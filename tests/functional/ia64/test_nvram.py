#!/usr/bin/env python3
"""IA-64 firmware: a variable store it cannot read is kept, not reformatted."""

# SPDX-License-Identifier: GPL-2.0-or-later

import struct
from pathlib import Path

from qemu_test import QemuSystemTest, wait_for_console_pattern

from ia64.console import (Ia64FirmwareTest, PDH_STORE_SIZE,
                          PDH_STORE_VARS_BASE, PDH_STORE_VARS_OFFSET)
from ia64.protocol import open_menu_entry


NVRAM_SIZE = 0x10000
STORE_AT = "0x%08X" % PDH_STORE_VARS_BASE
STORE_MAGIC = int.from_bytes(b"IVARSTOR", "little")
EFI_GLOBAL_VARIABLE = bytes((0x61, 0xdf, 0xe4, 0x8b, 0xca, 0x93, 0xd2, 0x11,
                             0xaa, 0x0d, 0x00, 0xe0, 0x98, 0x03, 0x2b, 0x8c))
NV_BS_RT = 0x7
PROMPT = "Reset the NVRAM variable store? All old contents are lost. [y/N]"
KEPT = "NVRAM: the variable store stays write-protected for this boot."
RESET = "NVRAM: the variable store was reset."


def store_slot(name: str, data: bytes, valid: int = 1) -> bytes:
    """One 1192-byte slot of the firmware's IVARSTOR variable store."""
    encoded = (name + "\0").encode("utf-16le")
    return (encoded.ljust(128, b"\0") + struct.pack("<Q", len(encoded)) +
            EFI_GLOBAL_VARIABLE + data.ljust(1024, b"\0") +
            struct.pack("<QIBB2x", len(data), NV_BS_RT, valid, 0))


def foreign_store() -> bytes:
    """A sector holding another firmware's variable store header."""
    return (b"vbrl\x01\x00\x00\xff").ljust(NVRAM_SIZE, b"\xff")


def damaged_store() -> bytes:
    """A valid BootOrder slot next to a slot with a torn 'valid' byte."""
    store = (struct.pack("<QII", STORE_MAGIC, 1, 2) +
             store_slot("BootOrder", struct.pack("<HH", 0x0000, 0x00FF)) +
             store_slot("Timeout", struct.pack("<H", 5), valid=0xFF))
    return store.ljust(NVRAM_SIZE, b"\0")


class Ia64NvramProtection(Ia64FirmwareTest):
    SHELL_BANNER = "EFI Shell version 1.10"

    def _nvram_file(self, name: str, sector: bytes) -> Path:
        store = bytearray(PDH_STORE_SIZE)
        store[PDH_STORE_VARS_OFFSET:PDH_STORE_VARS_OFFSET + NVRAM_SIZE] = sector
        path = Path(self.scratch_file(name))
        path.write_bytes(bytes(store))
        return path

    def _sector(self, path: Path) -> bytes:
        contents = path.read_bytes()
        start = PDH_STORE_VARS_OFFSET
        return contents[start:start + NVRAM_SIZE]

    def _open_shell(self, vm):
        sock = vm.console_socket
        open_menu_entry(
            sock,
            lambda: sock.sendall(b"\x1b[A\x1b[A\x1b[A\x1b[B\r"),
            self.SHELL_BANNER)

    def _command(self, vm, command, expected):
        vm.console_socket.sendall((command + "\r").encode("ascii"))
        return wait_for_console_pattern(self, expected, vm=vm)

    def test_damaged_store_kept_on_no(self):
        sector = damaged_store()
        nvram = self._nvram_file("damaged.nvram", sector)
        vm = self.launch_ia64(
            name="damaged", boot_timeout=None,
            machine_options=f"firmware-console=serial,nvram={nvram}")
        wait_for_console_pattern(self, "variable store at %s is damaged"
                                 % STORE_AT, vm=vm)
        wait_for_console_pattern(self, PROMPT, vm=vm)
        vm.console_socket.sendall(b"n")
        wait_for_console_pattern(self, KEPT, vm=vm)
        self._open_shell(vm)
        # The slot that passes the checks is still read.
        self._command(vm, "bootorder", "BootOrder: Boot0000 Boot00FF")
        self._command(vm, "info", "NVRAM backing:  write-protected")
        self._command(vm, "bootorder Boot0000",
                      "NVRAM is write-protected; not saved.")
        vm.shutdown()
        self.assertEqual(self._sector(nvram), sector)

    def test_foreign_store_kept_on_timeout(self):
        sector = foreign_store()
        nvram = self._nvram_file("foreign-timeout.nvram", sector)
        vm = self.launch_ia64(
            name="foreign-timeout",
            machine_options=f"firmware-console=serial,nvram={nvram}")
        wait_for_console_pattern(self, "variable store at %s is not "
                                 "recognized" % STORE_AT, vm=vm)
        wait_for_console_pattern(self, "76 62 72 6C 01 00 00 FF", vm=vm)
        wait_for_console_pattern(self, PROMPT, vm=vm)
        wait_for_console_pattern(self, "(timed out)", vm=vm)
        wait_for_console_pattern(self, KEPT, vm=vm)
        vm.shutdown()
        self.assertEqual(self._sector(nvram), sector)

    def test_foreign_store_reset_on_yes(self):
        nvram = self._nvram_file("foreign-reset.nvram", foreign_store())
        vm = self.launch_ia64(
            name="foreign-reset", boot_timeout=None,
            machine_options=f"firmware-console=serial,nvram={nvram}")
        wait_for_console_pattern(self, PROMPT, vm=vm)
        vm.console_socket.sendall(b"y")
        wait_for_console_pattern(self, RESET, vm=vm)
        self._open_shell(vm)
        self._command(vm, "info",
                      "NVRAM backing:  nonvolatile variable store")
        self._command(vm, "bootorder Boot0000", "BootOrder saved to NVRAM.")
        vm.shutdown()
        sector = self._sector(nvram)
        self.assertEqual(sector[:8], b"IVARSTOR")
        self.assertIn("BootOrder".encode("utf-16le") + b"\0\0", sector)


if __name__ == "__main__":
    QemuSystemTest.main()
