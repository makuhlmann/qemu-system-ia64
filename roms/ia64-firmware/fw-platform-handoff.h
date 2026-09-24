/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The platform -> EFI-core handoff, modeled on the Intel EFI 1.10
 * sample's SALEFIHANDOFF contract (NOTES/SalEfiHandOffState.pdf in that
 * sample): the platform side
 * (platform.c, efi_memmap.c, platform_tables.c) produces the memory
 * map and the guest platform tables and exposes them, plus the entry
 * points the core needs, through one struct.  The EFI core (firmware.c)
 * consumes it via fw_platform().
 *
 * The flat descriptor array stays directly visible to the core's page
 * allocator for now (fw-memmap.h); once that access path closes,
 * MemDesc/MemDescCount/MapKey here become the only channel.
 */

#ifndef IA64_FIRMWARE_FW_PLATFORM_HANDOFF_H
#define IA64_FIRMWARE_FW_PLATFORM_HANDOFF_H

#include "fw-base.h"
#include "fw-efi-types.h"

typedef struct {
    /* The memory map the platform built; the core mutates it in place. */
    EFI_MEMORY_DESCRIPTOR *MemDesc;
    UINTN                 *MemDescCount;
    UINTN                 *MapKey;
    /* Guest platform tables (valid after InitPlatformTables). */
    VOID                  *SalSystemTable;
    VOID                  *AcpiRsdp;
    /* Platform entry points the core drives. */
    void   (*DecodeTopology)(void);
    void   (*InitMemoryMap)(void);
    void   (*InitPlatformTables)(void);
    UINT64 (*SalProcFunctionEntry)(void);
} FW_PLATFORM_HANDOFF;

const FW_PLATFORM_HANDOFF *fw_platform(void);
/* platform_tables.c fills the table pointers once they exist. */
void fw_platform_publish_tables(VOID *SalSystemTable, VOID *AcpiRsdp);

#endif /* IA64_FIRMWARE_FW_PLATFORM_HANDOFF_H */
