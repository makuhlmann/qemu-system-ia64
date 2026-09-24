/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * PE32+ image loader: base selection, IA-64 IMM64/DIR64 relocation,
 * memory-type marking, HII resource extraction, runtime-image virtual
 * relocation, and the VS_FIXEDFILEINFO loader-version probe behind the
 * anchor-version-sniff quirk.
 */

#include "fw-base.h"
#include "fw-services.h"
#include "fw-memmap.h"
#include "fw-platform-layout.h"
#include "fw-pe.h"

/* --- PE32+ image loader ---------------------------------------------------- */
/* The PE/COFF specification for IA-64 uses machine type 0x200.
   The entry point of an IA-64 EFI image is a plabel (function descriptor)
   containing the function address and gp value. */

UINT64 mNextPeImageBase = IA64_EFI_IMAGE_FALLBACK_BASE;


UINT64 pe_loaded_image_allocation_size(UINTN ImageSize,
                                              EFI_MEMORY_TYPE CodeType)
{
    UINT64 size;

    if (!efi_align_up_u64(ImageSize,
                          efi_memory_type_allocation_granularity(CodeType),
                          &size)) {
        return 0;
    }
    return size;
}

void pe_release_loaded_image_memory(VOID *ImageBase, UINTN ImageSize,
                                           EFI_MEMORY_TYPE CodeType)
{
    UINT64 base = (UINTN)ImageBase;
    UINT64 size;

    if (ImageBase == NULL || ImageSize == 0) {
        return;
    }
    size = pe_loaded_image_allocation_size(ImageSize, CodeType);
    if (size == 0 || base + size < base) {
        return;
    }
    (void)efi_mark_memory_range(EfiConventionalMemory, base, base + size,
                                EFI_MEMORY_WB);
}

void pe_discard_loaded_image_result(PE_LOADED_IMAGE_RESULT *Result)
{
    EFI_MEMORY_TYPE code_type;
    EFI_MEMORY_TYPE data_type;

    if (Result->runtime_relocation_log != NULL) {
        (void)bs_free_pool(Result->runtime_relocation_log);
    }
    if (Result->base != NULL && Result->size != 0) {
        pe_image_memory_types(Result->subsystem, &code_type, &data_type);
        pe_release_loaded_image_memory(Result->base, Result->size, code_type);
    }
    fw_set_mem(Result, sizeof(*Result), 0);
}

static BOOLEAN pe_image_base_in_use(UINT64 base, UINT64 size)
{
    UINTN i;

    for (i = 0; i < LOADED_IMAGE_MAX; i++) {
        EFI_LOADED_IMAGE_RECORD *rec = &mLoadedImages[i];
        UINT64 image_size;

        if (!rec->in_use) {
            continue;
        }
        image_size = pe_loaded_image_allocation_size(
            rec->loaded_image.ImageSize, rec->loaded_image.ImageCodeType);
        if (ranges_overlap(base, size,
                           (UINT64)(UINTN)rec->loaded_image.ImageBase,
                           image_size)) {
            return 1;
        }
    }
    return 0;
}

static BOOLEAN pe_image_base_is_conventional(UINT64 base, UINT64 size)
{
    UINTN i;

    if (size == 0 || base + size < base) {
        return 0;
    }

    for (i = 0; i < mMemoryMapEntries; i++) {
        EFI_MEMORY_DESCRIPTOR *desc = &mMemoryMap[i];
        UINT64 desc_start;
        UINT64 desc_end;

        if (desc->Type != EfiConventionalMemory) {
            continue;
        }
        desc_start = desc->PhysicalStart;
        desc_end = desc_start + (desc->NumberOfPages << 12);
        if (desc_end < desc_start) {
            continue;
        }
        if (base >= desc_start && base + size <= desc_end) {
            return 1;
        }
    }
    return 0;
}

static UINT64 pe_image_allocation_floor(BOOLEAN RuntimeImage)
{
    /*
     * Some IA-64 loaders use the low image window as a descriptor-aligned
     * staging area.  Runtime images receive virtual-address-change callbacks
     * after the loader has consumed early low-memory mappings, so place them
     * in the ordinary low-RAM region above those loader-owned windows.
     */
    return RuntimeImage ?
        IA64_EFI_RUNTIME_IMAGE_FALLBACK_BASE :
        IA64_EFI_IMAGE_FALLBACK_BASE;
}

/*
 * Alignment a loaded image must satisfy for its memory-map descriptors.
 * This is the granularity pe_loaded_image_allocation_size() rounds to, so
 * the range checked here is exactly the range the image goes on to occupy.
 */
static UINT64 pe_image_base_alignment(BOOLEAN RuntimeImage)
{
    return efi_memory_type_allocation_granularity(
        RuntimeImage ? EfiRuntimeServicesCode : EfiLoaderCode);
}

/* Whether an image of Size bytes physically fits at base, ignoring policy. */
static BOOLEAN pe_find_loaded_image_overlap(UINT64 Start, UINT64 End,
                                            UINT64 *FirstEnd,
                                            UINT64 *LastStart)
{
    UINT64 first_end = ~0ULL;
    UINT64 last_start = 0;
    BOOLEAN found = 0;
    UINTN i;

    if (End <= Start) {
        return 0;
    }

    for (i = 0; i < LOADED_IMAGE_MAX; i++) {
        EFI_LOADED_IMAGE_RECORD *rec = &mLoadedImages[i];
        UINT64 base;
        UINT64 size;
        UINT64 end;

        if (!rec->in_use) {
            continue;
        }
        base = (UINT64)(UINTN)rec->loaded_image.ImageBase;
        size = pe_loaded_image_allocation_size(
            rec->loaded_image.ImageSize, rec->loaded_image.ImageCodeType);
        if (size == 0 || base > ~0ULL - size) {
            continue;
        }
        end = base + size;
        if (Start >= end || base >= End) {
            continue;
        }
        if (!found || end < first_end) {
            first_end = end;
        }
        if (!found || base > last_start) {
            last_start = base;
        }
        found = 1;
    }

    if (found) {
        if (FirstEnd != NULL) {
            *FirstEnd = first_end;
        }
        if (LastStart != NULL) {
            *LastStart = last_start;
        }
    }
    return found;
}

/*
 * First fit inside one conventional descriptor, jumping straight past the
 * allocation, loaded-image or source-buffer blocker instead of advancing
 * one IA64_EFI_IMAGE_ALIGN step at a time.
 */
static BOOLEAN pe_find_image_base_forward(UINT64 Start, UINT64 End,
                                          UINT64 Size,
                                          UINT64 SourceBase,
                                          UINT64 SourceSize,
                                          UINT64 *ImageBase)
{
    UINT64 base;
    UINT64 source_end;

    if (Size == 0 || End <= Start || End - Start < Size ||
        !efi_align_up_u64(Start, IA64_EFI_IMAGE_ALIGN, &base)) {
        return 0;
    }
    source_end = SourceSize > ~0ULL - SourceBase ?
                 ~0ULL : SourceBase + SourceSize;

    while (base <= End - Size) {
        UINT64 blocker_end = 0;
        UINT64 overlap_end;
        BOOLEAN blocked = 0;

        if (SourceSize != 0 && base < source_end &&
            SourceBase < base + Size) {
            blocker_end = source_end;
            blocked = 1;
        }
        if (efi_find_allocation_overlap(base, base + Size, &overlap_end,
                                        NULL) &&
            (!blocked || overlap_end < blocker_end)) {
            blocker_end = overlap_end;
            blocked = 1;
        }
        if (pe_find_loaded_image_overlap(base, base + Size, &overlap_end,
                                         NULL) &&
            (!blocked || overlap_end < blocker_end)) {
            blocker_end = overlap_end;
            blocked = 1;
        }
        if (!blocked) {
            *ImageBase = base;
            return 1;
        }
        if (blocker_end <= base ||
            !efi_align_up_u64(blocker_end, IA64_EFI_IMAGE_ALIGN, &base)) {
            return 0;
        }
    }
    return 0;
}

static BOOLEAN pe_image_base_usable(UINT64 base, UINT64 size, UINT64 alignment)
{
    if (alignment == 0 || (base & (alignment - 1U)) != 0) {
        return 0;
    }

    return pe_image_base_is_conventional(base, size) &&
           !efi_find_allocation_overlap(base, base + size, NULL, NULL) &&
           !pe_image_base_in_use(base, size);
}

static BOOLEAN pe_image_base_available(UINT64 base, UINT64 size,
                                       BOOLEAN RuntimeImage)
{
    /*
     * The allocation floor is placement policy for addresses the firmware
     * picks itself; it must not veto an address the image asks for.
     */
    if (base < pe_image_allocation_floor(RuntimeImage)) {
        return 0;
    }

    return pe_image_base_usable(base, size, IA64_EFI_IMAGE_ALIGN);
}

/*
 * FixedBase marks an image whose relocations were stripped: it runs at its
 * linked base or not at all, so the staging floor - which is only placement
 * policy for addresses the firmware picks itself - must not veto it.
 * SourceBase/SourceSize describe the raw image being loaded from.  Sections
 * are copied out of it, so the loaded copy must not alias it; LoadImage()
 * takes any caller buffer, including memory no allocation record covers.
 */
UINT64 pe_choose_image_base(UINT64 preferred_base, UINT64 size,
                                   BOOLEAN RuntimeImage, BOOLEAN FixedBase,
                                   UINT64 SourceBase, UINT64 SourceSize)
{
    UINT64 base;
    UINT64 aligned_size;
    UINT64 fixed_size;
    UINT64 fixed_align = pe_image_base_alignment(RuntimeImage);
    UINT64 floor = pe_image_allocation_floor(RuntimeImage);
    UINT64 cursor = 0;
    BOOLEAN cursor_valid;
    unsigned pass;

    if (size == 0 ||
        !efi_align_up_u64(size, IA64_EFI_IMAGE_ALIGN, &aligned_size)) {
        return 0;
    }

    if (FixedBase) {
        if (preferred_base == 0 ||
            !efi_align_up_u64(size, fixed_align, &fixed_size) ||
            ranges_overlap(preferred_base, fixed_size,
                           SourceBase, SourceSize) ||
            !pe_image_base_usable(preferred_base, fixed_size, fixed_align)) {
            return 0;
        }
        return preferred_base;
    }

    if (preferred_base != 0 &&
        !ranges_overlap(preferred_base, aligned_size,
                        SourceBase, SourceSize) &&
        pe_image_base_available(preferred_base, aligned_size,
                                RuntimeImage)) {
        return preferred_base;
    }

    cursor_valid = efi_align_up_u64(mNextPeImageBase,
                                    IA64_EFI_IMAGE_ALIGN, &cursor);
    for (pass = 0; pass < 2; pass++) {
        UINTN i;

        for (i = 0; i < mMemoryMapEntries; i++) {
            EFI_MEMORY_DESCRIPTOR *desc = &mMemoryMap[i];
            UINT64 desc_start;
            UINT64 desc_end;

            if (desc->Type != EfiConventionalMemory ||
                !efi_align_up_u64(desc->PhysicalStart,
                                  IA64_EFI_IMAGE_ALIGN, &desc_start)) {
                continue;
            }

            desc_end = desc->PhysicalStart +
                       (desc->NumberOfPages << 12);
            if (desc_end <= desc->PhysicalStart) {
                continue;
            }
            if (desc_start < floor) {
                if (!efi_align_up_u64(floor, IA64_EFI_IMAGE_ALIGN,
                                      &desc_start)) {
                    continue;
                }
            }
            if (pass == 0) {
                if (!cursor_valid) {
                    continue;
                }
                if (desc_start < cursor) {
                    desc_start = cursor;
                }
            } else if (cursor_valid && desc_start >= cursor) {
                continue;
            }
            if (desc_start >= desc_end ||
                desc_end - desc_start < aligned_size) {
                continue;
            }

            /*
             * The forward search returns the lowest usable base in the
             * descriptor at or above desc_start; if that already sits at
             * or past the cursor on the wrap-around pass, no base below
             * the cursor exists in this descriptor either.
             */
            if (pe_find_image_base_forward(desc_start, desc_end,
                                           aligned_size,
                                           SourceBase, SourceSize,
                                           &base)) {
                if (pass != 0 && cursor_valid && base >= cursor) {
                    continue;
                }
                mNextPeImageBase = base + aligned_size;
                return base;
            }
        }
    }

    return 0;
}

static UINT64 pe_ia64_bundle_slot(UINT64 low, UINT64 high, UINTN slot)
{
    switch (slot) {
    case 0:
        return (low >> 5) & IA64_SLOT_MASK;
    case 1:
        return ((low >> 46) | (high << 18)) & IA64_SLOT_MASK;
    case 2:
        return (high >> 23) & IA64_SLOT_MASK;
    default:
        return 0;
    }
}

void pe_ia64_store_bundle(UINT64 *bundle,
                                 UINT64 template,
                                 UINT64 slot0,
                                 UINT64 slot1,
                                 UINT64 slot2)
{
    bundle[0] = (template & IA64_BUNDLE_TEMPLATE_MASK) |
                ((slot0 & IA64_SLOT_MASK) << 5) |
                ((slot1 & ((1ULL << 18) - 1ULL)) << 46);
    bundle[1] = ((slot1 >> 18) & ((1ULL << 23) - 1ULL)) |
                ((slot2 & IA64_SLOT_MASK) << 23);
}

static UINT64 pe_ia64_movl_imm64(UINT64 l_slot, UINT64 x_slot)
{
    UINT64 i = (x_slot >> 36) & 1ULL;
    UINT64 imm9d = (x_slot >> 27) & 0x1FFULL;
    UINT64 imm5c = (x_slot >> 22) & 0x1FULL;
    UINT64 ic = (x_slot >> 21) & 1ULL;
    UINT64 imm7b = (x_slot >> 13) & 0x7FULL;
    UINT64 imm41 = l_slot & IA64_SLOT_MASK;

    return imm7b |
           (imm9d << 7) |
           (imm5c << 16) |
           (ic << 21) |
           (imm41 << 22) |
           (i << 63);
}

UINT64 pe_ia64_movl_set_imm64(UINT64 x_slot, UINT64 imm64)
{
    x_slot &= ~((1ULL << 36) | (0x1FFULL << 27) |
                (0x1FULL << 22) | (1ULL << 21) |
                (0x7FULL << 13));
    x_slot |= ((imm64 >> 63) & 1ULL) << 36;
    x_slot |= ((imm64 >> 7) & 0x1FFULL) << 27;
    x_slot |= ((imm64 >> 16) & 0x1FULL) << 22;
    x_slot |= ((imm64 >> 21) & 1ULL) << 21;
    x_slot |= (imm64 & 0x7FULL) << 13;
    return x_slot;
}

BOOLEAN pe_read_ia64_imm64_reloc(UINT8 *reloc_addr, UINT64 *Imm64)
{
    UINT64 *bundle = (UINT64 *)((UINTN)reloc_addr & ~(UINTN)0xFULL);
    UINT64 low = bundle[0];
    UINT64 high = bundle[1];
    UINT64 template = low & IA64_BUNDLE_TEMPLATE_MASK;
    UINT64 slot1 = pe_ia64_bundle_slot(low, high, 1);
    UINT64 slot2 = pe_ia64_bundle_slot(low, high, 2);

    if ((template != 4 && template != 5) ||
        ((slot2 >> 37) & 0xFULL) != 6) {
        return 0;
    }

    *Imm64 = pe_ia64_movl_imm64(slot1, slot2);
    return 1;
}

static BOOLEAN pe_write_ia64_imm64_reloc(UINT8 *reloc_addr, UINT64 Imm64)
{
    UINT64 *bundle = (UINT64 *)((UINTN)reloc_addr & ~(UINTN)0xFULL);
    UINT64 low = bundle[0];
    UINT64 high = bundle[1];
    UINT64 template = low & IA64_BUNDLE_TEMPLATE_MASK;
    UINT64 slot0 = pe_ia64_bundle_slot(low, high, 0);
    UINT64 slot1 = pe_ia64_bundle_slot(low, high, 1);
    UINT64 slot2 = pe_ia64_bundle_slot(low, high, 2);

    if ((template != 4 && template != 5) ||
        ((slot2 >> 37) & 0xFULL) != 6) {
        return 0;
    }

    slot1 = (Imm64 >> 22) & IA64_SLOT_MASK;
    slot2 = pe_ia64_movl_set_imm64(slot2, Imm64);
    pe_ia64_store_bundle(bundle, template, slot0, slot1, slot2);
    return 1;
}

void pe_image_memory_types(UINT16 Subsystem,
                                  EFI_MEMORY_TYPE *CodeType,
                                  EFI_MEMORY_TYPE *DataType)
{
    switch (Subsystem) {
    case IMAGE_SUBSYSTEM_EFI_BOOT_SERVICE_DRIVER:
        *CodeType = EfiBootServicesCode;
        *DataType = EfiBootServicesData;
        break;
    case IMAGE_SUBSYSTEM_EFI_RUNTIME_DRIVER:
        *CodeType = EfiRuntimeServicesCode;
        *DataType = EfiRuntimeServicesData;
        break;
    case IMAGE_SUBSYSTEM_EFI_APPLICATION:
    default:
        *CodeType = EfiLoaderCode;
        *DataType = EfiLoaderData;
        break;
    }
}

static BOOLEAN pe_section_is_code(const IMAGE_SECTION_HEADER *Section)
{
    UINT32 characteristics = Section->Characteristics;

    return (characteristics &
            (IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE)) != 0;
}

static BOOLEAN pe_section_is_data(const IMAGE_SECTION_HEADER *Section)
{
    if (pe_section_is_code(Section)) {
        return 0;
    }
    return 1;
}

static UINT64 pe_section_memory_size(const IMAGE_SECTION_HEADER *Section)
{
    UINT64 size = Section->VirtualSize;

    if (size < Section->SizeOfRawData) {
        size = Section->SizeOfRawData;
    }
    return size;
}

static BOOLEAN pe_mark_image_section(UINT64 ImageBase, UINT64 ImageEnd,
                                     const IMAGE_SECTION_HEADER *Section,
                                     EFI_MEMORY_TYPE Type)
{
    UINT64 section_size = pe_section_memory_size(Section);
    UINT64 section_start;
    UINT64 section_end;

    if (section_size == 0 || Section->VirtualAddress >= ImageEnd - ImageBase) {
        return 1;
    }
    section_start = ImageBase + Section->VirtualAddress;
    section_end = section_start + section_size;
    if (section_end < section_start) {
        return 0;
    }
    if (section_end > ImageEnd) {
        section_end = ImageEnd;
    }
    return efi_mark_memory_range(Type, section_start, section_end,
                                 efi_memory_attribute(Type, EFI_MEMORY_WB));
}

BOOLEAN pe_mark_loaded_image_memory(UINT64 ImageBase,
                                           UINT32 SizeOfImage,
                                           const IMAGE_SECTION_HEADER *Sections,
                                           UINT16 NumberOfSections,
                                           EFI_MEMORY_TYPE CodeType,
                                           EFI_MEMORY_TYPE DataType)
{
    UINT64 image_end = ImageBase + SizeOfImage;
    UINT64 allocation_size;
    UINT16 i;

    if (SizeOfImage == 0 || image_end < ImageBase) {
        return 0;
    }

    allocation_size = pe_loaded_image_allocation_size(SizeOfImage, CodeType);
    if (allocation_size == 0 || ImageBase + allocation_size < ImageBase) {
        return 0;
    }

    /*
     * IA-64 runtime descriptors must be 8 KB aligned and sized.  Runtime
     * image sections may still meet on 4 KB boundaries, so exposing each
     * section as a separate memory-map descriptor can violate that rule.
     * Allocate the rounded image range as runtime code, as the EFI reference
     * page allocator does; ImageDataType still describes the driver's data
     * allocations.
     */
    if (CodeType == EfiRuntimeServicesCode ||
        DataType == EfiRuntimeServicesData) {
        if ((ImageBase & (IA64_EFI_MEMORY_ALIGN - 1U)) != 0) {
            return 0;
        }
        return efi_mark_memory_range(
            CodeType, ImageBase, ImageBase + allocation_size,
            efi_memory_attribute(CodeType, EFI_MEMORY_WB));
    }

    /*
     * UEFI requires the code and data portions of loaded images to use their
     * corresponding memory types.  Start with data so headers, BSS-only
     * holes, plabels, and mixed pages stay writable; then mark executable
     * sections as code and restore data sections as data.
     */
    if (!efi_mark_memory_range(DataType, ImageBase, image_end,
                               efi_memory_attribute(DataType,
                                                    EFI_MEMORY_WB))) {
        return 0;
    }
    for (i = 0; i < NumberOfSections; i++) {
        if (pe_section_is_code(&Sections[i]) &&
            !pe_mark_image_section(ImageBase, image_end, &Sections[i],
                                   CodeType)) {
            return 0;
        }
    }
    for (i = 0; i < NumberOfSections; i++) {
        if (pe_section_is_data(&Sections[i]) &&
            !pe_mark_image_section(ImageBase, image_end, &Sections[i],
                                   DataType)) {
            return 0;
        }
    }
    return 1;
}

/*
 * Windows loaders 5.2.3790.1000+ (Server 2003 SP1/SP2/R2) map the kernel and
 * their heap with 64 MB large pages at [64 MB, 192 MB) and need that span to
 * be one free run, and no 5.2 kernel needs the anchor (see FW_LOW_ANCHOR_BASE),
 * so everything from 5.2.3790.0 up runs without it.  Recognised from the image's
 * VS_FIXEDFILEINFO (signature 0xFEEF04BD, then dwStrucVersion,
 * dwFileVersionMS, dwFileVersionLS) - the retail loaders carry no other
 * version marker in their PE headers.
 */
static BOOLEAN pe_image_wants_contiguous_low_ram(const UINT8 *Image,
                                                 UINTN ImageSize)
{
    UINTN i;

    if (Image == NULL || ImageSize < 20U) {
        return 0;
    }
    for (i = 0; i + 20U <= ImageSize; i += 4U) {
        UINT32 ms;
        UINT32 ls;

        if (Image[i] != 0xBDU || Image[i + 1] != 0x04U ||
            Image[i + 2] != 0xEFU || Image[i + 3] != 0xFEU) {
            continue;
        }
        fw_copy_mem(&ms, Image + i + 8U, sizeof(ms));
        fw_copy_mem(&ls, Image + i + 12U, sizeof(ls));
        /* 5.2.3790.0 (Server 2003 RTM) and later; 5.1.x (XP, 2462) keep it. */
        return ms > 0x00050002U ||
               (ms == 0x00050002U && (ls >> 16) >= 3790U);
    }
    return 0;
}

static BOOLEAN pe_rva_range_valid(UINT32 Rva, UINT32 Size, UINT32 ImageSize)
{
    return Rva <= ImageSize && Size <= ImageSize - Rva;
}

static UINT16 pe_read_u16(const UINT8 *Data)
{
    return (UINT16)Data[0] | ((UINT16)Data[1] << 8);
}

static UINT32 pe_read_u32(const UINT8 *Data)
{
    return (UINT32)Data[0] | ((UINT32)Data[1] << 8) |
           ((UINT32)Data[2] << 16) | ((UINT32)Data[3] << 24);
}

static BOOLEAN pe_resource_range_valid(UINT32 Offset, UINT32 Size,
                                       UINT32 ResourceSize)
{
    return Offset <= ResourceSize && Size <= ResourceSize - Offset;
}

static BOOLEAN pe_resource_name_is_hii(const UINT8 *Root,
                                       UINT32 ResourceSize, UINT32 Name)
{
    UINT32 offset;

    if ((Name & 0x80000000U) == 0) {
        return 0;
    }
    offset = Name & 0x7fffffffU;
    if (!pe_resource_range_valid(offset, 8U, ResourceSize) ||
        pe_read_u16(Root + offset) != 3U) {
        return 0;
    }
    return pe_read_u16(Root + offset + 2U) == 'H' &&
           pe_read_u16(Root + offset + 4U) == 'I' &&
           pe_read_u16(Root + offset + 6U) == 'I';
}

static BOOLEAN pe_resource_find_data(const UINT8 *Root,
                                     UINT32 ResourceSize,
                                     UINT32 EntryOffset, UINTN Depth,
                                     UINT32 *DataRva, UINT32 *DataSize)
{
    UINT32 offset = EntryOffset & 0x7fffffffU;
    UINT32 entry_count;
    UINT32 i;

    if ((EntryOffset & 0x80000000U) == 0) {
        if (!pe_resource_range_valid(offset, 16U, ResourceSize)) {
            return 0;
        }
        *DataRva = pe_read_u32(Root + offset);
        *DataSize = pe_read_u32(Root + offset + 4U);
        return 1;
    }
    if (Depth >= 4U ||
        !pe_resource_range_valid(offset, 16U, ResourceSize)) {
        return 0;
    }
    entry_count = (UINT32)pe_read_u16(Root + offset + 12U) +
                  (UINT32)pe_read_u16(Root + offset + 14U);
    if (entry_count > (ResourceSize - offset - 16U) / 8U) {
        return 0;
    }
    for (i = 0; i < entry_count; i++) {
        UINT32 child = pe_read_u32(Root + offset + 16U + i * 8U + 4U);

        if (pe_resource_find_data(Root, ResourceSize, child, Depth + 1U,
                                  DataRva, DataSize)) {
            return 1;
        }
    }
    return 0;
}

static BOOLEAN pe_hii_package_list_valid(const UINT8 *Packages,
                                         UINT32 ResourceDataSize)
{
    UINT32 package_list_size;
    UINT32 offset;
    BOOLEAN end_package = 0;

    if (ResourceDataSize < 24U) {
        return 0;
    }
    package_list_size = pe_read_u32(Packages + 16U);
    if (package_list_size < 24U || package_list_size > ResourceDataSize) {
        return 0;
    }
    offset = 20U;
    while (offset < package_list_size) {
        UINT32 header;
        UINT32 length;
        UINT8 type;

        if (package_list_size - offset < 4U) {
            return 0;
        }
        header = pe_read_u32(Packages + offset);
        length = header & 0x00ffffffU;
        type = (UINT8)(header >> 24);
        if (length < 4U || length > package_list_size - offset) {
            return 0;
        }
        if (type == 0xdfU) {
            if (length != 4U || offset + length != package_list_size) {
                return 0;
            }
            end_package = 1;
        } else if (end_package) {
            return 0;
        }
        offset += length;
    }
    return end_package;
}

static VOID *pe_hii_package_list(UINT64 ImageBase, UINT32 SizeOfImage,
                                 UINT32 ResourceRva, UINT32 ResourceSize)
{
    const UINT8 *root;
    UINT32 entry_count;
    UINT32 data_rva;
    UINT32 data_size;
    UINT32 i;

    if (ResourceRva == 0 || ResourceSize == 0 ||
        !pe_rva_range_valid(ResourceRva, ResourceSize, SizeOfImage)) {
        return NULL;
    }
    root = (const UINT8 *)(UINTN)(ImageBase + ResourceRva);
    if (!pe_resource_range_valid(0, 16U, ResourceSize)) {
        return NULL;
    }
    entry_count = (UINT32)pe_read_u16(root + 12U) +
                  (UINT32)pe_read_u16(root + 14U);
    if (entry_count > (ResourceSize - 16U) / 8U) {
        return NULL;
    }
    for (i = 0; i < entry_count; i++) {
        const UINT8 *entry = root + 16U + i * 8U;
        UINT32 name = pe_read_u32(entry);
        UINT32 child = pe_read_u32(entry + 4U);
        const UINT8 *packages;

        if (!pe_resource_name_is_hii(root, ResourceSize, name) ||
            !pe_resource_find_data(root, ResourceSize, child, 0,
                                   &data_rva, &data_size) ||
            !pe_rva_range_valid(data_rva, data_size, SizeOfImage)) {
            continue;
        }
        packages = (const UINT8 *)(UINTN)(ImageBase + data_rva);
        if (pe_hii_package_list_valid(packages, data_size)) {
            return (VOID *)packages;
        }
    }
    return NULL;
}

static void pe_write_u16(UINT8 *Data, UINT16 Value)
{
    Data[0] = (UINT8)Value;
    Data[1] = (UINT8)(Value >> 8);
}

static void pe_write_u32(UINT8 *Data, UINT32 Value)
{
    Data[0] = (UINT8)Value;
    Data[1] = (UINT8)(Value >> 8);
    Data[2] = (UINT8)(Value >> 16);
    Data[3] = (UINT8)(Value >> 24);
}

BOOLEAN pe_hii_package_list_selftest(VOID)
{
    UINT8 image[512] __attribute__((aligned(8)));
    UINT8 *root = image + 0x40U;
    UINT8 *packages = image + 0x180U;

    fw_set_mem(image, sizeof(image), 0);

    /* Root type entry named "HII". */
    pe_write_u16(root + 12U, 1U);
    pe_write_u32(root + 16U, 0x80000020U);
    pe_write_u32(root + 20U, 0x80000030U);
    pe_write_u16(root + 0x20U, 3U);
    pe_write_u16(root + 0x22U, 'H');
    pe_write_u16(root + 0x24U, 'I');
    pe_write_u16(root + 0x26U, 'I');

    /* Name and language directories, followed by a resource data entry. */
    pe_write_u16(root + 0x30U + 14U, 1U);
    pe_write_u32(root + 0x30U + 16U, 1U);
    pe_write_u32(root + 0x30U + 20U, 0x80000050U);
    pe_write_u16(root + 0x50U + 14U, 1U);
    pe_write_u32(root + 0x50U + 16U, 0x409U);
    pe_write_u32(root + 0x50U + 20U, 0x70U);
    pe_write_u32(root + 0x70U, 0x180U);
    pe_write_u32(root + 0x74U, 24U);

    /* Package-list header and its mandatory end package. */
    pe_write_u32(packages + 16U, 24U);
    pe_write_u32(packages + 20U, 0xdf000004U);
    if (pe_hii_package_list((UINT64)(UINTN)image, sizeof(image),
                            0x40U, 0x80U) != packages) {
        return 0;
    }

    pe_write_u32(packages + 16U, 23U);
    if (pe_hii_package_list((UINT64)(UINTN)image, sizeof(image),
                            0x40U, 0x80U) != NULL) {
        return 0;
    }
    pe_write_u32(packages + 16U, 24U);
    pe_write_u16(root + 12U, 0xffffU);
    return pe_hii_package_list((UINT64)(UINTN)image, sizeof(image),
                               0x40U, 0x80U) == NULL;
}

/* PE_RELOCATION_MODE lives in fw-pe.h. */

static BOOLEAN pe_relocation_log_entries(UINT32 SizeOfImage,
                                         UINT8 *RelocData,
                                         UINT32 RelocSize,
                                         UINTN *LogEntries)
{
    UINT32 offset = 0;
    UINTN entries = 0;

    if (LogEntries == NULL) {
        return 0;
    }
    while (offset < RelocSize) {
        UINT32 page_rva;
        UINT32 block_size;
        UINT32 count;
        UINT32 jj;

        if (RelocSize - offset < 8) {
            return 0;
        }
        page_rva = *(UINT32 *)(RelocData + offset);
        block_size = *(UINT32 *)(RelocData + offset + 4);
        if (block_size < 8 || (block_size & 3U) != 0 ||
            block_size > RelocSize - offset) {
            return 0;
        }

        count = (block_size - 8) / 2;
        for (jj = 0; jj < count; jj++) {
            UINT16 entry = *(UINT16 *)(RelocData + offset + 8 + jj * 2);
            UINT8 type = entry >> 12;
            UINT16 rva = entry & 0xFFF;
            UINT64 reloc_off = (UINT64)page_rva + rva;
            UINT64 bundle_off;

            if (type == IMAGE_REL_BASED_ABSOLUTE) {
                continue;
            }
            switch (type) {
            case IMAGE_REL_BASED_HIGHLOW:
                if (reloc_off > SizeOfImage ||
                    sizeof(UINT32) > SizeOfImage - reloc_off) {
                    return 0;
                }
                break;
            case IMAGE_REL_BASED_DIR64:
                if (reloc_off > SizeOfImage ||
                    sizeof(UINT64) > SizeOfImage - reloc_off) {
                    return 0;
                }
                break;
            case IMAGE_REL_BASED_IA64_IMM64:
                bundle_off = reloc_off & ~0xFULL;
                if (bundle_off > SizeOfImage ||
                    16U > SizeOfImage - bundle_off) {
                    return 0;
                }
                break;
            default:
                return 0;
            }
            if (entries == (UINTN)-1) {
                return 0;
            }
            entries++;
        }
        offset += block_size;
    }

    *LogEntries = entries;
    return 1;
}

BOOLEAN pe_apply_relocations(UINT64 ImageBase, UINT32 SizeOfImage,
                                    UINT32 RelocRva, UINT32 RelocSize,
                                    UINT64 Adjust, PE_RELOCATION_MODE Mode,
                                    UINT64 *RelocationLog,
                                    UINTN RelocationLogEntries)
{
    UINT8 *reloc_data;
    UINT32 offset = 0;
    UINTN expected_entries;
    UINTN log_index = 0;

    if (RelocRva == 0 || RelocSize == 0) {
        return RelocRva == 0 && RelocSize == 0 &&
               RelocationLogEntries == 0;
    }
    if (!pe_rva_range_valid(RelocRva, RelocSize, SizeOfImage)) {
        return 0;
    }

    reloc_data = (UINT8 *)(UINTN)(ImageBase + RelocRva);
    if (!pe_relocation_log_entries(SizeOfImage, reloc_data, RelocSize,
                                   &expected_entries) ||
        (RelocationLog == NULL && RelocationLogEntries != 0) ||
        (RelocationLog != NULL &&
         RelocationLogEntries != expected_entries) ||
        (Mode == PE_RELOCATE_RUNTIME && expected_entries != 0 &&
         RelocationLog == NULL)) {
        return 0;
    }

    while (offset < RelocSize) {
        UINT32 page_rva = *(UINT32 *)(reloc_data + offset);
        UINT32 block_size = *(UINT32 *)(reloc_data + offset + 4);
        UINT32 count = (block_size - 8) / 2;
        UINT32 jj;

        for (jj = 0; jj < count; jj++) {
            UINT16 entry = *(UINT16 *)(reloc_data + offset + 8 + jj * 2);
            UINT8 type = entry >> 12;
            UINT16 rva = entry & 0xFFF;
            UINT64 reloc_off = (UINT64)page_rva + rva;
            BOOLEAN apply;

            if (type == IMAGE_REL_BASED_ABSOLUTE) {
                continue;
            }
            apply = 1;

            if (type == IMAGE_REL_BASED_HIGHLOW) {
                UINT32 *patch = (UINT32 *)(UINTN)(ImageBase + reloc_off);

                if (Mode == PE_RELOCATE_RUNTIME) {
                    apply = *patch == (UINT32)RelocationLog[log_index];
                }
                if (apply) {
                    *patch += (UINT32)Adjust;
                }
                if (Mode == PE_RELOCATE_LOAD && RelocationLog != NULL) {
                    RelocationLog[log_index] = *patch;
                }
            } else if (type == IMAGE_REL_BASED_DIR64) {
                UINT64 *patch = (UINT64 *)(UINTN)(ImageBase + reloc_off);

                if (Mode == PE_RELOCATE_RUNTIME) {
                    apply = *patch == RelocationLog[log_index];
                }
                if (apply) {
                    *patch += Adjust;
                }
                if (Mode == PE_RELOCATE_LOAD && RelocationLog != NULL) {
                    RelocationLog[log_index] = *patch;
                }
            } else if (type == IMAGE_REL_BASED_IA64_IMM64) {
                UINT8 *reloc_addr = (UINT8 *)(UINTN)(ImageBase + reloc_off);
                UINT64 *bundle =
                    (UINT64 *)((UINTN)reloc_addr & ~(UINTN)0xFULL);
                UINT64 value;

                if (Mode == PE_RELOCATE_RUNTIME &&
                    bundle[0] != RelocationLog[log_index]) {
                    log_index++;
                    continue;
                }
                if (!pe_read_ia64_imm64_reloc(reloc_addr, &value)) {
                    return 0;
                }
                if (!pe_write_ia64_imm64_reloc(reloc_addr, value + Adjust)) {
                    return 0;
                }
                if (RelocationLog != NULL) {
                    RelocationLog[log_index] = bundle[0];
                }
            }
            log_index++;
        }

        offset += block_size;
    }

    return log_index == expected_entries;
}

static BOOLEAN pe_loaded_image_reloc_info(UINT64 ImageBase, UINTN ImageSize,
                                          UINT16 *Subsystem,
                                          UINT32 *SizeOfImage,
                                          UINT32 *RelocRva,
                                          UINT32 *RelocSize)
{
    IMAGE_DOS_HEADER *dos_hdr;
    IMAGE_FILE_HEADER *file_hdr;
    UINT32 *nt_sig;
    UINT16 magic;
    UINT32 *data_dir = NULL;
    UINT32 number_of_rva_and_sizes = 0;

    if (ImageSize < sizeof(IMAGE_DOS_HEADER)) {
        return 0;
    }
    dos_hdr = (IMAGE_DOS_HEADER *)(UINTN)ImageBase;
    if (dos_hdr->e_magic != IMAGE_DOS_SIGNATURE ||
        dos_hdr->e_lfanew >= ImageSize - sizeof(UINT32)) {
        return 0;
    }

    nt_sig = (UINT32 *)(UINTN)(ImageBase + dos_hdr->e_lfanew);
    if (*nt_sig != IMAGE_NT_SIGNATURE ||
        dos_hdr->e_lfanew + sizeof(UINT32) + sizeof(IMAGE_FILE_HEADER) >=
        ImageSize) {
        return 0;
    }
    file_hdr = (IMAGE_FILE_HEADER *)((UINT8 *)nt_sig + sizeof(UINT32));
    if (file_hdr->Machine != IMAGE_FILE_MACHINE_IA64) {
        return 0;
    }
    magic = *(UINT16 *)((UINT8 *)file_hdr + sizeof(IMAGE_FILE_HEADER));

    if (magic == 0x010B) {
        IMAGE_OPTIONAL_HEADER32 *opt32 = (IMAGE_OPTIONAL_HEADER32 *)
            ((UINT8 *)file_hdr + sizeof(IMAGE_FILE_HEADER));

        *Subsystem = opt32->Subsystem;
        *SizeOfImage = opt32->SizeOfImage;
        number_of_rva_and_sizes = *(UINT32 *)((UINT8 *)opt32 + 108);
        data_dir = (UINT32 *)((UINT8 *)opt32 + 112);
    } else if (magic == 0x020B) {
        IMAGE_OPTIONAL_HEADER64 *opt64 = (IMAGE_OPTIONAL_HEADER64 *)
            ((UINT8 *)file_hdr + sizeof(IMAGE_FILE_HEADER));

        *Subsystem = opt64->Subsystem;
        *SizeOfImage = opt64->SizeOfImage;
        number_of_rva_and_sizes = opt64->NumberOfRvaAndSizes;
        data_dir = (UINT32 *)((UINT8 *)opt64 + 112);
    } else {
        return 0;
    }

    if (*SizeOfImage == 0 || *SizeOfImage > ImageSize ||
        number_of_rva_and_sizes < 6 || data_dir == NULL) {
        return 0;
    }
    *RelocRva = data_dir[10];
    *RelocSize = data_dir[11];
    return 1;
}

EFI_STATUS pe_relocate_loaded_runtime_image(UINT64 ImageBase,
                                                   UINTN ImageSize,
                                                   UINT64 *RelocationLog,
                                                   UINTN RelocationLogEntries)
{
    UINT16 subsystem = IMAGE_SUBSYSTEM_EFI_APPLICATION;
    UINT32 size_of_image = 0;
    UINT32 reloc_rva = 0;
    UINT32 reloc_size = 0;
    UINTN virtual_base = (UINTN)ImageBase;

    if (!pe_loaded_image_reloc_info(ImageBase, ImageSize, &subsystem,
                                    &size_of_image, &reloc_rva,
                                    &reloc_size)) {
        return EFI_LOAD_ERROR;
    }
    if (subsystem != IMAGE_SUBSYSTEM_EFI_RUNTIME_DRIVER) {
        return EFI_SUCCESS;
    }
    if (rs_convert_pointer_value(&virtual_base) != EFI_SUCCESS) {
        return EFI_NOT_FOUND;
    }
    if (!pe_apply_relocations(ImageBase, size_of_image, reloc_rva,
                              reloc_size, virtual_base - ImageBase,
                              PE_RELOCATE_RUNTIME, RelocationLog,
                              RelocationLogEntries)) {
        return EFI_LOAD_ERROR;
    }
    return EFI_SUCCESS;
}

EFI_STATUS pe_relocate_runtime_images(void)
{
    UINTN i;

    for (i = 0; i < LOADED_IMAGE_MAX; i++) {
        EFI_LOADED_IMAGE_RECORD *rec = &mLoadedImages[i];
        EFI_STATUS st;

        if (!rec->in_use ||
            rec->loaded_image.ImageCodeType != EfiRuntimeServicesCode ||
            rec->loaded_image.ImageBase == NULL ||
            rec->loaded_image.ImageSize == 0) {
            continue;
        }

        st = pe_relocate_loaded_runtime_image(
            (UINT64)(UINTN)rec->loaded_image.ImageBase,
            rec->loaded_image.ImageSize, rec->runtime_relocation_log,
            rec->runtime_relocation_entries);
        if (st != EFI_SUCCESS) {
            return st;
        }
    }
    return EFI_SUCCESS;
}

void *load_pe_image(uint8_t *image_base, UINTN image_size,
                           PE_LOADED_IMAGE_RESULT *Result)
{
    IMAGE_DOS_HEADER *dos_hdr;
    IMAGE_FILE_HEADER *file_hdr;
    IMAGE_SECTION_HEADER *sections;
    UINT64 image_base_addr;
    UINT64 linked_image_base_addr;
    EFI_MEMORY_TYPE code_type;
    EFI_MEMORY_TYPE data_type;
    UINT32 *data_dir = NULL;
    UINT32 number_of_rva_and_sizes = 0;
    UINT32 entry_rva;
    UINT32 reloc_rva = 0;
    UINT32 reloc_size = 0;
    UINT32 size_of_image = 0;
    UINT32 size_of_headers = 0;
    UINT64 *relocation_log = NULL;
    UINTN relocation_entries = 0;
    UINT16 subsystem = IMAGE_SUBSYSTEM_EFI_APPLICATION;
    UINT16 machine;
    UINT16 magic;
    BOOLEAN relocations_stripped;
    UINTN optional_offset;
    UINTN section_offset;
    UINTN section_table_size;
    UINTN data_directory_capacity;
    UINTN i;

    if (Result == NULL) {
        return NULL;
    }
    fw_set_mem(Result, sizeof(*Result), 0);
    Result->subsystem = IMAGE_SUBSYSTEM_EFI_APPLICATION;
    if (image_base == NULL || image_size < sizeof(IMAGE_DOS_HEADER)) {
        return NULL;
    }

    dos_hdr = (IMAGE_DOS_HEADER *)image_base;
    if (dos_hdr->e_magic != IMAGE_DOS_SIGNATURE) {
        return NULL;
    }
    if (dos_hdr->e_lfanew > image_size - sizeof(UINT32) ||
        image_size - dos_hdr->e_lfanew <
            sizeof(UINT32) + sizeof(IMAGE_FILE_HEADER)) {
        return NULL;
    }

    /* Check NT signature */
    UINT32 *nt_sig = (UINT32 *)(image_base + dos_hdr->e_lfanew);
    if (*nt_sig != IMAGE_NT_SIGNATURE) {
        return NULL;
    }

    file_hdr = (IMAGE_FILE_HEADER *)((uint8_t *)nt_sig + 4);
    machine = file_hdr->Machine;
    if (machine != IMAGE_FILE_MACHINE_IA64 &&
        machine != IMAGE_FILE_MACHINE_EBC) {
        return NULL;
    }

    optional_offset = (UINTN)((UINT8 *)file_hdr - image_base) +
                      sizeof(IMAGE_FILE_HEADER);
    if (file_hdr->SizeOfOptionalHeader < 112U ||
        optional_offset > image_size ||
        file_hdr->SizeOfOptionalHeader > image_size - optional_offset) {
        return NULL;
    }
    magic = *(UINT16 *)(image_base + optional_offset);
    if (magic == 0x010B && machine == IMAGE_FILE_MACHINE_IA64) {
        IMAGE_OPTIONAL_HEADER32 *opt32 = (IMAGE_OPTIONAL_HEADER32 *)
            (image_base + optional_offset);
        entry_rva = opt32->AddressOfEntryPoint;
        linked_image_base_addr = opt32->ImageBase;
        size_of_image = opt32->SizeOfImage;
        subsystem = opt32->Subsystem;
        size_of_headers = opt32->SizeOfHeaders;
        /*
         * IA-64 EFI images can use PE32 magic while keeping 64-bit
         * stack/heap fields, so the data directory starts at the PE32+
         * offset.
         */
        number_of_rva_and_sizes = *(UINT32 *)((uint8_t *)opt32 + 108);
        data_dir = (UINT32 *)((uint8_t *)opt32 + 112);
    } else if (magic == 0x020B) {
        IMAGE_OPTIONAL_HEADER64 *opt64 = (IMAGE_OPTIONAL_HEADER64 *)
            (image_base + optional_offset);
        entry_rva = opt64->AddressOfEntryPoint;
        linked_image_base_addr = opt64->ImageBase;
        size_of_image = opt64->SizeOfImage;
        subsystem = opt64->Subsystem;
        size_of_headers = opt64->SizeOfHeaders;
        number_of_rva_and_sizes = opt64->NumberOfRvaAndSizes;
        data_dir = (UINT32 *)((uint8_t *)opt64 + 112);
    } else {
        return NULL;
    }

    data_directory_capacity =
        (file_hdr->SizeOfOptionalHeader - 112U) / (2U * sizeof(UINT32));
    if (file_hdr->NumberOfSections == 0 ||
        number_of_rva_and_sizes > data_directory_capacity ||
        size_of_image == 0 || size_of_headers == 0 ||
        size_of_headers > image_size || size_of_headers > size_of_image) {
        return NULL;
    }

    section_offset = optional_offset + file_hdr->SizeOfOptionalHeader;
    section_table_size = (UINTN)file_hdr->NumberOfSections *
                         sizeof(IMAGE_SECTION_HEADER);
    if (section_offset > image_size ||
        section_table_size > image_size - section_offset ||
        section_offset + section_table_size > size_of_headers) {
        return NULL;
    }
    sections = (IMAGE_SECTION_HEADER *)(image_base + section_offset);
    for (i = 0; i < file_hdr->NumberOfSections; i++) {
        UINT32 copy_size = sections[i].SizeOfRawData;

        if (sections[i].VirtualSize != 0 &&
            sections[i].VirtualSize < copy_size) {
            copy_size = sections[i].VirtualSize;
        }
        if (!pe_rva_range_valid(sections[i].VirtualAddress, copy_size,
                                size_of_image) ||
            (sections[i].SizeOfRawData != 0 &&
             (sections[i].PointerToRawData > image_size ||
              sections[i].SizeOfRawData >
                  image_size - sections[i].PointerToRawData))) {
            return NULL;
        }
    }

    if (entry_rva >= size_of_image ||
        (machine == IMAGE_FILE_MACHINE_IA64 &&
         sizeof(UINT64) * 2U > size_of_image - entry_rva) ||
        (machine == IMAGE_FILE_MACHINE_EBC &&
         ((entry_rva & 1U) != 0 ||
          subsystem == IMAGE_SUBSYSTEM_EFI_RUNTIME_DRIVER))) {
        return NULL;
    }
    /*
     * Without a base relocation directory the image can only run where it
     * was linked, so the base has to be honoured rather than reassigned.
     */
    relocations_stripped =
        (file_hdr->Characteristics & IMAGE_FILE_RELOCS_STRIPPED) != 0 ||
        number_of_rva_and_sizes < 6 || data_dir == NULL ||
        data_dir[11] == 0;
    if (subsystem == IMAGE_SUBSYSTEM_EFI_APPLICATION &&
        fw_map_quirk_enabled(IA64_FW_QUIRK_ANCHOR_VERSION_SNIFF) &&
        pe_image_wants_contiguous_low_ram(image_base, image_size)) {
        efi_release_low_anchor();
    }
    image_base_addr = pe_choose_image_base(
        linked_image_base_addr, size_of_image,
        subsystem == IMAGE_SUBSYSTEM_EFI_RUNTIME_DRIVER,
        relocations_stripped, (UINT64)(UINTN)image_base, image_size);
    if (image_base_addr == 0) {
        return NULL;
    }
    Result->base = (VOID *)(UINTN)image_base_addr;
    Result->size = size_of_image;
    Result->subsystem = subsystem;
    Result->machine = machine;

    fw_set_mem((VOID *)(UINTN)image_base_addr, size_of_image, 0);
    if (size_of_headers != 0) {
        fw_copy_mem((VOID *)(UINTN)image_base_addr, image_base,
                    size_of_headers);
    }

    for (i = 0; i < file_hdr->NumberOfSections; i++) {
        if (sections[i].SizeOfRawData > 0) {
            uint8_t *src = image_base + sections[i].PointerToRawData;
            uint8_t *dst = (uint8_t *)(UINTN)
                (image_base_addr + sections[i].VirtualAddress);
            UINT32 size = sections[i].SizeOfRawData;

            if (sections[i].VirtualSize != 0 && sections[i].VirtualSize < size) {
                size = sections[i].VirtualSize;
            }
            if (size > 0) {
                fw_copy_mem(dst, src, size);
            }
        }
    }

    if (number_of_rva_and_sizes >= 3 && data_dir != NULL) {
        Result->hii_package_list = pe_hii_package_list(
            image_base_addr, size_of_image, data_dir[4], data_dir[5]);
    }

    pe_image_memory_types(subsystem, &code_type, &data_type);
    if (!pe_mark_loaded_image_memory(image_base_addr, size_of_image, sections,
                                     file_hdr->NumberOfSections, code_type,
                                     data_type)) {
        return NULL;
    }

    /* PE base relocations (DataDirectory[5]).
     *
     * If the image base differs from the linked ImageBase, fix up
     * absolute address materialization.  IA-64 PE images use both
     * DIR64 data entries and IMM64 relocations for movl instructions.
     * Runtime images also retain the fixed-up values so virtual relocation
     * does not overwrite fields that the driver modifies after loading.
     */
    if (number_of_rva_and_sizes >= 6 && data_dir != NULL) {
        UINT8 *reloc_data;
        UINT64 delta = image_base_addr - linked_image_base_addr;

        reloc_rva = data_dir[10];
        reloc_size = data_dir[11];
        if ((reloc_rva == 0) != (reloc_size == 0) ||
            !pe_rva_range_valid(reloc_rva, reloc_size, size_of_image)) {
            return NULL;
        }
        reloc_data = (UINT8 *)(UINTN)(image_base_addr + reloc_rva);
        if (reloc_size != 0 &&
            !pe_relocation_log_entries(size_of_image, reloc_data, reloc_size,
                                       &relocation_entries)) {
            return NULL;
        }
        if (delta != 0 && relocation_entries == 0) {
            return NULL;
        }
        if (subsystem == IMAGE_SUBSYSTEM_EFI_RUNTIME_DRIVER &&
            relocation_entries != 0) {
            if (relocation_entries >
                (UINTN)-1 / sizeof(*relocation_log) ||
                bs_allocate_pool(EfiRuntimeServicesData,
                                 relocation_entries *
                                 sizeof(*relocation_log),
                                 (VOID **)&relocation_log) != EFI_SUCCESS) {
                return NULL;
            }
            Result->runtime_relocation_log = relocation_log;
            Result->runtime_relocation_entries = relocation_entries;
        }
        if ((delta != 0 || relocation_log != NULL) &&
            !pe_apply_relocations(image_base_addr, size_of_image,
                                  reloc_rva, reloc_size, delta,
                                  PE_RELOCATE_LOAD, relocation_log,
                                  relocation_log != NULL ?
                                  relocation_entries : 0)) {
            return NULL;
        }
    } else if (image_base_addr != linked_image_base_addr) {
        return NULL;
    }

    if (machine == IMAGE_FILE_MACHINE_EBC) {
        return (VOID *)(UINTN)(image_base_addr + entry_rva);
    }

    /* IA-64 function pointers are plabels, so return the descriptor itself. */
    return (VOID *)(UINTN)(image_base_addr + entry_rva);
}

