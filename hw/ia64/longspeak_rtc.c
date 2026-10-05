/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The Longs Peak (HP zx1 board) processor-dependent hardware clock.
 *
 * A DS1501/1511-class part behind the PDH bus (data sheet 19-6820, rev 10/13):
 * time in registers 0 to 7, the alarm in 8 to 11, the watchdog in 12 and 13,
 * control in 14 and 15, and a 256-byte battery-backed window addressed
 * through register 16 and read and written through register 19, as the vendor
 * firmware uses it (FFF3E426; battery-backed per the zx6000/rx2600 O&M Guide,
 * Sept 2002, p.88; there is no Dillon ERS).  The part has no
 * hundredths register: register 0 is the seconds, and the vendor firmware
 * sets all eight time registers in one go (FFF3E640, 1998-01-01 with the
 * month register's E32K bit).
 *
 * The seconds carry the board's frequency reference: the firmware waits for
 * bit 0 of register 0 to change twice and takes the ITC difference between
 * the two edges (FFF3F750).  A clock that ticks at any other rate gives a
 * wrong reference, and POST 0x000F23 "invalid real time clock cleared".
 *
 * The board wiring of the part's IRQ, RST and PWR pins is not known, so the
 * flags of control A reach no pin.
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
#define LONGSPEAK_RTC_ALARM       0x08
#define LONGSPEAK_RTC_WATCHDOG    0x0c
#define LONGSPEAK_RTC_CONTROL_A   0x0e
#define LONGSPEAK_RTC_CONTROL_B   0x0f
#define LONGSPEAK_RTC_RAM_ADDR    0x10
#define LONGSPEAK_RTC_RAM_DATA    0x13

#define LONGSPEAK_RTC_ALARM_REGS     4
#define LONGSPEAK_RTC_WATCHDOG_REGS  2

#define LONGSPEAK_RTC_A_PRS   0x20
#define LONGSPEAK_RTC_A_PAB   0x10
#define LONGSPEAK_RTC_A_TDF   0x08
#define LONGSPEAK_RTC_A_KSF   0x04
#define LONGSPEAK_RTC_A_WDF   0x02
#define LONGSPEAK_RTC_A_IRQF  0x01

#define LONGSPEAK_RTC_TRANSFER_ENABLE 0x80
#define LONGSPEAK_RTC_B_BME   0x20
#define LONGSPEAK_RTC_B_TPE   0x10
#define LONGSPEAK_RTC_B_TIE   0x08
#define LONGSPEAK_RTC_B_KIE   0x04
#define LONGSPEAK_RTC_B_WDE   0x02
#define LONGSPEAK_RTC_B_WDS   0x01

#define LONGSPEAK_RTC_MONTH_BITS      0xe0
#define LONGSPEAK_RTC_EOSC            0x80
#define LONGSPEAK_RTC_ALARM_MASK      0x80
#define LONGSPEAK_RTC_ALARM_DAY       0x40
#define LONGSPEAK_RTC_TIME_REGS       8

#define LONGSPEAK_RTC_DAY_SECONDS     86400

struct LongspeakRTCState {
    SysBusDevice parent_obj;

    MemoryRegion mr;
    int64_t base;                  /* the time at count zero */
    int64_t frozen_ms;             /* the count while the transfer is off */
    int64_t osc_stop_ms;           /* the virtual clock when EOSC was set */
    int64_t osc_lost_ms;           /* the time the oscillator was stopped */
    uint8_t control_a;
    uint8_t control_b;
    uint8_t month_bits;
    uint8_t ram_addr;
    uint8_t ram[IA64_PDH_RTC_RAM];
    /* Time registers written while the transfer is off, one bit each. */
    uint8_t pending[LONGSPEAK_RTC_TIME_REGS];
    uint8_t pending_mask;
    uint8_t alarm[LONGSPEAK_RTC_ALARM_REGS];
    uint8_t watchdog[LONGSPEAK_RTC_WATCHDOG_REGS];
    uint8_t weekday;               /* the day register on day wday_day */
    int64_t wday_day;
    int64_t alarm_t;               /* the last second compared with the alarm */
    int64_t wd_start_ms;           /* the oscillator time of the last reload */
};

OBJECT_DECLARE_SIMPLE_TYPE(LongspeakRTCState, LONGSPEAK_RTC)

/* The bits of each time register that are not fixed at 0 (Table 2). */
static const uint8_t longspeak_rtc_time_bits[LONGSPEAK_RTC_TIME_REGS] = {
    0x7f, 0x7f, 0x3f, 0x07, 0x3f, 0xff, 0xff, 0xff,
};

/* EOSC stops the oscillator, and with it the count and the watchdog. */
static int64_t longspeak_rtc_osc_ms(LongspeakRTCState *s)
{
    int64_t now = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);

    if (s->month_bits & LONGSPEAK_RTC_EOSC) {
        now = s->osc_stop_ms;
    }
    return now - s->osc_lost_ms;
}

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
    return longspeak_rtc_osc_ms(s);
}

static int64_t longspeak_rtc_seconds(LongspeakRTCState *s)
{
    return s->base + longspeak_rtc_count(s) / 1000;
}

static void longspeak_rtc_now(LongspeakRTCState *s, struct tm *tm)
{
    time_t t = longspeak_rtc_seconds(s);

    gmtime_r(&t, tm);
}

static int64_t longspeak_rtc_day(int64_t t)
{
    if (t >= 0) {
        return t / LONGSPEAK_RTC_DAY_SECONDS;
    }
    return -((-t + LONGSPEAK_RTC_DAY_SECONDS - 1) / LONGSPEAK_RTC_DAY_SECONDS);
}

/*
 * The day register counts on its own: it keeps what is written and steps at
 * midnight, from 7 to 1, whatever the date is.
 */
static uint8_t longspeak_rtc_weekday_on(LongspeakRTCState *s, int64_t day)
{
    int64_t d = day - s->wday_day;

    if (!d) {
        return s->weekday;
    }
    return ((s->weekday - 1 + d) % 7 + 7) % 7 + 1;
}

static void longspeak_rtc_set_weekday(LongspeakRTCState *s, uint8_t weekday)
{
    s->weekday = weekday;
    s->wday_day = longspeak_rtc_day(longspeak_rtc_seconds(s));
}

static void longspeak_rtc_set_month_bits(LongspeakRTCState *s, uint8_t bits)
{
    int64_t now = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);

    if ((bits ^ s->month_bits) & LONGSPEAK_RTC_EOSC) {
        if (bits & LONGSPEAK_RTC_EOSC) {
            s->osc_stop_ms = now;
        } else {
            s->osc_lost_ms += now - s->osc_stop_ms;
        }
    }
    s->month_bits = bits;
}

/* An alarm field that can match, or -1 for one that never does. */
static int longspeak_rtc_alarm_field(uint8_t v, int lo, int hi)
{
    int n;

    if ((v & 0x0f) > 9 || (v >> 4) > 9) {
        return -1;
    }
    n = from_bcd(v);
    return n >= lo && n <= hi ? n : -1;
}

/* The first second from @start on that has hour @h, minute @m, second @sec. */
static int longspeak_rtc_alarm_in_day(int start, int h, int m, int sec)
{
    int hh, mm, from;

    for (hh = start / 3600; hh < 24; hh++) {
        if (h >= 0 && hh != h) {
            continue;
        }
        for (mm = hh == start / 3600 ? start / 60 % 60 : 0; mm < 60; mm++) {
            if (m >= 0 && mm != m) {
                continue;
            }
            from = hh == start / 3600 && mm == start / 60 % 60 ?
                   start % 60 : 0;
            if (sec < 0) {
                return hh * 3600 + mm * 60 + from;
            }
            if (sec >= from) {
                return hh * 3600 + mm * 60 + sec;
            }
        }
    }
    return -1;
}

/*
 * The first second after @t that matches the alarm, or INT64_MAX.  Table 3:
 * the mask combinations that the table does not list give the once-per-second
 * rate.
 */
static int64_t longspeak_rtc_alarm_next(LongspeakRTCState *s, int64_t t)
{
    bool by_day = s->alarm[3] & LONGSPEAK_RTC_ALARM_DAY;
    int want[LONGSPEAK_RTC_ALARM_REGS] = { -2, -2, -2, -2 };
    unsigned masks = 0, fields, i;
    int64_t first, day;
    int from, sod;

    for (i = 0; i < LONGSPEAK_RTC_ALARM_REGS; i++) {
        if (s->alarm[i] & LONGSPEAK_RTC_ALARM_MASK) {
            masks |= 1u << i;
        }
    }
    switch (masks) {
    case 0xe:
        fields = 1;
        break;
    case 0xc:
        fields = 2;
        break;
    case 0x8:
        fields = 3;
        break;
    case 0x0:
        fields = 4;
        break;
    default:
        return t + 1;
    }
    want[0] = longspeak_rtc_alarm_field(s->alarm[0] & 0x7f, 0, 59);
    want[1] = fields > 1 ? longspeak_rtc_alarm_field(s->alarm[1] & 0x7f,
                                                     0, 59) : -2;
    want[2] = fields > 2 ? longspeak_rtc_alarm_field(s->alarm[2] & 0x3f,
                                                     0, 23) : -2;
    want[3] = fields > 3 ? longspeak_rtc_alarm_field(s->alarm[3] & 0x3f,
                                                     1, by_day ? 7 : 31) : -2;
    for (i = 0; i < fields; i++) {
        if (want[i] == -1) {
            return INT64_MAX;
        }
    }

    /* Every date from 1 to 31 comes within 62 days. */
    first = longspeak_rtc_day(t + 1);
    for (day = first; day < first + 63; day++) {
        if (fields == 4) {
            if (by_day) {
                if (longspeak_rtc_weekday_on(s, day) != want[3]) {
                    continue;
                }
            } else {
                time_t midnight = day * LONGSPEAK_RTC_DAY_SECONDS;
                struct tm tm;

                gmtime_r(&midnight, &tm);
                if (tm.tm_mday != want[3]) {
                    continue;
                }
            }
        }
        from = day == first ? t + 1 - day * LONGSPEAK_RTC_DAY_SECONDS : 0;
        sod = longspeak_rtc_alarm_in_day(from, want[2], want[1], want[0]);
        if (sod >= 0) {
            return day * LONGSPEAK_RTC_DAY_SECONDS + sod;
        }
    }
    return INT64_MAX;
}

/* Data sheet, Control A: PAB clears with KSF or with TDF and TPE. */
static void longspeak_rtc_flags(LongspeakRTCState *s)
{
    uint8_t a = s->control_a & ~LONGSPEAK_RTC_A_IRQF;
    uint8_t b = s->control_b;

    if ((a & LONGSPEAK_RTC_A_KSF) ||
        ((a & LONGSPEAK_RTC_A_TDF) && (b & LONGSPEAK_RTC_B_TPE))) {
        a &= ~LONGSPEAK_RTC_A_PAB;
    }
    if (((a & LONGSPEAK_RTC_A_TDF) && (b & LONGSPEAK_RTC_B_TIE)) ||
        ((a & LONGSPEAK_RTC_A_KSF) && (b & LONGSPEAK_RTC_B_KIE)) ||
        ((a & LONGSPEAK_RTC_A_WDF) && (b & LONGSPEAK_RTC_B_WDE))) {
        a |= LONGSPEAK_RTC_A_IRQF;
    }
    s->control_a = a;
}

/* 0Ch holds the tenths and hundredths, 0Dh the seconds, both in BCD. */
static int64_t longspeak_rtc_watchdog_ms(LongspeakRTCState *s)
{
    return from_bcd(s->watchdog[1]) * 1000 + from_bcd(s->watchdog[0]) * 10;
}

/*
 * The flags are brought up to date at each access.  A watchdog timeout sets
 * WDF whatever WDE is, and the watchdog reloads; with WDE and WDS it is a
 * single shot that clears WDE (Using the Watchdog Timer).  An alarm match
 * sets TDF only while TE is 1 (Using the Clock Alarm).
 */
static void longspeak_rtc_update(LongspeakRTCState *s)
{
    int64_t period = longspeak_rtc_watchdog_ms(s);
    int64_t osc = longspeak_rtc_osc_ms(s);
    int64_t t = longspeak_rtc_seconds(s);

    if (period && osc - s->wd_start_ms >= period) {
        s->wd_start_ms += (osc - s->wd_start_ms) / period * period;
        s->control_a |= LONGSPEAK_RTC_A_WDF;
        if ((s->control_b & (LONGSPEAK_RTC_B_WDE | LONGSPEAK_RTC_B_WDS)) ==
            (LONGSPEAK_RTC_B_WDE | LONGSPEAK_RTC_B_WDS)) {
            s->control_b &= ~LONGSPEAK_RTC_B_WDE;
        }
    }
    if (t > s->alarm_t) {
        if (longspeak_rtc_alarm_next(s, s->alarm_t) <= t) {
            s->control_a |= LONGSPEAK_RTC_A_TDF;
        }
        s->alarm_t = t;
    }
    longspeak_rtc_flags(s);
}

static uint64_t longspeak_rtc_read(void *opaque, hwaddr addr, unsigned size)
{
    LongspeakRTCState *s = LONGSPEAK_RTC(opaque);
    struct tm now;
    uint8_t val;

    longspeak_rtc_update(s);
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
        return longspeak_rtc_weekday_on(s,
                   longspeak_rtc_day(longspeak_rtc_seconds(s)));
    case LONGSPEAK_RTC_DATE:
        return to_bcd(now.tm_mday);
    case LONGSPEAK_RTC_MONTH:
        return to_bcd(now.tm_mon + 1) | s->month_bits;
    case LONGSPEAK_RTC_YEAR:
        return to_bcd(now.tm_year % 100);
    case LONGSPEAK_RTC_CENTURY:
        return to_bcd((now.tm_year + 1900) / 100);
    case LONGSPEAK_RTC_ALARM ... LONGSPEAK_RTC_ALARM + 3:
        return s->alarm[addr - LONGSPEAK_RTC_ALARM];
    case LONGSPEAK_RTC_WATCHDOG ... LONGSPEAK_RTC_WATCHDOG + 1:
        return s->watchdog[addr - LONGSPEAK_RTC_WATCHDOG];
    case LONGSPEAK_RTC_CONTROL_A:
        /* BLF1 and BLF2 read 0: the batteries are good. */
        val = s->control_a;
        s->control_a &= ~(LONGSPEAK_RTC_A_TDF | LONGSPEAK_RTC_A_KSF |
                          LONGSPEAK_RTC_A_WDF);
        longspeak_rtc_flags(s);
        return val;
    case LONGSPEAK_RTC_CONTROL_B:
        return s->control_b;
    case LONGSPEAK_RTC_RAM_ADDR:
        return s->ram_addr;
    case LONGSPEAK_RTC_RAM_DATA:
        val = s->ram[s->ram_addr];
        if (s->control_b & LONGSPEAK_RTC_B_BME) {
            s->ram_addr++;
        }
        return val;
    default:
        return 0;
    }
}

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
        longspeak_rtc_set_month_bits(s, data & LONGSPEAK_RTC_MONTH_BITS);
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

/* A new date leaves the day register as it was. */
static void longspeak_rtc_set_time(LongspeakRTCState *s, struct tm *tm,
                                   uint8_t weekday)
{
    s->base = mktimegm(tm) - longspeak_rtc_count(s) / 1000;
    longspeak_rtc_set_weekday(s, weekday);
    s->alarm_t = longspeak_rtc_seconds(s);
}

/* Setting the transfer bit again loads the frozen time with what was written. */
static void longspeak_rtc_load(LongspeakRTCState *s)
{
    time_t t = s->base + s->frozen_ms / 1000;
    uint8_t weekday = longspeak_rtc_weekday_on(s, longspeak_rtc_day(t));
    struct tm tm;
    unsigned i;

    gmtime_r(&t, &tm);
    for (i = 0; i < LONGSPEAK_RTC_TIME_REGS; i++) {
        if (!(s->pending_mask & (1u << i))) {
            continue;
        }
        if (i == LONGSPEAK_RTC_WEEKDAY) {
            weekday = s->pending[i];
        } else {
            longspeak_rtc_set_field(s, &tm, i, s->pending[i]);
        }
    }
    s->pending_mask = 0;
    longspeak_rtc_set_time(s, &tm, weekday);
}

static void longspeak_rtc_write(void *opaque, hwaddr addr, uint64_t data,
                                unsigned size)
{
    LongspeakRTCState *s = LONGSPEAK_RTC(opaque);
    struct tm now;
    uint8_t weekday;

    longspeak_rtc_update(s);
    if (addr < LONGSPEAK_RTC_TIME_REGS) {
        data &= longspeak_rtc_time_bits[addr];
        if (!(s->control_b & LONGSPEAK_RTC_TRANSFER_ENABLE)) {
            s->pending[addr] = data;
            s->pending_mask |= 1u << addr;
            return;
        }
        if (addr == LONGSPEAK_RTC_WEEKDAY) {
            longspeak_rtc_set_weekday(s, data);
            return;
        }
        weekday = longspeak_rtc_weekday_on(s,
                      longspeak_rtc_day(longspeak_rtc_seconds(s)));
        longspeak_rtc_now(s, &now);
        longspeak_rtc_set_field(s, &now, addr, data);
        longspeak_rtc_set_time(s, &now, weekday);
        return;
    }
    switch (addr) {
    case LONGSPEAK_RTC_ALARM ... LONGSPEAK_RTC_ALARM + 3:
        /* Bit 6 of the alarm hours is fixed at 0. */
        if (addr == LONGSPEAK_RTC_ALARM + 2) {
            data &= ~0x40;
        }
        s->alarm[addr - LONGSPEAK_RTC_ALARM] = data;
        s->alarm_t = longspeak_rtc_seconds(s);
        break;
    case LONGSPEAK_RTC_WATCHDOG ... LONGSPEAK_RTC_WATCHDOG + 1:
        /* A write reloads the watchdog. */
        s->watchdog[addr - LONGSPEAK_RTC_WATCHDOG] = data;
        s->wd_start_ms = longspeak_rtc_osc_ms(s);
        break;
    case LONGSPEAK_RTC_CONTROL_A:
        /*
         * Data sheet, Control A: TDF and WDF clear when written 0, KSF takes
         * the written bit; BLF1, BLF2 and IRQF are read-only.
         */
        s->control_a = (s->control_a & data &
                        (LONGSPEAK_RTC_A_TDF | LONGSPEAK_RTC_A_WDF)) |
                       (data & (LONGSPEAK_RTC_A_PRS | LONGSPEAK_RTC_A_PAB |
                                LONGSPEAK_RTC_A_KSF));
        longspeak_rtc_flags(s);
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
        /* No alarm matches while the transfer is off. */
        s->alarm_t = longspeak_rtc_seconds(s);
        longspeak_rtc_flags(s);
        break;
    case LONGSPEAK_RTC_RAM_ADDR:
        s->ram_addr = data;
        break;
    case LONGSPEAK_RTC_RAM_DATA:
        s->ram[s->ram_addr] = data;
        if (s->control_b & LONGSPEAK_RTC_B_BME) {
            s->ram_addr++;
        }
        break;
    default:
        /* 11h, 12h: reserved. */
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

/* Version 2 had no day register of its own and no oscillator stop. */
static int longspeak_rtc_post_load(void *opaque, int version_id)
{
    LongspeakRTCState *s = opaque;
    struct tm now;

    if (version_id < 3) {
        s->osc_stop_ms = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);
        s->osc_lost_ms = 0;
        longspeak_rtc_now(s, &now);
        longspeak_rtc_set_weekday(s, now.tm_wday ? now.tm_wday : 7);
        s->alarm_t = longspeak_rtc_seconds(s);
        s->wd_start_ms = longspeak_rtc_osc_ms(s);
    }
    return 0;
}

/* The part is battery-backed, so a system reset leaves all of this alone. */
static const VMStateDescription vmstate_longspeak_rtc = {
    .name = "longspeak-rtc",
    .version_id = 3,
    .minimum_version_id = 1,
    .post_load = longspeak_rtc_post_load,
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
        VMSTATE_INT64_V(osc_stop_ms, LongspeakRTCState, 3),
        VMSTATE_INT64_V(osc_lost_ms, LongspeakRTCState, 3),
        VMSTATE_UINT8_ARRAY_V(alarm, LongspeakRTCState,
                              LONGSPEAK_RTC_ALARM_REGS, 3),
        VMSTATE_UINT8_ARRAY_V(watchdog, LongspeakRTCState,
                              LONGSPEAK_RTC_WATCHDOG_REGS, 3),
        VMSTATE_UINT8_V(weekday, LongspeakRTCState, 3),
        VMSTATE_INT64_V(wday_day, LongspeakRTCState, 3),
        VMSTATE_INT64_V(alarm_t, LongspeakRTCState, 3),
        VMSTATE_INT64_V(wd_start_ms, LongspeakRTCState, 3),
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
    /*
     * The rx2600 reads KSF set after a power-on and clear after a warm reset
     * (capture 2026-10-04, run 3): the board powers on through the kickstart
     * input.  The rest of the part has no defined power-up state; the model
     * starts it at 0, with the day register counting Monday as 1.
     */
    s->control_a = LONGSPEAK_RTC_A_KSF;
    longspeak_rtc_set_weekday(s, start.tm_wday ? start.tm_wday : 7);
    s->alarm_t = longspeak_rtc_seconds(s);

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
