/*
 * IA-64 system-register and processor-state architecture operations.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/atomic.h"
#include "cpu.h"
#include "arch/arch.h"
#include "arch/system.h"
#include "exec/cpu-common.h"
#include "exec/cputlb.h"
#include "exec/tb-flush.h"
#include "exec/translation-block.h"
#include "exec/tlb-flags.h"

#define IA64_CPUID_VENDOR0           0x49656e69756e6547ULL /* "GenuineI" */
#define IA64_CPUID_VENDOR1           0x000000006c65746eULL /* "ntel" */
#define IA64_CPUID_SERIAL            0x0000000000000000ULL


static void ia64_swap_banked_gr(CPUIA64State *env);
uint64_t ia64_system_read_pr(CPUIA64State *env)
{
    uint64_t value = 0;

    for (uint32_t i = 0; i < IA64_PR_COUNT; i++) {
        value |= (env->pr[i] & 1) << i;
    }

    return value;
}


void ia64_system_epc(CPUIA64State *env, uint64_t fault_ip, uint64_t raw,
                uint32_t fault_slot)
{
    uint8_t current_cpl = ia64_psr_cpl(env->psr);
    uint8_t pfs_ppl = (env->ar_pfs & IA64_PFS_PPL_MASK) >> IA64_PFS_PPL_SHIFT;
    uint8_t new_cpl = current_cpl;

    if (pfs_ppl < current_cpl) {
        ia64_raise_exception(env, IA64_EXCP_ILLEGAL, fault_ip, raw,
                               fault_slot);
    }

    if (env->psr & IA64_PSR_IT) {
        uint32_t rid = ia64_region_rid(env, fault_ip);

        for (uint16_t i = 0; i < env->mmu.tlb_inst_count; i++) {
            IA64TlbEntry *entry = &env->mmu.tlb_inst[i];

            if (ia64_tlb_match(entry, fault_ip, rid) &&
                entry->ar == 7 && entry->pl < current_cpl) {
                new_cpl = entry->pl;
                break;
            }
        }
    } else {
        new_cpl = 0;
    }

    ia64_set_psr(env, (env->psr & ~IA64_PSR_CPL_MASK) |
                      ((uint64_t)new_cpl << IA64_PSR_CPL_SHIFT));
}














































void ia64_system_write_pr(CPUIA64State *env, uint64_t value, uint64_t mask)
{
    mask &= ~1ULL;
    if (ctpop64(mask) > IA64_PR_COUNT / 2) {
        for (uint32_t i = 1; i < IA64_PR_COUNT; i++) {
            if (mask & (1ULL << i)) {
                env->pr[i] = (value >> i) & 1;
            }
        }
        env->pr[IA64_PR_TRUE] = 1;
        return;
    }
    while (mask) {
        uint32_t i = ctz64(mask);

        mask &= mask - 1;
        env->pr[i] = (value >> i) & 1;
    }
    env->pr[IA64_PR_TRUE] = 1;
}

uint64_t ia64_system_read_ar(CPUIA64State *env, uint32_t ar_num)
{
    if (ar_num >= IA64_AR_COUNT) {
        return 0;
    }
    if (ar_num == 17) {
        return env->ar_bsp;
    }
    if (ar_num == 44) {
        return ia64_itc_read(env);
    }
    return env->ar[ar_num];
}

static bool ia64_reserved_rsc_field(uint64_t value)
{
    return value & ~(0x1fULL | (0x3fffULL << IA64_RSC_LOADRS_SHIFT));
}

static bool ia64_reserved_fpsr_field(uint64_t value)
{
    return (value >> 58) != 0 ||
           ((value >> 12) & 1) != 0 ||
           ((value >> 47) & 3) == 1 ||
           ((value >> 34) & 3) == 1 ||
           ((value >> 21) & 3) == 1 ||
           ((value >> 8) & 3) == 1;
}

static bool ia64_reserved_pfs_field(uint64_t value)
{
    uint32_t sof = value & 0x7f;
    uint32_t sol = (value >> 7) & 0x7f;
    uint32_t sor = ((value >> 14) & 0xf) << 3;
    uint32_t rrb_gr = (value >> 18) & 0x7f;
    uint32_t rrb_fr = (value >> 25) & 0x7f;
    uint32_t rrb_pr = (value >> 32) & 0x3f;

    if ((value & (0xfULL << 58)) ||
        (value & (0x3fffULL << 38))) {
        return true;
    }
    return sof > IA64_STACKED_GR_COUNT || sol > sof || sor > sof ||
           (sor ? rrb_gr >= sor : rrb_gr != 0) ||
           rrb_fr >= 96 || rrb_pr >= 48;
}

void ia64_system_validate_ar_access(CPUIA64State *env, uint64_t value,
                               uint32_t ar_num, uint32_t write,
                               uint64_t fault_ip, uint64_t raw,
                               uint32_t slot)
{
    if ((ar_num == 18 || ar_num == 19) &&
        (env->ar_rsc & IA64_RSC_MODE)) {
        env->cr_isr = 0;
        ia64_raise_exception(env, IA64_EXCP_ILLEGAL, fault_ip, raw, slot);
    }

    if (!write) {
        if (ar_num == 44 && (env->psr & IA64_PSR_SI) &&
            ia64_psr_cpl(env->psr) != 0) {
            env->cr_isr = 0x20;
            ia64_raise_exception(env, IA64_EXCP_PRIVILEGED_REG,
                                   fault_ip, raw, slot);
        }
        return;
    }

    if ((ar_num == 16 && ia64_reserved_rsc_field(value)) ||
        (ar_num == 40 && ia64_reserved_fpsr_field(value)) ||
        (ar_num == 64 && ia64_reserved_pfs_field(value))) {
        qemu_log_mask(CPU_LOG_INT,
                      "ia64 reserved AR field ip=%016" PRIx64
                      " ar=%u value=%016" PRIx64 " raw=%011" PRIx64
                      " slot=%u\n",
                      fault_ip, ar_num, value, raw, slot);
        env->cr_isr = 0x30;
        ia64_raise_exception(env, IA64_EXCP_RESERVED_REG_FIELD,
                               fault_ip, raw, slot);
    }
    if ((ar_num <= 7 || ar_num == 44) && ia64_psr_cpl(env->psr) != 0) {
        env->cr_isr = 0x20;
        ia64_raise_exception(env, IA64_EXCP_PRIVILEGED_REG,
                               fault_ip, raw, slot);
    }
}

void ia64_system_write_ar(CPUIA64State *env, uint32_t ar_num, uint64_t value)
{
    if (ar_num >= IA64_AR_COUNT) {
        return;
    }
    if ((ar_num >= 48 && ar_num <= 63) || ar_num >= 112) {
        return;
    }
    if (ar_num == 44) {
        bool match = env->cr[IA64_CR_ITM] == value;

        ia64_itc_write(env, value);
        env->interrupt.itm_last_match_valid = false;
        if (match) {
            env->interrupt.itm_armed = true;
            env->interrupt.itm_armed_value = value;
        }
        ia64_itm_update(env, env->cr[IA64_CR_ITM]);
        return;
    }
    if (ar_num == 16) {
        uint8_t pl = MAX(ia64_rsc_pl(value), ia64_psr_cpl(env->psr));
        uint64_t old_rsc = env->ar_rsc;

        env->ar_rsc = (value & ~IA64_RSC_PL) |
                      ((uint64_t)pl << IA64_RSC_PL_SHIFT);
        if ((old_rsc ^ env->ar_rsc) & IA64_RSC_PL) {
            /*
             * The RSC privilege level feeds the permission check for RSE
             * backing-store accesses, which are cached in the MMU_IDX_RSE
             * softmmu entries, so those must be discarded when it changes.
             * Windows writes ar.rsc=0 at every trap entry and restores the
             * user RSC on exit, so this runs on every user<->kernel
             * transition.  MMU_IDX_RSE is a data-only index (instruction
             * fetch never uses it), so the invalidation cannot stale a
             * translated block: keep the jump cache instead of wiping all
             * 4096 hints on each transition.
             */
            tlb_flush_by_mmuidx_no_jmp_cache(env_cpu(env), 1u << MMU_IDX_RSE);
        }
        return;
    }
    if (ar_num == 19) {
        value &= INT64_MAX;
        /*
         * A software AR.RNAT write supplies the collection bits for the
         * whole group BSPSTORE currently sits in (Windows restores the
         * value it saved after flushing this group).
         */
        env->rse.rse_rnat_low = env->ar_bspstore & ~0x1ffULL;
    } else if (ar_num == 18) {
        value &= ~7ULL;
        /*
         * AR.RNAT is undefined after a BSPSTORE write: nothing below the
         * new location is represented until software rewrites AR.RNAT or
         * stores extend the accumulation.
         */
        env->rse.rse_rnat_low = value;
    } else if (ar_num == 66) {
        value &= 0x3f;
    }
    env->ar[ar_num] = value;
    if (ar_num == 18) {
        /*
         * mov-to-BSPSTORE (SDM Vol.2 6.5.3): the clean partition
         * empties and the dirty partition is preserved by rebasing
         * AR.BSP to the new address plus the dirty registers and their
         * intervening NaT collections.  No memory traffic occurs.
         * RNAT becomes architecturally undefined; this implementation
         * keeps its previous contents.
         */
        int32_t dirty = MAX(env->rse.rse_dirty, 0);

        env->rse.rse_dirty_nat = ia64_rse_nat_words_grow(value, dirty);
        env->ar_bsp = value +
            (uint64_t)(env->rse.rse_dirty + env->rse.rse_dirty_nat) * 8;
        env->rse.rse_invalid += env->rse.rse_clean;
        env->rse.rse_clean = 0;
        env->rse.rse_clean_nat = 0;
        ia64_rse_check(env, "bspstore");
    }
}

uint64_t ia64_system_read_cr(CPUIA64State *env, uint32_t cr_num)
{
    if (cr_num >= IA64_CR_COUNT) {
        return 0;
    }
    switch (cr_num) {
    case IA64_CR_SAPIC_LID:
        return qatomic_read(&env->cr[cr_num]) &
               (IA64_SAPIC_LID_ID_MASK | IA64_SAPIC_LID_EID_MASK);
    case IA64_CR_SAPIC_IVR:
        return (uint64_t)ia64_sapic_get_ivr(env) & 0xFF;
    case IA64_CR_SAPIC_IRR0:
        return env->interrupt.sapic_irr[0];
    case IA64_CR_SAPIC_IRR1:
        return env->interrupt.sapic_irr[1];
    case IA64_CR_SAPIC_IRR2:
        return env->interrupt.sapic_irr[2];
    case IA64_CR_SAPIC_IRR3:
        return env->interrupt.sapic_irr[3];
    default:
        return env->cr[cr_num];
    }
}

static bool ia64_reserved_ipsr_field(uint64_t value)
{
    return (value >> 46) != 0 ||
           ((value >> 41) & 3) == 3 ||
           ((value >> 28) & 0xf) != 0 ||
           ((value >> 16) & 1) != 0 ||
           ((value >> 6) & 0x7f) != 0 ||
           (value & 1) != 0;
}

static bool ia64_reserved_ifs_field(uint64_t value)
{
    if (value & (0x1ffffffULL << 38)) {
        return true;
    }
    return (value >> 63) && ia64_reserved_pfs_field(value);
}

static bool ia64_reserved_cr_field(uint32_t cr_num, uint64_t value)
{
    switch (cr_num) {
    case 0:
        return (value >> 15) != 0 || (value & (0x1fULL << 3));
    case 8: {
        uint8_t ps = (value >> 2) & 0x3f;

        return (value & (0x3fULL << 9)) || (value & 2) ||
               ps < 15;
    }
    case 16:
        return ia64_reserved_ipsr_field(value);
    case 17:
        return (value >> 44) != 0 ||
               ((value >> 41) & 3) == 3 ||
               ((value >> 24) & 0xff) != 0;
    case 23:
        return ia64_reserved_ifs_field(value);
    case IA64_CR_SAPIC_LID:
        return (value & 0xffff) != 0;
    case IA64_CR_SAPIC_IVR:
        return (value & 0xff) != 0;
    case IA64_CR_SAPIC_TPR:
        return (value & 0xff00) != 0;
    case IA64_CR_ITV:
    case 73:
    case 74:
        return (value & (7ULL << 13)) ||
               (value & (0xfULL << 8));
    case 80:
    case 81: {
        uint8_t delivery_mode = (value >> 8) & 7;

        return (value & (1ULL << 14)) ||
               (value & (1ULL << 11)) ||
               delivery_mode == 1 || delivery_mode == 3 ||
               delivery_mode == 6;
    }
    default:
        return false;
    }
}

uint64_t ia64_system_validate_cr_access(CPUIA64State *env, uint64_t value,
                                   uint32_t cr_num, uint32_t write,
                                   uint64_t fault_ip, uint64_t raw,
                                   uint32_t slot)
{
    if ((env->psr & IA64_PSR_IC) && cr_num >= 16 && cr_num <= 25) {
        env->cr_isr = 0;
        ia64_raise_exception(env, IA64_EXCP_ILLEGAL, fault_ip, raw, slot);
    }
    if (write && ia64_reserved_cr_field(cr_num, value)) {
        qemu_log_mask(CPU_LOG_INT | LOG_GUEST_ERROR,
                      "ia64 reserved cr write cr%u value=%016" PRIx64
                      " ip=%016" PRIx64 " raw=%016" PRIx64
                      " slot=%u psr=%016" PRIx64 "\n",
                      cr_num, value, fault_ip, raw, slot, env->psr);
        env->cr_isr = 0x30;
        ia64_raise_exception(env, IA64_EXCP_RESERVED_REG_FIELD,
                               fault_ip, raw, slot);
    }

    if (write && cr_num == IA64_CR_IFA &&
        !ia64_va_is_implemented(env, value)) {
        /* Writing an unimplemented VA to CR.IFA raises Unimplemented Data
         * Address (a non-access reference). */
        env->cr_ifa = value;
        env->cr_isr = IA64_GENEX_UNIMPL_DATA_ADDR | IA64_ISR_NA |
                      (ia64_current_code_tlb_ed(env) ? IA64_ISR_ED : 0);
        ia64_raise_exception(env, IA64_EXCP_UNIMPL_DATA_ADDR,
                             fault_ip, raw, slot);
    }

    switch (cr_num) {
    case 2:
        return value & ~0x7fffULL;
    case 25:
        return value & ~3ULL;
    case IA64_CR_SAPIC_TPR:
        return value & 0x100f0;
    case IA64_CR_SAPIC_EOI:
        return 0;
    case IA64_CR_ITV:
    case 73:
    case 74:
    case 80:
    case 81:
        return value & 0x1efff;
    default:
        return value;
    }
}

uint64_t ia64_system_read_cpuid(CPUIA64State *env, uint64_t index)
{
    IA64CPUClass *icc = ia64_env_cpu_class(env);

    index &= 0xff;
    switch (index) {
    case 0:
        return IA64_CPUID_VENDOR0;
    case 1:
        return IA64_CPUID_VENDOR1;
    case 2:
        return IA64_CPUID_SERIAL;
    case 3:
        return icc->cpuid_version;
    case 4:
        return icc->cpuid_features;
    default:
        return 0;
    }
}

uint64_t ia64_system_read_dahr_indexed(CPUIA64State *env, uint64_t index)
{
    return env->dahr[index & 7] & 0x7ff;
}

uint64_t ia64_system_read_msr(CPUIA64State *env, uint64_t index)
{
    if (index < IA64_MSR_COUNT) {
        return env->msr[index];
    }
    return 0;
}

void ia64_system_write_msr(CPUIA64State *env, uint64_t index, uint64_t value)
{
    if (index < IA64_MSR_COUNT) {
        env->msr[index] = value;
    }
}

uint64_t ia64_system_read_dbr(CPUIA64State *env, uint32_t index)
{
    index &= 0xff;
    if (index >= IA64_DBR_COUNT) {
        return 0;
    }
    return env->dbr[index];
}

void ia64_system_write_dbr(CPUIA64State *env, uint32_t index, uint64_t value)
{
    index &= 0xff;
    if (index < IA64_DBR_COUNT) {
        if (index & 1) {
            value &= ~(3ULL << 60);
        }
        env->dbr[index] = value;
    }
}

uint64_t ia64_system_read_ibr(CPUIA64State *env, uint32_t index)
{
    index &= 0xff;
    if (index >= IA64_IBR_COUNT) {
        return 0;
    }
    return env->ibr[index];
}

void ia64_system_write_ibr(CPUIA64State *env, uint32_t index, uint64_t value)
{
    index &= 0xff;
    if (index < IA64_IBR_COUNT) {
        if (index & 1) {
            value &= ~(7ULL << 60);
        }
        env->ibr[index] = value;
    }
}

void ia64_write_cr(CPUIA64State *env, uint32_t cr_num, uint64_t value)
{
    if (cr_num >= IA64_CR_COUNT) {
        return;
    }
    switch (cr_num) {
    case 1:
        env->cr[IA64_CR_ITM] = value;
        ia64_itm_update(env, value);
        break;
    case 2:
        if (ia64_firmware_owns_iva(env->fw_image_base,
                                   env->cr[IA64_CR_IVA]) !=
            ia64_firmware_owns_iva(env->fw_image_base, value)) {
            /*
             * The firmware identity window is an emulator boot facility,
             * not an architectural translation.  Drop cached mappings when
             * IVA ownership passes between firmware and the operating
             * system.
             */
            tlb_flush(env_cpu(env));
            queue_tb_flush(env_cpu(env));
            ia64_tlb_bump_generation(env, false);
            ia64_tlb_bump_generation(env, true);
        }
        env->cr[IA64_CR_IVA] = value;
        break;
    case 8:
        if (env->cr[IA64_CR_PTA] == value) {
            break;
        }
        env->cr[IA64_CR_PTA] = value;
        ia64_tlb_bump_generation(env, false);
        ia64_tlb_bump_generation(env, true);
        tlb_flush(env_cpu(env));
        break;
    case IA64_CR_SAPIC_TPR:
        env->cr[cr_num] = value & IA64_TPR_WRITABLE_MASK;
        ia64_sapic_update_interrupt(env);
        break;
    case IA64_CR_SAPIC_LID:
        qatomic_set(&env->cr[cr_num],
                    value & (IA64_SAPIC_LID_ID_MASK | IA64_SAPIC_LID_EID_MASK));
        break;
    case IA64_CR_SAPIC_EOI:
        ia64_sapic_eoi(env);
        break;
    case IA64_CR_SAPIC_IVR:
        break;
    case IA64_CR_SAPIC_IRR0:
    case IA64_CR_SAPIC_IRR1:
    case IA64_CR_SAPIC_IRR2:
    case IA64_CR_SAPIC_IRR3:
        break;
    case IA64_CR_ITV:
        env->cr[cr_num] = value;
        ia64_itm_update(env, env->cr[IA64_CR_ITM]);
        break;
    case IA64_CR_LRR0:
    case IA64_CR_LRR1:
        env->cr[cr_num] = value;
        ia64_lint_lrr_written(env, cr_num - IA64_CR_LRR0);
        break;
    default:
        env->cr[cr_num] = value;
        break;
    }
}

uint64_t ia64_system_read_pmc(CPUIA64State *env, uint32_t index)
{
    if (index >= IA64_PMC_COUNT) {
        return 0;
    }
    return env->pmc[index];
}

void ia64_system_write_pmc(CPUIA64State *env, uint32_t index, uint64_t value)
{
    if (index >= IA64_PMC_COUNT) {
        return;
    }
    env->pmc[index] = value;
}

uint64_t ia64_system_read_pmc_indexed(CPUIA64State *env, uint64_t index)
{
    index &= 0xff;
    if (index >= IA64_PMC_COUNT) {
        return 0;
    }
    return env->pmc[index];
}

void ia64_system_write_pmc_indexed(CPUIA64State *env, uint64_t index,
                              uint64_t value)
{
    index &= 0xff;
    if (index >= IA64_PMC_COUNT) {
        return;
    }
    env->pmc[index] = value;
}

uint64_t ia64_system_read_pmd(CPUIA64State *env, uint32_t index)
{
    if (index >= IA64_PMD_COUNT) {
        return 0;
    }
    return env->pmd[index];
}

uint64_t ia64_system_read_pmd_checked(CPUIA64State *env, uint64_t index,
                                 uint64_t fault_ip, uint64_t raw,
                                 uint32_t slot)
{
    index &= 0xff;
    if (index >= IA64_PMD_COUNT) {
        env->cr_isr = 0x30;
        ia64_raise_exception(env, IA64_EXCP_RESERVED_REG_FIELD,
                               fault_ip, raw, slot);
    }
    if ((env->pmc[index] & (1ULL << 6)) &&
        ia64_psr_cpl(env->psr) != 0) {
        env->cr_isr = 0x20;
        ia64_raise_exception(env, IA64_EXCP_PRIVILEGED_REG,
                               fault_ip, raw, slot);
    }
    return (env->psr & IA64_PSR_SP) ? 0 : env->pmd[index];
}

void ia64_system_write_pmd(CPUIA64State *env, uint32_t index, uint64_t value)
{
    if (index >= IA64_PMD_COUNT) {
        return;
    }
    env->pmd[index] = value;
}

uint64_t ia64_system_read_pmd_indexed(CPUIA64State *env, uint64_t index)
{
    index &= 0xff;
    if (index >= IA64_PMD_COUNT) {
        return 0;
    }
    return env->pmd[index];
}

void ia64_system_write_pmd_indexed(CPUIA64State *env, uint64_t index,
                              uint64_t value)
{
    index &= 0xff;
    if (index >= IA64_PMD_COUNT) {
        return;
    }
    env->pmd[index] = value;
}




void ia64_system_st_spill_unat(CPUIA64State *env, uint32_t reg, uint64_t addr)
{
    uint32_t bit_pos = (addr >> 3) & 0x3f;

    if (ia64_gr_nat_get(env, reg)) {
        env->ar_unat |= 1ULL << bit_pos;
    } else {
        env->ar_unat &= ~(1ULL << bit_pos);
    }
}

static void ia64_swap_banked_gr(CPUIA64State *env)
{
    uint32_t i;

    for (i = 0; i < 16; i++) {
        uint32_t reg = 16 + i;
        uint64_t value = env->gr[reg];
        bool nat = ia64_gr_nat_get(env, reg);

        env->gr[reg] = env->banked_gr[i];
        ia64_gr_nat_set(env, reg, (env->banked_nat >> i) & 1);
        env->banked_gr[i] = value;
        if (nat) {
            env->banked_nat |= (uint16_t)(1U << i);
        } else {
            env->banked_nat &= (uint16_t)~(1U << i);
        }
    }
}

void ia64_set_psr(CPUIA64State *env, uint64_t value)
{
    if ((env->psr ^ value) & IA64_PSR_IC) {
        env->exception_state.psr_ic_inflight = true;
    }
    if ((env->psr ^ value) & IA64_PSR_BN) {
        ia64_swap_banked_gr(env);
    }
    env->psr = value;
}

void ia64_flush_on_pk_change(CPUIA64State *env, uint64_t old_psr)
{
    if ((old_psr ^ env->psr) & IA64_PSR_PK) {
        /*
         * Protection-key checks are reflected in cached QEMU TLB
         * permissions.  rfi, ssm and rsm may toggle PSR.pk without writing
         * the PKRs, so discard only translated entries on those rare
         * transitions.  DT/IT/RT and CPL are represented by the MMU index.
         */
        ia64_tlb_bump_generation(env, false);
        ia64_tlb_bump_generation(env, true);
        tlb_flush_by_mmuidx(env_cpu(env), MMU_IDX_TRANSLATED_MASK);
    }
}

void ia64_system_clear_psr_fault_suppression(CPUIA64State *env)
{
    uint64_t old_mask = env->exception_state.psr_suppression_before_insn &
                        IA64_PSR_FAULT_SUPPRESS_MASK;
    uint64_t clear_mask = env->psr & old_mask;

    if (clear_mask) {
        ia64_set_psr(env, env->psr & ~clear_mask);
    }
    if (old_mask & (IA64_PSR_DA | IA64_PSR_IA)) {
        ia64_flush_suppressed_tlb(env);
    }
    env->exception_state.psr_suppression_before_insn = 0;
}

void ia64_set_psr_bn(CPUIA64State *env, bool bank1)
{
    uint64_t value = bank1 ? (env->psr | IA64_PSR_BN) :
                             (env->psr & ~IA64_PSR_BN);

    ia64_set_psr(env, value);
}

void ia64_system_set_psr_bn(CPUIA64State *env, uint32_t bank1)
{
    ia64_set_psr_bn(env, bank1 != 0);
}




void ia64_system_ssm(CPUIA64State *env, uint64_t imm)
{
    uint64_t old_psr = env->psr;

    ia64_set_psr(env, env->psr | imm);
    ia64_flush_on_pk_change(env, old_psr);
}

void ia64_system_rsm(CPUIA64State *env, uint64_t imm)
{
    uint64_t old_psr = env->psr;

    ia64_set_psr(env, env->psr & ~imm);
    ia64_flush_on_pk_change(env, old_psr);
}
/* ---- Probe helper (optionally writes r1 and returns the probe result) ---- */
/* ---- tak / thash / ttag helpers ---- */


/* ---- mov from PSR helper ---- */

uint64_t ia64_system_mov_psrgr_read(CPUIA64State *env, uint32_t unused)
{
    (void)unused;
    /*
     * PSR.ri is only defined as an rfi restart selector and becomes
     * undefined after the restarted IA-64 instruction begins execution.
     * The translator keeps it live internally to select a nonzero TB entry
     * slot, so expose the chosen architectural undefined value of zero.
     */
    return env->psr & ~IA64_PSR_RI_MASK;
}

/* ---- mov to PSR helper ---- */

void ia64_system_mov_psr_write(CPUIA64State *env, uint64_t value,
                               uint32_t psr_l)
{
    uint64_t old_psr = env->psr;
    uint64_t new_psr;

    if (psr_l) {
        new_psr = (env->psr & ~0xffffffffULL) | (value & 0xffffffffULL);
    } else {
        new_psr = value;
    }
    if (env->psr == new_psr) {
        return;
    }
    ia64_set_psr(env, new_psr);
    /*
     * mov psr.l = r writes only PSR{31:0}.  The bits it can change that
     * matter to address translation are DT/IT (which select the MMU index,
     * recomputed per access), RT (consulted per RSE access) and PK.  Only PK
     * is reflected in the cached softmmu TLB permissions, so discarding the
     * whole softmmu TLB and jump cache on every write is wasteful -- Windows
     * executes mov psr.l on every user->kernel system call.  Flush only when
     * PK actually changes.  DA/IA live above bit 31 and cannot be reached by
     * the psr.l form; the full-PSR path (psr_l == 0) is not produced by the
     * decoder today, and would still be covered by the PK check for its
     * translation-relevant bits.
     */
    ia64_flush_on_pk_change(env, old_psr);
}

/* ---- mov from Region Register helper ---- */

uint64_t ia64_system_mov_rrgr_read(CPUIA64State *env, uint64_t rr_addr)
{
    uint32_t rr_num = (rr_addr >> 61) & 7;

    if (rr_num < 8) {
        return env->rr[rr_num];
    }
    return 0;
}

uint64_t ia64_system_validate_rr_value(CPUIA64State *env, uint64_t value,
                                  uint64_t fault_ip, uint64_t raw,
                                  uint32_t slot)
{
    uint8_t ps = (value >> 2) & 0x3f;
    uint64_t allowed = 1ULL | (0x3fULL << 2) |
                       (ia64_rid_mask(env) << 8);

    if ((value & ~allowed) || !ia64_page_shift_insertable(env, ps)) {
        env->cr_isr = 0x30;
        ia64_raise_exception(env, IA64_EXCP_RESERVED_REG_FIELD,
                               fault_ip, raw, slot);
    }
    return value;
}

/* ---- mov to Region Register helper ---- */

void ia64_system_mov_grrr_write(CPUIA64State *env, uint64_t rr_addr,
                                uint64_t value)
{
    uint32_t rr_num = (rr_addr >> 61) & 7;

    if (env->rr[rr_num] == value) {
        return;
    }

    env->rr[rr_num] = value;
    /*
     * The virtual softmmu indices and the jump cache are VA-keyed, so both
     * must be discarded when the RID changes: tlb_flush_by_mmuidx() of the
     * translated indices does both (its async worker flushes the jump cache).
     * Two things need not happen, though, and Windows/Linux run this on every
     * process/mm switch:
     *   - The PHYS index is physical-address-keyed and independent of the
     *     region registers, so it is left intact (MMU_IDX_TRANSLATED_MASK
     *     excludes it).
     *   - The fork's micro-TLB entries are RID-tagged and validated on lookup
     *     (cpu.h ia64_tlb_find_cached: cached->rid == rid), and the modeled TLB
     *     that backs them is not purged by a region-register write, so a stale
     *     entry simply never matches the new RID and a reused RID's entries are
     *     still architecturally valid.  Bumping the micro-TLB generation to
     *     wholesale-invalidate it is therefore unnecessary.
     * The global TB hash is keyed by physical page as well as virtual PC, so
     * its TBs remain valid and are reused when this address space is current.
     */
    tlb_flush_by_mmuidx(env_cpu(env), MMU_IDX_TRANSLATED_MASK);
}

/* ---- mov from PKR helper ---- */

uint64_t ia64_system_mov_pkrgr_read(CPUIA64State *env, uint32_t pkr_num)
{
    if (pkr_num < IA64_PKR_COUNT) {
        return env->pkr[pkr_num];
    }
    return 0;
}

uint64_t ia64_system_mov_pkrgr_indexed_read(CPUIA64State *env, uint64_t pkr_num)
{
    pkr_num &= 0xff;
    if (pkr_num < IA64_PKR_COUNT) {
        return env->pkr[pkr_num];
    }
    return 0;
}

/* ---- mov to PKR helper ---- */

static void ia64_pkr_write(CPUIA64State *env, uint32_t pkr_num,
                           uint64_t value)
{
    uint64_t masked = value & ia64_pkr_mask(env);
    uint64_t key = masked & ia64_pkr_key_mask(env);
    bool changed;

    if (pkr_num >= IA64_PKR_COUNT) {
        return;
    }

    changed = env->pkr[pkr_num] != masked;
    if (masked & IA64_PKR_VALID) {
        for (uint32_t i = 0; i < IA64_PKR_COUNT; i++) {
            if (i != pkr_num && (env->pkr[i] & IA64_PKR_VALID) &&
                (env->pkr[i] & ia64_pkr_key_mask(env)) == key) {
                env->pkr[i] &= ~IA64_PKR_VALID;
                changed = true;
            }
        }
    }
    env->pkr[pkr_num] = masked;
    if (changed) {
        ia64_tlb_bump_generation(env, false);
        ia64_tlb_bump_generation(env, true);
        tlb_flush(env_cpu(env));
    }
}

void ia64_system_mov_grpkr_write(CPUIA64State *env, uint32_t pkr_num,
                                 uint64_t value)
{
    ia64_pkr_write(env, pkr_num, value);
}

void ia64_system_mov_grpkr_indexed_write(CPUIA64State *env, uint64_t pkr_num,
                                    uint64_t value)
{
    pkr_num &= 0xff;
    ia64_pkr_write(env, pkr_num, value);
}
