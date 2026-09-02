/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Intel 460GX expander-bridge downstream PCI root bus.
 *
 * The 460GX reaches its PCI buses through expander bridges hanging off the
 * System Address Controller: the PXB carries the compatibility bus, two WXBs
 * carry the 64-bit PCI buses, and the GXB carries AGP.  Each presents its own
 * root bus with its own bus number and its own block of four INTx inputs on
 * the Programmable Interrupt Device, which is why the i2000 needs a 64-input
 * controller (see plans/460gx-i2000-fidelity-plan.md).
 *
 * This models one such downstream root bus.  The bridge's own configuration
 * registers live on the chipset's bus CBN and are answered separately (the
 * realfw configuration space in ia64_vpc.c); a guest sees only the root bus
 * presented here, exactly as it sees the compatibility bus today.
 */

#ifndef HW_IA64_EXPANDER_H
#define HW_IA64_EXPANDER_H

#include "qemu/typedefs.h"
#include "qapi/error.h"

#define TYPE_IA64_EXPANDER_HOST "ia64-460gx-expander-host"
#define TYPE_IA64_EXPANDER_BUS  "ia64-460gx-expander-bus"

/*
 * Create, configure and realize an expander root bridge as a child of
 * @parent, registering its root bus at bus number @first_bus against the
 * shared @mem/@io windows of the primary host bridge.  @name distinguishes
 * the child bus and the monitor path.  Returns the device, or NULL with
 * @errp set.
 */
DeviceState *ia64_expander_host_create(Object *parent, const char *name,
                                       MemoryRegion *mem, MemoryRegion *io,
                                       uint8_t first_bus, Error **errp);

/* The expander's root bus. */
PCIBus *ia64_expander_host_bus(DeviceState *dev);

#endif /* HW_IA64_EXPANDER_H */
