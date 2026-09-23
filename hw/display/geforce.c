/*
 * NVIDIA GeForce2 (NV15 / Quadro2 Pro NV15GL) PCI display device.
 *
 * Core: PCI/AGP config, MMIO register file, VRAM/RAMIN/DMA/RAMHT/FIFO, IRQ,
 * timers, legacy + extended CRTC I/O, and the QEMU display path.
 *
 * Ported to QEMU from the Bochs Project's bx_geforce_c SVGA device
 * (Copyright (C) 2025-2026 The Bochs Project, LGPL v2+).  Only the NV15
 * (card_type 0x15) code path is retained; the NV20/NV35/NV40 code of the
 * original is dropped.
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "hw/pci/pci.h"
#include "hw/core/qdev-properties.h"
#include "qapi/error.h"
#include "migration/vmstate.h"
#include "ui/console.h"
#include "ui/pixel_ops.h"
#include "hw/display/i2c-ddc.h"
#include "hw/i2c/i2c.h"
#include "hw/i2c/bitbang_i2c.h"
#include "system/memory.h"
#include "framebuffer.h"
#include "vga_int.h"
#include "geforce.h"
#include "geforce_pxextract.h"

#define PCI_VENDOR_ID_NVIDIA_LOCAL 0x10de

/* Refresh / vblank cadence. */
#define NV_VBLANK_HZ 60

/* NV_TRACE (GEFORCE_LOG diagnostic trace) is defined in geforce.h. */

static const VMStateDescription vmstate_nv15 = {
    .name = "nv15gl-vga",
    .version_id = 1,
    .minimum_version_id = 1,
    /*
     * The full GPU runtime state (PMC/PFIFO/PGRAPH/PTIMER/PCRTC/PRAMDAC
     * registers, the per-channel 2D/3D state, cursor, timers and the base
     * VGACommonState) is not serialized yet.  Migrating/snapshotting with
     * only the PCI config + VRAM would silently restore a corrupt device,
     * so mark it unmigratable until a complete vmstate is written.
     */
    .unmigratable = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_PCI_DEVICE(parent_obj, NV15State),
        VMSTATE_END_OF_LIST()
    }
};

/* forward */
static uint32_t nv_register_read32(NV15State *s, uint32_t address);
static void nv_register_write32(NV15State *s, uint32_t address, uint32_t value);
static uint8_t nv_register_read8(NV15State *s, uint32_t address);
static void nv_register_write8(NV15State *s, uint32_t address, uint8_t value);
static uint32_t nv_svga_read(NV15State *s, uint32_t address, unsigned io_len);
static void nv_svga_write(NV15State *s, uint32_t address, uint32_t value,
                          unsigned io_len);
static void nv_svga_write_crtc(NV15State *s, unsigned index, uint8_t value);
static uint8_t nv_svga_read_crtc(NV15State *s, unsigned index);
static void nv_fifo_process(NV15State *s);
static void nv_update_irq_level(NV15State *s);
static int nv_execute_command(NV15State *s, uint32_t chid, uint32_t subc,
                              uint32_t method, uint32_t param);

/* ===================================================================== */
/* Timers / time source                                                  */
/* ===================================================================== */

uint64_t nv_get_current_time(NV15State *s)
{
    return (s->timer_inittime1 +
            qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - s->timer_inittime2) &
           ~(uint64_t)0x1F;
}

/* ===================================================================== */
/* VRAM / RAMIN                                                          */
/* ===================================================================== */

uint8_t nv_vram_read8(NV15State *s, uint32_t address)
{
    return s->vga.vram_ptr[address & s->memsize_mask];
}

uint16_t nv_vram_read16(NV15State *s, uint32_t address)
{
    /* Mask every byte, not just the base: an access at memsize_mask must
     * wrap inside the aperture rather than run past the VRAM RAMBlock. */
    return s->vga.vram_ptr[(address + 0) & s->memsize_mask] << 0 |
           s->vga.vram_ptr[(address + 1) & s->memsize_mask] << 8;
}

uint32_t nv_vram_read32(NV15State *s, uint32_t address)
{
    return (uint32_t)s->vga.vram_ptr[(address + 0) & s->memsize_mask] << 0 |
           (uint32_t)s->vga.vram_ptr[(address + 1) & s->memsize_mask] << 8 |
           (uint32_t)s->vga.vram_ptr[(address + 2) & s->memsize_mask] << 16 |
           (uint32_t)s->vga.vram_ptr[(address + 3) & s->memsize_mask] << 24;
}

uint64_t nv_vram_read64(NV15State *s, uint32_t address)
{
    return (uint64_t)s->vga.vram_ptr[(address + 0) & s->memsize_mask] << 0 |
           (uint64_t)s->vga.vram_ptr[(address + 1) & s->memsize_mask] << 8 |
           (uint64_t)s->vga.vram_ptr[(address + 2) & s->memsize_mask] << 16 |
           (uint64_t)s->vga.vram_ptr[(address + 3) & s->memsize_mask] << 24 |
           (uint64_t)s->vga.vram_ptr[(address + 4) & s->memsize_mask] << 32 |
           (uint64_t)s->vga.vram_ptr[(address + 5) & s->memsize_mask] << 40 |
           (uint64_t)s->vga.vram_ptr[(address + 6) & s->memsize_mask] << 48 |
           (uint64_t)s->vga.vram_ptr[(address + 7) & s->memsize_mask] << 56;
}

static void nv_vram_touch(NV15State *s, uint32_t address, uint32_t len)
{
    address &= s->memsize_mask;
    if (len > s->memsize_mask + 1 - address) {
        len = s->memsize_mask + 1 - address;
    }
    memory_region_set_dirty(&s->vga.vram, address, len);
}

void nv_vram_write8(NV15State *s, uint32_t address, uint8_t value)
{
    address &= s->memsize_mask;
    s->vga.vram_ptr[address] = value;
    nv_vram_touch(s, address, 1);
}

void nv_vram_write16(NV15State *s, uint32_t address, uint16_t value)
{
    s->vga.vram_ptr[(address + 0) & s->memsize_mask] = (value >> 0) & 0xFF;
    s->vga.vram_ptr[(address + 1) & s->memsize_mask] = (value >> 8) & 0xFF;
    nv_vram_touch(s, address, 2);
}

void nv_vram_write32(NV15State *s, uint32_t address, uint32_t value)
{
    s->vga.vram_ptr[(address + 0) & s->memsize_mask] = (value >> 0) & 0xFF;
    s->vga.vram_ptr[(address + 1) & s->memsize_mask] = (value >> 8) & 0xFF;
    s->vga.vram_ptr[(address + 2) & s->memsize_mask] = (value >> 16) & 0xFF;
    s->vga.vram_ptr[(address + 3) & s->memsize_mask] = (value >> 24) & 0xFF;
    nv_vram_touch(s, address, 4);
}

void nv_vram_write64(NV15State *s, uint32_t address, uint64_t value)
{
    for (int i = 0; i < 8; i++) {
        s->vga.vram_ptr[(address + i) & s->memsize_mask] =
            (value >> (i * 8)) & 0xFF;
    }
    nv_vram_touch(s, address, 8);
}

uint8_t nv_ramin_read8(NV15State *s, uint32_t address)
{
    return nv_vram_read8(s, address ^ s->ramin_flip);
}

uint16_t nv_ramin_read16(NV15State *s, uint32_t address)
{
    return nv_vram_read16(s, address ^ s->ramin_flip);
}

uint32_t nv_ramin_read32(NV15State *s, uint32_t address)
{
    return nv_vram_read32(s, address ^ s->ramin_flip);
}

void nv_ramin_write8(NV15State *s, uint32_t address, uint8_t value)
{
    nv_vram_write8(s, address ^ s->ramin_flip, value);
}

void nv_ramin_write32(NV15State *s, uint32_t address, uint32_t value)
{
    nv_vram_write32(s, address ^ s->ramin_flip, value);
}

/* ===================================================================== */
/* Guest physical memory (PCI/AGP bus mastering)                         */
/* ===================================================================== */

uint8_t nv_physical_read8(NV15State *s, uint32_t address)
{
    uint8_t data = 0;
    pci_dma_read(PCI_DEVICE(s), address, &data, 1);
    return data;
}

uint16_t nv_physical_read16(NV15State *s, uint32_t address)
{
    uint8_t d[2] = {0};
    pci_dma_read(PCI_DEVICE(s), address, d, 2);
    return d[0] << 0 | d[1] << 8;
}

uint32_t nv_physical_read32(NV15State *s, uint32_t address)
{
    uint8_t d[4] = {0};
    pci_dma_read(PCI_DEVICE(s), address, d, 4);
    return (uint32_t)d[0] << 0 | (uint32_t)d[1] << 8 |
           (uint32_t)d[2] << 16 | (uint32_t)d[3] << 24;
}

uint64_t nv_physical_read64(NV15State *s, uint32_t address)
{
    uint8_t d[8] = {0};
    pci_dma_read(PCI_DEVICE(s), address, d, 8);
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) {
        v |= (uint64_t)d[i] << (i * 8);
    }
    return v;
}

void nv_physical_write8(NV15State *s, uint32_t address, uint8_t value)
{
    pci_dma_write(PCI_DEVICE(s), address, &value, 1);
}

void nv_physical_write16(NV15State *s, uint32_t address, uint16_t value)
{
    uint8_t d[2] = { value & 0xFF, (value >> 8) & 0xFF };
    pci_dma_write(PCI_DEVICE(s), address, d, 2);
}

void nv_physical_write32(NV15State *s, uint32_t address, uint32_t value)
{
    uint8_t d[4];
    for (int i = 0; i < 4; i++) {
        d[i] = (value >> (i * 8)) & 0xFF;
    }
    pci_dma_write(PCI_DEVICE(s), address, d, 4);
}

void nv_physical_write64(NV15State *s, uint32_t address, uint64_t value)
{
    uint8_t d[8];
    for (int i = 0; i < 8; i++) {
        d[i] = (value >> (i * 8)) & 0xFF;
    }
    pci_dma_write(PCI_DEVICE(s), address, d, 8);
}

/* ===================================================================== */
/* DMA objects (RAMIN page tables / linear)                              */
/* ===================================================================== */

static uint32_t nv_dma_pt_lookup(NV15State *s, uint32_t object, uint32_t address)
{
    uint32_t address_adj = address + (nv_ramin_read32(s, object) >> 20);
    uint32_t page_offset = address_adj & 0xFFF;
    uint32_t page_index = address_adj >> 12;
    uint32_t page = nv_ramin_read32(s, object + 8 + page_index * 4) & 0xFFFFF000;
    return page | page_offset;
}

uint32_t nv_dma_lin_lookup(NV15State *s, uint32_t object, uint32_t address)
{
    uint32_t adjust = nv_ramin_read32(s, object) >> 20;
    uint32_t base = nv_ramin_read32(s, object + 8) & 0xFFFFF000;
    return base + adjust + address;
}

uint8_t nv_dma_read8(NV15State *s, uint32_t object, uint32_t address)
{
    uint32_t flags = nv_ramin_read32(s, object);
    uint32_t a = (flags & 0x00002000) ? nv_dma_lin_lookup(s, object, address)
                                      : nv_dma_pt_lookup(s, object, address);
    return (flags & 0x00020000) ? nv_physical_read8(s, a)
                                : nv_vram_read8(s, a & s->memsize_mask);
}

uint16_t nv_dma_read16(NV15State *s, uint32_t object, uint32_t address)
{
    uint32_t flags = nv_ramin_read32(s, object);
    uint32_t a = (flags & 0x00002000) ? nv_dma_lin_lookup(s, object, address)
                                      : nv_dma_pt_lookup(s, object, address);
    return (flags & 0x00020000) ? nv_physical_read16(s, a)
                                : nv_vram_read16(s, a & s->memsize_mask);
}

uint32_t nv_dma_read32(NV15State *s, uint32_t object, uint32_t address)
{
    uint32_t flags = nv_ramin_read32(s, object);
    uint32_t a = (flags & 0x00002000) ? nv_dma_lin_lookup(s, object, address)
                                      : nv_dma_pt_lookup(s, object, address);
    return (flags & 0x00020000) ? nv_physical_read32(s, a)
                                : nv_vram_read32(s, a & s->memsize_mask);
}

uint64_t nv_dma_read64(NV15State *s, uint32_t object, uint32_t address)
{
    uint32_t flags = nv_ramin_read32(s, object);
    uint32_t a = (flags & 0x00002000) ? nv_dma_lin_lookup(s, object, address)
                                      : nv_dma_pt_lookup(s, object, address);
    return (flags & 0x00020000) ? nv_physical_read64(s, a)
                                : nv_vram_read64(s, a & s->memsize_mask);
}

void nv_dma_write8(NV15State *s, uint32_t object, uint32_t address, uint8_t value)
{
    uint32_t flags = nv_ramin_read32(s, object);
    uint32_t a = (flags & 0x00002000) ? nv_dma_lin_lookup(s, object, address)
                                      : nv_dma_pt_lookup(s, object, address);
    if (flags & 0x00020000) {
        nv_physical_write8(s, a, value);
    } else {
        nv_vram_write8(s, a, value);
    }
}

void nv_dma_write16(NV15State *s, uint32_t object, uint32_t address, uint16_t value)
{
    uint32_t flags = nv_ramin_read32(s, object);
    uint32_t a = (flags & 0x00002000) ? nv_dma_lin_lookup(s, object, address)
                                      : nv_dma_pt_lookup(s, object, address);
    if (flags & 0x00020000) {
        nv_physical_write16(s, a, value);
    } else {
        nv_vram_write16(s, a, value);
    }
}

void nv_dma_write32(NV15State *s, uint32_t object, uint32_t address, uint32_t value)
{
    uint32_t flags = nv_ramin_read32(s, object);
    uint32_t a = (flags & 0x00002000) ? nv_dma_lin_lookup(s, object, address)
                                      : nv_dma_pt_lookup(s, object, address);
    if (flags & 0x00020000) {
        nv_physical_write32(s, a, value);
    } else {
        nv_vram_write32(s, a, value);
    }
}

void nv_dma_write64(NV15State *s, uint32_t object, uint32_t address, uint64_t value)
{
    uint32_t flags = nv_ramin_read32(s, object);
    uint32_t a = (flags & 0x00002000) ? nv_dma_lin_lookup(s, object, address)
                                      : nv_dma_pt_lookup(s, object, address);
    if (flags & 0x00020000) {
        nv_physical_write64(s, a, value);
    } else {
        nv_vram_write64(s, a, value);
    }
}

void nv_dma_copy(NV15State *s, uint32_t dst_obj, uint32_t dst_addr,
                 uint32_t src_obj, uint32_t src_addr, uint32_t byte_count)
{
    uint32_t dst_flags = nv_ramin_read32(s, dst_obj);
    uint32_t src_flags = nv_ramin_read32(s, src_obj);
    uint8_t buffer[0x1000];
    uint32_t bytes_left = byte_count;
    while (bytes_left) {
        uint32_t dst_abs = (dst_flags & 0x00002000) ?
            nv_dma_lin_lookup(s, dst_obj, dst_addr) :
            nv_dma_pt_lookup(s, dst_obj, dst_addr);
        uint32_t src_abs = (src_flags & 0x00002000) ?
            nv_dma_lin_lookup(s, src_obj, src_addr) :
            nv_dma_pt_lookup(s, src_obj, src_addr);
        uint32_t chunk = MIN(bytes_left, MIN(0x1000 - (dst_abs & 0xFFF),
                                             0x1000 - (src_abs & 0xFFF)));
        if (src_flags & 0x00020000) {
            pci_dma_read(PCI_DEVICE(s), src_abs, buffer, chunk);
        } else {
            memcpy(buffer, s->vga.vram_ptr + (src_abs & s->memsize_mask), chunk);
        }
        if (dst_flags & 0x00020000) {
            pci_dma_write(PCI_DEVICE(s), dst_abs, buffer, chunk);
        } else {
            uint32_t off = dst_abs & s->memsize_mask;
            memcpy(s->vga.vram_ptr + off, buffer, chunk);
            nv_vram_touch(s, off, chunk);
        }
        dst_addr += chunk;
        src_addr += chunk;
        bytes_left -= chunk;
    }
}

static uint32_t nv_ramfc_address(NV15State *s, uint32_t chid, uint32_t offset)
{
    uint32_t ramfc = (s->fifo_ramfc & 0xFFF) << 8;
    uint32_t ramfc_ch_size = 0x40; /* NV15 (0x20 <= card < 0x40) */
    return ramfc + chid * ramfc_ch_size + offset;
}

static void nv_ramfc_write32(NV15State *s, uint32_t chid, uint32_t offset,
                             uint32_t value)
{
    nv_ramin_write32(s, nv_ramfc_address(s, chid, offset), value);
}

static uint32_t nv_ramfc_read32(NV15State *s, uint32_t chid, uint32_t offset)
{
    return nv_ramin_read32(s, nv_ramfc_address(s, chid, offset));
}

static void nv_ramht_lookup(NV15State *s, uint32_t handle, uint32_t chid,
                            uint32_t *object, uint8_t *engine)
{
    uint32_t ramht_addr = (s->fifo_ramht & 0xFFF) << 8;
    uint32_t ramht_bits = ((s->fifo_ramht >> 16) & 0xFF) + 9;
    uint32_t ramht_size = 1 << ramht_bits << 3;

    uint32_t hash = 0;
    uint32_t x = handle;
    while (x) {
        hash ^= (x & ((1 << ramht_bits) - 1));
        x >>= ramht_bits;
    }
    hash ^= (chid & 0xF) << (ramht_bits - 4);
    hash = hash << 3;

    uint32_t it = hash;
    do {
        if (nv_ramin_read32(s, ramht_addr + it) == handle) {
            uint32_t context = nv_ramin_read32(s, ramht_addr + it + 4);
            uint32_t ctx_chid = (context >> 24) & 0x1F;
            if (chid == ctx_chid) {
                if (object) {
                    *object = (context & 0xFFFF) << 4;
                }
                if (engine) {
                    *engine = (context >> 16) & 0xFF;
                }
                return;
            }
        }
        it += 8;
        if (it >= ramht_size) {
            it = 0;
        }
    } while (it != hash);

    qemu_log_mask(LOG_GUEST_ERROR, "nv15: ramht_lookup failed for 0x%08x\n",
                  handle);
    if (object) {
        *object = 0;
    }
    if (engine) {
        *engine = 0;
    }
}

/* ===================================================================== */
/* Redraw / dirty tracking (mirror of Bochs redraw_area into VGA dirty)  */
/* ===================================================================== */

static void nv_mark_rows(NV15State *s, uint32_t vram_byte, uint32_t rows)
{
    if (s->svga_pitch == 0) {
        return;
    }
    uint32_t span = rows * s->svga_pitch;
    if (vram_byte >= GEFORCE_VRAM_SIZE) {
        return;
    }
    if (vram_byte + span > GEFORCE_VRAM_SIZE) {
        span = GEFORCE_VRAM_SIZE - vram_byte;
    }
    memory_region_set_dirty(&s->vga.vram, vram_byte, span);
}

void nv_redraw_area_nd_off(NV15State *s, uint32_t offset,
                           uint32_t width, uint32_t height)
{
    nv_mark_rows(s, s->disp_offset + offset, height + 1);
}

void nv_redraw_area_nd(NV15State *s, int32_t x0, int32_t y0,
                       uint32_t width, uint32_t height)
{
    if (s->svga_pitch == 0 || y0 < 0) {
        y0 = 0;
    }
    uint32_t byte = s->disp_offset + (uint32_t)y0 * s->svga_pitch;
    nv_mark_rows(s, byte, height + 1);
}

void nv_redraw_area_d(NV15State *s, int32_t x0, int32_t y0,
                      uint32_t width, uint32_t height)
{
    nv_redraw_area_nd(s, x0, y0, width, height);
}

/* ===================================================================== */
/* IRQ                                                                   */
/* ===================================================================== */

static uint32_t nv_get_mc_intr(NV15State *s)
{
    uint32_t value = 0;
    if (s->bus_intr & s->bus_intr_en) {
        value |= 0x10000000;
    }
    if (s->fifo_intr & s->fifo_intr_en) {
        value |= 0x00000100;
    }
    if (s->graph_intr & s->graph_intr_en) {
        value |= 0x00001000;
    }
    if (s->crtc_intr & s->crtc_intr_en) {
        value |= 0x01000000;
    }
    return value;
}

static void nv_update_irq_level(NV15State *s)
{
    bool level = (nv_get_mc_intr(s) && (s->mc_intr_en & 1)) ||
                 (s->mc_soft_intr && (s->mc_intr_en & 2));
    if (level != s->irq_level) {
        s->irq_level = level;
        pci_set_irq(PCI_DEVICE(s), level);
    }
}

/* ===================================================================== */
/* PCI config mirror write (PBUS 0x1800 window / config_write)           */
/* ===================================================================== */

static void nv_config_write(PCIDevice *dev, uint32_t addr, uint32_t val, int len)
{
    /* Status register (0x06/0x07) is read-only on this chip. */
    for (int i = 0; i < len; i++) {
        unsigned a = addr + i;
        if (a == 0x06 || a == 0x07) {
            continue;
        }
        pci_default_write_config(dev, a, (val >> (i * 8)) & 0xFF, 1);
    }
}

/* ===================================================================== */
/* MMIO register file (BAR0)                                             */
/* ===================================================================== */

static uint8_t nv_prom_read8(NV15State *s, uint32_t offset)
{
    PCIDevice *pci = PCI_DEVICE(s);
    if (pci->config[0x50] != 0x00) {
        return 0x00;
    }
    if (memory_region_size(&pci->rom) > offset &&
        memory_region_is_ram(&pci->rom)) {
        uint8_t *p = memory_region_get_ram_ptr(&pci->rom);
        if (p) {
            return p[offset];
        }
    }
    return 0xff;
}

static uint8_t nv_register_read8(NV15State *s, uint32_t address)
{
    PCIDevice *pci = PCI_DEVICE(s);
    uint8_t value;
    if (address >= 0x1800 && address < 0x1900) {
        value = pci->config[address - 0x1800];
    } else if (address >= 0x300000 && address < 0x310000) {
        value = nv_prom_read8(s, address - 0x300000);
    } else if ((address >= 0xc0300 && address < 0xc0400) ||
               (address >= 0xc2300 && address < 0xc2400)) {
        uint32_t head = (address >> 13) & 1;
        uint32_t offset = address & 0x00000fff;
        if (offset == 0x3c3 || offset == 0x3c4 || offset == 0x3c5 ||
            offset == 0x3cc || offset == 0x3cf) {
            value = head ? 0x00 : nv_svga_read(s, offset, 1);
        } else {
            value = 0xFF;
        }
    } else if ((address >= 0x601300 && address < 0x601400) ||
               (address >= 0x603300 && address < 0x603400)) {
        uint32_t head = (address >> 13) & 1;
        uint32_t offset = address & 0x00000fff;
        if (offset == 0x3b4 || offset == 0x3b5 || offset == 0x3c0 ||
            offset == 0x3c1 || offset == 0x3c2 || offset == 0x3d4 ||
            offset == 0x3d5 || offset == 0x3d8 || offset == 0x3da) {
            value = head ? 0x00 : nv_svga_read(s, offset, 1);
        } else {
            value = 0xFF;
        }
    } else if ((address >= 0x681300 && address < 0x681400) ||
               (address >= 0x683300 && address < 0x683400)) {
        uint32_t head = (address >> 13) & 1;
        uint32_t offset = address & 0x00000fff;
        if (offset >= 0x3c6 && offset <= 0x3c9) {
            value = head ? 0x00 : nv_svga_read(s, offset, 1);
        } else {
            value = 0xFF;
        }
    } else if (address >= 0x700000 && address < 0x800000) {
        value = s->vga.vram_ptr[(address - 0x700000) ^ s->ramin_flip];
    } else {
        value = nv_register_read32(s, address);
    }
    return value;
}

static void nv_register_write8(NV15State *s, uint32_t address, uint8_t value)
{
    if ((address >= 0xc0300 && address < 0xc0400) ||
        (address >= 0xc2300 && address < 0xc2400)) {
        uint32_t head = (address >> 13) & 1;
        uint32_t offset = address & 0x00000fff;
        if (offset == 0x3c2 || offset == 0x3c3 || offset == 0x3c4 ||
            offset == 0x3c5 || offset == 0x3ce || offset == 0x3cf) {
            if (!head) {
                nv_svga_write(s, offset, value, 1);
            }
        }
    } else if ((address >= 0x601300 && address < 0x601400) ||
               (address >= 0x603300 && address < 0x603400)) {
        uint32_t head = (address >> 13) & 1;
        uint32_t offset = address & 0x00000fff;
        if (offset == 0x3b4 || offset == 0x3b5 || offset == 0x3c0 ||
            offset == 0x3c1 || offset == 0x3c2 || offset == 0x3d4 ||
            offset == 0x3d5 || offset == 0x3da) {
            if (!head) {
                nv_svga_write(s, offset, value, 1);
            }
        }
    } else if ((address >= 0x681300 && address < 0x681400) ||
               (address >= 0x683300 && address < 0x683400)) {
        uint32_t head = (address >> 13) & 1;
        uint32_t offset = address & 0x00000fff;
        if (offset >= 0x3c6 && offset <= 0x3c9) {
            if (!head) {
                nv_svga_write(s, offset, value, 1);
            }
        }
    } else if (address >= 0x700000 && address < 0x800000) {
        s->vga.vram_ptr[(address - 0x700000) ^ s->ramin_flip] = value;
    } else {
        nv_register_write32(s, address,
                            (nv_register_read32(s, address) & ~0xFF) | value);
    }
}

static uint32_t nv_register_read32(NV15State *s, uint32_t address)
{
    PCIDevice *pci = PCI_DEVICE(s);
    uint32_t value;

    if (address == 0x0) {
        value = GEFORCE_CARD_TYPE << 20;                 /* 0x01500000 */
    } else if (address == 0x100) {
        value = nv_get_mc_intr(s);
        if (s->mc_soft_intr) {
            value |= 0x80000000;
        }
    } else if (address == 0x140) {
        value = s->mc_intr_en;
    } else if (address == 0x200) {
        value = s->mc_enable;
    } else if (address == 0x1100) {
        value = s->bus_intr;
    } else if (address == 0x1140) {
        value = s->bus_intr_en;
    } else if (address >= 0x1800 && address < 0x1900) {
        uint32_t offset = address - 0x1800;
        value = (pci->config[offset + 0] << 0) |
                (pci->config[offset + 1] << 8) |
                (pci->config[offset + 2] << 16) |
                (pci->config[offset + 3] << 24);
    } else if (address == 0x2100) {
        value = s->fifo_intr;
    } else if (address == 0x2140) {
        value = s->fifo_intr_en;
    } else if (address == 0x2210) {
        value = s->fifo_ramht;
    } else if (address == 0x2214) {
        value = s->fifo_ramfc;
    } else if (address == 0x2218) {
        value = s->fifo_ramro;
    } else if (address == 0x2400) {
        value = (s->fifo_cache1_get != s->fifo_cache1_put) ? 0 : 0x10;
    } else if (address == 0x2504) {
        value = s->fifo_mode;
    } else if (address == 0x3200) {
        value = s->fifo_cache1_push0;
    } else if (address == 0x3204) {
        value = s->fifo_cache1_push1;
    } else if (address == 0x3210) {
        value = s->fifo_cache1_put;
    } else if (address == 0x3214) {
        value = (s->fifo_cache1_get != s->fifo_cache1_put) ? 0 : 0x10;
    } else if (address == 0x3220) {
        value = s->fifo_cache1_dma_push;
    } else if (address == 0x322c) {
        value = s->fifo_cache1_dma_instance;
    } else if (address == 0x3230) {
        value = 0x80000000;
    } else if (address == 0x3240) {
        value = s->fifo_cache1_dma_put;
    } else if (address == 0x3244) {
        value = s->fifo_cache1_dma_get;
    } else if (address == 0x3248) {
        value = s->fifo_cache1_ref_cnt;
    } else if (address == 0x3250) {
        if (s->fifo_cache1_get != s->fifo_cache1_put) {
            s->fifo_cache1_pull0 |= 0x00000100;
        }
        value = s->fifo_cache1_pull0;
    } else if (address == 0x3270) {
        value = s->fifo_cache1_get;
    } else if (address == 0x32e0) {
        value = s->fifo_grctx_instance;
    } else if (address == 0x3304) {
        value = 0x00000001;
    } else if (address >= 0x3800 && address < 0x4000) {
        uint32_t offset = address - 0x3800;
        uint32_t index = offset / 8;
        value = (offset % 8 == 0) ? s->fifo_cache1_method[index]
                                  : s->fifo_cache1_data[index];
    } else if (address == 0x9100) {
        value = s->timer_intr;
    } else if (address == 0x9140) {
        value = s->timer_intr_en;
    } else if (address == 0x9200) {
        value = s->timer_num;
    } else if (address == 0x9210) {
        value = s->timer_den;
    } else if (address == 0x9400) {
        value = (uint32_t)nv_get_current_time(s);
    } else if (address == 0x9410) {
        value = nv_get_current_time(s) >> 32;
    } else if (address == 0x9420) {
        value = s->timer_alarm;
    } else if ((address >= 0xc0300 && address < 0xc0400) ||
               (address >= 0xc2300 && address < 0xc2400)) {
        value = nv_register_read8(s, address);
    } else if (address == 0x10020c) {
        value = GEFORCE_VRAM_SIZE;
    } else if (address == 0x100320) {
        value = 0x0002e3ff;                              /* PFB_ZCOMP_SIZE */
    } else if (address == 0x101000) {
        value = s->straps0_primary;
    } else if (address >= 0x300000 && address < 0x310000) {
        uint32_t offset = address - 0x300000;
        value = nv_prom_read8(s, offset + 0) << 0 |
                nv_prom_read8(s, offset + 1) << 8 |
                nv_prom_read8(s, offset + 2) << 16 |
                nv_prom_read8(s, offset + 3) << 24;
    } else if (address == 0x400100) {
        value = s->graph_intr;
    } else if (address == 0x400108) {
        value = s->graph_nsource;
    } else if (address == 0x400140) {
        value = s->graph_intr_en;
    } else if (address == 0x40014C) {
        value = s->graph_ctx_switch1;
    } else if (address == 0x400150) {
        value = s->graph_ctx_switch2;
    } else if (address == 0x400158) {
        value = s->graph_ctx_switch4;
    } else if (address == 0x40032c) {
        value = s->graph_ctxctl_cur;
    } else if (address == 0x400700) {
        value = s->graph_status;
    } else if (address == 0x400704) {
        value = s->graph_trapped_addr;
    } else if (address == 0x400708) {
        value = s->graph_trapped_data;
    } else if (address == 0x400718) {
        value = s->graph_notify;
    } else if (address == 0x400720) {
        value = s->graph_fifo;
    } else if (address == 0x400724) {
        value = s->graph_bpixel;
    } else if (address == 0x400780) {
        value = s->graph_channel_ctx_table;
    } else if (address == 0x400640) {
        value = s->graph_offset0;
    } else if (address == 0x400670) {
        value = s->graph_pitch0;
    } else if (address == 0x600100) {
        value = s->crtc_intr;
    } else if (address == 0x600140) {
        value = s->crtc_intr_en;
    } else if (address == 0x600800) {
        value = s->crtc_start;
    } else if (address == 0x600804) {
        value = s->crtc_config;
    } else if (address == 0x600808) {
        s->crtc_raster_pos ^= 1;
        value = s->crtc_raster_pos;
    } else if (address == 0x60080c) {
        value = s->crtc_cursor_offset;
    } else if (address == 0x600810) {
        value = s->crtc_cursor_config;
    } else if (address == 0x60081c) {
        value = s->crtc_gpio_ext;
    } else if (address == 0x600868) {
        /* fake display position: creep through a frame */
        s->crtc_raster_pos = (s->crtc_raster_pos + 1) % 1024;
        value = s->crtc_raster_pos;
    } else if ((address >= 0x601300 && address < 0x601400) ||
               (address >= 0x603300 && address < 0x603400)) {
        value = nv_register_read8(s, address);
    } else if (address == 0x680300) {
        value = s->ramdac_cu_start_pos;
    } else if (address == 0x680404) {
        value = 0x00000000;
    } else if (address == 0x680500) {
        value = s->ramdac_nvpll;
    } else if (address == 0x680504) {
        value = s->ramdac_mpll;
    } else if (address == 0x680508) {
        value = s->ramdac_vpll;
    } else if (address == 0x68050c) {
        value = s->ramdac_pll_select;
    } else if (address == 0x680578) {
        value = s->ramdac_vpll_b;
    } else if (address == 0x680600) {
        value = s->ramdac_general_control;
    } else if (address == 0x680828) {
        value = 0x00000000;
    } else if ((address >= 0x681300 && address < 0x681400) ||
               (address >= 0x683300 && address < 0x683400)) {
        value = nv_register_read8(s, address);
    } else if (address >= 0x700000 && address < 0x800000) {
        uint32_t offset = address & 0x000fffff;
        if (offset & 3) {
            value = nv_ramin_read8(s, offset + 0) << 0 |
                    nv_ramin_read8(s, offset + 1) << 8 |
                    nv_ramin_read8(s, offset + 2) << 16 |
                    nv_ramin_read8(s, offset + 3) << 24;
        } else {
            value = nv_ramin_read32(s, offset);
        }
    } else if (address >= 0x800000 && address < 0xA00000) {
        uint32_t chid = (address >> 16) & 0x1F;
        uint32_t offset = address & 0x1FFF;
        value = 0x00000000;
        uint32_t curchid = s->fifo_cache1_push1 & 0x1F;
        if (offset == 0x10) {
            value = 0xffff;
        } else if (offset >= 0x40 && offset <= 0x48) {
            if (curchid == chid) {
                if (offset == 0x40) {
                    value = s->fifo_cache1_dma_put;
                } else if (offset == 0x44) {
                    value = s->fifo_cache1_dma_get;
                } else if (offset == 0x48) {
                    value = s->fifo_cache1_ref_cnt;
                }
            } else {
                if (offset == 0x40) {
                    value = nv_ramfc_read32(s, chid, 0x0);
                } else if (offset == 0x44) {
                    value = nv_ramfc_read32(s, chid, 0x4);
                } else if (offset == 0x48) {
                    value = nv_ramfc_read32(s, chid, 0x8);
                }
            }
        }
    } else {
        value = s->unk_regs[(address / 4) & (GEFORCE_PNPMMIO_SIZE / 4 - 1)];
        NV_TRACE(s, "nv15 R  UNMODELED reg 0x%06x = 0x%08x (scratch)\n",
                 address, value);
    }
    return value;
}

static void nv_register_write32(NV15State *s, uint32_t address, uint32_t value)
{
    if (address == 0x100) {
        s->mc_soft_intr = (bool)(value >> 31);
        nv_update_irq_level(s);
    } else if (address == 0x140) {
        s->mc_intr_en = value;
        nv_update_irq_level(s);
    } else if (address == 0x200) {
        s->mc_enable = value;
    } else if (address >= 0x1800 && address < 0x1900) {
        nv_config_write(PCI_DEVICE(s), address - 0x1800, value, 4);
    } else if (address == 0x1100) {
        s->bus_intr &= ~value;
        nv_update_irq_level(s);
    } else if (address == 0x1140) {
        s->bus_intr_en = value;
        nv_update_irq_level(s);
    } else if (address == 0x2100) {
        s->fifo_intr &= ~value;
        nv_update_irq_level(s);
    } else if (address == 0x2140) {
        s->fifo_intr_en = value;
        nv_update_irq_level(s);
    } else if (address == 0x2210) {
        s->fifo_ramht = value;
    } else if (address == 0x2214) {
        s->fifo_ramfc = value;
    } else if (address == 0x2218) {
        s->fifo_ramro = value;
    } else if (address == 0x2504) {
        bool process = (s->fifo_mode | value) != s->fifo_mode;
        s->fifo_mode = value;
        if (process) {
            nv_fifo_process(s);
        }
    } else if (address == 0x3200) {
        s->fifo_cache1_push0 = value;
        if (s->fifo_cache1_push0 & 1) {
            nv_fifo_process(s);
        }
    } else if (address == 0x3204) {
        s->fifo_cache1_push1 = value;
    } else if (address == 0x3210) {
        s->fifo_cache1_put = value;
    } else if (address == 0x3220) {
        s->fifo_cache1_dma_push = value;
    } else if (address == 0x322c) {
        s->fifo_cache1_dma_instance = value;
    } else if (address == 0x3240) {
        s->fifo_cache1_dma_put = value;
    } else if (address == 0x3244) {
        s->fifo_cache1_dma_get = value;
    } else if (address == 0x3248) {
        s->fifo_cache1_ref_cnt = value;
    } else if (address == 0x3250) {
        s->fifo_cache1_pull0 = value;
        if (s->fifo_cache1_pull0 & 1) {
            nv_fifo_process(s);
        }
    } else if (address == 0x3270) {
        s->fifo_cache1_get = value & (GEFORCE_CACHE1_SIZE * 4 - 1);
        if (s->fifo_cache1_get != s->fifo_cache1_put) {
            s->fifo_intr |= 0x00000001;
        } else {
            s->fifo_intr &= ~0x00000001;
            s->fifo_cache1_pull0 &= ~0x00000100;
            if (s->fifo_wait_soft) {
                s->fifo_wait_soft = false;
                s->fifo_wait = s->fifo_wait_notify || s->fifo_wait_flip ||
                               s->fifo_wait_acquire;
                nv_fifo_process(s);
            }
        }
        nv_update_irq_level(s);
    } else if (address == 0x32e0) {
        s->fifo_grctx_instance = value;
    } else if (address == 0x9100) {
        s->timer_intr &= ~value;
    } else if (address == 0x9140) {
        s->timer_intr_en = value;
    } else if (address == 0x9200) {
        s->timer_num = value;
    } else if (address == 0x9210) {
        s->timer_den = value;
    } else if (address == 0x9400 || address == 0x9410) {
        s->timer_inittime2 = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        if (address == 0x9400) {
            s->timer_inittime1 =
                (s->timer_inittime1 & 0xFFFFFFFF00000000ULL) | value;
        } else {
            s->timer_inittime1 =
                (s->timer_inittime1 & 0x00000000FFFFFFFFULL) |
                ((uint64_t)value << 32);
        }
    } else if (address == 0x9420) {
        s->timer_alarm = value;
    } else if ((address >= 0xc0300 && address < 0xc0400) ||
               (address >= 0xc2300 && address < 0xc2400)) {
        nv_register_write8(s, address, value);
    } else if (address == 0x101000) {
        if (value >> 31) {
            s->straps0_primary = value;
        } else {
            s->straps0_primary = s->straps0_primary_original;
        }
    } else if (address == 0x400100) {
        s->graph_intr &= ~value;
        nv_update_irq_level(s);
        if (s->fifo_wait_notify && s->graph_intr == 0) {
            s->fifo_wait_notify = false;
            s->fifo_wait = s->fifo_wait_soft || s->fifo_wait_flip ||
                           s->fifo_wait_acquire;
            nv_fifo_process(s);
        }
    } else if (address == 0x400108) {
        s->graph_nsource = value;
    } else if (address == 0x400140) {
        s->graph_intr_en = value;
        nv_update_irq_level(s);
    } else if (address == 0x40014C) {
        s->graph_ctx_switch1 = value;
    } else if (address == 0x400150) {
        s->graph_ctx_switch2 = value;
    } else if (address == 0x400158) {
        s->graph_ctx_switch4 = value;
    } else if (address == 0x40032c) {
        s->graph_ctxctl_cur = value;
    } else if (address == 0x400700) {
        s->graph_status = value;
    } else if (address == 0x400704) {
        s->graph_trapped_addr = value;
    } else if (address == 0x400708) {
        s->graph_trapped_data = value;
    } else if (address == 0x400718) {
        s->graph_notify = value;
    } else if (address == 0x40071c) {
        if (value & 0x00000002) {
            s->graph_flip_read++;
            if (s->graph_flip_modulo) {
                s->graph_flip_read %= s->graph_flip_modulo;
            }
            if (s->fifo_wait_flip &&
                s->graph_flip_read != s->graph_flip_write) {
                s->fifo_wait_flip = false;
                s->fifo_wait = s->fifo_wait_soft || s->fifo_wait_notify ||
                               s->fifo_wait_acquire;
                nv_fifo_process(s);
            }
        }
    } else if (address == 0x400720) {
        s->graph_fifo = value;
    } else if (address == 0x400724) {
        s->graph_bpixel = value;
    } else if (address == 0x400780) {
        s->graph_channel_ctx_table = value;
    } else if (address == 0x400640) {
        s->graph_offset0 = value;
    } else if (address == 0x400670) {
        s->graph_pitch0 = value;
    } else if (address == 0x600100) {
        s->crtc_intr &= ~value;
        nv_update_irq_level(s);
    } else if (address == 0x600140) {
        s->crtc_intr_en = value;
        nv_update_irq_level(s);
    } else if (address == 0x600800) {
        s->crtc_start = value;
        s->svga_needs_update_mode = 1;
    } else if (address == 0x600804) {
        s->crtc_config = value;
    } else if (address == 0x60080c) {
        s->crtc_cursor_offset = value;
        s->hw_cursor.offset = s->crtc_cursor_offset;
    } else if (address == 0x600810) {
        s->crtc_cursor_config = value;
        s->hw_cursor.enabled =
            (s->crtc.reg[0x31] & 0x01) || (value & 0x00000001);
        s->hw_cursor.vram =
            (s->crtc.reg[0x30] & 0x80) || (value & 0x00000100);
        s->hw_cursor.size = value & 0x00010000 ? 64 : 32;
        s->hw_cursor.bpp32 = value & 0x00001000;
    } else if (address == 0x60081c) {
        s->crtc_gpio_ext = value;
    } else if ((address >= 0x601300 && address < 0x601400) ||
               (address >= 0x603300 && address < 0x603400)) {
        nv_register_write8(s, address, value);
    } else if (address == 0x680300) {
        s->ramdac_cu_start_pos = value;
        s->hw_cursor.x = (int32_t)s->ramdac_cu_start_pos << 20 >> 20;
        s->hw_cursor.y = (int32_t)s->ramdac_cu_start_pos << 4 >> 20;
        s->full_update_pending = true;
    } else if (address == 0x680500) {
        s->ramdac_nvpll = value;
    } else if (address == 0x680504) {
        s->ramdac_mpll = value;
    } else if (address == 0x680508) {
        s->ramdac_vpll = value;
    } else if (address == 0x68050c) {
        s->ramdac_pll_select = value;
    } else if (address == 0x680578) {
        s->ramdac_vpll_b = value;
    } else if (address == 0x680600) {
        s->ramdac_general_control = value;
        s->dac_shift = (value >> 20) & 1 ? 0 : 2;
    } else if ((address >= 0x681300 && address < 0x681400) ||
               (address >= 0x683300 && address < 0x683400)) {
        nv_register_write8(s, address, value);
    } else if (address >= 0x700000 && address < 0x800000) {
        nv_ramin_write32(s, address - 0x700000, value);
    } else if (address >= 0x800000 && address < 0xA00000) {
        uint32_t chid = (address >> 16) & 0x1F;
        uint32_t offset = address & 0x1FFF;
        if (s->fifo_mode & (1 << chid)) {
            if (offset == 0x40) {
                uint32_t curchid = s->fifo_cache1_push1 & 0x1F;
                if (curchid == chid) {
                    s->fifo_cache1_dma_put = value;
                } else {
                    nv_ramfc_write32(s, chid, 0x0, value);
                }
                nv_fifo_process(s);
            }
        } else {
            uint32_t subc = (address >> 13) & 7;
            nv_execute_command(s, chid, subc, offset / 4, value);
        }
    } else {
        s->unk_regs[(address / 4) & (GEFORCE_PNPMMIO_SIZE / 4 - 1)] = value;
        NV_TRACE(s, "nv15 W  UNMODELED reg 0x%06x = 0x%08x (scratch)\n",
                 address, value);
    }
}

/* ===================================================================== */
/* FIFO command engine                                                   */
/* ===================================================================== */

static void nv_update_fifo_wait(NV15State *s)
{
    s->fifo_wait = s->fifo_wait_soft || s->fifo_wait_notify ||
                   s->fifo_wait_flip || s->fifo_wait_acquire;
}

/* Per-CONTEXT_BETA1-object blend-factor cache (see NV15State). */
static void nv_beta_store(NV15State *s, uint32_t obj, uint32_t val)
{
    int i;
    for (i = 0; i < 16; i++) {
        if (s->beta_obj_addr[i] == obj) {
            s->beta_obj_val[i] = val;
            return;
        }
    }
    for (i = 0; i < 16; i++) {
        if (s->beta_obj_addr[i] == 0) {
            s->beta_obj_addr[i] = obj;
            s->beta_obj_val[i] = val;
            return;
        }
    }
    s->beta_obj_addr[0] = obj;   /* cache full: evict slot 0 */
    s->beta_obj_val[0] = val;
}

static uint32_t nv_beta_load(NV15State *s, uint32_t obj, uint32_t def)
{
    for (int i = 0; i < 16; i++) {
        if (s->beta_obj_addr[i] == obj) {
            return s->beta_obj_val[i];
        }
    }
    return def;
}

static int nv_execute_command(NV15State *s, uint32_t chid, uint32_t subc,
                              uint32_t method, uint32_t param)
{
    int result = 0;
    bool software_method = false;
    gf_channel *ch = &s->chs[chid];
    if (method == 0x000) {
        if (ch->schs[subc].engine == 0x01) {
            uint32_t word1 = nv_ramin_read32(s, ch->schs[subc].object + 0x4);
            word1 = (word1 & 0x0000FFFF) | (ch->schs[subc].notifier >> 4 << 16);
            uint32_t word0 = nv_ramin_read32(s, ch->schs[subc].object);
            uint8_t cls8 = word0;
            if (cls8 == 0x4a || cls8 == 0x4b) {
                word0 = (word0 & 0xFFFC7FFF) | (ch->gdi_operation << 15);
                word1 = (word1 & 0xFFFFFFFC) | ch->gdi_mono_fmt;
                nv_ramin_write32(s, ch->schs[subc].object, word0);
            } else if (cls8 == 0x62) {
                nv_ramin_write32(s, ch->schs[subc].object + 0x8,
                    (ch->s2d_img_src >> 4) | (ch->s2d_img_dst >> 4 << 16));
            } else if (cls8 == 0x64) {
                nv_ramin_write32(s, ch->schs[subc].object + 0x8,
                                 ch->iifc_palette >> 4);
                word0 = (word0 & 0xFFFC7FFF) | (ch->iifc_operation << 15);
                nv_ramin_write32(s, ch->schs[subc].object, word0);
                word1 = (word1 & 0xFFFF00FF) | ((ch->iifc_color_fmt + 9) << 8);
            }
            nv_ramin_write32(s, ch->schs[subc].object + 0x4, word1);
        }
        nv_ramht_lookup(s, param, chid, &ch->schs[subc].object,
                        &ch->schs[subc].engine);
        if (ch->schs[subc].engine == 0x01) {
            uint32_t word1 = nv_ramin_read32(s, ch->schs[subc].object + 0x4);
            ch->schs[subc].notifier = word1 >> 16 << 4;
            uint32_t word0 = nv_ramin_read32(s, ch->schs[subc].object);
            uint8_t cls8 = word0;
            if (cls8 == 0x48) {
                if (!ch->s2d_locked) {
                    uint32_t srcdst =
                        nv_ramin_read32(s, ch->schs[subc].object + 0x8);
                    ch->s2d_img_src = (srcdst & 0xFFFF) << 4;
                    ch->s2d_img_dst = srcdst >> 16 << 4;
                    ch->s2d_color_fmt = s->graph_bpixel & 0xf;
                    nv2d_update_color_bytes_s2d(s, ch);
                    ch->s2d_pitch_src = s->graph_pitch0 & 0xffff;
                    ch->s2d_pitch_dst = ch->s2d_pitch_src;
                    ch->s2d_ofs_src = s->graph_offset0;
                    ch->s2d_ofs_dst = s->graph_offset0;
                }
            } else if (cls8 == 0x4a || cls8 == 0x4b) {
                ch->gdi_operation = (word0 >> 15) & 7;
                ch->gdi_mono_fmt = word1 & 3;
            } else if (cls8 == 0x62) {
                uint32_t srcdst =
                    nv_ramin_read32(s, ch->schs[subc].object + 0x8);
                ch->s2d_img_src = (srcdst & 0xFFFF) << 4;
                ch->s2d_img_dst = srcdst >> 16 << 4;
            } else if (cls8 == 0x64) {
                ch->iifc_palette =
                    nv_ramin_read32(s, ch->schs[subc].object + 0x8) << 4;
                ch->iifc_operation = (word0 >> 15) & 7;
                ch->iifc_color_fmt = (word1 >> 8 & 0xFF) - 9;
                nv2d_update_color_bytes_iifc(s, ch);
            } else if (cls8 == 0x96 || cls8 == 0x97) {
                nv3d_execute_d3d(s, ch, word0 & s->class_mask, 0, 0);
            }
        } else if (ch->schs[subc].engine == 0x00) {
            software_method = true;
        }
    } else if (method == 0x014) {
        s->fifo_cache1_ref_cnt = param;
    } else if (method == 0x018) {
        uint32_t semaphore_obj;
        nv_ramht_lookup(s, param, chid, &semaphore_obj, NULL);
        s->fifo_cache1_semaphore = semaphore_obj >> 4;
    } else if (method == 0x019) {
        s->fifo_cache1_semaphore &= 0x000FFFFF;
        s->fifo_cache1_semaphore |= param << 20;
    } else if (method == 0x01a || method == 0x01b) {
        uint32_t semaphore_obj = (s->fifo_cache1_semaphore & 0x000FFFFF) << 4;
        uint32_t semaphore_offset = s->fifo_cache1_semaphore >> 20;
        if (method == 0x01a) {
            if (nv_dma_read32(s, semaphore_obj, semaphore_offset) != param) {
                s->fifo_wait_acquire = true;
                s->fifo_wait = true;
                result = 2;
            }
        } else {
            nv_dma_write32(s, semaphore_obj, semaphore_offset, param);
        }
    } else if (method >= 0x040) {
        if (ch->schs[subc].engine == 0x01) {
            if (method >= 0x060 && method < 0x080) {
                nv_ramht_lookup(s, param, chid, &param, NULL);
            }
            uint32_t cls = nv_ramin_read32(s, ch->schs[subc].object) &
                           s->class_mask;
            uint8_t cls8 = cls;
            NV_TRACE(s, "nv15 M  ch%u sub%u cls 0x%02x mthd 0x%03x p 0x%08x\n",
                     chid, subc, cls8, method, param);
            /*
             * Beta (the op-5 blend factor) is a per-object context.  SetBeta
             * (class 0x72 method 0xc0) sets the value of the beta object bound
             * to this subchannel; a 2D op binds a CONTEXT_BETA1 object (class
             * 0x72) via one of its SetContext* methods -- the method number
             * differs per class (e.g. 0x066 for IFC), so detect the beta
             * object by its class when any context object is bound.  Track the
             * value per object and refresh ch->beta from the op's own beta
             * object, so a stale global beta from an unrelated operation cannot
             * leak into an opaque blit and render it semi-transparent.
             */
            if (method >= 0x060 && method < 0x080 &&
                (nv_ramin_read32(s, param) & s->class_mask) == 0x72) {
                ch->schs[subc].beta_object = param;
            } else if (cls8 == 0x72 && method == 0x0c0) {
                nv_beta_store(s, ch->schs[subc].object, param);
            }
            if (ch->schs[subc].beta_object) {
                ch->beta = nv_beta_load(s, ch->schs[subc].beta_object,
                                        ch->beta);
            }
            switch (cls8) {
            case 0x19:
                nv2d_execute_clip(s, ch, method, param);
                break;
            case 0x39:
                nv2d_execute_m2mf(s, ch, subc, method, param);
                break;
            case 0x43:
                nv2d_execute_rop(s, ch, method, param);
                break;
            case 0x44:
            case 0x18:
                nv2d_execute_patt(s, ch, method, param);
                break;
            case 0x4a:
            case 0x4b:
                nv2d_execute_gdi(s, ch, cls, method, param);
                break;
            case 0x52:
            case 0x9e:
                nv2d_execute_swzsurf(s, ch, method, param);
                break;
            case 0x57:
                nv2d_execute_chroma(s, ch, method, param);
                break;
            case 0x5c:
                nv2d_execute_lin(s, ch, method, param);
                break;
            case 0x5d:
                nv2d_execute_tri(s, ch, method, param);
                break;
            case 0x5e:
                nv2d_execute_rect(s, ch, method, param);
                break;
            case 0x5f:
            case 0x9f:
                nv2d_execute_imageblit(s, ch, method, param);
                break;
            case 0x61:
            case 0x65:
            case 0x8a:
            case 0x21:
                nv2d_execute_ifc(s, ch, method, param);
                break;
            case 0x62:
                nv2d_execute_surf2d(s, ch, method, param);
                break;
            case 0x64:
                nv2d_execute_iifc(s, ch, method, param);
                break;
            case 0x66:
            case 0x76:
                nv2d_execute_sifc(s, ch, method, param);
                break;
            case 0x72:
                nv2d_execute_beta(s, ch, method, param);
                break;
            case 0x7b:
                nv2d_execute_tfc(s, ch, method, param);
                break;
            case 0x89:
                nv2d_execute_sifm(s, ch, cls, method, param);
                break;
            case 0x96:
            case 0x97:
                nv3d_execute_d3d(s, ch, cls, method, param);
                if (s->fifo_wait_flip) {
                    result = 1;
                }
                break;
            default:
                NV_TRACE(s,
                    "nv15 M  UNHANDLED cls 0x%02x mthd 0x%03x p 0x%08x\n",
                    cls8, method, param);
                break;
            }
            if (ch->notify_pending) {
                ch->notify_pending = false;
                if ((nv_ramin_read32(s, ch->schs[subc].notifier) & 0xFF)
                    != 0x30) {
                    nv_dma_write64(s, ch->schs[subc].notifier, 0x0,
                                   nv_get_current_time(s));
                    nv_dma_write32(s, ch->schs[subc].notifier, 0x8, 0);
                    nv_dma_write32(s, ch->schs[subc].notifier, 0xC, 0);
                }
                if (ch->notify_type) {
                    s->graph_intr |= 0x00000001;
                    nv_update_irq_level(s);
                    s->graph_nsource |= 0x00000001;
                    s->graph_notify = 0x00110000;
                    uint32_t notifier = ch->schs[subc].notifier >> 4;
                    s->graph_ctx_switch2 = notifier << 16;
                    s->graph_ctx_switch4 = ch->schs[subc].object >> 4;
                    s->graph_trapped_addr =
                        (method << 2) | (subc << 16) | (chid << 20);
                    s->graph_trapped_data = param;
                    s->fifo_wait_notify = true;
                    s->fifo_wait = true;
                }
            }
            if (method == 0x041) {
                ch->notify_pending = true;
                ch->notify_type = param;
            } else if (method == 0x060) {
                ch->schs[subc].notifier = param;
            }
        } else if (ch->schs[subc].engine == 0x00) {
            software_method = true;
        }
    }
    if (software_method) {
        s->fifo_wait_soft = true;
        s->fifo_wait = true;
        s->fifo_intr |= 0x00000001;
        nv_update_irq_level(s);
        s->fifo_cache1_pull0 |= 0x00000100;
        s->fifo_cache1_method[s->fifo_cache1_put / 4] =
            (method << 2) | (subc << 13);
        s->fifo_cache1_data[s->fifo_cache1_put / 4] = param;
        s->fifo_cache1_put += 4;
        if (s->fifo_cache1_put == GEFORCE_CACHE1_SIZE * 4) {
            s->fifo_cache1_put = 0;
        }
        result = 1;
    }
    return result;
}

static void nv_fifo_process_chid(NV15State *s, uint32_t chid)
{
    if (s->fifo_wait) {
        return;
    }
    if ((s->fifo_mode & (1 << chid)) == 0) {
        return;
    }
    if ((s->fifo_cache1_push0 & 1) == 0) {
        return;
    }
    if ((s->fifo_cache1_pull0 & 1) == 0) {
        return;
    }
    uint32_t oldchid = s->fifo_cache1_push1 & 0x1F;
    if (oldchid == chid) {
        if (s->fifo_cache1_dma_put == s->fifo_cache1_dma_get) {
            return;
        }
    } else {
        if (nv_ramfc_read32(s, chid, 0x0) == nv_ramfc_read32(s, chid, 0x4)) {
            return;
        }
    }
    if (oldchid != chid) {
        nv_ramfc_write32(s, oldchid, 0x0, s->fifo_cache1_dma_put);
        nv_ramfc_write32(s, oldchid, 0x4, s->fifo_cache1_dma_get);
        nv_ramfc_write32(s, oldchid, 0x8, s->fifo_cache1_ref_cnt);
        nv_ramfc_write32(s, oldchid, 0xC, s->fifo_cache1_dma_instance);
        nv_ramfc_write32(s, oldchid, 0x2C, s->fifo_cache1_semaphore);
        s->fifo_cache1_dma_put = nv_ramfc_read32(s, chid, 0x0);
        s->fifo_cache1_dma_get = nv_ramfc_read32(s, chid, 0x4);
        s->fifo_cache1_ref_cnt = nv_ramfc_read32(s, chid, 0x8);
        s->fifo_cache1_dma_instance = nv_ramfc_read32(s, chid, 0xC);
        s->fifo_cache1_semaphore = nv_ramfc_read32(s, chid, 0x2C);
        s->fifo_cache1_push1 = (s->fifo_cache1_push1 & ~0x1F) | chid;
    }
    s->fifo_cache1_dma_push |= 0x100;
    if (s->fifo_cache1_dma_instance == 0) {
        return;
    }
    gf_channel *ch = &s->chs[chid];
    while (s->fifo_cache1_dma_get != s->fifo_cache1_dma_put) {
        uint32_t word = nv_dma_read32(s, s->fifo_cache1_dma_instance << 4,
                                      s->fifo_cache1_dma_get);
        s->fifo_cache1_dma_get += 4;
        if (ch->dma_state.mcnt) {
            int cmd_result = nv_execute_command(s, chid, ch->dma_state.subc,
                                                ch->dma_state.mthd, word);
            if (cmd_result <= 1) {
                if (!ch->dma_state.ni) {
                    ch->dma_state.mthd++;
                }
                ch->dma_state.mcnt--;
            } else {
                s->fifo_cache1_dma_get -= 4;
            }
            if (cmd_result != 0) {
                break;
            }
        } else {
            if ((word & 0xe0000003) == 0x20000000) {
                s->fifo_cache1_dma_get = word & 0x1fffffff;
            } else if ((word & 3) == 1) {
                s->fifo_cache1_dma_get = word & 0xfffffffc;
            } else if ((word & 3) == 2) {
                ch->subr_return = s->fifo_cache1_dma_get;
                ch->subr_active = true;
                s->fifo_cache1_dma_get = word & 0xfffffffc;
            } else if (word == 0x00020000) {
                s->fifo_cache1_dma_get = ch->subr_return;
                ch->subr_active = false;
            } else if ((word & 0xa0030003) == 0) {
                ch->dma_state.mthd = (word >> 2) & 0x7ff;
                ch->dma_state.subc = (word >> 13) & 7;
                ch->dma_state.mcnt = (word >> 18) & 0x7ff;
                ch->dma_state.ni = word & 0x40000000;
            } else {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "nv15: fifo unexpected word 0x%08x\n", word);
                break;
            }
        }
    }
}

static void nv_fifo_process(NV15State *s)
{
    uint32_t offset = (s->fifo_cache1_push1 & 0x1f) + 1;
    for (uint32_t i = 0; i < GEFORCE_CHANNEL_COUNT; i++) {
        nv_fifo_process_chid(s, (i + offset) & 0x1f);
    }
}

/* ===================================================================== */
/* Extended CRTC + legacy VGA I/O                                        */
/* ===================================================================== */

static uint8_t nv_svga_read_crtc(NV15State *s, unsigned index)
{
    if (index <= GEFORCE_CRTC_MAX) {
        return s->crtc.reg[index];
    }
    return 0xff;
}

static void nv_svga_write_crtc(NV15State *s, unsigned index, uint8_t value)
{
    bool update_cursor_addr = false;

    if (index == 0x1c) {
        if (!(s->crtc.reg[index] & 0x80) && (value & 0x80) != 0) {
            s->crtc_intr_en = 0x00000000;
            nv_update_irq_level(s);
        }
    } else if (index == 0x1d || index == 0x1e) {
        s->bank_base[index - 0x1d] = value * 0x8000;
    } else if (index == 0x2f || index == 0x30 || index == 0x31) {
        update_cursor_addr = true;
    } else if (index == 0x37 || index == 0x3f || index == 0x51) {
        int scl = (value & 0x20) != 0;
        int sda = (value & 0x10) != 0;
        if (index == 0x3f) {
            bitbang_i2c_set(&s->bbi2c, BITBANG_I2C_SCL, scl);
            int sda_in = bitbang_i2c_set(&s->bbi2c, BITBANG_I2C_SDA, sda);
            s->crtc.reg[0x3e] = (sda_in << 3) | (scl << 2);
        } else {
            s->crtc.reg[index - 1] = sda << 3 | scl << 2;
        }
    } else if (index == 0x58) {
        return;
    }

    if (index <= GEFORCE_CRTC_MAX) {
        s->crtc.reg[index] = value;
    }

    if (update_cursor_addr) {
        s->hw_cursor.enabled =
            (s->crtc.reg[0x31] & 0x01) || (s->crtc_cursor_config & 0x00000001);
        s->hw_cursor.vram =
            (s->crtc.reg[0x30] & 0x80) || (s->crtc_cursor_config & 0x00000100);
        s->hw_cursor.offset =
            (s->crtc.reg[0x31] >> 2 << 11) |
            (s->crtc.reg[0x30] & 0x7F) << 17 |
            s->crtc.reg[0x2f] << 24;
        s->hw_cursor.offset += s->crtc_cursor_offset;
        s->full_update_pending = true;
    }
}

static uint32_t nv_svga_read(NV15State *s, uint32_t address, unsigned io_len)
{
    if (address == 0x03C3 && io_len == 2) {
        return vga_ioport_read(&s->vga, address) |
               vga_ioport_read(&s->vga, address + 1) << 8;
    }

    if (address == 0x03d0 || address == 0x03d2) { /* RMA_ACCESS */
        if (io_len == 1) {
            return 0;
        }
        uint8_t crtc38 = s->crtc.reg[0x38];
        if (!(crtc38 & 1)) {
            return 0;
        }
        int rma_index = crtc38 >> 1;
        if (rma_index == 1) {
            return address == 0x03d0 ? s->rma_addr : s->rma_addr >> 16;
        } else if (rma_index == 2) {
            bool vram = false;
            uint32_t offset = s->rma_addr;
            if (s->rma_addr & 0x80000000) {
                vram = true;
                offset &= ~0x80000000;
            }
            if ((!vram && offset < GEFORCE_PNPMMIO_SIZE) ||
                (vram && offset < GEFORCE_VRAM_SIZE)) {
                uint32_t value = vram ? nv_vram_read32(s, offset)
                                      : nv_register_read32(s, offset);
                return address == 0x03d0 ? value : value >> 16;
            }
            return 0xFFFFFFFF;
        }
        return 0;
    }

    if ((io_len == 2) && ((address & 1) == 0)) {
        uint32_t value = (uint32_t)nv_svga_read(s, address, 1);
        value |= (uint32_t)nv_svga_read(s, address + 1, 1) << 8;
        return value;
    }

    switch (address) {
    case 0x03b4:
    case 0x03d4:
        return s->crtc.index;
    case 0x03b5:
    case 0x03d5:
        if (s->crtc.index > VGA_CRTC_MAX) {
            return nv_svga_read_crtc(s, s->crtc.index);
        }
        break;
    case 0x03c2:
        return 0x10; /* Monitor presence detection (DAC sensing) */
    default:
        break;
    }

    return vga_ioport_read(&s->vga, address);
}

static void nv_svga_write(NV15State *s, uint32_t address, uint32_t value,
                          unsigned io_len)
{
    if (address == 0x03d0 || address == 0x03d2) { /* RMA_ACCESS */
        if (io_len == 1) {
            return;
        }
        uint8_t crtc38 = s->crtc.reg[0x38];
        if (!(crtc38 & 1)) {
            return;
        }
        int rma_index = crtc38 >> 1;
        if (rma_index == 1) {
            if (address == 0x03d0) {
                if (io_len == 2) {
                    s->rma_addr &= 0xFFFF0000;
                    s->rma_addr |= value;
                } else {
                    s->rma_addr = value;
                }
            } else {
                s->rma_addr &= 0x0000FFFF;
                s->rma_addr |= value << 16;
            }
        } else if (rma_index == 3) {
            bool vram = false;
            uint32_t offset = s->rma_addr & ~3;
            if (s->rma_addr & 0x80000000) {
                vram = true;
                offset &= ~0x80000000;
            }
            if ((!vram && offset < GEFORCE_PNPMMIO_SIZE) ||
                (vram && offset < GEFORCE_VRAM_SIZE)) {
                if (address == 0x03d0) {
                    if (io_len == 2) {
                        uint32_t v32 = vram ? nv_vram_read32(s, offset)
                                            : nv_register_read32(s, offset);
                        v32 = (v32 & 0xFFFF0000) | value;
                        if (vram) {
                            nv_vram_write32(s, offset, v32);
                        } else {
                            nv_register_write32(s, offset, v32);
                        }
                    } else {
                        if (vram) {
                            nv_vram_write32(s, offset, value);
                        } else {
                            nv_register_write32(s, offset, value);
                        }
                    }
                } else {
                    uint32_t v32 = vram ? nv_vram_read32(s, offset)
                                        : nv_register_read32(s, offset);
                    v32 = (v32 & 0x0000FFFF) | (value << 16);
                    if (vram) {
                        nv_vram_write32(s, offset, v32);
                    } else {
                        nv_register_write32(s, offset, v32);
                    }
                }
            }
        }
        return;
    }

    if ((io_len == 2) && ((address & 1) == 0)) {
        nv_svga_write(s, address, value & 0xff, 1);
        nv_svga_write(s, address + 1, value >> 8, 1);
        return;
    }

    switch (address) {
    case 0x03b4:
    case 0x03d4:
        s->crtc.index = value;
        break;
    case 0x03b5:
    case 0x03d5:
        if (s->crtc.index == 0x01 || s->crtc.index == 0x07 ||
            s->crtc.index == 0x09 || s->crtc.index == 0x0c ||
            s->crtc.index == 0x0d || s->crtc.index == 0x12 ||
            s->crtc.index == 0x13 || s->crtc.index == 0x15 ||
            s->crtc.index == 0x19 || s->crtc.index == 0x25 ||
            s->crtc.index == 0x28 || s->crtc.index == 0x2D ||
            s->crtc.index == 0x41 || s->crtc.index == 0x42) {
            s->svga_needs_update_mode = 1;
        }
        if (s->crtc.index <= VGA_CRTC_MAX) {
            s->crtc.reg[s->crtc.index] = value;
        } else {
            nv_svga_write_crtc(s, s->crtc.index, value);
            return;
        }
        break;
    default:
        break;
    }

    vga_ioport_write(&s->vga, address, value);
}

/* I/O region at 0x3b0..0x3df dispatching to the NV SVGA handlers. */
static uint64_t nv_ioport_read(void *opaque, hwaddr addr, unsigned size)
{
    NV15State *s = opaque;
    return nv_svga_read(s, addr + 0x3b0, size);
}

static void nv_ioport_write(void *opaque, hwaddr addr, uint64_t val,
                            unsigned size)
{
    NV15State *s = opaque;
    nv_svga_write(s, addr + 0x3b0, val, size);
}

static const MemoryRegionOps nv_ioport_ops = {
    .read = nv_ioport_read,
    .write = nv_ioport_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 2,
    .impl.min_access_size = 1,
    .impl.max_access_size = 2,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

/* ===================================================================== */
/* BAR0 MMIO region                                                      */
/* ===================================================================== */

static uint64_t nv_mmio_read(void *opaque, hwaddr addr, unsigned size)
{
    NV15State *s = opaque;
    uint32_t offset = addr & (GEFORCE_PNPMMIO_SIZE - 1);
    uint64_t value;
    if (size == 1) {
        value = nv_register_read8(s, offset);
    } else if (size == 2) {
        value = (uint16_t)nv_register_read32(s, offset);
    } else if (size == 8) {
        value = (uint64_t)nv_register_read32(s, offset) |
                ((uint64_t)nv_register_read32(s, offset + 4) << 32);
    } else {
        value = nv_register_read32(s, offset);
    }
    NV_TRACE(s, "nv15 R  mmio+0x%06x /%u = 0x%0*" PRIx64 "\n",
             offset, size * 8, size * 2, value);
    return value;
}

static void nv_mmio_write(void *opaque, hwaddr addr, uint64_t data,
                          unsigned size)
{
    NV15State *s = opaque;
    uint32_t offset = addr & (GEFORCE_PNPMMIO_SIZE - 1);
    NV_TRACE(s, "nv15 W  mmio+0x%06x /%u = 0x%0*" PRIx64 "\n",
             offset, size * 8, size * 2, data);
    if (size == 1) {
        nv_register_write8(s, offset, data);
    } else if (size == 8) {
        nv_register_write32(s, offset, (uint32_t)data);
        nv_register_write32(s, offset + 4, data >> 32);
    } else {
        nv_register_write32(s, offset, data);
    }
}

static const MemoryRegionOps nv_mmio_ops = {
    .read = nv_mmio_read,
    .write = nv_mmio_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 8,
    .impl.min_access_size = 1,
    .impl.max_access_size = 8,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

/* ===================================================================== */
/* Display path                                                          */
/* ===================================================================== */

static const GraphicHwOps nv_hw_ops;
static GraphicHwOps nv_vga_hw_ops; /* the base VGACommonState ops */

static uint16_t nv_cursor_read16(NV15State *s, uint32_t address)
{
    return s->hw_cursor.vram ? nv_vram_read16(s, address)
                             : nv_ramin_read16(s, address);
}

static uint32_t nv_cursor_read32(NV15State *s, uint32_t address)
{
    return s->hw_cursor.vram ? nv_vram_read32(s, address)
                             : nv_ramin_read32(s, address);
}

/* Derive the current SVGA mode from the extended CRTC/RAMDAC file. */
static void nv_update_mode(NV15State *s)
{
    uint8_t crtc28 = s->crtc.reg[0x28] & 0x7F;

    uint32_t iTopOffset =
        s->crtc.reg[0x0d] |
        (s->crtc.reg[0x0c] << 8) |
        (s->crtc.reg[0x19] & 0x1F) << 16;
    iTopOffset <<= 2;
    iTopOffset += s->crtc_start;

    uint32_t iPitch =
        s->crtc.reg[0x13] |
        (s->crtc.reg[0x19] >> 5 << 8) |
        (s->crtc.reg[0x42] >> 6 & 1) << 11;
    iPitch <<= 3;

    uint8_t iBpp = 8;
    if (crtc28 == 0x02) {
        iBpp = 16;
    } else if (crtc28 == 0x03) {
        iBpp = 32;
    }

    uint32_t iWidth =
        (s->crtc.reg[1] + ((s->crtc.reg[0x2D] & 0x02) << 7) + 1) * 8;
    uint32_t iHeight =
        (s->crtc.reg[18] |
         ((s->crtc.reg[7] & 0x02) << 7) |
         ((s->crtc.reg[7] & 0x40) << 3) |
         ((s->crtc.reg[0x25] & 0x02) << 9) |
         ((s->crtc.reg[0x41] & 0x04) << 9)) + 1;

    s->svga_xres = iWidth;
    s->svga_yres = iHeight;
    s->svga_bpp = iBpp;
    s->svga_dispbpp = iBpp;
    s->disp_offset = iTopOffset;
    s->disp_end_offset = iTopOffset + iPitch * iHeight;
    s->svga_pitch = iPitch;
}

static inline void nv_put_rgb(uint8_t *dst, uint8_t r, uint8_t g, uint8_t b)
{
    *(uint32_t *)dst = ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

static void nv_draw_line8(void *opaque, uint8_t *dst, const uint8_t *src,
                          int width, int deststep)
{
    NV15State *s = opaque;
    for (int x = 0; x < width; x++) {
        uint8_t idx = src[x];
        uint8_t r = s->vga.palette[idx * 3 + 0];
        uint8_t g = s->vga.palette[idx * 3 + 1];
        uint8_t b = s->vga.palette[idx * 3 + 2];
        r = (r << 2) | (r >> 4);
        g = (g << 2) | (g >> 4);
        b = (b << 2) | (b >> 4);
        nv_put_rgb(dst, r, g, b);
        dst += 4;
    }
}

static void nv_draw_line16(void *opaque, uint8_t *dst, const uint8_t *src,
                           int width, int deststep)
{
    for (int x = 0; x < width; x++) {
        uint16_t v = src[0] | (src[1] << 8);
        uint8_t r, g, b;
        EXTRACT_565_TO_888(v, r, g, b);
        nv_put_rgb(dst, r, g, b);
        src += 2;
        dst += 4;
    }
}

static void nv_draw_line32(void *opaque, uint8_t *dst, const uint8_t *src,
                           int width, int deststep)
{
    for (int x = 0; x < width; x++) {
        nv_put_rgb(dst, src[2], src[1], src[0]);
        src += 4;
        dst += 4;
    }
}

/* Composite the hardware cursor over the finished surface. */
static void nv_draw_cursor(NV15State *s, DisplaySurface *surface)
{
    if (!s->hw_cursor.enabled) {
        return;
    }
    int size = s->hw_cursor.size;
    int hwcx = s->hw_cursor.x;
    int hwcy = s->hw_cursor.y;
    int sw = surface_width(surface);
    int sh = surface_height(surface);
    int stride = surface_stride(surface);
    uint8_t *data = surface_data(surface);
    unsigned cursor_color_bytes = s->hw_cursor.bpp32 ? 4 : 2;
    unsigned cpitch = s->hw_cursor.size * cursor_color_bytes;

    for (int cy = 0; cy < size; cy++) {
        int py = hwcy + cy;
        if (py < 0 || py >= sh) {
            continue;
        }
        uint32_t crow = s->hw_cursor.offset + cpitch * cy;
        for (int cx = 0; cx < size; cx++) {
            int px = hwcx + cx;
            if (px < 0 || px >= sw) {
                continue;
            }
            uint8_t *p = data + py * stride + px * 4;
            uint32_t d = *(uint32_t *)p;
            uint8_t dr = (d >> 16) & 0xff, dg = (d >> 8) & 0xff, db = d & 0xff;
            uint8_t r, g, b;
            if (s->hw_cursor.bpp32) {
                uint32_t cc = nv_cursor_read32(s, crow + cx * 4);
                if (cc != 0) {
                    uint8_t a, cr, cg, cb;
                    EXTRACT_8888_TO_8888(cc, a, cr, cg, cb);
                    uint8_t ica = 0xFF - a;
                    b = alpha_wrap(db * ica / 0xFF + cb);
                    g = alpha_wrap(dg * ica / 0xFF + cg);
                    r = alpha_wrap(dr * ica / 0xFF + cr);
                } else {
                    r = dr; g = dg; b = db;
                }
            } else {
                uint16_t cc = nv_cursor_read16(s, crow + cx * 2);
                uint8_t a, cr, cg, cb;
                EXTRACT_1555_TO_8888(cc, a, cr, cg, cb);
                if (a) {
                    b = cb; g = cg; r = cr;
                } else {
                    b = db ^ cb; g = dg ^ cg; r = dr ^ cr;
                }
            }
            nv_put_rgb(p, r, g, b);
        }
    }
}

static void nv_gfx_invalidate(void *opaque);

static bool nv_gfx_update(void *opaque)
{
    NV15State *s = opaque;
    VGACommonState *vga = &s->vga;
    uint8_t crtc28 = s->crtc.reg[0x28] & 0x7F;

    /*
     * The VGA core and the NV scanout each cache the console geometry; after
     * a switch the other side's frame is stale and the VGA core would not
     * resize a console it did not size itself.
     */
    if ((crtc28 != 0x00) != s->svga_scanout) {
        s->svga_scanout = crtc28 != 0x00;
        nv_gfx_invalidate(s);
    }

    if (crtc28 == 0x00) {
        return nv_vga_hw_ops.gfx_update ? nv_vga_hw_ops.gfx_update(vga) : true;
    }

    if (s->svga_needs_update_mode) {
        nv_update_mode(s);
    }

    DisplaySurface *surface = qemu_console_surface(vga->con);
    int width = s->svga_xres;
    int height = s->svga_yres;
    if (width == 0 || height == 0 || s->svga_pitch == 0) {
        return true;
    }

    bool full = s->full_update_pending;
    if (s->svga_needs_update_mode ||
        surface_width(surface) != width ||
        surface_height(surface) != height) {
        qemu_console_resize(vga->con, width, height);
        surface = qemu_console_surface(vga->con);
        s->fbsection_valid = false;
        full = true;
    }
    s->svga_needs_update_mode = 0;
    s->full_update_pending = false;

    drawfn fn;
    if (s->svga_bpp == 8) {
        fn = nv_draw_line8;
    } else if (s->svga_bpp == 16) {
        fn = nv_draw_line16;
    } else {
        fn = nv_draw_line32;
    }

    if (!s->fbsection_valid ||
        s->fbsection_base != s->disp_offset ||
        s->fbsection_rows != (unsigned)height ||
        s->fbsection_src_width != s->svga_pitch) {
        if (s->fbsection_valid && s->fbsection.mr) {
            memory_region_set_log(s->fbsection.mr, false, DIRTY_MEMORY_VGA);
            memory_region_unref(s->fbsection.mr);
        }
        /*
         * Build the framebuffer section directly on vga->vram.  The VRAM is a
         * subregion of the BAR1 aperture container (fb_aper), and
         * framebuffer_update_memory_section() resolves it with
         * memory_region_find(), which cannot locate a nested leaf and returns
         * a NULL section -- so framebuffer_update_display() drew nothing and
         * the screen stayed black (only the separately-composited hardware
         * cursor showed, and ghosted).  Point the section straight at the VRAM
         * region at the scanout offset and enable VGA dirty logging on it.
         */
        s->fbsection = (MemoryRegionSection) {
            .mr = &vga->vram,
            .offset_within_region = s->disp_offset,
            .size = int128_make64((uint64_t)s->svga_pitch * height),
        };
        memory_region_ref(&vga->vram);
        memory_region_set_log(&vga->vram, true, DIRTY_MEMORY_VGA);
        s->fbsection_valid = true;
        s->fbsection_base = s->disp_offset;
        s->fbsection_rows = height;
        s->fbsection_src_width = s->svga_pitch;
        full = true;
    }

    /*
     * The hardware cursor is composited directly into the display surface, so
     * the framebuffer under its previous position must be repainted or the
     * cursor leaves a trail.  Force a full framebuffer redraw whenever the
     * cursor is enabled so the overlay always starts from clean pixels.
     */
    if (s->hw_cursor.enabled) {
        full = true;
    }
    int first = 0, last = 0;
    framebuffer_update_display(surface, &s->fbsection, width, height,
                               s->svga_pitch, surface_stride(surface), 4,
                               full, fn, s, &first, &last);

    if (s->hw_cursor.enabled) {
        nv_draw_cursor(s, surface);
        dpy_gfx_update(vga->con, 0, 0, width, height);
    } else if (last >= first) {
        dpy_gfx_update(vga->con, 0, first, width, last - first + 1);
    }
    return true;
}

static void nv_gfx_invalidate(void *opaque)
{
    NV15State *s = opaque;
    s->full_update_pending = true;
    s->fbsection_valid = false;
    if (nv_vga_hw_ops.invalidate) {
        nv_vga_hw_ops.invalidate(&s->vga);
    }
}

/* ===================================================================== */
/* Periodic vblank / fifo-acquire retry                                  */
/* ===================================================================== */

static void nv_vblank_timer(void *opaque)
{
    NV15State *s = opaque;

    s->crtc_intr |= 0x00000001;
    nv_update_irq_level(s);

    if (s->fifo_wait_acquire) {
        s->fifo_wait_acquire = false;
        nv_update_fifo_wait(s);
        nv_fifo_process(s);
    }

    timer_mod(s->vertical_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              NANOSECONDS_PER_SECOND / NV_VBLANK_HZ);
}

static const GraphicHwOps nv_hw_ops = {
    .invalidate = nv_gfx_invalidate,
    .gfx_update = nv_gfx_update,
};

/* ===================================================================== */
/* Reset / realize / class                                               */
/* ===================================================================== */

static void nv_init_state(NV15State *s)
{
    s->card_type = GEFORCE_CARD_TYPE;
    s->crtc.index = GEFORCE_CRTC_MAX + 1;
    memset(s->crtc.reg, 0, sizeof(s->crtc.reg));

    s->mc_soft_intr = false;
    s->mc_intr_en = 0;
    s->mc_enable = 0;
    s->bus_intr = 0;
    s->bus_intr_en = 0;
    s->fifo_wait = false;
    s->fifo_wait_soft = false;
    s->fifo_wait_notify = false;
    s->fifo_wait_flip = false;
    s->fifo_wait_acquire = false;
    s->fifo_intr = 0;
    s->fifo_intr_en = 0;
    s->fifo_ramht = 0;
    s->fifo_ramfc = 0;
    s->fifo_ramro = 0;
    s->fifo_mode = 0;
    s->fifo_cache1_push0 = 0;
    s->fifo_cache1_push1 = 0;
    s->fifo_cache1_put = 0;
    s->fifo_cache1_dma_push = 0;
    s->fifo_cache1_dma_instance = 0;
    s->fifo_cache1_dma_put = 0;
    s->fifo_cache1_dma_get = 0;
    s->fifo_cache1_ref_cnt = 0;
    s->fifo_cache1_pull0 = 0;
    s->fifo_cache1_semaphore = 0;
    s->fifo_cache1_get = 0;
    s->fifo_grctx_instance = 0;
    memset(s->fifo_cache1_method, 0, sizeof(s->fifo_cache1_method));
    memset(s->fifo_cache1_data, 0, sizeof(s->fifo_cache1_data));
    s->rma_addr = 0;
    s->timer_intr = 0;
    s->timer_intr_en = 0;
    s->timer_num = 0;
    s->timer_den = 0;
    s->timer_inittime1 = 0;
    s->timer_inittime2 = 0;
    s->timer_alarm = 0;
    s->graph_intr = 0;
    s->graph_nsource = 0;
    s->graph_intr_en = 0;
    s->graph_ctx_switch1 = 0;
    s->graph_ctx_switch2 = 0;
    s->graph_ctx_switch4 = 0;
    s->graph_ctxctl_cur = 0;
    s->graph_status = 0;
    s->graph_trapped_addr = 0;
    s->graph_trapped_data = 0;
    s->graph_flip_read = 0;
    s->graph_flip_write = 0;
    s->graph_flip_modulo = 0;
    s->graph_notify = 0;
    s->graph_fifo = 0;
    s->graph_bpixel = 0;
    s->graph_channel_ctx_table = 0;
    s->graph_offset0 = 0;
    s->graph_pitch0 = 0;
    s->crtc_intr = 0;
    s->crtc_intr_en = 0;
    s->crtc_start = 0;
    s->crtc_config = 0;
    s->crtc_raster_pos = 0;
    s->crtc_cursor_offset = 0;
    s->crtc_cursor_config = 0;
    s->crtc_gpio_ext = 0;
    s->ramdac_cu_start_pos = 0;
    /*
     * Core (NVPLL) and memory (MPLL) clock PLL coefficients.  On a real
     * Quadro2 Pro the VBIOS programs these at POST; the pixel PLL (VPLL) is
     * (re)programmed by the driver, but NVPLL/MPLL are only read back.  With
     * no NVIDIA VBIOS to seed them, nv4_mini.sys reads a zero core/memory
     * clock during mode-set and asserts -> the retail exception unwinder then
     * bugchecks 0xD3 in RtlLookupFunctionEntry.  Seed sane non-zero
     * coefficients (~200 MHz).  Format matches VPLL: bits 0-7 M, 8-15 N,
     * 16-18 P; f = refclk * N / (M << P), refclk 13.5 MHz.
     */
    s->ramdac_nvpll = 0x0000c00d;   /* N=0xC0 M=0x0D P=0 -> ~199 MHz core */
    s->ramdac_mpll  = 0x0000c00d;   /* ~199 MHz memory                    */
    s->ramdac_vpll = 0;
    s->ramdac_vpll_b = 0;
    s->ramdac_pll_select = 0;
    s->ramdac_general_control = 0;
    s->dac_shift = 2;

    for (int i = 0; i < GEFORCE_CHANNEL_COUNT; i++) {
        g_free(s->chs[i].iifc_words);
        g_free(s->chs[i].sifc_words);
        g_free(s->chs[i].tfc_words);
        g_free(s->chs[i].gdi_words);
    }
    memset(s->chs, 0, sizeof(s->chs));
    for (int i = 0; i < GEFORCE_CHANNEL_COUNT; i++) {
        s->chs[i].swzs_color_bytes = 1;
        s->chs[i].s2d_color_bytes = 1;
        s->chs[i].d3d_color_bytes = 1;
        s->chs[i].d3d_depth_bytes = 1;
    }

    if (s->unk_regs) {
        memset(s->unk_regs, 0, sizeof(uint32_t) * (GEFORCE_PNPMMIO_SIZE / 4));
    }

    s->svga_unlock_special = 0;
    s->svga_needs_update_tile = 1;
    s->svga_needs_update_dispentire = 1;
    s->svga_needs_update_mode = 0;
    s->svga_double_width = 0;
    s->full_update_pending = true;
    s->fbsection_valid = false;

    s->svga_xres = 640;
    s->svga_yres = 480;
    s->svga_bpp = 8;
    s->svga_pitch = 640;
    s->bank_base[0] = 0;
    s->bank_base[1] = 0;

    s->hw_cursor.x = 0;
    s->hw_cursor.y = 0;
    s->hw_cursor.size = 32;
    s->hw_cursor.offset = 0;
    s->hw_cursor.bpp32 = false;
    s->hw_cursor.enabled = false;

    /* Guess, matching the Bochs NV15 straps. */
    s->straps0_primary_original = (0x7FF86C6B | 0x00000180);
    s->straps0_primary = s->straps0_primary_original;
    s->ramin_flip = GEFORCE_VRAM_SIZE - 64;
    s->memsize_mask = GEFORCE_VRAM_SIZE - 1;
    s->class_mask = 0x00000FFF;

    s->disp_offset = 0;
    s->disp_end_offset = 0;

    s->vclk[0] = 25180000;
    s->vclk[1] = 28325000;
    s->vclk[2] = 41165000;
    s->vclk[3] = 36082000;
}

static void nv_reset(DeviceState *dev)
{
    NV15State *s = NV15_VGA(dev);
    PCIDevice *pci = PCI_DEVICE(dev);
    nv_init_state(s);
    s->irq_level = false;
    /* Disable ROM shadowing to allow clearing of VRAM. */
    pci->config[0x50] = 0x00;
}

static void nv_setup_pci_caps(PCIDevice *dev, NV15State *s)
{
    uint8_t *c = dev->config;

    /* Subsystem identity: 10de:006d (Quadro2 Pro board). */
    pci_set_word(c + PCI_SUBSYSTEM_VENDOR_ID, s->subsys_vendor);
    pci_set_word(c + PCI_SUBSYSTEM_ID, s->subsys_id);
    /* Mirror at 0x40-0x43, as the chip reports. */
    pci_set_word(c + 0x40, s->subsys_vendor);
    pci_set_word(c + 0x42, s->subsys_id);

    /* Capabilities list present; status bits as the real chip. */
    c[PCI_STATUS] = 0xB0;
    c[PCI_STATUS + 1] = 0x02;
    c[PCI_CAPABILITY_LIST] = 0x60;

    /* AGP 2.0 capability at 0x44 (id 0x02, next 0x00). */
    c[0x44] = 0x02;
    c[0x45] = 0x00;
    c[0x46] = 0x20;
    c[0x47] = 0x00;
    c[0x48] = 0x07;
    c[0x49] = 0x00;
    c[0x4a] = 0x00;
    c[0x4b] = 0x1F;

    /* Power-management-ish block. */
    c[0x54] = 0x01;

    /* Power Management capability at 0x60 (id 0x01, next 0x44). */
    c[0x60] = 0x01;
    c[0x61] = 0x44;
    c[0x62] = 0x02;
    c[0x63] = 0x00;

    /* Allow the guest to program the AGP command register. */
    pci_set_long(dev->wmask + 0x48, 0xffffffff);
}

static void nv_realize(PCIDevice *dev, Error **errp)
{
    NV15State *s = NV15_VGA(dev);
    VGACommonState *vga = &s->vga;

    /* Opt-in diagnostic tracing of guest<->GPU traffic (GEFORCE_LOG=1),
     * emitted to the -D logfile.  Budget-capped so a stuck guest cannot
     * fill the disk. */
    s->log_traffic = getenv("GEFORCE_LOG") != NULL;
    s->log_budget = 50u * 1000u * 1000u;

    pci_set_word(dev->config + PCI_DEVICE_ID, s->dev_id);

    vga->vram_size_mb = GEFORCE_VRAM_SIZE / (1024 * 1024);
    if (!vga_common_init(vga, OBJECT(s), errp)) {
        return;
    }
    vga->vbe_legacy_mode_switch = true;
    /* Install the standard VGA memory window but NOT the standard ports;
     * NV drives the extended CRTC through its own I/O region below. */
    vga_init(vga, OBJECT(s), pci_address_space(dev),
             pci_address_space_io(dev), false);

    nv_vga_hw_ops = *vga->hw_ops;
    vga->con = graphic_console_init(DEVICE(s), 0, &nv_hw_ops, s);

    /* Our own legacy+extended I/O ports at 0x3b0-0x3df. */
    memory_region_init_io(&s->ioports, OBJECT(s), &nv_ioport_ops, s,
                          "nv15.ioports", 0x30);
    memory_region_add_subregion(pci_address_space_io(dev), 0x3b0, &s->ioports);

    /* BAR0: 16 MB MMIO register aperture. */
    memory_region_init_io(&s->mmio, OBJECT(s), &nv_mmio_ops, s,
                          "nv15.mmio", GEFORCE_PNPMMIO_SIZE);
    pci_register_bar(dev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->mmio);

    /* BAR1: 128 MB prefetchable framebuffer aperture over 64 MB VRAM. */
    memory_region_init(&s->fb_aper, OBJECT(dev), "nv15.fb",
                       GEFORCE_FB_APERTURE_SIZE);
    memory_region_add_subregion(&s->fb_aper, 0, &vga->vram);
    pci_register_bar(dev, 1,
                     PCI_BASE_ADDRESS_MEM_PREFETCH, &s->fb_aper);

    dev->config[PCI_INTERRUPT_PIN] = 1;

    nv_setup_pci_caps(dev, s);

    s->unk_regs = g_malloc0(sizeof(uint32_t) * (GEFORCE_PNPMMIO_SIZE / 4));

    nv2d_bitblt_init(s);
    nv3d_init_method_handlers(s);
    nv_init_state(s);

    /* DDC / EDID over the CRTC I2C bits. */
    s->i2c_bus = i2c_init_bus(DEVICE(s), "nv15.ddc");
    I2CDDCState *ddc = &s->ddc;
    i2c_slave_set_address(I2C_SLAVE(ddc), 0x50);
    qdev_realize(DEVICE(ddc), BUS(s->i2c_bus), &error_abort);
    bitbang_i2c_init(&s->bbi2c, s->i2c_bus);

    s->vertical_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, nv_vblank_timer, s);
    timer_mod(s->vertical_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              NANOSECONDS_PER_SECOND / NV_VBLANK_HZ);
}

static void nv_exit(PCIDevice *dev)
{
    NV15State *s = NV15_VGA(dev);

    if (s->vertical_timer) {
        timer_free(s->vertical_timer);
        s->vertical_timer = NULL;
    }
    if (s->fbsection_valid) {
        memory_region_unref(s->fbsection.mr);
        s->fbsection_valid = false;
    }
    for (int i = 0; i < GEFORCE_CHANNEL_COUNT; i++) {
        g_free(s->chs[i].iifc_words);
        g_free(s->chs[i].sifc_words);
        g_free(s->chs[i].tfc_words);
        g_free(s->chs[i].gdi_words);
    }
    g_free(s->unk_regs);
    s->unk_regs = NULL;
    graphic_console_close(s->vga.con);
}

static void nv_instance_init(Object *obj)
{
    NV15State *s = NV15_VGA(obj);
    object_initialize_child(obj, "ddc", &s->ddc, TYPE_I2CDDC);
}

static const Property nv_properties[] = {
    DEFINE_PROP_UINT16("x-device-id", NV15State, dev_id, 0x0153),
    DEFINE_PROP_UINT16("x-subsys-vendor", NV15State, subsys_vendor, 0x10de),
    DEFINE_PROP_UINT16("x-subsys-id", NV15State, subsys_id, 0x006d),
    DEFINE_EDID_PROPERTIES(NV15State, ddc.edid_info),
};

static void nv_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, nv_reset);
    device_class_set_props(dc, nv_properties);
    dc->vmsd = &vmstate_nv15;
    dc->hotpluggable = false;
    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);

    k->class_id = PCI_CLASS_DISPLAY_VGA;
    k->vendor_id = PCI_VENDOR_ID_NVIDIA_LOCAL;
    k->device_id = 0x0153;               /* Quadro2 Pro (NV15GL) */
    k->revision = 0x00;
    k->romfile = "vgabios-nv15gl.bin";
    k->realize = nv_realize;
    k->exit = nv_exit;
    k->config_write = nv_config_write;
}

static const TypeInfo nv15_vga_info = {
    .name = TYPE_NV15_VGA,
    .parent = TYPE_PCI_DEVICE,
    .instance_size = sizeof(NV15State),
    .instance_init = nv_instance_init,
    .class_init = nv_class_init,
    .interfaces = (const InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static void nv15_register_types(void)
{
    type_register_static(&nv15_vga_info);
}

type_init(nv15_register_types)
