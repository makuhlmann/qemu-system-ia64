/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Memory-mapped IPMI BT interface.
 *
 * The BT registers of the HP zx1 board sit in the processor-dependent
 * hardware block, not in I/O space, so the interface needs a sysbus
 * wrapper around the shared BT state machine.
 */

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qapi/error.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "hw/ipmi/ipmi_bt.h"
#include "migration/vmstate.h"
#include "qom/object.h"

OBJECT_DECLARE_SIMPLE_TYPE(IPMIBTMMDevice, IPMI_BT_MM)

struct IPMIBTMMDevice {
    SysBusDevice parent_obj;

    IPMIBT bt;
};

static const VMStateDescription vmstate_IPMIBTMMDevice = {
    .name = TYPE_IPMI_INTERFACE_PREFIX "bt-mm",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT(bt, IPMIBTMMDevice, 1, vmstate_IPMIBT, IPMIBT),
        VMSTATE_END_OF_LIST()
    }
};

static void ipmi_bt_mm_realize(DeviceState *dev, Error **errp)
{
    IPMIBTMMDevice *ibm = IPMI_BT_MM(dev);
    IPMIInterface *ii = IPMI_INTERFACE(dev);
    IPMIInterfaceClass *iic = IPMI_INTERFACE_GET_CLASS(ii);
    Error *err = NULL;

    if (!ibm->bt.bmc) {
        error_setg(errp, "IPMI device requires a bmc attribute to be set");
        return;
    }

    ibm->bt.bmc->intf = ii;
    ibm->bt.opaque = ibm;

    iic->init(ii, 0, &err);
    if (err) {
        error_propagate(errp, err);
        return;
    }

    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &ibm->bt.io);
}

static void ipmi_bt_mm_init(Object *obj)
{
    IPMIBTMMDevice *ibm = IPMI_BT_MM(obj);

    ipmi_bmc_find_and_link(obj, (Object **) &ibm->bt.bmc);
}

static void *ipmi_bt_mm_get_backend_data(IPMIInterface *ii)
{
    IPMIBTMMDevice *ibm = IPMI_BT_MM(ii);

    return &ibm->bt;
}

/*
 * Zero keeps what the interface itself can take, and no retries.  The sizes
 * are what Get BT Interface Capabilities advertises; nothing enforces them,
 * so a response larger than the size given is still delivered.
 */
static const Property ipmi_bt_mm_properties[] = {
    DEFINE_PROP_UINT8("input-size", IPMIBTMMDevice, bt.cap_inmsg, 0),
    DEFINE_PROP_UINT8("output-size", IPMIBTMMDevice, bt.cap_outmsg, 0),
    DEFINE_PROP_UINT8("retries", IPMIBTMMDevice, bt.cap_retries, 0),
};

static void ipmi_bt_mm_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    IPMIInterfaceClass *iic = IPMI_INTERFACE_CLASS(oc);

    dc->realize = ipmi_bt_mm_realize;
    dc->vmsd = &vmstate_IPMIBTMMDevice;
    device_class_set_props(dc, ipmi_bt_mm_properties);

    iic->get_backend_data = ipmi_bt_mm_get_backend_data;
    ipmi_bt_class_init(iic);
}

static const TypeInfo ipmi_bt_mm_info = {
    .name          = TYPE_IPMI_BT_MM,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(IPMIBTMMDevice),
    .instance_init = ipmi_bt_mm_init,
    .class_init    = ipmi_bt_mm_class_init,
    .interfaces = (const InterfaceInfo[]) {
        { TYPE_IPMI_INTERFACE },
        { }
    }
};

static void ipmi_bt_mm_register_types(void)
{
    type_register_static(&ipmi_bt_mm_info);
}

type_init(ipmi_bt_mm_register_types)
