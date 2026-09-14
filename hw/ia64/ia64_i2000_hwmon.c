/*
 * Intel SDV / HP i2000 board hardware monitor
 *
 * The i2000's I/O board carries hardware-monitor chips on the south bridge's
 * SMBus, at addresses 0x2C and 0x4E.  The vendor SDV firmware initialises
 * them during POST: byte-data writes of configuration and limit values to
 * register offsets scattered through the low register file, each one polled
 * to completion through the bridge's SMBus host controller.  A bus with
 * nothing on it fails those transactions (the controller reports a device
 * error rather than completion) and the firmware's completion poll never
 * exits, so the board's monitors have to answer.
 *
 * Which parts they are is not recorded anywhere available, and the firmware
 * only writes to them -- it never reads a sensor value back during the boot
 * this model has to survive.  So this is a register file, not a temperature
 * or voltage model: it acknowledges the byte-data protocol, stores what is
 * written and hands it back, which is enough for the firmware to init the
 * board and move on.  Adding real sensor semantics needs the part numbers.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"

#include "hw/i2c/smbus_slave.h"
#include "hw/ia64/ia64_i2000_hwmon.h"
#include "migration/vmstate.h"
#include "qemu/module.h"

#define IA64_I2000_HWMON_REGS 256

struct IA64I2000HwmonState {
    SMBusDevice parent_obj;

    uint8_t regs[IA64_I2000_HWMON_REGS];
    uint8_t pointer;
};

OBJECT_DECLARE_SIMPLE_TYPE(IA64I2000HwmonState, IA64_I2000_HWMON)

static int ia64_i2000_hwmon_write_data(SMBusDevice *dev, uint8_t *buf,
                                       uint8_t len)
{
    IA64I2000HwmonState *s = IA64_I2000_HWMON(dev);
    uint8_t i;

    if (len == 0) {
        return 0;
    }

    /*
     * The first byte is the register offset, whether this is a write of one
     * or more data bytes or just the pointer set-up half of a read.
     */
    s->pointer = buf[0];
    for (i = 1; i < len; i++) {
        s->regs[s->pointer] = buf[i];
        s->pointer++;
    }
    return 0;
}

static uint8_t ia64_i2000_hwmon_receive_byte(SMBusDevice *dev)
{
    IA64I2000HwmonState *s = IA64_I2000_HWMON(dev);

    /* Monitor register files do not auto-increment on read. */
    return s->regs[s->pointer];
}

static void ia64_i2000_hwmon_reset_hold(Object *obj, ResetType type)
{
    IA64I2000HwmonState *s = IA64_I2000_HWMON(obj);

    memset(s->regs, 0, sizeof(s->regs));
    s->pointer = 0;
}

static const VMStateDescription vmstate_ia64_i2000_hwmon = {
    .name = TYPE_IA64_I2000_HWMON,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_SMBUS_DEVICE(parent_obj, IA64I2000HwmonState),
        VMSTATE_UINT8_ARRAY(regs, IA64I2000HwmonState,
                            IA64_I2000_HWMON_REGS),
        VMSTATE_UINT8(pointer, IA64I2000HwmonState),
        VMSTATE_END_OF_LIST()
    },
};

static void ia64_i2000_hwmon_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    SMBusDeviceClass *sc = SMBUS_DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->desc = "Intel SDV / HP i2000 board hardware monitor";
    dc->vmsd = &vmstate_ia64_i2000_hwmon;
    sc->write_data = ia64_i2000_hwmon_write_data;
    sc->receive_byte = ia64_i2000_hwmon_receive_byte;
    rc->phases.hold = ia64_i2000_hwmon_reset_hold;
    /* Board-internal: created by the machine, not by the user. */
    dc->user_creatable = false;
}

static const TypeInfo ia64_i2000_hwmon_types[] = {
    {
        .name = TYPE_IA64_I2000_HWMON,
        .parent = TYPE_SMBUS_DEVICE,
        .instance_size = sizeof(IA64I2000HwmonState),
        .class_init = ia64_i2000_hwmon_class_init,
    },
};

DEFINE_TYPES(ia64_i2000_hwmon_types)
