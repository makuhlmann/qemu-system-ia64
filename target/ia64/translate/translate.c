/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * IA-64 TCG translation pipeline and translation-block lifecycle.
 */

#include "qemu/osdep.h"
#include "qemu/atomic.h"
#include "qemu/log.h"

#include "arch/arch.h"
#include "cpu.h"
#include "debug.h"
#include "decode/decode.h"
#include "decoder.h"
#include "ia32/translate.h"
#include "translate/translate.h"

#include "exec/helper-proto.h"
#include "exec/helper-gen.h"
#include "exec/translation-block.h"
#include "exec/translator.h"
#include "tcg/tcg-op.h"

TCGv_i64 cpu_ip;
TCGv_i64 cpu_gr[IA64_GR_COUNT];
TCGv_i64 cpu_pr[IA64_PR_COUNT];
TCGv_i64 cpu_br[IA64_BR_COUNT];
TCGv_i64 cpu_psr;
static TCGv_i64 cpu_fr[IA64_FR_COUNT];
static TCGv_i64 cpu_fr_nat[2];
static TCGv_i64 cpu_fr_sig[2];
static TCGv_i64 cpu_fr_ext_valid[2];
static TCGv_i64 cpu_fr_int_origin[2];
static TCGv_i64 cpu_nat[2];
static TCGv_i64 cpu_rse_gr_dirty[2];

#define IA64_COUNTED_SELF_BUDGET 4096
#define IA64_CLOOP_ZERO_ST1_MAX IA64_COUNTED_SELF_BUDGET

static bool ia64_insn_may_set_fault_suppression(const Ia64Instruction *insn)
{
    switch (insn->opcode) {
    case IA64_OP_SSM:
        return insn->operands.system.immediate & IA64_PSR_FAULT_SUPPRESS_MASK;
    case IA64_OP_MOV_GRPSR:
        return insn->operands.system.immediate == 0;
    default:
        return false;
    }
}

static bool ia64_insn_may_set_psr_ic(const Ia64Instruction *insn)
{
    switch (insn->opcode) {
    case IA64_OP_SSM:
        return insn->operands.system.immediate & IA64_PSR_IC;
    case IA64_OP_MOV_GRPSR:
        return true;
    default:
        return false;
    }
}

static bool ia64_insn_may_modify_psr_ic(const Ia64Instruction *insn)
{
    switch (insn->opcode) {
    case IA64_OP_SSM:
    case IA64_OP_RSM:
    case IA64_OP_MOV_GRPSR:
        return true;
    default:
        return false;
    }
}

static bool ia64_insn_may_modify_psr_ri(const Ia64Instruction *insn)
{
    return insn->opcode == IA64_OP_MOV_GRPSR;
}

/*
 * Conservatively true for any instruction that can write PSR.ac.  ssm/rsm can
 * set/clear it via their system-mask immediate; sum/rum and mov psr.um write
 * the user mask; mov psr.l writes PSR{31:0}.  Used to invalidate the TB-flag
 * view of PSR.ac for later ld/st in the same bundle.
 */
static bool ia64_insn_may_modify_psr_ac(const Ia64Instruction *insn)
{
    switch (insn->opcode) {
    case IA64_OP_SSM:
    case IA64_OP_RSM:
    case IA64_OP_SUM_UM:
    case IA64_OP_RUM:
        return insn->operands.system.immediate & IA64_PSR_AC;
    case IA64_OP_MOV_GRUM:
    case IA64_OP_MOV_GRPSR:
        return true;
    default:
        return false;
    }
}

bool ia64_insn_is_empty_hint(const Ia64Instruction *insn)
{
    switch (insn->opcode) {
    case IA64_OP_NOP:
    case IA64_OP_HINT_I:
    case IA64_OP_HINT_B:
    case IA64_OP_HINT_F:
    case IA64_OP_HINT_X:
    case IA64_OP_BRP:
        return true;
    default:
        return false;
    }
}

static bool ia64_instruction_address_matches_physical_entry(CPUIA64State *env,
                                                            uint64_t address,
                                                            uint64_t entry_pa)
{
    const IA64TlbEntry *entry;
    uint64_t pa;
    uint8_t perm;
    uint32_t rid;

    if (!(env->psr & IA64_PSR_IT)) {
        /*
         * Bit 63 of a physical address selects the uncacheable alias of the
         * same location (SDM vol. 2 4.4.6); SAL_A returns to PAL_RESET
         * through it.
         */
        return (address & ~IA64_PHYS_UC_BIT) == entry_pa;
    }

    /*
     * A break 0x100000 only ever sits at a PAL entry.  If the virtual IP
     * equals the physical entry, it is that entry reached through its identity
     * mapping (virtual==physical), so match it directly -- BEFORE the resolvers
     * below.  The firmware-identity and SAL-boot resolvers cover only our own
     * firmware, not a vendor image whose PAL stub sits at 0xff100000, and the
     * cached-TLB lookup can transiently miss the firmware's own ITLB entry (the
     * instruction fetch that reached this break used the softmmu TLB while this
     * match walks the guest TLB, so the two diverge).  Worse, if one of those
     * resolvers instead resolves this identity-mapped IP to some OTHER physical
     * address, the pa==entry_pa test wrongly fails and the PAL-hook
     * interception, decided here at translate time, drops the break into the
     * firmware's Break vector, which rfi-loops.  Deciding the identity case up
     * front avoids both the transient miss and the mis-resolution.  (This
     * supersedes the earlier "last resort" fallback that ran only after every
     * resolver had failed, which a wrong resolution slipped past.)
     */
    if (address == entry_pa) {
        return true;
    }

    if (ia64_firmware_identity_pa(&env->firmware, env->cr_iva, address, env->psr,
                                  address, &pa) ||
        ia64_sal_boot_virtual_pa(env, address, &pa)) {
        return pa == entry_pa;
    }

    rid = ia64_region_rid(env, address);
    entry = ia64_tlb_find_cached(env, address, rid, true);
    if (entry) {
        ia64_tlb_entry_translate(entry, address, ia64_psr_cpl(env->psr),
                                 &pa, &perm);
    }
    if (entry && (perm & IA64_TLB_X)) {
        return pa == entry_pa;
    }

    if (ia64_sal_boot_identity_pa_type(env, address, &pa, true)) {
        return pa == entry_pa;
    }

    return false;
}

bool ia64_is_pal_proc_break(CPUIA64State *env, uint64_t address)
{
    if (env->pal.pal_proc_reset_addr != 0 &&
        ia64_instruction_address_matches_physical_entry(
            env, address, env->pal.pal_proc_reset_addr)) {
        return true;
    }

    return qatomic_load_acquire(&env->pal.pal_proc_copy_valid) &&
           ia64_instruction_address_matches_physical_entry(
               env, address, qatomic_read(&env->pal.pal_proc_copy_addr));
}

/* SAL's return to PAL_RESET after the RECOVERY_CHECK call (break 0x100007). */
bool ia64_is_pal_reset_return_break(CPUIA64State *env, uint64_t address)
{
    return env->pal.pal_reset_return_addr != 0 &&
           ia64_instruction_address_matches_physical_entry(
               env, address, env->pal.pal_reset_return_addr);
}

bool ia64_is_sal_runtime_break(CPUIA64State *env, uint64_t address,
                               uint64_t imm)
{
    uint64_t entry_pa = imm == 0x100005 ? env->firmware.sal_entry
                                        : env->firmware.sal_return;

    return entry_pa != 0 &&
           ia64_instruction_address_matches_physical_entry(env, address,
                                                           entry_pa);
}

bool ia64_is_firmware_debug_break(CPUIA64State *env, uint64_t address,
                                  uint64_t imm)
{
    const IA64FirmwareRegistration *fw = &env->firmware;

    if (!ia64_firmware_registered(fw)) {
        return false;
    }
    if (imm == 0x100002) {
        return address >= fw->ivt && address - fw->ivt < 0x8000;
    }
    if (imm == 0x100003 || imm == 0x100004) {
        return ia64_firmware_in_image(fw, address);
    }
    return false;
}

static MemOp ia64_memop_with_endian(MemOp memop, bool big_endian)
{
    if ((memop & MO_SIZE) == MO_8) {
        return memop & ~MO_BSWAP;
    }
    return (memop & ~MO_BSWAP) | (big_endian ? MO_BE : MO_LE);
}

MemOp ia64_data_memop(DisasContext *ctx, MemOp memop)
{
    return ia64_memop_with_endian(memop, ctx->memory.be_data);
}

static bool ia64_insn_has_base_update(const Ia64Instruction *insn)
{
    return insn->reg_base_update || insn->imm_base_update;
}

static bool ia64_load_base_update_has_same_target(const Ia64Instruction *insn)
{
    return ia64_opcode_is_load(insn->opcode) &&
           ia64_insn_has_base_update(insn) &&
           insn->operands.common.destination == insn->operands.common.source2;
}

TCGv_i64 ia64_gr_src(uint8_t reg)
{
    return reg == 0 ? tcg_constant_i64(0) : cpu_gr[reg];
}

bool ia64_insn_must_start_group(const Ia64Instruction *insn)
{
    switch (insn->opcode) {
    case IA64_OP_ALLOC:
    case IA64_OP_FLUSHRS:
    case IA64_OP_LOADRS:
        return true;
    default:
        return false;
    }
}

bool ia64_insn_must_end_group(const Ia64Instruction *insn)
{
    switch (insn->opcode) {
    case IA64_OP_BSW0:
    case IA64_OP_BSW1:
    case IA64_OP_CLRRRB:
    case IA64_OP_CLRRRB_PR:
    case IA64_OP_COVER:
    case IA64_OP_ITC_D:
    case IA64_OP_ITC_I:
    case IA64_OP_PTC_G:
    case IA64_OP_PTC_GA:
    case IA64_OP_RFI:
        return true;
    default:
        return false;
    }
}

bool ia64_insn_requires_slot2(const Ia64Instruction *insn)
{
    switch (insn->opcode) {
    case IA64_OP_BR_CEXIT:
    case IA64_OP_BR_CLOOP:
    case IA64_OP_BR_CTOP:
    case IA64_OP_BR_WEXIT:
    case IA64_OP_BR_WTOP:
        return true;
    default:
        return false;
    }
}

static bool ia64_insn_is_fp_load_pair(const Ia64Instruction *insn)
{
    switch (insn->opcode) {
    case IA64_OP_LDFP8:
    case IA64_OP_LDFPD:
    case IA64_OP_LDFPS:
        return true;
    default:
        return false;
    }
}

/*
 * The bank conflict check of ldfp uses physical FR numbers (SDM Vol 2
 * Illegal Operation fault).  A pair whose registers are both static or both
 * rotating keeps its parity under rotation; the other pairs are checked at
 * run time by ia64_gen_check_fp_pair_bank().
 */
bool ia64_insn_has_invalid_fp_pair(const Ia64Instruction *insn)
{
    uint8_t f1 = insn->operands.common.destination;
    uint8_t f2 = insn->operands.common.source1;

    if (!ia64_insn_is_fp_load_pair(insn)) {
        return false;
    }
    return f1 <= 1 || f2 <= 1 ||
           ((f1 < 32) == (f2 < 32) && ((f1 ^ f2) & 1) == 0);
}

/* A rotating FR has the parity of its logical number XOR CFM.rrb.fr. */
static void ia64_gen_check_fp_pair_bank(const Ia64Instruction *insn)
{
    uint8_t f1 = insn->operands.common.destination;
    uint8_t f2 = insn->operands.common.source1;
    TCGv_i32 rrb_parity;
    TCGLabel *valid;

    if (!ia64_insn_is_fp_load_pair(insn) || (f1 < 32) == (f2 < 32)) {
        return;
    }

    rrb_parity = tcg_temp_new_i32();
    valid = gen_new_label();
    tcg_gen_ld8u_i32(rrb_parity, tcg_env, offsetof(CPUIA64State, cfm_rrb_fr));
    tcg_gen_andi_i32(rrb_parity, rrb_parity, 1);
    tcg_gen_brcondi_i32(((f1 ^ f2) & 1) ? TCG_COND_EQ : TCG_COND_NE,
                        rrb_parity, 0, valid);
    ia64_gen_raise_exception(IA64_EXCP_ILLEGAL, insn->address, insn->raw,
                             insn->slot);
    gen_set_label(valid);
}

static bool ia64_insn_writes_fr_f1(const Ia64Instruction *insn)
{
    switch (insn->opcode) {
    case IA64_OP_LDFS:
    case IA64_OP_LDFD:
    case IA64_OP_LDF8:
    case IA64_OP_LDFE:
    case IA64_OP_LDF_FILL:
    case IA64_OP_SETF_S:
    case IA64_OP_SETF_D:
    case IA64_OP_SETF_EXP:
    case IA64_OP_SETF_SIG:
    case IA64_OP_FADD:
    case IA64_OP_FSUB:
    case IA64_OP_FMPY:
    case IA64_OP_FMA:
    case IA64_OP_FMS:
    case IA64_OP_FNMA:
    case IA64_OP_FNORM:
    case IA64_OP_XMA_L:
    case IA64_OP_XMA_H:
    case IA64_OP_XMA_HU:
    case IA64_OP_XMPY_HU:
    case IA64_OP_FMOV:
    case IA64_OP_FMERGE:
    case IA64_OP_FMERGE_S:
    case IA64_OP_FMERGE_SE:
    case IA64_OP_FCVT_XF:
    case IA64_OP_FCVT_FX:
    case IA64_OP_FCVT_FXU:
    case IA64_OP_FMIN:
    case IA64_OP_FMAX:
    case IA64_OP_FAMIN:
    case IA64_OP_FAMAX:
    case IA64_OP_FRCPA:
    case IA64_OP_FRSQRTA:
    case IA64_OP_FSELECT:
    case IA64_OP_FAND:
    case IA64_OP_FANDCM:
    case IA64_OP_FOR:
    case IA64_OP_FXOR:
    case IA64_OP_FSWAP:
    case IA64_OP_FSWAP_NL:
    case IA64_OP_FSWAP_NR:
    case IA64_OP_FMIX_LR:
    case IA64_OP_FMIX_R:
    case IA64_OP_FMIX_L:
    case IA64_OP_FSXT_R:
    case IA64_OP_FSXT_L:
    case IA64_OP_FPACK:
    case IA64_OP_FPMERGE:
    case IA64_OP_FPMERGE_S:
    case IA64_OP_FPMERGE_SE:
    case IA64_OP_FPMIN:
    case IA64_OP_FPMAX:
    case IA64_OP_FPAMIN:
    case IA64_OP_FPAMAX:
    case IA64_OP_FPCMP:
    case IA64_OP_FPCVT:
    case IA64_OP_FPMA:
    case IA64_OP_FPMS:
    case IA64_OP_FPNMA:
    case IA64_OP_FPRCPA:
    case IA64_OP_FPRSQRTA:
        return true;
    default:
        return false;
    }
}

static bool ia64_insn_writes_gr_r1(const Ia64Instruction *insn)
{
    Ia64Opcode opcode = insn->opcode;

    if ((opcode >= IA64_OP_ADDS && opcode <= IA64_OP_XOR_IMM) ||
        (opcode >= IA64_OP_LD1 && opcode <= IA64_OP_LD8FILL) ||
        (opcode >= IA64_OP_SHL && opcode <= IA64_OP_ZXT4) ||
        (opcode >= IA64_OP_PADD1 && opcode <= IA64_OP_PSHRADD2) ||
        (opcode >= IA64_OP_XCHG1 && opcode <= IA64_OP_FETCHADD8) ||
        (opcode >= IA64_OP_LD1C_CLR && opcode <= IA64_OP_LD8C_NC) ||
        (opcode >= IA64_OP_PAVG1 && opcode <= IA64_OP_CZX2_R)) {
        return true;
    }

    switch (opcode) {
    case IA64_OP_LD16:
    case IA64_OP_MOVL:
    case IA64_OP_MOV_BRGR:
    case IA64_OP_MOV_PRGR:
    case IA64_OP_MOV_ARGR:
    case IA64_OP_MOV_CRGR:
    case IA64_OP_ALLOC:
    case IA64_OP_GETF_D:
    case IA64_OP_GETF_S:
    case IA64_OP_GETF_EXP:
    case IA64_OP_GETF_SIG:
    case IA64_OP_TPA:
    case IA64_OP_TAK:
    case IA64_OP_THASH:
    case IA64_OP_TTAG:
    case IA64_OP_SHLADDP4:
    case IA64_OP_MPY4:
    case IA64_OP_MPYSHL4:
    case IA64_OP_MPYSH:
    case IA64_OP_MPYUH:
    case IA64_OP_CLZ:
    case IA64_OP_POPCNT:
    case IA64_OP_MUX:
    case IA64_OP_ADDP4:
    case IA64_OP_ADDP4_IMM:
    case IA64_OP_MOV_PSRGR:
    case IA64_OP_MOV_RRGR:
    case IA64_OP_MOV_PKRGR:
    case IA64_OP_MOV_PKRGR_INDEXED:
    case IA64_OP_MOV_UMGR:
    case IA64_OP_MOV_IBRGR:
    case IA64_OP_MOV_IBRGR_INDEXED:
    case IA64_OP_MOV_DBRGR:
    case IA64_OP_MOV_DBRGR_INDEXED:
    case IA64_OP_MOV_PMCGR:
    case IA64_OP_MOV_PMCGR_INDEXED:
    case IA64_OP_MOV_PMDGR:
    case IA64_OP_MOV_PMDGR_INDEXED:
    case IA64_OP_MOV_CPUID:
    case IA64_OP_MOV_CPUID_INDEXED:
    case IA64_OP_MOV_DAHRGR_INDEXED:
    case IA64_OP_MOV_MSRGR:
    case IA64_OP_MOV_IP:
    case IA64_OP_MOV_CURRENT_IP:
        return true;
    case IA64_OP_PROBE_R:
    case IA64_OP_PROBE_W:
    case IA64_OP_PROBE_RW:
        return !insn->probe_fault;
    default:
        return false;
    }
}

/*
 * The TB this thread translates: NaT reads fold to 0 where its facts know a
 * bit clear, and a TCG write that may set a NaT bit forgets the fact.  The
 * policy in ia64_update_nat_known() then states what the instruction proved.
 */
static __thread DisasContext *ia64_nat_ctx;

static bool ia64_nat_is_known_clear(const DisasContext *ctx, uint8_t reg)
{
    return reg == 0 ||
           (ctx->memory.nat_known_clear[reg / 64] & (1ULL << (reg % 64)));
}

static void ia64_nat_set_known_clear(DisasContext *ctx, uint8_t reg,
                                     bool known_clear)
{
    uint64_t bit;

    if (reg == 0) {
        return;
    }
    bit = 1ULL << (reg % 64);
    if (known_clear) {
        ctx->memory.nat_known_clear[reg / 64] |= bit;
    } else {
        ctx->memory.nat_known_clear[reg / 64] &= ~bit;
    }
}

typedef enum IA64NatResultPolicy {
    IA64_NAT_RESULT_UNKNOWN,
    IA64_NAT_RESULT_CLEAR,
    IA64_NAT_RESULT_SOURCE_R1,
    IA64_NAT_RESULT_SOURCE_R2,
    IA64_NAT_RESULT_SOURCE_R3,
    IA64_NAT_RESULT_SOURCES_R2_R3,
    IA64_NAT_RESULT_SOURCES_R1_R2_R3,
    IA64_NAT_RESULT_PRESERVE_ON_FULL_ALAT,
} IA64NatResultPolicy;

static IA64NatResultPolicy ia64_nat_result_policy(
    const Ia64Instruction *insn)
{
    switch (insn->opcode) {
    case IA64_OP_LD1:
    case IA64_OP_LD2:
    case IA64_OP_LD4:
    case IA64_OP_LD8:
    case IA64_OP_LD1A:
    case IA64_OP_LD2A:
    case IA64_OP_LD4A:
    case IA64_OP_LD8A:
    case IA64_OP_LD16:
    case IA64_OP_MOVL:
    case IA64_OP_MOV_BRGR:
    case IA64_OP_MOV_PRGR:
    case IA64_OP_MOV_ARGR:
    case IA64_OP_MOV_CRGR:
    case IA64_OP_MOV_PSRGR:
    case IA64_OP_MOV_RRGR:
    case IA64_OP_MOV_PKRGR:
    case IA64_OP_MOV_PKRGR_INDEXED:
    case IA64_OP_MOV_UMGR:
    case IA64_OP_MOV_IBRGR:
    case IA64_OP_MOV_IBRGR_INDEXED:
    case IA64_OP_MOV_DBRGR:
    case IA64_OP_MOV_DBRGR_INDEXED:
    case IA64_OP_MOV_PMCGR:
    case IA64_OP_MOV_PMCGR_INDEXED:
    case IA64_OP_MOV_PMDGR:
    case IA64_OP_MOV_PMDGR_INDEXED:
    case IA64_OP_MOV_CPUID:
    case IA64_OP_MOV_CPUID_INDEXED:
    case IA64_OP_MOV_DAHRGR_INDEXED:
    case IA64_OP_MOV_MSRGR:
    case IA64_OP_MOV_IP:
    case IA64_OP_MOV_CURRENT_IP:
    case IA64_OP_PROBE_R:
    case IA64_OP_PROBE_W:
    case IA64_OP_PROBE_RW:
        return IA64_NAT_RESULT_CLEAR;
    case IA64_OP_LD1C_CLR:
    case IA64_OP_LD2C_CLR:
    case IA64_OP_LD4C_CLR:
    case IA64_OP_LD8C_CLR:
    case IA64_OP_LD1C_NC:
    case IA64_OP_LD2C_NC:
    case IA64_OP_LD4C_NC:
    case IA64_OP_LD8C_NC:
        /* A full-ALAT hit preserves the old destination and its NaT. */
        return IA64_NAT_RESULT_PRESERVE_ON_FULL_ALAT;
    case IA64_OP_ADDS:
    case IA64_OP_ADDL:
    case IA64_OP_SHL_IMM:
    case IA64_OP_SHRU_IMM:
    case IA64_OP_SHR_IMM:
    case IA64_OP_EXTRU:
    case IA64_OP_SXT1:
    case IA64_OP_SXT2:
    case IA64_OP_SXT4:
    case IA64_OP_ZXT1:
    case IA64_OP_ZXT2:
    case IA64_OP_ZXT4:
    case IA64_OP_DEP_IMM:
    case IA64_OP_EXTR:
    case IA64_OP_SUB_IMM:
    case IA64_OP_AND_IMM:
    case IA64_OP_ANDCM_IMM:
    case IA64_OP_OR_IMM:
    case IA64_OP_XOR_IMM:
    case IA64_OP_POPCNT:
    case IA64_OP_CLZ:
        return IA64_NAT_RESULT_SOURCE_R3;
    case IA64_OP_DEPZ:
        return IA64_NAT_RESULT_SOURCE_R2;
    case IA64_OP_DEPZ_IMM:
        return IA64_NAT_RESULT_CLEAR;
    case IA64_OP_SHLADD:
    case IA64_OP_ADD:
    case IA64_OP_ADD_ONE:
    case IA64_OP_SUB:
    case IA64_OP_SUB_ONE:
    case IA64_OP_AND:
    case IA64_OP_ANDCM:
    case IA64_OP_OR:
    case IA64_OP_XOR:
    case IA64_OP_SHL:
    case IA64_OP_SHRU:
    case IA64_OP_SHR:
    case IA64_OP_SHRP_IMM:
    case IA64_OP_DEP:
    case IA64_OP_SHLADDP4:
    case IA64_OP_MPY4:
    case IA64_OP_MPYSHL4:
    case IA64_OP_MPYSH:
    case IA64_OP_MPYUH:
    case IA64_OP_ADDP4:
        return IA64_NAT_RESULT_SOURCES_R2_R3;
    case IA64_OP_ADDP4_IMM:
        return IA64_NAT_RESULT_SOURCE_R3;
    case IA64_OP_MUX:
        return IA64_NAT_RESULT_SOURCES_R1_R2_R3;
    default:
        return IA64_NAT_RESULT_UNKNOWN;
    }
}

bool ia64_nat_result_is_known_clear(const DisasContext *ctx,
                                    const Ia64Instruction *insn)
{
    switch (ia64_nat_result_policy(insn)) {
    case IA64_NAT_RESULT_CLEAR:
        return true;
    case IA64_NAT_RESULT_SOURCE_R1:
        return ia64_nat_is_known_clear(ctx, insn->operands.common.destination);
    case IA64_NAT_RESULT_SOURCE_R2:
        return ia64_nat_is_known_clear(ctx, insn->operands.common.source1);
    case IA64_NAT_RESULT_SOURCE_R3:
        return ia64_nat_is_known_clear(ctx, insn->operands.common.source2);
    case IA64_NAT_RESULT_SOURCES_R2_R3:
        return ia64_nat_is_known_clear(ctx, insn->operands.common.source1) &&
               ia64_nat_is_known_clear(ctx, insn->operands.common.source2);
    case IA64_NAT_RESULT_SOURCES_R1_R2_R3:
        return ia64_nat_is_known_clear(
                   ctx, insn->operands.common.destination) &&
               ia64_nat_is_known_clear(ctx, insn->operands.common.source1) &&
               ia64_nat_is_known_clear(ctx, insn->operands.common.source2);
    case IA64_NAT_RESULT_PRESERVE_ON_FULL_ALAT:
        return !ctx->memory.full_alat;
    case IA64_NAT_RESULT_UNKNOWN:
    default:
        return false;
    }
}

static bool ia64_insn_may_modify_cfm_sof(const Ia64Instruction *insn)
{
    switch (insn->opcode) {
    case IA64_OP_ALLOC:
    case IA64_OP_COVER:
    case IA64_OP_LOADRS:
    case IA64_OP_RFI:
    case IA64_OP_BREAK:
    case IA64_OP_BR_CALL:
    case IA64_OP_BRL_CALL:
    case IA64_OP_BR_CALL_INDIRECT:
    case IA64_OP_BR_RET:
    case IA64_OP_BR_IA:
        return true;
    default:
        return false;
    }
}

void ia64_update_frame_tracking(DisasContext *ctx,
                                const Ia64Instruction *insn)
{
    if (ia64_insn_may_modify_cfm_sof(insn)) {
        ctx->cfm_sof_valid = false;
        ctx->cfm_sof_checked = 0;
    }

    /*
     * alloc is architecturally unpredicated and installs an immediate
     * SOF, so retain that value instead of reloading it for the next
     * frame check.
     */
    if (insn->opcode == IA64_OP_ALLOC) {
        uint8_t new_sof = insn->operands.common.immediate & 0x7f;

        ctx->cfm_sof = tcg_constant_i32(new_sof);
        ctx->cfm_sof_checked = new_sof;
        ctx->cfm_sof_valid = true;
    }
}

/*
 * The RSE helpers of these instructions rename or reload the stacked
 * registers, and bsw swaps r16-r31 with the other bank, before the TB goes
 * on or the branch leaves it.
 */
static void ia64_drop_nat_known_renamed(const Ia64Instruction *insn,
                                        uint64_t known[2])
{
    switch (insn->opcode) {
    case IA64_OP_ALLOC:
    case IA64_OP_COVER:
    case IA64_OP_LOADRS:
    case IA64_OP_CLRRRB:
    case IA64_OP_CLRRRB_PR:
    case IA64_OP_BR_CTOP:
    case IA64_OP_BR_CEXIT:
    case IA64_OP_BR_WTOP:
    case IA64_OP_BR_WEXIT:
    case IA64_OP_BR_CALL:
    case IA64_OP_BR_CALL_INDIRECT:
    case IA64_OP_BRL_CALL:
    case IA64_OP_BR_RET:
    case IA64_OP_RFI:
        known[0] &= IA64_STATIC_GR_NAT_MASK;
        known[1] = 0;
        break;
    case IA64_OP_BSW0:
    case IA64_OP_BSW1:
        known[0] &= ~(0xffffULL << 16);
        break;
    default:
        break;
    }
}

void ia64_update_nat_known(DisasContext *ctx,
                           const Ia64Instruction *insn)
{
    const uint64_t *before = ctx->memory.nat_known_before;
    uint8_t r1 = insn->operands.common.destination;
    uint8_t r2 = insn->operands.common.source1;
    uint8_t r3 = insn->operands.common.source2;
    bool old_r1 = r1 == 0 || (before[r1 / 64] >> (r1 % 64)) & 1;
    bool old_r2 = r2 == 0 || (before[r2 / 64] >> (r2 % 64)) & 1;
    bool old_r3 = r3 == 0 || (before[r3 / 64] >> (r3 % 64)) & 1;
    uint64_t after[2] = {
        ctx->memory.nat_known_clear[0], ctx->memory.nat_known_clear[1]
    };
    bool result;

    /* The policy judges the sources as they were before the instruction. */
    ctx->memory.nat_known_clear[0] = before[0];
    ctx->memory.nat_known_clear[1] = before[1];
    result = ia64_nat_result_is_known_clear(ctx, insn);
    ctx->memory.nat_known_clear[0] = after[0];
    ctx->memory.nat_known_clear[1] = after[1];

    if (ia64_insn_writes_gr_r1(insn)) {

        /* A predicated-off instruction preserves the old destination. */
        ia64_nat_set_known_clear(ctx, insn->operands.common.destination,
                                 result && (insn->qp == 0 || old_r1));
    }

    if (insn->reg_base_update) {
        /* The executed update assigns base.NaT | increment.NaT. */
        ia64_nat_set_known_clear(ctx, insn->operands.common.source2,
                                 old_r3 && old_r2);
    }
    /* An immediate base update preserves the base register's NaT bit. */

    ia64_drop_nat_known_renamed(insn, ctx->memory.nat_known_clear);
}

static void ia64_set_exit_nat_known(DisasContext *ctx,
                                    const Ia64Instruction *insn)
{
    ctx->memory.nat_known_before[0] = ctx->memory.nat_known_clear[0];
    ctx->memory.nat_known_before[1] = ctx->memory.nat_known_clear[1];
    ctx->memory.nat_known_at_exit[0] = ctx->memory.nat_known_clear[0];
    ctx->memory.nat_known_at_exit[1] = ctx->memory.nat_known_clear[1];
    ia64_drop_nat_known_renamed(insn, ctx->memory.nat_known_at_exit);
}

static bool ia64_insn_is_privileged(const Ia64Instruction *insn)
{
    switch (insn->opcode) {
    case IA64_OP_BSW0:
    case IA64_OP_BSW1:
    case IA64_OP_RFI:
    case IA64_OP_MOV_CRGR:
    case IA64_OP_MOV_GRCR:
    case IA64_OP_MOV_GRPSR:
    case IA64_OP_MOV_PSRGR:
    case IA64_OP_MOV_RRGR:
    case IA64_OP_MOV_GRRR:
    case IA64_OP_MOV_PKRGR:
    case IA64_OP_MOV_PKRGR_INDEXED:
    case IA64_OP_MOV_GRPKR:
    case IA64_OP_MOV_GRPKR_INDEXED:
    case IA64_OP_MOV_IBRGR:
    case IA64_OP_MOV_GRIBR:
    case IA64_OP_MOV_IBRGR_INDEXED:
    case IA64_OP_MOV_GRIBR_INDEXED:
    case IA64_OP_MOV_DBRGR:
    case IA64_OP_MOV_GRDBR:
    case IA64_OP_MOV_DBRGR_INDEXED:
    case IA64_OP_MOV_GRDBR_INDEXED:
    case IA64_OP_MOV_PMCGR:
    case IA64_OP_MOV_GRPMC:
    case IA64_OP_MOV_PMCGR_INDEXED:
    case IA64_OP_MOV_GRPMC_INDEXED:
    case IA64_OP_MOV_GRPMD:
    case IA64_OP_MOV_GRPMD_INDEXED:
    case IA64_OP_MOV_DAHRGR_INDEXED:
    case IA64_OP_MOV_MSRGR:
    case IA64_OP_MOV_GRMSR:
    case IA64_OP_ITC_D:
    case IA64_OP_ITC_I:
    case IA64_OP_ITR_D:
    case IA64_OP_ITR_I:
    case IA64_OP_SSM:
    case IA64_OP_RSM:
    case IA64_OP_PTC_G:
    case IA64_OP_PTC_GA:
    case IA64_OP_PTC_L:
    case IA64_OP_PTR_D:
    case IA64_OP_PTR_I:
    case IA64_OP_PTC_E:
    case IA64_OP_TAK:
    case IA64_OP_TPA:
        return true;
    default:
        return false;
    }
}

static bool ia64_reserved_ar_for_unit(uint8_t ar, IA64SlotUnit unit,
                                      bool write)
{
    if (unit == IA64_UNIT_I) {
        return ar <= 47 || (ar >= 67 && ar <= 111);
    }

    if ((ar >= 8 && ar <= 15) || ar == 20 ||
        (ar >= 22 && ar <= 23) || ar == 31 ||
        (ar >= 33 && ar <= 35) || (ar >= 37 && ar <= 39) ||
        (ar >= 41 && ar <= 43) || (ar >= 45 && ar <= 47) ||
        (ar >= 64 && ar <= 111)) {
        return true;
    }
    return write && ar == 17;
}

static bool ia64_insn_has_reserved_ar(const Ia64Instruction *insn)
{
    switch (insn->opcode) {
    case IA64_OP_MOV_ARGR:
        return ia64_reserved_ar_for_unit(
            insn->operands.common.source1, insn->unit, false);
    case IA64_OP_MOV_GRAR:
    case IA64_OP_MOV_IMMAR:
        return ia64_reserved_ar_for_unit(
            insn->operands.common.source1, insn->unit, true);
    default:
        return false;
    }
}

static bool ia64_reserved_cr(uint8_t cr)
{
    return (cr >= 3 && cr <= 7) || (cr >= 10 && cr <= 15) ||
           cr == 18 || (cr >= 26 && cr <= 63) ||
           (cr >= 75 && cr <= 79) || (cr >= 82);
}

bool ia64_insn_has_illegal_register(const Ia64Instruction *insn)
{
    if (ia64_insn_has_reserved_ar(insn)) {
        return true;
    }
    switch (insn->opcode) {
    case IA64_OP_MOV_CRGR:
        return ia64_reserved_cr(insn->operands.common.source1);
    case IA64_OP_MOV_GRCR:
        return ia64_reserved_cr(insn->operands.common.source1) ||
               ia64_cr_is_read_only(insn->operands.common.source1);
    default:
        return false;
    }
}

bool ia64_insn_has_reserved_mask_field(const Ia64Instruction *insn)
{
    uint64_t allowed;

    switch (insn->opcode) {
    case IA64_OP_RUM:
    case IA64_OP_SUM_UM:
        return insn->operands.common.immediate & ~0x3eULL;
    case IA64_OP_RSM:
    case IA64_OP_SSM:
        allowed = 0x3eULL |
                  (0x7ULL << 13) |
                  (0x7ffULL << 17) |
                  (0x1fffULL << 32);
        return insn->operands.common.immediate & ~allowed;
    default:
        return false;
    }
}
static bool ia64_fr_is_writable(uint8_t reg)
{
    return reg > 1;
}


static void ia64_gen_fr_nat_clear(uint8_t reg)
{
    if (reg <= 1) {
        return;
    }

    tcg_gen_andi_i64(cpu_fr_nat[reg / 64], cpu_fr_nat[reg / 64],
                     ~(1ULL << (reg % 64)));
}

static void ia64_gen_fr_sig_clear(uint8_t reg)
{
    if (reg <= 1) {
        return;
    }

    tcg_gen_andi_i64(cpu_fr_sig[reg / 64], cpu_fr_sig[reg / 64],
                     ~(1ULL << (reg % 64)));
    tcg_gen_andi_i64(cpu_fr_int_origin[reg / 64],
                     cpu_fr_int_origin[reg / 64],
                     ~(1ULL << (reg % 64)));
}

static void ia64_gen_fr_sig_set(uint8_t reg)
{
    if (reg <= 1) {
        return;
    }

    tcg_gen_ori_i64(cpu_fr_sig[reg / 64], cpu_fr_sig[reg / 64],
                    1ULL << (reg % 64));
    tcg_gen_ori_i64(cpu_fr_int_origin[reg / 64],
                    cpu_fr_int_origin[reg / 64],
                    1ULL << (reg % 64));
    tcg_gen_st_i64(cpu_fr[reg], tcg_env,
                   offsetof(CPUIA64State, fp.fr_int_value[reg]));
}

TCGv_i64 ia64_gen_fr_sig_read(uint8_t reg)
{
    TCGv_i64 bit = tcg_temp_new_i64();

    if (reg <= 1) {
        tcg_gen_movi_i64(bit, 1);
        return bit;
    }
    tcg_gen_shri_i64(bit, cpu_fr_sig[reg / 64], reg % 64);
    tcg_gen_andi_i64(bit, bit, 1);
    return bit;
}

TCGv_i64 ia64_fr_significand_src(uint8_t reg)
{
    if (reg == 0) {
        return tcg_constant_i64(0);
    }
    if (reg == 1) {
        return tcg_constant_i64(1ULL << 63);
    }
    return cpu_fr[reg];
}

static void ia64_gen_fr_ext_clear(uint8_t reg)
{
    if (reg <= 1) {
        return;
    }

    tcg_gen_andi_i64(cpu_fr_ext_valid[reg / 64],
                     cpu_fr_ext_valid[reg / 64],
                     ~(1ULL << (reg % 64)));
}

static void ia64_gen_fr_mark_written(uint8_t reg)
{
    if (reg > 1) {
        tcg_gen_ori_i64(cpu_psr, cpu_psr,
                        reg >= 32 ? IA64_PSR_MFH : IA64_PSR_MFL);
        if (reg >= 32) {
            tcg_gen_st8_i32(tcg_constant_i32(1), tcg_env,
                            offsetof(CPUIA64State, fp.rotating_fr_live));
        }
    }
}

static void ia64_gen_fr_nat_set(uint8_t reg)
{
    if (reg <= 1) {
        return;
    }

    tcg_gen_ori_i64(cpu_fr_nat[reg / 64], cpu_fr_nat[reg / 64],
                    1ULL << (reg % 64));
    ia64_gen_fr_sig_clear(reg);
    ia64_gen_fr_ext_clear(reg);
    ia64_gen_fr_mark_written(reg);
}

TCGv_i64 ia64_gen_fr_nat_read(uint8_t reg)
{
    TCGv_i64 bit = tcg_temp_new_i64();

    if (reg <= 1) {
        tcg_gen_movi_i64(bit, 0);
        return bit;
    }

    tcg_gen_shri_i64(bit, cpu_fr_nat[reg / 64], reg % 64);
    tcg_gen_andi_i64(bit, bit, 1);
    return bit;
}

static void ia64_gen_fr_nat_assign(uint8_t reg, TCGv_i64 bit)
{
    TCGv_i64 shifted;

    if (reg <= 1) {
        return;
    }

    shifted = tcg_temp_new_i64();
    tcg_gen_andi_i64(shifted, bit, 1);
    tcg_gen_shli_i64(shifted, shifted, reg % 64);
    tcg_gen_andi_i64(cpu_fr_nat[reg / 64], cpu_fr_nat[reg / 64],
                     ~(1ULL << (reg % 64)));
    tcg_gen_or_i64(cpu_fr_nat[reg / 64], cpu_fr_nat[reg / 64], shifted);
    tcg_gen_not_i64(shifted, shifted);
    tcg_gen_and_i64(cpu_fr_sig[reg / 64], cpu_fr_sig[reg / 64], shifted);
    tcg_gen_and_i64(cpu_fr_int_origin[reg / 64],
                    cpu_fr_int_origin[reg / 64], shifted);
    tcg_gen_and_i64(cpu_fr_ext_valid[reg / 64],
                    cpu_fr_ext_valid[reg / 64], shifted);
}


void ia64_gen_fr_mov(uint8_t reg, TCGv_i64 value)
{
    if (ia64_fr_is_writable(reg)) {
        tcg_gen_mov_i64(cpu_fr[reg], value);
        ia64_gen_fr_nat_clear(reg);
        ia64_gen_fr_sig_clear(reg);
        ia64_gen_fr_ext_clear(reg);
        ia64_gen_fr_mark_written(reg);
    }
}

void ia64_gen_fr_mov_sig(uint8_t reg, TCGv_i64 value)
{
    if (ia64_fr_is_writable(reg)) {
        tcg_gen_mov_i64(cpu_fr[reg], value);
        ia64_gen_fr_nat_clear(reg);
        ia64_gen_fr_sig_set(reg);
        ia64_gen_fr_ext_clear(reg);
        ia64_gen_fr_mark_written(reg);
    }
}

static void ia64_gen_fr_ld(uint8_t reg, TCGv_i64 addr, int mmu_idx, MemOp memop)
{
    TCGv_i64 dst = ia64_fr_is_writable(reg) ? cpu_fr[reg] : tcg_temp_new_i64();

    tcg_gen_qemu_ld_i64(dst, addr, mmu_idx, memop);
    if (ia64_fr_is_writable(reg)) {
        ia64_gen_fr_nat_clear(reg);
        ia64_gen_fr_sig_clear(reg);
        ia64_gen_fr_ext_clear(reg);
        ia64_gen_fr_mark_written(reg);
    }
}

static void ia64_gen_fr_ld_s(uint8_t reg, TCGv_i64 addr, int mmu_idx,
                             MemOp memop)
{
    TCGv_i64 value = tcg_temp_new_i64();

    tcg_gen_qemu_ld_i64(value, addr, mmu_idx, memop);
    gen_helper_setf_s(tcg_env, tcg_constant_i32(reg), value);
}

static void ia64_gen_fr_ld_sig(uint8_t reg, TCGv_i64 addr, int mmu_idx,
                               MemOp memop)
{
    TCGv_i64 dst = ia64_fr_is_writable(reg) ? cpu_fr[reg] : tcg_temp_new_i64();

    tcg_gen_qemu_ld_i64(dst, addr, mmu_idx, memop);
    if (ia64_fr_is_writable(reg)) {
        ia64_gen_fr_nat_clear(reg);
        ia64_gen_fr_sig_set(reg);
        ia64_gen_fr_ext_clear(reg);
        ia64_gen_fr_mark_written(reg);
    }
}

void ia64_gen_fr_load(uint8_t reg, TCGv_i64 addr, int mmu_idx, MemOp memop,
                      IA64FPRegisterLoadFormat format)
{
    switch (format) {
    case IA64_FP_REGISTER_LOAD_DOUBLE:
        ia64_gen_fr_ld(reg, addr, mmu_idx, memop);
        break;
    case IA64_FP_REGISTER_LOAD_SINGLE:
        ia64_gen_fr_ld_s(reg, addr, mmu_idx, memop);
        break;
    case IA64_FP_REGISTER_LOAD_SIGNIFICAND:
        ia64_gen_fr_ld_sig(reg, addr, mmu_idx, memop);
        break;
    default:
        g_assert_not_reached();
    }
}

void ia64_gen_fr_set_nat(uint8_t reg)
{
    if (ia64_fr_is_writable(reg)) {
        tcg_gen_movi_i64(cpu_fr[reg], 0);
    }
    ia64_gen_fr_nat_set(reg);
}

/* Resolution of PSR.ac for a misaligned, non-always-fault access. */
typedef enum IA64AlignAcMode {
    IA64_ALIGN_AC_RUNTIME,  /* PSR.ac unknown: test it at run time */
    IA64_ALIGN_AC_SET,      /* PSR.ac statically 1: any misalignment faults */
    IA64_ALIGN_AC_CLEAR,    /* PSR.ac statically 0: the model's rule applies */
} IA64AlignAcMode;

static IA64AlignAcMode ia64_align_ac_mode(const Ia64Instruction *insn)
{
    const DisasContext *ctx = insn->ctx;

    /*
     * PSR.ac is in the TB flags.  While no ssm/rsm/sum/rum/mov-psr earlier in
     * this bundle may have changed it, the flag is exact and the per-access
     * PSR.ac read and branch can be resolved at translation time.
     */
    if (!ctx || ctx->restart.psr_ac_modified) {
        return IA64_ALIGN_AC_RUNTIME;
    }
    return ctx->memory.psr_ac ? IA64_ALIGN_AC_SET : IA64_ALIGN_AC_CLEAR;
}

/*
 * 251110-003 sec 5.5: with PSR.ac = 0 an Itanium 2 handles an integer
 * reference inside an 8-byte window and an FP reference inside a 16-byte
 * window (ldfe and stfe cover 10 bytes of it); FP pairs and spill/fill must
 * be naturally aligned, and a UC or WC reference faults when it crosses an
 * 8-byte boundary.
 */
IA64UnalignedWindow ia64_unaligned_window(const Ia64Instruction *insn,
                                          uint32_t size)
{
    const DisasContext *ctx = insn->ctx;
    const IA64CPUClass *icc;
    IA64UnalignedWindow w = { .window = 0, .span = size };

    if (!ctx) {
        return w;
    }
    icc = ia64_env_cpu_class(ctx->env);

    switch (insn->opcode) {
    case IA64_OP_LDFPS:
    case IA64_OP_LDFPD:
    case IA64_OP_LDFP8:
    case IA64_OP_LDF_FILL:
    case IA64_OP_STF_SPILL:
        w.window = size;
        break;
    case IA64_OP_LDFE:
    case IA64_OP_STFE:
        w.window = 16;
        w.span = 10;
        w.uc_crosses_8 = true;
        break;
    case IA64_OP_LDFS:
    case IA64_OP_LDFD:
    case IA64_OP_LDF8:
    case IA64_OP_STFS:
    case IA64_OP_STFD:
    case IA64_OP_STF8:
        w.window = 16;
        w.uc_crosses_8 = true;
        break;
    default:
        w.window = icc->unaligned_windows ? 8 : icc->unaligned_int_block;
        return w;
    }
    if (!icc->unaligned_windows) {
        /* FP references: only the 4 KiB rule. */
        w = (IA64UnalignedWindow){ .window = 0, .span = size };
    }
    return w;
}

static void ia64_gen_branch_if_alignment_fault(const Ia64Instruction *insn,
                                               TCGv_i64 addr, uint32_t size,
                                               bool always_fault,
                                               bool is_write,
                                               TCGLabel *fault)
{
    TCGv_i64 tmp;
    TCGLabel *ok;

    if (size <= 1) {
        return;
    }

    ok = gen_new_label();
    tmp = tcg_temp_new_i64();
    tcg_gen_andi_i64(tmp, addr, size - 1);
    tcg_gen_brcondi_i64(TCG_COND_EQ, tmp, 0, ok);

    if (always_fault) {
        /*
         * Semaphore and ld16/st16 misalignment faults regardless of PSR.ac and
         * of the memory attribute (SDM Vol.2 4.5), so it is never exempt.
         */
        tcg_gen_br(fault);
    } else {
        const DisasContext *ctx = insn->ctx;
        IA64AlignAcMode ac_mode = ia64_align_ac_mode(insn);
        IA64UnalignedWindow w = ia64_unaligned_window(insn, size);
        bool uc_exempt =
            ctx && ia64_env_cpu_class(ctx->env)->unaligned_uc_exempt;
        /*
         * The exemption is consulted only once a fault condition holds, so
         * the aligned and PSR.ac-clear-in-page fast paths stay free of the
         * probing helper.
         */
        TCGLabel *maybe_fault = uc_exempt ? gen_new_label() : fault;

        if (ac_mode == IA64_ALIGN_AC_SET) {
            tcg_gen_br(maybe_fault);
        } else {
            if (ac_mode == IA64_ALIGN_AC_RUNTIME) {
                tcg_gen_andi_i64(tmp, cpu_psr, IA64_PSR_AC);
                tcg_gen_brcondi_i64(TCG_COND_NE, tmp, 0, maybe_fault);
            }
            if (w.window != 0) {
                tcg_gen_andi_i64(tmp, addr, w.window - 1);
                tcg_gen_addi_i64(tmp, tmp, w.span);
                tcg_gen_brcondi_i64(TCG_COND_GTU, tmp, w.window, maybe_fault);
                if (w.uc_crosses_8) {
                    tcg_gen_andi_i64(tmp, addr, 7);
                    tcg_gen_addi_i64(tmp, tmp, w.span);
                    tcg_gen_brcondi_i64(TCG_COND_LEU, tmp, 8, ok);
                    gen_helper_ia64_unaligned_uncacheable(
                        tmp, tcg_env, addr, tcg_constant_i32(w.span),
                        tcg_constant_i32(is_write));
                    tcg_gen_brcondi_i64(TCG_COND_NE, tmp, 0, maybe_fault);
                }
            } else {
                /* Every model faults a datum spanning 4 KiB (SDM Vol.2 4.5). */
                tcg_gen_andi_i64(tmp, addr, 0xfff);
                tcg_gen_addi_i64(tmp, tmp, size - 1);
                tcg_gen_brcondi_i64(TCG_COND_GTU, tmp, 0xfff, maybe_fault);
            }
            tcg_gen_br(ok);
        }

        if (uc_exempt) {
            gen_set_label(maybe_fault);
            gen_helper_ia64_alignment_exempt(tmp, tcg_env, addr,
                                             tcg_constant_i32(size),
                                             tcg_constant_i32(is_write));
            tcg_gen_brcondi_i64(TCG_COND_EQ, tmp, 0, fault);
        }
    }

    gen_set_label(ok);
}

void ia64_gen_check_alignment_access(const Ia64Instruction *insn,
                                     TCGv_i64 addr, uint32_t size,
                                     bool always_fault,
                                     uint64_t isr_access)
{
    TCGLabel *fault;
    TCGLabel *ok;

    if (size <= 1) {
        return;
    }

    fault = gen_new_label();
    ok = gen_new_label();
    ia64_gen_branch_if_alignment_fault(insn, addr, size, always_fault,
                                       (isr_access & IA64_ISR_W) != 0, fault);
    tcg_gen_br(ok);

    gen_set_label(fault);
    tcg_gen_movi_i64(cpu_ip, insn->address);
    gen_helper_raise_unaligned(tcg_env, addr, tcg_constant_i64(isr_access),
                               tcg_constant_i64(insn->address | insn->slot));
    gen_set_label(ok);
}

void ia64_gen_check_alignment(const Ia64Instruction *insn,
                              TCGv_i64 addr, uint32_t size,
                              bool always_fault, bool is_write)
{
    ia64_gen_check_alignment_access(insn, addr, size, always_fault,
                                    is_write ? IA64_ISR_W : IA64_ISR_R);
}

void ia64_gen_invalidate_alat_store(DisasContext *ctx, TCGv_i64 addr,
                                    uint32_t size)
{
    TCGLabel *done;
    TCGv_i32 active;

    if (!ctx->memory.full_alat) {
        return;
    }

    done = gen_new_label();
    active = tcg_temp_new_i32();

    tcg_gen_ld_i32(active, tcg_env,
                   offsetof(CPUIA64State, alat_state.alat_active_count));
    tcg_gen_brcondi_i32(TCG_COND_EQ, active, 0, done);
    gen_helper_invalidate_alat_store(tcg_env, addr, tcg_constant_i32(size));
    gen_set_label(done);
}

static TCGLabel *ia64_gen_predicate_skip(const Ia64Instruction *insn,
                                         TCGv_i64 qp_value)
{
    TCGLabel *skip;

    /*
     * For while-loop branches, PR[qp] is the kernel loop condition rather
     * than a predicate that nullifies the instruction.  A false condition
     * must still drain the software pipeline while AR.EC is nonzero.
     */
    if (insn->qp == 0 ||
        insn->opcode == IA64_OP_BR_WEXIT ||
        insn->opcode == IA64_OP_BR_WTOP) {
        return NULL;
    }

    skip = gen_new_label();
    tcg_gen_brcondi_i64(TCG_COND_EQ, qp_value, 0, skip);
    return skip;
}

static void ia64_gen_predicate_end(TCGLabel *skip)
{
    if (skip != NULL) {
        gen_set_label(skip);
    }
}

static void ia64_gen_clear_unc_compare_targets(const Ia64Instruction *insn)
{
    if (insn->compare_unc) {
        if (insn->operands.common.auxiliary1 != 0) {
            tcg_gen_movi_i64(cpu_pr[insn->operands.common.auxiliary1], 0);
        }
        if (insn->operands.common.auxiliary2 != 0) {
            tcg_gen_movi_i64(cpu_pr[insn->operands.common.auxiliary2], 0);
        }
    }
}

static bool ia64_compare_has_equal_targets(const Ia64Instruction *insn)
{
    if (insn->operands.common.auxiliary1 != insn->operands.common.auxiliary2) {
        return false;
    }

    switch (insn->opcode) {
    case IA64_OP_CMP_EQ:
    case IA64_OP_CMP_LT:
    case IA64_OP_CMP_LE:
    case IA64_OP_CMP_GT:
    case IA64_OP_CMP_GE:
    case IA64_OP_CMP_LTU:
    case IA64_OP_CMP_LEU:
    case IA64_OP_CMP_GTU:
    case IA64_OP_CMP_GEU:
    case IA64_OP_CMP_NE:
    case IA64_OP_CMP_EQ_AND:
    case IA64_OP_CMP_NE_AND:
    case IA64_OP_CMP_GT_AND:
    case IA64_OP_CMP_LE_AND:
    case IA64_OP_CMP_GE_AND:
    case IA64_OP_CMP_LT_AND:
    case IA64_OP_CMP_EQ_OR:
    case IA64_OP_CMP_NE_OR:
    case IA64_OP_CMP_GT_OR:
    case IA64_OP_CMP_LE_OR:
    case IA64_OP_CMP_GE_OR:
    case IA64_OP_CMP_LT_OR:
    case IA64_OP_CMP_EQ_OR_ANDCM:
    case IA64_OP_CMP_NE_OR_ANDCM:
    case IA64_OP_CMP_GT_OR_ANDCM:
    case IA64_OP_CMP_LE_OR_ANDCM:
    case IA64_OP_CMP_GE_OR_ANDCM:
    case IA64_OP_CMP_LT_OR_ANDCM:
    case IA64_OP_CMP_EQ_IMM:
    case IA64_OP_CMP_LT_IMM:
    case IA64_OP_CMP_EQ_AND_IMM:
    case IA64_OP_CMP_NE_AND_IMM:
    case IA64_OP_CMP_EQ_OR_IMM:
    case IA64_OP_CMP_NE_OR_IMM:
    case IA64_OP_CMP_EQ_OR_ANDCM_IMM:
    case IA64_OP_CMP_NE_OR_ANDCM_IMM:
    case IA64_OP_CMP_LTU_IMM:
    case IA64_OP_CMP4_EQ:
    case IA64_OP_CMP4_LT:
    case IA64_OP_CMP4_LE:
    case IA64_OP_CMP4_GT:
    case IA64_OP_CMP4_GE:
    case IA64_OP_CMP4_LTU:
    case IA64_OP_CMP4_LEU:
    case IA64_OP_CMP4_GTU:
    case IA64_OP_CMP4_GEU:
    case IA64_OP_CMP4_EQ_AND:
    case IA64_OP_CMP4_NE_AND:
    case IA64_OP_CMP4_EQ_OR:
    case IA64_OP_CMP4_NE_OR:
    case IA64_OP_CMP4_EQ_OR_ANDCM:
    case IA64_OP_CMP4_NE_OR_ANDCM:
    case IA64_OP_CMP4_GT_AND:
    case IA64_OP_CMP4_LE_AND:
    case IA64_OP_CMP4_GE_AND:
    case IA64_OP_CMP4_LT_AND:
    case IA64_OP_CMP4_GT_OR:
    case IA64_OP_CMP4_LE_OR:
    case IA64_OP_CMP4_GE_OR:
    case IA64_OP_CMP4_LT_OR:
    case IA64_OP_CMP4_GT_OR_ANDCM:
    case IA64_OP_CMP4_LE_OR_ANDCM:
    case IA64_OP_CMP4_GE_OR_ANDCM:
    case IA64_OP_CMP4_LT_OR_ANDCM:
    case IA64_OP_CMP4_EQ_OR_ANDCM_IMM:
    case IA64_OP_CMP4_NE_OR_ANDCM_IMM:
    case IA64_OP_CMP4_EQ_IMM:
    case IA64_OP_CMP4_LT_IMM:
    case IA64_OP_CMP4_LTU_IMM:
    case IA64_OP_CMP4_EQ_AND_IMM:
    case IA64_OP_CMP4_NE_AND_IMM:
    case IA64_OP_CMP4_EQ_OR_IMM:
    case IA64_OP_CMP4_NE_OR_IMM:
    case IA64_OP_TBIT_Z:
    case IA64_OP_TBIT_NZ:
    case IA64_OP_TBIT_Z_OR_ANDCM:
    case IA64_OP_TBIT_NZ_OR_ANDCM:
    case IA64_OP_TNAT_Z:
    case IA64_OP_TNAT_NZ:
    case IA64_OP_TNAT_NZ_AND:
    case IA64_OP_TF_Z:
    case IA64_OP_TF_NZ:
    case IA64_OP_FCMP:
    case IA64_OP_FCLASS:
        return true;
    default:
        return false;
    }
}

void ia64_gen_predicate_test_write(const Ia64Instruction *insn,
                                   TCGv_i64 cond, TCGv_i64 not_cond)
{
    switch (insn->pred_update) {
    case IA64_PRED_UPDATE_AND: {
        TCGLabel *done = gen_new_label();

        tcg_gen_brcondi_i64(TCG_COND_NE, cond, 0, done);
        if (insn->operands.common.auxiliary1 != 0) {
            tcg_gen_movi_i64(cpu_pr[insn->operands.common.auxiliary1], 0);
        }
        if (insn->operands.common.auxiliary2 != 0) {
            tcg_gen_movi_i64(cpu_pr[insn->operands.common.auxiliary2], 0);
        }
        gen_set_label(done);
        break;
    }
    case IA64_PRED_UPDATE_OR: {
        TCGLabel *done = gen_new_label();

        tcg_gen_brcondi_i64(TCG_COND_EQ, cond, 0, done);
        if (insn->operands.common.auxiliary1 != 0) {
            tcg_gen_movi_i64(cpu_pr[insn->operands.common.auxiliary1], 1);
        }
        if (insn->operands.common.auxiliary2 != 0) {
            tcg_gen_movi_i64(cpu_pr[insn->operands.common.auxiliary2], 1);
        }
        gen_set_label(done);
        break;
    }
    case IA64_PRED_UPDATE_OR_ANDCM: {
        TCGLabel *done = gen_new_label();

        tcg_gen_brcondi_i64(TCG_COND_EQ, cond, 0, done);
        if (insn->operands.common.auxiliary1 != 0) {
            tcg_gen_movi_i64(cpu_pr[insn->operands.common.auxiliary1], 1);
        }
        if (insn->operands.common.auxiliary2 != 0) {
            tcg_gen_movi_i64(cpu_pr[insn->operands.common.auxiliary2], 0);
        }
        gen_set_label(done);
        break;
    }
    case IA64_PRED_UPDATE_NORMAL:
        if (insn->operands.common.auxiliary1 != 0) {
            tcg_gen_mov_i64(cpu_pr[insn->operands.common.auxiliary1], cond);
        }
        if (insn->operands.common.auxiliary2 != 0) {
            tcg_gen_mov_i64(cpu_pr[insn->operands.common.auxiliary2], not_cond);
        }
        break;
    }
}


void ia64_gen_clear_ri(void)
{
    tcg_gen_andi_i64(cpu_psr, cpu_psr, ~IA64_PSR_RI_MASK);
}

static void ia64_gen_set_ri(uint8_t slot)
{
    ia64_gen_clear_ri();
    if (slot != 0) {
        tcg_gen_ori_i64(cpu_psr, cpu_psr,
                        (uint64_t)slot << IA64_PSR_RI_SHIFT);
    }
}

void ia64_gen_set_ri_tracked(DisasContext *ctx, uint8_t slot)
{
    if (ctx->restart.current_ri_known && ctx->restart.current_ri == slot) {
        return;
    }

    if (!ctx->restart.current_ri_known || ctx->restart.current_ri != 0) {
        ia64_gen_clear_ri();
    }
    if (slot != 0) {
        tcg_gen_ori_i64(cpu_psr, cpu_psr,
                        (uint64_t)slot << IA64_PSR_RI_SHIFT);
    }
    ctx->restart.current_ri = slot;
    ctx->restart.current_ri_known = true;
}

static void ia64_gen_force_ri_tracked(DisasContext *ctx, uint8_t slot)
{
    ia64_gen_set_ri(slot);
    ctx->restart.current_ri = slot;
    ctx->restart.current_ri_known = true;
}

/*
 * Move the restart point to the next instruction boundary after an
 * instruction completes.  PSR.ri itself is stored only before code that can
 * observe it (ia64_gen_sync_ri): under TCG a TB is left only at its exits, at
 * a helper that raises or longjmps, or at a softmmu fault or I/O recompile,
 * and a pending interrupt is only taken at TB entry.
 *
 * An MLX L+X instruction occupies slots 1 and 2, so its successor is slot 0
 * of the next bundle rather than slot 2 of the current bundle.  SDM Vol. 2,
 * 5.5.3 likewise defines the successor of an MLX slot-1 instruction as the
 * next bundle at RI 0.
 */
void ia64_gen_advance_restart_point(DisasContext *ctx, uint64_t bundle_ip,
                                    uint8_t slot, bool mlx_long)
{
    if (slot == 2 || (slot == 1 && mlx_long)) {
        ctx->restart.logical_ri = 0;
        tcg_gen_movi_i64(cpu_ip, bundle_ip + 16);
    } else {
        ctx->restart.logical_ri = slot + 1;
    }
}

static void ia64_gen_sync_ri(DisasContext *ctx)
{
    ia64_gen_set_ri_tracked(ctx, ctx->restart.logical_ri);
}

/*
 * An exit path branches off code that goes on, so it must leave the
 * translation-time view of PSR.ri as it is.
 */
static void ia64_gen_sync_ri_for_exit(const DisasContext *ctx)
{
    if (!ctx->restart.current_ri_known ||
        ctx->restart.current_ri != ctx->restart.logical_ri) {
        ia64_gen_set_ri(ctx->restart.logical_ri);
    }
}

static void ia64_gen_set_fault_slot(uint8_t slot)
{
    tcg_gen_st_i32(tcg_constant_i32(slot), tcg_env,
                   offsetof(CPUIA64State, exception_state.fault_slot));
}

static void ia64_gen_save_fault_slot_from_ri(void)
{
    TCGv_i64 slot64 = tcg_temp_new_i64();
    TCGv_i32 slot32 = tcg_temp_new_i32();

    tcg_gen_extract_i64(slot64, cpu_psr, IA64_PSR_RI_SHIFT, 2);
    tcg_gen_extrl_i64_i32(slot32, slot64);
    tcg_gen_st_i32(
        slot32, tcg_env,
        offsetof(CPUIA64State, exception_state.fault_slot));
}

void ia64_gen_save_fault_slot_for_exit(DisasContext *ctx)
{
    ia64_gen_set_fault_slot(ctx->restart.logical_ri);
}

static void ia64_gen_note_successful_bundle(const DisasContext *ctx,
                                            uint64_t bundle_ip,
                                            bool record_iipa,
                                            bool track_psr_suppression)
{
    if (record_iipa) {
        /*
         * Handler code with PSR.ic cleared must not advance the next IIPA
         * value.  record_iipa is only set when PSR.ic is believed set
         * (track_iipa), so unless an in-bundle ssm/rsm/mov-psr may have
         * changed PSR.ic since TB entry, it is statically set and the store
         * is unconditional -- no runtime andi/brcond/label per bundle.
         */
        if (!ctx->restart.psr_ic_modified) {
            tcg_gen_st_i64(tcg_constant_i64(ia64_ip_bundle_addr(bundle_ip)),
                           tcg_env,
                           offsetof(CPUIA64State, last_successful_bundle));
        } else {
            TCGv_i64 collecting = tcg_temp_new_i64();
            TCGLabel *l_done = gen_new_label();

            tcg_gen_andi_i64(collecting, cpu_psr, IA64_PSR_IC);
            tcg_gen_brcondi_i64(TCG_COND_EQ, collecting, 0, l_done);
            tcg_gen_st_i64(tcg_constant_i64(ia64_ip_bundle_addr(bundle_ip)),
                           tcg_env,
                           offsetof(CPUIA64State, last_successful_bundle));
            gen_set_label(l_done);
        }
    }

    if (track_psr_suppression) {
        TCGv_i64 suppression = tcg_temp_new_i64();
        TCGLabel *l_no_suppression = gen_new_label();

        tcg_gen_ld_i64(
            suppression, tcg_env,
            offsetof(CPUIA64State,
                     exception_state.psr_suppression_before_insn));
        tcg_gen_brcondi_i64(TCG_COND_EQ, suppression, 0, l_no_suppression);
        gen_helper_clear_psr_fault_suppression(tcg_env);
        gen_set_label(l_no_suppression);
    }
}

static void ia64_gen_exit_to(DisasContext *ctx, uint64_t ip)
{
    ia64_gen_save_fault_slot_from_ri();
    ia64_gen_clear_ri();
    tcg_gen_movi_i64(cpu_ip, ip);
    tcg_gen_exit_tb(NULL, 0);
}

void ia64_gen_store_instruction_group_start(bool group_start)
{
    tcg_gen_st8_i32(tcg_constant_i32(group_start), tcg_env,
                    offsetof(CPUIA64State, instruction_group_start));
}

void ia64_gen_exit_to_completed(DisasContext *ctx, uint64_t ip,
                                uint64_t completed_ip,
                                bool record_iipa,
                                bool track_psr_suppression)
{
    ia64_gen_note_successful_bundle(ctx, completed_ip, record_iipa,
                                    track_psr_suppression);
    ia64_gen_store_instruction_group_start(
        ctx->restart.next_instruction_group_start);
    ia64_gen_exit_to(ctx, ip);
}

/* A taken branch in a TB that notes completion traps (DisasContext.psr_tb). */
static void ia64_gen_note_taken_branch(DisasContext *ctx,
                                       uint64_t completed_ip)
{
    if (ctx->psr_ss || ctx->psr_tb) {
        gen_helper_completion_trap_taken(
            tcg_env, tcg_constant_i64(ia64_ip_bundle_addr(completed_ip)),
            tcg_constant_i32(ctx->trap_slot));
    }
}

void ia64_gen_lookup_tcg_completed(DisasContext *ctx, TCGv_i64 ip,
                                   uint64_t completed_ip,
                                   bool record_iipa,
                                   bool track_psr_suppression)
{
    ia64_gen_note_taken_branch(ctx, completed_ip);
    ia64_gen_note_successful_bundle(ctx, completed_ip, record_iipa,
                                    track_psr_suppression);
    ia64_gen_store_instruction_group_start(true);
    ia64_gen_save_fault_slot_from_ri();
    ia64_gen_clear_ri();
    tcg_gen_mov_i64(cpu_ip, ip);
    tcg_gen_lookup_and_goto_ptr();
}

void ia64_gen_lookup_current_completed(DisasContext *ctx,
                                       uint64_t completed_ip,
                                       bool record_iipa,
                                       bool track_psr_suppression)
{
    ia64_gen_note_taken_branch(ctx, completed_ip);
    ia64_gen_note_successful_bundle(ctx, completed_ip, record_iipa,
                                    track_psr_suppression);
    ia64_gen_store_instruction_group_start(true);
    ia64_gen_save_fault_slot_from_ri();
    ia64_gen_clear_ri();
    tcg_gen_lookup_and_goto_ptr();
}

/* Store the IP, RI and fault slot that resume execution at (ip, slot). */
static void ia64_gen_set_resume_slot(uint64_t ip, uint8_t slot)
{
    if (slot >= 3) {
        ia64_gen_save_fault_slot_from_ri();
        ia64_gen_clear_ri();
        tcg_gen_movi_i64(cpu_ip, ip + 16);
        return;
    }

    ia64_gen_set_fault_slot(slot);
    ia64_gen_set_ri(slot);
    tcg_gen_movi_i64(cpu_ip, ip);
}

void ia64_gen_exit_to_slot_completed(DisasContext *ctx, uint64_t ip,
                                     uint8_t slot,
                                     uint64_t completed_ip,
                                     bool record_iipa,
                                     bool track_psr_suppression)
{
    ia64_gen_note_successful_bundle(ctx, completed_ip, record_iipa,
                                    track_psr_suppression);
    ia64_gen_store_instruction_group_start(
        ctx->restart.next_instruction_group_start);
    ia64_gen_set_resume_slot(ip, slot);
    tcg_gen_exit_tb(NULL, 0);
}

/*
 * As ia64_gen_exit_to_slot_completed, but leave to the main loop only when
 * @main_loop is nonzero; otherwise look the next TB up directly, which is
 * what the main loop would do.
 */
void ia64_gen_exit_or_lookup_slot_completed(DisasContext *ctx, uint64_t ip,
                                            uint8_t slot,
                                            uint64_t completed_ip,
                                            bool record_iipa,
                                            bool track_psr_suppression,
                                            TCGv_i32 main_loop)
{
    TCGLabel *lookup;

    if (ctx->psr_ss || ctx->psr_tb) {
        ia64_gen_exit_to_slot_completed(ctx, ip, slot, completed_ip,
                                        record_iipa, track_psr_suppression);
        return;
    }
    ia64_gen_note_successful_bundle(ctx, completed_ip, record_iipa,
                                    track_psr_suppression);
    ia64_gen_store_instruction_group_start(
        ctx->restart.next_instruction_group_start);
    ia64_gen_set_resume_slot(ip, slot);
    lookup = gen_new_label();
    tcg_gen_brcondi_i32(TCG_COND_EQ, main_loop, 0, lookup);
    tcg_gen_exit_tb(NULL, 0);
    gen_set_label(lookup);
    tcg_gen_lookup_and_goto_ptr();
}

/*
 * hint @pause (immediate 0, in any unit) marks a spin-wait loop.  When one
 * host thread runs every vCPU in turn (round-robin TCG), the loop usually
 * waits for a vCPU that cannot run until this one leaves cpu_exec, so the
 * pause yields to the next vCPU.  A time-bounded wait otherwise uses its
 * whole bound before the other vCPU gets a turn: the firmware's processor
 * rendezvous (20 ms) published one processor.  Under MTTCG every vCPU has
 * its own thread, and a gdb single step must end in its debug exception,
 * so the pause is a no-op there.  With one vCPU there is nobody to yield
 * to, and MTTCG leaves CF_PARALLEL clear then: the Server 2003 idle loop
 * would leave cpu_exec on every pause.
 */
bool ia64_insn_is_yielding_pause(const DisasContext *ctx,
                                 const Ia64Instruction *insn)
{
    switch (insn->opcode) {
    case IA64_OP_HINT_M:
    case IA64_OP_HINT_I:
    case IA64_OP_HINT_B:
    case IA64_OP_HINT_F:
    case IA64_OP_HINT_X:
        return insn->operands.common.immediate == 0 &&
               !(tb_cflags(ctx->base.tb) & (CF_PARALLEL | CF_SINGLE_STEP)) &&
               first_cpu && CPU_NEXT(first_cpu);
    default:
        return false;
    }
}

/*
 * Complete the instruction at completed_ip, store the state that resumes at
 * (ip, slot), and return to the round-robin loop (EXCP_YIELD).
 */
void ia64_gen_yield_to_slot_completed(DisasContext *ctx, uint64_t ip,
                                      uint8_t slot,
                                      uint64_t completed_ip,
                                      bool record_iipa,
                                      bool track_psr_suppression)
{
    ia64_gen_note_successful_bundle(ctx, completed_ip, record_iipa,
                                    track_psr_suppression);
    ia64_gen_store_instruction_group_start(
        ctx->restart.next_instruction_group_start);
    ia64_gen_set_resume_slot(ip, slot);
    gen_helper_yield(tcg_env);
}

void ia64_gen_goto_completed(DisasContext *ctx, uint64_t ip,
                             uint64_t completed_ip,
                             bool record_iipa,
                             bool track_psr_suppression)
{
    ia64_gen_note_taken_branch(ctx, completed_ip);
    ia64_gen_note_successful_bundle(ctx, completed_ip, record_iipa,
                                    track_psr_suppression);
    ia64_gen_goto_tb_group(ctx, ip, true);
}

bool ia64_gen_completed_direct_branch(DisasContext *ctx, TCGLabel *skip,
                                      uint64_t target,
                                      uint64_t completed_ip,
                                      bool record_iipa,
                                      bool track_psr_suppression)
{
    ia64_gen_goto_completed(ctx, target, completed_ip, record_iipa,
                            track_psr_suppression);
    return skip == NULL;
}

IA64GenResult ia64_gen_complete_branch(
    DisasContext *ctx, TCGLabel *skip,
    const IA64BranchCompletion *completion)
{
    switch (completion->target_kind) {
    case IA64_BRANCH_TARGET_DIRECT:
        if (ia64_gen_completed_direct_branch(
                ctx, skip, completion->direct_target,
                completion->completed_ip, completion->record_iipa,
                completion->track_psr_suppression)) {
            return IA64_GEN_NORETURN;
        }
        return IA64_GEN_CONTINUE;
    case IA64_BRANCH_TARGET_TCG:
        ia64_gen_lookup_tcg_completed(
            ctx, completion->tcg_target, completion->completed_ip,
            completion->record_iipa, completion->track_psr_suppression);
        break;
    case IA64_BRANCH_TARGET_CURRENT:
        ia64_gen_lookup_current_completed(
            ctx, completion->completed_ip, completion->record_iipa,
            completion->track_psr_suppression);
        break;
    default:
        g_assert_not_reached();
    }
    return skip == NULL ? IA64_GEN_NORETURN : IA64_GEN_CONTINUE;
}

static bool ia64_is_zero_st1_postinc(const Ia64Instruction *insn)
{
    return (insn->opcode == IA64_OP_ST1 ||
            insn->opcode == IA64_OP_ST1REL) &&
           insn->qp == 0 &&
           insn->operands.common.source1 == 0 &&
           insn->operands.common.source2 != 0 &&
           insn->operands.common.immediate == 1;
}

static bool ia64_analyze_self_counted_loop(
    uint8_t template_code, const IA64TemplateInfo *template_info,
    uint64_t *slots, uint64_t bundle_ip, uint8_t start_slot,
    DisasContext *ctx)
{
    bool skip_x_slot = false;
    bool zero_st1_exact = true;
    bool zero_st1_seen = false;
    uint8_t zero_st1_base = 0;
    uint8_t zero_st1_slot = 0;
    bool zero_st1_release = false;
    int self_loop_slot = -1;
    int slot;

    for (slot = start_slot; slot < 3; ++slot) {
        Ia64Instruction insn;

        if (skip_x_slot && slot == 2) {
            continue;
        }

        insn = ia64_decode_insn(template_info->units[slot],
                                slots[slot], bundle_ip, slot);
        ia64_apply_mlx_long_fixup(template_code, slots, slot, &insn,
                                  &skip_x_slot);
        if (insn.valid &&
            ((insn.opcode == IA64_OP_BR_CLOOP &&
              insn.address + insn.operands.common.immediate == bundle_ip) ||
             (insn.opcode == IA64_OP_BR_CTOP &&
              insn.operands.common.branch2 == 0 &&
              insn.address + insn.operands.common.immediate == bundle_ip))) {
            self_loop_slot = slot;
            ctx->branch.counted_self_rotates =
                insn.opcode == IA64_OP_BR_CTOP;
            if (insn.opcode != IA64_OP_BR_CLOOP || insn.qp != 0) {
                zero_st1_exact = false;
            }
            continue;
        }
        if (insn.valid && ia64_is_zero_st1_postinc(&insn)) {
            if (zero_st1_seen) {
                zero_st1_exact = false;
            } else {
                zero_st1_seen = true;
                zero_st1_base = insn.operands.common.source2;
                zero_st1_slot = slot;
                zero_st1_release = insn.mem_release;
            }
            continue;
        }
        if (!insn.valid || !ia64_insn_is_empty_hint(&insn)) {
            zero_st1_exact = false;
        }
    }

    if (self_loop_slot < 0) {
        return false;
    }

    if (zero_st1_exact && zero_st1_seen && zero_st1_slot < self_loop_slot) {
        ctx->branch.cloop_zero_st1_valid = true;
        ctx->branch.cloop_zero_st1_release = zero_st1_release;
        ctx->branch.cloop_zero_st1_base = zero_st1_base;
        ctx->branch.cloop_zero_st1_slot = zero_st1_slot;
    }
    return true;
}

void ia64_prepare_self_counted_loop(
    DisasContext *ctx, uint8_t template_code,
    const IA64TemplateInfo *template_info, uint64_t *slots,
    uint64_t bundle_ip)
{
    ctx->branch.counted_self_label = NULL;
    ctx->branch.cloop_zero_st1_valid = false;

    if (ctx->restart.start_slot != 0 ||
        ctx->psr_ss || ctx->psr_tb ||
        ctx->base.plugin_enabled ||
        (tb_cflags(ctx->base.tb) & CF_USE_ICOUNT) ||
        !ia64_analyze_self_counted_loop(
            template_code, template_info, slots, bundle_ip,
            ctx->restart.start_slot, ctx)) {
        return;
    }

    /*
     * The counted self-loop places a TCG back edge at the loop label:
     * everything after it executes again with whatever the loop body
     * left behind.  Facts cached from code above the label (a CFM.sof
     * load) would be re-consumed on the second iteration even if the body
     * invalidated them later in translation order, so drop them before
     * the label is planted.  Facts learned inside the body are
     * re-established by the re-executed code.  The NaT-known bits stay:
     * the back edge tests the ones the body cannot prove again
     * (ia64_gen_self_counted_loop), and a br.ctop back edge rotates the
     * stacked registers, so those are dropped.
     */
    ctx->cfm_sof_valid = false;
    ctx->cfm_sof_checked = 0;
    if (ctx->branch.counted_self_rotates) {
        ctx->memory.nat_known_clear[0] &= IA64_STATIC_GR_NAT_MASK;
        ctx->memory.nat_known_clear[1] = 0;
    }
    ctx->branch.counted_self_nat_known[0] = ctx->memory.nat_known_clear[0];
    ctx->branch.counted_self_nat_known[1] = ctx->memory.nat_known_clear[1];

    /*
     * The budget temp is created and initialized once per TB in
     * ia64_tr_tb_start (see there for why it cannot be initialized here);
     * multiple self-loops in one TB share it, which only tightens the cap.
     * Both paths into the label hold RI 0: the back edge stores it.
     */
    ia64_gen_sync_ri(ctx);
    ctx->branch.counted_self_label = gen_new_label();
    ctx->branch.counted_self_ip = bundle_ip;
    gen_set_label(ctx->branch.counted_self_label);
}

/* Branch to set when any GR NaT bit selected by mask0/mask1 is set. */
static void ia64_gen_brcond_nat_set(uint64_t mask0, uint64_t mask1,
                                    TCGLabel *set)
{
    TCGv_i64 t = tcg_temp_new_i64();

    tcg_gen_andi_i64(t, cpu_nat[0], mask0);
    if (mask1 != 0) {
        TCGv_i64 t1 = tcg_temp_new_i64();

        tcg_gen_andi_i64(t1, cpu_nat[1], mask1);
        tcg_gen_or_i64(t, t, t1);
    }
    tcg_gen_brcondi_i64(TCG_COND_NE, t, 0, set);
}

/*
 * The self-loop label trusts the NaT facts it was planted with; the back
 * edge takes it only when the bits the body cannot prove again are clear.
 * A br.ctop back edge has renamed the stacked registers first.
 */
static void ia64_gen_self_loop_nat_check(DisasContext *ctx, TCGLabel *fail)
{
    uint64_t known0 = ctx->memory.nat_known_clear[0];
    uint64_t known1 = ctx->memory.nat_known_clear[1];
    uint64_t test0;
    uint64_t test1;

    if (ctx->branch.counted_self_rotates) {
        known0 &= IA64_STATIC_GR_NAT_MASK;
        known1 = 0;
    }
    test0 = ctx->branch.counted_self_nat_known[0] & ~known0;
    test1 = ctx->branch.counted_self_nat_known[1] & ~known1;
    if ((test0 | test1) != 0) {
        ia64_gen_brcond_nat_set(test0, test1, fail);
    }
}

bool ia64_gen_self_counted_loop(DisasContext *ctx, uint64_t target,
                                uint64_t completed_ip,
                                bool record_iipa,
                                bool track_psr_suppression)
{
    TCGLabel *exit_to_tb;

    if (ctx->branch.counted_self_label == NULL ||
        target != ctx->branch.counted_self_ip ||
        completed_ip != ctx->branch.counted_self_ip) {
        return false;
    }

    exit_to_tb = gen_new_label();
    tcg_gen_brcondi_i64(TCG_COND_EQ, ctx->branch.counted_self_budget,
                        0, exit_to_tb);
    ia64_gen_self_loop_nat_check(ctx, exit_to_tb);
    tcg_gen_subi_i64(ctx->branch.counted_self_budget,
                     ctx->branch.counted_self_budget, 1);
    ia64_gen_note_successful_bundle(ctx, completed_ip, record_iipa,
                                    track_psr_suppression);
    /*
     * The taken branch restarts the target bundle at slot 0.  The internal
     * TCG back-edge bypasses the normal TB-entry RI initialization, so clear
     * the branch slot's RI before jumping back.  Otherwise a following
     * iteration can OR slot 1 onto stale slot 2 and expose reserved RI=3 to
     * a fault or interruption.
     */
    ia64_gen_force_ri_tracked(ctx, 0);
    tcg_gen_br(ctx->branch.counted_self_label);

    gen_set_label(exit_to_tb);
    ia64_gen_goto_completed(ctx, target, completed_ip, record_iipa,
                            track_psr_suppression);
    return true;
}

void ia64_gen_raise_exception(uint32_t exception, uint64_t fault_ip,
                              uint64_t fault_imm, uint32_t fault_slot)
{
    tcg_gen_movi_i64(cpu_ip, fault_ip);
    gen_helper_raise_exception(tcg_env, tcg_constant_i32(exception),
                                tcg_constant_i64(fault_ip),
                                tcg_constant_i64(fault_imm),
                                tcg_constant_i32(fault_slot));
}

bool ia64_cr_is_read_only(uint32_t cr_num)
{
    switch (cr_num) {
    case IA64_CR_SAPIC_IVR:
    case IA64_CR_SAPIC_IRR0:
    case IA64_CR_SAPIC_IRR1:
    case IA64_CR_SAPIC_IRR2:
    case IA64_CR_SAPIC_IRR3:
        return true;
    default:
        return false;
    }
}

bool ia64_ar_access_reads_clock(uint32_t ar_num)
{
    return ar_num == 44;
}

bool ia64_cr_write_reads_clock(uint32_t cr_num)
{
    return cr_num == IA64_CR_ITM || cr_num == IA64_CR_ITV;
}

bool ia64_clock_access_needs_io(const DisasContext *ctx)
{
    return tb_cflags(ctx->base.tb) & CF_USE_ICOUNT;
}

void ia64_gen_note_stacked_gr_write(uint8_t reg)
{
    uint32_t bit;

    if (reg < IA64_STACKED_GR_BASE) {
        return;
    }
    bit = reg - IA64_STACKED_GR_BASE;
    tcg_gen_ori_i64(cpu_rse_gr_dirty[bit / 64],
                    cpu_rse_gr_dirty[bit / 64], 1ULL << (bit % 64));
}

void ia64_gen_gr_nat_clear(uint8_t reg)
{
    if (reg == 0) {
        return;
    }

    ia64_gen_note_stacked_gr_write(reg);
    if (ia64_nat_ctx && ia64_nat_is_known_clear(ia64_nat_ctx, reg)) {
        return;
    }
    tcg_gen_andi_i64(cpu_nat[reg / 64], cpu_nat[reg / 64],
                     ~(1ULL << (reg % 64)));
}

void ia64_gen_gr_nat_set(uint8_t reg)
{
    if (reg == 0) {
        return;
    }

    ia64_gen_note_stacked_gr_write(reg);
    if (ia64_nat_ctx) {
        ia64_nat_set_known_clear(ia64_nat_ctx, reg, false);
    }
    tcg_gen_ori_i64(cpu_nat[reg / 64], cpu_nat[reg / 64],
                    1ULL << (reg % 64));
}

void ia64_gen_gr_write_nat_clear(uint8_t reg, TCGv_i64 value)
{
    if (reg == 0) {
        return;
    }
    tcg_gen_mov_i64(cpu_gr[reg], value);
    ia64_gen_gr_nat_clear(reg);
}

void ia64_gen_gr_nat_assign(uint8_t reg, TCGv_i64 bit)
{
    TCGv_i64 shifted;

    if (reg == 0) {
        return;
    }

    ia64_gen_note_stacked_gr_write(reg);
    if (ia64_nat_ctx) {
        ia64_nat_set_known_clear(ia64_nat_ctx, reg, false);
    }
    shifted = tcg_temp_new_i64();
    tcg_gen_andi_i64(shifted, bit, 1);
    tcg_gen_shli_i64(shifted, shifted, reg % 64);
    tcg_gen_andi_i64(cpu_nat[reg / 64], cpu_nat[reg / 64],
                     ~(1ULL << (reg % 64)));
    tcg_gen_or_i64(cpu_nat[reg / 64], cpu_nat[reg / 64], shifted);
}

TCGv_i64 ia64_gen_gr_nat_read(uint8_t reg)
{
    TCGv_i64 bit = tcg_temp_new_i64();

    if (reg == 0 ||
        (ia64_nat_ctx && ia64_nat_is_known_clear(ia64_nat_ctx, reg))) {
        tcg_gen_movi_i64(bit, 0);
        return bit;
    }

    tcg_gen_shri_i64(bit, cpu_nat[reg / 64], reg % 64);
    tcg_gen_andi_i64(bit, bit, 1);
    return bit;
}

void ia64_gen_gr_nat_from_1(uint8_t dst, uint8_t src)
{
    if (dst == 0) {
        return;
    }

    ia64_gen_gr_nat_assign(dst, ia64_gen_gr_nat_read(src));
}

/*
 * The implemented virtual address width is a property of the CPU model and
 * is fixed from reset, so specialise on it at translation time.
 */
static TCGv_i64 ia64_gen_va_unimplemented(DisasContext *ctx, TCGv_i64 va)
{
    TCGv_i64 result = tcg_temp_new_i64();
    uint8_t va_msb = ctx->env->impl_va_msb;

    if (va_msb >= 60) {
        tcg_gen_movi_i64(result, 0);
    } else {
        TCGv_i64 sign = tcg_temp_new_i64();
        TCGv_i64 expected = tcg_temp_new_i64();
        uint64_t count = 60 - va_msb;
        uint64_t mask = (1ULL << count) - 1;

        tcg_gen_shri_i64(sign, va, va_msb);
        tcg_gen_andi_i64(sign, sign, 1);
        tcg_gen_neg_i64(expected, sign);
        tcg_gen_andi_i64(expected, expected, mask);
        tcg_gen_shri_i64(result, va, va_msb + 1);
        tcg_gen_andi_i64(result, result, mask);
        tcg_gen_xor_i64(result, result, expected);
        tcg_gen_setcondi_i64(TCG_COND_NE, result, result, 0);
    }

    return result;
}

void ia64_gen_gr_nat_from_1_or_unimplemented_va(DisasContext *ctx,
                                                uint8_t dst, uint8_t src)
{
    TCGv_i64 nat;
    TCGv_i64 unimplemented;

    if (dst == 0) {
        return;
    }

    nat = ia64_gen_gr_nat_read(src);
    unimplemented = ia64_gen_va_unimplemented(ctx, ia64_gr_src(src));
    tcg_gen_or_i64(nat, nat, unimplemented);
    ia64_gen_gr_nat_assign(dst, nat);
}

void ia64_gen_fr_nat_from_gr(uint8_t dst, uint8_t src)
{
    if (dst <= 1) {
        return;
    }

    ia64_gen_fr_nat_assign(dst, ia64_gen_gr_nat_read(src));
}

void ia64_gen_gr_nat_from_2(uint8_t dst, uint8_t src1, uint8_t src2)
{
    TCGv_i64 bit;
    TCGv_i64 src_bit;

    if (dst == 0) {
        return;
    }

    bit = ia64_gen_gr_nat_read(src1);
    src_bit = ia64_gen_gr_nat_read(src2);
    tcg_gen_or_i64(bit, bit, src_bit);
    ia64_gen_gr_nat_assign(dst, bit);
}


void ia64_gen_check_nat_consumption(const Ia64Instruction *insn,
                                    uint8_t reg, uint64_t isr_access,
                                    Ia64NatConsumptionKind kind)
{
    const DisasContext *ctx = insn->ctx;
    TCGv_i64 nat;
    TCGLabel *ok;

    if (reg == 0 ||
        (ctx &&
         (ctx->memory.nat_known_clear[reg / 64] &
          (1ULL << (reg % 64))))) {
        return;
    }

    nat = ia64_gen_gr_nat_read(reg);
    ok = gen_new_label();
    tcg_gen_brcondi_i64(TCG_COND_EQ, nat, 0, ok);
    tcg_gen_movi_i64(cpu_ip, insn->address);
    if (kind == IA64_NAT_NON_ACCESS) {
        isr_access |= IA64_ISR_NA;
    }
    gen_helper_raise_nat_consumption(
        tcg_env, tcg_constant_i64(isr_access),
        tcg_constant_i64(insn->address | insn->slot));
    gen_set_label(ok);
}

void ia64_gen_check_fr_nat_consumption(const Ia64Instruction *insn,
                                       uint8_t reg, uint64_t isr_access)
{
    TCGLabel *ok;

    if (reg <= 1) {
        return;
    }

    ok = gen_new_label();
    tcg_gen_brcondi_i64(TCG_COND_EQ, ia64_gen_fr_nat_read(reg), 0, ok);
    tcg_gen_movi_i64(cpu_ip, insn->address);
    gen_helper_raise_nat_consumption(
        tcg_env, tcg_constant_i64(isr_access),
        tcg_constant_i64(insn->address | insn->slot));
    gen_set_label(ok);
}

static void ia64_gen_check_gr_in_frame(const Ia64Instruction *insn,
                                       uint8_t reg)
{
    if (reg == 0) {
        ia64_gen_raise_exception(IA64_EXCP_ILLEGAL, insn->address,
                                  insn->raw, insn->slot);
        return;
    }

    if (reg >= IA64_STACKED_GR_BASE) {
        DisasContext *ctx = insn->ctx;
        uint8_t required_sof = reg - IA64_STACKED_GR_BASE + 1;
        TCGLabel *valid;
        TCGv_i32 sof;

        /*
         * A successful dominating check proves every lower stacked register
         * is also in frame until an instruction changes CFM.SOF.  Avoid
         * regenerating an exception path and branch for that already-proven
         * range.
         */
        if (ctx && required_sof <= ctx->cfm_sof_checked) {
            return;
        }
        valid = gen_new_label();
        if (ctx && ctx->cfm_sof_valid) {
            sof = ctx->cfm_sof;
        } else {
            sof = tcg_temp_new_i32();
            tcg_gen_ld8u_i32(sof, tcg_env,
                             offsetof(CPUIA64State, cfm_sof));
            /*
             * A load emitted under an unknown predicate does not dominate
             * later instructions: the predicated instruction may skip it
             * at runtime.  Cache only values whose defining load
             * certainly executes.
             */
            if (ctx && insn->qp == 0) {
                ctx->cfm_sof = sof;
                ctx->cfm_sof_valid = true;
            }
        }
        tcg_gen_brcondi_i32(TCG_COND_GTU, sof,
                            reg - IA64_STACKED_GR_BASE, valid);
        ia64_gen_raise_exception(IA64_EXCP_ILLEGAL, insn->address,
                                  insn->raw, insn->slot);
        gen_set_label(valid);
        if (ctx && insn->qp == 0) {
            ctx->cfm_sof_checked = required_sof;
        }
    }
}

void ia64_gen_check_privileged(DisasContext *ctx,
                               const Ia64Instruction *insn)
{
    TCGv_i64 cpl;
    TCGLabel *allowed;
    uint64_t isr = 0x10;

    if (ctx->cpl_known && ctx->cpl == 0) {
        /* Kernel-mode TB: the check can never fail. */
        return;
    }

    if (insn->opcode == IA64_OP_TAK) {
        isr |= IA64_ISR_NA | 3;
    } else if (insn->opcode == IA64_OP_TPA) {
        isr |= IA64_ISR_NA;
    }

    cpl = tcg_temp_new_i64();
    allowed = gen_new_label();
    tcg_gen_andi_i64(cpl, cpu_psr, IA64_PSR_CPL_MASK);
    tcg_gen_brcondi_i64(TCG_COND_EQ, cpl, 0, allowed);
    tcg_gen_st_i64(tcg_constant_i64(isr), tcg_env,
                   offsetof(CPUIA64State, cr_isr));
    ia64_gen_raise_exception(IA64_EXCP_PRIVILEGED_OP, insn->address,
                              insn->raw, insn->slot);
    gen_set_label(allowed);
}

void ia64_gen_check_register_index(const Ia64Instruction *insn,
                                   TCGv_i64 index, uint32_t count)
{
    TCGv_i64 low8 = tcg_temp_new_i64();
    TCGLabel *valid = gen_new_label();

    tcg_gen_andi_i64(low8, index, 0xff);
    tcg_gen_brcondi_i64(TCG_COND_LTU, low8, count, valid);
    tcg_gen_st_i64(tcg_constant_i64(0x30), tcg_env,
                   offsetof(CPUIA64State, cr_isr));
    ia64_gen_raise_exception(IA64_EXCP_RESERVED_REG_FIELD,
                              insn->address, insn->raw, insn->slot);
    gen_set_label(valid);
}

void ia64_gen_check_reserved_bits(const Ia64Instruction *insn,
                                  TCGv_i64 value, uint64_t allowed)
{
    TCGv_i64 reserved = tcg_temp_new_i64();
    TCGLabel *valid = gen_new_label();

    tcg_gen_andi_i64(reserved, value, ~allowed);
    tcg_gen_brcondi_i64(TCG_COND_EQ, reserved, 0, valid);
    tcg_gen_st_i64(tcg_constant_i64(0x30), tcg_env,
                   offsetof(CPUIA64State, cr_isr));
    ia64_gen_raise_exception(IA64_EXCP_RESERVED_REG_FIELD,
                              insn->address, insn->raw, insn->slot);
    gen_set_label(valid);
}

void ia64_gen_write_user_mask(TCGv_i64 value)
{
    TCGv_i64 new_psr = tcg_temp_new_i64();
    TCGv_i64 new_um = tcg_temp_new_i64();
    TCGv_i64 secure = tcg_temp_new_i64();
    TCGv_i64 old_up = tcg_temp_new_i64();
    TCGLabel *done = gen_new_label();

    tcg_gen_andi_i64(new_psr, cpu_psr, ~IA64_PSR_UM_WRITABLE_MASK);
    tcg_gen_andi_i64(new_um, value, IA64_PSR_UM_WRITABLE_MASK);
    tcg_gen_or_i64(new_psr, new_psr, new_um);

    /* PSR.sp makes the user performance-monitor bit read-only. */
    tcg_gen_andi_i64(secure, cpu_psr, IA64_PSR_SP);
    tcg_gen_brcondi_i64(TCG_COND_EQ, secure, 0, done);
    tcg_gen_andi_i64(new_psr, new_psr, ~IA64_PSR_UP);
    tcg_gen_andi_i64(old_up, cpu_psr, IA64_PSR_UP);
    tcg_gen_or_i64(new_psr, new_psr, old_up);
    gen_set_label(done);

    tcg_gen_mov_i64(cpu_psr, new_psr);
}

void ia64_gen_validate_ar_access(const Ia64Instruction *insn,
                                 TCGv_i64 value, bool write)
{
    gen_helper_validate_ar_access(tcg_env, value,
                                  tcg_constant_i32(
                                      insn->operands.common.source1),
                                  tcg_constant_i32(write),
                                  tcg_constant_i64(insn->address),
                                  tcg_constant_i64(insn->raw),
                                  tcg_constant_i32(insn->slot));
}

bool ia64_ar_is_simple(uint32_t ar)
{
    switch (ar) {
    case 32: /* CCV */
    case 36: /* UNAT */
    case 40: /* FPSR */
    case 64: /* PFS */
    case 65: /* LC */
    case 66: /* EC */
        return true;
    default:
        return false;
    }
}

void ia64_gen_read_simple_ar(TCGv_i64 value, uint32_t ar)
{
    tcg_gen_ld_i64(value, tcg_env,
                   offsetof(CPUIA64State, ar) + ar * sizeof(uint64_t));
}

void ia64_gen_write_simple_ar(uint32_t ar, TCGv_i64 value)
{
    if (ar == 66) {
        TCGv_i64 masked = tcg_temp_new_i64();

        tcg_gen_andi_i64(masked, value, 0x3f);
        value = masked;
    }
    tcg_gen_st_i64(value, tcg_env,
                   offsetof(CPUIA64State, ar) + ar * sizeof(uint64_t));
}

void ia64_gen_validate_cr_access(TCGv_i64 result,
                                 const Ia64Instruction *insn,
                                 TCGv_i64 value, bool write)
{
    gen_helper_validate_cr_access(result, tcg_env, value,
                                  tcg_constant_i32(
                                      insn->operands.common.source1),
                                  tcg_constant_i32(write),
                                  tcg_constant_i64(insn->address),
                                  tcg_constant_i64(insn->raw),
                                  tcg_constant_i32(insn->slot));
}

void ia64_gen_check_nat_register(const Ia64Instruction *insn, uint8_t reg)
{
    ia64_gen_check_nat_consumption(insn, reg, 0, IA64_NAT_ACCESS);
}


/*
 * IA-64 acquire/release completers are one-way ordering constraints.
 * Full serialization is reserved for explicit fence instructions such as mf.
 */
#define IA64_TCG_MO_ACQUIRE \
    (TCG_BAR_LDAQ | TCG_MO_LD_LD | TCG_MO_LD_ST)
#define IA64_TCG_MO_RELEASE \
    (TCG_BAR_STRL | TCG_MO_LD_ST | TCG_MO_ST_ST)

void ia64_gen_memory_acquire(const Ia64Instruction *insn)
{
    if (insn->mem_acquire) {
        tcg_gen_mb(IA64_TCG_MO_ACQUIRE);
    }
}

void ia64_gen_memory_release(const Ia64Instruction *insn)
{
    if (insn->mem_release) {
        tcg_gen_mb(IA64_TCG_MO_RELEASE);
    }
}

bool ia64_gen_zero_st1_cloop(DisasContext *ctx,
                             const Ia64Instruction *insn,
                             uint64_t target,
                             TCGLabel *l_nobr,
                             bool record_iipa,
                             bool track_psr_suppression)
{
    TCGv_i64 taken;

    if (!ctx->branch.cloop_zero_st1_valid ||
        target != ctx->branch.counted_self_ip ||
        insn->address != ctx->branch.counted_self_ip) {
        return false;
    }

    if (ctx->branch.cloop_zero_st1_release) {
        tcg_gen_mb(IA64_TCG_MO_RELEASE);
    }

    /*
     * Faults from the helper's future loop-body stores must be reported as
     * the original store slot, not the branch slot that calls the helper.
     */
    ia64_gen_force_ri_tracked(ctx, ctx->branch.cloop_zero_st1_slot);
    taken = tcg_temp_new_i64();
    gen_helper_cloop_zero_st1(taken, tcg_env,
                              tcg_constant_i32(ctx->branch.cloop_zero_st1_base),
                              tcg_constant_i32(ctx->memory.mmu_idx),
                              tcg_constant_i32(IA64_CLOOP_ZERO_ST1_MAX));
    ia64_gen_note_stacked_gr_write(ctx->branch.cloop_zero_st1_base);
    ia64_gen_force_ri_tracked(ctx, insn->slot);

    tcg_gen_brcondi_i64(TCG_COND_EQ, taken, 0, l_nobr);
    ia64_gen_goto_completed(ctx, target, insn->address, record_iipa,
                            track_psr_suppression);
    return true;
}


void ia64_gen_check_branch(DisasContext *ctx, TCGv_i64 failed,
                           uint64_t target,
                           uint64_t completed_ip,
                           bool record_iipa,
                           bool track_psr_suppression)
{
    TCGLabel *l_done = gen_new_label();

    tcg_gen_brcondi_i64(TCG_COND_EQ, failed, 0, l_done);
    ia64_gen_goto_completed(ctx, target, completed_ip, record_iipa,
                            track_psr_suppression);
    gen_set_label(l_done);
}

void ia64_gen_sync_ip_for_helper(const Ia64Instruction *insn)
{
    tcg_gen_movi_i64(cpu_ip, ia64_ip_bundle_addr(insn->address));
    ia64_gen_set_fault_slot(insn->slot);
}




static uint32_t ia64_fp_reg_set(uint8_t reg)
{
    if (reg >= 32) {
        return 2;
    }
    if (reg >= 2) {
        return 1;
    }
    return 0;
}

static uint32_t ia64_insn_fp_read_sets(const Ia64Instruction *insn)
{
    uint32_t sets;

    switch (insn->opcode) {
    case IA64_OP_LDFD:
    case IA64_OP_LDFS:
    case IA64_OP_LDF_FILL:
    case IA64_OP_LDF8:
    case IA64_OP_LDFE:
    case IA64_OP_SETF_D:
    case IA64_OP_SETF_S:
    case IA64_OP_SETF_EXP:
    case IA64_OP_SETF_SIG:
        return 0;

    case IA64_OP_LDFP8:
    case IA64_OP_LDFPD:
    case IA64_OP_LDFPS:
        return 0;

    case IA64_OP_STFD:
    case IA64_OP_STFS:
    case IA64_OP_STF_SPILL:
    case IA64_OP_STF8:
    case IA64_OP_STFE:
    case IA64_OP_GETF_D:
    case IA64_OP_GETF_S:
    case IA64_OP_GETF_EXP:
    case IA64_OP_GETF_SIG:
    case IA64_OP_FCLASS:
        return ia64_fp_reg_set(insn->operands.common.source1);

    case IA64_OP_CHK_S:
        return insn->check_fp ?
            ia64_fp_reg_set(insn->operands.common.source1) : 0;

    case IA64_OP_CHK_A:
    case IA64_OP_CHK_A_CLR:
        /* A floating chk.a uses the FR number only as an ALAT tag. */
        return 0;

    case IA64_OP_FCMP:
        return ia64_fp_reg_set(insn->operands.common.source1) |
               ia64_fp_reg_set(insn->operands.common.source2);

    case IA64_OP_FMOV:
    case IA64_OP_FCVT_XF:
    case IA64_OP_FCVT_FX:
    case IA64_OP_FCVT_FXU:
    case IA64_OP_FPCVT:
        return ia64_fp_reg_set(insn->operands.common.source1);

    case IA64_OP_FPRSQRTA:
    case IA64_OP_FRSQRTA:
        return ia64_fp_reg_set(insn->operands.common.source2);

    case IA64_OP_FMA:
    case IA64_OP_FMS:
    case IA64_OP_FNMA:
    case IA64_OP_FPMA:
    case IA64_OP_FPMS:
    case IA64_OP_FPNMA:
    case IA64_OP_FSELECT:
    case IA64_OP_XMA_L:
    case IA64_OP_XMA_H:
    case IA64_OP_XMA_HU:
    case IA64_OP_XMPY_HU:
        return ia64_fp_reg_set(insn->operands.common.source1) |
               ia64_fp_reg_set(insn->operands.common.source2) |
               ia64_fp_reg_set(insn->operands.common.auxiliary1);

    case IA64_OP_FADD:
    case IA64_OP_FSUB:
    case IA64_OP_FMPY:
    case IA64_OP_FMIN:
    case IA64_OP_FMAX:
    case IA64_OP_FAMIN:
    case IA64_OP_FAMAX:
    case IA64_OP_FRCPA:
    case IA64_OP_FPRCPA:
    case IA64_OP_FNORM:
    case IA64_OP_FPACK:
    case IA64_OP_FAND:
    case IA64_OP_FANDCM:
    case IA64_OP_FOR:
    case IA64_OP_FXOR:
    case IA64_OP_FSWAP:
    case IA64_OP_FSWAP_NL:
    case IA64_OP_FSWAP_NR:
    case IA64_OP_FMIX_LR:
    case IA64_OP_FMIX_R:
    case IA64_OP_FMIX_L:
    case IA64_OP_FSXT_R:
    case IA64_OP_FSXT_L:
    case IA64_OP_FPMERGE:
    case IA64_OP_FPMERGE_S:
    case IA64_OP_FPMERGE_SE:
    case IA64_OP_FPMIN:
    case IA64_OP_FPMAX:
    case IA64_OP_FPAMIN:
    case IA64_OP_FPAMAX:
    case IA64_OP_FPCMP:
    case IA64_OP_FMERGE:
    case IA64_OP_FMERGE_S:
    case IA64_OP_FMERGE_SE:
        sets = ia64_fp_reg_set(insn->operands.common.source1);
        sets |= ia64_fp_reg_set(insn->operands.common.source2);
        return sets;

    default:
        return 0;
    }
}

static uint32_t ia64_insn_fp_write_sets(const Ia64Instruction *insn)
{
    switch (insn->opcode) {
    case IA64_OP_LDFD:
    case IA64_OP_LDFS:
    case IA64_OP_LDF_FILL:
    case IA64_OP_LDF8:
    case IA64_OP_LDFE:
    case IA64_OP_SETF_D:
    case IA64_OP_SETF_S:
    case IA64_OP_SETF_EXP:
    case IA64_OP_SETF_SIG:
    case IA64_OP_FADD:
    case IA64_OP_FSUB:
    case IA64_OP_FMPY:
    case IA64_OP_FMA:
    case IA64_OP_FMS:
    case IA64_OP_FNMA:
    case IA64_OP_XMA_L:
    case IA64_OP_XMA_H:
    case IA64_OP_XMA_HU:
    case IA64_OP_XMPY_HU:
    case IA64_OP_FMIN:
    case IA64_OP_FMAX:
    case IA64_OP_FAMIN:
    case IA64_OP_FAMAX:
    case IA64_OP_FRCPA:
    case IA64_OP_FPRCPA:
    case IA64_OP_FSELECT:
    case IA64_OP_FNORM:
    case IA64_OP_FPRSQRTA:
    case IA64_OP_FRSQRTA:
    case IA64_OP_FPACK:
    case IA64_OP_FAND:
    case IA64_OP_FANDCM:
    case IA64_OP_FOR:
    case IA64_OP_FXOR:
    case IA64_OP_FSWAP:
    case IA64_OP_FSWAP_NL:
    case IA64_OP_FSWAP_NR:
    case IA64_OP_FMIX_LR:
    case IA64_OP_FMIX_R:
    case IA64_OP_FMIX_L:
    case IA64_OP_FSXT_R:
    case IA64_OP_FSXT_L:
    case IA64_OP_FPMERGE:
    case IA64_OP_FPMERGE_S:
    case IA64_OP_FPMERGE_SE:
    case IA64_OP_FPMIN:
    case IA64_OP_FPMAX:
    case IA64_OP_FPAMIN:
    case IA64_OP_FPAMAX:
    case IA64_OP_FPCMP:
    case IA64_OP_FPCVT:
    case IA64_OP_FPMA:
    case IA64_OP_FPMS:
    case IA64_OP_FPNMA:
    case IA64_OP_FMOV:
    case IA64_OP_FCVT_XF:
    case IA64_OP_FCVT_FX:
    case IA64_OP_FCVT_FXU:
    case IA64_OP_FMERGE:
    case IA64_OP_FMERGE_S:
    case IA64_OP_FMERGE_SE:
        return ia64_fp_reg_set(insn->operands.common.destination);

    case IA64_OP_LDFP8:
    case IA64_OP_LDFPD:
    case IA64_OP_LDFPS:
        return ia64_fp_reg_set(insn->operands.common.destination) |
               ia64_fp_reg_set(insn->operands.common.source1);

    default:
        return 0;
    }
}

static uint64_t ia64_insn_disabled_fp_isr_flags(
    const Ia64Instruction *insn)
{
    /* ISR.R/W describe the instruction's memory access, not its FP operands. */
    switch (insn->opcode) {
    case IA64_OP_LDFD:
    case IA64_OP_LDFS:
    case IA64_OP_LDF_FILL:
    case IA64_OP_LDF8:
    case IA64_OP_LDFE:
    case IA64_OP_LDFP8:
    case IA64_OP_LDFPD:
    case IA64_OP_LDFPS:
        return IA64_ISR_R |
               (insn->fp_load_speculative ? IA64_ISR_SP : 0);

    case IA64_OP_STFD:
    case IA64_OP_STFS:
    case IA64_OP_STF_SPILL:
    case IA64_OP_STF8:
    case IA64_OP_STFE:
        return IA64_ISR_W;

    default:
        return 0;
    }
}

static void ia64_gen_check_disabled_fp(const Ia64Instruction *insn)
{
    uint32_t reads = ia64_insn_fp_read_sets(insn);
    uint32_t writes = ia64_insn_fp_write_sets(insn);
    uint32_t sets = reads | writes;
    uint64_t disabled_mask;
    uint64_t isr_flags;
    TCGv_i64 disabled;
    TCGv_i64 isr;
    TCGLabel *done;

    if (sets == 0) {
        return;
    }

    /*
     * Check the live PSR: a mask instruction earlier in the same bundle can
     * change dfl/dfh after the TB was translated.
     */
    done = gen_new_label();
    disabled = tcg_temp_new_i64();
    isr = tcg_temp_new_i64();
    disabled_mask = ((sets & 1) ? IA64_PSR_DFL : 0) |
                    ((sets & 2) ? IA64_PSR_DFH : 0);
    isr_flags = ia64_insn_disabled_fp_isr_flags(insn);

    tcg_gen_andi_i64(disabled, cpu_psr, disabled_mask);
    tcg_gen_brcondi_i64(TCG_COND_EQ, disabled, 0, done);
    tcg_gen_shri_i64(isr, disabled, 18);
    if (isr_flags != 0) {
        tcg_gen_ori_i64(isr, isr, isr_flags);
    }
    tcg_gen_st_i64(isr, tcg_env, offsetof(CPUIA64State, cr_isr));
    ia64_gen_raise_exception(IA64_EXCP_DISABLED_FP, insn->address,
                             0, insn->slot);
    gen_set_label(done);
}

/*
 * The CPUID[4] capability bit an optional instruction needs, 0 for none.
 * clz, mpy4 and mpyshl4 fault only when PR[qp] is 1 (SDM Vol 3 clz, mpy4,
 * mpyshl4 operation).
 */
static uint64_t ia64_insn_cpuid4_feature(const Ia64Instruction *insn)
{
    switch (insn->opcode) {
    case IA64_OP_LD16:
    case IA64_OP_ST16:
    case IA64_OP_CMP8XCHG16:
        return IA64_CPUID4_AO;
    case IA64_OP_CLZ:
        return IA64_CPUID4_CZ;
    case IA64_OP_MPY4:
    case IA64_OP_MPYSHL4:
        return IA64_CPUID4_X2;
    default:
        return 0;
    }
}

/* Instructions gated by the CPUID[4].lb long-branch capability bit. */
static bool ia64_insn_needs_long_branch(const Ia64Instruction *insn)
{
    switch (insn->opcode) {
    case IA64_OP_BRL_COND:
    case IA64_OP_BRL_CALL:
        return true;
    default:
        return false;
    }
}

static IA64GenResult ia64_gen_dispatch(DisasContext *ctx,
                                       const Ia64Instruction *insn,
                                       TCGLabel *skip, bool record_iipa,
                                       bool track_psr_suppression)
{
    IA64GenResult result;

    result = ia64_gen_integer(ctx, insn);
    if (result != IA64_GEN_UNHANDLED) {
        return result;
    }
    result = ia64_gen_memory(ctx, insn, skip, record_iipa,
                             track_psr_suppression);
    if (result != IA64_GEN_UNHANDLED) {
        return result;
    }
    result = ia64_gen_fp(ctx, insn);
    if (result != IA64_GEN_UNHANDLED) {
        return result;
    }
    result = ia64_gen_simd(ctx, insn);
    if (result != IA64_GEN_UNHANDLED) {
        return result;
    }
    result = ia64_gen_system(ctx, insn, skip, record_iipa,
                             track_psr_suppression);
    if (result != IA64_GEN_UNHANDLED) {
        return result;
    }
    return ia64_gen_branch(ctx, insn, skip, record_iipa,
                           track_psr_suppression);
}

typedef enum IA64PrepareResult {
    IA64_PREPARE_DISPATCH,
    IA64_PREPARE_COMPLETE,
    IA64_PREPARE_NORETURN,
} IA64PrepareResult;

static IA64PrepareResult ia64_gen_prepare_insn(
    DisasContext *ctx, const Ia64Instruction *insn,
    TCGLabel **predicate_skip)
{
    TCGLabel *skip;
    TCGv_i64 qp_value;

    if (!insn->valid) {
        static unsigned invalid_logs;

        if (qatomic_fetch_inc(&invalid_logs) < 128) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "ia64 invalid insn ip=0x%016" PRIx64
                          " slot=%u unit=%u raw=0x%010" PRIx64 "\n",
                          insn->address, insn->slot, insn->unit,
                          insn->raw);
        }
        ia64_gen_raise_exception(IA64_EXCP_ILLEGAL, insn->address,
                                  insn->raw, insn->slot);
        return IA64_PREPARE_NORETURN;
    }

    qp_value = insn->qp == 0 ? tcg_constant_i64(1) : cpu_pr[insn->qp];
    if ((insn->compare_unc ||
         (insn->clear_p2_before_predicate &&
          insn->qp == insn->operands.common.auxiliary2)) &&
        insn->qp != 0) {
        TCGv_i64 saved_qp = tcg_temp_new_i64();

        tcg_gen_mov_i64(saved_qp, qp_value);
        qp_value = saved_qp;
    }
    if (insn->compare_unc && ia64_compare_has_equal_targets(insn)) {
        ia64_gen_raise_exception(IA64_EXCP_ILLEGAL, insn->address,
                                  insn->raw, insn->slot);
        return IA64_PREPARE_NORETURN;
    }
    if (insn->opcode == IA64_OP_ALLOC && insn->qp != 0) {
        ia64_gen_raise_exception(IA64_EXCP_ILLEGAL, insn->address,
                                  insn->raw, insn->slot);
        return IA64_PREPARE_NORETURN;
    }
    if (ia64_insn_needs_long_branch(insn) &&
        !(ia64_env_cpu_class(ctx->env)->cpuid_features & IA64_CPUID4_LB)) {
        /*
         * 245319-002 Vol. 3, brl: "This instruction is not implemented on the
         * Intel Itanium processor, which takes an Illegal Operation fault
         * whenever a long branch instruction is encountered, regardless of
         * whether the branch is taken or not...  Presence of this
         * instruction is indicated by a 1 in the lb bit of CPUID register
         * 4."  The fault ignores the qualifying predicate (SDM Vol 2 §7.4).
         * Windows keys KF_BRL off that bit and emulates brl from its Illegal
         * Operation handler when it is clear.
         */
        ia64_gen_raise_exception(IA64_EXCP_ILLEGAL, insn->address,
                                  insn->raw, insn->slot);
        return IA64_PREPARE_NORETURN;
    }
    ia64_gen_clear_unc_compare_targets(insn);
    /*
     * frcpa, frsqrta, fprcpa and fprsqrta clear p2 when PR[qp] is 0.  With
     * PR[qp] 1 only the helper writes it, so a fault leaves it unchanged
     * (SDM Vol 3 frcpa, frsqrta).
     */
    if (insn->clear_p2_before_predicate &&
        insn->operands.common.auxiliary2 != 0 && insn->qp != 0) {
        tcg_gen_and_i64(cpu_pr[insn->operands.common.auxiliary2],
                        cpu_pr[insn->operands.common.auxiliary2], qp_value);
    }
    skip = ia64_gen_predicate_skip(insn, qp_value);
    *predicate_skip = skip;
    if (insn->placement_illegal) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "ia64 illegal placement ip=0x%016" PRIx64
                      " slot=%u opcode=%u raw=0x%010" PRIx64 "\n",
                      insn->address, insn->slot, insn->opcode, insn->raw);
        ia64_gen_raise_exception(IA64_EXCP_ILLEGAL, insn->address,
                                  insn->raw, insn->slot);
        if (skip == NULL) {
            return IA64_PREPARE_NORETURN;
        }
        ia64_gen_predicate_end(skip);
        return IA64_PREPARE_COMPLETE;
    }
    if (insn->reserved_field) {
        tcg_gen_st_i64(tcg_constant_i64(0x30), tcg_env,
                       offsetof(CPUIA64State, cr_isr));
        ia64_gen_raise_exception(IA64_EXCP_RESERVED_REG_FIELD,
                                  insn->address, insn->raw, insn->slot);
        if (skip == NULL) {
            return IA64_PREPARE_NORETURN;
        }
        ia64_gen_predicate_end(skip);
        return IA64_PREPARE_COMPLETE;
    }
    ia64_gen_check_fp_pair_bank(insn);
    if (ia64_insn_cpuid4_feature(insn) &
        ~ia64_env_cpu_class(ctx->env)->cpuid_features) {
        /*
         * The instruction is not implemented on a model that clears its
         * CPUID[4] bit, so refuse it the way an unimplemented opcode is
         * refused.
         */
        ia64_gen_raise_exception(IA64_EXCP_ILLEGAL, insn->address,
                                  insn->raw, insn->slot);
        if (skip == NULL) {
            return IA64_PREPARE_NORETURN;
        }
        ia64_gen_predicate_end(skip);
        return IA64_PREPARE_COMPLETE;
    }
    if (insn->opcode == IA64_OP_ALLOC) {
        uint32_t new_sof = insn->operands.common.immediate & 0x7f;

        if (insn->operands.common.destination == 0 ||
            (insn->operands.common.destination >= IA64_STACKED_GR_BASE &&
             insn->operands.common.destination - IA64_STACKED_GR_BASE >=
             new_sof)) {
            ia64_gen_raise_exception(IA64_EXCP_ILLEGAL, insn->address,
                                      insn->raw, insn->slot);
            if (skip == NULL) {
                return IA64_PREPARE_NORETURN;
            }
            ia64_gen_predicate_end(skip);
            return IA64_PREPARE_COMPLETE;
        }
    } else if (ia64_insn_writes_gr_r1(insn)) {
        ia64_gen_check_gr_in_frame(insn, insn->operands.common.destination);
    }
    if (insn->reg_base_update || insn->imm_base_update) {
        ia64_gen_check_gr_in_frame(insn, insn->operands.common.source2);
    }
    if (ia64_insn_is_privileged(insn)) {
        ia64_gen_check_privileged(ctx, insn);
    }
    if (ia64_load_base_update_has_same_target(insn)) {
        ia64_gen_raise_exception(IA64_EXCP_ILLEGAL, insn->address,
                                  insn->raw, insn->slot);
        if (skip == NULL) {
            return IA64_PREPARE_NORETURN;
        }
        ia64_gen_predicate_end(skip);
        return IA64_PREPARE_COMPLETE;
    }
    if (ia64_compare_has_equal_targets(insn) ||
        /* FR 0 and FR 1 are read-only (SDM Vol 3 fp_check_target_register). */
        (ia64_insn_writes_fr_f1(insn) &&
         insn->operands.common.destination <= 1)) {
        ia64_gen_raise_exception(IA64_EXCP_ILLEGAL, insn->address,
                                  insn->raw, insn->slot);
        if (skip == NULL) {
            return IA64_PREPARE_NORETURN;
        }
        ia64_gen_predicate_end(skip);
        return IA64_PREPARE_COMPLETE;
    }

    ia64_gen_check_disabled_fp(insn);
    return IA64_PREPARE_DISPATCH;
}

bool ia64_gen_insn(DisasContext *ctx, const Ia64Instruction *insn,
                   bool record_iipa)
{
    TCGLabel *skip = NULL;
    IA64PrepareResult prepare;
    IA64GenResult result;
    const bool track_psr_suppression = ctx->restart.track_psr_suppression;

    prepare = ia64_gen_prepare_insn(ctx, insn, &skip);
    if (prepare == IA64_PREPARE_NORETURN) {
        return true;
    }
    if (prepare == IA64_PREPARE_COMPLETE) {
        return false;
    }
    result = ia64_gen_dispatch(ctx, insn, skip, record_iipa,
                               track_psr_suppression);
    if (result == IA64_GEN_NORETURN) {
        return true;
    }
    if (result == IA64_GEN_CONTINUE) {
        ia64_gen_predicate_end(skip);
        ia64_gen_note_successful_bundle(ctx, insn->address, record_iipa,
                                        track_psr_suppression);
        return false;
    }

    if (insn->opcode != IA64_OP_ILLEGAL) {
        g_assert_not_reached();
    }
    ia64_gen_raise_exception(IA64_EXCP_ILLEGAL, insn->address,
                             insn->raw, insn->slot);
    if (skip == NULL) {
        return true;
    }

    ia64_gen_predicate_end(skip);
    ia64_gen_note_successful_bundle(ctx, insn->address, record_iipa,
                                    track_psr_suppression);
    return false;
}

/*
 * One-shot guest-physical dump when a guest virtual page is first translated:
 *   IA64_DUMP_ON_TRANSLATE=<trigger-va>:<phys-base>:<length>:<file>
 * fires the first time a TB whose entry pc lands in <trigger-va>'s 4 KiB page
 * is translated, writing guest-physical [<phys-base>, +<length>) to <file>.
 * Debug rig for capturing an OS loader's phase-0 memory-descriptor lists at a
 * precise code point without a debugger.  Zero cost unless the environment
 * variable is set.
 */
static void ia64_dump_on_translate(uint64_t pc)
{
    static int state; /* 0 unparsed, -1 off, 1 armed, 2 fired */
    static uint64_t trigger_va;
    static uint64_t dump_pa;
    static uint64_t dump_len;
    static char dump_file[256];

    if (state < 0 || state == 2) {
        return;
    }
    if (state == 0) {
        const char *spec = getenv("IA64_DUMP_ON_TRANSLATE");
        char *end;

        state = -1;
        if (spec == NULL) {
            return;
        }
        trigger_va = strtoull(spec, &end, 0);
        if (*end != ':') {
            return;
        }
        dump_pa = strtoull(end + 1, &end, 0);
        if (*end != ':') {
            return;
        }
        dump_len = strtoull(end + 1, &end, 0);
        if (*end != ':' || dump_len == 0 ||
            strlen(end + 1) >= sizeof(dump_file)) {
            return;
        }
        g_strlcpy(dump_file, end + 1, sizeof(dump_file));
        state = 1;
    }
    if ((pc >> 12) == (trigger_va >> 12)) {
        void *buf = g_malloc(dump_len);
        FILE *f = fopen(dump_file, "wb");

        cpu_physical_memory_read(dump_pa, buf, dump_len);
        if (f != NULL) {
            fwrite(buf, 1, dump_len, f);
            fclose(f);
        }
        g_free(buf);
        fprintf(stderr, "IA64_DUMP_ON_TRANSLATE: pc=0x%" PRIx64
                " dumped [0x%" PRIx64 ", +0x%" PRIx64 ") to %s\n",
                pc, dump_pa, dump_len, dump_file);
        state = 2;
    }
}

static void ia64_tr_init_disas_context(DisasContextBase *db, CPUState *cs)
{
    DisasContext *ctx = container_of(db, DisasContext, base);
    uint32_t flags = ctx->base.tb->flags;

    ia64_dump_on_translate(ctx->base.pc_first);

    ctx->env = cpu_env(cs);
    ctx->memory.mmu_idx = (flags & IA64_TB_FLAG_DT) ?
        MMU_IDX_VIRT_CPL((flags & IA64_TB_FLAG_CPL_MASK) >>
                         IA64_TB_FLAG_CPL_SHIFT) :
        MMU_PHYS_IDX;
    ctx->cpl = (flags & IA64_TB_FLAG_CPL_MASK) >> IA64_TB_FLAG_CPL_SHIFT;
    ctx->cpl_known = true;
    ctx->cfm_sof_valid = false;
    ctx->cfm_sof_checked = 0;
    ctx->restart.start_slot = (ctx->base.tb->flags & IA64_TB_FLAG_RI_MASK) >>
                      IA64_TB_FLAG_RI_SHIFT;
    if (ctx->restart.start_slot > 2) {
        ctx->restart.start_slot = 0;
    }
    ctx->restart.current_ri = ctx->restart.start_slot;
    ctx->restart.current_ri_known = true;
    ctx->restart.logical_ri = ctx->restart.start_slot;
    ctx->restart.track_iipa = ctx->base.tb->flags & IA64_TB_FLAG_PSR_IC;
    ctx->restart.track_psr_suppression =
        ctx->base.tb->flags & IA64_TB_FLAG_PSR_SUPPRESS;
    ctx->memory.be_data = ctx->base.tb->flags & IA64_TB_FLAG_BE;
    ctx->memory.psr_ac = ctx->base.tb->flags & IA64_TB_FLAG_PSR_AC;
    ctx->memory.full_alat = ctx->env->alat_state.alat_full;
    if (ctx->base.tb->flags & IA64_TB_FLAG_NAT_CLEAR) {
        ctx->memory.nat_known_clear[0] = UINT64_MAX;
        ctx->memory.nat_known_clear[1] = UINT64_MAX;
    } else {
        ctx->memory.nat_known_clear[0] = 1;
        ctx->memory.nat_known_clear[1] = 0;
    }
    ctx->memory.nat_known_at_exit[0] = ctx->memory.nat_known_clear[0];
    ctx->memory.nat_known_at_exit[1] = ctx->memory.nat_known_clear[1];
    ctx->memory.nat_known_before[0] = ctx->memory.nat_known_clear[0];
    ctx->memory.nat_known_before[1] = ctx->memory.nat_known_clear[1];
    ia64_nat_ctx = ctx;
    ctx->restart.instruction_group_start =
        ctx->base.tb->flags & IA64_TB_FLAG_GROUP_START;
    ctx->restart.next_instruction_group_start =
        ctx->restart.instruction_group_start;
    ctx->psr_ss = flags & IA64_TB_FLAG_PSR_SS;
    ctx->psr_tb = flags & IA64_TB_FLAG_PSR_TB;
}

static void ia64_tr_tb_start(DisasContextBase *db, CPUState *cs)
{
    DisasContext *ctx = container_of(db, DisasContext, base);

    /*
     * Shared iteration budget for the counted self-loop back edges
     * (ia64_prepare_self_counted_loop).  It MUST be created and initialized
     * here, in the TB's entry EBB, not next to the loop label: the init
     * would otherwise sit between a preceding unconditional exit and the
     * loop label, and when the optimizer proves that exit always taken
     * (e.g. a predicate constant-folds), reachable_code_pass deletes the
     * init while the label survives through its back-edge reference.
     * liveness then reduces the now single-EBB temp to TEMP_EBB, discards
     * its remaining writes, and the loop's budget test reads a dead temp -
     * a temp_load() abort at code-generation time (measured: Server 2003
     * SP1 GUI setup, -cpu montecito -smp 4).  In the entry EBB the init is
     * reachable by construction whenever any use survives.  TBs without a
     * self-loop lose nothing: the unused temp's init is dead-code removed.
     */
    ctx->branch.counted_self_budget = tcg_temp_new_i64();
    tcg_gen_movi_i64(ctx->branch.counted_self_budget,
                     IA64_COUNTED_SELF_BUDGET);
}

/*
 * Whether the code for insn can observe PSR.ri: through a helper, a memory
 * access that may fault, an exception or a TB exit.  The rest runs with a
 * stale PSR.ri, which nothing reads before the next sync.
 */
static bool ia64_insn_observes_ri(DisasContext *ctx,
                                  const Ia64Instruction *insn)
{
    uint8_t dst = insn->operands.common.destination;

    if (ctx->psr_ss || ctx->psr_tb || ctx->restart.track_psr_suppression ||
        ctx->base.plugin_enabled || !insn->valid ||
        insn->placement_illegal || insn->reserved_field ||
        (ia64_insn_cpuid4_feature(insn) &
         ~ia64_env_cpu_class(ctx->env)->cpuid_features)) {
        return true;
    }
    switch (ia64_integer_pure_kind(insn)) {
    case IA64_PURE_GR:
        /* r0 and an unproven stacked target take the frame check's fault. */
        return dst == 0 ||
               (dst >= IA64_STACKED_GR_BASE &&
                dst - IA64_STACKED_GR_BASE + 1 > ctx->cfm_sof_checked);
    case IA64_PURE_PR:
        return ia64_compare_has_equal_targets(insn);
    default:
        return true;
    }
}

static void ia64_tr_insn_start(DisasContextBase *db, CPUState *cs)
{
    tcg_gen_insn_start(db->pc_next, 0, 0);
}

static void ia64_tr_translate_insn(DisasContextBase *db, CPUState *cs)
{
    DisasContext *ctx = container_of(db, DisasContext, base);
    const uint64_t bundle_ip = db->pc_next;
    uint64_t low;
    uint64_t high;
    uint8_t template_code;
    const IA64TemplateInfo *template_info;
    uint64_t slots[3];
    bool skip_x_slot;
    bool record_iipa;
    bool psr_ic_modified;
    int slot;

    if (ctx->base.tb->flags & IA64_TB_FLAG_PSR_IS) {
        db->pc_next = bundle_ip + 1;
        gen_helper_ia32_unsupported(tcg_env);
        db->is_jmp = DISAS_NORETURN;
        return;
    }

    {
        /* Full-speed IP trace (debug).  IA64_IPTRACE=hex[,hex...] lists
         * bundle IPs to log when executed (helper_ia64_ip_trace). */
        static bool tgt_read;
        static uint64_t tgt[8];
        static int ntgt;

        if (!tgt_read) {
            const char *e = getenv("IA64_IPTRACE");

            tgt_read = true;
            while (e && *e && ntgt < 8) {
                tgt[ntgt++] = strtoull(e, (char **)&e, 16);
                if (*e == ',') {
                    e++;
                }
            }
        }
        for (int i = 0; i < ntgt; i++) {
            if (bundle_ip == tgt[i]) {
                tcg_gen_movi_i64(cpu_ip, bundle_ip);
                gen_helper_ia64_ip_trace(tcg_env);
                break;
            }
        }
    }

    {
        /* Region-gated ring trace (debug).  IA64_TRACE="lo:hi" records a full
         * register snapshot for every bundle in [lo,hi); IA64_TRACE_TRIG=<hex>
         * dumps the ring.  See helper_ia64_trace_rec/dump. */
        static bool trace_read;
        static uint64_t trace_lo, trace_hi, trace_trig;
        static uint64_t trace_xlo, trace_xhi, trace_xlo2, trace_xhi2;

        if (!trace_read) {
            const char *e = getenv("IA64_TRACE");
            const char *colon = e ? strchr(e, ':') : NULL;
            const char *t = getenv("IA64_TRACE_TRIG");
            const char *x = getenv("IA64_TRACE_EXCL");
            const char *xcolon = x ? strchr(x, ':') : NULL;
            const char *x2 = getenv("IA64_TRACE_EXCL2");
            const char *x2colon = x2 ? strchr(x2, ':') : NULL;

            if (colon) {
                trace_lo = strtoull(e, NULL, 16);
                trace_hi = strtoull(colon + 1, NULL, 16);
            }
            if (t) {
                trace_trig = strtoull(t, NULL, 16);
            }
            if (xcolon) {
                trace_xlo = strtoull(x, NULL, 16);
                trace_xhi = strtoull(xcolon + 1, NULL, 16);
            }
            if (x2colon) {
                trace_xlo2 = strtoull(x2, NULL, 16);
                trace_xhi2 = strtoull(x2colon + 1, NULL, 16);
            }
            trace_read = true;
        }
        if (trace_hi > trace_lo &&
            bundle_ip >= trace_lo && bundle_ip < trace_hi &&
            !(trace_xhi > trace_xlo &&
              bundle_ip >= trace_xlo && bundle_ip < trace_xhi) &&
            !(trace_xhi2 > trace_xlo2 &&
              bundle_ip >= trace_xlo2 && bundle_ip < trace_xhi2)) {
            tcg_gen_movi_i64(cpu_ip, bundle_ip);
            gen_helper_ia64_trace_rec(tcg_env);
        }
        if (trace_trig && bundle_ip == trace_trig) {
            tcg_gen_movi_i64(cpu_ip, bundle_ip);
            gen_helper_ia64_trace_dump(tcg_env);
        }
    }

    low = translator_ldq_end(ctx->env, db, bundle_ip, MO_LE);
    high = translator_ldq_end(ctx->env, db, bundle_ip + 8, MO_LE);
    template_code = ia64_bundle_template_code(low);
    template_info = ia64_template_info(template_code);

    ctx->restart.exit_after_bundle = false;
    db->pc_next = bundle_ip + 16;

    if (!template_info->defined) {
        ia64_gen_raise_exception(IA64_EXCP_RESERVED_TEMPLATE, bundle_ip,
                                  template_code, 0);
        db->is_jmp = DISAS_NORETURN;
        return;
    }
    /*
     * Only an rfi with IPSR.ri = 2 enters an MLX bundle at slot 2: Illegal
     * Operation, with IPSR.ri and ISR.ei 2 (SDM Vol. 2 p. 2:192).
     */
    if (ctx->restart.start_slot == 2 &&
        template_info->units[1] == IA64_UNIT_L) {
        ia64_gen_raise_exception(IA64_EXCP_ILLEGAL, bundle_ip, 0, 2);
        db->is_jmp = DISAS_NORETURN;
        return;
    }

    slots[0] = ia64_bundle_slot(low, high, 0);
    slots[1] = ia64_bundle_slot(low, high, 1);
    slots[2] = ia64_bundle_slot(low, high, 2);
    ia64_prepare_self_counted_loop(ctx, template_code, template_info, slots,
                                   bundle_ip);

    skip_x_slot = false;
    record_iipa = true;
    psr_ic_modified = false;
    ctx->restart.psr_ic_modified = false;
    ctx->restart.psr_ac_modified = false;
    for (slot = ctx->restart.start_slot; slot < 3; ++slot) {
        if (skip_x_slot && slot == 2) {
            continue;
        }

        Ia64Instruction insn = ia64_decode_insn(
            template_info->units[slot], slots[slot], bundle_ip,
            slot);
        bool stop_after;
        bool track_iipa_for_insn;
        bool ri_synced;

        ia64_apply_mlx_long_fixup(template_code, slots, slot, &insn,
                                  &skip_x_slot);
        insn.ctx = ctx;
        stop_after = template_info->stop_after[
            skip_x_slot && slot == 1 ? 2 : slot];
        insn.placement_illegal =
            (ia64_insn_must_start_group(&insn) &&
             !ctx->restart.instruction_group_start) ||
            (ia64_insn_must_end_group(&insn) && !stop_after) ||
            (ia64_insn_requires_slot2(&insn) && slot != 2) ||
            ia64_insn_has_invalid_fp_pair(&insn) ||
            ia64_insn_has_illegal_register(&insn);
        insn.reserved_field = ia64_insn_has_reserved_mask_field(&insn);
        ctx->restart.next_instruction_group_start = stop_after;
        if (ia64_insn_may_set_psr_ic(&insn)) {
            ctx->restart.track_iipa = true;
        }
        if (ia64_insn_may_modify_psr_ic(&insn)) {
            psr_ic_modified = true;
            ctx->restart.psr_ic_modified = true;
        }
        if (ia64_insn_may_modify_psr_ac(&insn)) {
            ctx->restart.psr_ac_modified = true;
        }
        track_iipa_for_insn = ctx->restart.track_iipa;
        if (ia64_insn_is_empty_hint(&insn) &&
            !ia64_insn_is_yielding_pause(ctx, &insn) &&
            !(record_iipa && track_iipa_for_insn) &&
            !ctx->restart.track_psr_suppression &&
            !ctx->psr_ss && !ctx->psr_tb) {
            ia64_gen_advance_restart_point(ctx, bundle_ip, slot,
                                           skip_x_slot);
            ctx->restart.instruction_group_start =
                ctx->restart.next_instruction_group_start;
            continue;
        }
        if (ctx->restart.track_psr_suppression) {
            TCGv_i64 suppression = tcg_temp_new_i64();

            tcg_gen_andi_i64(suppression, cpu_psr,
                             IA64_PSR_FAULT_SUPPRESS_MASK);
            tcg_gen_st_i64(suppression, tcg_env,
                           offsetof(CPUIA64State,
                                    exception_state.
                                    psr_suppression_before_insn));
        }
        ctx->restart.logical_ri = slot;
        ri_synced = ia64_insn_observes_ri(ctx, &insn);
        if (ri_synced) {
            ia64_gen_sync_ri(ctx);
        }
        ctx->trap_slot = slot;
        if (ctx->psr_ss) {
            gen_helper_completion_trap_arm(tcg_env,
                                           tcg_constant_i64(bundle_ip),
                                           tcg_constant_i32(slot));
        }
        ia64_set_exit_nat_known(ctx, &insn);
        if (ia64_gen_insn(ctx, &insn, record_iipa && track_iipa_for_insn)) {
            db->is_jmp = DISAS_NORETURN;
            return;
        }
        ia64_gen_advance_restart_point(ctx, bundle_ip, slot, skip_x_slot);
        ia64_update_nat_known(ctx, &insn);
        ia64_update_frame_tracking(ctx, &insn);
        ctx->restart.instruction_group_start =
            ctx->restart.next_instruction_group_start;
        if (ia64_insn_may_modify_psr_ri(&insn)) {
            ctx->restart.current_ri_known = false;
        } else if (ri_synced) {
            /*
             * Code that stored another slot (a self-loop back edge) left the
             * TB there; the path that goes on still holds the synced slot.
             */
            ctx->restart.current_ri = slot;
            ctx->restart.current_ri_known = true;
        }
        if (track_iipa_for_insn && !psr_ic_modified) {
            record_iipa = false;
        }
        ctx->restart.track_psr_suppression =
            ia64_insn_may_set_fault_suppression(&insn);
        if (ctx->psr_ss || ctx->psr_tb) {
            /*
             * Leave after one instruction so that its traps are taken
             * before the next one starts.
             */
            ia64_gen_store_instruction_group_start(
                ctx->restart.instruction_group_start);
            ia64_gen_sync_ri_for_exit(ctx);
            ia64_gen_save_fault_slot_for_exit(ctx);
            tcg_gen_exit_tb(NULL, 0);
            db->is_jmp = DISAS_NORETURN;
            return;
        }
    }

    ctx->restart.start_slot = 0;
    /* Preserve translator_io_start() requests for timer register accesses. */
    if (ctx->restart.exit_after_bundle) {
        db->is_jmp = IA64_DISAS_EXIT;
    } else if (!translator_is_same_page(db, db->pc_next)) {
        /*
         * Bundles are 16-byte aligned and cannot straddle a 4 KiB target
         * page.  Do not fetch the next page until all bundles from this page
         * have executed; a translation fault there must not discard them.
         */
        db->is_jmp = DISAS_TOO_MANY;
    }
}

void ia64_gen_goto_tb_group(DisasContext *ctx, uint64_t dest,
                            bool group_start)
{
    uint8_t slot = ctx->branch.goto_tb_slots;

    ia64_gen_store_instruction_group_start(group_start);
    ia64_gen_save_fault_slot_for_exit(ctx);
    ia64_gen_clear_ri();
    tcg_gen_movi_i64(cpu_ip, dest);
    if (slot < 2 && translator_use_goto_tb(&ctx->base, dest)) {
        uint64_t test0 = ~ctx->memory.nat_known_at_exit[0];
        uint64_t test1 = ~ctx->memory.nat_known_at_exit[1];
        TCGLabel *unlinked = NULL;

        /*
         * A goto_tb link does not look at the flags again: take it only
         * with no GR NaT set, so the TB it reaches may trust
         * IA64_TB_FLAG_NAT_CLEAR.
         */
        if ((test0 | test1) != 0) {
            unlinked = gen_new_label();
            ia64_gen_brcond_nat_set(test0, test1, unlinked);
        }
        ctx->branch.goto_tb_slots = slot + 1;
        tcg_gen_goto_tb(slot);
        tcg_gen_exit_tb(ctx->base.tb, slot);
        if (unlinked != NULL) {
            gen_set_label(unlinked);
            tcg_gen_lookup_and_goto_ptr();
        }
    } else {
        tcg_gen_lookup_and_goto_ptr();
    }
}

static void ia64_gen_goto_tb(DisasContext *ctx, uint64_t dest)
{
    ia64_gen_goto_tb_group(ctx, dest, ctx->restart.instruction_group_start);
}

static void ia64_tr_tb_stop(DisasContextBase *db, CPUState *cs)
{
    DisasContext *ctx = container_of(db, DisasContext, base);

    switch (db->is_jmp) {
    case IA64_DISAS_EXIT:
    case DISAS_TOO_MANY:
        ctx->memory.nat_known_at_exit[0] = ctx->memory.nat_known_clear[0];
        ctx->memory.nat_known_at_exit[1] = ctx->memory.nat_known_clear[1];
        ia64_gen_goto_tb(ctx, db->pc_next);
        break;
    case DISAS_NORETURN:
        break;
    default:
        g_assert_not_reached();
    }
    ia64_nat_ctx = NULL;
}

static const char *ia64_unit_log_name(IA64SlotUnit unit)
{
    switch (unit) {
    case IA64_UNIT_M:
        return "M";
    case IA64_UNIT_I:
        return "I";
    case IA64_UNIT_B:
        return "B";
    case IA64_UNIT_F:
        return "F";
    case IA64_UNIT_L:
        return "L";
    case IA64_UNIT_X:
        return "X";
    case IA64_UNIT_RESERVED:
    default:
        return "reserved";
    }
}

static bool ia64_tr_disas_log(const DisasContextBase *db, CPUState *cs,
                              FILE *logfile)
{
    const DisasContext *ctx = container_of(db, DisasContext, base);
    vaddr bundle_ip = db->pc_first;
    size_t size = translator_st_len(db);
    int start_slot = ctx->restart.start_slot;

    fprintf(logfile, "QEMU-IA64-DECODE: pc=0x%016" PRIx64
            " size=%zu\n", (uint64_t)db->pc_first, size);

    while (size >= 16) {
        uint8_t bundle[16];
        uint64_t low;
        uint64_t high;
        uint8_t template_code;
        const IA64TemplateInfo *template_info;
        uint64_t slots[3];
        bool skip_x_slot = false;
        int slot;

        if (!translator_st(db, bundle, bundle_ip, sizeof(bundle))) {
            fprintf(logfile,
                    "QEMU-IA64-DECODE: read-failed pc=0x%016" PRIx64 "\n",
                    (uint64_t)bundle_ip);
            break;
        }

        low = ldq_le_p(bundle);
        high = ldq_le_p(bundle + 8);
        template_code = ia64_bundle_template_code(low);
        template_info = ia64_template_info(template_code);
        slots[0] = ia64_bundle_slot(low, high, 0);
        slots[1] = ia64_bundle_slot(low, high, 1);
        slots[2] = ia64_bundle_slot(low, high, 2);

        fprintf(logfile,
                "QEMU-IA64-DECODE: bundle=0x%016" PRIx64
                " template=0x%02x name=%s defined=%u\n",
                (uint64_t)bundle_ip, template_code, template_info->name,
                template_info->defined ? 1 : 0);

        if (template_info->defined) {
            for (slot = start_slot; slot < 3; ++slot) {
                IA64SlotUnit unit;
                Ia64Instruction insn;

                if (skip_x_slot && slot == 2) {
                    continue;
                }

                unit = template_info->units[slot];
                insn = ia64_decode_insn(unit, slots[slot], bundle_ip, slot);
                ia64_apply_mlx_long_fixup(template_code, slots, slot, &insn,
                                          &skip_x_slot);
                fprintf(logfile,
                        "QEMU-IA64-DECODE: slot=%d unit=%s opcode=%u"
                        " valid=%u qp=%u raw=0x%010" PRIx64 "\n",
                        slot, ia64_unit_log_name(insn.unit),
                        (unsigned)insn.opcode, insn.valid ? 1 : 0,
                        insn.qp, insn.raw);
            }
        }

        bundle_ip += 16;
        size -= 16;
        start_slot = 0;
    }

    return false;
}

void ia64_translate_init(void)
{
    static const char * const gr_names[IA64_GR_COUNT] = {
        "r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7",
        "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15",
        "r16", "r17", "r18", "r19", "r20", "r21", "r22", "r23",
        "r24", "r25", "r26", "r27", "r28", "r29", "r30", "r31",
        "r32", "r33", "r34", "r35", "r36", "r37", "r38", "r39",
        "r40", "r41", "r42", "r43", "r44", "r45", "r46", "r47",
        "r48", "r49", "r50", "r51", "r52", "r53", "r54", "r55",
        "r56", "r57", "r58", "r59", "r60", "r61", "r62", "r63",
        "r64", "r65", "r66", "r67", "r68", "r69", "r70", "r71",
        "r72", "r73", "r74", "r75", "r76", "r77", "r78", "r79",
        "r80", "r81", "r82", "r83", "r84", "r85", "r86", "r87",
        "r88", "r89", "r90", "r91", "r92", "r93", "r94", "r95",
        "r96", "r97", "r98", "r99", "r100", "r101", "r102", "r103",
        "r104", "r105", "r106", "r107", "r108", "r109", "r110",
        "r111", "r112", "r113", "r114", "r115", "r116", "r117",
        "r118", "r119", "r120", "r121", "r122", "r123", "r124",
        "r125", "r126", "r127",
    };
    static const char * const pr_names[IA64_PR_COUNT] = {
        "p0", "p1", "p2", "p3", "p4", "p5", "p6", "p7",
        "p8", "p9", "p10", "p11", "p12", "p13", "p14", "p15",
        "p16", "p17", "p18", "p19", "p20", "p21", "p22", "p23",
        "p24", "p25", "p26", "p27", "p28", "p29", "p30", "p31",
        "p32", "p33", "p34", "p35", "p36", "p37", "p38", "p39",
        "p40", "p41", "p42", "p43", "p44", "p45", "p46", "p47",
        "p48", "p49", "p50", "p51", "p52", "p53", "p54", "p55",
        "p56", "p57", "p58", "p59", "p60", "p61", "p62", "p63",
    };
    static const char * const br_names[IA64_BR_COUNT] = {
        "b0", "b1", "b2", "b3", "b4", "b5", "b6", "b7",
    };
    int i;

    for (i = 0; i < IA64_GR_COUNT; ++i) {
        cpu_gr[i] = tcg_global_mem_new_i64(tcg_env,
                                           offsetof(CPUIA64State, gr[i]),
                                           gr_names[i]);
    }
    for (i = 0; i < IA64_PR_COUNT; ++i) {
        cpu_pr[i] = tcg_global_mem_new_i64(tcg_env,
                                           offsetof(CPUIA64State, pr[i]),
                                           pr_names[i]);
    }
    for (i = 0; i < IA64_BR_COUNT; ++i) {
        cpu_br[i] = tcg_global_mem_new_i64(tcg_env,
                                           offsetof(CPUIA64State, br[i]),
                                           br_names[i]);
    }
    cpu_ip = tcg_global_mem_new_i64(tcg_env, offsetof(CPUIA64State, ip), "ip");
    cpu_psr = tcg_global_mem_new_i64(tcg_env, offsetof(CPUIA64State, psr),
                                     "psr");
    for (i = 0; i < IA64_FR_COUNT; ++i) {
        cpu_fr[i] = tcg_global_mem_new_i64(tcg_env,
                                           offsetof(CPUIA64State, fp.fr[i]),
                                           "fr");
    }
    for (i = 0; i < 2; ++i) {
        cpu_nat[i] = tcg_global_mem_new_i64(tcg_env,
                                           offsetof(CPUIA64State, nat[i]),
                                           "nat");
        cpu_fr_nat[i] = tcg_global_mem_new_i64(tcg_env,
                                               offsetof(CPUIA64State,
                                                        fp.fr_nat[i]),
                                               "fr_nat");
        cpu_fr_sig[i] = tcg_global_mem_new_i64(tcg_env,
                                               offsetof(CPUIA64State,
                                                        fp.fr_sig[i]),
                                               "fr_sig");
        cpu_fr_ext_valid[i] = tcg_global_mem_new_i64(
            tcg_env, offsetof(CPUIA64State, fp.fr_ext_valid[i]),
            "fr_ext_valid");
        cpu_fr_int_origin[i] = tcg_global_mem_new_i64(
            tcg_env, offsetof(CPUIA64State, fp.fr_int_origin[i]),
            "fr_int_origin");
        cpu_rse_gr_dirty[i] = tcg_global_mem_new_i64(
            tcg_env, offsetof(CPUIA64State, rse.rse_gr_dirty[i]),
            "rse_gr_dirty");
    }

    ia64_ia32_translate_init();
}

void ia64_translate_code(CPUState *cs, TranslationBlock *tb,
                                    int *max_insns, vaddr pc, void *host_pc)
{
    DisasContext ctx = {};
    static const TranslatorOps ia64_tr_ops = {
        .init_disas_context = ia64_tr_init_disas_context,
        .tb_start = ia64_tr_tb_start,
        .insn_start = ia64_tr_insn_start,
        .translate_insn = ia64_tr_translate_insn,
        .tb_stop = ia64_tr_tb_stop,
        .disas_log = ia64_tr_disas_log,
    };

    if (tb->flags & IA64_TB_FLAG_PSR_IS) {
        /*
         * The IA-32 execution engine reuses the i386 translator and must obey
         * the x86 memory model (x86-TSO: every ordering except store->load).
         * Match target/i386's guest_default_memory_order so WOW64 code keeps
         * the store-buffer ordering it relies on.
         */
        tcg_ctx->guest_mo = TCG_MO_ALL & ~TCG_MO_ST_LD;
        ia64_ia32_translate_code(cs, tb, max_insns, pc, host_pc);
    } else {
        /*
         * IA-64 is weakly ordered (SDM Vol.2 §2.5): ordinary loads and stores
         * carry no ordering guarantee; software must use ld.acq/st.rel/mf/sync
         * where it needs one, and the translator already emits the matching
         * TCG barriers for those (IA64_TCG_MO_ACQUIRE/RELEASE and mf).  The
         * inherited TCG_MO_ALL default instead declared the guest sequentially
         * consistent, so tcg_gen_req_mo() emitted a barrier -- a locked host
         * instruction on x86, a dmb on ARM -- before *every* guest access.
         * Set the weak order so only the architected barriers cost anything.
         */
        tcg_ctx->guest_mo = 0;
        translator_loop(cs, tb, max_insns, pc, host_pc, &ia64_tr_ops,
                        &ctx.base, TCG_TYPE_VA);
    }
}
