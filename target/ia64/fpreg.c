/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * IA-64 floating-point register representation helpers.
 */

#include "qemu/osdep.h"
#include "cpu.h"
#include "fpreg.h"

#define IA64_FP_SINGLE_EXP_BASE 0x0ff80
#define IA64_FP_SINGLE_FRAC_MASK ((1ULL << 23) - 1)
bool ia64_fpreg_get_extended(const CPUIA64State *env, unsigned reg,
                             bool *sign, uint32_t *exp, uint64_t *mant)
{
    if (reg <= 1 || reg >= IA64_FR_COUNT ||
        !((env->fp.fr_ext_valid[reg / 64] >> (reg % 64)) & 1)) {
        return false;
    }

    *sign = (env->fp.fr_ext_sign[reg / 64] >> (reg % 64)) & 1;
    *exp = env->fp.fr_ext_exp[reg];
    *mant = env->fp.fr_ext_mant[reg];
    return true;
}

static void binary32_to_register_format(uint32_t value, uint64_t *sig,
                                        uint32_t *exp, bool *sign)
{
    uint32_t frac = value & IA64_FP_SINGLE_FRAC_MASK;
    uint32_t binary_exp = (value >> 23) & 0xff;

    *sign = value >> 31;
    if (binary_exp == 0) {
        if (frac == 0) {
            *exp = 0;
            *sig = 0;
        } else {
            *exp = IA64_FP_SINGLE_EXP_BASE + 1;
            *sig = (uint64_t)frac << 40;
        }
    } else if (binary_exp == 0xff) {
        *exp = IA64_FP_REG_SPECIAL_EXP;
        *sig = IA64_FP_SIGNIFICAND_INTEGER_BIT | ((uint64_t)frac << 40);
    } else {
        *exp = IA64_FP_SINGLE_EXP_BASE + binary_exp;
        *sig = IA64_FP_SIGNIFICAND_INTEGER_BIT | ((uint64_t)frac << 40);
    }
}

static uint32_t register_format_to_binary32(uint64_t sig, uint32_t exp,
                                            bool sign)
{
    uint32_t value = (uint32_t)sign << 31;

    if (sig & IA64_FP_SIGNIFICAND_INTEGER_BIT) {
        value |= (((exp >> 9) & 0x80) | (exp & 0x7f)) << 23;
    }
    value |= (sig >> 40) & IA64_FP_SINGLE_FRAC_MASK;
    return value;
}

static uint64_t register_format_to_binary64(uint64_t sig, uint32_t exp,
                                            bool sign)
{
    uint64_t value = (uint64_t)sign << 63;

    if (sig & IA64_FP_SIGNIFICAND_INTEGER_BIT) {
        value |= (uint64_t)(((exp >> 6) & 0x400) | (exp & 0x3ff)) << 52;
    }
    value |= (sig >> 11) & IA64_FP_DOUBLE_FRAC_MASK;
    return value;
}

void ia64_fpreg_to_spill(const CPUIA64State *env, unsigned reg,
                         uint64_t *low, uint64_t *high)
{
    uint32_t exp;
    bool sign;

    g_assert(reg < IA64_FR_COUNT);
    ia64_fpreg_get(env, reg, &sign, &exp, low);
    *high = (exp & 0x1ffff) | ((uint64_t)sign << 17);
}

void ia64_fpreg_from_spill(CPUIA64State *env, unsigned reg,
                           uint64_t low, uint64_t high)
{
    g_assert(reg < IA64_FR_COUNT);
    ia64_fpreg_set(env, reg, (high >> 17) & 1, high & 0x1ffff, low);
}

void ia64_fpreg_from_binary32(CPUIA64State *env, unsigned reg,
                              uint32_t value)
{
    uint64_t sig;
    uint32_t exp;
    bool sign;

    binary32_to_register_format(value, &sig, &exp, &sign);
    ia64_fpreg_from_spill(env, reg, sig,
                         (uint64_t)exp | ((uint64_t)sign << 17));
}

uint32_t ia64_fpreg_to_binary32(const CPUIA64State *env, unsigned reg)
{
    uint64_t low;
    uint64_t high;

    ia64_fpreg_to_spill(env, reg, &low, &high);
    return register_format_to_binary32(low, high & 0x1ffff,
                                       (high >> 17) & 1);
}

uint64_t ia64_fpreg_to_binary64(const CPUIA64State *env, unsigned reg)
{
    uint64_t low;
    uint64_t high;

    ia64_fpreg_to_spill(env, reg, &low, &high);
    return register_format_to_binary64(low, high & 0x1ffff,
                                       (high >> 17) & 1);
}
