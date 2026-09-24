#!/usr/bin/env bash
#
# Runs INSIDE an ephemeral Ubuntu container (see the per-release wrappers).
# Installs build dependencies, builds a curated qemu-system-ia64, stages it into
# a private tree, computes dependencies with dpkg-shlibdeps, and emits a .deb.
#
# The container has NO IA-64 cross toolchain; the firmware is injected as a
# prebuilt blob at /firmware/ia64-firmware.bin, and only the qemu-system-ia64
# target is built (never the firmware target).
#
# Mounts provided by the caller:
#   /src        QEMU source tree (read-only)
#   /firmware   prebuilt IA-64 firmware (read-only)
#   /work       build + staging scratch (read-write)
#   /out        finished .deb lands here (read-write)
# Env: DEV_PACKAGES QEMU_DEB_VERSION DISTRO_TAG MAINTAINER JOBS LTO

set -euo pipefail
export DEBIAN_FRONTEND=noninteractive

: "${DEV_PACKAGES:?}" "${QEMU_DEB_VERSION:?}" "${DISTRO_TAG:?}" "${MAINTAINER:?}"
JOBS="${JOBS:-$(nproc)}"
LTO="${LTO:-1}"
# Honor an explicit compiler (e.g. clang on Debian 11); ignore empty passthrough
# so configure falls back to its default cc/c++.
[ -n "${CC:-}" ]  || unset CC
[ -n "${CXX:-}" ] || unset CXX

PREFIX=/usr/lib/qemu-system-ia64          # private tree, no collision with distro qemu
BUILD=/work/build
STAGE=/work/stage
PKG=/work/pkg

echo "==> apt: installing build dependencies"
apt-get update -qq
# shellcheck disable=SC2086
apt-get install -y --no-install-recommends $DEV_PACKAGES

# QEMU's configure needs a TOML parser: stdlib tomllib (Python >= 3.11) or the
# tomli package. Ensure one is importable. Debian 11 (Python 3.9) has neither by
# default and no python3-tomli in main, so fall back to pip.
if ! python3 -c 'import tomllib' 2>/dev/null && ! python3 -c 'import tomli' 2>/dev/null; then
    echo "==> providing a TOML parser (Python < 3.11)"
    apt-get install -y --no-install-recommends python3-tomli 2>/dev/null \
        || pip3 install --quiet tomli 2>/dev/null \
        || pip3 install --quiet --break-system-packages tomli 2>/dev/null \
        || { echo "could not provide tomli" >&2; exit 1; }
fi

# QEMU's configure does an editable install of its python package, which older
# pip (e.g. 22.04's) writes back into the source tree (qemu.egg-info). /src is
# mounted read-only (clean-room), so build from a writable in-container copy.
SRCDIR=/tmp/qemu-src
echo "==> copying source tree to a writable location"
rm -rf "$SRCDIR"; mkdir -p "$SRCDIR"
cp -a /src/. "$SRCDIR"/

echo "==> configure (curated feature set)"
rm -rf "$BUILD" "$STAGE" "$PKG"
mkdir -p "$BUILD" "$STAGE" "$PKG/debian"
lto_flag=--disable-lto
[ "$LTO" = 1 ] && lto_flag=--enable-lto

# Keep only the performance cflags this release's compiler understands.
# e.g. -ftrivial-auto-var-init needs gcc >= 12, so it is dropped on 22.04's
# gcc-11 (where it is a no-op anyway).
CC="${CC:-cc}"
EXTRA_CFLAGS=""
# -Wno-unknown-attributes silences clang-11's 595 "unknown attribute 'error'"
# warnings (it lacks GCC's __attribute__((error)); gcc has nothing to suppress).
for _flag in -fno-stack-protector -fzero-call-used-regs=skip -ftrivial-auto-var-init=uninitialized -Wno-unknown-attributes; do
    if printf 'int main(void){return 0;}\n' | "$CC" "$_flag" -x c -c -o /dev/null - >/dev/null 2>&1; then
        EXTRA_CFLAGS="${EXTRA_CFLAGS:+$EXTRA_CFLAGS }$_flag"
    else
        echo "    (compiler lacks $_flag - skipping)"
    fi
done
echo "    extra-cflags: $EXTRA_CFLAGS"

# gnutls (VNC / migration TLS) is enabled only where the version satisfies
# QEMU's minimum; 22.04 ships 3.7.3 (< 3.7.5), so it is dropped there. VNC
# itself still works without it.
gnutls_flag=--enable-gnutls
if ! pkg-config --atleast-version=3.7.5 gnutls 2>/dev/null; then
    gnutls_flag=--disable-gnutls
    echo "    (gnutls too old or absent - TLS support disabled)"
fi

cd "$BUILD"
"$SRCDIR"/configure \
    --prefix="$PREFIX" \
    --target-list=ia64-softmmu \
    --without-default-features \
    --enable-system --enable-tcg \
    --enable-pixman --enable-fdt=internal \
    --enable-gtk --enable-vte --enable-sdl \
    --enable-vnc --enable-png "$gnutls_flag" \
    --enable-slirp --enable-curl \
    --enable-libusb -Dusb_redir=enabled \
    --enable-vpc --enable-vhdx --enable-vdi --enable-vmdk --enable-qed \
    --enable-qcow1 --enable-parallels --enable-vvfat --enable-bochs \
    --enable-cloop --enable-dmg \
    -Dzstd=enabled -Dvnc_jpeg=enabled -Dcurses=enabled -Diconv=enabled \
    -Dseccomp=enabled -Dlinux_io_uring=enabled -Dlinux_aio=enabled \
    -Dpa=enabled -Dalsa=enabled --audio-drv-list=pa,alsa \
    --disable-tools --disable-guest-agent \
    --disable-install-blobs --disable-docs --disable-werror \
    --disable-qom-cast-debug --disable-stack-protector \
    "$lto_flag" -Doptimization=2 \
    --extra-cflags="$EXTRA_CFLAGS"

echo "==> build only the qemu-system-ia64 target"
# Building this target does NOT build the IA-64 firmware; 'ninja install' would
# (the firmware is build_by_default) and then fail, because this container has
# no IA-64 cross toolchain. So we stage the payload by hand and inject the
# prebuilt firmware below.
ninja -C "$BUILD" -j"$JOBS" qemu-system-ia64

echo "==> stage into private tree ($PREFIX)"
install -D -m0755 "$BUILD/qemu-system-ia64" "$STAGE$PREFIX/bin/qemu-system-ia64"
strip "$STAGE$PREFIX/bin/qemu-system-ia64"

# Curated data payload: our firmware + only the blobs the ia64-vpc machine
# loads. The binary is relocatable and resolves this directory relative to
# itself (prefix/share/qemu), which is exactly where it is installed.
DATADIR="$STAGE$PREFIX/share/qemu"
mkdir -p "$DATADIR/keymaps"
install -m0644 /firmware/ia64-firmware.bin "$DATADIR/ia64-firmware.bin"
# All ROMs the ia64-vpc machine can load: the four VGA BIOSes it selects
# between (vga=rage128/mach64/nv15gl/std) and the option ROMs of every -nic model.
for rom in vgabios-ati.bin vgabios-mach64.bin vgabios-nv15gl.bin vgabios-stdvga.bin \
           efi-e1000.rom efi-e1000e.rom pxe-eepro100.rom; do
    install -m0644 "$SRCDIR/pc-bios/$rom" "$DATADIR/$rom"
done
cp -a "$SRCDIR"/pc-bios/keymaps/. "$DATADIR/keymaps/"

# Expose the (relocatable) binary on PATH via a symlink; it still finds its
# data next to its real location.
mkdir -p "$STAGE/usr/bin"
ln -sf "$PREFIX/bin/qemu-system-ia64" "$STAGE/usr/bin/qemu-system-ia64"

# Licenses (Debian policy: /usr/share/doc/<pkg>/copyright). The bundled runtime
# libraries come from apt, which carries their own copyright files, so only
# QEMU's own licensing needs to ship here.
DOCDIR="$STAGE/usr/share/doc/qemu-system-ia64"
mkdir -p "$DOCDIR"
for f in LICENSE COPYING COPYING.LIB; do
    [ -f "$SRCDIR/$f" ] && install -m0644 "$SRCDIR/$f" "$DOCDIR/$f"
done
cat > "$DOCDIR/copyright" <<EOF
qemu-system-ia64 -- experimental QEMU IA-64 system emulator
Upstream source: https://github.com/makuhlmann/qemu-system-ia64 (branch: develop)

The QEMU emulator is released under the GNU General Public License, version 2
(see COPYING). Library components are under the GNU LGPL (see COPYING.LIB).
Per-file licensing is documented in LICENSE. On Debian systems the full GPL-2
text is also at /usr/share/common-licenses/GPL-2.
EOF

echo "==> resolve runtime dependencies (dpkg-shlibdeps)"
BIN="$STAGE$PREFIX/bin/qemu-system-ia64"
[ -x "$BIN" ] || { echo "staged binary missing: $BIN" >&2; exit 1; }
cat > "$PKG/debian/control" <<EOF
Source: qemu-system-ia64
Package: qemu-system-ia64
Architecture: amd64
EOF
( cd "$PKG" && dpkg-shlibdeps -O -e"$BIN" ) > "$PKG/shlibdeps.txt" 2>"$PKG/shlibdeps.err" || {
    echo "dpkg-shlibdeps failed:" >&2; cat "$PKG/shlibdeps.err" >&2; exit 1; }
SHDEPS="$(sed -n 's/^shlibs:Depends=//p' "$PKG/shlibdeps.txt")"
[ -n "$SHDEPS" ] || { echo "shlibdeps produced no Depends" >&2; exit 1; }
echo "    Depends: $SHDEPS"

echo "==> writing control + building .deb"
ISIZE="$(du -ks "$STAGE/usr" | cut -f1)"
mkdir -p "$STAGE/DEBIAN"
cat > "$STAGE/DEBIAN/control" <<EOF
Package: qemu-system-ia64
Version: $QEMU_DEB_VERSION
Architecture: amd64
Maintainer: $MAINTAINER
Installed-Size: $ISIZE
Depends: $SHDEPS
Recommends: qemu-utils
Section: otherosfs
Priority: optional
Homepage: https://github.com/makuhlmann/qemu-system-ia64
Description: QEMU system emulator for IA-64 (Itanium) guests
 Experimental QEMU full-system emulator for IA-64/Itanium (Merced, Madison,
 Montecito), with project-owned IA-64 EFI/SAL/PAL firmware. Runs Windows XP
 64-bit Edition, Windows Server 2003 IA-64, and IA-64 Linux guests.
 .
 This package ships only the qemu-system-ia64 binary and its firmware; disk
 image utilities such as qemu-img come from the qemu-utils package.
EOF

FNVER="${QEMU_DEB_VERSION#*:}"   # drop epoch for the filename
DEB="/out/qemu-system-ia64_${FNVER}_${DISTRO_TAG}_amd64.deb"
dpkg-deb --root-owner-group --build "$STAGE" "$DEB"

echo "==> built $DEB"
dpkg-deb -I "$DEB"
echo "---- package contents (top level) ----"
dpkg-deb -c "$DEB" | awk '{print $6}' | grep -vE '/keymaps/.+' | sort | head -40
