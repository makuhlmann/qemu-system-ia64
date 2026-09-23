#!/usr/bin/env python3
"""Host-only checks for the IA-64 functional-test infrastructure."""

# SPDX-License-Identifier: GPL-2.0-or-later

import importlib.util
from pathlib import Path
import struct
import tempfile
import unittest
import zlib

from qemu_test import QemuSystemTest

from ia64 import media as media_format
from ia64.media import (make_el_torito_iso, make_fat_disk,
                        make_udf_bridge_iso)
from ia64.protocol import ProtocolError, ProtocolParser


class Ia64HostInfrastructure(unittest.TestCase):
    def assert_iso9660(self, path, platform_id, catalog_sector_count):
        image = Path(path).read_bytes()
        block = media_format.ISO_SECTOR_SIZE
        self.assertEqual(len(image) % block, 0)

        def sector(lba):
            return image[lba * block:(lba + 1) * block]

        pvd = sector(16)
        self.assertEqual(pvd[:7], b"\x01CD001\x01")
        volume_blocks = struct.unpack_from("<I", pvd, 80)[0]
        self.assertEqual(volume_blocks, struct.unpack_from(">I", pvd, 84)[0])
        self.assertEqual(volume_blocks, len(image) // block)
        for offset, expected in ((120, 1), (124, 1), (128, block)):
            self.assertEqual(struct.unpack_from("<H", pvd, offset)[0],
                             expected)
            self.assertEqual(struct.unpack_from(">H", pvd, offset + 2)[0],
                             expected)
        path_size = struct.unpack_from("<I", pvd, 132)[0]
        self.assertEqual(path_size, struct.unpack_from(">I", pvd, 136)[0])
        self.assertEqual(path_size, 10)
        little_path_lba = struct.unpack_from("<I", pvd, 140)[0]
        big_path_lba = struct.unpack_from(">I", pvd, 148)[0]
        root_record = pvd[156:190]
        self.assertEqual(root_record[0], 34)
        root_lba = struct.unpack_from("<I", root_record, 2)[0]
        self.assertEqual(root_lba, struct.unpack_from(">I", root_record, 6)[0])
        self.assertEqual(root_record[25], 2)
        self.assertEqual(root_record[32:34], b"\x01\x00")
        self.assertEqual(sector(little_path_lba)[:10],
                         b"\x01\x00" + struct.pack("<IH", root_lba, 1) +
                         b"\x00\x00")
        self.assertEqual(sector(big_path_lba)[:10],
                         b"\x01\x00" + struct.pack(">IH", root_lba, 1) +
                         b"\x00\x00")
        root = sector(root_lba)
        self.assertEqual(root[32:34], b"\x01\x00")
        second = root[root[0]:]
        self.assertEqual(second[32:34], b"\x01\x01")

        boot_record = sector(17)
        self.assertEqual(boot_record[:7], b"\x00CD001\x01")
        self.assertEqual(boot_record[7:30], b"EL TORITO SPECIFICATION")
        catalog_lba = struct.unpack_from("<I", boot_record, 71)[0]
        catalog = sector(catalog_lba)
        self.assertEqual(catalog[0], 1)
        self.assertEqual(catalog[1], platform_id)
        self.assertEqual(catalog[30:32], b"\x55\xaa")
        self.assertEqual(sum(struct.unpack_from("<16H", catalog)) & 0xffff, 0)
        self.assertEqual(catalog[32], 0x88)
        self.assertEqual(struct.unpack_from("<H", catalog, 38)[0],
                         catalog_sector_count)
        boot_lba = struct.unpack_from("<I", catalog, 40)[0]
        boot = sector(boot_lba)
        self.assertEqual(boot[3:11], b"QEMUIA64")
        self.assertEqual(boot[510:512], b"\x55\xaa")

    def assert_fat_volume(self, image, start, app, *, expected_sectors,
                          expected_hidden, test_offset=None):
        volume = memoryview(image)[start:]
        self.assertEqual(bytes(volume[3:11]), b"QEMUIA64")
        self.assertEqual(struct.unpack_from("<H", volume, 11)[0], 512)
        self.assertEqual(volume[13], 1)
        total_sectors = (struct.unpack_from("<H", volume, 19)[0] or
                         struct.unpack_from("<I", volume, 32)[0])
        self.assertEqual(total_sectors, expected_sectors)
        self.assertEqual(struct.unpack_from("<I", volume, 28)[0],
                         expected_hidden)
        fat_sectors = struct.unpack_from("<H", volume, 22)[0]
        root_entries = struct.unpack_from("<H", volume, 17)[0]
        root_sectors = (root_entries * 32 + 511) // 512
        data_start = 1 + fat_sectors + root_sectors
        clusters = total_sectors - data_start
        expected_type = b"FAT12   " if clusters < 4085 else b"FAT16   "
        self.assertEqual(bytes(volume[54:62]), expected_type)

        root = volume[(1 + fat_sectors) * 512:]
        self.assertEqual(bytes(root[0:11]), b"IA64 TEST  ")
        self.assertEqual(root[11], 0x08)
        self.assertEqual(bytes(root[32:43]), b"EFI        ")
        self.assertEqual(root[43], 0x10)

        def cluster(number):
            offset = (data_start + number - 2) * 512
            return volume[offset:offset + 512]

        efi_dir = cluster(2)
        self.assertEqual(bytes(efi_dir[0:11]), b".          ")
        self.assertEqual(bytes(efi_dir[32:43]), b"..         ")
        self.assertEqual(bytes(efi_dir[64:75]), b"BOOT       ")
        boot_dir = cluster(3)
        self.assertEqual(bytes(boot_dir[0:11]), b".          ")
        self.assertEqual(bytes(boot_dir[32:43]), b"..         ")
        self.assertEqual(bytes(boot_dir[64:75]), b"BOOTIA64EFI")
        self.assertEqual(struct.unpack_from("<I", boot_dir, 64 + 28)[0],
                         len(app))
        first_file_cluster = struct.unpack_from("<H", boot_dir, 64 + 26)[0]
        file_offset = (data_start + first_file_cluster - 2) * 512
        self.assertEqual(bytes(volume[file_offset:file_offset + len(app)]), app)

        if test_offset is not None:
            expected = bytes((index * 19 + 0x31) & 0xff
                             for index in range(
                                 media_format.TEST_BLOCKS * 512))
            self.assertEqual(image[test_offset:test_offset + len(expected)],
                             expected)

    def assert_gpt(self, image):
        block = media_format.SECTOR_SIZE

        def valid_header(lba):
            header = bytearray(image[lba * block:(lba + 1) * block])
            self.assertEqual(header[:8], b"EFI PART")
            header_size = struct.unpack_from("<I", header, 12)[0]
            expected_crc = struct.unpack_from("<I", header, 16)[0]
            struct.pack_into("<I", header, 16, 0)
            self.assertEqual(zlib.crc32(header[:header_size]) & 0xffffffff,
                             expected_crc)
            return header

        primary = valid_header(1)
        backup_lba = struct.unpack_from("<Q", primary, 32)[0]
        backup = valid_header(backup_lba)
        self.assertEqual(struct.unpack_from("<Q", backup, 32)[0], 1)
        entry_lba, count, size, expected_crc = struct.unpack_from(
            "<QIII", primary, 72)
        entries = image[entry_lba * block:entry_lba * block + count * size]
        self.assertEqual(zlib.crc32(entries) & 0xffffffff, expected_crc)
        self.assertEqual(entries[:16], bytes.fromhex(
            "28732ac11ff8d211ba4b00a0c93ec93b"))
        first, last = struct.unpack_from("<QQ", entries, 32)
        self.assertEqual((first, last),
                         (media_format.GPT_ESP_START,
                          media_format.GPT_ESP_START +
                          media_format.DISK_SECTORS - 1))

    def assert_udf_tag(self, image, lba, expected_tag, *, location=None):
        block = media_format.ISO_SECTOR_SIZE
        descriptor = image[lba * block:(lba + 1) * block]
        self.assertEqual(struct.unpack_from("<H", descriptor)[0], expected_tag)
        self.assertEqual(struct.unpack_from("<H", descriptor, 2)[0], 3)
        checksum = sum(descriptor[index] for index in range(16)
                       if index != 4) & 0xff
        self.assertEqual(descriptor[4], checksum)
        self.assertEqual(descriptor[5], 0)
        self.assertEqual(struct.unpack_from("<I", descriptor, 12)[0],
                         lba if location is None else location)
        crc_length = struct.unpack_from("<H", descriptor, 10)[0]
        self.assertLessEqual(crc_length, block - 16)
        self.assertEqual(struct.unpack_from("<H", descriptor, 8)[0],
                         media_format.crc16_ccitt(
                             descriptor[16:16 + crc_length]))
        return descriptor

    def assert_udf(self, path, app):
        image = Path(path).read_bytes()
        block = media_format.ISO_SECTOR_SIZE
        self.assertEqual(image[19 * block + 1:19 * block + 7], b"BEA01\x01")
        self.assertEqual(image[20 * block + 1:20 * block + 7], b"NSR03\x01")
        self.assertEqual(image[21 * block + 1:21 * block + 7], b"TEA01\x01")

        for anchor in (256, media_format.UDF_TOTAL_SECTORS - 257,
                       media_format.UDF_TOTAL_SECTORS - 1):
            descriptor = self.assert_udf_tag(image, anchor, 2)
            self.assertEqual(struct.unpack_from("<II", descriptor, 16),
                             (6 * block, media_format.UDF_MAIN_VDS_LBA))
            self.assertEqual(struct.unpack_from("<II", descriptor, 24),
                             (6 * block, media_format.UDF_RESERVE_VDS_LBA))

        tags = (1, 4, 5, 6, 7, 8)
        for index, tag in enumerate(tags):
            self.assert_udf_tag(image, media_format.UDF_MAIN_VDS_LBA + index,
                                tag)
            self.assert_udf_tag(
                image, media_format.UDF_RESERVE_VDS_LBA + index, tag)

        logical = image[(media_format.UDF_MAIN_VDS_LBA + 3) * block:]
        self.assertEqual(struct.unpack_from("<I", logical, 212)[0], block)
        self.assertEqual(struct.unpack_from("<II", logical, 264), (6, 1))
        self.assertEqual(struct.unpack_from("<II", logical, 432),
                         (block, media_format.UDF_INTEGRITY_LBA))
        self.assertEqual(logical[440:446],
                         b"\x01\x06\x01\x00" +
                         struct.pack("<H", media_format.UDF_PARTITION_NUMBER))

        partition = image[(media_format.UDF_MAIN_VDS_LBA + 2) * block:]
        self.assertEqual(struct.unpack_from("<H", partition, 20)[0], 1)
        self.assertEqual(struct.unpack_from("<I", partition, 184)[0], 1)
        self.assertEqual(struct.unpack_from("<II", partition, 188),
                         (media_format.UDF_PARTITION_START,
                          media_format.UDF_PARTITION_LENGTH))

        integrity = self.assert_udf_tag(
            image, media_format.UDF_INTEGRITY_LBA, 9)
        self.assertEqual(struct.unpack_from("<I", integrity, 28)[0], 1)
        self.assertEqual(struct.unpack_from("<II", integrity, 72), (1, 46))
        self.assertEqual(struct.unpack_from("<II", integrity, 120), (1, 3))
        self.assertEqual(struct.unpack_from("<HHH", integrity, 128),
                         (0x0201, 0x0201, 0x0201))

        partition_start = media_format.UDF_PARTITION_START
        file_set = self.assert_udf_tag(image, partition_start, 256,
                                       location=0)
        self.assertEqual(struct.unpack_from("<IIH", file_set, 400),
                         (block, media_format.UDF_ROOT_ICB, 0))

        for icb, file_type, allocation_type, blocks in (
                (media_format.UDF_ROOT_ICB, 4, 3, 0),
                (media_format.UDF_EFI_ICB, 4, 3, 0),
                (media_format.UDF_BOOT_ICB, 4, 3, 0),
                (media_format.UDF_APP_ICB, 5, 0,
                 (len(app) + block - 1) // block)):
            entry = self.assert_udf_tag(image, partition_start + icb, 261,
                                        location=icb)
            self.assertEqual(struct.unpack_from("<H", entry, 24)[0], 1)
            self.assertEqual(entry[27], file_type)
            self.assertEqual(struct.unpack_from("<H", entry, 34)[0],
                             allocation_type)
            self.assertEqual(struct.unpack_from("<H", entry, 48)[0], 1)
            self.assertEqual(struct.unpack_from("<Q", entry, 64)[0], blocks)
            self.assertEqual(struct.unpack_from("<I", entry, 108)[0], 1)
            self.assertEqual(struct.unpack_from("<I", entry, 112)[0], 0)

        app_entry = image[(partition_start + media_format.UDF_APP_ICB) * block:]
        self.assertEqual(struct.unpack_from("<Q", app_entry, 56)[0], len(app))
        self.assertEqual(struct.unpack_from("<I", app_entry, 172)[0], 8)
        extent_length, extent_block = struct.unpack_from("<II", app_entry, 176)
        self.assertEqual(extent_length, len(app))
        self.assertEqual(extent_block, media_format.UDF_APP_DATA)
        data_offset = (partition_start + extent_block) * block
        self.assertEqual(image[data_offset:data_offset + len(app)], app)

        boot_entry = image[(partition_start + media_format.UDF_BOOT_ICB) * block:]
        allocation_length = struct.unpack_from("<I", boot_entry, 172)[0]
        allocation = boot_entry[176:176 + allocation_length]
        cursor = 0
        found_app = False
        while cursor < len(allocation):
            implementation_length = struct.unpack_from(
                "<H", allocation, cursor + 36)[0]
            name_length = allocation[cursor + 19]
            total = (38 + implementation_length + name_length + 3) & ~3
            fid = allocation[cursor:cursor + total]
            self.assertEqual(struct.unpack_from("<H", fid)[0], 257)
            self.assertEqual(struct.unpack_from("<I", fid, 20)[0], block)
            if name_length:
                encoded = fid[38 + implementation_length:
                              38 + implementation_length + name_length]
                if encoded == b"\x08BOOTIA64.EFI":
                    found_app = True
                    self.assertEqual(struct.unpack_from("<I", fid, 24)[0],
                                     media_format.UDF_APP_ICB)
            cursor += total
        self.assertTrue(found_app)

    def test_protocol_accepts_complete_suite(self):
        parser = ProtocolParser("host")
        # Firmware diagnostics, CRLF, and arbitrary stream boundaries must
        # not make the structured parser lose or duplicate a record.
        for chunk in (
                "firmware diagnostic\r\nIA64TEST suite=host ca",
                "se=one status=PASS\r",
                "\nIA64TEST suite=host case=two status=PASS\nIA64",
                "TEST suite=host status=DONE passed=2 failed=0\n"):
            parser.feed(chunk)
        parser.result.assert_valid({"one", "two"})

    def test_protocol_rejects_wrong_suite_and_post_done_case(self):
        with self.assertRaisesRegex(ProtocolError, "expected suite"):
            ProtocolParser("host").feed(
                "IA64TEST suite=other case=one status=PASS\n")

        parser = ProtocolParser("host")
        parser.feed("IA64TEST suite=host status=DONE passed=0 failed=0\n")
        with self.assertRaisesRegex(ProtocolError, "after DONE"):
            parser.feed("IA64TEST suite=host case=late status=PASS\n")

    def test_protocol_rejects_duplicate_case_and_done(self):
        parser = ProtocolParser("host")
        parser.feed("IA64TEST suite=host case=one status=PASS\n")
        with self.assertRaisesRegex(ProtocolError, "duplicate case"):
            parser.feed("IA64TEST suite=host case=one status=PASS\n")

        parser = ProtocolParser("host")
        parser.feed("IA64TEST suite=host status=DONE passed=0 failed=0\n")
        with self.assertRaisesRegex(ProtocolError, "duplicate DONE"):
            parser.feed(
                "IA64TEST suite=host status=DONE passed=0 failed=0\n")

    def test_protocol_requires_failure_fields(self):
        for line, expected_error in (
                ("IA64TEST suite=host case=bad status=FAIL\n",
                 "omits code/detail"),
                ("IA64TEST suite=host case=bad status=FAIL code=7\n",
                 "malformed"),
                ("IA64TEST suite=host case=bad status=FAIL detail=bad\n",
                 "malformed"),
                ("IA64TEST suite=host case=bad status=PASS "
                 "code=7 detail=bad\n", "PASS line contains")):
            with self.subTest(line=line):
                with self.assertRaisesRegex(ProtocolError, expected_error):
                    ProtocolParser("host").feed(line)

    def test_protocol_rejects_done_count_mismatch_and_missing_case(self):
        parser = ProtocolParser("host")
        parser.feed(
            "IA64TEST suite=host case=one status=PASS\n"
            "IA64TEST suite=host status=DONE passed=0 failed=0\n")
        with self.assertRaisesRegex(ProtocolError, "do not match"):
            parser.result.assert_valid({"one"})

        parser = ProtocolParser("host")
        parser.feed(
            "IA64TEST suite=host case=one status=PASS\n"
            "IA64TEST suite=host status=DONE passed=1 failed=0\n")
        with self.assertRaisesRegex(ProtocolError, "omitted cases"):
            parser.result.assert_valid({"one", "two"})

    def test_media_builders_are_deterministic(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            app = directory / "BOOTIA64.EFI"
            app.write_bytes(bytes((index * 29 + 7) & 0xff
                                  for index in range(8193)))

            for layout in ("whole", "gpt", "mbr", "mbr-fallback"):
                with self.subTest(builder="fat", layout=layout):
                    first = make_fat_disk(
                        directory / f"first-{layout}.img", app, layout)
                    second = make_fat_disk(
                        directory / f"second-{layout}.img", app, layout)
                    self.assertEqual(first.sha256, second.sha256)
                    self.assertEqual(first.test_offset, second.test_offset)
                    self.assertEqual(first.test_data, second.test_data)
                    self.assertEqual(first.path.read_bytes(),
                                     second.path.read_bytes())

            for platform_id in (0xEF, 0):
                with self.subTest(builder="eltorito",
                                  platform_id=platform_id):
                    first = make_el_torito_iso(
                        directory / f"first-{platform_id}.iso", app,
                        platform_id=platform_id)
                    second = make_el_torito_iso(
                        directory / f"second-{platform_id}.iso", app,
                        platform_id=platform_id)
                    self.assertEqual(first.sha256, second.sha256)
                    self.assertEqual(first.path.read_bytes(),
                                     second.path.read_bytes())

            udf_first = make_udf_bridge_iso(directory / "first.iso", app)
            udf_second = make_udf_bridge_iso(directory / "second.iso", app)
            self.assertEqual(udf_first.sha256, udf_second.sha256)
            self.assertEqual(udf_first.path.read_bytes(),
                             udf_second.path.read_bytes())

    def test_media_builders_have_valid_structures(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            app = directory / "BOOTIA64.EFI"
            app_bytes = b"MZ" + bytes((index * 29 + 7) & 0xff
                                      for index in range(8191))
            app.write_bytes(app_bytes)

            for layout in ("whole", "gpt", "mbr", "mbr-fallback"):
                with self.subTest(builder="fat", layout=layout):
                    info = make_fat_disk(directory / f"{layout}.img",
                                         app, layout)
                    image = info.path.read_bytes()
                    start_lba = (0 if layout == "whole" else
                                 media_format.GPT_ESP_START if layout == "gpt"
                                 else media_format.MBR_ESP_START)
                    self.assert_fat_volume(
                        image, start_lba * media_format.SECTOR_SIZE, app_bytes,
                        expected_sectors=media_format.DISK_SECTORS,
                        expected_hidden=start_lba,
                        test_offset=info.test_offset)
                    self.assertEqual(
                        info.test_data,
                        image[info.test_offset:
                              info.test_offset +
                              media_format.TEST_BLOCKS *
                              media_format.SECTOR_SIZE])
                    if layout == "gpt":
                        self.assert_gpt(image)
                    elif layout == "mbr":
                        self.assertEqual(image[510:512], b"\x55\xaa")
                        self.assertEqual(image[446 + 4], 0x0e)
                        self.assertEqual(
                            struct.unpack_from("<II", image, 446 + 8),
                            (media_format.MBR_ESP_START,
                             media_format.DISK_SECTORS))
                    elif layout == "mbr-fallback":
                        self.assertEqual(image[510:512], b"\x55\xaa")
                        self.assertEqual((image[446 + 4], image[462 + 4]),
                                         (0x0b, 0xef))
                        self.assertEqual(
                            struct.unpack_from("<II", image, 462 + 8),
                            (media_format.MBR_ESP_START,
                             media_format.DISK_SECTORS))

            for platform_id in (0xef, 0):
                with self.subTest(builder="eltorito",
                                  platform_id=platform_id):
                    info = make_el_torito_iso(
                        directory / f"eltorito-{platform_id}.iso", app,
                        platform_id=platform_id)
                    self.assert_iso9660(info.path, platform_id, 256)
                    image = info.path.read_bytes()
                    self.assert_fat_volume(
                        image, 64 * media_format.ISO_SECTOR_SIZE, app_bytes,
                        expected_sectors=256, expected_hidden=0)

            udf = make_udf_bridge_iso(directory / "udf.iso", app)
            self.assert_iso9660(udf.path, 0xef, 256)
            self.assert_fat_volume(
                udf.path.read_bytes(),
                media_format.UDF_BOOT_LBA * media_format.ISO_SECTOR_SIZE,
                app_bytes, expected_sectors=256, expected_hidden=0)
            self.assert_udf(udf.path, app_bytes)

class Ia64NvramTool(unittest.TestCase):
    """scripts/ia64-nvram.py, which converts a store for the zx1 board."""

    def setUp(self):
        path = Path(__file__).parents[3] / "scripts" / "ia64-nvram.py"
        spec = importlib.util.spec_from_file_location("ia64_nvram", path)
        self.tool = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.tool)

    def make_own_store(self, count=3):
        store = bytearray(self.tool.OWN_STORE_SIZE)
        store[:8] = self.tool.OWN_STORE_MAGIC
        struct.pack_into("<II", store, 8, 1, count)
        return bytes(store)

    def test_convert(self):
        tool = self.tool
        with tempfile.TemporaryDirectory() as directory:
            src = Path(directory) / "old.nvram"
            dst = Path(directory) / "zx1.nvram"
            src.write_bytes(self.make_own_store())
            self.assertEqual(tool.main(["convert", str(src), str(dst)]), 0)
            out = dst.read_bytes()
            self.assertEqual(len(out), tool.ZX1_FILE_SIZE)
            self.assertEqual(tool.own_store(out), self.make_own_store())
            # The BMC in the new file is a new one.
            self.assertIsNone(tool.bmc_tokens(out))
            # The vendor parts of the store stay blank for the firmware to form.
            for offset, tag, _name in tool.VENDOR_TAGS:
                self.assertNotEqual(out[offset:offset + len(tag)], tag)
            # A store that is already converted is refused, not converted twice.
            self.assertEqual(tool.main(["convert", str(dst), str(src)]), 1)

    def test_bmc_tokens(self):
        tool = self.tool
        image = bytearray(tool.ZX1_FILE_SIZE)
        area = tool.PDH_STORE_SIZE
        image[area:area + 8] = tool.BMC_TOKENS_TAG
        struct.pack_into("<HBB", image, area + 8, 0x500, 1, 18)
        struct.pack_into("<HB2s", image, area + 12, 0x941, 2, b"\x0a\x00")
        self.assertEqual(tool.bmc_tokens(bytes(image)),
                         {0x500: b"\x12", 0x941: b"\x0a\x00"})
        # A file of just the part keeps no tokens yet, and both are zx1 files.
        self.assertIsNone(tool.bmc_tokens(bytes(image[:area])))
        with tempfile.TemporaryDirectory() as directory:
            src = Path(directory) / "zx1.nvram"
            src.write_bytes(bytes(image))
            self.assertEqual(tool.main(["convert", str(src),
                                        str(Path(directory) / "x")]), 1)

    def test_convert_from_flash(self):
        tool = self.tool
        image = bytearray(0x400000)
        start = len(image) - tool.FLASH_STORE_FROM_END
        image[start:start + tool.OWN_STORE_SIZE] = self.make_own_store(5)
        # The reset pointer block names a table inside the image.
        struct.pack_into("<Q", image, len(image) - tool.FLASH_FIT_FROM_END,
                         0x80000000FFFF6000)
        with tempfile.TemporaryDirectory() as directory:
            src = Path(directory) / "flash.nvram"
            dst = Path(directory) / "zx1.nvram"
            src.write_bytes(bytes(image))
            self.assertTrue(tool.is_flash_image(bytes(image)))
            self.assertEqual(tool.main(["convert", str(src), str(dst)]), 0)
            self.assertEqual(tool.own_store(dst.read_bytes()),
                             self.make_own_store(5))

    def test_convert_refuses_a_file_with_no_store(self):
        tool = self.tool
        with tempfile.TemporaryDirectory() as directory:
            src = Path(directory) / "blank.nvram"
            dst = Path(directory) / "zx1.nvram"
            src.write_bytes(bytes(tool.OWN_STORE_SIZE))
            self.assertEqual(tool.main(["convert", str(src), str(dst)]), 1)
            self.assertFalse(dst.exists())


if __name__ == "__main__":
    QemuSystemTest.main()
