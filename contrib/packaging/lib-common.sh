# shellcheck shell=bash
#
# Shared helpers for the qemu-system-ia64 packaging build scripts.
# Sourced by the build-*.sh scripts and deb/lib-deb.sh; not meant to run on its own.
#
# The scripts package the source tree they live in (contrib/packaging/ of the
# fork). They write only below $PKG_OUT (default build-packaging/ in that tree,
# ignored by /build-*/), never into build/. Work trees stay inside the source
# tree so that a containerized IA-64 cross toolchain which maps the tree can
# reach them.

set -euo pipefail

# --- Path layout ------------------------------------------------------------
LIB_DIR="$(cd -P "$(dirname "${BASH_SOURCE[0]}")" && pwd)"   # contrib/packaging
SRC="${QEMU_SRC:-$(cd -P "$LIB_DIR/../.." && pwd)}"
OUTROOT="${PKG_OUT:-$SRC/build-packaging}"
FWDIR="$OUTROOT/firmware"                # shared firmware blob other builds consume
LOGDIR="$OUTROOT/logs"                   # preserved build logs (survive cleanup)
DISTDIR="$OUTROOT/dist"                  # unified output: .deb, firmware zip, Windows zip

# A shallow clone would give a wrong commit count in the package version.
ensure_src() {
    [ -x "$SRC/configure" ] || die "no QEMU source tree at $SRC (set QEMU_SRC)"
    if [ "$(git -C "$SRC" rev-parse --is-shallow-repository 2>/dev/null)" = true ]; then
        warn "$SRC is a shallow clone; the package version will be wrong (git fetch --unshallow)"
    fi
    mkdir -p "$OUTROOT"
}

# Package version in filename form: 11.0.50+makuhlmann<commit count>. The fork
# keeps QEMU's own version at 11.0.50, so the commit count disambiguates builds.
# The .deb prepends the "1:" epoch on top of this.
pkg_version() {
    local count
    count="$(git -C "$SRC" rev-list --count HEAD 2>/dev/null || echo 0)"
    printf '11.0.50+makuhlmann%s' "$count"
}

# --- Tunables (environment overrides) ---------------------------------------
JOBS="${JOBS:-$(nproc)}"                 # parallel build jobs
LTO="${LTO:-1}"                          # 1 = --enable-lto (heavier link, faster binary)
STRIP="${STRIP:-1}"                      # 1 = strip installed binaries (smaller package)
NATIVE="${NATIVE:-0}"                    # 1 = -march=native + -Doptimization=3 (NOT portable)
INCREMENTAL="${INCREMENTAL:-0}"          # 1 = reuse the work tree between runs (faster)
KEEP_WORK="${KEEP_WORK:-0}"              # 1 = keep work/temp trees after a build (debugging)

# --- Logging ----------------------------------------------------------------
_c_bold=$'\033[1m'; _c_red=$'\033[31m'; _c_grn=$'\033[32m'; _c_ylw=$'\033[33m'; _c_rst=$'\033[0m'
log()  { printf '%s==>%s %s\n' "$_c_grn$_c_bold" "$_c_rst" "$*" >&2; }
warn() { printf '%swarning:%s %s\n' "$_c_ylw$_c_bold" "$_c_rst" "$*" >&2; }
die()  { printf '%serror:%s %s\n' "$_c_red$_c_bold" "$_c_rst" "$*" >&2; exit 1; }

# Tee all output of the current build to $LOGDIR/<name>.log so a failure can
# be diagnosed after the work tree is cleaned up.
setup_logging() {
    mkdir -p "$LOGDIR"
    exec > >(tee "$LOGDIR/$1.log") 2>&1
}

# --- Cleanup ----------------------------------------------------------------
# Work/temp trees registered here are removed when the script exits (success or
# failure), so a build never leaves scratch behind. On failure, Meson's logs are
# first salvaged into $LOGDIR for diagnosis. KEEP_WORK=1 (or INCREMENTAL=1)
# preserves the trees instead.
_CLEANUP_DIRS=()
register_cleanup() { _CLEANUP_DIRS+=("$1"); }
_run_cleanup() {
    local rc=$?
    [ "$KEEP_WORK" = "1" ] && return "$rc"
    [ "$INCREMENTAL" = "1" ] && return "$rc"
    local d ml
    for d in "${_CLEANUP_DIRS[@]:-}"; do
        [ -n "$d" ] && [ -d "$d" ] || continue
        if [ "$rc" -ne 0 ]; then
            for ml in "$d/meson-logs" "$d/build/meson-logs"; do
                if [ -d "$ml" ]; then
                    mkdir -p "$LOGDIR/$(basename "$d")"
                    cp -a "$ml" "$LOGDIR/$(basename "$d")/" 2>/dev/null || true
                fi
            done
        fi
        rm -rf -- "$d"
    done
    return "$rc"
}
trap _run_cleanup EXIT

# --- Common configure options (performance-oriented, see README §Performance) -
# Optimization level is set through Meson (-Doptimization=), never via -O in
# --extra-cflags, so exactly one level reaches the compiler.
opt_level=2
extra_cflags="-fno-stack-protector -fzero-call-used-regs=skip -ftrivial-auto-var-init=uninitialized"
if [ "$NATIVE" = "1" ]; then
    opt_level=3
    extra_cflags="$extra_cflags -march=native -mtune=native"
    warn "NATIVE=1: building for the local CPU generation only; the result is not portable to other hosts."
fi

# Emit the flags common to both targets into the array named by $1.
common_configure_flags() {
    local -n _out="$1"
    _out=(
        --target-list=ia64-softmmu
        --disable-docs
        --disable-werror
        --disable-qom-cast-debug
        --disable-stack-protector
        "-Doptimization=$opt_level"
        "--extra-cflags=$extra_cflags"
    )
    if [ "$LTO" = "1" ]; then _out+=(--enable-lto); else _out+=(--disable-lto); fi
    if [ "$STRIP" = "1" ]; then _out+=(--enable-strip); fi
}

# --- Preflight --------------------------------------------------------------
need_cmd() { command -v "$1" >/dev/null 2>&1 || die "required command not found: $1${2:+  ($2)}"; }

# The QEMU C sources build with the host compiler; only the IA-64 firmware
# needs the cross toolchain (Fedora: gcc-ia64-linux-gnu, binutils-ia64-linux-gnu),
# either native or through PATH shims into a podman container named "fedora".
ensure_ia64_toolchain() {
    command -v ia64-linux-gnu-gcc >/dev/null 2>&1 \
        || die "ia64-linux-gnu-gcc not on PATH (see contrib/packaging/README.md, Prerequisites)"

    # A shim forwards into the "fedora" container; make sure it is running.
    if command -v podman >/dev/null 2>&1 && podman inspect fedora >/dev/null 2>&1; then
        if ! podman ps --format '{{.Names}}' | grep -qx fedora; then
            log "starting podman container 'fedora' (IA-64 firmware toolchain)"
            podman start fedora >/dev/null 2>&1 || true
        fi
    fi
    ia64-linux-gnu-gcc --version >/dev/null 2>&1 \
        || die "ia64-linux-gnu-gcc is not functional; is the podman container 'fedora' running? (podman start fedora)"
}

# Prepare a work (build) tree: wipe unless INCREMENTAL=1.
prepare_work_dir() {
    local d="$1"
    if [ "$INCREMENTAL" = "1" ] && [ -f "$d/build.ninja" ]; then
        log "reusing existing work tree $d (INCREMENTAL=1)"
    else
        rm -rf -- "$d"
        mkdir -p -- "$d"
    fi
}

# After `ninja install` into $1 (DESTDIR), drop the IA-64 firmware next to the
# other data blobs (it is install:false in Meson) and trim link-time-only files.
# Locating vgabios-ati.bin makes this work for both the Linux (share/qemu) and
# Windows (share) data-directory layouts.
finish_install_tree() {
    local destdir="$1" work="$2"
    # Prefer the shared blob from build-firmware.sh; fall back to one built
    # in-tree by ninja. Container-based builds have no IA-64 toolchain, so they
    # rely on the shared blob being present.
    local fwdir="$FWDIR"
    [ -f "$fwdir/ia64-firmware.bin" ] || fwdir="$work/roms/ia64-firmware"
    local fw="$fwdir/ia64-firmware.bin"
    [ -f "$fw" ] || die "firmware not found; run contrib/packaging/build-firmware.sh first (looked in $FWDIR and $work/roms/ia64-firmware)"

    local anchor datadir
    anchor="$(find "$destdir" -name vgabios-ati.bin -print -quit 2>/dev/null || true)"
    [ -n "$anchor" ] || die "installed data directory not found (no vgabios-ati.bin under $destdir)"
    datadir="$(dirname "$anchor")"
    install -m0644 "$fw" "$datadir/ia64-firmware.bin"
    [ -f "$fwdir/ia64-firmware.map" ] \
        && install -m0644 "$fwdir/ia64-firmware.map" "$datadir/ia64-firmware.map" || true
    log "firmware installed to ${datadir#$destdir}/ia64-firmware.bin (from ${fwdir#$SRC/})"

    # Link-time only: development headers and static/import libraries.
    find "$destdir" -type d \( -name include -o -name lib -o -name lib64 \) -prune -exec rm -rf {} + 2>/dev/null || true
}
