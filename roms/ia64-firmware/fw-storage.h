/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Storage types shared by firmware.c (IDE driver, Block I/O, boot path)
 * and storage.c (LSI/AHCI drivers + storage abstraction).
 */

#ifndef IA64_FIRMWARE_FW_STORAGE_H
#define IA64_FIRMWARE_FW_STORAGE_H

#include "fw-base.h"

typedef struct {
    UINT8  unit;         /* 0=master, 1=slave on this channel */
    UINT8  channel;      /* 0=primary channel, 1=secondary channel */
    UINT8  present;      /* 0=no device, 1=device responds */
    UINT8  media_present;
    UINT8  is_atapi;     /* 0=ATA disk, 1=ATAPI CD-ROM */
    UINT64 last_lba;
} IDE_DEVICE;

#define ATA_CMD_IDENTIFY          0xEC
#define ATA_CMD_READ_SECS         0x20
#define ATA_CMD_WRITE_SECS        0x30
#define ATA_CMD_READ_DMA          0xC8
#define ATA_CMD_WRITE_DMA         0xCA
#define ATA_CMD_FLUSH_CACHE       0xE7
#define ATA_CMD_PACKET            0xA0
#define ATA_CMD_IDENTIFY_PACKET   0xA1

#define ATA_SR_BSY   0x80
#define ATA_SR_DRDY  0x40
#define ATA_SR_DRQ   0x08
#define ATA_SR_ERR   0x01
#define ATA_SR_DF    0x20

#define ATAPI_SECTOR_SIZE  2048
#define ATAPI_MAX_TRANSFER_SECTORS 31U

#define SCSI_BOUNCE_SIZE             (64U * 1024U)

struct SCSI_DEVICE_STRUCT {
    UINT8   target;
    UINT8   lun;
    UINT8   present;
    UINT8   media_present;
    UINT8   is_cd;
    UINT8   removable;
    UINT8   read_only;
    UINT32  block_size;
    UINT64  last_lba;
};
typedef struct SCSI_DEVICE_STRUCT SCSI_DEVICE;

struct AHCI_DEVICE_STRUCT {
    UINT8 port;
    UINT8 present;
    UINT8 media_present;
    UINT8 is_atapi;
    UINT8 read_only;
    UINT8 removable;
    UINT8 lba48;
    UINT8 reserved;
    UINT32 block_size;
    UINT64 last_lba;
};
typedef struct AHCI_DEVICE_STRUCT AHCI_DEVICE;


typedef enum {
    FW_STORAGE_NONE = 0,
    FW_STORAGE_IDE,
    FW_STORAGE_AHCI,
    FW_STORAGE_SCSI,
} FW_STORAGE_KIND;

typedef struct {
    FW_STORAGE_KIND Kind;
    IDE_DEVICE *Ide;
    AHCI_DEVICE *Ahci;
    SCSI_DEVICE *Scsi;
} FW_STORAGE_DEVICE;

extern FW_STORAGE_DEVICE mBootStorageDevice;
extern FW_STORAGE_DEVICE mDiskStorageDevice;
extern FW_STORAGE_DEVICE mRawStorageDevice;
extern SCSI_DEVICE *mBootScsiDevice;
extern SCSI_DEVICE *mDiskScsiDevice;
extern AHCI_DEVICE *mBootAhciDevice;
extern AHCI_DEVICE *mDiskAhciDevice;

void storage_set_none(FW_STORAGE_DEVICE *Device);
void storage_set_ide(FW_STORAGE_DEVICE *Device, IDE_DEVICE *Ide);
void storage_set_ahci(FW_STORAGE_DEVICE *Device, AHCI_DEVICE *Ahci);
void storage_set_scsi(FW_STORAGE_DEVICE *Device, SCSI_DEVICE *Scsi);
BOOLEAN storage_device_present(const FW_STORAGE_DEVICE *Device);
BOOLEAN storage_present(const FW_STORAGE_DEVICE *Device);
BOOLEAN storage_same_device(const FW_STORAGE_DEVICE *Left,
                            const FW_STORAGE_DEVICE *Right);
BOOLEAN storage_is_cd(const FW_STORAGE_DEVICE *Device);
BOOLEAN storage_read_only(const FW_STORAGE_DEVICE *Device);
BOOLEAN storage_removable(const FW_STORAGE_DEVICE *Device);
BOOLEAN storage_write_caching(const FW_STORAGE_DEVICE *Device);
UINT32 storage_block_size(const FW_STORAGE_DEVICE *Device);
UINT64 storage_last_lba(const FW_STORAGE_DEVICE *Device);
BOOLEAN storage_read_blocks(const FW_STORAGE_DEVICE *Device,
                            UINT8 *Buffer, UINT64 Lba, UINT32 Count);
BOOLEAN storage_write_blocks(const FW_STORAGE_DEVICE *Device,
                             const UINT8 *Buffer, UINT64 Lba, UINT32 Count);
BOOLEAN storage_flush(const FW_STORAGE_DEVICE *Device);
BOOLEAN storage_refresh_media(const FW_STORAGE_DEVICE *Device);
BOOLEAN storage_reset(const FW_STORAGE_DEVICE *Device,
                      BOOLEAN ExtendedVerification);
void storage_invalidate_cache(const FW_STORAGE_DEVICE *Device);
void ahci_probe_devices(void);
void scsi_probe_devices(void);
const CHAR8 *scsi_transport_name(void);
void ahci_stop_all_ports(void);

typedef struct {
    UINT8 Bus;
    UINT8 Device;
    UINT8 Function;
} PCI_DEVICE_LOCATION;

/* IDE channel controller configuration (one per ATA channel). */
typedef struct {
    UINT64 data_base;    /* data port base (8-byte range) */
    UINT64 ctrl_base;    /* alt-status/control port */
    UINT64 bmdma_base;   /* PCI IDE bus-master base for this channel */
    UINT8  has_bmdma;    /* 1=PCI bus-master IDE registers available */
    UINT8  present;      /* 1=channel I/O bases configured */
} IDE_CONFIG;
#define PCI_CLASS_REVISION_OFFSET     0x08U
/* Programming interface; for IDE, bits 0 and 2 select native-mode channels. */
#define PCI_CFG_CLASS_PROG_OFFSET     0x09U
#define PCI_CFG_COMMAND_OFFSET        0x04U
#define PCI_CFG_COMMAND_IO_SPACE      0x0001U
#define PCI_CFG_COMMAND_MEMORY_SPACE  0x0002U
#define PCI_CFG_COMMAND_BUS_MASTER    0x0004U
#define PCI_HEADER_TYPE_OFFSET        0x0eU
#define PCI_HEADER_TYPE_MULTI_FUNC    0x80U
#define PCI_BASE_CLASS_MASS_STORAGE   0x01U
#define PCI_SUB_CLASS_IDE             0x01U
#define PCI_IDE_BAR0_OFFSET           0x10U
#define PCI_IDE_BAR1_OFFSET           0x14U
#define PCI_IDE_BAR2_OFFSET           0x18U
#define PCI_IDE_BAR3_OFFSET           0x1cU
#define PCI_IDE_BAR4_OFFSET           0x20U
#define PCI_IDE_DATA0_BAR             0x00000801U
#define PCI_IDE_CTRL0_BAR             0x00000809U
#define PCI_IDE_DATA1_BAR             0x00000811U
#define PCI_IDE_CTRL1_BAR             0x00000819U
#define PCI_IDE_BMDMA_BAR             0x0000c001U
#define PCI_MAX_BUSES                 256U
#define PCI_MAX_DEVICES               32U
#define PCI_MAX_FUNCTIONS             8U

extern IDE_CONFIG gIde;
#define IDE_CMD_OFF    0x07

UINT64 pci_config_read_value(UINT64 Segment, UINT64 Bus, UINT64 Device,
                             UINT64 Function, UINT64 Offset, UINTN Size);
void pci_config_write_value(UINT64 Segment, UINT64 Bus, UINT64 Device,
                            UINT64 Function, UINT64 Offset,
                            UINTN Size, UINT64 Value);

extern IDE_DEVICE *mBootIdeDevice;
extern IDE_DEVICE *mHardDiskIdeDevice;
void ide_probe_primary_devices(void);
IDE_DEVICE *ide_pick_boot_device(void);

/* IDE driver services used by the storage abstraction (storage.c). */
UINT8 ata_pio_read8(UINT64 port);
void ata_pio_write8(UINT64 port, UINT8 val);
void ata_pio_poll_delay(void);
BOOLEAN ata_pio_identify(IDE_DEVICE *dev, UINT8 command, UINT16 *identify);
BOOLEAN ata_pio_read_sectors(IDE_DEVICE *dev, UINT8 *buf, UINT32 lba,
                             UINTN count);
void ata_read_cache_invalidate(IDE_DEVICE *dev);
BOOLEAN atapi_refresh_media(IDE_DEVICE *Dev);
void ide_activate(const IDE_DEVICE *dev);
void ide_select_device(UINT8 drive_select);
UINT8 ide_lba_drive_select(const IDE_DEVICE *dev, UINT32 lba);
BOOLEAN ata_pio_wait_not_busy(VOID);
BOOLEAN ata_read_sectors(IDE_DEVICE *dev, UINT8 *buf, UINT32 lba,
                         UINTN count);
BOOLEAN ata_write_sectors(IDE_DEVICE *dev, const UINT8 *buf, UINT32 lba,
                          UINTN count);
BOOLEAN ata_pio_read_sector_cached(IDE_DEVICE *dev, UINT8 *buf, UINT32 lba);
BOOLEAN atapi_read_sectors(IDE_DEVICE *dev, UINT8 *buf, UINT32 lba,
                           UINT32 count);

#endif /* IA64_FIRMWARE_FW_STORAGE_H */
