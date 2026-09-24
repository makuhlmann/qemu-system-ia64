/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * EFI memory-map state and API (efi_memmap.c).  The descriptor arrays
 * stay exported for now: the page allocator and GetMemoryMap in
 * firmware.c iterate them directly; a closed API is not done yet.
 */

#ifndef IA64_FIRMWARE_FW_MEMMAP_H
#define IA64_FIRMWARE_FW_MEMMAP_H

#include "fw-base.h"
#include "fw-efi-types.h"

#define MEMORY_MAP_MAX   128

extern EFI_MEMORY_DESCRIPTOR  mMemoryMap[MEMORY_MAP_MAX];
extern UINTN                  mMemoryMapEntries;
extern UINTN                  mMapKey;
extern BOOLEAN                mLowAnchorArmed;

/*
 * firmware.c-owned state the builder and selftest still reach directly;
 * to be narrowed when firmware_main gains its phase structure.
 */
extern UINT64                 mGuestRamSize;
extern UINT64                 mGuestLowRamEnd;
extern UINT64                 mSystemTablePointerBase;
extern EFI_SYSTEM_TABLE_POINTER *mSystemTablePointer;
extern UINT64                 mBootStackBase;
extern UINT64                 mBootStackTop;
extern EFI_PHYSICAL_ADDRESS   mNextPageAddr;

void fw_init_guest_high_ram_ranges(UINT64 RamSize);
UINT64 fw_system_table_pointer_base(UINT64 LowRamEnd, UINT64 BootStackBase,
                                    UINT64 BootStackTop);
UINT64 efi_memory_attribute(EFI_MEMORY_TYPE Type, UINT64 Attribute);

void efi_init_memory_map(void);
/* Quirk toggles: bits are IA64_FW_QUIRK_* (ia64_vpc_abi.h), default all on. */
BOOLEAN fw_map_quirk_enabled(UINT64 QuirkBit);
UINT64 fw_handoff_map_quirk_disable(void);
void efi_add_memory_range(UINTN *Index, EFI_MEMORY_TYPE Type,
                          UINT64 Start, UINT64 End, UINT64 Attribute);
void efi_insert_memory_descriptor(UINTN Index,
                                  EFI_MEMORY_DESCRIPTOR Descriptor);
BOOLEAN efi_preserve_memory_map_boundary(UINT64 Boundary);
BOOLEAN efi_memory_descriptors_can_merge(EFI_MEMORY_DESCRIPTOR *A,
                                         EFI_MEMORY_DESCRIPTOR *B);
BOOLEAN efi_memory_descriptor_requires_ia64_alignment(EFI_MEMORY_TYPE Type,
                                                      UINT64 Attribute);
BOOLEAN efi_align_up_u64(UINT64 Value, UINT64 Alignment, UINT64 *Result);
void efi_coalesce_memory_map(void);
BOOLEAN efi_mark_memory_range(EFI_MEMORY_TYPE Type, UINT64 Start,
                              UINT64 End, UINT64 Attribute);
BOOLEAN efi_memory_map_has_descriptor(EFI_MEMORY_TYPE Type,
                                      UINT64 Start, UINT64 End,
                                      UINT64 Attribute);
BOOLEAN efi_memory_map_is_sorted(void);
BOOLEAN efi_memory_map_has_ia64_descriptor_alignment(void);
BOOLEAN efi_memory_map_covers_range(EFI_MEMORY_TYPE Type,
                                    UINT64 Start, UINT64 End,
                                    UINT64 Attribute);
BOOLEAN efi_memory_map_has_boot_stack_layout(void);
BOOLEAN uefi_memory_map_selftest(void);
UINT64 fw_low_anchor_base(void);
void efi_release_low_anchor(void);

#endif /* IA64_FIRMWARE_FW_MEMMAP_H */
