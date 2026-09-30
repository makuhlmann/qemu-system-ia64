/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Intel 460GX GXB AGP bridge and GART.
 *
 * The GXB (82465GX) is expander port 2 of the SAC, device 14h on the chipset
 * bus (SSDM Table 2-1), and the vendor DSDT keeps that bus from the OS
 * (CBN._STA is 08h).  Its function 1 holds the aperture registers, which
 * firmware and Windows' agp460 program through CF8/CFC on bus CBN;
 * ia64_460gx.c sends those offsets here.  The GART is SRAM on the GXB,
 * programmed through the fixed window at FE20_0000 (SSDM 7.1.2); a GATT entry
 * translates one aperture page to a 36-bit physical page.
 *
 * The SSDM does not lay out function 1.  The drivers that use it agree on:
 * GXBCTL (A0h) bit 1 selects 4 MB GART pages; AGPSIZ (A2h) bits 2:0 give the
 * size (1 = 256 MB, 2 = 1 GB, 4 = 32 GB, 0 = none, the power-on value), bit 3
 * selects BAPBASE and bit 4 turns the SRAM I/O off (Linux i460-agp.c).  The
 * aperture base is the header BAR APBASE (10h) while bit 3 is clear and
 * BAPBASE (98h) while it is set; both are 64 bits wide, only the selected one
 * shows, and neither without a size
 * (WXPSP1/NT/base/busdrv/agp/agp460/agp460.h:41-47).  SSDM 7.2.1 gives the
 * two uses: BAPBASE for an aperture above the top of memory, "exactly the
 * same as the standard PCI-defined BAR", and the standard BAR in the PCIS
 * range of device 14h.  The vendor firmware uses both: BAPBASE at max(4 GiB,
 * TOM), and when an AGP master answers, bit 3 clear and only the low dword
 * of APBASE written below 4 GiB (`sal_b` 4B513C-4B595C), so the two keep
 * their own contents.  Bits 27:12 are hardwired (7.1), the size clears the
 * bits below it as in any BAR, and the GXB decodes 40 bits.
 *
 * The SAC does not decode the aperture for the processor (Table 4-1): it
 * exists only on the AGP master's side, as the DMA translation below.
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "hw/ia64/ia64_agp.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_device.h"
#include "hw/core/qdev-properties.h"
#include "system/address-spaces.h"
#include "qapi/error.h"

/*
 * The GART SRAM window.  The GXB has SADDR[17:0] and SDATA[31:0] (Datasheet
 * p.2-12), so it decodes 1 MB of 32-bit entries at FE20_0000; FE30_0000 and
 * up are "not decoded by the GXB as a GART access" (SSDM 7.1.2).
 */
#define I460_GART_WINDOW_BASE   0x00000000fe200000ULL
#define I460_GART_DECODE        (1 * MiB)

#define I460_APBASE             PCI_BASE_ADDRESS_0
#define I460_BAPBASE            0x98
#define I460_GXBCTL             0xa0
#define I460_AGPSIZ             0xa2
#define I460_AGP_CAP            0xe0

#define I460_GXBCTL_4M_PS       0x02
#define I460_AGPSIZ_SIZE_MASK   0x07
#define I460_AGPSIZ_SIZE_256M   0x01
#define I460_AGPSIZ_SIZE_1G     0x02
#define I460_AGPSIZ_SIZE_32G    0x04
#define I460_AGPSIZ_BAPBASE_EN  0x08
#define I460_AGPSIZ_SRAM_IO_DIS 0x10
#define I460_AGPSIZ_WRITABLE    0x1f

/* Base bits 39:28: 40-bit decode (7.2.1), 27:12 hardwired (7.1). */
#define I460_BASE_BITS          0x000000fff0000000ULL

/*
 * AGP status: sideband, fast writes and 1x/2x/4x (SSDM 1.5.3), 16
 * outstanding requests (p.2-25, p.6-24), and the 4G bit that the Spec
 * Update's GXB erratum 2 describes.
 */
#define I460_AGP_STATUS         ((15u << 24) | PCI_AGP_STATUS_SBA | \
                                 PCI_AGP_STATUS_64BIT | PCI_AGP_STATUS_FW | \
                                 PCI_AGP_STATUS_RATE4 | PCI_AGP_STATUS_RATE2 | \
                                 PCI_AGP_STATUS_RATE1)
/* The request depth is the master's field; a target has none. */
#define I460_AGP_COMMAND_WRITABLE \
    (PCI_AGP_COMMAND_SBA | PCI_AGP_COMMAND_AGP | PCI_AGP_COMMAND_64BIT | \
     PCI_AGP_COMMAND_FW | PCI_AGP_COMMAND_RATE4 | PCI_AGP_COMMAND_RATE2 | \
     PCI_AGP_COMMAND_RATE1)

/*
 * GATT entry bits (SSDM 7.1.1, Figures 7-3/7-4): the page address is bits
 * 23:0 for 4 KiB pages and bits 23:10 for 4 MiB pages; bit 26 is parity.
 */
#define I460_GATT_VALID         (1u << 24)
#define I460_GATT_COHERENT      (1u << 25)
#define I460_GATT_PARITY        (1u << 26)
#define I460_GATT_PFN_MASK      0x00ffffffu     /* phys[35:12] */
#define I460_GATT_4M_PFN_SHIFT  10
#define I460_GATT_4M_PFN_MASK   0x3fffu         /* phys[35:22] */

/*
 * A smaller SRAM leaves the top SADDR lines open, so it repeats through the
 * GXB's 1 MB range: an index wraps to the entries that exist.
 */
static bool ia64_agp_sram_on(IA64AGPState *s)
{
    return s->gatt_entries != 0 && !(s->agpsiz & I460_AGPSIZ_SRAM_IO_DIS);
}

static IA64AGPState *ia64_agp_from_iommu(IOMMUMemoryRegion *iommu)
{
    return container_of(iommu, IA64AGPState, iommu);
}

/*
 * Aperture DMA -> DRAM.  Addresses outside [apbase, apbase+size) pass through
 * untranslated (ordinary 32-bit-reachable DMA); addresses inside walk the GATT
 * SRAM to a 36-bit physical page.
 */
static IOMMUTLBEntry ia64_agp_translate(IOMMUMemoryRegion *iommu, hwaddr addr,
                                        IOMMUAccessFlags flag, int iommu_idx)
{
    IA64AGPState *s = ia64_agp_from_iommu(iommu);
    IOMMUTLBEntry ret = {
        .target_as = &address_space_memory,
        .iova = addr & ~(hwaddr)0xfff,
        .translated_addr = addr & ~(hwaddr)0xfff,
        .addr_mask = 0xfff,
        .perm = IOMMU_RW,
    };
    uint64_t apbase = s->aperture_base;
    uint64_t index;
    uint32_t entry;

    if (!s->aperture_enabled || addr < apbase ||
        addr - apbase >= s->aperture_size) {
        /* Not the graphics aperture: identity map into system memory. */
        return ret;
    }

    index = (addr - apbase) >> s->page_shift;
    entry = s->gatt[index & (s->gatt_entries - 1)];
    /* "treated as GART misses and the address is passed on untranslated" */
    if (!(entry & I460_GATT_VALID)) {
        return ret;
    }
    if (s->page_shift == 22) {
        ret.translated_addr =
            ((hwaddr)((entry >> I460_GATT_4M_PFN_SHIFT) &
                      I460_GATT_4M_PFN_MASK) << 22) | (addr & 0x3ff000);
    } else {
        ret.translated_addr = (hwaddr)(entry & I460_GATT_PFN_MASK) << 12;
    }
    ret.iova = addr & ~(hwaddr)0xfff;
    return ret;
}

/* GART SRAM programming window (0xFE200000): 32-bit little-endian words. */
static uint64_t ia64_agp_gart_read(void *opaque, hwaddr addr, unsigned size)
{
    IA64AGPState *s = opaque;
    unsigned index = addr >> 2;
    uint32_t entry;

    if (!ia64_agp_sram_on(s)) {
        return 0;
    }
    /* Even parity over the whole entry, reserved bits included (7.1.1.3). */
    entry = s->gatt[index & (s->gatt_entries - 1)];
    return entry | (ctpop32(entry) & 1 ? I460_GATT_PARITY : 0);
}

static void ia64_agp_gart_write(void *opaque, hwaddr addr, uint64_t val,
                                unsigned size)
{
    IA64AGPState *s = opaque;
    unsigned index = addr >> 2;

    if (!ia64_agp_sram_on(s)) {
        return;
    }
    /*
     * HW regenerates parity (bit 26); keep it out of the stored value.  There
     * is no GART TLB (SSDM 7.1.1.2): emulated-master DMA re-walks the SRAM on
     * every access via ia64_agp_translate(), so a fresh entry is live at once
     * with no invalidation needed.
     */
    s->gatt[index & (s->gatt_entries - 1)] = (uint32_t)val & ~I460_GATT_PARITY;
}

static const MemoryRegionOps ia64_agp_gart_ops = {
    .read = ia64_agp_gart_read,
    .write = ia64_agp_gart_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};


/*
 * Only the AGP graphics master's DMA traverses the GART.  On the real 460GX the
 * GART is on the GXB's AGP port alone; the SAC/PXB PCI masters (SCSI, IDE, USB,
 * NIC) reach memory directly, and must NOT be caught by the graphics aperture
 * -- several of their BARs (e.g. the LSI SCRIPTS RAM) sit inside the aperture's
 * bus-address range and would otherwise be mis-translated.  Every non-AGP
 * devfn therefore gets a plain identity pass-through to system memory.
 */
static AddressSpace *ia64_agp_dma_as(PCIBus *bus, void *opaque, int devfn)
{
    IA64AGPState *s = opaque;

    if (devfn == s->agp_master_devfn) {
        return &s->dma_as;
    }
    return &address_space_memory;
}

static const PCIIOMMUOps ia64_agp_iommu_ops = {
    .get_address_space = ia64_agp_dma_as,
};

static uint64_t ia64_agp_size(uint8_t agpsiz)
{
    switch (agpsiz & I460_AGPSIZ_SIZE_MASK) {
    case I460_AGPSIZ_SIZE_256M:
        return 256 * MiB;
    case I460_AGPSIZ_SIZE_1G:
        return 1 * GiB;
    case I460_AGPSIZ_SIZE_32G:
        return 32 * GiB;
    default:
        return 0;
    }
}

/* Where the base register shows: APBASE, BAPBASE, or nowhere (-1). */
static int ia64_agp_base_reg(IA64AGPState *s)
{
    if (ia64_agp_size(s->agpsiz) == 0) {
        return -1;
    }
    return s->agpsiz & I460_AGPSIZ_BAPBASE_EN ? I460_BAPBASE : I460_APBASE;
}

static uint64_t *ia64_agp_base(IA64AGPState *s)
{
    return s->agpsiz & I460_AGPSIZ_BAPBASE_EN ? &s->bapbase : &s->apbase;
}

static uint64_t ia64_agp_base_value(IA64AGPState *s)
{
    return (*ia64_agp_base(s) & ~(ia64_agp_size(s->agpsiz) - 1)) |
           PCI_BASE_ADDRESS_MEM_TYPE_64;
}

static void ia64_agp_update_aperture(IA64AGPState *s)
{
    uint64_t size = ia64_agp_size(s->agpsiz);
    bool large = s->gxbctl & I460_GXBCTL_4M_PS;

    s->page_shift = large ? 22 : 12;
    /* 32 GB "requires 4 MB pages" (SSDM 7.1.1). */
    s->aperture_size = size == 32 * GiB && !large ? 0 : size;
    s->aperture_base = size ? ia64_agp_base_value(s) & PCI_BASE_ADDRESS_MEM_MASK
                            : 0;
    s->aperture_enabled = ia64_agp_sram_on(s) && s->aperture_size != 0;
}

bool ia64_agp_cfg_owns(unsigned off)
{
    return (off >= I460_APBASE && off < I460_APBASE + 8) ||
           (off >= I460_BAPBASE && off < I460_BAPBASE + 8) ||
           off == I460_GXBCTL || off == I460_AGPSIZ ||
           off == PCI_STATUS || off == PCI_STATUS + 1 ||
           off == PCI_CAPABILITY_LIST ||
           (off >= I460_AGP_CAP && off < I460_AGP_CAP + PCI_AGP_SIZEOF);
}

uint8_t ia64_agp_cfg_readb(IA64AGPState *s, unsigned off)
{
    unsigned cap = off - I460_AGP_CAP;
    int base = ia64_agp_base_reg(s);

    if (off >= I460_AGP_CAP && off < I460_AGP_CAP + PCI_AGP_SIZEOF) {
        if (cap >= PCI_AGP_COMMAND) {
            return s->agp_command >> ((cap - PCI_AGP_COMMAND) * 8);
        }
        if (cap >= PCI_AGP_STATUS) {
            return I460_AGP_STATUS >> ((cap - PCI_AGP_STATUS) * 8);
        }
        return cap == 0 ? PCI_CAP_ID_AGP : cap == PCI_AGP_VERSION ? 0x20 : 0;
    }
    switch (off) {
    case PCI_STATUS:
        return PCI_STATUS_CAP_LIST;
    case PCI_STATUS + 1:
        return PCI_STATUS_DEVSEL_MEDIUM >> 8;
    case PCI_CAPABILITY_LIST:
        return I460_AGP_CAP;
    case I460_GXBCTL:
        return s->gxbctl;
    case I460_AGPSIZ:
        return s->agpsiz;
    }
    if (base >= 0 && off >= base && off < base + 8) {
        return ia64_agp_base_value(s) >> ((off - base) * 8);
    }
    return 0;
}

void ia64_agp_cfg_writeb(IA64AGPState *s, unsigned off, uint8_t val)
{
    unsigned cap = off - I460_AGP_CAP;
    int base = ia64_agp_base_reg(s);

    if (off >= I460_AGP_CAP + PCI_AGP_COMMAND &&
        off < I460_AGP_CAP + PCI_AGP_SIZEOF) {
        unsigned shift = (cap - PCI_AGP_COMMAND) * 8;

        s->agp_command = (s->agp_command & ~(0xffu << shift)) |
                         (((uint32_t)val << shift) & I460_AGP_COMMAND_WRITABLE &
                          (0xffu << shift));
        return;
    }
    switch (off) {
    case I460_GXBCTL:
        s->gxbctl = val & 0x07;         /* OOG, 4 MB pages, BWC */
        break;
    case I460_AGPSIZ:
        s->agpsiz = val & I460_AGPSIZ_WRITABLE;
        break;
    default:
        if (base >= 0 && off >= base && off < base + 8) {
            unsigned shift = (off - base) * 8;
            uint64_t *reg = ia64_agp_base(s);

            *reg = ((*reg & ~(0xffULL << shift)) |
                    ((uint64_t)val << shift)) & I460_BASE_BITS;
        }
        break;
    }
    ia64_agp_update_aperture(s);
}

static void ia64_agp_realize(DeviceState *dev, Error **errp)
{
    IA64AGPState *s = IA64_AGP(dev);

    if (s->sram_size != 0 && s->sram_size != 256 * KiB &&
        s->sram_size != 1 * MiB) {
        error_setg(errp, "sram-size must be 0, 256 KiB or 1 MiB");
        return;
    }
    s->gatt_entries = s->sram_size / sizeof(uint32_t);
    s->gatt = g_new0(uint32_t, MAX(s->gatt_entries, 1));
    memory_region_init_io(&s->gart_window, OBJECT(s), &ia64_agp_gart_ops, s,
                          "ia64-agp-gart", I460_GART_DECODE);
    memory_region_add_subregion(get_system_memory(), I460_GART_WINDOW_BASE,
                                &s->gart_window);

    /* Per-bus DMA translation: aperture -> GATT -> DRAM, else passthrough. */
    memory_region_init_iommu(&s->iommu, sizeof(s->iommu),
                             TYPE_IA64_AGP_IOMMU_MEMORY_REGION, OBJECT(s),
                             "ia64-agp-dma", UINT64_MAX);
    address_space_init(&s->dma_as, MEMORY_REGION(&s->iommu), "ia64-agp-dma");
    ia64_agp_update_aperture(s);
}

/*
 * The GART translates the master on the GXB's downstream root; every other
 * device on that bus passes straight through (ia64_agp_dma_as).
 */
void ia64_agp_attach_bus(IA64AGPState *s, PCIBus *bus)
{
    pci_setup_iommu(bus, &ia64_agp_iommu_ops, s);
}

static void ia64_agp_reset(DeviceState *dev)
{
    IA64AGPState *s = IA64_AGP(dev);

    memset(s->gatt, 0, MAX(s->gatt_entries, 1) * sizeof(uint32_t));
    s->gxbctl = 0;
    s->agpsiz = 0;
    s->apbase = 0;
    s->bapbase = 0;
    s->agp_command = 0;
    ia64_agp_update_aperture(s);
}

static const Property ia64_agp_properties[] = {
    DEFINE_PROP_INT32("agp-master-devfn", IA64AGPState, agp_master_devfn, -1),
    DEFINE_PROP_UINT32("sram-size", IA64AGPState, sram_size, 1 * MiB),
};

static void ia64_agp_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = ia64_agp_realize;
    dc->desc = "Intel 460GX GXB AGP bridge";
    device_class_set_legacy_reset(dc, ia64_agp_reset);
    device_class_set_props(dc, ia64_agp_properties);
    /* Chipset device, not user-pluggable. */
    dc->user_creatable = false;
}

static const TypeInfo ia64_agp_info = {
    .name          = TYPE_IA64_AGP,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(IA64AGPState),
    .class_init    = ia64_agp_class_init,
};

static void ia64_agp_iommu_class_init(ObjectClass *klass, const void *data)
{
    IOMMUMemoryRegionClass *imrc = IOMMU_MEMORY_REGION_CLASS(klass);

    imrc->translate = ia64_agp_translate;
}

static const TypeInfo ia64_agp_iommu_info = {
    .parent = TYPE_IOMMU_MEMORY_REGION,
    .name = TYPE_IA64_AGP_IOMMU_MEMORY_REGION,
    .class_init = ia64_agp_iommu_class_init,
};

static void ia64_agp_register_types(void)
{
    type_register_static(&ia64_agp_info);
    type_register_static(&ia64_agp_iommu_info);
}

type_init(ia64_agp_register_types)
