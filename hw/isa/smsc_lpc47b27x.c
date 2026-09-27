/*
 * SMSC LPC47B27x Super I/O: the configuration space and what it decodes.
 *
 * The HP i2000 / Intel 460GX SDV carries an SMSC LPC47B27x on the LPC bus
 * behind the 82468GX.  The board wires one serial port (UART1) and the
 * keyboard controller (i2000 Owner's Guide, "Super I/O").  The machine
 * builds those as ordinary devices and attaches them here; this model is
 * the chip's configuration space (datasheet DS00002492A §20) and the
 * decoding it controls: a logical device answers at its base only while it
 * is activated, and its interrupt reaches the ISA IRQ its Interrupt Select
 * register names.  The vendor DSDT's \_SB.PCI0.LPC reads and writes this
 * space through an IndexField at 2Eh/2Fh (UAR1 _STA/_CRS/_SRS/_DIS).
 *
 * Reset is the chip's own (Table 20-1): every logical device inactive,
 * configuration port at 2Eh (SYSOPT pulled down).  Firmware programs the
 * chip.  The vendor i2000 firmware does it in its chipset-init script
 * (bios130.BIN 0x2c85a0, 0x2c8bd0), which the emulated boot never reaches
 * (the same script programs the IFB's ACPI base, which the IFB model takes
 * from its "init-acpi-base" property); the board supplies the script's
 * result through the "firmware-init" property, as the IFB does.
 *
 * Not modelled: the 12-bit address decode (and its aliases) before CR24
 * bit 6 is set, the floppy, parallel, UART2, game and MPU-401 functions
 * (no device answers when they are activated), the runtime registers'
 * behaviour, and the 82468GX LPC Enables gating of 2Eh/2Fh (SSDM 11.1.27).
 */

#include "qemu/osdep.h"
#include "hw/isa/isa.h"
#include "hw/isa/smsc_lpc47b27x.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "hw/input/i8042.h"
#include "migration/vmstate.h"
#include "qom/object.h"
#include "trace.h"

#define SIO_CONFIG_PORT         0x2e
#define SIO_CONFIG_PORT_SIZE    2
#define SIO_ENTER_KEY           0x55
#define SIO_EXIT_KEY            0xaa

#define SIO_REG_CONFIG_CONTROL  0x02
#define SIO_CONFIG_SOFT_RESET   0x01
#define SIO_REG_LDN             0x07
#define SIO_REG_DEVID           0x20
#define SIO_REG_DEVREV          0x21
#define SIO_REG_POWER           0x22
#define SIO_REG_POWER_MGMT      0x23
#define SIO_REG_OSC             0x24
#define SIO_REG_CFGPORT_LO      0x26
#define SIO_REG_CFGPORT_HI      0x27
#define SIO_REG_TEST_FIRST      0x2a
#define SIO_REG_TEST_LAST       0x2f
#define SIO_GLOBAL_REGS         0x30

#define SIO_DEVID_LPC47B27X     0x51
#define SIO_DEVREV              0x01
/* CR22 bit 1 is reserved; bit 7 (PME trickle clock) has no logical device. */
#define SIO_POWER_PME           0x80
#define SIO_POWER_MGMT_MASK     0x79
#define SIO_OSC_RESET           0x04

#define SIO_LDN_ACTIVATE        0x30
#define SIO_LDN_BASE_HI         0x60
#define SIO_LDN_BASE_LO         0x61
#define SIO_LDN_BASE2_HI        0x62
#define SIO_LDN_BASE2_LO        0x63
#define SIO_LDN_IRQ             0x70
#define SIO_LDN_IRQ2            0x72
#define SIO_LDN_DMA             0x74
#define SIO_LDN_REGS            0x100
#define SIO_LDN_COUNT           0x0c

#define LDN_FDC   0x0
#define LDN_LPT   0x3
#define LDN_UART1 0x4
#define LDN_UART2 0x5
#define LDN_KBD   0x7
#define LDN_GAME  0x9
#define LDN_PME   0xa
#define LDN_MPU   0xb

/* Table 20-4: UART bases on 8-byte boundaries in [100h, FF8h]. */
#define SIO_UART_BASE_MASK      0xfff8
#define SIO_UART_BASE_MIN       0x100
#define SIO_UART_BASE_MAX       0xff8
/* Table 20-4: the runtime block on 128-byte boundaries in [0, F7Fh]. */
#define SIO_RUNTIME_BASE_MASK   0xff80
#define SIO_RUNTIME_BASE_MAX    0xf00
#define SIO_RUNTIME_SIZE        0x80
#define SIO_CFGPORT_MASK        0xfffe
#define SIO_IRQ_SELECT_MASK     0x0f

typedef enum {
    SIO_SOURCE_UART1,
    SIO_SOURCE_KBD,
    SIO_SOURCE_MOUSE,
    SIO_SOURCES,
} SIOSource;

#define SIO_GPIO_SOURCE "source-irq"

struct SMSCLPC47B27xState {
    ISADevice parent_obj;

    MemoryRegion config_io;
    MemoryRegion runtime_io;
    bool config_mode;
    uint8_t index;
    uint8_t global[SIO_GLOBAL_REGS];
    uint8_t ldn[SIO_LDN_COUNT][SIO_LDN_REGS];
    uint8_t runtime[SIO_RUNTIME_SIZE];
    bool source_level[SIO_SOURCES];
    /* The ISA IRQ levels this chip drives; lines it never raised stay alone. */
    bool isa_level[ISA_NUM_IRQS];

    /* Board wiring. */
    MemoryRegion *uart1_io;
    MemoryRegion *kbc_io[2];
    bool firmware_init;
};

/*
 * The vendor chipset-init script's writes to this chip, in its order
 * (bios130.BIN 0x2c85a0 and 0x2c8bd0; 55h before, AAh after each block).
 * The keyboard's mouse IRQ (72h) is the IA-32 BIOS's, at POST C4h.
 */
static const struct {
    uint8_t ldn;    /* SIO_GLOBAL_WRITE: a global register */
    uint8_t reg;
    uint8_t value;
} sio_firmware_init[] = {
#define SIO_GLOBAL_WRITE 0xff
    { SIO_GLOBAL_WRITE, 0x24, 0x44 },
    { 0xa, 0x30, 0x00 }, { 0xa, 0x60, 0x08 }, { 0xa, 0x61, 0x00 },
    { 0xa, 0x30, 0x01 },
    { 0x0, 0x60, 0x03 }, { 0x0, 0x61, 0xf0 }, { 0x0, 0x70, 0x06 },
    { 0x0, 0x74, 0x02 }, { 0x0, 0xf0, 0x0e }, { 0x0, 0xf1, 0x00 },
    { 0x0, 0xf2, 0xff }, { 0x0, 0xf4, 0x00 }, { 0x0, 0xf5, 0x00 },
    { 0x0, 0x30, 0x00 },
    { 0x3, 0x60, 0x02 }, { 0x3, 0x61, 0x78 }, { 0x3, 0x70, 0x07 },
    { 0x3, 0x74, 0x04 }, { 0x3, 0xf0, 0x3c }, { 0x3, 0xf1, 0x00 },
    { 0x3, 0x30, 0x00 },
    { 0x4, 0x60, 0x03 }, { 0x4, 0x61, 0xf8 }, { 0x4, 0x70, 0x04 },
    { 0x4, 0xf0, 0x02 }, { 0x4, 0x30, 0x01 },
    { 0x5, 0x60, 0x02 }, { 0x5, 0x61, 0xf8 }, { 0x5, 0x62, 0x00 },
    { 0x5, 0x63, 0x00 }, { 0x5, 0x70, 0x03 }, { 0x5, 0x74, 0x04 },
    { 0x5, 0xf0, 0x02 }, { 0x5, 0xf1, 0x02 }, { 0x5, 0xf2, 0x03 },
    { 0x5, 0x30, 0x00 },
    { 0x7, 0x70, 0x01 }, { 0x7, 0xf0, 0x18 }, { 0x7, 0x30, 0x01 },
    { 0x9, 0x30, 0x00 },
    { 0xb, 0x30, 0x00 },
};

static const struct {
    uint8_t ldn;
    uint8_t reg;
} sio_source_select[SIO_SOURCES] = {
    [SIO_SOURCE_UART1] = { LDN_UART1, SIO_LDN_IRQ },
    [SIO_SOURCE_KBD] = { LDN_KBD, SIO_LDN_IRQ },
    [SIO_SOURCE_MOUSE] = { LDN_KBD, SIO_LDN_IRQ2 },
};

/* CR22 bit linked to each logical device's Activate bit (Table 20-3 Note 1). */
static int sio_power_bit(unsigned ldn)
{
    switch (ldn) {
    case LDN_FDC:
        return 0;
    case LDN_GAME:
        return 2;
    case LDN_LPT:
        return 3;
    case LDN_UART1:
        return 4;
    case LDN_UART2:
        return 5;
    case LDN_MPU:
        return 6;
    }
    return -1;
}

/* Implemented bits of a logical-device register (Tables 20-1, 20-3). */
static uint8_t sio_ldn_mask(unsigned ldn, unsigned reg)
{
    switch (reg) {
    case SIO_LDN_ACTIVATE:
        switch (ldn) {
        case LDN_FDC: case LDN_LPT: case LDN_UART1: case LDN_UART2:
        case LDN_KBD: case LDN_GAME: case LDN_PME: case LDN_MPU:
            return 0x01;
        }
        return 0;
    case SIO_LDN_BASE_HI:
    case SIO_LDN_BASE_LO:
        switch (ldn) {
        case LDN_FDC: case LDN_LPT: case LDN_UART1: case LDN_UART2:
        case LDN_GAME: case LDN_PME: case LDN_MPU:
            return 0xff;
        }
        return 0;
    case SIO_LDN_BASE2_HI:
    case SIO_LDN_BASE2_LO:
        return ldn == LDN_UART2 ? 0xff : 0;
    case SIO_LDN_IRQ:
        switch (ldn) {
        case LDN_FDC: case LDN_LPT: case LDN_UART1: case LDN_UART2:
        case LDN_KBD: case LDN_MPU:
            return SIO_IRQ_SELECT_MASK;
        }
        return 0;
    case SIO_LDN_IRQ2:
        return ldn == LDN_KBD ? SIO_IRQ_SELECT_MASK : 0;
    case SIO_LDN_DMA:
        switch (ldn) {
        case LDN_FDC: case LDN_LPT: case LDN_UART2:
            return 0x07;
        }
        return 0;
    case 0xf0:
        switch (ldn) {
        case LDN_FDC: case LDN_LPT: case LDN_UART1: case LDN_UART2:
        case LDN_KBD: case LDN_PME:
            return 0xff;
        }
        return 0;
    case 0xf1:
        switch (ldn) {
        case LDN_FDC: case LDN_LPT: case LDN_UART2: case LDN_PME:
            return 0xff;
        }
        return 0;
    case 0xf2:
        return ldn == LDN_FDC || ldn == LDN_UART2 ? 0xff : 0;
    case 0xf4:
    case 0xf5:
        return ldn == LDN_FDC ? 0xff : 0;
    }
    return 0;
}

static bool sio_active(SMSCLPC47B27xState *s, unsigned ldn)
{
    return s->ldn[ldn][SIO_LDN_ACTIVATE] & 1;
}

static uint16_t sio_base(SMSCLPC47B27xState *s, unsigned ldn)
{
    return (s->ldn[ldn][SIO_LDN_BASE_HI] << 8) | s->ldn[ldn][SIO_LDN_BASE_LO];
}

static void sio_update_irqs(SMSCLPC47B27xState *s)
{
    bool level[ISA_NUM_IRQS] = { false };
    unsigned i;

    for (i = 0; i < SIO_SOURCES; i++) {
        unsigned ldn = sio_source_select[i].ldn;
        unsigned irq = s->ldn[ldn][sio_source_select[i].reg];

        if (sio_active(s, ldn) && irq != 0 && s->source_level[i]) {
            level[irq] = true;
        }
    }
    for (i = 0; i < ISA_NUM_IRQS; i++) {
        if (level[i] != s->isa_level[i]) {
            s->isa_level[i] = level[i];
            qemu_set_irq(isa_get_irq(ISA_DEVICE(s), i), level[i]);
        }
    }
}

static void sio_update_decode(SMSCLPC47B27xState *s)
{
    uint16_t base;
    unsigned i;

    base = sio_base(s, LDN_PME) & SIO_RUNTIME_BASE_MASK;
    memory_region_set_address(&s->runtime_io, base);
    memory_region_set_enabled(&s->runtime_io,
                              sio_active(s, LDN_PME) &&
                              base <= SIO_RUNTIME_BASE_MAX);
    if (s->uart1_io) {
        base = sio_base(s, LDN_UART1) & SIO_UART_BASE_MASK;
        memory_region_set_address(s->uart1_io, base);
        memory_region_set_enabled(s->uart1_io,
                                  sio_active(s, LDN_UART1) &&
                                  base >= SIO_UART_BASE_MIN &&
                                  base <= SIO_UART_BASE_MAX);
    }
    for (i = 0; i < ARRAY_SIZE(s->kbc_io); i++) {
        if (s->kbc_io[i]) {
            memory_region_set_enabled(s->kbc_io[i], sio_active(s, LDN_KBD));
        }
    }
    sio_update_irqs(s);
}

static void sio_update_config_port(SMSCLPC47B27xState *s)
{
    uint16_t port = ((s->global[SIO_REG_CFGPORT_HI] << 8) |
                     s->global[SIO_REG_CFGPORT_LO]) & SIO_CFGPORT_MASK;

    memory_region_set_address(&s->config_io, port);
}

static void sio_set_activate(SMSCLPC47B27xState *s, unsigned ldn, bool on)
{
    s->ldn[ldn][SIO_LDN_ACTIVATE] = on;
}

/*
 * The logical-device registers every reset clears (the Soft Reset column of
 * Table 20-1); the F0h-F5h option registers keep their values.
 */
static void sio_reset_ldn_common(SMSCLPC47B27xState *s)
{
    unsigned ldn;

    for (ldn = 0; ldn < SIO_LDN_COUNT; ldn++) {
        if (ldn != LDN_UART2) {
            sio_set_activate(s, ldn, false);
        }
        s->ldn[ldn][SIO_LDN_BASE_HI] = 0;
        s->ldn[ldn][SIO_LDN_BASE_LO] = 0;
        s->ldn[ldn][SIO_LDN_BASE2_HI] = 0;
        s->ldn[ldn][SIO_LDN_BASE2_LO] = 0;
        s->ldn[ldn][SIO_LDN_IRQ] = 0;
        s->ldn[ldn][SIO_LDN_IRQ2] = 0;
        s->ldn[ldn][SIO_LDN_DMA] = 0;
    }
    s->ldn[LDN_FDC][SIO_LDN_BASE_HI] = 0x03;
    s->ldn[LDN_FDC][SIO_LDN_BASE_LO] = 0xf0;
    s->ldn[LDN_FDC][SIO_LDN_IRQ] = 0x06;
    s->ldn[LDN_FDC][SIO_LDN_DMA] = 0x02;
    s->ldn[LDN_LPT][SIO_LDN_DMA] = 0x04;
    s->ldn[LDN_UART2][SIO_LDN_DMA] = 0x04;
    s->ldn[LDN_MPU][SIO_LDN_BASE_HI] = 0x03;
    s->ldn[LDN_MPU][SIO_LDN_BASE_LO] = 0x30;
    s->ldn[LDN_MPU][SIO_LDN_IRQ] = 0x05;
    s->global[SIO_REG_LDN] = 0;
}

static void sio_soft_reset(SMSCLPC47B27xState *s)
{
    trace_smsc_lpc47b27x_soft_reset();
    sio_reset_ldn_common(s);
    sio_update_decode(s);
}

static uint8_t sio_global_read(SMSCLPC47B27xState *s, unsigned reg)
{
    uint8_t power;
    unsigned ldn;
    int bit;

    switch (reg) {
    case SIO_REG_LDN:
    case SIO_REG_DEVID:
    case SIO_REG_DEVREV:
    case SIO_REG_POWER_MGMT:
    case SIO_REG_OSC:
    case SIO_REG_CFGPORT_LO:
    case SIO_REG_CFGPORT_HI:
        return s->global[reg];
    case SIO_REG_POWER:
        power = s->global[SIO_REG_POWER] & SIO_POWER_PME;
        for (ldn = 0; ldn < SIO_LDN_COUNT; ldn++) {
            bit = sio_power_bit(ldn);
            if (bit >= 0 && sio_active(s, ldn)) {
                power |= 1 << bit;
            }
        }
        return power;
    }
    if (reg >= SIO_REG_TEST_FIRST && reg <= SIO_REG_TEST_LAST) {
        return s->global[reg];
    }
    return 0;
}

static void sio_global_write(SMSCLPC47B27xState *s, unsigned reg, uint8_t val)
{
    unsigned ldn;
    int bit;

    switch (reg) {
    case SIO_REG_CONFIG_CONTROL:
        if (val & SIO_CONFIG_SOFT_RESET) {
            sio_soft_reset(s);
        }
        return;
    case SIO_REG_LDN:
    case SIO_REG_OSC:
    case SIO_REG_CFGPORT_LO:
        s->global[reg] = val;
        return;
    case SIO_REG_POWER_MGMT:
        s->global[reg] = val & SIO_POWER_MGMT_MASK;
        return;
    case SIO_REG_CFGPORT_HI:
        /* Byte 0 first; the port moves when byte 1 is written. */
        s->global[reg] = val;
        sio_update_config_port(s);
        return;
    case SIO_REG_POWER:
        s->global[reg] = val & SIO_POWER_PME;
        for (ldn = 0; ldn < SIO_LDN_COUNT; ldn++) {
            bit = sio_power_bit(ldn);
            if (bit >= 0) {
                sio_set_activate(s, ldn, val & (1 << bit));
            }
        }
        sio_update_decode(s);
        return;
    }
    if (reg >= SIO_REG_TEST_FIRST && reg <= SIO_REG_TEST_LAST) {
        s->global[reg] = val;
    }
}

static uint64_t sio_config_read(void *opaque, hwaddr addr, unsigned size)
{
    SMSCLPC47B27xState *s = opaque;
    uint8_t ldn = s->global[SIO_REG_LDN];
    uint8_t val;

    /* §20.1: index and data decode only in the Configuration State. */
    if (!s->config_mode) {
        return 0xff;
    }
    if (addr == 0) {
        return s->index;
    }
    if (s->index < SIO_GLOBAL_REGS) {
        val = sio_global_read(s, s->index);
    } else if (ldn < SIO_LDN_COUNT) {
        val = s->ldn[ldn][s->index] & sio_ldn_mask(ldn, s->index);
    } else {
        val = 0;
    }
    return val;
}

static void sio_config_write(void *opaque, hwaddr addr, uint64_t val,
                             unsigned size)
{
    SMSCLPC47B27xState *s = opaque;
    uint8_t ldn = s->global[SIO_REG_LDN];
    uint8_t mask;

    if (!s->config_mode) {
        if (addr == 0 && val == SIO_ENTER_KEY) {
            s->config_mode = true;
        }
        return;
    }
    if (addr == 0) {
        if (val == SIO_EXIT_KEY) {
            s->config_mode = false;
        } else {
            s->index = val;
        }
        return;
    }
    trace_smsc_lpc47b27x_config_write(ldn, s->index, val);
    if (s->index < SIO_GLOBAL_REGS) {
        sio_global_write(s, s->index, val);
        return;
    }
    if (ldn >= SIO_LDN_COUNT) {
        return;
    }
    mask = sio_ldn_mask(ldn, s->index);
    if (mask == 0) {
        return;
    }
    s->ldn[ldn][s->index] = val & mask;
    switch (s->index) {
    case SIO_LDN_ACTIVATE:
    case SIO_LDN_BASE_HI:
    case SIO_LDN_BASE_LO:
    case SIO_LDN_IRQ:
    case SIO_LDN_IRQ2:
        sio_update_decode(s);
        break;
    }
}

static const MemoryRegionOps sio_config_ops = {
    .read = sio_config_read,
    .write = sio_config_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 1,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static uint64_t sio_runtime_read(void *opaque, hwaddr addr, unsigned size)
{
    SMSCLPC47B27xState *s = opaque;

    return s->runtime[addr];
}

static void sio_runtime_write(void *opaque, hwaddr addr, uint64_t val,
                              unsigned size)
{
    SMSCLPC47B27xState *s = opaque;

    s->runtime[addr] = val;
}

static const MemoryRegionOps sio_runtime_ops = {
    .read = sio_runtime_read,
    .write = sio_runtime_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 1,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void sio_source_irq(void *opaque, int n, int level)
{
    SMSCLPC47B27xState *s = opaque;

    s->source_level[n] = level;
    sio_update_irqs(s);
}

void smsc_lpc47b27x_attach_uart1(SMSCLPC47B27xState *s, SysBusDevice *uart)
{
    s->uart1_io = sysbus_mmio_get_region(uart, 0);
    sysbus_connect_irq(uart, 0,
                       qdev_get_gpio_in_named(DEVICE(s), SIO_GPIO_SOURCE,
                                              SIO_SOURCE_UART1));
    sio_update_decode(s);
}

void smsc_lpc47b27x_attach_kbc(SMSCLPC47B27xState *s, ISADevice *kbc)
{
    ISAKBDState *isa_kbc = I8042(kbc);

    s->kbc_io[0] = &isa_kbc->io[0];
    s->kbc_io[1] = &isa_kbc->io[1];
    qdev_connect_gpio_out(DEVICE(kbc), I8042_KBD_IRQ,
                          qdev_get_gpio_in_named(DEVICE(s), SIO_GPIO_SOURCE,
                                                 SIO_SOURCE_KBD));
    qdev_connect_gpio_out(DEVICE(kbc), I8042_MOUSE_IRQ,
                          qdev_get_gpio_in_named(DEVICE(s), SIO_GPIO_SOURCE,
                                                 SIO_SOURCE_MOUSE));
    sio_update_decode(s);
}

/*
 * Hard reset (nPCI_RESET).  UART2's Activate bit and CR22 bits 5 and 7,
 * the keyboard's F0h bits 6:5 and the runtime block's F0h/F1h reset on
 * VTR POR only (Table 20-1 Notes 1-3): sio_realize sets those.
 */
static void sio_reset(DeviceState *dev)
{
    SMSCLPC47B27xState *s = SMSC_LPC47B27X(dev);
    uint8_t power_vtr = s->global[SIO_REG_POWER] & SIO_POWER_PME;
    uint8_t kbd_vtr = s->ldn[LDN_KBD][0xf0] & 0x60;
    unsigned ldn, i;

    s->config_mode = false;
    s->index = 0;
    s->global[SIO_REG_POWER] = power_vtr;
    s->global[SIO_REG_POWER_MGMT] = 0;
    s->global[SIO_REG_OSC] = SIO_OSC_RESET;
    s->global[SIO_REG_CFGPORT_LO] = SIO_CONFIG_PORT;
    s->global[SIO_REG_CFGPORT_HI] = 0;
    sio_reset_ldn_common(s);
    for (ldn = 0; ldn < SIO_LDN_COUNT; ldn++) {
        if (ldn != LDN_PME) {
            memset(&s->ldn[ldn][0xf0], 0, SIO_LDN_REGS - 0xf0);
        }
    }
    s->ldn[LDN_FDC][0xf0] = 0x0e;
    s->ldn[LDN_FDC][0xf2] = 0xff;
    s->ldn[LDN_LPT][0xf0] = 0x3c;
    s->ldn[LDN_UART2][0xf1] = 0x02;
    s->ldn[LDN_UART2][0xf2] = 0x03;
    s->ldn[LDN_KBD][0xf0] = kbd_vtr;
    for (i = 0; s->firmware_init && i < ARRAY_SIZE(sio_firmware_init); i++) {
        uint8_t target = sio_firmware_init[i].ldn;
        uint8_t reg = sio_firmware_init[i].reg;

        if (target == SIO_GLOBAL_WRITE) {
            s->global[reg] = sio_firmware_init[i].value;
        } else {
            s->global[SIO_REG_LDN] = target;
            s->ldn[target][reg] = sio_firmware_init[i].value &
                                  sio_ldn_mask(target, reg);
        }
    }
    sio_update_config_port(s);
    sio_update_decode(s);
}

static void sio_realize(DeviceState *dev, Error **errp)
{
    SMSCLPC47B27xState *s = SMSC_LPC47B27X(dev);
    ISADevice *isa = ISA_DEVICE(dev);

    /* VTR POR. */
    memset(s->global, 0, sizeof(s->global));
    memset(s->ldn, 0, sizeof(s->ldn));
    s->global[SIO_REG_DEVID] = SIO_DEVID_LPC47B27X;
    s->global[SIO_REG_DEVREV] = SIO_DEVREV;

    qdev_init_gpio_in_named(dev, sio_source_irq, SIO_GPIO_SOURCE,
                            SIO_SOURCES);
    memory_region_init_io(&s->config_io, OBJECT(s), &sio_config_ops, s,
                          "smsc-lpc47b27x-config", SIO_CONFIG_PORT_SIZE);
    isa_register_ioport(isa, &s->config_io, SIO_CONFIG_PORT);
    memory_region_init_io(&s->runtime_io, OBJECT(s), &sio_runtime_ops, s,
                          "smsc-lpc47b27x-runtime", SIO_RUNTIME_SIZE);
    isa_register_ioport(isa, &s->runtime_io, 0);
    memory_region_set_enabled(&s->runtime_io, false);
}

static int sio_post_load(void *opaque, int version_id)
{
    SMSCLPC47B27xState *s = opaque;

    sio_update_config_port(s);
    sio_update_decode(s);
    return 0;
}

static const VMStateDescription vmstate_sio = {
    .name = "smsc-lpc47b27x",
    .version_id = 2,
    .minimum_version_id = 2,
    .post_load = sio_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_BOOL(config_mode, SMSCLPC47B27xState),
        VMSTATE_UINT8(index, SMSCLPC47B27xState),
        VMSTATE_UINT8_ARRAY(global, SMSCLPC47B27xState, SIO_GLOBAL_REGS),
        VMSTATE_UINT8_2DARRAY(ldn, SMSCLPC47B27xState, SIO_LDN_COUNT,
                              SIO_LDN_REGS),
        VMSTATE_UINT8_ARRAY(runtime, SMSCLPC47B27xState, SIO_RUNTIME_SIZE),
        VMSTATE_BOOL_ARRAY(source_level, SMSCLPC47B27xState, SIO_SOURCES),
        VMSTATE_BOOL_ARRAY(isa_level, SMSCLPC47B27xState, ISA_NUM_IRQS),
        VMSTATE_END_OF_LIST()
    },
};

static const Property sio_properties[] = {
    DEFINE_PROP_BOOL(SMSC_LPC47B27X_PROP_FIRMWARE_INIT, SMSCLPC47B27xState,
                     firmware_init, false),
};

static void sio_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_props(dc, sio_properties);
    dc->realize = sio_realize;
    device_class_set_legacy_reset(dc, sio_reset);
    dc->vmsd = &vmstate_sio;
    dc->desc = "SMSC LPC47B27x Super I/O";
    dc->user_creatable = false;
}

static const TypeInfo sio_info = {
    .name = TYPE_SMSC_LPC47B27X,
    .parent = TYPE_ISA_DEVICE,
    .instance_size = sizeof(SMSCLPC47B27xState),
    .class_init = sio_class_init,
};

static void sio_register_types(void)
{
    type_register_static(&sio_info);
}

type_init(sio_register_types)
