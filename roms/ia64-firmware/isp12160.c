/*
 * QLogic ISP12160 host adapter: firmware-side transport.
 *
 * The i2000 carries this adapter where our machine has carried an LSI
 * 53c895a, so the firmware needs to read a boot disk through it.  The
 * adapter is driven the way its RISC engine expects: a mailbox handshake
 * brings the processor up, two rings in host memory carry work to it and
 * status back, and each request is one command IOCB naming a target, a CDB
 * and a data segment.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "fw-base.h"
#include "fw-isp12160.h"
#include "fw-services.h"
#include "fw-storage.h"
#include "hw/scsi/isp12160_abi.h"

/*
 * Ring geometry.  Two entries is the architectural minimum and the
 * firmware issues one request at a time, but a slightly longer ring costs
 * nothing and leaves room for the response to trail the request.
 */
#define ISP_QUEUE_ENTRIES     8U
#define ISP_QUEUE_BYTES       (ISP_QUEUE_ENTRIES * ISP12160_QUEUE_ENTRY_BYTES)
#define ISP_MAILBOX_TIMEOUT   1000000U
#define ISP_RESPONSE_TIMEOUT  4000000U

static UINT8 mIspRequestRing[ISP_QUEUE_BYTES] __attribute__((aligned(64)));
static UINT8 mIspResponseRing[ISP_QUEUE_BYTES] __attribute__((aligned(64)));
static UINT8 mIspToken[ISP12160_QEMU_TOKEN_WORDS * 2]
    __attribute__((aligned(8)));

static UINT64 mIspMmioBase;
static UINT16 mIspRequestProducer;
static UINT16 mIspResponseConsumer;
static UINT32 mIspHandle;
static BOOLEAN mIspPresent;
static BOOLEAN mIspTried;

/* The adapter's identity on the bus. */
#define FW_PCI_ISP12160_ID    0x12161077U

/* Little-endian stores, byte at a time so ring entries need no alignment. */
static void isp_store16(UINT8 *Pointer, UINT16 Value)
{
    Pointer[0] = (UINT8)Value;
    Pointer[1] = (UINT8)(Value >> 8);
}

static void isp_store32(UINT8 *Pointer, UINT32 Value)
{
    isp_store16(Pointer, (UINT16)Value);
    isp_store16(Pointer + 2, (UINT16)(Value >> 16));
}

static UINT16 isp_load16(const UINT8 *Pointer)
{
    return (UINT16)((UINT16)Pointer[0] | ((UINT16)Pointer[1] << 8));
}

static volatile UINT16 *isp_reg(UINT32 Offset)
{
    return (volatile UINT16 *)(UINTN)(mIspMmioBase + Offset); /* MMIO */
}

static UINT16 isp_read16(UINT32 Offset)
{
    return *isp_reg(Offset);
}

static void isp_write16(UINT32 Offset, UINT16 Value)
{
    *isp_reg(Offset) = Value;
}

static UINT16 isp_mailbox_read(unsigned int Index)
{
    return isp_read16(ISP12160_REG_MAILBOX0 + Index * 2U);
}

static void isp_mailbox_write(unsigned int Index, UINT16 Value)
{
    isp_write16(ISP12160_REG_MAILBOX0 + Index * 2U, Value);
}

/*
 * Run one mailbox command.  The RISC answers by taking the semaphore and
 * leaving its status in mailbox 0; the host releases the semaphore and
 * acknowledges the interrupt before the next command.
 */
static BOOLEAN isp_mailbox_command(const UINT16 *In, unsigned int InCount,
                                   UINT16 *Out, unsigned int OutCount)
{
    UINT16 status;
    unsigned int i;

    if (In == NULL || InCount == 0 || InCount > ISP12160_MAILBOX_COUNT) {
        return 0;
    }

    /*
     * Mailbox 0 goes first: writing the command word is what tells the
     * adapter a command is being staged, and until it does, writes to
     * mailboxes 4 and 5 are queue doorbells rather than command words.
     */
    for (i = 0; i < InCount; i++) {
        isp_mailbox_write(i, In[i]);
    }
    __asm__ __volatile__ ("mf" : : : "memory");
    isp_write16(ISP12160_REG_HOST_COMMAND, ISP12160_HC_SET_HOST_INT);

    for (i = 0; i < ISP_MAILBOX_TIMEOUT; i++) {
        if ((isp_read16(ISP12160_REG_SEMAPHORE) &
             ISP12160_SEMAPHORE_LOCK) != 0) {
            break;
        }
    }
    if ((isp_read16(ISP12160_REG_SEMAPHORE) &
         ISP12160_SEMAPHORE_LOCK) == 0) {
        return 0;
    }

    status = isp_mailbox_read(0);
    for (i = 0; i < OutCount && Out != NULL; i++) {
        Out[i] = isp_mailbox_read(i);
    }

    isp_write16(ISP12160_REG_SEMAPHORE, 0);
    isp_write16(ISP12160_REG_HOST_COMMAND, ISP12160_HC_CLEAR_RISC_INT);
    return status == ISP12160_MBS_COMMAND_COMPLETE;
}

/* Split a firmware pointer into the mailbox words an A64 command wants. */
static BOOLEAN isp_dma_mailbox(const VOID *Pointer, UINT16 *Mailbox)
{
    UINTN address = (UINTN)Pointer;

    if (Pointer == NULL || Mailbox == NULL) {
        return 0;
    }
    Mailbox[2] = (UINT16)((address >> 16) & 0xffffU);
    Mailbox[3] = (UINT16)(address & 0xffffU);
    Mailbox[7] = (UINT16)((address >> 32) & 0xffffU);
    Mailbox[6] = (UINT16)((address >> 48) & 0xffffU);
    return 1;
}

static BOOLEAN isp_find_controller(PCI_DEVICE_LOCATION *Location)
{
    UINT16 bus;
    UINT8 device;
    UINT8 function;

    if (Location == NULL) {
        return 0;
    }

    for (bus = 0; bus < PCI_MAX_BUSES; bus++) {
        for (device = 0; device < PCI_MAX_DEVICES; device++) {
            UINT8 function_count = 1;

            for (function = 0; function < function_count; function++) {
                UINT32 id = (UINT32)pci_config_read_value(
                    0, (UINT8)bus, device, function, 0, 4);

                if ((id & 0xffffU) == 0xffffU) {
                    if (function == 0) {
                        break;
                    }
                    continue;
                }
                if (function == 0) {
                    UINT8 header = (UINT8)pci_config_read_value(
                        0, (UINT8)bus, device, function,
                        PCI_HEADER_TYPE_OFFSET, 1);

                    if ((header & PCI_HEADER_TYPE_MULTI_FUNC) != 0) {
                        function_count = PCI_MAX_FUNCTIONS;
                    }
                }
                if (id == FW_PCI_ISP12160_ID) {
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
 * The device model gates its RISC on an activation token rather than a
 * real QLogic firmware image, which the firmware has no licence to carry.
 * Load the token for the SCSI variant, verify it and start the processor.
 */
static BOOLEAN isp_start_risc(void)
{
    UINT16 mailbox[ISP12160_MAILBOX_COUNT];
    UINT16 out[ISP12160_MAILBOX_COUNT];
    unsigned int i;

    isp_store16(mIspToken + 0, ISP12160_QEMU_SCSI_TOKEN_WORD0);
    isp_store16(mIspToken + 2, ISP12160_QEMU_SCSI_TOKEN_WORD1);
    isp_store16(mIspToken + 4, ISP12160_QEMU_SCSI_TOKEN_WORD2);
    isp_store16(mIspToken + 6, ISP12160_QEMU_SCSI_TOKEN_WORD3);

    isp_write16(ISP12160_REG_HOST_COMMAND, ISP12160_HC_RESET_RISC);
    for (i = 0; i < 1000; i++) {
        (void)isp_read16(ISP12160_REG_ISTATUS);
    }
    isp_write16(ISP12160_REG_HOST_COMMAND, ISP12160_HC_RELEASE_RISC);
    isp_write16(ISP12160_REG_HOST_COMMAND, ISP12160_HC_CLEAR_RISC_INT);
    isp_write16(ISP12160_REG_SEMAPHORE, 0);

    fw_set_mem(mailbox, sizeof(mailbox), 0);
    mailbox[0] = ISP12160_MBC_LOAD_RAM_A64_ROM;
    mailbox[ISP12160_MBOX_RISC_ADDRESS] = ISP12160_QEMU_ACTIVATION_RISC_ADDR;
    mailbox[ISP12160_MBOX_LOAD_WORDS] = ISP12160_QEMU_TOKEN_WORDS;
    if (!isp_dma_mailbox(mIspToken, mailbox) ||
        !isp_mailbox_command(mailbox, ISP12160_MAILBOX_COUNT, NULL, 0)) {
        return 0;
    }

    fw_set_mem(mailbox, sizeof(mailbox), 0);
    mailbox[0] = ISP12160_MBC_VERIFY_CHECKSUM;
    mailbox[ISP12160_MBOX_RISC_ADDRESS] = ISP12160_QEMU_ACTIVATION_RISC_ADDR;
    if (!isp_mailbox_command(mailbox, 2, NULL, 0)) {
        return 0;
    }

    fw_set_mem(mailbox, sizeof(mailbox), 0);
    mailbox[0] = ISP12160_MBC_EXECUTE_FIRMWARE;
    mailbox[ISP12160_MBOX_RISC_ADDRESS] = ISP12160_QEMU_ACTIVATION_RISC_ADDR;
    if (!isp_mailbox_command(mailbox, 2, NULL, 0)) {
        return 0;
    }

    fw_set_mem(mailbox, sizeof(mailbox), 0);
    mailbox[0] = ISP12160_MBC_ABOUT_FIRMWARE;
    return isp_mailbox_command(mailbox, 1, out, ISP12160_MAILBOX_COUNT);
}

static BOOLEAN isp_init_queues(void)
{
    UINT16 mailbox[ISP12160_MAILBOX_COUNT];

    fw_set_mem(mIspRequestRing, sizeof(mIspRequestRing), 0);
    fw_set_mem(mIspResponseRing, sizeof(mIspResponseRing), 0);
    mIspRequestProducer = 0;
    mIspResponseConsumer = 0;

    fw_set_mem(mailbox, sizeof(mailbox), 0);
    mailbox[0] = ISP12160_MBC_INIT_REQUEST_QUEUE_A64;
    mailbox[ISP12160_MBOX_QUEUE_LENGTH] = ISP_QUEUE_ENTRIES;
    mailbox[ISP12160_MBOX_REQUEST_INDEX] = 0;
    if (!isp_dma_mailbox(mIspRequestRing, mailbox) ||
        !isp_mailbox_command(mailbox, ISP12160_MAILBOX_COUNT, NULL, 0)) {
        return 0;
    }

    fw_set_mem(mailbox, sizeof(mailbox), 0);
    mailbox[0] = ISP12160_MBC_INIT_RESPONSE_QUEUE_A64;
    mailbox[ISP12160_MBOX_QUEUE_LENGTH] = ISP_QUEUE_ENTRIES;
    mailbox[ISP12160_MBOX_RESPONSE_INDEX] = 0;
    if (!isp_dma_mailbox(mIspResponseRing, mailbox) ||
        !isp_mailbox_command(mailbox, ISP12160_MAILBOX_COUNT, NULL, 0)) {
        return 0;
    }

    /*
     * The parameter-setting commands (initiator id, timeouts, negotiation)
     * are only answered when a real QLogic firmware image has been loaded.
     * The activation token brings up the queue and IOCB protocol, which is
     * all a boot-time reader needs, so leave those parameters at their
     * reset values rather than issuing commands that would be refused.
     */
    return 1;
}

BOOLEAN isp12160_present(void)
{
    return mIspPresent;
}

UINT64 isp12160_mmio_base(void)
{
    return mIspMmioBase;
}

BOOLEAN isp12160_initialise(void)
{
    PCI_DEVICE_LOCATION location;
    UINT32 bar;
    UINT16 command;

    if (mIspTried) {
        return mIspPresent;
    }
    mIspTried = 1;

    if (!isp_find_controller(&location)) {
        return 0;
    }

    bar = (UINT32)pci_config_read_value(0, location.Bus, location.Device,
                                        location.Function,
                                        PCI_IDE_BAR1_OFFSET, 4);
    if (bar == 0 || bar == 0xffffffffU || (bar & 1U) != 0) {
        return 0;
    }
    mIspMmioBase = bar & ~(UINT64)0x0fU;
    if (mIspMmioBase < IA64_PCI_MMIO_BASE ||
        mIspMmioBase >= IA64_PCI_MMIO_BASE + IA64_PCI_MMIO_SIZE) {
        mIspMmioBase = 0;
        return 0;
    }

    command = (UINT16)pci_config_read_value(0, location.Bus, location.Device,
                                            location.Function,
                                            PCI_CFG_COMMAND_OFFSET, 2);
    command |= PCI_CFG_COMMAND_MEMORY_SPACE | PCI_CFG_COMMAND_BUS_MASTER;
    pci_config_write_value(0, location.Bus, location.Device,
                           location.Function, PCI_CFG_COMMAND_OFFSET, 2,
                           command);

    if (!isp_start_risc() || !isp_init_queues()) {
        mIspMmioBase = 0;
        return 0;
    }

    mIspPresent = 1;
    return 1;
}

BOOLEAN isp12160_command(UINT8 Target, const UINT8 *Cdb, UINTN CdbLength,
                         UINT8 *Data, UINT32 DataLength, BOOLEAN ToDevice,
                         UINT8 *ScsiStatus)
{
    UINT8 *entry;
    UINT8 *response;
    UINT16 control;
    UINT16 next;
    UINTN address;
    UINT32 i;

    if (!mIspPresent || Cdb == NULL || CdbLength == 0 ||
        CdbLength > ISP12160_IOCB_CDB_BYTES ||
        (DataLength != 0 && Data == NULL)) {
        return 0;
    }

    entry = mIspRequestRing +
            (UINTN)mIspRequestProducer * ISP12160_QUEUE_ENTRY_BYTES;
    fw_set_mem(entry, ISP12160_QUEUE_ENTRY_BYTES, 0);

    control = DataLength == 0 ? 0 :
              (ToDevice ? ISP12160_IOCB_CONTROL_DATA_TO_DEVICE :
                          ISP12160_IOCB_CONTROL_DATA_FROM_DEVICE);
    mIspHandle++;

    entry[ISP12160_IOCB_HEADER_TYPE_OFFSET] = ISP12160_IOCB_COMMAND_A64_TYPE;
    entry[ISP12160_IOCB_HEADER_COUNT_OFFSET] = 1;
    isp_store32(entry + ISP12160_IOCB_A64_HANDLE_OFFSET, mIspHandle);
    entry[ISP12160_IOCB_A64_LUN_OFFSET] = 0;
    entry[ISP12160_IOCB_A64_TARGET_OFFSET] = Target;
    isp_store16(entry + ISP12160_IOCB_A64_CDB_LENGTH_OFFSET,
                (UINT16)CdbLength);
    isp_store16(entry + ISP12160_IOCB_A64_CONTROL_FLAGS_OFFSET, control);
    isp_store16(entry + ISP12160_IOCB_A64_SEGMENT_COUNT_OFFSET,
                DataLength == 0 ? 0 : 1);
    for (i = 0; i < CdbLength; i++) {
        entry[ISP12160_IOCB_A64_CDB_OFFSET + i] = Cdb[i];
    }
    if (DataLength != 0) {
        address = (UINTN)Data;
        isp_store32(entry + ISP12160_IOCB_A64_SEGMENT0_OFFSET,
                    (UINT32)address);
        isp_store32(entry + ISP12160_IOCB_A64_SEGMENT0_OFFSET + 4,
                    (UINT32)(address >> 32));
        isp_store32(entry + ISP12160_IOCB_A64_SEGMENT0_OFFSET + 8,
                    DataLength);
    }

    response = mIspResponseRing +
               (UINTN)mIspResponseConsumer * ISP12160_QUEUE_ENTRY_BYTES;
    fw_set_mem(response, ISP12160_QUEUE_ENTRY_BYTES, 0);

    next = (UINT16)((mIspRequestProducer + 1U) % ISP_QUEUE_ENTRIES);
    __asm__ __volatile__ ("mf" : : : "memory");
    isp_mailbox_write(ISP12160_MBOX_REQUEST_INDEX, next);
    mIspRequestProducer = next;

    for (i = 0; i < ISP_RESPONSE_TIMEOUT; i++) {
        __asm__ __volatile__ ("mf" : : : "memory");
        if (response[ISP12160_IOCB_HEADER_TYPE_OFFSET] ==
            ISP12160_IOCB_STATUS_TYPE) {
            break;
        }
    }
    if (response[ISP12160_IOCB_HEADER_TYPE_OFFSET] !=
        ISP12160_IOCB_STATUS_TYPE) {
        return 0;
    }

    if (ScsiStatus != NULL) {
        *ScsiStatus = (UINT8)isp_load16(
            response + ISP12160_IOCB_STATUS_SCSI_STATUS_OFFSET);
    }
    i = isp_load16(response + ISP12160_IOCB_STATUS_COMPLETION_OFFSET);

    mIspResponseConsumer =
        (UINT16)((mIspResponseConsumer + 1U) % ISP_QUEUE_ENTRIES);
    isp_mailbox_write(ISP12160_MBOX_RESPONSE_INDEX, mIspResponseConsumer);
    isp_write16(ISP12160_REG_HOST_COMMAND, ISP12160_HC_CLEAR_RISC_INT);

    return i == ISP12160_IOCB_CS_COMPLETE ||
           i == ISP12160_IOCB_CS_DATA_UNDERRUN;
}
