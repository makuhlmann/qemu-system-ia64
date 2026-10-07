/*
 * NVIDIA GeForce2 (NV15) 2D acceleration engine.
 *
 * Ported to QEMU from the Bochs Project's bx_geforce_c SVGA device
 * (Copyright (C) 2025-2026 The Bochs Project, LGPL v2+).  Only the NV15
 * (card_type 0x15) code path is retained.  Contains the blit/raster
 * primitives and the 2D-class FIFO method executors.  The forward and
 * ternary raster-op helpers, originally in Bochs' bitblt.h, are
 * reconstructed here.
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "geforce.h"
#include "geforce_pxextract.h"

#define GF_ALIGN(x, a) (((x) + (a) - 1) & ~((a) - 1))

/* ---- raster-op helpers (reconstructed from Bochs bitblt.h) ------------- */

#define ROP_FWD(name, expr)                                                   \
static void bitblt_rop_fwd_##name(uint8_t *dst, uint8_t *src,                 \
                                  int dstpitch, int srcpitch,                 \
                                  int width, int height)                      \
{                                                                             \
    for (int y = 0; y < height; y++) {                                        \
        for (int x = 0; x < width; x++) {                                     \
            uint8_t s = src[x], d = dst[x];                                   \
            (void)s; (void)d;                                                 \
            dst[x] = (uint8_t)(expr);                                         \
        }                                                                     \
        dst += dstpitch; src += srcpitch;                                     \
    }                                                                         \
}

ROP_FWD(nop,               d)
ROP_FWD(0,                 0)
ROP_FWD(1,                 0xff)
ROP_FWD(src,               s)
ROP_FWD(notsrc,            ~s)
ROP_FWD(notdst,            ~d)
ROP_FWD(src_and_dst,       s & d)
ROP_FWD(src_and_notdst,    s & ~d)
ROP_FWD(notsrc_and_dst,    ~s & d)
ROP_FWD(notsrc_and_notdst, ~s & ~d)
ROP_FWD(src_or_dst,        s | d)
ROP_FWD(src_or_notdst,     s | ~d)
ROP_FWD(notsrc_or_dst,     ~s | d)
ROP_FWD(notsrc_or_notdst,  ~s | ~d)
ROP_FWD(src_xor_dst,       s ^ d)
ROP_FWD(src_notxor_dst,    ~(s ^ d))

#undef ROP_FWD

/*
 * Ternary raster-op: for each bit position, the 3-input truth table indexed by
 * (P<<2)|(S<<1)|D selects output bit (rop >> index) & 1.
 */
static void bx_ternary_rop(uint8_t rop, uint8_t *dst, const uint8_t *src,
                           const uint8_t *patt, unsigned cb)
{
    for (unsigned i = 0; i < cb; i++) {
        uint8_t P = patt[i], S = src[i], D = dst[i], r = 0;
        for (int b = 0; b < 8; b++) {
            int idx = (((P >> b) & 1) << 2) | (((S >> b) & 1) << 1) |
                      ((D >> b) & 1);
            r |= (uint8_t)(((rop >> idx) & 1) << b);
        }
        dst[i] = r;
    }
}

#define SETUP_BITBLT(num, name, fl)                                           \
    do {                                                                      \
        s->rop_handler[num] = bitblt_rop_fwd_##name;                          \
        s->rop_flags[num] = fl;                                               \
    } while (0)

void nv2d_bitblt_init(NV15State *s)
{
    for (int i = 0; i < 0x100; i++) {
        SETUP_BITBLT(i, nop, BX_ROP_PATTERN);
    }
    SETUP_BITBLT(0x00, 0, 0);                               /* 0 */
    SETUP_BITBLT(0x05, notsrc_and_notdst, BX_ROP_PATTERN); /* PSan */
    SETUP_BITBLT(0x0a, notsrc_and_dst, BX_ROP_PATTERN);    /* DPna */
    SETUP_BITBLT(0x0f, notsrc, BX_ROP_PATTERN);            /* Pn */
    SETUP_BITBLT(0x11, notsrc_and_notdst, 0);              /* DSon */
    SETUP_BITBLT(0x22, notsrc_and_dst, 0);                 /* DSna */
    SETUP_BITBLT(0x33, notsrc, 0);                         /* Sn */
    SETUP_BITBLT(0x44, src_and_notdst, 0);                 /* SDna */
    SETUP_BITBLT(0x50, src_and_notdst, 0);                 /* PDna */
    SETUP_BITBLT(0x55, notdst, 0);                         /* Dn */
    SETUP_BITBLT(0x5a, src_xor_dst, BX_ROP_PATTERN);       /* DPx */
    SETUP_BITBLT(0x5f, notsrc_or_notdst, BX_ROP_PATTERN);  /* DSan */
    SETUP_BITBLT(0x66, src_xor_dst, 0);                    /* DSx */
    SETUP_BITBLT(0x77, notsrc_or_notdst, 0);               /* DSan */
    SETUP_BITBLT(0x88, src_and_dst, 0);                    /* DSa */
    SETUP_BITBLT(0x99, src_notxor_dst, 0);                 /* DSxn */
    SETUP_BITBLT(0xaa, nop, 0);                            /* D */
    SETUP_BITBLT(0xad, src_and_dst, BX_ROP_PATTERN);       /* DPa */
    SETUP_BITBLT(0xaf, notsrc_or_dst, BX_ROP_PATTERN);     /* DPno */
    SETUP_BITBLT(0xbb, notsrc_or_dst, 0);                  /* DSno */
    SETUP_BITBLT(0xcc, src, 0);                            /* S */
    SETUP_BITBLT(0xdd, src_and_notdst, 0);                 /* SDna */
    SETUP_BITBLT(0xee, src_or_dst, 0);                     /* DSo */
    SETUP_BITBLT(0xf0, src, BX_ROP_PATTERN);               /* P */
    SETUP_BITBLT(0xf5, src_or_notdst, BX_ROP_PATTERN);     /* PDno */
    SETUP_BITBLT(0xfa, src_or_dst, BX_ROP_PATTERN);        /* DPo */
    SETUP_BITBLT(0xff, 1, 0);                              /* 1 */
}

#undef SETUP_BITBLT

/* ---- colour-format helpers -------------------------------------------- */

static uint32_t color_565_to_888(uint16_t value)
{
    uint8_t r, g, b;
    EXTRACT_565_TO_888(value, r, g, b);
    return r << 16 | g << 8 | b;
}

static uint16_t color_888_to_565(uint32_t value)
{
    return (((value >> 19) & 0x1F) << 11) | (((value >> 10) & 0x3F) << 5) |
           ((value >> 3) & 0x1F);
}

uint32_t nv_swizzle(uint32_t x, uint32_t y, uint32_t width, uint32_t height)
{
    bool xleft = true;
    bool yleft = height != 1;
    uint32_t xbit = 1;
    uint32_t ybit = 1;
    uint32_t rbit = 1;
    uint32_t r = 0;
    do {
        if (xleft) {
            if ((x & xbit) != 0) {
                r |= rbit;
            }
            rbit <<= 1;
            xbit <<= 1;
            xleft = xbit < width;
        }
        if (yleft) {
            if ((y & ybit) != 0) {
                r |= rbit;
            }
            rbit <<= 1;
            ybit <<= 1;
            yleft = ybit < height;
        }
    } while (xleft || yleft);
    return r;
}

/* ---- pixel access + raster-op ----------------------------------------- */

uint32_t nv_get_pixel(NV15State *s, uint32_t obj, uint32_t ofs,
                      uint32_t x, uint32_t cb)
{
    uint32_t result;
    if (cb == 1) {
        result = nv_dma_read8(s, obj, ofs + x);
    } else if (cb == 2) {
        result = nv_dma_read16(s, obj, ofs + x * 2);
    } else {
        result = nv_dma_read32(s, obj, ofs + x * 4);
    }
    return result;
}

void nv_put_pixel(NV15State *s, gf_channel *ch, uint32_t ofs,
                  uint32_t x, uint32_t value)
{
    if (ch->s2d_color_bytes == 1) {
        nv_dma_write8(s, ch->s2d_img_dst, ofs + x, value);
    } else if (ch->s2d_color_bytes == 2) {
        nv_dma_write16(s, ch->s2d_img_dst, ofs + x * 2, value);
    } else if (ch->s2d_color_fmt == 6) {
        nv_dma_write32(s, ch->s2d_img_dst, ofs + x * 4, value & 0x00FFFFFF);
    } else {
        nv_dma_write32(s, ch->s2d_img_dst, ofs + x * 4, value);
    }
}

void nv_put_pixel_swzs(NV15State *s, gf_channel *ch, uint32_t ofs,
                       uint32_t value)
{
    if (ch->swzs_color_bytes == 1) {
        nv_dma_write8(s, ch->swzs_img_obj, ofs, value);
    } else if (ch->swzs_color_bytes == 2) {
        nv_dma_write16(s, ch->swzs_img_obj, ofs, value);
    } else {
        nv_dma_write32(s, ch->swzs_img_obj, ofs, value);
    }
}

/*
 * A source in X8R8G8B8 has no alpha: the pre-multiplied blend takes it as
 * opaque, as the SIFM path does for its X8R8G8B8 (format 4).  Format 5 is
 * X8R8G8B8 for IFC, SIFC and the IIFC palette; without this an opaque black
 * pixel reads as fully transparent and the blend drops it.
 */
static uint32_t nv_premult_source(uint32_t op, uint32_t color_fmt,
                                  uint32_t color_bytes, uint32_t srccolor)
{
    if (op == 5 && color_fmt == 5 && color_bytes == 4) {
        return srccolor | 0xFF000000;
    }
    return srccolor;
}

void nv_pixel_operation(NV15State *s, gf_channel *ch, uint32_t op,
                        uint32_t *dstcolor, const uint32_t *srccolor,
                        uint32_t cb, uint32_t px, uint32_t py)
{
    if (op == 1) {
        uint8_t rop = ch->rop;
        if (s->rop_flags[rop]) {
            uint32_t i = py % 8 * 8 + px % 8;
            uint32_t patt_color;
            if (ch->patt_type_color) {
                patt_color = ch->patt_data_color[i];
            } else {
                patt_color = ch->patt_data_mono[i] ? ch->patt_fg_color
                                                   : ch->patt_bg_color;
            }
            bx_ternary_rop(rop, (uint8_t *)dstcolor, (uint8_t *)srccolor,
                           (uint8_t *)&patt_color, cb);
        } else {
            s->rop_handler[rop]((uint8_t *)dstcolor, (uint8_t *)srccolor,
                                0, 0, cb, 1);
        }
    } else if (op == 5) {
        if (cb == 4) {
            if (*srccolor) {
                uint8_t sb = *srccolor;
                uint8_t sg = *srccolor >> 8;
                uint8_t sr = *srccolor >> 16;
                uint8_t sa = *srccolor >> 24;
                uint32_t beta = ch->beta;
                if (beta != 0xFFFFFFFF) {
                    uint8_t bb = beta;
                    uint8_t bg = beta >> 8;
                    uint8_t br = beta >> 16;
                    uint8_t ba = beta >> 24;
                    sb = sb * bb / 0xFF;
                    sg = sg * bg / 0xFF;
                    sr = sr * br / 0xFF;
                    sa = sa * ba / 0xFF;
                }
                uint8_t db = *dstcolor;
                uint8_t dg = *dstcolor >> 8;
                uint8_t dr = *dstcolor >> 16;
                uint8_t da = *dstcolor >> 24;
                uint8_t isa = 0xFF - sa;
                uint8_t b = alpha_wrap(db * isa / 0xFF + sb);
                uint8_t g = alpha_wrap(dg * isa / 0xFF + sg);
                uint8_t r = alpha_wrap(dr * isa / 0xFF + sr);
                uint8_t a = alpha_wrap(da * isa / 0xFF + sa);
                *dstcolor = b << 0 | g << 8 | r << 16 | a << 24;
            }
        } else {
            uint32_t beta = ch->beta;
            uint8_t bb = beta;
            uint8_t bg = beta >> 8;
            uint8_t br = beta >> 16;
            uint8_t iba = 0xFF - (beta >> 24);
            uint8_t sb = *srccolor & 0x1F;
            uint8_t sg = (*srccolor >> 5) & 0x3F;
            uint8_t sr = (*srccolor >> 11) & 0x1F;
            uint8_t db = *dstcolor & 0x1F;
            uint8_t dg = (*dstcolor >> 5) & 0x3F;
            uint8_t dr = (*dstcolor >> 11) & 0x1F;
            uint8_t b = (db * iba + sb * bb) / 0xFF;
            uint8_t g = (dg * iba + sg * bg) / 0xFF;
            uint8_t r = (dr * iba + sr * br) / 0xFF;
            *dstcolor = b << 0 | g << 5 | r << 11;
        }
    } else {
        *dstcolor = *srccolor;
    }
}

/* ---- colour-byte width bookkeeping ------------------------------------ */

static void update_color_bytes(NV15State *s, uint32_t s2d_color_fmt,
                               uint32_t color_fmt, uint32_t *color_bytes)
{
    if (s2d_color_fmt == 1) {          /* Y8 */
        *color_bytes = 1;              /* hack */
    } else if (color_fmt == 1 ||       /* R5G6B5 */
               color_fmt == 2 ||       /* A1R5G5B5 */
               color_fmt == 3) {       /* X1R5G5B5 */
        *color_bytes = 2;
    } else if (color_fmt == 4 ||       /* A8R8G8B8 */
               color_fmt == 5) {       /* X8R8G8B8 */
        *color_bytes = 4;
    } else {
        qemu_log_mask(LOG_GUEST_ERROR, "nv15: unknown color format 0x%02x\n",
                      color_fmt);
    }
}

void nv2d_update_color_bytes_s2d(NV15State *s, gf_channel *ch)
{
    if (ch->s2d_color_fmt == 0x1) {          /* Y8 */
        ch->s2d_color_bytes = 1;
    } else if (ch->s2d_color_fmt == 0x2 ||   /* X1R5G5B5_Z1R5G5B5 */
               ch->s2d_color_fmt == 0x4 ||   /* R5G6B5 */
               ch->s2d_color_fmt == 0x5) {   /* Y16 */
        ch->s2d_color_bytes = 2;
    } else if (ch->s2d_color_fmt == 0x6 ||   /* X8R8G8B8_Z8R8G8B8 */
               ch->s2d_color_fmt == 0x7 ||   /* X8R8G8B8_O8R8G8B8 */
               ch->s2d_color_fmt == 0xA ||   /* A8R8G8B8 */
               ch->s2d_color_fmt == 0xB) {   /* Y32 */
        ch->s2d_color_bytes = 4;
    } else {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "nv15: unknown 2d surface color format 0x%02x\n",
                      ch->s2d_color_fmt);
    }
}

static void update_color_bytes_ifc(NV15State *s, gf_channel *ch)
{
    update_color_bytes(s, ch->s2d_color_fmt, ch->ifc_color_fmt,
                       &ch->ifc_color_bytes);
}

static void update_color_bytes_sifc(NV15State *s, gf_channel *ch)
{
    update_color_bytes(s, ch->s2d_color_fmt, ch->sifc_color_fmt,
                       &ch->sifc_color_bytes);
}

static void update_color_bytes_tfc(NV15State *s, gf_channel *ch)
{
    update_color_bytes(s, ch->s2d_color_fmt, ch->tfc_color_fmt,
                       &ch->tfc_color_bytes);
}

void nv2d_update_color_bytes_iifc(NV15State *s, gf_channel *ch)
{
    update_color_bytes(s, 0, ch->iifc_color_fmt, &ch->iifc_color_bytes);
}

/* ---- blit primitives -------------------------------------------------- */

static void gdi_fillrect(NV15State *s, gf_channel *ch, bool clipped)
{
    int16_t clipx0 = 0, clipy0 = 0, clipx1 = 0, clipy1 = 0;
    if (clipped) {
        clipx0 = ch->gdi_clip_yx0 & 0xFFFF;
        clipy0 = ch->gdi_clip_yx0 >> 16;
        clipx1 = ch->gdi_clip_yx1 & 0xFFFF;
        clipy1 = ch->gdi_clip_yx1 >> 16;
    }
    int16_t dx, dy;
    if (clipped) {
        dx = ch->gdi_rect_yx0 & 0xFFFF;
        dy = ch->gdi_rect_yx0 >> 16;
        clipx0 -= dx;
        clipy0 -= dy;
        clipx1 -= dx;
        clipy1 -= dy;
    } else {
        dx = ch->gdi_rect_xy >> 16;
        dy = ch->gdi_rect_xy & 0xFFFF;
    }
    uint16_t width, height;
    if (clipped) {
        width = (ch->gdi_rect_yx1 & 0xFFFF) - dx;
        height = (ch->gdi_rect_yx1 >> 16) - dy;
    } else {
        width = ch->gdi_rect_wh >> 16;
        height = ch->gdi_rect_wh & 0xFFFF;
    }
    uint32_t pitch = ch->s2d_pitch_dst;
    uint32_t srccolor = ch->gdi_rect_color;
    uint32_t draw_offset = ch->s2d_ofs_dst + dy * pitch +
                           dx * ch->s2d_color_bytes;
    uint32_t redraw_offset = nv_dma_lin_lookup(s, ch->s2d_img_dst,
                                               draw_offset) - s->disp_offset;
    for (uint16_t y = 0; y < height; y++) {
        for (uint16_t x = 0; x < width; x++) {
            if (!clipped || (x >= clipx0 && x < clipx1 &&
                             y >= clipy0 && y < clipy1)) {
                uint32_t dstcolor = nv_get_pixel(s, ch->s2d_img_dst,
                    draw_offset, x, ch->s2d_color_bytes);
                nv_pixel_operation(s, ch, ch->gdi_operation,
                    &dstcolor, &srccolor, ch->s2d_color_bytes, dx + x, dy + y);
                nv_put_pixel(s, ch, draw_offset, x, dstcolor);
            }
        }
        draw_offset += pitch;
    }
    nv_redraw_area_nd_off(s, redraw_offset, width, height);
}

static void gdi_blit(NV15State *s, gf_channel *ch, uint32_t type)
{
    int16_t dx = ch->gdi_image_xy & 0xFFFF;
    int16_t dy = ch->gdi_image_xy >> 16;
    int16_t clipx0 = (ch->gdi_clip_yx0 & 0xFFFF) - dx;
    int16_t clipy0 = (ch->gdi_clip_yx0 >> 16) - dy;
    int16_t clipx1 = (ch->gdi_clip_yx1 & 0xFFFF) - dx;
    int16_t clipy1 = (ch->gdi_clip_yx1 >> 16) - dy;
    uint32_t swidth = ch->gdi_image_swh & 0xFFFF;
    uint32_t dwidth = type ? ch->gdi_image_dwh & 0xFFFF : swidth;
    uint32_t height = ch->gdi_image_swh >> 16;
    uint32_t pitch = ch->s2d_pitch_dst;
    uint32_t bg_color = ch->gdi_bg_color;
    uint32_t fg_color = ch->gdi_fg_color;
    if (ch->s2d_color_bytes == 4 && ch->gdi_color_fmt != 3) {
        bg_color = color_565_to_888(bg_color);
        fg_color = color_565_to_888(fg_color);
    }
    uint32_t draw_offset = ch->s2d_ofs_dst + dy * pitch +
                           dx * ch->s2d_color_bytes;
    uint32_t redraw_offset = nv_dma_lin_lookup(s, ch->s2d_img_dst,
                                               draw_offset) - s->disp_offset;
    uint32_t bit_index = 0;
    for (uint16_t y = 0; y < height; y++) {
        for (uint16_t x = 0; x < dwidth; x++) {
            if (x >= clipx0 && x < clipx1 && y >= clipy0 && y < clipy1) {
                uint32_t word_offset = bit_index / 32;
                uint32_t bit_offset = bit_index % 32;
                if (ch->gdi_mono_fmt == 1) {
                    bit_offset ^= 7;
                }
                /* word_offset can run past the allocation when the guest sets
                 * dwidth > swidth (unsigned underflow of swidth - dwidth) or
                 * an overflowed wordCount; clamp to the allocated buffer. */
                bool pixel = (ch->gdi_words != NULL &&
                              word_offset < ch->gdi_words_count) ?
                    ((ch->gdi_words[word_offset] >> bit_offset) & 1) : 0;
                if (type || (!type && pixel)) {
                    uint32_t dstcolor = nv_get_pixel(s, ch->s2d_img_dst,
                        draw_offset, x, ch->s2d_color_bytes);
                    uint32_t srccolor = pixel ? fg_color : bg_color;
                    nv_pixel_operation(s, ch, ch->gdi_operation,
                        &dstcolor, &srccolor, ch->s2d_color_bytes,
                        dx + x, dy + y);
                    nv_put_pixel(s, ch, draw_offset, x, dstcolor);
                }
            }
            bit_index++;
        }
        bit_index += swidth - dwidth;
        draw_offset += pitch;
    }
    nv_redraw_area_nd_off(s, redraw_offset, dwidth, height);
}

static void rect(NV15State *s, gf_channel *ch)
{
    int16_t dx = ch->rect_yx & 0xFFFF;
    int16_t dy = ch->rect_yx >> 16;
    uint16_t width = ch->rect_hw & 0xFFFF;
    uint16_t height = ch->rect_hw >> 16;
    uint32_t pitch = ch->s2d_pitch_dst;
    uint32_t srccolor = ch->rect_color;
    uint32_t draw_offset = ch->s2d_ofs_dst + dy * pitch +
                           dx * ch->s2d_color_bytes;
    uint32_t redraw_offset = nv_dma_lin_lookup(s, ch->s2d_img_dst,
                                               draw_offset) - s->disp_offset;
    for (uint16_t y = 0; y < height; y++) {
        for (uint16_t x = 0; x < width; x++) {
            uint32_t dstcolor = nv_get_pixel(s, ch->s2d_img_dst,
                draw_offset, x, ch->s2d_color_bytes);
            nv_pixel_operation(s, ch, ch->rect_operation,
                &dstcolor, &srccolor, ch->s2d_color_bytes, dx + x, dy + y);
            nv_put_pixel(s, ch, draw_offset, x, dstcolor);
        }
        draw_offset += pitch;
    }
    nv_redraw_area_nd_off(s, redraw_offset, width, height);
}

static void ifc(NV15State *s, gf_channel *ch, uint32_t word)
{
    uint32_t chromacolor = 0;
    bool chroma_enabled = false;
    if (ch->ifc_color_key_enable) {
        if (ch->ifc_color_bytes == 4) {
            chromacolor = ch->chroma_color & 0x00FFFFFF;
            chroma_enabled = ch->chroma_color & 0xFF000000;
        } else if (ch->ifc_color_bytes == 2) {
            chromacolor = ch->chroma_color & 0x0000FFFF;
            chroma_enabled = ch->chroma_color & 0xFFFF0000;
        } else {
            chromacolor = ch->chroma_color & 0x000000FF;
            chroma_enabled = ch->chroma_color & 0xFFFFFF00;
        }
    }
    for (uint32_t i = 0; i < ch->ifc_pixels_per_word; i++) {
        if (ch->ifc_x >= ch->ifc_clip_x0 && ch->ifc_x < ch->ifc_clip_x1 &&
            ch->ifc_y >= ch->ifc_clip_y0 && ch->ifc_y < ch->ifc_clip_y1) {
            uint32_t srccolor;
            if (ch->ifc_color_bytes == 4) {
                srccolor = word;
            } else if (ch->ifc_color_bytes == 2) {
                srccolor = i == 0 ? word & 0xffff : word >> 16;
            } else {
                srccolor = (word >> (i * 8)) & 0xff;
            }
            if (!chroma_enabled || srccolor != chromacolor) {
                uint32_t dstcolor = nv_get_pixel(s, ch->s2d_img_dst,
                    ch->ifc_draw_offset, ch->ifc_x, ch->s2d_color_bytes);
                srccolor = nv_premult_source(ch->ifc_operation,
                    ch->ifc_color_fmt, ch->ifc_color_bytes, srccolor);
                if (ch->ifc_color_bytes == 4 && ch->s2d_color_bytes == 2) {
                    dstcolor = color_565_to_888(dstcolor);
                }
                nv_pixel_operation(s, ch, ch->ifc_operation, &dstcolor,
                    &srccolor, ch->ifc_color_bytes,
                    ch->ifc_ofs_x + ch->ifc_x, ch->ifc_ofs_y + ch->ifc_y);
                if (ch->ifc_color_bytes == 4 && ch->s2d_color_bytes == 2) {
                    dstcolor = color_888_to_565(dstcolor);
                }
                nv_put_pixel(s, ch, ch->ifc_draw_offset, ch->ifc_x, dstcolor);
            }
        }
        ch->ifc_x++;
        if (ch->ifc_x >= ch->ifc_src_width) {
            nv_redraw_area_nd_off(s, ch->ifc_redraw_offset,
                                  ch->ifc_dst_width, 1);
            ch->ifc_draw_offset += ch->s2d_pitch_dst;
            ch->ifc_redraw_offset += ch->s2d_pitch_dst;
            ch->ifc_x = 0;
            ch->ifc_y++;
        }
    }
}

static void iifc(NV15State *s, gf_channel *ch)
{
    int16_t dx = ch->iifc_yx & 0xFFFF;
    int16_t dy = ch->iifc_yx >> 16;
    int16_t clipx0 = ch->clip_x - dx;
    int16_t clipy0 = ch->clip_y - dy;
    int16_t clipx1 = clipx0 + ch->clip_width;
    int16_t clipy1 = clipy0 + ch->clip_height;
    uint32_t swidth = ch->iifc_shw & 0xFFFF;
    uint32_t dwidth = ch->iifc_dhw & 0xFFFF;
    uint32_t height = ch->iifc_dhw >> 16;
    uint32_t pitch = ch->s2d_pitch_dst;
    uint32_t draw_offset = ch->s2d_ofs_dst + dy * pitch +
                           dx * ch->s2d_color_bytes;
    uint32_t redraw_offset = nv_dma_lin_lookup(s, ch->s2d_img_dst,
                                               draw_offset) - s->disp_offset;
    uint32_t symbol_index = 0;
    for (uint16_t y = 0; y < height; y++) {
        for (uint16_t x = 0; x < dwidth; x++) {
            if (x >= clipx0 && x < clipx1 && y >= clipy0 && y < clipy1) {
                uint8_t symbol;
                if (ch->iifc_bpp4) {
                    uint32_t word_offset = symbol_index / 8;
                    uint32_t symbol_offset = (symbol_index % 8 ^ 1) * 4;
                    symbol = (ch->iifc_words != NULL &&
                              word_offset < ch->iifc_words_count) ?
                        ((ch->iifc_words[word_offset] >> symbol_offset) & 0xF)
                        : 0;
                } else {
                    uint32_t word_offset = symbol_index / 4;
                    uint32_t symbol_offset = symbol_index % 4 * 8;
                    symbol = (ch->iifc_words != NULL &&
                              word_offset < ch->iifc_words_count) ?
                        ((ch->iifc_words[word_offset] >> symbol_offset) & 0xFF)
                        : 0;
                }
                uint32_t dstcolor = nv_get_pixel(s, ch->s2d_img_dst,
                    draw_offset, x, ch->s2d_color_bytes);
                if (ch->iifc_color_bytes == 4) {
                    uint32_t srccolor = nv_premult_source(
                        ch->iifc_operation, ch->iifc_color_fmt, 4,
                        nv_dma_read32(s, ch->iifc_palette,
                                      ch->iifc_palette_ofs + symbol * 4));
                    if (ch->s2d_color_bytes == 2) {
                        dstcolor = color_565_to_888(dstcolor);
                    }
                    nv_pixel_operation(s, ch, ch->iifc_operation,
                        &dstcolor, &srccolor, 4, dx + x, dy + y);
                    if (ch->s2d_color_bytes == 2) {
                        dstcolor = color_888_to_565(dstcolor);
                    }
                } else if (ch->iifc_color_bytes == 2) {
                    uint32_t srccolor = nv_dma_read16(s, ch->iifc_palette,
                        ch->iifc_palette_ofs + symbol * 2);
                    nv_pixel_operation(s, ch, ch->iifc_operation,
                        &dstcolor, &srccolor, 2, dx + x, dy + y);
                }
                nv_put_pixel(s, ch, draw_offset, x, dstcolor);
            }
            symbol_index++;
        }
        symbol_index += swidth - dwidth;
        draw_offset += pitch;
    }
    nv_redraw_area_nd_off(s, redraw_offset, dwidth, height);
}

static void sifc(NV15State *s, gf_channel *ch)
{
    /* dxds/dydt are raw guest scale factors and default to 0; a zero divisor
     * would SIGFPE.  A zero scale is degenerate, so skip the blit. */
    if (ch->sifc_dxds == 0 || ch->sifc_dydt == 0) {
        return;
    }
    uint16_t dx = ch->sifc_clip_yx & 0xFFFF;
    uint16_t dy = ch->sifc_clip_yx >> 16;
    uint32_t dsdx = (uint32_t)(1099511627776ULL / ch->sifc_dxds);
    uint32_t dtdy = (uint32_t)(1099511627776ULL / ch->sifc_dydt);
    uint32_t swidth = ch->sifc_shw & 0xFFFF;
    uint32_t dwidth = ch->sifc_clip_hw & 0xFFFF;
    uint32_t height = ch->sifc_clip_hw >> 16;
    uint32_t pitch = ch->s2d_pitch_dst;
    uint32_t draw_offset = ch->s2d_ofs_dst + dy * pitch +
                           dx * ch->s2d_color_bytes;
    uint32_t redraw_offset = nv_dma_lin_lookup(s, ch->s2d_img_dst,
                                               draw_offset) - s->disp_offset;
    int32_t sx0 = ((ch->sifc_syx & 0xFFFF) << 16) - (dx << 20) - 0x80000;
    int32_t sy = (ch->sifc_syx & 0xFFFF0000) - (dy << 20) - 0x80000;
    if (sx0 < 0) {
        sx0 = 0;
    }
    if (sy < 0) {
        sy = 0;
    }
    uint32_t symbol_offset_y = 0;
    for (uint16_t y = 0; y < height; y++) {
        uint32_t sx = sx0;
        for (uint16_t x = 0; x < dwidth; x++) {
            uint32_t dstcolor = nv_get_pixel(s, ch->s2d_img_dst, draw_offset,
                                             x, ch->s2d_color_bytes);
            uint32_t srccolor;
            uint32_t symbol_offset = symbol_offset_y + (sx >> 20);
            /* symbol_offset is driven by guest scale factors independently of
             * the allocation size; clamp to the buffer (counted in 32-bit
             * words, so *2 for 16-bit and *4 for 8-bit reinterpretations). */
            if (ch->sifc_color_bytes == 4) {
                srccolor = (ch->sifc_words != NULL &&
                            symbol_offset < ch->sifc_words_count) ?
                           ch->sifc_words[symbol_offset] : 0;
            } else if (ch->sifc_color_bytes == 2) {
                uint16_t *sifc_words16 = (uint16_t *)ch->sifc_words;
                srccolor = (sifc_words16 != NULL &&
                            (uint64_t)symbol_offset <
                                (uint64_t)ch->sifc_words_count * 2) ?
                           sifc_words16[symbol_offset] : 0;
            } else {
                uint8_t *sifc_words8 = (uint8_t *)ch->sifc_words;
                srccolor = (sifc_words8 != NULL &&
                            (uint64_t)symbol_offset <
                                (uint64_t)ch->sifc_words_count * 4) ?
                           sifc_words8[symbol_offset] : 0;
            }
            srccolor = nv_premult_source(ch->sifc_operation,
                ch->sifc_color_fmt, ch->sifc_color_bytes, srccolor);
            if (ch->sifc_color_bytes == 4 && ch->s2d_color_bytes == 2) {
                dstcolor = color_565_to_888(dstcolor);
            }
            nv_pixel_operation(s, ch, ch->sifc_operation, &dstcolor,
                &srccolor, ch->sifc_color_bytes, dx + x, dy + y);
            if (ch->sifc_color_bytes == 4 && ch->s2d_color_bytes == 2) {
                dstcolor = color_888_to_565(dstcolor);
            }
            nv_put_pixel(s, ch, draw_offset, x, dstcolor);
            sx += dsdx;
        }
        sy += dtdy;
        symbol_offset_y = (sy >> 20) * swidth;
        draw_offset += pitch;
    }
    nv_redraw_area_nd_off(s, redraw_offset, dwidth, height);
}

static void copyarea(NV15State *s, gf_channel *ch)
{
    uint16_t sx = ch->blit_syx & 0xFFFF;
    uint16_t sy = ch->blit_syx >> 16;
    uint16_t dx = ch->blit_dyx & 0xFFFF;
    uint16_t dy = ch->blit_dyx >> 16;
    uint16_t width = ch->blit_hw & 0xFFFF;
    uint16_t height = ch->blit_hw >> 16;
    uint32_t spitch = ch->s2d_pitch_src;
    uint32_t dpitch = ch->s2d_pitch_dst;
    uint32_t src_offset = ch->s2d_ofs_src;
    uint32_t draw_offset = ch->s2d_ofs_dst;
    bool xdir = dx > sx;
    bool ydir = dy > sy;
    src_offset += (sy + ydir * (height - 1)) * spitch + sx * ch->s2d_color_bytes;
    uint32_t redraw_offset = nv_dma_lin_lookup(s, ch->s2d_img_dst, draw_offset) +
        dy * dpitch + dx * ch->s2d_color_bytes - s->disp_offset;
    draw_offset += (dy + ydir * (height - 1)) * dpitch + dx * ch->s2d_color_bytes;
    uint32_t chromacolor = 0;
    bool chroma_enabled = false;
    if (ch->blit_color_key_enable) {
        if (ch->s2d_color_bytes == 4) {
            chromacolor = ch->chroma_color & 0x00FFFFFF;
            chroma_enabled = ch->chroma_color & 0xFF000000;
        } else if (ch->s2d_color_bytes == 2) {
            chromacolor = ch->chroma_color & 0x0000FFFF;
            chroma_enabled = ch->chroma_color & 0xFFFF0000;
        } else {
            chromacolor = ch->chroma_color & 0x000000FF;
            chroma_enabled = ch->chroma_color & 0xFFFFFF00;
        }
    }
    for (uint16_t y = 0; y < height; y++) {
        for (uint16_t x = 0; x < width; x++) {
            uint16_t xa = xdir ? width - x - 1 : x;
            uint32_t srccolor = nv_get_pixel(s, ch->s2d_img_src,
                src_offset, xa, ch->s2d_color_bytes);
            if (!chroma_enabled || srccolor != chromacolor) {
                uint32_t dstcolor = nv_get_pixel(s, ch->s2d_img_dst,
                    draw_offset, xa, ch->s2d_color_bytes);
                nv_pixel_operation(s, ch, ch->blit_operation,
                    &dstcolor, &srccolor, ch->s2d_color_bytes, dx + x, dy + y);
                nv_put_pixel(s, ch, draw_offset, xa, dstcolor);
            }
        }
        src_offset += spitch * (1 - 2 * ydir);
        draw_offset += dpitch * (1 - 2 * ydir);
    }
    nv_redraw_area_nd_off(s, redraw_offset, width, height);
}

static void m2mf(NV15State *s, gf_channel *ch)
{
    uint32_t src_offset = ch->m2mf_src_offset;
    uint32_t dst_offset = ch->m2mf_dst_offset;
    for (uint16_t y = 0; y < ch->m2mf_line_count; y++) {
        nv_dma_copy(s, ch->m2mf_dst, dst_offset, ch->m2mf_src, src_offset,
                    ch->m2mf_line_length);
        src_offset += ch->m2mf_src_pitch;
        dst_offset += ch->m2mf_dst_pitch;
    }
    uint32_t dma_target = nv_ramin_read32(s, ch->m2mf_dst) >> 12 & 0xFF;
    if (dma_target == 0x03 || dma_target == 0x0b) {
        uint32_t redraw_offset = nv_dma_lin_lookup(s, ch->m2mf_dst,
            ch->m2mf_dst_offset) - s->disp_offset;
        uint32_t width = ch->m2mf_line_length / (s->svga_bpp >> 3);
        nv_redraw_area_nd_off(s, redraw_offset, width, ch->m2mf_line_count);
    }
}

static void tfc(NV15State *s, gf_channel *ch)
{
    uint16_t dx = ch->tfc_yx & 0xFFFF;
    uint16_t dy = ch->tfc_yx >> 16;
    int16_t clipx0 = (ch->tfc_clip_wx & 0xFFFF) - dx;
    int16_t clipy0 = (ch->tfc_clip_hy & 0xFFFF) - dy;
    int16_t clipx1 = clipx0 + (ch->tfc_clip_wx >> 16);
    int16_t clipy1 = clipy0 + (ch->tfc_clip_hy >> 16);
    uint32_t width = ch->tfc_hw & 0xFFFF;
    uint32_t height = ch->tfc_hw >> 16;
    uint32_t word_offset = 0;
    if (ch->tfc_swizzled) {
        for (uint16_t y = 0; y < height; y++) {
            for (uint16_t x = 0; x < width; x++) {
                if (x >= clipx0 && x < clipx1 && y >= clipy0 && y < clipy1) {
                    uint32_t srccolor;
                    if (ch->tfc_color_bytes == 4) {
                        srccolor = ch->tfc_words[word_offset];
                    } else if (ch->tfc_color_bytes == 2) {
                        uint16_t *tfc_words16 = (uint16_t *)ch->tfc_words;
                        srccolor = tfc_words16[word_offset];
                    } else {
                        uint8_t *tfc_words8 = (uint8_t *)ch->tfc_words;
                        srccolor = tfc_words8[word_offset];
                    }
                    nv_put_pixel_swzs(s, ch, ch->swzs_ofs +
                        nv_swizzle(x + dx, y + dy, ch->swzs_width,
                                   ch->swzs_height) * ch->swzs_color_bytes,
                        srccolor);
                }
                word_offset++;
            }
        }
    } else {
        uint32_t pitch = ch->s2d_pitch_dst;
        uint32_t draw_offset = ch->s2d_ofs_dst + dy * pitch +
                               dx * ch->s2d_color_bytes;
        for (uint16_t y = 0; y < height; y++) {
            for (uint16_t x = 0; x < width; x++) {
                if (x >= clipx0 && x < clipx1 && y >= clipy0 && y < clipy1) {
                    uint32_t srccolor;
                    if (ch->tfc_color_bytes == 4) {
                        srccolor = ch->tfc_words[word_offset];
                    } else if (ch->tfc_color_bytes == 2) {
                        uint16_t *tfc_words16 = (uint16_t *)ch->tfc_words;
                        srccolor = tfc_words16[word_offset];
                    } else {
                        uint8_t *tfc_words8 = (uint8_t *)ch->tfc_words;
                        srccolor = tfc_words8[word_offset];
                    }
                    nv_put_pixel(s, ch, draw_offset, x, srccolor);
                }
                word_offset++;
            }
            draw_offset += pitch;
        }
    }
}

static void sifm(NV15State *s, gf_channel *ch, bool swizzled)
{
    uint16_t dx = ch->sifm_dyx & 0xFFFF;
    uint16_t dy = ch->sifm_dyx >> 16;
    uint16_t dwidth = ch->sifm_dhw & 0xFFFF;
    uint16_t dheight = ch->sifm_dhw >> 16;
    uint32_t spitch = ch->sifm_sfmt & 0xFFFF;
    /* SIFM without scaling is used frequently in some operating systems */
    if (ch->sifm_dudx == 0x00100000 && ch->sifm_dvdy == 0x00100000) {
        uint16_t sx = (ch->sifm_syx & 0xFFFF) >> 4;
        uint16_t sy = (ch->sifm_syx >> 16) >> 4;
        uint32_t src_offset = ch->sifm_sofs + sy * spitch +
                              sx * ch->sifm_color_bytes;
        if (swizzled) {
            for (uint16_t y = 0; y < dheight; y++) {
                for (uint16_t x = 0; x < dwidth; x++) {
                    uint32_t srccolor = nv_get_pixel(s, ch->sifm_src,
                        src_offset, x, ch->sifm_color_bytes);
                    if (ch->sifm_color_bytes == 2 && ch->swzs_color_bytes == 4) {
                        srccolor = color_565_to_888(srccolor);
                    }
                    nv_put_pixel_swzs(s, ch, ch->swzs_ofs +
                        nv_swizzle(x + dx, y + dy, ch->swzs_width,
                                   ch->swzs_height) * ch->swzs_color_bytes,
                        srccolor);
                }
                src_offset += spitch;
            }
        } else {
            uint32_t dpitch = ch->s2d_pitch_dst;
            uint32_t draw_offset = ch->s2d_ofs_dst + dy * dpitch +
                                   dx * ch->s2d_color_bytes;
            uint32_t redraw_offset = nv_dma_lin_lookup(s, ch->s2d_img_dst,
                draw_offset) - s->disp_offset;
            for (uint16_t y = 0; y < dheight; y++) {
                for (uint16_t x = 0; x < dwidth; x++) {
                    uint32_t dstcolor = nv_get_pixel(s, ch->s2d_img_dst,
                        draw_offset, x, ch->s2d_color_bytes);
                    uint32_t srccolor = nv_get_pixel(s, ch->sifm_src,
                        src_offset, x, ch->sifm_color_bytes);
                    if (ch->sifm_color_fmt == 4) {
                        srccolor |= 0xFF000000;
                    }
                    nv_pixel_operation(s, ch, ch->sifm_operation,
                        &dstcolor, &srccolor, ch->s2d_color_bytes,
                        dx + x, dy + y);
                    nv_put_pixel(s, ch, draw_offset, x, dstcolor);
                }
                src_offset += spitch;
                draw_offset += dpitch;
            }
            nv_redraw_area_nd_off(s, redraw_offset, dwidth, dheight);
        }
    } else {
        int32_t sx0 = ((ch->sifm_syx & 0xFFFF) << 16) - 0x80000;
        int32_t sy = (ch->sifm_syx & 0xFFFF0000) +
                     ((int32_t)ch->sifm_dvdy < 0 ? 0x80000 : -0x80000);
        if (sx0 < 0) {
            sx0 = 0;
        }
        if (sy < 0) {
            sy = 0;
        }
        if (swizzled) {
            for (uint16_t y = 0; y < dheight; y++) {
                uint32_t sx = sx0;
                uint32_t src_offset = ch->sifm_sofs + (sy >> 20) * spitch;
                for (uint16_t x = 0; x < dwidth; x++) {
                    uint32_t srccolor = nv_get_pixel(s, ch->sifm_src,
                        src_offset, sx >> 20, ch->sifm_color_bytes);
                    if (ch->sifm_color_bytes == 2 && ch->swzs_color_bytes == 4) {
                        srccolor = color_565_to_888(srccolor);
                    }
                    nv_put_pixel_swzs(s, ch, ch->swzs_ofs +
                        nv_swizzle(x + dx, y + dy, ch->swzs_width,
                                   ch->swzs_height) * ch->swzs_color_bytes,
                        srccolor);
                    sx += ch->sifm_dudx;
                }
                sy += ch->sifm_dvdy;
            }
        } else {
            uint32_t dpitch = ch->s2d_pitch_dst;
            uint32_t draw_offset = ch->s2d_ofs_dst + dy * dpitch +
                                   dx * ch->s2d_color_bytes;
            uint32_t redraw_offset = nv_dma_lin_lookup(s, ch->s2d_img_dst,
                draw_offset) - s->disp_offset;
            for (uint16_t y = 0; y < dheight; y++) {
                uint32_t sx = sx0;
                uint32_t src_offset = ch->sifm_sofs + (sy >> 20) * spitch;
                for (uint16_t x = 0; x < dwidth; x++) {
                    uint32_t dstcolor = nv_get_pixel(s, ch->s2d_img_dst,
                        draw_offset, x, ch->s2d_color_bytes);
                    uint32_t srccolor = nv_get_pixel(s, ch->sifm_src,
                        src_offset, sx >> 20, ch->sifm_color_bytes);
                    if (ch->sifm_color_fmt == 4) {
                        srccolor |= 0xFF000000;
                    }
                    nv_pixel_operation(s, ch, ch->sifm_operation,
                        &dstcolor, &srccolor, ch->s2d_color_bytes,
                        dx + x, dy + y);
                    nv_put_pixel(s, ch, draw_offset, x, dstcolor);
                    sx += ch->sifm_dudx;
                }
                sy += ch->sifm_dvdy;
                draw_offset += dpitch;
            }
            nv_redraw_area_nd_off(s, redraw_offset, dwidth, dheight);
        }
    }
}

/* ---- 2D class method executors (dispatched from geforce.c) ------------ */

void nv2d_execute_clip(NV15State *s, gf_channel *ch, uint32_t method,
                       uint32_t param)
{
    if (method == 0x0c0) {
        ch->clip_x = (uint16_t)param;
        ch->clip_y = param >> 16;
    } else if (method == 0x0c1) {
        ch->clip_width = (uint16_t)param;
        ch->clip_height = param >> 16;
    }
}

void nv2d_execute_m2mf(NV15State *s, gf_channel *ch, uint32_t subc,
                       uint32_t method, uint32_t param)
{
    if (method == 0x061) {
        ch->m2mf_src = param;
    } else if (method == 0x062) {
        ch->m2mf_dst = param;
    } else if (method == 0x0c3) {
        ch->m2mf_src_offset = param;
    } else if (method == 0x0c4) {
        ch->m2mf_dst_offset = param;
    } else if (method == 0x0c5) {
        ch->m2mf_src_pitch = param;
    } else if (method == 0x0c6) {
        ch->m2mf_dst_pitch = param;
    } else if (method == 0x0c7) {
        ch->m2mf_line_length = param;
    } else if (method == 0x0c8) {
        ch->m2mf_line_count = param;
    } else if (method == 0x0c9) {
        ch->m2mf_format = param;
    } else if (method == 0x0ca) {
        ch->m2mf_buffer_notify = param;
        m2mf(s, ch);
        if ((nv_ramin_read32(s, ch->schs[subc].notifier) & 0xFF) == 0x30) {
            /* notify skipped */
        } else {
            nv_dma_write64(s, ch->schs[subc].notifier, 0x10 + 0x0,
                           nv_get_current_time(s));
            nv_dma_write32(s, ch->schs[subc].notifier, 0x10 + 0x8, 0);
            nv_dma_write32(s, ch->schs[subc].notifier, 0x10 + 0xC, 0);
        }
    }
}

void nv2d_execute_rop(NV15State *s, gf_channel *ch, uint32_t method,
                      uint32_t param)
{
    if (method == 0x0c0) {
        ch->rop = param;
    }
}

void nv2d_execute_patt(NV15State *s, gf_channel *ch, uint32_t method,
                       uint32_t param)
{
    if (method == 0x0c2) {
        ch->patt_shape = param;
    } else if (method == 0x0c3) {
        ch->patt_type_color = param == 2;
    } else if (method == 0x0c4) {
        ch->patt_bg_color = param;
    } else if (method == 0x0c5) {
        ch->patt_fg_color = param;
    } else if (method == 0x0c6 || method == 0x0c7) {
        for (uint32_t i = 0; i < 32; i++) {
            ch->patt_data_mono[i + (method & 1) * 32] = 1 << (i ^ 7) & param;
        }
    } else if (method >= 0x100 && method < 0x110) {
        uint32_t i = (method - 0x100) * 4;
        ch->patt_data_color[i] = param & 0xFF;
        ch->patt_data_color[i + 1] = (param >> 8) & 0xFF;
        ch->patt_data_color[i + 2] = (param >> 16) & 0xFF;
        ch->patt_data_color[i + 3] = param >> 24;
    } else if (method >= 0x140 && method < 0x160) {
        uint32_t i = (method - 0x140) * 2;
        ch->patt_data_color[i] = param & 0xFFFF;
        ch->patt_data_color[i + 1] = param >> 16;
    } else if (method >= 0x1c0 && method < 0x200) {
        ch->patt_data_color[method - 0x1c0] = param;
    }
}

void nv2d_execute_gdi(NV15State *s, gf_channel *ch, uint32_t cls,
                      uint32_t method, uint32_t param)
{
    if (method == 0x0bf) {
        ch->gdi_operation = param;
    } else if (method == 0x0c0) {
        ch->gdi_color_fmt = param;
    } else if (method == 0x0c1) {
        ch->gdi_mono_fmt = param;
    } else if (method == 0x0ff) {
        ch->gdi_rect_color = param;
    } else if (method >= 0x100 && method < 0x140) {
        if (method & 1) {
            ch->gdi_rect_wh = param;
            gdi_fillrect(s, ch, false);
        } else {
            ch->gdi_rect_xy = param;
        }
    } else if (method == 0x17d) {
        ch->gdi_clip_yx0 = param;
    } else if (method == 0x17e) {
        ch->gdi_clip_yx1 = param;
    } else if (method == 0x17f) {
        ch->gdi_rect_color = param;
    } else if (method >= 0x180 && method < 0x1c0) {
        if (method & 1) {
            ch->gdi_rect_yx1 = param;
            gdi_fillrect(s, ch, true);
        } else {
            ch->gdi_rect_yx0 = param;
        }
    } else if ((method == 0x1fb && cls == 0x004a) ||
               (method == 0x2fb && cls == 0x004b)) {
        ch->gdi_clip_yx0 = param;
    } else if ((method == 0x1fc && cls == 0x004a) ||
               (method == 0x2fc && cls == 0x004b)) {
        ch->gdi_clip_yx1 = param;
    } else if ((method == 0x1fd && cls == 0x004a) ||
               (method == 0x2fd && cls == 0x004b)) {
        ch->gdi_fg_color = param;
    } else if ((method == 0x1fe && cls == 0x004a) ||
               (method == 0x2fe && cls == 0x004b)) {
        ch->gdi_image_swh = param;
    } else if ((method == 0x1ff && cls == 0x004a) ||
               (method == 0x2ff && cls == 0x004b)) {
        ch->gdi_image_xy = param;
        uint32_t width = ch->gdi_image_swh & 0xFFFF;
        uint32_t height = ch->gdi_image_swh >> 16;
        uint32_t wordCount = GF_ALIGN(width * height, 32) >> 5;
        g_free(ch->gdi_words);
        ch->gdi_words_ptr = 0;
        ch->gdi_words_left = wordCount;
        ch->gdi_words_count = wordCount;
        ch->gdi_words = g_new(uint32_t, wordCount);
    } else if ((method >= 0x200 && method < 0x280 && cls == 0x004a) ||
               (method >= 0x300 && method < 0x380 && cls == 0x004b)) {
        if (ch->gdi_words != NULL && ch->gdi_words_left > 0) {
            ch->gdi_words[ch->gdi_words_ptr++] = param;
            ch->gdi_words_left--;
            if (!ch->gdi_words_left) {
                gdi_blit(s, ch, 0);
                g_free(ch->gdi_words);
                ch->gdi_words = NULL;
            }
        }
    } else if ((method == 0x2f9 && cls == 0x004a) ||
               (method == 0x4f9 && cls == 0x004b)) {
        ch->gdi_clip_yx0 = param;
    } else if ((method == 0x2fa && cls == 0x004a) ||
               (method == 0x4fa && cls == 0x004b)) {
        ch->gdi_clip_yx1 = param;
    } else if ((method == 0x2fb && cls == 0x004a) ||
               (method == 0x4fb && cls == 0x004b)) {
        ch->gdi_bg_color = param;
    } else if ((method == 0x2fc && cls == 0x004a) ||
               (method == 0x4fc && cls == 0x004b)) {
        ch->gdi_fg_color = param;
    } else if ((method == 0x2fd && cls == 0x004a) ||
               (method == 0x4fd && cls == 0x004b)) {
        ch->gdi_image_swh = param;
    } else if ((method == 0x2fe && cls == 0x004a) ||
               (method == 0x4fe && cls == 0x004b)) {
        ch->gdi_image_dwh = param;
    } else if ((method == 0x2ff && cls == 0x004a) ||
               (method == 0x4ff && cls == 0x004b)) {
        ch->gdi_image_xy = param;
        uint32_t width = ch->gdi_image_swh & 0xFFFF;
        uint32_t height = ch->gdi_image_swh >> 16;
        uint32_t wordCount = GF_ALIGN(width * height, 32) >> 5;
        g_free(ch->gdi_words);
        ch->gdi_words_ptr = 0;
        ch->gdi_words_left = wordCount;
        ch->gdi_words_count = wordCount;
        ch->gdi_words = g_new(uint32_t, wordCount);
    } else if ((method >= 0x300 && method < 0x380 && cls == 0x004a) ||
               (method >= 0x500 && method < 0x580 && cls == 0x004b)) {
        if (ch->gdi_words != NULL && ch->gdi_words_left > 0) {
            ch->gdi_words[ch->gdi_words_ptr++] = param;
            ch->gdi_words_left--;
            if (!ch->gdi_words_left) {
                gdi_blit(s, ch, 1);
                g_free(ch->gdi_words);
                ch->gdi_words = NULL;
            }
        }
    } else if (method == 0x3fd) {
        ch->gdi_clip_yx0 = param;
    } else if (method == 0x3fe) {
        ch->gdi_clip_yx1 = param;
    } else if (method == 0x3ff) {
        ch->gdi_fg_color = param;
    }
}

void nv2d_execute_swzsurf(NV15State *s, gf_channel *ch, uint32_t method,
                          uint32_t param)
{
    if (method == 0x061) {
        ch->swzs_img_obj = param;
    } else if (method == 0x0c0) {
        ch->swzs_fmt = param;
        ch->swzs_width = 1 << ((param >> 16) & 0xff);
        ch->swzs_height = 1 << (param >> 24);
        uint32_t color_fmt = param & 0xffff;
        if (color_fmt == 1) {             /* Y8 */
            ch->swzs_color_bytes = 1;
        } else if (color_fmt == 2 ||      /* X1R5G5B5_Z1R5G5B5 */
                   color_fmt == 4) {      /* R5G6B5 */
            ch->swzs_color_bytes = 2;
        } else if (color_fmt == 0x6 ||    /* X8R8G8B8_Z8R8G8B8 */
                   color_fmt == 0xA ||    /* A8R8G8B8 */
                   color_fmt == 0xB) {    /* Y32 */
            ch->swzs_color_bytes = 4;
        } else {
            qemu_log_mask(LOG_GUEST_ERROR,
                "nv15: unknown swizzled surface color format 0x%02x\n",
                color_fmt);
        }
    } else if (method == 0x0c1) {
        ch->swzs_ofs = param;
    }
}

void nv2d_execute_chroma(NV15State *s, gf_channel *ch, uint32_t method,
                         uint32_t param)
{
    if (method == 0x0c0) {
        ch->chroma_color_fmt = param;
    } else if (method == 0x0c1) {
        ch->chroma_color = param;
    }
}

void nv2d_execute_rect(NV15State *s, gf_channel *ch, uint32_t method,
                       uint32_t param)
{
    if (method == 0x0bf) {
        ch->rect_operation = param;
    } else if (method == 0x0c0) {
        ch->rect_color_fmt = param;
    } else if (method == 0x0c1) {
        ch->rect_color = param;
    } else if (method >= 0x100 && method < 0x120) {
        if (method & 1) {
            ch->rect_hw = param;
            rect(s, ch);
        } else {
            ch->rect_yx = param;
        }
    }
}

/* ---- NV4_LIN (0x5c) / NV4_TRI (0x5d): solid line and triangle ---------- */

/* One solid pixel through the destination surface, honouring the ROP/op. */
static void nv2d_solid_pixel(NV15State *s, gf_channel *ch, uint32_t op,
                             uint32_t color, int px, int py)
{
    if (px < 0 || py < 0) {
        return;
    }
    uint32_t draw_offset = ch->s2d_ofs_dst + (uint32_t)py * ch->s2d_pitch_dst;
    uint32_t dstcolor = nv_get_pixel(s, ch->s2d_img_dst, draw_offset,
                                     (uint32_t)px, ch->s2d_color_bytes);
    uint32_t srccolor = color;
    nv_pixel_operation(s, ch, op, &dstcolor, &srccolor, ch->s2d_color_bytes,
                       (uint32_t)px, (uint32_t)py);
    nv_put_pixel(s, ch, draw_offset, (uint32_t)px, dstcolor);
}

/* Bresenham line from (x0,y0) to (x1,y1). */
static void nv2d_lin_draw(NV15State *s, gf_channel *ch, uint32_t op,
                          int x0, int y0, int x1, int y1)
{
    int dx = abs(x1 - x0);
    int dy = -abs(y1 - y0);
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    /* Bound the step count against absurd guest coordinates. */
    int guard = dx - dy + 2;
    for (;;) {
        nv2d_solid_pixel(s, ch, op, ch->lin_color, x0, y0);
        if ((x0 == x1 && y0 == y1) || guard-- <= 0) {
            break;
        }
        int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

/* x coordinate where edge (ax,ay)-(bx,by) crosses scanline y. */
static int nv2d_edge_x(int ax, int ay, int bx, int by, int y)
{
    if (by == ay) {
        return ax;
    }
    return ax + (int)((int64_t)(bx - ax) * (y - ay) / (by - ay));
}

/* Filled triangle via scanline spans. */
static void nv2d_tri_fill(NV15State *s, gf_channel *ch, uint32_t op,
                          int x0, int y0, int x1, int y1, int x2, int y2)
{
    int t;
    if (y0 > y1) { t = x0; x0 = x1; x1 = t; t = y0; y0 = y1; y1 = t; }
    if (y0 > y2) { t = x0; x0 = x2; x2 = t; t = y0; y0 = y2; y2 = t; }
    if (y1 > y2) { t = x1; x1 = x2; x2 = t; t = y1; y1 = y2; y2 = t; }
    if (y2 == y0) {
        return; /* degenerate / zero height */
    }
    int ymin = y0 < 0 ? 0 : y0;
    int ymax = y2 > 8192 ? 8192 : y2;   /* clamp work against wild coords */
    for (int y = ymin; y <= ymax; y++) {
        int xa = nv2d_edge_x(x0, y0, x2, y2, y);
        int xb = (y < y1) ? nv2d_edge_x(x0, y0, x1, y1, y)
                          : nv2d_edge_x(x1, y1, x2, y2, y);
        int xl = xa < xb ? xa : xb;
        int xr = xa < xb ? xb : xa;
        if (xl < 0) {
            xl = 0;
        }
        if (xr > 16384) {
            xr = 16384;
        }
        for (int x = xl; x <= xr; x++) {
            nv2d_solid_pixel(s, ch, op, ch->tri_color, x, y);
        }
    }
}

void nv2d_execute_lin(NV15State *s, gf_channel *ch, uint32_t method,
                      uint32_t param)
{
    if (method == 0x0bf) {
        ch->lin_operation = param;
    } else if (method == 0x0c0) {
        ch->lin_color_fmt = param;
    } else if (method == 0x0c1) {
        ch->lin_color = param;
    } else if (method >= 0x100 && method < 0x120) {
        /* Lin[16]: point0 (even) then point1 (odd); draw on point1. */
        if (method & 1) {
            nv2d_lin_draw(s, ch, ch->lin_operation,
                          (int16_t)(ch->lin_point0 & 0xFFFF),
                          (int16_t)(ch->lin_point0 >> 16),
                          (int16_t)(param & 0xFFFF),
                          (int16_t)(param >> 16));
        } else {
            ch->lin_point0 = param;
        }
    } else if (method >= 0x120 && method < 0x140) {
        /* Lin32[8]: x0, y0, x1, y1 (4 dwords each). */
        ch->lin_l32[ch->lin_l32_idx & 3] = param;
        if ((++ch->lin_l32_idx & 3) == 0) {
            nv2d_lin_draw(s, ch, ch->lin_operation,
                          (int32_t)ch->lin_l32[0], (int32_t)ch->lin_l32[1],
                          (int32_t)ch->lin_l32[2], (int32_t)ch->lin_l32[3]);
        }
    } else if (method >= 0x140 && method < 0x160) {
        /* PolyLin: connect consecutive points (Y<<16 | X). */
        int x = (int16_t)(param & 0xFFFF);
        int y = (int16_t)(param >> 16);
        if (method == 0x140) {
            ch->lin_poly_valid = false;   /* first point restarts the run */
        }
        if (ch->lin_poly_valid) {
            nv2d_lin_draw(s, ch, ch->lin_operation,
                          ch->lin_poly_x, ch->lin_poly_y, x, y);
        }
        ch->lin_poly_x = x;
        ch->lin_poly_y = y;
        ch->lin_poly_valid = true;
    } else if (method >= 0x100) {
        NV_TRACE(s, "nv15 LIN unhandled method 0x%03x param 0x%08x\n",
                 method, param);
    }
}

void nv2d_execute_tri(NV15State *s, gf_channel *ch, uint32_t method,
                      uint32_t param)
{
    if (method == 0x0bf) {
        ch->tri_operation = param;
    } else if (method == 0x0c0) {
        ch->tri_color_fmt = param;
    } else if (method == 0x0c1) {
        ch->tri_color = param;
    } else if (method >= 0x0c4 && method <= 0x0c6) {
        /* Triangle: point0/1/2 (Y<<16 | X); draw on point2. */
        uint32_t i = method - 0x0c4;
        ch->tri_x[i] = (int16_t)(param & 0xFFFF);
        ch->tri_y[i] = (int16_t)(param >> 16);
        if (method == 0x0c6) {
            nv2d_tri_fill(s, ch, ch->tri_operation,
                          ch->tri_x[0], ch->tri_y[0], ch->tri_x[1],
                          ch->tri_y[1], ch->tri_x[2], ch->tri_y[2]);
        }
    } else if (method >= 0x0c8 && method <= 0x0cd) {
        /* Triangle32: x0, y0, x1, y1, x2, y2. */
        ch->tri_t32[method - 0x0c8] = param;
        if (method == 0x0cd) {
            nv2d_tri_fill(s, ch, ch->tri_operation,
                          (int32_t)ch->tri_t32[0], (int32_t)ch->tri_t32[1],
                          (int32_t)ch->tri_t32[2], (int32_t)ch->tri_t32[3],
                          (int32_t)ch->tri_t32[4], (int32_t)ch->tri_t32[5]);
        }
    } else if (method >= 0x100 && method < 0x140) {
        /* TriangleMesh: sliding window of the last three vertices (strip). */
        int x = (int16_t)(param & 0xFFFF);
        int y = (int16_t)(param >> 16);
        if (method == 0x100) {
            ch->tri_count = 0;            /* first point restarts the mesh */
        }
        ch->tri_x[0] = ch->tri_x[1];
        ch->tri_y[0] = ch->tri_y[1];
        ch->tri_x[1] = ch->tri_x[2];
        ch->tri_y[1] = ch->tri_y[2];
        ch->tri_x[2] = x;
        ch->tri_y[2] = y;
        if (++ch->tri_count >= 3) {
            nv2d_tri_fill(s, ch, ch->tri_operation,
                          ch->tri_x[0], ch->tri_y[0], ch->tri_x[1],
                          ch->tri_y[1], ch->tri_x[2], ch->tri_y[2]);
        }
    } else if (method >= 0x100) {
        NV_TRACE(s, "nv15 TRI unhandled method 0x%03x param 0x%08x\n",
                 method, param);
    }
}

void nv2d_execute_imageblit(NV15State *s, gf_channel *ch, uint32_t method,
                            uint32_t param)
{
    if (method == 0x061) {
        ch->blit_color_key_enable = (nv_ramin_read32(s, param) & 0xFF) != 0x30;
    } else if (method == 0x0bf) {
        ch->blit_operation = param;
    } else if (method == 0x0c0) {
        ch->blit_syx = param;
    } else if (method == 0x0c1) {
        ch->blit_dyx = param;
    } else if (method == 0x0c2) {
        ch->blit_hw = param;
        copyarea(s, ch);
    }
}

void nv2d_execute_ifc(NV15State *s, gf_channel *ch, uint32_t method,
                      uint32_t param)
{
    if (method == 0x061) {
        ch->ifc_color_key_enable = (nv_ramin_read32(s, param) & 0xFF) != 0x30;
    } else if (method == 0x062) {
        ch->ifc_clip_enable = (nv_ramin_read32(s, param) & 0xFF) != 0x30;
    } else if (method == 0x0bf) {
        ch->ifc_operation = param;
    } else if (method == 0x0c0) {
        ch->ifc_color_fmt = param;
        update_color_bytes_ifc(s, ch);
        /* update_color_bytes() leaves ifc_color_bytes at 0 for an unknown
         * format; guard the divide so a bad SET_COLOR_FORMAT cannot crash. */
        ch->ifc_pixels_per_word =
            ch->ifc_color_bytes ? 4 / ch->ifc_color_bytes : 0;
    } else if (method == 0x0c1) {
        ch->ifc_x = 0;
        ch->ifc_y = 0;
        ch->ifc_ofs_x = param & 0xFFFF;
        ch->ifc_ofs_y = param >> 16;
        ch->ifc_draw_offset = ch->s2d_ofs_dst +
            ch->ifc_ofs_y * ch->s2d_pitch_dst +
            ch->ifc_ofs_x * ch->s2d_color_bytes;
        ch->ifc_redraw_offset = nv_dma_lin_lookup(s, ch->s2d_img_dst,
            ch->ifc_draw_offset) - s->disp_offset;
    } else if (method == 0x0c2) {
        ch->ifc_dst_width = param & 0xFFFF;
        ch->ifc_dst_height = param >> 16;
        ch->ifc_clip_x0 = 0;
        ch->ifc_clip_y0 = 0;
        ch->ifc_clip_x1 = ch->ifc_dst_width;
        ch->ifc_clip_y1 = ch->ifc_dst_height;
        if (ch->ifc_clip_enable) {
            int32_t clipx0 = ch->clip_x - ch->ifc_ofs_x;
            int32_t clipy0 = ch->clip_y - ch->ifc_ofs_y;
            int32_t clipx1 = clipx0 + ch->clip_width;
            int32_t clipy1 = clipy0 + ch->clip_height;
            ch->ifc_clip_x0 = MAX((int32_t)ch->ifc_clip_x0, clipx0);
            ch->ifc_clip_y0 = MAX((int32_t)ch->ifc_clip_y0, clipy0);
            ch->ifc_clip_x1 = MIN((int32_t)ch->ifc_clip_x1, clipx1);
            ch->ifc_clip_y1 = MIN((int32_t)ch->ifc_clip_y1, clipy1);
        }
    } else if (method == 0x0c3) {
        ch->ifc_src_width = param & 0xFFFF;
        ch->ifc_src_height = param >> 16;
    } else if (method >= 0x100 && method < 0x800) {
        ifc(s, ch, param);
    }
}

void nv2d_execute_surf2d(NV15State *s, gf_channel *ch, uint32_t method,
                         uint32_t param)
{
    ch->s2d_locked = true;
    if (method == 0x061) {
        ch->s2d_img_src = param;
    } else if (method == 0x062) {
        ch->s2d_img_dst = param;
    } else if (method == 0x0c0) {
        ch->s2d_color_fmt = param;
        uint32_t s2d_color_bytes_prev = ch->s2d_color_bytes;
        nv2d_update_color_bytes_s2d(s, ch);
        if (ch->s2d_color_bytes != s2d_color_bytes_prev &&
            (ch->s2d_color_bytes == 1 || s2d_color_bytes_prev == 1)) {
            update_color_bytes_ifc(s, ch);
            update_color_bytes_sifc(s, ch);
            update_color_bytes_tfc(s, ch);
        }
    } else if (method == 0x0c1) {
        ch->s2d_pitch_src = param & 0xFFFF;
        ch->s2d_pitch_dst = param >> 16;
    } else if (method == 0x0c2) {
        ch->s2d_ofs_src = param;
    } else if (method == 0x0c3) {
        ch->s2d_ofs_dst = param;
    }
}

void nv2d_execute_iifc(NV15State *s, gf_channel *ch, uint32_t method,
                       uint32_t param)
{
    if (method == 0x061) {
        ch->iifc_palette = param;
    } else if (method == 0x0f9) {
        ch->iifc_operation = param;
    } else if (method == 0x0fa) {
        ch->iifc_color_fmt = param;
        nv2d_update_color_bytes_iifc(s, ch);
    } else if (method == 0x0fb) {
        ch->iifc_bpp4 = param;
    } else if (method == 0x0fc) {
        ch->iifc_palette_ofs = param;
    } else if (method == 0x0fd) {
        ch->iifc_yx = param;
    } else if (method == 0x0fe) {
        ch->iifc_dhw = param;
    } else if (method == 0x0ff) {
        ch->iifc_shw = param;
        uint32_t width = ch->iifc_shw & 0xFFFF;
        uint32_t height = ch->iifc_shw >> 16;
        uint32_t wordCount =
            GF_ALIGN(width * height * (ch->iifc_bpp4 ? 4 : 8), 32) >> 5;
        g_free(ch->iifc_words);
        ch->iifc_words_ptr = 0;
        ch->iifc_words_left = wordCount;
        ch->iifc_words_count = wordCount;
        ch->iifc_words = g_new(uint32_t, wordCount);
    } else if (method >= 0x100 && method < 0x800) {
        if (ch->iifc_words != NULL && ch->iifc_words_left > 0) {
            ch->iifc_words[ch->iifc_words_ptr++] = param;
            ch->iifc_words_left--;
            if (!ch->iifc_words_left) {
                iifc(s, ch);
                g_free(ch->iifc_words);
                ch->iifc_words = NULL;
            }
        }
    }
}

void nv2d_execute_sifc(NV15State *s, gf_channel *ch, uint32_t method,
                       uint32_t param)
{
    if (method == 0x0bf) {
        ch->sifc_operation = param;
    } else if (method == 0x0c0) {
        ch->sifc_color_fmt = param;
        update_color_bytes_sifc(s, ch);
    } else if (method == 0x0c1) {
        ch->sifc_shw = param;
    } else if (method == 0x0c2) {
        ch->sifc_dxds = param;
    } else if (method == 0x0c3) {
        ch->sifc_dydt = param;
    } else if (method == 0x0c4) {
        ch->sifc_clip_yx = param;
    } else if (method == 0x0c5) {
        ch->sifc_clip_hw = param;
    } else if (method == 0x0c6) {
        ch->sifc_syx = param;
        uint32_t width = ch->sifc_shw & 0xFFFF;
        uint32_t height = ch->sifc_shw >> 16;
        uint32_t wordCount = GF_ALIGN(width * height * ch->sifc_color_bytes, 4) >> 2;
        g_free(ch->sifc_words);
        ch->sifc_words_ptr = 0;
        ch->sifc_words_left = wordCount;
        ch->sifc_words_count = wordCount;
        ch->sifc_words = g_new(uint32_t, wordCount);
    } else if (method >= 0x100 && method < 0x800) {
        if (ch->sifc_words != NULL && ch->sifc_words_left > 0) {
            ch->sifc_words[ch->sifc_words_ptr++] = param;
            ch->sifc_words_left--;
            if (!ch->sifc_words_left) {
                sifc(s, ch);
                g_free(ch->sifc_words);
                ch->sifc_words = NULL;
            }
        }
    }
}

void nv2d_execute_beta(NV15State *s, gf_channel *ch, uint32_t method,
                       uint32_t param)
{
    if (method == 0x0c0) {
        ch->beta = param;
    }
}

void nv2d_execute_tfc(NV15State *s, gf_channel *ch, uint32_t method,
                      uint32_t param)
{
    if (method == 0x061) {
        uint8_t cls8 = nv_ramin_read32(s, param);
        ch->tfc_swizzled = cls8 == 0x52 || cls8 == 0x9e;
    } else if (method == 0x0c0) {
        ch->tfc_color_fmt = param;
        update_color_bytes_tfc(s, ch);
    } else if (method == 0x0c1) {
        ch->tfc_yx = param;
    } else if (method == 0x0c2) {
        ch->tfc_hw = param;
        ch->tfc_upload = param == 0x01000100 && ch->tfc_yx == 0 &&
            ch->tfc_color_fmt == 4 && ch->s2d_color_fmt == 0xA &&
            ch->s2d_pitch_src == 0x0400 && ch->s2d_pitch_dst == 0x0400;
        if (ch->tfc_upload) {
            ch->tfc_upload_offset = ch->s2d_ofs_dst;
        } else {
            uint32_t width = ch->tfc_hw & 0xFFFF;
            uint32_t height = ch->tfc_hw >> 16;
            uint32_t wordCount =
                GF_ALIGN(width * height * ch->tfc_color_bytes, 4) >> 2;
            g_free(ch->tfc_words);
            ch->tfc_words_ptr = 0;
            ch->tfc_words_left = wordCount;
            ch->tfc_words = g_new(uint32_t, wordCount);
        }
    } else if (method == 0x0c3) {
        ch->tfc_clip_wx = param;
    } else if (method == 0x0c4) {
        ch->tfc_clip_hy = param;
    } else if (method >= 0x100 && method < 0x800) {
        if (ch->tfc_upload) {
            nv_dma_write32(s, ch->s2d_img_dst, ch->tfc_upload_offset, param);
            ch->tfc_upload_offset += 4;
        } else if (ch->tfc_words != NULL) {
            ch->tfc_words[ch->tfc_words_ptr++] = param;
            ch->tfc_words_left--;
            if (!ch->tfc_words_left) {
                tfc(s, ch);
                g_free(ch->tfc_words);
                ch->tfc_words = NULL;
            }
        }
    }
}

void nv2d_execute_sifm(NV15State *s, gf_channel *ch, uint32_t cls,
                       uint32_t method, uint32_t param)
{
    if (method == 0x061) {
        ch->sifm_src = param;
    } else if (method == 0x066) {
        uint8_t surf_cls8 = nv_ramin_read32(s, param);
        bool swizzled = surf_cls8 == 0x52 || surf_cls8 == 0x9e;
        if (cls == 0x0389) {
            ch->sifm_swizzled_0389 = swizzled;
        } else {
            ch->sifm_swizzled = swizzled;
        }
    } else if (method == 0x0c0) {
        ch->sifm_color_fmt = param;
        if (ch->sifm_color_fmt == 8) {          /* ??? */
            ch->sifm_color_bytes = 1;
        } else if (ch->sifm_color_fmt == 1 ||   /* A1R5G5B5 */
                   ch->sifm_color_fmt == 2 ||   /* X1R5G5B5 */
                   ch->sifm_color_fmt == 7) {   /* R5G6B5 */
            ch->sifm_color_bytes = 2;
        } else if (ch->sifm_color_fmt == 3 ||   /* A8R8G8B8 */
                   ch->sifm_color_fmt == 4) {   /* X8R8G8B8 */
            ch->sifm_color_bytes = 4;
        } else {
            qemu_log_mask(LOG_GUEST_ERROR,
                "nv15: unknown sifm color format 0x%02x\n",
                ch->sifm_color_fmt);
        }
    } else if (method == 0x0c1) {
        ch->sifm_operation = param;
    } else if (method == 0x0c4) {
        ch->sifm_dyx = param;
    } else if (method == 0x0c5) {
        ch->sifm_dhw = param;
    } else if (method == 0x0c6) {
        ch->sifm_dudx = param;
    } else if (method == 0x0c7) {
        ch->sifm_dvdy = param;
    } else if (method == 0x100) {
        ch->sifm_shw = param;
    } else if (method == 0x101) {
        ch->sifm_sfmt = param;
    } else if (method == 0x102) {
        ch->sifm_sofs = param;
    } else if (method == 0x103) {
        ch->sifm_syx = param;
        sifm(s, ch, cls == 0x0389 ? ch->sifm_swizzled_0389 : ch->sifm_swizzled);
    }
}
