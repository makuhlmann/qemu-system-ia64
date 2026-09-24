/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * ECMA-167 / UDF 2.01 read-only optical filesystem.  The shared
 * EFI_FILE_PROTOCOL layer in filesystem.c consumes the fw_udf_* API.
 */

#include "fw-base.h"
#include "fw-services.h"
#include "fw-storage.h"
#include "fw-fs.h"

/* --- ECMA-167 / UDF 2.01 read-only optical filesystem --------------------- */

#define UDF_TAG_ANCHOR_VOLUME_DESCRIPTOR_POINTER 2U
#define UDF_TAG_PARTITION_DESCRIPTOR             5U
#define UDF_TAG_LOGICAL_VOLUME_DESCRIPTOR        6U
#define UDF_TAG_TERMINATING_DESCRIPTOR           8U
#define UDF_TAG_FILE_SET_DESCRIPTOR              256U
#define UDF_TAG_FILE_IDENTIFIER_DESCRIPTOR       257U
#define UDF_TAG_ALLOCATION_EXTENT_DESCRIPTOR     258U
#define UDF_TAG_FILE_ENTRY                       261U
#define UDF_TAG_EXTENDED_FILE_ENTRY              266U

#define UDF_AD_TYPE_RECORDED                     0U
#define UDF_AD_TYPE_UNRECORDED_ALLOCATED         1U
#define UDF_AD_TYPE_UNRECORDED_UNALLOCATED       2U
#define UDF_AD_TYPE_CONTINUATION                 3U

#define UDF_ICB_AD_SHORT                         0U
#define UDF_ICB_AD_LONG                          1U
#define UDF_ICB_AD_EXTENDED                      2U
#define UDF_ICB_AD_INLINE                        3U

#define UDF_FILE_TYPE_DIRECTORY                  4U
#define UDF_FILE_TYPE_RANDOM_BYTES               5U

#define UDF_FID_CHAR_EXISTENCE                   0x01U
#define UDF_FID_CHAR_DIRECTORY                   0x02U
#define UDF_FID_CHAR_DELETED                     0x04U
#define UDF_FID_CHAR_PARENT                      0x08U

static UINT16 fw_udf_crc16(const UINT8 *buf, UINTN len)
{
    UINT16 crc = 0;
    UINTN i;

    for (i = 0; i < len; i++) {
        UINTN bit;

        crc ^= (UINT16)buf[i] << 8;
        for (bit = 0; bit < 8; bit++) {
            if ((crc & 0x8000U) != 0) {
                crc = (UINT16)((crc << 1) ^ 0x1021U);
            } else {
                crc = (UINT16)(crc << 1);
            }
        }
    }
    return crc;
}

static BOOLEAN fw_udf_tag_valid(const UINT8 *buf, UINT16 expected_tag,
                                UINT32 expected_location, UINTN available)
{
    UINT8 checksum = 0;
    UINT16 tag;
    UINT16 version;
    UINT16 crc;
    UINT16 crc_len;
    UINTN i;

    if (buf == NULL || available < 16) {
        return 0;
    }

    tag = fw_le16(buf);
    version = fw_le16(buf + 2);
    if (tag != expected_tag || (version != 2U && version != 3U)) {
        return 0;
    }
    if (buf[5] != 0 ||
        (expected_location != 0xffffffffU &&
         fw_le32(buf + 12) != expected_location)) {
        return 0;
    }

    for (i = 0; i < 16; i++) {
        if (i != 4) {
            checksum = (UINT8)(checksum + buf[i]);
        }
    }
    if (checksum != buf[4]) {
        return 0;
    }

    crc_len = fw_le16(buf + 10);
    if ((UINTN)crc_len + 16U > available) {
        return 0;
    }
    crc = fw_le16(buf + 8);
    if (fw_udf_crc16(buf + 16, crc_len) != crc) {
        return 0;
    }
    return 1;
}

static BOOLEAN fw_udf_read_sector(UINT8 *buf, UINT32 lba)
{
    if (!storage_is_cd(&mBootStorageDevice) || buf == NULL) {
        return 0;
    }
    return storage_read_blocks(&mBootStorageDevice, buf, lba, 1);
}

static BOOLEAN fw_udf_regid_matches(const UINT8 *regid, const char *id)
{
    UINTN i;

    if (regid == NULL || id == NULL) {
        return 0;
    }
    for (i = 0; id[i] != 0; i++) {
        if (i >= 23 || regid[1 + i] != (UINT8)id[i]) {
            return 0;
        }
    }
    return 1;
}

static BOOLEAN fw_udf_vrs_valid(void)
{
    UINT8 sec[ATAPI_SECTOR_SIZE];
    BOOLEAN begin = 0;
    BOOLEAN nsr = 0;
    UINT32 lba;

    for (lba = 16; lba < 64; lba++) {
        if (!fw_udf_read_sector(sec, lba)) {
            return 0;
        }
        if (sec[0] != 0 || sec[6] != 1) {
            continue;
        }
        if (fw_bytes_eq(sec + 1, "BEA01", 5)) {
            begin = 1;
            continue;
        }
        if (begin &&
            (fw_bytes_eq(sec + 1, "NSR02", 5) ||
             fw_bytes_eq(sec + 1, "NSR03", 5))) {
            nsr = 1;
            continue;
        }
        if (begin && fw_bytes_eq(sec + 1, "TEA01", 5)) {
            return nsr;
        }
    }
    return 0;
}

static BOOLEAN fw_udf_extent_is_recorded(UINT32 raw_length)
{
    UINT32 extent_type = raw_length >> 30;

    return (raw_length & 0x3fffffffU) != 0 &&
           extent_type == UDF_AD_TYPE_RECORDED;
}

static BOOLEAN fw_udf_parse_anchor(UINT32 lba, UINT32 *MainLocation,
                                   UINT32 *MainLength)
{
    UINT8 sec[ATAPI_SECTOR_SIZE];

    if (MainLocation == NULL || MainLength == NULL ||
        !fw_udf_read_sector(sec, lba)) {
        return 0;
    }
    if (!fw_udf_tag_valid(sec, UDF_TAG_ANCHOR_VOLUME_DESCRIPTOR_POINTER,
                          lba, sizeof(sec))) {
        return 0;
    }

    *MainLength = fw_le32(sec + 16);
    *MainLocation = fw_le32(sec + 20);
    return *MainLength != 0;
}

static BOOLEAN fw_udf_find_anchor(UINT32 *MainLocation, UINT32 *MainLength)
{
    UINT32 anchors[4];
    UINTN count = 0;
    UINTN i;

    anchors[count++] = 256;
    if (mCdromBlocks > 0) {
        anchors[count++] = mCdromBlocks - 1U;
        if (mCdromBlocks > 256U) {
            anchors[count++] = mCdromBlocks - 256U;
        }
    }
    anchors[count++] = 512;

    for (i = 0; i < count; i++) {
        if (fw_udf_parse_anchor(anchors[i], MainLocation, MainLength)) {
            return 1;
        }
    }
    return 0;
}

static BOOLEAN fw_udf_partition_lba(UINT16 PartitionReference,
                                    UINT32 LogicalBlock, UINT32 *PhysicalLba)
{
    if (PhysicalLba == NULL ||
        PartitionReference != mUdfVolume.partition_reference ||
        LogicalBlock >= mUdfVolume.partition_length) {
        return 0;
    }
    *PhysicalLba = mUdfVolume.partition_start + LogicalBlock;
    return 1;
}

static BOOLEAN fw_udf_read_logical(UINT16 PartitionReference,
                                   UINT32 LogicalBlock, UINT8 *Buffer)
{
    UINT32 physical;

    if (!fw_udf_partition_lba(PartitionReference, LogicalBlock, &physical)) {
        return 0;
    }
    return fw_udf_read_sector(Buffer, physical);
}

static BOOLEAN fw_udf_read_logicals(UINT16 PartitionReference,
                                    UINT32 LogicalBlock, UINT8 *Buffer,
                                    UINT32 Count)
{
    UINT32 physical;

    if (Count == 0) {
        return 1;
    }
    if (Buffer == NULL ||
        PartitionReference != mUdfVolume.partition_reference ||
        LogicalBlock >= mUdfVolume.partition_length ||
        Count - 1U > mUdfVolume.partition_length - LogicalBlock - 1U) {
        return 0;
    }
    physical = mUdfVolume.partition_start + LogicalBlock;
    return fw_iso_read_sectors(Buffer, physical, Count);
}

static BOOLEAN fw_udf_read_descriptor(UINT16 PartitionReference,
                                      UINT32 LogicalBlock,
                                      UINT16 ExpectedTag, UINT8 *Buffer)
{
    if (!fw_udf_read_logical(PartitionReference, LogicalBlock, Buffer)) {
        return 0;
    }
    return fw_udf_tag_valid(Buffer, ExpectedTag, LogicalBlock,
                            ATAPI_SECTOR_SIZE);
}

static BOOLEAN fw_udf_parse_file_meta(UINT16 PartitionReference,
                                      UINT32 Icb, FW_UDF_FILE_META *Meta,
                                      UINT8 *Descriptor)
{
    UINT16 tag;
    UINT32 ea_len;
    UINT32 ad_len;
    UINT32 ad_offset;

    if (Meta == NULL || Descriptor == NULL ||
        !fw_udf_read_logical(PartitionReference, Icb, Descriptor)) {
        return 0;
    }

    tag = fw_le16(Descriptor);
    if (tag != UDF_TAG_FILE_ENTRY && tag != UDF_TAG_EXTENDED_FILE_ENTRY) {
        return 0;
    }
    if (!fw_udf_tag_valid(Descriptor, tag, Icb, ATAPI_SECTOR_SIZE)) {
        return 0;
    }

    Meta->icb = Icb;
    Meta->partition_reference = PartitionReference;
    Meta->file_type = Descriptor[16 + 11];
    Meta->icb_flags = fw_le16(Descriptor + 16 + 18);
    Meta->information_length = fw_le64(Descriptor + 56);

    if (tag == UDF_TAG_FILE_ENTRY) {
        ea_len = fw_le32(Descriptor + 168);
        ad_len = fw_le32(Descriptor + 172);
        ad_offset = 176U + ea_len;
    } else {
        ea_len = fw_le32(Descriptor + 208);
        ad_len = fw_le32(Descriptor + 212);
        ad_offset = 216U + ea_len;
    }

    if (ad_offset > ATAPI_SECTOR_SIZE ||
        ad_len > ATAPI_SECTOR_SIZE - ad_offset) {
        return 0;
    }
    Meta->allocation_offset = ad_offset;
    Meta->allocation_length = ad_len;
    return 1;
}

static EFI_STATUS fw_udf_read_extent(UINT16 PartitionReference,
                                     UINT32 LogicalBlock,
                                     UINT64 ExtentOffset,
                                     UINT8 *Buffer, UINT32 *Length)
{
    UINT8 sec[ATAPI_SECTOR_SIZE];
    UINT32 done = 0;
    UINT32 want;

    if (Buffer == NULL || Length == NULL) {
        return EFI_INVALID_PARAMETER;
    }
    want = *Length;

    while (done < want) {
        UINT32 block_delta = (UINT32)(ExtentOffset / ATAPI_SECTOR_SIZE);
        UINT32 block_off = (UINT32)(ExtentOffset -
                                    ((UINT64)block_delta * ATAPI_SECTOR_SIZE));
        UINT32 chunk = ATAPI_SECTOR_SIZE - block_off;

        if (chunk > want - done) {
            chunk = want - done;
        }
        if (block_off == 0 && chunk == ATAPI_SECTOR_SIZE) {
            UINT32 blocks = (want - done) / ATAPI_SECTOR_SIZE;

            if (!fw_udf_read_logicals(PartitionReference,
                                      LogicalBlock + block_delta,
                                      Buffer + done, blocks)) {
                *Length = done;
                return EFI_DEVICE_ERROR;
            }
            done += blocks * ATAPI_SECTOR_SIZE;
            ExtentOffset += (UINT64)blocks * ATAPI_SECTOR_SIZE;
            continue;
        }
        if (!fw_udf_read_logical(PartitionReference,
                                 LogicalBlock + block_delta, sec)) {
            *Length = done;
            return EFI_DEVICE_ERROR;
        }
        fw_copy_mem(Buffer + done, sec + block_off, chunk);
        done += chunk;
        ExtentOffset += chunk;
    }

    *Length = done;
    return EFI_SUCCESS;
}

static EFI_STATUS fw_udf_read_zeroes(UINT8 *Buffer, UINT32 *Length)
{
    UINT32 i;

    if (Buffer == NULL || Length == NULL) {
        return EFI_INVALID_PARAMETER;
    }
    for (i = 0; i < *Length; i++) {
        Buffer[i] = 0;
    }
    return EFI_SUCCESS;
}

static EFI_STATUS fw_udf_read_file_from_ad_field(const UINT8 *Field,
                                                 UINT32 FieldLength,
                                                 UINT8 AdType,
                                                 UINT16 DefaultPartition,
                                                 UINT64 Offset,
                                                 VOID *Buffer,
                                                 UINT32 *ReadSize,
                                                 UINTN Depth);

static EFI_STATUS fw_udf_read_continuation(UINT16 PartitionReference,
                                           UINT32 LogicalBlock,
                                           UINT64 Offset,
                                           VOID *Buffer,
                                           UINT32 *ReadSize,
                                           UINTN Depth)
{
    UINT8 sec[ATAPI_SECTOR_SIZE];
    UINT32 ad_len;

    if (Depth > 4 ||
        !fw_udf_read_descriptor(PartitionReference, LogicalBlock,
                                UDF_TAG_ALLOCATION_EXTENT_DESCRIPTOR, sec)) {
        return EFI_VOLUME_CORRUPTED;
    }

    ad_len = fw_le32(sec + 20);
    if (ad_len > ATAPI_SECTOR_SIZE - 24U) {
        return EFI_VOLUME_CORRUPTED;
    }
    return fw_udf_read_file_from_ad_field(sec + 24, ad_len, UDF_ICB_AD_SHORT,
                                          PartitionReference, Offset, Buffer,
                                          ReadSize, Depth + 1U);
}

static EFI_STATUS fw_udf_read_file_from_ad_field(const UINT8 *Field,
                                                 UINT32 FieldLength,
                                                 UINT8 AdType,
                                                 UINT16 DefaultPartition,
                                                 UINT64 Offset,
                                                 VOID *Buffer,
                                                 UINT32 *ReadSize,
                                                 UINTN Depth)
{
    UINT8 *dst = (UINT8 *)Buffer;
    UINT32 remaining;
    UINT32 done = 0;
    UINT32 pos = 0;

    if (Field == NULL || Buffer == NULL || ReadSize == NULL) {
        return EFI_INVALID_PARAMETER;
    }
    remaining = *ReadSize;

    if (AdType == UDF_ICB_AD_INLINE) {
        if (Offset >= FieldLength) {
            *ReadSize = 0;
            return EFI_SUCCESS;
        }
        if (remaining > FieldLength - (UINT32)Offset) {
            remaining = FieldLength - (UINT32)Offset;
        }
        fw_copy_mem(dst, (VOID *)(Field + (UINT32)Offset), remaining);
        *ReadSize = remaining;
        return EFI_SUCCESS;
    }

    while (pos < FieldLength && remaining != 0) {
        UINT32 raw_length;
        UINT32 length;
        UINT32 extent_type;
        UINT32 logical_block;
        UINT16 partition_ref;
        UINT64 info_length;
        UINT32 descriptor_size;

        if (AdType == UDF_ICB_AD_SHORT) {
            if (FieldLength - pos < 8U) {
                return EFI_VOLUME_CORRUPTED;
            }
            raw_length = fw_le32(Field + pos);
            logical_block = fw_le32(Field + pos + 4);
            partition_ref = DefaultPartition;
            info_length = raw_length & 0x3fffffffU;
            descriptor_size = 8;
        } else if (AdType == UDF_ICB_AD_LONG) {
            if (FieldLength - pos < 16U) {
                return EFI_VOLUME_CORRUPTED;
            }
            raw_length = fw_le32(Field + pos);
            logical_block = fw_le32(Field + pos + 4);
            partition_ref = fw_le16(Field + pos + 8);
            info_length = raw_length & 0x3fffffffU;
            descriptor_size = 16;
        } else if (AdType == UDF_ICB_AD_EXTENDED) {
            if (FieldLength - pos < 20U) {
                return EFI_VOLUME_CORRUPTED;
            }
            raw_length = fw_le32(Field + pos);
            logical_block = fw_le32(Field + pos + 12);
            partition_ref = fw_le16(Field + pos + 16);
            info_length = fw_le32(Field + pos + 8);
            descriptor_size = 20;
        } else {
            return EFI_UNSUPPORTED;
        }

        extent_type = raw_length >> 30;
        length = raw_length & 0x3fffffffU;
        if (length == 0) {
            break;
        }

        if (extent_type == UDF_AD_TYPE_CONTINUATION) {
            UINT32 continuation_read = remaining;
            EFI_STATUS st = fw_udf_read_continuation(partition_ref,
                                                     logical_block, Offset,
                                                     dst + done,
                                                     &continuation_read,
                                                     Depth);
            *ReadSize = done + continuation_read;
            return st;
        }

        if (info_length > length) {
            info_length = length;
        }
        if (Offset >= info_length) {
            Offset -= info_length;
            pos += descriptor_size;
            continue;
        }

        {
            UINT32 chunk = remaining;
            EFI_STATUS st;

            if ((UINT64)chunk > info_length - Offset) {
                chunk = (UINT32)(info_length - Offset);
            }
            if (extent_type == UDF_AD_TYPE_RECORDED) {
                st = fw_udf_read_extent(partition_ref, logical_block, Offset,
                                        dst + done, &chunk);
            } else {
                st = fw_udf_read_zeroes(dst + done, &chunk);
            }
            done += chunk;
            remaining -= chunk;
            if (st != EFI_SUCCESS || chunk == 0) {
                *ReadSize = done;
                return st;
            }
        }
        Offset = 0;
        pos += descriptor_size;
    }

    *ReadSize = done;
    return EFI_SUCCESS;
}

EFI_STATUS fw_udf_read_file_bytes(UINT16 PartitionReference,
                                         UINT32 Icb, UINT64 Offset,
                                         VOID *Buffer, UINT32 *ReadSize)
{
    UINT8 desc[ATAPI_SECTOR_SIZE];
    FW_UDF_FILE_META meta;
    UINT8 ad_type;
    UINT64 size_left;

    if (Buffer == NULL || ReadSize == NULL) {
        return EFI_INVALID_PARAMETER;
    }
    if (!fw_udf_parse_file_meta(PartitionReference, Icb, &meta, desc)) {
        *ReadSize = 0;
        return EFI_VOLUME_CORRUPTED;
    }
    if (Offset >= meta.information_length) {
        *ReadSize = 0;
        return EFI_SUCCESS;
    }

    size_left = meta.information_length - Offset;
    if ((UINT64)*ReadSize > size_left) {
        *ReadSize = (UINT32)size_left;
    }
    ad_type = (UINT8)(meta.icb_flags & 7U);
    return fw_udf_read_file_from_ad_field(desc + meta.allocation_offset,
                                          meta.allocation_length,
                                          ad_type, PartitionReference,
                                          Offset, Buffer, ReadSize, 0);
}

static UINTN fw_udf_uncompress_name(const UINT8 *Compressed, UINTN Bytes,
                                    CHAR16 *Name, UINTN NameChars)
{
    UINT8 comp_id;
    UINTN in = 1;
    UINTN out = 0;

    if (Compressed == NULL || Name == NULL || NameChars == 0 || Bytes == 0) {
        return 0;
    }
    comp_id = Compressed[0];
    if (comp_id != 8U && comp_id != 16U) {
        Name[0] = 0;
        return 0;
    }

    while (in < Bytes && out + 1U < NameChars) {
        if (comp_id == 16U) {
            if (in + 1U >= Bytes) {
                break;
            }
            Name[out++] = (CHAR16)(((UINT16)Compressed[in] << 8) |
                                   Compressed[in + 1U]);
            in += 2;
        } else {
            Name[out++] = (CHAR16)Compressed[in++];
        }
    }
    Name[out] = 0;
    return out;
}

static BOOLEAN fw_udf_name_matches(const CHAR16 *Name, UINTN Len,
                                   const CHAR16 *EntryName)
{
    UINTN i;

    if (Name == NULL || EntryName == NULL) {
        return 0;
    }
    for (i = 0; i < Len; i++) {
        CHAR16 a = Name[i];
        CHAR16 b = EntryName[i];

        if (b == 0) {
            return 0;
        }
        if (a <= 0xffU && b <= 0xffU) {
            a = (CHAR16)fw_ascii_upper((UINT8)a);
            b = (CHAR16)fw_ascii_upper((UINT8)b);
        }
        if (a != b) {
            return 0;
        }
    }
    return EntryName[Len] == 0;
}

static UINTN fw_udf_fid_total_length(const UINT8 *Fid, UINTN Available)
{
    UINT16 implementation_len;
    UINTN total;

    if (Fid == NULL || Available < 38U) {
        return 0;
    }
    implementation_len = fw_le16(Fid + 36);
    total = 38U + implementation_len + Fid[19];
    total = (total + 3U) & ~3U;
    return total <= Available ? total : 0;
}

static BOOLEAN fw_udf_parse_fid(const UINT8 *Fid, UINTN Available,
                                UINT32 TagLocation, FW_UDF_ENTRY *Entry)
{
    UINTN total;
    UINT16 implementation_len;
    UINT32 raw_length;

    if (Fid == NULL || Entry == NULL) {
        return 0;
    }
    total = fw_udf_fid_total_length(Fid, Available);
    if (total == 0 ||
        !fw_udf_tag_valid(Fid, UDF_TAG_FILE_IDENTIFIER_DESCRIPTOR,
                          TagLocation, total)) {
        return 0;
    }

    Entry->file_characteristics = Fid[18];
    implementation_len = fw_le16(Fid + 36);
    raw_length = fw_le32(Fid + 20);
    Entry->icb = fw_le32(Fid + 24);
    Entry->partition_reference = fw_le16(Fid + 28);
    Entry->size = raw_length & 0x3fffffffU;
    Entry->file_type = 0;
    Entry->icb_flags = 0;
    Entry->name[0] = 0;
    if (Fid[19] != 0) {
        (void)fw_udf_uncompress_name(Fid + 38U + implementation_len,
                                     Fid[19], Entry->name,
                                     sizeof(Entry->name) /
                                     sizeof(Entry->name[0]));
    }
    return 1;
}

BOOLEAN fw_udf_entry_load_meta(FW_UDF_ENTRY *Entry)
{
    UINT8 desc[ATAPI_SECTOR_SIZE];
    FW_UDF_FILE_META meta;

    if (Entry == NULL ||
        !fw_udf_parse_file_meta(Entry->partition_reference, Entry->icb,
                                &meta, desc)) {
        return 0;
    }
    Entry->file_type = meta.file_type;
    Entry->icb_flags = meta.icb_flags;
    Entry->size = meta.information_length;
    return 1;
}

EFI_STATUS fw_udf_next_dir_entry(FW_FILE *Dir, FW_UDF_ENTRY *Entry)
{
    UINT8 sec[ATAPI_SECTOR_SIZE];

    if (Dir == NULL || Entry == NULL) {
        return EFI_INVALID_PARAMETER;
    }

    while (Dir->position < Dir->size) {
        UINT64 block_start = Dir->position & ~(UINT64)(ATAPI_SECTOR_SIZE - 1U);
        UINT32 block_off = (UINT32)(Dir->position - block_start);
        UINT32 read_size = ATAPI_SECTOR_SIZE;
        UINTN total;
        EFI_STATUS st;

        st = fw_udf_read_file_bytes(Dir->partition_reference, Dir->extent,
                                    block_start, sec, &read_size);
        if (st != EFI_SUCCESS) {
            return st;
        }
        if (read_size <= block_off || sec[block_off] == 0) {
            Dir->position = block_start + ATAPI_SECTOR_SIZE;
            continue;
        }

        total = fw_udf_fid_total_length(sec + block_off,
                                        read_size - block_off);
        if (total == 0) {
            return EFI_VOLUME_CORRUPTED;
        }
        Dir->position += total;

        if (!fw_udf_parse_fid(sec + block_off, read_size - block_off,
                              0xffffffffU,
                              Entry)) {
            return EFI_VOLUME_CORRUPTED;
        }
        if ((Entry->file_characteristics &
             (UDF_FID_CHAR_EXISTENCE | UDF_FID_CHAR_DELETED |
              UDF_FID_CHAR_PARENT)) != 0) {
            continue;
        }
        if (!fw_udf_entry_load_meta(Entry)) {
            return EFI_VOLUME_CORRUPTED;
        }
        return EFI_SUCCESS;
    }

    return EFI_NOT_FOUND;
}

static EFI_STATUS fw_udf_find_in_dir(FW_FILE *Base, const CHAR16 *Name,
                                     UINTN Len, FW_UDF_ENTRY *Entry)
{
    FW_FILE dir;
    EFI_STATUS st;

    if (Name == NULL || Entry == NULL) {
        return EFI_INVALID_PARAMETER;
    }

    fw_set_mem(&dir, sizeof(dir), 0);
    dir.fs_kind = FW_FS_UDF;
    dir.is_dir = 1;
    if (Base != NULL && Base->fs_kind == FW_FS_UDF && Base->is_dir) {
        dir.extent = Base->extent;
        dir.partition_reference = Base->partition_reference;
        dir.size = Base->size;
    } else {
        dir.extent = mUdfVolume.root_icb;
        dir.partition_reference = mUdfVolume.root_partition_reference;
        {
            FW_UDF_ENTRY root;

            fw_set_mem(&root, sizeof(root), 0);
            root.icb = mUdfVolume.root_icb;
            root.partition_reference = mUdfVolume.root_partition_reference;
            if (!fw_udf_entry_load_meta(&root)) {
                return EFI_VOLUME_CORRUPTED;
            }
            dir.size = root.size;
        }
    }

    for (;;) {
        st = fw_udf_next_dir_entry(&dir, Entry);
        if (st != EFI_SUCCESS) {
            return st;
        }
        if (fw_udf_name_matches(Name, Len, Entry->name)) {
            return EFI_SUCCESS;
        }
    }
}

EFI_STATUS fw_udf_lookup(FW_FILE *Base, CHAR16 *Path,
                                FW_UDF_ENTRY *Entry)
{
    FW_FILE dir;
    FW_UDF_ENTRY current;
    CHAR16 *p;

    if (Path == NULL || Entry == NULL) {
        return EFI_INVALID_PARAMETER;
    }
    if (!fw_udf_init()) {
        return EFI_NOT_FOUND;
    }

    fw_set_mem(&dir, sizeof(dir), 0);
    dir.fs_kind = FW_FS_UDF;
    dir.is_dir = 1;
    if (Base != NULL && Base->fs_kind == FW_FS_UDF && Base->is_dir &&
        *Path != '\\' && *Path != '/') {
        dir.extent = Base->extent;
        dir.partition_reference = Base->partition_reference;
        dir.size = Base->size;
    } else {
        fw_set_mem(&current, sizeof(current), 0);
        current.icb = mUdfVolume.root_icb;
        current.partition_reference = mUdfVolume.root_partition_reference;
        if (!fw_udf_entry_load_meta(&current)) {
            return EFI_VOLUME_CORRUPTED;
        }
        dir.extent = current.icb;
        dir.partition_reference = current.partition_reference;
        dir.size = current.size;
    }

    p = Path;
    while (*p == '\\' || *p == '/') {
        p++;
    }
    if (*p == 0) {
        fw_set_mem(Entry, sizeof(*Entry), 0);
        Entry->icb = dir.extent;
        Entry->partition_reference = dir.partition_reference;
        Entry->file_characteristics = UDF_FID_CHAR_DIRECTORY;
        Entry->file_type = UDF_FILE_TYPE_DIRECTORY;
        Entry->size = dir.size;
        return EFI_SUCCESS;
    }

    for (;;) {
        CHAR16 *start = p;
        UINTN len;

        while (*p != 0 && *p != '\\' && *p != '/') {
            p++;
        }
        len = p - start;
        if (len == 0) {
            return EFI_INVALID_PARAMETER;
        }
        if (len == 1 && start[0] == '.') {
            while (*p == '\\' || *p == '/') {
                p++;
            }
            if (*p == 0) {
                fw_set_mem(Entry, sizeof(*Entry), 0);
                Entry->icb = dir.extent;
                Entry->partition_reference = dir.partition_reference;
                Entry->file_characteristics = UDF_FID_CHAR_DIRECTORY;
                Entry->file_type = UDF_FILE_TYPE_DIRECTORY;
                Entry->size = dir.size;
                return EFI_SUCCESS;
            }
            continue;
        }

        {
            EFI_STATUS st = fw_udf_find_in_dir(&dir, start, len, Entry);
            if (st != EFI_SUCCESS) {
                return st;
            }
        }

        while (*p == '\\' || *p == '/') {
            p++;
        }
        if (*p == 0) {
            return EFI_SUCCESS;
        }
        if ((Entry->file_characteristics & UDF_FID_CHAR_DIRECTORY) == 0 &&
            Entry->file_type != UDF_FILE_TYPE_DIRECTORY) {
            return EFI_NOT_FOUND;
        }
        dir.extent = Entry->icb;
        dir.partition_reference = Entry->partition_reference;
        dir.size = Entry->size;
    }
}

BOOLEAN fw_udf_init(void)
{
    UINT32 main_location = 0;
    UINT32 main_length = 0;
    UINT32 lvd_fsd_raw = 0;
    UINT32 lvd_fsd_location = 0;
    UINT16 lvd_fsd_partition = 0;
    UINT32 lba;
    UINT32 end_lba;
    UINT8 sec[ATAPI_SECTOR_SIZE];
    BOOLEAN have_lvd = 0;
    BOOLEAN have_partition = 0;
    UINT16 map_partition_number = 0;
    UINT16 map_partition_reference = 0;

    if (mUdfVolume.valid) {
        return 1;
    }
    if (mUdfVolume.checked) {
        return 0;
    }
    mUdfVolume.checked = 1;

    if (!storage_is_cd(&mBootStorageDevice)) {
        return 0;
    }
    if (mCdromBlocks == 0 && !atapi_configure_el_torito()) {
        return 0;
    }
    if (!fw_udf_vrs_valid()) {
        return 0;
    }
    if (!fw_udf_find_anchor(&main_location, &main_length)) {
        return 0;
    }

    end_lba = main_location + ((main_length + ATAPI_SECTOR_SIZE - 1U) /
                               ATAPI_SECTOR_SIZE);
    for (lba = main_location; lba < end_lba; lba++) {
        UINT16 tag;

        if (!fw_udf_read_sector(sec, lba)) {
            return 0;
        }
        tag = fw_le16(sec);
        if (tag == UDF_TAG_TERMINATING_DESCRIPTOR) {
            break;
        }
        if (tag == UDF_TAG_PARTITION_DESCRIPTOR &&
            fw_udf_tag_valid(sec, UDF_TAG_PARTITION_DESCRIPTOR,
                             lba, sizeof(sec)) &&
            (fw_udf_regid_matches(sec + 24, "+NSR02") ||
             fw_udf_regid_matches(sec + 24, "+NSR03"))) {
            mUdfVolume.partition_number = fw_le16(sec + 22);
            mUdfVolume.partition_start = fw_le32(sec + 188);
            mUdfVolume.partition_length = fw_le32(sec + 192);
            have_partition = 1;
        } else if (tag == UDF_TAG_LOGICAL_VOLUME_DESCRIPTOR &&
                   fw_udf_tag_valid(sec, UDF_TAG_LOGICAL_VOLUME_DESCRIPTOR,
                                    lba, sizeof(sec))) {
            UINT32 block_size = fw_le32(sec + 212);
            UINT32 map_len = fw_le32(sec + 264);
            UINT32 map_count = fw_le32(sec + 268);
            UINT32 pos = 440;
            UINT32 map_index;

            if (block_size != ATAPI_SECTOR_SIZE ||
                map_len > ATAPI_SECTOR_SIZE - 440U) {
                return 0;
            }
            mUdfVolume.logical_block_size = block_size;
            if (sec[211] != 0 && sec[211] <= 127U) {
                (void)fw_udf_uncompress_name(
                    sec + 84U, sec[211], mUdfVolume.label,
                    FW_ARRAY_SIZE(mUdfVolume.label));
            }
            lvd_fsd_raw = fw_le32(sec + 248);
            lvd_fsd_location = fw_le32(sec + 252);
            lvd_fsd_partition = fw_le16(sec + 256);
            for (map_index = 0; map_index < map_count && pos + 2U <= 440U + map_len;
                 map_index++) {
                UINT8 map_type = sec[pos];
                UINT8 this_map_len = sec[pos + 1U];

                if (this_map_len < 2U || pos + this_map_len > 440U + map_len) {
                    return 0;
                }
                if (map_type == 1U && this_map_len == 6U) {
                    map_partition_reference = (UINT16)map_index;
                    map_partition_number = fw_le16(sec + pos + 4U);
                }
                pos += this_map_len;
            }
            have_lvd = 1;
        }
    }

    if (!have_lvd || !have_partition ||
        map_partition_number != mUdfVolume.partition_number ||
        !fw_udf_extent_is_recorded(lvd_fsd_raw)) {
        return 0;
    }
    mUdfVolume.partition_reference = map_partition_reference;
    if (lvd_fsd_partition != mUdfVolume.partition_reference ||
        !fw_udf_read_descriptor(lvd_fsd_partition, lvd_fsd_location,
                                UDF_TAG_FILE_SET_DESCRIPTOR, sec)) {
        return 0;
    }

    {
        UINT32 root_raw = fw_le32(sec + 400);

        if (!fw_udf_extent_is_recorded(root_raw)) {
            return 0;
        }
        mUdfVolume.root_icb = fw_le32(sec + 404);
        mUdfVolume.root_partition_reference = fw_le16(sec + 408);
    }

    mUdfVolume.valid = 1;
    return 1;
}
