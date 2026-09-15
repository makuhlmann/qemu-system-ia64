#!/usr/bin/env python3
"""Guest-visible EFI, ACPI, SAL, and SMBIOS table validation."""

# SPDX-License-Identifier: GPL-2.0-or-later

import struct
from pathlib import Path

from qemu_test import QemuSystemTest, wait_for_console_pattern

from ia64.console import Ia64FirmwareTest
from ia64.efi_build import app_path
from ia64.media import make_fat_disk


TABLE_CASES = {
    "configuration-tables", "acpi-root-tables", "acpi-table-bounds",
    "acpi-fadt-links-gas", "acpi-topology", "acpi-mcfg",
    "acpi-console-tables", "acpi-aml-crs", "acpi-pci-routing",
    "platform-memory-descriptors", "pci-root-resources",
    "debug-image-info", "sal-smbios-tables",
}


def xsdt_tables(data: bytes, base: int):
    """The tables the XSDT lists, from a RAM dump that starts at base."""
    def table(address):
        offset = address - base
        if offset < 0 or offset + 36 > len(data):
            return None
        length = struct.unpack_from("<I", data, offset + 4)[0]
        body = data[offset:offset + length]
        if len(body) != length or sum(body) & 0xff:
            return None
        return body

    offset = data.find(b"RSD PTR ")
    while offset >= 0:
        rsdp = data[offset:offset + 36]
        if (len(rsdp) == 36 and rsdp[15] >= 2 and sum(rsdp[:20]) & 0xff == 0
                and sum(rsdp) & 0xff == 0):
            xsdt = table(struct.unpack_from("<Q", rsdp, 24)[0])
            if xsdt is not None and xsdt[:4] == b"XSDT":
                tables = {}
                for entry in range(36, len(xsdt) - 7, 8):
                    body = table(struct.unpack_from("<Q", xsdt, entry)[0])
                    if body is not None:
                        tables[body[:4]] = body
                return tables
        offset = data.find(b"RSD PTR ", offset + 8)
    return {}


def enabled_madt_lsapic_ids(madt: bytes):
    ids = []
    offset = 44
    while offset + 2 <= len(madt):
        kind, length = madt[offset], madt[offset + 1]
        if length < 2:
            break
        if kind == 7 and struct.unpack_from("<I", madt, offset + 8)[0] & 1:
            ids.append((madt[offset + 3], madt[offset + 4]))
        offset += length
    return ids


def enabled_srat_processor_ids(srat: bytes):
    ids = []
    offset = 48
    while offset + 2 <= len(srat):
        kind, length = srat[offset], srat[offset + 1]
        if length < 2:
            break
        if kind == 0 and struct.unpack_from("<I", srat, offset + 4)[0] & 1:
            ids.append((srat[offset + 3], srat[offset + 8]))
        offset += length
    return ids


class Ia64PlatformTables(Ia64FirmwareTest):
    def test_runtime_tables(self):
        disk = Path(self.scratch_file("tables.img"))
        debug_log = self.scratch_file("debugcon.log")
        nvram = self.make_nvram()
        make_fat_disk(disk, app_path("tables"))
        # The default machine (ia64-vpc == zx1) advertises MCFG/ECAM and the
        # nested HWP0001 IOC DSDT, so this validates the full table set against
        # the zx1 profile.  The 460gx machine deliberately omits MCFG (SAL
        # config), so the MCFG-requiring table suite does not apply there.
        vm = self.launch_ia64(
            media=disk, smp=4,
            machine_options=f"firmware-console=serial,nvram={nvram}",
            extra_args=("-debug-port", f"file:{debug_log}"))
        result = self.wait_ia64_suite(
            vm, "tables", TABLE_CASES, timeout=35.0)
        self.assertSetEqual(set(result.cases), TABLE_CASES)

    def test_460gx_processor_ids(self):
        # The SDV's two sockets answer to ids 0 and 3: PAL hands SAL that id
        # in GR33, the firmware makes it the LID, and the MADT and SRAT must
        # publish the LIDs the processors checked in with, or an OS sends
        # its wake-up IPI to a processor that does not exist.
        vm = self.launch_ia64(
            machine="460gx", smp=2, memory="1G",
            machine_options="firmware-console=serial,nvram=none")
        wait_for_console_pattern(self, "ACPI Table Checks:", vm=vm)
        dump = Path(self.scratch_file("ram-top.bin"))
        vm.cmd("pmemsave", val=0x3c000000, size=0x4000000,
               filename=str(dump))
        tables = xsdt_tables(dump.read_bytes(), 0x3c000000)
        self.assertIn(b"APIC", tables)
        self.assertIn(b"SRAT", tables)
        self.assertEqual(enabled_madt_lsapic_ids(tables[b"APIC"]),
                         [(0, 0), (3, 0)])
        self.assertEqual(enabled_srat_processor_ids(tables[b"SRAT"]),
                         [(0, 0), (3, 0)])


if __name__ == "__main__":
    QemuSystemTest.main()
