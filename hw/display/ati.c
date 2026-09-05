/*
 * QEMU ATI SVGA emulation
 *
 * Copyright (c) 2019 BALATON Zoltan
 *
 * This work is licensed under the GNU GPL license version 2 or later.
 */

/*
 * WARNING:
 * This is very incomplete and only enough for Linux console and some
 * unaccelerated X output at the moment.
 * Currently it's little more than a frame buffer with minimal functions,
 * other more advanced features of the hardware are yet to be implemented.
 * We only aim for Rage 128 Pro (and some RV100) and 2D only at first,
 * No 3D at all yet (maybe after 2D works, but feel free to improve it)
 */

#include "qemu/osdep.h"
#include "ati_int.h"
#include "ati_regs.h"
#include "vga-access.h"
#include "hw/core/qdev-properties.h"
#include "vga_regs.h"
#include "qemu/bswap.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "migration/vmstate.h"
#include "ui/console.h"
#include "trace.h"

#define ATI_DEBUG_HW_CURSOR 0

#ifdef CONFIG_PIXMAN
#define DEFAULT_X_PIXMAN 3
#else
#define DEFAULT_X_PIXMAN 0
#endif

static const struct {
    const char *name;
    uint16_t dev_id;
} ati_model_aliases[] = {
    { "rage128p", PCI_DEVICE_ID_ATI_RAGE128_PF },
    { "rv100", PCI_DEVICE_ID_ATI_RADEON_QY },
};

enum { VGA_MODE, EXT_MODE };

static void ati_vga_set_offset(VGACommonState *vga, uint32_t offs)
{
    int bypp = DIV_ROUND_UP(vga->vbe_regs[VBE_DISPI_INDEX_BPP], BITS_PER_BYTE);

    if (!bypp ||
        vga->vbe_regs[VBE_DISPI_INDEX_YRES] *
        vga->vbe_regs[VBE_DISPI_INDEX_VIRT_WIDTH] * bypp + offs >
        vga->vbe_size) {
        return;
    }
    vga->vbe_start_addr = offs / 4;
}

static void ati_vga_switch_mode(ATIVGAState *s)
{
    DPRINTF("%d -> %d\n",
            s->mode, !!(s->regs.crtc_gen_cntl & CRTC2_EXT_DISP_EN));
    if (s->regs.crtc_gen_cntl & CRTC2_EXT_DISP_EN) {
        /* Extended mode enabled */
        s->mode = EXT_MODE;
        if (s->regs.crtc_gen_cntl & CRTC2_EN) {
            /* CRT controller enabled, use CRTC values */
            /* FIXME Should these be the same as VGA CRTC regs? */
            uint32_t offs = s->regs.crtc_offset & 0x07ffffff;
            int stride = (s->regs.crtc_pitch & 0x7ff) * 8;
            int bpp = 0;
            int h, v;

            if (s->regs.crtc_h_total_disp == 0) {
                s->regs.crtc_h_total_disp = ((640 / 8) - 1) << 16;
            }
            if (s->regs.crtc_v_total_disp == 0) {
                s->regs.crtc_v_total_disp = (480 - 1) << 16;
            }
            h = ((s->regs.crtc_h_total_disp >> 16) + 1) * 8;
            v = (s->regs.crtc_v_total_disp >> 16) + 1;
            switch (s->regs.crtc_gen_cntl & CRTC_PIX_WIDTH_MASK) {
            case CRTC_PIX_WIDTH_4BPP:
                bpp = 4;
                break;
            case CRTC_PIX_WIDTH_8BPP:
                bpp = 8;
                break;
            case CRTC_PIX_WIDTH_15BPP:
                bpp = 15;
                break;
            case CRTC_PIX_WIDTH_16BPP:
                bpp = 16;
                break;
            case CRTC_PIX_WIDTH_24BPP:
                bpp = 24;
                break;
            case CRTC_PIX_WIDTH_32BPP:
                bpp = 32;
                break;
            default:
                qemu_log_mask(LOG_UNIMP, "Unsupported bpp value\n");
                return;
            }
            DPRINTF("Switching to %dx%d %d %d @ %x\n", h, v, stride, bpp, offs);
            vbe_ioport_write_index(&s->vga, 0, VBE_DISPI_INDEX_ENABLE);
            vbe_ioport_write_data(&s->vga, 0, VBE_DISPI_DISABLED);
            s->vga.big_endian_fb = (s->regs.config_cntl & APER_0_ENDIAN ||
                                    s->regs.config_cntl & APER_1_ENDIAN ?
                                    true : false);
            /* reset VBE regs then set up mode */
            s->vga.vbe_regs[VBE_DISPI_INDEX_XRES] = h;
            s->vga.vbe_regs[VBE_DISPI_INDEX_YRES] = v;
            s->vga.vbe_regs[VBE_DISPI_INDEX_BPP] = bpp;
            /* enable mode via ioport so it updates vga regs */
            vbe_ioport_write_index(&s->vga, 0, VBE_DISPI_INDEX_ENABLE);
            vbe_ioport_write_data(&s->vga, 0, VBE_DISPI_ENABLED |
                VBE_DISPI_LFB_ENABLED | VBE_DISPI_NOCLEARMEM |
                (s->regs.dac_cntl & DAC_8BIT_EN ? VBE_DISPI_8BIT_DAC : 0));
            /* now set offset and stride because enable resets these */
            if (stride) {
                vbe_ioport_write_index(&s->vga, 0, VBE_DISPI_INDEX_VIRT_WIDTH);
                vbe_ioport_write_data(&s->vga, 0, stride);
            }
            ati_vga_set_offset(&s->vga, offs);
        }
    } else {
        /* VGA mode enabled */
        s->mode = VGA_MODE;
        vbe_ioport_write_index(&s->vga, 0, VBE_DISPI_INDEX_ENABLE);
        vbe_ioport_write_data(&s->vga, 0, VBE_DISPI_DISABLED);
    }
}

/* Used by host side hardware cursor */
static void ati_cursor_define(ATIVGAState *s)
{
    uint64_t data[128];
    uint32_t srcoff;
    unsigned hoff, voff;

    if ((s->regs.cur_offset & BIT(31)) || s->cursor_guest_mode) {
        return; /* Do not update cursor if locked or rendered by guest */
    }
    /*
     * CUR_HORZ_VERT_OFF crops the 64x64 image down to the pointer the guest
     * actually wants to show: the high half skips that many leading columns
     * and the low half shortens it to 64 - v lines.  Measured against the XP
     * driver, which stores a 32x32 pointer in the top-right quadrant and
     * programs (32, 32): its unused left half is AND=0 XOR=0, i.e. *opaque
     * black*, so failing to crop paints a black square over the screen.  The
     * cropped-away part is padded transparent (AND=1, XOR=0).
     */
    hoff = (s->regs.cur_hv_offs >> 16) & 0x3f;
    voff = s->regs.cur_hv_offs & 0x3f;
    srcoff = s->regs.cur_offset & 0x07fffff0;
    if (srcoff > s->vga.vram_size - 64 * 16) {
        return;
    }
    for (unsigned i = 0; i < 64; i++) {
        uint64_t and_row = ~0ULL, xor_row = 0;

        if (i + voff < 64) {
            const uint8_t *src = &s->vga.vram_ptr[srcoff + i * 16];

            /* Bit 7 of byte 0 is the leftmost pixel: shift big-endian. */
            and_row = ldq_be_p(src);
            xor_row = ldq_be_p(src + 8);
            if (hoff) {
                and_row = (and_row << hoff) | ((1ULL << hoff) - 1);
                xor_row <<= hoff;
            }
        }
        stq_be_p(&data[i], and_row);
        stq_be_p(&data[i + 64], xor_row);
    }
    if (!s->cursor) {
        s->cursor = cursor_alloc(64, 64);
    }
    cursor_set_mono(s->cursor, s->regs.cur_color1, s->regs.cur_color0,
                    (uint8_t *)&data[64], 1, (uint8_t *)&data[0]);
    dpy_cursor_define(s->vga.con, s->cursor);
}

/* Alternatively support guest rendered hardware cursor */
static void ati_cursor_invalidate(VGACommonState *vga)
{
    ATIVGAState *s = container_of(vga, ATIVGAState, vga);
    int size = (s->regs.crtc_gen_cntl & CRTC2_CUR_EN) ? 64 : 0;

    if (s->regs.cur_offset & BIT(31)) {
        return; /* Do not update cursor if locked */
    }
    if (s->cursor_size != size ||
        vga->hw_cursor_x != s->regs.cur_hv_pos >> 16 ||
        vga->hw_cursor_y != (s->regs.cur_hv_pos & 0xffff) ||
        s->cursor_offset != (s->regs.cur_offset & 0x07fffff0) -
        (s->regs.cur_hv_offs >> 16) -
        (s->regs.cur_hv_offs & 0xffff) * 16) {
        /* Remove old cursor then update and show new one if needed */
        vga_invalidate_scanlines(vga, vga->hw_cursor_y, vga->hw_cursor_y + 63);
        vga->hw_cursor_x = s->regs.cur_hv_pos >> 16;
        vga->hw_cursor_y = s->regs.cur_hv_pos & 0xffff;
        s->cursor_offset = (s->regs.cur_offset & 0x07fffff0) -
                           (s->regs.cur_hv_offs >> 16) -
                           (s->regs.cur_hv_offs & 0xffff) * 16;
        s->cursor_size = size;
        if (size) {
            vga_invalidate_scanlines(vga,
                                     vga->hw_cursor_y, vga->hw_cursor_y + 63);
        }
    }
}

static void ati_cursor_draw_line(VGACommonState *vga, uint8_t *d, int scr_y)
{
    ATIVGAState *s = container_of(vga, ATIVGAState, vga);
    uint32_t h, srcoff, color;
    uint64_t abits, xbits, mask;
    uint32_t *dp = (uint32_t *)d;
    unsigned hoff = (s->regs.cur_hv_offs >> 16) & 0x3f;
    unsigned voff = s->regs.cur_hv_offs & 0x3f;
    int row = scr_y - vga->hw_cursor_y;

    if (!(s->regs.crtc_gen_cntl & CRTC2_CUR_EN) ||
        row < 0 || row >= 64 ||
        scr_y > s->regs.crtc_v_total_disp >> 16) {
        return;
    }
    /*
     * CUR_HORZ_VERT_OFF crops the 64x64 image down to the pointer, exactly as
     * ati_cursor_define() does for the host-overlay path: the high half skips
     * that many leading columns (shift the row left, padding transparent) and
     * the low half shortens the image to 64 - voff lines.  Without this the
     * opaque-black padding quadrant (AND=0, XOR=0) is drawn as a black box
     * beside the pointer.
     */
    if ((unsigned)row + voff >= 64) {
        return; /* padding row past the cropped image: fully transparent */
    }
    srcoff = (s->regs.cur_offset & 0x07fffff0) + row * 16;
    if (srcoff > s->vga.vram_size - 16) {
        return;
    }
    dp = &dp[vga->hw_cursor_x];
    h = ((s->regs.crtc_h_total_disp >> 16) + 1) * 8;
    abits = ldq_be_p(&vga->vram_ptr[srcoff]);
    xbits = ldq_be_p(&vga->vram_ptr[srcoff + 8]);
    if (hoff) {
        abits = (abits << hoff) | ((1ULL << hoff) - 1);
        xbits <<= hoff;
    }
    mask = BIT_ULL(63);
    for (int i = 0; i < 64; i++, mask >>= 1) {
        if (vga->hw_cursor_x + i >= h) {
            return; /* end of screen, don't span to next line */
        }
        if (abits & mask) {
            if (xbits & mask) {
                color = dp[i] ^ 0xffffffff; /* complement */
            } else {
                continue; /* transparent, no change */
            }
        } else {
            color = (xbits & mask ? s->regs.cur_color1 :
                                    s->regs.cur_color0) | 0xff000000;
        }
        dp[i] = color;
    }
}

static uint64_t ati_i2c(bitbang_i2c_interface *i2c, uint64_t data, int base)
{
    bool c = (data & BIT(base + 17) ? !!(data & BIT(base + 1)) : 1);
    bool d = (data & BIT(base + 16) ? !!(data & BIT(base)) : 1);

    bitbang_i2c_set(i2c, BITBANG_I2C_SCL, c);
    d = bitbang_i2c_set(i2c, BITBANG_I2C_SDA, d);

    data &= ~0xf00ULL;
    if (c) {
        data |= BIT(base + 9);
    }
    if (d) {
        data |= BIT(base + 8);
    }
    return data;
}

static void ati_vga_update_irq(ATIVGAState *s)
{
    pci_set_irq(&s->dev, !!(s->regs.gen_int_status & s->regs.gen_int_cntl));
}

static void ati_vga_vblank_irq(void *opaque)
{
    ATIVGAState *s = opaque;

    timer_mod(&s->vblank_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              NANOSECONDS_PER_SECOND / 60);
    s->regs.gen_int_status |= CRTC_VBLANK_INT;
    ati_vga_update_irq(s);
}

/*
 * Synthetic CRTC raster timing, derived from virtual time: a fixed 60 Hz
 * frame of 525 lines with the last 45 in vertical blank (VGA-ish 480-line
 * visible raster).  Drivers poll these instead of taking the vblank
 * interrupt: XP's ati2draa spins on CRTC_STATUS (0x5C) waiting for the
 * vblank bits to change, and a constant readback becomes bugcheck 0xEA.
 */
#define ATI_FRAME_NS   (NANOSECONDS_PER_SECOND / 60)
#define ATI_FRAME_LINES 525
#define ATI_VISIBLE_LINES 480

static uint64_t ati_crtc_frame_count(void)
{
    return qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / ATI_FRAME_NS;
}

static uint32_t ati_crtc_current_line(void)
{
    uint64_t in_frame = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) % ATI_FRAME_NS;

    return in_frame * ATI_FRAME_LINES / ATI_FRAME_NS;
}

static uint32_t ati_crtc_status(ATIVGAState *s)
{
    uint32_t val = 0;

    /* bit 0: currently inside vertical blank */
    if (ati_crtc_current_line() >= ATI_VISIBLE_LINES) {
        val |= 1;
    }
    /* bit 1: a vblank happened since it was last acknowledged (W1C) */
    if ((uint32_t)ati_crtc_frame_count() != s->regs.crtc_vblank_ack_frame) {
        val |= 2;
    }
    return val;
}

static inline uint32_t ati_reg_read_offs(uint32_t reg, int offs,
                                         unsigned int size)
{
    if (offs == 0 && size == 4) {
        return reg;
    } else {
        return extract32(reg, offs * BITS_PER_BYTE, size * BITS_PER_BYTE);
    }
}

/*
 * Registers the XP inbox Rage 128 miniport programs during HwFindAdapter /
 * HwInitialize that carry no device-visible behaviour we model yet.  They
 * must store writes and read them back (the driver programs the memory
 * controller and PLL and verifies by readback); silently dropping the
 * writes made the init fail.  Offsets and reset values follow the RAGE 128
 * PRO Register Reference Guide.
 */
static const uint16_t ati_init_aux_offs[] = {
    0x0030, /* BUS_CNTL          */
    0x0034, /* BUS_CNTL1         */
    0x00f0, /* GEN_RESET_CNTL    */
    0x0120, 0x0124, 0x0128,
    0x0130, /* HOST_PATH_CNTL    */
    0x0140, /* MEM_CNTL          */
    0x0144, /* EXT_MEM_CNTL      */
    0x0148, /* MEM_ADDR_CONFIG   */
    0x014c, /* MEM_INTF_CNTL     */
    0x0150, /* MEM_STR_CNTL      */
    0x0154, /* MEM_INIT_LAT_TIMER */
    0x0158, /* MEM_SDRAM_MODE_REG */
    0x0168, /* PAD_CTLR_STRENGTH */
    0x0170, /* AGP_BASE          */
    0x0174, /* AGP_CNTL          */
    0x0180, /* PC_NGUI_MODE      */
    0x0184, /* PC_NGUI_CTLSTAT   */
    0x017c, /* PCI_GART_PAGE     */
    0x02e0, 0x02e4, 0x02e8, 0x02ec, /* BIOS header scratch group */
    0x0700, /* PM4_BUFFER_OFFSET  */
    0x0704, /* PM4_BUFFER_CNTL    */
    0x0708, /* PM4_BUFFER_WM_CTL  */
    0x070c, /* PM4_BUFFER_DL_RPTR_ADDR */
    0x07d4, /* PM4_MICRO_CNTL     */
    0x07dc, /* PM4_MICROCODE_ADDR */
    0x07e0, /* PM4_MICRODE_DATAH/L group */
    0x0b00,
    0x0a18, /* BM_CHUNK_0_VAL (bits 21-23 force bus mastering to PCI) */
};

static int ati_init_aux_slot(hwaddr addr)
{
    unsigned i;

    for (i = 0; i < ARRAY_SIZE(ati_init_aux_offs); i++) {
        if (ati_init_aux_offs[i] == (addr & ~3ULL)) {
            return i;
        }
    }
    return -1;
}

/*
 * CLOCK_CNTL_INDEX (0x08) / CLOCK_CNTL_DATA (0x0c) indirect PLL file.
 * Index layout on Rage 128: PLL_ADDR in the low bits (drivers mask 0x1f or
 * 0x3f), PLL_WR_EN at bit 7, PPLL_DIV_SEL at bits 9:8.  The only handshake
 * on this chip is PPLL_ATOMIC_UPDATE: bit 15 of PPLL_REF_DIV (0x03) and of
 * PPLL_DIV_0..3 (0x04..0x07) reads as "update pending" and hardware clears
 * it when the divider update lands; we complete updates instantly, so those
 * reads mask bit 15.
 */
static uint32_t ati_pll_read(ATIVGAState *s)
{
    unsigned idx = s->regs.clock_cntl_index & 0x3f;
    uint32_t val;

    if (idx >= ARRAY_SIZE(s->regs.pll_regs)) {
        return 0;
    }
    val = s->regs.pll_regs[idx];
    if (idx >= 0x03 && idx <= 0x07) {
        val &= ~0x8000u;
    }
    return val;
}

/* PLL_TEST_CNTL (PLL:0x13): TEST_COUNT occupies bits 31:24. */
#define ATI_PLL_TEST_CNTL 0x13

/*
 * PLL_TEST_CNTL.TEST_COUNT (bits 31:24) is a free-running counter clocked by
 * whatever TEST_DEBUG_MUX.TEST_DEBUG_CLK muxes in -- the vendor vgabios points
 * it at Xtalin and builds every hardware-timed delay by clearing TEST_COUNT and
 * spinning until it crosses a threshold (RRG PLL_TEST_CNTL; delay library at
 * vgabios 0x6118).  The byte therefore has to advance each time it is sampled
 * or those loops never terminate and firmware init grinds through the full
 * software timeout on every microsecond delay.  Advance it by a coarse step per
 * sample: the guest only observes the count crossing its threshold (0x1d/0x91),
 * and a large step keeps the loops -- real-time on hardware -- short here.
 */
static void ati_pll_tick_test_count(ATIVGAState *s)
{
    uint32_t v = s->regs.pll_regs[ATI_PLL_TEST_CNTL];
    uint32_t cnt = ((v >> 24) + 0x40) & 0xff;

    s->regs.pll_regs[ATI_PLL_TEST_CNTL] = (v & 0x00ffffffu) | (cnt << 24);
}

static void ati_mm_write(void *opaque, hwaddr addr, uint64_t data,
                         unsigned int size);

#define R128_PM4_BUFFER_DL_DONE  (1u << 31)

/*
 * Concurrent Command Engine ring buffer.
 *
 * The XP inbox display driver (ati2draa) and the r128 DRM submit most 2D
 * work as PM4 packets through a ring buffer in bus-addressable memory:
 * PM4_BUFFER_OFFSET holds the ring base, PM4_BUFFER_CNTL carries the mode
 * bits plus log2(ring size in qwords), and PM4_BUFFER_DL_WPTR (a dword
 * index, bit 31 = "list done" flag) advances the tail.  Type-0/1 packets
 * are register writes and replay through the ordinary MMIO path, driving
 * the same blitter the PIO path uses.  Unknown type-3 opcodes are skipped
 * by their length field.  Packet encodings follow the RAGE 128 PM4
 * specification as used by XFree86 4.x r128_reg.h and the Linux r128 DRM.
 */
/*
 * PM4 addresses are card "VM space" addresses, not bus addresses.  Three
 * translation regimes, in priority order:
 *
 * 1. The r128 PCI pseudo-GART (PCI_GART_PAGE, 0x017c): a page table of
 *    little-endian physical page addresses.  The XP inbox driver runs the ring
 *    at VM offset 0 with the bus-master force-to-PCI bits set (BM_CHUNK_0_VAL
 *    bits 21-23); the r128 DRM in PCI (scatter-gather) mode uses this too.
 *    This GART is 32-bit and cannot reach DRAM above 4 GiB.
 *
 * 2. AGP mode: the r128 DRM, when agpgart is present, places its ring and
 *    buffers in AGP memory and points AGP_BASE (0x0170) at the chipset AGP
 *    aperture bus base (the i460 gart_bus_addr).  Two kinds of card address
 *    reach here in this mode, and the DRM addresses them differently:
 *      - the *ring* is imaged in the card's MC space at a fixed offset
 *        R128_AGP_OFFSET (0x02000000; the "AGP system memory image" of
 *        RRG-G04500-C sec 2.2.1, at AGP_APER_OFFSET), so PM4_BUFFER_OFFSET is
 *        AGP_BASE-relative: a vm in [R128_AGP_OFFSET, AGP_BASE) maps to AGP bus
 *        AGP_BASE + (vm - R128_AGP_OFFSET);
 *      - an *indirect buffer* is referenced by PM4_IW_INDOFF = buf->bus_address
 *        (Linux drm r128_cce_dispatch_indirect), which is already the absolute
 *        AGP bus address (dev->agp->base + offset), i.e. vm >= AGP_BASE, and is
 *        used as-is.
 *    Either way the resulting AGP bus address is issued through the 460GX GXB
 *    IOMMU, whose GART relocates the 4 KiB aperture page to 36-bit DRAM --
 *    reaching above 4 GiB.
 *
 * 3. Otherwise low addresses are local video memory, and anything past it is a
 *    raw bus address.
 */
#define R128_AGP_OFFSET 0x02000000u

uint32_t ati_cce_vm_dword(ATIVGAState *s, uint32_t vm)
{
    int gart_slot = ati_init_aux_slot(0x017c); /* PCI_GART_PAGE */
    uint32_t gart = gart_slot >= 0 ? s->regs.init_aux[gart_slot] & ~0xfffu : 0;
    int agp_slot = ati_init_aux_slot(0x0170);   /* AGP_BASE */
    uint32_t agp_base = agp_slot >= 0 ? s->regs.init_aux[agp_slot] : 0;
    uint32_t le;

    if (gart) {
        uint32_t off = vm >= R128_AGP_OFFSET ? vm - R128_AGP_OFFSET : vm;
        uint32_t ent;

        pci_dma_read(&s->dev, gart + (off >> 12) * 4, &ent, sizeof(ent));
        ent = le32_to_cpu(ent);
        pci_dma_read(&s->dev, (ent & ~0xfffu) | (off & 0xfff), &le,
                     sizeof(le));
        return le32_to_cpu(le);
    }
    if (agp_base && vm >= R128_AGP_OFFSET) {
        hwaddr agp_bus = vm >= agp_base ?
                         vm :                                 /* INDOFF: absolute */
                         (hwaddr)agp_base + (vm - R128_AGP_OFFSET);  /* ring image */

        pci_dma_read(&s->dev, agp_bus, &le, sizeof(le));
        return le32_to_cpu(le);
    }
    if (vm < s->vga.vram_size - 3) {
        return ldl_le_p(s->vga.vram_ptr + vm);
    }
    pci_dma_read(&s->dev, vm, &le, sizeof(le));
    return le32_to_cpu(le);
}

static uint32_t ati_cce_ring_dword(ATIVGAState *s, uint32_t base,
                                   uint32_t mask, uint32_t idx)
{
    return ati_cce_vm_dword(s, base + ((idx & mask) << 2));
}

/*
 * Type-3 engine-command packets, as emitted by the XP inbox ati2draa
 * display driver (measured from a live ring) and the r128 DRM
 * (Linux 2.4.18 drivers/char/drm/r128_state.c).  Payload dwords are the
 * values of documented GUI registers, so each packet decomposes into the
 * same ati_mm_write() sequence a PIO client would issue; the write to the
 * width/height register fires the blit.
 *
 * Layout common to the CNTL ops: DP_GUI_MASTER_CNTL first, and its low
 * bits announce which state dwords follow: bit 0 = SRC_PITCH_OFFSET,
 * bit 1 = DST_PITCH_OFFSET, bit 3 = SC_TOP_LEFT + SC_BOTTOM_RIGHT.
 * The brush field then decides brush colour dwords (solid = one
 * DP_BRUSH_FRGD_CLR; none = nothing), and the trailing dwords are
 * per-rectangle coordinates.  NEXT_CHAR carries no GMC at all - it draws
 * one glyph with the state a preceding HOSTDATA_BLT packet loaded.
 */
uint32_t ati_cce_next(ATICCEReader *r)
{
    if (r->pos >= r->count) {
        return 0;
    }
    return ati_cce_ring_dword(r->s, r->base, r->mask, r->rptr + r->pos++);
}

bool ati_cce_has(const ATICCEReader *r, unsigned n)
{
    return r->pos + n <= r->count;
}

static bool ati_cce_gmc_prefix(ATIVGAState *s, ATICCEReader *rd,
                               uint32_t *gmc_out)
{
    uint32_t gmc = ati_cce_next(rd);

    ati_mm_write(s, DP_GUI_MASTER_CNTL, gmc, 4);
    if (gmc & GMC_SRC_PITCH_OFFSET_CNTL) {
        ati_mm_write(s, SRC_PITCH_OFFSET, ati_cce_next(rd), 4);
    }
    if (gmc & GMC_DST_PITCH_OFFSET_CNTL) {
        ati_mm_write(s, DST_PITCH_OFFSET, ati_cce_next(rd), 4);
    }
    if (gmc & GMC_SRC_CLIPPING) {
        return false;   /* not emitted by known clients; layout unverified */
    }
    if (gmc & GMC_DST_CLIPPING) {
        ati_mm_write(s, SC_TOP_LEFT, ati_cce_next(rd), 4);
        ati_mm_write(s, SC_BOTTOM_RIGHT, ati_cce_next(rd), 4);
    }
    *gmc_out = gmc;
    return true;
}

static void ati_cce_host_data(ATIVGAState *s, ATICCEReader *rd)
{
    while (rd->pos < rd->count) {
        ati_mm_write(s, HOST_DATA0, ati_cce_next(rd), 4);
    }
    ati_host_data_finish(s);
}

/*
 * Brush-payload dwords after the GMC prefix, by GMC brush type: solid
 * colour carries DP_BRUSH_FRGD_CLR; 8x8 monochrome patterns carry
 * background/foreground colours (register-ascending order), the two
 * pattern dwords and the brush origin; brush "none" carries nothing.
 */
static bool ati_cce_paint_brush(ATIVGAState *s, ATICCEReader *rd,
                                uint32_t gmc)
{
    switch ((gmc >> 4) & 0xf) {
    case 0x0: /* 8x8 mono fg/bg */
    case 0x1: /* 8x8 mono fg/leave-alone */
        ati_mm_write(s, DP_BRUSH_BKGD_CLR, ati_cce_next(rd), 4);
        ati_mm_write(s, DP_BRUSH_FRGD_CLR, ati_cce_next(rd), 4);
        ati_mm_write(s, BRUSH_DATA0, ati_cce_next(rd), 4);
        ati_mm_write(s, BRUSH_DATA1, ati_cce_next(rd), 4);
        ati_mm_write(s, BRUSH_Y_X, ati_cce_next(rd), 4);
        return true;
    case 0xd: /* solid colour */
        ati_mm_write(s, DP_BRUSH_FRGD_CLR, ati_cce_next(rd), 4);
        return true;
    case 0xf: /* none */
        return true;
    default:  /* 32x32 and colour brush layouts unverified */
        return false;
    }
}

/*
 * A 2D drawing packet's opcode bit 7 (the "CNTL_" 0x9x form) selects whether
 * the packet carries a leading SETTINGS block (SDK Table 4-14/4-15): the GUI
 * control word (DP_GUI_MASTER_CNTL) and its optional SETUP_BODY (pitch/offset,
 * scissor, brush).  The plain 0x1x form omits SETTINGS and the engine draws
 * with the already-programmed registers.  The ATI-private glyph-cache blit
 * 0x28 also carries a SETTINGS block despite bit 7 being clear.
 *
 * Returns the effective GUI control word: the one just parsed from the packet
 * for the SETTINGS form, or the programmed DP_GUI_MASTER_CNTL otherwise.
 */
static bool ati_cce_has_settings(uint32_t op)
{
    return (op & 0x80) || op == 0x28;
}

static bool ati_cce_settings(ATIVGAState *s, ATICCEReader *rd, uint32_t op,
                             uint32_t *gmc_out)
{
    if (ati_cce_has_settings(op)) {
        return ati_cce_gmc_prefix(s, rd, gmc_out);
    }
    *gmc_out = s->regs.dp_gui_master_cntl;
    return true;
}

static bool ati_cce_execute_type3(ATIVGAState *s, uint32_t base,
                                  uint32_t mask, uint32_t rptr,
                                  uint32_t count, uint32_t op)
{
    ATICCEReader rd = { s, base, mask, rptr, count, 0 };
    uint32_t gmc;

    switch (op) {
    case 0x10: /* NOP: no payload, no effect.  The ring padder and DRM sync
                * paths emit type-3 NOPs; handle it explicitly so it is not
                * counted (and logged once) as an unhandled packet. */
        return true;
    case 0x1f: /* SET_MODE24BPP: a single flag dword (1 = set / 0 = clear the
                * engine microcode's 24bpp flag; SDK F.23, p.F-47).  It sets no
                * MMIO register -- it toggles an internal microcode flag that
                * governs HOST_DATA triplication for 24bpp destinations, which
                * this model does not have and no guest we run selects (XP/2003
                * desktops are 8/16/32bpp).  Consume it as a documented no-op so
                * it is not misreported as unhandled. */
        return true;
    case 0x19: /* NEXT_CHAR: (y,x), (h,w), inline monochrome bits */
    case 0x99: /* CNTL_NEXT_CHAR: as NEXT_CHAR with a leading SETTINGS block */
        if (ati_cce_has_settings(op) &&
            !ati_cce_gmc_prefix(s, &rd, &gmc)) {
            return false;
        }
        if (!ati_cce_has(&rd, 2)) {
            return false;
        }
        ati_mm_write(s, DST_Y_X, ati_cce_next(&rd), 4);
        ati_mm_write(s, DST_HEIGHT_WIDTH, ati_cce_next(&rd), 4);
        ati_cce_host_data(s, &rd);
        return true;
    case 0x1e: /* SET_SCISSORS */
        if (!ati_cce_has(&rd, 2)) {
            return false;
        }
        ati_mm_write(s, SC_TOP_LEFT, ati_cce_next(&rd), 4);
        ati_mm_write(s, SC_BOTTOM_RIGHT, ati_cce_next(&rd), 4);
        return true;
    case 0x18: /* POLYSCANLINES: plain form, no SETTINGS */
    case 0x98: /* CNTL_POLYSCANLINES: solid-fill a polygon as horizontal spans.
                * This packet carries only the setup (GMC prefix + the fill
                * colour); the spans themselves arrive in the PLY_NEXTSCAN
                * (0x1d) packets that follow.  The Windows user32 DrawEdge draws
                * 3D window borders (e.g. Internet Explorer's client edge) this
                * way, so without it those borders go unrendered. */
        if (!ati_cce_settings(s, &rd, op, &gmc)) {
            return false;
        }
        if (ati_cce_has(&rd, 1)) {
            ati_mm_write(s, DP_BRUSH_FRGD_CLR, ati_cce_next(&rd), 4);
        }
        return true;
    case 0x1d: /* PLY_NEXTSCAN: one polyscanline of the current CNTL_POLYSCANLINES
                * fill.  dword0 = top | (height << 16); each following dword is a
                * segment = x_start | (x_end << 16) (SDK F.20).  Fill each segment
                * as a rectangle with the colour/ROP set up by the 0x98 packet. */
    {
        uint32_t yh;
        int y, h;

        if (!ati_cce_has(&rd, 2)) {
            return false;
        }
        yh = ati_cce_next(&rd);
        y = yh & 0x3fff;
        h = (yh >> 16) & 0x3fff;
        while (ati_cce_has(&rd, 1)) {
            uint32_t seg = ati_cce_next(&rd);
            int x0 = seg & 0x3fff;
            int x1 = (seg >> 16) & 0x3fff;

            if (x1 <= x0 || h <= 0) {
                continue;
            }
            ati_mm_write(s, DST_Y_X, ((uint32_t)y << 16) | (uint32_t)x0, 4);
            ati_mm_write(s, DST_HEIGHT_WIDTH,
                         ((uint32_t)h << 16) | (uint32_t)(x1 - x0), 4);
        }
        return true;
    }
    case 0x11: /* PAINT: plain form, no SETTINGS */
    case 0x91: /* CNTL_PAINT: one rect given as two corners (like SET_SCISSORS),
                * top-left then bottom-right, each packed (y << 16) | x.  The
                * Windows DirectDraw colour-fill (DdBlt DDBLT_COLORFILL) uses
                * this packet; PAINT_MULTI below instead carries (x,y)+(w,h),
                * so the two must be decoded differently. */
        if (!ati_cce_settings(s, &rd, op, &gmc)) {
            return false;
        }
        if (ati_cce_has_settings(op) && !ati_cce_paint_brush(s, &rd, gmc)) {
            return false;
        }
        while (ati_cce_has(&rd, 2)) {
            uint32_t tl = ati_cce_next(&rd);
            uint32_t br = ati_cce_next(&rd);
            int w = (int)(br & 0x3fff) - (int)(tl & 0x3fff);
            int h = (int)((br >> 16) & 0x3fff) - (int)((tl >> 16) & 0x3fff);

            if (w <= 0 || h <= 0) {
                continue;
            }
            ati_mm_write(s, DST_Y_X, tl, 4);
            ati_mm_write(s, DST_HEIGHT_WIDTH,
                         ((uint32_t)h << 16) | (uint32_t)w, 4);
        }
        return true;
    case 0x1a: /* PAINT_MULTI: plain form, no SETTINGS */
    case 0x9a: /* CNTL_PAINT_MULTI: rects as (x<<16|y), (w<<16|h) */
        if (!ati_cce_settings(s, &rd, op, &gmc)) {
            return false;
        }
        if (ati_cce_has_settings(op) && !ati_cce_paint_brush(s, &rd, gmc)) {
            return false;
        }
        while (ati_cce_has(&rd, 2)) {
            ati_mm_write(s, DST_X_Y, ati_cce_next(&rd), 4);
            ati_mm_write(s, DST_WIDTH_HEIGHT, ati_cce_next(&rd), 4);
        }
        return true;
    case 0x15: /* POLYLINE: plain form, no SETTINGS */
    case 0x95: /* CNTL_POLYLINE: brush colour, then points as (y<<16|x) */
        if (!ati_cce_settings(s, &rd, op, &gmc)) {
            return false;
        }
        if (ati_cce_has_settings(op) && !ati_cce_paint_brush(s, &rd, gmc)) {
            return false;
        }
        if (!ati_cce_has(&rd, 2)) {
            return false;
        }
        ati_mm_write(s, DST_LINE_START, ati_cce_next(&rd), 4);
        while (ati_cce_has(&rd, 1)) {
            ati_mm_write(s, DST_LINE_END, ati_cce_next(&rd), 4);
        }
        return true;
    case 0x9c: /* CNTL_TRANS_BITBLT: colour-keyed copy (masked icons) */
        if (!ati_cce_gmc_prefix(s, &rd, &gmc)) {
            return false;
        }
        if (!ati_cce_has(&rd, 6)) {
            return false;
        }
        ati_mm_write(s, CLR_CMP_CNTL, ati_cce_next(&rd), 4);
        ati_mm_write(s, CLR_CMP_CLR_SRC, ati_cce_next(&rd), 4);
        ati_mm_write(s, CLR_CMP_CLR_DST, ati_cce_next(&rd), 4);
        ati_mm_write(s, SRC_X_Y, ati_cce_next(&rd), 4);
        ati_mm_write(s, DST_X_Y, ati_cce_next(&rd), 4);
        ati_mm_write(s, DST_WIDTH_HEIGHT, ati_cce_next(&rd), 4);
        /*
         * The compare stays programmed in the register file, so clear it
         * again: ordinary blits that follow must not be colour-keyed.
         */
        ati_mm_write(s, CLR_CMP_CNTL, 0, 4);
        return true;
    case 0x12: /* BITBLT: plain form, no SETTINGS */
    case 0x1b: /* BITBLT_MULTI: plain form, no SETTINGS */
    case 0x28: /* CNTL_BITBLT as used by ati2draa's glyph cache (carries
                * SETTINGS despite bit 7 being clear) */
    case 0x92: /* CNTL_BITBLT */
    case 0x9b: /* CNTL_BITBLT_MULTI */
        if (!ati_cce_settings(s, &rd, op, &gmc)) {
            return false;
        }
        while (ati_cce_has(&rd, 3)) {
            ati_mm_write(s, SRC_X_Y, ati_cce_next(&rd), 4);
            ati_mm_write(s, DST_X_Y, ati_cce_next(&rd), 4);
            ati_mm_write(s, DST_WIDTH_HEIGHT, ati_cce_next(&rd), 4);
        }
        return true;
    case 0x14: /* HOSTDATA_BLT: plain form, no SETTINGS */
    case 0x94: /* CNTL_HOSTDATA_BLT: colours, then optional rect + data */
        if (!ati_cce_settings(s, &rd, op, &gmc)) {
            return false;
        }
        if (!ati_cce_has(&rd, 2)) {
            return false;
        }
        ati_mm_write(s, DP_SRC_FRGD_CLR, ati_cce_next(&rd), 4);
        ati_mm_write(s, DP_SRC_BKGD_CLR, ati_cce_next(&rd), 4);
        /*
         * ati2draa emits this with no trailing rect purely to load the
         * text state that subsequent NEXT_CHAR packets use; the r128 DRM
         * appends (y,x), (h,w), a dword count and the host data itself.
         */
        if (ati_cce_has(&rd, 3)) {
            ati_mm_write(s, DST_Y_X, ati_cce_next(&rd), 4);
            ati_mm_write(s, DST_HEIGHT_WIDTH, ati_cce_next(&rd), 4);
            ati_cce_next(&rd);      /* dword count, implied by the rect */
            ati_cce_host_data(s, &rd);
        }
        return true;
    case 0x16: /* SCALING: plain form, no SETTINGS */
    case 0x17: /* TRANS_SCALING: plain form, no SETTINGS */
    case 0x96: /* SCALE: stretch a source bitmap into a destination rect */
    case 0x97: { /* TRANS_SCALE: as SCALE, with a source colour key */
        uint32_t db[11];
        unsigned i;

        if (!ati_cce_settings(s, &rd, op, &gmc)) {
            return false;
        }
        if (!ati_cce_has(&rd, 11)) {
            return false;
        }
        for (i = 0; i < 11; i++) {
            db[i] = ati_cce_next(&rd);
        }
        ati_scale_blt(s, gmc, db, op == 0x97 || op == 0x17);
        return true;
    }
    case 0x25: /* 3D_RNDR_GEN_PRIM: vertices inline in the ring */
        return ati_3d_gen_prim(s, &rd, false);
    case 0x23: /* 3D_RNDR_GEN_INDX_PRIM: vertices in a VRAM buffer */
        return ati_3d_gen_prim(s, &rd, true);
    case 0x2c: /* LOAD_PALETTE: a datatype word (1 = 16-entry/4bpp, else
                * 256-entry/8bpp) then that many RGBQUAD colours
                * (B[7:0] G[15:8] R[23:16] A[31:24]) loaded from CLUT entry 0
                * (SDK F.21).  Feed them to the VGA DAC the same way the MMIO
                * PALETTE_DATA path does; guests normally use that MMIO path,
                * so this is the documented ring alternative. */
    {
        uint32_t dt;
        unsigned n, i;

        if (!ati_cce_has(&rd, 1)) {
            return false;
        }
        dt = ati_cce_next(&rd);
        n = dt == 1 ? 16 : 256;
        vga_ioport_write(&s->vga, VGA_PEL_IW, 0);
        for (i = 0; i < n && ati_cce_has(&rd, 1); i++) {
            uint32_t c = ati_cce_next(&rd);

            vga_ioport_write(&s->vga, VGA_PEL_D, (c >> 16) & 0xff); /* R */
            vga_ioport_write(&s->vga, VGA_PEL_D, (c >> 8) & 0xff);  /* G */
            vga_ioport_write(&s->vga, VGA_PEL_D, c & 0xff);         /* B */
        }
        return true;
    }
    case 0x2d: /* PURGE: purge the pixel cache (SDK opcode summary).  This
                * model executes each packet synchronously and has no pixel
                * cache, so there is nothing to flush -- treat it as a no-op
                * rather than an unhandled packet. */
        return true;
    default:
        return false;
    }
}

/*
 * Process a stream of PM4 packets from [rptr, wptr) within a dword-addressed
 * window (base is a card VM byte address, mask the power-of-two index mask).
 * Shared by the ring-buffer engine and the indirect-buffer launcher: the ring
 * is a circular window sized by PM4_BUFFER_CNTL, an indirect buffer a linear
 * span [0, INDSIZE).  `guard` bounds the packet count at the window size so a
 * malformed length field (a type-3 count that skips the tail pointer) cannot
 * spin forever -- the circular `rptr != wptr` test alone does not bound this.
 */
static void ati_cce_run(ATIVGAState *s, uint32_t base, uint32_t mask,
                        uint32_t rptr, uint32_t wptr)
{
    uint32_t guard = mask + 2;

    while (rptr != wptr && guard--) {
        uint32_t hdr = ati_cce_ring_dword(s, base, mask, rptr++);
        uint32_t count = ((hdr >> 16) & 0x3fff) + 1;
        unsigned i;

        switch (hdr >> 30) {
        case 0: /* type 0: write count registers starting at bits 12:0 */
        {
            uint32_t reg = (hdr & 0x1fff) << 2;
            bool one_reg = hdr & 0x8000; /* all data to the same register */

            for (i = 0; i < count && rptr != wptr; i++) {
                uint32_t data = ati_cce_ring_dword(s, base, mask, rptr++);

                ati_mm_write(s, reg + (one_reg ? 0 : i * 4), data, 4);
            }
            break;
        }
        case 1: /* type 1: two scattered register writes */
        {
            uint32_t reg0 = (hdr & 0x7ff) << 2;
            uint32_t reg1 = ((hdr >> 11) & 0x7ff) << 2;

            if (rptr != wptr) {
                ati_mm_write(s, reg0, ati_cce_ring_dword(s, base, mask,
                                                         rptr++), 4);
            }
            if (rptr != wptr) {
                ati_mm_write(s, reg1, ati_cce_ring_dword(s, base, mask,
                                                         rptr++), 4);
            }
            break;
        }
        case 2: /* type 2: filler */
            break;
        case 3: /* type 3: engine command */
        {
            uint32_t op = (hdr >> 8) & 0xff;

            if (!ati_cce_execute_type3(s, base, mask, rptr, count, op)) {
                static uint32_t warned[8];

                if (!(warned[op >> 5] & (1u << (op & 31)))) {
                    warned[op >> 5] |= 1u << (op & 31);
                    qemu_log_mask(LOG_UNIMP,
                                  "ati: unhandled CCE type-3 packet 0x%02x\n",
                                  op);
                }
            }
            rptr = (rptr + count) & mask;
            break;
        }
        }
        rptr &= mask;
    }
}

/*
 * Launch an indirect buffer (PM4_IW_INDOFF / PM4_IW_INDSIZE).  The r128 DRM
 * (unlike the XP inbox driver, which inlines type-3 packets in the ring)
 * batches all 2D work into DMA buffers and only writes the buffer's card VM
 * byte offset and dword length into the ring; writing INDSIZE makes the CCE
 * fetch and run that buffer as an ordinary packet stream.  Without this every
 * drawing command the DRM issues -- the whole X server render path -- is
 * silently discarded.  Guard against a buffer that itself writes INDSIZE so a
 * recursive launch cannot run away.
 */
static void ati_cce_execute_indirect(ATIVGAState *s)
{
    uint32_t size = s->cce_indsize;
    uint32_t mask;

    if (s->cce_in_indirect || !size) {
        return;
    }
    for (mask = 1; mask < size; mask <<= 1) {
        /* smallest power-of-two window that spans the buffer */
    }
    mask -= 1;
    s->cce_in_indirect = true;
    ati_cce_run(s, s->cce_indoff, mask, 0, size);
    s->cce_in_indirect = false;
}

static void ati_cce_execute(ATIVGAState *s, uint32_t rptr, uint32_t wptr)
{
    int off_slot = ati_init_aux_slot(0x0700);
    int cntl_slot = ati_init_aux_slot(0x0704);
    uint32_t base, mask;
    unsigned l2qw;

    if (off_slot < 0 || cntl_slot < 0) {
        return;
    }
    base = s->regs.init_aux[off_slot] & ~0x03u;
    l2qw = s->regs.init_aux[cntl_slot] & 0x3f;
    /*
     * Mode 0 in PM4_BUFFER_CNTL bits 31:28 is PM4_NONPM4 - no command
     * engine.  A base of 0 is valid: the XP driver runs its ring at card
     * VM address 0 (translated through the PCI GART).
     */
    if (!((s->regs.init_aux[cntl_slot] >> 28) & 0xf) || l2qw > 17) {
        return;
    }
    mask = (2u << l2qw) - 1;    /* ring size in dwords, power of two */
    rptr &= mask;
    wptr &= mask;

    ati_cce_run(s, base, mask, rptr, wptr);
}

static void ati_pll_write(ATIVGAState *s, uint32_t data)
{
    unsigned idx = s->regs.clock_cntl_index & 0x3f;

    if ((s->regs.clock_cntl_index & 0x80) &&
        idx < ARRAY_SIZE(s->regs.pll_regs)) {
        s->regs.pll_regs[idx] = data;
    }
}

static uint32_t ati_mm_aper_offset(const ATIVGAState *s, hwaddr addr)
{
    uint32_t offset = (s->regs.mm_index & ~BIT(31)) + addr - MM_DATA;

    /* MM_APER (bit 31) selects Linear Aperture 0; physical VRAM aliases. */
    return offset & (s->vga.vram_size - 1);
}

static uint64_t ati_mm_read(void *opaque, hwaddr addr, unsigned int size)
{
    ATIVGAState *s = opaque;
    uint32_t val = 0;

    /* Register Aperture 1: the upper half of BAR2 mirrors the register file
     * (RRG 2.2.1; CONFIG_REG_APER_SIZE reports 8 KB, the BAR covers 16). */
    if (addr >= 0x2000 && addr < 0x4000) {
        addr -= 0x2000;
    }

    switch (addr) {
    case MM_INDEX:
        val = s->regs.mm_index;
        break;
    case CLOCK_CNTL_INDEX ... CLOCK_CNTL_INDEX + 3:
        val = ati_reg_read_offs(s->regs.clock_cntl_index,
                                addr - CLOCK_CNTL_INDEX, size);
        break;
    case CLOCK_CNTL_DATA ... CLOCK_CNTL_DATA + 3:
        if ((s->regs.clock_cntl_index & 0x3f) == ATI_PLL_TEST_CNTL) {
            ati_pll_tick_test_count(s);
        }
        val = ati_reg_read_offs(ati_pll_read(s), addr - CLOCK_CNTL_DATA,
                                size);
        break;
    case 0x710 ... 0x713: /* PM4_BUFFER_DL_RPTR */
        val = ati_reg_read_offs(s->regs.pm4_dl_rptr, addr - 0x710, size);
        break;
    case 0x714 ... 0x717: /* PM4_BUFFER_DL_WPTR */
        val = ati_reg_read_offs(s->regs.pm4_dl_wptr, addr - 0x714, size);
        break;
    case 0x05c ... 0x05f: /* CRTC_STATUS */
        val = ati_reg_read_offs(ati_crtc_status(s), addr - 0x05c, size);
        break;
    case 0x210 ... 0x213: /* CRTC_VLINE_CRNT_VLINE */
        val = ati_reg_read_offs(ati_crtc_current_line() << 16,
                                addr - 0x210, size);
        break;
    case 0x214 ... 0x217: /* CRTC_CRNT_FRAME */
        val = ati_reg_read_offs(ati_crtc_frame_count() & 0x1fffff,
                                addr - 0x214, size);
        break;
    case MM_DATA ... MM_DATA + 3:
        /* indexed access to regs or memory */
        if (s->regs.mm_index & BIT(31)) {
            uint32_t idx = ati_mm_aper_offset(s, addr);

            val = ldn_le_p(s->vga.vram_ptr + idx, size);
        } else if (s->regs.mm_index > MM_DATA + 3) {
            val = ati_mm_read(s, s->regs.mm_index + addr - MM_DATA, size);
        } else {
            qemu_log_mask(LOG_GUEST_ERROR,
                "ati_mm_read: mm_index too small: %u\n", s->regs.mm_index);
        }
        break;
    case BIOS_0_SCRATCH ... BUS_CNTL - 1:
    {
        int i = (addr - BIOS_0_SCRATCH) / 4;
        if (s->dev_id == PCI_DEVICE_ID_ATI_RAGE128_PF && i > 3) {
            break;
        }
        val = ati_reg_read_offs(s->regs.bios_scratch[i],
                                addr - (BIOS_0_SCRATCH + i * 4), size);
        break;
    }
    case GEN_INT_CNTL:
        val = s->regs.gen_int_cntl;
        break;
    case GEN_INT_STATUS:
        val = s->regs.gen_int_status;
        if (s->dev_id == PCI_DEVICE_ID_ATI_RAGE128_PF) {
            /*
             * GUI_IDLE_INT_STAT (bit 19) is a level-ish engine-idle status:
             * it powers up set - the only GEN_INT_STATUS bit that does - and
             * reasserts whenever the engine is idle (RAGE 128 PRO Register
             * Reference Guide).  Every operation in this model completes
             * synchronously, so the engine is always idle; a driver that
             * acknowledges the bit and waits for it to come back (the XP
             * display driver's engine-liveness test) must see it set again.
             */
            val |= BIT(19);
        }
        break;
    case CRTC_GEN_CNTL ... CRTC_GEN_CNTL + 3:
        val = ati_reg_read_offs(s->regs.crtc_gen_cntl,
                                addr - CRTC_GEN_CNTL, size);
        break;
    case CRTC_EXT_CNTL ... CRTC_EXT_CNTL + 3:
        val = ati_reg_read_offs(s->regs.crtc_ext_cntl,
                                addr - CRTC_EXT_CNTL, size);
        break;
    case DAC_CNTL:
        val = s->regs.dac_cntl;
        /*
         * DAC load-sense: with the comparator enabled, DAC_CMP_OUTPUT (bit 7)
         * reads 1 when the RGB DAC outputs are terminated, i.e. a CRT is
         * connected (RRG DAC_CNTL, and radeon/r128 CRT detection treat
         * DAC_CMP_OUTPUT set as "connected").  This model always presents a
         * connected CRT so the driver's monitor-detection succeeds.
         */
        if (val & DAC_CMP_EN) {
            val |= DAC_CMP_OUTPUT;
        }
        break;
    case GPIO_VGA_DDC ... GPIO_VGA_DDC + 3:
        val = ati_reg_read_offs(s->regs.gpio_vga_ddc,
                                addr - GPIO_VGA_DDC, size);
        break;
    case GPIO_DVI_DDC ... GPIO_DVI_DDC + 3:
        val = ati_reg_read_offs(s->regs.gpio_dvi_ddc,
                                addr - GPIO_DVI_DDC, size);
        break;
    case GPIO_MONID ... GPIO_MONID + 3:
        val = ati_reg_read_offs(s->regs.gpio_monid,
                                addr - GPIO_MONID, size);
        break;
    case PALETTE_INDEX:
        /* FIXME unaligned access */
        val = vga_ioport_read(&s->vga, VGA_PEL_IR) << 16;
        val |= vga_ioport_read(&s->vga, VGA_PEL_IW) & 0xff;
        break;
    case PALETTE_DATA:
        val = vga_ioport_read(&s->vga, VGA_PEL_D);
        break;
    case PALETTE_30_DATA:
        val = s->regs.palette[vga_ioport_read(&s->vga, VGA_PEL_IR)];
        break;
    case CNFG_CNTL:
        val = s->regs.config_cntl;
        break;
    case CNFG_MEMSIZE:
        val = s->vga.vram_size;
        break;
    case CONFIG_APER_0_BASE:
    case CONFIG_APER_1_BASE:
        val = pci_default_read_config(&s->dev,
                                      PCI_BASE_ADDRESS_0, size) & 0xfffffff0;
        break;
    case CONFIG_APER_SIZE:
        val = memory_region_size(&s->linear_aper) / 2;
        break;
    case CONFIG_REG_1_BASE:
        val = pci_default_read_config(&s->dev,
                                      PCI_BASE_ADDRESS_2, size) & 0xfffffff0;
        break;
    case CONFIG_REG_APER_SIZE:
        val = memory_region_size(&s->mm) / 2;
        break;
    case HOST_PATH_CNTL:
        val = BIT(23); /* Radeon HDP_APER_CNTL */
        break;
    case MC_STATUS:
        val = 5;
        break;
    case MEM_SDRAM_MODE_REG:
        if (s->dev_id != PCI_DEVICE_ID_ATI_RAGE128_PF) {
            val = BIT(28) | BIT(20);
        }
        break;
    case RBBM_STATUS:
    case GUI_STAT:
        val = 64; /* free CMDFIFO entries */
        break;
    case PM4_STAT:
        /*
         * Every GUI operation in this model completes synchronously, so
         * the CCE never has queued work: report the PM4 command FIFO as
         * fully free (PM4_FIFOCNT, bits 11:0) with PM4_BUSY (bit 16) and
         * PM4_GUI_ACTIVE (bit 31) clear.  Linux's r128 DRM requires
         * FIFOCNT >= its configured FIFO size (up to 192 depending on the
         * CCE mode) with both busy bits clear before r128_do_cce_idle()
         * succeeds; a constant 0 here made every such ioctl spin for its
         * full 10 ms usec_timeout and XFree86 4.x with the DRI module
         * loaded crawled at minutes per screen repaint.
         */
        val = 0xfff;
        break;
    case CRTC_H_TOTAL_DISP:
        val = s->regs.crtc_h_total_disp;
        break;
    case CRTC_H_SYNC_STRT_WID:
        val = s->regs.crtc_h_sync_strt_wid;
        break;
    case CRTC_V_TOTAL_DISP:
        val = s->regs.crtc_v_total_disp;
        break;
    case CRTC_V_SYNC_STRT_WID:
        val = s->regs.crtc_v_sync_strt_wid;
        break;
    case CRTC_OFFSET:
        val = s->regs.crtc_offset;
        break;
    case CRTC_OFFSET_CNTL:
        val = s->regs.crtc_offset_cntl;
        break;
    case CRTC_PITCH:
        val = s->regs.crtc_pitch;
        break;
    case 0xf00 ... 0xfff:
        val = pci_default_read_config(&s->dev, addr - 0xf00, size);
        break;
    case CUR_OFFSET ... CUR_OFFSET + 3:
        val = ati_reg_read_offs(s->regs.cur_offset, addr - CUR_OFFSET, size);
        break;
    case CUR_HORZ_VERT_POSN ... CUR_HORZ_VERT_POSN + 3:
        val = ati_reg_read_offs(s->regs.cur_hv_pos,
                                addr - CUR_HORZ_VERT_POSN, size);
        if (addr + size > CUR_HORZ_VERT_POSN + 3) {
            val |= (s->regs.cur_offset & BIT(31)) >> (4 - size);
        }
        break;
    case CUR_HORZ_VERT_OFF ... CUR_HORZ_VERT_OFF + 3:
        val = ati_reg_read_offs(s->regs.cur_hv_offs,
                                addr - CUR_HORZ_VERT_OFF, size);
        if (addr + size > CUR_HORZ_VERT_OFF + 3) {
            val |= (s->regs.cur_offset & BIT(31)) >> (4 - size);
        }
        break;
    case CUR_CLR0 ... CUR_CLR0 + 3:
        val = ati_reg_read_offs(s->regs.cur_color0, addr - CUR_CLR0, size);
        break;
    case CUR_CLR1 ... CUR_CLR1 + 3:
        val = ati_reg_read_offs(s->regs.cur_color1, addr - CUR_CLR1, size);
        break;
    case DST_OFFSET:
        val = s->regs.dst_offset;
        break;
    case DST_PITCH:
        val = s->regs.dst_pitch;
        if (s->dev_id == PCI_DEVICE_ID_ATI_RAGE128_PF) {
            val |= s->regs.dst_tile << 16;
        }
        break;
    case DST_WIDTH:
        val = s->regs.dst_width;
        break;
    case DST_HEIGHT:
        val = s->regs.dst_height;
        break;
    case SRC_X:
        val = s->regs.src_x;
        break;
    case SRC_Y:
        val = s->regs.src_y;
        break;
    case DST_X:
        val = s->regs.dst_x;
        break;
    case DST_Y:
        val = s->regs.dst_y;
        break;
    case DP_GUI_MASTER_CNTL:
        /* DP_GUI_MASTER_CNTL aliases fields from DP_MIX and DP_DATATYPE */
        val = s->regs.dp_gui_master_cntl |
              ((s->regs.dp_datatype & DP_BRUSH_DATATYPE) >> 4) |
              ((s->regs.dp_datatype & DP_DST_DATATYPE) << 8) |
              ((s->regs.dp_datatype & DP_SRC_DATATYPE) >> 4) |
              (s->regs.dp_mix & DP_ROP3) |
              ((s->regs.dp_mix & DP_SRC_SOURCE) << 16);
        break;
    case SRC_OFFSET:
        val = s->regs.src_offset;
        break;
    case SRC_PITCH:
        val = s->regs.src_pitch;
        if (s->dev_id == PCI_DEVICE_ID_ATI_RAGE128_PF) {
            val |= s->regs.src_tile << 16;
        }
        break;
    case DP_BRUSH_BKGD_CLR:
        val = s->regs.dp_brush_bkgd_clr;
        break;
    case DP_BRUSH_FRGD_CLR:
        val = s->regs.dp_brush_frgd_clr;
        break;
    case DP_SRC_FRGD_CLR:
        val = s->regs.dp_src_frgd_clr;
        break;
    case DP_SRC_BKGD_CLR:
        val = s->regs.dp_src_bkgd_clr;
        break;
    case DP_CNTL:
        val = s->regs.dp_cntl;
        break;
    case DP_DATATYPE:
        val = s->regs.dp_datatype;
        break;
    case DP_MIX:
        val = s->regs.dp_mix;
        break;
    case DP_WRITE_MASK:
        val = s->regs.dp_write_mask;
        break;
    case DEFAULT_OFFSET:
        val = s->regs.default_offset;
        if (s->dev_id != PCI_DEVICE_ID_ATI_RAGE128_PF) {
            val >>= 10;
            val |= s->regs.default_pitch << 16;
            val |= s->regs.default_tile << 30;
        }
        break;
    case DEFAULT_PITCH:
        val = s->regs.default_pitch;
        val |= s->regs.default_tile << 16;
        break;
    case DEFAULT_SC_BOTTOM_RIGHT:
        val = s->regs.default_sc_right;
        val |= s->regs.default_sc_bottom << 16;
        break;
    case SC_TOP:
        val = s->regs.sc_top;
        break;
    case SC_LEFT:
        val = s->regs.sc_left;
        break;
    case SC_BOTTOM:
        val = s->regs.sc_bottom;
        break;
    case SC_RIGHT:
        val = s->regs.sc_right;
        break;
    case SRC_SC_BOTTOM:
        val = s->regs.src_sc_bottom;
        break;
    case SRC_SC_RIGHT:
        val = s->regs.src_sc_right;
        break;
    case SC_TOP_LEFT:
    case SC_BOTTOM_RIGHT:
    case SRC_SC_BOTTOM_RIGHT:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "Read from write-only register 0x%x\n", (unsigned)addr);
        break;
    default:
    {
        int aux = ati_init_aux_slot(addr);

        if (aux >= 0) {
            uint32_t v = s->regs.init_aux[aux];

            if ((addr & ~3ULL) == 0x0140) {
                /*
                 * MEM_CNTL bits 22:20 are read-only status
                 * (MEM_CTLR_STATUS / MEM_SEQNCR_STATUS / MEM_ARBITER_STATUS),
                 * 0 = idle.  The memory controller in this model is never
                 * busy, and a driver that wrote those bits and then polled
                 * for them to clear would otherwise wait forever.
                 */
                v &= ~(7u << 20);
            }
            val = ati_reg_read_offs(v, addr & 3, size);
        }
        break;
    }
    }
    if (addr < CUR_OFFSET || addr > CUR_CLR1 || ATI_DEBUG_HW_CURSOR) {
        trace_ati_mm_read(size, addr, ati_reg_name(addr & ~3ULL), val);
    }
    return val;
}

static inline void ati_reg_write_offs(uint32_t *reg, int offs,
                                      uint64_t data, unsigned int size)
{
    if (offs == 0 && size == 4) {
        *reg = data;
    } else {
        *reg = deposit32(*reg, offs * BITS_PER_BYTE, size * BITS_PER_BYTE,
                         data);
    }
}

static void ati_mm_write(void *opaque, hwaddr addr,
                           uint64_t data, unsigned int size)
{
    ATIVGAState *s = opaque;

    if (addr >= 0x2000 && addr < 0x4000) {
        addr -= 0x2000; /* Register Aperture 1 mirror */
    }

    if (addr < CUR_OFFSET || addr > CUR_CLR1 || ATI_DEBUG_HW_CURSOR) {
        trace_ati_mm_write(size, addr, ati_reg_name(addr & ~3ULL), data);
    }
    switch (addr) {
    case CLOCK_CNTL_INDEX ... CLOCK_CNTL_INDEX + 3:
        ati_reg_write_offs(&s->regs.clock_cntl_index,
                           addr - CLOCK_CNTL_INDEX, data, size);
        break;
    case CLOCK_CNTL_DATA ... CLOCK_CNTL_DATA + 3:
    {
        uint32_t cur = ati_pll_read(s);

        ati_reg_write_offs(&cur, addr - CLOCK_CNTL_DATA, data, size);
        ati_pll_write(s, cur);
        break;
    }
    case 0x710 ... 0x713: /* PM4_BUFFER_DL_RPTR */
        ati_reg_write_offs(&s->regs.pm4_dl_rptr, addr - 0x710, data, size);
        break;
    case PM4_IW_INDOFF ... PM4_IW_INDOFF + 3:
        /* Indirect-buffer card VM byte offset; INDSIZE below launches it. */
        ati_reg_write_offs(&s->cce_indoff, addr - PM4_IW_INDOFF, data, size);
        break;
    case PM4_IW_INDSIZE ... PM4_IW_INDSIZE + 3:
        ati_reg_write_offs(&s->cce_indsize, addr - PM4_IW_INDSIZE, data, size);
        ati_cce_execute_indirect(s);
        break;
    case 0x05c: /* CRTC_STATUS: bit 1 is write-1-to-clear */
        if (data & 2) {
            s->regs.crtc_vblank_ack_frame = ati_crtc_frame_count();
        }
        break;
    case 0x714 ... 0x717: /* PM4_BUFFER_DL_WPTR */
        /*
         * CCE ring-buffer submission.  This model executes nothing from the
         * ring yet; consume it instantly so a driver polling the read
         * pointer (XP's ati2draa spins on PM4_BUFFER_DL_RPTR until it
         * catches up with its write pointer, and the kernel watchdog turns
         * a stall into bugcheck 0xEA) sees the engine keep up.  Mirror the
         * new read pointer through the ring read-pointer writeback address
         * if the driver configured one (PM4_BUFFER_DL_RPTR_ADDR).
         */
        ati_reg_write_offs(&s->regs.pm4_dl_wptr, addr - 0x714, data, size);
        ati_cce_execute(s, s->regs.pm4_dl_rptr,
                        s->regs.pm4_dl_wptr & ~R128_PM4_BUFFER_DL_DONE);
        s->regs.pm4_dl_rptr = s->regs.pm4_dl_wptr & ~R128_PM4_BUFFER_DL_DONE;
        {
            int slot = ati_init_aux_slot(0x070c);
            uint32_t wb = slot >= 0 ? s->regs.init_aux[slot] : 0;

            if (wb & ~3u) {
                uint32_t le = cpu_to_le32(s->regs.pm4_dl_rptr);

                pci_dma_write(&s->dev, wb & ~3u, &le, sizeof(le));
            }
        }
        break;
    case MM_INDEX:
        s->regs.mm_index = data & ~3;
        break;
    case MM_DATA ... MM_DATA + 3:
        /* indexed access to regs or memory */
        if (s->regs.mm_index & BIT(31)) {
            uint32_t idx = ati_mm_aper_offset(s, addr);

            stn_le_p(s->vga.vram_ptr + idx, size, data);
        } else if (s->regs.mm_index > MM_DATA + 3) {
            ati_mm_write(s, s->regs.mm_index + addr - MM_DATA, data, size);
        } else {
            qemu_log_mask(LOG_GUEST_ERROR,
                "ati_mm_write: mm_index too small: %u\n", s->regs.mm_index);
        }
        break;
    case BIOS_0_SCRATCH ... BUS_CNTL - 1:
    {
        int i = (addr - BIOS_0_SCRATCH) / 4;
        if (s->dev_id == PCI_DEVICE_ID_ATI_RAGE128_PF && i > 3) {
            break;
        }
        ati_reg_write_offs(&s->regs.bios_scratch[i],
                           addr - (BIOS_0_SCRATCH + i * 4), data, size);
        break;
    }
    case GEN_INT_CNTL:
        s->regs.gen_int_cntl = data;
        if (data & CRTC_VBLANK_INT) {
            ati_vga_vblank_irq(s);
        } else {
            timer_del(&s->vblank_timer);
            ati_vga_update_irq(s);
        }
        break;
    case GEN_INT_STATUS:
        data &= (s->dev_id == PCI_DEVICE_ID_ATI_RAGE128_PF ?
                 0x000f040fUL : 0xfc080effUL);
        s->regs.gen_int_status &= ~data;
        ati_vga_update_irq(s);
        break;
    case CRTC_GEN_CNTL ... CRTC_GEN_CNTL + 3:
    {
        uint32_t val = s->regs.crtc_gen_cntl;
        ati_reg_write_offs(&s->regs.crtc_gen_cntl,
                           addr - CRTC_GEN_CNTL, data, size);
        if ((val & CRTC2_CUR_EN) != (s->regs.crtc_gen_cntl & CRTC2_CUR_EN)) {
            ati_vga_switch_mode(s);
            if (s->cursor_guest_mode) {
                s->vga.force_shadow = !!(s->regs.crtc_gen_cntl & CRTC2_CUR_EN);
            } else {
                if (s->regs.crtc_gen_cntl & CRTC2_CUR_EN) {
                    ati_cursor_define(s);
                }
                dpy_mouse_set(s->vga.con, s->regs.cur_hv_pos >> 16,
                              s->regs.cur_hv_pos & 0xffff,
                              (s->regs.crtc_gen_cntl & CRTC2_CUR_EN) != 0);
            }
        }
        if ((val & (CRTC2_EXT_DISP_EN | CRTC2_EN)) !=
            (s->regs.crtc_gen_cntl & (CRTC2_EXT_DISP_EN | CRTC2_EN))) {
            ati_vga_switch_mode(s);
        }
        break;
    }
    case CRTC_EXT_CNTL ... CRTC_EXT_CNTL + 3:
    {
        uint32_t val = s->regs.crtc_ext_cntl;
        ati_reg_write_offs(&s->regs.crtc_ext_cntl,
                           addr - CRTC_EXT_CNTL, data, size);
        if (s->regs.crtc_ext_cntl & CRT_CRTC_DISPLAY_DIS) {
            DPRINTF("Display disabled\n");
            s->vga.ar_index &= ~BIT(5);
        } else {
            DPRINTF("Display enabled\n");
            s->vga.ar_index |= BIT(5);
            ati_vga_switch_mode(s);
        }
        if ((val & CRT_CRTC_DISPLAY_DIS) !=
            (s->regs.crtc_ext_cntl & CRT_CRTC_DISPLAY_DIS)) {
            ati_vga_switch_mode(s);
        }
        break;
    }
    case DAC_CNTL:
        s->regs.dac_cntl = data & 0xffffe3ff;
        s->vga.dac_8bit = !!(data & DAC_8BIT_EN);
        break;
    /*
     * GPIO regs for DDC access. Because some drivers access these via
     * multiple byte writes we have to be careful when we send bits to
     * avoid spurious changes in bitbang_i2c state. Only do it when either
     * the enable bits are changed or output bits changed while enabled.
     */
    case GPIO_VGA_DDC ... GPIO_VGA_DDC + 3:
        if (s->dev_id != PCI_DEVICE_ID_ATI_RAGE128_PF) {
            /* FIXME: Maybe add a property to select VGA or DVI port? */
        }
        break;
    case GPIO_DVI_DDC ... GPIO_DVI_DDC + 3:
        if (s->dev_id != PCI_DEVICE_ID_ATI_RAGE128_PF) {
            ati_reg_write_offs(&s->regs.gpio_dvi_ddc,
                               addr - GPIO_DVI_DDC, data, size);
            if ((addr <= GPIO_DVI_DDC + 2 && addr + size > GPIO_DVI_DDC + 2) ||
                (addr == GPIO_DVI_DDC && (s->regs.gpio_dvi_ddc & 0x30000))) {
                s->regs.gpio_dvi_ddc = ati_i2c(&s->bbi2c,
                                               s->regs.gpio_dvi_ddc, 0);
            }
        }
        break;
    case GPIO_MONID ... GPIO_MONID + 3:
        /* FIXME What does Radeon have here? */
        if (s->dev_id == PCI_DEVICE_ID_ATI_RAGE128_PF) {
            /* Rage128p accesses DDC via MONID(1-2) with additional mask bit */
            ati_reg_write_offs(&s->regs.gpio_monid,
                               addr - GPIO_MONID, data, size);
            if ((s->regs.gpio_monid & BIT(25)) &&
                ((addr <= GPIO_MONID + 2 && addr + size > GPIO_MONID + 2) ||
                 (addr == GPIO_MONID && (s->regs.gpio_monid & 0x60000)))) {
                s->regs.gpio_monid = ati_i2c(&s->bbi2c, s->regs.gpio_monid, 1);
            }
        }
        break;
    case PALETTE_INDEX ... PALETTE_INDEX + 3:
        if (size == 4) {
            vga_ioport_write(&s->vga, VGA_PEL_IR, (data >> 16) & 0xff);
            vga_ioport_write(&s->vga, VGA_PEL_IW, data & 0xff);
        } else {
            if (addr == PALETTE_INDEX) {
                vga_ioport_write(&s->vga, VGA_PEL_IW, data & 0xff);
            } else {
                vga_ioport_write(&s->vga, VGA_PEL_IR, data & 0xff);
            }
        }
        break;
    case PALETTE_DATA ... PALETTE_DATA + 3:
        data <<= addr - PALETTE_DATA;
        data = bswap32(data) >> 8;
        vga_ioport_write(&s->vga, VGA_PEL_D, data & 0xff);
        data >>= 8;
        vga_ioport_write(&s->vga, VGA_PEL_D, data & 0xff);
        data >>= 8;
        vga_ioport_write(&s->vga, VGA_PEL_D, data & 0xff);
        break;
    case PALETTE_30_DATA:
        s->regs.palette[vga_ioport_read(&s->vga, VGA_PEL_IW)] = data;
        vga_ioport_write(&s->vga, VGA_PEL_D, (data >> 22) & 0xff);
        vga_ioport_write(&s->vga, VGA_PEL_D, (data >> 12) & 0xff);
        vga_ioport_write(&s->vga, VGA_PEL_D, (data >> 2) & 0xff);
        break;
    case CNFG_CNTL:
        s->regs.config_cntl = data;
        break;
    case CRTC_H_TOTAL_DISP:
        s->regs.crtc_h_total_disp = data & 0x07ff07ff;
        break;
    case CRTC_H_SYNC_STRT_WID:
        s->regs.crtc_h_sync_strt_wid = data & 0x17bf1fff;
        break;
    case CRTC_V_TOTAL_DISP:
        s->regs.crtc_v_total_disp = data & 0x0fff0fff;
        break;
    case CRTC_V_SYNC_STRT_WID:
        s->regs.crtc_v_sync_strt_wid = data & 0x9f0fff;
        break;
    case CRTC_OFFSET:
        s->regs.crtc_offset = data & 0x87fffff8;
        ati_vga_set_offset(&s->vga, s->regs.crtc_offset & 0x07ffffff);
        break;
    case CRTC_OFFSET_CNTL:
        s->regs.crtc_offset_cntl = data; /* FIXME */
        break;
    case CRTC_PITCH:
        data &= 0x07ff07ff;
        if (s->regs.crtc_pitch != data) {
            s->regs.crtc_pitch = data;
            ati_vga_switch_mode(s);
        }
        break;
    case 0xf00 ... 0xfff:
        /* read-only copy of PCI config space so ignore writes */
        break;
    case CUR_OFFSET ... CUR_OFFSET + 3:
    {
        uint32_t t = s->regs.cur_offset;

        ati_reg_write_offs(&t, addr - CUR_OFFSET, data, size);
        t &= 0x87fffff0;
        if (s->regs.cur_offset != t) {
            s->regs.cur_offset = t;
            ati_cursor_define(s);
        }
        break;
    }
    case CUR_HORZ_VERT_POSN ... CUR_HORZ_VERT_POSN + 3:
    {
        uint32_t t = s->regs.cur_hv_pos | (s->regs.cur_offset & BIT(31));

        ati_reg_write_offs(&t, addr - CUR_HORZ_VERT_POSN, data, size);
        s->regs.cur_hv_pos = t & 0x3fff0fff;
        if (t & BIT(31)) {
            s->regs.cur_offset |= t & BIT(31);
        } else if (s->regs.cur_offset & BIT(31)) {
            s->regs.cur_offset &= ~BIT(31);
            ati_cursor_define(s);
        }
        /*
         * Push the new position to the display even when CUR_LOCK (BIT 31) is
         * set. The lock only exists to make a CUR_HORZ_VERT_OFF / _POSN /
         * _OFFSET update atomic on real hardware; there is no tearing to guard
         * against in this model. XFree86's r128 driver sets CUR_LOCK on *every*
         * R128SetCursorPosition write (r128_cursor.c), so gating on !lock left
         * the overlay cursor frozen at its last enable-time position while the
         * pointer moved -- it looked like the hardware cursor was not rendering
         * at all. Only the shape reload (ati_cursor_define) needs to respect
         * the lock, which it still does above.
         */
        if (!s->cursor_guest_mode &&
            (s->regs.crtc_gen_cntl & CRTC2_CUR_EN)) {
            dpy_mouse_set(s->vga.con, s->regs.cur_hv_pos >> 16,
                          s->regs.cur_hv_pos & 0xffff, true);
        }
        break;
    }
    case CUR_HORZ_VERT_OFF:
    {
        uint32_t t = s->regs.cur_hv_offs | (s->regs.cur_offset & BIT(31));

        ati_reg_write_offs(&t, addr - CUR_HORZ_VERT_OFF, data, size);
        s->regs.cur_hv_offs = t & 0x3f003f;
        if (t & BIT(31)) {
            s->regs.cur_offset |= t & BIT(31);
        } else if (s->regs.cur_offset & BIT(31)) {
            s->regs.cur_offset &= ~BIT(31);
            ati_cursor_define(s);
        }
        break;
    }
    case CUR_CLR0 ... CUR_CLR0 + 3:
    {
        uint32_t t = s->regs.cur_color0;

        ati_reg_write_offs(&t, addr - CUR_CLR0, data, size);
        t &= 0xffffff;
        if (s->regs.cur_color0 != t) {
            s->regs.cur_color0 = t;
            ati_cursor_define(s);
        }
        break;
    }
    case CUR_CLR1 ... CUR_CLR1 + 3:
        /*
         * Update cursor unconditionally here because some clients set up
         * other registers before actually writing cursor data to memory at
         * offset so we would miss cursor change unless always updating here
         */
        ati_reg_write_offs(&s->regs.cur_color1, addr - CUR_CLR1, data, size);
        s->regs.cur_color1 &= 0xffffff;
        ati_cursor_define(s);
        break;
    case DST_OFFSET:
            s->regs.dst_offset = data & 0xfffffff0;
        break;
    case DST_PITCH:
            s->regs.dst_pitch = data & 0x3fff;
        if (s->dev_id == PCI_DEVICE_ID_ATI_RAGE128_PF) {
            s->regs.dst_tile = (data >> 16) & 1;
        }
        break;
    case DST_TILE:
        if (s->dev_id == PCI_DEVICE_ID_ATI_RADEON_QY) {
            s->regs.dst_tile = data & 3;
        }
        break;
    case DST_WIDTH:
        s->regs.dst_width = data & 0x3fff;
        ati_2d_blt(s);
        break;
    case DST_HEIGHT:
        s->regs.dst_height = data & 0x3fff;
        break;
    case SRC_X:
        s->regs.src_x = data & 0x3fff;
        break;
    case SRC_Y:
        s->regs.src_y = data & 0x3fff;
        break;
    case DST_X:
        s->regs.dst_x = data & 0x3fff;
        break;
    case DST_Y:
        s->regs.dst_y = data & 0x3fff;
        break;
    case SRC_PITCH_OFFSET:
        if (s->dev_id == PCI_DEVICE_ID_ATI_RAGE128_PF) {
            s->regs.src_offset = (data & 0x1fffff) << 5;
            s->regs.src_pitch = (data & 0x7fe00000) >> 21;
            s->regs.src_tile = data >> 31;
        } else {
            s->regs.src_offset = (data & 0x3fffff) << 10;
            s->regs.src_pitch = (data & 0x3fc00000) >> 16;
            s->regs.src_tile = (data >> 30) & 1;
        }
        break;
    case DST_PITCH_OFFSET:
        if (s->dev_id == PCI_DEVICE_ID_ATI_RAGE128_PF) {
            s->regs.dst_offset = (data & 0x1fffff) << 5;
            s->regs.dst_pitch = (data & 0x7fe00000) >> 21;
            s->regs.dst_tile = data >> 31;
        } else {
            s->regs.dst_offset = (data & 0x3fffff) << 10;
            s->regs.dst_pitch = (data & 0x3fc00000) >> 16;
            s->regs.dst_tile = data >> 30;
        }
        break;
    case SRC_Y_X:
        s->regs.src_x = data & 0x3fff;
        s->regs.src_y = (data >> 16) & 0x3fff;
        break;
    case DST_Y_X:
        s->regs.dst_x = data & 0x3fff;
        s->regs.dst_y = (data >> 16) & 0x3fff;
        break;
    case DST_HEIGHT_WIDTH:
        s->regs.dst_width = data & 0x3fff;
        s->regs.dst_height = (data >> 16) & 0x3fff;
        ati_2d_blt(s);
        break;
    case DP_GUI_MASTER_CNTL:
        s->regs.dp_gui_master_cntl = data & 0xf800000f;
        s->regs.dp_datatype = (data & 0x0f00) >> 8 | (data & 0x30f0) << 4 |
                              (data & 0x4000) << 16;
        s->regs.dp_mix = (data & GMC_ROP3_MASK) | (data & 0x7000000) >> 16;
        /*
         * GMC_CLR_CMP_CNTL_DIS (bit 28): "1 = clear CLR_CMP_FN_DST,
         * CLR_CMP_FN_SRC" (RRG, DP_GUI_MASTER_CNTL).  The XFree86 r128 driver
         * sets this bit in the base GUI control word of every operation, so a
         * colour-key left enabled by a transparent blit is cleared by the next
         * op's control write.  Without honouring it a stale key (e.g. from a
         * KDE window-decoration blit) leaks into the following text and fill
         * operations, dropping keyed pixels and corrupting them.
         */
        if (data & GMC_DST_CLR_CMP_FCN_CLEAR) {
            s->regs.clr_cmp_cntl &= ~0x00000707u; /* clear FN_SRC[2:0],FN_DST[10:8] */
        }

        if (!(data & GMC_SRC_PITCH_OFFSET_CNTL)) {
            s->regs.src_offset = s->regs.default_offset;
            s->regs.src_pitch = s->regs.default_pitch;
        }
        if (!(data & GMC_DST_PITCH_OFFSET_CNTL)) {
            s->regs.dst_offset = s->regs.default_offset;
            s->regs.dst_pitch = s->regs.default_pitch;
        }
        if (!(data & GMC_SRC_CLIPPING)) {
            s->regs.src_sc_right = s->regs.default_sc_right;
            s->regs.src_sc_bottom = s->regs.default_sc_bottom;
        }
        if (!(data & GMC_DST_CLIPPING)) {
            s->regs.sc_top = 0;
            s->regs.sc_left = 0;
            s->regs.sc_right = s->regs.default_sc_right;
            s->regs.sc_bottom = s->regs.default_sc_bottom;
        }
        break;
    case DST_WIDTH_X:
        s->regs.dst_x = data & 0x3fff;
        s->regs.dst_width = (data >> 16) & 0x3fff;
        ati_2d_blt(s);
        break;
    case SRC_X_Y:
        s->regs.src_y = data & 0x3fff;
        s->regs.src_x = (data >> 16) & 0x3fff;
        break;
    case DST_X_Y:
        s->regs.dst_y = data & 0x3fff;
        s->regs.dst_x = (data >> 16) & 0x3fff;
        break;
    case DST_WIDTH_HEIGHT:
        s->regs.dst_height = data & 0x3fff;
        s->regs.dst_width = (data >> 16) & 0x3fff;
        ati_2d_blt(s);
        break;
    case DST_HEIGHT_Y:
        s->regs.dst_y = data & 0x3fff;
        s->regs.dst_height = (data >> 16) & 0x3fff;
        break;
    case SRC_OFFSET:
            s->regs.src_offset = data & 0xfffffff0;
        break;
    case SRC_PITCH:
            s->regs.src_pitch = data & 0x3fff;
        if (s->dev_id == PCI_DEVICE_ID_ATI_RAGE128_PF) {
            s->regs.src_tile = (data >> 16) & 1;
        }
        break;
    case DP_BRUSH_BKGD_CLR:
        s->regs.dp_brush_bkgd_clr = data;
        break;
    case DP_BRUSH_FRGD_CLR:
        s->regs.dp_brush_frgd_clr = data;
        break;
    case BRUSH_Y_X:
        s->regs.brush_y_x = data & 0x1f001f;
        break;
    case BRUSH_DATA0:
        s->regs.brush_data0 = data;
        break;
    case BRUSH_DATA1:
        s->regs.brush_data1 = data;
        break;
    case CLR_CMP_CNTL:
        s->regs.clr_cmp_cntl = data;
        break;
    case CLR_CMP_CLR_SRC:
        s->regs.clr_cmp_clr_src = data;
        break;
    case CLR_CMP_CLR_DST:
        s->regs.clr_cmp_clr_dst = data;
        break;
    case CLR_CMP_MASK:
        s->regs.clr_cmp_msk = data;
        break;
    case DST_LINE_START:
        s->regs.dst_line_start = data;
        break;
    case DST_LINE_END:
        ati_2d_line(s, s->regs.dst_line_start, data);
        s->regs.dst_line_start = data;
        break;
    case DP_CNTL:
        s->regs.dp_cntl = data;
        break;
    case DP_SRC_FRGD_CLR:
        s->regs.dp_src_frgd_clr = data;
        break;
    case DP_SRC_BKGD_CLR:
        s->regs.dp_src_bkgd_clr = data;
        break;
    case DP_DATATYPE:
        s->regs.dp_datatype = data & 0xe0070f0f;
        break;
    case DP_MIX:
        s->regs.dp_mix = data & 0x00ff0700;
        break;
    case DP_WRITE_MASK:
        s->regs.dp_write_mask = data;
        break;
    case DEFAULT_OFFSET:
        if (s->dev_id == PCI_DEVICE_ID_ATI_RAGE128_PF) {
            s->regs.default_offset = data & 0xfffffff0;
        } else {
            /* Radeon has DEFAULT_PITCH_OFFSET here like DST_PITCH_OFFSET */
            s->regs.default_offset = (data & 0x3fffff) << 10;
            s->regs.default_pitch = (data & 0x3fc00000) >> 16;
            s->regs.default_tile = data >> 30;
        }
        break;
    case DEFAULT_PITCH:
        if (s->dev_id == PCI_DEVICE_ID_ATI_RAGE128_PF) {
            s->regs.default_pitch = data & 0x3fff;
            s->regs.default_tile = (data >> 16) & 1;
        }
        break;
    case DEFAULT_SC_BOTTOM_RIGHT:
        s->regs.default_sc_right = data & 0x3fff;
        s->regs.default_sc_bottom = (data >> 16) & 0x3fff;
        break;
    case SC_TOP_LEFT:
        s->regs.sc_left = data & 0x3fff;
        s->regs.sc_top = (data >> 16) & 0x3fff;
        break;
    case SC_LEFT:
        s->regs.sc_left = data & 0x3fff;
        break;
    case SC_TOP:
        s->regs.sc_top = data & 0x3fff;
        break;
    case SC_BOTTOM_RIGHT:
        s->regs.sc_right = data & 0x3fff;
        s->regs.sc_bottom = (data >> 16) & 0x3fff;
        break;
    case SC_RIGHT:
        s->regs.sc_right = data & 0x3fff;
        break;
    case SC_BOTTOM:
        s->regs.sc_bottom = data & 0x3fff;
        break;
    case SRC_SC_BOTTOM_RIGHT:
        s->regs.src_sc_right = data & 0x3fff;
        s->regs.src_sc_bottom = (data >> 16) & 0x3fff;
        break;
    case SRC_SC_RIGHT:
        s->regs.src_sc_right = data & 0x3fff;
        break;
    case SRC_SC_BOTTOM:
        s->regs.src_sc_bottom = data & 0x3fff;
        break;
    case HOST_DATA0:
    case HOST_DATA1:
    case HOST_DATA2:
    case HOST_DATA3:
    case HOST_DATA4:
    case HOST_DATA5:
    case HOST_DATA6:
    case HOST_DATA7:
    case HOST_DATA_LAST:
        if (!s->host_data.active) {
            break;
        }
        s->host_data.acc[s->host_data.next++] = data;
        if (addr == HOST_DATA_LAST) {
            ati_host_data_finish(s);
            s->host_data.next = 0;
        } else if (s->host_data.next >= 4) {
            ati_host_data_flush(s);
            s->host_data.next = 0;
        }
        break;
    case SCALE_3D_CNTL ... SCALE_3D_CNTL + 3:
        ati_reg_write_offs(&s->regs.scale_3d_cntl,
                           addr - SCALE_3D_CNTL, data, size);
        break;
    case SETUP_CNTL ... SETUP_CNTL + 3:
        ati_reg_write_offs(&s->regs.setup_cntl,
                           addr - SETUP_CNTL, data, size);
        break;
    case 0x1a40 ... 0x1a63:
    {
        /*
         * Setup-engine per-channel colour DDA.  Three channels (R,G,B) on a
         * 12-byte stride, each {dx, dy, value} at +0/+4/+8 (signed 16.16).
         * A write arms a Gouraud fill; it stays armed across the clip-rect
         * blits of one gradient (a fresh plane starts a new burst).
         */
        unsigned off = addr - 0x1a40;
        unsigned ch = off / 0xc;
        unsigned field = off % 0xc;
        int32_t *reg = field < 4 ? &s->regs.su_color_dx[ch] :
                       field < 8 ? &s->regs.su_color_dy[ch] :
                                   &s->regs.su_color_val[ch];

        ati_reg_write_offs((uint32_t *)reg, addr & 3, data, size);
        s->regs.su_gouraud_armed = true;
        s->regs.su_gouraud_rect_valid = false;
        break;
    }
    default:
    {
        int aux = ati_init_aux_slot(addr);

        if (aux >= 0) {
            ati_reg_write_offs(&s->regs.init_aux[aux], addr & 3, data, size);
        }
        break;
    }
    }
}

static const MemoryRegionOps ati_mm_ops = {
    .read = ati_mm_read,
    .write = ati_mm_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static bool ati_init_regs_needed(void *opaque)
{
    ATIVGARegs *r = opaque;
    unsigned i;

    if (r->clock_cntl_index) {
        return true;
    }
    for (i = 0; i < ARRAY_SIZE(r->pll_regs); i++) {
        if (r->pll_regs[i]) {
            return true;
        }
    }
    for (i = 0; i < ARRAY_SIZE(r->init_aux); i++) {
        if (r->init_aux[i]) {
            return true;
        }
    }
    return r->pm4_dl_rptr || r->pm4_dl_wptr;
}

static const VMStateDescription vmstate_ati_vga_init_regs = {
    .name = "ati-vga/regs/init",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = ati_init_regs_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(clock_cntl_index, ATIVGARegs),
        VMSTATE_UINT32_ARRAY(pll_regs, ATIVGARegs, 32),
        VMSTATE_UINT32_ARRAY(init_aux, ATIVGARegs, 40),
        VMSTATE_UINT32(pm4_dl_rptr, ATIVGARegs),
        VMSTATE_UINT32(pm4_dl_wptr, ATIVGARegs),
        VMSTATE_UINT32(crtc_vblank_ack_frame, ATIVGARegs),
        VMSTATE_END_OF_LIST()
    }
};

static bool ati_brush_regs_needed(void *opaque)
{
    ATIVGARegs *r = opaque;

    return r->brush_y_x || r->brush_data0 || r->brush_data1 ||
           r->dst_line_start || r->clr_cmp_cntl || r->clr_cmp_clr_src ||
           r->clr_cmp_clr_dst || r->clr_cmp_msk;
}

static const VMStateDescription vmstate_ati_vga_brush_regs = {
    .name = "ati-vga/regs/brush",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = ati_brush_regs_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(brush_y_x, ATIVGARegs),
        VMSTATE_UINT32(brush_data0, ATIVGARegs),
        VMSTATE_UINT32(brush_data1, ATIVGARegs),
        VMSTATE_UINT32(dst_line_start, ATIVGARegs),
        VMSTATE_UINT32(clr_cmp_cntl, ATIVGARegs),
        VMSTATE_UINT32(clr_cmp_clr_src, ATIVGARegs),
        VMSTATE_UINT32(clr_cmp_clr_dst, ATIVGARegs),
        VMSTATE_UINT32(clr_cmp_msk, ATIVGARegs),
        VMSTATE_END_OF_LIST()
    }
};

static bool ati_setup_regs_needed(void *opaque)
{
    ATIVGARegs *r = opaque;
    unsigned i;

    if (r->scale_3d_cntl || r->setup_cntl || r->su_gouraud_armed) {
        return true;
    }
    for (i = 0; i < 3; i++) {
        if (r->su_color_dx[i] || r->su_color_dy[i] || r->su_color_val[i]) {
            return true;
        }
    }
    return false;
}

static const VMStateDescription vmstate_ati_vga_setup_regs = {
    .name = "ati-vga/regs/setup",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = ati_setup_regs_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(scale_3d_cntl, ATIVGARegs),
        VMSTATE_UINT32(setup_cntl, ATIVGARegs),
        VMSTATE_INT32_ARRAY(su_color_dx, ATIVGARegs, 3),
        VMSTATE_INT32_ARRAY(su_color_dy, ATIVGARegs, 3),
        VMSTATE_INT32_ARRAY(su_color_val, ATIVGARegs, 3),
        VMSTATE_BOOL(su_gouraud_armed, ATIVGARegs),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_ati_vga_regs = {
    .name = "ati-vga/regs",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(mm_index, ATIVGARegs),
        VMSTATE_UINT32_ARRAY(bios_scratch, ATIVGARegs, 8),
        VMSTATE_UINT32(gen_int_cntl, ATIVGARegs),
        VMSTATE_UINT32(gen_int_status, ATIVGARegs),
        VMSTATE_UINT32(crtc_gen_cntl, ATIVGARegs),
        VMSTATE_UINT32(crtc_ext_cntl, ATIVGARegs),
        VMSTATE_UINT32(dac_cntl, ATIVGARegs),
        VMSTATE_UINT32(gpio_vga_ddc, ATIVGARegs),
        VMSTATE_UINT32(gpio_dvi_ddc, ATIVGARegs),
        VMSTATE_UINT32(gpio_monid, ATIVGARegs),
        VMSTATE_UINT32(config_cntl, ATIVGARegs),
        VMSTATE_UINT32_ARRAY(palette, ATIVGARegs, 256),
        VMSTATE_UINT32(crtc_h_total_disp, ATIVGARegs),
        VMSTATE_UINT32(crtc_h_sync_strt_wid, ATIVGARegs),
        VMSTATE_UINT32(crtc_v_total_disp, ATIVGARegs),
        VMSTATE_UINT32(crtc_v_sync_strt_wid, ATIVGARegs),
        VMSTATE_UINT32(crtc_offset, ATIVGARegs),
        VMSTATE_UINT32(crtc_offset_cntl, ATIVGARegs),
        VMSTATE_UINT32(crtc_pitch, ATIVGARegs),
        VMSTATE_UINT32(cur_offset, ATIVGARegs),
        VMSTATE_UINT32(cur_hv_pos, ATIVGARegs),
        VMSTATE_UINT32(cur_hv_offs, ATIVGARegs),
        VMSTATE_UINT32(cur_color0, ATIVGARegs),
        VMSTATE_UINT32(cur_color1, ATIVGARegs),
        VMSTATE_UINT32(dst_offset, ATIVGARegs),
        VMSTATE_UINT32(dst_pitch, ATIVGARegs),
        VMSTATE_UINT32(dst_tile, ATIVGARegs),
        VMSTATE_UINT32(dst_width, ATIVGARegs),
        VMSTATE_UINT32(dst_height, ATIVGARegs),
        VMSTATE_UINT32(src_offset, ATIVGARegs),
        VMSTATE_UINT32(src_pitch, ATIVGARegs),
        VMSTATE_UINT32(src_tile, ATIVGARegs),
        VMSTATE_UINT32(src_x, ATIVGARegs),
        VMSTATE_UINT32(src_y, ATIVGARegs),
        VMSTATE_UINT32(dst_x, ATIVGARegs),
        VMSTATE_UINT32(dst_y, ATIVGARegs),
        VMSTATE_UINT32(dp_gui_master_cntl, ATIVGARegs),
        VMSTATE_UINT32(dp_brush_bkgd_clr, ATIVGARegs),
        VMSTATE_UINT32(dp_brush_frgd_clr, ATIVGARegs),
        VMSTATE_UINT32(dp_src_frgd_clr, ATIVGARegs),
        VMSTATE_UINT32(dp_src_bkgd_clr, ATIVGARegs),
        VMSTATE_UINT16(sc_top, ATIVGARegs),
        VMSTATE_UINT16(sc_left, ATIVGARegs),
        VMSTATE_UINT16(sc_bottom, ATIVGARegs),
        VMSTATE_UINT16(sc_right, ATIVGARegs),
        VMSTATE_UINT16(src_sc_bottom, ATIVGARegs),
        VMSTATE_UINT16(src_sc_right, ATIVGARegs),
        VMSTATE_UINT32(dp_cntl, ATIVGARegs),
        VMSTATE_UINT32(dp_datatype, ATIVGARegs),
        VMSTATE_UINT32(dp_mix, ATIVGARegs),
        VMSTATE_UINT32(dp_write_mask, ATIVGARegs),
        VMSTATE_UINT32(default_offset, ATIVGARegs),
        VMSTATE_UINT32(default_pitch, ATIVGARegs),
        VMSTATE_UINT16(default_sc_bottom, ATIVGARegs),
        VMSTATE_UINT16(default_sc_right, ATIVGARegs),
        VMSTATE_UINT32(default_tile, ATIVGARegs),
        VMSTATE_END_OF_LIST()
    },
    .subsections = (const VMStateDescription * const []) {
        &vmstate_ati_vga_init_regs,
        &vmstate_ati_vga_brush_regs,
        &vmstate_ati_vga_setup_regs,
        NULL
    }
};

static const VMStateDescription vmstate_ati_host_data = {
    .name = "ati-vga/host-data",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_BOOL(active, ATIHostDataState),
        VMSTATE_UINT32(row, ATIHostDataState),
        VMSTATE_UINT32(col, ATIHostDataState),
        VMSTATE_UINT32(next, ATIHostDataState),
        VMSTATE_UINT32_ARRAY(acc, ATIHostDataState, 4),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_ati_bitbang_i2c = {
    .name = "ati-vga/bitbang-i2c",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_SINGLE(state, bitbang_i2c_interface, 0,
                       vmstate_info_int32, bitbang_i2c_state),
        VMSTATE_INT32(last_data, bitbang_i2c_interface),
        VMSTATE_INT32(last_clock, bitbang_i2c_interface),
        VMSTATE_INT32(device_out, bitbang_i2c_interface),
        VMSTATE_UINT8(buffer, bitbang_i2c_interface),
        VMSTATE_INT32(current_addr, bitbang_i2c_interface),
        VMSTATE_END_OF_LIST()
    },
};

static int ati_vga_post_load(void *opaque, int version_id)
{
    ATIVGAState *s = opaque;
    bool cursor_enabled = s->regs.crtc_gen_cntl & CRTC2_CUR_EN;

    if (s->host_data.next >= ARRAY_SIZE(s->host_data.acc) ||
        s->bbi2c.state < STOPPED || s->bbi2c.state > SENT_NACK) {
        return -EINVAL;
    }

    s->mode = s->regs.crtc_gen_cntl & CRTC2_EXT_DISP_EN ?
              EXT_MODE : VGA_MODE;
    s->vga.graphic_mode = -1;
    if (s->cursor_guest_mode) {
        s->vga.force_shadow = cursor_enabled;
        s->cursor_size = UINT16_MAX;
    } else {
        s->vga.force_shadow = false;
        if (cursor_enabled) {
            ati_cursor_define(s);
        }
        dpy_mouse_set(s->vga.con, s->regs.cur_hv_pos >> 16,
                      s->regs.cur_hv_pos & 0xffff, cursor_enabled);
    }
    ati_vga_update_irq(s);
    graphic_hw_invalidate(s->vga.con);
    return 0;
}

static const VMStateDescription vmstate_ati_vga = {
    .name = "ati-vga",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = ati_vga_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_PCI_DEVICE(dev, ATIVGAState),
        VMSTATE_STRUCT(vga, ATIVGAState, 0,
                       vmstate_vga_common, VGACommonState),
        VMSTATE_STRUCT(regs, ATIVGAState, 0,
                       vmstate_ati_vga_regs, ATIVGARegs),
        VMSTATE_STRUCT(host_data, ATIVGAState, 0,
                       vmstate_ati_host_data, ATIHostDataState),
        VMSTATE_STRUCT(bbi2c, ATIVGAState, 0,
                       vmstate_ati_bitbang_i2c, bitbang_i2c_interface),
        VMSTATE_TIMER(vblank_timer, ATIVGAState),
        VMSTATE_END_OF_LIST()
    },
};

static void ati_vga_realize(PCIDevice *dev, Error **errp)
{
    ATIVGAState *s = ATI_VGA(dev);
    VGACommonState *vga = &s->vga;
    I2CBus *i2cbus;

    /*
     * The Rage 128 answers expansion-ROM reads as soon as the ROM BAR's
     * enable bit is set, memory decode or not.  The Intel SDV / HP i2000
     * firmware relies on that: its PCI enumeration leaves the card's command
     * register at bus-master-only (it treats the card as a second VGA and
     * disables decode) and its CSM then reads the BIOS through the ROM BAR
     * with Memory Space Enable still clear; the BIOS's own POST enables the
     * card.  Without this the CSM reads zeros, skips the video POST and the
     * console never comes up.
     */
    dev->rom_decodes_without_memory_enable = true;

#ifndef CONFIG_PIXMAN
    if (s->use_pixman != 0) {
        warn_report("x-pixman != 0, not effective without PIXMAN");
    }
#endif

    if (s->model) {
        int i;
        for (i = 0; i < ARRAY_SIZE(ati_model_aliases); i++) {
            if (!strcmp(s->model, ati_model_aliases[i].name)) {
                s->dev_id = ati_model_aliases[i].dev_id;
                break;
            }
        }
        if (i >= ARRAY_SIZE(ati_model_aliases)) {
            warn_report("Unknown ATI VGA model name, "
                        "using default rage128p");
        }
    }
    if (s->dev_id != PCI_DEVICE_ID_ATI_RAGE128_PF &&
        s->dev_id != PCI_DEVICE_ID_ATI_RADEON_QY) {
        error_setg(errp, "Unknown ATI VGA device id, "
                   "only 0x5046 and 0x5159 are supported");
        return;
    }
    pci_set_word(dev->config + PCI_DEVICE_ID, s->dev_id);

    if (s->dev_id == PCI_DEVICE_ID_ATI_RADEON_QY &&
        s->vga.vram_size_mb < 16) {
        warn_report("Too small video memory for device id");
        s->vga.vram_size_mb = 16;
    }

    /* init vga bits */
    if (!vga_common_init(vga, OBJECT(s), errp)) {
        return;
    }
    vga->vbe_legacy_mode_switch = true;
    vga_init(vga, OBJECT(s), pci_address_space(dev),
             pci_address_space_io(dev), true);
    vga->con = graphic_console_init(DEVICE(s), 0, s->vga.hw_ops, vga);
    if (s->cursor_guest_mode) {
        vga->cursor_invalidate = ati_cursor_invalidate;
        vga->cursor_draw_line = ati_cursor_draw_line;
    }

    /* ddc, edid */
    i2cbus = i2c_init_bus(DEVICE(s), "ati-vga.ddc");
    bitbang_i2c_init(&s->bbi2c, i2cbus);
    i2c_slave_set_address(I2C_SLAVE(&s->i2cddc), 0x50);
    qdev_realize(DEVICE(&s->i2cddc), BUS(i2cbus), &error_abort);

    /* mmio register space */
    memory_region_init_io(&s->mm, OBJECT(s), &ati_mm_ops, s,
                          "ati.mmregs", 0x4000);
    /* io space is alias to beginning of mmregs */
    memory_region_init_alias(&s->io, OBJECT(s), "ati.io", &s->mm, 0, 0x100);

    /*
     * The framebuffer is at the beginning of the linear aperture. For
     * Rage128 the upper half of the aperture is reserved for an AGP
     * window (which we do not emulate.)
     */
    if (!s->linear_aper_sz) {
        if (s->dev_id == PCI_DEVICE_ID_ATI_RAGE128_PF) {
            s->linear_aper_sz = ATI_RAGE128_LINEAR_APER_SIZE;
        } else {
            s->linear_aper_sz = ATI_R100_LINEAR_APER_SIZE;
        }
    }
    if (s->linear_aper_sz > 256 * MiB) {
        error_setg(errp, "x-linear-aper-size is too large (maximum 256 MiB)");
        return;
    }
    if (s->linear_aper_sz < 16 * MiB) {
        error_setg(errp, "x-linear-aper-size is too small (minimum 16 MiB)");
        return;
    }
    memory_region_init(&s->linear_aper, OBJECT(dev), "ati-linear-aperture0",
                       s->linear_aper_sz);
    memory_region_add_subregion(&s->linear_aper, 0, &vga->vram);

    pci_register_bar(dev, 0, PCI_BASE_ADDRESS_MEM_PREFETCH, &s->linear_aper);
    pci_register_bar(dev, 1, PCI_BASE_ADDRESS_SPACE_IO, &s->io);
    pci_register_bar(dev, 2, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->mm);

    /* most interrupts are not yet emulated but MacOS needs at least VBlank */
    dev->config[PCI_INTERRUPT_PIN] = 1;
    timer_init_ns(&s->vblank_timer, QEMU_CLOCK_VIRTUAL, ati_vga_vblank_irq, s);

    /*
     * Optional PCI AGP capability, at the Rage 128's real offset 0x50.  Two
     * guests key on it: Linux sba_iommu scans every PCI device for an AGP
     * capability and, finding one, reserves half its IOVA space as the AGP GART
     * (and writes the handshake cookie hp-agp looks for); and the r128 DRM will
     * negotiate AGP mode against it.  Off by default -- on the zx1 machine the
     * Rage 128 reaches memory through the SBA in PCI-GART mode, which needs no
     * AGP capability; the ia64 machine only sets this when its agp option opts
     * into the hp-agp AGP path.
     */
    if (s->agp) {
        if (pci_add_capability(dev, PCI_CAP_ID_AGP, 0x50, 8, errp) < 0) {
            return;
        }
        /* AGP 2.0, 1x/2x/4x, so the OS negotiates a rate. */
        pci_set_long(dev->config +
                     pci_find_capability(dev, PCI_CAP_ID_AGP) + PCI_AGP_STATUS,
                     0x1f000207);
    }
}

static void ati_vga_reset(DeviceState *dev)
{
    ATIVGAState *s = ATI_VGA(dev);

    timer_del(&s->vblank_timer);
    ati_vga_update_irq(s);

    /*
     * PLL and init-register power-up values from the RAGE 128 PRO Register
     * Reference Guide.  PLL indices follow the chip's PLL address space:
     * 0x01 CLK_PIN_CNTL, 0x02 PPLL_CNTL, 0x0b XPLL_CNTL, 0x0c XDLL_CNTL,
     * 0x0e MPLL_CNTL, 0x10 AGP_PLL_CNTL.
     */
    s->regs.clock_cntl_index = 0;
    memset(s->regs.pll_regs, 0, sizeof(s->regs.pll_regs));
    s->regs.pll_regs[0x01] = 0x000000f7;
    s->regs.pll_regs[0x02] = 0x0000cc03;
    s->regs.pll_regs[0x0b] = 0x0000cc03;
    s->regs.pll_regs[0x0c] = 0x000b000b;
    s->regs.pll_regs[0x0e] = 0x0000cc03;
    s->regs.pll_regs[0x10] = 0x7a770000;
    memset(s->regs.init_aux, 0, sizeof(s->regs.init_aux));
    s->regs.init_aux[ati_init_aux_slot(0x0030)] = 0x880f0f41; /* BUS_CNTL */
    s->regs.init_aux[ati_init_aux_slot(0x0034)] = 0x0000001f; /* BUS_CNTL1 */
    /* MEM_CNTL: MEM_LATENCY = 3 clocks, MEM_REFRESH_DIS set out of reset. */
    s->regs.init_aux[ati_init_aux_slot(0x0140)] = 0x08000300;

    /* 2D setup engine (caption-gradient colour interpolator). */
    s->regs.scale_3d_cntl = 0;
    s->regs.setup_cntl = 0;
    s->regs.su_gouraud_armed = false;
    s->regs.su_gouraud_rect_valid = false;
    memset(s->regs.su_color_dx, 0, sizeof(s->regs.su_color_dx));
    memset(s->regs.su_color_dy, 0, sizeof(s->regs.su_color_dy));
    memset(s->regs.su_color_val, 0, sizeof(s->regs.su_color_val));

    /* reset vga */
    vga_common_reset(&s->vga);
    s->mode = VGA_MODE;

    s->host_data.active = false;
    s->host_data.next = 0;
    s->host_data.row = 0;
    s->host_data.col = 0;
}

static void ati_vga_exit(PCIDevice *dev)
{
    ATIVGAState *s = ATI_VGA(dev);

    timer_del(&s->vblank_timer);
    graphic_console_close(s->vga.con);
}

static const Property ati_vga_properties[] = {
    DEFINE_PROP_UINT32("vgamem_mb", ATIVGAState, vga.vram_size_mb, 16),
    DEFINE_PROP_STRING("model", ATIVGAState, model),
    DEFINE_PROP_UINT16("x-device-id", ATIVGAState, dev_id,
                       PCI_DEVICE_ID_ATI_RAGE128_PF),
    DEFINE_PROP_BOOL("guest_hwcursor", ATIVGAState, cursor_guest_mode, false),
    DEFINE_PROP_BOOL("agp", ATIVGAState, agp, false),
    /* this is a debug option, prefer PROP_UINT over PROP_BIT for simplicity */
    DEFINE_PROP_UINT8("x-pixman", ATIVGAState, use_pixman, DEFAULT_X_PIXMAN),
    DEFINE_PROP_UINT64("x-linear-aper-size", ATIVGAState, linear_aper_sz, 0),
    DEFINE_EDID_PROPERTIES(ATIVGAState, i2cddc.edid_info),
};

static void ati_vga_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);


    device_class_set_legacy_reset(dc, ati_vga_reset);
    device_class_set_props(dc, ati_vga_properties);
    dc->vmsd = &vmstate_ati_vga;
    dc->hotpluggable = false;
    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);

    k->class_id = PCI_CLASS_DISPLAY_VGA;
    k->vendor_id = PCI_VENDOR_ID_ATI;
    k->device_id = PCI_DEVICE_ID_ATI_RAGE128_PF;
    k->romfile = "vgabios-ati.bin";
    k->realize = ati_vga_realize;
    k->exit = ati_vga_exit;
}

static void ati_vga_init(Object *o)
{
    ATIVGAState *s = ATI_VGA(o);

    object_initialize_child(o, "edid", &s->i2cddc, TYPE_I2CDDC);
    object_property_set_description(o, "x-pixman", "Use pixman for: "
                                    "1: fill, 2: blit");
}

static const TypeInfo ati_vga_info = {
    .name = TYPE_ATI_VGA,
    .parent = TYPE_PCI_DEVICE,
    .instance_size = sizeof(ATIVGAState),
    .class_init = ati_vga_class_init,
    .instance_init = ati_vga_init,
    .interfaces = (const InterfaceInfo[]) {
          { INTERFACE_CONVENTIONAL_PCI_DEVICE },
          { },
    },
};

static void ati_vga_register_types(void)
{
    type_register_static(&ati_vga_info);
}

type_init(ati_vga_register_types)
