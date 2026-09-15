/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Filesystem-layer types shared by firmware.c (FAT/ISO + the common
 * EFI_FILE_PROTOCOL layer) and udf.c (UDF 2.01), plus the UDF API.
 */

#ifndef IA64_FIRMWARE_FW_FS_H
#define IA64_FIRMWARE_FW_FS_H

#include "fw-base.h"
#include "fw-efi-types.h"
#include "fw-boot-shell.h"
#include "fw-uart.h"
#include "fw-device-path.h"
#include "fw-pe.h"

typedef struct _EFI_DISK_IO_PROTOCOL EFI_DISK_IO_PROTOCOL;
typedef EFI_STATUS (*EFI_DISK_READ)(EFI_DISK_IO_PROTOCOL *This,
                                    UINT32 MediaId, UINT64 Offset,
                                    UINTN BufferSize, VOID *Buffer);
typedef EFI_STATUS (*EFI_DISK_WRITE)(EFI_DISK_IO_PROTOCOL *This,
                                     UINT32 MediaId, UINT64 Offset,
                                     UINTN BufferSize, VOID *Buffer);
struct _EFI_DISK_IO_PROTOCOL {
    UINT64         Revision;
    EFI_DISK_READ  ReadDisk;
    EFI_DISK_WRITE WriteDisk;
};

typedef struct FW_FAT_VOLUME {
    BOOLEAN valid;
    BOOLEAN installed;
    UINT8 fat_type;
    BOOLEAN is_fat16;
    BOOLEAN is_fat32;
    UINT8   sec_per_cluster;
    UINT16  reserved_secs;
    UINT8   num_fats;
    UINT16  root_entries;
    UINT32  secs_per_fat;
    UINT32  eoc_cluster;
    UINT32  root_cluster;
    UINT32  root_dir_start;
    UINT32  root_dir_sectors;
    UINT32  data_start;
    UINT32  cluster_size;
    UINT32  total_sectors;
    UINT32  cluster_count;
    UINT32  lba_offset;
    BOOLEAN fat_cache_valid;
    UINT32  fat_cache_media_id;
    UINT32  fat_cache_lba;
    UINT8   fat_cache[512];
    EFI_HANDLE handle;
    EFI_BLOCK_IO_PROTOCOL *block_io;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL simple_fs;
    CHAR16 label[12];
} FW_FAT_VOLUME;

typedef enum {
    FW_FS_FAT = 0,
    FW_FS_ISO = 1,
    FW_FS_UDF = 2,
} FW_FS_KIND;

typedef struct {
    BOOLEAN valid;
    UINT32  root_extent;
    UINT32  root_size;
    CHAR16  label[33];
} FW_ISO_VOLUME;

typedef struct {
    UINT32 extent;
    UINT32 size;
    UINT8  flags;
    CHAR16 name[64];
} FW_ISO_ENTRY;

typedef struct {
    BOOLEAN valid;
    BOOLEAN checked;
    UINT32  partition_start;
    UINT32  partition_length;
    UINT16  partition_number;
    UINT16  partition_reference;
    UINT32  logical_block_size;
    UINT32  root_icb;
    UINT16  root_partition_reference;
    CHAR16  label[128];
} FW_UDF_VOLUME;

typedef struct {
    UINT32 icb;
    UINT16 partition_reference;
    UINT8  file_characteristics;
    UINT8  file_type;
    UINT16 icb_flags;
    UINT64 size;
    CHAR16 name[64];
} FW_UDF_ENTRY;

typedef struct {
    UINT32 icb;
    UINT16 partition_reference;
    UINT8  file_type;
    UINT16 icb_flags;
    UINT64 information_length;
    UINT32 allocation_offset;
    UINT32 allocation_length;
} FW_UDF_FILE_META;

typedef struct {
    EFI_FILE_PROTOCOL proto;
    BOOLEAN in_use;
    BOOLEAN is_root;
    BOOLEAN is_dir;
    FW_FS_KIND fs_kind;
    FW_FAT_VOLUME *fat_volume;
    UINT32  first_cluster;
    BOOLEAN fat_cursor_valid;
    UINT32  fat_cursor_cluster;
    UINT32  fat_cursor_index;
    UINT32  extent;
    UINT16  partition_reference;
    UINT64  size;
    UINT64  position;
    CHAR16  name[64];
    CHAR16  path[256];
} FW_FILE;

#define FW_FILE_MAX 16

#define UDF_FILE_TYPE_DIRECTORY                  4U
#define UDF_FID_CHAR_DIRECTORY                   0x02U

extern FW_UDF_VOLUME mUdfVolume;
extern UINT32 mCdromBlocks;

/* firmware.c services the UDF module consumes. */
UINT16 fw_le16(const UINT8 *p);
UINT32 fw_le32(const UINT8 *p);
UINT64 fw_le64(const UINT8 *p);
BOOLEAN fw_bytes_eq(const UINT8 *p, const char *s, UINTN len);
UINT8 fw_ascii_upper(UINT8 c);
BOOLEAN fw_iso_read_sectors(UINT8 *buf, UINT32 lba, UINT32 count);
BOOLEAN atapi_configure_el_torito(void);

BOOLEAN fw_udf_init(void);
BOOLEAN fw_udf_entry_load_meta(FW_UDF_ENTRY *Entry);
EFI_STATUS fw_udf_lookup(FW_FILE *Base, CHAR16 *Path, FW_UDF_ENTRY *Entry);
EFI_STATUS fw_udf_next_dir_entry(FW_FILE *Dir, FW_UDF_ENTRY *Entry);
EFI_STATUS fw_udf_read_file_bytes(UINT16 PartitionReference,
                                  UINT32 Icb, UINT64 Offset,
                                  VOID *Buffer, UINT32 *ReadSize);

#define FW_PARTITION_DEVICE_PATH_MAX 96U
typedef struct {
    UINT8  name[11];
    UINT8  attr;
    UINT8  reserved;
    UINT8  crt_time_tenth;
    UINT16 crt_time;
    UINT16 crt_date;
    UINT16 acc_date;
    UINT16 cluster_hi;
    UINT16 mod_time;
    UINT16 mod_date;
    UINT16 cluster_lo;
    UINT32 size;
} __attribute__((packed)) FAT_DIR_ENTRY;
typedef struct {
    FW_DEVICE_PATH_NODE Header;
    UINT32 BootEntry;
    UINT64 PartitionStart;
    UINT64 PartitionSize;
} __attribute__((packed)) FW_CDROM_DEVICE_PATH_NODE;
typedef struct {
    FW_DEVICE_PATH_NODE Header;
    UINT8 PrimarySecondary;
    UINT8 SlaveMaster;
    UINT16 Lun;
} __attribute__((packed)) FW_ATAPI_DEVICE_PATH_NODE;
typedef struct {
    FW_DEVICE_PATH_NODE Header;
    UINT16 HbaPort;
    UINT16 PortMultiplierPort;
    UINT16 Lun;
} __attribute__((packed)) FW_SATA_DEVICE_PATH_NODE;

typedef struct {
    FW_ACPI_HID_DEVICE_PATH_NODE Acpi;
    FW_PCI_DEVICE_PATH_NODE Pci;
    FW_ATAPI_DEVICE_PATH_NODE Atapi;
    FW_CDROM_DEVICE_PATH_NODE Cdrom;
    FW_DEVICE_PATH_NODE End;
} __attribute__((packed)) FW_BLOCK_DEVICE_PATH;
typedef struct {
    FW_ACPI_HID_DEVICE_PATH_NODE Acpi;
    FW_PCI_DEVICE_PATH_NODE Pci;
    FW_ATAPI_DEVICE_PATH_NODE Atapi;
    FW_CDROM_DEVICE_PATH_NODE Cdrom;
    FW_DEVICE_PATH_NODE FileHeader;
    CHAR16 PathName[23];
    FW_DEVICE_PATH_NODE End;
} __attribute__((packed)) FW_BOOT_FULL_DEVICE_PATH;
typedef struct {
    FW_ACPI_HID_DEVICE_PATH_NODE Acpi;
    FW_PCI_DEVICE_PATH_NODE Pci;
    FW_DEVICE_PATH_NODE End;
} __attribute__((packed)) FW_GRAPHICS_DEVICE_PATH;
typedef struct {
    FW_ACPI_HID_DEVICE_PATH_NODE Acpi;
    FW_PCI_DEVICE_PATH_NODE Pci;
    FW_ATAPI_DEVICE_PATH_NODE Atapi;
    FW_CDROM_DEVICE_PATH_NODE Cdrom;
    FW_DEVICE_PATH_NODE FileHeader;
    CHAR16 PathName[23];
    FW_DEVICE_PATH_NODE End;
} __attribute__((packed)) FW_OPTICAL_SETUP_LOADER_DEVICE_PATH;
typedef struct {
    FW_ACPI_HID_DEVICE_PATH_NODE Acpi;
    FW_PCI_DEVICE_PATH_NODE Pci;
    FW_DEVICE_PATH_NODE End;
} __attribute__((packed)) FW_PCI_CONTROLLER_DEVICE_PATH;
typedef struct {
    FW_ACPI_HID_DEVICE_PATH_NODE Acpi;
    FW_DEVICE_PATH_NODE End;
} __attribute__((packed)) FW_PCI_ROOT_BRIDGE_DEVICE_PATH;
typedef struct {
    FW_ACPI_HID_DEVICE_PATH_NODE Acpi;
    FW_PCI_DEVICE_PATH_NODE Pci;
    FW_ATAPI_DEVICE_PATH_NODE Atapi;
    FW_DEVICE_PATH_NODE End;
} __attribute__((packed)) FW_RAW_BLOCK_DEVICE_PATH;
typedef struct {
    FW_ACPI_HID_DEVICE_PATH_NODE Acpi;
    FW_PCI_DEVICE_PATH_NODE Pci;
    FW_SATA_DEVICE_PATH_NODE Sata;
    FW_CDROM_DEVICE_PATH_NODE Cdrom;
    FW_DEVICE_PATH_NODE End;
} __attribute__((packed)) FW_SATA_BLOCK_DEVICE_PATH;
typedef struct {
    FW_ACPI_HID_DEVICE_PATH_NODE Acpi;
    FW_PCI_DEVICE_PATH_NODE Pci;
    FW_SATA_DEVICE_PATH_NODE Sata;
    FW_DEVICE_PATH_NODE End;
} __attribute__((packed)) FW_SATA_RAW_BLOCK_DEVICE_PATH;
typedef struct {
    UINT32 Attributes;
    UINT16 FilePathListLength;
    CHAR16 Description[21];
    FW_OPTICAL_SETUP_LOADER_DEVICE_PATH FilePath;
} __attribute__((packed)) FW_EFI_BOOT_OPTION;
/* A HW_VENDOR device-path node (Type 0x01, SubType 0x04) carrying a 16-byte
 * GUID.  The sample fires up its built-in shell from a Boot#### whose file
 * path is exactly such a node; we mirror that for the internal EFI shell. */
typedef struct {
    FW_DEVICE_PATH_NODE Header;
    UINT8 Guid[16];
} __attribute__((packed)) FW_VENDOR_DEVICE_PATH_NODE;
typedef struct {
    UINT32 Attributes;
    UINT16 FilePathListLength;
    CHAR16 Description[21];
    FW_VENDOR_DEVICE_PATH_NODE Vendor;
    FW_DEVICE_PATH_NODE End;
} __attribute__((packed)) FW_SHELL_BOOT_OPTION;
typedef struct {
    FW_GRAPHICS_DEVICE_PATH Graphics;
    FW_SERIAL_DEVICE_PATH Serial;
} __attribute__((packed)) FW_CONSOLE_OUTPUT_DEVICE_PATH;

#define FW_PARTITION_MAX 128U
#define EFI_DISK_IO_PROTOCOL_REVISION   0x00010000
#define EFI_BLOCK_IO_PROTOCOL_REVISION  0x00010000
typedef struct {
    BOOLEAN in_use;
    BOOLEAN protocols_installed;
    EFI_HANDLE handle;
    EFI_HANDLE parent_handle;
    EFI_BLOCK_IO_PROTOCOL *parent_block_io;
    UINT64 start_lba;
    UINT64 block_count;
    UINT32 partition_number;
    UINT8 partition_type_guid[16];
    UINT8 partition_signature[16];
    UINT8 mbr_type;
    UINT8 signature_type;
    VOID *fat_volume;
    EFI_BLOCK_IO_MEDIA media;
    EFI_BLOCK_IO_PROTOCOL block_io;
    EFI_DISK_IO_PROTOCOL disk_io;
    UINT8 device_path[FW_PARTITION_DEVICE_PATH_MAX] FW_DEVICE_PATH_GUEST_ALIGN;
} FW_PARTITION_RECORD;
BOOLEAN fw_set_loaded_image_load_options(EFI_HANDLE ImageHandle,
                                                VOID *LoadOptions,
                                                UINT32 LoadOptionsSize);

/* filesystem.c: block/disk I/O, partitions, device paths, FAT/ISO, SFS. */
EFI_STATUS bs_locate_device_path(void *Protocol, void **DevicePath,
                                 EFI_HANDLE *Device);
UINTN fw_device_path_size(const VOID *Path);
BOOLEAN fw_iso_init(void);
BOOLEAN efi_handle_is_valid(EFI_HANDLE Handle);
BOOLEAN fw_boot_fat_available(void);
BOOLEAN fw_boot_optical_fs_available(void);
void *fw_loaded_image_file_path(void *DevicePath);
EFI_LOADED_IMAGE_RECORD *fw_loaded_image_record(EFI_HANDLE ImageHandle);
EFI_STATUS fw_loaded_image_source_paths(
    EFI_LOADED_IMAGE_RECORD *Record, void *DevicePath,
    EFI_HANDLE *DeviceHandle, VOID **FilePath);

EFI_STATUS blk_flush(EFI_BLOCK_IO_PROTOCOL *This);
EFI_STATUS blk_read(EFI_BLOCK_IO_PROTOCOL *This, UINT32 MediaId,
                            UINT64 Lba, UINTN BufferSize, VOID *Buffer);
EFI_STATUS blk_reset(EFI_BLOCK_IO_PROTOCOL *This,
                     BOOLEAN ExtendedVerification);
EFI_STATUS blk_write(EFI_BLOCK_IO_PROTOCOL *This, UINT32 MediaId,
                             UINT64 Lba, UINTN BufferSize, VOID *Buffer);
EFI_STATUS disk_read(EFI_DISK_IO_PROTOCOL *This, UINT32 MediaId,
                     UINT64 Offset, UINTN BufferSize, VOID *Buffer);
EFI_STATUS disk_write(EFI_DISK_IO_PROTOCOL *This, UINT32 MediaId,
                      UINT64 Offset, UINTN BufferSize, VOID *Buffer);
EFI_STATUS fat_open_volume(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *This,
                                  EFI_FILE_HANDLE *Root);
EFI_STATUS optical_open_volume(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *This,
                                      EFI_FILE_HANDLE *Root);
EFI_STATUS fw_fat_lookup(CHAR16 *path, FAT_DIR_ENTRY *out);
EFI_STATUS fw_fat_read_file_entry(const FAT_DIR_ENTRY *entry,
                                         VOID *Buffer, UINT32 *ReadSize);
EFI_STATUS fw_load_image_source_from_load_file(
    const UINT8 *ProtocolGuid, BOOLEAN BootPolicy, void *DevicePath,
    VOID **SourceBuffer, UINTN *SourceSize, BOOLEAN *ProtocolFound);
EFI_STATUS fw_load_image_source_from_simple_fs(
    void *DevicePath, VOID **SourceBuffer, UINTN *SourceSize,
    BOOLEAN *FileSystemFound);
EFI_STATUS fw_partition_discover(EFI_HANDLE ParentHandle,
                                        EFI_BLOCK_IO_PROTOCOL *Parent);
void fw_update_storage_device_paths(VOID);
BOOLEAN partition_driver_install(void);
BOOLEAN block_io_read_selftest(void);
BOOLEAN disk_block_io_selftest(void);
BOOLEAN el_torito_partition_selftest(void);
BOOLEAN fat_cursor_cache_selftest(VOID);
BOOLEAN file_protocol_contract_selftest(VOID);
BOOLEAN loaded_image_file_path_selftest(void);
BOOLEAN optical_raw_device_path_selftest(void);
BOOLEAN optical_setup_boot_option_selftest(void);
BOOLEAN partition_component_name_selftest(VOID);
EFI_HANDLE fw_boot_media_file_system(UINTN Index);
extern FW_BLOCK_DEVICE_PATH mBlockDevicePath;
extern EFI_DISK_IO_PROTOCOL  mBlockDiskIoProto;
extern EFI_BLOCK_IO_MEDIA    mBlockIoMedia;
extern EFI_BLOCK_IO_PROTOCOL mBlockIoProto;
extern FW_BOOT_FULL_DEVICE_PATH mBootFullDevicePath;
extern BOOLEAN mBootImageMapped;
extern UINT32 mBootImagePartitionBlocks;
extern UINT64 mBootImagePartitionCdBlocks;
extern UINT32 mBootImageStartLba;
extern FW_CONSOLE_OUTPUT_DEVICE_PATH mConsoleOutputDevicePath;
extern const CHAR16 mDefaultBootDescription[21];
extern FW_FAT_VOLUME *mDefaultFatVolume;
extern FW_RAW_BLOCK_DEVICE_PATH mDiskBlockDevicePath;
extern EFI_BLOCK_IO_MEDIA    mDiskBlockIoMedia;
extern EFI_BLOCK_IO_PROTOCOL mDiskBlockIoProto;
extern EFI_DISK_IO_PROTOCOL  mDiskIoProto;
extern FW_DEVICE_PATH_NODE mEndDevicePath;
extern FW_GRAPHICS_DEVICE_PATH mGraphicsDevicePath;
extern FW_OPTICAL_SETUP_LOADER_DEVICE_PATH mOpticalSetupLoaderDevicePath;
extern EFI_SIMPLE_FILE_SYSTEM_PROTOCOL mOpticalSimpleFsProto;
extern FW_PARTITION_RECORD mPartitions[FW_PARTITION_MAX];
extern FW_PCI_CONTROLLER_DEVICE_PATH mPciAhciDevicePath;
extern FW_PCI_CONTROLLER_DEVICE_PATH mPciIdeDevicePath;
extern FW_PCI_CONTROLLER_DEVICE_PATH mPciLsiDevicePath;
extern FW_PCI_CONTROLLER_DEVICE_PATH mPciOhciDevicePath;
extern FW_PCI_ROOT_BRIDGE_DEVICE_PATH mPciRootBridgeDevicePath;
extern FW_PCI_CONTROLLER_DEVICE_PATH mPciUhciDevicePath;
extern FW_RAW_BLOCK_DEVICE_PATH mRawBlockDevicePath;
extern EFI_BLOCK_IO_MEDIA    mRawBlockIoMedia;
extern EFI_BLOCK_IO_PROTOCOL mRawBlockIoProto;
extern EFI_DISK_IO_PROTOCOL  mRawDiskIoProto;
extern FW_SATA_BLOCK_DEVICE_PATH mSataBlockDevicePath;
extern FW_SATA_RAW_BLOCK_DEVICE_PATH mSataBootDevicePath;
extern FW_SATA_RAW_BLOCK_DEVICE_PATH mSataDiskDevicePath;
extern FW_SATA_RAW_BLOCK_DEVICE_PATH mSataRawDevicePath;
extern EFI_SIMPLE_FILE_SYSTEM_PROTOCOL mSimpleFsProto;

extern BOOLEAN mBootServicesExited;
extern BOOLEAN mSalLoaderHandoffPending;
VOID rs_reset_system(UINTN ResetType, EFI_STATUS ResetStatus,
                     UINTN DataSize, VOID *ResetData);

/* boot.c */
EFI_STATUS boot_image_from_boot_order(void);
EFI_STATUS boot_image_from_disk(void);

#endif /* IA64_FIRMWARE_FW_FS_H */
