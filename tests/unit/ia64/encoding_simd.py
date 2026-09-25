# IA-64 instruction encoders split on translator family boundaries.

from .encoding_common import bitfield, op

def czx(opcode, r1, r3, qp=0):
    return (bitfield(opcode, 27, 6) | bitfield(r3, 20, 7) |
            bitfield(r1, 6, 7) | bitfield(qp, 0, 6))

def czx1_l(r1, r3, qp=0):
    return czx(0x18, r1, r3, qp)

def czx1_r(r1, r3, qp=0):
    return czx(0x1c, r1, r3, qp)

def czx2_l(r1, r3, qp=0):
    return czx(0x19, r1, r3, qp)

def czx2_r(r1, r3, qp=0):
    return czx(0x1d, r1, r3, qp)

def mix(x6, x3, z, r1, r2, r3, qp=0, ignored=0):
    return (
        op(7)
        | bitfield(x6, 27, 6)
        | bitfield(ignored, 27, 1)
        | bitfield(x3, 33, 3)
        | bitfield(z, 36, 1)
        | bitfield(r3, 20, 7)
        | bitfield(r2, 13, 7)
        | bitfield(r1, 6, 7)
        | bitfield(qp, 0, 6)
    )

def mix4_r(r1, r2, r3, qp=0):
    return mix(0x10, 4, 1, r1, r2, r3, qp)

def mix4_l(r1, r2, r3, qp=0):
    return mix(0x14, 4, 1, r1, r2, r3, qp)

def mix2_r(r1, r2, r3, qp=0):
    return mix(0x10, 5, 0, r1, r2, r3, qp)

def mix2_l(r1, r2, r3, qp=0):
    return mix(0x14, 5, 0, r1, r2, r3, qp)

def mix1_r(r1, r2, r3, qp=0, ignored=0):
    return mix(0x10, 4, 0, r1, r2, r3, qp, ignored)

def mix1_l(r1, r2, r3, qp=0, ignored=0):
    return mix(0x14, 4, 0, r1, r2, r3, qp, ignored)

def unpack(size, low, r1, r2, r3, qp=0):
    fields = {
        1: (0, 0),
        2: (0, 1),
        4: (1, 0),
    }
    za, zb = fields[size]
    return (
        op(7)
        | bitfield(za, 36, 1)
        | bitfield(2, 34, 2)
        | bitfield(zb, 33, 1)
        | bitfield(1, 30, 2)
        | bitfield(2 if low else 0, 28, 2)
        | bitfield(r3, 20, 7)
        | bitfield(r2, 13, 7)
        | bitfield(r1, 6, 7)
        | bitfield(qp, 0, 6)
    )

def unpack2_l(r1, r2, r3, qp=0):
    return unpack(2, True, r1, r2, r3, qp)

def unpack1_h(r1, r2, r3, qp=0):
    return unpack(1, False, r1, r2, r3, qp)

def unpack1_l(r1, r2, r3, qp=0):
    return unpack(1, True, r1, r2, r3, qp)

def unpack2_h(r1, r2, r3, qp=0):
    return unpack(2, False, r1, r2, r3, qp)

def unpack4_h(r1, r2, r3, qp=0):
    return unpack(4, False, r1, r2, r3, qp)

def unpack4_l(r1, r2, r3, qp=0):
    return unpack(4, True, r1, r2, r3, qp)

def pcmp(r1, r2, r3, size, gt=False, qp=0):
    fields = {
        1: (0, 0),
        2: (0, 1),
        4: (1, 0),
    }
    za, zb = fields[size]
    return (
        op(8)
        | bitfield(za, 36, 1)
        | bitfield(1, 34, 2)
        | bitfield(zb, 33, 1)
        | bitfield(1, 32, 1)
        | bitfield(9, 29, 4)
        | bitfield(1 if gt else 0, 27, 2)
        | bitfield(r3, 20, 7)
        | bitfield(r2, 13, 7)
        | bitfield(r1, 6, 7)
        | bitfield(qp, 0, 6)
    )

def pcmp1_eq(r1, r2, r3, qp=0):
    return pcmp(r1, r2, r3, 1, False, qp)

def pavg(r1, r2, r3, size, raz=False, qp=0):
    fields = {
        1: (0, 0),
        2: (0, 1),
    }
    za, zb = fields[size]
    return (
        op(8)
        | bitfield(za, 36, 1)
        | bitfield(1, 34, 2)
        | bitfield(zb, 33, 1)
        | bitfield(2, 29, 4)
        | bitfield(3 if raz else 2, 27, 2)
        | bitfield(r3, 20, 7)
        | bitfield(r2, 13, 7)
        | bitfield(r1, 6, 7)
        | bitfield(qp, 0, 6)
    )

def pavgsub(r1, r2, r3, size, qp=0):
    fields = {
        1: (0, 0),
        2: (0, 1),
    }
    za, zb = fields[size]
    return (
        op(8)
        | bitfield(za, 36, 1)
        | bitfield(1, 34, 2)
        | bitfield(zb, 33, 1)
        | bitfield(3, 29, 4)
        | bitfield(2, 27, 2)
        | bitfield(r3, 20, 7)
        | bitfield(r2, 13, 7)
        | bitfield(r1, 6, 7)
        | bitfield(qp, 0, 6)
    )

def i2_multimedia(za, zb, x2b, x2c, r1, r2, r3, qp=0):
    return (
        op(7)
        | bitfield(za, 36, 1)
        | bitfield(2, 34, 2)
        | bitfield(zb, 33, 1)
        | bitfield(x2c, 30, 2)
        | bitfield(x2b, 28, 2)
        | bitfield(r3, 20, 7)
        | bitfield(r2, 13, 7)
        | bitfield(r1, 6, 7)
        | bitfield(qp, 0, 6)
    )

def pmin1_u(r1, r2, r3, qp=0):
    return i2_multimedia(0, 0, 1, 0, r1, r2, r3, qp)

def pmax1_u(r1, r2, r3, qp=0):
    return i2_multimedia(0, 0, 1, 1, r1, r2, r3, qp)

def pmin2(r1, r2, r3, qp=0):
    return i2_multimedia(0, 1, 3, 0, r1, r2, r3, qp)

def pmax2(r1, r2, r3, qp=0):
    return i2_multimedia(0, 1, 3, 1, r1, r2, r3, qp)

def pack2_uss(r1, r2, r3, qp=0):
    return i2_multimedia(0, 1, 0, 0, r1, r2, r3, qp)

def pack2_sss(r1, r2, r3, qp=0):
    return i2_multimedia(0, 1, 2, 0, r1, r2, r3, qp)

def pack4_sss(r1, r2, r3, qp=0):
    return i2_multimedia(1, 0, 2, 0, r1, r2, r3, qp)

def psad1(r1, r2, r3, qp=0):
    return i2_multimedia(0, 0, 3, 2, r1, r2, r3, qp)

def pmpy2(r1, r2, r3, right=False, ignored=0, qp=0):
    return (
        op(7)
        | bitfield((0x1a if right else 0x1e) | (ignored & 1), 27, 6)
        | bitfield(5, 33, 3)
        | bitfield(r3, 20, 7)
        | bitfield(r2, 13, 7)
        | bitfield(r1, 6, 7)
        | bitfield(qp, 0, 6)
    )

def pmpyshr2(r1, r2, r3, shift, signed=False, ignored=0, qp=0):
    shift_codes = {
        0: 0x02,
        7: 0x0a,
        15: 0x12,
        16: 0x1a,
    }
    return (
        op(7)
        | bitfield(shift_codes[shift] + (4 if signed else 0) |
                   (ignored & 1), 27, 6)
        | bitfield(1, 33, 3)
        | bitfield(r3, 20, 7)
        | bitfield(r2, 13, 7)
        | bitfield(r1, 6, 7)
        | bitfield(qp, 0, 6)
    )

def pshl(size, r1, value_reg, count_reg, ignored=0, qp=0):
    if size == 2:
        za = 0
        zb = 1
    elif size == 4:
        za = 1
        zb = 0
    else:
        raise ValueError("pshl supports 2- and 4-byte lanes")
    return (
        op(7)
        | bitfield(za, 36, 1)
        | bitfield(zb, 33, 1)
        | bitfield(0x08 | (ignored & 1), 27, 6)
        | bitfield(count_reg, 20, 7)
        | bitfield(value_reg, 13, 7)
        | bitfield(r1, 6, 7)
        | bitfield(qp, 0, 6)
    )

def pshl2(r1, value_reg, count_reg, ignored=0, qp=0):
    return pshl(2, r1, value_reg, count_reg, ignored, qp)

def pshl4(r1, value_reg, count_reg, ignored=0, qp=0):
    return pshl(4, r1, value_reg, count_reg, ignored, qp)

def pshl_fixed(size, r1, value_reg, count, ignored=0, qp=0):
    za, zb = {2: (0, 1), 4: (1, 0)}[size]
    return (
        op(7)
        | bitfield(za, 36, 1)
        | bitfield(3, 34, 2)
        | bitfield(zb, 33, 1)
        | bitfield(1, 30, 2)
        | bitfield(1, 28, 2)
        | bitfield(ignored, 25, 3)
        | bitfield(31 - count, 20, 5)
        | bitfield(value_reg, 13, 7)
        | bitfield(r1, 6, 7)
        | bitfield(qp, 0, 6)
    )

def pshl2_fixed(r1, value_reg, count, ignored=0, qp=0):
    return pshl_fixed(2, r1, value_reg, count, ignored, qp)

def pshl4_fixed(r1, value_reg, count, ignored=0, qp=0):
    return pshl_fixed(4, r1, value_reg, count, ignored, qp)

def pshr(size, r1, r3, count_or_r2, unsigned=False, variable=False,
         ignored=0, qp=0):
    za, zb = {2: (0, 1), 4: (1, 0)}[size]
    x2a = 0 if variable else 1
    if variable:
        x2b = 0 if unsigned else 2
    else:
        x2b = 1 if unsigned else 3
    raw = (
        op(7)
        | bitfield(za, 36, 1)
        | bitfield(x2a, 34, 2)
        | bitfield(zb, 33, 1)
        | bitfield(x2b, 28, 2)
        | bitfield(r3, 20, 7)
        | bitfield(r1, 6, 7)
        | bitfield(qp, 0, 6)
    )
    if variable:
        return raw | bitfield(count_or_r2, 13, 7)
    return (
        raw
        | bitfield(count_or_r2, 14, 5)
        | bitfield(ignored & 1, 13, 1)
        | bitfield((ignored >> 1) & 1, 19, 1)
        | bitfield((ignored >> 2) & 1, 27, 1)
    )

def pshr2(r1, r3, count_or_r2, unsigned=False, variable=False,
          ignored=0, qp=0):
    return pshr(2, r1, r3, count_or_r2, unsigned, variable, ignored, qp)

def pshr4(r1, r3, count_or_r2, unsigned=False, variable=False,
          ignored=0, qp=0):
    return pshr(4, r1, r3, count_or_r2, unsigned, variable, ignored, qp)

def mux1_rev(r1, r2, qp=0):
    return mux1(r1, r2, 0x0b, qp)

def mux1(r1, r2, mbtype4, qp=0):
    return (
        op(7)
        | bitfield(3, 34, 2)
        | bitfield(2, 30, 2)
        | bitfield(2, 28, 2)
        | bitfield(mbtype4 & 0xf, 20, 4)
        | bitfield(r2, 13, 7)
        | bitfield(r1, 6, 7)
        | bitfield(qp, 0, 6)
    )

def mux2(r1, r2, mhtype8, qp=0):
    return (
        op(7)
        | bitfield(3, 34, 2)
        | bitfield(1, 33, 1)
        | bitfield(2, 30, 2)
        | bitfield(2, 28, 2)
        | bitfield(mhtype8 & 0xff, 20, 8)
        | bitfield(r2, 13, 7)
        | bitfield(r1, 6, 7)
        | bitfield(qp, 0, 6)
    )

def padd1(r1, r2, r3, qp=0):
    return (
        op(8)
        | bitfield(1, 34, 2)
        | bitfield(r3, 20, 7)
        | bitfield(r2, 13, 7)
        | bitfield(r1, 6, 7)
        | bitfield(qp, 0, 6)
    )

def psub1_uuu(r1, r2, r3, qp=0):
    return (
        op(8)
        | bitfield(1, 34, 2)
        | bitfield(1, 29, 4)
        | bitfield(2, 27, 2)
        | bitfield(r3, 20, 7)
        | bitfield(r2, 13, 7)
        | bitfield(r1, 6, 7)
        | bitfield(qp, 0, 6)
    )

def pshladd2(r1, r2, count, r3, qp=0):
    return (
        op(8)
        | bitfield(1, 33, 1)
        | bitfield(1, 34, 2)
        | bitfield(4, 29, 4)
        | bitfield(count - 1, 27, 2)
        | bitfield(r3, 20, 7)
        | bitfield(r2, 13, 7)
        | bitfield(r1, 6, 7)
        | bitfield(qp, 0, 6)
    )

def pshradd2(r1, r2, count, r3, qp=0):
    return (
        op(8)
        | bitfield(1, 33, 1)
        | bitfield(1, 34, 2)
        | bitfield(6, 29, 4)
        | bitfield(count - 1, 27, 2)
        | bitfield(r3, 20, 7)
        | bitfield(r2, 13, 7)
        | bitfield(r1, 6, 7)
        | bitfield(qp, 0, 6)
    )

__all__ = (
    'czx',
    'czx1_l',
    'czx1_r',
    'czx2_l',
    'czx2_r',
    'mix',
    'mix4_r',
    'mix4_l',
    'mix2_r',
    'mix2_l',
    'mix1_r',
    'mix1_l',
    'unpack',
    'unpack2_l',
    'unpack1_h',
    'unpack1_l',
    'unpack2_h',
    'unpack4_h',
    'unpack4_l',
    'pcmp',
    'pcmp1_eq',
    'pavg',
    'pavgsub',
    'i2_multimedia',
    'pmin1_u',
    'pmax1_u',
    'pmin2',
    'pmax2',
    'pack2_uss',
    'pack2_sss',
    'pack4_sss',
    'psad1',
    'pmpy2',
    'pmpyshr2',
    'pshl',
    'pshl2',
    'pshl4',
    'pshl_fixed',
    'pshl2_fixed',
    'pshl4_fixed',
    'pshr',
    'pshr2',
    'pshr4',
    'mux1_rev',
    'mux1',
    'mux2',
    'padd1',
    'psub1_uuu',
    'pshladd2',
    'pshradd2',
)
