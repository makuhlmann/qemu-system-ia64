/*
 * QEMU ATI Mach64 "3D Rage" (DEV_4754) emulation - internal header
 *
 * A minimal-but-growing model of the classic Mach64 GUI (2D) engine: a
 * synchronous MMIO rectangle/line/host blitter over a linear framebuffer,
 * with a VGA-compatible core provided by VGACommonState.  There is no command
 * processor, ring, DMA or AGP on this chip, so none of the r128 CCE machinery
 * applies.
 *
 * This work is licensed under the GNU GPL license version 2 or later.
 */

#ifndef MACH64_INT_H
#define MACH64_INT_H

#include "qemu/units.h"
#include "qemu/timer.h"
#include "hw/pci/pci_device.h"
#include "hw/i2c/i2c.h"
#include "hw/display/i2c-ddc.h"
#include "vga_int.h"
#include "qom/object.h"
#include "ui/console.h"
#include "mach64_regs.h"

#define PCI_VENDOR_ID_ATI            0x1002
/* Mach64 GR = Rage XL (the default: the one id XP 2002 *and* XP 2003 auto-match) */
#define PCI_DEVICE_ID_ATI_MACH64_GR  0x4752
/* Mach64 GT = "3D Rage II"; XP 2002 only, and only with a matching REV */
#define PCI_DEVICE_ID_ATI_MACH64_GT  0x4754

/* Auto-selected revision sentinel for the x-revision property. */
#define MACH64_REV_AUTO              0x100
/* Plausible per-chip PCI revisions.  For Rage XL the inbox INFs carry no REV
 * qualifier so any value matches; for 3D Rage II XP 2002 requires one of
 * {01,19,1A,41,5A,9A} (display.inf), 9A = "3D RAGE II+ PCI". */
#define MACH64_REV_RAGE_XL           0x27
#define MACH64_REV_3DRAGE_II         0x9a

#define MACH64_LINEAR_APER_SIZE      (16 * MiB)

enum { VGA_MODE, EXT_MODE };

#define TYPE_MACH64_VGA "mach64-vga"
OBJECT_DECLARE_SIMPLE_TYPE(Mach64VGAState, MACH64_VGA)

/*
 * Host-data (CPU-to-screen) blit state.  A blit whose foreground source is
 * SRC_HOST (or whose mono source is HOST) is set up by the DST_HEIGHT_WIDTH
 * write, then consumes dwords the guest streams into HOST_DATA*.  We walk the
 * destination rectangle pixel by pixel as the data arrives.
 */
typedef struct Mach64HostData {
    bool active;
    bool mono;            /* 1bpp expansion vs. colour copy */
    unsigned x;           /* current dst column within the rectangle */
    unsigned y;           /* current dst row within the rectangle */
} Mach64HostData;

struct Mach64VGAState {
    PCIDevice dev;
    VGACommonState vga;

    uint16_t dev_id;
    uint16_t revision;            /* x-revision property; MACH64_REV_AUTO = pick */
    uint8_t chip_rev;             /* effective PCI revision, resolved in realize */
    uint8_t mode;                 /* VGA_MODE or EXT_MODE */
    uint8_t use_pixman;
    bool cursor_guest_mode;

    /* Hardware cursor host-overlay bookkeeping (see mach64.c). */
    uint16_t cursor_size;
    uint32_t cursor_offset;
    QEMUCursor *cursor;

    MemoryRegion linear_aper;
    MemoryRegion io;
    MemoryRegion mm;
    MemoryRegion vga_aper;
    MemoryRegion be_aper;
    MemoryRegion aper_block0;
    MemoryRegion aper_block1;

    /* Block-0 register file, indexed by Mach64 block index (see mach64_regs.h). */
    uint32_t regs[MACH64_NREGS];

    /*
     * Block-1 overlay/scaler registers as written, and the double-buffered
     * set the display uses (mach64_overlay.c).
     */
    uint32_t regs1[MACH64_NREGS1];
    uint32_t ovl[MACH64_NREGS1];
    bool ovl_locked;
    int ovl_drawn_y0, ovl_drawn_y1;

    /*
     * Internal PLL / clock generator.  The Mach64 exposes its PLL registers
     * indirectly through CLOCK_CNTL (0x24): byte 1 selects a PLL register
     * ((addr << 2) | PLL_WR_EN) and byte 2 is a data window over the selected
     * register.  Without this, native drivers (ati2mpad's ReadPllRegisterUchar
     * / GetMCLK path) that read back a PLL register spin forever polling a
     * value the plain CLOCK_CNTL latch never produces.
     */
    uint8_t pll_regs[64];

    Mach64HostData host_data;

    /* Synthetic 60 Hz vertical-blank interrupt (CRTC_INT_CNTL). */
    QEMUTimer vblank_timer;

    /*
     * CRT DDC/EDID monitor detection.  The native ATI miniport bit-bangs I2C
     * over an indexed GPIO byte (LCD register 7, reached through the
     * LCD_INDEX/LCD_DATA pair): SCL=bit6, SDA=bit5, open-drain.  Without a
     * valid EDID the miniport spins forever in its DAC load-sense fallback, so
     * back the bus with an i2c-ddc slave.
     */
    I2CBus *ddc_bus;
    I2CBus *amc_bus;            /* the hardware I2C engine's pins */
    bool amc_active;            /* an addressed slave acknowledged */
    I2CDDCState i2cddc;
    uint8_t lcd_index;
    uint8_t ddc_dir;            /* pin directions   (SDA/SCL bit5/bit6; 1=output) */
    uint8_t ddc_state;          /* output state     (SDA/SCL bit5/bit6) */
    uint32_t ddc_gpio;          /* assembled LCD_DATA readback */
    /* Bit-bang -> I2C decoder state (proper open-drain wired-AND, see mach64.c). */
    uint8_t ddc_scl;            /* last master SCL level (1 high) */
    uint8_t ddc_sda_m;          /* last master SDA drive (1 released) */
    uint8_t ddc_slave_sda;      /* slave's SDA contribution (1 released, 0 low) */
    uint8_t ddc_bus_state;      /* DDC_IDLE / DDC_ADDR / DDC_WRITE / DDC_READ */
    uint8_t ddc_cnt;            /* bit counter within the current byte (0..9) */
    uint8_t ddc_buf;            /* byte being shifted in/out */
    uint8_t ddc_read_nacked;    /* master NACKed the last read byte (end) */
    int16_t ddc_addr;           /* current I2C address, -1 when unaddressed */
};

/* mach64_2d.c */
void mach64_2d_dst_trigger(Mach64VGAState *s);
void mach64_2d_line_trigger(Mach64VGAState *s);
void mach64_2d_host_data(Mach64VGAState *s, uint32_t data);

/* mach64_overlay.c */
bool mach64_ovl_active(const Mach64VGAState *s);
bool mach64_ovl_read(Mach64VGAState *s, unsigned reg, uint32_t *val);
void mach64_ovl_write(Mach64VGAState *s, unsigned reg, unsigned byte,
                      unsigned size, uint32_t data);
uint32_t mach64_ovl_vblank(Mach64VGAState *s);
void mach64_ovl_reset(Mach64VGAState *s);
void mach64_ovl_invalidate(Mach64VGAState *s);
void mach64_ovl_draw_line(Mach64VGAState *s, uint8_t *d, int scr_y);

/* mach64.c helpers shared with the engine. */
void mach64_update_shadow(Mach64VGAState *s);
int mach64_dst_bpp(const Mach64VGAState *s);
uint32_t mach64_dst_base(const Mach64VGAState *s);
int mach64_dst_pitch_bytes(const Mach64VGAState *s);
void mach64_2d_set_dirty(Mach64VGAState *s, uint32_t base, int x, int y,
                         int w, int h);

#endif /* MACH64_INT_H */
