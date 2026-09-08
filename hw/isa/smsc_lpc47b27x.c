/*
 * SMSC LPC47B27x Super I/O -- the configuration face only.
 *
 * The HP i2000 / Intel 460GX SDV carries an SMSC LPC47B27x on the LPC bus
 * behind the 82468GX.  Its logical devices (UART, keyboard controller, ...)
 * are the ordinary ISA devices this machine already builds; what was
 * missing is the chip's configuration space, which the vendor firmware's
 * ACPI namespace reads to find them: the DSDT's \_SB.PCI0.LPC declares an
 * index/data pair at 2Eh/2Fh (SMC1) and an IndexField over it (LDN at 07h,
 * ACTR 30h, IOAH/IOAL 60h/61h, INTR 70h, INT1 72h, DMCH 74h, OPT1-3
 * F0h-F2h), and UAR1's _STA/_CRS select logical device 4 and read them --
 * open bus there made COM1 "present" at I/O FFFFh on IRQ 1<<FFh.  The same
 * firmware's chipset-init script programs exactly this chip: enter with 55h,
 * LDN 4 = UART1 at 3F8h IRQ 4 active, LDN 7 = keyboard IRQ 1 active, LDN Ah =
 * the runtime register block at 800h active, FDC/LPT/UART2/game disabled,
 * leave with AAh.  Those are the reset contents here.
 *
 * The 82468GX forwards 2Eh/2Fh to LPC when LPC Enables bit 11 is set (SSDM
 * 11.1.27); this model does not gate on it, as the machine's LPC devices
 * are not gated on that register either.  Reprogramming a logical device's
 * base or IRQ through this space is recorded but does not move the ISA
 * device it stands for.
 */

#include "qemu/osdep.h"
#include "hw/isa/isa.h"
#include "hw/isa/smsc_lpc47b27x.h"
#include "migration/vmstate.h"
#include "qom/object.h"

#define SIO_CONFIG_PORT     0x2e
#define SIO_ENTER_KEY       0x55
#define SIO_EXIT_KEY        0xaa
#define SIO_REG_LDN         0x07
#define SIO_REG_DEVID       0x20
#define SIO_REG_DEVREV      0x21
#define SIO_REG_PWRCTL      0x22
#define SIO_REG_PWRMGMT     0x23
#define SIO_REG_OSC         0x24
#define SIO_REG_CFGPORT     0x26
#define SIO_GLOBAL_REGS     0x30
#define SIO_LDN_COUNT       0x0c
#define SIO_LDN_REGS        0x100
#define SIO_DEVID_LPC47B27X 0x51
#define SIO_RUNTIME_BASE    0x800
#define SIO_RUNTIME_SIZE    0x80

#define LDN_FDC   0x0
#define LDN_LPT   0x3
#define LDN_UART1 0x4
#define LDN_UART2 0x5
#define LDN_KBD   0x7
#define LDN_GAME  0x9
#define LDN_PME   0xa

struct SMSCLPC47B27xState {
    ISADevice parent_obj;

    MemoryRegion config_io;
    MemoryRegion runtime_io;
    bool config_mode;
    uint8_t index;
    uint8_t global[SIO_GLOBAL_REGS];
    uint8_t ldn[SIO_LDN_COUNT][SIO_LDN_REGS];
    uint8_t runtime[SIO_RUNTIME_SIZE];
};

static void sio_ldn_set(SMSCLPC47B27xState *s, unsigned ldn, bool active,
                        uint16_t base, uint8_t irq, uint8_t dma)
{
    uint8_t *r = s->ldn[ldn];

    r[0x30] = active;
    r[0x60] = base >> 8;
    r[0x61] = base & 0xff;
    r[0x70] = irq;
    r[0x74] = dma;
}

static uint64_t sio_config_read(void *opaque, hwaddr addr, unsigned size)
{
    SMSCLPC47B27xState *s = opaque;
    uint8_t ldn;

    if (addr == 0) {
        return s->index;
    }
    if (!s->config_mode) {
        return 0xff;
    }
    if (s->index < SIO_GLOBAL_REGS) {
        return s->global[s->index];
    }
    ldn = s->global[SIO_REG_LDN];
    if (ldn >= SIO_LDN_COUNT) {
        return 0xff;
    }
    return s->ldn[ldn][s->index];
}

static void sio_config_write(void *opaque, hwaddr addr, uint64_t val,
                             unsigned size)
{
    SMSCLPC47B27xState *s = opaque;
    uint8_t ldn;

    if (addr == 0) {
        if (val == SIO_ENTER_KEY) {
            s->config_mode = true;
        } else if (val == SIO_EXIT_KEY) {
            s->config_mode = false;
        } else {
            s->index = val;
        }
        return;
    }
    if (!s->config_mode) {
        return;
    }
    if (s->index < SIO_GLOBAL_REGS) {
        if (s->index != SIO_REG_DEVID && s->index != SIO_REG_DEVREV) {
            s->global[s->index] = val;
        }
        return;
    }
    ldn = s->global[SIO_REG_LDN];
    if (ldn < SIO_LDN_COUNT) {
        s->ldn[ldn][s->index] = val;
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

static void sio_reset(DeviceState *dev)
{
    SMSCLPC47B27xState *s = SMSC_LPC47B27X(dev);

    s->config_mode = false;
    s->index = 0;
    memset(s->global, 0, sizeof(s->global));
    memset(s->ldn, 0, sizeof(s->ldn));
    memset(s->runtime, 0, sizeof(s->runtime));
    s->global[SIO_REG_DEVID] = SIO_DEVID_LPC47B27X;
    s->global[SIO_REG_DEVREV] = 0x01;
    s->global[SIO_REG_CFGPORT] = SIO_CONFIG_PORT;
    /* As the i2000 firmware's chipset-init script leaves it. */
    sio_ldn_set(s, LDN_FDC, false, 0x3f0, 6, 2);
    s->ldn[LDN_FDC][0xf0] = 0x0e;
    sio_ldn_set(s, LDN_LPT, false, 0x278, 7, 4);
    s->ldn[LDN_LPT][0xf0] = 0x3c;
    sio_ldn_set(s, LDN_UART1, true, 0x3f8, 4, 4);
    s->ldn[LDN_UART1][0xf0] = 0x02;
    sio_ldn_set(s, LDN_UART2, false, 0x2f8, 3, 4);
    s->ldn[LDN_UART2][0xf0] = 0x02;
    sio_ldn_set(s, LDN_KBD, true, 0x60, 1, 4);
    s->ldn[LDN_KBD][0x72] = 12;
    s->ldn[LDN_KBD][0xf0] = 0x18;
    sio_ldn_set(s, LDN_GAME, false, 0x200, 0, 4);
    sio_ldn_set(s, LDN_PME, true, SIO_RUNTIME_BASE, 0, 4);
}

static void sio_realize(DeviceState *dev, Error **errp)
{
    SMSCLPC47B27xState *s = SMSC_LPC47B27X(dev);
    ISADevice *isa = ISA_DEVICE(dev);

    memory_region_init_io(&s->config_io, OBJECT(s), &sio_config_ops, s,
                          "smsc-lpc47b27x-config", 2);
    isa_register_ioport(isa, &s->config_io, SIO_CONFIG_PORT);
    memory_region_init_io(&s->runtime_io, OBJECT(s), &sio_runtime_ops, s,
                          "smsc-lpc47b27x-runtime", SIO_RUNTIME_SIZE);
    isa_register_ioport(isa, &s->runtime_io, SIO_RUNTIME_BASE);
}

static const VMStateDescription vmstate_sio = {
    .name = "smsc-lpc47b27x",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_BOOL(config_mode, SMSCLPC47B27xState),
        VMSTATE_UINT8(index, SMSCLPC47B27xState),
        VMSTATE_UINT8_ARRAY(global, SMSCLPC47B27xState, SIO_GLOBAL_REGS),
        VMSTATE_UINT8_2DARRAY(ldn, SMSCLPC47B27xState, SIO_LDN_COUNT,
                              SIO_LDN_REGS),
        VMSTATE_UINT8_ARRAY(runtime, SMSCLPC47B27xState, SIO_RUNTIME_SIZE),
        VMSTATE_END_OF_LIST()
    },
};

static void sio_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = sio_realize;
    device_class_set_legacy_reset(dc, sio_reset);
    dc->vmsd = &vmstate_sio;
    dc->desc = "SMSC LPC47B27x Super I/O configuration space";
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
