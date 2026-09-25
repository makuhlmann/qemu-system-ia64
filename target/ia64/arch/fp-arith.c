/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * IA-64 register-format arithmetic: exact multiply-add and the rounding,
 * exponent-range and trap-response rules of SDM Vol 1 5.2-5.4.
 */

#include "qemu/osdep.h"
#include "qemu/host-utils.h"
#include "arch/fp-arith.h"

/* Register exponent of the value of an exponent field of 0 (Vol 1 5.1.2). */
#define IA64_FP_EXP_ZERO_FIELD_VALUE (IA64_FP_BIAS - 16382)

typedef struct IA64U256 {
    __uint128_t hi;
    __uint128_t lo;
} IA64U256;

static int ia64_clz128(__uint128_t v)
{
    uint64_t hi = v >> 64;

    return hi ? clz64(hi) : 64 + clz64((uint64_t)v);
}

/* Shift right; any bit shifted out is ORed into bit 0. */
static IA64U256 ia64_u256_shr_jam(IA64U256 a, uint32_t n)
{
    IA64U256 r;
    bool sticky;

    if (n == 0) {
        return a;
    }
    if (n >= 256) {
        r.hi = 0;
        r.lo = (a.hi | a.lo) != 0;
        return r;
    }
    if (n >= 128) {
        uint32_t s = n - 128;

        sticky = a.lo != 0 || (s != 0 && (a.hi << (128 - s)) != 0);
        r.hi = 0;
        r.lo = (s ? a.hi >> s : a.hi) | sticky;
        return r;
    }
    sticky = (a.lo << (128 - n)) != 0;
    r.lo = (a.lo >> n) | (a.hi << (128 - n)) | sticky;
    r.hi = a.hi >> n;
    return r;
}

static bool ia64_u256_lt(IA64U256 a, IA64U256 b)
{
    return a.hi < b.hi || (a.hi == b.hi && a.lo < b.lo);
}

static IA64U256 ia64_u256_add(IA64U256 a, IA64U256 b)
{
    IA64U256 r;

    r.lo = a.lo + b.lo;
    r.hi = a.hi + b.hi + (r.lo < a.lo);
    return r;
}

static IA64U256 ia64_u256_sub(IA64U256 a, IA64U256 b)
{
    IA64U256 r;

    r.lo = a.lo - b.lo;
    r.hi = a.hi - b.hi - (a.lo < b.lo);
    return r;
}

/*
 * Controls are one FPSR status field (bit 0 ftz, 1 wre, 3:2 pc, 5:4 rc);
 * pc is the instruction's completer: 0 none, 1 .s, 2 .d (Table 5-6).
 */
void ia64_fpa_format(IA64FPFormat *fmt, uint64_t controls, uint32_t pc,
                     uint64_t disabled)
{
    bool wre = controls & 2;
    uint32_t sf_pc = (controls >> 2) & 3;
    int32_t range;

    if (pc == 1 || (pc == 0 && sf_pc == 0)) {
        fmt->precision = 24;
    } else if (pc == 2 || (pc == 0 && sf_pc == 2)) {
        fmt->precision = 53;
    } else {
        fmt->precision = 64;
    }
    if (wre) {
        range = 17;
    } else if (pc == 1) {
        range = 8;
    } else if (pc == 2) {
        range = 11;
    } else {
        range = 15;
    }
    if (range == 17) {
        fmt->emin = 1;
        fmt->emax = 0x1fffe;
    } else {
        fmt->emax = IA64_FP_BIAS + (1 << (range - 1)) - 1;
        fmt->emin = IA64_FP_BIAS - (1 << (range - 1)) + 2;
    }
    /* Double-extended denormal results use exponent 0 (Vol 1 5.1.3). */
    fmt->denormal_exp = range == 15 ? 0 : fmt->emin;
    fmt->rc = (controls >> 4) & 3;
    fmt->ftz = controls & 1;
    fmt->disabled = disabled;
}

static int32_t ia64_fpa_value_exp(const IA64FPReg *v)
{
    return v->exp ? (int32_t)v->exp : IA64_FP_EXP_ZERO_FIELD_VALUE;
}

/*
 * Compute a * b + c (c may be NULL: the product alone) without rounding.
 * All operands are finite; a zero or pseudo-zero counts as a signed zero.
 * Returns false for an exact zero, whose sign is then in out->sign.
 */
bool ia64_fpa_muladd(IA64FPExact *out, const IA64FPReg *a,
                     const IA64FPReg *b, const IA64FPReg *c,
                     bool negate_product, bool negate_addend, uint32_t rc)
{
    bool psign = a->sign ^ b->sign ^ negate_product;
    bool csign = c ? c->sign ^ negate_addend : false;
    bool pzero = a->sig == 0 || b->sig == 0;
    bool czero = !c || c->sig == 0;
    __uint128_t prod = 0;
    __uint128_t addend = 0;
    int32_t pexp = 0;
    int32_t cexp = 0;
    IA64U256 big;
    IA64U256 small;
    int32_t big_exp;
    bool big_sign;
    int shift;

    if (!pzero) {
        prod = (__uint128_t)a->sig * b->sig;
        shift = ia64_clz128(prod);
        prod <<= shift;
        pexp = ia64_fpa_value_exp(a) + ia64_fpa_value_exp(b) -
               IA64_FP_BIAS + 1 - shift;
    }
    if (!czero) {
        addend = (__uint128_t)c->sig << 64;
        shift = ia64_clz128(addend);
        addend <<= shift;
        cexp = ia64_fpa_value_exp(c) - shift;
    }

    if (pzero && czero) {
        if (!c || psign == csign) {
            out->sign = psign;
        } else {
            out->sign = rc == 1;
        }
        return false;
    }
    if (czero || pzero) {
        out->sign = czero ? psign : csign;
        out->exp = czero ? pexp : cexp;
        out->sig = czero ? prod : addend;
        out->sticky = false;
        return true;
    }

    /* Put both at bits 254:127 and align the smaller magnitude. */
    if (pexp > cexp || (pexp == cexp && prod >= addend)) {
        big = (IA64U256){ prod >> 1, prod << 127 };
        small = (IA64U256){ addend >> 1, addend << 127 };
        small = ia64_u256_shr_jam(small, MIN((int64_t)pexp - cexp, 256));
        big_exp = pexp;
        big_sign = psign;
    } else {
        big = (IA64U256){ addend >> 1, addend << 127 };
        small = (IA64U256){ prod >> 1, prod << 127 };
        small = ia64_u256_shr_jam(small, MIN((int64_t)cexp - pexp, 256));
        big_exp = cexp;
        big_sign = csign;
    }

    if (psign == csign) {
        big = ia64_u256_add(big, small);
    } else {
        if (ia64_u256_lt(big, small)) {
            IA64U256 t = big;

            big = small;
            small = t;
            big_sign = !big_sign;
        }
        big = ia64_u256_sub(big, small);
        if (big.hi == 0 && big.lo == 0) {
            out->sign = rc == 1;
            return false;
        }
    }

    /* Normalize so that bit 255 is set; big_exp names bit 254. */
    shift = big.hi ? ia64_clz128(big.hi) : 128 + ia64_clz128(big.lo);
    if (shift >= 128) {
        big.hi = big.lo << (shift - 128);
        big.lo = 0;
    } else if (shift > 0) {
        big.hi = (big.hi << shift) | (big.lo >> (128 - shift));
        big.lo <<= shift;
    }
    out->sign = big_sign;
    out->exp = big_exp + 1 - shift;
    out->sig = big.hi;
    out->sticky = big.lo != 0;
    return true;
}

static bool ia64_fpa_round_up(uint32_t rc, bool sign, bool lsb, bool half,
                              bool below_half)
{
    bool inexact = half || below_half;

    switch (rc) {
    case 0:
        return half && (below_half || lsb);
    case 1:
        return inexact && sign;
    case 2:
        return inexact && !sign;
    default:
        return false;
    }
}

/*
 * Keep the bits of sig above bit 'shift' (sticky adds to what is below)
 * and round them by rc.  Returns the kept value, which can carry into
 * bit 128 - shift.
 */
static __uint128_t ia64_fpa_round_bits(__uint128_t sig, bool sticky,
                                       uint32_t shift, uint32_t rc,
                                       bool sign, bool *inexact, bool *up)
{
    __uint128_t keep;
    bool half;
    bool below;

    if (shift > 128) {
        keep = 0;
        half = false;
        below = sig != 0 || sticky;
    } else if (shift == 128) {
        keep = 0;
        half = sig >> 127;
        below = (sig << 1) != 0 || sticky;
    } else {
        __uint128_t rest = sig << (128 - shift);

        keep = sig >> shift;
        half = rest >> 127;
        below = (rest << 1) != 0 || sticky;
    }
    *inexact = half || below;
    *up = ia64_fpa_round_up(rc, sign, keep & 1, half, below);
    return keep + *up;
}

/*
 * Round an exact nonzero value for the destination format, and apply the
 * overflow, underflow, flush-to-zero and inexact rules of Vol 1 Figure
 * 5-12: tininess after rounding, the trap-enabled O/U responses keep the
 * unbounded exponent, ISR.i reports tmp_i with an O/U trap even when I is
 * disabled, and ISR.fpa records a magnitude that grew.
 */
void ia64_fpa_round(IA64FPRounded *out, const IA64FPExact *x,
                    const IA64FPFormat *fmt)
{
    uint32_t p = fmt->precision;
    bool o_enabled = !(fmt->disabled & IA64_FP_FLAG_O);
    bool u_enabled = !(fmt->disabled & IA64_FP_FLAG_U);
    bool i_enabled = !(fmt->disabled & IA64_FP_FLAG_I);
    bool tmp_i;
    bool tmp_fpa;
    __uint128_t keep;
    int32_t exp = x->exp;

    out->sign = x->sign;
    out->flags = 0;
    out->trap = 0;

    keep = ia64_fpa_round_bits(x->sig, x->sticky, 128 - p, fmt->rc,
                               x->sign, &tmp_i, &tmp_fpa);
    if (keep >> p) {
        keep >>= 1;
        exp++;
    }
    out->exp = exp;
    out->sig = (uint64_t)keep << (64 - p);

    if (exp > fmt->emax) {
        out->flags = IA64_FP_FLAG_O | IA64_FP_FLAG_I;
        if (o_enabled) {
            if (!tmp_i) {
                out->flags &= ~IA64_FP_FLAG_I;
            }
            out->trap = IA64_FP_TRAP_O | (tmp_i ? IA64_FP_TRAP_I : 0) |
                        (tmp_fpa ? IA64_FP_TRAP_FPA : 0);
            return;
        }
        if (fmt->rc == 0 || (fmt->rc == 1 && x->sign) ||
            (fmt->rc == 2 && !x->sign)) {
            out->exp = IA64_FP_EXP_SPECIAL;
            out->sig = IA64_FP_INT_BIT;
            tmp_fpa = true;
        } else {
            out->exp = fmt->emax;
            out->sig = UINT64_MAX << (64 - p);
            tmp_fpa = false;
        }
        if (i_enabled) {
            out->trap = IA64_FP_TRAP_I | (tmp_fpa ? IA64_FP_TRAP_FPA : 0);
        }
        return;
    }

    if (exp < fmt->emin) {
        if (u_enabled) {
            out->flags = IA64_FP_FLAG_U | (tmp_i ? IA64_FP_FLAG_I : 0);
            out->trap = IA64_FP_TRAP_U | (tmp_i ? IA64_FP_TRAP_I : 0) |
                        (tmp_fpa ? IA64_FP_TRAP_FPA : 0);
            return;
        }
        if (fmt->ftz) {
            out->exp = 0;
            out->sig = 0;
            out->flags = IA64_FP_FLAG_U | IA64_FP_FLAG_I;
            out->trap = i_enabled ? IA64_FP_TRAP_I : 0;
            return;
        }
        keep = ia64_fpa_round_bits(x->sig, x->sticky,
                                   128 - p + (fmt->emin - x->exp),
                                   fmt->rc, x->sign, &tmp_i, &tmp_fpa);
        out->sig = (uint64_t)keep << (64 - p);
        if (out->sig == 0) {
            out->exp = 0;
        } else if (out->sig & IA64_FP_INT_BIT) {
            out->exp = fmt->emin;
        } else {
            out->exp = fmt->denormal_exp;
        }
        if (tmp_i) {
            out->flags = IA64_FP_FLAG_U | IA64_FP_FLAG_I;
            if (i_enabled) {
                out->trap = IA64_FP_TRAP_I |
                            (tmp_fpa ? IA64_FP_TRAP_FPA : 0);
            }
        }
        return;
    }

    if (tmp_i) {
        out->flags = IA64_FP_FLAG_I;
        if (i_enabled) {
            out->trap = IA64_FP_TRAP_I | (tmp_fpa ? IA64_FP_TRAP_FPA : 0);
        }
    }
}

static int ia64_fpa_compare_magnitude(const IA64FPReg *a, const IA64FPReg *b)
{
    int32_t aexp;
    int32_t bexp;
    uint64_t asig;
    uint64_t bsig;
    int shift;

    if (ia64_fpr_is_inf(a) || ia64_fpr_is_inf(b)) {
        return ia64_fpr_is_inf(a) - ia64_fpr_is_inf(b);
    }
    shift = clz64(a->sig);
    aexp = ia64_fpa_value_exp(a) - shift;
    asig = a->sig << shift;
    shift = clz64(b->sig);
    bexp = ia64_fpa_value_exp(b) - shift;
    bsig = b->sig << shift;
    if (aexp != bexp) {
        return aexp < bexp ? -1 : 1;
    }
    return asig < bsig ? -1 : asig > bsig;
}

/*
 * Order two supported non-NaN operands by value, or by magnitude: zeros and
 * pseudo-zeros are equal, unnormals count by their value.
 */
int ia64_fpa_compare(const IA64FPReg *a, const IA64FPReg *b, bool magnitude)
{
    bool asign = !magnitude && a->sign;
    bool bsign = !magnitude && b->sign;
    bool azero = ia64_fpr_is_zero_value(a);
    bool bzero = ia64_fpr_is_zero_value(b);
    int order;

    if (azero && bzero) {
        return 0;
    }
    if (azero) {
        return bsign ? 1 : -1;
    }
    if (bzero) {
        return asign ? -1 : 1;
    }
    if (asign != bsign) {
        return asign ? -1 : 1;
    }
    order = ia64_fpa_compare_magnitude(a, b);
    return asign ? -order : order;
}

/*
 * Round a finite value to a 64-bit integer by rc: two's complement when
 * is_signed, else unsigned.  Returns false when the rounded value does
 * not fit.  fpa reports a magnitude that grew in rounding.
 */
bool ia64_fpa_to_integer(const IA64FPReg *v, bool is_signed, uint32_t rc,
                         uint64_t *result, bool *inexact, bool *fpa)
{
    __uint128_t mag;
    int32_t shift;
    int lz;

    *inexact = false;
    *fpa = false;
    if (v->sig == 0) {
        *result = 0;
        return true;
    }
    lz = clz64(v->sig);
    /* The integer part is (sig << lz << 64) >> shift. */
    shift = IA64_FP_BIAS + 127 + lz - ia64_fpa_value_exp(v);
    if (shift < 64) {
        return false;
    }
    mag = ia64_fpa_round_bits((__uint128_t)(v->sig << lz) << 64, false,
                              shift, rc, v->sign, inexact, fpa);
    if (is_signed) {
        if (mag > (v->sign ? 1ULL << 63 : (1ULL << 63) - 1)) {
            return false;
        }
        *result = v->sign ? -(uint64_t)mag : (uint64_t)mag;
    } else {
        if ((mag >> 64) != 0 || (v->sign && mag != 0)) {
            return false;
        }
        *result = mag;
    }
    return true;
}
