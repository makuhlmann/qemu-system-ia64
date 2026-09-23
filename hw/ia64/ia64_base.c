/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * IA-64 machine, the abstract base ("ia64-base") of the two boards.
 *
 * Provides RAM, the CPUs, the memory-mapped serial console, firmware
 * loading via -bios, the primary PCI host bridge, SCSI and AHCI storage
 * controllers, an Ethernet controller, OHCI/UHCI USB, local SAPIC/I/O SAPIC
 * wiring, and ACPI fixed power-management registers.  What differs between
 * the boards -- the core chipset and its further PCI roots, the south
 * bridge, INTx wiring, device seats -- comes from the concrete class
 * (sdv.c for "460gx", longspeak.c for "zx1"; see ia64_vpc_internal.h).
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
#include "hw/isa/smsc_lpc47b27x.h"
#include "hw/southbridge/intel_82468gx.h"
#include "hw/ia64/ia64_460gx_identity.h"
#include "ia64_vpc_internal.h"
#include "longspeak_pdh.h"
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
#include "hw/ia64/ia64_pci.h"
#include "hw/ia64/ia64_iosapic.h"
#include "hw/ia64/ia64_agp.h"
#include "hw/ia64/ia64_sba.h"
#include "hw/ia64/ia64_lba.h"
#include "hw/ia64/ia64_mercury.h"
#include "hw/ia64/ia64_expander.h"
#include "hw/i2c/i2c.h"
#include "hw/ia64/ia64_i2000_hwmon.h"
#include "hw/core/or-irq.h"
#include "migration/vmstate.h"
#include "system/address-spaces.h"
#include "system/rtc.h"
#include "system/runstate.h"
#include "system/system.h"
#include "system/reset.h"
#include "target/ia64/cpu-qom.h"
#include "target/ia64/cpu.h"

/*
 * Firmware image loaded when no -bios is given.  It is installed beside the
 * binary (share/), so an unpacked package runs without naming it every time.
 */
#define IA64_VPC_DEFAULT_FIRMWARE "ia64-firmware.bin"
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
#define IA64_REALFW_MAX_SIZE      IA64_U64(0x0000000000800000)
/*
 * The PAL emulation ROM: the PAL procedure entry handed to SAL in GR34/GR36
 * (and recognized via env->pal.pal_proc_reset_addr) is its first 32 bytes.
 */
#define IA64_PAL_ROM_BASE         IA64_U64(0x00000000ff100000)
#define IA64_PAL_ROM_SIZE         0x1000
/* PAL_RESET's return address for SAL's RECOVERY_CHECK call, in that ROM. */
#define IA64_PAL_RESET_RETURN     (IA64_PAL_ROM_BASE + 0x20)
/*
 * The reset IVT: a 32 KiB-aligned interruption vector table whose every
 * bundle is a branch-to-self, pointed to by cr.iva in the SALE_ENTRY entry
 * state.  Real PAL provides an IVT before entering SAL (SDM 11.2.2); the
 * machine plays PAL, so without this any firmware fault would vector to
 * physical 0 (no handler) and, under the bare-loader ic=0/iva=0 rule,
 * storm.  With it, a fatal fault instead freezes at the IVT base + vector
 * with all GRs, the RSE frame, ISR and IIPA preserved - the fault class is
 * the offset from the IVT base, and the interrupted state is inspectable
 * via the monitor.  See plans/phase5-real-firmware-boot.md.
 */
#define IA64_PAL_RESET_IVT_BASE   IA64_U64(0x00000000ff300000)
#define IA64_PAL_RESET_IVT_SIZE   0x8000
#define IA64_REALFW_PTR_FIT       (IA64_REALFW_WINDOW_END - 32)
#define IA64_REALFW_PTR_SALE      (IA64_REALFW_WINDOW_END - 24)
/* Bit 63 in firmware pointers is the uncacheable-attribute flag, not
 * part of the physical address (SAL sec 2.5). */
#define IA64_REALFW_PTR_ADDR_MASK (~(IA64_U64(1) << 63))
/* FIT entry types: the OEM NVRAM block (SDV FIT, 1Eh) and an unused slot. */
#define IA64_FIT_TYPE_NVRAM       0x1e
#define IA64_FIT_TYPE_UNUSED      0x7f
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
/*
 * The vendor ATI Rage 128 vgabios hardcodes its register I/O base at 0xD800 and
 * only falls back to a port-space scan if a signature probe there fails, so the
 * card's I/O BAR lives there: it is the address the card's own BIOS expects to
 * find it at, and a guest reads the BAR from config space and follows.  It is
 * inside the graphics root's I/O segment either way (0xD000-0xDFFF, see
 * roms/ia64-firmware/dsdt-pci-root.asl).
 */
#define IA64_VGA_IO_BASE        0x0000d800U
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
#define IA64_INT10_ROM_PCIR_OFFSET    0x0060U
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
#define IA64_PIB_IPI_LIMIT          0x00100000ULL
#define IA64_PIB_INTA_OFFSET        0x001e0000ULL
#define IA64_PIB_XTP_OFFSET         0x001e0008ULL
/* Graphics (Rage 128) lands here: slots 0-4 are reserved/built-in, VGA next. */
/*
 * The i2000 carries its 82559 Ethernet at 00:05.0, the slot the graphics
 * adapter used to occupy before it moved to the GXB root.  zx1 keeps the
 * adapter where it was.
 */

#define IA64_SAPIC_DELIVERY_INT     0
#define IA64_SAPIC_DELIVERY_NMI     4
#define IA64_SAPIC_DELIVERY_EXTINT  7

#ifdef CONFIG_IA64_VPC_GRAPHICS

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

/*
 * The ATI BIOS header and PLL info block as the Rage 128 miniport
 * (ati2mpaa) consumes them.  Traced from the driver on both firmwares: it
 * copies 82 bytes of header, follows header+14h to a 12-byte table and
 * header+30h to the PLL block, and reads 50 (32h) bytes of the latter:
 * +08h XCLK, +0Ah a second clock, +0Eh reference frequency, +10h reference
 * divider, +12h/+16h the PLL range, +22h a 32-bit clock limit and +2Eh/+30h
 * a 16-bit pair it recombines into another.  A block that stops at +20h
 * leaves the last three as whatever follows it in the image - which is why
 * the 2 KB synthetic image "worked" (its PCIR structure supplied non-zero
 * bytes) while the CSM-shadowed PCI ROM did not (zero padding): the memory
 * clock came out as 0 and every mode-set was refused.  Both builders now
 * publish the full block; the table pointer at +14h aims at the zeroed
 * header itself (12 bytes of zeros are all the driver needs from it).
 * Values are in the 10 kHz units of the Rage 128 BIOS interface.
 */
#define IA64_ATI_HDR_SIZE  0x40U
#define IA64_ATI_PLL_SIZE  0x32U

static void ia64_ati_write_bios_tables(uint8_t *rom, uint32_t hdr, uint32_t pll)
{
    memset(rom + hdr, 0, IA64_ATI_HDR_SIZE);
    memset(rom + pll, 0, IA64_ATI_PLL_SIZE);
    stw_le_p(rom + hdr + 0x14, hdr);
    stw_le_p(rom + hdr + 0x30, pll);
    stw_le_p(rom + pll + 0x08, IA64_ATI_PLL_XCLK);
    stw_le_p(rom + pll + 0x0a, IA64_ATI_PLL_XCLK);
    stw_le_p(rom + pll + 0x0e, IA64_ATI_PLL_REFERENCE_FREQ);
    stw_le_p(rom + pll + 0x10, IA64_ATI_PLL_REFERENCE_DIV);
    stl_le_p(rom + pll + 0x12, IA64_ATI_PLL_MIN_FREQ);
    stl_le_p(rom + pll + 0x16, IA64_ATI_PLL_MAX_FREQ);
    stl_le_p(rom + pll + 0x22, IA64_ATI_PLL_MAX_FREQ);
    stw_le_p(rom + pll + 0x2e, IA64_ATI_PLL_MAX_FREQ & 0xffffU);
    stw_le_p(rom + pll + 0x30, IA64_ATI_PLL_MAX_FREQ >> 16);
}

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
    ia64_ati_write_bios_tables(rom, IA64_INT10_ROM_ATI_HEADER_OFFSET,
                               IA64_INT10_ROM_ATI_PLL_OFFSET);
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
    g_assert(IA64_INT10_ROM_PCIR_OFFSET + 0x18 <=
             IA64_INT10_ROM_ATI_HEADER_OFFSET);
    g_assert(IA64_INT10_ROM_ATI_HEADER_OFFSET + IA64_ATI_HDR_SIZE <=
             IA64_INT10_ROM_ATI_PLL_OFFSET);
    g_assert(IA64_INT10_ROM_ATI_PLL_OFFSET + IA64_ATI_PLL_SIZE <=
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
    g_assert(IA64_INT10_ROM_MODES_OFFSET + (G_N_ELEMENTS(ia64_vbe_modes) + 1) * 2 +
             20 < sizeof(rom) - 1);
    memcpy(rom + IA64_INT10_ROM_MODES_OFFSET + (G_N_ELEMENTS(ia64_vbe_modes) + 1) * 2,
           "QEMU IA64 VBE INT10", 20);

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

static char *ia64_vpc_get_nvram(Object *obj, Error **errp)
{
    IA64VpcMachineState *s = IA64_VPC_MACHINE(obj);

    (void)errp;

    return g_strdup(s->nvram_path ?: "none");
}

/* nvram=<file> persists the flash; none (or auto, accepted) keeps it volatile. */
static void ia64_vpc_set_nvram(Object *obj, const char *value, Error **errp)
{
    IA64VpcMachineState *s = IA64_VPC_MACHINE(obj);

    (void)errp;

    g_free(s->nvram_path);
    s->nvram_path = g_strcmp0(value, "auto") == 0 ||
                    g_strcmp0(value, "none") == 0 || value[0] == '\0' ?
                    NULL : g_strdup(value);
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

void ia64_vpc_add_compat_defaults(MachineClass *mc)
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



static uint64_t ia64_vpc_fw_base(IA64VpcMachineState *s, uint64_t ram_size)
{
    return s->fw_relocate ? IA64_FW_IMAGE_BASE_FOR(ram_size)
                          : IA64_FW_LINK_BASE;
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
        g_strcmp0(value, "none") != 0 &&
        g_strcmp0(value, "std") != 0) {
        error_setg(errp,
                   "vga must be 'rage128', 'mach64', 'nv15gl', 'std' or "
                   "'none'");
        return;
    }
    g_free(s->vga_model);
    s->vga_model = g_strdup(value);
    s->vga_model_set = true;
}

/*
 * Which adapter the board gets.  A board whose own graphics is one of the
 * fork's adapters names it as the class default, which leaves -vga with
 * nothing to say; -vga still selects when the user has not asked for an
 * adapter by name, and -vga none always wins so that a run can be made with
 * no display at all.  VGA_ATI is what mc->default_display asks for, so
 * anything else means the user chose the display: with -vga, or with
 * -device, which asks for VGA_DEVICE and leaves pci_vga_init() to create
 * nothing.  NULL is that case: whatever -vga asks for.
 */
const char *ia64_vpc_vga_model(IA64VpcMachineState *s)
{
    if (vga_interface_type == VGA_NONE) {
        return "none";
    }
    if (!s->vga_model_set && vga_interface_type != VGA_ATI) {
        return NULL;
    }
    return s->vga_model;
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
    acpi_pm1_cnt_init(&s->acpi_regs, &s->acpi_pm, false, false,
                      IA64_VPC_MACHINE_GET_CLASS(s)->acpi_s5_slp_typ, true);
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

    if (IA64_VPC_MACHINE_GET_CLASS(s)->acpi_pm_mmio_base != 0) {
        static const struct { unsigned int from, to, size; } mmio[] = {
            { IA64_ACPI_PM_TMR_OFFSET,  IA64_PDH_ACPI_PM_TMR,  4 },
            { IA64_ACPI_PM1_EVT_OFFSET, IA64_PDH_ACPI_PM1_EVT, 4 },
            { IA64_ACPI_PM1_CNT_OFFSET, IA64_PDH_ACPI_PM1_CNT, 2 },
        };
        uint64_t base = IA64_VPC_MACHINE_GET_CLASS(s)->acpi_pm_mmio_base;
        unsigned int i;

        QEMU_BUILD_BUG_ON(ARRAY_SIZE(mmio) != ARRAY_SIZE(s->acpi_pm_mmio));
        for (i = 0; i < ARRAY_SIZE(mmio); i++) {
            memory_region_init_alias(&s->acpi_pm_mmio[i], OBJECT(s),
                                     "ia64-acpi-pm-mmio", &s->acpi_pm,
                                     mmio[i].from, mmio[i].size);
            memory_region_add_subregion_overlap(get_system_memory(),
                                                base + mmio[i].to,
                                                &s->acpi_pm_mmio[i], 1);
        }
    }
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

/*
 * The top 16 MiB below 4 GiB are the firmware region (SSDM Table 4-1:
 * FF00_0000-FFFF_FFFF).  The flash decodes at its top; below it, nothing on
 * the real boards answers.  The machine plants two things there, because it
 * plays PAL (PAL is emulated, so its entry and its reset IVT have to exist
 * somewhere): the PAL emulation ROM at FF10_0000, whose first 32-byte stub
 * is the PAL procedure entry handed to SAL in GR34 (and in GR36 on the RESET
 * call) and whose second is PAL_RESET's return address, GR36 on the
 * RECOVERY_CHECK call; and the reset IVT at FF30_0000, cr.iva at the RESET
 * call.  Both are read-only and present for every firmware.
 */
static bool ia64_vpc_map_firmware_address_space(IA64VpcMachineState *s,
                                                Error **errp)
{
    if (!memory_region_init_rom(&s->pal_rom, NULL,
                                "ia64-pal-emulation-rom",
                                IA64_PAL_ROM_SIZE, errp) ||
        !memory_region_init_rom(&s->pal_reset_ivt, NULL,
                                "ia64-pal-reset-ivt",
                                IA64_PAL_RESET_IVT_SIZE, errp)) {
        return false;
    }
    memory_region_add_subregion(get_system_memory(), IA64_PAL_ROM_BASE,
                                &s->pal_rom);
    memory_region_add_subregion(get_system_memory(), IA64_PAL_RESET_IVT_BASE,
                                &s->pal_reset_ivt);
    return true;
}

uint64_t ia64_vpc_map_ram_alias(IA64VpcMachineState *s,
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
    alias = &s->ram_aliases[s->ram_alias_count++];
    memory_region_init_alias(alias, OBJECT(s), name, machine->ram,
                             backing_offset, size);
    memory_region_add_subregion(get_system_memory(), guest_base, alias);
    return size;
}

static void ia64_vpc_map_ram(IA64VpcMachineState *s)
{
    MachineState *machine = MACHINE(s);
    IA64VpcMachineClass *imc = IA64_VPC_MACHINE_GET_CLASS(s);
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
    if (s->low_ram_limit == 0) {
        s->low_ram_limit = IA64_LOW_RAM_LIMIT;
    }
    if (imc->map_low_ram != NULL) {
        size = imc->map_low_ram(s, offset, remaining);
        offset += size;
        remaining -= size;
    } else {        size = ia64_vpc_map_ram_alias(s, 0, offset, remaining,
                                      s->low_ram_limit,
                                      "ia64-vpc.low-ram");
        offset += size;
        remaining -= size;
    }

    ia64_vpc_map_ram_alias(s, IA64_HIGH_RAM_AFTER_FIRMWARE_BASE,
                           offset, remaining, remaining,
                           "ia64-vpc.high-ram-above-4g");
}

/*
 * Move the top of the low DRAM band.  The 460GX decodes "10_0000h - PCIS[7]"
 * to DRAM and "PCIS[7] - FDFF_FFFFh" to PCI, with DRAM again from
 * "1_0000_0000h to TOM" (SSDM Table 4-1): memory behind the variable gap
 * "is moved so that it is addressed above 4 GB" (4.1.5).  The vendor
 * firmware sizes memory, then programs the ports' PCIS from what it found
 * -- 2 GB for 4 GB of DIMMs, which its TOM of 6 GB confirms -- so the band
 * follows the lowest PCIS rather than the static aperture our own firmware
 * assumes.  Everything DRAM-backed is re-aliased in one transaction; the
 * backing store is untouched, so the bytes stay where the guest wrote them
 * in the DRAM's own address order.
 */
void ia64_vpc_set_low_ram_limit(IA64VpcMachineState *s, uint64_t limit)
{
    unsigned i;

    if (limit == s->low_ram_limit || MACHINE(s)->ram == NULL) {
        return;
    }
    memory_region_transaction_begin();
    for (i = 0; i < s->ram_alias_count; i++) {
        memory_region_del_subregion(get_system_memory(), &s->ram_aliases[i]);
        object_unparent(OBJECT(&s->ram_aliases[i]));
    }
    s->ram_alias_count = 0;
    s->low_ram_limit = limit;
    ia64_vpc_map_ram(s);
    memory_region_transaction_commit();
}

/*
 * The firmware defaults record: a factory-programmed setup block in the
 * flash's NVRAM sector that carries the machine's console, IDE DMA, boot
 * timeout and memory-map policies to the project firmware.  Only an image
 * that ships the record (its magic in the -bios file) gets it refreshed
 * from the options, whatever the persisted sector holds; the vendor
 * image's own NVRAM sector is left alone.
 */
static bool ia64_vpc_nvram_defaults_record(IA64VpcMachineState *s,
                                           IA64NvramDefaults *defaults)
{
    uint64_t base = IA64_REALFW_WINDOW_END - s->fw_image_size;
    uint64_t record = IA64_NVRAM_BASE + IA64_NVRAM_DEFAULTS_OFFSET;

    *defaults = (IA64NvramDefaults) {
        .Magic = cpu_to_le64(IA64_NVRAM_DEFAULTS_MAGIC),
        .Version = cpu_to_le64(IA64_NVRAM_DEFAULTS_VERSION),
        .ConsolePolicy = cpu_to_le64(s->firmware_console),
        .IdeDmaEnabled = cpu_to_le64(s->firmware_ide_dma),
        .BootTimeout = cpu_to_le64(s->firmware_boot_timeout),
        .MapQuirkDisable = cpu_to_le64(s->fw_map_quirk_disable),
    };
    return record >= base &&
           record + sizeof(*defaults) <= IA64_REALFW_WINDOW_END &&
           ldq_le_p(s->fw_image + (record - base)) ==
               IA64_NVRAM_DEFAULTS_MAGIC;
}

static void ia64_vpc_seed_nvram_defaults(IA64VpcMachineState *s,
                                         uint8_t *flash, uint64_t flash_base)
{
    IA64NvramDefaults defaults;
    uint64_t record = IA64_NVRAM_BASE + IA64_NVRAM_DEFAULTS_OFFSET;

    if (!ia64_vpc_nvram_defaults_record(s, &defaults) || record < flash_base) {
        return;
    }
    memcpy(flash + (record - flash_base), &defaults, sizeof(defaults));
}

/* The same record, where a board keeps the store outside the flash. */
void ia64_vpc_seed_store_defaults(IA64VpcMachineState *s, uint8_t *store)
{
    IA64NvramDefaults defaults;

    if (!ia64_vpc_nvram_defaults_record(s, &defaults)) {
        return;
    }
    memcpy(store + IA64_NVRAM_DEFAULTS_OFFSET, &defaults, sizeof(defaults));
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
        unsigned int line;

        if (gsi_base == IA64_460GX_INTX_FALLBACK_GSI) {
            /* A 460gx root: its bus maps the pin straight to a PID input. */
            line = pci_get_bus(pci_dev)->map_irq(pci_dev, pin - 1);
        } else {
            line = gsi_base +
                (ia64_pci_route_intx_gsi(pci_dev->devfn, pin - 1) -
                 IA64_PCI_INTX_GSI_BASE);
        }
        pci_default_write_config(pci_dev, PCI_INTERRUPT_LINE, line, 1);
    }
}

/* The interrupt block owned by the root that carries bus @bus. */
static unsigned int ia64_vpc_root_gsi_base(const IA64VpcMachineState *s,
                                           uint8_t bus)
{
    return IA64_VPC_MACHINE_GET_CLASS(s)->root_gsi_base(s, bus);
}

static void ia64_vpc_configure_pci_irq(IA64VpcMachineState *s,
                                       PCIDevice *pci_dev)
{
    ia64_vpc_configure_pci_irq_on_root(pci_dev, ia64_vpc_root_gsi_base(s, 0));
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
 * an IDE controller that is part of the board rather than an option.
 */
static bool ia64_vpc_has_south_bridge(const IA64VpcMachineState *s)
{
    return IA64_VPC_MACHINE_GET_CLASS(s)->has_south_bridge;
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

    /*
     * Keep the header and PLL block inside the first 8 KB of the image.  The
     * Rage 128 miniport (ati2mpaa) maps the C0000h shadow with a single
     * VideoPortGetDeviceBase(0xC0000, 256) call - one 8 KB IA-64 page - and
     * then follows the 48h -> header -> header+30h -> PLL pointer chain
     * through that mapping; tables appended past the page (the previous
     * placement at the declared end, 9A00h for the shipped image) read as
     * whatever the neighbouring system PTEs map: XCLK 0, every mode-set
     * refused, VgaSave at 640x480x4.  The shipped SeaVGABIOS keeps a zero
     * run at 50h-150h (VGA_ATI_TABLES off, roms/config.vga-ati) and leaves
     * 48h to us; take the first zero run below 2000h that holds the
     * 40h-byte header plus the 32h-byte PLL block (72h), and
     * only if none exists fall back to appending.
     */
    hdr = 0;
    for (i = 0x50; i + IA64_ATI_HDR_SIZE + IA64_ATI_PLL_SIZE <= 0x2000U &&
         i + IA64_ATI_HDR_SIZE + IA64_ATI_PLL_SIZE <= declared; i += 16) {
        uint32_t z;

        for (z = 0; z < IA64_ATI_HDR_SIZE + IA64_ATI_PLL_SIZE &&
             rom[i + z] == 0; z++) {
            continue;
        }
        if (z == IA64_ATI_HDR_SIZE + IA64_ATI_PLL_SIZE) {
            hdr = i;
            break;
        }
    }
    if (hdr == 0) {
        hdr = declared;
    }
    pll = hdr + IA64_ATI_HDR_SIZE;
    if (pll + IA64_ATI_PLL_SIZE > rom_size) {
        return;
    }

    memcpy(rom + 0x30, ati_signature, sizeof(ati_signature) - 1);
    stw_le_p(rom + 0x48, hdr);
    ia64_ati_write_bios_tables(rom, hdr, pll);

    /* Grow the declared image so a bounds-checking parser sees the tables. */
    if (pll + IA64_ATI_PLL_SIZE > declared) {
        declared = ROUND_UP(pll + IA64_ATI_PLL_SIZE, 512U);
        if (declared > rom_size || declared / 512U > 0xffU) {
            return;
        }
        rom[2] = (uint8_t)(declared / 512U);
    }
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

/* Where the board seats a built-in device: *bus and *devfn preset to defaults. */
/*
 * Which adapter holds the board's SCSI seat when both are present: the one
 * the board itself carries.  The other parks.
 */
static bool ia64_vpc_lsi_at_seat(IA64VpcMachineState *s)
{
    if (!s->isp_enabled) {
        return true;
    }
    return s->lsi_enabled && IA64_VPC_MACHINE_GET_CLASS(s)->lsi_default;
}

static void ia64_vpc_seat(IA64VpcMachineState *s, IA64VpcSeat seat,
                          PCIBus **bus, int *devfn)
{
    IA64_VPC_MACHINE_GET_CLASS(s)->seat(s, seat, bus, devfn);
    if (*devfn >= 0) {
        /*
         * A slot the board names outranks a reservation, which only keeps
         * automatic placement stable across ahci=on/off.
         */
        pci_bus_clear_slot_reserved_mask(*bus, 1U << PCI_SLOT(*devfn));
    }
}

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
                           IA64_VGA_IO_BASE);
    for (unsigned int i = 0; i < s->nic_count; i++) {
        ia64_vpc_configure_nic(s->nic_devs[i], i);
    }
    ia64_vpc_configure_pci_irq(s, s->ahci_dev);
    ia64_vpc_configure_pci_irq(s, s->audio_dev);
    ia64_vpc_configure_pci_irq_on_root(
        s->isp_dev,
        ia64_vpc_root_gsi_base(s, IA64_460GX_WXB0_BUS));
    ia64_vpc_configure_pci_irq(s, s->ide_dev);
    ia64_vpc_configure_pci_irq(s, s->ohci_dev);
    ia64_vpc_configure_pci_irq(s, s->uhci_dev);
    ia64_vpc_configure_pci_irq(s,
        intel_82468gx_ifb_function(s->ifb, IA64_460GX_IFB_IDE_FUNCTION));
    ia64_vpc_configure_pci_irq(s,
        intel_82468gx_ifb_function(s->ifb, IA64_460GX_IFB_SMBUS_FUNCTION));
    ia64_vpc_configure_pci_irq_on_root(
        s->lsi_dev,
        ia64_vpc_root_gsi_base(s, s->isp_enabled ? IA64_460GX_WXB1_BUS :
                                                   IA64_460GX_WXB0_BUS));
    ia64_vpc_configure_pci_irq_on_root(
        s->vga_dev,
        ia64_vpc_root_gsi_base(s, IA64_460GX_GXB_BUS));
    for (unsigned int i = 0; i < s->nic_count; i++) {
        ia64_vpc_configure_pci_irq(s, s->nic_devs[i]);
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
    ia64_vpc_configure_pci_irq(s, pci_dev);
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
    {
        PCIBus *bus = pci_bus;
        int devfn = -1;

        ia64_vpc_seat(s, IA64_VPC_SEAT_NIC, &bus, &devfn);
        first_slot = PCI_SLOT(devfn);
    }
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


/*
 * CPU state initialization — called on every reset.
 *
 * Sets up the CPU in physical mode with firmware entry point.
 * Note: ROM content is loaded by rom_reset() which may run before or
 * after this handler, so we must NOT read ROM content here.  PE32+
 * plabel parsing is deferred to the machine_done notifier.
 */
static void ia64_vpc_reset(void *opaque)
{
    IA64VpcMachineState *s = opaque;
    CPUState *cs;

    CPU_FOREACH(cs) {
        /* The CPUs are not children of the platform system bus. */
        ia64_cpu_reset_to_boot_info(IA64_CPU(cs));
    }

    /* The 460GX chipset re-seeds its configuration store in its own reset. */

    if (!IA64_VPC_MACHINE_GET_CLASS(s)->has_south_bridge) {
        acpi_pm1_evt_reset(&s->acpi_regs);
        acpi_pm1_cnt_reset(&s->acpi_regs);
        acpi_pm_tmr_reset(&s->acpi_regs);
        acpi_gpe_reset(&s->acpi_regs);
    }
#ifdef CONFIG_IA64_VPC_GRAPHICS
    /*
     * The synthetic INT10 ROM: a passive 2 KiB image at the legacy video-ROM
     * window for guests that read the video BIOS through the PCI ROM BAR.
     * What sits at 0xC0000 is firmware's business rather than the machine's --
     * a firmware that POSTs the card's own option ROM the legacy PC-AT way
     * shadows it over this -- so put the same image there whichever firmware
     * is about to run.
     */
    if (s->vga_dev != NULL) {
        ia64_vpc_reset_int10(s);
    }
#endif
}

/*
 * Machine-done notifier — runs after the first reset cycle completes,
 * so ROM content is guaranteed to be in guest memory.  Parse a firmware
 * plabel only when the firmware image is a valid IA-64 PE32+ binary.
 */
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
     * scratch; the firmware derives the same base from the memory it probes.
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
        /*
         * The PAL_PROC entry of this entry state: the microprogram battery
         * places its PAL stub at firmware_base + 0x60 (PAL_PROC_ENTRY).
         */
        .raw_pal_proc = firmware_base + 0x60,
        /*
         * What the project firmware registers with the PAL emulation once it
         * runs (IA64_PAL_FIRMWARE_REGISTER), for an image at firmware_base:
         * the tests entered here run without one.
         */
        .firmware = {
            .image_base = firmware_base,
            .image_size = IA64_FW_IDENTITY_WINDOW_SIZE,
            .ivt = firmware_base + IA64_FW_IVT_OFFSET,
            .sal_entry = firmware_base + IA64_FW_SAL_RUNTIME_ENTRY_OFF,
            .sal_return = firmware_base + IA64_FW_SAL_RUNTIME_RETURN_OFF,
            .sal_block = firmware_base + IA64_FW_SAL_DISPATCH_BLOCK_OFF,
            .assist_base = assist_base,
        },
        .powered_off = cpu_index != 0,
    };

    return info;
}

static void ia64_vpc_machine_done(Notifier *notifier, void *data)
{
    IA64VpcMachineState *s = container_of(notifier, IA64VpcMachineState,
                                          done_notifier);
    CPUState *cs;

    (void)data;
    ia64_vpc_configure_platform_pci(s);

    /*
     * With no firmware at all, every processor comes up as the flat image
     * used to be entered: at the image base with a stack, a backing store
     * and cr.iva inside the image window, the boot processor running and
     * the others powered off.  That is what the microprogram battery and
     * the qtests build on; a guest never sees it.
     */
    if (s->realfw_entry == 0) {
        uint64_t fw_base = ia64_vpc_fw_base(s, current_machine->ram_size);

        CPU_FOREACH(cs) {
            IA64BootInfo info = ia64_vpc_boot_info(MACHINE(s), fw_base,
                                                   cs->cpu_index, fw_base,
                                                   fw_base);

            ia64_cpu_set_boot_info(IA64_CPU(cs), &info);
            ia64_cpu_reset_to_boot_info(IA64_CPU(cs));
        }
        return;
    }

    /*
     * Every processor leaves reset at SALE_ENTRY with the PALE_RESET exit
     * state (SDM vol. 2 11.2.2), the machine playing PAL.
     */
    {
        uint64_t entry = s->realfw_entry;

        CPU_FOREACH(cs) {
            IA64BootInfo info = {
                .firmware_base = s->realfw_base,
                .firmware_entry = entry,
                .iva = IA64_PAL_RESET_IVT_BASE,
                .raw_entry = true,
                /*
                 * SAL calls PAL procedures through the machine-planted stub
                 * (GR34; GR36's authentication procedure lands on the same
                 * dispatcher and returns not-implemented for unknown
                 * indices).  SAL_B stashes this in bank-0 GR18 and uses it
                 * for every static PAL call.  The RECOVERY_CHECK call
                 * offers the same full set, not the reduced one SDM vol. 2
                 * 11.2.2 names (the SDV's SAL_A calls PAL_PLATFORM_ADDR
                 * there, which is in it).
                 */
                .raw_pal_proc = IA64_PAL_ROM_BASE,
                .raw_pal_auth = IA64_PAL_ROM_BASE,
                /*
                 * PAL_RESET's return address for the RECOVERY_CHECK call,
                 * on the boards that make it.  0 = this board calls
                 * SALE_ENTRY once, with function RESET: the vendor 460GX
                 * firmware's recovery-check pass initializes the DRAM,
                 * resets the platform itself and then spins in a software
                 * delay loop of its RAM-resident recovery module, so it
                 * never reaches its boot manager
                 * (plans/phase6-zx1-real-firmware-boot.md session 2).
                 */
                .raw_pal_reset_return =
                    IA64_VPC_MACHINE_GET_CLASS(s)->sale_recovery_check ?
                    IA64_PAL_RESET_RETURN : 0,
                /*
                 * Every processor leaves reset together and runs SAL_A,
                 * which arbitrates the BSP through the SAC's write-once
                 * word at FEB0_0CC0h; the losers wait at FEB0_0CB0h for
                 * the BSP's release, then park in SAL_B polling cr.irr for
                 * the OS's wake-up IPI (SAL 3.2.3 step 4, "wake APs ...
                 * return them to rendezvous").  Powering them off here
                 * would leave the vendor MADT with one processor.
                 */
                .powered_off = false,
            };

            ia64_cpu_set_boot_info(IA64_CPU(cs), &info);
            ia64_cpu_reset_to_boot_info(IA64_CPU(cs));
        }
    }
}

static bool ia64_vpc_validate_configuration(MachineState *machine,
                                            IA64VpcMachineState *s,
                                            Error **errp)
{
    IA64VpcMachineClass *imc = IA64_VPC_MACHINE_GET_CLASS(s);

    if (machine->ram_size < IA64_FW_LOW_RAM_MIN) {
        g_autofree char *size = size_to_str(IA64_FW_LOW_RAM_MIN);

        error_setg(errp, "Invalid RAM size, should be at least %s", size);
        return false;
    }
    if (s->alat_full && machine->smp.cpus > 1) {
        error_setg(errp, "full ALAT emulation is not SMP-safe");
        return false;
    }
    if (imc->validate != NULL && !imc->validate(s, errp)) {
        return false;
    }
    return true;
}


/*
 * The PAL emulation ROM's content.  At +0x00 the PAL procedure entry stub,
 * PAL_COPY_PAL's copy of it included (target/ia64/arch/pal.c pal_copy_pal):
 *   break.m 0x100000 ;;  br.many b0 ;;
 * The translator services the break through ia64_pal_dispatch() when the
 * bundle sits at a recognized PAL entry address (env->pal.pal_proc_reset_addr,
 * seeded from IA64BootInfo.raw_pal_proc on every reset, or the copy).
 * At +0x20 PAL_RESET's return address (IA64_PAL_RESET_RETURN):
 *   break.m 0x100007 ;;  br.few . ;;
 * The translator turns the break at that address into the second SALE_ENTRY
 * call (ia64_cpu_pal_reset_return); the branch to itself is never reached.
 */

static const uint8_t ia64_pal_stub[64] = {
    0x0a, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00,
    0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00,
    0x11, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x00, 0x02, 0x00, 0x00, 0x08, 0x00, 0x80, 0x00,
    0x0a, 0x38, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00,
    0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00,
    0x11, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40,
};

/*
 * The flash's persistence file (machine option nvram=): a raw, writable
 * block backend the flash device loads from and writes back to, so what the
 * firmware keeps in its flash -- its NVRAM sector, and any other block it
 * programs -- survives across runs.  The pflash device requires the file to
 * be exactly the flash size.
 *
 * -bios names the firmware.  At power-on the machine programs the image's
 * FIT-declared components (every FIT entry other than NVRAM blocks and
 * unused slots), the FIT itself and the reset pointer block into the flash,
 * as a firmware update tool would, and leaves every other block as the
 * file holds it.  A rebuilt image therefore runs at once, and the blocks a
 * firmware writes at run time persist: the vendor SDV firmware programs its
 * FIT-1Eh NVRAM block and five undeclared blocks, never a component.
 *
 * A missing or empty file is created from the image, and a file holding
 * just the 64 KiB variable store of the earlier NVRAM window is imported
 * into the NVRAM sector.  Every other file is refused and left unchanged,
 * as a flash update tool refuses to keep NVRAM it cannot keep (WFlash64:
 * "NVRAM not found or size mismatch.  Unable to preserve NVRAM"): one of
 * another size, a 64 KiB store the image has no NVRAM block for, and a
 * flash image whose FIT declares other NVRAM blocks than the image does --
 * programming this image's components into it would overwrite the data
 * another firmware keeps outside its NVRAM blocks (the vendor SDV
 * firmware's record store below 0xFFD00000).
 */
static void ia64_vpc_flash_refresh_components(uint8_t *contents,
                                              const uint8_t *image,
                                              uint64_t image_size,
                                              uint64_t fit_ptr)
{
    uint64_t base = IA64_REALFW_WINDOW_END - image_size;
    const uint8_t *fit = image + (fit_ptr - base);
    uint32_t entries = ldl_le_p(fit + 8) & 0xffffff;
    g_autofree uint8_t *kept = g_memdup2(contents, image_size);
    int pass;
    uint32_t i;

    entries = MIN(entries, (IA64_REALFW_WINDOW_END - fit_ptr) / 16);
    /*
     * Components first, then the NVRAM blocks back from the file: a
     * component's declared size may run into an NVRAM block (the SDV's
     * SAL_B entry ends 0x2E0 bytes inside its 1Eh block), and what the
     * firmware keeps there wins.
     */
    for (pass = 0; pass < 2; pass++) {
        for (i = 1; i < entries; i++) {
            const uint8_t *e = fit + i * 16;
            uint64_t addr = ldq_le_p(e) & IA64_REALFW_PTR_ADDR_MASK;
            uint64_t size = (uint64_t)(ldl_le_p(e + 8) & 0xffffff) * 16;
            uint8_t type = e[14] & 0x7f;

            if (type == IA64_FIT_TYPE_UNUSED || addr < base || size == 0 ||
                size > IA64_REALFW_WINDOW_END - addr) {
                continue;
            }
            if (pass == 0 && type != IA64_FIT_TYPE_NVRAM) {
                memcpy(contents + (addr - base), image + (addr - base), size);
            } else if (pass == 1 && type == IA64_FIT_TYPE_NVRAM) {
                memcpy(contents + (addr - base), kept + (addr - base), size);
            }
        }
        if (pass == 0) {
            memcpy(contents + (fit_ptr - base), fit, (uint64_t)entries * 16);
            memcpy(contents + image_size - 48, image + image_size - 48, 48);
        }
    }
}

/* The NVRAM (type 1Eh) blocks a flash image's FIT declares. */
#define IA64_FLASH_NVRAM_BLOCKS_MAX 16

typedef struct IA64FlashNvramLayout {
    unsigned count;
    uint64_t addr[IA64_FLASH_NVRAM_BLOCKS_MAX];
    uint64_t size[IA64_FLASH_NVRAM_BLOCKS_MAX];
} IA64FlashNvramLayout;

/*
 * Read the NVRAM blocks from an image ending at 4 GiB.  False when it has no
 * reset pointer block naming a _FIT_ table inside it (the check
 * ia64_vpc_read_firmware makes of -bios), or more NVRAM blocks than fit.
 */
static bool ia64_vpc_flash_nvram_layout(const uint8_t *image,
                                        uint64_t image_size,
                                        IA64FlashNvramLayout *layout)
{
    uint64_t base = IA64_REALFW_WINDOW_END - image_size;
    uint64_t fit_ptr;
    const uint8_t *fit;
    uint32_t entries;
    uint32_t i;

    layout->count = 0;
    fit_ptr = ldq_le_p(image + (IA64_REALFW_PTR_FIT - base)) &
              IA64_REALFW_PTR_ADDR_MASK;
    if (fit_ptr < base || fit_ptr + 16 > IA64_REALFW_WINDOW_END) {
        return false;
    }
    fit = image + (fit_ptr - base);
    if (memcmp(fit, "_FIT_   ", 8) != 0) {
        return false;
    }
    entries = ldl_le_p(fit + 8) & 0xffffff;
    entries = MIN(entries, (IA64_REALFW_WINDOW_END - fit_ptr) / 16);
    for (i = 1; i < entries; i++) {
        const uint8_t *e = fit + i * 16;

        if ((e[14] & 0x7f) != IA64_FIT_TYPE_NVRAM) {
            continue;
        }
        if (layout->count == IA64_FLASH_NVRAM_BLOCKS_MAX) {
            return false;
        }
        layout->addr[layout->count] = ldq_le_p(e) & IA64_REALFW_PTR_ADDR_MASK;
        layout->size[layout->count] = (uint64_t)(ldl_le_p(e + 8) & 0xffffff) *
                                      16;
        layout->count++;
    }
    return true;
}

static bool ia64_vpc_flash_nvram_layout_has(const IA64FlashNvramLayout *l,
                                            uint64_t addr, uint64_t size)
{
    unsigned i;

    for (i = 0; i < l->count; i++) {
        if (l->addr[i] == addr && l->size[i] == size) {
            return true;
        }
    }
    return false;
}

static bool ia64_vpc_flash_nvram_layout_equal(const IA64FlashNvramLayout *a,
                                              const IA64FlashNvramLayout *b)
{
    unsigned i;

    if (a->count != b->count) {
        return false;
    }
    for (i = 0; i < a->count; i++) {
        if (!ia64_vpc_flash_nvram_layout_has(b, a->addr[i], a->size[i])) {
            return false;
        }
    }
    return true;
}

/* "0xfff90000+0x20000, ..." for an error message, "none" when empty. */
static char *ia64_vpc_flash_nvram_layout_str(const IA64FlashNvramLayout *l)
{
    GString *str = g_string_new(NULL);
    unsigned i;

    for (i = 0; i < l->count; i++) {
        g_string_append_printf(str, "%s0x%" PRIx64 "+0x%" PRIx64,
                               i ? ", " : "", l->addr[i], l->size[i]);
    }
    if (l->count == 0) {
        g_string_append(str, "none");
    }
    return g_string_free(str, false);
}

static bool ia64_vpc_buffer_is_filled(const uint8_t *buf, size_t len,
                                      uint8_t value)
{
    return len == 0 ||
           (buf[0] == value && memcmp(buf, buf + 1, len - 1) == 0);
}

static BlockBackend *ia64_vpc_open_flash_backing(IA64VpcMachineState *s,
                                                 const char *path,
                                                 Error **errp)
{
    const uint8_t *image = s->fw_image;
    uint64_t image_size = s->fw_image_size;
    uint64_t sector = IA64_NVRAM_BASE - (IA64_REALFW_WINDOW_END - image_size);
    IA64FlashNvramLayout image_layout;
    g_autofree uint8_t *contents = NULL;
    g_autofree char *existing = NULL;
    gsize existing_size = 0;
    GError *gerr = NULL;
    QDict *options;
    BlockBackend *blk;

    /*
     * -bios passed ia64_vpc_read_firmware, so its FIT is valid; only a
     * table with too many NVRAM blocks is left to refuse.
     */
    if (!ia64_vpc_flash_nvram_layout(image, image_size, &image_layout)) {
        error_setg(errp, "firmware '%s' declares more than %d NVRAM blocks; "
                   "nvram '%s' was not opened", s->fw_image_name,
                   IA64_FLASH_NVRAM_BLOCKS_MAX, path);
        return NULL;
    }

    if (g_file_test(path, G_FILE_TEST_EXISTS) &&
        !g_file_get_contents(path, &existing, &existing_size, &gerr)) {
        error_setg(errp, "nvram '%s': cannot read: %s", path, gerr->message);
        g_error_free(gerr);
        return NULL;
    }
    if (existing_size == image_size) {
        IA64FlashNvramLayout file_layout;
        const uint8_t *file = (const uint8_t *)existing;

        if (ia64_vpc_flash_nvram_layout(file, image_size, &file_layout)) {
            if (!ia64_vpc_flash_nvram_layout_equal(&file_layout,
                                                   &image_layout)) {
                g_autofree char *file_str =
                    ia64_vpc_flash_nvram_layout_str(&file_layout);
                g_autofree char *image_str =
                    ia64_vpc_flash_nvram_layout_str(&image_layout);

                error_setg(errp, "nvram '%s' was written by a firmware with "
                           "other NVRAM blocks (file: %s; firmware '%s': %s); "
                           "the file was not changed, use a separate nvram "
                           "file for each firmware", path, file_str,
                           s->fw_image_name, image_str);
                return NULL;
            }
            contents = (uint8_t *)g_steal_pointer(&existing);
            ia64_vpc_flash_refresh_components(contents, image, image_size,
                                              s->fw_fit_ptr);
        } else if (ia64_vpc_buffer_is_filled(file, existing_size, 0x00) ||
                   ia64_vpc_buffer_is_filled(file, existing_size, 0xff)) {
            contents = g_memdup2(image, image_size);
        } else {
            error_setg(errp, "nvram '%s' is not a flash image: it has no "
                       "firmware interface table and is not blank; the file "
                       "was not changed", path);
            return NULL;
        }
    } else if (existing_size == IA64_NVRAM_SIZE) {
        if (sector + IA64_NVRAM_SIZE > image_size ||
            !ia64_vpc_flash_nvram_layout_has(&image_layout, IA64_NVRAM_BASE,
                                             IA64_NVRAM_SIZE)) {
            error_setg(errp, "nvram '%s' is a 64 KiB variable store, but "
                       "firmware '%s' has no 64 KiB NVRAM block at 0x%" PRIx64
                       " to import it into; the file was not changed", path,
                       s->fw_image_name, (uint64_t)IA64_NVRAM_BASE);
            return NULL;
        }
        contents = g_memdup2(image, image_size);
        memcpy(contents + sector, existing, IA64_NVRAM_SIZE);
    } else if (existing_size == 0) {
        contents = g_memdup2(image, image_size);
    } else {
        error_setg(errp, "nvram '%s' is %" G_GSIZE_FORMAT " bytes, but "
                   "firmware '%s' is a flash image of %" PRIu64 " bytes; the "
                   "file was not changed, use a separate nvram file for each "
                   "firmware", path, existing_size, s->fw_image_name,
                   image_size);
        return NULL;
    }
    if (!g_file_set_contents(path, (const gchar *)contents, image_size,
                             &gerr)) {
        error_setg(errp, "nvram '%s': cannot write: %s", path,
                   gerr->message);
        g_error_free(gerr);
        return NULL;
    }

    options = qdict_new();
    qdict_put_str(options, "driver", "raw");
    blk = blk_new_open(path, NULL, options, BDRV_O_RDWR, errp);
    if (blk == NULL) {
        error_prepend(errp, "nvram '%s': ", path);
    }
    return blk;
}

/*
 * Read the firmware file: -bios, else the shipped default beside the
 * binary.  Not finding the default is not an error:
 * qtest brings this machine up with no firmware at all.  The file is
 * classified here, before the platform is built, because what it is
 * decides what the platform provides (a flash image carrying its own
 * NVRAM block needs no synthetic variable store).
 */
static bool ia64_vpc_read_firmware(IA64VpcMachineState *s,
                                   MachineState *machine, Error **errp)
{
    g_autofree char *path = NULL;
    const char *name;
    GError *gerr = NULL;
    gsize size = 0;

    if (machine->firmware != NULL) {
        name = machine->firmware;
        path = qemu_find_file(QEMU_FILE_TYPE_BIOS, name);
        if (path == NULL) {
            path = g_strdup(name);
        }
    } else {
        name = IA64_VPC_DEFAULT_FIRMWARE;
        path = qemu_find_file(QEMU_FILE_TYPE_BIOS, name);
        if (path == NULL) {
            return true;
        }
    }
    if (!g_file_get_contents(path, (gchar **)&s->fw_image, &size, &gerr)) {
        error_setg(errp, "failed to read firmware '%s': %s", name,
                   gerr->message);
        g_error_free(gerr);
        return false;
    }
    s->fw_image_size = size;
    s->fw_image_name = g_strdup(name);

    /*
     * A flash image is a whole number of 64 KiB blocks that ends at 4 GiB
     * with the architected reset pointer block in its last 48 bytes (SAL
     * sec 2.5): the FIT pointer at 4 GiB-32 and the SALE_ENTRY pointer at
     * 4 GiB-24 both point into it, and the FIT carries its signature.
     * Nothing else is a firmware image for this machine.
     */
    if (size != 0 && size <= IA64_REALFW_MAX_SIZE && (size & 0xffff) == 0) {
        uint64_t base = IA64_REALFW_WINDOW_END - size;
        uint64_t fit_ptr = ldq_le_p(s->fw_image + (IA64_REALFW_PTR_FIT - base))
                           & IA64_REALFW_PTR_ADDR_MASK;
        uint64_t sale_ptr = ldq_le_p(s->fw_image +
                                     (IA64_REALFW_PTR_SALE - base))
                            & IA64_REALFW_PTR_ADDR_MASK;

        if (fit_ptr >= base && fit_ptr + 16 <= IA64_REALFW_WINDOW_END &&
            sale_ptr >= base && sale_ptr < IA64_REALFW_WINDOW_END &&
            memcmp(s->fw_image + (fit_ptr - base), "_FIT_   ", 8) == 0) {
            s->fw_is_flash = true;
            s->fw_fit_ptr = fit_ptr;
            s->fw_sale_ptr = sale_ptr;
        }
    }
    if (!s->fw_is_flash) {
        error_setg(errp, "firmware '%s' is not a flash image: a whole "
                   "number of 64 KiB blocks, at most 8 MiB, with a reset "
                   "pointer block and a _FIT_ table", name);
        return false;
    }
    return true;
}

/*
 * The zx1 board keeps its settings in the PDH battery-backed SRAM and in its
 * BMC, so its `nvram=` file is a raw image of that part followed by the BMC's
 * tokens.  A file of just the part was kept before the BMC was, and gains the
 * area of a new BMC.  A file of another size belongs to another part -- a
 * flash image, or the 64 KiB store this firmware used to keep in one -- and
 * scripts/ia64-nvram.py converts it.  Only the size is checked: a store of the
 * right size that this firmware cannot read is the firmware's own business,
 * which asks before it resets one.
 */
BlockBackend *ia64_vpc_open_pdh_store(const char *path, Error **errp)
{
    const uint64_t size = LONGSPEAK_PDH_STORE_SIZE;
    g_autofree char *existing = NULL;
    gsize existing_size = 0;
    GError *gerr = NULL;
    QDict *options;
    BlockBackend *blk;

    if (g_file_test(path, G_FILE_TEST_EXISTS) &&
        !g_file_get_contents(path, &existing, &existing_size, &gerr)) {
        error_setg(errp, "nvram '%s': cannot read: %s", path, gerr->message);
        g_error_free(gerr);
        return NULL;
    }
    if (existing_size == 0) {
        g_autofree char *blank = g_malloc0(size);

        /* A new file is a new battery: the firmware formats it itself. */
        if (!g_file_set_contents(path, blank, size, &gerr)) {
            error_setg(errp, "nvram '%s': cannot write: %s", path,
                       gerr->message);
            g_error_free(gerr);
            return NULL;
        }
    } else if (existing_size == IA64_PDH_BBSRAM_SIZE) {
        g_autofree char *blank = g_malloc0(LONGSPEAK_PDH_STORE_BMC);
        int fd = qemu_open(path, O_WRONLY | O_APPEND, errp);

        if (fd < 0) {
            return NULL;
        }
        if (qemu_write_full(fd, blank, LONGSPEAK_PDH_STORE_BMC) !=
            LONGSPEAK_PDH_STORE_BMC) {
            error_setg_errno(errp, errno, "nvram '%s': cannot write", path);
            qemu_close(fd);
            return NULL;
        }
        qemu_close(fd);
    } else if (existing_size != size) {
        error_setg(errp, "nvram '%s' is %" G_GSIZE_FORMAT " bytes, but this "
                   "board keeps its settings in a %" PRIu64 "-byte file, the "
                   "PDH store and the BMC's tokens; the file was not changed, "
                   "convert it with scripts/ia64-nvram.py", path,
                   existing_size, size);
        return NULL;
    }

    options = qdict_new();
    qdict_put_str(options, "driver", "raw");
    blk = blk_new_open(path, NULL, options, BDRV_O_RDWR, errp);
    if (blk == NULL) {
        error_prepend(errp, "nvram '%s': ", path);
    }
    return blk;
}

/*
 * Map a flash image so that its end lands exactly at 4 GiB, and take the
 * boot entry from the architected SALE_ENTRY pointer at 4 GiB-24.  The
 * flash window lies inside the ia64-firmware-address-space RAM region.
 * The vendor SDV image and the project firmware's flash image both come
 * through here.
 */
static bool ia64_vpc_load_flash(IA64VpcMachineState *s, Error **errp)
{
    IA64VpcMachineClass *imc = IA64_VPC_MACHINE_GET_CLASS(s);
    uint64_t part = imc->flash_part_size != 0 ? imc->flash_part_size
                                              : s->fw_image_size;
    uint64_t base = IA64_REALFW_WINDOW_END - part;

    /*
     * The flash is a real Intel-CFI (command-set 0x0001) part: SDV firmware
     * probes it during QuickBoot (write 0x50 Clear-Status, 0x70 Read-Status,
     * read the WSM-ready bit 0x80, 0xff Read-Array) and uses it as writable
     * non-volatile storage for EFI settings / boot config in the FIT-0x1E
     * NVRAM sector.  Model it with pflash_cfi01 (the Intel CFI flash device)
     * initialized from the vendor image, overlaying the firmware address
     * space (priority above the identity RAM region) so its command interface
     * shadows plain RAM at the flash window.  The board says which part it
     * carries; see IA64VpcMachineClass::flash_part_size.
     */
    {
        DeviceState *dev = qdev_new(TYPE_PFLASH_CFI01);
        MemoryRegion *flash_mr;
        BlockBackend *flash_blk = NULL;

        if (s->nvram_path != NULL &&
            !IA64_VPC_MACHINE_GET_CLASS(s)->nvram_is_pdh_store) {
            flash_blk = ia64_vpc_open_flash_backing(s, s->nvram_path, errp);
            if (flash_blk == NULL) {
                return false;
            }
            qdev_prop_set_drive(dev, "drive", flash_blk);
        }

        qdev_prop_set_uint32(dev, "num-blocks", part / imc->flash_sector_len);
        qdev_prop_set_uint64(dev, "sector-length", imc->flash_sector_len);
        /*
         * Byte-wide commands.  The J3 is an x8/x16 part and how Dillon wires
         * it is not known; the vendor firmware issues no flash command over a
         * POST to the EFI shell, so keep the addressing the SDV needs.
         */
        qdev_prop_set_uint8(dev, "width", 1);
        qdev_prop_set_bit(dev, "big-endian", 0);
        /*
         * JEDEC identity the firmware checks.  It byte-reads read-ID offset
         * 0 for the manufacturer and offset 1 for the device, then combines
         * them.  pflash returns id0<<8|id1 at word offset 0 and id2<<8|id3
         * at word offset 1, so a byte read of offset 0 yields id1 (hold the
         * manufacturer there) and a byte read of offset 1 yields id3 (hold
         * the device there).  id0 also carries the device so that a 16-bit
         * read of offset 0 reads device<<8|manufacturer too.
         */
        qdev_prop_set_uint16(dev, "id0", imc->flash_device_id);
        qdev_prop_set_uint16(dev, "id1", imc->flash_manufacturer_id);
        qdev_prop_set_uint16(dev, "id2", 0x0000);
        qdev_prop_set_uint16(dev, "id3", imc->flash_device_id);
        qdev_prop_set_bit(dev, "block-locking", imc->flash_block_locking);
        /*
         * Firmware programs the NVRAM sector a byte at a time; batch the
         * backing file's writes rather than paying one host write per byte.
         */
        qdev_prop_set_uint32(dev, "x-flush-delay-ms", 50);
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
            uint8_t *ram = memory_region_get_ram_ptr(flash_mr);

            /* An erased block reads 0xFF; the image takes the top of it. */
            memset(ram, 0xff, part - s->fw_image_size);
            memcpy(ram + (part - s->fw_image_size), s->fw_image,
                   s->fw_image_size);
        }
        ia64_vpc_seed_nvram_defaults(s, memory_region_get_ram_ptr(flash_mr),
                                     base);
    }

    rom_add_blob_fixed("ia64-pal-stub", ia64_pal_stub,
                       sizeof(ia64_pal_stub),
                       IA64_PAL_ROM_BASE);

    {
        /* Branch-to-self bundle (MIB: nop.m; nop.i; br.few 0). */
        static const uint8_t self_branch[16] = {
            0x11, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
            0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40,
        };
        g_autofree uint8_t *ivt = g_malloc(IA64_PAL_RESET_IVT_SIZE);
        size_t off;

        for (off = 0; off < IA64_PAL_RESET_IVT_SIZE; off += sizeof(self_branch)) {
            memcpy(ivt + off, self_branch, sizeof(self_branch));
        }
        rom_add_blob_fixed("ia64-pal-reset-ivt", ivt, IA64_PAL_RESET_IVT_SIZE,
                           IA64_PAL_RESET_IVT_BASE);
    }

    s->realfw_base = base;
    s->realfw_entry = s->fw_sale_ptr;
    return true;
}


static bool ia64_vpc_load_firmware(IA64VpcMachineState *s, Error **errp)
{
    if (s->fw_image == NULL) {
        return true;
    }
    return ia64_vpc_load_flash(s, errp);
}

static bool ia64_vpc_build(MachineState *machine, Error **errp)
{
    IA64VpcMachineState *s = IA64_VPC_MACHINE(machine);
    IA64VpcMachineClass *imc = IA64_VPC_MACHINE_GET_CLASS(s);
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
    if (!ia64_vpc_read_firmware(s, machine, errp)) {
        return false;
    }

    for (i = 0; i < machine->smp.cpus; i++) {
        uint32_t threads = MAX(machine->smp.threads, 1U);
        uint32_t cores = MAX(machine->smp.cores, 1U);
        uint32_t per_socket = threads * cores;
        uint32_t package_base = (i / per_socket) * per_socket;
        cpu = IA64_CPU(object_new(machine->cpu_type));
        cpu->alat_full = s->alat_full;
        cpu->socket_id = i / per_socket;
        cpu->core_id = (i / threads) % cores;
        cpu->thread_id = i % threads;
        cpu->cores_per_socket = cores;
        cpu->threads_per_core = threads;
        cpu->package_base = package_base;
        cpu->package_cpus = MIN(per_socket,
                                machine->smp.cpus - package_base);
        /*
         * GR33 at SALE_ENTRY and PAL_FIXED_ADDR; both firmwares make it the
         * processor's LID.  Boards without a table use the CPU index.  A
         * geographic id the user set by hand (-global ia64-cpu.geographic-id,
         * applied when the object is created) stays: it is a debugging knob,
         * and every board has a table now.
         */
        if (imc->processor_ids != NULL &&
            object_property_get_uint(OBJECT(cpu), "geographic-id",
                                     &error_abort) == UINT32_MAX) {
            qdev_prop_set_uint32(DEVICE(cpu), "geographic-id",
                                 imc->processor_ids[MIN(i,
                                                        imc->nprocessor_ids - 1)]);
        }
        if (!qdev_realize_and_unref(DEVICE(cpu), NULL, errp)) {
            return false;
        }
    }
    ia64_vpc_map_lsapic(s);

    iosapic = qdev_new(TYPE_IA64_IOSAPIC);
    if (imc->iosapic_pins != 0) {
        qdev_prop_set_uint32(iosapic, "num-pins", imc->iosapic_pins);
        qdev_prop_set_uint32(iosapic, "version", imc->iosapic_version);
    }
    if (!sysbus_realize_and_unref(SYS_BUS_DEVICE(iosapic), errp)) {
        return false;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(iosapic), 0, IA64_IOSAPIC_BASE);

    /*
     * The zx1 machine's UARTs are memory-mapped stand-ins until the zx1
     * firmware work shows the real ones.  A board with legacy COM ports
     * gets them in PCI I/O space below, once that space exists.
     */
    if (!imc->legacy_com1_console) {
        s->console_uart = serial_mm_init(get_system_memory(), IA64_UART_BASE,
                                         0, qdev_get_gpio_in(iosapic, 4),
                                         115200, serial_hd(0),
                                         DEVICE_LITTLE_ENDIAN);
        if (debug_port_get_chardev()) {
            s->debug_uart = serial_mm_init(get_system_memory(),
                                           IA64_DEBUG_UART_BASE, 0,
                                           qdev_get_gpio_in(iosapic, 3),
                                           115200, debug_port_get_chardev(),
                                           DEVICE_LITTLE_ENDIAN);
        }
    }

    if (!ia64_vpc_load_firmware(s, errp)) {
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
    s->pci_host_dev = pci_host;
    qdev_prop_set_bit(pci_host, "ecam", imc->pci_config_ecam);
    if (imc->pci0_intx != NULL) {
        ia64_pci_host_set_intx_routes(pci_host, imc->pci0_intx,
                                      imc->pci0_nintx,
                                      imc->pci0_intx_fallback);
    }
    if (!sysbus_realize_and_unref(SYS_BUS_DEVICE(pci_host), errp)) {
        return false;
    }
    pci_bus = PCI_BUS(qdev_get_child_bus(pci_host, "pci"));

    /*
     * The board's core chipset: its DMA-translation device is created before
     * any other PCI device so its pci_setup_iommu() installs the bus-master
     * DMA routing before any master's address space is resolved, and its
     * further PCI roots come with it.
     */
    if (!imc->build_chipset(s, pci_host, pci_bus, pci_bus->address_space_io,
                            iosapic, errp)) {
        return false;
    }
    /*
     * Slot 0 is intentionally empty in the default machine.  Reserve it while
     * creating the built-in devices so their historical slot numbers remain
     * stable, then release it for an explicitly requested PCI controller.
     */
    pci_bus_set_slot_reserved_mask(pci_bus, 1U << 0);
    /*
     * A seat the board names on this bus must still be free when its device
     * arrives, so keep it out of automatic placement until then.
     */
    {
        PCIBus *seat_bus = pci_bus;
        int seat_devfn = -1;

        imc->seat(s, IA64_VPC_SEAT_SCSI, &seat_bus, &seat_devfn);
        if (seat_devfn >= 0 && seat_bus == pci_bus) {
            pci_bus_set_slot_reserved_mask(pci_bus,
                                           1U << PCI_SLOT(seat_devfn));
        }
    }
    pci_io = pci_bus->address_space_io;
    /*
     * A board with a south bridge carries its ACPI block there (the 460GX's
     * 82468GX IFB at A00h); the others get the stand-in block at 2000h until
     * their firmware work shows the real one.
     */
    if (!imc->has_south_bridge) {
        ia64_vpc_init_acpi_pm(s, iosapic, pci_io);
    }
    s->host_pci_bus = pci_bus;

    /*
     * The i2000's COM ports: the Super I/O's UART1 at 3F8h on IRQ 4 is the
     * console, which is what the vendor DSDT reports for it (UAR1, LDN 4)
     * and what its firmware talks to; a debug port, when configured, is
     * UART2 at 2F8h on IRQ 3.  Early IA-64 kernel debuggers predate the
     * ACPI DBGP table and drive these fixed ports directly (Windows
     * Whistler build 2462's kdcom.dll hardcodes 0x3f8/0x2f8/0x3e8/0x2e8
     * through HAL's READ_PORT_UCHAR/WRITE_PORT_UCHAR), so /debugport=com2
     * reaches the debug chardev and com1 the console.  ISA IRQs 0..15 are
     * the platform interrupt controller's first inputs.
     */
    if (imc->legacy_com1_console) {
        s->console_uart = serial_mm_init(pci_io, IA64_460GX_COM1_IO_BASE, 0,
                                         qdev_get_gpio_in(iosapic,
                                                          IA64_460GX_COM1_IRQ),
                                         115200, serial_hd(0),
                                         DEVICE_LITTLE_ENDIAN);
        if (debug_port_get_chardev()) {
            s->debug_uart = serial_mm_init(pci_io, IA64_460GX_COM2_IO_BASE,
                                           0,
                                           qdev_get_gpio_in(
                                               iosapic, IA64_460GX_COM2_IRQ),
                                           115200, debug_port_get_chardev(),
                                           DEVICE_LITTLE_ENDIAN);
        }
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
    imc->wire_intx(s, pci_host, iosapic);

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
        s->ahci_dev = pci_create_simple(pci_bus,
                                        PCI_DEVFN(imc->ahci_slot, 0),
                                        TYPE_ICH9_AHCI);
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
        pci_bus_set_slot_reserved_mask(pci_bus, 1U << imc->ahci_slot);
    }
#endif

    /*
     * The board's south bridge and ISA bus, with the RTC and the legacy
     * interrupt inputs (which drive the matching IOSAPIC inputs).
     */
    isa_bus = imc->build_isa(s, pci_bus, pci_io, iosapic, errp);
    if (isa_bus == NULL) {
        return false;
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
        int scsi_devfn = -1;

        ia64_vpc_seat(s, IA64_VPC_SEAT_SCSI, &scsi_bus, &scsi_devfn);
        if (ia64_vpc_lsi_at_seat(s)) {
            if (!ia64_vpc_init_lsi(s, scsi_bus, scsi_devfn, errp)) {
                return false;
            }
        } else {
            ia64_vpc_init_isp(s, scsi_bus, scsi_devfn);
        }
    }
#endif

#ifdef CONFIG_IA64_VPC_GRAPHICS
    /*
     * The graphics adapter sits where the board puts its AGP master: behind
     * the Mercury bridge on zx1, behind the GXB expander on 460gx.
     */
    {
    PCIBus *vga_bus = pci_bus;
    int vga_devfn = -1;
    unsigned int vga_slot;
    const char *vga_model = ia64_vpc_vga_model(s);

    ia64_vpc_seat(s, IA64_VPC_SEAT_VGA, &vga_bus, &vga_devfn);
    vga_slot = PCI_SLOT(vga_devfn);

    /*
     * vga= names the device, so the board creates it at its own seat and
     * reports the display itself, or -vga says none was made
     * (system/vl.c:2895).  Only the -vga case goes through pci_vga_init(),
     * which knows the types this option does not name.
     *
     * The Mach64 3D Rage (DEV_4752/4754) is a PCI 2D adapter with no AGP.
     * The NVIDIA Quadro2 Pro (NV15GL, 10de:0153) is an AGP graphics master
     * with a 16 MB MMIO BAR0 and a 128 MB prefetchable framebuffer BAR1,
     * which ia64_vpc_configure_vga() maps in the NVIDIA layout.
     */
    if (vga_model == NULL) {
        s->vga_dev = pci_vga_init(vga_bus);
    } else if (g_strcmp0(vga_model, "none") == 0) {
        s->vga_dev = NULL;
    } else {
        const char *type = "ati-vga";

        if (g_strcmp0(vga_model, "mach64") == 0) {
            type = "mach64-vga";
        } else if (g_strcmp0(vga_model, "nv15gl") == 0) {
            type = "nv15gl-vga";
        } else if (g_strcmp0(vga_model, "std") == 0) {
            type = "VGA";
        }
        s->vga_dev = pci_new(PCI_DEVFN(vga_slot, 0), type);
        if (!pci_realize_and_unref(s->vga_dev, vga_bus, errp)) {
            return false;
        }
        vga_interface_created = true;
    }
    /*
     * The GART scoping above assumes the graphics device is the AGP master at
     * the expected slot.  pci_vga_init() auto-assigns the lowest free slot,
     * which is IA64_MERCURY_VGA_SLOT on the (empty) Mercury bus and
     * the board's VGA seat; fail loudly if that ever drifts.
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
    ia64_vpc_configure_vga(s->vga_dev,
                           IA64_VGA_IO_BASE);
    ia64_vpc_map_vga_fixed_windows(s, s->vga_dev);
#ifdef CONFIG_IA64_VPC_GRAPHICS
    if (s->vga_dev != NULL) {
        ia64_vpc_init_int10(s, pci_io);
    }
#endif
    /*
     * Reserve the board's audio seat before any auto-placed adapter is
     * created, so that the slot map does not depend on whether the CS4281
     * is switched on: an add-in card must not land there.  The reservation
     * is dropped again below when the CS4281 is created.
     */
    {
        PCIBus *audio_bus = pci_bus;
        int audio_devfn = -1;

        ia64_vpc_seat(s, IA64_VPC_SEAT_AUDIO, &audio_bus, &audio_devfn);
        if (audio_devfn >= 0) {
            pci_bus_set_slot_reserved_mask(audio_bus,
                                           1U << PCI_SLOT(audio_devfn));
        }
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

        ia64_vpc_seat(s, IA64_VPC_SEAT_SCSI_PARK, &park_bus, &park_devfn);
        if (ia64_vpc_lsi_at_seat(s)) {
            ia64_vpc_init_isp(s, park_bus, park_devfn);
        } else if (!ia64_vpc_init_lsi(s, park_bus, park_devfn, errp)) {
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
        PCIBus *audio_bus = pci_bus;
        int audio_devfn = -1;

        ia64_vpc_seat(s, IA64_VPC_SEAT_AUDIO, &audio_bus, &audio_devfn);
        if (audio_devfn >= 0) {
            pci_bus_clear_slot_reserved_mask(audio_bus,
                                             1U << PCI_SLOT(audio_devfn));
        }
        s->audio_dev = pci_create_simple(audio_bus, audio_devfn, TYPE_CS4281);
        ia64_vpc_configure_audio(s->audio_dev);
    }
#endif
    pci_bus_clear_slot_reserved_mask(pci_bus,
                                     (1U << 0) | (1U << imc->ahci_slot));

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
     * On zx1, which has no such bridge, ide=on populates the reserved slot 0
     * with a dual-channel CMD646.  Slot 0 is the platform-anticipated home
     * for IDE there: the firmware's fixed PCI-I/O table and the DSDT _PRT
     * both describe an IDE function at that address, and it keeps every
     * other device's BDF stable.  The firmware assigns its I/O BARs on
     * demand, exactly as for a hand-attached -device cmd646-ide.
     */
    if (s->ifb != NULL) {
        s->ide_dev = intel_82468gx_ifb_function(s->ifb,
                                                IA64_460GX_IFB_IDE_FUNCTION);
        ia64_vpc_configure_ifb_ide(s->ide_dev);
        pci_ide_create_devs(s->ide_dev);
    } else if (s->ide_enabled) {
        s->ide_dev = pci_new(PCI_DEVFN(0, 0), "cmd646-ide");
        qdev_prop_set_uint32(DEVICE(s->ide_dev), "secondary", 1);
        if (!pci_realize_and_unref(s->ide_dev, pci_bus, errp)) {
            return false;
        }
        ia64_vpc_configure_pci_irq(s, s->ide_dev);
        pci_ide_create_devs(s->ide_dev);
    }
#endif

    /* The south bridge's ACPI block, where there is one, takes the button. */
    if (!imc->has_south_bridge) {
        s->powerdown_notifier.notify = ia64_vpc_powerdown_req;
        qemu_register_powerdown_notifier(&s->powerdown_notifier);
    }

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
    s->fw_relocate = true;

    s->i8042_enabled = IA64_VPC_MACHINE_GET_CLASS(s)->i8042_default;
#ifdef CONFIG_IA64_VPC_STORAGE
    /*
     * Default the SATA controller off: Windows XP/2003 IA-64 ship no inbox
     * AHCI driver and otherwise see an unidentified PCI device, so the guest
     * that most wants storage is better served booting off the SCSI HBA.
     * Re-enable with ahci=on for SATA-aware guests.  IDE (cmd646) is likewise
     * opt-in via ide=on.
     *
     * The SCSI HBA is the one the board carries: the QLogic ISP12160 on the
     * i2000, the LSI on rx2600/zx2000.  The other is opt-in (isp=on / lsi=on)
     * for images installed against it, and then parks off the seat.
     */
    s->ahci_enabled = false;
    s->audio_enabled = false;
    s->isp_enabled = !IA64_VPC_MACHINE_GET_CLASS(s)->lsi_default;
    s->lsi_enabled = IA64_VPC_MACHINE_GET_CLASS(s)->lsi_default;
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
    /* The board's own display adapter; ia64_vpc_vga_model() lets -vga in. */
    s->vga_model = g_strdup(IA64_VPC_MACHINE_GET_CLASS(s)->vga_default ?
                            IA64_VPC_MACHINE_GET_CLASS(s)->vga_default :
                            "rage128");
}

static void ia64_vpc_machine_instance_finalize(Object *obj)
{
    IA64VpcMachineState *s = IA64_VPC_MACHINE(obj);

    if (s->vmstate_registered) {
        vmstate_unregister(NULL, &vmstate_ia64_vpc, s);
    }
    g_free(s->nvram_path);
    g_free(s->vga_model);
    g_free(s->fw_image);
    g_free(s->fw_image_name);
}

/*
 * Shared class-init for the abstract "ia64-base": everything common to both
 * concrete machines.  The concrete 460gx/zx1 class-inits (below) run after this
 * and set the fields that differ -- desc, default CPU, and the board hooks.
 */
static void ia64_vpc_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    IA64VpcMachineClass *imc = IA64_VPC_MACHINE_CLASS(oc);

    (void)data;

    imc->ahci_slot = 1;
    /*
     * Intel 82802AC Firmware Hub, 8 Mbit, 64 KiB blocks, which locks every
     * block out of reset through its register interface (datasheet 290658).
     * The SDV board carries four of them; a board with another part says so.
     */
    imc->flash_part_size = 0;
    imc->flash_sector_len = 64 * KiB;
    imc->flash_manufacturer_id = 0x0089;
    imc->flash_device_id = 0x00ac;
    imc->flash_block_locking = true;
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
        "Where the CPU expects the firmware image: at the top of low RAM "
        "(default on, where the shipped firmware shadows itself) or at the "
        "historical 1 MB home (off; the microprogram battery loads code "
        "there with no firmware present)");
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
        "Display adapter: 'rage128' (ATI Rage 128, honours -vga), 'mach64' "
        "(ATI Mach64 3D Rage, a PCI 2D adapter with no AGP), 'nv15gl' "
        "(NVIDIA Quadro2 Pro), 'std' or 'none'. Each board defaults to its "
        "own adapter");
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
    object_class_property_add_str(oc, "nvram",
                                  ia64_vpc_get_nvram,
                                  ia64_vpc_set_nvram);
    object_class_property_set_description(oc, "nvram",
        "Path to a writable file that persists the firmware flash across "
        "runs (its NVRAM sector is the EFI variable store), or 'none' (the "
        "default; 'auto' is accepted too) for a flash that starts from the "
        "-bios image every run.  The image's FIT-declared components are "
        "programmed from -bios at every start; the other blocks persist.  "
        "A 64 KiB variable store from the earlier NVRAM window is imported.  "
        "A file of another size, or one written by a firmware with other "
        "NVRAM blocks, is refused and left unchanged.");
    object_class_property_add_str(oc, "alat",
                                  ia64_vpc_get_alat,
                                  ia64_vpc_set_alat);
    object_class_property_set_description(oc, "alat",
        "Set the IA-64 ALAT model to 'zero' (default) or 'full'");
}

static const TypeInfo ia64_vpc_machine_typeinfo = {
    .name = TYPE_IA64_VPC_MACHINE,
    .parent = TYPE_MACHINE,
    .abstract = true,
    .instance_size = sizeof(IA64VpcMachineState),
    .instance_init = ia64_vpc_machine_instance_init,
    .instance_finalize = ia64_vpc_machine_instance_finalize,
    .class_size = sizeof(IA64VpcMachineClass),
    .class_init = ia64_vpc_machine_class_init,
};

static void ia64_vpc_register_types(void)
{
    type_register_static(&ia64_vpc_machine_typeinfo);
}

type_init(ia64_vpc_register_types)
