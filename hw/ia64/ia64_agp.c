/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Intel 460GX GXB AGP host bridge + GART, minimal model.
 *
 * Models just enough of the 460GX "expander/graphics bridge" (chipset device
 * 14h) for Linux's i460-agp driver to bind and for an AGP master (the ATI
 * Rage 128) to DMA through the graphics aperture into DRAM above 4 GiB, which a
 * 32-bit PCI master cannot reach on its own.  Per the 460GX SSDM (248704-001,
 * ch. 7) the GART is not an in-DRAM table with a base pointer: it is on-chip
 * SRAM the OS programs through a fixed physical MMIO window at 0xFE200000, with
 * no TLB and no flush register.  A GATT entry translates a 4 KiB aperture page
 * to a 36-bit physical page.
 *
 * Contract taken from Linux 2.6.8 drivers/char/agp/i460-agp.c:
 *   - binds to class host-bridge, 8086:84ea, and requires a PCI AGP capability;
 *   - GXBCTL[0xa0] bit1 must read 0 (4 KiB pages); AGPSIZ[0xa2] bits[2:0] select
 *     the size (1 = 256 MiB); bit3 (BAPBASE_ENABLE) picks which register holds
 *     the aperture base;
 *   - the aperture base register is APBASE (BAR0, 0x10) when AGPSIZ bit3 is
 *     clear, or the non-header BAPBASE (0x98) when it is set;
 *   - GATT entry = 0x03000000 | (paddr[35:12]); bit24 valid, bit25 coherent.
 *
 * Aperture placement (SSDM 248704-001 sec 7.2.1).  The AGP master here is an
 * ATI Rage 128, whose AGP_BASE register (0x170) is only 32 bits wide -- it
 * cannot issue the dual-address cycles a >4 GiB aperture would need -- so the
 * aperture bus address must live below 4 GiB, and the GART translates those
 * 32-bit aperture pages up to 36-bit DRAM physical addresses (which may be
 * above 4 GiB).  The only sub-4 GiB range clear of DRAM on this platform is the
 * PCI MMIO hole [0xEE000000, 0xFE000000), so the aperture reuses it -- exactly
 * the SSDM "reserved gap in the PCI address space" placement.  Because the
 * aperture is never touched by the processor (i460-agp sets cant_use_aperture),
 * it is not a header BAR mapped into CPU space (which would collide with the
 * VGA framebuffer BAR and trip the guest PCI resource allocator); it is exposed
 * only in the per-bus DMA address space through the IOMMU below, and its base
 * is advertised through BAPBASE with AGPSIZ bit3 set.
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "hw/ia64/ia64_agp.h"
#include "hw/ia64/ia64_vpc_abi.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_device.h"
#include "hw/core/qdev-properties.h"
#include "system/address-spaces.h"
#include "qemu/log.h"
#include "qapi/error.h"

/* Fixed physical window through which the OS reads/writes the GART SRAM. */
#define I460_GART_WINDOW_BASE   0x00000000fe200000ULL

/* Config-space registers the i460-agp driver touches. */
#define I460_BAPBASE            0x98    /* 64-bit: above-header aperture base  */
#define I460_GXBCTL             0xa0    /* 8-bit: bit1 = 4 MiB page select */
#define I460_AGPSIZ             0xa2    /* 8-bit: [2:0] size, bit3/4 flags   */

#define I460_GXBCTL_4M_PS       0x02
#define I460_AGPSIZ_SIZE_256M   0x01    /* size_value 1 */
#define I460_AGPSIZ_BAPBASE_EN  0x08    /* bit3: aperture base is in BAPBASE   */
#define I460_AGPSIZ_SRAM_IO_DIS 0x10    /* bit4: GART SRAM I/O disabled        */

/* GATT entry bits. */
#define I460_GATT_VALID         (1u << 24)
#define I460_GATT_COHERENT      (1u << 25)
#define I460_GATT_PFN_MASK      0x00ffffffu     /* phys[35:12] */

/*
 * 256 MiB aperture (AGPSIZ size_value 1), 4 KiB pages => 65536 GATT entries
 * (256 KiB of SRAM).  The aperture bus range reuses the platform PCI MMIO hole
 * (see the placement note above); an out-of-aperture DMA still passes straight
 * through to system memory.
 */
#define I460_APERTURE_SIZE      (256 * MiB)
#define I460_GATT_ENTRIES       (I460_APERTURE_SIZE / (4 * KiB))
#define I460_APERTURE_BASE      IA64_PCI_MMIO_BASE

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
    uint32_t entry;
    unsigned index;

    if (!s->aperture_enabled || addr < apbase ||
        addr >= apbase + I460_APERTURE_SIZE) {
        /* Not the graphics aperture: identity map into system memory. */
        return ret;
    }

    index = (addr - apbase) >> 12;
    entry = s->gatt[index];
    if (!(entry & I460_GATT_VALID)) {
        ret.perm = IOMMU_NONE;
        return ret;
    }
    ret.translated_addr = ((hwaddr)(entry & I460_GATT_PFN_MASK) << 12) |
                          (addr & 0xfff);
    return ret;
}

/* GART SRAM programming window (0xFE200000): 32-bit little-endian words. */
static uint64_t ia64_agp_gart_read(void *opaque, hwaddr addr, unsigned size)
{
    IA64AGPState *s = opaque;
    unsigned index = addr >> 2;

    if (index >= I460_GATT_ENTRIES) {
        return 0;
    }
    return s->gatt[index];
}

static void ia64_agp_gart_write(void *opaque, hwaddr addr, uint64_t val,
                                unsigned size)
{
    IA64AGPState *s = opaque;
    unsigned index = addr >> 2;

    if (index >= I460_GATT_ENTRIES) {
        return;
    }
    /*
     * HW regenerates parity (bit 26); keep it out of the stored value.  There
     * is no GART TLB (SSDM 7.1.1.2): emulated-master DMA re-walks the SRAM on
     * every access via ia64_agp_translate(), so a fresh entry is live at once
     * with no invalidation needed.
     */
    s->gatt[index] = (uint32_t)val & ~(1u << 26);
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

/*
 * The aperture base is the BAPBASE register (0x98) with its low control bits
 * masked off -- the same value the i460-agp driver reads and stores in
 * gart_bus_addr (see i460_configure()).  Translation is live whenever it is
 * programmed; the GART's per-entry valid bit gates individual pages.
 */
static void ia64_agp_update_aperture(IA64AGPState *s)
{
    PCIDevice *dev = PCI_DEVICE(s);
    uint64_t base = pci_get_quad(dev->config + I460_BAPBASE) & ~7ULL;

    s->aperture_base = base;
    s->aperture_enabled = s->gart_enabled && base != 0;
}

static void ia64_agp_config_write(PCIDevice *dev, uint32_t addr,
                                  uint32_t val, int len)
{
    IA64AGPState *s = IA64_AGP(dev);

    pci_default_write_config(dev, addr, val, len);
    ia64_agp_update_aperture(s);
}

static void ia64_agp_realize(PCIDevice *dev, Error **errp)
{
    IA64AGPState *s = IA64_AGP(dev);
    uint8_t *c = dev->config;

    /* Host-bridge class so i460-agp's pci_device_id table matches. */
    pci_config_set_prog_interface(c, 0);

    /*
     * i460-agp reads GXBCTL bit1 (must be 0 = 4 KiB pages) and AGPSIZ[2:0]
     * (1 = 256 MiB).  AGPSIZ bit3 (BAPBASE_ENABLE) is set so the driver takes
     * the aperture base from BAPBASE (a >4 GiB-capable, non-header BAR) rather
     * than the standard header BAR -- see the placement note at the top.  When
     * the machine turns the GART off (agp=off), also assert bit4
     * (SRAM_IO_DISABLE), on which i460_fetch_size() bails ("GART SRAMS
     * disabled") so the OS keeps to the Rage 128's own PCI GART.
     */
    c[I460_GXBCTL] = 0x00;
    c[I460_AGPSIZ] = I460_AGPSIZ_SIZE_256M | I460_AGPSIZ_BAPBASE_EN |
                     (s->gart_enabled ? 0 : I460_AGPSIZ_SRAM_IO_DIS);
    dev->wmask[I460_GXBCTL] = 0x05;           /* driver writes OOG|BWC only */
    dev->wmask[I460_AGPSIZ] = 0x07;           /* size_value RMW, keep [7:3] */

    /*
     * BAPBASE (0x98): the aperture base, low 3 bits marking a 64-bit memory
     * BAR (the driver masks them off).  Fixed at the platform PCI MMIO hole
     * base; hardwired (read-only) since this synthetic chipset does not depend
     * on firmware to size/relocate it.
     */
    pci_set_quad(c + I460_BAPBASE,
                 I460_APERTURE_BASE | PCI_BASE_ADDRESS_MEM_TYPE_64);

    /* Mandatory: an AGP capability, or the driver returns -ENODEV. */
    if (pci_add_capability(dev, PCI_CAP_ID_AGP, 0, 8, errp) < 0) {
        return;
    }
    /* Advertise AGP 2.0, 1x/2x/4x so agp_generic_enable negotiates a rate. */
    pci_set_long(c + pci_find_capability(dev, PCI_CAP_ID_AGP) + PCI_AGP_STATUS,
                 0x1f000207);

    /* GART SRAM, exposed to the CPU at the fixed 0xFE200000 window. */
    s->gatt = g_new0(uint32_t, I460_GATT_ENTRIES);
    memory_region_init_io(&s->gart_window, OBJECT(s), &ia64_agp_gart_ops, s,
                          "ia64-agp-gart", I460_GATT_ENTRIES * sizeof(uint32_t));
    memory_region_add_subregion(get_system_memory(), I460_GART_WINDOW_BASE,
                                &s->gart_window);

    /* Per-bus DMA translation: aperture -> GATT -> DRAM, else passthrough. */
    memory_region_init_iommu(&s->iommu, sizeof(s->iommu),
                             TYPE_IA64_AGP_IOMMU_MEMORY_REGION, OBJECT(s),
                             "ia64-agp-dma", UINT64_MAX);
    address_space_init(&s->dma_as, MEMORY_REGION(&s->iommu), "ia64-agp-dma");
    pci_setup_iommu(pci_get_bus(dev), &ia64_agp_iommu_ops, s);

    ia64_agp_update_aperture(s);
}

/*
 * Extend the GART translation over another root bus.  The GXB bridge sits on
 * the chipset's own bus while the AGP master it translates is on the GXB's
 * downstream root, so that bus needs the same DMA routing: the master's devfn
 * is translated through the aperture, everything else passes through.
 */
void ia64_agp_attach_bus(IA64AGPState *s, PCIBus *bus)
{
    pci_setup_iommu(bus, &ia64_agp_iommu_ops, s);
}

static void ia64_agp_reset(DeviceState *dev)
{
    IA64AGPState *s = IA64_AGP(dev);

    /* GATT SRAM clears; BAPBASE is hardwired, so the aperture stays mapped. */
    memset(s->gatt, 0, I460_GATT_ENTRIES * sizeof(uint32_t));
    ia64_agp_update_aperture(s);
}

static const Property ia64_agp_properties[] = {
    DEFINE_PROP_INT32("agp-master-devfn", IA64AGPState, agp_master_devfn, -1),
    DEFINE_PROP_BOOL("gart-enabled", IA64AGPState, gart_enabled, true),
};

static void ia64_agp_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->realize = ia64_agp_realize;
    k->config_write = ia64_agp_config_write;
    k->vendor_id = PCI_VENDOR_ID_INTEL;
    k->device_id = 0x84ea;              /* PCI_DEVICE_ID_INTEL_84460GX */
    k->class_id = PCI_CLASS_BRIDGE_HOST;
    dc->desc = "Intel 460GX GXB AGP bridge";
    device_class_set_legacy_reset(dc, ia64_agp_reset);
    device_class_set_props(dc, ia64_agp_properties);
    /* Chipset device, not user-pluggable. */
    dc->user_creatable = false;
}

static const TypeInfo ia64_agp_info = {
    .name          = TYPE_IA64_AGP,
    .parent        = TYPE_PCI_DEVICE,
    .instance_size = sizeof(IA64AGPState),
    .class_init    = ia64_agp_class_init,
    .interfaces = (const InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
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
