#!/usr/bin/env bash
#
# Build every release artifact into the unified output folder build-packaging/dist/:
# the IA-64 firmware zip, the self-contained Windows zip, and the Debian/Ubuntu
# .deb for each supported release. Sub-builds are independent; one failing does
# not abort the others.
#
# Usage:   contrib/packaging/build-all.sh
# Env:     JOBS, LTO, KEEP_WORK, ... (see lib-common.sh)

set -uo pipefail
HERE="$(dirname "$(readlink -f "$0")")"

declare -A rc
run() { local name="$1"; shift; "$@"; rc[$name]=$?; }

# Firmware first: the Windows and .deb builds consume its blob.
run firmware "$HERE/build-firmware.sh"
run windows  "$HERE/build-windows.sh"
for w in "$HERE"/deb/ubuntu-*.sh "$HERE"/deb/debian-*.sh; do
    run "$(basename "$w" .sh)" "$w"
done

echo
echo "=== summary ==="
status=0
for name in firmware windows $(for w in "$HERE"/deb/ubuntu-*.sh "$HERE"/deb/debian-*.sh; do basename "$w" .sh; done); do
    if [ "${rc[$name]:-1}" -eq 0 ]; then
        printf '  %-14s OK\n' "$name"
    else
        printf '  %-14s FAILED (exit %s)\n' "$name" "${rc[$name]:-?}"
        status=1
    fi
done
echo "output: $HERE/dist/"
exit "$status"
