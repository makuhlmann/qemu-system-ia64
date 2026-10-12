/*
 * QEMU ATI Radeon R100: the 3D engine's registers and fields
 *
 * Names and values as in syunnPC's qemu-system-ia64 hw/display/ati_regs.h
 * (2a8f7bb), the R100 3D subset; they follow Linux radeon_reg.h and r100d.h
 * and Mesa's radeon_reg.h, where the RADEON_ names carry the same values.
 *
 * This work is licensed under the GNU GPL license version 2.
 */

/*
 * The R100 register names and values below are derived in part from
 * Linux drivers/gpu/drm/radeon/radeon_reg.h.  The following upstream notice
 * applies to those definitions:
 *
 * Copyright 2000 ATI Technologies Inc., Markham, Ontario, and
 *                VA Linux Systems Inc., Fremont, California.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.  IN NO EVENT SHALL
 * ATI, VA LINUX SYSTEMS AND/OR THEIR SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#ifndef ATI_R100_REGS_H
#define ATI_R100_REGS_H

#define R100_AIC_CNTL                            0x01d0
#define R100_AIC_HI_ADDR                         0x01e0
#define R100_AIC_LO_ADDR                         0x01dc
#define R100_AIC_PT_BASE                         0x01d8
#define R100_ALPHA_ARG_A_SHIFT                   0
#define R100_ALPHA_ARG_B_SHIFT                   4
#define R100_ALPHA_ARG_C_SHIFT                   8
#define R100_COLOR_MICROTILE_ENABLE              BIT(17)
#define R100_COLOR_TILE_ENABLE                   BIT(16)
#define R100_COMBINER_ARG_A_SHIFT                0
#define R100_COMBINER_ARG_B_SHIFT                5
#define R100_COMBINER_ARG_C_SHIFT                10
#define R100_COMBINER_CLAMP                      BIT(23)
#define R100_COMBINER_OP_ADD                     0
#define R100_COMBINER_OP_ADDSIGNED               2
#define R100_COMBINER_OP_BLEND                   3
#define R100_COMBINER_OP_DOT3                    4
#define R100_COMBINER_OP_SHIFT                   18
#define R100_COMBINER_OP_SUBTRACT                1
#define R100_COMBINER_SCALE_SHIFT                21
#define R100_DEPTH_FORMAT_16BIT_INT_Z            0
#define R100_DEPTH_FORMAT_24BIT_INT_Z            2
#define R100_DEPTH_FORMAT_32BIT_INT_Z            4
#define R100_DEPTH_FORMAT_MASK                   0xfU
#define R100_DIS_OUT_OF_PCI_GART_ACCESS          BIT(1)
#define R100_FOG_COLOR_MASK                      0x00ffffffU
#define R100_FOG_SOURCE_MASK                     (3U << 25)
#define R100_FOG_TABLE                           BIT(24)
#define R100_FOG_TABLE_DATA                      0x1a18
#define R100_FOG_TABLE_INDEX                     0x1a14
#define R100_FOG_USE_DEPTH                       (0U << 25)
#define R100_FOG_USE_DIFFUSE_ALPHA               (2U << 25)
#define R100_FOG_USE_SPEC_ALPHA                  (3U << 25)
#define R100_MC_FB_LOCATION                      0x0148
#define R100_PACKET3_3D_DRAW_IMMD                0x29
#define R100_PACKET3_3D_DRAW_IMMD_2              0x35
#define R100_PACKET3_3D_DRAW_INDX                0x2a
#define R100_PACKET3_3D_DRAW_INDX_2              0x36
#define R100_PACKET3_3D_DRAW_VBUF                0x28
#define R100_PACKET3_3D_DRAW_VBUF_2              0x34
#define R100_PACKET3_3D_LOAD_VBPNTR              0x2f
#define R100_PACKET3_3D_RNDR_GEN_INDX_PRIM       0x23
#define R100_PACKET3_3D_RNDR_GEN_PRIM            0x25
#define R100_PCIGART_TRANSLATE_EN                BIT(0)
#define R100_PP_ALPHA_TEST_ENABLE                BIT(23)
#define R100_PP_BORDER_COLOR_0                   0x1d40
#define R100_PP_CNTL                             0x1c38
#define R100_PP_FOG_COLOR                        0x1c18
#define R100_PP_FOG_ENABLE                       BIT(22)
#define R100_PP_MISC                             0x1c14
#define R100_PP_SPECULAR_ENABLE                  BIT(21)
#define R100_PP_TEX_PITCH_0                      0x1d08
#define R100_PP_TEX_SIZE_0                       0x1d04
#define R100_PP_TFACTOR_0                        0x1c68
#define R100_PP_TXABLEND_0                       0x1c64
#define R100_PP_TXCBLEND_0                       0x1c60
#define R100_PP_TXFILTER_0                       0x1c54
#define R100_PP_TXFORMAT_0                       0x1c58
#define R100_PP_TXOFFSET_0                       0x1c5c
#define R100_RB3D_ALPHA_BLEND_ENABLE             BIT(0)
#define R100_RB3D_BLENDCNTL                      0x1c20
#define R100_RB3D_CNTL                           0x1c3c
#define R100_RB3D_COLOROFFSET                    0x1c40
#define R100_RB3D_COLORPITCH                     0x1c48
#define R100_RB3D_COLOR_FORMAT_SHIFT             10
#define R100_RB3D_DEPTHOFFSET                    0x1c24
#define R100_RB3D_DEPTHPITCH                     0x1c28
#define R100_RB3D_PLANEMASK                      0x1d84
#define R100_RB3D_PLANE_MASK_ENABLE              BIT(1)
#define R100_RB3D_ROPCNTL                        0x1d80
#define R100_RB3D_ROP_ENABLE                     BIT(6)
#define R100_RB3D_STENCILREFMASK                 0x1d7c
#define R100_RB3D_STENCIL_ENABLE                 BIT(7)
#define R100_RB3D_ZSTENCILCNTL                   0x1c2c
#define R100_RB3D_Z_ENABLE                       BIT(8)
#define R100_RB3D_Z_WRITE_ENABLE                 BIT(30)
#define R100_RE_MISC                             0x26c4
#define R100_RE_TOP_LEFT                         0x26c0
#define R100_RE_WIDTH_HEIGHT                     0x1c44
#define R100_SE_ALPHA_SHADE_SHIFT                10
#define R100_SE_CNTL                             0x1c4c
#define R100_SE_CNTL_STATUS                      0x2140
#define R100_SE_COORD_FMT                        0x1c50
#define R100_SE_DIFFUSE_SHADE_SHIFT              8
#define R100_SE_FLAT_SHADE_VTX_SHIFT             6
#define R100_SE_FOG_SHADE_SHIFT                  14
#define R100_SE_PORT_DATA0                       0x2000
#define R100_SE_PORT_DATA_END                    0x207c
#define R100_SE_SHADE_FLAT                       1
#define R100_SE_SHADE_GOURAUD                    2
#define R100_SE_SPECULAR_SHADE_SHIFT             12
#define R100_SE_VF_CNTL                          0x2084
#define R100_SE_VPORT_XOFFSET                    0x1d9c
#define R100_SE_VPORT_XSCALE                     0x1d98
#define R100_SE_VPORT_YOFFSET                    0x1da4
#define R100_SE_VPORT_YSCALE                     0x1da0
#define R100_SE_VPORT_ZOFFSET                    0x1dac
#define R100_SE_VPORT_ZSCALE                     0x1da8
#define R100_SE_VTX_FMT                          0x2080
#define R100_STENCIL_FAIL_SHIFT                  16
#define R100_STENCIL_MASK_SHIFT                  16
#define R100_STENCIL_REF_SHIFT                   0
#define R100_STENCIL_TEST_SHIFT                  12
#define R100_STENCIL_WRITEMASK_SHIFT             24
#define R100_STENCIL_ZFAIL_SHIFT                 24
#define R100_STENCIL_ZPASS_SHIFT                 20
#define R100_TCL_BYPASS                          BIT(8)
#define R100_TXFILTER_BORDER_MODE_D3D            BIT(31)
#define R100_TXFILTER_CLAMP_BORDER               4
#define R100_TXFILTER_CLAMP_GL                   6
#define R100_TXFILTER_CLAMP_LAST                 2
#define R100_TXFILTER_CLAMP_MIRROR               1
#define R100_TXFILTER_CLAMP_S_SHIFT              23
#define R100_TXFILTER_CLAMP_T_SHIFT              27
#define R100_TXFILTER_CLAMP_WRAP                 0
#define R100_TXFILTER_LOD_BIAS_SHIFT             8
#define R100_TXFILTER_MAG_LINEAR                 BIT(0)
#define R100_TXFILTER_MAX_MIP_LEVEL_SHIFT        16
#define R100_TXFILTER_MIN_LINEAR                 (1U << 1)
#define R100_TXFILTER_MIN_LINEAR_MIP_LINEAR      (7U << 1)
#define R100_TXFILTER_MIN_LINEAR_MIP_NEAREST     (6U << 1)
#define R100_TXFILTER_MIN_MASK                   (0xfU << 1)
#define R100_TXFILTER_MIN_NEAREST                (0U << 1)
#define R100_TXFILTER_MIN_NEAREST_MIP_LINEAR     (3U << 1)
#define R100_TXFILTER_MIN_NEAREST_MIP_NEAREST    (2U << 1)
#define R100_TXFILTER_MIRROR_CLAMP_BORDER        5
#define R100_TXFILTER_MIRROR_CLAMP_GL            7
#define R100_TXFILTER_MIRROR_CLAMP_LAST          3
#define R100_TXFILTER_YUV_TO_RGB                 BIT(20)
#define R100_TXFORMAT_ALPHA_IN_MAP               BIT(6)
#define R100_TXFORMAT_DXT1                       12U
#define R100_TXFORMAT_DXT23                      14U
#define R100_TXFORMAT_DXT45                      15U
#define R100_TXFORMAT_FORMAT_MASK                0x1fU
#define R100_TXFORMAT_HEIGHT_SHIFT               12
#define R100_TXFORMAT_NON_POWER2                 BIT(7)
#define R100_TXFORMAT_PERSPECTIVE_ENABLE         BIT(31)
#define R100_TXFORMAT_ST_ROUTE_SHIFT             24
#define R100_TXFORMAT_VYUY422                    10U
#define R100_TXFORMAT_WIDTH_SHIFT                8
#define R100_TXFORMAT_YVYU422                    11U
#define R100_TXO_MACRO_TILE                      BIT(2)
#define R100_TXO_MICRO_TILE_OPT                  BIT(4)
#define R100_TXO_MICRO_TILE_X2                   BIT(3)
#define R100_VC_SWAP_MASK                        3U
#define R100_VF_COLOR_ORDER_RGBA                 BIT(6)
#define R100_VF_INDEX_SIZE_32                    BIT(11)
#define R100_VF_NUM_VERTICES_SHIFT               16
#define R100_VF_PRIM_LINE_LIST                   2
#define R100_VF_PRIM_LINE_STRIP                  3
#define R100_VF_PRIM_POINT_LIST                  1
#define R100_VF_PRIM_POLYGON                     15
#define R100_VF_PRIM_QUAD_LIST                   13
#define R100_VF_PRIM_QUAD_STRIP                  14
#define R100_VF_PRIM_RECTANGLE_LIST              8
#define R100_VF_PRIM_TRIANGLE_FAN                5
#define R100_VF_PRIM_TRIANGLE_LIST               4
#define R100_VF_PRIM_TRIANGLE_STRIP              6
#define R100_VF_PRIM_TYPE_MASK                   0xfU
#define R100_VF_PRIM_WALK_DATA                   (3U << 4)
#define R100_VF_PRIM_WALK_IND                    (1U << 4)
#define R100_VF_PRIM_WALK_LIST                   (2U << 4)
#define R100_VF_PRIM_WALK_MASK                   (3U << 4)
#define R100_VPORT_XY_XFORM_ENABLE               BIT(24)
#define R100_VPORT_Z_XFORM_ENABLE                BIT(25)
#define R100_VTX_FMT_FPALPHA                     BIT(2)
#define R100_VTX_FMT_FPCOLOR                     BIT(1)
#define R100_VTX_FMT_FPFOG                       BIT(5)
#define R100_VTX_FMT_FPSPEC                      BIT(4)
#define R100_VTX_FMT_N0                          BIT(18)
#define R100_VTX_FMT_N1                          BIT(30)
#define R100_VTX_FMT_PKCOLOR                     BIT(3)
#define R100_VTX_FMT_PKSPEC                      BIT(6)
#define R100_VTX_FMT_Q0                          BIT(14)
#define R100_VTX_FMT_Q1                          BIT(9)
#define R100_VTX_FMT_Q2                          BIT(11)
#define R100_VTX_FMT_Q3                          BIT(13)
#define R100_VTX_FMT_ST0                         BIT(7)
#define R100_VTX_FMT_ST1                         BIT(8)
#define R100_VTX_FMT_ST2                         BIT(10)
#define R100_VTX_FMT_ST3                         BIT(12)
#define R100_VTX_FMT_W0                          BIT(0)
#define R100_VTX_FMT_W1                          BIT(29)
#define R100_VTX_FMT_XY1                         BIT(27)
#define R100_VTX_FMT_Z                           BIT(31)
#define R100_VTX_FMT_Z1                          BIT(28)
#define R100_VTX_ST_NONPARAMETRIC(unit)          BIT(8 + (unit))
#define R100_VTX_W0_IS_NOT_1_OVER_W0             BIT(16)
#define R100_VTX_XY_PRE_MULT_1_OVER_W0           BIT(0)
#define R100_VTX_Z_PRE_MULT_1_OVER_W0            BIT(1)

#endif /* ATI_R100_REGS_H */
