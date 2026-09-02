/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_IA64_PCI_H
#define HW_IA64_PCI_H

#include "qemu/typedefs.h"
#include "hw/ia64/ia64_vpc_abi.h"

#define TYPE_IA64_PCI_HOST_BRIDGE "ia64-pcihost"

/* IA64_PCI_IO_* and IA64_PCI_CONFIG_* live in ia64_vpc_abi.h (shared with
 * the firmware). */
#if (IA64_PCI_IO_BASE & (IA64_PCI_IO_SPARSE_SIZE - 1)) != 0
#error "IA64_PCI_IO_BASE must be aligned for sparse I/O port addresses"
#endif

#define IA64_PCI_INTX_GSI_BASE 16
#define IA64_PCI_INTX_LINES    4
/* zx1 registers one secondary root; 460gx registers three expander roots. */
#define IA64_PCI_MAX_SECONDARY_ROOTS 4

int ia64_pci_route_intx_gsi(uint8_t devfn, int irq_num);

/*
 * The shared identity-mapped MMIO/I/O windows of the primary host bridge.  The
 * zx1 Mercury host bridge (hw/ia64/ia64_mercury.c) registers its second root bus
 * against these same regions so device BARs on the Mercury bus land in the same
 * fixed aperture the machine already programs -- see ia64_mercury.c.
 */
MemoryRegion *ia64_pci_host_mmio(DeviceState *pci_host);
MemoryRegion *ia64_pci_host_io(DeviceState *pci_host);

/*
 * Register the Mercury second root bus with the primary host bridge so its ECAM
 * config handler dispatches config cycles for IA64_MERCURY_BUS to that bus (the
 * faithful analog of zx1 SAL rope-routing).  NULL until the zx1 machine wires it.
 */
void ia64_pci_host_set_mercury_bus(DeviceState *pci_host, PCIBus *bus);
/* Register a secondary root bus for ECAM dispatch by its own bus number. */
void ia64_pci_host_add_secondary_bus(DeviceState *pci_host, PCIBus *bus);

#endif
