#!/usr/bin/env bash
# Build the qemu-system-ia64 .deb for Debian 11 (bullseye) in an ephemeral podman
# container. Requires build-packaging/firmware/ia64-firmware.bin (build-firmware.sh).
#
# Special case: bullseye's gcc is 10.2, below QEMU 11's required gcc 10.4, so this
# build uses clang instead (bullseye's clang-11 meets QEMU's clang floor) and
# disables LTO (clang-11 LTO would need extra linker plumbing). gnutls 3.7.1 is
# also auto-dropped by the version probe. Override with CC=/LTO= if desired.
D="$(dirname "$(readlink -f "$0")")"
source "$D/../lib-common.sh"
source "$D/lib-deb.sh"
BASE_IMAGE="${BASE_IMAGE:-docker.io/library/debian:11}"
DISTRO_TAG="debian11"
DEV_PACKAGES="$DEFAULT_DEV_PACKAGES clang"
export CC="${CC:-clang}" CXX="${CXX:-clang++}"
LTO=0   # clang-11 LTO needs extra linker plumbing; keep it off for bullseye
run_deb_build
