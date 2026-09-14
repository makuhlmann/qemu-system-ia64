/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Filesystem and block layer: FAT12/16 + BPB parsing, ISO-9660, the EFI
 * Block I/O / Disk I/O protocols, El Torito, MBR/GPT partition discovery
 * and the partition driver binding, device-path construction, and the
 * shared EFI_FILE_PROTOCOL / Simple File System layer serving FAT, ISO
 * and UDF.  Extracted verbatim from firmware.c (Phase 1 of
 * plans/firmware-rework-plan.md).
 */

#include "fw-base.h"
#include "fw-services.h"
#include "fw-storage.h"
#include "fw-fs.h"
#include "fw-platform-layout.h"
#include "fw-device-path.h"

/* --- FAT12/16 File System ------------------------------------------------- */

/* FAT_DIR_ENTRY lives in fw-fs.h. */

static BOOLEAN fw_read_512(UINT8 *buf, UINT32 lba);

/* --- FAT BPB parsing ------------------------------------------------------ */

typedef struct {
    UINT8  jmp[3];
    UINT8  oem[8];
    UINT16 bytes_per_sec;
    UINT8  sec_per_cluster;
    UINT16 reserved_secs;
    UINT8  num_fats;
    UINT16 root_entries;
    UINT16 total_secs_small;
    UINT8  media;
    UINT16 secs_per_fat_small;
    UINT16 secs_per_track;
    UINT16 num_heads;
    UINT32 hidden_secs;
    UINT32 total_secs_large;
    UINT8  drive_num;
    UINT8  reserved1;
    UINT8  boot_sig;
    UINT32 volume_id;
    UINT8  label[11];
    UINT8  fs_type[8];
} __attribute__((packed)) FAT_BPB;

static BOOLEAN bpb_is_valid(const FAT_BPB *bpb)
{
    return bpb->bytes_per_sec == 512 && bpb->sec_per_cluster > 0;
}

static UINT32 fw_udiv32(UINT32 dividend, UINT32 divisor)
{
    UINT32 quotient = 0;
    UINT32 bit = 1;

    if (divisor == 0) {
        return 0;
    }
    while ((divisor & 0x80000000U) == 0 && divisor <= (dividend >> 1)) {
        divisor <<= 1;
        bit <<= 1;
    }
    while (bit != 0) {
        if (dividend >= divisor) {
            dividend -= divisor;
            quotient |= bit;
        }
        divisor >>= 1;
        bit >>= 1;
    }
    return quotient;
}

/* --- EFI Block / Disk I/O Protocols --------------------------------------- */


/* EFI_BLOCK_IO typedefs live in fw-efi-types.h. */


/* struct _EFI_DISK_IO_PROTOCOL lives in fw-fs.h. */

EFI_BLOCK_IO_MEDIA    mBlockIoMedia;
EFI_BLOCK_IO_PROTOCOL mBlockIoProto;
EFI_DISK_IO_PROTOCOL  mBlockDiskIoProto;
EFI_BLOCK_IO_MEDIA    mRawBlockIoMedia;
EFI_BLOCK_IO_PROTOCOL mRawBlockIoProto;
EFI_DISK_IO_PROTOCOL  mRawDiskIoProto;
EFI_BLOCK_IO_MEDIA    mDiskBlockIoMedia;
EFI_BLOCK_IO_PROTOCOL mDiskBlockIoProto;
EFI_DISK_IO_PROTOCOL  mDiskIoProto;
static UINT8 mDiskIoScratch[SCSI_BOUNCE_SIZE]
    __attribute__((aligned(SCSI_BOUNCE_SIZE)));


/* FW_PARTITION_RECORD lives in fw-fs.h. */

FW_PARTITION_RECORD mPartitions[FW_PARTITION_MAX];
static EFI_DRIVER_BINDING_PROTOCOL mPartitionDriverBinding;
static EFI_COMPONENT_NAME_PROTOCOL mPartitionComponentName;
static EFI_STATUS fw_fat_install_partition_volume(
    FW_PARTITION_RECORD *Partition);
static EFI_STATUS fw_fat_uninstall_partition_volume(
    FW_PARTITION_RECORD *Partition);
static EFI_STATUS fw_fat_reinstall_partition_volume(
    FW_PARTITION_RECORD *Partition);
static VOID fw_fat_release_partition_volume(FW_PARTITION_RECORD *Partition);
UINT32 mBootImageStartLba;
UINT32 mBootImagePartitionBlocks;
UINT64 mBootImagePartitionCdBlocks;
static UINT32 mBootImageFatBlocks;
static UINT16 mBootImageCatalogSectorCount;
static BOOLEAN mBootImageUsesUefiSectorCount;
BOOLEAN mBootImageMapped;
static BOOLEAN mBootImageChecked;

UINT16 fw_le16(const UINT8 *p)
{
    return (UINT16)(p[0] | (p[1] << 8));
}

UINT32 fw_le32(const UINT8 *p)
{
    return (UINT32)p[0] | ((UINT32)p[1] << 8) |
           ((UINT32)p[2] << 16) | ((UINT32)p[3] << 24);
}

UINT64 fw_le64(const UINT8 *p)
{
    return (UINT64)fw_le32(p) | ((UINT64)fw_le32(p + 4) << 32);
}

BOOLEAN fw_bytes_eq(const UINT8 *p, const char *s, UINTN len)
{
    UINTN i;
    for (i = 0; i < len; i++) {
        if (p[i] != (UINT8)s[i]) {
            return 0;
        }
    }
    return 1;
}

BOOLEAN atapi_configure_el_torito(void)
{
    static UINT8 sec[ATAPI_SECTOR_SIZE];
    UINT32 catalog_lba = 0;
    UINT32 boot_lba;
    UINT32 filesystem_blocks;
    UINT32 partition_blocks;
    UINT16 catalog_sector_count;
    UINT8 platform_id;
    BOOLEAN have_bpb;
    BOOLEAN use_uefi_sector_count;
    UINTN i;

    if (mBootImageChecked) {
        return mBootImageMapped;
    }
    mBootImageChecked = 1;

    if (!storage_is_cd(&mBootStorageDevice)) {
        return 0;
    }

    for (i = 16; i < 32; i++) {
        if (!storage_read_blocks(&mBootStorageDevice, sec, (UINT32)i, 1)) {
            return 0;
        }
        if (!fw_bytes_eq(sec + 1, "CD001", 5)) {
            continue;
        }
        if (sec[0] == 1 && mCdromBlocks == 0) {
            mCdromBlocks = fw_le32(sec + 80);
        }
        if (sec[0] == 0xff) {
            break;
        }
        if (sec[0] == 0 &&
            fw_bytes_eq(sec + 7, "EL TORITO SPECIFICATION", 23)) {
            catalog_lba = fw_le32(sec + 71);
            break;
        }
    }

    if (catalog_lba == 0 ||
        !storage_read_blocks(&mBootStorageDevice, sec, catalog_lba, 1)) {
        return 0;
    }

    platform_id = sec[1];
    if (sec[0x20] != 0x88) {
        return 0;
    }

    catalog_sector_count = fw_le16(sec + 0x26);
    boot_lba = fw_le32(sec + 0x28);
    if (boot_lba == 0 ||
        !storage_read_blocks(&mBootStorageDevice, sec, boot_lba, 1)) {
        return 0;
    }

    have_bpb = bpb_is_valid((FAT_BPB *)sec);
    if (have_bpb) {
        FAT_BPB *bpb = (FAT_BPB *)sec;

        filesystem_blocks = bpb->total_secs_small;
        if (filesystem_blocks == 0) {
            filesystem_blocks = bpb->total_secs_large;
        }
    } else {
        filesystem_blocks = catalog_sector_count;
    }

    if (filesystem_blocks == 0) {
        return 0;
    }

    /*
     * EFI Platform ID entries use the catalog Sector Count as the EFI system
     * partition size.  Older no-emulation FAT boot images without the EFI
     * Platform ID are not EFI system partitions by that rule; expose the FAT
     * image described by the BPB so IA-64 EFI media that carry BOOTIA64.EFI in
     * such an image remain readable.
     */
    use_uefi_sector_count = platform_id == 0xef;
    if (!use_uefi_sector_count && have_bpb) {
        partition_blocks = filesystem_blocks;
    } else if (!use_uefi_sector_count) {
        /*
         * No EFI Platform ID and no FAT BPB at the boot image: this El Torito
         * entry is not an EFI System Partition.  Combo IA-64 discs (early
         * Server 2003 / .NET Server betas) carry only a legacy x86 CDBOOT
         * no-emulation loader here -- real-mode boot code, not a filesystem.
         * Do not map it as a bogus FAT volume; the disc's IA-64 loader
         * (\IA64\SETUPLDR.EFI) is reached through the raw ISO-9660 file system
         * instead, launched by hand from the EFI shell.
         */
        return 0;
    } else if (catalog_sector_count <= 1U) {
        if (mCdromBlocks <= boot_lba ||
            (UINT64)(mCdromBlocks - boot_lba) > (0xffffffffULL / 4U)) {
            return 0;
        }
        partition_blocks = (mCdromBlocks - boot_lba) * 4U;
    } else {
        partition_blocks = catalog_sector_count;
    }
    if (partition_blocks == 0) {
        return 0;
    }
    if (mCdromBlocks <= boot_lba ||
        ((UINT64)partition_blocks + 3U) / 4U >
            (UINT64)(mCdromBlocks - boot_lba)) {
        return 0;
    }

    mBootImageStartLba = boot_lba;
    mBootImagePartitionBlocks = partition_blocks;
    mBootImagePartitionCdBlocks = ((UINT64)partition_blocks + 3U) / 4U;
    mBootImageFatBlocks = filesystem_blocks;
    mBootImageCatalogSectorCount = catalog_sector_count;
    mBootImageUsesUefiSectorCount = use_uefi_sector_count;
    mBootImageMapped = 1;
    return 1;
}

static BOOLEAN fw_read_512(UINT8 *buf, UINT32 lba)
{
    if (!storage_present(&mBootStorageDevice)) {
        return 0;
    }

    if (!storage_is_cd(&mBootStorageDevice)) {
        if (storage_block_size(&mBootStorageDevice) != 512U) {
            return 0;
        }
        return storage_read_blocks(&mBootStorageDevice, buf, lba, 1);
    }

    if (!atapi_configure_el_torito()) {
        return 0;
    }

    if (lba >= mBootImagePartitionBlocks) {
        return 0;
    }

    {
        static UINT8 sec[ATAPI_SECTOR_SIZE] __attribute__((aligned(8)));
        static UINT32 cached_iso_lba;
        static BOOLEAN cached_valid;
        UINT32 iso_lba = mBootImageStartLba + (lba / 4);
        UINT32 off = (lba & 3) * 512;

        if (!cached_valid || cached_iso_lba != iso_lba) {
            if (!storage_read_blocks(&mBootStorageDevice, sec, iso_lba, 1)) {
                return 0;
            }
            cached_iso_lba = iso_lba;
            cached_valid = 1;
        }
        fw_copy_mem_fast(buf, sec + off, 512);
    }
    return 1;
}

static BOOLEAN fw_read_512s(UINT8 *buf, UINT32 lba, UINT32 count)
{
    UINT32 done = 0;

    if (count == 0) {
        return 1;
    }
    if (!storage_present(&mBootStorageDevice)) {
        return 0;
    }

    if (!storage_is_cd(&mBootStorageDevice)) {
        if (storage_block_size(&mBootStorageDevice) != 512U ||
            (UINT64)lba + count - 1U > storage_last_lba(&mBootStorageDevice)) {
            return 0;
        }
        while (done < count) {
            UINT32 chunk = count - done;

            if (chunk > 255) {
                chunk = 255;
            }
            if (!storage_read_blocks(&mBootStorageDevice, buf + done * 512,
                                     lba + done, chunk)) {
                return 0;
            }
            done += chunk;
        }
        return 1;
    }

    if (!atapi_configure_el_torito()) {
        return 0;
    }
    if (lba >= mBootImagePartitionBlocks ||
        (UINT64)count - 1U > (UINT64)mBootImagePartitionBlocks - lba - 1U) {
        return 0;
    }

    while (done < count) {
        UINT32 block = lba + done;
        UINT32 remaining = count - done;

        if ((block & 3U) == 0 && remaining >= 4) {
            UINT32 cd_count = remaining / 4U;

            if (!storage_read_blocks(&mBootStorageDevice, buf + done * 512,
                                     mBootImageStartLba + (block / 4U),
                                     cd_count)) {
                return 0;
            }
            done += cd_count * 4U;
            continue;
        }

        if (!fw_read_512(buf + done * 512, block)) {
            return 0;
        }
        done++;
    }
    return 1;
}

static FW_PARTITION_RECORD *partition_from_block_io(
    EFI_BLOCK_IO_PROTOCOL *This)
{
    UINTN i;

    for (i = 0; i < FW_ARRAY_SIZE(mPartitions); i++) {
        if (mPartitions[i].in_use && &mPartitions[i].block_io == This) {
            return &mPartitions[i];
        }
    }
    return NULL;
}

static FW_PARTITION_RECORD *partition_from_disk_io(
    EFI_DISK_IO_PROTOCOL *This)
{
    UINTN i;

    for (i = 0; i < FW_ARRAY_SIZE(mPartitions); i++) {
        if (mPartitions[i].in_use && &mPartitions[i].disk_io == This) {
            return &mPartitions[i];
        }
    }
    return NULL;
}

static FW_STORAGE_DEVICE *block_io_storage_device(EFI_BLOCK_IO_PROTOCOL *This)
{
    FW_PARTITION_RECORD *partition = partition_from_block_io(This);

    if (partition != NULL) {
        return block_io_storage_device(partition->parent_block_io);
    }
    if (This == &mDiskBlockIoProto) {
        return &mDiskStorageDevice;
    }
    if (This == &mRawBlockIoProto) {
        return &mRawStorageDevice;
    }
    return &mBootStorageDevice;
}

static VOID block_io_sync_media(EFI_BLOCK_IO_PROTOCOL *This,
                                BOOLEAN ForceMediaChange)
{
    FW_STORAGE_DEVICE *device;
    EFI_BLOCK_IO_MEDIA old_media;

    if (This == NULL || This->Media == NULL) {
        return;
    }
    device = block_io_storage_device(This);
    old_media = *This->Media;
    This->Media->MediaPresent = storage_present(device);
    This->Media->RemovableMedia = storage_removable(device);
    This->Media->ReadOnly = storage_read_only(device);
    This->Media->WriteCaching = storage_write_caching(device);
    if (This == &mBlockIoProto && mBootImageMapped) {
        This->Media->BlockSize = 512U;
        This->Media->LastBlock = mBootImagePartitionBlocks > 0 ?
            mBootImagePartitionBlocks - 1U : 0;
    } else {
        This->Media->BlockSize = storage_block_size(device);
        This->Media->LastBlock = storage_last_lba(device);
    }
    if (ForceMediaChange ||
        This->Media->MediaPresent != old_media.MediaPresent ||
        This->Media->BlockSize != old_media.BlockSize ||
        This->Media->LastBlock != old_media.LastBlock ||
        This->Media->ReadOnly != old_media.ReadOnly) {
        This->Media->MediaId++;
    }
}

EFI_STATUS blk_reset(EFI_BLOCK_IO_PROTOCOL *This,
                     BOOLEAN ExtendedVerification)
{
    FW_PARTITION_RECORD *partition;
    FW_STORAGE_DEVICE *device;

    if (This == NULL || This->Media == NULL) {
        return EFI_INVALID_PARAMETER;
    }
    partition = partition_from_block_io(This);
    if (partition != NULL) {
        EFI_STATUS st = partition->parent_block_io->Reset(
            partition->parent_block_io, ExtendedVerification);

        if (st != EFI_SUCCESS) {
            return EFI_DEVICE_ERROR;
        }
        partition->media.RemovableMedia =
            partition->parent_block_io->Media->RemovableMedia;
        partition->media.MediaPresent =
            partition->parent_block_io->Media->MediaPresent;
        partition->media.ReadOnly =
            partition->parent_block_io->Media->ReadOnly;
        partition->media.WriteCaching =
            partition->parent_block_io->Media->WriteCaching;
        partition->media.IoAlign = partition->parent_block_io->Media->IoAlign;
        partition->media.MediaId = partition->parent_block_io->Media->MediaId;
        return EFI_SUCCESS;
    }
    device = block_io_storage_device(This);
    if (!storage_device_present(device)) {
        This->Media->MediaPresent = 0;
        return EFI_DEVICE_ERROR;
    }
    if (!storage_reset(device, ExtendedVerification)) {
        return EFI_DEVICE_ERROR;
    }
    block_io_sync_media(This, 0);
    return EFI_SUCCESS;
}

static EFI_STATUS blk_validate_transfer(EFI_BLOCK_IO_PROTOCOL *This,
                                        UINT32 MediaId, UINT64 Lba,
                                        UINTN BufferSize, VOID *Buffer,
                                        EFI_BLOCK_IO_MEDIA **Media,
                                        FW_STORAGE_DEVICE **Device,
                                        UINT32 *BlockCount)
{
    EFI_BLOCK_IO_MEDIA *media;
    FW_STORAGE_DEVICE *dev;
    FW_PARTITION_RECORD *partition;
    UINTN block_size;

    if (This == NULL || This->Media == NULL) {
        return EFI_INVALID_PARAMETER;
    }
    media = This->Media;
    dev = block_io_storage_device(This);
    partition = partition_from_block_io(This);
    if (dev != NULL && storage_device_present(dev) &&
        storage_removable(dev) && !media->MediaPresent) {
        if (partition != NULL && partition->parent_block_io != NULL) {
            (void)partition->parent_block_io->Reset(
                partition->parent_block_io, 0);
            media->MediaPresent =
                partition->parent_block_io->Media->MediaPresent;
            media->MediaId = partition->parent_block_io->Media->MediaId;
        } else if (storage_refresh_media(dev)) {
            block_io_sync_media(This, 0);
        }
    }
    if (dev == NULL || !storage_device_present(dev) ||
        !storage_present(dev) || !media->MediaPresent) {
        return EFI_NO_MEDIA;
    }
    if (MediaId != media->MediaId) {
        return EFI_MEDIA_CHANGED;
    }
    if (BufferSize == 0) {
        if (Media != NULL) {
            *Media = media;
        }
        if (Device != NULL) {
            *Device = dev;
        }
        if (BlockCount != NULL) {
            *BlockCount = 0;
        }
        return EFI_SUCCESS;
    }
    if (Buffer == NULL || media->BlockSize == 0) {
        return EFI_INVALID_PARAMETER;
    }
    if ((media->IoAlign > 1U) && (((UINTN)Buffer % media->IoAlign) != 0)) {
        return EFI_INVALID_PARAMETER;
    }

    block_size = media->BlockSize;
    if ((BufferSize % block_size) != 0) {
        return EFI_BAD_BUFFER_SIZE;
    }
    if ((BufferSize / block_size) > 0xffffffffULL) {
        return EFI_INVALID_PARAMETER;
    }
    if (BlockCount != NULL) {
        *BlockCount = (UINT32)(BufferSize / block_size);
    }
    if (Lba > media->LastBlock ||
        (UINT64)(BufferSize / block_size) - 1U > media->LastBlock - Lba) {
        return EFI_INVALID_PARAMETER;
    }
    if (dev->Kind != FW_STORAGE_AHCI &&
        (Lba > 0xffffffffULL ||
         (UINT64)(BufferSize / block_size) - 1U >
             0xffffffffULL - Lba)) {
        return EFI_INVALID_PARAMETER;
    }
    if (Media != NULL) {
        *Media = media;
    }
    if (Device != NULL) {
        *Device = dev;
    }
    return EFI_SUCCESS;
}

static EFI_STATUS block_io_removable_failure(EFI_BLOCK_IO_PROTOCOL *This,
                                             FW_STORAGE_DEVICE *Device)
{
    if (!storage_removable(Device) || !storage_refresh_media(Device)) {
        return EFI_DEVICE_ERROR;
    }
    block_io_sync_media(This, 1);
    return This->Media->MediaPresent ? EFI_MEDIA_CHANGED : EFI_NO_MEDIA;
}

EFI_STATUS blk_read(EFI_BLOCK_IO_PROTOCOL *This, UINT32 MediaId,
                            UINT64 Lba, UINTN BufferSize, VOID *Buffer)
{
    FW_PARTITION_RECORD *partition;
    EFI_BLOCK_IO_MEDIA *media;
    FW_STORAGE_DEVICE *dev;
    UINT8 *buf = (UINT8 *)Buffer;
    UINT32 block_count;
    EFI_STATUS st;

    st = blk_validate_transfer(This, MediaId, Lba, BufferSize, Buffer,
                               &media, &dev, &block_count);
    if (st != EFI_SUCCESS || BufferSize == 0) {
        return st;
    }

    partition = partition_from_block_io(This);
    if (partition != NULL) {
        if (partition->parent_block_io == NULL ||
            Lba > ~0ULL - partition->start_lba) {
            return EFI_DEVICE_ERROR;
        }
        return partition->parent_block_io->ReadBlocks(
            partition->parent_block_io,
            partition->parent_block_io->Media->MediaId,
            partition->start_lba + Lba, BufferSize, Buffer);
    } else if (!media->LogicalPartition) {
        if (!storage_read_blocks(dev, buf, Lba, block_count)) {
            return block_io_removable_failure(This, dev);
        }
    } else if (media->BlockSize == 512) {
        if (!fw_read_512s(buf, (UINT32)Lba, block_count)) {
            return block_io_removable_failure(This, dev);
        }
    } else {
        return EFI_INVALID_PARAMETER;
    }
    return EFI_SUCCESS;
}

EFI_STATUS blk_write(EFI_BLOCK_IO_PROTOCOL *This, UINT32 MediaId,
                             UINT64 Lba, UINTN BufferSize, VOID *Buffer)
{
    FW_PARTITION_RECORD *partition;
    EFI_BLOCK_IO_MEDIA *media;
    FW_STORAGE_DEVICE *dev;
    UINT32 block_count;
    EFI_STATUS st;

    st = blk_validate_transfer(This, MediaId, Lba, BufferSize, Buffer,
                               &media, &dev, &block_count);
    if (st != EFI_SUCCESS || BufferSize == 0) {
        return st;
    }
    if (media->ReadOnly || storage_is_cd(dev)) {
        return EFI_WRITE_PROTECTED;
    }
    partition = partition_from_block_io(This);
    if (partition != NULL) {
        if (partition->parent_block_io == NULL ||
            Lba > ~0ULL - partition->start_lba) {
            return EFI_DEVICE_ERROR;
        }
        st = partition->parent_block_io->WriteBlocks(
            partition->parent_block_io,
            partition->parent_block_io->Media->MediaId,
            partition->start_lba + Lba, BufferSize, Buffer);
        if (st == EFI_SUCCESS) {
            storage_invalidate_cache(dev);
        }
        return st;
    }
    if (!storage_write_blocks(dev, (const UINT8 *)Buffer, Lba,
                              block_count)) {
        return block_io_removable_failure(This, dev);
    }
    storage_invalidate_cache(dev);
    return EFI_SUCCESS;
}

EFI_STATUS blk_flush(EFI_BLOCK_IO_PROTOCOL *This)
{
    FW_STORAGE_DEVICE *device;

    if (This == NULL || This->Media == NULL) {
        return EFI_INVALID_PARAMETER;
    }
    device = block_io_storage_device(This);
    if (!storage_present(device)) {
        return EFI_NO_MEDIA;
    }
    if (This->Media->ReadOnly || !This->Media->WriteCaching) {
        return EFI_SUCCESS;
    }
    return storage_flush(device) ? EFI_SUCCESS : EFI_DEVICE_ERROR;
}

BOOLEAN block_io_read_selftest(void)
{
    UINT8 *buf = mDiskIoScratch;
    EFI_BLOCK_IO_MEDIA *media = mBlockIoProto.Media;
    UINT64 blocks_to_read;
    UINTN valid_size;
    EFI_STATUS st;

    if (media == NULL || media->BlockSize == 0) {
        return 0;
    }

    st = mBlockIoProto.ReadBlocks(&mBlockIoProto, media->MediaId + 1U,
                                  0, media->BlockSize, buf);
    if (storage_present(&mBootStorageDevice) && media->MediaPresent) {
        if (st != EFI_MEDIA_CHANGED) {
            return 0;
        }
    } else if (st != EFI_NO_MEDIA) {
        return 0;
    }

    if (!storage_present(&mBootStorageDevice) || !media->MediaPresent) {
        return 1;
    }

    if (mBlockIoProto.ReadBlocks(&mBlockIoProto, media->MediaId, 0, 1,
                                 buf) != EFI_BAD_BUFFER_SIZE) {
        return 0;
    }
    if (mBlockIoProto.ReadBlocks(&mBlockIoProto, media->MediaId, 0,
                                 media->BlockSize,
                                 NULL) != EFI_INVALID_PARAMETER) {
        return 0;
    }
    if (mBlockIoProto.ReadBlocks(&mBlockIoProto, media->MediaId,
                                 media->LastBlock + 1U, media->BlockSize,
                                 buf) != EFI_INVALID_PARAMETER) {
        return 0;
    }

    blocks_to_read = 1;
    if (media->LastBlock >= 3U &&
        (UINT64)media->BlockSize * 4U <= sizeof(mDiskIoScratch)) {
        blocks_to_read = 4;
    } else if (media->LastBlock >= 1U &&
               (UINT64)media->BlockSize * 2U <= sizeof(mDiskIoScratch)) {
        blocks_to_read = 2;
    }
    valid_size = (UINTN)(blocks_to_read * media->BlockSize);
    if (mBlockIoProto.ReadBlocks(&mBlockIoProto, media->MediaId, 0,
                                 valid_size, buf) != EFI_SUCCESS) {
        return 0;
    }

    if (storage_is_cd(&mBootStorageDevice) && mRawBlockIoProto.Media != NULL) {
        EFI_BLOCK_IO_MEDIA *raw = mRawBlockIoProto.Media;

        if (mRawBlockIoProto.ReadBlocks(&mRawBlockIoProto,
                                        raw->MediaId + 1U, 16,
                                        raw->BlockSize,
                                        buf) != EFI_MEDIA_CHANGED) {
            return 0;
        }
        if (raw->LastBlock < 23U ||
            mRawBlockIoProto.ReadBlocks(&mRawBlockIoProto, raw->MediaId,
                                        16, raw->BlockSize * 8U,
                                        buf) != EFI_SUCCESS) {
            return 0;
        }
    }

    return 1;
}

BOOLEAN disk_block_io_selftest(void)
{
    UINT8 *buf = mDiskIoScratch;
    EFI_BLOCK_IO_MEDIA *media = mDiskBlockIoProto.Media;

    if (mDiskBlockIoHandle == NULL) {
        return 1;
    }
    if (media == NULL || !media->MediaPresent || media->RemovableMedia ||
        media->ReadOnly || media->BlockSize == 0 ||
        media->BlockSize > sizeof(mDiskIoScratch) ||
        !media->WriteCaching) {
        return 0;
    }
    if (mDiskBlockIoProto.ReadBlocks(&mDiskBlockIoProto, media->MediaId + 1U,
                                     0, media->BlockSize, buf) !=
        EFI_MEDIA_CHANGED) {
        return 0;
    }
    if (mDiskBlockIoProto.WriteBlocks(&mDiskBlockIoProto, media->MediaId,
                                      0, 0, NULL) != EFI_SUCCESS) {
        return 0;
    }
    if (mDiskIoProto.WriteDisk(&mDiskIoProto, media->MediaId,
                               0, 0, NULL) != EFI_SUCCESS) {
        return 0;
    }
    if (mDiskBlockIoProto.WriteBlocks(&mDiskBlockIoProto, media->MediaId,
                                      0, 1, buf) != EFI_BAD_BUFFER_SIZE) {
        return 0;
    }
    if (mDiskBlockIoProto.ReadBlocks(&mDiskBlockIoProto, media->MediaId,
                                     0, media->BlockSize, buf) != EFI_SUCCESS ||
        mDiskBlockIoProto.FlushBlocks(&mDiskBlockIoProto) != EFI_SUCCESS) {
        return 0;
    }
    return 1;
}

static EFI_BLOCK_IO_PROTOCOL *disk_io_block_proto(EFI_DISK_IO_PROTOCOL *This)
{
    FW_PARTITION_RECORD *partition = partition_from_disk_io(This);

    if (partition != NULL) {
        return &partition->block_io;
    }
    if (This == &mRawDiskIoProto) {
        return &mRawBlockIoProto;
    }
    if (This == &mDiskIoProto) {
        return &mDiskBlockIoProto;
    }
    return &mBlockIoProto;
}

static UINTN disk_io_aligned_span(UINTN Remaining, UINTN BlockSize)
{
    UINTN span = Remaining - (Remaining % BlockSize);

    if ((span / BlockSize) > 0xffffffffULL) {
        span = (UINTN)0xffffffffULL * BlockSize;
    }
    return span;
}

EFI_STATUS disk_read(EFI_DISK_IO_PROTOCOL *This, UINT32 MediaId,
                     UINT64 Offset, UINTN BufferSize, VOID *Buffer)
{
    EFI_BLOCK_IO_PROTOCOL *block = disk_io_block_proto(This);
    EFI_BLOCK_IO_MEDIA *media;
    UINT8 *dst = (UINT8 *)Buffer;
    UINT64 media_size;
    UINTN remaining = BufferSize;

    if (block == NULL || block->Media == NULL) {
        return EFI_INVALID_PARAMETER;
    }
    media = block->Media;
    if (BufferSize != 0 && Buffer == NULL) {
        return EFI_INVALID_PARAMETER;
    }
    if (MediaId != media->MediaId) {
        return EFI_MEDIA_CHANGED;
    }
    if (BufferSize == 0) {
        return EFI_SUCCESS;
    }
    if (media->BlockSize == 0 ||
        media->BlockSize > sizeof(mDiskIoScratch) ||
        media->LastBlock == ~0ULL ||
        media->LastBlock + 1U > ~0ULL / media->BlockSize) {
        return EFI_BAD_BUFFER_SIZE;
    }

    media_size = (media->LastBlock + 1) * (UINT64)media->BlockSize;
    if (Offset >= media_size || (UINT64)BufferSize > media_size - Offset) {
        return EFI_INVALID_PARAMETER;
    }

    while (remaining != 0) {
        UINT64 lba = Offset / media->BlockSize;
        UINTN block_offset = (UINTN)(Offset % media->BlockSize);
        UINTN chunk = media->BlockSize - block_offset;
        EFI_STATUS st;

        if (block_offset == 0 && remaining >= media->BlockSize) {
            chunk = disk_io_aligned_span(remaining, media->BlockSize);
            st = block->ReadBlocks(block, MediaId, lba, chunk, dst);
        } else {
            if (chunk > remaining) {
                chunk = remaining;
            }
            st = block->ReadBlocks(block, MediaId, lba,
                                   media->BlockSize, mDiskIoScratch);
            if (st == EFI_SUCCESS) {
                fw_copy_mem(dst, mDiskIoScratch + block_offset, chunk);
            }
        }
        if (st != EFI_SUCCESS) {
            return st;
        }
        Offset += chunk;
        dst += chunk;
        remaining -= chunk;
    }
    return EFI_SUCCESS;
}

EFI_STATUS disk_write(EFI_DISK_IO_PROTOCOL *This, UINT32 MediaId,
                      UINT64 Offset, UINTN BufferSize, VOID *Buffer)
{
    EFI_BLOCK_IO_PROTOCOL *block = disk_io_block_proto(This);
    EFI_BLOCK_IO_MEDIA *media;
    UINT8 *src = (UINT8 *)Buffer;
    UINT64 media_size;
    UINTN remaining = BufferSize;

    if (block == NULL || block->Media == NULL) {
        return EFI_INVALID_PARAMETER;
    }
    media = block->Media;
    if (BufferSize != 0 && Buffer == NULL) {
        return EFI_INVALID_PARAMETER;
    }
    if (MediaId != media->MediaId) {
        return EFI_MEDIA_CHANGED;
    }
    if (BufferSize == 0) {
        return EFI_SUCCESS;
    }
    if (media->BlockSize == 0 ||
        media->BlockSize > sizeof(mDiskIoScratch) ||
        media->LastBlock == ~0ULL ||
        media->LastBlock + 1U > ~0ULL / media->BlockSize) {
        return EFI_BAD_BUFFER_SIZE;
    }

    media_size = (media->LastBlock + 1) * (UINT64)media->BlockSize;
    if (Offset >= media_size || (UINT64)BufferSize > media_size - Offset) {
        return EFI_INVALID_PARAMETER;
    }

    while (remaining != 0) {
        UINT64 lba = Offset / media->BlockSize;
        UINTN block_offset = (UINTN)(Offset % media->BlockSize);
        UINTN chunk = media->BlockSize - block_offset;
        EFI_STATUS st;

        if (block_offset == 0 && remaining >= media->BlockSize) {
            chunk = disk_io_aligned_span(remaining, media->BlockSize);
            st = block->WriteBlocks(block, MediaId, lba, chunk, src);
        } else {
            if (chunk > remaining) {
                chunk = remaining;
            }
            st = block->ReadBlocks(block, MediaId, lba,
                                   media->BlockSize, mDiskIoScratch);
            if (st == EFI_SUCCESS) {
                fw_copy_mem(mDiskIoScratch + block_offset, src, chunk);
                st = block->WriteBlocks(block, MediaId, lba,
                                        media->BlockSize, mDiskIoScratch);
            }
        }
        if (st != EFI_SUCCESS) {
            return st;
        }
        Offset += chunk;
        src += chunk;
        remaining -= chunk;
    }
    return EFI_SUCCESS;
}

/* --- EFI Simple File System backed by the El Torito FAT image ------------- */

const UINT8 mFileInfoGuid[16] = {
    0x92, 0x6e, 0x57, 0x09, 0x3f, 0x6d, 0xd2, 0x11,
    0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b
};

const UINT8 mFileSystemInfoGuid[16] = {
    0x93, 0x6e, 0x57, 0x09, 0x3f, 0x6d, 0xd2, 0x11,
    0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b
};

static const UINT8 mFileSystemVolumeLabelGuid[16] = {
    0xd3, 0xd7, 0x47, 0xdb, 0x81, 0xfe, 0xd3, 0x11,
    0x9a, 0x35, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d
};

FW_STATIC_ASSERT(__builtin_offsetof(FW_EFI_FILE_INFO, FileName) == 80,
                 efi_file_info_name_offset);
FW_STATIC_ASSERT(__builtin_offsetof(FW_EFI_FILE_SYSTEM_INFO,
                                    VolumeLabel) == 36,
                 efi_file_system_info_label_offset);

/* Filesystem types live in fw-fs.h. */

static FW_FAT_VOLUME mBootFatVolume;
static FW_FAT_VOLUME mPartitionFatVolumes[FW_PARTITION_MAX];
FW_FAT_VOLUME *mDefaultFatVolume;
static BOOLEAN mBootFatChecked;
static FW_ISO_VOLUME mIsoVolume;
FW_UDF_VOLUME mUdfVolume;
static FW_FILE mFileHandles[FW_FILE_MAX];
EFI_SIMPLE_FILE_SYSTEM_PROTOCOL mSimpleFsProto;
EFI_SIMPLE_FILE_SYSTEM_PROTOCOL mOpticalSimpleFsProto;
EFI_STATUS fat_open_volume(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *This,
                                  EFI_FILE_HANDLE *Root);

static VOID fw_padded_ascii_to_char16(const UINT8 *Source, UINTN SourceSize,
                                      CHAR16 *Destination,
                                      UINTN DestinationChars)
{
    UINTN end = SourceSize;
    UINTN i;

    if (Source == NULL || Destination == NULL || DestinationChars == 0) {
        return;
    }
    while (end > 0 && (Source[end - 1U] == ' ' || Source[end - 1U] == 0)) {
        end--;
    }
    if (end >= DestinationChars) {
        end = DestinationChars - 1U;
    }
    for (i = 0; i < end; i++) {
        Destination[i] = (CHAR16)Source[i];
    }
    Destination[end] = 0;
}

/* FW_CDROM_DEVICE_PATH_NODE lives in fw-fs.h. */

typedef struct {
    FW_DEVICE_PATH_NODE Header;
    UINT32 PartitionNumber;
    UINT64 PartitionStart;
    UINT64 PartitionSize;
    UINT8 PartitionSignature[16];
    UINT8 MbrType;
    UINT8 SignatureType;
} __attribute__((packed)) FW_HARD_DRIVE_DEVICE_PATH_NODE;

FW_STATIC_ASSERT(sizeof(FW_HARD_DRIVE_DEVICE_PATH_NODE) == 42U,
                 hard_drive_device_path_size);

#define FW_IA64_ABI_HARD_DRIVE_DEVICE_PATH_SIZE 48U

/* FW_ATAPI_DEVICE_PATH_NODE lives in fw-fs.h. */

/* FW_SATA_DEVICE_PATH_NODE lives in fw-fs.h. */

/* FW_BOOT_FULL_DEVICE_PATH lives in fw-fs.h. */

/* FW_BLOCK_DEVICE_PATH lives in fw-fs.h. */

/* FW_RAW_BLOCK_DEVICE_PATH lives in fw-fs.h. */

/* FW_SATA_RAW_BLOCK_DEVICE_PATH lives in fw-fs.h. */

/* FW_SATA_BLOCK_DEVICE_PATH lives in fw-fs.h. */

/* FW_GRAPHICS_DEVICE_PATH lives in fw-fs.h. */

/* FW_CONSOLE_OUTPUT_DEVICE_PATH lives in fw-fs.h. */

FW_STATIC_ASSERT(sizeof(FW_CONSOLE_OUTPUT_DEVICE_PATH) == 57U,
                 console_output_device_path_size);

/* FW_PCI_ROOT_BRIDGE_DEVICE_PATH lives in fw-fs.h. */

/* FW_PCI_CONTROLLER_DEVICE_PATH lives in fw-fs.h. */

#define FW_PCI_CONTROLLER_DEVICE_PATH_INIT(DeviceNumber) \
    { \
        .Acpi = { \
            .Header = { \
                .Type = 0x02, \
                .SubType = 0x01, \
                .Length = sizeof(FW_ACPI_HID_DEVICE_PATH_NODE), \
            }, \
            .Hid = 0x0A0341D0, \
            .Uid = 0, \
        }, \
        .Pci = { \
            .Header = { \
                .Type = 0x01, \
                .SubType = 0x01, \
                .Length = sizeof(FW_PCI_DEVICE_PATH_NODE), \
            }, \
            .Function = 0, \
            .Device = (DeviceNumber), \
        }, \
        .End = { \
            .Type = 0x7f, \
            .SubType = 0xff, \
            .Length = 4, \
        }, \
    }

FW_DEVICE_PATH_NODE mEndDevicePath FW_DEVICE_PATH_GUEST_ALIGN = {
    .Type = 0x7f,
    .SubType = 0xff,
    .Length = 4,
};

FW_PCI_ROOT_BRIDGE_DEVICE_PATH FW_DEVICE_PATH_GUEST_ALIGN mPciRootBridgeDevicePath = {
    .Acpi = {
        .Header = {
            .Type = 0x02,
            .SubType = 0x01,
            .Length = sizeof(FW_ACPI_HID_DEVICE_PATH_NODE),
        },
        .Hid = 0x0A0341D0,
        .Uid = 0,
    },
    .End = {
        .Type = 0x7f,
        .SubType = 0xff,
        .Length = 4,
    },
};

FW_PCI_CONTROLLER_DEVICE_PATH FW_DEVICE_PATH_GUEST_ALIGN mPciIdeDevicePath =
    FW_PCI_CONTROLLER_DEVICE_PATH_INIT(0);
FW_PCI_CONTROLLER_DEVICE_PATH FW_DEVICE_PATH_GUEST_ALIGN mPciAhciDevicePath =
    FW_PCI_CONTROLLER_DEVICE_PATH_INIT(1);
FW_PCI_CONTROLLER_DEVICE_PATH FW_DEVICE_PATH_GUEST_ALIGN mPciOhciDevicePath =
    FW_PCI_CONTROLLER_DEVICE_PATH_INIT(2);
FW_PCI_CONTROLLER_DEVICE_PATH FW_DEVICE_PATH_GUEST_ALIGN mPciUhciDevicePath =
    FW_PCI_CONTROLLER_DEVICE_PATH_INIT(3);
FW_PCI_CONTROLLER_DEVICE_PATH FW_DEVICE_PATH_GUEST_ALIGN mPciLsiDevicePath =
    FW_PCI_CONTROLLER_DEVICE_PATH_INIT(4);

FW_BLOCK_DEVICE_PATH FW_DEVICE_PATH_GUEST_ALIGN mBlockDevicePath = {
    .Acpi = {
        .Header = {
            .Type = 0x02,
            .SubType = 0x01,
            .Length = sizeof(FW_ACPI_HID_DEVICE_PATH_NODE),
        },
        .Hid = 0x0A0341D0,
        .Uid = 0,
    },
    .Pci = {
        .Header = {
            .Type = 0x01,
            .SubType = 0x01,
            .Length = sizeof(FW_PCI_DEVICE_PATH_NODE),
        },
        .Function = 0,
        .Device = 4,
    },
    .Atapi = {
        .Header = {
            .Type = 0x03,
            .SubType = 0x02,
            .Length = sizeof(FW_ATAPI_DEVICE_PATH_NODE),
        },
        .PrimarySecondary = 0,
        .SlaveMaster = 0,
        .Lun = 0,
    },
    .Cdrom = {
        .Header = {
            .Type = 0x04,
            .SubType = 0x02,
            .Length = sizeof(FW_CDROM_DEVICE_PATH_NODE),
        },
        .BootEntry = 0,
        .PartitionStart = 0,
        .PartitionSize = 0,
    },
    .End = {
        .Type = 0x7f,
        .SubType = 0xff,
        .Length = 4,
    },
};

FW_RAW_BLOCK_DEVICE_PATH FW_DEVICE_PATH_GUEST_ALIGN mRawBlockDevicePath = {
    .Acpi = {
        .Header = {
            .Type = 0x02,
            .SubType = 0x01,
            .Length = sizeof(FW_ACPI_HID_DEVICE_PATH_NODE),
        },
        .Hid = 0x0A0341D0,
        .Uid = 0,
    },
    .Pci = {
        .Header = {
            .Type = 0x01,
            .SubType = 0x01,
            .Length = sizeof(FW_PCI_DEVICE_PATH_NODE),
        },
        .Function = 0,
        .Device = 4,
    },
    .Atapi = {
        .Header = {
            .Type = 0x03,
            .SubType = 0x02,
            .Length = sizeof(FW_ATAPI_DEVICE_PATH_NODE),
        },
        .PrimarySecondary = 0,
        .SlaveMaster = 0,
        .Lun = 0,
    },
    .End = {
        .Type = 0x7f,
        .SubType = 0xff,
        .Length = 4,
    },
};

FW_RAW_BLOCK_DEVICE_PATH FW_DEVICE_PATH_GUEST_ALIGN mDiskBlockDevicePath = {
    .Acpi = {
        .Header = {
            .Type = 0x02,
            .SubType = 0x01,
            .Length = sizeof(FW_ACPI_HID_DEVICE_PATH_NODE),
        },
        .Hid = 0x0A0341D0,
        .Uid = 0,
    },
    .Pci = {
        .Header = {
            .Type = 0x01,
            .SubType = 0x01,
            .Length = sizeof(FW_PCI_DEVICE_PATH_NODE),
        },
        .Function = 0,
        .Device = 4,
    },
    .Atapi = {
        .Header = {
            .Type = 0x03,
            .SubType = 0x02,
            .Length = sizeof(FW_ATAPI_DEVICE_PATH_NODE),
        },
        .PrimarySecondary = 1,
        .SlaveMaster = 0,
        .Lun = 0,
    },
    .End = {
        .Type = 0x7f,
        .SubType = 0xff,
        .Length = 4,
    },
};

#define FW_SATA_RAW_BLOCK_DEVICE_PATH_INIT \
    { \
        .Acpi = { \
            .Header = { 0x02, 0x01, sizeof(FW_ACPI_HID_DEVICE_PATH_NODE) }, \
            .Hid = 0x0A0341D0, \
            .Uid = 0, \
        }, \
        .Pci = { \
            .Header = { 0x01, 0x01, sizeof(FW_PCI_DEVICE_PATH_NODE) }, \
            .Function = 0, \
            .Device = 1, \
        }, \
        .Sata = { \
            .Header = { 0x03, 0x12, sizeof(FW_SATA_DEVICE_PATH_NODE) }, \
            .HbaPort = 0, \
            .PortMultiplierPort = 0xffffU, \
            .Lun = 0, \
        }, \
        .End = { 0x7f, 0xff, sizeof(FW_DEVICE_PATH_NODE) }, \
    }

FW_SATA_RAW_BLOCK_DEVICE_PATH FW_DEVICE_PATH_GUEST_ALIGN mSataBootDevicePath =
    FW_SATA_RAW_BLOCK_DEVICE_PATH_INIT;
FW_SATA_RAW_BLOCK_DEVICE_PATH FW_DEVICE_PATH_GUEST_ALIGN mSataDiskDevicePath =
    FW_SATA_RAW_BLOCK_DEVICE_PATH_INIT;
FW_SATA_RAW_BLOCK_DEVICE_PATH FW_DEVICE_PATH_GUEST_ALIGN mSataRawDevicePath =
    FW_SATA_RAW_BLOCK_DEVICE_PATH_INIT;
FW_SATA_BLOCK_DEVICE_PATH FW_DEVICE_PATH_GUEST_ALIGN mSataBlockDevicePath = {
    .Acpi = {
        .Header = { 0x02, 0x01, sizeof(FW_ACPI_HID_DEVICE_PATH_NODE) },
        .Hid = 0x0A0341D0,
        .Uid = 0,
    },
    .Pci = {
        .Header = { 0x01, 0x01, sizeof(FW_PCI_DEVICE_PATH_NODE) },
        .Function = 0,
        .Device = 1,
    },
    .Sata = {
        .Header = { 0x03, 0x12, sizeof(FW_SATA_DEVICE_PATH_NODE) },
        .HbaPort = 0,
        .PortMultiplierPort = 0xffffU,
        .Lun = 0,
    },
    .Cdrom = {
        .Header = { 0x04, 0x02, sizeof(FW_CDROM_DEVICE_PATH_NODE) },
        .BootEntry = 0,
        .PartitionStart = 0,
        .PartitionSize = 0,
    },
    .End = { 0x7f, 0xff, sizeof(FW_DEVICE_PATH_NODE) },
};

#undef FW_SATA_RAW_BLOCK_DEVICE_PATH_INIT

FW_GRAPHICS_DEVICE_PATH FW_DEVICE_PATH_GUEST_ALIGN mGraphicsDevicePath = {
    .Acpi = {
        .Header = {
            .Type = 0x02,
            .SubType = 0x01,
            .Length = sizeof(FW_ACPI_HID_DEVICE_PATH_NODE),
        },
        .Hid = 0x0A0341D0,
        .Uid = 0,
    },
    .Pci = {
        .Header = {
            .Type = 0x01,
            .SubType = 0x01,
            .Length = sizeof(FW_PCI_DEVICE_PATH_NODE),
        },
        .Function = 0,
        .Device = 5,
    },
    .End = {
        .Type = 0x7f,
        .SubType = 0xff,
        .Length = 4,
    },
};

FW_CONSOLE_OUTPUT_DEVICE_PATH FW_DEVICE_PATH_GUEST_ALIGN mConsoleOutputDevicePath = {
    .Graphics = {
        .Acpi = {
            .Header = {
                .Type = 0x02,
                .SubType = 0x01,
                .Length = sizeof(FW_ACPI_HID_DEVICE_PATH_NODE),
            },
            .Hid = 0x0A0341D0,
            .Uid = 0,
        },
        .Pci = {
            .Header = {
                .Type = 0x01,
                .SubType = 0x01,
                .Length = sizeof(FW_PCI_DEVICE_PATH_NODE),
            },
            .Function = 0,
            .Device = 5,
        },
        .End = {
            .Type = 0x7f,
            .SubType = 0x01,
            .Length = sizeof(FW_DEVICE_PATH_NODE),
        },
    },
    .Serial = {
        .Acpi = {
            .Header = {
                .Type = 0x02,
                .SubType = 0x01,
                .Length = sizeof(FW_ACPI_HID_DEVICE_PATH_NODE),
            },
            .Hid = FW_UART_DEVICE_PATH_HID_PNP0501,
            .Uid = 0,
        },
        .Uart = {
            .Header = {
                .Type = 0x03,
                .SubType = 0x0e,
                .Length = sizeof(FW_UART_DEVICE_PATH_NODE),
            },
            .Reserved = 0,
            .BaudRate = 115200,
            .DataBits = 8,
            .Parity = NoParity,
            .StopBits = OneStopBit,
        },
        .End = {
            .Type = 0x7f,
            .SubType = 0xff,
            .Length = sizeof(FW_DEVICE_PATH_NODE),
        },
    },
};

/* FW_OPTICAL_SETUP_LOADER_DEVICE_PATH lives in fw-fs.h. */

/* FW_EFI_BOOT_OPTION lives in fw-fs.h. */

const CHAR16 mDefaultBootDescription[21] = {
    'R', 'e', 'm', 'o', 'v', 'a', 'b', 'l', 'e', ' ',
    'M', 'e', 'd', 'i', 'a', ' ', 'B', 'o', 'o', 't', 0,
};


FW_OPTICAL_SETUP_LOADER_DEVICE_PATH FW_DEVICE_PATH_GUEST_ALIGN mOpticalSetupLoaderDevicePath = {
    .Acpi = {
        .Header = {
            .Type = 0x02,
            .SubType = 0x01,
            .Length = sizeof(FW_ACPI_HID_DEVICE_PATH_NODE),
        },
        .Hid = 0x0A0341D0,
        .Uid = 0,
    },
    .Pci = {
        .Header = {
            .Type = 0x01,
            .SubType = 0x01,
            .Length = sizeof(FW_PCI_DEVICE_PATH_NODE),
        },
        .Function = 0,
        .Device = 4,
    },
    .Atapi = {
        .Header = {
            .Type = 0x03,
            .SubType = 0x02,
            .Length = sizeof(FW_ATAPI_DEVICE_PATH_NODE),
        },
        .PrimarySecondary = 0,
        .SlaveMaster = 0,
        .Lun = 0,
    },
    .Cdrom = {
        .Header = {
            .Type = 0x04,
            .SubType = 0x02,
            .Length = sizeof(FW_CDROM_DEVICE_PATH_NODE),
        },
        .BootEntry = 0,
        .PartitionStart = 0,
        .PartitionSize = 0,
    },
    .FileHeader = {
        .Type = 0x04,
        .SubType = 0x04,
        .Length = sizeof(FW_DEVICE_PATH_NODE) + 23 * sizeof(CHAR16),
    },
    .PathName = {
        '\\', 'E', 'F', 'I', '\\', 'B', 'O', 'O', 'T', '\\',
        'B', 'O', 'O', 'T', 'I', 'A', '6', '4', '.', 'E', 'F', 'I', 0
    },
    .End = {
        .Type = 0x7f,
        .SubType = 0xff,
        .Length = 4,
    },
};

/*
 * Where a storage controller sits, as an EFI device path pair: the ACPI _UID
 * of the PCI root that carries it, and its device number.  IDE and AHCI are
 * on the compatibility bus.  The SCSI HBA is on device 4 of that bus on zx1,
 * but on the i2000 it lives at device 0 of the first WXB expander root
 * (ACPI _UID IA64_460GX_WXB0_BUS), which is where the board carries its
 * QLogic adapter.
 */
static UINT8 fw_storage_pci_device(const FW_STORAGE_DEVICE *Device)
{
    if (Device != NULL && Device->Kind == FW_STORAGE_IDE) {
        return 0;
    }
    if (Device != NULL && Device->Kind == FW_STORAGE_AHCI) {
        return 1;
    }
    return fw_platform_is_zx1() ? 4 : IA64_460GX_WXB0_SCSI_SLOT;
}

static UINT32 fw_storage_pci_root_uid(const FW_STORAGE_DEVICE *Device)
{
    if (Device != NULL && (Device->Kind == FW_STORAGE_IDE ||
                           Device->Kind == FW_STORAGE_AHCI)) {
        return 0;
    }
    return fw_platform_is_zx1() ? 0 : IA64_460GX_WXB0_BUS;
}

static void fw_set_storage_path_node(FW_ATAPI_DEVICE_PATH_NODE *Node,
                                     const FW_STORAGE_DEVICE *Device)
{
    if (Node == NULL) {
        return;
    }

    Node->Header.Type = 0x03;
    Node->Header.Length = sizeof(*Node);
    Node->Lun = 0;

    if (Device != NULL && Device->Kind == FW_STORAGE_IDE &&
        Device->Ide != NULL) {
        Node->Header.SubType = 0x01; /* ATAPI */
        Node->PrimarySecondary = 0;
        Node->SlaveMaster = Device->Ide->unit;
        return;
    }

    Node->Header.SubType = 0x02; /* SCSI */
    if (Device != NULL && Device->Kind == FW_STORAGE_SCSI &&
        Device->Scsi != NULL) {
        Node->PrimarySecondary = Device->Scsi->target;
        Node->SlaveMaster = 0;
        Node->Lun = Device->Scsi->lun;
    } else {
        Node->PrimarySecondary = 0;
        Node->SlaveMaster = 0;
    }
}

static BOOLEAN fw_storage_path_node_matches(
    const FW_ATAPI_DEVICE_PATH_NODE *Node,
    const FW_STORAGE_DEVICE *Device)
{
    if (Node == NULL || Device == NULL ||
        Node->Header.Type != 0x03 ||
        Node->Header.Length != sizeof(*Node)) {
        return 0;
    }

    if (Device->Kind == FW_STORAGE_IDE && Device->Ide != NULL) {
        return Node->Header.SubType == 0x01 &&
               Node->PrimarySecondary == 0 &&
               Node->SlaveMaster == Device->Ide->unit &&
               Node->Lun == 0;
    }
    if (Device->Kind == FW_STORAGE_SCSI && Device->Scsi != NULL) {
        return Node->Header.SubType == 0x02 &&
               Node->PrimarySecondary == Device->Scsi->target &&
               Node->SlaveMaster == 0 &&
               Node->Lun == Device->Scsi->lun;
    }
    return 0;
}

FW_BOOT_FULL_DEVICE_PATH FW_DEVICE_PATH_GUEST_ALIGN mBootFullDevicePath = {
    .Acpi = {
        .Header = {
            .Type = 0x02,
            .SubType = 0x01,
            .Length = sizeof(FW_ACPI_HID_DEVICE_PATH_NODE),
        },
        .Hid = 0x0A0341D0,
        .Uid = 0,
    },
    .Pci = {
        .Header = {
            .Type = 0x01,
            .SubType = 0x01,
            .Length = sizeof(FW_PCI_DEVICE_PATH_NODE),
        },
        .Function = 0,
        .Device = 4,
    },
    .Atapi = {
        .Header = {
            .Type = 0x03,
            .SubType = 0x02,
            .Length = sizeof(FW_ATAPI_DEVICE_PATH_NODE),
        },
        .PrimarySecondary = 0,
        .SlaveMaster = 0,
        .Lun = 0,
    },
    .Cdrom = {
        .Header = {
            .Type = 0x04,
            .SubType = 0x02,
            .Length = sizeof(FW_CDROM_DEVICE_PATH_NODE),
        },
        .BootEntry = 0,
        .PartitionStart = 0,
        .PartitionSize = 0,
    },
    .FileHeader = {
        .Type = 0x04,
        .SubType = 0x04,
        .Length = sizeof(FW_DEVICE_PATH_NODE) + 23 * sizeof(CHAR16),
    },
    .PathName = {
        '\\', 'E', 'F', 'I', '\\', 'B', 'O', 'O', 'T', '\\',
        'B', 'O', 'O', 'T', 'I', 'A', '6', '4', '.', 'E', 'F', 'I', 0
    },
    .End = {
        .Type = 0x7f,
        .SubType = 0xff,
        .Length = 4,
    },
};

void fw_update_storage_device_paths(VOID)
{
    UINT8 boot_pci = fw_storage_pci_device(&mBootStorageDevice);
    UINT8 disk_pci = fw_storage_pci_device(&mDiskStorageDevice);
    UINT8 raw_pci = fw_storage_pci_device(&mRawStorageDevice);

    mBlockDevicePath.Acpi.Uid = fw_storage_pci_root_uid(&mBootStorageDevice);
    mBlockDevicePath.Pci.Device = boot_pci;
    fw_set_storage_path_node(&mBlockDevicePath.Atapi, &mBootStorageDevice);
    mRawBlockDevicePath.Acpi.Uid =
        fw_storage_pci_root_uid(&mRawStorageDevice);
    mRawBlockDevicePath.Pci.Device = raw_pci;
    fw_set_storage_path_node(&mRawBlockDevicePath.Atapi, &mRawStorageDevice);
    mBootFullDevicePath.Acpi.Uid =
        fw_storage_pci_root_uid(&mBootStorageDevice);
    mBootFullDevicePath.Pci.Device = boot_pci;
    fw_set_storage_path_node(&mBootFullDevicePath.Atapi, &mBootStorageDevice);

    mOpticalSetupLoaderDevicePath.Acpi.Uid =
        fw_storage_pci_root_uid(&mBootStorageDevice);
    mOpticalSetupLoaderDevicePath.Pci.Device = boot_pci;
    fw_set_storage_path_node(&mOpticalSetupLoaderDevicePath.Atapi,
                             &mBootStorageDevice);

    mDiskBlockDevicePath.Acpi.Uid =
        fw_storage_pci_root_uid(&mDiskStorageDevice);
    mDiskBlockDevicePath.Pci.Device = disk_pci;
    fw_set_storage_path_node(&mDiskBlockDevicePath.Atapi,
                             &mDiskStorageDevice);
    if (mBootStorageDevice.Kind == FW_STORAGE_AHCI &&
        mBootStorageDevice.Ahci != NULL) {
        mSataBootDevicePath.Sata.HbaPort = mBootStorageDevice.Ahci->port;
        mSataBlockDevicePath.Sata.HbaPort = mBootStorageDevice.Ahci->port;
    }
    if (mDiskStorageDevice.Kind == FW_STORAGE_AHCI &&
        mDiskStorageDevice.Ahci != NULL) {
        mSataDiskDevicePath.Sata.HbaPort = mDiskStorageDevice.Ahci->port;
    }
    if (mRawStorageDevice.Kind == FW_STORAGE_AHCI &&
        mRawStorageDevice.Ahci != NULL) {
        mSataRawDevicePath.Sata.HbaPort = mRawStorageDevice.Ahci->port;
    }
}

static BOOLEAN fw_device_path_is_end(const FW_DEVICE_PATH_NODE *node)
{
    return node != NULL && node->Type == 0x7f && node->SubType == 0xff;
}

static BOOLEAN fw_cdrom_node_is_whole_media(const FW_DEVICE_PATH_NODE *node)
{
    const FW_CDROM_DEVICE_PATH_NODE *cdrom;

    if (node == NULL || node->Type != 0x04 || node->SubType != 0x02 ||
        node->Length != sizeof(FW_CDROM_DEVICE_PATH_NODE)) {
        return 0;
    }

    cdrom = (const FW_CDROM_DEVICE_PATH_NODE *)node;
    return cdrom->BootEntry == 0 &&
           cdrom->PartitionStart == 0 &&
           cdrom->PartitionSize == mCdromBlocks;
}

static UINTN fw_device_path_prefix_length(const FW_DEVICE_PATH_NODE *prefix,
                                          const FW_DEVICE_PATH_NODE *path)
{
    const UINT8 *prefix_bytes = (const UINT8 *)prefix;
    const UINT8 *path_bytes = (const UINT8 *)path;
    UINTN matched = 0;

    while (!fw_device_path_is_end((const FW_DEVICE_PATH_NODE *)prefix_bytes)) {
        const FW_DEVICE_PATH_NODE *prefix_node =
            (const FW_DEVICE_PATH_NODE *)prefix_bytes;
        const FW_DEVICE_PATH_NODE *path_node =
            (const FW_DEVICE_PATH_NODE *)path_bytes;
        UINTN i;

        if (prefix_node->Length < sizeof(FW_DEVICE_PATH_NODE) ||
            path_node->Length != prefix_node->Length ||
            path_node->Type != prefix_node->Type ||
            path_node->SubType != prefix_node->SubType) {
            return 0;
        }
        for (i = 0; i < prefix_node->Length; i++) {
            if (prefix_bytes[i] != path_bytes[i]) {
                return 0;
            }
        }
        matched += prefix_node->Length;
        prefix_bytes += prefix_node->Length;
        path_bytes += path_node->Length;
    }

    return matched;
}

static BOOLEAN fw_hard_drive_path_node_supported(
    const FW_DEVICE_PATH_NODE *Node)
{
    if (Node == NULL || Node->Type != 0x04U || Node->SubType != 0x01U) {
        return 0;
    }

    /*
     * EFI 1.10 specifies a 42-byte packed node.  Early IA-64 software also
     * persisted the naturally aligned 48-byte ABI form, including its six
     * tail-padding bytes.  In either form the defined fields have identical
     * offsets; callers advance by the advertised length and ignore the tail.
     */
    return Node->Length == sizeof(FW_HARD_DRIVE_DEVICE_PATH_NODE) ||
           Node->Length == FW_IA64_ABI_HARD_DRIVE_DEVICE_PATH_SIZE;
}

static BOOLEAN fw_short_hard_drive_path_matches(
    const FW_HARD_DRIVE_DEVICE_PATH_NODE *HardDrive,
    const FW_PARTITION_RECORD *Partition)
{
    UINTN signature_size;
    UINTN i;

    if (HardDrive == NULL || Partition == NULL || !Partition->in_use ||
        !fw_hard_drive_path_node_supported(&HardDrive->Header) ||
        HardDrive->PartitionNumber != Partition->partition_number ||
        HardDrive->MbrType != Partition->mbr_type ||
        HardDrive->SignatureType != Partition->signature_type) {
        return 0;
    }

    /* Short-form matching is by partition signature and number, not geometry. */
    if (HardDrive->MbrType == 0x02U && HardDrive->SignatureType == 0x02U) {
        signature_size = sizeof(HardDrive->PartitionSignature);
    } else if (HardDrive->MbrType == 0x01U &&
               HardDrive->SignatureType == 0x01U) {
        signature_size = sizeof(UINT32);
    } else {
        return 0;
    }

    for (i = 0; i < signature_size; i++) {
        if (HardDrive->PartitionSignature[i] !=
            Partition->partition_signature[i]) {
            return 0;
        }
    }
    return 1;
}

static EFI_HANDLE fw_locate_short_hard_drive_path(
    const FW_DEVICE_PATH_NODE *Path, const void *Protocol)
{
    const FW_HARD_DRIVE_DEVICE_PATH_NODE *hard_drive;
    UINTN i;

    if (Protocol == NULL || !fw_hard_drive_path_node_supported(Path)) {
        return NULL;
    }
    hard_drive = (const FW_HARD_DRIVE_DEVICE_PATH_NODE *)Path;

    for (i = 0; i < FW_ARRAY_SIZE(mPartitions); i++) {
        if (fw_short_hard_drive_path_matches(hard_drive, &mPartitions[i]) &&
            handle_supports_protocol(mPartitions[i].handle,
                                     (void *)Protocol, NULL)) {
            return mPartitions[i].handle;
        }
    }
    return NULL;
}

void *fw_loaded_image_file_path(void *DevicePath)
{
    UINT8 *bytes = (UINT8 *)DevicePath;
    UINTN path_size;
    UINTN offset = 0;

    if (DevicePath == NULL) {
        return NULL;
    }

    path_size = fw_device_path_size(DevicePath);
    if (path_size == 0) {
        return NULL;
    }
    while (offset + sizeof(FW_DEVICE_PATH_NODE) <= path_size) {
        FW_DEVICE_PATH_NODE *node =
            (FW_DEVICE_PATH_NODE *)(bytes + offset);

        if (node->Type == 0x04 && node->SubType == 0x04) {
            return node;
        }
        if (fw_device_path_is_end(node)) {
            break;
        }
        offset += node->Length;
    }

    return DevicePath;
}

UINTN fw_device_path_size(const VOID *Path)
{
    const UINT8 *bytes = (const UINT8 *)Path;
    UINTN size = 0;

    if (Path == NULL) {
        return 0;
    }

    for (;;) {
        const FW_DEVICE_PATH_NODE *node =
            (const FW_DEVICE_PATH_NODE *)(bytes + size);

        if (node->Length < sizeof(FW_DEVICE_PATH_NODE) ||
            size > (UINTN)-1 - node->Length) {
            return 0;
        }
        size += node->Length;
        if (fw_device_path_is_end(node)) {
            return size;
        }
    }
    return 0;
}

EFI_STATUS fw_build_file_device_path(
    EFI_HANDLE DeviceHandle, const FW_DEVICE_PATH_NODE *FilePath,
    UINT8 *Buffer, UINTN BufferSize)
{
    FW_DEVICE_PATH_NODE *device_path = NULL;
    UINTN device_size;
    UINTN file_size;
    UINTN prefix_size;

    if (DeviceHandle == NULL || FilePath == NULL || Buffer == NULL ||
        !handle_supports_protocol(DeviceHandle,
                                  (void *)mDevicePathProtocolGuid,
                                  (VOID **)&device_path)) {
        return EFI_INVALID_PARAMETER;
    }
    device_size = fw_device_path_size(device_path);
    file_size = fw_device_path_size(FilePath);
    if (device_size < sizeof(FW_DEVICE_PATH_NODE) || file_size == 0) {
        return EFI_UNSUPPORTED;
    }
    prefix_size = device_size - sizeof(FW_DEVICE_PATH_NODE);
    if (prefix_size > BufferSize || file_size > BufferSize - prefix_size) {
        return EFI_BUFFER_TOO_SMALL;
    }
    fw_copy_mem(Buffer, device_path, prefix_size);
    fw_copy_mem(Buffer + prefix_size, FilePath, file_size);
    return EFI_SUCCESS;
}

static const UINT8 mEfiSystemPartitionGuid[16] = {
    0x28, 0x73, 0x2a, 0xc1, 0x1f, 0xf8, 0xd2, 0x11,
    0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b,
};

static BOOLEAN fw_guid_is_zero(const UINT8 *Guid)
{
    UINTN i;

    if (Guid == NULL) {
        return 1;
    }
    for (i = 0; i < 16; i++) {
        if (Guid[i] != 0) {
            return 0;
        }
    }
    return 1;
}

static UINT32 fw_crc32_update(UINT32 Crc, const UINT8 *Data, UINTN Size)
{
    UINTN i;

    for (i = 0; i < Size; i++) {
        UINTN bit;

        Crc ^= Data[i];
        for (bit = 0; bit < 8; bit++) {
            Crc = (Crc >> 1) ^
                  ((Crc & 1U) != 0 ? 0xedb88320U : 0U);
        }
    }
    return Crc;
}

static BOOLEAN fw_block_read_512(EFI_BLOCK_IO_PROTOCOL *Block, UINT64 Lba,
                                 UINT8 *Buffer)
{
    if (Block == NULL || Block->Media == NULL || Buffer == NULL ||
        !Block->Media->MediaPresent || Block->Media->BlockSize < 512U ||
        Block->Media->BlockSize > sizeof(mDiskIoScratch) ||
        Lba > Block->Media->LastBlock) {
        return 0;
    }
    if (Block->Media->BlockSize == 512U) {
        return Block->ReadBlocks(Block, Block->Media->MediaId, Lba, 512U,
                                 Buffer) == EFI_SUCCESS;
    }
    if (Block->ReadBlocks(Block, Block->Media->MediaId, Lba,
                          Block->Media->BlockSize,
                          mDiskIoScratch) != EFI_SUCCESS) {
        return 0;
    }
    fw_copy_mem(Buffer, mDiskIoScratch, 512U);
    return 1;
}

static BOOLEAN fw_block_read_bytes(EFI_BLOCK_IO_PROTOCOL *Block,
                                   UINT64 Offset, UINTN Size, UINT8 *Buffer)
{
    UINT8 *sector = mDiskIoScratch;
    UINTN block_size;
    UINTN done = 0;

    if (Block == NULL || Block->Media == NULL || Buffer == NULL ||
        !Block->Media->MediaPresent || Block->Media->BlockSize == 0 ||
        Block->Media->BlockSize > sizeof(mDiskIoScratch) ||
        (Size != 0 && Offset > ~0ULL - (Size - 1U))) {
        return 0;
    }
    block_size = Block->Media->BlockSize;
    while (done < Size) {
        UINT64 current = Offset + done;
        UINTN sector_offset = (UINTN)(current % block_size);
        UINTN chunk = block_size - sector_offset;
        UINT64 lba = current / block_size;

        if (chunk > Size - done) {
            chunk = Size - done;
        }
        if (lba > Block->Media->LastBlock ||
            Block->ReadBlocks(Block, Block->Media->MediaId, lba,
                              block_size, sector) != EFI_SUCCESS) {
            return 0;
        }
        fw_copy_mem(Buffer + done, sector + sector_offset, chunk);
        done += chunk;
    }
    return 1;
}

static BOOLEAN fw_partition_overlaps(EFI_HANDLE ParentHandle, UINT64 Start,
                                     UINT64 Count)
{
    UINTN i;

    for (i = 0; i < FW_ARRAY_SIZE(mPartitions); i++) {
        FW_PARTITION_RECORD *partition = &mPartitions[i];

        if (!partition->in_use || partition->parent_handle != ParentHandle) {
            continue;
        }
        if (Start < partition->start_lba + partition->block_count &&
            partition->start_lba < Start + Count) {
            return 1;
        }
    }
    return 0;
}

static BOOLEAN fw_partition_build_device_path(FW_PARTITION_RECORD *Partition)
{
    FW_DEVICE_PATH_NODE *parent_path;
    FW_HARD_DRIVE_DEVICE_PATH_NODE *hard_drive;
    FW_DEVICE_PATH_NODE *end;
    UINTN parent_size;
    UINTN prefix_size;

    if (Partition == NULL ||
        !handle_supports_protocol(Partition->parent_handle,
                                  (void *)mDevicePathProtocolGuid,
                                  (VOID **)&parent_path)) {
        return 0;
    }
    parent_size = fw_device_path_size(parent_path);
    if (parent_size < sizeof(FW_DEVICE_PATH_NODE)) {
        return 0;
    }
    prefix_size = parent_size - sizeof(FW_DEVICE_PATH_NODE);
    if (prefix_size + sizeof(*hard_drive) + sizeof(*end) >
        sizeof(Partition->device_path)) {
        return 0;
    }

    fw_set_mem(Partition->device_path, sizeof(Partition->device_path), 0);
    fw_copy_mem(Partition->device_path, parent_path, prefix_size);
    hard_drive = (FW_HARD_DRIVE_DEVICE_PATH_NODE *)
        (VOID *)(Partition->device_path + prefix_size);
    hard_drive->Header.Type = 0x04;
    hard_drive->Header.SubType = 0x01;
    hard_drive->Header.Length = sizeof(*hard_drive);
    hard_drive->PartitionNumber = Partition->partition_number;
    hard_drive->PartitionStart = Partition->start_lba;
    hard_drive->PartitionSize = Partition->block_count;
    fw_copy_mem(hard_drive->PartitionSignature,
                Partition->partition_signature,
                sizeof(hard_drive->PartitionSignature));
    hard_drive->MbrType = Partition->mbr_type;
    hard_drive->SignatureType = Partition->signature_type;
    end = (FW_DEVICE_PATH_NODE *)
        (VOID *)((UINT8 *)hard_drive + sizeof(*hard_drive));
    end->Type = 0x7f;
    end->SubType = 0xff;
    end->Length = sizeof(*end);
    return 1;
}

static EFI_STATUS fw_partition_install_protocols(
    FW_PARTITION_RECORD *Partition)
{
    EFI_HANDLE handle;

    if (Partition == NULL || Partition->handle == NULL) {
        return EFI_INVALID_PARAMETER;
    }
    handle = Partition->handle;
    if (!fw_guid_is_zero(Partition->partition_type_guid)) {
        return bs_install_multiple_protocol_interfaces(
            &handle,
            (void *)mBlockIoProtocolGuid, &Partition->block_io,
            (void *)mDiskIoProtocolGuid, &Partition->disk_io,
            (void *)mDevicePathProtocolGuid, Partition->device_path,
            Partition->partition_type_guid, NULL,
            NULL);
    }
    return bs_install_multiple_protocol_interfaces(
        &handle,
        (void *)mBlockIoProtocolGuid, &Partition->block_io,
        (void *)mDiskIoProtocolGuid, &Partition->disk_io,
        (void *)mDevicePathProtocolGuid, Partition->device_path,
        NULL);
}

static EFI_STATUS fw_partition_uninstall_protocols(
    FW_PARTITION_RECORD *Partition)
{
    if (Partition == NULL || Partition->handle == NULL) {
        return EFI_INVALID_PARAMETER;
    }
    if (!fw_guid_is_zero(Partition->partition_type_guid)) {
        return bs_uninstall_multiple_protocol_interfaces(
            Partition->handle,
            Partition->partition_type_guid, NULL,
            (void *)mDevicePathProtocolGuid, Partition->device_path,
            (void *)mDiskIoProtocolGuid, &Partition->disk_io,
            (void *)mBlockIoProtocolGuid, &Partition->block_io,
            NULL);
    }
    return bs_uninstall_multiple_protocol_interfaces(
        Partition->handle,
        (void *)mDevicePathProtocolGuid, Partition->device_path,
        (void *)mDiskIoProtocolGuid, &Partition->disk_io,
        (void *)mBlockIoProtocolGuid, &Partition->block_io,
        NULL);
}

static EFI_STATUS fw_partition_uninstall(FW_PARTITION_RECORD *Partition)
{
    EFI_STATUS st;

    if (Partition == NULL || !Partition->in_use) {
        return EFI_INVALID_PARAMETER;
    }
    st = fw_fat_uninstall_partition_volume(Partition);
    if (st != EFI_SUCCESS) {
        return st;
    }
    if (Partition->protocols_installed) {
        st = fw_partition_uninstall_protocols(Partition);
        if (st != EFI_SUCCESS) {
            if (fw_fat_reinstall_partition_volume(Partition) !=
                EFI_SUCCESS) {
                return EFI_DEVICE_ERROR;
            }
            return st;
        }
        st = bs_close_protocol(Partition->parent_handle,
                               (void *)mBlockIoProtocolGuid,
                               mStorageDriverHandle, Partition->handle);
        if (st != EFI_SUCCESS && st != EFI_NOT_FOUND) {
            EFI_STATUS install_st =
                fw_partition_install_protocols(Partition);
            EFI_STATUS fat_st =
                fw_fat_reinstall_partition_volume(Partition);

            return install_st == EFI_SUCCESS && fat_st == EFI_SUCCESS ?
                   st : EFI_DEVICE_ERROR;
        }
        Partition->protocols_installed = 0;
    }
    fw_fat_release_partition_volume(Partition);
    fw_set_mem(Partition, sizeof(*Partition), 0);
    return EFI_SUCCESS;
}

static EFI_STATUS fw_partition_remove_children(EFI_HANDLE ParentHandle)
{
    UINTN i;

    for (i = 0; i < FW_ARRAY_SIZE(mPartitions); i++) {
        if (mPartitions[i].in_use &&
            (ParentHandle == NULL ||
             mPartitions[i].parent_handle == ParentHandle)) {
            EFI_STATUS st = fw_partition_uninstall(&mPartitions[i]);

            if (st != EFI_SUCCESS) {
                return st;
            }
        }
    }
    return EFI_SUCCESS;
}

static EFI_STATUS fw_partition_add(EFI_HANDLE ParentHandle,
                                   EFI_BLOCK_IO_PROTOCOL *ParentBlock,
                                   UINT32 PartitionNumber,
                                   UINT64 StartLba, UINT64 BlockCount,
                                   const UINT8 *PartitionTypeGuid,
                                   const UINT8 *PartitionSignature,
                                   UINT8 MbrType, UINT8 SignatureType)
{
    FW_PARTITION_RECORD *partition = NULL;
    VOID *parent_interface;
    EFI_STATUS st;
    UINTN i;

    if (ParentHandle == NULL || ParentBlock == NULL ||
        ParentBlock->Media == NULL || BlockCount == 0 ||
        StartLba > ParentBlock->Media->LastBlock ||
        BlockCount - 1U > ParentBlock->Media->LastBlock - StartLba ||
        StartLba > ~0ULL - BlockCount ||
        fw_partition_overlaps(ParentHandle, StartLba, BlockCount)) {
        return EFI_VOLUME_CORRUPTED;
    }
    for (i = 0; i < FW_ARRAY_SIZE(mPartitions); i++) {
        if (!mPartitions[i].in_use) {
            partition = &mPartitions[i];
            break;
        }
    }
    if (partition == NULL) {
        return EFI_OUT_OF_RESOURCES;
    }

    fw_set_mem(partition, sizeof(*partition), 0);
    partition->in_use = 1;
    partition->handle = (EFI_HANDLE)partition;
    partition->parent_handle = ParentHandle;
    partition->parent_block_io = ParentBlock;
    partition->start_lba = StartLba;
    partition->block_count = BlockCount;
    partition->partition_number = PartitionNumber;
    if (PartitionTypeGuid != NULL) {
        fw_copy_mem(partition->partition_type_guid, PartitionTypeGuid, 16);
    }
    if (PartitionSignature != NULL) {
        fw_copy_mem(partition->partition_signature, PartitionSignature, 16);
    }
    partition->mbr_type = MbrType;
    partition->signature_type = SignatureType;

    partition->media = *ParentBlock->Media;
    partition->media.LogicalPartition = 1;
    partition->media.LastBlock = BlockCount - 1U;
    partition->block_io.Revision = EFI_BLOCK_IO_PROTOCOL_REVISION;
    partition->block_io.Media = &partition->media;
    partition->block_io.Reset = blk_reset;
    partition->block_io.ReadBlocks = blk_read;
    partition->block_io.WriteBlocks = blk_write;
    partition->block_io.FlushBlocks = blk_flush;
    partition->disk_io.Revision = EFI_DISK_IO_PROTOCOL_REVISION;
    partition->disk_io.ReadDisk = disk_read;
    partition->disk_io.WriteDisk = disk_write;
    if (!fw_partition_build_device_path(partition)) {
        fw_set_mem(partition, sizeof(*partition), 0);
        return EFI_UNSUPPORTED;
    }

    st = fw_partition_install_protocols(partition);
    if (st == EFI_SUCCESS) {
        partition->protocols_installed = 1;
    }
    if (st == EFI_SUCCESS) {
        st = bs_open_protocol(ParentHandle, (void *)mBlockIoProtocolGuid,
                              &parent_interface, mStorageDriverHandle,
                              partition->handle,
                              EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER);
    }
    if (st == EFI_SUCCESS) {
        EFI_STATUS fat_status = fw_fat_install_partition_volume(partition);

        if (fat_status != EFI_SUCCESS && fat_status != EFI_NOT_FOUND) {
            st = fat_status;
        }
    }
    if (st != EFI_SUCCESS) {
        if (partition->protocols_installed) {
            (void)fw_partition_uninstall(partition);
        } else {
            fw_set_mem(partition, sizeof(*partition), 0);
        }
        return st;
    }
    return EFI_SUCCESS;
}

static BOOLEAN fw_mbr_extended_type(UINT8 Type)
{
    return Type == 0x05U || Type == 0x0fU || Type == 0x85U;
}

static EFI_STATUS fw_partition_add_mbr_entry(EFI_HANDLE ParentHandle,
                                             EFI_BLOCK_IO_PROTOCOL *Parent,
                                             UINT32 Number, UINT8 Type,
                                             UINT64 Start, UINT64 Count,
                                             const UINT8 *DiskSignature)
{
    const UINT8 *marker = Type == 0xefU ? mEfiSystemPartitionGuid : NULL;
    UINT8 signature[16];

    fw_set_mem(signature, sizeof(signature), 0);
    fw_copy_mem(signature, DiskSignature, 4);
    return fw_partition_add(ParentHandle, Parent, Number, Start, Count,
                            marker, signature, 0x01U, 0x01U);
}

static EFI_STATUS fw_partition_scan_mbr(EFI_HANDLE ParentHandle,
                                        EFI_BLOCK_IO_PROTOCOL *Parent)
{
    UINT8 mbr[512];
    UINT8 disk_signature[4];
    UINT64 extended_base = 0;
    UINTN i;
    EFI_STATUS st;

    if (!fw_block_read_512(Parent, 0, mbr) ||
        mbr[510] != 0x55U || mbr[511] != 0xaaU) {
        return EFI_NOT_FOUND;
    }
    fw_copy_mem(disk_signature, mbr + 440, sizeof(disk_signature));
    for (i = 0; i < 4; i++) {
        const UINT8 *entry = mbr + 446U + i * 16U;
        UINT8 type = entry[4];
        UINT64 start = fw_le32(entry + 8);
        UINT64 count = fw_le32(entry + 12);

        if (type == 0 || count == 0) {
            continue;
        }
        if (fw_mbr_extended_type(type)) {
            if (extended_base != 0) {
                return EFI_VOLUME_CORRUPTED;
            }
            extended_base = start;
            continue;
        }
        if (type == 0xeeU) {
            continue;
        }
        st = fw_partition_add_mbr_entry(ParentHandle, Parent,
                                        (UINT32)i + 1U, type, start, count,
                                        disk_signature);
        if (st != EFI_SUCCESS) {
            return st;
        }
    }

    if (extended_base != 0) {
        UINT64 ebr_lba = extended_base;
        UINT32 logical_number = 5;

        while (logical_number < FW_PARTITION_MAX + 5U) {
            UINT8 ebr[512];
            const UINT8 *logical;
            const UINT8 *link;
            UINT64 start;
            UINT64 count;
            UINT64 next;

            if (!fw_block_read_512(Parent, ebr_lba, ebr) ||
                ebr[510] != 0x55U || ebr[511] != 0xaaU) {
                return EFI_VOLUME_CORRUPTED;
            }
            logical = ebr + 446;
            link = ebr + 462;
            start = ebr_lba + fw_le32(logical + 8);
            count = fw_le32(logical + 12);
            if (logical[4] == 0 || count == 0 ||
                fw_mbr_extended_type(logical[4])) {
                return EFI_VOLUME_CORRUPTED;
            }
            st = fw_partition_add_mbr_entry(
                ParentHandle, Parent, logical_number, logical[4], start,
                count, disk_signature);
            if (st != EFI_SUCCESS) {
                return st;
            }
            logical_number++;

            if (link[4] == 0 && fw_le32(link + 12) == 0) {
                break;
            }
            if (!fw_mbr_extended_type(link[4]) ||
                fw_le32(link + 12) == 0) {
                return EFI_VOLUME_CORRUPTED;
            }
            next = extended_base + fw_le32(link + 8);
            if (next == ebr_lba || next < extended_base ||
                next > Parent->Media->LastBlock) {
                return EFI_VOLUME_CORRUPTED;
            }
            ebr_lba = next;
        }
    }
    return EFI_SUCCESS;
}

static EFI_STATUS fw_partition_scan_gpt_header(
    EFI_HANDLE ParentHandle, EFI_BLOCK_IO_PROTOCOL *Parent, UINT64 HeaderLba)
{
    static UINT8 entry[4096];
    UINT8 header[512];
    UINT8 crc_sector[512];
    UINT32 header_size;
    UINT32 header_crc;
    UINT32 entry_count;
    UINT32 entry_size;
    UINT32 entry_crc;
    UINT32 crc;
    UINT64 entries_lba;
    UINT64 entries_offset;
    UINT64 entry_bytes;
    UINT64 first_usable;
    UINT64 last_usable;
    UINT64 offset;
    UINTN i;

    if (!fw_block_read_512(Parent, HeaderLba, header) ||
        !fw_bytes_eq(header, "EFI PART", 8)) {
        return EFI_NOT_FOUND;
    }
    header_size = fw_le32(header + 12);
    header_crc = fw_le32(header + 16);
    /*
     * Deliberately no check on the Revision field at header+8.  The spec says
     * it "is 0x00010000", but real writers disagree and real firmware does not
     * care: EDK2's PartitionValidGptTable() validates the signature, the CRC
     * over HeaderSize, MyLBA and SizeOfPartitionEntry, and never looks at the
     * revision.  GNU Parted - which is what every period-correct IA-64 Linux
     * installer partitions with - writes 0x00010200 instead
     * (GPT_HEADER_REVISION_V1_02, parted-1.4.24 include/parted/disk_gpt.h:39,
     * used by gpt_write_new() at libparted/disk_gpt.c:653, alongside
     * "HeaderSize = 92; /_* per 1.02 spec *_/").  Requiring 0x00010000 exactly
     * rejected a stock Debian 3.0 install outright: the primary and backup
     * headers both failed, no partition was published, and the EFI shell's
     * "map" reported "No readable file systems were found" on a disk whose
     * ESP was perfectly intact.  The header CRC is the integrity guard here;
     * the revision adds nothing to it.
     */
    if (header_size < 92U || header_size > sizeof(header) ||
        fw_le32(header + 20) != 0 || fw_le64(header + 24) != HeaderLba ||
        fw_le64(header + 32) > Parent->Media->LastBlock ||
        fw_le64(header + 32) == HeaderLba) {
        return EFI_VOLUME_CORRUPTED;
    }
    header[16] = header[17] = header[18] = header[19] = 0;
    if (bs_calculate_crc32(header, header_size, &crc) != EFI_SUCCESS ||
        crc != header_crc) {
        return EFI_VOLUME_CORRUPTED;
    }

    first_usable = fw_le64(header + 40);
    last_usable = fw_le64(header + 48);
    entries_lba = fw_le64(header + 72);
    entry_count = fw_le32(header + 80);
    entry_size = fw_le32(header + 84);
    entry_crc = fw_le32(header + 88);
    if (first_usable > last_usable ||
        last_usable > Parent->Media->LastBlock || entry_count == 0 ||
        entry_size < 128U || (entry_size & 7U) != 0 ||
        entry_size > sizeof(entry) ||
        (UINT64)entry_count > ~0ULL / entry_size) {
        return EFI_VOLUME_CORRUPTED;
    }
    entry_bytes = (UINT64)entry_count * entry_size;
    if (entry_bytes > 64U * 1024U * 1024U ||
        entries_lba > Parent->Media->LastBlock ||
        Parent->Media->BlockSize == 0 ||
        (entry_bytes + Parent->Media->BlockSize - 1U) /
            Parent->Media->BlockSize - 1U >
            Parent->Media->LastBlock - entries_lba) {
        return EFI_VOLUME_CORRUPTED;
    }
    if (entries_lba > ~0ULL / Parent->Media->BlockSize) {
        return EFI_VOLUME_CORRUPTED;
    }
    entries_offset = entries_lba * Parent->Media->BlockSize;
    if (entry_bytes > ~0ULL - entries_offset) {
        return EFI_VOLUME_CORRUPTED;
    }

    crc = 0xffffffffU;
    offset = 0;
    while (offset < entry_bytes) {
        UINTN chunk = entry_bytes - offset > sizeof(crc_sector) ?
                      sizeof(crc_sector) : (UINTN)(entry_bytes - offset);

        if (!fw_block_read_bytes(Parent, entries_offset + offset,
                                 chunk, crc_sector)) {
            return EFI_DEVICE_ERROR;
        }
        crc = fw_crc32_update(crc, crc_sector, chunk);
        offset += chunk;
    }
    if (~crc != entry_crc) {
        return EFI_VOLUME_CORRUPTED;
    }

    for (i = 0; i < entry_count; i++) {
        UINT64 first_lba;
        UINT64 last_lba;
        EFI_STATUS st;

        if (!fw_block_read_bytes(Parent,
                                 entries_offset + (UINT64)i * entry_size,
                                 entry_size, entry)) {
            return EFI_DEVICE_ERROR;
        }
        if (fw_guid_is_zero(entry)) {
            continue;
        }
        first_lba = fw_le64(entry + 32);
        last_lba = fw_le64(entry + 40);
        if (fw_guid_is_zero(entry + 16) || first_lba < first_usable ||
            first_lba > last_lba || last_lba > last_usable) {
            return EFI_VOLUME_CORRUPTED;
        }
        st = fw_partition_add(ParentHandle, Parent, (UINT32)i + 1U,
                              first_lba, last_lba - first_lba + 1U,
                              entry, entry + 16, 0x02U, 0x02U);
        if (st != EFI_SUCCESS) {
            return st;
        }
    }
    return EFI_SUCCESS;
}

static EFI_STATUS fw_partition_scan_gpt(EFI_HANDLE ParentHandle,
                                        EFI_BLOCK_IO_PROTOCOL *Parent)
{
    UINT8 sector[512];
    BOOLEAN primary_signature;
    EFI_STATUS primary;
    EFI_STATUS backup;

    if (!fw_block_read_512(Parent, 1, sector)) {
        return EFI_DEVICE_ERROR;
    }
    primary_signature = fw_bytes_eq(sector, "EFI PART", 8);
    primary = fw_partition_scan_gpt_header(ParentHandle, Parent, 1);
    if (primary == EFI_SUCCESS) {
        return EFI_SUCCESS;
    }
    if (fw_partition_remove_children(ParentHandle) != EFI_SUCCESS) {
        return EFI_ACCESS_DENIED;
    }
    backup = fw_partition_scan_gpt_header(ParentHandle, Parent,
                                          Parent->Media->LastBlock);
    if (backup == EFI_SUCCESS) {
        return EFI_SUCCESS;
    }
    if (fw_partition_remove_children(ParentHandle) != EFI_SUCCESS) {
        return EFI_ACCESS_DENIED;
    }
    if (primary_signature || backup != EFI_NOT_FOUND) {
        return primary != EFI_NOT_FOUND ? primary : backup;
    }
    return EFI_NOT_FOUND;
}

EFI_STATUS fw_partition_discover(EFI_HANDLE ParentHandle,
                                        EFI_BLOCK_IO_PROTOCOL *Parent)
{
    VOID *interface;
    EFI_STATUS st;
    BOOLEAN opened_by_driver;

    if (ParentHandle == NULL || Parent == NULL || Parent->Media == NULL ||
        !Parent->Media->MediaPresent || Parent->Media->BlockSize < 512U ||
        Parent->Media->BlockSize > sizeof(mDiskIoScratch) ||
        Parent->Media->LogicalPartition) {
        return EFI_UNSUPPORTED;
    }
    st = fw_partition_remove_children(ParentHandle);
    if (st != EFI_SUCCESS) {
        return st;
    }
    st = bs_open_protocol(ParentHandle, (void *)mBlockIoProtocolGuid,
                          &interface, mStorageDriverHandle, ParentHandle,
                          EFI_OPEN_PROTOCOL_BY_DRIVER);
    opened_by_driver = st == EFI_SUCCESS;
    if (st != EFI_SUCCESS && st != EFI_ALREADY_STARTED) {
        return st;
    }

    st = fw_partition_scan_gpt(ParentHandle, Parent);
    if (st == EFI_NOT_FOUND) {
        st = fw_partition_scan_mbr(ParentHandle, Parent);
    }
    if (st != EFI_SUCCESS && st != EFI_NOT_FOUND) {
        EFI_STATUS cleanup = fw_partition_remove_children(ParentHandle);

        if (cleanup != EFI_SUCCESS) {
            return cleanup;
        }
        if (opened_by_driver) {
            EFI_STATUS close_st = bs_close_protocol(
                ParentHandle, (void *)mBlockIoProtocolGuid,
                mStorageDriverHandle, ParentHandle);

            if (close_st != EFI_SUCCESS) {
                return close_st;
            }
        }
    }
    return st;
}

BOOLEAN efi_handle_is_valid(EFI_HANDLE Handle)
{
    UINTN i;

    if (Handle == NULL) {
        return 0;
    }
    if (Handle == mBlockIoHandle || Handle == mRawBlockIoHandle ||
        Handle == mDiskBlockIoHandle || Handle == mImageHandle ||
        Handle == mUnicodeCollationHandle || Handle == mGraphicsHandle ||
        Handle == mPciRootBridgeHandle ||
        fw_pci_io_device_from_handle_opaque(Handle) != NULL) {
        return 1;
    }
    for (i = 0; i < FW_ARRAY_SIZE(mPartitions); i++) {
        if (mPartitions[i].in_use && mPartitions[i].handle == Handle) {
            return 1;
        }
    }
    for (i = 0; i < LOADED_IMAGE_MAX; i++) {
        if (mLoadedImages[i].in_use && mLoadedImages[i].handle == Handle) {
            return 1;
        }
    }
    for (i = 0; i < PROTOCOL_RECORD_MAX; i++) {
        if (mProtocolRecords[i].in_use &&
            mProtocolRecords[i].handle == Handle) {
            return 1;
        }
    }
    return 0;
}

static BOOLEAN partition_driver_manages_controller(EFI_HANDLE Controller)
{
    UINTN i;

    for (i = 0; i < OPEN_PROTOCOL_RECORD_MAX; i++) {
        EFI_OPEN_PROTOCOL_RECORD *rec = &mOpenProtocolRecords[i];

        if (rec->in_use && rec->handle == Controller &&
            rec->agent_handle == mPartitionDriverBinding.DriverBindingHandle &&
            rec->controller_handle == Controller &&
            rec->attributes == EFI_OPEN_PROTOCOL_BY_DRIVER &&
            guid_matches((void *)mBlockIoProtocolGuid, rec->guid)) {
            return 1;
        }
    }
    return 0;
}

static BOOLEAN component_name_language_supported(const CHAR8 *Language)
{
    return Language != NULL && Language[0] == 'e' &&
           Language[1] == 'n' && Language[2] == 'g';
}

static EFI_STATUS partition_component_get_driver_name(
    EFI_COMPONENT_NAME_PROTOCOL *This, CHAR8 *Language,
    CHAR16 **DriverName)
{
    static CHAR16 name[] = {
        'P', 'a', 'r', 't', 'i', 't', 'i', 'o', 'n', ' ',
        'D', 'r', 'i', 'v', 'e', 'r', 0
    };

    if (This != &mPartitionComponentName || Language == NULL ||
        DriverName == NULL) {
        return EFI_INVALID_PARAMETER;
    }
    *DriverName = NULL;
    if (!component_name_language_supported(Language)) {
        return EFI_UNSUPPORTED;
    }
    *DriverName = name;
    return EFI_SUCCESS;
}

static EFI_STATUS partition_component_get_controller_name(
    EFI_COMPONENT_NAME_PROTOCOL *This, EFI_HANDLE ControllerHandle,
    EFI_HANDLE ChildHandle, CHAR8 *Language, CHAR16 **ControllerName)
{
    static CHAR16 controller_name[] = {
        'P', 'a', 'r', 't', 'i', 't', 'i', 'o', 'n', 'a', 'b', 'l', 'e',
        ' ', 'B', 'l', 'o', 'c', 'k', ' ', 'D', 'e', 'v', 'i', 'c', 'e', 0
    };
    static CHAR16 child_name[] = {
        'D', 'i', 's', 'k', ' ', 'P', 'a', 'r', 't', 'i', 't', 'i', 'o', 'n',
        0
    };
    UINTN i;

    if (This != &mPartitionComponentName || Language == NULL ||
        ControllerName == NULL ||
        !efi_handle_is_valid(ControllerHandle) ||
        (ChildHandle != NULL && !efi_handle_is_valid(ChildHandle))) {
        return EFI_INVALID_PARAMETER;
    }
    *ControllerName = NULL;
    if (!component_name_language_supported(Language) ||
        !partition_driver_manages_controller(ControllerHandle)) {
        return EFI_UNSUPPORTED;
    }
    if (ChildHandle == NULL) {
        *ControllerName = controller_name;
        return EFI_SUCCESS;
    }
    for (i = 0; i < FW_ARRAY_SIZE(mPartitions); i++) {
        if (mPartitions[i].in_use &&
            mPartitions[i].parent_handle == ControllerHandle &&
            mPartitions[i].handle == ChildHandle) {
            *ControllerName = child_name;
            return EFI_SUCCESS;
        }
    }
    return EFI_UNSUPPORTED;
}

static EFI_STATUS partition_driver_supported(
    EFI_DRIVER_BINDING_PROTOCOL *This, EFI_HANDLE ControllerHandle,
    VOID *RemainingDevicePath)
{
    EFI_BLOCK_IO_PROTOCOL *block = NULL;
    FW_DEVICE_PATH_NODE *remaining =
        (FW_DEVICE_PATH_NODE *)RemainingDevicePath;
    UINTN i;

    if (This != &mPartitionDriverBinding || ControllerHandle == NULL ||
        (remaining != NULL &&
         (((UINTN)remaining & 3U) != 0 ||
          !fw_hard_drive_path_node_supported(remaining) ||
          fw_device_path_size(remaining) == 0 ||
          ((FW_HARD_DRIVE_DEVICE_PATH_NODE *)remaining)->PartitionNumber ==
              0 ||
          ((FW_HARD_DRIVE_DEVICE_PATH_NODE *)remaining)->PartitionSize ==
              0 ||
          ((FW_HARD_DRIVE_DEVICE_PATH_NODE *)remaining)->MbrType > 2U ||
          ((FW_HARD_DRIVE_DEVICE_PATH_NODE *)remaining)->SignatureType >
              2U)) ||
        !handle_supports_protocol(ControllerHandle,
                                  (void *)mBlockIoProtocolGuid,
                                  (VOID **)&block) ||
        !handle_supports_protocol(ControllerHandle,
                                  (void *)mDevicePathProtocolGuid, NULL) ||
        block == NULL || block->Media == NULL ||
        block->Media->LogicalPartition || !block->Media->MediaPresent ||
        block->Media->BlockSize < 512U ||
        block->Media->BlockSize > sizeof(mDiskIoScratch) ||
        (remaining != NULL &&
         (((FW_HARD_DRIVE_DEVICE_PATH_NODE *)remaining)->PartitionStart >
              block->Media->LastBlock ||
          ((FW_HARD_DRIVE_DEVICE_PATH_NODE *)remaining)->PartitionSize - 1U >
              block->Media->LastBlock -
              ((FW_HARD_DRIVE_DEVICE_PATH_NODE *)remaining)->
                  PartitionStart))) {
        return EFI_UNSUPPORTED;
    }
    for (i = 0; i < OPEN_PROTOCOL_RECORD_MAX; i++) {
        EFI_OPEN_PROTOCOL_RECORD *rec = &mOpenProtocolRecords[i];

        if (rec->in_use && rec->handle == ControllerHandle &&
            rec->agent_handle == This->DriverBindingHandle &&
            rec->attributes == EFI_OPEN_PROTOCOL_BY_DRIVER &&
            guid_matches((void *)mBlockIoProtocolGuid, rec->guid)) {
            return EFI_ALREADY_STARTED;
        }
    }
    return EFI_SUCCESS;
}

static EFI_STATUS partition_driver_start(
    EFI_DRIVER_BINDING_PROTOCOL *This, EFI_HANDLE ControllerHandle,
    VOID *RemainingDevicePath)
{
    EFI_BLOCK_IO_PROTOCOL *block = NULL;
    EFI_STATUS st;

    if (This != &mPartitionDriverBinding ||
        partition_driver_supported(This, ControllerHandle,
                                   RemainingDevicePath) != EFI_SUCCESS ||
        !handle_supports_protocol(ControllerHandle,
                                  (void *)mBlockIoProtocolGuid,
                                  (VOID **)&block) || block == NULL) {
        return EFI_UNSUPPORTED;
    }
    st = fw_partition_discover(ControllerHandle, block);
    return st == EFI_NOT_FOUND ? EFI_SUCCESS : st;
}

static EFI_STATUS partition_driver_stop(
    EFI_DRIVER_BINDING_PROTOCOL *This, EFI_HANDLE ControllerHandle,
    UINTN NumberOfChildren, EFI_HANDLE *ChildHandleBuffer)
{
    UINTN stopped = 0;
    UINTN i;

    if (This == NULL || ControllerHandle == NULL ||
        (NumberOfChildren != 0 && ChildHandleBuffer == NULL)) {
        return EFI_INVALID_PARAMETER;
    }
    if (NumberOfChildren != 0) {
        for (i = 0; i < NumberOfChildren; i++) {
            UINTN j;

            for (j = 0; j < FW_ARRAY_SIZE(mPartitions); j++) {
                if (mPartitions[j].in_use &&
                    mPartitions[j].handle == ChildHandleBuffer[i] &&
                    mPartitions[j].parent_handle == ControllerHandle) {
                    if (fw_partition_uninstall(&mPartitions[j]) ==
                        EFI_SUCCESS) {
                        stopped++;
                    }
                    break;
                }
            }
        }
        return stopped == NumberOfChildren ? EFI_SUCCESS : EFI_DEVICE_ERROR;
    }

    {
        EFI_STATUS st = fw_partition_remove_children(ControllerHandle);

        if (st != EFI_SUCCESS) {
            return st;
        }
    }
    {
        EFI_STATUS st = bs_close_protocol(
            ControllerHandle, (void *)mBlockIoProtocolGuid,
            This->DriverBindingHandle, ControllerHandle);

        return st == EFI_SUCCESS || st == EFI_NOT_FOUND ? EFI_SUCCESS : st;
    }
}

BOOLEAN partition_driver_install(void)
{
    EFI_HANDLE handle = mStorageDriverHandle;
    EFI_STATUS st;

    mPartitionDriverBinding.Supported = partition_driver_supported;
    mPartitionDriverBinding.Start = partition_driver_start;
    mPartitionDriverBinding.Stop = partition_driver_stop;
    mPartitionDriverBinding.Version = 0x10U;
    mPartitionDriverBinding.ImageHandle = mStorageDriverHandle;
    mPartitionDriverBinding.DriverBindingHandle = mStorageDriverHandle;
    mPartitionComponentName.GetDriverName =
        partition_component_get_driver_name;
    mPartitionComponentName.GetControllerName =
        partition_component_get_controller_name;
    mPartitionComponentName.SupportedLanguages = "eng";

    st = bs_install_protocol(&handle, (void *)mLoadedImageProtocolGuid, 0,
                             &mLoadedImageProto);
    if (st != EFI_SUCCESS) {
        return 0;
    }
    st = bs_install_protocol(&handle, (void *)mDriverBindingProtocolGuid, 0,
                             &mPartitionDriverBinding);
    if (st != EFI_SUCCESS) {
        (void)bs_uninstall_protocol(handle,
                                    (void *)mLoadedImageProtocolGuid,
                                    &mLoadedImageProto);
        return 0;
    }
    st = bs_install_protocol(&handle, (void *)mComponentNameProtocolGuid, 0,
                             &mPartitionComponentName);
    if (st != EFI_SUCCESS) {
        (void)bs_uninstall_protocol(handle,
                                    (void *)mDriverBindingProtocolGuid,
                                    &mPartitionDriverBinding);
        (void)bs_uninstall_protocol(handle,
                                    (void *)mLoadedImageProtocolGuid,
                                    &mLoadedImageProto);
        return 0;
    }
    return 1;
}

BOOLEAN partition_component_name_selftest(VOID)
{
    CHAR8 english[] = { 'e', 'n', 'g', 0 };
    CHAR8 unsupported[] = { 'f', 'r', 'a', 0 };
    EFI_COMPONENT_NAME_PROTOCOL *protocol = NULL;
    CHAR16 *name = (CHAR16 *)(UINTN)1;
    EFI_HANDLE managed = NULL;
    EFI_HANDLE child = NULL;
    UINTN i;

    if (!handle_supports_protocol(
            mStorageDriverHandle, (void *)mComponentNameProtocolGuid,
            (VOID **)&protocol) || protocol != &mPartitionComponentName ||
        protocol->SupportedLanguages == NULL ||
        protocol->SupportedLanguages[0] != 'e' ||
        protocol->SupportedLanguages[1] != 'n' ||
        protocol->SupportedLanguages[2] != 'g' ||
        protocol->SupportedLanguages[3] != 0 ||
        protocol->GetDriverName(protocol, NULL, &name) !=
            EFI_INVALID_PARAMETER ||
        protocol->GetDriverName(protocol, english, NULL) !=
            EFI_INVALID_PARAMETER ||
        protocol->GetDriverName(protocol, unsupported, &name) !=
            EFI_UNSUPPORTED || name != NULL ||
        protocol->GetDriverName(protocol, english, &name) != EFI_SUCCESS ||
        name == NULL || name[0] != 'P' ||
        protocol->GetControllerName(
            protocol, (EFI_HANDLE)(UINTN)0xf17eU, NULL,
            english, &name) != EFI_INVALID_PARAMETER ||
        protocol->GetControllerName(protocol, mGraphicsHandle, NULL,
                                    english, &name) != EFI_UNSUPPORTED) {
        return 0;
    }

    for (i = 0; i < OPEN_PROTOCOL_RECORD_MAX; i++) {
        EFI_OPEN_PROTOCOL_RECORD *rec = &mOpenProtocolRecords[i];

        if (rec->in_use &&
            rec->agent_handle == mPartitionDriverBinding.DriverBindingHandle &&
            rec->controller_handle == rec->handle &&
            rec->attributes == EFI_OPEN_PROTOCOL_BY_DRIVER &&
            guid_matches((void *)mBlockIoProtocolGuid, rec->guid)) {
            managed = rec->handle;
            break;
        }
    }
    if (managed == NULL) {
        return 1;
    }
    name = NULL;
    if (protocol->GetControllerName(protocol, managed, NULL,
                                    english, &name) != EFI_SUCCESS ||
        name == NULL || name[0] != 'P') {
        return 0;
    }
    for (i = 0; i < FW_ARRAY_SIZE(mPartitions); i++) {
        if (mPartitions[i].in_use &&
            mPartitions[i].parent_handle == managed) {
            child = mPartitions[i].handle;
            break;
        }
    }
    if (child == NULL) {
        return 1;
    }
    name = NULL;
    return protocol->GetControllerName(protocol, managed, child,
                                       english, &name) == EFI_SUCCESS &&
           name != NULL && name[0] == 'D' &&
           protocol->GetControllerName(protocol, managed, mGraphicsHandle,
                                       english, &name) == EFI_UNSUPPORTED;
}

EFI_LOADED_IMAGE_RECORD *fw_loaded_image_record(EFI_HANDLE ImageHandle)
{
    UINTN i;

    for (i = 0; i < LOADED_IMAGE_MAX; i++) {
        if (mLoadedImages[i].in_use &&
            mLoadedImages[i].handle == ImageHandle) {
            return &mLoadedImages[i];
        }
    }
    return NULL;
}

BOOLEAN fw_set_loaded_image_load_options(EFI_HANDLE ImageHandle,
                                                VOID *LoadOptions,
                                                UINT32 LoadOptionsSize)
{
    EFI_LOADED_IMAGE_RECORD *rec = fw_loaded_image_record(ImageHandle);

    if (rec == NULL ||
        ((LoadOptions == NULL) != (LoadOptionsSize == 0))) {
        return 0;
    }

    rec->loaded_image.LoadOptions = LoadOptions;
    rec->loaded_image.LoadOptionsSize = LoadOptionsSize;
    return 1;
}

EFI_STATUS fw_copy_loaded_image_load_options(EFI_HANDLE ImageHandle,
                                                    const VOID *LoadOptions,
                                                    UINT32 LoadOptionsSize,
                                                    VOID **AllocatedOptions)
{
    EFI_LOADED_IMAGE_RECORD *rec = fw_loaded_image_record(ImageHandle);
    VOID *copy;
    EFI_STATUS st;

    if (rec == NULL || LoadOptions == NULL || LoadOptionsSize == 0 ||
        AllocatedOptions == NULL ||
        rec->loaded_image.LoadOptions != NULL ||
        rec->loaded_image.LoadOptionsSize != 0) {
        return EFI_INVALID_PARAMETER;
    }

    *AllocatedOptions = NULL;
    st = mBootServices.AllocatePool(EfiBootServicesData, LoadOptionsSize,
                                    &copy);
    if (st != EFI_SUCCESS) {
        return st;
    }
    fw_copy_mem(copy, LoadOptions, LoadOptionsSize);
    if (!fw_set_loaded_image_load_options(ImageHandle, copy,
                                          LoadOptionsSize)) {
        (void)mBootServices.FreePool(copy);
        return EFI_INVALID_PARAMETER;
    }
    *AllocatedOptions = copy;
    return EFI_SUCCESS;
}

EFI_STATUS fw_release_loaded_image_load_options(
    EFI_HANDLE ImageHandle, VOID *AllocatedOptions)
{
    EFI_LOADED_IMAGE_RECORD *rec = fw_loaded_image_record(ImageHandle);
    EFI_STATUS st;

    if (rec == NULL || AllocatedOptions == NULL ||
        rec->loaded_image.LoadOptions != AllocatedOptions ||
        rec->loaded_image.LoadOptionsSize == 0) {
        return EFI_INVALID_PARAMETER;
    }

    st = mBootServices.FreePool(AllocatedOptions);
    if (st != EFI_SUCCESS) {
        return st;
    }
    rec->loaded_image.LoadOptions = NULL;
    rec->loaded_image.LoadOptionsSize = 0;
    return EFI_SUCCESS;
}

EFI_STATUS fw_loaded_image_source_paths(
    EFI_LOADED_IMAGE_RECORD *Record, void *DevicePath,
    EFI_HANDLE *DeviceHandle, VOID **FilePath)
{
    VOID *copy;
    VOID *remaining;
    EFI_HANDLE device = NULL;
    UINTN path_size;
    UINTN remaining_offset = 0;
    EFI_STATUS st;

    if (Record == NULL || DeviceHandle == NULL || FilePath == NULL) {
        return EFI_INVALID_PARAMETER;
    }
    Record->device_path = NULL;
    *DeviceHandle = NULL;
    *FilePath = NULL;
    if (DevicePath == NULL) {
        return EFI_SUCCESS;
    }

    path_size = fw_device_path_size(DevicePath);
    if (path_size == 0) {
        return EFI_INVALID_PARAMETER;
    }
    st = bs_allocate_pool(EfiBootServicesData, path_size, &copy);
    if (st != EFI_SUCCESS) {
        return st;
    }
    fw_copy_mem(copy, DevicePath, path_size);
    Record->device_path = copy;

    remaining = DevicePath;
    st = bs_locate_device_path((void *)mDevicePathProtocolGuid,
                               &remaining, &device);
    if (st == EFI_SUCCESS) {
        if ((UINT8 *)remaining < (UINT8 *)DevicePath ||
            (UINT8 *)remaining >= (UINT8 *)DevicePath + path_size) {
            (void)bs_free_pool(copy);
            Record->device_path = NULL;
            return EFI_INVALID_PARAMETER;
        }
        remaining_offset = (UINTN)((UINT8 *)remaining -
                                   (UINT8 *)DevicePath);
        *DeviceHandle = device;
    } else if (st != EFI_NOT_FOUND) {
        (void)bs_free_pool(copy);
        Record->device_path = NULL;
        return st;
    }

    *FilePath = (UINT8 *)copy + remaining_offset;
    return EFI_SUCCESS;
}

static BOOLEAN fw_device_path_file_name_eq_ascii(const FW_DEVICE_PATH_NODE *path,
                                                 const char *ascii);

BOOLEAN __attribute__((noinline)) loaded_image_file_path_selftest(void)
{
    EFI_LOADED_IMAGE_RECORD rec;
    EFI_HANDLE device_handle;
    VOID *relative_path;
    UINT8 long_path[300];
    UINT8 padded_hard_drive_path[
        48U + sizeof(mBootFullDevicePath.FileHeader) +
        sizeof(mBootFullDevicePath.PathName) +
        sizeof(mBootFullDevicePath.End)];
    void *full_path;
    void *file_path;
    UINT8 *file_bytes;
    UINT8 *storage_start;
    UINT8 *storage_end;
    FW_DEVICE_PATH_NODE *file_node;
    UINTN path_size;
    BOOLEAN ok = 0;

    fw_set_mem(&rec, sizeof(rec), 0);
    if (fw_loaded_image_source_paths(
            &rec, &mBootFullDevicePath.FileHeader,
            &device_handle, &relative_path) != EFI_SUCCESS) {
        return 0;
    }
    full_path = rec.device_path;
    file_path = fw_loaded_image_file_path(full_path);
    file_bytes = (UINT8 *)file_path;
    path_size = fw_device_path_size(&mBootFullDevicePath.FileHeader);
    storage_start = rec.device_path;
    storage_end = storage_start + path_size;

    if (full_path == &mBootFullDevicePath.FileHeader ||
        relative_path != full_path ||
        file_bytes < storage_start ||
        file_bytes + sizeof(FW_DEVICE_PATH_NODE) > storage_end) {
        goto out;
    }

    file_node = (FW_DEVICE_PATH_NODE *)file_path;
    if (file_node->Type != 0x04 || file_node->SubType != 0x04 ||
        file_node->Length != mBootFullDevicePath.FileHeader.Length) {
        goto out;
    }

    (void)bs_free_pool(rec.device_path);
    fw_set_mem(&rec, sizeof(rec), 0);
    fw_set_mem(long_path, sizeof(long_path), 0);
    file_node = (FW_DEVICE_PATH_NODE *)long_path;
    file_node->Type = 0x04;
    file_node->SubType = 0x04;
    file_node->Length = sizeof(long_path) - sizeof(FW_DEVICE_PATH_NODE);
    file_node = (FW_DEVICE_PATH_NODE *)(long_path + file_node->Length);
    file_node->Type = 0x7f;
    file_node->SubType = 0xff;
    file_node->Length = sizeof(FW_DEVICE_PATH_NODE);
    if (fw_device_path_size(long_path) != sizeof(long_path) ||
        fw_loaded_image_source_paths(&rec, long_path, &device_handle,
                                     &relative_path) != EFI_SUCCESS ||
        rec.device_path == long_path || relative_path != rec.device_path ||
        fw_device_path_size(rec.device_path) != sizeof(long_path)) {
        goto out;
    }

    fw_set_mem(padded_hard_drive_path, sizeof(padded_hard_drive_path), 0);
    file_node = (FW_DEVICE_PATH_NODE *)padded_hard_drive_path;
    file_node->Type = 0x04;
    file_node->SubType = 0x01;
    file_node->Length = 48U;
    fw_copy_mem(padded_hard_drive_path + 48U,
                &mBootFullDevicePath.FileHeader,
                sizeof(padded_hard_drive_path) - 48U);
    if (fw_loaded_image_file_path(padded_hard_drive_path) !=
            padded_hard_drive_path + 48U) {
        goto out;
    }

    (void)bs_free_pool(rec.device_path);
    fw_set_mem(&rec, sizeof(rec), 0);
    device_handle = (EFI_HANDLE)(UINTN)1;
    relative_path = (VOID *)(UINTN)1;
    if (fw_loaded_image_source_paths(&rec, NULL, &device_handle,
                                     &relative_path) != EFI_SUCCESS ||
        rec.device_path != NULL || device_handle != NULL ||
        relative_path != NULL || !pe_hii_package_list_selftest()) {
        goto out;
    }
    ok = 1;

out:
    if (rec.device_path != NULL) {
        (void)bs_free_pool(rec.device_path);
    }
    return ok;
}

BOOLEAN __attribute__((noinline)) optical_setup_boot_option_selftest(void)
{
    FW_EFI_BOOT_OPTION option;
    UINTN size = sizeof(option);
    UINT32 attributes = 0;

    if (rs_get_boot0000_variable(&attributes, &size, &option) != EFI_SUCCESS ||
        attributes != (EFI_VARIABLE_NON_VOLATILE |
                       EFI_VARIABLE_BOOTSERVICE_ACCESS |
                       EFI_VARIABLE_RUNTIME_ACCESS) ||
        size != sizeof(FW_EFI_BOOT_OPTION)) {
        return 0;
    }

    if (option.FilePathListLength != sizeof(option.FilePath) ||
        option.FilePath.Cdrom.Header.Type != 0x04 ||
        option.FilePath.Cdrom.Header.SubType != 0x02 ||
        option.FilePath.Cdrom.PartitionStart != mBootImageStartLba ||
        option.FilePath.Cdrom.PartitionSize != mBootImagePartitionCdBlocks ||
        option.FilePath.FileHeader.Type != 0x04 ||
        option.FilePath.FileHeader.SubType != 0x04 ||
        option.FilePath.End.Type != 0x7f ||
        option.FilePath.End.SubType != 0xff) {
        return 0;
    }

    if (!fw_device_path_file_name_eq_ascii(
            (const FW_DEVICE_PATH_NODE *)&option.FilePath,
            "bootia64.efi")) {
        return 0;
    }

    {
        void *remaining = &option.FilePath;
        EFI_HANDLE loader_device = NULL;

        if (bs_locate_device_path((void *)mBlockIoProtocolGuid,
                                  &remaining, &loader_device) != EFI_SUCCESS ||
            loader_device != mBlockIoHandle ||
            remaining != &option.FilePath.FileHeader ||
            fw_loaded_image_file_path(&option.FilePath) !=
                &option.FilePath.FileHeader) {
            return 0;
        }
    }

    /*
     * The synthetic optical boot option now carries empty load options: it
     * ends at the device path with no OptionalData, so there is no injected
     * OS loader payload to validate.
     */
    return 1;
}

BOOLEAN __attribute__((noinline)) optical_raw_device_path_selftest(void)
{
    return mRawBlockDevicePath.Acpi.Header.Type == 0x02 &&
           mRawBlockDevicePath.Pci.Header.Type == 0x01 &&
           fw_storage_path_node_matches(&mRawBlockDevicePath.Atapi,
                                        &mBootStorageDevice) &&
           mRawBlockDevicePath.End.Type == 0x7f &&
           mRawBlockDevicePath.End.SubType == 0xff &&
           fw_device_path_size((const FW_DEVICE_PATH_NODE *)
                               &mRawBlockDevicePath) ==
               sizeof(mRawBlockDevicePath);
}

BOOLEAN __attribute__((noinline)) el_torito_partition_selftest(void)
{
    UINT64 expected_cd_blocks;

    if (!storage_present(&mBootStorageDevice) ||
        !storage_is_cd(&mBootStorageDevice) || !mBootImageMapped) {
        return 1;
    }
    if (mBootImageFatBlocks == 0 ||
        mBootImagePartitionBlocks < mBootImageFatBlocks) {
        return 0;
    }

    if (mBootImageUsesUefiSectorCount &&
        mBootImageCatalogSectorCount <= 1U) {
        if (mCdromBlocks <= mBootImageStartLba) {
            return 0;
        }
        expected_cd_blocks = (UINT64)(mCdromBlocks - mBootImageStartLba);
    } else {
        expected_cd_blocks = ((UINT64)mBootImagePartitionBlocks + 3U) / 4U;
    }

    return mBootImagePartitionCdBlocks == expected_cd_blocks &&
           mBlockIoMedia.LastBlock + 1U ==
               mBootImagePartitionBlocks &&
           mBlockDevicePath.Cdrom.PartitionStart == mBootImageStartLba &&
           mBlockDevicePath.Cdrom.PartitionSize ==
               mBootImagePartitionCdBlocks &&
           mBootFullDevicePath.Cdrom.PartitionSize ==
               mBootImagePartitionCdBlocks;
}

EFI_STATUS bs_locate_device_path(void *Protocol, void **DevicePath,
                                 EFI_HANDLE *Device)
{
    FW_DEVICE_PATH_NODE *path;
    EFI_HANDLE *handles = NULL;
    EFI_HANDLE best_handle = NULL;
    UINTN best_match = 0;
    UINTN handle_count = 0;
    UINTN i;
    EFI_STATUS st;

    if (Protocol == NULL || DevicePath == NULL || *DevicePath == NULL ||
        Device == NULL) {
        return EFI_INVALID_PARAMETER;
    }

    path = (FW_DEVICE_PATH_NODE *)*DevicePath;
    if (fw_device_path_size(path) == 0) {
        return EFI_INVALID_PARAMETER;
    }

    st = bs_locate_handle_buffer(EFI_LOCATE_ALL_HANDLES, NULL, NULL,
                                 &handle_count, &handles);
    if (st != EFI_SUCCESS) {
        return st;
    }
    for (i = 0; i < handle_count; i++) {
        FW_DEVICE_PATH_NODE *candidate;
        UINTN matched;

        if (!handle_supports_protocol(handles[i], Protocol, NULL) ||
            !handle_supports_protocol(handles[i],
                                      (void *)mDevicePathProtocolGuid,
                                      (VOID **)&candidate) ||
            fw_device_path_size(candidate) == 0) {
            continue;
        }
        matched = fw_device_path_prefix_length(candidate, path);
        if (matched > best_match) {
            best_match = matched;
            best_handle = handles[i];
        }
    }
    (void)bs_free_pool(handles);

    if (best_handle == NULL) {
        best_handle = fw_locate_short_hard_drive_path(path, Protocol);
        if (best_handle != NULL) {
            *Device = best_handle;
            *DevicePath = (UINT8 *)path + path->Length;
            return EFI_SUCCESS;
        }
        /* A media-only file path is relative to the boot filesystem. */
        if (path->Type == 0x04 && path->SubType == 0x04 &&
            mBlockIoHandle != NULL &&
            handle_supports_protocol(mBlockIoHandle, Protocol, NULL)) {
            *Device = mBlockIoHandle;
            return EFI_SUCCESS;
        }
        return EFI_NOT_FOUND;
    }

    *Device = best_handle;
    *DevicePath = (UINT8 *)path + best_match;
    if (best_handle == mRawBlockIoHandle &&
        fw_cdrom_node_is_whole_media((FW_DEVICE_PATH_NODE *)*DevicePath)) {
        *DevicePath = (UINT8 *)*DevicePath +
                      ((FW_DEVICE_PATH_NODE *)*DevicePath)->Length;
    }
    return EFI_SUCCESS;
}

UINT8 fw_ascii_upper(UINT8 c)
{
    return (c >= 'a' && c <= 'z') ? (UINT8)(c - ('a' - 'A')) : c;
}

static BOOLEAN fw_char16_component_eq_ascii(const CHAR16 *name, UINTN len,
                                            const char *ascii)
{
    UINTN i;

    for (i = 0; i < len; i++) {
        UINT8 a = (UINT8)(name[i] & 0xff);
        UINT8 b = (UINT8)ascii[i];
        if (b == 0 || fw_ascii_upper(a) != fw_ascii_upper(b)) {
            return 0;
        }
    }
    return ascii[len] == 0;
}

static BOOLEAN fw_device_path_file_name_eq_ascii(const FW_DEVICE_PATH_NODE *path,
                                                 const char *ascii)
{
    const UINT8 *bytes = (const UINT8 *)path;
    UINTN path_size;
    UINTN walked = 0;

    if (path == NULL || ascii == NULL) {
        return 0;
    }
    path_size = fw_device_path_size(path);
    if (path_size == 0) {
        return 0;
    }

    while (walked + sizeof(FW_DEVICE_PATH_NODE) <= path_size) {
        const FW_DEVICE_PATH_NODE *node =
            (const FW_DEVICE_PATH_NODE *)(bytes + walked);
        UINTN char_count;
        const CHAR16 *name;
        UINTN name_len = 0;
        UINTN component = 0;
        UINTN i;

        if (node->Length < sizeof(FW_DEVICE_PATH_NODE)) {
            return 0;
        }
        if (fw_device_path_is_end(node)) {
            return 0;
        }
        if (node->Type == 0x04 && node->SubType == 0x04) {
            char_count = (node->Length - sizeof(FW_DEVICE_PATH_NODE)) /
                         sizeof(CHAR16);
            name = (const CHAR16 *)(const VOID *)(bytes + walked +
                                                  sizeof(FW_DEVICE_PATH_NODE));
            for (i = 0; i < char_count; i++) {
                if (name[i] == 0) {
                    break;
                }
                name_len++;
                if (name[i] == '\\' || name[i] == '/') {
                    component = i + 1U;
                }
            }
            if (component <= name_len &&
                fw_char16_component_eq_ascii(name + component,
                                             name_len - component, ascii)) {
                return 1;
            }
        }
        walked += node->Length;
    }

    return 0;
}

static BOOLEAN fw_fat_short_name_matches(const FAT_DIR_ENTRY *e,
                                         const CHAR16 *name, UINTN len)
{
    UINTN name_pos = 0;
    UINTN entry_pos;

    for (entry_pos = 0; entry_pos < 8; entry_pos++) {
        if (name_pos < len && name[name_pos] != '.') {
            UINT8 c = fw_ascii_upper((UINT8)(name[name_pos] & 0xff));
            if (e->name[entry_pos] != c) {
                return 0;
            }
            name_pos++;
        } else if (e->name[entry_pos] != ' ') {
            return 0;
        }
    }
    if (name_pos < len && name[name_pos] == '.') {
        name_pos++;
    }
    for (entry_pos = 8; entry_pos < 11; entry_pos++) {
        if (name_pos < len) {
            UINT8 c = fw_ascii_upper((UINT8)(name[name_pos] & 0xff));
            if (e->name[entry_pos] != c) {
                return 0;
            }
            name_pos++;
        } else if (e->name[entry_pos] != ' ') {
            return 0;
        }
    }
    return name_pos == len;
}

static void fw_fat_lfn_copy_part(char *lfn, const UINT8 *e)
{
    static const UINT8 pos[13] = {
        1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30
    };
    UINTN seq = (e[0] & 0x1f);
    UINTN base;
    UINTN i;

    if (seq == 0) {
        return;
    }
    base = (seq - 1) * 13;
    for (i = 0; i < 13 && base + i < 127; i++) {
        UINT16 ch = fw_le16(e + pos[i]);
        if (ch == 0 || ch == 0xffff) {
            lfn[base + i] = 0;
            break;
        }
        lfn[base + i] = (char)(ch & 0xff);
    }
}

static BOOLEAN fw_fat_lfn_matches(const char *lfn, const CHAR16 *name,
                                  UINTN len)
{
    if (lfn[0] == 0) {
        return 0;
    }
    return fw_char16_component_eq_ascii(name, len, lfn);
}

static BOOLEAN fw_fat_volume_init(FW_FAT_VOLUME *Volume,
                                  EFI_HANDLE Handle,
                                  EFI_BLOCK_IO_PROTOCOL *Block,
                                  UINT32 LbaOffset)
{
    UINT8 sec[512];
    UINT32 total_sectors;
    UINT32 fat_size;
    UINT32 root_dir_sectors;
    UINT32 data_start;
    UINT32 data_sectors;
    UINT32 cluster_count;
    UINT16 bytes_per_sector;
    UINT16 reserved;
    UINT16 root_entries;
    UINT16 total_small;
    UINT16 fat_small;
    UINT8 sectors_per_cluster;
    UINT8 fats;

    if (Volume == NULL || Block == NULL || Block->Media == NULL ||
        Block->Media->BlockSize != 512U || !Block->Media->MediaPresent ||
        LbaOffset > Block->Media->LastBlock ||
        Block->ReadBlocks(Block, Block->Media->MediaId, LbaOffset,
                          sizeof(sec), sec) != EFI_SUCCESS) {
        return 0;
    }
    bytes_per_sector = fw_le16(sec + 11);
    sectors_per_cluster = sec[13];
    reserved = fw_le16(sec + 14);
    fats = sec[16];
    root_entries = fw_le16(sec + 17);
    total_small = fw_le16(sec + 19);
    fat_small = fw_le16(sec + 22);
    total_sectors = total_small != 0 ? total_small : fw_le32(sec + 32);
    fat_size = fat_small != 0 ? fat_small : fw_le32(sec + 36);
    if (bytes_per_sector != 512U || sectors_per_cluster == 0 ||
        (sectors_per_cluster & (sectors_per_cluster - 1U)) != 0 ||
        sectors_per_cluster > 128U || reserved == 0 ||
        (fats != 1U && fats != 2U) || total_sectors == 0 || fat_size == 0 ||
        (UINT64)LbaOffset + total_sectors - 1U > Block->Media->LastBlock) {
        return 0;
    }
    root_dir_sectors = ((UINT32)root_entries * 32U + 511U) >> 9;
    if ((UINT64)reserved + (UINT64)fats * fat_size + root_dir_sectors >=
        total_sectors) {
        return 0;
    }
    data_start = reserved + (UINT32)fats * fat_size + root_dir_sectors;
    data_sectors = total_sectors - data_start;
    cluster_count = fw_udiv32(data_sectors, sectors_per_cluster);
    if (cluster_count == 0) {
        return 0;
    }

    fw_set_mem(Volume, sizeof(*Volume), 0);
    Volume->handle = Handle;
    Volume->block_io = Block;
    Volume->lba_offset = LbaOffset;
    Volume->sec_per_cluster = sectors_per_cluster;
    Volume->reserved_secs = reserved;
    Volume->num_fats = fats;
    Volume->root_entries = root_entries;
    Volume->secs_per_fat = fat_size;
    Volume->root_dir_sectors = root_dir_sectors;
    Volume->root_dir_start = reserved + (UINT32)fats * fat_size;
    Volume->data_start = data_start;
    Volume->cluster_size = (UINT32)sectors_per_cluster << 9;
    Volume->total_sectors = total_sectors;
    Volume->cluster_count = cluster_count;
    Volume->fat_type = cluster_count < 4085U ? 12U :
                       (cluster_count < 65525U ? 16U : 32U);
    Volume->is_fat16 = Volume->fat_type == 16U;
    Volume->is_fat32 = Volume->fat_type == 32U;
    Volume->eoc_cluster = Volume->fat_type == 12U ? 0x0ff8U :
                          (Volume->fat_type == 16U ? 0xfff8U : 0x0ffffff8U);
    if (Volume->is_fat32) {
        if (fat_small != 0 || root_entries != 0 ||
            fw_le16(sec + 42) != 0) {
            fw_set_mem(Volume, sizeof(*Volume), 0);
            return 0;
        }
        Volume->root_cluster = fw_le32(sec + 44) & 0x0fffffffU;
        if (Volume->root_cluster < 2U ||
            Volume->root_cluster >= cluster_count + 2U) {
            fw_set_mem(Volume, sizeof(*Volume), 0);
            return 0;
        }
    } else if (fat_small == 0 || root_entries == 0) {
        fw_set_mem(Volume, sizeof(*Volume), 0);
        return 0;
    }
    if (sec[Volume->is_fat32 ? 66U : 38U] == 0x29U) {
        fw_padded_ascii_to_char16(sec + (Volume->is_fat32 ? 71U : 43U),
                                  11U, Volume->label,
                                  FW_ARRAY_SIZE(Volume->label));
    }
    Volume->simple_fs.Revision = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_REVISION;
    Volume->simple_fs.OpenVolume = fat_open_volume;
    Volume->valid = 1;
    return 1;
}

static BOOLEAN fw_fat_init(void)
{
    if (mBootFatVolume.valid) {
        mDefaultFatVolume = &mBootFatVolume;
        return 1;
    }
    if (!mBootFatChecked) {
        mBootFatChecked = 1;
        if (mBlockIoHandle != NULL &&
            fw_fat_volume_init(&mBootFatVolume, mBlockIoHandle,
                               &mBlockIoProto, 0)) {
            mDefaultFatVolume = &mBootFatVolume;
            return 1;
        }
    }
    return mDefaultFatVolume != NULL && mDefaultFatVolume->valid;
}

BOOLEAN fw_boot_fat_available(void)
{
    return fw_fat_init() && mBootFatVolume.valid;
}

/*
 * True when the disc's ISO-9660/UDF file system should be presented on the
 * boot Block I/O handle (mBlockIoHandle) rather than the raw optical handle.
 *
 * This holds for optical media with no mappable EFI El Torito boot image -- a
 * combo IA-64 install disc whose only boot catalog entry is the legacy x86
 * CDBOOT loader, so there is no EFI System Partition and no FAT volume.  In
 * that case mBlockIoHandle carries the whole-media MEDIA_CDROM_DP device path
 * (see fw_update_storage_device_paths / handle_supports_protocol), so a loader
 * launched from this file system -- e.g. \IA64\SETUPLDR.EFI run by hand from
 * the EFI shell -- receives a CD-ROM device path.  Microsoft's setupldr needs
 * that to take its El Torito CD boot path (WSRV03 base/boot/efi/sumain.c), and
 * it matches the device path the reference firmware exposes from its El Torito
 * partition child handle (EFI 1.10 EDK/Drivers/Partition/ElTorito.c).
 *
 * When an EFI El Torito FAT volume *is* mapped, mBlockIoHandle carries that FAT
 * file system and the ISO/UDF file system stays on the raw optical handle.
 */
BOOLEAN fw_boot_optical_fs_available(void)
{
    return storage_is_cd(&mBootStorageDevice) && !mBootImageMapped &&
           !fw_boot_fat_available() && (fw_udf_init() || fw_iso_init());
}

static BOOLEAN fw_fat_read_512(FW_FAT_VOLUME *Volume, UINT8 *buf, UINT32 lba)
{
    if (Volume == NULL || !Volume->valid || Volume->block_io == NULL ||
        lba >= Volume->total_sectors ||
        lba > 0xffffffffU - Volume->lba_offset) {
        return 0;
    }
    return Volume->block_io->ReadBlocks(
               Volume->block_io, Volume->block_io->Media->MediaId,
               Volume->lba_offset + lba, 512U, buf) == EFI_SUCCESS;
}

static BOOLEAN fw_fat_read_512s(FW_FAT_VOLUME *Volume, UINT8 *buf,
                                UINT32 lba, UINT32 count)
{
    if (count == 0) {
        return 1;
    }
    if (Volume == NULL || !Volume->valid || Volume->block_io == NULL ||
        lba >= Volume->total_sectors ||
        count - 1U > Volume->total_sectors - lba - 1U ||
        lba > 0xffffffffU - Volume->lba_offset ||
        count - 1U > 0xffffffffU - Volume->lba_offset - lba) {
        return 0;
    }
    return Volume->block_io->ReadBlocks(
               Volume->block_io, Volume->block_io->Media->MediaId,
               Volume->lba_offset + lba, (UINTN)count * 512U,
               buf) == EFI_SUCCESS;
}

static const UINT8 *fw_fat_read_table_sector(FW_FAT_VOLUME *Volume,
                                             UINT32 lba)
{
    UINT32 media_id;

    if (Volume == NULL || !Volume->valid || Volume->block_io == NULL ||
        Volume->block_io->Media == NULL) {
        return NULL;
    }
    media_id = Volume->block_io->Media->MediaId;
    if (Volume->fat_cache_valid &&
        Volume->fat_cache_media_id == media_id &&
        Volume->fat_cache_lba == lba) {
        return Volume->fat_cache;
    }

    Volume->fat_cache_valid = 0;
    if (!fw_fat_read_512(Volume, Volume->fat_cache, lba)) {
        return NULL;
    }
    Volume->fat_cache_media_id = media_id;
    Volume->fat_cache_lba = lba;
    Volume->fat_cache_valid = 1;
    return Volume->fat_cache;
}

static BOOLEAN fw_fat_is_data_cluster(FW_FAT_VOLUME *Volume, UINT32 cluster)
{
    return Volume != NULL && cluster >= 2U &&
           cluster < Volume->cluster_count + 2U &&
           cluster < Volume->eoc_cluster;
}

static UINT32 fw_fat_next_cluster(FW_FAT_VOLUME *Volume, UINT32 cluster)
{
    const UINT8 *sec;
    UINT32 offset;
    UINT32 lba;
    UINT32 pos;

    if (Volume == NULL || !fw_fat_is_data_cluster(Volume, cluster)) {
        return 0xffffffffU;
    }
    if (Volume->fat_type == 12U) {
        UINT8 b0;
        UINT8 b1;
        UINT16 value;

        offset = cluster + (cluster >> 1);
        lba = Volume->reserved_secs + (offset / 512U);
        pos = offset & 511U;
        sec = fw_fat_read_table_sector(Volume, lba);
        if (sec == NULL) {
            return 0xffffffffU;
        }
        b0 = sec[pos];
        if (pos == 511U) {
            sec = fw_fat_read_table_sector(Volume, lba + 1U);
            if (sec == NULL) {
                return 0xffffffffU;
            }
            b1 = sec[0];
        } else {
            b1 = sec[pos + 1U];
        }
        value = (UINT16)(b0 | ((UINT16)b1 << 8));
        return (cluster & 1U) != 0 ? (value >> 4) : (value & 0x0fffU);
    }

    offset = cluster * (Volume->fat_type == 16U ? 2U : 4U);
    lba = Volume->reserved_secs + offset / 512U;
    sec = fw_fat_read_table_sector(Volume, lba);
    if (sec == NULL) {
        return 0xffffffffU;
    }
    if (Volume->fat_type == 16U) {
        return fw_le16(sec + (offset & 511U));
    }
    return fw_le32(sec + (offset & 511U)) & 0x0fffffffU;
}

static UINT32 fw_fat_entry_cluster(FW_FAT_VOLUME *Volume,
                                   const FAT_DIR_ENTRY *Entry)
{
    UINT32 cluster = Entry->cluster_lo;

    if (Volume != NULL && Volume->is_fat32) {
        cluster |= (UINT32)Entry->cluster_hi << 16;
    }
    return cluster;
}

static BOOLEAN fw_fat_find_in_dir(FW_FAT_VOLUME *Volume, BOOLEAN root,
                                  UINT32 dir_cluster, const CHAR16 *name,
                                  UINTN len, FAT_DIR_ENTRY *out)
{
    UINT8 sec[512];
    char lfn[128];
    UINT32 root_lba;
    UINT32 root_end;
    UINT32 cluster;
    UINTN s;
    UINTN off;

    fw_set_mem(lfn, sizeof(lfn), 0);

    if (root && !Volume->is_fat32) {
        root_lba = Volume->root_dir_start;
        root_end = root_lba + Volume->root_dir_sectors;
        for (; root_lba < root_end; root_lba++) {
            if (!fw_fat_read_512(Volume, sec, root_lba)) {
                return 0;
            }
            for (off = 0; off + sizeof(FAT_DIR_ENTRY) <= 512; off += 32) {
                FAT_DIR_ENTRY *e = (FAT_DIR_ENTRY *)(sec + off);
                if (e->name[0] == 0x00) {
                    return 0;
                }
                if (e->name[0] == 0xe5) {
                    fw_set_mem(lfn, sizeof(lfn), 0);
                    continue;
                }
                if (e->attr == 0x0f) {
                    fw_fat_lfn_copy_part(lfn, (UINT8 *)e);
                    continue;
                }
                if (fw_fat_short_name_matches(e, name, len) ||
                    fw_fat_lfn_matches(lfn, name, len)) {
                    fw_copy_mem(out, e, sizeof(*out));
                    return 1;
                }
                fw_set_mem(lfn, sizeof(lfn), 0);
            }
        }
        return 0;
    }

    cluster = root ? Volume->root_cluster : dir_cluster;
    while (fw_fat_is_data_cluster(Volume, cluster)) {
        for (s = 0; s < Volume->sec_per_cluster; s++) {
            UINT32 lba = Volume->data_start +
                         (cluster - 2U) * Volume->sec_per_cluster + s;
            if (!fw_fat_read_512(Volume, sec, lba)) {
                return 0;
            }
            for (off = 0; off + sizeof(FAT_DIR_ENTRY) <= 512; off += 32) {
                FAT_DIR_ENTRY *e = (FAT_DIR_ENTRY *)(sec + off);
                if (e->name[0] == 0x00) {
                    return 0;
                }
                if (e->name[0] == 0xe5) {
                    fw_set_mem(lfn, sizeof(lfn), 0);
                    continue;
                }
                if (e->attr == 0x0f) {
                    fw_fat_lfn_copy_part(lfn, (UINT8 *)e);
                    continue;
                }
                if (fw_fat_short_name_matches(e, name, len) ||
                    fw_fat_lfn_matches(lfn, name, len)) {
                    fw_copy_mem(out, e, sizeof(*out));
                    return 1;
                }
                fw_set_mem(lfn, sizeof(lfn), 0);
            }
        }
        cluster = fw_fat_next_cluster(Volume, cluster);
    }
    return 0;
}

static EFI_STATUS fw_fat_lookup_volume(FW_FAT_VOLUME *Volume, FW_FILE *Base,
                                       CHAR16 *path, FAT_DIR_ENTRY *out)
{
    CHAR16 *p = path;
    BOOLEAN root = 1;
    UINT32 dir_cluster = 0;
    FAT_DIR_ENTRY e;

    if (Volume == NULL || !Volume->valid || path == NULL || out == NULL) {
        return EFI_NOT_FOUND;
    }

    if (Base != NULL && Base->fs_kind == FW_FS_FAT && Base->is_dir &&
        Base->fat_volume == Volume && *p != '\\' && *p != '/') {
        root = Base->is_root;
        dir_cluster = Base->first_cluster;
    }

    while (*p == '\\' || *p == '/') {
        p++;
    }
    if (*p == 0) {
        fw_set_mem(&e, sizeof(e), 0);
        e.attr = 0x10;
        e.cluster_lo = (UINT16)Volume->root_cluster;
        e.cluster_hi = (UINT16)(Volume->root_cluster >> 16);
        e.size = Volume->is_fat32 ? 0 :
                 Volume->root_dir_sectors * 512U;
        fw_copy_mem(out, &e, sizeof(e));
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
        if (len == 1U && start[0] == '.') {
            while (*p == '\\' || *p == '/') {
                p++;
            }
            if (*p == 0) {
                return EFI_INVALID_PARAMETER;
            }
            continue;
        }
        if (!fw_fat_find_in_dir(Volume, root, dir_cluster, start, len, &e)) {
            return EFI_NOT_FOUND;
        }
        while (*p == '\\' || *p == '/') {
            p++;
        }
        if (*p == 0) {
            fw_copy_mem(out, &e, sizeof(e));
            return EFI_SUCCESS;
        }
        if ((e.attr & 0x10) == 0) {
            return EFI_NOT_FOUND;
        }
        root = 0;
        dir_cluster = fw_fat_entry_cluster(Volume, &e);
    }
}

EFI_STATUS fw_fat_lookup(CHAR16 *path, FAT_DIR_ENTRY *out)
{
    if (!fw_fat_init()) {
        return EFI_NOT_FOUND;
    }
    return fw_fat_lookup_volume(mDefaultFatVolume, NULL, path, out);
}

static EFI_STATUS fw_fat_read_file_entry_volume(FW_FAT_VOLUME *Volume,
                                                const FAT_DIR_ENTRY *entry,
                                                VOID *Buffer,
                                                UINT32 *ReadSize)
{
    UINT8 *dst = (UINT8 *)Buffer;
    UINT32 done = 0;
    UINT32 want;
    UINT32 cluster;

    if (entry == NULL || Buffer == NULL || ReadSize == NULL) {
        return EFI_INVALID_PARAMETER;
    }
    if (Volume == NULL || !Volume->valid) {
        return EFI_NOT_FOUND;
    }
    if ((entry->attr & 0x10) != 0) {
        return EFI_INVALID_PARAMETER;
    }

    want = entry->size;
    cluster = fw_fat_entry_cluster(Volume, entry);
    while (done < want && fw_fat_is_data_cluster(Volume, cluster)) {
        UINTN s;
        for (s = 0; s < Volume->sec_per_cluster && done < want; s++) {
            UINT8 sec[512];
            UINT32 lba = Volume->data_start +
                         (cluster - 2U) * Volume->sec_per_cluster + s;
            UINT32 chunk = want - done;
            UINT32 sectors;
            if (chunk > 512) {
                chunk = 512;
            }
            sectors = (want - done) / 512;
            if (sectors > Volume->sec_per_cluster - s) {
                sectors = Volume->sec_per_cluster - s;
            }
            if (sectors > 0) {
                if (!fw_fat_read_512s(Volume, dst + done, lba, sectors)) {
                    *ReadSize = done;
                    return EFI_DEVICE_ERROR;
                }
                done += sectors * 512;
                s += sectors - 1U;
                continue;
            }
            if (!fw_fat_read_512(Volume, sec, lba)) {
                *ReadSize = done;
                return EFI_DEVICE_ERROR;
            }
            fw_copy_mem(dst + done, sec, chunk);
            done += chunk;
        }
        if (done < want) {
            cluster = fw_fat_next_cluster(Volume, cluster);
        }
    }

    *ReadSize = done;
    return done == want ? EFI_SUCCESS : EFI_DEVICE_ERROR;
}

EFI_STATUS fw_fat_read_file_entry(const FAT_DIR_ENTRY *entry,
                                         VOID *Buffer, UINT32 *ReadSize)
{
    if (!fw_fat_init()) {
        return EFI_NOT_FOUND;
    }
    return fw_fat_read_file_entry_volume(mDefaultFatVolume, entry, Buffer,
                                         ReadSize);
}

static BOOLEAN fw_iso_read_sector(UINT8 *buf, UINT32 lba)
{
    if (!storage_is_cd(&mBootStorageDevice) || buf == NULL) {
        return 0;
    }
    return storage_read_blocks(&mBootStorageDevice, buf, lba, 1);
}

BOOLEAN fw_iso_read_sectors(UINT8 *buf, UINT32 lba, UINT32 count)
{
    if (count == 0) {
        return 1;
    }
    if (!storage_is_cd(&mBootStorageDevice) || buf == NULL) {
        return 0;
    }
    return storage_read_blocks(&mBootStorageDevice, buf, lba, count);
}

BOOLEAN fw_iso_init(void)
{
    static UINT8 sec[ATAPI_SECTOR_SIZE];
    UINTN i;

    if (mIsoVolume.valid) {
        return 1;
    }
    if (!storage_is_cd(&mBootStorageDevice)) {
        return 0;
    }

    for (i = 16; i < 32; i++) {
        if (!fw_iso_read_sector(sec, (UINT32)i)) {
            return 0;
        }
        if (!fw_bytes_eq(sec + 1, "CD001", 5)) {
            continue;
        }
        if (sec[0] == 1) {
            UINT8 *root = sec + 156;

            if (root[0] < 34 || root[32] == 0) {
                return 0;
            }
            mIsoVolume.root_extent = fw_le32(root + 2);
            mIsoVolume.root_size = fw_le32(root + 10);
            if (mIsoVolume.root_extent == 0 || mIsoVolume.root_size == 0) {
                return 0;
            }
            fw_padded_ascii_to_char16(sec + 40U, 32U, mIsoVolume.label,
                                      FW_ARRAY_SIZE(mIsoVolume.label));
            mIsoVolume.valid = 1;
            return 1;
        }
        if (sec[0] == 0xff) {
            break;
        }
    }

    return 0;
}

static UINTN fw_iso_record_name_len(const UINT8 *name, UINTN len)
{
    UINTN i;

    for (i = 0; i < len; i++) {
        if (name[i] == ';') {
            break;
        }
    }
    return i;
}

static UINTN fw_char16_component_len(const CHAR16 *name, UINTN len)
{
    UINTN i;

    for (i = 0; i < len; i++) {
        if (name[i] == ';') {
            break;
        }
    }
    return i;
}

static void fw_iso_copy_record_name(CHAR16 *dst, UINTN dst_chars,
                                    const UINT8 *name, UINTN len)
{
    UINTN out_len;
    UINTN i;

    if (dst == NULL || dst_chars == 0) {
        return;
    }
    if (len == 1 && name[0] == 0) {
        if (dst_chars > 1) {
            dst[0] = '.';
            dst[1] = 0;
        } else {
            dst[0] = 0;
        }
        return;
    }
    if (len == 1 && name[0] == 1) {
        if (dst_chars > 2) {
            dst[0] = '.';
            dst[1] = '.';
            dst[2] = 0;
        } else {
            dst[0] = 0;
        }
        return;
    }

    out_len = fw_iso_record_name_len(name, len);
    for (i = 0; i + 1 < dst_chars && i < out_len; i++) {
        dst[i] = (CHAR16)name[i];
    }
    dst[i] = 0;
}

static BOOLEAN fw_iso_name_matches(const UINT8 *iso_name, UINTN iso_len,
                                   const CHAR16 *name, UINTN len)
{
    UINTN match_len;
    UINTN name_len;
    UINTN i;

    if (iso_len == 1 && (iso_name[0] == 0 || iso_name[0] == 1)) {
        return 0;
    }

    match_len = fw_iso_record_name_len(iso_name, iso_len);
    name_len = fw_char16_component_len(name, len);
    if (match_len != name_len) {
        return 0;
    }

    for (i = 0; i < match_len; i++) {
        UINT8 a = fw_ascii_upper(iso_name[i]);
        UINT8 b = fw_ascii_upper((UINT8)(name[i] & 0xff));

        if (a != b) {
            return 0;
        }
    }
    return 1;
}

static EFI_STATUS fw_iso_next_dir_entry(UINT32 dir_extent, UINT32 dir_size,
                                        UINT32 *Position, FW_ISO_ENTRY *out)
{
    UINT8 sec[ATAPI_SECTOR_SIZE];
    UINT32 pos;

    if (Position == NULL || out == NULL) {
        return EFI_INVALID_PARAMETER;
    }
    pos = *Position;

    while (pos < dir_size) {
        UINT32 off = pos & (ATAPI_SECTOR_SIZE - 1);
        UINT8 rec_len;
        UINT8 name_len;
        UINT8 *rec;

        if (!fw_iso_read_sector(sec, dir_extent + (pos / ATAPI_SECTOR_SIZE))) {
            return EFI_DEVICE_ERROR;
        }

        rec_len = sec[off];
        if (rec_len == 0) {
            pos = (pos + ATAPI_SECTOR_SIZE) & ~(ATAPI_SECTOR_SIZE - 1U);
            continue;
        }
        if (rec_len < 34 || off + rec_len > ATAPI_SECTOR_SIZE) {
            return EFI_VOLUME_CORRUPTED;
        }

        rec = sec + off;
        name_len = rec[32];
        if ((UINTN)33 + name_len > rec_len) {
            return EFI_VOLUME_CORRUPTED;
        }
        pos += rec_len;

        if (name_len == 1 && (rec[33] == 0 || rec[33] == 1)) {
            continue;
        }

        out->extent = fw_le32(rec + 2);
        out->size = fw_le32(rec + 10);
        out->flags = rec[25];
        fw_iso_copy_record_name(out->name, sizeof(out->name) / sizeof(out->name[0]),
                                rec + 33, name_len);
        *Position = pos;
        return EFI_SUCCESS;
    }

    *Position = pos;
    return EFI_NOT_FOUND;
}

static EFI_STATUS fw_iso_find_in_dir(UINT32 dir_extent, UINT32 dir_size,
                                     const CHAR16 *name, UINTN len,
                                     FW_ISO_ENTRY *out)
{
    UINT8 sec[ATAPI_SECTOR_SIZE];
    UINT32 pos = 0;

    if (name == NULL || out == NULL) {
        return EFI_INVALID_PARAMETER;
    }

    while (pos < dir_size) {
        UINT32 off = pos & (ATAPI_SECTOR_SIZE - 1);
        UINT8 rec_len;
        UINT8 name_len;
        UINT8 *rec;

        if (!fw_iso_read_sector(sec, dir_extent + (pos / ATAPI_SECTOR_SIZE))) {
            return EFI_DEVICE_ERROR;
        }

        rec_len = sec[off];
        if (rec_len == 0) {
            pos = (pos + ATAPI_SECTOR_SIZE) & ~(ATAPI_SECTOR_SIZE - 1U);
            continue;
        }
        if (rec_len < 34 || off + rec_len > ATAPI_SECTOR_SIZE) {
            return EFI_VOLUME_CORRUPTED;
        }

        rec = sec + off;
        name_len = rec[32];
        if ((UINTN)33 + name_len > rec_len) {
            return EFI_VOLUME_CORRUPTED;
        }

        if (fw_iso_name_matches(rec + 33, name_len, name, len)) {
            out->extent = fw_le32(rec + 2);
            out->size = fw_le32(rec + 10);
            out->flags = rec[25];
            fw_iso_copy_record_name(out->name,
                                    sizeof(out->name) / sizeof(out->name[0]),
                                    rec + 33, name_len);
            return EFI_SUCCESS;
        }
        pos += rec_len;
    }

    return EFI_NOT_FOUND;
}

static EFI_STATUS fw_iso_lookup(FW_FILE *Base, CHAR16 *path, FW_ISO_ENTRY *out)
{
    UINT32 dir_extent;
    UINT32 dir_size;
    CHAR16 *p;
    FW_ISO_ENTRY entry;

    if (path == NULL || out == NULL) {
        return EFI_INVALID_PARAMETER;
    }
    if (!fw_iso_init()) {
        return EFI_NOT_FOUND;
    }

    p = path;
    if (Base != NULL && Base->fs_kind == FW_FS_ISO && Base->is_dir &&
        *p != '\\' && *p != '/') {
        dir_extent = Base->extent;
        dir_size = Base->size;
    } else {
        dir_extent = mIsoVolume.root_extent;
        dir_size = mIsoVolume.root_size;
    }

    while (*p == '\\' || *p == '/') {
        p++;
    }
    if (*p == 0) {
        out->extent = mIsoVolume.root_extent;
        out->size = mIsoVolume.root_size;
        out->flags = 0x02;
        out->name[0] = 0;
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
                out->extent = dir_extent;
                out->size = dir_size;
                out->flags = 0x02;
                out->name[0] = '.';
                out->name[1] = 0;
                return EFI_SUCCESS;
            }
            continue;
        }

        {
            EFI_STATUS st = fw_iso_find_in_dir(dir_extent, dir_size,
                                               start, len, &entry);
            if (st != EFI_SUCCESS) {
                return st;
            }
        }

        while (*p == '\\' || *p == '/') {
            p++;
        }
        if (*p == 0) {
            fw_copy_mem(out, &entry, sizeof(entry));
            return EFI_SUCCESS;
        }
        if ((entry.flags & 0x02) == 0) {
            return EFI_NOT_FOUND;
        }
        dir_extent = entry.extent;
        dir_size = entry.size;
    }
}

static EFI_STATUS fw_iso_read_extent(UINT32 extent, UINT32 size,
                                     UINT32 position, VOID *Buffer,
                                     UINT32 *ReadSize)
{
    UINT8 *dst = (UINT8 *)Buffer;
    UINT32 done = 0;
    UINT32 want;

    if (Buffer == NULL || ReadSize == NULL || position > size) {
        return EFI_INVALID_PARAMETER;
    }

    want = *ReadSize;
    if (want > size - position) {
        want = size - position;
    }

    while (done < want) {
        UINT8 sec[ATAPI_SECTOR_SIZE];
        UINT32 file_off = position + done;
        UINT32 sector_off = file_off & (ATAPI_SECTOR_SIZE - 1);
        UINT32 chunk = ATAPI_SECTOR_SIZE - sector_off;

        if (chunk > want - done) {
            chunk = want - done;
        }
        if (sector_off == 0 && chunk == ATAPI_SECTOR_SIZE) {
            UINT32 sectors = (want - done) / ATAPI_SECTOR_SIZE;

            if (!fw_iso_read_sectors(dst + done,
                                     extent +
                                     (file_off / ATAPI_SECTOR_SIZE),
                                     sectors)) {
                *ReadSize = done;
                return EFI_DEVICE_ERROR;
            }
            done += sectors * ATAPI_SECTOR_SIZE;
            continue;
        }
        if (!fw_iso_read_sector(sec, extent + (file_off / ATAPI_SECTOR_SIZE))) {
            *ReadSize = done;
            return EFI_DEVICE_ERROR;
        }
        fw_copy_mem(dst + done, sec + sector_off, chunk);
        done += chunk;
    }

    *ReadSize = done;
    return EFI_SUCCESS;
}

/* --- UDF 2.01 optical filesystem lives in udf.c -------------------------- */


static FW_FILE *fw_file_from_proto(EFI_FILE_PROTOCOL *This)
{
    UINTN i;

    if (This == NULL) {
        return NULL;
    }
    for (i = 0; i < FW_ARRAY_SIZE(mFileHandles); i++) {
        if (mFileHandles[i].in_use && This == &mFileHandles[i].proto) {
            return &mFileHandles[i];
        }
    }
    return NULL;
}

static void fw_file_init_proto(FW_FILE *file);

static FW_FILE *fw_file_alloc(void)
{
    UINTN i;
    for (i = 0; i < FW_FILE_MAX; i++) {
        if (!mFileHandles[i].in_use) {
            fw_set_mem(&mFileHandles[i], sizeof(mFileHandles[i]), 0);
            fw_file_init_proto(&mFileHandles[i]);
            mFileHandles[i].in_use = 1;
            return &mFileHandles[i];
        }
    }
    return NULL;
}


static EFI_STATUS fs_file_close(EFI_FILE_PROTOCOL *This)
{
    FW_FILE *file = fw_file_from_proto(This);

    if (file == NULL) {
        return EFI_INVALID_PARAMETER;
    }
    file->in_use = 0;
    return EFI_SUCCESS;
}

static EFI_STATUS fs_file_delete(EFI_FILE_PROTOCOL *This)
{
    if (fs_file_close(This) != EFI_SUCCESS) {
        return EFI_WARN_DELETE_FAILURE;
    }
    return EFI_WARN_DELETE_FAILURE;
}

static EFI_STATUS fs_file_validate_open(UINT64 OpenMode, UINT64 Attributes)
{
    if (OpenMode != EFI_FILE_MODE_READ &&
        OpenMode != (EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE) &&
        OpenMode != (EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE |
                     EFI_FILE_MODE_CREATE)) {
        return EFI_INVALID_PARAMETER;
    }
    if ((Attributes & ~EFI_FILE_VALID_ATTR) != 0 ||
        ((OpenMode & EFI_FILE_MODE_CREATE) == 0 && Attributes != 0)) {
        return EFI_INVALID_PARAMETER;
    }
    if ((OpenMode & (EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE)) != 0) {
        return EFI_WRITE_PROTECTED;
    }
    return EFI_SUCCESS;
}

static VOID fw_file_set_name_from_path(FW_FILE *File, const CHAR16 *Path)
{
    const CHAR16 *name = Path;
    const CHAR16 *p = Path;
    UINTN i;

    while (*p != 0) {
        if (*p == '\\' || *p == '/') {
            name = p + 1;
        }
        p++;
    }
    for (i = 0; i + 1U < FW_ARRAY_SIZE(File->name) && name[i] != 0; i++) {
        File->name[i] = name[i];
    }
    File->name[i] = 0;
}

static EFI_STATUS fs_file_normalize_path(const FW_FILE *Base,
                                         const CHAR16 *Input,
                                         CHAR16 *Output, UINTN Capacity)
{
    const CHAR16 *p;
    UINTN length = 1;
    UINTN i;

    if (Base == NULL || !Base->is_dir || Input == NULL || Output == NULL ||
        Capacity < 2) {
        return EFI_INVALID_PARAMETER;
    }
    Output[0] = '\\';
    Output[1] = 0;
    if (*Input != '\\' && *Input != '/' && Base->path[0] == '\\') {
        for (length = 0;
             length + 1U < Capacity && Base->path[length] != 0;
             length++) {
            Output[length] = Base->path[length];
        }
        if (Base->path[length] != 0) {
            return EFI_INVALID_PARAMETER;
        }
        Output[length] = 0;
    }

    p = Input;
    while (*p == '\\' || *p == '/') {
        p++;
    }
    while (*p != 0) {
        const CHAR16 *component = p;
        UINTN component_length;

        while (*p != 0 && *p != '\\' && *p != '/') {
            p++;
        }
        component_length = (UINTN)(p - component);
        while (*p == '\\' || *p == '/') {
            p++;
        }
        if (component_length == 1 && component[0] == '.') {
            continue;
        }
        if (component_length == 2 && component[0] == '.' &&
            component[1] == '.') {
            if (length <= 1) {
                return EFI_NOT_FOUND;
            }
            while (length > 1 && Output[length - 1U] != '\\') {
                length--;
            }
            if (length > 1) {
                length--;
            }
            Output[length] = 0;
            continue;
        }
        if (component_length == 0) {
            continue;
        }
        if (length > 1) {
            if (length + 1U >= Capacity) {
                return EFI_INVALID_PARAMETER;
            }
            Output[length++] = '\\';
        }
        if (component_length >= Capacity - length) {
            return EFI_INVALID_PARAMETER;
        }
        for (i = 0; i < component_length; i++) {
            Output[length++] = component[i];
        }
        Output[length] = 0;
    }
    return EFI_SUCCESS;
}

static VOID fw_file_set_path(FW_FILE *File, const CHAR16 *Path)
{
    UINTN i;

    for (i = 0; i + 1U < FW_ARRAY_SIZE(File->path) && Path[i] != 0; i++) {
        File->path[i] = Path[i];
    }
    File->path[i] = 0;
    File->is_root = i == 1 && File->path[0] == '\\';
}

static EFI_STATUS fat_file_open(EFI_FILE_PROTOCOL *This,
                                EFI_FILE_HANDLE *NewHandle,
                                CHAR16 *FileName, UINT64 OpenMode,
                                UINT64 Attributes)
{
    FW_FILE *base = fw_file_from_proto(This);
    FW_FAT_VOLUME *volume = base != NULL ? base->fat_volume : NULL;
    FAT_DIR_ENTRY e;
    FW_FILE *file;
    EFI_STATUS st;
    (void)Attributes;

    if (base == NULL || NewHandle == NULL || FileName == NULL ||
        OpenMode != EFI_FILE_MODE_READ) {
        return EFI_INVALID_PARAMETER;
    }

    st = fw_fat_lookup_volume(volume, base, FileName, &e);
    if (st != EFI_SUCCESS) {
        return st;
    }

    file = fw_file_alloc();
    if (file == NULL) {
        return EFI_OUT_OF_RESOURCES;
    }
    file->is_dir = (e.attr & 0x10) != 0;
    file->fs_kind = FW_FS_FAT;
    file->fat_volume = volume;
    file->first_cluster = fw_fat_entry_cluster(volume, &e);
    file->size = e.size;
    file->position = 0;
    fw_file_set_name_from_path(file, FileName);
    fw_file_set_path(file, FileName);
    *NewHandle = &file->proto;
    return EFI_SUCCESS;
}

static UINT32 fat_file_cluster_at(FW_FILE *File, UINT32 Position,
                                  UINT32 *ClusterPosition,
                                  UINT32 *ClusterIndex)
{
    FW_FAT_VOLUME *volume = File != NULL ? File->fat_volume : NULL;
    UINT32 target_index;
    UINT32 current_index;
    UINT32 cluster;

    if (volume == NULL || !volume->valid || volume->cluster_size == 0) {
        return 0xffffffffU;
    }

    target_index = Position / volume->cluster_size;
    if (File->fat_cursor_valid &&
        File->fat_cursor_index <= target_index &&
        fw_fat_is_data_cluster(volume, File->fat_cursor_cluster)) {
        cluster = File->fat_cursor_cluster;
        current_index = File->fat_cursor_index;
    } else {
        cluster = File->first_cluster;
        current_index = 0;
    }

    while (current_index < target_index &&
           fw_fat_is_data_cluster(volume, cluster)) {
        cluster = fw_fat_next_cluster(volume, cluster);
        current_index++;
    }
    if (!fw_fat_is_data_cluster(volume, cluster)) {
        return 0xffffffffU;
    }

    File->fat_cursor_valid = 1;
    File->fat_cursor_cluster = cluster;
    File->fat_cursor_index = current_index;
    if (ClusterPosition != NULL) {
        *ClusterPosition = Position % volume->cluster_size;
    }
    if (ClusterIndex != NULL) {
        *ClusterIndex = current_index;
    }
    return cluster;
}

typedef struct {
    EFI_BLOCK_IO_PROTOCOL protocol;
    EFI_BLOCK_IO_MEDIA media;
    UINT8 fat_sector[512];
    UINT32 read_count;
} FW_FAT_CACHE_TEST;

static EFI_STATUS fat_cache_test_read(EFI_BLOCK_IO_PROTOCOL *This,
                                      UINT32 MediaId, UINT64 Lba,
                                      UINTN BufferSize, VOID *Buffer)
{
    FW_FAT_CACHE_TEST *test = (FW_FAT_CACHE_TEST *)This;

    if (MediaId != test->media.MediaId) {
        return EFI_MEDIA_CHANGED;
    }
    if (Lba != 1U || BufferSize != sizeof(test->fat_sector) ||
        Buffer == NULL) {
        return EFI_INVALID_PARAMETER;
    }
    fw_copy_mem(Buffer, test->fat_sector, sizeof(test->fat_sector));
    test->read_count++;
    return EFI_SUCCESS;
}

BOOLEAN fat_cursor_cache_selftest(VOID)
{
    FW_FAT_CACHE_TEST test;
    FW_FAT_VOLUME volume;
    FW_FILE file;
    UINT32 cluster_position;
    UINT32 cluster_index;

    fw_set_mem(&test, sizeof(test), 0);
    fw_set_mem(&volume, sizeof(volume), 0);
    fw_set_mem(&file, sizeof(file), 0);

    test.media.MediaId = 7U;
    test.media.MediaPresent = 1;
    test.media.BlockSize = 512U;
    test.media.LastBlock = 31U;
    test.protocol.Media = &test.media;
    test.protocol.ReadBlocks = fat_cache_test_read;
    test.fat_sector[4] = 3U;  /* cluster 2 -> 3 */
    test.fat_sector[6] = 4U;  /* cluster 3 -> 4 */
    test.fat_sector[8] = 5U;  /* cluster 4 -> 5 */
    test.fat_sector[10] = 0xf8U; /* cluster 5 -> end of chain */
    test.fat_sector[11] = 0xffU;

    volume.valid = 1;
    volume.fat_type = 16U;
    volume.sec_per_cluster = 1U;
    volume.reserved_secs = 1U;
    volume.eoc_cluster = 0xfff8U;
    volume.cluster_size = 512U;
    volume.total_sectors = 32U;
    volume.cluster_count = 16U;
    volume.block_io = &test.protocol;

    file.fs_kind = FW_FS_FAT;
    file.fat_volume = &volume;
    file.first_cluster = 2U;

    if (fat_file_cluster_at(&file, 2U * 512U, &cluster_position,
                            &cluster_index) != 4U ||
        cluster_position != 0U || cluster_index != 2U ||
        test.read_count != 1U ||
        fat_file_cluster_at(&file, 3U * 512U, &cluster_position,
                            &cluster_index) != 5U ||
        cluster_index != 3U || test.read_count != 1U ||
        fat_file_cluster_at(&file, 512U, &cluster_position,
                            &cluster_index) != 3U ||
        cluster_index != 1U || test.read_count != 1U) {
        return 0;
    }

    test.fat_sector[6] = 6U;  /* changed medium: cluster 3 -> 6 */
    test.media.MediaId++;
    return fat_file_cluster_at(&file, 2U * 512U, &cluster_position,
                               &cluster_index) == 6U &&
           cluster_index == 2U && test.read_count == 2U;
}

static EFI_STATUS iso_file_open(EFI_FILE_PROTOCOL *This,
                                EFI_FILE_HANDLE *NewHandle,
                                CHAR16 *FileName, UINT64 OpenMode,
                                UINT64 Attributes)
{
    FW_FILE *base = fw_file_from_proto(This);
    FW_ISO_ENTRY entry;
    FW_FILE *file;
    UINTN i;
    EFI_STATUS st;
    (void)Attributes;

    if (base == NULL || NewHandle == NULL || FileName == NULL ||
        OpenMode != EFI_FILE_MODE_READ) {
        return EFI_INVALID_PARAMETER;
    }

    st = fw_iso_lookup(base, FileName, &entry);
    if (st != EFI_SUCCESS) {
        return st;
    }

    file = fw_file_alloc();
    if (file == NULL) {
        return EFI_OUT_OF_RESOURCES;
    }
    file->is_dir = (entry.flags & 0x02) != 0;
    file->fs_kind = FW_FS_ISO;
    file->extent = entry.extent;
    file->size = entry.size;
    file->position = 0;
    for (i = 0; i < 63 && entry.name[i] != 0; i++) {
        file->name[i] = entry.name[i];
    }
    file->name[i] = 0;
    fw_file_set_path(file, FileName);
    *NewHandle = &file->proto;
    return EFI_SUCCESS;
}

static EFI_STATUS udf_file_open(EFI_FILE_PROTOCOL *This,
                                EFI_FILE_HANDLE *NewHandle,
                                CHAR16 *FileName, UINT64 OpenMode,
                                UINT64 Attributes)
{
    FW_FILE *base = fw_file_from_proto(This);
    FW_UDF_ENTRY entry;
    FW_FILE *file;
    UINTN i;
    EFI_STATUS st;
    (void)Attributes;

    if (base == NULL || NewHandle == NULL || FileName == NULL ||
        OpenMode != EFI_FILE_MODE_READ) {
        return EFI_INVALID_PARAMETER;
    }

    st = fw_udf_lookup(base, FileName, &entry);
    if (st != EFI_SUCCESS) {
        return st;
    }

    file = fw_file_alloc();
    if (file == NULL) {
        return EFI_OUT_OF_RESOURCES;
    }
    file->is_dir = ((entry.file_characteristics & UDF_FID_CHAR_DIRECTORY) != 0 ||
                    entry.file_type == UDF_FILE_TYPE_DIRECTORY);
    file->fs_kind = FW_FS_UDF;
    file->extent = entry.icb;
    file->partition_reference = entry.partition_reference;
    file->size = entry.size;
    file->position = 0;
    for (i = 0; i < 63 && entry.name[i] != 0; i++) {
        file->name[i] = entry.name[i];
    }
    file->name[i] = 0;
    fw_file_set_path(file, FileName);
    *NewHandle = &file->proto;
    return EFI_SUCCESS;
}

static EFI_STATUS fs_file_open(EFI_FILE_PROTOCOL *This,
                               EFI_FILE_HANDLE *NewHandle,
                               CHAR16 *FileName, UINT64 OpenMode,
                               UINT64 Attributes)
{
    FW_FILE *file = fw_file_from_proto(This);
    CHAR16 normalized[256];
    EFI_STATUS st;

    if (file == NULL || NewHandle == NULL || FileName == NULL) {
        return EFI_INVALID_PARAMETER;
    }
    *NewHandle = NULL;
    st = fs_file_validate_open(OpenMode, Attributes);
    if (st != EFI_SUCCESS) {
        return st;
    }
    st = fs_file_normalize_path(file, FileName, normalized,
                                FW_ARRAY_SIZE(normalized));
    if (st != EFI_SUCCESS) {
        return st;
    }
    if (file->fs_kind == FW_FS_UDF) {
        st = udf_file_open(This, NewHandle, normalized,
                           OpenMode, Attributes);
    } else if (file->fs_kind == FW_FS_ISO) {
        st = iso_file_open(This, NewHandle, normalized,
                           OpenMode, Attributes);
    } else {
        st = fat_file_open(This, NewHandle, normalized,
                           OpenMode, Attributes);
    }
    return st;
}

static BOOLEAN fat_dir_entry_visible(const FAT_DIR_ENTRY *entry)
{
    if (entry->name[0] == 0x00 || entry->name[0] == 0xe5) {
        return 0;
    }
    if (entry->attr == 0x0f || (entry->attr & 0x08) != 0) {
        return 0;
    }
    return 1;
}

static UINTN fat_short_name_to_char16(const FAT_DIR_ENTRY *entry,
                                      CHAR16 *name, UINTN name_cap)
{
    UINTN out = 0;
    INTN end;

    if (name_cap == 0) {
        return 0;
    }

    end = 7;
    while (end >= 0 && entry->name[end] == ' ') {
        end--;
    }
    for (INTN i = 0; i <= end && out + 1 < name_cap; i++) {
        name[out++] = entry->name[i];
    }

    end = 10;
    while (end >= 8 && entry->name[end] == ' ') {
        end--;
    }
    if (end >= 8 && out + 1 < name_cap) {
        name[out++] = '.';
        for (INTN i = 8; i <= end && out + 1 < name_cap; i++) {
            name[out++] = entry->name[i];
        }
    }

    name[out] = 0;
    return out;
}

static BOOLEAN fat_dir_read_raw_at(FW_FILE *file, UINT32 pos,
                                   FAT_DIR_ENTRY *entry)
{
    FW_FAT_VOLUME *volume = file != NULL ? file->fat_volume : NULL;
    UINT8 sec[512];
    UINT32 lba;
    UINT32 off = pos & 511U;

    if (volume == NULL || !volume->valid) {
        return 0;
    }
    if (file->is_root && !volume->is_fat32) {
        if (pos >= (UINT32)volume->root_entries * sizeof(FAT_DIR_ENTRY)) {
            return 0;
        }
        lba = volume->root_dir_start + (pos >> 9);
    } else {
        UINT32 cluster;
        UINT32 cluster_pos;

        cluster = fat_file_cluster_at(file, pos, &cluster_pos, NULL);
        if (!fw_fat_is_data_cluster(volume, cluster)) {
            return 0;
        }

        lba = volume->data_start +
              (cluster - 2U) * volume->sec_per_cluster +
              (cluster_pos >> 9);
        off = cluster_pos & 511U;
    }

    if (!fw_fat_read_512(volume, sec, lba)) {
        return 0;
    }
    fw_copy_mem(entry, sec + off, sizeof(*entry));
    return 1;
}

static BOOLEAN fat_dir_next_visible(FW_FILE *file, FAT_DIR_ENTRY *entry)
{
    FAT_DIR_ENTRY cur;

    if (file->position > 0xffffffffU) {
        return 0;
    }
    while (fat_dir_read_raw_at(file, (UINT32)file->position, &cur)) {
        file->position += sizeof(FAT_DIR_ENTRY);
        if (cur.name[0] == 0x00) {
            return 0;
        }
        if (!fat_dir_entry_visible(&cur)) {
            continue;
        }
        fw_copy_mem(entry, &cur, sizeof(*entry));
        return 1;
    }
    return 0;
}

static EFI_STATUS fat_dir_read(FW_FILE *file, UINTN *BufferSize, VOID *Buffer)
{
    FAT_DIR_ENTRY entry;
    FW_EFI_FILE_INFO *info = (FW_EFI_FILE_INFO *)Buffer;
    CHAR16 name[64];
    UINTN name_len;
    UINTN need;
    UINT64 old_position;

    if (BufferSize == NULL) {
        return EFI_INVALID_PARAMETER;
    }

    old_position = file->position;
    if (!fat_dir_next_visible(file, &entry)) {
        *BufferSize = 0;
        return EFI_SUCCESS;
    }

    name_len = fat_short_name_to_char16(&entry, name,
                                        sizeof(name) / sizeof(name[0]));
    need = __builtin_offsetof(FW_EFI_FILE_INFO, FileName) +
           (name_len + 1U) * sizeof(CHAR16);
    if (Buffer == NULL || *BufferSize < need) {
        file->position = old_position;
        *BufferSize = need;
        return EFI_BUFFER_TOO_SMALL;
    }

    fw_set_mem(Buffer, need, 0);
    info->Size = need;
    info->FileSize = entry.size;
    info->PhysicalSize = entry.size;
    info->Attribute = ((entry.attr & 0x10) ? EFI_FILE_DIRECTORY :
                       (EFI_FILE_ARCHIVE | EFI_FILE_READ_ONLY));
    for (UINTN i = 0; i <= name_len; i++) {
        info->FileName[i] = name[i];
    }
    *BufferSize = need;
    return EFI_SUCCESS;
}

static EFI_STATUS fat_file_read(EFI_FILE_PROTOCOL *This, UINTN *BufferSize,
                                VOID *Buffer)
{
    FW_FILE *file = fw_file_from_proto(This);
    FW_FAT_VOLUME *volume = file != NULL ? file->fat_volume : NULL;
    UINT8 *dst = (UINT8 *)Buffer;
    UINT32 want;
    UINT32 done = 0;
    UINT32 pos;
    UINT32 cluster;
    UINT32 cluster_index;

    if (BufferSize == NULL || file == NULL || volume == NULL ||
        !volume->valid ||
        (!file->is_dir && Buffer == NULL && *BufferSize != 0)) {
        return EFI_INVALID_PARAMETER;
    }
    if (file->is_dir) {
        return fat_dir_read(file, BufferSize, Buffer);
    }

    if (file->position >= file->size) {
        *BufferSize = 0;
        return EFI_SUCCESS;
    }
    want = *BufferSize > 0xffffffffU ? 0xffffffffU : (UINT32)*BufferSize;
    if (want > file->size - file->position) {
        want = file->size - file->position;
    }

    cluster = fat_file_cluster_at(file, (UINT32)file->position,
                                  &pos, &cluster_index);

    while (done < want && fw_fat_is_data_cluster(volume, cluster)) {
        UINT32 cluster_off = pos;
        while (cluster_off < volume->cluster_size && done < want) {
            UINT8 sec[512];
            UINT32 sector_in_cluster = cluster_off / 512;
            UINT32 sector_off = cluster_off & 511;
            UINT32 lba = volume->data_start +
                         (cluster - 2U) * volume->sec_per_cluster +
                         sector_in_cluster;
            UINT32 chunk = 512 - sector_off;
            UINT32 sectors;
            UINT32 i;

            if (chunk > want - done) {
                chunk = want - done;
            }
            sectors = 0;
            if (sector_off == 0 && want - done >= 512) {
                sectors = (want - done) / 512;
                if (sectors > volume->sec_per_cluster - sector_in_cluster) {
                    sectors = volume->sec_per_cluster - sector_in_cluster;
                }
            }
            if (sectors > 0) {
                if (!fw_fat_read_512s(volume, dst + done, lba, sectors)) {
                    *BufferSize = done;
                    return EFI_DEVICE_ERROR;
                }
                done += sectors * 512;
                cluster_off += sectors * 512;
                continue;
            }
            if (!fw_fat_read_512(volume, sec, lba)) {
                *BufferSize = done;
                return EFI_DEVICE_ERROR;
            }
            for (i = 0; i < chunk; i++) {
                dst[done + i] = sec[sector_off + i];
            }
            done += chunk;
            cluster_off += chunk;
        }
        pos = 0;
        if (done < want) {
            cluster = fw_fat_next_cluster(volume, cluster);
            cluster_index++;
            if (fw_fat_is_data_cluster(volume, cluster)) {
                file->fat_cursor_valid = 1;
                file->fat_cursor_cluster = cluster;
                file->fat_cursor_index = cluster_index;
            }
        }
    }

    file->position += done;
    *BufferSize = done;
    return EFI_SUCCESS;
}

UINTN fw_char16_bounded_len(const CHAR16 *name, UINTN Maximum)
{
    UINTN len = 0;

    while (len < Maximum && name[len] != 0) {
        len++;
    }
    return len;
}

static UINTN fw_char16_len64(const CHAR16 *name)
{
    return fw_char16_bounded_len(name, 63U);
}

static EFI_STATUS iso_dir_read(FW_FILE *file, UINTN *BufferSize, VOID *Buffer)
{
    FW_ISO_ENTRY entry;
    UINT32 next_pos;
    UINTN name_len;
    UINTN need;
    FW_EFI_FILE_INFO *info = (FW_EFI_FILE_INFO *)Buffer;
    EFI_STATUS st;

    if (file->position > 0xffffffffU) {
        *BufferSize = 0;
        return EFI_SUCCESS;
    }
    next_pos = (UINT32)file->position;
    st = fw_iso_next_dir_entry(file->extent, file->size, &next_pos, &entry);
    if (st == EFI_NOT_FOUND) {
        *BufferSize = 0;
        return EFI_SUCCESS;
    }
    if (st != EFI_SUCCESS) {
        return st;
    }

    name_len = fw_char16_len64(entry.name);
    need = __builtin_offsetof(FW_EFI_FILE_INFO, FileName) +
           (name_len + 1U) * sizeof(CHAR16);
    if (Buffer == NULL || *BufferSize < need) {
        *BufferSize = need;
        return EFI_BUFFER_TOO_SMALL;
    }

    fw_set_mem(Buffer, need, 0);
    info->Size = need;
    info->FileSize = entry.size;
    info->PhysicalSize = entry.size;
    info->Attribute = (entry.flags & 0x02) ? EFI_FILE_DIRECTORY :
                                            (EFI_FILE_ARCHIVE | EFI_FILE_READ_ONLY);
    {
        UINTN i;
        for (i = 0; i <= name_len; i++) {
            info->FileName[i] = entry.name[i];
        }
    }
    file->position = next_pos;
    *BufferSize = need;
    return EFI_SUCCESS;
}

static EFI_STATUS iso_file_read(EFI_FILE_PROTOCOL *This, UINTN *BufferSize,
                                VOID *Buffer)
{
    FW_FILE *file = fw_file_from_proto(This);
    UINT32 read_size;
    EFI_STATUS st;

    if (BufferSize == NULL || file == NULL) {
        return EFI_INVALID_PARAMETER;
    }
    if (file->is_dir) {
        return iso_dir_read(file, BufferSize, Buffer);
    }
    if (Buffer == NULL && *BufferSize != 0) {
        return EFI_INVALID_PARAMETER;
    }
    if (file->position >= file->size) {
        *BufferSize = 0;
        return EFI_SUCCESS;
    }

    read_size = *BufferSize > 0xffffffffU ?
                0xffffffffU : (UINT32)*BufferSize;
    st = fw_iso_read_extent(file->extent, file->size, file->position,
                            Buffer, &read_size);
    if (st == EFI_SUCCESS) {
        file->position += read_size;
    }
    *BufferSize = read_size;
    return st;
}

static EFI_STATUS udf_dir_read(FW_FILE *file, UINTN *BufferSize, VOID *Buffer)
{
    FW_UDF_ENTRY entry;
    UINTN name_len;
    UINTN need;
    FW_EFI_FILE_INFO *info = (FW_EFI_FILE_INFO *)Buffer;
    UINT64 old_position;
    EFI_STATUS st;

    if (BufferSize == NULL || file == NULL) {
        return EFI_INVALID_PARAMETER;
    }

    old_position = file->position;
    st = fw_udf_next_dir_entry(file, &entry);
    if (st == EFI_NOT_FOUND) {
        *BufferSize = 0;
        return EFI_SUCCESS;
    }
    if (st != EFI_SUCCESS) {
        return st;
    }

    name_len = fw_char16_len64(entry.name);
    need = __builtin_offsetof(FW_EFI_FILE_INFO, FileName) +
           (name_len + 1U) * sizeof(CHAR16);
    if (Buffer == NULL || *BufferSize < need) {
        file->position = old_position;
        *BufferSize = need;
        return EFI_BUFFER_TOO_SMALL;
    }

    fw_set_mem(Buffer, need, 0);
    info->Size = need;
    info->FileSize = entry.size;
    info->PhysicalSize = entry.size;
    info->Attribute =
        ((entry.file_characteristics & UDF_FID_CHAR_DIRECTORY) != 0 ||
         entry.file_type == UDF_FILE_TYPE_DIRECTORY) ?
        EFI_FILE_DIRECTORY : (EFI_FILE_ARCHIVE | EFI_FILE_READ_ONLY);
    {
        UINTN i;

        for (i = 0; i <= name_len; i++) {
            info->FileName[i] = entry.name[i];
        }
    }
    *BufferSize = need;
    return EFI_SUCCESS;
}

static EFI_STATUS udf_file_read(EFI_FILE_PROTOCOL *This, UINTN *BufferSize,
                                VOID *Buffer)
{
    FW_FILE *file = fw_file_from_proto(This);
    UINT32 read_size;
    EFI_STATUS st;

    if (BufferSize == NULL) {
        return EFI_INVALID_PARAMETER;
    }
    if (file->is_dir) {
        return udf_dir_read(file, BufferSize, Buffer);
    }
    if (Buffer == NULL && *BufferSize != 0) {
        return EFI_INVALID_PARAMETER;
    }
    if (file->position >= file->size) {
        *BufferSize = 0;
        return EFI_SUCCESS;
    }

    read_size = *BufferSize > 0xffffffffU ?
                0xffffffffU : (UINT32)*BufferSize;
    st = fw_udf_read_file_bytes(file->partition_reference, file->extent,
                                file->position, Buffer, &read_size);
    if (st == EFI_SUCCESS) {
        file->position += read_size;
    }
    *BufferSize = read_size;
    return st;
}

static EFI_STATUS fs_file_read(EFI_FILE_PROTOCOL *This, UINTN *BufferSize,
                               VOID *Buffer)
{
    FW_FILE *file = fw_file_from_proto(This);

    if (file == NULL) {
        return EFI_INVALID_PARAMETER;
    }
    if (file->fs_kind == FW_FS_UDF) {
        return udf_file_read(This, BufferSize, Buffer);
    }
    if (file->fs_kind == FW_FS_ISO) {
        return iso_file_read(This, BufferSize, Buffer);
    }
    return fat_file_read(This, BufferSize, Buffer);
}

static EFI_STATUS fs_file_write(EFI_FILE_PROTOCOL *This, UINTN *BufferSize,
                                VOID *Buffer)
{
    FW_FILE *file = fw_file_from_proto(This);

    if (file == NULL || BufferSize == NULL ||
        (Buffer == NULL && *BufferSize != 0)) {
        return EFI_INVALID_PARAMETER;
    }
    if (file->is_dir) {
        return EFI_UNSUPPORTED;
    }
    return EFI_WRITE_PROTECTED;
}

static EFI_STATUS fs_file_get_position(EFI_FILE_PROTOCOL *This,
                                       UINT64 *Position)
{
    FW_FILE *file = fw_file_from_proto(This);

    if (file == NULL || Position == NULL) {
        return EFI_INVALID_PARAMETER;
    }
    if (file->is_dir) {
        return EFI_UNSUPPORTED;
    }
    *Position = file->position;
    return EFI_SUCCESS;
}

static EFI_STATUS fs_file_set_position(EFI_FILE_PROTOCOL *This,
                                       UINT64 Position)
{
    FW_FILE *file = fw_file_from_proto(This);

    if (file == NULL) {
        return EFI_INVALID_PARAMETER;
    }
    if (file->is_dir && Position != 0) {
        return EFI_UNSUPPORTED;
    }
    file->position = Position == ~0ULL ? file->size : Position;
    return EFI_SUCCESS;
}

static UINT32 fs_file_block_size(const FW_FILE *File)
{
    if (File->fs_kind == FW_FS_FAT && File->fat_volume != NULL &&
        File->fat_volume->cluster_size != 0) {
        return File->fat_volume->cluster_size;
    }
    if (File->fs_kind == FW_FS_UDF &&
        mUdfVolume.logical_block_size != 0) {
        return mUdfVolume.logical_block_size;
    }
    return ATAPI_SECTOR_SIZE;
}

static UINT64 fs_file_physical_size(const FW_FILE *File)
{
    UINT64 block_size = fs_file_block_size(File);

    if (File->size == 0 || File->size > ~0ULL - (block_size - 1U)) {
        return File->size;
    }
    return ((File->size + block_size - 1U) / block_size) * block_size;
}

static const CHAR16 *fs_file_volume_label(const FW_FILE *File)
{
    static const CHAR16 empty[] = { 0 };

    if (File->fs_kind == FW_FS_FAT && File->fat_volume != NULL) {
        return File->fat_volume->label;
    }
    if (File->fs_kind == FW_FS_UDF) {
        return mUdfVolume.label;
    }
    if (File->fs_kind == FW_FS_ISO) {
        return mIsoVolume.label;
    }
    return empty;
}

static EFI_STATUS fs_file_get_file_info(FW_FILE *File, UINTN *BufferSize,
                                        VOID *Buffer)
{
    FW_EFI_FILE_INFO *info = (FW_EFI_FILE_INFO *)Buffer;
    UINTN name_len = fw_char16_len64(File->name);
    UINTN need = __builtin_offsetof(FW_EFI_FILE_INFO, FileName) +
                 (name_len + 1U) * sizeof(CHAR16);
    UINTN i;

    if (Buffer == NULL || *BufferSize < need) {
        *BufferSize = need;
        return EFI_BUFFER_TOO_SMALL;
    }
    fw_set_mem(Buffer, need, 0);
    info->Size = need;
    info->FileSize = File->size;
    info->PhysicalSize = fs_file_physical_size(File);
    info->Attribute = File->is_dir ? EFI_FILE_DIRECTORY :
                      (EFI_FILE_ARCHIVE | EFI_FILE_READ_ONLY);
    for (i = 0; i <= name_len; i++) {
        info->FileName[i] = File->name[i];
    }
    *BufferSize = need;
    return EFI_SUCCESS;
}

static EFI_STATUS fs_file_get_system_info(FW_FILE *File, UINTN *BufferSize,
                                          VOID *Buffer)
{
    FW_EFI_FILE_SYSTEM_INFO *info = (FW_EFI_FILE_SYSTEM_INFO *)Buffer;
    const CHAR16 *label = fs_file_volume_label(File);
    UINTN label_len = fw_char16_bounded_len(label, 127U);
    const UINTN need =
        __builtin_offsetof(FW_EFI_FILE_SYSTEM_INFO, VolumeLabel) +
        (label_len + 1U) * sizeof(CHAR16);
    UINT64 volume_size = 0;
    UINT32 block_size = fs_file_block_size(File);
    UINTN i;

    if (!File->is_root) {
        return EFI_UNSUPPORTED;
    }
    if (Buffer == NULL || *BufferSize < need) {
        *BufferSize = need;
        return EFI_BUFFER_TOO_SMALL;
    }
    if (File->fs_kind == FW_FS_FAT && File->fat_volume != NULL) {
        volume_size = (UINT64)File->fat_volume->total_sectors * 512U;
    } else if (File->fs_kind == FW_FS_UDF) {
        volume_size = (UINT64)mUdfVolume.partition_length *
                      mUdfVolume.logical_block_size;
    } else {
        volume_size = (UINT64)mCdromBlocks * ATAPI_SECTOR_SIZE;
    }
    fw_set_mem(Buffer, need, 0);
    info->Size = need;
    info->ReadOnly = 1;
    info->VolumeSize = volume_size;
    info->FreeSpace = 0;
    info->BlockSize = block_size;
    for (i = 0; i <= label_len; i++) {
        info->VolumeLabel[i] = label[i];
    }
    *BufferSize = need;
    return EFI_SUCCESS;
}

static EFI_STATUS fs_file_get_volume_label(FW_FILE *File,
                                           UINTN *BufferSize, VOID *Buffer)
{
    FW_EFI_FILE_SYSTEM_VOLUME_LABEL *label =
        (FW_EFI_FILE_SYSTEM_VOLUME_LABEL *)Buffer;
    const CHAR16 *volume_label = fs_file_volume_label(File);
    UINTN label_len = fw_char16_bounded_len(volume_label, 127U);
    const UINTN need = (label_len + 1U) * sizeof(CHAR16);
    UINTN i;

    if (!File->is_root) {
        return EFI_UNSUPPORTED;
    }
    if (Buffer == NULL || *BufferSize < need) {
        *BufferSize = need;
        return EFI_BUFFER_TOO_SMALL;
    }
    for (i = 0; i <= label_len; i++) {
        label->VolumeLabel[i] = volume_label[i];
    }
    *BufferSize = need;
    return EFI_SUCCESS;
}

static EFI_STATUS fs_file_get_info(EFI_FILE_PROTOCOL *This,
                                   void *InformationType,
                                   UINTN *BufferSize, VOID *Buffer)
{
    FW_FILE *file = fw_file_from_proto(This);

    if (file == NULL || InformationType == NULL || BufferSize == NULL) {
        return EFI_INVALID_PARAMETER;
    }
    if (guid_matches(InformationType, mFileInfoGuid)) {
        return fs_file_get_file_info(file, BufferSize, Buffer);
    }
    if (guid_matches(InformationType, mFileSystemInfoGuid)) {
        return fs_file_get_system_info(file, BufferSize, Buffer);
    }
    if (guid_matches(InformationType, mFileSystemVolumeLabelGuid)) {
        return fs_file_get_volume_label(file, BufferSize, Buffer);
    }
    return EFI_UNSUPPORTED;
}

static EFI_STATUS fs_file_set_info(EFI_FILE_PROTOCOL *This,
                                   void *InformationType,
                                   UINTN BufferSize, VOID *Buffer)
{
    FW_FILE *file = fw_file_from_proto(This);
    UINTN minimum;

    if (file == NULL || InformationType == NULL || Buffer == NULL) {
        return EFI_INVALID_PARAMETER;
    }
    if (guid_matches(InformationType, mFileInfoGuid)) {
        minimum = __builtin_offsetof(FW_EFI_FILE_INFO, FileName) +
                  sizeof(CHAR16);
        if (BufferSize < minimum ||
            ((FW_EFI_FILE_INFO *)Buffer)->Size < minimum ||
            ((FW_EFI_FILE_INFO *)Buffer)->Size > BufferSize) {
            return EFI_BAD_BUFFER_SIZE;
        }
    } else if (guid_matches(InformationType, mFileSystemInfoGuid)) {
        if (!file->is_root) {
            return EFI_UNSUPPORTED;
        }
        minimum =
            __builtin_offsetof(FW_EFI_FILE_SYSTEM_INFO, VolumeLabel) +
            sizeof(CHAR16);
        if (BufferSize < minimum ||
            ((FW_EFI_FILE_SYSTEM_INFO *)Buffer)->Size < minimum ||
            ((FW_EFI_FILE_SYSTEM_INFO *)Buffer)->Size > BufferSize) {
            return EFI_BAD_BUFFER_SIZE;
        }
    } else if (guid_matches(InformationType,
                            mFileSystemVolumeLabelGuid)) {
        if (!file->is_root) {
            return EFI_UNSUPPORTED;
        }
        if (BufferSize < sizeof(CHAR16)) {
            return EFI_BAD_BUFFER_SIZE;
        }
    } else {
        return EFI_UNSUPPORTED;
    }
    return EFI_WRITE_PROTECTED;
}

static EFI_STATUS fs_file_flush(EFI_FILE_PROTOCOL *This)
{
    return fw_file_from_proto(This) != NULL ?
           EFI_SUCCESS : EFI_INVALID_PARAMETER;
}

static void fw_file_init_proto(FW_FILE *file)
{
    file->proto.Revision = EFI_FILE_PROTOCOL_REVISION;
    file->proto.Open = fs_file_open;
    file->proto.Close = fs_file_close;
    file->proto.Delete = fs_file_delete;
    file->proto.Read = fs_file_read;
    file->proto.Write = fs_file_write;
    file->proto.GetPosition = fs_file_get_position;
    file->proto.SetPosition = fs_file_set_position;
    file->proto.GetInfo = fs_file_get_info;
    file->proto.SetInfo = fs_file_set_info;
    file->proto.Flush = fs_file_flush;
}

BOOLEAN file_protocol_contract_selftest(VOID)
{
    static const UINT8 unknown_guid[16] = {
        0x46, 0x49, 0x4c, 0x45, 0x54, 0x45, 0x53, 0x54,
        0x9a, 0x64, 0x55, 0x24, 0x70, 0x33, 0x11, 0x02
    };
    UINT64 storage[32];
    FW_EFI_FILE_INFO *file_info = (FW_EFI_FILE_INFO *)storage;
    FW_EFI_FILE_SYSTEM_INFO *system_info =
        (FW_EFI_FILE_SYSTEM_INFO *)storage;
    FW_FILE *file = fw_file_alloc();
    FW_FILE *directory = NULL;
    EFI_FILE_HANDLE opened = NULL;
    CHAR16 path[] = { 'x', 0 };
    UINT64 position;
    UINTN size;
    BOOLEAN ok = 1;

    if (file == NULL) {
        return 0;
    }
    file->fs_kind = FW_FS_FAT;
    file->size = 123;
    file->position = 7;
    file->name[0] = 'x';
    file->name[1] = 0;

    if (file->proto.GetPosition(&file->proto, &position) != EFI_SUCCESS ||
        position != 7 ||
        file->proto.SetPosition(&file->proto, ~0ULL) != EFI_SUCCESS ||
        file->position != file->size ||
        file->proto.SetPosition(&file->proto, 4096) != EFI_SUCCESS ||
        file->position != 4096 ||
        file->proto.Open(&file->proto, &opened, path,
                         EFI_FILE_MODE_WRITE, 0) != EFI_INVALID_PARAMETER ||
        file->proto.Open(&file->proto, &opened, path,
                         EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE,
                         0) != EFI_WRITE_PROTECTED ||
        file->proto.Open(&file->proto, &opened, path,
                         EFI_FILE_MODE_READ,
                         EFI_FILE_ARCHIVE) != EFI_INVALID_PARAMETER) {
        ok = 0;
        goto out;
    }

    size = 0;
    if (file->proto.GetInfo(&file->proto, (VOID *)mFileInfoGuid,
                            &size, NULL) != EFI_BUFFER_TOO_SMALL ||
        size != __builtin_offsetof(FW_EFI_FILE_INFO, FileName) +
                2U * sizeof(CHAR16) || size > sizeof(storage)) {
        ok = 0;
        goto out;
    }
    size = sizeof(storage);
    if (file->proto.GetInfo(&file->proto, (VOID *)mFileInfoGuid,
                            &size, storage) != EFI_SUCCESS ||
        file_info->Size != size || file_info->FileSize != 123 ||
        file_info->FileName[0] != 'x' || file_info->FileName[1] != 0 ||
        (file_info->Attribute & EFI_FILE_READ_ONLY) == 0 ||
        file->proto.GetInfo(&file->proto, (VOID *)unknown_guid,
                            &size, storage) != EFI_UNSUPPORTED ||
        file->proto.GetInfo(&file->proto, (VOID *)mFileSystemInfoGuid,
                            &size, storage) != EFI_UNSUPPORTED ||
        file->proto.SetInfo(&file->proto, (VOID *)mFileInfoGuid,
                            0, storage) != EFI_BAD_BUFFER_SIZE ||
        file->proto.SetInfo(&file->proto, (VOID *)mFileInfoGuid,
                            file_info->Size,
                            storage) != EFI_WRITE_PROTECTED ||
        file->proto.SetInfo(&file->proto, (VOID *)unknown_guid,
                            sizeof(storage), storage) != EFI_UNSUPPORTED) {
        ok = 0;
        goto out;
    }

    directory = fw_file_alloc();
    if (directory == NULL) {
        ok = 0;
        goto out;
    }
    directory->is_root = 1;
    directory->is_dir = 1;
    directory->fs_kind = FW_FS_FAT;
    if (directory->proto.GetPosition(&directory->proto, &position) !=
            EFI_UNSUPPORTED ||
        directory->proto.SetPosition(&directory->proto, 1) !=
            EFI_UNSUPPORTED ||
        directory->proto.SetPosition(&directory->proto, 0) != EFI_SUCCESS) {
        ok = 0;
        goto out;
    }
    size = 0;
    if (directory->proto.GetInfo(
            &directory->proto, (VOID *)mFileSystemInfoGuid,
            &size, NULL) != EFI_BUFFER_TOO_SMALL ||
        size > sizeof(storage)) {
        ok = 0;
        goto out;
    }
    size = sizeof(storage);
    if (directory->proto.GetInfo(
            &directory->proto, (VOID *)mFileSystemInfoGuid,
            &size, storage) != EFI_SUCCESS ||
        system_info->Size != size || !system_info->ReadOnly ||
        system_info->FreeSpace != 0 || system_info->BlockSize == 0 ||
        directory->proto.Write(&directory->proto, &size, storage) !=
            EFI_UNSUPPORTED) {
        ok = 0;
    }

out:
    if (directory != NULL && directory->in_use) {
        (void)directory->proto.Close(&directory->proto);
    }
    if (file->in_use) {
        (void)file->proto.Close(&file->proto);
    }
    return ok;
}

static FW_FAT_VOLUME *fw_fat_volume_from_simple_fs(
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *This)
{
    UINTN i;

    if (This == &mSimpleFsProto || This == &mBootFatVolume.simple_fs) {
        return fw_fat_init() && mBootFatVolume.valid ?
               &mBootFatVolume : NULL;
    }
    for (i = 0; i < FW_ARRAY_SIZE(mPartitionFatVolumes); i++) {
        if (mPartitionFatVolumes[i].valid &&
            &mPartitionFatVolumes[i].simple_fs == This) {
            return &mPartitionFatVolumes[i];
        }
    }
    return NULL;
}

static EFI_STATUS fw_fat_install_partition_volume(
    FW_PARTITION_RECORD *Partition)
{
    FW_FAT_VOLUME *volume;
    EFI_HANDLE handle;
    UINTN index;
    EFI_STATUS st;

    if (Partition == NULL || !Partition->in_use ||
        Partition->parent_block_io == NULL) {
        return EFI_INVALID_PARAMETER;
    }
    index = (UINTN)(Partition - mPartitions);
    if (index >= FW_ARRAY_SIZE(mPartitionFatVolumes)) {
        return EFI_INVALID_PARAMETER;
    }
    volume = &mPartitionFatVolumes[index];
    if (!fw_fat_volume_init(volume, Partition->handle,
                            &Partition->block_io, 0)) {
        return EFI_NOT_FOUND;
    }
    handle = Partition->handle;
    st = bs_install_protocol(&handle,
                             (void *)mSimpleFileSystemProtocolGuid, 0,
                             &volume->simple_fs);
    if (st != EFI_SUCCESS) {
        fw_set_mem(volume, sizeof(*volume), 0);
        return st;
    }
    volume->installed = 1;
    Partition->fat_volume = volume;
    if (mDefaultFatVolume == NULL ||
        fw_guid_equal(Partition->partition_type_guid,
                      mEfiSystemPartitionGuid)) {
        mDefaultFatVolume = volume;
    }
    return EFI_SUCCESS;
}

static EFI_STATUS fw_fat_uninstall_partition_volume(
    FW_PARTITION_RECORD *Partition)
{
    FW_FAT_VOLUME *volume;
    EFI_STATUS st;
    UINTN i;

    if (Partition == NULL || Partition->fat_volume == NULL) {
        return EFI_SUCCESS;
    }
    volume = (FW_FAT_VOLUME *)Partition->fat_volume;
    for (i = 0; i < FW_ARRAY_SIZE(mFileHandles); i++) {
        if (mFileHandles[i].in_use &&
            mFileHandles[i].fs_kind == FW_FS_FAT &&
            mFileHandles[i].fat_volume == volume) {
            return EFI_ACCESS_DENIED;
        }
    }
    if (volume->installed) {
        st = bs_uninstall_protocol(Partition->handle,
                                   (void *)mSimpleFileSystemProtocolGuid,
                                   &volume->simple_fs);
        if (st != EFI_SUCCESS) {
            return st;
        }
        volume->installed = 0;
    }
    return EFI_SUCCESS;
}

static EFI_STATUS fw_fat_reinstall_partition_volume(
    FW_PARTITION_RECORD *Partition)
{
    FW_FAT_VOLUME *volume;
    EFI_HANDLE handle;
    EFI_STATUS st;

    if (Partition == NULL || Partition->fat_volume == NULL) {
        return EFI_SUCCESS;
    }
    volume = (FW_FAT_VOLUME *)Partition->fat_volume;
    if (volume->installed) {
        return EFI_SUCCESS;
    }
    handle = Partition->handle;
    st = bs_install_protocol(&handle,
                             (void *)mSimpleFileSystemProtocolGuid, 0,
                             &volume->simple_fs);
    if (st == EFI_SUCCESS) {
        volume->installed = 1;
    }
    return st;
}

static VOID fw_fat_release_partition_volume(FW_PARTITION_RECORD *Partition)
{
    FW_FAT_VOLUME *volume;

    if (Partition == NULL || Partition->fat_volume == NULL) {
        return;
    }
    volume = (FW_FAT_VOLUME *)Partition->fat_volume;
    if (mDefaultFatVolume == volume) {
        mDefaultFatVolume = NULL;
    }
    fw_set_mem(volume, sizeof(*volume), 0);
    Partition->fat_volume = NULL;
}

EFI_STATUS fat_open_volume(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *This,
                                  EFI_FILE_HANDLE *Root)
{
    FW_FAT_VOLUME *volume;
    FW_FILE *root;

    if (Root == NULL) {
        return EFI_INVALID_PARAMETER;
    }
    volume = fw_fat_volume_from_simple_fs(This);
    if (volume == NULL || !volume->valid) {
        return EFI_NOT_FOUND;
    }
    root = fw_file_alloc();
    if (root == NULL) {
        return EFI_OUT_OF_RESOURCES;
    }
    root->is_root = 1;
    root->is_dir = 1;
    root->fs_kind = FW_FS_FAT;
    root->fat_volume = volume;
    root->first_cluster = volume->is_fat32 ? volume->root_cluster : 0;
    root->name[0] = '\\';
    root->name[1] = 0;
    root->path[0] = '\\';
    root->path[1] = 0;
    *Root = &root->proto;
    return EFI_SUCCESS;
}

EFI_STATUS optical_open_volume(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *This,
                                      EFI_FILE_HANDLE *Root)
{
    FW_FILE *file;

    (void)This;
    if (Root == NULL) {
        return EFI_INVALID_PARAMETER;
    }
    *Root = NULL;
    file = fw_file_alloc();
    if (file == NULL) {
        return EFI_OUT_OF_RESOURCES;
    }
    if (fw_udf_init()) {
        FW_UDF_ENTRY root;

        fw_set_mem(&root, sizeof(root), 0);
        root.icb = mUdfVolume.root_icb;
        root.partition_reference = mUdfVolume.root_partition_reference;
        if (!fw_udf_entry_load_meta(&root)) {
            file->in_use = 0;
            return EFI_VOLUME_CORRUPTED;
        }
        file->is_root = 1;
        file->is_dir = 1;
        file->fs_kind = FW_FS_UDF;
        file->extent = root.icb;
        file->partition_reference = root.partition_reference;
        file->size = root.size;
        file->name[0] = '\\';
        file->name[1] = 0;
        file->path[0] = '\\';
        file->path[1] = 0;
        *Root = &file->proto;
        return EFI_SUCCESS;
    }

    if (!fw_iso_init()) {
        file->in_use = 0;
        return EFI_NOT_FOUND;
    }
    file->is_root = 1;
    file->is_dir = 1;
    file->fs_kind = FW_FS_ISO;
    file->extent = mIsoVolume.root_extent;
    file->size = mIsoVolume.root_size;
    file->name[0] = '\\';
    file->name[1] = 0;
    file->path[0] = '\\';
    file->path[1] = 0;
    *Root = &file->proto;
    return EFI_SUCCESS;
}

static EFI_STATUS fw_extract_file_path_node(void *DevicePath,
                                            CHAR16 *Path,
                                            UINTN PathChars)
{
    FW_DEVICE_PATH_NODE *node;
    CHAR16 *src;
    UINTN chars;
    UINTN i;

    if (DevicePath == NULL || Path == NULL || PathChars == 0) {
        return EFI_INVALID_PARAMETER;
    }

    node = (FW_DEVICE_PATH_NODE *)fw_loaded_image_file_path(DevicePath);
    if (node == NULL || node->Type != 0x04 || node->SubType != 0x04 ||
        node->Length <= sizeof(FW_DEVICE_PATH_NODE)) {
        return EFI_NOT_FOUND;
    }

    src = (CHAR16 *)((UINT8 *)node + sizeof(FW_DEVICE_PATH_NODE));
    chars = (node->Length - sizeof(FW_DEVICE_PATH_NODE)) / sizeof(CHAR16);
    if (chars == 0) {
        return EFI_NOT_FOUND;
    }

    for (i = 0; i + 1 < PathChars && i < chars; i++) {
        Path[i] = src[i];
        if (src[i] == 0) {
            return i == 0 ? EFI_NOT_FOUND : EFI_SUCCESS;
        }
    }
    Path[i] = 0;
    return i == 0 ? EFI_NOT_FOUND : EFI_SUCCESS;
}

EFI_STATUS fw_load_image_source_from_simple_fs(
    void *DevicePath, VOID **SourceBuffer, UINTN *SourceSize,
    BOOLEAN *FileSystemFound)
{
    static const UINT8 file_info_guid[16] = {
        0x92, 0x6e, 0x57, 0x09, 0x3f, 0x6d, 0xd2, 0x11,
        0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b,
    };
    CHAR16 path[128];
    UINT8 info_buffer[512];
    FW_EFI_FILE_INFO *info = (FW_EFI_FILE_INFO *)info_buffer;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *simple_fs = NULL;
    EFI_FILE_HANDLE root = NULL;
    EFI_FILE_HANDLE file = NULL;
    EFI_HANDLE device = NULL;
    VOID *remaining = DevicePath;
    VOID *buffer;
    UINTN info_size = sizeof(info_buffer);
    UINTN read_size;
    EFI_STATUS st;

    if (DevicePath == NULL || SourceBuffer == NULL || SourceSize == NULL ||
        FileSystemFound == NULL) {
        return EFI_INVALID_PARAMETER;
    }
    *SourceBuffer = NULL;
    *SourceSize = 0;
    *FileSystemFound = 0;
    st = bs_locate_device_path((void *)mSimpleFileSystemProtocolGuid,
                               &remaining, &device);
    if (st != EFI_SUCCESS) {
        return st;
    }
    *FileSystemFound = 1;
    st = bs_handle_protocol(device, (void *)mSimpleFileSystemProtocolGuid,
                            (VOID **)&simple_fs);
    if (st != EFI_SUCCESS || simple_fs == NULL ||
        simple_fs->OpenVolume == NULL) {
        return st == EFI_SUCCESS ? EFI_UNSUPPORTED : st;
    }
    st = fw_extract_file_path_node(remaining, path,
                                   sizeof(path) / sizeof(path[0]));
    if (st != EFI_SUCCESS) {
        return st;
    }
    st = simple_fs->OpenVolume(simple_fs, &root);
    if (st != EFI_SUCCESS || root == NULL) {
        return st == EFI_SUCCESS ? EFI_VOLUME_CORRUPTED : st;
    }
    st = root->Open(root, &file, path, EFI_FILE_MODE_READ, 0);
    if (st != EFI_SUCCESS || file == NULL) {
        (void)root->Close(root);
        return st == EFI_SUCCESS ? EFI_NOT_FOUND : st;
    }
    st = file->GetInfo(file, (void *)file_info_guid, &info_size, info_buffer);
    if (st != EFI_SUCCESS || info_size > sizeof(info_buffer) ||
        info->FileSize == 0 ||
        (info->Attribute & EFI_FILE_DIRECTORY) != 0 ||
        info->FileSize > (UINT64)(UINTN)-1) {
        (void)file->Close(file);
        (void)root->Close(root);
        return st == EFI_SUCCESS ? EFI_LOAD_ERROR : st;
    }
    st = bs_allocate_pool(EfiBootServicesData, (UINTN)info->FileSize, &buffer);
    if (st != EFI_SUCCESS) {
        (void)file->Close(file);
        (void)root->Close(root);
        return st;
    }
    read_size = (UINTN)info->FileSize;
    st = file->Read(file, &read_size, buffer);
    (void)file->Close(file);
    (void)root->Close(root);
    if (st != EFI_SUCCESS || read_size != (UINTN)info->FileSize) {
        (void)bs_free_pool(buffer);
        return st == EFI_SUCCESS ? EFI_DEVICE_ERROR : st;
    }

    *SourceBuffer = buffer;
    *SourceSize = read_size;
    return EFI_SUCCESS;
}

EFI_STATUS fw_load_image_source_from_load_file(
    const UINT8 *ProtocolGuid, BOOLEAN BootPolicy, void *DevicePath,
    VOID **SourceBuffer, UINTN *SourceSize, BOOLEAN *ProtocolFound)
{
    EFI_LOAD_FILE_PROTOCOL *load_file = NULL;
    EFI_HANDLE device = NULL;
    VOID *remaining = DevicePath;
    VOID *buffer = NULL;
    UINTN size = 0;
    UINTN read_size;
    EFI_STATUS st;

    if (ProtocolGuid == NULL || DevicePath == NULL || SourceBuffer == NULL ||
        SourceSize == NULL || ProtocolFound == NULL) {
        return EFI_INVALID_PARAMETER;
    }
    *SourceBuffer = NULL;
    *SourceSize = 0;
    *ProtocolFound = 0;
    st = bs_locate_device_path((void *)ProtocolGuid, &remaining, &device);
    if (st != EFI_SUCCESS) {
        return st;
    }
    *ProtocolFound = 1;
    st = bs_handle_protocol(device, (void *)ProtocolGuid,
                            (VOID **)&load_file);
    if (st != EFI_SUCCESS || load_file == NULL ||
        load_file->LoadFile == NULL) {
        return st == EFI_SUCCESS ? EFI_UNSUPPORTED : st;
    }

    st = load_file->LoadFile(load_file, remaining, BootPolicy, &size, NULL);
    if (st == EFI_SUCCESS) {
        return size == 0 ? EFI_LOAD_ERROR : EFI_DEVICE_ERROR;
    }
    if (st != EFI_BUFFER_TOO_SMALL) {
        return st;
    }
    if (size == 0) {
        return EFI_LOAD_ERROR;
    }
    st = bs_allocate_pool(EfiBootServicesData, size, &buffer);
    if (st != EFI_SUCCESS) {
        return st;
    }
    read_size = size;
    st = load_file->LoadFile(load_file, remaining, BootPolicy,
                             &read_size, buffer);
    if (st != EFI_SUCCESS || read_size == 0 || read_size > size) {
        (void)bs_free_pool(buffer);
        return st == EFI_SUCCESS ? EFI_DEVICE_ERROR : st;
    }
    *SourceBuffer = buffer;
    *SourceSize = read_size;
    return EFI_SUCCESS;
}

