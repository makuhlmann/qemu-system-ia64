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

/*
 * The power-up configuration an ioa latches (ERS 3.2.1) as BUS_MODE shows it
 * (ERS 3.2.2), for the kinds of rope the rx2600 has: its ropes 0, 2 and 6
 * read 0460h, rope 1 04E0h, rope 3 7CE0h and the AGP rope 4 0081h.  Bit 10
 * (and on rope 3 bits 12:11) is reserved and reads 1 there.
 */
#define IA64_LBA_STRAPS_PCI33  0x0460U   /* single rope, 3.3 V */
#define IA64_LBA_STRAPS_PCI66  0x04e0U   /* the same with M66EN */
#define IA64_LBA_STRAPS_PCIX   0x7ce0U   /* PCI-X 133, the rx2600's rope 3 */
#define IA64_LBA_STRAPS_AGP    0x0081U   /* AGP, dual rope, 1.5 V, M66EN */
OBJECT_DECLARE_SIMPLE_TYPE(IA64LBAState, IA64_LBA)

struct IA64LBAState {
    DeviceState parent_obj;

    MemoryRegion csr;              /* CSR block, mapped at csr_base */
    MemoryRegion regs;             /* the registers below, inside it */
    MemoryRegion iosapic_mr;       /* the ioa's own I/O SAPIC, at 0x800 */
    DeviceState *iosapic;
    uint64_t csr_base;             /* fixed chipset MMIO base (IA64_LBA_CSR_BASE) */
    uint32_t straps;               /* BUS_MODE's read-only bits */
    PCIBus *config_bus;            /* bus CONFIG_ADDRESS/DATA cycles on */

    /* Writable Mercury CSR registers (reset values in ia64_lba_reset). */
    uint32_t config_address;       /* CONFIG_ADDRESS (0x40) selector */
    uint32_t bus_number;           /* BUS_NUMBER (0x58) secondary|subordinate */
    uint32_t agp_command;          /* AGP_COMMAND (0x68) */
    uint32_t arbitration_mask;     /* ARBITRATION_MASK (0x80) */
    uint64_t pci_command;          /* FUNCTION_ID bits 47:32 */
    uint64_t line_latency;         /* FUNCTION_CLASS bits 47:32 */
    uint32_t arbitration_mode;     /* ARBITRATION_MODE (0x90) */
    uint32_t mt_latency;           /* MULTI-TRANSACTION LATENCY TIMER (0x98) */
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
    /* The error-log block of ioa ERS sec 6; storage, this model logs none. */
    uint64_t error_config;              /* ERROR_CONFIG  (0x680) */
    uint64_t error_control;             /* undocumented  (0x6a0) */
    uint64_t pcix_cap;                  /* PCI-X cap/cmd (0x0a0) */
    uint64_t rope_error;                /* undocumented  (0x600) */
    uint64_t error_status;              /* ERROR_STATUS  (0x688) */
    uint64_t error_master_id;           /* MASTER ID LOG (0x690) */
    uint64_t inbound_err_addr;          /* IN ERR ADDR   (0x290) */
    uint64_t inbound_err_attr;          /* IN ERR ATTR   (0x298) */
    uint64_t completion_msg;            /* COMPL MSG LOG (0x2a0) */
    uint64_t outbound_err_addr;         /* OUT ERR ADDR  (0x070) */

    /*
     * Inbound memory writes of the rope's masters: the MSI window in front
     * of the SBA's DMA translation (ERS 9.2).
     */
    MemoryRegion dma_root;
    MemoryRegion dma_sba;
    MemoryRegion msi;
    AddressSpace dma_as;
    bool dma_attached;

    /* Offsets already named in the log; see the mio's own pair. */
    unsigned long *unimp_read;
    unsigned long *unimp_write;
};

/* Wire the root bus whose configuration space CONFIG_ADDRESS/DATA reaches. */
void ia64_lba_set_config_bus(IA64LBAState *s, PCIBus *bus);

/*
 * Make this ioa the inbound path of @bus's masters: writes into its MSI
 * window become interrupt transactions, the rest goes to @dma (the SBA).
 */
void ia64_lba_attach_dma(IA64LBAState *s, PCIBus *bus, MemoryRegion *dma);

/*
 * An interrupt input of this ioa's own I/O SAPIC.  The zx1 ioa ERS sec 11.2
 * gives every ioa one, and the vendor firmware finds it there and publishes it
 * in the MADT, so PCI interrupts on this rope reach the processor through it.
 */
qemu_irq ia64_lba_iosapic_input(IA64LBAState *s, unsigned int pin);

#endif /* HW_IA64_LBA_H */
