/*
 * QEMU LSI 53C1030 Ultra320 SCSI Host Bus Adapter emulation
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

static void lsi53c1030_instance_init(Object *obj)
{
    MPTSASState *s = MPT_FUSION(obj);

    /* SPI extended configuration pages are not implemented. */
    s->variant = MPT_FUSION_VARIANT_LSI53C1030;
}

static void lsi53c1030_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    PCIDeviceClass *pc = PCI_DEVICE_CLASS(oc);

    pc->device_id = PCI_DEVICE_ID_LSI_53C1030;
    pc->revision = 0x07;
    pc->subsystem_id = 0x1000;
    dc->desc = "LSI 53C1030 Ultra320 SCSI";
}

static const TypeInfo lsi53c1030_info = {
    .name = TYPE_LSI53C1030,
    .parent = TYPE_MPT_FUSION,
    .instance_init = lsi53c1030_instance_init,
    .class_init = lsi53c1030_class_init,
};

static void lsi53c1030_register_types(void)
{
    type_register_static(&lsi53c1030_info);
}

type_init(lsi53c1030_register_types)
