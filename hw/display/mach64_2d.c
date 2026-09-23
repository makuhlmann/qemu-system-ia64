/*
 * QEMU ATI Mach64 "3D Rage" (DEV_4754) emulation - 2D GUI engine
 *
 * The Mach64 GUI engine is a synchronous MMIO blitter: the guest programs the
 * data-path (DP_*), source (SRC_*), pattern (PAT_*), scissor (SC_*) and colour-
 * compare (CLR_CMP_*) context, then a write to a trajectory register
 * (DST_HEIGHT_WIDTH for rectangles, DST_BRES_LNTH for lines) launches the
 * operation.  There is no command FIFO or DMA to model - each launch executes
 * immediately against the linear framebuffer.
 *
 * This work is licensed under the GNU GPL license version 2 or later.
 */

#include "qemu/osdep.h"
#include "mach64_int.h"
#include "mach64_regs.h"
#include "qemu/log.h"
#include "ui/console.h"

typedef struct {
    Mach64VGAState *s;
    uint8_t *vram;
    uint32_t vram_size;
    int bypp;
    int dst_pitch;          /* bytes */
    uint32_t dst_base;      /* bytes */
    uint32_t src_pitch;     /* bytes */
    uint32_t src_base;      /* bytes */
    unsigned frgd_mix;
    unsigned bkgd_mix;
    uint32_t frgd_clr;
    uint32_t bkgd_clr;
    uint32_t write_mask;
    uint32_t px_mask;       /* the bits one pixel occupies */
    /* packed 24 bpp: colour byte of the first destination byte, and where */
    bool rot24;
    int rot_x0;
    int rot_p0;
    /* scissor (inclusive) */
    int sc_left, sc_right, sc_top, sc_bottom;
    /* colour-compare */
    unsigned cmp_fn;
    bool cmp_on_src;
    uint32_t cmp_clr;
    uint32_t cmp_msk;
} Mach64Ctx;

static uint32_t px_read(const Mach64Ctx *c, uint32_t off)
{
    const uint8_t *p = c->vram + off;

    switch (c->bypp) {
    case 1:
        return p[0];
    case 2:
        return lduw_le_p(p);
    case 3:
        return p[0] | (p[1] << 8) | (p[2] << 16);
    default:
        return ldl_le_p(p);
    }
}

static void px_write(const Mach64Ctx *c, uint32_t off, uint32_t v)
{
    uint8_t *p = c->vram + off;

    switch (c->bypp) {
    case 1:
        p[0] = v;
        break;
    case 2:
        stw_le_p(p, v);
        break;
    case 3:
        p[0] = v;
        p[1] = v >> 8;
        p[2] = v >> 16;
        break;
    default:
        stl_le_p(p, v);
        break;
    }
}

static uint32_t apply_mix(unsigned mix, uint32_t src, uint32_t dst)
{
    switch (mix & 0xf) {
    case MIX_NOT_DST:          return ~dst;
    case MIX_0:                return 0;
    case MIX_1:                return ~0u;
    case MIX_DST:              return dst;
    case MIX_NOT_SRC:          return ~src;
    case MIX_XOR:              return src ^ dst;
    case MIX_XNOR:             return ~(src ^ dst);
    case MIX_SRC:              return src;
    case MIX_NAND:             return ~(src & dst);
    case MIX_NOT_SRC_OR_DST:   return ~src | dst;
    case MIX_SRC_OR_NOT_DST:   return src | ~dst;
    case MIX_OR:               return src | dst;
    case MIX_AND:              return src & dst;
    case MIX_SRC_AND_NOT_DST:  return src & ~dst;
    case MIX_NOT_SRC_AND_DST:  return ~src & dst;
    case MIX_NOR:              return ~(src | dst);
    default:                   return src;   /* arithmetic mixes: treat as SRC */
    }
}

/*
 * Colour compare (RAGE XL RRG CLR_CMP_CNTL MM 0_C2).  The comparison runs on
 * the destination or on the 2D source as CLR_CMP_SRC selects, and "if the
 * result of the comparison is false, the color source data is written to the
 * destination; otherwise destination data is written" -- a true result keeps
 * the pixel.  A source-keyed transparent blit is thus 2D source with the
 * EQUAL function, as WXPSP1 drivers/video/ms/ati/disp/ddraw64.c:447 programs
 * it, and a destination-keyed one is destination with NOT EQUAL (:454).
 */
static bool cmp_keeps_dst(const Mach64Ctx *c, uint32_t src, uint32_t dst)
{
    uint32_t v = c->cmp_on_src ? src : dst;
    uint32_t m = c->cmp_msk & c->px_mask;

    switch (c->cmp_fn) {
    case CLR_CMP_FN_TRUE:
        return true;
    case CLR_CMP_FN_NOT_EQUAL:
        return (v & m) != (c->cmp_clr & m);
    case CLR_CMP_FN_EQUAL:
        return (v & m) == (c->cmp_clr & m);
    default:
        return false;   /* FALSE and the reserved codes: always write */
    }
}

/*
 * Packed 24 bpp is an 8 bpp draw with DST_24_ROT_EN set: the engine hands each
 * destination byte one component of DP_FRGD_CLR, DP_BKGD_CLR and
 * DP_WRITE_MASK, and treats three bytes as one pixel of the fixed 8x8 mono
 * pattern (RAGE PRO PRG sec 6.4.1).  DST_24_ROT is the starting byte's dword
 * number mod 6, which pins the component of that byte: bytes run
 * component 0, 1, 2 from each pixel's first byte, so byte b holds component
 * b mod 3 and b mod 24 = 4 * DST_24_ROT + (b & 3).
 */
static int rot_phase(const Mach64Ctx *c, int x)
{
    return ((c->rot_p0 + (x - c->rot_x0)) % 3 + 3) % 3;
}

static uint32_t rot_clr(const Mach64Ctx *c, uint32_t clr, int x)
{
    return c->rot24 ? (clr >> (8 * rot_phase(c, x))) & 0xff : clr;
}

/* The pixel a destination byte belongs to, for the pattern. */
static int rot_pixel(const Mach64Ctx *c, int x)
{
    int first = x - rot_phase(c, x);

    return c->rot24 ? (first >= 0 ? first / 3 : (first - 2) / 3) : x;
}

static void blend_px(const Mach64Ctx *c, uint32_t off, int x, unsigned mix,
                     uint32_t src)
{
    uint32_t dst = px_read(c, off);
    uint32_t wm = rot_clr(c, c->write_mask, x);
    uint32_t res;

    if (cmp_keeps_dst(c, src, dst)) {
        return;
    }
    res = apply_mix(mix, src, dst);

    res = (res & wm) | (dst & ~wm);
    px_write(c, off, res);
}

/*
 * Coordinates are two's complement: DST_X, SRC_X and the left/right scissors
 * are signed 14-bit numbers, DST_Y, SRC_Y and the top/bottom scissors signed
 * 15-bit ones (RAGE XL RRG MM 0_41, 0_42, 0_61, 0_62, 0_A8-0_AC).  Windows
 * draws a window dragged past the left or top edge of the screen at negative
 * coordinates and lets the scissors clip it.
 */
static int yx_x(uint32_t yx)
{
    return sextract32(yx, 16, 14);
}

static int yx_y(uint32_t yx)
{
    return sextract32(yx, 0, 15);
}

static bool in_scissor(const Mach64Ctx *c, int x, int y)
{
    return x >= c->sc_left && x <= c->sc_right &&
           y >= c->sc_top && y <= c->sc_bottom;
}

static bool ctx_init(Mach64VGAState *s, Mach64Ctx *c)
{
    memset(c, 0, sizeof(*c));
    c->s = s;
    c->vram = s->vga.vram_ptr;
    c->vram_size = s->vga.vram_size;
    c->bypp = mach64_dst_bpp(s) / 8;
    if (c->bypp == 0) {
        return false;
    }
    c->px_mask = c->bypp >= 4 ? ~0u : (1u << (c->bypp * 8)) - 1;
    c->rot24 = c->bypp == 1 && (s->regs[DST_CNTL] & DST_24_ROT_EN);
    if (c->rot24) {
        int rot = (s->regs[DST_CNTL] & DST_24_ROT) >> DST_24_ROT_SHIFT;

        c->rot_x0 = yx_x(s->regs[DST_Y_X]);
        c->rot_p0 = (4 * rot + (c->rot_x0 & 3)) % 3;
    }
    c->dst_pitch = mach64_dst_pitch_bytes(s);
    c->dst_base = mach64_dst_base(s);

    int src_pitch_px = ((s->regs[SRC_OFF_PITCH] >> 22) & 0x3ff) * 8;
    c->src_pitch = src_pitch_px * c->bypp;
    c->src_base = (s->regs[SRC_OFF_PITCH] & 0x000fffff) * 8;
    if (c->src_pitch == 0) {
        c->src_pitch = c->dst_pitch;
        c->src_base = c->dst_base;
    }

    c->frgd_mix = (s->regs[DP_MIX] & DP_FRGD_MIX) >> DP_FRGD_MIX_SHIFT;
    c->bkgd_mix = s->regs[DP_MIX] & DP_BKGD_MIX;
    c->frgd_clr = s->regs[DP_FRGD_CLR];
    c->bkgd_clr = s->regs[DP_BKGD_CLR];
    c->write_mask = s->regs[DP_WRITE_MASK] ? s->regs[DP_WRITE_MASK] : ~0u;

    c->sc_left = sextract32(s->regs[SC_LEFT], 0, 14);
    c->sc_right = sextract32(s->regs[SC_RIGHT], 0, 14);
    c->sc_top = sextract32(s->regs[SC_TOP], 0, 15);
    c->sc_bottom = sextract32(s->regs[SC_BOTTOM], 0, 15);
    if (c->sc_right < c->sc_left) {
        c->sc_right = 0x3fff;
    }
    if (c->sc_bottom < c->sc_top) {
        c->sc_bottom = 0x3fff;
    }

    c->cmp_fn = s->regs[CLR_CMP_CNTL] & CLR_CMP_FN;
    c->cmp_on_src = ((s->regs[CLR_CMP_CNTL] & CLR_CMP_SRC) >> 24) !=
                    CLR_CMP_SRC_DST;
    c->cmp_clr = s->regs[CLR_CMP_CLR];
    c->cmp_msk = s->regs[CLR_CMP_MSK] ? s->regs[CLR_CMP_MSK] : ~0u;
    return true;
}

static uint32_t dst_off(const Mach64Ctx *c, int x, int y)
{
    return c->dst_base + (uint32_t)y * c->dst_pitch + (uint32_t)x * c->bypp;
}

static bool off_ok(const Mach64Ctx *c, uint32_t off)
{
    return off + c->bypp <= c->vram_size;
}

/* ---- pattern (8x8) ---- */

static bool pat_mono_bit(const Mach64VGAState *s, int x, int y)
{
    uint64_t pat = (uint64_t)s->regs[PAT_REG0] |
                   ((uint64_t)s->regs[PAT_REG1] << 32);
    int bit = (y & 7) * 8 + (x & 7);

    return (pat >> bit) & 1;
}

/* ---- solid / pattern rectangle fill ---- */

static void fill_rect(Mach64Ctx *c, int x0, int y0, int w, int h,
                      bool pattern)
{
    Mach64VGAState *s = c->s;

    for (int j = 0; j < h; j++) {
        int y = y0 + j;
        for (int i = 0; i < w; i++) {
            int x = x0 + i;
            uint32_t off;
            unsigned mix;
            uint32_t src;

            if (!in_scissor(c, x, y)) {
                continue;
            }
            off = dst_off(c, x, y);
            if (!off_ok(c, off)) {
                continue;
            }
            if (pattern) {
                bool bit = pat_mono_bit(s, rot_pixel(c, x), y);
                mix = bit ? c->frgd_mix : c->bkgd_mix;
                src = bit ? c->frgd_clr : c->bkgd_clr;
            } else {
                mix = c->frgd_mix;
                src = c->frgd_clr;
            }
            blend_px(c, off, x, mix, rot_clr(c, src, x));
        }
    }
}

/* ---- screen-to-screen copy ---- */

static void copy_rect(Mach64Ctx *c, int dx0, int dy0, int sx0, int sy0,
                      int w, int h, bool x_l2r, bool y_t2b)
{
    int xs = x_l2r ? 1 : -1;
    int ys = y_t2b ? 1 : -1;

    for (int j = 0; j < h; j++) {
        int dy = dy0 + j * ys, sy = sy0 + j * ys;
        for (int i = 0; i < w; i++) {
            int dx = dx0 + i * xs, sx = sx0 + i * xs;
            uint32_t soff, doff, src;

            if (!in_scissor(c, dx, dy)) {
                continue;
            }
            soff = c->src_base + (uint32_t)sy * c->src_pitch +
                   (uint32_t)sx * c->bypp;
            doff = dst_off(c, dx, dy);
            if (!off_ok(c, soff) || !off_ok(c, doff)) {
                continue;
            }
            src = px_read(c, soff);
            blend_px(c, doff, dx, c->frgd_mix, src);
        }
    }
}

/*
 * DST_X_TILE and DST_Y_TILE "determine the side effect of the DST_X and DST_Y
 * registers after the draw operation is completed": DST_X becomes
 * DST_X+DST_WIDTH for a left-to-right draw (DST_X-DST_WIDTH for right to
 * left), DST_Y likewise with DST_HEIGHT (RAGE XL RRG DST_CNTL MM 0_4C).  A
 * driver that tiles a strip issues the next rectangle without writing DST_Y_X.
 */
static void tile_advance(Mach64VGAState *s)
{
    uint32_t cntl = s->regs[DST_CNTL];
    uint32_t yx = s->regs[DST_Y_X];
    int x = yx_x(yx), y = yx_y(yx);
    int w = (s->regs[DST_HEIGHT_WIDTH] >> 16) & 0x3fff;
    int h = s->regs[DST_HEIGHT_WIDTH] & 0x7fff;

    if (cntl & DST_X_TILE) {
        x += (cntl & DST_X_DIR) ? w : -w;
    }
    if (cntl & DST_Y_TILE) {
        y += (cntl & DST_Y_DIR) ? h : -h;
    }
    s->regs[DST_Y_X] = ((uint32_t)(x & 0x3fff) << 16) | (y & 0x7fff);
    s->regs[DST_X] = x & 0x3fff;
    s->regs[DST_Y] = y & 0x7fff;
}

/* ---- launch a rectangle trajectory ---- */

void mach64_2d_dst_trigger(Mach64VGAState *s)
{
    Mach64Ctx c;
    int x = yx_x(s->regs[DST_Y_X]);
    int y = yx_y(s->regs[DST_Y_X]);
    int w = (s->regs[DST_HEIGHT_WIDTH] >> 16) & 0x3fff;
    int h = s->regs[DST_HEIGHT_WIDTH] & 0x7fff;
    unsigned frgd_src = (s->regs[DP_SRC] >> DP_FRGD_SRC_SHIFT) & 7;
    unsigned mono_src = (s->regs[DP_SRC] & DP_MONO_SRC) >> DP_MONO_SRC_SHIFT;
    /*
     * DST_X_DIR and DST_Y_DIR "determine the trajectory quadrant that the
     * destination area and the source area will take" (RAGE XL RRG,
     * DST_CNTL): DST_Y_X is where the draw starts, and the rectangle extends
     * from it the way the trajectory runs -- leftwards and upwards when the
     * bits are clear, which is how the tiling side effect subtracts the width
     * there.  It is not a walk order inside a fixed rectangle.
     */
    bool x_l2r = s->regs[DST_CNTL] & DST_X_DIR;
    bool y_t2b = s->regs[DST_CNTL] & DST_Y_DIR;

    s->host_data.active = false;
    if (w <= 0 || h <= 0) {
        return;
    }
    if (!ctx_init(s, &c)) {
        return;
    }

    /* CPU-to-screen blit: defer drawing until HOST_DATA* arrives. */
    if (frgd_src == SRC_HOST || mono_src == DP_MONO_SRC_HOST) {
        s->host_data.active = true;
        s->host_data.mono = (mono_src == DP_MONO_SRC_HOST);
        s->host_data.x = 0;
        s->host_data.y = 0;
        return;
    }

    if (frgd_src == SRC_BLIT) {
        int sx = yx_x(s->regs[SRC_Y_X]);
        int sy = yx_y(s->regs[SRC_Y_X]);

        copy_rect(&c, x, y, sx, sy, w, h, x_l2r, y_t2b);
    } else if (frgd_src == SRC_PATTERN ||
               (s->regs[SRC_CNTL] & SRC_PATT_EN)) {
        fill_rect(&c, x_l2r ? x : x - w + 1, y_t2b ? y : y - h + 1, w, h, true);
    } else {
        fill_rect(&c, x_l2r ? x : x - w + 1, y_t2b ? y : y - h + 1, w, h,
                  false);
    }
    mach64_2d_set_dirty(s, c.dst_base, x_l2r ? x : x - w + 1,
                        y_t2b ? y : y - h + 1, w, h);
    tile_advance(s);
}

/* ---- host-data (CPU-to-screen) stream ---- */

/*
 * Monochrome host pixels are consumed a byte at a time -- byte 0 first for a
 * left-to-right trajectory, byte 3 first for a right-to-left one -- and from
 * the most significant bit down inside each byte, unless DP_BYTE_PIX_ORDER
 * reverses that (RAGE PRO PRG sec 6.2.2.3, RAGE XL RRG DP_PIX_WIDTH MM 0_B4).
 */
static bool host_mono_bit(uint32_t data, unsigned n, bool lsb_first, bool l2r)
{
    unsigned byte = l2r ? n / 8 : 3 - n / 8;
    unsigned bit = n % 8;

    if (l2r != lsb_first) {
        bit = 7 - bit;
    }
    return (data >> (byte * 8 + bit)) & 1;
}

void mach64_2d_host_data(Mach64VGAState *s, uint32_t data)
{
    Mach64Ctx c;
    int x0 = yx_x(s->regs[DST_Y_X]);
    int y0 = yx_y(s->regs[DST_Y_X]);
    int w = (s->regs[DST_HEIGHT_WIDTH] >> 16) & 0x3fff;
    int h = s->regs[DST_HEIGHT_WIDTH] & 0x7fff;

    if (!s->host_data.active || w <= 0 || h <= 0) {
        return;
    }
    if (!ctx_init(s, &c)) {
        return;
    }

    if (s->host_data.mono) {
        bool x_l2r = s->regs[DST_CNTL] & DST_X_DIR;
        bool y_t2b = s->regs[DST_CNTL] & DST_Y_DIR;
        bool lsb_first = s->regs[DP_PIX_WIDTH] & DP_BYTE_PIX_ORDER;
        /*
         * HOST_BYTE_ALIGN: "when the destination trajectory advances in the Y
         * direction, pixels are consumed from the host data port until the
         * nearest byte boundary is reached" (RAGE XL RRG HOST_CNTL MM 0_90).
         * Windows sends a glyph wider than a byte this way, each row padded.
         */
        bool byte_align = s->regs[HOST_CNTL] & HOST_BYTE_ALIGN;
        /*
         * DP_HOST_TRIPLE_EN "enable[s] triplication of monochrome host data"
         * (RAGE XL RRG DP_PIX_WIDTH MM 0_B4): each bit covers the three bytes
         * of a packed 24 bpp pixel, which is how the ATI driver sends text.
         */
        int reps = (s->regs[DP_PIX_WIDTH] & DP_HOST_TRIPLE_EN) ? 3 : 1;

        for (unsigned n = 0; n < 32; n++) {
            bool bit = host_mono_bit(data, n, lsb_first, x_l2r);
            bool row_done = false;

            for (int r = 0; r < reps && !row_done; r++) {
                if (s->host_data.y >= (unsigned)h) {
                    break;
                }
                int x = x0 + (int)s->host_data.x * (x_l2r ? 1 : -1);
                int y = y0 + (int)s->host_data.y * (y_t2b ? 1 : -1);
                uint32_t off = dst_off(&c, x, y);
                unsigned mix = bit ? c.frgd_mix : c.bkgd_mix;
                uint32_t src = bit ? c.frgd_clr : c.bkgd_clr;

                if (in_scissor(&c, x, y) && off_ok(&c, off)) {
                    blend_px(&c, off, x, mix, rot_clr(&c, src, x));
                }
                if (++s->host_data.x >= (unsigned)w) {
                    s->host_data.x = 0;
                    s->host_data.y++;
                    if (byte_align) {
                        n |= 7;
                        row_done = true;
                    }
                }
            }
            if (s->host_data.y >= (unsigned)h) {
                break;
            }
        }
    } else {
        /* colour pixels packed in the dword, low pixel first. */
        int ppd = (c.bypp >= 4) ? 1 : (4 / c.bypp);
        for (int k = 0; k < ppd; k++) {
            if (s->host_data.y >= (unsigned)h) {
                break;
            }
            uint32_t src;
            switch (c.bypp) {
            case 1:
                src = (data >> (k * 8)) & 0xff;
                break;
            case 2:
                src = (data >> (k * 16)) & 0xffff;
                break;
            default:
                src = data;
                break;
            }
            int x = x0 + s->host_data.x;
            int y = y0 + s->host_data.y;
            uint32_t off = dst_off(&c, x, y);

            if (in_scissor(&c, x, y) && off_ok(&c, off)) {
                blend_px(&c, off, x, c.frgd_mix, src);
            }
            if (++s->host_data.x >= (unsigned)w) {
                s->host_data.x = 0;
                s->host_data.y++;
            }
        }
    }

    if (s->host_data.y >= (unsigned)h) {
        s->host_data.active = false;
        mach64_2d_set_dirty(s, c.dst_base, x0, y0, w, h);
        tile_advance(s);
    }
}

/* ---- Bresenham line ---- */

void mach64_2d_line_trigger(Mach64VGAState *s)
{
    Mach64Ctx c;
    int x = yx_x(s->regs[DST_Y_X]);
    int y = yx_y(s->regs[DST_Y_X]);
    int len = s->regs[DST_BRES_LNTH] & 0x7fff;
    int err = (int32_t)s->regs[DST_BRES_ERR];
    int inc = (int32_t)s->regs[DST_BRES_INC];
    int dec = (int32_t)s->regs[DST_BRES_DEC];
    uint32_t cntl = s->regs[DST_CNTL];
    bool x_dir = cntl & DST_X_DIR;
    bool y_dir = cntl & DST_Y_DIR;
    bool y_major = cntl & 0x00000004ul;    /* DST_Y_MAJOR */
    int minx = x, miny = y, maxx = x, maxy = y;

    if (len <= 0 || !ctx_init(s, &c)) {
        return;
    }

    for (int i = 0; i < len; i++) {
        uint32_t off = dst_off(&c, x, y);

        if (in_scissor(&c, x, y) && off_ok(&c, off)) {
            blend_px(&c, off, x, c.frgd_mix, c.frgd_clr);
        }
        minx = MIN(minx, x); miny = MIN(miny, y);
        maxx = MAX(maxx, x); maxy = MAX(maxy, y);

        /* Advance along the major axis; step the minor axis on error wrap. */
        if (y_major) {
            y += y_dir ? 1 : -1;
        } else {
            x += x_dir ? 1 : -1;
        }
        if (err >= 0) {
            err += dec;   /* DEC is programmed negative (2*dmajor - 2*dminor) */
            if (y_major) {
                x += x_dir ? 1 : -1;
            } else {
                y += y_dir ? 1 : -1;
            }
        } else {
            err += inc;
        }
    }

    mach64_2d_set_dirty(s, c.dst_base,
                        minx, miny, maxx - minx + 1, maxy - miny + 1);
}
