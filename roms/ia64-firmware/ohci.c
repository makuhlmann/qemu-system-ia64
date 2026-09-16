/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * OHCI host-controller driver and the boot-time USB keyboard client.
 * Extracted from firmware.c (Phase 1); the EFI USB protocol layer in
 * usb_protocols.c consumes the exported usb_ohci_* primitives.
 */

#include "fw-base.h"
#include "fw-services.h"
#include "fw-boot-shell.h"
#include "fw-usb.h"
#include "fw-platform-layout.h"

BOOLEAN                       mUsbKeyboardTried;
BOOLEAN                       mUsbKeyboardReady;
BOOLEAN                       mUsbKeyboardLowSpeed;
UINT8                         mUsbKeyboardPort;
FW_OHCI_HCCA                  mUsbOhciHcca __attribute__((aligned(256)));
static FW_OHCI_ED             mUsbOhciControlEd __attribute__((aligned(16)));
static FW_OHCI_ED             mUsbOhciInterruptEd __attribute__((aligned(16)));
static FW_OHCI_TD             mUsbOhciControlTd[4] __attribute__((aligned(16)));
static FW_OHCI_TD             mUsbOhciInterruptTd[2] __attribute__((aligned(16)));
static UINT8                  mUsbOhciSetupPacket[8] __attribute__((aligned(16)));
static UINT8                  mUsbOhciDataBuffer[64] __attribute__((aligned(16)));
static UINT8                  mUsbKeyboardReport[OHCI_USB_KEYBOARD_REPORT_SIZE]
    __attribute__((aligned(16)));
UINT8                         mUsbKeyboardPreviousReport[OHCI_USB_KEYBOARD_REPORT_SIZE];

static volatile UINT32 *usb_ohci_reg(UINTN Offset)
{
    return (volatile UINT32 *)(UINTN)(PCI_OHCI_MMIO_BAR + Offset);
}

UINT32 usb_ohci_read(UINTN Offset)
{
    return *usb_ohci_reg(Offset);
}

void usb_ohci_write(UINTN Offset, UINT32 Value)
{
    *usb_ohci_reg(Offset) = Value;
}

UINT32 usb_ohci_phys(const VOID *Pointer)
{
    return (UINT32)(UINTN)Pointer;
}

void usb_dma_barrier(void)
{
    __asm__ volatile ("" ::: "memory");
}

UINT32 usb_ohci_ed_head(const FW_OHCI_ED *Ed)
{
    return ((volatile const FW_OHCI_ED *)Ed)->Head;
}

UINT32 usb_ohci_ed_tail(const FW_OHCI_ED *Ed)
{
    return ((volatile const FW_OHCI_ED *)Ed)->Tail;
}

static UINT32 usb_ohci_td_flags(UINT32 Direction, BOOLEAN BufferRounding)
{
    UINT32 flags = (OHCI_TD_CC_NOT_ACCESSED << OHCI_TD_CC_SHIFT) |
                   (0U << OHCI_TD_DI_SHIFT) |
                   (Direction << OHCI_TD_DP_SHIFT);

    if (BufferRounding) {
        flags |= OHCI_TD_R;
    }
    return flags;
}

void usb_ohci_init_td(FW_OHCI_TD *Td, UINT32 Direction,
                      VOID *Buffer, UINTN Length, FW_OHCI_TD *Next,
                      BOOLEAN BufferRounding)
{
    Td->Flags = usb_ohci_td_flags(Direction, BufferRounding);
    if (Length != 0) {
        Td->CurrentBufferPointer = usb_ohci_phys(Buffer);
        Td->BufferEnd = Td->CurrentBufferPointer + (UINT32)Length - 1U;
    } else {
        Td->CurrentBufferPointer = 0;
        Td->BufferEnd = 0;
    }
    Td->Next = Next != NULL ? usb_ohci_phys(Next) : 0;
}

UINT32 usb_ohci_td_condition_code(const FW_OHCI_TD *Td)
{
    return ((volatile const FW_OHCI_TD *)Td)->Flags >> OHCI_TD_CC_SHIFT;
}

static BOOLEAN usb_ohci_wait_control_done(FW_OHCI_TD *First,
                                          FW_OHCI_TD *Last)
{
    UINT64 start = fw_read_itc();
    UINT64 timeout = 100000ULL * FW_ITC_TICKS_PER_MICROSECOND;

    while (fw_read_itc() - start < timeout) {
        if ((usb_ohci_ed_head(&mUsbOhciControlEd) & OHCI_DPTR_MASK) ==
            usb_ohci_ed_tail(&mUsbOhciControlEd)) {
            FW_OHCI_TD *td;

            usb_dma_barrier();
            for (td = First; td <= Last; td++) {
                if (usb_ohci_td_condition_code(td) != OHCI_TD_CC_NOERROR) {
                    return 0;
                }
            }
            return 1;
        }
    }
    return 0;
}

static BOOLEAN usb_ohci_control_transfer(UINT8 DeviceAddress,
                                         UINT8 RequestType,
                                         UINT8 Request,
                                         UINT16 Value,
                                         UINT16 Index,
                                         UINT16 Length,
                                         BOOLEAN DataIn)
{
    FW_OHCI_TD *setup_td = &mUsbOhciControlTd[0];
    FW_OHCI_TD *data_td = NULL;
    FW_OHCI_TD *status_td;
    FW_OHCI_TD *dummy_td;
    UINTN td_index = 1;
    UINT32 flags;

    if (Length > sizeof(mUsbOhciDataBuffer)) {
        return 0;
    }

    mUsbOhciSetupPacket[0] = RequestType;
    mUsbOhciSetupPacket[1] = Request;
    mUsbOhciSetupPacket[2] = (UINT8)Value;
    mUsbOhciSetupPacket[3] = (UINT8)(Value >> 8);
    mUsbOhciSetupPacket[4] = (UINT8)Index;
    mUsbOhciSetupPacket[5] = (UINT8)(Index >> 8);
    mUsbOhciSetupPacket[6] = (UINT8)Length;
    mUsbOhciSetupPacket[7] = (UINT8)(Length >> 8);

    fw_set_mem(mUsbOhciControlTd, sizeof(mUsbOhciControlTd), 0);

    if (Length != 0) {
        data_td = &mUsbOhciControlTd[td_index++];
    }
    status_td = &mUsbOhciControlTd[td_index++];
    dummy_td = &mUsbOhciControlTd[td_index];

    usb_ohci_init_td(setup_td, OHCI_TD_DIR_SETUP, mUsbOhciSetupPacket,
                     sizeof(mUsbOhciSetupPacket),
                     data_td != NULL ? data_td : status_td, 0);
    if (data_td != NULL) {
        usb_ohci_init_td(data_td,
                         DataIn ? OHCI_TD_DIR_IN : OHCI_TD_DIR_OUT,
                         mUsbOhciDataBuffer, Length, status_td, DataIn);
    }
    usb_ohci_init_td(status_td,
                     DataIn ? OHCI_TD_DIR_OUT : OHCI_TD_DIR_IN,
                     NULL, 0, dummy_td, 1);

    flags = (UINT32)DeviceAddress |
            (8U << OHCI_ED_MPS_SHIFT);
    if (mUsbKeyboardLowSpeed) {
        flags |= OHCI_ED_S;
    }
    mUsbOhciControlEd.Flags = flags;
    mUsbOhciControlEd.Tail = usb_ohci_phys(dummy_td);
    mUsbOhciControlEd.Head = usb_ohci_phys(setup_td);
    mUsbOhciControlEd.Next = 0;

    usb_dma_barrier();
    usb_ohci_write(OHCI_REG_CONTROL_HEAD_ED, usb_ohci_phys(&mUsbOhciControlEd));
    usb_ohci_write(OHCI_REG_COMMAND_STATUS, OHCI_STATUS_CLF);

    return usb_ohci_wait_control_done(setup_td, status_td);
}

BOOLEAN usb_ohci_controller_present(void)
{
    UINT32 revision = usb_ohci_read(OHCI_REG_REVISION);

    return revision != 0xffffffffU && (revision & 0xffU) == 0x10U;
}

/*
 * A controller reset and a root-port reset complete within milliseconds; the
 * bound leaves room for a host that stalls the guest.
 */
#define OHCI_RESET_TIMEOUT_US 1000000ULL

BOOLEAN usb_ohci_reset_controller(void)
{
    UINT64 start;

    usb_ohci_write(OHCI_REG_INTERRUPT_DISABLE, 0xffffffffU);
    usb_ohci_write(OHCI_REG_INTERRUPT_STATUS, 0xffffffffU);
    usb_ohci_write(OHCI_REG_COMMAND_STATUS, OHCI_STATUS_HCR);
    start = fw_read_itc();
    for (;;) {
        BOOLEAN expired = fw_wait_expired(start, OHCI_RESET_TIMEOUT_US);

        if ((usb_ohci_read(OHCI_REG_COMMAND_STATUS) & OHCI_STATUS_HCR) == 0) {
            return 1;
        }
        if (expired) {
            return 0;
        }
    }
}

static BOOLEAN usb_ohci_enable_keyboard_port(void)
{
    UINT32 rh_desc;
    UINT32 port_count;
    UINT32 port;

    rh_desc = usb_ohci_read(OHCI_REG_RH_DESCRIPTOR_A);
    port_count = rh_desc & 0xffU;
    if (port_count == 0 || port_count > 15U) {
        return 0;
    }

    usb_ohci_write(OHCI_REG_RH_STATUS, OHCI_RHS_LPSC);
    for (port = 0; port < port_count; port++) {
        UINTN reg = OHCI_REG_RH_PORT_STATUS_BASE + port * 4U;
        UINT32 status = usb_ohci_read(reg);
        UINT64 start;

        usb_ohci_write(reg, OHCI_PORT_WTC | OHCI_PORT_PPS);
        if ((status & OHCI_PORT_CCS) == 0) {
            continue;
        }

        usb_ohci_write(reg, OHCI_PORT_WTC | OHCI_PORT_PRS);
        start = fw_read_itc();
        for (;;) {
            BOOLEAN expired = fw_wait_expired(start, OHCI_RESET_TIMEOUT_US);

            status = usb_ohci_read(reg);
            if ((status & OHCI_PORT_PRS) == 0 &&
                (status & OHCI_PORT_PES) != 0) {
                usb_ohci_write(reg, OHCI_PORT_WTC);
                mUsbKeyboardLowSpeed = (status & OHCI_PORT_LSDA) != 0;
                mUsbKeyboardPort = (UINT8)port;
                return 1;
            }
            if (expired) {
                break;
            }
        }
    }
    return 0;
}

void usb_keyboard_submit_interrupt_td(void)
{
    UINT32 carry = usb_ohci_ed_head(&mUsbOhciInterruptEd) & OHCI_ED_C;

    fw_set_mem(mUsbKeyboardReport, sizeof(mUsbKeyboardReport), 0);
    fw_set_mem(mUsbOhciInterruptTd, sizeof(mUsbOhciInterruptTd), 0);
    usb_ohci_init_td(&mUsbOhciInterruptTd[0], OHCI_TD_DIR_IN,
                     mUsbKeyboardReport, sizeof(mUsbKeyboardReport),
                     &mUsbOhciInterruptTd[1], 1);

    mUsbOhciInterruptEd.Flags =
        OHCI_USB_KEYBOARD_ADDRESS |
        ((UINT32)OHCI_USB_KEYBOARD_ENDPOINT << 7) |
        (OHCI_TD_DIR_IN << OHCI_ED_D_SHIFT) |
        (OHCI_USB_KEYBOARD_REPORT_SIZE << OHCI_ED_MPS_SHIFT);
    if (mUsbKeyboardLowSpeed) {
        mUsbOhciInterruptEd.Flags |= OHCI_ED_S;
    }
    mUsbOhciInterruptEd.Tail = usb_ohci_phys(&mUsbOhciInterruptTd[1]);
    mUsbOhciInterruptEd.Head = usb_ohci_phys(&mUsbOhciInterruptTd[0]) | carry;
    mUsbOhciInterruptEd.Next = 0;
    usb_dma_barrier();
}

BOOLEAN __attribute__((noinline, used)) usb_keyboard_init(void)
{
    UINTN i;

    if (mUsbKeyboardReady) {
        return 1;
    }
    if (mUsbKeyboardTried) {
        return 0;
    }
    mUsbKeyboardTried = 1;
    mUsbKeyboardLowSpeed = 0;
    fw_set_mem(&mUsbOhciHcca, sizeof(mUsbOhciHcca), 0);
    fw_set_mem(&mUsbOhciControlEd, sizeof(mUsbOhciControlEd), 0);
    fw_set_mem(&mUsbOhciInterruptEd, sizeof(mUsbOhciInterruptEd), 0);
    fw_set_mem(mUsbKeyboardPreviousReport,
               sizeof(mUsbKeyboardPreviousReport), 0);

    if (!usb_ohci_controller_present() ||
        !usb_ohci_reset_controller()) {
        return 0;
    }

    usb_ohci_write(OHCI_REG_HCCA, usb_ohci_phys(&mUsbOhciHcca));
    usb_ohci_write(OHCI_REG_PERIODIC_START, 0x2a2fU);
    usb_ohci_write(OHCI_REG_CONTROL,
                   OHCI_USB_OPERATIONAL | OHCI_CTL_CLE | OHCI_CTL_PLE);

    if (!usb_ohci_enable_keyboard_port()) {
        return 0;
    }

    if (!usb_ohci_control_transfer(0, 0, USB_REQ_SET_ADDRESS,
                                   OHCI_USB_KEYBOARD_ADDRESS, 0, 0, 0)) {
        return 0;
    }
    (void)bs_stall(2000);

    if (!usb_ohci_control_transfer(OHCI_USB_KEYBOARD_ADDRESS, 0,
                                   USB_REQ_SET_CONFIGURATION,
                                   1, 0, 0, 0) ||
        !usb_ohci_control_transfer(OHCI_USB_KEYBOARD_ADDRESS,
                                   USB_TYPE_CLASS_INTERFACE_OUT,
                                   USB_REQ_HID_SET_IDLE, 0, 0, 0, 0) ||
        !usb_ohci_control_transfer(OHCI_USB_KEYBOARD_ADDRESS,
                                   USB_TYPE_CLASS_INTERFACE_OUT,
                                   USB_REQ_HID_SET_PROTOCOL, 0, 0, 0, 0)) {
        return 0;
    }

    for (i = 0; i < FW_ARRAY_SIZE(mUsbOhciHcca.InterruptTable); i++) {
        mUsbOhciHcca.InterruptTable[i] =
            usb_ohci_phys(&mUsbOhciInterruptEd);
    }
    usb_dma_barrier();
    mUsbKeyboardReady = 1;
    usb_keyboard_submit_interrupt_td();
    return 1;
}

static BOOLEAN usb_keyboard_report_has_usage(const UINT8 *Report, UINT8 Usage)
{
    UINTN i;

    for (i = 2; i < OHCI_USB_KEYBOARD_REPORT_SIZE; i++) {
        if (Report[i] == Usage) {
            return 1;
        }
    }
    return 0;
}

static CHAR16 usb_keyboard_usage_to_char(UINT8 Usage, BOOLEAN Shift)
{
    static const CHAR8 normal_digits[] = "1234567890";
    static const CHAR8 shifted_digits[] = "!@#$%^&*()";

    if (Usage >= 0x04 && Usage <= 0x1d) {
        CHAR16 ch = (CHAR16)('a' + Usage - 0x04);

        return Shift ? (CHAR16)(ch - ('a' - 'A')) : ch;
    }
    if (Usage >= 0x1e && Usage <= 0x27) {
        UINTN index = Usage - 0x1e;

        return Shift ? (CHAR16)shifted_digits[index] :
                       (CHAR16)normal_digits[index];
    }

    switch (Usage) {
    case 0x28: return '\r';
    case 0x2a: return '\b';
    case 0x2b: return '\t';
    case 0x2c: return ' ';
    case 0x2d: return Shift ? '_' : '-';
    case 0x2e: return Shift ? '+' : '=';
    case 0x2f: return Shift ? '{' : '[';
    case 0x30: return Shift ? '}' : ']';
    case 0x31: return Shift ? '|' : '\\';
    case 0x33: return Shift ? ':' : ';';
    case 0x34: return Shift ? '"' : '\'';
    case 0x35: return Shift ? '~' : '`';
    case 0x36: return Shift ? '<' : ',';
    case 0x37: return Shift ? '>' : '.';
    case 0x38: return Shift ? '?' : '/';
    default: return 0;
    }
}

static UINT16 usb_keyboard_usage_to_scan(UINT8 Usage)
{
    switch (Usage) {
    case 0x29: return EFI_SCAN_ESC;
    case 0x3a: return EFI_SCAN_F1;
    case 0x3b: return EFI_SCAN_F2;
    case 0x3c: return EFI_SCAN_F3;
    case 0x3d: return EFI_SCAN_F4;
    case 0x3e: return EFI_SCAN_F5;
    case 0x3f: return EFI_SCAN_F6;
    case 0x40: return EFI_SCAN_F7;
    case 0x41: return EFI_SCAN_F8;
    case 0x42: return EFI_SCAN_F9;
    case 0x43: return EFI_SCAN_F10;
    case 0x44: return EFI_SCAN_F11;
    case 0x45: return EFI_SCAN_F12;
    case 0x49: return EFI_SCAN_INSERT;
    case 0x4a: return EFI_SCAN_HOME;
    case 0x4b: return EFI_SCAN_PAGE_UP;
    case 0x4c: return EFI_SCAN_DELETE;
    case 0x4d: return EFI_SCAN_END;
    case 0x4e: return EFI_SCAN_PAGE_DOWN;
    case 0x4f: return EFI_SCAN_RIGHT;
    case 0x50: return EFI_SCAN_LEFT;
    case 0x51: return EFI_SCAN_DOWN;
    case 0x52: return EFI_SCAN_UP;
    default: return 0;
    }
}

BOOLEAN usb_keyboard_report_to_key(EFI_INPUT_KEY *Key)
{
    BOOLEAN shift = (mUsbKeyboardReport[0] & ((1U << 1) | (1U << 5))) != 0;
    UINTN i;

    for (i = 2; i < OHCI_USB_KEYBOARD_REPORT_SIZE; i++) {
        UINT8 usage = mUsbKeyboardReport[i];

        if (usage == 0 ||
            usb_keyboard_report_has_usage(mUsbKeyboardPreviousReport, usage)) {
            continue;
        }

        Key->ScanCode = usb_keyboard_usage_to_scan(usage);
        Key->UnicodeChar = 0;
        if (Key->ScanCode == 0) {
            Key->UnicodeChar = usb_keyboard_usage_to_char(usage, shift);
        }
        if (Key->ScanCode != 0 || Key->UnicodeChar != 0) {
            UINT8 modifiers = mUsbKeyboardReport[0];
            UINT32 state = EFI_SHIFT_STATE_VALID;

            if ((modifiers & (1U << 0)) != 0) {
                state |= EFI_LEFT_CONTROL_PRESSED;
            }
            if ((modifiers & (1U << 1)) != 0) {
                state |= EFI_LEFT_SHIFT_PRESSED;
            }
            if ((modifiers & (1U << 2)) != 0) {
                state |= EFI_LEFT_ALT_PRESSED;
            }
            if ((modifiers & (1U << 3)) != 0) {
                state |= EFI_LEFT_LOGO_PRESSED;
            }
            if ((modifiers & (1U << 4)) != 0) {
                state |= EFI_RIGHT_CONTROL_PRESSED;
            }
            if ((modifiers & (1U << 5)) != 0) {
                state |= EFI_RIGHT_SHIFT_PRESSED;
            }
            if ((modifiers & (1U << 6)) != 0) {
                state |= EFI_RIGHT_ALT_PRESSED;
            }
            if ((modifiers & (1U << 7)) != 0) {
                state |= EFI_RIGHT_LOGO_PRESSED;
            }
            fw_set_mem(&mConInCurrentKeyState,
                       sizeof(mConInCurrentKeyState), 0);
            mConInCurrentKeyState.KeyShiftState = state;
            return 1;
        }
    }
    return 0;
}

EFI_STATUS __attribute__((noinline, used))
usb_keyboard_read_key(EFI_INPUT_KEY *Key)
{
    UINT32 head;
    UINT32 cc;
    EFI_STATUS status = EFI_NOT_READY;

    if (!usb_keyboard_init()) {
        return EFI_NOT_READY;
    }

    head = usb_ohci_ed_head(&mUsbOhciInterruptEd) & OHCI_DPTR_MASK;
    if (head != usb_ohci_ed_tail(&mUsbOhciInterruptEd)) {
        return EFI_NOT_READY;
    }
    usb_dma_barrier();

    cc = usb_ohci_td_condition_code(&mUsbOhciInterruptTd[0]);
    if (cc == OHCI_TD_CC_NOERROR) {
        if (usb_keyboard_report_to_key(Key)) {
            status = EFI_SUCCESS;
        }
        fw_copy_mem(mUsbKeyboardPreviousReport, mUsbKeyboardReport,
                    sizeof(mUsbKeyboardPreviousReport));
    } else {
        fw_set_mem(mUsbKeyboardPreviousReport,
                   sizeof(mUsbKeyboardPreviousReport), 0);
    }
    usb_keyboard_submit_interrupt_td();
    return status;
}


BOOLEAN __attribute__((noinline)) uefi_usb_keyboard_selftest(void)
{
    UINT8 saved_report[OHCI_USB_KEYBOARD_REPORT_SIZE];
    UINT8 saved_previous[OHCI_USB_KEYBOARD_REPORT_SIZE];
    EFI_KEY_STATE saved_current_state = mConInCurrentKeyState;
    EFI_INPUT_KEY key;
    BOOLEAN ok = 1;

    fw_copy_mem(saved_report, mUsbKeyboardReport, sizeof(saved_report));
    fw_copy_mem(saved_previous, mUsbKeyboardPreviousReport,
                sizeof(saved_previous));
    fw_set_mem(mUsbKeyboardReport, sizeof(mUsbKeyboardReport), 0);
    fw_set_mem(mUsbKeyboardPreviousReport,
               sizeof(mUsbKeyboardPreviousReport), 0);

    mUsbKeyboardReport[2] = 0x04;
    if (!usb_keyboard_report_to_key(&key) ||
        key.ScanCode != 0 || key.UnicodeChar != 'a' ||
        mConInCurrentKeyState.KeyShiftState != EFI_SHIFT_STATE_VALID) {
        ok = 0;
    }
    fw_copy_mem(mUsbKeyboardPreviousReport, mUsbKeyboardReport,
                sizeof(mUsbKeyboardPreviousReport));
    if (usb_keyboard_report_to_key(&key)) {
        ok = 0;
    }

    fw_set_mem(mUsbKeyboardPreviousReport,
               sizeof(mUsbKeyboardPreviousReport), 0);
    mUsbKeyboardReport[0] = 1U << 1;
    mUsbKeyboardReport[2] = 0x04;
    if (!usb_keyboard_report_to_key(&key) ||
        key.ScanCode != 0 || key.UnicodeChar != 'A' ||
        mConInCurrentKeyState.KeyShiftState !=
            (EFI_SHIFT_STATE_VALID | EFI_LEFT_SHIFT_PRESSED)) {
        ok = 0;
    }

    mUsbKeyboardReport[0] = 0;
    mUsbKeyboardReport[2] = 0x51;
    if (!usb_keyboard_report_to_key(&key) ||
        key.ScanCode != EFI_SCAN_DOWN || key.UnicodeChar != 0) {
        ok = 0;
    }

    fw_copy_mem(mUsbKeyboardReport, saved_report, sizeof(saved_report));
    fw_copy_mem(mUsbKeyboardPreviousReport, saved_previous,
                sizeof(saved_previous));
    mConInCurrentKeyState = saved_current_state;
    return ok;
}

