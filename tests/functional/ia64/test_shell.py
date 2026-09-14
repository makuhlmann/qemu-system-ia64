#!/usr/bin/env python3
"""IA-64 firmware boot manager and interactive EFI shell tests."""

# SPDX-License-Identifier: GPL-2.0-or-later

from pathlib import Path

from qemu_test import QemuSystemTest, wait_for_console_pattern

from ia64.console import Ia64FirmwareTest
from ia64.efi_build import app_path
from ia64.media import make_fat_disk
from ia64.protocol import open_menu_entry


class Ia64BootShell(Ia64FirmwareTest):
    # 'EFI Shell [Built-in]' is the second boot-menu entry (after 'Removable
    # Media Boot'); the third is the maintenance menu.
    SHELL_BANNER = "EFI Shell version 1.10"

    def _open_shell(self, vm):
        """Open the built-in EFI shell from the boot menu over the serial
        console: re-anchor to the top entry (Up clamps), step down one entry to
        'EFI Shell [Built-in]', and select it.  open_menu_entry() re-sends the
        sequence until the shell banner appears, so a loaded host that drops the
        first keystrokes -- leaving the wait-forever menu sitting idle -- does
        not wedge the test for the full harness timeout."""
        sock = vm.console_socket
        open_menu_entry(
            sock,
            lambda: sock.sendall(b"\x1b[A\x1b[A\x1b[A\x1b[B\r"),
            self.SHELL_BANNER)

    def _command(self, vm, command, expected):
        vm.console_socket.sendall((command + "\r").encode("ascii"))
        return wait_for_console_pattern(self, expected, vm=vm)

    def test_shell_commands_and_persistence(self):
        disk = Path(self.scratch_file("shell.img"))
        nvram = self.make_nvram("shell.nvram")
        make_fat_disk(disk, app_path("smoke"))

        vm = self.launch_ia64(
            name="shell-first", media=disk, boot_timeout=None,
            machine_options=f"firmware-console=serial,nvram={nvram}")
        self._open_shell(vm)
        self._command(vm, "info", "NVRAM backing:  nonvolatile variable store")
        self._command(vm, "map", "fs0:")
        self._command(vm, r"ls fs0:\EFI\BOOT", "BOOTIA64.EFI")
        self._command(vm, "date 2024-02-29", "2024-02-29")
        self._command(vm, "time 12:34:56", "12:34:56")
        self._command(vm, "bootorder Boot0000",
                      "BootOrder saved to NVRAM")
        self._command(vm, "bootnext Boot0000",
                      "BootNext saved to NVRAM")
        self._command(vm, r"cd fs0:\EFI\BOOT", r"fs0:\EFI\BOOT>")
        self._command(vm, "pwd", r"fs0:\EFI\BOOT")
        self._command(vm, "run BOOTIA64.EFI",
                      "IA64TEST suite=smoke status=DONE")
        wait_for_console_pattern(self, r"fs0:\EFI\BOOT>", vm=vm)
        vm.shutdown()

        contents = nvram.read_bytes()
        self.assertIn("BootOrder".encode("utf-16le") + b"\0\0", contents)
        self.assertIn(b"IRT64OFT", contents)

        # A timed auto-boot (1s countdown) runs boot_image_from_boot_order(),
        # which honours BootNext=Boot0000 and -- per the UEFI spec -- deletes
        # it, so the smoke app boots once from BootNext.
        vm = self.launch_ia64(
            name="shell-bootnext", media=disk, boot_timeout=1,
            machine_options=f"firmware-console=serial,nvram={nvram}")
        wait_for_console_pattern(
            self, "IA64TEST suite=smoke status=DONE", vm=vm)
        vm.shutdown()

        # Reopen the shell: the date/time/BootOrder settings persisted, and the
        # one-shot BootNext was consumed by the auto-boot above.
        vm = self.launch_ia64(
            name="shell-verify", media=disk, boot_timeout=None,
            machine_options=f"firmware-console=serial,nvram={nvram}")
        self._open_shell(vm)
        self._command(vm, "date", "2024-02-29")
        # The RTC keeps running from the value set above, so only the date and
        # hour are stable across the intervening reboots.
        self._command(vm, "time", "2024-02-29 12:")
        self._command(vm, "bootorder", "BootOrder: Boot0000")
        self._command(vm, "bootnext", "BootNext is not set")
        vm.shutdown()

    def test_usb_menu_and_device_boot(self):
        # Drive the menu with a USB keyboard (i8042 disabled): open the shell
        # from the menu, then boot a device from the shell.
        disk = Path(self.scratch_file("device-boot.img"))
        make_fat_disk(disk, app_path("smoke"))
        vm = self.launch_ia64(
            name="device-boot", media=disk, boot_timeout=None,
            machine_options="i8042=off,firmware-console=serial,nvram=none")

        def send_nav():
            # Re-anchor to the top entry (Up clamps), then step down once to
            # the shell.  Re-sent until the banner shows, so a dropped key on a
            # loaded host is retried rather than wedging the test.
            for qcode in ("up", "up", "up", "down", "ret"):
                vm.cmd("send-key", keys=[{"type": "qcode", "data": qcode}],
                       hold_time=50)

        open_menu_entry(vm.console_socket, send_nav, self.SHELL_BANNER)
        self._command(vm, "boot fs0:",
                      "IA64TEST suite=smoke status=DONE")


if __name__ == "__main__":
    QemuSystemTest.main()
