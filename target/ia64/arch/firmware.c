/*
 * IA-64 native firmware-assist bridges.
 */

#include "qemu/osdep.h"
#include "arch/arch.h"
#include "arch/system.h"
#include "cpu.h"
#include "decode/decode.h"
#include "decoder.h"
#include "exec-access.h"
#include "fpreg.h"

/* ---- Native EFI Debug Support context bridge -------------------------- */

#define FW_DEBUG_CTX_R1         8U
#define FW_DEBUG_CTX_F2         256U
#define FW_DEBUG_CTX_PR         736U
#define FW_DEBUG_CTX_B0         744U
#define FW_DEBUG_CTX_AR_RSC     808U
#define FW_DEBUG_CTX_AR_BSP     816U
#define FW_DEBUG_CTX_AR_BSPSTORE 824U
#define FW_DEBUG_CTX_AR_RNAT    832U
#define FW_DEBUG_CTX_AR_FCR     840U
#define FW_DEBUG_CTX_AR_EFLAG   848U
#define FW_DEBUG_CTX_AR_CSD     856U
#define FW_DEBUG_CTX_AR_SSD     864U
#define FW_DEBUG_CTX_AR_CFLG    872U
#define FW_DEBUG_CTX_AR_FSR     880U
#define FW_DEBUG_CTX_AR_FIR     888U
#define FW_DEBUG_CTX_AR_FDR     896U
#define FW_DEBUG_CTX_AR_CCV     904U
#define FW_DEBUG_CTX_AR_UNAT    912U
#define FW_DEBUG_CTX_AR_FPSR    920U
#define FW_DEBUG_CTX_AR_PFS     928U
#define FW_DEBUG_CTX_AR_LC      936U
#define FW_DEBUG_CTX_AR_EC      944U
#define FW_DEBUG_CTX_CR_DCR     952U
#define FW_DEBUG_CTX_CR_ITM     960U
#define FW_DEBUG_CTX_CR_IVA     968U
#define FW_DEBUG_CTX_CR_PTA     976U
#define FW_DEBUG_CTX_CR_IPSR    984U
#define FW_DEBUG_CTX_CR_ISR     992U
#define FW_DEBUG_CTX_CR_IIP     1000U
#define FW_DEBUG_CTX_CR_IFA     1008U
#define FW_DEBUG_CTX_CR_ITIR    1016U
#define FW_DEBUG_CTX_CR_IIPA    1024U
#define FW_DEBUG_CTX_CR_IFS     1032U
#define FW_DEBUG_CTX_CR_IIM     1040U
#define FW_DEBUG_CTX_CR_IHA     1048U
#define FW_DEBUG_CTX_DBR0       1056U
#define FW_DEBUG_CTX_IBR0       1120U
#define FW_DEBUG_CTX_INT_NAT    1184U

QEMU_BUILD_BUG_ON(FW_DEBUG_CTX_INT_NAT + sizeof(uint64_t) !=
                  IA64_FW_DEBUG_CONTEXT_SIZE);

static void ia64_fw_debug_putq(CPUIA64State *env, size_t offset,
                               uint64_t value)
{
    stq_le_p(ia64_firmware_debug_state(env)->context + offset, value);
}

static uint64_t ia64_fw_debug_getq(const CPUIA64State *env, size_t offset)
{
    return ldq_le_p(ia64_firmware_debug_state_const(env)->context + offset);
}

static uint64_t ia64_fw_debug_pr(const CPUIA64State *env)
{
    uint64_t value = 1;
    unsigned i;

    for (i = 1; i < IA64_PR_COUNT; i++) {
        value |= (env->pr[i] & 1) << i;
    }
    return value;
}

static uint64_t ia64_fw_debug_int_nat(const CPUIA64State *env)
{
    uint64_t value = 0;
    unsigned i;

    for (i = 1; i < 32; i++) {
        value |= ((env->nat[i / 64] >> (i % 64)) & 1) << i;
    }
    return value;
}

static uint64_t ia64_fw_debug_current_cfm(const CPUIA64State *env)
{
    return env->cfm_sof
        | ((uint64_t)env->cfm_sol << IA64_CFM_SOL_SHIFT)
        | ((uint64_t)env->cfm_sor << IA64_CFM_SOR_SHIFT)
        | ((uint64_t)env->cfm_rrb_gr << IA64_CFM_RRB_GR_SHIFT)
        | ((uint64_t)env->cfm_rrb_fr << IA64_CFM_RRB_FR_SHIFT)
        | ((uint64_t)env->cfm_rrb_pr << IA64_CFM_RRB_PR_SHIFT);
}

void ia64_firmware_debug_capture(CPUIA64State *env, uint16_t vector,
                                 bool collected)
{
    IA64FirmwareDebugState *debug = ia64_firmware_debug_state(env);
    uint64_t low;
    uint64_t high;
    unsigned i;

    if (debug->handler_active) {
        return;
    }
    debug->context_valid = false;
    debug->rse_valid = false;
    if (!collected ||
        !ia64_firmware_registered(&env->firmware) ||
        env->cr_iva != env->firmware.ivt) {
        return;
    }

    memset(debug->context, 0, sizeof(debug->context));
    for (i = 1; i < 32; i++) {
        ia64_fw_debug_putq(env, FW_DEBUG_CTX_R1 + (i - 1) * 8,
                           env->gr[i]);
    }
    for (i = 2; i < 32; i++) {
        ia64_fpreg_to_spill(env, i, &low, &high);
        ia64_fw_debug_putq(env, FW_DEBUG_CTX_F2 + (i - 2) * 16, low);
        ia64_fw_debug_putq(env, FW_DEBUG_CTX_F2 + (i - 2) * 16 + 8,
                           high);
    }
    ia64_fw_debug_putq(env, FW_DEBUG_CTX_PR, ia64_fw_debug_pr(env));
    for (i = 0; i < IA64_BR_COUNT; i++) {
        ia64_fw_debug_putq(env, FW_DEBUG_CTX_B0 + i * 8, env->br[i]);
    }

    ia64_fw_debug_putq(env, FW_DEBUG_CTX_AR_RSC, env->ar_rsc);
    ia64_fw_debug_putq(env, FW_DEBUG_CTX_AR_BSP, env->ar_bsp);
    ia64_fw_debug_putq(env, FW_DEBUG_CTX_AR_BSPSTORE, env->ar_bspstore);
    ia64_fw_debug_putq(env, FW_DEBUG_CTX_AR_RNAT, env->ar_rnat);
    ia64_fw_debug_putq(env, FW_DEBUG_CTX_AR_FCR, env->ar_fcr);
    ia64_fw_debug_putq(env, FW_DEBUG_CTX_AR_EFLAG, env->ar_eflag);
    ia64_fw_debug_putq(env, FW_DEBUG_CTX_AR_CSD, env->ar_csd);
    ia64_fw_debug_putq(env, FW_DEBUG_CTX_AR_SSD, env->ar_ssd);
    ia64_fw_debug_putq(env, FW_DEBUG_CTX_AR_CFLG, env->ar_cflg);
    ia64_fw_debug_putq(env, FW_DEBUG_CTX_AR_FSR, env->ar_fsr);
    ia64_fw_debug_putq(env, FW_DEBUG_CTX_AR_FIR, env->ar_fir);
    ia64_fw_debug_putq(env, FW_DEBUG_CTX_AR_FDR, env->ar_fdr);
    ia64_fw_debug_putq(env, FW_DEBUG_CTX_AR_CCV, env->ar_ccv);
    ia64_fw_debug_putq(env, FW_DEBUG_CTX_AR_UNAT, env->ar_unat);
    ia64_fw_debug_putq(env, FW_DEBUG_CTX_AR_FPSR, env->ar_fpsr);
    ia64_fw_debug_putq(env, FW_DEBUG_CTX_AR_PFS, env->ar_pfs);
    ia64_fw_debug_putq(env, FW_DEBUG_CTX_AR_LC, env->ar_lc);
    ia64_fw_debug_putq(env, FW_DEBUG_CTX_AR_EC, env->ar_ec);

    ia64_fw_debug_putq(env, FW_DEBUG_CTX_CR_DCR, env->cr_dcr);
    ia64_fw_debug_putq(env, FW_DEBUG_CTX_CR_ITM, env->cr_itm);
    ia64_fw_debug_putq(env, FW_DEBUG_CTX_CR_IVA, env->cr_iva);
    ia64_fw_debug_putq(env, FW_DEBUG_CTX_CR_PTA, env->cr_pta);
    ia64_fw_debug_putq(env, FW_DEBUG_CTX_CR_IPSR, env->cr_ipsr);
    ia64_fw_debug_putq(env, FW_DEBUG_CTX_CR_ISR, env->cr_isr);
    ia64_fw_debug_putq(env, FW_DEBUG_CTX_CR_IIP, env->cr_iip);
    ia64_fw_debug_putq(env, FW_DEBUG_CTX_CR_IFA, env->cr_ifa);
    ia64_fw_debug_putq(env, FW_DEBUG_CTX_CR_ITIR, env->cr_itir);
    ia64_fw_debug_putq(env, FW_DEBUG_CTX_CR_IIPA, env->cr_iipa);
    ia64_fw_debug_putq(env, FW_DEBUG_CTX_CR_IFS,
                       IA64_IFS_V | ia64_fw_debug_current_cfm(env));
    ia64_fw_debug_putq(env, FW_DEBUG_CTX_CR_IIM, env->cr_iim);
    ia64_fw_debug_putq(env, FW_DEBUG_CTX_CR_IHA, env->cr_iha);
    for (i = 0; i < 8; i++) {
        ia64_fw_debug_putq(env, FW_DEBUG_CTX_DBR0 + i * 8, env->dbr[i]);
    }
    for (i = 0; i < 8; i++) {
        ia64_fw_debug_putq(env, FW_DEBUG_CTX_IBR0 + i * 8, env->ibr[i]);
    }
    ia64_fw_debug_putq(env, FW_DEBUG_CTX_INT_NAT,
                       ia64_fw_debug_int_nat(env));
    debug->vector = vector;
    debug->context_valid = true;
}

static unsigned ia64_fw_debug_exception_type(uint16_t vector)
{
    if (vector < 0x5000 && (vector & 0x3ff) == 0) {
        return vector >> 10;
    }
    if (vector >= 0x5000 && vector <= 0x6b00 &&
        (vector & 0xff) == 0) {
        return 20 + ((vector - 0x5000) >> 8);
    }
    return 64;
}

static unsigned ia64_fw_debug_cpu_index(CPUIA64State *env)
{
    CPUState *cs = env_cpu(env);
    unsigned index = cs->cpu_index < 0 ? 0 : cs->cpu_index;

    return MIN(index, IA64_VPC_MAX_CPUS - 1);
}

static hwaddr ia64_fw_debug_context_pa(CPUIA64State *env)
{
    unsigned index = ia64_fw_debug_cpu_index(env);

    return ia64_fw_cpu_assist_base(env) + IA64_FW_DEBUG_CONTEXT_OFFSET +
           (hwaddr)index * IA64_FW_DEBUG_CONTEXT_STRIDE;
}

uint32_t ia64_firmware_debug_enter(CPUIA64State *env, uint64_t address)
{
    IA64FirmwareDebugState *debug = ia64_firmware_debug_state(env);
    uint64_t vector_base = env->cr_iva & ~0x7fffULL;
    uint64_t vector_address = vector_base | debug->vector;
    uint64_t handler = env->gr[IA64_FW_DEBUG_GR_HANDLER];
    unsigned exception_type;

    if (!debug->context_valid || debug->handler_active ||
        !ia64_firmware_registered(&env->firmware) ||
        vector_base != env->firmware.ivt ||
        address < vector_address || address >= vector_address + 0x100 ||
        !ia64_firmware_in_image(&env->firmware, handler) ||
        (handler & (IA64_BUNDLE_SIZE - 1))) {
        return 0;
    }

    exception_type = ia64_fw_debug_exception_type(debug->vector);
    if (exception_type >= 64) {
        return 0;
    }
    debug->handler_active = true;
    env->gr[IA64_FW_DEBUG_GR_EXCEPTION] = exception_type;
    env->gr[IA64_FW_DEBUG_GR_CONTEXT] = ia64_fw_debug_context_pa(env);
    env->gr[IA64_FW_DEBUG_GR_CPU] = ia64_fw_debug_cpu_index(env);
    env->gr[IA64_GR_STACK_POINTER] = ia64_fw_cpu_assist_base(env) +
                  IA64_FW_DEBUG_STACK_OFFSET +
                  (ia64_fw_debug_cpu_index(env) + 1) *
                  IA64_FW_DEBUG_STACK_SIZE - 16;
    env->nat[0] &= ~((1ULL << IA64_GR_STACK_POINTER) |
                     (1ULL << IA64_FW_DEBUG_GR_EXCEPTION) |
                     (1ULL << IA64_FW_DEBUG_GR_CONTEXT) |
                     (1ULL << IA64_FW_DEBUG_GR_CPU));
    ia64_set_psr(env, env->psr & ~(IA64_PSR_DT | IA64_PSR_RT |
                                   IA64_PSR_IT | IA64_PSR_RI_MASK));
    ia64_tlb_serialize(env, 1, 1);
    env->ip = handler;
    env->exception_state.fault_slot = 0;
    env->instruction_group_start = true;
    return 1;
}

static void ia64_rse_state_save(CPUIA64State *env,
                                IA64FirmwareDebugRseState *state)
{
    memcpy(state->pgr, env->rse.rse_pgr, sizeof(state->pgr));
    memcpy(state->pgr_nat, env->rse.rse_pgr_nat, sizeof(state->pgr_nat));
    memcpy(state->gr_dirty, env->rse.rse_gr_dirty, sizeof(state->gr_dirty));
    state->bsp = env->ar_bsp;
    state->bspstore = env->ar_bspstore;
    state->rnat = env->ar_rnat;
    state->rnat_low = env->rse.rse_rnat_low;
    state->bol = env->rse.rse_bol;
    state->dirty = env->rse.rse_dirty;
    state->dirty_nat = env->rse.rse_dirty_nat;
    state->clean = env->rse.rse_clean;
    state->clean_nat = env->rse.rse_clean_nat;
    state->invalid = env->rse.rse_invalid;
    state->cfm_sof = env->cfm_sof;
    state->cfm_sol = env->cfm_sol;
    state->cfm_sor = env->cfm_sor;
    state->cfm_rrb_gr = env->cfm_rrb_gr;
    state->cfm_rrb_fr = env->cfm_rrb_fr;
    state->cfm_rrb_pr = env->cfm_rrb_pr;
    state->cfle = env->rse.rse_cfle;
}

static void ia64_rse_state_restore(CPUIA64State *env,
                                   const IA64FirmwareDebugRseState *state)
{
    memcpy(env->rse.rse_pgr, state->pgr, sizeof(state->pgr));
    memcpy(env->rse.rse_pgr_nat, state->pgr_nat, sizeof(state->pgr_nat));
    memcpy(env->rse.rse_gr_dirty, state->gr_dirty, sizeof(state->gr_dirty));
    env->ar_bsp = state->bsp;
    env->ar_bspstore = state->bspstore;
    env->ar_rnat = state->rnat;
    env->rse.rse_rnat_low = state->rnat_low;
    env->rse.rse_bol = state->bol;
    env->rse.rse_dirty = state->dirty;
    env->rse.rse_dirty_nat = state->dirty_nat;
    env->rse.rse_clean = state->clean;
    env->rse.rse_clean_nat = state->clean_nat;
    env->rse.rse_invalid = state->invalid;
    env->cfm_sof = state->cfm_sof;
    env->cfm_sol = state->cfm_sol;
    env->cfm_sor = state->cfm_sor;
    env->cfm_rrb_gr = state->cfm_rrb_gr;
    ia64_set_cfm_rrb_fr(env, state->cfm_rrb_fr);
    env->cfm_rrb_pr = state->cfm_rrb_pr;
    env->rse.rse_cfle = state->cfle;
}

uint32_t ia64_firmware_debug_save(CPUIA64State *env)
{
    IA64FirmwareDebugState *debug = ia64_firmware_debug_state(env);

    if (!debug->handler_active || !debug->context_valid) {
        return 0;
    }
    ia64_fw_debug_putq(env, FW_DEBUG_CTX_CR_IFS, env->cr_ifs);
    ia64_rse_state_save(env, &debug->rse);
    debug->rse_valid = true;
    (void)ia64_exec_physical_rw(
        ia64_fw_debug_context_pa(env),
        debug->context, sizeof(debug->context), true);
    return 1;
}

static void ia64_fw_debug_restore_static_gr(CPUIA64State *env,
                                            uint64_t ipsr,
                                            uint64_t int_nat)
{
    unsigned i;

    for (i = 1; i < 16; i++) {
        env->gr[i] = ia64_fw_debug_getq(env,
                                       FW_DEBUG_CTX_R1 + (i - 1) * 8);
        if (int_nat & (1ULL << i)) {
            env->nat[0] |= 1ULL << i;
        } else {
            env->nat[0] &= ~(1ULL << i);
        }
    }
    for (i = 16; i < 32; i++) {
        uint64_t value = ia64_fw_debug_getq(
            env, FW_DEBUG_CTX_R1 + (i - 1) * 8);
        bool nat = (int_nat >> i) & 1;

        if (ipsr & IA64_PSR_BN) {
            env->banked_gr[i - 16] = value;
            if (nat) {
                env->banked_nat |= 1U << (i - 16);
            } else {
                env->banked_nat &= ~(1U << (i - 16));
            }
        } else {
            env->gr[i] = value;
            if (nat) {
                env->nat[0] |= 1ULL << i;
            } else {
                env->nat[0] &= ~(1ULL << i);
            }
        }
    }
}

uint32_t ia64_firmware_debug_restore(CPUIA64State *env)
{
    IA64FirmwareDebugState *debug = ia64_firmware_debug_state(env);
    uint64_t ipsr;
    uint64_t int_nat;
    uint64_t pr;
    uint64_t original_bsp;
    uint64_t original_bspstore;
    uint64_t restored_bsp;
    uint64_t restored_bspstore;
    unsigned i;

    if (!debug->handler_active || !debug->context_valid ||
        !debug->rse_valid) {
        return 0;
    }
    original_bsp = ia64_fw_debug_getq(env, FW_DEBUG_CTX_AR_BSP);
    original_bspstore = ia64_fw_debug_getq(env,
                                            FW_DEBUG_CTX_AR_BSPSTORE);
    (void)ia64_exec_physical_rw(
        ia64_fw_debug_context_pa(env),
        debug->context, sizeof(debug->context), false);
    ia64_rse_state_restore(env, &debug->rse);
    restored_bsp = ia64_fw_debug_getq(env, FW_DEBUG_CTX_AR_BSP);
    restored_bspstore = ia64_fw_debug_getq(env,
                                            FW_DEBUG_CTX_AR_BSPSTORE);
    env->ar_bsp += restored_bsp - original_bsp;
    env->ar_bspstore += restored_bspstore - original_bspstore;
    env->ar_rnat = ia64_fw_debug_getq(env, FW_DEBUG_CTX_AR_RNAT);

    ipsr = ia64_fw_debug_getq(env, FW_DEBUG_CTX_CR_IPSR);
    int_nat = ia64_fw_debug_getq(env, FW_DEBUG_CTX_INT_NAT);
    ia64_fw_debug_restore_static_gr(env, ipsr, int_nat);
    for (i = 2; i < 32; i++) {
        uint64_t low = ia64_fw_debug_getq(
            env, FW_DEBUG_CTX_F2 + (i - 2) * 16);
        uint64_t high = ia64_fw_debug_getq(
            env, FW_DEBUG_CTX_F2 + (i - 2) * 16 + 8);

        ia64_fpreg_from_spill(env, i, low, high);
    }
    pr = ia64_fw_debug_getq(env, FW_DEBUG_CTX_PR) | 1;
    for (i = 1; i < IA64_PR_COUNT; i++) {
        env->pr[i] = (pr >> i) & 1;
    }
    env->pr[IA64_PR_TRUE] = 1;
    for (i = 0; i < IA64_BR_COUNT; i++) {
        env->br[i] = ia64_fw_debug_getq(env, FW_DEBUG_CTX_B0 + i * 8);
    }

    env->ar_rsc = ia64_fw_debug_getq(env, FW_DEBUG_CTX_AR_RSC);
    env->ar_fcr = ia64_fw_debug_getq(env, FW_DEBUG_CTX_AR_FCR);
    env->ar_eflag = ia64_fw_debug_getq(env, FW_DEBUG_CTX_AR_EFLAG);
    env->ar_csd = ia64_fw_debug_getq(env, FW_DEBUG_CTX_AR_CSD);
    env->ar_ssd = ia64_fw_debug_getq(env, FW_DEBUG_CTX_AR_SSD);
    env->ar_cflg = ia64_fw_debug_getq(env, FW_DEBUG_CTX_AR_CFLG);
    env->ar_fsr = ia64_fw_debug_getq(env, FW_DEBUG_CTX_AR_FSR);
    env->ar_fir = ia64_fw_debug_getq(env, FW_DEBUG_CTX_AR_FIR);
    env->ar_fdr = ia64_fw_debug_getq(env, FW_DEBUG_CTX_AR_FDR);
    env->ar_ccv = ia64_fw_debug_getq(env, FW_DEBUG_CTX_AR_CCV);
    env->ar_unat = ia64_fw_debug_getq(env, FW_DEBUG_CTX_AR_UNAT);
    env->ar_fpsr = ia64_fw_debug_getq(env, FW_DEBUG_CTX_AR_FPSR);
    env->ar_pfs = ia64_fw_debug_getq(env, FW_DEBUG_CTX_AR_PFS);
    env->ar_lc = ia64_fw_debug_getq(env, FW_DEBUG_CTX_AR_LC);
    env->ar_ec = ia64_fw_debug_getq(env, FW_DEBUG_CTX_AR_EC);

    env->cr_dcr = ia64_fw_debug_getq(env, FW_DEBUG_CTX_CR_DCR);
    ia64_write_cr(env, IA64_CR_ITM,
                  ia64_fw_debug_getq(env, FW_DEBUG_CTX_CR_ITM));
    ia64_write_cr(env, IA64_CR_IVA,
                  ia64_fw_debug_getq(env, FW_DEBUG_CTX_CR_IVA));
    ia64_write_cr(env, IA64_CR_PTA,
                  ia64_fw_debug_getq(env, FW_DEBUG_CTX_CR_PTA));
    env->cr_ipsr = ipsr;
    env->cr_isr = ia64_fw_debug_getq(env, FW_DEBUG_CTX_CR_ISR);
    env->cr_iip = ia64_fw_debug_getq(env, FW_DEBUG_CTX_CR_IIP);
    env->cr_ifa = ia64_fw_debug_getq(env, FW_DEBUG_CTX_CR_IFA);
    env->cr_itir = ia64_fw_debug_getq(env, FW_DEBUG_CTX_CR_ITIR);
    env->cr_iipa = ia64_fw_debug_getq(env, FW_DEBUG_CTX_CR_IIPA);
    env->cr_ifs = ia64_fw_debug_getq(env, FW_DEBUG_CTX_CR_IFS);
    env->cr_iim = ia64_fw_debug_getq(env, FW_DEBUG_CTX_CR_IIM);
    env->cr_iha = ia64_fw_debug_getq(env, FW_DEBUG_CTX_CR_IHA);
    for (i = 0; i < 8; i++) {
        env->dbr[i] = ia64_fw_debug_getq(env, FW_DEBUG_CTX_DBR0 + i * 8);
    }
    for (i = 0; i < 8; i++) {
        env->ibr[i] = ia64_fw_debug_getq(env, FW_DEBUG_CTX_IBR0 + i * 8);
    }

    ia64_firmware_debug_state(env)->handler_active = false;
    ia64_firmware_debug_state(env)->context_valid = false;
    ia64_firmware_debug_state(env)->rse_valid = false;
    ia64_rfi(env, env->ip, 0);
    return 1;
}

static bool ia64_opcode_has_firmware_unaligned_load_assist(Ia64Opcode opcode)
{
    switch (opcode) {
    case IA64_OP_LD1:
    case IA64_OP_LD2:
    case IA64_OP_LD4:
    case IA64_OP_LD8:
    case IA64_OP_LD1S:
    case IA64_OP_LD2S:
    case IA64_OP_LD4S:
    case IA64_OP_LD8S:
    case IA64_OP_LD1A:
    case IA64_OP_LD2A:
    case IA64_OP_LD4A:
    case IA64_OP_LD8A:
    case IA64_OP_LD1SA:
    case IA64_OP_LD2SA:
    case IA64_OP_LD4SA:
    case IA64_OP_LD8SA:
    case IA64_OP_LD8FILL:
    case IA64_OP_LD1C_CLR:
    case IA64_OP_LD2C_CLR:
    case IA64_OP_LD4C_CLR:
    case IA64_OP_LD8C_CLR:
    case IA64_OP_LD1C_NC:
    case IA64_OP_LD2C_NC:
    case IA64_OP_LD4C_NC:
    case IA64_OP_LD8C_NC:
        return true;
    default:
        return false;
    }
}

static bool ia64_opcode_has_firmware_unaligned_store_assist(Ia64Opcode opcode)
{
    switch (opcode) {
    case IA64_OP_ST1:
    case IA64_OP_ST2:
    case IA64_OP_ST4:
    case IA64_OP_ST8:
    case IA64_OP_ST1REL:
    case IA64_OP_ST2REL:
    case IA64_OP_ST4REL:
    case IA64_OP_ST8REL:
    case IA64_OP_ST8SPILL:
        return true;
    default:
        return false;
    }
}

static bool ia64_opcode_is_control_speculative_load(Ia64Opcode opcode)
{
    switch (opcode) {
    case IA64_OP_LD1S:
    case IA64_OP_LD2S:
    case IA64_OP_LD4S:
    case IA64_OP_LD8S:
    case IA64_OP_LD1SA:
    case IA64_OP_LD2SA:
    case IA64_OP_LD4SA:
    case IA64_OP_LD8SA:
        return true;
    default:
        return false;
    }
}

static bool ia64_opcode_is_data_speculative_load(Ia64Opcode opcode)
{
    switch (opcode) {
    case IA64_OP_LD1A:
    case IA64_OP_LD2A:
    case IA64_OP_LD4A:
    case IA64_OP_LD8A:
    case IA64_OP_LD1SA:
    case IA64_OP_LD2SA:
    case IA64_OP_LD4SA:
    case IA64_OP_LD8SA:
        return true;
    default:
        return false;
    }
}

static bool ia64_opcode_is_check_load_clear(Ia64Opcode opcode)
{
    switch (opcode) {
    case IA64_OP_LD1C_CLR:
    case IA64_OP_LD2C_CLR:
    case IA64_OP_LD4C_CLR:
    case IA64_OP_LD8C_CLR:
        return true;
    default:
        return false;
    }
}

static bool ia64_opcode_is_check_load_no_clear(Ia64Opcode opcode)
{
    switch (opcode) {
    case IA64_OP_LD1C_NC:
    case IA64_OP_LD2C_NC:
    case IA64_OP_LD4C_NC:
    case IA64_OP_LD8C_NC:
        return true;
    default:
        return false;
    }
}

static bool ia64_opcode_is_check_load(Ia64Opcode opcode)
{
    return ia64_opcode_is_check_load_clear(opcode) ||
           ia64_opcode_is_check_load_no_clear(opcode);
}

static bool ia64_opcode_is_fill_load(Ia64Opcode opcode)
{
    switch (opcode) {
    case IA64_OP_LD8FILL:
        return true;
    default:
        return false;
    }
}

static bool ia64_debug_read_code(CPUState *cs, uint64_t addr, void *buf,
                                 size_t size)
{
    return ia64_exec_debug_read(cs, addr, buf, size);
}

static bool ia64_firmware_data_access(CPUIA64State *env, uint64_t addr,
                                      void *buf, size_t size, bool is_write)
{
    uint64_t pa;

    if (!ia64_translate_data_access(env, addr, is_write, &pa)) {
        return false;
    }
    return ia64_exec_physical_rw(pa, buf, size, is_write);
}

static void ia64_resume_after_instruction(CPUIA64State *env, uint64_t ip,
                                          uint8_t slot)
{
    env->psr &= ~IA64_PSR_RI_MASK;
    if (slot >= 2) {
        env->ip = ip + 16;
    } else {
        env->ip = ip;
        env->psr |= (uint64_t)(slot + 1) << IA64_PSR_RI_SHIFT;
    }
}

static void ia64_gr_nat_clear_runtime(CPUIA64State *env, uint8_t reg)
{
    if (reg == IA64_GR_ZERO) {
        return;
    }

    env->nat[reg / 64] &= ~(1ULL << (reg % 64));
    ia64_rse_mark_gr_dirty(env, reg);
}

static bool ia64_gr_nat_get_runtime(CPUIA64State *env, uint8_t reg)
{
    if (reg == IA64_GR_ZERO) {
        return false;
    }

    return (env->nat[reg / 64] >> (reg % 64)) & 1;
}

static void ia64_gr_nat_set_runtime(CPUIA64State *env, uint8_t reg, bool nat)
{
    if (reg == IA64_GR_ZERO) {
        return;
    }

    if (nat) {
        env->nat[reg / 64] |= 1ULL << (reg % 64);
    } else {
        env->nat[reg / 64] &= ~(1ULL << (reg % 64));
    }
    ia64_rse_mark_gr_dirty(env, reg);
}

static void ia64_unaligned_base_update(CPUIA64State *env,
                                       const Ia64Instruction *insn,
                                       uint64_t addr)
{
    if (insn->operands.common.source2 == IA64_GR_ZERO) {
        return;
    }

    if (insn->reg_base_update) {
        bool base_nat = ia64_gr_nat_get_runtime(
            env, insn->operands.common.source2);
        bool inc_nat = ia64_gr_nat_get_runtime(
            env, insn->operands.common.source1);

        env->gr[insn->operands.common.source2] =
            addr + env->gr[insn->operands.common.source1];
        ia64_gr_nat_set_runtime(env, insn->operands.common.source2,
                                base_nat || inc_nat);
    } else if (insn->imm_base_update) {
        env->gr[insn->operands.common.source2] =
            addr + insn->operands.common.immediate;
        ia64_rse_mark_gr_dirty(env, insn->operands.common.source2);
    }
}

static void ia64_firmware_defer_speculative_load(CPUIA64State *env,
                                                 const Ia64Instruction *insn)
{
    if (insn->operands.common.destination != IA64_GR_ZERO) {
        env->gr[insn->operands.common.destination] = 0;
        ia64_gr_nat_set_runtime(env, insn->operands.common.destination, true);
    }
    if (env->alat_state.alat_full &&
        ia64_opcode_is_data_speculative_load(insn->opcode)) {
        ia64_alat_invalidate_reg(env, insn->operands.common.destination);
    }
}

/*
 * The SAL/firmware unaligned assist emulates a misaligned reference with a
 * single translated access, so it can only service references that stay within
 * one translation.  A reference is serviceable when its first and last bytes
 * map to contiguous physical addresses: this correctly admits large pages,
 * where an access may cross a 4KB sub-boundary yet remain inside one page,
 * while a reference whose bytes fall in different (or unmapped) translations
 * remains an architectural fault.  Using the actual translation instead of a
 * hardcoded 4KB span is required for OS loaders, which map their working set
 * with megabyte-sized translation registers.
 */
static bool ia64_firmware_unaligned_spans_translation(CPUIA64State *env,
                                                      uint64_t addr,
                                                      uint32_t size)
{
    uint64_t pa_first;
    uint64_t pa_last;

    if (size <= 1) {
        return false;
    }
    if (!ia64_data_address_to_phys(env, addr, &pa_first) ||
        !ia64_data_address_to_phys(env, addr + size - 1, &pa_last)) {
        return true;
    }
    return pa_last != pa_first + (size - 1);
}

bool ia64_try_emulate_firmware_unaligned(CPUState *cs,
                                         uint64_t fault_addr,
                                         uint8_t fault_slot)
{
    IA64CPU *cpu = ia64_cpu_from_cpu_state(cs);
    CPUIA64State *env = &cpu->env;
    uint8_t bundle[16];
    uint8_t data[16];
    uint64_t low, high;
    uint8_t template_code;
    const IA64TemplateInfo *template_info;
    Ia64Instruction insn;
    uint64_t slots[3];
    uint64_t addr;
    MemOp memop;
    uint32_t size;

    /*
     * Model the SAL/firmware IVT's unaligned assist only before the guest has
     * installed its own IVA.  Page-spanning and semaphore references remain
     * architectural faults.
     */
    if (!ia64_firmware_registered(&env->firmware) ||
        env->cr_iva != env->firmware.ivt ||
        !(env->psr & IA64_PSR_IC) ||
        fault_slot > 2) {
        return false;
    }

    if (!ia64_debug_read_code(cs, env->exception_state.fault_ip,
                              bundle, sizeof(bundle))) {
        return false;
    }

    low = ldq_le_p(bundle);
    high = ldq_le_p(bundle + 8);
    template_code = ia64_bundle_template_code(low);
    template_info = ia64_template_info(template_code);
    if (!template_info->defined) {
        return false;
    }

    slots[0] = ia64_bundle_slot(low, high, 0);
    slots[1] = ia64_bundle_slot(low, high, 1);
    slots[2] = ia64_bundle_slot(low, high, 2);
    insn = ia64_decode_insn(template_info->units[fault_slot],
                            slots[fault_slot],
                            env->exception_state.fault_ip, fault_slot);
    if (!insn.valid) {
        return false;
    }

    if (ia64_opcode_has_firmware_unaligned_load_assist(insn.opcode)) {
        bool check_load_clear = ia64_opcode_is_check_load_clear(insn.opcode);
        bool check_load_no_clear =
            ia64_opcode_is_check_load_no_clear(insn.opcode);

        memop = ia64_runtime_data_memop(
            env, ia64_memop_for_opcode(insn.opcode));
        size = ia64_memop_size(memop);
        addr = env->gr[insn.operands.common.source2];
        if (addr != fault_addr ||
            ia64_firmware_unaligned_spans_translation(env, addr, size)) {
            return false;
        }

        if (ia64_opcode_is_control_speculative_load(insn.opcode) &&
            (env->cr_isr & IA64_ISR_SP) &&
            (env->cr_isr & IA64_ISR_ED)) {
            ia64_firmware_defer_speculative_load(env, &insn);
            ia64_unaligned_base_update(env, &insn, addr);
            ia64_resume_after_instruction(
                env, env->exception_state.fault_ip, fault_slot);
            env->exception_state.exception = IA64_EXCP_NONE;
            return true;
        }

        /*
         * IA-64 SDM Vol. 2, 17.3.1 requires unaligned handlers to force
         * failed data-speculative loads; ALAT cannot track all misalignment
         * sizes for later store-conflict checks.
         */
        if (ia64_opcode_is_data_speculative_load(insn.opcode)) {
            ia64_alat_invalidate_reg(env,
                                     insn.operands.common.destination);
            ia64_unaligned_base_update(env, &insn, addr);
            ia64_resume_after_instruction(
                env, env->exception_state.fault_ip, fault_slot);
            env->exception_state.exception = IA64_EXCP_NONE;
            return true;
        }

        if (env->alat_state.alat_full &&
            ia64_opcode_is_check_load(insn.opcode) &&
            ia64_alat_check_load_addr(env,
                                      insn.operands.common.destination,
                                      addr, size, check_load_clear)) {
            ia64_unaligned_base_update(env, &insn, addr);
            ia64_resume_after_instruction(
                env, env->exception_state.fault_ip, fault_slot);
            env->exception_state.exception = IA64_EXCP_NONE;
            return true;
        }

        if (size > sizeof(data) ||
            !ia64_firmware_data_access(env, addr, data, size, false)) {
            return false;
        }
        if (insn.operands.common.destination != IA64_GR_ZERO) {
            env->gr[insn.operands.common.destination] = ldm_p(data, memop);
            if (ia64_opcode_is_fill_load(insn.opcode)) {
                uint64_t nat = (env->ar_unat >> ((addr >> 3) & 0x3f)) & 1;

                if (nat) {
                    ia64_gr_nat_set_runtime(
                        env, insn.operands.common.destination, true);
                } else {
                    ia64_gr_nat_clear_runtime(
                        env, insn.operands.common.destination);
                }
            } else {
                ia64_gr_nat_clear_runtime(
                    env, insn.operands.common.destination);
                if (check_load_no_clear && env->alat_state.alat_full) {
                    ia64_alat_set(env, insn.operands.common.destination,
                                  addr, size);
                }
            }
        }
        ia64_unaligned_base_update(env, &insn, addr);
        ia64_resume_after_instruction(env, env->exception_state.fault_ip,
                                      fault_slot);
        env->exception_state.exception = IA64_EXCP_NONE;
        return true;
    }

    if (ia64_opcode_has_firmware_unaligned_store_assist(insn.opcode)) {
        memop = ia64_runtime_data_memop(
            env, ia64_memop_for_opcode(insn.opcode));
        size = ia64_memop_size(memop);
        addr = env->gr[insn.operands.common.source2];
        if (addr != fault_addr ||
            ia64_firmware_unaligned_spans_translation(env, addr, size) ||
            size > sizeof(data)) {
            return false;
        }

        stm_p(data, memop, env->gr[insn.operands.common.source1]);
        if (!ia64_firmware_data_access(env, addr, data, size, true)) {
            return false;
        }
        if (env->alat_state.alat_full) {
            ia64_invalidate_alat_store(env, addr, size);
        }
        if (insn.opcode == IA64_OP_ST8SPILL) {
            ia64_system_st_spill_unat(env,
                                      insn.operands.common.source1, addr);
        }
        ia64_unaligned_base_update(env, &insn, addr);
        ia64_resume_after_instruction(env, env->exception_state.fault_ip,
                                      fault_slot);
        env->exception_state.exception = IA64_EXCP_NONE;
        return true;
    }

    return false;
}

/*
 * SAL_PROC physical re-entry bridge (entry.S sal_runtime_entry, break
 * 0x100005/0x100006).  The SST advertises a mode-agnostic SAL entry stub; a
 * virtual-mode caller (e.g. the NT HAL, which maps the SAL code region and
 * branches to it with translation on - SAL spec 3.1) cannot execute the
 * firmware's fixed-address C dispatcher directly, so the enter break saves
 * the caller's context, re-points the RSE at a firmware-owned physical
 * backing store and resumes at the C dispatcher in physical mode.  The
 * dispatcher's br.ret lands on sal_runtime_return, whose break restores the
 * saved context and completes the stub's br.ret back to the caller.  SAL
 * calls are not re-entrant (the OS serializes them).
 */
uint32_t ia64_sal_runtime_enter(CPUIA64State *env)
{
    IA64SalBridgeState *bridge = &env->sal_bridge;
    uint64_t block[4];
    uint64_t entry, gp, stack, bstore;
    uint64_t sal_runtime_base;

    if (bridge->active) {
        return 0;
    }
    if (env->firmware.sal_block == 0 ||
        !ia64_exec_physical_rw(env->firmware.sal_block, block,
                               sizeof(block), false)) {
        return 0;
    }
    entry = le64_to_cpu(block[0]);
    gp = le64_to_cpu(block[1]);
    stack = le64_to_cpu(block[2]);
    bstore = le64_to_cpu(block[3]);
    if (!entry || !gp || !stack || !bstore ||
        (entry & (IA64_BUNDLE_SIZE - 1)) || (bstore & 7)) {
        return 0;
    }

    /*
     * The dispatch block publishes CPU 0's physical stack and backing
     * store; every processor owns a private slot at a fixed stride so
     * concurrent SAL calls cannot share re-entry memory.
     */
    sal_runtime_base = ia64_fw_cpu_assist_base(env) +
                       IA64_FW_SAL_RUNTIME_OFFSET;
    if (stack >= sal_runtime_base &&
        stack < sal_runtime_base + IA64_FW_SAL_RUNTIME_SLOT_SIZE &&
        bstore >= sal_runtime_base &&
        bstore < sal_runtime_base + IA64_FW_SAL_RUNTIME_SLOT_SIZE) {
        unsigned index = ia64_fw_debug_cpu_index(env);

        stack += (uint64_t)index * IA64_FW_SAL_RUNTIME_SLOT_SIZE;
        bstore += (uint64_t)index * IA64_FW_SAL_RUNTIME_SLOT_SIZE;
    }

    bridge->psr = env->psr;
    bridge->b0 = env->br[IA64_BR_RETURN_LINK];
    bridge->sp = env->gr[IA64_GR_STACK_POINTER];
    bridge->gp = env->gr[IA64_GR_GLOBAL_POINTER];
    bridge->rsc = env->ar_rsc;
    ia64_rse_state_save(env, &bridge->rse);

    /*
     * Re-point the RSE at the firmware backing store.  The current frame
     * (the caller's outgoing SAL arguments) stays in the physical file;
     * nothing below it is dirty, so spills during the physical call go to
     * firmware memory and the snapshot restore discards them on exit.
     */
    env->ar_rsc = 0;
    env->ar_bspstore = bstore;
    env->ar_bsp = bstore;
    env->ar_rnat = 0;
    env->rse.rse_rnat_low = bstore;
    env->rse.rse_dirty = 0;
    env->rse.rse_dirty_nat = 0;
    env->rse.rse_clean = 0;
    env->rse.rse_clean_nat = 0;
    env->rse.rse_invalid = IA64_STACKED_GR_COUNT - env->cfm_sof;
    env->rse.rse_cfle = false;

    env->gr[IA64_GR_GLOBAL_POINTER] = gp;
    env->gr[IA64_GR_STACK_POINTER] = stack;
    ia64_gr_nat_set(env, IA64_GR_GLOBAL_POINTER, false);
    ia64_gr_nat_set(env, IA64_GR_STACK_POINTER, false);
    env->br[IA64_BR_RETURN_LINK] = env->firmware.sal_return;

    ia64_set_psr(env, env->psr & ~(IA64_PSR_DT | IA64_PSR_RT |
                                   IA64_PSR_IT | IA64_PSR_I |
                                   IA64_PSR_RI_MASK));
    ia64_tlb_serialize(env, 1, 1);
    env->ip = entry;
    env->exception_state.fault_slot = 0;
    env->instruction_group_start = true;
    bridge->active = true;
    return 1;
}

uint32_t ia64_sal_runtime_exit(CPUIA64State *env)
{
    IA64SalBridgeState *bridge = &env->sal_bridge;
    uint64_t pfs;
    uint8_t ppl;

    if (!bridge->active) {
        return 0;
    }
    bridge->active = false;

    /* Return to the caller's world: RSE snapshot + statics + psr. */
    ia64_rse_state_restore(env, &bridge->rse);
    env->ar_rsc = bridge->rsc;
    env->gr[IA64_GR_GLOBAL_POINTER] = bridge->gp;
    env->gr[IA64_GR_STACK_POINTER] = bridge->sp;
    ia64_gr_nat_set(env, IA64_GR_GLOBAL_POINTER, false);
    ia64_gr_nat_set(env, IA64_GR_STACK_POINTER, false);
    env->br[IA64_BR_RETURN_LINK] = bridge->b0;
    ia64_set_psr(env, bridge->psr);
    ia64_tlb_serialize(env, 1, 1);

    /*
     * Complete the stub's br.ret b0 on the caller's behalf: pop the call
     * frame and resume at the saved return address.  SAL results stay in
     * r8-r11 as written by the dispatcher.
     */
    pfs = env->ar_pfs;
    ppl = (pfs & IA64_PFS_PPL_MASK) >> IA64_PFS_PPL_SHIFT;
    env->ip = ia64_ip_bundle_addr(bridge->b0);
    env->psr &= ~IA64_PSR_RI_MASK;
    if (ia64_psr_cpl(env->psr) < ppl) {
        ia64_set_psr(env, (env->psr & ~IA64_PSR_CPL_MASK) |
                          ((uint64_t)ppl << IA64_PSR_CPL_SHIFT));
    }
    ia64_rse_pop_return_frame(env, pfs);
    env->exception_state.fault_slot = 0;
    env->instruction_group_start = true;
    return 1;
}
