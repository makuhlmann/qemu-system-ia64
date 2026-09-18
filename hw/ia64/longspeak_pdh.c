/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Longs Peak (HP zx1 board) PDH devices below the flash.
 *
 * The mio sends the firmware space FF00_0000-FFFF_FFFF to the "Dillon" ASIC
 * over the 4-bit PDH bus (mio ERS 2.1).  Dillon decodes the boot flash at
 * the top and, below it, the devices the HP firmware uses from its first
 * instructions.  There is no Dillon ERS in docs/; everything here comes from
 * the firmware's code (HP System Firmware 2.31, zx2000 flash dump), see
 * plans/zx1-real-firmware-reference.md sec 5.3, 6 and 7.3:
 *
 *   FF40_0000  NVM, 256 KiB.  Battery-backed on the board; volatile here
 *              (plans/one-hardware-model-plan.md P6.7).
 *   FF44_0000  SRAM, 768 KiB.  SAL_A's rendezvous record, SAL_B's first
 *              memory stack and RSE backing store.
 *   FF5B_0000  the BMC.  The firmware probes an IPMI BT at FF5B_00E4 and
 *              three IPMI KCS, which it calls KCS1 at FF5B_0CA2 (the
 *              standard SMS base), KCS2 at FF5B_0000 and KCS3 at FF5B_0062.
 *              Only BT and KCS1 are modelled; the firmware needs no more.
 *   FF5B_8000  the clock (plans/nvram-portability.md sec 2.2).
 *   FF5C_0000  processor presence, bits 3:0 active low (SAL_A FFFE0E60).
 *   FF5C_0018  POST byte (SAL_A writes (id << 4) | step).
 *   FF5E_0000  two 16550 UARTs, FF5E_0000 and FF5E_2000 (EFI PDHUART,
 *              PNP0501 in the firmware's device table at FFF8E918).  They
 *              take the second and third -serial chardev.
 *   FF5F_0000  Dillon registers.  0x20 and 0x68 are scratch latches the
 *              processors share (0x68 bits 19:16: check-in, SAL_A sec 5.3;
 *              0x20 bits 7:6: boot mode, FFFE0346); 0xB0 + 8 * id is one
 *              semaphore (below); 0x1010 bit 0 selects mx2 modules.
 *
 * Only these blocks decode.  An offset in a block that is not modelled reads
 * as zero, ignores writes and is reported once per offset and direction under
 * "-d unimp" -- the access log that drives the vendor bring-up.  The firmware
 * polls some registers in long loops, so nothing here logs per access.
 */

#include "qemu/osdep.h"
#include "qemu/bitmap.h"
#include "qemu/bitops.h"
#include "qemu/log.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "hw/core/cpu.h"
#include "hw/core/qdev-properties.h"
#include "hw/char/serial-mm.h"
#include "chardev/char-fe.h"
#include "system/system.h"
#include "hw/ia64/ia64_vpc_abi.h"
#include "hw/ipmi/ipmi.h"
#include "hw/ipmi/ipmi_bt.h"
#include "hw/ipmi/ipmi_kcs.h"
#include "migration/vmstate.h"
#include "system/runstate.h"
#include "longspeak_pdh.h"
#include "trace.h"

static const struct {
    hwaddr base;
    const char *name;
} longspeak_pdh_blocks[LONGSPEAK_PDH_BLOCKS] = {
    [LONGSPEAK_PDH_DEV5B] = { IA64_PDH_DEV5B_BASE, "longspeak-pdh.5b" },
    [LONGSPEAK_PDH_PRESENCE_BLOCK] = { IA64_PDH_PRESENCE_BASE,
                                       "longspeak-pdh.presence" },
    [LONGSPEAK_PDH_UART_BLOCK] = { IA64_PDH_UART_BASE, "longspeak-pdh.uart" },
    [LONGSPEAK_PDH_DILLON_BLOCK] = { IA64_PDH_DILLON_BASE,
                                     "longspeak-pdh.dillon" },
};

static int longspeak_pdh_cpu(void)
{
    return current_cpu ? current_cpu->cpu_index : -1;
}

/* An access inside the 8-byte register at reg, as a byte-lane field. */
static bool longspeak_pdh_in_reg(hwaddr addr, unsigned size, hwaddr reg)
{
    return addr >= reg && addr + size <= reg + 8;
}

static uint64_t longspeak_pdh_reg_read(uint64_t value, hwaddr addr,
                                       unsigned size, hwaddr reg)
{
    return extract64(value, (addr - reg) * 8, size * 8);
}

static void longspeak_pdh_reg_write(uint64_t *value, hwaddr addr,
                                    unsigned size, hwaddr reg, uint64_t data)
{
    *value = deposit64(*value, (addr - reg) * 8, size * 8, data);
}

static bool longspeak_pdh_semaphore_slot(hwaddr addr, unsigned size,
                                         unsigned *id)
{
    hwaddr off = addr - IA64_PDH_DILLON_SEMAPHORE;

    if (size != 1 || addr < IA64_PDH_DILLON_SEMAPHORE || (off & 7) ||
        off / 8 >= IA64_PDH_DILLON_SEMAPHORES) {
        return false;
    }
    *id = off / 8;
    return true;
}

/*
 * One semaphore, reached at FF5F_00B0 + 8 * id.  A read returns bit 0 set and
 * the holder in bits 7:1 while it is held; when it is free the read returns 0
 * and makes the reader's id the holder.  Writing 0 at the holder's slot frees
 * it.  SAL_A FFFE11D0 (acquire: bit 0 clear means "got it", a set bit with
 * its own id means "already held") and FFFE1250 (release: read, then write 0
 * at its own slot); SAL_B FFF3F9F0 frees a semaphore held by another id by
 * writing 0 at the holder's slot.
 */
static uint8_t longspeak_pdh_semaphore_read(LongspeakPDHState *s, unsigned id)
{
    if (s->semaphore & 1) {
        return s->semaphore;
    }
    s->semaphore = 1 | (id << 1);
    trace_longspeak_pdh_semaphore("claimed", id, longspeak_pdh_cpu());
    return 0;
}

static bool longspeak_pdh_semaphore_write(LongspeakPDHState *s, unsigned id,
                                          uint64_t data)
{
    if (data != 0) {
        return false;
    }
    if ((s->semaphore & 1) && (s->semaphore >> 1) == id) {
        s->semaphore = 0;
        trace_longspeak_pdh_semaphore("released", id, longspeak_pdh_cpu());
    }
    return true;
}

/*
 * FF5F_0000 to FF5F_0090 is a file of 8-byte registers: SAL_B walks it with a
 * 0x55 pattern ("found bad miscellaneous register" ends the boot), so every
 * offset stores what is written at any width.  Some carry meaning: 0x20 the
 * boot mode, 0x68 the check-in bits, 0x70 the monarch's LID, and 0x28 + 8n one
 * status byte per processor, indexed by cr.lid{28:24} (FFE79370), not by the
 * semaphore id.
 */
static bool longspeak_pdh_file_index(hwaddr addr, unsigned size, unsigned *idx)
{
    if (addr + size > IA64_PDH_DILLON_REGS * 8 || (addr & 7) + size > 8) {
        return false;
    }
    *idx = addr / 8;
    return true;
}

static const char *longspeak_pdh_reg_name(unsigned idx)
{
    switch (idx * 8) {
    case IA64_PDH_DILLON_SCRATCH0:
        return "scratch0";
    case IA64_PDH_DILLON_CHECKIN:
        return "checkin";
    case IA64_PDH_DILLON_MONARCH:
        return "monarch";
    default:
        return NULL;
    }
}

static uint8_t longspeak_pdh_presence(LongspeakPDHState *s)
{
    return (uint8_t)~MAKE_64BIT_MASK(0, MIN(s->sockets, 4));
}

static bool longspeak_pdh_do_read(LongspeakPDHBlock *b, hwaddr addr,
                                  unsigned size, uint64_t *data)
{
    LongspeakPDHState *s = b->pdh;
    unsigned id;

    switch (b->id) {
    case LONGSPEAK_PDH_PRESENCE_BLOCK:
        if (addr == IA64_PDH_PRESENCE && size == 1) {
            *data = longspeak_pdh_presence(s);
            return true;
        }
        if (addr == IA64_PDH_POST && size == 1) {
            *data = s->post;
            return true;
        }
        return false;
    case LONGSPEAK_PDH_DILLON_BLOCK:
        if (longspeak_pdh_file_index(addr, size, &id)) {
            *data = longspeak_pdh_reg_read(s->reg[id], addr, size, id * 8);
            return true;
        }
        if (longspeak_pdh_semaphore_slot(addr, size, &id)) {
            *data = longspeak_pdh_semaphore_read(s, id);
            return true;
        }
        if (longspeak_pdh_in_reg(addr, size, IA64_PDH_DILLON_CONTROL)) {
            *data = longspeak_pdh_reg_read(s->control, addr, size,
                                           IA64_PDH_DILLON_CONTROL);
            return true;
        }
        if (longspeak_pdh_in_reg(addr, size, IA64_PDH_DILLON_SCRATCH1)) {
            *data = longspeak_pdh_reg_read(s->scratch1, addr, size,
                                           IA64_PDH_DILLON_SCRATCH1);
            return true;
        }
        if (longspeak_pdh_in_reg(addr, size, IA64_PDH_DILLON_MISC)) {
            *data = longspeak_pdh_reg_read(s->misc, addr, size,
                                           IA64_PDH_DILLON_MISC);
            return true;
        }
        if (longspeak_pdh_in_reg(addr, size, IA64_PDH_DILLON_MODULE_LAYOUT)) {
            /* One processor module per socket, no mx2 (SAL_A forces 0). */
            *data = 0;
            return true;
        }
        return false;
    default:
        return false;
    }
}

static bool longspeak_pdh_do_write(LongspeakPDHBlock *b, hwaddr addr,
                                   unsigned size, uint64_t data)
{
    LongspeakPDHState *s = b->pdh;
    unsigned id;

    switch (b->id) {
    case LONGSPEAK_PDH_PRESENCE_BLOCK:
        if (addr == IA64_PDH_POST && size == 1) {
            if (s->post != (uint8_t)data) {
                s->post = data;
                trace_longspeak_pdh_post(s->post, longspeak_pdh_cpu());
            }
            return true;
        }
        return false;
    case LONGSPEAK_PDH_DILLON_BLOCK:
        if (longspeak_pdh_file_index(addr, size, &id)) {
            uint64_t old = s->reg[id];
            const char *name;

            longspeak_pdh_reg_write(&s->reg[id], addr, size, id * 8, data);
            if (s->reg[id] == old) {
                return true;
            }
            name = longspeak_pdh_reg_name(id);
            if (name != NULL) {
                trace_longspeak_pdh_register(name, s->reg[id],
                                             longspeak_pdh_cpu());
            } else if (addr >= IA64_PDH_DILLON_STATUS &&
                       addr < IA64_PDH_DILLON_STATUS +
                              8 * IA64_PDH_DILLON_STATUSES) {
                trace_longspeak_pdh_status(
                    (id * 8 - IA64_PDH_DILLON_STATUS) / 8,
                    s->reg[id] & 0xff, longspeak_pdh_cpu());
            }
            return true;
        }
        if (longspeak_pdh_in_reg(addr, size, IA64_PDH_DILLON_CONTROL)) {
            longspeak_pdh_reg_write(&s->control, addr, size,
                                    IA64_PDH_DILLON_CONTROL, data);
            trace_longspeak_pdh_register("control", s->control,
                                         longspeak_pdh_cpu());
            if ((s->control & IA64_PDH_DILLON_RESET) ==
                IA64_PDH_DILLON_RESET) {
                qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
            }
            return true;
        }
        if (longspeak_pdh_in_reg(addr, size, IA64_PDH_DILLON_SCRATCH1)) {
            uint64_t old = s->scratch1;

            longspeak_pdh_reg_write(&s->scratch1, addr, size,
                                    IA64_PDH_DILLON_SCRATCH1, data);
            if (s->scratch1 != old) {
                trace_longspeak_pdh_register("scratch1", s->scratch1,
                                             longspeak_pdh_cpu());
            }
            return true;
        }
        if (longspeak_pdh_in_reg(addr, size, IA64_PDH_DILLON_MISC)) {
            longspeak_pdh_reg_write(&s->misc, addr, size,
                                    IA64_PDH_DILLON_MISC, data);
            return true;
        }
        if (longspeak_pdh_semaphore_slot(addr, size, &id)) {
            return longspeak_pdh_semaphore_write(s, id, data);
        }
        return false;
    default:
        return false;
    }
}

static uint64_t longspeak_pdh_read(void *opaque, hwaddr addr, unsigned size)
{
    LongspeakPDHBlock *b = opaque;
    uint64_t data = 0;

    if (!longspeak_pdh_do_read(b, addr, size, &data) &&
        !test_and_set_bit(addr, b->unimp_read)) {
        qemu_log_mask(LOG_UNIMP, "longspeak-pdh: unimplemented read at 0x%"
                      HWADDR_PRIx " (size %u)\n", b->base + addr, size);
    }
    return data;
}

static void longspeak_pdh_write(void *opaque, hwaddr addr, uint64_t data,
                                unsigned size)
{
    LongspeakPDHBlock *b = opaque;

    if (!longspeak_pdh_do_write(b, addr, size, data) &&
        !test_and_set_bit(addr, b->unimp_write)) {
        qemu_log_mask(LOG_UNIMP, "longspeak-pdh: unimplemented write at 0x%"
                      HWADDR_PRIx " (size %u) value 0x%" PRIx64 "\n",
                      b->base + addr, size, data);
    }
}

static const MemoryRegionOps longspeak_pdh_ops = {
    .read = longspeak_pdh_read,
    .write = longspeak_pdh_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8, .unaligned = true },
    .impl = { .min_access_size = 1, .max_access_size = 8, .unaligned = true },
};

/*
 * The board has one BMC on two interfaces, but the IPMI core links a BMC to a
 * single interface, so each one gets its own.
 */
static DeviceState *longspeak_pdh_bmc_port(LongspeakPDHState *s,
                                           const char *type, const char *name,
                                           hwaddr offset, Error **errp)
{
    DeviceState *bmc = qdev_new(TYPE_LONGSPEAK_BMC);
    DeviceState *port = qdev_new(type);
    g_autofree char *bmc_name = g_strdup_printf("%s-bmc", name);

    /*
     * The firmware will not put a real command on a BT whose capabilities
     * report no retries, and it sizes a request from the buffer size it is
     * told: at 255 it asks for more than it can take back and reports
     * "BT_RECEIVE_NEXT: data overflow!".
     */
    if (object_dynamic_cast(OBJECT(port), TYPE_IPMI_BT_MM)) {
        qdev_prop_set_uint8(port, "input-size", IA64_PDH_BMC_BT_BUFFER);
        qdev_prop_set_uint8(port, "output-size", IA64_PDH_BMC_BT_BUFFER);
        qdev_prop_set_uint8(port, "retries", IA64_PDH_BMC_BT_RETRIES);
    }
    object_property_add_child(OBJECT(s), bmc_name, OBJECT(bmc));
    object_property_add_child(OBJECT(s), name, OBJECT(port));
    if (!qdev_realize_and_unref(bmc, NULL, errp)) {
        return NULL;
    }
    object_property_set_link(OBJECT(port), "bmc", OBJECT(bmc), &error_abort);
    if (!sysbus_realize_and_unref(SYS_BUS_DEVICE(port), errp)) {
        return NULL;
    }
    memory_region_add_subregion_overlap(
        &s->block[LONGSPEAK_PDH_DEV5B].container, offset,
        sysbus_mmio_get_region(SYS_BUS_DEVICE(port), 0), 1);
    return port;
}

static void longspeak_pdh_realize(DeviceState *dev, Error **errp)
{
    LongspeakPDHState *s = LONGSPEAK_PDH(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);
    int i;

    if (s->sockets == 0) {
        error_setg(errp, "longspeak-pdh: sockets must be at least 1");
        return;
    }
    if (!memory_region_init_ram(&s->nvm, OBJECT(dev), "longspeak-pdh.nvm",
                                IA64_PDH_NVM_SIZE, errp) ||
        !memory_region_init_ram(&s->sram, OBJECT(dev), "longspeak-pdh.sram",
                                IA64_PDH_SRAM_SIZE, errp)) {
        return;
    }
    sysbus_init_mmio(sbd, &s->nvm);
    sysbus_init_mmio(sbd, &s->sram);
    for (i = 0; i < LONGSPEAK_PDH_BLOCKS; i++) {
        LongspeakPDHBlock *b = &s->block[i];

        b->pdh = s;
        b->id = i;
        b->base = longspeak_pdh_blocks[i].base;
        b->unimp_read = bitmap_new(IA64_PDH_BLOCK_SIZE);
        b->unimp_write = bitmap_new(IA64_PDH_BLOCK_SIZE);
        memory_region_init(&b->container, OBJECT(dev),
                           longspeak_pdh_blocks[i].name,
                           IA64_PDH_BLOCK_SIZE);
        memory_region_init_io(&b->mr, OBJECT(dev), &longspeak_pdh_ops, b,
                              longspeak_pdh_blocks[i].name,
                              IA64_PDH_BLOCK_SIZE);
        memory_region_add_subregion(&b->container, 0, &b->mr);
        sysbus_init_mmio(sbd, &b->container);
    }

    /*
     * The UARTs and the BT overlay the log-only background, so an undecoded
     * offset is still reported.  Their interrupt wiring is unknown; the
     * firmware polls both.
     */
    for (i = 0; i < IA64_PDH_UARTS; i++) {
        LongspeakPDHBlock *b = &s->block[LONGSPEAK_PDH_UART_BLOCK];
        DeviceState *uart = qdev_new(TYPE_SERIAL_MM);

        qdev_prop_set_uint8(uart, "regshift", 0);
        qdev_prop_set_uint32(uart, "baudbase", 115200);
        qdev_prop_set_chr(uart, "chardev", serial_hd(i + 1));
        qdev_prop_set_uint8(uart, "endianness", DEVICE_LITTLE_ENDIAN);
        if (!sysbus_realize_and_unref(SYS_BUS_DEVICE(uart), errp)) {
            return;
        }
        s->uart[i] = uart;
        memory_region_add_subregion_overlap(
            &b->container, i * IA64_PDH_UART_STRIDE,
            sysbus_mmio_get_region(SYS_BUS_DEVICE(uart), 0), 1);
    }

    s->bt = longspeak_pdh_bmc_port(s, TYPE_IPMI_BT_MM, "bt",
                                   IA64_PDH_BMC_BT, errp);
    if (s->bt == NULL) {
        return;
    }
    s->kcs = longspeak_pdh_bmc_port(s, TYPE_IPMI_KCS_MM, "kcs",
                                    IA64_PDH_BMC_KCS, errp);
}

static void longspeak_pdh_unrealize(DeviceState *dev)
{
    LongspeakPDHState *s = LONGSPEAK_PDH(dev);
    int i;

    for (i = 0; i < LONGSPEAK_PDH_BLOCKS; i++) {
        g_free(s->block[i].unimp_read);
        g_free(s->block[i].unimp_write);
    }
}

/*
 * The rendezvous state is cleared, the chipset registers are not: SAL_B
 * leaves its boot mode in the scratch byte (FFE79060) before it resets the
 * box, and SAL_A reads that byte in its first instructions (FFFE0346).  The
 * NVM and the SRAM keep their contents.
 */
static void longspeak_pdh_reset(DeviceState *dev)
{
    LongspeakPDHState *s = LONGSPEAK_PDH(dev);

    s->post = 0;
    s->semaphore = 0;
    s->reg[IA64_PDH_DILLON_CHECKIN / 8] = 0;
    s->reg[IA64_PDH_DILLON_MONARCH / 8] = 0;
    memset(&s->reg[IA64_PDH_DILLON_STATUS / 8], 0,
           IA64_PDH_DILLON_STATUSES * sizeof(s->reg[0]));
}

static const VMStateDescription vmstate_longspeak_pdh = {
    .name = TYPE_LONGSPEAK_PDH,
    .version_id = 2,
    .minimum_version_id = 2,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8(post, LongspeakPDHState),
        VMSTATE_UINT8(semaphore, LongspeakPDHState),
        VMSTATE_UINT64_ARRAY(reg, LongspeakPDHState, IA64_PDH_DILLON_REGS),
        VMSTATE_UINT64(control, LongspeakPDHState),
        VMSTATE_UINT64(scratch1, LongspeakPDHState),
        VMSTATE_UINT64(misc, LongspeakPDHState),
        VMSTATE_END_OF_LIST()
    },
};

static const Property longspeak_pdh_properties[] = {
    DEFINE_PROP_UINT32("sockets", LongspeakPDHState, sockets, 1),
};

static void longspeak_pdh_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    (void)data;
    dc->realize = longspeak_pdh_realize;
    dc->unrealize = longspeak_pdh_unrealize;
    dc->desc = "HP Longs Peak PDH devices (Dillon)";
    dc->vmsd = &vmstate_longspeak_pdh;
    device_class_set_legacy_reset(dc, longspeak_pdh_reset);
    device_class_set_props(dc, longspeak_pdh_properties);
    dc->user_creatable = false;
}

static const TypeInfo longspeak_pdh_info = {
    .name          = TYPE_LONGSPEAK_PDH,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(LongspeakPDHState),
    .class_init    = longspeak_pdh_class_init,
};

static void longspeak_pdh_register_types(void)
{
    type_register_static(&longspeak_pdh_info);
}

type_init(longspeak_pdh_register_types)
