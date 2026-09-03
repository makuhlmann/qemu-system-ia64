/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * LSI53C895A SCSI + AHCI drivers and the storage abstraction layer.
 * Extracted verbatim from firmware.c (Phase 1 of
 * plans/firmware-rework-plan.md).
 */

#include "fw-base.h"
#include "fw-services.h"
#include "fw-legacy-io.h"
#include "fw-storage.h"
#include "fw-isp12160.h"

/* --- LSI53C895A SCSI Block I/O driver ----------------------------------- */

/* struct SCSI_DEVICE_STRUCT lives in fw-storage.h. */

#define SCSI_DEVICE_MAX              7U
#define SCSI_HOST_ID                 7U
#define SCSI_CDB_MAX                 16U
#define SCSI_INQUIRY_LEN             36U
#define SCSI_CAPACITY_LEN            8U
/* SCSI_BOUNCE_SIZE lives in fw-storage.h. */

#define SCSI_CMD_TEST_UNIT_READY     0x00U
#define SCSI_CMD_REQUEST_SENSE       0x03U
#define SCSI_CMD_INQUIRY             0x12U
#define SCSI_CMD_READ_CAPACITY_10    0x25U
#define SCSI_CMD_READ_10             0x28U
#define SCSI_CMD_WRITE_10            0x2aU
#define SCSI_CMD_SYNCHRONIZE_CACHE_10 0x35U

#define SCSI_TYPE_DIRECT             0x00U
#define SCSI_TYPE_CDROM              0x05U
#define SCSI_SENSE_LEN               18U
#define SCSI_SENSE_KEY_UNIT_ATTENTION 0x06U

#define PCI_SUB_CLASS_SCSI           0x00U
#define PCI_LSI_BAR1_OFFSET          0x14U

#define LSI_REG_SCID                 0x04U
#define LSI_REG_DSTAT                0x0cU
#define LSI_REG_SCNTL1               0x01U
#define LSI_REG_ISTAT0               0x14U
#define LSI_REG_DSP                  0x2cU
#define LSI_REG_DSPS                 0x30U
#define LSI_REG_SIEN0                0x40U
#define LSI_REG_SIEN1                0x41U
#define LSI_REG_SIST0                0x42U
#define LSI_REG_SIST1                0x43U
#define LSI_REG_RESPID0              0x4aU

#define LSI_ISTAT0_DIP               0x01U
#define LSI_ISTAT0_SIP               0x02U
#define LSI_ISTAT0_INTF              0x04U
#define LSI_ISTAT0_SRST              0x40U
#define LSI_ISTAT0_ABRT              0x80U
#define LSI_DSTAT_SIR                0x04U
#define LSI_SIST1_STO                0x04U
#define LSI_SCNTL1_RST               0x08U

#define LSI_PHASE_DO                 0U
#define LSI_PHASE_DI                 1U
#define LSI_PHASE_CMD                2U
#define LSI_PHASE_ST                 3U
#define LSI_PHASE_MO                 6U
#define LSI_PHASE_MI                 7U

#define LSI_SCRIPT_SELECT(Target) \
    (0x40000000U | ((UINT32)(Target) << 16) | (1U << 3))
#define LSI_SCRIPT_WAIT_RESELECT     0x50000000U
#define LSI_SCRIPT_DISCONNECT        0x48000000U
#define LSI_SCRIPT_MOVE(Phase, Count) \
    (((UINT32)(Phase) << 24) | (0x00ffffffU & (UINT32)(Count)))
#define LSI_SCRIPT_JUMP_IF_PHASE(Phase) \
    (0x80000000U | ((UINT32)(Phase) << 24) | (1U << 19) | (1U << 17))
#define LSI_SCRIPT_INTERRUPT         0x98080000U
#define LSI_SCRIPT_INTERRUPT_ERROR   1U

#define LSI_SCRIPT_DWORDS            64U

static UINT64 mLsiMmioBase;
static UINT8  mLsiPresent;
static SCSI_DEVICE mScsiDevices[SCSI_DEVICE_MAX];
SCSI_DEVICE *mBootScsiDevice;
SCSI_DEVICE *mDiskScsiDevice;
static UINT32 mLsiScript[LSI_SCRIPT_DWORDS] __attribute__((aligned(8)));
static UINT8  mLsiCdb[SCSI_CDB_MAX] __attribute__((aligned(8)));
static UINT8  mLsiMsgOut[1] __attribute__((aligned(8)));
static UINT8  mLsiMsgIn[8] __attribute__((aligned(8)));
static UINT8  mLsiStatus[1] __attribute__((aligned(8)));
static UINT8  mScsiBounce[SCSI_BOUNCE_SIZE] __attribute__((aligned(8)));

/* Which host adapter the SCSI layer is talking to, once one is found. */
#define SCSI_TRANSPORT_NONE     0U
#define SCSI_TRANSPORT_LSI      1U
#define SCSI_TRANSPORT_ISP12160 2U
static UINT8 mScsiTransport;

#define AHCI_MAX_PORTS                 6U
#define AHCI_COMMAND_LIST_ENTRIES      32U
#define AHCI_BOUNCE_SIZE               (64U * 1024U)
#define AHCI_HOST_CAP                  0x00U
#define AHCI_HOST_GHC                  0x04U
#define AHCI_HOST_IS                   0x08U
#define AHCI_HOST_PI                   0x0cU
#define AHCI_HOST_GHC_HR               (1U << 0)
#define AHCI_HOST_GHC_AE               (1U << 31)
#define AHCI_PORT_BASE(Port)           (0x100U + (Port) * 0x80U)
#define AHCI_PORT_CLB                  0x00U
#define AHCI_PORT_CLBU                 0x04U
#define AHCI_PORT_FB                   0x08U
#define AHCI_PORT_FBU                  0x0cU
#define AHCI_PORT_IS                   0x10U
#define AHCI_PORT_IE                   0x14U
#define AHCI_PORT_CMD                  0x18U
#define AHCI_PORT_TFD                  0x20U
#define AHCI_PORT_SIG                  0x24U
#define AHCI_PORT_SSTS                 0x28U
#define AHCI_PORT_SCTL                 0x2cU
#define AHCI_PORT_SERR                 0x30U
#define AHCI_PORT_SACT                 0x34U
#define AHCI_PORT_CI                   0x38U
#define AHCI_PORT_CMD_ST               (1U << 0)
#define AHCI_PORT_CMD_SUD              (1U << 1)
#define AHCI_PORT_CMD_POD              (1U << 2)
#define AHCI_PORT_CMD_FRE              (1U << 4)
#define AHCI_PORT_CMD_FR               (1U << 14)
#define AHCI_PORT_CMD_CR               (1U << 15)
#define AHCI_PORT_IS_TFES              (1U << 30)
#define AHCI_PORT_ERROR_MASK           0xfd800010U
#define AHCI_FIS_TYPE_REG_H2D          0x27U
#define AHCI_FIS_COMMAND               0x80U
#define AHCI_CMD_HEADER_ATAPI          (1U << 5)
#define AHCI_CMD_HEADER_WRITE          (1U << 6)
#define AHCI_PRDT_INTERRUPT            (1U << 31)
#define ATA_CMD_READ_DMA_EXT           0x25U
#define ATA_CMD_WRITE_DMA_EXT          0x35U
#define ATA_CMD_FLUSH_CACHE_EXT        0xeaU
#define ATA_IDENTIFY_LBA48             (1U << 10)
#define SATA_SIGNATURE_ATA             0x00000101U
#define SATA_SIGNATURE_ATAPI           0xeb140101U

typedef struct {
    UINT16 flags;
    UINT16 prdt_length;
    UINT32 transferred;
    UINT64 command_table;
    UINT32 reserved[4];
} __attribute__((packed)) AHCI_COMMAND_HEADER;

typedef struct {
    UINT64 data_base;
    UINT32 reserved;
    UINT32 byte_count;
} __attribute__((packed)) AHCI_PRDT_ENTRY;

typedef struct {
    UINT8 command_fis[64];
    UINT8 atapi_command[16];
    UINT8 reserved[48];
    AHCI_PRDT_ENTRY prdt[1];
} __attribute__((packed)) AHCI_COMMAND_TABLE;

/* struct AHCI_DEVICE_STRUCT lives in fw-storage.h. */

static UINT64 mAhciMmioBase;
static UINT32 mAhciPortsImplemented;
static BOOLEAN mAhciPresent;
static AHCI_DEVICE mAhciDevices[AHCI_MAX_PORTS];
AHCI_DEVICE *mBootAhciDevice;
AHCI_DEVICE *mDiskAhciDevice;
static AHCI_COMMAND_HEADER mAhciCommandList[AHCI_COMMAND_LIST_ENTRIES]
    __attribute__((aligned(1024)));
static UINT8 mAhciReceivedFis[256] __attribute__((aligned(256)));
static AHCI_COMMAND_TABLE mAhciCommandTable __attribute__((aligned(128)));
static UINT8 mAhciBounce[AHCI_BOUNCE_SIZE] __attribute__((aligned(8)));

FW_STATIC_ASSERT(sizeof(AHCI_COMMAND_HEADER) == 32U,
                 ahci_command_header_size);
FW_STATIC_ASSERT(sizeof(AHCI_PRDT_ENTRY) == 16U, ahci_prdt_entry_size);

/* FW_STORAGE_KIND / FW_STORAGE_DEVICE live in fw-storage.h. */
static BOOLEAN scsi_find_lsi_controller(PCI_DEVICE_LOCATION *Location);

FW_STORAGE_DEVICE mBootStorageDevice;
FW_STORAGE_DEVICE mDiskStorageDevice;
FW_STORAGE_DEVICE mRawStorageDevice;

static UINT32 fw_be32(const UINT8 *p)
{
    return ((UINT32)p[0] << 24) |
           ((UINT32)p[1] << 16) |
           ((UINT32)p[2] << 8) |
           (UINT32)p[3];
}

static void fw_write_be32(UINT8 *p, UINT32 value)
{
    p[0] = (UINT8)(value >> 24);
    p[1] = (UINT8)(value >> 16);
    p[2] = (UINT8)(value >> 8);
    p[3] = (UINT8)value;
}

static void fw_write_be16(UINT8 *p, UINT16 value)
{
    p[0] = (UINT8)(value >> 8);
    p[1] = (UINT8)value;
}

static BOOLEAN fw_addr32(const VOID *Ptr, UINT32 *Address)
{
    UINTN addr = (UINTN)Ptr;

    if (Address == NULL || (addr >> 32) != 0) {
        return 0;
    }
    *Address = (UINT32)addr;
    return 1;
}

/* Preserve every LSI MMIO register access performed by the firmware. */
static volatile UINT8 *lsi_reg(UINT32 Offset)
{
    return (volatile UINT8 *)(UINTN)(mLsiMmioBase + Offset); /* MMIO */
}

static UINT8 lsi_read8(UINT32 Offset)
{
    return *lsi_reg(Offset);
}

static void lsi_write8(UINT32 Offset, UINT8 Value)
{
    *lsi_reg(Offset) = Value;
}

static void lsi_write32(UINT32 Offset, UINT32 Value)
{
    lsi_write8(Offset, (UINT8)Value);
    lsi_write8(Offset + 1U, (UINT8)(Value >> 8));
    lsi_write8(Offset + 2U, (UINT8)(Value >> 16));
    lsi_write8(Offset + 3U, (UINT8)(Value >> 24));
}

static UINT32 lsi_read32(UINT32 Offset)
{
    return (UINT32)lsi_read8(Offset) |
           ((UINT32)lsi_read8(Offset + 1U) << 8) |
           ((UINT32)lsi_read8(Offset + 2U) << 16) |
           ((UINT32)lsi_read8(Offset + 3U) << 24);
}

static UINT32 lsi_script_addr(UINTN DwordIndex)
{
    UINT32 addr;

    if (!fw_addr32(&mLsiScript[DwordIndex], &addr)) {
        return 0;
    }
    return addr;
}

static UINTN lsi_script_emit(UINTN DwordIndex, UINT32 Insn, UINT32 Addr)
{
    if (DwordIndex + 2U > LSI_SCRIPT_DWORDS) {
        return DwordIndex;
    }
    mLsiScript[DwordIndex] = Insn;
    mLsiScript[DwordIndex + 1U] = Addr;
    return DwordIndex + 2U;
}

static BOOLEAN lsi_mmio_bar_address(UINT32 Bar, UINT64 *Address)
{
    UINT64 mmio;

    if (Address == NULL || Bar == 0 || Bar == 0xffffffffU ||
        (Bar & 1U) != 0) {
        return 0;
    }

    mmio = Bar & ~(UINT64)0x0fU;
    if (mmio < IA64_PCI_MMIO_BASE ||
        mmio >= IA64_PCI_MMIO_BASE + IA64_PCI_MMIO_SIZE) {
        return 0;
    }
    *Address = mmio;
    return 1;
}

static BOOLEAN scsi_find_lsi_controller(PCI_DEVICE_LOCATION *Location)
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
                UINT32 id;
                UINT32 class_rev;
                UINT8 base_class;
                UINT8 sub_class;

                id = (UINT32)pci_config_read_value(0, (UINT8)bus,
                                                   device, function, 0, 4);
                if ((id & 0xffffU) == 0xffffU) {
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
                if (id == 0x00121000U ||
                    (base_class == PCI_BASE_CLASS_MASS_STORAGE &&
                     sub_class == PCI_SUB_CLASS_SCSI)) {
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

static BOOLEAN lsi_init_controller(void)
{
    PCI_DEVICE_LOCATION location;
    UINT32 mmio_bar;
    UINT64 mmio_base;
    UINT16 command;

    if (mLsiPresent) {
        return 1;
    }
    if (!scsi_find_lsi_controller(&location)) {
        return 0;
    }

    mmio_bar = (UINT32)pci_config_read_value(0, location.Bus,
                                             location.Device,
                                             location.Function,
                                             PCI_LSI_BAR1_OFFSET, 4);
    if (!lsi_mmio_bar_address(mmio_bar, &mmio_base)) {
        return 0;
    }

    command = (UINT16)pci_config_read_value(0, location.Bus,
                                            location.Device,
                                            location.Function,
                                            PCI_CFG_COMMAND_OFFSET, 2);
    command |= PCI_CFG_COMMAND_IO_SPACE |
               PCI_CFG_COMMAND_MEMORY_SPACE |
               PCI_CFG_COMMAND_BUS_MASTER;
    pci_config_write_value(0, location.Bus, location.Device,
                           location.Function, PCI_CFG_COMMAND_OFFSET, 2,
                           command);

    mLsiMmioBase = mmio_base;
    mLsiPresent = 1;

    lsi_write8(LSI_REG_ISTAT0, LSI_ISTAT0_SRST);
    (void)lsi_read8(LSI_REG_DSTAT);
    (void)lsi_read8(LSI_REG_SIST0);
    (void)lsi_read8(LSI_REG_SIST1);
    lsi_write8(LSI_REG_SCID, SCSI_HOST_ID);
    lsi_write8(LSI_REG_RESPID0, (UINT8)(1U << SCSI_HOST_ID));
    lsi_write8(LSI_REG_SIEN0, 0);
    lsi_write8(LSI_REG_SIEN1, 0);
    lsi_write8(LSI_REG_ISTAT0, LSI_ISTAT0_INTF);

    return 1;
}

typedef enum {
    LsiScriptSuccess,
    LsiScriptTargetStatus,
    LsiScriptSelectionTimeout,
    LsiScriptCommandTimeout,
    LsiScriptDeviceError,
} LSI_SCRIPT_RESULT;

static LSI_SCRIPT_RESULT lsi_wait_for_script(UINT64 Timeout100ns,
                                             UINT8 *Status)
{
    UINT64 start = fw_read_itc();
    UINT64 timeout_ticks = 0;

    if (Timeout100ns != 0) {
        timeout_ticks = Timeout100ns >
            ~0ULL / FW_ITC_TICKS_PER_100NS ? ~0ULL :
            Timeout100ns * FW_ITC_TICKS_PER_100NS;
    }
    for (;;) {
        UINT8 istat = lsi_read8(LSI_REG_ISTAT0);

        if ((istat & LSI_ISTAT0_DIP) != 0) {
            UINT8 dstat = lsi_read8(LSI_REG_DSTAT);

            if ((dstat & LSI_DSTAT_SIR) == 0 ||
                lsi_read32(LSI_REG_DSPS) != 0) {
                return LsiScriptDeviceError;
            }
            if (Status != NULL) {
                *Status = mLsiStatus[0];
                if (mLsiStatus[0] != 0) {
                    return LsiScriptTargetStatus;
                }
            }
            return LsiScriptSuccess;
        }
        if ((istat & LSI_ISTAT0_SIP) != 0) {
            UINT8 sist0 = lsi_read8(LSI_REG_SIST0);
            UINT8 sist1 = lsi_read8(LSI_REG_SIST1);

            (void)sist0;
            return (sist1 & LSI_SIST1_STO) != 0 ?
                LsiScriptSelectionTimeout : LsiScriptDeviceError;
        }
        if (Timeout100ns != 0 &&
            fw_read_itc() - start >= timeout_ticks) {
            lsi_write8(LSI_REG_ISTAT0, LSI_ISTAT0_ABRT);
            (void)lsi_read8(LSI_REG_DSTAT);
            return LsiScriptCommandTimeout;
        }
    }
}

static LSI_SCRIPT_RESULT lsi_run_scsi_script_timed(
    UINT8 Target, UINT8 *Cdb, UINTN CdbLen, UINT8 *Data, UINT32 DataLen,
    UINT64 Timeout100ns, UINT8 *Status)
{
    UINTN pos = 0;
    UINTN jmp_mi_addr;
    UINTN jmp_di_addr;
    UINTN jmp_do_addr;
    UINTN jmp_st_addr;
    UINTN data_in_pos;
    UINTN data_in_status_addr;
    UINTN data_out_pos;
    UINTN data_out_status_addr;
    UINTN status_pos;
    UINTN msgin_pos;
    UINT32 script_addr;
    UINT32 cdb_addr;
    UINT32 data_addr = 0;
    UINT32 msgout_addr;
    UINT32 msgin_addr;
    UINT32 status_addr;
    if (!mLsiPresent || Cdb == NULL || CdbLen == 0 ||
        CdbLen > SCSI_CDB_MAX || Target >= SCSI_HOST_ID ||
        (DataLen != 0 && Data == NULL)) {
        return LsiScriptDeviceError;
    }
    if (!fw_addr32(Cdb, &cdb_addr) ||
        !fw_addr32(mLsiMsgOut, &msgout_addr) ||
        !fw_addr32(mLsiMsgIn, &msgin_addr) ||
        !fw_addr32(mLsiStatus, &status_addr)) {
        return LsiScriptDeviceError;
    }
    if (DataLen != 0 && !fw_addr32(Data, &data_addr)) {
        return LsiScriptDeviceError;
    }

    fw_set_mem(mLsiScript, sizeof(mLsiScript), 0);
    fw_set_mem(mLsiMsgIn, sizeof(mLsiMsgIn), 0);
    fw_set_mem(mLsiStatus, sizeof(mLsiStatus), 0xff);
    mLsiMsgOut[0] = 0x80; /* IDENTIFY, LUN 0 */

    pos = lsi_script_emit(pos, LSI_SCRIPT_SELECT(Target), 0);
    pos = lsi_script_emit(pos, LSI_SCRIPT_MOVE(LSI_PHASE_MO, 1),
                          msgout_addr);
    pos = lsi_script_emit(pos, LSI_SCRIPT_MOVE(LSI_PHASE_CMD, CdbLen),
                          cdb_addr);

    jmp_mi_addr = pos + 1U;
    pos = lsi_script_emit(pos, LSI_SCRIPT_JUMP_IF_PHASE(LSI_PHASE_MI), 0);
    jmp_di_addr = pos + 1U;
    pos = lsi_script_emit(pos, LSI_SCRIPT_JUMP_IF_PHASE(LSI_PHASE_DI), 0);
    jmp_do_addr = pos + 1U;
    pos = lsi_script_emit(pos, LSI_SCRIPT_JUMP_IF_PHASE(LSI_PHASE_DO), 0);
    jmp_st_addr = pos + 1U;
    pos = lsi_script_emit(pos, LSI_SCRIPT_JUMP_IF_PHASE(LSI_PHASE_ST), 0);
    pos = lsi_script_emit(pos, LSI_SCRIPT_INTERRUPT,
                          LSI_SCRIPT_INTERRUPT_ERROR);

    data_in_pos = pos;
    if (DataLen != 0) {
        pos = lsi_script_emit(pos, LSI_SCRIPT_MOVE(LSI_PHASE_DI, DataLen),
                              data_addr);
    } else {
        /*
         * The controller presents DATA IN while an asynchronous no-data
         * command is pending.  A zero-length move waits for completion while
         * leaving no residual byte count to turn the STATUS transition into
         * a phase-mismatch interrupt.
         */
        pos = lsi_script_emit(pos, LSI_SCRIPT_MOVE(LSI_PHASE_DI, 0),
                              status_addr);
    }
    data_in_status_addr = pos + 1U;
    pos = lsi_script_emit(pos, LSI_SCRIPT_JUMP_IF_PHASE(LSI_PHASE_ST), 0);
    pos = lsi_script_emit(pos, LSI_SCRIPT_INTERRUPT,
                          LSI_SCRIPT_INTERRUPT_ERROR);

    data_out_pos = pos;
    if (DataLen != 0) {
        pos = lsi_script_emit(pos, LSI_SCRIPT_MOVE(LSI_PHASE_DO, DataLen),
                              data_addr);
    }
    data_out_status_addr = pos + 1U;
    pos = lsi_script_emit(pos, LSI_SCRIPT_JUMP_IF_PHASE(LSI_PHASE_ST), 0);
    pos = lsi_script_emit(pos, LSI_SCRIPT_INTERRUPT,
                          LSI_SCRIPT_INTERRUPT_ERROR);

    status_pos = pos;
    pos = lsi_script_emit(pos, LSI_SCRIPT_MOVE(LSI_PHASE_ST, 1),
                          status_addr);
    pos = lsi_script_emit(pos, LSI_SCRIPT_MOVE(LSI_PHASE_MI, 1),
                          msgin_addr);
    pos = lsi_script_emit(pos, LSI_SCRIPT_DISCONNECT, 0);
    pos = lsi_script_emit(pos, LSI_SCRIPT_INTERRUPT, 0);

    msgin_pos = pos;
    /*
     * A target that defers an operation sends a one-byte DISCONNECT message,
     * then reselects and sends a one-byte IDENTIFY message.  Requesting the
     * whole message buffer in either phase makes the script stop on the
     * following bus-phase transition.
     */
    pos = lsi_script_emit(pos, LSI_SCRIPT_MOVE(LSI_PHASE_MI, 1),
                          msgin_addr);
    pos = lsi_script_emit(pos, LSI_SCRIPT_WAIT_RESELECT, 0);
    pos = lsi_script_emit(pos, LSI_SCRIPT_MOVE(LSI_PHASE_MI, 1),
                          msgin_addr);
    pos = lsi_script_emit(pos, LSI_SCRIPT_JUMP_IF_PHASE(LSI_PHASE_DI),
                          lsi_script_addr(data_in_pos));
    pos = lsi_script_emit(pos, LSI_SCRIPT_JUMP_IF_PHASE(LSI_PHASE_DO),
                          lsi_script_addr(data_out_pos));
    pos = lsi_script_emit(pos, LSI_SCRIPT_JUMP_IF_PHASE(LSI_PHASE_ST),
                          lsi_script_addr(status_pos));
    pos = lsi_script_emit(pos, LSI_SCRIPT_INTERRUPT,
                          LSI_SCRIPT_INTERRUPT_ERROR);

    mLsiScript[jmp_mi_addr] = lsi_script_addr(msgin_pos);
    mLsiScript[jmp_di_addr] = lsi_script_addr(data_in_pos);
    mLsiScript[jmp_do_addr] = lsi_script_addr(data_out_pos);
    mLsiScript[jmp_st_addr] = lsi_script_addr(status_pos);
    mLsiScript[data_in_status_addr] = lsi_script_addr(status_pos);
    mLsiScript[data_out_status_addr] = lsi_script_addr(status_pos);
    script_addr = lsi_script_addr(0);
    if (script_addr == 0) {
        return LsiScriptDeviceError;
    }

    (void)lsi_read8(LSI_REG_DSTAT);
    (void)lsi_read8(LSI_REG_SIST0);
    (void)lsi_read8(LSI_REG_SIST1);
    lsi_write8(LSI_REG_ISTAT0, LSI_ISTAT0_INTF);
    __asm__ __volatile__ ("mf" : : : "memory");
    lsi_write32(LSI_REG_DSP, script_addr);

    return lsi_wait_for_script(Timeout100ns, Status);
}

static BOOLEAN lsi_run_scsi_script(UINT8 Target, UINT8 *Cdb, UINTN CdbLen,
                                   UINT8 *Data, UINT32 DataLen,
                                   UINT8 *Status)
{
    /* Internal boot-media commands retain a finite recovery bound. */
    return lsi_run_scsi_script_timed(Target, Cdb, CdbLen, Data, DataLen,
                                     300000000ULL, Status) ==
        LsiScriptSuccess;
}

static LSI_SCRIPT_RESULT lsi_reset_scsi_target(UINT8 Target,
                                               UINT64 Timeout100ns)
{
    UINT32 script_addr;
    UINT32 msgout_addr;
    UINTN pos = 0;

    if (!mLsiPresent || Target >= SCSI_HOST_ID ||
        !fw_addr32(mLsiMsgOut, &msgout_addr)) {
        return LsiScriptDeviceError;
    }
    fw_set_mem(mLsiScript, sizeof(mLsiScript), 0);
    mLsiMsgOut[0] = 0x0cU; /* BUS DEVICE RESET message. */
    pos = lsi_script_emit(pos, LSI_SCRIPT_SELECT(Target), 0);
    pos = lsi_script_emit(pos, LSI_SCRIPT_MOVE(LSI_PHASE_MO, 1),
                          msgout_addr);
    (void)lsi_script_emit(pos, LSI_SCRIPT_INTERRUPT, 0);
    script_addr = lsi_script_addr(0);
    if (script_addr == 0) {
        return LsiScriptDeviceError;
    }

    (void)lsi_read8(LSI_REG_DSTAT);
    (void)lsi_read8(LSI_REG_SIST0);
    (void)lsi_read8(LSI_REG_SIST1);
    lsi_write8(LSI_REG_ISTAT0, LSI_ISTAT0_INTF);
    __asm__ __volatile__ ("mf" : : : "memory");
    lsi_write32(LSI_REG_DSP, script_addr);
    return lsi_wait_for_script(Timeout100ns, NULL);
}

/*
 * Run the CDB staged in mLsiCdb against a device, through whichever host
 * adapter the platform turned out to have.  The layer above -- inquiry,
 * capacity, read and write -- is the same for both, so this is the only
 * place that knows which transport is live.
 *
 * Only a write moves data to the device, and the QLogic's command IOCB has
 * to be told; the LSI's script works the direction out for itself.
 */
static BOOLEAN scsi_cdb_to_device(const UINT8 *Cdb)
{
    return Cdb[0] == SCSI_CMD_WRITE_10;
}

static BOOLEAN lsi_scsi_command_prepared(SCSI_DEVICE *Dev, UINTN CdbLen,
                                         UINT8 *Data, UINT32 DataLen)
{
    UINT8 status = 0xff;

    if (Dev == NULL || !Dev->present ||
        CdbLen == 0 || CdbLen > sizeof(mLsiCdb)) {
        return 0;
    }

    if (mScsiTransport == SCSI_TRANSPORT_ISP12160) {
        return isp12160_command(Dev->target, mLsiCdb, CdbLen, Data, DataLen,
                                scsi_cdb_to_device(mLsiCdb), &status) &&
               status == 0;
    }
    return lsi_run_scsi_script(Dev->target, mLsiCdb, CdbLen, Data, DataLen,
                               &status);
}

static BOOLEAN scsi_inquiry(SCSI_DEVICE *Dev, UINT8 *Buffer, UINT32 Length)
{
    fw_set_mem(mLsiCdb, sizeof(mLsiCdb), 0);
    mLsiCdb[0] = SCSI_CMD_INQUIRY;
    mLsiCdb[4] = (UINT8)Length;
    return lsi_scsi_command_prepared(Dev, 6, Buffer, Length);
}

static BOOLEAN scsi_read_capacity(SCSI_DEVICE *Dev)
{
    UINT8 *buf = mScsiBounce;
    UINT32 last_lba;
    UINT32 block_size;

    fw_set_mem(mLsiCdb, sizeof(mLsiCdb), 0);
    mLsiCdb[0] = SCSI_CMD_READ_CAPACITY_10;
    fw_set_mem(buf, SCSI_CAPACITY_LEN, 0);
    if (!lsi_scsi_command_prepared(Dev, 10, buf, SCSI_CAPACITY_LEN)) {
        return 0;
    }

    last_lba = fw_be32(buf);
    block_size = fw_be32(buf + 4);
    if (block_size == 0) {
        return 0;
    }
    Dev->last_lba = last_lba;
    Dev->block_size = block_size;
    Dev->media_present = 1;
    return 1;
}

static BOOLEAN scsi_test_unit_ready(SCSI_DEVICE *Dev)
{
    fw_set_mem(mLsiCdb, sizeof(mLsiCdb), 0);
    mLsiCdb[0] = SCSI_CMD_TEST_UNIT_READY;
    return lsi_scsi_command_prepared(Dev, 6, NULL, 0);
}

static BOOLEAN scsi_request_sense(SCSI_DEVICE *Dev, UINT8 *Sense)
{
    if (Sense == NULL) {
        return 0;
    }
    fw_set_mem(mLsiCdb, sizeof(mLsiCdb), 0);
    mLsiCdb[0] = SCSI_CMD_REQUEST_SENSE;
    mLsiCdb[4] = SCSI_SENSE_LEN;
    fw_set_mem(Sense, SCSI_SENSE_LEN, 0);
    return lsi_scsi_command_prepared(Dev, 6, Sense, SCSI_SENSE_LEN);
}

static VOID scsi_refresh_media(SCSI_DEVICE *Dev)
{
    UINT8 *sense = mScsiBounce;

    if (Dev == NULL || !Dev->present) {
        return;
    }
    Dev->media_present = 0;
    Dev->block_size = Dev->is_cd ? ATAPI_SECTOR_SIZE : 512U;
    Dev->last_lba = 0;

    if (!scsi_test_unit_ready(Dev)) {
        if (scsi_request_sense(Dev, sense) &&
            (sense[2] & 0x0fU) == SCSI_SENSE_KEY_UNIT_ATTENTION) {
            /* REQUEST SENSE clears unit attention; retry readiness once. */
            (void)scsi_test_unit_ready(Dev);
        }
    }
    if (!scsi_read_capacity(Dev)) {
        (void)scsi_request_sense(Dev, sense);
        Dev->media_present = 0;
        Dev->block_size = Dev->is_cd ? ATAPI_SECTOR_SIZE : 512U;
        Dev->last_lba = 0;
    }
}

static BOOLEAN scsi_read_blocks(SCSI_DEVICE *Dev, UINT8 *Buffer,
                                UINT32 Lba, UINT32 Count);

static BOOLEAN scsi_reset_device(SCSI_DEVICE *Dev,
                                 BOOLEAN ExtendedVerification)
{
    UINT8 *inquiry = mScsiBounce;
    UINT8 type;

    if (Dev == NULL || !Dev->present ||
        lsi_reset_scsi_target(Dev->target, 300000000ULL) !=
            LsiScriptSuccess) {
        return 0;
    }
    (void)bs_stall(1000U);
    fw_set_mem(inquiry, SCSI_INQUIRY_LEN, 0);
    if (!scsi_inquiry(Dev, inquiry, SCSI_INQUIRY_LEN)) {
        return 0;
    }
    type = inquiry[0] & 0x1fU;
    if (type == 0x1fU) {
        return 0;
    }
    Dev->is_cd = type == SCSI_TYPE_CDROM;
    Dev->removable = (inquiry[1] & 0x80U) != 0;
    Dev->read_only = Dev->is_cd;
    scsi_refresh_media(Dev);
    if (ExtendedVerification && Dev->media_present) {
        return scsi_read_blocks(Dev, mScsiBounce, 0, 1);
    }
    return 1;
}

static BOOLEAN scsi_read_blocks(SCSI_DEVICE *Dev, UINT8 *Buffer,
                                UINT32 Lba, UINT32 Count)
{
    UINT32 byte_count;

    if (Dev == NULL || !Dev->present || !Dev->media_present ||
        Buffer == NULL || Count == 0 || Count > 0xffffU ||
        Dev->block_size == 0) {
        return Count == 0;
    }

    byte_count = Dev->block_size * Count;
    if (byte_count / Dev->block_size != Count) {
        return 0;
    }

    fw_set_mem(mLsiCdb, sizeof(mLsiCdb), 0);
    mLsiCdb[0] = SCSI_CMD_READ_10;
    fw_write_be32(mLsiCdb + 2, Lba);
    fw_write_be16(mLsiCdb + 7, (UINT16)Count);
    return lsi_scsi_command_prepared(Dev, 10, Buffer, byte_count);
}

static BOOLEAN scsi_write_blocks(SCSI_DEVICE *Dev, const UINT8 *Buffer,
                                 UINT32 Lba, UINT32 Count)
{
    UINT32 byte_count;

    if (Dev == NULL || !Dev->present || !Dev->media_present ||
        Dev->read_only || Buffer == NULL || Count == 0 ||
        Count > 0xffffU || Dev->block_size == 0) {
        return Count == 0;
    }

    byte_count = Dev->block_size * Count;
    if (byte_count / Dev->block_size != Count ||
        byte_count > sizeof(mScsiBounce)) {
        return 0;
    }

    fw_copy_mem(mScsiBounce, Buffer, byte_count);
    fw_set_mem(mLsiCdb, sizeof(mLsiCdb), 0);
    mLsiCdb[0] = SCSI_CMD_WRITE_10;
    fw_write_be32(mLsiCdb + 2, Lba);
    fw_write_be16(mLsiCdb + 7, (UINT16)Count);
    return lsi_scsi_command_prepared(Dev, 10, mScsiBounce, byte_count);
}

const CHAR8 *scsi_transport_name(void)
{
    switch (mScsiTransport) {
    case SCSI_TRANSPORT_LSI:
        return "LSI53C895A";
    case SCSI_TRANSPORT_ISP12160:
        return "ISP12160";
    default:
        return "none";
    }
}

static void scsi_probe_transport(void)
{
    UINTN target;

    uart_puts("SCSI controller:      ");
    uart_puts(scsi_transport_name());
    uart_puts(" mmio=0x");
    uart_put_hex64(mScsiTransport == SCSI_TRANSPORT_ISP12160 ?
                   isp12160_mmio_base() : mLsiMmioBase);
    uart_puts("\r\n");

    for (target = 0; target < SCSI_DEVICE_MAX; target++) {
        SCSI_DEVICE *dev = &mScsiDevices[target];
        UINT8 *inquiry = mScsiBounce;
        UINT8 type;

        if (target == SCSI_HOST_ID) {
            continue;
        }

        fw_set_mem(dev, sizeof(*dev), 0);
        dev->target = (UINT8)target;
        dev->lun = 0;
        fw_set_mem(inquiry, SCSI_INQUIRY_LEN, 0);
        dev->present = 1;
        if (!scsi_inquiry(dev, inquiry, SCSI_INQUIRY_LEN)) {
            dev->present = 0;
            continue;
        }

        type = inquiry[0] & 0x1fU;
        if (type == 0x1fU) {
            dev->present = 0;
            continue;
        }

        dev->is_cd = type == SCSI_TYPE_CDROM;
        dev->removable = (inquiry[1] & 0x80U) != 0;
        dev->read_only = dev->is_cd;
        scsi_refresh_media(dev);

        uart_puts("SCSI device:          target ");
        uart_put_hex64(target);
        uart_puts(dev->is_cd ? " CD-ROM" : " disk");
        uart_puts(dev->media_present ? " media\r\n" : " no media\r\n");

        if (dev->is_cd &&
            (mBootScsiDevice == NULL ||
             (!mBootScsiDevice->media_present && dev->media_present))) {
            mBootScsiDevice = dev;
        }
        if (!dev->is_cd &&
            (mDiskScsiDevice == NULL ||
             (!mDiskScsiDevice->media_present && dev->media_present))) {
            mDiskScsiDevice = dev;
        }
    }
}

/*
 * Probe the LSI first, so a platform that has one behaves exactly as it
 * always has, then the QLogic the i2000 actually carries.  While guests are
 * being migrated from one to the other both adapters are present with the
 * disk on only one, so an adapter that answers but carries no device must
 * not end the search.
 */
void scsi_probe_devices(void)
{
    fw_set_mem(mScsiDevices, sizeof(mScsiDevices), 0);
    mBootScsiDevice = NULL;
    mDiskScsiDevice = NULL;
    mScsiTransport = SCSI_TRANSPORT_NONE;

    if (lsi_init_controller()) {
        mScsiTransport = SCSI_TRANSPORT_LSI;
        scsi_probe_transport();
    }
    if (mBootScsiDevice != NULL || mDiskScsiDevice != NULL) {
        return;
    }

    if (isp12160_initialise()) {
        mScsiTransport = SCSI_TRANSPORT_ISP12160;
        fw_set_mem(mScsiDevices, sizeof(mScsiDevices), 0);
        scsi_probe_transport();
        if (mBootScsiDevice == NULL && mDiskScsiDevice == NULL) {
            mScsiTransport = SCSI_TRANSPORT_NONE;
        }
    }

}

static volatile UINT32 *ahci_reg(UINT32 Offset)
{
    return (volatile UINT32 *)(UINTN)(mAhciMmioBase + Offset);
}

static UINT32 ahci_read32(UINT32 Offset)
{
    return *ahci_reg(Offset);
}

static void ahci_write32(UINT32 Offset, UINT32 Value)
{
    *ahci_reg(Offset) = Value;
}

static UINT32 ahci_port_read32(UINT8 Port, UINT32 Offset)
{
    return ahci_read32(AHCI_PORT_BASE(Port) + Offset);
}

static void ahci_port_write32(UINT8 Port, UINT32 Offset, UINT32 Value)
{
    ahci_write32(AHCI_PORT_BASE(Port) + Offset, Value);
}

static BOOLEAN ahci_wait_clear(UINT32 Offset, UINT32 Mask, UINTN Timeout)
{
    while (Timeout-- != 0) {
        if ((ahci_read32(Offset) & Mask) == 0) {
            return 1;
        }
    }
    return 0;
}

static BOOLEAN ahci_find_controller(PCI_DEVICE_LOCATION *Location)
{
    UINT16 bus;
    UINT8 device;
    UINT8 function;

    if (Location == NULL) {
        return 0;
    }
    for (bus = 0; bus < PCI_MAX_BUSES; bus++) {
        for (device = 0; device < PCI_MAX_DEVICES; device++) {
            for (function = 0; function < PCI_MAX_FUNCTIONS; function++) {
                UINT32 id = (UINT32)pci_config_read_value(
                    0, (UINT8)bus, device, function, 0, 4);
                UINT32 class_revision;

                if ((id & 0xffffU) == 0xffffU) {
                    if (function == 0) {
                        break;
                    }
                    continue;
                }
                class_revision = (UINT32)pci_config_read_value(
                    0, (UINT8)bus, device, function,
                    PCI_CLASS_REVISION_OFFSET, 4);
                if (id == 0x29228086U ||
                    (class_revision & 0xffff0000U) == 0x01060000U) {
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

static BOOLEAN ahci_init_controller(void)
{
    PCI_DEVICE_LOCATION location;
    UINT32 bar;
    UINT16 command;
    UINTN timeout;

    if (mAhciPresent) {
        return 1;
    }
    if (!ahci_find_controller(&location)) {
        return 0;
    }
    bar = (UINT32)pci_config_read_value(0, location.Bus, location.Device,
                                        location.Function, 0x24U, 4);
    bar &= ~0x0fU;
    if (bar < IA64_PCI_MMIO_BASE ||
        bar > IA64_PCI_MMIO_BASE + IA64_PCI_MMIO_SIZE - 0x1000U) {
        return 0;
    }
    command = (UINT16)pci_config_read_value(
        0, location.Bus, location.Device, location.Function,
        PCI_CFG_COMMAND_OFFSET, 2);
    command |= PCI_CFG_COMMAND_MEMORY_SPACE | PCI_CFG_COMMAND_BUS_MASTER;
    pci_config_write_value(0, location.Bus, location.Device,
                           location.Function, PCI_CFG_COMMAND_OFFSET, 2,
                           command);

    mAhciMmioBase = bar;
    ahci_write32(AHCI_HOST_GHC, AHCI_HOST_GHC_AE | AHCI_HOST_GHC_HR);
    timeout = 10000000U;
    while (timeout-- != 0 &&
           (ahci_read32(AHCI_HOST_GHC) & AHCI_HOST_GHC_HR) != 0) {
    }
    if ((ahci_read32(AHCI_HOST_GHC) & AHCI_HOST_GHC_HR) != 0) {
        mAhciMmioBase = 0;
        return 0;
    }
    ahci_write32(AHCI_HOST_GHC,
                 ahci_read32(AHCI_HOST_GHC) | AHCI_HOST_GHC_AE);
    mAhciPortsImplemented = ahci_read32(AHCI_HOST_PI);
    mAhciPresent = mAhciPortsImplemented != 0;
    if (!mAhciPresent) {
        mAhciMmioBase = 0;
        return 0;
    }

    uart_puts("AHCI controller:       mmio=0x");
    uart_put_hex64(mAhciMmioBase);
    uart_puts(" ports=0x");
    uart_put_hex64(mAhciPortsImplemented);
    uart_puts("\r\n");
    return 1;
}

static BOOLEAN ahci_port_stop(UINT8 Port)
{
    UINT32 command;
    UINT32 offset = AHCI_PORT_BASE(Port) + AHCI_PORT_CMD;

    command = ahci_read32(offset);
    command &= ~AHCI_PORT_CMD_ST;
    ahci_write32(offset, command);
    if (!ahci_wait_clear(offset, AHCI_PORT_CMD_CR, 10000000U)) {
        return 0;
    }
    command = ahci_read32(offset) & ~AHCI_PORT_CMD_FRE;
    ahci_write32(offset, command);
    return ahci_wait_clear(offset, AHCI_PORT_CMD_FR, 10000000U);
}

static BOOLEAN ahci_port_configure(UINT8 Port)
{
    UINT64 command_list = (UINT64)(UINTN)mAhciCommandList;
    UINT64 received_fis = (UINT64)(UINTN)mAhciReceivedFis;
    UINT32 command;

    if (!mAhciPresent || Port >= AHCI_MAX_PORTS ||
        (mAhciPortsImplemented & (1U << Port)) == 0 ||
        !ahci_port_stop(Port)) {
        return 0;
    }
    fw_set_mem(mAhciCommandList, sizeof(mAhciCommandList), 0);
    fw_set_mem(mAhciReceivedFis, sizeof(mAhciReceivedFis), 0);
    ahci_port_write32(Port, AHCI_PORT_CLB, (UINT32)command_list);
    ahci_port_write32(Port, AHCI_PORT_CLBU, (UINT32)(command_list >> 32));
    ahci_port_write32(Port, AHCI_PORT_FB, (UINT32)received_fis);
    ahci_port_write32(Port, AHCI_PORT_FBU, (UINT32)(received_fis >> 32));
    ahci_port_write32(Port, AHCI_PORT_IE, 0);
    ahci_port_write32(Port, AHCI_PORT_IS, 0xffffffffU);
    ahci_port_write32(Port, AHCI_PORT_SERR, 0xffffffffU);
    command = ahci_port_read32(Port, AHCI_PORT_CMD);
    command |= AHCI_PORT_CMD_FRE | AHCI_PORT_CMD_POD | AHCI_PORT_CMD_SUD;
    ahci_port_write32(Port, AHCI_PORT_CMD, command);
    command |= AHCI_PORT_CMD_ST;
    ahci_port_write32(Port, AHCI_PORT_CMD, command);
    return 1;
}

static BOOLEAN ahci_port_connected(UINT8 Port)
{
    UINT32 status = ahci_port_read32(Port, AHCI_PORT_SSTS);

    return (status & 0x0fU) == 3U && ((status >> 8) & 0x0fU) == 1U;
}

static void ahci_build_command_fis(UINT8 Command, UINT64 Lba, UINT16 Count,
                                   UINT8 Feature)
{
    UINT8 *fis = mAhciCommandTable.command_fis;

    fis[0] = AHCI_FIS_TYPE_REG_H2D;
    fis[1] = AHCI_FIS_COMMAND;
    fis[2] = Command;
    fis[3] = Feature;
    fis[4] = (UINT8)Lba;
    fis[5] = (UINT8)(Lba >> 8);
    fis[6] = (UINT8)(Lba >> 16);
    fis[7] = 0x40U;
    if (Command == ATA_CMD_READ_DMA || Command == ATA_CMD_WRITE_DMA) {
        fis[7] |= (UINT8)((Lba >> 24) & 0x0fU);
    }
    fis[8] = (UINT8)(Lba >> 24);
    fis[9] = (UINT8)(Lba >> 32);
    fis[10] = (UINT8)(Lba >> 40);
    fis[12] = (UINT8)Count;
    fis[13] = (UINT8)(Count >> 8);
}

static BOOLEAN ahci_issue_command(AHCI_DEVICE *Device, UINT8 Command,
                                  UINT64 Lba, UINT16 Count,
                                  const UINT8 *AtapiCommand,
                                  UINTN AtapiCommandSize,
                                  VOID *Data, UINT32 DataSize,
                                  BOOLEAN Write)
{
    AHCI_COMMAND_HEADER *header = &mAhciCommandList[0];
    UINT64 table_address = (UINT64)(UINTN)&mAhciCommandTable;
    UINT64 data_address = (UINT64)(UINTN)Data;
    UINTN timeout;
    UINT32 interrupt_status;
    UINT16 flags = 5U;

    if (Device == NULL || !Device->present || Device->port >= AHCI_MAX_PORTS ||
        AtapiCommandSize > sizeof(mAhciCommandTable.atapi_command) ||
        (DataSize != 0 && Data == NULL) ||
        (DataSize != 0 && DataSize > 0x400000U)) {
        return 0;
    }
    if (!ahci_port_configure(Device->port)) {
        return 0;
    }
    timeout = 10000000U;
    while (timeout-- != 0 &&
           (ahci_port_read32(Device->port, AHCI_PORT_TFD) &
            (ATA_SR_BSY | ATA_SR_DRQ)) != 0) {
    }
    if ((ahci_port_read32(Device->port, AHCI_PORT_TFD) &
         (ATA_SR_BSY | ATA_SR_DRQ)) != 0) {
        return 0;
    }

    fw_set_mem(header, sizeof(*header), 0);
    fw_set_mem(&mAhciCommandTable, sizeof(mAhciCommandTable), 0);
    if (AtapiCommand != NULL) {
        flags |= AHCI_CMD_HEADER_ATAPI;
        fw_copy_mem(mAhciCommandTable.atapi_command, AtapiCommand,
                    AtapiCommandSize);
    }
    if (Write) {
        flags |= AHCI_CMD_HEADER_WRITE;
    }
    header->flags = flags;
    header->prdt_length = DataSize != 0 ? 1U : 0U;
    header->command_table = table_address;
    if (DataSize != 0) {
        mAhciCommandTable.prdt[0].data_base = data_address;
        mAhciCommandTable.prdt[0].byte_count =
            (DataSize - 1U) | AHCI_PRDT_INTERRUPT;
    }
    ahci_build_command_fis(Command, Lba, Count,
                           AtapiCommand != NULL ? 1U : 0U);

    ahci_port_write32(Device->port, AHCI_PORT_IS, 0xffffffffU);
    ahci_port_write32(Device->port, AHCI_PORT_SERR, 0xffffffffU);
    __asm__ __volatile__("mf" ::: "memory");
    ahci_port_write32(Device->port, AHCI_PORT_CI, 1U);
    timeout = 20000000U;
    while (timeout-- != 0 &&
           (ahci_port_read32(Device->port, AHCI_PORT_CI) & 1U) != 0) {
    }
    __asm__ __volatile__("mf" ::: "memory");
    interrupt_status = ahci_port_read32(Device->port, AHCI_PORT_IS);
    if ((ahci_port_read32(Device->port, AHCI_PORT_CI) & 1U) != 0 ||
        (interrupt_status & AHCI_PORT_ERROR_MASK) != 0 ||
        (ahci_port_read32(Device->port, AHCI_PORT_TFD) &
         (ATA_SR_ERR | ATA_SR_DF)) != 0) {
        ahci_port_write32(Device->port, AHCI_PORT_IS, interrupt_status);
        return 0;
    }
    ahci_port_write32(Device->port, AHCI_PORT_IS, interrupt_status);
    ahci_write32(AHCI_HOST_IS, 1U << Device->port);
    return header->transferred == DataSize || DataSize == 0;
}

static BOOLEAN ahci_identify_device(AHCI_DEVICE *Device)
{
    UINT16 *identify = (UINT16 *)(VOID *)mAhciBounce;
    UINT8 command;
    UINT64 sectors;

    fw_set_mem(mAhciBounce, 512U, 0);
    command = Device->is_atapi ? ATA_CMD_IDENTIFY_PACKET : ATA_CMD_IDENTIFY;
    if (!ahci_issue_command(Device, command, 0, 0, NULL, 0,
                            mAhciBounce, 512U, 0)) {
        return 0;
    }
    Device->removable = (identify[0] & 0x0080U) != 0;
    if (Device->is_atapi) {
        UINT8 capacity_command[12];

        fw_set_mem(capacity_command, sizeof(capacity_command), 0);
        capacity_command[0] = SCSI_CMD_READ_CAPACITY_10;
        fw_set_mem(mAhciBounce, SCSI_CAPACITY_LEN, 0);
        Device->block_size = ATAPI_SECTOR_SIZE;
        Device->read_only = 1;
        if (!ahci_issue_command(Device, ATA_CMD_PACKET, 0, 0,
                                capacity_command, sizeof(capacity_command),
                                mAhciBounce, SCSI_CAPACITY_LEN, 0)) {
            Device->media_present = 0;
            Device->last_lba = 0;
            return 1;
        }
        Device->last_lba = fw_be32(mAhciBounce);
        Device->block_size = fw_be32(mAhciBounce + 4U);
        Device->media_present = Device->block_size != 0;
        return Device->media_present;
    }

    Device->lba48 = (identify[83] & ATA_IDENTIFY_LBA48) != 0;
    if (Device->lba48) {
        sectors = (UINT64)identify[100] |
                  ((UINT64)identify[101] << 16) |
                  ((UINT64)identify[102] << 32) |
                  ((UINT64)identify[103] << 48);
    } else {
        sectors = (UINT64)identify[60] | ((UINT64)identify[61] << 16);
    }
    if (sectors == 0) {
        return 0;
    }
    Device->block_size = 512U;
    if ((identify[106] & 0xd000U) == 0x5000U) {
        UINT32 logical_words = (UINT32)identify[117] |
                               ((UINT32)identify[118] << 16);
        UINT32 logical_bytes = logical_words * 2U;

        if (logical_words != 0 && logical_bytes / 2U == logical_words &&
            logical_bytes >= 512U && logical_bytes <= 4096U &&
            (logical_bytes & (logical_bytes - 1U)) == 0) {
            Device->block_size = logical_bytes;
        }
    }
    Device->last_lba = sectors - 1U;
    Device->media_present = 1;
    Device->read_only = 0;
    return 1;
}

void ahci_probe_devices(void)
{
    UINTN port;

    fw_set_mem(mAhciDevices, sizeof(mAhciDevices), 0);
    mBootAhciDevice = NULL;
    mDiskAhciDevice = NULL;
    if (!ahci_init_controller()) {
        return;
    }
    for (port = 0; port < AHCI_MAX_PORTS; port++) {
        AHCI_DEVICE *device = &mAhciDevices[port];
        UINT32 signature;

        if ((mAhciPortsImplemented & (1U << port)) == 0 ||
            !ahci_port_connected((UINT8)port) ||
            !ahci_port_configure((UINT8)port)) {
            continue;
        }
        signature = ahci_port_read32((UINT8)port, AHCI_PORT_SIG);
        if (signature != SATA_SIGNATURE_ATA &&
            signature != SATA_SIGNATURE_ATAPI) {
            continue;
        }
        device->port = (UINT8)port;
        device->present = 1;
        device->is_atapi = signature == SATA_SIGNATURE_ATAPI;
        if (!ahci_identify_device(device)) {
            fw_set_mem(device, sizeof(*device), 0);
            continue;
        }
        uart_puts("AHCI device:           port ");
        uart_put_hex64(port);
        uart_puts(device->is_atapi ? " ATAPI" : " SATA disk");
        uart_puts(device->media_present ? " media\r\n" : " no media\r\n");
        if (device->is_atapi &&
            (mBootAhciDevice == NULL ||
             (!mBootAhciDevice->media_present && device->media_present))) {
            mBootAhciDevice = device;
        }
        if (!device->is_atapi &&
            (mDiskAhciDevice == NULL ||
             (!mDiskAhciDevice->media_present && device->media_present))) {
            mDiskAhciDevice = device;
        }
    }
}

static BOOLEAN ahci_read_blocks(AHCI_DEVICE *Device, UINT8 *Buffer,
                                UINT64 Lba, UINT32 Count)
{
    UINT32 done = 0;
    UINT32 max_blocks;

    if (Device == NULL || !Device->present || !Device->media_present ||
        Buffer == NULL || Device->block_size == 0) {
        return Count == 0;
    }
    max_blocks = sizeof(mAhciBounce) / Device->block_size;
    if (max_blocks > 0xffffU) {
        max_blocks = 0xffffU;
    }
    if (!Device->is_atapi && !Device->lba48 && max_blocks > 255U) {
        max_blocks = 255U;
    }
    while (done < Count) {
        UINT32 chunk = Count - done;
        BOOLEAN ok;

        if (chunk > max_blocks) {
            chunk = max_blocks;
        }
        if (Device->is_atapi) {
            UINT8 cdb[12];

            if (Lba + done > 0xffffffffULL) {
                return 0;
            }
            fw_set_mem(cdb, sizeof(cdb), 0);
            cdb[0] = SCSI_CMD_READ_10;
            fw_write_be32(cdb + 2, (UINT32)(Lba + done));
            fw_write_be16(cdb + 7, (UINT16)chunk);
            ok = ahci_issue_command(Device, ATA_CMD_PACKET, 0, 0,
                                    cdb, sizeof(cdb), mAhciBounce,
                                    chunk * Device->block_size, 0);
        } else {
            ok = ahci_issue_command(
                Device, Device->lba48 ? ATA_CMD_READ_DMA_EXT : ATA_CMD_READ_DMA,
                Lba + done, (UINT16)chunk, NULL, 0, mAhciBounce,
                chunk * Device->block_size, 0);
        }
        if (!ok) {
            return 0;
        }
        fw_copy_mem(Buffer + (UINTN)done * Device->block_size,
                    mAhciBounce, chunk * Device->block_size);
        done += chunk;
    }
    return 1;
}

static BOOLEAN ahci_write_blocks(AHCI_DEVICE *Device, const UINT8 *Buffer,
                                 UINT64 Lba, UINT32 Count)
{
    UINT32 done = 0;
    UINT32 max_blocks;

    if (Device == NULL || !Device->present || !Device->media_present ||
        Device->read_only || Device->is_atapi || Buffer == NULL ||
        Device->block_size == 0) {
        return Count == 0;
    }
    max_blocks = sizeof(mAhciBounce) / Device->block_size;
    if (!Device->lba48 && max_blocks > 255U) {
        max_blocks = 255U;
    }
    while (done < Count) {
        UINT32 chunk = Count - done;

        if (chunk > max_blocks) {
            chunk = max_blocks;
        }
        fw_copy_mem(mAhciBounce,
                    Buffer + (UINTN)done * Device->block_size,
                    chunk * Device->block_size);
        if (!ahci_issue_command(
                Device,
                Device->lba48 ? ATA_CMD_WRITE_DMA_EXT : ATA_CMD_WRITE_DMA,
                Lba + done, (UINT16)chunk, NULL, 0, mAhciBounce,
                chunk * Device->block_size, 1)) {
            return 0;
        }
        done += chunk;
    }
    return 1;
}

static BOOLEAN ahci_flush(AHCI_DEVICE *Device)
{
    if (Device == NULL || !Device->present || !Device->media_present) {
        return 0;
    }
    if (Device->read_only || Device->is_atapi) {
        return 1;
    }
    return ahci_issue_command(
        Device, Device->lba48 ? ATA_CMD_FLUSH_CACHE_EXT : ATA_CMD_FLUSH_CACHE,
        0, 0, NULL, 0, NULL, 0, 0);
}

static BOOLEAN ahci_reset_device(AHCI_DEVICE *Device,
                                 BOOLEAN ExtendedVerification)
{
    UINT8 port;
    UINT32 control;
    UINTN timeout;

    if (Device == NULL || !Device->present) {
        return 0;
    }
    port = Device->port;
    if (!ahci_port_stop(port)) {
        return 0;
    }
    control = ahci_port_read32(port, AHCI_PORT_SCTL) & ~0x0fU;
    ahci_port_write32(port, AHCI_PORT_SCTL, control | 1U);
    (void)bs_stall(1000U);
    ahci_port_write32(port, AHCI_PORT_SCTL, control);
    timeout = 10000U;
    while (timeout-- != 0 && !ahci_port_connected(port)) {
        (void)bs_stall(100U);
    }
    if (!ahci_port_connected(port) || !ahci_port_configure(port) ||
        !ahci_identify_device(Device)) {
        return 0;
    }
    if (ExtendedVerification && Device->media_present) {
        return ahci_read_blocks(Device, mAhciBounce, 0, 1);
    }
    return 1;
}

void ahci_stop_all_ports(void)
{
    UINTN port;

    if (!mAhciPresent) {
        return;
    }
    for (port = 0; port < AHCI_MAX_PORTS; port++) {
        if ((mAhciPortsImplemented & (1U << port)) != 0) {
            (void)ahci_port_stop((UINT8)port);
        }
    }
    ahci_write32(AHCI_HOST_IS, 0xffffffffU);
}

void storage_set_none(FW_STORAGE_DEVICE *Device)
{
    if (Device != NULL) {
        fw_set_mem(Device, sizeof(*Device), 0);
        Device->Kind = FW_STORAGE_NONE;
    }
}

void storage_set_ide(FW_STORAGE_DEVICE *Device, IDE_DEVICE *Ide)
{
    storage_set_none(Device);
    if (Device != NULL && Ide != NULL && Ide->present) {
        Device->Kind = FW_STORAGE_IDE;
        Device->Ide = Ide;
    }
}

void storage_set_ahci(FW_STORAGE_DEVICE *Device, AHCI_DEVICE *Ahci)
{
    storage_set_none(Device);
    if (Device != NULL && Ahci != NULL && Ahci->present) {
        Device->Kind = FW_STORAGE_AHCI;
        Device->Ahci = Ahci;
    }
}

void storage_set_scsi(FW_STORAGE_DEVICE *Device, SCSI_DEVICE *Scsi)
{
    storage_set_none(Device);
    if (Device != NULL && Scsi != NULL && Scsi->present) {
        Device->Kind = FW_STORAGE_SCSI;
        Device->Scsi = Scsi;
    }
}

BOOLEAN storage_device_present(const FW_STORAGE_DEVICE *Device)
{
    if (Device == NULL) {
        return 0;
    }
    if (Device->Kind == FW_STORAGE_IDE) {
        return Device->Ide != NULL && Device->Ide->present;
    }
    if (Device->Kind == FW_STORAGE_AHCI) {
        return Device->Ahci != NULL && Device->Ahci->present;
    }
    if (Device->Kind == FW_STORAGE_SCSI) {
        return Device->Scsi != NULL && Device->Scsi->present;
    }
    return 0;
}

BOOLEAN storage_present(const FW_STORAGE_DEVICE *Device)
{
    if (!storage_device_present(Device)) {
        return 0;
    }
    if (Device->Kind == FW_STORAGE_IDE) {
        return Device->Ide->media_present;
    }
    if (Device->Kind == FW_STORAGE_AHCI) {
        return Device->Ahci->media_present;
    }
    return Device->Scsi->media_present;
}

BOOLEAN storage_same_device(const FW_STORAGE_DEVICE *Left,
                                   const FW_STORAGE_DEVICE *Right)
{
    if (!storage_device_present(Left) || !storage_device_present(Right) ||
        Left->Kind != Right->Kind) {
        return 0;
    }
    if (Left->Kind == FW_STORAGE_IDE) {
        return Left->Ide == Right->Ide;
    }
    if (Left->Kind == FW_STORAGE_AHCI) {
        return Left->Ahci == Right->Ahci;
    }
    return Left->Scsi == Right->Scsi;
}

BOOLEAN storage_is_cd(const FW_STORAGE_DEVICE *Device)
{
    if (!storage_device_present(Device)) {
        return 0;
    }
    if (Device->Kind == FW_STORAGE_IDE) {
        return Device->Ide->is_atapi;
    }
    if (Device->Kind == FW_STORAGE_AHCI) {
        return Device->Ahci->is_atapi;
    }
    return Device->Scsi->is_cd;
}

BOOLEAN storage_read_only(const FW_STORAGE_DEVICE *Device)
{
    if (!storage_device_present(Device)) {
        return 1;
    }
    if (Device->Kind == FW_STORAGE_IDE) {
        return Device->Ide->is_atapi;
    }
    if (Device->Kind == FW_STORAGE_AHCI) {
        return Device->Ahci->read_only;
    }
    return Device->Scsi->read_only;
}

BOOLEAN storage_removable(const FW_STORAGE_DEVICE *Device)
{
    if (!storage_device_present(Device)) {
        return 0;
    }
    if (Device->Kind == FW_STORAGE_IDE) {
        return Device->Ide->is_atapi;
    }
    if (Device->Kind == FW_STORAGE_AHCI) {
        return Device->Ahci->removable || Device->Ahci->is_atapi;
    }
    return Device->Scsi->removable;
}

BOOLEAN storage_write_caching(const FW_STORAGE_DEVICE *Device)
{
    return storage_present(Device) && !storage_read_only(Device) &&
           !storage_is_cd(Device);
}

UINT32 storage_block_size(const FW_STORAGE_DEVICE *Device)
{
    if (!storage_device_present(Device)) {
        return 512U;
    }
    if (Device->Kind == FW_STORAGE_IDE) {
        return Device->Ide->is_atapi ? ATAPI_SECTOR_SIZE : 512U;
    }
    if (Device->Kind == FW_STORAGE_AHCI) {
        return Device->Ahci->block_size;
    }
    return Device->Scsi->block_size;
}

UINT64 storage_last_lba(const FW_STORAGE_DEVICE *Device)
{
    if (!storage_present(Device)) {
        return 0;
    }
    if (Device->Kind == FW_STORAGE_IDE) {
        return Device->Ide->last_lba;
    }
    if (Device->Kind == FW_STORAGE_AHCI) {
        return Device->Ahci->last_lba;
    }
    return Device->Scsi->last_lba;
}

BOOLEAN storage_read_blocks(const FW_STORAGE_DEVICE *Device,
                                   UINT8 *Buffer, UINT64 Lba, UINT32 Count)
{
    UINT32 done;
    UINT32 blocks_per_bounce;

    if (!storage_present(Device) || Buffer == NULL) {
        return Count == 0;
    }
    if (Count == 0) {
        return 1;
    }
    if (Device->Kind == FW_STORAGE_IDE) {
        if (Lba > 0xffffffffULL ||
            (UINT64)Count - 1U > 0xffffffffULL - Lba) {
            return 0;
        }
        if (Device->Ide->is_atapi) {
            return atapi_read_sectors(Device->Ide, Buffer, (UINT32)Lba,
                                      Count);
        }
        if (Count == 1) {
            return ata_pio_read_sector_cached(Device->Ide, Buffer,
                                               (UINT32)Lba);
        }
        done = 0;
        while (done < Count) {
            UINT32 chunk = Count - done;

            if (chunk > 255U) {
                chunk = 255U;
            }
            if (!ata_read_sectors(Device->Ide, Buffer + (UINTN)done * 512U,
                                  (UINT32)Lba + done, chunk)) {
                return 0;
            }
            done += chunk;
        }
        return 1;
    }

    if (Device->Kind == FW_STORAGE_AHCI) {
        return ahci_read_blocks(Device->Ahci, Buffer, Lba, Count);
    }

    if (Device->Scsi->block_size == 0) {
        return 0;
    }
    if (Lba > 0xffffffffULL ||
        (UINT64)Count - 1U > 0xffffffffULL - Lba) {
        return 0;
    }
    blocks_per_bounce = sizeof(mScsiBounce) / Device->Scsi->block_size;
    if (blocks_per_bounce == 0) {
        return 0;
    }
    if (blocks_per_bounce > 0xffffU) {
        blocks_per_bounce = 0xffffU;
    }

    done = 0;
    while (done < Count) {
        UINT32 chunk = Count - done;
        UINT32 bytes;

        if (chunk > blocks_per_bounce) {
            chunk = blocks_per_bounce;
        }
        bytes = chunk * Device->Scsi->block_size;
        if (bytes / Device->Scsi->block_size != chunk ||
            !scsi_read_blocks(Device->Scsi, mScsiBounce,
                              (UINT32)Lba + done,
                              chunk)) {
            return 0;
        }
        fw_copy_mem(Buffer + (UINTN)done * Device->Scsi->block_size,
                    mScsiBounce, bytes);
        done += chunk;
    }
    return 1;
}

BOOLEAN storage_write_blocks(const FW_STORAGE_DEVICE *Device,
                                    const UINT8 *Buffer, UINT64 Lba,
                                    UINT32 Count)
{
    UINT32 done = 0;
    UINT32 max_blocks;

    if (!storage_present(Device) || Buffer == NULL ||
        storage_read_only(Device)) {
        return Count == 0;
    }
    if (Count == 0) {
        return 1;
    }
    if (Device->Kind == FW_STORAGE_IDE && Device->Ide->is_atapi) {
        return 0;
    }

    if (Device->Kind == FW_STORAGE_AHCI) {
        return ahci_write_blocks(Device->Ahci, Buffer, Lba, Count);
    }

    if (Lba > 0xffffffffULL ||
        (UINT64)Count - 1U > 0xffffffffULL - Lba) {
        return 0;
    }

    max_blocks = Device->Kind == FW_STORAGE_IDE ? 255U :
        (UINT32)(sizeof(mScsiBounce) / Device->Scsi->block_size);
    if (max_blocks == 0) {
        return 0;
    }
    if (max_blocks > 0xffffU) {
        max_blocks = 0xffffU;
    }

    while (done < Count) {
        UINT32 chunk = Count - done;
        const UINT8 *chunk_buffer =
            Buffer + (UINTN)done * storage_block_size(Device);
        BOOLEAN ok;

        if (chunk > max_blocks) {
            chunk = max_blocks;
        }
        if (Device->Kind == FW_STORAGE_IDE) {
            ok = ata_write_sectors(Device->Ide, chunk_buffer,
                                   (UINT32)Lba + done, chunk);
        } else {
            ok = scsi_write_blocks(Device->Scsi, chunk_buffer,
                                   (UINT32)Lba + done, chunk);
        }
        if (!ok) {
            return 0;
        }
        done += chunk;
    }
    return 1;
}

void storage_invalidate_cache(const FW_STORAGE_DEVICE *Device);

BOOLEAN storage_flush(const FW_STORAGE_DEVICE *Device)
{
    if (!storage_present(Device)) {
        return 0;
    }
    if (storage_read_only(Device) || storage_is_cd(Device)) {
        return 1;
    }
    if (Device->Kind == FW_STORAGE_IDE) {
        ide_activate(Device->Ide);
        ide_select_device(ide_lba_drive_select(Device->Ide, 0));
        if (!ata_pio_wait_not_busy()) {
            return 0;
        }
        ata_pio_write8(gIde.data_base + IDE_CMD_OFF, ATA_CMD_FLUSH_CACHE);
        return ata_pio_wait_not_busy();
    }

    if (Device->Kind == FW_STORAGE_AHCI) {
        return ahci_flush(Device->Ahci);
    }

    fw_set_mem(mLsiCdb, sizeof(mLsiCdb), 0);
    mLsiCdb[0] = SCSI_CMD_SYNCHRONIZE_CACHE_10;
    return lsi_scsi_command_prepared(Device->Scsi, 10, NULL, 0);
}

BOOLEAN storage_refresh_media(const FW_STORAGE_DEVICE *Device)
{
    if (!storage_device_present(Device)) {
        return 0;
    }
    storage_invalidate_cache(Device);
    if (Device->Kind == FW_STORAGE_IDE) {
        if (Device->Ide->is_atapi) {
            (void)atapi_refresh_media(Device->Ide);
        } else {
            Device->Ide->media_present = 1;
        }
        return 1;
    }
    if (Device->Kind == FW_STORAGE_AHCI) {
        return ahci_identify_device(Device->Ahci);
    }
    scsi_refresh_media(Device->Scsi);
    return 1;
}

BOOLEAN storage_reset(const FW_STORAGE_DEVICE *Device,
                             BOOLEAN ExtendedVerification)
{
    if (Device == NULL || Device->Kind == FW_STORAGE_NONE) {
        return 0;
    }

    storage_invalidate_cache(Device);
    if (Device->Kind == FW_STORAGE_IDE) {
        UINTN timeout = 1000000U;
        UINT16 identify[256];
        UINT8 command = Device->Ide->is_atapi ?
                        ATA_CMD_IDENTIFY_PACKET : ATA_CMD_IDENTIFY;

        ide_activate(Device->Ide);
        /* ATA Device Control: assert and then release software reset. */
        ata_pio_write8(gIde.ctrl_base, 0x04U);
        (void)bs_stall(5U);
        ata_pio_write8(gIde.ctrl_base, 0);
        (void)bs_stall(2000U);
        do {
            UINT8 status = ata_pio_read8(gIde.ctrl_base);

            if (status == 0xffU) {
                return 0;
            }
            if ((status & ATA_SR_BSY) == 0) {
                break;
            }
            ata_pio_poll_delay();
        } while (--timeout != 0);
        if (timeout == 0) {
            return 0;
        }
        if (!ata_pio_identify(Device->Ide, command, identify)) {
            return 0;
        }
        if (Device->Ide->is_atapi) {
            (void)atapi_refresh_media(Device->Ide);
        } else {
            UINT32 sectors = (UINT32)identify[60] |
                             ((UINT32)identify[61] << 16);

            if (sectors == 0) {
                Device->Ide->media_present = 0;
                return 0;
            }
            Device->Ide->last_lba = sectors - 1U;
            Device->Ide->media_present = 1;
        }
        if (ExtendedVerification && !Device->Ide->is_atapi) {
            UINT8 verify[512];

            return ata_pio_read_sectors(Device->Ide, verify, 0, 1);
        }
        return 1;
    }

    if (Device->Kind == FW_STORAGE_AHCI) {
        return ahci_reset_device(Device->Ahci, ExtendedVerification);
    }

    return scsi_reset_device(Device->Scsi, ExtendedVerification);
}

void storage_invalidate_cache(const FW_STORAGE_DEVICE *Device)
{
    if (Device != NULL && Device->Kind == FW_STORAGE_IDE) {
        ata_read_cache_invalidate(Device->Ide);
    }
}


BOOLEAN fw_scsi_controller_present(VOID)
{
    return mLsiPresent != 0;
}

BOOLEAN fw_scsi_device_present(UINTN target)
{
    return target < SCSI_DEVICE_MAX && mScsiDevices[target].present != 0;
}

FW_LSI_SCRIPT_RESULT fw_scsi_execute_buffered(
    UINT8 target, const UINT8 *cdb, UINTN cdb_length, VOID *data,
    UINT32 data_length, BOOLEAN write_to_device, UINT64 timeout_100ns,
    UINT8 *target_status)
{
    LSI_SCRIPT_RESULT result;

    if (cdb == NULL || cdb_length == 0 || cdb_length > sizeof(mLsiCdb) ||
        data_length > sizeof(mScsiBounce) ||
        (data_length != 0 && data == NULL)) {
        return FwLsiScriptDeviceError;
    }
    fw_set_mem(mLsiCdb, sizeof(mLsiCdb), 0);
    fw_copy_mem(mLsiCdb, cdb, cdb_length);
    if (data_length != 0) {
        if (write_to_device) {
            fw_copy_mem(mScsiBounce, data, data_length);
        } else {
            fw_set_mem(mScsiBounce, data_length, 0);
        }
    }
    result = lsi_run_scsi_script_timed(
        target, mLsiCdb, cdb_length,
        data_length != 0 ? mScsiBounce : NULL, data_length,
        timeout_100ns, target_status);
    if (!write_to_device && data_length != 0 &&
        (result == LsiScriptSuccess || result == LsiScriptTargetStatus)) {
        fw_copy_mem(data, mScsiBounce, data_length);
    }
    return (FW_LSI_SCRIPT_RESULT)result;
}

EFI_STATUS fw_scsi_reset_channel(VOID)
{
    UINT8 scntl1;

    lsi_write8(LSI_REG_ISTAT0, LSI_ISTAT0_ABRT);
    scntl1 = lsi_read8(LSI_REG_SCNTL1);
    lsi_write8(LSI_REG_SCNTL1, scntl1 | LSI_SCNTL1_RST);
    if (bs_stall(25U) != EFI_SUCCESS) {
        return EFI_DEVICE_ERROR;
    }
    lsi_write8(LSI_REG_SCNTL1, scntl1 & ~LSI_SCNTL1_RST);
    if (bs_stall(250000U) != EFI_SUCCESS) {
        return EFI_DEVICE_ERROR;
    }
    lsi_write8(LSI_REG_SCID, SCSI_HOST_ID);
    lsi_write8(LSI_REG_RESPID0, (UINT8)(1U << SCSI_HOST_ID));
    lsi_write8(LSI_REG_SIEN0, 0);
    lsi_write8(LSI_REG_SIEN1, 0);
    lsi_write8(LSI_REG_ISTAT0, LSI_ISTAT0_INTF);
    return EFI_SUCCESS;
}

FW_LSI_SCRIPT_RESULT fw_scsi_reset_target(UINT8 target,
                                           UINT64 timeout_100ns)
{
    return (FW_LSI_SCRIPT_RESULT)lsi_reset_scsi_target(target,
                                                        timeout_100ns);
}
