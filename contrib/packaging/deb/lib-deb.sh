# shellcheck shell=bash
#
# Shared logic for the per-release .deb builders. Sourced by the ubuntu-*.sh
# wrappers (which have already sourced ../lib-common.sh for paths + logging).
#
# Each wrapper sets BASE_IMAGE / DISTRO_TAG (and may override DEV_PACKAGES or
# MAINTAINER), then calls run_deb_build. The heavy lifting happens inside the
# container via in-container.sh; nothing here needs the IA-64 toolchain.

# Build dependencies (the -dev packages behind the curated feature set). The
# runtime Depends of the .deb are derived automatically by dpkg-shlibdeps, not
# from this list. These package names are stable across Ubuntu 22.04/24.04/26.04
# (the t64 transition renamed runtime libs, not their -dev packages); a wrapper
# can still override DEV_PACKAGES if a future release diverges.
DEFAULT_DEV_PACKAGES="\
build-essential ninja-build pkg-config flex bison dpkg-dev ca-certificates \
python3 python3-venv python3-pip python3-setuptools python3-wheel \
libglib2.0-dev libpixman-1-dev zlib1g-dev \
libgtk-3-dev libvte-2.91-dev libsdl2-dev \
libpng-dev libgnutls28-dev \
libslirp-dev libcurl4-openssl-dev \
libusb-1.0-0-dev libusbredirhost-dev \
libpulse-dev libasound2-dev \
libzstd-dev libncurses-dev liburing-dev libaio-dev libjpeg-dev libseccomp-dev"

run_deb_build() {
    : "${BASE_IMAGE:?set BASE_IMAGE in the wrapper}"
    : "${DISTRO_TAG:?set DISTRO_TAG in the wrapper}"
    setup_logging "deb-$DISTRO_TAG"
    need_cmd podman
    need_cmd git
    ensure_src
    [ -f "$FWDIR/ia64-firmware.bin" ] \
        || die "firmware missing; run contrib/packaging/build-firmware.sh first"

    local count ver out work
    ver="1:$(pkg_version)"                # 1:11.0.50+makuhlmann<count>
    out="$DISTDIR"; mkdir -p "$out"       # unified output folder
    work="$OUTROOT/.work-deb-$DISTRO_TAG"; rm -rf "$work"; mkdir -p "$work"
    register_cleanup "$work"

    log "deb build: $DISTRO_TAG  image=$BASE_IMAGE  version=$ver  jobs=${JOBS:-$(nproc)}"
    podman run --rm \
        -v "$SRC":/src:ro \
        -v "$FWDIR":/firmware:ro \
        -v "$LIB_DIR/deb/in-container.sh":/in-container.sh:ro \
        -v "$work":/work \
        -v "$out":/out \
        -e QEMU_DEB_VERSION="$ver" \
        -e DISTRO_TAG="$DISTRO_TAG" \
        -e DEV_PACKAGES="${DEV_PACKAGES:-$DEFAULT_DEV_PACKAGES}" \
        -e MAINTAINER="${MAINTAINER:-makuhlmann <19255462+makuhlmann@users.noreply.github.com>}" \
        -e JOBS="${JOBS:-$(nproc)}" \
        -e LTO="${LTO:-1}" \
        -e CC="${CC:-}" \
        -e CXX="${CXX:-}" \
        "$BASE_IMAGE" \
        bash /in-container.sh

    local deb
    deb="$(ls -1t "$out"/qemu-system-ia64_*_"${DISTRO_TAG}"_amd64.deb 2>/dev/null | head -1)"
    [ -n "$deb" ] || die "no .deb produced for $DISTRO_TAG"
    log "package ready: $deb"
    ls -l "$deb"
}
