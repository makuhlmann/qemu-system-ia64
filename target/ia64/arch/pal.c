/*
 * IA-64 Processor Abstraction Layer services.
 *
 * PAL calls update architected CPU state and return through the firmware
 * portal.  The helper ABI supplies only the translated-code return address.
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "cpu.h"
#include "arch/arch.h"
#include "exec-access.h"
#include "exec/cpu-common.h"
#include "exec/cputlb.h"
#include "exec/tb-flush.h"
#include "exec/translation-block.h"
#include "trace.h"

/*
 * PAL function indices (Intel IA-64 PAL specification).
 * GR28 holds the function index on entry.
 * Results are returned in GR8 (status), GR9-GR11 (outputs).
 */
#define PAL_CACHE_FLUSH     0x0001
#define PAL_CACHE_INFO      0x0002
#define PAL_CACHE_INIT      0x0003
#define PAL_CACHE_SUMMARY   0x0004
#define PAL_MEM_ATTRIB      0x0005
#define PAL_PTCE_INFO       0x0006
#define PAL_VM_INFO         0x0007
#define PAL_VM_SUMMARY      0x0008
#define PAL_BUS_GET_FEATURES 0x0009
#define PAL_BUS_SET_FEATURES 0x000A
#define PAL_DEBUG_INFO      0x000B
#define PAL_FIXED_ADDR      0x000C
#define PAL_FREQ_BASE       0x000D
#define PAL_FREQ_RATIOS     0x000E
#define PAL_PERF_MON_INFO   0x000F
#define PAL_PLATFORM_ADDR   0x0010
#define PAL_PROC_GET_FEATURES 0x0011
#define PAL_PROC_SET_FEATURES 0x0012
#define PAL_RSE_INFO        0x0013
#define PAL_VERSION         0x0014
#define PAL_MC_CLEAR_LOG    0x0015
#define PAL_MC_DRAIN        0x0016
#define PAL_MC_EXPECTED     0x0017
#define PAL_MC_DYNAMIC_STATE 0x0018
#define PAL_MC_ERROR_INFO   0x0019
#define PAL_MC_RESUME       0x001A
#define PAL_MC_REGISTER_MEM 0x001B
#define PAL_HALT            0x001C
#define PAL_HALT_LIGHT      0x001D
#define PAL_COPY_INFO       0x001E
#define PAL_CACHE_LINE_INIT 0x001F
#define PAL_PMI_ENTRYPOINT  0x0020
#define PAL_VM_PAGE_SIZE    0x0022
#define PAL_MEM_FOR_TEST    0x0025
#define PAL_CACHE_PROT_INFO 0x0026
#define PAL_REGISTER_INFO   0x0027
#define PAL_PREFETCH_VIS    0x0029
#define PAL_LOGICAL_TO_PHYSICAL 0x002A
#define PAL_CACHE_SHARED_INFO 0x002B

#define PAL_COPY_PAL       0x0100
#define PAL_HALT_INFO       0x0101
#define PAL_TEST_PROC       0x0102
#define PAL_VM_TR_READ      0x0105
#define PAL_BRAND_INFO      0x0112

#define PAL_COPY_BUFFER_SIZE  0x1000ULL
#define PAL_COPY_BUFFER_ALIGN 0x1000ULL
#define PAL_COPY_PROC_OFFSET  0
#define PAL_COPY_CODE_SIZE    0x20ULL
#define PAL_COPY_TARGET_CACHE_ATTR (1ULL << 63)
#define PAL_SELF_TEST_STATE_TESTED (1ULL << 2)
#define PAL_MEM_ATTR_WB            (1ULL << 0)
#define PAL_MEM_ATTR_VALID_MASK    0xffffULL
#define PAL_PERF_MON_INFO_VALUE    0x08123004ULL
#define PAL_PERF_PMC_MASK          0x3fffULL
#define PAL_PERF_PMD_MASK          0x3ffffULL
#define PAL_PERF_GENERIC_MASK      0xf0ULL

#define PAL_CACHE_FLUSH_OPERATION_MASK 0x3ULL
#define PAL_HALT_STATE_COUNT       8
#define PAL_HALT_STATE_IMPLEMENTED (1ULL << 60)
#define PAL_HALT_STATE_COHERENT    (1ULL << 61)
#define PAL_HALT_IO_TYPE_NONE      0
#define PAL_HALT_IO_TYPE_LOAD      1
#define PAL_HALT_IO_TYPE_STORE     2
#define PAL_HALT_IO_PHYS_ADDR_MASK (~(1ULL << 63))
#define IA64_L0_CACHE_LINE_SIZE    64ULL
#define IA64_L1_CACHE_LINE_SIZE    128ULL
#define IA64_L2_CACHE_LINE_SIZE    128ULL
#define IA64_MONTECITO_PACKAGE_CACHE_SIZE (24ULL * MiB)
#define IA64_MONTECITO_FREQUENCY   1600000000ULL
#define IA64_MONTECITO_BUS_FREQUENCY 533333333ULL

static bool pal_reserved_args_are_zero(CPUIA64State *env);

static uint64_t pal_stacked_arg(CPUIA64State *env, uint32_t arg)
{
    return env->gr[IA64_STACKED_GR_BASE + 1 + arg];
}

#define PAL_STATUS_SUCCESS         0
#define PAL_STATUS_NOT_IMPLEMENTED (-1)
#define PAL_STATUS_INVALID_ARGUMENT (-2)
#define PAL_STATUS_ERROR           (-3)
#define PAL_STATUS_NO_INFORMATION  (-6)
#define PAL_STATUS_BEYOND_MAX      (-8)
/*
 * An implementation-specific index (SDM Vol. 2 table 11-12: 512-767).  The HP
 * zx1 SAL_B sets a processor response timeout through it and ends the boot
 * with "cell halt" when it fails.  Nothing here has a bus timeout to set.
 */
#define PAL_IMPL_PROC_RESPONSE_TIMEOUT 0x213
#define PAL_STATUS_NEXT_HIGHER     1

static void pal_get_version(CPUIA64State *env)
{
    const IA64PalProfile *pal = ia64_env_cpu_class(env)->pal;

    if (pal_reserved_args_are_zero(env)) {
        /*
         * SDM Vol. 2 figure 11-37: PAL_B_version{15:0}, PAL_vendor{31:24},
         * PAL_A_version{47:32}.  Both the minimum and the current version
         * report the same firmware; this model has only one.
         */
        env->gr[IA64_PAL_GR_STATUS] = PAL_STATUS_SUCCESS;
        env->gr[IA64_PAL_GR_RESULT1] =
            ((uint64_t)pal->pal_a_model << 40) |
            ((uint64_t)pal->pal_a_revision << 32) |
            ((uint64_t)pal->pal_vendor << 24) |
            ((uint64_t)pal->pal_b_model << 8) |
            (uint64_t)pal->pal_b_revision;
        env->gr[IA64_PAL_GR_RESULT2] = env->gr[IA64_PAL_GR_RESULT1];
    } else {
        env->gr[IA64_PAL_GR_STATUS] = PAL_STATUS_INVALID_ARGUMENT;
        env->gr[IA64_PAL_GR_RESULT1] = 0;
        env->gr[IA64_PAL_GR_RESULT2] = 0;
    }
    env->gr[IA64_PAL_GR_RESULT3] = 0;
}

static void pal_rse_info(CPUIA64State *env)
{
    if (pal_reserved_args_are_zero(env)) {
        env->gr[IA64_PAL_GR_STATUS] = PAL_STATUS_SUCCESS;
        env->gr[IA64_PAL_GR_RESULT1] = 96;
        /*
         * hints is a 2-bit field ({1:0} = si/li, SDM Vol.2 rev 1.1
         * Fig. 11-36); everything above it is reserved.  Neither supported
         * generation implements the eager RSE modes, so the answer is 0.
         * (The previous value, 16, sat entirely in reserved bits.)
         */
        env->gr[IA64_PAL_GR_RESULT2] = 0;
    } else {
        env->gr[IA64_PAL_GR_STATUS] = PAL_STATUS_INVALID_ARGUMENT;
        env->gr[IA64_PAL_GR_RESULT1] = 0;
        env->gr[IA64_PAL_GR_RESULT2] = 0;
    }
    env->gr[IA64_PAL_GR_RESULT3] = 0;
}

static void pal_vm_summary(CPUIA64State *env)
{
    const IA64PalProfile *pal = ia64_env_cpu_class(env)->pal;
    uint64_t itr_count = ia64_env_cpu_class(env)->itr_count;
    uint64_t dtr_count = ia64_env_cpu_class(env)->dtr_count;

    g_assert(itr_count > 0 && itr_count <= IA64_TR_MAX);
    g_assert(dtr_count > 0 && dtr_count <= IA64_TR_MAX);
    if (pal_reserved_args_are_zero(env)) {
        env->gr[IA64_PAL_GR_STATUS] = PAL_STATUS_SUCCESS;
        /*
         * vm_info_1 field placement follows SDM Vol. 2 figure 11-39:
         * vw{0}, phys_add_size{7:1}, key_size{15:8}, max_pkr{23:16},
         * hash_tag_id{31:24}, max_dtr_entry{39:32}, max_itr_entry{47:40},
         * num_unique_tcs{55:48}, num_tc_levels{63:56}.  The data field comes
         * first: the two were transposed here, which stayed invisible while
         * every model had 64 of each and became live with the original
         * Itanium's asymmetric 8 ITR / 48 DTR file.
         */
        env->gr[IA64_PAL_GR_RESULT1] = 1ULL |
                     ((uint64_t)env->impl_pa_bits << 1) |
                     ((uint64_t)env->impl_key_bits << 8) |
                     (((uint64_t)IA64_PKR_COUNT - 1ULL) << 16) |
                     (8ULL << 24) |
                     ((dtr_count - 1ULL) << 32) |
                     ((itr_count - 1ULL) << 40) |
                     ((uint64_t)pal->unique_tcs << 48) |
                     ((uint64_t)pal->tc_levels << 56);
        env->gr[IA64_PAL_GR_RESULT2] = (uint64_t)env->impl_va_msb |
                      ((uint64_t)env->impl_rid_bits << 8);
    } else {
        env->gr[IA64_PAL_GR_STATUS] = PAL_STATUS_INVALID_ARGUMENT;
        env->gr[IA64_PAL_GR_RESULT1] = 0;
        env->gr[IA64_PAL_GR_RESULT2] = 0;
    }
    env->gr[IA64_PAL_GR_RESULT3] = 0;
}

static bool pal_halt_light(CPUIA64State *env)
{
    CPUState *cs = env_cpu(env);

    env->gr[IA64_PAL_GR_STATUS] = PAL_STATUS_SUCCESS;
    /*
     * PAL defines an unmasked external interrupt using TPR.mic/TPR.mmi,
     * independently of PSR.i.  Static PAL call wrappers mask PSR.i while
     * entering physical PAL and restore the caller state after this call
     * resumes, so remember that this halt has PAL wake semantics.
     */
    env->interrupt.pal_halt_wake = true;
    ia64_itc_enter_halt(env);
    /*
     * Leave cpu_exec only after the PAL break instruction has completed and
     * the translator has committed its continuation IP.  Setting halted here
     * merely ended the current translation block; cpu_exec could then run on
     * with a stale halted flag and stop later at an unrelated instruction.
     */
    cpu_set_interrupt(cs, CPU_INTERRUPT_HALT);
    return true;
}

static bool pal_halt_valid_io_size(uint64_t io_size)
{
    return io_size == 1 || io_size == 2 || io_size == 4 || io_size == 8;
}

static bool pal_halt_io_transaction(uint64_t io_detail_ptr,
                                    uint64_t *load_return)
{
    uint64_t info;
    uint64_t io_type;
    uint64_t io_size;
    uint64_t addr;
    uint64_t data;
    uint64_t phys_addr;

    *load_return = 0;
    if (io_detail_ptr == 0) {
        return true;
    }
    if ((io_detail_ptr & 7) != 0) {
        return false;
    }

    (void)ia64_exec_physical_rw(io_detail_ptr, &info, sizeof(info), false);
    info = le64_to_cpu(info);
    io_type = info & 0xff;
    io_size = (info >> 8) & 0xff;
    if (io_type == PAL_HALT_IO_TYPE_NONE) {
        return io_size == 0;
    }
    if ((io_type != PAL_HALT_IO_TYPE_LOAD &&
         io_type != PAL_HALT_IO_TYPE_STORE) ||
        !pal_halt_valid_io_size(io_size)) {
        return false;
    }

    (void)ia64_exec_physical_rw(io_detail_ptr + 8, &addr, sizeof(addr),
                                false);
    addr = le64_to_cpu(addr);
    phys_addr = addr & PAL_HALT_IO_PHYS_ADDR_MASK;
    if ((phys_addr & (io_size - 1)) != 0) {
        return false;
    }

    if (io_type == PAL_HALT_IO_TYPE_LOAD) {
        uint8_t buf[8] = { 0 };
        uint64_t value = 0;
        int i;

        (void)ia64_exec_physical_rw(phys_addr, buf, io_size, false);
        for (i = 0; i < io_size; i++) {
            value |= (uint64_t)buf[i] << (i * 8);
        }
        *load_return = value;
    } else {
        uint8_t buf[8];
        uint64_t store_value;
        int i;

        (void)ia64_exec_physical_rw(io_detail_ptr + 16, &data,
                                    sizeof(data), false);
        store_value = le64_to_cpu(data);
        for (i = 0; i < io_size; i++) {
            buf[i] = store_value >> (i * 8);
        }
        (void)ia64_exec_physical_rw(phys_addr, buf, io_size, true);
    }
    return true;
}

static bool pal_halt(CPUIA64State *env)
{
    CPUState *cs = env_cpu(env);
    uint64_t halt_state = env->gr[IA64_PAL_GR_ARG1];
    uint64_t io_detail_ptr = env->gr[IA64_PAL_GR_ARG2];
    uint64_t load_return = 0;

    if (halt_state != 1 || env->gr[IA64_PAL_GR_ARG3] != 0 ||
        !pal_halt_io_transaction(io_detail_ptr, &load_return)) {
        env->gr[IA64_PAL_GR_STATUS] = PAL_STATUS_INVALID_ARGUMENT;
        env->gr[IA64_PAL_GR_RESULT1] = 0;
        env->gr[IA64_PAL_GR_RESULT2] = 0;
        env->gr[IA64_PAL_GR_RESULT3] = 0;
        return false;
    }

    env->gr[IA64_PAL_GR_STATUS] = PAL_STATUS_SUCCESS;
    env->gr[IA64_PAL_GR_RESULT1] = load_return;
    env->gr[IA64_PAL_GR_RESULT2] = 0;
    env->gr[IA64_PAL_GR_RESULT3] = 0;
    env->interrupt.pal_halt_wake = true;
    ia64_itc_enter_halt(env);
    cpu_set_interrupt(cs, CPU_INTERRUPT_HALT);
    return true;
}

static void pal_prefetch_vis(CPUIA64State *env)
{
    if (pal_reserved_args_are_zero(env)) {
        env->gr[IA64_PAL_GR_STATUS] = PAL_STATUS_SUCCESS;
        env->gr[IA64_PAL_GR_RESULT1] = (1ULL << 0) | (1ULL << 1);
    } else {
        env->gr[IA64_PAL_GR_STATUS] = PAL_STATUS_INVALID_ARGUMENT;
        env->gr[IA64_PAL_GR_RESULT1] = 0;
    }
    env->gr[IA64_PAL_GR_RESULT2] = 0;
    env->gr[IA64_PAL_GR_RESULT3] = 0;
}

static bool pal_cache_flush(CPUIA64State *env)
{
    uint64_t cache_type = env->gr[IA64_PAL_GR_ARG1];
    uint64_t operation = env->gr[IA64_PAL_GR_ARG2];

    if (cache_type < 1 || cache_type > 4 ||
        (operation & ~PAL_CACHE_FLUSH_OPERATION_MASK) != 0) {
        env->gr[IA64_PAL_GR_STATUS] = PAL_STATUS_INVALID_ARGUMENT;
    } else {
        env->gr[IA64_PAL_GR_STATUS] = PAL_STATUS_SUCCESS;
        if (cache_type == 1 || cache_type == 3 || cache_type == 4) {
            queue_tb_flush(env_cpu(env));
            env->gr[IA64_PAL_GR_RESULT1] = 0;
            env->gr[IA64_PAL_GR_RESULT2] = 0;
            env->gr[IA64_PAL_GR_RESULT3] = 0;
            return true;
        }
    }
    env->gr[IA64_PAL_GR_RESULT1] = 0;
    env->gr[IA64_PAL_GR_RESULT2] = 0;
    env->gr[IA64_PAL_GR_RESULT3] = 0;
    return false;
}

static void pal_cache_init(CPUIA64State *env)
{
    uint64_t level = env->gr[IA64_PAL_GR_ARG1];
    uint64_t cache_type = env->gr[IA64_PAL_GR_ARG2];
    uint64_t restrict_side_effects = env->gr[IA64_PAL_GR_ARG3];

    if (level != UINT64_MAX &&
        (level >= 3 || cache_type < 1 || cache_type > 3 ||
         restrict_side_effects > 1)) {
        env->gr[IA64_PAL_GR_STATUS] = PAL_STATUS_INVALID_ARGUMENT;
    } else {
        env->gr[IA64_PAL_GR_STATUS] = PAL_STATUS_SUCCESS;
    }
    env->gr[IA64_PAL_GR_RESULT1] = 0;
    env->gr[IA64_PAL_GR_RESULT2] = 0;
    env->gr[IA64_PAL_GR_RESULT3] = 0;
}

static void pal_mem_attrib(CPUIA64State *env)
{
    if (pal_reserved_args_are_zero(env)) {
        env->gr[IA64_PAL_GR_STATUS] = PAL_STATUS_SUCCESS;
        env->gr[IA64_PAL_GR_RESULT1] =
            ia64_env_cpu_class(env)->pal->memory_attributes;
    } else {
        env->gr[IA64_PAL_GR_STATUS] = PAL_STATUS_INVALID_ARGUMENT;
        env->gr[IA64_PAL_GR_RESULT1] = 0;
    }
    env->gr[IA64_PAL_GR_RESULT2] = 0;
    env->gr[IA64_PAL_GR_RESULT3] = 0;
}

static void pal_vm_page_size(CPUIA64State *env)
{
    if (pal_reserved_args_are_zero(env)) {
        env->gr[IA64_PAL_GR_STATUS] = PAL_STATUS_SUCCESS;
        env->gr[IA64_PAL_GR_RESULT1] = env->insertable_page_mask;
        env->gr[IA64_PAL_GR_RESULT2] = env->purgeable_page_mask;
    } else {
        env->gr[IA64_PAL_GR_STATUS] = PAL_STATUS_INVALID_ARGUMENT;
        env->gr[IA64_PAL_GR_RESULT1] = 0;
        env->gr[IA64_PAL_GR_RESULT2] = 0;
    }
    env->gr[IA64_PAL_GR_RESULT3] = 0;
}

/*
 * PAL_CACHE_INFO and PAL_VM_INFO both index a (level, type) pair, where
 * type 1 is the instruction side and type 2 the data or unified side.  The
 * geometry differs per processor generation, so it lives in the model's PAL
 * profile; a pair the processor does not implement is left zeroed there and
 * reported as an invalid argument.
 */
/* Cache tags cover the whole implemented physical address. */
static uint8_t pal_cache_tag_msb(const CPUIA64State *env)
{
    return env->impl_pa_bits - 1;
}

static const IA64PalCacheLevel *
pal_cache_level_for_model(CPUIA64State *env, uint64_t level, uint64_t type)
{
    const IA64PalProfile *pal = ia64_env_cpu_class(env)->pal;
    const IA64PalCacheLevel *entry;

    if (type < 1 || type > IA64_PAL_CACHE_TYPES ||
        level >= IA64_PAL_CACHE_LEVELS || level >= pal->cache_levels) {
        return NULL;
    }

    entry = &pal->cache[level][type - 1];
    return entry->size != 0 ? entry : NULL;
}

static const IA64PalTcLevel *
pal_tc_level_for_model(CPUIA64State *env, uint64_t level, uint64_t type)
{
    const IA64PalProfile *pal = ia64_env_cpu_class(env)->pal;
    const IA64PalTcLevel *entry;

    if (type < 1 || type > IA64_PAL_CACHE_TYPES ||
        level >= IA64_PAL_CACHE_LEVELS || level >= pal->tc_levels) {
        return NULL;
    }

    entry = &pal->tc[level][type - 1];
    return entry->num_entries != 0 ? entry : NULL;
}

static void pal_cache_summary(CPUIA64State *env)
{
    const IA64PalProfile *pal = ia64_env_cpu_class(env)->pal;

    if (pal_reserved_args_are_zero(env)) {
        env->gr[IA64_PAL_GR_STATUS] = PAL_STATUS_SUCCESS;
        env->gr[IA64_PAL_GR_RESULT1] = pal->cache_levels;
        env->gr[IA64_PAL_GR_RESULT2] = pal->unique_caches;
    } else {
        env->gr[IA64_PAL_GR_STATUS] = PAL_STATUS_INVALID_ARGUMENT;
        env->gr[IA64_PAL_GR_RESULT1] = 0;
        env->gr[IA64_PAL_GR_RESULT2] = 0;
    }
    env->gr[IA64_PAL_GR_RESULT3] = 0;
}

static void pal_copy_info(CPUIA64State *env)
{
    uint64_t copy_type = env->gr[IA64_PAL_GR_ARG1];
    uint64_t platform_info = env->gr[IA64_PAL_GR_ARG2];

    if (copy_type == 0 && platform_info == 0) {
        env->gr[IA64_PAL_GR_STATUS] = PAL_STATUS_SUCCESS;
        env->gr[IA64_PAL_GR_RESULT1] = PAL_COPY_BUFFER_SIZE;
        env->gr[IA64_PAL_GR_RESULT2] = PAL_COPY_BUFFER_ALIGN;
    } else if (copy_type == 1) {
        env->gr[IA64_PAL_GR_STATUS] = PAL_STATUS_ERROR;
        env->gr[IA64_PAL_GR_RESULT1] = 0;
        env->gr[IA64_PAL_GR_RESULT2] = 0;
    } else {
        env->gr[IA64_PAL_GR_STATUS] = PAL_STATUS_INVALID_ARGUMENT;
        env->gr[IA64_PAL_GR_RESULT1] = 0;
        env->gr[IA64_PAL_GR_RESULT2] = 0;
    }
    env->gr[IA64_PAL_GR_RESULT3] = 0;
}

static void pal_copy_pal(CPUIA64State *env)
{
    /*
     * The relocated PAL procedure entry: break.m 0x100000 ;; br.many b0,
     * the same stub as the machine's PAL emulation ROM.  The return branch is br.many (a plain branch, no register-stack
     * pop), NOT br.ret: the PAL static-procedure convention runs in the
     * caller's frame without an alloc, so the caller reaches PAL_PROC by a
     * plain branch with the return address in b0.  A br.ret here would pop a
     * frame that was never pushed and corrupt the caller's stacked
     * registers (observed with bios130.BIN, whose SAL_B at 0xFFE797E0
     * reaches the relocated entry via br.few b5; 64703dd).
     */
    static const uint64_t pal_proc_words[] = {
        0x000002000000000aULL,
        0x0004000000000200ULL,
        0x0000000100000011ULL,
        0x0080000800000200ULL,
    };
    uint64_t target_addr = pal_stacked_arg(env, 0);
    uint64_t alloc_size = pal_stacked_arg(env, 1);
    uint64_t processor = pal_stacked_arg(env, 2);
    uint64_t target_pa = target_addr & ~PAL_COPY_TARGET_CACHE_ATTR;

    if (processor > 1 ||
        alloc_size < PAL_COPY_BUFFER_SIZE ||
        (target_pa & (PAL_COPY_BUFFER_ALIGN - 1)) != 0 ||
        target_pa > UINT64_MAX - PAL_COPY_CODE_SIZE) {
        env->gr[IA64_PAL_GR_STATUS] = PAL_STATUS_INVALID_ARGUMENT;
        env->gr[IA64_PAL_GR_RESULT1] = 0;
        env->gr[IA64_PAL_GR_RESULT2] = 0;
        env->gr[IA64_PAL_GR_RESULT3] = 0;
        return;
    }

    if (processor == 0) {
        uint64_t le_words[ARRAY_SIZE(pal_proc_words)];
        int i;

        for (i = 0; i < ARRAY_SIZE(pal_proc_words); i++) {
            le_words[i] = cpu_to_le64(pal_proc_words[i]);
        }
        (void)ia64_exec_physical_rw(target_pa, le_words,
                                    sizeof(le_words), true);
        ia64_exec_invalidate_phys_range(env, target_pa, PAL_COPY_CODE_SIZE);
    }

    /*
     * An application-processor call does not repeat the memory copy, but it
     * still installs the relocated procedure and PMI entry points in that
     * processor (SDM Vol. 2, PAL_COPY_PAL).  Keep this state per CPU so a
     * subsequent break in the shared PAL image is dispatched as a PAL call.
     */
    env->pal.pal_proc_copy_addr = target_pa + PAL_COPY_PROC_OFFSET;
    env->pal.pal_proc_copy_valid = true;
    env->pal.pal_pmi_entry = target_pa + PAL_COPY_PROC_OFFSET;

    env->gr[IA64_PAL_GR_STATUS] = PAL_STATUS_SUCCESS;
    env->gr[IA64_PAL_GR_RESULT1] = PAL_COPY_PROC_OFFSET;
    env->gr[IA64_PAL_GR_RESULT2] = 0;
    env->gr[IA64_PAL_GR_RESULT3] = 0;
}

static void pal_brand_info(CPUIA64State *env, uintptr_t ra)
{
    static const char montecito_brand[] =
        "QEMU Montecito-compatible IA-64 CPU 1.60GHz 24MB";
    static const char madison_brand[] =
        "QEMU Madison-compatible IA-64 CPU";
    bool montecito = ia64_env_cpu_class(env)->is_montecito;
    uint64_t request = pal_stacked_arg(env, 0);
    uint64_t address = pal_stacked_arg(env, 1);
    uint64_t reserved = pal_stacked_arg(env, 2);
    const char *brand = montecito ? montecito_brand : madison_brand;
    size_t length;
    size_t i;

    env->gr[IA64_PAL_GR_RESULT1] = 0;
    env->gr[IA64_PAL_GR_RESULT2] = 0;
    env->gr[IA64_PAL_GR_RESULT3] = 0;

    if (reserved != 0) {
        env->gr[IA64_PAL_GR_STATUS] = PAL_STATUS_INVALID_ARGUMENT;
        return;
    }

    switch (request) {
    case 0:
        if (address == 0) {
            env->gr[IA64_PAL_GR_STATUS] = PAL_STATUS_INVALID_ARGUMENT;
            return;
        }
        length = strlen(brand);
        for (i = 0; i <= length; i++) {
            ia64_exec_store_data(env, address + i, brand[i], 1, false, ra);
        }
        env->gr[IA64_PAL_GR_STATUS] = PAL_STATUS_SUCCESS;
        env->gr[IA64_PAL_GR_RESULT1] = length;
        break;
    case 16:
        env->gr[IA64_PAL_GR_STATUS] = montecito ? PAL_STATUS_SUCCESS :
                     PAL_STATUS_NO_INFORMATION;
        env->gr[IA64_PAL_GR_RESULT1] = montecito ? IA64_MONTECITO_FREQUENCY : 0;
        break;
    case 17:
        env->gr[IA64_PAL_GR_STATUS] = PAL_STATUS_SUCCESS;
        env->gr[IA64_PAL_GR_RESULT1] =
            montecito ? IA64_MONTECITO_PACKAGE_CACHE_SIZE : 3 * MiB;
        break;
    case 18:
        env->gr[IA64_PAL_GR_STATUS] = montecito ? PAL_STATUS_SUCCESS :
                     PAL_STATUS_NO_INFORMATION;
        env->gr[IA64_PAL_GR_RESULT1] =
            montecito ? IA64_MONTECITO_BUS_FREQUENCY : 0;
        break;
    default:
        env->gr[IA64_PAL_GR_STATUS] = PAL_STATUS_INVALID_ARGUMENT;
        break;
    }
}

typedef struct IA64PalTopology {
    uint32_t socket;
    uint32_t core;
    uint32_t thread;
    uint32_t cores_per_socket;
    uint32_t threads_per_core;
    uint32_t package_base;
    uint32_t package_cpus;
} IA64PalTopology;

static IA64PalTopology pal_cpu_topology(CPUIA64State *env)
{
    CPUState *cs = env_cpu(env);
    IA64CPU *cpu = ia64_cpu_from_cpu_state(cs);

    return (IA64PalTopology) {
        .socket = cpu->socket_id,
        .core = cpu->core_id,
        .thread = cpu->thread_id,
        .cores_per_socket = MAX(cpu->cores_per_socket, 1U),
        .threads_per_core = MAX(cpu->threads_per_core, 1U),
        .package_base = cpu->package_base,
        .package_cpus = MAX(cpu->package_cpus, 1U),
    };
}

static void pal_logical_to_physical(CPUIA64State *env)
{
    IA64PalTopology topology;
    int64_t requested = env->gr[IA64_PAL_GR_ARG1];
    uint32_t number;
    uint32_t target;
    uint32_t target_core;
    uint32_t target_thread;

    env->gr[IA64_PAL_GR_RESULT1] = 0;
    env->gr[IA64_PAL_GR_RESULT2] = 0;
    env->gr[IA64_PAL_GR_RESULT3] = 0;

    if (!ia64_env_cpu_class(env)->is_montecito) {
        env->gr[IA64_PAL_GR_STATUS] = PAL_STATUS_NOT_IMPLEMENTED;
        return;
    }
    if (env->gr[IA64_PAL_GR_ARG2] != 0 || env->gr[IA64_PAL_GR_ARG3] != 0) {
        env->gr[IA64_PAL_GR_STATUS] = PAL_STATUS_INVALID_ARGUMENT;
        return;
    }

    topology = pal_cpu_topology(env);
    if (requested == -1) {
        number = env_cpu(env)->cpu_index - topology.package_base;
    } else if (requested >= 0 && requested < topology.package_cpus) {
        number = requested;
    } else {
        env->gr[IA64_PAL_GR_STATUS] = PAL_STATUS_INVALID_ARGUMENT;
        return;
    }

    target = topology.package_base + number;
    target_core = (target / topology.threads_per_core) %
                  topology.cores_per_socket;
    target_thread = target % topology.threads_per_core;

    env->gr[IA64_PAL_GR_STATUS] = PAL_STATUS_SUCCESS;
    env->gr[IA64_PAL_GR_RESULT1] = topology.package_cpus |
                 ((uint64_t)topology.threads_per_core << 16) |
                 ((uint64_t)topology.cores_per_socket << 32) |
                 ((uint64_t)topology.socket << 48);
    env->gr[IA64_PAL_GR_RESULT2] =
        target_thread | ((uint64_t)target_core << 32);
    env->gr[IA64_PAL_GR_RESULT3] = target;
}

static void pal_cache_shared_info(CPUIA64State *env)
{
    IA64PalTopology topology;
    uint64_t number = env->gr[IA64_PAL_GR_ARG3];
    uint32_t core_base;
    uint32_t shared;
    uint32_t target;

    env->gr[IA64_PAL_GR_RESULT1] = 0;
    env->gr[IA64_PAL_GR_RESULT2] = 0;
    env->gr[IA64_PAL_GR_RESULT3] = 0;

    if (!ia64_env_cpu_class(env)->is_montecito) {
        env->gr[IA64_PAL_GR_STATUS] = PAL_STATUS_NOT_IMPLEMENTED;
        return;
    }
    if (!pal_cache_level_for_model(env, env->gr[IA64_PAL_GR_ARG1],
                                   env->gr[IA64_PAL_GR_ARG2])) {
        env->gr[IA64_PAL_GR_STATUS] = PAL_STATUS_INVALID_ARGUMENT;
        return;
    }

    topology = pal_cpu_topology(env);
    core_base = topology.package_base +
                topology.core * topology.threads_per_core;
    shared = MIN(topology.threads_per_core,
                 topology.package_cpus -
                 topology.core * topology.threads_per_core);
    if (number >= shared) {
        env->gr[IA64_PAL_GR_STATUS] = PAL_STATUS_INVALID_ARGUMENT;
        return;
    }

    target = core_base + number;
    env->gr[IA64_PAL_GR_STATUS] = PAL_STATUS_SUCCESS;
    env->gr[IA64_PAL_GR_RESULT1] = shared;
    env->gr[IA64_PAL_GR_RESULT2] = (target % topology.threads_per_core) |
                  ((uint64_t)topology.core << 32);
    env->gr[IA64_PAL_GR_RESULT3] = target;
}

static void pal_halt_info(CPUIA64State *env, uintptr_t ra)
{
    uint64_t power_buffer = pal_stacked_arg(env, 0);
    uint64_t reserved1 = pal_stacked_arg(env, 1);
    uint64_t reserved2 = pal_stacked_arg(env, 2);
    uint64_t power_states[PAL_HALT_STATE_COUNT] = { 0 };
    int i;

    if ((power_buffer & 7) != 0 || reserved1 != 0 || reserved2 != 0) {
        env->gr[IA64_PAL_GR_STATUS] = PAL_STATUS_INVALID_ARGUMENT;
        env->gr[IA64_PAL_GR_RESULT1] = 0;
        env->gr[IA64_PAL_GR_RESULT2] = 0;
        env->gr[IA64_PAL_GR_RESULT3] = 0;
        return;
    }

    power_states[0] = PAL_HALT_STATE_IMPLEMENTED | PAL_HALT_STATE_COHERENT |
                      (1000ULL << 32) | (1ULL << 16) | 1ULL;
    power_states[1] = PAL_HALT_STATE_IMPLEMENTED |
                      (1000ULL << 32) | (1ULL << 16) | 1ULL;

    for (i = 0; i < PAL_HALT_STATE_COUNT; i++) {
        ia64_exec_store_data(env, power_buffer + i * 8, power_states[i],
                             8, false, ra);
    }

    env->gr[IA64_PAL_GR_STATUS] = PAL_STATUS_SUCCESS;
    env->gr[IA64_PAL_GR_RESULT1] = 0;
    env->gr[IA64_PAL_GR_RESULT2] = 0;
    env->gr[IA64_PAL_GR_RESULT3] = 0;
}

static void pal_mc_drain(CPUIA64State *env)
{
    env->gr[IA64_PAL_GR_STATUS] = pal_reserved_args_are_zero(env) ?
        PAL_STATUS_SUCCESS : PAL_STATUS_INVALID_ARGUMENT;
    env->gr[IA64_PAL_GR_RESULT1] = 0;
    env->gr[IA64_PAL_GR_RESULT2] = 0;
    env->gr[IA64_PAL_GR_RESULT3] = 0;
}

static bool pal_reserved_args_are_zero(CPUIA64State *env)
{
    return env->gr[IA64_PAL_GR_ARG1] == 0 &&
           env->gr[IA64_PAL_GR_ARG2] == 0 &&
           env->gr[IA64_PAL_GR_ARG3] == 0;
}

static void pal_mc_clear_log(CPUIA64State *env)
{
    env->gr[IA64_PAL_GR_STATUS] = pal_reserved_args_are_zero(env) ?
        PAL_STATUS_SUCCESS : PAL_STATUS_INVALID_ARGUMENT;
    env->gr[IA64_PAL_GR_RESULT1] = 0;
    env->gr[IA64_PAL_GR_RESULT2] = 0;
    env->gr[IA64_PAL_GR_RESULT3] = 0;
}

static void pal_mc_expected(CPUIA64State *env)
{
    uint64_t expected = env->gr[IA64_PAL_GR_ARG1];

    if (expected > 1 || env->gr[IA64_PAL_GR_ARG2] != 0 ||
        env->gr[IA64_PAL_GR_ARG3] != 0) {
        env->gr[IA64_PAL_GR_STATUS] = PAL_STATUS_INVALID_ARGUMENT;
        env->gr[IA64_PAL_GR_RESULT1] = 0;
    } else {
        env->gr[IA64_PAL_GR_STATUS] = PAL_STATUS_SUCCESS;
        env->gr[IA64_PAL_GR_RESULT1] = env->pal.pal_mc_expected ? 1 : 0;
        env->pal.pal_mc_expected = expected != 0;
    }
    env->gr[IA64_PAL_GR_RESULT2] = 0;
    env->gr[IA64_PAL_GR_RESULT3] = 0;
}

static void pal_mc_dynamic_state(CPUIA64State *env)
{
    uint64_t offset = env->gr[IA64_PAL_GR_ARG1];

    if ((offset & 7) != 0 || env->gr[IA64_PAL_GR_ARG2] != 0 ||
        env->gr[IA64_PAL_GR_ARG3] != 0) {
        env->gr[IA64_PAL_GR_STATUS] = PAL_STATUS_INVALID_ARGUMENT;
    } else {
        env->gr[IA64_PAL_GR_STATUS] = PAL_STATUS_SUCCESS;
    }
    env->gr[IA64_PAL_GR_RESULT1] = 0;
    env->gr[IA64_PAL_GR_RESULT2] = 0;
    env->gr[IA64_PAL_GR_RESULT3] = 0;
}

static bool pal_mc_level_index_valid(uint64_t level_index)
{
    uint64_t structure_bits = (level_index >> 8) & ((1ULL << 40) - 1);

    if ((level_index >> 48) != 0 || (level_index & 0xff) != 0) {
        return false;
    }

    return structure_bits != 0 && (structure_bits & (structure_bits - 1)) == 0;
}

static void pal_mc_error_info(CPUIA64State *env)
{
    uint64_t info_index = env->gr[IA64_PAL_GR_ARG1];
    uint64_t level_index = env->gr[IA64_PAL_GR_ARG2];
    uint64_t err_type_index = env->gr[IA64_PAL_GR_ARG3];
    bool valid = false;

    switch (info_index) {
    case 0:
    case 1:
        valid = true;
        break;
    case 2:
        valid = pal_mc_level_index_valid(level_index) &&
                (err_type_index & 7) <= 4;
        break;
    default:
        valid = false;
        break;
    }

    env->gr[IA64_PAL_GR_STATUS] = valid ? PAL_STATUS_NO_INFORMATION :
        PAL_STATUS_INVALID_ARGUMENT;
    env->gr[IA64_PAL_GR_RESULT1] = 0;
    env->gr[IA64_PAL_GR_RESULT2] = 0;
    env->gr[IA64_PAL_GR_RESULT3] = 0;
}

static void pal_mc_resume(CPUIA64State *env)
{
    uint64_t set_cmci = env->gr[IA64_PAL_GR_ARG1];
    uint64_t save_ptr = env->gr[IA64_PAL_GR_ARG2];
    uint64_t new_context = env->gr[IA64_PAL_GR_ARG3];

    /*
     * save_ptr has the rules of the PAL_MC_REGISTER_MEM address (SDM Vol.2
     * PAL_MC_RESUME), so the uncacheable bit 63 is allowed there too.
     */
    if (set_cmci > 1 || new_context > 1 ||
        (save_ptr & 0x1ff) != 0) {
        env->gr[IA64_PAL_GR_STATUS] = PAL_STATUS_INVALID_ARGUMENT;
    } else {
        env->gr[IA64_PAL_GR_STATUS] = PAL_STATUS_ERROR;
    }
    env->gr[IA64_PAL_GR_RESULT1] = 0;
    env->gr[IA64_PAL_GR_RESULT2] = 0;
    env->gr[IA64_PAL_GR_RESULT3] = 0;
}

static void pal_mc_register_mem(CPUIA64State *env)
{
    uint64_t address = env->gr[IA64_PAL_GR_ARG1];
    /*
     * The min-state save area must be uncacheable (SDM Vol.2 sec 11.3.2.3,
     * PAL_MC_REGISTER_MEM), so firmware passes its physical address with
     * bit 63 - the uncacheable memory-attribute bit - set, exactly as it
     * does for PAL_COPY_PAL.  Mask that attribute off before checking the
     * 512-byte alignment; rejecting a bit-63-set address is wrong and
     * real SDV firmware fatal-spins on the INVALID_ARGUMENT status.
     */
    uint64_t pa = address & ~PAL_COPY_TARGET_CACHE_ATTR;

    if ((pa & 0x1ff) != 0 ||
        env->gr[IA64_PAL_GR_ARG2] != 0 || env->gr[IA64_PAL_GR_ARG3] != 0) {
        env->gr[IA64_PAL_GR_STATUS] = PAL_STATUS_INVALID_ARGUMENT;
    } else {
        env->gr[IA64_PAL_GR_STATUS] = PAL_STATUS_SUCCESS;
        env->pal.pal_mc_save_addr = pa;
    }
    env->gr[IA64_PAL_GR_RESULT1] = 0;
    env->gr[IA64_PAL_GR_RESULT2] = 0;
    env->gr[IA64_PAL_GR_RESULT3] = 0;
}

static void pal_cache_line_init(CPUIA64State *env)
{
    uint64_t address = env->gr[IA64_PAL_GR_ARG1];

    if ((address >> 63) != 0 || env->gr[IA64_PAL_GR_ARG3] != 0) {
        env->gr[IA64_PAL_GR_STATUS] = PAL_STATUS_INVALID_ARGUMENT;
    } else {
        env->gr[IA64_PAL_GR_STATUS] = PAL_STATUS_SUCCESS;
    }
    env->gr[IA64_PAL_GR_RESULT1] = 0;
    env->gr[IA64_PAL_GR_RESULT2] = 0;
    env->gr[IA64_PAL_GR_RESULT3] = 0;
}

static void pal_pmi_entrypoint(CPUIA64State *env)
{
    uint64_t entry = env->gr[IA64_PAL_GR_ARG1];

    if ((entry >> 63) != 0 || (entry & 0xff) != 0 ||
        env->gr[IA64_PAL_GR_ARG2] != 0 || env->gr[IA64_PAL_GR_ARG3] != 0) {
        env->gr[IA64_PAL_GR_STATUS] = PAL_STATUS_INVALID_ARGUMENT;
    } else {
        env->gr[IA64_PAL_GR_STATUS] = PAL_STATUS_SUCCESS;
        env->pal.pal_pmi_entry = entry;
    }
    env->gr[IA64_PAL_GR_RESULT1] = 0;
    env->gr[IA64_PAL_GR_RESULT2] = 0;
    env->gr[IA64_PAL_GR_RESULT3] = 0;
}

static void pal_return_not_implemented(CPUIA64State *env);

/*
 * PAL_FIRMWARE_REGISTER (hw/ia64/ia64_vpc_abi.h): the project firmware names
 * its IVT, identity window, SAL runtime stubs and CPU-assist region for this
 * processor.  A record this emulator does not recognise -- another firmware
 * calling an implementation-specific index of its own real PAL -- reads as
 * not implemented and changes nothing.
 */
static void pal_firmware_register(CPUIA64State *env)
{
    uint64_t record[IA64_FW_REGISTRATION_SIZE / 8];
    IA64FirmwareRegistration fw;
    uint64_t address = env->gr[IA64_PAL_GR_ARG1];
    uint64_t size = env->gr[IA64_PAL_GR_ARG2];
    unsigned int i;

    if (size < IA64_FW_REGISTRATION_SIZE ||
        env->gr[IA64_PAL_GR_ARG3] != 0 ||
        !ia64_exec_physical_rw(address, record, sizeof(record), false)) {
        pal_return_not_implemented(env);
        return;
    }
    for (i = 0; i < ARRAY_SIZE(record); i++) {
        record[i] = le64_to_cpu(record[i]);
    }
    fw = (IA64FirmwareRegistration) {
        .image_base = record[IA64_FW_REGISTRATION_IMAGE_BASE_OFF / 8],
        .image_size = record[IA64_FW_REGISTRATION_IMAGE_SIZE_OFF / 8],
        .ivt = record[IA64_FW_REGISTRATION_IVT_OFF / 8],
        .sal_entry = record[IA64_FW_REGISTRATION_SAL_ENTRY_OFF / 8],
        .sal_return = record[IA64_FW_REGISTRATION_SAL_RETURN_OFF / 8],
        .sal_block = record[IA64_FW_REGISTRATION_SAL_BLOCK_OFF / 8],
        .assist_base = record[IA64_FW_REGISTRATION_ASSIST_OFF / 8],
    };
    if (record[IA64_FW_REGISTRATION_MAGIC_OFF / 8] !=
            IA64_FW_REGISTRATION_MAGIC ||
        fw.ivt == 0 || (fw.ivt & 0xfff) != 0 ||
        fw.image_size == 0 || fw.image_base + fw.image_size < fw.image_base ||
        (fw.sal_entry & (IA64_BUNDLE_SIZE - 1)) != 0 ||
        (fw.sal_return & (IA64_BUNDLE_SIZE - 1)) != 0 ||
        (fw.sal_block & 7) != 0 || (fw.assist_base & 0xfff) != 0) {
        pal_return_not_implemented(env);
        return;
    }

    if (memcmp(&env->firmware, &fw, sizeof(fw)) != 0) {
        env->firmware = fw;
        /*
         * Break recognition and the identity window are decided at translate
         * and fill time; drop what was built without this registration.
         */
        tlb_flush(env_cpu(env));
        queue_tb_flush(env_cpu(env));
        ia64_tlb_bump_generation(env, false);
        ia64_tlb_bump_generation(env, true);
    }
    env->gr[IA64_PAL_GR_STATUS] = PAL_STATUS_SUCCESS;
    env->gr[IA64_PAL_GR_RESULT1] = 0;
    env->gr[IA64_PAL_GR_RESULT2] = 0;
    env->gr[IA64_PAL_GR_RESULT3] = 0;
}

static void pal_mem_for_test(CPUIA64State *env)
{
    env->gr[IA64_PAL_GR_STATUS] = pal_reserved_args_are_zero(env) ?
        PAL_STATUS_SUCCESS : PAL_STATUS_INVALID_ARGUMENT;
    env->gr[IA64_PAL_GR_RESULT1] = 0;
    env->gr[IA64_PAL_GR_RESULT2] =
        env->gr[IA64_PAL_GR_STATUS] == PAL_STATUS_SUCCESS ? 1 : 0;
    env->gr[IA64_PAL_GR_RESULT3] = 0;
}

static uint64_t pal_feature_set_status(CPUIA64State *env, uint64_t feature_set)
{
    uint32_t sets = ia64_env_cpu_class(env)->pal->impl_feature_sets;

    if (feature_set == 0) {
        return PAL_STATUS_SUCCESS;
    }
    if (feature_set < 16) {
        return PAL_STATUS_INVALID_ARGUMENT;
    }
    if (feature_set - 16 >= 32) {
        return PAL_STATUS_BEYOND_MAX;
    }
    sets >>= feature_set - 16;
    if (sets & 1) {
        return PAL_STATUS_SUCCESS;
    }
    return sets != 0 ? PAL_STATUS_NEXT_HIGHER : PAL_STATUS_BEYOND_MAX;
}

static void pal_proc_get_features(CPUIA64State *env)
{
    uint64_t feature_set = env->gr[IA64_PAL_GR_ARG2];

    env->gr[IA64_PAL_GR_RESULT1] = 0;
    env->gr[IA64_PAL_GR_RESULT2] = 0;
    env->gr[IA64_PAL_GR_RESULT3] = 0;

    if (env->gr[IA64_PAL_GR_ARG1] != 0 || env->gr[IA64_PAL_GR_ARG3] != 0) {
        env->gr[IA64_PAL_GR_STATUS] = PAL_STATUS_INVALID_ARGUMENT;
        return;
    }

    env->gr[IA64_PAL_GR_STATUS] = pal_feature_set_status(env, feature_set);
    if (env->gr[IA64_PAL_GR_STATUS] == PAL_STATUS_SUCCESS &&
        feature_set == 18) {
        /* Feature set 18, bit 18: Hyper-Threading is implemented. */
        env->gr[IA64_PAL_GR_RESULT1] = 1ULL << 18;
        env->gr[IA64_PAL_GR_RESULT2] = 1ULL << 18;
    }
}

static void pal_cache_info(CPUIA64State *env)
{
    uint64_t level = env->gr[IA64_PAL_GR_ARG1];
    uint64_t cache_type = env->gr[IA64_PAL_GR_ARG2];
    const IA64PalCacheLevel *info =
        pal_cache_level_for_model(env, level, cache_type);

    if (env->gr[IA64_PAL_GR_ARG3] != 0 || info == NULL) {
        env->gr[IA64_PAL_GR_STATUS] = PAL_STATUS_INVALID_ARGUMENT;
        env->gr[IA64_PAL_GR_RESULT1] = 0;
        env->gr[IA64_PAL_GR_RESULT2] = 0;
        env->gr[IA64_PAL_GR_RESULT3] = 0;
        return;
    }

    env->gr[IA64_PAL_GR_STATUS] = PAL_STATUS_SUCCESS;
    env->gr[IA64_PAL_GR_RESULT1] = (info->unified ? 1ULL : 0ULL) |
                 ((uint64_t)info->attribute << 1) |
                 ((uint64_t)info->associativity << 8) |
                 ((uint64_t)info->line_shift << 16) |
                 ((uint64_t)info->stride_shift << 24) |
                 ((uint64_t)info->store_latency << 32) |
                 ((uint64_t)info->load_latency << 40);
    /*
     * config_info_2{39:32} is alias_boundary: the binary log of the minimum
     * separation of aliased addresses for best performance (SDM Vol.2
     * rev 1.1 Fig. 11-18) -- i.e. the way span, log2(size/associativity),
     * which is by construction the same quantity as tag_lsb for every
     * modelled cache.  It previously carried the line size, which belongs
     * only in config_info_1.
     */
    env->gr[IA64_PAL_GR_RESULT2] = info->size |
                  ((uint64_t)info->tag_lsb << 32) |
                  ((uint64_t)info->tag_lsb << 40) |
                  ((uint64_t)pal_cache_tag_msb(env) << 48);
    env->gr[IA64_PAL_GR_RESULT3] = 0;
}

static void pal_cache_prot_info(CPUIA64State *env)
{
    uint64_t level = env->gr[IA64_PAL_GR_ARG1];
    uint64_t cache_type = env->gr[IA64_PAL_GR_ARG2];
    uint64_t reserved = env->gr[IA64_PAL_GR_ARG3];
    uint32_t data_none = 64;
    uint32_t tag_none;
    const IA64PalCacheLevel *info =
        pal_cache_level_for_model(env, level, cache_type);

    if (reserved != 0 || info == NULL) {
        env->gr[IA64_PAL_GR_STATUS] = PAL_STATUS_INVALID_ARGUMENT;
        env->gr[IA64_PAL_GR_RESULT1] = 0;
        env->gr[IA64_PAL_GR_RESULT2] = 0;
        env->gr[IA64_PAL_GR_RESULT3] = 0;
        return;
    }

    tag_none = (1U << 30) | ((uint32_t)info->tag_lsb << 8) |
               ((uint32_t)pal_cache_tag_msb(env) << 14);
    env->gr[IA64_PAL_GR_STATUS] = PAL_STATUS_SUCCESS;
    env->gr[IA64_PAL_GR_RESULT1] = data_none | ((uint64_t)tag_none << 32);
    env->gr[IA64_PAL_GR_RESULT2] = 0;
    env->gr[IA64_PAL_GR_RESULT3] = 0;
}

static void pal_vm_info(CPUIA64State *env)
{
    uint64_t level = env->gr[IA64_PAL_GR_ARG1];
    uint64_t tc_type = env->gr[IA64_PAL_GR_ARG2];
    const IA64PalTcLevel *tc = pal_tc_level_for_model(env, level, tc_type);

    if (env->gr[IA64_PAL_GR_ARG3] != 0 || tc == NULL) {
        env->gr[IA64_PAL_GR_STATUS] = PAL_STATUS_INVALID_ARGUMENT;
        env->gr[IA64_PAL_GR_RESULT1] = 0;
        env->gr[IA64_PAL_GR_RESULT2] = 0;
        env->gr[IA64_PAL_GR_RESULT3] = 0;
        return;
    }

    /*
     * tc_info layout, SDM Vol. 2 figure 11-38: num_sets{7:0},
     * num_ways{15:8}, num_entries{31:16}, pf{32}, ut{33}, tr{34}.
     */
    env->gr[IA64_PAL_GR_STATUS] = PAL_STATUS_SUCCESS;
    env->gr[IA64_PAL_GR_RESULT1] = (uint64_t)tc->num_sets |
                 ((uint64_t)tc->num_ways << 8) |
                 ((uint64_t)tc->num_entries << 16) |
                 ((uint64_t)tc->preferred_page_size_optimized << 32) |
                 ((uint64_t)tc->unified << 33) |
                 ((uint64_t)tc->reduced_by_trs << 34);
    env->gr[IA64_PAL_GR_RESULT2] = tc->page_mask;
    env->gr[IA64_PAL_GR_RESULT3] = 0;
}

static uint64_t pal_page_shift(uint64_t page_size)
{
    uint64_t shift = 0;

    while ((1ULL << shift) < page_size && shift < 63) {
        shift++;
    }
    return shift;
}

static void pal_vm_tr_read(CPUIA64State *env, uintptr_t ra)
{
    uint64_t reg_num = pal_stacked_arg(env, 0);
    uint64_t tr_type = pal_stacked_arg(env, 1);
    uint64_t tr_buffer = pal_stacked_arg(env, 2);
    const IA64TlbEntry *tlb;
    const IA64TlbEntry *entry;
    uint64_t pte = 0;
    uint64_t itir = 0;
    uint64_t ifa = 0;
    uint64_t rr = 0;
    uint64_t tr_valid = 0;
    uint64_t ps_shift;

    /*
     * tr_type: 0 = ITR, 1 = DTR (SDM 245318-004 sec 11.9 PAL_VM_TR_READ).
     * Bound reg_num by the bank the lookup below actually selects; the two
     * files are asymmetric on the original Itanium (8 ITR / 48 DTR).
     */
    uint64_t tr_bound = tr_type == 0 ? ia64_env_cpu_class(env)->itr_count
                                     : ia64_env_cpu_class(env)->dtr_count;
    if (tr_type > 1 || reg_num >= tr_bound || (tr_buffer & 7) != 0) {
        env->gr[IA64_PAL_GR_STATUS] = PAL_STATUS_INVALID_ARGUMENT;
        env->gr[IA64_PAL_GR_RESULT1] = 0;
        env->gr[IA64_PAL_GR_RESULT2] = 0;
        env->gr[IA64_PAL_GR_RESULT3] = 0;
        return;
    }

    tlb = tr_type == 0 ? env->mmu.tlb_inst : env->mmu.tlb_data;
    entry = &tlb[reg_num];
    if (entry->valid && entry->is_tr) {
        ps_shift = pal_page_shift(entry->ps);
        pte = entry->pte;
        itir = (ps_shift << IA64_ITIR_PS_SHIFT) |
               ((uint64_t)entry->key << IA64_ITIR_KEY_SHIFT);
        ifa = entry->va | 1;
        rr = ((uint64_t)entry->rid << IA64_RR_RID_SHIFT) |
             (ps_shift << IA64_ITIR_PS_SHIFT);
        tr_valid = 0xf;
    }

    ia64_exec_store_data(env, tr_buffer, pte, 8, false, ra);
    ia64_exec_store_data(env, tr_buffer + 8, itir, 8, false, ra);
    ia64_exec_store_data(env, tr_buffer + 16, ifa, 8, false, ra);
    ia64_exec_store_data(env, tr_buffer + 24, rr, 8, false, ra);

    env->gr[IA64_PAL_GR_STATUS] = PAL_STATUS_SUCCESS;
    env->gr[IA64_PAL_GR_RESULT1] = tr_valid;
    env->gr[IA64_PAL_GR_RESULT2] = 0;
    env->gr[IA64_PAL_GR_RESULT3] = 0;
}

static void pal_freq_base(CPUIA64State *env)
{
    if (pal_reserved_args_are_zero(env)) {
        env->gr[IA64_PAL_GR_STATUS] = PAL_STATUS_SUCCESS;
        env->gr[IA64_PAL_GR_RESULT1] =
            ia64_env_cpu_class(env)->pal->freq_base_hz;
    } else {
        env->gr[IA64_PAL_GR_STATUS] = PAL_STATUS_INVALID_ARGUMENT;
        env->gr[IA64_PAL_GR_RESULT1] = 0;
    }
    env->gr[IA64_PAL_GR_RESULT2] = 0;
    env->gr[IA64_PAL_GR_RESULT3] = 0;
}

static void pal_freq_ratios(CPUIA64State *env)
{
    const IA64PalProfile *pal = ia64_env_cpu_class(env)->pal;

    if (pal_reserved_args_are_zero(env)) {
        env->gr[IA64_PAL_GR_STATUS] = PAL_STATUS_SUCCESS;
        /* Each ratio is reported as (numerator << 32) | denominator. */
        env->gr[IA64_PAL_GR_RESULT1] =
            ((uint64_t)pal->proc_ratio_num << 32) | pal->proc_ratio_den;
        env->gr[IA64_PAL_GR_RESULT2] =
            ((uint64_t)pal->bus_ratio_num << 32) | pal->bus_ratio_den;
        env->gr[IA64_PAL_GR_RESULT3] =
            ((uint64_t)pal->itc_ratio_num << 32) | pal->itc_ratio_den;
    } else {
        env->gr[IA64_PAL_GR_STATUS] = PAL_STATUS_INVALID_ARGUMENT;
        env->gr[IA64_PAL_GR_RESULT1] = 0;
        env->gr[IA64_PAL_GR_RESULT2] = 0;
        env->gr[IA64_PAL_GR_RESULT3] = 0;
    }
}

static void pal_ptce_info(CPUIA64State *env)
{
    if (pal_reserved_args_are_zero(env)) {
        env->gr[IA64_PAL_GR_STATUS] = PAL_STATUS_SUCCESS;
        env->gr[IA64_PAL_GR_RESULT1] = 0;
        env->gr[IA64_PAL_GR_RESULT2] = (1ULL << 32) | 1ULL;
        env->gr[IA64_PAL_GR_RESULT3] = 0;
    } else {
        env->gr[IA64_PAL_GR_STATUS] = PAL_STATUS_INVALID_ARGUMENT;
        env->gr[IA64_PAL_GR_RESULT1] = 0;
        env->gr[IA64_PAL_GR_RESULT2] = 0;
        env->gr[IA64_PAL_GR_RESULT3] = 0;
    }
}

static void pal_bus_get_features(CPUIA64State *env)
{
    if (pal_reserved_args_are_zero(env)) {
        env->gr[IA64_PAL_GR_STATUS] = PAL_STATUS_SUCCESS;
        /*
         * This model has no software-configurable processor-bus features.
         * Bits 0 through 28 are reserved by the PAL specification, so do not
         * expose the old placeholder mask in features_avail.
         */
        env->gr[IA64_PAL_GR_RESULT1] = 0;
        env->gr[IA64_PAL_GR_RESULT2] = 0;
        env->gr[IA64_PAL_GR_RESULT3] = 0;
    } else {
        env->gr[IA64_PAL_GR_STATUS] = PAL_STATUS_INVALID_ARGUMENT;
        env->gr[IA64_PAL_GR_RESULT1] = 0;
        env->gr[IA64_PAL_GR_RESULT2] = 0;
        env->gr[IA64_PAL_GR_RESULT3] = 0;
    }
}

static void pal_set_features(CPUIA64State *env)
{
    /* A feature that cannot be set is ignored (SDM Vol. 2). */
    if (env->gr[IA64_PAL_GR_ARG3] != 0) {
        env->gr[IA64_PAL_GR_STATUS] = PAL_STATUS_INVALID_ARGUMENT;
    } else {
        env->gr[IA64_PAL_GR_STATUS] =
            pal_feature_set_status(env, env->gr[IA64_PAL_GR_ARG2]);
    }
    env->gr[IA64_PAL_GR_RESULT1] = 0;
    env->gr[IA64_PAL_GR_RESULT2] = 0;
    env->gr[IA64_PAL_GR_RESULT3] = 0;
}

static void pal_register_info(CPUIA64State *env)
{
    uint64_t info_type = env->gr[IA64_PAL_GR_ARG1];

    if (env->gr[IA64_PAL_GR_ARG2] != 0 ||
        env->gr[IA64_PAL_GR_ARG3] != 0 || info_type > 3) {
        env->gr[IA64_PAL_GR_STATUS] = PAL_STATUS_INVALID_ARGUMENT;
        env->gr[IA64_PAL_GR_RESULT1] = 0;
        env->gr[IA64_PAL_GR_RESULT2] = 0;
        env->gr[IA64_PAL_GR_RESULT3] = 0;
        return;
    }

    env->gr[IA64_PAL_GR_STATUS] = PAL_STATUS_SUCCESS;
    switch (info_type) {
    case 0:
        env->gr[IA64_PAL_GR_RESULT1] = 0x000011117f2f00ffULL;
        env->gr[IA64_PAL_GR_RESULT2] = 0x7;
        break;
    case 1:
        env->gr[IA64_PAL_GR_RESULT1] = 0;
        env->gr[IA64_PAL_GR_RESULT2] = 0;
        break;
    case 2:
        env->gr[IA64_PAL_GR_RESULT1] = 0x0000000003fb0107ULL;
        env->gr[IA64_PAL_GR_RESULT2] = 0x307ff;
        break;
    case 3:
        env->gr[IA64_PAL_GR_RESULT1] = 0;
        env->gr[IA64_PAL_GR_RESULT2] = 0x2;
        break;
    default:
        g_assert_not_reached();
        break;
    }
    env->gr[IA64_PAL_GR_RESULT3] = 0;
}

static void pal_perf_mon_info(CPUIA64State *env, uintptr_t ra)
{
    uint64_t pm_buffer = env->gr[IA64_PAL_GR_ARG1];
    int i;

    if (pm_buffer == 0 || (pm_buffer & 7) != 0 ||
        env->gr[IA64_PAL_GR_ARG2] != 0 || env->gr[IA64_PAL_GR_ARG3] != 0) {
        env->gr[IA64_PAL_GR_STATUS] = PAL_STATUS_INVALID_ARGUMENT;
        env->gr[IA64_PAL_GR_RESULT1] = 0;
        env->gr[IA64_PAL_GR_RESULT2] = 0;
        env->gr[IA64_PAL_GR_RESULT3] = 0;
        return;
    }

    for (i = 0; i < 16; i++) {
        ia64_exec_store_data(env, pm_buffer + i * 8, 0, 8, false, ra);
    }

    /*
     * The architecture requires at least four generic PMC/PMD pairs.
     * Model the baseline processor monitor layout used by this CPU model:
     * four 48-bit generic counters at indices 4 through 7, with event
     * selectors 0x12 for cycles and 0x08 for retired bundles.
     */
    ia64_exec_store_data(env, pm_buffer, PAL_PERF_PMC_MASK, 8, false, ra);
    ia64_exec_store_data(env, pm_buffer + 0x20, PAL_PERF_PMD_MASK,
                         8, false, ra);
    ia64_exec_store_data(env, pm_buffer + 0x40, PAL_PERF_GENERIC_MASK,
                         8, false, ra);
    ia64_exec_store_data(env, pm_buffer + 0x60, PAL_PERF_GENERIC_MASK,
                         8, false, ra);

    env->gr[IA64_PAL_GR_STATUS] = PAL_STATUS_SUCCESS;
    env->gr[IA64_PAL_GR_RESULT1] = PAL_PERF_MON_INFO_VALUE;
    env->gr[IA64_PAL_GR_RESULT2] = 0;
    env->gr[IA64_PAL_GR_RESULT3] = 0;
}

static bool pal_addr_overlaps_fw_update(uint64_t address, uint64_t alignment)
{
    uint64_t fw_base = IA64_FW_ADDRESS_SPACE_BASE;
    uint64_t fw_limit = IA64_FW_ADDRESS_SPACE_END;
    uint64_t block_end;

    if (address >= fw_limit) {
        return false;
    }

    block_end = address + alignment;
    return block_end > fw_base && address < fw_limit;
}

static void pal_platform_addr(CPUIA64State *env)
{
    uint64_t block_type = env->gr[IA64_PAL_GR_ARG1];
    uint64_t address = env->gr[IA64_PAL_GR_ARG2] & ~(1ULL << 63);
    uint64_t alignment;
    uint64_t supported;

    if (env->gr[IA64_PAL_GR_ARG3] != 0 || block_type > 1) {
        env->gr[IA64_PAL_GR_STATUS] = PAL_STATUS_INVALID_ARGUMENT;
        env->gr[IA64_PAL_GR_RESULT1] = 0;
        env->gr[IA64_PAL_GR_RESULT2] = 0;
        env->gr[IA64_PAL_GR_RESULT3] = 0;
        return;
    }

    if (block_type == 0) {
        alignment = 2ULL << 20;
        supported = IA64_LOCAL_SAPIC_PA;
    } else {
        alignment = 64ULL << 20;
        supported = ia64_env_cpu_class(env)->pal->io_block_pa;
    }

    if ((address & (alignment - 1)) != 0 ||
        pal_addr_overlaps_fw_update(address, alignment)) {
        env->gr[IA64_PAL_GR_STATUS] = PAL_STATUS_ERROR;
    } else if (address != supported) {
        env->gr[IA64_PAL_GR_STATUS] = PAL_STATUS_ERROR;
    } else {
        env->gr[IA64_PAL_GR_STATUS] = PAL_STATUS_SUCCESS;
        if (block_type == 0) {
            env->pal.pal_interrupt_block_addr = address;
        } else {
            env->pal.pal_io_block_addr = address;
        }
    }
    env->gr[IA64_PAL_GR_RESULT1] = 0;
    env->gr[IA64_PAL_GR_RESULT2] = 0;
    env->gr[IA64_PAL_GR_RESULT3] = 0;
}

static void pal_test_proc(CPUIA64State *env)
{
    uint64_t test_address = pal_stacked_arg(env, 0);
    uint64_t attributes = pal_stacked_arg(env, 2);

    if ((test_address >> 63) != 0 ||
        (attributes & ~PAL_MEM_ATTR_VALID_MASK) != 0 ||
        (attributes & PAL_MEM_ATTR_WB) == 0) {
        env->gr[IA64_PAL_GR_STATUS] = PAL_STATUS_INVALID_ARGUMENT;
        env->gr[IA64_PAL_GR_RESULT1] = 0;
    } else {
        env->gr[IA64_PAL_GR_STATUS] = PAL_STATUS_SUCCESS;
        env->gr[IA64_PAL_GR_RESULT1] = PAL_SELF_TEST_STATE_TESTED;
    }
    env->gr[IA64_PAL_GR_RESULT2] = 0;
    env->gr[IA64_PAL_GR_RESULT3] = 0;
}

/*
 * Number of implemented instruction and data breakpoint register pairs.
 *
 * Real hardware has four of each -- the architecture requires at least that
 * many (SDM Vol. 2 sec 7.1.1) and 245320-002 sec 6.2.4 refers to "all four
 * architectural breakpoint registers (IBRs)" on Merced -- and IBR/DBR reads
 * and writes are modelled.  Nothing matches against them, though: no
 * reference in the instruction-fetch or data paths consults IBR/DBR, so the
 * Debug vector (0x5900) is never delivered and a guest that programmes a
 * hardware breakpoint gets silence.
 *
 * Report zero until matching exists.  A guest told there are none will not
 * offer hardware breakpoints; a guest told there are four sets them and
 * waits forever, which is strictly worse.  Implementing the match needs a
 * PSR.db translation-block flag so the per-bundle instruction compare can be
 * skipped when debugging is off, hooks on every load, store, semaphore,
 * lfetch.fault, probe.fault and mandatory RSE reference for the data side,
 * and Debug-fault priority placed against the TLB faults.
 */
#define IA64_IMPLEMENTED_IBR_PAIRS 0
#define IA64_IMPLEMENTED_DBR_PAIRS 0

static void pal_debug_info(CPUIA64State *env)
{
    if (pal_reserved_args_are_zero(env)) {
        env->gr[IA64_PAL_GR_STATUS] = PAL_STATUS_SUCCESS;
        env->gr[IA64_PAL_GR_RESULT1] = IA64_IMPLEMENTED_IBR_PAIRS;
        env->gr[IA64_PAL_GR_RESULT2] = IA64_IMPLEMENTED_DBR_PAIRS;
    } else {
        env->gr[IA64_PAL_GR_STATUS] = PAL_STATUS_INVALID_ARGUMENT;
        env->gr[IA64_PAL_GR_RESULT1] = 0;
        env->gr[IA64_PAL_GR_RESULT2] = 0;
    }
    env->gr[IA64_PAL_GR_RESULT3] = 0;
}

/*
 * The processor's geographic id on its bus: the value PALE_RESET hands SAL
 * in GR33 (SDM vol. 2 11.2.2), which the board chooses per socket.
 */
static void pal_fixed_addr(CPUIA64State *env)
{
    if (pal_reserved_args_are_zero(env)) {
        env->gr[IA64_PAL_GR_STATUS] = PAL_STATUS_SUCCESS;
        env->gr[IA64_PAL_GR_RESULT1] =
            ia64_cpu_geographic_id(env_archcpu(env));
    } else {
        env->gr[IA64_PAL_GR_STATUS] = PAL_STATUS_INVALID_ARGUMENT;
        env->gr[IA64_PAL_GR_RESULT1] = 0;
    }
    env->gr[IA64_PAL_GR_RESULT2] = 0;
    env->gr[IA64_PAL_GR_RESULT3] = 0;
}

/* PAL procedures that post-date Merced return this on the merced model. */
static void pal_return_not_implemented(CPUIA64State *env)
{
    env->gr[IA64_PAL_GR_STATUS] = PAL_STATUS_NOT_IMPLEMENTED;
    env->gr[IA64_PAL_GR_RESULT1] = 0;
    env->gr[IA64_PAL_GR_RESULT2] = 0;
    env->gr[IA64_PAL_GR_RESULT3] = 0;
}

/* True when a post-Merced PAL procedure is available on this model. */
static bool pal_post_merced_available(CPUIA64State *env)
{
    return ia64_env_cpu_class(env)->pal->has_post_merced_pal;
}

uint32_t ia64_pal_dispatch(CPUIA64State *env, uintptr_t ra)
{
    uint64_t index = env->gr[IA64_PAL_GR_INDEX];
    uint32_t flags = 0;

    trace_ia64_pal_call(env_cpu(env)->cpu_index, index,
                        env->gr[IA64_PAL_GR_ARG1],
                        env->gr[IA64_PAL_GR_ARG2],
                        env->gr[IA64_PAL_GR_ARG3]);
    switch (index) {
    case PAL_VERSION:
        pal_get_version(env);
        break;
    case PAL_RSE_INFO:
        pal_rse_info(env);
        break;
    case PAL_VM_SUMMARY:
        pal_vm_summary(env);
        break;
    case PAL_HALT_LIGHT:
        if (pal_halt_light(env)) {
            flags |= IA64_PAL_DISPATCH_HALTED;
        }
        break;
    case PAL_PREFETCH_VIS:
        if (!pal_post_merced_available(env)) {
            pal_return_not_implemented(env);
        } else {
            pal_prefetch_vis(env);
        }
        break;
    case PAL_CACHE_FLUSH:
        if (pal_cache_flush(env)) {
            flags |= IA64_PAL_DISPATCH_EXIT_TB;
        }
        break;
    case PAL_CACHE_INIT:
        pal_cache_init(env);
        break;
    case PAL_CACHE_LINE_INIT:
        pal_cache_line_init(env);
        break;
    case PAL_CACHE_SUMMARY:
        pal_cache_summary(env);
        break;
    case PAL_MEM_ATTRIB:
        pal_mem_attrib(env);
        break;
    case PAL_PROC_GET_FEATURES:
        pal_proc_get_features(env);
        break;
    case PAL_PROC_SET_FEATURES:
        pal_set_features(env);
        break;
    case PAL_CACHE_INFO:
        pal_cache_info(env);
        break;
    case PAL_CACHE_PROT_INFO:
        pal_cache_prot_info(env);
        break;
    case PAL_CACHE_SHARED_INFO:
        if (!pal_post_merced_available(env)) {
            pal_return_not_implemented(env);
        } else {
            pal_cache_shared_info(env);
        }
        break;
    case PAL_VM_INFO:
        pal_vm_info(env);
        break;
    case PAL_VM_PAGE_SIZE:
        pal_vm_page_size(env);
        break;
    case PAL_VM_TR_READ:
        pal_vm_tr_read(env, ra);
        break;
    case PAL_FREQ_BASE:
        pal_freq_base(env);
        break;
    case PAL_FREQ_RATIOS:
        pal_freq_ratios(env);
        break;
    case PAL_PTCE_INFO:
        pal_ptce_info(env);
        break;
    case PAL_BUS_GET_FEATURES:
        pal_bus_get_features(env);
        break;
    case PAL_BUS_SET_FEATURES:
        pal_set_features(env);
        break;
    case PAL_REGISTER_INFO:
        pal_register_info(env);
        break;
    case PAL_PERF_MON_INFO:
        pal_perf_mon_info(env, ra);
        break;
    case PAL_PLATFORM_ADDR:
        pal_platform_addr(env);
        break;
    case PAL_TEST_PROC:
        pal_test_proc(env);
        break;
    case PAL_DEBUG_INFO:
        pal_debug_info(env);
        break;
    case PAL_FIXED_ADDR:
        pal_fixed_addr(env);
        break;
    case PAL_LOGICAL_TO_PHYSICAL:
        if (!pal_post_merced_available(env)) {
            pal_return_not_implemented(env);
        } else {
            pal_logical_to_physical(env);
        }
        break;
    case PAL_IMPL_PROC_RESPONSE_TIMEOUT:
        if (!pal_post_merced_available(env)) {
            pal_return_not_implemented(env);
        } else {
            env->gr[IA64_PAL_GR_STATUS] = PAL_STATUS_SUCCESS;
            env->gr[IA64_PAL_GR_RESULT1] = 0;
            env->gr[IA64_PAL_GR_RESULT2] = 0;
            env->gr[IA64_PAL_GR_RESULT3] = 0;
        }
        break;
    case PAL_MC_CLEAR_LOG:
        pal_mc_clear_log(env);
        break;
    case PAL_COPY_INFO:
        pal_copy_info(env);
        break;
    case PAL_COPY_PAL:
        pal_copy_pal(env);
        break;
    case PAL_BRAND_INFO:
        if (!pal_post_merced_available(env)) {
            pal_return_not_implemented(env);
        } else {
            pal_brand_info(env, ra);
        }
        break;
    case PAL_HALT_INFO:
        pal_halt_info(env, ra);
        break;
    case PAL_MC_DRAIN:
        pal_mc_drain(env);
        break;
    case PAL_MC_EXPECTED:
        pal_mc_expected(env);
        break;
    case PAL_MC_DYNAMIC_STATE:
        pal_mc_dynamic_state(env);
        break;
    case PAL_MC_ERROR_INFO:
        pal_mc_error_info(env);
        break;
    case PAL_MC_RESUME:
        pal_mc_resume(env);
        break;
    case PAL_MC_REGISTER_MEM:
        pal_mc_register_mem(env);
        break;
    case PAL_HALT:
        if (pal_halt(env)) {
            flags |= IA64_PAL_DISPATCH_HALTED;
        }
        break;
    case PAL_MEM_FOR_TEST:
        pal_mem_for_test(env);
        break;
    case IA64_PAL_FIRMWARE_REGISTER:
        pal_firmware_register(env);
        flags |= IA64_PAL_DISPATCH_EXIT_TB;
        break;
    case PAL_PMI_ENTRYPOINT:
        pal_pmi_entrypoint(env);
        break;
    default:
        env->gr[IA64_PAL_GR_STATUS] = PAL_STATUS_NOT_IMPLEMENTED;
        env->gr[IA64_PAL_GR_RESULT1] = 0;
        env->gr[IA64_PAL_GR_RESULT2] = 0;
        env->gr[IA64_PAL_GR_RESULT3] = 0;
        break;
    }

    /*
     * Every PAL procedure returns its status/values in GR8-GR11.  On hardware
     * PAL writes these as ordinary register writes, which clear the NaT bit;
     * the PAL calling convention (SDM Vol.2 sec 11.10) lists r8-r11 as return
     * registers, so a NaT left in one of them before the call must not survive
     * the return.  The pal_*() handlers above set env->gr[8..11] but not the
     * NaT bits, so clear them here -- otherwise a caller that leaves an output
     * register NaT before the call (legal: r8-r11 are outputs) consumes a
     * bogus NaT on the result.  (Observed: XP SETUPLDR wedges on such a store.)
     */
    ia64_gr_nat_set(env, IA64_PAL_GR_STATUS, false);
    ia64_gr_nat_set(env, IA64_PAL_GR_RESULT1, false);
    ia64_gr_nat_set(env, IA64_PAL_GR_RESULT2, false);
    ia64_gr_nat_set(env, IA64_PAL_GR_RESULT3, false);

    /*
     * PAL_PROC is a firmware portal, not a normal C function.  Static
     * calls arrive with a plain branch, stacked calls with br.call; the
     * PAL trampoline returns with a plain branch to b0 in both cases.
     * Stacked-convention indices are exactly those with bit 8 set
     * (256-511 and 768-1023, SDM Vol.2 table 11-11); complete such a
     * call's frame here before the trampoline branches back.
     */
    if (index & 0x100) {
        ia64_rse_pop_return_frame(env, env->ar_pfs);
    }

    trace_ia64_pal_return(env_cpu(env)->cpu_index, index,
                          (int64_t)env->gr[IA64_PAL_GR_STATUS],
                          env->gr[IA64_PAL_GR_RESULT1], flags);
    return flags;
}
