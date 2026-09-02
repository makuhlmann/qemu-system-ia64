/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * HP zx1 LBA (Local Bus Adapter) AGP capability block for the ia64 zx1 machine.
 *
 * Linux hp-agp (drivers/char/agp/hp-agp.c) provides AGPGART on zx1 by reusing
 * the SBA IOC's in-DRAM IOPDIR as the GART; it locates the AGP-capable "Local
 * Bus Adapter" through the ACPI HWP0003 device, ioremaps its CSR block, and
 * reads it as ordinary PCI configuration space to find an AGP capability:
 *
 *   - PCI_STATUS (0x06) must have the capability-list bit (0x10) set;
 *   - the capability-list pointer (0x34) points at the AGP capability;
 *   - the AGP capability (id 0x02) sits at 0x60, with AGP status at 0x64 and a
 *     writable AGP command at 0x68.
 *
 * This MMIO shell serves those reads plus the Mercury identity registers a
 * driver reads to recognise the bridge: FUNCTION_ID (0x00, vendor/device +
 * command/status), FUNCTION_CLASS (0x08, host-bridge class + revision),
 * CAPABILITIES_POINTER (0x30) and BUS_NUMBER (0x58).  It carries no PCI config
 * space of its own and does no DMA translation: the AGP GART is the SBA's IOPDIR
 * (hp-agp's "shared" path).  The graphics adapter itself is a real PCI device on
 * the Mercury root bus (hw/ia64/ia64_mercury.c); this block only models the
 * Mercury CSR / AGP-capability registers.  The register values mirror upstream
 * hw/pci-host/hp-zx1-ioa-regs.c in AGP mode.  The block is described to guests
 * only through the ACPI HWP0003 _CRS (a CCSR VendorLong and a memory
 * descriptor), never the EFI memory map; keep base/length in lockstep with LBA0
 * in roms/ia64-firmware/dsdt-pci-root-zx1.asl.
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/log.h"
#include "hw/ia64/ia64_lba.h"
#include "hw/ia64/ia64_vpc_abi.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "system/address-spaces.h"

/*
 * The 64-bit "register" holding the config bytes at [base, base+8).  hp-agp
 * reads sub-fields with byte-lane accesses (INREG8/16/32), so the CSR ops below
 * extract the requested lanes from this value.  Every offset the driver does
 * not touch reads as zero.
 */
static uint64_t ia64_lba_reg(IA64LBAState *s, uint64_t base)
{
    switch (base) {
    case 0x00:
        /* FUNCTION_ID: vendor | device | command(0) | status(CAP_LIST set). */
        return IA64_LBA_VENDOR_ID |
               (IA64_LBA_DEVICE_ID << 16) |
               (IA64_LBA_PCI_STATUS_RESET << 48);
    case 0x08:
        /* FUNCTION_CLASS: revision (byte 0x08) | class code (bytes 0x09-0x0b),
         * i.e. a host bridge at revision 2.0.  Cache-line size and latency timer
         * (bytes 0x0c/0x0d) read 0, as real Mercury resets them. */
        return IA64_LBA_REVISION | (IA64_LBA_CLASS_CODE << 8);
    case 0x30:
        /* capabilities pointer lands in byte 0x34 -> the AGP capability. */
        return IA64_LBA_AGP_CAP_OFFSET << 32;
    case 0x58:
        /* BUS_NUMBER: secondary (byte 0x58) and subordinate (byte 0x59) bus of
         * the Mercury root -- both IA64_MERCURY_BUS (no downstream bridges). */
        return IA64_MERCURY_BUS | (IA64_MERCURY_BUS << 8);
    case 0x60:
        /* AGP capability: id 0x02 at byte 0x60, AGP status at byte 0x64. */
        return IA64_LBA_AGP_CAPABILITY;
    case 0x68:
        /* AGP command, writable (mask IA64_LBA_AGP_COMMAND_WRITABLE). */
        return s->agp_command;
    default:
        return 0;
    }
}

static uint64_t ia64_lba_size_mask(unsigned int size)
{
    return size >= 8 ? UINT64_MAX : (UINT64_C(1) << (size * 8)) - 1;
}

static MemTxResult ia64_lba_read(void *opaque, hwaddr addr, uint64_t *data,
                                 unsigned int size, MemTxAttrs attrs)
{
    IA64LBAState *s = opaque;
    unsigned int lane = addr & 7;
    uint64_t reg;

    (void)attrs;
    if ((size != 1 && size != 2 && size != 4 && size != 8) || lane + size > 8) {
        *data = 0;
        return MEMTX_OK;
    }
    reg = ia64_lba_reg(s, addr & ~UINT64_C(7));
    *data = (reg >> (lane * 8)) & ia64_lba_size_mask(size);
    return MEMTX_OK;
}

static MemTxResult ia64_lba_write(void *opaque, hwaddr addr, uint64_t value,
                                  unsigned int size, MemTxAttrs attrs)
{
    IA64LBAState *s = opaque;
    unsigned int lane = addr & 7;
    uint64_t base = addr & ~UINT64_C(7);

    (void)attrs;
    if ((size != 1 && size != 2 && size != 4 && size != 8) || lane + size > 8) {
        return MEMTX_OK;
    }
    /* Only the AGP command register (0x68) is writable; the rest is read-only. */
    if (base == 0x68) {
        uint64_t mask = ia64_lba_size_mask(size) << (lane * 8);
        uint64_t merged = ((uint64_t)s->agp_command & ~mask) |
                          ((value << (lane * 8)) & mask);

        s->agp_command = (uint32_t)(merged & IA64_LBA_AGP_COMMAND_WRITABLE);
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

static void ia64_lba_realize(DeviceState *dev, Error **errp)
{
    IA64LBAState *s = IA64_LBA(dev);

    (void)errp;
    memory_region_init_io(&s->csr, OBJECT(s), &ia64_lba_ops, s,
                          "ia64-zx1-lba", IA64_LBA_CSR_SIZE);
    memory_region_add_subregion(get_system_memory(), s->csr_base, &s->csr);
}

static void ia64_lba_reset(DeviceState *dev)
{
    IA64LBAState *s = IA64_LBA(dev);

    s->agp_command = 0;
}

static const VMStateDescription vmstate_ia64_lba = {
    .name = "ia64-zx1-lba",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(agp_command, IA64LBAState),
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
    dc->desc = "HP zx1 LBA AGP capability block";
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
