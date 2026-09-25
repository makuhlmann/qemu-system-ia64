# IA-64 instruction encoders split on translator family boundaries.

from .encoding_common import bitfield, op

def getf_sig(r1, f2, qp=0, ignored=0):
    return (
        op(4)
        | bitfield(0xe1, 27, 9)
        | bitfield(ignored, 28, 2)
        | bitfield(f2, 13, 7)
        | bitfield(r1, 6, 7)
        | bitfield(qp, 0, 6)
    )

def getf_exp(r1, f2, qp=0, ignored=0):
    return (
        op(4)
        | bitfield(0xe9, 27, 9)
        | bitfield(ignored, 28, 2)
        | bitfield(f2, 13, 7)
        | bitfield(r1, 6, 7)
        | bitfield(qp, 0, 6)
    )

def getf_s(r1, f2, qp=0):
    return (
        op(4)
        | bitfield(0x1e, 30, 6)
        | bitfield(1, 27, 1)
        | bitfield(f2, 13, 7)
        | bitfield(r1, 6, 7)
        | bitfield(qp, 0, 6)
    )

def getf_d(r1, f2, qp=0):
    return (
        op(4)
        | bitfield(0x1f, 30, 6)
        | bitfield(1, 27, 1)
        | bitfield(f2, 13, 7)
        | bitfield(r1, 6, 7)
        | bitfield(qp, 0, 6)
    )

def setf_sig(f1, r2, ignored=0, qp=0):
    return (
        op(6)
        | bitfield(0xe1, 27, 9)
        | bitfield(ignored, 28, 2)
        | bitfield(r2, 13, 7)
        | bitfield(f1, 6, 7)
        | bitfield(qp, 0, 6)
    )

def setf_d(f1, r2, qp=0):
    return (
        op(6)
        | bitfield(0xf9, 27, 9)
        | bitfield(r2, 13, 7)
        | bitfield(f1, 6, 7)
        | bitfield(qp, 0, 6)
    )

def setf_s(f1, r2, qp=0):
    return (
        op(6)
        | bitfield(0xf1, 27, 9)
        | bitfield(r2, 13, 7)
        | bitfield(f1, 6, 7)
        | bitfield(qp, 0, 6)
    )

def fnorm(f1, f2, f3, qp=0, sf=1):
    return (
        op(8)
        | bitfield(sf, 34, 2)
        | bitfield(1, 27, 7)
        | bitfield(f3, 20, 7)
        | bitfield(f1, 6, 7)
        | bitfield(qp, 0, 6)
    )

def fnorm_s(f1, f2, f3, qp=0, sf=1):
    return fnorm(f1, f2, f3, qp=qp, sf=sf) | bitfield(1, 36, 1)

def fnorm_d(f1, f2, f3, qp=0, sf=1):
    return (
        op(9)
        | bitfield(sf, 34, 2)
        | bitfield(1, 27, 7)
        | bitfield(f3, 20, 7)
        | bitfield(f1, 6, 7)
        | bitfield(qp, 0, 6)
    )

def fmov(f1, f2, qp=0):
    # The assembler alias `mov f1=f2` is fmerge.s f1=f2,f2.
    return fmerge_s(f1, f2, f2, qp)

def fpabs(f1, f3, qp=0):
    return (
        op(1)
        | bitfield(0x10, 27, 6)
        | bitfield(f3, 20, 7)
        | bitfield(f1, 6, 7)
        | bitfield(qp, 0, 6)
    )

def fpneg(f1, f3, qp=0):
    return (
        op(1)
        | bitfield(0x11, 27, 6)
        | bitfield(f3, 20, 7)
        | bitfield(f3, 13, 7)
        | bitfield(f1, 6, 7)
        | bitfield(qp, 0, 6)
    )

def fpnegabs(f1, f3, qp=0):
    return (
        op(1)
        | bitfield(0x11, 27, 6)
        | bitfield(f3, 20, 7)
        | bitfield(f1, 6, 7)
        | bitfield(qp, 0, 6)
    )

def fmerge_ns(f1, f2, f3, qp=0):
    return (
        op(0)
        | bitfield(0x11, 27, 6)
        | bitfield(f3, 20, 7)
        | bitfield(f2, 13, 7)
        | bitfield(f1, 6, 7)
        | bitfield(qp, 0, 6)
    )

def fmerge_s(f1, f2, f3, qp=0):
    return (
        op(0)
        | bitfield(0x10, 27, 6)
        | bitfield(f3, 20, 7)
        | bitfield(f2, 13, 7)
        | bitfield(f1, 6, 7)
        | bitfield(qp, 0, 6)
    )

def fmerge_se(f1, f2, f3, qp=0):
    return (
        op(0)
        | bitfield(0x12, 27, 6)
        | bitfield(f3, 20, 7)
        | bitfield(f2, 13, 7)
        | bitfield(f1, 6, 7)
        | bitfield(qp, 0, 6)
    )

def _f8_scalar_form(form, f1, f2, f3, sf=0, qp=0, bit36=0):
    return (
        op(0)
        | bitfield(bit36, 36, 1)
        | bitfield(sf, 34, 2)
        | bitfield(form, 27, 6)
        | bitfield(f3, 20, 7)
        | bitfield(f2, 13, 7)
        | bitfield(f1, 6, 7)
        | bitfield(qp, 0, 6)
    )

def fmin(f1, f2, f3, sf=0, qp=0):
    return _f8_scalar_form(0x14, f1, f2, f3, sf, qp)

def fmax(f1, f2, f3, sf=0, qp=0):
    return _f8_scalar_form(0x15, f1, f2, f3, sf, qp)

def famin(f1, f2, f3, sf=0, qp=0):
    return _f8_scalar_form(0x16, f1, f2, f3, sf, qp)

def famax(f1, f2, f3, sf=0, qp=0, bit36=0):
    return _f8_scalar_form(0x17, f1, f2, f3, sf, qp, bit36)

def fpack(f1, f2, f3, qp=0):
    return (
        op(0)
        | bitfield(0x28, 27, 6)
        | bitfield(f3, 20, 7)
        | bitfield(f2, 13, 7)
        | bitfield(f1, 6, 7)
        | bitfield(qp, 0, 6)
    )

def _f9_form(x6, f1, f2, f3, qp=0, ignored=0):
    return (
        op(0)
        | bitfield(ignored, 34, 3)
        | bitfield(x6, 27, 6)
        | bitfield(f3, 20, 7)
        | bitfield(f2, 13, 7)
        | bitfield(f1, 6, 7)
        | bitfield(qp, 0, 6)
    )

def fand(f1, f2, f3, qp=0):
    return _f9_form(0x2c, f1, f2, f3, qp)

def fandcm(f1, f2, f3, qp=0):
    return _f9_form(0x2d, f1, f2, f3, qp)

def for_(f1, f2, f3, qp=0):
    return _f9_form(0x2e, f1, f2, f3, qp)

def fxor(f1, f2, f3, qp=0):
    return _f9_form(0x2f, f1, f2, f3, qp)

def fswap(f1, f2, f3, qp=0):
    return _f9_form(0x34, f1, f2, f3, qp)

def fswap_nl(f1, f2, f3, qp=0):
    return _f9_form(0x35, f1, f2, f3, qp)

def fswap_nr(f1, f2, f3, qp=0):
    return _f9_form(0x36, f1, f2, f3, qp)

def fmix_lr(f1, f2, f3, qp=0):
    return _f9_form(0x39, f1, f2, f3, qp)

def fmix_r(f1, f2, f3, qp=0):
    return _f9_form(0x3a, f1, f2, f3, qp)

def fmix_l(f1, f2, f3, qp=0, ignored=0):
    return _f9_form(0x3b, f1, f2, f3, qp, ignored)

def fsxt_r(f1, f2, f3, qp=0):
    return _f9_form(0x3c, f1, f2, f3, qp)

def fsxt_l(f1, f2, f3, qp=0):
    return _f9_form(0x3d, f1, f2, f3, qp)

def _fp_parallel_form(x6, f1, f2, f3, sf=0, qp=0):
    return (
        op(1)
        | bitfield(x6, 27, 6)
        | bitfield(sf, 34, 2)
        | bitfield(f3, 20, 7)
        | bitfield(f2, 13, 7)
        | bitfield(f1, 6, 7)
        | bitfield(qp, 0, 6)
    )

def fpmerge_s(f1, f2, f3, qp=0):
    return _fp_parallel_form(0x10, f1, f2, f3, 0, qp)

def fpmerge_ns(f1, f2, f3, qp=0):
    return _fp_parallel_form(0x11, f1, f2, f3, 0, qp)

def fpmerge_se(f1, f2, f3, qp=0):
    return _fp_parallel_form(0x12, f1, f2, f3, 0, qp)

def fpmin(f1, f2, f3, sf=0, qp=0):
    return _fp_parallel_form(0x14, f1, f2, f3, sf, qp)

def fpmax(f1, f2, f3, sf=0, qp=0):
    return _fp_parallel_form(0x15, f1, f2, f3, sf, qp)

def fpamin(f1, f2, f3, sf=0, qp=0):
    return _fp_parallel_form(0x16, f1, f2, f3, sf, qp)

def fpamax(f1, f2, f3, sf=0, qp=0):
    return _fp_parallel_form(0x17, f1, f2, f3, sf, qp)

def fpcmp(frel, f1, f2, f3, sf=0, qp=0):
    return _fp_parallel_form(0x30 + frel, f1, f2, f3, sf, qp)

def fpcvt_fx(f1, f2, sf=0, qp=0):
    return _fp_parallel_form(0x18, f1, f2, 0, sf, qp)

def fpcvt_fxu(f1, f2, sf=0, qp=0):
    return _fp_parallel_form(0x19, f1, f2, 0, sf, qp)

def fpcvt_fx_trunc(f1, f2, sf=0, qp=0):
    return _fp_parallel_form(0x1a, f1, f2, 0, sf, qp)

def fpcvt_fxu_trunc(f1, f2, sf=0, qp=0):
    return _fp_parallel_form(0x1b, f1, f2, 0, sf, qp)

def fpma(f1, f2, f3, f4, sf=0, qp=0):
    return (
        op(9)
        | bitfield(1, 36, 1)
        | bitfield(sf, 34, 2)
        | bitfield(f4, 27, 7)
        | bitfield(f3, 20, 7)
        | bitfield(f2, 13, 7)
        | bitfield(f1, 6, 7)
        | bitfield(qp, 0, 6)
    )

def fpms(f1, f2, f3, f4, sf=0, qp=0):
    return (
        op(0xb)
        | bitfield(1, 36, 1)
        | bitfield(sf, 34, 2)
        | bitfield(f4, 27, 7)
        | bitfield(f3, 20, 7)
        | bitfield(f2, 13, 7)
        | bitfield(f1, 6, 7)
        | bitfield(qp, 0, 6)
    )

def fpnma(f1, f2, f3, f4, sf=0, qp=0):
    return (
        op(0xd)
        | bitfield(1, 36, 1)
        | bitfield(sf, 34, 2)
        | bitfield(f4, 27, 7)
        | bitfield(f3, 20, 7)
        | bitfield(f2, 13, 7)
        | bitfield(f1, 6, 7)
        | bitfield(qp, 0, 6)
    )

def frsqrta(f1, p2, f3, sf=1, qp=0):
    return (
        op(0)
        | bitfield(1, 36, 1)
        | bitfield(sf, 34, 2)
        | bitfield(1, 33, 1)
        | bitfield(p2, 27, 6)
        | bitfield(f3, 20, 7)
        | bitfield(f1, 6, 7)
        | bitfield(qp, 0, 6)
    )

def fprsqrta(f1, p2, f3, sf=1, qp=0):
    return (
        op(1)
        | bitfield(1, 36, 1)
        | bitfield(sf, 34, 2)
        | bitfield(1, 33, 1)
        | bitfield(p2, 27, 6)
        | bitfield(f3, 20, 7)
        | bitfield(f1, 6, 7)
        | bitfield(qp, 0, 6)
    )

def frcpa(f1, p2, f3, f4, sf=1, qp=0):
    return (
        op(0)
        | bitfield(sf, 34, 2)
        | bitfield(1, 33, 1)
        | bitfield(p2, 27, 6)
        | bitfield(f4, 20, 7)
        | bitfield(f3, 13, 7)
        | bitfield(f1, 6, 7)
        | bitfield(qp, 0, 6)
    )

def fprcpa(f1, p2, f2, f3, sf=1, qp=0):
    return (
        op(1)
        | bitfield(sf, 34, 2)
        | bitfield(1, 33, 1)
        | bitfield(p2, 27, 6)
        | bitfield(f3, 20, 7)
        | bitfield(f2, 13, 7)
        | bitfield(f1, 6, 7)
        | bitfield(qp, 0, 6)
    )

def fcvt_fx(f1, f2, unsigned=False, trunc=False, sf=1, qp=0):
    form = 0x18 | (1 if unsigned else 0) | (2 if trunc else 0)

    return (
        op(0)
        | bitfield(sf, 34, 2)
        | bitfield(form, 27, 6)
        | bitfield(f2, 13, 7)
        | bitfield(f1, 6, 7)
        | bitfield(qp, 0, 6)
    )

def fcvt_fxu(f1, f2, trunc=True, sf=1, qp=0):
    return fcvt_fx(f1, f2, unsigned=True, trunc=trunc, sf=sf, qp=qp)

def fcvt_xf(f1, f2, qp=0):
    return (
        op(0)
        | bitfield(0x1c, 27, 6)
        | bitfield(f2, 13, 7)
        | bitfield(f1, 6, 7)
        | bitfield(qp, 0, 6)
    )

def fmpy_s1(f1, f3, f4, qp=0):
    return (
        op(8)
        | bitfield(1, 34, 2)
        | bitfield(f4, 27, 7)
        | bitfield(f3, 20, 7)
        | bitfield(f1, 6, 7)
        | bitfield(qp, 0, 6)
    )

def fmpy_s_s1(f1, f3, f4, qp=0):
    return fmpy_s1(f1, f3, f4, qp) | bitfield(1, 36, 1)

def fmpy_s0(f1, f3, f4, qp=0):
    return (
        op(8)
        | bitfield(f4, 27, 7)
        | bitfield(f3, 20, 7)
        | bitfield(f1, 6, 7)
        | bitfield(qp, 0, 6)
    )

def fma_s1(f1, f3, f4, f2, qp=0):
    return fmpy_s1(f1, f3, f4, qp) | bitfield(f2, 13, 7)

def fma_s0(f1, f3, f4, f2, qp=0):
    return fmpy_s0(f1, f3, f4, qp) | bitfield(f2, 13, 7)

def fma_s_s0(f1, f3, f4, f2, qp=0):
    return fma_s0(f1, f3, f4, f2, qp) | bitfield(1, 36, 1)

def fma_d_s0(f1, f3, f4, f2, qp=0):
    return (
        op(9)
        | bitfield(f4, 27, 7)
        | bitfield(f3, 20, 7)
        | bitfield(f2, 13, 7)
        | bitfield(f1, 6, 7)
        | bitfield(qp, 0, 6)
    )

def fsub_d_s0(f1, f3, f2, qp=0):
    return (
        op(0xb)
        | bitfield(1, 27, 7)
        | bitfield(f3, 20, 7)
        | bitfield(f2, 13, 7)
        | bitfield(f1, 6, 7)
        | bitfield(qp, 0, 6)
    )

def fsub_s0(f1, f3, f2, qp=0):
    return (
        op(0xa)
        | bitfield(1, 27, 7)
        | bitfield(f3, 20, 7)
        | bitfield(f2, 13, 7)
        | bitfield(f1, 6, 7)
        | bitfield(qp, 0, 6)
    )

def fsub_s_s0(f1, f3, f2, qp=0):
    return (
        op(0xa)
        | bitfield(1, 36, 1)
        | bitfield(1, 27, 7)
        | bitfield(f3, 20, 7)
        | bitfield(f2, 13, 7)
        | bitfield(f1, 6, 7)
        | bitfield(qp, 0, 6)
    )

def fms_s3(f1, f3, f4, f2, qp=0):
    return (
        op(0xa)
        | bitfield(3, 34, 2)
        | bitfield(f4, 27, 7)
        | bitfield(f3, 20, 7)
        | bitfield(f2, 13, 7)
        | bitfield(f1, 6, 7)
        | bitfield(qp, 0, 6)
    )

def fms_s0(f1, f3, f4, f2, qp=0):
    return (
        op(0xa)
        | bitfield(f4, 27, 7)
        | bitfield(f3, 20, 7)
        | bitfield(f2, 13, 7)
        | bitfield(f1, 6, 7)
        | bitfield(qp, 0, 6)
    )

def fms_d_s0(f1, f3, f4, f2, qp=0):
    return (
        op(0xb)
        | bitfield(f4, 27, 7)
        | bitfield(f3, 20, 7)
        | bitfield(f2, 13, 7)
        | bitfield(f1, 6, 7)
        | bitfield(qp, 0, 6)
    )

def fclass_m(p1, p2, f2, fclass9, unc=False, ignored=0, qp=0):
    return (
        op(5)
        | bitfield(ignored, 35, 2)
        | bitfield((fclass9 >> 2) & 0x7f, 20, 7)
        | bitfield(f2, 13, 7)
        | bitfield(1 if unc else 0, 12, 1)
        | bitfield(p1, 6, 6)
        | bitfield(fclass9 & 3, 33, 2)
        | bitfield(p2, 27, 6)
        | bitfield(qp, 0, 6)
    )

def fcmp(p1, p2, f2, f3, rel=0, sf=0, unc=False, qp=0):
    return (
        op(4)
        | bitfield((rel >> 1) & 1, 33, 1)
        | bitfield(sf, 34, 2)
        | bitfield(rel & 1, 36, 1)
        | bitfield(f3, 20, 7)
        | bitfield(f2, 13, 7)
        | bitfield(1 if unc else 0, 12, 1)
        | bitfield(p2, 27, 6)
        | bitfield(p1, 6, 6)
        | bitfield(qp, 0, 6)
    )

def fnma_s1(f1, f3, f4, f2, qp=0):
    return (
        op(0xc)
        | bitfield(1, 34, 2)
        | bitfield(f4, 27, 7)
        | bitfield(f3, 20, 7)
        | bitfield(f2, 13, 7)
        | bitfield(f1, 6, 7)
        | bitfield(qp, 0, 6)
    )

def fnma_s0(f1, f3, f4, f2, qp=0):
    return (
        op(0xc)
        | bitfield(f4, 27, 7)
        | bitfield(f3, 20, 7)
        | bitfield(f2, 13, 7)
        | bitfield(f1, 6, 7)
        | bitfield(qp, 0, 6)
    )

def fnma_s_s0(f1, f3, f4, f2, qp=0):
    return (
        op(0xc)
        | bitfield(1, 36, 1)
        | bitfield(f4, 27, 7)
        | bitfield(f3, 20, 7)
        | bitfield(f2, 13, 7)
        | bitfield(f1, 6, 7)
        | bitfield(qp, 0, 6)
    )

def fnmpy_s_s1(f1, f3, f4, qp=0):
    return fnma_s1(f1, f3, f4, 0, qp) | bitfield(1, 36, 1)

def fnma_d_s1(f1, f3, f4, f2, qp=0):
    return fnma_s1(f1, f3, f4, f2, qp) | op(0xd)

def setf_exp(f1, r2, qp=0, ignored=0):
    return (
        op(6)
        | bitfield(0xe9, 27, 9)
        | bitfield(ignored, 28, 2)
        | bitfield(r2, 13, 7)
        | bitfield(f1, 6, 7)
        | bitfield(qp, 0, 6)
    )

def fselect(f1, f2, f3, f4, qp=0):
    return (
        op(0xe)
        | bitfield(f4, 27, 7)
        | bitfield(f3, 20, 7)
        | bitfield(f2, 13, 7)
        | bitfield(f1, 6, 7)
        | bitfield(qp, 0, 6)
    )

def xma_h(f1, f2, f3, f4, qp=0):
    return (
        op(0xe)
        | bitfield(1, 36, 1)
        | bitfield(3, 34, 2)
        | bitfield(f4, 27, 7)
        | bitfield(f3, 20, 7)
        | bitfield(f2, 13, 7)
        | bitfield(f1, 6, 7)
        | bitfield(qp, 0, 6)
    )

def xma_l(f1, f2, f3, f4, qp=0):
    return (
        op(0xe)
        | bitfield(1, 36, 1)
        | bitfield(f4, 27, 7)
        | bitfield(f3, 20, 7)
        | bitfield(f2, 13, 7)
        | bitfield(f1, 6, 7)
        | bitfield(qp, 0, 6)
    )

def xma_hu(f1, f2, f3, f4, qp=0):
    return (
        op(0xe)
        | bitfield(1, 36, 1)
        | bitfield(2, 34, 2)
        | bitfield(f4, 27, 7)
        | bitfield(f3, 20, 7)
        | bitfield(f2, 13, 7)
        | bitfield(f1, 6, 7)
        | bitfield(qp, 0, 6)
    )

def xmpy_hu(f1, f3, f4, qp=0):
    return (
        op(0xe)
        | bitfield(1, 36, 1)
        | bitfield(2, 34, 2)
        | bitfield(f4, 27, 7)
        | bitfield(f3, 20, 7)
        | bitfield(f1, 6, 7)
        | bitfield(qp, 0, 6)
    )

__all__ = (
    'getf_sig',
    'getf_exp',
    'getf_s',
    'getf_d',
    'setf_sig',
    'setf_d',
    'setf_s',
    'fnorm',
    'fnorm_s',
    'fnorm_d',
    'fmov',
    'fpabs',
    'fpneg',
    'fpnegabs',
    'fmerge_ns',
    'fmerge_s',
    'fmerge_se',
    'fmin',
    'fmax',
    'famin',
    'famax',
    'fpack',
    'fand',
    'fandcm',
    'for_',
    'fxor',
    'fswap',
    'fswap_nl',
    'fswap_nr',
    'fmix_lr',
    'fmix_r',
    'fmix_l',
    'fsxt_r',
    'fsxt_l',
    'fpmerge_s',
    'fpmerge_ns',
    'fpmerge_se',
    'fpmin',
    'fpmax',
    'fpamin',
    'fpamax',
    'fpcmp',
    'fpcvt_fx',
    'fpcvt_fxu',
    'fpcvt_fx_trunc',
    'fpcvt_fxu_trunc',
    'fpma',
    'fpms',
    'fpnma',
    'frsqrta',
    'fprsqrta',
    'frcpa',
    'fprcpa',
    'fcvt_fx',
    'fcvt_fxu',
    'fcvt_xf',
    'fmpy_s1',
    'fmpy_s_s1',
    'fmpy_s0',
    'fma_s1',
    'fma_s0',
    'fma_s_s0',
    'fma_d_s0',
    'fsub_d_s0',
    'fsub_s0',
    'fsub_s_s0',
    'fms_s0',
    'fms_d_s0',
    'fms_s3',
    'fclass_m',
    'fcmp',
    'fnma_s1',
    'fnma_s0',
    'fnma_s_s0',
    'fnmpy_s_s1',
    'fnma_d_s1',
    'setf_exp',
    'fselect',
    'xma_h',
    'xma_l',
    'xma_hu',
    'xmpy_hu',
)
