#!/usr/bin/env python3
"""Read and convert the non-volatile stores of the IA-64 machines.

SPDX-License-Identifier: GPL-2.0-or-later

`nvram=` names a different part on each board: the firmware flash on 460gx,
the PDH battery-backed SRAM on zx1, followed there by the BMC's tokens.  A file
written for one is refused by the other, so a store that was kept on zx1
before the move has to be converted.
"""

import argparse
import struct
import sys

# The zx1 store is a raw image of the battery-backed part, FF400000 up, then
# the BMC's tokens: a tag, and records of token number, size and value up to
# a zero number.  QEMU adds the token area to a file of just the part.
PDH_STORE_SIZE = 0x80000
BMC_TOKENS_SIZE = 0x1000
ZX1_FILE_SIZE = PDH_STORE_SIZE + BMC_TOKENS_SIZE
BMC_TOKENS_TAG = b"BMCTOKEN"
BMC_FIRST_BOOT_TOKEN = 0x500
BMC_FIRST_BOOT_DONE = 18
ZX1_SIZES = (PDH_STORE_SIZE, ZX1_FILE_SIZE)
# What the vendor firmware writes there (plans/nvram-portability.md sec 2.2).
VENDOR_TAGS = (
    (0x00100, b"TINI MVN", "SAL control block"),   # "NVM INIT", two DATA4s
    (0x15A00, b"TOK_INIT", "token store"),
    (0x38000, b"vbrl", "EFI variable bank 1"),
    (0x3C000, b"vbrl", "EFI variable bank 2"),
)
# Our own firmware keeps its store in the gap between those two parts.
OWN_STORE_OFFSET = 0x20000
OWN_STORE_SIZE = 0x10000
OWN_STORE_MAGIC = b"IVARSTOR"
# In a flash image the same store sits at FFF90000, and the image ends at 4 GiB.
FLASH_STORE_FROM_END = 0x100000000 - 0xFFF90000
# A flash image names its firmware interface table in the reset pointer block.
FLASH_FIT_FROM_END = 0x30
FLASH_PTR_ADDR_MASK = 0x00000000FFFFFFFF


def read(path):
    with open(path, "rb") as f:
        return f.read()


def own_store_in_flash(image):
    """The 64 KiB store of a flash image, or None when it holds no store."""
    if len(image) <= FLASH_STORE_FROM_END:
        return None
    start = len(image) - FLASH_STORE_FROM_END
    store = image[start:start + OWN_STORE_SIZE]
    return store if store[:8] == OWN_STORE_MAGIC else None


def is_flash_image(image):
    """True when the file ends at 4 GiB with a table pointer that fits in it."""
    if len(image) < 0x100000 or len(image) & (len(image) - 1):
        return False
    fit = struct.unpack_from("<Q", image, len(image) - FLASH_FIT_FROM_END)[0]
    fit &= FLASH_PTR_ADDR_MASK
    return 0x100000000 - len(image) <= fit < 0x100000000


def bmc_tokens(image):
    """The BMC's tokens of a zx1 file as {number: value}, or None."""
    area = image[PDH_STORE_SIZE:PDH_STORE_SIZE + BMC_TOKENS_SIZE]
    if len(image) != ZX1_FILE_SIZE or area[:8] != BMC_TOKENS_TAG:
        return None
    tokens = {}
    at = len(BMC_TOKENS_TAG)
    while at + 3 <= len(area):
        number, size = struct.unpack_from("<HB", area, at)
        if number == 0 or at + 3 + size > len(area):
            break
        tokens[number] = area[at + 3:at + 3 + size]
        at += 3 + size
    return tokens


def own_store(image):
    """The 64 KiB store of any file that carries one."""
    if len(image) == OWN_STORE_SIZE and image[:8] == OWN_STORE_MAGIC:
        return image
    if len(image) in ZX1_SIZES:
        store = image[OWN_STORE_OFFSET:OWN_STORE_OFFSET + OWN_STORE_SIZE]
        if store[:8] == OWN_STORE_MAGIC:
            return store
        return None
    return own_store_in_flash(image)


def describe(path):
    image = read(path)
    lines = ["%s: %d bytes" % (path, len(image))]
    if len(image) in ZX1_SIZES:
        held = [name for off, tag, name in VENDOR_TAGS
                if image[off:off + len(tag)] == tag]
        if own_store(image) is not None:
            held.append("project firmware variables")
        lines.append("  zx1 PDH battery-backed store")
        lines.append("  holds: " + (", ".join(held) if held else "nothing"))
        tokens = bmc_tokens(image)
        if tokens is None:
            lines.append("  BMC tokens: none, a new BMC")
        else:
            done = tokens.get(BMC_FIRST_BOOT_TOKEN, b"")[:1] == \
                bytes([BMC_FIRST_BOOT_DONE])
            lines.append("  BMC tokens: %d, %s" % (
                len(tokens), "loaded by the vendor firmware" if done
                else "not loaded yet"))
    elif len(image) == OWN_STORE_SIZE and image[:8] == OWN_STORE_MAGIC:
        lines.append("  project firmware variable store (the 64 KiB form)")
        lines.append("  convert it before it is used on zx1")
    elif is_flash_image(image):
        lines.append("  firmware flash image: 460gx `nvram=`")
        if own_store_in_flash(image) is not None:
            lines.append("  holds project firmware variables; convert them "
                         "before they are used on zx1")
    elif not image or set(image) <= {0x00, 0xFF}:
        lines.append("  blank; either board accepts it")
    else:
        lines.append("  not a store this tool knows")
    print("\n".join(lines))
    return 0


def convert(src, dst):
    image = read(src)
    if len(image) in ZX1_SIZES:
        print("%s is already a zx1 store" % src, file=sys.stderr)
        return 1
    store = own_store(image)
    if store is None:
        print("%s holds no project firmware variable store" % src,
              file=sys.stderr)
        return 1
    out = bytearray(ZX1_FILE_SIZE)
    out[OWN_STORE_OFFSET:OWN_STORE_OFFSET + OWN_STORE_SIZE] = store
    with open(dst, "wb") as f:
        f.write(out)
    count = struct.unpack_from("<I", store, 12)[0]
    print("%s: zx1 store with %d variable slots from %s" % (dst, count, src))
    return 0


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="command", required=True)
    p = sub.add_parser("identify", help="say what a store file is")
    p.add_argument("file", nargs="+")
    p = sub.add_parser("convert", help="build a zx1 store from an older file")
    p.add_argument("source")
    p.add_argument("destination")
    args = parser.parse_args(argv)
    if args.command == "identify":
        return max(describe(f) for f in args.file)
    return convert(args.source, args.destination)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
