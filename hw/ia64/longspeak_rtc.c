/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The Longs Peak (HP zx1 board) processor-dependent hardware clock.
 *
 * A DS1501/1511-class part behind the PDH bus: time in registers 0 to 7,
 * control in 14 and 15, and a 256-byte battery-backed window addressed
 * through register 16 and read and written through register 19, as the
 * vendor firmware uses it (FFF3E426; battery-backed per the zx6000/rx2600
 * O&M Guide, Sept 2002, p.88; there is no Dillon ERS).  The part has no
 * hundredths register: register 0 is the seconds, and the vendor firmware
 * sets all eight time registers in one go (FFF3E640, 1998-01-01 with the
 * month register's E32K bit).
 *
 * The seconds carry the board's frequency reference: the firmware waits for
 * bit 0 of register 0 to change twice and takes the ITC difference between
 * the two edges (FFF3F750).  A clock that ticks at any other rate gives a
 * wrong reference, and POST 0x000F23 "invalid real time clock cleared".
 *
 * The window is battery-backed on the board and volatile here: nvram= holds
 * only the PDH SRAM and the BMC tokens.
 */

#include "qemu/osdep.h"
#include "qemu/bcd.h"
#include "qemu/cutils.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "hw/core/sysbus.h"
#include "hw/ia64/ia64_vpc_abi.h"
#include "migration/vmstate.h"
#include "qom/object.h"
#include "system/rtc.h"
#include "longspeak_pdh.h"

#define LONGSPEAK_RTC_SECONDS     0x00
#define LONGSPEAK_RTC_MINUTES     0x01
#define LONGSPEAK_RTC_HOURS       0x02
#define LONGSPEAK_RTC_WEEKDAY     0x03
#define LONGSPEAK_RTC_DATE        0x04
#define LONGSPEAK_RTC_MONTH       0x05
#define LONGSPEAK_RTC_YEAR        0x06
#define LONGSPEAK_RTC_CENTURY     0x07
#define LONGSPEAK_RTC_CONTROL_A   0x0e
#define LONGSPEAK_RTC_CONTROL_B   0x0f
#define LONGSPEAK_RTC_RAM_ADDR    0x10
#define LONGSPEAK_RTC_RAM_DATA    0x13

/* Control B bit 7; the month register keeps the oscillator bits. */
#define LONGSPEAK_RTC_TRANSFER_ENABLE 0x80
#define LONGSPEAK_RTC_MONTH_BITS      0xe0
#define LONGSPEAK_RTC_TIME_REGS       8

struct LongspeakRTCState {
    SysBusDevice parent_obj;

    MemoryRegion mr;
    int64_t base;                  /* the time at count zero */
    int64_t frozen_ms;             /* the count while the transfer is off */
    uint8_t control_a;
    uint8_t control_b;
    uint8_t month_bits;
    uint8_t ram_addr;
    uint8_t ram[IA64_PDH_RTC_RAM];
    /* Time registers written while the transfer is off, one bit each. */
    uint8_t pending[LONGSPEAK_RTC_TIME_REGS];
    uint8_t pending_mask;
};

OBJECT_DECLARE_SIMPLE_TYPE(LongspeakRTCState, LONGSPEAK_RTC)

/*
 * Control B bit 7 clear freezes the registers, so that a read of all eight is
 * coherent; what is written meanwhile loads together when the bit is set
 * again, so that no field is ever combined with a stale one (day 31 written
 * in a 30-day month).
 */
static int64_t longspeak_rtc_count(LongspeakRTCState *s)
{
    if (!(s->control_b & LONGSPEAK_RTC_TRANSFER_ENABLE)) {
        return s->frozen_ms;
    }
    return qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);
}

static void longspeak_rtc_now(LongspeakRTCState *s, struct tm *tm)
{
    time_t t = s->base + longspeak_rtc_count(s) / 1000;

    gmtime_r(&t, tm);
}

static uint64_t longspeak_rtc_read(void *opaque, hwaddr addr, unsigned size)
{
    LongspeakRTCState *s = LONGSPEAK_RTC(opaque);
    struct tm now;

    if (addr < LONGSPEAK_RTC_TIME_REGS && (s->pending_mask & (1u << addr))) {
        return s->pending[addr];
    }
    longspeak_rtc_now(s, &now);
    switch (addr) {
    case LONGSPEAK_RTC_SECONDS:
        return to_bcd(now.tm_sec);
    case LONGSPEAK_RTC_MINUTES:
        return to_bcd(now.tm_min);
    case LONGSPEAK_RTC_HOURS:
        return to_bcd(now.tm_hour);
    case LONGSPEAK_RTC_WEEKDAY:
        return now.tm_wday ? now.tm_wday : 7;
    case LONGSPEAK_RTC_DATE:
        return to_bcd(now.tm_mday);
    case LONGSPEAK_RTC_MONTH:
        return to_bcd(now.tm_mon + 1) | s->month_bits;
    case LONGSPEAK_RTC_YEAR:
        return to_bcd(now.tm_year % 100);
    case LONGSPEAK_RTC_CENTURY:
        return to_bcd((now.tm_year + 1900) / 100);
    case LONGSPEAK_RTC_CONTROL_A:
        return s->control_a;
    case LONGSPEAK_RTC_CONTROL_B:
        return s->control_b;
    case LONGSPEAK_RTC_RAM_ADDR:
        return s->ram_addr;
    case LONGSPEAK_RTC_RAM_DATA:
        return s->ram[s->ram_addr];
    default:
        return 0;
    }
}

/* The weekday follows the date, so a write to it is dropped. */
static void longspeak_rtc_set_field(LongspeakRTCState *s, struct tm *tm,
                                    hwaddr addr, uint8_t data)
{
    switch (addr) {
    case LONGSPEAK_RTC_SECONDS:
        tm->tm_sec = from_bcd(data & 0x7f);
        break;
    case LONGSPEAK_RTC_MINUTES:
        tm->tm_min = from_bcd(data & 0x7f);
        break;
    case LONGSPEAK_RTC_HOURS:
        tm->tm_hour = from_bcd(data & 0x3f);
        break;
    case LONGSPEAK_RTC_DATE:
        tm->tm_mday = from_bcd(data & 0x3f);
        break;
    case LONGSPEAK_RTC_MONTH:
        s->month_bits = data & LONGSPEAK_RTC_MONTH_BITS;
        tm->tm_mon = from_bcd(data & 0x1f) - 1;
        break;
    case LONGSPEAK_RTC_YEAR:
        tm->tm_year = tm->tm_year - tm->tm_year % 100 + from_bcd(data);
        break;
    case LONGSPEAK_RTC_CENTURY:
        tm->tm_year = from_bcd(data) * 100 + tm->tm_year % 100 - 1900;
        break;
    }
}

/* Setting the transfer bit again loads the frozen time with what was written. */
static void longspeak_rtc_load(LongspeakRTCState *s)
{
    time_t t = s->base + s->frozen_ms / 1000;
    struct tm tm;
    unsigned i;

    gmtime_r(&t, &tm);
    for (i = 0; i < LONGSPEAK_RTC_TIME_REGS; i++) {
        if (s->pending_mask & (1u << i)) {
            longspeak_rtc_set_field(s, &tm, i, s->pending[i]);
        }
    }
    s->pending_mask = 0;
    s->base = mktimegm(&tm) - longspeak_rtc_count(s) / 1000;
}

static void longspeak_rtc_write(void *opaque, hwaddr addr, uint64_t data,
                                unsigned size)
{
    LongspeakRTCState *s = LONGSPEAK_RTC(opaque);
    struct tm now;

    if (addr < LONGSPEAK_RTC_TIME_REGS) {
        if (!(s->control_b & LONGSPEAK_RTC_TRANSFER_ENABLE)) {
            s->pending[addr] = data;
            s->pending_mask |= 1u << addr;
            return;
        }
        longspeak_rtc_now(s, &now);
        longspeak_rtc_set_field(s, &now, addr, data);
        s->base = mktimegm(&now) - longspeak_rtc_count(s) / 1000;
        return;
    }
    switch (addr) {
    case LONGSPEAK_RTC_CONTROL_A:
        s->control_a = data;
        break;
    case LONGSPEAK_RTC_CONTROL_B:
        if (!(data & LONGSPEAK_RTC_TRANSFER_ENABLE)) {
            s->frozen_ms = longspeak_rtc_count(s);
            s->control_b = data;
        } else {
            s->control_b = data;
            if (s->pending_mask) {
                longspeak_rtc_load(s);
            }
        }
        break;
    case LONGSPEAK_RTC_RAM_ADDR:
        s->ram_addr = data;
        break;
    case LONGSPEAK_RTC_RAM_DATA:
        s->ram[s->ram_addr] = data;
        break;
    default:
        /* Nothing else is modelled. */
        break;
    }
}

static const MemoryRegionOps longspeak_rtc_ops = {
    .read = longspeak_rtc_read,
    .write = longspeak_rtc_write,
    .impl = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
    .endianness = DEVICE_LITTLE_ENDIAN,
};

/* The part is battery-backed, so a system reset leaves all of this alone. */
static const VMStateDescription vmstate_longspeak_rtc = {
    .name = "longspeak-rtc",
    .version_id = 2,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_INT64(base, LongspeakRTCState),
        VMSTATE_INT64(frozen_ms, LongspeakRTCState),
        VMSTATE_UINT8(control_a, LongspeakRTCState),
        VMSTATE_UINT8(control_b, LongspeakRTCState),
        VMSTATE_UINT8(month_bits, LongspeakRTCState),
        VMSTATE_UINT8(ram_addr, LongspeakRTCState),
        VMSTATE_UINT8_ARRAY(ram, LongspeakRTCState, IA64_PDH_RTC_RAM),
        VMSTATE_UINT8_ARRAY_V(pending, LongspeakRTCState,
                              LONGSPEAK_RTC_TIME_REGS, 2),
        VMSTATE_UINT8_V(pending_mask, LongspeakRTCState, 2),
        VMSTATE_END_OF_LIST()
    }
};

static void longspeak_rtc_realize(DeviceState *dev, Error **errp)
{
    LongspeakRTCState *s = LONGSPEAK_RTC(dev);
    struct tm start;

    /* -rtc base= decides where the count starts; it runs on its own after. */
    qemu_get_timedate(&start, 0);
    s->base = mktimegm(&start) - qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) / 1000;
    s->control_b = LONGSPEAK_RTC_TRANSFER_ENABLE;

    memory_region_init_io(&s->mr, OBJECT(dev), &longspeak_rtc_ops, s,
                          "longspeak-rtc", IA64_PDH_RTC_REGS);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mr);
}

static void longspeak_rtc_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = longspeak_rtc_realize;
    dc->desc = "HP Longs Peak PDH clock";
    dc->vmsd = &vmstate_longspeak_rtc;
    dc->user_creatable = false;
}

static const TypeInfo longspeak_rtc_info = {
    .name          = TYPE_LONGSPEAK_RTC,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(LongspeakRTCState),
    .class_init    = longspeak_rtc_class_init,
};

static void longspeak_rtc_register_types(void)
{
    type_register_static(&longspeak_rtc_info);
}

type_init(longspeak_rtc_register_types)
