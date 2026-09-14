#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Pack the flat firmware body into a flash image.

The image is what a real Itanium board's firmware hub holds: the body at
the bottom of the flash window, a FIT (Firmware Interface Table, SAL spec
2.5 fig. 2-4) describing it, and the architected reset pointer block in the
last 48 bytes -- the PAL_A FIT entry at 4 GiB-48, the FIT pointer at
4 GiB-32 and the SALE_ENTRY pointer at 4 GiB-24, all with bit 63 set (the
uncacheable attribute the SDV firmware uses too).  The machine maps the
image so that it ends at 4 GiB and enters every processor at SALE_ENTRY
with the PALE_RESET exit state; nothing in the image is position-dependent
on the flash address, since fw_flash_entry finds its own base from ip.

The 128 KiB block at 4 GiB-448 KiB (0xFFF90000, the SDV's NVRAM sector)
is left erased and, for now, undeclared: the machine's synthetic variable
store overlays it until the firmware writes the flash itself.

usage: fw-flash.py BODY ELF OUT [SIZE]
"""

import struct
import subprocess
import sys

FLASH_END = 0x100000000
UC = 1 << 63
FIT_HEADER, FIT_PAL_B, FIT_PAL_A, FIT_BODY, FIT_UNUSED = 0x00, 0x01, 0x0f, 0x10, 0x7f
FIT_OFFSET_FROM_END = 0x10000       # the FIT sits in the last 64 KiB block
NVRAM_OFFSET_FROM_END = 0x70000     # 0xFFF90000 for a 4 MiB image
NVRAM_SIZE = 0x20000


def symbol(elf, name):
    out = subprocess.check_output(['ia64-linux-gnu-nm', elf], text=True)
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[2] == name:
            return int(parts[0], 16)
    raise SystemExit('fw-flash: symbol %s not in %s' % (name, elf))


def fit_entry(address, size, version, etype):
    """A 16-byte FIT entry with C_V set and a checksum that zeroes the sum."""
    paragraphs = (size + 15) // 16
    body = struct.pack('<QI', address, paragraphs & 0xffffff)
    body += struct.pack('<HB', version, etype | 0x80)
    checksum = (-sum(body)) & 0xff
    return body + bytes([checksum])


def main():
    if len(sys.argv) not in (4, 5):
        raise SystemExit(__doc__)
    body_path, elf, out_path = sys.argv[1:4]
    size = int(sys.argv[4], 0) if len(sys.argv) == 5 else 0x400000
    with open(body_path, 'rb') as f:
        body = bytearray(f.read())
    if len(body) % 16:
        body += b'\0' * (16 - len(body) % 16)
    base = FLASH_END - size
    link_base = symbol(elf, '__fw_image_start')
    entry = symbol(elf, 'fw_flash_entry') - link_base
    body_size_slot = symbol(elf, 'fw_flash_body_size') - link_base
    pal_stub = symbol(elf, 'pal_proc_entry') - link_base
    nvram_off = size - NVRAM_OFFSET_FROM_END
    fit_off = size - FIT_OFFSET_FROM_END
    if len(body) > nvram_off:
        raise SystemExit('fw-flash: body (%d bytes) reaches the NVRAM block'
                         % len(body))
    struct.pack_into('<Q', body, body_size_slot, len(body))

    entries = [
        fit_entry(UC | (base + pal_stub), 32, 0x0100, FIT_PAL_B),
        fit_entry(UC | (base + pal_stub), 32, 0x0100, FIT_PAL_A),
        fit_entry(base, len(body), 0x0100, FIT_BODY),
        fit_entry(0, 0, 0, FIT_UNUSED),
    ]
    count = len(entries) + 1
    header = struct.pack('<8sI', b'_FIT_   ', count) + \
        struct.pack('<HB', 0x0100, FIT_HEADER | 0x80)
    header += bytes([(-sum(header + b''.join(entries))) & 0xff])
    fit = header + b''.join(entries)

    image = bytearray(b'\xff' * size)
    image[:len(body)] = body
    image[fit_off:fit_off + len(fit)] = fit
    image[size - 48:size - 32] = fit_entry(UC | (base + pal_stub), 32,
                                           0x0100, FIT_PAL_A)
    struct.pack_into('<Q', image, size - 32, UC | (base + fit_off))
    struct.pack_into('<Q', image, size - 24, UC | (base + entry))
    with open(out_path, 'wb') as f:
        f.write(image)
    print('fw-flash: %d-byte body at 0x%x, FIT at 0x%x, SALE_ENTRY 0x%x'
          % (len(body), base, base + fit_off, base + entry))


if __name__ == '__main__':
    main()
