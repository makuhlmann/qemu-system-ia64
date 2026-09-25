# IA-64 instruction encoders split on translator family boundaries.

from .encoding_common import bitfield, op
from .encoding_branch import branch_target_field

def load_mem(x6, r1, r3, qp=0):
    return (op(4) | bitfield(x6, 30, 6) | bitfield(r3, 20, 7) |
            bitfield(r1, 6, 7) | bitfield(qp, 0, 6))

def load_mem_reg_postinc(x6, r1, r3, r2, qp=0):
    return (
        op(4)
        | bitfield(1, 36, 1)
        | bitfield(x6, 30, 6)
        | bitfield(r3, 20, 7)
        | bitfield(r2, 13, 7)
        | bitfield(r1, 6, 7)
        | bitfield(qp, 0, 6)
    )

def load_mem_postinc(x6, r1, r3, imm, qp=0):
    encoded = imm & 0x1ff
    return (
        op(5)
        | bitfield(x6, 30, 6)
        | bitfield((encoded >> 7) & 1, 27, 1)
        | bitfield((encoded >> 8) & 1, 36, 1)
        | bitfield(r3, 20, 7)
        | bitfield(encoded & 0x7f, 13, 7)
        | bitfield(r1, 6, 7)
        | bitfield(qp, 0, 6)
    )

def ld1(r1, r3, qp=0):
    return load_mem(0x00, r1, r3, qp)

def ld2(r1, r3, qp=0):
    return load_mem(0x01, r1, r3, qp)

def ld2_s(r1, r3, qp=0):
    return load_mem(0x05, r1, r3, qp)

def ld2_bias(r1, r3, qp=0):
    return load_mem(0x11, r1, r3, qp)

def ld4(r1, r3, qp=0):
    return load_mem(0x02, r1, r3, qp)

def ld4_postinc(r1, r3, imm, qp=0):
    encoded = imm & 0x1ff
    return (
        op(5)
        | bitfield(0x02, 30, 6)
        | bitfield((encoded >> 7) & 1, 27, 1)
        | bitfield((encoded >> 8) & 1, 36, 1)
        | bitfield(r3, 20, 7)
        | bitfield(encoded & 0x7f, 13, 7)
        | bitfield(r1, 6, 7)
        | bitfield(qp, 0, 6)
    )

def ld8(r1, r3, qp=0):
    return load_mem(0x03, r1, r3, qp)

def ld16(r1, r3, qp=0):
    return (
        op(4)
        | bitfield(0x28, 30, 6)
        | bitfield(1, 27, 1)
        | bitfield(r3, 20, 7)
        | bitfield(r1, 6, 7)
        | bitfield(qp, 0, 6)
    )

def ld16_acq(r1, r3, qp=0, hint=0):
    return (
        op(4)
        | bitfield(0x2c, 30, 6)
        | bitfield(hint, 28, 2)
        | bitfield(1, 27, 1)
        | bitfield(r3, 20, 7)
        | bitfield(r1, 6, 7)
        | bitfield(qp, 0, 6)
    )

def ld8_s(r1, r3, qp=0):
    return load_mem(0x07, r1, r3, qp)

def ld8_s_hint(r1, r3, hint, qp=0):
    return ld8_s(r1, r3, qp) | bitfield(hint, 28, 2)

def ld8_postinc(r1, r3, imm, hint=0, qp=0):
    encoded = imm & 0x1ff
    return (
        op(5)
        | bitfield(0x03, 30, 6)
        | bitfield(hint, 28, 2)
        | bitfield((encoded >> 7) & 1, 27, 1)
        | bitfield((encoded >> 8) & 1, 36, 1)
        | bitfield(r3, 20, 7)
        | bitfield(encoded & 0x7f, 13, 7)
        | bitfield(r1, 6, 7)
        | bitfield(qp, 0, 6)
    )

def ld8_s_postinc(r1, r3, imm, hint=0, qp=0):
    return ld8_postinc(r1, r3, imm, hint, qp) | bitfield(0x07, 30, 6)

def ld2_c_clr_reg_update(r1, r3, r2, hint=0, qp=0):
    return (
        op(4)
        | bitfield(0x21, 30, 6)
        | bitfield(hint, 28, 2)
        | bitfield(1, 36, 1)
        | bitfield(r3, 20, 7)
        | bitfield(r2, 13, 7)
        | bitfield(r1, 6, 7)
        | bitfield(qp, 0, 6)
    )

def ld8_fill_postinc(r1, r3, imm, qp=0):
    encoded = imm & 0x1ff
    return (
        op(5)
        | bitfield(0x1b, 30, 6)
        | bitfield((encoded >> 7) & 1, 27, 1)
        | bitfield((encoded >> 8) & 1, 36, 1)
        | bitfield(r3, 20, 7)
        | bitfield(encoded & 0x7f, 13, 7)
        | bitfield(r1, 6, 7)
        | bitfield(qp, 0, 6)
    )

def ldf8(f1, r3, hint=0, qp=0):
    return (
        op(6)
        | bitfield(0x01, 30, 6)
        | bitfield(hint, 28, 2)
        | bitfield(r3, 20, 7)
        | bitfield(f1, 6, 7)
        | bitfield(qp, 0, 6)
    )

def ldfs(f1, r3, hint=0, qp=0):
    return (
        op(6)
        | bitfield(0x02, 30, 6)
        | bitfield(hint, 28, 2)
        | bitfield(r3, 20, 7)
        | bitfield(f1, 6, 7)
        | bitfield(qp, 0, 6)
    )

def ldfd(f1, r3, hint=0, qp=0):
    return (
        op(6)
        | bitfield(0x03, 30, 6)
        | bitfield(hint, 28, 2)
        | bitfield(r3, 20, 7)
        | bitfield(f1, 6, 7)
        | bitfield(qp, 0, 6)
    )

def ldfps(f1, f2, r3, hint=0, qp=0):
    return (
        op(6)
        | bitfield(0x02, 30, 6)
        | bitfield(hint, 28, 2)
        | bitfield(1, 27, 1)
        | bitfield(r3, 20, 7)
        | bitfield(f2, 13, 7)
        | bitfield(f1, 6, 7)
        | bitfield(qp, 0, 6)
    )

def ldfe(f1, r3, hint=0, qp=0):
    return (
        op(6)
        | bitfield(hint, 28, 2)
        | bitfield(r3, 20, 7)
        | bitfield(f1, 6, 7)
        | bitfield(qp, 0, 6)
    )

def ldf8_s(f1, r3, hint=0, qp=0):
    return ldf8(f1, r3, hint, qp) | bitfield(0x05, 30, 6)

def ldf8_a(f1, r3, hint=0, qp=0):
    return ldf8(f1, r3, hint, qp) | bitfield(0x09, 30, 6)

def ldf8_sa(f1, r3, hint=0, qp=0):
    return ldf8(f1, r3, hint, qp) | bitfield(0x0d, 30, 6)

def ldf8_c_nc(f1, r3, hint=0, qp=0):
    return ldf8(f1, r3, hint, qp) | bitfield(0x25, 30, 6)

def ldfp8_postinc(f1, f2, r3, hint=0, qp=0):
    return (
        op(6)
        | bitfield(1, 36, 1)
        | bitfield(0x01, 30, 6)
        | bitfield(hint, 28, 2)
        | bitfield(1, 27, 1)
        | bitfield(r3, 20, 7)
        | bitfield(f2, 13, 7)
        | bitfield(f1, 6, 7)
        | bitfield(qp, 0, 6)
    )

def ldf_fill_postinc(f1, r3, imm, qp=0):
    encoded = imm & 0x1ff
    return (
        op(7)
        | bitfield(0x1b, 30, 6)
        | bitfield((encoded >> 7) & 1, 27, 1)
        | bitfield((encoded >> 8) & 1, 36, 1)
        | bitfield(r3, 20, 7)
        | bitfield(encoded & 0x7f, 13, 7)
        | bitfield(f1, 6, 7)
        | bitfield(qp, 0, 6)
    )

def store_mem(x6, r3, r2, qp=0):
    return (op(4) | bitfield(x6, 30, 6) | bitfield(r3, 20, 7) |
            bitfield(r2, 13, 7) | bitfield(qp, 0, 6))

def st2(r3, r2, qp=0):
    return store_mem(0x31, r3, r2, qp)

def st4(r3, r2, qp=0):
    return store_mem(0x32, r3, r2, qp)

def st4_rel(r3, r2, qp=0):
    return store_mem(0x36, r3, r2, qp)

def store_mem_postinc(x6, r3, r2, imm, qp=0):
    encoded = imm & 0x1ff
    return (
        op(5)
        | bitfield(x6, 30, 6)
        | bitfield((encoded >> 7) & 1, 27, 1)
        | bitfield((encoded >> 8) & 1, 36, 1)
        | bitfield(r3, 20, 7)
        | bitfield(r2, 13, 7)
        | bitfield(encoded & 0x7f, 6, 7)
        | bitfield(qp, 0, 6)
    )

def st4_postinc(r3, r2, imm, qp=0):
    return store_mem_postinc(0x32, r3, r2, imm, qp)

def reserved_m_major2(qp=0):
    return op(2) | bitfield(qp, 0, 6)

def reserved_memory_selector(major, m, x, x6, qp=0):
    return (
        op(major)
        | bitfield(m, 36, 1)
        | bitfield(x6, 30, 6)
        | bitfield(x, 27, 1)
        | bitfield(qp, 0, 6)
    )

def cmpxchg4(r1, r3, r2, qp=0):
    # M/A major opcode 2 is reserved (SDM Vol 3 Table 4-3): every cmpxchg is
    # M16 in major opcode 4 with an ordering completer.
    return cmpxchg_acq(2, r1, r3, r2, qp)

def cmpxchg_acq(size_log2, r1, r3, r2, qp=0, hint=0):
    return (
        op(4)
        | bitfield(1, 27, 1)
        | bitfield(hint, 28, 2)
        | bitfield(size_log2 & 1, 30, 1)
        | bitfield(size_log2 >> 1, 31, 1)
        | bitfield(r3, 20, 7)
        | bitfield(r2, 13, 7)
        | bitfield(r1, 6, 7)
        | bitfield(qp, 0, 6)
    )

def cmpxchg4_acq(r1, r3, r2, qp=0):
    return cmpxchg_acq(2, r1, r3, r2, qp)

def cmpxchg_rel(size_log2, r1, r3, r2, qp=0, hint=0):
    return cmpxchg_acq(size_log2, r1, r3, r2, qp, hint) | bitfield(1, 32, 1)

def cmp8xchg16_acq(r1, r3, r2, qp=0, hint=0):
    return (
        op(4)
        | bitfield(1, 27, 1)
        | bitfield(hint, 28, 2)
        | bitfield(0x20, 30, 6)
        | bitfield(r3, 20, 7)
        | bitfield(r2, 13, 7)
        | bitfield(r1, 6, 7)
        | bitfield(qp, 0, 6)
    )

def cmp8xchg16_rel(r1, r3, r2, qp=0, hint=0):
    return cmp8xchg16_acq(r1, r3, r2, qp, hint) | bitfield(1, 32, 1)

def xchg(size_log2, r1, r3, r2, qp=0, hint=0):
    return (
        op(4)
        | bitfield(1, 27, 1)
        | bitfield(hint, 28, 2)
        | bitfield(size_log2 & 1, 30, 1)
        | bitfield(size_log2 >> 1, 31, 1)
        | bitfield(1, 33, 1)
        | bitfield(r3, 20, 7)
        | bitfield(r2, 13, 7)
        | bitfield(r1, 6, 7)
        | bitfield(qp, 0, 6)
    )

def xchg4(r1, r3, r2, qp=0):
    return xchg(2, r1, r3, r2, qp)

def st8(r3, r2, qp=0):
    return store_mem(0x33, r3, r2, qp)

def st8_postinc(r3, r2, imm, qp=0):
    encoded = imm & 0x1ff
    return (
        op(5)
        | bitfield(0x33, 30, 6)
        | bitfield((encoded >> 7) & 1, 27, 1)
        | bitfield((encoded >> 8) & 1, 36, 1)
        | bitfield(r3, 20, 7)
        | bitfield(r2, 13, 7)
        | bitfield(encoded & 0x7f, 6, 7)
        | bitfield(qp, 0, 6)
    )

def st8_rel(r3, r2, qp=0):
    return store_mem(0x37, r3, r2, qp)

def st16(r3, r2, x6=0x01, ignored=0, qp=0):
    return (
        op(4)
        | bitfield(6, 33, 3)
        | bitfield(x6, 27, 6)
        | bitfield(r3, 20, 7)
        | bitfield(r2, 13, 7)
        | bitfield(ignored, 6, 7)
        | bitfield(qp, 0, 6)
    )

def st8_spill_postinc(r3, r2, imm, qp=0):
    encoded = imm & 0x1ff
    return (
        op(5)
        | bitfield(0x3b, 30, 6)
        | bitfield((encoded >> 7) & 1, 27, 1)
        | bitfield((encoded >> 8) & 1, 36, 1)
        | bitfield(r3, 20, 7)
        | bitfield(r2, 13, 7)
        | bitfield(encoded & 0x7f, 6, 7)
        | bitfield(qp, 0, 6)
    )

def chk_s_target_bits(source, target):
    field = branch_target_field(source, target)
    return (
        bitfield(field & 0x7f, 6, 7)
        | bitfield((field >> 7) & 0x1fff, 20, 13)
        | bitfield((field >> 20) & 1, 36, 1)
    )

def chk_a_target_bits(source, target):
    field = branch_target_field(source, target)
    return (
        bitfield(field & 0xfffff, 13, 20)
        | bitfield((field >> 20) & 1, 36, 1)
    )

def chk_s_m(r2, source=0, target=0, qp=0):
    return (
        op(1)
        | bitfield(1, 33, 1)
        | chk_s_target_bits(source, target)
        | bitfield(r2, 13, 7)
        | bitfield(qp, 0, 6)
    )

def chk_s_f(f2, source=0, target=0, qp=0):
    return (
        op(1)
        | bitfield(3, 33, 3)
        | chk_s_target_bits(source, target)
        | bitfield(f2, 13, 7)
        | bitfield(qp, 0, 6)
    )

def chk_s_i(r2, source=0, target=0, qp=0):
    return (
        bitfield(1, 33, 1)
        | chk_s_target_bits(source, target)
        | bitfield(r2, 13, 7)
        | bitfield(qp, 0, 6)
    )

def chk_a_nc_m(r1, source=0, target=0, qp=0):
    return (
        bitfield(4, 33, 3)
        | chk_a_target_bits(source, target)
        | bitfield(r1, 6, 7)
        | bitfield(qp, 0, 6)
    )

def chk_a_nc_f(f1, source=0, target=0, qp=0):
    return (
        bitfield(6, 33, 3)
        | chk_a_target_bits(source, target)
        | bitfield(f1, 6, 7)
        | bitfield(qp, 0, 6)
    )

def chk_a_clr_f(f1, source=0, target=0, qp=0):
    return (
        bitfield(7, 33, 3)
        | chk_a_target_bits(source, target)
        | bitfield(f1, 6, 7)
        | bitfield(qp, 0, 6)
    )

def chk_a_clr_m(r2, source=0, target=0, qp=0):
    return (
        bitfield(1, 33, 1)
        | bitfield(2, 34, 2)
        | chk_a_target_bits(source, target)
        | bitfield(r2, 6, 7)
        | bitfield(qp, 0, 6)
    )

def invala(qp=0):
    return bitfield(0x10, 27, 6) | bitfield(qp, 0, 6)

def invala_e_gr(r1, qp=0):
    return (bitfield(0x12, 27, 6) | bitfield(r1, 6, 7)
            | bitfield(qp, 0, 6))

def invala_e_fp(f1, qp=0):
    return (bitfield(0x13, 27, 6) | bitfield(f1, 6, 7)
            | bitfield(qp, 0, 6))

def ld1_acq(r1, r3, qp=0):
    return (op(4) | bitfield(0x14, 30, 6) | bitfield(r3, 20, 7) |
            bitfield(r1, 6, 7) | bitfield(qp, 0, 6))

def ld8_c_clr_acq(r1, r3, qp=0):
    return load_mem(0x2b, r1, r3, qp)

def ld8_a(r1, r3, qp=0):
    return (op(4) | bitfield(2, 27, 2) | bitfield(0x0b, 30, 6) |
            bitfield(r3, 20, 7) | bitfield(r1, 6, 7) |
            bitfield(qp, 0, 6))

def ld8_sa(r1, r3, qp=0):
    return (op(4) | bitfield(2, 27, 2) | bitfield(0x0f, 30, 6) |
            bitfield(r3, 20, 7) | bitfield(r1, 6, 7) |
            bitfield(qp, 0, 6))

def ld2_sa(r1, r3, qp=0):
    return (op(4) | bitfield(2, 27, 2) | bitfield(0x0d, 30, 6)
            | bitfield(r3, 20, 7) | bitfield(r1, 6, 7)
            | bitfield(qp, 0, 6))

def ld2_c_clr(r1, r3, qp=0):
    return load_mem(0x21, r1, r3, qp)

def ld4_a(r1, r3, qp=0):
    return (op(4) | bitfield(2, 27, 2) | bitfield(0x0a, 30, 6)
            | bitfield(r3, 20, 7) | bitfield(r1, 6, 7)
            | bitfield(qp, 0, 6))

def ld4_c_clr(r1, r3, qp=0):
    return (op(4) | bitfield(0x22, 30, 6)
            | bitfield(r3, 20, 7) | bitfield(r1, 6, 7)
            | bitfield(qp, 0, 6))

def ld4_c_nc(r1, r3, qp=0):
    return (op(4) | bitfield(0x26, 30, 6)
            | bitfield(r3, 20, 7) | bitfield(r1, 6, 7)
            | bitfield(qp, 0, 6))

def ld4_c_clr_acq(r1, r3, hint=0, qp=0):
    return (op(4) | bitfield(0x2a, 30, 6) | bitfield(hint, 28, 2)
            | bitfield(r3, 20, 7) | bitfield(r1, 6, 7)
            | bitfield(qp, 0, 6))

def ld8_c_clr(r1, r3, qp=0):
    return (op(4) | bitfield(0x23, 30, 6) | bitfield(r3, 20, 7)
            | bitfield(r1, 6, 7) | bitfield(qp, 0, 6))

def ld8_c_nc(r1, r3, qp=0):
    return (op(4) | bitfield(0x27, 30, 6) | bitfield(r3, 20, 7)
            | bitfield(r1, 6, 7) | bitfield(qp, 0, 6))

def fetchadd4_acq(r1, r3, inc, qp=0, hint=0, ignored=0):
    inc3 = {16: 0, 8: 1, 4: 2, 1: 3, -16: 4, -8: 5, -4: 6, -1: 7}[inc]
    return (op(4) | bitfield(0x12, 30, 6) | bitfield(1, 27, 1)
            | bitfield(hint, 28, 2) | bitfield(ignored, 16, 4)
            | bitfield(inc3, 13, 3) | bitfield(r3, 20, 7)
            | bitfield(r1, 6, 7) | bitfield(qp, 0, 6))

def fetchadd4_rel(r1, r3, inc, qp=0, hint=0, ignored=0):
    return fetchadd4_acq(r1, r3, inc, qp, hint, ignored) | bitfield(1, 32, 1)

def ld4_bias(r1, r3, qp=0):
    return (op(4) | bitfield(0x12, 30, 6) | bitfield(r3, 20, 7) |
            bitfield(r1, 6, 7) | bitfield(qp, 0, 6))

def ld1_postinc(r1, r3, imm, qp=0):
    return (op(5) | bitfield(r3, 20, 7) | bitfield(imm, 13, 7) |
            bitfield(r1, 6, 7) | bitfield(qp, 0, 6))

def ld1_reg_postinc(r1, r3, r2, qp=0):
    return (
        op(4)
        | bitfield(1, 36, 1)
        | bitfield(r3, 20, 7)
        | bitfield(r2, 13, 7)
        | bitfield(r1, 6, 7)
        | bitfield(qp, 0, 6)
    )

def ld1_sa_postinc(r1, r3, imm, qp=0):
    return (
        op(5)
        | bitfield(0x0c, 30, 6)
        | bitfield(r3, 20, 7)
        | bitfield(imm, 13, 7)
        | bitfield(r1, 6, 7)
        | bitfield(qp, 0, 6)
    )

def st1_postinc(r3, r2, imm, qp=0):
    encoded = imm & 0x1ff
    return (
        op(5)
        | bitfield(0x30, 30, 6)
        | bitfield((encoded >> 7) & 1, 27, 1)
        | bitfield((encoded >> 8) & 1, 36, 1)
        | bitfield(r3, 20, 7)
        | bitfield(r2, 13, 7)
        | bitfield(encoded & 0x7f, 6, 7)
        | bitfield(qp, 0, 6)
    )

def stf_spill_postinc(r3, r2, imm, qp=0):
    encoded = imm & 0x1ff
    return (
        op(7)
        | bitfield(0x3b, 30, 6)
        | bitfield((encoded >> 7) & 1, 27, 1)
        | bitfield((encoded >> 8) & 1, 36, 1)
        | bitfield(r3, 20, 7)
        | bitfield(r2, 13, 7)
        | bitfield(encoded & 0x7f, 6, 7)
        | bitfield(qp, 0, 6)
    )

def stf8_postinc(r3, r2, imm, qp=0):
    encoded = imm & 0x1ff
    return (
        op(7)
        | bitfield(0x31, 30, 6)
        | bitfield((encoded >> 7) & 1, 27, 1)
        | bitfield((encoded >> 8) & 1, 36, 1)
        | bitfield(r3, 20, 7)
        | bitfield(r2, 13, 7)
        | bitfield(encoded & 0x7f, 6, 7)
        | bitfield(qp, 0, 6)
    )

def stf8(r3, f2, qp=0):
    return (
        op(6)
        | bitfield(0x31, 30, 6)
        | bitfield(r3, 20, 7)
        | bitfield(f2, 13, 7)
        | bitfield(qp, 0, 6)
    )

def stfe(r3, f2, qp=0):
    return (
        op(6)
        | bitfield(0x30, 30, 6)
        | bitfield(r3, 20, 7)
        | bitfield(f2, 13, 7)
        | bitfield(qp, 0, 6)
    )

def stfs(r3, f2, qp=0):
    return (
        op(6)
        | bitfield(0x32, 30, 6)
        | bitfield(r3, 20, 7)
        | bitfield(f2, 13, 7)
        | bitfield(qp, 0, 6)
    )

def stfd(r3, f2, qp=0):
    return (
        op(6)
        | bitfield(0x33, 30, 6)
        | bitfield(r3, 20, 7)
        | bitfield(f2, 13, 7)
        | bitfield(qp, 0, 6)
    )

def probe_rw_fault(r3, imm2, qp=0):
    return (
        op(1)
        | bitfield(0x31, 27, 6)
        | bitfield(r3, 20, 7)
        | bitfield(imm2, 13, 2)
        | bitfield(qp, 0, 6)
    )

def probe_r_fault(r3, imm2, qp=0):
    return (
        op(1)
        | bitfield(0x32, 27, 6)
        | bitfield(r3, 20, 7)
        | bitfield(imm2, 13, 2)
        | bitfield(qp, 0, 6)
    )

def probe_w_fault(r3, imm2, qp=0):
    return (
        op(1)
        | bitfield(0x33, 27, 6)
        | bitfield(r3, 20, 7)
        | bitfield(imm2, 13, 2)
        | bitfield(qp, 0, 6)
    )

def lfetch_fault(r3, qp=0):
    return lfetch(r3, 0x2e, qp)

def probe_r_fault_ignored(r3, imm2, ignored5=0, ignored7=0, bit36=0, qp=0):
    return (
        op(1)
        | bitfield(bit36, 36, 1)
        | bitfield(0x32, 27, 6)
        | bitfield(r3, 20, 7)
        | bitfield(ignored5, 15, 5)
        | bitfield(imm2, 13, 2)
        | bitfield(ignored7, 6, 7)
        | bitfield(qp, 0, 6)
    )

def probe_r_imm(r1, r3, imm2, qp=0):
    return (
        op(1)
        | bitfield(0x18, 27, 6)
        | bitfield(r3, 20, 7)
        | bitfield(imm2, 13, 2)
        | bitfield(r1, 6, 7)
        | bitfield(qp, 0, 6)
    )

def probe_w_imm(r1, r3, imm2, qp=0):
    return (
        op(1)
        | bitfield(0x19, 27, 6)
        | bitfield(r3, 20, 7)
        | bitfield(imm2, 13, 2)
        | bitfield(r1, 6, 7)
        | bitfield(qp, 0, 6)
    )

def probe_r_reg(r1, r3, r2, qp=0):
    return (
        op(1)
        | bitfield(0x38, 27, 6)
        | bitfield(r3, 20, 7)
        | bitfield(r2, 13, 7)
        | bitfield(r1, 6, 7)
        | bitfield(qp, 0, 6)
    )

def probe_w_reg(r1, r3, r2, qp=0):
    return (
        op(1)
        | bitfield(0x39, 27, 6)
        | bitfield(r3, 20, 7)
        | bitfield(r2, 13, 7)
        | bitfield(r1, 6, 7)
        | bitfield(qp, 0, 6)
    )

def lfetch(r3, x6a=0x2c, qp=0):
    return (op(6) | bitfield(x6a, 30, 6) | bitfield(r3, 20, 7) |
            bitfield(qp, 0, 6))

def lfetch_reg_postinc(r3, r2, x6a=0x2c, hint=0, qp=0):
    return (
        op(6)
        | bitfield(1, 36, 1)
        | bitfield(x6a, 30, 6)
        | bitfield(hint, 28, 2)
        | bitfield(r3, 20, 7)
        | bitfield(r2, 13, 7)
        | bitfield(qp, 0, 6)
    )

def lfetch_postinc(r3, imm, x6a=0x2c, hint=0, qp=0):
    encoded = imm & 0x1ff
    return (
        op(7)
        | bitfield(x6a, 30, 6)
        | bitfield(hint, 28, 2)
        | bitfield((encoded >> 7) & 1, 27, 1)
        | bitfield((encoded >> 8) & 1, 36, 1)
        | bitfield(r3, 20, 7)
        | bitfield(encoded & 0x7f, 13, 7)
        | bitfield(qp, 0, 6)
    )

def fc_i(r3, qp=0):
    return (op(1) | bitfield(0x30, 27, 6) | bitfield(r3, 20, 7) |
            bitfield(qp, 0, 6))

__all__ = (
    'reserved_m_major2',
    'reserved_memory_selector',
    'load_mem',
    'load_mem_reg_postinc',
    'load_mem_postinc',
    'ld1',
    'ld2',
    'ld2_s',
    'ld2_bias',
    'ld4',
    'ld4_postinc',
    'ld8',
    'ld16',
    'ld16_acq',
    'ld8_s',
    'ld8_s_hint',
    'ld8_postinc',
    'ld8_s_postinc',
    'ld2_c_clr_reg_update',
    'ld8_fill_postinc',
    'ldf8',
    'ldfs',
    'ldfd',
    'ldfps',
    'ldfe',
    'ldf8_s',
    'ldf8_a',
    'ldf8_sa',
    'ldf8_c_nc',
    'ldfp8_postinc',
    'ldf_fill_postinc',
    'store_mem',
    'st2',
    'st4',
    'st4_rel',
    'store_mem_postinc',
    'st4_postinc',
    'cmpxchg4',
    'cmpxchg_acq',
    'cmpxchg4_acq',
    'cmpxchg_rel',
    'cmp8xchg16_acq',
    'cmp8xchg16_rel',
    'xchg',
    'xchg4',
    'st8',
    'st8_postinc',
    'st8_rel',
    'st16',
    'st8_spill_postinc',
    'chk_s_target_bits',
    'chk_a_target_bits',
    'chk_s_m',
    'chk_s_f',
    'chk_s_i',
    'chk_a_nc_m',
    'chk_a_nc_f',
    'chk_a_clr_f',
    'chk_a_clr_m',
    'invala',
    'invala_e_gr',
    'invala_e_fp',
    'ld1_acq',
    'ld8_c_clr_acq',
    'ld8_a',
    'ld8_sa',
    'ld2_sa',
    'ld2_c_clr',
    'ld4_a',
    'ld4_c_clr',
    'ld4_c_nc',
    'ld4_c_clr_acq',
    'ld8_c_clr',
    'ld8_c_nc',
    'fetchadd4_acq',
    'fetchadd4_rel',
    'ld4_bias',
    'ld1_postinc',
    'ld1_reg_postinc',
    'ld1_sa_postinc',
    'st1_postinc',
    'stf_spill_postinc',
    'stf8_postinc',
    'stf8',
    'stfe',
    'stfs',
    'stfd',
    'probe_rw_fault',
    'probe_r_fault',
    'probe_w_fault',
    'lfetch_fault',
    'probe_r_fault_ignored',
    'probe_r_imm',
    'probe_w_imm',
    'probe_r_reg',
    'probe_w_reg',
    'lfetch',
    'lfetch_reg_postinc',
    'lfetch_postinc',
    'fc_i',
)
