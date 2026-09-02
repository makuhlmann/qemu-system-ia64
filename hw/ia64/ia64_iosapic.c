/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Minimal IA-64 I/O SAPIC device model.
 * Routes 24 external interrupt pins to the CPU Local SAPIC.
 */

#include "qemu/osdep.h"
#include "hw/core/qdev-properties.h"
#include "qapi/error.h"
#include "hw/ia64/ia64_iosapic.h"
#include "cpu.h"
#include "migration/vmstate.h"
#include "system/address-spaces.h"

#define IOSAPIC_IOREGSEL   0x00
#define IOSAPIC_IOWIN      0x10
#define IOSAPIC_EOI        0x40

#define IOSAPIC_REG_ID     0x00
#define IOSAPIC_REG_VER    0x01
#define IOSAPIC_RTE_BASE   0x10

#define RTE_VECTOR_MASK      0x00000000000000FFULL
#define RTE_DELIVERY_MODE    0x0000000000000700ULL
#define RTE_DELIVERY_STATUS  0x0000000000001000ULL
#define RTE_REMOTE_IRR       0x0000000000004000ULL
#define RTE_MASKED           0x0000000000010000ULL
#define RTE_TRIGGER_LEVEL    0x0000000000008000ULL
#define RTE_RO_BITS          (RTE_DELIVERY_STATUS | RTE_REMOTE_IRR)

#define IOSAPIC_DELIVERY_FIXED  0
#define IOSAPIC_DELIVERY_LOWEST 1
#define IOSAPIC_DELIVERY_NMI    4
#define IOSAPIC_DELIVERY_EXTINT 7

struct IA64IOSapicState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    uint64_t rte[IA64_IOSAPIC_MAX_PINS];
    uint8_t  irq_level[IA64_IOSAPIC_MAX_PINS];
    uint32_t reg_select;
    uint32_t num_pins;
    uint32_t version;
};

static void iosapic_update(IA64IOSapicState *s, int pin)
{
    uint64_t rte = s->rte[pin];
    unsigned delivery = (rte & RTE_DELIVERY_MODE) >> 8;
    uint8_t id = rte >> 56;
    uint8_t eid = rte >> 48;
    uint8_t vector;
    bool masked = (rte & RTE_MASKED) != 0;
    bool level_triggered = (rte & RTE_TRIGGER_LEVEL) != 0;
    CPUState *cs;

    if (masked || (level_triggered && !s->irq_level[pin])) {
        return;
    }

    switch (delivery) {
    case IOSAPIC_DELIVERY_FIXED:
    case IOSAPIC_DELIVERY_LOWEST:
        /*
         * Lowest-priority delivery is a redirection hint.  A platform may
         * ignore the hint and deliver to the valid ID/EID programmed in the
         * RTE.  The firmware advertises no external interrupt redirection,
         * so use that architected fallback.
         */
        vector = rte & RTE_VECTOR_MASK;
        if (!ia64_external_interrupt_vector_valid(vector)) {
            return;
        }
        break;
    case IOSAPIC_DELIVERY_NMI:
        vector = 2;
        break;
    case IOSAPIC_DELIVERY_EXTINT:
        vector = 0;
        break;
    default:
        return;
    }

    cs = ia64_cpu_by_sapic_id(id, eid);
    if (!cs) {
        return;
    }

    if (level_triggered) {
        if (rte & RTE_REMOTE_IRR) {
            return;
        }
        s->rte[pin] |= RTE_REMOTE_IRR;
    }

    ia64_sapic_set_irq(cs, vector);
}

static void iosapic_fix_edge_remote_irr(IA64IOSapicState *s, int pin)
{
    if (!(s->rte[pin] & RTE_TRIGGER_LEVEL)) {
        s->rte[pin] &= ~RTE_REMOTE_IRR;
    }
}

static void iosapic_rte_write(IA64IOSapicState *s, int pin, uint32_t val,
                              bool high)
{
    uint64_t ro_bits = s->rte[pin] & RTE_RO_BITS;

    if (high) {
        s->rte[pin] = (s->rte[pin] & 0xFFFFFFFFULL) | ((uint64_t)val << 32);
    } else {
        s->rte[pin] = (s->rte[pin] & 0xFFFFFFFF00000000ULL) | val;
    }

    s->rte[pin] = (s->rte[pin] & ~RTE_RO_BITS) | ro_bits;
    iosapic_fix_edge_remote_irr(s, pin);
    /*
     * A redirection-table write is not an interrupt request.  A level
     * route must be re-evaluated -- unmasking or rerouting an entry whose
     * input is asserted delivers it -- but an edge route requests service
     * only on an inactive-to-active transition of the input (460GX SSDM
     * 248704-001 sec 2.6.2, redirection table).  Windows rewrites RTEs
     * continually during PnP; delivering a vector for each rewrite of an
     * unmasked edge entry injected interrupts no device had raised.
     */
    if (s->rte[pin] & RTE_TRIGGER_LEVEL) {
        iosapic_update(s, pin);
    }
}

static void iosapic_eoi(IA64IOSapicState *s, uint8_t vector)
{
    unsigned pin;

    /*
     * An EOI write clears Remote IRR on *every* redirection entry whose
     * vector matches, not just the first: "The PID will compare this vector
     * value with the vector field of each entry in the RT.  When a match is
     * found, the RIRR bit for that entry will be cleared... If multiple
     * redirection entries assign the same vector for more than one interrupt
     * pin, each of those pins will be resampled and new interrupt messages
     * issued for those that are still asserted" (Intel 460GX Chipset System
     * Software Developer's Manual, 248704-001, sec 2.6.2.2, (x)APIC EOI
     * Register).
     *
     * Stopping at the first match strands the other entries with Remote IRR
     * set, which silences those pins permanently.
     */
    for (pin = 0; pin < IA64_IOSAPIC_MAX_PINS; pin++) {
        if ((s->rte[pin] & RTE_VECTOR_MASK) != vector ||
            !(s->rte[pin] & RTE_TRIGGER_LEVEL)) {
            continue;
        }
        if (!(s->rte[pin] & RTE_REMOTE_IRR)) {
            continue;
        }
        s->rte[pin] &= ~RTE_REMOTE_IRR;
        iosapic_update(s, pin);
    }
}

static void iosapic_irq_handler(void *opaque, int pin, int level)
{
    IA64IOSapicState *s = opaque;

    if (pin < 0 || pin >= (int)s->num_pins) {
        return;
    }

    /*
     * Deliver level routes while the input is asserted, edge routes only
     * on the 0->1 transition: a redundant assert of an already-high line
     * is not a new edge.
     */
    bool old_level = s->irq_level[pin] != 0;

    level = level != 0;
    s->irq_level[pin] = (uint8_t)level;
    if (level &&
        ((s->rte[pin] & RTE_TRIGGER_LEVEL) != 0 || !old_level)) {
        iosapic_update(s, pin);
    }
}

static uint64_t iosapic_read(void *opaque, hwaddr addr, unsigned size)
{
    IA64IOSapicState *s = opaque;
    uint32_t result = 0;
    uint32_t index;

    switch (addr) {
    case IOSAPIC_IOREGSEL:
        result = s->reg_select;
        break;
    case IOSAPIC_IOWIN:
        index = s->reg_select;
        if (index == IOSAPIC_REG_ID) {
            result = 0;
        } else if (index == IOSAPIC_REG_VER) {
            result = ((s->num_pins - 1) << 16) | s->version;
        } else if (index >= IOSAPIC_RTE_BASE &&
                   index < IOSAPIC_RTE_BASE + s->num_pins * 2) {
            int pin = (index - IOSAPIC_RTE_BASE) / 2;
            if ((index - IOSAPIC_RTE_BASE) & 1) {
                result = (uint32_t)(s->rte[pin] >> 32);
            } else {
                result = (uint32_t)s->rte[pin];
            }
        }
        break;
    case IOSAPIC_EOI:
        break;
    default:
        break;
    }
    return result;
}

static void iosapic_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    IA64IOSapicState *s = opaque;
    uint32_t index;

    switch (addr) {
    case IOSAPIC_IOREGSEL:
        s->reg_select = (uint32_t)val;
        break;
    case IOSAPIC_IOWIN:
        index = s->reg_select;
        if (index == IOSAPIC_REG_ID) {
            break;
        } else if (index >= IOSAPIC_RTE_BASE &&
                   index < IOSAPIC_RTE_BASE + s->num_pins * 2) {
            int pin = (index - IOSAPIC_RTE_BASE) / 2;
            iosapic_rte_write(s, pin, (uint32_t)val,
                              (index - IOSAPIC_RTE_BASE) & 1);
        }
        break;
    case IOSAPIC_EOI:
        iosapic_eoi(s, (uint8_t)val);
        break;
    default:
        break;
    }
}

static const MemoryRegionOps iosapic_ops = {
    .read = iosapic_read,
    .write = iosapic_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static const Property iosapic_properties[] = {
    DEFINE_PROP_UINT32("num-pins", IA64IOSapicState, num_pins,
                       IA64_IOSAPIC_NUM_PINS),
    DEFINE_PROP_UINT32("version", IA64IOSapicState, version,
                       IA64_IOSAPIC_VERSION),
};

static void iosapic_realize(DeviceState *dev, Error **errp)
{
    IA64IOSapicState *s = IA64_IOSAPIC(dev);

    if (s->num_pins == 0 || s->num_pins > IA64_IOSAPIC_MAX_PINS) {
        error_setg(errp, "num-pins must be between 1 and %d",
                   IA64_IOSAPIC_MAX_PINS);
        return;
    }
    qdev_init_gpio_in(dev, iosapic_irq_handler, s->num_pins);
    memory_region_init_io(&s->mmio, OBJECT(dev), &iosapic_ops, s,
                          "iosapic", 0x2000);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mmio);
}

static void iosapic_reset(DeviceState *dev)
{
    IA64IOSapicState *s = IA64_IOSAPIC(dev);
    int i;

    memset(s->rte, 0, sizeof(s->rte));
    memset(s->irq_level, 0, sizeof(s->irq_level));
    for (i = 0; i < IA64_IOSAPIC_MAX_PINS; i++) {
        s->rte[i] = RTE_MASKED;
    }
    s->reg_select = 0;
}

static int iosapic_post_load(void *opaque, int version_id)
{
    IA64IOSapicState *s = opaque;
    unsigned int pin;

    /*
     * Edge inputs are historical events and must not be replayed merely
     * because their input wire was high at the snapshot boundary.  An
     * asserted level input, however, must be re-evaluated if it did not
     * already have Remote IRR set.
     */
    for (pin = 0; pin < s->num_pins; pin++) {
        if (s->rte[pin] & RTE_TRIGGER_LEVEL) {
            iosapic_update(s, pin);
        }
    }
    return 0;
}

static const VMStateDescription vmstate_ia64_iosapic = {
    .name = "ia64-iosapic",
    .version_id = 2,
    .minimum_version_id = 2,
    .post_load = iosapic_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT64_ARRAY(rte, IA64IOSapicState,
                             IA64_IOSAPIC_MAX_PINS),
        VMSTATE_UINT8_ARRAY(irq_level, IA64IOSapicState,
                            IA64_IOSAPIC_MAX_PINS),
        VMSTATE_UINT32(reg_select, IA64IOSapicState),
        VMSTATE_END_OF_LIST()
    }
};

static void iosapic_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = iosapic_realize;
    device_class_set_legacy_reset(dc, iosapic_reset);
    dc->vmsd = &vmstate_ia64_iosapic;
    device_class_set_props(dc, iosapic_properties);
}

static const TypeInfo iosapic_info = {
    .name          = TYPE_IA64_IOSAPIC,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(IA64IOSapicState),
    .class_init    = iosapic_class_init,
};

static void iosapic_register_types(void)
{
    type_register_static(&iosapic_info);
}
type_init(iosapic_register_types);
