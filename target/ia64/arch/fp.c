/*
 * IA-64 floating-point register model, arithmetic, transaction rollback,
 * FPSWA, and floating-point memory operations.
 *
 * IA64FPState is authoritative: binary64 is only the fast representation;
 * extended, integer-origin, NaTVal, and transaction metadata preserve the
 * architected register format and precise-fault rollback state.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/atomic.h"
#include "qemu/atomic128.h"
#include "cpu.h"
#include "arch/arch.h"
#include "arch/fp.h"
#include "arch/fp-arith.h"
#include "exec-access.h"
#include "fpreg.h"
#include "exec/cpu-common.h"
#include "fpu/softfloat.h"
#include "trace.h"

#define IA64_FP_WRE_BIAS          0x0ffff
#define IA64_FP_WRE_EXP_MASK      0x1ffff
#define IA64_FP_SINGLE_BIAS       127
#define IA64_FP_SINGLE_MANT_WIDTH 24
#define IA64_FP_ISR_SWA           0x8ULL
#define IA64_FPSR_TRAPS_MASK      0x3fULL
#define IA64_FPSR_SF_BITS         13
#define IA64_FPSR_SF0_SHIFT       6
#define IA64_FPSR_SF_CONTROLS_MASK 0x7fULL
#define IA64_FPSR_SF_FLAGS_MASK   0x3fULL
#define IA64_FPSR_SF_FLAGS_SHIFT  7
#define IA64_FPSR_SF_TD           (1U << 6)
#define IA64_ROTATING_FR_BASE     32
#define IA64_ROTATING_FR_COUNT    (IA64_FR_COUNT - IA64_ROTATING_FR_BASE)

static inline void ia64_fp_backup_bitmap_bit(uint64_t backup[2],
                                             const uint64_t value[2],
                                             uint32_t reg)
{
    uint64_t bit = 1ULL << (reg % 64);
    uint32_t word = reg / 64;

    backup[word] = (backup[word] & ~bit) | (value[word] & bit);
}

static inline void ia64_fp_backup_fr(CPUIA64State *env, uint32_t reg)
{
    uint64_t bit;
    uint32_t word;

    if (!env->fp.transaction.active || reg <= 1) {
        return;
    }
    word = reg / 64;
    bit = 1ULL << (reg % 64);
    if (env->fp.transaction.backup_fr_mask[word] & bit) {
        return;
    }

    env->fp.transaction.backup_fr[reg] = env->fp.fr[reg];
    env->fp.transaction.backup_fr_ext_mant[reg] = env->fp.fr_ext_mant[reg];
    env->fp.transaction.backup_fr_ext_exp[reg] = env->fp.fr_ext_exp[reg];
    env->fp.transaction.backup_fr_int_value[reg] = env->fp.fr_int_value[reg];
    ia64_fp_backup_bitmap_bit(env->fp.transaction.backup_fr_nat,
                              env->fp.fr_nat, reg);
    ia64_fp_backup_bitmap_bit(env->fp.transaction.backup_fr_sig,
                              env->fp.fr_sig, reg);
    ia64_fp_backup_bitmap_bit(env->fp.transaction.backup_fr_ext_sign,
                              env->fp.fr_ext_sign, reg);
    ia64_fp_backup_bitmap_bit(env->fp.transaction.backup_fr_ext_valid,
                              env->fp.fr_ext_valid, reg);
    ia64_fp_backup_bitmap_bit(env->fp.transaction.backup_fr_int_origin,
                              env->fp.fr_int_origin, reg);
    env->fp.transaction.backup_fr_mask[word] |= bit;
}

static inline void ia64_fp_backup_pr(CPUIA64State *env, uint32_t reg)
{
    uint64_t bit;

    if (!env->fp.transaction.active || reg == IA64_FR_ZERO_INDEX) {
        return;
    }
    bit = 1ULL << reg;
    if (!(env->fp.transaction.backup_pr_mask & bit)) {
        env->fp.transaction.backup_pr[reg] = env->pr[reg];
        env->fp.transaction.backup_pr_mask |= bit;
    }
}

static inline void ia64_fr_write(CPUIA64State *env, uint32_t reg,
                                 uint64_t value)
{
    ia64_fp_backup_fr(env, reg);
    ia64_fpreg_from_binary64(env, reg, value);
}

static inline void ia64_fr_write_sig(CPUIA64State *env, uint32_t reg,
                                     uint64_t value)
{
    ia64_fp_backup_fr(env, reg);
    ia64_fpreg_from_spill(env, reg, value, IA64_FP_REG_INTEGER_EXP);
}

static void ia64_fr_write_ext(CPUIA64State *env, uint32_t reg, bool sign,
                              uint32_t exp, uint64_t mant)
{
    ia64_fp_backup_fr(env, reg);
    ia64_fpreg_set(env, reg, sign, exp & 0x1ffff, mant);
}

static bool ia64_fr_ext_get(const CPUIA64State *env, uint32_t reg,
                            bool *sign, uint32_t *exp, uint64_t *mant)
{
    return ia64_fpreg_get_extended(env, reg, sign, exp, mant);
}

static bool ia64_fr_int_origin_get(const CPUIA64State *env, uint32_t reg)
{
    return reg > 1 &&
           ((env->fp.fr_int_origin[reg / 64] >> (reg % 64)) & 1);
}

static inline bool ia64_fr_nat_get(const CPUIA64State *env, uint32_t reg)
{
    return ia64_fpreg_is_nat(env, reg);
}

static inline bool ia64_fr_sig_get(const CPUIA64State *env, uint32_t reg)
{
    return ia64_fpreg_is_integer(env, reg);
}

static inline void ia64_fr_write_nat(CPUIA64State *env, uint32_t reg)
{
    ia64_fp_backup_fr(env, reg);
    ia64_fpreg_from_spill(env, reg, 0, IA64_FP_REG_NATVAL_EXP);
}
static void ia64_raise_fp_fault(CPUIA64State *env, uint64_t isr);
static uint64_t ia64_fp_active_traps(CPUIA64State *env, uint32_t sf);
static uint64_t ia64_fp_soft_flags_to_ia64(int soft);
static uint32_t ia64_fpsr_sf_shift(uint32_t sf);
static uint64_t ia64_fpsr_sf_controls(const CPUIA64State *env, uint32_t sf);
static void ia64_fp_simd_fault_end(CPUIA64State *env, uint32_t sf,
                                   int hi_soft, int lo_soft);
static void ia64_fp_simd_end(CPUIA64State *env, uint32_t sf,
                             uint32_t hi_flags, uint32_t lo_flags,
                             uint32_t hi_trap, uint32_t lo_trap);

static floatx80 ia64_register_format_to_floatx80(CPUIA64State *env,
                                                  bool sign, uint32_t exp,
                                                  uint64_t mant)
{
    int shift;
    int32_t adjusted_exp;

    if (exp == IA64_FP_REG_SPECIAL_EXP) {
        return ia64_make_floatx80(((uint16_t)sign << 15) | 0x7fff, mant);
    }
    if (mant == 0) {
        return ia64_make_floatx80((uint16_t)sign << 15, 0);
    }
    if (exp == 0) {
        return ia64_make_floatx80((uint16_t)sign << 15, mant);
    }

    /*
     * setf.sig architecturally produces a positive integer significand
     * with exponent 0x1003e.  Normalize the raw dword before handing it
     * to SoftFloat's x87-style extended format.  The same normalization
     * is required for ldf.fill/ldfe values whose register significand
     * is architecturally unnormalized.
     */
    if (mant & IA64_FP_SIGNIFICAND_INTEGER_BIT) {
        adjusted_exp = exp;
    } else {
        shift = clz64(mant);
        adjusted_exp = (int32_t)exp - shift;
        mant <<= shift;
    }

    if (adjusted_exp > 0xc000 && adjusted_exp - 0xc000 < 0x7fff) {
        return ia64_make_floatx80(((uint16_t)sign << 15) |
                                  (uint16_t)(adjusted_exp - 0xc000), mant);
    }

    /*
     * SoftFloat's floatx80 exponent is narrower than IA-64's 17-bit
     * wide-range exponent.  Preserve such values losslessly in the register
     * file, but report the narrowing when an arithmetic operation consumes
     * one.
     */
    if (adjusted_exp <= 0xc000) {
        float_raise(float_flag_underflow | float_flag_inexact,
                    &env->fp.fp_status);
        return ia64_make_floatx80((uint16_t)sign << 15, 0);
    }
    float_raise(float_flag_overflow | float_flag_inexact, &env->fp.fp_status);
    return floatx80_default_inf(sign, &env->fp.fp_status);
}

static void ia64_fr_copy(CPUIA64State *env, uint32_t dst, uint32_t src,
                         int sign_mode)
{
    bool sign;
    uint32_t exp;
    uint64_t mant;
    uint64_t value;

    if (dst <= 1) {
        return;
    }
    if (ia64_fr_nat_get(env, src)) {
        ia64_fr_write_nat(env, dst);
        return;
    }
    if (ia64_fr_sig_get(env, src)) {
        value = src == IA64_FR_ZERO_INDEX ? 0 :
                src == IA64_FR_ONE_INDEX ? IA64_FR_ONE : env->fp.fr[src];
        if (sign_mode == 0 || sign_mode == 1) {
            ia64_fr_write_sig(env, dst, value);
        } else if (sign_mode < 0 || sign_mode == 2) {
            ia64_fr_write_ext(env, dst, true, IA64_FP_REG_INTEGER_EXP, value);
        } else {
            ia64_fr_write_sig(env, dst, value);
        }
        return;
    }
    if (ia64_fr_ext_get(env, src, &sign, &exp, &mant)) {
        if (sign_mode == 0) {
            sign = false;
        } else if (sign_mode < 0) {
            sign = !sign;
        } else if (sign_mode == 2) {
            sign = true;
        }
        ia64_fr_write_ext(env, dst, sign, exp, mant);
        if (ia64_fr_int_origin_get(env, src)) {
            env->fp.fr_int_value[dst] = env->fp.fr_int_value[src];
            env->fp.fr_int_origin[dst / 64] |= 1ULL << (dst % 64);
        }
        return;
    }

    value = src == IA64_FR_ZERO_INDEX ? 0 :
            src == IA64_FR_ONE_INDEX ? IA64_FR_ONE : env->fp.fr[src];
    if (sign_mode == 0) {
        value &= INT64_MAX;
    } else if (sign_mode < 0) {
        value ^= 1ULL << 63;
    } else if (sign_mode == 2) {
        value |= 1ULL << 63;
    }
    ia64_fr_write(env, dst, value);
}

static void ia64_pr_write(CPUIA64State *env, uint32_t reg, bool value)
{
    if (reg != IA64_FR_ZERO_INDEX) {
        ia64_fp_backup_pr(env, reg);
        env->pr[reg] = value ? 1 : 0;
    }
    env->pr[IA64_PR_TRUE] = 1;
}

static bool ia64_fr_nat_any2(const CPUIA64State *env, uint32_t reg1,
                             uint32_t reg2)
{
    return ia64_fr_nat_get(env, reg1) || ia64_fr_nat_get(env, reg2);
}

static bool ia64_fr_nat_any3(const CPUIA64State *env, uint32_t reg1,
                             uint32_t reg2, uint32_t reg3)
{
    return ia64_fr_nat_any2(env, reg1, reg2) ||
           ia64_fr_nat_get(env, reg3);
}

static bool ia64_fr_write_nat_if_any2(CPUIA64State *env, uint32_t dst,
                                      uint32_t src1, uint32_t src2)
{
    if (!ia64_fr_nat_any2(env, src1, src2)) {
        return false;
    }
    ia64_fr_write_nat(env, dst);
    return true;
}

static bool ia64_fr_write_nat_if_any3(CPUIA64State *env, uint32_t dst,
                                      uint32_t src1, uint32_t src2,
                                      uint32_t src3)
{
    if (!ia64_fr_nat_any3(env, src1, src2, src3)) {
        return false;
    }
    ia64_fr_write_nat(env, dst);
    return true;
}


static void ia64_rebase_rotating_fr_u64(uint64_t values[IA64_FR_COUNT],
                                        uint32_t shift)
{
    uint64_t tmp[IA64_ROTATING_FR_COUNT];
    uint32_t i;

    if (shift == 1) {
        uint64_t first = values[IA64_ROTATING_FR_BASE];

        memmove(&values[IA64_ROTATING_FR_BASE],
                &values[IA64_ROTATING_FR_BASE + 1],
                (IA64_ROTATING_FR_COUNT - 1) * sizeof(values[0]));
        values[IA64_FR_COUNT - 1] = first;
        return;
    }
    if (shift == IA64_ROTATING_FR_COUNT - 1) {
        uint64_t last = values[IA64_FR_COUNT - 1];

        memmove(&values[IA64_ROTATING_FR_BASE + 1],
                &values[IA64_ROTATING_FR_BASE],
                (IA64_ROTATING_FR_COUNT - 1) * sizeof(values[0]));
        values[IA64_ROTATING_FR_BASE] = last;
        return;
    }

    for (i = 0; i < IA64_ROTATING_FR_COUNT; i++) {
        uint32_t src = (i + shift) % IA64_ROTATING_FR_COUNT;

        tmp[i] = values[IA64_ROTATING_FR_BASE + src];
    }
    memcpy(&values[IA64_ROTATING_FR_BASE], tmp, sizeof(tmp));
}

static void ia64_rebase_rotating_fr_u32(uint32_t values[IA64_FR_COUNT],
                                        uint32_t shift)
{
    uint32_t tmp[IA64_ROTATING_FR_COUNT];
    uint32_t i;

    if (shift == 1) {
        uint32_t first = values[IA64_ROTATING_FR_BASE];

        memmove(&values[IA64_ROTATING_FR_BASE],
                &values[IA64_ROTATING_FR_BASE + 1],
                (IA64_ROTATING_FR_COUNT - 1) * sizeof(values[0]));
        values[IA64_FR_COUNT - 1] = first;
        return;
    }
    if (shift == IA64_ROTATING_FR_COUNT - 1) {
        uint32_t last = values[IA64_FR_COUNT - 1];

        memmove(&values[IA64_ROTATING_FR_BASE + 1],
                &values[IA64_ROTATING_FR_BASE],
                (IA64_ROTATING_FR_COUNT - 1) * sizeof(values[0]));
        values[IA64_ROTATING_FR_BASE] = last;
        return;
    }

    for (i = 0; i < IA64_ROTATING_FR_COUNT; i++) {
        uint32_t src = (i + shift) % IA64_ROTATING_FR_COUNT;

        tmp[i] = values[IA64_ROTATING_FR_BASE + src];
    }
    memcpy(&values[IA64_ROTATING_FR_BASE], tmp, sizeof(tmp));
}

static void ia64_rebase_rotating_fr_bits(uint64_t bits[2], uint32_t shift)
{
    const uint64_t low_nonrotating = bits[0] & UINT32_MAX;
    const __uint128_t mask = (((__uint128_t)1 << 96) - 1);
    __uint128_t rotating = ((__uint128_t)bits[1] << 32) | (bits[0] >> 32);

    rotating = ((rotating >> shift) |
                (rotating << (IA64_ROTATING_FR_COUNT - shift))) & mask;
    bits[0] = low_nonrotating | (uint64_t)(rotating << 32);
    bits[1] = rotating >> 32;
}

static G_GNUC_NO_INLINE void
ia64_rebase_rotating_fr(CPUIA64State *env, uint32_t shift)
{
    const uint64_t rotating_word0_mask = UINT64_MAX << IA64_ROTATING_FR_BASE;
    bool has_ext = (env->fp.fr_ext_valid[0] & rotating_word0_mask) ||
                   env->fp.fr_ext_valid[1];
    bool has_int = (env->fp.fr_int_origin[0] & rotating_word0_mask) ||
                   env->fp.fr_int_origin[1];

    ia64_rebase_rotating_fr_u64(env->fp.fr, shift);
    if (has_int) {
        ia64_rebase_rotating_fr_u64(env->fp.fr_int_value, shift);
    }
    if (has_ext) {
        ia64_rebase_rotating_fr_u64(env->fp.fr_ext_mant, shift);
        ia64_rebase_rotating_fr_u32(env->fp.fr_ext_exp, shift);
    }
    ia64_rebase_rotating_fr_bits(env->fp.fr_nat, shift);
    ia64_rebase_rotating_fr_bits(env->fp.fr_sig, shift);
    ia64_rebase_rotating_fr_bits(env->fp.fr_ext_sign, shift);
    ia64_rebase_rotating_fr_bits(env->fp.fr_ext_valid, shift);
    ia64_rebase_rotating_fr_bits(env->fp.fr_int_origin, shift);
    env->fp.fr_sig[0] &= ~(env->fp.fr_nat[0] & rotating_word0_mask);
    env->fp.fr_sig[1] &= ~env->fp.fr_nat[1];

    ia64_invalidate_rotating_fp_alat(env);
}

static G_GNUC_NO_INLINE uint32_t ia64_normalize_rrb_fr(uint32_t rrb)
{
    return rrb % IA64_ROTATING_FR_COUNT;
}

static G_GNUC_NO_INLINE void
ia64_set_cfm_rrb_fr_slow(CPUIA64State *env, uint32_t new_rrb,
                         uint32_t old_rrb)
{
    if (new_rrb >= IA64_ROTATING_FR_COUNT) {
        new_rrb = ia64_normalize_rrb_fr(new_rrb);
    }
    if (old_rrb >= IA64_ROTATING_FR_COUNT) {
        old_rrb = ia64_normalize_rrb_fr(old_rrb);
    }

    if (new_rrb != old_rrb && env->fp.rotating_fr_live) {
        uint32_t shift = new_rrb >= old_rrb ?
                         new_rrb - old_rrb :
                         new_rrb + IA64_ROTATING_FR_COUNT - old_rrb;

        ia64_rebase_rotating_fr(env, shift);
    }
    env->cfm_rrb_fr = new_rrb;
}

/*
 * Floating-point helpers index env->fp.fr[] by the current logical register
 * number.  Rebase every live part of that representation when RRB.FR changes
 * so that cover/call and rfi/return expose the selected physical registers.
 * Most operating-system code never writes a rotating FR; keep that case a
 * small leaf function and leave the array work out of its prologue.
 */
void ia64_set_cfm_rrb_fr(CPUIA64State *env, uint32_t new_rrb)
{
    uint32_t old_rrb = env->cfm_rrb_fr;

    if (likely(new_rrb < IA64_ROTATING_FR_COUNT &&
               old_rrb < IA64_ROTATING_FR_COUNT &&
               (new_rrb == old_rrb || !env->fp.rotating_fr_live))) {
        env->cfm_rrb_fr = new_rrb;
        return;
    }
    ia64_set_cfm_rrb_fr_slow(env, new_rrb, old_rrb);
}

static IA64FPReg ia64_fr_reg(CPUIA64State *env, uint32_t reg)
{
    IA64FPReg value;
    bool sign;
    uint32_t exp;

    ia64_fpreg_get(env, reg, &sign, &exp, &value.sig);
    value.sign = sign;
    value.exp = exp & IA64_FP_WRE_EXP_MASK;
    return value;
}

static void ia64_fr_write_reg(CPUIA64State *env, uint32_t reg,
                              IA64FPReg value)
{
    ia64_fr_write_ext(env, reg, value.sign, value.exp, value.sig);
}

static const IA64FPReg ia64_fp_qnan_indefinite = {
    .sign = true,
    .exp = IA64_FP_EXP_SPECIAL,
    .sig = IA64_FP_INT_BIT | IA64_FP_QUIET_BIT,
};

/*
 * An enabled V, D or Z exception faults before any state changes; a
 * disabled one only sets its flag (SDM Vol 1 5.4.1.2).
 */
static void ia64_fp_raise_flag(CPUIA64State *env, uint32_t sf,
                               uint32_t flag)
{
    if (!(ia64_fp_active_traps(env, sf) & flag)) {
        ia64_raise_fp_fault(env, flag);
    }
    env->fp.transaction.flags |= flag;
}

static void ia64_fp_round_to_reg(CPUIA64State *env, IA64FPReg *result,
                                 const IA64FPExact *exact,
                                 const IA64FPFormat *fmt)
{
    IA64FPRounded rounded;

    ia64_fpa_round(&rounded, exact, fmt);
    result->sign = rounded.sign;
    result->exp = rounded.exp & IA64_FP_WRE_EXP_MASK;
    result->sig = rounded.sig;
    env->fp.transaction.flags |= rounded.flags;
    env->fp.transaction.trap |= rounded.trap;
}

/*
 * fma, fms, fnma and their pseudo-ops: +-(f3 * f4) +- f2, computed exactly
 * and rounded once; f2 = f0 selects the plain IEEE multiply (SDM Vol 3
 * fma, fms, fnma).  The checks follow Vol 1 Figure 5-11.
 */
static void ia64_fp_muladd(CPUIA64State *env, uint32_t r1, uint32_t f3,
                           uint32_t f4, uint32_t f2, bool negate_product,
                           bool negate_addend, uint32_t sf, uint32_t pc)
{
    bool has_addend = f2 != IA64_FR_ZERO_INDEX;
    IA64FPReg a;
    IA64FPReg b;
    IA64FPReg c;
    IA64FPReg result;
    IA64FPExact exact;
    IA64FPFormat fmt;
    bool product_inf;
    bool addend_inf;
    bool product_sign;

    if (ia64_fr_write_nat_if_any3(env, r1, f2, f3, f4)) {
        return;
    }
    a = ia64_fr_reg(env, f3);
    b = ia64_fr_reg(env, f4);
    c = ia64_fr_reg(env, f2);

    if (ia64_fpr_is_unsupported(&a) || ia64_fpr_is_unsupported(&b) ||
        ia64_fpr_is_unsupported(&c)) {
        ia64_fp_raise_flag(env, sf, IA64_FP_FLAG_V);
        ia64_fr_write_reg(env, r1, ia64_fp_qnan_indefinite);
        return;
    }
    if (ia64_fpr_is_nan(&a) || ia64_fpr_is_nan(&b) ||
        ia64_fpr_is_nan(&c)) {
        if (ia64_fpr_is_snan(&a) || ia64_fpr_is_snan(&b) ||
            ia64_fpr_is_snan(&c)) {
            ia64_fp_raise_flag(env, sf, IA64_FP_FLAG_V);
        }
        /* NaN operand priority is f4, f2, f3 (Vol 1 5.4.6). */
        result = ia64_fpr_is_nan(&b) ? b : ia64_fpr_is_nan(&c) ? c : a;
        result.sig |= IA64_FP_QUIET_BIT;
        ia64_fr_write_reg(env, r1, result);
        return;
    }

    product_sign = a.sign ^ b.sign ^ negate_product;
    product_inf = ia64_fpr_is_inf(&a) || ia64_fpr_is_inf(&b);
    addend_inf = ia64_fpr_is_inf(&c);
    /* A pseudo-zero times infinity is an infinity (Vol 1 5.1.3). */
    if ((ia64_fpr_is_inf(&a) && b.sig == 0 && b.exp == 0) ||
        (ia64_fpr_is_inf(&b) && a.sig == 0 && a.exp == 0) ||
        (product_inf && addend_inf &&
         product_sign != (c.sign ^ negate_addend))) {
        ia64_fp_raise_flag(env, sf, IA64_FP_FLAG_V);
        ia64_fr_write_reg(env, r1, ia64_fp_qnan_indefinite);
        return;
    }
    if (ia64_fpr_is_unnormal(&a) || ia64_fpr_is_unnormal(&b) ||
        ia64_fpr_is_unnormal(&c)) {
        ia64_fp_raise_flag(env, sf, IA64_FP_FLAG_D);
    }
    if (product_inf || addend_inf) {
        result.sign = product_inf ? product_sign : c.sign ^ negate_addend;
        result.exp = IA64_FP_EXP_SPECIAL;
        result.sig = IA64_FP_INT_BIT;
        ia64_fr_write_reg(env, r1, result);
        return;
    }

    ia64_fpa_format(&fmt, ia64_fpsr_sf_controls(env, sf), pc,
                    ia64_fp_active_traps(env, sf));
    if (ia64_fpa_muladd(&exact, &a, &b, has_addend ? &c : NULL,
                        negate_product, negate_addend, fmt.rc)) {
        ia64_fp_round_to_reg(env, &result, &exact, &fmt);
    } else {
        result.sign = exact.sign;
        result.exp = 0;
        result.sig = 0;
    }
    ia64_fr_write_reg(env, r1, result);
}

/*
 * FR.significand of the architected register (SDM Vol 1 Figure 5-1).
 * fp.fr[] holds it only for the integer form; for a setf.d or ldfd value it
 * holds the binary64 cache.
 */
static uint64_t ia64_fr_significand(const CPUIA64State *env, uint32_t reg)
{
    uint64_t low;
    uint64_t high;

    if (ia64_fr_sig_get(env, reg)) {
        return env->fp.fr[reg];
    }
    ia64_fpreg_to_spill(env, reg, &low, &high);
    return low;
}

void ia64_fp_xma(CPUIA64State *env, uint32_t r1, uint32_t r2,
                uint32_t r3, uint32_t r4, uint32_t mode)
{
    uint64_t a;
    uint64_t b;
    uint64_t addend;
    __uint128_t result;

    if (ia64_fr_write_nat_if_any3(env, r1, r2, r3, r4)) {
        return;
    }
    a = ia64_fr_significand(env, r3);
    b = ia64_fr_significand(env, r4);
    addend = ia64_fr_significand(env, r2);

    if (mode == 1) {
        result = (__int128)(int64_t)a * (__int128)(int64_t)b + addend;
        ia64_fr_write_sig(env, r1, result >> 64);
    } else {
        result = (__uint128_t)a * b;
        if (mode != 3) {
            result += addend;
        }
        ia64_fr_write_sig(env, r1, mode == 0 ?
                          (uint64_t)result : result >> 64);
    }
}

/*
 * Supported non-NaN operands compare by value in the full register range;
 * an unsupported operand is unordered with V, a NaN unordered with V when
 * it is an SNaN or the relation signals (lt, le), and an unnormal sets D
 * (SDM Vol 3 fcmp, Vol 1 Figure 5-11).  Returns the ordering, or 2 for
 * unordered.
 */
static int ia64_fp_compare_checked(CPUIA64State *env, uint32_t sf,
                                   const IA64FPReg *a, const IA64FPReg *b,
                                   bool quiet, bool magnitude)
{
    if (ia64_fpr_is_unsupported(a) || ia64_fpr_is_unsupported(b)) {
        ia64_fp_raise_flag(env, sf, IA64_FP_FLAG_V);
        return 2;
    }
    if (ia64_fpr_is_nan(a) || ia64_fpr_is_nan(b)) {
        if (!quiet || ia64_fpr_is_snan(a) || ia64_fpr_is_snan(b)) {
            ia64_fp_raise_flag(env, sf, IA64_FP_FLAG_V);
        }
        return 2;
    }
    if (ia64_fpr_is_unnormal(a) || ia64_fpr_is_unnormal(b)) {
        ia64_fp_raise_flag(env, sf, IA64_FP_FLAG_D);
    }
    return ia64_fpa_compare(a, b, magnitude);
}

static void ia64_do_fcmp(CPUIA64State *env, uint32_t p1, uint32_t p2,
                         uint32_t r2, uint32_t r3, uint32_t cond_code,
                         uint32_t sf)
{
    IA64FPReg left;
    IA64FPReg right;
    int order;
    bool cond;

    if (ia64_fr_nat_get(env, r2) || ia64_fr_nat_get(env, r3)) {
        ia64_pr_write(env, p1, false);
        ia64_pr_write(env, p2, false);
        return;
    }

    left = ia64_fr_reg(env, r2);
    right = ia64_fr_reg(env, r3);
    /* eq and unord (and their negations) are quiet on a QNaN. */
    order = ia64_fp_compare_checked(env, sf, &left, &right,
                                    (cond_code & 3) == 0 ||
                                    (cond_code & 3) == 3, false);

    switch (cond_code & 3) {
    case 0:
        cond = order == 0;
        break;
    case 1:
        cond = order == -1;
        break;
    case 2:
        cond = order == -1 || order == 0;
        break;
    default:
        cond = order == 2;
        break;
    }

    ia64_pr_write(env, p1, cond);
    ia64_pr_write(env, p2, !cond);
}

/* ---- FP min/max (F8 forms select f3 on equality or NaN) ---- */

static void ia64_fminmax(CPUIA64State *env, uint32_t r1, uint32_t r2,
                         uint32_t r3, bool is_max, bool is_abs, uint32_t sf)
{
    IA64FPReg left;
    IA64FPReg right;
    int order;
    bool take_left;

    if (ia64_fr_write_nat_if_any2(env, r1, r2, r3)) {
        return;
    }

    left = ia64_fr_reg(env, r2);
    right = ia64_fr_reg(env, r3);
    /* Invalid is signaled as for fcmp.lt (SDM Vol 3 fmin). */
    order = ia64_fp_compare_checked(env, sf, &left, &right, false, is_abs);
    take_left = is_max ? order == 1 : order == -1;
    ia64_fr_copy(env, r1, take_left ? r2 : r3, 1);
}

static void ia64_do_fmin(CPUIA64State *env, uint32_t r1, uint32_t r2,
                         uint32_t r3, uint32_t sf)
{
    ia64_fminmax(env, r1, r2, r3, false, false, sf);
}

static void ia64_do_fmax(CPUIA64State *env, uint32_t r1, uint32_t r2,
                         uint32_t r3, uint32_t sf)
{
    ia64_fminmax(env, r1, r2, r3, true, false, sf);
}

static void ia64_do_famin(CPUIA64State *env, uint32_t r1, uint32_t r2,
                          uint32_t r3, uint32_t sf)
{
    ia64_fminmax(env, r1, r2, r3, false, true, sf);
}

static void ia64_do_famax(CPUIA64State *env, uint32_t r1, uint32_t r2,
                          uint32_t r3, uint32_t sf)
{
    ia64_fminmax(env, r1, r2, r3, true, true, sf);
}

/* ---- FP reciprocal approximation (frcpa: ~1/x in table index 0) ---- */

typedef struct IA64FPRegisterFormat {
    bool sign;
    uint32_t exp;
    uint64_t mant;
} IA64FPRegisterFormat;

static void ia64_normalize_fp_register_format(IA64FPRegisterFormat *fmt)
{
    int shift;

    if (fmt->exp == 0 ||
        fmt->exp == IA64_FP_REG_SPECIAL_EXP ||
        fmt->mant == 0) {
        return;
    }

    shift = clz64(fmt->mant);
    fmt->mant <<= shift;
    fmt->exp = fmt->exp > (uint32_t)shift ? fmt->exp - shift : 0;
}

static IA64FPRegisterFormat ia64_fr_register_format(CPUIA64State *env,
                                                     uint32_t reg)
{
    uint64_t low;
    uint64_t high;
    IA64FPRegisterFormat fmt;

    ia64_fpreg_to_spill(env, reg, &low, &high);
    fmt.sign = (high >> 17) & 1;
    fmt.exp = (high & 0xffff) | (((high >> 16) & 1) << 16);
    fmt.mant = low;
    ia64_normalize_fp_register_format(&fmt);
    return fmt;
}

static bool ia64_fp_register_format_is_zero(
    const IA64FPRegisterFormat *fmt)
{
    return fmt->mant == 0 && fmt->exp != IA64_FP_REG_SPECIAL_EXP;
}

static bool ia64_fp_register_format_is_infinity(
    const IA64FPRegisterFormat *fmt)
{
    return fmt->exp == IA64_FP_REG_SPECIAL_EXP &&
           fmt->mant == IA64_FP_SIGNIFICAND_INTEGER_BIT;
}

static bool ia64_fp_register_format_is_nan(
    const IA64FPRegisterFormat *fmt)
{
    return fmt->exp == IA64_FP_REG_SPECIAL_EXP &&
           fmt->mant != IA64_FP_SIGNIFICAND_INTEGER_BIT;
}

static bool ia64_frcpa_limits_swa_fault(
    const IA64FPRegisterFormat *num,
    const IA64FPRegisterFormat *den)
{
    int32_t est_exp;

    if (ia64_fp_register_format_is_nan(num) ||
        ia64_fp_register_format_is_nan(den) ||
        ia64_fp_register_format_is_infinity(num) ||
        ia64_fp_register_format_is_zero(num) ||
        ia64_fp_register_format_is_infinity(den) ||
        ia64_fp_register_format_is_zero(den)) {
        return false;
    }

    est_exp = (int32_t)num->exp - (int32_t)den->exp;
    return den->exp == 0 ||
           den->exp >= 2 * IA64_FP_WRE_BIAS - 2 ||
           est_exp <= 2 - (int32_t)IA64_FP_WRE_BIAS ||
           est_exp >= (int32_t)IA64_FP_WRE_BIAS ||
           num->exp <= 64;
}

static bool ia64_frsqrta_limits_swa_fault(
    const IA64FPRegisterFormat *val)
{
    if (val->sign ||
        ia64_fp_register_format_is_nan(val) ||
        ia64_fp_register_format_is_infinity(val) ||
        ia64_fp_register_format_is_zero(val)) {
        return false;
    }

    return val->exp <= 64;
}

static uint32_t ia64_recip_table_index(uint64_t mant)
{
    return (mant >> 55) & 0xff;
}

static uint32_t ia64_recip_sqrt_table_index(
    const IA64FPRegisterFormat *fmt)
{
    return ((fmt->exp & 1) << 7) | ((fmt->mant >> 56) & 0x7f);
}

static uint32_t ia64_fp_register_recip_exp(uint32_t exp)
{
    int32_t result = (int32_t)(2 * IA64_FP_WRE_BIAS - 1) - (int32_t)exp;

    return result > 0 ? (uint32_t)result : 0;
}

static uint32_t ia64_fp_register_rsqrt_exp(uint32_t exp)
{
    int32_t unbiased = (int32_t)exp - IA64_FP_WRE_BIAS;
    int32_t result = (IA64_FP_WRE_BIAS - 1) - (unbiased >> 1);

    return (uint32_t)result & IA64_FP_WRE_EXP_MASK;
}

static const uint16_t ia64_recip_table[256] = {
    0x7fc, 0x7f4, 0x7ec, 0x7e4, 0x7dd, 0x7d5, 0x7cd, 0x7c6,
    0x7be, 0x7b7, 0x7af, 0x7a8, 0x7a1, 0x799, 0x792, 0x78b,
    0x784, 0x77d, 0x776, 0x76f, 0x768, 0x761, 0x75b, 0x754,
    0x74d, 0x746, 0x740, 0x739, 0x733, 0x72c, 0x726, 0x720,
    0x719, 0x713, 0x70d, 0x707, 0x700, 0x6fa, 0x6f4, 0x6ee,
    0x6e8, 0x6e2, 0x6dc, 0x6d7, 0x6d1, 0x6cb, 0x6c5, 0x6bf,
    0x6ba, 0x6b4, 0x6af, 0x6a9, 0x6a3, 0x69e, 0x699, 0x693,
    0x68e, 0x688, 0x683, 0x67e, 0x679, 0x673, 0x66e, 0x669,
    0x664, 0x65f, 0x65a, 0x655, 0x650, 0x64b, 0x646, 0x641,
    0x63c, 0x637, 0x632, 0x62e, 0x629, 0x624, 0x61f, 0x61b,
    0x616, 0x611, 0x60d, 0x608, 0x604, 0x5ff, 0x5fb, 0x5f6,
    0x5f2, 0x5ed, 0x5e9, 0x5e5, 0x5e0, 0x5dc, 0x5d8, 0x5d4,
    0x5cf, 0x5cb, 0x5c7, 0x5c3, 0x5bf, 0x5bb, 0x5b6, 0x5b2,
    0x5ae, 0x5aa, 0x5a6, 0x5a2, 0x59e, 0x59a, 0x597, 0x593,
    0x58f, 0x58b, 0x587, 0x583, 0x57f, 0x57c, 0x578, 0x574,
    0x571, 0x56d, 0x569, 0x566, 0x562, 0x55e, 0x55b, 0x557,
    0x554, 0x550, 0x54d, 0x549, 0x546, 0x542, 0x53f, 0x53b,
    0x538, 0x534, 0x531, 0x52e, 0x52a, 0x527, 0x524, 0x520,
    0x51d, 0x51a, 0x517, 0x513, 0x510, 0x50d, 0x50a, 0x507,
    0x503, 0x500, 0x4fd, 0x4fa, 0x4f7, 0x4f4, 0x4f1, 0x4ee,
    0x4eb, 0x4e8, 0x4e5, 0x4e2, 0x4df, 0x4dc, 0x4d9, 0x4d6,
    0x4d3, 0x4d0, 0x4cd, 0x4ca, 0x4c8, 0x4c5, 0x4c2, 0x4bf,
    0x4bc, 0x4b9, 0x4b7, 0x4b4, 0x4b1, 0x4ae, 0x4ac, 0x4a9,
    0x4a6, 0x4a4, 0x4a1, 0x49e, 0x49c, 0x499, 0x496, 0x494,
    0x491, 0x48e, 0x48c, 0x489, 0x487, 0x484, 0x482, 0x47f,
    0x47c, 0x47a, 0x477, 0x475, 0x473, 0x470, 0x46e, 0x46b,
    0x469, 0x466, 0x464, 0x461, 0x45f, 0x45d, 0x45a, 0x458,
    0x456, 0x453, 0x451, 0x44f, 0x44c, 0x44a, 0x448, 0x445,
    0x443, 0x441, 0x43f, 0x43c, 0x43a, 0x438, 0x436, 0x433,
    0x431, 0x42f, 0x42d, 0x42b, 0x429, 0x426, 0x424, 0x422,
    0x420, 0x41e, 0x41c, 0x41a, 0x418, 0x415, 0x413, 0x411,
    0x40f, 0x40d, 0x40b, 0x409, 0x407, 0x405, 0x403, 0x401,
};

static const uint16_t ia64_recip_sqrt_table[256] = {
    0x5a5, 0x5a0, 0x59a, 0x595, 0x58f, 0x58a, 0x585, 0x580,
    0x57a, 0x575, 0x570, 0x56b, 0x566, 0x561, 0x55d, 0x558,
    0x553, 0x54e, 0x54a, 0x545, 0x540, 0x53c, 0x538, 0x533,
    0x52f, 0x52a, 0x526, 0x522, 0x51e, 0x51a, 0x515, 0x511,
    0x50d, 0x509, 0x505, 0x501, 0x4fd, 0x4fa, 0x4f6, 0x4f2,
    0x4ee, 0x4ea, 0x4e7, 0x4e3, 0x4df, 0x4dc, 0x4d8, 0x4d5,
    0x4d1, 0x4ce, 0x4ca, 0x4c7, 0x4c3, 0x4c0, 0x4bd, 0x4b9,
    0x4b6, 0x4b3, 0x4b0, 0x4ad, 0x4a9, 0x4a6, 0x4a3, 0x4a0,
    0x49d, 0x49a, 0x497, 0x494, 0x491, 0x48e, 0x48b, 0x488,
    0x485, 0x482, 0x47f, 0x47d, 0x47a, 0x477, 0x474, 0x471,
    0x46f, 0x46c, 0x469, 0x467, 0x464, 0x461, 0x45f, 0x45c,
    0x45a, 0x457, 0x454, 0x452, 0x44f, 0x44d, 0x44a, 0x448,
    0x445, 0x443, 0x441, 0x43e, 0x43c, 0x43a, 0x437, 0x435,
    0x433, 0x430, 0x42e, 0x42c, 0x429, 0x427, 0x425, 0x423,
    0x420, 0x41e, 0x41c, 0x41a, 0x418, 0x416, 0x414, 0x411,
    0x40f, 0x40d, 0x40b, 0x409, 0x407, 0x405, 0x403, 0x401,
    0x7fc, 0x7f4, 0x7ec, 0x7e5, 0x7dd, 0x7d5, 0x7ce, 0x7c7,
    0x7bf, 0x7b8, 0x7b1, 0x7aa, 0x7a3, 0x79c, 0x795, 0x78e,
    0x788, 0x781, 0x77a, 0x774, 0x76d, 0x767, 0x761, 0x75a,
    0x754, 0x74e, 0x748, 0x742, 0x73c, 0x736, 0x730, 0x72b,
    0x725, 0x71f, 0x71a, 0x714, 0x70f, 0x709, 0x704, 0x6fe,
    0x6f9, 0x6f4, 0x6ee, 0x6e9, 0x6e4, 0x6df, 0x6da, 0x6d5,
    0x6d0, 0x6cb, 0x6c6, 0x6c1, 0x6bd, 0x6b8, 0x6b3, 0x6ae,
    0x6aa, 0x6a5, 0x6a1, 0x69c, 0x698, 0x693, 0x68f, 0x68a,
    0x686, 0x682, 0x67d, 0x679, 0x675, 0x671, 0x66d, 0x668,
    0x664, 0x660, 0x65c, 0x658, 0x654, 0x650, 0x64c, 0x649,
    0x645, 0x641, 0x63d, 0x639, 0x635, 0x632, 0x62e, 0x62a,
    0x627, 0x623, 0x620, 0x61c, 0x618, 0x615, 0x611, 0x60e,
    0x60a, 0x607, 0x604, 0x600, 0x5fd, 0x5f9, 0x5f6, 0x5f3,
    0x5f0, 0x5ec, 0x5e9, 0x5e6, 0x5e3, 0x5df, 0x5dc, 0x5d9,
    0x5d6, 0x5d3, 0x5d0, 0x5cd, 0x5ca, 0x5c7, 0x5c4, 0x5c1,
    0x5be, 0x5bb, 0x5b8, 0x5b5, 0x5b2, 0x5af, 0x5ac, 0x5aa,
};

typedef struct IA64FPSWAFormat {
    bool sign;
    bool unnormal;
    int32_t exp;
    uint64_t mant;
} IA64FPSWAFormat;

typedef struct IA64FPSWARawResult {
    bool sign;
    int32_t exp;
    uint64_t mant;
    bool tail;
    bool half_greater;
    bool half_equal;
} IA64FPSWARawResult;

static IA64FPSWAFormat ia64_fpswa_format(CPUIA64State *env, uint32_t reg)
{
    IA64FPSWAFormat value;
    uint64_t high;
    int shift;

    ia64_fpreg_to_spill(env, reg, &value.mant, &high);
    value.sign = (high >> 17) & 1;
    value.exp = high & IA64_FP_WRE_EXP_MASK;
    value.unnormal = value.mant != 0 &&
                     !(value.mant & IA64_FP_SIGNIFICAND_INTEGER_BIT);
    if (value.unnormal) {
        shift = clz64(value.mant);
        value.mant <<= shift;
        value.exp -= shift;
    }
    return value;
}

static uint8_t ia64_fpswa_physical_fr(const CPUIA64State *env,
                                      uint32_t reg)
{
    if (reg < IA64_ROTATING_FR_BASE) {
        return reg;
    }
    return IA64_ROTATING_FR_BASE +
           ((reg - IA64_ROTATING_FR_BASE + env->cfm_rrb_fr) %
            IA64_ROTATING_FR_COUNT);
}

static uint8_t ia64_fpswa_physical_pr(const CPUIA64State *env,
                                      uint32_t reg)
{
    if (reg < 16) {
        return reg;
    }
    return 16 + ((reg - 16 + env->cfm_rrb_pr) % 48);
}

static __uint128_t ia64_fpswa_round_right(
    const IA64FPSWARawResult *raw, uint32_t shift, uint32_t rounding,
    bool *inexact, bool *fpa)
{
    __uint128_t upper;
    uint64_t discarded = 0;
    uint64_t half = 0;
    bool round_up = false;

    if (shift == 0) {
        upper = raw->mant;
        *inexact = raw->tail;
        if (rounding == 0 && raw->tail) {
            round_up = raw->half_greater ||
                       (raw->half_equal && (raw->mant & 1));
        }
    } else if (shift < 64) {
        upper = raw->mant >> shift;
        discarded = raw->mant & ((1ULL << shift) - 1);
        half = 1ULL << (shift - 1);
        *inexact = discarded != 0 || raw->tail;
        if (rounding == 0 && *inexact) {
            round_up = discarded > half ||
                       (discarded == half &&
                        (raw->tail || ((uint64_t)upper & 1)));
        }
    } else if (shift == 64) {
        upper = 0;
        discarded = raw->mant;
        half = 1ULL << 63;
        *inexact = discarded != 0 || raw->tail;
        if (rounding == 0 && *inexact) {
            round_up = discarded > half ||
                       (discarded == half && raw->tail);
        }
    } else {
        upper = 0;
        *inexact = raw->mant != 0 || raw->tail;
    }

    if (*inexact && rounding != 0) {
        round_up = (rounding == 1 && raw->sign) ||
                   (rounding == 2 && !raw->sign);
    }
    if (round_up) {
        upper++;
    }
    *fpa = round_up;
    return upper;
}

static uint64_t ia64_fpswa_isqrt128(__uint128_t value,
                                    __uint128_t *remainder)
{
    __uint128_t estimate;
    __uint128_t next;
    uint64_t high = value >> 64;
    uint32_t significant_bits;

    if (value == 0) {
        *remainder = 0;
        return 0;
    }

    significant_bits = high != 0 ? 128U - clz64(high) :
                                   64U - clz64((uint64_t)value);
    estimate = (__uint128_t)1 << ((significant_bits + 1U) / 2U);

    for (;;) {
        next = (estimate + value / estimate) >> 1;
        if (next >= estimate) {
            break;
        }
        estimate = next;
    }

    *remainder = value - estimate * estimate;
    return estimate;
}

static bool ia64_fpswa_exception_masked(uint64_t fpsr, uint32_t sf,
                                        uint32_t flag)
{
    uint64_t controls = (fpsr >> ia64_fpsr_sf_shift(sf)) &
                        IA64_FPSR_SF_CONTROLS_MASK;

    return ((sf != 0) && (controls & IA64_FPSR_SF_TD)) ||
           (fpsr & flag);
}

static void ia64_fpswa_cache_result(CPUIA64State *env,
                                    const IA64FPSWARawResult *raw,
                                    uint32_t sf, bool unnormal)
{
    uint64_t controls = ia64_fpsr_sf_controls(env, sf);
    uint32_t precision_control = (controls >> 2) & 3;
    uint32_t precision = precision_control == 0 ? 24 :
                         precision_control == 2 ? 53 : 64;
    uint32_t rounding = (controls >> 4) & 3;
    int32_t min_exp = (controls & 2) ? 1 : 0xc001;
    int32_t max_exp = (controls & 2) ? 0x1fffe : 0x13ffe;
    uint32_t precision_shift = 64 - precision;
    uint32_t shift = precision_shift;
    int32_t result_exp = raw->exp;
    __uint128_t rounded;
    uint64_t result_mant;
    bool inexact;
    bool fpa;

    env->fp.fpswa_flags = unnormal ? (1U << 1) : 0;
    if (result_exp < min_exp) {
        shift += (uint32_t)(min_exp - result_exp);
    }
    rounded = ia64_fpswa_round_right(raw, shift, rounding, &inexact, &fpa);

    if (result_exp < min_exp) {
        uint64_t normal_threshold = 1ULL << (precision - 1);

        result_mant = (uint64_t)rounded << precision_shift;
        result_exp = (uint64_t)rounded >= normal_threshold ? min_exp : 0;
        if (result_exp == 0 && (controls & 1) && result_mant != 0) {
            result_mant = 0;
            inexact = true;
            env->fp.fpswa_flags |= (1U << 4) | (1U << 5);
        } else if (result_exp == 0 &&
                   (inexact ||
                    !ia64_fpswa_exception_masked(env->ar_fpsr, sf,
                                                  1U << 4))) {
            env->fp.fpswa_flags |= 1U << 4;
        }
    } else {
        if (rounded == ((__uint128_t)1 << precision)) {
            rounded >>= 1;
            result_exp++;
        }
        result_mant = (uint64_t)rounded << precision_shift;
    }

    if (result_exp > max_exp) {
        bool overflow_masked = ia64_fpswa_exception_masked(
            env->ar_fpsr, sf, 1U << 3);

        env->fp.fpswa_flags |= (1U << 3) | (1U << 5);
        inexact = true;
        if (!overflow_masked) {
            result_exp &= IA64_FP_WRE_EXP_MASK;
        } else if (rounding == 0 ||
                   (rounding == 1 && raw->sign) ||
                   (rounding == 2 && !raw->sign)) {
            result_exp = IA64_FP_REG_SPECIAL_EXP;
            result_mant = IA64_FP_SIGNIFICAND_INTEGER_BIT;
        } else {
            result_exp = max_exp;
            result_mant = UINT64_MAX << precision_shift;
        }
    }
    if (inexact) {
        env->fp.fpswa_flags |= 1U << 5;
    }

    env->fp.fpswa_result_low = result_mant;
    env->fp.fpswa_result_high =
        ((uint64_t)result_exp & IA64_FP_WRE_EXP_MASK) |
        ((uint64_t)raw->sign << 17);
    env->fp.fpswa_sf = sf;
    env->fp.fpswa_fpa = fpa;
    env->fp.fpswa_pending = true;
}

static void ia64_fpswa_prepare_divide(CPUIA64State *env, uint32_t r1,
                                      uint32_t p2, uint32_t r2,
                                      uint32_t r3, uint32_t sf)
{
    IA64FPSWAFormat num = ia64_fpswa_format(env, r2);
    IA64FPSWAFormat den = ia64_fpswa_format(env, r3);
    IA64FPSWARawResult raw = { 0 };
    __uint128_t dividend;
    __uint128_t remainder;
    uint32_t normalize = num.mant < den.mant;

    raw.sign = num.sign ^ den.sign;
    raw.exp = IA64_FP_WRE_BIAS + num.exp - den.exp - normalize;
    dividend = (__uint128_t)num.mant << (63 + normalize);
    raw.mant = dividend / den.mant;
    remainder = dividend % den.mant;
    raw.tail = remainder != 0;
    if (raw.tail) {
        __uint128_t twice = remainder << 1;

        raw.half_greater = twice > den.mant;
        raw.half_equal = twice == den.mant;
    }

    env->fp.fpswa_dest_fr = ia64_fpswa_physical_fr(env, r1);
    env->fp.fpswa_dest_pr = ia64_fpswa_physical_pr(env, p2);
    ia64_fpswa_cache_result(env, &raw, sf,
                            num.unnormal || den.unnormal);
}

static void ia64_fpswa_prepare_sqrt(CPUIA64State *env, uint32_t r1,
                                    uint32_t p2, uint32_t r3, uint32_t sf)
{
    IA64FPSWAFormat val = ia64_fpswa_format(env, r3);
    IA64FPSWARawResult raw = { 0 };
    __uint128_t radicand;
    __uint128_t remainder;
    int32_t unbiased = val.exp - IA64_FP_WRE_BIAS;
    uint32_t odd = unbiased & 1;

    raw.sign = false;
    raw.exp = IA64_FP_WRE_BIAS + (unbiased - (int32_t)odd) / 2;
    radicand = (__uint128_t)val.mant << (63 + odd);
    raw.mant = ia64_fpswa_isqrt128(radicand, &remainder);
    raw.tail = remainder != 0;
    raw.half_greater = remainder > raw.mant;

    env->fp.fpswa_dest_fr = ia64_fpswa_physical_fr(env, r1);
    env->fp.fpswa_dest_pr = ia64_fpswa_physical_pr(env, p2);
    ia64_fpswa_cache_result(env, &raw, sf, val.unnormal);
}

static IA64FPReg ia64_fp_special_reg(bool sign, bool inf)
{
    return (IA64FPReg){
        .sign = sign,
        .exp = inf ? IA64_FP_EXP_SPECIAL : 0,
        .sig = inf ? IA64_FP_INT_BIT : 0,
    };
}

/*
 * frcpa gives the IEEE quotient f2/f3 for special operands with p2 = 0,
 * and otherwise the table approximation of 1/f3 with p2 = 1 (SDM Vol 3
 * frcpa).  The checks follow Vol 1 Figure 5-11; zeros and pseudo-zeros
 * give canonical zeros (Vol 1 5.1.3).
 */
static void ia64_do_frcpa(CPUIA64State *env, uint32_t r1, uint32_t p2,
                          uint32_t r2, uint32_t r3, uint32_t sf)
{
    IA64FPRegisterFormat num_fmt;
    IA64FPRegisterFormat den_fmt;
    IA64FPReg num;
    IA64FPReg den;
    IA64FPReg result;
    bool sign;

    if (ia64_fr_nat_get(env, r2) || ia64_fr_nat_get(env, r3)) {
        ia64_fr_write_nat(env, r1);
        ia64_pr_write(env, p2, false);
        return;
    }

    num = ia64_fr_reg(env, r2);
    den = ia64_fr_reg(env, r3);
    sign = num.sign ^ den.sign;
    if (ia64_fpr_is_unsupported(&num) || ia64_fpr_is_unsupported(&den) ||
        (ia64_fpr_is_zero_value(&num) && ia64_fpr_is_zero_value(&den)) ||
        (ia64_fpr_is_inf(&num) && ia64_fpr_is_inf(&den))) {
        ia64_fp_raise_flag(env, sf, IA64_FP_FLAG_V);
        ia64_fr_write_reg(env, r1, ia64_fp_qnan_indefinite);
        ia64_pr_write(env, p2, false);
        return;
    }
    if (ia64_fpr_is_nan(&num) || ia64_fpr_is_nan(&den)) {
        if (ia64_fpr_is_snan(&num) || ia64_fpr_is_snan(&den)) {
            ia64_fp_raise_flag(env, sf, IA64_FP_FLAG_V);
        }
        result = ia64_fpr_is_nan(&num) ? num : den;
        result.sig |= IA64_FP_QUIET_BIT;
        ia64_fr_write_reg(env, r1, result);
        ia64_pr_write(env, p2, false);
        return;
    }
    if (ia64_fpr_is_zero_value(&den) && !ia64_fpr_is_inf(&num)) {
        ia64_fp_raise_flag(env, sf, IA64_FP_FLAG_Z);
        ia64_fr_write_reg(env, r1, ia64_fp_special_reg(sign, true));
        ia64_pr_write(env, p2, false);
        return;
    }

    num_fmt = ia64_fr_register_format(env, r2);
    den_fmt = ia64_fr_register_format(env, r3);
    if (ia64_frcpa_limits_swa_fault(&num_fmt, &den_fmt)) {
        ia64_fpswa_prepare_divide(env, r1, p2, r2, r3, sf);
        ia64_raise_fp_fault(env, IA64_FP_ISR_SWA);
    }
    if (ia64_fpr_is_unnormal(&num) || ia64_fpr_is_unnormal(&den)) {
        ia64_fp_raise_flag(env, sf, IA64_FP_FLAG_D);
    }

    if (ia64_fpr_is_inf(&num) || ia64_fpr_is_inf(&den) ||
        ia64_fpr_is_zero_value(&num)) {
        ia64_fr_write_reg(env, r1,
                          ia64_fp_special_reg(sign, ia64_fpr_is_inf(&num)));
        ia64_pr_write(env, p2, false);
        return;
    }
    result.sign = den_fmt.sign;
    result.exp = ia64_fp_register_recip_exp(den_fmt.exp);
    result.sig = (uint64_t)ia64_recip_table[
        ia64_recip_table_index(den_fmt.mant)] << 53;
    ia64_fr_write_reg(env, r1, result);
    ia64_pr_write(env, p2, true);
}

static bool ia64_float32_rcpa_predicate(float32 num, float32 den)
{
    return !float32_is_zero(num) &&
           !float32_is_infinity(num) &&
           !float32_is_any_nan(num) &&
           !float32_is_zero(den) &&
           !float32_is_infinity(den) &&
           !float32_is_any_nan(den);
}

static float32 ia64_float32_rcpa(float32 num, float32 den,
                                 bool approximate, float_status *status)
{
    return float32_div(approximate ? float32_one : num, den, status);
}

static bool ia64_float32_register_format(float32 val,
                                         IA64FPRegisterFormat *fmt)
{
    uint32_t bits = float32_val(val);
    uint32_t exp = (bits >> 23) & 0xff;
    uint32_t frac = bits & 0x7fffff;

    if (exp == 0 || exp == 0xff) {
        return false;
    }

    fmt->sign = bits >> 31;
    fmt->exp = exp + IA64_FP_WRE_BIAS - IA64_FP_SINGLE_BIAS;
    fmt->mant = IA64_FP_SIGNIFICAND_INTEGER_BIT | ((uint64_t)frac << 40);
    return true;
}

/* fp_normalize(fp_reg_read_hi/lo()) of a normal or denormal lane. */
static bool ia64_float32_normalized_register_format(float32 val,
                                                    IA64FPRegisterFormat *fmt)
{
    uint32_t bits = float32_val(val);

    if (ia64_float32_register_format(val, fmt)) {
        return true;
    }
    if (!float32_is_denormal(val)) {
        return false;
    }

    fmt->sign = bits >> 31;
    fmt->exp = IA64_FP_WRE_BIAS + 1 - IA64_FP_SINGLE_BIAS;
    fmt->mant = (uint64_t)(bits & 0x7fffff) << 40;
    ia64_normalize_fp_register_format(fmt);
    return true;
}

static float32
ia64_float32_from_register_format(CPUIA64State *env,
                                  const IA64FPRegisterFormat *fmt)
{
    float_status status = env->fp.fp_status;

    return floatx80_to_float32(
        ia64_register_format_to_floatx80(env, fmt->sign, fmt->exp,
                                         fmt->mant),
        &status);
}

static float32 ia64_float32_rcpa_approx(CPUIA64State *env,
                                        const IA64FPRegisterFormat *den)
{
    IA64FPRegisterFormat result = {
        .sign = den->sign,
        .exp = ia64_fp_register_recip_exp(den->exp),
        .mant = (uint64_t)ia64_recip_table[
            ia64_recip_table_index(den->mant)] << 53,
    };

    return ia64_float32_from_register_format(env, &result);
}

static bool ia64_float32_fprcpa_predicate(const IA64FPRegisterFormat *num,
                                          const IA64FPRegisterFormat *den)
{
    int32_t est_exp = (int32_t)num->exp - (int32_t)den->exp;

    return est_exp < IA64_FP_SINGLE_BIAS &&
           est_exp > 2 - IA64_FP_SINGLE_BIAS &&
           num->exp > IA64_FP_WRE_BIAS - IA64_FP_SINGLE_BIAS +
                      IA64_FP_SINGLE_MANT_WIDTH;
}

/* fprcpa raises only V, Z and D (SDM Vol 3 fprcpa "FP Exceptions"). */
#define IA64_FP_APPROX_SOFT_FLAGS                                  \
    (float_flag_invalid | float_flag_divbyzero |                   \
     float_flag_input_denormal_flushed | float_flag_input_denormal_used)

/*
 * A denormal operand is normalized and sets D; the result is still the
 * approximation of the denominator's reciprocal (SDM Vol 3 fprcpa,
 * fp_ieee_recip).  Outside the denominator limits (limits_check.hi_fr3 and
 * lo_fr3) the lane is infinity for eb <= emin - 1 and zero for
 * eb >= emax - 2, with the sign of the denominator, and p2 is cleared
 * (245415-001 §5.1).
 */
static float32 ia64_fprcpa_lane(CPUIA64State *env, float32 num, float32 den,
                                bool *pred, float_status *status)
{
    IA64FPRegisterFormat num_fmt = { 0 };
    IA64FPRegisterFormat den_fmt = { 0 };
    bool finite = ia64_float32_rcpa_predicate(num, den);

    if (finite && ia64_float32_normalized_register_format(num, &num_fmt) &&
        ia64_float32_normalized_register_format(den, &den_fmt)) {
        if (float32_is_denormal(num) || float32_is_denormal(den)) {
            float_raise(float_flag_input_denormal_used, status);
        }
        *pred = false;
        if (den_fmt.exp <= IA64_FP_WRE_BIAS - IA64_FP_SINGLE_BIAS) {
            return float32_set_sign(float32_infinity, den_fmt.sign);
        }
        if (den_fmt.exp >= IA64_FP_WRE_BIAS + IA64_FP_SINGLE_BIAS - 2) {
            return float32_set_sign(float32_zero, den_fmt.sign);
        }
        *pred = ia64_float32_fprcpa_predicate(&num_fmt, &den_fmt);
        return ia64_float32_rcpa_approx(env, &den_fmt);
    }
    *pred = false;
    return ia64_float32_rcpa(num, den, finite, status);
}

static void ia64_do_fprcpa(CPUIA64State *env, uint32_t r1, uint32_t p2,
                           uint32_t r2, uint32_t r3, uint32_t sf)
{
    uint64_t num = ia64_fr_significand(env, r2);
    uint64_t den = ia64_fr_significand(env, r3);
    float_status hi_status = env->fp.fp_status;
    float_status lo_status = env->fp.fp_status;
    bool hi_pred;
    bool lo_pred;
    float32 hi_result;
    float32 lo_result;
    int hi_soft;
    int lo_soft;

    if (ia64_fr_nat_get(env, r2) || ia64_fr_nat_get(env, r3)) {
        ia64_fr_write_nat(env, r1);
        ia64_pr_write(env, p2, false);
        return;
    }

    hi_result = ia64_fprcpa_lane(env, make_float32(num >> 32),
                                 make_float32(den >> 32), &hi_pred,
                                 &hi_status);
    lo_result = ia64_fprcpa_lane(env, make_float32(num), make_float32(den),
                                 &lo_pred, &lo_status);
    hi_soft = get_float_exception_flags(&hi_status) &
              IA64_FP_APPROX_SOFT_FLAGS;
    lo_soft = get_float_exception_flags(&lo_status) &
              IA64_FP_APPROX_SOFT_FLAGS;

    {
        uint64_t traps = ia64_fp_active_traps(env, sf);
        uint64_t hi_fault = ia64_fp_soft_flags_to_ia64(hi_soft) & ~traps & 0x7;
        uint64_t lo_fault = ia64_fp_soft_flags_to_ia64(lo_soft) & ~traps & 0x7;

        set_float_exception_flags(hi_soft | lo_soft, &env->fp.fp_status);
        if (hi_fault || lo_fault) {
            ia64_raise_fp_fault(env, hi_fault | (lo_fault << 4));
        }
    }

    ia64_fr_write_sig(env, r1,
                      ((uint64_t)float32_val(hi_result) << 32) |
                      float32_val(lo_result));
    ia64_pr_write(env, p2, hi_pred && lo_pred);
}

/* ---- FP classify ---- */

void ia64_fp_fclass(CPUIA64State *env, uint32_t p1, uint32_t p2,
                   uint32_t f2, uint32_t fclass9)
{
    uint64_t mant;
    uint64_t high;
    uint32_t exp;
    bool is_neg;
    bool is_nat = ia64_fr_nat_get(env, f2);
    bool is_zero;
    bool is_unorm;
    bool is_inf;
    bool is_qnan;
    bool is_snan;
    bool is_normal;
    bool sign_match;
    bool type_match;
    bool member;

    ia64_fpreg_to_spill(env, f2, &mant, &high);
    exp = (high & 0xffff) | (((high >> 16) & 1) << 16);
    is_neg = (high >> 17) & 1;
    /*
     * A pseudo-zero is an unnormal, and a pseudo-NaN or pseudo-infinity is
     * in no class (SDM Vol 1 5.1.3, Table 5-2; Vol 3 fclass).
     */
    is_zero = !is_nat && mant == 0 && exp == 0;
    is_inf = !is_nat && exp == IA64_FP_REG_SPECIAL_EXP &&
             mant == IA64_FP_SIGNIFICAND_INTEGER_BIT;
    is_qnan = !is_nat && exp == IA64_FP_REG_SPECIAL_EXP &&
              mant >= 0xc000000000000000ULL;
    is_snan = !is_nat && exp == IA64_FP_REG_SPECIAL_EXP &&
              mant > IA64_FP_SIGNIFICAND_INTEGER_BIT &&
              mant < 0xc000000000000000ULL;
    is_unorm = !is_nat && !is_zero &&
                exp != IA64_FP_REG_SPECIAL_EXP &&
                (!(mant & IA64_FP_SIGNIFICAND_INTEGER_BIT) || exp == 0);
    is_normal = !is_nat && exp != 0 && exp != IA64_FP_REG_SPECIAL_EXP &&
                (mant & IA64_FP_SIGNIFICAND_INTEGER_BIT);
    sign_match = ((fclass9 & 0x001) && !is_neg) ||
                 ((fclass9 & 0x002) && is_neg);
    type_match = ((fclass9 & 0x004) && is_zero) ||
                 ((fclass9 & 0x008) && is_unorm) ||
                 ((fclass9 & 0x010) && is_normal) ||
                 ((fclass9 & 0x020) && is_inf);
    member = (sign_match && type_match) ||
                  ((fclass9 & 0x040) && is_snan) ||
                  ((fclass9 & 0x080) && is_qnan) ||
                  ((fclass9 & 0x100) && is_nat);

    if (is_nat && !(fclass9 & 0x100)) {
        if (p1) {
            env->pr[p1] = 0;
        }
        if (p2) {
            env->pr[p2] = 0;
        }
    } else {
        if (p1) {
            env->pr[p1] = member ? 1 : 0;
        }
        if (p2) {
            env->pr[p2] = member ? 0 : 1;
        }
    }
    env->pr[IA64_PR_TRUE] = 1;
}

#define IA64_FP_SIGN_MASK 0x8000000000000000ULL
#define IA64_FP_EXP_MASK  0x7ff0000000000000ULL
#define IA64_FP_FRAC_MASK 0x000fffffffffffffULL

/* ---- FP merge ---- */

void ia64_fp_fmerge_ns(CPUIA64State *env, uint32_t r1,
                      uint32_t r2, uint32_t r3)
{
    uint64_t low2;
    uint64_t high2;
    uint64_t low3;
    uint64_t high3;

    if (ia64_fr_nat_get(env, r2) || ia64_fr_nat_get(env, r3)) {
        ia64_fr_write_nat(env, r1);
        return;
    }

    ia64_fpreg_to_spill(env, r2, &low2, &high2);
    ia64_fpreg_to_spill(env, r3, &low3, &high3);
    ia64_fpreg_from_spill(env, r1, low3,
                          (high3 & ~(1ULL << 17)) |
                          ((~high2) & (1ULL << 17)));
}

void ia64_fp_fmerge_s(CPUIA64State *env, uint32_t r1, uint32_t r2, uint32_t r3)
{
    uint64_t low2;
    uint64_t high2;
    uint64_t low3;
    uint64_t high3;

    if (ia64_fr_nat_get(env, r2) || ia64_fr_nat_get(env, r3)) {
        ia64_fr_write_nat(env, r1);
        return;
    }

    ia64_fpreg_to_spill(env, r2, &low2, &high2);
    ia64_fpreg_to_spill(env, r3, &low3, &high3);
    ia64_fpreg_from_spill(env, r1, low3,
                          (high3 & ~(1ULL << 17)) |
                          (high2 & (1ULL << 17)));
}

void ia64_fp_fmerge_se(CPUIA64State *env, uint32_t r1, uint32_t r2, uint32_t r3)
{
    uint64_t low2;
    uint64_t high2;
    uint64_t low3;
    uint64_t high3;

    if (ia64_fr_nat_get(env, r2) || ia64_fr_nat_get(env, r3)) {
        ia64_fr_write_nat(env, r1);
        return;
    }

    ia64_fpreg_to_spill(env, r2, &low2, &high2);
    ia64_fpreg_to_spill(env, r3, &low3, &high3);
    ia64_fpreg_from_spill(env, r1, low3, high2);
}

/* ---- FP logical/swap ---- */

void ia64_fp_flogical_and(CPUIA64State *env, uint32_t r1,
                         uint32_t r2, uint32_t r3)
{
    if (ia64_fr_write_nat_if_any2(env, r1, r2, r3)) {
        return;
    }

    ia64_fr_write_sig(env, r1, ia64_fr_significand(env, r2) &
                      ia64_fr_significand(env, r3));
}

void ia64_fp_flogical_andcm(CPUIA64State *env, uint32_t r1,
                           uint32_t r2, uint32_t r3)
{
    if (ia64_fr_write_nat_if_any2(env, r1, r2, r3)) {
        return;
    }

    ia64_fr_write_sig(env, r1, ia64_fr_significand(env, r2) &
                      ~ia64_fr_significand(env, r3));
}

void ia64_fp_flogical_or(CPUIA64State *env, uint32_t r1,
                        uint32_t r2, uint32_t r3)
{
    if (ia64_fr_write_nat_if_any2(env, r1, r2, r3)) {
        return;
    }

    ia64_fr_write_sig(env, r1, ia64_fr_significand(env, r2) |
                      ia64_fr_significand(env, r3));
}

void ia64_fp_flogical_xor(CPUIA64State *env, uint32_t r1,
                         uint32_t r2, uint32_t r3)
{
    if (ia64_fr_write_nat_if_any2(env, r1, r2, r3)) {
        return;
    }

    ia64_fr_write_sig(env, r1, ia64_fr_significand(env, r2) ^
                      ia64_fr_significand(env, r3));
}

void ia64_fp_fswap(CPUIA64State *env, uint32_t r1, uint32_t r2,
                  uint32_t r3, uint32_t form)
{
    uint32_t hi;
    uint32_t lo;

    if (ia64_fr_write_nat_if_any2(env, r1, r2, r3)) {
        return;
    }

    hi = ia64_fr_significand(env, r3);
    lo = ia64_fr_significand(env, r2) >> 32;
    if (form == 1) {
        hi ^= 0x80000000U;
    } else if (form == 2) {
        lo ^= 0x80000000U;
    }

    ia64_fr_write_sig(env, r1, ((uint64_t)hi << 32) | lo);
}

void ia64_fp_fmix(CPUIA64State *env, uint32_t r1, uint32_t r2,
                 uint32_t r3, uint32_t form)
{
    uint64_t f2;
    uint64_t f3;
    uint32_t hi;
    uint32_t lo;

    if (ia64_fr_write_nat_if_any2(env, r1, r2, r3)) {
        return;
    }

    f2 = ia64_fr_significand(env, r2);
    f3 = ia64_fr_significand(env, r3);
    if (form == 1) {
        hi = f2;
        lo = f3;
    } else {
        hi = f2 >> 32;
        lo = form == 2 ? f3 >> 32 : f3;
    }

    ia64_fr_write_sig(env, r1, ((uint64_t)hi << 32) | lo);
}

void ia64_fp_fsxt(CPUIA64State *env, uint32_t r1, uint32_t r2,
                 uint32_t r3, uint32_t form)
{
    uint64_t f2;
    uint64_t f3;
    uint32_t hi;
    uint32_t lo;

    if (ia64_fr_write_nat_if_any2(env, r1, r2, r3)) {
        return;
    }

    f2 = ia64_fr_significand(env, r2);
    f3 = ia64_fr_significand(env, r3);
    if (form == 1) {
        hi = (f2 >> 63) ? UINT32_MAX : 0;
        lo = f3 >> 32;
    } else {
        hi = ((f2 >> 31) & 1) ? UINT32_MAX : 0;
        lo = f3;
    }

    ia64_fr_write_sig(env, r1, ((uint64_t)hi << 32) | lo);
}

/* ---- Parallel FP merge/min/max/compare ---- */

static uint64_t ia64_pack_fp32_lanes(uint32_t hi, uint32_t lo)
{
    return ((uint64_t)hi << 32) | lo;
}

void ia64_fp_fpmerge(CPUIA64State *env, uint32_t r1, uint32_t r2,
                    uint32_t r3, uint32_t form)
{
    uint64_t f2;
    uint64_t f3;
    uint32_t f2_hi;
    uint32_t f2_lo;
    uint32_t f3_hi;
    uint32_t f3_lo;
    uint32_t hi;
    uint32_t lo;

    if (ia64_fr_write_nat_if_any2(env, r1, r2, r3)) {
        return;
    }

    f2 = ia64_fr_significand(env, r2);
    f3 = ia64_fr_significand(env, r3);
    f2_hi = f2 >> 32;
    f2_lo = f2;
    f3_hi = f3 >> 32;
    f3_lo = f3;

    if (form == 0) {
        hi = ((f2_hi ^ 0x80000000U) & 0x80000000U) |
             (f3_hi & 0x7fffffffU);
        lo = ((f2_lo ^ 0x80000000U) & 0x80000000U) |
             (f3_lo & 0x7fffffffU);
    } else if (form == 1) {
        hi = (f2_hi & 0x80000000U) | (f3_hi & 0x7fffffffU);
        lo = (f2_lo & 0x80000000U) | (f3_lo & 0x7fffffffU);
    } else {
        hi = (f2_hi & 0xff800000U) | (f3_hi & 0x007fffffU);
        lo = (f2_lo & 0xff800000U) | (f3_lo & 0x007fffffU);
    }

    ia64_fr_write_sig(env, r1, ia64_pack_fp32_lanes(hi, lo));
}

static uint32_t ia64_fpminmax_lane(uint32_t a_bits, uint32_t b_bits,
                                   bool is_max, bool is_abs,
                                   float_status *status)
{
    float32 a = make_float32(a_bits);
    float32 b = make_float32(b_bits);
    float32 cmp_a = is_abs ? float32_abs(a) : a;
    float32 cmp_b = is_abs ? float32_abs(b) : b;
    FloatRelation rel;

    if (float32_is_any_nan(a) || float32_is_any_nan(b)) {
        float_raise(float_flag_invalid, status);
        return b_bits;
    }

    rel = float32_compare(cmp_a, cmp_b, status);
    if (is_max) {
        return rel == float_relation_greater ? a_bits : b_bits;
    }
    return rel == float_relation_less ? a_bits : b_bits;
}

static void ia64_do_fpminmax(CPUIA64State *env, uint32_t r1, uint32_t r2,
                             uint32_t r3, uint32_t is_max, uint32_t is_abs,
                             uint32_t sf)
{
    uint64_t f2;
    uint64_t f3;
    float_status hi_status = env->fp.fp_status;
    float_status lo_status = env->fp.fp_status;
    uint32_t f2_hi;
    uint32_t f2_lo;
    uint32_t f3_hi;
    uint32_t f3_lo;
    uint32_t hi;
    uint32_t lo;

    if (ia64_fr_write_nat_if_any2(env, r1, r2, r3)) {
        return;
    }

    f2 = ia64_fr_significand(env, r2);
    f3 = ia64_fr_significand(env, r3);
    f2_hi = f2 >> 32;
    f2_lo = f2;
    f3_hi = f3 >> 32;
    f3_lo = f3;
    hi = ia64_fpminmax_lane(f2_hi, f3_hi, is_max != 0, is_abs != 0,
                            &hi_status);
    lo = ia64_fpminmax_lane(f2_lo, f3_lo, is_max != 0, is_abs != 0,
                            &lo_status);
    ia64_fp_simd_fault_end(env, sf, get_float_exception_flags(&hi_status),
                           get_float_exception_flags(&lo_status));

    ia64_fr_write_sig(env, r1, ia64_pack_fp32_lanes(hi, lo));
}

static bool ia64_fpcmp_lane(uint32_t a_bits, uint32_t b_bits, uint32_t frel,
                            float_status *status)
{
    /* eq, unord, neq and ord are quiet on a QNaN (SDM Vol 3 Table 2-30). */
    bool quiet = (frel & 3) == 0 || (frel & 3) == 3;
    FloatRelation rel = quiet ?
        float32_compare_quiet(make_float32(a_bits), make_float32(b_bits),
                              status) :
        float32_compare(make_float32(a_bits), make_float32(b_bits), status);

    switch (frel & 7) {
    case 0:
        return rel == float_relation_equal;
    case 1:
        return rel == float_relation_less;
    case 2:
        return rel == float_relation_less || rel == float_relation_equal;
    case 3:
        return rel == float_relation_unordered;
    case 4:
        return rel != float_relation_equal;
    case 5:
        return rel != float_relation_less;
    case 6:
        return rel != float_relation_less && rel != float_relation_equal;
    default:
        return rel != float_relation_unordered;
    }
}

static void ia64_do_fpcmp(CPUIA64State *env, uint32_t r1, uint32_t r2,
                          uint32_t r3, uint32_t frel, uint32_t sf)
{
    uint64_t f2;
    uint64_t f3;
    float_status hi_status = env->fp.fp_status;
    float_status lo_status = env->fp.fp_status;
    uint32_t hi;
    uint32_t lo;

    if (ia64_fr_write_nat_if_any2(env, r1, r2, r3)) {
        return;
    }

    f2 = ia64_fr_significand(env, r2);
    f3 = ia64_fr_significand(env, r3);
    hi = ia64_fpcmp_lane(f2 >> 32, f3 >> 32, frel, &hi_status) ?
         UINT32_MAX : 0;
    lo = ia64_fpcmp_lane(f2, f3, frel, &lo_status) ? UINT32_MAX : 0;
    ia64_fp_simd_fault_end(env, sf, get_float_exception_flags(&hi_status),
                           get_float_exception_flags(&lo_status));

    ia64_fr_write_sig(env, r1, ia64_pack_fp32_lanes(hi, lo));
}

static uint32_t ia64_fpcvt_lane(uint32_t value, bool is_unsigned,
                                bool is_trunc, float_status *status,
                                bool *fpa)
{
    float32 f = make_float32(value);
    float_status chop = *status;
    uint32_t result;
    uint32_t truncated;

    /* A denormal lane is an unnormal operand: D (SDM Vol 1 5.4.1.2). */
    if (float32_is_denormal(f)) {
        float_raise(float_flag_input_denormal_used, status);
    }
    if (is_unsigned) {
        result = is_trunc ?
            float32_to_uint32_round_to_zero(f, status) :
            float32_to_uint32(f, status);
        truncated = float32_to_uint32_round_to_zero(f, &chop);
    } else {
        result = is_trunc ?
            (uint32_t)float32_to_int32_round_to_zero(f, status) :
            (uint32_t)float32_to_int32(f, status);
        truncated = float32_to_int32_round_to_zero(f, &chop);
    }
    /* A result other than the truncated one grew in magnitude. */
    *fpa = result != truncated;
    /*
     * An unrepresentable lane is the 32-bit integer indefinite, not a
     * saturated value (SDM Vol 3 fpcvt.fx).
     */
    return (get_float_exception_flags(status) & float_flag_invalid) ?
           0x80000000U : result;
}

/* An enabled Inexact trap of one result, with its ISR.fpa (Figure 5-12). */
static uint32_t ia64_fp_inexact_trap(uint64_t traps, uint64_t flags,
                                     bool fpa)
{
    if (!(flags & IA64_FP_FLAG_I) || (traps & IA64_FP_FLAG_I)) {
        return 0;
    }
    return IA64_FP_TRAP_I | (fpa ? IA64_FP_TRAP_FPA : 0);
}

static void ia64_do_fpcvt(CPUIA64State *env, uint32_t r1, uint32_t r2,
                          uint32_t is_unsigned, uint32_t is_trunc,
                          uint32_t sf)
{
    uint64_t f2;
    float_status hi_status = env->fp.fp_status;
    float_status lo_status = env->fp.fp_status;
    uint64_t traps = ia64_fp_active_traps(env, sf);
    uint64_t hi_flags;
    uint64_t lo_flags;
    uint32_t hi;
    uint32_t lo;
    bool hi_fpa;
    bool lo_fpa;

    if (ia64_fr_nat_get(env, r2)) {
        ia64_fr_write_nat(env, r1);
        return;
    }

    f2 = ia64_fr_significand(env, r2);
    hi = ia64_fpcvt_lane(f2 >> 32, is_unsigned != 0,
                         is_trunc != 0, &hi_status, &hi_fpa);
    lo = ia64_fpcvt_lane(f2, is_unsigned != 0,
                         is_trunc != 0, &lo_status, &lo_fpa);
    hi_flags = ia64_fp_soft_flags_to_ia64(
        get_float_exception_flags(&hi_status));
    lo_flags = ia64_fp_soft_flags_to_ia64(
        get_float_exception_flags(&lo_status));
    ia64_fp_simd_end(env, sf, hi_flags, lo_flags,
                     ia64_fp_inexact_trap(traps, hi_flags, hi_fpa),
                     ia64_fp_inexact_trap(traps, lo_flags, lo_fpa));

    ia64_fr_write_sig(env, r1, ia64_pack_fp32_lanes(hi, lo));
}

static IA64FPReg ia64_fp_single_to_reg(uint32_t bits)
{
    uint32_t exp = (bits >> 23) & 0xff;
    uint64_t frac = (uint64_t)(bits & 0x7fffff) << 40;
    IA64FPReg value = { .sign = bits >> 31 };

    if (exp == 0xff) {
        value.exp = IA64_FP_EXP_SPECIAL;
        value.sig = IA64_FP_INT_BIT | frac;
    } else if (exp == 0) {
        value.exp = frac ? IA64_FP_BIAS - 126 : 0;
        value.sig = frac;
    } else {
        value.exp = IA64_FP_BIAS - 127 + exp;
        value.sig = IA64_FP_INT_BIT | frac;
    }
    return value;
}

/*
 * The single-format lane of a rounded result.  The enabled O and U
 * responses keep the biased exponent modulo 2^8 (SDM Vol 1 5.4.2, 5.4.3).
 */
static uint32_t ia64_fp_rounded_to_single(const IA64FPRounded *rounded)
{
    uint32_t bits = (uint32_t)rounded->sign << 31;

    if (rounded->exp == IA64_FP_EXP_SPECIAL) {
        return bits | 0x7f800000;
    }
    if (!(rounded->sig & IA64_FP_INT_BIT)) {
        return bits | (uint32_t)(rounded->sig >> 40);
    }
    return bits | (((rounded->exp - IA64_FP_BIAS + 127) & 0xff) << 23) |
           ((uint32_t)(rounded->sig >> 40) & 0x7fffff);
}

/*
 * One lane of fpma, fpms or fpnma: the fma operand checks, the exact
 * result and one rounding in the single format (SDM Vol 3 fpma, fpms,
 * fpnma; Vol 1 Table 5-6).  The negations apply only to a numeric result.
 */
static uint32_t ia64_fpma_lane(uint32_t addend_bits, uint32_t multiplicand_bits,
                               uint32_t multiplier_bits, uint32_t form,
                               bool has_addend, const IA64FPFormat *fmt,
                               uint32_t *flags, uint32_t *trap)
{
    IA64FPReg a = ia64_fp_single_to_reg(multiplicand_bits);
    IA64FPReg b = ia64_fp_single_to_reg(multiplier_bits);
    IA64FPReg c = ia64_fp_single_to_reg(has_addend ? addend_bits : 0);
    bool negate_product = form == 2;
    bool negate_addend = form == 1;
    bool product_sign = a.sign ^ b.sign ^ negate_product;
    bool product_inf = ia64_fpr_is_inf(&a) || ia64_fpr_is_inf(&b);
    IA64FPExact exact;
    IA64FPRounded rounded;

    *flags = 0;
    *trap = 0;
    if (ia64_fpr_is_nan(&a) || ia64_fpr_is_nan(&b) ||
        ia64_fpr_is_nan(&c)) {
        if (ia64_fpr_is_snan(&a) || ia64_fpr_is_snan(&b) ||
            ia64_fpr_is_snan(&c)) {
            *flags = IA64_FP_FLAG_V;
        }
        /* NaN operand priority is f4, f2, f3 (Vol 1 5.4.6). */
        return (ia64_fpr_is_nan(&b) ? multiplier_bits :
                ia64_fpr_is_nan(&c) ? addend_bits :
                multiplicand_bits) | 0x00400000;
    }
    if ((product_inf && (a.sig == 0 || b.sig == 0)) ||
        (product_inf && ia64_fpr_is_inf(&c) &&
         product_sign != (c.sign ^ negate_addend))) {
        *flags = IA64_FP_FLAG_V;
        return 0xffc00000;
    }
    if (ia64_fpr_is_unnormal(&a) || ia64_fpr_is_unnormal(&b) ||
        ia64_fpr_is_unnormal(&c)) {
        *flags = IA64_FP_FLAG_D;
    }
    if (product_inf || ia64_fpr_is_inf(&c)) {
        return ((uint32_t)(product_inf ? product_sign :
                           c.sign ^ negate_addend) << 31) | 0x7f800000;
    }
    if (!ia64_fpa_muladd(&exact, &a, &b, has_addend ? &c : NULL,
                         negate_product, negate_addend, fmt->rc)) {
        return (uint32_t)exact.sign << 31;
    }
    ia64_fpa_round(&rounded, &exact, fmt);
    *flags |= rounded.flags;
    *trap = rounded.trap;
    return ia64_fp_rounded_to_single(&rounded);
}

static void ia64_do_fpma(CPUIA64State *env, uint32_t r1, uint32_t r2,
                         uint32_t r3, uint32_t r4, uint32_t form,
                         uint32_t sf)
{
    bool has_addend = r2 != IA64_FR_ZERO_INDEX;
    IA64FPFormat fmt;
    uint64_t f2;
    uint64_t f3;
    uint64_t f4;
    uint32_t hi_flags;
    uint32_t lo_flags;
    uint32_t hi_trap;
    uint32_t lo_trap;
    uint32_t hi;
    uint32_t lo;

    if (ia64_fr_write_nat_if_any3(env, r1, r2, r3, r4)) {
        return;
    }

    /* A pair of singles ignores the pc and wre controls (Table 5-6). */
    ia64_fpa_format(&fmt, ia64_fpsr_sf_controls(env, sf) & ~2ULL, 1,
                    ia64_fp_active_traps(env, sf));
    f2 = ia64_fr_significand(env, r2);
    f3 = ia64_fr_significand(env, r3);
    f4 = ia64_fr_significand(env, r4);
    hi = ia64_fpma_lane(f2 >> 32, f3 >> 32, f4 >> 32, form, has_addend, &fmt,
                        &hi_flags, &hi_trap);
    lo = ia64_fpma_lane(f2, f3, f4, form, has_addend, &fmt, &lo_flags,
                        &lo_trap);
    ia64_fp_simd_end(env, sf, hi_flags, lo_flags, hi_trap, lo_trap);

    ia64_fr_write_sig(env, r1, ia64_pack_fp32_lanes(hi, lo));
}

/* ---- FP status field controls ---- */

static uint32_t ia64_fpsr_sf_shift(uint32_t sf)
{
    return IA64_FPSR_SF0_SHIFT + (sf & 3) * IA64_FPSR_SF_BITS;
}

static uint64_t ia64_fpsr_sf_controls(const CPUIA64State *env, uint32_t sf)
{
    return (env->ar_fpsr >> ia64_fpsr_sf_shift(sf)) &
           IA64_FPSR_SF_CONTROLS_MASK;
}

static uint64_t ia64_fpsr_sf_flags(const CPUIA64State *env, uint32_t sf)
{
    return (env->ar_fpsr >>
            (ia64_fpsr_sf_shift(sf) + IA64_FPSR_SF_FLAGS_SHIFT)) &
           IA64_FPSR_SF_FLAGS_MASK;
}

static uint64_t ia64_fp_active_traps(CPUIA64State *env, uint32_t sf)
{
    uint64_t controls = ia64_fpsr_sf_controls(env, sf);

    return (controls & IA64_FPSR_SF_TD) ?
           IA64_FPSR_TRAPS_MASK :
           (env->ar_fpsr & IA64_FPSR_TRAPS_MASK);
}

static uint64_t ia64_fp_soft_flags_to_ia64(int soft)
{
    uint64_t flags = 0;

    if (soft & float_flag_invalid) {
        flags |= 1U << 0;
    }
    if (soft & (float_flag_input_denormal_flushed |
                float_flag_input_denormal_used)) {
        flags |= 1U << 1;
    }
    if (soft & float_flag_divbyzero) {
        flags |= 1U << 2;
    }
    if (soft & float_flag_overflow) {
        flags |= 1U << 3;
    }
    if (soft & (float_flag_underflow |
                float_flag_output_denormal_flushed)) {
        flags |= 1U << 4;
    }
    if (soft & float_flag_inexact) {
        flags |= 1U << 5;
    }

    return flags;
}

/*
 * A parallel FP fault reports the high lane in ISR.code{3:0} and the low
 * lane in ISR.code{7:4} (SDM Vol 2 Floating-point Fault vector); a trap
 * reports them in ISR.code{14:11} and {10:7} (Vol 2 Table 8-3).  The lane
 * trap bits arrive in the high-lane positions.
 */
static void ia64_fp_simd_end(CPUIA64State *env, uint32_t sf,
                             uint32_t hi_flags, uint32_t lo_flags,
                             uint32_t hi_trap, uint32_t lo_trap)
{
    uint64_t traps = ia64_fp_active_traps(env, sf);
    uint64_t hi_fault = hi_flags & ~traps & 0x7;
    uint64_t lo_fault = lo_flags & ~traps & 0x7;

    if (hi_fault || lo_fault) {
        ia64_raise_fp_fault(env, hi_fault | (lo_fault << 4));
    }
    env->fp.transaction.flags |= hi_flags | lo_flags;
    env->fp.transaction.trap |= hi_trap | (lo_trap >> 4);
}

static void ia64_fp_simd_fault_end(CPUIA64State *env, uint32_t sf,
                                   int hi_soft, int lo_soft)
{
    ia64_fp_simd_end(env, sf, ia64_fp_soft_flags_to_ia64(hi_soft),
                     ia64_fp_soft_flags_to_ia64(lo_soft), 0, 0);
}

static void ia64_fp_restore_state(CPUIA64State *env)
{
    uint32_t word;

    for (word = 0; word < 2; word++) {
        uint64_t pending = env->fp.transaction.backup_fr_mask[word];

        while (pending) {
            uint32_t reg = word * 64 + ctz64(pending);
            uint64_t bit = 1ULL << (reg % 64);

            pending &= pending - 1;
            env->fp.fr[reg] = env->fp.transaction.backup_fr[reg];
            env->fp.fr_ext_mant[reg] =
                env->fp.transaction.backup_fr_ext_mant[reg];
            env->fp.fr_ext_exp[reg] =
                env->fp.transaction.backup_fr_ext_exp[reg];
            env->fp.fr_int_value[reg] =
                env->fp.transaction.backup_fr_int_value[reg];
            env->fp.fr_nat[word] = (env->fp.fr_nat[word] & ~bit) |
                                (env->fp.transaction.backup_fr_nat[word] & bit);
            env->fp.fr_sig[word] = (env->fp.fr_sig[word] & ~bit) |
                                (env->fp.transaction.backup_fr_sig[word] & bit);
            env->fp.fr_ext_sign[word] =
                (env->fp.fr_ext_sign[word] & ~bit) |
                (env->fp.transaction.backup_fr_ext_sign[word] & bit);
            env->fp.fr_ext_valid[word] =
                (env->fp.fr_ext_valid[word] & ~bit) |
                (env->fp.transaction.backup_fr_ext_valid[word] & bit);
            env->fp.fr_int_origin[word] =
                (env->fp.fr_int_origin[word] & ~bit) |
                (env->fp.transaction.backup_fr_int_origin[word] & bit);
        }
    }
    while (env->fp.transaction.backup_pr_mask) {
        uint32_t reg = ctz64(env->fp.transaction.backup_pr_mask);

        env->fp.transaction.backup_pr_mask &=
            env->fp.transaction.backup_pr_mask - 1;
        env->pr[reg] = env->fp.transaction.backup_pr[reg];
    }
    env->psr = (env->psr & ~(IA64_PSR_MFL | IA64_PSR_MFH)) |
               env->fp.transaction.backup_psr_mf;
    env->fp.transaction.active = false;
}

static void ia64_raise_fp_fault(CPUIA64State *env, uint64_t isr)
{
    uint32_t slot = (env->psr & IA64_PSR_RI_MASK) >>
                    IA64_PSR_RI_SHIFT;

    ia64_fp_restore_state(env);
    env->cr_isr = isr;
    ia64_raise_exception(env, IA64_EXCP_FP_FAULT, env->ip, 0, slot);
}

static void ia64_fp_begin(CPUIA64State *env, uint32_t sf,
                          uint32_t precision)
{
    static const FloatRoundMode rounding[4] = {
        float_round_nearest_even,
        float_round_down,
        float_round_up,
        float_round_to_zero,
    };
    uint64_t controls = ia64_fpsr_sf_controls(env, sf);
    uint32_t pc = (controls >> 2) & 3;
    FloatX80RoundPrec round_precision;

    env->fp.transaction.backup_fr_mask[0] = 0;
    env->fp.transaction.backup_fr_mask[1] = 0;
    env->fp.transaction.backup_pr_mask = 0;
    env->fp.transaction.backup_psr_mf =
        env->psr & (IA64_PSR_MFL | IA64_PSR_MFH);
    env->fp.transaction.flags = 0;
    env->fp.transaction.trap = 0;
    env->fp.transaction.active = true;

    set_float_rounding_mode(rounding[(controls >> 4) & 3],
                            &env->fp.fp_status);
    if (precision == 1 || (precision == 0 && pc == 0)) {
        round_precision = floatx80_precision_s;
    } else if (precision == 2 || (precision == 0 && pc == 2)) {
        round_precision = floatx80_precision_d;
    } else {
        round_precision = floatx80_precision_x;
    }
    set_floatx80_rounding_precision(round_precision, &env->fp.fp_status);
    set_flush_to_zero(controls & 1, &env->fp.fp_status);
    set_flush_inputs_to_zero(false, &env->fp.fp_status);
    set_float_exception_flags(0, &env->fp.fp_status);
}

/*
 * Record the flags of the finished instruction in FPSR.sfX and deliver its
 * trap, if any.  Traps arrive with the instruction complete (SDM Vol 1
 * 5.4.1.3).
 */
static void ia64_fp_commit(CPUIA64State *env, uint32_t sf, uint64_t flags,
                           uint32_t trap_code)
{
    uint32_t shift;

    flags |= env->fp.transaction.flags;
    trap_code |= env->fp.transaction.trap;
    if (flags == 0 && trap_code == 0) {
        return;
    }

    shift = ia64_fpsr_sf_shift(sf) + IA64_FPSR_SF_FLAGS_SHIFT;
    env->ar_fpsr |= flags << shift;

    if (trap_code) {
        uint32_t slot = (env->psr & IA64_PSR_RI_MASK) >>
                        IA64_PSR_RI_SHIFT;
        uint64_t trap_ip = env->ip;
        uint64_t next_ip = env->ip;
        uint32_t next_slot;

        if (slot < 2) {
            next_slot = slot + 1;
        } else {
            next_ip += 16;
            next_slot = 0;
        }
        env->psr = (env->psr & ~IA64_PSR_RI_MASK) |
                   ((uint64_t)next_slot << IA64_PSR_RI_SHIFT);
        /* A concurrent Single Step trap shows in ISR.code (Table 8-3). */
        env->cr_isr = trap_code | IA64_ISR_CODE_FP |
                      ((env->psr & IA64_PSR_SS) ? IA64_ISR_CODE_SS : 0);
        ia64_raise_exception(env, IA64_EXCP_FP_TRAP, next_ip, trap_ip,
                               slot);
    }
}

static void ia64_fp_end(CPUIA64State *env, uint32_t sf)
{
    int soft = get_float_exception_flags(&env->fp.fp_status);
    uint64_t flags = 0;
    uint64_t enabled = 0;

    if (soft != 0) {
        flags = ia64_fp_soft_flags_to_ia64(soft);
        enabled = flags & ~ia64_fp_active_traps(env, sf);
        if (enabled & 0x7) {
            ia64_raise_fp_fault(env, enabled & 0x7);
        }
    }
    env->fp.transaction.active = false;
    ia64_fp_commit(env, sf, flags, (enabled & 0x38) << 8);
}

/*
 * For the instructions computed without softfloat that write their results
 * after the last fault check (Vol 1 Figure 5-11): the multiply-add family
 * (fma, fms, fnma and their pseudo-ops), frcpa and fcvt.fx.  They need
 * neither the softfloat set-up nor a backup of their targets; a V, D, Z or
 * SWA fault only restores PSR.mfl/mfh, which no write has changed yet.
 */
static void ia64_fp_begin_native(CPUIA64State *env)
{
    env->fp.transaction.backup_fr_mask[0] = 0;
    env->fp.transaction.backup_fr_mask[1] = 0;
    env->fp.transaction.backup_pr_mask = 0;
    env->fp.transaction.backup_psr_mf =
        env->psr & (IA64_PSR_MFL | IA64_PSR_MFH);
    env->fp.transaction.flags = 0;
    env->fp.transaction.trap = 0;
}

void ia64_fp_fsetc(CPUIA64State *env, uint32_t sf,
                  uint32_t amask7, uint32_t omask7)
{
    uint64_t controls = (ia64_fpsr_sf_controls(env, 0) & (amask7 & 0x7f)) |
                        (omask7 & 0x7f);
    uint64_t shift = ia64_fpsr_sf_shift(sf);

    if (((sf & 3) == 0 && (controls & IA64_FPSR_SF_TD)) ||
        (((controls >> 2) & 3) == 1)) {
        uint32_t slot = (env->psr & IA64_PSR_RI_MASK) >>
                        IA64_PSR_RI_SHIFT;

        env->cr_isr = 0x30;
        ia64_raise_exception(env, IA64_EXCP_RESERVED_REG_FIELD,
                               env->ip, 0, slot);
    }

    env->ar_fpsr &= ~(IA64_FPSR_SF_CONTROLS_MASK << shift);
    env->ar_fpsr |= controls << shift;
}

void ia64_fp_fclrf(CPUIA64State *env, uint32_t sf)
{
    uint64_t mask = IA64_FPSR_SF_FLAGS_MASK <<
                    (ia64_fpsr_sf_shift(sf) + IA64_FPSR_SF_FLAGS_SHIFT);

    env->ar_fpsr &= ~mask;
}

uint64_t ia64_fp_fchkf(CPUIA64State *env, uint32_t sf)
{
    uint64_t traps = env->ar_fpsr & IA64_FPSR_TRAPS_MASK;
    uint64_t sf0_flags = ia64_fpsr_sf_flags(env, 0);
    uint64_t flags = ia64_fpsr_sf_flags(env, sf);

    return (flags & ~traps) || (flags & ~sf0_flags);
}

/* ---- FP bitwise select ---- */

void ia64_fp_fselect(CPUIA64State *env, uint32_t r1,
                     uint32_t r2, uint32_t r3, uint32_t r4)
{
    uint64_t mask;

    /*
     * IA-64 fselect: f1 = (f3 AND f2) OR (f4 AND NOT f2)
     * For each bit position, if f2 bit is 1, select from f3; else from f4.
     */
    if (ia64_fr_write_nat_if_any3(env, r1, r2, r3, r4)) {
        return;
    }
    mask = ia64_fr_significand(env, r2);
    ia64_fr_write_sig(env, r1,
                      (ia64_fr_significand(env, r3) & mask) |
                      (ia64_fr_significand(env, r4) & ~mask));
}

void ia64_fp_fcvt_xf(CPUIA64State *env, uint32_t r1, uint32_t r2)
{
    uint64_t value;
    uint64_t magnitude;
    uint32_t shift;
    bool sign;

    if (ia64_fr_nat_get(env, r2)) {
        ia64_fr_write_nat(env, r1);
        return;
    }

    /*
     * fcvt.xf produces register-file precision, is always exact, and is
     * independent of FPSR rounding controls.  Building the register-format
     * value directly also prevents it from inheriting SoftFloat's precision
     * from a preceding status-field-controlled instruction.
     */
    value = ia64_fr_significand(env, r2);
    if (value == 0) {
        ia64_fr_write_ext(env, r1, false, 0, 0);
        return;
    }

    sign = (int64_t)value < 0;
    magnitude = sign ? -value : value;
    shift = clz64(magnitude);
    ia64_fr_write_ext(env, r1, sign, IA64_FP_REG_INTEGER_EXP - shift,
                      magnitude << shift);
}

#define IA64_FP_INTEGER_INDEFINITE 0x8000000000000000ULL

static void ia64_do_fcvt_fx(CPUIA64State *env, uint32_t r1, uint32_t r2,
                            uint32_t is_unsigned, uint32_t is_trunc,
                            uint32_t sf)
{
    IA64FPReg value;
    uint64_t result;
    uint32_t rc;
    bool inexact;
    bool fpa;

    if (ia64_fr_nat_get(env, r2)) {
        ia64_fr_write_nat(env, r1);
        return;
    }

    /*
     * A NaN, an infinity, an unsupported operand or a result that does not
     * fit is invalid and gives the integer indefinite; that check comes
     * before D (SDM Vol 3 fcvt.fx, Vol 1 5.4.4 and Figure 5-11).
     */
    value = ia64_fr_reg(env, r2);
    rc = is_trunc ? 3 : (ia64_fpsr_sf_controls(env, sf) >> 4) & 3;
    if (ia64_fpr_is_special(&value) ||
        !ia64_fpa_to_integer(&value, !is_unsigned, rc, &result, &inexact,
                             &fpa)) {
        ia64_fp_raise_flag(env, sf, IA64_FP_FLAG_V);
        ia64_fr_write_sig(env, r1, IA64_FP_INTEGER_INDEFINITE);
        return;
    }
    if (ia64_fpr_is_unnormal(&value)) {
        ia64_fp_raise_flag(env, sf, IA64_FP_FLAG_D);
    }
    if (inexact) {
        env->fp.transaction.flags |= IA64_FP_FLAG_I;
        env->fp.transaction.trap |= ia64_fp_inexact_trap(
            ia64_fp_active_traps(env, sf), IA64_FP_FLAG_I, fpa);
    }
    ia64_fr_write_sig(env, r1, result);
}

uint64_t ia64_fp_getf(CPUIA64State *env, uint32_t reg, uint32_t kind)
{
    uint64_t low;
    uint64_t high;

    if (kind == 0) {
        return ia64_fpreg_to_binary64(env, reg);
    }
    if (kind == 1) {
        return ia64_fpreg_to_binary32(env, reg);
    }
    ia64_fpreg_to_spill(env, reg, &low, &high);
    return kind == 2 ? low : high;
}

void ia64_fp_setf_exp(CPUIA64State *env, uint32_t reg, uint64_t value)
{
    ia64_fr_write_ext(env, reg, (value >> 17) & 1,
                      value & 0x1ffff,
                      IA64_FP_SIGNIFICAND_INTEGER_BIT);
}

void ia64_fp_setf_s(CPUIA64State *env, uint32_t reg, uint64_t value)
{
    ia64_fpreg_from_binary32(env, reg, value);
}

void ia64_fp_fmov(CPUIA64State *env, uint32_t dst, uint32_t src)
{
    ia64_fr_copy(env, dst, src, 1);
}

/* ---- FP reciprocal sqrt approx ---- */

static bool ia64_float32_rsqrta_predicate(float32 val)
{
    return !float32_is_zero(val) &&
           !float32_is_neg(val) &&
           !float32_is_infinity(val) &&
           !float32_is_any_nan(val);
}

static float32 ia64_float32_rsqrta(float32 val, float_status *status)
{
    float32 sqrtv = float32_sqrt(val, status);

    if (float32_is_zero(sqrtv)) {
        return float32_set_sign(float32_infinity, float32_is_neg(val));
    }
    return float32_div(float32_one, sqrtv, status);
}

static float32 ia64_float32_rsqrta_approx(
    CPUIA64State *env, const IA64FPRegisterFormat *val)
{
    IA64FPRegisterFormat result = {
        .sign = false,
        .exp = ia64_fp_register_rsqrt_exp(val->exp),
        .mant = (uint64_t)ia64_recip_sqrt_table[
            ia64_recip_sqrt_table_index(val)] << 53,
    };

    return ia64_float32_from_register_format(env, &result);
}

static bool ia64_float32_fprsqrta_predicate(
    const IA64FPRegisterFormat *val)
{
    return val->exp >
           IA64_FP_WRE_BIAS - IA64_FP_SINGLE_BIAS +
           IA64_FP_SINGLE_MANT_WIDTH;
}

/*
 * A positive denormal lane is normalized, sets D and takes the table
 * approximation (SDM Vol 3 fprsqrta, fp_ieee_recip_sqrt).
 */
static float32 ia64_fprsqrta_lane(CPUIA64State *env, float32 val, bool *pred,
                                  float_status *status)
{
    IA64FPRegisterFormat fmt = { 0 };

    if (ia64_float32_rsqrta_predicate(val) &&
        ia64_float32_normalized_register_format(val, &fmt)) {
        if (float32_is_denormal(val)) {
            float_raise(float_flag_input_denormal_used, status);
        }
        *pred = ia64_float32_fprsqrta_predicate(&fmt);
        return ia64_float32_rsqrta_approx(env, &fmt);
    }
    *pred = false;
    return ia64_float32_rsqrta(val, status);
}

static void ia64_do_fprsqrta(CPUIA64State *env, uint32_t r1, uint32_t p2,
                             uint32_t r3, uint32_t sf)
{
    uint64_t val = ia64_fr_significand(env, r3);
    float_status hi_status = env->fp.fp_status;
    float_status lo_status = env->fp.fp_status;
    bool hi_pred;
    bool lo_pred;
    float32 hi_result;
    float32 lo_result;
    int hi_soft;
    int lo_soft;

    if (ia64_fr_nat_get(env, r3)) {
        ia64_fr_write_nat(env, r1);
        ia64_pr_write(env, p2, false);
        return;
    }

    hi_result = ia64_fprsqrta_lane(env, make_float32(val >> 32), &hi_pred,
                                   &hi_status);
    lo_result = ia64_fprsqrta_lane(env, make_float32(val), &lo_pred,
                                   &lo_status);
    hi_soft = get_float_exception_flags(&hi_status);
    lo_soft = get_float_exception_flags(&lo_status);

    {
        uint64_t traps = ia64_fp_active_traps(env, sf);
        uint64_t hi_fault = ia64_fp_soft_flags_to_ia64(hi_soft) & ~traps & 0x7;
        uint64_t lo_fault = ia64_fp_soft_flags_to_ia64(lo_soft) & ~traps & 0x7;

        set_float_exception_flags(hi_soft | lo_soft, &env->fp.fp_status);
        if (hi_fault || lo_fault) {
            ia64_raise_fp_fault(env, hi_fault | (lo_fault << 4));
        }
    }

    ia64_fr_write_sig(env, r1,
                      ((uint64_t)float32_val(hi_result) << 32) |
                      float32_val(lo_result));
    ia64_pr_write(env, p2, hi_pred && lo_pred);
}

/*
 * frsqrta gives the IEEE square root for special operands with p2 = 0,
 * and otherwise the table approximation of 1/sqrt(f3) with p2 = 1 (SDM
 * Vol 3 frsqrta).  The checks follow Vol 1 Figure 5-11.
 */
static void ia64_do_frsqrta(CPUIA64State *env, uint32_t r1, uint32_t p2,
                            uint32_t r3, uint32_t sf)
{
    IA64FPRegisterFormat fmt;
    IA64FPReg val;
    IA64FPReg result;

    if (ia64_fr_nat_get(env, r3)) {
        ia64_fr_write_nat(env, r1);
        ia64_pr_write(env, p2, false);
        return;
    }

    val = ia64_fr_reg(env, r3);
    if (ia64_fpr_is_unsupported(&val) ||
        (val.sign && !ia64_fpr_is_nan(&val) &&
         !ia64_fpr_is_zero_value(&val))) {
        ia64_fp_raise_flag(env, sf, IA64_FP_FLAG_V);
        ia64_fr_write_reg(env, r1, ia64_fp_qnan_indefinite);
        ia64_pr_write(env, p2, false);
        return;
    }
    if (ia64_fpr_is_nan(&val)) {
        if (ia64_fpr_is_snan(&val)) {
            ia64_fp_raise_flag(env, sf, IA64_FP_FLAG_V);
        }
        val.sig |= IA64_FP_QUIET_BIT;
        ia64_fr_write_reg(env, r1, val);
        ia64_pr_write(env, p2, false);
        return;
    }

    fmt = ia64_fr_register_format(env, r3);
    if (ia64_frsqrta_limits_swa_fault(&fmt)) {
        ia64_fpswa_prepare_sqrt(env, r1, p2, r3, sf);
        ia64_raise_fp_fault(env, IA64_FP_ISR_SWA);
    }
    if (ia64_fpr_is_unnormal(&val)) {
        ia64_fp_raise_flag(env, sf, IA64_FP_FLAG_D);
    }

    if (ia64_fpr_is_zero_value(&val) || ia64_fpr_is_inf(&val)) {
        ia64_fr_write_reg(env, r1,
                          ia64_fp_special_reg(val.sign,
                                              ia64_fpr_is_inf(&val)));
        ia64_pr_write(env, p2, false);
        return;
    }
    result.sign = false;
    result.exp = ia64_fp_register_rsqrt_exp(fmt.exp);
    result.sig = (uint64_t)ia64_recip_sqrt_table[
        ia64_recip_sqrt_table_index(&fmt)] << 53;
    ia64_fr_write_reg(env, r1, result);
    ia64_pr_write(env, p2, true);
}

static uint32_t ia64_fp_context_sf(uint32_t context)
{
    return context & IA64_FP_CONTEXT_SF_MASK;
}

static uint32_t ia64_fp_context_precision(uint32_t context)
{
    return context >> IA64_FP_CONTEXT_PREC_SHIFT;
}

static void ia64_fp_begin_context(CPUIA64State *env, uint32_t context)
{
    ia64_fp_begin(env, ia64_fp_context_sf(context),
                  ia64_fp_context_precision(context));
}

static void ia64_fp_end_context(CPUIA64State *env, uint32_t context)
{
    ia64_fp_end(env, ia64_fp_context_sf(context));
}

static void ia64_fp_muladd_insn(CPUIA64State *env, uint32_t r1, uint32_t f3,
                                uint32_t f4, uint32_t f2, bool negate_product,
                                bool negate_addend, uint32_t context)
{
    uint32_t sf = ia64_fp_context_sf(context);

    ia64_fp_begin_native(env);
    ia64_fp_muladd(env, r1, f3, f4, f2, negate_product, negate_addend, sf,
                   ia64_fp_context_precision(context));
    ia64_fp_commit(env, sf, 0, 0);
}

void ia64_fp_fadd(CPUIA64State *env, uint32_t r1, uint32_t r2, uint32_t r3,
                 uint32_t context)
{
    ia64_fp_muladd_insn(env, r1, r3, IA64_FR_ONE_INDEX, r2, false, false,
                        context);
}

void ia64_fp_fsub(CPUIA64State *env, uint32_t r1, uint32_t r2, uint32_t r3,
                 uint32_t context)
{
    ia64_fp_muladd_insn(env, r1, r2, IA64_FR_ONE_INDEX, r3, false, true,
                        context);
}

void ia64_fp_fmpy(CPUIA64State *env, uint32_t r1, uint32_t r2, uint32_t r3,
                 uint32_t context)
{
    ia64_fp_muladd_insn(env, r1, r2, r3, IA64_FR_ZERO_INDEX, false, false,
                        context);
}

void ia64_fp_fma4(CPUIA64State *env, uint32_t r1, uint32_t r2, uint32_t r3,
                 uint32_t r4, uint32_t context)
{
    ia64_fp_muladd_insn(env, r1, r3, r4, r2, false, false, context);
}

void ia64_fp_fcmp(CPUIA64State *env, uint32_t p1, uint32_t p2, uint32_t r2,
                 uint32_t r3, uint32_t cond_code, uint32_t context)
{
    ia64_fp_begin_context(env, context);
    ia64_do_fcmp(env, p1, p2, r2, r3, cond_code,
                 ia64_fp_context_sf(context));
    ia64_fp_end_context(env, context);
}

#define IA64_DEFINE_FP_BINARY_WRAPPER(name)                                \
    void ia64_fp_##name(CPUIA64State *env, uint32_t r1, uint32_t r2,        \
                       uint32_t r3, uint32_t context)                       \
    {                                                                       \
        ia64_fp_begin_context(env, context);                                \
        ia64_do_##name(env, r1, r2, r3, ia64_fp_context_sf(context));      \
        ia64_fp_end_context(env, context);                                  \
    }

IA64_DEFINE_FP_BINARY_WRAPPER(fmin)
IA64_DEFINE_FP_BINARY_WRAPPER(fmax)
IA64_DEFINE_FP_BINARY_WRAPPER(famin)
IA64_DEFINE_FP_BINARY_WRAPPER(famax)

#undef IA64_DEFINE_FP_BINARY_WRAPPER

void ia64_fp_frcpa(CPUIA64State *env, uint32_t r1, uint32_t p2, uint32_t r2,
                  uint32_t r3, uint32_t context)
{
    uint32_t sf = ia64_fp_context_sf(context);

    ia64_fp_begin_native(env);
    ia64_do_frcpa(env, r1, p2, r2, r3, sf);
    ia64_fp_commit(env, sf, 0, 0);
}

void ia64_fp_fprcpa(CPUIA64State *env, uint32_t r1, uint32_t p2, uint32_t r2,
                   uint32_t r3, uint32_t context)
{
    ia64_fp_begin_context(env, context);
    ia64_do_fprcpa(env, r1, p2, r2, r3, ia64_fp_context_sf(context));
    ia64_fp_end_context(env, context);
}

void ia64_fp_fpminmax(CPUIA64State *env, uint32_t r1, uint32_t r2,
                     uint32_t r3, uint32_t is_max, uint32_t is_abs,
                     uint32_t context)
{
    ia64_fp_begin_context(env, context);
    ia64_do_fpminmax(env, r1, r2, r3, is_max, is_abs,
                     ia64_fp_context_sf(context));
    ia64_fp_end_context(env, context);
}

void ia64_fp_fpcmp(CPUIA64State *env, uint32_t r1, uint32_t r2, uint32_t r3,
                  uint32_t frel, uint32_t context)
{
    ia64_fp_begin_context(env, context);
    ia64_do_fpcmp(env, r1, r2, r3, frel, ia64_fp_context_sf(context));
    ia64_fp_end_context(env, context);
}

void ia64_fp_fpcvt(CPUIA64State *env, uint32_t r1, uint32_t r2,
                  uint32_t is_unsigned, uint32_t is_trunc,
                  uint32_t context)
{
    ia64_fp_begin_context(env, context);
    ia64_do_fpcvt(env, r1, r2, is_unsigned, is_trunc,
                  ia64_fp_context_sf(context));
    ia64_fp_end_context(env, context);
}

void ia64_fp_fpma(CPUIA64State *env, uint32_t r1, uint32_t r2, uint32_t r3,
                 uint32_t r4, uint32_t form, uint32_t context)
{
    ia64_fp_begin_context(env, context);
    ia64_do_fpma(env, r1, r2, r3, r4, form,
                 ia64_fp_context_sf(context));
    ia64_fp_end_context(env, context);
}

void ia64_fp_fms(CPUIA64State *env, uint32_t r1, uint32_t r2, uint32_t r3,
                uint32_t r4, uint32_t context)
{
    ia64_fp_muladd_insn(env, r1, r3, r4, r2, false, true, context);
}

void ia64_fp_fnma4(CPUIA64State *env, uint32_t r1, uint32_t r2, uint32_t r3,
                  uint32_t r4, uint32_t context)
{
    ia64_fp_muladd_insn(env, r1, r3, r4, r2, true, false, context);
}

void ia64_fp_fnorm(CPUIA64State *env, uint32_t r1, uint32_t r2, uint32_t r3,
                  uint32_t context)
{
    ia64_fp_muladd_insn(env, r1, r3, IA64_FR_ONE_INDEX, IA64_FR_ZERO_INDEX,
                        false, false, context);
}

void ia64_fp_fcvt_fx(CPUIA64State *env, uint32_t r1, uint32_t r2,
                    uint32_t is_unsigned, uint32_t is_trunc,
                    uint32_t context)
{
    uint32_t sf = ia64_fp_context_sf(context);

    ia64_fp_begin_native(env);
    ia64_do_fcvt_fx(env, r1, r2, is_unsigned, is_trunc, sf);
    ia64_fp_commit(env, sf, 0, 0);
}

void ia64_fp_fprsqrta(CPUIA64State *env, uint32_t r1, uint32_t p2,
                     uint32_t r3, uint32_t context)
{
    ia64_fp_begin_context(env, context);
    ia64_do_fprsqrta(env, r1, p2, r3, ia64_fp_context_sf(context));
    ia64_fp_end_context(env, context);
}

void ia64_fp_frsqrta(CPUIA64State *env, uint32_t r1, uint32_t p2,
                    uint32_t r3, uint32_t context)
{
    ia64_fp_begin_context(env, context);
    ia64_do_frsqrta(env, r1, p2, r3, ia64_fp_context_sf(context));
    ia64_fp_end_context(env, context);
}

static void ia64_fpswa_return(CPUIA64State *env, int64_t status,
                              uint64_t err1, uint64_t err2, uint64_t err3)
{
    env->gr[IA64_PAL_GR_STATUS] = status;
    env->gr[IA64_PAL_GR_RESULT1] = err1;
    env->gr[IA64_PAL_GR_RESULT2] = err2;
    env->gr[IA64_PAL_GR_RESULT3] = err3;
    trace_ia64_fpswa_return(env_cpu(env)->cpu_index, status, err1, err2,
                            err3);
}

static void ia64_fpswa_error(CPUIA64State *env, uint32_t number)
{
    ia64_fpswa_return(env, -1, (uint64_t)number << 56, 0, 0);
}

static bool ia64_fpswa_result_save_address(CPUIA64State *env,
                                           uint64_t state,
                                           uint32_t reg,
                                           uint64_t *address,
                                           uintptr_t ra)
{
    uint64_t mask;
    uint64_t base;
    uint32_t first;

    mask = ia64_ldq_data_ra(env, state + (reg >= 64 ? 8 : 0), ra);
    if (!(mask & (1ULL << (reg % 64)))) {
        *address = 0;
        return true;
    }

    if (reg < 6) {
        base = ia64_ldq_data_ra(env, state + 16, ra);
        first = 2;
    } else if (reg < 16) {
        base = ia64_ldq_data_ra(env, state + 24, ra);
        first = 6;
    } else if (reg < 32) {
        base = ia64_ldq_data_ra(env, state + 32, ra);
        first = 16;
    } else {
        base = ia64_ldq_data_ra(env, state + 40, ra);
        first = 32;
    }
    if (base == 0 || (base & 7)) {
        return false;
    }
    *address = base + (uint64_t)(reg - first) * 16;
    return true;
}

static uint32_t ia64_fpswa_current_fr(const CPUIA64State *env,
                                      uint32_t physical)
{
    uint32_t rrb;

    if (physical < IA64_ROTATING_FR_BASE) {
        return physical;
    }
    rrb = env->cfm_rrb_fr % IA64_ROTATING_FR_COUNT;
    return IA64_ROTATING_FR_BASE +
           ((physical - IA64_ROTATING_FR_BASE +
             IA64_ROTATING_FR_COUNT - rrb) % IA64_ROTATING_FR_COUNT);
}

void ia64_fp_fpswa_dispatch(CPUIA64State *env, uintptr_t ra)
{
    uint64_t trap_type = env->gr[IA64_FPSWA_GR_TRAP_TYPE];
    uint64_t bundle = env->gr[IA64_FPSWA_GR_BUNDLE];
    uint64_t ipsr_ptr = env->gr[IA64_FPSWA_GR_IPSR_PTR];
    uint64_t fpsr_ptr = env->gr[IA64_FPSWA_GR_FPSR_PTR];
    uint64_t isr_ptr = env->gr[IA64_FPSWA_GR_ISR_PTR];
    uint64_t preds_ptr = env->gr[IA64_FPSWA_GR_PREDS_PTR];
    uint64_t ifs_ptr = env->gr[IA64_FPSWA_GR_IFS_PTR];
    uint64_t state = env->gr[IA64_FPSWA_GR_STATE];
    uint64_t ipsr;
    uint64_t fpsr;
    uint64_t isr;
    uint64_t preds;
    uint64_t save_address;
    uint64_t enabled = 0;
    uint64_t flags;
    uint32_t reg;

    trace_ia64_fpswa_call(env_cpu(env)->cpu_index, trap_type, bundle,
                          env->fp.fpswa_pending);
    if (trap_type > 1 || bundle == 0 || ipsr_ptr == 0 || fpsr_ptr == 0 ||
        isr_ptr == 0 || preds_ptr == 0 || ifs_ptr == 0 || state == 0 ||
        (bundle & 7) || (ipsr_ptr & 7) || (fpsr_ptr & 7) ||
        (isr_ptr & 7) || (preds_ptr & 7) || (ifs_ptr & 7) || (state & 7)) {
        ia64_fpswa_error(env, 5);
        return;
    }

    isr = ia64_ldq_data_ra(env, isr_ptr, ra);
    if (!env->fp.fpswa_pending) {
        if (trap_type == 1 && (isr & IA64_FP_ISR_SWA)) {
            ia64_fpswa_error(env, 6);
        } else {
            ia64_fpswa_return(env, 1, 0, 0, 0);
        }
        return;
    }
    if (trap_type != 1 || !(isr & IA64_FP_ISR_SWA)) {
        ia64_fpswa_error(env, 7);
        return;
    }

    fpsr = ia64_ldq_data_ra(env, fpsr_ptr, ra);
    if ((env->fp.fpswa_flags & (1U << 1)) &&
        !ia64_fpswa_exception_masked(fpsr, env->fp.fpswa_sf, 1U << 1)) {
        isr = (isr & ~0xffffULL) | (1U << 1);
        ia64_stq_data_ra(env, isr_ptr, isr, ra);
        env->fp.fpswa_pending = false;
        ia64_fpswa_return(env, 1, 0, 0, 0);
        return;
    }

    reg = env->fp.fpswa_dest_fr;
    if (reg <= 1 || reg >= IA64_FR_COUNT ||
        !ia64_fpswa_result_save_address(env, state, reg,
                                        &save_address, ra)) {
        ia64_fpswa_error(env, 8);
        return;
    }
    if (save_address != 0) {
        ia64_stq_data_ra(env, save_address, env->fp.fpswa_result_low, ra);
        ia64_stq_data_ra(env, save_address + 8,
                         env->fp.fpswa_result_high, ra);
    } else {
        uint32_t current = ia64_fpswa_current_fr(env, reg);

        ia64_fpreg_from_spill(env, current, env->fp.fpswa_result_low,
                              env->fp.fpswa_result_high);
    }

    preds = ia64_ldq_data_ra(env, preds_ptr, ra);
    preds &= ~(1ULL << env->fp.fpswa_dest_pr);
    preds |= 1;
    ia64_stq_data_ra(env, preds_ptr, preds, ra);

    ipsr = ia64_ldq_data_ra(env, ipsr_ptr, ra);
    ipsr |= reg < 32 ? IA64_PSR_MFL : IA64_PSR_MFH;
    ia64_stq_data_ra(env, ipsr_ptr, ipsr, ra);

    flags = env->fp.fpswa_flags & IA64_FPSR_SF_FLAGS_MASK;
    fpsr |= flags << (ia64_fpsr_sf_shift(env->fp.fpswa_sf) +
                      IA64_FPSR_SF_FLAGS_SHIFT);
    ia64_stq_data_ra(env, fpsr_ptr, fpsr, ra);

    for (reg = 3; reg <= 5; reg++) {
        uint64_t flag = 1ULL << reg;

        if ((flags & flag) &&
            !ia64_fpswa_exception_masked(fpsr, env->fp.fpswa_sf, flag)) {
            enabled |= flag;
        }
    }
    env->fp.fpswa_pending = false;
    if (enabled != 0) {
        isr = (isr & ~0xffffULL) | 1 | (enabled << 8);
        if (env->fp.fpswa_fpa) {
            isr |= 1U << 14;
        }
        ia64_stq_data_ra(env, isr_ptr, isr, ra);
        ia64_fpswa_return(env, 3, 0, 0, 0);
    } else {
        ia64_fpswa_return(env, 0, 0, 0, 0);
    }
}


/* ---- FP pack ---- */

void ia64_fp_fpack(CPUIA64State *env, uint32_t r1, uint32_t r2, uint32_t r3)
{
    if (ia64_fr_nat_get(env, r2) || ia64_fr_nat_get(env, r3)) {
        ia64_fr_write_nat(env, r1);
        return;
    }

    /*
     * fp_single is the register to single memory format translation of
     * stfs (SDM Vol 3 fpack Figure 2-14, Vol 1 Figure 5-7): it moves bits
     * and does not round.
     */
    ia64_fr_write_sig(env, r1,
                      ((uint64_t)ia64_fpreg_to_binary32(env, r2) << 32) |
                      ia64_fpreg_to_binary32(env, r3));
}

static bool ia64_probe_writeback_ram(CPUIA64State *env, uint64_t addr,
                                     MMUAccessType access_type,
                                     bool *direct, uintptr_t ra)
{
    return ia64_exec_probe_writeback_ram(env, addr, 10, access_type,
                                         direct, ra);
}

static Int128 ia64_ld16_raw(CPUIA64State *env, uint64_t addr, uintptr_t ra)
{
    int mmu_idx = ia64_exec_mmu_index(env, false);
    MemOpIdx oi = make_memop_idx(MO_LE | MO_UO, mmu_idx);

    return ia64_exec_load_16(env, addr, oi, ra);
}

static void ia64_st10_atomic(CPUIA64State *env, uint64_t addr,
                             uint64_t mant, uint16_t ext, uintptr_t ra)
{
#if HAVE_CMPXCHG128
    int mmu_idx = ia64_exec_mmu_index(env, false);
    MemOpIdx oi = make_memop_idx(MO_LE | MO_UO, mmu_idx);
    Int128 observed = ia64_ld16_raw(env, addr, ra);
    uint64_t raw_mant = ia64_data_big_endian(env) ? bswap64(mant) : mant;
    uint16_t raw_ext = ia64_data_big_endian(env) ? bswap16(ext) : ext;
    uint64_t low = ia64_data_big_endian(env) ?
                   (raw_mant << 16) | raw_ext : mant;

    for (;;) {
        Int128 expected = observed;
        uint64_t high = int128_gethi(expected);
        Int128 desired;

        if (ia64_data_big_endian(env)) {
            high = (high & ~UINT64_C(0xffff)) | (raw_mant >> 48);
        } else {
            high = (high & ~UINT64_C(0xffff)) | ext;
        }
        desired = int128_make128(low, high);
        observed = ia64_exec_cmpxchg_16(env, addr, expected, desired,
                                        false, oi, ra);
        if (int128_eq(observed, expected)) {
            return;
        }
    }
#else
    ia64_exec_exit_atomic(env, ra);
#endif
}

void ia64_fp_ldfe(CPUIA64State *env, uint32_t r1, uint64_t addr, uintptr_t ra)
{
    uint64_t low;
    uint16_t high;
    uint32_t exp;
    bool sign;
    bool direct;

    if ((addr & 15) == 0 &&
        ia64_probe_writeback_ram(env, addr, MMU_DATA_LOAD, &direct, ra) &&
        ia64_exec_is_parallel(env)) {
        Int128 raw;

        if (!direct) {
            ia64_exec_exit_atomic(env, ra);
        }
        raw = ia64_ld16_raw(env, addr, ra);
        if (ia64_data_big_endian(env)) {
            uint64_t raw_low = int128_getlo(raw);
            uint64_t raw_high = int128_gethi(raw);

            high = bswap16(raw_low & 0xffff);
            low = bswap64((raw_low >> 16) | (raw_high << 48));
        } else {
            low = int128_getlo(raw);
            high = int128_gethi(raw) & 0xffff;
        }
    } else if (ia64_data_big_endian(env)) {
        high = ia64_exec_load_data(env, addr, 2, true, ra);
        low = ia64_exec_load_data(env, addr + 2, 8, true, ra);
    } else {
        low = ia64_exec_load_data(env, addr, 8, false, ra);
        high = ia64_exec_load_data(env, addr + 8, 2, false, ra);
    }
    sign = high >> 15;
    exp = high & 0x7fff;
    if (exp == 0x7fff) {
        exp = IA64_FP_REG_SPECIAL_EXP;
    } else if (exp != 0) {
        exp += 0xc000;
    }
    ia64_fr_write_ext(env, r1, sign, exp, low);
}

void ia64_fp_stfe(CPUIA64State *env, uint64_t addr, uint32_t r2, uintptr_t ra)
{
    uint64_t high;
    uint64_t mant;
    uint32_t exp;
    bool sign;
    uint16_t ext_exp;
    bool direct;

    ia64_fpreg_to_spill(env, r2, &mant, &high);
    exp = (high & 0xffff) | (((high >> 16) & 1) << 16);
    sign = (high >> 17) & 1;
    ext_exp = ((exp >> 2) & 0x4000) | (exp & 0x3fff);

    if ((addr & 15) == 0 &&
        ia64_probe_writeback_ram(env, addr, MMU_DATA_STORE, &direct, ra) &&
        ia64_exec_is_parallel(env)) {
        if (!direct) {
            ia64_exec_exit_atomic(env, ra);
        }
        ia64_st10_atomic(env, addr, mant,
                         ((uint16_t)sign << 15) | ext_exp, ra);
        return;
    }

    if (ia64_data_big_endian(env)) {
        ia64_exec_store_data(env, addr,
                             ((uint16_t)sign << 15) | ext_exp, 2, true, ra);
        ia64_exec_store_data(env, addr + 2, mant, 8, true, ra);
    } else {
        ia64_exec_store_data(env, addr, mant, 8, false, ra);
        ia64_exec_store_data(env, addr + 8,
                             ((uint16_t)sign << 15) | ext_exp, 2, false, ra);
    }
}
