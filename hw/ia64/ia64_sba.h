/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * HP zx1 SBA (System Bus Adapter) IOC IOMMU for the ia64-vpc machine.
 */

#ifndef HW_IA64_SBA_H
#define HW_IA64_SBA_H

#include "hw/pci/pci_device.h"
#include "hw/pci-host/hp-zx1-iommu.h"
#include "system/memory.h"
#include "qemu/thread.h"
#include "qom/object.h"

#define TYPE_IA64_SBA "ia64-sba-ioc"
OBJECT_DECLARE_SIMPLE_TYPE(IA64SBAState, IA64_SBA)

#define TYPE_IA64_SBA_IOMMU_MEMORY_REGION "ia64-sba-iommu-memory-region"

struct IA64SBAState {
    PCIDevice parent_obj;

    MemoryRegion csr;              /* IOC CSR window, mapped at csr_base       */
    IOMMUMemoryRegion iommu;       /* the single translating region            */
    AddressSpace dma_as;           /* returned for every devfn on the bus      */
    QemuRecMutex iommu_lock;       /* serializes translate + register access   */
    HPZX1IOMMUFrontend fe;         /* adopted zx1 IOC frontend (ibase/.../TLB)  */

    uint64_t csr_base;             /* fixed chipset MMIO base (IA64_SBA_CSR_BASE) */
};

/*
 * Route DMA from masters on @bus through the SBA's single shared translated
 * address space too (the IOC translates every master on the platform, not just
 * those on its own PCI bus).  Used by the zx1 machine to bring the Mercury
 * second root bus under the same IOPDIR/GART.  Call before any device is
 * realized on @bus.
 */
void ia64_sba_attach_bus(IA64SBAState *s, PCIBus *bus);

#endif /* HW_IA64_SBA_H */
