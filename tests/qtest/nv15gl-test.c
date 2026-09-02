/*
 * QTest smoke test for the NVIDIA Quadro2 Pro (NV15GL) on the ia64-vpc machine.
 *
 * The card is opt-in (vga=nv15gl); the machine maps its BARs at fixed windows
 * (ia64_vpc_map_vga_fixed_windows()), so the register aperture and framebuffer
 * are reachable without firmware.  This locks in the device's PCI identity / BAR
 * geometry contract and a couple of live-aperture reads, so a future refactor
 * cannot silently change them.  It runs on the 460gx machine, which keeps the
 * graphics adapter on PCI0 at IA64_VPC_VGA_SLOT; the zx1 machine instead places
 * it behind the Mercury host bridge on a second root bus (see ia64_mercury.c),
 * which this device-model test does not need to exercise.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "libqos/generic-pcihost.h"
#include "libqos/pci.h"
#include "hw/pci/pci_ids.h"
#include "hw/pci/pci_regs.h"
#include "hw/ia64/ia64_vpc_abi.h"

/*
 * Fixed NVIDIA BAR windows inside the PCI0 MMIO aperture, matching
 * hw/ia64/ia64_vpc.c: FB at 0xF0000000 (128 MiB, BAR1), MMIO register aperture
 * at 0xF8000000 (16 MiB, BAR0), expansion ROM at 0xF9000000 (BAR6).
 */
#define IA64_NV15_FB_BASE     (IA64_PCI_MMIO_BASE + 0x02000000ULL)
#define IA64_NV15_MMIO_BASE   (IA64_PCI_MMIO_BASE + 0x0A000000ULL)
#define IA64_NV15_ROM_BASE    (IA64_PCI_MMIO_BASE + 0x0B000000ULL)
#define IA64_VPC_VGA_SLOT     5

#define NV15_VENDOR_ID        0x10deU
#define NV15_DEVICE_ID        0x0153U   /* Quadro2 Pro */
#define NV15_SUBSYS_VENDOR    0x10deU
#define NV15_SUBSYS_ID        0x006dU
/* PMC_BOOT_0 (MMIO offset 0): chipset id 0x15 (NV15) in bits [27:20]. */
#define NV15_PMC_BOOT_0       0x0000U
#define NV15_PMC_BOOT_0_VALUE 0x01500000U

static QTestState *nv15_start(void)
{
    return qtest_init("-machine 460gx,vga=nv15gl -m 256M -S");
}

/* The device realizes and the machine reaches the qtest stub. */
static void nv15_smoke(void)
{
    QTestState *qts = nv15_start();

    qtest_quit(qts);
}

/* PCI identity, class, subsystem id and fixed BAR geometry. */
static void nv15_pci_contract(void)
{
    QTestState *qts = nv15_start();
    QGenericPCIBus gbus;
    QPCIDevice *dev;

    qpci_init_generic(&gbus, qts, NULL, false);
    gbus.ecam_alloc_ptr = IA64_PCI_CONFIG_BASE;
    gbus.gpex_pio_base = IA64_PCI_IO_BASE;

    dev = qpci_device_find(&gbus.bus, QPCI_DEVFN(IA64_VPC_VGA_SLOT, 0));
    g_assert_nonnull(dev);
    g_assert_cmphex(qpci_config_readw(dev, PCI_VENDOR_ID), ==, NV15_VENDOR_ID);
    g_assert_cmphex(qpci_config_readw(dev, PCI_DEVICE_ID), ==, NV15_DEVICE_ID);
    g_assert_cmphex(qpci_config_readw(dev, PCI_CLASS_DEVICE), ==,
                    PCI_CLASS_DISPLAY_VGA);
    g_assert_cmphex(qpci_config_readw(dev, PCI_SUBSYSTEM_VENDOR_ID), ==,
                    NV15_SUBSYS_VENDOR);
    g_assert_cmphex(qpci_config_readw(dev, PCI_SUBSYSTEM_ID), ==,
                    NV15_SUBSYS_ID);

    /* BAR0: non-prefetchable MMIO register aperture. */
    g_assert_cmphex(qpci_config_readl(dev, PCI_BASE_ADDRESS_0), ==,
                    (uint32_t)IA64_NV15_MMIO_BASE |
                    PCI_BASE_ADDRESS_SPACE_MEMORY);
    /* BAR1: prefetchable framebuffer aperture. */
    g_assert_cmphex(qpci_config_readl(dev, PCI_BASE_ADDRESS_1), ==,
                    (uint32_t)IA64_NV15_FB_BASE |
                    PCI_BASE_ADDRESS_SPACE_MEMORY |
                    PCI_BASE_ADDRESS_MEM_PREFETCH);
    /* Expansion ROM (the NV15GL video BIOS is read from the ROM BAR). */
    g_assert_cmphex(qpci_config_readl(dev, PCI_ROM_ADDRESS) &
                    PCI_ROM_ADDRESS_MASK, ==, (uint32_t)IA64_NV15_ROM_BASE);

    g_free(dev);
    qtest_quit(qts);
}

/* The MMIO register aperture is live and identifies the NV15 chip. */
static void nv15_pmc_boot0(void)
{
    QTestState *qts = nv15_start();

    g_assert_cmphex(qtest_readl(qts, IA64_NV15_MMIO_BASE + NV15_PMC_BOOT_0),
                    ==, NV15_PMC_BOOT_0_VALUE);
    qtest_quit(qts);
}

/* The framebuffer BAR maps VRAM: writes read back verbatim. */
static void nv15_framebuffer(void)
{
    QTestState *qts = nv15_start();
    const uint32_t magic = 0xa5c30f69U;

    qtest_writel(qts, IA64_NV15_FB_BASE, magic);
    qtest_writel(qts, IA64_NV15_FB_BASE + 0x1000, ~magic);
    g_assert_cmphex(qtest_readl(qts, IA64_NV15_FB_BASE), ==, magic);
    g_assert_cmphex(qtest_readl(qts, IA64_NV15_FB_BASE + 0x1000), ==, ~magic);
    qtest_quit(qts);
}

/* ====================================================================== *
 * FIFO / PGRAPH 2D command-engine tests.
 *
 * The tests above only prove the card's PCI/BAR identity.  These drive the
 * 2D engine the way the guest driver does: build RAMHT and the graphics /
 * DMA objects in RAMIN, then submit methods through the PIO FIFO, exercising
 * the puller -> RAMHT lookup -> class dispatch -> 2D op path.
 *
 * Programming model, per the envytools "nVidia Hardware Documentation"
 * (docs/nVidia Quadro2 Pro/, chapter 2, "RAMHT and the FIFO objects" and the
 * PGRAPH object-class table):
 *   - RAMHT is a hash table mapping a 32-bit handle to an engine id and an
 *     object's RAMIN address; the stored selector is shifted left by 4.
 *   - method 0 ("SetObject") binds the looked-up object to a subchannel;
 *     the object's class (8-bit on NV4:NV25, which NV15 is) selects the op.
 *   - Object classes used here, straight from that table:
 *       0x003d DMA read/write   0x0062 NV10_SURF2D   0x005e NV4_RECT
 *       0x0066 NV5_SIFC         0x0061 NV4_IFC       0x0064 NV5_INDEX (IIFC)
 *
 * RAMIN is reachable through the BAR0 window at +0x700000; the PIO FIFO
 * "USER" submission area is the BAR0 window at +0x800000, where a method
 * write lands at (chid << 16) | (subchannel << 13) | (method << 2).  Method
 * numbers below are dword indices, matching hw/display/geforce.c.
 * ====================================================================== */

#define NV15_RAMIN_WINDOW   0x00700000U
#define NV15_USER_WINDOW    0x00800000U
#define NV15_PFIFO_RAMHT    0x00002210U   /* RAMHT base/size register */

#define NV_CLASS_DMA        0x3dU
#define NV_CLASS_SURF2D     0x62U
#define NV_CLASS_RECT       0x5eU
#define NV_CLASS_SIFC       0x66U
#define NV_CLASS_IFC        0x61U
#define NV_CLASS_IIFC       0x64U
#define NV_ENGINE_GRAPH     0x01U

/* RAMIN layout built by these tests (byte offsets into RAMIN). */
#define NV15_RAMIN_RAMHT    0x1000U        /* 512-entry hash table       */
#define NV15_RAMIN_SURF     0x2000U
#define NV15_RAMIN_DMA      0x2100U
#define NV15_RAMIN_RECT     0x2200U
#define NV15_RAMIN_SIFC     0x2300U
#define NV15_RAMIN_IFC      0x2400U
#define NV15_RAMIN_IIFC     0x2500U

/* fifo_ramht = 0x10: table at RAMIN 0x1000, 2^9 entries (size field 0). */
#define NV15_RAMHT_CFG      0x00000010U
#define NV15_RAMHT_BITS     9

/* Arbitrary distinct object handles. */
#define NV15_H_SURF         0xbeef0001U
#define NV15_H_DMA          0xbeef0002U
#define NV15_H_RECT         0xbeef0003U
#define NV15_H_SIFC         0xbeef0004U
#define NV15_H_IFC          0xbeef0005U
#define NV15_H_IIFC         0xbeef0006U

static void nv_ramin_w(QTestState *qts, uint32_t off, uint32_t val)
{
    qtest_writel(qts, IA64_NV15_MMIO_BASE + NV15_RAMIN_WINDOW + off, val);
}

static uint32_t nv_ramin_r(QTestState *qts, uint32_t off)
{
    return qtest_readl(qts, IA64_NV15_MMIO_BASE + NV15_RAMIN_WINDOW + off);
}

/* Submit one PIO method to (chid, subchannel). */
static void nv_method(QTestState *qts, uint32_t chid, uint32_t subc,
                      uint32_t method, uint32_t param)
{
    uint64_t addr = IA64_NV15_MMIO_BASE + NV15_USER_WINDOW +
                    ((uint64_t)chid << 16) + ((uint64_t)subc << 13) +
                    ((uint64_t)method << 2);
    qtest_writel(qts, addr, param);
}

/* Reproduces nv_ramht_lookup()'s hash: fold the handle in bits-sized
 * chunks, mix in the channel id, then scale to the 8-byte entry stride. */
static uint32_t nv_ramht_hash(uint32_t handle, uint32_t chid, uint32_t bits)
{
    uint32_t hash = 0;
    uint32_t x = handle;

    while (x) {
        hash ^= x & ((1U << bits) - 1);
        x >>= bits;
    }
    hash ^= (chid & 0xf) << (bits - 4);
    return hash << 3;
}

/* Insert a RAMHT entry, forward-probing for a free slot exactly like the
 * lookup does, so colliding handles still resolve. */
static void nv_ramht_insert(QTestState *qts, uint32_t handle, uint32_t chid,
                            uint32_t engine, uint32_t obj_off)
{
    uint32_t size = (1U << NV15_RAMHT_BITS) << 3;
    uint32_t it = nv_ramht_hash(handle, chid, NV15_RAMHT_BITS);
    uint32_t start = it;

    while (nv_ramin_r(qts, NV15_RAMIN_RAMHT + it) != 0) {
        it = (it + 8) % size;
        g_assert_cmpuint(it, !=, start);   /* table full: test bug */
    }
    nv_ramin_w(qts, NV15_RAMIN_RAMHT + it, handle);
    nv_ramin_w(qts, NV15_RAMIN_RAMHT + it + 4,
               (chid << 24) | (engine << 16) | (obj_off >> 4));
}

/* A graphics object is just its class byte in word0 (+ zeroed context). */
static void nv_make_gr_object(QTestState *qts, uint32_t off, uint8_t cls)
{
    nv_ramin_w(qts, off + 0x0, cls);
    nv_ramin_w(qts, off + 0x4, 0);
    nv_ramin_w(qts, off + 0x8, 0);
}

/* A linear VRAM DMA object with base 0 and adjust 0 (flag bit 0x2000). */
static void nv_make_dma_object(QTestState *qts, uint32_t off)
{
    nv_ramin_w(qts, off + 0x0, 0x00002000);
    nv_ramin_w(qts, off + 0x4, 0);
    nv_ramin_w(qts, off + 0x8, 0);
}

/* Clear the hash table and point PFIFO at it. */
static void nv_engine_reset_ramht(QTestState *qts)
{
    uint32_t i;

    for (i = 0; i < ((1U << NV15_RAMHT_BITS) << 3); i += 4) {
        nv_ramin_w(qts, NV15_RAMIN_RAMHT + i, 0);
    }
    qtest_writel(qts, IA64_NV15_MMIO_BASE + NV15_PFIFO_RAMHT, NV15_RAMHT_CFG);
}

/* Liveness check: a crash in the engine kills QEMU, so a good register read
 * after the pathological submission proves the guard held. */
static void nv_assert_alive(QTestState *qts)
{
    g_assert_cmphex(qtest_readl(qts, IA64_NV15_MMIO_BASE + NV15_PMC_BOOT_0),
                    ==, NV15_PMC_BOOT_0_VALUE);
}

/*
 * Smoke: a NV4_RECT solid fill through a NV10_SURF2D destination surface
 * must land in VRAM and be visible through the framebuffer BAR.  This walks
 * the whole PIO -> RAMHT -> SetObject -> 2D dispatch -> DMA-object write path.
 */
static void nv15_fifo_solid_rect(void)
{
    QTestState *qts = nv15_start();
    const uint32_t color = 0xaabbccddU;
    const uint32_t dst_ofs = 0x00010000U;   /* surface origin in VRAM  */
    const uint32_t pitch = 0x0400U;
    const uint32_t width = 4, height = 2;
    uint32_t x, y;

    nv_engine_reset_ramht(qts);

    nv_make_gr_object(qts, NV15_RAMIN_SURF, NV_CLASS_SURF2D);
    nv_make_dma_object(qts, NV15_RAMIN_DMA);
    nv_make_gr_object(qts, NV15_RAMIN_RECT, NV_CLASS_RECT);

    nv_ramht_insert(qts, NV15_H_SURF, 0, NV_ENGINE_GRAPH, NV15_RAMIN_SURF);
    nv_ramht_insert(qts, NV15_H_DMA,  0, NV_ENGINE_GRAPH, NV15_RAMIN_DMA);
    nv_ramht_insert(qts, NV15_H_RECT, 0, NV_ENGINE_GRAPH, NV15_RAMIN_RECT);

    /* Subchannel 0: SURFACE_2D -> A8R8G8B8, pitch, dst DMA object + offset. */
    nv_method(qts, 0, 0, 0x000, NV15_H_SURF);
    nv_method(qts, 0, 0, 0x0c0, 0x0a);                  /* A8R8G8B8, 4 bpp  */
    nv_method(qts, 0, 0, 0x0c1, (pitch << 16) | pitch);
    nv_method(qts, 0, 0, 0x062, NV15_H_DMA);            /* SetContextDmaDst */
    nv_method(qts, 0, 0, 0x0c3, dst_ofs);               /* dst offset       */

    /* Subchannel 1: NV4_RECT -> copy op, colour, one filled rectangle. */
    nv_method(qts, 0, 1, 0x000, NV15_H_RECT);
    nv_method(qts, 0, 1, 0x0bf, 3);                     /* SRCCOPY -> copy  */
    nv_method(qts, 0, 1, 0x0c1, color);
    nv_method(qts, 0, 1, 0x100, 0);                     /* (y<<16 | x) = 0  */
    nv_method(qts, 0, 1, 0x101, (height << 16) | width);/* size -> renders  */

    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++) {
            uint32_t off = dst_ofs + y * pitch + x * 4;
            g_assert_cmphex(qtest_readl(qts, IA64_NV15_FB_BASE + off),
                            ==, color);
        }
    }
    /* The pixel one past the rectangle must be untouched. */
    g_assert_cmphex(qtest_readl(qts, IA64_NV15_FB_BASE + dst_ofs + width * 4),
                    ==, 0);
    qtest_quit(qts);
}

/*
 * Crash regression (hardening H1): SIFC with zero dxds/dydt scale factors.
 * The engine computes 2^40 / scale; a zero divisor must be rejected before
 * the divide, not fault the host.
 */
static void nv15_sifc_zero_scale(void)
{
    QTestState *qts = nv15_start();

    nv_engine_reset_ramht(qts);
    nv_make_gr_object(qts, NV15_RAMIN_SURF, NV_CLASS_SURF2D);
    nv_make_gr_object(qts, NV15_RAMIN_SIFC, NV_CLASS_SIFC);
    nv_ramht_insert(qts, NV15_H_SURF, 0, NV_ENGINE_GRAPH, NV15_RAMIN_SURF);
    nv_ramht_insert(qts, NV15_H_SIFC, 0, NV_ENGINE_GRAPH, NV15_RAMIN_SIFC);

    /* A bound surface so the SIFC pixel size resolves non-zero. */
    nv_method(qts, 0, 0, 0x000, NV15_H_SURF);
    nv_method(qts, 0, 0, 0x0c0, 0x0a);           /* A8R8G8B8 */

    /* SIFC: a 1x1 source with a colour format, but dxds/dydt left at 0. */
    nv_method(qts, 0, 1, 0x000, NV15_H_SIFC);
    nv_method(qts, 0, 1, 0x0bf, 3);              /* operation           */
    nv_method(qts, 0, 1, 0x0c0, 4);              /* A8R8G8B8 -> 4 bytes  */
    nv_method(qts, 0, 1, 0x0c1, 0x00010001);     /* source h<<16 | w     */
    /* 0x0c2 (dxds) and 0x0c3 (dydt) deliberately omitted -> both zero.  */
    nv_method(qts, 0, 1, 0x0c6, 0);              /* source point: alloc  */
    nv_method(qts, 0, 1, 0x100, 0);              /* data word -> runs op */

    nv_assert_alive(qts);
    qtest_quit(qts);
}

/*
 * Crash regression (hardening H1): an unknown IFC colour format leaves the
 * pixel size 0; the "4 / colour_bytes" pixels-per-word divide must be
 * guarded.
 */
static void nv15_ifc_unknown_format(void)
{
    QTestState *qts = nv15_start();

    nv_engine_reset_ramht(qts);
    nv_make_gr_object(qts, NV15_RAMIN_IFC, NV_CLASS_IFC);
    nv_ramht_insert(qts, NV15_H_IFC, 0, NV_ENGINE_GRAPH, NV15_RAMIN_IFC);

    nv_method(qts, 0, 0, 0x000, NV15_H_IFC);
    nv_method(qts, 0, 0, 0x0c0, 0x00ff);         /* SET_COLOR_FORMAT: bad */

    nv_assert_alive(qts);
    qtest_quit(qts);
}

/*
 * Crash regression (hardening H2): an image-data method arriving before any
 * SET_SIZE/allocation must not dereference the NULL word buffer.
 */
static void nv15_iifc_data_without_alloc(void)
{
    QTestState *qts = nv15_start();

    nv_engine_reset_ramht(qts);
    nv_make_gr_object(qts, NV15_RAMIN_IIFC, NV_CLASS_IIFC);
    nv_ramht_insert(qts, NV15_H_IIFC, 0, NV_ENGINE_GRAPH, NV15_RAMIN_IIFC);

    nv_method(qts, 0, 0, 0x000, NV15_H_IIFC);
    nv_method(qts, 0, 0, 0x100, 0xdeadbeef);     /* data before any size  */

    nv_assert_alive(qts);
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    if (g_str_equal(qtest_get_arch(), "ia64") &&
        qtest_has_device("nv15gl-vga")) {
        qtest_add_func("/nv15gl/smoke", nv15_smoke);
        qtest_add_func("/nv15gl/pci-contract", nv15_pci_contract);
        qtest_add_func("/nv15gl/pmc-boot0", nv15_pmc_boot0);
        qtest_add_func("/nv15gl/framebuffer", nv15_framebuffer);
        qtest_add_func("/nv15gl/fifo-solid-rect", nv15_fifo_solid_rect);
        qtest_add_func("/nv15gl/sifc-zero-scale", nv15_sifc_zero_scale);
        qtest_add_func("/nv15gl/ifc-unknown-format", nv15_ifc_unknown_format);
        qtest_add_func("/nv15gl/iifc-data-without-alloc",
                       nv15_iifc_data_without_alloc);
    }

    return g_test_run();
}
