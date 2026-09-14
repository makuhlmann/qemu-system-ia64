#include "fw-boot-shell.h"
#include "fw-usb.h"
#include "fw-pointer.h"
/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Console stack: UART primitives, the PS/2 controller front-end, the
 * software VGA text renderer, and the EFI SimpleTextOutput /
 * SimpleTextInput(+Ex) protocols with their PS/2, USB-keyboard and
 * serial key sources.  Extracted verbatim from firmware.c (Phase 1 of
 * plans/firmware-rework-plan.md).
 */

#include "fw-base.h"
#include "fw-services.h"
#include "fw-platform-layout.h"
#include "vga_font_8x16.h"

#define FW_CONIN_KEY_NOTIFY_MAX 16U

typedef struct {
    BOOLEAN in_use;
    EFI_KEY_DATA key_data;
    EFI_KEY_NOTIFY_FUNCTION notify;
} FW_CONIN_KEY_NOTIFY_RECORD;

EFI_SIMPLE_TEXT_OUT_PROTOCOL mConOutProto;
SIMPLE_TEXT_OUTPUT_MODE      mConOutMode;
static FW_CONIN_KEY_NOTIFY_RECORD mConInKeyNotifyRecords[FW_CONIN_KEY_NOTIFY_MAX];
static BOOLEAN                mConInBufferedKeyValid;
static EFI_INPUT_KEY          mConInBufferedKey;
static EFI_KEY_STATE          mConInBufferedKeyState;
EFI_KEY_STATE                 mConInCurrentKeyState;
static BOOLEAN                mPs2Break;
static BOOLEAN                mPs2Extended;
static BOOLEAN                mPs2Shift;
static UINT32                 mPs2ModifierState;
static BOOLEAN                mPs2Translated;
static UINT8                  mPs2KeyboardRaw[32];
static UINTN                  mPs2KeyboardRawRead;
static UINTN                  mPs2KeyboardRawWrite;
static UINTN                  mPs2KeyboardRawCount;
static CHAR16                 mTextChars[VGA_TEXT_ROWS][VGA_TEXT_COLUMNS];
static UINT8                  mTextAttrs[VGA_TEXT_ROWS][VGA_TEXT_COLUMNS];
static BOOLEAN                mTextWrapPending;

/* --- UART helpers --------------------------------------------------------- */

/*
 * The console UART: the 460GX board's COM1 in legacy I/O space, the zx1
 * machine's memory-mapped stand-in otherwise.
 */
UINT64 fw_console_uart_base(void)
{
    return fw_platform_is_460gx()
        ? LEGACY_IO_BASE + IA64_460GX_COM1_IO_BASE
        : IA64_UART_BASE;
}

volatile UINT8 *fw_uart_reg(UINTN offset)
{
    return (volatile UINT8 *)(UINTN)(fw_console_uart_base() + offset);
}

UINT64 fw_read_psr(void)
{
    UINT64 psr;

    __asm__ volatile ("mov %0 = psr" : "=r"(psr));
    return psr;
}

UINT64 fw_read_cpuid3(void)
{
    UINT64 value;

    __asm__ volatile ("mov %0 = cpuid[%1]" : "=r"(value) : "r"(3UL));
    return value;
}

void fw_flush_instruction_cache(VOID *start, UINTN bytes)
{
    UINTN address = (UINTN)start & ~(UINTN)31U;
    UINTN end = ((UINTN)start + bytes + 31U) & ~(UINTN)31U;

    while (address < end) {
        __asm__ volatile ("fc %0" :: "r"(address) : "memory");
        address += 32U;
    }
    __asm__ volatile ("sync.i;;\n\tsrlz.i;;" ::: "memory");
}


static void uart_putc(char c)
{
    if (fw_data_translation_enabled()) {
        return;
    }

    /* Some emulated paths don't expose a stable LSR_THRE bit early in boot. */
    (void)*fw_uart_reg(UART_LSR);
    *fw_uart_reg(UART_THR) = (UINT8)c;
}

static BOOLEAN uart_can_read(void)
{
    return (*fw_uart_reg(UART_LSR) & UART_LSR_DR) != 0;
}

static UINT8 uart_getc(void)
{
    return *fw_uart_reg(UART_RBR);
}

volatile UINT8 *ps2_reg(UINTN addr)
{
    return (volatile UINT8 *)addr;
}

UINT8 ps2_read_status(void)
{
    if (!fw_handoff_i8042_enabled()) {
        return 0;
    }
    return *ps2_reg(PS2_STATUS_PORT);
}

static BOOLEAN ps2_wait_input_clear(void)
{
    UINTN limit;

    for (limit = 0; limit < 100000; limit++) {
        if ((ps2_read_status() & PS2_STATUS_IBF) == 0) {
            return 1;
        }
    }
    return 0;
}

static BOOLEAN ps2_wait_output_full(void)
{
    UINTN limit;

    for (limit = 0; limit < 100000; limit++) {
        if ((ps2_read_status() & PS2_STATUS_OBF) != 0) {
            return 1;
        }
    }
    return 0;
}

BOOLEAN ps2_write_command(UINT8 command)
{
    if (!ps2_wait_input_clear()) {
        return 0;
    }
    *ps2_reg(PS2_STATUS_PORT) = command;
    return 1;
}

BOOLEAN ps2_write_data(UINT8 data)
{
    if (!ps2_wait_input_clear()) {
        return 0;
    }
    *ps2_reg(PS2_DATA_PORT) = data;
    return 1;
}

BOOLEAN ps2_keyboard_raw_push(UINT8 Data)
{
    if (mPs2KeyboardRawCount == FW_ARRAY_SIZE(mPs2KeyboardRaw)) {
        return 0;
    }
    mPs2KeyboardRaw[mPs2KeyboardRawWrite] = Data;
    mPs2KeyboardRawWrite = (mPs2KeyboardRawWrite + 1U) %
        FW_ARRAY_SIZE(mPs2KeyboardRaw);
    mPs2KeyboardRawCount++;
    return 1;
}

static BOOLEAN ps2_keyboard_raw_pop(UINT8 *Data)
{
    if (Data == NULL || mPs2KeyboardRawCount == 0) {
        return 0;
    }
    *Data = mPs2KeyboardRaw[mPs2KeyboardRawRead];
    mPs2KeyboardRawRead = (mPs2KeyboardRawRead + 1U) %
        FW_ARRAY_SIZE(mPs2KeyboardRaw);
    mPs2KeyboardRawCount--;
    return 1;
}

static BOOLEAN ps2_keyboard_wait_response(UINT8 expected)
{
    UINTN limit;

    for (limit = 0; limit < 100000; limit++) {
        UINT8 status;
        UINT8 data;

        status = ps2_read_status();
        if ((status & PS2_STATUS_OBF) == 0) {
            continue;
        }

        data = *ps2_reg(PS2_DATA_PORT);
        if ((status & PS2_STATUS_MOUSE_OBF) != 0) {
            continue;
        }
        return data == expected;
    }
    return 0;
}

static BOOLEAN __attribute__((noinline, used)) ps2_keyboard_enable_scanning(void)
{
    return ps2_write_data(PS2_KBD_CMD_ENABLE_SCAN) &&
           ps2_keyboard_wait_response(PS2_KBD_ACK);
}

void ps2_init_controller(void)
{
    UINT8 mode;

    mPs2Break = 0;
    mPs2Extended = 0;
    mPs2Shift = 0;
    mPs2ModifierState = 0;
    mPs2KeyboardRawRead = 0;
    mPs2KeyboardRawWrite = 0;
    mPs2KeyboardRawCount = 0;
    if (!fw_handoff_i8042_enabled()) {
        return;
    }

    if (!ps2_write_command(PS2_CMD_READ_MODE) ||
        !ps2_wait_output_full()) {
        return;
    }

    mode = *ps2_reg(PS2_DATA_PORT);
    mode |= PS2_MODE_KBD_INT | PS2_MODE_MOUSE_INT |
            PS2_MODE_SYS | PS2_MODE_KCC;

    if (!ps2_write_command(PS2_CMD_WRITE_MODE)) {
        return;
    }
    if (!ps2_write_data(mode)) {
        return;
    }
    mPs2Translated = (mode & PS2_MODE_KCC) != 0;
    (void)ps2_write_command(PS2_CMD_KBD_ENABLE);
    (void)ps2_keyboard_enable_scanning();
}

void uart_puts(const char *s)
{
    while (*s) {
        if (*s == '\n') uart_putc('\r');
        uart_putc(*s++);
    }
}

const CHAR8 *efi_status_name(EFI_STATUS Status)
{
    switch (Status) {
    case EFI_SUCCESS:             return "SUCCESS";
    case EFI_LOAD_ERROR:          return "LOAD_ERROR";
    case EFI_INVALID_PARAMETER:   return "INVALID_PARAMETER";
    case EFI_UNSUPPORTED:         return "UNSUPPORTED";
    case EFI_BAD_BUFFER_SIZE:     return "BAD_BUFFER_SIZE";
    case EFI_BUFFER_TOO_SMALL:    return "BUFFER_TOO_SMALL";
    case EFI_NOT_READY:           return "NOT_READY";
    case EFI_DEVICE_ERROR:        return "DEVICE_ERROR";
    case EFI_WRITE_PROTECTED:     return "WRITE_PROTECTED";
    case EFI_OUT_OF_RESOURCES:    return "OUT_OF_RESOURCES";
    case EFI_VOLUME_CORRUPTED:    return "VOLUME_CORRUPTED";
    case EFI_VOLUME_FULL:         return "VOLUME_FULL";
    case EFI_NO_MEDIA:            return "NO_MEDIA";
    case EFI_MEDIA_CHANGED:       return "MEDIA_CHANGED";
    case EFI_NOT_FOUND:           return "NOT_FOUND";
    case EFI_ACCESS_DENIED:       return "ACCESS_DENIED";
    case EFI_NO_RESPONSE:         return "NO_RESPONSE";
    case EFI_NO_MAPPING:          return "NO_MAPPING";
    case EFI_TIMEOUT:             return "TIMEOUT";
    case EFI_NOT_STARTED:         return "NOT_STARTED";
    case EFI_ALREADY_STARTED:     return "ALREADY_STARTED";
    case EFI_ABORTED:             return "ABORTED";
    case EFI_ICMP_ERROR:          return "ICMP_ERROR";
    case EFI_TFTP_ERROR:          return "TFTP_ERROR";
    case EFI_PROTOCOL_ERROR:      return "PROTOCOL_ERROR";
    default:                      return "UNKNOWN";
    }
}

void uart_put_hex64(UINT64 value)
{
    static const char hx[] = "0123456789ABCDEF";

    uart_putc(hx[(value >> 60) & 0xF]);
    uart_putc(hx[(value >> 56) & 0xF]);
    uart_putc(hx[(value >> 52) & 0xF]);
    uart_putc(hx[(value >> 48) & 0xF]);
    uart_putc(hx[(value >> 44) & 0xF]);
    uart_putc(hx[(value >> 40) & 0xF]);
    uart_putc(hx[(value >> 36) & 0xF]);
    uart_putc(hx[(value >> 32) & 0xF]);
    uart_putc(hx[(value >> 28) & 0xF]);
    uart_putc(hx[(value >> 24) & 0xF]);
    uart_putc(hx[(value >> 20) & 0xF]);
    uart_putc(hx[(value >> 16) & 0xF]);
    uart_putc(hx[(value >> 12) & 0xF]);
    uart_putc(hx[(value >> 8) & 0xF]);
    uart_putc(hx[(value >> 4) & 0xF]);
    uart_putc(hx[value & 0xF]);
}

/* --- VGA text console helpers -------------------------------------------- */

#define GLYPH7(a, b, c, d, e, f, g) \
    ((UINT64)(a) | ((UINT64)(b) << 5) | ((UINT64)(c) << 10) | \
     ((UINT64)(d) << 15) | ((UINT64)(e) << 20) | \
     ((UINT64)(f) << 25) | ((UINT64)(g) << 30))

static UINT8 text_unicode_to_cp437(CHAR16 Ch)
{
    static const CHAR16 cp437_unicode[256] = {
        0x0000U, 0x263aU, 0x263bU, 0x2665U,
        0x2666U, 0x2663U, 0x2660U, 0x2022U,
        0x25d8U, 0x25cbU, 0x25d9U, 0x2642U,
        0x2640U, 0x266aU, 0x266bU, 0x263cU,
        0x25baU, 0x25c4U, 0x2195U, 0x203cU,
        0x00b6U, 0x00a7U, 0x25acU, 0x21a8U,
        0x2191U, 0x2193U, 0x2192U, 0x2190U,
        0x221fU, 0x2194U, 0x25b2U, 0x25bcU,
        0x0020U, 0x0021U, 0x0022U, 0x0023U,
        0x0024U, 0x0025U, 0x0026U, 0x0027U,
        0x0028U, 0x0029U, 0x002aU, 0x002bU,
        0x002cU, 0x002dU, 0x002eU, 0x002fU,
        0x0030U, 0x0031U, 0x0032U, 0x0033U,
        0x0034U, 0x0035U, 0x0036U, 0x0037U,
        0x0038U, 0x0039U, 0x003aU, 0x003bU,
        0x003cU, 0x003dU, 0x003eU, 0x003fU,
        0x0040U, 0x0041U, 0x0042U, 0x0043U,
        0x0044U, 0x0045U, 0x0046U, 0x0047U,
        0x0048U, 0x0049U, 0x004aU, 0x004bU,
        0x004cU, 0x004dU, 0x004eU, 0x004fU,
        0x0050U, 0x0051U, 0x0052U, 0x0053U,
        0x0054U, 0x0055U, 0x0056U, 0x0057U,
        0x0058U, 0x0059U, 0x005aU, 0x005bU,
        0x005cU, 0x005dU, 0x005eU, 0x005fU,
        0x0060U, 0x0061U, 0x0062U, 0x0063U,
        0x0064U, 0x0065U, 0x0066U, 0x0067U,
        0x0068U, 0x0069U, 0x006aU, 0x006bU,
        0x006cU, 0x006dU, 0x006eU, 0x006fU,
        0x0070U, 0x0071U, 0x0072U, 0x0073U,
        0x0074U, 0x0075U, 0x0076U, 0x0077U,
        0x0078U, 0x0079U, 0x007aU, 0x007bU,
        0x007cU, 0x007dU, 0x007eU, 0x2302U,
        0x00c7U, 0x00fcU, 0x00e9U, 0x00e2U,
        0x00e4U, 0x00e0U, 0x00e5U, 0x00e7U,
        0x00eaU, 0x00ebU, 0x00e8U, 0x00efU,
        0x00eeU, 0x00ecU, 0x00c4U, 0x00c5U,
        0x00c9U, 0x00e6U, 0x00c6U, 0x00f4U,
        0x00f6U, 0x00f2U, 0x00fbU, 0x00f9U,
        0x00ffU, 0x00d6U, 0x00dcU, 0x00a2U,
        0x00a3U, 0x00a5U, 0x20a7U, 0x0192U,
        0x00e1U, 0x00edU, 0x00f3U, 0x00faU,
        0x00f1U, 0x00d1U, 0x00aaU, 0x00baU,
        0x00bfU, 0x2310U, 0x00acU, 0x00bdU,
        0x00bcU, 0x00a1U, 0x00abU, 0x00bbU,
        0x2591U, 0x2592U, 0x2593U, 0x2502U,
        0x2524U, 0x2561U, 0x2562U, 0x2556U,
        0x2555U, 0x2563U, 0x2551U, 0x2557U,
        0x255dU, 0x255cU, 0x255bU, 0x2510U,
        0x2514U, 0x2534U, 0x252cU, 0x251cU,
        0x2500U, 0x253cU, 0x255eU, 0x255fU,
        0x255aU, 0x2554U, 0x2569U, 0x2566U,
        0x2560U, 0x2550U, 0x256cU, 0x2567U,
        0x2568U, 0x2564U, 0x2565U, 0x2559U,
        0x2558U, 0x2552U, 0x2553U, 0x256bU,
        0x256aU, 0x2518U, 0x250cU, 0x2588U,
        0x2584U, 0x258cU, 0x2590U, 0x2580U,
        0x03b1U, 0x00dfU, 0x0393U, 0x03c0U,
        0x03a3U, 0x03c3U, 0x00b5U, 0x03c4U,
        0x03a6U, 0x0398U, 0x03a9U, 0x03b4U,
        0x221eU, 0x03c6U, 0x03b5U, 0x2229U,
        0x2261U, 0x00b1U, 0x2265U, 0x2264U,
        0x2320U, 0x2321U, 0x00f7U, 0x2248U,
        0x00b0U, 0x2219U, 0x00b7U, 0x221aU,
        0x207fU, 0x00b2U, 0x25a0U, 0x00a0U
    };
    UINTN code;

    if (Ch >= 0x20U && Ch <= 0x7eU) {
        return (UINT8)Ch;
    }
    for (code = 1; code < 256U; code++) {
        if (cp437_unicode[code] == Ch) {
            return (UINT8)code;
        }
    }
    return (UINT8)'?';
}

static int text_font_glyphs_identical(unsigned char A, unsigned char B)
{
    UINTN i;

    for (i = 0; i < VGA_FONT_8X16_HEIGHT; i++) {
        if (gVgaFont8x16[A][i] != gVgaFont8x16[B][i]) {
            return 0;
        }
    }
    return 1;
}

static UINT32 text_efi_color(UINTN Color)
{
    static const UINT32 colors[16] = {
        0x00000000U, 0x000000aaU, 0x0000aa00U, 0x0000aaaaU,
        0x00aa0000U, 0x00aa00aaU, 0x00aa5500U, 0x00aaaaaaU,
        0x00555555U, 0x005555ffU, 0x0055ff55U, 0x0055ffffU,
        0x00ff5555U, 0x00ff55ffU, 0x00ffff55U, 0x00ffffffU
    };

    return colors[Color & 0x0fU];
}

static UINT64 text_pixel_pair(UINT32 Left, UINT32 Right)
{
    return (UINT64)Left | ((UINT64)Right << 32);
}

static UINT32 text_read_pixel(UINTN X, UINTN Y)
{
    volatile UINT32 *p =
        (volatile UINT32 *)(UINTN)(VGA_FB_BASE +
                                   Y * mGraphicsStride + X * 4U);

    return *p;
}

static void text_draw_graphics_cell(UINTN X0, UINTN Y0, UINT8 Ch,
                                    UINT32 Fg, UINT32 Bg)
{
    const unsigned char *glyph = gVgaFont8x16[Ch];
    UINTN y;

    /*
     * Rasterise the standard 8x16 VGA glyph: one byte per scan line, bit 0x80
     * leftmost, eight pixels per row packed as four 32-bit-pixel pairs.  This
     * matches what the VGA character generator produces from plane 2, so the
     * graphics-mode console and the hardware text-mode console show an
     * identical font.
     */
    for (y = 0; y < VGA_TEXT_CELL_HEIGHT; y++) {
        volatile UINT64 *dst =
            (volatile UINT64 *)(UINTN)(VGA_FB_BASE +
                                       (Y0 + y) * mGraphicsStride +
                                       X0 * sizeof(UINT32));
        UINT8 bits = (y < VGA_FONT_8X16_HEIGHT) ? glyph[y] : 0U;

        dst[0] = text_pixel_pair((bits & 0x80U) ? Fg : Bg,
                                 (bits & 0x40U) ? Fg : Bg);
        dst[1] = text_pixel_pair((bits & 0x20U) ? Fg : Bg,
                                 (bits & 0x10U) ? Fg : Bg);
        dst[2] = text_pixel_pair((bits & 0x08U) ? Fg : Bg,
                                 (bits & 0x04U) ? Fg : Bg);
        dst[3] = text_pixel_pair((bits & 0x02U) ? Fg : Bg,
                                 (bits & 0x01U) ? Fg : Bg);
    }
}

static void text_write_legacy_cell(UINTN Column, UINTN Row, UINT8 Ch)
{
    volatile UINT16 *fb = (volatile UINT16 *)(UINTN)VGA_TEXT_FB_BASE;
    UINT16 attr = (UINT16)mTextAttrs[Row][Column] << 8;

    fb[Row * VGA_TEXT_COLUMNS + Column] = attr | (UINT16)Ch;
}

void text_clear_legacy_cells(void)
{
    volatile UINT16 *fb = (volatile UINT16 *)(UINTN)VGA_TEXT_FB_BASE;
    UINTN cell;

    for (cell = 0; cell < VGA_TEXT_COLUMNS * VGA_TEXT_ROWS; cell++) {
        fb[cell] = 0x0720U;
    }
}

static void text_draw_cell(UINTN Column, UINTN Row)
{
    UINTN x0 = Column * VGA_TEXT_CELL_WIDTH;
    UINTN y0 = Row * VGA_TEXT_CELL_HEIGHT;
    UINT8 attr = mTextAttrs[Row][Column];
    UINT32 fg = text_efi_color(attr & 0x0fU);
    UINT32 bg = text_efi_color((attr >> 4) & 0x07U);
    UINT8 ch = text_unicode_to_cp437(mTextChars[Row][Column]);

    text_write_legacy_cell(Column, Row, ch);

    if (!mGraphicsActive) {
        return;
    }

    text_draw_graphics_cell(x0, y0, ch, fg, bg);
}

void text_redraw_screen(void)
{
    UINTN row;
    UINTN col;

    for (row = 0; row < VGA_TEXT_ROWS; row++) {
        for (col = 0; col < VGA_TEXT_COLUMNS; col++) {
            text_draw_cell(col, row);
        }
    }
}

/* Push the software cursor position and visibility to the VGA hardware. */
static void text_sync_cursor(void)
{
    UINT16 loc = (UINT16)((UINTN)mConOutMode.CursorRow * VGA_TEXT_COLUMNS +
                          (UINTN)mConOutMode.CursorColumn);

    graphics_set_text_cursor(loc, mConOutMode.CursorVisible ? 1 : 0);
}

static void text_clear_row(UINTN Row)
{
    UINTN col;

    for (col = 0; col < VGA_TEXT_COLUMNS; col++) {
        mTextChars[Row][col] = ' ';
        mTextAttrs[Row][col] = (UINT8)(mConOutMode.Attribute & 0x7f);
        text_draw_cell(col, Row);
    }
}

void text_clear_screen(void)
{
    UINTN row;

    for (row = 0; row < VGA_TEXT_ROWS; row++) {
        text_clear_row(row);
    }
    mConOutMode.CursorColumn = 0;
    mConOutMode.CursorRow = 0;
    mTextWrapPending = 0;
    text_sync_cursor();
}

static void text_scroll(void)
{
    UINTN row;
    UINTN col;

    for (row = 1; row < VGA_TEXT_ROWS; row++) {
        for (col = 0; col < VGA_TEXT_COLUMNS; col++) {
            mTextChars[row - 1][col] = mTextChars[row][col];
            mTextAttrs[row - 1][col] = mTextAttrs[row][col];
        }
    }
    for (col = 0; col < VGA_TEXT_COLUMNS; col++) {
        mTextChars[VGA_TEXT_ROWS - 1U][col] = ' ';
        mTextAttrs[VGA_TEXT_ROWS - 1U][col] =
            (UINT8)(mConOutMode.Attribute & 0x7f);
    }
    mConOutMode.CursorRow = VGA_TEXT_ROWS - 1U;
    text_redraw_screen();
}

static void text_advance_line(void)
{
    mConOutMode.CursorColumn = 0;
    mConOutMode.CursorRow++;
    mTextWrapPending = 0;
    if ((UINTN)mConOutMode.CursorRow >= VGA_TEXT_ROWS) {
        text_scroll();
    }
}

static BOOLEAN efi_conout_char_supported(CHAR16 Ch)
{
    if (Ch == 0 || Ch == '\r' || Ch == '\n' || Ch == '\b' || Ch == '\t') {
        return 1;
    }
    if (Ch == '?') {
        return 1;
    }
    return text_unicode_to_cp437(Ch) != (UINT8)'?';
}

static void text_put_char(CHAR16 Ch)
{
    UINTN col;
    UINTN row;

    switch (Ch) {
    case '\r':
        mConOutMode.CursorColumn = 0;
        mTextWrapPending = 0;
        return;
    case '\n':
        text_advance_line();
        return;
    case '\b':
        if (mTextWrapPending) {
            mTextWrapPending = 0;
            return;
        }
        if (mConOutMode.CursorColumn > 0) {
            mConOutMode.CursorColumn--;
        }
        return;
    case '\t':
        do {
            text_put_char(' ');
        } while ((mConOutMode.CursorColumn & 7) != 0);
        return;
    default:
        break;
    }

    if (mTextWrapPending) {
        text_advance_line();
    }
    if (!efi_conout_char_supported(Ch)) {
        return;
    }
    col = (UINTN)mConOutMode.CursorColumn;
    row = (UINTN)mConOutMode.CursorRow;
    if (col >= VGA_TEXT_COLUMNS || row >= VGA_TEXT_ROWS) {
        text_advance_line();
        col = (UINTN)mConOutMode.CursorColumn;
        row = (UINTN)mConOutMode.CursorRow;
    }

    mTextChars[row][col] = Ch;
    mTextAttrs[row][col] = (UINT8)(mConOutMode.Attribute & 0x7f);
    text_draw_cell(col, row);

    if ((UINTN)mConOutMode.CursorColumn + 1U >= VGA_TEXT_COLUMNS) {
        mTextWrapPending = 1;
    } else {
        mConOutMode.CursorColumn++;
    }
}

static EFI_STATUS efi_conout_string_supported(CHAR16 *String)
{
    if (String == NULL) {
        return EFI_INVALID_PARAMETER;
    }
    while (*String != 0) {
        if (!efi_conout_char_supported(*String)) {
            return EFI_UNSUPPORTED;
        }
        String++;
    }
    return EFI_SUCCESS;
}

/* --- EFI text output protocol implementation ------------------------------ */

EFI_STATUS efi_conout_reset(VOID *This, BOOLEAN ExtendedVerification)
{
    (void)This; (void)ExtendedVerification;
    text_clear_screen();
    return EFI_SUCCESS;
}

EFI_STATUS efi_conout_string(VOID *This, CHAR16 *String)
{
    EFI_STATUS st = EFI_SUCCESS;

    (void)This;
    if (String == NULL) {
        return EFI_INVALID_PARAMETER;
    }
    while (*String) {
        if (!efi_conout_char_supported(*String)) {
            st = EFI_WARN_UNKNOWN_GLYPH;
            text_put_char('?');
            String++;
            continue;
        }
        if (*String <= 0x7fU) {
            char c = (char)(*String & 0xFF);
            if (c == '\n') {
                uart_putc('\r');
            }
            uart_putc(c);
        }
        text_put_char(*String);
        String++;
    }
    if (mConOutMode.CursorVisible) {
        text_sync_cursor();
    }
    return st;
}

void efi_conout_ascii(const CHAR8 *String)
{
    CHAR16 ch[2];

    ch[1] = 0;
    while (*String) {
        ch[0] = (CHAR16)(UINT8)*String++;
        (void)efi_conout_string(&mConOutProto, ch);
    }
}

EFI_STATUS efi_conout_test_string(VOID *This, CHAR16 *String)
{
    (void)This;
    return efi_conout_string_supported(String);
}

EFI_STATUS efi_conout_query_mode(VOID *This, UINTN ModeNumber,
                                         UINTN *Columns, UINTN *Rows)
{
    (void)This;
    if (Columns == NULL || Rows == NULL) {
        return EFI_INVALID_PARAMETER;
    }
    if (ModeNumber > 0) {
        return EFI_UNSUPPORTED;
    }
    *Columns = VGA_TEXT_COLUMNS;
    *Rows = VGA_TEXT_ROWS;
    return EFI_SUCCESS;
}

EFI_STATUS efi_conout_set_mode(VOID *This, UINTN ModeNumber)
{
    (void)This;
    if (ModeNumber != 0) {
        return EFI_UNSUPPORTED;
    }
    mConOutMode.Mode = 0;
    text_clear_screen();
    return EFI_SUCCESS;
}

EFI_STATUS efi_conout_set_attribute(VOID *This, UINTN Attribute)
{
    (void)This;
    if ((Attribute & ~0x7fU) != 0) {
        return EFI_INVALID_PARAMETER;
    }
    mConOutMode.Attribute = (INT32)(Attribute & 0x7fU);
    return EFI_SUCCESS;
}

EFI_STATUS efi_conout_clear_screen(VOID *This)
{
    (void)This;
    uart_puts("\n\n");
    text_clear_screen();
    return EFI_SUCCESS;
}

EFI_STATUS efi_conout_set_cursor(VOID *This, UINTN Column, UINTN Row)
{
    (void)This;
    if (Column >= VGA_TEXT_COLUMNS || Row >= VGA_TEXT_ROWS) {
        return EFI_INVALID_PARAMETER;
    }
    mConOutMode.CursorColumn = (INT32)Column;
    mConOutMode.CursorRow = (INT32)Row;
    mTextWrapPending = 0;
    text_sync_cursor();
    return EFI_SUCCESS;
}

EFI_STATUS efi_conout_enable_cursor(VOID *This, BOOLEAN Enable)
{
    (void)This;
    mConOutMode.CursorVisible = Enable ? 1 : 0;
    text_sync_cursor();
    return EFI_SUCCESS;
}

/* Show or hide the text cursor -- the boot menu hides it, the shell shows it. */
void fw_console_set_cursor_visible(BOOLEAN Visible)
{
    (void)efi_conout_enable_cursor(&mConOutProto, Visible);
}

/* Set the console text attribute (foreground | background<<4). */
void fw_console_set_attr(UINTN Attribute)
{
    (void)efi_conout_set_attribute(&mConOutProto, Attribute);
}

static BOOLEAN text_graphics_a_cell_selftest(UINTN Column, UINTN Row)
{
    static const UINT8 expected_rows[VGA_TEXT_CELL_HEIGHT] = {
        0x00U,
        0x1cU, 0x1cU,
        0x22U, 0x22U,
        0x22U, 0x22U,
        0x3eU, 0x3eU,
        0x22U, 0x22U,
        0x22U, 0x22U,
        0x22U, 0x22U,
        0x00U,
    };
    UINT32 fg = 0x00ffff55U;
    UINT32 bg = 0x000000aaU;
    UINTN x0 = Column * VGA_TEXT_CELL_WIDTH;
    UINTN y0 = Row * VGA_TEXT_CELL_HEIGHT;
    UINTN x;
    UINTN y;

    if (!mGraphicsActive) {
        return 1;
    }
    for (y = 0; y < VGA_TEXT_CELL_HEIGHT; y++) {
        for (x = 0; x < VGA_TEXT_CELL_WIDTH; x++) {
            UINT32 expected =
                (expected_rows[y] & (1U << x)) ? fg : bg;

            if (text_read_pixel(x0 + x, y0 + y) != expected) {
                return 0;
            }
        }
    }
    return 1;
}

BOOLEAN __attribute__((noinline)) uefi_conout_selftest(void)
{
    CHAR16 supported[] = { 'O', 'K', '\r', '\n', 0 };
    CHAR16 required_glyphs[] = {
        0x2500U, 0x2502U, 0x250cU, 0x2510U, 0x2514U, 0x2518U,
        0x251cU, 0x2524U, 0x252cU, 0x2534U, 0x253cU,
        0x2550U, 0x2551U, 0x2552U, 0x2553U, 0x2554U, 0x2555U,
        0x2556U, 0x2557U, 0x2558U, 0x2559U, 0x255aU, 0x255bU,
        0x255cU, 0x255dU, 0x255eU, 0x255fU, 0x2560U, 0x2561U,
        0x2562U, 0x2563U, 0x2564U, 0x2565U, 0x2566U, 0x2567U,
        0x2568U, 0x2569U, 0x256aU, 0x256bU, 0x256cU,
        0x2588U, 0x2591U,
        0x25b2U, 0x25baU, 0x25bcU, 0x25c4U,
        0x2191U, 0x2193U, 0
    };
    CHAR16 unsupported[] = { 0x2603U, 0 };
    UINTN columns = 0;
    UINTN rows = 0;
    INT32 saved_attribute = mConOutMode.Attribute;
    INT32 saved_column = mConOutMode.CursorColumn;
    INT32 saved_row = mConOutMode.CursorRow;
    BOOLEAN saved_wrap = mTextWrapPending;
    CHAR16 saved_chars[2];
    UINT8 saved_attrs[2];
    EFI_STATUS st;
    BOOLEAN ok = 1;

    st = efi_conout_query_mode(&mConOutProto, 0, &columns, &rows);
    if (st != EFI_SUCCESS || columns != VGA_TEXT_COLUMNS ||
        rows != VGA_TEXT_ROWS) {
        ok = 0;
    }
    st = efi_conout_query_mode(&mConOutProto, 0, NULL, &rows);
    if (st != EFI_INVALID_PARAMETER) {
        ok = 0;
    }
    st = efi_conout_query_mode(&mConOutProto, 1, &columns, &rows);
    if (st != EFI_UNSUPPORTED) {
        ok = 0;
    }
    st = efi_conout_test_string(&mConOutProto, supported);
    if (st != EFI_SUCCESS) {
        ok = 0;
    }
    st = efi_conout_test_string(&mConOutProto, required_glyphs);
    if (st != EFI_SUCCESS) {
        ok = 0;
    }
    st = efi_conout_test_string(&mConOutProto, unsupported);
    if (st != EFI_UNSUPPORTED) {
        ok = 0;
    }
    st = efi_conout_string(&mConOutProto, unsupported);
    if (st != EFI_WARN_UNKNOWN_GLYPH) {
        ok = 0;
    }
    if (text_font_glyphs_identical('a', 'A') ||
        text_font_glyphs_identical('g', 'G') ||
        text_font_glyphs_identical('z', 'Z')) {
        ok = 0;
    }
    saved_chars[0] = mTextChars[0][0];
    saved_chars[1] = mTextChars[0][1];
    saved_attrs[0] = mTextAttrs[0][0];
    saved_attrs[1] = mTextAttrs[0][1];
    mConOutMode.CursorColumn = 0;
    mConOutMode.CursorRow = 0;
    mTextWrapPending = 0;
    text_put_char('A');
    text_put_char('B');
    text_put_char('\b');
    if (mConOutMode.CursorColumn != 1 || mConOutMode.CursorRow != 0 ||
        mTextWrapPending ||
        mTextChars[0][0] != 'A' || mTextChars[0][1] != 'B') {
        ok = 0;
    }
    text_put_char('\b');
    if (mConOutMode.CursorColumn != 0 || mConOutMode.CursorRow != 0 ||
        mTextWrapPending || mTextChars[0][0] != 'A') {
        ok = 0;
    }
    text_put_char('\b');
    if (mConOutMode.CursorColumn != 0 || mConOutMode.CursorRow != 0 ||
        mTextWrapPending || mTextChars[0][0] != 'A') {
        ok = 0;
    }
    mTextChars[0][0] = 'A';
    mTextAttrs[0][0] = 0x1eU;
    text_draw_cell(0, 0);
    if (!text_graphics_a_cell_selftest(0, 0)) {
        ok = 0;
    }
    mTextChars[0][0] = saved_chars[0];
    mTextChars[0][1] = saved_chars[1];
    mTextAttrs[0][0] = saved_attrs[0];
    mTextAttrs[0][1] = saved_attrs[1];
    text_draw_cell(0, 0);
    text_draw_cell(1, 0);
    mConOutMode.CursorColumn = saved_column;
    mConOutMode.CursorRow = saved_row;
    mTextWrapPending = saved_wrap;
    st = efi_conout_set_attribute(&mConOutProto, 0x1fU);
    if (st != EFI_SUCCESS || mConOutMode.Attribute != 0x1f) {
        ok = 0;
    }
    st = efi_conout_set_attribute(&mConOutProto, 0x80U);
    if (st != EFI_INVALID_PARAMETER || mConOutMode.Attribute != 0x1f) {
        ok = 0;
    }
    mConOutMode.Attribute = saved_attribute;
    return ok;
}

/* --- Console Input Protocol ------------------------------------------------ */

typedef struct {
    UINT8 ScanCode;
    UINT16 EfiScanCode;
} PS2_EFI_SCAN_MAP;

static const PS2_EFI_SCAN_MAP mPs2ExtendedEfiScanMap[] = {
    { 0x47, EFI_SCAN_HOME },
    { 0x48, EFI_SCAN_UP },
    { 0x49, EFI_SCAN_PAGE_UP },
    { 0x4b, EFI_SCAN_LEFT },
    { 0x4d, EFI_SCAN_RIGHT },
    { 0x4f, EFI_SCAN_END },
    { 0x50, EFI_SCAN_DOWN },
    { 0x51, EFI_SCAN_PAGE_DOWN },
    { 0x52, EFI_SCAN_INSERT },
    { 0x53, EFI_SCAN_DELETE },
    { 0x69, EFI_SCAN_END },
    { 0x6b, EFI_SCAN_LEFT },
    { 0x6c, EFI_SCAN_HOME },
    { 0x70, EFI_SCAN_INSERT },
    { 0x71, EFI_SCAN_DELETE },
    { 0x72, EFI_SCAN_DOWN },
    { 0x74, EFI_SCAN_RIGHT },
    { 0x75, EFI_SCAN_UP },
    { 0x7a, EFI_SCAN_PAGE_DOWN },
    { 0x7d, EFI_SCAN_PAGE_UP },
};

static const PS2_EFI_SCAN_MAP mPs2Set1EfiScanMap[] = {
    { 0x3b, EFI_SCAN_F1 },
    { 0x3c, EFI_SCAN_F2 },
    { 0x3d, EFI_SCAN_F3 },
    { 0x3e, EFI_SCAN_F4 },
    { 0x3f, EFI_SCAN_F5 },
    { 0x40, EFI_SCAN_F6 },
    { 0x41, EFI_SCAN_F7 },
    { 0x42, EFI_SCAN_F8 },
    { 0x43, EFI_SCAN_F9 },
    { 0x44, EFI_SCAN_F10 },
    { 0x57, EFI_SCAN_F11 },
    { 0x58, EFI_SCAN_F12 },
};

static const PS2_EFI_SCAN_MAP mPs2Set2EfiScanMap[] = {
    { 0x05, EFI_SCAN_F1 },
    { 0x06, EFI_SCAN_F2 },
    { 0x04, EFI_SCAN_F3 },
    { 0x0c, EFI_SCAN_F4 },
    { 0x03, EFI_SCAN_F5 },
    { 0x0b, EFI_SCAN_F6 },
    { 0x83, EFI_SCAN_F7 },
    { 0x0a, EFI_SCAN_F8 },
    { 0x01, EFI_SCAN_F9 },
    { 0x09, EFI_SCAN_F10 },
    { 0x78, EFI_SCAN_F11 },
    { 0x07, EFI_SCAN_F12 },
};

static UINT16 ps2_lookup_efi_scan(const PS2_EFI_SCAN_MAP *Map,
                                  UINTN Count, UINT8 ScanCode)
{
    UINTN i;

    for (i = 0; i < Count; i++) {
        if (Map[i].ScanCode == ScanCode) {
            return Map[i].EfiScanCode;
        }
    }
    return 0;
}

static BOOLEAN __attribute__((noinline, used)) ps2_shift_scan_code(UINT8 code)
{
    if (mPs2Translated) {
        return code == 0x2a || code == 0x36;
    }
    return code == 0x12 || code == 0x59;
}

static UINT32 ps2_modifier_state_bit(UINT8 code, BOOLEAN extended)
{
    if (mPs2Translated) {
        if (!extended) {
            switch (code) {
            case 0x2a: return EFI_LEFT_SHIFT_PRESSED;
            case 0x36: return EFI_RIGHT_SHIFT_PRESSED;
            case 0x1d: return EFI_LEFT_CONTROL_PRESSED;
            case 0x38: return EFI_LEFT_ALT_PRESSED;
            default: return 0;
            }
        }
        switch (code) {
        case 0x1d: return EFI_RIGHT_CONTROL_PRESSED;
        case 0x38: return EFI_RIGHT_ALT_PRESSED;
        case 0x5b: return EFI_LEFT_LOGO_PRESSED;
        case 0x5c: return EFI_RIGHT_LOGO_PRESSED;
        case 0x5d: return EFI_MENU_KEY_PRESSED;
        default: return 0;
        }
    }

    if (!extended) {
        switch (code) {
        case 0x12: return EFI_LEFT_SHIFT_PRESSED;
        case 0x59: return EFI_RIGHT_SHIFT_PRESSED;
        case 0x14: return EFI_LEFT_CONTROL_PRESSED;
        case 0x11: return EFI_LEFT_ALT_PRESSED;
        default: return 0;
        }
    }
    switch (code) {
    case 0x14: return EFI_RIGHT_CONTROL_PRESSED;
    case 0x11: return EFI_RIGHT_ALT_PRESSED;
    case 0x1f: return EFI_LEFT_LOGO_PRESSED;
    case 0x27: return EFI_RIGHT_LOGO_PRESSED;
    case 0x2f: return EFI_MENU_KEY_PRESSED;
    default: return 0;
    }
}

static void ps2_update_modifier_state(UINT32 bit, BOOLEAN pressed)
{
    if (pressed) {
        mPs2ModifierState |= bit;
    } else {
        mPs2ModifierState &= ~bit;
    }
    mPs2Shift = (mPs2ModifierState &
                  (EFI_LEFT_SHIFT_PRESSED | EFI_RIGHT_SHIFT_PRESSED)) != 0;
}

static CHAR16 ps2_set2_to_char(UINT8 code)
{
    switch (code) {
    case 0x1c: return mPs2Shift ? 'A' : 'a';
    case 0x32: return mPs2Shift ? 'B' : 'b';
    case 0x21: return mPs2Shift ? 'C' : 'c';
    case 0x23: return mPs2Shift ? 'D' : 'd';
    case 0x24: return mPs2Shift ? 'E' : 'e';
    case 0x2b: return mPs2Shift ? 'F' : 'f';
    case 0x34: return mPs2Shift ? 'G' : 'g';
    case 0x33: return mPs2Shift ? 'H' : 'h';
    case 0x43: return mPs2Shift ? 'I' : 'i';
    case 0x3b: return mPs2Shift ? 'J' : 'j';
    case 0x42: return mPs2Shift ? 'K' : 'k';
    case 0x4b: return mPs2Shift ? 'L' : 'l';
    case 0x3a: return mPs2Shift ? 'M' : 'm';
    case 0x31: return mPs2Shift ? 'N' : 'n';
    case 0x44: return mPs2Shift ? 'O' : 'o';
    case 0x4d: return mPs2Shift ? 'P' : 'p';
    case 0x15: return mPs2Shift ? 'Q' : 'q';
    case 0x2d: return mPs2Shift ? 'R' : 'r';
    case 0x1b: return mPs2Shift ? 'S' : 's';
    case 0x2c: return mPs2Shift ? 'T' : 't';
    case 0x3c: return mPs2Shift ? 'U' : 'u';
    case 0x2a: return mPs2Shift ? 'V' : 'v';
    case 0x1d: return mPs2Shift ? 'W' : 'w';
    case 0x22: return mPs2Shift ? 'X' : 'x';
    case 0x35: return mPs2Shift ? 'Y' : 'y';
    case 0x1a: return mPs2Shift ? 'Z' : 'z';
    case 0x16: return mPs2Shift ? '!' : '1';
    case 0x1e: return mPs2Shift ? '@' : '2';
    case 0x26: return mPs2Shift ? '#' : '3';
    case 0x25: return mPs2Shift ? '$' : '4';
    case 0x2e: return mPs2Shift ? '%' : '5';
    case 0x36: return mPs2Shift ? '^' : '6';
    case 0x3d: return mPs2Shift ? '&' : '7';
    case 0x3e: return mPs2Shift ? '*' : '8';
    case 0x46: return mPs2Shift ? '(' : '9';
    case 0x45: return mPs2Shift ? ')' : '0';
    case 0x29: return ' ';
    case 0x5a: return '\r';
    case 0x66: return '\b';
    case 0x0d: return '\t';
    case 0x4e: return mPs2Shift ? '_' : '-';
    case 0x55: return mPs2Shift ? '+' : '=';
    case 0x54: return mPs2Shift ? '{' : '[';
    case 0x5b: return mPs2Shift ? '}' : ']';
    case 0x4c: return mPs2Shift ? ':' : ';';
    case 0x52: return mPs2Shift ? '"' : '\'';
    case 0x0e: return mPs2Shift ? '~' : '`';
    case 0x41: return mPs2Shift ? '<' : ',';
    case 0x49: return mPs2Shift ? '>' : '.';
    case 0x4a: return mPs2Shift ? '?' : '/';
    case 0x5d: return mPs2Shift ? '|' : '\\';
    default: return 0;
    }
}

static CHAR16 ps2_set1_to_char(UINT8 code)
{
    switch (code) {
    case 0x1e: return mPs2Shift ? 'A' : 'a';
    case 0x30: return mPs2Shift ? 'B' : 'b';
    case 0x2e: return mPs2Shift ? 'C' : 'c';
    case 0x20: return mPs2Shift ? 'D' : 'd';
    case 0x12: return mPs2Shift ? 'E' : 'e';
    case 0x21: return mPs2Shift ? 'F' : 'f';
    case 0x22: return mPs2Shift ? 'G' : 'g';
    case 0x23: return mPs2Shift ? 'H' : 'h';
    case 0x17: return mPs2Shift ? 'I' : 'i';
    case 0x24: return mPs2Shift ? 'J' : 'j';
    case 0x25: return mPs2Shift ? 'K' : 'k';
    case 0x26: return mPs2Shift ? 'L' : 'l';
    case 0x32: return mPs2Shift ? 'M' : 'm';
    case 0x31: return mPs2Shift ? 'N' : 'n';
    case 0x18: return mPs2Shift ? 'O' : 'o';
    case 0x19: return mPs2Shift ? 'P' : 'p';
    case 0x10: return mPs2Shift ? 'Q' : 'q';
    case 0x13: return mPs2Shift ? 'R' : 'r';
    case 0x1f: return mPs2Shift ? 'S' : 's';
    case 0x14: return mPs2Shift ? 'T' : 't';
    case 0x16: return mPs2Shift ? 'U' : 'u';
    case 0x2f: return mPs2Shift ? 'V' : 'v';
    case 0x11: return mPs2Shift ? 'W' : 'w';
    case 0x2d: return mPs2Shift ? 'X' : 'x';
    case 0x15: return mPs2Shift ? 'Y' : 'y';
    case 0x2c: return mPs2Shift ? 'Z' : 'z';
    case 0x02: return mPs2Shift ? '!' : '1';
    case 0x03: return mPs2Shift ? '@' : '2';
    case 0x04: return mPs2Shift ? '#' : '3';
    case 0x05: return mPs2Shift ? '$' : '4';
    case 0x06: return mPs2Shift ? '%' : '5';
    case 0x07: return mPs2Shift ? '^' : '6';
    case 0x08: return mPs2Shift ? '&' : '7';
    case 0x09: return mPs2Shift ? '*' : '8';
    case 0x0a: return mPs2Shift ? '(' : '9';
    case 0x0b: return mPs2Shift ? ')' : '0';
    case 0x39: return ' ';
    case 0x1c: return '\r';
    case 0x0e: return '\b';
    case 0x0f: return '\t';
    case 0x0c: return mPs2Shift ? '_' : '-';
    case 0x0d: return mPs2Shift ? '+' : '=';
    case 0x1a: return mPs2Shift ? '{' : '[';
    case 0x1b: return mPs2Shift ? '}' : ']';
    case 0x27: return mPs2Shift ? ':' : ';';
    case 0x28: return mPs2Shift ? '"' : '\'';
    case 0x29: return mPs2Shift ? '~' : '`';
    case 0x33: return mPs2Shift ? '<' : ',';
    case 0x34: return mPs2Shift ? '>' : '.';
    case 0x35: return mPs2Shift ? '?' : '/';
    case 0x2b: return mPs2Shift ? '|' : '\\';
    default: return 0;
    }
}

static EFI_STATUS ps2_read_key(EFI_INPUT_KEY *Key)
{
    UINTN limit;

    for (limit = 0; limit < 8; limit++) {
        UINT8 status;
        UINT8 code;

        if (ps2_keyboard_raw_pop(&code)) {
            status = 0;
        } else {
            status = ps2_read_status();
            if ((status & PS2_STATUS_OBF) == 0) {
                return EFI_NOT_READY;
            }
            code = *ps2_reg(PS2_DATA_PORT);
        }
        if ((status & PS2_STATUS_MOUSE_OBF) != 0) {
            fw_pointer_consume_byte(code);
            continue;
        }

        if (code == 0xe0) {
            mPs2Extended = 1;
            continue;
        }
        if (!mPs2Translated && code == 0xf0) {
            mPs2Break = 1;
            continue;
        }

        if (mPs2Translated && (code & 0x80) != 0) {
            UINT32 modifier;

            code &= 0x7f;
            modifier = ps2_modifier_state_bit(code, mPs2Extended);
            if (modifier != 0) {
                ps2_update_modifier_state(modifier, 0);
            }
            mPs2Break = 0;
            mPs2Extended = 0;
            continue;
        }

        if (mPs2Break) {
            UINT32 modifier =
                ps2_modifier_state_bit(code, mPs2Extended);

            if (modifier != 0) {
                ps2_update_modifier_state(modifier, 0);
            }
            mPs2Break = 0;
            mPs2Extended = 0;
            continue;
        }

        {
            UINT32 modifier =
                ps2_modifier_state_bit(code, mPs2Extended);

            if (modifier != 0) {
                ps2_update_modifier_state(modifier, 1);
                mPs2Extended = 0;
                continue;
            }
        }

        Key->ScanCode = 0;
        Key->UnicodeChar = 0;
        if (mPs2Extended) {
            Key->ScanCode = ps2_lookup_efi_scan(mPs2ExtendedEfiScanMap,
                                                FW_ARRAY_SIZE(mPs2ExtendedEfiScanMap),
                                                code);
        } else if ((mPs2Translated && code == 0x01) ||
                   (!mPs2Translated && code == 0x76)) {
            Key->ScanCode = EFI_SCAN_ESC;
        } else if (mPs2Translated) {
            Key->ScanCode = ps2_lookup_efi_scan(mPs2Set1EfiScanMap,
                                                FW_ARRAY_SIZE(mPs2Set1EfiScanMap),
                                                code);
        } else {
            Key->ScanCode = ps2_lookup_efi_scan(mPs2Set2EfiScanMap,
                                                FW_ARRAY_SIZE(mPs2Set2EfiScanMap),
                                                code);
        }
        if (Key->ScanCode == 0) {
            Key->UnicodeChar = mPs2Translated ?
                ps2_set1_to_char(code) : ps2_set2_to_char(code);
        }
        mPs2Extended = 0;
        if (Key->ScanCode != 0 || Key->UnicodeChar != 0) {
            fw_set_mem(&mConInCurrentKeyState,
                       sizeof(mConInCurrentKeyState), 0);
            mConInCurrentKeyState.KeyShiftState =
                EFI_SHIFT_STATE_VALID | mPs2ModifierState;
            return EFI_SUCCESS;
        }
    }

    return EFI_NOT_READY;
}

/*
 * Wait briefly for the next serial byte.  Escape-sequence bytes for a single
 * key (e.g. ESC '[' 'B') arrive as a burst, so a bounded spin suffices to tell
 * a multi-byte sequence apart from a lone ESC keypress without hanging ConIn.
 */
static BOOLEAN conin_uart_read_wait(UINT8 *ch)
{
    UINT32 spin;

    for (spin = 0; spin < 200000U; spin++) {
        if (uart_can_read()) {
            *ch = uart_getc();
            return 1;
        }
    }
    return 0;
}

UINT16 conin_ansi_numeric_scan(UINTN Number)
{
    switch (Number) {
    case 1:  return EFI_SCAN_HOME;
    case 2:  return EFI_SCAN_INSERT;
    case 3:  return EFI_SCAN_DELETE;
    case 4:  return EFI_SCAN_END;
    case 5:  return EFI_SCAN_PAGE_UP;
    case 6:  return EFI_SCAN_PAGE_DOWN;
    case 11: return EFI_SCAN_F1;
    case 12: return EFI_SCAN_F2;
    case 13: return EFI_SCAN_F3;
    case 14: return EFI_SCAN_F4;
    case 15: return EFI_SCAN_F5;
    case 17: return EFI_SCAN_F6;
    case 18: return EFI_SCAN_F7;
    case 19: return EFI_SCAN_F8;
    case 20: return EFI_SCAN_F9;
    case 21: return EFI_SCAN_F10;
    case 23: return EFI_SCAN_F11;
    case 24: return EFI_SCAN_F12;
    default: return 0;
    }
}

static EFI_STATUS conin_read_device_key(EFI_INPUT_KEY *Key)
{
    UINT8 ch;

    fw_set_mem(&mConInCurrentKeyState, sizeof(mConInCurrentKeyState), 0);

    if (uart_can_read()) {
        ch = uart_getc();
        Key->ScanCode = 0;
        Key->UnicodeChar = 0;

        if (ch == 0x1b) {
            /*
             * Translate VT100/ANSI escape sequences from a serial console into
             * EFI scan codes so menu navigation (arrows, Home/End, etc.) works
             * over serial as it does with the PS/2 keyboard.  A lone ESC (no
             * following byte) is reported as EFI_SCAN_ESC.
             */
            UINT8 b1;

            if (!conin_uart_read_wait(&b1)) {
                Key->ScanCode = EFI_SCAN_ESC;
                return EFI_SUCCESS;
            }
            if (b1 == '[' || b1 == 'O') {
                UINT8 b2;

                if (conin_uart_read_wait(&b2)) {
                    switch (b2) {
                    case 'A': Key->ScanCode = EFI_SCAN_UP;    return EFI_SUCCESS;
                    case 'B': Key->ScanCode = EFI_SCAN_DOWN;  return EFI_SUCCESS;
                    case 'C': Key->ScanCode = EFI_SCAN_RIGHT; return EFI_SUCCESS;
                    case 'D': Key->ScanCode = EFI_SCAN_LEFT;  return EFI_SUCCESS;
                    case 'H': Key->ScanCode = EFI_SCAN_HOME;  return EFI_SUCCESS;
                    case 'F': Key->ScanCode = EFI_SCAN_END;   return EFI_SUCCESS;
                    default:
                        break;
                    }
                    if (b1 == 'O' && b2 >= 'P' && b2 <= 'S') {
                        Key->ScanCode = (UINT16)(EFI_SCAN_F1 + b2 - 'P');
                        return EFI_SUCCESS;
                    }
                    if (b1 == '[' && b2 == '[' &&
                        conin_uart_read_wait(&b2) &&
                        b2 >= 'A' && b2 <= 'E') {
                        Key->ScanCode = (UINT16)(EFI_SCAN_F1 + b2 - 'A');
                        return EFI_SUCCESS;
                    }
                    if (b1 == '[' && b2 >= '0' && b2 <= '9') {
                        UINTN number = b2 - '0';
                        UINTN digits;

                        for (digits = 1; digits < 3U; digits++) {
                            if (!conin_uart_read_wait(&b2)) {
                                break;
                            }
                            if (b2 == '~') {
                                Key->ScanCode =
                                    conin_ansi_numeric_scan(number);
                                if (Key->ScanCode != 0) {
                                    return EFI_SUCCESS;
                                }
                                break;
                            }
                            if (b2 < '0' || b2 > '9') {
                                break;
                            }
                            number = number * 10U + b2 - '0';
                        }
                    }
                }
                /* Unrecognized sequence: surface ESC rather than lose it. */
                Key->ScanCode = EFI_SCAN_ESC;
                return EFI_SUCCESS;
            }

            /* ESC followed by an ordinary byte: report the Escape key. */
            Key->ScanCode = EFI_SCAN_ESC;
            return EFI_SUCCESS;
        }

        Key->UnicodeChar = (ch == '\n') ? '\r' : (CHAR16)ch;
        return EFI_SUCCESS;
    }

    if (usb_keyboard_read_key(Key) == EFI_SUCCESS) {
        return EFI_SUCCESS;
    }

    return ps2_read_key(Key);
}

EFI_STATUS conin_peek_key(EFI_INPUT_KEY *Key)
{
    EFI_STATUS st;

    if (mConInBufferedKeyValid) {
        mConInCurrentKeyState = mConInBufferedKeyState;
        if (Key != NULL) {
            *Key = mConInBufferedKey;
        }
        return EFI_SUCCESS;
    }

    st = conin_read_device_key(&mConInBufferedKey);
    if (st != EFI_SUCCESS) {
        return st;
    }
    mConInBufferedKeyValid = 1;
    mConInBufferedKeyState = mConInCurrentKeyState;
    if (Key != NULL) {
        *Key = mConInBufferedKey;
    }
    return EFI_SUCCESS;
}

BOOLEAN conin_key_available(void)
{
    return conin_peek_key(NULL) == EFI_SUCCESS;
}

static void conin_fill_key_data(EFI_KEY_DATA *KeyData,
                                const EFI_INPUT_KEY *Key)
{
    fw_set_mem(KeyData, sizeof(*KeyData), 0);
    KeyData->Key = *Key;
    KeyData->KeyState = mConInCurrentKeyState;
}

static BOOLEAN conin_key_notify_matches(const EFI_KEY_DATA *Registered,
                                        const EFI_KEY_DATA *KeyData)
{
    BOOLEAN has_match_field = 0;

    if (Registered->Key.ScanCode != 0) {
        has_match_field = 1;
        if (Registered->Key.ScanCode != KeyData->Key.ScanCode) {
            return 0;
        }
    }
    if (Registered->Key.UnicodeChar != 0) {
        has_match_field = 1;
        if (Registered->Key.UnicodeChar != KeyData->Key.UnicodeChar) {
            return 0;
        }
    }
    if (Registered->KeyState.KeyShiftState != 0) {
        has_match_field = 1;
        if (Registered->KeyState.KeyShiftState !=
            KeyData->KeyState.KeyShiftState) {
            return 0;
        }
    }
    if (Registered->KeyState.KeyToggleState != 0) {
        has_match_field = 1;
        if (Registered->KeyState.KeyToggleState !=
            KeyData->KeyState.KeyToggleState) {
            return 0;
        }
    }

    return has_match_field;
}

static void conin_dispatch_key_notifications(const EFI_KEY_DATA *KeyData)
{
    UINTN i;

    for (i = 0; i < FW_ARRAY_SIZE(mConInKeyNotifyRecords); i++) {
        FW_CONIN_KEY_NOTIFY_RECORD *rec = &mConInKeyNotifyRecords[i];

        if (rec->in_use &&
            conin_key_notify_matches(&rec->key_data, KeyData)) {
            EFI_TPL old_tpl = mCurrentTpl;

            if (mCurrentTpl < TPL_CALLBACK) {
                mCurrentTpl = TPL_CALLBACK;
            }
            (void)rec->notify((EFI_KEY_DATA *)KeyData);
            mCurrentTpl = old_tpl;
        }
    }
}

static EFI_STATUS conin_reset(EFI_SIMPLE_TEXT_INPUT_PROTOCOL *This,
                               BOOLEAN ExtendedVerification)
{
    (void)This; (void)ExtendedVerification;
    mConInBufferedKeyValid = 0;
    fw_set_mem(&mConInBufferedKey, sizeof(mConInBufferedKey), 0);
    fw_set_mem(&mConInBufferedKeyState, sizeof(mConInBufferedKeyState), 0);
    fw_set_mem(&mConInCurrentKeyState, sizeof(mConInCurrentKeyState), 0);
    mUsbKeyboardTried = 0;
    mUsbKeyboardReady = 0;
    fw_set_mem(mUsbKeyboardPreviousReport,
               sizeof(mUsbKeyboardPreviousReport), 0);
    ps2_init_controller();
    return EFI_SUCCESS;
}

EFI_STATUS conin_read_key_raw(EFI_INPUT_KEY *Key)
{
    if (Key == NULL) {
        return EFI_INVALID_PARAMETER;
    }
    if (mConInBufferedKeyValid) {
        *Key = mConInBufferedKey;
        mConInCurrentKeyState = mConInBufferedKeyState;
        mConInBufferedKeyValid = 0;
        fw_set_mem(&mConInBufferedKey, sizeof(mConInBufferedKey), 0);
        fw_set_mem(&mConInBufferedKeyState,
                   sizeof(mConInBufferedKeyState), 0);
        return EFI_SUCCESS;
    }

    return conin_read_device_key(Key);
}

static EFI_STATUS conin_read_key(EFI_SIMPLE_TEXT_INPUT_PROTOCOL *This,
                                  EFI_INPUT_KEY *Key)
{
    EFI_STATUS st;

    (void)This;
    st = conin_read_key_raw(Key);
    if (st == EFI_SUCCESS) {
        EFI_KEY_DATA key_data;

        conin_fill_key_data(&key_data, Key);
        conin_dispatch_key_notifications(&key_data);
    }
    return st;
}

static EFI_STATUS conin_ex_reset(EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *This,
                                 BOOLEAN ExtendedVerification)
{
    (void)This;
    return conin_reset(&mConInProto, ExtendedVerification);
}

static EFI_STATUS conin_ex_read_key(EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *This,
                                    EFI_KEY_DATA *KeyData)
{
    EFI_INPUT_KEY key;
    EFI_STATUS st;

    (void)This;
    if (KeyData == NULL) {
        return EFI_INVALID_PARAMETER;
    }
    st = conin_read_key_raw(&key);
    if (st != EFI_SUCCESS) {
        fw_set_mem(KeyData, sizeof(*KeyData), 0);
        return st;
    }
    conin_fill_key_data(KeyData, &key);
    conin_dispatch_key_notifications(KeyData);
    return EFI_SUCCESS;
}

static EFI_STATUS conin_ex_set_state(EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *This,
                                     EFI_KEY_TOGGLE_STATE *KeyToggleState)
{
    EFI_KEY_TOGGLE_STATE state;

    (void)This;
    if (KeyToggleState == NULL) {
        return EFI_INVALID_PARAMETER;
    }
    state = *KeyToggleState;
    if ((state & ~(EFI_TOGGLE_STATE_VALID)) != 0) {
        return EFI_UNSUPPORTED;
    }
    return EFI_SUCCESS;
}

static EFI_STATUS conin_ex_register_key_notify(
    EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *This, EFI_KEY_DATA *KeyData,
    EFI_KEY_NOTIFY_FUNCTION KeyNotificationFunction, VOID **NotifyHandle)
{
    UINTN i;

    (void)This;
    if (KeyData == NULL || KeyNotificationFunction == NULL ||
        NotifyHandle == NULL) {
        return EFI_INVALID_PARAMETER;
    }
    for (i = 0; i < FW_ARRAY_SIZE(mConInKeyNotifyRecords); i++) {
        FW_CONIN_KEY_NOTIFY_RECORD *rec = &mConInKeyNotifyRecords[i];

        if (!rec->in_use) {
            rec->in_use = 1;
            rec->key_data = *KeyData;
            rec->notify = KeyNotificationFunction;
            *NotifyHandle = rec;
            return EFI_SUCCESS;
        }
    }
    return EFI_OUT_OF_RESOURCES;
}

static EFI_STATUS conin_ex_unregister_key_notify(
    EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *This, VOID *NotificationHandle)
{
    UINTN i;

    (void)This;
    if (NotificationHandle == NULL) {
        return EFI_INVALID_PARAMETER;
    }
    for (i = 0; i < FW_ARRAY_SIZE(mConInKeyNotifyRecords); i++) {
        FW_CONIN_KEY_NOTIFY_RECORD *rec = &mConInKeyNotifyRecords[i];

        if (NotificationHandle == rec && rec->in_use) {
            fw_set_mem(rec, sizeof(*rec), 0);
            return EFI_SUCCESS;
        }
    }
    return EFI_INVALID_PARAMETER;
}

EFI_SIMPLE_TEXT_INPUT_PROTOCOL mConInProto = {
    .Reset         = conin_reset,
    .ReadKeyStroke = conin_read_key,
    .WaitForKey    = NULL,
};

EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL mConInExProto = {
    .Reset = conin_ex_reset,
    .ReadKeyStrokeEx = conin_ex_read_key,
    .WaitForKeyEx = NULL,
    .SetState = conin_ex_set_state,
    .RegisterKeyNotify = conin_ex_register_key_notify,
    .UnregisterKeyNotify = conin_ex_unregister_key_notify,
};

const UINT8 mConOutProtocolGuid[16] = {
    0xc2, 0x77, 0x74, 0x38, 0xc7, 0x69, 0xd2, 0x11,
    0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b
};

const UINT8 mConInProtocolGuid[16] = {
    0xc1, 0x77, 0x74, 0x38, 0xc7, 0x69, 0xd2, 0x11,
    0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b
};

const UINT8 mConInExProtocolGuid[16] = {
    0x34, 0x75, 0x9e, 0xdd, 0x62, 0x77, 0x98, 0x46,
    0x8c, 0x14, 0xf5, 0x85, 0x17, 0xa6, 0x25, 0xaa
};



void efi_init_conout(void)
{
    mConOutMode.MaxMode = 1;
    mConOutMode.Mode = 0;
    mConOutMode.Attribute = 0x07;
    mConOutMode.CursorColumn = 0;
    mConOutMode.CursorRow = 0;
    mConOutMode.CursorVisible = 0;

    mConOutProto.Reset          = efi_conout_reset;
    mConOutProto.OutputString   = efi_conout_string;
    mConOutProto.TestString     = efi_conout_test_string;
    mConOutProto.QueryMode      = efi_conout_query_mode;
    mConOutProto.SetMode        = efi_conout_set_mode;
    mConOutProto.SetAttribute   = efi_conout_set_attribute;
    mConOutProto.ClearScreen    = efi_conout_clear_screen;
    mConOutProto.SetCursorPosition = efi_conout_set_cursor;
    mConOutProto.EnableCursor   = efi_conout_enable_cursor;
    mConOutProto.Mode           = &mConOutMode;
    text_clear_screen();

    efi_init_conin_wait_events();
}

BOOLEAN __attribute__((noinline)) uefi_conin_buffer_selftest(void)
{
    BOOLEAN saved_valid = mConInBufferedKeyValid;
    EFI_INPUT_KEY saved_key = mConInBufferedKey;
    EFI_KEY_STATE saved_buffered_state = mConInBufferedKeyState;
    EFI_KEY_STATE saved_current_state = mConInCurrentKeyState;
    EFI_INPUT_KEY peek;
    EFI_INPUT_KEY key;
    BOOLEAN ok = 1;

    mConInBufferedKeyValid = 1;
    mConInBufferedKey.ScanCode = 0;
    mConInBufferedKey.UnicodeChar = 'X';
    fw_set_mem(&mConInBufferedKeyState, sizeof(mConInBufferedKeyState), 0);
    mConInBufferedKeyState.KeyShiftState =
        EFI_SHIFT_STATE_VALID | EFI_LEFT_SHIFT_PRESSED;

    if (!conin_key_available() ||
        conin_peek_key(&peek) != EFI_SUCCESS ||
        peek.ScanCode != 0 ||
        peek.UnicodeChar != 'X' ||
        !mConInBufferedKeyValid ||
        conin_read_key_raw(&key) != EFI_SUCCESS ||
        key.ScanCode != 0 ||
        key.UnicodeChar != 'X' ||
        mConInCurrentKeyState.KeyShiftState !=
            (EFI_SHIFT_STATE_VALID | EFI_LEFT_SHIFT_PRESSED) ||
        mConInBufferedKeyValid) {
        ok = 0;
    }

    mConInBufferedKeyValid = saved_valid;
    mConInBufferedKey = saved_key;
    mConInBufferedKeyState = saved_buffered_state;
    mConInCurrentKeyState = saved_current_state;
    return ok;
}

BOOLEAN __attribute__((noinline)) uefi_ps2_scancode_selftest(void)
{
    BOOLEAN saved_break = mPs2Break;
    BOOLEAN saved_extended = mPs2Extended;
    BOOLEAN saved_shift = mPs2Shift;
    UINT32 saved_modifier_state = mPs2ModifierState;
    BOOLEAN saved_translated = mPs2Translated;
    BOOLEAN ok = 1;

    mPs2Break = 0;
    mPs2Extended = 0;
    mPs2Translated = 1;
    mPs2Shift = 0;
    mPs2ModifierState = 0;
    if (ps2_shift_scan_code(0x12) ||
        !ps2_shift_scan_code(0x2a) ||
        !ps2_shift_scan_code(0x36) ||
        ps2_modifier_state_bit(0x2a, 0) != EFI_LEFT_SHIFT_PRESSED ||
        ps2_modifier_state_bit(0x36, 0) != EFI_RIGHT_SHIFT_PRESSED ||
        ps2_set1_to_char(0x12) != 'e' ||
        ps2_lookup_efi_scan(mPs2Set1EfiScanMap,
                            FW_ARRAY_SIZE(mPs2Set1EfiScanMap),
                            0x58) != EFI_SCAN_F12) {
        ok = 0;
    }
    mPs2Shift = 1;
    if (ps2_set1_to_char(0x12) != 'E') {
        ok = 0;
    }

    mPs2Translated = 0;
    mPs2Shift = 0;
    mPs2ModifierState = 0;
    if (!ps2_shift_scan_code(0x12) ||
        !ps2_shift_scan_code(0x59) ||
        ps2_shift_scan_code(0x2a) ||
        ps2_modifier_state_bit(0x12, 0) != EFI_LEFT_SHIFT_PRESSED ||
        ps2_modifier_state_bit(0x59, 0) != EFI_RIGHT_SHIFT_PRESSED ||
        ps2_set2_to_char(0x24) != 'e' ||
        ps2_lookup_efi_scan(mPs2Set2EfiScanMap,
                            FW_ARRAY_SIZE(mPs2Set2EfiScanMap),
                            0x83) != EFI_SCAN_F7) {
        ok = 0;
    }
    mPs2Shift = 1;
    if (ps2_set2_to_char(0x24) != 'E') {
        ok = 0;
    }

    mPs2Break = saved_break;
    mPs2Extended = saved_extended;
    mPs2Shift = saved_shift;
    mPs2ModifierState = saved_modifier_state;
    mPs2Translated = saved_translated;
    return ok;
}

