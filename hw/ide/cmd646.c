/*
 * QEMU IDE Emulation: PCI cmd646 support.
 *
 * Copyright (c) 2003 Fabrice Bellard
 * Copyright (c) 2006 Openedhand Ltd.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu/osdep.h"
#include "hw/pci/pci.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "qemu/module.h"
#include "hw/isa/isa.h"
#include "system/dma.h"
#include "system/reset.h"

#include "hw/ide/pci.h"
#include "ide-internal.h"
#include "trace.h"

/* CMD646 specific */
#define CFR                  0x50
#define   CFR_INTR_CH0       0x04
#define CNTRL                0x51
#define   CNTRL_EN_CH0       0x04
#define   CNTRL_EN_CH1       0x08
#define ARTTIM23             0x57
#define    ARTTIM23_INTR_CH1 0x10
#define MRDMODE              0x71
#define   MRDMODE_INTR_CH0   0x04
#define   MRDMODE_INTR_CH1   0x08
#define   MRDMODE_BLK_CH0    0x10
#define   MRDMODE_BLK_CH1    0x20
#define UDIDETCR0            0x73
#define UDIDETCR1            0x7B

/* CMD649 (PCI-649 Product Specification, Rev. 1.2, chapter 6) */
#define SUBCONF              0x4F
#define   SUBCONF_SUBSYS_WE  0x01
#define   SUBCONF_JP7        0x02
#define   SUBCONF_CLASS_WE   0x04
#define CFR_JP1_NATIVE       0x40
#define CMDTIM               0x52
#define ARTTIM0              0x53
#define BRST                 0x59
#define CMD649_PM_CAP        0x60
#define CMD649_BM_FIRST      0x70
#define CMD649_BM_LAST       0x7F
#define BMIDECSR             0x79
#define CMD649_SUBSYS_SHADOW 0x8C

static bool cmd646_is_649(PCIDevice *pd)
{
    return pci_get_word(pd->config + PCI_DEVICE_ID) == PCI_DEVICE_ID_CMD_649;
}

static void cmd646_update_irq(PCIDevice *pd);

static void cmd646_update_dma_interrupts(PCIDevice *pd)
{
    /* Sync DMA interrupt status from UDMA interrupt status */
    if (pd->config[MRDMODE] & MRDMODE_INTR_CH0) {
        pd->config[CFR] |= CFR_INTR_CH0;
    } else {
        pd->config[CFR] &= ~CFR_INTR_CH0;
    }

    if (pd->config[MRDMODE] & MRDMODE_INTR_CH1) {
        pd->config[ARTTIM23] |= ARTTIM23_INTR_CH1;
    } else {
        pd->config[ARTTIM23] &= ~ARTTIM23_INTR_CH1;
    }
}

static void cmd646_update_udma_interrupts(PCIDevice *pd)
{
    /* Sync UDMA interrupt status from DMA interrupt status */
    if (pd->config[CFR] & CFR_INTR_CH0) {
        pd->config[MRDMODE] |= MRDMODE_INTR_CH0;
    } else {
        pd->config[MRDMODE] &= ~MRDMODE_INTR_CH0;
    }

    if (pd->config[ARTTIM23] & ARTTIM23_INTR_CH1) {
        pd->config[MRDMODE] |= MRDMODE_INTR_CH1;
    } else {
        pd->config[MRDMODE] &= ~MRDMODE_INTR_CH1;
    }
}

static uint64_t bmdma_read(void *opaque, hwaddr addr,
                           unsigned size)
{
    BMDMAState *bm = opaque;
    PCIDevice *pci_dev = PCI_DEVICE(bm->pci_dev);
    uint32_t val;

    if (size != 1) {
        return ((uint64_t)1 << (size * 8)) - 1;
    }

    switch(addr & 3) {
    case 0:
        val = bm->cmd;
        break;
    case 1:
        /* Base Address #4 + 09h is BMIDECSR on the CMD649 (6.8). */
        if (cmd646_is_649(pci_dev) && bm == &bm->pci_dev->bmdma[1]) {
            val = pci_dev->config[BMIDECSR];
        } else {
            val = pci_dev->config[MRDMODE];
        }
        break;
    case 2:
        val = bm->status;
        break;
    case 3:
        if (bm == &bm->pci_dev->bmdma[0]) {
            val = pci_dev->config[UDIDETCR0];
        } else {
            val = pci_dev->config[UDIDETCR1];
        }
        break;
    default:
        val = 0xff;
        break;
    }

    trace_bmdma_read_cmd646(addr, val);
    return val;
}

static void bmdma_write(void *opaque, hwaddr addr,
                        uint64_t val, unsigned size)
{
    BMDMAState *bm = opaque;
    PCIDevice *pci_dev = PCI_DEVICE(bm->pci_dev);

    if (size != 1) {
        return;
    }

    trace_bmdma_write_cmd646(addr, val);
    switch(addr & 3) {
    case 0:
        bmdma_cmd_writeb(bm, val);
        break;
    case 1:
        if (cmd646_is_649(pci_dev) && bm == &bm->pci_dev->bmdma[1]) {
            pci_dev->config[BMIDECSR] = (pci_dev->config[BMIDECSR] & 0x0f) |
                                        (val & 0xf0);
            break;
        }
        {
            uint8_t wm = cmd646_is_649(pci_dev) ? 0xf0 : 0x30;

            pci_dev->config[MRDMODE] =
                (pci_dev->config[MRDMODE] & ~wm) | (val & wm);
        }
        cmd646_update_dma_interrupts(pci_dev);
        cmd646_update_irq(pci_dev);
        break;
    case 2:
        bmdma_status_writeb(bm, val);
        break;
    case 3:
        if (bm == &bm->pci_dev->bmdma[0]) {
            pci_dev->config[UDIDETCR0] = val;
        } else {
            pci_dev->config[UDIDETCR1] = val;
        }
        break;
    }
}

static const MemoryRegionOps cmd646_bmdma_ops = {
    .read = bmdma_read,
    .write = bmdma_write,
};

static void bmdma_setup_bar(PCIIDEState *d)
{
    BMDMAState *bm;
    int i;

    memory_region_init(&d->bmdma_bar, OBJECT(d), "cmd646-bmdma", 16);
    for(i = 0;i < 2; i++) {
        bm = &d->bmdma[i];
        memory_region_init_io(&bm->extra_io, OBJECT(d), &cmd646_bmdma_ops, bm,
                              "cmd646-bmdma-bus", 4);
        memory_region_add_subregion(&d->bmdma_bar, i * 8, &bm->extra_io);
        memory_region_init_io(&bm->addr_ioport, OBJECT(d),
                              &bmdma_addr_ioport_ops, bm,
                              "cmd646-bmdma-ioport", 4);
        memory_region_add_subregion(&d->bmdma_bar, i * 8 + 4, &bm->addr_ioport);
    }
}

static void cmd646_update_irq(PCIDevice *pd)
{
    int pci_level;

    pci_level = ((pd->config[MRDMODE] & MRDMODE_INTR_CH0) &&
                 !(pd->config[MRDMODE] & MRDMODE_BLK_CH0)) ||
        ((pd->config[MRDMODE] & MRDMODE_INTR_CH1) &&
         !(pd->config[MRDMODE] & MRDMODE_BLK_CH1));
    pci_set_irq(pd, pci_level);
}

/* the PCI irq level is the logical OR of the two channels */
static void cmd646_set_irq(void *opaque, int channel, int level)
{
    PCIIDEState *d = opaque;
    PCIDevice *pd = PCI_DEVICE(d);
    int irq_mask;

    irq_mask = MRDMODE_INTR_CH0 << channel;
    if (level) {
        pd->config[MRDMODE] |= irq_mask;
        /*
         * The real PCI0646 also latches every IDE-bus interrupt into the
         * per-channel Bus Master IDE status register (BMIDESR bit 2,
         * "Interrupt", Base Address #4 + 02h; CMD PCI0646 spec, Bus Master
         * IDE Status Register).  It is sticky and cleared by the driver
         * writing 1 to it.  Intel's Bus Master IDE programming interface (and
         * hence Linux' generic ide_dma_end(), which requires
         * (dma_stat & 7) == 4) relies on this bit to acknowledge a bus-master
         * transfer, so mirror the standard bmdma_irq() behaviour here.
         */
        d->bmdma[channel].status |= BM_STATUS_INT;
    } else {
        pd->config[MRDMODE] &= ~irq_mask;
    }
    cmd646_update_dma_interrupts(pd);
    cmd646_update_irq(pd);
}

/* The CMD649's reset values (6.1-6.10); the timings are kept, not applied. */
static void cmd649_reset_regs(PCIDevice *pd)
{
    PCIIDEState *d = PCI_IDE(pd);
    uint8_t *c = pd->config;

    pci_set_word(c + PCI_SUBSYSTEM_VENDOR_ID, PCI_VENDOR_ID_CMD);
    pci_set_word(c + PCI_SUBSYSTEM_ID, PCI_DEVICE_ID_CMD_649);
    memcpy(c + CMD649_SUBSYS_SHADOW, c + PCI_SUBSYSTEM_VENDOR_ID, 4);
    c[SUBCONF] = SUBCONF_JP7;
    c[CFR] = CFR_JP1_NATIVE;
    c[CNTRL] = 0xe0 | CNTRL_EN_CH0 | (d->secondary ? CNTRL_EN_CH1 : 0);
    memset(c + CMDTIM, 0, 0x5c - CMDTIM);
    c[ARTTIM0] = 0x80;
    c[ARTTIM0 + 2] = 0x80;
    c[ARTTIM23] = 0x8c;
    c[BRST] = 0x40;
    c[MRDMODE] = 0;
    c[UDIDETCR0] = 0xf0;
    c[UDIDETCR1] = 0xf0;
    /* Write FIFO threshold 1/2; no 80-conductor cable (rx2600, 00:02.0). */
    c[BMIDECSR] = 0x80;
    pci_set_word(c + CMD649_PM_CAP + PCI_PM_CTRL, 0x6000);
}

static void cmd646_reset(DeviceState *dev)
{
    PCIIDEState *d = PCI_IDE(dev);
    unsigned int i;

    for (i = 0; i < 2; i++) {
        ide_bus_reset(&d->bus[i]);
    }
    if (cmd646_is_649(PCI_DEVICE(dev))) {
        cmd649_reset_regs(PCI_DEVICE(dev));
    }
}

/* The CMD649 decodes its bus master registers at 70h-7Fh too (6.8-6.10). */
static uint8_t cmd649_bm_config_byte(PCIDevice *pd, uint32_t addr)
{
    BMDMAState *bm = &PCI_IDE(pd)->bmdma[(addr - CMD649_BM_FIRST) >> 3];
    unsigned int off = addr & 7;

    switch (off) {
    case 0:
        return bm->cmd;
    case 2:
        return bm->status;
    case 1:
    case 3:
        return pd->config[addr];
    default:
        return bm->addr >> ((off - 4) * 8);
    }
}

static void cmd649_config_write(PCIDevice *pd, uint32_t addr, uint32_t val,
                                int len)
{
    int i;

    for (i = 0; i < len; i++) {
        uint32_t a = addr + i;
        uint8_t v = val >> (i * 8);

        if (a >= PCI_SUBSYSTEM_VENDOR_ID && a < PCI_SUBSYSTEM_VENDOR_ID + 4) {
            /* Writable only with SUBCONF bit 0; 8Ch-8Fh mirror them. */
            if (pd->config[SUBCONF] & SUBCONF_SUBSYS_WE) {
                pd->config[a] = v;
                pd->config[a - PCI_SUBSYSTEM_VENDOR_ID +
                           CMD649_SUBSYS_SHADOW] = v;
            }
        } else if (a >= CMD649_SUBSYS_SHADOW && a < CMD649_SUBSYS_SHADOW + 4) {
            pd->config[a - CMD649_SUBSYS_SHADOW + PCI_SUBSYSTEM_VENDOR_ID] =
                pd->config[a];
        } else if (a >= CMD649_BM_FIRST && a <= CMD649_BM_LAST) {
            BMDMAState *bm = &PCI_IDE(pd)->bmdma[(a - CMD649_BM_FIRST) >> 3];
            unsigned int off = a & 7;

            if (off == 0) {
                bmdma_cmd_writeb(bm, v);
            } else if (off == 2) {
                bmdma_status_writeb(bm, v);
            } else if (off >= 4) {
                unsigned int shift = (off - 4) * 8;

                bm->addr = ((bm->addr & ~(0xffu << shift)) |
                            ((uint32_t)v << shift)) & ~3u;
            }
        }
    }
}

static uint32_t cmd646_pci_config_read(PCIDevice *d,
                                       uint32_t address, int len)
{
    uint32_t val = pci_default_read_config(d, address, len);
    int i;

    if (!cmd646_is_649(d)) {
        return val;
    }
    for (i = 0; i < len; i++) {
        uint32_t a = address + i;

        if (a >= CMD649_BM_FIRST && a <= CMD649_BM_LAST) {
            val &= ~(0xffu << (i * 8));
            val |= (uint32_t)cmd649_bm_config_byte(d, a) << (i * 8);
        }
    }
    return val;
}

static void cmd646_pci_config_write(PCIDevice *d, uint32_t addr, uint32_t val,
                                    int l)
{
    uint32_t i;

    pci_default_write_config(d, addr, val, l);
    if (cmd646_is_649(d)) {
        cmd649_config_write(d, addr, val, l);
    }

    for (i = addr; i < addr + l; i++) {
        switch (i) {
        case CFR:
        case ARTTIM23:
            cmd646_update_udma_interrupts(d);
            break;
        case MRDMODE:
            cmd646_update_dma_interrupts(d);
            break;
        }
    }

    cmd646_update_irq(d);
}

/*
 * PCI-649 chapter 6: Min_Gnt 02h, Max_Lat 04h, a power management capability
 * at 60h (D1, D2, DSI; Data Register F0h at scale 11b) and writable timing
 * registers.  The programming interface stays 8Fh: the model has no
 * compatibility mode.
 */
static bool cmd649_init(PCIDevice *dev, Error **errp)
{
    uint8_t *c = dev->config;
    int i;

    c[PCI_MIN_GNT] = 0x02;
    c[PCI_MAX_LAT] = 0x04;
    pci_set_word(c + PCI_STATUS, pci_get_word(c + PCI_STATUS) |
                 PCI_STATUS_FAST_BACK | PCI_STATUS_DEVSEL_MEDIUM);
    /*
     * The rx2600 reads cache line size 00h after its firmware wrote 20h, and
     * the latency timer Linux set.  Reserved registers read 0 and ignore
     * writes (chapter 1, PCI Configuration Space).
     */
    dev->wmask[PCI_CACHE_LINE_SIZE] = 0;
    dev->wmask[PCI_LATENCY_TIMER] = 0xff;
    memset(dev->wmask + PCI_CONFIG_HEADER_SIZE, 0,
           PCI_CONFIG_SPACE_SIZE - PCI_CONFIG_HEADER_SIZE);
    if (pci_pm_init(dev, CMD649_PM_CAP, errp) < 0) {
        return false;
    }
    pci_set_word(c + CMD649_PM_CAP + PCI_PM_PMC, 0x0622);
    pci_set_word(dev->wmask + CMD649_PM_CAP + PCI_PM_CTRL,
                 PCI_PM_CTRL_STATE_MASK | PCI_PM_CTRL_DATA_SEL_MASK);
    c[CMD649_PM_CAP + PCI_PM_DATA_REGISTER] = 0xf0;

    dev->wmask[SUBCONF] = SUBCONF_SUBSYS_WE | SUBCONF_CLASS_WE;
    dev->wmask[CNTRL] = 0xcc;
    for (i = CMDTIM; i < 0x5c; i++) {
        dev->wmask[i] = 0xff;
    }
    dev->wmask[ARTTIM0] = 0xc0;
    dev->wmask[ARTTIM0 + 2] = 0xc0;
    dev->wmask[ARTTIM23] = 0xcc;
    dev->wmask[0x5a] = 0;
    dev->wmask[MRDMODE] = 0xf0;
    dev->wmask[UDIDETCR0] = 0xff;
    dev->wmask[UDIDETCR1] = 0xff;
    dev->wmask[BMIDECSR] = 0xf0;
    memset(dev->wmask + CMD649_SUBSYS_SHADOW, 0xff, 4);
    cmd649_reset_regs(dev);
    return true;
}

/* CMD646 PCI IDE controller */
static void pci_cmd646_ide_realize(PCIDevice *dev, Error **errp)
{
    PCIIDEState *d = PCI_IDE(dev);
    DeviceState *ds = DEVICE(dev);
    uint8_t *pci_conf = dev->config;
    int i;

    pci_conf[PCI_CLASS_PROG] = 0x8f;

    pci_conf[CNTRL] = CNTRL_EN_CH0; // enable IDE0
    if (d->secondary) {
        /* XXX: if not enabled, really disable the secondary IDE controller */
        pci_conf[CNTRL] |= CNTRL_EN_CH1; /* enable IDE1 */
    }

    /* Set write-to-clear interrupt bits */
    dev->wmask[CFR] = 0x0;
    dev->w1cmask[CFR] = CFR_INTR_CH0;
    dev->wmask[ARTTIM23] = 0x0;
    dev->w1cmask[ARTTIM23] = ARTTIM23_INTR_CH1;
    dev->wmask[MRDMODE] = 0x0;
    dev->w1cmask[MRDMODE] = MRDMODE_INTR_CH0 | MRDMODE_INTR_CH1;

    memory_region_init_io(&d->data_bar[0], OBJECT(d), &pci_ide_data_le_ops,
                          &d->bus[0], "cmd646-data0", 8);
    pci_register_bar(dev, 0, PCI_BASE_ADDRESS_SPACE_IO, &d->data_bar[0]);

    memory_region_init_io(&d->cmd_bar[0], OBJECT(d), &pci_ide_cmd_le_ops,
                          &d->bus[0], "cmd646-cmd0", 4);
    pci_register_bar(dev, 1, PCI_BASE_ADDRESS_SPACE_IO, &d->cmd_bar[0]);

    memory_region_init_io(&d->data_bar[1], OBJECT(d), &pci_ide_data_le_ops,
                          &d->bus[1], "cmd646-data1", 8);
    pci_register_bar(dev, 2, PCI_BASE_ADDRESS_SPACE_IO, &d->data_bar[1]);

    memory_region_init_io(&d->cmd_bar[1], OBJECT(d), &pci_ide_cmd_le_ops,
                          &d->bus[1], "cmd646-cmd1", 4);
    pci_register_bar(dev, 3, PCI_BASE_ADDRESS_SPACE_IO, &d->cmd_bar[1]);

    bmdma_setup_bar(d);
    pci_register_bar(dev, 4, PCI_BASE_ADDRESS_SPACE_IO, &d->bmdma_bar);

    /* TODO: RST# value should be 0 */
    pci_conf[PCI_INTERRUPT_PIN] = 0x01; // interrupt on pin 1

    qdev_init_gpio_in(ds, cmd646_set_irq, 2);
    for (i = 0; i < 2; i++) {
        ide_bus_init(&d->bus[i], sizeof(d->bus[i]), ds, i, 2);
        ide_bus_init_output_irq(&d->bus[i], qdev_get_gpio_in(ds, i));

        bmdma_init(&d->bus[i], &d->bmdma[i], d);
        ide_bus_register_restart_cb(&d->bus[i]);
    }

    if (cmd646_is_649(dev) && !cmd649_init(dev, errp)) {
        return;
    }
}

static void pci_cmd646_ide_exitfn(PCIDevice *dev)
{
    PCIIDEState *d = PCI_IDE(dev);
    unsigned i;

    for (i = 0; i < 2; ++i) {
        memory_region_del_subregion(&d->bmdma_bar, &d->bmdma[i].extra_io);
        memory_region_del_subregion(&d->bmdma_bar, &d->bmdma[i].addr_ioport);
    }
}

static const Property cmd646_ide_properties[] = {
    DEFINE_PROP_UINT32("secondary", PCIIDEState, secondary, 0),
};

static void cmd646_ide_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, cmd646_reset);
    dc->vmsd = &vmstate_ide_pci;
    k->realize = pci_cmd646_ide_realize;
    k->exit = pci_cmd646_ide_exitfn;
    k->vendor_id = PCI_VENDOR_ID_CMD;
    k->device_id = PCI_DEVICE_ID_CMD_646;
    k->revision = 0x07;
    k->class_id = PCI_CLASS_STORAGE_IDE;
    k->config_read = cmd646_pci_config_read;
    k->config_write = cmd646_pci_config_write;
    device_class_set_props(dc, cmd646_ide_properties);
    set_bit(DEVICE_CATEGORY_STORAGE, dc->categories);
}

static const TypeInfo cmd646_ide_info = {
    .name          = "cmd646-ide",
    .parent        = TYPE_PCI_IDE,
    .class_init    = cmd646_ide_class_init,
};

/*
 * The HP rx2600's core I/O IDE is a CMD649, revision 02h (Rev C) with both
 * channels native (programming interface 8Fh, rx2600 capture 2026-10-04,
 * 00:02.0), subsystem 1095:0649.
 */
static void cmd649_ide_class_init(ObjectClass *klass, const void *data)
{
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->device_id = PCI_DEVICE_ID_CMD_649;
    k->revision = 0x02;
    k->subsystem_vendor_id = PCI_VENDOR_ID_CMD;
    k->subsystem_id = PCI_DEVICE_ID_CMD_649;
}

static const TypeInfo cmd649_ide_info = {
    .name          = "cmd649-ide",
    .parent        = "cmd646-ide",
    .class_init    = cmd649_ide_class_init,
};

static void cmd646_ide_register_types(void)
{
    type_register_static(&cmd646_ide_info);
    type_register_static(&cmd649_ide_info);
}

type_init(cmd646_ide_register_types)
