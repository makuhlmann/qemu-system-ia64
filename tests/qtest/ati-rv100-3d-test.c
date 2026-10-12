/*
 * QTest cases for the ATI RV100's fixed-function 3D engine (x-r100-3d=on)
 *
 * Copyright (c) 2014 Red Hat, Inc
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 * The cases are syunnPC's qemu-system-ia64 tests/qtest/display-vga-test.c
 * (2a8f7bb) RV100 3D cases, moved onto zx1's vga=rv100 seat.
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "hw/pci/pci_regs.h"
#include "hw/ia64/ia64_vpc_abi.h"
#include "libqtest.h"
#include "qobject/qdict.h"

#define RV100_DEVFN     ((IA64_MERCURY_VGA_SLOT << 3) | 0)
#define RV100_CFG(reg)  (IA64_PCI_CONFIG_BASE + \
                         ((uint64_t)IA64_MERCURY_BUS << 20) + \
                         ((uint64_t)RV100_DEVFN << 12) + (reg))

#define ATI_GEN_INT_STATUS     0x0044
#define ATI_GEN_INT_CNTL       0x0040
#define ATI_R100_GEN_INT_STATUS_RESET     0x00080000U
#define ATI_SW_INT_ENABLE      (1U << 25)
#define ATI_SW_INT_TEST        (1U << 25)
#define ATI_SW_INT_FIRE        (1U << 26)
#define R100_CP_RB_BASE        0x0700
#define R100_CP_RB_CNTL        0x0704
#define R100_CP_RB_RPTR_ADDR   0x070c
#define R100_CP_RB_RPTR        0x0710
#define R100_CP_RB_WPTR        0x0714
#define R100_CP_RB_RPTR_WR     0x071c
#define R100_CP_IB_BASE        0x0738
#define R100_CP_IB_BUFSZ       0x073c
#define R100_CP_CSQ_CNTL       0x0740
#define R100_CSQ_PRIBM_INDBM   (4U << 28)
#define R100_CP_ME_RAM_ADDR    0x07d4
#define R100_CP_ME_RAM_DATAH   0x07dc
#define R100_CP_ME_RAM_DATAL   0x07e0
#define R100_SCRATCH_REG0      0x15e0
#define R100_FOG_TABLE_INDEX   0x1a14
#define R100_FOG_TABLE_DATA    0x1a18
#define R100_PP_FOG_COLOR      0x1c18
#define R100_RB3D_BLENDCNTL    0x1c20
#define R100_RB3D_DEPTHOFFSET  0x1c24
#define R100_RB3D_DEPTHPITCH   0x1c28
#define R100_RB3D_ZSTENCILCNTL 0x1c2c
#define R100_PP_CNTL           0x1c38
#define R100_RB3D_CNTL         0x1c3c
#define R100_RB3D_COLOROFFSET  0x1c40
#define R100_RE_WIDTH_HEIGHT   0x1c44
#define R100_RB3D_COLORPITCH   0x1c48
#define R100_SE_CNTL           0x1c4c
#define R100_SE_COORD_FMT      0x1c50
#define R100_PP_TXFILTER_0     0x1c54
#define R100_PP_TXFORMAT_0     0x1c58
#define R100_PP_TXOFFSET_0     0x1c5c
#define R100_PP_TXCBLEND_0     0x1c60
#define R100_PP_TXABLEND_0     0x1c64
#define R100_PP_TXFILTER_1     0x1c6c
#define R100_PP_TXFORMAT_1     0x1c70
#define R100_PP_TXOFFSET_1     0x1c74
#define R100_PP_TXCBLEND_1     0x1c78
#define R100_PP_TXABLEND_1     0x1c7c
#define R100_PP_TEX_SIZE_0     0x1d04
#define R100_PP_TEX_PITCH_0    0x1d08
#define R100_PP_TEX_SIZE_1     0x1d0c
#define R100_PP_TEX_PITCH_1    0x1d10
#define R100_PP_BORDER_COLOR_0 0x1d40
#define R100_RB3D_STENCILREFMASK 0x1d7c
#define R100_SE_VPORT_ZSCALE  0x1da8
#define R100_SE_VPORT_ZOFFSET 0x1dac
#define R100_RE_TOP_LEFT       0x26c0
#define R100_SE_PORT_DATA0     0x2000
#define R100_SE_VTX_FMT        0x2080
#define R100_SE_VF_CNTL        0x2084
#define R100_MC_FB_LOCATION    0x0148
#define R100_AIC_CNTL          0x01d0
#define R100_AIC_PT_BASE       0x01d8
#define R100_AIC_LO_ADDR       0x01dc
#define R100_AIC_HI_ADDR       0x01e0
#define R100_CP_PACKET2        (2U << 30)
#define R100_CP_PACKET3        (3U << 30)
#define R100_PACKET3_RNDR_GEN_PRIM 0x25
#define R100_PACKET3_DRAW_VBUF 0x28
#define R100_PACKET3_DRAW_IMMD 0x29
#define R100_PACKET3_DRAW_INDX 0x2a
#define R100_PACKET3_LOAD_VBPNTR 0x2f
#define R100_VTX_FMT_PKCOLOR   (1U << 3)
#define R100_VTX_FMT_FPSPEC    (1U << 4)
#define R100_VTX_FMT_FPFOG     (1U << 5)
#define R100_VTX_FMT_PKSPEC    (1U << 6)
#define R100_VTX_FMT_W0        (1U << 0)
#define R100_VTX_FMT_ST0       (1U << 7)
#define R100_VTX_FMT_ST1       (1U << 8)
#define R100_VTX_FMT_Q0        (1U << 14)
#define R100_VTX_FMT_N1        (1U << 30)
#define R100_VTX_FMT_Z         (1U << 31)
#define R100_VF_POINT_LIST     1
#define R100_VF_LINE_LIST      2
#define R100_VF_TRIANGLE_LIST  4
#define R100_VF_RECTANGLE_LIST 8
#define R100_VF_POLYGON        15
#define R100_VF_WALK_DATA      (3U << 4)
#define R100_VF_WALK_IND       (1U << 4)
#define R100_VF_WALK_LIST      (2U << 4)
#define R100_VF_COLOR_RGBA     (1U << 6)
#define R100_VF_INDEX_SIZE_32  (1U << 11)
#define R100_RB_Z_ENABLE       (1U << 8)
#define R100_RB_STENCIL_ENABLE (1U << 7)
#define R100_RB_ALPHA_BLEND    (1U << 0)
#define R100_RB_COLOR_ARGB8888 (6U << 10)
#define R100_SRC_BLEND_ONE     (33U << 16)
#define R100_DST_BLEND_ONE     (33U << 24)
#define R100_COLOR_TILE_ENABLE (1U << 16)
#define R100_Z_WRITE_ENABLE    (1U << 30)
#define R100_VPORT_Z_XFORM     (1U << 25)
#define R100_VTX_PIX_CENTER_OGL (1U << 27)
#define R100_TX0_ENABLE        (1U << 4)
#define R100_TX1_ENABLE        (1U << 5)
#define R100_TX_BLEND0_ENABLE  (1U << 12)
#define R100_TX_BLEND1_ENABLE  (1U << 13)
#define R100_PP_SPECULAR_ENABLE (1U << 21)
#define R100_PP_FOG_ENABLE     (1U << 22)
#define R100_FOG_TABLE         (1U << 24)
#define R100_FOG_USE_DIFFUSE_ALPHA (2U << 25)
#define R100_FOG_USE_SPEC_ALPHA (3U << 25)
#define R100_TX_ARGB8888       6
#define R100_TX_I8             0
#define R100_TX_AI88           1
#define R100_TX_Y8             8
#define R100_TX_VYUY422        10
#define R100_TX_YVYU422        11
#define R100_TX_DXT1           12
#define R100_TX_DXT3           14
#define R100_TX_DXT5           15
#define R100_TX_ALPHA_IN_MAP   (1U << 6)
#define R100_TX_NON_POWER2     (1U << 7)
#define R100_TX_PERSPECTIVE    (1U << 31)
#define R100_TX_ROUTE_ST1      (1U << 24)
#define R100_TX_MICRO_X2       (1U << 3)
#define R100_TX_MACRO_TILE     (1U << 2)
#define R100_TX_MAG_LINEAR     (1U << 0)
#define R100_TX_MIN_LINEAR     (1U << 1)
#define R100_TX_MIN_NEAREST_MIP_NEAREST (2U << 1)
#define R100_TX_MIN_MIP_NEAREST R100_TX_MIN_NEAREST_MIP_NEAREST
#define R100_TX_MIN_NEAREST_MIP_LINEAR (3U << 1)
#define R100_TX_MIN_LINEAR_MIP_NEAREST (6U << 1)
#define R100_TX_MIN_LINEAR_MIP_LINEAR (7U << 1)
#define R100_TX_LOD_BIAS(value) ((uint32_t)(value) << 8)
#define R100_TX_MAX_MIP_LEVEL(value) ((uint32_t)(value) << 16)
#define R100_TX_YUV_TO_RGB     (1U << 20)
#define R100_TX_LINEAR         (R100_TX_MAG_LINEAR | R100_TX_MIN_LINEAR)
#define R100_TX_CLAMP_S(mode)  ((uint32_t)(mode) << 23)
#define R100_TX_CLAMP_T(mode)  ((uint32_t)(mode) << 27)
#define R100_TX_BORDER_D3D     (1U << 31)
#define R100_COMBINER_CLAMP    (1U << 23)
#define R100_COORD_XY_PRE_MULT (1U << 0)
#define R100_COORD_Z_PRE_MULT  (1U << 1)
#define R100_COORD_ST0_NONPARAM (1U << 8)
#define R100_COORD_W0_IS_W     (1U << 16)
#define R100_COORD_ST1_NONPARAM (1U << 9)
#define R100_RB_RPTR_WR_ENA    (1U << 31)
#define R100_PCIGART_TRANSLATE_EN (1U << 0)
#define R100_DIS_OUT_OF_PCI_GART_ACCESS (1U << 1)

/* The seat's BARs; main() reads them before the cases build addresses. */
static uint64_t rv100_fb;
static uint64_t rv100_mmio;

static uint64_t rv100_bar(QTestState *qts, uint8_t reg)
{
    return qtest_readl(qts, RV100_CFG(reg)) & PCI_BASE_ADDRESS_MEM_MASK;
}

/* The R100 always tiles its depth buffer (XFree86 4.3 radeon_span.c). */
static uint64_t rv100_z32(unsigned int pitch, unsigned int x, unsigned int y)
{
    uint32_t ba = (y / 16) * (pitch / 16) + x / 16;
    uint32_t a = (x & 7) << 2;

    a |= (y & 3) << 5;
    a |= (((x & 0x10) >> 2) ^ (y & 4)) << 5;
    a |= (ba & 3) << 8;
    a |= (y & 8) << 7;
    a |= (((x & 8) << 1) ^ (y & 0x10)) << 7;
    return a | (uint64_t)(ba & ~3U) << 10;
}

/* Memory decode and bus master, as pci.sys and Linux leave the card. */
static void rv100_3d_enable(QTestState *qts)
{
    qtest_writew(qts, RV100_CFG(PCI_COMMAND),
                 PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
}

static QTestState *rv100_3d_init(const char *extra)
{
    QTestState *qts = qtest_initf("-machine zx1,vga=rv100 -m 256M -S "
                                  "-global ati-vga.x-r100-3d=on "
                                  "-global ati-vga.x-tiling=on %s", extra);

    g_assert_cmphex(rv100_bar(qts, PCI_BASE_ADDRESS_0), ==, rv100_fb);
    g_assert_cmphex(rv100_bar(qts, PCI_BASE_ADDRESS_2), ==, rv100_mmio);
    rv100_3d_enable(qts);
    return qts;
}

static void display_wait_for_migration_status(QTestState *qts,
                                              const char *wanted);
static void display_wait_for_migration(QTestState *qts);

static uint32_t f32_bits(float value)
{
    uint32_t bits;

    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

static void display_wait_for_u32(QTestState *qts, uint64_t address,
                                 uint32_t expected)
{
    int64_t deadline = g_get_monotonic_time() + 60 * G_TIME_SPAN_SECOND;
    uint32_t actual;

    do {
        actual = qtest_readl(qts, address);
        if (actual == expected) {
            return;
        }
        g_usleep(1000);
    } while (g_get_monotonic_time() < deadline);

    g_assert_cmphex(actual, ==, expected);
}

static void ati_rv100_draw_immediate(QTestState *qts, uint32_t format,
                                     uint32_t primitive,
                                     const uint32_t *data,
                                     size_t data_dwords,
                                     unsigned int vertex_count)
{
    uint32_t vf_cntl = primitive | R100_VF_WALK_DATA |
                       R100_VF_COLOR_RGBA | (vertex_count << 16);
    size_t i;

    qtest_writel(qts, rv100_mmio + R100_SE_VTX_FMT, format);
    qtest_writel(qts, rv100_mmio + R100_SE_VF_CNTL, vf_cntl);
    for (i = 0; i < data_dwords; i++) {
        qtest_writel(qts, rv100_mmio + R100_SE_PORT_DATA0,
                     data[i]);
    }
}

static uint32_t ati_rv100_rgba_vertex_color(uint32_t argb)
{
    return (argb & 0xff00ff00U) | ((argb & 0xffU) << 16) |
           ((argb >> 16) & 0xffU);
}

static void ati_rv100_3d_ring(void)
{
    enum {
        WIDTH = 64,
        HEIGHT = 64,
        TEXTURE_OFFSET = 0x10000,
        DEPTH_OFFSET = 0x20000,
        RING_OFFSET = 0x30000,
        IB_OFFSET = 0x31000,
        VERTEX_XYZ_OFFSET = 0x32000,
        VERTEX_COLOR_OFFSET = 0x32100,
        VERTEX_ST_OFFSET = 0x32200,
        RPTR_WRITEBACK_OFFSET = 0x32300,
        WRITEBACK_RING_OFFSET = 0x32400,
        RPTR_SCALED_DECOY_OFFSET = RPTR_WRITEBACK_OFFSET * 4,
    };
    const uint32_t texture_color = 0xff3366cc;
    const uint32_t marker = 0x52563130;
    const uint32_t vertex_format = R100_VTX_FMT_Z |
                                   R100_VTX_FMT_PKCOLOR |
                                   R100_VTX_FMT_ST0;
    const uint32_t vf_cntl = R100_VF_TRIANGLE_LIST |
                             R100_VF_WALK_DATA |
                             R100_VF_COLOR_RGBA | (3U << 16);
    const uint32_t indexed_vf_cntl = R100_VF_TRIANGLE_LIST |
                                     R100_VF_WALK_IND |
                                     R100_VF_COLOR_RGBA |
                                     R100_VF_INDEX_SIZE_32 | (6U << 16);
    uint32_t depth[WIDTH * HEIGHT];
    uint32_t vertex_xyz[] = {
        f32_bits(32.0f), f32_bits(32.0f), f32_bits(0.25f),
        f32_bits(56.0f), f32_bits(32.0f), f32_bits(0.25f),
        f32_bits(32.0f), f32_bits(56.0f), f32_bits(0.25f),
    };
    uint32_t vertex_color[] = { UINT32_MAX };
    uint32_t vertex_st[] = {
        f32_bits(0.75f), f32_bits(0.25f),
        f32_bits(0.75f), f32_bits(0.25f),
        f32_bits(0.75f), f32_bits(0.25f),
    };
    uint32_t ib[] = {
        R100_CP_PACKET3 | (19U << 16) | (R100_PACKET3_DRAW_IMMD << 8),
        vertex_format,
        vf_cntl,
        f32_bits(4.0f), f32_bits(4.0f), f32_bits(0.25f), UINT32_MAX,
        f32_bits(0.75f), f32_bits(0.25f),
        f32_bits(52.0f), f32_bits(4.0f), f32_bits(0.25f), UINT32_MAX,
        f32_bits(0.75f), f32_bits(0.25f),
        f32_bits(4.0f), f32_bits(52.0f), f32_bits(0.25f), UINT32_MAX,
        f32_bits(0.75f), f32_bits(0.25f),
        R100_CP_PACKET3 | (5U << 16) | (R100_PACKET3_LOAD_VBPNTR << 8),
        3,
        3U | (3U << 8) | (1U << 16),
        VERTEX_XYZ_OFFSET,
        VERTEX_COLOR_OFFSET,
        2U | (2U << 8),
        VERTEX_ST_OFFSET,
        /* Repeated indices exercise reuse within a single indexed draw. */
        R100_CP_PACKET3 | (7U << 16) | (R100_PACKET3_DRAW_INDX << 8),
        vertex_format,
        indexed_vf_cntl,
        0, 1, 2, 0, 1, 2,
    };
    uint32_t ring[] = {
        R100_SCRATCH_REG0 >> 2,
        marker,
        (1U << 16) | (R100_CP_IB_BASE >> 2),
        IB_OFFSET,
        ARRAY_SIZE(ib),
        ATI_GEN_INT_STATUS >> 2,
        ATI_SW_INT_FIRE,
    };
    uint32_t legacy_ib[] = {
        R100_CP_PACKET3 | (13U << 16) | (R100_PACKET3_RNDR_GEN_PRIM << 8),
        R100_VTX_FMT_ST0,
        R100_VF_RECTANGLE_LIST | R100_VF_WALK_DATA | (3U << 16),
        f32_bits(32.0f), f32_bits(4.0f), f32_bits(0.75f), f32_bits(0.25f),
        f32_bits(48.0f), f32_bits(4.0f), f32_bits(0.75f), f32_bits(0.25f),
        f32_bits(48.0f), f32_bits(20.0f), f32_bits(0.75f), f32_bits(0.25f),
        R100_SCRATCH_REG0 >> 2, marker + 1,
    };
    QTestState *qts;
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(depth); i++) {
        depth[i] = UINT32_MAX;
    }
    for (i = 0; i < ARRAY_SIZE(vertex_xyz); i++) {
        vertex_xyz[i] = cpu_to_le32(vertex_xyz[i]);
    }
    for (i = 0; i < ARRAY_SIZE(vertex_color); i++) {
        vertex_color[i] = cpu_to_le32(vertex_color[i]);
    }
    for (i = 0; i < ARRAY_SIZE(vertex_st); i++) {
        vertex_st[i] = cpu_to_le32(vertex_st[i]);
    }
    for (i = 0; i < ARRAY_SIZE(ib); i++) {
        ib[i] = cpu_to_le32(ib[i]);
    }
    for (i = 0; i < ARRAY_SIZE(ring); i++) {
        ring[i] = cpu_to_le32(ring[i]);
    }
    for (i = 0; i < ARRAY_SIZE(depth); i++) {
        depth[i] = cpu_to_le32(depth[i]);
    }

    qts = rv100_3d_init("");
    qtest_writel(qts, rv100_fb + TEXTURE_OFFSET + 4,
                 texture_color);
    qtest_memwrite(qts, rv100_fb + DEPTH_OFFSET,
                   depth, sizeof(depth));
    qtest_memwrite(qts, rv100_fb + RING_OFFSET,
                   ring, sizeof(ring));
    qtest_memwrite(qts, rv100_fb + IB_OFFSET, ib, sizeof(ib));
    qtest_memwrite(qts, rv100_fb + VERTEX_XYZ_OFFSET,
                   vertex_xyz, sizeof(vertex_xyz));
    qtest_memwrite(qts, rv100_fb + VERTEX_COLOR_OFFSET,
                   vertex_color, sizeof(vertex_color));
    qtest_memwrite(qts, rv100_fb + VERTEX_ST_OFFSET,
                   vertex_st, sizeof(vertex_st));
    qtest_writel(qts, rv100_fb + RPTR_WRITEBACK_OFFSET,
                 0xfeedface);
    qtest_writel(qts, rv100_fb + RPTR_SCALED_DECOY_OFFSET,
                 0xdeadbeef);

    qtest_writel(qts, rv100_mmio + R100_PP_CNTL,
                 R100_TX0_ENABLE | R100_TX_BLEND0_ENABLE);
    qtest_writel(qts, rv100_mmio + R100_RB3D_CNTL,
                 R100_RB_Z_ENABLE | R100_RB_COLOR_ARGB8888);
    qtest_writel(qts, rv100_mmio + R100_RB3D_COLOROFFSET, 0);
    qtest_writel(qts, rv100_mmio + R100_RB3D_COLORPITCH, WIDTH);
    qtest_writel(qts, rv100_mmio + R100_RB3D_DEPTHOFFSET,
                 DEPTH_OFFSET);
    qtest_writel(qts, rv100_mmio + R100_RB3D_DEPTHPITCH, WIDTH);
    qtest_writel(qts, rv100_mmio + R100_RB3D_ZSTENCILCNTL,
                 4 | (1U << 4) | R100_Z_WRITE_ENABLE);
    qtest_writel(qts, rv100_mmio + R100_RE_TOP_LEFT, 0);
    qtest_writel(qts, rv100_mmio + R100_RE_WIDTH_HEIGHT,
                 (HEIGHT - 1) << 16 | (WIDTH - 1));
    qtest_writel(qts, rv100_mmio + R100_SE_CNTL,
                 (3U << 1) | (3U << 3) | (2U << 8) | (2U << 10));
    qtest_writel(qts, rv100_mmio + R100_PP_TXFILTER_0, 0);
    qtest_writel(qts, rv100_mmio + R100_PP_TXFORMAT_0,
                 R100_TX_ARGB8888 | R100_TX_ALPHA_IN_MAP |
                 R100_TX_NON_POWER2);
    qtest_writel(qts, rv100_mmio + R100_PP_TXOFFSET_0,
                 TEXTURE_OFFSET);
    qtest_writel(qts, rv100_mmio + R100_PP_TEX_SIZE_0,
                 1U << 16 | 1U);
    qtest_writel(qts, rv100_mmio + R100_PP_TEX_PITCH_0, 0);
    qtest_writel(qts, rv100_mmio + R100_PP_TXCBLEND_0,
                 4U | (10U << 5) | R100_COMBINER_CLAMP);
    qtest_writel(qts, rv100_mmio + R100_PP_TXABLEND_0,
                 2U | (5U << 4) | R100_COMBINER_CLAMP);
    qtest_writel(qts, rv100_mmio + ATI_GEN_INT_CNTL,
                 ATI_SW_INT_ENABLE);
    qtest_writel(qts, rv100_mmio + R100_CP_ME_RAM_ADDR, 7);
    qtest_writel(qts, rv100_mmio + R100_CP_ME_RAM_DATAH,
                 0x01234567);
    g_assert_cmpuint(qtest_readl(qts, rv100_mmio +
                                 R100_CP_ME_RAM_ADDR), ==, 7);
    qtest_writel(qts, rv100_mmio + R100_CP_ME_RAM_DATAL,
                 0x89abcdef);
    g_assert_cmpuint(qtest_readl(qts, rv100_mmio +
                                 R100_CP_ME_RAM_ADDR), ==, 8);

    qtest_writel(qts, rv100_mmio + R100_CP_RB_BASE, RING_OFFSET);
    qtest_writel(qts, rv100_mmio + R100_CP_RB_CNTL,
                 R100_RB_RPTR_WR_ENA | 4);
    qtest_writel(qts, rv100_mmio + R100_CP_RB_RPTR_ADDR,
                 RPTR_WRITEBACK_OFFSET);
    qtest_writel(qts, rv100_mmio + R100_CP_RB_RPTR_WR, 0);
    qtest_writel(qts, rv100_mmio + R100_CP_CSQ_CNTL,
                 R100_CSQ_PRIBM_INDBM);
    qtest_writel(qts, rv100_mmio + R100_CP_RB_WPTR,
                 ARRAY_SIZE(ring));

    g_assert_cmphex(qtest_readl(qts, rv100_mmio +
                                R100_SCRATCH_REG0), ==, marker);
    g_assert_cmpuint(qtest_readl(qts, rv100_mmio +
                                 R100_CP_RB_RPTR), ==, ARRAY_SIZE(ring));
    g_assert_cmpuint(qtest_readl(qts, rv100_fb +
                                 RPTR_WRITEBACK_OFFSET), ==,
                     ARRAY_SIZE(ring));
    g_assert_cmphex(qtest_readl(qts, rv100_fb +
                                RPTR_SCALED_DECOY_OFFSET), ==, 0xdeadbeef);
    g_assert_cmphex(qtest_readl(qts, rv100_fb +
                                (10 * WIDTH + 10) * 4), ==, texture_color);
    g_assert_cmphex(qtest_readl(qts, rv100_fb +
                                (40 * WIDTH + 40) * 4), ==, texture_color);
    g_assert_cmpuint(qtest_readl(qts, rv100_fb + DEPTH_OFFSET +
                                 rv100_z32(WIDTH, 10, 10)), <, UINT32_MAX);
    g_assert_cmphex(qtest_readl(qts, rv100_mmio +
                                ATI_GEN_INT_STATUS) & ATI_SW_INT_TEST,
                    ==, ATI_SW_INT_TEST);
    qtest_writel(qts, rv100_mmio + ATI_GEN_INT_STATUS,
                 ATI_SW_INT_TEST);
    g_assert_cmphex(qtest_readl(qts, rv100_mmio +
                                ATI_GEN_INT_STATUS) & ATI_SW_INT_TEST,
                    ==, 0);

    /* Zero is valid; the low two register bits are not address bits. */
    qtest_writel(qts, rv100_fb + WRITEBACK_RING_OFFSET,
                 R100_CP_PACKET2);
    qtest_writel(qts, rv100_fb, 0xfeedface);
    qtest_writel(qts, rv100_mmio + R100_CP_RB_BASE,
                 WRITEBACK_RING_OFFSET);
    qtest_writel(qts, rv100_mmio + R100_CP_RB_RPTR_ADDR, 0);
    qtest_writel(qts, rv100_mmio + R100_CP_RB_RPTR_WR, 0);
    qtest_writel(qts, rv100_mmio + R100_CP_RB_WPTR, 1);
    g_assert_cmpuint(qtest_readl(qts, rv100_fb), ==, 1);

    /* Legacy opcode 0x25 uses Radeon XY/ST vertices without implicit Z. */
    for (i = 0; i < ARRAY_SIZE(legacy_ib); i++) {
        legacy_ib[i] = cpu_to_le32(legacy_ib[i]);
    }
    qtest_memset(qts, rv100_fb, 0, WIDTH * HEIGHT * 4);
    qtest_writel(qts, rv100_mmio + R100_RB3D_CNTL,
                 R100_RB_COLOR_ARGB8888);
    qtest_memwrite(qts, rv100_fb + IB_OFFSET,
                   legacy_ib, sizeof(legacy_ib));
    qtest_writel(qts, rv100_mmio + R100_CP_IB_BUFSZ,
                 ARRAY_SIZE(legacy_ib));
    g_assert_cmphex(qtest_readl(qts, rv100_mmio +
                                R100_SCRATCH_REG0), ==, marker + 1);
    g_assert_cmphex(qtest_readl(qts, rv100_fb +
                                (10 * WIDTH + 40) * 4), ==, texture_color);
    g_assert_cmphex(qtest_readl(qts, rv100_fb +
                                (10 * WIDTH + 31) * 4), ==, 0);
    g_assert_cmphex(qtest_readl(qts, rv100_fb +
                                (10 * WIDTH + 48) * 4), ==, 0);

    qtest_writel(qts, rv100_mmio + ATI_GEN_INT_STATUS,
                 ATI_SW_INT_FIRE);
    qtest_system_reset(qts);
    rv100_3d_enable(qts);
    g_assert_cmphex(qtest_readl(qts, rv100_mmio +
                                ATI_GEN_INT_STATUS), ==,
                    ATI_R100_GEN_INT_STATUS_RESET);
    g_assert_cmphex(qtest_readl(qts, rv100_mmio +
                                R100_SCRATCH_REG0), ==, 0);
    g_assert_cmphex(qtest_readl(qts, rv100_mmio +
                                R100_CP_RB_RPTR), ==, 0);
    qtest_quit(qts);
}

static void ati_rv100_vertex_fog(void)
{
    enum {
        WIDTH = 64,
        HEIGHT = 64,
    };
    const uint32_t fp_fog_point[] = {
        f32_bits(2.0f), f32_bits(2.0f),
        ati_rv100_rgba_vertex_color(0x20000000),
        f32_bits(2.0f), f32_bits(0.0f), f32_bits(0.0f),
        f32_bits(0.25f),
    };
    const uint32_t packed_fog_point[] = {
        f32_bits(4.0f), f32_bits(2.0f),
        ati_rv100_rgba_vertex_color(0x20ffffff), 0x80000000,
    };
    const uint32_t table_fog_points[] = {
        f32_bits(6.0f), f32_bits(2.0f), f32_bits(0.0f),
        ati_rv100_rgba_vertex_color(0x30506070), f32_bits(1.0f),
        f32_bits(8.0f), f32_bits(2.0f), f32_bits(64.5f / 256.0f),
        ati_rv100_rgba_vertex_color(0x30506070), f32_bits(0.0f),
        f32_bits(10.0f), f32_bits(2.0f), f32_bits(1.0f),
        ati_rv100_rgba_vertex_color(0x30506070), f32_bits(0.0f),
    };
    const uint32_t diffuse_table_fog_point[] = {
        f32_bits(12.0f), f32_bits(2.0f), f32_bits(0.0f),
        ati_rv100_rgba_vertex_color(0x30506070), f32_bits(0.0f),
    };
    const uint32_t spec_table_fog_point[] = {
        f32_bits(14.0f), f32_bits(2.0f), f32_bits(0.0f),
        ati_rv100_rgba_vertex_color(0x30506070), f32_bits(0.5f),
    };
    const uint32_t migrated_table_fog_point[] = {
        f32_bits(16.0f), f32_bits(2.0f), f32_bits(64.5f / 256.0f),
        ati_rv100_rgba_vertex_color(0x30506070),
    };
    const uint32_t migrated_table_index_point[] = {
        f32_bits(18.0f), f32_bits(2.0f), f32_bits(200.0f / 256.0f),
        ati_rv100_rgba_vertex_color(0x30506070),
    };
    const uint32_t fog_triangles[] = {
        f32_bits(4.0f), f32_bits(36.0f), UINT32_MAX, f32_bits(0.0f),
        f32_bits(20.0f), f32_bits(36.0f), UINT32_MAX, f32_bits(1.0f),
        f32_bits(4.0f), f32_bits(52.0f), UINT32_MAX, f32_bits(1.0f),
        f32_bits(36.0f), f32_bits(36.0f), UINT32_MAX, f32_bits(0.0f),
        f32_bits(36.0f), f32_bits(52.0f), UINT32_MAX, f32_bits(1.0f),
        f32_bits(52.0f), f32_bits(36.0f), UINT32_MAX, f32_bits(1.0f),
    };
    const uint64_t color_base = rv100_fb;
    const uint64_t mmio = rv100_mmio;
    g_autofree char *path = g_strdup_printf(
        "%s/ati-rv100-table-fog.XXXXXX", g_get_tmp_dir());
    g_autofree char *uri = NULL;
    QTestState *qts;
    int fd;

    qts = rv100_3d_init("");
    qtest_memset(qts, color_base, 0, WIDTH * HEIGHT * 4);
    qtest_writel(qts, mmio + R100_RB3D_COLOROFFSET, 0);
    qtest_writel(qts, mmio + R100_RB3D_COLORPITCH, WIDTH);
    qtest_writel(qts, mmio + R100_RE_TOP_LEFT, 0);
    qtest_writel(qts, mmio + R100_RE_WIDTH_HEIGHT,
                 (HEIGHT - 1) << 16 | (WIDTH - 1));
    qtest_writel(qts, mmio + R100_SE_CNTL,
                 (3U << 1) | (3U << 3) | (2U << 8) | (2U << 10) |
                 (2U << 14));
    qtest_writel(qts, mmio + R100_RB3D_CNTL, R100_RB_COLOR_ARGB8888);
    /* PP_FOG_COLOR's factor-source bits are unused in vertex-fog mode. */
    qtest_writel(qts, mmio + R100_PP_FOG_COLOR,
                 R100_FOG_USE_DIFFUSE_ALPHA | 0x000000ff);

    /* Color sum is clamped before fog; primary alpha bypasses fog unchanged. */
    qtest_writel(qts, mmio + R100_PP_CNTL,
                 R100_PP_SPECULAR_ENABLE | R100_PP_FOG_ENABLE);
    ati_rv100_draw_immediate(qts, R100_VTX_FMT_PKCOLOR |
                             R100_VTX_FMT_FPSPEC | R100_VTX_FMT_FPFOG,
                             R100_VF_POINT_LIST, fp_fog_point,
                             ARRAY_SIZE(fp_fog_point), 1);
    g_assert_cmphex(qtest_readl(qts, color_base + (2 * WIDTH + 2) * 4),
                    ==, 0x204000bf);

    /* Software TCL packs the same fog factor into PKSPEC alpha. */
    qtest_writel(qts, mmio + R100_PP_CNTL, R100_PP_FOG_ENABLE);
    ati_rv100_draw_immediate(qts, R100_VTX_FMT_PKCOLOR |
                             R100_VTX_FMT_PKSPEC, R100_VF_POINT_LIST,
                             packed_fog_point, ARRAY_SIZE(packed_fog_point),
                             1);
    g_assert_cmphex(qtest_readl(qts, color_base + (2 * WIDTH + 4) * 4),
                    ==, 0x208080ff);

    /* DATA's low 8 bits post-increment INDEX; Z interpolates table entries. */
    qtest_writel(qts, mmio + R100_FOG_TABLE_INDEX, 0);
    qtest_writel(qts, mmio + R100_FOG_TABLE_DATA, 0);
    qtest_writel(qts, mmio + R100_FOG_TABLE_INDEX, 48);
    qtest_writel(qts, mmio + R100_FOG_TABLE_DATA, 255);
    qtest_writel(qts, mmio + R100_FOG_TABLE_DATA, 255);
    qtest_writel(qts, mmio + R100_FOG_TABLE_INDEX, 64);
    qtest_writel(qts, mmio + R100_FOG_TABLE_DATA, 0xdeadbe40);
    qtest_writel(qts, mmio + R100_FOG_TABLE_DATA, 0xcafebac0);
    qtest_writel(qts, mmio + R100_FOG_TABLE_INDEX, 128);
    qtest_writel(qts, mmio + R100_FOG_TABLE_DATA, 255);
    qtest_writel(qts, mmio + R100_FOG_TABLE_DATA, 255);
    qtest_writel(qts, mmio + R100_FOG_TABLE_INDEX, 255);
    qtest_writel(qts, mmio + R100_FOG_TABLE_DATA, 255);
    qtest_writel(qts, mmio + R100_PP_FOG_COLOR,
                 R100_FOG_TABLE | 0x000000ff);
    ati_rv100_draw_immediate(qts, R100_VTX_FMT_Z | R100_VTX_FMT_PKCOLOR |
                             R100_VTX_FMT_FPFOG, R100_VF_POINT_LIST,
                             table_fog_points, ARRAY_SIZE(table_fog_points),
                             3);
    g_assert_cmphex(qtest_readl(qts, color_base + (2 * WIDTH + 6) * 4),
                    ==, 0x300000ff);
    g_assert_cmphex(qtest_readl(qts, color_base + (2 * WIDTH + 8) * 4),
                    ==, 0x302830b7);
    g_assert_cmphex(qtest_readl(qts, color_base + (2 * WIDTH + 10) * 4),
                    ==, 0x30506070);

    /* The table source field can select either interpolated color alpha. */
    qtest_writel(qts, mmio + R100_PP_FOG_COLOR,
                 R100_FOG_TABLE | R100_FOG_USE_DIFFUSE_ALPHA | 0x000000ff);
    ati_rv100_draw_immediate(qts, R100_VTX_FMT_Z | R100_VTX_FMT_PKCOLOR |
                             R100_VTX_FMT_FPFOG, R100_VF_POINT_LIST,
                             diffuse_table_fog_point,
                             ARRAY_SIZE(diffuse_table_fog_point), 1);
    g_assert_cmphex(qtest_readl(qts, color_base + (2 * WIDTH + 12) * 4),
                    ==, 0x30506070);
    qtest_writel(qts, mmio + R100_PP_FOG_COLOR,
                 R100_FOG_TABLE | R100_FOG_USE_SPEC_ALPHA | 0x000000ff);
    ati_rv100_draw_immediate(qts, R100_VTX_FMT_Z | R100_VTX_FMT_PKCOLOR |
                             R100_VTX_FMT_FPFOG, R100_VF_POINT_LIST,
                             spec_table_fog_point,
                             ARRAY_SIZE(spec_table_fog_point), 1);
    g_assert_cmphex(qtest_readl(qts, color_base + (2 * WIDTH + 14) * 4),
                    ==, 0x30506070);

    /* Both table RAM and its current write index survive migration. */
    qtest_writel(qts, mmio + R100_PP_FOG_COLOR,
                 R100_FOG_TABLE | 0x000000ff);
    qtest_writel(qts, mmio + R100_FOG_TABLE_INDEX, 200);
    fd = g_mkstemp(path);
    g_assert_cmpint(fd, >=, 0);
    close(fd);
    uri = g_strdup_printf("file:%s", path);
    qtest_qmp_assert_success(
        qts, "{'execute':'migrate','arguments':{'uri':%s}}", uri);
    display_wait_for_migration(qts);
    qtest_quit(qts);

    qts = rv100_3d_init("-incoming defer");
    qtest_qmp_assert_success(
        qts, "{'execute':'migrate-incoming','arguments':"
             "{'uri':%s,'exit-on-error':false}}", uri);
    display_wait_for_migration(qts);
    ati_rv100_draw_immediate(qts, R100_VTX_FMT_Z | R100_VTX_FMT_PKCOLOR,
                             R100_VF_POINT_LIST, migrated_table_fog_point,
                             ARRAY_SIZE(migrated_table_fog_point), 1);
    g_assert_cmphex(qtest_readl(qts, color_base + (2 * WIDTH + 16) * 4),
                    ==, 0x302830b7);
    qtest_writel(qts, mmio + R100_FOG_TABLE_DATA, 255);
    ati_rv100_draw_immediate(qts, R100_VTX_FMT_Z | R100_VTX_FMT_PKCOLOR,
                             R100_VF_POINT_LIST,
                             migrated_table_index_point,
                             ARRAY_SIZE(migrated_table_index_point), 1);
    g_assert_cmphex(qtest_readl(qts, color_base + (2 * WIDTH + 18) * 4),
                    ==, 0x30506070);

    /* Gouraud fog interpolation is independent of triangle winding. */
    qtest_writel(qts, mmio + R100_PP_FOG_COLOR, 0);
    ati_rv100_draw_immediate(qts, R100_VTX_FMT_PKCOLOR |
                             R100_VTX_FMT_FPFOG, R100_VF_TRIANGLE_LIST,
                             fog_triangles, ARRAY_SIZE(fog_triangles), 6);
    g_assert_cmphex(qtest_readl(qts, color_base + (40 * WIDTH + 8) * 4),
                    ==, 0xff8f8f8f);
    g_assert_cmphex(qtest_readl(qts, color_base + (40 * WIDTH + 40) * 4),
                    ==, 0xff8f8f8f);

    qtest_quit(qts);
    g_assert_cmpint(g_unlink(path), ==, 0);
}

static void ati_rv100_3d_live_migration(void)
{
    enum {
        WIDTH = 64,
        HEIGHT = 64,
        GPU_FB_BASE = 0x10000000,
        COLOR_OFFSET = 0x00004000,
        DEPTH_OFFSET = 0x00008000,
        TILED_COLOR_OFFSET = 0x0000c000,
        PARTIAL_DEPTH_OFFSET = 0x00010000,
        SPARSE_COLOR_OFFSET = 0x00020000,
        TILED_PIXEL_OFFSET = 272,
        SPARSE_PITCH = 2048,
        SPARSE_POINTS = 40,
        GART_BASE = 0x00100000,
        GART_COLOR_OFFSET = 0x100,
        PAGE_TABLE = 0x08000000,
        GART_PAGE = 0x08001000,
        SAMPLE_X = 6,
        SAMPLE_Y = 6,
        TILED_X = 4,
        TILED_Y = 2,
        GART_X = 8,
        GART_Y = 8,
        PARTIAL_X = 12,
        PARTIAL_Y = 12,
        SPARSE_X = 32,
    };
    const uint32_t color = 0xff13579b;
    const uint32_t tiled_color = 0xff2468ac;
    const uint32_t gart_color = 0xff55aa33;
    const uint32_t old_color = 0x11111111;
    const uint32_t old_depth = UINT32_MAX;
    const uint32_t old_tiled = 0x22222222;
    const uint32_t old_gart = 0x33333333;
    const uint32_t old_sparse_first = 0x44444444;
    const uint32_t old_sparse_last = 0x55555555;
    const uint32_t sparse_first = 0xff600000;
    const uint32_t sparse_last = 0xff600000 | (SPARSE_POINTS - 1);
    const uint32_t depth = 0xff3fffff;
    const uint32_t rectangle[] = {
        f32_bits(4.0f), f32_bits(24.0f), f32_bits(0.25f),
        ati_rv100_rgba_vertex_color(color),
        f32_bits(24.0f), f32_bits(24.0f), f32_bits(0.25f),
        ati_rv100_rgba_vertex_color(color),
        f32_bits(24.0f), f32_bits(4.0f), f32_bits(0.25f),
        ati_rv100_rgba_vertex_color(color),
    };
    const uint32_t tiled_point[] = {
        f32_bits(TILED_X), f32_bits(TILED_Y),
        ati_rv100_rgba_vertex_color(tiled_color),
    };
    const uint32_t gart_point[] = {
        f32_bits(GART_X), f32_bits(GART_Y),
        ati_rv100_rgba_vertex_color(gart_color),
    };
    const uint32_t partial_point[] = {
        f32_bits(PARTIAL_X), f32_bits(PARTIAL_Y), f32_bits(0.25f),
        ati_rv100_rgba_vertex_color(color),
    };
    uint32_t sparse_points[SPARSE_POINTS * 3];
    const uint64_t color_address = rv100_fb + COLOR_OFFSET +
        (SAMPLE_Y * WIDTH + SAMPLE_X) * sizeof(uint32_t);
    const uint64_t depth_address = rv100_fb + DEPTH_OFFSET +
        rv100_z32(WIDTH, SAMPLE_X, SAMPLE_Y);
    const uint64_t tiled_address = rv100_fb +
        TILED_COLOR_OFFSET + TILED_PIXEL_OFFSET;
    const uint64_t gart_address = GART_PAGE + GART_COLOR_OFFSET +
        (GART_Y * WIDTH + GART_X) * sizeof(uint32_t);
    const uint64_t partial_depth_address = rv100_fb +
        PARTIAL_DEPTH_OFFSET + rv100_z32(WIDTH, PARTIAL_X, PARTIAL_Y);
    const uint64_t sparse_first_address = rv100_fb +
        SPARSE_COLOR_OFFSET + SPARSE_X * sizeof(uint32_t);
    const uint64_t sparse_last_address = rv100_fb +
        SPARSE_COLOR_OFFSET +
        ((SPARSE_POINTS - 1) * SPARSE_PITCH + SPARSE_X) * sizeof(uint32_t);
    g_autoptr(GError) error = NULL;
    g_autofree char *tmpdir = NULL;
    g_autofree char *socket_path = NULL;
    g_autofree char *uri = NULL;
    QTestState *from;
    QTestState *to;
    uint64_t mmio = rv100_mmio;

    for (unsigned int i = 0; i < SPARSE_POINTS; i++) {
        uint32_t sparse_color = sparse_first | i;

        sparse_points[i * 3] = f32_bits(SPARSE_X);
        sparse_points[i * 3 + 1] = f32_bits(i);
        sparse_points[i * 3 + 2] =
            ati_rv100_rgba_vertex_color(sparse_color);
    }

    tmpdir = g_dir_make_tmp("ati-rv100-3d-live-migration-XXXXXX", &error);
    g_assert_no_error(error);
    g_assert_nonnull(tmpdir);
    socket_path = g_build_filename(tmpdir, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket_path);

    from = rv100_3d_init("");
    to = rv100_3d_init("-incoming defer");
    rv100_3d_enable(from);
    rv100_3d_enable(to);

    qtest_writel(from, color_address, old_color);
    qtest_writel(from, depth_address, old_depth);
    qtest_writel(from, tiled_address, old_tiled);
    qtest_writel(from, partial_depth_address, old_depth);
    qtest_writel(from, sparse_first_address, old_sparse_first);
    qtest_writel(from, sparse_last_address, old_sparse_last);
    qtest_writel(from, PAGE_TABLE, GART_PAGE);
    qtest_writel(from, gart_address, old_gart);
    qtest_writel(from, mmio + R100_MC_FB_LOCATION,
                 (0x10ffU << 16) | 0x1000U);
    qtest_writel(from, mmio + R100_AIC_PT_BASE, PAGE_TABLE);
    qtest_writel(from, mmio + R100_AIC_LO_ADDR, GART_BASE);
    qtest_writel(from, mmio + R100_AIC_HI_ADDR, GART_BASE + 0xfff);
    qtest_writel(from, mmio + R100_AIC_CNTL,
                 R100_PCIGART_TRANSLATE_EN |
                 R100_DIS_OUT_OF_PCI_GART_ACCESS);
    qtest_writel(from, mmio + R100_RB3D_COLOROFFSET,
                 GPU_FB_BASE + COLOR_OFFSET);
    qtest_writel(from, mmio + R100_RB3D_COLORPITCH, WIDTH);
    qtest_writel(from, mmio + R100_RB3D_DEPTHOFFSET,
                 GPU_FB_BASE + DEPTH_OFFSET);
    qtest_writel(from, mmio + R100_RB3D_DEPTHPITCH, WIDTH);
    qtest_writel(from, mmio + R100_RB3D_ZSTENCILCNTL,
                 2U | (7U << 4) | R100_Z_WRITE_ENABLE);
    qtest_writel(from, mmio + R100_RE_TOP_LEFT, 0);
    qtest_writel(from, mmio + R100_RE_WIDTH_HEIGHT,
                 (HEIGHT - 1) << 16 | (WIDTH - 1));
    qtest_writel(from, mmio + R100_SE_CNTL,
                 (3U << 1) | (3U << 3) | (2U << 8) | (2U << 10));
    qtest_writel(from, mmio + R100_PP_CNTL, 0);
    qtest_writel(from, mmio + R100_RB3D_CNTL,
                 R100_RB_COLOR_ARGB8888 | R100_RB_Z_ENABLE);

    qtest_qmp_assert_success(
        from, "{'execute':'migrate-set-capabilities','arguments':"
              " {'capabilities':[{'capability':"
              " 'pause-before-switchover','state':true}]}}");
    qtest_qmp_assert_success(
        to, "{'execute':'migrate-incoming','arguments':"
            " {'uri':%s,'exit-on-error':false}}", uri);
    qtest_qmp_assert_success(from, "{'execute':'cont'}");
    qtest_qmp_assert_success(
        from, "{'execute':'migrate','arguments':{'uri':%s}}", uri);
    display_wait_for_migration_status(from, "pre-switchover");

    /* The target must have received the old pages before the late draw. */
    display_wait_for_u32(to, color_address, old_color);
    display_wait_for_u32(to, depth_address, old_depth);
    display_wait_for_u32(to, tiled_address, old_tiled);
    display_wait_for_u32(to, gart_address, old_gart);
    display_wait_for_u32(to, partial_depth_address, old_depth);
    display_wait_for_u32(to, sparse_first_address, old_sparse_first);
    display_wait_for_u32(to, sparse_last_address, old_sparse_last);

    /* One batched draw dirties both linear color and depth VRAM ranges. */
    ati_rv100_draw_immediate(from, R100_VTX_FMT_Z |
                             R100_VTX_FMT_PKCOLOR,
                             R100_VF_RECTANGLE_LIST, rectangle,
                             ARRAY_SIZE(rectangle), 3);
    g_assert_cmphex(qtest_readl(from, color_address), ==, color);
    g_assert_cmphex(qtest_readl(from, depth_address), ==, depth);

    /* Tiled color writes must dirty the swizzled physical VRAM address. */
    qtest_writel(from, mmio + R100_RB3D_COLOROFFSET,
                 GPU_FB_BASE + TILED_COLOR_OFFSET);
    qtest_writel(from, mmio + R100_RB3D_COLORPITCH,
                 WIDTH | R100_COLOR_TILE_ENABLE);
    qtest_writel(from, mmio + R100_RB3D_CNTL, R100_RB_COLOR_ARGB8888);
    ati_rv100_draw_immediate(from, R100_VTX_FMT_PKCOLOR,
                             R100_VF_POINT_LIST, tiled_point,
                             ARRAY_SIZE(tiled_point), 1);
    g_assert_cmphex(qtest_readl(from, tiled_address), ==, tiled_color);

    /* GART targets remain PCI RAM writes and migrate through RAM dirtiness. */
    qtest_writel(from, mmio + R100_RB3D_COLOROFFSET,
                 GART_BASE + GART_COLOR_OFFSET);
    qtest_writel(from, mmio + R100_RB3D_COLORPITCH, WIDTH);
    ati_rv100_draw_immediate(from, R100_VTX_FMT_PKCOLOR,
                             R100_VF_POINT_LIST, gart_point,
                             ARRAY_SIZE(gart_point), 1);
    g_assert_cmphex(qtest_readl(from, gart_address), ==, gart_color);

    /* A later color fault must not lose an already batched depth write. */
    qtest_writel(from, mmio + R100_RB3D_COLOROFFSET, GART_BASE + 0x2000);
    qtest_writel(from, mmio + R100_RB3D_DEPTHOFFSET,
                 GPU_FB_BASE + PARTIAL_DEPTH_OFFSET);
    qtest_writel(from, mmio + R100_RB3D_CNTL,
                 R100_RB_COLOR_ARGB8888 | R100_RB_Z_ENABLE);
    ati_rv100_draw_immediate(from, R100_VTX_FMT_Z |
                             R100_VTX_FMT_PKCOLOR,
                             R100_VF_POINT_LIST, partial_point,
                             ARRAY_SIZE(partial_point), 1);
    g_assert_cmphex(qtest_readl(from, partial_depth_address), ==, depth);

    /*
     * Forty 8 KiB-pitched points occupy separate dirty pages.  This exceeds
     * the eight-range batch: the first page is flushed on capacity and the
     * last page by the final flush at the end of this single POINT_LIST.
     */
    qtest_writel(from, mmio + R100_RB3D_COLOROFFSET,
                 GPU_FB_BASE + SPARSE_COLOR_OFFSET);
    qtest_writel(from, mmio + R100_RB3D_COLORPITCH, SPARSE_PITCH);
    qtest_writel(from, mmio + R100_RB3D_CNTL, R100_RB_COLOR_ARGB8888);
    ati_rv100_draw_immediate(from, R100_VTX_FMT_PKCOLOR,
                             R100_VF_POINT_LIST, sparse_points,
                             ARRAY_SIZE(sparse_points), SPARSE_POINTS);
    g_assert_cmphex(qtest_readl(from, sparse_first_address), ==,
                    sparse_first);
    g_assert_cmphex(qtest_readl(from, sparse_last_address), ==, sparse_last);

    qtest_qmp_assert_success(
        from, "{'execute':'migrate-continue','arguments':"
              " {'state':'pre-switchover'}}");
    display_wait_for_migration(from);
    display_wait_for_migration(to);
    g_assert_cmphex(qtest_readl(to, color_address), ==, color);
    g_assert_cmphex(qtest_readl(to, depth_address), ==, depth);
    g_assert_cmphex(qtest_readl(to, tiled_address), ==, tiled_color);
    g_assert_cmphex(qtest_readl(to, gart_address), ==, gart_color);
    g_assert_cmphex(qtest_readl(to, partial_depth_address), ==, depth);
    g_assert_cmphex(qtest_readl(to, sparse_first_address), ==,
                    sparse_first);
    g_assert_cmphex(qtest_readl(to, sparse_last_address), ==, sparse_last);

    qtest_quit(from);
    qtest_quit(to);
    if (g_file_test(socket_path, G_FILE_TEST_EXISTS)) {
        g_assert_cmpint(g_unlink(socket_path), ==, 0);
    }
    g_assert_cmpint(g_rmdir(tmpdir), ==, 0);
}

static void ati_rv100_fixed_function(void)
{
    enum {
        WIDTH = 64,
        HEIGHT = 64,
        I8_OFFSET = 0x10000,
        Y8_OFFSET = 0x10100,
        AI88_OFFSET = 0x10180,
        TEX0_OFFSET = 0x10200,
        TEX1_OFFSET = 0x10300,
        MICRO_OFFSET = 0x10400,
        FILTER_OFFSET = 0x10500,
        MACRO_TEXTURE_OFFSET = 0x10600,
        MIRROR_TEXTURE_OFFSET = 0x10700,
        POT_TEXTURE_OFFSET = 0x10800,
        POT_MICRO_TEXTURE_OFFSET = 0x10900,
        DEPTH_OFFSET = 0x20000,
        TILED_COLOR_OFFSET = 0x24000,
    };
    const uint32_t replace_texture_color =
        (10U << 10) | R100_COMBINER_CLAMP;
    const uint32_t replace_texture_alpha =
        (5U << 8) | R100_COMBINER_CLAMP;
    const uint32_t rectangle[] = {
        f32_bits(4.0f), f32_bits(24.0f), UINT32_MAX,
        f32_bits(24.0f), f32_bits(24.0f), UINT32_MAX,
        f32_bits(24.0f), f32_bits(4.0f), UINT32_MAX,
    };
    const uint32_t additive_rectangle[] = {
        f32_bits(4.0f), f32_bits(24.0f), 0x10101010,
        f32_bits(24.0f), f32_bits(24.0f), 0x10101010,
        f32_bits(24.0f), f32_bits(4.0f), 0x10101010,
    };
    const uint32_t additive_triangles[] = {
        f32_bits(32.0f), f32_bits(24.0f), 0x10101010,
        f32_bits(52.0f), f32_bits(4.0f), 0x10101010,
        f32_bits(52.0f), f32_bits(24.0f), 0x10101010,
        f32_bits(32.0f), f32_bits(24.0f), 0x10101010,
        f32_bits(32.0f), f32_bits(4.0f), 0x10101010,
        f32_bits(52.0f), f32_bits(4.0f), 0x10101010,
    };
    const uint32_t perspective_i8[] = {
        f32_bits(30.0f), f32_bits(2.0f), UINT32_MAX,
        f32_bits(2.0f), f32_bits(0.0f), f32_bits(2.0f),
    };
    const uint32_t y8[] = {
        f32_bits(32.0f), f32_bits(2.0f), UINT32_MAX,
        f32_bits(0.0f), f32_bits(0.0f),
    };
    const uint32_t ai88[] = {
        f32_bits(34.0f), f32_bits(2.0f), UINT32_MAX,
        f32_bits(0.0f), f32_bits(0.0f),
    };
    const uint32_t stencil_pass[] = {
        f32_bits(40.0f), f32_bits(2.0f), f32_bits(0.25f), 0xff0000ff,
    };
    const uint32_t stencil_fail[] = {
        f32_bits(40.0f), f32_bits(2.0f), f32_bits(0.10f), 0xffff0000,
    };
    const uint32_t viewport_z[] = {
        f32_bits(42.0f), f32_bits(6.0f), f32_bits(0.5f),
        f32_bits(2.0f), UINT32_MAX,
    };
    const uint32_t multitexture[] = {
        f32_bits(50.0f), f32_bits(2.0f), UINT32_MAX,
        f32_bits(0.0f), f32_bits(0.0f),
        f32_bits(0.0f), f32_bits(0.0f),
    };
    const uint32_t microtiled[] = {
        f32_bits(60.0f), f32_bits(2.0f), UINT32_MAX,
        f32_bits(4.0f), f32_bits(0.0f),
    };
    const uint32_t clipped_point[] = {
        f32_bits(5.0f), f32_bits(31.0f), UINT32_MAX,
    };
    const uint32_t tiled_target[] = {
        f32_bits(4.0f), f32_bits(2.0f), 0xff0000ff,
    };
    const uint32_t filtered[] = {
        f32_bits(58.0f), f32_bits(10.0f), UINT32_MAX,
        f32_bits(1.0f), f32_bits(1.0f),
    };
    const uint32_t macro_texture[] = {
        f32_bits(56.0f), f32_bits(12.0f), UINT32_MAX,
        f32_bits(4.0f), f32_bits(2.0f),
    };
    const uint32_t pot_pitch[] = {
        f32_bits(54.0f), f32_bits(18.0f), UINT32_MAX,
        f32_bits(0.0f), f32_bits(1.0f),
    };
    const uint32_t pot_micro[] = {
        f32_bits(52.0f), f32_bits(18.0f), UINT32_MAX,
        f32_bits(0.0f), f32_bits(2.0f),
    };
    static const struct {
        uint32_t filter;
        float s;
        uint32_t expected;
    } nearest_address[] = {
        { R100_TX_CLAMP_S(0), 5.25f, 0xffff0000 },
        { R100_TX_CLAMP_S(0), -1.25f, 0xff00ff00 },
        { R100_TX_CLAMP_S(1), 5.25f, 0xff00ff00 },
        { R100_TX_CLAMP_S(1), -1.25f, 0xffff0000 },
        { R100_TX_CLAMP_S(1), 5.0f, 0xff0000ff },
        { R100_TX_CLAMP_S(1), -1.0f, 0xffff0000 },
        { R100_TX_CLAMP_S(2), 5.25f, 0xff0000ff },
        { R100_TX_CLAMP_S(2), -1.25f, 0xff000000 },
        { R100_TX_CLAMP_S(3), 5.25f, 0xff0000ff },
        { R100_TX_CLAMP_S(3), -1.25f, 0xffff0000 },
        { R100_TX_CLAMP_S(4), 5.25f, 0xffff00ff },
        { R100_TX_CLAMP_S(4), -1.25f, 0xff000000 },
        { R100_TX_CLAMP_S(5), 5.25f, 0xffff00ff },
        { R100_TX_CLAMP_S(5), -1.25f, 0xffff0000 },
        { R100_TX_CLAMP_S(6), 5.25f, 0xff0000ff },
        { R100_TX_CLAMP_S(6), -1.25f, 0xff000000 },
        { R100_TX_CLAMP_S(7), 5.25f, 0xff0000ff },
        { R100_TX_CLAMP_S(7), -1.25f, 0xffff0000 },
        { R100_TX_CLAMP_S(6) | R100_TX_BORDER_D3D,
          5.25f, 0xffff00ff },
        { R100_TX_CLAMP_S(6) | R100_TX_BORDER_D3D,
          -1.25f, 0xffff00ff },
        { R100_TX_CLAMP_S(7) | R100_TX_BORDER_D3D,
          5.25f, 0xffff00ff },
        { R100_TX_CLAMP_S(7) | R100_TX_BORDER_D3D,
          -1.25f, 0xffff0000 },
    };
    static const struct {
        uint32_t filter;
        float s;
        uint32_t expected;
    } linear_address[] = {
        { R100_TX_LINEAR | R100_TX_CLAMP_S(0),
          0.0f, 0xff000080 },
        { R100_TX_LINEAR | R100_TX_CLAMP_S(1),
          0.0f, 0xff000000 },
        { R100_TX_LINEAR | R100_TX_CLAMP_S(2),
          0.0f, 0xff000000 },
        { R100_TX_LINEAR | R100_TX_CLAMP_S(3),
          0.0f, 0xff000000 },
        { R100_TX_LINEAR | R100_TX_CLAMP_S(4),
          0.0f, 0xff800080 },
        { R100_TX_LINEAR | R100_TX_CLAMP_S(5),
          0.0f, 0xff000000 },
        { R100_TX_LINEAR | R100_TX_CLAMP_S(6),
          0.0f, 0xff800080 },
        { R100_TX_LINEAR | R100_TX_CLAMP_S(7),
          0.0f, 0xff000000 },
        { R100_TX_LINEAR | R100_TX_CLAMP_S(4),
          5.0f, 0xff8000ff },
        { R100_TX_LINEAR | R100_TX_CLAMP_S(5),
          5.0f, 0xff8000ff },
        { R100_TX_LINEAR | R100_TX_CLAMP_S(6),
          5.0f, 0xff8000ff },
        { R100_TX_LINEAR | R100_TX_CLAMP_S(7),
          5.0f, 0xff8000ff },
        { R100_TX_LINEAR | R100_TX_CLAMP_S(6) |
          R100_TX_BORDER_D3D, 5.0f, 0xffff00ff },
        { R100_TX_LINEAR | R100_TX_CLAMP_S(7) |
          R100_TX_BORDER_D3D, 5.0f, 0xffff00ff },
    };
    const uint32_t invalid_point[] = {
        0x7fc00000, f32_bits(40.0f), UINT32_MAX,
    };
    const uint32_t packed_n1_points[] = {
        f32_bits(10.0f), f32_bits(2.0f), UINT32_MAX, 0,
        f32_bits(12.0f), f32_bits(2.0f), UINT32_MAX, 0,
    };
    const uint32_t invalid_line[] = {
        f32_bits(0.0f), f32_bits(40.0f), UINT32_MAX,
        0x7f800000, f32_bits(40.0f), UINT32_MAX,
    };
    const uint32_t invalid_triangle[] = {
        f32_bits(1.0e30f), f32_bits(40.0f), UINT32_MAX,
        f32_bits(1.0e30f), f32_bits(41.0f), UINT32_MAX,
        f32_bits(1.0e30f), f32_bits(42.0f), UINT32_MAX,
    };
    const uint32_t clipped_line[] = {
        f32_bits(0.0f), f32_bits(32.0f), UINT32_MAX,
        f32_bits(20.0f), f32_bits(32.0f), UINT32_MAX,
    };
    const uint32_t scissored_rectangle[] = {
        f32_bits(0.0f), f32_bits(48.0f), 0xff00ff00,
        f32_bits(48.0f), f32_bits(48.0f), 0xff00ff00,
        f32_bits(48.0f), f32_bits(0.0f), 0xff00ff00,
    };
    const uint32_t unsupported_depth[] = {
        f32_bits(42.0f), f32_bits(18.0f), f32_bits(0.5f), UINT32_MAX,
    };
    const uint32_t flat_shade_line[] = {
        f32_bits(40.0f), f32_bits(20.0f), 0xff0000ff,
        f32_bits(44.0f), f32_bits(20.0f), 0xffff0000,
    };
    const uint32_t flat_shade_triangle[] = {
        f32_bits(40.0f), f32_bits(34.0f), 0xff0000ff,
        f32_bits(44.0f), f32_bits(34.0f), 0xff00ff00,
        f32_bits(40.0f), f32_bits(38.0f), 0xffff0000,
    };
    const uint32_t flat_shade_polygon[] = {
        f32_bits(48.0f), f32_bits(34.0f), 0xff0000ff,
        f32_bits(52.0f), f32_bits(34.0f), 0xff00ff00,
        f32_bits(52.0f), f32_bits(38.0f), 0xffff0000,
        f32_bits(48.0f), f32_bits(38.0f), 0xff00ffff,
    };
    const uint32_t driver_style_rectangle[] = {
        f32_bits(40.0f), f32_bits(26.0f),
        f32_bits(0.0f), f32_bits(1.0f),
        f32_bits(44.0f), f32_bits(26.0f),
        f32_bits(1.0f), f32_bits(1.0f),
        f32_bits(44.0f), f32_bits(22.0f),
        f32_bits(1.0f), f32_bits(0.0f),
    };
    const uint32_t flat_triangle_list[] = {
        f32_bits(40.0f), f32_bits(28.0f), 0xff0000ff,
        f32_bits(44.0f), f32_bits(28.0f), 0xff0000ff,
        f32_bits(40.0f), f32_bits(32.0f), 0xff0000ff,
        f32_bits(46.0f), f32_bits(28.0f), 0xff00ff00,
        f32_bits(50.0f), f32_bits(28.0f), 0xff00ff00,
        f32_bits(46.0f), f32_bits(32.0f), 0xff00ff00,
    };
    const uint32_t zero_gradient_triangle[] = {
        f32_bits(52.0f), f32_bits(28.0f),
        f32_bits(0.0f), f32_bits(0.0f),
        f32_bits(56.0f), f32_bits(28.0f),
        f32_bits(0.0f), f32_bits(0.0f),
        f32_bits(52.0f), f32_bits(32.0f),
        f32_bits(0.0f), f32_bits(0.0f),
    };
    const uint64_t color_base = rv100_fb;
    const uint64_t mmio = rv100_mmio;
    const uint64_t stencil_color = color_base + (2 * WIDTH + 40) * 4;
    const uint64_t stencil_depth = color_base + DEPTH_OFFSET +
                                   rv100_z32(WIDTH, 40, 2);
    uint32_t texture_point[5];
    QTestState *qts;
    unsigned int x;
    unsigned int y;
    size_t i;

    qts = rv100_3d_init("");
    qtest_memset(qts, color_base, 0, 0x30000);
    qtest_writel(qts, mmio + R100_RB3D_COLOROFFSET, 0);
    qtest_writel(qts, mmio + R100_RB3D_COLORPITCH, WIDTH);
    qtest_writel(qts, mmio + R100_RE_TOP_LEFT, 0);
    qtest_writel(qts, mmio + R100_RE_WIDTH_HEIGHT,
                 (HEIGHT - 1) << 16 | (WIDTH - 1));
    qtest_writel(qts, mmio + R100_SE_CNTL,
                 (3U << 1) | (3U << 3) | (2U << 8) | (2U << 10));
    qtest_writel(qts, mmio + R100_RB3D_CNTL, R100_RB_COLOR_ARGB8888);

    /* A rectangle list supplies three corners; hardware infers the fourth. */
    ati_rv100_draw_immediate(qts, R100_VTX_FMT_PKCOLOR,
                             R100_VF_RECTANGLE_LIST, rectangle,
                             ARRAY_SIZE(rectangle), 3);
    g_assert_cmphex(qtest_readl(qts, color_base +
                                (6 * WIDTH + 6) * 4), ==, UINT32_MAX);

    /* Shared polygon edges have single coverage, including R100 RECT_LIST. */
    qtest_memset(qts, color_base, 0, WIDTH * HEIGHT * 4);
    qtest_writel(qts, mmio + R100_RB3D_BLENDCNTL,
                 R100_SRC_BLEND_ONE | R100_DST_BLEND_ONE);
    qtest_writel(qts, mmio + R100_RB3D_CNTL,
                 R100_RB_ALPHA_BLEND | R100_RB_COLOR_ARGB8888);
    ati_rv100_draw_immediate(qts, R100_VTX_FMT_PKCOLOR,
                             R100_VF_RECTANGLE_LIST, additive_rectangle,
                             ARRAY_SIZE(additive_rectangle), 3);
    ati_rv100_draw_immediate(qts, R100_VTX_FMT_PKCOLOR,
                             R100_VF_TRIANGLE_LIST, additive_triangles,
                             ARRAY_SIZE(additive_triangles), 6);
    for (y = 4; y < 24; y++) {
        for (x = 4; x < 24; x++) {
            g_assert_cmphex(qtest_readl(qts, color_base +
                                            (y * WIDTH + x) * 4), ==,
                            0x10101010);
            g_assert_cmphex(qtest_readl(qts, color_base +
                                            (y * WIDTH + x + 28) * 4), ==,
                            0x10101010);
        }
    }
    g_assert_cmphex(qtest_readl(qts, color_base +
                                (24 * WIDTH + 4) * 4), ==, 0);
    g_assert_cmphex(qtest_readl(qts, color_base +
                                (4 * WIDTH + 24) * 4), ==, 0);
    g_assert_cmphex(qtest_readl(qts, color_base +
                                (4 * WIDTH + 52) * 4), ==, 0);
    qtest_writel(qts, mmio + R100_RB3D_CNTL, R100_RB_COLOR_ARGB8888);

    /* N1 is one packed dword; it must not shift the following vertex. */
    ati_rv100_draw_immediate(qts, R100_VTX_FMT_PKCOLOR | R100_VTX_FMT_N1,
                             R100_VF_POINT_LIST, packed_n1_points,
                             ARRAY_SIZE(packed_n1_points), 2);
    g_assert_cmphex(qtest_readl(qts, color_base +
                                (2 * WIDTH + 10) * 4), ==, UINT32_MAX);
    g_assert_cmphex(qtest_readl(qts, color_base +
                                (2 * WIDTH + 12) * 4), ==, UINT32_MAX);

    /* I8 carries alpha; Q0 perspective-divides non-parametric S. */
    qtest_writeb(qts, color_base + I8_OFFSET, 0x20);
    qtest_writeb(qts, color_base + I8_OFFSET + 1, 0x80);
    qtest_writel(qts, mmio + R100_PP_CNTL,
                 R100_TX0_ENABLE | R100_TX_BLEND0_ENABLE);
    qtest_writel(qts, mmio + R100_SE_COORD_FMT,
                 R100_COORD_ST0_NONPARAM);
    qtest_writel(qts, mmio + R100_PP_TXFILTER_0, 0);
    qtest_writel(qts, mmio + R100_PP_TXFORMAT_0,
                 R100_TX_I8 | R100_TX_ALPHA_IN_MAP |
                 R100_TX_NON_POWER2 | R100_TX_PERSPECTIVE);
    qtest_writel(qts, mmio + R100_PP_TXOFFSET_0, I8_OFFSET);
    qtest_writel(qts, mmio + R100_PP_TEX_SIZE_0, 1);
    qtest_writel(qts, mmio + R100_PP_TEX_PITCH_0, 0);
    qtest_writel(qts, mmio + R100_PP_TXCBLEND_0,
                 replace_texture_color);
    qtest_writel(qts, mmio + R100_PP_TXABLEND_0,
                 replace_texture_alpha);
    ati_rv100_draw_immediate(qts, R100_VTX_FMT_PKCOLOR |
                             R100_VTX_FMT_ST0 | R100_VTX_FMT_Q0,
                             R100_VF_POINT_LIST, perspective_i8,
                             ARRAY_SIZE(perspective_i8), 1);
    g_assert_cmphex(qtest_readl(qts, color_base +
                                (2 * WIDTH + 30) * 4), ==, 0x80808080);

    /* Y8 is an opaque luminance texture, distinct from intensity I8. */
    qtest_writeb(qts, color_base + Y8_OFFSET, 0x44);
    qtest_writel(qts, mmio + R100_PP_TXFORMAT_0,
                 R100_TX_Y8 | R100_TX_NON_POWER2);
    qtest_writel(qts, mmio + R100_PP_TXOFFSET_0, Y8_OFFSET);
    qtest_writel(qts, mmio + R100_PP_TEX_SIZE_0, 0);
    ati_rv100_draw_immediate(qts, R100_VTX_FMT_PKCOLOR |
                             R100_VTX_FMT_ST0,
                             R100_VF_POINT_LIST, y8,
                             ARRAY_SIZE(y8), 1);
    g_assert_cmphex(qtest_readl(qts, color_base +
                                (2 * WIDTH + 32) * 4), ==, 0xff444444);

    /* AI88 only exposes its stored alpha when ALPHA_IN_MAP is set. */
    qtest_writew(qts, color_base + AI88_OFFSET, 0x4080);
    qtest_writel(qts, mmio + R100_PP_TXFORMAT_0,
                 R100_TX_AI88 | R100_TX_NON_POWER2);
    qtest_writel(qts, mmio + R100_PP_TXOFFSET_0, AI88_OFFSET);
    ati_rv100_draw_immediate(qts, R100_VTX_FMT_PKCOLOR |
                             R100_VTX_FMT_ST0,
                             R100_VF_POINT_LIST, ai88,
                             ARRAY_SIZE(ai88), 1);
    g_assert_cmphex(qtest_readl(qts, color_base +
                                (2 * WIDTH + 34) * 4), ==, 0xff808080);
    qtest_writel(qts, mmio + R100_PP_TXFORMAT_0,
                 R100_TX_AI88 | R100_TX_ALPHA_IN_MAP |
                 R100_TX_NON_POWER2);
    ati_rv100_draw_immediate(qts, R100_VTX_FMT_PKCOLOR |
                             R100_VTX_FMT_ST0,
                             R100_VF_POINT_LIST, ai88,
                             ARRAY_SIZE(ai88), 1);
    g_assert_cmphex(qtest_readl(qts, color_base +
                                (2 * WIDTH + 34) * 4), ==, 0x40808080);

    /* NEVER rejects without changing color/depth, including textured input. */
    qtest_writel(qts, stencil_depth, 0x7f800000);
    qtest_writel(qts, stencil_color, 0xaabbccdd);
    qtest_writel(qts, mmio + R100_PP_CNTL, 1U << 4);
    qtest_writel(qts, mmio + R100_RB3D_DEPTHOFFSET, DEPTH_OFFSET);
    qtest_writel(qts, mmio + R100_RB3D_DEPTHPITCH, WIDTH);
    qtest_writel(qts, mmio + R100_RB3D_ZSTENCILCNTL,
                 2U | R100_Z_WRITE_ENABLE);
    qtest_writel(qts, mmio + R100_RB3D_CNTL,
                 R100_RB_COLOR_ARGB8888 | R100_RB_Z_ENABLE);
    ati_rv100_draw_immediate(qts, R100_VTX_FMT_Z | R100_VTX_FMT_PKCOLOR,
                             R100_VF_POINT_LIST, stencil_pass,
                             ARRAY_SIZE(stencil_pass), 1);
    g_assert_cmphex(qtest_readl(qts, stencil_depth), ==, 0x7f800000);
    g_assert_cmphex(qtest_readl(qts, stencil_color), ==, 0xaabbccdd);

    /* With stencil enabled, NEVER must still apply the depth-fail update. */
    qtest_writel(qts, mmio + R100_RB3D_STENCILREFMASK,
                 (0xffU << 16) | (0xffU << 24));
    qtest_writel(qts, mmio + R100_RB3D_ZSTENCILCNTL,
                 2U | (7U << 12) | (3U << 24) | R100_Z_WRITE_ENABLE);
    qtest_writel(qts, mmio + R100_RB3D_CNTL,
                 R100_RB_COLOR_ARGB8888 | R100_RB_Z_ENABLE |
                 R100_RB_STENCIL_ENABLE);
    ati_rv100_draw_immediate(qts, R100_VTX_FMT_Z | R100_VTX_FMT_PKCOLOR,
                             R100_VF_POINT_LIST, stencil_pass,
                             ARRAY_SIZE(stencil_pass), 1);
    g_assert_cmphex(qtest_readl(qts, stencil_depth), ==, 0x80800000);
    g_assert_cmphex(qtest_readl(qts, stencil_color), ==, 0xaabbccdd);

    /* D24S8: a passing fragment writes depth and increments stencil. */
    qtest_writel(qts, stencil_depth, 0x7f800000);
    qtest_writel(qts, mmio + R100_PP_CNTL, 0);
    qtest_writel(qts, mmio + R100_RB3D_DEPTHOFFSET, DEPTH_OFFSET);
    qtest_writel(qts, mmio + R100_RB3D_DEPTHPITCH, WIDTH);
    qtest_writel(qts, mmio + R100_RB3D_STENCILREFMASK,
                 (0xffU << 16) | (0xffU << 24));
    qtest_writel(qts, mmio + R100_RB3D_ZSTENCILCNTL,
                 2U | (1U << 4) | (7U << 12) | (3U << 20) |
                 R100_Z_WRITE_ENABLE);
    qtest_writel(qts, mmio + R100_RB3D_CNTL,
                 R100_RB_COLOR_ARGB8888 | R100_RB_Z_ENABLE |
                 R100_RB_STENCIL_ENABLE);
    ati_rv100_draw_immediate(qts, R100_VTX_FMT_Z |
                             R100_VTX_FMT_PKCOLOR,
                             R100_VF_POINT_LIST, stencil_pass,
                             ARRAY_SIZE(stencil_pass), 1);
    g_assert_cmphex(qtest_readl(qts, stencil_depth), ==, 0x803fffff);
    g_assert_cmphex(qtest_readl(qts, stencil_color), ==, 0xffff0000);

    /* A stencil failure applies REPLACE but preserves depth and color. */
    qtest_writel(qts, mmio + R100_RB3D_STENCILREFMASK,
                 0x12U | (0xffU << 16) | (0xffU << 24));
    qtest_writel(qts, mmio + R100_RB3D_ZSTENCILCNTL,
                 2U | (1U << 4) | (3U << 12) | (2U << 16) |
                 R100_Z_WRITE_ENABLE);
    ati_rv100_draw_immediate(qts, R100_VTX_FMT_Z |
                             R100_VTX_FMT_PKCOLOR,
                             R100_VF_POINT_LIST, stencil_fail,
                             ARRAY_SIZE(stencil_fail), 1);
    g_assert_cmphex(qtest_readl(qts, stencil_depth), ==, 0x123fffff);
    g_assert_cmphex(qtest_readl(qts, stencil_color), ==, 0xffff0000);

    /*
     * Viewport Z transformation divides clip-space Z by W before scaling;
     * SE_COORD_FMT says that W0 holds w and that x and y are projected.
     */
    qtest_writel(qts, color_base + DEPTH_OFFSET + rv100_z32(WIDTH, 42, 6),
                 UINT32_MAX);
    qtest_writel(qts, mmio + R100_SE_COORD_FMT,
                 R100_COORD_W0_IS_W | R100_COORD_XY_PRE_MULT);
    qtest_writel(qts, mmio + R100_SE_VPORT_ZSCALE, f32_bits(1.0f));
    qtest_writel(qts, mmio + R100_SE_VPORT_ZOFFSET, f32_bits(0.0f));
    qtest_writel(qts, mmio + R100_SE_CNTL,
                 (3U << 1) | (3U << 3) | (2U << 8) | (2U << 10) |
                 R100_VPORT_Z_XFORM);
    qtest_writel(qts, mmio + R100_RB3D_ZSTENCILCNTL,
                 2U | (7U << 4) | R100_Z_WRITE_ENABLE);
    qtest_writel(qts, mmio + R100_RB3D_CNTL,
                 R100_RB_COLOR_ARGB8888 | R100_RB_Z_ENABLE);
    ati_rv100_draw_immediate(qts, R100_VTX_FMT_W0 | R100_VTX_FMT_Z |
                             R100_VTX_FMT_PKCOLOR,
                             R100_VF_POINT_LIST, viewport_z,
                             ARRAY_SIZE(viewport_z), 1);
    g_assert_cmphex(qtest_readl(qts, color_base + DEPTH_OFFSET +
                                rv100_z32(WIDTH, 42, 6)), ==, 0xff3fffff);
    qtest_writel(qts, mmio + R100_SE_CNTL,
                 (3U << 1) | (3U << 3) | (2U << 8) | (2U << 10));

    /* Stage 0 replaces with T0; stage 1 adds routed ST1/T1. */
    qtest_writel(qts, color_base + TEX0_OFFSET, 0xffff0000);
    qtest_writel(qts, color_base + TEX1_OFFSET, 0xff0000ff);
    qtest_writel(qts, mmio + R100_RB3D_CNTL, R100_RB_COLOR_ARGB8888);
    qtest_writel(qts, mmio + R100_SE_COORD_FMT,
                 R100_COORD_ST0_NONPARAM | R100_COORD_ST1_NONPARAM);
    qtest_writel(qts, mmio + R100_PP_TXFORMAT_0,
                 R100_TX_ARGB8888 | R100_TX_ALPHA_IN_MAP |
                 R100_TX_NON_POWER2);
    qtest_writel(qts, mmio + R100_PP_TXOFFSET_0, TEX0_OFFSET);
    qtest_writel(qts, mmio + R100_PP_TEX_SIZE_0, 0);
    qtest_writel(qts, mmio + R100_PP_TXCBLEND_0,
                 replace_texture_color);
    qtest_writel(qts, mmio + R100_PP_TXABLEND_0,
                 replace_texture_alpha);
    qtest_writel(qts, mmio + R100_PP_TXFILTER_1, 0);
    qtest_writel(qts, mmio + R100_PP_TXFORMAT_1,
                 R100_TX_ARGB8888 | R100_TX_ALPHA_IN_MAP |
                 R100_TX_NON_POWER2 | R100_TX_ROUTE_ST1);
    qtest_writel(qts, mmio + R100_PP_TXOFFSET_1, TEX1_OFFSET);
    qtest_writel(qts, mmio + R100_PP_TEX_SIZE_1, 0);
    qtest_writel(qts, mmio + R100_PP_TEX_PITCH_1, 0);
    qtest_writel(qts, mmio + R100_PP_TXCBLEND_1,
                 2U | (1U << 16) | (12U << 10) |
                 R100_COMBINER_CLAMP);
    qtest_writel(qts, mmio + R100_PP_TXABLEND_1,
                 (1U << 8) | R100_COMBINER_CLAMP);
    qtest_writel(qts, mmio + R100_PP_CNTL,
                 R100_TX0_ENABLE | R100_TX1_ENABLE |
                 R100_TX_BLEND0_ENABLE | R100_TX_BLEND1_ENABLE);
    ati_rv100_draw_immediate(qts, R100_VTX_FMT_PKCOLOR |
                             R100_VTX_FMT_ST0 | R100_VTX_FMT_ST1,
                             R100_VF_POINT_LIST, multitexture,
                             ARRAY_SIZE(multitexture), 1);
    g_assert_cmphex(qtest_readl(qts, color_base +
                                (2 * WIDTH + 50) * 4), ==, 0xffff00ff);

    /* An 8x2 ARGB texture maps texel x=4 to the second 32-byte microtile. */
    qtest_writel(qts, color_base + MICRO_OFFSET + 16, 0xffff0000);
    qtest_writel(qts, color_base + MICRO_OFFSET + 32, 0xff00ff00);
    qtest_writel(qts, mmio + R100_SE_COORD_FMT,
                 R100_COORD_ST0_NONPARAM);
    qtest_writel(qts, mmio + R100_PP_TXFORMAT_0,
                 R100_TX_ARGB8888 | R100_TX_ALPHA_IN_MAP |
                 R100_TX_NON_POWER2);
    qtest_writel(qts, mmio + R100_PP_TXOFFSET_0,
                 MICRO_OFFSET | R100_TX_MICRO_X2);
    qtest_writel(qts, mmio + R100_PP_TEX_SIZE_0, (1U << 16) | 7U);
    qtest_writel(qts, mmio + R100_PP_CNTL,
                 R100_TX0_ENABLE | R100_TX_BLEND0_ENABLE);
    ati_rv100_draw_immediate(qts, R100_VTX_FMT_PKCOLOR |
                             R100_VTX_FMT_ST0,
                             R100_VF_POINT_LIST, microtiled,
                             ARRAY_SIZE(microtiled), 1);
    g_assert_cmphex(qtest_readl(qts, color_base +
                                (2 * WIDTH + 60) * 4), ==, 0xff00ff00);

    /* RB3D macro tiling swizzles (4, 2) away from its linear address. */
    qtest_writel(qts, color_base + TILED_COLOR_OFFSET + 528, 0x11223344);
    qtest_writel(qts, mmio + R100_PP_CNTL, 0);
    qtest_writel(qts, mmio + R100_RB3D_COLOROFFSET, TILED_COLOR_OFFSET);
    qtest_writel(qts, mmio + R100_RB3D_COLORPITCH,
                 WIDTH | R100_COLOR_TILE_ENABLE);
    ati_rv100_draw_immediate(qts, R100_VTX_FMT_PKCOLOR,
                             R100_VF_POINT_LIST, tiled_target,
                             ARRAY_SIZE(tiled_target), 1);
    g_assert_cmphex(qtest_readl(qts, color_base +
                                TILED_COLOR_OFFSET + 272), ==, 0xffff0000);
    g_assert_cmphex(qtest_readl(qts, color_base +
                                TILED_COLOR_OFFSET + 528), ==, 0x11223344);

    /* Linear filtering at the four-texel center averages all four colors. */
    qtest_writel(qts, color_base + FILTER_OFFSET, 0xffff0000);
    qtest_writel(qts, color_base + FILTER_OFFSET + 4, 0xff00ff00);
    qtest_writel(qts, color_base + FILTER_OFFSET + 32, 0xff0000ff);
    qtest_writel(qts, color_base + FILTER_OFFSET + 36, UINT32_MAX);
    qtest_writel(qts, mmio + R100_RB3D_COLOROFFSET, 0);
    qtest_writel(qts, mmio + R100_RB3D_COLORPITCH, WIDTH);
    qtest_writel(qts, mmio + R100_RE_TOP_LEFT, 0);
    qtest_writel(qts, mmio + R100_RE_WIDTH_HEIGHT,
                 (HEIGHT - 1) << 16 | (WIDTH - 1));
    qtest_writel(qts, mmio + R100_SE_COORD_FMT,
                 R100_COORD_ST0_NONPARAM);
    qtest_writel(qts, mmio + R100_PP_TXFILTER_0, R100_TX_LINEAR);
    qtest_writel(qts, mmio + R100_PP_TXFORMAT_0,
                 R100_TX_ARGB8888 | R100_TX_ALPHA_IN_MAP |
                 R100_TX_NON_POWER2);
    qtest_writel(qts, mmio + R100_PP_TXOFFSET_0, FILTER_OFFSET);
    qtest_writel(qts, mmio + R100_PP_TEX_SIZE_0, (1U << 16) | 1U);
    qtest_writel(qts, mmio + R100_PP_CNTL,
                 R100_TX0_ENABLE | R100_TX_BLEND0_ENABLE);
    ati_rv100_draw_immediate(qts, R100_VTX_FMT_PKCOLOR |
                             R100_VTX_FMT_ST0,
                             R100_VF_POINT_LIST, filtered,
                             ARRAY_SIZE(filtered), 1);
    g_assert_cmphex(qtest_readl(qts, color_base +
                                (10 * WIDTH + 58) * 4), ==, 0xff808080);

    /* A macro-tiled texture uses its swizzled, not linear, texel address. */
    qtest_writel(qts, color_base + MACRO_TEXTURE_OFFSET + 272, 0xff00ff00);
    qtest_writel(qts, color_base + MACRO_TEXTURE_OFFSET + 528, 0xffff0000);
    qtest_writel(qts, mmio + R100_PP_TXFILTER_0, 0);
    qtest_writel(qts, mmio + R100_PP_TXFORMAT_0,
                 R100_TX_ARGB8888 | R100_TX_ALPHA_IN_MAP |
                 R100_TX_NON_POWER2);
    qtest_writel(qts, mmio + R100_PP_TXOFFSET_0,
                 MACRO_TEXTURE_OFFSET | R100_TX_MACRO_TILE);
    qtest_writel(qts, mmio + R100_PP_TEX_SIZE_0, (7U << 16) | 63U);
    qtest_writel(qts, mmio + R100_PP_TEX_PITCH_0, 256 - 32);
    ati_rv100_draw_immediate(qts, R100_VTX_FMT_PKCOLOR |
                             R100_VTX_FMT_ST0,
                             R100_VF_POINT_LIST, macro_texture,
                             ARRAY_SIZE(macro_texture), 1);
    g_assert_cmphex(qtest_readl(qts, color_base +
                                (12 * WIDTH + 56) * 4), ==, 0xff00ff00);

    /* Linear POT texture rows retain the R100 32-byte pitch alignment. */
    qtest_writel(qts, color_base + POT_TEXTURE_OFFSET + 16, 0xffff0000);
    qtest_writel(qts, color_base + POT_TEXTURE_OFFSET + 32, 0xff00ff00);
    qtest_writel(qts, mmio + R100_PP_TXFORMAT_0,
                 R100_TX_ARGB8888 | R100_TX_ALPHA_IN_MAP |
                 (2U << 8) | (1U << 12));
    qtest_writel(qts, mmio + R100_PP_TXOFFSET_0, POT_TEXTURE_OFFSET);
    qtest_writel(qts, mmio + R100_PP_TXFILTER_0, 0);
    ati_rv100_draw_immediate(qts, R100_VTX_FMT_PKCOLOR |
                             R100_VTX_FMT_ST0,
                             R100_VF_POINT_LIST, pot_pitch,
                             ARRAY_SIZE(pot_pitch), 1);
    g_assert_cmphex(qtest_readl(qts, color_base +
                                (18 * WIDTH + 54) * 4), ==, 0xff00ff00);

    /* POT microtiled rows retain the same 32-byte pitch alignment. */
    qtest_writel(qts, color_base + POT_MICRO_TEXTURE_OFFSET + 8,
                 0xffff0000);
    qtest_writel(qts, color_base + POT_MICRO_TEXTURE_OFFSET + 64,
                 0xff00ff00);
    qtest_writel(qts, mmio + R100_PP_TXFORMAT_0,
                 R100_TX_ARGB8888 | R100_TX_ALPHA_IN_MAP | (2U << 12));
    qtest_writel(qts, mmio + R100_PP_TXOFFSET_0,
                 POT_MICRO_TEXTURE_OFFSET | R100_TX_MICRO_X2);
    ati_rv100_draw_immediate(qts, R100_VTX_FMT_PKCOLOR |
                             R100_VTX_FMT_ST0,
                             R100_VF_POINT_LIST, pot_micro,
                             ARRAY_SIZE(pot_micro), 1);
    g_assert_cmphex(qtest_readl(qts, color_base +
                                (18 * WIDTH + 52) * 4), ==, 0xff00ff00);

    /* Exercise all R100 address modes with a power-of-two texture. */
    qtest_writel(qts, color_base + MIRROR_TEXTURE_OFFSET, 0xff000000);
    qtest_writel(qts, color_base + MIRROR_TEXTURE_OFFSET + 4, 0xffff0000);
    qtest_writel(qts, color_base + MIRROR_TEXTURE_OFFSET + 8, 0xff00ff00);
    qtest_writel(qts, color_base + MIRROR_TEXTURE_OFFSET + 12, 0xff0000ff);
    qtest_writel(qts, mmio + R100_PP_TXFORMAT_0,
                 R100_TX_ARGB8888 | R100_TX_ALPHA_IN_MAP | (2U << 8));
    qtest_writel(qts, mmio + R100_PP_TXOFFSET_0, MIRROR_TEXTURE_OFFSET);
    qtest_writel(qts, mmio + R100_PP_BORDER_COLOR_0, 0xffff00ff);
    for (i = 0; i < ARRAY_SIZE(nearest_address); i++) {
        texture_point[0] = f32_bits(2.0f + i);
        texture_point[1] = f32_bits(14.0f);
        texture_point[2] = UINT32_MAX;
        texture_point[3] = f32_bits(nearest_address[i].s);
        texture_point[4] = f32_bits(0.0f);
        qtest_writel(qts, mmio + R100_PP_TXFILTER_0,
                     nearest_address[i].filter);
        ati_rv100_draw_immediate(qts, R100_VTX_FMT_PKCOLOR |
                                 R100_VTX_FMT_ST0,
                                 R100_VF_POINT_LIST, texture_point,
                                 ARRAY_SIZE(texture_point), 1);
        g_assert_cmphex(qtest_readl(qts, color_base +
                                    sizeof(uint32_t) *
                                    (14 * WIDTH + 2 + i)), ==,
                        nearest_address[i].expected);
    }

    /* Normalized GL clamp coordinates select the last texel at s == 1. */
    qtest_writel(qts, mmio + R100_SE_COORD_FMT, 0);
    for (i = 0; i < 4; i++) {
        texture_point[0] = f32_bits(24.0f + i);
        texture_point[1] = f32_bits(14.0f);
        texture_point[2] = UINT32_MAX;
        texture_point[3] = f32_bits(1.0f);
        texture_point[4] = f32_bits(0.0f);
        qtest_writel(qts, mmio + R100_PP_TXFILTER_0,
                     R100_TX_CLAMP_S(6 + i % 2) |
                     (i >= 2 ? R100_TX_BORDER_D3D : 0));
        ati_rv100_draw_immediate(qts, R100_VTX_FMT_PKCOLOR |
                                 R100_VTX_FMT_ST0,
                                 R100_VF_POINT_LIST, texture_point,
                                 ARRAY_SIZE(texture_point), 1);
        g_assert_cmphex(qtest_readl(qts, color_base +
                                    sizeof(uint32_t) *
                                    (14 * WIDTH + 24 + i)), ==,
                        0xff0000ff);
    }
    qtest_writel(qts, mmio + R100_SE_COORD_FMT,
                 R100_COORD_ST0_NONPARAM);

    for (i = 0; i < ARRAY_SIZE(linear_address); i++) {
        texture_point[0] = f32_bits(2.0f + i);
        texture_point[1] = f32_bits(16.0f);
        texture_point[2] = UINT32_MAX;
        texture_point[3] = f32_bits(linear_address[i].s);
        texture_point[4] = f32_bits(0.0f);
        qtest_writel(qts, mmio + R100_PP_TXFILTER_0,
                     linear_address[i].filter);
        ati_rv100_draw_immediate(qts, R100_VTX_FMT_PKCOLOR |
                                 R100_VTX_FMT_ST0,
                                 R100_VF_POINT_LIST, texture_point,
                                 ARRAY_SIZE(texture_point), 1);
        g_assert_cmphex(qtest_readl(qts, color_base +
                                    sizeof(uint32_t) *
                                    (16 * WIDTH + 2 + i)), ==,
                        linear_address[i].expected);
    }

    /* Border colors are always supplied as ARGB8888. */
    qtest_writel(qts, mmio + R100_PP_BORDER_COLOR_0, 0x80402010);
    texture_point[0] = f32_bits(2.0f);
    texture_point[1] = f32_bits(18.0f);
    texture_point[2] = UINT32_MAX;
    texture_point[3] = f32_bits(-1.25f);
    texture_point[4] = f32_bits(0.0f);
    qtest_writel(qts, mmio + R100_PP_TXFILTER_0,
                 R100_TX_CLAMP_S(6) | R100_TX_BORDER_D3D);
    ati_rv100_draw_immediate(qts, R100_VTX_FMT_PKCOLOR |
                             R100_VTX_FMT_ST0,
                             R100_VF_POINT_LIST, texture_point,
                             ARRAY_SIZE(texture_point), 1);
    g_assert_cmphex(qtest_readl(qts, color_base +
                                (18 * WIDTH + 2) * 4), ==, 0x80402010);

    /* A one-texel T axis still returns border outside its coordinate range. */
    texture_point[0] = f32_bits(3.0f);
    texture_point[3] = f32_bits(0.0f);
    texture_point[4] = f32_bits(-1.25f);
    qtest_writel(qts, mmio + R100_PP_TXFILTER_0,
                 R100_TX_CLAMP_T(6) | R100_TX_BORDER_D3D);
    ati_rv100_draw_immediate(qts, R100_VTX_FMT_PKCOLOR |
                             R100_VTX_FMT_ST0,
                             R100_VF_POINT_LIST, texture_point,
                             ARRAY_SIZE(texture_point), 1);
    g_assert_cmphex(qtest_readl(qts, color_base +
                                (18 * WIDTH + 3) * 4), ==, 0x80402010);

    /* MIN and MAG remain independent; point sampling uses the MAG filter. */
    qtest_writel(qts, color_base + (18 * WIDTH + 40) * 4, 0);
    texture_point[0] = f32_bits(40.0f);
    texture_point[1] = f32_bits(18.0f);
    texture_point[2] = UINT32_MAX;
    texture_point[3] = f32_bits(0.0f);
    texture_point[4] = f32_bits(0.0f);
    qtest_writel(qts, mmio + R100_PP_TXFILTER_0, R100_TX_MAG_LINEAR);
    ati_rv100_draw_immediate(qts, R100_VTX_FMT_PKCOLOR |
                             R100_VTX_FMT_ST0,
                             R100_VF_POINT_LIST, texture_point,
                             ARRAY_SIZE(texture_point), 1);
    g_assert_cmphex(qtest_readl(qts, color_base +
                                (18 * WIDTH + 40) * 4), ==, 0xff000080);

    qtest_writel(qts, color_base + (18 * WIDTH + 41) * 4, 0);
    texture_point[0] = f32_bits(41.0f);
    qtest_writel(qts, mmio + R100_PP_TXFILTER_0, R100_TX_MIN_LINEAR);
    ati_rv100_draw_immediate(qts, R100_VTX_FMT_PKCOLOR |
                             R100_VTX_FMT_ST0,
                             R100_VF_POINT_LIST, texture_point,
                             ARRAY_SIZE(texture_point), 1);
    g_assert_cmphex(qtest_readl(qts, color_base +
                                (18 * WIDTH + 41) * 4), ==, 0xff000000);

    /* A point has no texture gradients, so mipmap MIN still uses MAG. */
    qtest_writel(qts, color_base + (18 * WIDTH + 39) * 4, 0);
    texture_point[0] = f32_bits(39.0f);
    qtest_writel(qts, mmio + R100_PP_TXFILTER_0,
                 R100_TX_MIN_MIP_NEAREST);
    ati_rv100_draw_immediate(qts, R100_VTX_FMT_PKCOLOR |
                             R100_VTX_FMT_ST0,
                             R100_VF_POINT_LIST, texture_point,
                             ARRAY_SIZE(texture_point), 1);
    g_assert_cmphex(qtest_readl(qts, color_base +
                                (18 * WIDTH + 39) * 4), ==, 0xff000000);

    /* Zero-gradient triangles also use MAG regardless of their MIN mode. */
    qtest_writel(qts, color_base + (29 * WIDTH + 53) * 4, 0x11223344);
    qtest_writel(qts, mmio + R100_PP_TXFILTER_0, R100_TX_MAG_LINEAR);
    ati_rv100_draw_immediate(qts, R100_VTX_FMT_ST0,
                             R100_VF_TRIANGLE_LIST,
                             zero_gradient_triangle,
                             ARRAY_SIZE(zero_gradient_triangle), 3);
    g_assert_cmphex(qtest_readl(qts, color_base +
                                (29 * WIDTH + 53) * 4), ==, 0xff000080);
    qtest_writel(qts, color_base + (29 * WIDTH + 53) * 4, 0x22334455);
    qtest_writel(qts, mmio + R100_PP_TXFILTER_0,
                 R100_TX_MIN_MIP_NEAREST);
    ati_rv100_draw_immediate(qts, R100_VTX_FMT_ST0,
                             R100_VF_TRIANGLE_LIST,
                             zero_gradient_triangle,
                             ARRAY_SIZE(zero_gradient_triangle), 3);
    g_assert_cmphex(qtest_readl(qts, color_base +
                                (29 * WIDTH + 53) * 4), ==, 0xff000000);

    qtest_writel(qts, color_base + (18 * WIDTH + 42) * 4, 0x22334455);
    qtest_writel(qts, color_base + DEPTH_OFFSET + rv100_z32(WIDTH, 42, 18),
                 0x33445566);
    qtest_writel(qts, mmio + R100_PP_CNTL, 0);
    qtest_writel(qts, mmio + R100_RB3D_DEPTHOFFSET, DEPTH_OFFSET);
    qtest_writel(qts, mmio + R100_RB3D_DEPTHPITCH, WIDTH);
    qtest_writel(qts, mmio + R100_RB3D_ZSTENCILCNTL,
                 3U | (7U << 4) | R100_Z_WRITE_ENABLE);
    qtest_writel(qts, mmio + R100_RB3D_CNTL,
                 R100_RB_COLOR_ARGB8888 | R100_RB_Z_ENABLE);
    ati_rv100_draw_immediate(qts, R100_VTX_FMT_Z |
                             R100_VTX_FMT_PKCOLOR,
                             R100_VF_POINT_LIST, unsupported_depth,
                             ARRAY_SIZE(unsupported_depth), 1);
    g_assert_cmphex(qtest_readl(qts, color_base +
                                (18 * WIDTH + 42) * 4), ==, 0x22334455);
    g_assert_cmphex(qtest_readl(qts, color_base + DEPTH_OFFSET +
                                rv100_z32(WIDTH, 42, 18)), ==, 0x33445566);

    /* Flat shading selects one provoking vertex instead of rejecting it. */
    qtest_writel(qts, color_base + (20 * WIDTH + 42) * 4, 0);
    qtest_writel(qts, mmio + R100_RB3D_CNTL, R100_RB_COLOR_ARGB8888);
    qtest_writel(qts, mmio + R100_SE_CNTL,
                 (3U << 1) | (3U << 3) | (1U << 8) | (2U << 10));
    ati_rv100_draw_immediate(qts, R100_VTX_FMT_PKCOLOR,
                             R100_VF_LINE_LIST, flat_shade_line,
                             ARRAY_SIZE(flat_shade_line), 2);
    g_assert_cmphex(qtest_readl(qts, color_base +
                                (20 * WIDTH + 42) * 4), ==, 0xffff0000);
    qtest_writel(qts, color_base + (20 * WIDTH + 42) * 4, 0);
    qtest_writel(qts, mmio + R100_SE_CNTL,
                 (3U << 1) | (3U << 3) | (3U << 6) |
                 (1U << 8) | (2U << 10));
    ati_rv100_draw_immediate(qts, R100_VTX_FMT_PKCOLOR,
                             R100_VF_LINE_LIST, flat_shade_line,
                             ARRAY_SIZE(flat_shade_line), 2);
    g_assert_cmphex(qtest_readl(qts, color_base +
                                (20 * WIDTH + 42) * 4), ==, 0xff0000ff);

    /* VTX_1 and VTX_2 are distinct selectors for polygonal primitives. */
    qtest_writel(qts, mmio + R100_SE_CNTL,
                 (3U << 1) | (3U << 3) | (1U << 6) |
                 (1U << 8) | (2U << 10));
    ati_rv100_draw_immediate(qts, R100_VTX_FMT_PKCOLOR,
                             R100_VF_TRIANGLE_LIST, flat_shade_triangle,
                             ARRAY_SIZE(flat_shade_triangle), 3);
    g_assert_cmphex(qtest_readl(qts, color_base +
                                (35 * WIDTH + 41) * 4), ==, 0xff00ff00);
    qtest_writel(qts, color_base + (35 * WIDTH + 41) * 4, 0);
    qtest_writel(qts, mmio + R100_SE_CNTL,
                 (3U << 1) | (3U << 3) | (2U << 6) |
                 (1U << 8) | (2U << 10));
    ati_rv100_draw_immediate(qts, R100_VTX_FMT_PKCOLOR,
                             R100_VF_TRIANGLE_LIST, flat_shade_triangle,
                             ARRAY_SIZE(flat_shade_triangle), 3);
    g_assert_cmphex(qtest_readl(qts, color_base +
                                (35 * WIDTH + 41) * 4), ==, 0xff0000ff);

    /* A polygon is one primitive, so LAST stays fixed across its fan. */
    qtest_writel(qts, mmio + R100_SE_CNTL,
                 (3U << 1) | (3U << 3) | (3U << 6) |
                 (1U << 8) | (2U << 10));
    ati_rv100_draw_immediate(qts, R100_VTX_FMT_PKCOLOR,
                             R100_VF_POLYGON, flat_shade_polygon,
                             ARRAY_SIZE(flat_shade_polygon), 4);
    g_assert_cmphex(qtest_readl(qts, color_base +
                                (35 * WIDTH + 51) * 4), ==, 0xffffff00);
    g_assert_cmphex(qtest_readl(qts, color_base +
                                (37 * WIDTH + 49) * 4), ==, 0xffffff00);

    /* Reserved mode 3 with varying diffuse color remains unsupported. */
    qtest_writel(qts, color_base + (20 * WIDTH + 42) * 4, 0x44556677);
    qtest_writel(qts, mmio + R100_SE_CNTL,
                 (3U << 1) | (3U << 3) | (3U << 8) | (2U << 10));
    ati_rv100_draw_immediate(qts, R100_VTX_FMT_PKCOLOR,
                             R100_VF_LINE_LIST, flat_shade_line,
                             ARRAY_SIZE(flat_shade_line), 2);
    g_assert_cmphex(qtest_readl(qts, color_base +
                                (20 * WIDTH + 42) * 4), ==, 0x44556677);

    /* X.Org rectangles omit vertex colors and leave alpha shading solid. */
    qtest_writel(qts, color_base + (24 * WIDTH + 42) * 4, 0);
    qtest_writel(qts, mmio + R100_SE_CNTL,
                 (3U << 1) | (3U << 3) | (2U << 8));
    ati_rv100_draw_immediate(qts, R100_VTX_FMT_ST0,
                             R100_VF_RECTANGLE_LIST,
                             driver_style_rectangle,
                             ARRAY_SIZE(driver_style_rectangle), 3);
    g_assert_cmphex(qtest_readl(qts, color_base +
                                (24 * WIDTH + 42) * 4), ==, UINT32_MAX);

    /* Flat batches may use a different constant color per primitive. */
    qtest_writel(qts, color_base + (29 * WIDTH + 41) * 4, 0);
    qtest_writel(qts, color_base + (29 * WIDTH + 47) * 4, 0);
    qtest_writel(qts, mmio + R100_SE_CNTL,
                 (3U << 1) | (3U << 3) | (1U << 8));
    ati_rv100_draw_immediate(qts, R100_VTX_FMT_PKCOLOR,
                             R100_VF_TRIANGLE_LIST, flat_triangle_list,
                             ARRAY_SIZE(flat_triangle_list), 6);
    g_assert_cmphex(qtest_readl(qts, color_base +
                                (29 * WIDTH + 41) * 4), ==, 0xffff0000);
    g_assert_cmphex(qtest_readl(qts, color_base +
                                (29 * WIDTH + 47) * 4), ==, 0xff00ff00);
    qtest_writel(qts, mmio + R100_SE_CNTL,
                 (3U << 1) | (3U << 3) | (2U << 8) | (2U << 10));

    /*
     * Point and line paths obey the same render-engine scissor as triangles.
     * RE_WIDTH_HEIGHT holds the inclusive bottom-right corner (Linux 2.4
     * radeon_state.c radeon_emit_clip_rect), not a size as upstream's case.
     */
    qtest_writel(qts, mmio + R100_PP_CNTL, 0);
    qtest_writel(qts, mmio + R100_RB3D_COLOROFFSET, 0);
    qtest_writel(qts, mmio + R100_RB3D_COLORPITCH, WIDTH);
    qtest_writel(qts, mmio + R100_RE_TOP_LEFT, (30U << 16) | 10U);
    qtest_writel(qts, mmio + R100_RE_WIDTH_HEIGHT, (34U << 16) | 14U);
    ati_rv100_draw_immediate(qts, R100_VTX_FMT_PKCOLOR,
                             R100_VF_POINT_LIST, clipped_point,
                             ARRAY_SIZE(clipped_point), 1);
    ati_rv100_draw_immediate(qts, R100_VTX_FMT_PKCOLOR,
                             R100_VF_LINE_LIST, clipped_line,
                             ARRAY_SIZE(clipped_line), 2);
    g_assert_cmphex(qtest_readl(qts, color_base +
                                (31 * WIDTH + 5) * 4), ==, 0);
    g_assert_cmphex(qtest_readl(qts, color_base +
                                (32 * WIDTH + 9) * 4), ==, 0);
    g_assert_cmphex(qtest_readl(qts, color_base +
                                (32 * WIDTH + 12) * 4), ==, UINT32_MAX);
    g_assert_cmphex(qtest_readl(qts, color_base +
                                (32 * WIDTH + 15) * 4), ==, 0);
    ati_rv100_draw_immediate(qts, R100_VTX_FMT_PKCOLOR,
                             R100_VF_RECTANGLE_LIST, scissored_rectangle,
                             ARRAY_SIZE(scissored_rectangle), 3);
    for (y = 28; y < 38; y++) {
        for (x = 8; x < 18; x++) {
            g_assert_cmphex(qtest_readl(qts, color_base +
                                            (y * WIDTH + x) * 4), ==,
                            x >= 10 && x <= 14 && y >= 30 && y <= 34 ?
                            0xff00ff00 : 0);
        }
    }

    /* Malformed guest floats are rejected before any float-to-int cast. */
    qtest_writel(qts, mmio + R100_RE_TOP_LEFT, 0);
    qtest_writel(qts, mmio + R100_RE_WIDTH_HEIGHT,
                 (HEIGHT - 1) << 16 | (WIDTH - 1));
    ati_rv100_draw_immediate(qts, R100_VTX_FMT_PKCOLOR,
                             R100_VF_POINT_LIST, invalid_point,
                             ARRAY_SIZE(invalid_point), 1);
    ati_rv100_draw_immediate(qts, R100_VTX_FMT_PKCOLOR,
                             R100_VF_LINE_LIST, invalid_line,
                             ARRAY_SIZE(invalid_line), 2);
    ati_rv100_draw_immediate(qts, R100_VTX_FMT_PKCOLOR,
                             R100_VF_TRIANGLE_LIST, invalid_triangle,
                             ARRAY_SIZE(invalid_triangle), 3);
    g_assert_cmphex(qtest_readl(qts, color_base +
                                (40 * WIDTH) * 4), ==, 0);

    qtest_quit(qts);
}

static void ati_rv100_mipmap_triangle(uint32_t triangle[12], float x, float y,
                                      float gradient)
{
    float origin = 0.5f - gradient * 0.5f;

    triangle[0] = f32_bits(x);
    triangle[1] = f32_bits(y);
    triangle[2] = f32_bits(origin);
    triangle[3] = f32_bits(origin);
    triangle[4] = f32_bits(x + 4.0f);
    triangle[5] = f32_bits(y);
    triangle[6] = f32_bits(origin + 4.0f * gradient);
    triangle[7] = f32_bits(origin);
    triangle[8] = f32_bits(x);
    triangle[9] = f32_bits(y + 4.0f);
    triangle[10] = f32_bits(origin);
    triangle[11] = f32_bits(origin + 4.0f * gradient);
}

static void ati_rv100_texture_mipmaps(void)
{
    enum {
        WIDTH = 64,
        HEIGHT = 64,
        PATTERN_OFFSET = 0x10000,
        SOLID_OFFSET = 0x10200,
        LEVEL1_OFFSET = 4 * 32,
        LEVEL2_OFFSET = LEVEL1_OFFSET + 2 * 32,
    };
    static const struct {
        uint32_t filter;
        float gradient;
        uint32_t expected;
    } filter_cases[] = {
        { R100_TX_MAX_MIP_LEVEL(2), 0.5f, 0xffff0000 },
        { R100_TX_MIN_LINEAR | R100_TX_MAX_MIP_LEVEL(2),
          0.5f, 0xff400000 },
        { R100_TX_MIN_NEAREST_MIP_NEAREST | R100_TX_MAX_MIP_LEVEL(2),
          0.5f, 0xffffffff },
        { R100_TX_MIN_NEAREST_MIP_LINEAR | R100_TX_MAX_MIP_LEVEL(2),
          0.5f, 0xff808080 },
        { R100_TX_MIN_LINEAR_MIP_NEAREST | R100_TX_MAX_MIP_LEVEL(2),
          0.4204482f, 0xffffbfbf },
        { R100_TX_MIN_LINEAR_MIP_LINEAR | R100_TX_MAX_MIP_LEVEL(2),
          0.4204482f, 0xff706060 },
        { R100_TX_MAG_LINEAR | R100_TX_MIN_NEAREST_MIP_NEAREST |
          R100_TX_MAX_MIP_LEVEL(2), 0.2973018f, 0xff400000 },
    };
    static const struct {
        uint32_t filter;
        float gradient;
        uint32_t expected;
    } lod_cases[] = {
        { R100_TX_MIN_NEAREST_MIP_NEAREST | R100_TX_MAX_MIP_LEVEL(2) |
          R100_TX_LOD_BIAS(0x20), 0.25f, 0xff0000ff },
        { R100_TX_MIN_NEAREST_MIP_NEAREST | R100_TX_MAX_MIP_LEVEL(2) |
          R100_TX_LOD_BIAS(0x80), 0.5f, 0xffff0000 },
        { R100_TX_MIN_NEAREST_MIP_NEAREST | R100_TX_MAX_MIP_LEVEL(2),
          1.0f, 0xff00ff00 },
        { R100_TX_MIN_NEAREST_MIP_NEAREST | R100_TX_MAX_MIP_LEVEL(1),
          1.0f, 0xff0000ff },
    };
    const uint32_t replace_texture_color =
        (10U << 10) | R100_COMBINER_CLAMP;
    const uint32_t replace_texture_alpha =
        (5U << 8) | R100_COMBINER_CLAMP;
    const uint64_t color_base = rv100_fb;
    const uint64_t mmio = rv100_mmio;
    uint32_t triangle[12];
    const uint32_t perspective_triangle[] = {
        f32_bits(40.0f), f32_bits(16.0f),
        f32_bits(0.375f), f32_bits(0.375f), f32_bits(1.0f),
        f32_bits(44.0f), f32_bits(16.0f),
        f32_bits(0.375f), f32_bits(0.375f), f32_bits(-1.0f),
        f32_bits(40.0f), f32_bits(20.0f),
        f32_bits(0.375f), f32_bits(0.375f), f32_bits(1.0f),
    };
    QTestState *qts;
    unsigned int x;
    unsigned int y;
    size_t i;

    qts = rv100_3d_init("");
    qtest_memset(qts, color_base, 0, 0x11000);

    /* A 4x4, 2x2, 1x1 POT chain uses a 32-byte pitch at every level. */
    for (y = 0; y < 4; y++) {
        for (x = 0; x < 4; x++) {
            qtest_writel(qts, color_base + PATTERN_OFFSET + y * 32 + x * 4,
                         0xff000000);
        }
    }
    qtest_writel(qts, color_base + PATTERN_OFFSET + 2 * 32 + 2 * 4,
                 0xffff0000);
    qtest_writel(qts, color_base + PATTERN_OFFSET + LEVEL1_OFFSET,
                 0xffff0000);
    qtest_writel(qts, color_base + PATTERN_OFFSET + LEVEL1_OFFSET + 4,
                 0xff00ff00);
    qtest_writel(qts, color_base + PATTERN_OFFSET + LEVEL1_OFFSET + 32,
                 0xff0000ff);
    qtest_writel(qts, color_base + PATTERN_OFFSET + LEVEL1_OFFSET + 36,
                 0xffffffff);
    qtest_writel(qts, color_base + PATTERN_OFFSET + LEVEL2_OFFSET,
                 0xff000000);

    for (y = 0; y < 4; y++) {
        for (x = 0; x < 4; x++) {
            qtest_writel(qts, color_base + SOLID_OFFSET + y * 32 + x * 4,
                         0xffff0000);
        }
    }
    for (y = 0; y < 2; y++) {
        for (x = 0; x < 2; x++) {
            qtest_writel(qts, color_base + SOLID_OFFSET + LEVEL1_OFFSET +
                         y * 32 + x * 4, 0xff0000ff);
        }
    }
    qtest_writel(qts, color_base + SOLID_OFFSET + LEVEL2_OFFSET,
                 0xff00ff00);

    qtest_writel(qts, mmio + R100_RB3D_COLOROFFSET, 0);
    qtest_writel(qts, mmio + R100_RB3D_COLORPITCH, WIDTH);
    qtest_writel(qts, mmio + R100_RE_TOP_LEFT, 0);
    qtest_writel(qts, mmio + R100_RE_WIDTH_HEIGHT,
                 (HEIGHT - 1) << 16 | (WIDTH - 1));
    qtest_writel(qts, mmio + R100_SE_CNTL,
                 (3U << 1) | (3U << 3) | (2U << 8) | (2U << 10));
    qtest_writel(qts, mmio + R100_SE_COORD_FMT, 0);
    qtest_writel(qts, mmio + R100_RB3D_CNTL, R100_RB_COLOR_ARGB8888);
    qtest_writel(qts, mmio + R100_PP_TXFORMAT_0,
                 R100_TX_ARGB8888 | R100_TX_ALPHA_IN_MAP |
                 (2U << 8) | (2U << 12));
    qtest_writel(qts, mmio + R100_PP_TXOFFSET_0, PATTERN_OFFSET);
    qtest_writel(qts, mmio + R100_PP_TXCBLEND_0, replace_texture_color);
    qtest_writel(qts, mmio + R100_PP_TXABLEND_0, replace_texture_alpha);
    qtest_writel(qts, mmio + R100_PP_CNTL,
                 R100_TX0_ENABLE | R100_TX_BLEND0_ENABLE);

    /* The register names list the mip kernel before the texel kernel. */
    for (i = 0; i < ARRAY_SIZE(filter_cases); i++) {
        x = 4 + i * 8;
        ati_rv100_mipmap_triangle(triangle, x, 4.0f,
                                  filter_cases[i].gradient);
        qtest_writel(qts, mmio + R100_PP_TXFILTER_0,
                     filter_cases[i].filter);
        ati_rv100_draw_immediate(qts, R100_VTX_FMT_ST0,
                                 R100_VF_TRIANGLE_LIST, triangle,
                                 ARRAY_SIZE(triangle), 3);
        g_assert_cmphex(qtest_readl(qts, color_base +
                                    (4 * WIDTH + x) * 4), ==,
                        filter_cases[i].expected);
    }

    qtest_writel(qts, mmio + R100_PP_TXOFFSET_0, SOLID_OFFSET);
    for (i = 0; i < ARRAY_SIZE(lod_cases); i++) {
        x = 4 + i * 8;
        ati_rv100_mipmap_triangle(triangle, x, 16.0f,
                                  lod_cases[i].gradient);
        qtest_writel(qts, mmio + R100_PP_TXFILTER_0, lod_cases[i].filter);
        ati_rv100_draw_immediate(qts, R100_VTX_FMT_ST0,
                                 R100_VF_TRIANGLE_LIST, triangle,
                                 ARRAY_SIZE(triangle), 3);
        g_assert_cmphex(qtest_readl(qts, color_base +
                                    (16 * WIDTH + x) * 4), ==,
                        lod_cases[i].expected);
    }

    /* Each triangle selects its own LOD within a single draw command. */
    {
        const float gradients[] = { 0.25f, 0.5f, 1.0f, 0.25f };
        const uint32_t colors[] = {
            0xffff0000, 0xff0000ff, 0xff00ff00, 0xffff0000,
        };
        uint32_t triangles[4 * 12];

        qtest_writel(qts, mmio + R100_SE_CNTL,
                     (3U << 1) | (3U << 3) | 0xaa00);
        qtest_writel(qts, mmio + R100_PP_TXFILTER_0,
                     R100_TX_MIN_NEAREST_MIP_NEAREST |
                     R100_TX_MAX_MIP_LEVEL(2));
        for (i = 0; i < ARRAY_SIZE(gradients); i++) {
            ati_rv100_mipmap_triangle(triangles + i * 12, 4 + i * 8, 32,
                                      gradients[i]);
        }
        ati_rv100_draw_immediate(qts, R100_VTX_FMT_ST0,
                                 R100_VF_TRIANGLE_LIST, triangles,
                                 ARRAY_SIZE(triangles), 12);
        for (i = 0; i < ARRAY_SIZE(colors); i++) {
            g_assert_cmphex(qtest_readl(qts, color_base +
                                       (32 * WIDTH + 4 + i * 8) * 4), ==,
                            colors[i]);
        }
    }

    /* Perspective LOD accounts for Q's gradient, not just affine S and T. */
    qtest_writel(qts, mmio + R100_PP_TXFORMAT_0,
                 R100_TX_ARGB8888 | R100_TX_ALPHA_IN_MAP |
                 (2U << 8) | (2U << 12) | R100_TX_PERSPECTIVE);
    qtest_writel(qts, mmio + R100_PP_TXFILTER_0,
                 R100_TX_MIN_NEAREST_MIP_NEAREST |
                 R100_TX_MAX_MIP_LEVEL(2));
    ati_rv100_draw_immediate(qts, R100_VTX_FMT_ST0 | R100_VTX_FMT_Q0,
                             R100_VF_TRIANGLE_LIST, perspective_triangle,
                             ARRAY_SIZE(perspective_triangle), 3);
    g_assert_cmphex(qtest_readl(qts, color_base +
                                (16 * WIDTH + 40) * 4), ==, 0xff00ff00);
    qtest_quit(qts);
}

static void ati_rv100_texture_point(QTestState *qts, float x, float y,
                                    float s, float t)
{
    const uint32_t point[] = {
        f32_bits(x), f32_bits(y), f32_bits(s), f32_bits(t),
    };

    ati_rv100_draw_immediate(qts, R100_VTX_FMT_ST0,
                             R100_VF_POINT_LIST, point,
                             ARRAY_SIZE(point), 1);
}

static void ati_rv100_yuv_textures(void)
{
    enum {
        WIDTH = 64,
        HEIGHT = 64,
        TEXTURE_OFFSET = 0x10000,
    };
    static const struct {
        uint32_t format;
        uint8_t texels[8];
    } cases[] = {
        {
            R100_TX_VYUY422,
            { 16, 128, 235, 128, 81, 90, 81, 240 },
        }, {
            R100_TX_YVYU422,
            { 128, 16, 128, 235, 90, 81, 240, 81 },
        },
    };
    static const uint32_t expected[] = {
        0xff000000, 0xffffffff, 0xfffe0000, 0xfffe0000,
    };
    const uint32_t replace_texture_color =
        (10U << 10) | R100_COMBINER_CLAMP;
    const uint32_t replace_texture_alpha =
        (5U << 8) | R100_COMBINER_CLAMP;
    const uint64_t color_base = rv100_fb;
    const uint64_t mmio = rv100_mmio;
    QTestState *qts;
    size_t i;
    size_t j;

    for (i = 0; i < ARRAY_SIZE(cases); i++) {
        qts = rv100_3d_init("");
        qtest_memset(qts, color_base, 0, 0x11000);
        qtest_memwrite(qts, color_base + TEXTURE_OFFSET,
                       cases[i].texels, sizeof(cases[i].texels));

        qtest_writel(qts, mmio + R100_RB3D_COLOROFFSET, 0);
        qtest_writel(qts, mmio + R100_RB3D_COLORPITCH, WIDTH);
        qtest_writel(qts, mmio + R100_RE_TOP_LEFT, 0);
        qtest_writel(qts, mmio + R100_RE_WIDTH_HEIGHT,
                     (HEIGHT - 1) << 16 | (WIDTH - 1));
        qtest_writel(qts, mmio + R100_SE_CNTL,
                     (3U << 1) | (3U << 3) | (2U << 8) | (2U << 10));
        qtest_writel(qts, mmio + R100_SE_COORD_FMT,
                     R100_COORD_ST0_NONPARAM);
        qtest_writel(qts, mmio + R100_RB3D_CNTL,
                     R100_RB_COLOR_ARGB8888);
        qtest_writel(qts, mmio + R100_PP_TXFILTER_0,
                     R100_TX_YUV_TO_RGB);
        qtest_writel(qts, mmio + R100_PP_TXFORMAT_0,
                     cases[i].format | R100_TX_NON_POWER2);
        qtest_writel(qts, mmio + R100_PP_TXOFFSET_0, TEXTURE_OFFSET);
        qtest_writel(qts, mmio + R100_PP_TEX_SIZE_0, 3);
        qtest_writel(qts, mmio + R100_PP_TEX_PITCH_0, 0);
        qtest_writel(qts, mmio + R100_PP_TXCBLEND_0,
                     replace_texture_color);
        qtest_writel(qts, mmio + R100_PP_TXABLEND_0,
                     replace_texture_alpha);
        qtest_writel(qts, mmio + R100_PP_CNTL,
                     R100_TX0_ENABLE | R100_TX_BLEND0_ENABLE);

        for (j = 0; j < ARRAY_SIZE(expected); j++) {
            ati_rv100_texture_point(qts, 4.0f + j, 4.0f, j, 0.0f);
            g_assert_cmphex(qtest_readl(
                qts, color_base + (4 * WIDTH + 4 + j) * 4), ==,
                expected[j]);
        }
        qtest_writel(qts, mmio + R100_PP_TXFILTER_0,
                     R100_TX_YUV_TO_RGB | R100_TX_LINEAR);
        ati_rv100_texture_point(qts, 10.0f, 4.0f, 1.0f, 0.5f);
        g_assert_cmphex(qtest_readl(qts, color_base +
                                        (4 * WIDTH + 10) * 4), ==,
                        0xff7f7f7f);
        qtest_quit(qts);
    }
}

static void ati_rv100_compressed_textures(void)
{
    enum {
        WIDTH = 64,
        HEIGHT = 64,
        DXT1_OFFSET = 0x10000,
        DXT1_ALPHA_OFFSET = 0x10100,
        DXT3_OFFSET = 0x10200,
        DXT5_SEVEN_OFFSET = 0x10300,
        DXT5_FIVE_OFFSET = 0x10400,
        PITCH_OFFSET = 0x10600,
        MIP_OFFSET = 0x10800,
    };
    static const uint8_t dxt1_four[] = {
        0x00, 0xf8, 0xe0, 0x07, 0xe4, 0x00, 0x00, 0x00,
    };
    static const uint32_t dxt1_four_expected[] = {
        0xffff0000, 0xff00ff00, 0xffaa5500, 0xff55aa00,
    };
    static const uint8_t dxt1_alpha[] = {
        0x1f, 0x00, 0xff, 0xff, 0xe4, 0x00, 0x00, 0x00,
    };
    static const uint32_t dxt1_alpha_expected[] = {
        0xff0000ff, 0xffffffff, 0xff8080ff, 0x00000000,
    };
    static const uint8_t dxt3[] = {
        0x50, 0xfa, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x1f, 0x00, 0x00, 0xf8, 0xe4, 0x00, 0x00, 0x00,
    };
    static const uint32_t dxt3_expected[] = {
        0x000000ff, 0x55ff0000, 0xaa5500aa, 0xffaa0055,
    };
    static const uint8_t dxt5_seven[] = {
        0xff, 0x00, 0x88, 0x0e, 0x00, 0x00, 0x00, 0x00,
        0x00, 0xf8, 0xe0, 0x07, 0x00, 0x00, 0x00, 0x00,
    };
    static const uint32_t dxt5_seven_expected[] = {
        0xffff0000, 0x00ff0000, 0xdbff0000, 0x24ff0000,
    };
    static const uint8_t dxt5_five[] = {
        0x00, 0xff, 0xaa, 0x0f, 0x00, 0x00, 0x00, 0x00,
        0x00, 0xf8, 0xe0, 0x07, 0x00, 0x00, 0x00, 0x00,
    };
    static const uint32_t dxt5_five_expected[] = {
        0x33ff0000, 0xccff0000, 0x00ff0000, 0xffff0000,
    };
    static const uint8_t solid_red[] = {
        0x00, 0xf8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    static const uint8_t solid_green[] = {
        0xe0, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    static const struct {
        uint32_t offset;
        uint32_t format;
        const uint8_t *block;
        size_t block_size;
        const uint32_t *expected;
    } cases[] = {
        { DXT1_OFFSET, R100_TX_DXT1, dxt1_four,
          sizeof(dxt1_four), dxt1_four_expected },
        { DXT1_ALPHA_OFFSET, R100_TX_DXT1 | R100_TX_ALPHA_IN_MAP,
          dxt1_alpha, sizeof(dxt1_alpha), dxt1_alpha_expected },
        { DXT3_OFFSET, R100_TX_DXT3 | R100_TX_ALPHA_IN_MAP,
          dxt3, sizeof(dxt3), dxt3_expected },
        { DXT5_SEVEN_OFFSET, R100_TX_DXT5 | R100_TX_ALPHA_IN_MAP,
          dxt5_seven, sizeof(dxt5_seven), dxt5_seven_expected },
        { DXT5_FIVE_OFFSET, R100_TX_DXT5 | R100_TX_ALPHA_IN_MAP,
          dxt5_five, sizeof(dxt5_five), dxt5_five_expected },
    };
    const uint32_t replace_texture_color =
        (10U << 10) | R100_COMBINER_CLAMP;
    const uint32_t replace_texture_alpha =
        (5U << 8) | R100_COMBINER_CLAMP;
    const uint64_t color_base = rv100_fb;
    const uint64_t mmio = rv100_mmio;
    uint32_t triangle[12];
    QTestState *qts;
    size_t i;
    size_t j;

    qts = rv100_3d_init("");
    qtest_memset(qts, color_base, 0, 0x11000);
    qtest_writel(qts, mmio + R100_RB3D_COLOROFFSET, 0);
    qtest_writel(qts, mmio + R100_RB3D_COLORPITCH, WIDTH);
    qtest_writel(qts, mmio + R100_RE_TOP_LEFT, 0);
    qtest_writel(qts, mmio + R100_RE_WIDTH_HEIGHT,
                 (HEIGHT - 1) << 16 | (WIDTH - 1));
    qtest_writel(qts, mmio + R100_SE_CNTL,
                 (3U << 1) | (3U << 3) | (2U << 8) | (2U << 10));
    qtest_writel(qts, mmio + R100_SE_COORD_FMT,
                 R100_COORD_ST0_NONPARAM);
    qtest_writel(qts, mmio + R100_RB3D_CNTL, R100_RB_COLOR_ARGB8888);
    qtest_writel(qts, mmio + R100_PP_TXFILTER_0, 0);
    qtest_writel(qts, mmio + R100_PP_TXCBLEND_0, replace_texture_color);
    qtest_writel(qts, mmio + R100_PP_TXABLEND_0, replace_texture_alpha);
    qtest_writel(qts, mmio + R100_PP_CNTL,
                 R100_TX0_ENABLE | R100_TX_BLEND0_ENABLE);

    for (i = 0; i < ARRAY_SIZE(cases); i++) {
        qtest_memwrite(qts, color_base + cases[i].offset,
                       cases[i].block, cases[i].block_size);
        qtest_writel(qts, mmio + R100_PP_TXFORMAT_0,
                     cases[i].format | (2U << 8) | (2U << 12));
        qtest_writel(qts, mmio + R100_PP_TXOFFSET_0, cases[i].offset);
        for (j = 0; j < 4; j++) {
            ati_rv100_texture_point(qts, 4.0f + j, 4.0f + i * 2,
                                    j, 0.0f);
            g_assert_cmphex(qtest_readl(
                qts, color_base + ((4 + i * 2) * WIDTH + 4 + j) * 4),
                ==, cases[i].expected[j]);
        }
    }

    /* RGB DXT1 keeps selector 3 opaque while retaining its black color. */
    qtest_writel(qts, mmio + R100_PP_TXFORMAT_0,
                 R100_TX_DXT1 | (2U << 8) | (2U << 12));
    qtest_writel(qts, mmio + R100_PP_TXOFFSET_0, DXT1_ALPHA_OFFSET);
    ati_rv100_texture_point(qts, 10.0f, 6.0f, 3.0f, 0.0f);
    g_assert_cmphex(qtest_readl(qts, color_base +
                                    (6 * WIDTH + 10) * 4), ==,
                    0xff000000);

    /* Bilinear sampling reuses a decoded block and blends adjacent texels. */
    qtest_writel(qts, mmio + R100_PP_TXFORMAT_0,
                 R100_TX_DXT1 | (2U << 8) | (2U << 12));
    qtest_writel(qts, mmio + R100_PP_TXOFFSET_0, DXT1_OFFSET);
    qtest_writel(qts, mmio + R100_PP_TXFILTER_0, R100_TX_LINEAR);
    ati_rv100_texture_point(qts, 10.0f, 8.0f, 1.0f, 0.5f);
    g_assert_cmphex(qtest_readl(qts, color_base +
                                    (8 * WIDTH + 10) * 4), ==,
                    0xff808000);
    qtest_writel(qts, mmio + R100_PP_TXFILTER_0, 0);

    /* A 32x8 DXT1 image uses a 64-byte block-row pitch, not 32 bytes. */
    qtest_memset(qts, color_base + PITCH_OFFSET, 0, 96);
    qtest_memwrite(qts, color_base + PITCH_OFFSET + 32,
                   solid_red, sizeof(solid_red));
    qtest_memwrite(qts, color_base + PITCH_OFFSET + 64,
                   solid_green, sizeof(solid_green));
    qtest_writel(qts, mmio + R100_PP_TXFORMAT_0,
                 R100_TX_DXT1 | (5U << 8) | (3U << 12));
    qtest_writel(qts, mmio + R100_PP_TXOFFSET_0, PITCH_OFFSET);
    ati_rv100_texture_point(qts, 4.0f, 16.0f, 16.0f, 0.0f);
    ati_rv100_texture_point(qts, 5.0f, 16.0f, 0.0f, 4.0f);
    g_assert_cmphex(qtest_readl(qts, color_base +
                                    (16 * WIDTH + 4) * 4), ==,
                    0xffff0000);
    g_assert_cmphex(qtest_readl(qts, color_base +
                                    (16 * WIDTH + 5) * 4), ==,
                    0xff00ff00);

    /* A 4x4 DXT1 level occupies 32 bytes before its 2x2 mip level. */
    qtest_memset(qts, color_base + MIP_OFFSET, 0, 64);
    qtest_memwrite(qts, color_base + MIP_OFFSET,
                   solid_red, sizeof(solid_red));
    qtest_memwrite(qts, color_base + MIP_OFFSET + 32,
                   solid_green, sizeof(solid_green));
    qtest_writel(qts, mmio + R100_SE_COORD_FMT, 0);
    qtest_writel(qts, mmio + R100_PP_TXFORMAT_0,
                 R100_TX_DXT1 | (2U << 8) | (2U << 12));
    qtest_writel(qts, mmio + R100_PP_TXOFFSET_0, MIP_OFFSET);
    qtest_writel(qts, mmio + R100_PP_TXFILTER_0,
                 R100_TX_MIN_NEAREST_MIP_NEAREST |
                 R100_TX_MAX_MIP_LEVEL(1));
    ati_rv100_mipmap_triangle(triangle, 16.0f, 16.0f, 0.5f);
    ati_rv100_draw_immediate(qts, R100_VTX_FMT_ST0,
                             R100_VF_TRIANGLE_LIST, triangle,
                             ARRAY_SIZE(triangle), 3);
    g_assert_cmphex(qtest_readl(qts, color_base +
                                    (16 * WIDTH + 16) * 4), ==,
                    0xff00ff00);
    qtest_quit(qts);
}

/*
 * The vertex layout Mesa emits: q0 right after s0/t0, and the specular dword
 * in VF_COLOR_ORDER_RGBA like the diffuse one.  A LOAD_VBPNTR of zero arrays
 * leaves DRAW_VBUF nothing to read.
 */
static void ati_rv100_vertex_layout(void)
{
    enum {
        WIDTH = 16,
        HEIGHT = 8,
        TEX0_OFFSET = 0x10000,
        TEX1_OFFSET = 0x10100,
        VERTEX_OFFSET = 0x11000,
        IB_OFFSET = 0x12000,
    };
    const uint32_t se_cntl = (3U << 1) | (3U << 3) | (2U << 8) | (2U << 10) |
                             (2U << 12) | (2U << 14) | R100_VTX_PIX_CENTER_OGL;
    const uint32_t q0_then_st1[] = {
        f32_bits(2.0f), f32_bits(2.0f),
        f32_bits(4.0f), f32_bits(0.0f), f32_bits(2.0f),
        f32_bits(1.0f), f32_bits(0.0f),
    };
    const uint32_t specular_point[] = {
        f32_bits(6.0f), f32_bits(2.0f),
        ati_rv100_rgba_vertex_color(0xff000000), 0x000000ff,
    };
    const uint32_t vertex[] = { f32_bits(8.0f), f32_bits(2.0f) };
    const uint32_t draw_vbuf[] = {
        R100_CP_PACKET3 | (1U << 16) | (R100_PACKET3_DRAW_VBUF << 8),
        0, R100_VF_POINT_LIST | R100_VF_WALK_LIST | (1U << 16),
    };
    const uint32_t one_array[] = {
        R100_CP_PACKET3 | (2U << 16) | (R100_PACKET3_LOAD_VBPNTR << 8),
        1, 2U | (2U << 8), VERTEX_OFFSET,
    };
    const uint32_t no_array[] = {
        R100_CP_PACKET3 | (R100_PACKET3_LOAD_VBPNTR << 8), 0,
    };
    const uint64_t vbuf_pixel = rv100_fb + (2 * WIDTH + 8) * 4;
    QTestState *qts = rv100_3d_init("");
    unsigned int i;

    qtest_memset(qts, rv100_fb, 0, WIDTH * HEIGHT * 4);
    for (i = 0; i < 4; i++) {
        qtest_writel(qts, rv100_fb + TEX0_OFFSET + i * 4,
                     0xff000000 | (i + 1) << 20);
        qtest_writel(qts, rv100_fb + TEX1_OFFSET + i * 4,
                     0xff000000 | (i + 1) << 4);
    }
    qtest_writel(qts, rv100_mmio + R100_RB3D_CNTL, R100_RB_COLOR_ARGB8888);
    qtest_writel(qts, rv100_mmio + R100_RB3D_COLOROFFSET, 0);
    qtest_writel(qts, rv100_mmio + R100_RB3D_COLORPITCH, WIDTH);
    qtest_writel(qts, rv100_mmio + R100_RE_TOP_LEFT, 0);
    qtest_writel(qts, rv100_mmio + R100_RE_WIDTH_HEIGHT,
                 (HEIGHT - 1) << 16 | (WIDTH - 1));
    qtest_writel(qts, rv100_mmio + R100_SE_CNTL, se_cntl);
    qtest_writel(qts, rv100_mmio + R100_SE_COORD_FMT,
                 R100_COORD_ST0_NONPARAM | R100_COORD_ST1_NONPARAM);
    qtest_writel(qts, rv100_mmio + R100_PP_TXFILTER_0, 0);
    qtest_writel(qts, rv100_mmio + R100_PP_TXFORMAT_0,
                 R100_TX_ARGB8888 | R100_TX_ALPHA_IN_MAP |
                 R100_TX_NON_POWER2 | R100_TX_PERSPECTIVE);
    qtest_writel(qts, rv100_mmio + R100_PP_TXOFFSET_0, TEX0_OFFSET);
    qtest_writel(qts, rv100_mmio + R100_PP_TEX_SIZE_0, 3);
    qtest_writel(qts, rv100_mmio + R100_PP_TEX_PITCH_0, 0);
    qtest_writel(qts, rv100_mmio + R100_PP_TXCBLEND_0,
                 (10U << 10) | R100_COMBINER_CLAMP);
    qtest_writel(qts, rv100_mmio + R100_PP_TXABLEND_0,
                 (5U << 8) | R100_COMBINER_CLAMP);
    qtest_writel(qts, rv100_mmio + R100_PP_TXFILTER_1, 0);
    qtest_writel(qts, rv100_mmio + R100_PP_TXFORMAT_1,
                 R100_TX_ARGB8888 | R100_TX_ALPHA_IN_MAP |
                 R100_TX_NON_POWER2 | R100_TX_ROUTE_ST1);
    qtest_writel(qts, rv100_mmio + R100_PP_TXOFFSET_1, TEX1_OFFSET);
    qtest_writel(qts, rv100_mmio + R100_PP_TEX_SIZE_1, 3);
    qtest_writel(qts, rv100_mmio + R100_PP_TEX_PITCH_1, 0);
    qtest_writel(qts, rv100_mmio + R100_PP_TXCBLEND_1,
                 (12U << 10) | R100_COMBINER_CLAMP);
    qtest_writel(qts, rv100_mmio + R100_PP_TXABLEND_1,
                 (6U << 8) | R100_COMBINER_CLAMP);

    /* s0 / q0 = 2 selects texel 2 of unit 0; s1 = 1 texel 1 of unit 1. */
    qtest_writel(qts, rv100_mmio + R100_PP_CNTL,
                 R100_TX0_ENABLE | R100_TX_BLEND0_ENABLE);
    ati_rv100_draw_immediate(qts, R100_VTX_FMT_ST0 | R100_VTX_FMT_Q0 |
                             R100_VTX_FMT_ST1, R100_VF_POINT_LIST,
                             q0_then_st1, ARRAY_SIZE(q0_then_st1), 1);
    g_assert_cmphex(qtest_readl(qts, rv100_fb + (2 * WIDTH + 2) * 4), ==,
                    0xff300000);
    qtest_writel(qts, rv100_mmio + R100_PP_CNTL,
                 R100_TX1_ENABLE | R100_TX_BLEND1_ENABLE);
    ati_rv100_draw_immediate(qts, R100_VTX_FMT_ST0 | R100_VTX_FMT_Q0 |
                             R100_VTX_FMT_ST1, R100_VF_POINT_LIST,
                             q0_then_st1, ARRAY_SIZE(q0_then_st1), 1);
    g_assert_cmphex(qtest_readl(qts, rv100_fb + (2 * WIDTH + 2) * 4), ==,
                    0xff000020);

    /* Byte 0 of an RGBA-ordered specular dword is red. */
    qtest_writel(qts, rv100_mmio + R100_PP_CNTL, R100_PP_SPECULAR_ENABLE);
    ati_rv100_draw_immediate(qts, R100_VTX_FMT_PKCOLOR | R100_VTX_FMT_PKSPEC,
                             R100_VF_POINT_LIST, specular_point,
                             ARRAY_SIZE(specular_point), 1);
    g_assert_cmphex(qtest_readl(qts, rv100_fb + (2 * WIDTH + 6) * 4), ==,
                    0xffff0000);

    qtest_writel(qts, rv100_mmio + R100_PP_CNTL, 0);
    for (i = 0; i < ARRAY_SIZE(vertex); i++) {
        qtest_writel(qts, rv100_fb + VERTEX_OFFSET + i * 4, vertex[i]);
    }
    for (i = 0; i < ARRAY_SIZE(one_array); i++) {
        qtest_writel(qts, rv100_fb + IB_OFFSET + i * 4, one_array[i]);
    }
    for (i = 0; i < ARRAY_SIZE(draw_vbuf); i++) {
        qtest_writel(qts, rv100_fb + IB_OFFSET + (4 + i) * 4, draw_vbuf[i]);
    }
    qtest_writel(qts, rv100_mmio + R100_CP_CSQ_CNTL, R100_CSQ_PRIBM_INDBM);
    qtest_writel(qts, rv100_mmio + R100_CP_IB_BASE, IB_OFFSET);
    qtest_writel(qts, rv100_mmio + R100_CP_IB_BUFSZ, 7);
    g_assert_cmphex(qtest_readl(qts, vbuf_pixel), ==, UINT32_MAX);

    qtest_writel(qts, vbuf_pixel, 0);
    for (i = 0; i < ARRAY_SIZE(no_array); i++) {
        qtest_writel(qts, rv100_fb + IB_OFFSET + i * 4, no_array[i]);
    }
    for (i = 0; i < ARRAY_SIZE(draw_vbuf); i++) {
        qtest_writel(qts, rv100_fb + IB_OFFSET + (2 + i) * 4, draw_vbuf[i]);
    }
    qtest_writel(qts, rv100_mmio + R100_CP_IB_BUFSZ, 5);
    g_assert_cmphex(qtest_readl(qts, vbuf_pixel), ==, 0);
    qtest_quit(qts);
}

/*
 * W0 = 1/w with projected x, y as XP 3790's Direct3D sends them: a
 * PERSPECTIVE_ENABLE texture interpolates s * 1/w and 1/w across the
 * screen, a texture without it interpolates s.
 */
static void ati_rv100_perspective(void)
{
    enum {
        WIDTH = 16,
        HEIGHT = 4,
        TEX_OFFSET = 0x10000,
    };
    const uint32_t se_cntl = (3U << 1) | (3U << 3) | (2U << 8) | (2U << 10) |
                             (2U << 12) | (2U << 14) | R100_VTX_PIX_CENTER_OGL;
    const uint32_t quad[] = {
        f32_bits(0.0f), f32_bits(0.0f), f32_bits(1.0f),
        f32_bits(0.0f), f32_bits(0.0f),
        f32_bits(16.0f), f32_bits(0.0f), f32_bits(0.25f),
        f32_bits(1.0f), f32_bits(0.0f),
        f32_bits(16.0f), f32_bits(4.0f), f32_bits(0.25f),
        f32_bits(1.0f), f32_bits(0.0f),
        f32_bits(0.0f), f32_bits(0.0f), f32_bits(1.0f),
        f32_bits(0.0f), f32_bits(0.0f),
        f32_bits(16.0f), f32_bits(4.0f), f32_bits(0.25f),
        f32_bits(1.0f), f32_bits(0.0f),
        f32_bits(0.0f), f32_bits(4.0f), f32_bits(1.0f),
        f32_bits(0.0f), f32_bits(0.0f),
    };
    const uint32_t txformat = R100_TX_ARGB8888 | R100_TX_ALPHA_IN_MAP |
                              (3U << 8);
    const uint64_t pixel = rv100_fb + (2 * WIDTH + 8) * 4;
    QTestState *qts = rv100_3d_init("");
    unsigned int i;

    qtest_memset(qts, rv100_fb, 0, WIDTH * HEIGHT * 4);
    for (i = 0; i < 8; i++) {
        qtest_writel(qts, rv100_fb + TEX_OFFSET + i * 4,
                     0xff000000 | i << 4);
    }
    qtest_writel(qts, rv100_mmio + R100_RB3D_CNTL, R100_RB_COLOR_ARGB8888);
    qtest_writel(qts, rv100_mmio + R100_RB3D_COLOROFFSET, 0);
    qtest_writel(qts, rv100_mmio + R100_RB3D_COLORPITCH, WIDTH);
    qtest_writel(qts, rv100_mmio + R100_RE_TOP_LEFT, 0);
    qtest_writel(qts, rv100_mmio + R100_RE_WIDTH_HEIGHT,
                 (HEIGHT - 1) << 16 | (WIDTH - 1));
    qtest_writel(qts, rv100_mmio + R100_SE_CNTL, se_cntl);
    qtest_writel(qts, rv100_mmio + R100_SE_COORD_FMT,
                 R100_COORD_XY_PRE_MULT | R100_COORD_Z_PRE_MULT);
    qtest_writel(qts, rv100_mmio + R100_PP_TXFILTER_0, 0);
    qtest_writel(qts, rv100_mmio + R100_PP_TXOFFSET_0, TEX_OFFSET);
    qtest_writel(qts, rv100_mmio + R100_PP_TXCBLEND_0,
                 (10U << 10) | R100_COMBINER_CLAMP);
    qtest_writel(qts, rv100_mmio + R100_PP_TXABLEND_0,
                 (5U << 8) | R100_COMBINER_CLAMP);
    qtest_writel(qts, rv100_mmio + R100_PP_CNTL,
                 R100_TX0_ENABLE | R100_TX_BLEND0_ENABLE);

    /* At x = 8.5, s = (0.25 * 8.5 / 16) / (1 - 0.75 * 8.5 / 16): texel 1. */
    qtest_writel(qts, rv100_mmio + R100_PP_TXFORMAT_0,
                 txformat | R100_TX_PERSPECTIVE);
    ati_rv100_draw_immediate(qts, R100_VTX_FMT_W0 | R100_VTX_FMT_ST0,
                             R100_VF_TRIANGLE_LIST, quad, ARRAY_SIZE(quad), 6);
    g_assert_cmphex(qtest_readl(qts, pixel), ==, 0xff000010);

    /* Without PERSPECTIVE_ENABLE, s = 8.5 / 16: texel 4. */
    qtest_writel(qts, rv100_mmio + R100_PP_TXFORMAT_0, txformat);
    ati_rv100_draw_immediate(qts, R100_VTX_FMT_W0 | R100_VTX_FMT_ST0,
                             R100_VF_TRIANGLE_LIST, quad, ARRAY_SIZE(quad), 6);
    g_assert_cmphex(qtest_readl(qts, pixel), ==, 0xff000040);
    qtest_quit(qts);
}

static void display_wait_for_migration_status(QTestState *qts,
                                              const char *wanted)
{
    int64_t deadline = g_get_monotonic_time() + 60 * G_TIME_SPAN_SECOND;

    for (;;) {
        QDict *result = qtest_qmp_assert_success_ref(
            qts, "{'execute':'query-migrate'}");
        const char *status = qdict_get_str(result, "status");

        if (!strcmp(status, wanted)) {
            qobject_unref(result);
            return;
        }
        if (!strcmp(status, "failed") || !strcmp(status, "failing") ||
            !strcmp(status, "cancelled") ||
            (!strcmp(status, "completed") && strcmp(wanted, "completed"))) {
            g_error("migration entered terminal status '%s'", status);
        }
        qobject_unref(result);
        g_assert_cmpint(g_get_monotonic_time(), <, deadline);
        g_usleep(1000);
    }
}

static void display_wait_for_migration(QTestState *qts)
{
    display_wait_for_migration_status(qts, "completed");
}

int main(int argc, char **argv)
{
    QTestState *qts;

    g_test_init(&argc, &argv, NULL);
    qts = qtest_init("-machine zx1,vga=rv100 -m 256M -S");
    rv100_fb = rv100_bar(qts, PCI_BASE_ADDRESS_0);
    rv100_mmio = rv100_bar(qts, PCI_BASE_ADDRESS_2);
    qtest_quit(qts);

    qtest_add_func("/ati-rv100-3d/ring", ati_rv100_3d_ring);
    qtest_add_func("/ati-rv100-3d/vertex-fog", ati_rv100_vertex_fog);
    qtest_add_func("/ati-rv100-3d/live-migration",
                   ati_rv100_3d_live_migration);
    qtest_add_func("/ati-rv100-3d/fixed-function", ati_rv100_fixed_function);
    qtest_add_func("/ati-rv100-3d/texture-mipmaps",
                   ati_rv100_texture_mipmaps);
    qtest_add_func("/ati-rv100-3d/compressed-textures",
                   ati_rv100_compressed_textures);
    qtest_add_func("/ati-rv100-3d/yuv-textures", ati_rv100_yuv_textures);
    qtest_add_func("/ati-rv100-3d/vertex-layout", ati_rv100_vertex_layout);
    qtest_add_func("/ati-rv100-3d/perspective", ati_rv100_perspective);
    return g_test_run();
}
