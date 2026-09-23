/*
 * QEMU ATI Mach64 "3D Rage" (DEV_4754) register definitions
 *
 * Register numbers here are the Mach64 "IOPortTag" block index (tag >> 2 &
 * 0x1ff), NOT a byte offset.  A guest reaches a register through MMIO BAR2:
 * Block 0 registers (index 0x00-0xff, the CRTC/DAC/config plus the GUI engine)
 * sit at BAR2 + 0x400 + index*4; Block 1 registers (index 0x100-0x1ff, the
 * overlay/scaler, not modelled here yet) sit at BAR2 + (index-0x100)*4.  See
 * xf86-video-mach64 atimach64io.h and the Mach64 Register Reference Guide.
 *
 * This work is licensed under the GNU GPL license version 2 or later.
 */

#ifndef MACH64_REGS_H
#define MACH64_REGS_H

/* BAR2 (MMIO register aperture) size. */
#define MACH64_MMIO_SIZE        0x1000

/*
 * Decode a BAR2 byte offset to a Block-0/Block-1 register index (see the file
 * header).  Block 0 lives in the upper half of the 2 KiB register window at
 * BAR2 + 0x400; Block 1 (overlay/scaler) in the lower half.
 */
#define MACH64_REG_BLOCK0_BASE  0x400
#define MACH64_OFF_TO_REG(off)                                          \
    (((off) >= MACH64_REG_BLOCK0_BASE) ?                                \
     (((off) - MACH64_REG_BLOCK0_BASE) >> 2) :                          \
     (0x100 + ((off) >> 2)))

/* Number of Block-0 register slots we back with storage. */
#define MACH64_NREGS            0x100

/* ---- Block 0: CRTC / DAC / clock / config / cursor ---- */
#define CRTC_H_TOTAL_DISP       0x00
#define CRTC_H_SYNC_STRT_WID    0x01
#define CRTC_V_TOTAL_DISP       0x02
#define CRTC_V_SYNC_STRT_WID    0x03
#define CRTC_VLINE_CRNT_VLINE   0x04
#define CRTC_OFF_PITCH          0x05
#define CRTC_INT_CNTL           0x06
#define CRTC_GEN_CNTL           0x07
#define DSP_CONFIG              0x08
#define DSP_ON_OFF              0x09
#define OVR_CLR                 0x10
#define OVR_WID_LEFT_RIGHT      0x11
#define OVR_WID_TOP_BOTTOM      0x12
#define CUR_CLR0                0x18
#define CUR_CLR1                0x19
#define CUR_OFFSET              0x1a
#define CUR_HORZ_VERT_POSN      0x1b
#define CUR_HORZ_VERT_OFF       0x1c
#define SCRATCH_REG0            0x20
#define SCRATCH_REG1            0x21
#define SCRATCH_REG2            0x22
#define SCRATCH_REG3            0x23
#define CLOCK_CNTL              0x24
/*
 * CLOCK_CNTL indirect PLL access (dword view of the byte-addressable register):
 * byte 1 holds (PLL_ADDR << 2) | PLL_WR_EN, byte 2 is the PLL data window.
 */
#define CLOCK_CNTL_PLL_WR_EN      0x00000200ul   /* byte 1, bit 1 */
#define CLOCK_CNTL_PLL_ADDR_MASK  0x0000fc00ul   /* byte 1, bits [7:2] */
#define CLOCK_CNTL_PLL_ADDR_SHIFT 10
#define CLOCK_CNTL_PLL_DATA_MASK  0x00ff0000ul   /* byte 2 */
#define CLOCK_CNTL_PLL_DATA_SHIFT 16
#define BUS_CNTL                0x28
#define LCD_INDEX               0x29
#define LCD_DATA                0x2a
#define MEM_CNTL                0x2c

/*
 * The DDC (monitor EDID) I2C bus is exposed as LCD register 7, reached through
 * the LCD_INDEX/LCD_DATA indirection.  Each of the two lines follows the
 * Mach64 "monitor-ID pin" direction+state open-drain model, split across two
 * bytes of the 32-bit LCD_DATA register (the miniport accesses each byte on
 * its own, read-modify-write, via ReadLcdRegisterUchar/WriteLcdRegisterUchar):
 *
 *   byte 3 (bits 24-31) = DIRECTION: SDA=bit29, SCL=bit30 (1 = output/drive-enable)
 *   byte 1 (bits 8-15)  = STATE:     SDA=bit13, SCL=bit14 (driven value while an
 *       output; the live wired-AND bus level while an input, so a released SDA
 *       reads 0 when a slave pulls it low to ACK).
 * A line is pulled low only when it is an output with state 0; otherwise it is
 * released.  Within a byte the two lines sit at bit5 (SDA) and bit6 (SCL).
 */
#define MACH64_LCD_DDC_INDEX    0x07
#define MACH64_DDC_SDA          (1u << 5)      /* SDA within a LCD_DATA byte */
#define MACH64_DDC_SCL          (1u << 6)      /* SCL within a LCD_DATA byte */
#define MACH64_DDC_STATE_BYTE   1
#define MACH64_DDC_DIR_BYTE     3
#define MACH64_DDC_SDA_ST       (1u << 13)     /* dword: SDA state (byte1 bit5) */
#define MACH64_DDC_SCL_ST       (1u << 14)     /* dword: SCL state (byte1 bit6) */
#define MACH64_DDC_SDA_DIR      (1u << 29)     /* dword: SDA dir   (byte3 bit5) */
#define MACH64_DDC_SCL_DIR      (1u << 30)     /* dword: SCL dir   (byte3 bit6) */

/*
 * Rage XL hardware I2C engine control (Block-0 reg 0x0F, "I2C_CNTL_0" in the
 * Rage Pro register set; the Rage XL/XC guide leaves 0_0F undocumented but the
 * silicon inherits it).  The native ATI miniport (ati2mpad) uses this engine for
 * CRT-monitor DDC: it loads the transfer into I2C_CNTL_1 and I2C_CNTL_0
 * (START/GO/clock-divider), then polls the low-byte status field until
 * I2C_CNTL_DONE goes high.  Our engine has no latency, so a transfer is complete
 * the instant it is issued and the status always reads DONE with no error;
 * without this the miniport spins forever in its CRT-DDC probe.  (This is a
 * distinct path from the LCD-register-7 DDC above, which the miniport bit-bangs
 * for the LCD/DFP.)
 */
#define I2C_CNTL_0              0x0f
#define I2C_CNTL_STAT           0x0000000ful   /* byte 0: status field */
#define I2C_CNTL_DONE           0x00000001ul   /* transfer complete */
#define I2C_CNTL_NACK           0x00000002ul   /* slave did not acknowledge */
#define I2C_CNTL_GO             0x00000400ul   /* byte 1: issue the transfer */
#define MEM_VGA_WP_SEL          0x2d
#define MEM_VGA_RP_SEL          0x2e
#define MEM_VGA_PS0             0x0000fffful   /* page of the 0xA0000 window */
#define MEM_VGA_PS1_SHIFT       16             /* page of the 0xA8000 window */
#define MEM_VGA_PAGE_SIZE       0x8000
#define DAC_REGS                0x30
#define DAC_CNTL                0x31
#define GEN_TEST_CNTL           0x34
#define CUSTOM_MACRO_CNTL       0x35
#define CONFIG_CNTL             0x37
#define CFG_MEM_AP_SIZE         0x00000003ul   /* bits 1:0, read-only */
#define CFG_MEM_AP_SIZE_2X8M    0x00000002ul
#define CFG_MEM_VGA_AP_EN       0x00000004ul   /* bit 2 */
#define CONFIG_CHIP_ID          0x38
#define CONFIG_STAT0            0x39

/* ---- Block 0: GUI (2D) engine ---- */
#define DST_OFF_PITCH           0x40
#define DST_X                   0x41
#define DST_Y                   0x42
#define DST_Y_X                 0x43
#define DST_WIDTH               0x44
#define DST_HEIGHT              0x45
#define DST_HEIGHT_WIDTH        0x46
#define DST_X_WIDTH             0x47
#define DST_BRES_LNTH           0x48
#define DST_BRES_ERR            0x49
#define DST_BRES_INC            0x4a
#define DST_BRES_DEC            0x4b
#define DST_CNTL                0x4c
#define DST_Y_X_ALIAS           0x4d           /* RAGE XL RRG: DST_Y_X alias */
#define SRC_OFF_PITCH           0x60
#define SRC_X                   0x61
#define SRC_Y                   0x62
#define SRC_Y_X                 0x63
#define SRC_WIDTH1              0x64
#define SRC_HEIGHT1             0x65
#define SRC_HEIGHT1_WIDTH1      0x66
#define SRC_X_START             0x67
#define SRC_Y_START             0x68
#define SRC_Y_X_START           0x69
#define SRC_HEIGHT2_WIDTH2      0x6a
#define SRC_CNTL                0x6d
#define HOST_DATA0              0x80
#define HOST_DATA1              0x81
#define HOST_DATA2              0x82
#define HOST_DATA3              0x83
#define HOST_DATA4              0x84
#define HOST_DATA5              0x85
#define HOST_DATA6              0x86
#define HOST_DATA7              0x87
#define HOST_DATA8              0x88
#define HOST_DATA9              0x89
#define HOST_DATAA              0x8a
#define HOST_DATAB              0x8b
#define HOST_DATAC              0x8c
#define HOST_DATAD              0x8d
#define HOST_DATAE              0x8e
#define HOST_DATAF              0x8f
#define HOST_CNTL               0x90
#define PAT_REG0                0xa0
#define PAT_REG1                0xa1
#define PAT_CNTL                0xa2
#define SC_LEFT                 0xa8
#define SC_RIGHT                0xa9
#define SC_LEFT_RIGHT           0xaa
#define SC_TOP                  0xab
#define SC_BOTTOM               0xac
#define SC_TOP_BOTTOM           0xad
#define DP_BKGD_CLR             0xb0
#define DP_FRGD_CLR             0xb1
#define DP_WRITE_MASK           0xb2
#define DP_CHAIN_MASK           0xb3
#define DP_PIX_WIDTH            0xb4
#define DP_MIX                  0xb5
#define DP_SRC                  0xb6
#define DP_SET_GUI_ENGINE       0xbf

/*
 * DP_SET_GUI_ENGINE (RAGE XL Register Reference sec 5.2): one write programs
 * the whole data path.  SET_DST_PITCH anchors the low fields -- it is 4 (640)
 * on a 640-wide guest and 8 (1024) on a 1024-wide one -- and SET_DRAWING_COMBO
 * the high ones: at bits 23:20 the values XP and Server 2003 write decode to
 * the defined rows of RRG Table 5-12 (1, 2, 3, 5, 11, 13) and nowhere else.
 * Only the fields the model can act on are decoded.
 */
#define DP_SGE_DST_PIX_WIDTH    0x00000038ul   /* 000 = mono, 010 = 8 bpp ... */
#define DP_SGE_DST_PIX_WIDTH_SHIFT 3
#define DP_SGE_SRC_PIX_WIDTH    0x00000040ul   /* 0 = mono, 1 = same as dst   */
#define DP_SGE_DST_OFFSET       0x00000380ul
#define DP_SGE_DST_OFFSET_SHIFT 7
#define DP_SGE_DST_PITCH        0x00003c00ul
#define DP_SGE_DST_PITCH_SHIFT  10
#define DP_SGE_DST_PITCH_BY_2   0x00004000ul
#define DP_SGE_SRC_OFFPITCH_COPY 0x00008000ul
#define DP_SGE_SRC_HGTWID       0x00030000ul
#define DP_SGE_SRC_HGTWID_SHIFT 16
#define DP_SGE_DRAWING_COMBO    0x00f00000ul
#define DP_SGE_DRAWING_COMBO_SHIFT 20
#define DST_X_Y                 0xba
#define DST_WIDTH_HEIGHT        0xbb
#define CLR_CMP_CLR             0xc0
#define CLR_CMP_MSK             0xc1
#define CLR_CMP_CNTL            0xc2
#define FIFO_STAT               0xc4
#define CONTEXT_MASK            0xc8
#define CONTEXT_LOAD_CNTL       0xcb
#define GUI_TRAJ_CNTL           0xcc
#define GUI_STAT                0xce

/* ---- CRTC_GEN_CNTL bits ---- */
#define CRTC_PIX_WIDTH          0x00000700ul
#define CRTC_PIX_WIDTH_SHIFT    8
#define CRTC_EXT_DISP_EN        0x01000000ul
#define CRTC_EN                 0x02000000ul

/* ---- CRTC_H_TOTAL_DISP / CRTC_V_TOTAL_DISP ---- */
#define CRTC_H_DISP             0x01ff0000ul   /* (chars - 1) in high half */
#define CRTC_V_DISP             0x07ff0000ul   /* (lines - 1) in high half */

/* ---- CRTC_OFF_PITCH ---- */
#define CRTC_OFFSET_MASK        0x000ffffful   /* in units of 8 bytes */
#define CRTC_PITCH_MASK         0xffc00000ul   /* in units of 8 pixels */
#define CRTC_PITCH_SHIFT        22

/* ---- CRTC_INT_CNTL (reg 0x06): interrupt enable + status ---- */
#define CRTC_VBLANK             0x00000001ul   /* vblank status (read-only) */
#define CRTC_VBLANK_INT_EN      0x00000002ul
#define CRTC_VBLANK_INT         0x00000004ul   /* status; write 1 to ack */
#define CRTC_VLINE_INT_EN       0x00000008ul
#define CRTC_VLINE_INT          0x00000010ul   /* status; write 1 to ack */
#define CRTC_INT_STATUS_BITS    (CRTC_VBLANK_INT | CRTC_VLINE_INT)

/* ---- CRTC_VLINE_CRNT_VLINE (reg 0x04): current scanline ---- */
#define CRTC_VLINE              0x000007fful   /* programmed compare value */
#define CRTC_CRNT_VLINE         0x07ff0000ul   /* current scanline */
#define CRTC_CRNT_VLINE_SHIFT   16

/* ---- DAC_CNTL ---- */
#define DAC_8BIT_EN             0x00000100ul

/* ---- GEN_TEST_CNTL ---- */
#define GEN_CUR_EN              0x00000080ul   /* hardware cursor enable */
#define GEN_GUI_RESETB          0x00000100ul   /* 264xT: 0 = engine in reset */
#define GEN_SOFT_RESET          0x00000200ul   /* VTB/GTB: engine soft reset */

/* ---- CONFIG_CHIP_ID ---- */
#define CFG_CHIP_TYPE           0x0000fffful   /* = PCI device id */
#define CFG_CHIP_CLASS          0x00ff0000ul
#define CFG_CHIP_REV            0xff000000ul

/* ---- CONFIG_STAT0 (CONFIG_STATUS64_0) straps a real VBIOS POST leaves ---- */
#define CFG_MEM_TYPE_T          0x00000007ul   /* 264xT memory type (bits[2:0]) */
#define CFG_VGA_EN_T            0x00000010ul   /* VT/GT VGA enable */
#define CFG_VGA_EN              0x00800000ul   /* GX/CX VGA enable */
#define CFG_CHIP_EN             0x02000000ul   /* GX/CX chip enable */
#define MEM_264_SGRAM           0x5            /* SGRAM (1:1) memory type */

/* ---- MEM_CNTL memory-size fields ---- */
#define CTL_MEM_SIZE            0x00000007ul   /* <264VTB: index into size table */
#define CTL_MEM_SIZEB          0x0000000ful    /* 264VTB+: encoded size */
#define CTL_MEM_SIZE_8M         0x5            /* videoRamSizes[5] = 8192 KiB */
#define CTL_MEM_SIZEB_8M        0xB            /* (0xB-3)*1024 = 8192 KiB */

/* ---- GUI_STAT / FIFO_STAT ---- */
#define GUI_ACTIVE              0x00000001ul
/*
 * GUI_STAT's GUI_FIFO field counts the DWORDs still FREE in the command FIFO
 * (RAGE XL Register Reference sec 5.2.8), the opposite sense to FIFO_STAT,
 * where a set bit marks a filled entry (sec 5.2.6).  A driver that waits for
 * room spins for ever on a zero here.  The depth follows
 * CMDFIFO_SIZE_MODE@GUI_CNTL, whose reset mode 00 gives 512 DWORDs of PIO.
 */
#define GUI_FIFO_SHIFT          16
#define GUI_FIFO_PIO_DWORDS     512

/* ---- pixel-width codes (CRTC_PIX_WIDTH and DP_*_PIX_WIDTH nibbles) ---- */
#define PIX_WIDTH_1BPP          0x00
#define PIX_WIDTH_4BPP          0x01
#define PIX_WIDTH_8BPP          0x02
#define PIX_WIDTH_15BPP         0x03
#define PIX_WIDTH_16BPP         0x04
#define PIX_WIDTH_24BPP         0x05
#define PIX_WIDTH_32BPP         0x06

/* ---- DP_PIX_WIDTH fields ---- */
#define DP_DST_PIX_WIDTH        0x0000000ful
#define DP_SRC_PIX_WIDTH        0x00000f00ul
#define DP_SRC_PIX_WIDTH_SHIFT  8
#define DP_HOST_PIX_WIDTH       0x000f0000ul
#define DP_HOST_PIX_WIDTH_SHIFT 16
#define DP_HOST_TRIPLE_EN       0x00002000ul
#define DP_BYTE_PIX_ORDER       0x01000000ul   /* 1 = LSB first in each byte */

/* ---- DP_MIX ---- */
#define DP_BKGD_MIX             0x0000001ful
#define DP_FRGD_MIX             0x001f0000ul
#define DP_FRGD_MIX_SHIFT       16

/* ROP / mix codes (DP_FRGD_MIX and DP_BKGD_MIX). */
#define MIX_NOT_DST             0x00
#define MIX_0                   0x01
#define MIX_1                   0x02
#define MIX_DST                 0x03
#define MIX_NOT_SRC             0x04
#define MIX_XOR                 0x05
#define MIX_XNOR                0x06
#define MIX_SRC                 0x07
#define MIX_NAND                0x08
#define MIX_NOT_SRC_OR_DST      0x09
#define MIX_SRC_OR_NOT_DST      0x0a
#define MIX_OR                  0x0b
#define MIX_AND                 0x0c
#define MIX_SRC_AND_NOT_DST     0x0d
#define MIX_NOT_SRC_AND_DST     0x0e
#define MIX_NOR                 0x0f

/* ---- DP_SRC ---- */
#define DP_BKGD_SRC             0x00000007ul
#define DP_FRGD_SRC             0x00000700ul
#define DP_FRGD_SRC_SHIFT       8
#define DP_MONO_SRC             0x00030000ul
#define DP_MONO_SRC_SHIFT       16
#define DP_MONO_SRC_ALLONES     0x0
#define DP_MONO_SRC_PATTERN     0x1
#define DP_MONO_SRC_HOST        0x2
#define DP_MONO_SRC_BLIT        0x3

/* Source-select codes for DP_FRGD_SRC / DP_BKGD_SRC. */
#define SRC_BKGD                0x0            /* DP_BKGD_CLR */
#define SRC_FRGD                0x1            /* DP_FRGD_CLR */
#define SRC_HOST                0x2            /* HOST_DATA* */
#define SRC_BLIT                0x3            /* blit source pixels */
#define SRC_PATTERN             0x4            /* pattern */

/* ---- DST_CNTL ---- */
#define DST_X_DIR               0x00000001ul   /* 1 = left-to-right */
#define DST_Y_DIR               0x00000002ul   /* 1 = top-to-bottom */
#define DST_X_TILE              0x00000008ul
#define DST_Y_TILE              0x00000010ul
#define DST_LAST_PEL            0x00000020ul
#define DST_24_ROT_EN           0x00000080ul
#define DST_24_ROT              0x00000700ul
#define DST_24_ROT_SHIFT        8

/* ---- DST_WIDTH ---- */
#define DST_WIDTH_FILL_DIS      0x80000000ul   /* write without launching */

/* ---- SRC_CNTL ---- */
#define SRC_PATT_EN             0x00000001ul
#define SRC_PATT_ROT_EN         0x00000002ul
#define SRC_LINEAR_EN           0x00000004ul
#define SRC_BYTE_ALIGN          0x00000008ul
#define SRC_COLOR_REG_WRITE_EN  0x00002000ul

/* ---- PAT_CNTL ---- */
#define PAT_MONO_EN             0x00000001ul

/* ---- HOST_CNTL ---- */
#define HOST_BYTE_ALIGN         0x00000001ul

/*
 * GUI_TRAJ_CNTL (MM 0_CC) "is a composite of registers DST_CNTL, SRC_CNTL,
 * PAT_CNTL, and HOST_CNTL" (RAGE XL RRG sec 5.2.9), in that order from the
 * low byte up: DST_CNTL takes two bytes, the other three one nibble or byte
 * each.  Anchored by the DRAWING_COMBO values of RRG Table 5-12 --
 * SrcLinearEnable is 0004_0000h (SRC_CNTL bit 2), PatMonoEnable 0100_0000h
 * (PAT_CNTL bit 0) and HostByteAlign 1000_0000h (HOST_CNTL bit 0).
 */
#define GUI_TRAJ_DST_CNTL_SHIFT  0
#define GUI_TRAJ_DST_CNTL_MASK   0x0000fffful
#define GUI_TRAJ_SRC_CNTL_SHIFT  16
#define GUI_TRAJ_SRC_CNTL_MASK   0x00ff0000ul
#define GUI_TRAJ_PAT_CNTL_SHIFT  24
#define GUI_TRAJ_PAT_CNTL_MASK   0x0f000000ul
#define GUI_TRAJ_HOST_CNTL_SHIFT 28
#define GUI_TRAJ_HOST_CNTL_MASK  0xf0000000ul

/* ---- CLR_CMP_CNTL ---- */
#define CLR_CMP_FN              0x00000007ul   /* a true result keeps dst */
#define CLR_CMP_FN_FALSE        0x0
#define CLR_CMP_FN_TRUE         0x1
#define CLR_CMP_FN_NOT_EQUAL    0x4
#define CLR_CMP_FN_EQUAL        0x5
#define CLR_CMP_SRC             0x03000000ul
#define CLR_CMP_SRC_DST         0x0
#define CLR_CMP_SRC_2D          0x1

#endif /* MACH64_REGS_H */
