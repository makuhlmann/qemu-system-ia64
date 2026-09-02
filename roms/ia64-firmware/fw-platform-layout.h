/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Guest-visible platform layout: fixed BAR assignments, the low-RAM
 * loader-contract landmarks (32/48/64/80/128 MB), ACPI staging, the
 * firmware/RTC/NVRAM windows, and the EFI descriptor constants.  Single
 * source for firmware.c and efi_memmap.c.  The layout's real-hardware
 * fidelity status is documented per entry in
 * plans/firmware-rework-target-model.md and the rework plan.
 */

#ifndef IA64_FIRMWARE_FW_PLATFORM_LAYOUT_H
#define IA64_FIRMWARE_FW_PLATFORM_LAYOUT_H

#include "fw-base.h"
#include "hw/ia64/ia64_vpc_abi.h"

/* Owned by firmware.c; several layout macros are relative to it. */
extern UINT64 mCpuAssistBase;
/* Linker-defined image bounds; relocated with the image (phase 2.2). */
extern char __fw_image_start[];

#define PCI_OHCI_MMIO_BAR             (IA64_PCI_MMIO_BASE + 0x00010000ULL)
#define PCI_AHCI_MMIO_BAR             (IA64_PCI_MMIO_BASE + 0x00020000ULL)
/*
 * The SCSI host bus adapter lives on the first WXB expander root, so its BAR
 * comes out of that root's 32 MiB aperture (see dsdt-pci-root.asl).
 */
#define PCI_LSI_MMIO_BAR              (IA64_PCI_MMIO_BASE + 0x0c000000ULL)
#define PCI_VGA_FB_BAR                (IA64_PCI_MMIO_BASE + 0x02000000ULL)
#define PCI_VGA_MMIO_BAR              (IA64_PCI_MMIO_BASE + 0x07000000ULL)
#define PCI_VGA_ATI_ID                0x50461002U
#define PCI_VGA_STD_ID                0x11111234U
#define PCI_VGA_ATI_FB_SIZE           0x04000000ULL
#define PCI_VGA_STD_FB_SIZE           0x01000000ULL
#define VGA_FB_BASE                   ((UINT64)PCI_VGA_FB_BAR)
#define VGA_MMIO_BASE                 ((UINT64)PCI_VGA_MMIO_BAR)
/*
 * ACPI staging region: one FACS EfiACPIMemoryNVS page followed by the
 * reclaimable tables, 128 KiB total.  Its base is chosen at map-init time:
 * with the acpi-low-island quirk enabled (the historical, validated default)
 * it sits at the invented 8 MB low-RAM island; with the quirk disabled it
 * sits directly below the RAM-top CPU-assist region, adjacent to the rest of
 * the firmware reservation, the way real 460GX/E8870 firmware stages ACPI
 * tables (target-model doc sec 2; phase 2.2 of the rework plan).
 */
#define FW_ACPI_REGION_SIZE IA64_FW_ACPI_REGION_SIZE
#define FW_LOW_ACPI_ISLAND_BASE 0x0000000000800000ULL
extern UINT64 mAcpiRegionBase;
#define ACPI_RECLAIM_BASE (mAcpiRegionBase)
#define ACPI_RECLAIM_TABLE_BASE \
    (ACPI_RECLAIM_BASE + IA64_EFI_MEMORY_ALIGN)
#define ACPI_RECLAIM_END (ACPI_RECLAIM_BASE + FW_ACPI_REGION_SIZE)
/* 460GX/i2000 SDV SAPIC message block, just below the local SAPIC. */
#define IOSAPIC_BASE     IA64_IOSAPIC_BASE
#define IOSAPIC_SIZE     IA64_IOSAPIC_MMIO_SIZE
#define ACPI_PM_IO_BASE  IA64_ACPI_PM_IO_BASE
#define ACPI_PM1_EVT_OFFSET 0x0U
#define ACPI_PM1_CNT_OFFSET 0x4U
#define ACPI_PM_TMR_OFFSET 0x8U
#define ACPI_PM_RESET_OFFSET IA64_ACPI_PM_RESET_OFFSET
#define ACPI_PM_RESET_VALUE  IA64_ACPI_PM_RESET_VALUE
#define ACPI_PM1_CNT_SLEEP_ENABLE 0x2000U
#define ACPI_SCI_IRQ     ((UINT32)IA64_ACPI_SCI_IRQ)
/* MADT Flags: the platform carries a PC/AT-compatible legacy interrupt space. */
#define ACPI_MADT_FLAG_PCAT_COMPAT 0x1U
#define ACPI_GAS_SYSTEM_MEMORY 0U
#define ACPI_GAS_SYSTEM_IO     1U
#define ACPI_DBGP_INTERFACE_16550_FULL 0U
#define ACPI_FADT_FLAG_WBINVD        (1U << 0)
#define ACPI_FADT_FLAG_PWR_BUTTON    (1U << 4)
#define ACPI_FADT_FLAG_SLP_BUTTON    (1U << 5)
#define ACPI_FADT_FLAG_RESET_REG_SUP (1U << 10)
#define ACPI_FADT_FLAG_SW_CPU_SLP    (1U << 13)
#define VGA_MODE_TEXT_WIDTH  640U
#define VGA_MODE_TEXT_HEIGHT 400U
#define VGA_MODE_640_WIDTH   640U
#define VGA_MODE_640_HEIGHT  480U
#define VGA_MODE_800_WIDTH   800U
#define VGA_MODE_800_HEIGHT  600U
#define VGA_MODE_1024_WIDTH  1024U
#define VGA_MODE_1024_HEIGHT 768U
#define VGA_MODE_1280_WIDTH  1280U
#define VGA_MODE_1280_HEIGHT 1024U
#define VGA_BPP          32U
#define VGA_BAR_SIZE     (16U * 1024U * 1024U)
#define FW_POOL_ZERO_LIMIT (1U * 1024U * 1024U)
#define FW_LOW_RECLAIM_BASE 0x0000000000800000ULL
#define FW_LOW_FREE_BASE  0x0000000001100000ULL
#define FW_LOW_IMAGE_ALIGN 0x0000000002000000ULL
#define FW_LOW_IMAGE_BASE 0x0000000002000000ULL
#define FW_LOW_LEGACY_IMAGE_BASE 0x0000000003000000ULL
#define FW_LOW_IMAGE_ALIGNED_END (FW_LOW_IMAGE_BASE + FW_LOW_IMAGE_ALIGN)
#define FW_LOW_IMAGE_END  0x0000000005000000ULL
/*
 * FW_LOW_IMAGE_END (80 MB) is the Windows setup loader's TR-staging line.  It
 * is a conventional-memory DESCRIPTOR boundary (XP-era sumain.c:760 must not
 * see a descriptor straddling it), but no longer a reserved guard page: the
 * heap-carve bound that page provided is the job of the 32 MB split page, and
 * all RAM from 32 MB up to the RAM-top CPU-assist region is free so loaders
 * that map with large TRs (Server 2003 SP1 setupldr: one 64 MB page at
 * [64 MB, 128 MB)) find their region.  See efi_init_memory_map().
 */
/*
 * Firmware-owned physical stack + RSE backing store used while a virtual-
 * mode SAL_PROC call is re-entered physically (see sal_runtime_entry in
 * entry.S).  Lives in the firmware-permanent low RAM below the image base;
 * SAL calls are not re-entrant (SAL spec 3.1, the OS serializes them).
 */
/*
 * Per-CPU SAL_PROC physical re-entry slots in the CPU-assist area: the
 * lower half of each slot is the RSE backing store, the upper half the
 * memory stack.  The dispatch block publishes CPU 0's values; the
 * emulator-side bridge adds cpu_index * IA64_FW_SAL_RUNTIME_SLOT_SIZE.
 */
#define FW_SAL_PHYS_BSTORE_BASE (mCpuAssistBase + IA64_FW_SAL_RUNTIME_OFFSET)
#define FW_SAL_PHYS_STACK_TOP \
    (FW_SAL_PHYS_BSTORE_BASE + IA64_FW_SAL_RUNTIME_SLOT_SIZE - 0x10ULL)
#define FW_LOADER_STAGING_GUARD_SIZE 0x0000000000002000ULL
#define FW_LOW_RAM_STAGING_BASE (FW_LOW_IMAGE_END + FW_LOADER_STAGING_GUARD_SIZE)
/*
 * SAL-style reserved split page ending exactly at the loader's 48 MB image
 * base.  The Windows IA-64 setup loader requires that "any descriptor which
 * starts less than 48MB [must] not extend beyond 48MB" (efi/ia64/memory.c);
 * its own MempAllocDescriptor(_48MB,_80MB) split is erased when
 * BlInsertDescriptor re-merges adjacent MemoryFree runs, after which the
 * heap-extension (confined to selecting [16MB,48MB)-based free blocks but
 * carving from the block's END) escapes to just below 80MB - outside the
 * [16-64MB] the kernel's three loader-TRs cover -> NTOSKRNL bugcheck 0x1A.
 * EfiReservedMemoryType maps to MemoryFirmwarePermanent (EfiToArcType
 * default), which neither merges nor is reclaimed, so the [16-48MB) free run
 * stays bounded below 48MB while [48MB,80MB) remains a single free
 * descriptor for the loader's systemblock split.
 */
#define FW_LOADER_HEAP_SPLIT_SIZE 0x0000000000002000ULL
/*
 * The split page sits immediately below FW_LOW_IMAGE_BASE (32 MB) since
 * d30791f; this macro (and uefi_memory_map_selftest, written in terms of it)
 * tracks the map builder.
 */
#define FW_LOADER_HEAP_SPLIT_BASE \
    (FW_LOW_IMAGE_BASE - FW_LOADER_HEAP_SPLIT_SIZE)
#define FW_BOOT_STACK_SIZE     IA64_FW_BOOT_STACK_SIZE
#define IA64_EFI_MEMORY_ALIGN IA64_FW_LOW_RAM_ALIGN
#define IA64_EFI_MIN_STACK_BYTES   0x0000000000020000ULL
#define IA64_EFI_MIN_BACKING_BYTES 0x0000000000004000ULL
/*
 * 8 KiB reserved "SAL boot-structure" anchor at 128 MB (FW_LOW_ANCHOR_BASE).
 * XP-era kernels (2002/2462/2600) place their Phase-0 allocations and the
 * PFN database in the largest free descriptor below 256 MB; with low RAM one
 * unbroken run from the kernel image to the RAM top that descriptor starts
 * right behind the kernel, inside the loader's 16-80 MB TR window, and
 * MiInitMachineDependent then VHPT-faults writing the KSEG0 PTEs for it
 * (bugcheck 0x50, measured on installed XP 2600 UP/SMP and the XP 2002
 * installer).  A non-free page at 128 MB makes the descriptor above it the
 * largest low one again, exactly as the old [126 MB, 128 MB) CPU-assist
 * region did, while leaving [64 MB, 128 MB) free for Server 2003 SP1's 64 MB
 * kernel large page.  Below a 130 MB machine it falls back to the historical
 * 80 MB line.  Runtime images load above the anchor.
 *
 * The anchor and the Server 2003 loaders are mutually exclusive: the SP1
 * loader (setupldr/ia64ldr 5.2.3790.1830+) maps the kernel with a 64 MB large
 * page at [64 MB, 128 MB) and its heap with another at [128 MB, 192 MB), and
 * it derives the heap base from the first free descriptor at or above 128 MB
 * - with the anchor there it silently takes 0x8002000, maps it with a 64 MB
 * identity TR and every KSEG0 address is then off by one page (bugcheck 0xD1
 * on SharedUserData, measured).  The XP-era kernels need the anchor (see
 * efi_init_memory_map); the 2003 kernels (RTM measured both ways) do not.  So
 * the firmware's PE loader drops the anchor when the EFI application it is
 * about to start carries a VS_FIXEDFILEINFO file version 5.2.3790.0 (Server
 * 2003 RTM, whose rewritten Mm has no such descriptor-reset path and which
 * was measured fine without the anchor) or later
 * (pe_image_wants_contiguous_low_ram); 5.1.x loaders (XP, 2462) keep it.
 */
#define FW_LOW_ANCHOR_BASE       0x0000000008000000ULL
#define FW_LOW_ANCHOR_SIZE       0x0000000000002000ULL
#define FW_LOW_RUNTIME_IMAGE_BASE 0x0000000008010000ULL
/*
 * Low (sub-aperture) DRAM ends at the PCI/MMIO aperture: it runs contiguously
 * from 0 to here, matching real 460GX, and any RAM beyond it is remapped above
 * 4 GiB.  There is no sub-4 GiB DRAM island above the aperture.
 */
#define FW_LOW_RAM_LIMIT  IA64_PCI_MMIO_BASE
#define FW_HIGH_RAM_AFTER_PCI_BASE (IA64_PCI_MMIO_BASE + IA64_PCI_MMIO_SIZE)
/* Shared with the machine model via hw/ia64/ia64_vpc_abi.h. */
#define FW_LOCAL_SAPIC_BASE IA64_LOCAL_SAPIC_BASE
#define FW_LOCAL_SAPIC_SIZE IA64_LOCAL_SAPIC_SIZE
#define FW_FIRMWARE_ADDRESS_SPACE_BASE IA64_FW_ADDRESS_SPACE_BASE
#define FW_FIRMWARE_ADDRESS_SPACE_SIZE IA64_FW_ADDRESS_SPACE_SIZE
#define FW_FIRMWARE_ADDRESS_SPACE_END IA64_FW_ADDRESS_SPACE_END
#define FW_WATCHDOG_BASE IA64_WATCHDOG_BASE
#define FW_WATCHDOG_SIZE IA64_WATCHDOG_SIZE
#define FW_WATCHDOG_TIMEOUT_OFFSET IA64_WATCHDOG_TIMEOUT_OFFSET
#define FW_WATCHDOG_CODE_OFFSET    IA64_WATCHDOG_CODE_OFFSET
#define FW_NVRAM_BASE IA64_NVRAM_BASE
#define FW_NVRAM_SIZE IA64_NVRAM_SIZE
#define FW_NVRAM_RTC_OFFSET 0x000000000000f000ULL
#define FW_NVRAM_COMMIT_OFFSET IA64_NVRAM_COMMIT_OFFSET
#define FW_NVRAM_COMMIT_MAGIC IA64_NVRAM_COMMIT_MAGIC
#define FW_HIGH_RAM_RANGE_MAX 3U
#define FW_MEMORY_AFFINITY_MAX (1U + FW_HIGH_RAM_RANGE_MAX)
#define FW_AP_STACK_SIZE  IA64_FW_CPU_STACK_SIZE
#define FW_SYSTEM_TABLE_POINTER_ALIGN 0x0000000000400000ULL
#define FW_SYSTEM_TABLE_POINTER_SIZE  0x0000000000001000ULL
#define EFI_MEMORY_UC     0x0000000000000001ULL
#define EFI_MEMORY_WC     0x0000000000000002ULL
#define EFI_MEMORY_WT     0x0000000000000004ULL
#define EFI_MEMORY_WB     0x0000000000000008ULL
#define EFI_MEMORY_RUNTIME 0x8000000000000000ULL
#define EFI_MEMORY_DESCRIPTOR_VERSION 1U
#define EFI_OPTIONAL_PTR  0x0000000000000001ULL
#define FW_NANOSECONDS_PER_SECOND 1000000000ULL
#define FW_RTC_RESOLUTION_HZ 1U
#define FW_TIME_ACCURACY_1E6_PPM 50000000U

#define VGA_LEGACY_FB_BASE            0x00000000000a0000ULL
#define VGA_LEGACY_FB_SIZE            0x00020000ULL

#define PCI_IO_SIZE                   IA64_PCI_IO_SIZE
#define PCI_IO_SPARSE_SIZE            IA64_PCI_IO_SPARSE_SIZE
#define LEGACY_IO_BASE                IA64_PCI_IO_BASE
#define LEGACY_IO_LIMIT               (LEGACY_IO_BASE + PCI_IO_SIZE)
#define LEGACY_IO_SPARSE_LIMIT        (LEGACY_IO_BASE + PCI_IO_SPARSE_SIZE)
#define LEGACY_IO_SPARSE_END          (LEGACY_IO_SPARSE_LIMIT - 1)
/*
 * A zero ACPI translation offset selects IA-64 legacy I/O space zero.  The
 * EFI memory map supplies LEGACY_IO_BASE for that space; publishing the
 * two's-complement negative base creates a separate, invalid Linux I/O space.
 */
#define PCI_IO_TRANSLATION_OFFSET     0ULL
#define PCI_MMIO_END \
    (IA64_PCI_MMIO_BASE + IA64_PCI_MMIO_SIZE - 1U)
#define PCI_MMIO_TRANSLATION_OFFSET   0ULL
#define PCI_CONFIG_ECAM_BASE          IA64_PCI_CONFIG_BASE
#define PCI_CONFIG_ECAM_SIZE          IA64_PCI_CONFIG_SIZE

#define VGA_TEXT_FB_BASE              (VGA_LEGACY_FB_BASE + 0x18000ULL)
#define VGA_TEXT_COLUMNS              80U
#define VGA_TEXT_ROWS                 25U
#define VGA_TEXT_CELL_WIDTH           8U
#define VGA_TEXT_CELL_HEIGHT          16U
#define VGA_TEXT_GLYPH_WIDTH          5U
#define VGA_TEXT_GLYPH_HEIGHT         7U
#define VGA_TEXT_GLYPH_X              1U
#define VGA_TEXT_GLYPH_Y              1U
#define VGA_TEXT_GLYPH_SCALE_Y        2U

#endif /* IA64_FIRMWARE_FW_PLATFORM_LAYOUT_H */
