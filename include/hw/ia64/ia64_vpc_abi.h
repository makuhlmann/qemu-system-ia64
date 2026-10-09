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

/* The firmware's link base; it executes from the RAM-top shadow. */
#define IA64_FW_LINK_BASE             IA64_U64(0x0000000000100000)
/*
 * Core-chipset personality, as the firmware's flash stage probes it
 * (roms/ia64-firmware/flash_probe.c): the 460GX SAC on bus 0 device 10h,
 * else the zx1 mio's IOC function ID; DERIVE falls back to the CPU family.
 */
#define IA64_FW_CHIPSET_DERIVE        0ULL
#define IA64_FW_CHIPSET_460GX         1ULL
#define IA64_FW_CHIPSET_ZX1           2ULL
/* Default boot-manager Timeout: wait for the user forever (EFI sample). */
#define IA64_FW_BOOT_TIMEOUT_WAIT_FOREVER 0xffffU

#define IA64_FW_CONSOLE_SERIAL        0ULL
#define IA64_FW_CONSOLE_VGA           1ULL
#define IA64_FW_DEBUG_PORT_PRESENT    1ULL

#define IA64_VPC_MAX_CPUS             8U

/*
 * Memory-map quirk bits (IA64NvramDefaults.MapQuirkDisable).  Each set bit
 * DISABLES one guest-specific map workaround in the firmware; the machine's
 * default mask is IA64_VPC_FW_QUIRK_DEFAULT_DISABLE (hw/ia64/ia64_base.c).
 * The motivating guest bug for each lives at the emission site in
 * roms/ia64-firmware/efi_memmap.c.
 */
#define IA64_FW_QUIRK_LOADER_SPLIT_PAGE    (1ULL << 0) /* 8K page below 32 MB */
#define IA64_FW_QUIRK_LOW_BOUNDARIES       (1ULL << 1) /* 32/48/64/80 MB no-coalesce */
#define IA64_FW_QUIRK_LOW_ANCHOR           (1ULL << 2) /* 8K reserve at 128 MB */
#define IA64_FW_QUIRK_ANCHOR_VERSION_SNIFF (1ULL << 3) /* drop anchor for >=5.2.3790 loaders */
#define IA64_FW_QUIRK_SCRATCH_2G           (1ULL << 4) /* 1 MiB reserve at 2 GiB */
/* Bit 5 was pal-8k-page: the EfiPalCode descriptor is 256 KB now. */
#define IA64_FW_QUIRK_ACPI_LOW_ISLAND      (1ULL << 6) /* ACPI tables at 8 MB */
#define IA64_FW_QUIRK_ALL                  0x5fULL

/*
 * CPU-private physical memory used before and after ExitBootServices().
 *
 * Real IA-64 firmware keeps low DRAM contiguous from 1 MiB up to the PCI/MMIO
 * aperture and carves its own SAL/boot scratch from the TOP of installed RAM
 * (SAL spec 245359-001 3.2.3 step 8: "Allocate memory for use by PAL and SAL
 * near the top of physical memory"; the E8870 SR870BH2 BIOS 86B.0183.P02
 * keeps a 512 KB SAL data block, tagged ___BSP__, MPBUFSTR and others, at
 * negative offsets from a RAM-top base).  The fork does the same: the
 * 2 MiB CPU-assist region (per-CPU SAL re-entry slots, debug contexts and
 * stacks, initial RSE backing stores, and the boot memory stacks) sits at
 * [low_ram_end - 2 MiB, low_ram_end), where low_ram_end is installed RAM
 * clamped to the top of the board's DRAM run at 0 (the PCI aperture on the
 * 460GX, the end of Memory0 on zx1) and rounded down to IA64_FW_LOW_RAM_ALIGN.
 * Low DRAM below it stays conventional (the firmware's efi_init_memory_map
 * keeps only the loader-contract boundaries described there), so OS loaders
 * that map their working set with large translation registers (Server 2003
 * SP1 setupldr: 64 MiB pages at [64 MiB, 192 MiB)) are satisfied.
 *
 * The minimum machine has 128 MiB of low RAM, where this layout coincides
 * exactly with the historical fixed [126 MiB, 128 MiB) region.
 *
 * entry.S includes this header (it is assembled with the C preprocessor)
 * and re-derives the region base from the probed memory size with these
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

/*
 * The application-processor release block, in the gap between the debug
 * contexts and the debug stacks.  The flash stage's application processors
 * leave reset together with the boot processor (PALE_RESET exit state, every
 * processor at SALE_ENTRY) and wait for an IPI with
 * IA64_FW_AP_RELEASE_VECTOR.  Before it sends that IPI, the boot processor
 * writes the shadow's _start (+0) and the installed DRAM size (+8) here, in
 * the region of a minimum machine (IA64_FW_AP_RELEASE_BLOCK), which every
 * machine has and which the flash stage already uses.  cr.irr is clear after
 * a reset, so stale contents cannot start a processor.
 *
 * The word at +16 (IA64_FW_AP_PRESENT_OFF) has bit n set for each processor
 * id n that waits for the release.  The boot processor's reset entry clears
 * it, and every waiting processor sets its bit again while it polls, so the
 * boot processor knows which processors exist before it releases them.
 */
#define IA64_FW_AP_RELEASE_OFFSET      0x0000000000044000ULL
#define IA64_FW_AP_RELEASE_SIZE        0x0000000000000018ULL
#define IA64_FW_AP_PRESENT_OFF         0x10
#define IA64_FW_AP_RELEASE_BLOCK \
    (IA64_FW_LOW_RAM_MIN - IA64_FW_CPU_ASSIST_SIZE + IA64_FW_AP_RELEASE_OFFSET)
#define IA64_FW_AP_RELEASE_VECTOR      0xf0

/*
 * One min-state save area per processor id, which every processor registers
 * with PAL_MC_REGISTER_MEM (SAL spec 245359-007 5.1; SDM Vol. 2 11.3.2.4:
 * 4 KB, 512-byte aligned).  PAL writes the first 0x1d0 bytes; SAL owns the
 * rest of the first 1 KB and keeps there the physical entry of its SAL_INIT
 * (read by SALE_ENTRY) and whether the OS runs on the processor.  The areas
 * are uncacheable, 16 KB away from any other data, and reported to the OS as
 * EfiMemoryMappedIO (SAL spec 3.3.2): the guard is part of that range.
 */
#define IA64_FW_MINSTATE_OFFSET        0x0000000000050000ULL
#define IA64_FW_MINSTATE_SIZE          0x0000000000001000ULL
#define IA64_FW_MINSTATE_GUARD         0x0000000000004000ULL
#define IA64_FW_MINSTATE_END_OFFSET \
    (IA64_FW_MINSTATE_OFFSET + IA64_VPC_MAX_CPUS * IA64_FW_MINSTATE_SIZE)
#define IA64_FW_MINSTATE_OS_OWNED_OFF  0x3e8
#define IA64_FW_MINSTATE_SAL_INIT_OFF  0x3f0

#define IA64_FW_CPU_STACK_SIZE         0x0000000000020000ULL
#define IA64_FW_BOOT_STACK_SIZE \
    (IA64_VPC_MAX_CPUS * IA64_FW_CPU_STACK_SIZE)
/* The boot memory stacks occupy the top of the region, ending at low_ram_end. */
#define IA64_FW_BOOT_STACK_OFFSET \
    (IA64_FW_CPU_ASSIST_SIZE - IA64_FW_BOOT_STACK_SIZE)

/*
 * RAM-top firmware image shadow (55e553d).  The machine loads the
 * firmware binary at IA64_FW_IMAGE_BASE_FOR(ram_size, low_top) - 1 MB
 * aligned, sized for the image plus bss with headroom (the linker asserts the
 * real span fits) - applies the image's self-relocation fixup table for the
 * delta from the 1 MB link base.  Above the image
 * sit the ACPI staging region and the CPU-assist region, ending exactly at
 * the end of installed low RAM, mirroring how real 460GX/E8870 firmware
 * shadows itself near the top of memory.
 */
#define IA64_FW_IMAGE_SPAN            IA64_U64(0x0000000000400000)
/* The firmware IVT lives inside the image at this fixed link offset. */
#define IA64_FW_IVT_OFFSET            IA64_U64(0x0000000000008000)
/*
 * The SAL runtime stub trio at fixed offsets in the image (entry.S
 * .text.sal_runtime, pinned by firmware.lds), and the PAL procedure entry
 * stub.  Only the firmware's link layout and the machine's no-firmware test
 * entry state use these; the CPU learns the addresses from the firmware's
 * registration below.
 */
#define IA64_FW_SAL_RUNTIME_ENTRY_OFF  0x2000
#define IA64_FW_SAL_RUNTIME_RETURN_OFF 0x2020
#define IA64_FW_SAL_DISPATCH_BLOCK_OFF 0x2040
/*
 * The buffer PAL copies itself into (PAL_COPY_PAL, SAL 3.2.3 step 9), past
 * the image's bss.  It is the alignment every vendor PAL_COPY_INFO asks for,
 * and also the most the SDM allows (Vol. 2 PAL_COPY_INFO); the rx2600's
 * EfiPalCode descriptor is 256 KB too (capture 2026-10-03, memmap).
 */
#define IA64_FW_PAL_BUFFER_SIZE        0x40000
/* The low part of the image a firmware context reaches identity-mapped. */
#define IA64_FW_IDENTITY_WINDOW_SIZE   IA64_U64(0x0000000000100000)

/*
 * PAL_FIRMWARE_REGISTER: an implementation-specific static PAL procedure
 * (index range 512-767, SDM vol. 2 table 11-11) of the PAL emulation.  The
 * project firmware calls it on every processor once it runs from its RAM
 * shadow, the way SAL registers its PMI entry with PAL (SAL 3.2.3 step 12),
 * to hand over what the emulator's firmware assists need: its IVT, the
 * image window that a firmware context reaches identity-mapped, the SAL
 * runtime stubs and dispatch block, and the CPU-assist region.  A processor
 * that never registers -- under any other firmware -- gets none of those
 * assists.
 *
 *   r28 = IA64_PAL_FIRMWARE_REGISTER, r29 = physical address of the record,
 *   r30 = record size in bytes.  Returns r8 = 0, or -1 (not implemented)
 *   for a record this emulator does not recognise.
 *
 * The record: little-endian 64-bit words at the offsets below.
 */
#define IA64_PAL_FIRMWARE_REGISTER          0x200
#define IA64_FW_REGISTRATION_MAGIC          IA64_U64(0x4752574634364149) /* "IA64FWRG" */
#define IA64_FW_REGISTRATION_SIZE           64
#define IA64_FW_REGISTRATION_MAGIC_OFF      0x00
#define IA64_FW_REGISTRATION_IMAGE_BASE_OFF 0x08
#define IA64_FW_REGISTRATION_IMAGE_SIZE_OFF 0x10
#define IA64_FW_REGISTRATION_IVT_OFF        0x18
#define IA64_FW_REGISTRATION_SAL_ENTRY_OFF  0x20
#define IA64_FW_REGISTRATION_SAL_RETURN_OFF 0x28
#define IA64_FW_REGISTRATION_SAL_BLOCK_OFF  0x30
#define IA64_FW_REGISTRATION_ASSIST_OFF     0x38
#define IA64_FW_ACPI_REGION_SIZE      IA64_U64(0x0000000000020000)

/*
 * low_ram_end for an installed RAM size, as both QEMU and the firmware see it;
 * low_top is where the board's DRAM run at 0 ends when RAM fills it
 * (IA64_460GX_LOW_RAM_END on the 460GX, IA64_ZX1_MEMORY0_END on zx1).
 */
#define IA64_FW_LOW_RAM_END(ram_size, low_top) \
    ((((ram_size) < (low_top) ? (ram_size) : (low_top))) & \
     ~(IA64_FW_LOW_RAM_ALIGN - 1ULL))
#define IA64_FW_CPU_ASSIST_BASE_FOR(ram_size, low_top) \
    (IA64_FW_LOW_RAM_END(ram_size, low_top) - IA64_FW_CPU_ASSIST_SIZE)
/* 4 MB aligned: the SST names a truthful 4 MB ITR(0) over the shadow. */
#define IA64_FW_IMAGE_BASE_FOR(ram_size, low_top) \
    ((IA64_FW_CPU_ASSIST_BASE_FOR(ram_size, low_top) - \
      IA64_FW_ACPI_REGION_SIZE - IA64_FW_IMAGE_SPAN) & ~IA64_U64(0xfffff))

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
 * to it and any displaced RAM is remapped above 4 GiB (SSDM 4.1.3, 4.1.5).
 */
#define IA64_PCI_MMIO_BASE            IA64_U64(0x00000000ee000000)
#define IA64_PCI_MMIO_SIZE            IA64_U64(0x0000000010000000)

/*
 * The 460GX's variable gap also holds the GXB's AGP aperture, below the PCI
 * windows (SSDM 7.2.1, the reserved-gap case): 256 MB, on a 256 MB boundary
 * because AGP_BASE bits 27:12 are hardwired (7.1), and clear of DRAM and PCI
 * space (7.2.3).  The DRAM band at 0 ends where the gap starts; the gap plus
 * the fixed 32 MB is then 768 MB, a multiple of 64 MB as 4.1.3.1 requires.
 */
#define IA64_460GX_AGP_APERTURE_BASE  IA64_U64(0x00000000d0000000)
/* The GXB's GART SRAM window (SSDM 7.1.2, p.4-3). */
#define IA64_460GX_GART_SRAM_BASE     IA64_U64(0x00000000fe200000)
#define IA64_460GX_AGP_APERTURE_SIZE  IA64_U64(0x0000000010000000)
#define IA64_460GX_LOW_RAM_END        IA64_460GX_AGP_APERTURE_BASE

/*
 * IA-64 legacy I/O port block and PCI config space.  (Deviation from real
 * hardware for the CONFIG space: the 460GX has no MMCFG at all, only
 * mechanism #1 at CF8/CFC, SSDM 2.1.)  The I/O port block sits at the
 * architected default: the 64 MB below the top of the 44-bit PA space
 * (SAL spec 245359-001 3.2.1), where bios130.BIN's I/O helper at
 * 0xFFFEA350 also puts it.
 */
#define IA64_PCI_IO_BASE              IA64_U64(0x00000ffffc000000)
#define IA64_PCI_IO_SIZE              IA64_U64(0x0000000001000000)
#define IA64_PCI_IO_SPARSE_SKIP       IA64_U64(0x0000000000001000)
/* Sparse IA-64 port encoding expands the legacy 16-bit I/O port space. */
#define IA64_PCI_IO_SPARSE_SIZE       IA64_U64(0x0000000004000000)
/*
 * PCI config window at the E8870's MMCFG home (E8870 SNC datasheet 4.1.4:
 * 64 MB above 4 GB; the SR870BH2 BIOS 86B.0183.P02 puts it at
 * 0xFFFF8000000, directly below the architected I/O block).  ECAM semantics
 * are an interim simplification (real E8870 encodes 16 bytes per dword);
 * the 460GX profile does not advertise it at all - no MCFG, no descriptor -
 * so 460GX-profile guests use SAL_PCI_CONFIG, as on real hardware.
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
/* Module Info (mio ERS register 3): module 0x000a, functions 0, 1, 8, 9, 10. */
#define IA64_SBA_MODULE_INFO          IA64_U64(0x000000000703000a)
/*
 * FUNC_ID of function 0, the "Root Bridge" 1229, and of function 8, the
 * memory controller 122B.  The vendor shell's "info chiprev" takes the device
 * from bits 31:16 (FFEEA8C0 reads FED0_8000) and the revision as the largest
 * FCLASS low byte of functions 0, 1, 8, 9 and 10 (FFEEA810).  rx2600 capture
 * 2026-10-03, MIO-1; function 8's vendor half is HP's, as in the others.
 */
#define IA64_SBA_FUNC0_ID             IA64_U64(0x000000001229103c)
#define IA64_SBA_MC_FUNC_ID           IA64_U64(0x00000000122b103c)
/*
 * The zx1 SBA "safe IOVA space": the 1 GiB window at 1 GiB the IOC advertises
 * through IBASE/IMASK and that the OS's sba_iommu allocates IOVAs from.  The
 * mio maps no memory there (mio ERS 2.1, "The I/O Virtual Region").
 */
#define IA64_SBA_IOVA_BASE            IA64_U64(0x0000000040000000)
#define IA64_SBA_IOVA_SIZE            IA64_U64(0x0000000040000000) /* 1 GiB */
#define IA64_SBA_IOVA_END \
    (IA64_SBA_IOVA_BASE + IA64_SBA_IOVA_SIZE)
/*
 * zx1 mio DRAM (mio ERS 2.1, Figure 3).  Memory0 runs from 0 to the I/O
 * virtual region at 1 GiB.  The DRAM that would sit from there to 4 GiB is
 * Memory1, at 0x40_4000_0000; Memory2 starts at 4 GiB and ends below Memory1.
 * Memory1 is used only once Memory0 is full, and Memory2 once Memory1 is full.
 */
#define IA64_ZX1_MEMORY0_END          IA64_SBA_IOVA_BASE
#define IA64_ZX1_MEMORY1_BASE         IA64_U64(0x0000004040000000)
#define IA64_ZX1_MEMORY1_SIZE         IA64_U64(0x00000000c0000000) /* 3 GiB */
#define IA64_ZX1_MEMORY2_BASE         IA64_U64(0x0000000100000000)
#define IA64_ZX1_MEMORY2_END          IA64_U64(0x0000004000000000)
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
 * The mio hands I/O port space to the ropes by port bits 15:13, 8 KB a rope,
 * and a double-wide rope gets both ropes' share (mio ERS 2.4.1, 2.5.4); the
 * rx2600's own roots read so (rx2600 capture 2026-10-04, `/proc/ioports`:
 * bus 20h at 2000h, the AGP bus 80h at 8000h-BFFFh).
 */
#define IA64_ZX1_ROPE_IO_SIZE         0x00002000U

/*
 * The AGP ioa is a double-wide rope 4 (B1), so the I/O ports of ropes 4 and
 * 5 are its own; the graphics I/O BAR sits at their start, as the rx2600's
 * card at 80:00.0 does.  Keep in lockstep with LBA0 in dsdt-pci-root-zx1.asl.
 */
#define IA64_ZX1_AGP_IO_BASE          (4U * IA64_ZX1_ROPE_IO_SIZE)
#define IA64_ZX1_AGP_IO_SIZE          (2U * IA64_ZX1_ROPE_IO_SIZE)
/*
 * Rope 1's ioa carries a bus of its own, numbered 0x20 as on the rx2600
 * (its SCRAM gives rope 1 buses 20h-3Fh; rx2600 capture 2026-10-03, DEV-3),
 * with the core I/O SCSI at device 1 and the gigabit LAN at device 2.  Under
 * our firmware the root owns this I/O and memory window, cut out of PCI0's,
 * and its INTx reaches the platform inputs from IA64_ZX1_ROPE1_GSI_BASE.
 * The SCSI's ports are where the rx2600 has its function 0's.  Keep in
 * lockstep with PCI1 in dsdt-pci-root-zx1.asl.
 */
#define IA64_ZX1_ROPE1_BUS            0x20
#define IA64_ZX1_ROPE1_IO_BASE        (1U * IA64_ZX1_ROPE_IO_SIZE)
#define IA64_ZX1_ROPE1_IO_SIZE        IA64_ZX1_ROPE_IO_SIZE
#define IA64_ZX1_SCSI_IO_BASE         (IA64_ZX1_ROPE1_IO_BASE + 0x100U)
/*
 * Function 1 of the rx2600's 53C1030 sits at 2000h (rx2600 capture
 * 2026-10-03).  The stand-in ACPI PM block (IA64_ACPI_PM_IO_BASE) holds that
 * port until it moves to the PDH, where the rx2600 has it, so function 1
 * takes the next free port of rope 1.
 */
#define IA64_ZX1_SCSI_FN1_IO_BASE     (IA64_ZX1_ROPE1_IO_BASE + 0x200U)
#define IA64_ZX1_ROPE1_MMIO_BASE      IA64_U64(0x00000000ef400000)
#define IA64_ZX1_ROPE1_MMIO_SIZE      IA64_U64(0x0000000000400000)
#define IA64_ZX1_ROPE1_GSI_BASE       22

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
/* Where the vendor firmware parks the chipset's own bus (SSDM 2.3.2). */
#define IA64_460GX_CBN_BUS            0xee
/* Stable indices into the machine's expander arrays, not creation order. */
#define IA64_460GX_ROOT_WXB0          0
#define IA64_460GX_ROOT_WXB1          1
#define IA64_460GX_ROOT_GXB           2
/* The AGP graphics adapter sits at device 0 of the GXB root bus. */
#define IA64_460GX_GXB_VGA_SLOT       0x00
/*
 * The i2000's SCSI host bus adapter sits at device 0 of the first WXB
 * expander bus.  That seat belongs to the QLogic ISP12160, the adapter the
 * real board carries and the machine's default (scsi=).
 */
#define IA64_460GX_WXB0_SCSI_SLOT     0x00
/*
 * The Intel 82468GX I/O and Firmware Bridge, the platform south bridge: a
 * four-function device carrying the LPC/ISA bridge, the IDE controller, the
 * UHCI host controller and the SMBus controller.  Device 3 is where the real
 * SDV firmware expects it -- it pokes the south bridge's config register
 * 0xd0 at 00:03.0 for its CPU-frequency mailbox (b18d80a).
 */
/*
 * The Programmable Interrupt Device's seat on the compatibility bus, and the
 * Integrated Hot-Plug Controller's on each WXB bus.  Unlike the south
 * bridge's device number, these are reconstructions: the chipset
 * documentation places the PID on the compatibility bus (SSDM 1.7.2) without
 * naming a device number, and nothing in the real firmware pins them down.
 * They follow upstream's i2000 machine.
 */
#define IA64_460GX_PID_SLOT           0x00
#define IA64_460GX_IHPC_SLOT          0x0f
#define IA64_460GX_IFB_SLOT           0x03
#define IA64_460GX_IFB_LPC_FUNCTION   0
#define IA64_460GX_IFB_IDE_FUNCTION   1
#define IA64_460GX_IFB_USB_FUNCTION   2
#define IA64_460GX_IFB_SMBUS_FUNCTION 3
/* The Cirrus Logic CS4281 on the i2000's I/O board. */
#define IA64_460GX_AUDIO_SLOT         0x04
#define IA64_MERCURY_VGA_SLOT        0x00
/*
 * zx1's storage seats: the SCSI adapter at device 1 of rope 1's bus, where
 * the rx2600 carries its 53C1030, and the opt-in AHCI at device 4 of PCI0.
 * The i2000 keeps the AHCI at device 1 of its compatibility bus.
 */
#define IA64_ZX1_SCSI_BUS             IA64_ZX1_ROPE1_BUS
#define IA64_ZX1_SCSI_SLOT            0x01
/* PCI0's core I/O: USB at device 1, IDE at 2 and the LAN at 3 (DEV-4). */
#define IA64_ZX1_USB_SLOT             0x01
#define IA64_ZX1_IDE_SLOT             0x02
#define IA64_ZX1_AHCI_SLOT            0x04
#define IA64_460GX_AHCI_SLOT          0x01
/* 16 MiB PAL/SAL firmware address space below 4 GiB. */
#define IA64_FW_ADDRESS_SPACE_BASE    IA64_U64(0x00000000ff000000)
#define IA64_FW_ADDRESS_SPACE_SIZE    IA64_U64(0x0000000001000000)
#define IA64_FW_ADDRESS_SPACE_END \
    (IA64_FW_ADDRESS_SPACE_BASE + IA64_FW_ADDRESS_SPACE_SIZE)
/*
 * Longs Peak (zx1 board) PDH devices below the flash.  The mio sends
 * FF00_0000-FFFF_FFFF to the Dillon ASIC over the PDH bus (mio ERS 2.1);
 * the HP firmware reaches these blocks from its first instructions (see
 * hw/ia64/longspeak_pdh.c for the addresses).  No Dillon ERS exists: the
 * block bounds are the ones the firmware's code shows, and only these blocks
 * decode.  The project firmware uses none of them.
 */
/*
 * One battery-backed 512 KB SRAM (zx2000 O&M 02 p.16), of which the vendor
 * firmware formats the first 256 KiB as its NVM: the SAL control block and
 * token store at the bottom, the EFI variable banks at FF43_8000 and
 * FF43_C000.  It puts its own stack and working tables in the rest.  What
 * decodes above the part is volatile.
 */
#define IA64_PDH_BBSRAM_BASE          IA64_U64(0x00000000ff400000)
#define IA64_PDH_BBSRAM_SIZE          IA64_U64(0x0000000000080000)
/*
 * Where the project firmware keeps its variable store inside the part: the
 * gap between what the vendor firmware formats as its SAL NVM (which ends at
 * FF41_93FF) and its EFI variable banks (which start at FF43_8000), so a
 * store file can carry both.
 */
#define IA64_PDH_STORE_VARS_OFFSET    IA64_U64(0x0000000000020000)
#define IA64_PDH_SRAM_BASE            IA64_U64(0x00000000ff480000)
#define IA64_PDH_SRAM_SIZE            IA64_U64(0x0000000000080000)
#define IA64_PDH_DEV5B_BASE           IA64_U64(0x00000000ff5b0000)
#define IA64_PDH_BMC_BT               0x00e4U   /* BT_CTRL, then data */
#define IA64_PDH_BMC_BT_BUFFER        64U
#define IA64_PDH_BMC_BT_RETRIES       2U
#define IA64_PDH_RTC                  0x8000U   /* the PDH clock */
#define IA64_PDH_RTC_REGS             0x0014U
#define IA64_PDH_RTC_RAM              256U
/*
 * The firmware's KCS1 (the SPMI one), KCS2 (the one the vendor DSDT gives the
 * OS as IPI0001) and KCS3: data, then status.  It counts BT and the three as
 * its BMC ports 1 to 4 (FFF446A0).
 */
#define IA64_PDH_BMC_KCS              0x0ca2U
#define IA64_PDH_BMC_KCS2             0x0000U
#define IA64_PDH_BMC_KCS3             0x0062U
/*
 * Get Device ID of the rx2600's BMC (rx2600 captures 2026-10-03 and
 * 2026-10-04, BMC-1): device 32h, revision 1 with device SDRs (bit 7),
 * firmware 1.53, IPMI 1.0, the version BCD with the major digit in bits 3:0
 * (IPMI v2.0 table 20-2), every additional device support bit up to the
 * event generator, manufacturer 0Bh (HP), product 8201h.
 */
#define IA64_PDH_BMC_DEVICE_ID        0x32U
#define IA64_PDH_BMC_DEVICE_REV       0x81U
#define IA64_PDH_BMC_FW_MAJOR         0x01U
#define IA64_PDH_BMC_FW_MINOR         0x53U
#define IA64_PDH_BMC_IPMI_VERSION     0x01U
#define IA64_PDH_BMC_DEVICE_SUPPORT   0x3fU
#define IA64_PDH_BMC_MANUFACTURER     0x00000bU
#define IA64_PDH_BMC_IPMI_PRODUCT     0x8201U
/* The board id the firmware picks its DIMM slot table with (FFF62880). */
#define IA64_PDH_BMC_PRODUCT_ID       257U
#define IA64_PDH_BMC_PRODUCT_ID_OFFSET 115U  /* in the FRU product area */
#define IA64_PDH_PRESENCE_BASE        IA64_U64(0x00000000ff5c0000)
/*
 * The vendor FADT reaches the ACPI PM block here, through its extended
 * (SystemMemory) fields and in a different order from the I/O block: the
 * timer first, then the PM1a event pair, then PM1a control.
 */
#define IA64_PDH_ACPI_PM_BASE         IA64_U64(0x00000000ff5c1000)
#define IA64_PDH_ACPI_PM_TMR          0x0004U
#define IA64_PDH_ACPI_PM1_EVT         0x0008U
#define IA64_PDH_ACPI_PM1_CNT         0x000cU
#define IA64_PDH_UART_BASE            IA64_U64(0x00000000ff5e0000)
#define IA64_PDH_UARTS                2U
#define IA64_PDH_UART_STRIDE          0x2000U
#define IA64_PDH_DILLON_BASE          IA64_U64(0x00000000ff5f0000)
#define IA64_PDH_BLOCK_SIZE           IA64_U64(0x0000000000010000)
/* Offsets in the presence block and in the Dillon register block. */
#define IA64_PDH_PRESENCE             0x0000U   /* bits 3:0, active low */
#define IA64_PDH_POST                 0x0018U
/*
 * The revisions of the board's two PDH parts, as "info chiprev" prints them
 * ("Other Bridge"): the Meson in the presence block reads 7 and the Dillon
 * reads 2, both with 16-bit loads (FFF449C0, FFF44940).  rx2600 capture
 * 2026-10-03, PDH-2 and PDH-5.
 */
#define IA64_PDH_MESON_REV            0x0020U
#define IA64_PDH_MESON_REV_VALUE      7U
#define IA64_PDH_DILLON_REV           0x2070U
#define IA64_PDH_DILLON_REV_VALUE     2U
#define IA64_PDH_DILLON_SCRATCH0      0x0020U   /* bits 7:6: boot mode */
#define IA64_PDH_DILLON_REGS          19U       /* 8-byte, 0x00-0x90 */
#define IA64_PDH_DILLON_STATUS        0x0028U   /* + 8 * processor index */
#define IA64_PDH_DILLON_STATUSES      4U
#define IA64_PDH_DILLON_CHECKIN       0x0068U   /* bits 19:16: check-in */
#define IA64_PDH_DILLON_MONARCH       0x0070U   /* bits 15:0: ~(LID >> 16) */
#define IA64_PDH_DILLON_SEMAPHORE     0x00b0U   /* + 8 * claimant id */
#define IA64_PDH_DILLON_SEMAPHORES    8U
#define IA64_PDH_DILLON_CONTROL       0x1000U   /* bits 3:1 carry a command */
#define IA64_PDH_DILLON_COMMAND       0x0eU     /* the command field */
#define IA64_PDH_DILLON_RESET         0x06U     /* the one that reboots */
#define IA64_PDH_DILLON_MODULE_LAYOUT 0x1010U   /* reads 0xFF, SAL_A uses 0 */
#define IA64_PDH_DILLON_SCRATCH1      0x1038U   /* written 0 and 2 */
#define IA64_PDH_DILLON_MISC          0x31c0U   /* last of the tested file */
/*
 * The flash's NVRAM sector, the EFI variable store.  The real i2000/SDV
 * flash keeps its NVRAM/variable scratch block at 0xFFF90000 (bios130.BIN
 * FIT entry 5, type 0x1E); the project firmware's image declares one 64 KiB
 * block there and programs it through the flash's command interface.
 */
#define IA64_NVRAM_BASE               IA64_U64(0x00000000fff90000)
#define IA64_NVRAM_SIZE               IA64_U64(0x0000000000010000)
/*
 * The zx1 machine's ACPI PM block in PCI I/O port space, and the SCI it
 * raises.  A stand-in until the zx1 firmware work shows the real block.
 */
/* Offsets inside the PM block, as the shared ACPI core lays it out. */
#define IA64_ACPI_PM1_EVT_OFFSET      0x0000U
#define IA64_ACPI_PM1_CNT_OFFSET      0x0004U
#define IA64_ACPI_PM_TMR_OFFSET       0x0008U
#define IA64_ACPI_PM_IO_BASE          0x00002000U
#define IA64_ACPI_PM_IO_SIZE          0x00000010U
#define IA64_ACPI_PM_RESET_OFFSET     0x0000000cU
#define IA64_ACPI_PM_RESET_VALUE      0x01U
#define IA64_ACPI_SCI_IRQ             9
/*
 * The 460GX board's ACPI block is the 82468GX IFB's, which its firmware
 * programs to A00h (00:03.0 @44h = 0, @40h = 0A00h, @44h = 1): PM1a_EVT
 * A00h, PM1a_CNT A04h, PM_TMR A08h, GPE0 A0Ch.  The vendor FADT names SMI_CMD
 * B2h with ACPI_ENABLE A0h / ACPI_DISABLE A1h, the reset register at CF9h
 * (RST_CNT, value 06h), and SCI_INT 9 (bios130.BIN FADT/MADT templates at
 * 0x1001D0 and 0x100070).  The template's source override takes ISA IRQ 9 to
 * GSI 49, active high, level -- the wiring of the part's own SCI pin, which
 * the board reaches only when SCIRC (45h) selects it over IRQ9.
 */
#define IA64_460GX_ACPI_PM_IO_BASE    0x00000a00U
#define IA64_460GX_ACPI_GPE0_OFFSET   0x0000000cU
#define IA64_460GX_ACPI_GPE0_LENGTH   4U
#define IA64_460GX_SMI_CMD_PORT       0x000000b2U
#define IA64_460GX_ACPI_ENABLE_CMD    0xa0U
#define IA64_460GX_ACPI_DISABLE_CMD   0xa1U
#define IA64_460GX_RESET_CONTROL_PORT 0x00000cf9U
#define IA64_460GX_RESET_CONTROL_VALUE 0x06U
#define IA64_460GX_SCI_GSI            49
/*
 * The IFB's SMI registers after the ACPI block (SSDM 11.2.8): Global Control
 * with SMI_EN, EOS and APMC_EN, and Global Status with APM_STS (write 1 to
 * clear).  Its SMI# is the processors' PMI pin, which SALE_PMI serves.
 */
#define IA64_460GX_ACPI_GLBCTL_OFFSET 0x0000001aU
#define IA64_460GX_ACPI_GLBSTS_OFFSET 0x0000001cU
#define IA64_460GX_GLBCTL_SMI_EN      0x0001U
#define IA64_460GX_GLBCTL_EOS         0x0008U
#define IA64_460GX_GLBCTL_APMC_EN     0x0400U
#define IA64_460GX_GLBSTS_APM_STS     0x0008U
/*
 * The board's Super I/O UARTs (LPC47B27x LDN 4 and 5): COM1 at 3F8h on ISA
 * IRQ 4 is the console, COM2 at 2F8h on IRQ 3 the debug port when one is
 * configured.  Both ports are what kdcom's fixed table expects.
 */
#define IA64_460GX_COM1_IO_BASE       0x000003f8U
#define IA64_460GX_COM1_IRQ           4
#define IA64_460GX_COM2_IO_BASE       0x000002f8U
#define IA64_460GX_COM2_IRQ           3
#define IA64_460GX_COM_IO_SIZE        8U

/*
 * The firmware defaults record: what a board's setup menu holds -- the
 * console policy, the IDE DMA policy, the boot-manager timeout and the
 * memory-map quirks.  It lives in the flash's NVRAM sector, which the
 * project firmware lays out as
 *
 *   0x0000-0x950F  the EFI variable store ("IVARSTOR")
 *   0xF000-0xF01F  the time zone record ("IRT64OFT"; the clock itself is
 *                  the board's: the CMOS RTC, or the PDH clock on zx1)
 *   0xF800-0xF82F  this record ("IA64DFLT")
 *
 * and the machine writes it from its options before the firmware runs, as
 * a factory programs a board's configuration.  A sector without it means
 * the firmware's own defaults.
 */
#define IA64_NVRAM_DEFAULTS_OFFSET    0xf800U
#define IA64_NVRAM_DEFAULTS_MAGIC     IA64_U64(0x544c464434364149) /* "IA64DFLT" */
#define IA64_NVRAM_DEFAULTS_VERSION   1ULL

#ifndef __ASSEMBLER__
typedef struct __attribute__((packed)) IA64NvramDefaults {
    unsigned long long Magic;
    unsigned long long Version;
    unsigned long long ConsolePolicy;     /* IA64_FW_CONSOLE_* */
    unsigned long long IdeDmaEnabled;
    unsigned long long BootTimeout;       /* seconds; 0xFFFF waits forever */
    unsigned long long MapQuirkDisable;   /* IA64_FW_QUIRK_* bits */
} IA64NvramDefaults;

_Static_assert(sizeof(IA64NvramDefaults) == 48,
               "the firmware defaults record is 48 bytes");
#endif /* __ASSEMBLER__ */

#endif /* HW_IA64_VPC_ABI_H */
