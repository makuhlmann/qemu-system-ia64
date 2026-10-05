/*
 * Intel 82468GX I/O and Firmware Bridge
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"

#include "hw/acpi/acpi.h"
#include "hw/core/irq.h"
#include "hw/i2c/pm_smbus.h"
#include "hw/ide/pci.h"
#include "hw/intc/i8259.h"
#include "hw/isa/isa.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_device.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/block/block.h"
#include "system/block-backend.h"
#include "qemu/timer.h"
#include "qemu/log.h"
#include "hw/rtc/mc146818rtc.h"
#include "hw/rtc/mc146818rtc_regs.h"
#include "hw/timer/i8254.h"
#include "hw/southbridge/intel_82468gx.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "system/runstate.h"
#include "trace.h"

typedef struct Intel82468GXSMBusState {
    PCIDevice parent_obj;

    PMSMBus smb;
    MemoryRegion io_alias;
    qemu_irq smi;
} Intel82468GXSMBusState;

#define INTEL_82468GX_IFB_SMBUS(obj) \
    OBJECT_CHECK(Intel82468GXSMBusState, (obj), \
                 TYPE_INTEL_82468GX_IFB_SMBUS)

/*
 * The RTC is a 256-byte part in two 128-byte banks (SSDM 15.5.1).  The
 * standard bank answers at 0x70/0x71; the extended bank, a full 128 bytes of
 * battery-backed SRAM, answers at 0x72/0x73 while RTCCFG (config offset C8h)
 * bit 2 "Upper RAM Enable" is set, and with the bit clear 0x72/0x73 alias
 * 0x70/0x71 and reach the standard bank instead.
 *
 * The SSDM (11.1.20) gives RTCCFG a reset value of 00h.  The i2000's own
 * firmware (v1.30) never writes RTCCFG, yet keeps its configuration store in
 * the extended bank through 0x72/0x73 -- including an 0xFF fill of bytes
 * 08h-1Fh during POST.  Under the documented reset value that fill would land
 * on the standard bank's registers A-D, put the divider chain in reset and set
 * SET, and the clock would stop for good (which is exactly what happened here:
 * the firmware's PXE timeouts are driven by GetTime and never expired).  A
 * board whose firmware halted its own clock on every boot could not have
 * shipped, so the silicon must present the extended bank at 0x72/0x73 out of
 * reset; the bit therefore resets set here, and stays writable so software can
 * still select the alias.
 */
/*
 * General-purpose I/O (SSDM 11.1.21-22, 11.2.9): 64 bytes of I/O space at
 * GPIOBA (D0h) bits 15:6, decoded while GPIOE (D4h) bit 0 is set.  In each
 * register bits 28:24 are the muxed GPIO[22:18], bits 19:16 the muxed
 * GPIO[13:10] and bits 8:0 the dedicated GPIO[8:0].  An input reads the level
 * the board puts on its pin ("gpio-inputs"); nothing on the board listens to
 * an output.  Blink, SMI routing, pulse and core-well control are latches
 * only, and MGPIOC (84h), whose bits the SSDM leaves undocumented, does not
 * gate the muxed pins.
 */
#define IFB_GPIOBA             0xd0
#define IFB_GPIOE              0xd4
#define IFB_GPIO_SIZE          64
#define IFB_GP_OUTPUT          0x00
#define IFB_GP_DATA            0x04
#define IFB_GP_TTL             0x08
#define IFB_GP_BLINK           0x0c
#define IFB_GP_LOCK            0x10
#define IFB_GP_INVERT          0x14
#define IFB_GP_SMI             0x1c
#define IFB_GP_PULSE           0x20
#define IFB_GP_CORE            0x24
#define IFB_GP_PULLUP          0x28
#define IFB_GP_REGS            (IFB_GP_PULLUP / 4 + 1)
#define IFB_GP_ALL             0x1f0f01ffU
#define IFB_GP_DEDICATED       0x000001ffU
#define IFB_GP_PULLUP_RESET    0x000003ffU

#define IFB_FREQ_MAILBOX       0xd0
#define IFB_FREQ_MAILBOX_DONE  0x8000
#define IFB_RTC_CFG            0xc8
#define IFB_RTC_CFG_UPPER_EN   0x04
#define IFB_RTC_CFG_LOCK_LOWER 0x08
#define IFB_RTC_CFG_LOCK_UPPER 0x10
#define IFB_RTC_BANK_SIZE      128
#define IFB_RTC_EXT_IOPORT     0x72
#define IFB_RTC_LOCK_FIRST     0x38
#define IFB_RTC_LOCK_LAST      0x3f

struct Intel82468GXIFBState {
    PCIDevice parent_obj;

    PCIDevice *functions[INTEL_82468GX_IFB_FUNCTIONS];
    ISABus *isa_bus;
    qemu_irq legacy_irq;
    qemu_irq isa_irq[ISA_NUM_IRQS];
    qemu_irq sci;
    qemu_irq sci_in;
    qemu_irq *pic_irqs;
    qemu_irq *isa_irqs;
    PICCommonState *master_pic;
    MemoryRegion acpi_pm;
    MemoryRegion acpi_gpe;
    MemoryRegion acpi_smi;
    MemoryRegion apm;
    ACPIREGS acpi_regs;
    /* SSDM 11.2.8.1 Global Control and Enable, at ACPI block offset 1Ah. */
    uint16_t glbctl;
    /* SSDM 11.2.8.2 Global Status, at 1Ch: of its causes, APM_STS. */
    uint16_t glbsts;
    /* SSDM 11.2.6 APM control and status ports, B2h/B3h. */
    uint8_t apmc;
    uint8_t apms;
    /* The SMI# pin, asserted high here. */
    qemu_irq smi;
    bool smi_asserted;
    /*
     * The ACPI I/O base the board's firmware programs at POST, or 0 for the
     * part's own reset state (block disabled).  The vendor i2000 firmware
     * carries the register pokes in a chipset-init script this build never
     * reaches, while its FADT, DSDT and PMI handler all assume the block at
     * that base; the machine supplies the script's result through this.
     */
    uint16_t init_acpi_base;
    Notifier powerdown_notifier;
    ISADevice *pit;
    MC146818RtcState *rtc;
    MemoryRegion nmisc;
    MemoryRegion rtc_ext;
    MemoryRegion rtc_ext_alias;
    uint8_t nmisc_value;
    /*
     * CPU-frequency mailbox (config offset D0h).  The SDV firmware writes a
     * frequency command here with bit 15 clear and polls until the register
     * reads back with bit 15 set -- the hardware "done/valid" flag -- then
     * compares the detected frequency against its reference and reboots
     * through port 0xCF9 if they disagree.  Real silicon latches the value
     * and the register is battery-backed, so this cell survives reset (a
     * cleared mailbox makes the detection fall back to a sentinel and the
     * reboot repeat forever; b18d80a).
     */
    uint32_t freq_mailbox;
    MemoryRegion gpio;
    uint32_t gp[IFB_GP_REGS];
    /* Pin levels in GP Data bit positions, as the board drives or pulls them. */
    uint32_t gpio_inputs;
    uint8_t rtc_ext_index;
    uint8_t rtc_ext_ram[IFB_RTC_BANK_SIZE];
    /* The board's battery for both RTC banks; NULL keeps them volatile. */
    BlockBackend *battery;
    uint8_t *battery_shadow;
    QEMUTimer *battery_timer;
    VMChangeStateEntry *battery_vmstate;
};

#define IFB_ACPI_PM_IO_SIZE 0x40
/*
 * Nmisc, the NMI Status and Control register at I/O 0x61 (SSDM 11.2.4.1).
 * Bits 3:0 are read/write; bits 7, 5 and 4 are status and must be written 0.
 */
#define IFB_NMISC_IOPORT       0x61
#define IFB_NMISC_WRITABLE     0x0f
#define IFB_NMISC_TIMER2_GATE  0x01
#define IFB_NMISC_REFRESH      0x10
#define IFB_NMISC_TIMER2_OUT   0x20
/*
 * Bit 4 toggles once per DRAM refresh cycle, which the IFB drives from timer
 * counter 1.  Counter 1's output is not otherwise observable and a guest need
 * not have programmed it, so run the toggle off the virtual clock at the
 * period the refresh counter is programmed for on this platform: everything
 * that reads this bit is measuring the toggle's rate, not counter 1's state.
 */
#define IFB_NMISC_REFRESH_NS   15000
#define IFB_ACPI_GPE_OFFSET 0x0c
#define IFB_ACPI_GPE_LENGTH 4
/*
 * Global Control and Enable and Global Status (SSDM 11.2.8), "added to the
 * end of the I/O register space defined by the ACPI block".  SMI_EN enables
 * every SMI cause; APMC_EN enables "SMIs based upon accesses to the APM
 * control port at B2h", which set APM_STS (write 1 to clear).  SMI# "remains
 * active until the EOS bit is set", and EOS is "automatically cleared once
 * IFB asserts SMI#" (16.2.2): software clears the status, then sets EOS, and
 * a cause still pending asserts SMI# again.  EOS resets to 1.
 */
#define IFB_ACPI_SMI_OFFSET     0x1a
#define IFB_ACPI_SMI_LENGTH     4
#define IFB_SMI_GLBCTL          0
#define IFB_SMI_GLBSTS          2
#define IFB_GLBCTL_SMI_EN       BIT(0)
#define IFB_GLBCTL_EOS          BIT(3)
#define IFB_GLBCTL_DEFAULT      IFB_GLBCTL_EOS
#define IFB_GLBCTL_APMC_EN      BIT(10)
#define IFB_GLBCTL_WRITABLE     0x1fff
#define IFB_GLBSTS_APM_STS      BIT(3)
/*
 * APMC/APMS (SSDM 11.2.6): "located in normal I/O space", always decoded --
 * "the APM power management ranges (B2/B3h) are always enabled and are not
 * affected by" ACPI Enable (11.1.9).  "Writes to [APMC] store data ... In
 * addition, writes generate an SMI, if the APMC_EN bit ... is set to 1.
 * Reads do not generate an SMI."
 */
#define IFB_APM_IOPORT          0xb2

/*
 * SCIRC (45h) "specifies on which pin the SCI will appear on internally",
 * reset value 000b = IRQ9 (SSDM 11.1.10).  "When this interrupt is chosen,
 * the corresponding interrupt pin is blocked", so the SCI owns the line
 * rather than sharing it with an ISA source.  The two encodings that name no
 * ISA interrupt -- the part's own SCI pin and FEMPTY# -- leave through the
 * separate "sci" output, which a board wires wherever it takes them.
 */
#define IFB_SCIRC           0x45
#define IFB_SCIRC_MAP       0x07

static const int ifb_scirc_isa_irq[8] = { 9, -1, -1, 8, 0, 10, 11, -1 };

static int ifb_sci_isa_irq(Intel82468GXIFBState *s)
{
    PCIDevice *pci = PCI_DEVICE(s);

    return ifb_scirc_isa_irq[pci->config[IFB_SCIRC] & IFB_SCIRC_MAP];
}

static void ifb_sci_set(void *opaque, int n, int level)
{
    Intel82468GXIFBState *s = opaque;
    int irq = ifb_sci_isa_irq(s);

    if (irq < 0) {
        qemu_set_irq(s->sci, level);
        return;
    }
    qemu_set_irq(s->pic_irqs[irq], level);
    qemu_set_irq(s->isa_irq[irq], level);
}

static void ifb_acpi_update_sci(ACPIREGS *ar)
{
    Intel82468GXIFBState *s = container_of(ar, Intel82468GXIFBState,
                                           acpi_regs);

    if (ar->pm1.cnt.cnt & ACPI_BITMASK_SCI_ENABLE) {
        acpi_update_sci(ar, s->sci_in);
    } else {
        qemu_set_irq(s->sci_in, 0);
    }
}

static uint64_t ifb_nmisc_read(void *opaque, hwaddr addr, unsigned size)
{
    Intel82468GXIFBState *s = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    PITChannelInfo ch;
    uint8_t value;

    value = s->nmisc_value & IFB_NMISC_WRITABLE;
    if ((now / IFB_NMISC_REFRESH_NS) & 1) {
        value |= IFB_NMISC_REFRESH;
    }
    if (s->pit != NULL) {
        pit_get_channel_info(PIT_COMMON(s->pit), 2, &ch);
        if (ch.out) {
            value |= IFB_NMISC_TIMER2_OUT;
        }
    }
    /* Bit 7 reports a latched SERR#, which nothing on this machine drives. */
    return value;
}

static void ifb_nmisc_write(void *opaque, hwaddr addr, uint64_t value,
                            unsigned size)
{
    Intel82468GXIFBState *s = opaque;

    s->nmisc_value = value & IFB_NMISC_WRITABLE;
    if (s->pit != NULL) {
        pit_set_gate(PIT_COMMON(s->pit), 2,
                     (s->nmisc_value & IFB_NMISC_TIMER2_GATE) != 0);
    }
}

static const MemoryRegionOps ifb_nmisc_ops = {
    .read = ifb_nmisc_read,
    .write = ifb_nmisc_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 1,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

/*
 * Bytes 38h-3Fh of each bank can be locked away by RTCCFG bits 4 (extended
 * bank) and 3 (standard bank): once locked they are neither readable nor
 * writeable until a hardware reset.  The extended bank is ours to enforce;
 * the standard bank lives inside the RTC device, so bit 3 is latched but its
 * effect is not modelled -- nothing on this platform sets it.
 */
static bool ifb_rtc_ext_locked(Intel82468GXIFBState *s)
{
    PCIDevice *pci = PCI_DEVICE(s);

    return (pci->config[IFB_RTC_CFG] & IFB_RTC_CFG_LOCK_UPPER) &&
           s->rtc_ext_index >= IFB_RTC_LOCK_FIRST &&
           s->rtc_ext_index <= IFB_RTC_LOCK_LAST;
}

static uint64_t ifb_rtc_ext_read(void *opaque, hwaddr addr, unsigned size)
{
    Intel82468GXIFBState *s = opaque;

    /* The index register is write-only, as on the standard bank's port. */
    if (addr == 0 || ifb_rtc_ext_locked(s)) {
        return 0xff;
    }
    return s->rtc_ext_ram[s->rtc_ext_index];
}

static void ifb_rtc_ext_write(void *opaque, hwaddr addr, uint64_t value,
                              unsigned size)
{
    Intel82468GXIFBState *s = opaque;

    if (addr == 0) {
        s->rtc_ext_index = value & (IFB_RTC_BANK_SIZE - 1);
        return;
    }
    if (!ifb_rtc_ext_locked(s)) {
        s->rtc_ext_ram[s->rtc_ext_index] = value;
        trace_ifb_rtc_ext_write(s->rtc_ext_index, value);
    }
}

static const MemoryRegionOps ifb_rtc_ext_ops = {
    .read = ifb_rtc_ext_read,
    .write = ifb_rtc_ext_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 1,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

/* Point 0x72/0x73 at whichever bank RTCCFG bit 2 currently selects. */
static void ifb_rtc_bank_update(Intel82468GXIFBState *s)
{
    bool upper = (PCI_DEVICE(s)->config[IFB_RTC_CFG] &
                  IFB_RTC_CFG_UPPER_EN) != 0;

    memory_region_set_enabled(&s->rtc_ext, upper);
    memory_region_set_enabled(&s->rtc_ext_alias, !upper);
}

/*
 * The 8259 pair's INTR output.  It has to go through a handler rather than
 * straight to the "legacy" GPIO line: the pair is built when the bridge is
 * realized, and a GPIO output is not bound to anything until the machine
 * connects it afterwards, so passing the line by value here would capture the
 * unconnected placeholder and the pair could never deliver an interrupt.
 */
static void ifb_legacy_irq_handler(void *opaque, int n, int level)
{
    Intel82468GXIFBState *s = opaque;

    qemu_set_irq(s->legacy_irq, level);
}

static void ifb_isa_irq_handler(void *opaque, int irq, int level)
{
    Intel82468GXIFBState *s = opaque;

    if (irq == ifb_sci_isa_irq(s)) {
        return;
    }
    qemu_set_irq(s->pic_irqs[irq], level);
    qemu_set_irq(s->isa_irq[irq], level);
}

static uint64_t ifb_acpi_gpe_read(void *opaque, hwaddr addr, unsigned size)
{
    Intel82468GXIFBState *s = opaque;
    uint64_t value = 0;
    unsigned i;

    for (i = 0; i < size; i++) {
        value |= (uint64_t)acpi_gpe_ioport_readb(&s->acpi_regs, addr + i)
            << (i * 8);
    }
    return value;
}

static void ifb_acpi_gpe_write(void *opaque, hwaddr addr, uint64_t value,
                               unsigned size)
{
    Intel82468GXIFBState *s = opaque;
    unsigned i;

    for (i = 0; i < size; i++) {
        acpi_gpe_ioport_writeb(&s->acpi_regs, addr + i,
                               value >> (i * 8));
    }
    ifb_acpi_update_sci(&s->acpi_regs);
}

static const MemoryRegionOps ifb_acpi_gpe_ops = {
    .read = ifb_acpi_gpe_read,
    .write = ifb_acpi_gpe_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
        .unaligned = true,
    },
};

static void ifb_smi_update(Intel82468GXIFBState *s)
{
    bool cause = (s->glbsts & IFB_GLBSTS_APM_STS) &&
                 (s->glbctl & IFB_GLBCTL_APMC_EN);

    if (s->smi_asserted && (s->glbctl & IFB_GLBCTL_EOS)) {
        s->smi_asserted = false;
        qemu_set_irq(s->smi, 0);
    }
    if (!s->smi_asserted && cause && (s->glbctl & IFB_GLBCTL_SMI_EN) &&
        (s->glbctl & IFB_GLBCTL_EOS)) {
        s->smi_asserted = true;
        s->glbctl &= ~IFB_GLBCTL_EOS;
        qemu_set_irq(s->smi, 1);
    }
}

static uint64_t ifb_acpi_smi_read(void *opaque, hwaddr addr, unsigned size)
{
    Intel82468GXIFBState *s = opaque;
    uint32_t regs = s->glbctl | ((uint32_t)s->glbsts << 16);

    return (regs >> (addr * 8)) & MAKE_64BIT_MASK(0, size * 8);
}

static void ifb_acpi_smi_write(void *opaque, hwaddr addr, uint64_t value,
                               unsigned size)
{
    Intel82468GXIFBState *s = opaque;
    uint32_t mask = MAKE_64BIT_MASK(0, size * 8) << (addr * 8);
    uint32_t data = (value << (addr * 8)) & mask;
    uint16_t ctl_mask = mask >> (IFB_SMI_GLBCTL * 8);
    uint16_t sts_clear = data >> (IFB_SMI_GLBSTS * 8);

    s->glbctl = ((s->glbctl & ~ctl_mask) | (data & ctl_mask)) &
                IFB_GLBCTL_WRITABLE;
    s->glbsts &= ~(sts_clear & IFB_GLBSTS_APM_STS);
    ifb_smi_update(s);
}

static const MemoryRegionOps ifb_acpi_smi_ops = {
    .read = ifb_acpi_smi_read,
    .write = ifb_acpi_smi_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static uint64_t ifb_apm_read(void *opaque, hwaddr addr, unsigned size)
{
    Intel82468GXIFBState *s = opaque;

    return addr == 0 ? s->apmc : s->apms;
}

static void ifb_apm_write(void *opaque, hwaddr addr, uint64_t value,
                          unsigned size)
{
    Intel82468GXIFBState *s = opaque;

    if (addr == 0) {
        s->apmc = value;
        if (s->glbctl & IFB_GLBCTL_APMC_EN) {
            s->glbsts |= IFB_GLBSTS_APM_STS;
            ifb_smi_update(s);
        }
    } else {
        s->apms = value;
    }
}

static const MemoryRegionOps ifb_apm_ops = {
    .read = ifb_apm_read,
    .write = ifb_apm_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
};

static const uint32_t ifb_gp_mask[IFB_GP_REGS] = {
    [IFB_GP_OUTPUT / 4] = IFB_GP_ALL,
    [IFB_GP_DATA / 4] = IFB_GP_ALL,
    [IFB_GP_TTL / 4] = IFB_GP_ALL,
    [IFB_GP_BLINK / 4] = IFB_GP_DEDICATED,
    [IFB_GP_LOCK / 4] = IFB_GP_ALL,
    [IFB_GP_INVERT / 4] = IFB_GP_DEDICATED,
    [IFB_GP_SMI / 4] = IFB_GP_DEDICATED,
    [IFB_GP_PULSE / 4] = IFB_GP_DEDICATED,
    [IFB_GP_CORE / 4] = IFB_GP_DEDICATED,
    [IFB_GP_PULLUP / 4] = IFB_GP_PULLUP_RESET,
};

static uint32_t ifb_gp_read_reg(Intel82468GXIFBState *s, unsigned reg)
{
    uint32_t out, in;

    if (reg != IFB_GP_DATA / 4) {
        return s->gp[reg];
    }
    /* An input is inverted "before entering the data register" (11.2.9.6). */
    out = s->gp[IFB_GP_OUTPUT / 4];
    in = (s->gpio_inputs ^ s->gp[IFB_GP_INVERT / 4]) & IFB_GP_ALL;
    return (s->gp[reg] & out) | (in & ~out);
}

static uint64_t ifb_gpio_read(void *opaque, hwaddr addr, unsigned size)
{
    Intel82468GXIFBState *s = opaque;
    unsigned reg = addr / 4;
    uint32_t value;

    if (reg >= IFB_GP_REGS) {
        return 0;
    }
    value = extract32(ifb_gp_read_reg(s, reg), (addr & 3) * 8, size * 8);
    if (reg == IFB_GP_DATA / 4) {
        trace_ifb_gp_data_read(value);
    }
    return value;
}

static void ifb_gpio_write(void *opaque, hwaddr addr, uint64_t value,
                           unsigned size)
{
    Intel82468GXIFBState *s = opaque;
    unsigned reg = addr / 4;
    unsigned shift = (addr & 3) * 8;
    uint32_t wmask;

    if (reg >= IFB_GP_REGS) {
        return;
    }
    wmask = ifb_gp_mask[reg] & MAKE_64BIT_MASK(shift, size * 8);
    value <<= shift;
    if (reg == IFB_GP_LOCK / 4) {
        /* Set-only: "it can only be cleared by a PCIRST#" (11.2.9.5). */
        s->gp[reg] |= value & wmask;
        return;
    }
    wmask &= ~s->gp[IFB_GP_LOCK / 4];
    if (reg == IFB_GP_DATA / 4) {
        wmask &= s->gp[IFB_GP_OUTPUT / 4];
    }
    s->gp[reg] = (s->gp[reg] & ~wmask) | (value & wmask);
}

static const MemoryRegionOps ifb_gpio_ops = {
    .read = ifb_gpio_read,
    .write = ifb_gpio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

/*
 * Decode at the written base: the D0h read-back carries the frequency
 * mailbox's done flag (bit 15, see freq_mailbox), which is not an address.
 */
static void ifb_gpio_io_update(Intel82468GXIFBState *s)
{
    PCIDevice *pci = PCI_DEVICE(s);

    memory_region_transaction_begin();
    memory_region_set_address(&s->gpio,
                              pci_get_long(pci->config + IFB_GPIOBA) &
                              0xffc0U);
    memory_region_set_enabled(&s->gpio, pci->config[IFB_GPIOE] & BIT(0));
    memory_region_transaction_commit();
}

static void ifb_acpi_io_update(Intel82468GXIFBState *s)
{
    PCIDevice *pci = PCI_DEVICE(s);
    uint32_t base = pci_get_long(pci->config + 0x40) & 0xffc0U;

    memory_region_transaction_begin();
    memory_region_set_address(&s->acpi_pm, base);
    memory_region_set_enabled(&s->acpi_pm,
                              base != 0 && (pci->config[0x44] & BIT(0)));
    memory_region_transaction_commit();
}

static void ifb_acpi_powerdown(Notifier *notifier, void *opaque)
{
    Intel82468GXIFBState *s = container_of(notifier,
                                           Intel82468GXIFBState,
                                           powerdown_notifier);

    (void)opaque;
    acpi_pm1_evt_power_down(&s->acpi_regs);
}

static void ifb_lpc_reset(DeviceState *dev)
{
    Intel82468GXIFBState *s = INTEL_82468GX_IFB(dev);
    PCIDevice *pci = PCI_DEVICE(s);

    pci_set_word(pci->config + PCI_COMMAND, 0x0007);
    pci_set_word(pci->config + PCI_STATUS, 0x0280);
    pci_set_long(pci->config + 0x40, 0x00000001);
    pci->config[0x44] = 0;
    pci->config[0x45] = 0;
    pci_set_word(pci->config + 0x4e, 0x07c1);
    memset(pci->config + 0x60, 0x80, 4);
    pci->config[0x64] = 0x10;
    pci->config[0x69] = 0x02;
    pci_set_word(pci->config + 0x6a, 0);
    pci->config[0x82] = 0;
    pci_set_word(pci->config + 0x84, 0x0500);
    pci_set_word(pci->config + 0x90, 0);
    pci_set_long(pci->config + 0x92, 0);
    pci->config[0xc8] = IFB_RTC_CFG_UPPER_EN;
    ifb_rtc_bank_update(s);
    pci_set_long(pci->config + 0xd0, 1);
    pci->config[0xd4] = 0;
    memset(pci->config + 0xe0, 0, 9);
    pci_set_long(pci->config + 0xe8, 0x00112233);

    s->nmisc_value = 0;
    s->rtc_ext_index = 0;
    memset(s->gp, 0, sizeof(s->gp));
    s->gp[IFB_GP_PULLUP / 4] = IFB_GP_PULLUP_RESET;
    ifb_gpio_io_update(s);

    acpi_pm1_evt_reset(&s->acpi_regs);
    acpi_pm1_cnt_reset(&s->acpi_regs);
    acpi_pm_tmr_reset(&s->acpi_regs);
    acpi_gpe_reset(&s->acpi_regs);
    s->acpi_regs.gpe.sts[1] = BIT(3);
    s->glbctl = IFB_GLBCTL_DEFAULT;
    s->glbsts = 0;
    s->apmc = 0;
    s->apms = 0;
    if (s->smi_asserted) {
        s->smi_asserted = false;
        qemu_set_irq(s->smi, 0);
    }
    if (s->init_acpi_base != 0) {
        /*
         * The board firmware's chipset-init pokes, in its order: ACPI Enable
         * off, ACPI Base, ACPI Enable on (the vendor script at bios130.BIN
         * 0x2c7a80: 00:03.0 @44h = 0, @40h = 0A00h, @44h = 1).  SMI_EN and
         * APMC_EN are the firmware's own (SAL_B run-time 0x3FF17CE0).
         */
        pci_set_long(pci->config + 0x40, (s->init_acpi_base & 0xffc0U) | 1U);
        pci->config[0x44] = BIT(0);
    }
    ifb_acpi_update_sci(&s->acpi_regs);
    ifb_acpi_io_update(s);
}

static bool ifb_freq_mailbox_reg(uint32_t address)
{
    return address >= IFB_FREQ_MAILBOX && address < IFB_FREQ_MAILBOX + 4;
}

static uint32_t ifb_lpc_read_config(PCIDevice *pci, uint32_t address,
                                    int length)
{
    Intel82468GXIFBState *s = INTEL_82468GX_IFB(pci);
    uint32_t value = 0;
    int i;

    if (!ranges_overlap(address, length, IFB_FREQ_MAILBOX, 4)) {
        return pci_default_read_config(pci, address, length);
    }

    /* Present the stored command with the done flag set: we latch at once. */
    for (i = 0; i < length; i++) {
        uint32_t byte = address + i;
        uint8_t data;

        if (ifb_freq_mailbox_reg(byte)) {
            uint32_t cell = s->freq_mailbox | IFB_FREQ_MAILBOX_DONE;

            data = cell >> ((byte - IFB_FREQ_MAILBOX) * 8);
        } else {
            data = pci_default_read_config(pci, byte, 1);
        }
        value |= (uint32_t)data << (i * 8);
    }
    return value;
}

static void ifb_lpc_write_config(PCIDevice *pci, uint32_t address,
                                 uint32_t value, int length)
{
    Intel82468GXIFBState *s = INTEL_82468GX_IFB(pci);
    uint16_t biosen = pci_get_word(pci->config + 0x4e);
    uint8_t rtccfg = pci->config[0xc8];
    int old_sci_irq = ifb_sci_isa_irq(s);
    int i;

    for (i = 0; i < length; i++) {
        uint32_t byte = address + i;

        if (ifb_freq_mailbox_reg(byte)) {
            unsigned shift = (byte - IFB_FREQ_MAILBOX) * 8;

            s->freq_mailbox &= ~(0xffU << shift);
            s->freq_mailbox |= ((value >> (i * 8)) & 0xff) << shift;
        }
    }

    pci_default_write_config(pci, address, value, length);
    if (ranges_overlap(address, length, IFB_SCIRC, 1) &&
        ifb_sci_isa_irq(s) != old_sci_irq) {
        if (old_sci_irq < 0) {
            qemu_set_irq(s->sci, 0);
        } else {
            qemu_set_irq(s->pic_irqs[old_sci_irq], 0);
            qemu_set_irq(s->isa_irq[old_sci_irq], 0);
        }
        ifb_acpi_update_sci(&s->acpi_regs);
    }
    if (biosen & BIT(15)) {
        pci_word_test_and_set_mask(pci->config + 0x4e, BIT(15));
    }
    pci->config[0xc8] |= rtccfg & (BIT(4) | BIT(3));
    ifb_rtc_bank_update(INTEL_82468GX_IFB(pci));
    if (ranges_overlap(address, length, 0x40, 5)) {
        ifb_acpi_io_update(INTEL_82468GX_IFB(pci));
    }
    if (ranges_overlap(address, length, IFB_GPIOBA, 5)) {
        ifb_gpio_io_update(s);
    }
}

static void ifb_lpc_init_config(PCIDevice *pci)
{
    /*
     * ACPI decode and BIOS/RTC locks have modeled side effects. The other
     * writable chipset fields are compatibility readback latches; their
     * hardware effects are not implemented.
     */
    memset(pci->wmask, 0, pci_config_size(pci));
    memset(pci->w1cmask, 0, pci_config_size(pci));
    pci_set_word(pci->wmask + PCI_COMMAND, BIT(8) | BIT(3));
    pci_set_word(pci->w1cmask + PCI_STATUS,
                 BIT(14) | BIT(13) | BIT(12) | BIT(11));
    pci_set_long(pci->wmask + 0x40, 0x0000ffc0);
    pci->wmask[0x44] = BIT(0);
    pci->wmask[0x45] = 0x07;
    pci_set_word(pci->wmask + 0x4e, BIT(15) | BIT(2));
    memset(pci->wmask + 0x60, 0x8f, 4);
    pci->wmask[0x64] = 0xff;
    pci->wmask[0x69] = 0xf0;
    pci->wmask[0x6a] = BIT(0);
    pci->w1cmask[0x6b] = BIT(7);
    pci->wmask[0x82] = 0x0f;
    pci_set_long(pci->wmask + 0x92, 0xffc0ffc0);
    pci->wmask[0xc8] = BIT(4) | BIT(3) | BIT(2);
    pci_set_long(pci->wmask + 0xd0, 0x0000ffc0);
    pci->wmask[0xd4] = BIT(0);
    pci->wmask[0xe0] = 0x77;
    pci->wmask[0xe1] = 0x13;
    pci->wmask[0xe2] = 0x3b;
    pci->wmask[0xe3] = 0xff;
    pci_set_word(pci->wmask + 0xe4, 0xfe01);
    pci_set_word(pci->wmask + 0xe6, 0x1fff);
    pci_set_long(pci->wmask + 0xe8, UINT32_MAX);
}

static PCIDevice *ifb_realize_function(Intel82468GXIFBState *s,
                                       unsigned function,
                                       const char *name, const char *type,
                                       Error **errp)
{
    PCIDevice *pci = pci_new(s->parent_obj.devfn + function, type);

    object_property_add_child(OBJECT(s), name, OBJECT(pci));
    object_unref(OBJECT(pci));
    if (!qdev_realize(DEVICE(pci), BUS(pci_get_bus(&s->parent_obj)), errp)) {
        object_unparent(OBJECT(pci));
        return NULL;
    }
    s->functions[function] = pci;
    return pci;
}

static void ifb_remove_function(PCIDevice **pci)
{
    if (!*pci) {
        return;
    }
    if (qdev_is_realized(DEVICE(*pci))) {
        qdev_unrealize(DEVICE(*pci));
    }
    object_unparent(OBJECT(*pci));
    *pci = NULL;
}

/*
 * The clock registers (00h-0Dh) come from -rtc at each start, as the part's
 * own clock would have kept counting; the rest of both banks is what the
 * battery kept.
 */
#define IFB_BATTERY_HEADER      16
#define IFB_BATTERY_PERIOD_MS   1000

static bool ifb_battery_restores(unsigned index)
{
    return index > RTC_REG_D;
}

static void ifb_battery_image(Intel82468GXIFBState *s, uint8_t *area)
{
    memset(area, 0, INTEL_82468GX_IFB_BATTERY_SIZE);
    memcpy(area, INTEL_82468GX_IFB_BATTERY_MAGIC, 8);
    memcpy(area + IFB_BATTERY_HEADER, s->rtc->cmos_data, IFB_RTC_BANK_SIZE);
    memcpy(area + IFB_BATTERY_HEADER + IFB_RTC_BANK_SIZE, s->rtc_ext_ram,
           IFB_RTC_BANK_SIZE);
}

/*
 * The banks are written through I/O ports that do not trap here, so compare
 * the banks with what the file holds and write only a change, once a second
 * while the machine runs and whenever it stops.  A ticking clock alone is
 * not a change.
 */
static void ifb_battery_flush(Intel82468GXIFBState *s)
{
    g_autofree uint8_t *area = g_malloc(INTEL_82468GX_IFB_BATTERY_SIZE);
    unsigned i;

    ifb_battery_image(s, area);
    for (i = 0; i < 2 * IFB_RTC_BANK_SIZE; i++) {
        if (ifb_battery_restores(i) &&
            area[IFB_BATTERY_HEADER + i] !=
            s->battery_shadow[IFB_BATTERY_HEADER + i]) {
            break;
        }
    }
    if (i == 2 * IFB_RTC_BANK_SIZE &&
        memcmp(area, s->battery_shadow, IFB_BATTERY_HEADER) == 0) {
        return;
    }
    memcpy(s->battery_shadow, area, INTEL_82468GX_IFB_BATTERY_SIZE);
    if (blk_pwrite(s->battery, 0, INTEL_82468GX_IFB_BATTERY_SIZE, area,
                   0) < 0) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "82468gx: cannot write the RTC battery area\n");
    }
}

static void ifb_battery_timer(void *opaque)
{
    Intel82468GXIFBState *s = opaque;

    ifb_battery_flush(s);
    timer_mod(s->battery_timer, qemu_clock_get_ms(QEMU_CLOCK_REALTIME) +
              IFB_BATTERY_PERIOD_MS);
}

static void ifb_battery_vm_state(void *opaque, bool running, RunState state)
{
    Intel82468GXIFBState *s = opaque;

    if (running) {
        timer_mod(s->battery_timer, qemu_clock_get_ms(QEMU_CLOCK_REALTIME) +
                  IFB_BATTERY_PERIOD_MS);
    } else {
        timer_del(s->battery_timer);
        ifb_battery_flush(s);
    }
}

static bool ifb_battery_load(Intel82468GXIFBState *s, Error **errp)
{
    g_autofree uint8_t *area = g_malloc(INTEL_82468GX_IFB_BATTERY_SIZE);
    unsigned i;

    if (blk_set_perm(s->battery, BLK_PERM_CONSISTENT_READ | BLK_PERM_WRITE,
                     BLK_PERM_ALL, errp) < 0 ||
        !blk_check_size_and_read_all(s->battery, DEVICE(s), area,
                                     INTEL_82468GX_IFB_BATTERY_SIZE, errp)) {
        return false;
    }
    if (memcmp(area, INTEL_82468GX_IFB_BATTERY_MAGIC, 8) == 0) {
        for (i = 0; i < 2 * IFB_RTC_BANK_SIZE; i++) {
            uint8_t byte = area[IFB_BATTERY_HEADER + i];

            if (!ifb_battery_restores(i)) {
                continue;
            }
            if (i < IFB_RTC_BANK_SIZE) {
                s->rtc->cmos_data[i] = byte;
            } else {
                s->rtc_ext_ram[i - IFB_RTC_BANK_SIZE] = byte;
            }
        }
    }
    /* A new battery reads as zeros and is written once the banks change. */
    s->battery_shadow = g_steal_pointer(&area);
    s->battery_timer = timer_new_ms(QEMU_CLOCK_REALTIME, ifb_battery_timer, s);
    s->battery_vmstate = qemu_add_vm_change_state_handler(ifb_battery_vm_state,
                                                          s);
    return true;
}

static void ifb_lpc_realize(PCIDevice *pci, Error **errp)
{
    Intel82468GXIFBState *s = INTEL_82468GX_IFB(pci);
    ISADevice *rtc;

    if (PCI_FUNC(pci->devfn) != 0 ||
        !(pci->config[PCI_HEADER_TYPE] & PCI_HEADER_TYPE_MULTI_FUNCTION)) {
        error_setg(errp, "%s must occupy function 0 of a multifunction slot",
                   TYPE_INTEL_82468GX_IFB);
        return;
    }

    ifb_lpc_init_config(pci);
    s->functions[0] = pci;
    s->isa_bus = isa_bus_new_non_default(DEVICE(s), pci_address_space(pci),
                                         pci_address_space_io(pci));
    s->pic_irqs = i8259_init_pair(s->isa_bus,
                                  qemu_allocate_irq(ifb_legacy_irq_handler,
                                                    s, 0),
                                  &s->master_pic);
    s->isa_irqs = qemu_allocate_irqs(ifb_isa_irq_handler, s,
                                     ISA_NUM_IRQS);
    isa_bus_register_input_irqs(s->isa_bus, s->isa_irqs);

    /*
     * The bridge carries the platform's three 82C54-equivalent counters as
     * one timer unit (SSDM 15.4): counter 0 drives IRQ 0, counter 1 the DRAM
     * refresh request and counter 2 the speaker tone, which port 0x61 gates.
     * IRQ 0 goes through the ISA input above, so it reaches both the 8259
     * pair and the platform's IOSAPIC line -- a firmware running the legacy
     * tick through ExtINT and an OS running it through the IOSAPIC see the
     * same counter.
     */
    s->pit = i8254_pit_init(s->isa_bus, 0x40, 0, NULL);
    memory_region_init_io(&s->nmisc, OBJECT(s), &ifb_nmisc_ops, s,
                          TYPE_INTEL_82468GX_IFB ".nmisc", 1);
    memory_region_add_subregion(pci_address_space_io(pci), IFB_NMISC_IOPORT,
                                &s->nmisc);

    /*
     * The bridge carries the RTC too (SSDM 15.5), both of its banks.  Bytes
     * 0Eh-7Fh of the standard bank are user RAM (Table 15-2): the part has
     * no century register, and firmware keeps the century at 32h.
     */
    rtc = isa_new(TYPE_MC146818_RTC);
    qdev_prop_set_int32(DEVICE(rtc), "base_year", 2000);
    qdev_prop_set_bit(DEVICE(rtc), "century-register", false);
    if (!isa_realize_and_unref(rtc, s->isa_bus, errp)) {
        return;
    }
    s->rtc = MC146818_RTC(rtc);
    isa_connect_gpio_out(rtc, 0, s->rtc->isairq);
    object_property_add_alias(qdev_get_machine(), "rtc-time", OBJECT(rtc),
                              "date");
    if (s->battery && !ifb_battery_load(s, errp)) {
        return;
    }
    memory_region_init_io(&s->rtc_ext, OBJECT(s), &ifb_rtc_ext_ops, s,
                          TYPE_INTEL_82468GX_IFB ".rtc-ext", 2);
    memory_region_init_alias(&s->rtc_ext_alias, OBJECT(s),
                             TYPE_INTEL_82468GX_IFB ".rtc-ext-alias",
                             &s->rtc->io, 0, 2);
    memory_region_add_subregion(pci_address_space_io(pci), IFB_RTC_EXT_IOPORT,
                                &s->rtc_ext);
    memory_region_add_subregion(pci_address_space_io(pci), IFB_RTC_EXT_IOPORT,
                                &s->rtc_ext_alias);

    memory_region_init(&s->acpi_pm, OBJECT(s),
                       TYPE_INTEL_82468GX_IFB ".acpi-pm",
                       IFB_ACPI_PM_IO_SIZE);
    memory_region_add_subregion(pci_address_space_io(pci), 0,
                                &s->acpi_pm);
    memory_region_set_enabled(&s->acpi_pm, false);
    s->sci_in = qemu_allocate_irq(ifb_sci_set, s, 0);
    acpi_pm1_evt_init(&s->acpi_regs, ifb_acpi_update_sci, &s->acpi_pm);
    acpi_pm1_cnt_init(&s->acpi_regs, &s->acpi_pm,
                      false, false, 4, false);
    /*
     * SLP_TYP 4 is S4/S5; 0 is ON, and so is any reserved value
     * (460GX SSDM 11.2.7.3).
     */
    s->acpi_regs.pm1.cnt.type0_on = true;
    acpi_pm_tmr_init(&s->acpi_regs, ifb_acpi_update_sci, &s->acpi_pm);
    acpi_gpe_init(&s->acpi_regs, IFB_ACPI_GPE_LENGTH);
    memory_region_init_io(&s->acpi_gpe, OBJECT(s), &ifb_acpi_gpe_ops, s,
                          TYPE_INTEL_82468GX_IFB ".acpi-gpe",
                          IFB_ACPI_GPE_LENGTH);
    memory_region_add_subregion(&s->acpi_pm, IFB_ACPI_GPE_OFFSET,
                                &s->acpi_gpe);
    memory_region_init_io(&s->acpi_smi, OBJECT(s), &ifb_acpi_smi_ops,
                          s, TYPE_INTEL_82468GX_IFB ".acpi-smi",
                          IFB_ACPI_SMI_LENGTH);
    memory_region_add_subregion(&s->acpi_pm, IFB_ACPI_SMI_OFFSET,
                                &s->acpi_smi);
    memory_region_init_io(&s->gpio, OBJECT(s), &ifb_gpio_ops, s,
                          TYPE_INTEL_82468GX_IFB ".gpio", IFB_GPIO_SIZE);
    memory_region_add_subregion(pci_address_space_io(pci), 0, &s->gpio);
    memory_region_set_enabled(&s->gpio, false);
    memory_region_init_io(&s->apm, OBJECT(s), &ifb_apm_ops, s,
                          TYPE_INTEL_82468GX_IFB ".apm", 2);
    memory_region_add_subregion(pci_address_space_io(pci), IFB_APM_IOPORT,
                                &s->apm);
    s->powerdown_notifier.notify = ifb_acpi_powerdown;
    qemu_register_powerdown_notifier(&s->powerdown_notifier);
    ifb_lpc_reset(DEVICE(pci));

    if (!ifb_realize_function(s, 1, "ide", TYPE_INTEL_82468GX_IFB_IDE,
                              errp) ||
        !ifb_realize_function(s, 2, "usb", TYPE_INTEL_82468GX_IFB_USB,
                              errp) ||
        !ifb_realize_function(s, 3, "smbus", TYPE_INTEL_82468GX_IFB_SMBUS,
                              errp)) {
        ifb_remove_function(&s->functions[3]);
        ifb_remove_function(&s->functions[2]);
        ifb_remove_function(&s->functions[1]);
        if (s->isa_irqs) {
            qemu_free_irqs(s->isa_irqs, ISA_NUM_IRQS);
            s->isa_irqs = NULL;
        }
        g_clear_pointer(&s->pic_irqs, g_free);
    }
}

static void ifb_lpc_exit(PCIDevice *pci)
{
    Intel82468GXIFBState *s = INTEL_82468GX_IFB(pci);

    if (s->battery_vmstate) {
        qemu_del_vm_change_state_handler(s->battery_vmstate);
        s->battery_vmstate = NULL;
    }
    g_clear_pointer(&s->battery_timer, timer_free);
    g_clear_pointer(&s->battery_shadow, g_free);

    ifb_remove_function(&s->functions[3]);
    ifb_remove_function(&s->functions[2]);
    ifb_remove_function(&s->functions[1]);
    if (s->isa_irqs) {
        qemu_free_irqs(s->isa_irqs, ISA_NUM_IRQS);
        s->isa_irqs = NULL;
    }
    g_clear_pointer(&s->pic_irqs, g_free);
    s->isa_bus = NULL;
    s->master_pic = NULL;
}

static int ifb_lpc_post_load(void *opaque, int version_id)
{
    Intel82468GXIFBState *s = opaque;
    uint16_t pm_enable = s->acpi_regs.pm1.evt.en;

    (void)version_id;
    qemu_system_wakeup_enable(
        QEMU_WAKEUP_REASON_RTC,
        (pm_enable & ACPI_BITMASK_RT_CLOCK_ENABLE) != 0);
    qemu_system_wakeup_enable(
        QEMU_WAKEUP_REASON_PMTIMER,
        (pm_enable & ACPI_BITMASK_TIMER_ENABLE) != 0);
    ifb_acpi_io_update(s);
    ifb_gpio_io_update(s);
    ifb_acpi_update_sci(&s->acpi_regs);
    return 0;
}

static const VMStateDescription vmstate_ifb_lpc = {
    .name = TYPE_INTEL_82468GX_IFB,
    .version_id = 5,
    .minimum_version_id = 1,
    .post_load = ifb_lpc_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_PCI_DEVICE(parent_obj, Intel82468GXIFBState),
        VMSTATE_UINT16(acpi_regs.pm1.evt.sts, Intel82468GXIFBState),
        VMSTATE_UINT16(acpi_regs.pm1.evt.en, Intel82468GXIFBState),
        VMSTATE_UINT16(acpi_regs.pm1.cnt.cnt, Intel82468GXIFBState),
        VMSTATE_TIMER_PTR(acpi_regs.tmr.timer, Intel82468GXIFBState),
        VMSTATE_INT64(acpi_regs.tmr.overflow_time, Intel82468GXIFBState),
        VMSTATE_BUFFER_POINTER_UNSAFE(acpi_regs.gpe.sts,
                                      Intel82468GXIFBState, 1,
                                      IFB_ACPI_GPE_LENGTH),
        VMSTATE_BUFFER_POINTER_UNSAFE(acpi_regs.gpe.en,
                                      Intel82468GXIFBState, 1,
                                      IFB_ACPI_GPE_LENGTH),
        VMSTATE_UINT8_V(nmisc_value, Intel82468GXIFBState, 2),
        VMSTATE_UINT8_V(rtc_ext_index, Intel82468GXIFBState, 2),
        VMSTATE_UINT8_ARRAY_V(rtc_ext_ram, Intel82468GXIFBState,
                              IFB_RTC_BANK_SIZE, 2),
        VMSTATE_UINT32_V(freq_mailbox, Intel82468GXIFBState, 2),
        VMSTATE_UINT16_V(glbctl, Intel82468GXIFBState, 3),
        VMSTATE_UINT8_V(apmc, Intel82468GXIFBState, 3),
        VMSTATE_UINT8_V(apms, Intel82468GXIFBState, 3),
        VMSTATE_UINT16_V(glbsts, Intel82468GXIFBState, 4),
        VMSTATE_BOOL_V(smi_asserted, Intel82468GXIFBState, 4),
        VMSTATE_UINT32_ARRAY_V(gp, Intel82468GXIFBState, IFB_GP_REGS, 5),
        VMSTATE_END_OF_LIST()
    },
};

MC146818RtcState *intel_82468gx_ifb_rtc(Intel82468GXIFBState *s)
{
    return s->rtc;
}

static void ifb_lpc_init(Object *obj)
{
    Intel82468GXIFBState *s = INTEL_82468GX_IFB(obj);

    qdev_init_gpio_out_named(DEVICE(obj), &s->legacy_irq,
                             INTEL_82468GX_IFB_GPIO_LEGACY, 1);
    qdev_init_gpio_out_named(DEVICE(obj), s->isa_irq,
                             INTEL_82468GX_IFB_GPIO_ISA_IRQ,
                             ISA_NUM_IRQS);
    qdev_init_gpio_out_named(DEVICE(obj), &s->sci,
                             INTEL_82468GX_IFB_GPIO_SCI, 1);
    qdev_init_gpio_out_named(DEVICE(obj), &s->smi,
                             INTEL_82468GX_IFB_GPIO_SMI, 1);
}

static const Property ifb_lpc_properties[] = {
    DEFINE_PROP_UINT16(INTEL_82468GX_IFB_PROP_INIT_ACPI_BASE,
                       Intel82468GXIFBState, init_acpi_base, 0),
    DEFINE_PROP_UINT32(INTEL_82468GX_IFB_PROP_GPIO_INPUTS,
                       Intel82468GXIFBState, gpio_inputs, UINT32_MAX),
    DEFINE_PROP_DRIVE(INTEL_82468GX_IFB_PROP_BATTERY, Intel82468GXIFBState,
                      battery),
};

static void ifb_lpc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *pc = PCI_DEVICE_CLASS(klass);

    pc->realize = ifb_lpc_realize;
    pc->exit = ifb_lpc_exit;
    pc->config_read = ifb_lpc_read_config;
    pc->config_write = ifb_lpc_write_config;
    pc->vendor_id = INTEL_82468GX_IFB_VENDOR_ID;
    pc->device_id = INTEL_82468GX_IFB_LPC_DEVICE_ID;
    pc->revision = INTEL_82468GX_IFB_REVISION;
    pc->class_id = PCI_CLASS_BRIDGE_ISA;
    dc->desc = "Intel 82468GX I/O and Firmware Bridge";
    dc->user_creatable = false;
    dc->hotpluggable = false;
    dc->vmsd = &vmstate_ifb_lpc;
    device_class_set_props(dc, ifb_lpc_properties);
    device_class_set_legacy_reset(dc, ifb_lpc_reset);
    set_bit(DEVICE_CATEGORY_BRIDGE, dc->categories);
}

static void ifb_smbus_set_irq(PMSMBus *smb, bool level)
{
    Intel82468GXSMBusState *s = smb->opaque;
    PCIDevice *pci = PCI_DEVICE(s);
    bool smi = pci->config[0x40] & BIT(1);

    pci_set_irq(pci, level && !smi);
    qemu_set_irq(s->smi, level && smi);
}

static void ifb_smbus_reset(DeviceState *dev)
{
    Intel82468GXSMBusState *s = INTEL_82468GX_IFB_SMBUS(dev);
    PCIDevice *pci = PCI_DEVICE(dev);

    pci_set_word(pci->config + PCI_COMMAND, 0);
    pci_set_word(pci->config + PCI_STATUS, 0x0280);
    pci->config[PCI_INTERRUPT_LINE] = 0;
    pci_config_set_interrupt_pin(pci->config, 2);
    memset(pci->config + 0x40, 0, 4);
    if (s->smb.reset) {
        s->smb.reset(&s->smb);
    }
    ifb_smbus_set_irq(&s->smb, false);
}

static void ifb_smbus_write_config(PCIDevice *pci, uint32_t address,
                                    uint32_t value, int length)
{
    Intel82468GXSMBusState *s = INTEL_82468GX_IFB_SMBUS(pci);

    pci_default_write_config(pci, address, value, length);
    ifb_smbus_set_irq(&s->smb, s->smb.smb_stat & 0x1e);
}

static void ifb_smbus_realize(PCIDevice *pci, Error **errp)
{
    Intel82468GXSMBusState *s = INTEL_82468GX_IFB_SMBUS(pci);

    (void)errp;
    memset(pci->wmask, 0, pci_config_size(pci));
    memset(pci->w1cmask, 0, pci_config_size(pci));
    pci_set_word(pci->wmask + PCI_COMMAND, BIT(3) | BIT(1) | BIT(0));
    pci_set_word(pci->w1cmask + PCI_STATUS, BIT(11));
    pci->wmask[PCI_LATENCY_TIMER] = 0;
    pci->wmask[PCI_INTERRUPT_LINE] = 0xff;
    pci->wmask[0x40] = 0x03;
    memset(pci->wmask + 0x41, 0xff, 3);

    pm_smbus_init(DEVICE(s), &s->smb, false);
    s->smb.opaque = s;
    s->smb.set_irq = ifb_smbus_set_irq;
    memory_region_init_alias(&s->io_alias, OBJECT(s),
                             TYPE_INTEL_82468GX_IFB_SMBUS ".io",
                             &s->smb.io, 0, 0x10);
    pci_register_bar(pci, 4, PCI_BASE_ADDRESS_SPACE_IO, &s->io_alias);
    ifb_smbus_reset(DEVICE(pci));
}

static int ifb_smbus_post_load(void *opaque, int version_id)
{
    Intel82468GXSMBusState *s = opaque;

    (void)version_id;
    ifb_smbus_set_irq(&s->smb, s->smb.smb_stat & 0x1e);
    return 0;
}

static const VMStateDescription vmstate_ifb_smbus = {
    .name = TYPE_INTEL_82468GX_IFB_SMBUS,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = ifb_smbus_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_PCI_DEVICE(parent_obj, Intel82468GXSMBusState),
        VMSTATE_STRUCT(smb, Intel82468GXSMBusState, 1,
                       pmsmb_vmstate, PMSMBus),
        VMSTATE_END_OF_LIST()
    },
};

static void ifb_smbus_init(Object *obj)
{
    Intel82468GXSMBusState *s = INTEL_82468GX_IFB_SMBUS(obj);

    qdev_init_gpio_out_named(DEVICE(obj), &s->smi,
                             INTEL_82468GX_IFB_GPIO_SMI, 1);
}

static void ifb_smbus_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *pc = PCI_DEVICE_CLASS(klass);

    pc->realize = ifb_smbus_realize;
    pc->config_write = ifb_smbus_write_config;
    pc->vendor_id = INTEL_82468GX_IFB_VENDOR_ID;
    pc->device_id = INTEL_82468GX_IFB_SMBUS_DEVICE_ID;
    pc->revision = INTEL_82468GX_IFB_REVISION;
    pc->class_id = PCI_CLASS_SERIAL_SMBUS;
    dc->desc = "Intel 82468GX SMBus controller";
    dc->user_creatable = false;
    dc->hotpluggable = false;
    dc->vmsd = &vmstate_ifb_smbus;
    device_class_set_legacy_reset(dc, ifb_smbus_reset);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo intel_82468gx_ifb_types[] = {
    {
        .name = TYPE_INTEL_82468GX_IFB,
        .parent = TYPE_PCI_DEVICE,
        .instance_size = sizeof(Intel82468GXIFBState),
        .instance_init = ifb_lpc_init,
        .class_init = ifb_lpc_class_init,
        .interfaces = (const InterfaceInfo[]) {
            { INTERFACE_CONVENTIONAL_PCI_DEVICE },
            { },
        },
    }, {
        .name = TYPE_INTEL_82468GX_IFB_SMBUS,
        .parent = TYPE_PCI_DEVICE,
        .instance_size = sizeof(Intel82468GXSMBusState),
        .instance_init = ifb_smbus_init,
        .class_init = ifb_smbus_class_init,
        .interfaces = (const InterfaceInfo[]) {
            { INTERFACE_CONVENTIONAL_PCI_DEVICE },
            { },
        },
    },
};

DEFINE_TYPES(intel_82468gx_ifb_types)

Intel82468GXIFBState *intel_82468gx_ifb_create(PCIBus *bus, int devfn,
                                               uint32_t gpio_inputs,
                                               BlockBackend *battery,
                                               uint16_t init_acpi_base,
                                               Error **errp)
{
    PCIDevice *pci;

    if (!bus) {
        error_setg(errp, "82468GX IFB requires a PCI bus");
        return NULL;
    }
    if (devfn < 0 || devfn >= PCI_DEVFN_MAX || PCI_FUNC(devfn) != 0) {
        error_setg(errp, "82468GX IFB requires a function-zero devfn");
        return NULL;
    }

    pci = pci_new_multifunction(devfn, TYPE_INTEL_82468GX_IFB);
    qdev_prop_set_uint16(DEVICE(pci), INTEL_82468GX_IFB_PROP_INIT_ACPI_BASE,
                         init_acpi_base);
    qdev_prop_set_uint32(DEVICE(pci), INTEL_82468GX_IFB_PROP_GPIO_INPUTS,
                         gpio_inputs);
    if (battery) {
        qdev_prop_set_drive_err(DEVICE(pci), INTEL_82468GX_IFB_PROP_BATTERY,
                                battery, &error_abort);
    }
    if (!pci_realize_and_unref(pci, bus, errp)) {
        return NULL;
    }
    return INTEL_82468GX_IFB(pci);
}

PCIDevice *intel_82468gx_ifb_function(Intel82468GXIFBState *s,
                                      unsigned function)
{
    return s && function < INTEL_82468GX_IFB_FUNCTIONS ?
           s->functions[function] : NULL;
}

ISABus *intel_82468gx_ifb_isa_bus(Intel82468GXIFBState *s)
{
    return s ? s->isa_bus : NULL;
}

IDEBus *intel_82468gx_ifb_ide_bus(Intel82468GXIFBState *s,
                                  unsigned channel)
{
    PCIDevice *pci;

    if (!s || channel >= 2) {
        return NULL;
    }
    pci = s->functions[1];
    return pci ? &PCI_IDE(pci)->bus[channel] : NULL;
}

I2CBus *intel_82468gx_ifb_smbus(Intel82468GXIFBState *s)
{
    Intel82468GXSMBusState *sm;

    if (!s || !s->functions[3]) {
        return NULL;
    }
    sm = INTEL_82468GX_IFB_SMBUS(s->functions[3]);
    return sm->smb.smbus;
}

int intel_82468gx_ifb_pic_read_irq(Intel82468GXIFBState *s)
{
    return s && s->master_pic ? pic_read_irq(s->master_pic) : -1;
}

void intel_82468gx_ifb_configure_acpi(Intel82468GXIFBState *s,
                                      uint16_t io_base)
{
    PCIDevice *pci;

    g_return_if_fail(s != NULL);
    g_return_if_fail(io_base != 0 && (io_base & 0x3fU) == 0);
    pci = PCI_DEVICE(s);
    ifb_lpc_write_config(pci, 0x40, io_base | 1U, 4);
    ifb_lpc_write_config(pci, 0x44, 1U, 1);
}
