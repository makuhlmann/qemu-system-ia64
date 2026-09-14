/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * IA-64 machine internals shared between the abstract base machine
 * (ia64_base.c) and the concrete boards (sdv.c, longspeak.c).  Nothing
 * outside hw/ia64/ includes this.
 */

#ifndef HW_IA64_VPC_INTERNAL_H
#define HW_IA64_VPC_INTERNAL_H

#include CONFIG_DEVICES

#include "hw/core/boards.h"
#include "hw/block/flash.h"
#include "hw/char/serial-mm.h"
#include "hw/acpi/acpi.h"
#include "hw/isa/isa.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_bus.h"
#include "hw/southbridge/intel_82468gx.h"
#include "hw/ia64/ia64_pci.h"
#include "hw/ia64/ia64_460gx.h"
#include "hw/ia64/ia64_vpc_abi.h"
#include "net/net.h"
#include "qemu/notify.h"
#include "qemu/timer.h"

/*
 * Low (sub-aperture) DRAM runs contiguously from 0 up to the PCI/MMIO
 * aperture, exactly as the real 460GX keeps a single MMIO gap at the top of
 * the 32-bit space; RAM displaced by that gap is remapped above 4 GiB.  There
 * is no DRAM island between the aperture and the chipset/SAPIC region.
 */
#define IA64_LOW_RAM_LIMIT IA64_PCI_MMIO_BASE

/*
 * The spare Programmable Interrupt Device inputs a 460gx root swizzles an
 * unlisted slot into; also the marker ia64_vpc_configure_pci_irq_on_root
 * takes for "a 460gx root: ask the bus".
 */
#define IA64_460GX_INTX_FALLBACK_GSI 60

#ifdef CONFIG_IA64_VPC_GRAPHICS
enum {
    IA64_INT10_REG_AX,
    IA64_INT10_REG_BX,
    IA64_INT10_REG_CX,
    IA64_INT10_REG_DX,
    IA64_INT10_REG_DI,
    IA64_INT10_REG_ES,
    IA64_INT10_REG_EXEC,
    IA64_INT10_REG_DATA,
};

typedef struct IA64Int10Registers {
    uint16_t ax;
    uint16_t bx;
    uint16_t cx;
    uint16_t dx;
    uint16_t di;
    uint16_t es;
} IA64Int10Registers;
#endif

/*
 * The IA-64 machine is modeled as an abstract base ("ia64-base") carrying all
 * the shared platform (PCI host, IOSAPIC, firmware, devices), with two concrete
 * boards built on it: "460gx" (the Intel SDV, sold by HP as the i2000: Merced
 * + 460GX, sdv.c) and "zx1" (HP Longs Peak, the zx2000 / zx6000 / rx2600
 * system board: Itanium 2 + zx1, longspeak.c).  Each board fixes its default
 * CPU and its chipset personality, and fills in the hooks below; "ia64-vpc"
 * survives as a deprecated alias of zx1.
 */
#define TYPE_IA64_VPC_MACHINE MACHINE_TYPE_NAME("ia64-base")
OBJECT_DECLARE_TYPE(IA64VpcMachineState, IA64VpcMachineClass, IA64_VPC_MACHINE)

#define TYPE_IA64_460GX_MACHINE MACHINE_TYPE_NAME("460gx")
#define TYPE_IA64_ZX1_MACHINE   MACHINE_TYPE_NAME("zx1")

/* Built-in devices the board seats: see IA64VpcMachineClass.seat. */
typedef enum IA64VpcSeat {
    IA64_VPC_SEAT_SCSI,        /* the SCSI host bus adapter */
    IA64_VPC_SEAT_SCSI_PARK,   /* the second adapter, when both are present */
    IA64_VPC_SEAT_VGA,         /* the graphics adapter (the AGP master) */
    IA64_VPC_SEAT_AUDIO,       /* the CS4281; devfn -1 = anywhere */
    IA64_VPC_SEAT_NIC,         /* the first network adapter's slot */
} IA64VpcSeat;

struct IA64VpcMachineClass {
    MachineClass parent_class;
    /* IOSAPIC inputs and version register; 0 keeps the device's defaults. */
    uint32_t iosapic_pins;
    uint32_t iosapic_version;
    /* Per-slot INTx routing of PCI bus 0; NULL = the (slot+pin)%4 swizzle. */
    const IA64IntxRoute *pci0_intx;
    unsigned int pci0_nintx;
    /* The board carries the 82468GX south bridge (and its IDE function). */
    bool has_south_bridge;
    /* Default of the i8042 option. */
    bool i8042_default;
    /* Alias the console UART at legacy COM1 (3F8h) when no debug port is. */
    bool legacy_com1_console;

    /* Board-specific configuration checks; NULL = none. */
    bool (*validate)(IA64VpcMachineState *s, Error **errp);
    /*
     * Map the low DRAM band from backing offset @offset, @remaining bytes
     * left; returns the bytes mapped.  NULL = one contiguous run up to
     * low_ram_limit (ia64_vpc_map_ram_alias).
     */
    uint64_t (*map_low_ram)(IA64VpcMachineState *s, uint64_t offset,
                            uint64_t remaining);
    /*
     * Create the core chipset: its DMA-translation device (before any other
     * PCI device) and its further PCI roots.
     */
    bool (*build_chipset)(IA64VpcMachineState *s, DeviceState *pci_host,
                          PCIBus *pci_bus, MemoryRegion *pci_io,
                          DeviceState *iosapic, Error **errp);
    /* Connect the PCI host bridges' INTx outputs to the IOSAPIC inputs. */
    void (*wire_intx)(IA64VpcMachineState *s, DeviceState *pci_host,
                      DeviceState *iosapic);
    /*
     * Create the south bridge (if any), the ISA bus with its interrupt
     * inputs wired to the IOSAPIC, and the RTC.  Returns the ISA bus.
     */
    ISABus *(*build_isa)(IA64VpcMachineState *s, PCIBus *pci_bus,
                         MemoryRegion *pci_io, DeviceState *iosapic,
                         Error **errp);
    /* Where a built-in device sits; *bus preset to PCI0, *devfn to -1. */
    void (*seat)(IA64VpcMachineState *s, IA64VpcSeat seat, PCIBus **bus,
                 int *devfn);
    /* The interrupt block owned by the root that carries bus @bus. */
    unsigned int (*root_gsi_base)(const IA64VpcMachineState *s, uint8_t bus);
};

struct IA64VpcMachineState {
    MachineState parent_obj;

    bool i8042_enabled;
    bool ahci_enabled;
    bool audio_enabled;
    bool isp_enabled;
    bool lsi_enabled;
    /*
     * Where the CPU's firmware identity window sits: the RAM-top shadow
     * (on) or the historical 1 MB home (off).  Off is the microprogram
     * battery's lever: it loads code at 1 MB with no firmware present.
     */
    bool fw_relocate;
    uint64_t fw_map_quirk_disable;
    bool ide_enabled;
    bool firmware_ide_dma;
    bool agp_enabled;
    uint64_t firmware_console;
    uint16_t firmware_boot_timeout;
    char *nvram_path;
    char *realfw_vga_rom_path;
    char *realfw_nvram_path;
    uint64_t realfw_entry;
    uint64_t realfw_base;
    PFlashCFI01 *realfw_flash;
    /* The firmware file, read before the platform is built. */
    uint8_t *fw_image;
    size_t fw_image_size;
    char *fw_image_name;
    /* A flash image: reset pointer block, FIT, mapped to end at 4 GiB. */
    bool fw_is_flash;
    /* The flash image's FIT declares an NVRAM block (type 1Eh). */
    bool fw_flash_has_nvram;
    uint64_t fw_fit_ptr;
    uint64_t fw_sale_ptr;
    qemu_irq extint;
    /* 460gx: the chipset (SAC, CF8/CFC, config store, SPD rows). */
    IA64460GXState *chipset;
    PCIBus *host_pci_bus;
    char *vga_model;
    bool alat_full;

    PCIDevice *agp_dev;
    PCIDevice *sba_dev;
    DeviceState *lba_dev;
    DeviceState *mercury_host;      /* zx1: the Mercury (LBA) PCI host bridge */
    /* 460gx: the WXB0, WXB1 and GXB expander roots (buses 1, 2 and 3). */
    DeviceState *expander_host[IA64_460GX_EXPANDER_ROOTS];
    PCIBus *expander_bus[IA64_460GX_EXPANDER_ROOTS];
    PCIBus *mercury_bus;            /* zx1: the Mercury second root bus         */
    PCIDevice *ahci_dev;
    PCIDevice *audio_dev;
    PCIDevice *isp_dev;
    PCIDevice *ide_dev;
    PCIDevice *ohci_dev;
    PCIDevice *uhci_dev;
    Intel82468GXIFBState *ifb;
    PCIDevice *lsi_dev;
    PCIDevice *vga_dev;
    PCIDevice *nic_devs[MAX_NICS];
    unsigned int nic_count;

    MemoryRegion ram_aliases[4];
    unsigned int ram_alias_count;
    /* Where the low DRAM band ends: the chipset's PCI gap base. */
    uint64_t low_ram_limit;
    MemoryRegion *vga_fb_alias;
    MemoryRegion *vga_mmio_alias;
    MemoryRegion *vga_legacy_alias;
    MemoryRegion *lsapic_mmio;
    MemoryRegion firmware_space;
    MemoryRegion watchdog_mmio;
    MemoryRegion nvram_mmio;
    MemoryRegion acpi_pm;
    MemoryRegion acpi_reset;
    MemoryRegion debug_uart_legacy_io;
    SerialMM *debug_uart;
    MemoryRegion console_uart_legacy_io;
    SerialMM *console_uart;
    DeviceState *pci_host_dev;
#ifdef CONFIG_IA64_VPC_GRAPHICS
    MemoryRegion int10_pci_io;
    IA64Int10Registers int10_request;
    IA64Int10Registers int10_result;
    uint32_t int10_input_signature;
    uint8_t int10_response[512];
    uint16_t int10_response_length;
    uint16_t int10_response_offset;
    uint8_t int10_input_signature_words;
    uint8_t int10_dpms_state;
    uint8_t int10_legacy_mode;
    uint8_t int10_legacy_columns;
#endif

    Object *pci_fixup_reset;
    QEMUTimer *watchdog_timer;
    uint64_t watchdog_timeout;
    uint64_t watchdog_code;
    uint8_t nvram_data[IA64_NVRAM_SIZE];
    char *nvram_resolved_path;
    bool nvram_write_warning;
    ACPIREGS acpi_regs;
    qemu_irq acpi_sci_irq;
    qemu_irq isa_irqs[ISA_NUM_IRQS];
    Notifier powerdown_notifier;
    Notifier done_notifier;
    bool vmstate_registered;
};

/* Base-machine helpers the boards use. */
void ia64_vpc_add_compat_defaults(MachineClass *mc);
uint64_t ia64_vpc_map_ram_alias(IA64VpcMachineState *s, hwaddr guest_base,
                                uint64_t backing_offset, uint64_t remaining,
                                uint64_t capacity, const char *name);
void ia64_vpc_set_low_ram_limit(IA64VpcMachineState *s, uint64_t limit);

#endif /* HW_IA64_VPC_INTERNAL_H */
