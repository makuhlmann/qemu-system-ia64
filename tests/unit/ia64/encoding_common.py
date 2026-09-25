# IA-64 instruction encoders split on translator family boundaries.

def bitfield(value, low, width):
    return (value & ((1 << width) - 1)) << low

def op(major):
    return bitfield(major, 37, 4)

def nop_m():
    return bitfield(1, 27, 4)

def nop_x_mlx(imm62, qp=0):
    imm62 &= (1 << 62) - 1
    imm21 = imm62 & 0x1fffff
    l_slot = imm62 >> 21
    x_slot = (
        bitfield(1, 27, 6)
        | bitfield(imm21 & 0xfffff, 6, 20)
        | bitfield((imm21 >> 20) & 1, 36, 1)
        | bitfield(qp, 0, 6)
    )
    return (0x04, nop_m(), l_slot, x_slot)

def nop_i():
    return bitfield(1, 27, 6)

def nop_b():
    return op(2)

def nop_x(qp=0):
    return bitfield(1, 27, 6) | bitfield(qp, 0, 6)

def nop_f(imm=0, qp=0):
    return (
        bitfield(1, 27, 6)
        | bitfield(imm & 0xfffff, 6, 20)
        | bitfield((imm >> 20) & 1, 36, 1)
        | bitfield(qp, 0, 6)
    )

class MUnitAlloc(int):
    pass

class IUnitInsn(int):
    pass

class StartGroupInsn(int):
    pass

class EndGroupInsn(int):
    pass

def bundle_words(template, slot0, slot1, slot2):
    if isinstance(slot1, MUnitAlloc):
        slot0, slot1 = slot1, slot0
    if isinstance(slot2, StartGroupInsn) and \
            slot0 == nop_m() and slot1 == nop_i():
        slot0, slot2 = slot2, nop_i()
    if isinstance(slot0, IUnitInsn):
        if slot2 != nop_i():
            raise ValueError(
                "cannot relocate I-unit instruction into full bundle")
        slot0, slot1, slot2 = nop_m(), slot0, slot1
    if isinstance(slot0, EndGroupInsn):
        stop_after_slot0 = {
            0x08: 0x0a, 0x09: 0x0b,
        }
        if template in (0x00, 0x01):
            if slot1 != nop_i():
                raise ValueError(
                    "cannot change a non-NOP MII slot 1 into an MMI slot")
            template = {0x00: 0x0a, 0x01: 0x0b}[template]
        elif template in stop_after_slot0:
            template = stop_after_slot0[template]
        else:
            raise ValueError("template cannot stop after slot 0")
    elif isinstance(slot1, EndGroupInsn):
        stop_after_slot1 = {0x00: 0x02, 0x01: 0x03}
        if template not in stop_after_slot1:
            raise ValueError("template cannot stop after slot 1")
        template = stop_after_slot1[template]
    elif isinstance(slot2, EndGroupInsn):
        template |= 1
    raw = template | (slot0 << 5) | (slot1 << 46) | (slot2 << 87)
    return raw & ((1 << 64) - 1), raw >> 64

def raw_bundle(address, low, high):
    raw = low | (high << 64)
    return (
        address,
        raw & 0x1f,
        (raw >> 5) & ((1 << 41) - 1),
        (raw >> 46) & ((1 << 41) - 1),
        (raw >> 87) & ((1 << 41) - 1),
    )

def ia32_bundle(address, code):
    """Place up to 16 IA-32 instruction bytes in an IA-64 test bundle."""
    code = bytes(code)
    if address & 0xf:
        raise ValueError("IA-32 test bundle address must be 16-byte aligned")
    if len(code) > 16:
        raise ValueError("IA-32 test bundle cannot exceed 16 bytes")
    code = code.ljust(16, b'\x90')
    return raw_bundle(address,
                      int.from_bytes(code[:8], "little"),
                      int.from_bytes(code[8:], "little"))

def normalized_bundles(bundles):
    bundles = list(bundles)
    for index, bundle in enumerate(bundles):
        address, template, slot0, slot1, slot2 = bundle
        starts_group = (
            isinstance(slot0, (MUnitAlloc, StartGroupInsn))
            or isinstance(slot1, MUnitAlloc)
            or isinstance(slot2, StartGroupInsn)
        )

        if starts_group and index > 0 and \
                bundles[index - 1][0] + 0x10 == address:
            previous = bundles[index - 1]
            bundles[index - 1] = (
                previous[0], previous[1] | 1,
                previous[2], previous[3], previous[4],
            )
    return bundles

__all__ = (
    'bitfield',
    'op',
    'nop_m',
    'nop_x_mlx',
    'nop_i',
    'nop_b',
    'nop_x',
    'nop_f',
    'MUnitAlloc',
    'IUnitInsn',
    'StartGroupInsn',
    'EndGroupInsn',
    'bundle_words',
    'raw_bundle',
    'ia32_bundle',
    'normalized_bundles',
)
