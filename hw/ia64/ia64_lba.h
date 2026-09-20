/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * HP zx1 LBA (Local Bus Adapter) AGP capability block for the zx1 machine.
 */

#ifndef HW_IA64_LBA_H
#define HW_IA64_LBA_H

#include "hw/core/qdev.h"
#include "hw/pci/pci_bus.h"
#include "system/memory.h"
#include "qom/object.h"

#define TYPE_IA64_LBA "ia64-zx1-lba"
OBJECT_DECLARE_SIMPLE_TYPE(IA64LBAState, IA64_LBA)

struct IA64LBAState {
    DeviceState parent_obj;

    MemoryRegion csr;              /* CSR block, mapped at csr_base */
    MemoryRegion regs;             /* the registers below, inside it */
    MemoryRegion iosapic_mr;       /* the ioa's own I/O SAPIC, at 0x800 */
    DeviceState *iosapic;
    uint64_t csr_base;             /* fixed chipset MMIO base (IA64_LBA_CSR_BASE) */
    PCIBus *config_bus;            /* bus CONFIG_ADDRESS/DATA cycles on */

    /* Writable Mercury CSR registers (reset values in ia64_lba_reset). */
    uint32_t config_address;       /* CONFIG_ADDRESS (0x40) selector */
    uint32_t bus_number;           /* BUS_NUMBER (0x58) secondary|subordinate */
    uint32_t agp_command;          /* AGP_COMMAND (0x68) */
    uint32_t arbitration_mask;     /* ARBITRATION_MASK (0x80) */
    uint32_t status_control;       /* STATUS_CONTROL / SIC (0x108) */
    uint64_t lmmio_base, lmmio_mask;    /* LMMIO decode  (0x200/0x208) */
    uint64_t gmmio_base, gmmio_mask;    /* GMMIO decode  (0x210/0x218) */
    uint64_t wlmmio_base, wlmmio_mask;  /* WLMMIO decode (0x220/0x228) */
    uint64_t wgmmio_base, wgmmio_mask;  /* WGMMIO decode (0x230/0x238) */
    uint64_t elmmio_base, elmmio_mask;  /* ELMMIO decode (0x250/0x258) */
    uint64_t msi_base, msi_mask;        /* MSI window    (0x280/0x288) */
    uint64_t slave_control;             /* SLAVE_CONTROL (0x278) */
    uint64_t bus_mode;                  /* BUS_MODE      (0x620) */
    uint64_t rope_config;               /* rope width    (0x610) */
};

/* Wire the root bus whose configuration space CONFIG_ADDRESS/DATA reaches. */
void ia64_lba_set_config_bus(IA64LBAState *s, PCIBus *bus);

/*
 * An interrupt input of this ioa's own I/O SAPIC.  The zx1 ioa ERS sec 11.2
 * gives every ioa one, and the vendor firmware finds it there and publishes it
 * in the MADT, so PCI interrupts on this rope reach the processor through it.
 */
qemu_irq ia64_lba_iosapic_input(IA64LBAState *s, unsigned int pin);

#endif /* HW_IA64_LBA_H */
