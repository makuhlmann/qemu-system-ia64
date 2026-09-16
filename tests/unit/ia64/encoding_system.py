# IA-64 instruction encoders split on translator family boundaries.

from .encoding_constants import PAL_PROC_ENTRY
from .encoding_common import (
    EndGroupInsn,
    IUnitInsn,
    MUnitAlloc,
    StartGroupInsn,
    bitfield,
    nop_i,
    nop_m,
    op,
)
from .encoding_branch import br_call, br_cond, br_ret, branch_target_field
from .encoding_integer import addl, movl_mlx

def break_m(imm=0, qp=0):
    return (
        bitfield(imm & 0xfffff, 6, 20)
        | bitfield((imm >> 20) & 1, 36, 1)
        | bitfield(qp, 0, 6)
    )

def illegal_m():
    return (
        nop_m() & ~((1 << 41) - 1)
        | bitfield(0xf, 37, 4)
        | bitfield(0x3f, 27, 6)
        | bitfield(0x3f, 30, 6)
    )

def break_x_mlx(imm62, qp=0):
    imm62 &= (1 << 62) - 1
    imm21 = imm62 & 0x1fffff
    l_slot = imm62 >> 21
    x_slot = (
        bitfield(imm21 & 0xfffff, 6, 20)
        | bitfield((imm21 >> 20) & 1, 36, 1)
        | bitfield(qp, 0, 6)
    )
    return (0x04, nop_m(), l_slot, x_slot)

def hint_x_mlx(imm62, qp=0):
    imm62 &= (1 << 62) - 1
    imm21 = imm62 & 0x1fffff
    l_slot = imm62 >> 21
    x_slot = (
        bitfield(1, 27, 6)
        | bitfield(1, 26, 1)
        | bitfield(imm21 & 0xfffff, 6, 20)
        | bitfield((imm21 >> 20) & 1, 36, 1)
        | bitfield(qp, 0, 6)
    )
    return (0x04, nop_m(), l_slot, x_slot)

def break_b():
    return 0

def break_f(imm=0, qp=0):
    return (
        bitfield(imm & 0xfffff, 6, 20)
        | bitfield((imm >> 20) & 1, 36, 1)
        | bitfield(qp, 0, 6)
    )

def clrrrb_b(qp=0, ignored=0):
    return EndGroupInsn(
        0x20000000 | bitfield(ignored, 6, 21) | bitfield(qp, 0, 6))

def clrrrb_pr_b(qp=0, ignored=0):
    return EndGroupInsn(
        0x28000000 | bitfield(ignored, 6, 21) | bitfield(qp, 0, 6))

def pal_break(qp=0):
    return break_m(0x100000, qp)

def pal_call_program(index, args=()):
    program = [
        (0x10, 0x00, nop_m(), addl(28, index, 0), nop_i()),
    ]
    addr = 0x20
    for reg, value in args:
        signed_value = value if value < (1 << 63) else value - (1 << 64)
        if -(1 << 21) <= signed_value < (1 << 21):
            program.append((addr, 0x00, nop_m(), addl(reg, value, 0),
                            nop_i()))
        else:
            program.append((addr, *movl_mlx(reg, value)))
        addr += 0x10
    program += [
        (addr, 0x10, nop_m(), nop_i(), br_call(0, addr, PAL_PROC_ENTRY)),
        (addr + 0x10, 0x10, nop_m(), nop_i(),
         br_cond(addr + 0x10, addr + 0x10)),
        (PAL_PROC_ENTRY, 0x0a, pal_break(), nop_m(), nop_i()),
        (PAL_PROC_ENTRY + 0x10, 0x10, nop_m(), nop_i(), br_ret(0)),
    ]
    return program

def pal_stacked_call_program(index, args=()):
    values = [0, 0, 0]
    for arg, value in enumerate(args[:3]):
        values[arg] = value

    return [
        (0x10, 0x00, nop_m(), alloc(2, 4, 0, 0, 0), nop_i()),
        (0x20, *movl_mlx(28, index)),
        (0x30, *movl_mlx(32, index)),
        (0x40, *movl_mlx(33, values[0])),
        (0x50, *movl_mlx(34, values[1])),
        (0x60, *movl_mlx(35, values[2])),
        (0x70, 0x10, nop_m(), nop_i(), br_call(0, 0x70, PAL_PROC_ENTRY)),
        (0x80, 0x10, nop_m(), nop_i(), br_cond(0x80, 0x80)),
        (PAL_PROC_ENTRY, 0x0a, pal_break(), nop_m(), nop_i()),
        (PAL_PROC_ENTRY + 0x10, 0x10, nop_m(), nop_i(), br_ret(0)),
    ]

def mov_br_gr(b1, r2, qp=0):
    return (
        bitfield(b1, 6, 3)
        | bitfield(r2, 13, 7)
        | bitfield(7, 33, 3)
        | bitfield(qp, 0, 6)
    )

def rfi_b(qp=0):
    return EndGroupInsn(
        bitfield(0x08, 27, 6) | bitfield(qp, 0, 6))

def rfi_to_gr(address, psr_reg, target_reg):
    """Install a full PSR and resume at the IIP held in target_reg."""
    return [
        (address, 0x09, mov_m_gr_cr(psr_reg, 16),
         mov_m_gr_cr(target_reg, 19), nop_i()),
        (address + 0x10, 0x11, nop_m(), nop_i(), rfi_b()),
    ]

def epc_b(qp=0, ignored=0):
    return bitfield(0x10, 27, 6) | bitfield(ignored, 6, 21) | bitfield(qp, 0, 6)

def mov_ar(r1, ar_num, qp=0):
    return (op(1) | bitfield(0x2a, 27, 6)
            | bitfield(ar_num, 20, 7) | bitfield(r1, 13, 7)
            | bitfield(qp, 0, 6))

def alloc(r1, sof, sol, sor, rrb, qp=0):
    return MUnitAlloc(alloc_m(r1, sof, sol, sor, rrb, qp))

def alloc_m(r1, sof, sol, sor, rrb, qp=0, ignored31=0, ignored36=0):
    return (
        op(1)
        | bitfield(6, 33, 3)
        | bitfield(sof & 0x7f, 13, 7)
        | bitfield(sol & 0x7f, 20, 7)
        | bitfield(sor & 0x0f, 27, 4)
        | bitfield(ignored31, 31, 1)
        | bitfield(rrb & 0x01, 32, 1)
        | bitfield(ignored36, 36, 1)
        | bitfield(r1, 6, 7)
        | bitfield(qp, 0, 6)
    )

def mov_ar_lc(r1, qp=0):
    return (bitfield(0x32, 27, 6) | bitfield(65, 20, 7) |
            bitfield(r1, 6, 7) | bitfield(qp, 0, 6))

def mov_i_ar_gr(r1, ar_num, qp=0):
    return (bitfield(0x32, 27, 6) | bitfield(ar_num, 20, 7) |
            bitfield(r1, 6, 7) | bitfield(qp, 0, 6))

def mov_lc_gr(r2, qp=0):
    return (bitfield(0x2a, 27, 6) | bitfield(65, 20, 7) |
            bitfield(r2, 13, 7) | bitfield(qp, 0, 6))

def mov_lc_imm(imm, qp=0):
    encoded = imm & 0xff
    return (
        bitfield(0x0a, 27, 6)
        | bitfield(65, 20, 7)
        | bitfield(encoded & 0x7f, 13, 7)
        | bitfield(encoded >> 7, 36, 1)
        | bitfield(qp, 0, 6)
    )

def mov_pr_rot_imm(value, qp=0):
    encoded = (value >> 16) & 0x0fffffff
    return (
        bitfield(1, 34, 2)
        | bitfield(encoded & 0x7f, 6, 7)
        | bitfield((encoded >> 7) & 0x7f, 13, 7)
        | bitfield((encoded >> 14) & 0x0f, 20, 4)
        | bitfield((encoded >> 18) & 0xff, 24, 8)
        | bitfield((encoded >> 26) & 1, 32, 1)
        | bitfield((encoded >> 27) & 1, 36, 1)
        | bitfield(qp, 0, 6)
    )

def mov_m_imm_ar(ar_num, imm, qp=0):
    encoded = imm & 0xff
    if ar_num in (64, 65, 66):
        return IUnitInsn(
            bitfield(0x0a, 27, 6)
            | bitfield(ar_num, 20, 7)
            | bitfield(encoded & 0x7f, 13, 7)
            | bitfield(encoded >> 7, 36, 1)
            | bitfield(qp, 0, 6)
        )
    return (
        bitfield(0x28, 27, 6)
        | bitfield(ar_num, 20, 7)
        | bitfield(encoded & 0x7f, 13, 7)
        | bitfield(encoded >> 7, 36, 1)
        | bitfield(qp, 0, 6)
    )

def mov_i_imm_ar(ar_num, imm, qp=0):
    encoded = imm & 0xff
    return (
        bitfield(0x0a, 27, 6)
        | bitfield(ar_num, 20, 7)
        | bitfield(encoded & 0x7f, 13, 7)
        | bitfield(encoded >> 7, 36, 1)
        | bitfield(qp, 0, 6)
    )

def mov_m_gr_ar(r2, ar_num, qp=0):
    if ar_num in (64, 65, 66):
        return IUnitInsn(
            bitfield(0x2a, 27, 6)
            | bitfield(ar_num, 20, 7)
            | bitfield(r2, 13, 7)
            | bitfield(qp, 0, 6)
        )
    return (op(1) | bitfield(0x2a, 27, 6) |
            bitfield(ar_num, 20, 7) | bitfield(r2, 13, 7) |
            bitfield(qp, 0, 6))

def mov_m_ar_gr(r1, ar_num, qp=0):
    if ar_num in (64, 65, 66):
        return IUnitInsn(
            bitfield(0x32, 27, 6)
            | bitfield(ar_num, 20, 7)
            | bitfield(r1, 6, 7)
            | bitfield(qp, 0, 6)
        )
    return (op(1) | bitfield(0x22, 27, 6) |
            bitfield(ar_num, 20, 7) | bitfield(r1, 6, 7) |
            bitfield(qp, 0, 6))

def mov_m_psr_gr(r1, qp=0):
    return (op(1) | bitfield(0x25, 27, 6) |
            bitfield(r1, 6, 7) | bitfield(qp, 0, 6))

def mov_m_gr_psrl(r2, qp=0):
    return (op(1) | bitfield(0x2d, 27, 6) |
            bitfield(r2, 13, 7) | bitfield(qp, 0, 6))

def mov_m_psr_um_gr(r1, qp=0):
    return (op(1) | bitfield(0x21, 27, 6) |
            bitfield(r1, 6, 7) | bitfield(qp, 0, 6))

def mov_m_gr_psr_um(r2, qp=0):
    return (op(1) | bitfield(0x29, 27, 6) |
            bitfield(r2, 13, 7) | bitfield(qp, 0, 6))

def mov_gr_psr_full(r1, qp=0):
    return (
        op(1)
        | bitfield(0x2d, 27, 6)
        | bitfield(r1, 13, 7)
        | bitfield(qp, 0, 6)
    )

def mov_m_cr_gr(r1, cr_num, qp=0):
    return (op(1) | bitfield(0x24, 27, 6) |
            bitfield(cr_num, 20, 7) | bitfield(r1, 6, 7) |
            bitfield(qp, 0, 6))

def mov_m_gr_cr(r2, cr_num, qp=0):
    return (op(1) | bitfield(0x2c, 27, 6) |
            bitfield(cr_num, 20, 7) | bitfield(r2, 13, 7) |
            bitfield(qp, 0, 6))

def mov_dbr_indexed_read(r1, index_reg, bit36=0, qp=0):
    return (op(1) | bitfield(bit36, 36, 1) |
            bitfield(0x11, 27, 6) | bitfield(index_reg, 20, 7) |
            bitfield(r1, 6, 7) | bitfield(qp, 0, 6))

def mov_dbr_indexed_write(index_reg, value_reg, qp=0):
    return (op(1) | bitfield(0x01, 27, 6) |
            bitfield(index_reg, 20, 7) | bitfield(value_reg, 13, 7) |
            bitfield(qp, 0, 6))

def mov_ibr_indexed_read(r1, index_reg, bit36=0, qp=0):
    return (op(1) | bitfield(bit36, 36, 1) |
            bitfield(0x12, 27, 6) | bitfield(index_reg, 20, 7) |
            bitfield(r1, 6, 7) | bitfield(qp, 0, 6))

def mov_ibr_indexed_write(index_reg, value_reg, qp=0):
    return (op(1) | bitfield(0x02, 27, 6) |
            bitfield(index_reg, 20, 7) | bitfield(value_reg, 13, 7) |
            bitfield(qp, 0, 6))

def mov_pkr_indexed(index_reg, value_reg, qp=0, bit36=0):
    return (
        op(1)
        | bitfield(bit36, 36, 1)
        | bitfield(0x03, 27, 6)
        | bitfield(index_reg, 20, 7)
        | bitfield(value_reg, 13, 7)
        | bitfield(qp, 0, 6)
    )

def mov_pkr_indexed_read(r1, index_reg, bit36=0, qp=0):
    return (
        op(1)
        | bitfield(bit36, 36, 1)
        | bitfield(0x13, 27, 6)
        | bitfield(index_reg, 20, 7)
        | bitfield(r1, 6, 7)
        | bitfield(qp, 0, 6)
    )

def mov_rr_write(value_reg, addr_reg, qp=0, ignored36=0):
    return (op(1) | bitfield(ignored36, 36, 1) | bitfield(addr_reg, 20, 7)
            | bitfield(value_reg, 13, 7) | bitfield(qp, 0, 6))

def mov_rr_read(dest_reg, addr_reg, qp=0, ignored36=0):
    return (op(1) | bitfield(ignored36, 36, 1) | bitfield(0x10, 27, 6)
            | bitfield(addr_reg, 20, 7) | bitfield(dest_reg, 6, 7)
            | bitfield(qp, 0, 6))

def mov_cpuid(r1, index_reg, qp=0, bit36=0):
    return (op(1) | bitfield(bit36, 36, 1) | bitfield(0x17, 27, 6)
            | bitfield(index_reg, 20, 7) | bitfield(r1, 6, 7)
            | bitfield(qp, 0, 6))

def mov_dahr_read(r1, index_reg, qp=0, bit36=0, ignored=0):
    return (op(1) | bitfield(bit36, 36, 1) | bitfield(0x20, 27, 6)
            | bitfield(index_reg, 20, 7) | bitfield(ignored, 13, 7)
            | bitfield(r1, 6, 7) | bitfield(qp, 0, 6))

def mov_msr_read(r1, index_reg, qp=0, bit36=0):
    return (op(1) | bitfield(bit36, 36, 1) | bitfield(0x16, 27, 6)
            | bitfield(index_reg, 20, 7) | bitfield(r1, 6, 7)
            | bitfield(qp, 0, 6))

def mov_msr_write(index_reg, value_reg, qp=0, bit36=0):
    return (op(1) | bitfield(bit36, 36, 1) | bitfield(0x06, 27, 6)
            | bitfield(index_reg, 20, 7) | bitfield(value_reg, 13, 7)
            | bitfield(qp, 0, 6))

def itr_i(index_reg, source_reg, qp=0):
    return (op(1) | bitfield(0x0f, 27, 6) |
            bitfield(index_reg, 20, 7) | bitfield(source_reg, 13, 7) |
            bitfield(qp, 0, 6))

def itr_d(index_reg, source_reg, bit36=0, qp=0):
    return (op(1) | bitfield(bit36, 36, 1) |
            bitfield(0x0e, 27, 6) | bitfield(index_reg, 20, 7) |
            bitfield(source_reg, 13, 7) | bitfield(qp, 0, 6))

def itc_d(source_reg, qp=0):
    return EndGroupInsn(
        op(1) | bitfield(0x2e, 27, 6) | bitfield(source_reg, 13, 7)
        | bitfield(qp, 0, 6))

def itc_i(source_reg, qp=0):
    return EndGroupInsn(
        op(1) | bitfield(0x2f, 27, 6) | bitfield(source_reg, 13, 7)
        | bitfield(qp, 0, 6))

def ptc_l(addr_reg, size_reg, qp=0):
    return (op(1) | bitfield(0x09, 27, 6) |
            bitfield(addr_reg, 20, 7) | bitfield(size_reg, 13, 7) |
            bitfield(qp, 0, 6))

def ptc_e(addr_reg, qp=0):
    return (op(1) | bitfield(0x34, 27, 6) |
            bitfield(addr_reg, 20, 7) | bitfield(qp, 0, 6))

def ptr_op(x6, addr_reg, size_reg, qp=0, alt=False):
    prefix = op(1) if alt else 0
    return (prefix | bitfield(x6, 27, 6) |
            bitfield(addr_reg, 20, 7) | bitfield(size_reg, 13, 7) |
            bitfield(qp, 0, 6))

def ptr_d(addr_reg, size_reg, qp=0):
    return ptr_op(0x0c, addr_reg, size_reg, qp, alt=True)

def ptr_i(addr_reg, size_reg, qp=0):
    return ptr_op(0x0d, addr_reg, size_reg, qp, alt=True)

def ptr_d_alt(addr_reg, size_reg, qp=0):
    return ptr_op(0x0c, addr_reg, size_reg, qp, alt=True)

def ptr_i_alt(addr_reg, size_reg, qp=0):
    return ptr_op(0x0d, addr_reg, size_reg, qp, alt=True)

def tpa(dest_reg, source_reg, qp=0, bit36=0):
    return (op(1) | bitfield(bit36, 36, 1) | bitfield(0x1e, 27, 6)
            | bitfield(source_reg, 20, 7) | bitfield(dest_reg, 6, 7)
            | bitfield(qp, 0, 6))

def tak(dest_reg, source_reg, qp=0, bit36=0, ignored=0):
    return (op(1) | bitfield(bit36, 36, 1) | bitfield(0x1f, 27, 6)
            | bitfield(source_reg, 20, 7) | bitfield(ignored, 13, 7)
            | bitfield(dest_reg, 6, 7) | bitfield(qp, 0, 6))

def thash(dest_reg, source_reg, qp=0, bit36=0, ignored=0):
    return (op(1) | bitfield(bit36, 36, 1) | bitfield(0x1a, 27, 6)
            | bitfield(source_reg, 20, 7) | bitfield(ignored, 13, 7)
            | bitfield(dest_reg, 6, 7) | bitfield(qp, 0, 6))

def ttag(dest_reg, source_reg, qp=0, bit36=0, ignored=0):
    return (op(1) | bitfield(bit36, 36, 1) | bitfield(0x1b, 27, 6)
            | bitfield(source_reg, 20, 7) | bitfield(ignored, 13, 7)
            | bitfield(dest_reg, 6, 7) | bitfield(qp, 0, 6))

def mov_b_gr(b1, r2, x6=0, qp=0):
    return (bitfield(7, 33, 3) | bitfield(x6, 27, 6) |
            bitfield(r2, 13, 7) | bitfield(b1, 6, 3) |
            bitfield(qp, 0, 6))

def mov_gr_b(r1, b2, qp=0):
    return (bitfield(0x31, 27, 6) | bitfield(b2, 13, 3) |
            bitfield(r1, 6, 7) | bitfield(qp, 0, 6))

def psr_mask_op(x6, mask, qp=0):
    return (
        bitfield(x6, 27, 6)
        | bitfield(mask & 0x7f, 6, 7)
        | bitfield((mask >> 7) & 0x7f, 13, 7)
        | bitfield((mask >> 14) & 0x7f, 20, 7)
        | bitfield((mask >> 21) & 0x3, 31, 2)
        | bitfield((mask >> 23) & 1, 36, 1)
        | bitfield(qp, 0, 6)
    )

def ssm(mask, qp=0):
    return psr_mask_op(0x06, mask, qp)

def rsm(mask, qp=0):
    return psr_mask_op(0x07, mask, qp)

def sum_um(mask, qp=0):
    return psr_mask_op(0x04, mask, qp)

def rum(mask, qp=0):
    return psr_mask_op(0x05, mask, qp)

def bsw0(qp=0):
    return EndGroupInsn(
        bitfield(0x0c, 27, 6) | bitfield(qp, 0, 6))

def bsw1(qp=0):
    return EndGroupInsn(
        bitfield(0x0d, 27, 6) | bitfield(qp, 0, 6))

def vmsw0(qp=0):
    return bitfield(0x18, 27, 6) | bitfield(qp, 0, 6)

def vmsw1(qp=0):
    return bitfield(0x19, 27, 6) | bitfield(qp, 0, 6)

def flushrs_enc(qp=0):
    return StartGroupInsn(
        bitfield(0x0c, 27, 6)
        | bitfield(qp, 0, 6)
    )

def loadrs_enc(qp=0):
    return StartGroupInsn(
        bitfield(0x0a, 27, 6)
        | bitfield(qp, 0, 6)
    )

def fsetc(sf, amask, omask, qp=0):
    return (
        op(0)
        | bitfield(0x04, 27, 6)
        | bitfield(sf, 34, 2)
        | bitfield(omask, 20, 7)
        | bitfield(amask, 13, 7)
        | bitfield(qp, 0, 6)
    )

def fclrf(sf, qp=0):
    return (
        op(0)
        | bitfield(0x05, 27, 6)
        | bitfield(sf, 34, 2)
        | bitfield(qp, 0, 6)
    )

def fchkf(sf, source, target, qp=0, ignored26=0):
    field = branch_target_field(source, target)
    return (
        op(0)
        | bitfield(0x08, 27, 6)
        | bitfield(sf, 34, 2)
        | bitfield(field, 6, 20)
        | bitfield(ignored26, 26, 1)
        | bitfield(field >> 20, 36, 1)
        | bitfield(qp, 0, 6)
    )

def hint_m(qp=0):
    return bitfield(1, 27, 6) | bitfield(64, 20, 7) | bitfield(qp, 0, 6)

def hint_i(qp=0):
    return bitfield(1, 27, 6) | bitfield(64, 20, 7) | bitfield(qp, 0, 6)

def hint_b(imm=0):
    """hint.b (B9): op 2, x6 1."""
    return (op(2) | bitfield(1, 27, 6) | bitfield(imm & 0xfffff, 6, 20)
            | bitfield((imm >> 20) & 1, 36, 1))

def mov_grpmc_indexed(r3, r2, qp=0, bit36=0):
    return (
        op(1)
        | bitfield(bit36, 36, 1)
        | bitfield(0x04, 27, 6)
        | bitfield(r3, 20, 7)
        | bitfield(r2, 13, 7)
        | bitfield(qp, 0, 6)
    )

def mov_pmcgr_indexed(r1, r3, qp=0, bit36=0):
    return (
        op(1)
        | bitfield(bit36, 36, 1)
        | bitfield(0x14, 27, 6)
        | bitfield(r3, 20, 7)
        | bitfield(r1, 6, 7)
        | bitfield(qp, 0, 6)
    )

def mov_grpmd_indexed(r3, r2, qp=0, bit36=0):
    return (
        op(1)
        | bitfield(bit36, 36, 1)
        | bitfield(0x05, 27, 6)
        | bitfield(r3, 20, 7)
        | bitfield(r2, 13, 7)
        | bitfield(qp, 0, 6)
    )

def mov_pmdgr_indexed(r1, r3, qp=0, bit36=0):
    return (
        op(1)
        | bitfield(bit36, 36, 1)
        | bitfield(0x15, 27, 6)
        | bitfield(r3, 20, 7)
        | bitfield(r1, 6, 7)
        | bitfield(qp, 0, 6)
    )

def mov_ip(r1, qp=0):
    return (
        bitfield(0x30, 27, 6)
        | bitfield(r1, 6, 7)
        | bitfield(qp, 0, 6)
    )

def mov_gr_pr(r2, mask, qp=0):
    encoded = ((mask & 0x1fffe) >> 1)
    return (
        bitfield(3, 33, 3)
        | bitfield(encoded & 0x7f, 6, 7)
        | bitfield(r2, 13, 7)
        | bitfield((encoded >> 7) & 0xff, 24, 8)
        | bitfield((encoded >> 15) & 1, 36, 1)
        | bitfield(qp, 0, 6)
    )

def mov_pr_gr(r1, qp=0):
    return (
        bitfield(0x33, 27, 6)
        | bitfield(r1, 6, 7)
        | bitfield(qp, 0, 6)
    )

def brp_loop_imp():
    return op(7) | bitfield(0x100, 27, 9) | bitfield(1, 6, 1) | 8

def brp_sptk():
    return 0xf1fcf84840

def cover_b(qp=0):
    return EndGroupInsn(0x10000000 | bitfield(qp, 0, 6))

def cover_b_ignored_fields(qp=0):
    return EndGroupInsn(
        cover_b(qp)
        | bitfield(1, 12, 1)
        | bitfield(6, 13, 7)
        | bitfield(79, 20, 7)
        | bitfield(1, 6, 3)
    )

def srlz_d(qp=0, ignored36=0):
    return (bitfield(ignored36, 36, 1) | bitfield(0x30, 27, 6) |
            bitfield(qp, 0, 6))

def srlz_i(qp=0, ignored36=0):
    return (bitfield(ignored36, 36, 1) | bitfield(0x31, 27, 6) |
            bitfield(qp, 0, 6))

def sync_i(qp=0, ignored36=0, ignored=0):
    return (bitfield(ignored36, 36, 1) | bitfield(0x33, 27, 6) |
            bitfield(ignored, 6, 21) | bitfield(qp, 0, 6))

def fwb(qp=0, ignored36=0, ignored=0):
    return (bitfield(ignored36, 36, 1) | bitfield(0x20, 27, 6) |
            bitfield(ignored, 6, 21) | bitfield(qp, 0, 6))

def mf(advanced=False, qp=0, ignored36=0, ignored=0):
    x6 = 0x23 if advanced else 0x22
    return (bitfield(ignored36, 36, 1) | bitfield(x6, 27, 6) |
            bitfield(ignored, 6, 21) | bitfield(qp, 0, 6))

__all__ = (
    'break_m',
    'illegal_m',
    'break_x_mlx',
    'hint_x_mlx',
    'break_b',
    'break_f',
    'clrrrb_b',
    'clrrrb_pr_b',
    'pal_break',
    'pal_call_program',
    'pal_stacked_call_program',
    'mov_br_gr',
    'rfi_b',
    'rfi_to_gr',
    'epc_b',
    'mov_ar',
    'alloc',
    'alloc_m',
    'mov_ar_lc',
    'mov_i_ar_gr',
    'mov_lc_gr',
    'mov_lc_imm',
    'mov_pr_rot_imm',
    'mov_m_imm_ar',
    'mov_i_imm_ar',
    'mov_m_gr_ar',
    'mov_m_ar_gr',
    'mov_m_psr_gr',
    'mov_m_gr_psrl',
    'mov_m_psr_um_gr',
    'mov_m_gr_psr_um',
    'mov_gr_psr_full',
    'mov_m_cr_gr',
    'mov_m_gr_cr',
    'mov_dbr_indexed_read',
    'mov_dbr_indexed_write',
    'mov_ibr_indexed_read',
    'mov_ibr_indexed_write',
    'mov_pkr_indexed',
    'mov_pkr_indexed_read',
    'mov_rr_write',
    'mov_rr_read',
    'mov_cpuid',
    'mov_dahr_read',
    'mov_msr_read',
    'mov_msr_write',
    'itr_i',
    'itr_d',
    'itc_d',
    'itc_i',
    'ptc_l',
    'ptc_e',
    'ptr_op',
    'ptr_d',
    'ptr_i',
    'ptr_d_alt',
    'ptr_i_alt',
    'tpa',
    'tak',
    'thash',
    'ttag',
    'mov_b_gr',
    'mov_gr_b',
    'psr_mask_op',
    'ssm',
    'rsm',
    'sum_um',
    'rum',
    'bsw0',
    'bsw1',
    'vmsw0',
    'vmsw1',
    'flushrs_enc',
    'loadrs_enc',
    'fsetc',
    'fclrf',
    'fchkf',
    'hint_m',
    'hint_i',
    'hint_b',
    'mov_grpmc_indexed',
    'mov_pmcgr_indexed',
    'mov_grpmd_indexed',
    'mov_pmdgr_indexed',
    'mov_ip',
    'mov_gr_pr',
    'mov_pr_gr',
    'brp_loop_imp',
    'brp_sptk',
    'cover_b',
    'cover_b_ignored_fields',
    'srlz_d',
    'srlz_i',
    'sync_i',
    'fwb',
    'mf',
)
