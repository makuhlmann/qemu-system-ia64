/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * HP "Longs Peak": the A7231-66010 system board of the HP Workstation
 * zx2000 / zx6000 and the HP Integrity rx2600 -- Itanium 2 processors on
 * the HP zx1 chipset (mio SBA + ioa LBA / Mercury).  Machine type "zx1"
 * (default; "ia64-vpc" is a deprecated alias).
 *
 * The zx1 mio and ioa ERS (docs/HP zx1 Chipset/) are the authorities.
 */

#include "qemu/osdep.h"
#include "qemu/cutils.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/or-irq.h"
#include "hw/core/sysbus.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_bus.h"
#include "hw/ia64/ia64_lba.h"
#include "hw/ia64/ia64_mercury.h"
#include "hw/ia64/ia64_sba.h"
#include "hw/ia64/ia64_iosapic.h"
#include "hw/core/split-irq.h"
#include "longspeak_pdh.h"
#include "system/address-spaces.h"
#include "target/ia64/cpu.h"
#include "ia64_vpc_internal.h"

/* The first network adapter's slot on the single PCI0 root. */
#define IA64_VPC_NIC_SLOT           6
/* Longs Peak core I/O: the SCSI adapter is device 1 of the root. */
#define LONGSPEAK_SCSI_SLOT         1
/* How many of the ioa I/O SAPIC's pins the board wires. */
#define LONGSPEAK_INTX_PINS         6
/*
 * The rx2600's AGP ioa is on rope 4, double-wide with rope 5, at
 * FED2_8000 (rx2600 capture of 2026-10-03, SCRAM LBA records).
 */
#define LONGSPEAK_AGP_ROPE          4
/*
 * Rope 1's I/O SAPIC takes the PDH UART0 and UART1 on inputs 7 and 8 and the
 * SCI on input 9: GSI 34 to 36 under its base of 27 (the rx2600's SPCR,
 * HCDP, FADT and SCRAM serial records).
 */
#define LONGSPEAK_UART_PIN          7
#define LONGSPEAK_SCI_PIN           9

/*
 * The I/O backplane's PCI and PCI-X ropes, empty here.  SAL_B's table for a
 * board with the backplane names them (FFEAB2F0, longspeak_bmc.c), and each
 * has an ioa on the rx2600.
 */
static const struct {
    unsigned int rope;
    uint32_t straps;
} longspeak_backplane_ropes[] = {
    { 2, IA64_LBA_STRAPS_PCI33 },
    { 3, IA64_LBA_STRAPS_PCIX },
    { 6, IA64_LBA_STRAPS_PCI33 },
};

/* An ioa that answers only in the rope guest window the mio opens. */
static DeviceState *longspeak_rope_ioa(uint32_t straps, Error **errp)
{
    DeviceState *ioa = qdev_new(TYPE_IA64_LBA);

    object_property_set_uint(OBJECT(ioa), "csr-base", 0, &error_abort);
    qdev_prop_set_uint32(ioa, "straps", straps);
    if (!qdev_realize_and_unref(ioa, NULL, errp)) {
        return NULL;
    }
    return ioa;
}

/*
 * The board's INTx wiring, as the vendor firmware's own SCRAM interrupt
 * records give it: device 1 INTA/INTB/INTC on pins 0/1/2, device 2 INTA on
 * pin 5 and device 3 INTA on pin 4 of the rope's I/O SAPIC (read out of the
 * record array, decoded by \LBA.PRTE; the firmware publishes that I/O SAPIC
 * with global-interrupt base 16, so its _PRT names them 16..21).  A slot the
 * table does not name keeps the (slot + pin) % 4 swizzle.  Keep in lockstep
 * with the _PRT packages in roms/ia64-firmware/dsdt-pci-root-zx1.asl.
 */
static const IA64IntxRoute longspeak_pci0_intx[] = {
    { LONGSPEAK_SCSI_SLOT, { 0, 1, 2, 3 } },   /* core I/O SCSI */
    { 0x02, { 5, 5, 5, 5 } },                  /* core I/O LAN  */
    { 0x03, { 4, 4, 4, 4 } },                  /* core I/O USB  */
};

/*
 * The mio's fixed DRAM map (mio ERS 2.1): Memory0 from 0 to the I/O virtual
 * region at 1 GiB, then Memory1 at 0x40_4000_0000 for up to 3 GiB; the rest
 * is Memory2, the generic run at 4 GiB.  No DRAM decodes from 1 GiB to 4 GiB,
 * so the SBA's IOVA window and the LMMIO range overlap no memory.  Keep this
 * in lockstep with probe_ram_zx1() and fw_init_guest_high_ram_ranges() in
 * roms/ia64-firmware/.
 */
static uint64_t longspeak_map_low_ram(IA64VpcMachineState *s, uint64_t offset,
                                      uint64_t remaining)
{
    uint64_t size;

    size = ia64_vpc_map_ram_alias(s, 0, offset, remaining,
                                  IA64_ZX1_MEMORY0_END, "ia64-vpc.memory0");
    return size + ia64_vpc_map_ram_alias(s, IA64_ZX1_MEMORY1_BASE,
                                         offset + size, remaining - size,
                                         IA64_ZX1_MEMORY1_SIZE,
                                         "ia64-vpc.memory1");
}

static bool longspeak_validate(IA64VpcMachineState *s, Error **errp)
{
    uint64_t max = IA64_ZX1_MEMORY0_END + IA64_ZX1_MEMORY1_SIZE +
                   IA64_ZX1_MEMORY2_END - IA64_ZX1_MEMORY2_BASE;

    if (MACHINE(s)->ram_size > max) {
        g_autofree char *top = size_to_str(max);

        error_setg(errp, "Invalid RAM size: the zx1 mio decodes at most %s",
                   top);
        return false;
    }
    return true;
}

/*
 * The firmware's LMMIO ranges say where PCI memory lives; PCI addresses are
 * CPU physical addresses here, so open the machine's PCI MMIO window there.
 * Our own firmware never writes those registers, so this fires only under
 * the vendor one.
 */
static void longspeak_lmmio_window_moved(void *opaque, uint64_t base)
{
    IA64VpcMachineState *s = opaque;

    ia64_pci_host_set_low_mmio_window(s->pci_host_dev, base);
}

static bool longspeak_build_chipset(IA64VpcMachineState *s,
                                    DeviceState *pci_host, PCIBus *pci_bus,
                                    MemoryRegion *pci_io, DeviceState *iosapic,
                                    Error **errp)
{
    DeviceState *pdh;
    SysBusDevice *pdh_sbd;
    unsigned int r;
    int i;

    /*
     * The PDH devices Dillon decodes below the flash (the battery-backed
     * SRAM, the volatile SRAM, processor presence, POST byte, Dillon
     * registers).  The HP firmware needs them from its first instructions.
     * Both firmwares keep their settings in the battery-backed part, so
     * `nvram=` stands in for the battery here rather than for the flash.
     */
    pdh = qdev_new(TYPE_LONGSPEAK_PDH);
    qdev_prop_set_uint32(pdh, "sockets", MACHINE(s)->smp.cpus);
    if (s->nvram_path != NULL) {
        BlockBackend *blk = ia64_vpc_open_pdh_store(s->nvram_path, errp);

        if (blk == NULL) {
            return false;
        }
        qdev_prop_set_drive(pdh, "store", blk);
    }
    pdh_sbd = SYS_BUS_DEVICE(pdh);
    if (!sysbus_realize_and_unref(pdh_sbd, errp)) {
        return false;
    }
    /*
     * The project firmware reads the machine's options from a record in its
     * own store, which on this board is in the part rather than the flash.
     */
    ia64_vpc_seed_store_defaults(s,
        (uint8_t *)memory_region_get_ram_ptr(&LONGSPEAK_PDH(pdh)->bbsram) +
        IA64_PDH_STORE_VARS_OFFSET);
    longspeak_pdh_store_seeded(pdh);
    sysbus_mmio_map(pdh_sbd, LONGSPEAK_PDH_MMIO_BBSRAM, IA64_PDH_BBSRAM_BASE);
    sysbus_mmio_map(pdh_sbd, LONGSPEAK_PDH_MMIO_SRAM, IA64_PDH_SRAM_BASE);
    for (i = 0; i < LONGSPEAK_PDH_BLOCKS; i++) {
        sysbus_mmio_map(pdh_sbd, LONGSPEAK_PDH_MMIO_BLOCK0 + i,
                        LONGSPEAK_PDH(pdh)->block[i].base);
    }

    s->sba_dev = pci_new(PCI_DEVFN(PCI_SLOT_MAX - 1, 0), TYPE_IA64_SBA);
    object_property_set_uint(OBJECT(s->sba_dev), "csr-base",
                             IA64_SBA_CSR_BASE, &error_abort);
    if (!pci_realize_and_unref(s->sba_dev, pci_bus, errp)) {
        return false;
    }
    /*
     * The zx1 LBA AGP capability block, published to guests as ACPI
     * HWP0003 nested inside the SBA (HWP0001).  Linux hp-agp negotiates AGP
     * mode against it and reuses the SBA IOPDIR as the GART; it is faithful
     * real-zx1 hardware, so it is present regardless of the agp option.
     * The AGP capability it advertises only becomes usable once the
     * graphics master also advertises a PCI AGP capability, which the agp
     * option gates below.
     */
    s->lba_dev = qdev_new(TYPE_IA64_LBA);
    object_property_set_uint(OBJECT(s->lba_dev), "csr-base",
                             IA64_LBA_CSR_BASE, &error_abort);
    qdev_prop_set_uint32(s->lba_dev, "straps", IA64_LBA_STRAPS_AGP);
    if (!qdev_realize_and_unref(s->lba_dev, NULL, errp)) {
        return false;
    }
    /*
     * The vendor firmware looks for an I/O host bridge in the rope guest
     * configuration space, one per rope, and does its PCI configuration
     * cycles through the bridge it finds there.  The primary root gets an ioa
     * of its own for that -- it answers only in the window, which our own
     * firmware never opens -- and the AGP bridge above keeps the fixed base
     * ACPI publishes for it.
     */
    s->rope0_lba_dev = longspeak_rope_ioa(IA64_LBA_STRAPS_PCI33, errp);
    if (s->rope0_lba_dev == NULL) {
        return false;
    }
    /*
     * Rope 1's ioa: the rx2600's carries the SCSI and the gigabit LAN, and its
     * I/O SAPIC takes the PDH UARTs and the SCI on inputs 7 to 9 (the vendor
     * SPCR, HCDP and FADT name GSI 34 to 36, base 27).  Its bus stays empty
     * here, so a configuration cycle on it finds nothing, as on an empty rope.
     */
    s->rope1_lba_dev = longspeak_rope_ioa(IA64_LBA_STRAPS_PCI66, errp);
    if (s->rope1_lba_dev == NULL) {
        return false;
    }
    ia64_sba_add_rope(IA64_SBA(s->sba_dev), 0,
                      &IA64_LBA(s->rope0_lba_dev)->csr);
    ia64_sba_add_rope(IA64_SBA(s->sba_dev), 1,
                      &IA64_LBA(s->rope1_lba_dev)->csr);
    ia64_sba_add_rope(IA64_SBA(s->sba_dev), LONGSPEAK_AGP_ROPE,
                      &IA64_LBA(s->lba_dev)->csr);
    for (r = 0; r < IA64_PDH_UARTS; r++) {
        sysbus_connect_irq(SYS_BUS_DEVICE(LONGSPEAK_PDH(pdh)->uart[r]), 0,
                           ia64_lba_iosapic_input(IA64_LBA(s->rope1_lba_dev),
                                                  LONGSPEAK_UART_PIN + r));
    }
    for (r = 0; r < ARRAY_SIZE(longspeak_backplane_ropes); r++) {
        DeviceState *ioa =
            longspeak_rope_ioa(longspeak_backplane_ropes[r].straps, errp);

        if (ioa == NULL) {
            return false;
        }
        ia64_sba_add_rope(IA64_SBA(s->sba_dev),
                          longspeak_backplane_ropes[r].rope,
                          &IA64_LBA(ioa)->csr);
    }
    ia64_sba_set_window_notify(IA64_SBA(s->sba_dev),
                               longspeak_lmmio_window_moved, s);
    /*
     * The Mercury (LBA/ioa) PCI host bridge: a second PCI root bus sharing
     * the primary host bridge's identity-mapped MMIO/I/O windows, which will
     * carry the AGP graphics adapter -- exactly as real zx1 puts the AGP
     * master behind Mercury.  The primary host's ECAM config handler
     * dispatches config cycles for IA64_MERCURY_BUS here, and the SBA extends
     * its shared DMA translation over this bus too, so a master here reaches
     * RAM above 4 GiB through the same IOPDIR/GART.  Attach the SBA before any
     * device is realized on the bus.
     */
    s->mercury_host = ia64_mercury_host_create(OBJECT(s),
                              ia64_pci_host_mmio(pci_host),
                              ia64_pci_host_io(pci_host),
                              IA64_MERCURY_BUS, errp);
    if (s->mercury_host == NULL) {
        return false;
    }
    s->mercury_bus = ia64_mercury_host_bus(s->mercury_host);
    ia64_pci_host_set_mercury_bus(pci_host, s->mercury_bus);
    ia64_sba_attach_bus(IA64_SBA(s->sba_dev), s->mercury_bus);
    /* The Mercury CSR CONFIG_ADDRESS/DATA pair does config on this bus. */
    ia64_lba_set_config_bus(IA64_LBA(s->lba_dev), s->mercury_bus);
    ia64_lba_set_config_bus(IA64_LBA(s->rope0_lba_dev), pci_bus);
    /*
     * QEMU gives a -device without bus= to the youngest PCI root, which is
     * now Mercury's: the AGP port, with the one slot of the graphics seat.
     * There a card has no _CRS window and no _PRT entry (Server 2003: code
     * 12).  Re-inserting the primary host makes its root the youngest, as
     * sdv.c orders its roots for the same reason.
     */
    qdev_set_parent_bus(pci_host, sysbus_get_default(), &error_abort);

#ifdef CONFIG_IA64_VPC_GRAPHICS
    /*
     * With the Rage 128's AGP capability Linux sba_iommu reserves the SBA
     * IOVA GART half (and writes the cookie hp-agp handshakes on), and
     * hp-agp can negotiate AGP mode.
     */
    ia64_vpc_rage128_agp(s);

#endif
    return true;
}

/* One interrupt line that reaches two controllers. */
static qemu_irq longspeak_split_irq(qemu_irq a, qemu_irq b)
{
    DeviceState *split = qdev_new(TYPE_SPLIT_IRQ);

    object_property_set_int(OBJECT(split), "num-lines", 2, &error_abort);
    qdev_realize_and_unref(split, NULL, &error_abort);
    qdev_connect_gpio_out(split, 0, a);
    qdev_connect_gpio_out(split, 1, b);
    return qdev_get_gpio_in(split, 0);
}

/*
 * Each root's lines reach two controllers: the I/O SAPIC of the root's own
 * ioa, which the vendor firmware finds and publishes (ERS sec 11.2), and the
 * platform IOSAPIC our own firmware publishes, where both roots wire-OR into
 * one block of lines.  A line an OS has not programmed stays masked, so only
 * the controller its firmware described ever delivers.
 */
static void longspeak_wire_intx(IA64VpcMachineState *s, DeviceState *pci_host,
                                DeviceState *iosapic)
{
    IA64LBAState *rope0 = IA64_LBA(s->rope0_lba_dev);
    IA64LBAState *agp = IA64_LBA(s->lba_dev);
    unsigned int i;

    for (i = 0; i < LONGSPEAK_INTX_PINS; i++) {
        DeviceState *org = qdev_new(TYPE_OR_IRQ);

        object_property_set_int(OBJECT(org), "num-lines", 2, &error_abort);
        qdev_realize_and_unref(org, NULL, &error_abort);
        qdev_connect_gpio_out(org, 0,
                              qdev_get_gpio_in(iosapic,
                                               IA64_PCI_INTX_GSI_BASE + i));
        qdev_connect_gpio_out(pci_host, i,
            longspeak_split_irq(ia64_lba_iosapic_input(rope0, i),
                                qdev_get_gpio_in(org, 0)));
        /* The second root swizzles into the first four lines only. */
        if (i < IA64_PCI_INTX_LINES) {
            qdev_connect_gpio_out(s->mercury_host, i,
                longspeak_split_irq(ia64_lba_iosapic_input(agp, i),
                                    qdev_get_gpio_in(org, 1)));
        }
    }
    /* The SCI also keeps the platform IOSAPIC line our own FADT names. */
    s->acpi_sci_irq = longspeak_split_irq(s->acpi_sci_irq,
        ia64_lba_iosapic_input(IA64_LBA(s->rope1_lba_dev), LONGSPEAK_SCI_PIN));
}

/*
 * zx1 is a different platform with a different south bridge, so it keeps
 * the parentless ISA bus until it gets one of its own.  It carries no clock:
 * "systems using zx1 mio do not support ISA" (mio ERS 2.2.1), and the
 * board's clock is the PDH part (longspeak_rtc.c).
 */
static ISABus *longspeak_build_isa(IA64VpcMachineState *s, PCIBus *pci_bus,
                                   MemoryRegion *pci_io, DeviceState *iosapic,
                                   Error **errp)
{
    ISABus *isa_bus;
    int i;

    isa_bus = isa_bus_new(NULL, get_system_memory(), pci_io, errp);
    if (isa_bus == NULL) {
        return NULL;
    }
    for (i = 0; i < ISA_NUM_IRQS; i++) {
        s->isa_irqs[i] = qdev_get_gpio_in(iosapic, i);
    }
    isa_bus_register_input_irqs(isa_bus, s->isa_irqs);
    return isa_bus;
}

static void longspeak_seat(IA64VpcMachineState *s, IA64VpcSeat seat,
                           PCIBus **bus, int *devfn)
{
    switch (seat) {
    case IA64_VPC_SEAT_SCSI:
        *devfn = PCI_DEVFN(LONGSPEAK_SCSI_SLOT, 0);
        break;
    case IA64_VPC_SEAT_SCSI_PARK:
        /* The second adapter takes the next free slot of the single root. */
        break;
    case IA64_VPC_SEAT_VGA:
        /*
         * The AGP graphics master sits behind the Mercury PCI host bridge,
         * on its own root bus -- exactly as real zx1 puts the AGP adapter
         * behind Mercury.
         */
        *bus = s->mercury_bus;
        *devfn = PCI_DEVFN(IA64_MERCURY_VGA_SLOT, 0);
        break;
    case IA64_VPC_SEAT_AUDIO:
        break;
    case IA64_VPC_SEAT_NIC:
        *devfn = PCI_DEVFN(IA64_VPC_NIC_SLOT, 0);
        break;
    }
}

/* Both roots wire-OR into one block of four lines. */
static unsigned int longspeak_root_gsi_base(const IA64VpcMachineState *s,
                                            uint8_t bus)
{
    return IA64_PCI_INTX_GSI_BASE;
}

/*
 * A Longs Peak processor's geographic id is its position on the bus:
 * (module << 1) | core.  The vendor SAL_A derives the id it publishes from
 * it with czx2.r(GR33 >> 1) -- the module -- and takes the core from the
 * module-layout register at FF5F_1010 (SAL_A FFFE_0EF0, SAL_B FFE7_94B0),
 * so two single-core processors have to arrive as ids 0 and 2, not 0 and 1:
 * with 0 and 1 SAL_A gives both the id 0, and its recovery-check rendezvous
 * then has the two processors sharing one check-in bit (cccfce6).
 *
 * The board has two sockets, so the first two processors fill the two
 * modules and the next two are those modules' second cores (an mx2 pair).
 * Processors 4-7, which no Longs Peak has, continue the same geography over
 * two more modules; every id stays below the eight per-processor slots the
 * project firmware carries (FW_MAX_CPUS), which it indexes by id.
 */
static const uint8_t longspeak_processor_ids[] = { 0, 2, 1, 3, 4, 6, 5, 7 };

/* Concrete: HP rx2600 / zx2000 / zx6000 -- zx1 chipset, Itanium 2. Default. */
static void longspeak_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    IA64VpcMachineClass *imc = IA64_VPC_MACHINE_CLASS(oc);

    (void)data;
    mc->desc = "HP rx2600 / zx2000 / zx6000 (zx1 chipset, Itanium 2)";
#ifdef CONFIG_IA64_VPC_NETWORK
    /* "The LAN controller is an Intel 82550 chip" (O&M Guide App. B). */
    mc->default_nic = "i82550";
#endif
    mc->default_cpu_type = IA64_CPU_TYPE_NAME("madison");
    /*
     * The zx1 generation dropped PS/2 entirely: an rx2600 or zx6000 has USB
     * keyboard and mouse only, so this machine defaults to the USB HID
     * devices instead (see ia64_vpc_init_usb).  Override with i8042=on.
     */
    imc->pci_config_ecam = true;
    imc->i8042_default = false;
    imc->processor_ids = longspeak_processor_ids;
    imc->nprocessor_ids = ARRAY_SIZE(longspeak_processor_ids);
    imc->map_low_ram = longspeak_map_low_ram;
    imc->low_ram_top = IA64_ZX1_MEMORY0_END;
    imc->validate = longspeak_validate;
    imc->build_chipset = longspeak_build_chipset;
    imc->pci0_intx = longspeak_pci0_intx;
    imc->pci0_nintx = ARRAY_SIZE(longspeak_pci0_intx);
    imc->pci0_intx_fallback = 0;
    imc->acpi_pm_mmio_base = IA64_PDH_ACPI_PM_BASE;
    /*
     * The rx2600's PDH timer counts 32 bits (FF5C_1004 read 2345_4096h, 165 s
     * after reset), as the vendor FADT's TMR_VAL_EXT announces, and its
     * PM1_EN keeps 0721h of the FFFFh SAL_B writes at FFE78E26 (rx2600
     * capture of 2026-10-03).
     */
    imc->acpi_pm_tmr_ext = true;
    imc->acpi_pm1_en_mask = 0x0721;
    /*
     * What rx2600/zx2000 carry: an LSI SCSI in core I/O and an ATI Rage XL
     * for video.  The Rage XL is also the adapter whose video BIOS the vendor
     * firmware runs: its x86 emulator interprets that ROM and sets a mode,
     * where it refuses the Rage 128's ("Unable to execute video bios")
     * without touching a VGA port.
     */
    imc->nvram_is_pdh_store = true;
    /*
     * The vendor firmware's DSDT declares _S5 as SLP_TYP 5 and the box
     * has no other sleep state.  Windows stores that with SLP_EN to power
     * off; a store the chipset ignores leaves the HAL to fall back on
     * EFI ResetSystem(EfiResetCold), which reboots instead
     * (WSRV03/base/hals/halia64/ia64/pmsleep.c).  Our own firmware's DSDT
     * uses 0, which the ACPI core takes anyway.
     */
    imc->acpi_s5_slp_typ = 5;
    /*
     * One soldered Intel 28F640J3 StrataFlash, 8 MiB in 64 blocks of 128 KiB,
     * the part the zx2000 flash dump is named for
     * (HP_ZX2000_IPF_Intel_28F640J3_00-7FFFFF.bin).
     * It has no FWH register interface: the J3 locks blocks by command.  The
     * vendor firmware issues none over a POST to the EFI shell, so the lock
     * commands stay unmodelled and the part answers unlocked.
     */
    imc->flash_part_size = 8 * MiB;
    imc->flash_sector_len = 128 * KiB;
    imc->flash_device_id = 0x0017;
    imc->flash_block_locking = false;
    imc->lsi_default = true;
    imc->vga_default = "mach64";
    /* Device 1 is core I/O on this board; the opt-in AHCI takes device 4. */
    imc->ahci_slot = 4;
    imc->wire_intx = longspeak_wire_intx;
    imc->build_isa = longspeak_build_isa;
    imc->seat = longspeak_seat;
    imc->root_gsi_base = longspeak_root_gsi_base;
    /* The zx1 machine is the default; "ia64-vpc" is a deprecated alias of it. */
    mc->is_default = true;
    mc->alias = "ia64-vpc";
    ia64_vpc_add_compat_defaults(mc);
}

static const TypeInfo longspeak_machine_typeinfo = {
    .name = TYPE_IA64_ZX1_MACHINE,
    .parent = TYPE_IA64_VPC_MACHINE,
    .class_init = longspeak_machine_class_init,
};

static void longspeak_register_types(void)
{
    type_register_static(&longspeak_machine_typeinfo);
}

type_init(longspeak_register_types)
