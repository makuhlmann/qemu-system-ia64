/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * IA-64 register-format arithmetic: exact multiply-add and the rounding,
 * exponent-range and trap-response rules of SDM Vol 1 5.2-5.4.
 */

#ifndef TARGET_IA64_ARCH_FP_ARITH_H
#define TARGET_IA64_ARCH_FP_ARITH_H

/* FPSR.sfx flag bits, shifted down to bit 0 (SDM Vol 1 Figure 5-3). */
#define IA64_FP_FLAG_V 0x01u
#define IA64_FP_FLAG_D 0x02u
#define IA64_FP_FLAG_Z 0x04u
#define IA64_FP_FLAG_O 0x08u
#define IA64_FP_FLAG_U 0x10u
#define IA64_FP_FLAG_I 0x20u

/* ISR.code trap bits of a normal or high parallel result (Vol 2 Table 8-3). */
#define IA64_FP_TRAP_O   (1u << 11)
#define IA64_FP_TRAP_U   (1u << 12)
#define IA64_FP_TRAP_I   (1u << 13)
#define IA64_FP_TRAP_FPA (1u << 14)

#define IA64_FP_BIAS        0xffff
#define IA64_FP_EXP_SPECIAL 0x1ffff
#define IA64_FP_EXP_NATVAL  0x1fffe
#define IA64_FP_INT_BIT     (1ULL << 63)
#define IA64_FP_QUIET_BIT   (1ULL << 62)

/* A register-format value (SDM Vol 1 Figure 5-1). */
typedef struct IA64FPReg {
    bool sign;
    uint32_t exp;
    uint64_t sig;
} IA64FPReg;

/*
 * The significand precision, exponent range and controls of one result
 * (SDM Vol 1 Table 5-6).  emin and emax are biased by 0xffff.
 */
typedef struct IA64FPFormat {
    uint32_t precision;
    int32_t emin;
    int32_t emax;
    /* Register exponent of a denormal result: 0 for the 15-bit range. */
    uint32_t denormal_exp;
    uint32_t rc;
    bool ftz;
    /* FPSR.traps-style disable bits (od, ud, id at bits 3-5). */
    uint32_t disabled;
} IA64FPFormat;

/*
 * An exact nonzero finite value: sig has bit 127 set, exp is the biased
 * exponent of that bit, sticky records nonzero bits below sig.
 */
typedef struct IA64FPExact {
    bool sign;
    int32_t exp;
    __uint128_t sig;
    bool sticky;
} IA64FPExact;

/*
 * A rounded result.  exp is the biased exponent in full: the trap-enabled
 * overflow and underflow responses leave it outside the range, and the
 * caller wraps it to the destination exponent width.
 */
typedef struct IA64FPRounded {
    bool sign;
    int32_t exp;
    uint64_t sig;
    uint32_t flags;
    uint32_t trap;
} IA64FPRounded;

static inline bool ia64_fpr_is_special(const IA64FPReg *v)
{
    return v->exp == IA64_FP_EXP_SPECIAL;
}

static inline bool ia64_fpr_is_unsupported(const IA64FPReg *v)
{
    return ia64_fpr_is_special(v) && !(v->sig & IA64_FP_INT_BIT);
}

static inline bool ia64_fpr_is_inf(const IA64FPReg *v)
{
    return ia64_fpr_is_special(v) && v->sig == IA64_FP_INT_BIT;
}

static inline bool ia64_fpr_is_nan(const IA64FPReg *v)
{
    return ia64_fpr_is_special(v) && (v->sig & IA64_FP_INT_BIT) &&
           v->sig != IA64_FP_INT_BIT;
}

static inline bool ia64_fpr_is_snan(const IA64FPReg *v)
{
    return ia64_fpr_is_nan(v) && !(v->sig & IA64_FP_QUIET_BIT);
}

/* A zero or a pseudo-zero: both compute as a signed zero (Vol 1 5.1.3). */
static inline bool ia64_fpr_is_zero_value(const IA64FPReg *v)
{
    return !ia64_fpr_is_special(v) && v->sig == 0;
}

/*
 * An unnormalized operand for the D exception: every finite nonzero value
 * without the integer bit, the double-extended denormals and
 * pseudo-denormals (exponent 0), and the pseudo-zeros (Vol 1 Table 5-2).
 */
static inline bool ia64_fpr_is_unnormal(const IA64FPReg *v)
{
    if (ia64_fpr_is_special(v)) {
        return false;
    }
    if (v->sig == 0) {
        return v->exp != 0;
    }
    return v->exp == 0 || !(v->sig & IA64_FP_INT_BIT);
}

void ia64_fpa_format(IA64FPFormat *fmt, uint64_t controls, uint32_t pc,
                     uint64_t disabled);
bool ia64_fpa_muladd(IA64FPExact *out, const IA64FPReg *a,
                     const IA64FPReg *b, const IA64FPReg *c,
                     bool negate_product, bool negate_addend, uint32_t rc);
void ia64_fpa_round(IA64FPRounded *out, const IA64FPExact *x,
                    const IA64FPFormat *fmt);
int ia64_fpa_compare(const IA64FPReg *a, const IA64FPReg *b,
                     bool magnitude);

#endif /* TARGET_IA64_ARCH_FP_ARITH_H */
