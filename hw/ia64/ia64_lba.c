/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * HP zx1 LBA (Local Bus Adapter / Mercury I/O adapter) CSR block for the ia64
 * zx1 machine.
 *
 * On real zx1 the Mercury ioa is a PCI/PCI-X/AGP host bridge whose registers
 * live in an MMIO CSR block that the mio locates for it (not standard PCI config
 * space).  Linux hp-agp (drivers/char/agp/hp-agp.c) and the Windows HP AGP
 * "AgpMercury" miniport reach it through the ACPI HWP0003 device and read it as
 * ordinary configuration space to find the AGP capability (id 0x02 at 0x60, AGP
 * status at 0x64, writable AGP command at 0x68; PCI_STATUS.CAP_LIST set,
 * capability-list pointer at 0x34).
 *
 * This models that CSR block faithfully: the identity registers a driver reads
 * to recognise the bridge (FUNCTION_ID 0x00, FUNCTION_CLASS 0x08,
 * CAPABILITIES_POINTER 0x30, BUS_NUMBER 0x58), the AGP capability/command, the
 * CONFIG_ADDRESS/DATA pair (0x40/0x48) which generates configuration cycles on
 * the Mercury root bus, and the control/decode registers real Mercury exposes
 * (ARBITRATION_MASK 0x80, STATUS_CONTROL/SIC 0x108, the LMMIO/GMMIO/WLMMIO/
 * WGMMIO/ELMMIO window decoders 0x200-0x258, SLAVE_CONTROL 0x278, MSI 0x280/
 * 0x288, BUS_MODE 0x620) with their real reset values and writable masks.
 *
 * Register values and masks mirror upstream hw/pci-host/hp-zx1-ioa-regs.c in AGP
 * mode.  Deliberate simplifications for this machine: the LMMIO/GMMIO/MSI
 * decoders are modelled for driver-visible fidelity but do not themselves route
 * decode -- this machine decodes through the shared PCI window and the ACPI
 * _CRS, and DMA/GART is the SBA's IOPDIR (hp-agp's "shared" path).  The block
 * carries no PCI config space of its own and does no DMA translation; the
 * graphics adapter is a real PCI device on the Mercury root bus
 * (hw/ia64/ia64_mercury.c).  It is described to guests only through the ACPI
 * HWP0003 _CRS (a CCSR VendorLong and a memory descriptor), never the EFI memory
 * map; keep base/length in lockstep with LBA0 in
 * roms/ia64-firmware/dsdt-pci-root-zx1.asl.
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "hw/ia64/ia64_lba.h"
#include "hw/ia64/ia64_vpc_abi.h"
#include "hw/core/qdev-properties.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_host.h"
#include "hw/pci/pci_device.h"
#include "migration/vmstate.h"
#include "system/address-spaces.h"

/* Mercury CSR register offsets (HP zx1 ioa ERS / upstream hp-zx1-ioa-regs.h). */
#define LBA_FUNCTION_ID          0x000
#define LBA_FUNCTION_CLASS       0x008
#define LBA_CAP_POINTER          0x030
#define LBA_CONFIG_ADDRESS       0x040
#define LBA_CONFIG_DATA          0x048
#define LBA_BUS_NUMBER           0x058
#define LBA_AGP_CAPABILITY       0x060
#define LBA_AGP_COMMAND          0x068
#define LBA_ARBITRATION_MASK     0x080
#define LBA_STATUS_CONTROL       0x108
#define LBA_LMMIO_BASE           0x200
#define LBA_LMMIO_MASK           0x208
#define LBA_GMMIO_BASE           0x210
#define LBA_GMMIO_MASK           0x218
#define LBA_WLMMIO_BASE          0x220
#define LBA_WLMMIO_MASK          0x228
#define LBA_WGMMIO_BASE          0x230
#define LBA_WGMMIO_MASK          0x238
#define LBA_ELMMIO_BASE          0x250
#define LBA_ELMMIO_MASK          0x258
#define LBA_SLAVE_CONTROL        0x278
#define LBA_MSI_BASE             0x280
#define LBA_MSI_MASK             0x288
#define LBA_BUS_MODE             0x620

/* Reset values and writable masks (upstream hp-zx1-ioa-regs.h, AGP mode). */
#define LBA_CONFIG_ADDRESS_MASK  UINT32_C(0x00fffffc)
#define LBA_ARBITRATION_RESET    UINT32_C(0x00000001)
#define LBA_ARBITRATION_WRITE    UINT32_C(0x0000007f)
#define LBA_ARBITRATION_F        UINT32_C(0x00000040)
#define LBA_SIC_FORWARD_VGA      (UINT64_C(1) << 3)
#define LBA_SIC_CLEAR_LOG        (UINT64_C(1) << 4)
#define LBA_SIC_CLEAR_ENABLE     (UINT64_C(1) << 5)
#define LBA_SIC_HARD_FAIL        (UINT64_C(1) << 6)
#define LBA_SIC_RESET_COMPLETE   (UINT64_C(1) << 32)
#define LBA_SIC_LATCH_MASK       (LBA_SIC_FORWARD_VGA | LBA_SIC_CLEAR_LOG | \
                                  LBA_SIC_CLEAR_ENABLE | LBA_SIC_HARD_FAIL)
#define LBA_LMMIO_BASE_RESET     UINT64_C(0x80000000)
#define LBA_LMMIO_BASE_WRITE     UINT64_C(0x7fff0001)
#define LBA_LMMIO_MASK_RESET     UINT64_C(0x80000000)
#define LBA_LMMIO_MASK_WRITE     UINT64_C(0x7fff0000)
#define LBA_GMMIO_BASE_WRITE     UINT64_C(0x00000ffffc000001)
#define LBA_GMMIO_MASK_WRITE     UINT64_C(0x00000ffffc000000)
#define LBA_WLMMIO_BASE_RESET    UINT64_C(0x80000000)
#define LBA_WLMMIO_BASE_WRITE    UINT64_C(0x7ff00001)
#define LBA_WLMMIO_MASK_RESET    UINT64_C(0x80000000)
#define LBA_WLMMIO_MASK_WRITE    UINT64_C(0x7ff00000)
#define LBA_WGMMIO_BASE_WRITE    UINT64_C(0x00000fff00000001)
#define LBA_WGMMIO_MASK_WRITE    UINT64_C(0x00000fff00000000)
#define LBA_ELMMIO_BASE_RESET    UINT64_C(0x80000000)
#define LBA_ELMMIO_BASE_WRITE    UINT64_C(0x7ff00001)
#define LBA_ELMMIO_MASK_RESET    UINT64_C(0x80000000)
#define LBA_ELMMIO_MASK_WRITE    UINT64_C(0x7ff00000)
#define LBA_MSI_BASE_RESET       UINT64_C(0x00000000fee00001)
#define LBA_MSI_BASE_WRITE       UINT64_C(0x00000fffffff0001)
#define LBA_MSI_MASK_RESET       UINT64_C(0x00000ffffff00000)
#define LBA_MSI_MASK_WRITE       UINT64_C(0x00000fffffff0000)
#define LBA_SLAVE_CONTROL_RESET  UINT64_C(0x00000006)
#define LBA_SLAVE_CONTROL_WRITE  UINT64_C(0x0006200f)
#define LBA_BUS_MODE_AGP         UINT64_C(0x00000001)
#define LBA_BUS_MODE_SIX_MASTERS (UINT64_C(1) << 3)
#define LBA_BUS_MODE_SAFE_WRITE  UINT64_C(0x00010100)

static uint64_t ia64_lba_size_mask(unsigned int size)
{
    return size >= 8 ? UINT64_MAX : (UINT64_C(1) << (size * 8)) - 1;
}

/* Masked read-modify-write of a register latch (upstream ioa_latch_write). */
static void ia64_lba_latch(uint64_t *latch, uint64_t writable,
                           uint64_t access_mask, uint64_t data)
{
    uint64_t mask = writable & access_mask;

    *latch = (*latch & ~mask) | (data & mask);
}

/*
 * The 64-bit value at CSR offset [base, base+8) for reads.  hp-agp/hpagp read
 * sub-fields with byte-lane accesses; the caller extracts the requested lanes.
 * Every offset not modelled reads as zero.  CONFIG_DATA (0x48) is handled
 * separately by ia64_lba_config_read().
 */
static uint64_t ia64_lba_reg(IA64LBAState *s, uint64_t base)
{
    switch (base) {
    case LBA_FUNCTION_ID:
        /* vendor | device | command(0) | status(CAP_LIST set). */
        return IA64_LBA_VENDOR_ID |
               (IA64_LBA_DEVICE_ID << 16) |
               (IA64_LBA_PCI_STATUS_RESET << 48);
    case LBA_FUNCTION_CLASS:
        /* revision (byte 0x08) | class code (bytes 0x09-0x0b): host bridge. */
        return IA64_LBA_REVISION | (IA64_LBA_CLASS_CODE << 8);
    case LBA_CAP_POINTER:
        /* capabilities pointer lands in byte 0x34 -> the AGP capability. */
        return IA64_LBA_AGP_CAP_OFFSET << 32;
    case LBA_CONFIG_ADDRESS:
        return s->config_address;
    case LBA_BUS_NUMBER:
        /* secondary (byte 0x58) and subordinate (byte 0x59): the Mercury bus.
         * Fixed by the machine, so read-only in this model. */
        return IA64_MERCURY_BUS | (IA64_MERCURY_BUS << 8);
    case LBA_AGP_CAPABILITY:
        /* AGP capability: id 0x02 at byte 0x60, AGP status at byte 0x64. */
        return IA64_LBA_AGP_CAPABILITY;
    case LBA_AGP_COMMAND:
        return s->agp_command;
    case LBA_ARBITRATION_MASK:
        return s->arbitration_mask;
    case LBA_STATUS_CONTROL:
        /* SIC latch; reset-complete reads set (this model never asserts the
         * subordinate-bus reset the SIC RESET_FUNCTION bit would drive). */
        return (s->status_control & LBA_SIC_LATCH_MASK) | LBA_SIC_RESET_COMPLETE;
    case LBA_LMMIO_BASE:   return s->lmmio_base;
    case LBA_LMMIO_MASK:   return s->lmmio_mask;
    case LBA_GMMIO_BASE:   return s->gmmio_base;
    case LBA_GMMIO_MASK:   return s->gmmio_mask;
    case LBA_WLMMIO_BASE:  return s->wlmmio_base;
    case LBA_WLMMIO_MASK:  return s->wlmmio_mask;
    case LBA_WGMMIO_BASE:  return s->wgmmio_base;
    case LBA_WGMMIO_MASK:  return s->wgmmio_mask;
    case LBA_ELMMIO_BASE:  return s->elmmio_base;
    case LBA_ELMMIO_MASK:  return s->elmmio_mask;
    case LBA_SLAVE_CONTROL: return s->slave_control;
    case LBA_MSI_BASE:     return s->msi_base;
    case LBA_MSI_MASK:     return s->msi_mask;
    case LBA_BUS_MODE:     return s->bus_mode;
    default:
        return 0;
    }
}

/* Decode CONFIG_ADDRESS into a device on the Mercury bus + config offset. */
static PCIDevice *ia64_lba_config_target(IA64LBAState *s, unsigned int lane,
                                         uint32_t *reg)
{
    uint32_t addr = s->config_address & LBA_CONFIG_ADDRESS_MASK;
    uint8_t bus = (addr >> 16) & 0xff;
    uint8_t dev = (addr >> 11) & 0x1f;
    uint8_t func = (addr >> 8) & 0x7;

    *reg = (addr & 0xfc) | lane;
    if (s->mercury_bus == NULL) {
        return NULL;
    }
    return pci_find_device(s->mercury_bus, bus, PCI_DEVFN(dev, func));
}

static uint64_t ia64_lba_config_read(IA64LBAState *s, unsigned int lane,
                                     unsigned int size)
{
    unsigned int cycle = size == 8 ? 4 : size;
    uint32_t reg;
    PCIDevice *d;

    if (lane + cycle > 4) {
        return 0;
    }
    d = ia64_lba_config_target(s, lane, &reg);
    if (d == NULL) {
        return ia64_lba_size_mask(cycle);   /* open bus: all-ones */
    }
    return pci_host_config_read_common(d, reg, pci_config_size(d), cycle);
}

static void ia64_lba_config_write(IA64LBAState *s, unsigned int lane,
                                  unsigned int size, uint64_t value)
{
    unsigned int cycle = size == 8 ? 4 : size;
    uint32_t reg;
    PCIDevice *d;

    if (lane + cycle > 4) {
        return;
    }
    d = ia64_lba_config_target(s, lane, &reg);
    if (d != NULL) {
        pci_host_config_write_common(d, reg, pci_config_size(d),
                                     value & ia64_lba_size_mask(cycle), cycle);
    }
}

static MemTxResult ia64_lba_read(void *opaque, hwaddr addr, uint64_t *data,
                                 unsigned int size, MemTxAttrs attrs)
{
    IA64LBAState *s = opaque;
    unsigned int lane = addr & 7;
    uint64_t base = addr & ~UINT64_C(7);
    uint64_t reg;

    (void)attrs;
    if ((size != 1 && size != 2 && size != 4 && size != 8) || lane + size > 8) {
        *data = 0;
        return MEMTX_OK;
    }
    if (base == LBA_CONFIG_DATA) {
        *data = ia64_lba_config_read(s, (unsigned int)(addr - LBA_CONFIG_DATA),
                                     size);
        return MEMTX_OK;
    }
    reg = ia64_lba_reg(s, base);
    *data = (reg >> (lane * 8)) & ia64_lba_size_mask(size);
    return MEMTX_OK;
}

static MemTxResult ia64_lba_write(void *opaque, hwaddr addr, uint64_t value,
                                  unsigned int size, MemTxAttrs attrs)
{
    IA64LBAState *s = opaque;
    unsigned int lane = addr & 7;
    uint64_t base = addr & ~UINT64_C(7);
    uint64_t mask, data, latch, writable;

    (void)attrs;
    if ((size != 1 && size != 2 && size != 4 && size != 8) || lane + size > 8) {
        return MEMTX_OK;
    }
    if (base == LBA_CONFIG_DATA) {
        ia64_lba_config_write(s, (unsigned int)(addr - LBA_CONFIG_DATA), size,
                              value);
        return MEMTX_OK;
    }

    /* Right-justified byte-lane write of the addressed sub-field. */
    mask = ia64_lba_size_mask(size) << (lane * 8);
    data = (value << (lane * 8)) & mask;

    switch (base) {
    case LBA_CONFIG_ADDRESS:
        latch = s->config_address;
        ia64_lba_latch(&latch, LBA_CONFIG_ADDRESS_MASK, mask, data);
        s->config_address = (uint32_t)(latch & LBA_CONFIG_ADDRESS_MASK);
        break;
    case LBA_AGP_COMMAND:
        latch = s->agp_command;
        ia64_lba_latch(&latch, IA64_LBA_AGP_COMMAND_WRITABLE, mask, data);
        s->agp_command = (uint32_t)latch;
        break;
    case LBA_ARBITRATION_MASK:
        /* The F bit (six-masters arbitration) is writable only in that mode. */
        writable = LBA_ARBITRATION_WRITE;
        if (!(s->bus_mode & LBA_BUS_MODE_SIX_MASTERS)) {
            writable &= ~(uint64_t)LBA_ARBITRATION_F;
        }
        latch = s->arbitration_mask;
        ia64_lba_latch(&latch, writable, mask, data);
        s->arbitration_mask = (uint32_t)latch;
        break;
    case LBA_STATUS_CONTROL:
        latch = s->status_control;
        ia64_lba_latch(&latch, LBA_SIC_LATCH_MASK, mask, data);
        s->status_control = (uint32_t)latch;
        break;
    case LBA_LMMIO_BASE:
        ia64_lba_latch(&s->lmmio_base, LBA_LMMIO_BASE_WRITE, mask, data);
        break;
    case LBA_LMMIO_MASK:
        ia64_lba_latch(&s->lmmio_mask, LBA_LMMIO_MASK_WRITE, mask, data);
        break;
    case LBA_GMMIO_BASE:
        ia64_lba_latch(&s->gmmio_base, LBA_GMMIO_BASE_WRITE, mask, data);
        break;
    case LBA_GMMIO_MASK:
        ia64_lba_latch(&s->gmmio_mask, LBA_GMMIO_MASK_WRITE, mask, data);
        break;
    case LBA_WLMMIO_BASE:
        ia64_lba_latch(&s->wlmmio_base, LBA_WLMMIO_BASE_WRITE, mask, data);
        break;
    case LBA_WLMMIO_MASK:
        ia64_lba_latch(&s->wlmmio_mask, LBA_WLMMIO_MASK_WRITE, mask, data);
        break;
    case LBA_WGMMIO_BASE:
        ia64_lba_latch(&s->wgmmio_base, LBA_WGMMIO_BASE_WRITE, mask, data);
        break;
    case LBA_WGMMIO_MASK:
        ia64_lba_latch(&s->wgmmio_mask, LBA_WGMMIO_MASK_WRITE, mask, data);
        break;
    case LBA_ELMMIO_BASE:
        ia64_lba_latch(&s->elmmio_base, LBA_ELMMIO_BASE_WRITE, mask, data);
        break;
    case LBA_ELMMIO_MASK:
        ia64_lba_latch(&s->elmmio_mask, LBA_ELMMIO_MASK_WRITE, mask, data);
        break;
    case LBA_SLAVE_CONTROL:
        ia64_lba_latch(&s->slave_control, LBA_SLAVE_CONTROL_WRITE, mask, data);
        break;
    case LBA_MSI_BASE:
        ia64_lba_latch(&s->msi_base, LBA_MSI_BASE_WRITE, mask, data);
        break;
    case LBA_MSI_MASK:
        ia64_lba_latch(&s->msi_mask, LBA_MSI_MASK_WRITE, mask, data);
        break;
    case LBA_BUS_MODE:
        ia64_lba_latch(&s->bus_mode, LBA_BUS_MODE_SAFE_WRITE, mask, data);
        break;
    default:
        break;  /* read-only / unmodelled registers ignore writes */
    }
    return MEMTX_OK;
}

static const MemoryRegionOps ia64_lba_ops = {
    .read_with_attrs = ia64_lba_read,
    .write_with_attrs = ia64_lba_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8, .unaligned = true },
    .impl = { .min_access_size = 1, .max_access_size = 8, .unaligned = true },
};

void ia64_lba_set_mercury_bus(IA64LBAState *s, PCIBus *bus)
{
    s->mercury_bus = bus;
}

static void ia64_lba_reset(DeviceState *dev);

static void ia64_lba_realize(DeviceState *dev, Error **errp)
{
    IA64LBAState *s = IA64_LBA(dev);

    (void)errp;
    memory_region_init_io(&s->csr, OBJECT(s), &ia64_lba_ops, s,
                          "ia64-zx1-lba", IA64_LBA_CSR_SIZE);
    memory_region_add_subregion(get_system_memory(), s->csr_base, &s->csr);
    /*
     * Seed the register reset values here too: this device sits on no qbus
     * (it is mapped straight into system memory), so it is outside the machine
     * reset tree and its legacy reset method would not otherwise run.
     */
    ia64_lba_reset(dev);
}

static void ia64_lba_reset(DeviceState *dev)
{
    IA64LBAState *s = IA64_LBA(dev);

    s->config_address = 0;
    s->agp_command = 0;
    s->arbitration_mask = LBA_ARBITRATION_RESET;
    s->status_control = 0;
    s->lmmio_base = LBA_LMMIO_BASE_RESET;
    s->lmmio_mask = LBA_LMMIO_MASK_RESET;
    s->gmmio_base = 0;
    s->gmmio_mask = 0;
    s->wlmmio_base = LBA_WLMMIO_BASE_RESET;
    s->wlmmio_mask = LBA_WLMMIO_MASK_RESET;
    s->wgmmio_base = 0;
    s->wgmmio_mask = 0;
    s->elmmio_base = LBA_ELMMIO_BASE_RESET;
    s->elmmio_mask = LBA_ELMMIO_MASK_RESET;
    s->msi_base = LBA_MSI_BASE_RESET;
    s->msi_mask = LBA_MSI_MASK_RESET;
    s->slave_control = LBA_SLAVE_CONTROL_RESET;
    s->bus_mode = LBA_BUS_MODE_AGP;
}

static const VMStateDescription vmstate_ia64_lba = {
    .name = "ia64-zx1-lba",
    .version_id = 2,
    .minimum_version_id = 2,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(config_address, IA64LBAState),
        VMSTATE_UINT32(agp_command, IA64LBAState),
        VMSTATE_UINT32(arbitration_mask, IA64LBAState),
        VMSTATE_UINT32(status_control, IA64LBAState),
        VMSTATE_UINT64(lmmio_base, IA64LBAState),
        VMSTATE_UINT64(lmmio_mask, IA64LBAState),
        VMSTATE_UINT64(gmmio_base, IA64LBAState),
        VMSTATE_UINT64(gmmio_mask, IA64LBAState),
        VMSTATE_UINT64(wlmmio_base, IA64LBAState),
        VMSTATE_UINT64(wlmmio_mask, IA64LBAState),
        VMSTATE_UINT64(wgmmio_base, IA64LBAState),
        VMSTATE_UINT64(wgmmio_mask, IA64LBAState),
        VMSTATE_UINT64(elmmio_base, IA64LBAState),
        VMSTATE_UINT64(elmmio_mask, IA64LBAState),
        VMSTATE_UINT64(msi_base, IA64LBAState),
        VMSTATE_UINT64(msi_mask, IA64LBAState),
        VMSTATE_UINT64(slave_control, IA64LBAState),
        VMSTATE_UINT64(bus_mode, IA64LBAState),
        VMSTATE_END_OF_LIST()
    },
};

static const Property ia64_lba_properties[] = {
    DEFINE_PROP_UINT64("csr-base", IA64LBAState, csr_base, IA64_LBA_CSR_BASE),
};

static void ia64_lba_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    (void)data;
    dc->realize = ia64_lba_realize;
    dc->desc = "HP zx1 LBA/Mercury CSR block";
    dc->vmsd = &vmstate_ia64_lba;
    device_class_set_legacy_reset(dc, ia64_lba_reset);
    device_class_set_props(dc, ia64_lba_properties);
    dc->user_creatable = false;
}

static const TypeInfo ia64_lba_info = {
    .name          = TYPE_IA64_LBA,
    .parent        = TYPE_DEVICE,
    .instance_size = sizeof(IA64LBAState),
    .class_init    = ia64_lba_class_init,
};

static void ia64_lba_register_types(void)
{
    type_register_static(&ia64_lba_info);
}

type_init(ia64_lba_register_types)
