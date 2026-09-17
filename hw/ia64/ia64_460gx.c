/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Intel 460GX chipset model: System Address Controller aperture, CF8/CFC
 * configuration mechanism and the chipset's bus-CBN configuration space,
 * Memory Card A SPD tunnel, PCIS-programmed DRAM/PCI gap, diagnostic port.
 *
 * The 460GX SSDM (248704-001) is the authority; the register offsets it does
 * not publish are noted where they were measured against the vendor firmware
 * (plans/460gx-config-space-notes.md, plans/phase5-real-firmware-boot.md).
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "hw/core/qdev-properties.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_device.h"
#include "hw/pci/pci_host.h"
#include "hw/ia64/ia64_460gx.h"
#include "hw/ia64/ia64_pci.h"
#include "system/address-spaces.h"
#include "system/runstate.h"
#include "hw/core/cpu.h"
#include "target/ia64/cpu.h"

/*
 * POST-code port 0x80/0x81 (SAL narrates boot progress there; the codes are
 * tabulated in plans/sdv-i2000-firmware-reference.md sec 6.5).  Logged on
 * change only, so a code re-written in a wait loop cannot flood the log.
 */
static uint64_t ia64_460gx_post_read(void *opaque, hwaddr addr, unsigned size)
{
    IA64460GXState *s = opaque;

    return s->post_last >> (addr * 8);
}

static void ia64_460gx_post_write(void *opaque, hwaddr addr, uint64_t val,
                                   unsigned size)
{
    IA64460GXState *s = opaque;
    uint16_t code = s->post_last;

    if (size == 2 && addr == 0) {
        code = val;
    } else {
        code &= ~(0xff << (addr * 8));
        code |= (val & 0xff) << (addr * 8);
    }
    if (code != s->post_last) {
        s->post_last = code;
        qemu_log("ia64-460gx: POST %02x%02x\n", code >> 8, code & 0xff);
    }
}

static const MemoryRegionOps ia64_460gx_post_ops = {
    .read = ia64_460gx_post_read,
    .write = ia64_460gx_post_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 2,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

/*
 * FEB0_0CC0h "is used for BSP selection.  It is a write once register in
 * the SAC" (SSDM 4.1.3).  The vendor firmware's normal reset path never
 * stores to it: SAL_B loads the word, polls until bit 7 is set and compares
 * the low seven bits with its own LID.id (link 0x400A90; SAL_A's recovery
 * path additionally stores 80h | id first, 0xFFFF32C2).  So the first
 * processor whose access reaches the SAC claims it -- the system bus carries
 * the requesting agent's id -- and every later access reads that claim.
 * The poll is a single load: "ld4.acq r3=[r4]; tbit.z p7,p6=r3,7;;
 * (p07) br.cond 0x400AA0" branches to its own bundle, so the word must
 * already carry the claim when the first load returns -- the SAC decides on
 * that access, it does not leave the processor to try again.
 *
 * Which processor's access arrives first is a bus-arbitration outcome, and
 * with multi-threaded TCG it would be a coin toss; socket 3 won one boot in
 * three, and that boot never reached the video POST (the IA-32 CSM runs on
 * the machine's first CPU).  Real boards boot from socket 0 unless it is
 * absent, so whichever access claims the word, the claim names the first
 * CPU's LID.id; later stores are dropped.
 */
static void ia64_460gx_sac_claim_bsp(IA64460GXState *s)
{
    if (!(s->sac_data[IA64_460GX_SAC_BOOT_SEM] & 0x80) && first_cpu != NULL) {
        CPUIA64State *env = cpu_env(first_cpu);

        s->sac_data[IA64_460GX_SAC_BOOT_SEM] =
            0x80 | ((env->cr[IA64_CR_SAPIC_LID] >> IA64_SAPIC_LID_ID_SHIFT) &
                    0x7f);
    }
}

static uint64_t ia64_460gx_sac_read(void *opaque, hwaddr addr, unsigned size)
{
    IA64460GXState *s = opaque;
    uint64_t val = 0;
    unsigned i;

    if (addr <= IA64_460GX_SAC_BOOT_SEM &&
        addr + size > IA64_460GX_SAC_BOOT_SEM) {
        ia64_460gx_sac_claim_bsp(s);
    }
    for (i = 0; i < size; i++) {
        val |= (uint64_t)s->sac_data[addr + i] << (i * 8);
    }
    qemu_log_mask(LOG_UNIMP, "ia64-460gx: SAC read  +%04x/%u = 0x%" PRIx64
                  "\n", (unsigned)addr, size, val);
    return val;
}

static void ia64_460gx_sac_write(void *opaque, hwaddr addr, uint64_t val,
                                  unsigned size)
{
    IA64460GXState *s = opaque;
    unsigned i;

    for (i = 0; i < size; i++) {
        if (addr + i == IA64_460GX_SAC_BOOT_SEM) {
            ia64_460gx_sac_claim_bsp(s);
            continue;
        }
        s->sac_data[addr + i] = val >> (i * 8);
    }
    qemu_log_mask(LOG_UNIMP, "ia64-460gx: SAC write +%04x/%u = 0x%" PRIx64
                  "\n", (unsigned)addr, size, val);
}

static const MemoryRegionOps ia64_460gx_sac_ops = {
    .read = ia64_460gx_sac_read,
    .write = ia64_460gx_sac_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 8,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

/*
 * 460GX CF8/CFC configuration space.  Bus CBN (reset FFh, programmed to
 * EEh by both firmwares) carries the chipset's own devices (SSDM Table 2-1):
 * dev 00h/01h SAC,
 * 04h SDC, 05h/06h Memory Card A/B (MAC; SPD EEPROMs tunnel through its
 * higher functions over I2C), dev 10h the CBN-programming device.  The
 * public SSDM documents none of the platform-setup register offsets
 * (plans/460gx-config-space-notes.md), so the model is a write-store/
 * read-back scratch per function with the empirically required specials:
 * Memory Card A claims presence with a MAC ID, dev 10h reg 40h is the CBN.
 * Accesses to non-chipset device numbers forward to the QEMU PCI bus.
 */
/*
 * The chipset's own functions, on bus CBN (460GX SSDM Table 2-1): the SAC at
 * 00h/01h, the SDC at 04h, the memory cards at 05h/06h and the expander
 * ports at 10h+.  The south bridge is NOT among them -- it is an ordinary
 * PCI device on the compatibility bus, and this machine models it there.
 */
static const uint8_t ia64_460gx_chipset_devs[] = { 0x00, 0x01, 0x04, 0x05,
                                                   0x10, 0x12, 0x13, 0x14 };
#define IA64_460GX_CFG_FN_SIZE   256
#define IA64_460GX_CFG_DEV_SIZE  (8 * IA64_460GX_CFG_FN_SIZE)
/*
 * One block per chipset device, plus one more for the CBN window: bus 0
 * device 10h is a register file of its own (SSDM 2.2.1 and 2.3.2, "Device
 * 10h on Bus #0 is mapped to the SAC; it contains the programmable Chipset
 * Bus Number"), distinct from the expander port that Table 2-1 puts at
 * device 10h on bus CBN.
 */
#define IA64_460GX_CFG_SIZE      \
    ((ARRAY_SIZE(ia64_460gx_chipset_devs) + 1) * IA64_460GX_CFG_DEV_SIZE)
#define IA64_460GX_CBN_DEV       0x10
#define IA64_460GX_CBN_REG       0x40
/*
 * An expander port's bus-number pair, at the offsets the vendor firmware's
 * host enumeration programs and its DSDT reads (\_SB.CBN.SACn.BSNO/SBNO;
 * the SSDM names the registers without placing them: "the destination ...
 * is determined by the Bus Number and Subordinate Bus Number of each PCI
 * port in each PXB", 2.3.1).  Port 0 is "Expander 0, Bus a ... the
 * compatibility bus (where the boot vector is always directed)" (Table
 * 2-1), which bus 0 reaches regardless (2.2.1: every non-chipset device
 * number on bus 0 forwards to it), and which its programmed pair reaches
 * like any other port's -- the firmware only computes a port's windows
 * (PCIS, IOR) from what it finds on the bus it just numbered, so a port
 * that ignored its pair would leave PCI0 without a window, and its DSDT
 * hands that pair to Windows as PCI0's _CRS bus range.
 */
#define IA64_460GX_XXB_BUSNO_REG 0x48
#define IA64_460GX_XXB_SUBNO_REG 0x49
/*
 * PCIS: the port's PCI memory window base in 32 MB units.  "PCIS[7] -
 * FDFF_FFFFh: PCIx, PCIS register determines target PCI bus" (SSDM
 * memory-map table; 4.1.3.1 for the variable gap it bounds).  The vendor
 * DSDT hands [PCIS << 25, FE000000h) to Windows as PCI0's window, so the
 * compatibility port's value is where the machine's routed window has to
 * start -- Windows placed the OHCI's and 82557's BARs below the fixed
 * EE000000h aperture and their drivers read open bus (Code 10).
 */
#define IA64_460GX_XXB_PCIS_REG  0x84
#define IA64_460GX_COMPAT_PORT   0x10

/*
 * The GXB AGP host bridge (chipset device 14h, function 1 -- "BRI4") holds the
 * AGP graphics aperture base.  AGPSIZ (reg A2h) bit 3 selects which register
 * supplies it: the 32-bit APBASE (reg 10h) when clear, or the 64-bit BAPBASE
 * (reg 98h) when set (460GX SSDM 7).  The vendor firmware programs AGPSIZ=09h
 * (bit 3 set, bit 0 = 256 MiB) and BAPBASE=0x1_00000000, i.e. a 256 MiB
 * aperture based at 4 GiB.
 *
 * That above-4-GiB base is what makes Windows XP 64-bit (build 2600) fail the
 * GXB/AGP root with Code 12.  agp460.sys reads AGPSIZ then BAPBASE
 * (WSRV03 base/busdrv/agp/agp460/gart.c AgpQueryAperture) and agplib appends a
 * *pinned*, non-relocatable memory requirement [base, base+size-1] for the
 * aperture -- on IA-64 PnP may not move the aperture base, so only that one
 * "preferred" descriptor is offered (agplib/resource.c ~205, 262-271).  This
 * build serialises the aperture as a 32-bit CmResourceTypeMemory descriptor,
 * so a 4 GiB base truncates to [0, 0x0FFFFFFF]; that range lies inside RAM,
 * the arbiter cannot grant it, and the root gets Code 12 and never enumerates
 * its AGP child.  A below-4-GiB base is represented and placed intact.
 *
 * So when a write leaves BAPBASE naming an address at or above 4 GiB, re-base
 * the aperture inside PCI3's producer window, below the graphics framebuffer.
 * Only an above-4-GiB base is clamped -- a legitimate below-4-GiB base (agp460
 * writes the aperture back once the OS owns it) is left as written.  AGPSIZ
 * bit 3 stays set: it only selects the 64-bit register, not an above-4-GiB
 * address (our own ia64_agp GART runs bit 3 set with a below-4-GiB base too).
 * Vendor-firmware-only: the project firmware never writes this register,
 * guests reach PCI config through SAL, and the 460GX
 * GART-translation device (ia64_agp, bus 0 dev 31) keeps its own aperture base
 * (0xEE000000), so the Linux AGP-GART DMA path is unaffected.  agp460 now
 * programs the aperture at a different base than ia64_agp decodes, so Windows
 * AGP-texture DMA would need the two reconciled -- a known gap, not a
 * regression (that path was dead while the root failed Code 12).
 */
#define IA64_460GX_GXB_DEV              0x14
#define IA64_460GX_GXB_BRIDGE_FN       1
#define IA64_460GX_GXB_BAPBASE_REG     0x98    /* 64-bit AGP aperture base */
#define IA64_460GX_GXB_BAPBASE_LAST    0x9f
#define IA64_460GX_GXB_AGP_APERTURE_BASE 0x00000000d0000000ULL

/*
 * SPD EEPROMs served through the MAC's I2C pass-through: firmware writes the
 * DIMM's I2C address (bit 7 = read) into the SAC IIADR register (dev 00h fn 0
 * reg 0x68), then config reads of Memory Card fn 2/3 return the addressed
 * EEPROM's bytes at the register offset (observed protocol,
 * plans/phase5-real-firmware-boot.md sec 5.5; register naming per
 * plans/460gx-config-space-notes.md).  The card's eight rows are addressed
 * as two stacks of four: the stack by the I2C address (54h-57h and 50h-53h
 * = DIMM 0-3 of a row in either stack), the row within the stack by which of
 * the MAC's functions 4-7 has bit 0 of its register 48h set -- the firmware
 * raises exactly one before it reads a row's four EEPROMs (POST F0 trace).
 *
 * The sizing loop reads bytes 2 (memory type, must say SDRAM), 3, 4, 5 and
 * 17 (row and column address bits, ranks, banks per device) and requires
 * the row's four DIMMs to match, so a row's size is
 * 2^(rows+columns) x banks x ranks x 8 bytes per DIMM, times four.  The
 * DIMM geometries below are Table 5-2's x72 SDRAM configurations, one per
 * size from 2Mx72 (16 MB) to 64Mx72x2 (1 GB), so a row holds 64 MB to 4 GB
 * -- "64 MB is the smallest increment" (Table 5-1).
 */
#define IA64_460GX_SAC_IIADR_REG 0x68
static const struct {
    uint32_t dimm_mb;
    uint8_t row_bits;
    uint8_t col_bits;
    uint8_t ranks;
    uint8_t banks;
    uint8_t width;      /* primary SDRAM device width, SPD byte 13 */
} ia64_460gx_dimm_geometries[] = {
    /* Table 5-2: 64M x 72 x 2, 256 Mbit 64Mx4, double sided */
    { 1024, 13, 11, 2, 4, 4 },
    /* 64M x 72, 256 Mbit 64Mx4 */
    {  512, 13, 11, 1, 4, 4 },
    /* 32M x 72, 128 Mbit 32Mx4 */
    {  256, 13, 10, 1, 4, 4 },
    /* 16M x 72, 64 Mbit 16Mx4 */
    {  128, 12, 10, 1, 4, 4 },
    /* 8M x 72, 64 Mbit 8Mx8 */
    {   64, 12,  9, 1, 4, 8 },
    /* 4M x 72, 16 Mbit 4Mx4 */
    {   32, 11, 10, 1, 2, 4 },
    /* 2M x 72, 16 Mbit 2Mx8 */
    {   16, 11,  9, 1, 2, 8 },
};

/*
 * Populate Memory Card A for the machine's RAM size: rows are filled from
 * the largest DIMM down, so 1 GiB is one row of 32Mx72 (the i2000's four
 * slots), and a size the card cannot hold exactly leaves the remainder
 * unpopulated (the caller rejects that where it matters).  Rows may differ
 * in DIMM type (SSDM 5.2.1: "Different rows may use different size DIMMs").
 * Returns the size populated, in bytes.
 */
static uint64_t ia64_460gx_plan_memory_rows(IA64460GXState *s,
                                            uint64_t ram_size)
{
    uint64_t left_mb = ram_size / MiB;
    unsigned row = 0, g;

    memset(s->mem_row_dimm_mb, 0, sizeof(s->mem_row_dimm_mb));
    for (g = 0; g < ARRAY_SIZE(ia64_460gx_dimm_geometries); g++) {
        uint32_t row_mb = ia64_460gx_dimm_geometries[g].dimm_mb * 4;

        while (row < IA64_460GX_MEM_ROWS && left_mb >= row_mb) {
            s->mem_row_dimm_mb[row++] = ia64_460gx_dimm_geometries[g].dimm_mb;
            left_mb -= row_mb;
        }
    }
    return ram_size - left_mb * MiB;
}

/*
 * One byte of the JEDEC SDRAM SPD image describing a DIMM of the given
 * size: the geometry bytes the sizing loop reads, plus the PC100 x72 ECC
 * identity bytes a stricter parser would check, and the byte-63 checksum.
 */
static uint8_t ia64_460gx_spd_byte(uint32_t dimm_mb, unsigned off)
{
    unsigned g;

    for (g = 0; g < ARRAY_SIZE(ia64_460gx_dimm_geometries); g++) {
        if (ia64_460gx_dimm_geometries[g].dimm_mb == dimm_mb) {
            break;
        }
    }
    if (g == ARRAY_SIZE(ia64_460gx_dimm_geometries) || off > 63) {
        return 0;
    }
    switch (off) {
    case 0:  return 128;    /* bytes written by manufacturer */
    case 1:  return 8;      /* log2 of EEPROM size (256 bytes) */
    case 2:  return 4;      /* memory type: SDRAM */
    case 3:  return ia64_460gx_dimm_geometries[g].row_bits;
    case 4:  return ia64_460gx_dimm_geometries[g].col_bits;
    case 5:  return ia64_460gx_dimm_geometries[g].ranks;
    case 6:  return 72;     /* module data width low */
    case 8:  return 1;      /* interface level: LVTTL */
    case 9:  return 0xa0;   /* cycle time 10 ns (PC100) */
    case 11: return 2;      /* ECC */
    case 12: return 0x82;   /* refresh: self-refresh, 15.6 us */
    case 13: return ia64_460gx_dimm_geometries[g].width;
    case 17: return ia64_460gx_dimm_geometries[g].banks;
    case 18: return 4;      /* CAS latencies supported */
    case 31:                /* rank density, 4 MB units, bit per size */
        return (dimm_mb / ia64_460gx_dimm_geometries[g].ranks) / 4;
    case 63: {
        unsigned sum = 0, i;

        for (i = 0; i < 63; i++) {
            sum += ia64_460gx_spd_byte(dimm_mb, i);
        }
        return sum;
    }
    default:
        return 0;
    }
}

/* Bus 0 device 10h: the SAC face that carries the Chipset Bus Number. */
static uint8_t *ia64_460gx_cbn_window(IA64460GXState *s, uint8_t fn)
{
    return s->chipset_cfg +
           ARRAY_SIZE(ia64_460gx_chipset_devs) * IA64_460GX_CFG_DEV_SIZE +
           fn * IA64_460GX_CFG_FN_SIZE;
}

static uint8_t ia64_460gx_cbn(IA64460GXState *s)
{
    return ia64_460gx_cbn_window(s, 0)[IA64_460GX_CBN_REG];
}

static uint8_t *ia64_460gx_chipset_cfg(IA64460GXState *s,
                                        uint8_t bus, uint8_t dev, uint8_t fn)
{
    unsigned i;

    /*
     * The CBN window answers on bus 0 whatever CBN itself holds, so resolve
     * it before consulting CBN -- which also keeps the two apart if firmware
     * ever programs CBN to 0.  Everything else lives on bus CBN.
     */
    if (bus == 0 && dev == IA64_460GX_CBN_DEV) {
        return ia64_460gx_cbn_window(s, fn);
    }
    if (bus != ia64_460gx_cbn(s)) {
        return NULL;
    }
    for (i = 0; i < ARRAY_SIZE(ia64_460gx_chipset_devs); i++) {
        if (ia64_460gx_chipset_devs[i] == dev) {
            return s->chipset_cfg + i * IA64_460GX_CFG_DEV_SIZE +
                   fn * IA64_460GX_CFG_FN_SIZE;
        }
    }
    return NULL;
}

/*
 * The byte a SAC function-0 register-file access lands on: 70h-73h is a window
 * onto the entry 64h selects, so it comes from the file rather than from the
 * device's own config storage.  Any other device, function or offset stays
 * where it was.
 */
static uint8_t *ia64_460gx_sac_indexed(IA64460GXState *s, uint8_t dev,
                                       uint8_t fn, const uint8_t *cfg,
                                       unsigned off)
{
    if (fn != 0 || dev > 0x01 ||
        off < IA64_460GX_SAC_IDX_DATA ||
        off >= IA64_460GX_SAC_IDX_DATA + 4) {
        return NULL;
    }
    return &s->sac_indexed[dev][cfg[IA64_460GX_SAC_IDX_REG]]
                          [off - IA64_460GX_SAC_IDX_DATA];
}

/*
 * The expander ports this board populates, in Table 2-1's device numbers,
 * and the root each one's PCI bus is modelled as: Expander 1 (the WXB) at
 * 12h/13h with its buses a and b, Expander 2 (the GXB) at 14h.  Expander 0
 * is the compatibility bus, handled before these are consulted.
 */
#define IA64_460GX_ROOT_COMPAT   (-1)
static const struct {
    uint8_t dev;
    int root;
} ia64_460gx_expander_ports[] = {
    { 0x10, IA64_460GX_ROOT_COMPAT },
    { 0x12, IA64_460GX_ROOT_WXB0 },
    { 0x13, IA64_460GX_ROOT_WXB1 },
    { 0x14, IA64_460GX_ROOT_GXB },
};
#define IA64_460GX_ROOT_NONE     (-2)

static int ia64_460gx_expander_port_root(uint8_t dev)
{
    unsigned i;

    for (i = 0; i < ARRAY_SIZE(ia64_460gx_expander_ports); i++) {
        if (ia64_460gx_expander_ports[i].dev == dev) {
            return ia64_460gx_expander_ports[i].root;
        }
    }
    return IA64_460GX_ROOT_NONE;
}

/*
 * The variable gap below 4G - 32M is carved among the expander ports by
 * their PCIS registers: each port decodes from PCIS x 32M up to the next
 * port's PCIS, and the highest one up to the fixed ranges (SSDM 4.1.3.1,
 * "PCIS[7] - FDFF_FFFFh -> PCIx").  The vendor DSDT hands those exact slices
 * out as the root windows: PCI0 [PCIS(10h), FE000000), PCI1 [PCIS(12h),
 * PCIS(10h)), PCI3 [PCIS(14h), PCIS(13h)).  All four roots share one PCI
 * memory space here, so a single alias from the lowest programmed PCIS to
 * the fixed aperture covers every window a guest can place a BAR in.  00h
 * (reset) and FFh (what the firmware writes for an empty port) carry no
 * window.
 */
static void ia64_460gx_update_low_mmio_window(IA64460GXState *s)
{
    uint8_t cbn = ia64_460gx_cbn(s);
    uint64_t base = ~0ULL;
    unsigned i;

    for (i = 0; i < ARRAY_SIZE(ia64_460gx_expander_ports); i++) {
        const uint8_t *cfg = ia64_460gx_chipset_cfg(
            s, cbn, ia64_460gx_expander_ports[i].dev, 0);
        uint8_t pcis;

        if (cfg == NULL) {
            continue;
        }
        pcis = cfg[IA64_460GX_XXB_PCIS_REG];
        if (pcis == 0x00 || pcis == 0xff) {
            continue;
        }
        base = MIN(base, (uint64_t)pcis << 25);
    }
    ia64_pci_host_set_low_mmio_window(s->pci_host, base);
    s->window_notify(s->window_opaque, base);
}

/*
 * Find the device a configuration address names.  "If the Bus Number is not
 * CBN, the destination and type of access is determined by the Bus Number
 * and Subordinate Bus Number of each PCI port in each PXB.  A type 0 access
 * is generated on the appropriate PCI bus if one of the PXB port's bus number
 * is matched.  Otherwise, a type 1 configuration cycle is generated on the
 * appropriate PCI bus below the PXB port whose subordinate bus number is in
 * that range" (SSDM 2.3.1).  So a port whose firmware-programmed pair
 * brackets the bus claims the cycle: its own bus number reaches the root's
 * children, a higher one descends through the bridges below, which carry
 * the numbers the same firmware gave them.  Bus 0 is the compatibility bus
 * (Table 2-1) whatever its port's pair says, and is never looked up.
 *
 * A bus no port claims falls back to the board's fixed numbering -- the
 * numbers both firmwares give the ports -- so a cycle to those buses keeps
 * working before POST has programmed them.
 */
static PCIDevice *ia64_460gx_cfg_find_device(IA64460GXState *s,
                                             uint8_t bus, uint8_t devfn)
{
    PCIDevice *pci_dev;
    unsigned int i;

    if (bus != 0) {
        uint8_t cbn = ia64_460gx_cbn(s);

        for (i = 0; i < ARRAY_SIZE(ia64_460gx_expander_ports); i++) {
            int root_index = ia64_460gx_expander_ports[i].root;
            PCIBus *root = root_index == IA64_460GX_ROOT_COMPAT
                ? s->compat_bus : s->root_bus[root_index];
            const uint8_t *cfg = ia64_460gx_chipset_cfg(
                s, cbn, ia64_460gx_expander_ports[i].dev, 0);
            uint8_t busno, subno;

            if (root == NULL || cfg == NULL) {
                continue;
            }
            busno = cfg[IA64_460GX_XXB_BUSNO_REG];
            subno = cfg[IA64_460GX_XXB_SUBNO_REG];
            if (busno == 0) {
                continue;
            }
            /*
             * The port's own number is a type 0 cycle whatever SUBNO holds:
             * firmware writes BUSNO, scans the bus, and only then raises
             * SUBNO, so the scan must already reach the port's devices.
             */
            if (bus == busno) {
                return pci_find_device(root, pci_bus_num(root), devfn);
            }
            if (bus > busno && bus <= subno) {
                return pci_find_device(root, bus, devfn);
            }
        }
    }

    pci_dev = pci_find_device(s->compat_bus, bus, devfn);
    for (i = 0; pci_dev == NULL && i < ARRAY_SIZE(s->root_bus); i++) {
        if (s->root_bus[i] != NULL) {
            pci_dev = pci_find_device(s->root_bus[i], bus, devfn);
        }
    }
    return pci_dev;
}

/*
 * The DIMM row the firmware's SPD access names, or -1: the IIADR address
 * picks the stack, the one MAC function 4-7 whose register 48h bit 0 is
 * raised picks the row in it.  The four DIMMs of a row are identical, so
 * the address's DIMM number is not needed.
 */
static int ia64_460gx_spd_row(IA64460GXState *s, uint8_t bus)
{
    const uint8_t *sac = ia64_460gx_chipset_cfg(s, bus, 0, 0);
    int row = -1, stack;
    unsigned fn;

    if (sac == NULL) {
        return -1;
    }
    switch (sac[IA64_460GX_SAC_IIADR_REG] & 0xfc) {
    case 0xd4:
        stack = 0;
        break;
    case 0xd0:
        stack = 1;
        break;
    default:
        return -1;
    }
    for (fn = 4; fn < 8; fn++) {
        const uint8_t *r = ia64_460gx_chipset_cfg(s, bus, 0x05, fn);

        if (r == NULL) {
            return -1;
        }
        if (r[0x48] & 1) {
            if (row >= 0) {
                return -1;
            }
            row = stack * 4 + (fn - 4);
        }
    }
    return row;
}

static uint64_t ia64_460gx_cfg_read(void *opaque, hwaddr addr, unsigned size)
{
    IA64460GXState *s = opaque;
    uint32_t cf8 = s->cfg_address;
    uint8_t bus = cf8 >> 16, dev = (cf8 >> 11) & 0x1f, fn = (cf8 >> 8) & 7;
    unsigned reg = (cf8 & 0xfc) | (addr & 3);
    uint8_t *cfg;
    uint64_t val = 0;
    unsigned i;

    if (addr < 4) {
        return s->cfg_address >> (addr * 8);
    }
    if (!(cf8 & 0x80000000)) {
        return (1ULL << (size * 8)) - 1;
    }
    cfg = ia64_460gx_chipset_cfg(s, bus, dev, fn);
    if (cfg != NULL && dev == 0x05 && (fn == 2 || fn == 3)) {
        /*
         * MAC I2C pass-through: serve the addressed DIMM's SPD EEPROM.  An
         * unpopulated row answers zeros, which the sizing loop takes as
         * "no SDRAM here" from byte 2 and moves on.  The SAC and the MAC's
         * row-select functions are looked up on the bus the firmware used
         * (CBN, which it reprograms late in POST), not a fixed one.
         */
        int row = ia64_460gx_spd_row(s, bus);

        if (row >= 0 && s->mem_row_dimm_mb[row] != 0) {
            for (i = 0; i < size; i++) {
                val |= (uint64_t)ia64_460gx_spd_byte(s->mem_row_dimm_mb[row],
                                                     (reg + i) & 0xff)
                       << (i * 8);
            }
        }
    } else if (cfg != NULL && (reg & 0xfc) == 0x30) {
        /*
         * The 460GX chipset functions carry no expansion ROM, so their ROM BAR
         * (0x30) must read back 0.  The config store is a plain write/read-back
         * cell, so without this it returns whatever the firmware last wrote --
         * during BAR sizing that is 0xFFFFFFFE, which the firmware decodes as a
         * 2 KiB ROM.  A real add-in card at the same dev number (our ATI video
         * shares dev 5 with the MAC while CBN still reads 0) then has its 64 KiB
         * video ROM sized as 2 KiB, and the next option ROM is shadowed on top
         * of the video ROM body -- corrupting the vgabios INT10 handler.
         */
        val = 0;
    } else if (cfg != NULL) {
        for (i = 0; i < size; i++) {
            unsigned off = (reg + i) & 0xff;
            const uint8_t *file = ia64_460gx_sac_indexed(s, dev, fn, cfg, off);

            val |= (uint64_t)(file != NULL ? *file : cfg[off]) << (i * 8);
        }
    } else if (bus == ia64_460gx_cbn(s)) {
        /*
         * "On the bus that the chipset is mapped into (determined by the CBN
         * register), Device Numbers 0-31 are reserved for the 460GX chipset
         * components as shown in Table 2-1.  All other devices numbers are
         * forwarded to the selected bus" (SSDM 2.3.1).  Every device number
         * on bus CBN is therefore chipset space: one this board does not
         * populate answers as absent rather than being looked for on a PCI
         * bus, which is what keeps the two apart should CBN ever land on a
         * bus number a root actually carries.
         */
        val = (1ULL << (size * 8)) - 1;
    } else {
        PCIDevice *pci_dev = ia64_460gx_cfg_find_device(s, bus,
                                                        PCI_DEVFN(dev, fn));

        val = pci_dev != NULL
            ? pci_host_config_read_common(pci_dev, reg,
                                          pci_config_size(pci_dev), size)
            : (1ULL << (size * 8)) - 1;
    }
    qemu_log_mask(LOG_UNIMP, "ia64-460gx: cfg%c read  %02x:%02x.%x "
                  "@0x%02x/%u = 0x%" PRIx64 "\n", cfg ? '*' : ' ',
                  bus, dev, fn, reg, size, val);
    return val;
}

static void ia64_460gx_cfg_write(void *opaque, hwaddr addr, uint64_t data,
                                  unsigned size)
{
    IA64460GXState *s = opaque;
    uint32_t cf8 = s->cfg_address;
    uint8_t bus = cf8 >> 16, dev = (cf8 >> 11) & 0x1f, fn = (cf8 >> 8) & 7;
    unsigned reg = (cf8 & 0xfc) | (addr & 3);
    uint8_t *cfg;
    unsigned i;

    if (addr == 1 && size == 1) {
        /*
         * Port 0xCF9 (RST_CNT): an 8-bit access is the legacy PC reset-control
         * register, aliased with byte 1 of the 0xCF8 config-address register.
         * Software addresses the config register with dword writes, so only a
         * byte access here is the reset control -- as on real hardware, which
         * aliases them the same way.  RST_CPU (bit 2) resets the system.
         */
        qemu_log_mask(LOG_UNIMP, "ia64-460gx: CF9 write 0x%02x\n",
                      (unsigned)(data & 0xff));
        if (data & 0x04) {
            qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
            return;
        }
    }

    if (addr < 4) {
        for (i = 0; i < size && addr + i < 4; i++) {
            s->cfg_address &= ~(0xffU << ((addr + i) * 8));
            s->cfg_address |= ((data >> (i * 8)) & 0xff) <<
                                        ((addr + i) * 8);
        }
        return;
    }
    if (!(cf8 & 0x80000000)) {
        return;
    }
    cfg = ia64_460gx_chipset_cfg(s, bus, dev, fn);
    qemu_log_mask(LOG_UNIMP, "ia64-460gx: cfg%c write %02x:%02x.%x "
                  "@0x%02x/%u = 0x%" PRIx64 "\n", cfg ? '*' : ' ',
                  bus, dev, fn, reg, size, data);
    if (cfg != NULL) {
        for (i = 0; i < size; i++) {
            unsigned off = (reg + i) & 0xff;
            uint8_t *file = ia64_460gx_sac_indexed(s, dev, fn, cfg, off);
            uint8_t byte = data >> (i * 8);

            if (file != NULL) {
                *file = byte;
            } else {
                cfg[off] = byte;
            }
            if (bus != 0 && fn == 0 && off == IA64_460GX_XXB_PCIS_REG &&
                ia64_460gx_expander_port_root(dev) != IA64_460GX_ROOT_NONE) {
                ia64_460gx_update_low_mmio_window(s);
            }
        }
        /*
         * Re-base an above-4-GiB GXB AGP aperture below 4 GiB (see
         * IA64_460GX_GXB_BAPBASE_REG above).  Clamp only when the stored 64-bit
         * BAPBASE actually names an address at or above 4 GiB, so the firmware's
         * 0x1_00000000 is corrected while a legitimate below-4-GiB base -- such
         * as agp460's own AgpSetAperture write-back -- is stored verbatim.  The
         * check runs when this access touches the register (its high dword
         * arrives as a separate size-4 write at 0x9c, per the POST trace).
         */
        if (bus != 0 && dev == IA64_460GX_GXB_DEV &&
            fn == IA64_460GX_GXB_BRIDGE_FN &&
            reg <= IA64_460GX_GXB_BAPBASE_LAST &&
            reg + size > IA64_460GX_GXB_BAPBASE_REG) {
            uint64_t bap = ldq_le_p(cfg + IA64_460GX_GXB_BAPBASE_REG);

            if (bap >> 32) {
                stq_le_p(cfg + IA64_460GX_GXB_BAPBASE_REG,
                         IA64_460GX_GXB_AGP_APERTURE_BASE);
            }
        }
        return;
    }
    if (bus == ia64_460gx_cbn(s)) {
        return;     /* an unpopulated chipset device number (SSDM 2.3.1) */
    }
    {
        PCIDevice *pci_dev = ia64_460gx_cfg_find_device(s, bus,
                                                        PCI_DEVFN(dev, fn));

        if (pci_dev != NULL) {
            pci_host_config_write_common(pci_dev, reg,
                                         pci_config_size(pci_dev),
                                         data, size);
        }
    }
}

static const MemoryRegionOps ia64_460gx_cfg_ops = {
    .read = ia64_460gx_cfg_read,
    .write = ia64_460gx_cfg_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

/*
 * Seed one 460GX chipset function's PCI header.  Subsystem ids repeat the
 * vendor and device id, as the real parts report, and DEVSEL timing is
 * medium.  The rest of the function's config space stays read/write
 * scratch.
 */
static void ia64_460gx_init_chipset_identity(IA64460GXState *s,
                                                  uint8_t dev, uint8_t fn,
                                                  uint16_t device_id,
                                                  uint8_t revision,
                                                  uint16_t class_id,
                                                  bool multifunction)
{
    uint8_t cbn = ia64_460gx_cbn(s);
    uint8_t *cfg = ia64_460gx_chipset_cfg(s, cbn, dev, fn);

    if (cfg == NULL) {
        return;
    }
    stw_le_p(cfg + PCI_VENDOR_ID, PCI_VENDOR_ID_INTEL);
    stw_le_p(cfg + PCI_DEVICE_ID, device_id);
    stw_le_p(cfg + PCI_STATUS, PCI_STATUS_DEVSEL_MEDIUM);
    cfg[PCI_REVISION_ID] = revision;
    stw_le_p(cfg + PCI_CLASS_DEVICE, class_id);
    stw_le_p(cfg + PCI_SUBSYSTEM_VENDOR_ID, PCI_VENDOR_ID_INTEL);
    stw_le_p(cfg + PCI_SUBSYSTEM_ID, device_id);
    if (multifunction) {
        cfg[PCI_HEADER_TYPE] = PCI_HEADER_TYPE_MULTI_FUNCTION;
    }
}

static void ia64_460gx_reset_cfg(IA64460GXState *s)
{
    uint8_t *cbn_window;
    uint8_t *mac_a;

    if (s->chipset_cfg == NULL) {
        s->chipset_cfg = g_malloc0(IA64_460GX_CFG_SIZE);
    } else {
        memset(s->chipset_cfg, 0, IA64_460GX_CFG_SIZE);
    }
    s->cfg_address = 0;
    if (s->pci_host != NULL) {
        ia64_pci_host_set_low_mmio_window(s->pci_host, ~0ULL);
    }
    /*
     * CBN comes out of reset as FFh: the chipset's own functions answer on
     * bus FF until firmware moves them.  The vendor firmware programs its
     * SAC and expander ports there throughout POST and only writes 0xEE at
     * the end of enumeration, so a store that reset CBN to 0 both dropped
     * all of that programming and shadowed the compatibility bus's real
     * devices 00h-05h behind the chipset's.  This has to happen before the
     * identity seeding below, which resolves every device through CBN and
     * would otherwise seed bus 0's blocks.
     */
    cbn_window = ia64_460gx_cbn_window(s, 0);
    cbn_window[IA64_460GX_CBN_REG] = 0xff;
    /*
     * The SSDM says the device holding CBN *is* the SAC, so its window
     * carries the SAC identity rather than the expander port's.
     */
    stw_le_p(cbn_window + PCI_VENDOR_ID, PCI_VENDOR_ID_INTEL);
    stw_le_p(cbn_window + PCI_DEVICE_ID, 0x84e0);
    stw_le_p(cbn_window + PCI_STATUS, PCI_STATUS_DEVSEL_MEDIUM);
    cbn_window[PCI_REVISION_ID] = 0x03;
    stw_le_p(cbn_window + PCI_CLASS_DEVICE, PCI_CLASS_BRIDGE_HOST);
    stw_le_p(cbn_window + PCI_SUBSYSTEM_VENDOR_ID, PCI_VENDOR_ID_INTEL);
    stw_le_p(cbn_window + PCI_SUBSYSTEM_ID, 0x84e0);
    cbn_window[PCI_HEADER_TYPE] = PCI_HEADER_TYPE_MULTI_FUNCTION;
    /*
     * The chipset's own functions carry their real identities.  Without
     * them a firmware config read of the SAC, SDC or expander returns a
     * zero vendor id, which is neither "present" nor the architected
     * "absent" 0xffff.  Device ids, revisions and classes per the 460GX
     * SSDM Table 2-1 and upstream's intel_460gx_chipset.c (fda8a29);
     * expander device numbers per plans/sdv-i2000-firmware-reference.md,
     * which places expander port n at bus CBN device 10h + n.
     */
    ia64_460gx_init_chipset_identity(s, 0x00, 0, 0x84e0, 0x03,
                                          PCI_CLASS_BRIDGE_HOST, true);
    ia64_460gx_init_chipset_identity(s, 0x01, 0, 0x84e0, 0x03,
                                          PCI_CLASS_BRIDGE_HOST, false);
    ia64_460gx_init_chipset_identity(s, 0x04, 0, 0x84e1, 0x03,
                                          PCI_CLASS_BRIDGE_HOST, false);
    /*
     * Expander port 0 is the PXB, which hosts the compatibility bus.  Its
     * function 0 is the downstream SAC; function 1 is the bridge itself.
     * Function 0 register 40h is also where the firmware programs CBN
     * (observed; see ia64_460gx_chipset_cfg), so only the header is
     * seeded here.
     */
    ia64_460gx_init_chipset_identity(s, IA64_460GX_CBN_DEV, 0,
                                          0x84e0, 0x03,
                                          PCI_CLASS_BRIDGE_HOST, true);
    ia64_460gx_init_chipset_identity(s, IA64_460GX_CBN_DEV, 1,
                                          0x84cb, 0x05,
                                          PCI_CLASS_BRIDGE_HOST, false);
    /*
     * The other expander ports the i2000 populates, at Table 2-1's
     * device numbers: Expander 1 is the WXB with its two buses (12h/13h,
     * fn1 the bridge, 84E6), Expander 2 the GXB (14h; fn1 the AGP bridge
     * 84EA, fn2 its GART function 84E2).  Each port's fn0 is its
     * downstream SAC face, as on port 0.  The vendor firmware scans for
     * exactly these (ee:12.0/13.0/14.0 @48h, ee:12.1 @80h, ee:14.1/14.2) and
     * decides which buses carry a window -- the graphics card behind the GXB
     * stays disabled unless its port exists.
     */
    ia64_460gx_init_chipset_identity(s, 0x12, 0, 0x84e0, 0x03,
                                          PCI_CLASS_BRIDGE_HOST, true);
    ia64_460gx_init_chipset_identity(s, 0x12, 1, 0x84e6, 0x07,
                                          PCI_CLASS_BRIDGE_HOST, false);
    ia64_460gx_init_chipset_identity(s, 0x13, 0, 0x84e0, 0x03,
                                          PCI_CLASS_BRIDGE_HOST, true);
    ia64_460gx_init_chipset_identity(s, 0x13, 1, 0x84e6, 0x07,
                                          PCI_CLASS_BRIDGE_HOST, false);
    ia64_460gx_init_chipset_identity(s, 0x14, 0, 0x84e0, 0x03,
                                          PCI_CLASS_BRIDGE_HOST, true);
    ia64_460gx_init_chipset_identity(s, 0x14, 1, 0x84ea, 0x02,
                                          PCI_CLASS_BRIDGE_HOST, false);
    ia64_460gx_init_chipset_identity(s, 0x14, 2, 0x84e2, 0x02,
                                          PCI_CLASS_BRIDGE_HOST, false);

    /*
     * The SAC's device-specific registers (40h-FFh of devices 00h/01h) are
     * not documented in the SSDM.  The vendor firmware's early POST reads
     * them before writing anything (it selects a register through 78h and
     * reads 60h, and checks DEVNPRES at 70h), and it stops at POST 0x54 when
     * they read as zero while it passes when they read as open bus -- which
     * is what they were on this machine before bus FF was modelled.  Seed
     * them with 0xFF and let writes stick.  The memory card (dev 05h) must
     * not be seeded: its SPD/I2C tunnel reading 0xFF fails memory init at
     * POST 0xF1.
     */
    /*
     * The register file behind 64h/70h resets to the chipset's device-present
     * word rather than to open bus.  The SSDM never lays the register out,
     * but it names it and numbers its bits: "The DEVNPRES register is used
     * to determine which chipset devices are present; see Table 2-1"
     * (2.2.1), and the memory map keys the AGP GART range on "DEVNPRES[14]"
     * -- "If the DEVNPRES bit for Device 14 is set (meaning that there is no
     * xXB attached to the Expander bus)" (4.1.3.1).  So bit n is Table 2-1's
     * device n, and a set bit means absent.
     *
     * The vendor firmware reads that word through this window, right after
     * its walk over the entries (pciHost.c, link 0x4b0ca0 of SAL_B): bit 20
     * set makes it record "GXB Status" = 1 under "/IO/Bus/PCI", set bits
     * 20-23 itself, and later park expander ports 14h-18h onto the WXB's bus
     * (link 0x4b0670) -- which is what left the vendor DSDT's PCI1 and PCI3
     * roots on one bus number, and both of them Code 12 under Windows.  An
     * all-ones seed says every expander port is missing, the GXB included.
     * Seed every entry with the populated devices instead; writes still
     * stick per entry.
     */
    {
        uint32_t devnpres = ~0u;
        unsigned i, e;

        for (i = 0; i < ARRAY_SIZE(ia64_460gx_chipset_devs); i++) {
            devnpres &= ~(1u << ia64_460gx_chipset_devs[i]);
        }
        for (i = 0; i < ARRAY_SIZE(s->sac_indexed); i++) {
            for (e = 0; e < IA64_460GX_SAC_IDX_ENTRIES; e++) {
                stl_le_p(s->sac_indexed[i][e], devnpres);
            }
        }
    }

    {
        unsigned i, fn;

        for (i = 0; i < ARRAY_SIZE(ia64_460gx_chipset_devs); i++) {
            if (ia64_460gx_chipset_devs[i] > 0x01) {
                continue;
            }
            for (fn = 0; fn < 8; fn++) {
                memset(s->chipset_cfg + i * IA64_460GX_CFG_DEV_SIZE +
                       fn * IA64_460GX_CFG_FN_SIZE + 0x40, 0xff, 0xc0);
            }
            /*
             * Function 0's 60h, read as a byte after the firmware writes 0
             * to 78h, is the number of expanders the SAC has ports for
             * (SSDM Table 2-1: Expander 0-3 at 10h-17h).  The vendor
             * firmware loops that many times over devices 10h, 12h, 14h,
             * ... testing each one's DEVNPRES bit, and hands the ports it
             * finds to its bus-numbering loop.  Read as open bus the count
             * is 255: the loop wraps through every device number, treats
             * the wrapped ones as present, and numbers 16 rounds of phantom
             * ports before the real ones, which leaves the compatibility
             * bus at D0h/D6h instead of 0 and the GXB's bus off the 4 its
             * IA-32 CSM addresses the VGA card at.  The firmware also
             * read-modify-writes the dword to clear bit 0, so the count
             * lives in the same cell and writes stick.
             */
            stl_le_p(s->chipset_cfg + i * IA64_460GX_CFG_DEV_SIZE + 0x60,
                     IA64_460GX_EXPANDER_COUNT);
        }
    }

    /*
     * Memory Card A (dev 05h fn 0) claims presence with the MAC identity
     * (8086:84E3, rev B-1 = 03h; pci.ids, flagged unverified in
     * plans/460gx-config-space-notes.md).  Memory Card B stays absent.
     */
    mac_a = ia64_460gx_chipset_cfg(s, 0xff, 0x05, 0);
    stw_le_p(mac_a + PCI_VENDOR_ID, 0x8086);
    stw_le_p(mac_a + PCI_DEVICE_ID, 0x84e3);
    mac_a[PCI_REVISION_ID] = 0x03;

    ia64_460gx_plan_memory_rows(s, s->ram_size);
}

/*
 * The chipset answers PCI configuration cycles at the architected CF8/CFC
 * port pair (460GX SSDM 2.3): CONFIG_ADDRESS at 0xCF8 latches bus, device,
 * function and register, and a CFC access reads or writes the addressed
 * config space.  Device numbers on the bus the CBN register maps the chipset
 * into are the chipset's own functions (Table 2-1); everything else forwards
 * to the PCI bus.  This is the only configuration mechanism the chipset
 * has: both firmwares enumerate through it, and guests reach it through
 * SAL_PCI_CONFIG (the 460gx machine maps no ECAM window).
 */
static void ia64_460gx_realize(DeviceState *dev, Error **errp)
{
    IA64460GXState *s = IA64_460GX(dev);

    s->sac_data = g_malloc0(IA64_460GX_SAC_SIZE);
    memory_region_init_io(&s->sac_mmio, OBJECT(s), &ia64_460gx_sac_ops, s,
                          "ia64-460gx.sac", IA64_460GX_SAC_SIZE);
    memory_region_init_io(&s->post_io, OBJECT(s), &ia64_460gx_post_ops, s,
                          "ia64-460gx.post", 2);
    memory_region_init_io(&s->cfg_io, OBJECT(s),
                          &ia64_460gx_cfg_ops, s, "ia64-460gx.cfg", 8);
    ia64_460gx_reset_cfg(s);
}

/*
 * The configuration store (CBN, the chipset functions' BARs and command
 * registers, and the CF8 config-address latch) models chipset state that a
 * real SYS_RST/RST_CPU (port 0xCF9) clears.  Without this a warm reset would
 * leave it holding the previous boot's programming (a non-zero CBN and
 * assigned BARs); the firmware's re-enumeration then takes a different path
 * and the second boot's video-ROM POST diverges (it hangs in a vgabios
 * timed-delay whose INT8 tick never advances).
 *
 * The SAC's scratch block keeps its contents instead: SAL_A hands the result
 * of one boot pass to the next one there, across a reset it asks for itself.
 * Its recovery-check pass sizes and initializes the DRAM, sets bit 0 of the
 * word at +0xCB0 (bios130.BIN @0xFFFF4F98) and resets the platform through
 * port 0xCF9 (0x02 then 0x06); the pass after the reset reads that word
 * (@0xFFFF4B80: "ld4.acq r35=[r34]; tbit.z p7,p6=r35,0"), finds bit 0 set and
 * branches past the memory initialization.  The BSP-arbitration read-modify-
 * write at @0xFFFF31F0 clears only bit 7 of the same word, so the hand-off
 * survives arbitration too.  Clearing the block made the vendor firmware
 * repeat the initialization and the reset without end: 21 resets in 45 s, no
 * POST code.  The SSDM does not publish this register, and it does define
 * registers whose data "remains valid and unchanged, during and following a
 * hard reset" (sec 2.2.2) -- sticky is the attribute the firmware's own use
 * asks for [inferred].
 *
 * Only the write-once BSP-select word is released, so the processors
 * arbitrate for the boot role again on every reset.
 */
static void ia64_460gx_reset(DeviceState *dev)
{
    IA64460GXState *s = IA64_460GX(dev);

    ia64_460gx_reset_cfg(s);
    s->sac_data[IA64_460GX_SAC_BOOT_SEM] = 0;
    s->post_last = 0;
}

IA64460GXState *ia64_460gx_create(Object *parent, MemoryRegion *pci_io,
                                  DeviceState *pci_host, uint64_t ram_size,
                                  IA64460GXWindowNotify notify, void *opaque,
                                  Error **errp)
{
    DeviceState *dev = qdev_new(TYPE_IA64_460GX);
    IA64460GXState *s = IA64_460GX(dev);

    object_property_add_child(parent, "460gx", OBJECT(dev));
    s->pci_host = pci_host;
    s->ram_size = ram_size;
    s->window_notify = notify;
    s->window_opaque = opaque;
    if (!sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), errp)) {
        return NULL;
    }
    memory_region_add_subregion(get_system_memory(), IA64_460GX_SAC_BASE,
                                &s->sac_mmio);
    memory_region_add_subregion(pci_io, 0x80, &s->post_io);
    memory_region_add_subregion(pci_io, 0xcf8, &s->cfg_io);
    return s;
}

void ia64_460gx_attach_root(IA64460GXState *s, int root, PCIBus *bus)
{
    if (root < 0) {
        s->compat_bus = bus;
    } else {
        g_assert(root < IA64_460GX_EXPANDER_ROOTS);
        s->root_bus[root] = bus;
    }
}

static void ia64_460gx_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = ia64_460gx_realize;
    device_class_set_legacy_reset(dc, ia64_460gx_reset);
    /* Created by the board, not on the command line. */
    dc->user_creatable = false;
}

static const TypeInfo ia64_460gx_info = {
    .name = TYPE_IA64_460GX,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(IA64460GXState),
    .class_init = ia64_460gx_class_init,
};

static void ia64_460gx_register_types(void)
{
    type_register_static(&ia64_460gx_info);
}

type_init(ia64_460gx_register_types)
