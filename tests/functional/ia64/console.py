"""QEMU launch helpers for IA-64 firmware functional tests."""

# SPDX-License-Identifier: GPL-2.0-or-later

import logging
from pathlib import Path

from qemu_test import QemuSystemTest

from ia64.efi_build import firmware_path
from ia64.protocol import ProtocolError, wait_for_suite


SOURCE_ROOT = Path(__file__).resolve().parents[3]

# The zx1 board's nvram= file images its battery-backed SRAM, and the
# firmware's own variable store sits at a fixed offset in it.
PDH_STORE_SIZE = 0x80000
PDH_STORE_VARS_OFFSET = 0x20000
PDH_STORE_VARS_BASE = 0xFF420000


class Ia64FirmwareTest(QemuSystemTest):
    """Base class that boots the in-tree IA-64 firmware and test media."""

    def make_nvram(self, name: str = "nvram.bin") -> Path:
        """A blank store for the default board, which keeps it in the PDH."""
        path = Path(self.scratch_file(name))
        path.write_bytes(bytes(PDH_STORE_SIZE))
        return path

    def launch_ia64(self, *, name: str = "default", media: Path | None = None,
                    optical: bool = False, machine: str = "ia64-vpc",
                    machine_options: str = "",
                    memory: str = "512M", smp: int = 1,
                    boot_timeout: int | None = 1,
                    extra_args: tuple[str, ...] = (),
                    drive_args: tuple[str, ...] | None = None):
        vm = self.get_vm(name=name)
        if machine_options:
            machine += "," + machine_options
        # By default give the boot manager a short auto-boot countdown
        # (firmware-boot-timeout=1s) so a test's media boots without any
        # interactive menu selection -- the wait-forever default (0xFFFF) would
        # require driving the menu over serial, which races under load.  A 1s
        # countdown (not 0/immediate) uses the same clear-screen + BootOrder
        # path as a real timed auto-boot, so the OS-handoff state matches.  A
        # test that needs the menu (e.g. to reach the shell) passes
        # boot_timeout=None to keep the machine default.
        if boot_timeout is not None:
            machine += f",firmware-boot-timeout={boot_timeout}"
        vm.set_machine(machine)
        vm.set_console()
        # QEMUMachine starts with ``-vga none`` for generic headless tests.
        # Keep the display backend headless, but restore this machine's
        # guest-visible default adapter so firmware graphics and PCI paths
        # are exercised.
        vm.add_args("-vga", "ati", "-smp", str(smp), "-m", memory,
                    "-bios", str(firmware_path()),
                    "-monitor", "none", "-L", str(SOURCE_ROOT / "pc-bios"))
        if drive_args is not None:
            vm.add_args(*drive_args)
        elif media is not None:
            drive = f"file={media},format=raw"
            if optical:
                drive += ",media=cdrom,readonly=on"
            vm.add_args("-drive", drive)
        if extra_args:
            vm.add_args(*extra_args)
        vm.launch()
        return vm

    def log_console(self, raw_console: str) -> None:
        logger = logging.getLogger("console")
        for line in raw_console.replace("\r", "").splitlines():
            logger.debug(line)

    def wait_ia64_suite(self, vm, suite: str, required_cases,
                        timeout: float = 25.0, on_case=None,
                        read_pause: float = 0.0):
        # launch_ia64() sets a short firmware-boot-timeout, so the medium
        # auto-boots; no menu interaction is needed here.
        try:
            result = wait_for_suite(
                vm.console_socket, suite, required_cases, timeout,
                on_case=on_case, process_alive=vm.is_running,
                read_pause=read_pause)
        except ProtocolError as error:
            self.log_console(getattr(error, "raw_console", ""))
            raise
        self.log_console(result.raw_console)
        # Deliberately no liveness assertion here.  wait_for_suite() already
        # fails if the process exits before the suite completes, and it
        # validates every required case plus DONE, so the result is known
        # good by this point.  Whether the guest is still running afterwards
        # is incidental -- it has returned from StartImage and the firmware
        # may finish shutting down first -- and asserting on it made the
        # functional tests fail intermittently.
        return result
