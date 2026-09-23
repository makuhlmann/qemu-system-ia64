/*
 * QEMU ATI Mach64 "3D Rage" (DEV_4754) emulation
 *
 * A PCI display adapter with a VGA-compatible core (VGACommonState) plus the
 * classic Mach64 GUI (2D) engine.  Unlike the Rage 128 (hw/display/ati.c) this
 * chip has no command processor, ring, DMA or AGP: 2D is driven entirely by
 * synchronous MMIO register writes, so it never touches the 460GX AGP GART and
 * cannot hit the >4 GiB AGP-DRI hazard.  Guaranteed inbox drivers exist on
 * Windows XP SP1 / Server 2003 (ati.sys, PCI\VEN_1002&DEV_4754) and Debian
 * (atimisc/atyfb).  See plans/mach64-design.md.
 *
 * This work is licensed under the GNU GPL license version 2 or later.
 */

#include "qemu/osdep.h"
#include "qemu/cutils.h"
#include "mach64_int.h"
#include "mach64_regs.h"
#include "hw/core/qdev-properties.h"
#include "qemu/timer.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "migration/vmstate.h"
#include "ui/console.h"

enum { VGA_MODE, EXT_MODE };

static const GraphicHwOps *mach64_vga_hw_ops;
static GraphicHwOps mach64_hw_ops;

/*
 * Opt-in mode-set/scanout diagnostic (MACH64_TRACE=1 in the environment).
 * Logs the infrequent CRTC/DAC/clock register writes and the extended-mode
 * switch decision to stderr; never hooks a guest tight loop.
 */
static bool mach64_trace_on(void)
{
    static int on = -1;

    if (on < 0) {
        on = getenv("MACH64_TRACE") != NULL;
    }
    return on;
}

/* MACH64_TRACE=<n> raises the line cap; anything else keeps the default. */
#define MACH64_TRACE_LINE_CAP 40000UL

static unsigned long mach64_trace_cap(void)
{
    static unsigned long cap;

    if (cap == 0) {
        const char *v = getenv("MACH64_TRACE");
        uint64_t n = 0;

        if (v == NULL || qemu_strtou64(v, NULL, 0, &n) < 0 || n <= 1) {
            n = MACH64_TRACE_LINE_CAP;
        }
        cap = n;
    }
    return cap;
}
#define M64_TRACE(fmt, ...) do {                                        \
    if (mach64_trace_on()) {                                            \
        fprintf(stderr, "mach64: " fmt "\n", ##__VA_ARGS__);           \
    }                                                                   \
} while (0)

/* Deduplicated register-access trace (collapses poll loops). */
static void mach64_trace_access(char rw, unsigned reg, uint32_t val)
{
    static char last_rw;
    static unsigned last_reg = ~0u, rep;
    /*
     * Hard line cap: an A/B/A/B poll loop can defeat the consecutive-dedup
     * below, and this trace is reachable from a guest poll, so bound the output
     * so it cannot fill the disk (CLAUDE.md tight-loop rule).
     */
    static unsigned long emitted;

    if (!mach64_trace_on() || emitted >= mach64_trace_cap()) {
        return;
    }
    if (rw != last_rw || reg != last_reg) {
        if (rep) {
            fprintf(stderr, "mach64:   (repeated x%u)\n", rep);
            emitted++;
        }
        fprintf(stderr, "mach64: %c reg[%03x] %s %08x\n", rw, reg,
                rw == 'r' ? "->" : "<-", val);
        emitted++;
        if (emitted == mach64_trace_cap()) {
            fprintf(stderr, "mach64: (trace line cap reached, silencing)\n");
        }
        last_rw = rw;
        last_reg = reg;
        rep = 0;
    } else {
        rep++;
    }
}

#ifdef CONFIG_PIXMAN
#define MACH64_DEFAULT_PIXMAN 3
#else
#define MACH64_DEFAULT_PIXMAN 0
#endif

/* Bytes per pixel for a Mach64 PIX_WIDTH code. */
static int mach64_pixw_bytes(unsigned code)
{
    switch (code) {
    case PIX_WIDTH_8BPP:
        return 1;
    case PIX_WIDTH_15BPP:
    case PIX_WIDTH_16BPP:
        return 2;
    case PIX_WIDTH_24BPP:
        return 3;
    case PIX_WIDTH_32BPP:
        return 4;
    default:
        return 0;
    }
}

int mach64_dst_bpp(const Mach64VGAState *s)
{
    return mach64_pixw_bytes(s->regs[DP_PIX_WIDTH] & DP_DST_PIX_WIDTH) * 8;
}

uint32_t mach64_dst_base(const Mach64VGAState *s)
{
    return (s->regs[DST_OFF_PITCH] & 0x000fffff) * 8;
}

int mach64_dst_pitch_bytes(const Mach64VGAState *s)
{
    int pitch_px = ((s->regs[DST_OFF_PITCH] >> CRTC_PITCH_SHIFT) & 0x3ff) * 8;
    int bypp = mach64_pixw_bytes(s->regs[DP_PIX_WIDTH] & DP_DST_PIX_WIDTH);

    return pitch_px * bypp;
}

void mach64_2d_set_dirty(Mach64VGAState *s, uint32_t base, int x, int y,
                         int w, int h)
{
    int bypp = mach64_pixw_bytes(s->regs[DP_PIX_WIDTH] & DP_DST_PIX_WIDTH);
    int pitch = mach64_dst_pitch_bytes(s);
    hwaddr start, end;

    /* Only the on-screen part of a rectangle drawn past the top or left. */
    if (x < 0) {
        w += x;
        x = 0;
    }
    if (y < 0) {
        h += y;
        y = 0;
    }
    if (w <= 0 || h <= 0 || bypp == 0 || pitch == 0) {
        return;
    }
    start = base + (hwaddr)y * pitch + (hwaddr)x * bypp;
    end = start + (hwaddr)(h - 1) * pitch + (hwaddr)w * bypp;
    if (end > s->vga.vram_size) {
        end = s->vga.vram_size;
    }
    if (start >= end) {
        return;
    }
    memory_region_set_dirty(&s->vga.vram, start, end - start);
}

/*
 * Translate the extended-CRTC registers into a linear-framebuffer mode by
 * driving the VGACommonState VBE machinery, exactly as hw/display/ati.c does.
 * When the extended display enable is clear we fall back to the VGA core.
 */
static void mach64_switch_mode(Mach64VGAState *s)
{
    VGACommonState *vga = &s->vga;

    M64_TRACE("switch_mode gen_cntl=%08x ext=%d en=%d htd=%08x vtd=%08x "
              "off_pitch=%08x pixw=%08x dac_cntl=%08x",
              s->regs[CRTC_GEN_CNTL],
              !!(s->regs[CRTC_GEN_CNTL] & CRTC_EXT_DISP_EN),
              !!(s->regs[CRTC_GEN_CNTL] & CRTC_EN),
              s->regs[CRTC_H_TOTAL_DISP], s->regs[CRTC_V_TOTAL_DISP],
              s->regs[CRTC_OFF_PITCH], s->regs[DP_PIX_WIDTH],
              s->regs[DAC_CNTL]);

    if (!(s->regs[CRTC_GEN_CNTL] & CRTC_EXT_DISP_EN) ||
        !(s->regs[CRTC_GEN_CNTL] & CRTC_EN)) {
        s->mode = VGA_MODE;
        vbe_ioport_write_index(vga, 0, VBE_DISPI_INDEX_ENABLE);
        vbe_ioport_write_data(vga, 0, VBE_DISPI_DISABLED);
        M64_TRACE("  -> VGA_MODE (ext/en not both set)");
        return;
    }

    s->mode = EXT_MODE;
    /*
     * CRTC_EXT_DISP_EN selects the extended display over the VGA one (RAGE XL
     * RRG, CRTC_GEN_CNTL MM 0_07), so the attribute controller's PAS bit stops
     * gating the output -- but the VGA core keeps blanking the console while
     * that bit is clear (vga.c, GMODE_BLANK), and ati2drad programs the mode
     * through the extended registers alone.  hw/display/ati.c drives the same
     * bit from its own display enable.
     */
    vga->ar_index |= BIT(5);

    uint32_t htd = s->regs[CRTC_H_TOTAL_DISP];
    uint32_t vtd = s->regs[CRTC_V_TOTAL_DISP];
    int h = (((htd & CRTC_H_DISP) >> 16) + 1) * 8;
    int v = ((vtd & CRTC_V_DISP) >> 16) + 1;
    int pitch_px = ((s->regs[CRTC_OFF_PITCH] >> CRTC_PITCH_SHIFT) & 0x3ff) * 8;
    uint32_t offs = (s->regs[CRTC_OFF_PITCH] & CRTC_OFFSET_MASK) * 8;
    unsigned pw = (s->regs[CRTC_GEN_CNTL] & CRTC_PIX_WIDTH) >> CRTC_PIX_WIDTH_SHIFT;
    int bpp;

    switch (pw) {
    case PIX_WIDTH_8BPP:
        bpp = 8;
        break;
    case PIX_WIDTH_15BPP:
        bpp = 15;
        break;
    case PIX_WIDTH_16BPP:
        bpp = 16;
        break;
    case PIX_WIDTH_24BPP:
        bpp = 24;
        break;
    case PIX_WIDTH_32BPP:
        bpp = 32;
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "mach64: unsupported CRTC pix width %u\n", pw);
        M64_TRACE("  -> BAIL unsupported pix width %u", pw);
        return;
    }
    if (h <= 0 || v <= 0) {
        M64_TRACE("  -> BAIL bad geometry h=%d v=%d", h, v);
        return;
    }
    if (pitch_px <= 0) {
        pitch_px = h;
    }

    vbe_ioport_write_index(vga, 0, VBE_DISPI_INDEX_ENABLE);
    vbe_ioport_write_data(vga, 0, VBE_DISPI_DISABLED);
    vga->vbe_regs[VBE_DISPI_INDEX_XRES] = h;
    vga->vbe_regs[VBE_DISPI_INDEX_YRES] = v;
    vga->vbe_regs[VBE_DISPI_INDEX_BPP] = bpp;
    vbe_ioport_write_index(vga, 0, VBE_DISPI_INDEX_ENABLE);
    vbe_ioport_write_data(vga, 0, VBE_DISPI_ENABLED | VBE_DISPI_LFB_ENABLED |
                          VBE_DISPI_NOCLEARMEM |
                          (s->regs[DAC_CNTL] & DAC_8BIT_EN ?
                           VBE_DISPI_8BIT_DAC : 0));
    if (pitch_px) {
        vbe_ioport_write_index(vga, 0, VBE_DISPI_INDEX_VIRT_WIDTH);
        vbe_ioport_write_data(vga, 0, pitch_px);
    }
    int bypp = DIV_ROUND_UP(vga->vbe_regs[VBE_DISPI_INDEX_BPP], 8);
    bool off_ok = bypp && (uint64_t)v * pitch_px * bypp + offs <= vga->vbe_size;
    if (off_ok) {
        vga->vbe_start_addr = offs / 4;
    }
    M64_TRACE("  -> EXT_MODE %dx%d bpp=%d pitch_px=%d offs=%#x start=%#x "
              "off_ok=%d vbe_size=%#x", h, v, bpp, pitch_px, offs,
              vga->vbe_start_addr, off_ok, vga->vbe_size);
}

/* ---- hardware cursor ---- */

/*
 * "Each cursor pixel is defined by a 2-bit field": 00 cursor colour 0, 01
 * cursor colour 1, 10 transparent, 11 complement.  "Cursor pitch is always 64
 * pixels ... 16 bytes of data ... The pixel definition is specified in Intel
 * order.  The first pixel is defined in the low-order 2 bits of the low-order
 * byte in memory" (RAGE PRO PRG sec 6.4.3).
 *
 * The two halves of CUR_HORZ_VERT_OFF do not crop alike.  CUR_HORZ_OFF is a
 * shift: the first column displayed is CUR_HORZ_OFF, so the cursor is
 * 64 - CUR_HORZ_OFF wide.  Vertically there is no shift -- CUR_OFFSET already
 * addresses the first line to display, and CUR_VERT_OFF only shortens the
 * cursor to 64 - CUR_VERT_OFF lines.  The PRG's own SetHWCursorPos says so:
 * when the cursor runs off the top of the screen it raises CUR_VERT_OFF *and*
 * advances "CUR_OFFSET ... to point to the appropriate line in the cursor
 * definition", where the horizontal case moves CUR_HORZ_OFF alone.
 */
#define MACH64_CUR_CLR0         0
#define MACH64_CUR_CLR1         1
#define MACH64_CUR_TRANSPARENT  2

static unsigned mach64_cursor_pixel(const Mach64VGAState *s, uint32_t srcoff,
                                    unsigned row, unsigned col)
{
    uint8_t byte = s->vga.vram_ptr[srcoff + row * 16 + col / 4];

    return (byte >> ((col % 4) * 2)) & 3;
}

/*
 * "for pseudo color modes, the colors are specified in color indices, and for
 * direct color modes ... in 24-bit true color" (PRG sec 6.4.3).
 */
static uint32_t mach64_cursor_color(Mach64VGAState *s, uint32_t clr)
{
    unsigned pw = (s->regs[CRTC_GEN_CNTL] & CRTC_PIX_WIDTH) >>
                  CRTC_PIX_WIDTH_SHIFT;

    if (pw == PIX_WIDTH_8BPP) {
        return s->vga.last_palette[clr & 0xff] | 0xff000000;
    }
    return (clr & 0x00ffffff) | 0xff000000;
}

static void mach64_cursor_define(Mach64VGAState *s)
{
    uint32_t srcoff;
    unsigned hoff, voff;

    if (s->cursor_guest_mode) {
        return;
    }
    hoff = s->regs[CUR_HORZ_VERT_OFF] & 0x3f;
    voff = (s->regs[CUR_HORZ_VERT_OFF] >> 16) & 0x3f;
    srcoff = (s->regs[CUR_OFFSET] & 0x000fffff) * 8;
    if (srcoff > s->vga.vram_size - 64 * 16) {
        return;
    }
    if (!s->cursor) {
        s->cursor = cursor_alloc(64, 64);
    }
    for (unsigned y = 0; y < 64; y++) {
        for (unsigned x = 0; x < 64; x++) {
            uint32_t *px = &s->cursor->data[y * 64 + x];

            if (x + hoff >= 64 || y + voff >= 64) {
                *px = 0;
                continue;
            }
            switch (mach64_cursor_pixel(s, srcoff, y, x + hoff)) {
            case MACH64_CUR_CLR0:
                *px = mach64_cursor_color(s, s->regs[CUR_CLR0]);
                break;
            case MACH64_CUR_CLR1:
                *px = mach64_cursor_color(s, s->regs[CUR_CLR1]);
                break;
            case MACH64_CUR_TRANSPARENT:
                *px = 0;
                break;
            default:
                /* complement; ui/cursor.c marks such a pixel this way */
                *px = 0x80000000;
                break;
            }
        }
    }
    dpy_cursor_define(s->vga.con, s->cursor);
}

static bool mach64_cursor_enabled(Mach64VGAState *s)
{
    return !!(s->regs[GEN_TEST_CNTL] & GEN_CUR_EN);
}

static void mach64_cursor_invalidate(VGACommonState *vga)
{
    Mach64VGAState *s = container_of(vga, Mach64VGAState, vga);
    int size = mach64_cursor_enabled(s) ? 64 : 0;
    int x = s->regs[CUR_HORZ_VERT_POSN] & 0xffff;
    int y = (s->regs[CUR_HORZ_VERT_POSN] >> 16) & 0xffff;
    uint32_t off = (s->regs[CUR_OFFSET] & 0x000fffff) * 8;

    if (s->cursor_size != size || vga->hw_cursor_x != x ||
        vga->hw_cursor_y != y || s->cursor_offset != off) {
        vga_invalidate_scanlines(vga, vga->hw_cursor_y, vga->hw_cursor_y + 63);
        vga->hw_cursor_x = x;
        vga->hw_cursor_y = y;
        s->cursor_offset = off;
        s->cursor_size = size;
        if (size) {
            vga_invalidate_scanlines(vga, y, y + 63);
        }
    }
}

static void mach64_cursor_draw_line(VGACommonState *vga, uint8_t *d, int scr_y)
{
    Mach64VGAState *s = container_of(vga, Mach64VGAState, vga);
    uint32_t srcoff, color, h;
    uint32_t *dp = (uint32_t *)d;
    unsigned hoff = s->regs[CUR_HORZ_VERT_OFF] & 0x3f;
    unsigned voff = (s->regs[CUR_HORZ_VERT_OFF] >> 16) & 0x3f;
    int row = scr_y - vga->hw_cursor_y;

    if (!mach64_cursor_enabled(s) || row < 0 || row + (int)voff >= 64) {
        return;
    }
    srcoff = (s->regs[CUR_OFFSET] & 0x000fffff) * 8;
    if (srcoff > s->vga.vram_size - 64 * 16) {
        return;
    }
    dp = &dp[vga->hw_cursor_x];
    h = (((s->regs[CRTC_H_TOTAL_DISP] & CRTC_H_DISP) >> 16) + 1) * 8;
    for (int i = 0; i < 64 - (int)hoff; i++) {
        if (vga->hw_cursor_x + i >= h) {
            return;
        }
        switch (mach64_cursor_pixel(s, srcoff, row, i + hoff)) {
        case MACH64_CUR_CLR0:
            color = mach64_cursor_color(s, s->regs[CUR_CLR0]);
            break;
        case MACH64_CUR_CLR1:
            color = mach64_cursor_color(s, s->regs[CUR_CLR1]);
            break;
        case MACH64_CUR_TRANSPARENT:
            continue;
        default:
            color = dp[i] ^ 0x00ffffff;
            break;
        }
        dp[i] = color;
    }
}

/* ---- DAC palette access via the MMIO DAC_REGS byte window ---- */

static void mach64_dac_write(Mach64VGAState *s, unsigned byte, uint8_t val)
{
    VGACommonState *vga = &s->vga;

    switch (byte) {
    case 0: /* DAC_W_INDEX */
        vga->dac_write_index = val;
        vga->dac_sub_index = 0;
        break;
    case 1: /* DAC_DATA */
        vga->palette[vga->dac_write_index * 3 + vga->dac_sub_index] = val;
        if (++vga->dac_sub_index == 3) {
            vga->dac_sub_index = 0;
            vga->dac_write_index++;
        }
        break;
    case 2: /* DAC_MASK */
        break;
    case 3: /* DAC_R_INDEX */
        vga->dac_read_index = val;
        vga->dac_sub_index = 0;
        break;
    }
}

static uint8_t mach64_dac_read(Mach64VGAState *s, unsigned byte)
{
    VGACommonState *vga = &s->vga;
    uint8_t v = 0;

    if (byte == 1) {
        v = vga->palette[vga->dac_read_index * 3 + vga->dac_sub_index];
        if (++vga->dac_sub_index == 3) {
            vga->dac_sub_index = 0;
            vga->dac_read_index++;
        }
    }
    return v;
}

/* Synthetic current scanline, so drivers polling CRTC_VLINE make progress. */
static uint32_t mach64_crtc_vline(Mach64VGAState *s)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    unsigned total = s->regs[CRTC_V_TOTAL_DISP] & 0x7ff;
    /*
     * The CRTC free-runs: it generates raster timing (and therefore a toggling
     * vblank status) whenever the chip is powered, even before software programs
     * a custom mode.  When CRTC_V_TOTAL is still zero (unprogrammed, or reset
     * mid-init) fall back to a standard 525-line frame so the scanline counter
     * keeps cycling; otherwise the count would be pinned at 0 and the vblank
     * status could never toggle, hanging any driver that waits on it.
     */
    int lines = total ? (int)total + 1 : 525;
    int64_t frame_ns = NANOSECONDS_PER_SECOND / 60;

    return (now % frame_ns) * lines / frame_ns;
}

/* True while the synthetic raster is in the vertical-blank region. */
static bool mach64_in_vblank(Mach64VGAState *s)
{
    unsigned vd = (s->regs[CRTC_V_TOTAL_DISP] & CRTC_V_DISP) >> 16;
    /* Match the crtc_vline free-run default: 480 active lines when unprogrammed. */
    int vdisp = vd ? (int)vd + 1 : 480;

    return (int)mach64_crtc_vline(s) >= vdisp;
}

static void mach64_update_irq(Mach64VGAState *s)
{
    uint32_t ic = s->regs[CRTC_INT_CNTL];
    bool level = ((ic & CRTC_VBLANK_INT) && (ic & CRTC_VBLANK_INT_EN)) ||
                 ((ic & CRTC_VLINE_INT) && (ic & CRTC_VLINE_INT_EN));

    pci_set_irq(&s->dev, level);
}

static void mach64_vblank_timer(void *opaque)
{
    Mach64VGAState *s = opaque;

    timer_mod(&s->vblank_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              NANOSECONDS_PER_SECOND / 60);
    s->regs[CRTC_INT_CNTL] |= CRTC_VBLANK_INT;
    mach64_update_irq(s);
}

/*
 * CRT DDC bit-bang.  LCD register 7 (via LCD_INDEX/LCD_DATA) carries the two
 * open-drain I2C lines SCL=bit6, SDA=bit5: a written 0 drives the line low, a 1
 * releases it; a read samples the wired-AND line level.  Drive the shared
 * bitbang_i2c master, which clocks the i2c-ddc EDID slave, and latch the
 * resulting line levels for readback.
 */
/* DDC I2C bus decoder states (mach64_ddc_clock). */
enum { DDC_IDLE, DDC_ADDR, DDC_WRITE, DDC_READ };

/*
 * Decode the guest's SCL/SDA bit-bang into I2C transactions on s->ddc_bus.
 * Unlike hw/i2c/bitbang_i2c, SDA is modelled as a true open-drain wired-AND of
 * the master drive and the slave (s->ddc_slave_sda): a master that releases SDA
 * while the slave holds it low (reading an ACK/data bit with SCL high) is NOT
 * mistaken for a STOP.  The ATI miniport releases SDA to sample the ACK while
 * SCL is high, which bitbang_i2c would misread as a STOP.
 *
 * scl/sda_m are the master's line intents (1 = high/released, 0 = pulled low).
 */
static void mach64_ddc_clock(Mach64VGAState *s, int scl, int sda_m)
{
    I2CBus *bus = s->ddc_bus;
    int prev_scl = s->ddc_scl;
    int prev_sda = s->ddc_sda_m;

    if (scl && prev_scl) {
        /* SCL steady high: an SDA edge is a START or (open-drain) STOP. */
        if (prev_sda && !sda_m) {
            if (s->ddc_bus_state != DDC_IDLE) {
                i2c_end_transfer(bus);
            }
            s->ddc_bus_state = DDC_ADDR;
            s->ddc_cnt = 0;
            s->ddc_buf = 0;
            s->ddc_addr = -1;
            s->ddc_slave_sda = 1;
        } else if (!prev_sda && sda_m && s->ddc_slave_sda) {
            /* Line actually rose (slave not holding it low): STOP. */
            if (s->ddc_bus_state != DDC_IDLE) {
                i2c_end_transfer(bus);
            }
            s->ddc_bus_state = DDC_IDLE;
            s->ddc_addr = -1;
            s->ddc_slave_sda = 1;
        }
    } else if (scl && !prev_scl) {
        /* SCL rising: the receiver samples.  ddc_cnt: 0-7 data bits, 8 the ACK
         * bit, 9 = ACK clocked (waiting for the falling edge to advance). */
        switch (s->ddc_bus_state) {
        case DDC_ADDR:
        case DDC_WRITE:
            if (s->ddc_cnt < 8) {
                s->ddc_buf = (s->ddc_buf << 1) | (sda_m & 1);
                if (++s->ddc_cnt == 8) {
                    int nack;
                    if (s->ddc_bus_state == DDC_ADDR) {
                        s->ddc_addr = s->ddc_buf;
                        nack = i2c_start_transfer(bus, s->ddc_addr >> 1,
                                                  s->ddc_addr & 1);
                    } else {
                        nack = i2c_send(bus, s->ddc_buf);
                    }
                    s->ddc_slave_sda = nack ? 1 : 0;   /* drive the ACK low */
                }
            } else if (s->ddc_cnt == 8) {
                s->ddc_cnt = 9;                        /* master read the ACK */
            }
            break;
        case DDC_READ:
            if (s->ddc_cnt < 8) {
                s->ddc_cnt++;                          /* master read a bit */
            } else if (s->ddc_cnt == 8) {
                if (sda_m == 0) {                      /* master ACK: continue */
                    i2c_ack(bus);
                    s->ddc_read_nacked = 0;
                } else {                               /* master NACK: end */
                    i2c_nack(bus);
                    s->ddc_read_nacked = 1;
                }
                s->ddc_cnt = 9;
            }
            break;
        default:
            break;
        }
    } else if (!scl && prev_scl) {
        /* SCL falling: the transmitter sets up the next bit. */
        switch (s->ddc_bus_state) {
        case DDC_ADDR:
        case DDC_WRITE:
            if (s->ddc_cnt == 9) {                     /* ACK complete */
                s->ddc_slave_sda = 1;                  /* release the ACK */
                s->ddc_cnt = 0;
                s->ddc_buf = 0;
                if (s->ddc_addr >= 0 && (s->ddc_addr & 1)) {
                    s->ddc_bus_state = DDC_READ;
                    s->ddc_buf = i2c_recv(bus);
                    s->ddc_slave_sda = (s->ddc_buf >> 7) & 1;
                } else {
                    s->ddc_bus_state = DDC_WRITE;
                }
            }
            break;
        case DDC_READ:
            if (s->ddc_cnt < 8) {                      /* drive the next bit */
                s->ddc_slave_sda = (s->ddc_buf >> (7 - s->ddc_cnt)) & 1;
            } else if (s->ddc_cnt == 8) {
                s->ddc_slave_sda = 1;                  /* release for master ACK */
            } else {                                   /* cnt == 9: after ACK */
                s->ddc_cnt = 0;
                if (s->ddc_read_nacked) {
                    s->ddc_slave_sda = 1;              /* done; wait for STOP */
                } else {
                    s->ddc_buf = i2c_recv(bus);        /* fetch next byte */
                    s->ddc_slave_sda = (s->ddc_buf >> 7) & 1;
                }
            }
            break;
        default:
            break;
        }
    }

    s->ddc_scl = scl;
    s->ddc_sda_m = sda_m;
}

static void mach64_ddc_access(Mach64VGAState *s, unsigned byte, uint32_t data)
{
    bool sda_out, scl_out, sda_st, scl_st;
    int scl_m, sda_m, sda_line, sda_rd, scl_rd;

    /*
     * The miniport drives each line with a direction (byte 3) and a state
     * (byte 1); latch whichever byte it just wrote.  A line is pulled low only
     * when it is an output with state 0, otherwise it is released.
     */
    if (byte == MACH64_DDC_DIR_BYTE) {
        s->ddc_dir = data & (MACH64_DDC_SDA | MACH64_DDC_SCL);
    } else if (byte == MACH64_DDC_STATE_BYTE) {
        s->ddc_state = data & (MACH64_DDC_SDA | MACH64_DDC_SCL);
    } else {
        return;
    }

    sda_out = s->ddc_dir & MACH64_DDC_SDA;
    scl_out = s->ddc_dir & MACH64_DDC_SCL;
    sda_st  = s->ddc_state & MACH64_DDC_SDA;
    scl_st  = s->ddc_state & MACH64_DDC_SCL;

    scl_m = (scl_out && !scl_st) ? 0 : 1;
    sda_m = (sda_out && !sda_st) ? 0 : 1;
    mach64_ddc_clock(s, scl_m, sda_m);

    /* Byte-1 state reads back the driven value while an output, else the live
     * wired-AND bus level (so a released SDA senses the slave). */
    sda_line = sda_m & s->ddc_slave_sda;
    sda_rd = sda_out ? (sda_st ? 1 : 0) : sda_line;
    scl_rd = scl_out ? (scl_st ? 1 : 0) : scl_m;
    s->ddc_gpio = (sda_out ? MACH64_DDC_SDA_DIR : 0) |
                  (scl_out ? MACH64_DDC_SCL_DIR : 0) |
                  (sda_rd ? MACH64_DDC_SDA_ST : 0) |
                  (scl_rd ? MACH64_DDC_SCL_ST : 0);
}

/*
 * I2C_CNTL_0 (reg 0x0F) hardware-I2C engine status.  ati2mpad issues a CRT-DDC
 * transfer and then polls the low-byte status field for I2C_CNTL_DONE.  The
 * modelled engine has no latency, so every transfer is already complete: report
 * DONE with the rest of the status field (NACK/HALT/FULL) clear.
 */
static uint32_t mach64_i2c0_readback(uint32_t val)
{
    return (val & ~I2C_CNTL_STAT) | I2C_CNTL_DONE;
}

/* Diagnostic wrapper: log the VBE/scanout state each render (MACH64_TRACE=1). */
static bool mach64_gfx_update(void *opaque)
{
    VGACommonState *vga = opaque;
    bool r = mach64_vga_hw_ops->gfx_update(opaque);

    if (mach64_trace_on()) {
        M64_TRACE("gfx_update vbe_en=%04x %ux%u bpp=%u start=%#x "
                  "graphic_mode=%d ret=%d",
                  vga->vbe_regs[VBE_DISPI_INDEX_ENABLE],
                  vga->vbe_regs[VBE_DISPI_INDEX_XRES],
                  vga->vbe_regs[VBE_DISPI_INDEX_YRES],
                  vga->vbe_regs[VBE_DISPI_INDEX_BPP],
                  vga->vbe_start_addr, vga->graphic_mode, r);
    }
    return r;
}

/* ---- MMIO register access ---- */

/*
 * GUI_TRAJ_CNTL is the composite view of DST_CNTL, SRC_CNTL, PAT_CNTL and
 * HOST_CNTL (RAGE XL RRG sec 5.2.9).  The engine reads the four, so a write
 * here has to reach them; native drivers set the trajectory through this
 * register alone.
 */
static void mach64_gui_traj_split(Mach64VGAState *s, uint32_t v)
{
    s->regs[DST_CNTL] = (v & GUI_TRAJ_DST_CNTL_MASK) >>
                        GUI_TRAJ_DST_CNTL_SHIFT;
    s->regs[SRC_CNTL] = (v & GUI_TRAJ_SRC_CNTL_MASK) >>
                        GUI_TRAJ_SRC_CNTL_SHIFT;
    s->regs[PAT_CNTL] = (v & GUI_TRAJ_PAT_CNTL_MASK) >>
                        GUI_TRAJ_PAT_CNTL_SHIFT;
    s->regs[HOST_CNTL] = (v & GUI_TRAJ_HOST_CNTL_MASK) >>
                         GUI_TRAJ_HOST_CNTL_SHIFT;
}

static uint32_t mach64_gui_traj_compose(const Mach64VGAState *s)
{
    return ((s->regs[DST_CNTL] << GUI_TRAJ_DST_CNTL_SHIFT) &
            GUI_TRAJ_DST_CNTL_MASK) |
           ((s->regs[SRC_CNTL] << GUI_TRAJ_SRC_CNTL_SHIFT) &
            GUI_TRAJ_SRC_CNTL_MASK) |
           ((s->regs[PAT_CNTL] << GUI_TRAJ_PAT_CNTL_SHIFT) &
            GUI_TRAJ_PAT_CNTL_MASK) |
           ((s->regs[HOST_CNTL] << GUI_TRAJ_HOST_CNTL_SHIFT) &
            GUI_TRAJ_HOST_CNTL_MASK);
}

/*
 * Small dual paged apertures (RAGE PRO PRG sec 3.2.2.2): two 32 KiB windows at
 * 0xA0000 and 0xA8000 whose pages into the frame buffer are selected
 * independently for reads (MEM_VGA_RP_SEL) and writes (MEM_VGA_WP_SEL), the
 * low half of each register paging the first window and the high half the
 * second (RRG 0_2D/0_2E).  They reach the whole 8 MiB.
 *
 * The adapter BIOS sizes video memory through them: for each candidate size it
 * writes a page-derived pattern to 32 bytes of every 32 KiB page and reads it
 * back (vgabios-mach64.bin offsets 0x7B96 and 0x7CAA).  Without the paging
 * every page aliases onto the same window, every candidate above one page
 * fails, and the BIOS leaves MEM_CNTL's MEM_SIZE at 1 MiB.
 *
 * The PRG gates the apertures on accelerator or SVGA packed-pixel mode and
 * names CFG_MEM_VGA_AP_EN only as the gate for the memory-mapped registers.
 * We gate on CFG_MEM_VGA_AP_EN alone: guests that page these windows set it
 * first, and it is zero in every VGA and VBE path, so the banked VBE window at
 * 0xA0000 keeps the plain VGA behaviour.
 */
static hwaddr mach64_vga_aper_off(Mach64VGAState *s, hwaddr addr, bool write)
{
    uint32_t sel = s->regs[write ? MEM_VGA_WP_SEL : MEM_VGA_RP_SEL];
    uint32_t page;

    addr &= 0xffff;
    page = (addr & MEM_VGA_PAGE_SIZE) ? (sel >> MEM_VGA_PS1_SHIFT) & 0xffff
                                      : sel & MEM_VGA_PS0;
    /* Unpopulated address bits alias, which is what makes the sizing work. */
    return ((hwaddr)page * MEM_VGA_PAGE_SIZE + (addr & (MEM_VGA_PAGE_SIZE - 1)))
           % s->vga.vram_size;
}

static uint64_t mach64_vga_aper_read(void *opaque, hwaddr addr, unsigned size)
{
    Mach64VGAState *s = opaque;
    uint64_t val = 0;
    unsigned i;

    for (i = 0; i < size; i++) {
        val |= (uint64_t)s->vga.vram_ptr[mach64_vga_aper_off(s, addr + i,
                                                             false)] << (i * 8);
    }
    return val;
}

static void mach64_vga_aper_write(void *opaque, hwaddr addr, uint64_t data,
                                  unsigned size)
{
    Mach64VGAState *s = opaque;
    unsigned i;

    for (i = 0; i < size; i++) {
        hwaddr off = mach64_vga_aper_off(s, addr + i, true);

        s->vga.vram_ptr[off] = (data >> (i * 8)) & 0xff;
        memory_region_set_dirty(&s->vga.vram, off, 1);
    }
}

static const MemoryRegionOps mach64_vga_aper_ops = {
    .read = mach64_vga_aper_read,
    .write = mach64_vga_aper_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

/*
 * Second half of the linear aperture.  Enabling the big aperture also enables
 * an 8 MiB big-endian view of the same frame buffer "at the standard linear
 * aperture address plus 8MB", which is why the part needs a 16 MiB BAR and why
 * CFG_MEM_AP_SIZE reads "2x8MB" (RAGE PRO PRG sec 3.2.2.3, RRG 0_37).  Windows'
 * miniport sizes the frame buffer as half the aperture, so an 8 MiB BAR made it
 * refuse every configuration above 4 MiB.
 */
static uint64_t mach64_be_aper_read(void *opaque, hwaddr addr, unsigned size)
{
    Mach64VGAState *s = opaque;
    uint64_t val = 0;
    unsigned i;

    for (i = 0; i < size; i++) {
        val |= (uint64_t)s->vga.vram_ptr[(addr + i) % s->vga.vram_size]
               << (i * 8);
    }
    return val;
}

static void mach64_be_aper_write(void *opaque, hwaddr addr, uint64_t data,
                                 unsigned size)
{
    Mach64VGAState *s = opaque;
    unsigned i;

    for (i = 0; i < size; i++) {
        hwaddr off = (addr + i) % s->vga.vram_size;

        s->vga.vram_ptr[off] = (data >> (i * 8)) & 0xff;
        memory_region_set_dirty(&s->vga.vram, off, 1);
    }
}

static const MemoryRegionOps mach64_be_aper_ops = {
    .read = mach64_be_aper_read,
    .write = mach64_be_aper_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

static void mach64_vga_aper_update(Mach64VGAState *s)
{
    memory_region_set_enabled(&s->vga_aper,
                              (s->regs[CONFIG_CNTL] & CFG_MEM_VGA_AP_EN) != 0);
}

static uint64_t mach64_mm_read(void *opaque, hwaddr addr, unsigned size)
{
    Mach64VGAState *s = opaque;
    unsigned reg = MACH64_OFF_TO_REG(addr);
    unsigned byte = addr & 3;
    uint32_t val;

    if (reg >= MACH64_NREGS) {
        mach64_trace_access('r', reg, 0);
        return 0; /* Block 1 (overlay/scaler): not modelled yet */
    }

    switch (reg) {
    case CONFIG_CHIP_ID:
        /* type field = PCI device id, rev field [31:24] = PCI revision. */
        val = s->dev_id | ((uint32_t)s->chip_rev << 24);
        break;
    case GUI_STAT:
        /* Engine idle, and the whole command FIFO free (see GUI_FIFO_SHIFT). */
        val = (uint32_t)GUI_FIFO_PIO_DWORDS << GUI_FIFO_SHIFT;
        break;
    case FIFO_STAT:
        val = 0;           /* no entry filled: a wait for room passes */
        break;
    case GUI_TRAJ_CNTL:
        val = mach64_gui_traj_compose(s);
        break;
    case CRTC_VLINE_CRNT_VLINE:
        val = (mach64_crtc_vline(s) << CRTC_CRNT_VLINE_SHIFT) & CRTC_CRNT_VLINE;
        val |= s->regs[reg] & CRTC_VLINE;   /* keep the programmed compare value */
        break;
    case CRTC_INT_CNTL:
        val = s->regs[reg] & ~CRTC_VBLANK;
        if (mach64_in_vblank(s)) {
            val |= CRTC_VBLANK;             /* live vblank status bit */
        }
        break;
    case CLOCK_CNTL: {
        /*
         * Indirect PLL read window: byte 2 reflects the PLL register selected
         * by byte 1, not the last value latched into CLOCK_CNTL itself.  A
         * native driver setting PLL_ADDR and reading the register back must see
         * the addressed PLL register (ati2mpad's GetMCLK/ReadPllRegisterUchar
         * poll it), otherwise it spins forever on the stale latch.
         */
        unsigned pll_addr = (s->regs[reg] & CLOCK_CNTL_PLL_ADDR_MASK)
                            >> CLOCK_CNTL_PLL_ADDR_SHIFT;
        uint8_t pll_data = s->pll_regs[pll_addr];
        /*
         * PLL register 0x1C behaves as a self-clearing command/strobe: the
         * miniport writes a command byte and then polls the register until the
         * hardware clears it.  Modelled as an instantaneous operation, so it
         * always reads back idle (0); a plain data latch would spin forever
         * (ati2mpad's clock path writes 0x18 here and waits for it to change).
         */
        if (pll_addr == 0x1c) {
            pll_data = 0;
        }
        val = s->regs[reg] & ~CLOCK_CNTL_PLL_DATA_MASK;
        val |= (uint32_t)pll_data << CLOCK_CNTL_PLL_DATA_SHIFT;
        break;
    }
    case LCD_DATA:
        /* LCD register 7 is the DDC I2C GPIO; other indices are plain latches. */
        val = (s->lcd_index == MACH64_LCD_DDC_INDEX) ? s->ddc_gpio : s->regs[reg];
        break;
    case I2C_CNTL_0:
        /* CRT DDC bit-bang: patch bit4 with the live wired-AND SDA level. */
        val = mach64_i2c0_readback(s->regs[reg]);
        break;
    case DAC_REGS:
        if (size == 1) {
            return mach64_dac_read(s, byte);
        }
        val = s->regs[reg];
        break;
    default:
        val = s->regs[reg];
        break;
    }

    if (reg != DAC_REGS) {
        mach64_trace_access('r', reg, val);
    }

    if (size < 4) {
        val = (val >> (byte * 8)) & ((1u << (size * 8)) - 1);
    }
    return val;
}

/*
 * DP_SET_GUI_ENGINE (0_BF, write only): one store programs the data path.
 * RAGE XL Register Reference sec 5.2, Table 5-11 for the registers it resets
 * and Table 5-12 for the DRAWING_COMBO presets.  Server 2003's ati2drad writes
 * it before every batch of glyph rectangles, so ignoring it left the engine
 * running on whatever the previous operation had left behind.
 */
static void mach64_dp_set_gui_engine(Mach64VGAState *s, uint32_t v)
{
    /* RRG Table 5-12; row 0 and row 14 are undefined and leave the path be. */
    static const struct { uint32_t src, mix, traj; } combo[16] = {
        [1]  = { 0x0000100, 0x0070003, 0x00000023 },
        [2]  = { 0x0000200, 0x0070007, 0x00000003 },
        [3]  = { 0x0020100, 0x0070007, 0x00000003 },
        [4]  = { 0x0000100, 0x0070007, 0x00000023 },
        [5]  = { 0x0010100, 0x0070007, 0x01000003 },
        [6]  = { 0x0000100, 0x0070007, 0x00000003 },
        [7]  = { 0x0000300, 0x0070007, 0x00030003 },
        [8]  = { 0x0000300, 0x0070007, 0x00000000 },
        [9]  = { 0x0000300, 0x0070007, 0x00000001 },
        [10] = { 0x0000300, 0x0070007, 0x00000002 },
        [11] = { 0x0000300, 0x0070007, 0x00000003 },
        [12] = { 0x0020100, 0x0070003, 0x1004001b },
        [13] = { 0x0020100, 0x0070003, 0x0004001b },
        [15] = { 0x0000300, 0x0070007, 0x0004001b },
    };
    static const uint16_t pitch_tab[16] = {
        0, 320, 352, 384, 640, 800, 896, 512,
        1024, 1152, 1280, 400, 832, 1600, 448, 2048,
    };
    unsigned dstw = (v & DP_SGE_DST_PIX_WIDTH) >> DP_SGE_DST_PIX_WIDTH_SHIFT;
    unsigned cmb = (v & DP_SGE_DRAWING_COMBO) >> DP_SGE_DRAWING_COMBO_SHIFT;
    unsigned pidx = (v & DP_SGE_DST_PITCH) >> DP_SGE_DST_PITCH_SHIFT;
    unsigned pitch = pitch_tab[pidx];
    unsigned srcw = (v & DP_SGE_SRC_PIX_WIDTH) ? dstw : PIX_WIDTH_1BPP;

    /* Table 5-11 leaves DP_HOST_PIX_WIDTH and DP_BYTE_PIX_ORDER at 0. */
    s->regs[DP_PIX_WIDTH] = dstw | (srcw << DP_SRC_PIX_WIDTH_SHIFT);
    if (v & DP_SGE_DST_PITCH_BY_2) {
        pitch *= 2;
    }
    if (pitch != 0) {
        s->regs[DST_OFF_PITCH] = (s->regs[DST_OFF_PITCH] & 0x000fffff) |
                                 ((uint32_t)(pitch / 8) << CRTC_PITCH_SHIFT);
    }
    s->regs[SRC_OFF_PITCH] = (v & DP_SGE_SRC_OFFPITCH_COPY) ?
                             s->regs[DST_OFF_PITCH] : 0;
    if (combo[cmb].src != 0) {
        s->regs[DP_SRC] = combo[cmb].src;
        s->regs[DP_MIX] = combo[cmb].mix;
        mach64_gui_traj_split(s, combo[cmb].traj);
    }
    /* SET_SRC_HGTWID1_2: both general-pattern extents, 8x8, 32x1 or 24x8. */
    switch ((v & DP_SGE_SRC_HGTWID) >> DP_SGE_SRC_HGTWID_SHIFT) {
    case 0:
        s->regs[SRC_HEIGHT1_WIDTH1] = s->regs[SRC_HEIGHT2_WIDTH2] =
            (8u << 16) | 8;
        break;
    case 1:
        s->regs[SRC_HEIGHT1_WIDTH1] = s->regs[SRC_HEIGHT2_WIDTH2] =
            (32u << 16) | 1;
        break;
    case 2:
        s->regs[SRC_HEIGHT1_WIDTH1] = s->regs[SRC_HEIGHT2_WIDTH2] =
            (24u << 16) | 8;
        break;
    }
    /* Table 5-11: the registers the write leaves in a known state. */
    s->regs[SRC_Y_X_START] &= 0x0000ffff;           /* SRC_X_START = 0 */
    s->regs[DST_Y_X] = 0;
    s->regs[DST_HEIGHT_WIDTH] = 0;
    s->regs[SRC_Y_X] = 0;
    s->regs[SC_TOP_BOTTOM] = 0x3fff0000;
    s->regs[SC_LEFT_RIGHT] = 0x1fff0000;
    s->regs[DP_WRITE_MASK] = 0xffffffff;
    s->regs[CLR_CMP_CNTL] = 0;
}

static void mach64_reg_store(Mach64VGAState *s, unsigned reg, unsigned byte,
                             unsigned size, uint32_t data)
{
    if (size >= 4) {
        s->regs[reg] = data;
        return;
    }
    uint32_t mask = ((1u << (size * 8)) - 1) << (byte * 8);
    s->regs[reg] = (s->regs[reg] & ~mask) | ((data << (byte * 8)) & mask);
}

static void mach64_mm_write(void *opaque, hwaddr addr, uint64_t data,
                            unsigned size)
{
    Mach64VGAState *s = opaque;
    unsigned reg = MACH64_OFF_TO_REG(addr);
    unsigned byte = addr & 3;

    if (reg >= MACH64_NREGS) {
        mach64_trace_access('w', reg, data);
        return; /* Block 1 (overlay/scaler): not modelled yet */
    }

    /* Host-data stream: any of HOST_DATA0..F feeds the active blit. */
    if (reg >= HOST_DATA0 && reg <= HOST_DATAF) {
        s->regs[reg] = data;
        mach64_2d_host_data(s, data);
        return;
    }

    if (reg != DAC_REGS) {
        mach64_trace_access('w', reg, data);
    }

    switch (reg) {
    case DAC_REGS:
        if (size == 1) {
            static unsigned dac_n;
            if (mach64_trace_on() && byte == 1 && (dac_n++ % 256) == 0) {
                M64_TRACE("dac palette data burst @#%u (widx=%u)", dac_n,
                          s->vga.dac_write_index);
            }
            mach64_dac_write(s, byte, data & 0xff);
            return;
        }
        mach64_reg_store(s, reg, byte, size, data);
        return;
    case CONFIG_CHIP_ID:
    case CONFIG_STAT0:
    case GUI_STAT:
    case FIFO_STAT:
        return; /* read-only */
    case CONFIG_CNTL:
        mach64_reg_store(s, reg, byte, size, data);
        /* CFG_MEM_AP_SIZE is read-only on PCI and always 2x8MB (RRG 0_37). */
        s->regs[CONFIG_CNTL] = (s->regs[CONFIG_CNTL] & ~CFG_MEM_AP_SIZE) |
                               CFG_MEM_AP_SIZE_2X8M;
        mach64_vga_aper_update(s);
        return;
    case GUI_TRAJ_CNTL:
        mach64_reg_store(s, reg, byte, size, data);
        mach64_gui_traj_split(s, s->regs[reg]);
        return;
    case CRTC_INT_CNTL:
        /*
         * Plain R/W: the hardware sets the _INT status bits on the event (the
         * vblank timer ORs CRTC_VBLANK_INT in); the driver's ISR clears them by
         * reading the register and writing it back with the bit cleared (a
         * read-modify-write to 0), not write-1-to-ack.  Store the value as
         * written and re-evaluate the interrupt line.
         */
        s->regs[reg] = data;
        mach64_update_irq(s);
        return;
    case DP_SET_GUI_ENGINE:
        mach64_dp_set_gui_engine(s, data);
        return;
    case LCD_INDEX:
        s->lcd_index = data & 0xff;
        mach64_reg_store(s, reg, byte, size, data);
        return;
    case LCD_DATA:
        if (s->lcd_index == MACH64_LCD_DDC_INDEX) {
            mach64_ddc_access(s, byte, data);   /* drive the DDC I2C bus */
        }
        mach64_reg_store(s, reg, byte, size, data);
        return;
    }

    mach64_reg_store(s, reg, byte, size, data);

    switch (reg) {
    case CLOCK_CNTL:
        /*
         * Indirect PLL write: when PLL_WR_EN is set, byte 2 is committed to the
         * PLL register selected by byte 1.  (For a read the driver leaves
         * PLL_WR_EN clear and byte 2 is a don't-care window filled in on read.)
         */
        if (s->regs[reg] & CLOCK_CNTL_PLL_WR_EN) {
            unsigned pll_addr = (s->regs[reg] & CLOCK_CNTL_PLL_ADDR_MASK)
                                >> CLOCK_CNTL_PLL_ADDR_SHIFT;
            s->pll_regs[pll_addr] = (s->regs[reg] & CLOCK_CNTL_PLL_DATA_MASK)
                                    >> CLOCK_CNTL_PLL_DATA_SHIFT;
        }
        break;
    case CRTC_GEN_CNTL:
    case CRTC_H_TOTAL_DISP:
    case CRTC_V_TOTAL_DISP:
    case CRTC_OFF_PITCH:
    case DAC_CNTL:
        mach64_switch_mode(s);
        break;
    case GEN_TEST_CNTL: {
        bool en = mach64_cursor_enabled(s);

        if (s->cursor_guest_mode) {
            s->vga.force_shadow = en;
            graphic_hw_invalidate(s->vga.con);
        } else if (en) {
            mach64_cursor_define(s);
            dpy_mouse_set(s->vga.con, s->regs[CUR_HORZ_VERT_POSN] & 0xffff,
                          (s->regs[CUR_HORZ_VERT_POSN] >> 16) & 0xffff, 1);
        } else {
            dpy_mouse_set(s->vga.con, 0, 0, 0);
        }
        break;
    }
    case CUR_HORZ_VERT_POSN:
        if (!s->cursor_guest_mode) {
            dpy_mouse_set(s->vga.con, s->regs[reg] & 0xffff,
                          (s->regs[reg] >> 16) & 0xffff,
                          mach64_cursor_enabled(s));
        }
        break;
    case CUR_OFFSET:
    case CUR_HORZ_VERT_OFF:
    case CUR_CLR0:
    case CUR_CLR1:
        if (!s->cursor_guest_mode && mach64_cursor_enabled(s)) {
            mach64_cursor_define(s);
        }
        break;

    /*
     * Fold the separate and alternate-order destination / scissor registers
     * into the canonical DST_Y_X (X high, Y low), DST_HEIGHT_WIDTH (W high,
     * H low) and SC_LEFT/RIGHT/TOP/BOTTOM that the engine reads, so a guest
     * may program the engine through whichever aliases it prefers.
     */
    case DST_X:
        s->regs[DST_Y_X] = (s->regs[DST_Y_X] & 0x0000ffff) |
                           ((s->regs[reg] & 0x3fff) << 16);
        break;
    case DST_Y:
        s->regs[DST_Y_X] = (s->regs[DST_Y_X] & 0xffff0000) |
                           (s->regs[reg] & 0x7fff);
        break;
    case DST_X_Y:  /* X low, Y high */
        s->regs[DST_Y_X] = ((s->regs[reg] & 0x3fff) << 16) |
                           ((s->regs[reg] >> 16) & 0x7fff);
        break;
    case DST_Y_X_ALIAS:
        s->regs[DST_Y_X] = s->regs[reg];
        break;
    case SRC_X:
        s->regs[SRC_Y_X] = (s->regs[SRC_Y_X] & 0x0000ffff) |
                           ((s->regs[reg] & 0x3fff) << 16);
        break;
    case SRC_Y:
        s->regs[SRC_Y_X] = (s->regs[SRC_Y_X] & 0xffff0000) |
                           (s->regs[reg] & 0x7fff);
        break;
    case SRC_WIDTH1:
        s->regs[SRC_HEIGHT1_WIDTH1] = (s->regs[SRC_HEIGHT1_WIDTH1] &
                                       0x0000ffff) |
                                      ((s->regs[reg] & 0x3fff) << 16);
        break;
    case SRC_HEIGHT1:
        s->regs[SRC_HEIGHT1_WIDTH1] = (s->regs[SRC_HEIGHT1_WIDTH1] &
                                       0xffff0000) |
                                      (s->regs[reg] & 0x7fff);
        break;
    case SRC_X_START:
        s->regs[SRC_Y_X_START] = (s->regs[SRC_Y_X_START] & 0x0000ffff) |
                                 ((s->regs[reg] & 0x3fff) << 16);
        break;
    case SRC_Y_START:
        s->regs[SRC_Y_X_START] = (s->regs[SRC_Y_X_START] & 0xffff0000) |
                                 (s->regs[reg] & 0x7fff);
        break;
    case SRC_WIDTH2:
        s->regs[SRC_HEIGHT2_WIDTH2] = (s->regs[SRC_HEIGHT2_WIDTH2] &
                                       0x0000ffff) |
                                      ((s->regs[reg] & 0x3fff) << 16);
        break;
    case SRC_HEIGHT2:
        s->regs[SRC_HEIGHT2_WIDTH2] = (s->regs[SRC_HEIGHT2_WIDTH2] &
                                       0xffff0000) |
                                      (s->regs[reg] & 0x7fff);
        break;
    case SC_LEFT_RIGHT:  /* LEFT low, RIGHT high */
        s->regs[SC_LEFT] = s->regs[reg] & 0x3fff;
        s->regs[SC_RIGHT] = (s->regs[reg] >> 16) & 0x3fff;
        break;
    case SC_TOP_BOTTOM:  /* TOP low, BOTTOM high */
        s->regs[SC_TOP] = s->regs[reg] & 0x7fff;
        s->regs[SC_BOTTOM] = (s->regs[reg] >> 16) & 0x7fff;
        break;

    case DST_HEIGHT:
        s->regs[DST_HEIGHT_WIDTH] = (s->regs[DST_HEIGHT_WIDTH] & 0xffff0000) |
                                    (s->regs[reg] & 0x7fff);
        break;

    /*
     * Trajectory launches.  Of the separate registers it is DST_WIDTH that
     * "initiates a draw operation", unless DST_WIDTH_FILL_DIS is set in the
     * same write; DST_HEIGHT only holds the height (RAGE XL RRG MM 0_44,
     * 0_45).  A span fill writes DST_HEIGHT = 1 once and then DST_X and
     * DST_WIDTH per span, stepping down with DST_Y_TILE (WXPSP1
     * drivers/video/ms/ati/disp/fastfill.c:1037-1069).
     */
    case DST_WIDTH:
        s->regs[DST_HEIGHT_WIDTH] = (s->regs[DST_HEIGHT_WIDTH] & 0x0000ffff) |
                                    ((s->regs[reg] & 0x3fff) << 16);
        if (!(data & DST_WIDTH_FILL_DIS)) {
            mach64_2d_dst_trigger(s);
        }
        break;
    case DST_X_WIDTH:  /* X low, W high */
        s->regs[DST_Y_X] = (s->regs[DST_Y_X] & 0x0000ffff) |
                           ((s->regs[reg] & 0x3fff) << 16);
        s->regs[DST_HEIGHT_WIDTH] = (s->regs[DST_HEIGHT_WIDTH] & 0x0000ffff) |
                                    (s->regs[reg] & 0x3fff0000);
        mach64_2d_dst_trigger(s);
        break;
    case DST_WIDTH_HEIGHT:  /* W low, H high */
        s->regs[DST_HEIGHT_WIDTH] = ((s->regs[reg] & 0x3fff) << 16) |
                                    ((s->regs[reg] >> 16) & 0x7fff);
        mach64_2d_dst_trigger(s);
        break;
    case DST_HEIGHT_WIDTH:
        mach64_2d_dst_trigger(s);
        break;
    case DST_BRES_LNTH:
        mach64_2d_line_trigger(s);
        break;
    }
}

static const MemoryRegionOps mach64_mm_ops = {
    .read = mach64_mm_read,
    .write = mach64_mm_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

/* ---- vmstate ---- */

static int mach64_post_load(void *opaque, int version_id)
{
    Mach64VGAState *s = opaque;

    s->mode = (s->regs[CRTC_GEN_CNTL] & CRTC_EXT_DISP_EN) ? EXT_MODE : VGA_MODE;
    s->vga.graphic_mode = -1;
    if (s->cursor_guest_mode) {
        s->vga.force_shadow = mach64_cursor_enabled(s);
        s->cursor_size = UINT16_MAX;
    } else if (mach64_cursor_enabled(s)) {
        mach64_cursor_define(s);
    }
    mach64_update_irq(s);
    mach64_vga_aper_update(s);
    graphic_hw_invalidate(s->vga.con);
    return 0;
}

static const VMStateDescription vmstate_mach64_host_data = {
    .name = "mach64-vga/host-data",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_BOOL(active, Mach64HostData),
        VMSTATE_BOOL(mono, Mach64HostData),
        VMSTATE_UINT32(x, Mach64HostData),
        VMSTATE_UINT32(y, Mach64HostData),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_mach64_vga = {
    .name = "mach64-vga",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = mach64_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_PCI_DEVICE(dev, Mach64VGAState),
        VMSTATE_STRUCT(vga, Mach64VGAState, 0, vmstate_vga_common,
                       VGACommonState),
        VMSTATE_UINT32_ARRAY(regs, Mach64VGAState, MACH64_NREGS),
        VMSTATE_STRUCT(host_data, Mach64VGAState, 0, vmstate_mach64_host_data,
                       Mach64HostData),
        VMSTATE_TIMER(vblank_timer, Mach64VGAState),
        VMSTATE_UINT8(lcd_index, Mach64VGAState),
        VMSTATE_UINT8(ddc_dir, Mach64VGAState),
        VMSTATE_UINT8(ddc_state, Mach64VGAState),
        VMSTATE_UINT32(ddc_gpio, Mach64VGAState),
        VMSTATE_UINT8(ddc_scl, Mach64VGAState),
        VMSTATE_UINT8(ddc_sda_m, Mach64VGAState),
        VMSTATE_UINT8(ddc_slave_sda, Mach64VGAState),
        VMSTATE_UINT8(ddc_bus_state, Mach64VGAState),
        VMSTATE_UINT8(ddc_cnt, Mach64VGAState),
        VMSTATE_UINT8(ddc_buf, Mach64VGAState),
        VMSTATE_UINT8(ddc_read_nacked, Mach64VGAState),
        VMSTATE_INT16(ddc_addr, Mach64VGAState),
        VMSTATE_END_OF_LIST()
    },
};

/* ---- realize / reset ---- */

static void mach64_vga_realize(PCIDevice *dev, Error **errp)
{
    Mach64VGAState *s = MACH64_VGA(dev);
    VGACommonState *vga = &s->vga;

    if (s->dev_id != PCI_DEVICE_ID_ATI_MACH64_GR &&
        s->dev_id != PCI_DEVICE_ID_ATI_MACH64_GT) {
        error_setg(errp, "mach64: only device id 0x4752 (Rage XL) or 0x4754 "
                   "(3D Rage II) is supported");
        return;
    }
    pci_set_word(dev->config + PCI_DEVICE_ID, s->dev_id);

    /*
     * Resolve the PCI revision.  Windows' inbox INFs bind the miniport by
     * hardware id: Rage XL (0x4752) carries no REV qualifier so any value
     * matches on both XP 2002 and XP 2003; 3D Rage II (0x4754) matches only
     * with a REV in the display.inf set, of which 0x9A ("3D RAGE II+ PCI") is
     * one.  A revision of 0 (our previous default) matched nothing, which is
     * why the driver never installed.
     */
    if (s->revision > 0xff) {
        s->chip_rev = (s->dev_id == PCI_DEVICE_ID_ATI_MACH64_GT) ?
                      MACH64_REV_3DRAGE_II : MACH64_REV_RAGE_XL;
    } else {
        s->chip_rev = s->revision;
    }
    pci_set_byte(dev->config + PCI_REVISION_ID, s->chip_rev);

    if (!vga_common_init(vga, OBJECT(s), errp)) {
        return;
    }
    vga->vbe_legacy_mode_switch = true;
    vga_init(vga, OBJECT(s), pci_address_space(dev),
             pci_address_space_io(dev), true);
    mach64_vga_hw_ops = vga->hw_ops;
    mach64_hw_ops = *vga->hw_ops;
    mach64_hw_ops.gfx_update = mach64_gfx_update;
    vga->con = graphic_console_init(DEVICE(s), 0, &mach64_hw_ops, vga);
    if (s->cursor_guest_mode) {
        vga->cursor_invalidate = mach64_cursor_invalidate;
        vga->cursor_draw_line = mach64_cursor_draw_line;
    }

    /*
     * Paged VGA aperture, disabled until CFG_MEM_VGA_AP_EN turns it on.  It
     * needs a priority above 2: the guests that page it also put the VGA into
     * chain-4 packed-pixel mode, for which vga_update_memory_access() maps the
     * frame buffer straight onto 0xA0000 as a RAM alias at priority 2.
     */
    memory_region_init_io(&s->vga_aper, OBJECT(s), &mach64_vga_aper_ops, s,
                          "mach64.vga-paged-aper", 0x10000);
    memory_region_set_enabled(&s->vga_aper, false);
    memory_region_add_subregion_overlap(pci_address_space(dev), 0x000a0000,
                                        &s->vga_aper, 3);

    /* MMIO register block (BAR2). */
    memory_region_init_io(&s->mm, OBJECT(s), &mach64_mm_ops, s,
                          "mach64.mmregs", MACH64_MMIO_SIZE);
    /*
     * Block-I/O alias (BAR1): CPIO register access.  A block-I/O port at
     * offset N reaches Block-0 register index N/4, which lives at BAR2 + 0x400
     * + N in the MMIO window - so alias the I/O BAR onto the Block-0 region,
     * not the start of the window (which decodes as the unmodelled Block 1).
     */
    memory_region_init_alias(&s->io, OBJECT(s), "mach64.io", &s->mm,
                             MACH64_REG_BLOCK0_BASE, 0x100);

    /* Linear framebuffer aperture (BAR0): 8 MiB, then its big-endian view. */
    memory_region_init(&s->linear_aper, OBJECT(dev), "mach64.vram",
                       MACH64_LINEAR_APER_SIZE);
    memory_region_add_subregion(&s->linear_aper, 0, &vga->vram);
    memory_region_init_io(&s->be_aper, OBJECT(s), &mach64_be_aper_ops, s,
                          "mach64.vram-be", MACH64_LINEAR_APER_SIZE / 2);
    memory_region_add_subregion(&s->linear_aper, MACH64_LINEAR_APER_SIZE / 2,
                                &s->be_aper);

    pci_register_bar(dev, 0, PCI_BASE_ADDRESS_MEM_PREFETCH, &s->linear_aper);
    pci_register_bar(dev, 1, PCI_BASE_ADDRESS_SPACE_IO, &s->io);
    pci_register_bar(dev, 2, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->mm);

    dev->config[PCI_INTERRUPT_PIN] = 1;
    timer_init_ns(&s->vblank_timer, QEMU_CLOCK_VIRTUAL, mach64_vblank_timer, s);
    timer_mod(&s->vblank_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              NANOSECONDS_PER_SECOND / 60);

    /* DDC/EDID: an i2c-ddc slave at 0x50 driven by the LCD-reg-7 bit-bang. */
    s->ddc_bus = i2c_init_bus(DEVICE(s), "mach64.ddc");
    i2c_slave_set_address(I2C_SLAVE(&s->i2cddc), 0x50);
    qdev_realize(DEVICE(&s->i2cddc), BUS(s->ddc_bus), &error_abort);
    s->ddc_scl = 1;
    s->ddc_sda_m = 1;
    s->ddc_slave_sda = 1;
    s->ddc_addr = -1;
}

static void mach64_vga_reset(DeviceState *dev)
{
    Mach64VGAState *s = MACH64_VGA(dev);

    memset(s->regs, 0, sizeof(s->regs));
    memset(&s->host_data, 0, sizeof(s->host_data));
    s->mode = VGA_MODE;
    s->cursor_size = 0;
    s->cursor_offset = 0;
    s->lcd_index = 0;
    s->ddc_dir = 0;
    s->ddc_state = 0;
    s->ddc_gpio = 0;
    s->ddc_scl = 1;
    s->ddc_sda_m = 1;
    s->ddc_slave_sda = 1;
    s->ddc_bus_state = DDC_IDLE;
    s->ddc_cnt = 0;
    s->ddc_buf = 0;
    s->ddc_read_nacked = 0;
    s->ddc_addr = -1;
    /* Engine out of reset on 264xT parts. */
    s->regs[GEN_TEST_CNTL] = GEN_GUI_RESETB;
    s->regs[CONFIG_CNTL] = CFG_MEM_AP_SIZE_2X8M;   /* read-only, RRG 0_37 */
    mach64_update_irq(s);
    mach64_vga_aper_update(s);

    /*
     * Seed the internal PLL with divider values a real video-BIOS POST would
     * leave, so the miniport's clock reads return a sane, non-zero clock (the
     * checked Server 2003 ati2mpad GetMCLK reads PLL_REF_DIV[2], MCLK_FB_DIV[4]
     * and PLL_XCLK_CNTL[0x0b] and float-divides them, breaking on a zero ref
     * divider) and its PLL read-back polls terminate instead of spinning on a
     * stale CLOCK_CNTL latch.
     */
    memset(s->pll_regs, 0, sizeof(s->pll_regs));
    /* Power-on defaults from the mach64 264VT/3D RAGE Register Reference Guide
     * (RRG-G02700, internal-PLL register table). */
    s->pll_regs[0x01] = 0xd4;   /* PLL_MACRO_CNTL */
    s->pll_regs[0x02] = 0x36;   /* PLL_REF_DIV */
    s->pll_regs[0x03] = 0x4f;   /* PLL_GEN_CNTL */
    s->pll_regs[0x04] = 0x97;   /* MCLK_FB_DIV (40 MHz) */
    s->pll_regs[0x05] = 0x04;   /* PLL_VCLK_CNTL */
    s->pll_regs[0x06] = 0x6a;   /* VCLK_POST_DIV */
    s->pll_regs[0x07] = 0xbe;   /* VCLK0_FB_DIV */
    s->pll_regs[0x08] = 0xd6;   /* VCLK1_FB_DIV */
    s->pll_regs[0x09] = 0xee;   /* VCLK2_FB_DIV */
    s->pll_regs[0x0a] = 0x88;   /* VCLK3_FB_DIV */
    s->pll_regs[0x0b] = 0x00;   /* PLL_XCLK_CNTL */
    s->pll_regs[0x0c] = 0x41;   /* PLL_FCP_CNTL */

    /*
     * The IA-64 firmware runs no Mach64 video-BIOS POST, so populate the
     * configuration straps a real POST would leave in the chip: the miniport
     * reads CONFIG_STAT0 for the chip/VGA-enable bits and memory type, and
     * MEM_CNTL for the installed VRAM size.  Without these it fails with
     * "Unable to obtain configuration information for graphics card" and never
     * initialises the display.
     */
    s->regs[CONFIG_STAT0] = CFG_CHIP_EN | CFG_VGA_EN | CFG_VGA_EN_T |
                            MEM_264_SGRAM;
    if (s->dev_id == PCI_DEVICE_ID_ATI_MACH64_GT) {
        s->regs[MEM_CNTL] = CTL_MEM_SIZE_8M;    /* <264VTB reads bits[2:0] */
    } else {
        s->regs[MEM_CNTL] = CTL_MEM_SIZEB_8M;   /* 264VTB+ reads bits[3:0] */
    }
}

static void mach64_vga_exit(PCIDevice *dev)
{
    Mach64VGAState *s = MACH64_VGA(dev);

    timer_del(&s->vblank_timer);
    graphic_console_close(s->vga.con);
    if (s->cursor) {
        cursor_unref(s->cursor);
        s->cursor = NULL;
    }
}

static const Property mach64_vga_properties[] = {
    DEFINE_PROP_UINT32("vgamem_mb", Mach64VGAState, vga.vram_size_mb, 8),
    DEFINE_PROP_UINT16("x-device-id", Mach64VGAState, dev_id,
                       PCI_DEVICE_ID_ATI_MACH64_GR),
    DEFINE_PROP_UINT16("x-revision", Mach64VGAState, revision,
                       MACH64_REV_AUTO),
    DEFINE_PROP_BOOL("guest_hwcursor", Mach64VGAState, cursor_guest_mode,
                     false),
    DEFINE_PROP_UINT8("x-pixman", Mach64VGAState, use_pixman,
                      MACH64_DEFAULT_PIXMAN),
    DEFINE_EDID_PROPERTIES(Mach64VGAState, i2cddc.edid_info),
};

static void mach64_vga_instance_init(Object *obj)
{
    Mach64VGAState *s = MACH64_VGA(obj);

    object_initialize_child(obj, "ddc", &s->i2cddc, TYPE_I2CDDC);
}

/* Diagnostic: trace PCI config writes to the ROM BAR / command register. */
static void mach64_config_write(PCIDevice *dev, uint32_t addr, uint32_t val,
                                int len)
{
    if (mach64_trace_on() &&
        (ranges_overlap(addr, len, PCI_ROM_ADDRESS, 4) ||
         ranges_overlap(addr, len, PCI_COMMAND, 2))) {
        M64_TRACE("pci cfg wr [%02x/%d] <- %08x (rom_bar now %08x cmd %04x)",
                  addr, len, val,
                  pci_get_long(dev->config + PCI_ROM_ADDRESS),
                  pci_get_word(dev->config + PCI_COMMAND));
    }
    pci_default_write_config(dev, addr, val, len);
}

static void mach64_vga_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, mach64_vga_reset);
    device_class_set_props(dc, mach64_vga_properties);
    dc->vmsd = &vmstate_mach64_vga;
    dc->hotpluggable = false;
    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);

    k->class_id = PCI_CLASS_DISPLAY_VGA;
    k->vendor_id = PCI_VENDOR_ID_ATI;
    k->device_id = PCI_DEVICE_ID_ATI_MACH64_GR;
    k->romfile = "vgabios-mach64.bin";
    k->realize = mach64_vga_realize;
    k->exit = mach64_vga_exit;
    k->config_write = mach64_config_write;
}

static const TypeInfo mach64_vga_info = {
    .name = TYPE_MACH64_VGA,
    .parent = TYPE_PCI_DEVICE,
    .instance_size = sizeof(Mach64VGAState),
    .instance_init = mach64_vga_instance_init,
    .class_init = mach64_vga_class_init,
    .interfaces = (const InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static void mach64_vga_register_types(void)
{
    type_register_static(&mach64_vga_info);
}

type_init(mach64_vga_register_types)
