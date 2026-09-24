#!/usr/bin/env bash
#
# Build the IA-64 EFI/SAL/PAL firmware into build-packaging/firmware/.
#
# The firmware is a distro- and host-architecture-independent IA-64 ROM blob:
# build it once here, then copy build-packaging/firmware/ia64-firmware.bin into the Linux
# and Windows packages (and inject it into the per-distro .deb containers, which
# have no IA-64 cross toolchain of their own).
#
# It needs only the IA-64 cross toolchain (podman "fedora" shim) and the
# roms/ia64-firmware build script -- no Meson/Ninja, no QEMU configure.
#
# Usage:  contrib/packaging/build-firmware.sh

source "$(dirname "$(readlink -f "$0")")/lib-common.sh"
setup_logging firmware

OUT="$FWDIR"
WORK="$OUTROOT/.work-firmware"
register_cleanup "$WORK"

log "IA-64 firmware build"
ensure_src
FW_SRC="$SRC/roms/ia64-firmware"
[ -x "$FW_SRC/build_firmware.sh" ] || die "cannot find $FW_SRC/build_firmware.sh"
ensure_ia64_toolchain

# Intermediate .o/.elf/.sections go to WORK; only the deliverables land in OUT.
rm -rf "$WORK"; mkdir -p "$WORK" "$OUT"

log "compiling firmware (IA-64 cross toolchain via podman shim)"
sh "$FW_SRC/build_firmware.sh" \
    "$WORK/ia64-firmware.bin" \
    "$WORK/ia64-firmware.raw" \
    "$WORK/ia64-firmware.elf" \
    "$WORK/ia64-firmware.map" \
    "$WORK/ia64-firmware.sections" \
    "$FW_SRC" \
    "$WORK/ia64-firmware.d"

[ -s "$WORK/ia64-firmware.bin" ] || die "firmware build produced no output"
# build-packaging/firmware/ keeps the raw blob other builds consume as a shared input.
install -m0644 "$WORK/ia64-firmware.bin" "$OUT/ia64-firmware.bin"
install -m0644 "$WORK/ia64-firmware.map" "$OUT/ia64-firmware.map"

# Versioned deliverable zip in the unified output folder.
need_cmd zip "install zip"
ZIP="$DISTDIR/qemu-system-ia64-firmware_$(pkg_version).zip"
mkdir -p "$DISTDIR"
rm -f "$ZIP"
zip -qj "$ZIP" "$OUT/ia64-firmware.bin" "$OUT/ia64-firmware.map"
# The firmware is GPLv2; ship the license alongside it.
for f in LICENSE COPYING; do
    [ -f "$SRC/$f" ] && zip -qj "$ZIP" "$SRC/$f"
done

log "firmware ready:"
printf '  blob: %s (%s bytes)\n' "$OUT/ia64-firmware.bin" "$(stat -c%s "$OUT/ia64-firmware.bin")"
printf '  zip:  %s\n' "$ZIP"
