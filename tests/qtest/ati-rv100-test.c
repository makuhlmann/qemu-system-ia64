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
#include "qobject/qdict.h"

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
    /* AGP_ENABLE does not stick: the card is on PCI (XFree86 4.3's probe). */
    rv100_cfg_writel(qts, 0x60, 0x00000300);
    g_assert_cmphex(rv100_cfg_readl(qts, 0x60) & 0x100, ==, 0);
    rv100_cfg_writel(qts, 0x60, 0x00000200);

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
 * table through +50h with one VGA connector (CRT, DDC_VGA, primary DAC), the
 * empty init tables and the memory table; no other table pointer is set.
 * limit: the bytes a driver maps.
 */
static void check_radeon_tables(const uint8_t *rom, uint32_t limit)
{
    uint32_t declared = rom[2] * 512U;
    uint32_t hdr, pll, conn, init, mem, i;
    uint8_t sum = 0;

    g_assert_cmphex(lduw_le_p(rom), ==, 0xaa55);
    g_assert_cmpmem(rom + 0x30, 10, " 761295520", 10);
    hdr = lduw_le_p(rom + 0x48);
    g_assert_cmphex(hdr, >=, 0x4a);
    g_assert_cmphex(rom[hdr + 0x00], ==, 2);
    g_assert_cmphex(rom[hdr + 0x06], ==, 0x80);
    pll = lduw_le_p(rom + hdr + 0x30);
    conn = lduw_le_p(rom + hdr + 0x50);
    for (i = 0x0e; i < 0x80; i += 2) {
        if (i != 0x30 && i != 0x46 && i != 0x48 && i != 0x4e && i != 0x50) {
            g_assert_cmphex(lduw_le_p(rom + hdr + i), ==, 0);
        }
    }
    /* ASIC_INIT_1, PLL_INIT and ASIC_INIT_2: one empty table, 0 before it. */
    init = lduw_le_p(rom + hdr + 0x0c);
    g_assert_cmphex(init, !=, 0);
    g_assert_cmphex(lduw_le_p(rom + hdr + 0x46), ==, init);
    g_assert_cmphex(lduw_le_p(rom + hdr + 0x4e), ==, init);
    g_assert_cmphex(rom[init - 1], ==, 0);
    g_assert_cmphex(lduw_le_p(rom + init), ==, 0);
    g_assert_cmphex(init + 2, <=, limit);
    /* MEM_CONFIG: step 2 MB, revision 0, 8 x 2 MB at MEM_CNTL[15:8] 03h. */
    mem = lduw_le_p(rom + hdr + 0x48);
    g_assert_cmphex(mem + 6, <=, limit);
    g_assert_cmphex(rom[mem - 2], ==, 2);
    g_assert_cmphex(rom[mem - 1], ==, 0);
    g_assert_cmphex(rom[mem], ==, 8);
    g_assert_cmphex(rom[mem + 1], ==, 0x03);
    g_assert_cmphex(rom[mem + 2], ==, 0);
    g_assert_cmphex(rom[mem + 5], ==, 0xff);
    g_assert_cmphex(hdr + 0x80, <=, limit);
    g_assert_cmphex(pll + 0x32, <=, limit);
    g_assert_cmphex(conn + 6, <=, limit);

    g_assert_cmpuint(lduw_le_p(rom + pll + 0x08), ==, 16600);
    g_assert_cmpuint(lduw_le_p(rom + pll + 0x0a), ==, 16600);
    g_assert_cmpuint(lduw_le_p(rom + pll + 0x0e), ==, 2700);
    g_assert_cmpuint(lduw_le_p(rom + pll + 0x10), ==, 60);
    g_assert_cmpuint(ldl_le_p(rom + pll + 0x12), ==, 12000);
    g_assert_cmpuint(ldl_le_p(rom + pll + 0x16), ==, 35000);
    /* The engine and memory PLLs: the POST state's reference divider. */
    g_assert_cmpuint(lduw_le_p(rom + pll + 0x1c), ==, 27);
    g_assert_cmpuint(lduw_le_p(rom + pll + 0x28), ==, 27);

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

/* An I/O port at the sparse address the IA-64 HAL builds. */
static uint64_t rv100_io(uint16_t port)
{
    return IA64_PCI_IO_BASE + (((uint64_t)(port >> 2) << 12) | (port & 0xfff));
}

/* The firmware's INT 10h interface at 1E0h: AX BX CX DX DI ES, then EXEC. */
static uint16_t rv100_int10(QTestState *qts, uint16_t ax, uint16_t bx)
{
    qtest_writew(qts, rv100_io(0x1e0), ax);
    qtest_writew(qts, rv100_io(0x1e2), bx);
    qtest_writew(qts, rv100_io(0x1ec), 0x4941);
    return qtest_readw(qts, rv100_io(0x1e0));
}

static uint32_t rv100_pll_read(QTestState *qts, uint64_t mmio, uint8_t index)
{
    qtest_writel(qts, mmio + 0x08, index);
    return qtest_readl(qts, mmio + 0x0c);
}

/*
 * A VBE mode set programs the RV100's own CRTC and pixel PLL, as the card's
 * BIOS does: ati2mtag takes a card with H_TOTAL 0 or FFh for one that was
 * never POSTed.  1024x768 is the DMT timing (1344 x 806, 65.00 MHz): with the
 * card's 27.00 MHz / 60 the PLL runs at 130 MHz, feedback 289, post /2.
 */
static void test_rv100_vbe_mode_crtc(void)
{
    QTestState *qts = qtest_init(RV100_ARGS);
    uint64_t mmio = rv100_mmio(qts);
    uint32_t htotal;

    g_assert_cmphex(rv100_int10(qts, 0x4f02, 0x4144), ==, 0x004f);
    htotal = qtest_readl(qts, mmio + 0x200);
    g_assert_cmphex(htotal, ==, (127u << 16) | 167);
    g_assert_cmphex(htotal & 0xff, !=, 0);
    g_assert_cmphex(htotal & 0xff, !=, 0xff);
    g_assert_cmphex(qtest_readl(qts, mmio + 0x208), ==, (767u << 16) | 805);
    g_assert_cmphex(qtest_readl(qts, mmio + 0x050), ==,
                    (1u << 25) | (1u << 24) | (6u << 8));
    g_assert_cmphex(qtest_readl(qts, mmio + 0x22c) & 0x7ff, ==, 128);
    g_assert_cmphex(rv100_pll_read(qts, mmio, 0x03) & 0x3ff, ==, 60);
    g_assert_cmphex(rv100_pll_read(qts, mmio, 0x07) & 0x707ff, ==,
                    (1u << 16) | 289);

    /* Text mode 3: the CRTC stays on and leaves the extended display. */
    rv100_int10(qts, 0x0003, 0);
    g_assert_cmphex(qtest_readl(qts, mmio + 0x050) & (3u << 24), ==, 1u << 25);
    qtest_quit(qts);
}

/* The Radeon's PLL file runs to 3Fh: P2PLL_DIV_0 (2Ch), SCLK_MORE_CNTL (35h). */
static void test_rv100_pll_file(void)
{
    QTestState *qts = qtest_init(RV100_ARGS);
    uint64_t mmio = rv100_mmio(qts);

    qtest_writel(qts, mmio + 0x08, 0x2c | 0x80);
    qtest_writel(qts, mmio + 0x0c, 0x00030121);
    qtest_writel(qts, mmio + 0x08, 0x35 | 0x80);
    qtest_writel(qts, mmio + 0x0c, 0x00000501);
    g_assert_cmphex(rv100_pll_read(qts, mmio, 0x2c), ==, 0x00030121);
    g_assert_cmphex(rv100_pll_read(qts, mmio, 0x2b), ==, 0);
    g_assert_cmphex(rv100_pll_read(qts, mmio, 0x35), ==, 0x00000501);
    qtest_quit(qts);
}

/*
 * D9: out of reset the PLL file is the RV100's POST state.  The pixel PLL
 * has the BIOS divider 60 and drives the display; the engine and memory
 * clocks come out at the BIOS table's 166 MHz whether a driver computes
 * 2 * fb * ref / M / post (Linux radeon_clocks.c) or fb * ref / M
 * (radeonfb's probe); no PLL is in reset or asleep, and the crystal is in use.
 */
static void test_rv100_pll_post_state(void)
{
    QTestState *qts = qtest_init(RV100_ARGS);
    uint64_t mmio = rv100_mmio(qts);
    uint32_t spll = rv100_pll_read(qts, mmio, 0x0a);
    uint32_t m = spll & 0xff;
    uint32_t sclk_post = rv100_pll_read(qts, mmio, 0x0d) & 7;
    uint32_t mclk_post = rv100_pll_read(qts, mmio, 0x12) & 7;

    g_assert_cmphex(rv100_pll_read(qts, mmio, 0x03) & 0x3ff, ==, 60);
    g_assert_cmphex(rv100_pll_read(qts, mmio, 0x08) & 3, ==, 3);
    g_assert_cmphex(spll, ==, (166u << 16) | (166u << 8) | 27);
    g_assert_cmpuint(sclk_post, ==, 2);
    g_assert_cmpuint(mclk_post, ==, 2);
    g_assert_cmpuint(2 * ((spll >> 16) & 0xff) * 2700 / m / 2, ==, 16600);
    g_assert_cmpuint(2 * ((spll >> 8) & 0xff) * 2700 / m / 2, ==, 16600);
    g_assert_cmpuint(((spll >> 16) & 0xff) * 2700 / m, ==, 16600);
    g_assert_cmphex(rv100_pll_read(qts, mmio, 0x02) & 3, ==, 0);
    g_assert_cmphex(rv100_pll_read(qts, mmio, 0x0c) & 3, ==, 0);
    g_assert_cmphex(rv100_pll_read(qts, mmio, 0x0e) & 3, ==, 0);
    g_assert_cmphex(rv100_pll_read(qts, mmio, 0x01) & (1u << 4), ==, 0);
    qtest_quit(qts);
}

/* The RGB of pixel (x, y) in a 1024x768 screendump. */
static uint32_t rv100_ppm_pixel(const char *filename, unsigned x, unsigned y)
{
    g_autofree char *contents = NULL;
    g_autoptr(GError) error = NULL;
    const uint8_t *px;
    char *end;
    gsize length;

    g_assert_true(g_file_get_contents(filename, &contents, &length, &error));
    g_assert_no_error(error);
    g_assert_true(g_str_has_prefix(contents, "P6\n"));
    g_assert_cmpuint(g_ascii_strtoull(contents + 3, &end, 10), ==, 1024);
    g_assert_cmpuint(g_ascii_strtoull(end, &end, 10), ==, 768);
    g_assert_cmpuint(g_ascii_strtoull(end, &end, 10), ==, 255);
    end++;
    g_assert_cmpuint(length - (end - contents), >=, 1024 * 768 * 3);
    px = (const uint8_t *)end + ((gsize)y * 1024 + x) * 3;
    return (px[0] << 16) | (px[1] << 8) | px[2];
}

/*
 * The Radeon's ARGB cursor (CRTC_CUR_MODE 2, 256 bytes a row, premultiplied
 * alpha): XFree86 4.3 loads every cursor so, and read as the Rage 128's
 * AND/XOR image it showed as a black block.  Drawn by the device
 * (guest_hwcursor) over grey 40h: opaque red and blue stay, green at alpha
 * 80h blends, alpha 0 leaves the background.
 */
static void test_rv100_argb_cursor(void)
{
    QTestState *qts = qtest_init(RV100_ARGS
                                 " -global ati-vga.guest_hwcursor=true");
    uint64_t mmio = rv100_mmio(qts);
    uint64_t fb = rv100_cfg_readl(qts, PCI_BASE_ADDRESS_0) &
                  PCI_BASE_ADDRESS_MEM_MASK;
    g_autofree char *tmpdir = NULL;
    g_autofree char *ppm = NULL;
    g_autoptr(GError) error = NULL;
    uint32_t gen;
    unsigned x, y;

    g_assert_cmphex(rv100_int10(qts, 0x4f02, 0x4144), ==, 0x004f);
    for (y = 48; y < 54; y++) {
        for (x = 96; x < 112; x++) {
            qtest_writel(qts, fb + (y * 1024 + x) * 4, 0x00404040);
        }
    }
    qtest_writel(qts, fb + 0x400000, 0xffff0000);
    qtest_writel(qts, fb + 0x400004, 0x80008000);
    qtest_writel(qts, fb + 0x400008, 0);
    qtest_writel(qts, fb + 0x400100, 0xff0000ff);
    qtest_writel(qts, mmio + 0x260, 0x400000);              /* CUR_OFFSET */
    qtest_writel(qts, mmio + 0x268, 0);                     /* ..._OFF */
    qtest_writel(qts, mmio + 0x264, (100u << 16) | 50);     /* ..._POSN */
    gen = qtest_readl(qts, mmio + 0x050);
    qtest_writel(qts, mmio + 0x050, (gen & ~(7u << 20)) | (2u << 20) |
                 (1u << 16));

    tmpdir = g_dir_make_tmp("ati-rv100-cursor-XXXXXX", &error);
    g_assert_no_error(error);
    ppm = g_build_filename(tmpdir, "cursor.ppm", NULL);
    qtest_qmp_assert_success(qts, "{'execute':'screendump','arguments':"
                             " {'filename':%s}}", ppm);
    g_assert_cmphex(rv100_ppm_pixel(ppm, 100, 50), ==, 0xff0000);
    g_assert_cmphex(rv100_ppm_pixel(ppm, 101, 50), ==, 0x20a020);
    g_assert_cmphex(rv100_ppm_pixel(ppm, 102, 50), ==, 0x404040);
    g_assert_cmphex(rv100_ppm_pixel(ppm, 100, 51), ==, 0x0000ff);
    g_assert_cmphex(rv100_ppm_pixel(ppm, 99, 50), ==, 0x404040);
    unlink(ppm);
    rmdir(tmpdir);
    qtest_quit(qts);
}

/*
 * The Radeon registers that drivers program and read back (RV100 plan, M3):
 * stored, CONFIG_MEMSIZE writable within its size field, the destination
 * caches never busy, GPIO_CRT2_DDC an idle line with pull-ups.
 */
static void test_rv100_stored_registers(void)
{
    static const uint16_t stored[] = {
        0x007c, 0x0094, 0x0188, 0x018c, 0x01c0, 0x023c, 0x027c, 0x02f0,
        0x033c, 0x03f0, 0x0d04, 0x0d08, 0x0d60, 0x1720, 0x1c3c, 0x3250,
        0x3258, 0x3428, 0x0b04, 0x0b08, 0x0b0c, 0x0b74, 0x0b78, 0x0b7c,
    };
    QTestState *qts = qtest_init(RV100_ARGS);
    uint64_t mmio = rv100_mmio(qts);
    size_t i;

    for (i = 0; i < G_N_ELEMENTS(stored); i++) {
        qtest_writel(qts, mmio + stored[i], 0x5a000000 | stored[i]);
    }
    for (i = 0; i < G_N_ELEMENTS(stored); i++) {
        g_assert_cmphex(qtest_readl(qts, mmio + stored[i]), ==,
                        0x5a000000 | stored[i]);
    }
    qtest_writel(qts, mmio + 0x0b10, 0xffffffff);
    g_assert_cmphex(qtest_readl(qts, mmio + 0x0b10), ==, 0);

    g_assert_cmphex(qtest_readl(qts, mmio + 0x00f8), ==, 16 * 1024 * 1024);
    qtest_writel(qts, mmio + 0x00f8, 0x02000fff);
    g_assert_cmphex(qtest_readl(qts, mmio + 0x00f8), ==, 0x02000000);

    qtest_writel(qts, mmio + 0x342c, 0x0000000f);
    g_assert_cmphex(qtest_readl(qts, mmio + 0x342c), ==, 0);
    qtest_writel(qts, mmio + 0x325c, 0x0000000f);
    g_assert_cmphex(qtest_readl(qts, mmio + 0x325c), ==, 0);

    g_assert_cmphex(qtest_readl(qts, mmio + 0x006c) & 0x300, ==, 0x300);
    qtest_writel(qts, mmio + 0x006c, 1u << 17);     /* drive SCL low */
    g_assert_cmphex(qtest_readl(qts, mmio + 0x006c) & 0x300, ==, 0x100);
    qtest_writel(qts, mmio + 0x006c, 0);
    g_assert_cmphex(qtest_readl(qts, mmio + 0x006c) & 0x300, ==, 0x300);
    qtest_quit(qts);
}

/* The VGA registers in the register aperture are the I/O ports' registers. */
static void test_rv100_vga_mmio(void)
{
    QTestState *qts = qtest_init(RV100_ARGS);
    uint64_t mmio = rv100_mmio(qts);

    qtest_writeb(qts, mmio + 0x3c4, 0x02);          /* SEQ index: map mask */
    qtest_writeb(qts, mmio + 0x3c5, 0x05);
    qtest_writeb(qts, rv100_io(0x3c4), 0x02);
    g_assert_cmphex(qtest_readb(qts, rv100_io(0x3c5)), ==, 0x05);
    qtest_writeb(qts, rv100_io(0x3c5), 0x0a);
    g_assert_cmphex(qtest_readb(qts, mmio + 0x3c5), ==, 0x0a);
    g_assert_cmphex(qtest_readw(qts, mmio + 0x3c4), ==, 0x0a02);
    g_assert_cmphex(qtest_readb(qts, mmio + 0x3cc),
                    ==, qtest_readb(qts, rv100_io(0x3cc)));
    qtest_quit(qts);
}

/*
 * GEN_INT_STATUS: the engine is always idle (GUI_IDLE_STAT, bit 19), and
 * SW_INT_FIRE raises SW_INT_TEST until it is acknowledged.
 */
static void test_rv100_sw_int(void)
{
    QTestState *qts = qtest_init(RV100_ARGS);
    uint64_t mmio = rv100_mmio(qts);

    g_assert_cmphex(qtest_readl(qts, mmio + 0x044) & (1u << 19), !=, 0);
    g_assert_cmphex(qtest_readl(qts, mmio + 0x044) & (1u << 25), ==, 0);
    qtest_writel(qts, mmio + 0x040, 1u << 25);      /* SW_INT_ENABLE */
    qtest_writel(qts, mmio + 0x044, 1u << 26);      /* SW_INT_FIRE */
    g_assert_cmphex(qtest_readl(qts, mmio + 0x044) & (3u << 25), ==, 1u << 25);
    qtest_writel(qts, mmio + 0x044, 1u << 25);      /* acknowledge */
    g_assert_cmphex(qtest_readl(qts, mmio + 0x044) & (3u << 25), ==, 0);
    qtest_writel(qts, mmio + 0x044, 1u << 19);
    g_assert_cmphex(qtest_readl(qts, mmio + 0x044) & (1u << 19), !=, 0);
    qtest_quit(qts);
}

/* A 1x4 solid fill at the card address dst, 32 bpp, pitch 256 bytes. */
static void rv100_fill(QTestState *qts, uint64_t mmio, uint32_t dst,
                       uint32_t colour)
{
    qtest_writel(qts, mmio + 0x16e8, 0x1fff1fff);   /* default scissor */
    qtest_writel(qts, mmio + 0x16cc, 0xffffffff);   /* DP_WRITE_MASK */
    qtest_writel(qts, mmio + 0x146c,                /* DP_GUI_MASTER_CNTL */
                 0x00000002 | 0x000000d0 | (6u << 8) | (3u << 12) |
                 0x00f00000 | (2u << 24) | 0x10000000);
    qtest_writel(qts, mmio + 0x142c, (4u << 22) | (dst >> 10));
    qtest_writel(qts, mmio + 0x147c, colour);       /* DP_BRUSH_FRGD_CLR */
    qtest_writel(qts, mmio + 0x16c0, 3);            /* DP_CNTL: +x, +y */
    qtest_writel(qts, mmio + 0x1438, 0);            /* DST_Y_X */
    qtest_writel(qts, mmio + 0x143c, (1u << 16) | 4); /* DST_HEIGHT_WIDTH */
}

/*
 * The 2D engine addresses the card's memory map: MC_FB_LOCATION puts VRAM
 * at card address 0 after POST (XFree86 4.3's offsets count from there),
 * and ati2mtag moves it to F000_0000h; a fill lands at its offset in VRAM
 * either way, and an address outside VRAM writes nothing.
 */
static void test_rv100_mc_fb_location(void)
{
    QTestState *qts = qtest_init(RV100_ARGS);
    uint64_t mmio = rv100_mmio(qts);
    uint64_t fb = rv100_cfg_readl(qts, PCI_BASE_ADDRESS_0) &
                  PCI_BASE_ADDRESS_MEM_MASK;
    unsigned i;

    g_assert_cmphex(qtest_readl(qts, mmio + 0x148), ==, 0x00ff0000);
    rv100_fill(qts, mmio, 0x100000, 0x00112233);
    for (i = 0; i < 4; i++) {
        g_assert_cmphex(qtest_readl(qts, fb + 0x100000 + i * 4), ==,
                        0x00112233);
    }

    qtest_writel(qts, mmio + 0x148, 0xf0fff000);    /* VRAM at F000_0000h */
    rv100_fill(qts, mmio, 0xf0200000, 0x00445566);
    g_assert_cmphex(qtest_readl(qts, fb + 0x200000), ==, 0x00445566);
    g_assert_cmphex(qtest_readl(qts, fb + 0x20000c), ==, 0x00445566);
    rv100_fill(qts, mmio, 0x00100000, 0x00778899);
    g_assert_cmphex(qtest_readl(qts, fb + 0x100000), ==, 0x00112233);
    qtest_quit(qts);
}

/*
 * The engine's coordinates are 14-bit signed: a fill that starts 5 pixels
 * left of its surface (x 3FFBh) draws its 3 pixels inside the scissor and
 * nothing before the row, as XP 3790's glyphs at the edge of a surface do.
 */
static void test_rv100_negative_x(void)
{
    QTestState *qts = qtest_init(RV100_ARGS);
    uint64_t mmio = rv100_mmio(qts);
    uint64_t fb = rv100_cfg_readl(qts, PCI_BASE_ADDRESS_0) &
                  PCI_BASE_ADDRESS_MEM_MASK;
    unsigned i;

    rv100_fill(qts, mmio, 0x100000, 0);
    qtest_writel(qts, mmio + 0x1438, (1u << 16) | 0x3ffb);  /* DST_Y_X */
    qtest_writel(qts, mmio + 0x143c, (1u << 16) | 8);       /* DST_H_W */
    for (i = 0; i < 3; i++) {
        g_assert_cmphex(qtest_readl(qts, fb + 0x100100 + i * 4), ==,
                        0x00000000);
    }
    qtest_writel(qts, mmio + 0x147c, 0x00556677);
    qtest_writel(qts, mmio + 0x1438, (1u << 16) | 0x3ffb);
    qtest_writel(qts, mmio + 0x143c, (1u << 16) | 8);
    for (i = 0; i < 3; i++) {
        g_assert_cmphex(qtest_readl(qts, fb + 0x100100 + i * 4), ==,
                        0x00556677);
    }
    g_assert_cmphex(qtest_readl(qts, fb + 0x10010c), !=, 0x00556677);
    for (i = 1; i <= 5; i++) {
        g_assert_cmphex(qtest_readl(qts, fb + 0x100100 - i * 4), !=,
                        0x00556677);
    }
    qtest_quit(qts);
}

/*
 * The Radeon's scissor corner is exclusive (xf86-video-ati adds 1 to the
 * last pixel): a fill clipped to SC_BOTTOM_RIGHT (3, 1) covers columns 0-2
 * of row 0 only.
 */
static void test_rv100_scissor(void)
{
    QTestState *qts = qtest_init(RV100_ARGS);
    uint64_t mmio = rv100_mmio(qts);
    uint64_t fb = rv100_cfg_readl(qts, PCI_BASE_ADDRESS_0) &
                  PCI_BASE_ADDRESS_MEM_MASK;
    unsigned i;

    rv100_fill(qts, mmio, 0x100000, 0);
    qtest_writel(qts, mmio + 0x146c,                /* + GMC_DST_CLIPPING */
                 0x00000002 | 0x00000008 | 0x000000d0 | (6u << 8) |
                 (3u << 12) | 0x00f00000 | (2u << 24) | 0x10000000);
    qtest_writel(qts, mmio + 0x16ec, 0);            /* SC_TOP_LEFT */
    qtest_writel(qts, mmio + 0x16f0, (1u << 16) | 3);   /* SC_BOTTOM_RIGHT */
    qtest_writel(qts, mmio + 0x147c, 0x00aabbcc);
    qtest_writel(qts, mmio + 0x1438, 0);
    qtest_writel(qts, mmio + 0x143c, (2u << 16) | 4);
    for (i = 0; i < 3; i++) {
        g_assert_cmphex(qtest_readl(qts, fb + 0x100000 + i * 4), ==,
                        0x00aabbcc);
    }
    g_assert_cmphex(qtest_readl(qts, fb + 0x10000c), ==, 0);
    g_assert_cmphex(qtest_readl(qts, fb + 0x100100), ==, 0);
    qtest_quit(qts);
}

/*
 * ROP3 B8h (PSDPxax), which XP 3790's ati2dvag uses for masked drawing:
 * where the source is white the destination stays, where it is black the
 * pattern (a solid brush here) is written; generally each result bit is bit
 * (P << 2 | S << 1 | D) of the code.
 */
static void test_rv100_rop3(void)
{
    QTestState *qts = qtest_init(RV100_ARGS);
    uint64_t mmio = rv100_mmio(qts);
    uint64_t fb = rv100_cfg_readl(qts, PCI_BASE_ADDRESS_0) &
                  PCI_BASE_ADDRESS_MEM_MASK;

    qtest_writel(qts, fb + 0x200000, 0x00ffffff);   /* source */
    qtest_writel(qts, fb + 0x200004, 0x00000000);
    qtest_writel(qts, fb + 0x200008, 0x00f0f0f0);
    qtest_writel(qts, fb + 0x100000, 0x00123456);   /* destination */
    qtest_writel(qts, fb + 0x100004, 0x00123456);
    qtest_writel(qts, fb + 0x100008, 0x00123456);
    qtest_writel(qts, mmio + 0x16e8, 0x1fff1fff);   /* default scissor */
    qtest_writel(qts, mmio + 0x16cc, 0xffffffff);   /* DP_WRITE_MASK */
    qtest_writel(qts, mmio + 0x16c0, 3);            /* DP_CNTL */
    qtest_writel(qts, mmio + 0x146c,                /* DP_GUI_MASTER_CNTL */
                 0x00000003 | 0x000000d0 | (6u << 8) | (3u << 12) |
                 0x00b80000 | (2u << 24) | 0x10000000);
    qtest_writel(qts, mmio + 0x1428, (4u << 22) | (0x200000 >> 10));
    qtest_writel(qts, mmio + 0x142c, (4u << 22) | (0x100000 >> 10));
    qtest_writel(qts, mmio + 0x147c, 0x00ff00ff);   /* brush foreground */
    qtest_writel(qts, mmio + 0x1434, 0);            /* SRC_Y_X */
    qtest_writel(qts, mmio + 0x1438, 0);            /* DST_Y_X */
    qtest_writel(qts, mmio + 0x143c, (1u << 16) | 3);
    g_assert_cmphex(qtest_readl(qts, fb + 0x100000), ==, 0x00123456);
    g_assert_cmphex(qtest_readl(qts, fb + 0x100004), ==, 0x00ff00ff);
    g_assert_cmphex(qtest_readl(qts, fb + 0x100008), ==,
                    (0x00f0f0f0 & 0x00123456) | (0xff0f0f0f & 0x00ff00ff));
    qtest_quit(qts);
}

/*
 * A CP set-up as the DRMs and ati2mtag make it: a PCI GART of 4 pages at card
 * address E000_0000h (page table at 100000h in RAM, pages at 200000h), the
 * ring in its page 0 (RB_BUFSZ 8: 512 dwords), the read pointer written back
 * to page 1 and the scratch registers behind it.
 */
#define RV100_PT        0x00100000u
#define RV100_PAGES     0x00200000u
#define RV100_GART      0xe0000000u

static void rv100_cp_setup(QTestState *qts, uint64_t mmio, uint32_t csq_mode)
{
    unsigned i;

    /* Memory decode and bus master, as pci.sys and Linux leave the card. */
    rv100_cfg_writel(qts, PCI_COMMAND,
                     (rv100_cfg_readl(qts, PCI_COMMAND) & 0xffff) |
                     PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
    for (i = 0; i < 4; i++) {
        qtest_writel(qts, RV100_PT + i * 4, RV100_PAGES + i * 0x1000);
    }
    qtest_memset(qts, RV100_PAGES, 0, 4 * 0x1000);
    qtest_writel(qts, mmio + 0x1d8, RV100_PT);              /* AIC_PT_BASE */
    qtest_writel(qts, mmio + 0x1dc, RV100_GART);            /* AIC_LO_ADDR */
    qtest_writel(qts, mmio + 0x1e0, RV100_GART + 0x3fff);   /* AIC_HI_ADDR */
    qtest_writel(qts, mmio + 0x1d0, 1);                     /* AIC_CNTL */
    qtest_writel(qts, mmio + 0x700, RV100_GART);            /* CP_RB_BASE */
    qtest_writel(qts, mmio + 0x704, 8);                     /* CP_RB_CNTL */
    qtest_writel(qts, mmio + 0x70c, RV100_GART + 0x1000);   /* ..._RPTR_ADDR */
    qtest_writel(qts, mmio + 0x774, RV100_GART + 0x1020);   /* SCRATCH_ADDR */
    qtest_writel(qts, mmio + 0x770, 0x3);                   /* SCRATCH_UMSK */
    qtest_writel(qts, mmio + 0x740, csq_mode << 28);        /* CP_CSQ_CNTL */
    qtest_writel(qts, mmio + 0x16e8, 0x1fff1fff);   /* default scissor */
    qtest_writel(qts, mmio + 0x16cc, 0xffffffff);   /* DP_WRITE_MASK */
    qtest_writel(qts, mmio + 0x16c0, 3);            /* DP_CNTL: +x, +y */
}

/*
 * The DRMs load the CP microcode from address 0 in DATAH/DATAL pairs: the
 * address advances after each low word (RAGE 128 SDK manual 5.2.2).
 */
static void test_rv100_cp_microcode_address(void)
{
    QTestState *qts = qtest_init(RV100_ARGS);
    uint64_t mmio = rv100_mmio(qts);
    unsigned i;

    qtest_writel(qts, mmio + 0x7d4, 0);             /* CP_ME_RAM_ADDR */
    for (i = 0; i < 3; i++) {
        qtest_writel(qts, mmio + 0x7dc, 0x01234567);    /* DATAH */
        g_assert_cmpuint(qtest_readl(qts, mmio + 0x7d4), ==, i);
        qtest_writel(qts, mmio + 0x7e0, 0x89abcdef);    /* DATAL */
    }
    g_assert_cmpuint(qtest_readl(qts, mmio + 0x7d4), ==, 3);
    qtest_quit(qts);
}

/* CNTL_PAINT_MULTI: a 4x1 solid fill at VRAM 100000h, as XP's ring has it. */
static unsigned rv100_paint_packet(uint32_t *p, uint32_t colour)
{
    p[0] = 0xc0049a00;
    p[1] = 0x00000002 | 0x000000d0 | (6u << 8) | (3u << 12) | 0x00f00000 |
           (2u << 24) | 0x10000000;
    p[2] = (4u << 22) | (0x100000 >> 10);
    p[3] = colour;
    p[4] = 0;
    p[5] = (4u << 16) | 1;
    return 6;
}

static void rv100_put_dwords(QTestState *qts, uint64_t addr,
                             const uint32_t *p, unsigned n)
{
    unsigned i;

    for (i = 0; i < n; i++) {
        qtest_writel(qts, addr + i * 4, p[i]);
    }
}

/*
 * D11: the CP fetches its ring through the PCI GART, runs the 2D packet and
 * a type-0 write to SCRATCH_REG1, writes the read pointer back to
 * CP_RB_RPTR_ADDR and the scratch register to SCRATCH_ADDR + 4.
 */
static void test_rv100_cp_ring_gart(void)
{
    QTestState *qts = qtest_init(RV100_ARGS);
    uint64_t mmio = rv100_mmio(qts);
    uint64_t fb = rv100_cfg_readl(qts, PCI_BASE_ADDRESS_0) &
                  PCI_BASE_ADDRESS_MEM_MASK;
    uint32_t ring[8];
    unsigned n, i;

    rv100_cp_setup(qts, mmio, 4);
    n = rv100_paint_packet(ring, 0x00abcdef);
    ring[n++] = 0x00000579;                 /* type 0: SCRATCH_REG1 (15E4h) */
    ring[n++] = 0x12345678;
    rv100_put_dwords(qts, RV100_PAGES, ring, n);
    qtest_writel(qts, mmio + 0x714, n);     /* CP_RB_WPTR */

    for (i = 0; i < 4; i++) {
        g_assert_cmphex(qtest_readl(qts, fb + 0x100000 + i * 4), ==,
                        0x00abcdef);
    }
    g_assert_cmphex(qtest_readl(qts, fb + 0x100010), !=, 0x00abcdef);
    g_assert_cmpuint(qtest_readl(qts, mmio + 0x710), ==, n);
    g_assert_cmpuint(qtest_readl(qts, RV100_PAGES + 0x1000), ==, n);
    g_assert_cmphex(qtest_readl(qts, mmio + 0x15e4), ==, 0x12345678);
    g_assert_cmphex(qtest_readl(qts, RV100_PAGES + 0x1024), ==, 0x12345678);
    qtest_quit(qts);
}

/*
 * With CP_CSQ_CNTL 0 the CP is off: the ring is not fetched, nothing moves;
 * once on, it runs what is queued.
 */
static void test_rv100_cp_off(void)
{
    QTestState *qts = qtest_init(RV100_ARGS);
    uint64_t mmio = rv100_mmio(qts);
    uint64_t fb = rv100_cfg_readl(qts, PCI_BASE_ADDRESS_0) &
                  PCI_BASE_ADDRESS_MEM_MASK;
    uint32_t ring[8];
    unsigned n;

    rv100_cp_setup(qts, mmio, 0);
    n = rv100_paint_packet(ring, 0x00abcdef);
    rv100_put_dwords(qts, RV100_PAGES, ring, n);
    qtest_writel(qts, mmio + 0x714, n);
    g_assert_cmphex(qtest_readl(qts, fb + 0x100000), !=, 0x00abcdef);
    g_assert_cmpuint(qtest_readl(qts, mmio + 0x710), ==, 0);
    g_assert_cmpuint(qtest_readl(qts, RV100_PAGES + 0x1000), ==, 0);
    /* Turned on, the CP fetches what is queued. */
    qtest_writel(qts, mmio + 0x740, 8u << 28);
    g_assert_cmphex(qtest_readl(qts, fb + 0x100000), ==, 0x00abcdef);
    g_assert_cmpuint(qtest_readl(qts, mmio + 0x710), ==, n);
    qtest_quit(qts);
}

/*
 * An indirect buffer: the ring writes CP_IB_BASE and CP_IB_BUFSZ (738h,
 * 73Ch) in one type-0 packet, as the DRMs do; the buffer in GART page 2
 * holds the fill.  It runs in mode 4 (PRIBM_INDBM) and in mode 8, the Rage
 * 128's 64BM_64VCBM_64INDBM that ati2mtag uses, not in mode 2.
 */
static void rv100_cp_ib(QTestState *qts, uint64_t mmio, uint32_t csq_mode)
{
    uint32_t ib[8], ring[4];
    unsigned n;

    rv100_cp_setup(qts, mmio, csq_mode);
    n = rv100_paint_packet(ib, 0x00456789);
    rv100_put_dwords(qts, RV100_PAGES + 0x2000, ib, n);
    ring[0] = (1u << 16) | (0x738 >> 2);
    ring[1] = RV100_GART + 0x2000;
    ring[2] = n;
    rv100_put_dwords(qts, RV100_PAGES, ring, 3);
    qtest_writel(qts, mmio + 0x714, 3);
}

static void test_rv100_cp_indirect(void)
{
    QTestState *qts = qtest_init(RV100_ARGS);
    uint64_t mmio = rv100_mmio(qts);
    uint64_t fb = rv100_cfg_readl(qts, PCI_BASE_ADDRESS_0) &
                  PCI_BASE_ADDRESS_MEM_MASK;

    rv100_cp_ib(qts, mmio, 2);
    g_assert_cmphex(qtest_readl(qts, fb + 0x100000), !=, 0x00456789);
    g_assert_cmpuint(qtest_readl(qts, mmio + 0x710), ==, 3);
    qtest_quit(qts);

    qts = qtest_init(RV100_ARGS);
    mmio = rv100_mmio(qts);
    rv100_cp_ib(qts, mmio, 4);
    g_assert_cmphex(qtest_readl(qts, fb + 0x100000), ==, 0x00456789);
    g_assert_cmphex(qtest_readl(qts, fb + 0x10000c), ==, 0x00456789);
    qtest_quit(qts);

    qts = qtest_init(RV100_ARGS);
    mmio = rv100_mmio(qts);
    rv100_cp_ib(qts, mmio, 8);
    g_assert_cmphex(qtest_readl(qts, fb + 0x100000), ==, 0x00456789);
    g_assert_cmpuint(qtest_readl(qts, mmio + 0x710), ==, 3);
    qtest_quit(qts);
}

/*
 * A CNTL_PAINT_MULTI with an 8x8 mono pattern, as ati2mtag sends it: GMC bit
 * 31 (BRUSH_FLAG) clear, so four brush dwords and no BRUSH_Y_X (AMD R5xx
 * Acceleration v1.5, SETTINGS).  The 8x2 rectangle gets both colours; read
 * as five brush dwords it would land elsewhere.
 */
static void test_rv100_cp_mono_brush(void)
{
    QTestState *qts = qtest_init(RV100_ARGS);
    uint64_t mmio = rv100_mmio(qts);
    uint64_t fb = rv100_cfg_readl(qts, PCI_BASE_ADDRESS_0) &
                  PCI_BASE_ADDRESS_MEM_MASK;
    uint32_t ring[16];
    unsigned n = 0, x, fg = 0, bg = 0;

    rv100_cp_setup(qts, mmio, 8);
    ring[n++] = 0xc0089a00;                         /* 9 dwords follow */
    ring[n++] = 0x00000002 | (0u << 4) | (6u << 8) | (3u << 12) |
                0x00f00000 | (2u << 24) | 0x10000000;
    ring[n++] = (4u << 22) | (0x100000 >> 10);
    ring[n++] = 0x00000011;                         /* background */
    ring[n++] = 0x00000022;                         /* foreground */
    ring[n++] = 0xaa55aa55;
    ring[n++] = 0xaa55aa55;
    ring[n++] = 0;                                  /* x 0, y 0 */
    ring[n++] = (8u << 16) | 2;                     /* 8 x 2 */
    ring[n++] = 0x80000000;                         /* type 2 filler */
    rv100_put_dwords(qts, RV100_PAGES, ring, n);
    qtest_writel(qts, mmio + 0x714, n);

    for (x = 0; x < 8; x++) {
        uint32_t px = qtest_readl(qts, fb + 0x100000 + x * 4);

        g_assert_true(px == 0x11 || px == 0x22);
        fg += px == 0x22;
        bg += px == 0x11;
        g_assert_cmphex(qtest_readl(qts, fb + 0x100100 + x * 4), !=, px);
    }
    g_assert_cmpuint(fg, ==, 4);
    g_assert_cmpuint(bg, ==, 4);
    g_assert_cmphex(qtest_readl(qts, fb + 0x100020), ==, 0);
    g_assert_cmpuint(qtest_readl(qts, mmio + 0x710), ==, n);
    qtest_quit(qts);
}

static void rv100_wait_migration(QTestState *qts)
{
    for (;;) {
        QDict *rsp = qtest_qmp(qts, "{ 'execute': 'query-migrate' }");
        QDict *ret = qdict_get_qdict(rsp, "return");
        const char *status = qdict_haskey(ret, "status") ?
                             qdict_get_str(ret, "status") : "none";
        bool done = !strcmp(status, "completed");

        g_assert_cmpstr(status, !=, "failed");
        qobject_unref(rsp);
        if (done) {
            return;
        }
        g_usleep(1000);
    }
}

/*
 * The CP's state migrates: a ring that ran through the PCI GART before the
 * move continues after it, from the restored read pointer, through the
 * restored GART and ring registers; the stored Radeon registers come along.
 */
static void test_rv100_cp_migration(void)
{
    QTestState *src = qtest_init(RV100_ARGS);
    uint64_t mmio = rv100_mmio(src);
    uint64_t fb = rv100_cfg_readl(src, PCI_BASE_ADDRESS_0) &
                  PCI_BASE_ADDRESS_MEM_MASK;
    g_autofree char *tmpdir = NULL;
    g_autofree char *file = NULL;
    g_autofree char *uri = NULL;
    g_autoptr(GError) error = NULL;
    QTestState *dst;
    uint32_t ring[8];
    unsigned n;

    rv100_cp_setup(src, mmio, 8);
    n = rv100_paint_packet(ring, 0x00abcdef);
    rv100_put_dwords(src, RV100_PAGES, ring, n);
    qtest_writel(src, mmio + 0x714, n);
    qtest_writel(src, mmio + 0x027c, 0x5a00027c);   /* CRTC_MORE_CNTL */
    qtest_writel(src, mmio + 0x0b0c, 0x00000000);   /* SURFACE0_INFO */
    qtest_writel(src, mmio + 0x00f8, 0x02000000);   /* CONFIG_MEMSIZE */

    tmpdir = g_dir_make_tmp("ati-rv100-mig-XXXXXX", &error);
    g_assert_no_error(error);
    file = g_build_filename(tmpdir, "state", NULL);
    uri = g_strdup_printf("exec:cat > %s", file);
    qtest_qmp_assert_success(src, "{ 'execute': 'migrate',"
                             " 'arguments': { 'uri': %s } }", uri);
    rv100_wait_migration(src);
    qtest_quit(src);

    dst = qtest_initf(RV100_ARGS " -incoming 'exec:cat %s'", file);
    rv100_wait_migration(dst);
    g_assert_cmphex(qtest_readl(dst, fb + 0x100000), ==, 0x00abcdef);
    g_assert_cmpuint(qtest_readl(dst, mmio + 0x710), ==, n);
    g_assert_cmphex(qtest_readl(dst, mmio + 0x0740), ==, 8u << 28);
    g_assert_cmphex(qtest_readl(dst, mmio + 0x027c), ==, 0x5a00027c);
    g_assert_cmphex(qtest_readl(dst, mmio + 0x00f8), ==, 0x02000000);
    g_assert_cmphex(rv100_pll_read(dst, mmio, 0x0a), ==,
                    (166u << 16) | (166u << 8) | 27);

    /* The ring goes on where it stopped. */
    n += rv100_paint_packet(ring, 0x00135791);
    rv100_put_dwords(dst, RV100_PAGES + 6 * 4, ring, 6);
    qtest_writel(dst, mmio + 0x714, n);
    g_assert_cmphex(qtest_readl(dst, fb + 0x100000), ==, 0x00135791);
    g_assert_cmpuint(qtest_readl(dst, RV100_PAGES + 0x1000), ==, n);
    qtest_quit(dst);
    unlink(file);
    rmdir(tmpdir);
}

static uint32_t f32_bits(float value)
{
    uint32_t bits;

    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

/* SE_VTX_FMT, SE_VF_CNTL, then the vertex dwords through SE_PORT_DATA0. */
static void rv100_draw_port(QTestState *qts, uint64_t mmio, uint32_t format,
                            uint32_t vf_cntl, const uint32_t *data, unsigned n)
{
    unsigned i;

    qtest_writel(qts, mmio + 0x2080, format);
    qtest_writel(qts, mmio + 0x2084, vf_cntl);
    for (i = 0; i < n; i++) {
        qtest_writel(qts, mmio + 0x2000, data[i]);
    }
}

/*
 * SE_PORT_DATA0-31 hold one vertex batch of at most 4096 dwords: data with
 * no vertex count in SE_VF_CNTL, or with a count above that, is dropped,
 * and the card goes on drawing.
 */
static void test_rv100_3d_port_bounds(void)
{
    QTestState *qts = qtest_init(RV100_ARGS);
    uint64_t mmio = rv100_mmio(qts);
    uint64_t fb = rv100_cfg_readl(qts, PCI_BASE_ADDRESS_0) &
                  PCI_BASE_ADDRESS_MEM_MASK;
    static const uint32_t vf_cntl[] = {
        0,
        (0xffffu << 16) | (3u << 4) | 4,    /* 65535 vertices, triangles */
    };
    unsigned i, j;

    for (j = 0; j < ARRAY_SIZE(vf_cntl); j++) {
        qtest_writel(qts, mmio + 0x2080, 1u << 3);  /* SE_VTX_FMT: PKCOLOR */
        qtest_writel(qts, mmio + 0x2084, vf_cntl[j]);
        for (i = 0; i < 5000; i++) {
            qtest_writel(qts, mmio + 0x2000 + (i & 31) * 4, i);
        }
        g_assert_cmphex(qtest_readl(qts, mmio + 0x2080), ==, 1u << 3);
        g_assert_cmphex(qtest_readl(qts, mmio + 0x2084), ==, vf_cntl[j]);
    }
    rv100_fill(qts, mmio, 0x100000, 0x00112233);
    g_assert_cmphex(qtest_readl(qts, fb + 0x100000), ==, 0x00112233);
    qtest_quit(qts);
}

/*
 * x-r100-3d=on: a rectangle list through SE_PORT_DATA0 (colours in RGBA
 * order) and a triangle list in a 3D_DRAW_IMMD packet on the CP ring
 * (colours in ARGB order) land in an ARGB8888 colour buffer at card
 * address 0.  The rectangle's fourth corner is implied; its right and
 * bottom edges are exclusive.
 */
static void test_rv100_3d_draw(void)
{
    const uint32_t rect[] = {
        f32_bits(4.0f), f32_bits(24.0f), 0xff563412,
        f32_bits(24.0f), f32_bits(24.0f), 0xff563412,
        f32_bits(24.0f), f32_bits(4.0f), 0xff563412,
    };
    QTestState *qts = qtest_init(RV100_ARGS " -global ati-vga.x-r100-3d=on");
    uint64_t mmio = rv100_mmio(qts);
    uint64_t fb = rv100_cfg_readl(qts, PCI_BASE_ADDRESS_0) &
                  PCI_BASE_ADDRESS_MEM_MASK;
    uint32_t ring[16];
    unsigned n = 0, i;

#define PIXEL(x, y) (fb + ((y) * 64 + (x)) * 4)
    qtest_memset(qts, fb, 0, 64 * 64 * 4);
    qtest_writel(qts, mmio + 0x1c40, 0);                /* RB3D_COLOROFFSET */
    qtest_writel(qts, mmio + 0x1c48, 64);               /* RB3D_COLORPITCH */
    qtest_writel(qts, mmio + 0x26c0, 0);                /* RE_TOP_LEFT */
    qtest_writel(qts, mmio + 0x1c44, (63u << 16) | 63); /* RE_WIDTH_HEIGHT */
    qtest_writel(qts, mmio + 0x1c3c, 6u << 10);         /* RB3D_CNTL ARGB8888 */

    rv100_draw_port(qts, mmio, 1u << 3,
                    (3u << 16) | (1u << 6) | (3u << 4) | 8, rect, 9);
    g_assert_cmphex(qtest_readl(qts, PIXEL(4, 4)), ==, 0xff123456);
    g_assert_cmphex(qtest_readl(qts, PIXEL(23, 23)), ==, 0xff123456);
    g_assert_cmphex(qtest_readl(qts, PIXEL(24, 10)), ==, 0);
    g_assert_cmphex(qtest_readl(qts, PIXEL(10, 24)), ==, 0);
    g_assert_cmphex(qtest_readl(qts, PIXEL(3, 3)), ==, 0);

    rv100_cp_setup(qts, mmio, 8);
    ring[n++] = 0xc0000000 | (10u << 16) | (0x29 << 8); /* 3D_DRAW_IMMD */
    ring[n++] = 1u << 3;                                /* PKCOLOR */
    ring[n++] = (3u << 16) | (3u << 4) | 4;             /* triangle list */
    ring[n++] = f32_bits(32.0f);
    ring[n++] = f32_bits(24.0f);
    ring[n++] = 0xff654321;
    ring[n++] = f32_bits(52.0f);
    ring[n++] = f32_bits(4.0f);
    ring[n++] = 0xff654321;
    ring[n++] = f32_bits(52.0f);
    ring[n++] = f32_bits(24.0f);
    ring[n++] = 0xff654321;
    rv100_put_dwords(qts, RV100_PAGES, ring, n);
    qtest_writel(qts, mmio + 0x714, n);                 /* CP_RB_WPTR */
    g_assert_cmpuint(qtest_readl(qts, mmio + 0x710), ==, n);
    for (i = 48; i < 52; i++) {
        g_assert_cmphex(qtest_readl(qts, PIXEL(i, 22)), ==, 0xff654321);
    }
    g_assert_cmphex(qtest_readl(qts, PIXEL(34, 6)), ==, 0);
    g_assert_cmphex(qtest_readl(qts, PIXEL(10, 10)), ==, 0xff123456);
#undef PIXEL
    qtest_quit(qts);
}

/*
 * Face culling as Mesa's R100 driver uses it (radeonFrontFace,
 * radeonCullFace): with Y down, (4,4) (4,20) (20,4) is counter-clockwise for
 * FFACE_CULL_DIR. SE_CNTL bit 0 picks the front winding, bits 2:1 and 4:3
 * cull (0) or draw (3) back and front faces.
 */
static void test_rv100_3d_cull(void)
{
    static const struct {
        uint32_t se_cntl;
        bool drawn;
    } cases[] = {
        { 1 | (0u << 1) | (3u << 3), true },    /* CCW front, cull back */
        { 0 | (0u << 1) | (3u << 3), false },   /* CW front, cull back */
        { 1 | (3u << 1) | (0u << 3), false },   /* CCW front, cull front */
        { 0 | (3u << 1) | (0u << 3), true },    /* CW front, cull front */
    };
    const uint32_t tri[] = {
        f32_bits(4.0f), f32_bits(4.0f), 0xff00ff00,
        f32_bits(4.0f), f32_bits(20.0f), 0xff00ff00,
        f32_bits(20.0f), f32_bits(4.0f), 0xff00ff00,
    };
    QTestState *qts = qtest_init(RV100_ARGS " -global ati-vga.x-r100-3d=on");
    uint64_t mmio = rv100_mmio(qts);
    uint64_t fb = rv100_cfg_readl(qts, PCI_BASE_ADDRESS_0) &
                  PCI_BASE_ADDRESS_MEM_MASK;
    size_t i;

    qtest_writel(qts, mmio + 0x1c40, 0);                /* RB3D_COLOROFFSET */
    qtest_writel(qts, mmio + 0x1c48, 64);               /* RB3D_COLORPITCH */
    qtest_writel(qts, mmio + 0x26c0, 0);                /* RE_TOP_LEFT */
    qtest_writel(qts, mmio + 0x1c44, (63u << 16) | 63); /* RE_WIDTH_HEIGHT */
    qtest_writel(qts, mmio + 0x1c3c, 6u << 10);         /* RB3D_CNTL ARGB8888 */
    for (i = 0; i < ARRAY_SIZE(cases); i++) {
        qtest_writel(qts, fb + (6 * 64 + 6) * 4, 0);
        qtest_writel(qts, mmio + 0x1c4c,                /* SE_CNTL */
                     cases[i].se_cntl | (2u << 8) | (2u << 10));
        rv100_draw_port(qts, mmio, 1u << 3,
                        (3u << 16) | (3u << 4) | 4, tri, 9);
        g_assert_cmphex(qtest_readl(qts, fb + (6 * 64 + 6) * 4), ==,
                        cases[i].drawn ? 0xff00ff00 : 0);
    }
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
    qtest_add_func("/ati-rv100/vbe-mode-crtc", test_rv100_vbe_mode_crtc);
    qtest_add_func("/ati-rv100/pll-file", test_rv100_pll_file);
    qtest_add_func("/ati-rv100/pll-post-state", test_rv100_pll_post_state);
    qtest_add_func("/ati-rv100/argb-cursor", test_rv100_argb_cursor);
    qtest_add_func("/ati-rv100/stored-registers",
                   test_rv100_stored_registers);
    qtest_add_func("/ati-rv100/vga-mmio", test_rv100_vga_mmio);
    qtest_add_func("/ati-rv100/sw-int", test_rv100_sw_int);
    qtest_add_func("/ati-rv100/mc-fb-location", test_rv100_mc_fb_location);
    qtest_add_func("/ati-rv100/negative-x", test_rv100_negative_x);
    qtest_add_func("/ati-rv100/rop3", test_rv100_rop3);
    qtest_add_func("/ati-rv100/scissor", test_rv100_scissor);
    qtest_add_func("/ati-rv100/cp-ring-gart", test_rv100_cp_ring_gart);
    qtest_add_func("/ati-rv100/cp-off", test_rv100_cp_off);
    qtest_add_func("/ati-rv100/cp-microcode-address",
                   test_rv100_cp_microcode_address);
    qtest_add_func("/ati-rv100/cp-indirect", test_rv100_cp_indirect);
    qtest_add_func("/ati-rv100/cp-mono-brush", test_rv100_cp_mono_brush);
    qtest_add_func("/ati-rv100/cp-migration", test_rv100_cp_migration);
    qtest_add_func("/ati-rv100/3d-port-bounds", test_rv100_3d_port_bounds);
    qtest_add_func("/ati-rv100/3d-draw", test_rv100_3d_draw);
    qtest_add_func("/ati-rv100/3d-cull", test_rv100_3d_cull);
    qtest_add_func("/ati-rv100/not-on-460gx", test_rv100_not_on_460gx);

    return g_test_run();
}
