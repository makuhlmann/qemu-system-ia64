/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * EFI 1.10 compatibility protocols for platform I/O devices.
 */

#include "fw-services.h"
#include "fw-legacy-io.h"
#include "fw-uart.h"

/* ------------------------------------------------------------------------- */
/* Device I/O protocol                                                       */

typedef enum {
    EfiIoWidthUint8,
    EfiIoWidthUint16,
    EfiIoWidthUint32,
    EfiIoWidthUint64,
    EfiIoWidthMaximum
} EFI_IO_WIDTH;

typedef enum {
    EfiBusMasterRead,
    EfiBusMasterWrite,
    EfiBusMasterCommonBuffer,
    EfiBusMasterOperationMaximum
} EFI_IO_OPERATION_TYPE;

typedef struct _EFI_DEVICE_IO_INTERFACE EFI_DEVICE_IO_INTERFACE;

typedef EFI_STATUS (*EFI_DEVICE_IO_ACCESS)(EFI_DEVICE_IO_INTERFACE *This,
                                           EFI_IO_WIDTH Width,
                                           UINT64 Address, UINTN Count,
                                           VOID *Buffer);

typedef struct {
    EFI_DEVICE_IO_ACCESS Read;
    EFI_DEVICE_IO_ACCESS Write;
} EFI_DEVICE_IO_ACCESS_PAIR;

struct _EFI_DEVICE_IO_INTERFACE {
    EFI_DEVICE_IO_ACCESS_PAIR Mem;
    EFI_DEVICE_IO_ACCESS_PAIR Io;
    EFI_DEVICE_IO_ACCESS_PAIR Pci;
    EFI_STATUS (*Map)(EFI_DEVICE_IO_INTERFACE *This,
                      EFI_IO_OPERATION_TYPE Operation,
                      EFI_PHYSICAL_ADDRESS *HostAddress,
                      UINTN *NumberOfBytes,
                      EFI_PHYSICAL_ADDRESS *DeviceAddress,
                      VOID **Mapping);
    EFI_STATUS (*PciDevicePath)(EFI_DEVICE_IO_INTERFACE *This,
                                UINT64 PciAddress,
                                FW_DEVICE_PATH_NODE **PciDevicePath);
    EFI_STATUS (*Unmap)(EFI_DEVICE_IO_INTERFACE *This, VOID *Mapping);
    EFI_STATUS (*AllocateBuffer)(EFI_DEVICE_IO_INTERFACE *This,
                                 EFI_ALLOCATE_TYPE Type,
                                 EFI_MEMORY_TYPE MemoryType, UINTN Pages,
                                 EFI_PHYSICAL_ADDRESS *HostAddress);
    EFI_STATUS (*Flush)(EFI_DEVICE_IO_INTERFACE *This);
    EFI_STATUS (*FreeBuffer)(EFI_DEVICE_IO_INTERFACE *This, UINTN Pages,
                             EFI_PHYSICAL_ADDRESS HostAddress);
};

static const UINT8 mDeviceIoProtocolGuid[16] = {
    0x11, 0xc3, 0x6a, 0xaf, 0xc3, 0x84, 0xd2, 0x11,
    0x8e, 0x3c, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b
};

static EFI_DEVICE_IO_INTERFACE mDeviceIoProtocol;

static BOOLEAN device_io_valid(EFI_DEVICE_IO_INTERFACE *This,
                               EFI_IO_WIDTH Width)
{
    return This == &mDeviceIoProtocol && Width < EfiIoWidthMaximum;
}

static EFI_STATUS device_io_mem_read(EFI_DEVICE_IO_INTERFACE *This,
                                     EFI_IO_WIDTH Width, UINT64 Address,
                                     UINTN Count, VOID *Buffer)
{
    if (!device_io_valid(This, Width)) {
        return EFI_INVALID_PARAMETER;
    }
    return fw_pci_root_read(FwPciRootMemory, Width, Address, Count, Buffer);
}

static EFI_STATUS device_io_mem_write(EFI_DEVICE_IO_INTERFACE *This,
                                      EFI_IO_WIDTH Width, UINT64 Address,
                                      UINTN Count, VOID *Buffer)
{
    if (!device_io_valid(This, Width)) {
        return EFI_INVALID_PARAMETER;
    }
    return fw_pci_root_write(FwPciRootMemory, Width, Address, Count, Buffer);
}

static EFI_STATUS device_io_io_read(EFI_DEVICE_IO_INTERFACE *This,
                                    EFI_IO_WIDTH Width, UINT64 Address,
                                    UINTN Count, VOID *Buffer)
{
    if (!device_io_valid(This, Width)) {
        return EFI_INVALID_PARAMETER;
    }
    return fw_pci_root_read(FwPciRootIo, Width, Address, Count, Buffer);
}

static EFI_STATUS device_io_io_write(EFI_DEVICE_IO_INTERFACE *This,
                                     EFI_IO_WIDTH Width, UINT64 Address,
                                     UINTN Count, VOID *Buffer)
{
    if (!device_io_valid(This, Width)) {
        return EFI_INVALID_PARAMETER;
    }
    return fw_pci_root_write(FwPciRootIo, Width, Address, Count, Buffer);
}

static EFI_STATUS device_io_pci_read(EFI_DEVICE_IO_INTERFACE *This,
                                     EFI_IO_WIDTH Width, UINT64 Address,
                                     UINTN Count, VOID *Buffer)
{
    if (!device_io_valid(This, Width)) {
        return EFI_INVALID_PARAMETER;
    }
    return fw_pci_root_read(FwPciRootConfiguration, Width, Address, Count,
                            Buffer);
}

static EFI_STATUS device_io_pci_write(EFI_DEVICE_IO_INTERFACE *This,
                                      EFI_IO_WIDTH Width, UINT64 Address,
                                      UINTN Count, VOID *Buffer)
{
    if (!device_io_valid(This, Width)) {
        return EFI_INVALID_PARAMETER;
    }
    return fw_pci_root_write(FwPciRootConfiguration, Width, Address, Count,
                             Buffer);
}

static EFI_STATUS device_io_map(EFI_DEVICE_IO_INTERFACE *This,
                                EFI_IO_OPERATION_TYPE Operation,
                                EFI_PHYSICAL_ADDRESS *HostAddress,
                                UINTN *NumberOfBytes,
                                EFI_PHYSICAL_ADDRESS *DeviceAddress,
                                VOID **Mapping)
{
    if (This != &mDeviceIoProtocol || HostAddress == NULL ||
        Operation >= EfiBusMasterOperationMaximum) {
        return EFI_INVALID_PARAMETER;
    }
    return fw_pci_root_map(Operation, (VOID *)(UINTN)*HostAddress,
                           NumberOfBytes, DeviceAddress, Mapping);
}

static EFI_STATUS device_io_pci_device_path(
    EFI_DEVICE_IO_INTERFACE *This, UINT64 PciAddress,
    FW_DEVICE_PATH_NODE **PciDevicePath)
{
    UINT8 function = (UINT8)(PciAddress >> 8);
    UINT8 device = (UINT8)(PciAddress >> 16);
    UINT8 bus = (UINT8)(PciAddress >> 24);
    UINT8 segment = (UINT8)(PciAddress >> 32);

    if (This != &mDeviceIoProtocol || PciDevicePath == NULL) {
        return EFI_INVALID_PARAMETER;
    }
    *PciDevicePath = NULL;
    if ((PciAddress >> 40) != 0 || segment != 0) {
        return EFI_UNSUPPORTED;
    }
    return fw_pci_copy_device_path(bus, device, function, PciDevicePath);
}

static EFI_STATUS device_io_unmap(EFI_DEVICE_IO_INTERFACE *This,
                                  VOID *Mapping)
{
    if (This != &mDeviceIoProtocol) {
        return EFI_INVALID_PARAMETER;
    }
    return fw_pci_root_unmap(Mapping);
}

static EFI_STATUS device_io_allocate_buffer(
    EFI_DEVICE_IO_INTERFACE *This, EFI_ALLOCATE_TYPE Type,
    EFI_MEMORY_TYPE MemoryType, UINTN Pages,
    EFI_PHYSICAL_ADDRESS *HostAddress)
{
    VOID *address;
    EFI_STATUS st;

    if (This != &mDeviceIoProtocol || HostAddress == NULL) {
        return EFI_INVALID_PARAMETER;
    }
    address = Type == AllocateAnyPages ? NULL : (VOID *)(UINTN)*HostAddress;
    st = fw_pci_root_allocate_buffer(Type, MemoryType, Pages, &address);
    if (st == EFI_SUCCESS) {
        *HostAddress = (EFI_PHYSICAL_ADDRESS)(UINTN)address;
    }
    return st;
}

static EFI_STATUS device_io_flush(EFI_DEVICE_IO_INTERFACE *This)
{
    if (This != &mDeviceIoProtocol) {
        return EFI_INVALID_PARAMETER;
    }
    return fw_pci_root_flush();
}

static EFI_STATUS device_io_free_buffer(EFI_DEVICE_IO_INTERFACE *This,
                                        UINTN Pages,
                                        EFI_PHYSICAL_ADDRESS HostAddress)
{
    if (This != &mDeviceIoProtocol) {
        return EFI_INVALID_PARAMETER;
    }
    return fw_pci_root_free_buffer(Pages, (VOID *)(UINTN)HostAddress);
}

static BOOLEAN device_io_install(VOID)
{
    EFI_HANDLE handle = fw_pci_root_handle();

    mDeviceIoProtocol.Mem.Read = device_io_mem_read;
    mDeviceIoProtocol.Mem.Write = device_io_mem_write;
    mDeviceIoProtocol.Io.Read = device_io_io_read;
    mDeviceIoProtocol.Io.Write = device_io_io_write;
    mDeviceIoProtocol.Pci.Read = device_io_pci_read;
    mDeviceIoProtocol.Pci.Write = device_io_pci_write;
    mDeviceIoProtocol.Map = device_io_map;
    mDeviceIoProtocol.PciDevicePath = device_io_pci_device_path;
    mDeviceIoProtocol.Unmap = device_io_unmap;
    mDeviceIoProtocol.AllocateBuffer = device_io_allocate_buffer;
    mDeviceIoProtocol.Flush = device_io_flush;
    mDeviceIoProtocol.FreeBuffer = device_io_free_buffer;
    return bs_install_protocol(&handle, (VOID *)mDeviceIoProtocolGuid, 0,
                               &mDeviceIoProtocol) == EFI_SUCCESS;
}

/* ------------------------------------------------------------------------- */
/* Serial I/O protocol                                                       */

typedef struct {
    UINT32 ControlMask;
    UINT32 Timeout;
    UINT64 BaudRate;
    UINT32 ReceiveFifoDepth;
    UINT32 DataBits;
    UINT32 Parity;
    UINT32 StopBits;
} EFI_SERIAL_IO_MODE;

typedef struct _EFI_SERIAL_IO_PROTOCOL EFI_SERIAL_IO_PROTOCOL;

struct _EFI_SERIAL_IO_PROTOCOL {
    UINT32 Revision;
    EFI_STATUS (*Reset)(EFI_SERIAL_IO_PROTOCOL *This);
    EFI_STATUS (*SetAttributes)(EFI_SERIAL_IO_PROTOCOL *This,
                                UINT64 BaudRate, UINT32 ReceiveFifoDepth,
                                UINT32 Timeout, EFI_PARITY_TYPE Parity,
                                UINT8 DataBits, EFI_STOP_BITS_TYPE StopBits);
    EFI_STATUS (*SetControl)(EFI_SERIAL_IO_PROTOCOL *This, UINT32 Control);
    EFI_STATUS (*GetControl)(EFI_SERIAL_IO_PROTOCOL *This, UINT32 *Control);
    EFI_STATUS (*Write)(EFI_SERIAL_IO_PROTOCOL *This, UINTN *BufferSize,
                        VOID *Buffer);
    EFI_STATUS (*Read)(EFI_SERIAL_IO_PROTOCOL *This, UINTN *BufferSize,
                       VOID *Buffer);
    EFI_SERIAL_IO_MODE *Mode;
};

#define EFI_SERIAL_DATA_TERMINAL_READY          0x0001U
#define EFI_SERIAL_REQUEST_TO_SEND              0x0002U
#define EFI_SERIAL_CLEAR_TO_SEND                0x0010U
#define EFI_SERIAL_DATA_SET_READY               0x0020U
#define EFI_SERIAL_RING_INDICATE                0x0040U
#define EFI_SERIAL_CARRIER_DETECT               0x0080U
#define EFI_SERIAL_INPUT_BUFFER_EMPTY           0x0100U
#define EFI_SERIAL_OUTPUT_BUFFER_EMPTY          0x0200U
#define EFI_SERIAL_HARDWARE_LOOPBACK_ENABLE     0x1000U
#define EFI_SERIAL_SOFTWARE_LOOPBACK_ENABLE     0x2000U
#define EFI_SERIAL_HARDWARE_FLOW_CONTROL_ENABLE 0x4000U

static const UINT8 mSerialIoProtocolGuid[16] = {
    0x6f, 0xcf, 0x25, 0xbb, 0xd4, 0xf1, 0xd2, 0x11,
    0x9a, 0x0c, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0xfd
};

static EFI_HANDLE mSerialHandle;
static EFI_SERIAL_IO_PROTOCOL mSerialIoProtocol;
static EFI_SERIAL_IO_MODE mSerialIoMode;
static UINT32 mSerialControl;
static UINT8 mSerialLoopback[256];
static UINTN mSerialLoopbackRead;
static UINTN mSerialLoopbackWrite;
static UINTN mSerialLoopbackCount;
static FW_SERIAL_DEVICE_PATH FW_DEVICE_PATH_GUEST_ALIGN mSerialDevicePath = {
    .Acpi = {
        .Header = { 0x02, 0x01, sizeof(FW_ACPI_HID_DEVICE_PATH_NODE) },
        .Hid = FW_UART_DEVICE_PATH_HID_PNP0501,
        .Uid = 0,
    },
    .Uart = {
        .Header = { 0x03, 0x0e, sizeof(FW_UART_DEVICE_PATH_NODE) },
        .Reserved = 0,
        .BaudRate = 115200,
        .DataBits = 8,
        .Parity = NoParity,
        .StopBits = OneStopBit,
    },
    .End = { 0x7f, 0xff, sizeof(FW_DEVICE_PATH_NODE) },
};

static BOOLEAN serial_protocol_valid(EFI_SERIAL_IO_PROTOCOL *This)
{
    return This == &mSerialIoProtocol;
}

static UINT64 serial_timeout_ticks(VOID)
{
    UINT64 timeout = mSerialIoMode.Timeout;

    if (timeout > ~0ULL / FW_ITC_TICKS_PER_MICROSECOND) {
        return ~0ULL;
    }
    return timeout * FW_ITC_TICKS_PER_MICROSECOND;
}

static BOOLEAN serial_wait_mask(UINTN Register, UINT8 Mask, BOOLEAN Set)
{
    UINT64 start = fw_read_itc();
    UINT64 timeout = serial_timeout_ticks();

    do {
        BOOLEAN current = (*fw_uart_reg(Register) & Mask) != 0;

        if (current == Set) {
            return 1;
        }
    } while (fw_read_itc() - start < timeout);
    return 0;
}

static EFI_STATUS serial_set_attributes(EFI_SERIAL_IO_PROTOCOL *This,
                                        UINT64 BaudRate,
                                        UINT32 ReceiveFifoDepth,
                                        UINT32 Timeout,
                                        EFI_PARITY_TYPE Parity,
                                        UINT8 DataBits,
                                        EFI_STOP_BITS_TYPE StopBits)
{
    UINT64 divisor;
    UINT64 actual_baud;
    UINT8 lcr;
    UINT32 fifo;

    if (!serial_protocol_valid(This) || Parity > SpaceParity ||
        StopBits > TwoStopBits) {
        return EFI_INVALID_PARAMETER;
    }
    BaudRate = BaudRate == 0 ? 115200U : BaudRate;
    ReceiveFifoDepth = ReceiveFifoDepth == 0 ? 1U : ReceiveFifoDepth;
    Timeout = Timeout == 0 ? 1000000U : Timeout;
    Parity = Parity == DefaultParity ? NoParity : Parity;
    DataBits = DataBits == 0 ? 8U : DataBits;
    StopBits = StopBits == DefaultStopBits ? OneStopBit : StopBits;
    if (BaudRate < 2U || DataBits < 5U || DataBits > 8U ||
        (StopBits == OneFiveStopBits && DataBits != 5U) ||
        (StopBits == TwoStopBits && DataBits == 5U)) {
        return EFI_INVALID_PARAMETER;
    }

    divisor = 115200U / BaudRate;
    if (115200U % BaudRate != 0) {
        divisor++;
    }
    if (divisor == 0) {
        divisor = 1;
    }
    if (divisor > 0xffffU) {
        return EFI_INVALID_PARAMETER;
    }
    actual_baud = 115200U / divisor;
    fifo = ReceiveFifoDepth >= 16U ? 16U : 1U;

    lcr = (UINT8)(DataBits - 5U);
    if (StopBits != OneStopBit) {
        lcr |= 0x04U;
    }
    switch (Parity) {
    case NoParity:
        break;
    case OddParity:
        lcr |= 0x08U;
        break;
    case EvenParity:
        lcr |= 0x18U;
        break;
    case MarkParity:
        lcr |= 0x28U;
        break;
    case SpaceParity:
        lcr |= 0x38U;
        break;
    default:
        return EFI_INVALID_PARAMETER;
    }

    *fw_uart_reg(UART_LCR) = UART_LCR_DLAB;
    *fw_uart_reg(UART_RBR) = (UINT8)divisor;
    *fw_uart_reg(UART_IER) = (UINT8)(divisor >> 8);
    *fw_uart_reg(UART_LCR) = lcr;
    *fw_uart_reg(UART_FCR) = fifo == 16U ? 0x07U : 0x06U;

    mSerialIoMode.Timeout = Timeout;
    mSerialIoMode.BaudRate = actual_baud;
    mSerialIoMode.ReceiveFifoDepth = fifo;
    mSerialIoMode.DataBits = DataBits;
    mSerialIoMode.Parity = Parity;
    mSerialIoMode.StopBits = StopBits;
    mSerialDevicePath.Uart.BaudRate = actual_baud;
    mSerialDevicePath.Uart.DataBits = DataBits;
    mSerialDevicePath.Uart.Parity = (UINT8)Parity;
    mSerialDevicePath.Uart.StopBits = (UINT8)StopBits;
    return EFI_SUCCESS;
}

static EFI_STATUS serial_set_control(EFI_SERIAL_IO_PROTOCOL *This,
                                     UINT32 Control)
{
    const UINT32 settable = EFI_SERIAL_DATA_TERMINAL_READY |
        EFI_SERIAL_REQUEST_TO_SEND |
        EFI_SERIAL_HARDWARE_LOOPBACK_ENABLE |
        EFI_SERIAL_SOFTWARE_LOOPBACK_ENABLE |
        EFI_SERIAL_HARDWARE_FLOW_CONTROL_ENABLE;
    UINT8 mcr = 0;

    if (!serial_protocol_valid(This)) {
        return EFI_INVALID_PARAMETER;
    }
    if ((Control & ~settable) != 0) {
        return EFI_UNSUPPORTED;
    }
    if ((Control & EFI_SERIAL_DATA_TERMINAL_READY) != 0) {
        mcr |= UART_MCR_DTR;
    }
    if ((Control & EFI_SERIAL_REQUEST_TO_SEND) != 0) {
        mcr |= UART_MCR_RTS;
    }
    if ((Control & EFI_SERIAL_HARDWARE_LOOPBACK_ENABLE) != 0) {
        mcr |= UART_MCR_LOOP;
    }
    *fw_uart_reg(UART_MCR) = mcr;
    mSerialControl = Control;
    if ((Control & EFI_SERIAL_SOFTWARE_LOOPBACK_ENABLE) == 0) {
        mSerialLoopbackRead = 0;
        mSerialLoopbackWrite = 0;
        mSerialLoopbackCount = 0;
    }
    return EFI_SUCCESS;
}

static EFI_STATUS serial_reset(EFI_SERIAL_IO_PROTOCOL *This)
{
    EFI_STATUS st;

    if (!serial_protocol_valid(This)) {
        return EFI_INVALID_PARAMETER;
    }
    *fw_uart_reg(UART_IER) = 0;
    *fw_uart_reg(UART_FCR) = 0x07U;
    st = serial_set_attributes(This, 115200, 1, 1000000,
                               NoParity, 8, OneStopBit);
    if (st != EFI_SUCCESS) {
        return EFI_DEVICE_ERROR;
    }
    return serial_set_control(This, EFI_SERIAL_DATA_TERMINAL_READY |
                              EFI_SERIAL_REQUEST_TO_SEND);
}

static EFI_STATUS serial_get_control(EFI_SERIAL_IO_PROTOCOL *This,
                                     UINT32 *Control)
{
    UINT8 lsr;
    UINT8 msr;
    UINT32 value;

    if (!serial_protocol_valid(This) || Control == NULL) {
        return EFI_INVALID_PARAMETER;
    }
    lsr = *fw_uart_reg(UART_LSR);
    msr = *fw_uart_reg(UART_MSR);
    value = mSerialControl;
    value |= msr & (EFI_SERIAL_CLEAR_TO_SEND |
                    EFI_SERIAL_DATA_SET_READY |
                    EFI_SERIAL_RING_INDICATE |
                    EFI_SERIAL_CARRIER_DETECT);
    if ((lsr & UART_LSR_DR) == 0 && mSerialLoopbackCount == 0) {
        value |= EFI_SERIAL_INPUT_BUFFER_EMPTY;
    }
    if ((lsr & UART_LSR_THRE) != 0) {
        value |= EFI_SERIAL_OUTPUT_BUFFER_EMPTY;
    }
    *Control = value;
    return EFI_SUCCESS;
}

static EFI_STATUS serial_write(EFI_SERIAL_IO_PROTOCOL *This,
                               UINTN *BufferSize, VOID *Buffer)
{
    UINT8 *bytes = (UINT8 *)Buffer;
    UINTN requested;
    UINTN done = 0;

    if (!serial_protocol_valid(This) || BufferSize == NULL ||
        (*BufferSize != 0 && Buffer == NULL)) {
        return EFI_INVALID_PARAMETER;
    }
    requested = *BufferSize;
    while (done < requested) {
        if ((mSerialControl & EFI_SERIAL_SOFTWARE_LOOPBACK_ENABLE) != 0) {
            if (mSerialLoopbackCount == FW_ARRAY_SIZE(mSerialLoopback)) {
                break;
            }
            mSerialLoopback[mSerialLoopbackWrite] = bytes[done];
            mSerialLoopbackWrite = (mSerialLoopbackWrite + 1U) %
                FW_ARRAY_SIZE(mSerialLoopback);
            mSerialLoopbackCount++;
        } else {
            if ((mSerialControl & EFI_SERIAL_HARDWARE_FLOW_CONTROL_ENABLE) !=
                    0 &&
                !serial_wait_mask(UART_MSR, 0x10U, 1)) {
                break;
            }
            if (!serial_wait_mask(UART_LSR, UART_LSR_THRE, 1)) {
                break;
            }
            *fw_uart_reg(UART_THR) = bytes[done];
        }
        done++;
    }
    *BufferSize = done;
    return done == requested ? EFI_SUCCESS : EFI_TIMEOUT;
}

static EFI_STATUS serial_read(EFI_SERIAL_IO_PROTOCOL *This,
                              UINTN *BufferSize, VOID *Buffer)
{
    UINT8 *bytes = (UINT8 *)Buffer;
    UINTN requested;
    UINTN done = 0;

    if (!serial_protocol_valid(This) || BufferSize == NULL ||
        (*BufferSize != 0 && Buffer == NULL)) {
        return EFI_INVALID_PARAMETER;
    }
    requested = *BufferSize;
    while (done < requested) {
        if ((mSerialControl & EFI_SERIAL_SOFTWARE_LOOPBACK_ENABLE) != 0) {
            if (mSerialLoopbackCount == 0) {
                break;
            }
            bytes[done] = mSerialLoopback[mSerialLoopbackRead];
            mSerialLoopbackRead = (mSerialLoopbackRead + 1U) %
                FW_ARRAY_SIZE(mSerialLoopback);
            mSerialLoopbackCount--;
        } else {
            if (!serial_wait_mask(UART_LSR, UART_LSR_DR, 1)) {
                break;
            }
            bytes[done] = *fw_uart_reg(UART_RBR);
        }
        done++;
    }
    *BufferSize = done;
    return done == requested ? EFI_SUCCESS : EFI_TIMEOUT;
}

static BOOLEAN serial_io_install(VOID)
{
    EFI_STATUS st;

    mSerialIoMode.ControlMask = EFI_SERIAL_DATA_TERMINAL_READY |
        EFI_SERIAL_REQUEST_TO_SEND | EFI_SERIAL_CLEAR_TO_SEND |
        EFI_SERIAL_DATA_SET_READY | EFI_SERIAL_RING_INDICATE |
        EFI_SERIAL_CARRIER_DETECT | EFI_SERIAL_INPUT_BUFFER_EMPTY |
        EFI_SERIAL_OUTPUT_BUFFER_EMPTY |
        EFI_SERIAL_HARDWARE_LOOPBACK_ENABLE |
        EFI_SERIAL_SOFTWARE_LOOPBACK_ENABLE |
        EFI_SERIAL_HARDWARE_FLOW_CONTROL_ENABLE;
    mSerialIoProtocol.Revision = 0x00010000U;
    mSerialIoProtocol.Reset = serial_reset;
    mSerialIoProtocol.SetAttributes = serial_set_attributes;
    mSerialIoProtocol.SetControl = serial_set_control;
    mSerialIoProtocol.GetControl = serial_get_control;
    mSerialIoProtocol.Write = serial_write;
    mSerialIoProtocol.Read = serial_read;
    mSerialIoProtocol.Mode = &mSerialIoMode;
    if (serial_reset(&mSerialIoProtocol) != EFI_SUCCESS) {
        return 0;
    }
    mSerialHandle = NULL;
    st = bs_install_protocol(&mSerialHandle, (VOID *)mSerialIoProtocolGuid,
                             0, &mSerialIoProtocol);
    if (st != EFI_SUCCESS) {
        return 0;
    }
    st = bs_install_protocol(&mSerialHandle, (VOID *)mDevicePathProtocolGuid,
                             0, &mSerialDevicePath);
    if (st != EFI_SUCCESS) {
        (void)bs_uninstall_protocol(mSerialHandle,
                                    (VOID *)mSerialIoProtocolGuid,
                                    &mSerialIoProtocol);
        mSerialHandle = NULL;
        return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------------- */
/* SCSI pass-through protocol                                                */

typedef struct {
    UINT64 Timeout;
    VOID *DataBuffer;
    VOID *SenseData;
    VOID *Cdb;
    UINT32 TransferLength;
    UINT8 CdbLength;
    UINT8 DataDirection;
    UINT8 HostAdapterStatus;
    UINT8 TargetStatus;
    UINT8 SenseDataLength;
} EFI_SCSI_PASS_THRU_SCSI_REQUEST_PACKET;

typedef struct {
    CHAR16 *ControllerName;
    CHAR16 *ChannelName;
    UINT32 AdapterId;
    UINT32 Attributes;
    UINT32 IoAlign;
} EFI_SCSI_PASS_THRU_MODE;

typedef struct _EFI_SCSI_PASS_THRU_PROTOCOL EFI_SCSI_PASS_THRU_PROTOCOL;

struct _EFI_SCSI_PASS_THRU_PROTOCOL {
    EFI_SCSI_PASS_THRU_MODE *Mode;
    EFI_STATUS (*PassThru)(EFI_SCSI_PASS_THRU_PROTOCOL *This,
                           UINT32 Target, UINT64 Lun,
                           EFI_SCSI_PASS_THRU_SCSI_REQUEST_PACKET *Packet,
                           EFI_EVENT Event);
    EFI_STATUS (*GetNextDevice)(EFI_SCSI_PASS_THRU_PROTOCOL *This,
                                UINT32 *Target, UINT64 *Lun);
    EFI_STATUS (*BuildDevicePath)(EFI_SCSI_PASS_THRU_PROTOCOL *This,
                                  UINT32 Target, UINT64 Lun,
                                  FW_DEVICE_PATH_NODE **DevicePath);
    EFI_STATUS (*GetTargetLun)(EFI_SCSI_PASS_THRU_PROTOCOL *This,
                               FW_DEVICE_PATH_NODE *DevicePath,
                               UINT32 *Target, UINT64 *Lun);
    EFI_STATUS (*ResetChannel)(EFI_SCSI_PASS_THRU_PROTOCOL *This);
    EFI_STATUS (*ResetTarget)(EFI_SCSI_PASS_THRU_PROTOCOL *This,
                              UINT32 Target, UINT64 Lun);
};

typedef struct {
    FW_DEVICE_PATH_NODE Header;
    UINT16 Pun;
    UINT16 Lun;
} __attribute__((packed)) FW_SCSI_DEVICE_PATH_NODE;

#define EFI_SCSI_STATUS_HOST_ADAPTER_OK                0x00U
#define EFI_SCSI_STATUS_HOST_ADAPTER_TIMEOUT_COMMAND   0x09U
#define EFI_SCSI_STATUS_HOST_ADAPTER_TIMEOUT            0x0bU
#define EFI_SCSI_STATUS_HOST_ADAPTER_REQUEST_SENSE_FAILED 0x10U
#define EFI_SCSI_STATUS_HOST_ADAPTER_SELECTION_TIMEOUT  0x11U
#define EFI_SCSI_STATUS_HOST_ADAPTER_OTHER              0x7fU
#define EFI_SCSI_STATUS_TARGET_GOOD                      0x00U
#define EFI_SCSI_STATUS_TARGET_CHECK_CONDITION           0x02U
#define EFI_SCSI_PASS_THRU_ATTRIBUTES_PHYSICAL          0x0001U
#define EFI_SCSI_PASS_THRU_ATTRIBUTES_LOGICAL           0x0002U

static const UINT8 mScsiPassThruProtocolGuid[16] = {
    0xcf, 0x8f, 0x9e, 0xa5, 0xa0, 0xbd, 0xbb, 0x43,
    0x90, 0xb1, 0xd3, 0x73, 0x2e, 0xca, 0xa8, 0x77
};

static EFI_SCSI_PASS_THRU_PROTOCOL mScsiPassThruProtocol;
static EFI_SCSI_PASS_THRU_MODE mScsiPassThruMode;
static BOOLEAN mScsiPassThruBusy;
static CHAR16 mScsiControllerName[] = {
    'P', 'C', 'I', ' ', 'S', 'C', 'S', 'I', ' ',
    'C', 'o', 'n', 't', 'r', 'o', 'l', 'l', 'e', 'r', 0
};
static CHAR16 mScsiChannelName[] = {
    'S', 'C', 'S', 'I', ' ', 'C', 'h', 'a', 'n', 'n', 'e', 'l', ' ', '0', 0
};

static BOOLEAN scsi_pass_thru_valid(EFI_SCSI_PASS_THRU_PROTOCOL *This)
{
    return This == &mScsiPassThruProtocol && fw_scsi_controller_present();
}

static BOOLEAN scsi_target_valid(UINT32 Target, UINT64 Lun)
{
    return Target < FW_SCSI_DEVICE_MAX && Target != FW_SCSI_HOST_ID &&
           Lun == 0;
}

static EFI_STATUS scsi_pass_thru(EFI_SCSI_PASS_THRU_PROTOCOL *This,
                                 UINT32 Target, UINT64 Lun,
                                 EFI_SCSI_PASS_THRU_SCSI_REQUEST_PACKET *Packet,
                                 EFI_EVENT Event)
{
    UINT8 target_status = 0xff;
    FW_LSI_SCRIPT_RESULT result;
    UINT32 transfer_length;

    (void)Event;
    if (!scsi_pass_thru_valid(This) || !scsi_target_valid(Target, Lun) ||
        Packet == NULL || Packet->Cdb == NULL || Packet->CdbLength == 0 ||
        Packet->CdbLength > FW_SCSI_CDB_MAX || Packet->DataDirection > 1 ||
        (Packet->TransferLength != 0 && Packet->DataBuffer == NULL) ||
        (Packet->SenseDataLength != 0 && Packet->SenseData == NULL)) {
        return EFI_INVALID_PARAMETER;
    }
    if (Packet->TransferLength > FW_SCSI_BOUNCE_SIZE) {
        Packet->TransferLength = FW_SCSI_BOUNCE_SIZE;
        return EFI_BAD_BUFFER_SIZE;
    }
    if (mScsiPassThruBusy) {
        return EFI_NOT_READY;
    }
    mScsiPassThruBusy = 1;
    transfer_length = Packet->TransferLength;
    Packet->HostAdapterStatus = EFI_SCSI_STATUS_HOST_ADAPTER_OK;
    Packet->TargetStatus = 0xff;
    result = fw_scsi_execute_buffered(
        (UINT8)Target, Packet->Cdb, Packet->CdbLength,
        Packet->DataBuffer, transfer_length, Packet->DataDirection == 1,
        Packet->Timeout, &target_status);
    Packet->TargetStatus = target_status;

    if (result == FwLsiScriptTargetStatus && target_status ==
        EFI_SCSI_STATUS_TARGET_CHECK_CONDITION &&
        Packet->SenseData != NULL && Packet->SenseDataLength != 0) {
        UINT8 requested = Packet->SenseDataLength;
        UINT8 sense_status = 0xff;
        FW_LSI_SCRIPT_RESULT sense_result;

        UINT8 sense_cdb[6] = { 0x03U, 0, 0, 0, requested, 0 };

        sense_result = fw_scsi_execute_buffered(
            (UINT8)Target, sense_cdb, sizeof(sense_cdb), Packet->SenseData,
            requested, 0, Packet->Timeout, &sense_status);
        if (sense_result == FwLsiScriptSuccess) {
            Packet->SenseDataLength = requested;
        } else {
            Packet->SenseDataLength = 0;
            Packet->HostAdapterStatus =
                EFI_SCSI_STATUS_HOST_ADAPTER_REQUEST_SENSE_FAILED;
            result = FwLsiScriptDeviceError;
        }
    } else {
        Packet->SenseDataLength = 0;
    }
    mScsiPassThruBusy = 0;

    if (result == FwLsiScriptSelectionTimeout) {
        Packet->HostAdapterStatus =
            EFI_SCSI_STATUS_HOST_ADAPTER_SELECTION_TIMEOUT;
        Packet->TransferLength = 0;
        return EFI_TIMEOUT;
    }
    if (result == FwLsiScriptCommandTimeout) {
        Packet->HostAdapterStatus =
            EFI_SCSI_STATUS_HOST_ADAPTER_TIMEOUT_COMMAND;
        Packet->TransferLength = 0;
        return EFI_TIMEOUT;
    }
    if (result == FwLsiScriptDeviceError) {
        if (Packet->HostAdapterStatus == EFI_SCSI_STATUS_HOST_ADAPTER_OK) {
            Packet->HostAdapterStatus = EFI_SCSI_STATUS_HOST_ADAPTER_OTHER;
        }
        Packet->TransferLength = 0;
        return EFI_DEVICE_ERROR;
    }
    Packet->TransferLength = transfer_length;
    return EFI_SUCCESS;
}

static EFI_STATUS scsi_get_next_device(EFI_SCSI_PASS_THRU_PROTOCOL *This,
                                       UINT32 *Target, UINT64 *Lun)
{
    UINTN start;
    UINTN i;

    if (!scsi_pass_thru_valid(This) || Target == NULL || Lun == NULL) {
        return EFI_INVALID_PARAMETER;
    }
    if (*Target == 0xffffffffU) {
        start = 0;
    } else {
        if (*Target >= FW_SCSI_DEVICE_MAX || *Lun != 0 ||
            !fw_scsi_device_present(*Target)) {
            return EFI_INVALID_PARAMETER;
        }
        start = (UINTN)*Target + 1U;
    }
    for (i = start; i < FW_SCSI_DEVICE_MAX; i++) {
        if (fw_scsi_device_present(i)) {
            *Target = (UINT32)i;
            *Lun = 0;
            return EFI_SUCCESS;
        }
    }
    return EFI_NOT_FOUND;
}

static EFI_STATUS scsi_build_device_path(EFI_SCSI_PASS_THRU_PROTOCOL *This,
                                         UINT32 Target, UINT64 Lun,
                                         FW_DEVICE_PATH_NODE **DevicePath)
{
    FW_SCSI_DEVICE_PATH_NODE *node;
    EFI_STATUS st;

    if (!scsi_pass_thru_valid(This) || DevicePath == NULL) {
        return EFI_INVALID_PARAMETER;
    }
    *DevicePath = NULL;
    if (!scsi_target_valid(Target, Lun) ||
        !fw_scsi_device_present(Target)) {
        return EFI_NOT_FOUND;
    }
    st = bs_allocate_pool(EfiBootServicesData, sizeof(*node),
                          (VOID **)&node);
    if (st != EFI_SUCCESS) {
        return EFI_OUT_OF_RESOURCES;
    }
    node->Header.Type = 0x03;
    node->Header.SubType = 0x02;
    node->Header.Length = sizeof(*node);
    node->Pun = (UINT16)Target;
    node->Lun = (UINT16)Lun;
    *DevicePath = &node->Header;
    return EFI_SUCCESS;
}

static EFI_STATUS scsi_get_target_lun(EFI_SCSI_PASS_THRU_PROTOCOL *This,
                                      FW_DEVICE_PATH_NODE *DevicePath,
                                      UINT32 *Target, UINT64 *Lun)
{
    FW_SCSI_DEVICE_PATH_NODE *node =
        (FW_SCSI_DEVICE_PATH_NODE *)DevicePath;

    if (!scsi_pass_thru_valid(This) || DevicePath == NULL ||
        Target == NULL || Lun == NULL) {
        return EFI_INVALID_PARAMETER;
    }
    if (node->Header.Type != 0x03 || node->Header.SubType != 0x02 ||
        node->Header.Length != sizeof(*node)) {
        return EFI_UNSUPPORTED;
    }
    if (!scsi_target_valid(node->Pun, node->Lun) ||
        !fw_scsi_device_present(node->Pun)) {
        return EFI_NOT_FOUND;
    }
    *Target = node->Pun;
    *Lun = node->Lun;
    return EFI_SUCCESS;
}

static EFI_STATUS scsi_reset_channel(EFI_SCSI_PASS_THRU_PROTOCOL *This)
{
    if (!scsi_pass_thru_valid(This)) {
        return EFI_INVALID_PARAMETER;
    }
    if (mScsiPassThruBusy) {
        return EFI_DEVICE_ERROR;
    }
    return fw_scsi_reset_channel();
}

static EFI_STATUS scsi_reset_target(EFI_SCSI_PASS_THRU_PROTOCOL *This,
                                    UINT32 Target, UINT64 Lun)
{
    FW_LSI_SCRIPT_RESULT result;

    if (!scsi_pass_thru_valid(This) ||
        !scsi_target_valid(Target, Lun)) {
        return EFI_INVALID_PARAMETER;
    }
    if (mScsiPassThruBusy) {
        return EFI_DEVICE_ERROR;
    }
    mScsiPassThruBusy = 1;
    result = fw_scsi_reset_target((UINT8)Target, 100000000ULL);
    mScsiPassThruBusy = 0;
    if (result == FwLsiScriptSuccess) {
        return EFI_SUCCESS;
    }
    if (result == FwLsiScriptSelectionTimeout ||
        result == FwLsiScriptCommandTimeout) {
        return EFI_TIMEOUT;
    }
    return EFI_DEVICE_ERROR;
}

static BOOLEAN scsi_pass_thru_install(VOID)
{
    EFI_HANDLE handle = fw_scsi_controller_handle();

    if (!fw_scsi_controller_present()) {
        return 1;
    }
    mScsiPassThruMode.ControllerName = mScsiControllerName;
    mScsiPassThruMode.ChannelName = mScsiChannelName;
    mScsiPassThruMode.AdapterId = FW_SCSI_HOST_ID;
    mScsiPassThruMode.Attributes =
        EFI_SCSI_PASS_THRU_ATTRIBUTES_PHYSICAL |
        EFI_SCSI_PASS_THRU_ATTRIBUTES_LOGICAL;
    mScsiPassThruMode.IoAlign = 1;
    mScsiPassThruProtocol.Mode = &mScsiPassThruMode;
    mScsiPassThruProtocol.PassThru = scsi_pass_thru;
    mScsiPassThruProtocol.GetNextDevice = scsi_get_next_device;
    mScsiPassThruProtocol.BuildDevicePath = scsi_build_device_path;
    mScsiPassThruProtocol.GetTargetLun = scsi_get_target_lun;
    mScsiPassThruProtocol.ResetChannel = scsi_reset_channel;
    mScsiPassThruProtocol.ResetTarget = scsi_reset_target;
    return bs_install_protocol(&handle, (VOID *)mScsiPassThruProtocolGuid,
                               0, &mScsiPassThruProtocol) == EFI_SUCCESS;
}

BOOLEAN fw_legacy_io_protocols_install(VOID)
{
    if (!device_io_install()) {
        return 0;
    }
    if (!serial_io_install()) {
        (void)bs_uninstall_protocol(fw_pci_root_handle(),
                                    (VOID *)mDeviceIoProtocolGuid,
                                    &mDeviceIoProtocol);
        return 0;
    }
    if (!scsi_pass_thru_install()) {
        (void)bs_uninstall_protocol(mSerialHandle,
                                    (VOID *)mDevicePathProtocolGuid,
                                    &mSerialDevicePath);
        (void)bs_uninstall_protocol(mSerialHandle,
                                    (VOID *)mSerialIoProtocolGuid,
                                    &mSerialIoProtocol);
        mSerialHandle = NULL;
        (void)bs_uninstall_protocol(fw_pci_root_handle(),
                                    (VOID *)mDeviceIoProtocolGuid,
                                    &mDeviceIoProtocol);
        return 0;
    }
    return 1;
}

BOOLEAN fw_legacy_io_protocols_selftest(VOID)
{
    UINT32 id = 0;
    UINT32 control = 0;
    UINT8 loop_out[3] = { 0x31, 0x32, 0x33 };
    UINT8 loop_in[3] = { 0, 0, 0 };
    UINTN size;
    FW_DEVICE_PATH_NODE *path = NULL;
    EFI_PHYSICAL_ADDRESS device_address;
    EFI_PHYSICAL_ADDRESS host_address = (EFI_PHYSICAL_ADDRESS)(UINTN)&id;
    VOID *mapping = NULL;
    UINTN bytes = sizeof(id);
    UINT64 pci_address;

    /*
     * Build a device path for a controller this machine has.  Which PCI I/O
     * controllers exist, and on which bus and slot, depends on the board and
     * the machine options (the LSI is opt-in, the IDE and AHCI can be left
     * out), so take the first one present.  Every machine has one.
     */
    if (!fw_pci_io_first_present_address(&pci_address) ||
        mDeviceIoProtocol.Pci.Read(&mDeviceIoProtocol, EfiIoWidthUint32,
                                   0, 1, &id) != EFI_SUCCESS || id == 0 ||
        mDeviceIoProtocol.PciDevicePath(&mDeviceIoProtocol,
                                        pci_address, &path) != EFI_SUCCESS ||
        path == NULL || bs_free_pool(path) != EFI_SUCCESS ||
        mDeviceIoProtocol.Map(&mDeviceIoProtocol, EfiBusMasterRead,
                              &host_address, &bytes,
                              &device_address, &mapping) != EFI_SUCCESS ||
        mapping == NULL ||
        mDeviceIoProtocol.Unmap(&mDeviceIoProtocol, mapping) != EFI_SUCCESS ||
        mDeviceIoProtocol.Unmap(&mDeviceIoProtocol, mapping) !=
            EFI_INVALID_PARAMETER) {
        return 0;
    }

    if (mSerialDevicePath.Acpi.Hid != FW_UART_DEVICE_PATH_HID_PNP0501 ||
        mSerialDevicePath.Acpi.Uid != 0 ||
        mSerialIoProtocol.SetControl(&mSerialIoProtocol,
            EFI_SERIAL_SOFTWARE_LOOPBACK_ENABLE) != EFI_SUCCESS) {
        return 0;
    }
    size = sizeof(loop_out);
    if (mSerialIoProtocol.Write(&mSerialIoProtocol, &size, loop_out) !=
            EFI_SUCCESS || size != sizeof(loop_out)) {
        return 0;
    }
    size = sizeof(loop_in);
    if (mSerialIoProtocol.Read(&mSerialIoProtocol, &size, loop_in) !=
            EFI_SUCCESS || size != sizeof(loop_in) ||
        loop_in[0] != loop_out[0] || loop_in[1] != loop_out[1] ||
        loop_in[2] != loop_out[2] ||
        mSerialIoProtocol.SetControl(&mSerialIoProtocol,
            EFI_SERIAL_DATA_TERMINAL_READY |
            EFI_SERIAL_REQUEST_TO_SEND) != EFI_SUCCESS ||
        mSerialIoProtocol.GetControl(&mSerialIoProtocol, &control) !=
            EFI_SUCCESS ||
        (control & EFI_SERIAL_OUTPUT_BUFFER_EMPTY) == 0) {
        return 0;
    }

    if (fw_scsi_controller_present()) {
        UINT32 target = 0xffffffffU;
        UINT64 lun = 0;
        EFI_STATUS st;

        st = mScsiPassThruProtocol.GetNextDevice(&mScsiPassThruProtocol,
                                                 &target, &lun);
        if (st == EFI_NOT_FOUND) {
            return 1;
        }
        if (st != EFI_SUCCESS ||
            mScsiPassThruProtocol.BuildDevicePath(&mScsiPassThruProtocol,
                                                  target, lun, &path) !=
                EFI_SUCCESS || path == NULL ||
            mScsiPassThruProtocol.GetTargetLun(&mScsiPassThruProtocol,
                                               path, &id, &lun) !=
                EFI_SUCCESS || id != target || lun != 0 ||
            bs_free_pool(path) != EFI_SUCCESS) {
            return 0;
        }
    }
    return 1;
}
