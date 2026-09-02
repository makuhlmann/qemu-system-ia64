/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * ATA/ATAPI PIO + bus-master DMA (CMD646) IDE driver, both channels.
 * Extracted verbatim from firmware.c (Phase 1 of
 * plans/firmware-rework-plan.md).
 */

#include "fw-base.h"
#include "fw-services.h"
#include "fw-storage.h"
#include "fw-platform-layout.h"

/* --- ATA PIO Block I/O driver --------------------------------------------- */

/* IDE_DEVICE lives in fw-storage.h (shared with storage.c). */

/* IDE_CONFIG lives in fw-storage.h. */

#define IDE_CHANNEL_COUNT 2U

/*
 * Active-channel scratch register set.  Every IDE operation loads this from
 * the target device's channel via ide_activate() before touching ports, so
 * the many existing gIde.<port> sites stay channel-agnostic and unchanged.
 */
IDE_CONFIG gIde = {
    .data_base  = LEGACY_IO_BASE + 0x1F0U,
    .ctrl_base  = LEGACY_IO_BASE + 0x3F6U,
    .bmdma_base = 0,
    .has_bmdma  = 0,
};

/* Per-channel controller state, defaulting to the legacy primary/secondary
 * ISA port bases until ide_configure_channels_from_pci() reprograms them. */
static IDE_CONFIG gIdeChannels[IDE_CHANNEL_COUNT] = {
    { .data_base = LEGACY_IO_BASE + 0x1F0U,
      .ctrl_base = LEGACY_IO_BASE + 0x3F6U, .present = 0 },
    { .data_base = LEGACY_IO_BASE + 0x170U,
      .ctrl_base = LEGACY_IO_BASE + 0x376U, .present = 0 },
};

/* Primary master/slave then secondary master/slave. */
static IDE_DEVICE mIdeDevices[4] = {
    { .unit = 0, .channel = 0 },
    { .unit = 1, .channel = 0 },
    { .unit = 0, .channel = 1 },
    { .unit = 1, .channel = 1 },
};
IDE_DEVICE *mBootIdeDevice = &mIdeDevices[0];
IDE_DEVICE *mHardDiskIdeDevice;

/*
 * Load a device's channel port bases into the active gIde scratch.  IDE
 * access in this firmware is strictly serial, so switching the active
 * channel immediately before an operation is sufficient (and idempotent for
 * nested calls that re-select the same device's channel).
 */
void ide_activate(const IDE_DEVICE *dev)
{
    if (dev != NULL && dev->channel < IDE_CHANNEL_COUNT) {
        gIde = gIdeChannels[dev->channel];
    }
}

/* PCI config-space constants live in fw-storage.h. */

/* PCI_DEVICE_LOCATION lives in fw-storage.h. */

/* Register offsets from data_base */
#define IDE_DATA_OFF   0x00
#define IDE_ERR_OFF    0x01
#define IDE_NSEC_OFF   0x02
#define IDE_LBALO_OFF  0x03
#define IDE_LBAMID_OFF 0x04
#define IDE_LBAHI_OFF  0x05
#define IDE_DRV_OFF    0x06

/* ATA command/status + ATAPI sector macros live in fw-storage.h. */

#define IDE_BMDMA_CMD_OFF       0x00U
#define IDE_BMDMA_STATUS_OFF    0x02U
#define IDE_BMDMA_PRDT_OFF      0x04U
#define IDE_BMDMA_CMD_START     0x01U
#define IDE_BMDMA_CMD_READ      0x08U
#define IDE_BMDMA_STATUS_ACTIVE 0x01U
#define IDE_BMDMA_STATUS_ERROR  0x02U
#define IDE_BMDMA_STATUS_INT    0x04U
#define IDE_BMDMA_PRD_EOT       0x80000000U
#define IDE_BMDMA_PRD_MAX       8U

typedef struct {
    UINT32 BaseAddress;
    UINT32 ByteCount;
} __attribute__((packed, aligned(8))) IDE_BMDMA_PRD;

static IDE_BMDMA_PRD mIdeBmdmaPrd[IDE_BMDMA_PRD_MAX];

static volatile UINT8 *ata_reg(UINT64 offset)
{
    return (volatile UINT8 *)(UINTN)offset;
}

static volatile UINT16 *ata_reg16(UINT64 offset)
{
    return (volatile UINT16 *)(UINTN)offset;
}

static volatile UINT32 *ata_reg32(UINT64 offset)
{
    return (volatile UINT32 *)(UINTN)offset;
}

UINT8 ata_pio_read8(UINT64 port)
{
    return *ata_reg(port);
}

void ata_pio_write8(UINT64 port, UINT8 val)
{
    *ata_reg(port) = val;
}

static void ata_pio_write32(UINT64 port, UINT32 val)
{
    *ata_reg32(port) = val;
}

static void ata_pio_read16(UINT64 port, UINT16 *dst, UINTN count)
{
    UINTN i;
    UINT16 *p = (UINT16 *)dst;
    for (i = 0; i < count; i++) {
        p[i] = *ata_reg16(port);
    }
}

static void ata_pio_read16_to_bytes(UINT64 port, UINT8 *dst, UINTN count)
{
    UINTN i;

    for (i = 0; i < count; i++) {
        UINT16 word = *ata_reg16(port);

        dst[i * 2] = (UINT8)(word & 0xffU);
        dst[i * 2 + 1] = (UINT8)(word >> 8);
    }
}

static void ata_pio_write16(UINT64 port, const UINT16 *src, UINTN count)
{
    UINTN i;
    for (i = 0; i < count; i++) {
        *ata_reg16(port) = src[i];
    }
}

static void ata_pio_write16_from_bytes(UINT64 port, const UINT8 *src,
                                       UINTN count)
{
    UINTN i;

    for (i = 0; i < count; i++) {
        UINT16 word = (UINT16)src[i * 2] |
                      ((UINT16)src[i * 2 + 1] << 8);

        *ata_reg16(port) = word;
    }
}

void ata_pio_poll_delay(void)
{
    /*
     * ATA software delays are specified as an I/O-bus delay.  Four reads of
     * the alternate-status register are the conventional 400ns delay and avoid
     * burning guest cycles in an arbitrary nop loop.
     */
    (void)ata_pio_read8(gIde.ctrl_base);
    (void)ata_pio_read8(gIde.ctrl_base);
    (void)ata_pio_read8(gIde.ctrl_base);
    (void)ata_pio_read8(gIde.ctrl_base);
}

static BOOLEAN ide_io_bar_address(UINT32 Bar, UINT64 *Address)
{
    UINT64 io_base;

    if (Address == NULL || Bar == 0 || Bar == 0xffffffffU ||
        (Bar & 1U) == 0 || (Bar & ~(UINT64)3U) == 0) {
        return 0;
    }

    io_base = LEGACY_IO_BASE + (Bar & ~(UINT64)3U);
    if (io_base < LEGACY_IO_BASE || io_base >= LEGACY_IO_LIMIT) {
        return 0;
    }

    *Address = io_base;
    return 1;
}

static BOOLEAN ide_find_pci_controller(PCI_DEVICE_LOCATION *Location)
{
    UINT16 bus;
    UINT8 device;
    UINT8 function;
    UINT8 function_count;

    if (Location == NULL) {
        return 0;
    }

    for (bus = 0; bus < PCI_MAX_BUSES; bus++) {
        for (device = 0; device < PCI_MAX_DEVICES; device++) {
            function_count = 1;
            for (function = 0; function < function_count; function++) {
                UINT32 vendor;
                UINT32 class_rev;
                UINT8 base_class;
                UINT8 sub_class;

                vendor = (UINT32)pci_config_read_value(0, (UINT8)bus,
                                                       device, function, 0, 2);
                if (vendor == 0xffffU) {
                    if (function == 0) {
                        break;
                    }
                    continue;
                }

                if (function == 0) {
                    UINT8 header_type = (UINT8)pci_config_read_value(
                        0, (UINT8)bus, device, function,
                        PCI_HEADER_TYPE_OFFSET, 1);
                    if ((header_type & PCI_HEADER_TYPE_MULTI_FUNC) != 0) {
                        function_count = PCI_MAX_FUNCTIONS;
                    }
                }

                class_rev = (UINT32)pci_config_read_value(
                    0, (UINT8)bus, device, function,
                    PCI_CLASS_REVISION_OFFSET, 4);
                sub_class = (UINT8)((class_rev >> 16) & 0xffU);
                base_class = (UINT8)((class_rev >> 24) & 0xffU);
                if (base_class == PCI_BASE_CLASS_MASS_STORAGE &&
                    sub_class == PCI_SUB_CLASS_IDE) {
                    Location->Bus = (UINT8)bus;
                    Location->Device = device;
                    Location->Function = function;
                    return 1;
                }
            }
        }
    }

    return 0;
}

/*
 * The fixed ports an IDE channel decodes in compatibility mode, in I/O BAR
 * form so they flow through the same address decoding as a programmed BAR.
 */
static const UINT32 ide_legacy_data_bar[IDE_CHANNEL_COUNT] = {
    0x000001F1U, 0x00000171U,
};
static const UINT32 ide_legacy_ctrl_bar[IDE_CHANNEL_COUNT] = {
    0x000003F5U, 0x00000375U,
};

static BOOLEAN ide_configure_channels_from_pci(void)
{
    static const UINT8 bar_offset[5] = {
        PCI_IDE_BAR0_OFFSET, PCI_IDE_BAR1_OFFSET, PCI_IDE_BAR2_OFFSET,
        PCI_IDE_BAR3_OFFSET, PCI_IDE_BAR4_OFFSET,
    };
    PCI_DEVICE_LOCATION location;
    UINT32 bar[5];
    UINT8 prog_if;
    UINT64 base;
    UINT64 bmdma_base = 0;
    UINT16 command;
    BOOLEAN dma_ok;
    UINTN i;
    UINTN ch;

    for (ch = 0; ch < IDE_CHANNEL_COUNT; ch++) {
        gIdeChannels[ch].bmdma_base = 0;
        gIdeChannels[ch].has_bmdma = 0;
        gIdeChannels[ch].present = 0;
    }

    if (!ide_find_pci_controller(&location)) {
        return 0;
    }

    /*
     * Bits 0 and 2 of the programming interface say whether the primary and
     * secondary channels are in native mode.  A channel in compatibility mode
     * -- which is how the 82468GX I/O and Firmware Bridge's IDE function
     * ships -- decodes the fixed legacy ports and ignores its BARs entirely,
     * so it must be addressed there and not through BAR values that read back
     * as zero.
     */
    prog_if = (UINT8)pci_config_read_value(0, location.Bus, location.Device,
                                           location.Function,
                                           PCI_CFG_CLASS_PROG_OFFSET, 1);

    for (i = 0; i < 5; i++) {
        bar[i] = (UINT32)pci_config_read_value(0, location.Bus, location.Device,
                                               location.Function,
                                               bar_offset[i], 4);
    }
    for (ch = 0; ch < IDE_CHANNEL_COUNT; ch++) {
        if (!(prog_if & (1U << (ch * 2)))) {
            bar[ch * 2] = ide_legacy_data_bar[ch];
            bar[ch * 2 + 1] = ide_legacy_ctrl_bar[ch];
        }
    }

    /*
     * Command-line and synthetic PCI controllers arrive with unassigned BARs.
     * Allocate the platform's reserved legacy-style IDE I/O ranges -- both
     * channels plus the 16-byte bus-master register file -- on demand, only
     * once an IDE controller has actually been requested.
     */
    if (!ide_io_bar_address(bar[0], &base) || base + 7U >= LEGACY_IO_LIMIT ||
        !ide_io_bar_address(bar[1], &base) || base + 2U >= LEGACY_IO_LIMIT) {
        bar[0] = PCI_IDE_DATA0_BAR;
        bar[1] = PCI_IDE_CTRL0_BAR;
        bar[2] = PCI_IDE_DATA1_BAR;
        bar[3] = PCI_IDE_CTRL1_BAR;
        bar[4] = PCI_IDE_BMDMA_BAR;
        /*
         * Only a native-mode primary can reach this, but its secondary may
         * still be in compatibility mode, so put that channel's fixed ports
         * back after the blanket assignment above.
         */
        for (ch = 0; ch < IDE_CHANNEL_COUNT; ch++) {
            if (!(prog_if & (1U << (ch * 2)))) {
                bar[ch * 2] = ide_legacy_data_bar[ch];
                bar[ch * 2 + 1] = ide_legacy_ctrl_bar[ch];
            }
        }
        for (i = 0; i < 5; i++) {
            if (i < 4 && !(prog_if & (1U << ((i / 2) * 2)))) {
                continue;   /* compatibility-mode channel: BARs are ignored */
            }
            pci_config_write_value(0, location.Bus, location.Device,
                                   location.Function, bar_offset[i], 4, bar[i]);
        }
    }

    command = (UINT16)pci_config_read_value(0, location.Bus, location.Device,
                                            location.Function,
                                            PCI_CFG_COMMAND_OFFSET, 2);
    command |= PCI_CFG_COMMAND_IO_SPACE;

    /* The bus-master register file spans 16 bytes: primary channel at +0,
     * secondary at +8 (PCI IDE / cmd646). */
    dma_ok = fw_handoff_ide_dma_enabled() &&
             ide_io_bar_address(bar[4], &bmdma_base) &&
             bmdma_base + 15U < LEGACY_IO_LIMIT;

    for (ch = 0; ch < IDE_CHANNEL_COUNT; ch++) {
        UINT64 data_base;
        UINT64 ctrl_base;

        if (!ide_io_bar_address(bar[ch * 2], &data_base) ||
            !ide_io_bar_address(bar[ch * 2 + 1], &ctrl_base) ||
            data_base + 7U >= LEGACY_IO_LIMIT ||
            ctrl_base + 2U >= LEGACY_IO_LIMIT) {
            continue;   /* channel BAR unassigned or out of range */
        }
        gIdeChannels[ch].data_base = data_base;
        gIdeChannels[ch].ctrl_base = ctrl_base + 2U;
        gIdeChannels[ch].present = 1;
        if (dma_ok) {
            gIdeChannels[ch].bmdma_base = bmdma_base + ch * 8U;
            gIdeChannels[ch].has_bmdma = 1;
            command |= PCI_CFG_COMMAND_BUS_MASTER;
        }
    }

    if (!gIdeChannels[0].present) {
        return 0;
    }

    pci_config_write_value(0, location.Bus, location.Device,
                           location.Function, PCI_CFG_COMMAND_OFFSET, 2,
                           command);

    gIde = gIdeChannels[0];   /* default active channel = primary */

    uart_puts("IDE controller:       PCI BAR primary data=0x");
    uart_put_hex64(gIdeChannels[0].data_base);
    uart_puts(" ctrl=0x");
    uart_put_hex64(gIdeChannels[0].ctrl_base);
    if (gIdeChannels[0].has_bmdma) {
        uart_puts(" bmdma=0x");
        uart_put_hex64(gIdeChannels[0].bmdma_base);
    }
    if (gIdeChannels[1].present) {
        uart_puts(" | secondary data=0x");
        uart_put_hex64(gIdeChannels[1].data_base);
        uart_puts(" ctrl=0x");
        uart_put_hex64(gIdeChannels[1].ctrl_base);
    }
    uart_puts("\r\n");
    return 1;
}

static BOOLEAN ata_pio_wait_ready_timeout(UINT64 cmd_port, UINTN timeout)
{
    UINT8 status;

    do {
        status = ata_pio_read8(cmd_port);
        if (status & (ATA_SR_ERR | ATA_SR_DF))
            return 0;
        if (!(status & ATA_SR_BSY) && (status & ATA_SR_DRQ))
            return 1;
        ata_pio_poll_delay();
        timeout--;
    } while (timeout > 0);
    return 0;
}

static BOOLEAN ata_pio_wait_ready(UINT64 cmd_port)
{
    return ata_pio_wait_ready_timeout(cmd_port, 1000000);
}

static const char *ide_unit_name(const IDE_DEVICE *dev)
{
    if (dev != NULL && dev->channel != 0) {
        return dev->unit != 0 ? "secondary slave" : "secondary master";
    }
    return (dev != NULL && dev->unit != 0) ? "primary slave" : "primary master";
}

static UINT8 ide_packet_drive_select(const IDE_DEVICE *dev)
{
    return (UINT8)(0xA0U | ((dev != NULL && dev->unit != 0) ? 0x10U : 0U));
}

UINT8 ide_lba_drive_select(const IDE_DEVICE *dev, UINT32 lba)
{
    UINT8 unit = (dev != NULL && dev->unit != 0) ? 0x10U : 0U;

    return (UINT8)(0xE0U | unit | ((lba >> 24) & 0x0FU));
}

void ide_select_device(UINT8 drive_select)
{
    ata_pio_write8(gIde.data_base + IDE_DRV_OFF, drive_select);
    ata_pio_poll_delay();
}

BOOLEAN ata_pio_identify(IDE_DEVICE *dev, UINT8 command,
                                UINT16 *identify)
{
    if (dev == NULL) {
        return 0;
    }

    ide_activate(dev);
    ide_select_device(ide_packet_drive_select(dev));
    ata_pio_write8(gIde.data_base + IDE_NSEC_OFF, 0);
    ata_pio_write8(gIde.data_base + IDE_LBALO_OFF, 0);
    ata_pio_write8(gIde.data_base + IDE_LBAMID_OFF, 0);
    ata_pio_write8(gIde.data_base + IDE_LBAHI_OFF, 0);
    ata_pio_write8(gIde.data_base + IDE_CMD_OFF, command);

    if (!ata_pio_wait_ready_timeout(gIde.data_base + IDE_CMD_OFF, 10000)) {
        return 0;
    }

    ata_pio_read16(gIde.data_base + IDE_DATA_OFF, identify, 256);
    return 1;
}


void ide_probe_primary_devices(void)
{
    UINT16 identify[256];
    UINTN i;

    if (!ide_configure_channels_from_pci()) {
        uart_puts("IDE controller:       not present\r\n");
        mHardDiskIdeDevice = NULL;
        for (i = 0; i < FW_ARRAY_SIZE(mIdeDevices); i++) {
            mIdeDevices[i].present = 0;
            mIdeDevices[i].media_present = 0;
            mIdeDevices[i].is_atapi = 0;
            mIdeDevices[i].last_lba = 0;
        }
        return;
    }

    mHardDiskIdeDevice = NULL;
    for (i = 0; i < FW_ARRAY_SIZE(mIdeDevices); i++) {
        IDE_DEVICE *dev = &mIdeDevices[i];

        dev->present = 0;
        dev->media_present = 0;
        dev->is_atapi = 0;
        dev->last_lba = 0;

        /* Skip a channel whose I/O ports were never assigned (e.g. a
         * single-channel controller, or the secondary disabled). */
        if (dev->channel >= IDE_CHANNEL_COUNT ||
            !gIdeChannels[dev->channel].present) {
            continue;
        }
        ide_activate(dev);

        if (ata_pio_identify(dev, ATA_CMD_IDENTIFY_PACKET, identify)) {
            dev->present = 1;
            dev->is_atapi = 1;
            (void)atapi_refresh_media(dev);
            uart_puts("IDE device:           ATAPI ");
            uart_puts(ide_unit_name(dev));
            uart_puts(dev->media_present ? " media\r\n" :
                                            " no media\r\n");
            continue;
        }

        if (ata_pio_identify(dev, ATA_CMD_IDENTIFY, identify)) {
            UINT32 sectors = (UINT32)identify[60] |
                             ((UINT32)identify[61] << 16);

            dev->present = 1;
            dev->media_present = sectors != 0;
            dev->is_atapi = 0;
            dev->last_lba = sectors > 0 ?
                (UINT64)(sectors - 1U) : 0xFFFFFFFFULL;
            if (mHardDiskIdeDevice == NULL) {
                mHardDiskIdeDevice = dev;
            }
            uart_puts("IDE device:           ATA ");
            uart_puts(ide_unit_name(dev));
            uart_puts("\r\n");
            continue;
        }

        uart_puts("IDE device:           no ");
        uart_puts(ide_unit_name(dev));
        uart_puts("\r\n");
    }
}

/*
 * Choose the IDE device to expose as the optical/boot candidate.  Prefer any
 * present ATAPI CD-ROM -- on either channel, master or slave -- so that a data
 * disk on the primary master does not shadow a bootable CD elsewhere on the
 * IDE bus; fall back to the first present device for plain fixed-disk boot.
 */
IDE_DEVICE *ide_pick_boot_device(void)
{
    UINTN i;

    for (i = 0; i < FW_ARRAY_SIZE(mIdeDevices); i++) {
        if (mIdeDevices[i].present && mIdeDevices[i].is_atapi) {
            return &mIdeDevices[i];
        }
    }
    for (i = 0; i < FW_ARRAY_SIZE(mIdeDevices); i++) {
        if (mIdeDevices[i].present) {
            return &mIdeDevices[i];
        }
    }
    return NULL;
}

BOOLEAN ata_pio_read_sectors(IDE_DEVICE *dev, UINT8 *buf, UINT32 lba,
                                    UINTN count)
{
    UINTN sector;

    ide_activate(dev);
    if (dev == NULL || !dev->present || dev->is_atapi ||
        count == 0 || count > 255) {
        return 0;
    }

    ide_select_device(ide_lba_drive_select(dev, lba));
    ata_pio_write8(gIde.data_base + IDE_NSEC_OFF, (UINT8)count);
    ata_pio_write8(gIde.data_base + IDE_LBALO_OFF, lba & 0xFF);
    ata_pio_write8(gIde.data_base + IDE_LBAMID_OFF, (lba >> 8) & 0xFF);
    ata_pio_write8(gIde.data_base + IDE_LBAHI_OFF, (lba >> 16) & 0xFF);
    ata_pio_write8(gIde.data_base + IDE_CMD_OFF, ATA_CMD_READ_SECS);

    for (sector = 0; sector < count; sector++) {
        if (!ata_pio_wait_ready(gIde.data_base + IDE_CMD_OFF)) {
            return 0;
        }
        ata_pio_read16_to_bytes(gIde.data_base + IDE_DATA_OFF,
                                buf + sector * 512, 256);
    }
    return 1;
}

BOOLEAN ata_pio_wait_not_busy(VOID)
{
    UINTN timeout = 1000000;
    UINT8 status;

    do {
        status = ata_pio_read8(gIde.data_base + IDE_CMD_OFF);
        if ((status & ATA_SR_BSY) == 0) {
            return (status & (ATA_SR_ERR | ATA_SR_DF)) == 0;
        }
        ata_pio_poll_delay();
        timeout--;
    } while (timeout > 0);
    return 0;
}

static BOOLEAN ata_pio_write_sectors(IDE_DEVICE *dev, const UINT8 *buf,
                                     UINT32 lba, UINTN count)
{
    UINTN sector;

    ide_activate(dev);
    if (dev == NULL || !dev->present || dev->is_atapi ||
        count == 0 || count > 255) {
        return 0;
    }

    ide_select_device(ide_lba_drive_select(dev, lba));
    ata_pio_write8(gIde.data_base + IDE_NSEC_OFF, (UINT8)count);
    ata_pio_write8(gIde.data_base + IDE_LBALO_OFF, lba & 0xFF);
    ata_pio_write8(gIde.data_base + IDE_LBAMID_OFF, (lba >> 8) & 0xFF);
    ata_pio_write8(gIde.data_base + IDE_LBAHI_OFF, (lba >> 16) & 0xFF);
    ata_pio_write8(gIde.data_base + IDE_CMD_OFF, ATA_CMD_WRITE_SECS);

    for (sector = 0; sector < count; sector++) {
        if (!ata_pio_wait_ready(gIde.data_base + IDE_CMD_OFF)) {
            return 0;
        }
        ata_pio_write16_from_bytes(gIde.data_base + IDE_DATA_OFF,
                                   buf + sector * 512, 256);
        __asm__ __volatile__ ("mf" ::: "memory");
    }

    if (!ata_pio_wait_not_busy()) {
        return 0;
    }
    ata_pio_write8(gIde.data_base + IDE_CMD_OFF, ATA_CMD_FLUSH_CACHE);
    return ata_pio_wait_not_busy();
}

static BOOLEAN ide_bmdma_addr32(const VOID *Ptr, UINT32 *Address);
static BOOLEAN ide_bmdma_prepare_prdt(const VOID *Buffer, UINT32 ByteCount,
                                      UINT32 *PrdtAddress);
static void ide_bmdma_stop(void);
static BOOLEAN ide_bmdma_wait(UINT32 lba, UINT32 chunk);

static BOOLEAN ata_dma_read_sectors(IDE_DEVICE *dev, UINT8 *buf, UINT32 lba,
                                    UINTN count)
{
    UINT32 done = 0;
    UINT32 prd_addr;

    ide_activate(dev);
    if (dev == NULL || !dev->present || dev->is_atapi || !gIde.has_bmdma ||
        count == 0 || count > 255) {
        return 0;
    }

    while (done < count) {
        UINT32 chunk = (UINT32)(count - done);
        UINT32 byte_count;

        if (chunk > 255) {
            chunk = 255;
        }
        byte_count = chunk * 512U;
        if (!ide_bmdma_prepare_prdt(buf + done * 512U, byte_count,
                                    &prd_addr)) {
            return 0;
        }

        ide_select_device(ide_lba_drive_select(dev, lba));
        ide_bmdma_stop();
        ata_pio_write32(gIde.bmdma_base + IDE_BMDMA_PRDT_OFF, prd_addr);
        ata_pio_write8(gIde.bmdma_base + IDE_BMDMA_CMD_OFF,
                       IDE_BMDMA_CMD_READ | IDE_BMDMA_CMD_START);

        ata_pio_write8(gIde.data_base + IDE_NSEC_OFF, (UINT8)chunk);
        ata_pio_write8(gIde.data_base + IDE_LBALO_OFF, lba & 0xFF);
        ata_pio_write8(gIde.data_base + IDE_LBAMID_OFF, (lba >> 8) & 0xFF);
        ata_pio_write8(gIde.data_base + IDE_LBAHI_OFF, (lba >> 16) & 0xFF);
        ata_pio_write8(gIde.data_base + IDE_CMD_OFF, ATA_CMD_READ_DMA);

        if (!ide_bmdma_wait(lba, chunk)) {
            return 0;
        }

        lba += chunk;
        done += chunk;
    }

    return 1;
}

static BOOLEAN ata_dma_write_sectors(IDE_DEVICE *dev, const UINT8 *buf,
                                     UINT32 lba, UINTN count)
{
    UINT32 done = 0;
    UINT32 prd_addr;

    ide_activate(dev);
    if (dev == NULL || !dev->present || dev->is_atapi || !gIde.has_bmdma ||
        count == 0 || count > 255) {
        return 0;
    }

    while (done < count) {
        UINT32 chunk = (UINT32)(count - done);
        UINT32 byte_count;

        if (chunk > 255) {
            chunk = 255;
        }
        byte_count = chunk * 512U;
        if (!ide_bmdma_prepare_prdt(buf + done * 512U, byte_count,
                                    &prd_addr)) {
            return 0;
        }

        ide_select_device(ide_lba_drive_select(dev, lba));
        ide_bmdma_stop();
        ata_pio_write32(gIde.bmdma_base + IDE_BMDMA_PRDT_OFF, prd_addr);
        ata_pio_write8(gIde.bmdma_base + IDE_BMDMA_CMD_OFF,
                       IDE_BMDMA_CMD_START);

        ata_pio_write8(gIde.data_base + IDE_NSEC_OFF, (UINT8)chunk);
        ata_pio_write8(gIde.data_base + IDE_LBALO_OFF, lba & 0xFF);
        ata_pio_write8(gIde.data_base + IDE_LBAMID_OFF, (lba >> 8) & 0xFF);
        ata_pio_write8(gIde.data_base + IDE_LBAHI_OFF, (lba >> 16) & 0xFF);
        ata_pio_write8(gIde.data_base + IDE_CMD_OFF, ATA_CMD_WRITE_DMA);

        if (!ide_bmdma_wait(lba, chunk)) {
            return 0;
        }

        lba += chunk;
        done += chunk;
    }

    if (!ata_pio_wait_not_busy()) {
        return 0;
    }
    ata_pio_write8(gIde.data_base + IDE_CMD_OFF, ATA_CMD_FLUSH_CACHE);
    return ata_pio_wait_not_busy();
}

BOOLEAN ata_read_sectors(IDE_DEVICE *dev, UINT8 *buf, UINT32 lba,
                                UINTN count)
{
    ide_activate(dev);
    if (gIde.has_bmdma && ata_dma_read_sectors(dev, buf, lba, count)) {
        return 1;
    }
    return ata_pio_read_sectors(dev, buf, lba, count);
}

BOOLEAN ata_write_sectors(IDE_DEVICE *dev, const UINT8 *buf, UINT32 lba,
                                 UINTN count)
{
    ide_activate(dev);
    if (gIde.has_bmdma && ata_dma_write_sectors(dev, buf, lba, count)) {
        return 1;
    }
    return ata_pio_write_sectors(dev, buf, lba, count);
}

#define ATA_READ_CACHE_SECTORS 32U

static UINT8 mAtaReadCache[ATA_READ_CACHE_SECTORS * 512U]
    __attribute__((aligned(8)));
static IDE_DEVICE *mAtaReadCacheDevice;
static UINT32 mAtaReadCacheLba;
static UINT32 mAtaReadCacheCount;
static BOOLEAN mAtaReadCacheValid;

void ata_read_cache_invalidate(IDE_DEVICE *dev)
{
    if (dev == NULL || mAtaReadCacheDevice == dev) {
        mAtaReadCacheValid = 0;
        mAtaReadCacheDevice = NULL;
    }
}

BOOLEAN ata_pio_read_sector_cached(IDE_DEVICE *dev, UINT8 *buf,
                                          UINT32 lba)
{
    UINT32 count = ATA_READ_CACHE_SECTORS;

    if (mAtaReadCacheValid && mAtaReadCacheDevice == dev &&
        lba >= mAtaReadCacheLba &&
        lba < mAtaReadCacheLba + mAtaReadCacheCount) {
        fw_copy_mem_fast(buf, mAtaReadCache +
                         (lba - mAtaReadCacheLba) * 512U, 512);
        return 1;
    }

    if (dev == NULL || (UINT64)lba > dev->last_lba) {
        return 0;
    }
    if ((UINT64)lba + count - 1U > dev->last_lba) {
        count = (UINT32)(dev->last_lba - lba + 1U);
    }
    if (count == 0) {
        return 0;
    }

    mAtaReadCacheValid = 0;
    if (!ata_read_sectors(dev, mAtaReadCache, lba, count)) {
        return 0;
    }

    mAtaReadCacheDevice = dev;
    mAtaReadCacheLba = lba;
    mAtaReadCacheCount = count;
    mAtaReadCacheValid = 1;
    fw_copy_mem_fast(buf, mAtaReadCache, 512);
    return 1;
}

static void atapi_build_read10_cdb(UINT16 cdb[6], UINT32 lba, UINT32 count)
{
    /* READ(10) CDB: 0x28, flags, LBA(4), group, len(2), control */
    cdb[0] = 0x0028;                              /* bytes 0..1 */
    cdb[1] = ((lba >> 24) & 0xffU) |
             (((lba >> 16) & 0xffU) << 8);       /* bytes 2..3 */
    cdb[2] = ((lba >> 8) & 0xffU) |
             ((lba & 0xffU) << 8);               /* bytes 4..5 */
    cdb[3] = ((count >> 8) & 0xffU) << 8;         /* bytes 6..7 */
    cdb[4] = count & 0xffU;                       /* bytes 8..9 */
    cdb[5] = 0x0000;                              /* bytes 10..11 */
}

static BOOLEAN atapi_pio_wait_data(UINT32 lba, UINT32 chunk,
                                   UINTN remaining, UINTN *byte_count)
{
    UINTN timeout = 1000000;
    UINT8 status;

    (void)lba;
    (void)chunk;

    do {
        status = ata_pio_read8(gIde.data_base + IDE_CMD_OFF);
        if (status & (ATA_SR_ERR | ATA_SR_DF)) {
            return 0;
        }
        if (!(status & ATA_SR_BSY) && (status & ATA_SR_DRQ)) {
            UINTN count;

            count = ata_pio_read8(gIde.data_base + IDE_LBAMID_OFF);
            count |= (UINTN)ata_pio_read8(gIde.data_base + IDE_LBAHI_OFF) << 8;
            if (count == 0 || (count & 1U) != 0) {
                return 0;
            }
            if (count > remaining) {
                count = remaining;
            }
            if (count > ATAPI_SECTOR_SIZE) {
                count = ATAPI_SECTOR_SIZE;
            }
            *byte_count = count;
            return 1;
        }
        ata_pio_poll_delay();
        if ((timeout & 0x3FF) == 0) {
            __asm__ __volatile__ ("mf" ::: "memory");
        }
        timeout--;
    } while (timeout > 0);
    return 0;
}

static BOOLEAN atapi_pio_wait_complete(UINT32 lba, UINT32 chunk)
{
    UINTN timeout = 1000000;
    UINT8 status;

    (void)lba;
    (void)chunk;

    do {
        status = ata_pio_read8(gIde.data_base + IDE_CMD_OFF);
        if (status & (ATA_SR_ERR | ATA_SR_DF)) {
            return 0;
        }
        if (!(status & ATA_SR_BSY) && !(status & ATA_SR_DRQ)) {
            return 1;
        }
        ata_pio_poll_delay();
        if ((timeout & 0x3FF) == 0) {
            __asm__ __volatile__ ("mf" ::: "memory");
        }
        timeout--;
    } while (timeout > 0);
    return 0;
}

static BOOLEAN atapi_packet_data_in(IDE_DEVICE *Dev, const UINT8 *Cdb,
                                    UINTN CdbSize, UINT8 *Buffer,
                                    UINTN BufferSize)
{
    UINTN remaining = BufferSize;
    UINTN offset = 0;

    ide_activate(Dev);
    if (Dev == NULL || !Dev->present || !Dev->is_atapi || Cdb == NULL ||
        CdbSize != 12U || Buffer == NULL || BufferSize == 0 ||
        BufferSize > 0xffffU || (BufferSize & 1U) != 0) {
        return 0;
    }

    ide_select_device(ide_packet_drive_select(Dev));
    ata_pio_write8(gIde.data_base + IDE_ERR_OFF, 0);
    ata_pio_write8(gIde.data_base + IDE_NSEC_OFF, 0);
    ata_pio_write8(gIde.data_base + IDE_LBAMID_OFF,
                   (UINT8)BufferSize);
    ata_pio_write8(gIde.data_base + IDE_LBAHI_OFF,
                   (UINT8)(BufferSize >> 8));
    ata_pio_write8(gIde.data_base + IDE_CMD_OFF, ATA_CMD_PACKET);
    if (!ata_pio_wait_ready(gIde.data_base + IDE_CMD_OFF)) {
        return 0;
    }
    ata_pio_write16_from_bytes(gIde.data_base + IDE_DATA_OFF, Cdb,
                               CdbSize / 2U);

    while (remaining > 0) {
        UINTN byte_count;

        if (!atapi_pio_wait_data(0, 0, remaining, &byte_count) ||
            byte_count == 0 || (byte_count & 1U) != 0) {
            return 0;
        }
        ata_pio_read16_to_bytes(gIde.data_base + IDE_DATA_OFF,
                                Buffer + offset, byte_count / 2U);
        offset += byte_count;
        remaining -= byte_count;
    }
    return atapi_pio_wait_complete(0, 0);
}

BOOLEAN atapi_refresh_media(IDE_DEVICE *Dev)
{
    UINT8 cdb[12];
    UINT8 capacity[8];
    UINT32 block_size;

    if (Dev == NULL || !Dev->present || !Dev->is_atapi) {
        return 0;
    }
    Dev->media_present = 0;
    Dev->last_lba = 0;
    fw_set_mem(cdb, sizeof(cdb), 0);
    cdb[0] = 0x25U; /* READ CAPACITY (10) */
    fw_set_mem(capacity, sizeof(capacity), 0);
    if (!atapi_packet_data_in(Dev, cdb, sizeof(cdb), capacity,
                              sizeof(capacity))) {
        return 0;
    }
    block_size = ((UINT32)capacity[4] << 24) |
                 ((UINT32)capacity[5] << 16) |
                 ((UINT32)capacity[6] << 8) |
                 (UINT32)capacity[7];
    if (block_size != ATAPI_SECTOR_SIZE) {
        return 0;
    }
    Dev->last_lba = ((UINT32)capacity[0] << 24) |
                    ((UINT32)capacity[1] << 16) |
                    ((UINT32)capacity[2] << 8) |
                    (UINT32)capacity[3];
    Dev->media_present = 1;
    return 1;
}

static BOOLEAN atapi_pio_read_sectors(IDE_DEVICE *dev, UINT8 *buf, UINT32 lba,
                                      UINT32 count)
{
    UINT32 done = 0;

    ide_activate(dev);
    if (dev == NULL || !dev->present || !dev->is_atapi) {
        return 0;
    }
    if (count == 0) {
        return 1;
    }

    while (done < count) {
        UINT16 cdb[6];
        UINT32 chunk = count - done;
        UINT32 chunk_lba = lba;
        UINTN remaining;
        UINTN offset = (UINTN)done * ATAPI_SECTOR_SIZE;

        if (chunk > ATAPI_MAX_TRANSFER_SECTORS) {
            chunk = ATAPI_MAX_TRANSFER_SECTORS;
        }

        atapi_build_read10_cdb(cdb, lba, chunk);

        /* PACKET command setup */
        ide_select_device(ide_packet_drive_select(dev));
        ata_pio_write8(gIde.data_base + IDE_ERR_OFF, 0);
        ata_pio_write8(gIde.data_base + IDE_NSEC_OFF, 0);
        ata_pio_write8(gIde.data_base + IDE_LBAMID_OFF,
                       (chunk * ATAPI_SECTOR_SIZE) & 0xffU);
        ata_pio_write8(gIde.data_base + IDE_LBAHI_OFF,
                       ((chunk * ATAPI_SECTOR_SIZE) >> 8) & 0xffU);
        ata_pio_write8(gIde.data_base + IDE_CMD_OFF, ATA_CMD_PACKET);

        if (!ata_pio_wait_ready(gIde.data_base + IDE_CMD_OFF)) {
            return 0;
        }

        ata_pio_write16(gIde.data_base + IDE_DATA_OFF, cdb, 6);

        remaining = (UINTN)chunk * ATAPI_SECTOR_SIZE;
        while (remaining > 0) {
            UINTN byte_count;

            if (!atapi_pio_wait_data(chunk_lba, chunk, remaining,
                                     &byte_count)) {
                return 0;
            }
            ata_pio_read16_to_bytes(gIde.data_base + IDE_DATA_OFF,
                                    buf + offset, byte_count / 2);
            offset += byte_count;
            remaining -= byte_count;
        }

        if (!atapi_pio_wait_complete(chunk_lba, chunk)) {
            return 0;
        }

        lba += chunk;
        done += chunk;
    }
    return 1;
}

static BOOLEAN ide_bmdma_addr32(const VOID *Ptr, UINT32 *Address)
{
    UINTN addr = (UINTN)Ptr;

    if (Address == NULL || (addr >> 32) != 0) {
        return 0;
    }
    *Address = (UINT32)addr;
    return 1;
}

static BOOLEAN ide_bmdma_prepare_prdt(const VOID *Buffer, UINT32 ByteCount,
                                      UINT32 *PrdtAddress)
{
    UINTN addr = (UINTN)Buffer;
    UINT32 remaining = ByteCount;
    UINTN entry = 0;

    if (Buffer == NULL || ByteCount == 0 ||
        !ide_bmdma_addr32(mIdeBmdmaPrd, PrdtAddress)) {
        return 0;
    }

    while (remaining > 0) {
        UINT32 chunk;
        UINT32 boundary;

        if ((addr >> 32) != 0 || entry >= IDE_BMDMA_PRD_MAX) {
            return 0;
        }

        boundary = 0x10000U - ((UINT32)addr & 0xffffU);
        chunk = remaining < boundary ? remaining : boundary;
        if (chunk > 0x10000U) {
            chunk = 0x10000U;
        }

        mIdeBmdmaPrd[entry].BaseAddress = (UINT32)addr;
        mIdeBmdmaPrd[entry].ByteCount =
            chunk == 0x10000U ? 0 : chunk;
        addr += chunk;
        remaining -= chunk;
        entry++;
    }

    mIdeBmdmaPrd[entry - 1U].ByteCount |= IDE_BMDMA_PRD_EOT;
    __asm__ __volatile__ ("mf" ::: "memory");
    return 1;
}

static void ide_bmdma_stop(void)
{
    ata_pio_write8(gIde.bmdma_base + IDE_BMDMA_CMD_OFF, 0);
    ata_pio_write8(gIde.bmdma_base + IDE_BMDMA_STATUS_OFF,
                   IDE_BMDMA_STATUS_ERROR | IDE_BMDMA_STATUS_INT);
}

static BOOLEAN ide_bmdma_wait(UINT32 lba, UINT32 chunk)
{
    UINTN timeout = 1000000;
    UINT8 status;

    (void)lba;
    (void)chunk;

    do {
        status = ata_pio_read8(gIde.bmdma_base + IDE_BMDMA_STATUS_OFF);
        if ((status & IDE_BMDMA_STATUS_ERROR) != 0) {
            ide_bmdma_stop();
            return 0;
        }
        if ((status & (IDE_BMDMA_STATUS_ACTIVE | IDE_BMDMA_STATUS_INT)) ==
            IDE_BMDMA_STATUS_INT) {
            UINT8 ide_status = ata_pio_read8(gIde.data_base + IDE_CMD_OFF);

            if ((ide_status & (ATA_SR_ERR | ATA_SR_DF)) != 0) {
                ide_bmdma_stop();
                return 0;
            }
            ide_bmdma_stop();
            __asm__ __volatile__ ("mf" ::: "memory");
            return 1;
        }
        ata_pio_poll_delay();
        timeout--;
    } while (timeout > 0);

    ide_bmdma_stop();
    return 0;
}

static BOOLEAN atapi_dma_read_sectors(IDE_DEVICE *dev, UINT8 *buf, UINT32 lba,
                                      UINT32 count)
{
    UINT32 done = 0;
    UINT32 prd_addr;

    ide_activate(dev);
    if (dev == NULL || !dev->present || !dev->is_atapi || !gIde.has_bmdma) {
        return 0;
    }

    while (done < count) {
        UINT16 cdb[6];
        UINT32 chunk = count - done;
        UINT32 chunk_lba = lba;
        UINT32 byte_count;

        if (chunk > ATAPI_MAX_TRANSFER_SECTORS) {
            chunk = ATAPI_MAX_TRANSFER_SECTORS;
        }
        byte_count = chunk * ATAPI_SECTOR_SIZE;

        if (!ide_bmdma_prepare_prdt(buf + (UINTN)done * ATAPI_SECTOR_SIZE,
                                    byte_count, &prd_addr)) {
            return 0;
        }

        atapi_build_read10_cdb(cdb, lba, chunk);

        ide_select_device(ide_packet_drive_select(dev));
        ide_bmdma_stop();
        ata_pio_write32(gIde.bmdma_base + IDE_BMDMA_PRDT_OFF, prd_addr);
        ata_pio_write8(gIde.bmdma_base + IDE_BMDMA_CMD_OFF,
                       IDE_BMDMA_CMD_READ | IDE_BMDMA_CMD_START);

        ata_pio_write8(gIde.data_base + IDE_ERR_OFF, 1);
        ata_pio_write8(gIde.data_base + IDE_NSEC_OFF, 0);
        ata_pio_write8(gIde.data_base + IDE_LBAMID_OFF, 0);
        ata_pio_write8(gIde.data_base + IDE_LBAHI_OFF, 0);
        ata_pio_write8(gIde.data_base + IDE_CMD_OFF, ATA_CMD_PACKET);

        if (!ata_pio_wait_ready(gIde.data_base + IDE_CMD_OFF)) {
            return 0;
        }

        ata_pio_write16(gIde.data_base + IDE_DATA_OFF, cdb, 6);

        if (!ide_bmdma_wait(chunk_lba, chunk)) {
            return 0;
        }

        lba += chunk;
        done += chunk;
    }

    return 1;
}

static BOOLEAN atapi_read_sectors_uncached(IDE_DEVICE *dev, UINT8 *buf,
                                           UINT32 lba, UINT32 count)
{
    ide_activate(dev);
    if (gIde.has_bmdma) {
        if (atapi_dma_read_sectors(dev, buf, lba, count)) {
            return 1;
        }
    }
    return atapi_pio_read_sectors(dev, buf, lba, count);
}

#define ATAPI_READ_CACHE_SECTORS 32U

static UINT8 mAtapiReadCache[ATAPI_READ_CACHE_SECTORS * ATAPI_SECTOR_SIZE]
    __attribute__((aligned(8)));
static IDE_DEVICE *mAtapiReadCacheDevice;
static UINT32 mAtapiReadCacheLba;
static UINT32 mAtapiReadCacheCount;
static BOOLEAN mAtapiReadCacheValid;

BOOLEAN atapi_read_sectors(IDE_DEVICE *dev, UINT8 *buf, UINT32 lba,
                                  UINT32 count)
{
    UINT32 cache_count;

    if (count != 1 || dev == NULL || !dev->present || !dev->is_atapi ||
        buf == NULL) {
        return atapi_read_sectors_uncached(dev, buf, lba, count);
    }

    if (mAtapiReadCacheValid &&
        mAtapiReadCacheDevice == dev &&
        lba >= mAtapiReadCacheLba &&
        lba - mAtapiReadCacheLba < mAtapiReadCacheCount) {
        fw_copy_mem_fast(buf,
                         mAtapiReadCache +
                         (lba - mAtapiReadCacheLba) * ATAPI_SECTOR_SIZE,
                         ATAPI_SECTOR_SIZE);
        return 1;
    }

    mAtapiReadCacheValid = 0;
    cache_count = ATAPI_READ_CACHE_SECTORS;
    if (!atapi_read_sectors_uncached(dev, mAtapiReadCache, lba,
                                     cache_count)) {
        cache_count = 1;
        if (!atapi_read_sectors_uncached(dev, mAtapiReadCache, lba,
                                         cache_count)) {
            return 0;
        }
    }

    mAtapiReadCacheDevice = dev;
    mAtapiReadCacheLba = lba;
    mAtapiReadCacheCount = cache_count;
    mAtapiReadCacheValid = 1;
    fw_copy_mem_fast(buf, mAtapiReadCache, ATAPI_SECTOR_SIZE);
    return 1;
}

