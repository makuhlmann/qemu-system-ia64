/*
 * IA-64 MMU, translation cache, VHPT, purge, and probe architecture logic.
 *
 * IA64MMUState owns architected TR/TC entries and derived replacement and
 * purge bookkeeping.  TCG helper ABI adapters live in helper/helper-mmu.c.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "cpu.h"
#include "arch/arch.h"
#include "exec-access.h"
#include "exec/cpu-common.h"
#include "exec/cputlb.h"
#include "exec/tb-flush.h"
#include "exec/translation-block.h"
#include "exec/target_page.h"
#include "exec/tlb-flags.h"
#include "trace.h"

static bool ia64_code_tlb_ed(CPUIA64State *env);

#define IA64_PTE_PL_SHIFT 7
#define IA64_PTE_PL_MASK  (3ULL << IA64_PTE_PL_SHIFT)
#define IA64_PTE_AR_SHIFT 9
#define IA64_PTE_AR_MASK  (7ULL << IA64_PTE_AR_SHIFT)
#define IA64_PTE_RESERVED_MASK ((1ULL << 1) | (3ULL << 50))
#define IA64_ITIR_RESERVED_MASK (3ULL | (0xffffffffULL << 32))
#define IA64_L0_CACHE_LINE_SIZE 64ULL

static uint8_t ia64_pte_ar(uint64_t pte)
{
    return (pte & IA64_PTE_AR_MASK) >> IA64_PTE_AR_SHIFT;
}

static uint8_t ia64_pte_pl(uint64_t pte)
{
    return (pte & IA64_PTE_PL_MASK) >> IA64_PTE_PL_SHIFT;
}

static uint8_t ia64_pte_perm(uint64_t pte, uint8_t access_level)
{
    if (!(pte & IA64_PTE_PRESENT)) {
        return 0;
    }

    return ia64_tlb_effective_perm(ia64_pte_ar(pte), ia64_pte_pl(pte),
                                   access_level);
}

static uint64_t ia64_page_size_from_shift(uint64_t ps_bits)
{
    if (ps_bits < 12) {
        ps_bits = 12;
    }
    return 1ULL << ps_bits;
}

static uint64_t ia64_itir_page_size(CPUIA64State *env)
{
    uint64_t ps_bits = (env->cr_itir >> IA64_ITIR_PS_SHIFT) & IA64_ITIR_PS_MASK;

    return ia64_page_size_from_shift(ps_bits);
}

static uint64_t ia64_gr_page_size(uint64_t value)
{
    return ia64_page_size_from_shift((value >> IA64_ITIR_PS_SHIFT) &
                                     IA64_ITIR_PS_MASK);
}

static bool ia64_translation_insert_fields_valid(const CPUIA64State *env,
                                                 uint64_t pte, uint64_t itir)
{
    uint8_t page_shift = (itir >> IA64_ITIR_PS_SHIFT) &
                         IA64_ITIR_PS_MASK;
    uint64_t itir_reserved = IA64_ITIR_RESERVED_MASK;
    uint8_t ma;

    if (!ia64_page_shift_insertable(env, page_shift)) {
        return false;
    }
    if (!(pte & IA64_PTE_PRESENT)) {
        itir_reserved &= 3;
        return !(itir & itir_reserved);
    }

    ma = (pte & IA64_PTE_MA_MASK) >> IA64_PTE_MA_SHIFT;
    return !(pte & IA64_PTE_RESERVED_MASK) &&
           !(itir & itir_reserved) && (ma == 0 || ma >= 4);
}

static bool ia64_tlb_entry_overlaps(const IA64TlbEntry *entry,
                                    uint64_t start, uint64_t ps,
                                    uint32_t rid)
{
    uint64_t mask;

    if (entry->rid != rid || !entry->valid || entry->ps == 0) {
        return false;
    }

    /*
     * Both ranges are power-of-two sized and naturally aligned.  They
     * overlap exactly when their bases agree after masking by the larger
     * page size.  Reuse the entry's precomputed mask in the common case.
     */
    mask = entry->ps >= ps ? entry->page_mask : ia64_va_vpn_mask(ps);
    return ((entry->va ^ start) & mask) == 0;
}

static int ia64_tlb_circular_tc_victim(const IA64TlbEntry *tlb,
                                       uint16_t capacity,
                                       uint16_t next_replace)
{
    uint16_t i = next_replace;
    uint16_t n;

    for (n = 0; n < capacity; n++) {
        if (tlb[i].valid && !tlb[i].is_tr) {
            return i;
        }
        if (++i == capacity) {
            i = 0;
        }
    }
    return -1;
}

/*
 * Largest IA-64 page for which ia64_qemu_tlb_flush_entry() walks the softmmu
 * TLB one 4 KiB page at a time instead of issuing a range flush.  Covers the
 * 4/8/16/64/256 KiB pages the guest OSes actually map their working sets with
 * (Windows 8 KiB, Linux 16 KiB); 1 MiB+ TR granules fall through to the range
 * flush.  256 KiB is 64 single-page flushes worst case -- far cheaper than the
 * full-index flush and refill storm the range path degrades to.
 */
#define IA64_TLB_PAGEWISE_FLUSH_MAX (64ULL * TARGET_PAGE_SIZE)

static void ia64_qemu_tlb_flush_entry(CPUIA64State *env,
                                      const IA64TlbEntry *entry,
                                      bool is_data)
{
    uint64_t base;
    uint8_t region;

    if (!entry->valid || entry->ps < TARGET_PAGE_SIZE) {
        return;
    }

    base = ia64_va_page_base(entry->va, entry->ps);
    for (region = 0; region <= IA64_REGION_MASK; region++) {
        if (ia64_rr_rid(env->rr[region]) != entry->rid) {
            continue;
        }

        uint64_t va = ((uint64_t)region << IA64_REGION_SHIFT) | base;

        /*
         * A range flush degrades to a full flush of every dirty translated
         * MMU index whenever the range is longer than the softmmu table's
         * byte span (upstream tlb_flush_range_locked: len > f->mask).  The
         * tables sit at a few hundred entries, so even an 8 KiB Windows page
         * or a 16 KiB Linux page always tripped that path -- discarding the
         * whole softmmu TLB (and, for the instruction case, the jump cache)
         * on every itc/purge/VHPT-walk completion and provoking a refill
         * storm.  For pages small enough that a per-4 KiB-page walk is
         * cheaper than rebuilding the table, flush each 4 KiB page
         * individually; single-page flushes never degrade.  Genuinely large
         * pages (TR-sized granules) keep the range flush, where a full flush
         * really is the cheaper choice.
         */
        if (entry->ps <= IA64_TLB_PAGEWISE_FLUSH_MAX) {
            for (uint64_t off = 0; off < entry->ps; off += TARGET_PAGE_SIZE) {
                if (is_data) {
                    tlb_flush_page_by_mmuidx_no_jmp_cache(
                        env_cpu(env), va + off, MMU_IDX_TRANSLATED_MASK);
                } else {
                    tlb_flush_page_by_mmuidx(env_cpu(env), va + off,
                                             MMU_IDX_TRANSLATED_MASK);
                }
            }
        } else if (is_data) {
            /*
             * IA-64 has separate data and instruction translation caches.
             * Clearing QEMU's unified softmmu entry is still required, but
             * an architecturally data-only change cannot stale a translated
             * code block or its jump-cache hint.
             */
            tlb_flush_range_by_mmuidx_no_jmp_cache(
                env_cpu(env), va, entry->ps, MMU_IDX_TRANSLATED_MASK,
                TARGET_LONG_BITS);
        } else {
            tlb_flush_range_by_mmuidx(env_cpu(env), va, entry->ps,
                                      MMU_IDX_TRANSLATED_MASK,
                                      TARGET_LONG_BITS);
        }
    }
}

static int ia64_tlb_select_tc_slot(IA64TlbEntry *tlb,
                                   uint16_t *next_replace, uint64_t va,
                                   uint32_t rid, bool *matched);

bool ia64_memory_allows_advanced_load(IA64MemorySpeculation spec)
{
    return spec != IA64_MEM_NON_SPECULATIVE;
}

static bool ia64_memory_allows_control_speculation(IA64MemorySpeculation spec)
{
    return spec == IA64_MEM_SPECULATIVE;
}

static bool ia64_data_address_to_mapped_phys_attr(CPUIA64State *env,
                                                  uint64_t va, bool is_rse,
                                                  uint8_t access_level,
                                                  uint64_t *pa,
                                                  IA64MemorySpeculation *spec)
{
    uint8_t perm;
    uint32_t rid;
    const IA64TlbEntry *entry;

    if (ia64_firmware_identity_pa(env->fw_image_base, env->cr_iva, env->ip, env->psr, va,
                                  pa)) {
        if (spec) {
            *spec = IA64_MEM_SPECULATIVE;
        }
        return true;
    }

    if (ia64_sal_boot_virtual_pa(env, va, pa)) {
        if (spec) {
            *spec = IA64_MEM_SPECULATIVE;
        }
        return true;
    }

    rid = ia64_region_rid(env, va);
    entry = ia64_tlb_find_cached(env, va, rid, false);
    if (entry) {
        ia64_tlb_entry_translate(entry, va, access_level, pa, &perm);
        if (spec) {
            *spec = ia64_pte_memory_speculation(entry->pte);
        }
        return (perm & IA64_TLB_R) != 0;
    }

    {
        uint64_t pte = 0;

        if (ia64_vhpt_walk_full(env, va, rid, false, is_rse, access_level,
                                pa, &perm, &pte, NULL, NULL)) {
            if (spec) {
                *spec = ia64_pte_memory_speculation(pte);
            }
            return (perm & IA64_TLB_R) != 0;
        }
    }

    if (ia64_sal_boot_identity_pa(env, va, pa)) {
        if (spec) {
            *spec = IA64_MEM_SPECULATIVE;
        }
        return true;
    }

    return false;
}


bool ia64_data_address_to_phys_attr(CPUIA64State *env, uint64_t va,
                                    uint64_t *pa,
                                    IA64MemorySpeculation *spec)
{
    if (!(env->psr & IA64_PSR_DT)) {
        *pa = ia64_physical_address(va);
        if (spec) {
            *spec = (va & IA64_PHYS_UC_BIT) ?
                    IA64_MEM_NON_SPECULATIVE :
                    IA64_MEM_LIMITED_SPECULATION;
        }
        return true;
    }

    return ia64_data_address_to_mapped_phys_attr(
        env, va, false, ia64_psr_cpl(env->psr), pa, spec);
}

bool ia64_data_address_to_phys(CPUIA64State *env, uint64_t va,
                               uint64_t *pa)
{
    return ia64_data_address_to_phys_attr(env, va, pa, NULL);
}


void ia64_mmu_fc(CPUIA64State *env, uint64_t addr)
{
    uint64_t pa;

    if ((env->psr & IA64_PSR_DT) ?
        !ia64_va_is_implemented(env, addr) :
        !ia64_pa_is_implemented(env, addr)) {
        ia64_raise_unimplemented_data_address(
            env, addr, IA64_ISR_R, true, false, ia64_code_tlb_ed(env));
    }

    if (ia64_data_address_to_phys(env, addr, &pa)) {
        uint64_t start = pa & ~(IA64_L0_CACHE_LINE_SIZE - 1);

        /* Removing a cache line is an architected ALAT-collision event. */
        ia64_invalidate_alat_phys_range(env, start, IA64_L0_CACHE_LINE_SIZE);
        ia64_exec_invalidate_phys_range(env, start, IA64_L0_CACHE_LINE_SIZE);
    }
}

static void ia64_discard_pending_purge(IA64TlbEntry *entry,
                                       uint16_t *pending_count)
{
    if (!entry->pending_purge) {
        return;
    }

    g_assert(*pending_count > 0);
    entry->pending_purge = 0;
    (*pending_count)--;
}

static bool ia64_purge_tc_entries(CPUIA64State *env, IA64TlbEntry *tlb,
                                  uint16_t *count,
                                  uint16_t *pending_count, uint64_t va,
                                  uint64_t ps, uint32_t rid, bool is_data,
                                  uint16_t *next_replace, int *insert_slot)
{
    int empty = -1;
    uint64_t start = ia64_va_page_base(va, ps);
    uint16_t i;
    bool purged = false;

    if (insert_slot) {
        *insert_slot = -1;
    }
    for (i = 0; i < *count; i++) {
        if (!tlb[i].valid) {
            if (insert_slot && empty < 0) {
                empty = i;
            }
            continue;
        }
        if (tlb[i].rid != rid || tlb[i].is_tr) {
            continue;
        }
        if (ia64_tlb_entry_overlaps(&tlb[i], start, ps, rid)) {
            ia64_qemu_tlb_flush_entry(env, &tlb[i], is_data);
            ia64_discard_pending_purge(&tlb[i], pending_count);
            tlb[i].valid = 0;
            ia64_tlb_bump_slot_generation(env, !is_data, i);
            if (insert_slot && empty < 0) {
                empty = i;
            }
            purged = true;
        }
    }

    while (*count > 0 && !tlb[*count - 1].valid) {
        (*count)--;
    }

    if (insert_slot) {
        if (empty < 0 && *count < IA64_TLB_MAX) {
            empty = *count;
        }
        *insert_slot = empty >= 0 ? empty :
            ia64_tlb_circular_tc_victim(tlb, IA64_TLB_MAX, *next_replace);
        if (*insert_slot >= 0) {
            uint16_t next = *insert_slot + 1;

            *next_replace = next == IA64_TLB_MAX ? 0 : next;
        }
    }

    return purged;
}

static bool ia64_mark_pending_purge_entries(IA64TlbEntry *tlb, uint16_t count,
                                            uint16_t *pending_count,
                                            uint64_t va, uint64_t ps,
                                            uint32_t rid, bool tc_only,
                                            char kind)
{
    uint64_t start = ia64_va_page_base(va, ps);
    uint16_t i;
    bool marked = false;

    for (i = 0; i < count; i++) {
        if ((!tc_only || !tlb[i].is_tr) &&
            ia64_tlb_entry_overlaps(&tlb[i], start, ps, rid)) {
            qemu_log_mask(CPU_LOG_MMU,
                          "ia64 pending purge.%c slot=%u %s"
                          " va=0x%016" PRIx64 " rid=0x%06" PRIx32
                          " pa=0x%016" PRIx64 " ps=0x%016" PRIx64
                          " purge_va=0x%016" PRIx64
                          " purge_ps=0x%016" PRIx64 "\n",
                          kind, i, tlb[i].is_tr ? "TR" : "TC",
                          tlb[i].va, tlb[i].rid, tlb[i].pa, tlb[i].ps,
                          va, ps);
            if (!tlb[i].pending_purge) {
                tlb[i].pending_purge = 1;
                (*pending_count)++;
            }
            marked = true;
        }
    }

    return marked;
}

static bool ia64_mark_pending_purge_all_tc(IA64TlbEntry *tlb, uint16_t count,
                                           uint16_t *pending_count,
                                           char kind)
{
    uint16_t i;
    bool marked = false;

    for (i = 0; i < count; i++) {
        if (!tlb[i].is_tr && tlb[i].valid) {
            qemu_log_mask(CPU_LOG_MMU,
                          "ia64 pending purge.%c slot=%u TC"
                          " va=0x%016" PRIx64 " rid=0x%06" PRIx32
                          " pa=0x%016" PRIx64 " ps=0x%016" PRIx64
                          " purge=all-tc\n",
                          kind, i, tlb[i].va, tlb[i].rid,
                          tlb[i].pa, tlb[i].ps);
            if (!tlb[i].pending_purge) {
                tlb[i].pending_purge = 1;
                (*pending_count)++;
            }
            marked = true;
        }
    }

    return marked;
}

#define IA64_TLB_TARGETED_PURGE_LIMIT 8

#ifdef CONFIG_DEBUG_TCG
static uint16_t ia64_count_pending_purges(const IA64TlbEntry *tlb,
                                          uint16_t count)
{
    uint16_t pending = 0;
    uint16_t i;

    for (i = 0; i < count; i++) {
        pending += tlb[i].valid && tlb[i].pending_purge;
    }
    return pending;
}

static void ia64_assert_pending_purge_counts(CPUIA64State *env)
{
    g_assert(env->mmu.pending_purge_data_count ==
             ia64_count_pending_purges(env->mmu.tlb_data,
                                       env->mmu.tlb_data_count));
    g_assert(env->mmu.pending_purge_inst_count ==
             ia64_count_pending_purges(env->mmu.tlb_inst,
                                       env->mmu.tlb_inst_count));
}
#else
static inline void ia64_assert_pending_purge_counts(CPUIA64State *env)
{
    (void)env;
}
#endif

static bool ia64_complete_pending_purges(CPUIA64State *env,
                                         IA64TlbEntry *tlb, uint16_t *count,
                                         uint16_t *pending_count, char kind,
                                         bool targeted, bool is_ifetch)
{
    uint16_t i;
    bool purged = false;

    for (i = 0; i < *count; i++) {
        if (tlb[i].valid && tlb[i].pending_purge) {
            qemu_log_mask(CPU_LOG_MMU,
                          "ia64 complete purge.%c slot=%u %s"
                          " va=0x%016" PRIx64 " rid=0x%06" PRIx32
                          " pa=0x%016" PRIx64 " ps=0x%016" PRIx64 "\n",
                          kind, i, tlb[i].is_tr ? "TR" : "TC",
                          tlb[i].va, tlb[i].rid, tlb[i].pa, tlb[i].ps);
            if (targeted) {
                ia64_qemu_tlb_flush_entry(env, &tlb[i], !is_ifetch);
            }
            tlb[i].pending_purge = 0;
            g_assert(*pending_count > 0);
            (*pending_count)--;
            tlb[i].valid = 0;
            ia64_tlb_bump_slot_generation(env, is_ifetch, i);
            purged = true;
        }
    }

    while (*count > 0 && !tlb[*count - 1].valid) {
        (*count)--;
    }

    return purged;
}

void ia64_tlb_serialize(CPUIA64State *env, uint32_t serialize_data,
                          uint32_t serialize_inst)
{
    bool data_purged = false;
    bool inst_purged = false;
    uint16_t pending = 0;
    bool targeted;

    if (serialize_data) {
        pending += env->mmu.pending_purge_data_count;
    }
    if (serialize_inst) {
        pending += env->mmu.pending_purge_inst_count;
    }
    targeted = pending <= IA64_TLB_TARGETED_PURGE_LIMIT;
    trace_ia64_mmu_serialize(env_cpu(env)->cpu_index, serialize_data,
                             serialize_inst, pending, targeted);

    if (serialize_data) {
        env->exception_state.psr_ic_inflight = false;
        if (env->mmu.pending_purge_data_count != 0) {
            data_purged = ia64_complete_pending_purges(
                env, env->mmu.tlb_data, &env->mmu.tlb_data_count,
                &env->mmu.pending_purge_data_count, 'd', targeted, false);
        }
    }
    if (serialize_inst && env->mmu.pending_purge_inst_count != 0) {
        inst_purged = ia64_complete_pending_purges(
            env, env->mmu.tlb_inst, &env->mmu.tlb_inst_count,
            &env->mmu.pending_purge_inst_count, 'i', targeted, true);
    }
    if (!targeted && (data_purged || inst_purged)) {
        tlb_flush(env_cpu(env));
    }
    ia64_assert_pending_purge_counts(env);

    /*
     * An instruction translation change does not change guest code.  The
     * softmmu flush also clears the virtual-PC jump cache, and the next
     * global TB lookup resolves the new physical page before matching a TB.
     * Keep the physical-page-keyed TBs so they can be reused when a mapping
     * becomes current again.
     */
}

static int ia64_tlb_select_tc_slot(IA64TlbEntry *tlb,
                                   uint16_t *next_replace, uint64_t va,
                                   uint32_t rid, bool *matched)
{
    int empty = -1;
    int victim;
    uint16_t i;

    *matched = false;
    for (i = 0; i < IA64_TLB_MAX; i++) {
        if (!tlb[i].valid) {
            if (empty < 0) {
                empty = i;
            }
            continue;
        }
        if (tlb[i].is_tr) {
            continue;
        }
        if (tlb[i].va == va && tlb[i].rid == rid) {
            *matched = true;
            return i;
        }
    }

    if (empty >= 0) {
        *next_replace = (empty + 1) % IA64_TLB_MAX;
        return empty;
    }

    victim = ia64_tlb_circular_tc_victim(tlb, IA64_TLB_MAX, *next_replace);
    if (victim >= 0) {
        /* Keep replacement moving forward over non-TR entries. */
        *next_replace = (victim + 1) % IA64_TLB_MAX;
        return victim;
    }

    return -1;
}

static bool ia64_cache_replaced_tr(CPUIA64State *env, IA64TlbEntry *tlb,
                                   uint16_t *cnt,
                                   uint16_t *next_replace,
                                   uint16_t *pending_count,
                                   const IA64TlbEntry *old_tr,
                                   bool is_ifetch)
{
    uint32_t micro_generation;
    bool matched;
    int slot;

    if (!old_tr->valid || !old_tr->is_tr) {
        return false;
    }

    /*
     * Replacing a TR slot does not purge the previous translation from the
     * processor TLBs.  Model that architected behavior as a TC copy, which
     * remains until normal TC replacement or an explicit ptr purge.
     */
    slot = ia64_tlb_select_tc_slot(tlb, next_replace, old_tr->va,
                                   old_tr->rid, &matched);
    if (slot < 0) {
        qemu_log_mask(CPU_LOG_MMU,
                      "ia64 cache replaced tr failed va=0x%016" PRIx64
                      " rid=0x%06" PRIx32 " ps=0x%016" PRIx64 "\n",
                      old_tr->va, old_tr->rid, old_tr->ps);
        return false;
    }

    qemu_log_mask(CPU_LOG_MMU,
                  "ia64 cache replaced tr slot=%d va=0x%016" PRIx64
                  " rid=0x%06" PRIx32 " pa=0x%016" PRIx64
                  " ps=0x%016" PRIx64 "\n",
                  slot, old_tr->va, old_tr->rid, old_tr->pa, old_tr->ps);
    ia64_discard_pending_purge(&tlb[slot], pending_count);
    micro_generation = tlb[slot].micro_generation;
    tlb[slot] = *old_tr;
    tlb[slot].micro_generation = micro_generation;
    tlb[slot].is_tr = 0;
    tlb[slot].slot = slot;
    ia64_tlb_bump_slot_generation(env, is_ifetch, slot);
    if (slot >= *cnt) {
        *cnt = slot + 1;
    }
    return true;
}

void ia64_mmu_itr_insert(CPUIA64State *env, uint64_t pte, uint64_t slot_reg,
                       uint32_t is_data, uint64_t raw, uint32_t fault_slot)
{
    IA64TlbEntry *tlb;
    uint16_t *cnt;
    uint16_t *pending_count;
    uint64_t ps;
    uint64_t va;
    uint64_t pa;
    uint32_t key;
    uint8_t ar;
    uint8_t pl;
    uint8_t perm;
    uint32_t rid;
    uint32_t slot = slot_reg & 0xff;
    uint16_t *next_replace;
    bool cached_old_tr;

    /* itr.d (is_data=1) targets the DTR file, itr.i (is_data=0) the ITR file. */
    uint8_t tr_slots = is_data ? ia64_env_cpu_class(env)->dtr_count
                               : ia64_env_cpu_class(env)->itr_count;
    if (slot >= tr_slots) {
        env->cr_isr = 0x30;
        ia64_raise_exception(env, IA64_EXCP_RESERVED_REG_FIELD,
                               ia64_ip_bundle_addr(env->ip), raw,
                               fault_slot);
        return;
    }
    if (!ia64_translation_insert_fields_valid(env, pte, env->cr_itir)) {
        env->cr_isr = 0x30;
        ia64_raise_exception(env, IA64_EXCP_RESERVED_REG_FIELD,
                               ia64_ip_bundle_addr(env->ip), raw,
                               fault_slot);
        return;
    }

    ps = ia64_itir_page_size(env);
    va = env->cr_ifa & ~(ps - 1);
    pa = (pte & ia64_pte_ppn_mask(env)) & ~(ps - 1);
    key = (env->cr_itir & IA64_ITIR_KEY_MASK) >> IA64_ITIR_KEY_SHIFT;
    ar = ia64_pte_ar(pte);
    pl = ia64_pte_pl(pte);
    perm = ia64_pte_perm(pte, 0);
    rid = ia64_region_rid(env, env->cr_ifa);

    if (!ia64_va_is_implemented(env, env->cr_ifa)) {
        ia64_raise_unimplemented_data_address(
            env, env->cr_ifa, 0, true, false, ia64_code_tlb_ed(env));
    }

    if ((pte & IA64_PTE_PRESENT) && perm == 0) {
        return;
    }

    if (is_data) {
        tlb = env->mmu.tlb_data;
        cnt = &env->mmu.tlb_data_count;
        next_replace = &env->mmu.tlb_data_replace;
        pending_count = &env->mmu.pending_purge_data_count;
    } else {
        tlb = env->mmu.tlb_inst;
        cnt = &env->mmu.tlb_inst_count;
        next_replace = &env->mmu.tlb_inst_replace;
        pending_count = &env->mmu.pending_purge_inst_count;
    }

    IA64TlbEntry old_tr = tlb[slot];

    ia64_purge_tc_entries(env, tlb, cnt, pending_count, va, ps, rid,
                          is_data, NULL, NULL);
    if (old_tr.valid && !old_tr.is_tr) {
        ia64_qemu_tlb_flush_entry(env, &old_tr, is_data);
    }
    cached_old_tr = ia64_cache_replaced_tr(env, tlb, cnt, next_replace,
                                           pending_count, &old_tr,
                                           !is_data);
    if (!cached_old_tr) {
        ia64_discard_pending_purge(&tlb[slot], pending_count);
    }

    tlb[slot].va = va;
    tlb[slot].pa = pa;
    tlb[slot].ps = ps;
    tlb[slot].page_mask = ia64_va_vpn_mask(ps);
    tlb[slot].pte = pte;
    tlb[slot].perm = perm;
    tlb[slot].ar = ar;
    tlb[slot].pl = pl;
    tlb[slot].valid = 1;
    tlb[slot].is_tr = 1;
    tlb[slot].pending_purge = 0;
    tlb[slot].rid = rid;
    tlb[slot].key = key;
    tlb[slot].slot = slot;
    if (slot >= *cnt) {
        *cnt = slot + 1;
    }
    ia64_tlb_bump_slot_generation(env, !is_data, slot);
    ia64_assert_pending_purge_counts(env);
    qemu_log_mask(CPU_LOG_MMU,
                  "ia64 itr.%c slot=%u va=0x%016" PRIx64
                  " rid=0x%06" PRIx32 " pa=0x%016" PRIx64
                  " ps=0x%016" PRIx64 " pte=0x%016" PRIx64 "\n",
                  is_data ? 'd' : 'i', slot, va, rid, pa, ps, pte);
    /*
     * Only the new TR's VA range can have stale softmmu entries: any
     * overlapping TC was already purged (ia64_purge_tc_entries) and the
     * replaced slot's range flushed above, and inserting a TR changes no
     * mapping outside its own range.  Flush just that range instead of the
     * whole TLB -- and, for a data TR (which cannot stale a code block), keep
     * the jump cache.  ia64_qemu_tlb_flush_entry() leaves the PHYS index (not
     * region-mapped) intact in every case.
     */
    ia64_qemu_tlb_flush_entry(env, &tlb[slot], is_data);
}

void ia64_mmu_ptr_purge(CPUIA64State *env, uint64_t ifa, uint64_t size_reg,
                      uint32_t is_data)
{
    IA64TlbEntry *tlb;
    uint64_t ps = ia64_gr_page_size(size_reg);
    uint64_t va = ifa & ~(ps - 1);
    uint32_t rid = ia64_region_rid(env, ifa);
    uint16_t count;

    if (!ia64_va_is_implemented(env, ifa)) {
        ia64_raise_unimplemented_data_address(
            env, ifa, 0, true, false, ia64_code_tlb_ed(env));
    }

    if (is_data) {
        tlb = env->mmu.tlb_data;
        count = env->mmu.tlb_data_count;
    } else {
        tlb = env->mmu.tlb_inst;
        count = env->mmu.tlb_inst_count;
    }

    trace_ia64_mmu_purge(env_cpu(env)->cpu_index,
                         is_data ? "ptr.d" : "ptr.i", va, ps, rid,
                         is_data);
    ia64_mark_pending_purge_entries(
        tlb, count,
        is_data ? &env->mmu.pending_purge_data_count :
                  &env->mmu.pending_purge_inst_count,
        va, ps, rid, false, is_data ? 'd' : 'i');
    ia64_assert_pending_purge_counts(env);
}

typedef struct IA64PtcGlobalWork {
    uint64_t va;
    uint64_t ps;
    uint32_t rid;
    bool global_alat;
} IA64PtcGlobalWork;

static void ia64_ptc_mark_global(CPUIA64State *env,
                                 const IA64PtcGlobalWork *work,
                                 bool remote)
{
    ia64_mark_pending_purge_entries(
        env->mmu.tlb_data, env->mmu.tlb_data_count,
        &env->mmu.pending_purge_data_count,
        work->va, work->ps, work->rid, true, 'd');
    ia64_mark_pending_purge_entries(
        env->mmu.tlb_inst, env->mmu.tlb_inst_count,
        &env->mmu.pending_purge_inst_count,
        work->va, work->ps, work->rid, true, 'i');
    ia64_assert_pending_purge_counts(env);

    if (remote) {
        /* The remote processor must not execute through the old mapping. */
        ia64_tlb_serialize(env, 1, 1);
        if (work->global_alat) {
            memset(env->alat_state.alat, 0, sizeof(env->alat_state.alat));
            env->alat_state.alat_active_count = 0;
        }
    }
}

static void ia64_ptc_global_remote_work(CPUState *cs, run_on_cpu_data data)
{
    IA64PtcGlobalWork *work = data.host_ptr;

    ia64_ptc_mark_global(cpu_env(cs), work, true);
    g_free(work);
}

static void ia64_ptc_global_source_work(CPUState *cs, run_on_cpu_data data)
{
    IA64PtcGlobalWork *work = data.host_ptr;

    ia64_ptc_mark_global(cpu_env(cs), work, false);
    g_free(work);
}

void ia64_mmu_ptc_purge(CPUIA64State *env, uint64_t va, uint64_t size_reg,
                      uint32_t mode)
{
    uint32_t rid = ia64_region_rid(env, va);
    uint64_t ps = ia64_gr_page_size(size_reg);

    /*
     * ptc.e (mode 2) takes an implementation-specific purge count in r3, not a
     * virtual address, so it must not raise an Unimplemented Data Address fault.
     */
    if (mode != 2 && !ia64_va_is_implemented(env, va)) {
        ia64_raise_unimplemented_data_address(
            env, va, 0, true, false, ia64_code_tlb_ed(env));
    }

    trace_ia64_mmu_purge(env_cpu(env)->cpu_index, "ptc", va, ps, rid, mode);
    if (mode == 1 || mode == 3) {
        CPUState *src = env_cpu(env);
        CPUState *cs;
        IA64PtcGlobalWork template = {
            .va = va & ~(ps - 1),
            .ps = ps,
            .rid = rid,
            .global_alat = mode == 3,
        };
        bool wait = false;

        CPU_FOREACH(cs) {
            if (cs != src) {
                IA64PtcGlobalWork *work = g_new(IA64PtcGlobalWork, 1);

                *work = template;
                async_run_on_cpu(cs, ia64_ptc_global_remote_work,
                                 RUN_ON_CPU_HOST_PTR(work));
                wait = true;
            }
        }
        if (wait) {
            IA64PtcGlobalWork *work = g_new(IA64PtcGlobalWork, 1);

            *work = template;
            async_safe_run_on_cpu(src, ia64_ptc_global_source_work,
                                  RUN_ON_CPU_HOST_PTR(work));
        } else {
            ia64_ptc_mark_global(env, &template, false);
        }
    } else if (mode == 2) {
        ia64_mark_pending_purge_all_tc(
            env->mmu.tlb_data, env->mmu.tlb_data_count,
            &env->mmu.pending_purge_data_count, 'd');
        ia64_mark_pending_purge_all_tc(
            env->mmu.tlb_inst, env->mmu.tlb_inst_count,
            &env->mmu.pending_purge_inst_count, 'i');
    } else {
        ia64_mark_pending_purge_entries(
            env->mmu.tlb_data, env->mmu.tlb_data_count,
            &env->mmu.pending_purge_data_count, va, ps, rid, true, 'd');
        ia64_mark_pending_purge_entries(
            env->mmu.tlb_inst, env->mmu.tlb_inst_count,
            &env->mmu.pending_purge_inst_count, va, ps, rid, true, 'i');
    }
    ia64_assert_pending_purge_counts(env);
}

uint64_t ia64_mmu_tpa(CPUIA64State *env, uint64_t va)
{
    CPUState *cs = env_cpu(env);
    uint64_t pa;
    uint8_t perm;
    uint32_t rid = ia64_region_rid(env, va);
    IA64Exception excp;
    uint8_t vhpt_size;
    bool vhpt_long_format;
    bool vhpt_enabled;
    uint64_t pte;
    uint32_t key;
    const IA64TlbEntry *entry;

    if (env->psr & IA64_PSR_DT) {
        if (!ia64_va_is_implemented(env, va)) {
            excp = IA64_EXCP_UNIMPL_DATA_ADDR;
            goto tpa_fault;
        }

        entry = ia64_tlb_find_cached(env, va, rid, false);
        if (entry) {
            ia64_tlb_entry_translate(entry, va, ia64_psr_cpl(env->psr), &pa,
                                     &perm);
            excp = ia64_tlb_exception_for_access(env, entry, perm,
                                                 IA64_TLB_R, false, false,
                                                 false);
            if (excp != IA64_EXCP_NONE) {
                goto tpa_fault;
            }
            return pa;
        }
        pte = 0;
        key = 0;
        if (ia64_vhpt_walk_full(env, va, rid, false, false,
                                ia64_psr_cpl(env->psr), &pa, &perm, &pte,
                                &key, &entry)) {
            excp = entry ?
                ia64_tlb_exception_for_access(env, entry, perm, IA64_TLB_R,
                                              false, false, false) :
                ia64_translation_exception_for_access(env, pte, key, perm,
                                                      IA64_TLB_R, false,
                                                      false, false);
            if (excp != IA64_EXCP_NONE) {
                goto tpa_fault;
            }
            return pa;
        }
        if (ia64_sal_boot_identity_pa(env, va, &pa)) {
            return pa;
        }
        vhpt_enabled = ia64_vhpt_walker_enabled(env, va, false, false,
                                                &vhpt_size,
                                                &vhpt_long_format);
        if (ia64_data_nested_tlb_active(env)) {
            excp = IA64_EXCP_DATA_NESTED_TLB;
        } else if (!ia64_vhpt_entry_accessible(env, va, false, false,
                                               &env->cr_iha)) {
            excp = IA64_EXCP_VHPT_FAULT;
        } else if (vhpt_enabled) {
            excp = IA64_EXCP_DTLB_FAULT;
        } else {
            excp = IA64_EXCP_ALT_DTLB;
        }
    } else {
        entry = ia64_tlb_find_cached(env, va, rid, false);
        if (entry) {
            ia64_tlb_entry_translate(entry, va, ia64_psr_cpl(env->psr),
                                     &pa, &perm);
            excp = ia64_tlb_exception_for_access(
                env, entry, perm, IA64_TLB_R, false, false, false);
            if (excp != IA64_EXCP_NONE) {
                goto tpa_fault;
            }
            return pa;
        }
        excp = IA64_EXCP_ALT_DTLB;
    }

tpa_fault:
    if (env->psr & IA64_PSR_IC) {
        env->cr_ifa = va;
        if (ia64_exception_initializes_iha(excp)) {
            env->cr_iha = ia64_vhpt_hash_address(env, va);
        }
        env->cr_itir = ia64_region_itir(
            env, excp == IA64_EXCP_VHPT_FAULT ? env->cr_iha : va);
    }
    if (excp != IA64_EXCP_DATA_NESTED_TLB) {
        env->cr_isr = IA64_ISR_NA;
        if (excp == IA64_EXCP_UNIMPL_DATA_ADDR) {
            env->cr_isr |= IA64_GENEX_UNIMPL_DATA_ADDR;
        }
    }
    cs->exception_index = excp;
    cpu_loop_exit(cs);
}

static uint64_t ia64_probe_address(CPUIA64State *env, uint64_t va,
                                   uint32_t is_write, uint32_t is_ifetch,
                                   uint8_t access_level)
{
    uint64_t pa;
    uint8_t perm;
    uint32_t rid = ia64_region_rid(env, va);
    uint8_t needed;
    const IA64TlbEntry *entry;

    needed = is_write ? IA64_TLB_W :
             (is_ifetch ? IA64_TLB_X : IA64_TLB_R);

    if (!(env->psr & (is_ifetch ? IA64_PSR_IT : IA64_PSR_DT))) {
        return 1;
    }

    if (!ia64_va_is_implemented(env, va)) {
        return 0;
    }

    if (ia64_firmware_identity_pa(env->fw_image_base, env->cr_iva, env->ip, env->psr, va,
                                  &pa)) {
        return 1;
    }

    if (ia64_sal_boot_virtual_pa(env, va, &pa)) {
        return 1;
    }

    entry = ia64_tlb_find_cached(env, va, rid, is_ifetch);
    if (entry) {
        IA64Exception excp;

        ia64_tlb_entry_translate(entry, va, access_level, &pa, &perm);
        excp = ia64_tlb_exception_for_access(env, entry, perm, needed,
                                             is_ifetch, is_write, false);
        return excp == IA64_EXCP_NONE ? 1 : 0;
    }

    if (ia64_sal_boot_identity_pa_type(env, va, &pa, is_ifetch)) {
        return 1;
    }
    return 0;
}

typedef struct IA64DataReferenceResult {
    uint64_t pa;
    IA64MemorySpeculation speculation;
    uint8_t memory_attribute;
    bool valid;
} IA64DataReferenceResult;

static void ia64_set_data_reference_result(IA64DataReferenceResult *result,
                                           uint64_t pa,
                                           IA64MemorySpeculation speculation,
                                           uint8_t memory_attribute)
{
    if (result) {
        result->pa = pa;
        result->speculation = speculation;
        result->memory_attribute = memory_attribute;
        result->valid = true;
    }
}

static IA64Exception
ia64_data_reference_exception(CPUIA64State *env, uint64_t va,
                              uint32_t is_write, uint32_t is_rw,
                              uint8_t access_level, bool walk_vhpt,
                              IA64DataReferenceResult *result)
{
    uint64_t pa;
    uint8_t perm;
    uint8_t needed = is_rw ? (IA64_TLB_R | IA64_TLB_W) :
                     (is_write ? IA64_TLB_W : IA64_TLB_R);
    uint32_t rid = ia64_region_rid(env, va);
    uint8_t vhpt_size;
    bool vhpt_long_format;
    bool vhpt_enabled;
    uint64_t pte = IA64_PTE_PRESENT;
    uint32_t key = 0;
    const IA64TlbEntry *entry;
    bool found = false;

    if (result) {
        result->valid = false;
    }
    if (!(env->psr & IA64_PSR_DT)) {
        if (!ia64_pa_is_implemented(env, va)) {
            return IA64_EXCP_UNIMPL_DATA_ADDR;
        }
        pa = ia64_physical_address(va);
        ia64_set_data_reference_result(
            result, pa, (va & IA64_PHYS_UC_BIT) ?
                        IA64_MEM_NON_SPECULATIVE :
                        IA64_MEM_LIMITED_SPECULATION,
            (va & IA64_PHYS_UC_BIT) ? IA64_PTE_MA_UC : IA64_PTE_MA_WB);
        return IA64_EXCP_NONE;
    }
    if (ia64_firmware_identity_pa(env->fw_image_base, env->cr_iva, env->ip, env->psr, va,
                                  &pa) ||
        ia64_sal_boot_virtual_pa(env, va, &pa)) {
        ia64_set_data_reference_result(result, pa, IA64_MEM_SPECULATIVE,
                                       IA64_PTE_MA_WB);
        return IA64_EXCP_NONE;
    }

    if (!ia64_va_is_implemented(env, va)) {
        return IA64_EXCP_UNIMPL_DATA_ADDR;
    }

    entry = ia64_tlb_find_cached(env, va, rid, false);
    if (entry) {
        ia64_tlb_entry_translate(entry, va, access_level, &pa, &perm);
        pte = entry->pte;
        found = true;
    } else if (walk_vhpt) {
        found = ia64_vhpt_walk_full(env, va, rid, false, false,
                                    access_level, &pa, &perm, &pte, &key,
                                    &entry);
    }

    if (found) {
        uint64_t resolved_pte = entry ? entry->pte : pte;
        uint8_t ma = (resolved_pte & IA64_PTE_MA_MASK) >> IA64_PTE_MA_SHIFT;

        ia64_set_data_reference_result(
            result, pa, ia64_pte_memory_speculation(resolved_pte), ma);

        if (entry) {
            return ia64_tlb_exception_for_access(env, entry, perm, needed,
                                                false, is_write || is_rw,
                                                false);
        }
        return ia64_translation_exception_for_access(env, pte, key, perm,
                                                     needed, false,
                                                     is_write || is_rw,
                                                     false);
    }
    if (ia64_sal_boot_identity_pa(env, va, &pa)) {
        ia64_set_data_reference_result(result, pa, IA64_MEM_SPECULATIVE,
                                       IA64_PTE_MA_WB);
        return IA64_EXCP_NONE;
    }
    if (ia64_data_nested_tlb_active(env)) {
        return IA64_EXCP_DATA_NESTED_TLB;
    }
    if (ia64_vhpt_pte_not_present(env, va, false, false, NULL)) {
        return IA64_EXCP_PAGE_NOT_PRESENT;
    }
    vhpt_enabled = ia64_vhpt_walker_enabled(env, va, false, false,
                                            &vhpt_size, &vhpt_long_format);
    if (!ia64_vhpt_entry_accessible(env, va, false, false, &pa)) {
        return IA64_EXCP_VHPT_FAULT;
    }

    return vhpt_enabled ? IA64_EXCP_DTLB_FAULT : IA64_EXCP_ALT_DTLB;
}

bool ia64_translate_data_access(CPUIA64State *env, uint64_t va,
                                bool is_write, uint64_t *pa)
{
    IA64DataReferenceResult result;
    IA64Exception excp;

    if (env == NULL || pa == NULL) {
        return false;
    }
    excp = ia64_data_reference_exception(
        env, va, is_write, false, ia64_psr_cpl(env->psr), true, &result);
    if (excp != IA64_EXCP_NONE || !result.valid) {
        return false;
    }
    *pa = result.pa;
    return true;
}

static bool ia64_vhpt_lookup_pte(CPUIA64State *env, uint64_t va,
                                 bool is_ifetch, bool is_rse, uint64_t *pte,
                                 uint64_t *entry_va, uint8_t *page_shift);

/*
 * The ED bit of the code page the CPU is currently executing from.
 *
 * ia64_current_code_tlb_ed() answers this from the modelled ITLB alone, which
 * is not sufficient here.  With PSR.it set, real hardware cannot execute an
 * instruction unless a valid ITLB entry for its page exists - an evicted or
 * purged entry is re-inserted by the very next instruction fetch, so ITLB.ed
 * is always well defined at execution time.  We can lose it: QEMU executes a
 * cached translation block without re-consulting the guest ITLB, so our
 * round-robin replacement (IA64_TLB_MAX entries) can drop the entry for code
 * that is still running.  Reporting ED as 0 in that state is a pure emulation
 * artifact, and it is not harmless: SDM Vol.2 rev 1.1 Table 5-4 defers an
 * Unaligned Data Reference on a speculative load iff
 * "!PSR.ic || (PSR.it && ITLB.ed)" - with no DCR bit involved - so a lost
 * entry turns a deferral into a fault.  Windows IA-64 runs with PSR.ac set and
 * maps kernel code with ED, and its compiler hoists ld.s off pointers that may
 * be wild; build 2462 bugchecks 0x1E in Npfs.SYS when one of those faults
 * instead of NaTing.
 *
 * Recover the architected bit from the VHPT when the entry is missing.
 */
static bool ia64_code_tlb_ed_lookup(CPUIA64State *env, bool *known)
{
    const IA64TlbEntry *entry;
    uint64_t pte, entry_va;

    *known = true;
    if (!(env->psr & IA64_PSR_IT)) {
        return false;
    }

    entry = ia64_tlb_find_cached(env, env->ip,
                                 ia64_region_rid(env, env->ip), true);
    if (entry) {
        return (entry->pte & IA64_PTE_ED) != 0;
    }

    if (ia64_vhpt_lookup_pte(env, env->ip, true, false, &pte, &entry_va,
                             NULL)) {
        return (pte & IA64_PTE_ED) != 0;
    }

    /*
     * Neither source can answer.  The VHPT is only a best-effort fallback:
     * the walker may be disabled for the region (RR.ve), and a short-format
     * VHPT holds one entry per hash bucket, so a colliding page routinely
     * displaces the one we want.
     */
    *known = false;
    return false;
}

static bool ia64_code_tlb_ed(CPUIA64State *env)
{
    bool known;

    return ia64_code_tlb_ed_lookup(env, &known);
}

static uint64_t ia64_speculative_deferral_dcr_mask(IA64Exception excp)
{
    switch (excp) {
    case IA64_EXCP_ALT_DTLB:
    case IA64_EXCP_VHPT_FAULT:
    case IA64_EXCP_DTLB_FAULT:
        return IA64_DCR_DM;
    case IA64_EXCP_PAGE_NOT_PRESENT:
        return IA64_DCR_DP;
    case IA64_EXCP_DATA_KEY_MISS:
        return IA64_DCR_DK;
    case IA64_EXCP_KEY_PERMISSION:
        return IA64_DCR_DX;
    case IA64_EXCP_DATA_ACCESS:
        return IA64_DCR_DR;
    case IA64_EXCP_DATA_ACCESS_BIT:
        return IA64_DCR_DA;
    case IA64_EXCP_UNIMPL_DATA_ADDR:
        return UINT64_MAX;
    default:
        return 0;
    }
}

static bool ia64_speculative_exception_deferrable(CPUIA64State *env,
                                                  IA64Exception excp,
                                                  bool itlb_ed,
                                                  bool itlb_ed_unknown)
{
    uint64_t dcr_mask;

    if (!(env->psr & IA64_PSR_IC)) {
        return true;
    }

    if (excp == IA64_EXCP_NAT_CONSUMPTION) {
        return true;
    }

    if (excp == IA64_EXCP_UNALIGNED) {
        /*
         * SDM Vol.2 rev 1.1 Table 5-4: deferred iff
         * "!PSR.ic || (PSR.it && ITLB.ed)" - no DCR bit is involved, so
         * ITLB.ed alone decides it.
         *
         * When we cannot determine ITLB.ed at all, defer.  Executing here
         * with PSR.it set means real hardware necessarily holds a valid
         * ITLB entry for this page, so "no entry" is an emulation artifact
         * (QEMU runs a cached translation block without re-consulting the
         * guest ITLB, and our replacement can drop the entry for code that
         * is still running).  Of the two answers we could guess, deferring
         * is the one that cannot turn a recoverable control speculation
         * into a bugcheck - the ld.s already has a chk.s recovery path by
         * construction, which is why the compiler emitted it.
         */
        return (env->psr & IA64_PSR_IT) && (itlb_ed || itlb_ed_unknown);
    }

    dcr_mask = ia64_speculative_deferral_dcr_mask(excp);
    if (dcr_mask == UINT64_MAX) {
        return true;
    }

    /*
     * A deferrable fault is deferred when the ld.s bundle's page carries the
     * TLB ED bit, OR when the DCR mask bit for the fault is set.  These are
     * independent enables (SDM Vol.2, control speculation): in particular the
     * DCR path does not require instruction translation to be enabled, so an
     * ld.s issued with PSR.it == 0 (physical code, as in the OS loaders) still
     * defers a TLB/access fault whenever the corresponding DCR bit is set.
     * (ia64_current_code_tlb_ed() already returns false when PSR.it == 0, so
     * the ED path keeps its dependence on instruction translation.)
     */
    if (itlb_ed) {
        return true;
    }

    return dcr_mask != 0 && (env->cr_dcr & dcr_mask);
}

static bool ia64_speculative_alignment_fault(CPUIA64State *env,
                                             uint64_t va, uint32_t size)
{
    if (size <= 1 || (va & (size - 1)) == 0) {
        return false;
    }

    return (env->psr & IA64_PSR_AC) ||
           ((va & 0xfff) + size - 1 > 0xfff);
}

static void ia64_raise_data_reference_exception_at(CPUIA64State *env,
                                                   uint64_t va,
                                                   uint32_t is_write,
                                                   uint32_t is_rw,
                                                   bool is_non_access,
                                                   uint8_t non_access_code,
                                                   IA64Exception excp,
                                                   bool is_speculative,
                                                   bool itlb_ed,
                                                   uint64_t fault_ip,
                                                   uint8_t fault_slot)
{
    CPUState *cs = env_cpu(env);

    env->exception_state.fault_addr = va;
    if (env->psr & IA64_PSR_IC) {
        env->cr_ifa = va;
        if (ia64_exception_initializes_iha(excp)) {
            env->cr_iha = ia64_vhpt_hash_address(env, va);
        }
        env->cr_itir = ia64_region_itir(
            env, excp == IA64_EXCP_VHPT_FAULT ? env->cr_iha : va);
    }
    if (excp != IA64_EXCP_DATA_NESTED_TLB) {
        if (excp == IA64_EXCP_UNIMPL_DATA_ADDR) {
            env->cr_isr = IA64_GENEX_UNIMPL_DATA_ADDR |
                          (is_non_access ? IA64_ISR_NA : 0) |
                          (is_rw ? (IA64_ISR_R | IA64_ISR_W) :
                           (is_write ? IA64_ISR_W : IA64_ISR_R));
        } else {
            env->cr_isr = (is_non_access ?
                           IA64_ISR_NA | non_access_code : 0) |
                          (is_rw ? (IA64_ISR_R | IA64_ISR_W) :
                           (is_write ? IA64_ISR_W : IA64_ISR_R));
            if (excp == IA64_EXCP_NAT_CONSUMPTION) {
                /*
                 * Data NaT Page Consumption reports ISR.code{5:4} = 2 above
                 * the non-access instruction code in ISR.code{3:0}, which is
                 * zero for an ordinary access.
                 */
                env->cr_isr |= IA64_ISR_CODE_NAT_PAGE;
            }
        }
        /* Data NaT Page Consumption always reports ISR.sp and ISR.ed as 0. */
        if (is_speculative && excp != IA64_EXCP_NAT_CONSUMPTION) {
            env->cr_isr |= IA64_ISR_SP;
        }
        if (itlb_ed && excp != IA64_EXCP_NAT_CONSUMPTION) {
            env->cr_isr |= IA64_ISR_ED;
        }
    }
    env->exception_state.fault_ip = fault_ip;
    env->exception_state.fault_imm = 0;
    env->exception_state.fault_slot = fault_slot;
    env->exception_state.exception = excp;
    cs->exception_index = excp;
    cpu_loop_exit(cs);
}

static void ia64_raise_data_reference_exception(CPUIA64State *env,
                                                uint64_t va,
                                                uint32_t is_write,
                                                uint32_t is_rw,
                                                bool is_non_access,
                                                uint8_t non_access_code,
                                                IA64Exception excp,
                                                bool is_speculative,
                                                bool itlb_ed)
{
    ia64_raise_data_reference_exception_at(
        env, va, is_write, is_rw, is_non_access, non_access_code, excp,
        is_speculative, itlb_ed, ia64_ip_bundle_addr(env->ip),
        (env->psr & IA64_PSR_RI_MASK) >> IA64_PSR_RI_SHIFT);
}

static uint8_t ia64_probe_access_level(CPUIA64State *env,
                                       uint64_t access_level)
{
    uint8_t requested_pl = access_level & 3;
    uint8_t current_cpl = ia64_psr_cpl(env->psr);

    return requested_pl < current_cpl ? current_cpl : requested_pl;
}

/*
 * Map a translation-check result to the architected result of the
 * non-faulting probe forms.  tlb_grant_permission() takes VHPT
 * Translation, TLB Miss, Nested TLB, Page Not Present, NaT Page
 * Consumption and Key Miss faults exactly like a normal reference;
 * permission failures and unimplemented addresses return 0; maintenance
 * conditions (access/dirty bit) do not gate the granted access.
 */
static uint64_t ia64_probe_grant(CPUIA64State *env, uint64_t va,
                                 uint32_t is_write, IA64Exception excp)
{
    switch (excp) {
    case IA64_EXCP_NONE:
    case IA64_EXCP_DATA_DIRTY:
    case IA64_EXCP_DATA_ACCESS_BIT:
        return 1;
    case IA64_EXCP_UNIMPL_DATA_ADDR:
    case IA64_EXCP_DATA_ACCESS:
    case IA64_EXCP_KEY_PERMISSION:
        return 0;
    default:
        ia64_raise_data_reference_exception(
            env, va, is_write, false, true, 2, excp, false,
            ia64_code_tlb_ed(env));
        g_assert_not_reached();
    }
}

static uint64_t ia64_probe_dt_disabled(CPUIA64State *env, uint64_t va,
                                       uint32_t is_write,
                                       uint8_t access_level)
{
    uint64_t pa;
    uint8_t perm;
    uint8_t needed = is_write ? IA64_TLB_W : IA64_TLB_R;
    uint32_t rid = ia64_region_rid(env, va);
    const IA64TlbEntry *entry;
    IA64Exception excp;

    if (!ia64_va_is_implemented(env, va)) {
        return 0;
    }

    entry = ia64_tlb_find_cached(env, va, rid, false);
    if (!entry) {
        /*
         * A data-translation miss taken with PSR.ic clear reports the Data
         * Nested TLB vector in place of the Alternate Data TLB vector, the
         * same conversion the ordinary fill path performs.
         */
        IA64Exception miss_excp = ia64_data_nested_tlb_active(env) ?
                                  IA64_EXCP_DATA_NESTED_TLB :
                                  IA64_EXCP_ALT_DTLB;

        ia64_raise_data_reference_exception(
            env, va, is_write, false, true, 2, miss_excp, false,
            ia64_code_tlb_ed(env));
        g_assert_not_reached();
    }

    /*
     * The dt=0 probe queries the DTLB with a virtual address, so the
     * checked conditions keep their architected order: present, NaTPage,
     * protection key (gated by PSR.pk alone), then access rights.
     */
    ia64_tlb_entry_translate(entry, va, access_level, &pa, &perm);
    if (!(entry->pte & IA64_PTE_PRESENT)) {
        excp = IA64_EXCP_PAGE_NOT_PRESENT;
    } else if (ia64_pte_ma(entry->pte) == IA64_PTE_MA_NATPAGE) {
        excp = IA64_EXCP_NAT_CONSUMPTION;
    } else {
        excp = IA64_EXCP_NONE;
        if (env->psr & IA64_PSR_PK) {
            excp = ia64_key_exception_for_key(env, entry->key, needed,
                                              false);
        }
        if (excp == IA64_EXCP_NONE) {
            excp = ia64_pte_exception_for_access(entry->pte, perm, needed,
                                                 false, is_write, env->psr);
        }
    }
    return ia64_probe_grant(env, va, is_write, excp);
}

/*
 * Only the non-faulting probe.r/probe.w forms reach here; probe.rw is
 * encoded solely as probe.rw.fault and goes through ia64_mmu_probe_fault().
 */
uint64_t ia64_mmu_probe(CPUIA64State *env, uint64_t va, uint32_t is_write,
                      uint64_t access_level)
{
    uint8_t effective_pl = ia64_probe_access_level(env, access_level);
    IA64Exception excp;

    if (!(env->psr & IA64_PSR_DT)) {
        return ia64_probe_dt_disabled(env, va, is_write, effective_pl);
    }

    excp = ia64_data_reference_exception(env, va, is_write, false,
                                         effective_pl, true, NULL);
    return ia64_probe_grant(env, va, is_write, excp);
}

static void ia64_raise_data_reference_fault_if_needed(CPUIA64State *env,
                                                      uint64_t va,
                                                      uint32_t is_write,
                                                      uint32_t is_rw,
                                                      uint8_t access_level,
                                                      bool is_non_access,
                                                      uint8_t non_access_code)
{
    IA64Exception excp = ia64_data_reference_exception(
        env, va, is_write, is_rw, access_level, true, NULL);

    if (excp == IA64_EXCP_NONE) {
        return;
    }
    ia64_raise_data_reference_exception(env, va, is_write, is_rw,
                                        is_non_access, non_access_code,
                                        excp, false,
                                        ia64_code_tlb_ed(env));
}

void ia64_raise_pre_unaligned_data_fault(CPUIA64State *env,
                                                uint64_t va,
                                                uint32_t is_write,
                                                uint32_t is_rw,
                                                uint64_t fault_ip,
                                                uint8_t fault_slot)
{
    IA64Exception excp = ia64_data_reference_exception(
        env, va, is_write, is_rw, ia64_psr_cpl(env->psr), true, NULL);

    if (excp == IA64_EXCP_NONE) {
        return;
    }
    ia64_raise_data_reference_exception_at(
        env, va, is_write, is_rw, false, 0, excp, false,
        ia64_code_tlb_ed(env), fault_ip, fault_slot);
}

void ia64_mmu_probe_fault(CPUIA64State *env, uint64_t va, uint32_t is_write,
                        uint32_t is_rw, uint64_t access_level)
{
    uint8_t effective_pl = ia64_probe_access_level(env, access_level);

    ia64_raise_data_reference_fault_if_needed(env, va, is_write, is_rw,
                                              effective_pl, true, 5);
}

void ia64_mmu_lfetch_fault(CPUIA64State *env, uint64_t va,
                         uint64_t fault_ip, uint32_t fault_slot)
{
    IA64Exception excp = ia64_data_reference_exception(
        env, va, false, false, ia64_psr_cpl(env->psr), true, NULL);

    if (excp == IA64_EXCP_NONE) {
        return;
    }
    ia64_raise_data_reference_exception_at(
        env, va, false, false, true, 4, excp, false,
        ia64_code_tlb_ed(env), fault_ip, fault_slot);
}

void ia64_mmu_check_semaphore_access(CPUIA64State *env, uint64_t va)
{
    IA64DataReferenceResult translation = { 0 };
    IA64Exception excp = ia64_data_reference_exception(
        env, va, true, true, ia64_psr_cpl(env->psr), true, &translation);

    /* A NaTPage translation already reports NaT Page Consumption here. */
    if (excp != IA64_EXCP_NONE) {
        ia64_raise_data_reference_exception(
            env, va, true, true, false, 0, excp, false,
            ia64_code_tlb_ed(env));
    }
    if (translation.memory_attribute != IA64_PTE_MA_WB) {
        ia64_raise_data_reference_exception(
            env, va, true, true, false, 0,
            IA64_EXCP_UNSUPPORTED_DATA_REFERENCE, false,
            ia64_code_tlb_ed(env));
    }
}

void ia64_mmu_check_montecito_16byte_access(CPUIA64State *env, uint64_t va,
                                          uint32_t is_write)
{
    IA64DataReferenceResult translation = { 0 };
    IA64Exception excp;

    if (!ia64_env_cpu_class(env)->is_montecito) {
        return;
    }

    excp = ia64_data_reference_exception(
        env, va, is_write, false, ia64_psr_cpl(env->psr), true,
        &translation);
    /* A NaTPage translation already reports NaT Page Consumption here. */
    if (excp != IA64_EXCP_NONE) {
        ia64_raise_data_reference_exception(
            env, va, is_write, false, false, 0, excp, false,
            ia64_code_tlb_ed(env));
    }
    if (translation.memory_attribute != IA64_PTE_MA_WB) {
        ia64_raise_data_reference_exception(
            env, va, is_write, false, false, 0,
            IA64_EXCP_UNSUPPORTED_DATA_REFERENCE, false,
            ia64_code_tlb_ed(env));
    }
}

uint64_t ia64_mmu_speculative_probe(CPUIA64State *env, uint64_t va,
                                  uint32_t is_write, uint32_t is_ifetch,
                                  uint32_t size)
{
    bool alignment_fault;
    bool itlb_ed;
    bool itlb_ed_known;
    IA64Exception excp;
    IA64DataReferenceResult translation;

    if (env->psr & IA64_PSR_ED) {
        return 0;
    }

    itlb_ed = ia64_code_tlb_ed_lookup(env, &itlb_ed_known);
    alignment_fault = ia64_speculative_alignment_fault(env, va, size);
    if (is_ifetch) {
        if (alignment_fault) {
            excp = IA64_EXCP_UNALIGNED;
            goto qualify;
        }
        return ia64_probe_address(env, va, is_write, is_ifetch,
                                  ia64_psr_cpl(env->psr));
    } else {
        /* PSR.ic controls fault collection, not data VHPT walking. */
        excp = ia64_data_reference_exception(
            env, va, is_write, false, ia64_psr_cpl(env->psr),
            true, &translation);
    }

qualify:
    /*
     * An unimplemented address precludes an unaligned-reference condition.
     * Other data-reference conditions retain their architectural priority;
     * only a condition that is itself deferred permits a lower-priority
     * unaligned condition to be considered.
     */
    if (excp == IA64_EXCP_UNIMPL_DATA_ADDR) {
        alignment_fault = false;
    }
    if (excp != IA64_EXCP_NONE &&
        !ia64_speculative_exception_deferrable(env, excp, itlb_ed,
                                               !itlb_ed_known)) {
        ia64_raise_data_reference_exception(
            env, va, is_write, false, false, 0, excp, true, itlb_ed);
    }
    if (alignment_fault &&
        !ia64_speculative_exception_deferrable(
            env, IA64_EXCP_UNALIGNED, itlb_ed, !itlb_ed_known)) {
        ia64_raise_data_reference_exception(
            env, va, is_write, false, false, 0, IA64_EXCP_UNALIGNED, true,
            itlb_ed);
    }
    if (excp != IA64_EXCP_NONE || alignment_fault) {
        return 0;
    }

    if (!is_ifetch && translation.valid &&
        !ia64_memory_allows_control_speculation(translation.speculation)) {
        return 0;
    }
    return 1;
}

uint64_t ia64_mmu_advanced_load_allowed(CPUIA64State *env, uint64_t va)
{
    int mmu_idx = env->psr & IA64_PSR_DT ?
                  MMU_IDX_VIRT_CPL(ia64_psr_cpl(env->psr)) : MMU_PHYS_IDX;

    return ia64_exec_advanced_load_allowed(env, va, mmu_idx);
}

uint64_t ia64_mmu_tak(CPUIA64State *env, uint64_t va)
{
    uint32_t rid;
    const IA64TlbEntry *entry;
    uint64_t pa;
    uint8_t perm;
    uint64_t pte = 0;

    /*
     * An unimplemented virtual address returns the architected miss value; a
     * TLB/VHPT lookup could otherwise return a spurious short-format alias.
     */
    if (!ia64_va_is_implemented(env, va)) {
        return 1;
    }

    rid = ia64_region_rid(env, va);
    entry = ia64_tlb_find_cached(env, va, rid, false);
    if (entry && ia64_tlb_entry_present(entry)) {
        return entry->key;
    }

    if ((env->psr & IA64_PSR_DT) &&
        ia64_vhpt_walk_full(env, va, rid, false, false,
                            ia64_psr_cpl(env->psr), &pa, &perm, &pte, NULL,
                            &entry) &&
        (pte & IA64_PTE_PRESENT)) {
        if (entry && ia64_tlb_entry_present(entry)) {
            return entry->key;
        }
    }

    return 1;
}

uint64_t ia64_mmu_thash(CPUIA64State *env, uint64_t va)
{
    return ia64_vhpt_hash_address(env, va);
}

static uint64_t ia64_implemented_va_payload(const CPUIA64State *env,
                                            uint64_t va)
{
    return va & ((1ULL << (env->impl_va_msb + 1)) - 1);
}

static uint8_t ia64_region_preferred_ps(CPUIA64State *env, uint64_t va)
{
    uint8_t rr_ps = (env->rr[ia64_rr_index(va)] >> IA64_ITIR_PS_SHIFT) &
                    IA64_ITIR_PS_MASK;

    return rr_ps < 12 ? 12 : rr_ps;
}

static bool ia64_vhpt_preferred_page_size_supported(CPUIA64State *env,
                                                    uint64_t va)
{
    uint8_t rr_ps = (env->rr[ia64_rr_index(va)] >> IA64_ITIR_PS_SHIFT) &
                    IA64_ITIR_PS_MASK;

    return ia64_page_shift_insertable(env, rr_ps);
}

static uint64_t ia64_vhpt_hpn(CPUIA64State *env, uint64_t va)
{
    return ia64_implemented_va_payload(env, va) >>
           ia64_region_preferred_ps(env, va);
}

/*
 * Long-format VHPT tag.  The function is implementation-specific; only the
 * original Itanium's is published, in 245320-002 sec 5.5:
 *
 *   GR[r1] = (VA{50:0} >> RR.ps) ^ ((zero{5:0} || RR.RID{17:0}) << 39)
 *
 * Nothing equivalent is published for Itanium 2, so those models keep the
 * function this fork has always used.
 */
#define IA64_MERCED_VHPT_TAG_RID_SHIFT 39

static uint64_t ia64_vhpt_long_tag(CPUIA64State *env, uint64_t va)
{
    uint8_t rr_ps = ia64_region_preferred_ps(env, va);
    uint8_t hpn_bits;
    uint64_t hpn = ia64_vhpt_hpn(env, va);
    uint64_t rid = ia64_region_rid(env, va);

    if (!ia64_env_cpu_class(env)->vhpt_hash_folds_hpn) {
        return hpn ^ (rid << IA64_MERCED_VHPT_TAG_RID_SHIFT);
    }

    hpn_bits = rr_ps > env->impl_va_msb ? 0 : env->impl_va_msb + 1 - rr_ps;
    if (hpn_bits == 0) {
        return rid;
    }
    return (rid << hpn_bits) | (hpn & ((1ULL << hpn_bits) - 1));
}

static uint64_t ia64_vhpt_short_hash_address(CPUIA64State *env, uint64_t va,
                                             uint8_t size)
{
    uint64_t region = va & (IA64_REGION_MASK << IA64_REGION_SHIFT);
    uint64_t offset;
    uint64_t mask;
    uint64_t base;

    offset = ia64_vhpt_hpn(env, va) << 3;
    mask = (1ULL << size) - 1;
    base = env->cr_pta & (((1ULL << IA64_REGION_SHIFT) - 1) & ~0x7fffULL);
    return region | ((base & ~mask) | (offset & mask));
}

static uint64_t ia64_vhpt_long_hash_address(CPUIA64State *env, uint64_t va,
                                            uint8_t size, uint64_t *hash_out)
{
    uint64_t base = env->cr_pta & IA64_PTA_BASE_MASK;
    uint64_t entries = 1ULL << (size - 5);
    uint64_t hpn = ia64_vhpt_hpn(env, va);
    /*
     * 245320-002 sec 5.4 gives the original Itanium's hash as
     * Hash_Index = HPN ^ (zero{63:18} || rid{17:0}), with no folded copy of
     * HPN.  Itanium 2's function is not published, so those models keep the
     * extra term this fork has always applied.
     */
    uint64_t folded = ia64_env_cpu_class(env)->vhpt_hash_folds_hpn ?
                      hpn >> 7 : 0;
    uint64_t hash = (hpn ^ folded ^ ia64_region_rid(env, va)) &
                    (entries - 1);
    uint64_t offset = hash << 5;
    uint64_t mask = (1ULL << size) - 1;

    if (hash_out) {
        *hash_out = hash;
    }
    return (base & ~mask) | (offset & mask);
}

uint64_t ia64_vhpt_hash_address(CPUIA64State *env, uint64_t va)
{
    uint8_t size;
    bool long_format;

    if (!ia64_vhpt_config_valid(env, &size, &long_format)) {
        return va;
    }

    if (!long_format) {
        return ia64_vhpt_short_hash_address(env, va, size);
    }

    return ia64_vhpt_long_hash_address(env, va, size, NULL);
}

uint64_t ia64_mmu_ttag(CPUIA64State *env, uint64_t va)
{
    return ia64_vhpt_long_tag(env, va);
}

typedef enum IA64VhptEntryStatus {
    IA64_VHPT_ENTRY_TRANSLATED,
    IA64_VHPT_ENTRY_TLB_MISS,
    IA64_VHPT_ENTRY_ABORT,
} IA64VhptEntryStatus;

static IA64VhptEntryStatus ia64_vhpt_entry_phys(CPUIA64State *env,
                                                uint64_t entry_va,
                                                uint64_t *entry_pa)
{
    const IA64TlbEntry *entry;
    uint8_t perm;
    uint32_t rid;

    if (ia64_firmware_identity_pa(env->fw_image_base, env->cr_iva, env->ip, env->psr,
                                  entry_va, entry_pa)) {
        return IA64_VHPT_ENTRY_TRANSLATED;
    }

    rid = ia64_region_rid(env, entry_va);
    /*
     * VHPT walker references to the VHPT itself are performed at
     * privilege level 0 regardless of PSR.cpl.
     */
    entry = ia64_tlb_find_cached(env, entry_va, rid, false);
    if (entry && entry->pending_purge) {
        /*
         * A purge may complete before its required serialization point.
         * Do not use a pending translation to read the VHPT: a PTE fetched
         * through it could install a new, non-pending TC entry which survives
         * completion of the original purge.
         */
        qemu_log_mask(CPU_LOG_MMU,
                      "ia64 vhpt entry pending purge va=0x%016" PRIx64
                      " rid=0x%06" PRIx32 "\n",
                      entry_va, rid);
        return IA64_VHPT_ENTRY_TLB_MISS;
    }
    if (entry) {
        IA64Exception excp;
        uint8_t ma = (entry->pte & IA64_PTE_MA_MASK) >> IA64_PTE_MA_SHIFT;

        ia64_tlb_entry_translate(entry, entry_va, 0, entry_pa, &perm);
        excp = ia64_tlb_exception_for_access(env, entry, perm, IA64_TLB_R,
                                             false, false, false);
        if (excp == IA64_EXCP_NONE && ma == 0) {
            return IA64_VHPT_ENTRY_TRANSLATED;
        }
        qemu_log_mask(CPU_LOG_MMU,
                      "ia64 vhpt entry abort va=0x%016" PRIx64
                      " rid=0x%06" PRIx32
                      " pte=0x%016" PRIx64 " ma=%u excp=%u\n",
                      entry_va, rid, entry->pte, ma, excp);
        return IA64_VHPT_ENTRY_ABORT;
    }

    return IA64_VHPT_ENTRY_TLB_MISS;
}

bool ia64_vhpt_entry_accessible(CPUIA64State *env, uint64_t va,
                                bool is_ifetch, bool is_rse,
                                uint64_t *entry_va)
{
    uint64_t entry_pa;
    bool long_format;
    uint8_t size;

    if (!ia64_vhpt_walker_enabled(env, va, is_ifetch, is_rse,
                                  &size, &long_format)) {
        return true;
    }
    if (!ia64_vhpt_preferred_page_size_supported(env, va)) {
        return true;
    }
    *entry_va = long_format ? ia64_vhpt_long_hash_address(env, va, size,
                                                          NULL) :
                ia64_vhpt_short_hash_address(env, va, size);
    /*
     * A present-but-faulting translation for the VHPT entry makes the walker
     * abort to the original TLB miss.  Only a missing DTLB translation for
     * the VHPT entry raises a VHPT Translation fault.
     */
    return ia64_vhpt_entry_phys(env, *entry_va, &entry_pa) !=
           IA64_VHPT_ENTRY_TLB_MISS;
}

static uint64_t ia64_vhpt_load_u64(CPUIA64State *env, uint64_t pa)
{
    uint8_t buf[8];

    (void)ia64_exec_physical_rw(pa, buf, sizeof(buf), false);
    return env->cr_dcr & IA64_DCR_BE ? ldq_be_p(buf) : ldq_le_p(buf);
}

static void ia64_vhpt_load_long_entry(CPUIA64State *env, uint64_t pa,
                                      uint64_t *pte, uint64_t *itir,
                                      uint64_t *tag)
{
    *pte = ia64_vhpt_load_u64(env, pa);
    *itir = ia64_vhpt_load_u64(env, pa + 8);
    *tag = ia64_vhpt_load_u64(env, pa + 16);
}

static bool ia64_vhpt_pte_valid(uint64_t pte)
{
    uint8_t ma;

    if (!(pte & IA64_PTE_PRESENT)) {
        return true;
    }
    ma = (pte & IA64_PTE_MA_MASK) >> IA64_PTE_MA_SHIFT;
    return !(pte & IA64_PTE_RESERVED_MASK) && (ma == 0 || ma >= 4);
}

static bool ia64_vhpt_itir_valid(const CPUIA64State *env,
                                 uint64_t pte, uint64_t itir)
{
    uint8_t page_shift = (itir >> IA64_ITIR_PS_SHIFT) & IA64_ITIR_PS_MASK;
    uint64_t reserved_mask = IA64_ITIR_RESERVED_MASK;

    if (!(pte & IA64_PTE_PRESENT)) {
        reserved_mask &= 3;
    }
    return !(itir & reserved_mask) &&
           ia64_page_shift_insertable(env, page_shift);
}

static bool ia64_vhpt_lookup_pte(CPUIA64State *env, uint64_t va,
                                 bool is_ifetch, bool is_rse, uint64_t *pte,
                                 uint64_t *entry_va, uint8_t *page_shift)
{
    uint8_t size;
    bool long_format;
    uint64_t entry_pa;

    if (!ia64_vhpt_walker_enabled(env, va, is_ifetch, is_rse,
                                  &size, &long_format)) {
        return false;
    }
    if (!ia64_vhpt_preferred_page_size_supported(env, va)) {
        return false;
    }

    if (!long_format) {
        *entry_va = ia64_vhpt_short_hash_address(env, va, size);
        if (ia64_vhpt_entry_phys(env, *entry_va, &entry_pa) !=
            IA64_VHPT_ENTRY_TRANSLATED) {
            return false;
        }
        *pte = ia64_vhpt_load_u64(env, entry_pa);
        if (page_shift) {
            *page_shift = ia64_region_preferred_ps(env, va);
        }
        return ia64_vhpt_pte_valid(*pte);
    }

    {
        uint64_t expected_tag = ia64_vhpt_long_tag(env, va);
        uint64_t itir;
        uint64_t tag;

        *entry_va = ia64_vhpt_long_hash_address(env, va, size, NULL);
        if (ia64_vhpt_entry_phys(env, *entry_va, &entry_pa) !=
            IA64_VHPT_ENTRY_TRANSLATED) {
            return false;
        }
        ia64_vhpt_load_long_entry(env, entry_pa, pte, &itir, &tag);
        if ((tag & (1ULL << 63)) || tag != expected_tag) {
            return false;
        }
        if (page_shift) {
            *page_shift = (itir >> IA64_ITIR_PS_SHIFT) & IA64_ITIR_PS_MASK;
        }
        return ia64_vhpt_pte_valid(*pte) &&
               ia64_vhpt_itir_valid(env, *pte, itir);
    }
}

bool ia64_vhpt_pte_not_present(CPUIA64State *env, uint64_t va,
                               bool is_ifetch, bool is_rse,
                               uint64_t *entry_va)
{
    uint64_t local_entry_va;
    uint64_t pte;

    if (!entry_va) {
        entry_va = &local_entry_va;
    }

    return ia64_vhpt_lookup_pte(env, va, is_ifetch, is_rse,
                                &pte, entry_va, NULL) &&
           !(pte & IA64_PTE_PRESENT);
}

/*
 * Side-effect-free translation for the monitor, the gdbstub and guest-memory
 * dumps.  Consults, in order: physical addressing, the firmware/SAL boot
 * identity windows, the data then instruction TLBs, and finally the VHPT
 * through the no-insert lookup (nothing is installed into the TC and no
 * fault is raised).  Returns false when no source can translate: reporting
 * failure is strictly better for a debugger than the old fallback of
 * handing back the virtual address, which silently read whatever physical
 * page happened to share the low bits.
 */
bool ia64_mmu_translate_debug(CPUIA64State *env, uint64_t va, uint64_t *pa)
{
    const IA64TlbEntry *entry;
    uint64_t pte;
    uint64_t entry_va;
    uint8_t page_shift;
    uint8_t perm;
    uint32_t rid;

    if (!(env->psr & IA64_PSR_IT)) {
        *pa = va;
        return true;
    }

    if (ia64_firmware_identity_pa(env->fw_image_base, env->cr_iva, va, env->psr, va, pa)) {
        return true;
    }
    if (ia64_sal_boot_virtual_pa(env, va, pa)) {
        return true;
    }

    rid = ia64_region_rid(env, va);
    entry = ia64_tlb_find_cached(env, va, rid, false);
    if (!entry) {
        entry = ia64_tlb_find_cached(env, va, rid, true);
    }
    if (entry) {
        ia64_tlb_entry_translate(entry, va, ia64_psr_cpl(env->psr),
                                 pa, &perm);
        return true;
    }

    if ((ia64_vhpt_lookup_pte(env, va, false, false, &pte, &entry_va,
                              &page_shift) ||
         ia64_vhpt_lookup_pte(env, va, true, false, &pte, &entry_va,
                              &page_shift)) &&
        (pte & IA64_PTE_PRESENT)) {
        uint64_t page_mask = (1ULL << page_shift) - 1;

        *pa = ((pte & ia64_pte_ppn_mask(env)) & ~page_mask) |
              (va & page_mask);
        return true;
    }

    return ia64_sal_boot_identity_pa(env, va, pa);
}

static IA64TlbEntry *
ia64_vhpt_install_tc(CPUIA64State *env, uint64_t va, uint32_t rid,
                     bool is_ifetch, uint64_t pa, uint64_t page_size,
                     uint8_t ar, uint8_t pl, uint8_t perm, uint32_t key,
                     uint64_t pte)
{
    IA64TlbEntry *tlb = is_ifetch ? env->mmu.tlb_inst : env->mmu.tlb_data;
    uint16_t *cnt = is_ifetch ?
        &env->mmu.tlb_inst_count : &env->mmu.tlb_data_count;
    uint16_t *next_replace = is_ifetch ? &env->mmu.tlb_inst_replace :
                                         &env->mmu.tlb_data_replace;
    uint16_t *pending_count = is_ifetch ?
        &env->mmu.pending_purge_inst_count : &env->mmu.pending_purge_data_count;
    uint64_t base_va = va & ~(page_size - 1);
    uint64_t base_pa = pa & ~(page_size - 1);
    int slot;

    ia64_purge_tc_entries(env, tlb, cnt, pending_count, base_va, page_size,
                          rid, !is_ifetch, next_replace, &slot);
    if (slot < 0) {
        return NULL;
    }

    ia64_qemu_tlb_flush_entry(env, &tlb[slot], !is_ifetch);
    ia64_discard_pending_purge(&tlb[slot], pending_count);
    tlb[slot].va = base_va;
    tlb[slot].pa = base_pa;
    tlb[slot].ps = page_size;
    tlb[slot].page_mask = ia64_va_vpn_mask(page_size);
    tlb[slot].pte = pte;
    tlb[slot].perm = perm;
    tlb[slot].ar = ar;
    tlb[slot].pl = pl;
    tlb[slot].valid = 1;
    tlb[slot].is_tr = 0;
    tlb[slot].pending_purge = 0;
    tlb[slot].rid = rid;
    tlb[slot].key = key;
    tlb[slot].slot = slot;
    if (slot >= *cnt) {
        *cnt = slot + 1;
    }
    ia64_tlb_bump_slot_generation(env, is_ifetch, slot);
    ia64_assert_pending_purge_counts(env);
    qemu_log_mask(CPU_LOG_MMU,
                  "ia64 vhpt install tc.%c slot=%d va=0x%016" PRIx64
                  " rid=0x%06" PRIx32 " pa=0x%016" PRIx64
                  " ps=0x%016" PRIx64 " perm=0x%x key=0x%x"
                  " pte=0x%016" PRIx64 "\n",
                  is_ifetch ? 'i' : 'd', slot, base_va, rid, base_pa,
                  page_size, perm, key, pte);
    /*
     * The QEMU softmmu TLB is indexed by guest virtual address and mmu_idx;
     * it does not carry IA-64 region IDs.  A VHPT walk can install a TC entry
     * for a different RID than a cached same-VA host entry, so discard the
     * host translation range covered by the installed TC.
     */
    ia64_qemu_tlb_flush_entry(env, &tlb[slot], !is_ifetch);
    return &tlb[slot];
}

/* ---- VHPT walker ---- */

bool ia64_vhpt_walk_full(CPUIA64State *env, uint64_t va, uint32_t rid,
                         bool is_ifetch, bool is_rse, uint8_t access_level,
                         uint64_t *pa, uint8_t *perm, uint64_t *pte,
                         uint32_t *access_key,
                         const IA64TlbEntry **installed_entry)
{
    uint64_t vhpt_base;
    uint64_t hash;
    uint64_t tag;
    uint64_t expected_tag;
    uint64_t translation;
    uint64_t itir;
    uint64_t entry_pa;
    uint64_t entry_va;
    uint8_t page_shift;
    uint8_t size;
    bool long_format;

    if (installed_entry) {
        *installed_entry = NULL;
    }

    if (!ia64_vhpt_walker_enabled(env, va, is_ifetch, is_rse,
                                  &size, &long_format)) {
        qemu_log_mask(CPU_LOG_MMU,
                      "ia64 vhpt disabled %c va=0x%016" PRIx64
                      " rid=0x%06" PRIx32 " pta=0x%016" PRIx64
                      " rr=0x%016" PRIx64 " psr=0x%016" PRIx64 "\n",
                      is_ifetch ? 'i' : 'd', va, rid, env->cr_pta,
                      env->rr[ia64_rr_index(va)], env->psr);
        return false;
    }
    if (!ia64_vhpt_preferred_page_size_supported(env, va)) {
        qemu_log_mask(CPU_LOG_MMU,
                      "ia64 vhpt unsupported preferred page size %c"
                      " va=0x%016" PRIx64 " rid=0x%06" PRIx32 "\n",
                      is_ifetch ? 'i' : 'd', va, rid);
        return false;
    }

    if (!long_format) {
        uint64_t page_mask;

        entry_va = ia64_vhpt_short_hash_address(env, va, size);
        page_shift = ia64_region_preferred_ps(env, va);
        if (ia64_vhpt_entry_phys(env, entry_va, &entry_pa) !=
            IA64_VHPT_ENTRY_TRANSLATED) {
            qemu_log_mask(CPU_LOG_MMU,
                          "ia64 vhpt short entry miss %c va=0x%016" PRIx64
                          " rid=0x%06" PRIx32
                          " entry_va=0x%016" PRIx64 "\n",
                          is_ifetch ? 'i' : 'd', va, rid, entry_va);
            return false;
        }

        translation = ia64_vhpt_load_u64(env, entry_pa);
        if (!ia64_vhpt_pte_valid(translation)) {
            qemu_log_mask(CPU_LOG_MMU,
                          "ia64 vhpt short reserved translation %c"
                          " va=0x%016" PRIx64 " rid=0x%06" PRIx32
                          " pte=0x%016" PRIx64 "\n",
                          is_ifetch ? 'i' : 'd', va, rid, translation);
            return false;
        }
        if (pte) {
            *pte = translation;
        }
        if (access_key) {
            *access_key = rid;
        }
        {
            uint8_t ar = ia64_pte_ar(translation);
            uint8_t pl = ia64_pte_pl(translation);

            *perm = ia64_pte_perm(translation, access_level);
            page_mask = (1ULL << page_shift) - 1;
            *pa = ((translation & ia64_pte_ppn_mask(env)) & ~page_mask) |
                  (va & page_mask);
            IA64TlbEntry *entry = ia64_vhpt_install_tc(
                env, va, rid, is_ifetch, *pa, 1ULL << page_shift, ar, pl,
                *perm, rid, translation);

            if (installed_entry) {
                *installed_entry = entry;
            }
        }
        if (!(translation & IA64_PTE_PRESENT)) {
            qemu_log_mask(CPU_LOG_MMU,
                          "ia64 vhpt short not-present %c va=0x%016" PRIx64
                          " rid=0x%06" PRIx32
                          " entry_va=0x%016" PRIx64
                          " entry_pa=0x%016" PRIx64
                          " pte=0x%016" PRIx64 "\n",
                          is_ifetch ? 'i' : 'd', va, rid, entry_va, entry_pa,
                          translation);
            return true;
        }
        if (*perm == 0) {
            qemu_log_mask(CPU_LOG_MMU,
                          "ia64 vhpt short access denied %c va=0x%016" PRIx64
                          " rid=0x%06" PRIx32
                          " entry_va=0x%016" PRIx64
                          " entry_pa=0x%016" PRIx64
                          " pte=0x%016" PRIx64 "\n",
                          is_ifetch ? 'i' : 'd', va, rid, entry_va,
                          entry_pa, translation);
            return true;
        }

        qemu_log_mask(CPU_LOG_MMU,
                      "ia64 vhpt short walk %c va=0x%016" PRIx64
                      " rid=0x%06" PRIx32
                      " entry_va=0x%016" PRIx64
                      " entry_pa=0x%016" PRIx64
                      " pte=0x%016" PRIx64
                      " pa=0x%016" PRIx64 " perm=0x%x\n",
                      is_ifetch ? 'i' : 'd', va, rid, entry_va, entry_pa,
                      translation, *pa, *perm);
        return true;
    }

    vhpt_base = env->cr_pta & IA64_PTA_BASE_MASK;
    expected_tag = ia64_vhpt_long_tag(env, va);
    {
        entry_va = ia64_vhpt_long_hash_address(env, va, size, &hash);
        if (ia64_vhpt_entry_phys(env, entry_va, &entry_pa) !=
            IA64_VHPT_ENTRY_TRANSLATED) {
            qemu_log_mask(CPU_LOG_MMU,
                          "ia64 vhpt long entry miss %c va=0x%016" PRIx64
                          " rid=0x%06" PRIx32
                          " entry_va=0x%016" PRIx64 "\n",
                          is_ifetch ? 'i' : 'd', va, rid, entry_va);
            return false;
        }

        ia64_vhpt_load_long_entry(env, entry_pa, &translation, &itir, &tag);
        if (tag & (1ULL << 63)) {
            goto long_miss;
        }
        if (tag != expected_tag) {
            goto long_miss;
        }
        if (!ia64_vhpt_pte_valid(translation) ||
            !ia64_vhpt_itir_valid(env, translation, itir)) {
            qemu_log_mask(CPU_LOG_MMU,
                          "ia64 vhpt reserved translation %c"
                          " va=0x%016" PRIx64 " rid=0x%06" PRIx32
                          " pte=0x%016" PRIx64 " itir=0x%016" PRIx64 "\n",
                          is_ifetch ? 'i' : 'd', va, rid, translation, itir);
            return false;
        }
        if (pte) {
            *pte = translation;
        }

        {
            uint64_t page_mask;
            uint8_t long_page_shift =
                (itir >> IA64_ITIR_PS_SHIFT) & IA64_ITIR_PS_MASK;

            page_mask = (1ULL << long_page_shift) - 1;
            {
                uint8_t ar = ia64_pte_ar(translation);
                uint8_t pl = ia64_pte_pl(translation);
                uint32_t entry_key = (itir & IA64_ITIR_KEY_MASK) >>
                                     IA64_ITIR_KEY_SHIFT;

                if (access_key) {
                    *access_key = entry_key;
                }
                *perm = ia64_pte_perm(translation, access_level);
                *pa = ((translation & ia64_pte_ppn_mask(env)) & ~page_mask) |
                      (va & page_mask);
                IA64TlbEntry *entry = ia64_vhpt_install_tc(
                    env, va, rid, is_ifetch, *pa,
                    1ULL << long_page_shift, ar, pl, *perm, entry_key,
                    translation);

                if (installed_entry) {
                    *installed_entry = entry;
                }
            }
            if (!(translation & IA64_PTE_PRESENT)) {
                qemu_log_mask(CPU_LOG_MMU,
                              "ia64 vhpt not-present %c va=0x%016" PRIx64
                              " rid=0x%06" PRIx32
                              " entry_va=0x%016" PRIx64
                              " entry_pa=0x%016" PRIx64
                              " tag=0x%016" PRIx64
                              " pte=0x%016" PRIx64 "\n",
                              is_ifetch ? 'i' : 'd', va, rid, entry_va,
                              entry_pa, tag, translation);
                return true;
            }
            if (*perm == 0) {
                qemu_log_mask(CPU_LOG_MMU,
                              "ia64 vhpt access denied %c va=0x%016" PRIx64
                              " rid=0x%06" PRIx32
                              " entry_va=0x%016" PRIx64
                              " entry_pa=0x%016" PRIx64
                              " tag=0x%016" PRIx64
                              " pte=0x%016" PRIx64 "\n",
                              is_ifetch ? 'i' : 'd', va, rid, entry_va,
                              entry_pa, tag, translation);
                return true;
            }

            qemu_log_mask(CPU_LOG_MMU,
                          "ia64 vhpt walk %c va=0x%016" PRIx64
                          " rid=0x%06" PRIx32
                          " entry_va=0x%016" PRIx64
                          " entry_pa=0x%016" PRIx64
                          " tag=0x%016" PRIx64 " pte=0x%016" PRIx64
                          " pa=0x%016" PRIx64 " perm=0x%x\n",
                          is_ifetch ? 'i' : 'd', va, rid, entry_va, entry_pa,
                          tag, translation, *pa, *perm);
            return true;
        }
    }
long_miss:
    qemu_log_mask(CPU_LOG_MMU,
                  "ia64 vhpt miss %c va=0x%016" PRIx64
                  " rid=0x%06" PRIx32 " base=0x%016" PRIx64
                  " hash=0x%016" PRIx64 "\n",
                  is_ifetch ? 'i' : 'd', va, rid, vhpt_base, hash);
    return false;
}

bool ia64_vhpt_walk(CPUIA64State *env, uint64_t va, uint32_t rid,
                    bool is_ifetch, bool is_rse, uint8_t access_level,
                    uint64_t *pa, uint8_t *perm)
{
    return ia64_vhpt_walk_full(env, va, rid, is_ifetch, is_rse, access_level,
                               pa, perm, NULL, NULL, NULL);
}

/* ---- ITC insert helper (software-managed TLB insert) ---- */

void ia64_mmu_itc_insert(CPUIA64State *env, uint64_t pte, uint32_t is_data,
                       uint64_t raw, uint32_t fault_slot)
{
    IA64TlbEntry *tlb;
    uint16_t *cnt;
    uint16_t *next_replace;
    uint16_t *pending_count;
    uint64_t ps = ia64_itir_page_size(env);
    uint64_t va = env->cr_ifa & ~(ps - 1);
    uint64_t pa = (pte & ia64_pte_ppn_mask(env)) & ~(ps - 1);
    uint32_t key = (env->cr_itir & IA64_ITIR_KEY_MASK) >>
                   IA64_ITIR_KEY_SHIFT;
    uint32_t rid = ia64_region_rid(env, env->cr_ifa);
    uint8_t ar = ia64_pte_ar(pte);
    uint8_t pl = ia64_pte_pl(pte);
    uint8_t perm = ia64_pte_perm(pte, 0);
    int slot;
    bool purged;

    if (!ia64_translation_insert_fields_valid(env, pte, env->cr_itir)) {
        env->cr_isr = 0x30;
        ia64_raise_exception(env, IA64_EXCP_RESERVED_REG_FIELD,
                               ia64_ip_bundle_addr(env->ip), raw,
                               fault_slot);
        return;
    }

    if (!ia64_va_is_implemented(env, env->cr_ifa)) {
        ia64_raise_unimplemented_data_address(
            env, env->cr_ifa, 0, true, false, ia64_code_tlb_ed(env));
    }

    if ((pte & IA64_PTE_PRESENT) && perm == 0) {
        return;
    }

    if (is_data) {
        tlb = env->mmu.tlb_data;
        cnt = &env->mmu.tlb_data_count;
        next_replace = &env->mmu.tlb_data_replace;
        pending_count = &env->mmu.pending_purge_data_count;
    } else {
        tlb = env->mmu.tlb_inst;
        cnt = &env->mmu.tlb_inst_count;
        next_replace = &env->mmu.tlb_inst_replace;
        pending_count = &env->mmu.pending_purge_inst_count;
    }

    purged = ia64_purge_tc_entries(env, tlb, cnt, pending_count, va, ps, rid,
                                   is_data, next_replace, &slot);
    if (slot < 0) {
        return;
    }
    ia64_qemu_tlb_flush_entry(env, &tlb[slot], is_data);
    ia64_discard_pending_purge(&tlb[slot], pending_count);

    tlb[slot].va = va;
    tlb[slot].pa = pa;
    tlb[slot].ps = ps;
    tlb[slot].page_mask = ia64_va_vpn_mask(ps);
    tlb[slot].pte = pte;
    tlb[slot].perm = perm;
    tlb[slot].ar = ar;
    tlb[slot].pl = pl;
    tlb[slot].valid = 1;
    tlb[slot].is_tr = 0;
    tlb[slot].pending_purge = 0;
    tlb[slot].rid = rid;
    tlb[slot].key = key;
    tlb[slot].slot = slot;
    if (slot >= *cnt) {
        *cnt = slot + 1;
    }
    ia64_tlb_bump_slot_generation(env, !is_data, slot);
    ia64_assert_pending_purge_counts(env);
    qemu_log_mask(CPU_LOG_MMU,
                  "ia64 itc.%c %s slot=%u va=0x%016" PRIx64
                  " rid=0x%06" PRIx32 " pa=0x%016" PRIx64
                  " ps=0x%016" PRIx64 " perm=0x%x"
                  " pte=0x%016" PRIx64 "\n",
                  is_data ? 'd' : 'i', purged ? "update" : "slot",
                  slot, va, rid, pa, ps, perm, pte);
    /*
     * Overlapping guest TC entries were already purged above.  Only the
     * emulator-provided firmware/SAL identity mappings can exist in the
     * QEMU TLB without a corresponding modeled TC entry.
     */
    ia64_qemu_tlb_flush_entry(env, &tlb[slot], is_data);
}
