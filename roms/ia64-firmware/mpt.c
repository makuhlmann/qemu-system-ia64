/*
 * LSI Fusion-MPT host adapter (53C1030): firmware-side transport.
 *
 * The rx2600 carries a two-function 53C1030 at 20:01, and Server 2008 R2
 * has a driver for no other parallel SCSI adapter, so the firmware has to
 * read a boot disk through it.  Each function is an I/O controller (IOC) of
 * its own.  A doorbell handshake takes it to the operational state; after
 * that, request frames reach it through the request FIFO and replies come
 * back through the reply FIFO, either as the request's message context (a
 * success) or as the address of a reply frame the host gave it.  Message
 * layouts and values: MPI 1.x as in Linux
 * drivers/message/fusion/lsi/mpi.h, mpi_ioc.h and mpi_init.h.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "fw-base.h"
#include "fw-mpt.h"
#include "fw-services.h"
#include "fw-storage.h"

#define FW_PCI_LSI53C1030_ID        0x00301000U
#define MPT_BAR1_LOW_OFFSET         0x14U
#define MPT_BAR1_HIGH_OFFSET        0x18U

/* System interface registers (mpi.h). */
#define MPT_REG_DOORBELL            0x00U
#define MPT_REG_HOST_INT_STATUS     0x30U
#define MPT_REG_HOST_INT_MASK       0x34U
#define MPT_REG_REQUEST_FIFO        0x40U
#define MPT_REG_REPLY_FIFO          0x44U

#define MPT_STATE_MASK              0xf0000000U
#define MPT_STATE_READY             0x10000000U
#define MPT_STATE_OPERATIONAL       0x20000000U
#define MPT_DOORBELL_ACTIVE         0x08000000U
#define MPT_DOORBELL_DATA_MASK      0x0000ffffU
#define MPT_DOORBELL_FUNCTION_SHIFT 24U
#define MPT_DOORBELL_DWORDS_SHIFT   16U
#define MPT_HIS_IOP_DOORBELL        0x80000000U
#define MPT_HIS_DOORBELL            0x00000001U
#define MPT_HIM_REPLY               0x00000008U
#define MPT_HIM_DOORBELL            0x00000001U
#define MPT_ADDRESS_REPLY           0x80000000U
#define MPT_FIFO_EMPTY              0xffffffffU

#define MPT_FUNC_SCSI_IO            0x00U
#define MPT_FUNC_IOC_INIT           0x02U
#define MPT_FUNC_IOC_FACTS          0x03U
#define MPT_FUNC_PORT_ENABLE        0x06U
#define MPT_FUNC_MESSAGE_UNIT_RESET 0x40U
#define MPT_FUNC_HANDSHAKE          0x42U

/* The firmware is the system's own, not an adapter option ROM (mpi_ioc.h). */
#define MPT_WHOINIT_SYSTEM_BIOS     0x01U

#define MPT_IOCSTATUS_MASK          0x7fffU
#define MPT_IOCSTATUS_SUCCESS       0x0000U
#define MPT_IOCSTATUS_SCSI_RECOVERED 0x0040U
#define MPT_IOCSTATUS_SCSI_INVALID_BUS 0x0041U
#define MPT_IOCSTATUS_SCSI_INVALID_TARGETID 0x0042U
#define MPT_IOCSTATUS_SCSI_NOT_THERE 0x0043U
#define MPT_IOCSTATUS_SCSI_UNDERRUN 0x0045U

#define MPT_SCSIIO_CONTROL_WRITE    0x01000000U
#define MPT_SCSIIO_CONTROL_READ     0x02000000U

/* SGE flags, in bits 31:24 of FlagsLength (mpi.h). */
#define MPT_SGE_LAST_ELEMENT        0x80U
#define MPT_SGE_END_OF_BUFFER       0x40U
#define MPT_SGE_SIMPLE_ELEMENT      0x10U
#define MPT_SGE_HOST_TO_IOC         0x04U
#define MPT_SGE_64_BIT_ADDRESSING   0x02U
#define MPT_SGE_END_OF_LIST         0x01U
#define MPT_SGE_FLAGS_SHIFT         24U
#define MPT_SGE_LENGTH_MASK         0x00ffffffU

/* Message offsets (MSG_IOC_FACTS_REPLY, MSG_IOC_INIT, MSG_SCSI_IO_*). */
#define MPT_MSG_LENGTH              0x02U
#define MPT_MSG_FUNCTION            0x03U
#define MPT_MSG_CONTEXT             0x08U
#define MPT_REPLY_IOC_STATUS        0x0eU
#define MPT_FACTS_MSG_VERSION       0x00U
#define MPT_FACTS_HEADER_VERSION    0x04U
#define MPT_FACTS_REQUEST_FRAME     0x1aU
#define MPT_FACTS_MAX_DEVICES       0x2eU
#define MPT_INIT_WHO_INIT           0x00U
#define MPT_INIT_MAX_DEVICES        0x05U
#define MPT_INIT_MAX_BUSES          0x06U
#define MPT_INIT_REPLY_FRAME_SIZE   0x0cU
#define MPT_INIT_MSG_VERSION        0x28U
#define MPT_INIT_HEADER_VERSION     0x2aU
#define MPT_INIT_BYTES              0x2cU
#define MPT_FACTS_BYTES             0x0cU
#define MPT_PORT_ENABLE_BYTES       0x0cU
#define MPT_IO_TARGET               0x00U
#define MPT_IO_CDB_LENGTH           0x04U
#define MPT_IO_SENSE_LENGTH         0x05U
#define MPT_IO_CONTROL              0x14U
#define MPT_IO_CDB                  0x18U
#define MPT_IO_DATA_LENGTH          0x28U
#define MPT_IO_SENSE_ADDRESS        0x2cU
#define MPT_IO_SGL                  0x30U
#define MPT_IO_BYTES                (MPT_IO_SGL + 12U)
#define MPT_IO_REPLY_SCSI_STATUS    0x0cU
#define MPT_CDB_MAX                 16U

#define MPT_REQUEST_FRAME_BYTES     128U
#define MPT_REPLY_FRAME_BYTES       64U
#define MPT_REPLY_FRAMES            4U
#define MPT_SENSE_BYTES             32U
#define MPT_HANDSHAKE_WORDS         64U

/*
 * Waits are bounded in time, read from the ITC, as in isp12160.c: 10 s for
 * the handshake and a state change, 30 s for a command that moves data.
 */
#define MPT_HANDSHAKE_TIMEOUT_US    10000000ULL
#define MPT_COMMAND_TIMEOUT_US      30000000ULL

typedef struct {
    UINT64 MmioBase;
    PCI_DEVICE_LOCATION Location;
    UINT32 Context;
    BOOLEAN Present;
} MPT_IOC;

/*
 * Frames live in the firmware image, below 4 GiB, so the high halves of
 * their addresses that IOCInit fixes (HostMfaHighAddr, SenseBufferHighAddr)
 * are zero.  Data buffers can be anywhere; their SGEs carry 64 bits.
 */
static UINT8 mMptRequest[MPT_MAX_IOCS][MPT_REQUEST_FRAME_BYTES]
    __attribute__((aligned(16)));
static UINT8 mMptReply[MPT_MAX_IOCS][MPT_REPLY_FRAMES][MPT_REPLY_FRAME_BYTES]
    __attribute__((aligned(16)));
static UINT8 mMptSense[MPT_MAX_IOCS][MPT_SENSE_BYTES]
    __attribute__((aligned(16)));
static MPT_IOC mMptIocs[MPT_MAX_IOCS];
static UINTN mMptIocCount;
static BOOLEAN mMptTried;

static void mpt_store16(UINT8 *Pointer, UINT16 Value)
{
    Pointer[0] = (UINT8)Value;
    Pointer[1] = (UINT8)(Value >> 8);
}

static void mpt_store32(UINT8 *Pointer, UINT32 Value)
{
    mpt_store16(Pointer, (UINT16)Value);
    mpt_store16(Pointer + 2, (UINT16)(Value >> 16));
}

static UINT16 mpt_load16(const UINT8 *Pointer)
{
    return (UINT16)((UINT16)Pointer[0] | ((UINT16)Pointer[1] << 8));
}

static UINT32 mpt_load32(const UINT8 *Pointer)
{
    return (UINT32)mpt_load16(Pointer) |
           ((UINT32)mpt_load16(Pointer + 2) << 16);
}

static UINT32 mpt_read32(const MPT_IOC *Ioc, UINT32 Offset)
{
    return *(volatile UINT32 *)(UINTN)(Ioc->MmioBase + Offset); /* MMIO */
}

static void mpt_write32(const MPT_IOC *Ioc, UINT32 Offset, UINT32 Value)
{
    *(volatile UINT32 *)(UINTN)(Ioc->MmioBase + Offset) = Value; /* MMIO */
}

static BOOLEAN mpt_addr32(const VOID *Pointer, UINT32 *Address)
{
    UINTN address = (UINTN)Pointer;

    if ((address >> 32) != 0) {
        return 0;
    }
    *Address = (UINT32)address;
    return 1;
}

static BOOLEAN mpt_wait_his(const MPT_IOC *Ioc, UINT32 Mask, UINT32 Value)
{
    UINT64 start = fw_read_itc();

    while ((mpt_read32(Ioc, MPT_REG_HOST_INT_STATUS) & Mask) != Value) {
        if (fw_wait_expired(start, MPT_HANDSHAKE_TIMEOUT_US)) {
            return (mpt_read32(Ioc, MPT_REG_HOST_INT_STATUS) & Mask) == Value;
        }
    }
    return 1;
}

static BOOLEAN mpt_wait_doorbell_int(const MPT_IOC *Ioc)
{
    return mpt_wait_his(Ioc, MPT_HIS_DOORBELL, MPT_HIS_DOORBELL);
}

/* The IOC has taken the last doorbell write once it drops this bit. */
static BOOLEAN mpt_wait_doorbell_ack(const MPT_IOC *Ioc)
{
    return mpt_wait_his(Ioc, MPT_HIS_IOP_DOORBELL, 0);
}

static BOOLEAN mpt_wait_state(const MPT_IOC *Ioc, UINT32 State)
{
    UINT64 start = fw_read_itc();

    while ((mpt_read32(Ioc, MPT_REG_DOORBELL) & MPT_STATE_MASK) != State) {
        if (fw_wait_expired(start, MPT_HANDSHAKE_TIMEOUT_US)) {
            return (mpt_read32(Ioc, MPT_REG_DOORBELL) & MPT_STATE_MASK) ==
                   State;
        }
    }
    return 1;
}

/*
 * Send a request through the doorbell and read the reply back sixteen bits
 * at a time.  The reply's own MsgLength says how long it is; words past
 * ReplyBytes are read and dropped.  The IOC raises one more doorbell
 * interrupt after the last word, and it has to be cleared, or the next
 * handshake would take it for its first acknowledgement.
 */
static BOOLEAN mpt_handshake(const MPT_IOC *Ioc, const UINT8 *Request,
                             UINTN RequestBytes, UINT8 *Reply,
                             UINTN ReplyBytes)
{
    UINTN words = 2;
    UINTN i;

    if ((RequestBytes & 3U) != 0 ||
        (mpt_read32(Ioc, MPT_REG_DOORBELL) & MPT_DOORBELL_ACTIVE) != 0) {
        return 0;
    }
    fw_set_mem(Reply, ReplyBytes, 0);
    mpt_write32(Ioc, MPT_REG_HOST_INT_STATUS, 0);
    mpt_write32(Ioc, MPT_REG_DOORBELL,
                (MPT_FUNC_HANDSHAKE << MPT_DOORBELL_FUNCTION_SHIFT) |
                ((UINT32)(RequestBytes / 4U) << MPT_DOORBELL_DWORDS_SHIFT));
    if (!mpt_wait_doorbell_int(Ioc)) {
        return 0;
    }
    mpt_write32(Ioc, MPT_REG_HOST_INT_STATUS, 0);
    if (!mpt_wait_doorbell_ack(Ioc)) {
        return 0;
    }
    for (i = 0; i < RequestBytes; i += 4) {
        mpt_write32(Ioc, MPT_REG_DOORBELL, mpt_load32(Request + i));
        if (!mpt_wait_doorbell_ack(Ioc)) {
            return 0;
        }
    }

    for (i = 0; i < words && i < MPT_HANDSHAKE_WORDS; i++) {
        UINT16 word;

        if (!mpt_wait_doorbell_int(Ioc)) {
            return 0;
        }
        word = (UINT16)(mpt_read32(Ioc, MPT_REG_DOORBELL) &
                        MPT_DOORBELL_DATA_MASK);
        mpt_write32(Ioc, MPT_REG_HOST_INT_STATUS, 0);
        if (i * 2U + 1U < ReplyBytes) {
            mpt_store16(Reply + i * 2U, word);
        }
        if (i == 1) {
            /* MsgLength, in dwords, is the low byte of the second word. */
            words = (UINTN)(word & 0xffU) * 2U;
            if (words < 2) {
                words = 2;
            }
        }
    }
    if (mpt_wait_doorbell_int(Ioc)) {
        mpt_write32(Ioc, MPT_REG_HOST_INT_STATUS, 0);
    }
    return 1;
}

static BOOLEAN mpt_make_ready(const MPT_IOC *Ioc)
{
    if ((mpt_read32(Ioc, MPT_REG_DOORBELL) & MPT_STATE_MASK) ==
        MPT_STATE_READY) {
        return 1;
    }
    mpt_write32(Ioc, MPT_REG_HOST_INT_STATUS, 0);
    mpt_write32(Ioc, MPT_REG_DOORBELL,
                MPT_FUNC_MESSAGE_UNIT_RESET << MPT_DOORBELL_FUNCTION_SHIFT);
    return mpt_wait_state(Ioc, MPT_STATE_READY);
}

static void mpt_release_reply(const MPT_IOC *Ioc, const UINT8 *Frame)
{
    UINT32 address;

    if (mpt_addr32(Frame, &address)) {
        mpt_write32(Ioc, MPT_REG_REPLY_FIFO, address);
    }
}

/*
 * Post the controller's request frame and wait for the reply that carries
 * its context.  A context reply leaves *ReplyFrame NULL; an address reply
 * hands back the frame, which the caller returns with mpt_release_reply().
 */
static BOOLEAN mpt_post_and_wait(MPT_IOC *Ioc, UINTN Index, UINT32 Context,
                                 UINT64 TimeoutUs, UINT8 **ReplyFrame)
{
    UINT32 request;
    UINT64 start;

    *ReplyFrame = NULL;
    if (!mpt_addr32(mMptRequest[Index], &request)) {
        return 0;
    }
    __asm__ __volatile__ ("mf" : : : "memory");
    start = fw_read_itc();
    mpt_write32(Ioc, MPT_REG_REQUEST_FIFO, request);

    for (;;) {
        UINT32 value = mpt_read32(Ioc, MPT_REG_REPLY_FIFO);

        if (value == MPT_FIFO_EMPTY) {
            if (fw_wait_expired(start, TimeoutUs)) {
                return 0;
            }
            continue;
        }
        if ((value & MPT_ADDRESS_REPLY) == 0) {
            if (value == Context) {
                return 1;
            }
            continue;
        }
        {
            UINT8 *frame = (UINT8 *)(UINTN)(UINT32)(value << 1);

            __asm__ __volatile__ ("mf" : : : "memory");
            if (mpt_load32(frame + MPT_MSG_CONTEXT) == Context) {
                *ReplyFrame = frame;
                return 1;
            }
            mpt_release_reply(Ioc, frame);
        }
    }
}

static BOOLEAN mpt_reply_ok(const UINT8 *Frame)
{
    UINT16 status = mpt_load16(Frame + MPT_REPLY_IOC_STATUS) &
                    MPT_IOCSTATUS_MASK;

    return status == MPT_IOCSTATUS_SUCCESS;
}

static BOOLEAN mpt_bring_up(MPT_IOC *Ioc, UINTN Index)
{
    UINT8 request[MPT_INIT_BYTES];
    UINT8 reply[MPT_HANDSHAKE_WORDS * 2U];
    UINT16 msg_version;
    UINT16 header_version;
    UINT8 *frame;
    UINTN i;

    if (!mpt_make_ready(Ioc)) {
        return 0;
    }
    /* The firmware polls; keep both interrupt sources masked. */
    mpt_write32(Ioc, MPT_REG_HOST_INT_MASK, MPT_HIM_REPLY | MPT_HIM_DOORBELL);

    fw_set_mem(request, sizeof(request), 0);
    request[MPT_MSG_FUNCTION] = MPT_FUNC_IOC_FACTS;
    mpt_store32(request + MPT_MSG_CONTEXT, ++Ioc->Context);
    if (!mpt_handshake(Ioc, request, MPT_FACTS_BYTES, reply, sizeof(reply)) ||
        !mpt_reply_ok(reply) ||
        (UINTN)mpt_load16(reply + MPT_FACTS_REQUEST_FRAME) * 4U <
            MPT_IO_BYTES) {
        return 0;
    }
    msg_version = mpt_load16(reply + MPT_FACTS_MSG_VERSION);
    header_version = mpt_load16(reply + MPT_FACTS_HEADER_VERSION);

    fw_set_mem(request, sizeof(request), 0);
    request[MPT_INIT_WHO_INIT] = MPT_WHOINIT_SYSTEM_BIOS;
    request[MPT_MSG_FUNCTION] = MPT_FUNC_IOC_INIT;
    request[MPT_INIT_MAX_DEVICES] = reply[MPT_FACTS_MAX_DEVICES];
    request[MPT_INIT_MAX_BUSES] = 1;
    mpt_store32(request + MPT_MSG_CONTEXT, ++Ioc->Context);
    mpt_store16(request + MPT_INIT_REPLY_FRAME_SIZE, MPT_REPLY_FRAME_BYTES);
    mpt_store16(request + MPT_INIT_MSG_VERSION, msg_version);
    mpt_store16(request + MPT_INIT_HEADER_VERSION, header_version);
    if (!mpt_handshake(Ioc, request, MPT_INIT_BYTES, reply, sizeof(reply)) ||
        !mpt_reply_ok(reply) ||
        !mpt_wait_state(Ioc, MPT_STATE_OPERATIONAL)) {
        return 0;
    }

    for (i = 0; i < MPT_REPLY_FRAMES; i++) {
        fw_set_mem(mMptReply[Index][i], MPT_REPLY_FRAME_BYTES, 0);
        mpt_release_reply(Ioc, mMptReply[Index][i]);
    }

    fw_set_mem(mMptRequest[Index], MPT_REQUEST_FRAME_BYTES, 0);
    mMptRequest[Index][MPT_MSG_FUNCTION] = MPT_FUNC_PORT_ENABLE;
    mpt_store32(mMptRequest[Index] + MPT_MSG_CONTEXT, ++Ioc->Context);
    if (!mpt_post_and_wait(Ioc, Index, Ioc->Context, MPT_HANDSHAKE_TIMEOUT_US,
                           &frame)) {
        return 0;
    }
    if (frame != NULL) {
        BOOLEAN ok = mpt_reply_ok(frame);

        mpt_release_reply(Ioc, frame);
        if (!ok) {
            return 0;
        }
    }
    return 1;
}

static BOOLEAN mpt_map_bar(const PCI_DEVICE_LOCATION *Location, UINT64 *Base)
{
    UINT32 low = (UINT32)pci_config_read_value(0, Location->Bus,
                                               Location->Device,
                                               Location->Function,
                                               MPT_BAR1_LOW_OFFSET, 4);
    UINT32 high = (UINT32)pci_config_read_value(0, Location->Bus,
                                                Location->Device,
                                                Location->Function,
                                                MPT_BAR1_HIGH_OFFSET, 4);
    UINT64 base = ((UINT64)high << 32) | (low & ~(UINT64)0x0fU);
    UINT16 command;

    if ((low & 1U) != 0 || base < IA64_PCI_MMIO_BASE ||
        base >= IA64_PCI_MMIO_BASE + IA64_PCI_MMIO_SIZE) {
        return 0;
    }
    command = (UINT16)pci_config_read_value(0, Location->Bus,
                                            Location->Device,
                                            Location->Function,
                                            PCI_CFG_COMMAND_OFFSET, 2);
    command |= PCI_CFG_COMMAND_MEMORY_SPACE | PCI_CFG_COMMAND_BUS_MASTER;
    pci_config_write_value(0, Location->Bus, Location->Device,
                           Location->Function, PCI_CFG_COMMAND_OFFSET, 2,
                           command);
    *Base = base;
    return 1;
}

static void mpt_try_function(const PCI_DEVICE_LOCATION *Location)
{
    MPT_IOC *ioc;

    if (mMptIocCount >= MPT_MAX_IOCS) {
        return;
    }
    ioc = &mMptIocs[mMptIocCount];
    fw_set_mem(ioc, sizeof(*ioc), 0);
    ioc->Location = *Location;
    if (!mpt_map_bar(Location, &ioc->MmioBase) ||
        !mpt_bring_up(ioc, mMptIocCount)) {
        fw_set_mem(ioc, sizeof(*ioc), 0);
        return;
    }
    ioc->Present = 1;
    mMptIocCount++;
}

UINTN mpt_initialise(void)
{
    PCI_DEVICE_LOCATION seat;
    BOOLEAN seated;
    UINT16 bus;
    UINT8 device;
    UINT8 function;

    if (mMptTried) {
        return mMptIocCount;
    }
    mMptTried = 1;

    /* The seat's two functions come first (scsi_seat_location()). */
    seated = scsi_seat_holds(FW_PCI_LSI53C1030_ID, &seat);
    if (seated) {
        mpt_try_function(&seat);
        seat.Function = 1;
        if ((UINT32)pci_config_read_value(0, seat.Bus, seat.Device, 1, 0,
                                          4) == FW_PCI_LSI53C1030_ID) {
            mpt_try_function(&seat);
        }
    }

    for (bus = 0; bus < PCI_MAX_BUSES; bus++) {
        for (device = 0; device < PCI_MAX_DEVICES; device++) {
            UINT8 function_count = 1;

            for (function = 0; function < function_count; function++) {
                PCI_DEVICE_LOCATION location;
                UINT32 id = (UINT32)pci_config_read_value(
                    0, (UINT8)bus, device, function, 0, 4);

                if ((id & 0xffffU) == 0xffffU) {
                    if (function == 0) {
                        break;
                    }
                    continue;
                }
                if (function == 0 &&
                    ((UINT8)pci_config_read_value(0, (UINT8)bus, device, 0,
                                                  PCI_HEADER_TYPE_OFFSET,
                                                  1) &
                     PCI_HEADER_TYPE_MULTI_FUNC) != 0) {
                    function_count = PCI_MAX_FUNCTIONS;
                }
                if (id != FW_PCI_LSI53C1030_ID ||
                    (seated && bus == seat.Bus && device == seat.Device)) {
                    continue;
                }
                location.Bus = (UINT8)bus;
                location.Device = device;
                location.Function = function;
                mpt_try_function(&location);
            }
        }
    }
    return mMptIocCount;
}

UINTN mpt_ioc_count(void)
{
    return mMptIocCount;
}

UINT64 mpt_mmio_base(UINTN Ioc)
{
    return Ioc < mMptIocCount ? mMptIocs[Ioc].MmioBase : 0;
}

BOOLEAN mpt_location(UINTN Ioc, PCI_DEVICE_LOCATION *Location)
{
    if (Ioc >= mMptIocCount || Location == NULL) {
        return 0;
    }
    *Location = mMptIocs[Ioc].Location;
    return 1;
}

MPT_RESULT mpt_execute(UINTN Ioc, UINT8 Target, const UINT8 *Cdb,
                       UINTN CdbLength, UINT8 *Data, UINT32 DataLength,
                       BOOLEAN ToDevice, UINT8 *ScsiStatus)
{
    MPT_IOC *ioc;
    UINT8 *frame;
    UINT8 *reply;
    UINT32 sense;
    UINT32 flags;
    UINT16 status;
    UINT8 scsi_status;
    UINTN address = (UINTN)Data;
    UINTN i;

    if (Ioc >= mMptIocCount || !mMptIocs[Ioc].Present || Cdb == NULL ||
        CdbLength == 0 || CdbLength > MPT_CDB_MAX ||
        DataLength > MPT_SGE_LENGTH_MASK ||
        (DataLength != 0 && Data == NULL) ||
        !mpt_addr32(mMptSense[Ioc], &sense)) {
        return MptResultError;
    }
    ioc = &mMptIocs[Ioc];
    frame = mMptRequest[Ioc];

    fw_set_mem(frame, MPT_REQUEST_FRAME_BYTES, 0);
    frame[MPT_IO_TARGET] = Target;
    frame[MPT_MSG_FUNCTION] = MPT_FUNC_SCSI_IO;
    frame[MPT_IO_CDB_LENGTH] = (UINT8)CdbLength;
    frame[MPT_IO_SENSE_LENGTH] = MPT_SENSE_BYTES;
    /* Bit 31 marks an address reply, so a context must keep it clear. */
    ioc->Context = (ioc->Context + 1U) & 0x7fffffffU;
    mpt_store32(frame + MPT_MSG_CONTEXT, ioc->Context);
    mpt_store32(frame + MPT_IO_CONTROL,
                DataLength == 0 ? 0 :
                (ToDevice ? MPT_SCSIIO_CONTROL_WRITE :
                            MPT_SCSIIO_CONTROL_READ));
    for (i = 0; i < CdbLength; i++) {
        frame[MPT_IO_CDB + i] = Cdb[i];
    }
    mpt_store32(frame + MPT_IO_DATA_LENGTH, DataLength);
    mpt_store32(frame + MPT_IO_SENSE_ADDRESS, sense);
    flags = MPT_SGE_SIMPLE_ELEMENT | MPT_SGE_LAST_ELEMENT |
            MPT_SGE_END_OF_BUFFER | MPT_SGE_END_OF_LIST |
            MPT_SGE_64_BIT_ADDRESSING |
            (ToDevice ? MPT_SGE_HOST_TO_IOC : 0);
    mpt_store32(frame + MPT_IO_SGL,
                (flags << MPT_SGE_FLAGS_SHIFT) | DataLength);
    mpt_store32(frame + MPT_IO_SGL + 4, (UINT32)address);
    mpt_store32(frame + MPT_IO_SGL + 8, (UINT32)((UINT64)address >> 32));

    if (!mpt_post_and_wait(ioc, Ioc, ioc->Context, MPT_COMMAND_TIMEOUT_US,
                           &reply)) {
        /* The controller may still own the frame: drive it no further. */
        ioc->Present = 0;
        return MptResultTimeout;
    }
    if (reply == NULL) {
        if (ScsiStatus != NULL) {
            *ScsiStatus = 0;
        }
        return MptResultGood;
    }
    status = mpt_load16(reply + MPT_REPLY_IOC_STATUS) & MPT_IOCSTATUS_MASK;
    scsi_status = reply[MPT_IO_REPLY_SCSI_STATUS];
    if (ScsiStatus != NULL) {
        *ScsiStatus = scsi_status;
    }
    mpt_release_reply(ioc, reply);
    switch (status) {
    case MPT_IOCSTATUS_SUCCESS:
    case MPT_IOCSTATUS_SCSI_RECOVERED:
    case MPT_IOCSTATUS_SCSI_UNDERRUN:
        return scsi_status == 0 ? MptResultGood : MptResultTargetStatus;
    case MPT_IOCSTATUS_SCSI_INVALID_BUS:
    case MPT_IOCSTATUS_SCSI_INVALID_TARGETID:
    case MPT_IOCSTATUS_SCSI_NOT_THERE:
        return MptResultNoDevice;
    default:
        return MptResultError;
    }
}

BOOLEAN mpt_command(UINTN Ioc, UINT8 Target, const UINT8 *Cdb,
                    UINTN CdbLength, UINT8 *Data, UINT32 DataLength,
                    BOOLEAN ToDevice, UINT8 *ScsiStatus)
{
    MPT_RESULT result = mpt_execute(Ioc, Target, Cdb, CdbLength, Data,
                                    DataLength, ToDevice, ScsiStatus);

    return result == MptResultGood || result == MptResultTargetStatus;
}

void mpt_stop_all(void)
{
    UINTN i;

    for (i = 0; i < mMptIocCount; i++) {
        if (mMptIocs[i].MmioBase != 0) {
            (void)mpt_make_ready(&mMptIocs[i]);
        }
    }
}
