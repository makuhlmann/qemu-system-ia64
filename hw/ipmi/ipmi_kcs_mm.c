/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Memory-mapped IPMI KCS interface.
 *
 * The KCS registers of the HP zx1 board sit in the processor-dependent
 * hardware block, not in I/O space, so the interface needs a sysbus
 * wrapper around the shared KCS state machine.
 */

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qapi/error.h"
#include "hw/core/sysbus.h"
#include "hw/ipmi/ipmi_kcs.h"
#include "migration/vmstate.h"
#include "qom/object.h"

OBJECT_DECLARE_SIMPLE_TYPE(IPMIKCSMMDevice, IPMI_KCS_MM)

struct IPMIKCSMMDevice {
    SysBusDevice parent_obj;

    IPMIKCS kcs;
};

static const VMStateDescription vmstate_IPMIKCSMMDevice = {
    .name = TYPE_IPMI_INTERFACE_PREFIX "kcs-mm",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT(kcs, IPMIKCSMMDevice, 1, vmstate_IPMIKCS, IPMIKCS),
        VMSTATE_END_OF_LIST()
    }
};

static void ipmi_kcs_mm_realize(DeviceState *dev, Error **errp)
{
    IPMIKCSMMDevice *ikm = IPMI_KCS_MM(dev);
    IPMIInterface *ii = IPMI_INTERFACE(dev);
    IPMIInterfaceClass *iic = IPMI_INTERFACE_GET_CLASS(ii);
    Error *err = NULL;

    if (!ikm->kcs.bmc) {
        error_setg(errp, "IPMI device requires a bmc attribute to be set");
        return;
    }

    ikm->kcs.bmc->intf = ii;
    ikm->kcs.opaque = ikm;

    iic->init(ii, 0, &err);
    if (err) {
        error_propagate(errp, err);
        return;
    }

    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &ikm->kcs.io);
}

static void ipmi_kcs_mm_init(Object *obj)
{
    IPMIKCSMMDevice *ikm = IPMI_KCS_MM(obj);

    ipmi_bmc_find_and_link(obj, (Object **) &ikm->kcs.bmc);
}

static void *ipmi_kcs_mm_get_backend_data(IPMIInterface *ii)
{
    IPMIKCSMMDevice *ikm = IPMI_KCS_MM(ii);

    return &ikm->kcs;
}

static void ipmi_kcs_mm_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    IPMIInterfaceClass *iic = IPMI_INTERFACE_CLASS(oc);

    dc->realize = ipmi_kcs_mm_realize;
    dc->vmsd = &vmstate_IPMIKCSMMDevice;

    iic->get_backend_data = ipmi_kcs_mm_get_backend_data;
    ipmi_kcs_class_init(iic);
}

static const TypeInfo ipmi_kcs_mm_info = {
    .name          = TYPE_IPMI_KCS_MM,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(IPMIKCSMMDevice),
    .instance_init = ipmi_kcs_mm_init,
    .class_init    = ipmi_kcs_mm_class_init,
    .interfaces = (const InterfaceInfo[]) {
        { TYPE_IPMI_INTERFACE },
        { }
    }
};

static void ipmi_kcs_mm_register_types(void)
{
    type_register_static(&ipmi_kcs_mm_info);
}

type_init(ipmi_kcs_mm_register_types)
