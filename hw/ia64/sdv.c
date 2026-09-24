/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The Intel Itanium SDV (Software Development Vehicle) board, sold by HP as
 * the Workstation i2000: Merced processors on the 460GX chipset, the
 * 82468GX south bridge, four PCI roots.  Machine type "460gx".
 *
 * The 460GX SSDM (248704-001) and the DSDT the vendor firmware (bios130.BIN)
 * builds, read back from guest RAM after its POST, are the authorities for
 * what is modelled here.
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/cutils.h"
#include "qapi/error.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "hw/i2c/i2c.h"
#include "hw/isa/smsc_lpc47b27x.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_bus.h"
#include "hw/ia64/ia64_agp.h"
#include "hw/ia64/ia64_expander.h"
#include "hw/ia64/ia64_460gx_identity.h"
#include "hw/ia64/ia64_i2000_hwmon.h"
#include "hw/ia64/ia64_iosapic.h"
#include "target/ia64/cpu.h"
#include "ia64_vpc_internal.h"

/* The 82557 seat on the compatibility bus. */
#define IA64_460GX_NIC_SLOT         5

static void ia64_vpc_realfw_apmc(void *opaque, int n, int level)
{
    IA64VpcMachineState *s = opaque;

    (void)n;
    if (level == IA64_460GX_ACPI_ENABLE_CMD) {
        intel_82468gx_ifb_acpi_sci_enable(s->ifb, true);
    } else if (level == IA64_460GX_ACPI_DISABLE_CMD) {
        intel_82468gx_ifb_acpi_sci_enable(s->ifb, false);
    }
}

/*
 * The master 8259's INTR line drives the boot processor's LINT0 pin.  What
 * the processor makes of it is up to its Local Redirection Register 0: the
 * SDV firmware programs a level-triggered ExtINT there for its legacy tick
 * (IVR reads 0 and it fetches the 8-bit vector through the INTA byte), and
 * an operating system masks the pin before it enables interrupts.  The PIC
 * never programs the IOSAPIC; the pair's INTR does not pass through it.
 */
static void ia64_vpc_extint(void *opaque, int n, int level)
{
    (void)opaque;
    (void)n;
    if (first_cpu != NULL) {
        ia64_cpu_set_lint(first_cpu, 0, level);
    }
}

/*
 * How the i2000 wires PCI INTx into the 460GX Programmable Interrupt Device,
 * per slot, INTA..INTD.  This is the board's own description of itself: the
 * _PRT packages of the vendor firmware's DSDT (W460GXBS, PLAT()=1 branch),
 * which Windows programs the PID's 64 inputs from.  A slot a root's table
 * does not list has no interrupt on the real board; here it swizzles into
 * the spare inputs at IA64_460GX_INTX_FALLBACK_GSI so an add-in card in an
 * unlisted slot still works.  Keep in lockstep with the _PRT packages in
 * roms/ia64-firmware/dsdt-pci-root.asl.
 */
static const IA64IntxRoute ia64_i2000_pci0_intx[] = {
    { 0x01, { 35, 34, 33, 32 } },
    { 0x02, { 39, 38, 37, 36 } },   /* OHCI */
    { 0x03, { 46, 46, 47, 47 } },   /* 82468GX: SMBus INTB, USB INTD */
    { 0x04, { 45, 45, 45, 45 } },
    { 0x05, { 44, 44, 44, 44 } },   /* 82557 */
};
static const IA64IntxRoute ia64_i2000_wxb0_intx[] = {
    { 0x00, { 19, 18, 17, 16 } },   /* SCSI */
    { 0x01, { 23, 22, 21, 20 } },
    { 0x02, { 43, 42, 41, 40 } },
    { 0x0f, { 56, 56, 56, 56 } },   /* hot-plug controller */
};
static const IA64IntxRoute ia64_i2000_wxb1_intx[] = {
    { 0x00, { 27, 26, 25, 24 } },
    { 0x01, { 31, 30, 29, 28 } },
    { 0x0f, { 57, 57, 57, 57 } },
};
static const IA64IntxRoute ia64_i2000_gxb_intx[] = {
    { 0x00, { 55, 54, 55, 54 } },   /* AGP graphics */
};

static const struct {
    const IA64IntxRoute *routes;
    unsigned int nroutes;
} ia64_i2000_root_intx[IA64_460GX_EXPANDER_ROOTS] = {
    [IA64_460GX_ROOT_WXB0] = { ia64_i2000_wxb0_intx,
                               ARRAY_SIZE(ia64_i2000_wxb0_intx) },
    [IA64_460GX_ROOT_WXB1] = { ia64_i2000_wxb1_intx,
                               ARRAY_SIZE(ia64_i2000_wxb1_intx) },
    [IA64_460GX_ROOT_GXB]  = { ia64_i2000_gxb_intx,
                               ARRAY_SIZE(ia64_i2000_gxb_intx) },
};

/* The lowest programmed expander-port PCIS moved the top of the low DRAM band. */
static void ia64_vpc_460gx_window_moved(void *opaque, uint64_t base)
{
    ia64_vpc_set_low_ram_limit(opaque, MIN(base, IA64_LOW_RAM_LIMIT));
}

/*
 * The i2000/SDV is a two-socket board whose processors answer to bus-agent
 * ids 0 and 3: the vendor firmware's MADT template declares exactly those
 * two Local SAPIC slots (ids 0 and 3, the second enabled once its processor
 * checks in), and the OS's AP wake-up IPI is addressed to that LID.  SAL_A
 * derives the LID from the geographic id PAL hands it in GR33 (at
 * 0xFFFF3608), so the id has to travel there: a second CPU announced as id 1
 * checks in, is published as id 3, and never hears the IPI.
 */
static const uint8_t sdv_processor_ids[] = { 0, 3 };

static bool sdv_validate(IA64VpcMachineState *s, Error **errp)
{
    MachineState *machine = MACHINE(s);

    /* The i2000 is a two-socket board: its firmware declares two LSAPICs. */
    if (machine->smp.cpus > ARRAY_SIZE(sdv_processor_ids)) {
        error_setg(errp, "the 460gx machine has at most 2 CPUs: the i2000/SDV "
                   "is a two-socket board");
        return false;
    }
    {
        /*
         * Firmware sizes memory from the DIMMs (SSDM 5.5.1), so RAM must be
         * a population of Memory Card A: a multiple of the 64 MB row
         * increment, at most eight rows of 4 GB.
         */
        uint64_t max = (uint64_t)IA64_460GX_MEM_ROWS * 4 * GiB;

        if (machine->ram_size % (IA64_460GX_MEM_ROW_MIN_MB * MiB) != 0 ||
            machine->ram_size > max) {
            g_autofree char *inc = size_to_str(IA64_460GX_MEM_ROW_MIN_MB * MiB);
            g_autofree char *top = size_to_str(max);

            error_setg(errp, "Invalid RAM size: the 460GX memory card takes "
                       "multiples of %s up to %s", inc, top);
            return false;
        }
    }
    return true;
}

static bool sdv_build_chipset(IA64VpcMachineState *s, DeviceState *pci_host,
                              PCIBus *pci_bus, MemoryRegion *pci_io,
                              DeviceState *iosapic, Error **errp)
{
    MachineState *machine = MACHINE(s);

    s->agp_dev = pci_new(PCI_DEVFN(PCI_SLOT_MAX - 1, 0), TYPE_IA64_AGP);
    /*
     * The AGP master is the graphics adapter on the GXB's downstream
     * root, so the GART translates that bus's device 0.
     */
    object_property_set_int(OBJECT(s->agp_dev), "agp-master-devfn",
                            PCI_DEVFN(IA64_460GX_GXB_VGA_SLOT, 0),
                            &error_abort);
    object_property_set_bool(OBJECT(s->agp_dev), "gart-enabled",
                            s->agp_enabled, &error_abort);
    if (!pci_realize_and_unref(s->agp_dev, pci_bus, errp)) {
        return false;
    }

    s->chipset = ia64_460gx_create(OBJECT(s), pci_io, pci_host,
                                   machine->ram_size,
                                   ia64_vpc_460gx_window_moved, s, errp);
    if (s->chipset == NULL) {
        return false;
    }
    ia64_460gx_attach_root(s->chipset, -1, pci_bus);

    /*
     * The i2000's other three PCI roots: the two WXB buses and the GXB AGP
     * bus.  Each is a root in its own right, sharing the primary host
     * bridge's identity-mapped windows the way the zx1 Mercury root does, and
     * each owns its own block of four PID inputs rather than sharing bus 0's
     * -- which is why the Programmable Interrupt Device has 64 of them
     * (SSDM 2.6.3).  They are created empty here; sdv_seat() puts the
     * board's devices on them.
     */
    {
        static const struct {
            const char *name;
            uint8_t bus;
            unsigned int index;
        } expanders[IA64_460GX_EXPANDER_ROOTS] = {
            /*
             * Created youngest-first.  QEMU resolves a "-device" with no
             * bus= to the most recently realized PCI bus, so this order
             * makes WXB0 the default: on a real i2000 bus 0 is the
             * compatibility bus carrying the on-board south bridge, while
             * add-in cards go in the WXB slots.  Name the bus explicitly
             * ("bus=pci", "bus=gxb", ...) to place a device elsewhere.
             */
            { "gxb",  IA64_460GX_GXB_BUS,  IA64_460GX_ROOT_GXB },
            { "wxb1", IA64_460GX_WXB1_BUS, IA64_460GX_ROOT_WXB1 },
            { "wxb0", IA64_460GX_WXB0_BUS, IA64_460GX_ROOT_WXB0 },
        };
        unsigned int root;

        for (root = 0; root < IA64_460GX_EXPANDER_ROOTS; root++) {
            unsigned int line;

            unsigned int index = expanders[root].index;

            s->expander_host[index] = ia64_expander_host_create(
                OBJECT(s), expanders[root].name,
                ia64_pci_host_mmio(pci_host), ia64_pci_host_io(pci_host),
                expanders[root].bus,
                ia64_i2000_root_intx[index].routes,
                ia64_i2000_root_intx[index].nroutes,
                IA64_460GX_INTX_FALLBACK_GSI, errp);
            if (s->expander_host[index] == NULL) {
                return false;
            }
            s->expander_bus[index] =
                ia64_expander_host_bus(s->expander_host[index]);
            ia64_460gx_attach_root(s->chipset, index, s->expander_bus[index]);
            ia64_pci_host_add_secondary_bus(pci_host, s->expander_bus[index]);

            for (line = IA64_PCI_INTX_GSI_BASE;
                 line < IA64_PCI_INTX_MAX_OUTPUTS; line++) {
                qdev_connect_gpio_out(s->expander_host[index], line,
                                      qdev_get_gpio_in(iosapic, line));
            }
        }

        /*
         * The GART translates the AGP master, which lives on the GXB's
         * downstream root, so that bus needs the same DMA routing as the
         * bus the GXB bridge itself sits on.
         */
        if (s->agp_dev != NULL) {
            ia64_agp_attach_bus(IA64_AGP(s->agp_dev),
                                s->expander_bus[IA64_460GX_ROOT_GXB]);
        }

        /*
         * Each WXB bus carries an Integrated Hot-Plug Controller for its
         * expansion slots.  Nothing implements hot plug here; the controller
         * is present because the board has one, and idle.
         */
        for (root = 0; root < IA64_460GX_EXPANDER_ROOTS; root++) {
            unsigned int index = expanders[root].index;

            if (index != IA64_460GX_ROOT_WXB0 &&
                index != IA64_460GX_ROOT_WXB1) {
                continue;
            }
            if (!pci_realize_and_unref(
                    pci_new(PCI_DEVFN(IA64_460GX_IHPC_SLOT, 0),
                            TYPE_IA64_460GX_IHPC),
                    s->expander_bus[index], errp)) {
                return false;
            }
        }
    }
    return true;
}

static void sdv_wire_intx(IA64VpcMachineState *s, DeviceState *pci_host,
                          DeviceState *iosapic)
{
    unsigned int i;

    /*
     * On the i2000 each root's outputs are numbered by PID input (the
     * board tables above), so every output goes to the input of the
     * same number; inputs 0-15 stay the ISA lines, which no table names.
     */
    for (i = IA64_PCI_INTX_GSI_BASE; i < IA64_PCI_INTX_MAX_OUTPUTS; i++) {
        qdev_connect_gpio_out(pci_host, i, qdev_get_gpio_in(iosapic, i));
    }
}

static ISABus *sdv_build_isa(IA64VpcMachineState *s, PCIBus *pci_bus,
                             MemoryRegion *pci_io, DeviceState *iosapic,
                             Error **errp)
{
    ISABus *isa_bus;
    int i;

    /*
     * The bridge comes up with its ACPI block at A00h, where the vendor
     * firmware's FADT (PM1a_EVT A00h, PM1a_CNT A04h, SMI_CMD B2h with
     * ACPI_ENABLE A0h), its DSDT and its PMI handler all expect it.  That
     * firmware's own pokes for it (00:03.0 @44h = 0, @40h = 0A00h,
     * @44h = 1) sit in a chipset-init script (bios130.BIN offset 0x2C7A80)
     * that nothing here reaches (d35d664), so the machine supplies their
     * result.
     * The project firmware makes the same pokes itself and publishes this
     * block; the board has no other PM block.
     */
    s->ifb = intel_82468gx_ifb_create(
        pci_bus, PCI_DEVFN(IA64_460GX_IFB_SLOT,
                           IA64_460GX_IFB_LPC_FUNCTION),
        IA64_460GX_ACPI_PM_IO_BASE, errp);
    if (s->ifb == NULL) {
        return NULL;
    }
    /*
     * The bridge leaves the SCI on IRQ9 (SCIRC 45h reset value, SSDM
     * 11.1.10), so it arrives over the ISA interrupt wires above.  This
     * output carries only the encodings that name no ISA interrupt, and the
     * board takes those to input 49 -- the global interrupt the vendor MADT
     * template's source override names for the part's own SCI pin.
     */
    qdev_connect_gpio_out_named(DEVICE(s->ifb), INTEL_82468GX_IFB_GPIO_SCI,
                                0, qdev_get_gpio_in(iosapic,
                                                    IA64_460GX_SCI_GSI));
    /*
     * The APM control port's SMI is the processor's PMI on this platform,
     * and the vendor SAL's PMI handler answers the FADT's ACPI_ENABLE and
     * ACPI_DISABLE commands by setting or clearing SCI_EN.  PMI delivery is
     * not modelled; this stands in for that handler's effect
     * (intel_82468gx_ifb_acpi_sci_enable).
     */
    qdev_connect_gpio_out_named(DEVICE(s->ifb), INTEL_82468GX_IFB_GPIO_APMC,
                                0, qemu_allocate_irq(ia64_vpc_realfw_apmc,
                                                     s, 0));
    for (i = 0; i < INTEL_82468GX_IFB_FUNCTIONS; i++) {
        PCIDevice *fn = intel_82468gx_ifb_function(s->ifb, i);

        if (fn == NULL) {
            error_setg(errp, "%s did not create function %d",
                       TYPE_INTEL_82468GX_IFB, i);
            return NULL;
        }
        /*
         * A chipset part carries no subsystem identity, so leave those
         * registers at zero rather than at the PCI bus default.
         */
        pci_set_word(fn->config + PCI_SUBSYSTEM_VENDOR_ID, 0);
        pci_set_word(fn->config + PCI_SUBSYSTEM_ID, 0);
    }
    {
        /*
         * The board's hardware monitors, which the vendor firmware
         * initialises over the bridge's SMBus during POST.  They belong
         * to the I/O board rather than to the chipset, so the machine
         * puts them on the bus the bridge provides.
         */
        I2CBus *smbus = intel_82468gx_ifb_smbus(s->ifb);
        static const uint8_t hwmon_addrs[] = {
            IA64_I2000_HWMON_ADDR_0, IA64_I2000_HWMON_ADDR_1,
        };

        for (i = 0; i < ARRAY_SIZE(hwmon_addrs); i++) {
            i2c_slave_create_simple(smbus, TYPE_IA64_I2000_HWMON,
                                    hwmon_addrs[i]);
        }
    }
    isa_bus = intel_82468gx_ifb_isa_bus(s->ifb);
    for (i = 0; i < ISA_NUM_IRQS; i++) {
        s->isa_irqs[i] = qdev_get_gpio_in(iosapic, i);
        qdev_connect_gpio_out_named(DEVICE(s->ifb),
                                    INTEL_82468GX_IFB_GPIO_ISA_IRQ, i,
                                    s->isa_irqs[i]);
    }
    /*
     * The bridge's 8259 pair drives INTR, which a processor takes as an
     * ExtINT.  Without this the pair answers its ports but can deliver
     * nothing, and firmware that runs the legacy tick through the PIC --
     * as the vendor firmware does during POST -- never sees an interrupt.
     */
    s->extint = qemu_allocate_irq(ia64_vpc_extint, s, 0);
    qdev_connect_gpio_out_named(DEVICE(s->ifb),
                                INTEL_82468GX_IFB_GPIO_LEGACY, 0,
                                s->extint);
    /*
     * The board's Super I/O behind the bridge, as far as its
     * configuration space: the vendor DSDT finds COM1 and the keyboard
     * controller through it (see hw/isa/smsc_lpc47b27x.c).  Its UART2 is
     * fitted when the machine has a debug port, which the base machine
     * then decodes at COM2.
     */
    {
        ISADevice *sio = isa_new(TYPE_SMSC_LPC47B27X);

        qdev_prop_set_bit(DEVICE(sio), SMSC_LPC47B27X_PROP_UART2,
                          s->debug_uart != NULL);
        if (!isa_realize_and_unref(sio, isa_bus, errp)) {
            return NULL;
        }
    }
    return isa_bus;
}

static void sdv_seat(IA64VpcMachineState *s, IA64VpcSeat seat, PCIBus **bus,
                     int *devfn)
{
    switch (seat) {
    case IA64_VPC_SEAT_SCSI:
        /* The i2000's SCSI host bus adapter sits at 01:00.0, on WXB0. */
        *bus = s->expander_bus[IA64_460GX_ROOT_WXB0];
        *devfn = PCI_DEVFN(IA64_460GX_WXB0_SCSI_SLOT, 0);
        break;
    case IA64_VPC_SEAT_SCSI_PARK:
        /* The second adapter parks on the second WXB bus. */
        *bus = s->expander_bus[IA64_460GX_ROOT_WXB1];
        *devfn = PCI_DEVFN(IA64_460GX_WXB1_SCSI_SLOT, 0);
        break;
    case IA64_VPC_SEAT_VGA:
        /* The i2000 puts its AGP Pro graphics at 03:00.0, behind the GXB. */
        *bus = s->expander_bus[IA64_460GX_ROOT_GXB];
        *devfn = PCI_DEVFN(IA64_460GX_GXB_VGA_SLOT, 0);
        break;
    case IA64_VPC_SEAT_AUDIO:
        /* Device 4 of the compatibility bus is the i2000's audio seat. */
        *devfn = PCI_DEVFN(IA64_460GX_AUDIO_SLOT, 0);
        break;
    case IA64_VPC_SEAT_NIC:
        *devfn = PCI_DEVFN(IA64_460GX_NIC_SLOT, 0);
        break;
    }
}

/* Each 460gx root has its own interrupt block: its bus maps the pin. */
static unsigned int sdv_root_gsi_base(const IA64VpcMachineState *s,
                                      uint8_t bus)
{
    return IA64_460GX_INTX_FALLBACK_GSI;
}

/* Concrete: Intel SDV / HP i2000 -- 460GX chipset, Merced. */
static void sdv_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    IA64VpcMachineClass *imc = IA64_VPC_MACHINE_CLASS(oc);

    (void)data;
    mc->desc = "Intel SDV / HP i2000 (460GX chipset, Merced)";
    mc->default_cpu_type = IA64_CPU_TYPE_NAME("merced");
    /*
     * On the i2000 the interrupt controller is the 460GX Programmable
     * Interrupt Device: 64 inputs reporting IOSAPIC version 2.1.  Its
     * width is what lets each PCI root own its own block of four INTx
     * lines (16, 20, 24, 28) rather than sharing one block.
     */
    imc->iosapic_pins = IA64_IOSAPIC_460GX_PINS;
    imc->iosapic_version = IA64_IOSAPIC_460GX_VERSION;
    imc->pci0_intx = ia64_i2000_pci0_intx;
    imc->pci0_nintx = ARRAY_SIZE(ia64_i2000_pci0_intx);
    imc->pci0_intx_fallback = IA64_460GX_INTX_FALLBACK_GSI;
    imc->has_south_bridge = true;
    imc->pci_config_ecam = false;
    /*
     * The i2000 and the other 460GX workstations carry a Super-I/O PS/2
     * controller, and PS/2 was the input of choice on them, so 460gx keeps
     * it.  Either default can be overridden with i8042=on|off.
     */
    imc->i8042_default = true;
    imc->legacy_com1_console = true;
    /*
     * imc->sale_recovery_check stays false: PALE_RESET makes the RESET call
     * only on this board.  The vendor firmware's recovery-check pass sizes
     * and initializes the DRAM, hands the result to the next pass through
     * the SAC (see ia64_460gx_reset), resets the platform, and the pass
     * after the reset stalls in a software delay loop of its RAM-resident
     * recovery module (PspRecover, loop at RAM 0x02011C10) -- 30 minutes
     * with no further progress and no boot manager (0edbeda).
     */
    imc->processor_ids = sdv_processor_ids;
    imc->nprocessor_ids = ARRAY_SIZE(sdv_processor_ids);
    imc->validate = sdv_validate;
    imc->build_chipset = sdv_build_chipset;
    imc->wire_intx = sdv_wire_intx;
    imc->build_isa = sdv_build_isa;
    imc->seat = sdv_seat;
    imc->root_gsi_base = sdv_root_gsi_base;
    ia64_vpc_add_compat_defaults(mc);
}

static const TypeInfo sdv_machine_typeinfo = {
    .name = TYPE_IA64_460GX_MACHINE,
    .parent = TYPE_IA64_VPC_MACHINE,
    .class_init = sdv_machine_class_init,
};

static void sdv_register_types(void)
{
    type_register_static(&sdv_machine_typeinfo);
}

type_init(sdv_register_types)
