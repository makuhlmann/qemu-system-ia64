#!/usr/bin/env bash
#
# Runs INSIDE an ephemeral fedora:44 container to cross-build the Windows package
# (mirrors the project's CI windows-cross job). Fedora is used because dnf
# resolves the full mingw64 GTK stack automatically, so the package gets a GTK
# display (plus SDL via sdl2-compat, VNC, curl, GnuTLS) without hand-pinning
# MSYS2 packages.
#
# Mounts: /src (ro) source, /firmware (ro) prebuilt blob, /work (rw) build+output.
# Produces /work/out/$QEMU_ZIPBASE/ ready for the host to zip.
# Env: QEMU_ZIPBASE JOBS LTO

set -euo pipefail
: "${QEMU_ZIPBASE:?}"
JOBS="${JOBS:-$(nproc)}"
LTO="${LTO:-1}"
CROSS=x86_64-w64-mingw32
MINGW=/usr/${CROSS}/sys-root/mingw

echo "==> dnf: installing mingw64 toolchain + GTK stack"
dnf install -y --setopt=install_weak_deps=False \
    bash bison bzip2 diffutils findutils flex \
    gcc gcc-c++ git glib2 glib2-devel glibc-langpack-en make ninja-build \
    pkgconf-pkg-config python3 python3-pip which zip \
    mingw64-gcc mingw64-gcc-c++ mingw64-glib2 mingw64-pixman \
    mingw64-pkg-config mingw64-gtk3 mingw64-SDL2 \
    mingw64-libpng mingw64-libjpeg-turbo mingw64-curl mingw64-gnutls \
    mingw64-bzip2 mingw64-libusb1 mingw64-usbredir mingw64-zstd mingw-w64-tools \
    gcc-ia64-linux-gnu binutils-ia64-linux-gnu acpica-tools >/dev/null
echo "    mingw64-gtk3: $(rpm -q --qf '%{version}' mingw64-gtk3)"

# QEMU's configure editable-installs its python package into the source tree;
# /src is read-only, so build from a writable copy.
SRCDIR=/tmp/qemu-src
echo "==> copying source tree to a writable location"
cp -a /src/. "$SRCDIR"/

BUILD=/work/build
STAGE=/work/stage
rm -rf "$BUILD" "$STAGE"; mkdir -p "$BUILD" "$STAGE"

# Keep only the perf cflags the cross compiler accepts (probe with mingw gcc).
EXTRA_CFLAGS=""
for _flag in -fno-stack-protector -fzero-call-used-regs=skip -ftrivial-auto-var-init=uninitialized; do
    if printf 'int main(void){return 0;}\n' | "${CROSS}-gcc" "$_flag" -x c -c -o /dev/null - >/dev/null 2>&1; then
        EXTRA_CFLAGS="${EXTRA_CFLAGS:+$EXTRA_CFLAGS }$_flag"
    fi
done
lto_flag=--disable-lto
[ "$LTO" = 1 ] && lto_flag=--enable-lto

echo "==> configure (mingw cross, GTK + SDL + VNC)"
cd "$BUILD"
"$SRCDIR"/configure \
    --cross-prefix="${CROSS}-" \
    --host-cc=gcc \
    --python=/usr/bin/python3 \
    --prefix=/qemu \
    --target-list=ia64-softmmu \
    --without-default-features \
    --enable-system --enable-tcg \
    --enable-pixman --enable-fdt=internal \
    --enable-gtk --enable-sdl --enable-vnc --enable-png \
    --enable-gnutls --enable-curl \
    --enable-slirp --enable-tools \
    --enable-vpc --enable-vhdx --enable-vdi --enable-vmdk --enable-qed \
    --enable-qcow1 --enable-parallels --enable-vvfat --enable-bochs \
    --enable-cloop --enable-dmg \
    --enable-libusb -Dusb_redir=enabled \
    -Ddsound=enabled --audio-drv-list=dsound \
    -Dzstd=enabled -Dvnc_jpeg=enabled \
    "$lto_flag" --enable-strip -Doptimization=2 \
    --disable-docs --disable-werror --disable-qom-cast-debug --disable-stack-protector \
    --extra-cflags="$EXTRA_CFLAGS"

echo "==> build (fedora has the IA-64 toolchain, so the firmware target builds too)"
ninja -C "$BUILD" -j"$JOBS"

echo "==> install into staging"
DESTDIR="$STAGE" ninja -C "$BUILD" install

# Windows install is flat: exes at the prefix root, blobs in share/. Inject the
# shared firmware blob next to the other data (it is install:false in Meson).
DATADIR="$(dirname "$(find "$STAGE" -name vgabios-ati.bin -print -quit)")"
install -m0644 /firmware/ia64-firmware.bin "$DATADIR/ia64-firmware.bin"

PKGDIR="/work/out/$QEMU_ZIPBASE"
rm -rf "$PKGDIR"; mkdir -p "$PKGDIR"
cp -a "$STAGE/qemu/." "$PKGDIR/"
rm -rf "$PKGDIR/include" "$PKGDIR/lib/pkgconfig" "$PKGDIR"/lib/*.a 2>/dev/null || true

# --- GTK runtime bits that a DLL import sweep cannot see --------------------
# GSettings schemas (GTK aborts some dialogs without them).
if [ -d "$MINGW/share/glib-2.0/schemas" ]; then
    mkdir -p "$PKGDIR/share/glib-2.0/schemas"
    glib-compile-schemas --targetdir="$PKGDIR/share/glib-2.0/schemas" \
        "$MINGW/share/glib-2.0/schemas" || true
fi
# gdk-pixbuf image loaders (for icons/themes), best effort.
PIXBUF_VER="$(basename "$(find "$MINGW/lib/gdk-pixbuf-2.0" -maxdepth 1 -type d -name '2.*' 2>/dev/null | head -1)" 2>/dev/null || true)"
if [ -n "$PIXBUF_VER" ] && [ -d "$MINGW/lib/gdk-pixbuf-2.0/$PIXBUF_VER/loaders" ]; then
    dst="$PKGDIR/lib/gdk-pixbuf-2.0/$PIXBUF_VER"
    mkdir -p "$dst/loaders"
    cp "$MINGW/lib/gdk-pixbuf-2.0/$PIXBUF_VER/loaders/"*.dll "$dst/loaders/" 2>/dev/null || true
    [ -f "$MINGW/lib/gdk-pixbuf-2.0/$PIXBUF_VER/loaders.cache" ] \
        && cp "$MINGW/lib/gdk-pixbuf-2.0/$PIXBUF_VER/loaders.cache" "$dst/" || true
fi

# --- Bundle runtime DLLs (self-contained) ----------------------------------
echo "==> bundling runtime DLLs"
search_dirs=("$MINGW/bin")
for probe in libwinpthread-1.dll libgcc_s_seh-1.dll libstdc++-6.dll; do
    p="$("${CROSS}-gcc" -print-file-name="$probe" 2>/dev/null || true)"
    [ -n "$p" ] && [ "$p" != "$probe" ] && [ -f "$p" ] && search_dirs+=("$(dirname "$p")")
done
find_dll() { local n="$1" d; for d in "${search_dirs[@]}"; do [ -f "$d/$n" ] && { printf '%s\n' "$d/$n"; return 0; }; done; return 1; }
(
    cd "$PKGDIR"
    # Fedora's mingw64-SDL2 is sdl2-compat: SDL2.dll dlopens SDL3.dll, invisible
    # to an import-table sweep. Seed it (and any loader DLLs' own deps resolve below).
    for extra in SDL3.dll; do
        s="$(find_dll "$extra" || true)"; [ -n "$s" ] && cp -f "$s" .
    done
    for _pass in 1 2 3; do
        for f in *.exe *.dll lib/gdk-pixbuf-2.0/*/loaders/*.dll; do
            [ -e "$f" ] || continue
            "${CROSS}-objdump" -p "$f" 2>/dev/null | awk '/DLL Name/{print $3}'
        done | sort -u | while read -r dll; do
            if [ ! -f "$dll" ]; then
                s="$(find_dll "$dll" || true)"; [ -n "$s" ] && cp -f "$s" .
            fi
        done
    done
    [ -f SDL2.dll ] && [ ! -f SDL3.dll ] && { echo "SDL2 present but SDL3 missing" >&2; exit 1; }
    echo "    $(ls -1 ./*.dll | wc -l) DLLs, $(ls -1 ./*.exe | wc -l) exes"
)

# --- Licenses ---------------------------------------------------------------
# QEMU's own licenses plus the license text of every bundled third-party DLL
# (LGPL/MIT/BSD/... compliance for a self-contained binary distribution).
echo "==> collecting license files"
LIC="$PKGDIR/licenses"
mkdir -p "$LIC/thirdparty"
for f in LICENSE COPYING COPYING.LIB; do
    [ -f "$SRCDIR/$f" ] && cp "$SRCDIR/$f" "$LIC/QEMU-$f"
done

# Where the bundled DLLs came from, so we can map each back to its rpm.
lic_dirs=("$MINGW/bin")
for probe in libwinpthread-1.dll libgcc_s_seh-1.dll libstdc++-6.dll; do
    p="$("${CROSS}-gcc" -print-file-name="$probe" 2>/dev/null || true)"
    [ -n "$p" ] && [ -f "$p" ] && lic_dirs+=("$(dirname "$p")")
done
declare -A lic_seen
collect_pkg_license() {
    local pkg="$1" outd got=0 lf
    [ -z "$pkg" ] && return 0
    [ -n "${lic_seen[$pkg]:-}" ] && return 0
    lic_seen[$pkg]=1
    outd="$LIC/thirdparty/$pkg"; mkdir -p "$outd"
    while IFS= read -r lf; do
        if [ -f "$lf" ]; then cp "$lf" "$outd/" 2>/dev/null && got=1 || true; fi
    done < <(rpm -q --licensefiles "$pkg" 2>/dev/null || true)
    if [ "$got" = 0 ] && [ -d "/usr/share/licenses/$pkg" ]; then
        cp -a "/usr/share/licenses/$pkg/." "$outd/" 2>/dev/null && got=1 || true
    fi
    [ "$got" = 0 ] && rmdir "$outd" 2>/dev/null || true
    return 0
}
while IFS= read -r dll; do
    base="$(basename "$dll")"; src=""
    for d in "${lic_dirs[@]}"; do [ -f "$d/$base" ] && { src="$d/$base"; break; }; done
    [ -n "$src" ] || continue
    collect_pkg_license "$(rpm -qf --qf '%{NAME}\n' "$src" 2>/dev/null | head -1)"
done < <( { ls -1 "$PKGDIR"/*.dll 2>/dev/null; find "$PKGDIR/lib" -name '*.dll' 2>/dev/null; } )

cat > "$LIC/README.txt" <<EOF
qemu-system-ia64 -- license information
=======================================
The QEMU emulator is licensed under the GNU General Public License version 2.
QEMU-LICENSE describes the per-component licensing; QEMU-COPYING is the GPLv2
text and QEMU-COPYING.LIB the LGPL text.

This package also bundles third-party runtime libraries (GTK, GLib, Cairo,
Pango, gdk-pixbuf, GnuTLS, cURL, SDL, pixman, zlib and their dependencies).
Each retains its own license; the texts are under thirdparty/<package>/.

Source code
-----------
Complete corresponding source for qemu-system-ia64 is at
https://github.com/makuhlmann/qemu-system-ia64 (branch: develop). Upstream QEMU
source is at https://www.qemu.org/. Source for the bundled libraries is
available from the Fedora Project (https://src.fedoraproject.org/) and the
respective upstream projects.
EOF
echo "    QEMU licenses + $(ls -1 "$LIC/thirdparty" 2>/dev/null | wc -l) third-party license sets"

echo "==> package assembled at $PKGDIR"
