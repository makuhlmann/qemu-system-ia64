/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * HP zx1 SBA (System Bus Adapter) IOC IOMMU for the ia64-vpc machine.
 *
 * A thin QOM shell over the portable zx1 IOC cores (hw/pci-host/hp-sba-iommu.c
 * + hp-zx1-iommu.c), modeled on hw/ia64/ia64_agp.c and on upstream's
 * hw/pci-host/hp-zx1-mio.c.  Unlike the 460GX GXB GART -- which translates only
 * the single AGP graphics master through on-chip SRAM -- the SBA is a real
 * IOMMU: every PCI master's DMA is translated through one shared in-DRAM IOPDIR
 * the OS programs, so a 32-bit master (the Rage 128) can reach RAM above 4 GiB
 * by address translation, with no bounce buffer.
 *
 * The IOC register block is exposed at a fixed chipset MMIO base
 * (IA64_SBA_CSR_BASE).  Linux sba_iommu takes the SBA base from the ACPI
 * HWP0001 _CRS and reads the IOC registers at base + ZX1_IOC_OFFSET(0x1000) +
 * IBASE(0x300); the IOMMU register window is therefore modeled at CSR offset
 * 0x1300, exactly as on real hardware and in the upstream mio model.  Reset
 * leaves the IOC in bypass (IBASE.enable == 0): a guest with no SBA driver
 * still does correct, untranslated DMA.
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/bswap.h"
#include "qemu/log.h"
#include "hw/ia64/ia64_sba.h"
#include "hw/ia64/ia64_vpc_abi.h"
#include "hw/pci-host/hp-sba-iommu.h"
#include "hw/pci-host/hp-zx1-iommu.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_device.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "system/address-spaces.h"
#include "qapi/error.h"

/*
 * IOC CSR layout (mirrors upstream hp-zx1-mio.c).  The IOMMU register file
 * lives in IOC "function 1" at CSR offset 0x1000; its five registers (IBASE,
 * IMASK, PCOM, TCNFG, PDIR_BASE) are at frontend offsets 0x300..0x320.
 */
#define IA64_SBA_IOC_FUNCTION_OFFSET   UINT64_C(0x1000)
/* IOC identity registers at the function block base: FUNC_ID + FCLASS. */
#define IA64_SBA_IOC_FUNC_ID_OFFSET    (IA64_SBA_IOC_FUNCTION_OFFSET + 0x000)
#define IA64_SBA_IOC_FCLASS_OFFSET     (IA64_SBA_IOC_FUNCTION_OFFSET + 0x008)
#define IA64_SBA_IOMMU_FIRST           UINT64_C(0x1300)
#define IA64_SBA_IOMMU_LAST            UINT64_C(0x1320)
#define IA64_SBA_IOMMU_END             UINT64_C(0x1328)

/* IOVA span covered by the translating region (50-bit IOC physical reach). */
#define IA64_SBA_IOMMU_SIZE            (HP_ZX1_IOMMU_PHYS_MASK + UINT64_C(1))

/*
 * The "safe IOVA space" the IOC advertises through its IBASE/IMASK registers
 * (IA64_SBA_IOVA_BASE/SIZE, in ia64_vpc_abi.h): the OS's sba_iommu *reads* it to
 * size its in-DRAM IOPDIR (iov_size = ~imask + 1) before programming its own
 * mappings -- with a 0 mask it would try to allocate a 4 GiB-worth page table
 * and panic ("IOC: Couldn't allocate I/O Page Table").  It is the classic 1 GiB
 * window at 1 GiB, 32-bit-addressable so a 32-bit master (the Rage 128) can
 * issue IOVAs into it, and the zx1 machine keeps a DRAM hole there so it
 * overlaps no memory.  The IBASE enable bit is left clear at reset, so until an
 * sba_iommu-class OS turns translation on the IOC stays in bypass.
 */

/* A whole-aperture UNMAP, emitted on non-PCOM register writes and on reset. */
static const HPSBAIOMMUPurge ia64_sba_full_unmap = {
    .iova = 0,
    .size = IA64_SBA_IOMMU_SIZE,
};

static IA64SBAState *ia64_sba_from_iommu(IOMMUMemoryRegion *iommu)
{
    return container_of(iommu, IA64SBAState, iommu);
}

/* Read a host-order IOPDIR entry from guest memory (little-endian in DRAM). */
static bool ia64_sba_pdir_read(void *opaque, uint64_t address, uint64_t *entry)
{
    uint8_t buffer[sizeof(*entry)];

    (void)opaque;
    if (address_space_read(&address_space_memory, address,
                           MEMTXATTRS_UNSPECIFIED, buffer,
                           sizeof(buffer)) != MEMTX_OK) {
        return false;
    }
    *entry = ldq_le_p(buffer);
    return true;
}

static void ia64_sba_notify_unmap(IA64SBAState *s,
                                  const HPSBAIOMMUPurge *purge)
{
    IOMMUTLBEvent event;

    g_assert(purge && purge->size);
    event = (IOMMUTLBEvent) {
        .type = IOMMU_NOTIFIER_UNMAP,
        .entry = {
            .target_as = &address_space_memory,
            .iova = purge->iova,
            .translated_addr = 0,
            .addr_mask = purge->size - 1,
            .perm = IOMMU_NONE,
        },
    };

    if (s->iommu.iommu_notify_flags & IOMMU_NOTIFIER_UNMAP) {
        memory_region_notify_iommu(&s->iommu, 0, event);
    }
}

/*
 * Aperture DMA translation.  Delegates to the shared IOC frontend: outside the
 * IOVA window (or before IBASE is enabled) it identity-maps; inside, it walks
 * the guest IOPDIR and caches the result in the 16-entry IOTLB.
 */
static IOMMUTLBEntry ia64_sba_translate(IOMMUMemoryRegion *iommu, hwaddr addr,
                                        IOMMUAccessFlags flag, int iommu_idx)
{
    IA64SBAState *s = ia64_sba_from_iommu(iommu);
    HPZX1IOMMUEvictionResult eviction = { 0 };
    HPZX1IOMMUTranslateResult result;
    HPSBAIOMMUEntry entry;

    (void)flag;
    if (iommu_idx != 0) {
        return (IOMMUTLBEntry) { .perm = IOMMU_NONE };
    }

    qemu_rec_mutex_lock(&s->iommu_lock);
    /* The DMA frontend performs translations with DVI (direct) disabled. */
    result = hp_zx1_iommu_frontend_translate(&s->fe, addr, false,
                                             ia64_sba_pdir_read, s, &entry,
                                             &eviction);
    qemu_rec_mutex_unlock(&s->iommu_lock);

    if (result == HP_ZX1_IOMMU_TRANSLATE_BLOCKED) {
        return (IOMMUTLBEntry) { .perm = IOMMU_NONE };
    }
    if (eviction.evicted) {
        ia64_sba_notify_unmap(s, &eviction.range);
    }

    return (IOMMUTLBEntry) {
        .target_as = &address_space_memory,
        .iova = entry.iova,
        .translated_addr = entry.translated_addr,
        .addr_mask = entry.addr_mask,
        .perm = IOMMU_RW,
    };
}

/* Decompose an aligned/contiguous CSR access into a 64-bit reg + byte-enable. */
static bool ia64_sba_write_shape(hwaddr addr, uint64_t value, unsigned int size,
                                 uint64_t *base, uint64_t *reg_value,
                                 uint8_t *byte_enable)
{
    unsigned int lane = addr & 7;

    if ((size != 1 && size != 2 && size != 4 && size != 8) ||
        lane + size > 8) {
        return false;
    }
    *base = addr & ~UINT64_C(7);
    *reg_value = value << (lane * 8);
    *byte_enable = size == 8 ? UINT8_MAX : ((1U << size) - 1) << lane;
    return true;
}

static bool ia64_sba_is_iommu_addr(hwaddr addr)
{
    return addr >= IA64_SBA_IOMMU_FIRST && addr < IA64_SBA_IOMMU_END;
}

/*
 * The IOC identity registers Linux sba_iommu reads to name the IOC and take its
 * revision (FCLASS & 0xff).  Serving these lets func_id == ZX1_IOC_ID match, so
 * the driver runs the zx1-specific ioc_zx1_init() path rather than reporting an
 * "Unknown 0.0" IOC.  Only these two offsets in the function block are modeled;
 * every other MIO CSR still reads zero.
 */
static bool ia64_sba_identity_reg(hwaddr base, uint64_t *reg)
{
    switch (base) {
    case IA64_SBA_IOC_FUNC_ID_OFFSET:
        *reg = IA64_SBA_IOC_FUNC_ID;
        return true;
    case IA64_SBA_IOC_FCLASS_OFFSET:
        *reg = IA64_SBA_IOC_FCLASS;
        return true;
    default:
        return false;
    }
}

static MemTxResult ia64_sba_csr_read(void *opaque, hwaddr addr, uint64_t *data,
                                     unsigned int size, MemTxAttrs attrs)
{
    IA64SBAState *s = opaque;
    uint64_t reg;
    bool ok = false;

    (void)attrs;
    qemu_rec_mutex_lock(&s->iommu_lock);
    if (ia64_sba_is_iommu_addr(addr) && size == 8 && !(addr & 7) &&
        addr <= IA64_SBA_IOMMU_LAST) {
        ok = hp_zx1_iommu_frontend_reg_latch(
                 &s->fe, addr - IA64_SBA_IOC_FUNCTION_OFFSET, data);
    }
    qemu_rec_mutex_unlock(&s->iommu_lock);

    if (!ok && size >= 1 && size <= 8 && (addr & 7) + size <= 8 &&
        ia64_sba_identity_reg(addr & ~UINT64_C(7), &reg)) {
        /* Right-justify the addressed byte lane, like the IOMMU/LBA reads. */
        *data = (reg >> ((addr & 7) * 8)) &
                (size >= 8 ? UINT64_MAX : (UINT64_C(1) << (size * 8)) - 1);
        ok = true;
    }

    if (!ok) {
        /* Unmodeled IOC registers read as zero (do not fault the guest). */
        *data = 0;
    }
    return MEMTX_OK;
}

static MemTxResult ia64_sba_csr_write(void *opaque, hwaddr addr, uint64_t value,
                                      unsigned int size, MemTxAttrs attrs)
{
    IA64SBAState *s = opaque;
    HPSBAIOMMUPurge unmap = { 0 };
    bool notify = false;

    (void)attrs;
    qemu_rec_mutex_lock(&s->iommu_lock);
    if (ia64_sba_is_iommu_addr(addr)) {
        HPZX1IOMMUWriteResult result;
        uint64_t base, reg_value, reg_offset;
        uint8_t byte_enable;

        if (ia64_sba_write_shape(addr, value, size, &base, &reg_value,
                                 &byte_enable) &&
            base >= IA64_SBA_IOMMU_FIRST && base <= IA64_SBA_IOMMU_LAST) {
            reg_offset = base - IA64_SBA_IOC_FUNCTION_OFFSET;
            if (hp_zx1_iommu_frontend_reg_write(&s->fe, reg_offset, reg_value,
                                                byte_enable, &result)) {
                if (reg_offset == HP_ZX1_IOC_IOMMU_PCOM) {
                    if (result.purged) {
                        unmap = result.purge;
                        notify = true;
                    }
                } else {
                    /* IBASE/IMASK/TCNFG/PDIR_BASE change the whole mapping. */
                    unmap = ia64_sba_full_unmap;
                    notify = true;
                }
            }
        }
    }
    qemu_rec_mutex_unlock(&s->iommu_lock);

    if (notify) {
        ia64_sba_notify_unmap(s, &unmap);
    }
    return MEMTX_OK;
}

static const MemoryRegionOps ia64_sba_csr_ops = {
    .read_with_attrs = ia64_sba_csr_read,
    .write_with_attrs = ia64_sba_csr_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8, .unaligned = true },
    .impl = { .min_access_size = 1, .max_access_size = 8, .unaligned = true },
};

/*
 * The SBA translates DMA from every master on the bus (unlike the 460GX GART,
 * which is on the AGP port alone) -- one shared IOMMU address space for all.
 */
static AddressSpace *ia64_sba_dma_as(PCIBus *bus, void *opaque, int devfn)
{
    IA64SBAState *s = opaque;

    (void)bus;
    (void)devfn;
    return &s->dma_as;
}

static bool ia64_sba_supports_as(PCIBus *bus, void *opaque, int devfn,
                                 Error **errp)
{
    (void)bus;
    (void)opaque;
    (void)devfn;
    (void)errp;
    return true;
}

static const PCIIOMMUOps ia64_sba_iommu_ops = {
    .get_address_space = ia64_sba_dma_as,
    .supports_address_space = ia64_sba_supports_as,
};

static void ia64_sba_frontend_reset(IA64SBAState *s)
{
    /*
     * Present the firmware-programmed safe IOVA window in IBASE/IMASK, with the
     * IBASE enable bit clear so the IOC starts in bypass (identity DMA) until an
     * OS turns translation on.  imask encodes the window size the same way the
     * hardware does: iov_size = ~imask + 1 (the high 32 bits are don't-care --
     * both this model and Linux force them set).
     */
    HPZX1IOMMUResetConfig config = {
        .ibase = IA64_SBA_IOVA_BASE,          /* enable bit (bit0) clear */
        .imask = ~(IA64_SBA_IOVA_SIZE - 1),   /* 1 GiB => 0xffffffffc0000000 */
        .pcom = 0,
        .tcnfg = 0,
        .pdir_base = 0,
    };
    bool ok = hp_zx1_iommu_frontend_reset(&s->fe, &config);

    g_assert(ok);
}

static void ia64_sba_realize(PCIDevice *dev, Error **errp)
{
    IA64SBAState *s = IA64_SBA(dev);
    uint8_t *c = dev->config;

    /* Chipset host-bridge identity; the IOC "function 1" id 0x122A/HP. */
    pci_config_set_prog_interface(c, 0);

    qemu_rec_mutex_init(&s->iommu_lock);
    ia64_sba_frontend_reset(s);

    /* IOC CSR block, exposed to the CPU at the fixed chipset base. */
    memory_region_init_io(&s->csr, OBJECT(s), &ia64_sba_csr_ops, s,
                          "ia64-sba-csr", IA64_SBA_CSR_SIZE);
    memory_region_add_subregion(get_system_memory(), s->csr_base, &s->csr);

    /* Per-bus DMA translation: IOPDIR walk, else bypass to system memory. */
    memory_region_init_iommu(&s->iommu, sizeof(s->iommu),
                             TYPE_IA64_SBA_IOMMU_MEMORY_REGION, OBJECT(s),
                             "ia64-sba-dma", IA64_SBA_IOMMU_SIZE);
    address_space_init(&s->dma_as, MEMORY_REGION(&s->iommu), "ia64-sba-dma");
    pci_setup_iommu(pci_get_bus(dev), &ia64_sba_iommu_ops, s);
}

void ia64_sba_attach_bus(IA64SBAState *s, PCIBus *bus)
{
    pci_setup_iommu(bus, &ia64_sba_iommu_ops, s);
}

static void ia64_sba_reset(DeviceState *dev)
{
    IA64SBAState *s = IA64_SBA(dev);

    qemu_rec_mutex_lock(&s->iommu_lock);
    ia64_sba_frontend_reset(s);
    qemu_rec_mutex_unlock(&s->iommu_lock);
    ia64_sba_notify_unmap(s, &ia64_sba_full_unmap);
}

/*
 * Migrate the IOC register latches and the round-robin cursor.  The IOTLB is a
 * pure translation cache reconstructible from the (migrated) guest IOPDIR, so
 * it is not migrated; it is cleared on load and re-walked on demand.
 */
static int ia64_sba_post_load(void *opaque, int version_id)
{
    IA64SBAState *s = opaque;

    hp_zx1_iotlb_clear(&s->fe.iotlb);
    s->fe.rr_next = 0;
    return 0;
}

static const VMStateDescription vmstate_ia64_sba = {
    .name = "ia64-sba-ioc",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = ia64_sba_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_PCI_DEVICE(parent_obj, IA64SBAState),
        VMSTATE_UINT64(fe.ibase, IA64SBAState),
        VMSTATE_UINT64(fe.imask, IA64SBAState),
        VMSTATE_UINT64(fe.pcom, IA64SBAState),
        VMSTATE_UINT64(fe.tcnfg, IA64SBAState),
        VMSTATE_UINT64(fe.pdir_base, IA64SBAState),
        VMSTATE_END_OF_LIST()
    },
};

static const Property ia64_sba_properties[] = {
    DEFINE_PROP_UINT64("csr-base", IA64SBAState, csr_base, IA64_SBA_CSR_BASE),
};

static void ia64_sba_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->realize = ia64_sba_realize;
    k->vendor_id = 0x103c;              /* Hewlett-Packard */
    k->device_id = 0x122a;              /* zx1 mio IOC (function 1) */
    k->class_id = PCI_CLASS_BRIDGE_HOST;
    dc->desc = "HP zx1 SBA IOC IOMMU";
    dc->vmsd = &vmstate_ia64_sba;
    device_class_set_legacy_reset(dc, ia64_sba_reset);
    device_class_set_props(dc, ia64_sba_properties);
    /* Chipset device, not user-pluggable. */
    dc->user_creatable = false;
}

static const TypeInfo ia64_sba_info = {
    .name          = TYPE_IA64_SBA,
    .parent        = TYPE_PCI_DEVICE,
    .instance_size = sizeof(IA64SBAState),
    .class_init    = ia64_sba_class_init,
    .interfaces = (const InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static uint64_t ia64_sba_iommu_min_page_size(IOMMUMemoryRegion *iommu)
{
    (void)iommu;
    return 4 * KiB;
}

static int ia64_sba_iommu_notify_flag_changed(IOMMUMemoryRegion *iommu,
                                              IOMMUNotifierFlag old_flags,
                                              IOMMUNotifierFlag new_flags,
                                              Error **errp)
{
    unsigned int unsupported = (unsigned int)new_flags &
                               ~(unsigned int)IOMMU_NOTIFIER_UNMAP;

    (void)iommu;
    (void)old_flags;
    if (unsupported) {
        error_setg(errp, "zx1 SBA IOMMU does not support notifier flags 0x%x",
                   unsupported);
        return -EINVAL;
    }
    return 0;
}

static void ia64_sba_iommu_class_init(ObjectClass *klass, const void *data)
{
    IOMMUMemoryRegionClass *imrc = IOMMU_MEMORY_REGION_CLASS(klass);

    imrc->translate = ia64_sba_translate;
    imrc->get_min_page_size = ia64_sba_iommu_min_page_size;
    imrc->notify_flag_changed = ia64_sba_iommu_notify_flag_changed;
}

static const TypeInfo ia64_sba_iommu_info = {
    .parent = TYPE_IOMMU_MEMORY_REGION,
    .name = TYPE_IA64_SBA_IOMMU_MEMORY_REGION,
    .class_init = ia64_sba_iommu_class_init,
};

static void ia64_sba_register_types(void)
{
    type_register_static(&ia64_sba_info);
    type_register_static(&ia64_sba_iommu_info);
}

type_init(ia64_sba_register_types)
