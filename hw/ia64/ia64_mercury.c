/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * HP zx1 Mercury (LBA / ioa) PCI host bridge for the ia64 zx1 machine.
 *
 * A second PCI root bus, registered against the primary host bridge's shared
 * identity-mapped MMIO/I/O windows, so device BARs on it land in the same fixed
 * aperture the machine already programs.  Its bus number is fixed by a bus_num
 * override (adapted from qemu-upstream hw/pci-host/hp-zx1-ioa.c) to
 * IA64_MERCURY_BUS; the primary host's ECAM config handler dispatches config
 * cycles for that bus number here (ia64_pci.c).  INTx uses the same (slot+pin)%4
 * swizzle and the same four IOSAPIC lines as the primary bus, combined by the
 * machine.  All DMA is deferred to the shared SBA IOMMU (ia64_sba_attach_bus).
 */

#include "qemu/osdep.h"
#include "hw/ia64/ia64_mercury.h"
#include "hw/ia64/ia64_pci.h"
#include "hw/ia64/ia64_vpc_abi.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_host.h"
#include "hw/pci/pci_bus.h"
#include "hw/pci/pci_device.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev.h"
#include "hw/core/irq.h"
#include "qom/object.h"

/* The child bus must be decodable through the single segment-0 ECAM window. */
QEMU_BUILD_BUG_ON(IA64_MERCURY_BUS > 0x3f);

OBJECT_DECLARE_SIMPLE_TYPE(IA64MercuryState, IA64_MERCURY_HOST)

struct IA64MercuryState {
    PCIHostState parent_obj;

    MemoryRegion *pci_mem;   /* shared with the primary host bridge */
    MemoryRegion *pci_io;    /* shared with the primary host bridge */
    uint8_t first_bus;       /* fixed bus number of the Mercury root bus */
    char bus_path[8];        /* "0000:10" style, stable for the monitor */
    qemu_irq irq[IA64_PCI_INTX_LINES];
};

/* Bus subclass whose bus_num reports the board-assigned first_bus. */
struct IA64MercuryBus {
    PCIBus parent_obj;
    uint8_t first_bus;
};
typedef struct IA64MercuryBus IA64MercuryBus;
DECLARE_INSTANCE_CHECKER(IA64MercuryBus, IA64_MERCURY_BUS_OBJ, TYPE_IA64_MERCURY_BUS)

static int ia64_mercury_bus_num(PCIBus *bus)
{
    return IA64_MERCURY_BUS_OBJ(bus)->first_bus;
}

static void ia64_mercury_bus_class_init(ObjectClass *klass, const void *data)
{
    PCIBusClass *pbc = PCI_BUS_CLASS(klass);

    pbc->bus_num = ia64_mercury_bus_num;
}

static const TypeInfo ia64_mercury_bus_info = {
    .name          = TYPE_IA64_MERCURY_BUS,
    .parent        = TYPE_PCI_BUS,
    .instance_size = sizeof(IA64MercuryBus),
    .class_init    = ia64_mercury_bus_class_init,
};

/* INTx: same swizzle as the primary bus so the ACPI _PRT/GSI map is shared. */
static int ia64_mercury_map_irq(PCIDevice *pdev, int pin)
{
    return (PCI_SLOT(pdev->devfn) + pin) % IA64_PCI_INTX_LINES;
}

static void ia64_mercury_set_irq(void *opaque, int irq_num, int level)
{
    IA64MercuryState *s = opaque;

    if (irq_num >= 0 && irq_num < IA64_PCI_INTX_LINES) {
        qemu_set_irq(s->irq[irq_num], level);
    }
}

static const char *ia64_mercury_root_bus_path(PCIHostState *host,
                                              PCIBus *root_bus)
{
    IA64MercuryState *s = IA64_MERCURY_HOST(host);

    return s->bus_path;
}

static void ia64_mercury_realize(DeviceState *dev, Error **errp)
{
    IA64MercuryState *s = IA64_MERCURY_HOST(dev);
    PCIHostState *host = PCI_HOST_BRIDGE(dev);
    PCIBus *bus;

    if (s->pci_mem == NULL || s->pci_io == NULL) {
        error_setg(errp, "Mercury host bridge requires shared MMIO/I/O windows");
        return;
    }

    qdev_init_gpio_out(dev, s->irq, IA64_PCI_INTX_LINES);

    bus = pci_register_root_bus(dev, "mercury",
                                ia64_mercury_set_irq, ia64_mercury_map_irq, s,
                                s->pci_mem, s->pci_io, PCI_DEVFN(0, 0),
                                IA64_PCI_INTX_LINES, TYPE_IA64_MERCURY_BUS);
    IA64_MERCURY_BUS_OBJ(bus)->first_bus = s->first_bus;
    host->bus = bus;
    snprintf(s->bus_path, sizeof(s->bus_path), "0000:%02x", s->first_bus);
}

static void ia64_mercury_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIHostBridgeClass *hc = PCI_HOST_BRIDGE_CLASS(klass);

    dc->realize = ia64_mercury_realize;
    dc->user_creatable = false;
    hc->root_bus_path = ia64_mercury_root_bus_path;
}

static const TypeInfo ia64_mercury_info = {
    .name          = TYPE_IA64_MERCURY_HOST,
    .parent        = TYPE_PCI_HOST_BRIDGE,
    .instance_size = sizeof(IA64MercuryState),
    .class_init    = ia64_mercury_class_init,
};

DeviceState *ia64_mercury_host_create(Object *parent, MemoryRegion *mem,
                                      MemoryRegion *io, uint8_t first_bus,
                                      Error **errp)
{
    DeviceState *dev = qdev_new(TYPE_IA64_MERCURY_HOST);
    IA64MercuryState *s = IA64_MERCURY_HOST(dev);

    (void)parent;
    s->pci_mem = mem;
    s->pci_io = io;
    s->first_bus = first_bus;
    if (!sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), errp)) {
        return NULL;
    }
    return dev;
}

PCIBus *ia64_mercury_host_bus(DeviceState *dev)
{
    return PCI_HOST_BRIDGE(dev)->bus;
}

static void ia64_mercury_register_types(void)
{
    type_register_static(&ia64_mercury_bus_info);
    type_register_static(&ia64_mercury_info);
}

type_init(ia64_mercury_register_types)
