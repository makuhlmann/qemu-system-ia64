#!/usr/bin/env bash
# Build the qemu-system-ia64 .deb for Ubuntu 26.04 LTS in an ephemeral podman
# container. Requires build-packaging/firmware/ia64-firmware.bin (build-firmware.sh).
# If docker.io lacks the 26.04 tag yet, override with BASE_IMAGE=... .
D="$(dirname "$(readlink -f "$0")")"
source "$D/../lib-common.sh"
source "$D/lib-deb.sh"
BASE_IMAGE="${BASE_IMAGE:-docker.io/library/ubuntu:26.04}"
DISTRO_TAG="ubuntu26.04"
run_deb_build
