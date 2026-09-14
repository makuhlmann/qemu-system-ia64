/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * IA-64 CPU QOM and execution-engine glue.
 *
 * Instruction decoding, family generators, and architectural helper logic
 * live in decode/, translate/, and arch/ respectively.
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "cpu.h"
#include "arch/arch.h"
#include "ia32/ia32.h"
#include "debug.h"
#include "translate/translate.h"
#include "exec/cputlb.h"
#include "exec/cpu-common.h"
#include "exec/page-protection.h"
#include "exec/target_page.h"
#include "exec/translation-block.h"
#include "hw/core/sysemu-cpu-ops.h"
#include "hw/core/boards.h"
#include "accel/tcg/cpu-ops.h"
#include "tcg/debug-assert.h"
#include "exec/translator.h"
#include "exec/helper-proto.h"
#include "system/memory.h"

#define HELPER_H "helper.h"
#include "exec/helper-info.c.inc"
#undef HELPER_H

static void ia64_cpu_set_pc(CPUState *cs, vaddr value)
{
    IA64CPU *cpu = ia64_cpu_from_cpu_state(cs);

    if (cpu->env.psr & IA64_PSR_IS) {
        cpu->env.ia32.eip =
            (uint32_t)(value - cpu->env.ia32.segs[R_CS].base);
    }
    cpu->env.ip = value;
}

static vaddr ia64_cpu_get_pc(CPUState *cs)
{
    IA64CPU *cpu = ia64_cpu_from_cpu_state(cs);

    return cpu->env.psr & IA64_PSR_IS ?
           ia64_ia32_virtual_ip(&cpu->env) : cpu->env.ip;
}


static TCGTBCPUState ia64_get_tb_cpu_state(CPUState *cs)
{
    IA64CPU *cpu = ia64_cpu_from_cpu_state(cs);
    uint64_t psr = cpu->env.psr;
    CPUX86State *xenv = &cpu->env.ia32;

    if (psr & IA64_PSR_IS) {
        uint32_t cs_base = xenv->segs[R_CS].base;
        uint32_t flags = xenv->hflags |
            (xenv->eflags &
             (IOPL_MASK | TF_MASK | RF_MASK | VM_MASK | AC_MASK)) |
            ((psr & IA64_PSR_DB) ? IA64_TB_FLAG_IA32_PSR_DB : 0) |
            ((psr & IA64_PSR_AC) ? IA64_TB_FLAG_IA32_PSR_AC : 0) |
            ((psr & IA64_PSR_SS) ? TF_MASK : 0);

        return (TCGTBCPUState) {
            .pc = (uint32_t)(cs_base + xenv->eip),
            .cs_base = cs_base,
            .flags = flags | IA64_TB_FLAG_PSR_IS,
        };
    }

    uint32_t flags =
        ((psr >> 17) & IA64_TB_FLAG_DT) |
        ((psr >> 35) & IA64_TB_FLAG_IT) |
        ((psr >> (IA64_PSR_RI_SHIFT - IA64_TB_FLAG_RI_SHIFT)) &
         IA64_TB_FLAG_RI_MASK) |
        ((psr >> 8) & IA64_TB_FLAG_PSR_IC) |
        /* PSR.be (bit 1) -> flag bit 6, PSR.ac (bit 3) -> flag bit 8: both <<5. */
        ((psr << 5) & (IA64_TB_FLAG_BE | IA64_TB_FLAG_PSR_AC)) |
        ((uint32_t)cpu->env.instruction_group_start << 7) |
        ((psr >> (IA64_PSR_CPL_SHIFT - IA64_TB_FLAG_CPL_SHIFT)) &
         IA64_TB_FLAG_CPL_MASK);

    flags |= (psr & IA64_PSR_FAULT_SUPPRESS_MASK) != 0 ?
             IA64_TB_FLAG_PSR_SUPPRESS : 0;

    return (TCGTBCPUState) {
        .pc = cpu->env.ip,
        .flags = flags,
    };
}

void ia64_tlb_bump_generation(CPUIA64State *env, bool is_ifetch)
{
    IA64MicroTlbEntry *micro = is_ifetch ? env->mmu.tlb_inst_micro :
                                           env->mmu.tlb_data_micro;
    uint32_t *generation = is_ifetch ? &env->mmu.tlb_inst_generation :
                                       &env->mmu.tlb_data_generation;

    (*generation)++;
    if (*generation == 0) {
        *generation = 1;
        memset(micro, 0, sizeof(*micro) * IA64_MICRO_TLB_SIZE);
    }
}

void ia64_tlb_bump_slot_generation(CPUIA64State *env, bool is_ifetch,
                                   uint16_t slot)
{
    IA64TlbEntry *tlb = is_ifetch ? env->mmu.tlb_inst :
                                    env->mmu.tlb_data;

    g_assert(slot < IA64_TLB_MAX);
    if (++tlb[slot].micro_generation == 0) {
        /*
         * A wrapped slot version could validate a very old hint.  Make the
         * wrap unambiguous by invalidating all hints before reusing version
         * one.  This path requires over four billion changes to one slot.
         */
        tlb[slot].micro_generation = 1;
        ia64_tlb_bump_generation(env, is_ifetch);
    }
}

const IA64TlbEntry *ia64_tlb_find_slow(CPUIA64State *env, uint64_t va,
                                       uint32_t rid, bool is_ifetch)
{
    IA64TlbEntry *tlb = is_ifetch ? env->mmu.tlb_inst : env->mmu.tlb_data;
    IA64MicroTlbEntry *micro = is_ifetch ? env->mmu.tlb_inst_micro :
                                           env->mmu.tlb_data_micro;
    uint16_t tlb_count = is_ifetch ? env->mmu.tlb_inst_count :
                                     env->mmu.tlb_data_count;
    uint32_t generation = is_ifetch ? env->mmu.tlb_inst_generation :
                                      env->mmu.tlb_data_generation;
    uint16_t i;

    for (i = 0; i < tlb_count; i++) {
        IA64TlbEntry *entry = &tlb[i];

        if (ia64_tlb_match(entry, va, rid)) {
            micro[ia64_micro_tlb_index(va, rid)] = (IA64MicroTlbEntry) {
                .va = entry->va,
                .page_mask = entry->page_mask,
                .rid = entry->rid,
                .generation = generation,
                .slot_generation = entry->micro_generation,
                .slot = i,
                .valid = true,
            };
            return entry;
        }
    }
    return NULL;
}

static void ia64_cpu_synchronize_from_tb(CPUState *cs,
                                         const TranslationBlock *tb)
{
    IA64CPU *cpu = ia64_cpu_from_cpu_state(cs);

    if (tb->flags & IA64_TB_FLAG_PSR_IS) {
        tcg_debug_assert(!tcg_cflags_has(cs, CF_PCREL));
        cpu->env.ia32.eip = (uint32_t)(tb->pc - tb->cs_base);
        cpu->env.ip = (uint32_t)tb->pc;
        return;
    }

    uint64_t ri =
        (tb->flags & IA64_TB_FLAG_RI_MASK) >> IA64_TB_FLAG_RI_SHIFT;

    tcg_debug_assert(!tcg_cflags_has(cs, CF_PCREL));
    cpu->env.ip = tb->pc;
    /*
     * Translation-time instruction fetch faults occur before generated TCG
     * can update PSR.ri.  Restore the slot encoded in the TB key along with
     * its bundle address; otherwise a stale slot from the preceding TB is
     * saved in IPSR and rfi can skip the faulting bundle's prologue.
     */
    cpu->env.psr = (cpu->env.psr & ~IA64_PSR_RI_MASK) |
                   (ri << IA64_PSR_RI_SHIFT);
}

static void ia64_restore_state_to_opc(CPUState *cs,
                                       const TranslationBlock *tb,
                                       const uint64_t *data)
{
    IA64CPU *cpu = ia64_cpu_from_cpu_state(cs);

    if (tb->flags & IA64_TB_FLAG_PSR_IS) {
        CPUX86State *xenv = &cpu->env.ia32;
        uint64_t new_pc;

        if (tb_cflags(tb) & CF_PCREL) {
            uint64_t pc = xenv->eip + tb->cs_base;

            new_pc = (pc & TARGET_PAGE_MASK) | data[0];
        } else {
            new_pc = data[0];
        }
        xenv->eip = (uint32_t)(new_pc - tb->cs_base);
        cpu->env.ip = (uint32_t)new_pc;
        if (data[1] != CC_OP_DYNAMIC) {
            xenv->cc_op = data[1];
        }
        return;
    }

    cpu->env.ip = data[0];
}

static int ia64_cpu_mmu_index(CPUState *cs, bool ifetch)
{
    IA64CPU *cpu = ia64_cpu_from_cpu_state(cs);

    if (cpu->env.psr & (ifetch ? IA64_PSR_IT : IA64_PSR_DT)) {
        return MMU_IDX_VIRT_CPL(ia64_psr_cpl(cpu->env.psr));
    }
    return MMU_PHYS_IDX;
}

static vaddr ia64_pointer_wrap(CPUState *cs, int mmu_idx,
                               vaddr result, vaddr base)
{
    IA64CPU *cpu = ia64_cpu_from_cpu_state(cs);

    return cpu->env.psr & IA64_PSR_IS ? (uint32_t)result : result;
}


static int ia64_tlb_perm_to_prot(uint8_t perm)
{
    int prot = 0;

    if (perm & IA64_TLB_R) {
        prot |= PAGE_READ;
    }
    if (perm & IA64_TLB_W) {
        prot |= PAGE_WRITE;
    }
    if (perm & IA64_TLB_X) {
        prot |= PAGE_EXEC;
    }
    return prot;
}

static int ia64_tlb_prot_for_pte_psr(uint64_t pte, uint8_t perm,
                                     bool is_ifetch, uint64_t psr)
{
    int prot = ia64_tlb_perm_to_prot(perm);

    /* IA-64 has independent instruction and data translation caches. */
    prot &= is_ifetch ? PAGE_EXEC : (PAGE_READ | PAGE_WRITE);

    /*
     * QEMU's software TLB may satisfy later accesses without re-entering
     * tlb_fill.  Do not cache write permission for a clean IA-64 PTE: a
     * later store must take Data Dirty so the OS can update the PTE or break
     * copy-on-write sharing.
     */
    if (!is_ifetch && !(psr & IA64_PSR_DA)) {
        if (!(pte & IA64_PTE_ACCESSED)) {
            prot &= ~(PAGE_READ | PAGE_WRITE);
        }
        if (!(pte & IA64_PTE_DIRTY)) {
            prot &= ~PAGE_WRITE;
        }
    } else if (is_ifetch && !(psr & IA64_PSR_IA) &&
               !(pte & IA64_PTE_ACCESSED)) {
        prot &= ~PAGE_EXEC;
    }

    return prot;
}

static int ia64_tlb_prot_for_pte(CPUIA64State *env, uint64_t pte,
                                 uint8_t perm, bool is_ifetch)
{
    return ia64_tlb_prot_for_pte_psr(pte, perm, is_ifetch, env->psr);
}

static void ia64_record_suppressed_tlb_fill(CPUIA64State *env, vaddr addr,
                                             int mmu_idx)
{
    uint64_t page = addr & TARGET_PAGE_MASK;
    uint16_t idxmap = 1u << mmu_idx;
    uint8_t i;

    for (i = 0; i < env->exception_state.suppressed_tlb_count; i++) {
        if (env->exception_state.suppressed_tlb_pages[i] == page) {
            env->exception_state.suppressed_tlb_idxmaps[i] |= idxmap;
            return;
        }
    }

    if (env->exception_state.suppressed_tlb_count == IA64_SUPPRESSED_TLB_MAX) {
        env->exception_state.suppressed_tlb_overflow = true;
        return;
    }

    i = env->exception_state.suppressed_tlb_count++;
    env->exception_state.suppressed_tlb_pages[i] = page;
    env->exception_state.suppressed_tlb_idxmaps[i] = idxmap;
}

static void ia64_record_suppressed_tlb_fill_if_needed(
    CPUIA64State *env, vaddr addr, int mmu_idx, uint64_t pte, uint8_t perm,
    bool is_ifetch, int prot)
{
    uint64_t unsuppressed_psr = env->psr & ~(IA64_PSR_DA | IA64_PSR_IA);
    int unsuppressed_prot;

    if (!(env->psr & (IA64_PSR_DA | IA64_PSR_IA))) {
        return;
    }

    unsuppressed_prot = ia64_tlb_prot_for_pte_psr(
        pte, perm, is_ifetch, unsuppressed_psr);
    if (prot != unsuppressed_prot) {
        ia64_record_suppressed_tlb_fill(env, addr, mmu_idx);
    }
}

void ia64_flush_suppressed_tlb(CPUIA64State *env)
{
    CPUState *cs = env_cpu(env);
    uint8_t i;

    if (env->exception_state.suppressed_tlb_overflow) {
        tlb_flush(cs);
    } else {
        for (i = 0; i < env->exception_state.suppressed_tlb_count; i++) {
            tlb_flush_page_by_mmuidx(
                cs, env->exception_state.suppressed_tlb_pages[i],
                env->exception_state.suppressed_tlb_idxmaps[i]);
        }
    }

    env->exception_state.suppressed_tlb_count = 0;
    env->exception_state.suppressed_tlb_overflow = false;
}

static void ia64_tlb_set_entry_page(CPUState *cs, vaddr addr, hwaddr pa,
                                    uint64_t page_size, int prot, int mmu_idx,
                                    IA64MemorySpeculation speculation,
                                    uint8_t memory_attribute)
{
    CPUTLBEntryFull full = {
        .phys_addr = pa & TARGET_PAGE_MASK,
        .attrs = MEMTXATTRS_UNSPECIFIED,
        .prot = prot,
        .lg_page_size = TARGET_PAGE_BITS,
    };

    (void)page_size;
    full.extra.ia64.speculation = speculation;
    full.extra.ia64.memory_attribute = memory_attribute;
    tlb_set_page_full(cs, mmu_idx, addr & TARGET_PAGE_MASK, &full);
}

static hwaddr ia64_cpu_get_phys_page_debug(CPUState *cs, vaddr addr)
{
    IA64CPU *cpu = ia64_cpu_from_cpu_state(cs);
    uint64_t pa;

    if (!ia64_mmu_translate_debug(&cpu->env, addr, &pa)) {
        return -1;
    }
    return pa & TARGET_PAGE_MASK;
}

static bool ia64_cpu_tlb_fill(CPUState *cs, vaddr addr, int size,
                              MMUAccessType access_type, int mmu_idx,
                              bool probe, uintptr_t retaddr)
{
    IA64CPU *cpu = ia64_cpu_from_cpu_state(cs);
    bool is_ifetch = (access_type == MMU_INST_FETCH);
    uint8_t needed = is_ifetch ? IA64_TLB_X :
                     (access_type == MMU_DATA_STORE ? IA64_TLB_W :
                      IA64_TLB_R);
    uint64_t pa;
    uint8_t perm;
    uint32_t rid;
    IA64Exception excp;
    bool is_rse = !is_ifetch && mmu_idx == MMU_IDX_RSE;
    uint8_t access_level;
    bool virt_translation_enabled;

    if (!probe && is_ifetch && (cpu->env.psr & IA64_PSR_IS) &&
        (uint32_t)addr == ia64_ia32_virtual_ip(&cpu->env)) {
        /*
         * The first executable-page lookup happens before x86 decoding.
         * Preserve the architectural ordering of IA-32 instruction
         * breakpoint and code-fetch faults ahead of instruction TLB faults.
         */
        ia64_ia32_check_fetch_fault_priority(&cpu->env, addr, 0);
    }

    rid = ia64_region_rid(&cpu->env, addr);
    if (mmu_idx == MMU_PHYS_IDX) {
        if (!ia64_pa_is_implemented(&cpu->env, addr)) {
            if (probe) {
                return false;
            }
            excp = is_ifetch ? IA64_EXCP_UNIMPL_INST_ADDR :
                   IA64_EXCP_UNIMPL_DATA_ADDR;
            if (is_ifetch) {
                cpu->env.ip = ia64_pa_canonicalize(&cpu->env, addr);
            }
            goto raise_exception;
        }
        pa = ia64_physical_address(addr);
        ia64_tlb_set_entry_page(
            cs, addr, pa, TARGET_PAGE_SIZE,
            PAGE_READ | PAGE_WRITE | PAGE_EXEC, mmu_idx,
            (addr & IA64_PHYS_UC_BIT) ? IA64_MEM_NON_SPECULATIVE :
                                       IA64_MEM_LIMITED_SPECULATION,
            (addr & IA64_PHYS_UC_BIT) ? 4 : 0);
        return true;
    }

    if (is_rse) {
        access_level = ia64_rsc_pl(cpu->env.ar_rsc);
    } else {
        g_assert(mmu_idx >= MMU_IDX_VIRT_CPL0 &&
                 mmu_idx <= MMU_IDX_VIRT_CPL3);
        access_level = mmu_idx - MMU_IDX_VIRT_CPL0;
    }

    /* A translated MMU index is itself the serialized translation state. */
    virt_translation_enabled = true;
    if (virt_translation_enabled && !ia64_va_is_implemented(&cpu->env, addr)) {
        if (probe) {
            return false;
        }
        excp = is_ifetch ? IA64_EXCP_UNIMPL_INST_ADDR :
               IA64_EXCP_UNIMPL_DATA_ADDR;
        if (is_ifetch) {
            cpu->env.ip = ia64_va_canonicalize(&cpu->env, addr);
        }
        goto raise_exception;
    }

    if (ia64_firmware_identity_pa(cpu->env.fw_image_base, cpu->env.cr_iva,
                                  is_ifetch ? addr : cpu->env.ip,
                                  cpu->env.psr, addr, &pa)) {
        int prot = is_ifetch ? PAGE_EXEC : (PAGE_READ | PAGE_WRITE);

        ia64_tlb_set_entry_page(cs, addr, pa, TARGET_PAGE_SIZE, prot,
                                mmu_idx, IA64_MEM_SPECULATIVE, 0);
        return true;
    }

    if (ia64_sal_boot_virtual_pa(&cpu->env, addr, &pa)) {
        int prot = is_ifetch ? PAGE_EXEC : (PAGE_READ | PAGE_WRITE);

        qemu_log_mask(CPU_LOG_MMU,
                      "ia64 firmware identity %c va=0x%016" PRIx64
                      " pa=0x%016" PRIx64 " psr=0x%016" PRIx64 "\n",
                      is_ifetch ? 'i' :
                      (access_type == MMU_DATA_STORE ? 'w' : 'd'),
                      (uint64_t)addr, pa, cpu->env.psr);
        ia64_tlb_set_entry_page(cs, addr, pa, TARGET_PAGE_SIZE, prot,
                                mmu_idx, IA64_MEM_SPECULATIVE, 0);
        return true;
    }

    {
        const IA64TlbEntry *entry = ia64_tlb_find_cached(
            &cpu->env, addr, rid, is_ifetch);

        if (entry) {
            int prot;
            IA64Exception pte_excp;

            ia64_tlb_entry_translate(entry, addr, access_level, &pa, &perm);
            pte_excp = ia64_tlb_exception_for_access(
                &cpu->env, entry, perm, needed, is_ifetch,
                access_type == MMU_DATA_STORE, is_rse);
            if (pte_excp != IA64_EXCP_NONE) {
                if (probe) {
                    return false;
                }
                excp = pte_excp;
                goto raise_exception;
            }
            prot = ia64_tlb_prot_for_pte(&cpu->env, entry->pte, perm,
                                         is_ifetch);
            ia64_record_suppressed_tlb_fill_if_needed(
                &cpu->env, addr, mmu_idx, entry->pte, perm, is_ifetch, prot);
            qemu_log_mask(CPU_LOG_MMU,
                          "ia64 tlb hit %c va=0x%016" PRIx64
                          " rid=0x%06" PRIx32 " pa=0x%016" PRIx64
                          " perm=0x%x\n",
                          is_ifetch ? 'i' : 'd', (uint64_t)addr, rid, pa,
                          perm);
            ia64_tlb_set_entry_page(
                cs, addr, pa, entry->ps, prot, mmu_idx,
                ia64_pte_memory_speculation(entry->pte),
                (entry->pte >> 2) & 7);
            return true;
        }
    }

    if (!is_ifetch) {
        const IA64TlbEntry *new_entry;
        uint64_t pte = 0;
        uint32_t key = 0;

        if (ia64_vhpt_walk_full(&cpu->env, addr, rid, false, is_rse,
                                access_level, &pa, &perm, &pte, &key,
                                &new_entry)) {
            int prot;
            IA64Exception pte_excp;
            uint64_t page_size = new_entry ? new_entry->ps : TARGET_PAGE_SIZE;

            pte_excp = new_entry ?
                ia64_tlb_exception_for_access(
                    &cpu->env, new_entry, perm, needed, false,
                    access_type == MMU_DATA_STORE, is_rse) :
                ia64_translation_exception_for_access(
                    &cpu->env, pte, key, perm, needed, false,
                    access_type == MMU_DATA_STORE, is_rse);
            if (pte_excp != IA64_EXCP_NONE) {
                if (probe) {
                    return false;
                }
                excp = pte_excp;
                goto raise_exception;
            }
            prot = ia64_tlb_prot_for_pte(&cpu->env,
                                         new_entry ? new_entry->pte : pte,
                                         perm, false);
            ia64_record_suppressed_tlb_fill_if_needed(
                &cpu->env, addr, mmu_idx,
                new_entry ? new_entry->pte : pte, perm, false, prot);
            qemu_log_mask(CPU_LOG_MMU,
                          "ia64 vhpt hit d va=0x%016" PRIx64
                          " rid=0x%06" PRIx32 " pa=0x%016" PRIx64
                          " perm=0x%x iha=0x%016" PRIx64 "\n",
                          (uint64_t)addr, rid, pa, perm,
                          ia64_vhpt_hash_address(&cpu->env, addr));
            ia64_tlb_set_entry_page(
                cs, addr, pa, page_size, prot, mmu_idx,
                ia64_pte_memory_speculation(new_entry ? new_entry->pte :
                                                        pte),
                ((new_entry ? new_entry->pte : pte) >> 2) & 7);
            return true;
        }
    }

    if (is_ifetch) {
        const IA64TlbEntry *new_entry;
        uint64_t pte = 0;
        uint32_t key = 0;

        if (ia64_vhpt_walk_full(&cpu->env, addr, rid, true, false,
                                access_level, &pa, &perm, &pte, &key,
                                &new_entry)) {
            int prot;
            IA64Exception pte_excp;
            uint64_t page_size = new_entry ? new_entry->ps : TARGET_PAGE_SIZE;

            pte_excp = new_entry ?
                ia64_tlb_exception_for_access(
                    &cpu->env, new_entry, perm, needed, true, false, false) :
                ia64_translation_exception_for_access(
                    &cpu->env, pte, key, perm, needed, true, false, false);
            if (pte_excp != IA64_EXCP_NONE) {
                if (probe) {
                    return false;
                }
                excp = pte_excp;
                goto raise_exception;
            }
            prot = ia64_tlb_prot_for_pte(&cpu->env,
                                         new_entry ? new_entry->pte : pte,
                                         perm, true);
            ia64_record_suppressed_tlb_fill_if_needed(
                &cpu->env, addr, mmu_idx,
                new_entry ? new_entry->pte : pte, perm, true, prot);
            qemu_log_mask(CPU_LOG_MMU,
                          "ia64 vhpt hit i va=0x%016" PRIx64
                          " rid=0x%06" PRIx32 " pa=0x%016" PRIx64
                          " perm=0x%x iha=0x%016" PRIx64 "\n",
                          (uint64_t)addr, rid, pa, perm,
                          ia64_vhpt_hash_address(&cpu->env, addr));
            ia64_tlb_set_entry_page(
                cs, addr, pa, page_size, prot, mmu_idx,
                ia64_pte_memory_speculation(new_entry ? new_entry->pte :
                                                        pte),
                ((new_entry ? new_entry->pte : pte) >> 2) & 7);
            return true;
        }
    }
    if (ia64_sal_boot_identity_pa_type(&cpu->env, addr, &pa, is_ifetch)) {
        int prot = is_ifetch ? PAGE_EXEC : (PAGE_READ | PAGE_WRITE);

        qemu_log_mask(CPU_LOG_MMU,
                      "ia64 sal boot identity %c va=0x%016" PRIx64
                      " pa=0x%016" PRIx64 " psr=0x%016" PRIx64 "\n",
                      is_ifetch ? 'i' :
                      (access_type == MMU_DATA_STORE ? 'w' : 'd'),
                      (uint64_t)addr, pa, cpu->env.psr);
        ia64_tlb_set_entry_page(cs, addr, pa, TARGET_PAGE_SIZE, prot,
                                mmu_idx, IA64_MEM_SPECULATIVE, 0);
        return true;
    }
    if (probe) {
        return false;
    }

    {
        uint64_t vhpt_entry_va;
        uint8_t vhpt_size;
        bool vhpt_long_format;
        bool vhpt_enabled = ia64_vhpt_walker_enabled(&cpu->env, addr,
                                                     is_ifetch, is_rse,
                                                     &vhpt_size,
                                                     &vhpt_long_format);

        if (!is_ifetch && ia64_data_nested_tlb_active(&cpu->env)) {
            excp = IA64_EXCP_DATA_NESTED_TLB;
        } else if (vhpt_enabled &&
                   ia64_vhpt_pte_not_present(&cpu->env, addr, is_ifetch,
                                             is_rse, &vhpt_entry_va)) {
            excp = IA64_EXCP_PAGE_NOT_PRESENT;
        } else if (!ia64_vhpt_entry_accessible(&cpu->env, addr, is_ifetch,
                                               is_rse, &vhpt_entry_va)) {
            excp = IA64_EXCP_VHPT_FAULT;
        } else if (vhpt_enabled) {
            excp = is_ifetch ? IA64_EXCP_ITLB_FAULT : IA64_EXCP_DTLB_FAULT;
        } else {
            excp = is_ifetch ? IA64_EXCP_ALT_ITLB : IA64_EXCP_ALT_DTLB;
        }
    }
raise_exception:
    if ((cpu->env.psr & IA64_PSR_IS) && retaddr) {
        cpu_restore_state(cs, retaddr);
        retaddr = 0;
    }
    if (cpu->env.psr & IA64_PSR_IS) {
        cpu->env.ip = ia64_ia32_virtual_ip(&cpu->env);
        cpu->env.exception_state.fault_ip = cpu->env.ip;
    }
    if (is_ifetch && excp == IA64_EXCP_PAGE_NOT_PRESENT &&
        (cpu->env.psr & IA64_PSR_IC) &&
        !(cpu->env.psr & IA64_PSR_IS)) {
        /*
         * IIP receives IP on interruption entry, and for faults it must point
         * at the faulting instruction bundle when interruption collection is
         * enabled.  Instruction fetch page-not-present faults may be raised
         * while looking up the next TB, before env->ip has otherwise advanced
         * to the fetched bundle.
         */
        cpu->env.ip = ia64_ip_bundle_addr(addr);
    }
    /*
     * IPSR.ri must name the slot execution resumes at.  PSR.ri holds
     * the current slot for data references and, for instruction
     * fetches, the slot the fetch will resume at (0 after a branch;
     * the interrupted slot when refetching after an rfi).  Without
     * this, an instruction-fetch fault would reuse a stale fault_slot
     * and the handler's rfi would skip slots of the target bundle.
     */
    cpu->env.exception_state.fault_slot =
        cpu->env.psr & IA64_PSR_IS ? 0 :
        (cpu->env.psr & IA64_PSR_RI_MASK) >> IA64_PSR_RI_SHIFT;
    if (cpu->env.psr & IA64_PSR_IC) {
        cpu->env.cr_ifa = is_ifetch && (cpu->env.psr & IA64_PSR_IS) ?
                          addr & ~0xfULL : addr;
        if (ia64_exception_initializes_iha(excp)) {
            cpu->env.cr_iha = ia64_vhpt_hash_address(&cpu->env, addr);
        }
        cpu->env.cr_itir = ia64_region_itir(
            &cpu->env, excp == IA64_EXCP_VHPT_FAULT ? cpu->env.cr_iha : addr);
    }
    if (excp != IA64_EXCP_DATA_NESTED_TLB) {
        if (excp == IA64_EXCP_UNIMPL_DATA_ADDR) {
            cpu->env.cr_isr = IA64_GENEX_UNIMPL_DATA_ADDR |
                              (access_type == MMU_DATA_STORE ?
                               IA64_ISR_W : IA64_ISR_R);
        } else if (excp == IA64_EXCP_UNIMPL_INST_ADDR) {
            cpu->env.cr_isr = IA64_GENEX_UNIMPL_INST_ADDR | IA64_ISR_X;
        } else {
            cpu->env.cr_isr = is_ifetch ? IA64_ISR_X :
                              (access_type == MMU_DATA_STORE ? IA64_ISR_W :
                               IA64_ISR_R);
            if (excp == IA64_EXCP_NAT_CONSUMPTION) {
                /*
                 * NaT Page Consumption reports ISR.code{5:4} = 2; the
                 * non-access code in ISR.code{3:0} is zero for an access.
                 */
                cpu->env.cr_isr |= IA64_ISR_CODE_NAT_PAGE;
            }
        }
        if (is_rse) {
            cpu->env.cr_isr |= IA64_ISR_RS;
            if (cpu->env.rse.rse_dirty < 0 || cpu->env.rse.rse_dirty_nat < 0) {
                /* Mandatory load for an incomplete frame (SDM 6.8). */
                cpu->env.cr_isr |= IA64_ISR_IR;
            }
        } else if (!is_ifetch && excp != IA64_EXCP_NAT_CONSUMPTION &&
                   ia64_current_code_tlb_ed(&cpu->env)) {
            /* NaT Page Consumption always reports ISR.ed as 0. */
            cpu->env.cr_isr |= IA64_ISR_ED;
        }
    }
    qemu_log_mask(CPU_LOG_MMU,
                  "ia64 tlb miss %c va=0x%016" PRIx64
                  " rid=0x%06" PRIx32 " ps=0x%016" PRIx64
                  " iha=0x%016" PRIx64 " pta=0x%016" PRIx64
                  " isr=0x%016" PRIx64 "\n",
                  is_ifetch ? 'i' :
                  (access_type == MMU_DATA_STORE ? 'w' : 'r'),
                  (uint64_t)addr, rid, cpu->env.cr_itir,
                  cpu->env.cr_iha, cpu->env.cr_pta, cpu->env.cr_isr);
    cs->exception_index = excp;
    if (cpu->env.psr & IA64_PSR_IS) {
        cpu_loop_exit(cs);
    }
    cpu_loop_exit_restore(cs, retaddr);
}


void ia64_cpu_set_boot_info(IA64CPU *cpu, const IA64BootInfo *info)
{
    cpu->boot_info = *info;
    cpu->boot_info_valid = true;
    cpu->boot_info_pending = true;
    CPU(cpu)->start_powered_off = info->powered_off;
}

void ia64_cpu_reset_to_boot_info(IA64CPU *cpu)
{
    g_assert(cpu->boot_info_valid);
    cpu->boot_info_pending = true;
    cpu_reset(CPU(cpu));
}

static void ia64_cpu_apply_boot_info(IA64CPU *cpu)
{
    CPUIA64State *env = &cpu->env;
    const IA64BootInfo *info = &cpu->boot_info;

    if (!cpu->boot_info_valid || !cpu->boot_info_pending) {
        return;
    }
    cpu->boot_info_pending = false;

    if (info->raw_entry) {
        /*
         * Architected PALE_RESET exit state (SDM Vol.2 rev 1.1 sec 11.2.2)
         * for a healthy normal cold boot, synthesized so real SAL firmware
         * can be entered at SALE_ENTRY without running real PAL_A/PAL_B
         * (plans/phase5-real-firmware-boot.md sec 3).  reset_hold has just
         * zeroed the whole env; only the non-zero pieces are set here.
         *
         * PSR.bn = 1 selects bank 1 for GR16-31.  Both banks are zero at
         * this point, so setting the bit without a bank swap is consistent,
         * and the bank-1 GR20 state parameter (function RESET) is 0 anyway.
         */
        env->psr = IA64_PSR_BN;
        env->ip = info->firmware_entry;
        /*
         * IVA points at the capture IVT (SDM 11.2.2 has PAL provide an IVT
         * before SALE_ENTRY; we synthesize one).  Zero keeps the historical
         * no-IVT behavior for callers that leave it unset.
         */
        env->cr_iva = info->iva;
        /* All 96 stacked registers accessible: CFM.sof = 96, rest 0. */
        env->cfm_sof = IA64_STACKED_GR_COUNT;
        env->rse.rse_invalid = 0;
        /*
         * PTA is architecturally undefined here, but PTA.size below 15 is a
         * reserved encoding even with ve=0 (same reasoning as the synthetic
         * path below) — encode the minimal valid dont-care.
         */
        env->cr_pta = 15ULL << 2;
        env->gr[IA64_SALE_GR_PROC_ID] = info->raw_proc_id;
        env->gr[IA64_SALE_GR_PAL_PROC] = info->raw_pal_proc;
        env->gr[IA64_SALE_GR_PAL_RETURN] = info->raw_pal_auth;
        /* Keep the physical stacked file coherent with the virtual view
         * (rse_bol = 0, no rotation: GR32+n maps to rse_pgr[n]). */
        env->rse.rse_pgr[IA64_SALE_GR_PROC_ID - IA64_SALE_GR_FROM_PAL] =
            info->raw_proc_id;
        env->rse.rse_pgr[IA64_SALE_GR_PAL_PROC - IA64_SALE_GR_FROM_PAL] =
            info->raw_pal_proc;
        env->rse.rse_pgr[IA64_SALE_GR_PAL_RETURN - IA64_SALE_GR_FROM_PAL] =
            info->raw_pal_auth;
        /*
         * Recognize the machine-planted PAL stub as a PAL procedure entry so
         * its break instruction dispatches into the PAL emulation
         * (ia64_is_pal_proc_break checks pal_proc_copy_addr).
         */
        if (info->raw_pal_proc != 0) {
            env->pal.pal_proc_copy_valid = true;
            env->pal.pal_proc_copy_addr = info->raw_pal_proc;
        }
        env->interrupt.pal_halt_wake = info->powered_off;
        env->ar_fpsr = IA64_FPSR_DEFAULT;
        set_float_rounding_mode(float_round_nearest_even, &env->fp.fp_status);
        set_flush_to_zero(false, &env->fp.fp_status);
        set_flush_inputs_to_zero(false, &env->fp.fp_status);
        set_default_nan_mode(false, &env->fp.fp_status);
        return;
    }

    env->psr = 0;
    env->ip = info->firmware_entry;
    env->br[IA64_BR_RETURN_LINK] = info->firmware_entry;
    env->cr_iva = info->iva;
    /*
     * VHPT disabled (ve=0), size field at its architectural minimum.
     * The minimum VHPT size is 32 KB, i.e. PTA.size{7:2} below 15 is a
     * reserved encoding (SDM Vol.2 rev 1.1 Table 3-6), so even a
     * dont-care reset value must encode 15.  The firmware rewrites PTA
     * to SAL_PTA_DISABLED_VALUE (the same encoding) before any OS code
     * runs.
     */
    env->cr_pta = 15ULL << 2;
    env->cr_dcr = IA64_DCR_DM | IA64_DCR_DP;
    env->ar_kr0 = info->firmware_base;
    env->ar_kr7 = 0;
    env->ar_rsc = info->rsc;
    env->ar_bsp = info->bsp;
    env->ar_bspstore = info->bsp;
    env->ar_rnat = 0;
    env->gr[IA64_GR_STACK_POINTER] = info->stack_pointer;
    env->gr[IA64_GR_GLOBAL_POINTER] = info->global_pointer;
    env->interrupt.pal_halt_wake = info->powered_off;
    env->ar_fpsr = IA64_FPSR_DEFAULT;
    set_float_rounding_mode(float_round_nearest_even, &env->fp.fp_status);
    set_flush_to_zero(false, &env->fp.fp_status);
    set_flush_inputs_to_zero(false, &env->fp.fp_status);
    set_default_nan_mode(false, &env->fp.fp_status);
}

static void ia64_cpu_reset_hold(Object *obj, ResetType type)
{
    IA64CPUClass *icc = IA64_CPU_GET_CLASS(obj);
    IA64CPU *cpu = IA64_CPU(obj);

    if (icc->parent_phases.hold) {
        icc->parent_phases.hold(obj, type);
    }

    if (cpu->itm_timer != NULL) {
        timer_del(cpu->itm_timer);
    }
    memset(&cpu->env, 0, sizeof(cpu->env));
    /*
     * Page sizes this model implements, cached out of the class so the MMU
     * and VHPT paths do not reach for it on every check.  Must be restored
     * after the memset above, on every reset.
     */
    cpu->env.insertable_page_mask = icc->insertable_page_mask;
    cpu->env.purgeable_page_mask = icc->purgeable_page_mask;
    cpu->env.itc_hz = icc->pal->freq_base_hz * icc->pal->itc_ratio_num /
                      icc->pal->itc_ratio_den;
    cpu->env.impl_pa_bits = icc->impl_pa_bits;
    cpu->env.impl_va_msb = icc->impl_va_msb;
    cpu->env.impl_rid_bits = icc->impl_rid_bits;
    cpu->env.impl_key_bits = icc->impl_key_bits;
    /*
     * The LINT0/LINT1 pins start masked: the architecture leaves LRR0-1
     * undefined out of reset and firmware programs them before it relies on
     * a pin (SDM vol 2 5.8.3.9), and nothing must reach the SAPIC through a
     * pin nobody has steered yet.
     */
    cpu->env.cr[IA64_CR_LRR0] = 1ULL << 16;
    cpu->env.cr[IA64_CR_LRR1] = 1ULL << 16;
    /*
     * Bound of the persistent region-7 KSEG physical alias (see
     * ia64_sal_boot_identity_pa_type()): the kernel reaches KSEG0 structures
     * through region-7 VA = PA + IA64_FW_REGION7_DIRECTMAP_BASE.  Clamp the
     * window to the fixed KSEG0 span (IA64_FW_REGION7_DIRECTMAP_SIZE) and to
     * backed RAM, whichever is smaller: a window that grows with RAM would
     * shadow kernel system space and corrupt large-memory guests.
     */
    cpu->env.mmu.region7_directmap_limit = IA64_FW_REGION7_DIRECTMAP_BASE +
        MIN(current_machine ? current_machine->ram_size : 0,
            IA64_FW_REGION7_DIRECTMAP_SIZE);
    /*
     * Where the firmware image executes.  The machine may relocate it (the
     * phase-2.2 RAM-top shadow); zero means the historical 1 MB link home.
     */
    cpu->env.fw_image_base = cpu->fw_image_base ? cpu->fw_image_base
                                                : IA64_FW_IDENTITY_BASE;
    cpu->env.alat_state.alat_full = cpu->alat_full;
    cpu->env.fp.fr[IA64_FR_ONE_INDEX] = IA64_FR_ONE;
    cpu->env.pr[IA64_PR_TRUE] = 1;
    cpu->env.psr = 0;
    cpu->env.ar_rsc = 0;
    /* Empty frame: every stacked physical register is invalid. */
    cpu->env.rse.rse_invalid = IA64_STACKED_GR_COUNT;
    cpu->env.ar_fpsr = IA64_FPSR_DEFAULT;
    cpu->env.cr_iva = 0;
    cpu->env.instruction_group_start = true;
    ia64_itc_write(&cpu->env, 0);
    set_float_2nan_prop_rule(float_2nan_prop_ab, &cpu->env.fp.fp_status);
    set_float_3nan_prop_rule(float_3nan_prop_abc, &cpu->env.fp.fp_status);
    set_float_infzeronan_rule(float_infzeronan_dnan_never,
                              &cpu->env.fp.fp_status);
    set_float_default_nan_pattern(0b01000000, &cpu->env.fp.fp_status);
    cpu->env.cr[IA64_CR_SAPIC_LID] =
        ia64_sapic_lid(MAX(CPU(cpu)->cpu_index, 0), 0);
    cpu->env.cr[IA64_CR_SAPIC_TPR] = 0;
    cpu->env.cr[IA64_CR_ITV] = IA64_VECTOR_MASKED;
    cpu->env.pal.pal_proc_copy_valid = false;
    cpu->env.pal.pal_proc_copy_addr = 0;
    cpu->env.pal.pal_interrupt_block_addr = IA64_LOCAL_SAPIC_PA;
    cpu->env.pal.pal_io_block_addr = IA64_PAL_IO_BLOCK_PA;
    ia64_cpu_apply_boot_info(cpu);
}

static ObjectClass *ia64_cpu_class_by_name(const char *cpu_model)
{
    ObjectClass *oc = object_class_by_name(cpu_model);
    char *typename;

    if (oc != NULL && object_class_dynamic_cast(oc, TYPE_IA64_CPU) != NULL) {
        return oc;
    }

    typename = g_strdup_printf(IA64_CPU_TYPE_NAME("%s"), cpu_model);
    oc = object_class_by_name(typename);
    g_free(typename);
    return oc;
}

static void ia64_cpu_realize(DeviceState *dev, Error **errp)
{
    CPUState *cs = CPU(dev);
    IA64CPU *cpu = IA64_CPU(dev);
    IA64CPUClass *icc = IA64_CPU_GET_CLASS(dev);
    Error *local_err = NULL;

    cpu_exec_realizefn(cs, &local_err);
    if (local_err != NULL) {
        error_propagate(errp, local_err);
        return;
    }

    cpu->itm_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, ia64_itm_timer_cb, cpu);

    qemu_init_vcpu(cs);
    cpu_reset(cs);

    icc->parent_realize(dev, errp);
}

static const struct SysemuCPUOps ia64_sysemu_ops = {
    .has_work = ia64_cpu_has_work,
    .get_phys_page_debug = ia64_cpu_get_phys_page_debug,
};

static bool ia64_precise_smc_enabled(CPUState *cs)
{
    IA64CPU *cpu = ia64_cpu_from_cpu_state(cs);

    /* IA-32 stores, unlike IA-64 stores, participate in hardware SMC. */
    return cpu->env.psr & IA64_PSR_IS;
}

static const TCGCPUOps ia64_tcg_ops = {
    /*
     * IA-64 is weakly ordered; ia64_translate_code() sets the effective
     * per-TB order (0 for native IA-64 code, x86-TSO for the IA-32 engine).
     * This default applies before the first translated instruction of a TB.
     */
    .guest_default_memory_order = 0,
    .mttcg_supported = true,
    .precise_smc = true,
    .precise_smc_enabled = ia64_precise_smc_enabled,
    .initialize = ia64_translate_init,
    .translate_code = ia64_translate_code,
    .get_tb_cpu_state = ia64_get_tb_cpu_state,
    .synchronize_from_tb = ia64_cpu_synchronize_from_tb,
    .restore_state_to_opc = ia64_restore_state_to_opc,
    .mmu_index = ia64_cpu_mmu_index,
    .tlb_fill = ia64_cpu_tlb_fill,
    .pointer_wrap = ia64_pointer_wrap,
#ifndef CONFIG_USER_ONLY
    .do_unaligned_access = ia64_cpu_do_unaligned_access,
#endif
    .cpu_exec_interrupt = ia64_cpu_exec_interrupt,
    .cpu_exec_halt = ia64_cpu_has_work,
    .cpu_exec_reset = cpu_reset,
    .do_interrupt = ia64_cpu_do_interrupt,
};

/*
 * Per-model PAL profiles (see IA64PalProfile in cpu.h).  Madison/Montecito
 * carry the exact values the PAL dispatcher used before profiles existed, so
 * their PAL responses stay bit-identical; only the merced profile differs.
 * All models report a 100 MHz PAL_FREQ_BASE and encode core/bus/ITC as ratios.
 */
/*
 * Memory attributes reported by PAL_MEM_ATTRIB: bit n is set for the memory
 * attribute with encoding n (SDM Vol. 2 figure 11-34, encodings in sec 4.4).
 * Both supported generations implement write-back, uncacheable, uncacheable
 * exported and write-coalescing: 251110-003 sec 12.1 states it for Itanium 2
 * ("supports WB, UC, and WC ... The UCE memory attribute is also supported"),
 * and Merced's write-coalescing buffer has a chapter of its own in
 * 245320-002 ch. 4.  This emulation implements all four identically on every
 * model, so every model reports all four.
 */
#define IA64_PAL_MEM_ATTRIB_WB_UC_UCE_WC \
    ((1ULL << IA64_PTE_MA_WB) | (1ULL << IA64_PTE_MA_UC) | \
     (1ULL << IA64_PTE_MA_UCE) | (1ULL << IA64_PTE_MA_WC))

/*
 * Itanium 2 (Madison) translation caches, 251110-003 sec 6.1.1 and 6.1.2:
 * a 32-entry fully associative L1 ITLB and L1 DTLB that "directly support
 * only a 4KB-page size", over a 128-entry fully associative L2 ITLB and
 * L2 DTLB, each of which may hold up to 64 translation registers and holds
 * every architected page size.  Four unique TCs across two levels.
 */
#define IA64_PAL_TC_ITANIUM2_L1 \
    { .num_entries = 32, .num_ways = 32, .num_sets = 1, \
      .page_mask = 1ULL << 12 }
#define IA64_PAL_TC_ITANIUM2_L2 \
    { .num_entries = 128, .num_ways = 128, .num_sets = 1, \
      .preferred_page_size_optimized = true, .reduced_by_trs = true, \
      .page_mask = IA64_INSERTABLE_PAGE_SIZE_MASK }

static const IA64PalProfile ia64_pal_profile_madison = {
    .freq_base_hz = 100000000ULL,
    .proc_ratio_num = 16, .proc_ratio_den = 1,   /* 1.6 GHz */
    .bus_ratio_num = 4,   .bus_ratio_den = 1,     /* 400 MHz */
    .itc_ratio_num = 16,  .itc_ratio_den = 1,     /* ITC at the core clock */
    .has_post_merced_pal = true,
    .pal_vendor = 1,
    .pal_a_model = 2, .pal_a_revision = 0x23,
    .pal_b_model = 2, .pal_b_revision = 0x23,
    .memory_attributes = IA64_PAL_MEM_ATTRIB_WB_UC_UCE_WC,
    .cache_levels = 3,
    .unique_caches = 4,
    .cache = {
        [0] = {
            [0] = { .size = 16 * KiB, .associativity = 4, .line_shift = 6,
                    .stride_shift = 6, .store_latency = 0xff,
                    .load_latency = 1, .tag_lsb = 12 },
            [1] = { .size = 16 * KiB, .associativity = 4, .line_shift = 6,
                    .stride_shift = 6, .store_latency = 1,
                    .load_latency = 1, .tag_lsb = 12 },
        },
        /* Unified L2: reported on the data/unified type only. */
        [1] = {
            [1] = { .size = 256 * KiB, .associativity = 8, .line_shift = 7,
                    .stride_shift = 7, .attribute = 1, .store_latency = 1,
                    .load_latency = 5, .tag_lsb = 15, .unified = true },
        },
        [2] = {
            [1] = { .size = 3 * MiB, .associativity = 12, .line_shift = 7,
                    .stride_shift = 7, .attribute = 1, .store_latency = 1,
                    .load_latency = 12, .tag_lsb = 18, .unified = true },
        },
    },
    .tc_levels = 2,
    .unique_tcs = 4,
    .tc = {
        [0] = { IA64_PAL_TC_ITANIUM2_L1, IA64_PAL_TC_ITANIUM2_L1 },
        [1] = { IA64_PAL_TC_ITANIUM2_L2, IA64_PAL_TC_ITANIUM2_L2 },
    },
};

static const IA64PalProfile ia64_pal_profile_montecito = {
    .freq_base_hz = 100000000ULL,
    .proc_ratio_num = 16, .proc_ratio_den = 1,    /* 1.6 GHz */
    .bus_ratio_num = 16,  .bus_ratio_den = 3,      /* 533.33 MHz */
    .itc_ratio_num = 16,  .itc_ratio_den = 1,      /* ITC at the core clock */
    .has_post_merced_pal = true,
    .pal_vendor = 1,
    .pal_a_model = 2, .pal_a_revision = 0x23,
    .pal_b_model = 2, .pal_b_revision = 0x23,
    .memory_attributes = IA64_PAL_MEM_ATTRIB_WB_UC_UCE_WC,
    .cache_levels = 3,
    .unique_caches = 5,
    .cache = {
        [0] = {
            [0] = { .size = 16 * KiB, .associativity = 4, .line_shift = 6,
                    .stride_shift = 6, .store_latency = 0xff,
                    .load_latency = 1, .tag_lsb = 12 },
            [1] = { .size = 16 * KiB, .associativity = 4, .line_shift = 6,
                    .stride_shift = 6, .store_latency = 1,
                    .load_latency = 1, .tag_lsb = 12 },
        },
        /* Montecito splits L2 into separate instruction and data caches. */
        [1] = {
            [0] = { .size = 1 * MiB, .associativity = 8, .line_shift = 7,
                    .stride_shift = 7, .store_latency = 0xff,
                    .load_latency = 7, .tag_lsb = 17 },
            [1] = { .size = 256 * KiB, .associativity = 8, .line_shift = 7,
                    .stride_shift = 7, .attribute = 1, .store_latency = 1,
                    .load_latency = 5, .tag_lsb = 15 },
        },
        [2] = {
            [1] = { .size = 12 * MiB, .associativity = 12,
                    .line_shift = 7, .stride_shift = 7, .attribute = 1,
                    .store_latency = 1, .load_latency = 14, .tag_lsb = 20,
                    .unified = true },
        },
    },
    .tc_levels = 2,
    .unique_tcs = 4,
    .tc = {
        [0] = { IA64_PAL_TC_ITANIUM2_L1, IA64_PAL_TC_ITANIUM2_L1 },
        [1] = { IA64_PAL_TC_ITANIUM2_L2, IA64_PAL_TC_ITANIUM2_L2 },
    },
};

/*
 * Original Itanium (Merced), 800 MHz / 133 MHz bus / 4 MB L3 SKU (249634-002
 * datasheet; CPUID table 249720-009).  brl is not implemented
 * (cpuid_features = 0) and the post-Merced PAL procedures are absent
 * (245318-001/-002 §11.8).  See plans/merced-model-notes.md for full
 * citations.
 *
 * Cache geometry, 245473-002 sec 4.1-4.4 and 248701-002 sec 2.5.4:
 *   L1I  16 KB, 4-way, 32 B lines
 *   L1D  16 KB, 4-way, 32 B lines, write-through, no write-allocate,
 *        2-cycle integer load latency
 *   L2   96 KB, 6-way, 64 B lines, write-back, write-allocate,
 *        6-cycle integer load latency
 *   L3   4 MB,  4-way, 64 B lines, 21-cycle integer load latency
 * tag_lsb is the first tag bit above the index and offset: 128 sets of 32 B
 * for the 16 KB caches (12), 256 sets of 64 B for L2 (14), 16384 sets of 64 B
 * for L3 (20).
 *
 * Translation caches, 248701-002 sec 2.5.6: a single-level 64-entry fully
 * associative ITLB holding the instruction TRs, and a two-level data TLB --
 * a 32-entry DTLB1 that is not architecturally visible over a 96-entry DTLB2
 * holding the data TRs.  Both data levels hold every architected page size
 * (245473-002 sec 4.7).  That is three unique TCs across two levels, and
 * there is no second instruction level.
 */
static const IA64PalProfile ia64_pal_profile_merced = {
    .freq_base_hz = 100000000ULL,
    .proc_ratio_num = 8,  .proc_ratio_den = 1,     /* 800 MHz */
    .bus_ratio_num = 4,   .bus_ratio_den = 3,       /* 133.33 MHz */
    .itc_ratio_num = 8,   .itc_ratio_den = 1,       /* ITC at the core clock
                                                     * (245473-002: the ITC counts
                                                     * processor clocks) */
    .has_post_merced_pal = false,
    /*
     * PAL 8.8.30, the C2 stepping's firmware version (249720-009 revision
     * history and the errata stepping/PAL-version matrix, which lists 6.6.21,
     * 6.6.23, 6.6.24, 6.6.25, 6.6.26, 7.7.27, 7.7.28 and 8.8.30).  The split
     * into PAL_A and PAL_B version fields is vendor-defined (SDM Vol. 2
     * figure 11-37), but the third component is pinned by a guest: XP's EFI
     * loader reads the byte at PAL_A_version{7:0} and refuses to boot below
     * 0x23 (WXPSP1 NT/base/boot/efi/ia64/miscc.c:38-50 for the field layout,
     * :356 and :407 for the minimum).  0x23 is itself one of the published
     * versions, so that byte is the monotonic third component read as hex,
     * and the leading pair tracks the stepping (C0 6.6, C1 7.7, C2 8.8).
     */
    .pal_vendor = 1,
    .pal_a_model = 8, .pal_a_revision = 0x30,
    .pal_b_model = 8, .pal_b_revision = 0x30,
    .memory_attributes = IA64_PAL_MEM_ATTRIB_WB_UC_UCE_WC,
    .cache_levels = 3,
    .unique_caches = 4,
    .cache = {
        [0] = {
            [0] = { .size = 16 * KiB, .associativity = 4, .line_shift = 5,
                    .stride_shift = 5, .store_latency = 0xff,
                    .load_latency = 1, .tag_lsb = 12 },
            [1] = { .size = 16 * KiB, .associativity = 4, .line_shift = 5,
                    .stride_shift = 5, .store_latency = 1,
                    .load_latency = 2, .tag_lsb = 12 },
        },
        [1] = {
            [1] = { .size = 96 * KiB, .associativity = 6, .line_shift = 6,
                    .stride_shift = 6, .attribute = 1, .store_latency = 1,
                    .load_latency = 6, .tag_lsb = 14, .unified = true },
        },
        [2] = {
            [1] = { .size = 4 * MiB, .associativity = 4, .line_shift = 6,
                    .stride_shift = 6, .attribute = 1, .store_latency = 1,
                    .load_latency = 21, .tag_lsb = 20, .unified = true },
        },
    },
    .tc_levels = 2,
    .unique_tcs = 3,
    .tc = {
        [0] = {
            /* ITLB: single level, holds the instruction TRs. */
            [0] = { .num_entries = 64, .num_ways = 64, .num_sets = 1,
                    .reduced_by_trs = true,
                    .page_mask = IA64_MERCED_INSERTABLE_PAGE_SIZE_MASK },
            /* DTLB1: a cache of DTLB2, holds no TRs. */
            [1] = { .num_entries = 32, .num_ways = 32, .num_sets = 1,
                    .page_mask = IA64_MERCED_INSERTABLE_PAGE_SIZE_MASK },
        },
        [1] = {
            /* No second instruction TC level. */
            [1] = { .num_entries = 96, .num_ways = 96, .num_sets = 1,
                    .preferred_page_size_optimized = true,
                    .reduced_by_trs = true,
                    .page_mask = IA64_MERCED_INSERTABLE_PAGE_SIZE_MASK },
        },
    },
};

static void ia64_cpu_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    CPUClass *cc = CPU_CLASS(oc);
    IA64CPUClass *icc = IA64_CPU_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    device_class_set_parent_realize(dc, ia64_cpu_realize,
                                    &icc->parent_realize);
    resettable_class_set_parent_phases(rc, NULL, ia64_cpu_reset_hold, NULL,
                                       &icc->parent_phases);

    dc->vmsd = &vmstate_ia64_cpu;
    cc->class_by_name = ia64_cpu_class_by_name;
    cc->dump_state = ia64_cpu_dump_state;
    cc->set_pc = ia64_cpu_set_pc;
    cc->get_pc = ia64_cpu_get_pc;
    cc->sysemu_ops = &ia64_sysemu_ops;
    cc->gdb_read_register = ia64_cpu_gdb_read_register;
    cc->gdb_write_register = ia64_cpu_gdb_write_register;
    cc->gdb_num_core_regs = IA64_GDB_NUM_CORE_REGS;
    cc->tcg_ops = &ia64_tcg_ops;

    /*
     * Keep direct instantiation of the base type aligned with the legacy model.
     */
    icc->cpuid_version = 0x000000001f010504ULL;
    icc->cpuid_features = IA64_CPUID4_LB;
    icc->ia32_cpuid_version = 0x00000673;
    icc->itr_count = 64;
    icc->dtr_count = 64;
    icc->insertable_page_mask = IA64_INSERTABLE_PAGE_SIZE_MASK;
    icc->purgeable_page_mask = IA64_PURGEABLE_PAGE_SIZE_MASK;
    icc->vhpt_hash_folds_hpn = true;
    icc->impl_pa_bits = IA64_IMPL_PA_BITS;
    icc->impl_va_msb = IA64_IMPL_VA_MSB;
    icc->impl_rid_bits = IA64_IMPL_RID_BITS;
    icc->impl_key_bits = IA64_IMPL_KEY_BITS;
    icc->has_native_ia32 = true;
    icc->has_virtualization = false;
    icc->is_montecito = false;
    icc->pal = &ia64_pal_profile_madison;
}

typedef struct IA64CPUModelDef {
    uint64_t cpuid_version;
    uint64_t cpuid_features;
    uint32_t ia32_cpuid_version;
    uint8_t itr_count;
    uint8_t dtr_count;
    uint64_t insertable_page_mask;
    uint64_t purgeable_page_mask;
    uint8_t impl_pa_bits;
    uint8_t impl_va_msb;
    uint8_t impl_rid_bits;
    uint8_t impl_key_bits;
    bool vhpt_hash_folds_hpn;
    bool has_native_ia32;
    bool has_virtualization;
    bool is_montecito;
    const IA64PalProfile *pal;
} IA64CPUModelDef;

static void ia64_cpu_model_class_init(ObjectClass *oc, const void *data)
{
    IA64CPUClass *icc = IA64_CPU_CLASS(oc);
    const IA64CPUModelDef *model = data;

    icc->cpuid_version = model->cpuid_version;
    icc->cpuid_features = model->cpuid_features;
    icc->ia32_cpuid_version = model->ia32_cpuid_version;
    icc->itr_count = model->itr_count;
    icc->dtr_count = model->dtr_count;
    icc->insertable_page_mask = model->insertable_page_mask;
    icc->purgeable_page_mask = model->purgeable_page_mask;
    icc->vhpt_hash_folds_hpn = model->vhpt_hash_folds_hpn;
    icc->impl_pa_bits = model->impl_pa_bits;
    icc->impl_va_msb = model->impl_va_msb;
    icc->impl_rid_bits = model->impl_rid_bits;
    icc->impl_key_bits = model->impl_key_bits;
    icc->has_native_ia32 = model->has_native_ia32;
    icc->has_virtualization = model->has_virtualization;
    icc->is_montecito = model->is_montecito;
    icc->pal = model->pal;
}

/*
 * Translation-register file size is implementation-specific; the SDM only
 * guarantees eight of each bank.  Model both supported CPU generations
 * with 64 ITRs and 64 DTRs.
 */
static const IA64CPUModelDef ia64_cpu_model_madison = {
    /* Family 0x1f, model 1, revision 5, CPUID[4] is the last register. */
    .cpuid_version = 0x000000001f010504ULL,
    /*
     * brl only.  No 16-byte atomics and no virtualization (both post-date
     * Madison), and no spontaneous deferral either: no Itanium 2 implements
     * it (era /proc/cpuinfo dumps show only "branchlong"), and this model's
     * own ld.s deferral is DCR-gated -- the behaviour of an sd=0 processor.
     */
    .cpuid_features = IA64_CPUID4_LB,
    /* P6-class IA-32 engine identity: family 6, model 7, stepping 3. */
    .ia32_cpuid_version = 0x00000673,
    .itr_count = 64,
    .dtr_count = 64,
    .insertable_page_mask = IA64_INSERTABLE_PAGE_SIZE_MASK,
    .purgeable_page_mask = IA64_PURGEABLE_PAGE_SIZE_MASK,
    .impl_pa_bits = IA64_IMPL_PA_BITS,
    .impl_va_msb = IA64_IMPL_VA_MSB,
    .impl_rid_bits = IA64_IMPL_RID_BITS,
    .impl_key_bits = IA64_IMPL_KEY_BITS,
    .vhpt_hash_folds_hpn = true,
    .has_native_ia32 = true,
    .has_virtualization = false,
    .pal = &ia64_pal_profile_madison,
};

static const IA64CPUModelDef ia64_cpu_model_montecito = {
    /* Family 0x20, model 0, C2 revision 7, CPUID[4] is the last register. */
    .cpuid_version = 0x0000000020000704ULL,
    /* brl and 16-byte atomics; spontaneous deferral stays unimplemented. */
    .cpuid_features = IA64_CPUID4_LB | IA64_CPUID4_AO,
    .ia32_cpuid_version = 0x00000673,
    .itr_count = 64,
    .dtr_count = 64,
    .insertable_page_mask = IA64_INSERTABLE_PAGE_SIZE_MASK,
    .purgeable_page_mask = IA64_PURGEABLE_PAGE_SIZE_MASK,
    .impl_pa_bits = IA64_IMPL_PA_BITS,
    .impl_va_msb = IA64_IMPL_VA_MSB,
    .impl_rid_bits = IA64_IMPL_RID_BITS,
    .impl_key_bits = IA64_IMPL_KEY_BITS,
    .vhpt_hash_folds_hpn = true,
    /*
     * Montecito implements the virtualization extensions, but this model
     * does not virtualize.  vmsw is decoded and reported as a Virtualization
     * fault so a guest sees the architected interruption instead of a
     * silently succeeding privilege-mode switch.
     */
    .has_virtualization = true,
    .is_montecito = true,
    .pal = &ia64_pal_profile_montecito,
};

/*
 * Original Itanium ("Merced"), 800 MHz / 4 MB L3, C2 stepping.  Family 0x07,
 * model 0, revision 8, CPUID[4] is the last register (249720-009 spec update).
 * cpuid_features = 0: brl is not implemented (CPUID[4].lb = 0, 245319-002 brl
 * page), which is what Windows' KF_BRL check expects on Merced.  Asymmetric TR
 * file: 8 ITR / 48 DTR (248701-002 §2.5.6).  No 16-byte atomics, no
 * virtualization.  See plans/merced-model-notes.md.
 */
static const IA64CPUModelDef ia64_cpu_model_merced = {
    .cpuid_version = 0x0000000007000804ULL,
    .cpuid_features = 0,
    /*
     * x86 family 7 is the AP-485 assignment for the original Itanium's
     * IA-32 engine.  Model/stepping mirror the chosen native C2 identity
     * (model 0, revision 8); no public document pins them, so replace
     * with a hardware dump value if one ever surfaces.
     */
    .ia32_cpuid_version = 0x00000708,
    .itr_count = 8,
    .dtr_count = 48,
    .insertable_page_mask = IA64_MERCED_INSERTABLE_PAGE_SIZE_MASK,
    .purgeable_page_mask = IA64_MERCED_PURGEABLE_PAGE_SIZE_MASK,
    /*
     * Merced implements 44 physical address bits (245320-002 sec 3.2), but
     * this machine cannot yet be described inside a 44-bit physical space:
     * ia64-vpc places the PCI I/O port window at 0x8000_1000_0000 and the
     * PAL I/O block at 0x8000_0C00_0000, both of which set bit 47.
     * Narrowing impl_pa_bits makes the firmware's own UART and I/O accesses
     * take Unimplemented Data Address faults before the loader ever runs.
     * Relocating those windows is a machine-wide change -- hw/ia64, the
     * firmware and the ACPI _CRS all describe them -- so until that lands
     * the physical width stays at what this platform needs.  The virtual
     * width, region-ID width and key width do not depend on the platform
     * layout and are Merced's.
     */
    .impl_pa_bits = IA64_IMPL_PA_BITS,
    .impl_va_msb = IA64_MERCED_IMPL_VA_MSB,
    .impl_rid_bits = IA64_MERCED_IMPL_RID_BITS,
    .impl_key_bits = IA64_MERCED_IMPL_KEY_BITS,
    .vhpt_hash_folds_hpn = false,
    .has_native_ia32 = true,
    .has_virtualization = false,
    .is_montecito = false,
    .pal = &ia64_pal_profile_merced,
};

static const TypeInfo ia64_cpu_type_info[] = {
    {
        .name = TYPE_IA64_CPU,
        .parent = TYPE_CPU,
        .instance_size = sizeof(IA64CPU),
        .instance_align = __alignof__(IA64CPU),
        .class_size = sizeof(IA64CPUClass),
        .class_init = ia64_cpu_class_init,
    },
    {
        .name = IA64_CPU_TYPE_NAME("madison"),
        .parent = TYPE_IA64_CPU,
        .class_init = ia64_cpu_model_class_init,
        .class_data = &ia64_cpu_model_madison,
    },
    {
        .name = IA64_CPU_TYPE_NAME("montecito"),
        .parent = TYPE_IA64_CPU,
        .class_init = ia64_cpu_model_class_init,
        .class_data = &ia64_cpu_model_montecito,
    },
    {
        .name = IA64_CPU_TYPE_NAME("merced"),
        .parent = TYPE_IA64_CPU,
        .class_init = ia64_cpu_model_class_init,
        .class_data = &ia64_cpu_model_merced,
    },
    {
        /* "itanium" is an alias for the original Itanium (Merced). */
        .name = IA64_CPU_TYPE_NAME("itanium"),
        .parent = TYPE_IA64_CPU,
        .class_init = ia64_cpu_model_class_init,
        .class_data = &ia64_cpu_model_merced,
    },
};

DEFINE_TYPES(ia64_cpu_type_info)
