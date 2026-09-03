/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * IA-64 virtual PC platform.
 *
 * Provides RAM, a bootstrap CPU, a memory-mapped serial console,
 * firmware ROM loading via -bios, a PCI host bridge, SCSI and AHCI storage
 * controllers, an Ethernet controller, OHCI/UHCI USB,
 * local SAPIC/I/O SAPIC wiring,
 * and ACPI fixed power-management registers.
 */

#include "qemu/osdep.h"

#include CONFIG_DEVICES

#include "qemu/units.h"
#include "qemu/cutils.h"
#include "qemu/datadir.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "qapi/visitor.h"
#include "hw/core/boards.h"
#include "hw/core/cpu.h"
#include "hw/core/qdev-properties.h"
#include "hw/char/serial-mm.h"
#include "hw/display/bochs-vbe.h"
#include "hw/display/edid.h"
#include "hw/display/vga_regs.h"
#include "hw/core/loader.h"
#include "hw/core/sysbus.h"
#include "hw/block/flash.h"
#include "system/block-backend.h"
#include "block/block.h"
#include "qobject/qdict.h"
#include "hw/ide/ahci-pci.h"
#include "hw/ide/ide-dev.h"
#include "hw/ide/pci.h"
#include "hw/input/i8042.h"
#include "hw/southbridge/intel_82468gx.h"
#include "hw/ia64/ia64_460gx_identity.h"
#include "hw/acpi/acpi.h"
#ifdef CONFIG_IA64_VPC_STORAGE
#include "hw/scsi/isp12160.h"
#include "hw/scsi/scsi.h"
#endif
#ifdef CONFIG_IA64_VPC_AUDIO
#include "hw/audio/cs4281.h"
#endif
#include "hw/pci/pci.h"
#include "hw/pci/pci_bus.h"
#include "hw/pci/pci_device.h"
#include "hw/pci/pci_host.h"
#include "net/net.h"
#include "hw/isa/isa.h"
#include "hw/rtc/mc146818rtc.h"
#include "hw/intc/i8259.h"
#include "hw/timer/i8254.h"
#include "hw/usb/hcd-uhci.h"
#include "hw/usb/usb.h"
#include "hw/ia64/ia64_loader.h"
#include "hw/ia64/ia64_pci.h"
#include "hw/ia64/ia64_iosapic.h"
#include "hw/ia64/ia64_agp.h"
#include "hw/ia64/ia64_sba.h"
#include "hw/ia64/ia64_lba.h"
#include "hw/ia64/ia64_mercury.h"
#include "hw/ia64/ia64_expander.h"
#include "hw/core/or-irq.h"
#include "migration/vmstate.h"
#include "system/address-spaces.h"
#include "system/rtc.h"
#include "system/runstate.h"
#include "system/system.h"
#include "system/reset.h"
#include "system/watchdog.h"
#include "target/ia64/cpu-qom.h"
#include "target/ia64/cpu.h"

/* The firmware's 1 MB link base; it executes from the RAM-top shadow. */
#define IA64_FW_LINK_BASE 0x0000000000100000ULL
#define IA64_FW_BASE    0x0000000000100000ULL
/*
 * Firmware image loaded when no -bios is given.  It is installed beside the
 * binary (share/), so an unpacked package runs without naming it every time.
 */
#define IA64_VPC_DEFAULT_FIRMWARE "ia64-firmware.bin"
/*
 * Low (sub-aperture) DRAM runs contiguously from 0 up to the PCI/MMIO
 * aperture, exactly as the real 460GX keeps a single MMIO gap at the top of
 * the 32-bit space; RAM displaced by that gap is remapped above 4 GiB.  There
 * is no DRAM island between the aperture and the chipset/SAPIC region.
 */
#define IA64_LOW_RAM_LIMIT IA64_PCI_MMIO_BASE
/*
 * The firmware address space, RTC/watchdog/NVRAM devices, IVT, IOSAPIC,
 * local SAPIC and ACPI PM block addresses are shared with the firmware via
 * hw/ia64/ia64_vpc_abi.h.
 */
#define IA64_FIRMWARE_ADDRESS_SPACE_BASE IA64_FW_ADDRESS_SPACE_BASE
#define IA64_FIRMWARE_ADDRESS_SPACE_SIZE IA64_FW_ADDRESS_SPACE_SIZE

/*
 * Real-firmware (realfw) mode: a vendor flash image mapped so that it ends
 * exactly at 4 GiB, with the architected reset pointer block in its last
 * 48 bytes (SAL sec 2.5): 4 GiB-48 = PAL_A FIT entry, -32 = FIT pointer,
 * -24 = SALE_ENTRY pointer.  See plans/phase5-real-firmware-boot.md.
 */
#define IA64_REALFW_WINDOW_END    IA64_U64(0x0000000100000000)
#define IA64_REALFW_MAX_SIZE      IA64_U64(0x0000000000400000)
/*
 * PAL procedure entry handed to real SAL in GR34/GR36 (and recognized via
 * env->pal.pal_proc_copy_addr): a stub in the firmware address-space RAM,
 * below the flash window and clear of the watchdog/handoff pages.
 */
#define IA64_REALFW_PAL_STUB_BASE IA64_U64(0x00000000ff100000)
/*
 * Capture IVT (realfw mode): a 32 KiB-aligned interruption vector table
 * whose every bundle is a branch-to-self, planted in firmware scratch RAM
 * and pointed to by cr.iva in the synthesized SALE_ENTRY entry state.  Real
 * PAL provides an IVT before entering SAL (SDM 11.2.2); we skip PAL, so
 * without this any firmware fault would vector to physical 0 (no handler)
 * and, under the bare-loader ic=0/iva=0 rule, storm.  With it, a fatal fault
 * instead freezes at IVT_BASE + vector with all GRs, the RSE frame, ISR and
 * IIPA preserved - the fault class is the offset from IVT_BASE, and the
 * interrupted state is inspectable via the monitor.  See
 * plans/phase5-real-firmware-boot.md.
 */
#define IA64_REALFW_IVT_BASE      IA64_U64(0x00000000ff300000)
#define IA64_REALFW_IVT_SIZE      0x8000
/*
 * 460GX chipset CSR scratch below the IOAPIC window.  SAL_B's first act
 * after PAL_PROC_GET_FEATURES is a BSP-arbitration handshake here: clear
 * bit 7 at +0xCB0, poll +0xCC0 until bit 7 sets, then compare the low
 * 7 bits with LID.id (bios130.BIN @ 0xffe76700..0xffe76790).  Model the
 * grant register as always-granted-to-id-0; everything else in the page
 * is write-store/read-back scratch, logged for the stage-1 inventory.
 */
#define IA64_REALFW_SAC_BASE      IA64_U64(0x00000000feb00000)
#define IA64_REALFW_SAC_SIZE      0x10000
#define IA64_REALFW_SAC_BOOT_SEM  0xcc0
#define IA64_REALFW_PTR_FIT       (IA64_REALFW_WINDOW_END - 32)
#define IA64_REALFW_PTR_SALE      (IA64_REALFW_WINDOW_END - 24)
/* Bit 63 in firmware pointers is the uncacheable-attribute flag, not
 * part of the physical address (SAL sec 2.5). */
#define IA64_REALFW_PTR_ADDR_MASK (~(IA64_U64(1) << 63))
#define IA64_HIGH_RAM_AFTER_FIRMWARE_BASE IA64_FW_ADDRESS_SPACE_END
#define IA64_AHCI_IDP_IO_BASE   0x0000c100U
#define IA64_UHCI_IO_BASE       0x0000c120U
/* LSI BAR0 is 0x100 bytes and therefore requires 0x100-byte alignment. */
/*
 * The chipset routes I/O in 4 KiB segments, one or more per logical PCI bus
 * (SSDM 4; plans/sdv-i2000-firmware-reference.md 8.3), so a device behind an
 * expander root takes a port range out of a segment that belongs to that
 * root rather than a hole punched in the compatibility bus's.  Segment B is
 * the first WXB root's, D the AGP root's and E the second WXB root's; the
 * compatibility bus keeps the rest, including the legacy ports.
 */
/*
 * The SCSI seat's ports come out of segment B, the first WXB root's; the
 * second adapter parks on the second WXB root and takes segment E.
 */
#define IA64_SCSI_SEAT_IO_BASE  0x0000b000U
#define IA64_SCSI_PARK_IO_BASE  0x0000e000U
#define IA64_VGA_IO_BASE        0x0000d000U
/*
 * The vendor ATI Rage 128 vgabios hardcodes its register I/O base at 0xD800 and
 * only falls back to a port-space scan if a signature probe there fails, so in
 * realfw mode the card's I/O BAR must live at 0xD800 for the BIOS's register
 * accesses (MM_INDEX/DATA, the PLL file) to reach the device.  Guests read the
 * BAR from config space, so they use the layout-fixed IA64_VGA_IO_BASE.
 */
#define IA64_VGA_IO_BASE_REALFW 0x0000d800U
#define IA64_E1000_IO_BASE      0x0000c400U
#define IA64_OHCI_MMIO_PCI_BASE (IA64_PCI_MMIO_BASE + 0x00010000ULL)
#define IA64_AHCI_MMIO_PCI_BASE (IA64_PCI_MMIO_BASE + 0x00020000ULL)
/*
 * Devices behind an expander root must have their BARs inside that root's
 * own producer window, or the guest's PnP resource arbiter cannot assign
 * them: a boot controller that fails this bugchecks the guest with STOP
 * 0x7B before it ever reaches the disk.
 *
 * The 460GX decodes one n x 32 MB aperture per logical PCI bus out of the
 * gap below 4 GiB - 32 MiB (SSDM 4; plans/sdv-i2000-firmware-reference.md
 * 7.1), so each root owns a whole number of those units and nothing is
 * carved out of another root's range: the compatibility bus takes the unit
 * at the bottom of the gap, graphics takes the five units its framebuffer
 * and register apertures need, and the two WXB roots take one unit each at
 * the top.  The DSDT windows in roms/ia64-firmware/dsdt-pci-root.asl mirror
 * the split exactly.
 */
#define IA64_PCI_MMIO_UNIT      0x02000000ULL
#define IA64_WXB0_MMIO_PCI_BASE (IA64_PCI_MMIO_BASE + 6 * IA64_PCI_MMIO_UNIT)
#define IA64_WXB1_MMIO_PCI_BASE (IA64_PCI_MMIO_BASE + 7 * IA64_PCI_MMIO_UNIT)
/*
 * The memory BARs of whichever adapter holds the SCSI seat come out of the
 * first WXB root's aperture, and the parked adapter's out of the second's.
 * The LSI's script RAM BAR sits 8 KiB above its register BAR either way.
 */
#define IA64_SCSI_SEAT_MMIO_PCI_BASE IA64_WXB0_MMIO_PCI_BASE
#define IA64_SCSI_PARK_MMIO_PCI_BASE IA64_WXB1_MMIO_PCI_BASE
#define IA64_LSI_RAM_BAR_OFFSET      0x00002000ULL
#define IA64_E1000_MMIO_PCI_BASE (IA64_PCI_MMIO_BASE + 0x00040000ULL)
#define IA64_E1000_MMIO_SIZE    0x00020000ULL
#define IA64_E1000_IO_SIZE      0x00000040U
/*
 * CS4281 BA0 is 4 KiB and BA1 is 64 KiB.  They sit above the NIC slices
 * (IA64_E1000_MMIO_PCI_BASE plus MAX_NICS * IA64_NIC_MMIO_STRIDE, which
 * includes each adapter's Flash aperture) and below the graphics
 * framebuffer at IA64_PCI_MMIO_BASE + 0x02000000.
 */
/*
 * The south bridge's IDE bus-master register file.  Both channels are in
 * compatibility mode and decode the fixed legacy ports, so only this BAR
 * needs an address; it is the one the firmware allocates for a controller
 * that arrives unassigned (PCI_IDE_BMDMA_BAR), kept identical so guest and
 * firmware agree.
 */
#define IA64_IFB_IDE_BMDMA_IO_BASE 0x0000c000U
/*
 * The south bridge's SMBus host controller.  The real SDV firmware programs
 * this BAR to 0xFFF0 and drives the board's sensor chips through it
 * (plans/phase5 SESSION 8), so use the same base here.
 */
#define IA64_IFB_SMBUS_IO_BASE   0x0000fff0U
#define IA64_CS4281_BA0_PCI_BASE (IA64_PCI_MMIO_BASE + 0x01800000ULL)
#define IA64_CS4281_BA1_PCI_BASE (IA64_PCI_MMIO_BASE + 0x01810000ULL)
/*
 * Per-adapter slice of the NIC memory / I/O windows.  Sized to hold the
 * largest BAR set of any supported model: the Intel PRO/100 needs a 1 MiB
 * flash BAR on top of its CSR/I/O BARs, so reserve 2 MiB of memory (and a
 * generous I/O slice) per adapter.  MAX_NICS slices stay well inside the
 * PCI0 _CRS windows the firmware advertises.
 */
#define IA64_NIC_MMIO_STRIDE    0x00200000ULL
#define IA64_NIC_IO_STRIDE      0x00000100U
#define IA64_VGA_FB_PCI_BASE    (IA64_PCI_MMIO_BASE + 0x02000000ULL)
#define IA64_VGA_MMIO_PCI_BASE  (IA64_PCI_MMIO_BASE + 0x07000000ULL)
#define IA64_VGA_ROM_PCI_BASE   (IA64_PCI_MMIO_BASE + 0x08000000ULL)
/*
 * The NVIDIA NV15GL (vga=nv15gl) uses a different, larger BAR layout than the
 * ATI adapters: BAR0 is a 16 MiB MMIO register aperture and BAR1 is a 128 MiB
 * prefetchable framebuffer aperture.  Its 128 MiB FB does not fit the ATI
 * fixed-window spacing, so it gets its own naturally aligned bases inside the
 * PCI0 MMIO window [0xEE000000, 0xFE000000): FB at 0xF0000000 (128 MiB,
 * shared with the firmware's fixed FB address), MMIO at 0xF8000000 (16 MiB),
 * expansion ROM at 0xF9000000.
 */
#define IA64_NV_FB_PCI_BASE     (IA64_PCI_MMIO_BASE + 0x02000000ULL)
#define IA64_NV_MMIO_PCI_BASE   (IA64_PCI_MMIO_BASE + 0x0A000000ULL)
#define IA64_NV_ROM_PCI_BASE    (IA64_PCI_MMIO_BASE + 0x0B000000ULL)
#define IA64_NV_VENDOR_ID       0x10deU
#define IA64_VGA_LEGACY_BASE   0x000a0000U
#define IA64_VGA_LEGACY_SIZE   0x00020000U
#ifdef CONFIG_IA64_VPC_GRAPHICS
#define IA64_INT10_ROM_BASE     0x000c0000U
/*
 * At least 2 KB: the XP inbox Rage 128 miniport validates the option ROM's
 * size byte and rejects images smaller than 4 x 512 bytes
 * (.GetVgaEnabledRomImage compares size_byte << 9 against 2048 and logs
 * event 0xC1010002 UniqueId 26 on failure).
 */
#define IA64_INT10_ROM_SIZE     0x00000800U
/*
 * PCIR sits above the ATI data blocks.  A real Rage 128 Pro BIOS keeps it
 * at 16Ch, well clear of both the ATI ROM signature at 30h and the legacy
 * ATI BIOS pointer at 48h (verified against three retail Rage 128 Pro
 * dumps).  At 20h its 18h-byte data structure would straddle 30h.
 */
#define IA64_INT10_ROM_PCIR_OFFSET    0x00e0U
#define IA64_INT10_ROM_ATI_SIG_OFFSET 0x0030U
#define IA64_INT10_ROM_ATI_HEADER_OFFSET 0x0080U
#define IA64_INT10_ROM_ATI_PLL_OFFSET 0x00c0U
#define IA64_INT10_ROM_HANDLER_OFFSET 0x0100U
#define IA64_INT10_ROM_OEM_OFFSET     0x0180U
#define IA64_INT10_ROM_VENDOR_OFFSET  0x0190U
#define IA64_INT10_ROM_PRODUCT_OFFSET 0x01a0U
#define IA64_INT10_ROM_REVISION_OFFSET 0x01c0U
#define IA64_INT10_ROM_MODES_OFFSET   0x01d0U
#define IA64_INT10_VECTOR_ADDR  (0x10U * 4U)
#define IA64_INT10_IO_BASE      0x000001e0U
#define IA64_INT10_IO_SIZE      0x00000010U
#define IA64_INT10_TRIGGER      0x4941U
#define IA64_VBE2_SIGNATURE     0x32454256U
#define IA64_VBE_IO_INDEX       0x01ceU
#define IA64_VBE_IO_DATA        0x01d0U
#define IA64_VGA_PLANAR_MEMORY_SIZE (256 * KiB)
#define IA64_BDA_VIDEO_MODE      0x00000449U
#define IA64_BDA_VIDEO_COLUMNS   0x0000044aU
#define IA64_BDA_VIDEO_PAGE_SIZE 0x0000044cU
#define IA64_BDA_VIDEO_PAGE_START 0x0000044eU
#define IA64_BDA_CURSOR_POSITIONS 0x00000450U
#define IA64_BDA_CURSOR_TYPE     0x00000460U
#define IA64_BDA_VIDEO_PAGE      0x00000462U
#define IA64_BDA_CRTC_ADDRESS    0x00000463U
#define IA64_BDA_VIDEO_ROWS      0x00000484U
#define IA64_BDA_CHARACTER_HEIGHT 0x00000485U
#define IA64_BDA_VIDEO_CONTROL   0x00000487U
#define IA64_BDA_VIDEO_SWITCHES  0x00000488U
#define IA64_ATI_VENDOR_ID        0x1002U
#define IA64_ATI_RAGE128_PF_ID    0x5046U
#define IA64_ATI_PLL_XCLK         12000U
#define IA64_ATI_PLL_REFERENCE_FREQ 2950U
#define IA64_ATI_PLL_REFERENCE_DIV  65U
#define IA64_ATI_PLL_MIN_FREQ     12500U
#define IA64_ATI_PLL_MAX_FREQ     40000U
#endif
#define IA64_LEGACY_COM1_IO_BASE 0x000003f8U
#define IA64_LEGACY_COM1_IO_SIZE 0x00000008U
#define IA64_PIB_IPI_LIMIT          0x00100000ULL
#define IA64_PIB_INTA_OFFSET        0x001e0000ULL
#define IA64_PIB_XTP_OFFSET         0x001e0008ULL
/* Graphics (Rage 128) lands here: slots 0-4 are reserved/built-in, VGA next. */
#define IA64_VPC_VGA_SLOT           5
/*
 * The i2000 carries its 82559 Ethernet at 00:05.0, the slot the graphics
 * adapter used to occupy before it moved to the GXB root.  zx1 keeps the
 * adapter where it was.
 */
#define IA64_VPC_NIC_SLOT           6
#define IA64_460GX_NIC_SLOT         5

#define IA64_SAPIC_DELIVERY_INT     0
#define IA64_SAPIC_DELIVERY_NMI     4
#define IA64_SAPIC_DELIVERY_EXTINT  7

#ifdef CONFIG_IA64_VPC_GRAPHICS
enum {
    IA64_INT10_REG_AX,
    IA64_INT10_REG_BX,
    IA64_INT10_REG_CX,
    IA64_INT10_REG_DX,
    IA64_INT10_REG_DI,
    IA64_INT10_REG_ES,
    IA64_INT10_REG_EXEC,
    IA64_INT10_REG_DATA,
};

typedef struct IA64Int10Registers {
    uint16_t ax;
    uint16_t bx;
    uint16_t cx;
    uint16_t dx;
    uint16_t di;
    uint16_t es;
} IA64Int10Registers;

typedef struct IA64VbeMode {
    uint16_t number;
    uint16_t width;
    uint16_t height;
    uint8_t bpp;
} IA64VbeMode;

typedef struct IA64VgaLegacyMode {
    uint8_t number;
    uint8_t columns;
    uint8_t rows;
    uint8_t character_height;
    uint16_t page_size;
    uint8_t misc;
    const uint8_t *sequencer;
    const uint8_t *crtc;
    const uint8_t *attribute;
    const uint8_t *graphics;
} IA64VgaLegacyMode;

static const IA64VbeMode ia64_vbe_modes[] = {
    { 0x111,  640,  480, 16 },
    { 0x112,  640,  480, 24 },
    { 0x114,  800,  600, 16 },
    { 0x115,  800,  600, 24 },
    { 0x117, 1024,  768, 16 },
    { 0x118, 1024,  768, 24 },
    { 0x11a, 1280, 1024, 16 },
    { 0x11b, 1280, 1024, 24 },
    { 0x141,  640,  400, 32 },
    { 0x142,  640,  480, 32 },
    { 0x143,  800,  600, 32 },
    { 0x144, 1024,  768, 32 },
    { 0x145, 1280, 1024, 32 },
};

/* Standard VGA BIOS mode 12h: 640x480, 16-color planar graphics. */
static const uint8_t ia64_vga_mode_12_sequencer[] = {
    0x01, 0x0f, 0x00, 0x06,
};

static const uint8_t ia64_vga_mode_12_crtc[] = {
    0x5f, 0x4f, 0x50, 0x82, 0x54, 0x80, 0x0b, 0x3e,
    0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xea, 0x8c, 0xdf, 0x28, 0x00, 0xe7, 0x04, 0xe3,
    0xff,
};

static const uint8_t ia64_vga_mode_12_attribute[] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x14, 0x07,
    0x38, 0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f,
    0x01, 0x00, 0x0f, 0x00, 0x00,
};

static const uint8_t ia64_vga_mode_12_graphics[] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05, 0x0f,
    0xff,
};

static const IA64VgaLegacyMode ia64_vga_legacy_modes[] = {
    {
        .number = 0x12,
        .columns = 80,
        .rows = 30,
        .character_height = 16,
        .page_size = 0xa000,
        .misc = 0xe3,
        .sequencer = ia64_vga_mode_12_sequencer,
        .crtc = ia64_vga_mode_12_crtc,
        .attribute = ia64_vga_mode_12_attribute,
        .graphics = ia64_vga_mode_12_graphics,
    },
};

static const char ia64_vbe_oem[] = "QEMU IA64 VBE";
static const char ia64_vbe_vendor[] = "QEMU";
static const char ia64_vbe_product[] = "IA64 VGA VBE bridge";
static const char ia64_vbe_revision[] = "1.0";

/*
 * The real-mode INT 10h entry marshals the registers through the private
 * I/O window above.  Keeping the executable stub small is intentional: the
 * VBE implementation remains normal, testable C code, and the stub also
 * works when the guest uses a software x86 BIOS emulator instead of native
 * IA-32 execution.  The bytes below are 16-bit code equivalent to:
 *
 *     push bp                 ; save registers not returned by VBE
 *     mov  bp, sp
 *     push ax
 *     push dx
 *     mov  dx, 1e0h
 *     out  dx, ax
 *     add  dx, 2
 *     mov  ax, bx
 *     out  dx, ax
 *     add  dx, 2
 *     mov  ax, cx
 *     out  dx, ax
 *     add  dx, 2
 *     mov  ax, [bp-4]
 *     out  dx, ax
 *     add  dx, 2
 *     mov  ax, di
 *     out  dx, ax
 *     add  dx, 2
 *     mov  ax, es
 *     out  dx, ax
 *     add  dx, 2
 *     cmp  word [bp-2], 4f00h
 *     jne  execute
 *     add  dx, 2
 *     mov  ax, es:[di]
 *     out  dx, ax
 *     mov  ax, es:[di+2]
 *     out  dx, ax             ; pass the VBE2 input signature
 *     sub  dx, 2
 * execute:
 *     mov  ax, 4941h
 *     out  dx, ax             ; execute the request at 1ech
 *     in   ax, dx
 *     mov  cx, ax
 *     jcxz response_done
 *     push di                 ; deliver the response to the RESULT es:di.  For
 *     mov  dx, 1e8h           ; VBE that is the request es:di (unchanged); the
 *     in   ax, dx             ; ATI BIOS query sets it to the caller's dx:bx
 *     mov  di, ax             ; buffer instead.
 *     mov  dx, 1eah
 *     in   ax, dx
 *     mov  es, ax
 *     mov  dx, 1eeh
 *     cld
 * response_loop:
 *     in   ax, dx
 *     stosw
 *     loop response_loop
 *     pop  di
 * response_done:
 *     mov  dx, 1e0h
 *     in   ax, dx
 *     mov  [bp-2], ax
 *     add  dx, 2
 *     in   ax, dx
 *     mov  bx, ax
 *     add  dx, 2
 *     in   ax, dx
 *     mov  cx, ax
 *     add  dx, 2
 *     in   ax, dx
 *     mov  dx, ax
 *     mov  ax, [bp-2]
 *     mov  sp, bp
 *     pop  bp
 *     iret
 */
static const uint8_t ia64_int10_handler[] = {
    0x55, 0x89, 0xe5, 0x50, 0x52, 0xba, 0xe0, 0x01,
    0xef, 0x83, 0xc2, 0x02, 0x89, 0xd8, 0xef, 0x83,
    0xc2, 0x02, 0x89, 0xc8, 0xef, 0x83, 0xc2, 0x02,
    0x8b, 0x46, 0xfc, 0xef, 0x83, 0xc2, 0x02, 0x89,
    0xf8, 0xef, 0x83, 0xc2, 0x02, 0x8c, 0xc0, 0xef,
    0x83, 0xc2, 0x02, 0x81, 0x7e, 0xfe, 0x00, 0x4f,
    0x75, 0x0f, 0x83, 0xc2, 0x02, 0x26, 0x8b, 0x05,
    0xef, 0x26, 0x8b, 0x45, 0x02, 0xef, 0x83, 0xea,
    0x02, 0xb8, 0x41, 0x49, 0xef, 0xed, 0x89, 0xc1,
    0xe3, 0x16, 0x57, 0xba, 0xe8, 0x01, 0xed, 0x89,
    0xc7, 0xba, 0xea, 0x01, 0xed, 0x8e, 0xc0, 0xba,
    0xee, 0x01, 0xfc, 0xed, 0xab, 0xe2, 0xfc, 0x5f,
    0xba, 0xe0, 0x01, 0xed, 0x89, 0x46, 0xfe, 0x83,
    0xc2, 0x02, 0xed, 0x89, 0xc3, 0x83, 0xc2, 0x02,
    0xed, 0x89, 0xc1, 0x83, 0xc2, 0x02, 0xed, 0x89,
    0xc2, 0x8b, 0x46, 0xfe, 0x89, 0xec, 0x5d, 0xcf,
};

/* Option-ROM initialization entry: install C000:0100 as vector 10h. */
static const uint8_t ia64_int10_rom_init[] = {
    0x50, 0x1e, 0x31, 0xc0, 0x8e, 0xd8, 0xc7, 0x06,
    0x40, 0x00, 0x00, 0x01, 0xc7, 0x06, 0x42, 0x00,
    0x00, 0xc0, 0x1f, 0x58, 0xcb,
};
#endif

/*
 * The IA-64 machine is modeled as an abstract base ("ia64-base") carrying all
 * the shared platform (PCI host, IOSAPIC, firmware, devices), with two concrete
 * machine types built on it: "460gx" (Intel SDV / HP i2000, Merced + 460GX) and
 * "zx1" (HP rx2600 / zx2000 / zx6000, Itanium 2 + zx1 SBA).  Each concrete class
 * fixes its default CPU and its chipset personality via ChipsetProfile below;
 * "ia64-vpc" survives as a deprecated alias of zx1.
 */
#define TYPE_IA64_VPC_MACHINE MACHINE_TYPE_NAME("ia64-base")
OBJECT_DECLARE_TYPE(IA64VpcMachineState, IA64VpcMachineClass, IA64_VPC_MACHINE)

#define TYPE_IA64_460GX_MACHINE MACHINE_TYPE_NAME("460gx")
#define TYPE_IA64_ZX1_MACHINE   MACHINE_TYPE_NAME("zx1")

struct IA64VpcMachineClass {
    MachineClass parent_class;
    /* The chipset personality this machine type fixes: IA64_FW_CHIPSET_*. */
    uint64_t chipset_profile;
};

struct IA64VpcMachineState {
    MachineState parent_obj;

    bool i8042_enabled;
    bool ahci_enabled;
    bool audio_enabled;
    bool isp_enabled;
    bool lsi_enabled;
    bool fw_relocate;
    uint64_t fw_map_quirk_disable;
    bool ide_enabled;
    bool firmware_ide_dma;
    bool agp_enabled;
    uint64_t firmware_console;
    uint16_t firmware_boot_timeout;
    char *nvram_path;
    char *realfw_path;
    char *realfw_vga_rom_path;
    char *realfw_nvram_path;
    uint64_t realfw_entry;
    uint64_t realfw_base;
    PFlashCFI01 *realfw_flash;
    MemoryRegion realfw_post_io;
    MemoryRegion realfw_sac_mmio;
    MemoryRegion realfw_cfg_io;
    MemoryRegion realfw_ide_data[2];
    MemoryRegion realfw_ide_cmd[2];
    MemoryRegion realfw_rtc_ext_alias;
    MemoryRegion realfw_smbus_io;
    MemoryRegion realfw_port61_io;
    qemu_irq extint;
    uint8_t *realfw_sac_data;
    uint16_t realfw_post_last;
    uint32_t realfw_config_address;
    /*
     * Persistent CPU-frequency mailbox (south bridge 00:03.0 reg 0xd0), NOT
     * cleared by ia64_vpc_reset so the firmware's one-time "New CPU frequency
     * is set" write survives its CF9 reboot.  See ia64_realfw_cfg_read.
     */
    uint32_t realfw_freq_mailbox;
    /* 460GX chipset config space: bus CBN devices, 8 fns x 256 bytes. */
    uint8_t *realfw_chipset_cfg;
    PCIBus *realfw_pci_bus;
    char *vga_model;
    bool alat_full;

    PCIDevice *agp_dev;
    PCIDevice *sba_dev;
    DeviceState *lba_dev;
    DeviceState *mercury_host;      /* zx1: the Mercury (LBA) PCI host bridge */
    /* 460gx: the WXB0, WXB1 and GXB expander roots (buses 1, 2 and 3). */
    DeviceState *expander_host[IA64_460GX_EXPANDER_ROOTS];
    PCIBus *expander_bus[IA64_460GX_EXPANDER_ROOTS];
    PCIBus *mercury_bus;            /* zx1: the Mercury second root bus         */
    PCIDevice *ahci_dev;
    PCIDevice *audio_dev;
    PCIDevice *isp_dev;
    PCIDevice *ide_dev;
    PCIDevice *ohci_dev;
    PCIDevice *uhci_dev;
    Intel82468GXIFBState *ifb;
    PCIDevice *lsi_dev;
    PCIDevice *vga_dev;
    PCIDevice *nic_devs[MAX_NICS];
    unsigned int nic_count;

    MemoryRegion *ram_aliases[4];
    unsigned int ram_alias_count;
    MemoryRegion *vga_fb_alias;
    MemoryRegion *vga_mmio_alias;
    MemoryRegion *vga_legacy_alias;
    MemoryRegion *lsapic_mmio;
    MemoryRegion firmware_space;
    MemoryRegion watchdog_mmio;
    MemoryRegion nvram_mmio;
    MemoryRegion acpi_pm;
    MemoryRegion acpi_reset;
    MemoryRegion debug_uart_legacy_io;
    SerialMM *debug_uart;
#ifdef CONFIG_IA64_VPC_GRAPHICS
    MemoryRegion int10_pci_io;
    IA64Int10Registers int10_request;
    IA64Int10Registers int10_result;
    uint32_t int10_input_signature;
    uint8_t int10_response[512];
    uint16_t int10_response_length;
    uint16_t int10_response_offset;
    uint8_t int10_input_signature_words;
    uint8_t int10_dpms_state;
    uint8_t int10_legacy_mode;
    uint8_t int10_legacy_columns;
#endif

    Object *pci_fixup_reset;
    QEMUTimer *watchdog_timer;
    uint64_t watchdog_timeout;
    uint64_t watchdog_code;
    uint8_t nvram_data[IA64_NVRAM_SIZE];
    size_t firmware_size;
    char *nvram_resolved_path;
    bool nvram_write_warning;
    ACPIREGS acpi_regs;
    qemu_irq acpi_sci_irq;
    qemu_irq isa_irqs[ISA_NUM_IRQS];
    Notifier powerdown_notifier;
    Notifier done_notifier;
    bool vmstate_registered;
};

static uint64_t ia64_vpc_fw_base(IA64VpcMachineState *s, uint64_t ram_size)
{
    return s->fw_relocate ? IA64_FW_IMAGE_BASE_FOR(ram_size)
                          : IA64_FW_LINK_BASE;
}


#ifdef CONFIG_IA64_VPC_GRAPHICS
static const IA64VbeMode *ia64_vbe_find_mode(uint16_t number)
{
    size_t i;

    for (i = 0; i < G_N_ELEMENTS(ia64_vbe_modes); i++) {
        if (ia64_vbe_modes[i].number == number) {
            return &ia64_vbe_modes[i];
        }
    }
    return NULL;
}

static const IA64VgaLegacyMode *ia64_vga_find_legacy_mode(uint8_t number)
{
    size_t i;

    for (i = 0; i < G_N_ELEMENTS(ia64_vga_legacy_modes); i++) {
        if (ia64_vga_legacy_modes[i].number == number) {
            return &ia64_vga_legacy_modes[i];
        }
    }
    return NULL;
}

static void ia64_vbe_write(uint16_t index, uint16_t value)
{
    address_space_stw_le(&address_space_memory,
                         IA64_PCI_IO_BASE + IA64_VBE_IO_INDEX,
                         index, MEMTXATTRS_UNSPECIFIED, NULL);
    address_space_stw_le(&address_space_memory,
                         IA64_PCI_IO_BASE + IA64_VBE_IO_DATA,
                         value, MEMTXATTRS_UNSPECIFIED, NULL);
}

static uint16_t ia64_vbe_read(uint16_t index)
{
    address_space_stw_le(&address_space_memory,
                         IA64_PCI_IO_BASE + IA64_VBE_IO_INDEX,
                         index, MEMTXATTRS_UNSPECIFIED, NULL);
    return address_space_lduw_le(&address_space_memory,
                                 IA64_PCI_IO_BASE + IA64_VBE_IO_DATA,
                                 MEMTXATTRS_UNSPECIFIED, NULL);
}

static uint32_t ia64_vbe_memory_size(void)
{
    return (uint32_t)ia64_vbe_read(VBE_DISPI_INDEX_VIDEO_MEMORY_64K) *
           (64 * KiB);
}

static void ia64_vga_writeb(uint16_t port, uint8_t value)
{
    address_space_stb(&address_space_memory, IA64_PCI_IO_BASE + port,
                      value, MEMTXATTRS_UNSPECIFIED, NULL);
}

static uint8_t ia64_vga_readb(uint16_t port)
{
    return address_space_ldub(&address_space_memory,
                              IA64_PCI_IO_BASE + port,
                              MEMTXATTRS_UNSPECIFIED, NULL);
}

static void ia64_vga_indexed_write(uint16_t index_port,
                                   uint16_t data_port,
                                   uint8_t index, uint8_t value)
{
    ia64_vga_writeb(index_port, index);
    ia64_vga_writeb(data_port, value);
}

static void ia64_int10_update_legacy_bda(const IA64VgaLegacyMode *mode,
                                         bool no_clear)
{
    address_space_stb(&address_space_memory, IA64_BDA_VIDEO_MODE,
                      mode->number, MEMTXATTRS_UNSPECIFIED, NULL);
    address_space_stw_le(&address_space_memory, IA64_BDA_VIDEO_COLUMNS,
                         mode->columns, MEMTXATTRS_UNSPECIFIED, NULL);
    address_space_stw_le(&address_space_memory, IA64_BDA_VIDEO_PAGE_SIZE,
                         mode->page_size, MEMTXATTRS_UNSPECIFIED, NULL);
    address_space_stw_le(&address_space_memory, IA64_BDA_VIDEO_PAGE_START,
                         0, MEMTXATTRS_UNSPECIFIED, NULL);
    address_space_set(&address_space_memory, IA64_BDA_CURSOR_POSITIONS,
                      0, 16, MEMTXATTRS_UNSPECIFIED);
    address_space_stw_le(&address_space_memory, IA64_BDA_CURSOR_TYPE,
                         0, MEMTXATTRS_UNSPECIFIED, NULL);
    address_space_stb(&address_space_memory, IA64_BDA_VIDEO_PAGE,
                      0, MEMTXATTRS_UNSPECIFIED, NULL);
    address_space_stw_le(&address_space_memory, IA64_BDA_CRTC_ADDRESS,
                         VGA_CRT_IC, MEMTXATTRS_UNSPECIFIED, NULL);
    address_space_stb(&address_space_memory, IA64_BDA_VIDEO_ROWS,
                      mode->rows - 1, MEMTXATTRS_UNSPECIFIED, NULL);
    address_space_stw_le(&address_space_memory, IA64_BDA_CHARACTER_HEIGHT,
                         mode->character_height,
                         MEMTXATTRS_UNSPECIFIED, NULL);
    address_space_stb(&address_space_memory, IA64_BDA_VIDEO_CONTROL,
                      0x60 | (no_clear ? 0x80 : 0),
                      MEMTXATTRS_UNSPECIFIED, NULL);
    address_space_stb(&address_space_memory, IA64_BDA_VIDEO_SWITCHES,
                      0xf9, MEMTXATTRS_UNSPECIFIED, NULL);
}

static void ia64_vga_load_ega_palette(void)
{
    unsigned int color;

    ia64_vga_writeb(VGA_PEL_MSK, 0xff);
    ia64_vga_writeb(VGA_PEL_IW, 0);
    for (color = 0; color < 64; color++) {
        uint8_t red = (color & 0x04 ? 0x2a : 0) |
                      (color & 0x20 ? 0x15 : 0);
        uint8_t green = (color & 0x02 ? 0x2a : 0) |
                        (color & 0x10 ? 0x15 : 0);
        uint8_t blue = (color & 0x01 ? 0x2a : 0) |
                       (color & 0x08 ? 0x15 : 0);

        ia64_vga_writeb(VGA_PEL_D, red);
        ia64_vga_writeb(VGA_PEL_D, green);
        ia64_vga_writeb(VGA_PEL_D, blue);
    }
}

static void ia64_int10_program_legacy_mode(IA64VpcMachineState *s,
                                            const IA64VgaLegacyMode *mode,
                                            bool no_clear)
{
    size_t i;

    /*
     * A legacy VGA caller uses the planar A0000h aperture.  Disable the
     * synthetic VBE layout before programming standard VGA registers so a
     * previous packed-pixel framebuffer cannot reinterpret those writes.
     */
    ia64_vbe_write(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_DISABLED);
    ia64_vga_indexed_write(VGA_SEQ_I, VGA_SEQ_D, VGA_SEQ_RESET, 0x01);
    for (i = 0; i < VGA_SEQ_C - 1; i++) {
        ia64_vga_indexed_write(VGA_SEQ_I, VGA_SEQ_D, i + 1,
                               mode->sequencer[i]);
    }
    ia64_vga_writeb(VGA_MIS_W, mode->misc);
    ia64_vga_indexed_write(VGA_GFX_I, VGA_GFX_D, VGA_GFX_MISC,
                           mode->graphics[VGA_GFX_MISC]);
    ia64_vga_indexed_write(VGA_SEQ_I, VGA_SEQ_D, VGA_SEQ_RESET, 0x03);
    for (i = 0; i < VGA_GFX_C; i++) {
        ia64_vga_indexed_write(VGA_GFX_I, VGA_GFX_D, i,
                               mode->graphics[i]);
    }

    ia64_vga_indexed_write(VGA_CRT_IC, VGA_CRT_DC,
                           VGA_CRTC_V_SYNC_END, 0);
    for (i = 0; i < VGA_CRT_C; i++) {
        ia64_vga_indexed_write(VGA_CRT_IC, VGA_CRT_DC, i,
                               mode->crtc[i]);
    }
    for (i = 0; i < VGA_ATT_C; i++) {
        (void)ia64_vga_readb(VGA_IS1_RC);
        ia64_vga_writeb(VGA_ATT_W, i);
        ia64_vga_writeb(VGA_ATT_W, mode->attribute[i]);
    }
    ia64_vga_load_ega_palette();

    if (!no_clear) {
        address_space_set(&address_space_memory, IA64_VGA_FB_PCI_BASE,
                          0, IA64_VGA_PLANAR_MEMORY_SIZE,
                          MEMTXATTRS_UNSPECIFIED);
    }
    (void)ia64_vga_readb(VGA_IS1_RC);
    ia64_vga_writeb(VGA_ATT_W, VGA_AR_ENABLE_DISPLAY);

    s->int10_legacy_mode = mode->number;
    s->int10_legacy_columns = mode->columns;
    ia64_int10_update_legacy_bda(mode, no_clear);
}

static bool ia64_int10_set_legacy_mode(IA64VpcMachineState *s,
                                       uint8_t request)
{
    uint8_t number = request & 0x7f;
    bool no_clear = request & 0x80;
    const IA64VgaLegacyMode *mode;

    if (number == 3) {
        ia64_vbe_write(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_DISABLED);
        s->int10_legacy_mode = number;
        s->int10_legacy_columns = 80;
        return true;
    }

    mode = ia64_vga_find_legacy_mode(number);
    if (mode == NULL) {
        return false;
    }
    ia64_int10_program_legacy_mode(s, mode, no_clear);
    return true;
}

static uint32_t ia64_int10_rom_pointer(uint16_t offset)
{
    return ((IA64_INT10_ROM_BASE >> 4) << 16) | offset;
}

static void ia64_int10_response_clear(IA64VpcMachineState *s)
{
    memset(s->int10_response, 0, sizeof(s->int10_response));
    s->int10_response_length = 0;
    s->int10_response_offset = 0;
}

static void ia64_int10_response_size(IA64VpcMachineState *s, size_t size)
{
    g_assert(size <= sizeof(s->int10_response));
    g_assert((size & 1) == 0);
    memset(s->int10_response, 0, size);
    s->int10_response_length = size;
    s->int10_response_offset = 0;
}

static void ia64_int10_vbe_success(IA64VpcMachineState *s)
{
    s->int10_result.ax = 0x004f;
}

static void ia64_int10_vbe_failure(IA64VpcMachineState *s)
{
    s->int10_result.ax = 0x014f;
}

static void ia64_int10_vbe_unsupported(IA64VpcMachineState *s)
{
    s->int10_result.ax = 0x024f;
}

static void ia64_int10_controller_info(IA64VpcMachineState *s)
{
    size_t response_size;
    uint8_t *info;

    response_size = s->int10_input_signature == IA64_VBE2_SIGNATURE ?
                    512 : 256;
    ia64_int10_response_size(s, response_size);
    info = s->int10_response;
    memcpy(info, "VESA", 4);
    stw_le_p(info + 4, 0x0300);
    stl_le_p(info + 6,
             ia64_int10_rom_pointer(IA64_INT10_ROM_OEM_OFFSET));
    stl_le_p(info + 10, 0);
    stl_le_p(info + 14,
             ia64_int10_rom_pointer(IA64_INT10_ROM_MODES_OFFSET));
    stw_le_p(info + 18,
             ia64_vbe_read(VBE_DISPI_INDEX_VIDEO_MEMORY_64K));
    stw_le_p(info + 20, 0x0100);
    stl_le_p(info + 22,
             ia64_int10_rom_pointer(IA64_INT10_ROM_VENDOR_OFFSET));
    stl_le_p(info + 26,
             ia64_int10_rom_pointer(IA64_INT10_ROM_PRODUCT_OFFSET));
    stl_le_p(info + 30,
             ia64_int10_rom_pointer(IA64_INT10_ROM_REVISION_OFFSET));
    ia64_int10_vbe_success(s);
}

static void ia64_int10_mode_info(IA64VpcMachineState *s)
{
    const IA64VbeMode *mode =
        ia64_vbe_find_mode(s->int10_request.cx & 0x01ff);
    uint32_t pitch;
    uint32_t image_size;
    uint32_t memory_size;
    uint32_t pages;
    uint8_t red_size;
    uint8_t green_size;
    uint8_t alpha_size;
    uint8_t alpha_pos;
    uint8_t *info;

    if (mode == NULL) {
        ia64_int10_vbe_failure(s);
        return;
    }

    ia64_int10_response_size(s, 256);
    info = s->int10_response;
    pitch = mode->width * DIV_ROUND_UP(mode->bpp, 8);
    image_size = pitch * mode->height;
    memory_size = ia64_vbe_memory_size();
    if (image_size > memory_size) {
        ia64_int10_response_clear(s);
        ia64_int10_vbe_failure(s);
        return;
    }
    pages = memory_size /
            ((image_size + 64 * KiB - 1) & ~((64 * KiB) - 1));
    pages = CLAMP(pages, 1, 256) - 1;

    stw_le_p(info + 0, 0x00bb);
    info[2] = 0x07;
    info[3] = 0;
    stw_le_p(info + 4, 64);
    stw_le_p(info + 6, 64);
    stw_le_p(info + 8, 0xa000);
    stw_le_p(info + 10, 0);
    stl_le_p(info + 12, 0);
    stw_le_p(info + 16, pitch);
    stw_le_p(info + 18, mode->width);
    stw_le_p(info + 20, mode->height);
    info[22] = 8;
    info[23] = 16;
    info[24] = 1;
    info[25] = mode->bpp;
    info[26] = 1;
    info[27] = 6; /* Direct-color memory model. */
    info[28] = 64;
    info[29] = pages;
    info[30] = 1;

    red_size = mode->bpp == 16 ? 5 : 8;
    green_size = mode->bpp == 16 ? 6 : 8;
    alpha_size = mode->bpp == 32 ? 8 : 0;
    alpha_pos = mode->bpp == 32 ? 24 : 0;
    info[31] = red_size;
    info[32] = mode->bpp == 16 ? 11 : 16;
    info[33] = green_size;
    info[34] = mode->bpp == 16 ? 5 : 8;
    info[35] = mode->bpp == 16 ? 5 : 8;
    info[36] = 0;
    info[37] = alpha_size;
    info[38] = alpha_pos;
    info[39] = mode->bpp == 32 ? 2 : 0;
    stl_le_p(info + 40, IA64_VGA_FB_PCI_BASE);
    stw_le_p(info + 50, pitch);
    info[52] = pages;
    info[53] = pages;
    memcpy(info + 54, info + 31, 8);
    ia64_int10_vbe_success(s);
}

static const IA64VbeMode *ia64_int10_current_mode(IA64VpcMachineState *s,
                                                   uint16_t *number)
{
    const IA64VbeMode *mode = NULL;
    uint16_t enable = ia64_vbe_read(VBE_DISPI_INDEX_ENABLE);
    uint16_t width;
    uint16_t height;
    uint16_t bpp;
    size_t i;

    (void)s;
    if (!(enable & VBE_DISPI_ENABLED)) {
        *number = 3;
        return NULL;
    }
    width = ia64_vbe_read(VBE_DISPI_INDEX_XRES);
    height = ia64_vbe_read(VBE_DISPI_INDEX_YRES);
    bpp = ia64_vbe_read(VBE_DISPI_INDEX_BPP);
    for (i = 0; i < G_N_ELEMENTS(ia64_vbe_modes); i++) {
        if (ia64_vbe_modes[i].width == width &&
            ia64_vbe_modes[i].height == height &&
            ia64_vbe_modes[i].bpp == bpp) {
            mode = &ia64_vbe_modes[i];
            break;
        }
    }
    *number = mode ? mode->number : 3;
    if (mode && (enable & VBE_DISPI_LFB_ENABLED)) {
        *number |= 0x4000;
    }
    return mode;
}

static void ia64_int10_set_mode(IA64VpcMachineState *s)
{
    const IA64VbeMode *mode =
        ia64_vbe_find_mode(s->int10_request.bx & 0x01ff);
    uint32_t image_size;
    uint16_t enable;

    if (mode == NULL) {
        ia64_int10_vbe_failure(s);
        return;
    }
    image_size = mode->width * mode->height *
                 DIV_ROUND_UP(mode->bpp, 8);
    if (image_size > ia64_vbe_memory_size()) {
        ia64_int10_vbe_failure(s);
        return;
    }

    ia64_vbe_write(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_DISABLED);
    ia64_vbe_write(VBE_DISPI_INDEX_ID, VBE_DISPI_ID5);
    ia64_vbe_write(VBE_DISPI_INDEX_BPP, mode->bpp);
    ia64_vbe_write(VBE_DISPI_INDEX_XRES, mode->width);
    ia64_vbe_write(VBE_DISPI_INDEX_YRES, mode->height);
    ia64_vbe_write(VBE_DISPI_INDEX_BANK, 0);
    ia64_vbe_write(VBE_DISPI_INDEX_VIRT_WIDTH, mode->width);
    ia64_vbe_write(VBE_DISPI_INDEX_X_OFFSET, 0);
    ia64_vbe_write(VBE_DISPI_INDEX_Y_OFFSET, 0);
    enable = VBE_DISPI_ENABLED;
    if (s->int10_request.bx & 0x4000) {
        enable |= VBE_DISPI_LFB_ENABLED;
    }
    if (s->int10_request.bx & 0x8000) {
        enable |= VBE_DISPI_NOCLEARMEM;
    }
    ia64_vbe_write(VBE_DISPI_INDEX_ENABLE, enable);

    /*
     * Enabling the Bochs VBE registers programs the packed-pixel layout but
     * leaves the VGA attribute controller's Palette-Address-Source bit clear,
     * exactly as it is after reset.  QEMU's VGA core treats a clear PAS bit as
     * "screen disabled" and forces GMODE_BLANK in vga_update_display(), so the
     * guest would render its desktop into VRAM yet the console would stay
     * black.  A real VGABIOS finishes every mode-set by writing 0x20 to the
     * attribute-controller write port to re-enable video output; the legacy
     * text/planar path above already does this.  Do the same for VBE modes so
     * the linear framebuffer is actually scanned out.
     *
     * The attribute controller shares an address/data flip-flop that a read of
     * Input Status 1 resets to the index state.  That register is only decoded
     * at its colour alias (0x3DA) when the Misc Output register selects colour
     * I/O addressing, so force that bit first; otherwise the reset (and hence
     * the enable) would silently depend on whatever mode ran before.
     */
    ia64_vga_writeb(VGA_MIS_W, ia64_vga_readb(VGA_MIS_R) | 0x01);
    (void)ia64_vga_readb(VGA_IS1_RC);
    ia64_vga_writeb(VGA_ATT_W, VGA_AR_ENABLE_DISPLAY);

    if (getenv("IA64_INT10_TRACE")) {
        fprintf(stderr, "int10: set_mode bx=%04x -> %dx%dx%d img=%u vbemem=%u "
                "enable=%04x readback=%04x\n", s->int10_request.bx,
                mode->width, mode->height, mode->bpp, image_size,
                (unsigned)ia64_vbe_memory_size(), enable,
                ia64_vbe_read(VBE_DISPI_INDEX_ENABLE));
    }
    ia64_int10_vbe_success(s);
}

static void ia64_int10_window_control(IA64VpcMachineState *s)
{
    uint8_t subfunction = s->int10_request.bx >> 8;
    uint8_t window = s->int10_request.bx;

    if (window != 0 || subfunction > 1) {
        ia64_int10_vbe_failure(s);
        return;
    }
    if (subfunction == 0) {
        ia64_vbe_write(VBE_DISPI_INDEX_BANK, s->int10_request.dx);
    } else {
        s->int10_result.dx = ia64_vbe_read(VBE_DISPI_INDEX_BANK);
    }
    ia64_int10_vbe_success(s);
}

static void ia64_int10_scanline(IA64VpcMachineState *s)
{
    uint16_t number;
    const IA64VbeMode *mode = ia64_int10_current_mode(s, &number);
    uint8_t subfunction = s->int10_request.bx;
    uint32_t bytes_per_pixel;
    uint32_t width;
    uint32_t pitch;

    if (mode == NULL || subfunction > 2) {
        ia64_int10_vbe_failure(s);
        return;
    }

    bytes_per_pixel = DIV_ROUND_UP(mode->bpp, 8);
    if (subfunction == 0) {
        ia64_vbe_write(VBE_DISPI_INDEX_VIRT_WIDTH,
                       s->int10_request.cx);
    } else if (subfunction == 2) {
        width = DIV_ROUND_UP(s->int10_request.cx, bytes_per_pixel);
        if (width == 0 || width > UINT16_MAX) {
            ia64_int10_vbe_failure(s);
            return;
        }
        ia64_vbe_write(VBE_DISPI_INDEX_VIRT_WIDTH, width);
    }

    width = ia64_vbe_read(VBE_DISPI_INDEX_VIRT_WIDTH);
    pitch = width * bytes_per_pixel;
    if (pitch == 0) {
        ia64_int10_vbe_failure(s);
        return;
    }
    s->int10_result.bx = pitch;
    s->int10_result.cx = width;
    s->int10_result.dx = MIN(ia64_vbe_memory_size() / pitch, UINT16_MAX);
    ia64_int10_vbe_success(s);
}

static void ia64_int10_display_start(IA64VpcMachineState *s)
{
    uint16_t number;
    const IA64VbeMode *mode = ia64_int10_current_mode(s, &number);
    uint8_t subfunction = s->int10_request.bx;

    if (mode == NULL) {
        ia64_int10_vbe_failure(s);
        return;
    }
    switch (subfunction) {
    case 0x00:
    case 0x80:
        ia64_vbe_write(VBE_DISPI_INDEX_X_OFFSET, s->int10_request.cx);
        ia64_vbe_write(VBE_DISPI_INDEX_Y_OFFSET, s->int10_request.dx);
        break;
    case 0x01:
        s->int10_result.cx = ia64_vbe_read(VBE_DISPI_INDEX_X_OFFSET);
        s->int10_result.dx = ia64_vbe_read(VBE_DISPI_INDEX_Y_OFFSET);
        break;
    default:
        ia64_int10_vbe_failure(s);
        return;
    }
    ia64_int10_vbe_success(s);
}

static void ia64_int10_dpms(IA64VpcMachineState *s)
{
    uint8_t subfunction = s->int10_request.bx;

    switch (subfunction) {
    case 0:
        s->int10_result.bx = 0x0f30;
        break;
    case 1:
        s->int10_dpms_state = (s->int10_request.bx >> 8) & 0x0f;
        break;
    case 2:
        s->int10_result.bx = (uint16_t)s->int10_dpms_state << 8 | 2;
        break;
    default:
        ia64_int10_vbe_failure(s);
        return;
    }
    ia64_int10_vbe_success(s);
}

static void ia64_int10_ddc(IA64VpcMachineState *s)
{
    qemu_edid_info edid_info = {
        .vendor = "RHT",
        .name = "QEMU IA64",
        .prefx = 1280,
        .prefy = 1024,
        .maxx = 1280,
        .maxy = 1024,
        .refresh_rate = 60000,
    };
    uint8_t subfunction = s->int10_request.bx;

    switch (subfunction) {
    case 0:
        s->int10_result.bx = 0x0103;
        break;
    case 1:
        if (s->int10_request.dx != 0) {
            ia64_int10_vbe_failure(s);
            return;
        }
        ia64_int10_response_size(s, 128);
        qemu_edid_generate(s->int10_response, 128, &edid_info);
        break;
    default:
        ia64_int10_vbe_failure(s);
        return;
    }
    ia64_int10_vbe_success(s);
}

/*
 * ATI Accelerator-BIOS INT 10h functions (BIOS prefix 0xA000, "VGA enabled").
 * The native Mach64 miniport calls these to obtain the card's configuration;
 * the function number is the low byte of AX.  The synthesised VBE handler does
 * not otherwise answer them, so without this the driver reports "Unable to
 * obtain configuration information for graphics card" and never brings up a
 * mode.  Contract from the ATI Mach64 SDK (M64BIOS.C long_query) and the
 * query_structure layout (Mach64 driver source amach1.h / SDK MAIN.H):
 *   0x08 BIOS_GET_QUERY_SIZE -> CX = header size in bytes, AH = 0.
 *   0x09 BIOS_QUERY          -> write the query_structure header to the buffer
 *                               at DX:BX (segment:offset), AH = 0.
 */
static void ia64_int10_ati_bios(IA64VpcMachineState *s)
{
    unsigned fn = s->int10_request.ax & 0xff;
    uint8_t *q;

    switch (fn) {
    case 0x08:  /* BIOS_GET_QUERY_SIZE */
        s->int10_result.cx = 0x20;              /* 32-byte header */
        s->int10_result.ax &= 0x00ff;           /* AH = 0: success */
        break;
    case 0x09:  /* BIOS_QUERY: deliver the header to DX:BX via the stub copy */
        ia64_int10_response_size(s, 0x20);
        q = s->int10_response;
        stw_le_p(q + 0x00, 0x20);               /* q_sizeof_struct */
        q[0x02] = 0x02;                         /* q_structure_rev */
        q[0x03] = 0x00;                         /* q_number_modes (header only) */
        stw_le_p(q + 0x04, 0x0000);             /* q_mode_offset */
        q[0x06] = 0x00;                         /* q_sizeof_mode */
        q[0x07] = 0x01;                         /* q_VGA_type: enabled */
        stw_le_p(q + 0x08, 0x4752);             /* q_asic_id (Rage XL) */
        q[0x0a] = 0x00;                         /* q_VGA_boundary */
        /*
         * q_memory_size is an INDEX into the miniport's video-RAM-size table,
         * NOT a byte/quarter-meg count.  The XP Rage XL miniport (atimpae.sys
         * .BiosQueryAdapter) rejects the whole query with
         * "Unable to obtain configuration information" (0xC1010003, event
         * DumpData UniqueId 0x106) when this index is >= 16, then falls back to
         * VgaSave.  Its table (ex_ulaVideoRamSize) maps 0->512K, 1->1M, 2->2M,
         * 3->4M, 4->6M, 5->8M, ... so 8 MiB of VRAM is index 5.
         */
        q[0x0b] = 0x05;                         /* q_memory_size: index 5 = 8MiB */
        q[0x0c] = 0x00;                         /* q_DAC_type: 0 = internal (CT) DAC */
        q[0x0d] = 0x0a;                         /* q_memory_type: SDRAM */
        q[0x0e] = 0x07;                         /* q_bus_type: BUS_PCI */
        q[0x0f] = 0x00;                         /* q_monitor_cntl */
        stw_le_p(q + 0x10, IA64_VGA_FB_PCI_BASE >> 20); /* q_aperture_addr (MiB) */
        q[0x12] = 0x02;                         /* q_aperture_cfg: 8MiB linear */
        q[0x13] = 0x2f;                         /* colour depths 565/555/RGB/BGR/RGBA */
        s->int10_result.es = s->int10_request.dx;
        s->int10_result.di = s->int10_request.bx;
        s->int10_result.ax &= 0x00ff;           /* AH = 0: success */
        break;
    default:
        /* Acknowledge other ATI functions (e.g. 0x14) as success no-ops. */
        s->int10_result.ax &= 0x00ff;
        break;
    }
}

static void ia64_int10_execute(IA64VpcMachineState *s)
{
    uint16_t current_mode;

    s->int10_result = s->int10_request;
    ia64_int10_response_clear(s);

    if (getenv("IA64_INT10_TRACE")) {
        bool handled = (s->int10_request.ax & 0xff00) == 0x4f00 ||
                       (s->int10_request.ax >> 8) == 0x00 ||
                       (s->int10_request.ax >> 8) == 0x0f ||
                       (s->int10_request.ax >> 8) == 0x1a;
        fprintf(stderr, "int10: ax=%04x bx=%04x cx=%04x dx=%04x di=%04x "
                "es=%04x%s\n", s->int10_request.ax, s->int10_request.bx,
                s->int10_request.cx, s->int10_request.dx, s->int10_request.di,
                s->int10_request.es, handled ? "" : "  [UNHANDLED]");
    }

    if ((s->int10_request.ax & 0xff00) == 0xa000) {
        ia64_int10_ati_bios(s);
        return;
    }

    if ((s->int10_request.ax & 0xff00) == 0x4f00) {
        switch (s->int10_request.ax & 0xff) {
        case 0x00:
            ia64_int10_controller_info(s);
            return;
        case 0x01:
            ia64_int10_mode_info(s);
            return;
        case 0x02:
            ia64_int10_set_mode(s);
            return;
        case 0x03:
            ia64_int10_current_mode(s, &current_mode);
            s->int10_result.bx = current_mode;
            ia64_int10_vbe_success(s);
            return;
        case 0x05:
            ia64_int10_window_control(s);
            return;
        case 0x06:
            ia64_int10_scanline(s);
            return;
        case 0x07:
            ia64_int10_display_start(s);
            return;
        case 0x10:
            ia64_int10_dpms(s);
            return;
        case 0x15:
            ia64_int10_ddc(s);
            return;
        default:
            ia64_int10_vbe_unsupported(s);
            return;
        }
    }

    switch (s->int10_request.ax >> 8) {
    case 0x00:
        ia64_int10_set_legacy_mode(s, s->int10_request.ax);
        break;
    case 0x0f:
        if (ia64_vbe_read(VBE_DISPI_INDEX_ENABLE) & VBE_DISPI_ENABLED) {
            s->int10_result.ax = 80 << 8 | 3;
        } else {
            s->int10_result.ax = (uint16_t)s->int10_legacy_columns << 8 |
                                 s->int10_legacy_mode;
        }
        s->int10_result.bx &= 0x00ff;
        break;
    case 0x1a:
        if ((s->int10_request.ax & 0xff) == 0) {
            s->int10_result.ax = 0x001a;
            s->int10_result.bx = 0x0008;
        }
        break;
    default:
        break;
    }
}

static uint64_t ia64_int10_io_read(void *opaque, hwaddr addr, unsigned size)
{
    IA64VpcMachineState *s = opaque;
    unsigned reg = addr >> 1;

    if (size != 2 || (addr & 1)) {
        return 0xffff;
    }
    switch (reg) {
    case IA64_INT10_REG_AX:
        return s->int10_result.ax;
    case IA64_INT10_REG_BX:
        return s->int10_result.bx;
    case IA64_INT10_REG_CX:
        return s->int10_result.cx;
    case IA64_INT10_REG_DX:
        return s->int10_result.dx;
    case IA64_INT10_REG_DI:
        return s->int10_result.di;
    case IA64_INT10_REG_ES:
        return s->int10_result.es;
    case IA64_INT10_REG_EXEC:
        return s->int10_response_length / 2;
    case IA64_INT10_REG_DATA:
        if (s->int10_response_offset < s->int10_response_length) {
            uint16_t value = lduw_le_p(s->int10_response +
                                      s->int10_response_offset);

            s->int10_response_offset += 2;
            return value;
        }
        return 0;
    default:
        return 0xffff;
    }
}

static void ia64_int10_io_write(void *opaque, hwaddr addr, uint64_t value,
                                unsigned size)
{
    IA64VpcMachineState *s = opaque;
    unsigned reg = addr >> 1;

    if (size != 2 || (addr & 1)) {
        return;
    }
    switch (reg) {
    case IA64_INT10_REG_AX:
        s->int10_request.ax = value;
        s->int10_input_signature = 0;
        s->int10_input_signature_words = 0;
        break;
    case IA64_INT10_REG_BX:
        s->int10_request.bx = value;
        break;
    case IA64_INT10_REG_CX:
        s->int10_request.cx = value;
        break;
    case IA64_INT10_REG_DX:
        s->int10_request.dx = value;
        break;
    case IA64_INT10_REG_DI:
        s->int10_request.di = value;
        break;
    case IA64_INT10_REG_ES:
        s->int10_request.es = value;
        break;
    case IA64_INT10_REG_EXEC:
        if ((uint16_t)value == IA64_INT10_TRIGGER) {
            ia64_int10_execute(s);
        }
        break;
    case IA64_INT10_REG_DATA:
        if (s->int10_input_signature_words < 2) {
            s->int10_input_signature |=
                (uint32_t)(uint16_t)value <<
                (s->int10_input_signature_words * 16);
            s->int10_input_signature_words++;
        }
        break;
    default:
        break;
    }
}

static const MemoryRegionOps ia64_int10_io_ops = {
    .read = ia64_int10_io_read,
    .write = ia64_int10_io_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 2,
        .max_access_size = 2,
        .unaligned = false,
    },
    .impl = {
        .min_access_size = 2,
        .max_access_size = 2,
        .unaligned = false,
    },
};

static void ia64_int10_install_ati_bios_info(uint8_t *rom,
                                             uint16_t vendor,
                                             uint16_t device)
{
    if (vendor != IA64_ATI_VENDOR_ID) {
        return;
    }

    /*
     * ATI's drivers locate and validate the video BIOS by the ROM signature
     * " 761295520" at 30h before following the pointer chain at 48h.  All
     * three retail Rage 128 Pro dumps carry it there.  Windows Whistler
     * build 2462's miniport (ati2mpaa.sys, "RAGE128/128PRO Miniport Driver
     * VersionR128.121") embeds the string and bugchecks 0x1E dereferencing
     * the NULL table pointer it is left with when the signature is absent.
     *
     * The Server 2003 (build 3790) inbox *mach64* miniport (ati2mpad.sys)
     * needs it too: its GetVgaEnabledRomImage scans offsets 30h..80h of the
     * C0000h shadow for "761295520" (Get_BIOS_Seg, WSRV03 drivers/video/ms/
     * ati/mini/services.c:1472) and, when absent, returns a NULL RomImage
     * that RageProEnable->InitializeBiosInfoStructure dereferences unchecked
     * at base+78h -> STOP 0x8E in videoprt!VideoPortReadRegisterBufferUchar.
     * So the signature is published for every ATI adapter, not just Rage128.
     */
    memcpy(rom + IA64_INT10_ROM_ATI_SIG_OFFSET, " 761295520", 10);

    if (device != IA64_ATI_RAGE128_PF_ID) {
        /*
         * mach64 (DEV_4752 Rage XL): ati2mpad reads its adapter configuration
         * through the a009 INT 10h query (ia64_int10_ati_bios), not the legacy
         * 48h Rage128 PLL pointer chain, so only the signature is required
         * here.  Do not publish the Rage128-format header/PLL block below.
         */
        return;
    }

    /*
     * Native Rage128 drivers follow the legacy ATI BIOS pointer chain at
     * 48h to obtain PLL limits.  A generic VBE ROM which only has a valid
     * 55AAh header is otherwise mistaken for an ATI BIOS, and the driver
     * interprets executable bytes as clock values.  Publish the small,
     * device-specific data block expected by those drivers while keeping
     * all video services in the generic INT 10h implementation.
     *
     * Values use the units defined by the Rage128 BIOS interface: clocks
     * are in 10 kHz units.  They match the range supported by QEMU's
     * Rage128-compatible display model and its existing VGA BIOS.
     */
    stw_le_p(rom + 0x48, IA64_INT10_ROM_ATI_HEADER_OFFSET);
    stw_le_p(rom + IA64_INT10_ROM_ATI_HEADER_OFFSET + 0x30,
             IA64_INT10_ROM_ATI_PLL_OFFSET);
    stw_le_p(rom + IA64_INT10_ROM_ATI_PLL_OFFSET + 0x08,
             IA64_ATI_PLL_XCLK);
    stw_le_p(rom + IA64_INT10_ROM_ATI_PLL_OFFSET + 0x0e,
             IA64_ATI_PLL_REFERENCE_FREQ);
    stw_le_p(rom + IA64_INT10_ROM_ATI_PLL_OFFSET + 0x10,
             IA64_ATI_PLL_REFERENCE_DIV);
    stl_le_p(rom + IA64_INT10_ROM_ATI_PLL_OFFSET + 0x12,
             IA64_ATI_PLL_MIN_FREQ);
    stl_le_p(rom + IA64_INT10_ROM_ATI_PLL_OFFSET + 0x16,
             IA64_ATI_PLL_MAX_FREQ);
}

static void ia64_vpc_install_int10(IA64VpcMachineState *s)
{
    uint8_t rom[IA64_INT10_ROM_SIZE] = { 0 };
    uint8_t vector[4];
    uint8_t checksum = 0;
    uint16_t vendor = pci_get_word(s->vga_dev->config + PCI_VENDOR_ID);
    uint16_t device = pci_get_word(s->vga_dev->config + PCI_DEVICE_ID);
    size_t i;

    g_assert(IA64_INT10_ROM_ATI_SIG_OFFSET + 10 <= 0x48);
    g_assert(IA64_INT10_ROM_ATI_PLL_OFFSET + 0x20 <=
             IA64_INT10_ROM_PCIR_OFFSET);
    g_assert(IA64_INT10_ROM_PCIR_OFFSET + 0x18 <=
             IA64_INT10_ROM_HANDLER_OFFSET);
    g_assert(IA64_INT10_ROM_HANDLER_OFFSET +
             sizeof(ia64_int10_handler) <= IA64_INT10_ROM_OEM_OFFSET);
    g_assert(IA64_INT10_ROM_OEM_OFFSET + sizeof(ia64_vbe_oem) <=
             IA64_INT10_ROM_VENDOR_OFFSET);
    g_assert(IA64_INT10_ROM_VENDOR_OFFSET + sizeof(ia64_vbe_vendor) <=
             IA64_INT10_ROM_PRODUCT_OFFSET);
    g_assert(IA64_INT10_ROM_PRODUCT_OFFSET + sizeof(ia64_vbe_product) <=
             IA64_INT10_ROM_REVISION_OFFSET);
    g_assert(IA64_INT10_ROM_REVISION_OFFSET + sizeof(ia64_vbe_revision) <=
             IA64_INT10_ROM_MODES_OFFSET);
    g_assert(IA64_INT10_ROM_MODES_OFFSET +
             (G_N_ELEMENTS(ia64_vbe_modes) + 1) * 2 < sizeof(rom));
    rom[0] = 0x55;
    rom[1] = 0xaa;
    rom[2] = IA64_INT10_ROM_SIZE / 512;
    memcpy(rom + 3, ia64_int10_rom_init, sizeof(ia64_int10_rom_init));

    /*
     * Keep PCIR away from the legacy ATI BIOS pointer at 48h.  Both fields
     * are consumed by real drivers and ROM validators.
     */
    stw_le_p(rom + 0x18, IA64_INT10_ROM_PCIR_OFFSET);
    memcpy(rom + IA64_INT10_ROM_PCIR_OFFSET, "PCIR", 4);
    stw_le_p(rom + IA64_INT10_ROM_PCIR_OFFSET + 0x04, vendor);
    stw_le_p(rom + IA64_INT10_ROM_PCIR_OFFSET + 0x06, device);
    stw_le_p(rom + IA64_INT10_ROM_PCIR_OFFSET + 0x08, 0);
    stw_le_p(rom + IA64_INT10_ROM_PCIR_OFFSET + 0x0a, 0x18);
    rom[IA64_INT10_ROM_PCIR_OFFSET + 0x0c] = 0;
    rom[IA64_INT10_ROM_PCIR_OFFSET + 0x0d] = 0;
    rom[IA64_INT10_ROM_PCIR_OFFSET + 0x0e] = 0;
    rom[IA64_INT10_ROM_PCIR_OFFSET + 0x0f] =
        PCI_CLASS_DISPLAY_VGA >> 8;
    stw_le_p(rom + IA64_INT10_ROM_PCIR_OFFSET + 0x10,
             IA64_INT10_ROM_SIZE / 512);
    stw_le_p(rom + IA64_INT10_ROM_PCIR_OFFSET + 0x12, 0x0100);
    rom[IA64_INT10_ROM_PCIR_OFFSET + 0x14] = 0;
    rom[IA64_INT10_ROM_PCIR_OFFSET + 0x15] = 0x80;
    memcpy(rom + 0x60, "QEMU IA64 VBE INT10", 20);
    ia64_int10_install_ati_bios_info(rom, vendor, device);
    memcpy(rom + IA64_INT10_ROM_HANDLER_OFFSET, ia64_int10_handler,
           sizeof(ia64_int10_handler));
    memcpy(rom + IA64_INT10_ROM_OEM_OFFSET,
           ia64_vbe_oem, sizeof(ia64_vbe_oem));
    memcpy(rom + IA64_INT10_ROM_VENDOR_OFFSET,
           ia64_vbe_vendor, sizeof(ia64_vbe_vendor));
    memcpy(rom + IA64_INT10_ROM_PRODUCT_OFFSET,
           ia64_vbe_product, sizeof(ia64_vbe_product));
    memcpy(rom + IA64_INT10_ROM_REVISION_OFFSET,
           ia64_vbe_revision, sizeof(ia64_vbe_revision));
    for (i = 0; i < G_N_ELEMENTS(ia64_vbe_modes); i++) {
        stw_le_p(rom + IA64_INT10_ROM_MODES_OFFSET + i * 2,
                 ia64_vbe_modes[i].number);
    }
    stw_le_p(rom + IA64_INT10_ROM_MODES_OFFSET +
             G_N_ELEMENTS(ia64_vbe_modes) * 2, 0xffff);

    for (i = 0; i < sizeof(rom) - 1; i++) {
        checksum += rom[i];
    }
    rom[sizeof(rom) - 1] = -checksum;
    cpu_physical_memory_write(IA64_INT10_ROM_BASE, rom, sizeof(rom));

    /*
     * Keep the interrupt entry inside its option ROM.  In addition to being
     * the conventional PC BIOS layout, Windows videoprt validates that the
     * INT 10h vector resolves into the C0000h-CFFFFh video-ROM window before
     * it enables its x86 BIOS emulator.
     */
    stw_le_p(vector, IA64_INT10_ROM_HANDLER_OFFSET);
    stw_le_p(vector + 2, IA64_INT10_ROM_BASE >> 4);
    cpu_physical_memory_write(IA64_INT10_VECTOR_ADDR, vector,
                              sizeof(vector));
}

/*
 * Real-firmware video-ROM shadow.  The synthetic INT10 ROM above is a passive
 * 2 KiB image for Windows guests, which read the video BIOS through the PCI ROM
 * BAR (VideoPortGetRomImage).  The vendor SDV firmware instead POSTs the video
 * card's option ROM the legacy PC-AT way: shadow it to 0xC0000 and call
 * C000:0003.  Its shadow copy reads through the ROM BAR, and our ROM-BAR model
 * is not faithful enough for that read to capture the whole image -- only the
 * header lands, so the option ROM's entry jump (e.g. std vgabios `jmp 0x55C3`)
 * runs the CPU into empty shadow and hangs POST at ~0xc6.
 *
 * Place a complete option ROM at the 0xC0000 shadow directly so the firmware
 * finds a whole, valid option ROM to POST in place.  This is the realfw analogue
 * of install_int10 (the synthetic stub is skipped in realfw).  The ROM is the
 * emulated card's own expansion ROM by default, or -- when realfw-vga-rom= names
 * a file -- an authentic vendor card BIOS (e.g. the ATI Rage 128 Pro the SDV
 * shipped with), so the firmware POSTs the real BIOS for accurate emulation.
 */
static void ia64_vpc_install_realfw_video_rom(IA64VpcMachineState *s)
{
    PCIDevice *pci_dev = s->vga_dev;
    g_autofree uint8_t *file_rom = NULL;
    const uint8_t *rom = NULL;
    uint64_t rom_size = 0;
    uint32_t declared;

    if (s->realfw_vga_rom_path != NULL) {
        GError *gerr = NULL;
        gsize len = 0;

        if (!g_file_get_contents(s->realfw_vga_rom_path, (gchar **)&file_rom,
                                 &len, &gerr)) {
            warn_report("realfw-vga-rom '%s': %s (falling back to card ROM)",
                        s->realfw_vga_rom_path, gerr->message);
            g_error_free(gerr);
        } else {
            rom = file_rom;
            rom_size = len;
        }
    }
    if (file_rom == NULL) {
        if (pci_dev == NULL ||
            pci_dev->io_regions[PCI_ROM_SLOT].size == 0 || !pci_dev->has_rom) {
            return;
        }
        rom = memory_region_get_ram_ptr(&pci_dev->rom);
        rom_size = memory_region_size(&pci_dev->rom);
    }
    if (rom == NULL || rom_size < 0x400 || rom[0] != 0x55 || rom[1] != 0xaa) {
        return;
    }
    declared = (uint32_t)rom[2] * 512U;
    if (declared == 0 || declared > rom_size) {
        declared = rom_size;
    }
    /* The legacy video-ROM window is C0000h-CFFFFh (64 KiB). */
    if (declared > 0x10000) {
        declared = 0x10000;
    }
    cpu_physical_memory_write(IA64_INT10_ROM_BASE, rom, declared);
}

/*
 * When realfw-vga-rom= supplies an authentic card BIOS, load it into the video
 * device's own expansion ROM as well as the 0xC0000 shadow.  The vendor firmware
 * re-shadows the option ROM's header from the PCI ROM BAR during POST; if the BAR
 * still held the emulated card's stock vgabios, that header's entry jump (a
 * different offset) would be laid over the real BIOS body already shadowed at
 * 0xC0000, and the CPU would jump into the wrong image and run away.  Keeping the
 * BAR and the shadow the same image keeps the re-shadow consistent.  Called
 * before configure_vga() so the ATI table / checksum fixups act on this image.
 */
static void ia64_vpc_load_realfw_device_rom(IA64VpcMachineState *s)
{
    PCIDevice *pci_dev = s->vga_dev;
    g_autofree uint8_t *file_rom = NULL;
    GError *gerr = NULL;
    gsize len = 0;
    uint8_t *rom;
    uint64_t rom_size;

    if (s->realfw_path == NULL || s->realfw_vga_rom_path == NULL ||
        pci_dev == NULL ||
        pci_dev->io_regions[PCI_ROM_SLOT].size == 0 || !pci_dev->has_rom) {
        return;
    }
    if (!g_file_get_contents(s->realfw_vga_rom_path, (gchar **)&file_rom,
                             &len, &gerr)) {
        warn_report("realfw-vga-rom '%s': %s", s->realfw_vga_rom_path,
                    gerr->message);
        g_error_free(gerr);
        return;
    }
    if (len < 0x400 || file_rom[0] != 0x55 || file_rom[1] != 0xaa) {
        warn_report("realfw-vga-rom '%s': not a 55AA option ROM",
                    s->realfw_vga_rom_path);
        return;
    }
    rom = memory_region_get_ram_ptr(&pci_dev->rom);
    rom_size = memory_region_size(&pci_dev->rom);
    if (rom == NULL || rom_size == 0) {
        return;
    }
    if (len > rom_size) {
        len = rom_size;
    }
    memset(rom, 0, rom_size);
    memcpy(rom, file_rom, len);
}

static void ia64_vpc_reset_int10(IA64VpcMachineState *s)
{
    memset(&s->int10_request, 0, sizeof(s->int10_request));
    memset(&s->int10_result, 0, sizeof(s->int10_result));
    s->int10_input_signature = 0;
    s->int10_input_signature_words = 0;
    ia64_int10_response_clear(s);
    s->int10_dpms_state = 0;
    s->int10_legacy_mode = 3;
    s->int10_legacy_columns = 80;
    ia64_vpc_install_int10(s);
}

static void ia64_vpc_init_int10(IA64VpcMachineState *s,
                                MemoryRegion *pci_io)
{
    memory_region_init_io(&s->int10_pci_io, OBJECT(s),
                          &ia64_int10_io_ops, s,
                          "ia64-vpc.int10-pci-io", IA64_INT10_IO_SIZE);
    memory_region_add_subregion(pci_io, IA64_INT10_IO_BASE,
                                &s->int10_pci_io);
    ia64_vpc_reset_int10(s);
}
#endif


static void ia64_vpc_watchdog_expired(void *opaque)
{
    IA64VpcMachineState *s = opaque;

    warn_report("IA-64 firmware watchdog expired (code 0x%" PRIx64 ")",
                s->watchdog_code);
    s->watchdog_timeout = 0;
    watchdog_perform_action();
}

static uint64_t ia64_vpc_watchdog_read(void *opaque, hwaddr addr,
                                       unsigned size)
{
    IA64VpcMachineState *s = opaque;

    (void)size;

    switch (addr) {
    case IA64_WATCHDOG_TIMEOUT_OFFSET:
        return s->watchdog_timeout;
    case IA64_WATCHDOG_CODE_OFFSET:
        return s->watchdog_code;
    default:
        return 0;
    }
}

static void ia64_vpc_watchdog_write(void *opaque, hwaddr addr,
                                    uint64_t value, unsigned size)
{
    IA64VpcMachineState *s = opaque;
    int64_t now;
    int64_t delta;

    if (size != sizeof(uint64_t)) {
        return;
    }

    switch (addr) {
    case IA64_WATCHDOG_CODE_OFFSET:
        s->watchdog_code = value;
        break;
    case IA64_WATCHDOG_TIMEOUT_OFFSET:
        s->watchdog_timeout = value;
        timer_del(s->watchdog_timer);
        if (value == 0) {
            break;
        }
        now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        if (value > (uint64_t)(INT64_MAX - now) / NANOSECONDS_PER_SECOND) {
            delta = INT64_MAX - now;
        } else {
            delta = value * NANOSECONDS_PER_SECOND;
        }
        timer_mod(s->watchdog_timer, now + delta);
        break;
    default:
        break;
    }
}

static const MemoryRegionOps ia64_vpc_watchdog_ops = {
    .read = ia64_vpc_watchdog_read,
    .write = ia64_vpc_watchdog_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 8,
        .max_access_size = 8,
        .unaligned = false,
    },
    .impl = {
        .min_access_size = 8,
        .max_access_size = 8,
        .unaligned = false,
    },
};

static void ia64_vpc_watchdog_reset(void *opaque)
{
    IA64VpcMachineState *s = opaque;

    timer_del(s->watchdog_timer);
    s->watchdog_timeout = 0;
    s->watchdog_code = 0;
}

static void ia64_vpc_init_watchdog(IA64VpcMachineState *s)
{
    s->watchdog_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                     ia64_vpc_watchdog_expired, s);
    memory_region_init_io(&s->watchdog_mmio, OBJECT(s),
                          &ia64_vpc_watchdog_ops, s,
                          "ia64-vpc.firmware-watchdog",
                          IA64_WATCHDOG_SIZE);
    memory_region_add_subregion_overlap(get_system_memory(),
                                        IA64_WATCHDOG_BASE,
                                        &s->watchdog_mmio, 2);
    qemu_register_reset(ia64_vpc_watchdog_reset, s);
}

static char *ia64_vpc_get_realfw(Object *obj, Error **errp)
{
    IA64VpcMachineState *s = IA64_VPC_MACHINE(obj);

    (void)errp;

    return g_strdup(s->realfw_path ?: "");
}

static void ia64_vpc_set_realfw(Object *obj, const char *value, Error **errp)
{
    IA64VpcMachineState *s = IA64_VPC_MACHINE(obj);

    (void)errp;

    g_free(s->realfw_path);
    s->realfw_path = value[0] != '\0' ? g_strdup(value) : NULL;
}

static char *ia64_vpc_get_realfw_vga_rom(Object *obj, Error **errp)
{
    IA64VpcMachineState *s = IA64_VPC_MACHINE(obj);

    (void)errp;

    return g_strdup(s->realfw_vga_rom_path ?: "");
}

static void ia64_vpc_set_realfw_vga_rom(Object *obj, const char *value,
                                        Error **errp)
{
    IA64VpcMachineState *s = IA64_VPC_MACHINE(obj);

    (void)errp;

    g_free(s->realfw_vga_rom_path);
    s->realfw_vga_rom_path = value[0] != '\0' ? g_strdup(value) : NULL;
}

static char *ia64_vpc_get_realfw_nvram(Object *obj, Error **errp)
{
    IA64VpcMachineState *s = IA64_VPC_MACHINE(obj);

    (void)errp;

    return g_strdup(s->realfw_nvram_path ?: "");
}

static void ia64_vpc_set_realfw_nvram(Object *obj, const char *value,
                                      Error **errp)
{
    IA64VpcMachineState *s = IA64_VPC_MACHINE(obj);

    (void)errp;

    g_free(s->realfw_nvram_path);
    s->realfw_nvram_path = value[0] != '\0' ? g_strdup(value) : NULL;
}

static char *ia64_vpc_get_nvram(Object *obj, Error **errp)
{
    IA64VpcMachineState *s = IA64_VPC_MACHINE(obj);

    (void)errp;

    return g_strdup(s->nvram_path ?: "auto");
}

static void ia64_vpc_set_nvram(Object *obj, const char *value, Error **errp)
{
    IA64VpcMachineState *s = IA64_VPC_MACHINE(obj);

    (void)errp;

    g_free(s->nvram_path);
    s->nvram_path = g_strcmp0(value, "auto") == 0 ?
                    NULL : g_strdup(value);
}

static uint64_t ia64_vpc_nvram_read(void *opaque, hwaddr addr,
                                    unsigned size)
{
    IA64VpcMachineState *s = opaque;
    uint64_t value = 0;
    unsigned i;

    for (i = 0; i < size; i++) {
        value |= (uint64_t)s->nvram_data[addr + i] << (i * 8);
    }
    return value;
}

static void ia64_vpc_nvram_commit(IA64VpcMachineState *s)
{
    g_autoptr(GError) err = NULL;

    if (!s->nvram_resolved_path) {
        return;
    }
    if (!g_file_set_contents(s->nvram_resolved_path,
                             (const char *)s->nvram_data,
                             sizeof(s->nvram_data), &err) &&
        !s->nvram_write_warning) {
        warn_report("failed to save IA-64 NVRAM '%s': %s",
                    s->nvram_resolved_path,
                    err ? err->message : "unknown error");
        s->nvram_write_warning = true;
    }
}

static void ia64_vpc_nvram_write(void *opaque, hwaddr addr,
                                 uint64_t value, unsigned size)
{
    IA64VpcMachineState *s = opaque;
    unsigned i;

    if (addr == IA64_NVRAM_COMMIT_OFFSET && size == 8 &&
        value == IA64_NVRAM_COMMIT_MAGIC) {
        ia64_vpc_nvram_commit(s);
        return;
    }
    for (i = 0; i < size; i++) {
        s->nvram_data[addr + i] = value >> (i * 8);
    }
}

static const MemoryRegionOps ia64_vpc_nvram_ops = {
    .read = ia64_vpc_nvram_read,
    .write = ia64_vpc_nvram_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 8,
        .unaligned = true,
    },
    .impl = {
        .min_access_size = 1,
        .max_access_size = 8,
        .unaligned = true,
    },
};

static void ia64_vpc_init_nvram(IA64VpcMachineState *s)
{
    MachineState *machine = MACHINE(s);
    g_autofree char *firmware_path = NULL;
    g_autofree char *directory = NULL;
    g_autofree char *contents = NULL;
    g_autoptr(GError) err = NULL;
    gsize length = 0;

    /*
     * In realfw mode the FIT-0x1E NVRAM sector is part of the vendor flash
     * image, which is modelled by the pflash device (writable, in the flash
     * itself); the synthetic NVRAM MMIO would shadow it, so skip it here.
     */
    if (s->realfw_path != NULL) {
        return;
    }

    memset(s->nvram_data, 0, sizeof(s->nvram_data));
    g_clear_pointer(&s->nvram_resolved_path, g_free);
    s->nvram_write_warning = false;

    if (g_strcmp0(s->nvram_path, "none") != 0) {
        if (s->nvram_path) {
            s->nvram_resolved_path = g_strdup(s->nvram_path);
        } else if (machine->firmware) {
            firmware_path = qemu_find_file(QEMU_FILE_TYPE_BIOS,
                                           machine->firmware);
            if (!firmware_path) {
                firmware_path = g_strdup(machine->firmware);
            }
            directory = g_path_get_dirname(firmware_path);
            s->nvram_resolved_path =
                g_build_filename(directory, "nvram", NULL);
        }
    }

    if (s->nvram_resolved_path &&
        g_file_get_contents(s->nvram_resolved_path, &contents,
                            &length, &err)) {
        if (length == sizeof(s->nvram_data)) {
            memcpy(s->nvram_data, contents, length);
        } else {
            warn_report("ignoring IA-64 NVRAM '%s': expected %zu bytes, "
                        "found %zu",
                        s->nvram_resolved_path,
                        sizeof(s->nvram_data), (size_t)length);
        }
    } else if (err && !g_error_matches(err, G_FILE_ERROR,
                                       G_FILE_ERROR_NOENT)) {
        warn_report("failed to load IA-64 NVRAM '%s': %s",
                    s->nvram_resolved_path, err->message);
    }

    memory_region_init_io(&s->nvram_mmio, OBJECT(s),
                          &ia64_vpc_nvram_ops, s, "ia64-vpc.nvram",
                          IA64_NVRAM_SIZE);
    memory_region_add_subregion_overlap(get_system_memory(), IA64_NVRAM_BASE,
                                        &s->nvram_mmio, 2);
}

typedef struct IA64VpcCompatDefault {
    const char *driver;
    const char *property;
    const char *value;
} IA64VpcCompatDefault;

static const IA64VpcCompatDefault ia64_vpc_compat_defaults[] = {
    /*
     * Some IA-64 USB hub drivers use an alignment-requiring 32-bit load for
     * packed extended-property descriptors.  Do not expose the optional
     * selective-suspend property on HID input devices.
     */
    { "usb-kbd", "msos-desc", "off" },
    { "usb-mouse", "msos-desc", "off" },
    { "usb-tablet", "msos-desc", "off" },
    /*
     * Render the RAGE 128 hardware cursor into the framebuffer rather than as
     * a host overlay.  The chip has no hotspot register -- the driver bakes the
     * hotspot into CUR_HORZ_VERT_POSN/_OFF -- so a host overlay (which needs an
     * explicit hotspot) cannot place arbitrary cursors correctly: Windows XP
     * drives the hardware cursor at 8bpp and the overlay landed ~10px off, and
     * a per-cursor hotspot guess only works for the arrow, not centre-hotspot
     * cursors (I-beam, hourglass).  Compositing reproduces the exact hardware
     * pixels at the exact hardware position, so every cursor type is correct.
     */
    { "ati-vga", "guest_hwcursor", "on" },
    /* Same reasoning for the Mach64 hardware cursor. */
    { "mach64-vga", "guest_hwcursor", "on" },
};

static void ia64_vpc_add_compat_defaults(MachineClass *mc)
{
    size_t i;

    for (i = 0; i < G_N_ELEMENTS(ia64_vpc_compat_defaults); i++) {
        const IA64VpcCompatDefault *value = &ia64_vpc_compat_defaults[i];
        GlobalProperty *property = g_new0(GlobalProperty, 1);

        property->driver = value->driver;
        property->property = value->property;
        property->value = value->value;
        g_ptr_array_add(mc->compat_props, property);
    }
}

static bool ia64_vpc_get_fw_relocate(Object *obj, Error **errp)
{
    IA64VpcMachineState *s = IA64_VPC_MACHINE(obj);

    (void)errp;

    return s->fw_relocate;
}

static void ia64_vpc_set_fw_relocate(Object *obj, bool value, Error **errp)
{
    IA64VpcMachineState *s = IA64_VPC_MACHINE(obj);

    (void)errp;

    s->fw_relocate = value;
}

static bool ia64_vpc_get_i8042(Object *obj, Error **errp)
{
    IA64VpcMachineState *s = IA64_VPC_MACHINE(obj);

    (void)errp;

    return s->i8042_enabled;
}

static void ia64_vpc_set_i8042(Object *obj, bool value, Error **errp)
{
    IA64VpcMachineState *s = IA64_VPC_MACHINE(obj);

#ifndef CONFIG_IA64_VPC_PS2
    if (value) {
        error_setg(errp, "i8042 support is not present in this build");
        return;
    }
#else
    (void)errp;
#endif

    s->i8042_enabled = value;
}

static bool ia64_vpc_get_ahci(Object *obj, Error **errp)
{
    IA64VpcMachineState *s = IA64_VPC_MACHINE(obj);

    (void)errp;

    return s->ahci_enabled;
}

static void ia64_vpc_set_ahci(Object *obj, bool value, Error **errp)
{
    IA64VpcMachineState *s = IA64_VPC_MACHINE(obj);

#ifndef CONFIG_IA64_VPC_STORAGE
    if (value) {
        error_setg(errp, "AHCI support is not present in this build");
        return;
    }
#else
    (void)errp;
#endif

    s->ahci_enabled = value;
}

static bool ia64_vpc_get_audio(Object *obj, Error **errp)
{
    IA64VpcMachineState *s = IA64_VPC_MACHINE(obj);

    (void)errp;

    return s->audio_enabled;
}

static void ia64_vpc_set_audio(Object *obj, bool value, Error **errp)
{
    IA64VpcMachineState *s = IA64_VPC_MACHINE(obj);

#ifndef CONFIG_IA64_VPC_AUDIO
    if (value) {
        error_setg(errp, "audio support is not present in this build");
        return;
    }
#else
    (void)errp;
#endif

    s->audio_enabled = value;
}

static bool ia64_vpc_get_isp(Object *obj, Error **errp)
{
    IA64VpcMachineState *s = IA64_VPC_MACHINE(obj);

    (void)errp;

    return s->isp_enabled;
}

static void ia64_vpc_set_isp(Object *obj, bool value, Error **errp)
{
    IA64VpcMachineState *s = IA64_VPC_MACHINE(obj);

#ifndef CONFIG_IA64_VPC_STORAGE
    if (value) {
        error_setg(errp, "SCSI support is not present in this build");
        return;
    }
#else
    (void)errp;
#endif

    s->isp_enabled = value;
}

static bool ia64_vpc_get_lsi(Object *obj, Error **errp)
{
    IA64VpcMachineState *s = IA64_VPC_MACHINE(obj);

    (void)errp;

    return s->lsi_enabled;
}

static void ia64_vpc_set_lsi(Object *obj, bool value, Error **errp)
{
    IA64VpcMachineState *s = IA64_VPC_MACHINE(obj);

#ifndef CONFIG_IA64_VPC_STORAGE
    if (value) {
        error_setg(errp, "SCSI support is not present in this build");
        return;
    }
#else
    (void)errp;
#endif

    s->lsi_enabled = value;
}

static bool ia64_vpc_get_ide(Object *obj, Error **errp)
{
    IA64VpcMachineState *s = IA64_VPC_MACHINE(obj);

    (void)errp;

    return s->ide_enabled;
}

static void ia64_vpc_set_ide(Object *obj, bool value, Error **errp)
{
    IA64VpcMachineState *s = IA64_VPC_MACHINE(obj);

#ifndef CONFIG_IA64_VPC_STORAGE
    if (value) {
        error_setg(errp, "IDE support is not present in this build");
        return;
    }
#else
    (void)errp;
#endif

    s->ide_enabled = value;
}

static bool ia64_vpc_get_agp(Object *obj, Error **errp)
{
    IA64VpcMachineState *s = IA64_VPC_MACHINE(obj);

    (void)errp;

    return s->agp_enabled;
}

static void ia64_vpc_set_agp(Object *obj, bool value, Error **errp)
{
    IA64VpcMachineState *s = IA64_VPC_MACHINE(obj);

    (void)errp;

    s->agp_enabled = value;
}

static bool ia64_vpc_get_firmware_ide_dma(Object *obj, Error **errp)
{
    IA64VpcMachineState *s = IA64_VPC_MACHINE(obj);

    (void)errp;

    return s->firmware_ide_dma;
}

static void ia64_vpc_set_firmware_ide_dma(Object *obj, bool value,
                                          Error **errp)
{
    IA64VpcMachineState *s = IA64_VPC_MACHINE(obj);

#ifndef CONFIG_IA64_VPC_STORAGE
    if (value) {
        error_setg(errp,
                   "firmware IDE DMA support is not present in this build");
        return;
    }
#else
    (void)errp;
#endif

    s->firmware_ide_dma = value;
}

static const struct {
    const char *name;
    uint64_t bit;
} ia64_vpc_fw_quirks[] = {
    { "split-page",          IA64_FW_QUIRK_LOADER_SPLIT_PAGE },
    { "low-boundaries",      IA64_FW_QUIRK_LOW_BOUNDARIES },
    { "low-anchor",          IA64_FW_QUIRK_LOW_ANCHOR },
    { "anchor-version-sniff", IA64_FW_QUIRK_ANCHOR_VERSION_SNIFF },
    { "2g-scratch",          IA64_FW_QUIRK_SCRATCH_2G },
    { "pal-8k-page",         IA64_FW_QUIRK_PAL_8K_PAGE },
    { "acpi-low-island",     IA64_FW_QUIRK_ACPI_LOW_ISLAND },
};

/*
 * Quirks disabled by default (plans/firmware-rework-plan.md; re-enable any
 * of them with fw-quirks=+name):
 *  - acpi-low-island: retired in phase 2.2 - ACPI staging now sits in the
 *    RAM-top firmware block, validated on 2462/XP2600/XP2002-installer/
 *    checked-3790.
 *  - 2g-scratch: retired in phase 2.3 - experiment E2 showed the XP 2600
 *    SMP deadlock it once papered over no longer reproduces (3/3 SMP boots
 *    to desktop with the page removed, control green).
 *  - low-boundaries: retired in phase 2.3 on the relocated map - the
 *    32/48/64/80 MB no-coalesce boundaries' motivating lanes (XP 2600
 *    ntoskrnl-missing class; 2003 SP1 installer error 16) pass without
 *    them.
 *  - low-anchor + anchor-version-sniff: retired in phase 2.4 - on the
 *    relocated map the XP-era MiInitMachineDependent reset class no
 *    longer fires (XP 2600 UP+SMP desktops, 2462 logon, XP 2002 and
 *    2003 SP1 installers to text setup, XP SP1 desktop, all A/B'd with
 *    the anchor off).  With the sniff gone the map is no longer
 *    guest-build-specific.
 *
 *  NOT retired: split-page - the XP 2002 installer wedges in kernel-init
 *  memmove without it (see the expected-state ledger); pal-8k-page.
 */
#define IA64_VPC_FW_QUIRK_DEFAULT_DISABLE \
    (IA64_FW_QUIRK_ACPI_LOW_ISLAND | IA64_FW_QUIRK_SCRATCH_2G | \
     IA64_FW_QUIRK_LOW_BOUNDARIES | IA64_FW_QUIRK_LOW_ANCHOR | \
     IA64_FW_QUIRK_ANCHOR_VERSION_SNIFF)

static char *ia64_vpc_get_fw_quirks(Object *obj, Error **errp)
{
    IA64VpcMachineState *s = IA64_VPC_MACHINE(obj);
    GString *out = g_string_new(NULL);
    size_t i;

    (void)errp;
    for (i = 0; i < ARRAY_SIZE(ia64_vpc_fw_quirks); i++) {
        if (s->fw_map_quirk_disable & ia64_vpc_fw_quirks[i].bit) {
            if (out->len != 0) {
                g_string_append_c(out, ',');
            }
            g_string_append_c(out, '-');
            g_string_append(out, ia64_vpc_fw_quirks[i].name);
        }
    }
    if (out->len == 0) {
        g_string_append(out, "default");
    }
    return g_string_free(out, false);
}

static void ia64_vpc_set_fw_quirks(Object *obj, const char *value,
                                   Error **errp)
{
    IA64VpcMachineState *s = IA64_VPC_MACHINE(obj);
    uint64_t disable = s->fw_map_quirk_disable;
    /*
     * -machine option parsing consumes commas, so a single fw-quirks value
     * uses ':' between names; alternatively repeat fw-quirks= per name
     * (the setter accumulates).
     */
    g_auto(GStrv) tokens = g_strsplit_set(value, ",:", 0);
    size_t i;
    char **tok;

    for (tok = tokens; *tok != NULL; tok++) {
        const char *name = *tok;
        bool off;

        if (name[0] == '\0') {
            continue;
        }
        if (g_strcmp0(name, "default") == 0) {
            disable = IA64_VPC_FW_QUIRK_DEFAULT_DISABLE;
            continue;
        }
        off = name[0] == '-';
        if (name[0] == '-' || name[0] == '+') {
            name++;
        }
        for (i = 0; i < ARRAY_SIZE(ia64_vpc_fw_quirks); i++) {
            if (g_strcmp0(name, ia64_vpc_fw_quirks[i].name) == 0) {
                if (off) {
                    disable |= ia64_vpc_fw_quirks[i].bit;
                } else {
                    disable &= ~ia64_vpc_fw_quirks[i].bit;
                }
                break;
            }
        }
        if (i == ARRAY_SIZE(ia64_vpc_fw_quirks)) {
            error_setg(errp, "unknown firmware map quirk '%s'", name);
            return;
        }
    }
    s->fw_map_quirk_disable = disable;
}

static char *ia64_vpc_get_firmware_console(Object *obj, Error **errp)
{
    IA64VpcMachineState *s = IA64_VPC_MACHINE(obj);

    (void)errp;

    return g_strdup(s->firmware_console == IA64_FW_CONSOLE_VGA ?
                    "vga" : "serial");
}

static void ia64_vpc_set_firmware_console(Object *obj, const char *value,
                                          Error **errp)
{
    IA64VpcMachineState *s = IA64_VPC_MACHINE(obj);

    if (g_strcmp0(value, "serial") == 0) {
        s->firmware_console = IA64_FW_CONSOLE_SERIAL;
        return;
    }
    if (g_strcmp0(value, "vga") == 0) {
#ifndef CONFIG_IA64_VPC_GRAPHICS
        error_setg(errp, "VGA support is not present in this build");
#else
        s->firmware_console = IA64_FW_CONSOLE_VGA;
#endif
        return;
    }

    error_setg(errp, "firmware-console must be 'serial' or 'vga'");
}

static void ia64_vpc_get_boot_timeout(Object *obj, Visitor *v, const char *name,
                                      void *opaque, Error **errp)
{
    IA64VpcMachineState *s = IA64_VPC_MACHINE(obj);
    uint16_t value = s->firmware_boot_timeout;

    visit_type_uint16(v, name, &value, errp);
}

static void ia64_vpc_set_boot_timeout(Object *obj, Visitor *v, const char *name,
                                      void *opaque, Error **errp)
{
    IA64VpcMachineState *s = IA64_VPC_MACHINE(obj);
    uint16_t value;

    if (!visit_type_uint16(v, name, &value, errp)) {
        return;
    }
    s->firmware_boot_timeout = value;
}

static char *ia64_vpc_get_vga(Object *obj, Error **errp)
{
    IA64VpcMachineState *s = IA64_VPC_MACHINE(obj);

    (void)errp;

    return g_strdup(s->vga_model ? s->vga_model : "rage128");
}

static void ia64_vpc_set_vga(Object *obj, const char *value, Error **errp)
{
    IA64VpcMachineState *s = IA64_VPC_MACHINE(obj);

    if (g_strcmp0(value, "rage128") != 0 &&
        g_strcmp0(value, "mach64") != 0 &&
        g_strcmp0(value, "nv15gl") != 0 &&
        g_strcmp0(value, "std") != 0) {
        error_setg(errp,
                   "vga must be 'rage128', 'mach64', 'nv15gl' or 'std'");
        return;
    }
    g_free(s->vga_model);
    s->vga_model = g_strdup(value);
}

/* The chipset personality is fixed by the concrete machine type's class. */
static bool ia64_vpc_chipset_is_zx1(const IA64VpcMachineState *s)
{
    IA64VpcMachineClass *imc = IA64_VPC_MACHINE_GET_CLASS(s);

    return imc->chipset_profile == IA64_FW_CHIPSET_ZX1;
}

static char *ia64_vpc_get_alat(Object *obj, Error **errp)
{
    IA64VpcMachineState *s = IA64_VPC_MACHINE(obj);

    (void)errp;

    return g_strdup(s->alat_full ? "full" : "zero");
}

static void ia64_vpc_set_alat(Object *obj, const char *value, Error **errp)
{
    IA64VpcMachineState *s = IA64_VPC_MACHINE(obj);

    if (g_strcmp0(value, "zero") == 0) {
        s->alat_full = false;
        return;
    }
    if (g_strcmp0(value, "full") == 0) {
        s->alat_full = true;
        return;
    }

    error_setg(errp, "alat must be 'zero' or 'full'");
}

static void ia64_vpc_acpi_update_sci(ACPIREGS *ar)
{
    IA64VpcMachineState *s = container_of(ar, IA64VpcMachineState,
                                          acpi_regs);

    acpi_update_sci(ar, s->acpi_sci_irq);
}

static uint64_t ia64_vpc_acpi_reset_read(void *opaque, hwaddr addr,
                                         unsigned size)
{
    (void)opaque;
    (void)addr;
    (void)size;
    return 0;
}

static void ia64_vpc_acpi_reset_write(void *opaque, hwaddr addr,
                                      uint64_t value, unsigned size)
{
    (void)opaque;
    if (addr == 0 && size == 1 &&
        (value & 0xff) == IA64_ACPI_PM_RESET_VALUE) {
        qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
    }
}

static const MemoryRegionOps ia64_vpc_acpi_reset_ops = {
    .read = ia64_vpc_acpi_reset_read,
    .write = ia64_vpc_acpi_reset_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
};

static void ia64_vpc_init_acpi_pm(IA64VpcMachineState *s,
                                  DeviceState *iosapic,
                                  MemoryRegion *pci_io)
{
    s->acpi_sci_irq = qdev_get_gpio_in(iosapic, IA64_ACPI_SCI_IRQ);

    memory_region_init(&s->acpi_pm, OBJECT(s), "ia64-acpi-pm",
                       IA64_ACPI_PM_IO_SIZE);
    memory_region_add_subregion(pci_io, IA64_ACPI_PM_IO_BASE,
                                &s->acpi_pm);

    acpi_pm1_evt_init(&s->acpi_regs, ia64_vpc_acpi_update_sci,
                      &s->acpi_pm);
    acpi_pm1_cnt_init(&s->acpi_regs, &s->acpi_pm,
                      false, false, 0, true);
    acpi_pm_tmr_init(&s->acpi_regs, ia64_vpc_acpi_update_sci,
                     &s->acpi_pm);
    memory_region_init_io(&s->acpi_reset, OBJECT(s),
                          &ia64_vpc_acpi_reset_ops, s,
                          "ia64-acpi-reset", 1);
    memory_region_add_subregion(&s->acpi_pm,
                                IA64_ACPI_PM_RESET_OFFSET,
                                &s->acpi_reset);

    /*
     * acpi_update_sci() always folds in GPE status.  The current platform
     * exposes no GPE block to the guest, but the shared ACPI core still needs
     * backing storage for that internal zero-valued contribution.
     */
    acpi_gpe_init(&s->acpi_regs, 2);
}

static void ia64_vpc_powerdown_req(Notifier *n, void *opaque)
{
    IA64VpcMachineState *s = container_of(n, IA64VpcMachineState,
                                          powerdown_notifier);

    (void)opaque;

    if (s->acpi_regs.pm1.evt.en & ACPI_BITMASK_POWER_BUTTON_ENABLE) {
        acpi_pm1_evt_power_down(&s->acpi_regs);
    } else {
        /* Avoid making QEMU's powerdown action a no-op before ACPI is armed. */
        qemu_system_shutdown_request(SHUTDOWN_CAUSE_GUEST_SHUTDOWN);
    }
}

#ifdef CONFIG_IA64_VPC_GRAPHICS
static const VMStateDescription vmstate_ia64_int10_registers = {
    .name = "ia64-vpc/int10-registers",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT16(ax, IA64Int10Registers),
        VMSTATE_UINT16(bx, IA64Int10Registers),
        VMSTATE_UINT16(cx, IA64Int10Registers),
        VMSTATE_UINT16(dx, IA64Int10Registers),
        VMSTATE_UINT16(di, IA64Int10Registers),
        VMSTATE_UINT16(es, IA64Int10Registers),
        VMSTATE_END_OF_LIST()
    }
};
#endif

static int ia64_vpc_post_load(void *opaque, int version_id)
{
    IA64VpcMachineState *s = opaque;
    uint16_t pm_enable = s->acpi_regs.pm1.evt.en;

#ifdef CONFIG_IA64_VPC_GRAPHICS
    if (s->int10_response_length > sizeof(s->int10_response) ||
        s->int10_response_offset > s->int10_response_length ||
        s->int10_input_signature_words > 2) {
        return -EINVAL;
    }
#endif

    qemu_system_wakeup_enable(
        QEMU_WAKEUP_REASON_RTC,
        (pm_enable & ACPI_BITMASK_RT_CLOCK_ENABLE) != 0);
    qemu_system_wakeup_enable(
        QEMU_WAKEUP_REASON_PMTIMER,
        (pm_enable & ACPI_BITMASK_TIMER_ENABLE) != 0);
    ia64_vpc_acpi_update_sci(&s->acpi_regs);
    return 0;
}

static const VMStateDescription vmstate_ia64_vpc = {
    .name = "ia64-vpc",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = ia64_vpc_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT64(watchdog_timeout, IA64VpcMachineState),
        VMSTATE_UINT64(watchdog_code, IA64VpcMachineState),
        VMSTATE_TIMER_PTR(watchdog_timer, IA64VpcMachineState),
        VMSTATE_UINT8_ARRAY(nvram_data, IA64VpcMachineState,
                            IA64_NVRAM_SIZE),

        VMSTATE_UINT16(acpi_regs.pm1.evt.sts, IA64VpcMachineState),
        VMSTATE_UINT16(acpi_regs.pm1.evt.en, IA64VpcMachineState),
        VMSTATE_UINT16(acpi_regs.pm1.cnt.cnt, IA64VpcMachineState),
        VMSTATE_TIMER_PTR(acpi_regs.tmr.timer, IA64VpcMachineState),
        VMSTATE_INT64(acpi_regs.tmr.overflow_time, IA64VpcMachineState),
        VMSTATE_BUFFER_POINTER_UNSAFE(acpi_regs.gpe.sts,
                                      IA64VpcMachineState, 1, 2),
        VMSTATE_BUFFER_POINTER_UNSAFE(acpi_regs.gpe.en,
                                      IA64VpcMachineState, 1, 2),

#ifdef CONFIG_IA64_VPC_GRAPHICS
        VMSTATE_STRUCT(int10_request, IA64VpcMachineState, 1,
                       vmstate_ia64_int10_registers, IA64Int10Registers),
        VMSTATE_STRUCT(int10_result, IA64VpcMachineState, 1,
                       vmstate_ia64_int10_registers, IA64Int10Registers),
        VMSTATE_UINT32(int10_input_signature, IA64VpcMachineState),
        VMSTATE_UINT8_ARRAY(int10_response, IA64VpcMachineState, 512),
        VMSTATE_UINT16(int10_response_length, IA64VpcMachineState),
        VMSTATE_UINT16(int10_response_offset, IA64VpcMachineState),
        VMSTATE_UINT8(int10_input_signature_words, IA64VpcMachineState),
        VMSTATE_UINT8(int10_dpms_state, IA64VpcMachineState),
        VMSTATE_UINT8(int10_legacy_mode, IA64VpcMachineState),
        VMSTATE_UINT8(int10_legacy_columns, IA64VpcMachineState),
#endif
        VMSTATE_END_OF_LIST()
    }
};

static uint64_t ia64_vpc_lsapic_read(void *opaque, hwaddr addr,
                                       unsigned size)
{
    IA64VpcMachineState *s = opaque;

    if (addr == IA64_PIB_INTA_OFFSET && size == 1) {
        /*
         * Interrupt-acknowledge byte.  When an ExtINT is delivered (IVR reads
         * 0) firmware reads this location to run the INTA cycle against the
         * external 8259 PIC and obtain the real 8-bit vector.  The PIC is the
         * pair inside the south bridge where the platform has one; failing
         * that, the machine-wide legacy PIC.  With neither, the cycle reads
         * back 0.
         */
        if (s != NULL && s->ifb != NULL) {
            int vector = intel_82468gx_ifb_pic_read_irq(s->ifb);

            return vector < 0 ? 0 : (uint64_t)vector;
        }
        if (isa_pic != NULL) {
            return pic_read_irq(isa_pic);
        }
        return 0;
    }
    return 0;
}

static void ia64_vpc_lsapic_write(void *opaque, hwaddr addr,
                                    uint64_t value, unsigned size)
{
    CPUState *cs;
    unsigned delivery;
    uint8_t id;
    uint8_t eid;
    uint8_t vector;

    (void)opaque;
    /*
     * The upper half of the Processor Interrupt Block contains the XTP byte.
     * XTP is a platform hint; systems without XTP support must still accept
     * and discard the one-byte store.
     */
    if (addr == IA64_PIB_XTP_OFFSET && size == 1) {
        return;
    }

    if (addr >= IA64_PIB_IPI_LIMIT || size != 8 || (addr & 7)) {
        return;
    }

    /*
     * The lower half of the Processor Interrupt Block is the IPI delivery
     * region.  The address selects the target processor and the low data byte
     * carries the interrupt vector for INT delivery messages.
     */
    id = (addr >> 12) & 0xff;
    eid = (addr >> 4) & 0xff;
    delivery = (value >> 8) & 7;
    switch (delivery) {
    case IA64_SAPIC_DELIVERY_INT:
        vector = value & 0xff;
        if (!ia64_external_interrupt_vector_valid(vector)) {
            return;
        }
        break;
    case IA64_SAPIC_DELIVERY_NMI:
        vector = 2;
        break;
    case IA64_SAPIC_DELIVERY_EXTINT:
        vector = 0;
        break;
    default:
        return;
    }

    cs = ia64_cpu_by_sapic_id(id, eid);
    if (cs == NULL) {
        return;
    }

    ia64_sapic_set_irq(cs, vector);
}

static const MemoryRegionOps ia64_vpc_lsapic_ops = {
    .read = ia64_vpc_lsapic_read,
    .write = ia64_vpc_lsapic_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 8,
    },
    .impl = {
        .min_access_size = 1,
        .max_access_size = 8,
    },
};

static void ia64_vpc_map_lsapic(IA64VpcMachineState *s)
{
    if (s->lsapic_mmio != NULL) {
        return;
    }

    s->lsapic_mmio = g_new(MemoryRegion, 1);
    memory_region_init_io(s->lsapic_mmio, OBJECT(s),
                          &ia64_vpc_lsapic_ops, s,
                          "ia64-vpc.local-sapic",
                          IA64_LOCAL_SAPIC_SIZE);
    memory_region_add_subregion(get_system_memory(), IA64_LOCAL_SAPIC_PA,
                                s->lsapic_mmio);
}

static bool ia64_vpc_map_firmware_address_space(IA64VpcMachineState *s,
                                                Error **errp)
{
    Error *local_err = NULL;

    /*
     * IA-64 reserves the top 16 MiB below 4 GiB for PAL/SAL firmware
     * resources.  Decode it so firmware identity mappings can use the
     * platform address space directly.
     */
    memory_region_init_ram(&s->firmware_space, NULL,
                           "ia64-firmware-address-space",
                           IA64_FIRMWARE_ADDRESS_SPACE_SIZE, &local_err);
    if (local_err != NULL) {
        error_propagate(errp, local_err);
        return false;
    }
    memory_region_add_subregion_overlap(get_system_memory(),
                                        IA64_FIRMWARE_ADDRESS_SPACE_BASE,
                                        &s->firmware_space, 1);
    return true;
}

static uint64_t ia64_vpc_map_ram_alias(IA64VpcMachineState *s,
                                       hwaddr guest_base,
                                       uint64_t backing_offset,
                                       uint64_t remaining,
                                       uint64_t capacity,
                                       const char *name)
{
    MachineState *machine = MACHINE(s);
    MemoryRegion *alias;
    uint64_t size = MIN(remaining, capacity);

    if (size == 0) {
        return 0;
    }

    g_assert(s->ram_alias_count < ARRAY_SIZE(s->ram_aliases));
    alias = g_new(MemoryRegion, 1);
    s->ram_aliases[s->ram_alias_count++] = alias;
    memory_region_init_alias(alias, OBJECT(s), name, machine->ram,
                             backing_offset, size);
    memory_region_add_subregion(get_system_memory(), guest_base, alias);
    return size;
}

static void ia64_vpc_map_ram(IA64VpcMachineState *s)
{
    MachineState *machine = MACHINE(s);
    uint64_t remaining = machine->ram_size;
    uint64_t offset = 0;
    uint64_t size;

    if (machine->ram == NULL) {
        return;
    }

    /*
     * Real 460GX layout: DRAM is contiguous from 0 up to the top-of-memory
     * MMIO gap (the PCI aperture just below the fixed chipset/SAPIC/firmware
     * region at [0xFE000000, 4 GiB)), and only RAM displaced by that gap is
     * remapped above 4 GiB.  There is no DRAM island between the aperture and
     * the chipset region.  With the IOSAPIC no longer parked at 2 GiB the low
     * band is a single unbroken run, which also avoids the fragmented
     * single-DMA-zone layout that Linux 2.6.8 IA-64 mishandled.
     *
     * The zx1 machine additionally carves a DRAM hole for the SBA "safe IOVA
     * space" [IA64_SBA_IOVA_BASE, IA64_SBA_IOVA_END) (1-2 GiB): the RAM that
     * would sit there is shifted up past IA64_SBA_IOVA_END, so the enabled IOVA
     * window overlaps no DRAM (see IA64_SBA_IOVA_BASE in ia64_vpc_abi.h).
     *
     * The hole is only carved once installed RAM exceeds the PCI aperture
     * (IA64_LOW_RAM_LIMIT ~= 3.72 GiB), i.e. exactly when there is already RAM
     * displaced above 4 GiB.  In that regime the low band fills to the aperture
     * regardless of the hole, so the firmware's aperture-relative self-placement
     * (image, CPU-assist, SRAT/SMBIOS top) is unaffected and the two maps stay
     * trivially consistent.  For a guest at or below the aperture the layout is
     * identical to 460gx (a single contiguous low run) -- carving the hole there
     * would move the top of low RAM and the firmware image with it, which needs
     * a hole-aware low_ram_end the firmware does not yet compute.  See
     * plans/zx1-chipset-port-plan.md for the mid-range follow-up.
     *
     * Keep this in lockstep with fw_init_guest_high_ram_ranges() +
     * efi_add_low_ram_band() in roms/ia64-firmware/.
     */
    if (ia64_vpc_chipset_is_zx1(s) && remaining > IA64_LOW_RAM_LIMIT) {
        size = ia64_vpc_map_ram_alias(s, 0, offset, remaining,
                                      IA64_SBA_IOVA_BASE,
                                      "ia64-vpc.low-ram-below-iova");
        offset += size;
        remaining -= size;
        size = ia64_vpc_map_ram_alias(s, IA64_SBA_IOVA_END, offset, remaining,
                                      IA64_LOW_RAM_LIMIT - IA64_SBA_IOVA_END,
                                      "ia64-vpc.low-ram-above-iova");
        offset += size;
        remaining -= size;
    } else {
        size = ia64_vpc_map_ram_alias(s, 0, offset, remaining,
                                      IA64_LOW_RAM_LIMIT,
                                      "ia64-vpc.low-ram");
        offset += size;
        remaining -= size;
    }

    ia64_vpc_map_ram_alias(s, IA64_HIGH_RAM_AFTER_FIRMWARE_BASE,
                           offset, remaining, remaining,
                           "ia64-vpc.high-ram-above-4g");
}

static void ia64_vpc_write_firmware_handoff(IA64VpcMachineState *s)
{
    MachineState *machine = MACHINE(s);
    IA64VpcHandoff handoff = { 0 };
    bool debug_port_present = debug_port_get_chardev() != NULL;

    _Static_assert(sizeof(IA64VpcHandoff) == 128,
                   "IA-64 firmware handoff ABI size changed");
    _Static_assert(offsetof(IA64VpcHandoff, ProcessorCount) == 64,
                   "IA-64 firmware handoff CPU count offset changed");
    _Static_assert(offsetof(IA64VpcHandoff, NvramPersistent) == 72,
                   "IA-64 firmware handoff NVRAM offset changed");
    _Static_assert(offsetof(IA64VpcHandoff, SocketCount) == 80,
                   "IA-64 firmware handoff socket count offset changed");
    _Static_assert(offsetof(IA64VpcHandoff, CoresPerSocket) == 88,
                   "IA-64 firmware handoff core count offset changed");
    _Static_assert(offsetof(IA64VpcHandoff, ThreadsPerCore) == 96,
                   "IA-64 firmware handoff thread count offset changed");

    handoff.Magic = cpu_to_le64(IA64_FW_HANDOFF_MAGIC);
    handoff.Version = cpu_to_le64(IA64_FW_HANDOFF_VERSION);
    handoff.RamSize = cpu_to_le64(machine->ram_size);
    handoff.ConsolePolicy = cpu_to_le64(s->firmware_console);
    handoff.IdeDmaEnabled = cpu_to_le64(s->firmware_ide_dma);
    handoff.DebugPortFlags = cpu_to_le64(
        debug_port_present ? IA64_FW_DEBUG_PORT_PRESENT : 0);
    handoff.DebugPortBase = cpu_to_le64(
        debug_port_present ? IA64_DEBUG_UART_BASE : 0);
    handoff.I8042Enabled = cpu_to_le64(s->i8042_enabled);
    handoff.ProcessorCount = cpu_to_le64(machine->smp.cpus);
    handoff.NvramPersistent = cpu_to_le64(
        s->nvram_resolved_path != NULL);
    handoff.SocketCount = cpu_to_le64(machine->smp.sockets);
    handoff.CoresPerSocket = cpu_to_le64(machine->smp.cores);
    handoff.ThreadsPerCore = cpu_to_le64(machine->smp.threads);
    handoff.MapQuirkDisable = cpu_to_le64(s->fw_map_quirk_disable);
    handoff.BootTimeout = cpu_to_le64(s->firmware_boot_timeout);
    /*
     * The machine type fixes the firmware personality, independent of the CPU
     * model: the 460gx machine selects the Intel 460GX and the zx1 machine the
     * HP zx1.  This retired the old CPU-keyed default (which gave the default
     * Itanium 2 machine a half-modelled E8870 personality); users pair machine
     * and CPU at their own risk (460GX suits Merced, zx1 suits Itanium 2).
     * DERIVE remains only as the firmware's fallback for a pre-version-14
     * handoff.
     */
    handoff.ChipsetProfile =
        cpu_to_le64(IA64_VPC_MACHINE_GET_CLASS(s)->chipset_profile);
    cpu_physical_memory_write(IA64_FW_HANDOFF_ADDR, &handoff,
                              sizeof(handoff));
}

/*
 * Program a device's interrupt line from the interrupt block its root owns.
 * Devices on bus 0, and everything on zx1 (where both roots wire-OR into one
 * block of four), use IA64_PCI_INTX_GSI_BASE.  Each 460GX expander root has
 * its own block, so a device behind one must report a line from that block --
 * the line has to agree with the root's ACPI _PRT and with the input the
 * expander's GPIO actually drives.
 */
static void ia64_vpc_configure_pci_irq_on_root(PCIDevice *pci_dev,
                                               unsigned int gsi_base)
{
    uint8_t pin;

    if (pci_dev == NULL) {
        return;
    }

    pin = pci_dev->config[PCI_INTERRUPT_PIN];
    if (pin >= 1 && pin <= PCI_NUM_PINS) {
        unsigned int line = gsi_base +
            (ia64_pci_route_intx_gsi(pci_dev->devfn, pin - 1) -
             IA64_PCI_INTX_GSI_BASE);

        pci_default_write_config(pci_dev, PCI_INTERRUPT_LINE, line, 1);
    }
}

static void ia64_vpc_configure_pci_irq(PCIDevice *pci_dev)
{
    ia64_vpc_configure_pci_irq_on_root(pci_dev, IA64_PCI_INTX_GSI_BASE);
}

/* The interrupt block owned by the root that carries bus @bus. */
static unsigned int ia64_vpc_root_gsi_base(const IA64VpcMachineState *s,
                                           uint8_t bus)
{
    if (ia64_vpc_chipset_is_zx1(s)) {
        return IA64_PCI_INTX_GSI_BASE;
    }
    return IA64_PCI_INTX_GSI_BASE + bus * IA64_PCI_INTX_LINES;
}

static void ia64_vpc_configure_ahci(PCIDevice *pci_dev)
{
    if (pci_dev == NULL) {
        return;
    }

    pci_default_write_config(pci_dev, PCI_BASE_ADDRESS_4,
                             IA64_AHCI_IDP_IO_BASE, 4);
    pci_default_write_config(pci_dev, PCI_BASE_ADDRESS_5,
                             IA64_AHCI_MMIO_PCI_BASE, 4);
    pci_default_write_config(pci_dev, PCI_COMMAND,
                             PCI_COMMAND_IO | PCI_COMMAND_MEMORY |
                             PCI_COMMAND_MASTER, 2);
}

static void ia64_vpc_configure_audio(PCIDevice *pci_dev)
{
    if (pci_dev == NULL) {
        return;
    }

    pci_default_write_config(pci_dev, PCI_BASE_ADDRESS_0,
                             IA64_CS4281_BA0_PCI_BASE, 4);
    pci_default_write_config(pci_dev, PCI_BASE_ADDRESS_1,
                             IA64_CS4281_BA1_PCI_BASE, 4);
    pci_default_write_config(pci_dev, PCI_COMMAND,
                             PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER, 2);
}

/*
 * The QLogic is the default adapter and always holds the SCSI seat when it
 * is present, so its BARs come out of the first WXB root's window.
 */
static void ia64_vpc_configure_isp(PCIDevice *pci_dev)
{
    if (pci_dev == NULL) {
        return;
    }

    pci_default_write_config(pci_dev, PCI_BASE_ADDRESS_0,
                             IA64_SCSI_SEAT_IO_BASE |
                             PCI_BASE_ADDRESS_SPACE_IO, 4);
    pci_default_write_config(pci_dev, PCI_BASE_ADDRESS_1,
                             IA64_SCSI_SEAT_MMIO_PCI_BASE, 4);
    pci_default_write_config(pci_dev, PCI_COMMAND,
                             PCI_COMMAND_IO | PCI_COMMAND_MEMORY |
                             PCI_COMMAND_MASTER, 2);
}

static void ia64_vpc_configure_ohci(PCIDevice *pci_dev)
{
    if (pci_dev == NULL) {
        return;
    }

    pci_default_write_config(pci_dev, PCI_BASE_ADDRESS_0,
                             IA64_OHCI_MMIO_PCI_BASE, 4);
    pci_default_write_config(pci_dev, PCI_COMMAND,
                             PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER, 2);
}

static void ia64_vpc_configure_uhci(PCIDevice *pci_dev)
{
    if (pci_dev == NULL) {
        return;
    }

    pci_default_write_config(pci_dev, PCI_BASE_ADDRESS_4,
                             IA64_UHCI_IO_BASE, 4);
    pci_default_write_config(pci_dev, PCI_COMMAND,
                             PCI_COMMAND_IO | PCI_COMMAND_MASTER, 2);
}

/*
 * Whether the machine carries the modelled 82468GX south bridge, and with it
 * an IDE controller that is part of the board rather than an option.  zx1 is
 * a different platform, and realfw mode models a south bridge of its own.
 */
static bool ia64_vpc_has_south_bridge(const IA64VpcMachineState *s)
{
    return !ia64_vpc_chipset_is_zx1(s) && s->realfw_path == NULL;
}

static void ia64_vpc_configure_ifb_ide(PCIDevice *pci_dev)
{
    if (pci_dev == NULL) {
        return;
    }

    pci_default_write_config(pci_dev, PCI_BASE_ADDRESS_4,
                             IA64_IFB_IDE_BMDMA_IO_BASE |
                             PCI_BASE_ADDRESS_SPACE_IO, 4);
    pci_default_write_config(pci_dev, PCI_COMMAND,
                             PCI_COMMAND_IO | PCI_COMMAND_MASTER, 2);
}

static void ia64_vpc_configure_ifb_smbus(PCIDevice *pci_dev)
{
    if (pci_dev == NULL) {
        return;
    }

    pci_default_write_config(pci_dev, PCI_BASE_ADDRESS_4,
                             IA64_IFB_SMBUS_IO_BASE |
                             PCI_BASE_ADDRESS_SPACE_IO, 4);
    pci_default_write_config(pci_dev, PCI_COMMAND, PCI_COMMAND_IO, 2);
}

/*
 * The LSI holds the seat only when the QLogic is off; with both adapters
 * present it parks on the second WXB root and its BARs follow it there.
 */
static void ia64_vpc_configure_lsi(IA64VpcMachineState *s, PCIDevice *pci_dev)
{
    uint32_t io_base;
    uint64_t mmio_base;

    if (pci_dev == NULL) {
        return;
    }

    io_base = s->isp_enabled ? IA64_SCSI_PARK_IO_BASE : IA64_SCSI_SEAT_IO_BASE;
    mmio_base = s->isp_enabled ? IA64_SCSI_PARK_MMIO_PCI_BASE :
                                 IA64_SCSI_SEAT_MMIO_PCI_BASE;

    pci_default_write_config(pci_dev, PCI_BASE_ADDRESS_0, io_base, 4);
    pci_default_write_config(pci_dev, PCI_BASE_ADDRESS_1, mmio_base, 4);
    pci_default_write_config(pci_dev, PCI_BASE_ADDRESS_2,
                             mmio_base + IA64_LSI_RAM_BAR_OFFSET, 4);
    pci_default_write_config(pci_dev, PCI_COMMAND,
                             PCI_COMMAND_IO | PCI_COMMAND_MEMORY |
                             PCI_COMMAND_MASTER, 2);
}

/*
 * Give the stock VGA BIOS the ATI data blocks a native Rage 128 driver looks
 * for.  Windows' videoprt reads the image through the PCI ROM BAR, and the
 * shipped vgabios-ati.bin is a SeaVGABIOS build with none of ATI's tables:
 * the signature " 761295520" that ATI drivers validate the ROM by occurs
 * nowhere in it, so Whistler build 2462's miniport leaves its BIOS table
 * pointer NULL and bugchecks 0x1E dereferencing it.
 *
 * The blocks written here are ours, not ATI's - the layout is the documented
 * one (signature at 30h, header pointer at 48h, PLL pointer at header+30h)
 * and the clock parameters are the Rage 128 Pro's published values, which is
 * also what the synthesised INT 10h ROM publishes.  Nothing is copied out of
 * a retail BIOS image.
 *
 * A user-supplied romfile that already carries the signature is left strictly
 * alone.
 */
static void ia64_vpc_install_ati_rom_tables(PCIDevice *pci_dev)
{
    static const char ati_signature[] = " 761295520";
    uint8_t *rom;
    uint64_t rom_size;
    uint32_t declared;
    uint32_t hdr;
    uint32_t pll;
    uint32_t pcir;
    uint32_t i;
    uint8_t checksum = 0;

    if (pci_get_word(pci_dev->config + PCI_VENDOR_ID) !=
            IA64_ATI_VENDOR_ID ||
        pci_get_word(pci_dev->config + PCI_DEVICE_ID) !=
            IA64_ATI_RAGE128_PF_ID) {
        return;
    }
    if (pci_dev->io_regions[PCI_ROM_SLOT].size == 0 || !pci_dev->has_rom) {
        return;
    }

    rom = memory_region_get_ram_ptr(&pci_dev->rom);
    rom_size = memory_region_size(&pci_dev->rom);
    if (rom == NULL || rom_size < 0x400 || rom[0] != 0x55 || rom[1] != 0xaa) {
        return;
    }

    declared = (uint32_t)rom[2] * 512U;
    if (declared == 0 || declared > rom_size) {
        return;
    }

    /* A real ATI image already has everything; do not touch it. */
    for (i = 0; i + sizeof(ati_signature) - 1 <= declared; i++) {
        if (memcmp(rom + i, ati_signature,
                   sizeof(ati_signature) - 1) == 0) {
            return;
        }
    }

    /* 30h..47h is padding in the shipped image; refuse if that changes. */
    for (i = 0x30; i < 0x48; i++) {
        if (rom[i] != 0) {
            return;
        }
    }

    hdr = declared;
    pll = hdr + 0x40U;
    if (pll + 0x20U > rom_size) {
        return;
    }

    memcpy(rom + 0x30, ati_signature, sizeof(ati_signature) - 1);
    stw_le_p(rom + 0x48, hdr);
    memset(rom + hdr, 0, 0x60);
    stw_le_p(rom + hdr + 0x30, pll);
    stw_le_p(rom + pll + 0x08, IA64_ATI_PLL_XCLK);
    stw_le_p(rom + pll + 0x0e, IA64_ATI_PLL_REFERENCE_FREQ);
    stw_le_p(rom + pll + 0x10, IA64_ATI_PLL_REFERENCE_DIV);
    stl_le_p(rom + pll + 0x12, IA64_ATI_PLL_MIN_FREQ);
    stl_le_p(rom + pll + 0x16, IA64_ATI_PLL_MAX_FREQ);

    /* Grow the declared image so a bounds-checking parser sees the tables. */
    declared = ROUND_UP(pll + 0x20U, 512U);
    if (declared > rom_size || declared / 512U > 0xffU) {
        return;
    }
    rom[2] = (uint8_t)(declared / 512U);
    pcir = lduw_le_p(rom + 0x18);
    if (pcir != 0 && pcir + 0x18U <= declared &&
        memcmp(rom + pcir, "PCIR", 4) == 0) {
        stw_le_p(rom + pcir + 0x10, declared / 512U);
        /*
         * The shipped image is a SeaVGABIOS build whose PCIR data structure
         * still advertises 1002:5159 (Radeon RV100).  EFI 1.10 §12.4 requires
         * the PCIR vendor/device ID to match the adapter's configuration
         * header, and a driver that validates the ROM against the device it
         * bound to will reject an image belonging to another chip.  We only
         * get here when the header really is 1002:5046, so restate that.
         */
        stw_le_p(rom + pcir + 0x04, IA64_ATI_VENDOR_ID);
        stw_le_p(rom + pcir + 0x06, IA64_ATI_RAGE128_PF_ID);
    }
    rom[declared - 1] = 0;
    for (i = 0; i < declared - 1U; i++) {
        checksum += rom[i];
    }
    rom[declared - 1] = (uint8_t)(-checksum);
}

/*
 * Restate a video BIOS's PCI Data Structure vendor/device id to match the
 * adapter's configuration header, and fix the ROM image checksum.  A real ATI
 * ROM carries the id of the exact board it shipped on (e.g. a Mach64 GT VBIOS
 * declares 1002:4754 in its PCIR), but we may present that same silicon under
 * a different, driver-friendlier id (the Rage XL 1002:4752, the one both XP
 * IA-64 builds auto-match).  EFI 1.10 12.4 requires the PCIR id to match the
 * device, and a driver that validates its ROM against the bound device rejects
 * a mismatch, so bring the two into agreement.  A no-op when they already
 * agree (e.g. the Rage 128 SeaBIOS path, whose PCIR is fixed up above).
 */
static void ia64_vpc_match_rom_pcir(PCIDevice *pci_dev)
{
    uint8_t *rom;
    uint64_t rom_size;
    uint32_t declared, pcir, i;
    uint16_t ven, dev;
    uint8_t checksum = 0;

    if (pci_dev->io_regions[PCI_ROM_SLOT].size == 0 || !pci_dev->has_rom) {
        return;
    }
    rom = memory_region_get_ram_ptr(&pci_dev->rom);
    rom_size = memory_region_size(&pci_dev->rom);
    if (rom == NULL || rom_size < 0x400 || rom[0] != 0x55 || rom[1] != 0xaa) {
        return;
    }
    declared = (uint32_t)rom[2] * 512U;
    if (declared == 0 || declared > rom_size) {
        return;
    }
    pcir = lduw_le_p(rom + 0x18);
    if (pcir == 0 || pcir + 0x18U > declared ||
        memcmp(rom + pcir, "PCIR", 4) != 0) {
        return;
    }
    ven = pci_get_word(pci_dev->config + PCI_VENDOR_ID);
    dev = pci_get_word(pci_dev->config + PCI_DEVICE_ID);
    if (lduw_le_p(rom + pcir + 0x04) == ven &&
        lduw_le_p(rom + pcir + 0x06) == dev) {
        return; /* already matches */
    }
    stw_le_p(rom + pcir + 0x04, ven);
    stw_le_p(rom + pcir + 0x06, dev);
    rom[declared - 1] = 0;
    for (i = 0; i < declared - 1U; i++) {
        checksum += rom[i];
    }
    rom[declared - 1] = (uint8_t)(-checksum);
}

static void ia64_vpc_configure_vga(PCIDevice *pci_dev, uint32_t io_base)
{
    if (pci_dev == NULL) {
        return;
    }

    /*
     * The NVIDIA NV15GL keeps its own subsystem id (10de:006d, programmed by
     * the device) and a distinct BAR layout: BAR0 is the 16 MiB MMIO register
     * aperture and BAR1 is the 128 MiB prefetchable framebuffer.  It carries no
     * ATI BIOS tables, so bypass the Rage-specific ROM patching entirely.
     */
    if (pci_get_word(pci_dev->config + PCI_VENDOR_ID) == IA64_NV_VENDOR_ID) {
        pci_default_write_config(pci_dev, PCI_BASE_ADDRESS_0,
                                 IA64_NV_MMIO_PCI_BASE, 4);
        pci_default_write_config(pci_dev, PCI_BASE_ADDRESS_1,
                                 IA64_NV_FB_PCI_BASE, 4);
        if (pci_dev->io_regions[PCI_ROM_SLOT].size != 0) {
            pci_default_write_config(pci_dev, PCI_ROM_ADDRESS,
                                     IA64_NV_ROM_PCI_BASE, 4);
        }
        pci_default_write_config(pci_dev, PCI_COMMAND,
                                 PCI_COMMAND_IO | PCI_COMMAND_MEMORY, 2);
        return;
    }

    /*
     * QEMU's generic 1af4:1100 subsystem ID is not a value this chip can
     * report.  A Rage 128 loads the subsystem ID from the video BIOS on an
     * add-in card; with none loaded the documented hardware fallback is
     * SVID = vendor, SID = device (RAGE 128 PRO Register Reference Guide,
     * configuration space chapter).  Drivers index board tables by it.
     */
    pci_set_word(pci_dev->config + PCI_SUBSYSTEM_VENDOR_ID,
                 pci_get_word(pci_dev->config + PCI_VENDOR_ID));
    pci_set_word(pci_dev->config + PCI_SUBSYSTEM_ID,
                 pci_get_word(pci_dev->config + PCI_DEVICE_ID));

    pci_default_write_config(pci_dev, PCI_BASE_ADDRESS_0,
                             IA64_VGA_FB_PCI_BASE, 4);
    if (pci_dev->io_regions[1].memory != NULL) {
        pci_default_write_config(pci_dev, PCI_BASE_ADDRESS_0 + 4,
                                 io_base, 4);
    }
    pci_default_write_config(pci_dev, PCI_BASE_ADDRESS_0 + 8,
                             IA64_VGA_MMIO_PCI_BASE, 4);
    /*
     * Assign and enable the expansion ROM.  IA-64 has no architectural legacy
     * video BIOS shadow at 0xC0000, so Windows' videoprt reads the image
     * through the PCI ROM BAR (VideoPortGetRomImage).  Leaving BAR6
     * unassigned means a native display driver never sees a video BIOS at
     * all: Windows Whistler build 2462's Rage 128 miniport then leaves its
     * BIOS table pointer NULL and bugchecks 0x1E dereferencing it.  Every
     * other BAR on this machine is assigned by the machine model too.
     */
    if (pci_dev->io_regions[PCI_ROM_SLOT].size != 0) {
        ia64_vpc_install_ati_rom_tables(pci_dev);
        ia64_vpc_match_rom_pcir(pci_dev);
        /*
         * Assign the ROM BAR but leave its enable bit CLEAR.  With the bit
         * set at enumeration time, Windows' pci.sys generates a fourth
         * memory resource for the devnode (busdrv/pci device.c/enum.c), and
         * XP's inbox Rage 128 miniport calls VideoPortGetAccessRanges with a
         * three-entry array: videoprt's copy loop filters only legacy VGA
         * ranges, so the ROM range overflows the array and the call fails
         * with ERROR_MORE_DATA - silently, no event log - and HwFindAdapter
         * returns 234 (captured live: VideoPortGetAccessRanges RVA 0x33180
         * -> ati2mpaa .GetResources -> .FindAdapter -> Code 10).
         *
         * Readers of the ROM image do not need the bit set at handoff:
         * videoprt/pci.sys enable ROM decode transiently around
         * VideoPortGetRomImage (busdrv/pci romimage.c), which is how build
         * 2462's miniport reads the BIOS tables through BAR6.
         */
        pci_default_write_config(pci_dev, PCI_ROM_ADDRESS,
                                 IA64_VGA_ROM_PCI_BASE, 4);
    }
    /*
     * Both decodes on.  Windows XP's inbox Rage 128 miniport branches on
     * (Command & 3) == 3 in .GetResources (ati2mpaa.sys VMA 0x9375c) and only
     * then treats itself as the VGA device, so it is tempting to advertise
     * something else and take the "VGA disabled" path, which claims no legacy
     * VGA resources and reads the video BIOS from the ROM BAR instead of from
     * the 0xC0000 shadow (which this machine does provide - see
     * ia64_vpc_install_int10()).
     *
     * That does not work, and the reason is worth recording so it is not
     * retried: the miniport claims all three BARs as access ranges, and BAR1
     * is an I/O BAR.  videoprt's CheckIoEnabled (WSRV03 drivers/video/ms/port/
     * registry.c:2114) walks the claimed ranges and fails the whole call if a
     * RangeInIoSpace range is claimed while PCI_ENABLE_IO_SPACE is clear -
     * or, symmetrically, a memory range while PCI_ENABLE_MEMORY_SPACE is
     * clear.  VideoPortVerifyAccessRanges then returns ERROR_INVALID_PARAMETER
     * (registry.c:1966) *silently*, with no event logged, and the device stops
     * with Code 10 before touching a single register.  Any Command value that
     * satisfies CheckIoEnabled for a device with both I/O and memory BARs is
     * therefore exactly 3, which is also what real hardware presents.
     */
    pci_default_write_config(pci_dev, PCI_COMMAND,
                             PCI_COMMAND_IO | PCI_COMMAND_MEMORY, 2);

}

static bool ia64_vpc_enable_vga_legacy_switch(PCIDevice *pci_dev,
                                               Error **errp)
{
    if (pci_dev == NULL ||
        !object_property_find(OBJECT(pci_dev),
                              "x-vbe-legacy-mode-switch")) {
        return true;
    }

    return object_property_set_bool(OBJECT(pci_dev),
                                    "x-vbe-legacy-mode-switch", true,
                                    errp);
}

/*
 * Program a network adapter's BARs from the machine's fixed NIC resource
 * pools.  Unlike the other platform devices the NIC model is user-selectable
 * (-nic model=...), so we cannot assume a single fixed BAR layout: the e1000
 * exposes one 128 KiB memory BAR plus a 64-byte I/O BAR, while the Intel
 * PRO/100 (i82557b, the adapter Windows IA-64 actually ships a driver for)
 * exposes a 4 KiB CSR memory BAR, a 64-byte I/O BAR, and a 1 MiB flash memory
 * BAR.  Walk the realised regions instead and hand each BAR a naturally
 * aligned slice of the per-index memory / I/O window.  The firmware advertises
 * these same windows through the PCI0 _CRS, so keep every BAR inside them.
 */
static void ia64_vpc_configure_nic(PCIDevice *pci_dev, unsigned int index)
{
    uint64_t mmio_cursor;
    uint32_t io_cursor;
    int i;

    if (pci_dev == NULL || index >= MAX_NICS) {
        return;
    }

    mmio_cursor = IA64_E1000_MMIO_PCI_BASE + index * IA64_NIC_MMIO_STRIDE;
    io_cursor = IA64_E1000_IO_BASE + index * IA64_NIC_IO_STRIDE;

    for (i = 0; i < PCI_NUM_REGIONS - 1; i++) {
        PCIIORegion *r = &pci_dev->io_regions[i];
        int offset = PCI_BASE_ADDRESS_0 + i * 4;

        if (r->size == 0) {
            continue;
        }

        if (r->type & PCI_BASE_ADDRESS_SPACE_IO) {
            io_cursor = QEMU_ALIGN_UP(io_cursor, r->size);
            pci_default_write_config(pci_dev, offset, io_cursor, 4);
            io_cursor += r->size;
        } else {
            mmio_cursor = QEMU_ALIGN_UP(mmio_cursor, r->size);
            pci_default_write_config(pci_dev, offset,
                                     (uint32_t)mmio_cursor |
                                     (r->type & ~PCI_BASE_ADDRESS_MEM_MASK), 4);
            mmio_cursor += r->size;
            if (r->type & PCI_BASE_ADDRESS_MEM_TYPE_64) {
                pci_default_write_config(pci_dev, offset + 4, 0, 4);
                i++;
            }
        }
    }

    pci_default_write_config(pci_dev, PCI_COMMAND,
                             PCI_COMMAND_IO | PCI_COMMAND_MEMORY |
                             PCI_COMMAND_MASTER, 2);
}

/*
 * Build one SCSI adapter at the bus and device number the caller picked.
 * Both take the drives given without an explicit interface, so the adapter
 * built first -- the one holding the seat -- is the one that gets them.
 */
#ifdef CONFIG_IA64_VPC_STORAGE
static bool ia64_vpc_init_lsi(IA64VpcMachineState *s, PCIBus *bus, int devfn,
                              Error **errp)
{
    s->lsi_dev = pci_new(devfn, "lsi53c895a");
    qdev_prop_set_bit(DEVICE(s->lsi_dev), "disconnect-on-data-wait", false);
    if (!pci_realize_and_unref(s->lsi_dev, bus, errp)) {
        return false;
    }
    ia64_vpc_configure_lsi(s, s->lsi_dev);
    lsi53c8xx_handle_legacy_cmdline(DEVICE(s->lsi_dev));
    return true;
}

static void ia64_vpc_init_isp(IA64VpcMachineState *s, PCIBus *bus, int devfn)
{
    s->isp_dev = pci_create_simple(bus, devfn, TYPE_ISP12160_SCSI);
    ia64_vpc_configure_isp(s->isp_dev);
    scsi_bus_legacy_handle_cmdline(
        SCSI_BUS(qdev_get_child_bus(DEVICE(s->isp_dev), "isp12160-scsi.0")));
}
#endif

static void ia64_vpc_configure_platform_pci(IA64VpcMachineState *s)
{
    ia64_vpc_configure_ahci(s->ahci_dev);
    ia64_vpc_configure_audio(s->audio_dev);
    ia64_vpc_configure_isp(s->isp_dev);
    ia64_vpc_configure_ohci(s->ohci_dev);
    ia64_vpc_configure_uhci(s->uhci_dev);
    ia64_vpc_configure_ifb_ide(
        intel_82468gx_ifb_function(s->ifb, IA64_460GX_IFB_IDE_FUNCTION));
    ia64_vpc_configure_ifb_smbus(
        intel_82468gx_ifb_function(s->ifb, IA64_460GX_IFB_SMBUS_FUNCTION));
    ia64_vpc_configure_lsi(s, s->lsi_dev);
    ia64_vpc_configure_vga(s->vga_dev,
                           s->realfw_path != NULL ? IA64_VGA_IO_BASE_REALFW
                                                  : IA64_VGA_IO_BASE);
    for (unsigned int i = 0; i < s->nic_count; i++) {
        ia64_vpc_configure_nic(s->nic_devs[i], i);
    }
    ia64_vpc_configure_pci_irq(s->ahci_dev);
    ia64_vpc_configure_pci_irq(s->audio_dev);
    ia64_vpc_configure_pci_irq_on_root(
        s->isp_dev,
        ia64_vpc_root_gsi_base(s, ia64_vpc_chipset_is_zx1(s) ? 0 :
                               IA64_460GX_WXB0_BUS));
    ia64_vpc_configure_pci_irq(s->ide_dev);
    ia64_vpc_configure_pci_irq(s->ohci_dev);
    ia64_vpc_configure_pci_irq(s->uhci_dev);
    ia64_vpc_configure_pci_irq(
        intel_82468gx_ifb_function(s->ifb, IA64_460GX_IFB_IDE_FUNCTION));
    ia64_vpc_configure_pci_irq(
        intel_82468gx_ifb_function(s->ifb, IA64_460GX_IFB_SMBUS_FUNCTION));
    ia64_vpc_configure_pci_irq_on_root(
        s->lsi_dev,
        ia64_vpc_root_gsi_base(s, ia64_vpc_chipset_is_zx1(s) ? 0 :
                               s->isp_enabled ? IA64_460GX_WXB1_BUS :
                                                IA64_460GX_WXB0_BUS));
    ia64_vpc_configure_pci_irq_on_root(
        s->vga_dev,
        ia64_vpc_root_gsi_base(s, ia64_vpc_chipset_is_zx1(s) ? 0 :
                               IA64_460GX_GXB_BUS));
    for (unsigned int i = 0; i < s->nic_count; i++) {
        ia64_vpc_configure_pci_irq(s->nic_devs[i]);
    }
}

#ifdef CONFIG_IA64_VPC_NETWORK
static void ia64_vpc_record_nic(IA64VpcMachineState *s, PCIBus *bus,
                                PCIDevice *pci_dev)
{
    uint16_t class;

    if (pci_dev == NULL || s->nic_count >= MAX_NICS) {
        return;
    }

    class = pci_get_word(pci_dev->config + PCI_CLASS_DEVICE);
    if (class != PCI_CLASS_NETWORK_ETHERNET ||
        pci_get_bus(pci_dev) != bus) {
        return;
    }

    s->nic_devs[s->nic_count] = pci_dev;
    ia64_vpc_configure_nic(pci_dev, s->nic_count);
    ia64_vpc_configure_pci_irq(pci_dev);
    s->nic_count++;
}

static void ia64_vpc_init_network(IA64VpcMachineState *s, PCIBus *pci_bus)
{
    MachineState *machine = MACHINE(s);
    MachineClass *mc = MACHINE_GET_CLASS(machine);
    g_autofree char *slot_arg = NULL;
    unsigned int first_slot;
    unsigned int slot;

    s->nic_count = 0;
    memset(s->nic_devs, 0, sizeof(s->nic_devs));

    /* Keep the default adapter at a stable BDF after the built-in devices. */
    first_slot = ia64_vpc_chipset_is_zx1(s) ? IA64_VPC_NIC_SLOT :
                                              IA64_460GX_NIC_SLOT;
    slot_arg = g_strdup_printf("%u", first_slot);
    pci_init_nic_in_slot(pci_bus, mc->default_nic, NULL, slot_arg);
    pci_init_nic_devices(pci_bus, mc->default_nic);

    for (slot = first_slot; slot < PCI_SLOT_MAX; slot++) {
        ia64_vpc_record_nic(s, pci_bus,
                            pci_find_device(pci_bus, 0, PCI_DEVFN(slot, 0)));
    }
}
#endif

#define TYPE_IA64_PCI_FIXUP_RESET "ia64-pci-fixup-reset"
OBJECT_DECLARE_SIMPLE_TYPE(IA64PciFixupReset, IA64_PCI_FIXUP_RESET)

struct IA64PciFixupReset {
    Object parent;
    ResettableState reset_state;
    IA64VpcMachineState *machine;
};

OBJECT_DEFINE_SIMPLE_TYPE_WITH_INTERFACES(
    IA64PciFixupReset, ia64_pci_fixup_reset, IA64_PCI_FIXUP_RESET, OBJECT,
    { TYPE_RESETTABLE_INTERFACE }, { })

static ResettableState *ia64_pci_fixup_reset_get_state(Object *obj)
{
    IA64PciFixupReset *s = IA64_PCI_FIXUP_RESET(obj);

    return &s->reset_state;
}

static void ia64_pci_fixup_reset_exit(Object *obj, ResetType type)
{
    IA64PciFixupReset *r = IA64_PCI_FIXUP_RESET(obj);

    (void)type;

    ia64_vpc_configure_platform_pci(r->machine);
}

static void ia64_pci_fixup_reset_class_init(ObjectClass *klass,
                                            const void *data)
{
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    (void)data;
    rc->get_state = ia64_pci_fixup_reset_get_state;
    rc->phases.exit = ia64_pci_fixup_reset_exit;
}

static void ia64_pci_fixup_reset_init(Object *obj)
{
    (void)obj;
}

static void ia64_pci_fixup_reset_finalize(Object *obj)
{
    (void)obj;
}

static void ia64_vpc_map_vga_fixed_windows(IA64VpcMachineState *s,
                                           PCIDevice *pci_dev)
{
    PCIIORegion *fb;
    PCIIORegion *mmio;

    if (pci_dev == NULL) {
        return;
    }

    bool is_nvidia =
        pci_get_word(pci_dev->config + PCI_VENDOR_ID) == IA64_NV_VENDOR_ID;

    /*
     * NVIDIA uses BAR0=MMIO / BAR1=FB; ATI uses BAR0=FB / BAR2=MMIO.  The NV
     * BARs are already assigned at the firmware's fixed addresses by
     * ia64_vpc_configure_vga(), and its 128 MiB FB would overlap the ATI fixed
     * MMIO window, so NV only needs the legacy 0xA0000 alias set up below.
     */
    fb = &pci_dev->io_regions[is_nvidia ? 1 : 0];
    mmio = &pci_dev->io_regions[is_nvidia ? 0 : 2];
    if (fb->memory == NULL || mmio->memory == NULL ||
        fb->address_space == NULL || mmio->address_space == NULL) {
        return;
    }

    if (fb->address_space != mmio->address_space) {
        return;
    }

    if (!is_nvidia && s->vga_fb_alias == NULL) {
        s->vga_fb_alias = g_new(MemoryRegion, 1);
        memory_region_init_alias(s->vga_fb_alias, OBJECT(s),
                                 "ia64-vga-fb-fixed", fb->memory, 0, fb->size);
        memory_region_add_subregion_overlap(fb->address_space,
                                            IA64_VGA_FB_PCI_BASE,
                                            s->vga_fb_alias, 1);
    }

    if (!is_nvidia && s->vga_mmio_alias == NULL) {
        s->vga_mmio_alias = g_new(MemoryRegion, 1);
        memory_region_init_alias(s->vga_mmio_alias, OBJECT(s),
                                 "ia64-vga-mmio-fixed", mmio->memory, 0,
                                 mmio->size);
        memory_region_add_subregion_overlap(fb->address_space,
                                            IA64_VGA_MMIO_PCI_BASE,
                                            s->vga_mmio_alias, 1);
    }

    if (s->vga_legacy_alias == NULL) {
        s->vga_legacy_alias = g_new(MemoryRegion, 1);
        memory_region_init_alias(s->vga_legacy_alias,
                                 OBJECT(s),
                                 "ia64-vga-legacy-fixed",
                                 fb->address_space,
                                 IA64_VGA_LEGACY_BASE,
                                 IA64_VGA_LEGACY_SIZE);
        memory_region_add_subregion_overlap(get_system_memory(),
                                            IA64_VGA_LEGACY_BASE,
                                            s->vga_legacy_alias, 1);
    }
}

#ifdef CONFIG_IA64_VPC_USB
static bool ia64_vpc_init_usb(IA64VpcMachineState *s, PCIBus *pci_bus,
                              Error **errp)
{
    MachineState *machine = MACHINE(s);
    USBBus *usb_bus;
    bool add_default_input;

    machine->usb |= defaults_enabled() && !machine->usb_disabled;
    if (!machine->usb) {
        return true;
    }

    s->ohci_dev = pci_create_simple(pci_bus, -1, "pci-ohci");
    ia64_vpc_configure_ohci(s->ohci_dev);

    /*
     * The UHCI controller is function 2 of the south bridge on 460gx, so it
     * already exists by the time this runs; zx1 still gets a discrete one.
     */
    if (s->ifb != NULL) {
        s->uhci_dev = intel_82468gx_ifb_function(s->ifb,
                                                 IA64_460GX_IFB_USB_FUNCTION);
        if (s->uhci_dev == NULL) {
            error_setg(errp, "%s did not create its USB function",
                       TYPE_INTEL_82468GX_IFB);
            return false;
        }
    } else {
        s->uhci_dev = pci_create_simple(pci_bus, -1, TYPE_PIIX3_USB_UHCI);
    }
    ia64_vpc_configure_uhci(s->uhci_dev);

    add_default_input = defaults_enabled() && !s->i8042_enabled;
    if (add_default_input) {
        /*
         * Attach default USB input only when PS/2 is disabled. HID keyboards
         * become QEMU's active input handler, which would otherwise hide
         * firmware-visible PS/2 input before a guest USB stack exists.  Use
         * an absolute pointer so graphical front ends do not require a
         * relative-pointer grab.  Name the OHCI's bus rather than resolving
         * the only USB bus in the machine: with the south bridge's UHCI
         * present there is more than one.
         */
        usb_bus = USB_BUS(QLIST_FIRST(&DEVICE(s->ohci_dev)->child_bus));
        if (usb_bus == NULL) {
            error_setg(errp, "the OHCI controller has no USB bus");
            return false;
        }
        usb_create_simple(usb_bus, "usb-kbd");
        usb_create_simple(usb_bus, "usb-tablet");
    }
    return true;
}
#endif

static IA64BootInfo ia64_vpc_boot_info(MachineState *machine,
                                       uint64_t firmware_base,
                                       unsigned int cpu_index,
                                       uint64_t entry,
                                       uint64_t global_pointer)
{
    /*
     * The firmware's CPU-assist region (SAL re-entry slots, debug
     * contexts/stacks, early RSE backing stores, boot memory stacks) sits at
     * the top of installed low RAM, as real IA-64 firmware places its SAL
     * scratch; entry.S re-derives the same base from the handoff block.
     */
    uint64_t assist_base = IA64_FW_CPU_ASSIST_BASE_FOR(machine->ram_size);
    IA64BootInfo info = {
        .firmware_base = firmware_base,
        .firmware_entry = entry,
        .global_pointer = global_pointer,
        .iva = firmware_base + IA64_FW_IVT_OFFSET,
        .bsp = assist_base + IA64_FW_EARLY_RSE_OFFSET +
            cpu_index * IA64_FW_EARLY_RSE_SIZE,
        .stack_pointer = assist_base + IA64_FW_CPU_ASSIST_SIZE - 16 -
            cpu_index * IA64_FW_CPU_STACK_SIZE,
        .rsc = IA64_RSC_MODE,
        .fw_cpu_assist_base = assist_base,
        .powered_off = cpu_index != 0,
    };

    return info;
}

/*
 * CPU state initialization — called on every reset.
 *
 * Sets up the CPU in physical mode with firmware entry point.
 * Note: ROM content is loaded by rom_reset() which may run before or
 * after this handler, so we must NOT read ROM content here.  PE32+
 * plabel parsing is deferred to the machine_done notifier.
 */
static void ia64_vpc_init_realfw_chipset_cfg(IA64VpcMachineState *s);

static void ia64_vpc_reset(void *opaque)
{
    IA64VpcMachineState *s = opaque;
    CPUState *cs;

    /*
     * The handoff block lives in ordinary guest RAM; rom_reset() restores
     * the firmware image but nothing restores the block, and entry.S reads
     * it on every entry, so re-emit it or a guest that scribbled over it
     * would warm-reset with a corrupt handoff.
     */
    ia64_vpc_write_firmware_handoff(s);

    CPU_FOREACH(cs) {
        /* The CPUs are not children of the platform system bus. */
        ia64_cpu_reset_to_boot_info(IA64_CPU(cs));
    }

    /*
     * The 460GX configuration store (CBN, the chipset functions' BARs and
     * command registers, and the CF8 config-address latch) models chipset
     * state that a real SYS_RST/RST_CPU (port 0xCF9) clears.  It is only
     * seeded at machine-init, so without this a warm reset would leave it
     * holding the previous boot's programming (e.g. a non-zero CBN and
     * assigned BARs); the firmware's re-enumeration then takes a different
     * path and the second boot's video-ROM POST diverges (it hangs in a
     * vgabios timed-delay whose INT8 tick never advances).  Re-seed it to its
     * power-on identity on every reset (harmless on the initial cold reset,
     * which merely repeats the init-time seed).
     */
    if (s->realfw_path != NULL) {
        ia64_vpc_init_realfw_chipset_cfg(s);
    }

    acpi_pm1_evt_reset(&s->acpi_regs);
    acpi_pm1_cnt_reset(&s->acpi_regs);
    acpi_pm_tmr_reset(&s->acpi_regs);
    acpi_gpe_reset(&s->acpi_regs);
#ifdef CONFIG_IA64_VPC_GRAPHICS
    /*
     * The synthetic INT10 ROM is a passive 2 KiB image for Windows guests that
     * read the video BIOS through the PCI ROM BAR.  In realfw mode the vendor
     * SDV firmware instead POSTs the video device's own expansion ROM the
     * legacy way (shadow to 0xC0000, call C000:0003); a 2 KiB stub whose entry
     * jumps into its (absent) body then runs the CPU away into empty shadow, so
     * shadow the device's complete option ROM there instead.
     */
    if (s->vga_dev != NULL) {
        if (s->realfw_path != NULL) {
            ia64_vpc_install_realfw_video_rom(s);
        } else {
            ia64_vpc_reset_int10(s);
        }
    }
#endif
}

/*
 * Machine-done notifier — runs after the first reset cycle completes,
 * so ROM content is guaranteed to be in guest memory.  Parse a firmware
 * plabel only when the firmware image is a valid IA-64 PE32+ binary.
 */
static void ia64_vpc_machine_done(Notifier *notifier, void *data)
{
    IA64VpcMachineState *s = container_of(notifier, IA64VpcMachineState,
                                          done_notifier);
    g_autofree uint8_t *image = NULL;
    IA64FirmwareEntrypoint entrypoint;
    CPUState *cs;

    (void)data;
    ia64_vpc_configure_platform_pci(s);

    if (s->realfw_entry != 0) {
        CPU_FOREACH(cs) {
            IA64BootInfo info = {
                .firmware_base = s->realfw_base,
                .firmware_entry = s->realfw_entry,
                .iva = IA64_REALFW_IVT_BASE,
                .raw_entry = true,
                .raw_proc_id = cs->cpu_index,
                /*
                 * SAL calls PAL procedures through the machine-planted stub
                 * (GR34; GR36's authentication procedure lands on the same
                 * dispatcher and returns not-implemented for unknown
                 * indices).  SAL_B stashes this in bank-0 GR18 and uses it
                 * for every static PAL call.
                 */
                .raw_pal_proc = IA64_REALFW_PAL_STUB_BASE,
                .raw_pal_auth = IA64_REALFW_PAL_STUB_BASE,
                .powered_off = cs->cpu_index != 0,
            };

            ia64_cpu_set_boot_info(IA64_CPU(cs), &info);
            ia64_cpu_reset_to_boot_info(IA64_CPU(cs));
        }
        return;
    }

    if (s->firmware_size == 0) {
        return;
    }

    /*
     * The project firmware is a flat raw binary (no DOS+PE header).
     * Without a strict PE signature gate, random bytes can be mistaken
     * for PE metadata and clobber startup registers (including gp).
     */
    image = g_malloc(s->firmware_size);
    cpu_physical_memory_read(ia64_vpc_fw_base(s, current_machine->ram_size),
                             image, s->firmware_size);
    if (!ia64_loader_parse_pe_plabel(image, s->firmware_size,
                                     &entrypoint)) {
        return;
    }

    CPU_FOREACH(cs) {
        IA64BootInfo info = ia64_vpc_boot_info(MACHINE(s),
                                               ia64_vpc_fw_base(s,
                                                   current_machine->ram_size),
                                               cs->cpu_index,
                                               entrypoint.entry,
                                               entrypoint.global_pointer);

        ia64_cpu_set_boot_info(IA64_CPU(cs), &info);
        ia64_cpu_reset_to_boot_info(IA64_CPU(cs));
    }
}

static bool ia64_vpc_validate_configuration(MachineState *machine,
                                            IA64VpcMachineState *s,
                                            Error **errp)
{
    if (machine->ram_size < IA64_FW_LOW_RAM_MIN) {
        g_autofree char *size = size_to_str(IA64_FW_LOW_RAM_MIN);

        error_setg(errp, "Invalid RAM size, should be at least %s", size);
        return false;
    }
    if (s->alat_full && machine->smp.cpus > 1) {
        error_setg(errp, "full ALAT emulation is not SMP-safe");
        return false;
    }
    if (s->realfw_path != NULL && machine->firmware != NULL) {
        error_setg(errp, "realfw= and -bios are mutually exclusive");
        return false;
    }
    return true;
}

/*
 * Firmware self-relocation (rework phase 2.2).  The image links at 1 MB but
 * executes from the RAM-top shadow (IA64_FW_IMAGE_BASE_FOR); the build embeds
 * a machine-checked fixup table (fw-fixups.py) and a trailing footer
 * { "IA64FXUP", table file offset }.  QEMU plays SAL_A here: it applies the
 * fixups while placing the shadow, exactly as real firmware fixes up SAL_B
 * when shadowing it near the top of memory.
 */
#define IA64_FW_FIXUP_FOOTER_MAGIC 0x5055584634364149ULL /* "IA64FXUP" */
#define IA64_FW_FIXUP_MAGIC        0x50555846u           /* "FXUP" */

static uint64_t ia64_fw_bundle_imm64_get(const uint8_t *bundle)
{
    uint64_t lo = ldq_le_p(bundle);
    uint64_t hi = ldq_le_p(bundle + 8);
    uint64_t slot1 = ((lo >> 46) | (hi << 18)) & ((1ULL << 41) - 1);
    uint64_t slot2 = (hi >> 23) & ((1ULL << 41) - 1);
    uint64_t imm7b = (slot2 >> 6) & 0x7f;
    uint64_t imm9d = (slot2 >> 27) & 0x1ff;
    uint64_t imm5c = (slot2 >> 22) & 0x1f;
    uint64_t ic = (slot2 >> 21) & 0x1;
    uint64_t i = (slot2 >> 36) & 0x1;

    return (i << 63) | (slot1 << 22) | (ic << 21) | (imm5c << 16) |
           (imm9d << 7) | imm7b;
}

static void ia64_fw_bundle_imm64_set(uint8_t *bundle, uint64_t imm64)
{
    uint64_t lo = ldq_le_p(bundle);
    uint64_t hi = ldq_le_p(bundle + 8);
    uint64_t slot2 = (hi >> 23) & ((1ULL << 41) - 1);
    uint64_t slot1 = (imm64 >> 22) & ((1ULL << 41) - 1);

    slot2 &= ~((1ULL << 36) | (0x1ffULL << 27) | (0x1fULL << 22) |
               (1ULL << 21) | (0x7fULL << 6));
    slot2 |= (((imm64 >> 63) & 1) << 36) | (((imm64 >> 7) & 0x1ff) << 27) |
             (((imm64 >> 16) & 0x1f) << 22) | (((imm64 >> 21) & 1) << 21) |
             ((imm64 & 0x7f) << 6);
    lo = (lo & ((1ULL << 46) - 1)) | (slot1 << 46);
    hi = (slot1 >> 18) | (slot2 << 23);
    stq_le_p(bundle, lo);
    stq_le_p(bundle + 8, hi);
}

static bool ia64_vpc_relocate_firmware(uint8_t *image, int64_t size,
                                       uint64_t delta, Error **errp)
{
    uint64_t fixups_off;
    uint32_t n64, n32, nimm, i;
    const uint8_t *table;
    uint64_t entries;

    if (size < 16 ||
        ldq_le_p(image + size - 16) != IA64_FW_FIXUP_FOOTER_MAGIC) {
        error_setg(errp, "firmware image carries no relocation footer "
                   "(rebuilt with fw-fixups.py?)");
        return false;
    }
    fixups_off = ldq_le_p(image + size - 8);
    if (fixups_off > (uint64_t)size - 32) {
        error_setg(errp, "firmware relocation table offset out of range");
        return false;
    }
    table = image + fixups_off;
    if (ldl_le_p(table) != IA64_FW_FIXUP_MAGIC || ldl_le_p(table + 4) != 1) {
        error_setg(errp, "firmware relocation table has a bad header");
        return false;
    }
    n64 = ldl_le_p(table + 8);
    n32 = ldl_le_p(table + 12);
    nimm = ldl_le_p(table + 16);
    entries = (uint64_t)n64 + n32 + nimm;
    if (fixups_off + 32 + entries * 8 > (uint64_t)size) {
        error_setg(errp, "firmware relocation table truncated");
        return false;
    }
    table += 32;
    for (i = 0; i < n64; i++, table += 8) {
        uint64_t off = ldq_le_p(table);

        if (off > (uint64_t)size - 8) {
            error_setg(errp, "firmware DIR64 fixup out of range");
            return false;
        }
        stq_le_p(image + off, ldq_le_p(image + off) + delta);
    }
    for (i = 0; i < n32; i++, table += 8) {
        uint64_t off = ldq_le_p(table);

        if (off > (uint64_t)size - 4) {
            error_setg(errp, "firmware DIR32 fixup out of range");
            return false;
        }
        stl_le_p(image + off, ldl_le_p(image + off) + (uint32_t)delta);
    }
    for (i = 0; i < nimm; i++, table += 8) {
        uint64_t off = ldq_le_p(table);

        if (off > (uint64_t)size - 16) {
            error_setg(errp, "firmware IMM64 fixup out of range");
            return false;
        }
        ia64_fw_bundle_imm64_set(image + off,
                                 ia64_fw_bundle_imm64_get(image + off) +
                                 delta);
    }
    return true;
}

/*
 * Load a real vendor flash image (machine option realfw=) so that its end
 * lands exactly at 4 GiB, and derive the boot entry from the architected
 * SALE_ENTRY pointer at 4 GiB-24.  The flash window lies inside the
 * ia64-firmware-address-space RAM region, so rom_add_blob_fixed() both
 * installs the content and restores it on reset.  The blob is split around
 * the NVRAM MMIO window (which deliberately overlays the image's FIT-0x1E
 * scratch sector at priority 2): a rom_reset() write through the NVRAM ops
 * would wipe the guest's stored variables with erased-flash bytes.
 */
/*
 * The same two-bundle PAL procedure entry stub the project firmware carries
 * at IA64_FW_PAL_PROC_ENTRY_OFF (roms/ia64-firmware/entry.S pal_proc_entry):
 *   break.m 0x100000 ;;  br.many b0 ;;
 * The translator services the break through ia64_pal_dispatch() when the
 * bundle sits at a recognized PAL entry address (env->pal.pal_proc_copy_addr,
 * seeded from IA64BootInfo.raw_pal_proc in realfw mode).
 */
/*
 * POST-code port 0x80/0x81 (SAL narrates boot progress there; the codes are
 * tabulated in plans/sdv-i2000-firmware-reference.md sec 6.5).  Logged on
 * change only, so a code re-written in a wait loop cannot flood the log.
 */
static uint64_t ia64_realfw_post_read(void *opaque, hwaddr addr, unsigned size)
{
    IA64VpcMachineState *s = opaque;

    return s->realfw_post_last >> (addr * 8);
}

static void ia64_realfw_post_write(void *opaque, hwaddr addr, uint64_t val,
                                   unsigned size)
{
    IA64VpcMachineState *s = opaque;
    uint16_t code = s->realfw_post_last;

    if (size == 2 && addr == 0) {
        code = val;
    } else {
        code &= ~(0xff << (addr * 8));
        code |= (val & 0xff) << (addr * 8);
    }
    if (code != s->realfw_post_last) {
        s->realfw_post_last = code;
        qemu_log("ia64-realfw: POST %02x%02x\n", code >> 8, code & 0xff);
    }
}

static const MemoryRegionOps ia64_realfw_post_ops = {
    .read = ia64_realfw_post_read,
    .write = ia64_realfw_post_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 2,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

/*
 * IFB fn3 SMBus host controller (PIIX4-style register file at I/O 0xFFF0).
 *
 * During QuickBoot the SDV firmware programs the IFB Function 3 SMBus I/O BAR
 * to 0xFFF0 and runs SMBus byte-data transactions to initialise the board's
 * hardware-monitor sensor chips (observed device addresses 0x2C and 0x4E): it
 * writes SMBHSTCMD/ADD/DAT0, kicks SMBHSTCNT with START (bit 6), then spins on
 * SMBHSTSTS bit 1 (INTR = transaction complete) with HOST_BUSY (bit 0) clear
 * -- i.e. `(status & 3) == 2`.  With no I/O region here the poll floats high
 * (0xff -> status bits 11b) and the firmware hangs at POST 0xc6.
 *
 * We have no physical devices behind the bus.  The firmware only needs the
 * transaction to *complete*: it polls SMBHSTSTS for `(status & 3) == 2` (INTR
 * set, HOST_BUSY clear), so report the controller permanently idle-with-INTR
 * (0x02).  Every other register reads back 0 -- the value the firmware got for
 * these ports before this region existed (the sparse-I/O container answers 0
 * for an in-range but unclaimed port), which its controller-enable poll at
 * offset 0xe depends on: floating those bytes high (0xff) instead makes that
 * poll spin forever.  Register offsets follow the Intel PIIX4 SMBus layout
 * (SMBHSTSTS 0, SMBHSTCNT 2, SMBHSTCMD 3, SMBHSTADD 4, SMBHSTDAT0 5).
 */
#define IA64_REALFW_SMB_STS   0x00   /* bit0 HOST_BUSY, bit1 INTR, bit2 DEV_ERR */
#define IA64_REALFW_SMB_BASE  0xfff0
#define IA64_REALFW_SMB_SIZE  0x10

static uint64_t ia64_realfw_smbus_read(void *opaque, hwaddr addr, unsigned size)
{
    return (addr == IA64_REALFW_SMB_STS) ? 0x02 : 0;
}

static void ia64_realfw_smbus_write(void *opaque, hwaddr addr, uint64_t val,
                                    unsigned size)
{
    /* No physical device: every transaction "completes" with nothing to do. */
}

static const MemoryRegionOps ia64_realfw_smbus_ops = {
    .read = ia64_realfw_smbus_read,
    .write = ia64_realfw_smbus_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

/*
 * System Control Port B (I/O 0x61).  Near the end of POST the SDV firmware
 * uses bit 4 -- the DRAM REFRESH toggle -- as a timing reference: it reads
 * port 0x61 in a tight loop and waits for bit 4 to flip a full 0->1->0 refresh
 * period to calibrate a delay.  Real hardware toggles that bit roughly every
 * 15 us; unmodelled the port floats to a constant (open-bus 0xff, bit 4 stuck
 * at 1) and the loop never sees the flip, hanging at POST ~0x05.
 *
 * Report the pre-existing open-bus value 0xff -- which is what the firmware saw
 * for the other bits before this region existed and reached this far with, so
 * nothing that reads the port earlier regresses -- but drive bit 4 from the
 * virtual clock so the refresh toggle is observed.  Writes (the firmware pokes
 * the timer-2/speaker gate bits) are dropped, exactly as the unbacked port did.
 */
#define IA64_REALFW_PORT61            0x61
#define IA64_REALFW_PORT61_REFRESH_NS 15000

static uint64_t ia64_realfw_port61_read(void *opaque, hwaddr addr, unsigned size)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint8_t refresh = (now / IA64_REALFW_PORT61_REFRESH_NS) & 1;

    return (uint8_t)((0xff & ~0x10) | (refresh << 4));
}

static void ia64_realfw_port61_write(void *opaque, hwaddr addr, uint64_t val,
                                     unsigned size)
{
    /* Open bus: writes are dropped, as for the previously unbacked port. */
}

static const MemoryRegionOps ia64_realfw_port61_ops = {
    .read = ia64_realfw_port61_read,
    .write = ia64_realfw_port61_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 1,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static uint64_t ia64_realfw_sac_read(void *opaque, hwaddr addr, unsigned size)
{
    IA64VpcMachineState *s = opaque;
    uint64_t val = 0;
    unsigned i;

    for (i = 0; i < size; i++) {
        val |= (uint64_t)s->realfw_sac_data[addr + i] << (i * 8);
    }
    if (addr <= IA64_REALFW_SAC_BOOT_SEM &&
        addr + size > IA64_REALFW_SAC_BOOT_SEM) {
        /* Boot semaphore: granted, holder id 0 (the BSP's LID.id). */
        val |= 0x80ULL << ((IA64_REALFW_SAC_BOOT_SEM - addr) * 8);
    }
    qemu_log_mask(LOG_UNIMP, "ia64-realfw: SAC read  +%04x/%u = 0x%" PRIx64
                  "\n", (unsigned)addr, size, val);
    return val;
}

static void ia64_realfw_sac_write(void *opaque, hwaddr addr, uint64_t val,
                                  unsigned size)
{
    IA64VpcMachineState *s = opaque;
    unsigned i;

    for (i = 0; i < size; i++) {
        s->realfw_sac_data[addr + i] = val >> (i * 8);
    }
    qemu_log_mask(LOG_UNIMP, "ia64-realfw: SAC write +%04x/%u = 0x%" PRIx64
                  "\n", (unsigned)addr, size, val);
}

static const MemoryRegionOps ia64_realfw_sac_ops = {
    .read = ia64_realfw_sac_read,
    .write = ia64_realfw_sac_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 8,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

/*
 * 460GX CF8/CFC configuration space (realfw mode).  Bus CBN (reset 0)
 * carries the chipset's own devices (SSDM Table 2-1): dev 00h/01h SAC,
 * 04h SDC, 05h/06h Memory Card A/B (MAC; SPD EEPROMs tunnel through its
 * higher functions over I2C), dev 10h the CBN-programming device.  The
 * public SSDM documents none of the platform-setup register offsets
 * (plans/460gx-config-space-notes.md), so the model is a write-store/
 * read-back scratch per function with the empirically required specials:
 * Memory Card A claims presence with a MAC ID, dev 10h reg 40h is the CBN.
 * Accesses to non-chipset device numbers forward to the QEMU PCI bus.
 */
#define IA64_REALFW_IFB_DEV       0x1e
static const uint8_t ia64_realfw_chipset_devs[] = { 0x00, 0x01, 0x04, 0x05,
                                                    IA64_REALFW_IFB_DEV,
                                                    0x10 };
#define IA64_REALFW_CFG_FN_SIZE   256
#define IA64_REALFW_CFG_DEV_SIZE  (8 * IA64_REALFW_CFG_FN_SIZE)
#define IA64_REALFW_CFG_SIZE      \
    (ARRAY_SIZE(ia64_realfw_chipset_devs) * IA64_REALFW_CFG_DEV_SIZE)
#define IA64_REALFW_CBN_DEV       0x10
#define IA64_REALFW_CBN_REG       0x40

/*
 * SPD EEPROM served through the MAC's I2C pass-through: firmware writes the
 * DIMM's I2C address (0x54..0x57, bit 7 = read) into the SAC IIADR register
 * (dev 00h fn 0 reg 0x68), then config reads of Memory Card fn 2/3 return
 * the addressed EEPROM's bytes at the register offset (observed protocol,
 * plans/phase5-real-firmware-boot.md sec 5.5; register naming per
 * plans/460gx-config-space-notes.md).  One image serves all four DIMMs:
 * 256 MB registered SDRAM (32Mx4 devices: 13 row / 10 column address bits,
 * 4 banks, 1 module rank, x72 ECC) - 4 x 256 MB = 1 GiB on Memory Card A.
 */
#define IA64_REALFW_SAC_IIADR_REG 0x68
static const uint8_t ia64_realfw_spd[64] = {
    [0] = 128,    /* bytes written by manufacturer */
    [1] = 8,      /* log2 of EEPROM size (256 bytes) */
    [2] = 4,      /* memory type: SDRAM */
    [3] = 13,     /* row address bits */
    [4] = 10,     /* column address bits */
    [5] = 1,      /* module rows (ranks) */
    [6] = 72,     /* module data width low */
    [8] = 1,      /* interface level: LVTTL */
    [9] = 0xa0,   /* cycle time 10 ns (PC100) */
    [11] = 2,     /* ECC */
    [12] = 0x82,  /* refresh: self-refresh, 15.6 us */
    [13] = 4,     /* primary SDRAM device width x4 */
    [17] = 4,     /* banks per SDRAM device */
    [18] = 4,     /* CAS latencies supported */
    [31] = 0x40,  /* module rank density: 256 MB */
};

static uint8_t *ia64_realfw_chipset_cfg(IA64VpcMachineState *s,
                                        uint8_t bus, uint8_t dev, uint8_t fn)
{
    uint8_t cbn = s->realfw_chipset_cfg[(ARRAY_SIZE(ia64_realfw_chipset_devs)
                                         - 1) * IA64_REALFW_CFG_DEV_SIZE +
                                        IA64_REALFW_CBN_REG];
    unsigned i;

    /* Dev 10h is always on bus 0; the rest live on bus CBN. */
    if (bus == 0 && dev == IA64_REALFW_CBN_DEV) {
        dev = IA64_REALFW_CBN_DEV;
    } else if (bus != cbn) {
        return NULL;
    }
    for (i = 0; i < ARRAY_SIZE(ia64_realfw_chipset_devs); i++) {
        if (ia64_realfw_chipset_devs[i] == dev) {
            return s->realfw_chipset_cfg + i * IA64_REALFW_CFG_DEV_SIZE +
                   fn * IA64_REALFW_CFG_FN_SIZE;
        }
    }
    return NULL;
}

static uint64_t ia64_realfw_cfg_read(void *opaque, hwaddr addr, unsigned size)
{
    IA64VpcMachineState *s = opaque;
    uint32_t cf8 = s->realfw_config_address;
    uint8_t bus = cf8 >> 16, dev = (cf8 >> 11) & 0x1f, fn = (cf8 >> 8) & 7;
    unsigned reg = (cf8 & 0xfc) | (addr & 3);
    uint8_t *cfg;
    uint64_t val = 0;
    unsigned i;

    if (addr < 4) {
        return s->realfw_config_address >> (addr * 8);
    }
    if (!(cf8 & 0x80000000)) {
        return (1ULL << (size * 8)) - 1;
    }
    /*
     * CPU-frequency mailbox (south bridge 00:03.0 reg 0xd0).  The SDV firmware
     * stores the detected processor frequency here and reads it back on the
     * next boot; a fresh (zero) mailbox makes its frequency detector fall back
     * to a sentinel and take the one-time "New CPU frequency is set. System
     * resets." reboot (port 0xCF9).  The register is battery-backed on real
     * hardware, so the written value must survive that reset for the reboot to
     * be one-shot rather than an infinite loop.  Model it as a persistent cell
     * that ia64_vpc_reset does NOT clear.  See plans/phase5 SESSION 17.
     */
    if (s->realfw_path != NULL && bus == 0 && dev == 3 && fn == 0 &&
        (reg & 0xfc) == 0xd0) {
        {
            /*
             * Bit 15 is the hardware "done/valid" flag: the firmware writes a
             * frequency command (bit 15 clear) and polls until the mailbox
             * reads back with bit 15 set.  Real silicon sets it once it has
             * latched the value; our model completes instantly, so present the
             * stored command with bit 15 forced set.
             */
            uint32_t cell = s->realfw_freq_mailbox | 0x8000;
            for (i = 0; i < size; i++) {
                unsigned b = (reg & 3) + i;
                if (b < 4) {
                    val |= (uint64_t)((cell >> (b * 8)) & 0xff) << (i * 8);
                }
            }
        }
        return val;
    }
    cfg = ia64_realfw_chipset_cfg(s, bus, dev, fn);
    if (cfg != NULL && dev == 0x05 && (fn == 2 || fn == 3)) {
        /*
         * MAC I2C pass-through: serve the addressed DIMM's SPD EEPROM.
         * The card carries 4 rows x 4 DIMMs (row select = one-hot in
         * fn 4..7 reg 0x48); only row 0 is populated - 4 x 256 MB = 1 GiB.
         *
         * The SAC (dev 0) and the MAC's fn 4..7 sit on the same bus as the
         * addressed dev 5 - i.e. the CBN bus, which the guest's own address
         * decoded here as 'bus'.  Once the firmware has programmed CBN to a
         * non-zero value (which happens late in POST) a hard-coded bus 0 no
         * longer resolves these functions and the lookup returns NULL, so use
         * 'bus' and guard defensively.
         */
        uint8_t *sac = ia64_realfw_chipset_cfg(s, bus, 0, 0);
        uint8_t *r4 = ia64_realfw_chipset_cfg(s, bus, 0x05, 4);
        uint8_t *r5 = ia64_realfw_chipset_cfg(s, bus, 0x05, 5);
        uint8_t *r6 = ia64_realfw_chipset_cfg(s, bus, 0x05, 6);
        uint8_t *r7 = ia64_realfw_chipset_cfg(s, bus, 0x05, 7);

        if (sac != NULL && r4 != NULL && r5 != NULL && r6 != NULL &&
            r7 != NULL) {
            uint8_t iiadr = sac[IA64_REALFW_SAC_IIADR_REG];
            bool row0 = r4[0x48] == 1 && r5[0x48] == 0 &&
                        r6[0x48] == 0 && r7[0x48] == 0;

            if ((iiadr & 0xfc) == 0xd4 && row0) {
                for (i = 0; i < size; i++) {
                    unsigned off = (reg + i) & 0xff;

                    val |= (uint64_t)(off < sizeof(ia64_realfw_spd)
                                      ? ia64_realfw_spd[off] : 0) << (i * 8);
                }
            }
        }
    } else if (cfg != NULL && (reg & 0xfc) == 0x30) {
        /*
         * The 460GX chipset functions carry no expansion ROM, so their ROM BAR
         * (0x30) must read back 0.  The config store is a plain write/read-back
         * cell, so without this it returns whatever the firmware last wrote --
         * during BAR sizing that is 0xFFFFFFFE, which the firmware decodes as a
         * 2 KiB ROM.  A real add-in card at the same dev number (our ATI video
         * shares dev 5 with the MAC while CBN still reads 0) then has its 64 KiB
         * video ROM sized as 2 KiB, and the next option ROM is shadowed on top
         * of the video ROM body -- corrupting the vgabios INT10 handler.
         */
        val = 0;
    } else if (cfg != NULL) {
        for (i = 0; i < size; i++) {
            val |= (uint64_t)cfg[(reg + i) & 0xff] << (i * 8);
        }
    } else {
        PCIDevice *pci_dev = pci_find_device(s->realfw_pci_bus, bus,
                                             PCI_DEVFN(dev, fn));

        val = pci_dev != NULL
            ? pci_host_config_read_common(pci_dev, reg,
                                          pci_config_size(pci_dev), size)
            : (1ULL << (size * 8)) - 1;
    }
    qemu_log_mask(LOG_UNIMP, "ia64-realfw: cfg%c read  %02x:%02x.%x "
                  "@0x%02x/%u = 0x%" PRIx64 "\n", cfg ? '*' : ' ',
                  bus, dev, fn, reg, size, val);
    return val;
}

static void ia64_realfw_cfg_write(void *opaque, hwaddr addr, uint64_t data,
                                  unsigned size)
{
    IA64VpcMachineState *s = opaque;
    uint32_t cf8 = s->realfw_config_address;
    uint8_t bus = cf8 >> 16, dev = (cf8 >> 11) & 0x1f, fn = (cf8 >> 8) & 7;
    unsigned reg = (cf8 & 0xfc) | (addr & 3);
    uint8_t *cfg;
    unsigned i;

    if (addr == 1 && size == 1) {
        /*
         * Port 0xCF9 (RST_CNT): an 8-bit access is the legacy PC reset-control
         * register, aliased with byte 1 of the 0xCF8 config-address register.
         * RST_CPU (bit 2) set triggers a system reset.  The SDV firmware writes
         * 0xCF9=2 then 0xCF9=6 to reboot after its one-time "New CPU frequency
         * is set" configuration step (the historical POST-0xc6 "wall").  The
         * warm-boot path this reset lands in is still being brought up (the
         * post-reset video-ROM POST diverges), so honouring the reset is gated
         * behind STDBG_CF9RESET for now — see plans/phase5-real-firmware-boot.md.
         */
        if ((data & 0x04) && getenv("STDBG_CF9RESET")) {
            qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
            return;
        }
    }

    if (addr < 4) {
        for (i = 0; i < size && addr + i < 4; i++) {
            s->realfw_config_address &= ~(0xffU << ((addr + i) * 8));
            s->realfw_config_address |= ((data >> (i * 8)) & 0xff) <<
                                        ((addr + i) * 8);
        }
        return;
    }
    if (!(cf8 & 0x80000000)) {
        return;
    }
    /* CPU-frequency mailbox at 00:03.0 reg 0xd0 - see the read path. */
    if (s->realfw_path != NULL && bus == 0 && dev == 3 && fn == 0 &&
        (reg & 0xfc) == 0xd0) {
        for (i = 0; i < size; i++) {
            unsigned b = (reg & 3) + i;
            if (b < 4) {
                s->realfw_freq_mailbox &= ~(0xffU << (b * 8));
                s->realfw_freq_mailbox |= ((data >> (i * 8)) & 0xff) << (b * 8);
            }
        }
        return;
    }
    cfg = ia64_realfw_chipset_cfg(s, bus, dev, fn);
    qemu_log_mask(LOG_UNIMP, "ia64-realfw: cfg%c write %02x:%02x.%x "
                  "@0x%02x/%u = 0x%" PRIx64 "\n", cfg ? '*' : ' ',
                  bus, dev, fn, reg, size, data);
    if (cfg != NULL) {
        for (i = 0; i < size; i++) {
            cfg[(reg + i) & 0xff] = data >> (i * 8);
        }
        return;
    }
    {
        PCIDevice *pci_dev = pci_find_device(s->realfw_pci_bus, bus,
                                             PCI_DEVFN(dev, fn));

        if (pci_dev != NULL) {
            pci_host_config_write_common(pci_dev, reg,
                                         pci_config_size(pci_dev),
                                         data, size);
        }
    }
}

static const MemoryRegionOps ia64_realfw_cfg_ops = {
    .read = ia64_realfw_cfg_read,
    .write = ia64_realfw_cfg_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

/*
 * Seed one 460GX chipset function's PCI header.  Subsystem ids repeat the
 * vendor and device id, as the real parts report, and DEVSEL timing is
 * medium.  The rest of the function's config space stays read/write
 * scratch.
 */
static void ia64_vpc_init_realfw_chipset_identity(IA64VpcMachineState *s,
                                                  uint8_t dev, uint8_t fn,
                                                  uint16_t device_id,
                                                  uint8_t revision,
                                                  uint16_t class_id,
                                                  bool multifunction)
{
    uint8_t *cfg = ia64_realfw_chipset_cfg(s, 0, dev, fn);

    if (cfg == NULL) {
        return;
    }
    stw_le_p(cfg + PCI_VENDOR_ID, PCI_VENDOR_ID_INTEL);
    stw_le_p(cfg + PCI_DEVICE_ID, device_id);
    stw_le_p(cfg + PCI_STATUS, PCI_STATUS_DEVSEL_MEDIUM);
    cfg[PCI_REVISION_ID] = revision;
    stw_le_p(cfg + PCI_CLASS_DEVICE, class_id);
    stw_le_p(cfg + PCI_SUBSYSTEM_VENDOR_ID, PCI_VENDOR_ID_INTEL);
    stw_le_p(cfg + PCI_SUBSYSTEM_ID, device_id);
    if (multifunction) {
        cfg[PCI_HEADER_TYPE] = PCI_HEADER_TYPE_MULTI_FUNCTION;
    }
}

static void ia64_vpc_init_realfw_chipset_cfg(IA64VpcMachineState *s)
{
    uint8_t *mac_a;

    if (s->realfw_chipset_cfg == NULL) {
        s->realfw_chipset_cfg = g_malloc0(IA64_REALFW_CFG_SIZE);
    } else {
        memset(s->realfw_chipset_cfg, 0, IA64_REALFW_CFG_SIZE);
    }
    s->realfw_config_address = 0;
    /*
     * The chipset's own functions carry their real identities.  Without
     * them a firmware config read of the SAC, SDC or expander returns a
     * zero vendor id, which is neither "present" nor the architected
     * "absent" 0xffff.  Device ids, revisions and classes per the 460GX
     * SSDM Table 2-1 and upstream's intel_460gx_chipset.c (fda8a29);
     * expander device numbers per plans/sdv-i2000-firmware-reference.md,
     * which places expander port n at bus CBN device 10h + n.
     */
    ia64_vpc_init_realfw_chipset_identity(s, 0x00, 0, 0x84e0, 0x03,
                                          PCI_CLASS_BRIDGE_HOST, true);
    ia64_vpc_init_realfw_chipset_identity(s, 0x01, 0, 0x84e0, 0x03,
                                          PCI_CLASS_BRIDGE_HOST, false);
    ia64_vpc_init_realfw_chipset_identity(s, 0x04, 0, 0x84e1, 0x03,
                                          PCI_CLASS_BRIDGE_HOST, false);
    /*
     * Expander port 0 is the PXB, which hosts the compatibility bus.  Its
     * function 0 is the downstream SAC; function 1 is the bridge itself.
     * Function 0 register 40h is also where the firmware programs CBN
     * (observed; see ia64_realfw_chipset_cfg), so only the header is
     * seeded here.
     */
    ia64_vpc_init_realfw_chipset_identity(s, IA64_REALFW_CBN_DEV, 0,
                                          0x84e0, 0x03,
                                          PCI_CLASS_BRIDGE_HOST, true);
    ia64_vpc_init_realfw_chipset_identity(s, IA64_REALFW_CBN_DEV, 1,
                                          0x84cb, 0x05,
                                          PCI_CLASS_BRIDGE_HOST, false);

    /*
     * Memory Card A (dev 05h fn 0) claims presence with the MAC identity
     * (8086:84E3, rev B-1 = 03h; pci.ids, flagged unverified in
     * plans/460gx-config-space-notes.md).  Memory Card B stays absent.
     */
    mac_a = ia64_realfw_chipset_cfg(s, 0, 0x05, 0);
    stw_le_p(mac_a + PCI_VENDOR_ID, 0x8086);
    stw_le_p(mac_a + PCI_DEVICE_ID, 0x84e3);
    mac_a[PCI_REVISION_ID] = 0x03;

    /*
     * 460GX I/O & Firmware Bridge (IFB), the platform south bridge.  Real
     * SDV firmware's QuickBoot scans bus 0 for it (8086:7600) and fatal-spins
     * if absent (POST 0x98).  Model Function 0's identity per the 460GX SSDM
     * §11: multi-function ISA/LPC bridge.  The other functions (fn1 IDE,
     * fn2 USB, fn3 SMBus) and the config-register behaviours are added as the
     * firmware exercises them; the rest of the config space is read/write
     * scratch (see ia64_realfw_cfg_read/write).
     */
    {
        uint8_t *ifb0 = ia64_realfw_chipset_cfg(s, 0, IA64_REALFW_IFB_DEV, 0);

        stw_le_p(ifb0 + PCI_VENDOR_ID, 0x8086);          /* VID */
        stw_le_p(ifb0 + PCI_DEVICE_ID, 0x7600);          /* DID = IFB */
        ifb0[PCI_REVISION_ID] = 0x03;                    /* RID (stepping) */
        ifb0[PCI_CLASS_PROG] = 0x00;                     /* CLASSC 060100h: */
        stw_le_p(ifb0 + PCI_CLASS_DEVICE, 0x0601);       /*   ISA bridge */
        ifb0[PCI_HEADER_TYPE] = 0x80;                    /* multi-function */
    }
}

static void ia64_vpc_init_realfw_devices(IA64VpcMachineState *s,
                                         MemoryRegion *pci_io)
{
    s->realfw_sac_data = g_malloc0(IA64_REALFW_SAC_SIZE);
    memory_region_init_io(&s->realfw_sac_mmio, OBJECT(s),
                          &ia64_realfw_sac_ops, s, "ia64-realfw.sac",
                          IA64_REALFW_SAC_SIZE);
    memory_region_add_subregion(get_system_memory(), IA64_REALFW_SAC_BASE,
                                &s->realfw_sac_mmio);
    memory_region_init_io(&s->realfw_post_io, OBJECT(s),
                          &ia64_realfw_post_ops, s, "ia64-realfw.post", 2);
    memory_region_add_subregion(pci_io, 0x80, &s->realfw_post_io);
    memory_region_init_io(&s->realfw_smbus_io, OBJECT(s),
                          &ia64_realfw_smbus_ops, s, "ia64-realfw.smbus",
                          IA64_REALFW_SMB_SIZE);
    memory_region_add_subregion(pci_io, IA64_REALFW_SMB_BASE,
                                &s->realfw_smbus_io);
    memory_region_init_io(&s->realfw_port61_io, OBJECT(s),
                          &ia64_realfw_port61_ops, s, "ia64-realfw.port61", 1);
    memory_region_add_subregion(pci_io, IA64_REALFW_PORT61,
                                &s->realfw_port61_io);
    ia64_vpc_init_realfw_chipset_cfg(s);
    memory_region_init_io(&s->realfw_cfg_io, OBJECT(s),
                          &ia64_realfw_cfg_ops, s, "ia64-realfw.cfg", 8);
    memory_region_add_subregion(pci_io, 0xcf8, &s->realfw_cfg_io);
}

/*
 * Real SDV firmware drives IDE through the fixed legacy I/O ports, not the
 * controller's PCI BARs: it polls the primary status register at 0x1f7 during
 * drive detection and spins forever if nothing answers.  The CMD646's ATA
 * register blocks are otherwise only reachable at firmware-assigned BAR
 * addresses, so alias them into the legacy ranges (command block 0x1f0-0x1f7
 * and 0x170-0x177, control block at 0x3f4/0x374 whose offset-2 register is the
 * 0x3f6/0x376 alt-status).  With no media attached the channels report an
 * empty bus, which the firmware reads as "no drive" and moves on.  This runs
 * only in realfw mode; our own firmware and guests use the PCI BARs.
 */
static void ia64_vpc_map_realfw_legacy_ide(IA64VpcMachineState *s,
                                           MemoryRegion *pci_io)
{
    PCIIDEState *ide = PCI_IDE(s->ide_dev);
    static const struct {
        uint16_t data_base;
        uint16_t cmd_base;
    } channel[2] = {
        { 0x1f0, 0x3f4 },
        { 0x170, 0x374 },
    };
    int i;

    for (i = 0; i < 2; i++) {
        g_autofree char *data_name =
            g_strdup_printf("ia64-realfw.ide-data%d", i);
        g_autofree char *cmd_name =
            g_strdup_printf("ia64-realfw.ide-cmd%d", i);

        memory_region_init_alias(&s->realfw_ide_data[i], OBJECT(s), data_name,
                                 &ide->data_bar[i], 0, 8);
        memory_region_add_subregion(pci_io, channel[i].data_base,
                                    &s->realfw_ide_data[i]);
        memory_region_init_alias(&s->realfw_ide_cmd[i], OBJECT(s), cmd_name,
                                 &ide->cmd_bar[i], 0, 4);
        memory_region_add_subregion(pci_io, channel[i].cmd_base,
                                    &s->realfw_ide_cmd[i]);
    }
}

/*
 * The master 8259's INTR line, delivered to the boot processor as an IA-64
 * ExtINT (SAPIC vector 0): while the PIC asserts INTR the processor takes an
 * external interrupt whose IVR reads 0, and firmware then fetches the real
 * 8-bit vector from the PIC itself.  ExtINT is level-sensitive, so forward the
 * line state directly -- de-asserting it (for example when firmware masks the
 * PIC before draining IVR) withdraws the pending vector 0.
 */
static void ia64_vpc_extint(void *opaque, int n, int level)
{
    (void)opaque;
    (void)n;
    if (first_cpu != NULL) {
        ia64_sapic_set_extint(first_cpu, level);
    }
}

/*
 * Real SDV firmware uses the legacy PC-AT timer tick during POST: it programs
 * the 8254 PIT channel 0 for a periodic square wave and routes its IRQ 0
 * through the 8259 PIC, whose INTR reaches the processor as an ExtINT (above).
 * The machine is otherwise IOSAPIC-only, so instantiate the pair only in
 * realfw mode and wire PIT OUT0 straight into 8259 IR0, independent of the
 * IOSAPIC-backed ISA IRQ inputs the rest of the machine uses.
 */
/*
 * realfw mode models the chipset itself, south bridge included, so it carries
 * its own PIC rather than the one inside the modelled 82468GX (which that mode
 * does not create -- see the south-bridge comment in ia64_vpc_build).
 */
static void ia64_vpc_init_realfw_pic(IA64VpcMachineState *s, ISABus *isa_bus)
{
    qemu_irq *pic_irqs;

    s->extint = qemu_allocate_irq(ia64_vpc_extint, s, 0);
    pic_irqs = i8259_init(isa_bus, s->extint);
    /* PIT OUT0 -> 8259 IR0 (isa_irq = -1 selects the explicit alt_irq). */
    i8254_pit_init(isa_bus, 0x40, -1, pic_irqs[0]);
    g_free(pic_irqs);
}

static const uint8_t ia64_realfw_pal_stub[32] = {
    0x0a, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00,
    0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00,
    0x11, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x00, 0x02, 0x00, 0x00, 0x08, 0x00, 0x80, 0x00,
};

/*
 * Open the realfw-nvram persistence file as a raw, writable block backend for
 * the flash device.  On first use (or if the file is the wrong size, e.g. the
 * realfw image changed) it is created from the vendor image; the pflash device
 * requires the backing file to be exactly the flash size.  Once attached, the
 * flash loads its contents from the file and writes back to it, so the
 * firmware's one-time NVRAM reprogram survives across runs.
 */
static BlockBackend *ia64_realfw_open_nvram(const char *path,
                                            const uint8_t *image,
                                            uint64_t image_size, Error **errp)
{
    QDict *options;
    BlockBackend *blk;
    struct stat st;

    if (stat(path, &st) != 0 || (uint64_t)st.st_size != image_size) {
        GError *gerr = NULL;

        if (!g_file_set_contents(path, (const gchar *)image, image_size,
                                 &gerr)) {
            error_setg(errp, "realfw-nvram '%s': cannot initialise: %s",
                       path, gerr->message);
            g_error_free(gerr);
            return NULL;
        }
    }

    options = qdict_new();
    qdict_put_str(options, "driver", "raw");
    blk = blk_new_open(path, NULL, options, BDRV_O_RDWR, errp);
    if (blk == NULL) {
        error_prepend(errp, "realfw-nvram '%s': ", path);
    }
    return blk;
}

static bool ia64_vpc_load_realfw(IA64VpcMachineState *s, Error **errp)
{
    g_autofree uint8_t *image = NULL;
    gsize image_size = 0;
    GError *gerr = NULL;
    uint64_t base, fit_ptr, sale_ptr;
    char fit_sig[8];

    if (!g_file_get_contents(s->realfw_path, (gchar **)&image,
                             &image_size, &gerr)) {
        error_setg(errp, "failed to read realfw image '%s': %s",
                   s->realfw_path, gerr->message);
        g_error_free(gerr);
        return false;
    }
    if (image_size == 0 || image_size > IA64_REALFW_MAX_SIZE ||
        (image_size & 0xffff) != 0) {
        error_setg(errp, "realfw image '%s' must be a whole number of "
                   "64 KiB flash blocks, at most 4 MiB", s->realfw_path);
        return false;
    }
    base = IA64_REALFW_WINDOW_END - image_size;

    fit_ptr = ldq_le_p(image + (IA64_REALFW_PTR_FIT - base)) &
              IA64_REALFW_PTR_ADDR_MASK;
    sale_ptr = ldq_le_p(image + (IA64_REALFW_PTR_SALE - base)) &
               IA64_REALFW_PTR_ADDR_MASK;
    if (fit_ptr < base || fit_ptr + sizeof(fit_sig) > IA64_REALFW_WINDOW_END ||
        sale_ptr < base || sale_ptr >= IA64_REALFW_WINDOW_END) {
        error_setg(errp, "realfw image '%s': reset pointer block does not "
                   "point into the image (FIT 0x%" PRIx64 ", SALE_ENTRY "
                   "0x%" PRIx64 ")", s->realfw_path, fit_ptr, sale_ptr);
        return false;
    }
    memcpy(fit_sig, image + (fit_ptr - base), sizeof(fit_sig));
    if (memcmp(fit_sig, "_FIT_   ", sizeof(fit_sig)) != 0) {
        error_setg(errp, "realfw image '%s': no _FIT_ signature at the "
                   "FIT pointer target 0x%" PRIx64, s->realfw_path, fit_ptr);
        return false;
    }

    /*
     * The flash is a real Intel-CFI (command-set 0x0001) part: SDV firmware
     * probes it during QuickBoot (write 0x50 Clear-Status, 0x70 Read-Status,
     * read the WSM-ready bit 0x80, 0xff Read-Array) and uses it as writable
     * non-volatile storage for EFI settings / boot config in the FIT-0x1E
     * NVRAM sector.  Model it with pflash_cfi01 (the Intel CFI flash device)
     * initialized from the vendor image, overlaying the firmware address
     * space (priority above the identity RAM region) so its command interface
     * shadows plain RAM at the flash window.  64 KiB blocks match the block
     * size the firmware's flash descriptor uses.
     */
    {
        DeviceState *dev = qdev_new(TYPE_PFLASH_CFI01);
        MemoryRegion *flash_mr;
        BlockBackend *flash_blk = NULL;

        if (s->realfw_nvram_path != NULL) {
            flash_blk = ia64_realfw_open_nvram(s->realfw_nvram_path, image,
                                               image_size, errp);
            if (flash_blk == NULL) {
                return false;
            }
            qdev_prop_set_drive(dev, "drive", flash_blk);
        }

        qdev_prop_set_uint32(dev, "num-blocks", image_size / 0x10000);
        qdev_prop_set_uint64(dev, "sector-length", 0x10000);
        qdev_prop_set_uint8(dev, "width", 1);
        qdev_prop_set_bit(dev, "big-endian", 0);
        /*
         * JEDEC ID the firmware checks: manufacturer 0x89 (Intel), device
         * 0xAC (82802AB Firmware Hub).  It byte-reads read-ID offset 0 for
         * the manufacturer and offset 1 for the device, then combines them to
         * 0xAC89.  pflash returns id0<<8|id1 at word offset 0 and id2<<8|id3
         * at word offset 1, so a byte read of offset 0 yields id1 (hold the
         * manufacturer there) and a byte read of offset 1 yields id3 (hold
         * the device there).  id0 also carries the device so that a 16-bit
         * read of offset 0 reads 0xAC89 too.
         */
        qdev_prop_set_uint16(dev, "id0", 0x00ac);
        qdev_prop_set_uint16(dev, "id1", 0x0089);        /* Intel (offset 0) */
        qdev_prop_set_uint16(dev, "id2", 0x0000);
        qdev_prop_set_uint16(dev, "id3", 0x00ac);        /* 82802AB (offset 1) */
        qdev_prop_set_string(dev, "name", "ia64-realfw-flash");
        sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);

        s->realfw_flash = PFLASH_CFI01(dev);
        flash_mr = pflash_cfi01_get_memory(s->realfw_flash);
        memory_region_add_subregion_overlap(get_system_memory(), base,
                                            flash_mr, 2);
        /*
         * Without a persistence file the flash starts from the vendor image
         * every boot; with one the pflash device has already loaded the
         * (possibly firmware-updated) contents from the backing file, so the
         * image copy would clobber them.
         */
        if (flash_blk == NULL) {
            memcpy(memory_region_get_ram_ptr(flash_mr), image, image_size);
        }
    }

    rom_add_blob_fixed("ia64-realfw-palstub", ia64_realfw_pal_stub,
                       sizeof(ia64_realfw_pal_stub),
                       IA64_REALFW_PAL_STUB_BASE);

    {
        /* Branch-to-self bundle (MIB: nop.m; nop.i; br.few 0). */
        static const uint8_t self_branch[16] = {
            0x11, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
            0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40,
        };
        g_autofree uint8_t *ivt = g_malloc(IA64_REALFW_IVT_SIZE);
        size_t off;

        for (off = 0; off < IA64_REALFW_IVT_SIZE; off += sizeof(self_branch)) {
            memcpy(ivt + off, self_branch, sizeof(self_branch));
        }
        rom_add_blob_fixed("ia64-realfw-ivt", ivt, IA64_REALFW_IVT_SIZE,
                           IA64_REALFW_IVT_BASE);
    }

    s->realfw_base = base;
    s->realfw_entry = sale_ptr;
    /* No project firmware image: machine_done must not parse a PE plabel. */
    s->firmware_size = 0;
    return true;
}

static bool ia64_vpc_load_firmware(IA64VpcMachineState *s,
                                   MachineState *machine, Error **errp)
{
    g_autofree char *firmware_path = NULL;
    const char *firmware = machine->firmware;
    Error *local_err = NULL;
    int64_t firmware_size;

    if (s->realfw_path != NULL) {
        return ia64_vpc_load_realfw(s, errp);
    }

    if (firmware == NULL) {
        /*
         * Fall back to the shipped image.  Not finding it is not an error:
         * qtest brings this machine up with no firmware at all.
         */
        firmware_path = qemu_find_file(QEMU_FILE_TYPE_BIOS,
                                       IA64_VPC_DEFAULT_FIRMWARE);
        if (firmware_path == NULL) {
            return true;
        }
        firmware = IA64_VPC_DEFAULT_FIRMWARE;
    } else {
        firmware_path = qemu_find_file(QEMU_FILE_TYPE_BIOS, firmware);
        if (firmware_path == NULL) {
            firmware_path = g_strdup(firmware);
        }
    }
    firmware_size = get_image_size(firmware_path, &local_err);
    if (local_err != NULL) {
        error_prepend(&local_err, "failed to inspect firmware '%s': ",
                      firmware);
        error_propagate(errp, local_err);
        return false;
    }
    {
        uint64_t fw_base = ia64_vpc_fw_base(s, machine->ram_size);
        g_autofree uint8_t *image = NULL;
        gsize image_size = 0;
        GError *gerr = NULL;

        if (firmware_size <= 0 ||
            (uint64_t)firmware_size > IA64_FW_IMAGE_SPAN) {
            error_setg(errp, "invalid firmware image size for '%s'",
                       firmware);
            return false;
        }
        if (!g_file_get_contents(firmware_path, (gchar **)&image,
                                 &image_size, &gerr)) {
            error_setg(errp, "failed to read firmware '%s': %s", firmware,
                       gerr->message);
            g_error_free(gerr);
            return false;
        }
        if (fw_base != IA64_FW_LINK_BASE &&
            !ia64_vpc_relocate_firmware(image, image_size,
                                        fw_base - IA64_FW_LINK_BASE, errp)) {
            return false;
        }
        rom_add_blob_fixed("ia64-firmware", image, image_size, fw_base);
        s->firmware_size = firmware_size;
    }
    return true;
}

static bool ia64_vpc_build(MachineState *machine, Error **errp)
{
    IA64VpcMachineState *s = IA64_VPC_MACHINE(machine);
    IA64CPU *cpu;
    DeviceState *pci_host;
    DeviceState *iosapic;
    PCIBus *pci_bus;
    ISABus *isa_bus;
    MemoryRegion *pci_io;
#ifdef CONFIG_IA64_VPC_STORAGE
    DriveInfo *sata_drives[6] = { NULL };
    AHCIPCIState *ahci;
#endif
    int i;

    if (!ia64_vpc_validate_configuration(machine, s, errp)) {
        return false;
    }

    ia64_vpc_map_ram(s);
    if (!ia64_vpc_map_firmware_address_space(s, errp)) {
        return false;
    }
    ia64_vpc_init_watchdog(s);
    ia64_vpc_init_nvram(s);
    ia64_vpc_write_firmware_handoff(s);

    for (i = 0; i < machine->smp.cpus; i++) {
        uint32_t threads = MAX(machine->smp.threads, 1U);
        uint32_t cores = MAX(machine->smp.cores, 1U);
        uint32_t per_socket = threads * cores;
        uint32_t package_base = (i / per_socket) * per_socket;
        uint64_t fw_base = ia64_vpc_fw_base(s, machine->ram_size);
        IA64BootInfo boot_info = ia64_vpc_boot_info(machine, fw_base, i,
                                                    fw_base, fw_base);

        cpu = IA64_CPU(object_new(machine->cpu_type));
        cpu->alat_full = s->alat_full;
        cpu->fw_image_base = fw_base;
        cpu->socket_id = i / per_socket;
        cpu->core_id = (i / threads) % cores;
        cpu->thread_id = i % threads;
        cpu->cores_per_socket = cores;
        cpu->threads_per_core = threads;
        cpu->package_base = package_base;
        cpu->package_cpus = MIN(per_socket,
                                machine->smp.cpus - package_base);
        ia64_cpu_set_boot_info(cpu, &boot_info);
        if (!qdev_realize_and_unref(DEVICE(cpu), NULL, errp)) {
            return false;
        }
    }
    ia64_vpc_map_lsapic(s);

    iosapic = qdev_new(TYPE_IA64_IOSAPIC);
    if (!ia64_vpc_chipset_is_zx1(s)) {
        /*
         * On the i2000 the interrupt controller is the 460GX Programmable
         * Interrupt Device: 64 inputs reporting IOSAPIC version 2.1.  Its
         * width is what lets each PCI root own its own block of four INTx
         * lines (16, 20, 24, 28) rather than sharing one block.
         */
        qdev_prop_set_uint32(iosapic, "num-pins", IA64_IOSAPIC_460GX_PINS);
        qdev_prop_set_uint32(iosapic, "version",
                             IA64_IOSAPIC_460GX_VERSION);
    }
    if (!sysbus_realize_and_unref(SYS_BUS_DEVICE(iosapic), errp)) {
        return false;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(iosapic), 0, IA64_IOSAPIC_BASE);

    serial_mm_init(get_system_memory(), IA64_UART_BASE, 0,
                   qdev_get_gpio_in(iosapic, 4),
                   115200, serial_hd(0), DEVICE_LITTLE_ENDIAN);
    if (debug_port_get_chardev()) {
        s->debug_uart = serial_mm_init(get_system_memory(),
                                       IA64_DEBUG_UART_BASE, 0,
                                       qdev_get_gpio_in(iosapic, 3),
                                       115200, debug_port_get_chardev(),
                                       DEVICE_LITTLE_ENDIAN);
    }

    if (!ia64_vpc_load_firmware(s, machine, errp)) {
        return false;
    }

    /*
     * The firmware IVT now lives inside the image (.fw_ivt, zero-filled =
     * break bundles), so the historical machine-side fill is gone.
     */

    /* Defer PE32+ plabel parsing until after ROM content is loaded */
    s->done_notifier.notify = ia64_vpc_machine_done;
    qemu_add_machine_init_done_notifier(&s->done_notifier);

    pci_host = qdev_new(TYPE_IA64_PCI_HOST_BRIDGE);
    if (!sysbus_realize_and_unref(SYS_BUS_DEVICE(pci_host), errp)) {
        return false;
    }
    pci_bus = PCI_BUS(qdev_get_child_bus(pci_host, "pci"));

    /*
     * The core chipset's DMA-translation device, created before any other PCI
     * device so its pci_setup_iommu() installs the bus-master DMA routing before
     * any master's address space is resolved.  Exactly one owner per bus.
     *
     * chipset=zx1: the HP zx1 SBA IOC IOMMU -- one shared translated address
     * space for every master, walking the guest-programmed in-DRAM IOPDIR, so a
     * 32-bit master (the Rage 128) reaches RAM above 4 GiB by address
     * translation.  No 460GX GXB is created; the Rage 128 falls back to its own
     * PCI-GART, now translated >4 GiB by the SBA.  The zx1 LBA (created below)
     * additionally lets Linux hp-agp negotiate AGP mode reusing the SBA IOPDIR
     * as the GART.  The SBA is parked at a fixed high slot like the GXB.
     *
     * chipset=460gx (default): the 460GX GXB AGP host bridge + GART, which
     * translates only the AGP graphics master (the Rage 128 at the fixed
     * graphics slot below); every other master identity-passes to memory.
     */
    if (ia64_vpc_chipset_is_zx1(s)) {
        s->sba_dev = pci_new(PCI_DEVFN(PCI_SLOT_MAX - 1, 0), TYPE_IA64_SBA);
        object_property_set_uint(OBJECT(s->sba_dev), "csr-base",
                                 IA64_SBA_CSR_BASE, &error_abort);
        if (!pci_realize_and_unref(s->sba_dev, pci_bus, errp)) {
            return false;
        }
        /*
         * The zx1 LBA AGP capability block, published to guests as ACPI
         * HWP0003 nested inside the SBA (HWP0001).  Linux hp-agp negotiates AGP
         * mode against it and reuses the SBA IOPDIR as the GART; it is faithful
         * real-zx1 hardware, so it is present regardless of the agp option.
         * The AGP capability it advertises only becomes usable once the
         * graphics master also advertises a PCI AGP capability, which the agp
         * option gates below.
         */
        s->lba_dev = qdev_new(TYPE_IA64_LBA);
        object_property_set_uint(OBJECT(s->lba_dev), "csr-base",
                                 IA64_LBA_CSR_BASE, &error_abort);
        if (!qdev_realize_and_unref(s->lba_dev, NULL, errp)) {
            return false;
        }
        /*
         * The Mercury (LBA/ioa) PCI host bridge: a second PCI root bus sharing
         * the primary host bridge's identity-mapped MMIO/I/O windows, which will
         * carry the AGP graphics adapter -- exactly as real zx1 puts the AGP
         * master behind Mercury.  The primary host's ECAM config handler
         * dispatches config cycles for IA64_MERCURY_BUS here, and the SBA extends
         * its shared DMA translation over this bus too, so a master here reaches
         * RAM above 4 GiB through the same IOPDIR/GART.  Attach the SBA before any
         * device is realized on the bus.
         */
        s->mercury_host = ia64_mercury_host_create(OBJECT(s),
                                  ia64_pci_host_mmio(pci_host),
                                  ia64_pci_host_io(pci_host),
                                  IA64_MERCURY_BUS, errp);
        if (s->mercury_host == NULL) {
            return false;
        }
        s->mercury_bus = ia64_mercury_host_bus(s->mercury_host);
        ia64_pci_host_set_mercury_bus(pci_host, s->mercury_bus);
        ia64_sba_attach_bus(IA64_SBA(s->sba_dev), s->mercury_bus);
        /* The Mercury CSR CONFIG_ADDRESS/DATA pair does config on this bus. */
        ia64_lba_set_mercury_bus(IA64_LBA(s->lba_dev), s->mercury_bus);
    } else {
        s->agp_dev = pci_new(PCI_DEVFN(PCI_SLOT_MAX - 1, 0), TYPE_IA64_AGP);
        /*
         * The AGP master is the graphics adapter on the GXB's downstream
         * root, so the GART translates that bus's device 0.
         */
        object_property_set_int(OBJECT(s->agp_dev), "agp-master-devfn",
                                PCI_DEVFN(IA64_460GX_GXB_VGA_SLOT, 0),
                                &error_abort);
        object_property_set_bool(OBJECT(s->agp_dev), "gart-enabled",
                                s->agp_enabled, &error_abort);
        if (!pci_realize_and_unref(s->agp_dev, pci_bus, errp)) {
            return false;
        }

    }

    /*
     * Slot 0 is intentionally empty in the default machine.  Reserve it while
     * creating the built-in devices so their historical slot numbers remain
     * stable, then release it for an explicitly requested PCI controller.
     */
    pci_bus_set_slot_reserved_mask(pci_bus, 1U << 0);
    pci_io = pci_bus->address_space_io;
    ia64_vpc_init_acpi_pm(s, iosapic, pci_io);
    if (s->realfw_path != NULL) {
        s->realfw_pci_bus = pci_bus;
        ia64_vpc_init_realfw_devices(s, pci_io);
    }

    /*
     * Early IA-64 kernel debuggers predate the ACPI DBGP table and drive a
     * fixed legacy COM1 at I/O port 0x3f8 (Windows Whistler build 2462's
     * kdcom.dll hardcodes 0x3f8/0x2f8/0x3e8/0x2e8 and reaches them through
     * HAL's READ_PORT_UCHAR/WRITE_PORT_UCHAR).  Real Merced platforms carry a
     * Super I/O UART there, so alias the debug port's register window into
     * the legacy I/O range as well.  Aliasing rather than instantiating a
     * second UART keeps one device, one chardev and one interrupt line.
     */
    if (s->debug_uart != NULL) {
        memory_region_init_alias(&s->debug_uart_legacy_io, OBJECT(s),
                                 "ia64-vpc.debug-uart-legacy-io",
                                 &s->debug_uart->serial.io, 0,
                                 IA64_LEGACY_COM1_IO_SIZE);
        memory_region_add_subregion(pci_io, IA64_LEGACY_COM1_IO_BASE,
                                    &s->debug_uart_legacy_io);
    }

    /*
     * Leave ISA/SCI lines in the legacy range and route PCI INTx above 15.
     * On zx1 the graphics master lives on the Mercury second root bus, which
     * uses the same (slot+pin)%4 swizzle and the same four GSIs (16..19) as
     * PCI0.  Both host bridges drive their own GPIO-out line per INTx, so OR
     * each PCI0 line with the matching Mercury line into the shared IOSAPIC
     * input (level-triggered, wire-OR -- exactly how the two roots share the
     * platform's four PCI interrupt lines).
     */
    for (i = 0; i < IA64_PCI_INTX_LINES; i++) {
        qemu_irq gsi = qdev_get_gpio_in(iosapic, IA64_PCI_INTX_GSI_BASE + i);

        if (ia64_vpc_chipset_is_zx1(s)) {
            DeviceState *org = qdev_new(TYPE_OR_IRQ);

            object_property_set_int(OBJECT(org), "num-lines", 2, &error_abort);
            qdev_realize_and_unref(org, NULL, &error_abort);
            qdev_connect_gpio_out(org, 0, gsi);
            qdev_connect_gpio_out(pci_host, i, qdev_get_gpio_in(org, 0));
            qdev_connect_gpio_out(s->mercury_host, i, qdev_get_gpio_in(org, 1));
        } else {
            qdev_connect_gpio_out(pci_host, i, gsi);
        }
    }

    /*
     * The i2000's other three PCI roots: the two WXB buses and the GXB AGP
     * bus.  Each is a root in its own right, sharing the primary host
     * bridge's identity-mapped windows the way the zx1 Mercury root does, and
     * each owns its own block of four PID inputs rather than sharing bus 0's
     * -- which is why the Programmable Interrupt Device has 64 of them.  They
     * are created empty here; devices move onto them in a later step of
     * plans/460gx-i2000-fidelity-plan.md.
     */
    if (!ia64_vpc_chipset_is_zx1(s)) {
        static const struct {
            const char *name;
            uint8_t bus;
            unsigned int index;
        } expanders[IA64_460GX_EXPANDER_ROOTS] = {
            /*
             * Created youngest-first.  QEMU resolves a "-device" with no
             * bus= to the most recently realized PCI bus, so this order
             * makes WXB0 the default: on a real i2000 bus 0 is the
             * compatibility bus carrying the on-board south bridge, while
             * add-in cards go in the WXB slots.  Name the bus explicitly
             * ("bus=pci", "bus=gxb", ...) to place a device elsewhere.
             */
            { "gxb",  IA64_460GX_GXB_BUS,  IA64_460GX_ROOT_GXB },
            { "wxb1", IA64_460GX_WXB1_BUS, IA64_460GX_ROOT_WXB1 },
            { "wxb0", IA64_460GX_WXB0_BUS, IA64_460GX_ROOT_WXB0 },
        };
        unsigned int root;

        for (root = 0; root < IA64_460GX_EXPANDER_ROOTS; root++) {
            unsigned int line;

            unsigned int index = expanders[root].index;

            s->expander_host[index] = ia64_expander_host_create(
                OBJECT(s), expanders[root].name,
                ia64_pci_host_mmio(pci_host), ia64_pci_host_io(pci_host),
                expanders[root].bus, errp);
            if (s->expander_host[index] == NULL) {
                return false;
            }
            s->expander_bus[index] =
                ia64_expander_host_bus(s->expander_host[index]);
            ia64_pci_host_add_secondary_bus(pci_host, s->expander_bus[index]);

            for (line = 0; line < IA64_PCI_INTX_LINES; line++) {
                unsigned int input = IA64_PCI_INTX_GSI_BASE +
                                     expanders[root].bus *
                                     IA64_PCI_INTX_LINES + line;

                qdev_connect_gpio_out(s->expander_host[index], line,
                                      qdev_get_gpio_in(iosapic, input));
            }
        }

        /*
         * The GART translates the AGP master, which lives on the GXB's
         * downstream root, so that bus needs the same DMA routing as the
         * bus the GXB bridge itself sits on.
         */
        if (s->agp_dev != NULL) {
            ia64_agp_attach_bus(IA64_AGP(s->agp_dev),
                                s->expander_bus[IA64_460GX_ROOT_GXB]);
        }

        /*
         * Each WXB bus carries an Integrated Hot-Plug Controller for its
         * expansion slots.  Nothing implements hot plug here; the controller
         * is present because the board has one, and idle.
         */
        for (root = 0; root < IA64_460GX_EXPANDER_ROOTS; root++) {
            unsigned int index = expanders[root].index;

            if (index != IA64_460GX_ROOT_WXB0 &&
                index != IA64_460GX_ROOT_WXB1) {
                continue;
            }
            if (!pci_realize_and_unref(
                    pci_new(PCI_DEVFN(IA64_460GX_IHPC_SLOT, 0),
                            TYPE_IA64_460GX_IHPC),
                    s->expander_bus[index], errp)) {
                return false;
            }
        }
    }

    /*
     * AHCI remains available for guests that support SATA.  Firmware boot
     * storage is provided by the LSI SCSI HBA below.  ahci=off removes the
     * controller entirely: guests without a SATA driver (e.g. Windows XP
     * IA-64) then see neither an unknown PCI device nor its INTx line,
     * which the INTx swizzle would otherwise share with the VGA slot.
     * Slot 1 stays reserved so the remaining devices keep their BDFs.
     */
#ifdef CONFIG_IA64_VPC_STORAGE
    if (s->ahci_enabled) {
        s->ahci_dev = pci_create_simple(pci_bus, -1, TYPE_ICH9_AHCI);
        ia64_vpc_configure_ahci(s->ahci_dev);
        ahci = ICH9_AHCI(s->ahci_dev);
        g_assert(ahci->ahci.ports <= ARRAY_SIZE(sata_drives));
        /*
         * The AHCI ports and the IDE controller both present an ATA "if=ide"
         * bus.  IDE owns those drives whenever it is there to own them --
         * always on a board with the south bridge, and with ide=on
         * elsewhere -- so only bind if=ide media to SATA otherwise; a user
         * can still attach disks to this controller explicitly.
         */
        if (!s->ide_enabled && !ia64_vpc_has_south_bridge(s)) {
            ide_drive_get(sata_drives, ahci->ahci.ports);
            ahci_ide_create_devs(&ahci->ahci, sata_drives);
        }
    } else {
        pci_bus_set_slot_reserved_mask(pci_bus, 1U << 1);
    }
#endif

    /*
     * The i2000's south bridge is the Intel 82468GX I/O and Firmware Bridge
     * at 00:03, a four-function device: LPC/ISA, IDE, UHCI and SMBus.  Its
     * function 0 owns the ISA bus and the 8259 pair, so on 460gx the legacy
     * devices below hang off the bridge that carries them on the board
     * rather than off a bus with no parent.  Each of the sixteen ISA
     * interrupt inputs drives both that PIC and the matching Programmable
     * Interrupt Device input, which is how they reach the guest: an IA-64
     * guest runs the SAPIC, and the PIC is there for the real SDV firmware
     * (and for the PC/AT-compatible flag our MADT already sets).
     *
     * zx1 is a different platform with a different south bridge, so it keeps
     * the parentless ISA bus until it gets one of its own.
     *
     * realfw mode keeps it too: that path replaces the whole chipset with the
     * shadow config space in ia64_vpc_init_realfw_chipset_cfg(), which models
     * an IFB of its own (at device 1Eh, where the SDV firmware's bus scan
     * happens to find it).  Two models of one chip on one bus is incoherent,
     * and converging them means re-validating the real firmware's POST, so
     * that belongs to plans/phase5-real-firmware-boot.md rather than here.
     */
    if (ia64_vpc_has_south_bridge(s)) {
        s->ifb = intel_82468gx_ifb_create(
            pci_bus, PCI_DEVFN(IA64_460GX_IFB_SLOT,
                               IA64_460GX_IFB_LPC_FUNCTION), errp);
        if (s->ifb == NULL) {
            return false;
        }
        for (i = 0; i < INTEL_82468GX_IFB_FUNCTIONS; i++) {
            PCIDevice *fn = intel_82468gx_ifb_function(s->ifb, i);

            if (fn == NULL) {
                error_setg(errp, "%s did not create function %d",
                           TYPE_INTEL_82468GX_IFB, i);
                return false;
            }
            /*
             * A chipset part carries no subsystem identity, so leave those
             * registers at zero rather than at the PCI bus default.
             */
            pci_set_word(fn->config + PCI_SUBSYSTEM_VENDOR_ID, 0);
            pci_set_word(fn->config + PCI_SUBSYSTEM_ID, 0);
        }
        isa_bus = intel_82468gx_ifb_isa_bus(s->ifb);
        for (i = 0; i < ISA_NUM_IRQS; i++) {
            s->isa_irqs[i] = qdev_get_gpio_in(iosapic, i);
            qdev_connect_gpio_out_named(DEVICE(s->ifb),
                                        INTEL_82468GX_IFB_GPIO_ISA_IRQ, i,
                                        s->isa_irqs[i]);
        }
        /*
         * The bridge's 8259 pair drives INTR, which a processor takes as an
         * ExtINT.  Without this the pair answers its ports but can deliver
         * nothing, and firmware that runs the legacy tick through the PIC --
         * as the vendor firmware does during POST -- never sees an interrupt.
         */
        s->extint = qemu_allocate_irq(ia64_vpc_extint, s, 0);
        qdev_connect_gpio_out_named(DEVICE(s->ifb),
                                    INTEL_82468GX_IFB_GPIO_LEGACY, 0,
                                    s->extint);
    } else {
        isa_bus = isa_bus_new(NULL, get_system_memory(), pci_io, errp);
        if (isa_bus == NULL) {
            return false;
        }
        for (i = 0; i < ISA_NUM_IRQS; i++) {
            s->isa_irqs[i] = qdev_get_gpio_in(iosapic, i);
        }
        isa_bus_register_input_irqs(isa_bus, s->isa_irqs);
    }
    if (s->realfw_path != NULL) {
        ia64_vpc_init_realfw_pic(s, isa_bus);
    }
    /*
     * The real-time clock is the standard MC146818 CMOS device at legacy
     * ports 0x70/0x71 (IRQ 8) - the invented MMIO seconds register at
     * 0xFFEF0000 is gone (rework D8).  On the 460GX it belongs to the south
     * bridge, which builds it along with the extended bank at 0x72/0x73 that
     * RTCCFG banks (SSDM 15.5); a machine with no bridge carries its own.
     */
    {
        MC146818RtcState *rtc = s->ifb != NULL ?
            intel_82468gx_ifb_rtc(s->ifb) :
            mc146818_rtc_init(isa_bus, 2000, NULL);

        if (s->realfw_path != NULL) {
            /*
             * Make the century byte (CMOS 0x32) a read-only hardware register,
             * as on the real 460GX RTC.  Late in POST the i2000 SDV firmware
             * probes it by writing 0 (with the RTC halted) and requires it to
             * still read back the century; a writable byte reads back the
             * written 0, the firmware's RTC self-test returns EFI_DEVICE_ERROR,
             * and the zero result count trips a break 1 at POST 0x0a.  Dropping
             * writes keeps the stored century so the probe reads it back.  Set
             * directly rather than via a property because mc146818_rtc_init
             * realizes the device before returning.  realfw-only; guests keep
             * the standard writable byte.
             */
            rtc->century_read_only = true;
            /*
             * The 460GX RTC is a 256-byte part: the standard 128-byte bank is
             * reached through ports 0x70/0x71 (RTCI/RTCD), and ports 0x72/0x73
             * (RTCEI/RTCED) reach the upper 128-byte battery-backed bank ONLY
             * when RTCCFG (IFB function 0, config offset C8h) bit 2 "Upper RAM
             * Enable" is set.  [460GX SSDM 11.1.20, 11.2.5, 15.5.1]  The i2000
             * firmware never writes RTCCFG (the IFB at bus0 dev 0x1e gets no
             * config write to offset C8h), so that bit stays clear and 0x72/
             * 0x73 alias 0x70/0x71 - the SAME 128-byte bank.  POST writes its
             * CMOS configuration and checksum through 0x70/0x71 but reads them
             * back through 0x72/0x73; without this alias every such read is
             * open-bus 0xFF and the CMOS checksum never validates.  (This is
             * distinct from the separate "New CPU frequency is set" reboot,
             * which turns on the firmware's CPU-frequency-detection reads of
             * unmodelled 460GX registers - see plans/phase5 SESSION 15.)
             */
            if (s->ifb == NULL) {
                memory_region_init_alias(&s->realfw_rtc_ext_alias, OBJECT(s),
                                         "rtc-ext-alias", &rtc->io, 0, 2);
                memory_region_add_subregion(isa_bus->address_space_io, 0x72,
                                            &s->realfw_rtc_ext_alias);
            }
        }
    }
#ifdef CONFIG_IA64_VPC_PS2
    if (s->i8042_enabled) {
        ISADevice *i8042 = isa_new(TYPE_I8042);

        /*
         * Model the PS/2 serial transfer latency of the Super I/O KBC (see
         * the LPC47B27 that real Merced platforms carry).  Presenting mouse
         * and keyboard bytes synchronously with the guest port access lets a
         * solicited AUX reply race the psmouse driver's unlocked command
         * bookkeeping across CPUs and fatally dereference a not-yet-installed
         * protocol_handler; the throttle spaces bytes at ~1 ms as on hardware.
         */
        object_property_set_bool(OBJECT(i8042), "kbd-throttle", true,
                                 &error_abort);
        if (!isa_realize_and_unref(i8042, isa_bus, errp)) {
            return false;
        }
    }
#endif

#ifdef CONFIG_IA64_VPC_USB
    if (!ia64_vpc_init_usb(s, pci_bus, errp)) {
        return false;
    }
#endif

    /*
     * The SCSI HBA.  On the i2000 it belongs at 01:00.0 on the first WXB
     * bus, and the adapter the real board carries there is the QLogic
     * ISP12160, so that is what the machine builds by default.  The LSI
     * 53c895a stays available behind lsi=on for images installed before the
     * swap: on its own it takes the seat and the addresses it always had,
     * and alongside the QLogic it parks on the second WXB bus, which is the
     * layout an image is migrated from one adapter to the other on.
     *
     * Whichever adapter holds the seat is created here, before anything
     * else that places itself automatically, so it claims the drives given
     * without an interface and keeps the rest of the map fixed.  Device 4
     * of the compatibility bus, where the LSI used to live, belongs to the
     * CS4281 audio.  zx1 keeps device 4 for the seat.
     */
#ifdef CONFIG_IA64_VPC_STORAGE
    if (s->isp_enabled || s->lsi_enabled) {
        PCIBus *scsi_bus = pci_bus;
        int scsi_devfn = PCI_DEVFN(4, 0);

        if (!ia64_vpc_chipset_is_zx1(s)) {
            scsi_bus = s->expander_bus[IA64_460GX_ROOT_WXB0];
            scsi_devfn = PCI_DEVFN(IA64_460GX_WXB0_SCSI_SLOT, 0);
        }
        if (s->isp_enabled) {
            ia64_vpc_init_isp(s, scsi_bus, scsi_devfn);
        } else if (!ia64_vpc_init_lsi(s, scsi_bus, scsi_devfn, errp)) {
            return false;
        }
    }
#endif

#ifdef CONFIG_IA64_VPC_GRAPHICS
    /*
     * On zx1 with agp=on, give the Rage 128 a PCI AGP capability so Linux
     * sba_iommu reserves the SBA IOVA GART half (and writes the cookie hp-agp
     * handshakes on) and hp-agp can negotiate AGP mode.  The default VGA is
     * created by pci_vga_init() below, which realizes it internally, so opt it
     * in through a global property applied to the ati-vga it creates.  460gx
     * uses the GXB GART instead and never needs this.
     */
    if (ia64_vpc_chipset_is_zx1(s) && s->agp_enabled) {
        static GlobalProperty ati_agp = {
            .driver = "ati-vga", .property = "agp", .value = "on",
        };
        qdev_prop_register_global(&ati_agp);
    }

    /*
     * On zx1 the AGP graphics master sits behind the Mercury PCI host bridge,
     * on its own root bus at slot IA64_MERCURY_VGA_SLOT -- exactly as real zx1
     * puts the AGP adapter behind Mercury.  On 460gx it stays on PCI0 at the
     * GXB-AGP slot.  The built-in device layout (SBA/LSI/AHCI/USB/NIC) is
     * unchanged: those remain on PCI0.
     */
    {
    PCIBus *vga_bus = pci_bus;
    unsigned int vga_slot = IA64_VPC_VGA_SLOT;

    if (ia64_vpc_chipset_is_zx1(s)) {
        vga_bus = s->mercury_bus;
        vga_slot = IA64_MERCURY_VGA_SLOT;
    } else {
        /*
         * The i2000 puts its AGP Pro graphics at 03:00.0, behind the GXB
         * expander -- the 460GX analogue of the zx1 arrangement above.
         */
        vga_bus = s->expander_bus[IA64_460GX_ROOT_GXB];
        vga_slot = IA64_460GX_GXB_VGA_SLOT;
    }

    if (g_strcmp0(s->vga_model, "mach64") == 0) {
        /*
         * The Mach64 3D Rage (DEV_4754): a PCI 2D adapter with no AGP, chosen
         * with -machine ia64-vpc,vga=mach64.  Create it explicitly at the VGA
         * slot rather than through pci_vga_init()/-vga.
         */
        s->vga_dev = pci_new(PCI_DEVFN(vga_slot, 0), "mach64-vga");
        if (!pci_realize_and_unref(s->vga_dev, vga_bus, errp)) {
            return false;
        }
    } else if (g_strcmp0(s->vga_model, "nv15gl") == 0) {
        /*
         * The NVIDIA Quadro2 Pro (NV15GL, 10de:0153): an AGP graphics master
         * with a 16 MB MMIO BAR0 and a 128 MB prefetchable framebuffer BAR1,
         * chosen with -machine ia64-vpc,vga=nv15gl.  Created explicitly at the
         * AGP/VGA slot; ia64_vpc_configure_vga() maps its BARs (NVIDIA layout).
         */
        s->vga_dev = pci_new(PCI_DEVFN(vga_slot, 0), "nv15gl-vga");
        if (!pci_realize_and_unref(s->vga_dev, vga_bus, errp)) {
            return false;
        }
    } else {
        s->vga_dev = pci_vga_init(vga_bus);
    }
    /*
     * The GART scoping above assumes the graphics device is the AGP master at
     * the expected slot.  pci_vga_init() auto-assigns the lowest free slot,
     * which is IA64_MERCURY_VGA_SLOT on the (empty) Mercury bus and
     * IA64_VPC_VGA_SLOT on PCI0; fail loudly if that ever drifts.
     */
    if (s->vga_dev != NULL &&
        s->vga_dev->devfn != PCI_DEVFN(vga_slot, 0)) {
        error_setg(errp, "graphics device landed at devfn %#x, expected %#x",
                   s->vga_dev->devfn, PCI_DEVFN(vga_slot, 0));
        return false;
    }
    }
#endif
    if (!ia64_vpc_enable_vga_legacy_switch(s->vga_dev, errp)) {
        return false;
    }
    ia64_vpc_load_realfw_device_rom(s);
    ia64_vpc_configure_vga(s->vga_dev,
                           s->realfw_path != NULL ? IA64_VGA_IO_BASE_REALFW
                                                  : IA64_VGA_IO_BASE);
    ia64_vpc_map_vga_fixed_windows(s, s->vga_dev);
#ifdef CONFIG_IA64_VPC_GRAPHICS
    if (s->vga_dev != NULL) {
        ia64_vpc_init_int10(s, pci_io);
    }
#endif
    /*
     * Device 4 of the compatibility bus is the i2000's audio seat.  Reserve
     * it before any auto-placed adapter is created, so that the slot map does
     * not depend on whether the CS4281 is switched on: an add-in card must
     * not land there, and add-in cards belong on the WXB buses in any case.
     * The reservation is dropped again below when the CS4281 is created.
     */
    if (!ia64_vpc_chipset_is_zx1(s)) {
        pci_bus_set_slot_reserved_mask(pci_bus,
                                       1U << IA64_460GX_AUDIO_SLOT);
    }

#ifdef CONFIG_IA64_VPC_NETWORK
    ia64_vpc_init_network(s, pci_bus);
#endif

    /*
     * The second SCSI adapter, when both are asked for.  It parks on the
     * second WXB bus so the seat's addresses and interrupt stay with the
     * primary; on zx1 it takes the next free slot of the single root.
     * Created here, after everything that has a fixed seat of its own, so
     * asking for it cannot move another function's BDF.
     */
#ifdef CONFIG_IA64_VPC_STORAGE
    if (s->isp_enabled && s->lsi_enabled) {
        PCIBus *park_bus = pci_bus;
        int park_devfn = -1;

        if (!ia64_vpc_chipset_is_zx1(s)) {
            park_bus = s->expander_bus[IA64_460GX_ROOT_WXB1];
            park_devfn = PCI_DEVFN(IA64_460GX_WXB1_SCSI_SLOT, 0);
        }
        if (!ia64_vpc_init_lsi(s, park_bus, park_devfn, errp)) {
            return false;
        }
    }
#endif

    /*
     * The real i2000 carries a CS4281 codec on its I/O board, so audio=on
     * models a device the platform actually had.  It is off by default:
     * adding a PCI function changes what an installed guest enumerates, and
     * nothing in the firmware needs it.  Created after every other device so
     * enabling it cannot move another function's BDF.
     */
#ifdef CONFIG_IA64_VPC_AUDIO
    if (s->audio_enabled) {
        if (!ia64_vpc_chipset_is_zx1(s)) {
            pci_bus_clear_slot_reserved_mask(pci_bus,
                                             1U << IA64_460GX_AUDIO_SLOT);
        }
        s->audio_dev = pci_create_simple(
            pci_bus,
            ia64_vpc_chipset_is_zx1(s) ? -1 :
                PCI_DEVFN(IA64_460GX_AUDIO_SLOT, 0),
            TYPE_CS4281);
        ia64_vpc_configure_audio(s->audio_dev);
    }
#endif
    pci_bus_clear_slot_reserved_mask(pci_bus, (1U << 0) | (1U << 1));

    /*
     * The Programmable Interrupt Device's face in configuration space.  Its
     * function -- the SAPIC message block every interrupt in the machine is
     * delivered through -- is the IOSAPIC created above; this is the same
     * chip seen by a guest enumerating the compatibility bus, which is where
     * a 460GX platform carries it (SSDM 1.7.2).  It takes slot 0, which the
     * CMD646 vacated when storage moved onto the south bridge.
     */
    if (ia64_vpc_has_south_bridge(s)) {
        if (!pci_realize_and_unref(pci_new(PCI_DEVFN(IA64_460GX_PID_SLOT, 0),
                                           TYPE_IA64_460GX_PID),
                                   pci_bus, errp)) {
            return false;
        }
    }

#ifdef CONFIG_IA64_VPC_STORAGE
    /*
     * The i2000's IDE controller is function 1 of the south bridge, so it is
     * part of the board and not switchable: the ide= option has no effect
     * there, and any if=ide media binds across its two channels.  Both
     * channels are in compatibility mode and decode the fixed legacy ports,
     * so only the bus-master BAR is placed.
     *
     * Elsewhere -- zx1, and realfw mode, which models a south bridge of its
     * own -- ide=on populates the reserved slot 0 with a dual-channel CMD646.
     * Slot 0 is the platform-anticipated home for IDE there: the firmware's
     * fixed PCI-I/O table and the DSDT _PRT both describe an IDE function at
     * that address, and it keeps every other device's BDF stable.  The
     * firmware assigns its I/O BARs on demand, exactly as for a hand-attached
     * -device cmd646-ide.  realfw always instantiates it, because the SDV
     * firmware probes a legacy IDE during POST regardless of the switch and
     * reaches it through the legacy ports aliased below.
     */
    if (s->ifb != NULL) {
        s->ide_dev = intel_82468gx_ifb_function(s->ifb,
                                                IA64_460GX_IFB_IDE_FUNCTION);
        ia64_vpc_configure_ifb_ide(s->ide_dev);
        pci_ide_create_devs(s->ide_dev);
    } else if (s->ide_enabled || s->realfw_path != NULL) {
        s->ide_dev = pci_new(PCI_DEVFN(0, 0), "cmd646-ide");
        qdev_prop_set_uint32(DEVICE(s->ide_dev), "secondary", 1);
        if (!pci_realize_and_unref(s->ide_dev, pci_bus, errp)) {
            return false;
        }
        ia64_vpc_configure_pci_irq(s->ide_dev);
        pci_ide_create_devs(s->ide_dev);
        if (s->realfw_path != NULL) {
            ia64_vpc_map_realfw_legacy_ide(s, pci_io);
        }
    }
#endif

    s->powerdown_notifier.notify = ia64_vpc_powerdown_req;
    qemu_register_powerdown_notifier(&s->powerdown_notifier);

    qemu_register_reset(ia64_vpc_reset, s);
    s->pci_fixup_reset = object_new(TYPE_IA64_PCI_FIXUP_RESET);
    IA64_PCI_FIXUP_RESET(s->pci_fixup_reset)->machine = s;
    qemu_register_resettable(s->pci_fixup_reset);
    if (vmstate_register_with_alias_id(NULL, 0, &vmstate_ia64_vpc, s,
                                       -1, 0, errp) < 0) {
        return false;
    }
    s->vmstate_registered = true;
    return true;
}

static void ia64_vpc_init(MachineState *machine)
{
    Error *err = NULL;

    if (!ia64_vpc_build(machine, &err)) {
        error_propagate(&error_fatal, err);
    }
}

static void ia64_vpc_machine_instance_init(Object *obj)
{
    IA64VpcMachineState *s = IA64_VPC_MACHINE(obj);

    s->fw_map_quirk_disable = IA64_VPC_FW_QUIRK_DEFAULT_DISABLE;

#ifdef CONFIG_IA64_VPC_PS2
    /*
     * The i2000 and the other 460GX workstations carry a Super-I/O PS/2
     * controller, and PS/2 was the input of choice on them, so 460gx keeps
     * it.  The zx1 generation dropped PS/2 entirely: an rx2600 or zx6000
     * has USB keyboard and mouse only, so that machine defaults to the USB
     * HID devices instead (see ia64_vpc_init_usb).  Either default can be
     * overridden with i8042=on|off.
     */
    s->i8042_enabled = !ia64_vpc_chipset_is_zx1(s);
#endif
    s->fw_relocate = true;
#ifdef CONFIG_IA64_VPC_STORAGE
    /*
     * Default the SATA controller off: Windows XP/2003 IA-64 ship no inbox
     * AHCI driver and otherwise see an unidentified PCI device, so the guest
     * that most wants storage is better served booting off the SCSI HBA.
     * Re-enable with ahci=on for SATA-aware guests.  IDE (cmd646) is likewise
     * opt-in via ide=on.
     *
     * The SCSI HBA is the QLogic ISP12160, the adapter the i2000 carries and
     * the one both XP and Server 2003 have an in-box driver for.  The LSI
     * that used to hold that seat is opt-in via lsi=on, for images installed
     * against it.
     */
    s->ahci_enabled = false;
    s->audio_enabled = false;
    s->isp_enabled = true;
    s->lsi_enabled = false;
    s->ide_enabled = false;
    s->firmware_ide_dma = true;
#endif
#ifdef CONFIG_IA64_VPC_GRAPHICS
    s->firmware_console = IA64_FW_CONSOLE_VGA;
#else
    s->firmware_console = IA64_FW_CONSOLE_SERIAL;
#endif
    /* Boot manager waits for the user by default (like the EFI sample). */
    s->firmware_boot_timeout = IA64_FW_BOOT_TIMEOUT_WAIT_FOREVER;
    /*
     * AGP is on by default on both chipsets, as on real hardware.  On 460gx it
     * enables the GXB AGP GART; on zx1 it gives the Rage 128 a PCI AGP
     * capability so sba_iommu reserves the GART half and Linux hp-agp can
     * negotiate AGP mode (reusing the SBA IOPDIR as the GART).  Either way a
     * guest with no AGP driver still does correct DMA -- on zx1 through the SBA
     * in PCI-GART mode, which renders the Rage 128 greeter pixel-perfect.
     */
    s->agp_enabled = true;
    /* Default display adapter: the Rage 128 (honouring -vga); mach64 opt-in. */
    s->vga_model = g_strdup("rage128");
}

static void ia64_vpc_machine_instance_finalize(Object *obj)
{
    IA64VpcMachineState *s = IA64_VPC_MACHINE(obj);

    if (s->vmstate_registered) {
        vmstate_unregister(NULL, &vmstate_ia64_vpc, s);
    }
    g_free(s->nvram_path);
    g_free(s->nvram_resolved_path);
    g_free(s->vga_model);
    g_free(s->realfw_path);
}

/*
 * Shared class-init for the abstract "ia64-base": everything common to both
 * concrete machines.  The concrete 460gx/zx1 class-inits (below) run after this
 * and set the fields that differ -- desc, default CPU, and chipset_profile.
 */
static void ia64_vpc_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    (void)data;

    mc->desc = "IA-64 virtual PC platform (abstract base)";
    mc->init = ia64_vpc_init;
    mc->max_cpus = IA64_VPC_MAX_CPUS;
    mc->default_cpus = 1;
    mc->smp_props.prefer_sockets = true;
    mc->default_ram_size = 1 * GiB;
    mc->default_ram_id = "ia64-vpc.ram";
#ifdef CONFIG_IA64_VPC_GRAPHICS
    mc->default_display = "ati";
#endif
#ifdef CONFIG_IA64_VPC_NETWORK
    /*
     * Default to the 100 Mbit PRO/100 (i82557b, NET557.IN_ / DEV_1229).
     * The 82543GC gigabit adapter (PCI\VEN_8086&DEV_1004&REV_02,
     * e1000w64.sys) remains available via -nic model=e1000-82543gc; the
     * plain e1000 (82540EM, DEV_100E) has no inbox IA-64 driver.
     */
    mc->default_nic = "i82557b";
#endif
#ifdef CONFIG_IA64_VPC_STORAGE
    mc->block_default_type = IF_SCSI;
#else
    mc->block_default_type = IF_NONE;
#endif
    /*
     * The firmware UART is created explicitly below (serial_mm_init with
     * serial_hd(0)); do not also let QEMU synthesise a default serial VC.
     * With -display sdl that default would pop a separate console window
     * streaming the firmware's UART diagnostics -- which reads as a garbled,
     * never-cleared display.  A user who wants the serial still gets it by
     * passing -serial explicitly; no_serial only suppresses the auto VC.
     */
    mc->no_serial = 1;
    mc->no_parallel = 1;
    mc->no_floppy = 1;
    mc->no_cdrom = 1;

    /*
     * mc->compat_props is allocated by machine_class_base_init only for
     * concrete (non-abstract) machine classes, so the compat defaults are
     * added by each concrete class-init below, not here on the abstract base.
     */

    object_class_property_add_bool(oc, "fw-relocate",
                                   ia64_vpc_get_fw_relocate,
                                   ia64_vpc_set_fw_relocate);
    object_class_property_set_description(oc, "fw-relocate",
        "Shadow the firmware image at the top of low RAM (default on; "
        "off keeps the historical 1 MB execution home - the A/B lever "
        "for plans/firmware-rework-plan.md phase 2.2)");
    object_class_property_add_bool(oc, "i8042",
                                   ia64_vpc_get_i8042,
                                   ia64_vpc_set_i8042);
    object_class_property_set_description(oc, "i8042",
        "Set on/off to enable/disable the i8042 PS/2 controller");
    object_class_property_add_bool(oc, "isp",
                                   ia64_vpc_get_isp,
                                   ia64_vpc_set_isp);
    object_class_property_set_description(oc, "isp",
        "Set on/off to enable/disable the QLogic ISP12160 SCSI controller "
        "(default on; it holds the platform's SCSI seat)");
    object_class_property_add_bool(oc, "lsi",
                                   ia64_vpc_get_lsi,
                                   ia64_vpc_set_lsi);
    object_class_property_set_description(oc, "lsi",
        "Add the LSI 53c895a SCSI controller (default off; it takes the "
        "SCSI seat when isp=off, and parks on the second expander bus "
        "otherwise)");
    object_class_property_add_bool(oc, "audio",
                                   ia64_vpc_get_audio,
                                   ia64_vpc_set_audio);
    object_class_property_set_description(oc, "audio",
        "Add the CS4281 PCI audio controller (default off)");
    object_class_property_add_bool(oc, "ahci",
                                   ia64_vpc_get_ahci,
                                   ia64_vpc_set_ahci);
    object_class_property_set_description(oc, "ahci",
        "Set on/off to enable/disable the AHCI SATA controller (default off; "
        "on adds a PCI device that guests without a SATA driver cannot use)");
    object_class_property_add_bool(oc, "ide",
                                   ia64_vpc_get_ide,
                                   ia64_vpc_set_ide);
    object_class_property_set_description(oc, "ide",
        "Set on/off to enable/disable the CMD646 PCI IDE controller "
        "(default off; on adds a dual-channel ATA/ATAPI controller in slot 0 "
        "and auto-attaches if=ide drives)");
    object_class_property_add_bool(oc, "agp",
                                   ia64_vpc_get_agp,
                                   ia64_vpc_set_agp);
    object_class_property_set_description(oc, "agp",
        "AGP support (default on for both chipsets, as on real hardware). On "
        "460gx it enables the GXB AGP GART; off makes the Rage 128 fall back to "
        "its 32-bit PCI GART (clean 2D, but graphics DMA cannot reach RAM above "
        "4 GiB). On zx1 it gives the Rage 128 a PCI AGP capability so Linux "
        "hp-agp negotiates AGP mode reusing the SBA IOPDIR as the GART; off "
        "keeps the Rage 128 on the SBA's PCI-GART path (which already reaches "
        ">4 GiB)");
    object_class_property_add_str(oc, "vga",
                                  ia64_vpc_get_vga,
                                  ia64_vpc_set_vga);
    object_class_property_set_description(oc, "vga",
        "Display adapter: 'rage128' (default, ATI Rage 128, honours -vga), "
        "'mach64' (ATI Mach64 3D Rage, a PCI 2D adapter with no AGP), or "
        "'std'");
    object_class_property_add_bool(oc, "firmware-ide-dma",
                                   ia64_vpc_get_firmware_ide_dma,
                                   ia64_vpc_set_firmware_ide_dma);
    object_class_property_set_description(oc, "firmware-ide-dma",
        "Set on/off to enable/disable firmware IDE bus-master DMA");
    object_class_property_add_str(oc, "fw-quirks",
                                  ia64_vpc_get_fw_quirks,
                                  ia64_vpc_set_fw_quirks);
    object_class_property_set_description(oc, "fw-quirks",
        "Comma list of firmware memory-map quirks to toggle: '-name' "
        "disables, '+name'/'name' re-enables, 'default' resets.  Names: "
        "split-page, low-boundaries, low-anchor, anchor-version-sniff, "
        "2g-scratch, pal-8k-page, acpi-low-island.  Retired quirks "
        "(acpi-low-island, 2g-scratch, low-boundaries, low-anchor, "
        "anchor-version-sniff) default off, the rest default on; "
        "toggling changes the guest-visible EFI memory map -- "
        "A/B rig for plans/firmware-rework-plan.md Phase 2");
    object_class_property_add_str(oc, "firmware-console",
                                  ia64_vpc_get_firmware_console,
                                  ia64_vpc_set_firmware_console);
    object_class_property_set_description(oc, "firmware-console",
        "Set firmware HCDP primary console to 'serial' or 'vga'");
    object_class_property_add(oc, "firmware-boot-timeout", "uint16",
                              ia64_vpc_get_boot_timeout,
                              ia64_vpc_set_boot_timeout, NULL, NULL);
    object_class_property_set_description(oc, "firmware-boot-timeout",
        "Default boot-manager Timeout in seconds when no NVRAM 'Timeout' "
        "variable exists: 0 boots the BootOrder immediately, 0xFFFF (the "
        "default) waits for the user like the EFI sample.");
    object_class_property_add_str(oc, "realfw",
                                  ia64_vpc_get_realfw,
                                  ia64_vpc_set_realfw);
    object_class_property_set_description(oc, "realfw",
        "Path to a real vendor flash image (e.g. the HP i2000 bios130.BIN) "
        "to map ending at 4 GiB and enter at its architected SALE_ENTRY "
        "pointer with synthesized PALE_RESET exit state, instead of the "
        "project firmware (plans/phase5-real-firmware-boot.md)");
    object_class_property_add_str(oc, "realfw-vga-rom",
                                  ia64_vpc_get_realfw_vga_rom,
                                  ia64_vpc_set_realfw_vga_rom);
    object_class_property_set_description(oc, "realfw-vga-rom",
        "Path to a real video-card option ROM to shadow at 0xC0000 for the "
        "realfw video POST, instead of the emulated card's own vgabios.  Used "
        "to run the vendor firmware against an authentic card BIOS (e.g. the "
        "ATI Rage 128 Pro the SDV shipped with); realfw mode only.");
    object_class_property_add_str(oc, "realfw-nvram",
                                  ia64_vpc_get_realfw_nvram,
                                  ia64_vpc_set_realfw_nvram);
    object_class_property_set_description(oc, "realfw-nvram",
        "Path to a writable file that persists the realfw flash (the firmware's "
        "NVRAM/EFI-variable store) across runs.  Created from the realfw image "
        "on first use; thereafter the flash is loaded from and written back to "
        "it.  realfw mode only.");
    object_class_property_add_str(oc, "nvram",
                                  ia64_vpc_get_nvram,
                                  ia64_vpc_set_nvram);
    object_class_property_set_description(oc, "nvram",
        "Set the IA-64 EFI NVRAM file path, 'auto', or 'none'");
    object_class_property_add_str(oc, "alat",
                                  ia64_vpc_get_alat,
                                  ia64_vpc_set_alat);
    object_class_property_set_description(oc, "alat",
        "Set the IA-64 ALAT model to 'zero' (default) or 'full'");
}

/* Concrete: Intel SDV / HP i2000 -- 460GX chipset, Merced. */
static void ia64_460gx_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    IA64VpcMachineClass *imc = IA64_VPC_MACHINE_CLASS(oc);

    (void)data;
    mc->desc = "Intel SDV / HP i2000 (460GX chipset, Merced)";
    mc->default_cpu_type = IA64_CPU_TYPE_NAME("merced");
    imc->chipset_profile = IA64_FW_CHIPSET_460GX;
    ia64_vpc_add_compat_defaults(mc);
}

/* Concrete: HP rx2600 / zx2000 / zx6000 -- zx1 chipset, Itanium 2. Default. */
static void ia64_zx1_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    IA64VpcMachineClass *imc = IA64_VPC_MACHINE_CLASS(oc);

    (void)data;
    mc->desc = "HP rx2600 / zx2000 / zx6000 (zx1 chipset, Itanium 2)";
    mc->default_cpu_type = IA64_CPU_TYPE_NAME("madison");
    imc->chipset_profile = IA64_FW_CHIPSET_ZX1;
    /* The zx1 machine is the default; "ia64-vpc" is a deprecated alias of it. */
    mc->is_default = true;
    mc->alias = "ia64-vpc";
    ia64_vpc_add_compat_defaults(mc);
}

static const TypeInfo ia64_machine_typeinfos[] = {
    {
        .name = TYPE_IA64_VPC_MACHINE,
        .parent = TYPE_MACHINE,
        .abstract = true,
        .instance_size = sizeof(IA64VpcMachineState),
        .instance_init = ia64_vpc_machine_instance_init,
        .instance_finalize = ia64_vpc_machine_instance_finalize,
        .class_size = sizeof(IA64VpcMachineClass),
        .class_init = ia64_vpc_machine_class_init,
    },
    {
        .name = TYPE_IA64_460GX_MACHINE,
        .parent = TYPE_IA64_VPC_MACHINE,
        .class_init = ia64_460gx_machine_class_init,
    },
    {
        .name = TYPE_IA64_ZX1_MACHINE,
        .parent = TYPE_IA64_VPC_MACHINE,
        .class_init = ia64_zx1_machine_class_init,
    },
};

DEFINE_TYPES(ia64_machine_typeinfos)
