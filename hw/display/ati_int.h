/*
 * QEMU ATI SVGA emulation
 *
 * Copyright (c) 2019 BALATON Zoltan
 *
 * This work is licensed under the GNU GPL license version 2 or later.
 */

#ifndef ATI_INT_H
#define ATI_INT_H

#include "qemu/bitmap.h"
#include "qemu/timer.h"
#include "qemu/units.h"
#include "hw/pci/pci_device.h"
#include "hw/i2c/bitbang_i2c.h"
#include "hw/display/i2c-ddc.h"
#include "vga_int.h"
#include "qom/object.h"

/*#define DEBUG_ATI*/

#ifdef DEBUG_ATI
#define DPRINTF(fmt, ...) printf("%s: " fmt, __func__, ## __VA_ARGS__)
#else
#define DPRINTF(fmt, ...) do {} while (0)
#endif

#define PCI_VENDOR_ID_ATI 0x1002
/* Rage128 Pro GL */
#define PCI_DEVICE_ID_ATI_RAGE128_PF 0x5046
/* Rage128 GL, AGP 1x/2x (RRG-G04100-C DEVICE_ID, "RF") */
#define PCI_DEVICE_ID_ATI_RAGE128_RF 0x5246
/* Radeon RV100 (VE) */
#define PCI_DEVICE_ID_ATI_RADEON_QY 0x5159

#define ATI_RAGE128_LINEAR_APER_SIZE (64 * MiB)
#define ATI_R100_LINEAR_APER_SIZE (128 * MiB)
/*
 * The register BAR.  A Rage 128 decodes 16 KiB, its upper 8 KiB a mirror of
 * the lower (RRG 2.2.1).  The Radeon's is 64 KiB (the rx2600's RV100 sizes
 * it so) and has registers of its own in 2000h-3FFFh: the setup engine, TCL
 * and the 3D and 2D caches (Linux radeon_reg.h, RADEON_SE_PORT_DATA0 2000h
 * to RADEON_RB2D_DSTCACHE_CTLSTAT 342Ch); radeonfb maps 4000h of it
 * (include/video/radeon.h RADEON_REGSIZE).
 */
#define ATI_RAGE128_MMIO_SIZE (16 * KiB)
#define ATI_R100_MMIO_SIZE (64 * KiB)
#define ATI_HOST_DATA_ACC_BITS 128

#define TYPE_ATI_VGA "ati-vga"
OBJECT_DECLARE_SIMPLE_TYPE(ATIVGAState, ATI_VGA)

typedef struct ATIVGARegs {
    uint32_t mm_index;
    uint32_t clock_cntl_index;
    uint32_t pll_regs[64];
    uint32_t init_aux[40];
    uint32_t pm4_dl_rptr;
    uint32_t pm4_dl_wptr;
    uint32_t crtc_vblank_ack_frame;
    uint32_t bios_scratch[8];
    uint32_t gui_scratch[6];
    uint32_t gen_int_cntl;
    uint32_t gen_int_status;
    uint32_t crtc_gen_cntl;
    uint32_t crtc_ext_cntl;
    uint32_t dac_cntl;
    uint32_t gpio_vga_ddc;
    uint32_t gpio_dvi_ddc;
    uint32_t gpio_monid;
    uint32_t config_cntl;
    uint32_t palette[256];
    uint32_t crtc_h_total_disp;
    uint32_t crtc_h_sync_strt_wid;
    uint32_t crtc_v_total_disp;
    uint32_t crtc_v_sync_strt_wid;
    uint32_t crtc_offset;
    uint32_t crtc_offset_cntl;
    uint32_t crtc_pitch;
    uint32_t cur_offset;
    uint32_t cur_hv_pos;
    uint32_t cur_hv_offs;
    uint32_t cur_color0;
    uint32_t cur_color1;
    uint32_t dst_offset;
    uint32_t dst_pitch;
    uint32_t dst_tile;
    uint32_t dst_width;
    uint32_t dst_height;
    uint32_t src_offset;
    uint32_t src_pitch;
    uint32_t src_tile;
    uint32_t src_x;
    uint32_t src_y;
    uint32_t dst_x;
    uint32_t dst_y;
    uint32_t dp_gui_master_cntl;
    uint32_t dp_brush_bkgd_clr;
    uint32_t dp_brush_frgd_clr;
    uint32_t brush_y_x;
    uint32_t brush_data0;
    uint32_t brush_data1;
    uint32_t dst_line_start;
    uint32_t dp_src_frgd_clr;
    uint32_t dp_src_bkgd_clr;
    uint32_t clr_cmp_cntl;
    uint32_t clr_cmp_clr_src;
    uint32_t clr_cmp_clr_dst;
    uint32_t clr_cmp_msk;
    uint16_t sc_top;
    uint16_t sc_left;
    uint16_t sc_bottom;
    uint16_t sc_right;
    uint16_t src_sc_bottom;
    uint16_t src_sc_right;
    uint32_t dp_cntl;
    uint32_t dp_datatype;
    uint32_t dp_mix;
    uint32_t dp_write_mask;
    uint32_t default_offset;
    uint32_t default_pitch;
    uint16_t default_sc_bottom;
    uint16_t default_sc_right;
    uint32_t default_tile;
    /*
     * 2D setup engine (SCALE_3D_CNTL 0x1a00, SETUP_CNTL 0x1bc4) and its
     * per-channel colour DDA (0x1a40-0x1a60).  The Windows driver draws
     * window-caption gradients by filling a rectangle with COLOR_FCN=Gouraud:
     * each channel c has a value at the primitive origin plus screen-space
     * slopes, all signed 16.16.  su_gouraud_armed is set when a fresh colour
     * plane is loaded; it stays armed for every clip-rectangle blit of the
     * same gradient (GDI splits one DrvGradientFill into several scissored
     * blits that share one colour plane and destination rectangle), and is
     * released when a blit with a different destination rectangle arrives.
     * su_gouraud_rect{,_valid} record that shared destination rectangle;
     * they are transient burst scratch (never span a savevm, so unmigrated).
     */
    uint32_t scale_3d_cntl;
    uint32_t setup_cntl;
    int32_t su_color_dx[3];   /* R,G,B  d(colour)/dx, 16.16 */
    int32_t su_color_dy[3];   /* R,G,B  d(colour)/dy, 16.16 */
    int32_t su_color_val[3];  /* R,G,B  colour at primitive origin, 16.16 */
    bool su_gouraud_armed;
    bool su_gouraud_rect_valid;
    int32_t su_gouraud_rect[4];   /* dst x, y, w, h of the active gradient */
    /*
     * Radeon only: the registers of ati_r100_aux_offs, the surfaces' lower
     * bound, upper bound and info, CONFIG_MEMSIZE and GPIO_CRT2_DDC.
     */
    uint32_t r100_aux[40];
    uint32_t surface[8][3];
    uint32_t config_memsize;
    uint32_t gpio_crt2_ddc;
} ATIVGARegs;

/*
 * The RV100's 3D engine (ati_r100_3d.c): its register context at 1C00h-1DFFh,
 * the setup-engine registers outside it, the fog table, the vertex arrays of
 * 3D_LOAD_VBPNTR and the vertices written to SE_PORT_DATA.
 */
#define ATI_R100_CONTEXT_DWORDS 128
#define ATI_R100_FOG_TABLE_ENTRIES 256
#define ATI_R100_MAX_VERTEX_DWORDS 4096
#define ATI_R100_MAX_VERTEX_ARRAYS 12

typedef struct ATIR100VertexArray {
    uint32_t address;
    uint16_t stride;
    uint8_t components;
    uint8_t reserved;
} ATIR100VertexArray;

typedef struct ATIR100State {
    uint32_t context[ATI_R100_CONTEXT_DWORDS];
    uint32_t re_top_left;
    uint32_t re_misc;
    uint32_t se_vtx_fmt;
    uint32_t se_vf_cntl;
    uint32_t se_cntl_status;
    uint8_t fog_table[ATI_R100_FOG_TABLE_ENTRIES];
    uint8_t fog_table_index;
    ATIR100VertexArray vertex_array[ATI_R100_MAX_VERTEX_ARRAYS];
    uint32_t vertex_array_count;
    uint32_t port_data[ATI_R100_MAX_VERTEX_DWORDS];
    uint32_t port_data_count;
    uint32_t port_data_expected;
    uint64_t submitted_primitives;
    uint64_t rejected_commands;
    /*
     * Transient: the work limits of the draw in progress, and a counter that
     * invalidates the cached draw state when a register changes.
     */
    uint64_t draw_budget_remaining;
    uint64_t command_work_remaining;
    bool command_budget_exhausted;
    uint64_t generation;
} ATIR100State;

typedef struct ATIHostDataState {
    bool active;
    uint32_t row;
    uint32_t col;
    uint32_t next;
    uint32_t acc[4];
} ATIHostDataState;

struct ATIVGAState {
    PCIDevice dev;
    VGACommonState vga;
    char *model;
    uint16_t dev_id;
    uint8_t mode;
    uint8_t use_pixman;
    bool cursor_guest_mode;
    bool agp;
    bool ddc_vga;
    uint16_t cursor_size;
    uint32_t cursor_offset;
    QEMUCursor *cursor;
    /* Register offsets already reported as unimplemented (-d unimp). */
    DECLARE_BITMAP(unimp_seen, ATI_R100_MMIO_SIZE / 4);
    /* PLL indices written since reset, and those reported as unimplemented. */
    uint64_t pll_written;
    uint64_t pll_unimp_seen;
    /* Surface settings the model does not follow, reported once. */
    bool surface_unimp_seen;
    QEMUTimer vblank_timer;
    bitbang_i2c_interface bbi2c;
    I2CDDCState i2cddc;
    uint64_t linear_aper_sz;
    MemoryRegion linear_aper;
    MemoryRegion io;
    MemoryRegion mm;
    ATIVGARegs regs;
    ATIHostDataState host_data;
    /*
     * The guest's values of the VGA registers that vga.c's VBE path
     * overwrites while it drives the extended display (ati.c).
     */
    bool vga_held;
    uint8_t vga_gr[2];
    uint8_t vga_cr[7];
    PortioList vga_port_list;
    /*
     * Indirect-buffer launch state (PM4_IW_INDOFF / PM4_IW_INDSIZE).  Transient
     * scratch consumed the instant INDSIZE is written, so it never spans a
     * savevm; cce_in_indirect breaks a buffer that recursively launches itself.
     */
    uint32_t cce_indoff;
    uint32_t cce_indsize;
    bool cce_in_indirect;
    /* The Radeon CP is running its ring (ati_r100_cp_run); transient. */
    bool r100_cp_active;
    ATIR100State r100;
    bool r100_3d;           /* x-r100-3d: the 3D engine draws */
};

const char *ati_reg_name(int num);

/*
 * The 2D engine's X/Y coordinate registers are 14-bit signed (RRG: DST_X/DST_Y
 * "range -8192 to 8191").  A window dragged off the top or left edge produces
 * negative coordinates, so sign-extend bit 13 rather than treating the field
 * as unsigned.
 */
/* The Rage 128 chips share one register layout; the Radeon's differs. */
static inline bool ati_is_rage128(const ATIVGAState *s)
{
    return s->dev_id == PCI_DEVICE_ID_ATI_RAGE128_PF ||
           s->dev_id == PCI_DEVICE_ID_ATI_RAGE128_RF;
}

static inline int ati_sext14(unsigned v)
{
    return ((int)(v & 0x3fff) ^ 0x2000) - 0x2000;
}

uint32_t ati_mc_vram_offset(ATIVGAState *s, uint32_t mc);
uint32_t ati_r100_reg(ATIVGAState *s, hwaddr addr);

/* RV100 3D engine (ati_r100_3d.c). */
bool ati_r100_3d_read(ATIVGAState *s, hwaddr addr, unsigned int size,
                      uint32_t *val);
bool ati_r100_3d_write(ATIVGAState *s, hwaddr addr, uint64_t data,
                       unsigned int size);
bool ati_r100_3d_packet3(ATIVGAState *s, unsigned int opcode,
                         const uint32_t *payload, unsigned int count);
void ati_r100_3d_reset(ATIVGAState *s);
int ati_r100_3d_post_load(ATIVGAState *s);
extern const VMStateDescription vmstate_ati_r100_3d;
bool ati_r100_gpu_vram_offset(ATIVGAState *s, uint64_t address,
                              uint64_t length, uint64_t *offset);
bool ati_r100_gpu_access_valid(ATIVGAState *s, uint64_t address,
                               uint64_t length, bool is_write);
bool ati_r100_gpu_ranges_overlap(ATIVGAState *s, uint64_t first,
                                 uint64_t first_length, uint64_t second,
                                 uint64_t second_length, bool *overlap);
bool ati_r100_gpu_read(ATIVGAState *s, uint64_t address, void *buf,
                       uint64_t length);
bool ati_r100_gpu_write(ATIVGAState *s, uint64_t address, const void *buf,
                        uint64_t length, bool dirty);
bool ati_2d_tile_offset(const ATIVGAState *s, uint32_t base, uint32_t pitch,
                        unsigned int cpp, unsigned int tile, uint32_t xbyte,
                        uint32_t y, uint64_t *offset);
void ati_2d_blt(ATIVGAState *s);
void ati_2d_line(ATIVGAState *s, uint32_t start_yx, uint32_t end_yx);
bool ati_host_data_flush(ATIVGAState *s);
void ati_host_data_finish(ATIVGAState *s);

/*
 * CCE ring reader shared with the 3D/scale engine (ati_3d.c).  A packet
 * handler consumes payload dwords from the ring through ati_cce_next(),
 * bounded by ati_cce_has(); ati_cce_vm_dword() reads an arbitrary card
 * (GART-translated) address, used to fetch indexed vertex buffers.
 */
typedef struct ATICCEReader {
    ATIVGAState *s;
    uint32_t base;
    uint32_t mask;
    uint32_t rptr;
    uint32_t count;
    uint32_t pos;
} ATICCEReader;

uint32_t ati_cce_vm_dword(ATIVGAState *s, uint32_t vm);
uint32_t ati_cce_next(ATICCEReader *r);
bool ati_cce_has(const ATICCEReader *r, unsigned n);

/* RAGE 128 3D/scale pipeline (ati_3d.c). */
void ati_scale_blt(ATIVGAState *s, uint32_t gmc, const uint32_t db[11],
                   bool trans);
bool ati_3d_gen_prim(ATIVGAState *s, ATICCEReader *rd, bool indexed);
bool ati_setup_gouraud_fill(ATIVGAState *s);

#endif /* ATI_INT_H */
