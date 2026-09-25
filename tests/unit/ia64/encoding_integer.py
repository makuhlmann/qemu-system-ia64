# IA-64 instruction encoders split on translator family boundaries.

from .encoding_common import bitfield, nop_m, op

def adds(r1, imm, r3, qp=0):
    encoded = imm & 0x3fff
    return (
        op(8)
        | bitfield(2, 34, 2)
        | bitfield(encoded & 0x7f, 13, 7)
        | bitfield((encoded >> 7) & 0x3f, 27, 6)
        | bitfield((encoded >> 13) & 1, 36, 1)
        | bitfield(r3, 20, 7)
        | bitfield(r1, 6, 7)
        | bitfield(qp, 0, 6)
    )

def addl(r1, imm, r3, qp=0):
    if not 0 <= r3 < 4:
        raise ValueError("addl base must be a static general register r0-r3")
    encoded = imm & 0x3FFFFF
    return (
        op(9)
        | bitfield((encoded >> 21) & 1, 36, 1)
        | bitfield((encoded >> 7) & 0x1FF, 27, 9)
        | bitfield((encoded >> 16) & 0x1F, 22, 5)
        | bitfield(r3, 20, 2)
        | bitfield(encoded & 0x7F, 13, 7)
        | bitfield(r1, 6, 7)
        | bitfield(qp, 0, 6)
    )

def alu_reg(x4, x2b, r1, r2, r3, qp=0):
    return (
        op(8)
        | bitfield(x4, 29, 4)
        | bitfield(x2b, 27, 2)
        | bitfield(r3, 20, 7)
        | bitfield(r2, 13, 7)
        | bitfield(r1, 6, 7)
        | bitfield(qp, 0, 6)
    )

def add(r1, r2, r3, qp=0):
    return alu_reg(0x0, 0x0, r1, r2, r3, qp)

def add_one(r1, r2, r3, qp=0):
    return alu_reg(0x0, 0x1, r1, r2, r3, qp)

def sub_reg(r1, r2, r3, qp=0):
    return alu_reg(0x1, 0x1, r1, r2, r3, qp)

def or_reg(r1, r2, r3, qp=0):
    return alu_reg(0x3, 0x2, r1, r2, r3, qp)

def movl_mlx(r1, imm64, qp=0):
    imm64 &= 0xffffffffffffffff
    imm7b = imm64 & 0x7f
    imm9d = (imm64 >> 7) & 0x1ff
    imm5c = (imm64 >> 16) & 0x1f
    ic = (imm64 >> 21) & 1
    imm41 = (imm64 >> 22) & 0x1ffffffffff
    i = (imm64 >> 63) & 1
    l_slot = imm41
    x_slot = (
        op(6)
        | bitfield(i, 36, 1)
        | bitfield(imm9d, 27, 9)
        | bitfield(imm5c, 22, 5)
        | bitfield(ic, 21, 1)
        | bitfield(imm7b, 13, 7)
        | bitfield(r1, 6, 7)
        | bitfield(qp, 0, 6)
    )
    return (0x04, nop_m(), l_slot, x_slot)

def andcm_imm(r1, imm, r3, qp=0):
    encoded = imm & 0xff
    return (
        op(8)
        | bitfield(0xb, 29, 4)
        | bitfield(1, 27, 2)
        | bitfield((encoded >> 7) & 1, 36, 1)
        | bitfield(encoded & 0x7f, 13, 7)
        | bitfield(r3, 20, 7)
        | bitfield(r1, 6, 7)
        | bitfield(qp, 0, 6)
    )

def tnat_z(p1, p2, r2, qp=0):
    return (
        op(5)
        | bitfield(1, 13, 1)
        | bitfield(p1, 6, 6)
        | bitfield(p2, 27, 6)
        | bitfield(r2, 20, 7)
        | bitfield(qp, 0, 6)
    )

def tnat_z_unc(p1, p2, r2, qp=0):
    return tnat_z(p1, p2, r2, qp) | bitfield(1, 12, 1)

def tnat_nz_or(p1, p2, r2, qp=0):
    return (
        op(5)
        | bitfield(1, 12, 1)
        | bitfield(1, 13, 1)
        | bitfield(p1, 6, 6)
        | bitfield(p2, 27, 6)
        | bitfield(1, 33, 1)
        | bitfield(r2, 20, 7)
        | bitfield(qp, 0, 6)
    )

def tnat_nz_and(p1, p2, r2, ignored=0, qp=0):
    return (
        op(5)
        | bitfield(1, 12, 1)
        | bitfield(1, 13, 1)
        | bitfield(ignored, 14, 6)
        | bitfield(p1, 6, 6)
        | bitfield(p2, 27, 6)
        | bitfield(1, 36, 1)
        | bitfield(r2, 20, 7)
        | bitfield(qp, 0, 6)
    )

def popcnt(r1, r3, qp=0, ignored=0):
    return (
        op(7)
        | bitfield(0x12, 27, 6)
        | bitfield(ignored, 27, 1)
        | bitfield(3, 33, 3)
        | bitfield(r3, 20, 7)
        | bitfield(r1, 6, 7)
        | bitfield(qp, 0, 6)
    )

def clz(r1, r3, qp=0, ignored=0):
    return (
        op(7)
        | bitfield(0x1a, 27, 6)
        | bitfield(ignored, 27, 1)
        | bitfield(3, 33, 3)
        | bitfield(r3, 20, 7)
        | bitfield(r1, 6, 7)
        | bitfield(qp, 0, 6)
    )

def shl_var(r1, value_reg, count_reg, ignored=0, qp=0):
    return (
        op(7)
        | bitfield(1, 36, 1)
        | bitfield(1, 33, 1)
        | bitfield(8, 27, 6)
        | bitfield(ignored, 27, 1)
        | bitfield(count_reg, 20, 7)
        | bitfield(value_reg, 13, 7)
        | bitfield(r1, 6, 7)
        | bitfield(qp, 0, 6)
    )

def shr_u_var(r1, value_reg, count_reg, qp=0):
    return (
        op(7)
        | bitfield(1, 36, 1)
        | bitfield(1, 33, 1)
        | bitfield(0, 27, 6)
        | bitfield(value_reg, 20, 7)
        | bitfield(count_reg, 13, 7)
        | bitfield(r1, 6, 7)
        | bitfield(qp, 0, 6)
    )

def shr_var(r1, value_reg, count_reg, qp=0):
    return (
        op(7)
        | bitfield(1, 36, 1)
        | bitfield(1, 33, 1)
        | bitfield(4, 27, 6)
        | bitfield(value_reg, 20, 7)
        | bitfield(count_reg, 13, 7)
        | bitfield(r1, 6, 7)
        | bitfield(qp, 0, 6)
    )

def mpy4(r1, r2, r3, ignored=0, qp=0):
    return (
        op(7)
        | bitfield(1, 36, 1)
        | bitfield(0x1a | (ignored & 1), 27, 6)
        | bitfield(r3, 20, 7)
        | bitfield(r2, 13, 7)
        | bitfield(r1, 6, 7)
        | bitfield(qp, 0, 6)
    )

def mpyshl4(r1, r2, r3, ignored=0, qp=0):
    return (
        op(7)
        | bitfield(1, 36, 1)
        | bitfield(0x1e | (ignored & 1), 27, 6)
        | bitfield(r3, 20, 7)
        | bitfield(r2, 13, 7)
        | bitfield(r1, 6, 7)
        | bitfield(qp, 0, 6)
    )

def cmp_lt_unc_imm(p1, p2, imm, r3, qp=0):
    encoded = imm & 0xff
    return (
        op(0xc)
        | bitfield(1, 12, 1)
        | bitfield(2, 34, 2)
        | bitfield((encoded >> 7) & 1, 36, 1)
        | bitfield(encoded & 0x7f, 13, 7)
        | bitfield(r3, 20, 7)
        | bitfield(p2, 27, 6)
        | bitfield(p1, 6, 6)
        | bitfield(qp, 0, 6)
    )

def cmp4_lt_unc(p1, p2, r2, r3, qp=0):
    return (
        op(0xc)
        | bitfield(1, 12, 1)
        | bitfield(1, 34, 2)
        | bitfield(r3, 20, 7)
        | bitfield(r2, 13, 7)
        | bitfield(p2, 27, 6)
        | bitfield(p1, 6, 6)
        | bitfield(qp, 0, 6)
    )

def cmp_ltu_unc(p1, p2, r2, r3, qp=0):
    return (
        op(0xd)
        | bitfield(1, 12, 1)
        | bitfield(r3, 20, 7)
        | bitfield(r2, 13, 7)
        | bitfield(p2, 27, 6)
        | bitfield(p1, 6, 6)
        | bitfield(qp, 0, 6)
    )

def cmp4_ltu_unc(p1, p2, r2, r3, qp=0):
    return cmp_ltu_unc(p1, p2, r2, r3, qp) | bitfield(1, 34, 2)

def cmp_ltu_imm(p1, p2, imm, r3, qp=0):
    encoded = imm & 0xff
    return (
        op(0xd)
        | bitfield(2, 34, 2)
        | bitfield((encoded >> 7) & 1, 36, 1)
        | bitfield(encoded & 0x7f, 13, 7)
        | bitfield(r3, 20, 7)
        | bitfield(p2, 27, 6)
        | bitfield(p1, 6, 6)
        | bitfield(qp, 0, 6)
    )

def cmp4_ltu_imm(p1, p2, imm, r3, qp=0):
    return cmp_ltu_imm(p1, p2, imm, r3, qp) | bitfield(3, 34, 2)

def cmp4_ltu_unc_imm(p1, p2, imm, r3, qp=0):
    return cmp4_ltu_imm(p1, p2, imm, r3, qp) | bitfield(1, 12, 1)

def cmp4_eq_unc_imm(p1, p2, imm, r3, qp=0):
    encoded = imm & 0xff
    return (
        op(0xe)
        | bitfield(1, 12, 1)
        | bitfield(3, 34, 2)
        | bitfield((encoded >> 7) & 1, 36, 1)
        | bitfield(encoded & 0x7f, 13, 7)
        | bitfield(r3, 20, 7)
        | bitfield(p2, 27, 6)
        | bitfield(p1, 6, 6)
        | bitfield(qp, 0, 6)
    )

def tbit_z(p1, p2, r3, bit, qp=0):
    return (
        op(5)
        | bitfield(bit, 14, 6)
        | bitfield(r3, 20, 7)
        | bitfield(p2, 27, 6)
        | bitfield(p1, 6, 6)
        | bitfield(qp, 0, 6)
    )

def tbit_z_unc(p1, p2, r3, bit, qp=0):
    return tbit_z(p1, p2, r3, bit, qp) | bitfield(1, 12, 1)

def tbit_z_and(p1, p2, r3, bit, qp=0):
    return tbit_z(p1, p2, r3, bit, qp) | bitfield(1, 36, 1)

def tbit_z_or(p1, p2, r3, bit, qp=0):
    return tbit_z(p1, p2, r3, bit, qp) | bitfield(1, 33, 1)

def _tf_form(p1, p2, feature, nz=False, update="normal", qp=0):
    imm5 = feature - 32
    value = (
        op(5)
        | bitfield(1, 13, 1)
        | bitfield(imm5, 14, 5)
        | bitfield(1, 19, 1)
        | bitfield(p2, 27, 6)
        | bitfield(p1, 6, 6)
        | bitfield(qp, 0, 6)
    )
    if nz:
        value |= bitfield(1, 12, 1)
    if update in ("and", "or_andcm"):
        value |= bitfield(1, 36, 1)
    if update in ("or", "or_andcm"):
        value |= bitfield(1, 33, 1)
    return value

def tf_z(p1, p2, feature, qp=0):
    return _tf_form(p1, p2, feature, qp=qp)

def tf_z_unc(p1, p2, feature, qp=0):
    return _tf_form(p1, p2, feature, nz=True, qp=qp)

def tf_nz_and(p1, p2, feature, qp=0):
    return _tf_form(p1, p2, feature, nz=True, update="and", qp=qp)

def tf_nz_or_andcm(p1, p2, feature, qp=0):
    return _tf_form(p1, p2, feature, nz=True, update="or_andcm", qp=qp)

def cmp4_eq_imm(p1, p2, imm, r3, qp=0):
    encoded = imm & 0xff
    return (
        op(0xe)
        | bitfield(3, 34, 2)
        | bitfield((encoded >> 7) & 1, 36, 1)
        | bitfield(encoded & 0x7f, 13, 7)
        | bitfield(r3, 20, 7)
        | bitfield(p2, 27, 6)
        | bitfield(p1, 6, 6)
        | bitfield(qp, 0, 6)
    )

def cmp_eq_imm(p1, p2, imm, r3, qp=0):
    return cmp4_eq_imm(p1, p2, imm, r3, qp) & ~bitfield(1, 34, 1)

def cmp_eq_and(p1, p2, r2, r3, qp=0):
    return (
        op(0xc)
        | bitfield(1, 33, 1)
        | bitfield(r3, 20, 7)
        | bitfield(r2, 13, 7)
        | bitfield(p2, 27, 6)
        | bitfield(p1, 6, 6)
        | bitfield(qp, 0, 6)
    )

def cmp_ne_and(p1, p2, r2, r3, qp=0):
    return cmp_eq_and(p1, p2, r2, r3, qp) | bitfield(1, 12, 1)

def cmp4_eq_and(p1, p2, r2, r3, qp=0):
    return cmp_eq_and(p1, p2, r2, r3, qp) | bitfield(1, 34, 2)

def cmp_ge_and(p1, p2, r3, ignored=0, qp=0):
    return (
        op(0xc)
        | bitfield(1, 33, 1)
        | bitfield(1, 36, 1)
        | bitfield(ignored, 13, 7)
        | bitfield(r3, 20, 7)
        | bitfield(p2, 27, 6)
        | bitfield(p1, 6, 6)
        | bitfield(qp, 0, 6)
    )

def cmp_gt_and(p1, p2, r3, ignored=0, qp=0):
    return (
        op(0xc)
        | bitfield(1, 36, 1)
        | bitfield(ignored, 13, 7)
        | bitfield(r3, 20, 7)
        | bitfield(p2, 27, 6)
        | bitfield(p1, 6, 6)
        | bitfield(qp, 0, 6)
    )

def cmp_le_or(p1, p2, r3, ignored=0, qp=0):
    return (
        op(0xd)
        | bitfield(1, 12, 1)
        | bitfield(1, 36, 1)
        | bitfield(ignored, 13, 7)
        | bitfield(r3, 20, 7)
        | bitfield(p2, 27, 6)
        | bitfield(p1, 6, 6)
        | bitfield(qp, 0, 6)
    )

def cmp_ne_or_andcm(p1, p2, r2, r3, qp=0):
    return (
        op(0xe)
        | bitfield(1, 12, 1)
        | bitfield(1, 33, 1)
        | bitfield(r3, 20, 7)
        | bitfield(r2, 13, 7)
        | bitfield(p2, 27, 6)
        | bitfield(p1, 6, 6)
        | bitfield(qp, 0, 6)
    )

def cmp_ge_or(p1, p2, r3, qp=0):
    return (
        op(0xd)
        | bitfield(1, 33, 1)
        | bitfield(1, 36, 1)
        | bitfield(r3, 20, 7)
        | bitfield(p2, 27, 6)
        | bitfield(p1, 6, 6)
        | bitfield(qp, 0, 6)
    )

def cmp_ge_or_issue_raw(p1, p2, r3, ignored, qp=0):
    return cmp_ge_or(p1, p2, r3, qp) | bitfield(ignored, 13, 7)

def cmp_ge_or_andcm_issue_raw(p1, p2, r3, ignored, qp=0):
    return (
        op(0xe)
        | bitfield(1, 33, 1)
        | bitfield(1, 36, 1)
        | bitfield(ignored, 13, 7)
        | bitfield(r3, 20, 7)
        | bitfield(p2, 27, 6)
        | bitfield(p1, 6, 6)
        | bitfield(qp, 0, 6)
    )

def cmp_ne_or_andcm_imm(p1, p2, imm, r3, qp=0):
    encoded = imm & 0xff
    return (
        op(0xe)
        | bitfield(1, 12, 1)
        | bitfield(1, 33, 1)
        | bitfield(2, 34, 2)
        | bitfield((encoded >> 7) & 1, 36, 1)
        | bitfield(encoded & 0x7f, 13, 7)
        | bitfield(r3, 20, 7)
        | bitfield(p2, 27, 6)
        | bitfield(p1, 6, 6)
        | bitfield(qp, 0, 6)
    )

def _cmp_update_imm(major, p1, p2, imm, r3, cmp4=False, ne=False, qp=0):
    encoded = imm & 0xff
    return (
        op(major)
        | bitfield(1 if ne else 0, 12, 1)
        | bitfield(1, 33, 1)
        | bitfield(3 if cmp4 else 2, 34, 2)
        | bitfield((encoded >> 7) & 1, 36, 1)
        | bitfield(encoded & 0x7f, 13, 7)
        | bitfield(r3, 20, 7)
        | bitfield(p2, 27, 6)
        | bitfield(p1, 6, 6)
        | bitfield(qp, 0, 6)
    )

def cmp_eq_and_imm(p1, p2, imm, r3, qp=0):
    return _cmp_update_imm(0xc, p1, p2, imm, r3, qp=qp)

def cmp_ne_and_imm(p1, p2, imm, r3, qp=0):
    return _cmp_update_imm(0xc, p1, p2, imm, r3, ne=True, qp=qp)

def cmp_eq_or_imm(p1, p2, imm, r3, qp=0):
    return _cmp_update_imm(0xd, p1, p2, imm, r3, qp=qp)

def cmp_ne_or_imm(p1, p2, imm, r3, qp=0):
    return _cmp_update_imm(0xd, p1, p2, imm, r3, ne=True, qp=qp)

def cmp4_eq_and_imm(p1, p2, imm, r3, qp=0):
    return _cmp_update_imm(0xc, p1, p2, imm, r3, cmp4=True, qp=qp)

def cmp4_ne_and_imm(p1, p2, imm, r3, qp=0):
    return _cmp_update_imm(0xc, p1, p2, imm, r3, cmp4=True, ne=True, qp=qp)

def cmp4_eq_or_imm(p1, p2, imm, r3, qp=0):
    return _cmp_update_imm(0xd, p1, p2, imm, r3, cmp4=True, qp=qp)

def cmp4_ne_or_imm(p1, p2, imm, r3, qp=0):
    return _cmp_update_imm(0xd, p1, p2, imm, r3, cmp4=True, ne=True, qp=qp)

def cmp4_ne_or_andcm(p1, p2, r2, r3, qp=0):
    return cmp_ne_or_andcm(p1, p2, r2, r3, qp) | bitfield(1, 34, 2)

def cmp4_eq_or(p1, p2, r2, r3, qp=0):
    return (
        op(0xd)
        | bitfield(1, 33, 1)
        | bitfield(1, 34, 2)
        | bitfield(r3, 20, 7)
        | bitfield(r2, 13, 7)
        | bitfield(p2, 27, 6)
        | bitfield(p1, 6, 6)
        | bitfield(qp, 0, 6)
    )

def cmp4_ne_or(p1, p2, r2, r3, qp=0):
    return cmp4_eq_or(p1, p2, r2, r3, qp) | bitfield(1, 12, 1)

def cmp4_ge_or_andcm(p1, p2, r3, qp=0):
    return (
        op(0xe)
        | bitfield(1, 33, 1)
        | bitfield(1, 34, 2)
        | bitfield(1, 36, 1)
        | bitfield(r3, 20, 7)
        | bitfield(p2, 27, 6)
        | bitfield(p1, 6, 6)
        | bitfield(qp, 0, 6)
    )

def dep(r1, r2, r3, pos, length, qp=0):
    cpos = 63 - pos
    encoded_len = length - 1
    return (
        op(4)
        | bitfield(encoded_len & 0xf, 27, 4)
        | bitfield(cpos & 0x3, 31, 2)
        | bitfield((cpos >> 2) & 1, 33, 1)
        | bitfield((cpos >> 3) & 0x3, 34, 2)
        | bitfield((cpos >> 5) & 1, 36, 1)
        | bitfield(r3, 20, 7)
        | bitfield(r2, 13, 7)
        | bitfield(r1, 6, 7)
        | bitfield(qp, 0, 6)
    )

def depz_reg(r1, r2, pos, length, qp=0):
    cpos = 63 - pos
    return (
        op(5)
        | bitfield(r1, 6, 7)
        | bitfield(r2, 13, 7)
        | bitfield(cpos, 20, 7)
        | bitfield(length - 1, 27, 6)
        | bitfield(1, 33, 1)
        | bitfield(1, 34, 2)
        | bitfield(qp, 0, 6)
    )

def depz_imm(r1, imm, pos, length, qp=0):
    imm8 = imm & 0xff
    cpos = 127 - pos
    return (
        op(5)
        | bitfield(r1, 6, 7)
        | bitfield(imm8 & 0x7f, 13, 7)
        | bitfield(cpos, 20, 7)
        | bitfield(length - 1, 27, 6)
        | bitfield(1, 33, 1)
        | bitfield(1, 34, 2)
        | bitfield((imm8 >> 7) & 1, 36, 1)
        | bitfield(qp, 0, 6)
    )

def extr_u(r1, r3, pos, length, bit36=0, qp=0):
    return (
        op(5)
        | bitfield(r1, 6, 7)
        | bitfield(pos << 1, 13, 7)
        | bitfield(r3, 20, 7)
        | bitfield(length - 1, 27, 6)
        | bitfield(1, 34, 2)
        | bitfield(bit36, 36, 1)
        | bitfield(qp, 0, 6)
    )

def extr(r1, r3, pos, length, qp=0):
    return extr_u(r1, r3, pos, length, qp=qp) | bitfield(1, 13, 1)

def sxt1(r1, r3, qp=0):
    return (bitfield(0x14, 27, 6) | bitfield(r3, 20, 7) |
            bitfield(r1, 6, 7) | bitfield(qp, 0, 6))

def shr_u_imm(r1, r3, imm, qp=0):
    return (
        op(5)
        | bitfield(1, 34, 2)
        | bitfield(63 - imm, 27, 6)
        | bitfield(r3, 20, 7)
        | bitfield(imm << 1, 13, 7)
        | bitfield(r1, 6, 7)
        | bitfield(qp, 0, 6)
    )

def shrp_imm(r1, r2, r3, imm, qp=0, ignored=0):
    return (
        op(5)
        | bitfield(3, 34, 2)
        | bitfield(ignored, 36, 1)
        | bitfield(r3, 20, 7)
        | bitfield(r2, 13, 7)
        | bitfield(r1, 6, 7)
        | bitfield(imm, 27, 6)
        | bitfield(qp, 0, 6)
    )

def reserved_a1_x4_5_x2b_1(r1, r2, r3, qp=0):
    return (
        op(8)
        | bitfield(5, 29, 4)
        | bitfield(1, 27, 2)
        | bitfield(r3, 20, 7)
        | bitfield(r2, 13, 7)
        | bitfield(r1, 6, 7)
        | bitfield(qp, 0, 6)
    )

def shladd(r1, r2, shift, r3, qp=0):
    return (
        op(8)
        | bitfield(4, 29, 4)
        | bitfield(shift - 1, 27, 2)
        | bitfield(r3, 20, 7)
        | bitfield(r2, 13, 7)
        | bitfield(r1, 6, 7)
        | bitfield(qp, 0, 6)
    )

def shladdp4(r1, r2, shift, r3, qp=0):
    return (
        op(8)
        | bitfield(6, 29, 4)
        | bitfield(shift - 1, 27, 2)
        | bitfield(r3, 20, 7)
        | bitfield(r2, 13, 7)
        | bitfield(r1, 6, 7)
        | bitfield(qp, 0, 6)
    )

def addp4(r1, r2, r3, qp=0):
    return (
        op(8)
        | bitfield(2, 29, 4)
        | bitfield(r3, 20, 7)
        | bitfield(r2, 13, 7)
        | bitfield(r1, 6, 7)
        | bitfield(qp, 0, 6)
    )

def addp4_imm(r1, imm, r3, qp=0):
    encoded = imm & 0x3fff
    return (
        op(8)
        | bitfield(3, 34, 2)
        | bitfield(encoded & 0x7f, 13, 7)
        | bitfield((encoded >> 7) & 0x3f, 27, 6)
        | bitfield((encoded >> 13) & 1, 36, 1)
        | bitfield(r3, 20, 7)
        | bitfield(r1, 6, 7)
        | bitfield(qp, 0, 6)
    )

__all__ = (
    'adds',
    'addl',
    'alu_reg',
    'add',
    'add_one',
    'sub_reg',
    'or_reg',
    'movl_mlx',
    'andcm_imm',
    'tnat_z',
    'tnat_z_unc',
    'tnat_nz_or',
    'tnat_nz_and',
    'popcnt',
    'clz',
    'shl_var',
    'shr_u_var',
    'shr_var',
    'mpy4',
    'mpyshl4',
    'cmp_lt_unc_imm',
    'cmp4_lt_unc',
    'cmp_ltu_unc',
    'cmp4_ltu_unc',
    'cmp_ltu_imm',
    'cmp4_ltu_imm',
    'cmp4_ltu_unc_imm',
    'cmp4_eq_unc_imm',
    'tbit_z',
    'tbit_z_unc',
    'tbit_z_and',
    'tbit_z_or',
    'tf_z',
    'tf_z_unc',
    'tf_nz_and',
    'tf_nz_or_andcm',
    'cmp4_eq_imm',
    'cmp_eq_imm',
    'cmp_eq_and',
    'cmp_ne_and',
    'cmp4_eq_and',
    'cmp_ge_and',
    'cmp_gt_and',
    'cmp_le_or',
    'cmp_ne_or_andcm',
    'cmp_ge_or',
    'cmp_ge_or_issue_raw',
    'cmp_ge_or_andcm_issue_raw',
    'cmp_ne_or_andcm_imm',
    'cmp_eq_and_imm',
    'cmp_ne_and_imm',
    'cmp_eq_or_imm',
    'cmp_ne_or_imm',
    'cmp4_eq_and_imm',
    'cmp4_ne_and_imm',
    'cmp4_eq_or_imm',
    'cmp4_ne_or_imm',
    'cmp4_ne_or_andcm',
    'cmp4_eq_or',
    'cmp4_ne_or',
    'cmp4_ge_or_andcm',
    'dep',
    'depz_reg',
    'depz_imm',
    'extr_u',
    'extr',
    'sxt1',
    'shr_u_imm',
    'shrp_imm',
    'reserved_a1_x4_5_x2b_1',
    'shladd',
    'shladdp4',
    'addp4',
    'addp4_imm',
)
