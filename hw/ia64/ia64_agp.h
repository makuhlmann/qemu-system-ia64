/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Intel 460GX GXB AGP host bridge + GART.
 */

#ifndef HW_IA64_AGP_H
#define HW_IA64_AGP_H

#include "hw/pci/pci_device.h"
#include "hw/core/sysbus.h"
#include "system/memory.h"
#include "qom/object.h"

#define TYPE_IA64_AGP "ia64-agp-gxb"
OBJECT_DECLARE_SIMPLE_TYPE(IA64AGPState, IA64_AGP)

#define TYPE_IA64_AGP_IOMMU_MEMORY_REGION "ia64-agp-iommu-memory-region"

struct IA64AGPState {
    SysBusDevice parent_obj;

    MemoryRegion gart_window;    /* GART SRAM window at 0xFE200000           */
    IOMMUMemoryRegion iommu;     /* per-bus DMA translation                  */
    AddressSpace dma_as;

    uint32_t *gatt;              /* GART SRAM, one 32-bit entry per page     */
    uint32_t gatt_entries;       /* sram_size / 4                            */

    /* Function 1 registers (SSDM ch. 7; agp460.h, i460-agp for the layout). */
    uint8_t gxbctl;
    uint8_t agpsiz;
    uint64_t apbase;             /* 10h, the base while AGPSIZ bit 3 is 0    */
    uint64_t bapbase;            /* 98h, the base while AGPSIZ bit 3 is 1    */
    uint32_t agp_command;

    uint64_t aperture_base;      /* decoded from the registers above         */
    uint64_t aperture_size;      /* 0 = no aperture                          */
    unsigned page_shift;         /* 12, or 22 with GXBCTL 4 MB pages         */
    bool aperture_enabled;

    /*
     * Devfn of the single AGP graphics master whose DMA the GART translates.
     * On the real 460GX the GART sits only on the GXB's AGP port; other PCI
     * masters do not traverse it.  Set by the machine; -1 leaves every master
     * on a plain identity pass-through.
     */
    int32_t agp_master_devfn;

    /* GART SRAM fitted on the board: 0 (none), 256 KiB or 1 MiB. */
    uint32_t sram_size;
};

void ia64_agp_attach_bus(IA64AGPState *s, PCIBus *bus);

/*
 * The GXB's function 1 on the chipset bus: the offsets this model answers
 * for, and byte accesses to them.  The rest of the function's header is
 * the chipset's configuration store.
 */
bool ia64_agp_cfg_owns(unsigned off);
uint8_t ia64_agp_cfg_readb(IA64AGPState *s, unsigned off);
void ia64_agp_cfg_writeb(IA64AGPState *s, unsigned off, uint8_t val);

#endif /* HW_IA64_AGP_H */
