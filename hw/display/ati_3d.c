/*
 * QEMU ATI RAGE 128 3D / scale engine
 *
 * Implements the parts of the RAGE 128 (Pro) rasterisation pipeline that the
 * Windows ati2draa display driver and Direct3D/DirectDraw clients drive
 * through the CCE ring:
 *
 *   - the SCALE / TRANS_SCALE packets (opcodes 0x96 / 0x97), a filtered
 *     stretch-blit of a source bitmap.  ati2draa's accelerated
 *     DrvGradientFill renders a window-caption gradient by stretching a
 *     tiny two-colour texture across the caption, so getting this right is
 *     what makes title-bar gradients (and DirectDraw stretches) appear;
 *
 *   - the 3D_RNDR_GEN_PRIM / 3D_RNDR_GEN_INDX_PRIM packets (opcodes 0x25 /
 *     0x23), which draw flexible-format vertices as points, lines or
 *     Gouraud-shaded triangles.  This is the groundwork for hardware 3D:
 *     the vertex parser and the triangle rasteriser here are written to be
 *     extended with texture mapping, depth and alpha blending later.
 *
 * Field layouts follow the RAGE 128 Software Development Manual, appendix F
 * (F.13 SCALE, F.24 3D_RNDR_GEN_PRIM) and the RAGE 128 Pro Register
 * Reference.  This is a clean-room model; no Windows source is used.
 *
 * This work is licensed under the GNU GPL license version 2 or later.
 */

#include "qemu/osdep.h"
#include <math.h>
#include "qemu/log.h"
#include "ati_int.h"
#include "ati_regs.h"

/*
 * A drawable surface in card memory: a byte pointer to the first pixel, the
 * row stride in bytes, and a pixel format expressed as an ATI datatype code
 * (the low nibble of DP_DATATYPE / a GUI_MASTER_CNTL DST_TYPE field).
 */
typedef struct ATISurface {
    uint8_t *bits;
    const uint8_t *cbits;   /* read view (may equal bits) */
    int stride;             /* bytes per row */
    unsigned datatype;      /* 2=8bpp 3=aRGB1555 4=RGB565 5=RGB888 6=aRGB8888 */
    unsigned bypp;          /* bytes per pixel */
    uint32_t vram_size;
} ATISurface;

static unsigned ati_datatype_bypp(unsigned dt)
{
    switch (dt & 0xf) {
    case 2: return 1;       /* 8 bpp */
    case 3: return 2;       /* aRGB1555 */
    case 4: return 2;       /* RGB565 */
    case 5: return 3;       /* RGB888 */
    case 6: return 4;       /* aRGB8888 */
    default: return 0;
    }
}

/* Decode a stored pixel to 0xAARRGGBB (alpha 0xff when the format has none). */
static uint32_t ati_pixel_to_argb(const ATISurface *s, const uint8_t *p)
{
    switch (s->datatype & 0xf) {
    case 2: { /* 8 bpp: treated as grey intensity (palette is not tracked
                 by the blitter path either) */
        uint32_t v = p[0];
        return 0xff000000u | (v << 16) | (v << 8) | v;
    }
    case 3: { /* aRGB1555 */
        uint32_t v = lduw_le_p(p);
        uint32_t a = (v & 0x8000) ? 0xff : 0xff; /* opaque unless keyed */
        uint32_t r = (v >> 10) & 0x1f, g = (v >> 5) & 0x1f, b = v & 0x1f;
        r = (r << 3) | (r >> 2); g = (g << 3) | (g >> 2); b = (b << 3) | (b >> 2);
        return (a << 24) | (r << 16) | (g << 8) | b;
    }
    case 4: { /* RGB565 */
        uint32_t v = lduw_le_p(p);
        uint32_t r = (v >> 11) & 0x1f, g = (v >> 5) & 0x3f, b = v & 0x1f;
        r = (r << 3) | (r >> 2); g = (g << 2) | (g >> 4); b = (b << 3) | (b >> 2);
        return 0xff000000u | (r << 16) | (g << 8) | b;
    }
    case 5: /* RGB888 */
        return 0xff000000u | (p[2] << 16) | (p[1] << 8) | p[0];
    case 6: /* aRGB8888 */
        return ldl_le_p(p);
    default:
        return 0;
    }
}

/* Encode 0xAARRGGBB into a stored pixel of the surface's format. */
static void ati_argb_to_pixel(const ATISurface *s, uint8_t *p, uint32_t argb)
{
    uint32_t r = (argb >> 16) & 0xff, g = (argb >> 8) & 0xff, b = argb & 0xff;

    switch (s->datatype & 0xf) {
    case 2:
        p[0] = (r * 77 + g * 150 + b * 29) >> 8; /* luma */
        break;
    case 3:
        stw_le_p(p, 0x8000u | ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3));
        break;
    case 4:
        stw_le_p(p, ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
        break;
    case 5:
        p[0] = b; p[1] = g; p[2] = r;
        break;
    case 6:
        stl_le_p(p, argb);
        break;
    }
}

/* Bounds-checked pixel access within the VRAM window. */
static const uint8_t *ati_surf_ro(const ATISurface *s, int x, int y)
{
    long off = (long)y * s->stride + (long)x * s->bypp;

    if (off < 0 || (uint32_t)(off + s->bypp) > s->vram_size) {
        return NULL;
    }
    return s->cbits + off;
}

static uint8_t *ati_surf_wo(const ATISurface *s, int x, int y)
{
    long off = (long)y * s->stride + (long)x * s->bypp;

    if (off < 0 || (uint32_t)(off + s->bypp) > s->vram_size) {
        return NULL;
    }
    return s->bits + off;
}

/* Clamped nearest source texel as ARGB. */
static uint32_t ati_sample_nn(const ATISurface *s, int sw, int sh,
                              int x, int y)
{
    const uint8_t *p;

    if (x < 0) {
        x = 0;
    } else if (x >= sw) {
        x = sw - 1;
    }
    if (y < 0) {
        y = 0;
    } else if (y >= sh) {
        y = sh - 1;
    }
    p = ati_surf_ro(s, x, y);
    return p ? ati_pixel_to_argb(s, p) : 0;
}

/* Bilinear source sample at fixed-point (16.16) source coordinates. */
static uint32_t ati_sample_bilinear(const ATISurface *s, int sw, int sh,
                                    int64_t fx, int64_t fy)
{
    int x0 = (int)(fx >> 16), y0 = (int)(fy >> 16);
    uint32_t tx = (uint32_t)(fx >> 8) & 0xff;
    uint32_t ty = (uint32_t)(fy >> 8) & 0xff;
    uint32_t c00 = ati_sample_nn(s, sw, sh, x0, y0);
    uint32_t c10 = ati_sample_nn(s, sw, sh, x0 + 1, y0);
    uint32_t c01 = ati_sample_nn(s, sw, sh, x0, y0 + 1);
    uint32_t c11 = ati_sample_nn(s, sw, sh, x0 + 1, y0 + 1);
    uint32_t out = 0;
    int sh8;

    for (sh8 = 0; sh8 < 32; sh8 += 8) {
        uint32_t a = (c00 >> sh8) & 0xff, b = (c10 >> sh8) & 0xff;
        uint32_t c = (c01 >> sh8) & 0xff, d = (c11 >> sh8) & 0xff;
        uint32_t top = a * (256 - tx) + b * tx;
        uint32_t bot = c * (256 - tx) + d * tx;
        uint32_t v = (top * (256 - ty) + bot * ty) >> 16;
        out |= (v & 0xff) << sh8;
    }
    return out;
}

/* Build the destination surface from the register file (set by the GMC
 * prefix that precedes the packet body). */
static void ati_dst_surface(ATIVGAState *s, unsigned dst_type, ATISurface *d)
{
    uint32_t off = ati_mc_vram_offset(s, s->regs.dst_offset);

    d->datatype = dst_type;
    d->bypp = ati_datatype_bypp(dst_type);
    d->bits = s->vga.vram_ptr + off;
    d->cbits = d->bits;
    /*
     * pitch is in 8-pixel units, so stride = pitch * 8 * bypp -- except at
     * 24bpp, where the driver folds the *3 into the pitch register (see
     * setup_2d_blt_ctx / r128 XAA), so use *8 to avoid triple-counting.
     */
    d->stride = s->regs.dst_pitch * (d->bypp == 3 ? 8 : d->bypp * 8);
    /* Bounds are checked relative to the surface base, so store the number
     * of bytes remaining from dst_offset to the end of VRAM. */
    d->vram_size = off < s->vga.vram_size ? s->vga.vram_size - off : 0;
}

static void ati_set_dirty(ATIVGAState *s, uint32_t off, uint32_t len)
{
    if (off < s->vga.vram_size) {
        if (off + len > s->vga.vram_size) {
            len = s->vga.vram_size - off;
        }
        memory_region_set_dirty(&s->vga.vram, off, len);
    }
}

/*
 * SCALE / TRANS_SCALE (opcodes 0x96 / 0x97).
 *
 * db[] is the 11-dword DATA_BLOCK that follows the GUI_MASTER_CNTL prefix
 * (RAGE 128 SDM table F-17):
 *
 *   0 MISC_3D_STATE   4 SCALE_OFFSET     8 SCALE_Y_INC
 *   1 TEX_CNTL        5 SCALE_PITCH      9 DST_X<<16 | DST_Y
 *   2 TEX_COMB_CTL    6 (reserved=0)    10 DST_H<<16 | DST_W
 *   3 SCALE_DATATYPE  7 SCALE_X_INC
 *
 * SCALE_X_INC / SCALE_Y_INC are the source-to-destination ratio SRC/DST in
 * a fixed-point form with a 4-bit integer part at [19:16] and a 12-bit
 * fraction at [15:4]; i.e. the step in source pixels per destination pixel.
 * gmc DST_TYPE ([11:8]) gives the destination pixel format.
 */
void ati_scale_blt(ATIVGAState *s, uint32_t gmc, const uint32_t db[11],
                   bool trans)
{
    ATISurface src, dst;
    uint32_t dst_type = (gmc >> 8) & 0xf;
    int dst_x = (db[9] >> 16) & 0x3fff;
    int dst_y = db[9] & 0x3fff;
    int dst_w = db[10] & 0x3fff;
    int dst_h = (db[10] >> 16) & 0x3fff;
    /* step in source pixels per dest pixel, as 16.16 fixed-point */
    int64_t xinc = (int64_t)((db[7] >> 4) & 0xffff) << 4; /* (>>4)/4096 -> .16 */
    int64_t yinc = (int64_t)((db[8] >> 4) & 0xffff) << 4;
    unsigned dither = (db[0] >> 1) & 1;
    int64_t sx0 = 0, sy0;
    int sw, sh, y;
    uint32_t key = 0, key_msk = 0xffffff;
    bool have_key = false;

    ati_dst_surface(s, dst_type, &dst);
    if (!dst.bypp || !dst.stride || dst_w <= 0 || dst_h <= 0) {
        return;
    }

    src.datatype = (db[3] & 0xf) ? db[3] : dst_type;   /* SCALE_DATATYPE */
    src.bypp = ati_datatype_bypp(src.datatype);
    if (!src.bypp) {
        return;
    }
    src.cbits = s->vga.vram_ptr + (db[4] & 0x3ffffff); /* SCALE_OFFSET bytes */
    src.bits = (uint8_t *)src.cbits;
    src.stride = db[5] * (src.bypp * 8);               /* SCALE_PITCH, 8-px */
    if (!src.stride) {
        src.stride = src.bypp; /* degenerate 1-pixel-wide source */
    }
    src.vram_size = s->vga.vram_size;
    /* Give the bounds checks a base at the texture origin. */
    src.vram_size = s->vga.vram_size - (uint32_t)(src.cbits - s->vga.vram_ptr);

    /* Source extent: enough texels to cover the scaled destination. */
    sw = (int)(((xinc * dst_w) >> 16) + 2);
    sh = (int)(((yinc * dst_h) >> 16) + 2);
    if (sw < 1) {
        sw = 1;
    }
    if (sh < 1) {
        sh = 1;
    }

    if (trans) {
        unsigned fn = s->regs.clr_cmp_cntl & 7;
        if (fn) {
            have_key = true;
            key = s->regs.clr_cmp_clr_src & 0xffffff;
            key_msk = s->regs.clr_cmp_msk ? s->regs.clr_cmp_msk : 0xffffff;
        }
    }

    sy0 = 0;
    for (y = 0; y < dst_h; y++) {
        int64_t sy = sy0 + yinc * y;
        int x;
        for (x = 0; x < dst_w; x++) {
            int64_t sx = sx0 + xinc * x;
            uint32_t argb;
            uint8_t *dp;

            if (xinc == (1 << 16) && yinc == (1 << 16)) {
                argb = ati_sample_nn(&src, sw, sh, (int)(sx >> 16),
                                     (int)(sy >> 16));
            } else {
                argb = ati_sample_bilinear(&src, sw, sh, sx, sy);
            }
            (void)dither;
            if (have_key &&
                (argb & key_msk & 0xffffff) == (key & key_msk)) {
                continue;
            }
            dp = ati_surf_wo(&dst, dst_x + x, dst_y + y);
            if (dp) {
                ati_argb_to_pixel(&dst, dp, argb);
            }
        }
    }
    ati_set_dirty(s, ati_mc_vram_offset(s, s->regs.dst_offset) +
                     (uint32_t)dst_y * dst.stride,
                  (uint32_t)dst_h * dst.stride);
}

/* --------------------------------------------------------------------- */
/* 3D primitive rendering                                                 */
/* --------------------------------------------------------------------- */

typedef struct ATIVertex {
    float x, y, z;
    uint32_t argb;          /* 0xAARRGGBB */
} ATIVertex;

/* VC_FORMAT flag bits (RAGE 128 SDM table F-43). */
#define VC_FRMT_RHW          BIT(0)
#define VC_FRMT_DIFFUSE_BGR  BIT(1)
#define VC_FRMT_DIFFUSE_A    BIT(2)
#define VC_FRMT_DIFFUSE_ARGB BIT(3)
#define VC_FRMT_SPEC_BGR     BIT(4)
#define VC_FRMT_SPEC_F       BIT(5)
#define VC_FRMT_SPEC_FRGB    BIT(6)
#define VC_FRMT_S_T          BIT(7)
#define VC_FRMT_S2_T2        BIT(8)
#define VC_FRMT_RHW2         BIT(9)

static float ati_dword_to_float(uint32_t v)
{
    union { uint32_t u; float f; } c = { .u = v };
    return c.f;
}

static uint8_t ati_unorm8(float f)
{
    int v;

    if (!(f > 0.0f)) {  /* also catches NaN */
        return 0;
    }
    v = (int)(f * 255.0f + 0.5f);
    return v > 255 ? 255 : v;
}

/*
 * Read one flexible vertex.  X, Y, Z are always present; the remaining
 * fields are present according to VC_FORMAT.  Only position and diffuse
 * colour are retained (the rest are parsed to keep the stream aligned).
 * next() supplies successive dwords from either the ring or a vertex buffer.
 */
static void ati_read_vertex(uint32_t (*next)(void *), void *ctx,
                            uint32_t fmt, ATIVertex *v)
{
    v->x = ati_dword_to_float(next(ctx));
    v->y = ati_dword_to_float(next(ctx));
    v->z = ati_dword_to_float(next(ctx));
    v->argb = 0xffffffff;

    if (fmt & VC_FRMT_RHW) {
        next(ctx);
    }
    if (fmt & VC_FRMT_DIFFUSE_BGR) {
        float b = ati_dword_to_float(next(ctx));
        float g = ati_dword_to_float(next(ctx));
        float r = ati_dword_to_float(next(ctx));
        uint32_t a = 0xff;
        if (fmt & VC_FRMT_DIFFUSE_A) {
            a = ati_unorm8(ati_dword_to_float(next(ctx)));
        }
        v->argb = (a << 24) | (ati_unorm8(r) << 16) |
                  (ati_unorm8(g) << 8) | ati_unorm8(b);
    } else if (fmt & VC_FRMT_DIFFUSE_A) {
        uint32_t a = ati_unorm8(ati_dword_to_float(next(ctx)));
        v->argb = (a << 24) | 0xffffff;
    }
    if (fmt & VC_FRMT_DIFFUSE_ARGB) {
        v->argb = next(ctx);
    }
    if (fmt & VC_FRMT_SPEC_BGR) {
        next(ctx); next(ctx); next(ctx);
    }
    if (fmt & VC_FRMT_SPEC_F) {
        next(ctx);
    }
    if (fmt & VC_FRMT_SPEC_FRGB) {
        next(ctx);
    }
    if (fmt & VC_FRMT_S_T) {
        next(ctx); next(ctx);
    }
    if (fmt & VC_FRMT_S2_T2) {
        next(ctx); next(ctx);
    }
    if (fmt & VC_FRMT_RHW2) {
        next(ctx);
    }
}

/* Count the dwords one vertex occupies for the given VC_FORMAT. */
static unsigned ati_vertex_dwords(uint32_t fmt)
{
    unsigned n = 3;                                 /* x, y, z */

    if (fmt & VC_FRMT_RHW) {
        n += 1;
    }
    if (fmt & VC_FRMT_DIFFUSE_BGR) {
        n += 3;
    }
    if (fmt & VC_FRMT_DIFFUSE_A) {
        n += 1;
    }
    if (fmt & VC_FRMT_DIFFUSE_ARGB) {
        n += 1;
    }
    if (fmt & VC_FRMT_SPEC_BGR) {
        n += 3;
    }
    if (fmt & VC_FRMT_SPEC_F) {
        n += 1;
    }
    if (fmt & VC_FRMT_SPEC_FRGB) {
        n += 1;
    }
    if (fmt & VC_FRMT_S_T) {
        n += 2;
    }
    if (fmt & VC_FRMT_S2_T2) {
        n += 2;
    }
    if (fmt & VC_FRMT_RHW2) {
        n += 1;
    }
    return n;
}

/* Gouraud-blend the three vertex colours by barycentric weights (w* in .16). */
static uint32_t ati_gouraud(const ATIVertex *a, const ATIVertex *b,
                            const ATIVertex *c, int64_t wa, int64_t wb,
                            int64_t wc, int64_t area)
{
    uint32_t out = 0;
    int sh;

    for (sh = 0; sh < 32; sh += 8) {
        int64_t ca = (a->argb >> sh) & 0xff;
        int64_t cb = (b->argb >> sh) & 0xff;
        int64_t cc = (c->argb >> sh) & 0xff;
        int64_t v = (ca * wa + cb * wb + cc * wc) / area;
        if (v < 0) {
            v = 0;
        } else if (v > 255) {
            v = 255;
        }
        out |= (uint32_t)v << sh;
    }
    return out;
}

/* Rasterise one Gouraud triangle into the destination surface, clipped to
 * the scissor rectangle.  Integer edge functions; top-left fill rule. */
static void ati_raster_tri(ATIVGAState *s, ATISurface *d,
                           const ATIVertex *v0, const ATIVertex *v1,
                           const ATIVertex *v2, int scx0, int scy0,
                           int scx1, int scy1)
{
    int x0 = (int)lrintf(v0->x), y0 = (int)lrintf(v0->y);
    int x1 = (int)lrintf(v1->x), y1 = (int)lrintf(v1->y);
    int x2 = (int)lrintf(v2->x), y2 = (int)lrintf(v2->y);
    int64_t area = (int64_t)(x1 - x0) * (y2 - y0) -
                   (int64_t)(x2 - x0) * (y1 - y0);
    int minx, miny, maxx, maxy, x, y;
    int64_t dirty_lo = -1, dirty_hi = 0;

    if (area == 0) {
        return;                 /* degenerate */
    }
    if (area < 0) {             /* enforce counter-clockwise winding */
        const ATIVertex *t = v1; v1 = v2; v2 = t;
        x1 = (int)lrintf(v1->x); y1 = (int)lrintf(v1->y);
        x2 = (int)lrintf(v2->x); y2 = (int)lrintf(v2->y);
        area = -area;
    }

    minx = MIN(x0, MIN(x1, x2));
    maxx = MAX(x0, MAX(x1, x2));
    miny = MIN(y0, MIN(y1, y2));
    maxy = MAX(y0, MAX(y1, y2));
    minx = MAX(minx, scx0);
    miny = MAX(miny, scy0);
    maxx = MIN(maxx, scx1);
    maxy = MIN(maxy, scy1);

    for (y = miny; y <= maxy; y++) {
        for (x = minx; x <= maxx; x++) {
            /* barycentric weights = signed sub-triangle areas */
            int64_t w0 = (int64_t)(x1 - x) * (y2 - y) -
                         (int64_t)(x2 - x) * (y1 - y);
            int64_t w1 = (int64_t)(x2 - x) * (y0 - y) -
                         (int64_t)(x0 - x) * (y2 - y);
            int64_t w2 = (int64_t)(x0 - x) * (y1 - y) -
                         (int64_t)(x1 - x) * (y0 - y);
            uint8_t *dp;
            uint32_t argb;

            if ((w0 | w1 | w2) < 0) {
                continue;       /* outside: some weight negative */
            }
            dp = ati_surf_wo(d, x, y);
            if (!dp) {
                continue;
            }
            argb = ati_gouraud(v0, v1, v2, w0, w1, w2, area);
            ati_argb_to_pixel(d, dp, argb);
            {
                int64_t off = (int64_t)y * d->stride + (int64_t)x * d->bypp;
                if (dirty_lo < 0 || off < dirty_lo) {
                    dirty_lo = off;
                }
                if (off + d->bypp > dirty_hi) {
                    dirty_hi = off + d->bypp;
                }
            }
        }
    }
    if (dirty_lo >= 0) {
        uint32_t base = ati_mc_vram_offset(s, s->regs.dst_offset);
        ati_set_dirty(s, base + (uint32_t)dirty_lo,
                      (uint32_t)(dirty_hi - dirty_lo));
    }
}

/*
 * 2D setup-engine Gouraud rectangle fill (SETUP_CNTL COLOR_FCN = Gouraud).
 *
 * The Windows ati2draa driver draws window-caption gradients not through the
 * CCE 3D packets above but through the 2D setup engine: it programs a
 * per-channel colour DDA (0x1a40-0x1a60) and then issues an ordinary solid
 * rectangle paint (DST_X_Y + DST_HEIGHT_WIDTH).  With SCALE_3D_CNTL enabled
 * and SETUP_CNTL COLOR_FCN = Gouraud, the engine replaces the solid brush with
 * the interpolated colour, giving the classic dark->light caption ramp.
 *
 * Colour plane per channel c (16.16, signed):
 *   colour_c(x,y) = val_c + dx_c*(x - dst_x) + dy_c*(y - dst_y)
 * val_c is the colour at the primitive origin (the rectangle's top-left, which
 * is the gradient's dark endpoint); for a horizontal gradient dy_c = 0.
 *
 * Returns true if it consumed the paint (so the caller skips the solid fill),
 * false if this is not an armed Gouraud fill and normal 2D handling applies.
 */
bool ati_setup_gouraud_fill(ATIVGAState *s)
{
    ATISurface d;
    int x0, y0, w, h, scx0, scy0, scx1, scy1, x, y, xstep;

    if (!s->regs.su_gouraud_armed) {
        return false;
    }

    /*
     * The gate must still be Gouraud (SCALE_3D_CNTL[7:6] setup-block enable and
     * SETUP_CNTL[5:3] COLOR_FCN == 4); if not, the burst is over -- release the
     * plane and let the blit run normally.
     */
    if (((s->regs.scale_3d_cntl >> 6) & 3) == 0 ||
        ((s->regs.setup_cntl >> 3) & 7) != 4) {
        s->regs.su_gouraud_armed = false;
        s->regs.su_gouraud_rect_valid = false;
        return false;
    }

    x0 = ati_sext14(s->regs.dst_x);
    y0 = ati_sext14(s->regs.dst_y);
    w = s->regs.dst_width;
    h = s->regs.dst_height;
    /*
     * GDI emits one gradient as several scissored blits that share the colour
     * plane and destination rectangle but tile different clip rectangles.  The
     * driver arms the plane only once, so keep it armed for every blit whose
     * destination rectangle matches; the first blit records it.  A blit with a
     * different rectangle ends the burst -- release the plane and run it
     * normally (this is what stops the following window-content blits, which
     * would otherwise copy garbage into the caption).
     */
    if (!s->regs.su_gouraud_rect_valid) {
        s->regs.su_gouraud_rect[0] = x0;
        s->regs.su_gouraud_rect[1] = y0;
        s->regs.su_gouraud_rect[2] = w;
        s->regs.su_gouraud_rect[3] = h;
        s->regs.su_gouraud_rect_valid = true;
    } else if (s->regs.su_gouraud_rect[0] != x0 ||
               s->regs.su_gouraud_rect[1] != y0 ||
               s->regs.su_gouraud_rect[2] != w ||
               s->regs.su_gouraud_rect[3] != h) {
        s->regs.su_gouraud_armed = false;
        s->regs.su_gouraud_rect_valid = false;
        return false;
    }

    ati_dst_surface(s, s->regs.dp_datatype & 0xf, &d);
    if (!d.bypp || !d.stride) {
        return true;    /* armed but undrawable: still count as consumed */
    }
    /*
     * At 24bpp the setup engine advances its colour DDA once per byte (three
     * steps per pixel), so the driver programs dC/dx at a third of the
     * per-pixel slope (measured: the 24bpp dx is exactly 1/3 of the 32bpp dx
     * for the same caption).  Scale the per-pixel x displacement by the byte
     * count to recover the intended slope; 16/32bpp step once per pixel.
     */
    xstep = (d.bypp == 3) ? 3 : 1;

    if (w <= 0 || h <= 0) {
        return true;
    }

    /* Scissor rectangle; SC_* right/bottom are inclusive. */
    scx0 = s->regs.sc_left;
    scy0 = s->regs.sc_top;
    scx1 = s->regs.sc_right;
    scy1 = s->regs.sc_bottom;

    for (y = y0; y < y0 + h; y++) {
        if (y < scy0 || y > scy1) {
            continue;
        }
        for (x = x0; x < x0 + w; x++) {
            uint32_t argb = 0xff000000u;
            uint8_t *dp;
            int c;

            if (x < scx0 || x > scx1) {
                continue;
            }
            for (c = 0; c < 3; c++) {
                int64_t v = (int64_t)s->regs.su_color_val[c] +
                            (int64_t)s->regs.su_color_dx[c] * (x - x0) * xstep +
                            (int64_t)s->regs.su_color_dy[c] * (y - y0);
                int iv = (int)(v >> 16);

                if (iv < 0) {
                    iv = 0;
                } else if (iv > 255) {
                    iv = 255;
                }
                argb |= (uint32_t)iv << (16 - c * 8); /* c: 0=R,1=G,2=B */
            }
            dp = ati_surf_wo(&d, x, y);
            if (dp) {
                ati_argb_to_pixel(&d, dp, argb);
            }
        }
    }
    ati_set_dirty(s, ati_mc_vram_offset(s, s->regs.dst_offset) +
                     (uint32_t)y0 * d.stride,
                  (uint32_t)h * d.stride);
    return true;
}

/* Context for pulling vertex dwords from the CCE ring. */
static uint32_t ati_next_ring(void *ctx)
{
    return ati_cce_next((ATICCEReader *)ctx);
}

/* Context for pulling vertex dwords from a card-memory vertex buffer. */
typedef struct ATIVBufCtx {
    ATIVGAState *s;
    uint32_t addr;          /* current card (GART) address */
} ATIVBufCtx;

static uint32_t ati_next_vbuf(void *ctx)
{
    ATIVBufCtx *c = ctx;
    uint32_t v = ati_cce_vm_dword(c->s, c->addr);

    c->addr += 4;
    return v;
}

/*
 * 3D_RNDR_GEN_PRIM (0x25) and 3D_RNDR_GEN_INDX_PRIM (0x23).
 *
 * Ring-walk form (indexed == false):
 *   [VC_FORMAT] [VC_CNTL] [FTLVERTEX_1] ... [FTLVERTEX_n]
 * Indexed form (indexed == true):
 *   [PM4_VC_VLOFF] [PM4_VC_SIZE] [VC_FORMAT] [VC_CNTL] [indices...]
 * with the vertex data blocks read from the VLOFF vertex buffer and, for the
 * indexed walk, addressed through 16-bit indices packed two per dword.
 *
 * VC_CNTL: [3:0] primitive type, [5:4] walk mode, [31:16] vertex count.
 */
bool ati_3d_gen_prim(ATIVGAState *s, ATICCEReader *rd, bool indexed)
{
    uint32_t vloff = 0, fmt, cntl;
    unsigned prim, walk, nvert, vstride, i;
    ATISurface dst;
    int scx0, scy0, scx1, scy1;
    ATIVertex fan0, prev1, prev2;
    bool have_prev = false;

    if (indexed) {
        if (!ati_cce_has(rd, 4)) {
            return false;
        }
        vloff = ati_cce_next(rd);
        ati_cce_next(rd);               /* PM4_VC_SIZE (buffer vertex count) */
        fmt = ati_cce_next(rd);
        cntl = ati_cce_next(rd);
    } else {
        if (!ati_cce_has(rd, 2)) {
            return false;
        }
        fmt = ati_cce_next(rd);
        cntl = ati_cce_next(rd);
    }

    prim = cntl & 0xf;
    walk = (cntl >> 4) & 0x3;
    nvert = (cntl >> 16) & 0xffff;
    vstride = ati_vertex_dwords(fmt);
    if (!vstride || !nvert) {
        return true;
    }

    /* Destination: current 2D destination surface + scissor. */
    ati_dst_surface(s, s->regs.dp_datatype & 0xf, &dst);
    if (!dst.bypp || !dst.stride) {
        return true;
    }
    scx0 = s->regs.sc_left;
    scy0 = s->regs.sc_top;
    scx1 = s->regs.sc_right;
    scy1 = s->regs.sc_bottom;
    if (scx1 <= scx0 || scy1 <= scy0) {
        scx0 = 0;
        scy0 = 0;
        scx1 = s->vga.last_scr_width ? s->vga.last_scr_width - 1 : 0x3fff;
        scy1 = s->vga.last_scr_height ? s->vga.last_scr_height - 1 : 0x3fff;
    }

    /*
     * Read vertices in stream order and assemble primitives on the fly.
     * For the ring walk the vertices follow inline; for the indexed/list
     * walks they come from the VLOFF buffer.  Only PRIM_WALK_RING carries
     * vertices in the packet; the vertex-walker walks (1, 2) read from the
     * buffer, and the indexed walk additionally needs the index array,
     * which is not emitted by the Windows 2D driver.  Support the ring
     * walk fully; for the buffer walks, stream vertices sequentially.
     */
    {
        ATIVBufCtx vb = { s, vloff };
        uint32_t (*next)(void *);
        void *ctx;

        if (indexed && walk != 3) {
            next = ati_next_vbuf;
            ctx = &vb;
        } else {
            next = ati_next_ring;
            ctx = rd;
            if (!ati_cce_has(rd, vstride * nvert) && !indexed) {
                /* truncated packet: clamp to what is present */
                nvert = rd->count > rd->pos ?
                        (rd->count - rd->pos) / vstride : 0;
            }
        }

        for (i = 0; i < nvert; i++) {
            ATIVertex v;

            ati_read_vertex(next, ctx, fmt, &v);

            switch (prim) {
            case 1: /* points */
                ati_raster_tri(s, &dst, &v, &v, &v, scx0, scy0, scx1, scy1);
                break;
            case 2: /* independent lines: pair up */
            case 3: /* polyline */
                if (!have_prev) {
                    prev1 = v;
                    have_prev = true;
                } else {
                    /* draw a thin degenerate triangle as the line */
                    ati_raster_tri(s, &dst, &prev1, &v, &v,
                                   scx0, scy0, scx1, scy1);
                    if (prim == 2) {
                        have_prev = false;
                    } else {
                        prev1 = v;
                    }
                }
                break;
            case 4: /* independent triangles */
                if (i % 3 == 0) {
                    fan0 = v;
                } else if (i % 3 == 1) {
                    prev1 = v;
                } else {
                    ati_raster_tri(s, &dst, &fan0, &prev1, &v,
                                   scx0, scy0, scx1, scy1);
                }
                break;
            case 5: /* triangle fan: v0, then (v0, vi-1, vi) */
                if (i == 0) {
                    fan0 = v;
                } else if (i == 1) {
                    prev1 = v;
                } else {
                    ati_raster_tri(s, &dst, &fan0, &prev1, &v,
                                   scx0, scy0, scx1, scy1);
                    prev1 = v;
                }
                break;
            case 6: /* triangle strip */
                if (i == 0) {
                    prev2 = v;
                } else if (i == 1) {
                    prev1 = v;
                } else {
                    if (i & 1) {
                        ati_raster_tri(s, &dst, &prev1, &prev2, &v,
                                       scx0, scy0, scx1, scy1);
                    } else {
                        ati_raster_tri(s, &dst, &prev2, &prev1, &v,
                                       scx0, scy0, scx1, scy1);
                    }
                    prev2 = prev1;
                    prev1 = v;
                }
                break;
            default:
                break;
            }
        }
    }
    return true;
}
