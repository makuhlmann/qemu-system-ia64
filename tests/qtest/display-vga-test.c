/*
 * QTest testcase for vga cards
 *
 * Copyright (c) 2014 Red Hat, Inc
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include <glib/gstdio.h>
#include "libqtest.h"

#define VBE_DISPI_IOPORT_INDEX 0x1ce
#define VBE_DISPI_IOPORT_DATA  0x1cf
#define VBE_DISPI_INDEX_ID     0
#define VBE_DISPI_INDEX_XRES   1
#define VBE_DISPI_INDEX_YRES   2
#define VBE_DISPI_INDEX_BPP    3
#define VBE_DISPI_INDEX_ENABLE 4
#define VBE_DISPI_INDEX_VIRT_WIDTH 6
#define VBE_DISPI_ID5          0xb0c5
#define VBE_DISPI_ENABLED      0x01
#define VBE_DISPI_LFB_ENABLED  0x40
#define VBE_DISPI_NOCLEARMEM   0x80
#define IA64_LEGACY_IO_BASE    0x00000ffffc000000ULL
#define IA64_ATI_FB_BASE       0x00000000f0000000ULL
#define IA64_ATI_MMIO_BASE     0x00000000f5000000ULL
#define IA64_VGA_LEGACY_BASE   0x00000000000a0000ULL
#define ATI_MM_INDEX           0x0000
#define ATI_MM_DATA            0x0004
#define ATI_VRAM_SIZE          (16U * 1024U * 1024U)
#define ATI_CRTC_GEN_CNTL      0x0050
#define ATI_CRTC_H_TOTAL_DISP  0x0200
#define ATI_CRTC_V_TOTAL_DISP  0x0208
#define ATI_CRTC_OFFSET        0x0224
#define ATI_CRTC_PITCH         0x022c
#define ATI_CRTC_EXT_DISP_EN   0x01000000
#define ATI_CRTC_EN            0x02000000
#define ATI_CRTC_PIX_WIDTH_8   0x00000200
#define ATI_CRTC_PIX_WIDTH_32  0x00000600
#define ATI_DST_OFFSET         0x1404
#define ATI_DST_PITCH          0x1408
#define ATI_DST_WIDTH          0x140c
#define ATI_DST_HEIGHT         0x1410
#define ATI_SRC_X              0x1414
#define ATI_SRC_Y              0x1418
#define ATI_DST_X              0x141c
#define ATI_DST_Y              0x1420
#define ATI_DP_GUI_MASTER_CNTL 0x146c
#define ATI_DP_BRUSH_FRGD_CLR  0x147c
#define ATI_SRC_OFFSET         0x15ac
#define ATI_SRC_PITCH          0x15b0
#define ATI_SC_TOP_LEFT        0x16ec
#define ATI_SC_BOTTOM_RIGHT    0x16f0
#define ATI_DP_CNTL            0x16c0
#define ATI_DST_LTR_TTB        0x00000003
#define ATI_DST_RTL_TTB        0x00000002
#define ATI_GMC_SRC_PITCH      0x00000001
#define ATI_GMC_DST_PITCH      0x00000002
#define ATI_GMC_DST_CLIPPING   0x00000008
#define ATI_GMC_BRUSH_SOLID    0x000000d0
#define ATI_GMC_DST_8BPP       0x00000200
#define ATI_GMC_SRC_COLOR      0x00003000
#define ATI_GMC_ROP3_SRCCOPY   0x00cc0000
#define ATI_GMC_ROP3_PATCOPY   0x00f00000
#define ATI_GMC_DP_SRC_RECT    0x02000000
#define VGA_SEQ_INDEX          0x3c4
#define VGA_SEQ_DATA           0x3c5
#define VGA_SEQ_RESET          0
#define VGA_SEQ_PLANE_WRITE    2
#define VGA_SEQ_MEMORY_MODE    4
#define VGA_GFX_INDEX          0x3ce
#define VGA_GFX_DATA           0x3cf
#define VGA_GFX_SR_ENABLE      1
#define VGA_GFX_DATA_ROTATE    3
#define VGA_GFX_PLANE_READ     4
#define VGA_GFX_MODE           5
#define VGA_GFX_MISC           6
#define VGA_GFX_BIT_MASK       8
#define VGA_CRTC_INDEX         0x3b4
#define VGA_CRTC_DATA          0x3b5
#define VGA_CRTC_OFFSET        0x13
#define VGA_ATTR_INDEX         0x3c0
#define VGA_INPUT_STATUS1      0x3ba
#define VGA_PEL_WRITE_INDEX    0x3c8
#define VGA_PEL_DATA           0x3c9

static const char *machine_args(void)
{
    return g_str_equal(qtest_get_arch(), "ia64") ?
           "-machine ia64-vpc " : "";
}

static void pci_multihead(void)
{
    QTestState *qts;

    qts = qtest_initf("%s-vga none -device VGA -device secondary-vga",
                      machine_args());
    qtest_quit(qts);
}

static void test_vga(gconstpointer data)
{
    QTestState *qts;

    qts = qtest_initf("%s-vga none -device %s", machine_args(),
                      (const char *)data);
    qtest_quit(qts);
}

static void vbe_legacy_data_port(void)
{
    QTestState *qts;
    uint16_t id;

    if (g_str_equal(qtest_get_arch(), "ia64")) {
        qts = qtest_init("-machine ia64-vpc -vga std");
        qtest_writew(qts, IA64_LEGACY_IO_BASE + VBE_DISPI_IOPORT_INDEX,
                     VBE_DISPI_INDEX_ID);
        id = qtest_readw(qts,
                         IA64_LEGACY_IO_BASE + VBE_DISPI_IOPORT_INDEX + 2);
        g_assert_cmphex(id, ==, VBE_DISPI_ID5);
        g_assert_cmphex(qtest_readw(qts,
                                    IA64_LEGACY_IO_BASE +
                                    VBE_DISPI_IOPORT_DATA), ==, id);

        qtest_writew(qts, IA64_LEGACY_IO_BASE + VBE_DISPI_IOPORT_INDEX,
                     VBE_DISPI_INDEX_ENABLE);
        qtest_writew(qts, IA64_LEGACY_IO_BASE +
                          VBE_DISPI_IOPORT_INDEX + 2,
                     VBE_DISPI_ENABLED | VBE_DISPI_LFB_ENABLED);
        g_assert_cmphex(qtest_readw(qts, IA64_LEGACY_IO_BASE +
                                        VBE_DISPI_IOPORT_INDEX + 2), ==,
                        VBE_DISPI_ENABLED | VBE_DISPI_LFB_ENABLED);
        qtest_writeb(qts, IA64_LEGACY_IO_BASE + VGA_SEQ_INDEX,
                     VGA_SEQ_RESET);
        qtest_writeb(qts, IA64_LEGACY_IO_BASE + VGA_SEQ_DATA, 1);
        g_assert_cmphex(qtest_readw(qts, IA64_LEGACY_IO_BASE +
                                        VBE_DISPI_IOPORT_INDEX + 2), ==, 0);
    } else {
        qts = qtest_init("-vga none -device VGA");
        qtest_outw(qts, VBE_DISPI_IOPORT_INDEX, VBE_DISPI_INDEX_ID);
        id = qtest_inw(qts, VBE_DISPI_IOPORT_INDEX + 2);
        g_assert_cmphex(id, ==, VBE_DISPI_ID5);
        g_assert_cmphex(qtest_inw(qts, VBE_DISPI_IOPORT_DATA), ==, id);
    }
    qtest_quit(qts);
}

static void vga_wide_planar_access(void)
{
    const uint64_t plane0 = 0x0123456789abcdefULL;
    const uint64_t plane2 = 0xfedcba9876543210ULL;
    const uint64_t unaligned = 0x55aa996633ccf00fULL;
    const uint64_t colors = 0x0f0e0d0c0b0a0908ULL;
    const uint64_t expanded[4] = {
        0xff00ff00ff00ff00ULL,
        0xffff0000ffff0000ULL,
        0xffffffff00000000ULL,
        0xffffffffffffffffULL,
    };
    QTestState *qts;
    unsigned plane;

    qts = qtest_init("-machine ia64-vpc -vga std -S");

    qtest_writeb(qts, IA64_LEGACY_IO_BASE + VGA_SEQ_INDEX,
                 VGA_SEQ_MEMORY_MODE);
    qtest_writeb(qts, IA64_LEGACY_IO_BASE + VGA_SEQ_DATA, 0x06);
    qtest_writeb(qts, IA64_LEGACY_IO_BASE + VGA_GFX_INDEX, VGA_GFX_MISC);
    qtest_writeb(qts, IA64_LEGACY_IO_BASE + VGA_GFX_DATA, 0x01);
    qtest_writeb(qts, IA64_LEGACY_IO_BASE + VGA_GFX_INDEX, VGA_GFX_MODE);
    qtest_writeb(qts, IA64_LEGACY_IO_BASE + VGA_GFX_DATA, 0);
    qtest_writeb(qts, IA64_LEGACY_IO_BASE + VGA_GFX_INDEX,
                 VGA_GFX_SR_ENABLE);
    qtest_writeb(qts, IA64_LEGACY_IO_BASE + VGA_GFX_DATA, 0);
    qtest_writeb(qts, IA64_LEGACY_IO_BASE + VGA_GFX_INDEX,
                 VGA_GFX_DATA_ROTATE);
    qtest_writeb(qts, IA64_LEGACY_IO_BASE + VGA_GFX_DATA, 0);
    qtest_writeb(qts, IA64_LEGACY_IO_BASE + VGA_GFX_INDEX,
                 VGA_GFX_BIT_MASK);
    qtest_writeb(qts, IA64_LEGACY_IO_BASE + VGA_GFX_DATA, 0xff);

    qtest_writeb(qts, IA64_LEGACY_IO_BASE + VGA_SEQ_INDEX,
                 VGA_SEQ_PLANE_WRITE);
    qtest_writeb(qts, IA64_LEGACY_IO_BASE + VGA_SEQ_DATA, 1U << 0);
    qtest_writeq(qts, IA64_VGA_LEGACY_BASE, plane0);
    qtest_writeb(qts, IA64_LEGACY_IO_BASE + VGA_SEQ_DATA, 1U << 2);
    qtest_writeq(qts, IA64_VGA_LEGACY_BASE, plane2);

    qtest_writeb(qts, IA64_LEGACY_IO_BASE + VGA_GFX_INDEX,
                 VGA_GFX_PLANE_READ);
    qtest_writeb(qts, IA64_LEGACY_IO_BASE + VGA_GFX_DATA, 0);
    g_assert_cmphex(qtest_readq(qts, IA64_VGA_LEGACY_BASE), ==, plane0);
    qtest_writeb(qts, IA64_LEGACY_IO_BASE + VGA_GFX_DATA, 2);
    g_assert_cmphex(qtest_readq(qts, IA64_VGA_LEGACY_BASE), ==, plane2);

    qtest_writeb(qts, IA64_LEGACY_IO_BASE + VGA_SEQ_INDEX,
                 VGA_SEQ_PLANE_WRITE);
    qtest_writeb(qts, IA64_LEGACY_IO_BASE + VGA_SEQ_DATA, 1U << 1);
    qtest_writeq(qts, IA64_VGA_LEGACY_BASE + 1, unaligned);
    qtest_writeb(qts, IA64_LEGACY_IO_BASE + VGA_GFX_INDEX,
                 VGA_GFX_PLANE_READ);
    qtest_writeb(qts, IA64_LEGACY_IO_BASE + VGA_GFX_DATA, 1);
    g_assert_cmphex(qtest_readq(qts, IA64_VGA_LEGACY_BASE + 1), ==,
                    unaligned);

    qtest_writeb(qts, IA64_LEGACY_IO_BASE + VGA_SEQ_DATA, 0x0f);
    qtest_writeb(qts, IA64_LEGACY_IO_BASE + VGA_GFX_INDEX, VGA_GFX_MODE);
    qtest_writeb(qts, IA64_LEGACY_IO_BASE + VGA_GFX_DATA, 2);
    qtest_writeq(qts, IA64_VGA_LEGACY_BASE + 0x100, colors);
    qtest_writeb(qts, IA64_LEGACY_IO_BASE + VGA_GFX_INDEX, VGA_GFX_MODE);
    qtest_writeb(qts, IA64_LEGACY_IO_BASE + VGA_GFX_DATA, 0);
    qtest_writeb(qts, IA64_LEGACY_IO_BASE + VGA_GFX_INDEX,
                 VGA_GFX_PLANE_READ);
    for (plane = 0; plane < 4; plane++) {
        qtest_writeb(qts, IA64_LEGACY_IO_BASE + VGA_GFX_DATA, plane);
        g_assert_cmphex(qtest_readq(qts, IA64_VGA_LEGACY_BASE + 0x100), ==,
                        expanded[plane]);
    }

    qtest_quit(qts);
}

static uint16_t ati_vbe_read(QTestState *qts, uint16_t index)
{
    uint64_t index_port = IA64_LEGACY_IO_BASE + VBE_DISPI_IOPORT_INDEX;

    qtest_writew(qts, index_port, index);
    return qtest_readw(qts, index_port + 2);
}

static char *ppm_next_token(const uint8_t **cursor, const uint8_t *end)
{
    const uint8_t *start;

    while (*cursor < end) {
        if (g_ascii_isspace(**cursor)) {
            (*cursor)++;
            continue;
        }
        if (**cursor == '#') {
            while (*cursor < end && **cursor != '\n') {
                (*cursor)++;
            }
            continue;
        }
        break;
    }
    g_assert_cmpuint(end - *cursor, >, 0);
    start = *cursor;
    while (*cursor < end && !g_ascii_isspace(**cursor) && **cursor != '#') {
        (*cursor)++;
    }
    g_assert_cmpuint(*cursor - start, >, 0);
    return g_strndup((const char *)start, *cursor - start);
}

static void assert_ppm_stride(const char *filename, unsigned width,
                              unsigned height)
{
    g_autofree char *contents = NULL;
    g_autofree char *magic = NULL;
    g_autofree char *width_token = NULL;
    g_autofree char *height_token = NULL;
    g_autofree char *max_token = NULL;
    g_autoptr(GError) error = NULL;
    const uint8_t *cursor;
    const uint8_t *end;
    const uint8_t *row0;
    const uint8_t *row1;
    gsize length;

    g_assert_true(g_file_get_contents(filename, &contents, &length, &error));
    g_assert_no_error(error);
    cursor = (const uint8_t *)contents;
    end = cursor + length;
    magic = ppm_next_token(&cursor, end);
    width_token = ppm_next_token(&cursor, end);
    height_token = ppm_next_token(&cursor, end);
    max_token = ppm_next_token(&cursor, end);
    g_assert_cmpstr(magic, ==, "P6");
    g_assert_cmpuint(g_ascii_strtoull(width_token, NULL, 10), ==, width);
    g_assert_cmpuint(g_ascii_strtoull(height_token, NULL, 10), ==, height);
    g_assert_cmpuint(g_ascii_strtoull(max_token, NULL, 10), ==, 255);

    g_assert_true(cursor < end && g_ascii_isspace(*cursor));
    if (*cursor++ == '\r' && cursor < end && *cursor == '\n') {
        cursor++;
    }
    g_assert_cmpuint(end - cursor, >=, (gsize)width * height * 3);
    row0 = cursor;
    row1 = cursor + width * 3;

    /* Both markers are visible only if row 1 starts at the virtual pitch. */
    g_assert_cmpmem(row0, 3, row1, 3);
    g_assert_cmpint(memcmp(row0, row0 + 3, 3), !=, 0);
}

static void assert_ppm_pixel(const char *filename, unsigned width,
                             unsigned height, unsigned x, unsigned y,
                             uint8_t red, uint8_t green, uint8_t blue)
{
    g_autofree char *contents = NULL;
    g_autofree char *magic = NULL;
    g_autofree char *width_token = NULL;
    g_autofree char *height_token = NULL;
    g_autofree char *max_token = NULL;
    g_autoptr(GError) error = NULL;
    const uint8_t *cursor;
    const uint8_t *end;
    const uint8_t *pixel;
    gsize length;

    g_assert_true(g_file_get_contents(filename, &contents, &length, &error));
    g_assert_no_error(error);
    cursor = (const uint8_t *)contents;
    end = cursor + length;
    magic = ppm_next_token(&cursor, end);
    width_token = ppm_next_token(&cursor, end);
    height_token = ppm_next_token(&cursor, end);
    max_token = ppm_next_token(&cursor, end);
    g_assert_cmpstr(magic, ==, "P6");
    g_assert_cmpuint(g_ascii_strtoull(width_token, NULL, 10), ==, width);
    g_assert_cmpuint(g_ascii_strtoull(height_token, NULL, 10), ==, height);
    g_assert_cmpuint(g_ascii_strtoull(max_token, NULL, 10), ==, 255);
    g_assert_cmpuint(x, <, width);
    g_assert_cmpuint(y, <, height);

    g_assert_true(cursor < end && g_ascii_isspace(*cursor));
    if (*cursor++ == '\r' && cursor < end && *cursor == '\n') {
        cursor++;
    }
    g_assert_cmpuint(end - cursor, >=, (gsize)width * height * 3);
    pixel = cursor + ((gsize)y * width + x) * 3;
    g_assert_cmphex(pixel[0], ==, red);
    g_assert_cmphex(pixel[1], ==, green);
    g_assert_cmphex(pixel[2], ==, blue);
}

static void ati_blit_visible_intersection(void)
{
    const unsigned width = 640;
    const unsigned height = 480;
    const unsigned virtual_width = 704;
    const unsigned pitch = virtual_width;
    const unsigned offset = pitch;
    QTestState *qts;
    g_autofree char *tmpdir = NULL;
    g_autofree char *before = NULL;
    g_autofree char *after = NULL;
    g_autoptr(GError) error = NULL;

    qts = qtest_init("-machine ia64-vpc -m 256M -S");
    qtest_writel(qts, IA64_ATI_MMIO_BASE + ATI_CRTC_H_TOTAL_DISP,
                 ((width / 8) - 1) << 16);
    qtest_writel(qts, IA64_ATI_MMIO_BASE + ATI_CRTC_V_TOTAL_DISP,
                 (height - 1) << 16);
    qtest_writel(qts, IA64_ATI_MMIO_BASE + ATI_CRTC_OFFSET, offset);
    qtest_writel(qts, IA64_ATI_MMIO_BASE + ATI_CRTC_PITCH,
                 virtual_width / 8);
    qtest_writel(qts, IA64_ATI_MMIO_BASE + ATI_CRTC_GEN_CNTL,
                 ATI_CRTC_EXT_DISP_EN | ATI_CRTC_EN |
                 ATI_CRTC_PIX_WIDTH_8);

    qtest_writeb(qts, IA64_LEGACY_IO_BASE + VGA_PEL_WRITE_INDEX, 1);
    qtest_writeb(qts, IA64_LEGACY_IO_BASE + VGA_PEL_DATA, 0x3f);
    qtest_writeb(qts, IA64_LEGACY_IO_BASE + VGA_PEL_DATA, 0);
    qtest_writeb(qts, IA64_LEGACY_IO_BASE + VGA_PEL_DATA, 0);
    qtest_readb(qts, IA64_LEGACY_IO_BASE + VGA_INPUT_STATUS1);
    qtest_writeb(qts, IA64_LEGACY_IO_BASE + VGA_ATTR_INDEX, 0x20);

    tmpdir = g_dir_make_tmp("ia64-ati-dirty-XXXXXX", &error);
    g_assert_no_error(error);
    g_assert_nonnull(tmpdir);
    before = g_build_filename(tmpdir, "before.ppm", NULL);
    after = g_build_filename(tmpdir, "after.ppm", NULL);
    qtest_qmp_assert_success(qts,
                             "{'execute':'screendump','arguments':"
                             " {'filename':%s}}", before);
    assert_ppm_pixel(before, width, height, 0, 0, 0, 0, 0);

    qtest_writel(qts, IA64_ATI_MMIO_BASE + ATI_DST_OFFSET, 0);
    qtest_writel(qts, IA64_ATI_MMIO_BASE + ATI_DST_PITCH,
                 virtual_width / 8);
    qtest_writel(qts, IA64_ATI_MMIO_BASE + ATI_SC_TOP_LEFT, 0);
    qtest_writel(qts, IA64_ATI_MMIO_BASE + ATI_SC_BOTTOM_RIGHT,
                 (height << 16) | (width - 1));
    qtest_writel(qts, IA64_ATI_MMIO_BASE + ATI_DP_CNTL, ATI_DST_LTR_TTB);
    qtest_writel(qts, IA64_ATI_MMIO_BASE + ATI_DP_BRUSH_FRGD_CLR, 1);
    qtest_writel(qts, IA64_ATI_MMIO_BASE + ATI_DP_GUI_MASTER_CNTL,
                 ATI_GMC_DST_PITCH | ATI_GMC_DST_CLIPPING |
                 ATI_GMC_BRUSH_SOLID | ATI_GMC_DST_8BPP |
                 ATI_GMC_ROP3_PATCOPY);
    qtest_writel(qts, IA64_ATI_MMIO_BASE + ATI_DST_X, 0);
    qtest_writel(qts, IA64_ATI_MMIO_BASE + ATI_DST_Y, 1);
    qtest_writel(qts, IA64_ATI_MMIO_BASE + ATI_DST_HEIGHT, 1);
    qtest_writel(qts, IA64_ATI_MMIO_BASE + ATI_DST_WIDTH, 16);

    qtest_qmp_assert_success(qts,
                             "{'execute':'screendump','arguments':"
                             " {'filename':%s}}", after);
    assert_ppm_pixel(after, width, height, 0, 0, 0xff, 0, 0);
    assert_ppm_pixel(after, width, height, 16, 0, 0, 0, 0);
    qtest_quit(qts);

    g_assert_cmpint(g_unlink(before), ==, 0);
    g_assert_cmpint(g_unlink(after), ==, 0);
    g_assert_cmpint(g_rmdir(tmpdir), ==, 0);
}

static void ati_reverse_overlap_blit(void)
{
    const unsigned pitch = 64;
    uint8_t initial[32];
    uint8_t expected[32];
    uint8_t actual[32];
    QTestState *qts;
    unsigned i;

    for (i = 0; i < sizeof(initial); i++) {
        initial[i] = i;
    }
    memcpy(expected, initial, sizeof(expected));
    memmove(&expected[4], &expected[0], 16);

    qts = qtest_init("-machine ia64-vpc -m 256M -S");
    qtest_memwrite(qts, IA64_ATI_FB_BASE, initial, sizeof(initial));
    qtest_writel(qts, IA64_ATI_MMIO_BASE + ATI_DST_OFFSET, 0);
    qtest_writel(qts, IA64_ATI_MMIO_BASE + ATI_DST_PITCH, pitch / 8);
    qtest_writel(qts, IA64_ATI_MMIO_BASE + ATI_SRC_OFFSET, 0);
    qtest_writel(qts, IA64_ATI_MMIO_BASE + ATI_SRC_PITCH, pitch / 8);
    qtest_writel(qts, IA64_ATI_MMIO_BASE + ATI_SC_TOP_LEFT, 0);
    qtest_writel(qts, IA64_ATI_MMIO_BASE + ATI_SC_BOTTOM_RIGHT,
                 (1U << 16) | (pitch - 1));
    qtest_writel(qts, IA64_ATI_MMIO_BASE + ATI_DP_CNTL, ATI_DST_RTL_TTB);
    qtest_writel(qts, IA64_ATI_MMIO_BASE + ATI_DP_GUI_MASTER_CNTL,
                 ATI_GMC_SRC_PITCH | ATI_GMC_DST_PITCH |
                 ATI_GMC_DST_CLIPPING | ATI_GMC_DST_8BPP |
                 ATI_GMC_SRC_COLOR | ATI_GMC_ROP3_SRCCOPY |
                 ATI_GMC_DP_SRC_RECT);
    qtest_writel(qts, IA64_ATI_MMIO_BASE + ATI_SRC_X, 15);
    qtest_writel(qts, IA64_ATI_MMIO_BASE + ATI_SRC_Y, 0);
    qtest_writel(qts, IA64_ATI_MMIO_BASE + ATI_DST_X, 19);
    qtest_writel(qts, IA64_ATI_MMIO_BASE + ATI_DST_Y, 0);
    qtest_writel(qts, IA64_ATI_MMIO_BASE + ATI_DST_HEIGHT, 1);
    qtest_writel(qts, IA64_ATI_MMIO_BASE + ATI_DST_WIDTH, 16);

    qtest_memread(qts, IA64_ATI_FB_BASE, actual, sizeof(actual));
    g_assert_cmpmem(actual, sizeof(actual), expected, sizeof(expected));
    qtest_quit(qts);
}

/*
 * MM_APER (MM_INDEX bit 31) turns MM_DATA into a window on Linear Aperture
 * 0.  The offset is a VRAM byte offset that includes the sub-dword part of
 * the MM_DATA address, and it aliases within physical VRAM.
 */
static void ati_mm_aper(void)
{
    QTestState *qts;

    qts = qtest_init("-machine ia64-vpc -m 256M -S");

    qtest_writel(qts, IA64_ATI_MMIO_BASE + ATI_MM_INDEX, 0x80000004);
    qtest_writel(qts, IA64_ATI_MMIO_BASE + ATI_MM_DATA, 0x11223344);
    g_assert_cmphex(qtest_readl(qts, IA64_ATI_FB_BASE + 4), ==, 0x11223344);
    g_assert_cmphex(qtest_readl(qts, IA64_ATI_MMIO_BASE + ATI_MM_DATA), ==,
                    0x11223344);

    /* An offset past the end of VRAM wraps instead of reading as zero. */
    qtest_writel(qts, IA64_ATI_MMIO_BASE + ATI_MM_INDEX, UINT32_MAX);
    g_assert_cmphex(qtest_readl(qts, IA64_ATI_MMIO_BASE + ATI_MM_INDEX), ==,
                    0xfffffffc);
    qtest_writel(qts, IA64_ATI_MMIO_BASE + ATI_MM_DATA, 0xaabbccdd);
    g_assert_cmphex(qtest_readl(qts, IA64_ATI_FB_BASE + ATI_VRAM_SIZE - 4),
                    ==, 0xaabbccdd);
    qtest_writel(qts, IA64_ATI_FB_BASE + ATI_VRAM_SIZE - 4, 0x55667788);
    g_assert_cmphex(qtest_readl(qts, IA64_ATI_MMIO_BASE + ATI_MM_DATA), ==,
                    0x55667788);

    /* Sub-dword accesses address bytes within the selected offset. */
    qtest_writel(qts, IA64_ATI_MMIO_BASE + ATI_MM_INDEX, 0x81000007);
    g_assert_cmphex(qtest_readl(qts, IA64_ATI_MMIO_BASE + ATI_MM_INDEX), ==,
                    0x81000004);
    g_assert_cmphex(qtest_readl(qts, IA64_ATI_MMIO_BASE + ATI_MM_DATA), ==,
                    0x11223344);
    qtest_writeb(qts, IA64_ATI_MMIO_BASE + ATI_MM_DATA + 1, 0x5a);
    qtest_writew(qts, IA64_ATI_MMIO_BASE + ATI_MM_DATA + 2, 0xc3d4);
    g_assert_cmphex(qtest_readb(qts, IA64_ATI_MMIO_BASE + ATI_MM_DATA + 1),
                    ==, 0x5a);
    g_assert_cmphex(qtest_readw(qts, IA64_ATI_MMIO_BASE + ATI_MM_DATA + 2),
                    ==, 0xc3d4);
    g_assert_cmphex(qtest_readl(qts, IA64_ATI_FB_BASE + 4), ==, 0xc3d45a44);

    qtest_quit(qts);
}

static void ati_stride(void)
{
    const unsigned width = 640;
    const unsigned height = 480;
    const unsigned bpp = 32;
    const unsigned virtual_width = 704;
    const uint64_t pitch = virtual_width * (bpp / 8);
    const uint32_t marker = 0x00123456;
    const uint32_t padding_decoy = 0x00654321;
    QTestState *qts;
    g_autofree char *tmpdir = NULL;
    g_autofree char *ppm = NULL;
    g_autoptr(GError) error = NULL;

    qts = qtest_init("-machine ia64-vpc -m 256M -S");
    /*
     * Program the ATI CRTC, not the generic VBE ports.  ati_vga_switch_mode()
     * must translate the Rage128 pitch (eight-pixel units) into the VBE
     * virtual width used by the common scanout path.
     */
    qtest_writel(qts, IA64_ATI_MMIO_BASE + ATI_CRTC_H_TOTAL_DISP,
                 ((width / 8) - 1) << 16);
    qtest_writel(qts, IA64_ATI_MMIO_BASE + ATI_CRTC_V_TOTAL_DISP,
                 (height - 1) << 16);
    qtest_writel(qts, IA64_ATI_MMIO_BASE + ATI_CRTC_OFFSET, 0);
    qtest_writel(qts, IA64_ATI_MMIO_BASE + ATI_CRTC_PITCH,
                 virtual_width / 8);
    qtest_writel(qts, IA64_ATI_MMIO_BASE + ATI_CRTC_GEN_CNTL,
                 ATI_CRTC_EXT_DISP_EN | ATI_CRTC_EN |
                 ATI_CRTC_PIX_WIDTH_32);

    g_assert_cmphex(ati_vbe_read(qts, VBE_DISPI_INDEX_ENABLE), ==,
                    VBE_DISPI_ENABLED | VBE_DISPI_LFB_ENABLED |
                    VBE_DISPI_NOCLEARMEM);
    g_assert_cmpuint(ati_vbe_read(qts, VBE_DISPI_INDEX_XRES), ==, width);
    g_assert_cmpuint(ati_vbe_read(qts, VBE_DISPI_INDEX_YRES), ==, height);
    g_assert_cmpuint(ati_vbe_read(qts, VBE_DISPI_INDEX_BPP), ==, bpp);
    g_assert_cmpuint(ati_vbe_read(qts, VBE_DISPI_INDEX_VIRT_WIDTH), ==,
                     virtual_width);
    qtest_writeb(qts, IA64_LEGACY_IO_BASE + VGA_CRTC_INDEX,
                 VGA_CRTC_OFFSET);
    g_assert_cmphex(qtest_readb(qts, IA64_LEGACY_IO_BASE + VGA_CRTC_DATA), ==,
                    (pitch / 8) & 0xff);

    /* Leave attribute-controller blanking, as a real VBE client does. */
    qtest_readb(qts, IA64_LEGACY_IO_BASE + VGA_INPUT_STATUS1);
    qtest_writeb(qts, IA64_LEGACY_IO_BASE + VGA_ATTR_INDEX, 0x20);

    qtest_writel(qts, IA64_ATI_FB_BASE, marker);
    qtest_writel(qts, IA64_ATI_FB_BASE + width * (bpp / 8), padding_decoy);
    qtest_writel(qts, IA64_ATI_FB_BASE + pitch, marker);

    tmpdir = g_dir_make_tmp("ia64-ati-stride-XXXXXX", &error);
    g_assert_no_error(error);
    g_assert_nonnull(tmpdir);
    ppm = g_build_filename(tmpdir, "stride.ppm", NULL);
    qtest_qmp_assert_success(qts,
                             "{'execute':'screendump','arguments':"
                             " {'filename':%s}}", ppm);
    assert_ppm_stride(ppm, width, height);

    qtest_writeb(qts, IA64_LEGACY_IO_BASE + VGA_SEQ_INDEX, VGA_SEQ_RESET);
    qtest_writeb(qts, IA64_LEGACY_IO_BASE + VGA_SEQ_DATA, 1);
    g_assert_cmphex(ati_vbe_read(qts, VBE_DISPI_INDEX_ENABLE), ==, 0);
    g_assert_cmpuint(ati_vbe_read(qts, VBE_DISPI_INDEX_VIRT_WIDTH), ==,
                     virtual_width);

    qtest_system_reset(qts);
    g_assert_cmphex(ati_vbe_read(qts, VBE_DISPI_INDEX_ENABLE), ==, 0);
    g_assert_cmphex(ati_vbe_read(qts, VBE_DISPI_INDEX_VIRT_WIDTH), ==, 0);
    qtest_quit(qts);

    g_assert_cmpint(g_unlink(ppm), ==, 0);
    g_assert_cmpint(g_rmdir(tmpdir), ==, 0);
}

int main(int argc, char **argv)
{
    static const char *devices[] = {
        "cirrus-vga",
        "VGA",
        "secondary-vga",
        "virtio-gpu-pci",
        "virtio-vga"
    };

    g_test_init(&argc, &argv, NULL);

    for (int i = 0; i < ARRAY_SIZE(devices); i++) {
        if (qtest_has_device(devices[i])) {
            char *testpath = g_strdup_printf("/display/pci/%s", devices[i]);
            qtest_add_data_func(testpath, devices[i], test_vga);
            g_free(testpath);
        }
    }

    if (qtest_has_device("secondary-vga")) {
        qtest_add_func("/display/pci/multihead", pci_multihead);
    }
    if (qtest_has_device("VGA")) {
        qtest_add_func("/display/pci/vbe-legacy-data-port",
                       vbe_legacy_data_port);
    }
    if (g_str_equal(qtest_get_arch(), "ia64") &&
        qtest_has_device("ati-vga")) {
        qtest_add_func("/display/pci/ati-mm-aper", ati_mm_aper);
        qtest_add_func("/display/pci/ati-stride", ati_stride);
        qtest_add_func("/display/pci/ati-blit-visible-intersection",
                       ati_blit_visible_intersection);
        qtest_add_func("/display/pci/ati-reverse-overlap-blit",
                       ati_reverse_overlap_blit);
    }
    if (g_str_equal(qtest_get_arch(), "ia64") &&
        qtest_has_device("VGA")) {
        qtest_add_func("/display/pci/vga-wide-planar-access",
                       vga_wide_planar_access);
    }

    return g_test_run();
}
