/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The flash stage's hardware probes: which core chipset this board carries
 * and how much DRAM is installed.  Runs in place from the flash window
 * before the image is shadowed, so like flash_entry.c it names no absolute
 * address of its own: no globals, no string literals, only its arguments
 * and constants.
 *
 * Chipset: the 460GX answers CF8/CFC configuration cycles on bus 0 device
 * 10h with the SAC's identity (SSDM 2.2.1: "Device 10h on Bus #0 is mapped
 * to the SAC"); the zx1 mio answers its IOC function ID at the fixed CSR
 * base (mio ERS 3.3.1).  Each reads as open bus on the other board.
 *
 * Memory: the 460GX sizes DRAM from the DIMMs' SPD EEPROMs through the
 * Memory Card's I2C pass-through (SSDM 5.5.1; the protocol the vendor
 * firmware uses, plans/phase5-real-firmware-boot.md 5.5).  The zx1 mio ERS
 * publishes no memory-sizing register, so there DRAM is sized by presence
 * probing at 64 MB steps, refined to 1 MB -- below the PCI aperture, where
 * the band may carry the SBA IOVA hole, and again from 4 GB up.
 */

#include "fw-base.h"
#include "hw/ia64/ia64_vpc_abi.h"

#define CHIPSET_460GX 1
#define CHIPSET_ZX1   2

#define CF8 ((volatile UINT32 *)(UINTN)(IA64_PCI_IO_BASE + 0xcf8))
#define CFC ((volatile UINT8 *)(UINTN)(IA64_PCI_IO_BASE + 0xcfc))

#define SBA_IOC_FUNC_ID ((volatile UINT64 *)(UINTN)(IA64_SBA_CSR_BASE + 0x1000))

#define PROBE_STEP   (64ULL << 20)
#define PROBE_FINE   (1ULL << 20)
#define PROBE_START  IA64_FW_LOW_RAM_MIN
#define PROBE_HIGH   0x100000000ULL
#define PROBE_HIGH_LIMIT (PROBE_HIGH + (64ULL << 30))

static UINT32 cfg_addr(UINT64 bus, UINT64 dev, UINT64 fn, UINT64 reg)
{
    return 0x80000000u | ((UINT32)bus << 16) | ((UINT32)dev << 11) |
           ((UINT32)fn << 8) | ((UINT32)reg & 0xfc);
}

static UINT8 cfg_read8(UINT64 bus, UINT64 dev, UINT64 fn, UINT64 reg)
{
    *CF8 = cfg_addr(bus, dev, fn, reg);
    return CFC[reg & 3];
}

static void cfg_write8(UINT64 bus, UINT64 dev, UINT64 fn, UINT64 reg,
                       UINT8 value)
{
    *CF8 = cfg_addr(bus, dev, fn, reg);
    CFC[reg & 3] = value;
}

static UINT64 probe_chipset(void)
{
    UINT32 id = cfg_read8(0, 0x10, 0, 0) | ((UINT32)cfg_read8(0, 0x10, 0, 1) << 8) |
                ((UINT32)cfg_read8(0, 0x10, 0, 2) << 16) |
                ((UINT32)cfg_read8(0, 0x10, 0, 3) << 24);

    if (id == 0x84e08086u) {
        return CHIPSET_460GX;
    }
    if ((*SBA_IOC_FUNC_ID & 0xffffffffULL) == IA64_SBA_IOC_FUNC_ID) {
        return CHIPSET_ZX1;
    }
    return 0;
}

/*
 * One Memory Card A row through the SPD tunnel: select the stack in the
 * SAC's IIADR (D4h = stack 0, D0h = stack 1), raise the row's select bit in
 * the MAC function that owns it, and read the addressed EEPROM through
 * function 2.  Byte 2 says SDRAM (4) for a populated row; the geometry
 * bytes give 2^(rows+columns) x banks x ranks x 8 bytes per DIMM, four
 * DIMMs a row (SSDM Table 5-2).
 */
static UINT64 spd_row_bytes(UINT64 cbn, unsigned row)
{
    unsigned stack = row / 4, fn = 4 + row % 4, f;
    UINT64 bytes = 0;

    cfg_write8(cbn, 0x00, 0, 0x68, stack == 0 ? 0xd4 : 0xd0);
    for (f = 4; f < 8; f++) {
        cfg_write8(cbn, 0x05, f, 0x48, f == fn ? 1 : 0);
    }
    if (cfg_read8(cbn, 0x05, 2, 2) == 4) {
        unsigned rows = cfg_read8(cbn, 0x05, 2, 3);
        unsigned cols = cfg_read8(cbn, 0x05, 2, 4);
        unsigned ranks = cfg_read8(cbn, 0x05, 2, 5);
        unsigned banks = cfg_read8(cbn, 0x05, 2, 17);

        if (rows + cols < 40 && ranks != 0 && banks != 0) {
            bytes = (1ULL << (rows + cols)) * banks * ranks * 8 * 4;
        }
    }
    cfg_write8(cbn, 0x05, fn, 0x48, 0);
    return bytes;
}

static UINT64 probe_ram_460gx(void)
{
    UINT64 cbn = cfg_read8(0, 0x10, 0, 0x40);
    UINT64 total = 0;
    unsigned row;

    for (row = 0; row < 8; row++) {
        total += spd_row_bytes(cbn, row);
    }
    return total;
}

/* True when DRAM answers at @addr: two patterns survive a write and read. */
static BOOLEAN ram_present(UINT64 addr)
{
    volatile UINT64 *p = (volatile UINT64 *)(UINTN)addr;
    UINT64 saved = *p;
    BOOLEAN ok;

    *p = 0x5aa5f00f0ff05aa5ULL;
    ok = *p == 0x5aa5f00f0ff05aa5ULL;
    *p = ~0x5aa5f00f0ff05aa5ULL;
    ok = ok && *p == ~0x5aa5f00f0ff05aa5ULL;
    *p = saved;
    return ok;
}

/* Bytes of DRAM present in [@base, @limit): coarse steps, then the tail. */
static UINT64 probe_band(UINT64 base, UINT64 limit, BOOLEAN stop_at_gap)
{
    UINT64 addr, bytes = 0, last_hit = 0;

    for (addr = base; addr + PROBE_STEP <= limit; addr += PROBE_STEP) {
        /* A whole step is present only if its last chunk is too. */
        if (ram_present(addr) &&
            ram_present(addr + PROBE_STEP - PROBE_FINE)) {
            bytes += PROBE_STEP;
            last_hit = addr + PROBE_STEP;
        } else if (ram_present(addr) || stop_at_gap) {
            /* A partial step ends the band; the fine tail sizes it. */
            last_hit = addr;
            break;
        }
    }
    /* The tail past the last whole step, in fine steps. */
    for (addr = last_hit; addr + PROBE_FINE <= limit; addr += PROBE_FINE) {
        if (!ram_present(addr)) {
            break;
        }
        bytes += PROBE_FINE;
    }
    return bytes;
}

/*
 * Installed DRAM is what the low band holds plus what sits above 4 GB; the
 * low band may carry a hole (the zx1 SBA IOVA space), so its count can be
 * short of the aperture while DRAM still fills up to it -- the chunk just
 * below the aperture says whether there is more above 4 GB.
 */
static UINT64 probe_ram_generic(void)
{
    UINT64 low = PROBE_START + probe_band(PROBE_START, IA64_PCI_MMIO_BASE, 0);

    if (!ram_present(IA64_PCI_MMIO_BASE - PROBE_FINE)) {
        return low;
    }
    return low + probe_band(PROBE_HIGH, PROBE_HIGH_LIMIT, 1);
}

/* Returns installed DRAM in bytes; *Chipset receives the core chipset. */
UINT64 fw_flash_probe(UINT64 *Chipset)
{
    UINT64 chipset = probe_chipset();
    UINT64 ram;

    *Chipset = chipset;
    ram = chipset == CHIPSET_460GX ? probe_ram_460gx() : 0;
    if (ram < IA64_FW_LOW_RAM_MIN) {
        ram = probe_ram_generic();
    }
    if (ram < IA64_FW_LOW_RAM_MIN) {
        ram = IA64_FW_LOW_RAM_MIN;
    }
    return ram & ~(PROBE_FINE - 1);
}
