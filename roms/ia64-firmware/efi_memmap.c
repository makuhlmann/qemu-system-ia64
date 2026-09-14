/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * EFI memory-map construction and mutation.  Extracted verbatim from
 * firmware.c (Phase 1 of plans/firmware-rework-plan.md): the builder
 * (efi_init_memory_map + primitives), the coalescer with its preserved
 * loader-contract boundaries, the 128 MB anchor arming/release, and the
 * memory-map selftest.  Every guest-specific map workaround is
 * documented at its site; the reification into an explicit quirk table
 * is the next step of the plan.
 */

#include "fw-base.h"
#include "fw-services.h"
#include "fw-memmap.h"
#include "fw-platform-layout.h"
#include "linker-symbols.h"

#define MEMORY_MAP_MAX   128

EFI_MEMORY_DESCRIPTOR  mMemoryMap[MEMORY_MAP_MAX];
UINTN                  mMemoryMapEntries;
UINTN                  mMapKey = 1;
/* The anchor is a soft reservation: see efi_release_low_anchor(). */
BOOLEAN                  mLowAnchorArmed;

void efi_add_memory_range(UINTN *Index, EFI_MEMORY_TYPE Type,
                                 UINT64 Start, UINT64 End, UINT64 Attribute)
{
    EFI_MEMORY_DESCRIPTOR desc;
    UINTN pos;
    UINTN i;

    if (End <= Start || *Index >= MEMORY_MAP_MAX) {
        return;
    }

    desc.Type = Type;
    desc.Pad = 0;
    desc.PhysicalStart = Start;
    desc.VirtualStart = 0;
    desc.NumberOfPages = (End - Start) / 4096U;
    desc.Attribute = Attribute;

    pos = *Index;
    for (i = 0; i < *Index; i++) {
        if (Start < mMemoryMap[i].PhysicalStart) {
            pos = i;
            break;
        }
    }
    for (i = *Index; i > pos; i--) {
        mMemoryMap[i] = mMemoryMap[i - 1U];
    }
    mMemoryMap[pos] = desc;
    (*Index)++;
}

void efi_insert_memory_descriptor(UINTN Index,
                                         EFI_MEMORY_DESCRIPTOR Descriptor)
{
    UINTN i;

    if (mMemoryMapEntries >= MEMORY_MAP_MAX || Index > mMemoryMapEntries) {
        return;
    }

    for (i = mMemoryMapEntries; i > Index; i--) {
        mMemoryMap[i] = mMemoryMap[i - 1U];
    }
    mMemoryMap[Index] = Descriptor;
    mMemoryMapEntries++;
}

/*
 * Guest-specific map workarounds ("quirks"), each keyed to a named guest
 * bug at its emission site.  All default ON -- the validated map.  Each can
 * be disabled for A/B experiments with -machine ia64-vpc,fw-quirks=-<name>
 * (plans/firmware-rework-plan.md Phase 2 retires them one by one).
 */
BOOLEAN fw_map_quirk_enabled(UINT64 QuirkBit)
{
    return (fw_handoff_map_quirk_disable() & QuirkBit) == 0;
}

BOOLEAN efi_preserve_memory_map_boundary(UINT64 Boundary)
{
    if (!fw_map_quirk_enabled(IA64_FW_QUIRK_LOW_BOUNDARIES)) {
        return 0;
    }
    return Boundary == FW_LOW_IMAGE_BASE ||
           Boundary == FW_LOW_LEGACY_IMAGE_BASE ||
           Boundary == FW_LOW_IMAGE_ALIGNED_END ||
           Boundary == FW_LOW_IMAGE_END;
}

BOOLEAN efi_memory_descriptors_can_merge(EFI_MEMORY_DESCRIPTOR *A,
                                                EFI_MEMORY_DESCRIPTOR *B)
{
    UINT64 a_size;

    if (A->Type != B->Type || A->Attribute != B->Attribute) {
        return 0;
    }

    a_size = A->NumberOfPages << 12;
    if (A->PhysicalStart + a_size != B->PhysicalStart) {
        return 0;
    }
    if (efi_preserve_memory_map_boundary(B->PhysicalStart)) {
        return 0;
    }

    if (A->VirtualStart == 0 && B->VirtualStart == 0) {
        return 1;
    }
    return A->VirtualStart + a_size == B->VirtualStart;
}

void efi_coalesce_memory_map(void)
{
    UINTN i = 0;

    while (i + 1U < mMemoryMapEntries) {
        EFI_MEMORY_DESCRIPTOR *desc = &mMemoryMap[i];
        EFI_MEMORY_DESCRIPTOR *next = &mMemoryMap[i + 1U];

        if (efi_memory_descriptors_can_merge(desc, next)) {
            UINTN j;

            desc->NumberOfPages += next->NumberOfPages;
            for (j = i + 1U; j + 1U < mMemoryMapEntries; j++) {
                mMemoryMap[j] = mMemoryMap[j + 1U];
            }
            mMemoryMapEntries--;
            continue;
        }

        i++;
    }
}

BOOLEAN efi_mark_memory_range(EFI_MEMORY_TYPE Type, UINT64 Start,
                                     UINT64 End, UINT64 Attribute)
{
    EFI_MEMORY_DESCRIPTOR saved_map[MEMORY_MAP_MAX];
    UINTN saved_entries;
    UINTN saved_key;
    UINT64 current = Start & ~0xfffULL;
    UINT64 aligned_end;
    BOOLEAN changed = 0;

    if (efi_memory_descriptor_requires_ia64_alignment(Type, Attribute) &&
        ((Start % IA64_EFI_MEMORY_ALIGN) != 0 ||
         (End % IA64_EFI_MEMORY_ALIGN) != 0)) {
        return 0;
    }
    if (!efi_align_up_u64(End, EFI_PAGE_SIZE, &aligned_end) ||
        aligned_end <= current) {
        return 0;
    }

    saved_entries = mMemoryMapEntries;
    saved_key = mMapKey;
    fw_copy_mem(saved_map, mMemoryMap,
                saved_entries * sizeof(mMemoryMap[0]));

    while (current < aligned_end) {
        BOOLEAN advanced = 0;
        UINTN i;

        for (i = 0; i < mMemoryMapEntries; i++) {
            EFI_MEMORY_DESCRIPTOR *desc = &mMemoryMap[i];
            EFI_MEMORY_TYPE type = Type;
            UINT64 desc_start = desc->PhysicalStart;
            UINT64 desc_end = desc_start + (desc->NumberOfPages << 12);
            UINT64 mark_end = aligned_end < desc_end ? aligned_end : desc_end;
            EFI_MEMORY_DESCRIPTOR marked = *desc;
            EFI_MEMORY_DESCRIPTOR after = *desc;
            BOOLEAN has_before;
            BOOLEAN has_after;

            /* Every byte in the requested range must already be described. */
            if (current < desc_start || current >= desc_end) {
                continue;
            }

            if (desc->Type == type && desc->Attribute == Attribute) {
                current = mark_end;
                advanced = 1;
                break;
            }

            has_before = current > desc_start;
            has_after = mark_end < desc_end;

            marked.Type = type;
            marked.PhysicalStart = current;
            marked.VirtualStart = 0;
            marked.NumberOfPages = (mark_end - current) >> 12;
            marked.Attribute = Attribute;

            after.PhysicalStart = mark_end;
            after.VirtualStart = 0;
            after.NumberOfPages = (desc_end - mark_end) >> 12;

            if (has_before) {
                desc->NumberOfPages = (current - desc_start) >> 12;
                if (has_after) {
                    if (mMemoryMapEntries + 2U > MEMORY_MAP_MAX) {
                        goto rollback;
                    }
                    efi_insert_memory_descriptor(i + 1U, marked);
                    efi_insert_memory_descriptor(i + 2U, after);
                } else {
                    if (mMemoryMapEntries + 1U > MEMORY_MAP_MAX) {
                        goto rollback;
                    }
                    efi_insert_memory_descriptor(i + 1U, marked);
                }
            } else {
                *desc = marked;
                if (has_after) {
                    if (mMemoryMapEntries + 1U > MEMORY_MAP_MAX) {
                        goto rollback;
                    }
                    efi_insert_memory_descriptor(i + 1U, after);
                }
            }
            changed = 1;
            current = mark_end;
            advanced = 1;
            break;
        }

        if (!advanced) {
            goto rollback;
        }
    }

    if (changed) {
        efi_coalesce_memory_map();
        mMapKey++;
    }
    return 1;

rollback:
    fw_copy_mem(mMemoryMap, saved_map,
                saved_entries * sizeof(mMemoryMap[0]));
    mMemoryMapEntries = saved_entries;
    mMapKey = saved_key;
    return 0;
}

BOOLEAN efi_memory_map_has_descriptor(EFI_MEMORY_TYPE Type,
                                             UINT64 Start, UINT64 End,
                                             UINT64 Attribute)
{
    UINTN i;

    for (i = 0; i < mMemoryMapEntries; i++) {
        EFI_MEMORY_DESCRIPTOR *desc = &mMemoryMap[i];
        UINT64 desc_end = desc->PhysicalStart + (desc->NumberOfPages << 12);

        if (desc->Type == Type &&
            desc->PhysicalStart == Start &&
            desc_end == End &&
            desc->Attribute == Attribute) {
            return 1;
        }
    }
    return 0;
}

BOOLEAN efi_memory_map_is_sorted(void)
{
    UINTN i;

    for (i = 1; i < mMemoryMapEntries; i++) {
        if (mMemoryMap[i - 1U].PhysicalStart > mMemoryMap[i].PhysicalStart) {
            return 0;
        }
    }
    return 1;
}

BOOLEAN efi_memory_map_has_ia64_descriptor_alignment(void)
{
    UINTN i;

    for (i = 0; i < mMemoryMapEntries; i++) {
        EFI_MEMORY_DESCRIPTOR *desc = &mMemoryMap[i];
        UINT64 size = desc->NumberOfPages << 12;

        if (!efi_memory_descriptor_requires_ia64_alignment(
                desc->Type, desc->Attribute)) {
            continue;
        }
        if ((desc->PhysicalStart & (IA64_EFI_MEMORY_ALIGN - 1U)) != 0 ||
            (size & (IA64_EFI_MEMORY_ALIGN - 1U)) != 0) {
            return 0;
        }
    }
    return 1;
}

BOOLEAN efi_memory_map_covers_range(EFI_MEMORY_TYPE Type,
                                           UINT64 Start, UINT64 End,
                                           UINT64 Attribute)
{
    UINTN i;

    for (i = 0; i < mMemoryMapEntries; i++) {
        EFI_MEMORY_DESCRIPTOR *desc = &mMemoryMap[i];
        UINT64 desc_end = desc->PhysicalStart + (desc->NumberOfPages << 12);

        if (desc->Type == Type &&
            desc->PhysicalStart <= Start &&
            desc_end >= End &&
            desc->Attribute == Attribute) {
            return 1;
        }
    }
    return 0;
}

BOOLEAN efi_memory_map_has_boot_stack_layout(void)
{
    UINT64 pointer_start = mSystemTablePointerBase;
    UINT64 pointer_end = pointer_start + FW_SYSTEM_TABLE_POINTER_SIZE;

    if (!efi_memory_map_has_descriptor(
            EfiRuntimeServicesData,
            mCpuAssistBase, mCpuAssistBase + IA64_FW_CPU_ASSIST_SIZE,
            EFI_MEMORY_WB | EFI_MEMORY_RUNTIME) ||
        !efi_memory_map_covers_range(
            EfiRuntimeServicesData, mBootStackBase, mBootStackTop,
            EFI_MEMORY_WB | EFI_MEMORY_RUNTIME)) {
        return 0;
    }

    return pointer_start == 0 ||
           efi_memory_map_has_descriptor(EfiReservedMemoryType,
                                         pointer_start, pointer_end,
                                         EFI_MEMORY_WB);
}

void efi_add_conventional_with_system_pointer(UINTN *Index,
                                                     UINT64 Start,
                                                     UINT64 End)
{
    UINT64 pointer_start = mSystemTablePointerBase;
    UINT64 pointer_end = pointer_start + FW_SYSTEM_TABLE_POINTER_SIZE;

    if (Start >= End) {
        return;
    }
    if (pointer_start != 0 && pointer_start >= Start &&
        pointer_end <= End) {
        efi_add_memory_range(Index, EfiConventionalMemory,
                             Start, pointer_start, EFI_MEMORY_WB);
        efi_add_memory_range(Index, EfiReservedMemoryType,
                             pointer_start, pointer_end, EFI_MEMORY_WB);
        efi_add_memory_range(Index, EfiConventionalMemory,
                             pointer_end, End, EFI_MEMORY_WB);
    } else {
        efi_add_memory_range(Index, EfiConventionalMemory,
                             Start, End, EFI_MEMORY_WB);
    }
}

/*
 * Emit a conventional-memory band that, when the SBA IOVA hole is active
 * (fw_zx1_iova_hole_active(): zx1 with RAM past the aperture), skips the "safe
 * IOVA space" DRAM hole [IA64_SBA_IOVA_BASE, IA64_SBA_IOVA_END).  The RAM that
 * would sit there is shifted up past the window by the machine (and accounted
 * for by fw_init_guest_high_ram_ranges), so the enabled IOVA window overlaps no
 * DRAM; the hole is left an undescribed gap.  When the hole is inactive, or for
 * a band that does not fully span the window, this is just
 * efi_add_conventional_with_system_pointer().  In the hole-active regime the
 * low band always reaches the aperture, so it fully spans the window and this
 * split is exact.  Keep in lockstep with ia64_vpc_map_ram() in
 * hw/ia64/ia64_base.c.
 */
static void efi_add_low_ram_band(UINTN *Index, UINT64 Start, UINT64 End)
{
    if (fw_zx1_iova_hole_active() &&
        Start < IA64_SBA_IOVA_BASE && End > IA64_SBA_IOVA_END) {
        efi_add_conventional_with_system_pointer(Index, Start,
                                                 IA64_SBA_IOVA_BASE);
        efi_add_conventional_with_system_pointer(Index, IA64_SBA_IOVA_END, End);
    } else {
        efi_add_conventional_with_system_pointer(Index, Start, End);
    }
}

/* See FW_LOW_ANCHOR_BASE: 128 MB when it lies below the CPU-assist region. */
UINT64 fw_low_anchor_base(void)
{
    return mCpuAssistBase >= FW_LOW_ANCHOR_BASE + FW_LOW_ANCHOR_SIZE ?
           FW_LOW_ANCHOR_BASE : FW_LOW_IMAGE_END;
}

/*
 * The resident firmware image, shadowed at the top of low RAM.  Linux
 * discovers the PAL entry through an EfiPalCode memory descriptor and calls
 * it through a region 7 alias, so expose the actual PAL trampoline page
 * separately.  Keep the one-time entry path as boot-services code, then
 * expose the aligned runtime text and data as ONE runtime descriptor: IA-64
 * SAL enters with a GP supplied by the SAL system table, and the linked code
 * uses GP-relative references into rodata/data - a loader may assign
 * unrelated virtual bases to separate runtime descriptors, which would break
 * those references.
 */
static void efi_add_firmware_image(UINTN *Index)
{
    UINTN image_start = (UINTN)__fw_image_start;
    UINTN firmware_end = ((UINTN)&_end + 0x1FFFU) & ~0x1FFFULL;
    UINTN runtime_code_start = (UINTN)&__runtime_code_start;
    /*
     * Describe PAL code as a whole 8 KB OS page.  The Windows IA-64 loader
     * stores its descriptors in 4 KB units and, in InsertDescriptor
     * (WXPSP1 base/boot/efi/sumain.c), sets MustCoellesce whenever an
     * entry's 4 KB base is odd or the PREVIOUS entry's 4 KB page count is
     * odd -- and for MemoryFirmwarePermanent it then resolves that by
     * "stealing" a page from the prior (free) entry.  A 4 KB PAL descriptor
     * makes the run odd and perturbs every boundary after it.
     */
    UINTN pal_align = fw_map_quirk_enabled(IA64_FW_QUIRK_PAL_8K_PAGE) ?
                      0x1FFFULL : 0xFFFULL;
    UINTN pal_start = (UINTN)pal_proc_entry & ~pal_align;
    UINTN pal_end = pal_start + pal_align + 1U;

    if (pal_start >= image_start && pal_end <= firmware_end) {
        efi_add_memory_range(Index, EfiBootServicesCode, image_start,
                             pal_start, EFI_MEMORY_WB);
        efi_add_memory_range(Index, EfiPalCode, pal_start, pal_end,
                             EFI_MEMORY_WB);
        efi_add_memory_range(Index, EfiBootServicesCode, pal_end,
                             runtime_code_start, EFI_MEMORY_WB);
        efi_add_memory_range(Index, EfiRuntimeServicesCode,
                             runtime_code_start, firmware_end,
                             efi_memory_attribute(EfiRuntimeServicesCode,
                                                  EFI_MEMORY_WB));
    } else {
        efi_add_memory_range(Index, EfiRuntimeServicesCode, image_start,
                             firmware_end,
                             efi_memory_attribute(EfiRuntimeServicesCode,
                                                  EFI_MEMORY_WB));
    }
}

void efi_add_boot_stack_low_ram(UINTN *Index, UINT64 StartRam,
                                       UINT64 LowRamEnd)
{
    UINTN image_start = (UINTN)__fw_image_start;
    BOOLEAN image_low = image_start == 0x00100000ULL;
    UINTN image_end = ((UINTN)&_end + 0x1FFFU) & ~0x1FFFULL;
    BOOLEAN island = fw_map_quirk_enabled(IA64_FW_QUIRK_ACPI_LOW_ISLAND);
    UINT64 top_block = island ? mCpuAssistBase : mAcpiRegionBase;

    /*
     * Real IA-64 firmware carves its SAL/boot scratch from the top of
     * installed RAM and leaves the DRAM below it contiguous (460GX SDV and
     * E8870 SR870BH2 alike).  In the relocated layout the whole RAM-top
     * firmware reservation is one contiguous run ending exactly at the
     * low-RAM end: the shadowed image (descriptors emitted by
     * efi_add_firmware_image), its bss/headroom tail, the ACPI staging
     * region (unless the acpi-low-island quirk parks it at 8 MB), and the
     * CPU-assist region - SAL re-entry slots, debug contexts/stacks,
     * initial RSE backing stores and the boot memory stacks that SAL
     * reuses after ExitBootServices().
     */
    if (image_low) {
        efi_add_low_ram_band(Index, StartRam, top_block);
    } else {
        efi_add_low_ram_band(Index, StartRam, image_start);
        /* bss + span headroom up to the next block: boot-services scratch. */
        efi_add_memory_range(Index, EfiBootServicesData, image_end,
                             top_block, EFI_MEMORY_WB);
    }
    if (!island) {
        efi_add_memory_range(Index, EfiACPIMemoryNVS, ACPI_RECLAIM_BASE,
                             ACPI_RECLAIM_TABLE_BASE, EFI_MEMORY_WB);
        efi_add_memory_range(Index, EfiACPIReclaimMemory,
                             ACPI_RECLAIM_TABLE_BASE, ACPI_RECLAIM_END,
                             EFI_MEMORY_WB);
    }
    efi_add_memory_range(
        Index, EfiRuntimeServicesData,
        mCpuAssistBase, mCpuAssistBase + IA64_FW_CPU_ASSIST_SIZE,
        efi_memory_attribute(EfiRuntimeServicesData, EFI_MEMORY_WB));
    /* Any sub-alignment tail of installed RAM stays ordinary memory. */
    efi_add_conventional_with_system_pointer(
        Index, mCpuAssistBase + IA64_FW_CPU_ASSIST_SIZE, LowRamEnd);
}

void efi_init_memory_map(void)
{
    UINT64 ram_size = fw_guest_ram_size();
    UINT64 low_ram_end = fw_guest_low_ram_end();
    /*
     * The image executes either from its 1 MB link home (machine option
     * fw-relocate=off, and the microprogram batteries) or from the RAM-top
     * shadow; its own relocated start symbol says which.  In the relocated
     * layout the vacated low RAM is conventional from 1 MB.
     */
    UINTN image_start = (UINTN)__fw_image_start;
    BOOLEAN image_low = image_start == 0x00100000ULL;
    UINTN image_end = ((UINTN)&_end + 0x1FFFU) & ~0x1FFFULL;
    UINT64 low_conv_start = image_low ? image_end : 0x00100000ULL;
    UINTN index = 0;
    UINTN i;

    mGuestRamSize = ram_size;
    mGuestLowRamEnd = low_ram_end;
    fw_init_guest_high_ram_ranges(ram_size);
    /* See fw-platform-layout.h: island at 8 MB vs the RAM-top block. */
    mAcpiRegionBase = fw_map_quirk_enabled(IA64_FW_QUIRK_ACPI_LOW_ISLAND) ?
                      FW_LOW_ACPI_ISLAND_BASE :
                      mCpuAssistBase - FW_ACPI_REGION_SIZE;
    mSystemTablePointerBase =
        fw_system_table_pointer_base(low_ram_end, mBootStackBase,
                                     mBootStackTop);
    mSystemTablePointer =
        (EFI_SYSTEM_TABLE_POINTER *)(UINTN)mSystemTablePointerBase;
    efi_add_firmware_image(&index);

    /*
     * In the relocated layout the image no longer bounds the allocator;
     * the static 16 MB floor keeps boot-services allocations out of the
     * loaders' low staging area.
     */
    if (image_low && mNextPageAddr < image_end) {
        mNextPageAddr = image_end;
    }

    /*
     * The sub-1 MB compatibility area, published the way real 460GX/E8870
     * firmware does (target-model doc sec 1.3/2): the whole megabyte is
     * DRAM-capable, and only the VGA aperture is genuine MMIO.
     *
     * [0, 0x2000)       reserved WB DRAM: the IA-32 IVT/BDA (the INT10
     *                   vector at 0:0x40 is live - ia64_vpc_install_int10).
     *                   The firmware IVT moved into the image (.fw_ivt).
     * [0x2000, 0xA0000)  conventional WB, as on real hardware (SAL spec
     *                   Table 2-3: "0x500-0x9FFFF memory").  The allocator
     *                   only reaches below mNextPageAddr on its wrap pass,
     *                   so boot-services allocations do not land here.
     * [0xA0000, 0xC0000) UC MMIO while a VGA device decodes it (VGASE=1).
     * [0xC0000, 0x100000) the shadowed IA-32 option-ROM/system-BIOS DRAM,
     *                   firmware-owned.  EfiReservedMemoryType becomes
     *                   LoaderFirmwarePermanent (WXPSP1 memdesc.c:103),
     *                   which is exactly what the 3790 HAL's video-BIOS
     *                   scan needs over pages 0x60-0x67 (WSRV03
     *                   halia64/ia64/i64krnl.c:1212) and what keeps the
     *                   kernel from ever WB-mapping it, so videoprt's UC
     *                   MmMapIoSpace of the shadow has no WB alias to
     *                   collide with (iosup.c:7261 takes the I/O path for
     *                   non-PFN-database frames).  The attribute mask
     *                   advertises the full DRAM capability set per EFI
     *                   1.10 sec 3.2.3 (capabilities, not current setting).
     *
     * Descriptor-geometry warning: the sub-1 MB descriptor count changes
     * the loader-visible map, which Whistler 2462 is sensitive to -
     * revalidate 2462 whenever this block is touched.
     */
    efi_add_memory_range(&index, EfiReservedMemoryType, 0x00000000,
                         0x2000, EFI_MEMORY_WB);
    efi_add_memory_range(&index, EfiConventionalMemory, 0x2000,
                         VGA_LEGACY_FB_BASE, EFI_MEMORY_WB);
    efi_add_memory_range(&index, EfiMemoryMappedIO, VGA_LEGACY_FB_BASE,
                         VGA_LEGACY_FB_BASE + VGA_LEGACY_FB_SIZE,
                         EFI_MEMORY_UC);
    efi_add_memory_range(&index, EfiReservedMemoryType,
                         VGA_LEGACY_FB_BASE + VGA_LEGACY_FB_SIZE,
                         0x00100000,
                         EFI_MEMORY_UC | EFI_MEMORY_WC |
                         EFI_MEMORY_WT | EFI_MEMORY_WB);

    /*
     * IA-64 loaders commonly build page lists from EFI descriptors before
     * reserving image pages.  Expose the natural 32 MiB/64 MiB low-image
     * boundaries while also keeping the legacy 48 MiB/80 MiB staging bounds
     * visible as descriptor boundaries.
     */
    if (fw_map_quirk_enabled(IA64_FW_QUIRK_ACPI_LOW_ISLAND)) {
        efi_add_memory_range(&index, EfiConventionalMemory, low_conv_start,
                             FW_LOW_ACPI_ISLAND_BASE, EFI_MEMORY_WB);
        efi_add_memory_range(&index, EfiACPIMemoryNVS, ACPI_RECLAIM_BASE,
                             ACPI_RECLAIM_TABLE_BASE, EFI_MEMORY_WB);
        efi_add_memory_range(&index, EfiACPIReclaimMemory,
                             ACPI_RECLAIM_TABLE_BASE, ACPI_RECLAIM_END,
                             EFI_MEMORY_WB);
        efi_add_memory_range(&index, EfiConventionalMemory, ACPI_RECLAIM_END,
                             FW_LOW_FREE_BASE, EFI_MEMORY_WB);
    } else {
        /*
         * ACPI staging lives in the RAM-top firmware block (emitted by
         * efi_add_boot_stack_low_ram) and low RAM stays contiguous here,
         * as on real hardware.
         */
        efi_add_memory_range(&index, EfiConventionalMemory, low_conv_start,
                             FW_LOW_FREE_BASE, EFI_MEMORY_WB);
    }
    efi_add_memory_range(&index, EfiConventionalMemory, FW_LOW_FREE_BASE,
                         FW_LOADER_HEAP_SPLIT_BASE,
                         EFI_MEMORY_WB);
    /*
     * The non-free split page sits just below 32 MB, and [32MB,80MB) is left
     * as a single free run.
     *
     * blmemory.c:BlMemoryInitialize (WXPSP1 base/boot/lib/blmemory.c:341-370)
     * takes the FIRST free descriptor whose BasePage lies in [16MB,48MB) and
     * carves heap+stack from that descriptor's END, and only 16-80 MB is
     * TR-mapped.  Bounding the [17MB,..) run at 32 MB keeps that carve in
     * range, which is the job the 48 MB page was doing.
     *
     * Leaving [32MB,80MB) unsplit then gives the loader the "existing (much
     * larger) descriptor" its MempAllocDescriptor(_48MB,_80MB) systemblock
     * split expects (efi/ia64/memory.c:236-247); pre-splitting it at 48 MB
     * defeats that split, and the Whistler 2462 loader then loses the kernel
     * image so the kernel allocates page tables over itself.
     */
    efi_add_memory_range(&index,
                         fw_map_quirk_enabled(IA64_FW_QUIRK_LOADER_SPLIT_PAGE) ?
                         EfiReservedMemoryType : EfiConventionalMemory,
                         FW_LOADER_HEAP_SPLIT_BASE,
                         FW_LOW_IMAGE_BASE, EFI_MEMORY_WB);
    /*
     * [32 MB, CPU-assist base) is all conventional RAM, as on real 460GX /
     * E8870 platforms where low DRAM is contiguous up to the firmware's
     * RAM-top scratch.  The historical reserved guard PAGE at the 80 MB
     * staging line is gone (the setup loader's heap carve is bounded by the
     * 32 MB split page), but the 80 MB DESCRIPTOR boundary stays: the XP-era
     * sumain.c:760 (WXPSP1 base/boot/efi/sumain.c) turns the sub-80 MB part
     * of any conventional descriptor that straddles 80 MB into
     * MemoryFirmwareTemporary, after which the kernel carve finds no free
     * block ("ntoskrnl.exe is missing or corrupt" on an installed XP 2002 -
     * measured).  Two adjacent free descriptors cost the Server 2003 SP1
     * loader nothing: its ARC list merges adjacent MemoryFree runs, so the
     * 64 MB-aligned 64 MB large page it maps the kernel with at
     * [64 MB, 128 MB) still fits (previously ENOMEM, load error 16).
     * efi_preserve_memory_map_boundary() keeps the two from coalescing.
     */
    efi_add_memory_range(&index, EfiConventionalMemory, FW_LOW_IMAGE_BASE,
                         FW_LOW_IMAGE_END, EFI_MEMORY_WB);
    {
        UINT64 anchor = fw_low_anchor_base();

        BOOLEAN anchor_on = fw_map_quirk_enabled(IA64_FW_QUIRK_LOW_ANCHOR);

        efi_add_memory_range(&index, EfiConventionalMemory, FW_LOW_IMAGE_END,
                             anchor, EFI_MEMORY_WB);
        efi_add_memory_range(&index,
                             anchor_on ? EfiReservedMemoryType :
                                         EfiConventionalMemory,
                             anchor, anchor + FW_LOW_ANCHOR_SIZE,
                             EFI_MEMORY_WB);
        mLowAnchorArmed = anchor_on;
        efi_add_boot_stack_low_ram(&index, anchor + FW_LOW_ANCHOR_SIZE,
                                   low_ram_end);
    }

    /*
     * XP RTM (build 2600) SMP bring-up requires the 2 GiB firmware scratch
     * page to appear as a reserved descriptor: without it the two processors
     * deadlock spinning during kernel init (measured -- the IOSAPIC relocation
     * is harmless, this descriptor is what XP needs).  The page only fits as a
     * reserved hole while it lies above installed low RAM; once low DRAM runs
     * past 2 GiB (contiguous to the aperture, as real 460GX provides) the page
     * is ordinary WB DRAM and must not be carved out, so gate on the low-RAM
     * end.  Guests that run past 2 GiB here are the ones that specifically
     * need the unbroken contiguous DRAM (Linux), so this costs them nothing.
     */
    if (low_ram_end <= 0x80000000ULL &&
        fw_map_quirk_enabled(IA64_FW_QUIRK_SCRATCH_2G)) {
        efi_add_memory_range(&index, EfiReservedMemoryType, 0x80000000,
                             0x80100000, EFI_MEMORY_WB);
    }

    efi_add_memory_range(&index, EfiMemoryMappedIO, IOSAPIC_BASE,
                         IOSAPIC_BASE + IOSAPIC_SIZE, EFI_MEMORY_UC);

    for (i = 0; i < fw_guest_high_ram_count(); i++) {
        efi_add_memory_range(&index, EfiConventionalMemory,
                             fw_guest_high_ram_base(i), fw_guest_high_ram_end(i),
                             EFI_MEMORY_WB);
    }

    efi_add_memory_range(&index, EfiMemoryMappedIO, FW_LOCAL_SAPIC_BASE,
                         FW_LOCAL_SAPIC_BASE + FW_LOCAL_SAPIC_SIZE,
                         EFI_MEMORY_UC);

    /* IA-64 defines a single memory-mapped I/O port translation window. */
    efi_add_memory_range(&index, EfiMemoryMappedIOPortSpace, LEGACY_IO_BASE,
                         LEGACY_IO_SPARSE_LIMIT,
                         EFI_MEMORY_UC | EFI_MEMORY_RUNTIME);

    /*
     * The zx1 machine's segment-0 ECAM window, at the E8870 MMCFG home.  It
     * is described RUNTIME because the firmware's own SAL_PCI_CONFIG reaches
     * it through a SetVirtualAddressMap-relocated pointer
     * (mRuntimePciConfigEcam); the MCFG table steers the guest's own
     * mechanism.  The 460GX has only CF8/CFC, inside the I/O port space
     * above, and its guests use SAL_PCI_CONFIG as on real hardware.
     */
    if (!fw_pci_config_by_ports()) {
        efi_add_memory_range(&index, EfiMemoryMappedIO, PCI_CONFIG_ECAM_BASE,
                             PCI_CONFIG_ECAM_BASE + PCI_CONFIG_ECAM_SIZE,
                             EFI_MEMORY_UC | EFI_MEMORY_RUNTIME);
    }

    /* PCI host-bridge memory window, including VGA/AHCI/OHCI BAR space. */
    efi_add_memory_range(&index, EfiMemoryMappedIO, IA64_PCI_MMIO_BASE,
                         IA64_PCI_MMIO_BASE + IA64_PCI_MMIO_SIZE,
                         EFI_MEMORY_UC);

    /*
     * The 460GX chipset-specific area [4G-32M, 4G-20M) carries the GART SRAM
     * programming window at 0xFE200000, which the SSDM (248704-001 sec 7.1.2)
     * requires the processor to map UC.  We deliberately do NOT add an EFI
     * memory-map descriptor for it: Linux's i460-agp reaches the GATT through
     * ioremap(), which maps the physical window UC via the region-6 identity
     * area on its own, independent of the EFI map -- and adding a descriptor
     * here perturbs the descriptor layout the XP build-2600 SMP loader is
     * exquisitely sensitive to (see plans/status.md 2.2 and the
     * platform-map-460gx-realign notes), deadlocking that guest at kernel
     * bring-up.  The GART window is left an undescribed chipset gap, exactly
     * as it was before AGP support.
     */

    /* The RTC is a CMOS device at legacy ports now (rework D8). */
    efi_add_memory_range(&index, EfiMemoryMappedIO,
                         FW_FIRMWARE_ADDRESS_SPACE_BASE,
                         FW_NVRAM_BASE, EFI_MEMORY_UC);
    efi_add_memory_range(&index, EfiMemoryMappedIO,
                         FW_NVRAM_BASE, FW_NVRAM_BASE + FW_NVRAM_SIZE,
                         EFI_MEMORY_UC | EFI_MEMORY_RUNTIME);
    efi_add_memory_range(&index, EfiMemoryMappedIO,
                         FW_NVRAM_BASE + FW_NVRAM_SIZE,
                         FW_FIRMWARE_ADDRESS_SPACE_END, EFI_MEMORY_UC);

    /*
     * The zx1 machine's memory-mapped UART pages (HCDP and DBGP).  The 460GX
     * board's COM ports live in legacy I/O space, described above.
     */
    if (!fw_platform_is_460gx()) {
        efi_add_memory_range(&index, EfiMemoryMappedIO, IA64_UART_BASE,
                             IA64_UART_BASE + IA64_UART_MMIO_SIZE,
                             EFI_MEMORY_UC);
    }

    mMemoryMapEntries = index;
}


/* See FW_LOW_ANCHOR_BASE: give the anchor page back to conventional memory. */
void efi_release_low_anchor(void)
{
    UINT64 anchor = fw_low_anchor_base();

    if (!mLowAnchorArmed) {
        return;
    }
    if (efi_mark_memory_range(EfiConventionalMemory, anchor,
                              anchor + FW_LOW_ANCHOR_SIZE, EFI_MEMORY_WB)) {
        mLowAnchorArmed = 0;
        efi_coalesce_memory_map();
    }
}
