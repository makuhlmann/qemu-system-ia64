#!/usr/bin/env bash
# Build the qemu-system-ia64 .deb for Ubuntu 22.04 LTS (jammy) in an ephemeral
# podman container. Requires build-packaging/firmware/ia64-firmware.bin (build-firmware.sh).
D="$(dirname "$(readlink -f "$0")")"
source "$D/../lib-common.sh"
source "$D/lib-deb.sh"
BASE_IMAGE="docker.io/library/ubuntu:22.04"
DISTRO_TAG="ubuntu22.04"
run_deb_build
