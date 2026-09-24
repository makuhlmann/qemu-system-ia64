#!/usr/bin/env bash
#
# Build the self-contained Windows x86_64 package of qemu-system-ia64 into
# build-packaging/dist/qemu-system-ia64_<ver>_win64.zip.
#
# The actual cross build runs inside an ephemeral fedora:44 podman container
# (win-in-container.sh), because Fedora's dnf resolves the full mingw64 GTK stack
# automatically -- so the package gets a GTK display (plus SDL via sdl2-compat,
# VNC, GnuTLS, curl) with no MSYS2 hand-pinning. It ships the qemu-img/qemu-io/…
# utilities and every non-system DLL. The host needs only podman and zip.
#
# Usage:   contrib/packaging/build-windows.sh
# Env:     JOBS=N  LTO=0  KEEP_WORK=1   (see lib-common.sh)

source "$(dirname "$(readlink -f "$0")")/lib-common.sh"
setup_logging windows

WORK="$OUTROOT/.work-windows"
register_cleanup "$WORK"
IMAGE="${BASE_IMAGE:-registry.fedoraproject.org/fedora:44}"

log "Windows cross build (fedora container)  jobs=$JOBS lto=$LTO"

# --- Preflight --------------------------------------------------------------
need_cmd podman
need_cmd zip "install zip"
need_cmd git
ensure_src
[ -f "$FWDIR/ia64-firmware.bin" ] \
    || die "firmware missing; run contrib/packaging/build-firmware.sh first"

zipbase="qemu-system-ia64_$(pkg_version)_win64"
rm -rf "$WORK"; mkdir -p "$WORK"
mkdir -p "$DISTDIR"

# --- Build + assemble the package inside the container ----------------------
log "building in $IMAGE (dnf pulls the mingw64 GTK stack; this takes a while)"
podman run --rm \
    -v "$SRC":/src:ro \
    -v "$FWDIR":/firmware:ro \
    -v "$LIB_DIR/win-in-container.sh":/win-in-container.sh:ro \
    -v "$WORK":/work \
    -e QEMU_ZIPBASE="$zipbase" \
    -e JOBS="$JOBS" \
    -e LTO="$LTO" \
    "$IMAGE" \
    bash /win-in-container.sh

PKGDIR="$WORK/out/$zipbase"
[ -f "$PKGDIR/qemu-system-ia64.exe" ] || die "container produced no qemu-system-ia64.exe"
[ -f "$(find "$PKGDIR/share" -name ia64-firmware.bin -print -quit 2>/dev/null)" ] \
    || die "firmware missing from package"

# --- Pack the versioned zip (the only published artifact) -------------------
ZIP="$DISTDIR/${zipbase}.zip"
log "packing ${zipbase}.zip"
rm -f "$ZIP"
( cd "$WORK/out" && zip -rq "$ZIP" "$zipbase" )

log "Windows package ready:"
printf '  zip: %s (%s)\n' "$ZIP" "$(du -h "$ZIP" | cut -f1)"
( cd "$PKGDIR" && ls -1 ./*.exe )
printf '  display: GTK + SDL, %s DLLs bundled\n' "$(ls -1 "$PKGDIR"/*.dll | wc -l)"
