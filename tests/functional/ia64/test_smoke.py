#!/usr/bin/env python3
"""IA-64 firmware and EFI application smoke tests."""

# SPDX-License-Identifier: GPL-2.0-or-later

from pathlib import Path

from qemu_test import QemuSystemTest

from ia64.console import Ia64FirmwareTest
from ia64.efi_build import app_path
from ia64.media import make_el_torito_iso, make_fat_disk


SMOKE_CASES = {
    "entry", "system-table", "loaded-image", "device-path",
    "console-output",
}


class Ia64FirmwareSmoke(Ia64FirmwareTest):
    def make_disk(self, name: str = "smoke.img") -> Path:
        path = Path(self.scratch_file(name))
        make_fat_disk(path, app_path("smoke"))
        return path

    def assert_post_self_tests_pass(self, console: str) -> None:
        # The firmware checks its own protocols during POST and prints one
        # line per check; nothing else reads those lines.
        failed = [line.strip() for line in console.replace("\r", "").splitlines()
                  if "verification failed" in line or
                  "installation failed" in line]
        self.assertEqual(failed, [])

    def test_cold_boot_and_reset(self):
        vm = self.launch_ia64(
            media=self.make_disk(),
            machine_options="firmware-console=serial,nvram=none")
        result = self.wait_ia64_suite(vm, "smoke", SMOKE_CASES)
        self.assert_post_self_tests_pass(result.raw_console)
        vm.cmd("system_reset")
        self.wait_ia64_suite(vm, "smoke", SMOKE_CASES)

    def test_460gx_cold_boot(self):
        vm = self.launch_ia64(
            machine="460gx", media=self.make_disk("460gx.img"),
            machine_options="firmware-console=serial,nvram=none")
        result = self.wait_ia64_suite(vm, "smoke", SMOKE_CASES)
        self.assert_post_self_tests_pass(result.raw_console)

    def test_slow_console_reader(self):
        # The serial backend refuses bytes while its reader is behind, and
        # QEMU's 16550 then holds THRE clear; firmware that writes THR without
        # waiting overwrites the pending byte and loses output (this flaked
        # func-ia64-smp under gate load).  Read slowly and require all of it.
        vm = self.launch_ia64(
            media=self.make_disk("slow-reader.img"),
            machine_options="firmware-console=serial,nvram=none")
        self.wait_ia64_suite(vm, "smoke", SMOKE_CASES, read_pause=0.05)

    def test_icount_boot(self):
        vm = self.launch_ia64(
            media=self.make_disk("icount.img"),
            machine_options="firmware-console=serial,nvram=none",
            extra_args=("-icount", "shift=3"))
        self.wait_ia64_suite(vm, "smoke", SMOKE_CASES, timeout=40.0)

    def test_el_torito_efi_platform(self):
        path = Path(self.scratch_file("efi-platform.iso"))
        make_el_torito_iso(path, app_path("smoke"), platform_id=0xEF)
        vm = self.launch_ia64(
            media=path, optical=True,
            machine_options="firmware-console=serial,nvram=none")
        self.wait_ia64_suite(vm, "smoke", SMOKE_CASES)

    def test_el_torito_legacy_platform(self):
        path = Path(self.scratch_file("legacy-platform.iso"))
        make_el_torito_iso(path, app_path("smoke"), platform_id=0)
        vm = self.launch_ia64(
            media=path, optical=True,
            machine_options="firmware-console=serial,nvram=none")
        self.wait_ia64_suite(vm, "smoke", SMOKE_CASES)


if __name__ == "__main__":
    QemuSystemTest.main()
