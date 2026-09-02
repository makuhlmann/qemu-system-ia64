/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * HP zx1 Mercury (LBA / ioa) PCI host bridge for the ia64 zx1 machine.
 *
 * Real zx1 exposes the AGP graphics adapter behind the Mercury "Ropes to
 * AGP/PCI/PCI-X bridge": a PCI host bridge presenting its own root bus.  This
 * models that second root bus so Windows pci.sys (which owns the ACPI HWP0003 /
 * PNP0A03 node per HpAgp.inf) enumerates the graphics on Mercury's child bus and
 * the hpagp AgpMercury miniport binds it.  DMA from masters on this bus is
 * translated by the shared SBA IOMMU (ia64_sba_attach_bus); the Mercury CSR /
 * AGP-capability register block itself lives in the separate ia64_lba.c MMIO
 * device (drivers read it as MMIO via the ACPI _CRS, never PCI config space).
 */

#ifndef HW_IA64_MERCURY_H
#define HW_IA64_MERCURY_H

#include "qemu/typedefs.h"
#include "qapi/error.h"

#define TYPE_IA64_MERCURY_HOST "ia64-mercury-host"
#define TYPE_IA64_MERCURY_BUS  "ia64-mercury-bus"

/*
 * Create, configure and realize the Mercury host bridge as a child of @parent,
 * registering its root bus at bus number @first_bus against the shared @mem/@io
 * windows of the primary host bridge.  Returns the created device (its root bus
 * is the "mercury" child bus) or NULL with @errp set.
 */
DeviceState *ia64_mercury_host_create(Object *parent, MemoryRegion *mem,
                                      MemoryRegion *io, uint8_t first_bus,
                                      Error **errp);

/* The Mercury root bus (the "mercury" child bus of the created host). */
PCIBus *ia64_mercury_host_bus(DeviceState *dev);

#endif /* HW_IA64_MERCURY_H */
