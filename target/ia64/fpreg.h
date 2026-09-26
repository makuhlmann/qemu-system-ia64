/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * IA-64 floating-point register representation helpers.
 */

#ifndef TARGET_IA64_FPREG_H
#define TARGET_IA64_FPREG_H

#include <stdbool.h>
#include <stdint.h>

typedef struct CPUArchState CPUIA64State;

#define IA64_FP_REG_INTEGER_EXP 0x1003e
#define IA64_FP_REG_NATVAL_EXP  0x1fffe
#define IA64_FP_REG_SPECIAL_EXP 0x1ffff
#define IA64_FP_SIGNIFICAND_INTEGER_BIT (1ULL << 63)
#define IA64_FP_SPILL_EXP_SIGN_MASK 0x3ffffULL
#define IA64_FP_DOUBLE_EXP_BASE 0x0fc00
#define IA64_FP_DOUBLE_FRAC_MASK ((1ULL << 52) - 1)

/*
 * Keep the padding in floatx80 initialized.  Building the value with the
 * generic positional compound literal can make the host compiler assemble
 * the padded return value through overlapping stack stores and loads.
 */
static inline floatx80 ia64_make_floatx80(uint16_t exp, uint64_t mant)
{
    floatx80 value = { 0 };

    value.low = mant;
    value.high = exp;
    return value;
}

/*
 * These tag operations sit on every floating-point helper path.  Keep them
 * inline even though the full spill/fill conversion lives in fpreg.c; putting
 * these operations behind an out-of-line ABI measurably increases the cost of
 * ordinary FP instructions.
 *
 * Include cpu.h before this header so CPUIA64State is complete.
 */
static inline uint64_t ia64_fpreg_tag_bit(unsigned reg)
{
    return 1ULL << (reg % 64);
}

static inline void ia64_fpreg_mark_written(CPUIA64State *env, unsigned reg)
{
    if (reg <= 1) {
        return;
    }

    env->psr |= reg >= 32 ? IA64_PSR_MFH : IA64_PSR_MFL;
    if (reg >= 32) {
        env->fp.rotating_fr_live = true;
    }
}

static inline void ia64_fpreg_clear_tags(CPUIA64State *env, unsigned reg)
{
    uint64_t mask = ~ia64_fpreg_tag_bit(reg);

    env->fp.fr_nat[reg / 64] &= mask;
    env->fp.fr_sig[reg / 64] &= mask;
    env->fp.fr_ext_valid[reg / 64] &= mask;
    env->fp.fr_int_origin[reg / 64] &= mask;
    env->fp.fr_int_value[reg] = 0;
}

static inline bool ia64_fpreg_is_nat(const CPUIA64State *env, unsigned reg)
{
    return reg > 1 && reg < IA64_FR_COUNT &&
           ((env->fp.fr_nat[reg / 64] >> (reg % 64)) & 1);
}

static inline bool ia64_fpreg_is_integer(const CPUIA64State *env,
                                         unsigned reg)
{
    return reg > 1 && reg < IA64_FR_COUNT &&
           ((env->fp.fr_sig[reg / 64] >> (reg % 64)) & 1);
}

static inline void ia64_fpreg_from_binary64(CPUIA64State *env, unsigned reg,
                                            uint64_t value)
{
    g_assert(reg < IA64_FR_COUNT);
    if (reg <= 1) {
        return;
    }

    ia64_fpreg_clear_tags(env, reg);
    env->fp.fr[reg] = value;
    ia64_fpreg_mark_written(env, reg);
}

static inline void ia64_binary64_to_register_format(uint64_t value,
                                                    uint64_t *sig,
                                                    uint32_t *exp,
                                                    bool *sign)
{
    uint64_t frac = value & IA64_FP_DOUBLE_FRAC_MASK;
    uint32_t binary_exp = (value >> 52) & 0x7ff;

    *sign = value >> 63;
    if (binary_exp == 0) {
        if (frac == 0) {
            *exp = 0;
            *sig = 0;
        } else {
            *exp = IA64_FP_DOUBLE_EXP_BASE + 1;
            *sig = frac << 11;
        }
    } else if (binary_exp == 0x7ff) {
        *exp = IA64_FP_REG_SPECIAL_EXP;
        *sig = IA64_FP_SIGNIFICAND_INTEGER_BIT | (frac << 11);
    } else {
        *exp = IA64_FP_DOUBLE_EXP_BASE + binary_exp;
        *sig = IA64_FP_SIGNIFICAND_INTEGER_BIT | (frac << 11);
    }
}

/*
 * The register as sign, 17-bit exponent and significand (SDM Vol 1
 * Figure 5-1), from whichever form the tags select.  Inline because every
 * FP arithmetic helper reads up to three operands this way.
 */
static inline void ia64_fpreg_get(const CPUIA64State *env, unsigned reg,
                                  bool *sign, uint32_t *exp, uint64_t *sig)
{
    uint64_t bit = ia64_fpreg_tag_bit(reg);
    unsigned word = reg / 64;

    if (reg <= 1) {
        ia64_binary64_to_register_format(reg == IA64_FR_ZERO_INDEX ? 0 :
                                         IA64_FR_ONE, sig, exp, sign);
    } else if (env->fp.fr_nat[word] & bit) {
        *sig = 0;
        *exp = IA64_FP_REG_NATVAL_EXP;
        *sign = false;
    } else if (env->fp.fr_ext_valid[word] & bit) {
        *sign = (env->fp.fr_ext_sign[word] & bit) != 0;
        *exp = env->fp.fr_ext_exp[reg];
        *sig = env->fp.fr_ext_mant[reg];
    } else if (env->fp.fr_sig[word] & bit) {
        *sig = env->fp.fr[reg];
        *exp = IA64_FP_REG_INTEGER_EXP;
        *sign = false;
    } else {
        ia64_binary64_to_register_format(env->fp.fr[reg], sig, exp, sign);
    }
}

/*
 * Store a register-format value.  NaTVal and positive integer-exponent
 * values take their tagged forms; every other value is kept as sign,
 * exponent and significand, and fp.fr[] is not read for it.
 */
static inline void ia64_fpreg_set(CPUIA64State *env, unsigned reg,
                                  bool sign, uint32_t exp, uint64_t sig)
{
    uint64_t bit = ia64_fpreg_tag_bit(reg);
    unsigned word = reg / 64;

    if (reg <= 1) {
        return;
    }

    ia64_fpreg_clear_tags(env, reg);
    if (!sign && exp == IA64_FP_REG_NATVAL_EXP && sig == 0) {
        env->fp.fr[reg] = 0;
        env->fp.fr_nat[word] |= bit;
    } else if (!sign && exp == IA64_FP_REG_INTEGER_EXP) {
        env->fp.fr[reg] = sig;
        env->fp.fr_sig[word] |= bit;
        env->fp.fr_int_value[reg] = sig;
        env->fp.fr_int_origin[word] |= bit;
    } else {
        env->fp.fr[reg] = 0;
        env->fp.fr_ext_mant[reg] = sig;
        env->fp.fr_ext_exp[reg] = exp;
        if (sign) {
            env->fp.fr_ext_sign[word] |= bit;
        } else {
            env->fp.fr_ext_sign[word] &= ~bit;
        }
        env->fp.fr_ext_valid[word] |= bit;
    }
    ia64_fpreg_mark_written(env, reg);
}

/*
 * Convert between the internal tagged register cache and the architected
 * 128-bit spill/fill representation.  Bits above bit 17 of high are reserved
 * and are ignored on fill, as required by the spill format.
 */
void ia64_fpreg_to_spill(const CPUIA64State *env, unsigned reg,
                         uint64_t *low, uint64_t *high);
void ia64_fpreg_from_spill(CPUIA64State *env, unsigned reg,
                           uint64_t low, uint64_t high);

/* Exact extended-format state is retained separately from the float64 cache. */
bool ia64_fpreg_get_extended(const CPUIA64State *env, unsigned reg,
                             bool *sign, uint32_t *exp, uint64_t *mant);

/* IEEE interchange helpers used by setf/getf and the pure unit test. */
void ia64_fpreg_from_binary32(CPUIA64State *env, unsigned reg,
                              uint32_t value);
uint32_t ia64_fpreg_to_binary32(const CPUIA64State *env, unsigned reg);
uint64_t ia64_fpreg_to_binary64(const CPUIA64State *env, unsigned reg);

#endif
