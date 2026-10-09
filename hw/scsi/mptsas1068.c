/*
 * QEMU LSI SAS1068 Host Bus Adapter emulation
 *
 * Copyright (c) 2009-2012 Hannes Reinecke, SUSE Labs
 * Copyright (c) 2012 Verizon, Inc.
 * Copyright (c) 2016 Red Hat, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "hw/pci/pci.h"
#include "qemu/module.h"
#include "mptsas.h"

static void mptsas1068_instance_init(Object *obj)
{
    MPTSASState *s = MPT_FUSION(obj);

    s->variant = MPT_FUSION_VARIANT_SAS1068;
}

static void mptsas1068_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    PCIDeviceClass *pc = PCI_DEVICE_CLASS(oc);

    pc->device_id = PCI_DEVICE_ID_LSI_SAS1068;
    pc->revision = 0x01;
    pc->subsystem_id = 0x8000;
    dc->desc = "LSI SAS 1068";
}

static const TypeInfo mptsas1068_info = {
    .name = TYPE_MPTSAS1068,
    .parent = TYPE_MPT_FUSION,
    .instance_init = mptsas1068_instance_init,
    .class_init = mptsas1068_class_init,
};

static void mptsas1068_register_types(void)
{
    type_register_static(&mptsas1068_info);
}

type_init(mptsas1068_register_types)
