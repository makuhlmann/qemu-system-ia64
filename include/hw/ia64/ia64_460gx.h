/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Intel 460GX chipset: the System Address Controller's register aperture,
 * the CF8/CFC configuration mechanism with the chipset's own bus-CBN
 * configuration space (SAC, SDC, memory cards, expander ports), the memory
 * cards' SPD tunnel, the PCIS-programmed DRAM/PCI gap, and the diagnostic
 * port.
 */

#ifndef HW_IA64_460GX_H
#define HW_IA64_460GX_H

#include "hw/core/sysbus.h"
#include "hw/pci/pci_bus.h"
#include "hw/ia64/ia64_vpc_abi.h"

#define TYPE_IA64_460GX "ia64-460gx"
OBJECT_DECLARE_SIMPLE_TYPE(IA64460GXState, IA64_460GX)

/*
 * 460GX Memory Card A: two stacks of four DIMM rows, each row four identical
 * DIMMs (SSDM Table 5-1: "4 DIMMs per row which must be populated as a unit",
 * "Up to 4 rows per stack", "2 stacks per card").  Memory Card B stays absent.
 */
#define IA64_460GX_MEM_ROWS 8
#define IA64_460GX_MEM_ROW_MIN_MB 64
/*
 * 460GX chipset CSR scratch below the IOAPIC window.  SAL_B's first act
 * after PAL_PROC_GET_FEATURES is a BSP-arbitration handshake here: clear
 * bit 7 at +0xCB0, poll +0xCC0 until bit 7 sets, then compare the low
 * 7 bits with LID.id (bios130.BIN @ 0xffe76700..0xffe76790).  Model the
 * grant register as always-granted-to-id-0; everything else in the page
 * is write-store/read-back scratch, logged for the stage-1 inventory.
 */
#define IA64_460GX_SAC_BASE      IA64_U64(0x00000000feb00000)
#define IA64_460GX_SAC_SIZE      0x10000
#define IA64_460GX_SAC_BOOT_SEM  0xcc0
/*
 * The SAC's function-0 indexed register file.  Neither the register pair nor
 * the file is published -- the SSDM documents only the SAC's error, monitor
 * and interrupt registers -- but the vendor firmware's use of it is not
 * ambiguous: it writes an entry number to 64h, reads 64h back to confirm the
 * selector took, then reads and rewrites 70h-73h, and it walks that sequence
 * over entries 01h, 03h-07h and 10h-1Eh -- Table 2-1's chipset device numbers,
 * expander ports included.  Backing 70h with one cell, as ordinary config
 * storage does, makes every entry the same cell: the firmware's own walk then
 * reads at entry 04h what it wrote at 03h, so anything it concludes about
 * which expander ports exist is an artefact of the alias.
 */
#define IA64_460GX_SAC_IDX_REG      0x64
#define IA64_460GX_SAC_IDX_DATA     0x70
#define IA64_460GX_SAC_IDX_ENTRIES  256
/* Expanders the SAC has ports for: Expander 0-3 at 10h-17h (Table 2-1). */
#define IA64_460GX_EXPANDER_COUNT   4

/* Called when the lowest programmed PCIS moves the top of the low DRAM band. */
typedef void (*IA64460GXWindowNotify)(void *opaque, uint64_t base);

struct IA64460GXState {
    SysBusDevice parent_obj;

    MemoryRegion post_io;
    MemoryRegion sac_mmio;
    MemoryRegion cfg_io;
    uint8_t *sac_data;
    uint16_t post_last;
    uint32_t cfg_address;
    /* 460GX chipset config space: bus CBN devices, 8 fns x 256 bytes. */
    uint8_t *chipset_cfg;
    /* The SAC function-0 register file behind 64h/70h, one per SAC. */
    uint8_t sac_indexed[2][IA64_460GX_SAC_IDX_ENTRIES][4];
    /* DIMM size per Memory Card A row, in MB; 0 = row not populated. */
    uint32_t mem_row_dimm_mb[IA64_460GX_MEM_ROWS];
    uint64_t ram_size;

    /* The primary PCI host bridge (its routed low MMIO window follows PCIS). */
    DeviceState *pci_host;
    /* The compatibility bus and the expander roots, attached by the board. */
    PCIBus *compat_bus;
    PCIBus *root_bus[IA64_460GX_EXPANDER_ROOTS];
    IA64460GXWindowNotify window_notify;
    void *window_opaque;
};

/*
 * Create and realize the chipset as a child of @parent: the SAC aperture at
 * IA64_460GX_SAC_BASE, and the diagnostic port (80h) and CF8/CFC pair in
 * @pci_io.  @pci_host is the primary host bridge whose routed window follows
 * PCIS; @ram_size populates Memory Card A; @notify is told when the lowest
 * PCIS moves the top of the low DRAM band.
 */
IA64460GXState *ia64_460gx_create(Object *parent, MemoryRegion *pci_io,
                                  DeviceState *pci_host, uint64_t ram_size,
                                  IA64460GXWindowNotify notify, void *opaque,
                                  Error **errp);

/* Attach the compatibility bus (@root < 0) or expander root @root's bus. */
void ia64_460gx_attach_root(IA64460GXState *s, int root, PCIBus *bus);

#endif /* HW_IA64_460GX_H */
