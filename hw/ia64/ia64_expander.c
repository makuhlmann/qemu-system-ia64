/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Intel 460GX expander-bridge downstream PCI root bus for the 460gx machine.
 *
 * Registered against the primary host bridge's shared identity-mapped MMIO
 * and I/O windows, so a device BAR on this bus lands in the same aperture the
 * machine already programs.  The bus number is fixed by a bus_num override,
 * and the primary host's ECAM configuration handler dispatches cycles for
 * that bus number here (ia64_pci.c), which is what the real chipset does when
 * the SAC forwards a configuration cycle to the expander that owns the bus.
 *
 * INTx uses the conventional (slot + pin) % 4 swizzle.  Each expander drives
 * its own four GPIO-out lines; the machine wires them to that root's block of
 * PID inputs, so unlike zx1 -- where both roots share one block of four -- the
 * four 460GX roots do not share interrupt lines.
 *
 * Structure follows hw/ia64/ia64_mercury.c, which does the same job for the
 * single zx1 second root.
 */

#include "qemu/osdep.h"
#include "qemu/cutils.h"
#include "hw/ia64/ia64_expander.h"
#include "hw/ia64/ia64_pci.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_host.h"
#include "hw/pci/pci_bus.h"
#include "hw/pci/pci_device.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev.h"
#include "hw/core/irq.h"
#include "qom/object.h"

OBJECT_DECLARE_SIMPLE_TYPE(IA64ExpanderState, IA64_EXPANDER_HOST)

struct IA64ExpanderState {
    PCIHostState parent_obj;

    MemoryRegion *pci_mem;   /* shared with the primary host bridge */
    MemoryRegion *pci_io;    /* shared with the primary host bridge */
    uint8_t first_bus;       /* fixed bus number of this root bus */
    char bus_name[16];
    char bus_path[8];        /* "0000:01" style, stable for the monitor */
    qemu_irq irq[IA64_PCI_INTX_LINES];
};

/* Bus subclass whose bus_num reports the board-assigned first_bus. */
struct IA64ExpanderBus {
    PCIBus parent_obj;
    uint8_t first_bus;
};
typedef struct IA64ExpanderBus IA64ExpanderBus;
DECLARE_INSTANCE_CHECKER(IA64ExpanderBus, IA64_EXPANDER_BUS_OBJ,
                         TYPE_IA64_EXPANDER_BUS)

static int ia64_expander_bus_num(PCIBus *bus)
{
    return IA64_EXPANDER_BUS_OBJ(bus)->first_bus;
}

static void ia64_expander_bus_class_init(ObjectClass *klass, const void *data)
{
    PCIBusClass *pbc = PCI_BUS_CLASS(klass);

    pbc->bus_num = ia64_expander_bus_num;
}

static const TypeInfo ia64_expander_bus_info = {
    .name          = TYPE_IA64_EXPANDER_BUS,
    .parent        = TYPE_PCI_BUS,
    .instance_size = sizeof(IA64ExpanderBus),
    .class_init    = ia64_expander_bus_class_init,
};

static int ia64_expander_map_irq(PCIDevice *pdev, int pin)
{
    return (PCI_SLOT(pdev->devfn) + pin) % IA64_PCI_INTX_LINES;
}

static void ia64_expander_set_irq(void *opaque, int irq_num, int level)
{
    IA64ExpanderState *s = opaque;

    if (irq_num >= 0 && irq_num < IA64_PCI_INTX_LINES) {
        qemu_set_irq(s->irq[irq_num], level);
    }
}

static const char *ia64_expander_root_bus_path(PCIHostState *host,
                                               PCIBus *root_bus)
{
    IA64ExpanderState *s = IA64_EXPANDER_HOST(host);

    return s->bus_path;
}

static void ia64_expander_realize(DeviceState *dev, Error **errp)
{
    IA64ExpanderState *s = IA64_EXPANDER_HOST(dev);
    PCIHostState *host = PCI_HOST_BRIDGE(dev);
    PCIBus *bus;

    if (s->pci_mem == NULL || s->pci_io == NULL) {
        error_setg(errp,
                   "460GX expander root requires shared MMIO/I/O windows");
        return;
    }

    qdev_init_gpio_out(dev, s->irq, IA64_PCI_INTX_LINES);

    bus = pci_register_root_bus(dev, s->bus_name, ia64_expander_set_irq,
                                ia64_expander_map_irq, s, s->pci_mem,
                                s->pci_io, PCI_DEVFN(0, 0),
                                IA64_PCI_INTX_LINES, TYPE_IA64_EXPANDER_BUS);
    IA64_EXPANDER_BUS_OBJ(bus)->first_bus = s->first_bus;
    host->bus = bus;
    snprintf(s->bus_path, sizeof(s->bus_path), "0000:%02x", s->first_bus);
}

static void ia64_expander_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIHostBridgeClass *hc = PCI_HOST_BRIDGE_CLASS(klass);

    dc->realize = ia64_expander_realize;
    dc->user_creatable = false;
    dc->desc = "Intel 460GX expander bridge PCI root";
    hc->root_bus_path = ia64_expander_root_bus_path;
}

static const TypeInfo ia64_expander_info = {
    .name          = TYPE_IA64_EXPANDER_HOST,
    .parent        = TYPE_PCI_HOST_BRIDGE,
    .instance_size = sizeof(IA64ExpanderState),
    .class_init    = ia64_expander_class_init,
};

DeviceState *ia64_expander_host_create(Object *parent, const char *name,
                                       MemoryRegion *mem, MemoryRegion *io,
                                       uint8_t first_bus, Error **errp)
{
    DeviceState *dev = qdev_new(TYPE_IA64_EXPANDER_HOST);
    IA64ExpanderState *s = IA64_EXPANDER_HOST(dev);

    (void)parent;
    s->pci_mem = mem;
    s->pci_io = io;
    s->first_bus = first_bus;
    pstrcpy(s->bus_name, sizeof(s->bus_name), name);
    if (!sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), errp)) {
        return NULL;
    }
    return dev;
}

PCIBus *ia64_expander_host_bus(DeviceState *dev)
{
    return PCI_HOST_BRIDGE(dev)->bus;
}

static void ia64_expander_register_types(void)
{
    type_register_static(&ia64_expander_bus_info);
    type_register_static(&ia64_expander_info);
}

type_init(ia64_expander_register_types)
