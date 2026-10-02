/*
 * QEMU ATI Mach64 overlay and back-end scaler (register block 1)
 *
 * The scaler reads a video buffer from the frame buffer, scales it to the
 * overlay window and the keyer mixes it into the graphics stream on its way
 * to the DAC (RAGE PRO PRG sec 8.4, 8.8; 264VT/3D RAGE RRG chapter 5).  The
 * mix happens per scanline in the VGA core's draw hook.
 *
 * This work is licensed under the GNU GPL license version 2 or later.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "mach64_int.h"

#define OVL(s, reg)     ((s)->ovl[(reg) - MACH64_BLOCK1])

/* Block-1 registers with storage; the rest of the block reads 0. */
static bool mach64_ovl_reg(unsigned reg)
{
    switch (reg) {
    case OVERLAY_Y_X_START ... OVERLAY_KEY_CNTL:
    case OVERLAY_SCALE_INC ... SCALER_BUF_PITCH:
    case VIDEO_FORMAT:
    case SCALER_COLOUR_CNTL ... SCALER_H_COEFF4:
    case SCALER_BUF0_OFFSET_U ... SCALER_BUF1_OFFSET_V:
        return true;
    default:
        return false;
    }
}

bool mach64_ovl_active(const Mach64VGAState *s)
{
    return s->mode == EXT_MODE &&
           (OVL(s, OVERLAY_SCALE_CNTL) & (OVERLAY_EN | SCALE_EN)) ==
           (OVERLAY_EN | SCALE_EN);
}

/*
 * The overlay and scaler registers are double-buffered: the hardware takes
 * them at the next vertical sync unless HW_DEBUG.BYPASS_SUBPIC_DBF is set
 * (RAGE XL RRG p. 4-30).  OVERLAY_LOCK in either window register holds the
 * window pair until a write with the bit clear (264VT/3D RAGE RRG p. 5-6).
 */
static void mach64_ovl_latch(Mach64VGAState *s)
{
    uint32_t start = OVL(s, OVERLAY_Y_X_START);
    uint32_t end = OVL(s, OVERLAY_Y_X_END);

    memcpy(s->ovl, s->regs1, sizeof(s->ovl));
    if (s->ovl_locked) {
        OVL(s, OVERLAY_Y_X_START) = start;
        OVL(s, OVERLAY_Y_X_END) = end;
    }
    mach64_update_shadow(s);
}

bool mach64_ovl_read(Mach64VGAState *s, unsigned reg, uint32_t *val)
{
    if (!mach64_ovl_reg(reg)) {
        return false;
    }
    *val = s->regs1[reg - MACH64_BLOCK1];
    return true;
}

void mach64_ovl_write(Mach64VGAState *s, unsigned reg, unsigned byte,
                      unsigned size, uint32_t data)
{
    uint32_t *r;
    uint32_t lanes;

    if (!mach64_ovl_reg(reg)) {
        return;
    }
    r = &s->regs1[reg - MACH64_BLOCK1];
    lanes = size >= 4 ? UINT32_MAX : ((1u << (size * 8)) - 1) << (byte * 8);
    *r = (*r & ~lanes) | ((data << (byte * 8)) & lanes);

    switch (reg) {
    case OVERLAY_SCALE_CNTL:
        /* Writing SCALE_BANDWIDTH resets the status; it never sets here. */
        *r &= ~SCALE_BANDWIDTH;
        if (*r & SCALE_GAMMA_SEL) {
            qemu_log_mask(LOG_UNIMP, "mach64: scaler gamma correction\n");
        }
        break;
    case OVERLAY_Y_X_START:
    case OVERLAY_Y_X_END:
        if (lanes & OVERLAY_LOCK) {
            s->ovl_locked = !!(*r & OVERLAY_LOCK);
        }
        break;
    }
    if (s->regs[HW_DEBUG] & HW_DEBUG_BYPASS_SUBPIC_DBF) {
        mach64_ovl_latch(s);
    }
}

/* At the start of the vertical blank: the status bits to set. */
uint32_t mach64_ovl_vblank(Mach64VGAState *s)
{
    mach64_ovl_latch(s);
    return mach64_ovl_active(s) ? CRTC_OVERLAY_EOF_INT : 0;
}

void mach64_ovl_reset(Mach64VGAState *s)
{
    memset(s->regs1, 0, sizeof(s->regs1));
    memset(s->ovl, 0, sizeof(s->ovl));
    s->ovl_locked = false;
    s->ovl_drawn_y0 = -1;
}

static void mach64_ovl_window(const Mach64VGAState *s, int *x0, int *y0,
                              int *x1, int *y1)
{
    uint32_t start = OVL(s, OVERLAY_Y_X_START);
    uint32_t end = OVL(s, OVERLAY_Y_X_END);

    *x0 = (start & OVERLAY_X_MASK) >> OVERLAY_X_SHIFT;
    *y0 = start & OVERLAY_Y_MASK;
    *x1 = (end & OVERLAY_X_MASK) >> OVERLAY_X_SHIFT;
    *y1 = end & OVERLAY_Y_MASK;
}

/*
 * The video changes without a write to the graphics lines the window covers,
 * so redraw them on every update, and the lines a moved window left.
 */
void mach64_ovl_invalidate(Mach64VGAState *s)
{
    int x0, y0 = -1, x1, y1 = -1;

    if (mach64_ovl_active(s)) {
        mach64_ovl_window(s, &x0, &y0, &x1, &y1);
        if (y1 < y0) {
            y0 = y1 = -1;
        }
    }
    if (s->ovl_drawn_y0 >= 0 &&
        (y0 != s->ovl_drawn_y0 || y1 != s->ovl_drawn_y1)) {
        vga_invalidate_scanlines(&s->vga, s->ovl_drawn_y0,
                                 s->ovl_drawn_y1 + 1);
    }
    if (y0 >= 0) {
        vga_invalidate_scanlines(&s->vga, y0, y1 + 1);
    }
    s->ovl_drawn_y0 = y0;
    s->ovl_drawn_y1 = y1;
}

static uint8_t ovl_byte(const Mach64VGAState *s, uint32_t addr)
{
    return addr < s->vga.vram_size ? s->vga.vram_ptr[addr] : 0;
}

static uint32_t ovl_le(const Mach64VGAState *s, uint32_t addr, int n)
{
    uint32_t v = 0;

    for (int i = n - 1; i >= 0; i--) {
        v = (v << 8) | ovl_byte(s, addr + i);
    }
    return v;
}

/* One source sample: Y, U, V for the YUV inputs, R, G, B for the others. */
typedef struct OvlPix {
    int c[3];
} OvlPix;

/*
 * The buffer offsets are in bytes, the pitch in pixels; for the planar
 * formats the one pitch is the Y plane's, and the U and V planes are the
 * subsampled size (RAGE PRO PRG sec 8.5, 8.6).
 */
typedef struct OvlSrc {
    unsigned fmt;
    bool yuv;
    uint32_t y_off, u_off, v_off;
    uint32_t pitch;
    int bypp;                   /* bytes per packed or Y pixel */
    int width, height;
    int sub;                    /* log2 of the planar chroma subsampling */
} OvlSrc;

/*
 * RGB sources pass through the scaler as 565 (264VT/3D RAGE RRG p. 5-9),
 * then expand to 8 bits per component by zero extension or, with
 * SCALE_PIX_EXPAND, by repeating the high bits (RAGE PRO PRG Table 8-1).
 */
static int ovl_expand(uint32_t v, int bits, bool dyn)
{
    v <<= 8 - bits;
    return dyn ? v | (v >> bits) : v;
}

static OvlPix ovl_fetch(const Mach64VGAState *s, const OvlSrc *src,
                        int x, int y)
{
    bool dyn = OVL(s, OVERLAY_SCALE_CNTL) & SCALE_PIX_EXPAND;
    uint32_t a = src->y_off + ((uint32_t)y * src->pitch + x) * src->bypp;
    uint32_t v, c;
    OvlPix p;

    switch (src->fmt) {
    case SCALER_IN_15BPP:
        v = ovl_le(s, a, 2);
        p.c[0] = ovl_expand((v >> 10) & 0x1f, 5, dyn);
        p.c[1] = ovl_expand((v >> 5) & 0x1f, 5, dyn);
        p.c[2] = ovl_expand(v & 0x1f, 5, dyn);
        break;
    case SCALER_IN_16BPP:
        v = ovl_le(s, a, 2);
        p.c[0] = ovl_expand((v >> 11) & 0x1f, 5, dyn);
        p.c[1] = ovl_expand((v >> 5) & 0x3f, 6, dyn);
        p.c[2] = ovl_expand(v & 0x1f, 5, dyn);
        break;
    case SCALER_IN_32BPP:
        v = ovl_le(s, a, 4);
        p.c[0] = ovl_expand((v >> 19) & 0x1f, 5, dyn);
        p.c[1] = ovl_expand((v >> 10) & 0x3f, 6, dyn);
        p.c[2] = ovl_expand((v >> 3) & 0x1f, 5, dyn);
        break;
    case SCALER_IN_VYUY422:     /* bytes Y0 U Y1 V */
        a -= (x & 1) * 2;
        p.c[0] = ovl_byte(s, a + (x & 1) * 2);
        p.c[1] = ovl_byte(s, a + 1);
        p.c[2] = ovl_byte(s, a + 3);
        break;
    case SCALER_IN_YVYU422:     /* bytes U Y0 V Y1 */
        a -= (x & 1) * 2;
        p.c[0] = ovl_byte(s, a + (x & 1) * 2 + 1);
        p.c[1] = ovl_byte(s, a);
        p.c[2] = ovl_byte(s, a + 2);
        break;
    default:                    /* YUV9, YUV12: planar */
        c = (uint32_t)(y >> src->sub) * (src->pitch >> src->sub) +
            (x >> src->sub);
        p.c[0] = ovl_byte(s, a);
        p.c[1] = ovl_byte(s, src->u_off + c);
        p.c[2] = ovl_byte(s, src->v_off + c);
        break;
    }
    return p;
}

/* blendedPixel = (1 - a) * current + a * next, a in 1/32 (PRG sec 8.4.4) */
static OvlPix ovl_blend(OvlPix a, OvlPix b, int alpha)
{
    for (int i = 0; i < 3; i++) {
        a.c[i] = (a.c[i] * (32 - alpha) + b.c[i] * alpha) >> 5;
    }
    return a;
}

/*
 * CCIR-601 YUV to RGB (RAGE PRO PRG sec 8.4.5), with the inputs first
 * saturated to 16..235 and 16..240.  SCALE_Y2R_TEMP clear keeps red at
 * 6500 K: "0 = Red@6500 K, GB@9300 K" (RAGE XL RRG, DP_PIX_WIDTH).
 */
static uint32_t ovl_y2r(int y, int u, int v, bool red9300)
{
    int r, g, b;

    y = MIN(MAX(y, 16), 235);
    u = MIN(MAX(u, 16), 240);
    v = MIN(MAX(v, 16), 240);
    if (red9300) {
        r = 9 * y / 8 + 25 * v / 16 - 218;
    } else {
        r = 9 * y / 8 + 25 * v / 8 - 418;
    }
    g = 9 * y / 8 - 13 * v / 16 - 25 * u / 64 + 136;
    b = 9 * y / 8 + 2 * u - 274;
    r = MIN(MAX(r, 0), 255);
    g = MIN(MAX(g, 0), 255);
    b = MIN(MAX(b, 0), 255);
    return (r << 16) | (g << 8) | b;
}

static bool ovl_key(unsigned fn, uint32_t pix, uint32_t clr, uint32_t msk)
{
    switch (fn) {
    case 1:
        return true;
    case 4:
        return (pix & msk) != (clr & msk);
    case 5:
        return (pix & msk) == (clr & msk);
    default:                    /* 0 = false, 2-3 reserved */
        return false;
    }
}

/* OVERLAY_CMP_MIX: true selects video (264VT/3D RAGE RRG p. 5-3). */
static bool ovl_mix(unsigned mix, bool g, bool v)
{
    switch (mix) {
    case 0x0: return g;
    case 0x1: return false;
    case 0x2: return true;
    case 0x3: return !g;
    case 0x4: return !v;
    case 0x5: return v ^ g;
    case 0x6: return !g ^ v;
    case 0x7: return v;
    case 0x8: return !g || !v;
    case 0x9: return g || !v;
    case 0xa: return !g || v;
    case 0xb: return g || v;
    case 0xc: return g && v;
    case 0xd: return !g && v;
    case 0xe: return g && !v;
    default:  return !g && !v;
    }
}

static bool ovl_source(const Mach64VGAState *s, OvlSrc *src)
{
    uint32_t hw = OVL(s, SCALER_HEIGHT_WIDTH);
    static bool warned;

    src->fmt = OVL(s, VIDEO_FORMAT) & SCALER_IN_MASK;
    src->width = (hw >> 16) & 0x7ff;
    src->height = hw & 0x7ff;
    src->pitch = OVL(s, SCALER_BUF_PITCH) & 0xfff;
    src->y_off = OVL(s, SCALER_BUF0_OFFSET) & 0xffffff;
    src->u_off = OVL(s, SCALER_BUF0_OFFSET_U) & 0xffffff;
    src->v_off = OVL(s, SCALER_BUF0_OFFSET_V) & 0xffffff;
    src->sub = 0;
    src->yuv = true;
    src->bypp = 2;

    switch (src->fmt) {
    case SCALER_IN_32BPP:
        src->bypp = 4;
        /* fall through */
    case SCALER_IN_15BPP:
    case SCALER_IN_16BPP:
        src->yuv = false;
        break;
    case SCALER_IN_VYUY422:
    case SCALER_IN_YVYU422:
        break;
    case SCALER_IN_YUV9:
        src->bypp = 1;
        src->sub = 2;
        break;
    case SCALER_IN_YUV12:
        src->bypp = 1;
        src->sub = 1;
        break;
    default:
        if (!warned) {
            qemu_log_mask(LOG_UNIMP, "mach64: scaler input format %u\n",
                          src->fmt >> 16);
            warned = true;
        }
        return false;
    }
    return src->width && src->height;
}

/*
 * Mix the overlay into one displayed line.  d holds the line as the VGA core
 * drew it (host xRGB8888); the graphics keyer compares the frame-buffer pixel
 * in its own depth, and the video keyer the scaled 24-bit video.
 */
void mach64_ovl_draw_line(Mach64VGAState *s, uint8_t *d, int scr_y)
{
    uint32_t *dp = (uint32_t *)d;
    uint32_t kcntl = OVL(s, OVERLAY_KEY_CNTL);
    uint32_t inc = OVL(s, OVERLAY_SCALE_INC);
    uint32_t sc = OVL(s, OVERLAY_SCALE_CNTL);
    unsigned vfn = kcntl & OVERLAY_VIDEO_FN;
    unsigned gfn = (kcntl & OVERLAY_GRAPHICS_FN) >> OVERLAY_GRAPHICS_FN_SHIFT;
    unsigned mix = (kcntl & OVERLAY_CMP_MIX) >> OVERLAY_CMP_MIX_SHIFT;
    unsigned pw = (s->regs[CRTC_GEN_CNTL] & CRTC_PIX_WIDTH) >>
                  CRTC_PIX_WIDTH_SHIFT;
    int hdisp = (((s->regs[CRTC_H_TOTAL_DISP] & CRTC_H_DISP) >> 16) + 1) * 8;
    int pitch_px = ((s->regs[CRTC_OFF_PITCH] >> CRTC_PITCH_SHIFT) & 0x3ff) * 8;
    uint32_t gbase = (s->regs[CRTC_OFF_PITCH] & CRTC_OFFSET_MASK) * 8;
    int x0, y0, x1, y1, bypp, ly, lfrac;
    uint32_t hinc, vinc, vacc, gline;
    bool hblend, vblend;
    OvlSrc src;

    if (!mach64_ovl_active(s)) {
        return;
    }
    mach64_ovl_window(s, &x0, &y0, &x1, &y1);
    if (scr_y < y0 || scr_y > y1 || !ovl_source(s, &src)) {
        return;
    }
    x1 = MIN(x1, hdisp - 1);

    switch (pw) {
    case PIX_WIDTH_8BPP:
        bypp = 1;
        break;
    case PIX_WIDTH_15BPP:
    case PIX_WIDTH_16BPP:
        bypp = 2;
        break;
    case PIX_WIDTH_24BPP:
        bypp = 3;
        break;
    default:
        bypp = 4;
        break;
    }
    gline = gbase + (uint32_t)scr_y * (pitch_px ? pitch_px : hdisp) * bypp;

    /*
     * 4.12 increments.  The horizontal accumulator steps once per scaler
     * clock (ECP), so with ECP = VCLK/2 the driver programs twice the ratio
     * (PLL_VCLK_CNTL.ECP_DIV; xf86-video-mach64 ATIMach64ScaleVideo).
     */
    hinc = (inc >> 16) >> ((s->pll_regs[PLL_VCLK_CNTL] & PLL_ECP_DIV) >> 4);
    vinc = inc & 0xffff;

    /*
     * Only YUV sources blend; RGB ones replicate pixels (264VT/3D RAGE RRG
     * p. 5-9).  When a step skips a source pixel or line, the alpha is
     * dropped (RAGE PRO PRG sec 8.4.4).
     */
    hblend = src.yuv && !(sc & SCALE_HORZ_MODE) && hinc < 0x2000;
    vblend = src.yuv && !(sc & SCALE_VERT_MODE) && vinc < 0x2000;

    vacc = (uint32_t)(scr_y - y0) * vinc;
    ly = vacc >> 12;
    lfrac = (vacc & 0xfff) >> 7;
    if (ly >= src.height - 1) {
        ly = src.height - 1;    /* "the last line ... until the end" */
        lfrac = 0;
    }

    for (int x = x0; x <= x1; x++) {
        uint32_t hacc = (uint32_t)(x - x0) * hinc;
        int lx = hacc >> 12;
        int pfrac = (hacc & 0xfff) >> 7;
        uint32_t gpix, video;
        OvlPix p;

        if (lx >= src.width - 1) {
            lx = src.width - 1;
            pfrac = 0;
        }
        p = ovl_fetch(s, &src, lx, ly);
        if (hblend && pfrac) {
            p = ovl_blend(p, ovl_fetch(s, &src, lx + 1, ly), pfrac);
        }
        if (vblend && lfrac) {
            OvlPix q = ovl_fetch(s, &src, lx, ly + 1);

            if (hblend && pfrac) {
                q = ovl_blend(q, ovl_fetch(s, &src, lx + 1, ly + 1), pfrac);
            }
            p = ovl_blend(p, q, lfrac);
        }
        if (src.yuv) {
            if (sc & SCALE_SIGNED_UV) {
                p.c[1] ^= 0x80;
                p.c[2] ^= 0x80;
            }
            video = ovl_y2r(p.c[0], p.c[1], p.c[2], sc & SCALE_Y2R_TEMP);
        } else {
            video = (p.c[0] << 16) | (p.c[1] << 8) | p.c[2];
        }

        gpix = ovl_le(s, gline + x * bypp, bypp) & 0xffffff;
        if (ovl_mix(mix,
                    ovl_key(gfn, gpix, OVL(s, OVERLAY_GRAPHICS_KEY_CLR),
                            OVL(s, OVERLAY_GRAPHICS_KEY_MSK)),
                    ovl_key(vfn, video, OVL(s, OVERLAY_VIDEO_KEY_CLR),
                            OVL(s, OVERLAY_VIDEO_KEY_MSK)))) {
            dp[x] = video;
        }
    }
}
