/*
 * IA-64 Register Stack Engine and rotating-register architecture logic.
 *
 * The state in IA64RSEState is architected unless its field comment marks it
 * as a derived physical/virtual view.  Helper ABI adapters live in helper/.
 */

#include "qemu/osdep.h"
#include "qemu/atomic.h"
#include "qemu/log.h"
#include "cpu.h"
#include "arch/arch.h"
#include "ia32/ia32.h"
#include "exec-access.h"
#include "exec/target_page.h"
#include "trace.h"

#define IA64_ROTATING_FR_COUNT (IA64_FR_COUNT - 32)

#define IA64_TRACE_RSE_STATE(env, operation) \
    trace_ia64_rse_state(env_cpu(env)->cpu_index, operation, \
                         (env)->ar_bsp, (env)->ar_bspstore, \
                         (env)->rse.rse_dirty, (env)->rse.rse_dirty_nat, \
                         (env)->rse.rse_clean, (env)->rse.rse_clean_nat, \
                         (env)->rse.rse_invalid, (env)->cfm_sof)

static int ia64_rse_mmu_index(CPUIA64State *env)
{
    return env->psr & IA64_PSR_RT ? MMU_IDX_RSE : MMU_PHYS_IDX;
}

/*
 * RSE backing-store accesses.  The retaddr is threaded from the helper
 * entry point: a non-zero value unwinds to the issuing instruction
 * (alloc/flushrs/loadrs mandatory operations fault on the issuing
 * instruction), while 0 delivers the fault with the already-committed
 * env state (br.ret/rfi mandatory loads fault on the target
 * instruction, SDM Vol.2 6.6).
 */
static void ia64_rse_write_u64(CPUIA64State *env, uint64_t addr,
                               uint64_t value, uintptr_t ra)
{
    int mmu_idx = ia64_rse_mmu_index(env);

    ia64_exec_store_mmuidx(env, addr, value, 8,
                           (env->ar_rsc & IA64_RSC_BE) != 0, mmu_idx, ra);
}

static uint64_t ia64_rse_write_collection(CPUIA64State *env, uint64_t addr,
                                          uint64_t value, uint64_t defined,
                                          uint64_t *previous, uintptr_t ra)
{
    int mmu_idx = ia64_rse_mmu_index(env);

    return ia64_exec_rse_store_collection(
        env, addr, value, defined, (env->ar_rsc & IA64_RSC_BE) != 0,
        mmu_idx, previous, ra);
}

static uint64_t ia64_rse_read_u64(CPUIA64State *env, uint64_t addr,
                                  uintptr_t ra)
{
    int mmu_idx = ia64_rse_mmu_index(env);

    return ia64_exec_load_mmuidx(env, addr, 8,
                                 (env->ar_rsc & IA64_RSC_BE) != 0,
                                 mmu_idx, ra);
}

uint64_t ia64_rse_current_cfm(const CPUIA64State *env)
{
    return env->cfm_sof
        | ((uint64_t)env->cfm_sol << IA64_CFM_SOL_SHIFT)
        | ((uint64_t)env->cfm_sor << IA64_CFM_SOR_SHIFT)
        | ((uint64_t)env->cfm_rrb_gr << IA64_CFM_RRB_GR_SHIFT)
        | ((uint64_t)env->cfm_rrb_fr << IA64_CFM_RRB_FR_SHIFT)
        | ((uint64_t)env->cfm_rrb_pr << IA64_CFM_RRB_PR_SHIFT);
}

static uint64_t ia64_rse_current_pfs(const CPUIA64State *env)
{
    return ia64_rse_current_cfm(env)
        | ((env->ar_ec & 0x3fULL) << IA64_PFS_PEC_SHIFT)
        | ((uint64_t)ia64_psr_cpl(env->psr) << IA64_PFS_PPL_SHIFT);
}

/*
 * ---- Register Stack Engine core ----
 *
 * Direct implementation of the architected register stack model of
 * SDM Vol.2 chapter 6: a circular physical stacked register file
 * partitioned into current frame, dirty, clean and invalid regions,
 * with the register stack backing store in guest memory as the
 * authoritative home of spilled values.  See cpu.h for the state
 * layout and partition invariants.
 */

static inline uint32_t ia64_rse_wrap_phys(int32_t idx)
{
    idx %= (int32_t)IA64_STACKED_GR_COUNT;
    if (idx < 0) {
        idx += IA64_STACKED_GR_COUNT;
    }
    return idx;
}

/* BSPSTORE{8:3}: the RNAT bit that collects the register spilled there. */
static inline uint32_t ia64_rse_collect_bit(uint64_t addr)
{
    return (addr >> 3) & 0x3f;
}

/*
 * NaT collection words emitted when a backing-store pointer advances
 * over nregs registers: (addr{8:3} + nregs) / 63 (SDM Vol.2 table 6-2,
 * e.g. the br.call row's AR[BSP] update).
 */
uint32_t ia64_rse_nat_words_grow(uint64_t addr, uint32_t nregs)
{
    return (ia64_rse_collect_bit(addr) + nregs) / 63;
}

/*
 * NaT collection words crossed when a backing-store pointer retreats
 * over nregs registers: (62 - addr{8:3} + nregs) / 63 (SDM Vol.2
 * table 6-2, e.g. the br.ret row's AR[BSP] update).
 */
static inline uint32_t ia64_rse_nat_words_shrink(uint64_t addr, uint32_t nregs)
{
    return (62 - ia64_rse_collect_bit(addr) + nregs) / 63;
}

/*
 * Map a virtual stacked register index [0, sof) to its physical index:
 * registers inside the rotating region are renamed by CFM.rrb.gr
 * modulo the region size before the bottom-of-frame bias is applied
 * (register rotation, SDM Vol.1 4.5.3).
 */
static uint32_t ia64_rse_virt_to_phys(const CPUIA64State *env, uint32_t v)
{
    uint32_t sor_regs = (uint32_t)env->cfm_sor << 3;
    uint32_t p;

    if (v < sor_regs) {
        v += env->cfm_rrb_gr;
        if (v >= sor_regs) {
            v -= sor_regs;
        }
    }
    p = env->rse.rse_bol + v;
    return p < IA64_STACKED_GR_COUNT ? p : p - IA64_STACKED_GR_COUNT;
}

/* Inverse of ia64_rse_virt_to_phys. */
static uint32_t ia64_rse_phys_to_virt(const CPUIA64State *env, uint32_t p)
{
    uint32_t off = p >= env->rse.rse_bol ?
                   p - env->rse.rse_bol :
                   p + IA64_STACKED_GR_COUNT - env->rse.rse_bol;
    uint32_t sor_regs = (uint32_t)env->cfm_sor << 3;

    if (off < sor_regs) {
        off += sor_regs - env->cfm_rrb_gr;
        if (off >= sor_regs) {
            off -= sor_regs;
        }
    }
    return off;
}

static bool ia64_rse_pgr_nat_get(const CPUIA64State *env, uint32_t p)
{
    return (env->rse.rse_pgr_nat[p / 64] >> (p % 64)) & 1;
}

static void ia64_rse_pgr_nat_set(CPUIA64State *env, uint32_t p, bool nat)
{
    uint64_t mask = 1ULL << (p % 64);

    if (nat) {
        env->rse.rse_pgr_nat[p / 64] |= mask;
    } else {
        env->rse.rse_pgr_nat[p / 64] &= ~mask;
    }
}

static void ia64_copy_bit_range(uint64_t dst[2], uint32_t dst_bit,
                                const uint64_t src[2], uint32_t src_bit,
                                uint32_t count)
{
    __uint128_t source;
    __uint128_t target;
    __uint128_t mask;

    if (count == 0) {
        return;
    }

    source = ((__uint128_t)src[1] << 64) | src[0];
    target = ((__uint128_t)dst[1] << 64) | dst[0];
    mask = count == 128 ? ~(__uint128_t)0 :
                          (((__uint128_t)1 << count) - 1);
    mask <<= dst_bit;
    target = (target & ~mask) |
             (((source >> src_bit) << dst_bit) & mask);
    dst[0] = target;
    dst[1] = target >> 64;
}

static void ia64_clear_bit_range(uint64_t bits[2], uint32_t first,
                                 uint32_t count)
{
    uint32_t word = first / 64;
    uint32_t shift = first % 64;
    uint32_t n = MIN(count, 64 - shift);
    uint64_t mask;

    if (count == 0) {
        return;
    }

    mask = n == 64 ? UINT64_MAX : ((1ULL << n) - 1) << shift;
    bits[word] &= ~mask;
    count -= n;
    if (count != 0) {
        mask = count == 64 ? UINT64_MAX : (1ULL << count) - 1;
        bits[word + 1] &= ~mask;
    }
}

/* Copy dirty registers from the virtual view into the physical file. */
static G_GNUC_NO_INLINE void
ia64_rse_sync_frame_out_slow(CPUIA64State *env, uint64_t dirty0,
                             uint64_t dirty1)
{
    uint64_t dirty[2] = { dirty0, dirty1 };
    uint64_t nat[2] = { env->nat[0], env->nat[1] };
    uint32_t sof = env->cfm_sof;
    uint32_t sor_regs = (uint32_t)env->cfm_sor << 3;
    uint32_t rrb_gr = env->cfm_rrb_gr;
    uint32_t bol = env->rse.rse_bol;
    bool nat_files_clear =
        ((nat[0] >> IA64_STACKED_GR_BASE) | nat[1] |
         env->rse.rse_pgr_nat[0] | env->rse.rse_pgr_nat[1]) == 0;
    uint32_t word;

    env->rse.rse_gr_dirty[0] = 0;
    env->rse.rse_gr_dirty[1] = 0;
    /*
     * Dirty bits outside the current frame were ignored by the loops below.
     * Mask them once so the hot loops need neither to visit nor test them.
     */
    if (sof < 64) {
        dirty[0] &= sof == 0 ? 0 : (1ULL << sof) - 1;
        dirty[1] = 0;
    } else {
        dirty[1] &= (1ULL << (sof - 64)) - 1;
    }

    /*
     * Most frames neither rotate their GRs nor wrap around the physical
     * register ring.  With no NaT state to update, their mapping is a simple
     * base-plus-index copy; select that case once per frame instead of
     * repeating rotation and wrap checks for every dirty register.
     */
    if (nat_files_clear && (sor_regs == 0 || rrb_gr == 0) &&
        bol + sof <= IA64_STACKED_GR_COUNT) {
        for (word = 0; word < 2; word++) {
            while (dirty[word] != 0) {
                uint32_t bit = ctz64(dirty[word]);
                uint32_t v = word * 64 + bit;

                dirty[word] &= dirty[word] - 1;
                env->rse.rse_pgr[bol + v] =
                    env->gr[IA64_STACKED_GR_BASE + v];
            }
        }
        return;
    }

    if (nat_files_clear) {
        for (word = 0; word < 2; word++) {
            while (dirty[word] != 0) {
                uint32_t bit = ctz64(dirty[word]);
                uint32_t v = word * 64 + bit;
                uint32_t p = v;

                dirty[word] &= dirty[word] - 1;
                if (v < sor_regs) {
                    p += rrb_gr;
                    if (p >= sor_regs) {
                        p -= sor_regs;
                    }
                }
                p += bol;
                if (p >= IA64_STACKED_GR_COUNT) {
                    p -= IA64_STACKED_GR_COUNT;
                }
                env->rse.rse_pgr[p] =
                    env->gr[IA64_STACKED_GR_BASE + v];
            }
        }
        return;
    }

    for (word = 0; word < 2; word++) {
        while (dirty[word] != 0) {
            uint32_t bit = ctz64(dirty[word]);
            uint32_t v = word * 64 + bit;
            uint32_t p = v;
            uint32_t reg = IA64_STACKED_GR_BASE + v;

            dirty[word] &= dirty[word] - 1;
            if (v < sor_regs) {
                p += rrb_gr;
                if (p >= sor_regs) {
                    p -= sor_regs;
                }
            }
            p += bol;
            if (p >= IA64_STACKED_GR_COUNT) {
                p -= IA64_STACKED_GR_COUNT;
            }
            env->rse.rse_pgr[p] = env->gr[reg];
            ia64_rse_pgr_nat_set(env, p, v < 32 ?
                                 (nat[0] >> (IA64_STACKED_GR_BASE + v)) & 1 :
                                 (nat[1] >> (v - 32)) & 1);
        }
    }
}

/* Keep the overwhelmingly common clean-frame check at each call site. */
static inline QEMU_ALWAYS_INLINE void
ia64_rse_sync_frame_out(CPUIA64State *env)
{
    uint64_t dirty0 = env->rse.rse_gr_dirty[0];
    uint64_t dirty1 = env->rse.rse_gr_dirty[1];

    if (unlikely(dirty0 | dirty1)) {
        ia64_rse_sync_frame_out_slow(env, dirty0, dirty1);
    }
}

static void ia64_rse_sync_frame_in_range(CPUIA64State *env, uint32_t first,
                                         uint32_t count)
{
    uint32_t end = MIN(first + count, (uint32_t)env->cfm_sof);
    uint32_t i;

    if (first >= end) {
        return;
    }

    if (env->cfm_sor == 0 || env->cfm_rrb_gr == 0) {
        uint32_t p = env->rse.rse_bol + first;
        uint32_t total = end - first;
        uint32_t first_span;
        uint32_t second_span;

        if (p >= IA64_STACKED_GR_COUNT) {
            p -= IA64_STACKED_GR_COUNT;
        }
        first_span = MIN(total, IA64_STACKED_GR_COUNT - p);
        second_span = total - first_span;
        memcpy(&env->gr[IA64_STACKED_GR_BASE + first], &env->rse.rse_pgr[p],
               first_span * sizeof(env->rse.rse_pgr[0]));
        if (second_span != 0) {
            memcpy(&env->gr[IA64_STACKED_GR_BASE + first + first_span],
                   env->rse.rse_pgr,
                   second_span * sizeof(env->rse.rse_pgr[0]));
        }
        if (likely((env->rse.rse_pgr_nat[0] | env->rse.rse_pgr_nat[1]) == 0)) {
            ia64_clear_bit_range(env->nat, IA64_STACKED_GR_BASE + first,
                                 total);
        } else {
            ia64_copy_bit_range(env->nat, IA64_STACKED_GR_BASE + first,
                                env->rse.rse_pgr_nat, p, first_span);
            if (second_span != 0) {
                ia64_copy_bit_range(
                    env->nat, IA64_STACKED_GR_BASE + first + first_span,
                    env->rse.rse_pgr_nat, 0, second_span);
            }
        }
        return;
    }

    for (i = first; i < end; i++) {
        uint32_t p = ia64_rse_virt_to_phys(env, i);

        env->gr[IA64_STACKED_GR_BASE + i] = env->rse.rse_pgr[p];
        ia64_gr_nat_set(env, IA64_STACKED_GR_BASE + i,
                        ia64_rse_pgr_nat_get(env, p));
    }
}

/* Load the virtual view of the current frame from the physical file. */
static void ia64_rse_sync_frame_in(CPUIA64State *env)
{
    uint32_t i;

    if (env->cfm_sor == 0 || env->cfm_rrb_gr == 0) {
        uint32_t first = MIN((uint32_t)env->cfm_sof,
                             IA64_STACKED_GR_COUNT - env->rse.rse_bol);
        uint32_t second = env->cfm_sof - first;

        memcpy(&env->gr[IA64_STACKED_GR_BASE],
               &env->rse.rse_pgr[env->rse.rse_bol],
               first * sizeof(env->rse.rse_pgr[0]));
        if (second != 0) {
            memcpy(&env->gr[IA64_STACKED_GR_BASE + first], env->rse.rse_pgr,
                   second * sizeof(env->rse.rse_pgr[0]));
        }
        if (likely((env->rse.rse_pgr_nat[0] | env->rse.rse_pgr_nat[1]) == 0)) {
            ia64_clear_bit_range(env->nat, IA64_STACKED_GR_BASE,
                                 env->cfm_sof);
        } else {
            ia64_copy_bit_range(env->nat, IA64_STACKED_GR_BASE,
                                env->rse.rse_pgr_nat, env->rse.rse_bol, first);
            if (second != 0) {
                ia64_copy_bit_range(env->nat, IA64_STACKED_GR_BASE + first,
                                    env->rse.rse_pgr_nat, 0, second);
            }
        }
        env->rse.rse_gr_dirty[0] = 0;
        env->rse.rse_gr_dirty[1] = 0;
        return;
    }

    for (i = 0; i < env->cfm_sof; i++) {
        uint32_t p = ia64_rse_virt_to_phys(env, i);

        env->gr[IA64_STACKED_GR_BASE + i] = env->rse.rse_pgr[p];
        ia64_gr_nat_set(env, IA64_STACKED_GR_BASE + i,
                        ia64_rse_pgr_nat_get(env, p));
    }
    env->rse.rse_gr_dirty[0] = 0;
    env->rse.rse_gr_dirty[1] = 0;
}

/*
 * Perform one mandatory RSE store at AR.BSPSTORE, spilling either the
 * oldest dirty register or, when BSPSTORE{8:3} is all ones, the RNAT
 * collection (SDM Vol.2 6.5.2).  Returns 1 when a register was stored,
 * 0 for a NaT collection word.
 */
static int ia64_rse_store_one(CPUIA64State *env, uintptr_t ra)
{
    uint64_t bspstore = env->ar_bspstore;
    uint32_t ncb = ia64_rse_collect_bit(bspstore);

    if (ncb == 63) {
        /*
         * SDM Vol.2 6.5: whenever BSPSTORE{8:3} are all ones the RSE
         * stores RNAT; bit 63 is always written as zero, and the spill
         * leaves RNAT undefined (cleared here).
         *
         * AR.RNAT only carries collection bits for the slots stored since
         * it last became authoritative (rse_rnat_low).  When a spill
         * episode begins in the middle of a collection group -- BSPSTORE
         * descends arithmetically as br.ret consumes registers, then a
         * later call re-spills only the upper slots -- writing AR.RNAT
         * verbatim would destroy the committed bits of the untouched
         * lower slots, which their eventual mandatory reloads still need.
         * Merge the bits below the authoritative floor from the word in
         * memory.
         */
        uint64_t word = env->ar_rnat & INT64_MAX;
        uint64_t group_base = bspstore - 0x1f8;
        uint64_t keep = 0;

        if (env->rse.rse_rnat_low > group_base) {
            if (env->rse.rse_rnat_low > bspstore) {
                /*
                 * The floor sits above this collection word -- possible
                 * when br.ret rebases BSPSTORE arithmetically down to
                 * exactly a collection-word address without any register
                 * store in between.  AR.RNAT then says nothing about this
                 * group at all, and collect_bit() of an address in a
                 * *different* group is a meaningless index (it used to
                 * yield keep = 0 here, overwriting the whole committed
                 * word with stale AR.RNAT content).  Preserve the word.
                 */
                keep = INT64_MAX;
            } else {
                /*
                 * group_base < rse_rnat_low <= bspstore: the episode
                 * began mid-group, the index is in-group by construction.
                 */
                keep = (1ULL << ia64_rse_collect_bit(
                            env->rse.rse_rnat_low)) - 1;
            }

        }
        if (keep != 0) {
            /*
             * Merge the committed lower bits from the word in memory
             * without an architectural load: the RSE is only supposed to
             * be *storing* here, so read permission checks and read
             * watchpoints must not fire on the backing store page.
             */
            uint64_t previous;

            word = ia64_rse_write_collection(env, bspstore,
                                             word & ~keep & INT64_MAX,
                                             INT64_MAX & ~keep,
                                             &previous, ra);
            trace_ia64_rse_rnat_boundary_store(env_cpu(env)->cpu_index,
                                               bspstore, word & INT64_MAX,
                                               keep, env->ar_rnat,
                                               env->rse.rse_rnat_low);
        } else {
            ia64_rse_write_u64(env, bspstore, word & INT64_MAX, ra);
        }
        env->ar_rnat = 0;
        trace_ia64_rse_rnat_floor(env_cpu(env)->cpu_index, "boundary-store",
                                  env->rse.rse_rnat_low, bspstore + 8,
                                  bspstore, 0);
        env->rse.rse_rnat_low = bspstore + 8;
        env->ar_bspstore = bspstore + 8;
        env->rse.rse_dirty_nat--;
        env->rse.rse_clean_nat++;
        env->psr &= ~(IA64_PSR_DA | IA64_PSR_DD);
        return 0;
    } else {
        uint32_t p = ia64_rse_wrap_phys((int32_t)env->rse.rse_bol -
                                        env->rse.rse_dirty);

        ia64_rse_write_u64(env, bspstore, env->rse.rse_pgr[p], ra);
        trace_ia64_rse_spill(env_cpu(env)->cpu_index, bspstore,
                             env->rse.rse_pgr[p],
                             ia64_rse_pgr_nat_get(env, p));
        if (env->rse.rse_rnat_low > bspstore) {
            env->rse.rse_rnat_low = bspstore;
        }
        if (ia64_rse_pgr_nat_get(env, p)) {
            env->ar_rnat |= 1ULL << ncb;
        } else {
            env->ar_rnat &= ~(1ULL << ncb);
        }
        env->ar_bspstore = bspstore + 8;
        env->rse.rse_dirty--;
        env->rse.rse_clean++;
        env->psr &= ~(IA64_PSR_DA | IA64_PSR_DD);
        return 1;
    }
}

/*
 * Perform one mandatory RSE load from the first backing-store word
 * below the clean partition, filling either an invalid physical
 * register or reloading the RNAT collection when the load pointer sits
 * on a NaT collection word (SDM Vol.2 6.5.2).  Returns 1 when a
 * register was loaded, 0 for a NaT collection word.  When the load
 * targets the current frame (only possible while the frame is
 * incomplete) the virtual view is updated alongside the physical file
 * so that a fault on a later load leaves a consistent frame.
 */
static int ia64_rse_load_one(CPUIA64State *env, uintptr_t ra)
{
    int64_t live = (int64_t)env->rse.rse_clean + env->rse.rse_clean_nat +
                   env->rse.rse_dirty + env->rse.rse_dirty_nat;
    uint64_t bspload = env->ar_bsp - (live + 1) * 8;
    uint32_t ncb = ia64_rse_collect_bit(bspload);

    if (ncb == 63) {
        env->ar_rnat = ia64_rse_read_u64(env, bspload, ra) & INT64_MAX;
        trace_ia64_rse_rnat_floor(env_cpu(env)->cpu_index, "collection-load",
                                  env->rse.rse_rnat_low, bspload - 0x1f8,
                                  env->ar_bspstore, env->ar_rnat);
        env->rse.rse_rnat_low = bspload - 0x1f8;
        env->rse.rse_clean_nat++;
        env->psr &= ~(IA64_PSR_DA | IA64_PSR_DD);
        return 0;
    } else {
        uint64_t value = ia64_rse_read_u64(env, bspload, ra);
        uint32_t p = ia64_rse_wrap_phys(
            (int32_t)env->rse.rse_bol -
            (env->rse.rse_clean + env->rse.rse_dirty + 1));
        uint32_t v;
        bool nat;

        /*
         * AR.RNAT only represents the collection bits of slots at or
         * above rse_rnat_low.  A mandatory load below that (the reload
         * range is truncated to the missing registers, so the descent
         * never passes the slot's collection word) must take the bit from
         * the collection word in memory, exactly where the spill
         * committed it.
         */
        if (bspload < env->rse.rse_rnat_low) {
            uint64_t word = ia64_rse_read_u64(env, bspload | 0x1f8, ra);

            nat = (word >> ncb) & 1;
        } else {
            nat = (env->ar_rnat >> ncb) & 1;
        }
        trace_ia64_rse_fill(env_cpu(env)->cpu_index, bspload, value, nat);
        env->rse.rse_pgr[p] = value;
        ia64_rse_pgr_nat_set(env, p, nat);
        env->rse.rse_clean++;
        env->rse.rse_invalid--;

        v = ia64_rse_phys_to_virt(env, p);
        if (v < env->cfm_sof) {
            env->gr[IA64_STACKED_GR_BASE + v] = value;
            ia64_gr_nat_set(env, IA64_STACKED_GR_BASE + v,
                            ia64_rse_pgr_nat_get(env, p));
        }
        env->psr &= ~(IA64_PSR_DA | IA64_PSR_DD);
        return 1;
    }
}

/*
 * Complete an incomplete frame with mandatory RSE loads after br.ret
 * or rfi (SDM Vol.2 6.8).  env must already be committed to the
 * post-branch state: faults are delivered on the target instruction
 * (ra == 0) with the partitions describing exactly the loads that
 * remain.
 *
 * RSE.CFLE (SDM Vol.2 6.6) is set for the duration of the sequence:
 * br.ret and rfi set it when the frame being returned to is not
 * entirely contained in the physical stacked register file, and it is
 * cleared when the sequence completes.  A load that faults leaves the
 * frame incomplete; interruption delivery then clears CFLE and the
 * handler runs with the incomplete frame until it executes cover or
 * an rfi resumes the sequence.
 */
static void ia64_rse_complete_frame_loads(CPUIA64State *env, uintptr_t ra)
{
    if (env->rse.rse_dirty >= 0 && env->rse.rse_dirty_nat >= 0) {
        return;
    }

    env->rse.rse_cfle = true;
    while (env->rse.rse_dirty < 0 || env->rse.rse_dirty_nat < 0) {
        if (ia64_rse_load_one(env, ra)) {
            env->rse.rse_clean--;
            env->rse.rse_dirty++;
        } else {
            env->rse.rse_clean_nat--;
            env->rse.rse_dirty_nat++;
        }
        env->ar_bspstore -= 8;
    }
    env->rse.rse_cfle = false;
}

/* br.call/cover: the current frame joins the dirty partition. */
static void ia64_rse_preserve_frame(CPUIA64State *env, uint32_t nregs)
{
    uint32_t nats;

    if (nregs == 0) {
        return;
    }
    nats = ia64_rse_nat_words_grow(env->ar_bsp, nregs);

    env->rse.rse_bol = ia64_rse_wrap_phys(env->rse.rse_bol + nregs);
    env->ar_bsp += (uint64_t)(nregs + nats) * 8;
    env->rse.rse_dirty += nregs;
    env->rse.rse_dirty_nat += nats;
}

/*
 * Grow the current frame for alloc.  New registers come from the
 * invalid partition first; once that is exhausted the oldest clean
 * registers are reused (their backing-store copies remain valid), and
 * only when the whole file is occupied does the RSE issue mandatory
 * stores to make room (SDM Vol.2 6.4).  Restartable: a faulting store
 * leaves the partitions describing the completed portion and the
 * alloc re-executes.
 */
static void ia64_rse_new_frame(CPUIA64State *env, int32_t growth,
                               uintptr_t ra)
{
    if (growth <= env->rse.rse_invalid) {
        env->rse.rse_invalid -= growth;
        return;
    }
    growth -= env->rse.rse_invalid;

    if (growth <= env->rse.rse_clean) {
        env->rse.rse_invalid = 0;
        env->rse.rse_clean -= growth;
        env->rse.rse_clean_nat =
            ia64_rse_nat_words_shrink(
                env->ar_bsp,
                env->rse.rse_clean + env->rse.rse_dirty + 1) -
            env->rse.rse_dirty_nat;
        return;
    }
    growth -= env->rse.rse_clean;

    /*
     * Mandatory stores make room for the remainder.  The invalid and
     * clean partitions are consumed only after the last store
     * completes: a store that faults must leave every register still
     * owned by its partition so that the re-executed alloc recomputes
     * exactly the work that remains.
     */
    while (growth > 0) {
        growth -= ia64_rse_store_one(env, ra);
    }
    env->rse.rse_invalid = 0;
    env->rse.rse_clean = 0;
    env->rse.rse_clean_nat = 0;
}

/*
 * Partition bookkeeping for the frame restored by br.ret or rfi.
 * "preserved" is the number of new-frame registers that lie below the
 * old bottom-of-frame (AR.PFS.pfm.sol for br.ret, CR.IFS.ifm.sof for
 * rfi, per the AR[BSP] rows of SDM Vol.2 table 6-2); "growth" is how
 * far the new frame's top extends beyond the old frame's top;
 * "old_sof" is the returned-from frame's size.  rse_bol must already
 * have been moved down by "preserved" and CFM set to the restored
 * frame marker.
 */
static void ia64_rse_restore_frame(CPUIA64State *env, uint32_t preserved,
                                   int32_t growth, uint32_t old_sof)
{
    int32_t preserved_nats =
        ia64_rse_nat_words_shrink(env->ar_bsp, preserved);
    int32_t missing;
    int32_t missing_nats;

    env->ar_bsp -= (uint64_t)(preserved + preserved_nats) * 8;

    if (growth > env->rse.rse_invalid + env->rse.rse_clean) {
        /*
         * Bad PFS used by branch return (SDM Vol.2 6.5.5): the output
         * area of the frame being returned to does not fit in the
         * physical file.  CFM is forced to zero, the preserved and
         * returned-from registers all join the invalid partition, and
         * the dirty partition shrinks by the preserved registers; the
         * clean partition is left unchanged.
         */
        env->rse.rse_invalid += preserved + old_sof;
        env->rse.rse_dirty -= preserved;
        env->rse.rse_dirty_nat -= preserved_nats;
        env->cfm_sof = 0;
        env->cfm_sol = 0;
        env->cfm_sor = 0;
        env->cfm_rrb_gr = 0;
        ia64_set_cfm_rrb_fr(env, 0);
        env->cfm_rrb_pr = 0;
        return;
    }

    /*
     * Any growth of the frame's top consumes invalid registers first
     * and then the oldest clean registers, whose backing-store copies
     * remain valid.
     */
    if (growth > env->rse.rse_invalid) {
        env->rse.rse_clean -= growth - env->rse.rse_invalid;
        env->rse.rse_clean_nat =
            ia64_rse_nat_words_shrink(
                env->ar_bsp,
                env->rse.rse_clean + env->rse.rse_dirty + 1) -
            env->rse.rse_dirty_nat;
        env->rse.rse_invalid = 0;
    } else {
        env->rse.rse_invalid -= growth;
    }

    /*
     * The preserved registers re-enter the frame from the top of the
     * dirty partition.  Anything beyond it is taken from the clean
     * partition, and anything older than that is no longer in the
     * physical file: the frame becomes incomplete (negative dirty
     * counts, BSPSTORE above BSP) until mandatory loads bring the
     * missing registers back from the backing store (SDM Vol.2 6.8).
     */
    missing = (int32_t)preserved - env->rse.rse_dirty;
    missing_nats = preserved_nats - env->rse.rse_dirty_nat;
    if (missing <= 0) {
        env->rse.rse_dirty -= preserved;
        env->rse.rse_dirty_nat -= preserved_nats;
        return;
    }

    /*
     * The restore extends below AR.BSPSTORE.  Do not satisfy it from the
     * clean partition: discard the clean registers and reload the whole
     * range from the backing store with mandatory loads instead.
     *
     * The clean partition is only a cache of memory (SDM Vol.2 6.5.2) and
     * an implementation may drop it at any time, but which copy is used
     * is observable when software has modified the backing store under
     * it.  The architected edit sequence (SDM Vol.2 6.10) rewrites
     * BSPSTORE, which empties the clean partition, so architected guests
     * see no difference.  gcc 2.96's IA-64 unwinder, however, edits the
     * flushed backing store *without* the BSPSTORE rewrite (its
     * ia64_throw_helper carries a literal "TODO, do we need to do
     * anything to make the values we wrote 'stick'?") and relies on the
     * edited saved-b0/ar.pfs slots being reloaded: restoring the stale
     * clean copy makes __throw "return" to its call site instead of the
     * landing pad, which broke every C++ cleanup unwind on Debian 3.0
     * (update-menus SIGSEGV, g++ 2.95/2.96 exception tests SIGABRT)
     * whenever no interruption happened to evict the copies first.
     */
    if (env->rse.rse_clean > 0 || env->rse.rse_clean_nat > 0) {
        env->rse.rse_invalid += env->rse.rse_clean;
        env->rse.rse_clean = 0;
        env->rse.rse_clean_nat = 0;
    }

    env->rse.rse_dirty = -missing;
    env->rse.rse_dirty_nat = -missing_nats;
    env->ar_bspstore = env->ar_bsp -
        (int64_t)(env->rse.rse_dirty + env->rse.rse_dirty_nat) * 8;
}

/* br.ia and rfi-to-IA-32: only the current frame stays valid. */
static void ia64_rse_invalidate_non_current(CPUIA64State *env)
{
    env->rse.rse_dirty = 0;
    env->rse.rse_dirty_nat = 0;
    env->rse.rse_clean = 0;
    env->rse.rse_clean_nat = 0;
    env->rse.rse_invalid = IA64_STACKED_GR_COUNT - env->cfm_sof;
    /*
     * Dropping the dirty partition empties the backing store's dirty
     * region, so AR.BSPSTORE meets AR.BSP.  br.ia rejects a non-empty
     * store beforehand, but an rfi to an IA-32 target arrives here with
     * whatever the interrupted context left, and leaving BSPSTORE behind
     * would break BSPSTORE == BSP - 8*ndirty.
     *
     * Upstream writes this through ia64_rse_rnat_move_bspstore(), which
     * also maintains the AR.RNAT collection-group tracking added by
     * 1b35c22; that commit is reverted on this branch (it corrupts NaT
     * state for Windows guests), so the plain assignment used by the other
     * BSPSTORE updates here applies.
     */
    env->ar_bspstore = env->ar_bsp;
}

/*
 * Common CFM/BOF update for br.ret and rfi.  Writes the restored frame
 * marker, moves BOF down by "preserved", adjusts the partitions, and
 * performs any mandatory loads.  The caller must have committed PSR
 * and IP to the post-branch state first (mandatory load faults are
 * delivered on the target instruction).
 */
static void ia64_rse_return_to_frame(CPUIA64State *env, uint64_t pfm,
                                     uint32_t preserved)
{
    uint32_t old_sof = env->cfm_sof;
    uint32_t new_sof = pfm & IA64_CFM_SOF_MASK;
    uint32_t new_sol = (pfm & IA64_CFM_SOL_MASK) >> IA64_CFM_SOL_SHIFT;
    int32_t growth = (int32_t)new_sof - (int32_t)preserved -
                     (int32_t)old_sof;

    ia64_rse_sync_frame_out(env);

    env->cfm_sof = new_sof;
    env->cfm_sol = new_sol;
    env->cfm_sor = (pfm & IA64_CFM_SOR_MASK) >> IA64_CFM_SOR_SHIFT;
    env->cfm_rrb_gr = (pfm & IA64_CFM_RRB_GR_MASK) >> IA64_CFM_RRB_GR_SHIFT;
    ia64_set_cfm_rrb_fr(env, (pfm & IA64_CFM_RRB_FR_MASK) >>
                             IA64_CFM_RRB_FR_SHIFT);
    env->cfm_rrb_pr = (pfm & IA64_CFM_RRB_PR_MASK) >> IA64_CFM_RRB_PR_SHIFT;
    env->rse.rse_bol = ia64_rse_wrap_phys((int32_t)env->rse.rse_bol -
                                      (int32_t)preserved);

    ia64_rse_restore_frame(env, preserved, growth, old_sof);
    ia64_rse_sync_frame_in(env);
    ia64_invalidate_stacked_alat(env);
    ia64_rse_complete_frame_loads(env, 0);
    ia64_rse_check(env, "return");
}

void ia64_rse_delivery_check(CPUIA64State *env, int excp)
{
    char site[32];

    snprintf(site, sizeof(site), "delivery excp=%d", excp);
    ia64_rse_check(env, site);
}

/*
 * Internal consistency checks for the register stack model.  Each RSE
 * operation must leave the four partitions covering the physical file
 * exactly once, the backing-store pointers in their architected
 * relationship, and the NaT-collection word counts matching the
 * partition boundaries.  A violation indicates an emulator bug; log it
 * once with enough state to reconstruct the failure.
 */
void ia64_rse_check(CPUIA64State *env, const char *site)
{
#ifdef CONFIG_DEBUG_TCG
    static unsigned reported;
    int64_t total = (int64_t)env->cfm_sof + env->rse.rse_dirty +
                    env->rse.rse_clean + env->rse.rse_invalid;
    uint64_t expected_bspstore = env->ar_bsp -
        (int64_t)(env->rse.rse_dirty + env->rse.rse_dirty_nat) * 8;
    /*
     * Each violated invariant sets its own bit so the report names the
     * failing condition instead of leaving it to be reconstructed from
     * the printed fields (which historically omitted exactly the fields
     * the RNAT checks read).
     */
    unsigned viol = 0;
#define RSE_VIOL(cond, bit) do { if (cond) viol |= 1u << (bit); } while (0)
    RSE_VIOL(total != IA64_STACKED_GR_COUNT, 0);          /* partition sum */
    RSE_VIOL(env->ar_bspstore != expected_bspstore, 1);   /* bspstore rel. */
    RSE_VIOL(env->rse.rse_clean < 0, 2);
    RSE_VIOL(env->rse.rse_invalid < 0, 3);
    RSE_VIOL(env->rse.rse_bol >= IA64_STACKED_GR_COUNT, 4);

    if (!viol && env->rse.rse_dirty >= 0) {
        /* NaT collection words live at addresses 0x1f8 mod 0x200. */
        RSE_VIOL(env->rse.rse_dirty_nat !=
                 (int32_t)((int64_t)(env->ar_bsp >> 9) -
                           (int64_t)(env->ar_bspstore >> 9)), 5);
    }
    if (!viol && env->rse.rse_clean >= 0 && env->rse.rse_dirty >= 0) {
        uint64_t bspload = env->ar_bspstore -
            (int64_t)(env->rse.rse_clean + env->rse.rse_clean_nat) * 8;

        RSE_VIOL(env->rse.rse_clean_nat !=
                 (int32_t)((int64_t)(env->ar_bspstore >> 9) -
                           (int64_t)(bspload >> 9)), 6);
    }

    /*
     * AR.RNAT floor invariants (see the comments in ia64_rse_store_one):
     * the floor is a slot address, so 8-byte aligned, and AR.RNAT's
     * bit 63 is unrepresentable and masked on every write path.
     *
     * There is no checkable upper bound against AR.BSP or AR.BSPSTORE:
     * a shrinking br.ret rebases both pointers arithmetically down and
     * legitimately leaves the floor above them (the keep = INT64_MAX
     * branch in ia64_rse_store_one exists exactly for that state, and
     * clamping the floor there would misapply stale AR.RNAT bits into a
     * lower collection word).  Only the alignment and the bit-63 mask
     * are invariant.
     */
    RSE_VIOL((env->rse.rse_rnat_low & 7) != 0, 7);
    RSE_VIOL((env->ar_rnat >> 63) != 0, 9);
#undef RSE_VIOL

    if (viol && qatomic_fetch_inc(&reported) < 8) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "ia64 rse inconsistency at %s: viol=%#x excp=%u"
                      " ip=%016" PRIx64
                      " sof=%u sol=%u sor=%u rrb=%u bol=%u dirty=%d/%d"
                      " clean=%d/%d invalid=%d bsp=%016" PRIx64
                      " bspstore=%016" PRIx64 " rnat=%016" PRIx64
                      " rnatlow=%016" PRIx64 " cfle=%d\n",
                      site, viol, env->exception_state.exception, env->ip,
                      env->cfm_sof, env->cfm_sol,
                      env->cfm_sor, env->cfm_rrb_gr, env->rse.rse_bol,
                      env->rse.rse_dirty, env->rse.rse_dirty_nat,
                      env->rse.rse_clean,
                      env->rse.rse_clean_nat, env->rse.rse_invalid, env->ar_bsp,
                      env->ar_bspstore, env->ar_rnat, env->rse.rse_rnat_low,
                      env->rse.rse_cfle);
    }
#else
    (void)env;
    (void)site;
#endif
}

void ia64_rfi(CPUIA64State *env, uint64_t fault_ip, uint32_t fault_slot)
{
    uint64_t old_psr = env->psr;
    uint64_t ipsr = env->cr_ipsr;
    uint64_t raw_iip = env->cr_iip;
    uint64_t iip = (ipsr & IA64_PSR_IS) ?
                   (raw_iip & UINT32_MAX) :
                   ia64_ip_bundle_addr(raw_iip);
    uint64_t ifs = env->cr_ifs;
    bool unimplemented_ia32_target =
        (ipsr & IA64_PSR_IS) &&
        ((ipsr & IA64_PSR_IT) ? !ia64_va_is_implemented(env, raw_iip) :
                                !ia64_pa_is_implemented(env, raw_iip));

    /*
     * Montecito has no native IA-32 execution engine.  An OS must use its
     * IA-32 execution layer instead of restoring PSR.is.  Keep the rfi
     * itself as the faulting instruction and do not commit the target PSR,
     * IP, or RSE state when that transition is requested.
     */
    if ((ipsr & IA64_PSR_IS) &&
        !ia64_env_cpu_class(env)->has_native_ia32) {
        ia64_raise_disabled_isa_transition(env, fault_ip, fault_slot);
    }

    env->exception_state.exception = IA64_EXCP_NONE;
    env->exception_state.fault_ip = 0;
    env->exception_state.fault_addr = 0;
    env->exception_state.fault_imm = 0;
    env->exception_state.fault_slot = 0;
    env->instruction_group_start = true;

    /*
     * rfi restores PSR and IP before the RSE moves the frame: mandatory
     * RSE loads are delivered on the target instruction and use the
     * restored PSR state (SDM Vol.2 6.6).  CR[IPSR], CR[IIP] and
     * CR[IFS] are consumed, not modified.
     */
    if (ipsr & IA64_PSR_IS) {
        ipsr &= ~(IA64_PSR_DA | IA64_PSR_DD |
                  IA64_PSR_IA | IA64_PSR_ED);
    }
    ia64_set_psr(env, ipsr);
    env->ip = iip;
    ia64_tlb_serialize(env, 1, 1);
    ia64_flush_on_pk_change(env, old_psr);

    if (ipsr & IA64_PSR_IS) {
        /* Return to IA-32: the register stack is left empty. */
        ia64_rse_sync_frame_out(env);
        env->cfm_sof = 0;
        env->cfm_sol = 0;
        env->cfm_sor = 0;
        env->cfm_rrb_gr = 0;
        ia64_set_cfm_rrb_fr(env, 0);
        env->cfm_rrb_pr = 0;
        ia64_rse_invalidate_non_current(env);
        ia64_alat_invala(env);
        ia64_ia32_enter(env);
        if (unimplemented_ia32_target) {
            /*
             * This is an IA-64 trap on the completed rfi, not a fault on
             * the first IA-32 instruction.  Preserve all 64 target bits.
             */
            env->cr_isr = IA64_ISR_CODE_UI;
            env->exception_state.ia32_transition_trap = true;
            ia64_raise_exception(env, IA64_EXCP_UNIMPL_INST_ADDR,
                                 raw_iip, fault_ip, fault_slot);
        }
        return;
    }

    if (ifs & IA64_IFS_V) {
        ia64_rse_return_to_frame(env, ifs & IA64_IFS_IFM_MASK,
                                 ifs & IA64_CFM_SOF_MASK);
    } else if (env->rse.rse_dirty < 0 || env->rse.rse_dirty_nat < 0) {
        /*
         * SDM Vol.2 6.8: an interruption taken during the mandatory
         * loads of a br.ret/rfi leaves the current frame incomplete.
         * Returning with IFS.v = 0 resumes the original sequence of
         * mandatory loads for the still-incomplete frame.
         */
        ia64_rse_complete_frame_loads(env, 0);
        ia64_rse_check(env, "rfi-resume");
    }
}

bool ia64_gr_nat_get(const CPUIA64State *env, uint32_t reg)
{
    if (reg == 0) {
        return false;
    }

    return (env->nat[reg / 64] >> (reg % 64)) & 1;
}

void ia64_gr_nat_set(CPUIA64State *env, uint32_t reg, bool nat)
{
    if (reg == 0) {
        return;
    }

    if (nat) {
        env->nat[reg / 64] |= (1ULL << (reg % 64));
    } else {
        env->nat[reg / 64] &= ~(1ULL << (reg % 64));
    }
}

static void ia64_rotate_rotating_gr_right(CPUIA64State *env)
{
    const __uint128_t all_mask = (((__uint128_t)1 << 96) - 1);
    uint32_t count = env->cfm_sor * 8;
    __uint128_t nat;
    __uint128_t mask;
    __uint128_t rotating_nat;
    uint64_t last;

    if (count == 0) {
        return;
    }
    if (count > env->cfm_sof) {
        count = env->cfm_sof;
    }
    if (count > IA64_STACKED_GR_COUNT) {
        count = IA64_STACKED_GR_COUNT;
    }

    last = env->gr[IA64_STACKED_GR_BASE + count - 1];
    memmove(&env->gr[IA64_STACKED_GR_BASE + 1],
            &env->gr[IA64_STACKED_GR_BASE],
            (count - 1) * sizeof(*env->gr));
    env->gr[IA64_STACKED_GR_BASE] = last;

    nat = (((__uint128_t)env->nat[1] << 32) | (env->nat[0] >> 32)) &
          all_mask;
    mask = (((__uint128_t)1 << count) - 1);
    rotating_nat = nat & mask;
    rotating_nat = ((rotating_nat << 1) |
                    (rotating_nat >> (count - 1))) & mask;
    nat = (nat & ~mask) | rotating_nat;
    env->nat[0] = (env->nat[0] & UINT32_MAX) | (uint64_t)(nat << 32);
    env->nat[1] = nat >> 32;
    ia64_invalidate_alat_reg_range(env, IA64_STACKED_GR_BASE,
                                   IA64_STACKED_GR_BASE + count, false);
}

static void ia64_rotate_predicates_right(CPUIA64State *env)
{
    uint8_t last = env->pr[IA64_PR_LAST] & 1;

    memmove(&env->pr[IA64_PR_ROTATING_NEXT],
            &env->pr[IA64_PR_ROTATING_BASE],
            47 * sizeof(env->pr[IA64_PR_TRUE]));
    env->pr[IA64_PR_ROTATING_BASE] = last;
    env->pr[IA64_PR_TRUE] = 1;
}

static void ia64_rotate_loop_regs(CPUIA64State *env)
{
    ia64_rse_check(env, "ctop");
    ia64_rse_sync_frame_out(env);
    ia64_rotate_rotating_gr_right(env);
    ia64_rotate_predicates_right(env);
    if (env->cfm_sor != 0) {
        uint8_t count = env->cfm_sor << 3;

        env->cfm_rrb_gr = env->cfm_rrb_gr ?
                          env->cfm_rrb_gr - 1 : count - 1;
    }
    ia64_set_cfm_rrb_fr(env, env->cfm_rrb_fr ?
                             env->cfm_rrb_fr - 1 :
                             IA64_ROTATING_FR_COUNT - 1);
    env->cfm_rrb_pr = env->cfm_rrb_pr ?
                      env->cfm_rrb_pr - 1 : 47;
}

void ia64_rse_br_call(CPUIA64State *env, uint32_t b_reg,
                         uint64_t next_ip, uint64_t target)
{
    uint64_t pfs = ia64_rse_current_pfs(env);
    uint32_t old_sof = env->cfm_sof;
    uint32_t sol = env->cfm_sol;
    uint32_t outputs = old_sof > sol ? old_sof - sol : 0;
    bool move_outputs = env->cfm_rrb_gr == 0;

    ia64_rse_sync_frame_out(env);
    ia64_rse_preserve_frame(env, sol);
    if (move_outputs && outputs != 0 && sol != 0) {
        memmove(&env->gr[IA64_STACKED_GR_BASE],
                &env->gr[IA64_STACKED_GR_BASE + sol],
                outputs * sizeof(*env->gr));
        /*
         * The output frame moves toward lower logical registers.  Copy its
         * NaT bits from a snapshot so the overlapping ranges cannot affect
         * one another.
         */
        ia64_copy_bit_range(env->nat, IA64_STACKED_GR_BASE,
                            env->nat, IA64_STACKED_GR_BASE + sol, outputs);
    }
    env->cfm_sof = outputs;
    env->cfm_sol = 0;
    env->cfm_sor = 0;
    env->cfm_rrb_gr = 0;
    ia64_set_cfm_rrb_fr(env, 0);
    env->cfm_rrb_pr = 0;
    if (!move_outputs) {
        ia64_rse_sync_frame_in(env);
    }
    ia64_invalidate_stacked_alat(env);

    env->ar_pfs = pfs;
    env->br[b_reg] = next_ip;
    env->ip = ia64_ip_bundle_addr(target);
    env->psr &= ~IA64_PSR_RI_MASK;
    ia64_rse_check(env, "br.call");
    IA64_TRACE_RSE_STATE(env, "br.call");
}

void ia64_rse_br_ia(CPUIA64State *env, uint32_t b_reg,
                  uint64_t fault_ip, uint32_t fault_slot)
{
    uint64_t target = env->br[b_reg];
    uint64_t trap_code;
    IA64Exception trap;

    if (env->ar_bspstore != env->ar_bsp) {
        ia64_raise_exception(env, IA64_EXCP_ILLEGAL, fault_ip, 0,
                               fault_slot);
        return;
    }

    if ((env->psr & IA64_PSR_DI) ||
        !ia64_env_cpu_class(env)->has_native_ia32) {
        ia64_raise_disabled_isa_transition(env, fault_ip, fault_slot);
    }

    /*
     * Perform the architectural IA-64 to IA-32 transition: IP takes
     * BR[b1]{31:0} with byte granularity, PSR.is is set, and only the
     * current (zero-size) frame stays valid.  Madison then imports the
     * architected IA-32 register image before dispatching an IA-32 TB.
     */
    env->ip = target & UINT32_MAX;
    env->psr |= IA64_PSR_IS;
    env->psr &= ~(IA64_PSR_DA | IA64_PSR_DD | IA64_PSR_IA | IA64_PSR_ED |
                  IA64_PSR_RI_MASK);
    ia64_rse_sync_frame_out(env);
    env->cfm_sof = 0;
    env->cfm_sol = 0;
    env->cfm_sor = 0;
    env->cfm_rrb_gr = 0;
    ia64_set_cfm_rrb_fr(env, 0);
    env->cfm_rrb_pr = 0;
    ia64_rse_invalidate_non_current(env);
    ia64_alat_invala(env);
    ia64_ia32_enter(env);

    trap_code = ((env->psr & IA64_PSR_IT) ?
                 !ia64_va_is_implemented(env, target) :
                 !ia64_pa_is_implemented(env, target)) ? IA64_ISR_CODE_UI : 0;
    trap_code |= (env->psr & IA64_PSR_TB) ? IA64_ISR_CODE_TB : 0;
    trap_code |= (env->psr & IA64_PSR_SS) ? IA64_ISR_CODE_SS : 0;
    if (!trap_code) {
        return;
    }

    if (trap_code & IA64_ISR_CODE_UI) {
        trap = IA64_EXCP_UNIMPL_INST_ADDR;
    } else if (trap_code & IA64_ISR_CODE_TB) {
        trap = IA64_EXCP_TAKEN_BRANCH;
    } else {
        trap = IA64_EXCP_SINGLE_STEP;
    }
    env->cr_isr = trap_code;
    env->exception_state.ia32_transition_trap = true;
    ia64_raise_exception(env, trap, target, fault_ip, fault_slot);
}





void ia64_rse_pop_return_frame(CPUIA64State *env, uint64_t pfs)
{
    /*
     * Restore AR.EC before the mandatory RSE loads: return_to_frame may take a
     * fill fault, and the handler must already see the restored epilog count.
     */
    env->ar_ec = (pfs & IA64_PFS_PEC_MASK) >> IA64_PFS_PEC_SHIFT;
    ia64_rse_return_to_frame(env, pfs & IA64_PFS_PFM_MASK,
                             (pfs & IA64_CFM_SOL_MASK) >>
                             IA64_CFM_SOL_SHIFT);
}

void ia64_rse_br_ret(CPUIA64State *env, uint32_t b_reg)
{
    uint64_t pfs = env->ar_pfs;
    uint64_t target = env->br[b_reg];
    uint8_t ppl = (pfs & IA64_PFS_PPL_MASK) >> IA64_PFS_PPL_SHIFT;

    /*
     * Commit the branch target (slot 0) and demoted privilege level
     * before the frame restore: mandatory RSE load faults are
     * delivered on the target instruction (SDM Vol.2 6.6).
     */
    env->ip = ia64_ip_bundle_addr(target);
    env->psr &= ~IA64_PSR_RI_MASK;
    if (ia64_psr_cpl(env->psr) < ppl) {
        ia64_set_psr(env, (env->psr & ~IA64_PSR_CPL_MASK) |
                          ((uint64_t)ppl << IA64_PSR_CPL_SHIFT));
    }
    ia64_rse_pop_return_frame(env, pfs);
    IA64_TRACE_RSE_STATE(env, "br.ret");
}

void ia64_rse_alloc(CPUIA64State *env, uint32_t r1, uint32_t pfm,
                    uint64_t fault_ip, uint32_t slot, uintptr_t ra)
{
    uint32_t old_sof = env->cfm_sof;
    uint32_t new_sof = pfm & 0x7f;
    uint32_t new_sol = (pfm >> 7) & 0x7f;
    uint32_t new_sor = (pfm >> 14) & 0x0f;
    int32_t growth = (int32_t)new_sof - (int32_t)env->cfm_sof;
    /*
     * SDM Vol.2 6.6: alloc raises a Reserved Register/Field fault when
     * it changes the rotating-region size while any RRB is non-zero.
     * The RRBs themselves are not modified by alloc.
     */
    if (new_sor != env->cfm_sor &&
        (env->cfm_rrb_gr || env->cfm_rrb_fr || env->cfm_rrb_pr)) {
        env->cr_isr = 0x30;
        ia64_raise_exception(env, IA64_EXCP_RESERVED_REG_FIELD,
                               fault_ip, 0, slot);
    }

    ia64_rse_sync_frame_out(env);
    ia64_rse_new_frame(env, growth, ra);
    env->cfm_sof = new_sof;
    env->cfm_sol = new_sol;
    env->cfm_sor = new_sor;
    if (new_sof > old_sof) {
        ia64_rse_sync_frame_in_range(env, old_sof, new_sof - old_sof);
    }
    ia64_invalidate_stacked_alat(env);

    if (r1 != 0) {
        env->gr[r1] = env->ar_pfs;
        ia64_gr_nat_set(env, r1, false);
        ia64_rse_mark_gr_dirty(env, r1);
    }
    ia64_rse_check(env, "alloc");
    IA64_TRACE_RSE_STATE(env, "alloc");
}

void ia64_rse_cover(CPUIA64State *env)
{
    if (!(env->psr & IA64_PSR_IC)) {
        env->cr_ifs = IA64_IFS_V | ia64_rse_current_cfm(env);
    }
    ia64_rse_sync_frame_out(env);
    ia64_rse_preserve_frame(env, env->cfm_sof);
    env->cfm_sof = 0;
    env->cfm_sol = 0;
    env->cfm_sor = 0;
    env->cfm_rrb_gr = 0;
    ia64_set_cfm_rrb_fr(env, 0);
    env->cfm_rrb_pr = 0;
    ia64_invalidate_stacked_alat(env);
    ia64_rse_check(env, "cover");
    IA64_TRACE_RSE_STATE(env, "cover");
}

void ia64_rse_flush(CPUIA64State *env, uintptr_t ra)
{

    /*
     * Spill every dirty register and intervening NaT collection
     * (SDM Vol.2 6.5.4).  Each completed store updates BSPSTORE and
     * the partitions, so a faulting store restarts cleanly on the
     * issuing instruction.
     */
    while (env->rse.rse_dirty + env->rse.rse_dirty_nat > 0) {
        ia64_rse_store_one(env, ra);
    }
    ia64_rse_check(env, "flushrs");
    IA64_TRACE_RSE_STATE(env, "flushrs");
}

void ia64_rse_load(CPUIA64State *env, uint64_t fault_ip, uint64_t raw,
                   uint32_t slot, uintptr_t ra)
{
    uint64_t loadrs_bytes = ((env->ar_rsc >> IA64_RSC_LOADRS_SHIFT) &
                             IA64_RSC_LOADRS_MASK) & ~7ULL;
    int32_t words = loadrs_bytes >> 3;
    int32_t words_to_load;

    if ((env->ar_rsc & IA64_RSC_MODE) != 0 ||
        (env->cfm_sof != 0 && loadrs_bytes != 0)) {
        ia64_raise_exception(env, IA64_EXCP_ILLEGAL, fault_ip, raw, slot);
    }

    /*
     * Discard the clean partition up front rather than promoting it to
     * dirty: promotion would reuse the physical copies without a memory
     * read, which is observable when software modified the backing
     * store under them (see ia64_rse_restore_frame).  Linux writes the
     * user backing store from the kernel for ptrace and signal
     * delivery, and the following kernel exit restores it with exactly
     * this loadrs; the discarded range is re-read by the load loop
     * below.
     */
    env->rse.rse_invalid += env->rse.rse_clean;
    env->rse.rse_clean = 0;
    env->rse.rse_clean_nat = 0;

    /*
     * SDM Vol.2 6.5.4: ensure the backing store between BSP and the
     * tear point is present and dirty in the physical file; everything
     * below the tear point becomes invalid.
     */
    words_to_load = words - (env->rse.rse_dirty + env->rse.rse_dirty_nat);
    if (words_to_load >= 0) {
        env->ar_bspstore = env->ar_bsp -
            (int64_t)(env->rse.rse_dirty + env->rse.rse_dirty_nat) * 8;
        while (words_to_load > 0) {
            int64_t live = (int64_t)env->rse.rse_clean +
                           env->rse.rse_clean_nat + env->rse.rse_dirty +
                           env->rse.rse_dirty_nat;
            uint64_t bspload = env->ar_bsp - (live + 1) * 8;

            if (env->rse.rse_dirty == IA64_STACKED_GR_COUNT &&
                ia64_rse_collect_bit(bspload) != 63) {
                /* More registers than fit in the physical file. */
                ia64_raise_exception(env, IA64_EXCP_ILLEGAL, fault_ip,
                                       raw, slot);
            }
            if (ia64_rse_load_one(env, ra)) {
                env->rse.rse_dirty++;
                env->rse.rse_clean--;
            } else {
                env->rse.rse_dirty_nat++;
                env->rse.rse_clean_nat--;
            }
            env->ar_bspstore = env->ar_bsp -
                (int64_t)(env->rse.rse_dirty + env->rse.rse_dirty_nat) * 8;
            words_to_load--;
        }
    } else {
        uint64_t tear = env->ar_bsp - loadrs_bytes;

        env->rse.rse_dirty_nat = (int32_t)((int64_t)(env->ar_bsp >> 9) -
                                       (int64_t)(tear >> 9));
        env->rse.rse_dirty = words - env->rse.rse_dirty_nat;
        env->ar_bspstore = env->ar_bsp -
            (int64_t)(env->rse.rse_dirty + env->rse.rse_dirty_nat) * 8;
        env->rse.rse_clean = 0;
        env->rse.rse_clean_nat = 0;
        env->rse.rse_invalid = IA64_STACKED_GR_COUNT -
                           (env->cfm_sof + env->rse.rse_dirty);
    }
    /*
     * SDM Vol.2 6.5.4 leaves AR.RNAT undefined after loadrs; the Itanium
     * processor preserves it and Windows depends on that: KiFlushRse
     * (WSRV03 miscs.s, flushrs; loadrs with RSC.loadrs = 0; br.ret)
     * neither saves nor restores RNAT, yet the mandatory reloads after
     * the invalidation take the top partial collection group's NaT bits
     * from the accumulation flushrs left in AR.RNAT.  Zeroing it here
     * destroyed those NaT bits (and applied stale ones after later
     * accumulation), which surfaced in SMP Windows guests as spurious
     * register NaT consumption in registry code (REGISTRY_ERROR 0x51
     * with STATUS_REG_NAT_CONSUMPTION) and random access violations.
     */
    ia64_rse_check(env, "loadrs");
    IA64_TRACE_RSE_STATE(env, "loadrs");
}

/* ---- Loop branch helpers ---- */

uint64_t ia64_rse_br_cexit(CPUIA64State *env, uint64_t target, uint32_t b_reg)
{
    uint64_t lc = env->ar_lc;
    uint64_t ec = env->ar_ec;
    bool active = lc != 0 || ec > 1;

    if (lc != 0) {
        env->ar_lc = lc - 1;
        env->pr[IA64_PR_LAST] = 1;
        ia64_rotate_loop_regs(env);
    } else if (ec != 0) {
        env->ar_ec = ec - 1;
        env->pr[IA64_PR_LAST] = 0;
        ia64_rotate_loop_regs(env);
    } else {
        env->pr[IA64_PR_LAST] = 0;
    }

    return active ? 0 : ((b_reg == 0) ? target : env->br[b_reg]);
}

uint64_t ia64_rse_br_ctop(CPUIA64State *env, uint64_t target, uint32_t b_reg)
{
    uint64_t lc = env->ar_lc;
    uint64_t ec = env->ar_ec;
    bool active = lc != 0 || ec > 1;

    if (lc != 0) {
        env->ar_lc = lc - 1;
        env->pr[IA64_PR_LAST] = 1;
        ia64_rotate_loop_regs(env);
    } else if (ec != 0) {
        env->ar_ec = ec - 1;
        env->pr[IA64_PR_LAST] = 0;
        ia64_rotate_loop_regs(env);
    } else {
        env->pr[IA64_PR_LAST] = 0;
    }

    return active ? ((b_reg == 0) ? target : env->br[b_reg]) : 0;
}

static bool ia64_update_while_loop(CPUIA64State *env, uint32_t qp)
{
    bool kernel_active = env->pr[qp & 63];
    bool pipeline_active = kernel_active || env->ar_ec > 1;

    if (kernel_active) {
        env->pr[IA64_PR_LAST] = 0;
        ia64_rotate_loop_regs(env);
    } else if (env->ar_ec != 0) {
        env->ar_ec--;
        env->pr[IA64_PR_LAST] = 0;
        ia64_rotate_loop_regs(env);
    } else {
        env->pr[IA64_PR_LAST] = 0;
    }

    return pipeline_active;
}

uint64_t ia64_rse_br_wexit(CPUIA64State *env, uint64_t target, uint32_t qp)
{
    return ia64_update_while_loop(env, qp) ? 0 : target;
}

uint64_t ia64_rse_br_wtop(CPUIA64State *env, uint64_t target, uint32_t qp)
{
    return ia64_update_while_loop(env, qp) ? target : 0;
}

void ia64_rse_clrrrb(CPUIA64State *env, uint32_t predicate_only)
{
    /*
     * clrrrb resets the rename bases (SDM Vol.2 table 6-2 notes; Vol.3
     * clrrrb).  The stacked physical registers do not move, so the
     * virtual view is re-derived under the new mapping.
     */
    ia64_rse_sync_frame_out(env);
    if (predicate_only) {
        env->cfm_rrb_pr = 0;
    } else {
        env->cfm_rrb_gr = 0;
        ia64_set_cfm_rrb_fr(env, 0);
        env->cfm_rrb_pr = 0;
    }
    ia64_rse_sync_frame_in(env);
    ia64_invalidate_stacked_alat(env);
    ia64_rse_check(env, "clrrrb");
}


uint64_t ia64_rse_cloop_zero_st1(CPUIA64State *env, uint32_t base_reg,
                                 uint32_t mmu_idx, uint32_t max_stores,
                                 uintptr_t ra)
{
    uint64_t lc = env->ar[IA64_AR_LC];
    uint64_t done = 0;
    uint64_t limit = MIN(lc, (uint64_t)max_stores);

    if (lc == 0 || limit == 0) {
        return 0;
    }

    while (done < limit) {
        uint64_t addr = env->gr[base_reg];
        uint64_t page_left = TARGET_PAGE_SIZE - (addr & (TARGET_PAGE_SIZE - 1));
        uint64_t span = MIN(limit - done, page_left);
        void *host = NULL;

        /*
         * The helper is called from br.cloop after the current loop-body store.
         * Each future store is reached by a taken branch first, so LC has
         * already been decremented before a faulting store is observed.
         */
        env->ar[IA64_AR_LC] = lc - done - 1;
        env->gr[base_reg] = addr;

        if (ia64_exec_probe_host(env, addr, (int)span, MMU_DATA_STORE,
                                 mmu_idx, &host, ra)) {
            memset(host, 0, span);
            ia64_invalidate_alat_store(env, addr, (uint32_t)span);
            env->gr[base_reg] = addr + span;
            done += span;
            env->ar[IA64_AR_LC] = lc - done;
            continue;
        }

        ia64_exec_store_mmuidx(env, addr, 0, 1, false, mmu_idx, ra);
        ia64_invalidate_alat_store(env, addr, 1);
        env->gr[base_reg] = addr + 1;
        done++;
        env->ar[IA64_AR_LC] = lc - done;
    }

    if (done < lc) {
        env->ar[IA64_AR_LC] = lc - done - 1;
        return 1;
    }
    return 0;
}
