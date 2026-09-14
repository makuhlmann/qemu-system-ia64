/*
 * Intel 460GX platform devices that exist in PCI configuration space without
 * software-visible behaviour: the Programmable Interrupt Device's config face
 * and the two Integrated Hot-Plug Controllers.
 *
 * A guest enumerating the compatibility and WXB buses finds these where the
 * i2000 has them.  Neither decodes an address range nor raises an interrupt,
 * so they carry no state beyond the PCI header.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"

#include "hw/ia64/ia64_460gx_identity.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_device.h"
#include "migration/vmstate.h"
#include "qemu/module.h"

static void ia64_identity_class_init_common(ObjectClass *oc,
                                            const char *description)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    PCIDeviceClass *pc = PCI_DEVICE_CLASS(oc);

    pc->vendor_id = PCI_VENDOR_ID_INTEL;
    pc->revision = 0x01;
    dc->desc = description;
    dc->vmsd = &vmstate_pci_device;
    /* Chipset devices, placed by the machine. */
    dc->user_creatable = false;
    dc->hotpluggable = false;
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static void ia64_460gx_pid_realize(PCIDevice *pci, Error **errp)
{
    (void)errp;
    /* The I/O APIC programming interface; PCI spec appendix D. */
    pci_config_set_prog_interface(pci->config, 0x20);
    /*
     * A chipset part carries no subsystem identity unless the silicon
     * hardwires one, so clear what the PCI bus supplies by default.  The
     * hot-plug controller does hardwire one and sets it from its class.
     */
    pci_set_word(pci->config + PCI_SUBSYSTEM_VENDOR_ID, 0);
    pci_set_word(pci->config + PCI_SUBSYSTEM_ID, 0);
}

static void ia64_460gx_pid_class_init(ObjectClass *oc, const void *data)
{
    PCIDeviceClass *pc = PCI_DEVICE_CLASS(oc);

    (void)data;
    ia64_identity_class_init_common(
        oc, "Intel 683053 Programmable Interrupt Device");
    pc->realize = ia64_460gx_pid_realize;
    pc->device_id = 0x123d;
    pc->class_id = PCI_CLASS_SYSTEM_PIC;
}

static void ia64_460gx_ihpc_class_init(ObjectClass *oc, const void *data)
{
    PCIDeviceClass *pc = PCI_DEVICE_CLASS(oc);

    (void)data;
    ia64_identity_class_init_common(
        oc, "Intel 82466GX Integrated Hot-Plug Controller");
    pc->device_id = 0x123f;
    pc->class_id = PCI_CLASS_SYSTEM_PCI_HOTPLUG;
    pc->subsystem_vendor_id = PCI_VENDOR_ID_INTEL;
    pc->subsystem_id = 0x123f;
}

static const TypeInfo ia64_460gx_identity_types[] = {
    {
        .name = TYPE_IA64_460GX_PID,
        .parent = TYPE_PCI_DEVICE,
        .instance_size = sizeof(PCIDevice),
        .class_init = ia64_460gx_pid_class_init,
        .interfaces = (const InterfaceInfo[]) {
            { INTERFACE_CONVENTIONAL_PCI_DEVICE },
            { },
        },
    }, {
        .name = TYPE_IA64_460GX_IHPC,
        .parent = TYPE_PCI_DEVICE,
        .instance_size = sizeof(PCIDevice),
        .class_init = ia64_460gx_ihpc_class_init,
        .interfaces = (const InterfaceInfo[]) {
            { INTERFACE_CONVENTIONAL_PCI_DEVICE },
            { },
        },
    },
};

DEFINE_TYPES(ia64_460gx_identity_types)
