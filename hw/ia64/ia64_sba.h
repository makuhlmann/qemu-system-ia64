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
    /* CSR offsets already reported as unimplemented, one bit per offset. */
    uint64_t bus_config;           /* FED0_9410, kept across a platform reset */
    uint64_t vga_config;           /* FED0_9418, bit 25 = the box has a VGA   */
    uint64_t lba_port[8];          /* LBA_Port(N)_CNTRL, FED0_1200 + 8 * N    */
    uint64_t range[28];            /* address range registers, FED0_0300 on   */
    uint64_t error_control;        /* the IOC's own error log control, 0x0108 */
    void (*window_notify)(void *opaque, uint64_t base);
    void *window_opaque;
    uint64_t window_base;          /* the lowest LMMIO base announced so far  */
    MemoryRegion rope_config;      /* the 16 rope guests, per FED0_03A8       */
    uint64_t rope_base;            /* where it is mapped while enabled        */
    bool rope_mapped;
    unsigned long *unimp_read;
    unsigned long *unimp_write;
};

/*
 * Route DMA from masters on @bus through the SBA's single shared translated
 * address space too (the IOC translates every master on the platform, not just
 * those on its own PCI bus).  Used by the zx1 machine to bring the Mercury
 * second root bus under the same IOPDIR/GART.  Call before any device is
 * realized on @bus.
 */
void ia64_sba_attach_bus(IA64SBAState *s, PCIBus *bus);

/*
 * Put @mr in rope @rope's 8 KiB slot of the rope guest configuration space.
 * The firmware reads the rope guest's identity there to recognise the I/O host
 * bridge below that rope; the window itself follows ROPE_CONFIG_BASE.
 */
void ia64_sba_add_rope(IA64SBAState *s, unsigned int rope, MemoryRegion *mr);

/*
 * Hear where the firmware puts the LMMIO ranges.  The machine opens its PCI
 * MMIO window there: PCI addresses are CPU physical addresses on this
 * platform, so a device whose BAR the firmware sets from a range the machine
 * does not decode is unreachable.
 */
void ia64_sba_set_window_notify(IA64SBAState *s,
                                void (*notify)(void *opaque, uint64_t base),
                                void *opaque);

#endif /* HW_IA64_SBA_H */
