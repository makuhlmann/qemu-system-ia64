/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * IA-64 virtual PC ABI shared by QEMU and its freestanding firmware.
 *
 * This header must remain usable with -nostdinc.  Do not include QEMU or
 * hosted C library headers here; use compiler built-in types only.
 */

#ifndef HW_IA64_VPC_ABI_H
#define HW_IA64_VPC_ABI_H

/*
 * 64-bit constant helper usable from C and from entry.S (assembled with the
 * C preprocessor): the assembler does not understand the ULL suffix.
 */
#ifdef __ASSEMBLER__
#define IA64_U64(x) x
#else
#define IA64_U64(x) x##ULL
#endif

/*
 * The QEMU->firmware handoff block lives in the machine-RAM-backed firmware
 * address-space window [0xFF000000, 4 GiB), not in guest low RAM: its old
 * home at 0xFF000 sat inside the sub-1 MB compatibility area, which real
 * firmware hands to the OS (shadowed IA-32 BIOS DRAM), and which the Phase 2
 * map rework publishes accordingly.  The handoff page and the (guest-
 * invisible) watchdog assist page sit at 0xFF0FF000/0xFF0FE000, clear of
 * the 0xFFC00000+ flash range a future flash-resident firmware would claim;
 * the NVRAM window sits inside that range at the real SDV flash's
 * NVRAM-sector address.
 */
#define IA64_FW_HANDOFF_ADDR          IA64_U64(0x00000000ff0ff000)
#define IA64_FW_HANDOFF_MAGIC         IA64_U64(0x4d41523436414951) /* "QIA64RAM" */
#define IA64_FW_HANDOFF_VERSION       14ULL
/* Handoff version that first carries IA64VpcHandoff.BootTimeout. */
#define IA64_FW_HANDOFF_BOOT_TIMEOUT_VERSION 13ULL
/* Handoff version that first carries IA64VpcHandoff.ChipsetProfile. */
#define IA64_FW_HANDOFF_CHIPSET_VERSION 14ULL
/*
 * Core-chipset personality (IA64VpcHandoff.ChipsetProfile, version 14+).  The
 * machine's -machine chipset= option overrides the CPU-family default the
 * firmware would otherwise derive (Merced => 460GX, else E8870).
 */
#define IA64_FW_CHIPSET_DERIVE        0ULL  /* derive from CPU family (legacy) */
#define IA64_FW_CHIPSET_460GX         1ULL
#define IA64_FW_CHIPSET_ZX1           2ULL
/* Default boot-manager Timeout: wait for the user forever (EFI sample). */
#define IA64_FW_BOOT_TIMEOUT_WAIT_FOREVER 0xffffU
/* Offset of IA64VpcHandoff.RamSize, re-derived by entry.S. */
#define IA64_FW_HANDOFF_RAMSIZE_OFFSET 16

#define IA64_FW_CONSOLE_SERIAL        0ULL
#define IA64_FW_CONSOLE_VGA           1ULL
#define IA64_FW_DEBUG_PORT_PRESENT    1ULL

#define IA64_VPC_MAX_CPUS             8U

/*
 * Memory-map quirk bits (IA64VpcHandoff.MapQuirkDisable, handoff version
 * 11+).  Each bit DISABLES one guest-specific map workaround the firmware
 * applies by default; all-zero keeps the validated default map.  The
 * motivating guest bug for each lives at the emission site in
 * roms/ia64-firmware/efi_memmap.c and in plans/firmware-rework-plan.md.
 */
#define IA64_FW_QUIRK_LOADER_SPLIT_PAGE    (1ULL << 0) /* 8K page below 32 MB */
#define IA64_FW_QUIRK_LOW_BOUNDARIES       (1ULL << 1) /* 32/48/64/80 MB no-coalesce */
#define IA64_FW_QUIRK_LOW_ANCHOR           (1ULL << 2) /* 8K reserve at 128 MB */
#define IA64_FW_QUIRK_ANCHOR_VERSION_SNIFF (1ULL << 3) /* drop anchor for >=5.2.3790 loaders */
#define IA64_FW_QUIRK_SCRATCH_2G           (1ULL << 4) /* 1 MiB reserve at 2 GiB */
#define IA64_FW_QUIRK_PAL_8K_PAGE          (1ULL << 5) /* whole-8K EfiPalCode page */
#define IA64_FW_QUIRK_ACPI_LOW_ISLAND      (1ULL << 6) /* ACPI tables at 8 MB */
#define IA64_FW_QUIRK_ALL                  0x7fULL

/*
 * CPU-private physical memory used before and after ExitBootServices().
 *
 * Real IA-64 firmware keeps low DRAM contiguous from 1 MiB up to the PCI/MMIO
 * aperture and carves its own SAL/boot scratch from the TOP of installed RAM
 * (460GX SDV: "allocate PAL/SAL memory near top of RAM"; E8870 SR870BH2: a
 * SAL data block at negative offsets from a RAM-top base holding the BSP
 * backing store and MP buffers - plans/sdv-i2000-firmware-reference.md 6.1,
 * plans/sr870bh2-firmware-reference.md 6.2).  The fork does the same: the
 * 2 MiB CPU-assist region (per-CPU SAL re-entry slots, debug contexts and
 * stacks, initial RSE backing stores, and the boot memory stacks) sits at
 * [low_ram_end - 2 MiB, low_ram_end), where low_ram_end is installed RAM
 * clamped to the PCI aperture and rounded down to IA64_FW_LOW_RAM_ALIGN.
 * Low DRAM below it stays conventional (the firmware's efi_init_memory_map
 * keeps only the loader-contract boundaries described there), so OS loaders
 * that map their working set with large translation registers (Server 2003
 * SP1 setupldr: 64 MiB pages at [64 MiB, 192 MiB)) are satisfied.
 *
 * The minimum machine has 128 MiB of low RAM, where this layout coincides
 * exactly with the historical fixed [126 MiB, 128 MiB) region.
 *
 * entry.S includes this header (it is assembled with the C preprocessor)
 * and re-derives the region base from the handoff block with these
 * constants; its AP stack stride shift must match IA64_FW_CPU_STACK_SIZE.
 */
#define IA64_FW_LOW_RAM_MIN            IA64_U64(0x0000000008000000)
#define IA64_FW_LOW_RAM_ALIGN          IA64_U64(0x0000000000002000)
#define IA64_FW_CPU_ASSIST_SIZE        IA64_U64(0x0000000000200000)

/* Offsets inside the CPU-assist region. */
#define IA64_FW_SAL_RUNTIME_OFFSET     0x0000000000000000ULL
#define IA64_FW_SAL_RUNTIME_SLOT_SIZE  0x0000000000008000ULL
#define IA64_FW_SAL_RUNTIME_END_OFFSET \
    (IA64_FW_SAL_RUNTIME_OFFSET + \
     IA64_VPC_MAX_CPUS * IA64_FW_SAL_RUNTIME_SLOT_SIZE)

#define IA64_FW_DEBUG_CONTEXT_OFFSET   0x0000000000040000ULL
#define IA64_FW_DEBUG_CONTEXT_STRIDE   0x0000000000000800ULL
#define IA64_FW_DEBUG_CONTEXT_SIZE     1192U
#define IA64_FW_DEBUG_CONTEXT_END_OFFSET \
    (IA64_FW_DEBUG_CONTEXT_OFFSET + \
     IA64_VPC_MAX_CPUS * IA64_FW_DEBUG_CONTEXT_STRIDE)

#define IA64_FW_DEBUG_STACK_OFFSET     0x0000000000080000ULL
#define IA64_FW_DEBUG_STACK_SIZE       0x0000000000008000ULL
#define IA64_FW_DEBUG_STACK_END_OFFSET \
    (IA64_FW_DEBUG_STACK_OFFSET + \
     IA64_VPC_MAX_CPUS * IA64_FW_DEBUG_STACK_SIZE)

#define IA64_FW_EARLY_RSE_OFFSET       0x00000000000c0000ULL
#define IA64_FW_EARLY_RSE_SIZE         0x0000000000008000ULL
#define IA64_FW_EARLY_RSE_END_OFFSET \
    (IA64_FW_EARLY_RSE_OFFSET + \
     IA64_VPC_MAX_CPUS * IA64_FW_EARLY_RSE_SIZE)

#define IA64_FW_CPU_STACK_SIZE         0x0000000000020000ULL
#define IA64_FW_BOOT_STACK_SIZE \
    (IA64_VPC_MAX_CPUS * IA64_FW_CPU_STACK_SIZE)
/* The boot memory stacks occupy the top of the region, ending at low_ram_end. */
#define IA64_FW_BOOT_STACK_OFFSET \
    (IA64_FW_CPU_ASSIST_SIZE - IA64_FW_BOOT_STACK_SIZE)

/*
 * RAM-top firmware image shadow (rework phase 2.2).  The machine loads the
 * firmware binary at IA64_FW_IMAGE_BASE_FOR(ram_size) - 1 MB aligned, sized
 * for the image plus bss with headroom (the linker asserts the real span
 * fits) - applies the image's self-relocation fixup table for the delta from
 * the 1 MB link base, and seeds each CPU's fw_image_base.  Above the image
 * sit the ACPI staging region and the CPU-assist region, ending exactly at
 * the end of installed low RAM, mirroring how real 460GX/E8870 firmware
 * shadows itself near the top of memory.
 */
#define IA64_FW_IMAGE_SPAN            IA64_U64(0x0000000000400000)
/* The firmware IVT lives inside the image at this fixed link offset. */
#define IA64_FW_IVT_OFFSET            IA64_U64(0x0000000000008000)
#define IA64_FW_ACPI_REGION_SIZE      IA64_U64(0x0000000000020000)

/* low_ram_end for an installed RAM size, as both QEMU and the firmware see it. */
#define IA64_FW_LOW_RAM_END(ram_size) \
    ((((ram_size) < IA64_PCI_MMIO_BASE ? (ram_size) : IA64_PCI_MMIO_BASE)) & \
     ~(IA64_FW_LOW_RAM_ALIGN - 1ULL))
#define IA64_FW_CPU_ASSIST_BASE_FOR(ram_size) \
    (IA64_FW_LOW_RAM_END(ram_size) - IA64_FW_CPU_ASSIST_SIZE)
/* 4 MB aligned: the SST names a truthful 4 MB ITR(0) over the shadow. */
#define IA64_FW_IMAGE_BASE_FOR(ram_size) \
    ((IA64_FW_CPU_ASSIST_BASE_FOR(ram_size) - IA64_FW_ACPI_REGION_SIZE - \
      IA64_FW_IMAGE_SPAN) & ~IA64_U64(0xfffff))

#ifndef __ASSEMBLER__
/*
 * Layout guards.  The early RSE backing stores and the boot stacks share the
 * CPU-assist region; the equality below holds only because of the current
 * IA64_VPC_MAX_CPUS, so raising the CPU cap must not silently overlap them.
 */
_Static_assert(IA64_FW_EARLY_RSE_END_OFFSET <= IA64_FW_BOOT_STACK_OFFSET,
               "early RSE backing stores overlap the boot stacks");
_Static_assert(IA64_FW_CPU_STACK_SIZE == (1ULL << 17),
               "entry.S derives the AP stack stride as shl 17");
#endif

#define IA64_UART_BASE                0x00000047f0000000ULL
#define IA64_DEBUG_UART_BASE          0x00000047f0001000ULL
#define IA64_UART_MMIO_SIZE           0x0000000000002000ULL

/*
 * The PCI/MMIO aperture sits just below the fixed chipset/SAPIC/firmware
 * region [0xFE000000, 4 GiB), mirroring real 460GX hardware, which keeps a
 * single MMIO gap at the top of the 32-bit space so DRAM stays contiguous up
 * to it and any displaced RAM is remapped above 4 GiB (see
 * plans/sdv-i2000-firmware-reference.md 7.1).
 */
#define IA64_PCI_MMIO_BASE            IA64_U64(0x00000000ee000000)
#define IA64_PCI_MMIO_SIZE            IA64_U64(0x0000000010000000)

/*
 * IA-64 legacy I/O port block and PCI config space.  (Deviation from real
 * hardware for the CONFIG space: the 460GX has no MMCFG at all -- see
 * plans/firmware-rework-target-model.md D7.)  The I/O port block sits at the
 * architected default: the top 64 MB of the 44-bit PA space (rework D6,
 * phase 3; formerly the invented 0x800010000000, which needed 48 PA bits no
 * real Merced or Madison implements).
 */
#define IA64_PCI_IO_BASE              IA64_U64(0x00000ffffc000000)
#define IA64_PCI_IO_SIZE              IA64_U64(0x0000000001000000)
#define IA64_PCI_IO_SPARSE_SKIP       IA64_U64(0x0000000000001000)
/* Sparse IA-64 port encoding expands the legacy 16-bit I/O port space. */
#define IA64_PCI_IO_SPARSE_SIZE       IA64_U64(0x0000000004000000)
/*
 * PCI config window at the E8870's MMCFG home (SR870BH2 reference: 64 MB at
 * 0xFFFF8000000, directly below the architected I/O block).  ECAM semantics
 * are an interim simplification (real E8870 encodes 16 bytes per dword);
 * the 460GX profile does not advertise it at all - no MCFG, no descriptor -
 * so 460GX-profile guests use SAL_PCI_CONFIG, as on real hardware (D7).
 */
#define IA64_PCI_CONFIG_BASE          IA64_U64(0x00000ffff8000000)
#define IA64_PCI_CONFIG_SIZE          IA64_U64(0x0000000004000000)

/*
 * Fixed platform device and interrupt-block addresses.  Single source for
 * the machine model (hw/ia64/), the CPU/PAL code (target/ia64/) and the
 * firmware, which used to carry parallel transcriptions of this block.
 */
#define IA64_IVT_BASE                 IA64_U64(0x0000000000010000)
#define IA64_IVT_SIZE                 IA64_U64(0x0000000000008000)
/*
 * IOSAPIC at the 460GX/i2000 SDV address (SAPIC/IOAPIC message block just
 * below the local SAPIC at 0xFEE00000), inside the fixed chipset region above
 * the PCI aperture.  Keeping it here -- rather than the old 2 GiB parking spot
 * -- leaves low DRAM contiguous all the way to the aperture.
 */
#define IA64_IOSAPIC_BASE             IA64_U64(0x00000000fec00000)
#define IA64_IOSAPIC_MMIO_SIZE        IA64_U64(0x0000000000002000)
#define IA64_LOCAL_SAPIC_BASE         IA64_U64(0x00000000fee00000)
#define IA64_LOCAL_SAPIC_SIZE         IA64_U64(0x0000000000200000)
/*
 * HP zx1 SBA (System Bus Adapter) IOC CSR block -- only mapped by the zx1
 * chipset profile.  Placed in the free chipset MMIO gap between the IOSAPIC
 * (0xFEC00000) and the local SAPIC (0xFEE00000); this is the real zx1 mio
 * config base (mio ERS 3.1.1) and matches the upstream model.  Linux
 * sba_iommu reads the IOC registers at base + ZX1_IOC_OFFSET(0x1000) +
 * IBASE(0x300); the model maps the IOMMU register window there (CSR offset
 * 0x1300).  The window is described to guests only through the ACPI HWP0001
 * _CRS, never the EFI memory map (see roms/ia64-firmware/efi_memmap.c).
 */
#define IA64_SBA_CSR_BASE             IA64_U64(0x00000000fed00000)
#define IA64_SBA_CSR_SIZE             IA64_U64(0x0000000000010000)
/*
 * The IOC (MIO "function 1") identity registers, read by Linux sba_iommu's
 * ioc_init() at IOC base + 0x000 (FUNC_ID) and + 0x008 (FCLASS).  Values from
 * the HP zx1 mio ERS (Reg 21/22, p.46-47) and upstream hp-zx1-mio-regs.c:
 * FUNC_ID = device 0x122a (IOC) | vendor 0x103c (HP); FCLASS low byte 0x23 is
 * revision 2.3 (>= the 2.0 the driver requires), then class 0x068000 and a
 * 0x20 (128-byte) cache-line.  With FUNC_ID served, the driver's
 * func_id == ZX1_IOC_ID test passes and it runs the zx1-specific ioc_zx1_init()
 * path (name, dma_mask, rope config) instead of falling back to the generic
 * "Unknown 0.0" IOC.  These live in the IOC function block (CSR offset 0x1000);
 * keep in lockstep with ia64_sba.c.
 */
#define IA64_SBA_IOC_FUNC_ID          IA64_U64(0x00000000122a103c)
#define IA64_SBA_IOC_FCLASS           IA64_U64(0x0000002006800023)
/*
 * The zx1 SBA "safe IOVA space": the 1 GiB window at 1 GiB the IOC advertises
 * through IBASE/IMASK and that the OS's sba_iommu allocates IOVAs from.  On the
 * zx1 machine this range is a DRAM HOLE -- the machine shifts the RAM that would
 * sit here up past the window, exactly as real zx1 keeps a "Virtual I/O" hole so
 * the IOVA window never overlaps memory.  Without the hole, Linux sba_iommu's
 * ALLOW_IOV_BYPASS path (which DMAs to a buffer's raw physical address) can put
 * a >1 GiB buffer inside the enabled window and have the IOC mis-translate it.
 *
 * The hole is only carved when installed RAM exceeds the PCI aperture
 * (IA64_PCI_MMIO_BASE), i.e. when there is already RAM displaced above 4 GiB and
 * the low band fills to the aperture regardless of the hole (see the gate in
 * hw/ia64/ia64_vpc.c and fw_zx1_iova_hole_active() in the firmware).  Carving it
 * for a smaller guest would move the top of low RAM -- and the firmware image,
 * CPU-assist region and SRAT/SMBIOS ranges pinned near it -- which needs a
 * hole-aware low_ram_end the firmware does not yet compute; see
 * plans/zx1-chipset-port-plan.md.  hw/ia64/ia64_vpc.c (RAM map),
 * roms/ia64-firmware/efi_memmap.c (EFI map) and platform.c (high-RAM ranges)
 * carve this hole in lockstep.
 */
#define IA64_SBA_IOVA_BASE            IA64_U64(0x0000000040000000)
#define IA64_SBA_IOVA_SIZE            IA64_U64(0x0000000040000000) /* 1 GiB */
#define IA64_SBA_IOVA_END \
    (IA64_SBA_IOVA_BASE + IA64_SBA_IOVA_SIZE)
/*
 * The zx1 LBA (Local Bus Adapter / Mercury I/O adapter) config block.  Linux
 * hp-agp (drivers/char/agp/hp-agp.c) finds it via the ACPI HWP0003 device's
 * CCSR VendorLong resource, ioremaps it, and reads it as PCI config space to
 * locate an AGP capability: PCI_STATUS(0x06).CAP_LIST, cap-list pointer at
 * 0x34, AGP capability (id 0x02) at 0x60, AGP status at 0x64, AGP command at
 * 0x68.  It is a small MMIO block, distinct from the SBA CSR, placed just above
 * it in the chipset gap.  The register values mirror upstream's
 * hw/pci-host/hp-zx1-ioa-regs.c (AGP mode).  Keep base/length in lockstep with
 * the LBA0 _CRS in dsdt-pci-root-zx1.asl.
 */
#define IA64_LBA_CSR_BASE             IA64_U64(0x00000000fed10000)
#define IA64_LBA_CSR_SIZE             IA64_U64(0x0000000000001000)
/* AGP capability (byte 0x60) and writable-command mask, from the IOA model. */
#define IA64_LBA_AGP_CAPABILITY       IA64_U64(0x0f00023700200002)
#define IA64_LBA_AGP_COMMAND_WRITABLE IA64_U64(0x0000000000000337)
#define IA64_LBA_PCI_STATUS_RESET     IA64_U64(0x00000000000002b0)
#define IA64_LBA_VENDOR_ID            IA64_U64(0x000000000000103c) /* HP */
#define IA64_LBA_DEVICE_ID            IA64_U64(0x000000000000122e) /* zx1 LBA */
#define IA64_LBA_AGP_CAP_OFFSET       IA64_U64(0x0000000000000060)
/*
 * Mercury FUNCTION_CLASS (CSR 0x08): PCI class code 0x060000 (host bridge) in
 * bytes 0x09-0x0b and revision 0x32 (rev 2.0) in byte 0x08; cache-line/latency
 * (bytes 0x0c/0x0d) reset to 0.  Values from the HP zx1 ioa ERS and the upstream
 * hp-zx1-ioa-regs.h identity constants.
 */
#define IA64_LBA_CLASS_CODE           IA64_U64(0x0000000000060000)
#define IA64_LBA_REVISION             IA64_U64(0x0000000000000032)
/*
 * The HP zx1 Mercury (LBA/ioa) presents its own PCI root bus so the AGP graphics
 * adapter sits behind it, exactly as on real zx1 hardware: the ACPI HWP0003 node
 * carries _CID PNP0A03, Windows pci.sys owns it as a PCI root bridge (per
 * HpAgp.inf), and the graphics is enumerated on its child bus while hpagp filters
 * it.  The child bus number must be reachable through the single segment-0 ECAM
 * window: a config address carries the bus in bits [27:20] and the config
 * aperture is IA64_PCI_CONFIG_SIZE (64 MiB), so only buses 0..63 are decodable --
 * IA64_MERCURY_BUS must stay <= 0x3f (checked in hw/ia64/ia64_mercury.c).  PCI0
 * keeps bus 0; the graphics lands at slot 0 of the Mercury bus.  Only the zx1
 * chipset profile creates it; keep in lockstep with LBA0 in dsdt-pci-root-zx1.asl.
 */
#define IA64_MERCURY_BUS              0x10

/*
 * 460GX expander roots.  The i2000 reaches its PCI buses through expander
 * bridges on the System Address Controller: the PXB carries the
 * compatibility bus 0, the two WXBs carry buses 1 and 2, and the GXB carries
 * the AGP bus 3.  Each root owns its own block of four INTx inputs on the
 * Programmable Interrupt Device, starting at IA64_PCI_INTX_GSI_BASE.
 */
#define IA64_460GX_EXPANDER_ROOTS     3
#define IA64_460GX_WXB0_BUS           0x01
#define IA64_460GX_WXB1_BUS           0x02
#define IA64_460GX_GXB_BUS            0x03
#define IA64_MERCURY_VGA_SLOT        0x00
/* 16 MiB PAL/SAL firmware address space below 4 GiB. */
#define IA64_FW_ADDRESS_SPACE_BASE    IA64_U64(0x00000000ff000000)
#define IA64_FW_ADDRESS_SPACE_SIZE    IA64_U64(0x0000000001000000)
#define IA64_FW_ADDRESS_SPACE_END \
    (IA64_FW_ADDRESS_SPACE_BASE + IA64_FW_ADDRESS_SPACE_SIZE)
/*
 * Invented MMIO devices inside the firmware window (deviation D8 in
 * plans/firmware-rework-target-model.md; to be relocated).
 */
/* QEMU-internal EFI watchdog assist; undescribed, guests never see it. */
#define IA64_WATCHDOG_BASE            IA64_U64(0x00000000ff0fe000)
#define IA64_WATCHDOG_SIZE            IA64_U64(0x0000000000001000)
#define IA64_WATCHDOG_TIMEOUT_OFFSET  0x00U
#define IA64_WATCHDOG_CODE_OFFSET     0x08U
/*
 * EFI variable store, standing in for the flash variable sector.  The real
 * i2000/SDV flash keeps its NVRAM/variable scratch block at 0xFFF90000
 * (FIT type 0x1E, 128 KB - plans/sdv-i2000-firmware-reference.md sec 11),
 * so the window sits at that address.
 */
#define IA64_NVRAM_BASE               IA64_U64(0x00000000fff90000)
#define IA64_NVRAM_SIZE               IA64_U64(0x0000000000010000)
#define IA64_NVRAM_COMMIT_OFFSET      (IA64_NVRAM_SIZE - 8U)
#define IA64_NVRAM_COMMIT_MAGIC       IA64_U64(0x54494d4d4f43564e) /* "NVCOMMIT" */
/* ACPI PM block in PCI I/O port space, and the SCI it raises. */
#define IA64_ACPI_PM_IO_BASE          0x00002000U
#define IA64_ACPI_PM_IO_SIZE          0x00000010U
#define IA64_ACPI_PM_RESET_OFFSET     0x0000000cU
#define IA64_ACPI_PM_RESET_VALUE      0x01U
#define IA64_ACPI_SCI_IRQ             9

#ifndef __ASSEMBLER__
typedef struct __attribute__((packed)) IA64VpcHandoff {
    unsigned long long Magic;
    unsigned long long Version;
    unsigned long long RamSize;
    unsigned long long ConsolePolicy;
    unsigned long long IdeDmaEnabled;
    unsigned long long DebugPortFlags;
    unsigned long long DebugPortBase;
    unsigned long long I8042Enabled;
    unsigned long long ProcessorCount;
    unsigned long long NvramPersistent;
    unsigned long long SocketCount;
    unsigned long long CoresPerSocket;
    unsigned long long ThreadsPerCore;
    unsigned long long MapQuirkDisable;   /* version 11+ */
    unsigned long long BootTimeout;       /* version 13+; default boot-manager
                                           * Timeout (seconds) when no NVRAM
                                           * Timeout variable exists.  0xFFFF
                                           * (the default) waits for the user
                                           * like the EFI sample; 0 boots the
                                           * BootOrder immediately. */
    unsigned long long ChipsetProfile;    /* version 14+; IA64_FW_CHIPSET_*.
                                           * 0 = derive from CPU family. */
} IA64VpcHandoff;

_Static_assert(__builtin_offsetof(IA64VpcHandoff, RamSize) ==
               IA64_FW_HANDOFF_RAMSIZE_OFFSET,
               "entry.S reads RamSize at this offset");
#endif /* __ASSEMBLER__ */

#endif /* HW_IA64_VPC_ABI_H */
