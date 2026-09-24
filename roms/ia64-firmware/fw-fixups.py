#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Derive the firmware self-relocation fixup table from alternate links.

The firmware is linked three times, at the shipping base and at two
alternate bases (firmware.lds FW_LINK_BASE).  Any byte that differs
between the flat binaries must be part of an absolute-address value; this
tool classifies every such site as one of

  DIR64  - an 8-byte little-endian word whose value moves by the link delta
  DIR32  - a  4-byte little-endian word whose value moves by the link delta
  IMM64  - the movl immediate of an MLX bundle (slots 1+2) moving by delta

requiring *linearity across both deltas*, which rules out coincidental
values and validates the IMM64 bit scatter.  It then proves the table
complete and correct by applying it to the primary binary with each delta
and demanding byte-identity with the corresponding alternate link, and
finally injects the encoded table into the reserved .fw_fixups region of
the primary binary (whose content is offsets only, hence identical - all
zeroes - in all three links).

This gives the firmware's self-relocation a machine-checked fixup
table without trusting any relocation metadata (--emit-relocs does not
cover linker-synthesized GOT/OPD words; the diff sees everything).

usage: fw-fixups.py PRIMARY ALT1 DELTA1 ALT2 DELTA2 FIXUPS_OFFSET OUT
"""

import struct
import sys

FIXUP_MAGIC = 0x50555846  # "FXUP"
FIXUP_VERSION = 1


def load(path):
    with open(path, 'rb') as f:
        return bytearray(f.read())


def bundle_slots(chunk):
    """Split a 16-byte bundle into (template, slot0, slot1, slot2)."""
    lo, hi = struct.unpack('<QQ', chunk)
    template = lo & 0x1f
    slot0 = (lo >> 5) & ((1 << 41) - 1)
    slot1 = ((lo >> 46) | (hi << 18)) & ((1 << 41) - 1)
    slot2 = (hi >> 23) & ((1 << 41) - 1)
    return template, slot0, slot1, slot2


def imm64_decode(slot1, slot2):
    """Recover the movl imm64 from MLX slots (SDM vol 3, X2 format)."""
    imm7b = (slot2 >> 6) & 0x7f
    imm9d = (slot2 >> 27) & 0x1ff
    imm5c = (slot2 >> 22) & 0x1f
    ic = (slot2 >> 21) & 0x1
    i = (slot2 >> 36) & 0x1
    imm41 = slot1
    return (i << 63) | (imm41 << 22) | (ic << 21) | (imm5c << 16) | \
           (imm9d << 7) | imm7b


def imm64_encode(chunk, imm64):
    """Re-scatter imm64 into an MLX bundle, preserving all other bits."""
    lo, hi = struct.unpack('<QQ', chunk)
    template, slot0, slot1, slot2 = bundle_slots(chunk)
    imm7b = imm64 & 0x7f
    imm9d = (imm64 >> 7) & 0x1ff
    imm5c = (imm64 >> 16) & 0x1f
    ic = (imm64 >> 21) & 0x1
    imm41 = (imm64 >> 22) & ((1 << 41) - 1)
    i = (imm64 >> 63) & 0x1
    slot1 = imm41
    slot2 &= ~((1 << 36) | (0x1ff << 27) | (0x1f << 22) | (1 << 21) |
               (0x7f << 6))
    slot2 |= (i << 36) | (imm9d << 27) | (imm5c << 22) | (ic << 21) | \
             (imm7b << 6)
    lo = template | (slot0 << 5) | ((slot1 & ((1 << 18) - 1)) << 46)
    hi = (slot1 >> 18) | (slot2 << 23)
    return struct.pack('<QQ', lo & (1 << 64) - 1, hi & (1 << 64) - 1)


def classify(primary, alt1, d1, alt2, d2):
    n = len(primary)
    dir64, dir32, imm64 = [], [], []
    explained = bytearray(n)
    off = 0
    while off < n:
        if primary[off] == alt1[off] == alt2[off] or explained[off]:
            off += 1
            continue
        # 8-byte word moving linearly?
        w = off & ~7
        if w + 8 <= n:
            w0, = struct.unpack_from('<Q', primary, w)
            w1, = struct.unpack_from('<Q', alt1, w)
            w2, = struct.unpack_from('<Q', alt2, w)
            if (w1 - w0) & (1 << 64) - 1 == d1 and \
               (w2 - w0) & (1 << 64) - 1 == d2:
                dir64.append(w)
                explained[w:w + 8] = b'\1' * 8
                off = w + 8
                continue
        # 4-byte word moving linearly?
        w = off & ~3
        if w + 4 <= n:
            w0, = struct.unpack_from('<I', primary, w)
            w1, = struct.unpack_from('<I', alt1, w)
            w2, = struct.unpack_from('<I', alt2, w)
            if (w1 - w0) & 0xffffffff == d1 & 0xffffffff and \
               (w2 - w0) & 0xffffffff == d2 & 0xffffffff:
                dir32.append(w)
                explained[w:w + 4] = b'\1' * 4
                off = w + 4
                continue
        # movl immediate in an MLX bundle moving linearly?
        b = off & ~15
        if b + 16 <= n:
            t0 = bundle_slots(bytes(primary[b:b + 16]))
            t1 = bundle_slots(bytes(alt1[b:b + 16]))
            t2 = bundle_slots(bytes(alt2[b:b + 16]))
            if t0[0] in (4, 5) and t1[0] == t0[0] and t2[0] == t0[0] and \
               t0[1] == t1[1] == t2[1]:
                v0 = imm64_decode(t0[2], t0[3])
                v1 = imm64_decode(t1[2], t1[3])
                v2 = imm64_decode(t2[2], t2[3])
                if (v1 - v0) & (1 << 64) - 1 == d1 and \
                   (v2 - v0) & (1 << 64) - 1 == d2:
                    imm64.append(b)
                    explained[b:b + 16] = b'\1' * 16
                    off = b + 16
                    continue
        raise SystemExit(
            'fw-fixups: unexplained difference at offset 0x%x '
            '(%02x/%02x/%02x) - not a linear DIR64/DIR32/IMM64 site'
            % (off, primary[off], alt1[off], alt2[off]))
    return dir64, dir32, imm64


def apply_fixups(image, dir64, dir32, imm64, delta):
    out = bytearray(image)
    for off in dir64:
        w, = struct.unpack_from('<Q', out, off)
        struct.pack_into('<Q', out, off, (w + delta) & (1 << 64) - 1)
    for off in dir32:
        w, = struct.unpack_from('<I', out, off)
        struct.pack_into('<I', out, off, (w + delta) & 0xffffffff)
    for off in imm64:
        chunk = bytes(out[off:off + 16])
        _, _, s1, s2 = bundle_slots(chunk)
        v = imm64_decode(s1, s2)
        out[off:off + 16] = imm64_encode(chunk, (v + delta) & (1 << 64) - 1)
    return out


def main():
    if len(sys.argv) != 8:
        raise SystemExit(__doc__)
    primary = load(sys.argv[1])
    alt1 = load(sys.argv[2])
    d1 = int(sys.argv[3], 0)
    alt2 = load(sys.argv[4])
    d2 = int(sys.argv[5], 0)
    fixups_off = int(sys.argv[6], 0)
    out_path = sys.argv[7]

    if len(primary) != len(alt1) or len(primary) != len(alt2):
        raise SystemExit('fw-fixups: link sizes differ (%d/%d/%d)'
                         % (len(primary), len(alt1), len(alt2)))

    dir64, dir32, imm64 = classify(primary, alt1, d1, alt2, d2)

    # Proof: reconstructing each alternate link from the primary must be exact.
    if apply_fixups(primary, dir64, dir32, imm64, d1) != alt1:
        raise SystemExit('fw-fixups: reconstruction at delta1 mismatched')
    if apply_fixups(primary, dir64, dir32, imm64, d2) != alt2:
        raise SystemExit('fw-fixups: reconstruction at delta2 mismatched')

    blob = struct.pack('<IIIII', FIXUP_MAGIC, FIXUP_VERSION,
                       len(dir64), len(dir32), len(imm64))
    blob += b'\0' * 12  # pad header to 32 bytes
    for off in dir64 + dir32 + imm64:
        blob += struct.pack('<Q', off)

    reserved = 0x6000
    if len(blob) > reserved:
        raise SystemExit('fw-fixups: table (%d bytes) exceeds the reserved '
                         '.fw_fixups region (%d bytes) - grow it in '
                         'firmware.lds' % (len(blob), reserved))
    if fixups_off + reserved > len(primary):
        raise SystemExit('fw-fixups: .fw_fixups offset 0x%x outside binary'
                         % fixups_off)
    if any(primary[fixups_off:fixups_off + reserved]):
        raise SystemExit('fw-fixups: reserved .fw_fixups region is not '
                         'zero-filled - offset wrong?')

    final = bytearray(primary)
    final[fixups_off:fixups_off + len(blob)] = blob
    # Trailing footer so the loader (QEMU) can find the table without
    # knowing the layout: last 16 bytes = { "IA64FXUP", file offset }.
    # The footer lands at the start of the bss region in RAM, which entry.S
    # zeroes before use.
    final += struct.pack('<8sQ', b'IA64FXUP', fixups_off)
    with open(out_path, 'wb') as f:
        f.write(final)
    print('fw-fixups: %d DIR64, %d DIR32, %d IMM64 sites; table %d bytes'
          % (len(dir64), len(dir32), len(imm64), len(blob)))


if __name__ == '__main__':
    main()
