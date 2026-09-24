#!/usr/bin/env bash
# Build the qemu-system-ia64 .deb for Debian 13 in an ephemeral podman
# container. Requires build-packaging/firmware/ia64-firmware.bin (build-firmware.sh).
D="$(dirname "$(readlink -f "$0")")"
source "$D/../lib-common.sh"
source "$D/lib-deb.sh"
BASE_IMAGE="${BASE_IMAGE:-docker.io/library/debian:13}"
DISTRO_TAG="debian13"
run_deb_build
