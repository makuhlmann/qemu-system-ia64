/*
 * QEMU ATI SVGA emulation
 * 2D engine functions
 *
 * Copyright (c) 2019 BALATON Zoltan
 *
 * This work is licensed under the GNU GPL license version 2 or later.
 */

#include "qemu/osdep.h"
#include "ati_int.h"
#include "ati_regs.h"
#include "qemu/log.h"
#include "ui/pixel_ops.h"
#include "ui/console.h"
#include "ui/rect.h"

/*
 * NOTE:
 * This is 2D _acceleration_ and supposed to be fast. Therefore, don't try to
 * reinvent the wheel (unlikely to get better with a naive implementation than
 * existing libraries) and avoid (poorly) reimplementing gfx primitives.
 * That is unnecessary and would become a performance problem. Instead, try to
 * map to and reuse existing optimised facilities (e.g. pixman) wherever
 * possible.
 */

static int ati_bpp_from_datatype(const ATIVGAState *s)
{
    switch (s->regs.dp_datatype & 0xf) {
    case 2:
        return 8;
    case 3:
    case 4:
        return 16;
    case 5:
        return 24;
    case 6:
        return 32;
    default:
        qemu_log_mask(LOG_UNIMP, "Unknown dst datatype %d\n",
                      s->regs.dp_datatype & 0xf);
        return 0;
    }
}

typedef struct {
    VGACommonState *vga;
    int bpp;
    uint32_t rop3;
    bool host_data_active;
    bool left_to_right;
    bool top_to_bottom;
    bool need_swap;
    uint32_t frgd_clr;
    uint32_t bkgd_clr;
    /*
     * Source colour-key compare (CLR_CMP_*).  Function 5 draws only where
     * the masked source colour differs from the key, which is how the
     * Windows driver blits masked icons and other transparent bitmaps
     * (RRG 3-178: 0 = always draw, 1 = never, 4 = draw on equal,
     * 5 = draw on not-equal).
     */
    unsigned cmp_fn;
    uint32_t cmp_clr;
    uint32_t cmp_msk;
    unsigned brush_type;
    uint32_t brush_data[2];
    unsigned brush_x;
    unsigned brush_y;
    QemuRect scissor;

    QemuRect dst;
    int dst_stride;
    uint8_t *dst_bits;
    uint32_t dst_offset;

    QemuRect src;
    int src_stride;
    const uint8_t *src_bits;

    /*
     * Transparent (foreground-only) source mask. When non-NULL, SRCCOPY blits
     * copy a source pixel only where the mask byte is non-zero; background
     * pixels leave the destination untouched. Used for SRC_MONO_FRGD
     * (MONO_FG_LA) host-data colour expansion. The mask is one byte per source
     * pixel with the same row stride, in pixels, as the source (src_stride /
     * bytes-per-pixel).
     */
    const uint8_t *src_fg_mask;
} ATI2DCtx;

static void ati_set_dirty(const ATI2DCtx *ctx)
{
    VGACommonState *vga = ctx->vga;
    DisplaySurface *ds = qemu_console_surface(vga->con);
    unsigned int bypp = ctx->bpp / 8;
    QemuRect dirty;
    hwaddr dirty_start;
    hwaddr dirty_end;
    hwaddr visible_start = vga->vbe_start_addr * 4;
    hwaddr visible_end = visible_start +
                         vga->vbe_regs[VBE_DISPI_INDEX_YRES] *
                         vga->vbe_line_offset;
    hwaddr start;
    hwaddr end;

    qemu_rect_intersect(&ctx->dst, &ctx->scissor, &dirty);
    if (!dirty.width || !dirty.height) {
        return;
    }
    dirty_start = ctx->dst_offset + dirty.x * bypp +
                  dirty.y * ctx->dst_stride;
    dirty_end = dirty_start + dirty.width * bypp +
                (dirty.height - 1) * ctx->dst_stride;
    start = MAX(visible_start, dirty_start);
    end = MIN(visible_end, dirty_end);

    (void)ds;
    DPRINTF("%p %u ds: %p %d %d rop: %x\n", vga->vram_ptr, vga->vbe_start_addr,
            surface_data(ds), surface_stride(ds), surface_bits_per_pixel(ds),
            ctx->rop3 >> 16);
    if (start < end) {
        memory_region_set_dirty(&vga->vram, start, end - start);
    }
}

static void setup_2d_blt_ctx(ATIVGAState *s, ATI2DCtx *ctx)
{
    ctx->vga = &s->vga;
    ctx->bpp = ati_bpp_from_datatype(s);
    ctx->rop3 = s->regs.dp_mix & GMC_ROP3_MASK;
    ctx->src_fg_mask = NULL;
    ctx->host_data_active = s->host_data.active;
    ctx->left_to_right = s->regs.dp_cntl & DST_X_LEFT_TO_RIGHT;
    ctx->top_to_bottom = s->regs.dp_cntl & DST_Y_TOP_TO_BOTTOM;
    ctx->need_swap = (HOST_BIG_ENDIAN != s->vga.big_endian_fb);
    ctx->frgd_clr = s->regs.dp_brush_frgd_clr;
    ctx->bkgd_clr = s->regs.dp_brush_bkgd_clr;
    ctx->cmp_fn = s->regs.clr_cmp_cntl & 7;
    ctx->cmp_clr = s->regs.clr_cmp_clr_src;
    ctx->cmp_msk = s->regs.clr_cmp_msk;
    ctx->brush_type = (s->regs.dp_datatype & DP_BRUSH_DATATYPE) >> 8;
    ctx->brush_data[0] = s->regs.brush_data0;
    ctx->brush_data[1] = s->regs.brush_data1;
    ctx->brush_x = s->regs.brush_y_x & 7;
    ctx->brush_y = (s->regs.brush_y_x >> 16) & 7;
    ctx->dst_offset = s->regs.dst_offset;

    ctx->scissor.width = s->regs.sc_right - s->regs.sc_left + 1;
    ctx->scissor.height = s->regs.sc_bottom - s->regs.sc_top + 1;
    ctx->scissor.x = s->regs.sc_left;
    ctx->scissor.y = s->regs.sc_top;

    ctx->dst.width = s->regs.dst_width;
    ctx->dst.height = s->regs.dst_height;
    ctx->dst.x = (ctx->left_to_right ?
                 ati_sext14(s->regs.dst_x) :
                 ati_sext14(s->regs.dst_x) + 1 - ctx->dst.width);
    ctx->dst.y = (ctx->top_to_bottom ?
                 ati_sext14(s->regs.dst_y) :
                 ati_sext14(s->regs.dst_y) + 1 - ctx->dst.height);
    ctx->dst_stride = s->regs.dst_pitch;
    ctx->dst_bits = s->vga.vram_ptr + s->regs.dst_offset;
    if (ati_is_rage128(s)) {
        /*
         * DST_PITCH is in units of 8 pixels, so the byte stride is
         * pitch * 8 * bytes_per_pixel == pitch * bpp -- except at 24bpp, where
         * the driver folds the *3 into the register itself (the RAGE 128 2D
         * engine treats a 24bpp surface as byte-wide, see r128 XAA
         * R128AccelInit: pitch = (width/8) * (cpp==3 ? 3 : 1)).  Multiplying by
         * bpp there would triple-count it, so use *8.
         */
        ctx->dst_stride *= (ctx->bpp == 24) ? 8 : ctx->bpp;
    }

    ctx->src.x = (ctx->left_to_right ?
                 ati_sext14(s->regs.src_x) :
                 ati_sext14(s->regs.src_x) + 1 - ctx->dst.width);
    ctx->src.y = (ctx->top_to_bottom ?
                 ati_sext14(s->regs.src_y) :
                 ati_sext14(s->regs.src_y) + 1 - ctx->dst.height);
    ctx->src_stride = s->regs.src_pitch;
    ctx->src_bits = s->vga.vram_ptr + s->regs.src_offset;
    if (ati_is_rage128(s)) {
        ctx->src_stride *= (ctx->bpp == 24) ? 8 : ctx->bpp;
    }
    DPRINTF("%d %d %d, %d %d %d, (%d,%d) -> (%d,%d) %dx%d %c %c\n",
            s->regs.src_offset, s->regs.dst_offset, s->regs.default_offset,
            ctx->src_stride, ctx->dst_stride, s->regs.default_pitch,
            ctx->src.x, ctx->src.y, ctx->dst.x, ctx->dst.y,
            ctx->dst.width, ctx->dst.height,
            (ctx->left_to_right ? '>' : '<'),
            (ctx->top_to_bottom ? 'v' : '^'));
}

static uint32_t make_filler(int bpp, uint32_t color)
{
    if (bpp < 24) {
        color |= color << 16;
        if (bpp < 15) {
            color |= color << 8;
        }
    }
    return color;
}

/*
 * Host-endian pixel load/store that also handle the 3-byte (24bpp) case, which
 * ldn_he_p/stn_he_p do not (they assert on any size other than 1/2/4/8).  For
 * 24bpp the low three bytes of the value are the pixel, in the same order a
 * 4-byte host-endian access would place them, so copies, fills and scanout
 * (VGA draw_line24) all agree.  Byte-swapped framebuffers are handled by the
 * callers bailing out before reaching here (see ati_2d_do_blt et al.).
 */
static inline uint32_t ati_ldpix(const uint8_t *p, unsigned bypp)
{
    if (bypp == 3) {
#if HOST_BIG_ENDIAN
        return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
#else
        return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
#endif
    }
    return ldn_he_p(p, bypp);
}

static inline void ati_stpix(uint8_t *p, unsigned bypp, uint32_t v)
{
    if (bypp == 3) {
#if HOST_BIG_ENDIAN
        p[0] = v >> 16;
        p[1] = v >> 8;
        p[2] = v;
#else
        p[0] = v;
        p[1] = v >> 8;
        p[2] = v >> 16;
#endif
        return;
    }
    stn_he_p(p, bypp, v);
}

/*
 * ROP3 codes that combine only source and destination (the pattern bits of
 * the code are dont-care).  Bitwise, so they work on raw pixel bytes at any
 * depth.
 */
/* True when this source pixel passes the colour-key test and may be drawn. */
static bool ati_cmp_pass(const ATI2DCtx *ctx, uint32_t src)
{
    switch (ctx->cmp_fn) {
    case 1:
        return false;
    case 4:
        return (src & ctx->cmp_msk) == (ctx->cmp_clr & ctx->cmp_msk);
    case 5:
    case 7:
        return (src & ctx->cmp_msk) != (ctx->cmp_clr & ctx->cmp_msk);
    default:
        return true;
    }
}

static bool ati_rop_is_src_dst(uint32_t rop3)
{
    switch (rop3 >> 16) {
    case 0x11: case 0x22: case 0x33: case 0x44: case 0x55:
    case 0x66: case 0x77: case 0x88: case 0x99: case 0xaa:
    case 0xbb: case 0xdd: case 0xee:
        return true;
    }
    return false;
}

static uint32_t ati_rop_src_dst(uint32_t rop3, uint32_t src, uint32_t dst)
{
    switch (rop3 >> 16) {
    case 0x11: return ~(src | dst);         /* NOTSRCERASE */
    case 0x22: return dst & ~src;
    case 0x33: return ~src;                 /* NOTSRCCOPY */
    case 0x44: return src & ~dst;           /* SRCERASE */
    case 0x55: return ~dst;                 /* DSTINVERT */
    case 0x66: return src ^ dst;            /* SRCINVERT */
    case 0x77: return ~(src & dst);
    case 0x88: return src & dst;            /* SRCAND */
    case 0x99: return ~(src ^ dst);
    case 0xaa: return dst;
    case 0xbb: return dst | ~src;           /* MERGEPAINT */
    case 0xdd: return src | ~dst;
    case 0xee: return src | dst;            /* SRCPAINT */
    }
    return src;
}

/*
 * Brush pixel at destination coordinates (x,y).  Returns false when the
 * pixel is transparent (FG_LA brushes leave the destination untouched on
 * background bits).  Only monochrome 8x8 patterns and solid colours are
 * modelled; 32x32 and colour brushes fall back to the foreground colour.
 */
static bool ati_brush_pixel(const ATI2DCtx *ctx, unsigned x, unsigned y,
                            uint32_t *color)
{
    unsigned row, bit;

    switch (ctx->brush_type) {
    case 0: /* 8x8 mono, fg/bg */
    case 1: /* 8x8 mono, fg/leave-alone */
        row = (y + ctx->brush_y) & 7;
        bit = (ctx->brush_data[row >> 2] >> (((row & 3) * 8) +
                                             ((x + ctx->brush_x) & 7))) & 1;
        if (bit) {
            *color = ctx->frgd_clr;
            return true;
        }
        if (ctx->brush_type == 1) {
            return false;
        }
        *color = ctx->bkgd_clr;
        return true;
    default:
        *color = ctx->frgd_clr;
        return true;
    }
}

static bool ati_2d_do_blt(const ATI2DCtx *ctx, uint8_t use_pixman)
{
    QemuRect vis_src, vis_dst;
    unsigned int x, y, i, j, bypp = ctx->bpp / 8;
    const uint8_t *vram_end = ctx->vga->vram_ptr + ctx->vga->vram_size;

    if (!ctx->bpp) {
        qemu_log_mask(LOG_GUEST_ERROR, "Invalid bpp\n");
        return false;
    }
    /*
     * 24bpp pixels are three bytes and are read/written through ati_ldpix/
     * ati_stpix.  The one case those cannot serve is a byte-swapped
     * framebuffer, where the callers' 32-bit filler bswap does not reduce to a
     * 3-byte store; no IA-64 guest uses a big-endian framebuffer.
     */
    if (bypp == 3 && ctx->need_swap) {
        qemu_log_mask(LOG_UNIMP, "24bpp blt on a byte-swapped framebuffer\n");
        return false;
    }
    if (!ctx->dst_stride) {
        qemu_log_mask(LOG_GUEST_ERROR, "Zero dest pitch\n");
        return false;
    }
    if (ctx->dst.x > 0x3fff || ctx->dst.y > 0x3fff ||
        ctx->dst_bits >= vram_end - bypp ||
        ctx->dst_bits + ctx->dst.x * bypp + (ctx->dst.y + ctx->dst.height) *
        ctx->dst_stride >= vram_end - bypp) {
        qemu_log_mask(LOG_UNIMP, "blt outside vram not implemented\n");
        return false;
    }
    qemu_rect_intersect(&ctx->dst, &ctx->scissor, &vis_dst);
    if (!vis_dst.height || !vis_dst.width) {
        /* Nothing is visible, completely clipped */
        return false;
    }
    /*
     * The src must be offset if clipping is applied to the dst.
     * This is so that when the source is blit to a dst clipped
     * on the top or left the src image is not shifted into the
     * clipped region but actually clipped.
     */
    vis_src.x = ctx->src.x + (vis_dst.x - ctx->dst.x);
    vis_src.y = ctx->src.y + (vis_dst.y - ctx->dst.y);
    vis_src.width = vis_dst.width;
    vis_src.height = vis_dst.height;

    DPRINTF("dst: (%d,%d) %dx%d -> vis_dst: (%d,%d) %dx%d\n",
            ctx->dst.x, ctx->dst.y, ctx->dst.width, ctx->dst.height,
            vis_dst.x, vis_dst.y, vis_dst.width, vis_dst.height);
    DPRINTF("src: (%d,%d) %dx%d -> vis_src: (%d,%d) %dx%d\n",
            ctx->src.x, ctx->src.y, ctx->dst.width, ctx->dst.height,
            vis_src.x, vis_src.y, vis_src.width, vis_src.height);

    switch (ctx->rop3) {
    case ROP3_SRCCOPY:
    {
        bool fallback = false;
        if (!ctx->src_stride) {
            qemu_log_mask(LOG_GUEST_ERROR, "Zero source pitch\n");
            return false;
        }
        if (!ctx->host_data_active &&
            (vis_src.x > 0x3fff || vis_src.y > 0x3fff ||
            ctx->src_bits >= vram_end - bypp ||
            ctx->src_bits + vis_src.x * bypp + (vis_src.y + vis_dst.height) *
            ctx->src_stride >= vram_end - bypp)) {
            qemu_log_mask(LOG_UNIMP, "blt outside vram not implemented\n");
            return false;
        }

        DPRINTF("pixman_blt(%p, %p, %ld, %ld, %d, %d, %d, %d, %d, %d, %d, %d)\n",
                ctx->src_bits, ctx->dst_bits,
                ctx->src_stride / sizeof(uint32_t),
                ctx->dst_stride / sizeof(uint32_t),
                ctx->bpp, ctx->bpp, vis_src.x, vis_src.y, vis_dst.x, vis_dst.y,
                vis_dst.width, vis_dst.height);
        /*
         * A same-surface copy whose source and destination rectangles overlap
         * is a screen-to-screen window move or scroll.  The hardware buffers a
         * scanline at a time, so horizontal overlap is always safe regardless
         * of DST_X_LEFT_TO_RIGHT; only the vertical walk direction matters, and
         * the Windows driver leaves the horizontal direction bit fixed.
         * pixman_blt() is undefined for overlapping regions (it shears such
         * copies), so handle this case directly: memmove each row (overlap-safe
         * within a row) walking rows away from the destination, a direction
         * derived from geometry rather than trusted from DP_CNTL.
         */
        if (ctx->src_bits == ctx->dst_bits && !ctx->src_fg_mask &&
            !ctx->cmp_fn &&
            vis_src.x < vis_dst.x + (int)vis_dst.width &&
            vis_dst.x < vis_src.x + (int)vis_dst.width &&
            vis_src.y < vis_dst.y + (int)vis_dst.height &&
            vis_dst.y < vis_src.y + (int)vis_dst.height) {
            bool top_down = vis_dst.y <= vis_src.y;

            for (y = 0; y < vis_dst.height; y++) {
                unsigned r = top_down ? y : vis_dst.height - 1 - y;

                memmove(ctx->dst_bits + vis_dst.x * bypp +
                            (vis_dst.y + r) * ctx->dst_stride,
                        ctx->src_bits + vis_src.x * bypp +
                            (vis_src.y + r) * ctx->src_stride,
                        vis_dst.width * bypp);
            }
            break;
        }
#ifdef CONFIG_PIXMAN
        int src_stride_words = ctx->src_stride / sizeof(uint32_t);
        int dst_stride_words = ctx->dst_stride / sizeof(uint32_t);
        if ((use_pixman & BIT(1)) && !ctx->src_fg_mask && !ctx->cmp_fn &&
            ctx->left_to_right && ctx->top_to_bottom) {
            fallback = !pixman_blt((uint32_t *)ctx->src_bits,
                                   (uint32_t *)ctx->dst_bits, src_stride_words,
                                   dst_stride_words, ctx->bpp, ctx->bpp,
                                   vis_src.x, vis_src.y, vis_dst.x, vis_dst.y,
                                   vis_dst.width, vis_dst.height);
        } else
#endif
        {
            fallback = true;
        }
        if (fallback) {
            unsigned mask_stride = ctx->src_stride / bypp;
            for (y = 0; y < vis_dst.height; y++) {
                unsigned src_row;
                i = vis_dst.x * bypp;
                j = vis_src.x * bypp;
                if (ctx->top_to_bottom) {
                    src_row = vis_src.y + y;
                } else {
                    src_row = vis_src.y + vis_dst.height - 1 - y;
                }
                i += (ctx->top_to_bottom ? vis_dst.y + y :
                      vis_dst.y + vis_dst.height - 1 - y) * ctx->dst_stride;
                j += src_row * ctx->src_stride;
                if (ctx->src_fg_mask || ctx->cmp_fn) {
                    /*
                     * Transparent: copy only pixels that are foreground in
                     * the mono-expansion mask and pass the colour key.
                     */
                    const uint8_t *mrow = ctx->src_fg_mask ?
                                          ctx->src_fg_mask +
                                          src_row * mask_stride + vis_src.x :
                                          NULL;
                    for (x = 0; x < vis_dst.width; x++) {
                        const uint8_t *sp = &ctx->src_bits[j + x * bypp];

                        if (mrow && !mrow[x]) {
                            continue;
                        }
                        if (ctx->cmp_fn &&
                            !ati_cmp_pass(ctx, ati_ldpix(sp, bypp))) {
                            continue;
                        }
                        memcpy(&ctx->dst_bits[i + x * bypp], sp, bypp);
                    }
                } else {
                    memmove(&ctx->dst_bits[i], &ctx->src_bits[j],
                            vis_dst.width * bypp);
                }
            }
        }
        break;
    }
    case ROP3_PATCOPY:
    case ROP3_BLACKNESS:
    case ROP3_WHITENESS:
    {
        const uint8_t *palette = ctx->vga->palette;
        uint32_t filler = 0;

        if (ctx->rop3 == ROP3_PATCOPY &&
            (ctx->brush_type == 0 || ctx->brush_type == 1)) {
            /* 8x8 monochrome pattern fill */
            uint32_t fg = make_filler(ctx->bpp, ctx->frgd_clr);
            uint32_t bg = make_filler(ctx->bpp, ctx->bkgd_clr);

            if (ctx->need_swap) {
                bswap32s(&fg);
                bswap32s(&bg);
            }
            for (y = 0; y < vis_dst.height; y++) {
                unsigned dy = vis_dst.y + y;

                i = vis_dst.x * bypp + dy * ctx->dst_stride;
                for (x = 0; x < vis_dst.width; x++, i += bypp) {
                    uint32_t c;

                    if (ati_brush_pixel(ctx, vis_dst.x + x, dy, &c)) {
                        ati_stpix(&ctx->dst_bits[i], bypp,
                                  c == ctx->frgd_clr ? fg : bg);
                    }
                }
            }
            break;
        }
        switch (ctx->rop3) {
        case ROP3_PATCOPY:
            filler = make_filler(ctx->bpp, ctx->frgd_clr);
            break;
        case ROP3_BLACKNESS:
            filler = 0xffUL << 24 | rgb_to_pixel32(palette[0], palette[1],
                                                   palette[2]);
            break;
        case ROP3_WHITENESS:
            filler = 0xffUL << 24 | rgb_to_pixel32(palette[3], palette[4],
                                                   palette[5]);
            break;
        }
        DPRINTF("pixman_fill(%p, %ld, %d, %d, %d, %d, %d, %x)\n",
                ctx->dst_bits, ctx->dst_stride / sizeof(uint32_t), ctx->bpp,
                vis_dst.x, vis_dst.y, vis_dst.width, vis_dst.height, filler);
        if (ctx->need_swap) {
            bswap32s(&filler);
        }
#ifdef CONFIG_PIXMAN
        if (!(use_pixman & BIT(0)) || ctx->bpp == 24 ||
            !pixman_fill((uint32_t *)ctx->dst_bits,
                         ctx->dst_stride / sizeof(uint32_t), ctx->bpp,
                         vis_dst.x, vis_dst.y, vis_dst.width, vis_dst.height,
                         filler))
#endif
        {
            /* fallback when pixman failed or we don't want to call it */
            for (y = 0; y < vis_dst.height; y++) {
                i = vis_dst.x * bypp + (vis_dst.y + y) * ctx->dst_stride;
                for (x = 0; x < vis_dst.width; x++, i += bypp) {
                    ati_stpix(&ctx->dst_bits[i], bypp, filler);
                }
            }
        }
        break;
    }
    default:
        if (ctx->rop3 >> 16 == 0xaa) {
            /* destination no-op */
            break;
        }
        if (ati_rop_is_src_dst(ctx->rop3)) {
            bool need_src = ctx->rop3 >> 16 != 0x55;
            unsigned mask_stride = ctx->src_stride / bypp;

            if (need_src) {
                if (!ctx->src_stride) {
                    qemu_log_mask(LOG_GUEST_ERROR, "Zero source pitch\n");
                    return false;
                }
                if (!ctx->host_data_active &&
                    (vis_src.x > 0x3fff || vis_src.y > 0x3fff ||
                    ctx->src_bits >= vram_end - bypp ||
                    ctx->src_bits + vis_src.x * bypp +
                    (vis_src.y + vis_dst.height) * ctx->src_stride >=
                    vram_end - bypp)) {
                    qemu_log_mask(LOG_UNIMP,
                                  "blt outside vram not implemented\n");
                    return false;
                }
            }
            for (y = 0; y < vis_dst.height; y++) {
                unsigned dst_row, src_row;

                if (ctx->top_to_bottom) {
                    dst_row = vis_dst.y + y;
                    src_row = vis_src.y + y;
                } else {
                    dst_row = vis_dst.y + vis_dst.height - 1 - y;
                    src_row = vis_src.y + vis_dst.height - 1 - y;
                }
                for (x = 0; x < vis_dst.width; x++) {
                    unsigned dst_col = ctx->left_to_right ?
                                       x : vis_dst.width - 1 - x;
                    uint8_t *dp = &ctx->dst_bits[(vis_dst.x + dst_col) *
                                                 bypp +
                                                 dst_row * ctx->dst_stride];
                    uint32_t sv = 0, dv;

                    if (need_src) {
                        if (ctx->src_fg_mask &&
                            !ctx->src_fg_mask[src_row * mask_stride +
                                              vis_src.x + dst_col]) {
                            continue;
                        }
                        sv = ati_ldpix(&ctx->src_bits[(vis_src.x + dst_col) *
                                                     bypp +
                                                     src_row *
                                                     ctx->src_stride], bypp);
                        if (ctx->cmp_fn && !ati_cmp_pass(ctx, sv)) {
                            continue;
                        }
                    }
                    dv = ati_ldpix(dp, bypp);
                    ati_stpix(dp, bypp, ati_rop_src_dst(ctx->rop3, sv, dv));
                }
            }
            break;
        }
        if (ctx->rop3 >> 16 == 0x5a) {
            /* PATINVERT: dst ^= pattern */
            uint32_t fg = make_filler(ctx->bpp, ctx->frgd_clr);
            uint32_t bg = make_filler(ctx->bpp, ctx->bkgd_clr);

            if (ctx->need_swap) {
                bswap32s(&fg);
                bswap32s(&bg);
            }
            for (y = 0; y < vis_dst.height; y++) {
                unsigned dy = vis_dst.y + y;

                i = vis_dst.x * bypp + dy * ctx->dst_stride;
                for (x = 0; x < vis_dst.width; x++, i += bypp) {
                    uint32_t c;

                    if (ati_brush_pixel(ctx, vis_dst.x + x, dy, &c)) {
                        ati_stpix(&ctx->dst_bits[i], bypp,
                                  ati_ldpix(&ctx->dst_bits[i], bypp) ^
                                 (c == ctx->frgd_clr ? fg : bg));
                    }
                }
            }
            break;
        }
        qemu_log_mask(LOG_UNIMP, "Unimplemented ati_2d blt op %x\n",
                      ctx->rop3 >> 16);
        return false;
    }

    return true;
}

void ati_2d_blt(ATIVGAState *s)
{
    ATI2DCtx ctx;
    uint32_t src_source = s->regs.dp_mix & DP_SRC_SOURCE;

    /* Finish any active HOST_DATA blits before starting a new blit */
    ati_host_data_finish(s);

    if (src_source == DP_SRC_HOST || src_source == DP_SRC_HOST_BYTEALIGN) {
        /* Begin a HOST_DATA blit */
        s->host_data.active = true;
        s->host_data.next = 0;
        s->host_data.col = 0;
        s->host_data.row = 0;
        return;
    }
    /*
     * A rectangle paint immediately following a setup-engine Gouraud colour
     * plane is a caption gradient: fill it with the interpolated colour
     * instead of a solid brush.  ati_setup_gouraud_fill() consumes the paint
     * only when the plane is armed and the engine is in Gouraud mode.
     */
    if (ati_setup_gouraud_fill(s)) {
        return;
    }
    setup_2d_blt_ctx(s, &ctx);
    if (ati_2d_do_blt(&ctx, s->use_pixman)) {
        ati_set_dirty(&ctx);
    }
}

bool ati_host_data_flush(ATIVGAState *s)
{
    ATI2DCtx ctx, chunk;
    unsigned bypp, pix_count, row, col, idx;
    uint8_t pix_buf[ATI_HOST_DATA_ACC_BITS * sizeof(uint32_t)];
    uint8_t fg_mask[ATI_HOST_DATA_ACC_BITS];
    uint32_t src_source = s->regs.dp_mix & DP_SRC_SOURCE;
    uint32_t src_datatype = s->regs.dp_datatype & DP_SRC_DATATYPE;
    bool transparent = (src_datatype == SRC_MONO_FRGD);

    if (!s->host_data.active) {
        return false;
    }
    if (src_source != DP_SRC_HOST) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "host_data_blt: unsupported src_source %x\n", src_source);
        return false;
    }
    if (src_datatype != SRC_MONO_FRGD_BKGD && src_datatype != SRC_MONO_FRGD &&
        src_datatype != SRC_COLOR) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "host_data_blt: undefined src_datatype %x\n",
                      src_datatype);
        return false;
    }

    setup_2d_blt_ctx(s, &ctx);

    if (!ctx.bpp) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "host_data_blt: invalid bpp from datatype\n");
        return false;
    }
    if (ctx.bpp == 24 && ctx.need_swap) {
        qemu_log_mask(LOG_UNIMP,
                      "host_data_blt: 24bpp on a byte-swapped framebuffer\n");
        return false;
    }
    if (!ctx.left_to_right || !ctx.top_to_bottom) {
        qemu_log_mask(LOG_UNIMP,
                      "host_data_blt: unsupported blit direction %c%c\n",
                      ctx.left_to_right ? '>' : '<',
                      ctx.top_to_bottom ? 'v' : '^');
        return false;
    }

    bypp = ctx.bpp / 8;
    pix_count = ATI_HOST_DATA_ACC_BITS;
    if (src_datatype == SRC_COLOR) {
        pix_count /= ctx.bpp;
        memcpy(pix_buf, s->host_data.acc, sizeof(s->host_data.acc));
    } else {
        /* Expand monochrome bits to color pixels */
        uint32_t byte_pix_order = s->regs.dp_datatype & DP_BYTE_PIX_ORDER;
        uint32_t fg = make_filler(ctx.bpp, s->regs.dp_src_frgd_clr);
        uint32_t bg = make_filler(ctx.bpp, s->regs.dp_src_bkgd_clr);

        if (ctx.need_swap) {
            bswap32s(&fg);
            bswap32s(&bg);
        }
        idx = 0;
        for (int word = 0; word < 4; word++) {
            for (int byte = 0; byte < 4; byte++) {
                uint8_t byte_val = s->host_data.acc[word] >> (byte * 8);
                for (int i = 0; i < 8; i++) {
                    bool is_fg = byte_val & BIT(byte_pix_order ? i : 7 - i);
                    fg_mask[idx / bypp] = is_fg;
                    ati_stpix(&pix_buf[idx], bypp, is_fg ? fg : bg);
                    idx += bypp;
                }
            }
        }
    }

    /* Copy and then modify blit ctx for use in a chunked blit */
    chunk = ctx;
    chunk.src_bits = pix_buf;
    chunk.src.y = 0;
    chunk.src_stride = ATI_HOST_DATA_ACC_BITS * bypp;
    /*
     * MONO_FG_LA expansion has a transparent background: only foreground bits
     * are drawn, background bits leave the destination untouched. Drive that
     * through the per-pixel mask; SRC_MONO_FRGD_BKGD and SRC_COLOR stay opaque.
     */
    chunk.src_fg_mask = transparent ? fg_mask : NULL;

    /* Blit one scanline chunk at a time */
    row = s->host_data.row;
    col = s->host_data.col;
    idx = 0;
    DPRINTF("blt %dpx @ row: %d, col: %d\n", pix_count, row, col);
    while (idx < pix_count && row < ctx.dst.height) {
        unsigned pix_in_scanline = MIN(pix_count - idx,
                                       ctx.dst.width - col);
        chunk.src.x = idx;
        /* Build a rect for this scanline chunk */
        chunk.dst.x = ctx.dst.x + col;
        chunk.dst.y = ctx.dst.y + row;
        chunk.dst.width = pix_in_scanline;
        chunk.dst.height = 1;
        DPRINTF("blt %dpx span @ row: %d, col: %d to dst (%d,%d)\n",
                pix_in_scanline, row, col, chunk.dst.x, chunk.dst.y);
        if (ati_2d_do_blt(&chunk, s->use_pixman)) {
            ati_set_dirty(&chunk);
        }
        idx += pix_in_scanline;
        col += pix_in_scanline;
        if (col >= ctx.dst.width) {
            col = 0;
            row += 1;
        }
    }

    /* Track state of the overall blit for use by the next flush */
    s->host_data.row = row;
    s->host_data.col = col;
    if (s->host_data.row >= ctx.dst.height) {
        s->host_data.active = false;
    }

    return s->host_data.active;
}

void ati_host_data_finish(ATIVGAState *s)
{
    if (ati_host_data_flush(s)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "HOST_DATA blit ended before all data was written\n");
    }
    s->host_data.active = false;
}

/*
 * Bresenham line from start to end (both packed (y << 16) | x), excluding
 * the final pixel as the hardware does by default.  Draws with the solid
 * foreground colour; ROP 0x66/0x5A XOR the destination instead, which is
 * what rubber-band lines use.
 */
void ati_2d_line(ATIVGAState *s, uint32_t start_yx, uint32_t end_yx)
{
    ATI2DCtx ctx;
    int x0 = ati_sext14(start_yx), y0 = ati_sext14(start_yx >> 16);
    int x1 = ati_sext14(end_yx), y1 = ati_sext14(end_yx >> 16);
    int dx, dy, sx, sy, err;
    unsigned bypp;
    uint32_t color;
    bool xor_rop;
    const uint8_t *vram_end;
    QemuRect dirty;

    ati_host_data_finish(s);
    setup_2d_blt_ctx(s, &ctx);
    if (!ctx.bpp || (ctx.bpp == 24 && ctx.need_swap) || !ctx.dst_stride) {
        qemu_log_mask(LOG_UNIMP, "line: unsupported state\n");
        return;
    }
    bypp = ctx.bpp / 8;
    vram_end = ctx.vga->vram_ptr + ctx.vga->vram_size;
    xor_rop = (ctx.rop3 >> 16) == 0x66 || (ctx.rop3 >> 16) == 0x5a;
    color = make_filler(ctx.bpp, ctx.frgd_clr);
    if (ctx.need_swap) {
        bswap32s(&color);
    }

    dx = x1 > x0 ? x1 - x0 : x0 - x1;
    dy = y1 > y0 ? y1 - y0 : y0 - y1;
    sx = x0 < x1 ? 1 : -1;
    sy = y0 < y1 ? 1 : -1;
    err = (dx > dy ? dx : -dy) / 2;

    while (x0 != x1 || y0 != y1) {
        int e2;

        if (x0 >= ctx.scissor.x && x0 < ctx.scissor.x + ctx.scissor.width &&
            y0 >= ctx.scissor.y && y0 < ctx.scissor.y + ctx.scissor.height) {
            uint8_t *dp = ctx.dst_bits + x0 * bypp + y0 * ctx.dst_stride;

            if (dp >= ctx.vga->vram_ptr && dp + bypp <= vram_end) {
                if (xor_rop) {
                    ati_stpix(dp, bypp, ati_ldpix(dp, bypp) ^ color);
                } else {
                    ati_stpix(dp, bypp, color);
                }
            }
        }
        e2 = err;
        if (e2 > -dx) {
            err -= dy;
            x0 += sx;
        }
        if (e2 < dy) {
            err += dx;
            y0 += sy;
        }
    }

    /* Dirty the bounding box of the segment. */
    dirty.x = MIN(ati_sext14(start_yx), x1);
    dirty.y = MIN(ati_sext14(start_yx >> 16), y1);
    dirty.width = dx + 1;
    dirty.height = dy + 1;
    ctx.dst = dirty;
    ati_set_dirty(&ctx);
}
