#!/usr/bin/env python3
"""IA-64 firmware storage, partition, and optical boot tests."""

# SPDX-License-Identifier: GPL-2.0-or-later

from pathlib import Path

from qemu_test import QemuSystemTest

from ia64.console import Ia64FirmwareTest
from ia64.efi_build import app_path
from ia64.media import (file_sha256, make_el_torito_iso, make_fat_disk,
                        make_udf_bridge_iso)


COMMON_CASES = {
    "loaded-protocols", "block-media", "media-layout", "bulk-read",
    "block-error-contracts", "disk-read", "simple-filesystem",
    "file-protocol-contracts", "unicode-collation",
}


class Ia64Storage(Ia64FirmwareTest):
    def run_scenario(self, name, media, *, optical=False,
                     machine_options="", drive_args=None, ide_mode=None,
                     required_cases=()):
        media = Path(media)
        before_data = media.read_bytes()
        before = file_sha256(media)
        extra_args = ()
        if ide_mode is not None:
            extra_args = ("-trace", "enable=bmdma_cmd_writeb")
        vm = self.launch_ia64(
            name=name, media=media, optical=optical,
            machine_options=(
                "firmware-console=serial,nvram=none" +
                ("," + machine_options if machine_options else "")),
            drive_args=drive_args, extra_args=extra_args)
        required = set(COMMON_CASES)
        required.add("read-only-media" if optical else "write-read-restore")
        required.update(required_cases)
        try:
            self.wait_ia64_suite(vm, "storage", required, timeout=40.0)
        finally:
            try:
                vm.shutdown()
            finally:
                after = file_sha256(media)
                if after != before:
                    # Preserve the scratch fixture even when a failing guest
                    # transfer only completed part of its write.
                    media.write_bytes(before_data)
                self.assertEqual(
                    before, after,
                    f"{name}: media was not restored exactly")
        if ide_mode is not None:
            trace = vm.get_log() or ""
            if ide_mode == "dma":
                self.assertIn("bmdma_cmd_writeb val: 0x00000009", trace)
                self.assertIn("bmdma_cmd_writeb val: 0x00000001", trace)
            else:
                self.assertNotIn("bmdma_cmd_writeb", trace)

    def run_scsi_layout(self, layout, *, fat32=False, machine_options=""):
        app = app_path("storage")
        suffix = "-fat32" if fat32 else ""
        media = Path(self.scratch_file(f"scsi-{layout}{suffix}.img"))
        extra_files = (() if layout == "whole" or fat32 else
                       ((b"START   EFI", app_path("start-image-child")),))
        make_fat_disk(media, app, layout=layout, fat32=fat32,
                      extra_boot_files=extra_files)
        required = []
        if layout != "whole":
            required.extend(("logical-partition-handle",
                             "partition-driver-contracts"))
            if not fat32:
                required.append("short-form-hard-drive-path")
        if fat32:
            required.append("fat32-filesystem")
        name = f"scsi-{layout}{suffix}"
        if machine_options:
            name += "-" + machine_options.replace("=", "-")
        self.run_scenario(name, media, machine_options=machine_options,
                          required_cases=required)

    # A hand-attached CMD646 goes to slot 0 of the compatibility bus ("pci").
    # It must name that bus: the zx1 machine has a second PCI root behind
    # Mercury, and without bus= the device lands there, on the slot the
    # graphics adapter holds.  ide=off removes the zx1 board's own CMD649,
    # whose channels would otherwise share the "ide.0" bus name.
    CMD646_DEVICE = "cmd646-ide,id=ide,secondary=1,bus=pci,addr=0"

    def run_ide(self, mode):
        app = app_path("storage")
        media = Path(self.scratch_file(f"ide-{mode}.img"))
        make_fat_disk(media, app)
        drive_args = (
            "-drive", f"file={media},format=raw,if=none,id=testdisk",
            "-device", self.CMD646_DEVICE,
            "-device", "ide-hd,drive=testdisk,bus=ide.0,unit=0",
        )
        self.run_scenario(
            f"ide-{mode}", media,
            machine_options=("ide=off,firmware-ide-dma=off"
                             if mode == "pio" else "ide=off"),
            drive_args=drive_args, ide_mode=mode)

    # An empty optical drive at the primary master must not hide a disk or a
    # bootable CD on the secondary channel.
    EMPTY_PRIMARY_CD = ("-drive", "if=ide,index=0,media=cdrom")

    def run_ide_machine(self, *, secondary=False):
        """The ide=on machine option with an auto-attached if=ide disk.

        index 0 lands on the primary master, index 2 on the secondary master,
        exercising both channels of the zx1 board's CMD649.
        """
        app = app_path("storage")
        where = "secondary" if secondary else "primary"
        index = 2 if secondary else 0
        media = Path(self.scratch_file(f"ide-machine-{where}.img"))
        make_fat_disk(media, app)
        drive_args = (
            "-drive", f"file={media},format=raw,if=ide,index={index}",
        ) + (self.EMPTY_PRIMARY_CD if secondary else ())
        self.run_scenario(
            f"ide-machine-{where}", media, machine_options="ide=on",
            drive_args=drive_args)

    def run_ide_optical(self, *, secondary=False, shadow_disk=False):
        """El Torito boot from an IDE ATAPI CD via the ide=on machine option.

        With shadow_disk, a plain data disk sits on the primary master so the
        test also proves a non-bootable disk does not shadow the bootable CD
        elsewhere on the IDE bus (the boot-device selection fix).
        """
        where = "secondary" if secondary else "primary"
        tag = f"ide-eltorito-{where}" + ("-shadowed" if shadow_disk else "")
        media = Path(self.scratch_file(tag + ".iso"))
        make_el_torito_iso(media, app_path("storage"), platform_id=0xEF)
        # CD on the secondary master (index 2) or the primary slave (index 1).
        cd_index = 2 if secondary else 1
        drive_args = [
            "-drive",
            f"file={media},format=raw,if=ide,index={cd_index},"
            "media=cdrom,readonly=on",
        ]
        if shadow_disk:
            disk = Path(self.scratch_file(tag + "-disk.img"))
            make_fat_disk(disk, app_path("storage"))
            drive_args = [
                "-drive", f"file={disk},format=raw,if=ide,index=0",
            ] + drive_args
        elif secondary:
            drive_args.extend(self.EMPTY_PRIMARY_CD)
        self.run_scenario(
            tag, media, optical=True, machine_options="ide=on",
            drive_args=tuple(drive_args))

    def run_ahci(self, layout):
        app = app_path("storage")
        name = "ahci" if layout == "gpt" else f"ahci-{layout}"
        media = Path(self.scratch_file(name + ".img"))
        required = ()
        extra_files = ()
        if layout != "whole":
            required = ("logical-partition-handle",
                        "short-form-hard-drive-path",
                        "partition-driver-contracts")
            extra_files = ((b"START   EFI", app_path("start-image-child")),)
        make_fat_disk(media, app, layout=layout, extra_boot_files=extra_files)
        drive_args = (
            "-drive", f"file={media},format=raw,if=ide,index=0",
        )
        self.run_scenario(
            name, media, drive_args=drive_args,
            machine_options="ahci=on,ide=off", required_cases=required)

    def run_empty_cd(self, transport):
        app = app_path("storage")
        media = Path(self.scratch_file(f"{transport}-empty-cd.img"))
        make_fat_disk(media, app)
        drive_args = [
            "-drive", f"file={media},format=raw,if=scsi,index=0",
        ]
        if transport == "scsi":
            # An empty if=scsi CD at ID 1 joins the disk on whichever adapter
            # holds the SCSI seat; the qdev bus name changes with the adapter.
            drive_args.extend((
                "-drive", "if=scsi,index=1,media=cdrom",
            ))
        elif transport == "ahci":
            drive_args.extend((
                "-device", "ide-cd,bus=ide.0,unit=0",
            ))
        elif transport == "ide":
            drive_args.extend((
                "-device", self.CMD646_DEVICE,
                "-device", "ide-cd,bus=ide.0,unit=0",
            ))
        else:
            raise ValueError(f"unknown empty-media transport: {transport}")
        self.run_scenario(
            f"{transport}-empty-cd", media, drive_args=tuple(drive_args),
            machine_options=("ahci=on,ide=off" if transport == "ahci" else
                             "ide=off" if transport == "ide" else ""),
            required_cases=("empty-removable-media",))

    def run_optical(self, name, builder, *, udf=False):
        media = Path(self.scratch_file(name + ".iso"))
        builder(media)
        self.run_scenario(
            name, media, optical=True,
            required_cases=("udf-filesystem",) if udf else ())

    def test_scsi_whole_disk(self):
        self.run_scsi_layout("whole")

    def test_scsi_gpt_esp(self):
        self.run_scsi_layout("gpt")

    def test_scsi_gpt_fat32(self):
        self.run_scsi_layout("gpt", fat32=True)

    def test_scsi_mbr_fat(self):
        self.run_scsi_layout("mbr")

    def test_scsi_mbr_fallback(self):
        self.run_scsi_layout("mbr-fallback")

    def test_scsi_lsi53c1030(self):
        """Boot from a disk on function 0 of the rx2600's 53C1030."""
        self.run_scsi_layout("gpt", machine_options="scsi=lsi53c1030")

    def test_scsi_lsi53c1030_function1(self):
        """Boot from a disk on the 53C1030's second function.

        Function 1 is a controller of its own with its own bus, and the
        boot path has to name it: Pci(1|1) under rope 1's root.
        """
        app = app_path("storage")
        media = Path(self.scratch_file("scsi-mpt-fn1.img"))
        make_fat_disk(media, app)
        drive_args = (
            "-drive", f"file={media},format=raw,if=none,id=testdisk",
            "-device", "scsi-hd,bus=scsi.1,scsi-id=3,drive=testdisk",
        )
        self.run_scenario("scsi-mpt-fn1", media, drive_args=drive_args,
                          machine_options="scsi=lsi53c1030")

    def test_scsi_lsi_fallback(self):
        """Boot from a disk on an added LSI while the QLogic holds the seat.

        The probe has to fall through to the LSI added with -device rather
        than stopping at the QLogic, which answers but carries no device.
        """
        app = app_path("storage")
        media = Path(self.scratch_file("scsi-lsi.img"))
        make_fat_disk(media, app)
        drive_args = (
            "-device", "lsi53c895a,id=lsi",
            "-drive", f"file={media},format=raw,if=none,id=testdisk",
            "-device", "scsi-hd,bus=lsi.0,drive=testdisk",
        )
        self.run_scenario("scsi-lsi", media, drive_args=drive_args,
                          machine_options="scsi=isp12160")

    def test_cmd646_ide_dma(self):
        self.run_ide("dma")

    def test_cmd646_ide_pio(self):
        self.run_ide("pio")

    def test_ide_machine_option(self):
        self.run_ide_machine()

    def test_ide_machine_secondary_channel(self):
        self.run_ide_machine(secondary=True)

    def test_ide_el_torito_boot(self):
        self.run_ide_optical(shadow_disk=True)

    def test_ide_el_torito_boot_secondary(self):
        self.run_ide_optical(secondary=True)

    def test_ahci(self):
        self.run_ahci("gpt")

    def test_ahci_whole_disk(self):
        self.run_ahci("whole")

    def test_scsi_empty_cd(self):
        self.run_empty_cd("scsi")

    def test_ahci_empty_cd(self):
        self.run_empty_cd("ahci")

    def test_ide_empty_cd(self):
        self.run_empty_cd("ide")

    def test_el_torito_efi_platform(self):
        self.run_optical(
            "eltorito-efi",
            lambda path: make_el_torito_iso(
                path, app_path("storage"), platform_id=0xEF))

    def test_el_torito_legacy_platform(self):
        self.run_optical(
            "eltorito-legacy",
            lambda path: make_el_torito_iso(
                path, app_path("storage"), platform_id=0))

    def test_udf_bridge_filesystem(self):
        self.run_optical(
            "udf-bridge",
            lambda path: make_udf_bridge_iso(path, app_path("storage")),
            udf=True)


if __name__ == "__main__":
    QemuSystemTest.main()
