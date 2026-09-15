#!/usr/bin/env python3
"""Four-processor firmware rendezvous under multi-threaded TCG."""

# SPDX-License-Identifier: GPL-2.0-or-later

from pathlib import Path

from qemu_test import QemuSystemTest

from ia64.console import Ia64FirmwareTest
from ia64.efi_build import app_path
from ia64.media import make_fat_disk


SMP_CASES = {
    "sal-ap-wake", "sal-per-processor-reentry",
    "four-processor-rendezvous", "repeat-rendezvous",
    "local-tc-shootdown", "global-tc-source-purge",
    "global-tc-remote-purge", "partial-tc-overlap-purge",
    "global-large-tc-source-purge-no-alat",
    "global-large-tc-remote-purge-no-alat",
    "rid-translation-switch", "translation-cache-capacity-churn",
    "translation-remap-word-store",
    "atomic-16-byte-semaphore", "fetchadd4-fetchadd8-contention",
    "cmpxchg4-cmpxchg8-contention", "queued-lock-guarded-word",
    "packed-pfn-halfword-cmpxchg8",
    "xchg8-guarded-word", "cmpxchg4-bit-lock-guarded-word",
    "contended-call-rse-guarded-word",
    "translated-cmpxchg-guarded-word",
    "high-large-page-translated-guarded-word",
    "big-endian-16-byte-semaphore", "vhpt-walker-global-refill", "vhpt-walker-table-remap",
}


class Ia64Smp(Ia64FirmwareTest):
    def test_four_processors_mttcg(self):
        disk = Path(self.scratch_file("smp.img"))
        nvram = self.make_nvram()
        make_fat_disk(disk, app_path("smp"))
        vm = self.launch_ia64(
            media=disk, smp=4, memory="8G",
            machine_options=f"firmware-console=serial,nvram={nvram}",
            extra_args=("-accel", "tcg,thread=multi"))
        result = self.wait_ia64_suite(vm, "smp", SMP_CASES, timeout=60.0)
        self.assertSetEqual(set(result.cases), SMP_CASES)


if __name__ == "__main__":
    QemuSystemTest.main()
