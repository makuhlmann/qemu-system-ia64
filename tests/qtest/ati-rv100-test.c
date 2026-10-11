/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * QTest cases for the ATI RV100 (Radeon 7000/VE) that zx1 seats with
 * vga=rv100: the adapter of the rx2600's iLO MP card.  The expected values
 * are that card's (rx2600 run 4, 2026-10-10: configuration space, BAR sizes,
 * memory type).
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
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

/*
 * The Radeon legacy BIOS tables behind the ATI signature: header through 48h
 * with its size byte at +06h, the PLL block through +30h with the MP card's
 * values (reference 27.00 MHz, divider 60, 120-350 MHz), and the connector
 * table through +50h with one VGA connector (CRT, DDC_VGA, primary DAC).  No
 * other table pointer is set.  limit: the bytes a driver maps.
 */
static void check_radeon_tables(const uint8_t *rom, uint32_t limit)
{
    uint32_t declared = rom[2] * 512U;
    uint32_t hdr, pll, conn, i;
    uint8_t sum = 0;

    g_assert_cmphex(lduw_le_p(rom), ==, 0xaa55);
    g_assert_cmpmem(rom + 0x30, 10, " 761295520", 10);
    hdr = lduw_le_p(rom + 0x48);
    g_assert_cmphex(hdr, >=, 0x4a);
    g_assert_cmphex(rom[hdr + 0x06], ==, 0x80);
    pll = lduw_le_p(rom + hdr + 0x30);
    conn = lduw_le_p(rom + hdr + 0x50);
    for (i = 0x0c; i < 0x80; i += 2) {
        if (i != 0x30 && i != 0x50) {
            g_assert_cmphex(lduw_le_p(rom + hdr + i), ==, 0);
        }
    }
    g_assert_cmphex(hdr + 0x80, <=, limit);
    g_assert_cmphex(pll + 0x32, <=, limit);
    g_assert_cmphex(conn + 6, <=, limit);

    g_assert_cmpuint(lduw_le_p(rom + pll + 0x08), ==, 16600);
    g_assert_cmpuint(lduw_le_p(rom + pll + 0x0a), ==, 16600);
    g_assert_cmpuint(lduw_le_p(rom + pll + 0x0e), ==, 2700);
    g_assert_cmpuint(lduw_le_p(rom + pll + 0x10), ==, 60);
    g_assert_cmpuint(ldl_le_p(rom + pll + 0x12), ==, 12000);
    g_assert_cmpuint(ldl_le_p(rom + pll + 0x16), ==, 35000);

    g_assert_cmphex(rom[conn], ==, 0x10);
    g_assert_cmphex(rom[conn + 1], ==, 0x01);
    g_assert_cmphex(lduw_le_p(rom + conn + 2), ==, 0x2300);
    g_assert_cmphex(lduw_le_p(rom + conn + 4), ==, 0);

    for (i = 0; i < declared; i++) {
        sum += rom[i];
    }
    g_assert_cmphex(sum, ==, 0);
}

/* The C0000h ROM: ati2mtag maps 800h bytes of it (GetVgaEnabledResources). */
static void test_rv100_int10_rom_tables(void)
{
    QTestState *qts = qtest_init(RV100_ARGS);
    uint8_t rom[0x800];

    qtest_memread(qts, 0xc0000, rom, sizeof(rom));
    g_assert_cmphex(rom[2], ==, sizeof(rom) / 512);
    check_radeon_tables(rom, sizeof(rom));
    qtest_quit(qts);
}

/* The ROM BAR image, with decode enabled the way pci.sys does it. */
static void test_rv100_rom_bar_tables(void)
{
    QTestState *qts = qtest_init(RV100_ARGS);
    uint32_t bar = rv100_cfg_readl(qts, PCI_ROM_ADDRESS) & PCI_ROM_ADDRESS_MASK;
    g_autofree uint8_t *rom = g_malloc(0x20000);
    uint32_t pcir;

    g_assert_cmphex(bar, !=, 0);
    rv100_cfg_writel(qts, PCI_ROM_ADDRESS, bar | PCI_ROM_ADDRESS_ENABLE);
    qtest_memread(qts, bar, rom, 0x20000);
    check_radeon_tables(rom, 0x2000);
    pcir = lduw_le_p(rom + 0x18);
    g_assert_cmpmem(rom + pcir, 4, "PCIR", 4);
    g_assert_cmphex(ldl_le_p(rom + pcir + 4), ==, 0x51591002);
    qtest_quit(qts);
}

/* XP 2600, the 460GX's Windows, has no Radeon driver: 460gx refuses it. */
static void test_rv100_not_on_460gx(void)
{
    const char *argv[] = {
        qtest_qemu_binary(NULL),
        "-machine", "460gx,vga=rv100",
        "-m", "256M",
        "-display", "none",
        "-S",
        NULL,
    };
    g_autofree char *stderr_text = NULL;
    g_autoptr(GError) error = NULL;
    int wait_status;

    g_assert_true(g_spawn_sync(NULL, (char **)argv, NULL,
                               G_SPAWN_STDOUT_TO_DEV_NULL,
                               NULL, NULL, NULL, &stderr_text,
                               &wait_status, &error));
    g_assert_no_error(error);
    g_assert_true(WIFEXITED(wait_status));
    g_assert_cmpint(WEXITSTATUS(wait_status), ==, 1);
    g_assert_nonnull(strstr(stderr_text,
                            "vga=rv100 is the rx2600's adapter (zx1 only)"));
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/ati-rv100/config-image", test_rv100_config_image);
    qtest_add_func("/ati-rv100/register-aperture",
                   test_rv100_register_aperture);
    qtest_add_func("/ati-rv100/int10-rom-tables", test_rv100_int10_rom_tables);
    qtest_add_func("/ati-rv100/rom-bar-tables", test_rv100_rom_bar_tables);
    qtest_add_func("/ati-rv100/not-on-460gx", test_rv100_not_on_460gx);

    return g_test_run();
}
