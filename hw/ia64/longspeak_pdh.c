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
 *   FF5B_0000  an unidentified device; FF5B_8000 is the clock
 *              (plans/nvram-portability.md sec 2.2).  Not modelled yet.
 *   FF5C_0000  processor presence, bits 3:0 active low (SAL_A FFFE0E60).
 *   FF5C_0018  POST byte (SAL_A writes (id << 4) | step).
 *   FF5E_0000  two 16550 UARTs, FF5E_0000 and FF5E_2000.  Not modelled yet.
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
#include "hw/ia64/ia64_vpc_abi.h"
#include "migration/vmstate.h"
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
        if (longspeak_pdh_in_reg(addr, size, IA64_PDH_DILLON_SCRATCH0)) {
            *data = longspeak_pdh_reg_read(s->scratch0, addr, size,
                                           IA64_PDH_DILLON_SCRATCH0);
            return true;
        }
        if (longspeak_pdh_in_reg(addr, size, IA64_PDH_DILLON_CHECKIN)) {
            *data = longspeak_pdh_reg_read(s->checkin, addr, size,
                                           IA64_PDH_DILLON_CHECKIN);
            return true;
        }
        if (longspeak_pdh_semaphore_slot(addr, size, &id)) {
            *data = longspeak_pdh_semaphore_read(s, id);
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
        if (longspeak_pdh_in_reg(addr, size, IA64_PDH_DILLON_SCRATCH0)) {
            uint64_t old = s->scratch0;

            longspeak_pdh_reg_write(&s->scratch0, addr, size,
                                    IA64_PDH_DILLON_SCRATCH0, data);
            if (s->scratch0 != old) {
                trace_longspeak_pdh_register("scratch0", s->scratch0,
                                             longspeak_pdh_cpu());
            }
            return true;
        }
        if (longspeak_pdh_in_reg(addr, size, IA64_PDH_DILLON_CHECKIN)) {
            uint64_t old = s->checkin;

            longspeak_pdh_reg_write(&s->checkin, addr, size,
                                    IA64_PDH_DILLON_CHECKIN, data);
            if (s->checkin != old) {
                trace_longspeak_pdh_register("checkin", s->checkin,
                                             longspeak_pdh_cpu());
            }
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
        memory_region_init_io(&b->mr, OBJECT(dev), &longspeak_pdh_ops, b,
                              longspeak_pdh_blocks[i].name,
                              IA64_PDH_BLOCK_SIZE);
        sysbus_init_mmio(sbd, &b->mr);
    }
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
 * A system reset clears the POST byte, the latches and the semaphore, so each
 * boot's rendezvous starts with no processor checked in.  The NVM and the
 * SRAM keep their contents.
 */
static void longspeak_pdh_reset(DeviceState *dev)
{
    LongspeakPDHState *s = LONGSPEAK_PDH(dev);

    s->post = 0;
    s->scratch0 = 0;
    s->checkin = 0;
    s->semaphore = 0;
}

static const VMStateDescription vmstate_longspeak_pdh = {
    .name = TYPE_LONGSPEAK_PDH,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8(post, LongspeakPDHState),
        VMSTATE_UINT64(scratch0, LongspeakPDHState),
        VMSTATE_UINT64(checkin, LongspeakPDHState),
        VMSTATE_UINT8(semaphore, LongspeakPDHState),
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
