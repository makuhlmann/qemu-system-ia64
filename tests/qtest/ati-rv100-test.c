/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * QTest cases for the ATI RV100 (Radeon 7000/VE) that zx1 seats with
 * vga=rv100: the adapter of the rx2600's iLO MP card.  The expected values
 * are that card's (rx2600 run 4, 2026-10-10: configuration space, BAR sizes,
 * memory type).
 */

#include "qemu/osdep.h"
#include "hw/pci/pci_regs.h"
#include "hw/ia64/ia64_vpc_abi.h"
#include "libqtest.h"

#define RV100_ARGS      "-machine zx1,vga=rv100 -m 256M -S"
#define RV100_DEVFN     ((IA64_MERCURY_VGA_SLOT << 3) | 0)
#define RV100_CFG(reg)  (IA64_PCI_CONFIG_BASE + \
                         ((uint64_t)IA64_MERCURY_BUS << 20) + \
                         ((uint64_t)RV100_DEVFN << 12) + (reg))

static uint32_t rv100_cfg_readl(QTestState *qts, uint8_t reg)
{
    return qtest_readl(qts, RV100_CFG(reg));
}

static void rv100_cfg_writel(QTestState *qts, uint8_t reg, uint32_t value)
{
    qtest_writel(qts, RV100_CFG(reg), value);
}

static uint64_t rv100_mmio(QTestState *qts)
{
    return rv100_cfg_readl(qts, PCI_BASE_ADDRESS_2) & PCI_BASE_ADDRESS_MEM_MASK;
}

/* Configuration space 00h-63h as the MP card's RV100 shows it. */
static void test_rv100_config_image(void)
{
    static const struct {
        uint8_t reg;
        uint32_t sized;
    } bars[] = {
        { PCI_BASE_ADDRESS_0, 0xfc000008 },     /* 64 MiB, prefetchable */
        { PCI_BASE_ADDRESS_1, 0xffffff01 },     /* I/O, 256 bytes */
        { PCI_BASE_ADDRESS_2, 0xffff0000 },     /* registers, 64 KiB */
        { PCI_BASE_ADDRESS_3, 0x00000000 },
        { PCI_ROM_ADDRESS, 0xfffe0000 },        /* 128 KiB */
    };
    QTestState *qts = qtest_init(RV100_ARGS);
    uint8_t reg;
    size_t i;

    g_assert_cmphex(rv100_cfg_readl(qts, PCI_VENDOR_ID), ==, 0x51591002);
    g_assert_cmphex(rv100_cfg_readl(qts, PCI_COMMAND) >> 16, ==, 0x0290);
    g_assert_cmphex(rv100_cfg_readl(qts, PCI_REVISION_ID), ==, 0x03000000);
    g_assert_cmphex((rv100_cfg_readl(qts, PCI_CACHE_LINE_SIZE) >> 16) & 0xff,
                    ==, 0x00);
    g_assert_cmphex(rv100_cfg_readl(qts, PCI_SUBSYSTEM_VENDOR_ID), ==,
                    0x1292103c);
    g_assert_cmphex(rv100_cfg_readl(qts, PCI_CAPABILITY_LIST) & 0xff, ==, 0x50);
    g_assert_cmphex(rv100_cfg_readl(qts, PCI_INTERRUPT_LINE) >> 8, ==,
                    0x000801);
    for (reg = 0x40; reg < 0x4c; reg += 4) {
        g_assert_cmphex(rv100_cfg_readl(qts, reg), ==, 0);
    }
    g_assert_cmphex(rv100_cfg_readl(qts, 0x4c), ==, 0x1292103c);
    g_assert_cmphex(rv100_cfg_readl(qts, 0x50), ==, 0x06020001);
    g_assert_cmphex(rv100_cfg_readl(qts, 0x54), ==, 0);
    g_assert_cmphex(rv100_cfg_readl(qts, 0x58), ==, 0x00205002);
    g_assert_cmphex(rv100_cfg_readl(qts, 0x5c), ==, 0x2f000200);
    g_assert_cmphex(rv100_cfg_readl(qts, 0x60), ==, 0x00000200);

    /* Only the power state field of PMCSR is writable. */
    rv100_cfg_writel(qts, 0x54, 0xffffffff);
    g_assert_cmphex(rv100_cfg_readl(qts, 0x54) & 0xffff, ==,
                    PCI_PM_CTRL_STATE_MASK);
    rv100_cfg_writel(qts, 0x54, 0);
    /* The AGP block outside the list: identifier and status are read-only. */
    rv100_cfg_writel(qts, 0x58, 0xffffffff);
    rv100_cfg_writel(qts, 0x5c, 0xffffffff);
    g_assert_cmphex(rv100_cfg_readl(qts, 0x58), ==, 0x00205002);
    g_assert_cmphex(rv100_cfg_readl(qts, 0x5c), ==, 0x2f000200);

    for (i = 0; i < G_N_ELEMENTS(bars); i++) {
        uint32_t saved = rv100_cfg_readl(qts, bars[i].reg);

        rv100_cfg_writel(qts, bars[i].reg, 0xffffffff);
        g_assert_cmphex(rv100_cfg_readl(qts, bars[i].reg) &
                        (bars[i].reg == PCI_ROM_ADDRESS ? ~1u : ~0u), ==,
                        bars[i].sized);
        rv100_cfg_writel(qts, bars[i].reg, saved);
    }
    qtest_quit(qts);
}

/*
 * The Radeon register BAR: 2000h-3FFFh are registers of their own, not the
 * Rage 128's mirror of 0000h-1FFFh, and 4000h-FFFFh read 0.  The memory
 * reports 16 MB of DDR.
 */
static void test_rv100_register_aperture(void)
{
    QTestState *qts = qtest_init(RV100_ARGS);
    uint64_t mmio = rv100_mmio(qts);

    g_assert_cmphex(mmio, !=, 0);
    /* BIOS_0_SCRATCH (10h) keeps what is written to it. */
    qtest_writel(qts, mmio + 0x0010, 0x11223344);
    g_assert_cmphex(qtest_readl(qts, mmio + 0x0010), ==, 0x11223344);
    qtest_writel(qts, mmio + 0x2010, 0x55667788);
    g_assert_cmphex(qtest_readl(qts, mmio + 0x0010), ==, 0x11223344);
    qtest_writel(qts, mmio + 0x4010, 0x99aabbcc);
    g_assert_cmphex(qtest_readl(qts, mmio + 0x4010), ==, 0);
    g_assert_cmphex(qtest_readl(qts, mmio + 0xfffc), ==, 0);
    g_assert_cmphex(qtest_readl(qts, mmio + 0x0010), ==, 0x11223344);

    /* CONFIG_MEMSIZE (F8h) and MEM_SDRAM_MODE_REG (158h) bit 30, DDR. */
    g_assert_cmphex(qtest_readl(qts, mmio + 0x00f8), ==, 16 * 1024 * 1024);
    g_assert_cmphex(qtest_readl(qts, mmio + 0x0158) & (1u << 30), !=, 0);
    qtest_quit(qts);
}

/* XP 2600, the 460GX's Windows, has no Radeon driver: 460gx refuses it. */
static void test_rv100_not_on_460gx(void)
{
    if (g_test_subprocess()) {
        QTestState *qts = qtest_init("-machine 460gx,vga=rv100 -S");

        qtest_quit(qts);
        return;
    }
    g_test_trap_subprocess(NULL, 0, 0);
    g_test_trap_assert_failed();
    g_test_trap_assert_stderr("*vga=rv100 is the rx2600's adapter (zx1 only)*");
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/ati-rv100/config-image", test_rv100_config_image);
    qtest_add_func("/ati-rv100/register-aperture",
                   test_rv100_register_aperture);
    qtest_add_func("/ati-rv100/not-on-460gx", test_rv100_not_on_460gx);

    return g_test_run();
}
