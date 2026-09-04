/*
 * IA-64 virtual platform machine tests
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include <glib/gstdio.h>
#include "qemu/bitops.h"
#include "qemu/bswap.h"
#include "qemu/cutils.h"
#include "qemu/sockets.h"
#include "qemu/timer.h"
#include "qemu/units.h"
#include "qobject/qdict.h"
#include "qobject/qlist.h"
#include "libqtest.h"
#include "libqos/generic-pcihost.h"
#include "libqos/pci.h"
#include "hw/display/bochs-vbe.h"
#include "hw/display/vga_regs.h"
#include "hw/pci/pci_ids.h"
#include "hw/pci/pci_regs.h"
#include "hw/ia64/ia64_vpc_abi.h"
#include "hw/net/e1000_regs.h"

/* Platform addresses come from hw/ia64/ia64_vpc_abi.h; test-only register
 * offsets stay local. */
#define IA64_LEGACY_IO_BASE          IA64_PCI_IO_BASE
#define IA64_ACPI_PM1_EVT_EN_OFFSET  0x02ULL
#define IA64_ACPI_PM1_CNT_OFFSET     0x04ULL
#define IA64_IOSAPIC_IOREGSEL        0x00ULL
#define IA64_IOSAPIC_IOWIN           0x10ULL
#define IA64_IOSAPIC_EOI             0x40ULL
#define IA64_IOSAPIC_RTE_BASE        0x10U
#define IA64_IOSAPIC_RTE_LOWEST      BIT(8)
#define IA64_IOSAPIC_RTE_DELIVERY    BIT(12)
#define IA64_IOSAPIC_RTE_REMOTE_IRR  BIT(14)
#define IA64_IOSAPIC_RTE_LEVEL       BIT(15)
#define IA64_TEST_RAM_SIZE           (256 * MiB)
#define IA64_INT10_ROM_BASE          0x000c0000ULL
/*
 * 2 KB: the XP inbox Rage 128 miniport rejects option ROMs whose size byte
 * declares less than 2048 bytes.  Keep in sync with hw/ia64/ia64_vpc.c.
 */
#define IA64_INT10_ROM_SIZE          0x00000800U
#define IA64_INT10_VECTOR_ADDR       0x00000040ULL
#define IA64_INT10_ROM_PCIR_OFFSET   0x00e0U
#define IA64_INT10_ROM_ATI_SIG_OFFSET 0x0030U
#define IA64_INT10_ROM_ATI_HEADER_OFFSET 0x0080U
#define IA64_INT10_ROM_ATI_PLL_OFFSET 0x00c0U
#define IA64_INT10_ROM_HANDLER_OFFSET 0x0100U
#define IA64_INT10_HANDLER_SIZE      128U
#define IA64_INT10_ROM_OEM_OFFSET    0x0180U
#define IA64_INT10_ROM_MODES_OFFSET  0x01d0U
#define IA64_INT10_IO_BASE           0x01e0U
#define IA64_INT10_TRIGGER           0x4941U
#define IA64_VBE2_SIGNATURE          0x32454256U
#define IA64_VBE_IO_INDEX            0x01ceU
#define IA64_VBE_IO_DATA             0x01d0U
#define IA64_VGA_FB_BASE             0x00000000f0000000ULL
#define IA64_VGA_MMIO_BASE           0x00000000f5000000ULL
#define IA64_VGA_LEGACY_BASE         0x00000000000a0000ULL
#define IA64_ATI_BIOS_0_SCRATCH      0x0010U
#define IA64_BDA_VIDEO_MODE          0x00000449ULL
#define IA64_BDA_VIDEO_COLUMNS       0x0000044aULL
#define IA64_BDA_VIDEO_PAGE_SIZE     0x0000044cULL
#define IA64_BDA_VIDEO_ROWS          0x00000484ULL
#define IA64_BDA_CHARACTER_HEIGHT    0x00000485ULL
#define IA64_BDA_VIDEO_CONTROL       0x00000487ULL

enum TestInt10Register {
    TEST_INT10_AX,
    TEST_INT10_BX,
    TEST_INT10_CX,
    TEST_INT10_DX,
    TEST_INT10_DI,
    TEST_INT10_ES,
    TEST_INT10_EXEC,
    TEST_INT10_DATA,
};

typedef struct TestInt10Registers {
    uint16_t ax;
    uint16_t bx;
    uint16_t cx;
    uint16_t dx;
    uint16_t di;
    uint16_t es;
    uint32_t input_signature;
} TestInt10Registers;

/* The SCSI HBA's register BAR, inside the WXB0 root's 32 MB aperture. */
#define IA64_LSI_MMIO_BASE           0x00000000fa000000ULL
#define IA64_LSI_SCRIPT_ADDR         0x00100000U
#define IA64_LSI_MSGOUT_ADDR         0x00110000U
#define IA64_LSI_CDB_ADDR            0x00110010U
#define IA64_LSI_STATUS_ADDR         0x00110020U
#define IA64_LSI_COMPLETE_ADDR       0x00110030U
#define IA64_LSI_REG_DSTAT           0x0c
#define IA64_LSI_REG_ISTAT0          0x14
#define IA64_LSI_REG_DSP             0x2c
#define IA64_LSI_REG_SIST0           0x42
#define IA64_LSI_REG_SIST1           0x43
#define IA64_LSI_ISTAT0_DIP          0x01
#define IA64_LSI_ISTAT0_INTF         0x04
#define IA64_LSI_DSTAT_SIR           0x04
#define IA64_LSI_PHASE_CMD           2
#define IA64_LSI_PHASE_ST            3
#define IA64_LSI_PHASE_MO            6
#define IA64_LSI_PHASE_MI            7
#define IA64_LSI_SCRIPT_SELECT       0x40000008U
#define IA64_LSI_SCRIPT_DISCONNECT   0x48000000U
#define IA64_LSI_SCRIPT_INTERRUPT    0x98080000U
#define IA64_LSI_SCRIPT_MOVE(phase, count) \
    (((phase) << 24) | (count))

#define IA64_E1000_MMIO_BASE         0x00000000ee040000ULL
#define IA64_E1000_IO_BASE           0x0000c400U
/*
 * The i2000 carries its Ethernet at 00:05.0, the slot the graphics adapter
 * vacated when it moved to the GXB root.  INTA on slot 5 swizzles to
 * IA64_PCI_INTX_GSI_BASE + (5 + 0) % 4 = 17.
 */
#define IA64_E1000_SLOT              5U
#define IA64_E1000_GSI               17U
#define IA64_E1000_TX_DESC_ADDR      0x00120000U
#define IA64_E1000_TX_BUFFER_ADDR    0x00121000U
#define IA64_E1000_RX_DESC_ADDR      0x00122000U
#define IA64_E1000_RX_BUFFER_ADDR    0x00123000U
#define IA64_E1000_RING_SIZE         128U
#define IA64_E1000_TEST_TIMEOUT_MS   5000

typedef struct ExpectedPCIDevice {
    unsigned slot;
    uint16_t vendor;
    uint16_t device;
    uint16_t command;
    uint8_t irq_line;
    uint8_t irq_pin;
    uint32_t bars[6];
} ExpectedPCIDevice;

static const ExpectedPCIDevice expected_e1000 = {
    .slot = IA64_E1000_SLOT,
    .vendor = PCI_VENDOR_ID_INTEL,
    .device = E1000_DEV_ID_82540EM,
    .command = PCI_COMMAND_IO | PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER,
    .irq_line = IA64_E1000_GSI,
    .irq_pin = 1,
    .bars = {
        [0] = IA64_E1000_MMIO_BASE,
        [1] = IA64_E1000_IO_BASE | PCI_BASE_ADDRESS_SPACE_IO,
    },
};

/*
 * The default adapter is the 100 Mbit PRO/100 (i82557b, DEV_1229), the
 * device Windows IA-64 actually ships an inbox driver for (NET557.IN_).
 * Unlike the e1000 it exposes three BARs: a 4 KiB prefetchable CSR memory
 * BAR, a 32-byte I/O BAR, and the 1 MiB Flash aperture this controller
 * generation decodes.  The machine's generic per-BAR NIC allocator hands
 * each one a naturally aligned slice of the per-index memory / I/O window,
 * so the Flash BAR lands at the next 1 MiB boundary above the CSR BAR.
 */
static const ExpectedPCIDevice expected_i82557b = {
    .slot = IA64_E1000_SLOT,
    .vendor = PCI_VENDOR_ID_INTEL,
    .device = 0x1229,
    .command = PCI_COMMAND_IO | PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER,
    .irq_line = IA64_E1000_GSI,
    .irq_pin = 1,
    .bars = {
        [0] = IA64_E1000_MMIO_BASE | PCI_BASE_ADDRESS_MEM_PREFETCH,
        [1] = IA64_E1000_IO_BASE | PCI_BASE_ADDRESS_SPACE_IO,
        [2] = (IA64_E1000_MMIO_BASE + 0x100000) & ~0xfffffULL,
    },
};

static uint32_t iosapic_read(QTestState *qts, uint32_t reg);
static void iosapic_write(QTestState *qts, uint32_t reg, uint32_t value);

static QTestState *ia64_vpc_start(const char *extra_args)
{
    return qtest_initf("-machine 460gx -m 256M -S %s",
                       extra_args ?: "");
}

/*
 * The LSI is opt-in now that the QLogic holds the platform's SCSI seat.
 * Turning the QLogic off puts the LSI back on that seat, so these tests see
 * the bus name and the register addresses the adapter has always had there.
 */
static QTestState *ia64_vpc_start_lsi(const char *extra_args)
{
    return qtest_initf("-machine 460gx,lsi=on,isp=off -m 256M -S %s",
                       extra_args ?: "");
}

static uint64_t ia64_sparse_io_offset(uint32_t port)
{
    return ((uint64_t)(port >> 2) << 12) | (port & 0xfff);
}

static void int10_outw(QTestState *qts, uint16_t port, uint16_t value)
{
    qtest_writew(qts, IA64_LEGACY_IO_BASE +
                 ia64_sparse_io_offset(port), value);
}

static uint16_t int10_inw(QTestState *qts, uint16_t port)
{
    return qtest_readw(qts, IA64_LEGACY_IO_BASE +
                       ia64_sparse_io_offset(port));
}

static size_t int10_call(QTestState *qts, TestInt10Registers *regs,
                         uint8_t *response, size_t response_size)
{
    static const size_t register_offsets[] = {
        [TEST_INT10_AX] = offsetof(TestInt10Registers, ax),
        [TEST_INT10_BX] = offsetof(TestInt10Registers, bx),
        [TEST_INT10_CX] = offsetof(TestInt10Registers, cx),
        [TEST_INT10_DX] = offsetof(TestInt10Registers, dx),
        [TEST_INT10_DI] = offsetof(TestInt10Registers, di),
        [TEST_INT10_ES] = offsetof(TestInt10Registers, es),
    };
    size_t word_count;
    size_t i;

    for (i = TEST_INT10_AX; i <= TEST_INT10_ES; i++) {
        uint16_t value;

        memcpy(&value, (uint8_t *)regs + register_offsets[i],
               sizeof(value));
        int10_outw(qts, IA64_INT10_IO_BASE + i * 2, value);
    }
    if (regs->input_signature != 0) {
        int10_outw(qts, IA64_INT10_IO_BASE + TEST_INT10_DATA * 2,
                   (uint16_t)regs->input_signature);
        int10_outw(qts, IA64_INT10_IO_BASE + TEST_INT10_DATA * 2,
                   (uint16_t)(regs->input_signature >> 16));
    }
    int10_outw(qts, IA64_INT10_IO_BASE + TEST_INT10_EXEC * 2,
               IA64_INT10_TRIGGER);
    word_count = int10_inw(qts,
                           IA64_INT10_IO_BASE + TEST_INT10_EXEC * 2);
    g_assert_cmpuint(word_count * 2, <=, response_size);
    for (i = 0; i < word_count; i++) {
        stw_le_p(response + i * 2, int10_inw(
            qts, IA64_INT10_IO_BASE + TEST_INT10_DATA * 2));
    }
    for (i = TEST_INT10_AX; i <= TEST_INT10_ES; i++) {
        uint16_t value = int10_inw(qts,
                                   IA64_INT10_IO_BASE + i * 2);

        memcpy((uint8_t *)regs + register_offsets[i], &value,
               sizeof(value));
    }
    return word_count * 2;
}

static uint32_t int10_far_to_linear(uint32_t pointer)
{
    return (pointer >> 16) * 16 + (pointer & 0xffff);
}

static uint16_t test_vbe_read(QTestState *qts, uint16_t index)
{
    qtest_writew(qts, IA64_LEGACY_IO_BASE + IA64_VBE_IO_INDEX, index);
    return qtest_readw(qts, IA64_LEGACY_IO_BASE + IA64_VBE_IO_DATA);
}

static uint8_t test_vga_indexed_read(QTestState *qts, uint16_t index_port,
                                     uint16_t data_port, uint8_t index)
{
    qtest_writeb(qts, IA64_LEGACY_IO_BASE + index_port, index);
    return qtest_readb(qts, IA64_LEGACY_IO_BASE + data_port);
}

static void test_assert_ppm_pixel(const char *filename, unsigned width,
                                  unsigned height, unsigned x, unsigned y,
                                  uint8_t red, uint8_t green, uint8_t blue)
{
    g_autofree char *contents = NULL;
    g_autoptr(GError) error = NULL;
    const uint8_t *pixel;
    char *end;
    unsigned actual_width;
    unsigned actual_height;
    unsigned maximum;
    gsize length;

    g_assert_true(g_file_get_contents(filename, &contents, &length, &error));
    g_assert_no_error(error);
    g_assert_true(g_str_has_prefix(contents, "P6\n"));
    actual_width = g_ascii_strtoull(contents + 3, &end, 10);
    actual_height = g_ascii_strtoull(end, &end, 10);
    maximum = g_ascii_strtoull(end, &end, 10);
    g_assert_cmpuint(actual_width, ==, width);
    g_assert_cmpuint(actual_height, ==, height);
    g_assert_cmpuint(maximum, ==, 255);
    g_assert_cmpuint(x, <, width);
    g_assert_cmpuint(y, <, height);
    g_assert_cmpuint(length - (end - contents), >,
                     (gsize)width * height * 3);
    g_assert_true(g_ascii_isspace(*end));
    if (*end++ == '\r' && *end == '\n') {
        end++;
    }
    pixel = (const uint8_t *)end + ((gsize)y * width + x) * 3;
    g_assert_cmphex(pixel[0], ==, red);
    g_assert_cmphex(pixel[1], ==, green);
    g_assert_cmphex(pixel[2], ==, blue);
}

static void test_int10_rom(void)
{
    uint8_t rom[IA64_INT10_ROM_SIZE];
    uint8_t zero[IA64_INT10_ROM_SIZE] = { 0 };
    uint8_t vector[4];
    uint32_t vector_linear;
    uint16_t ati_header;
    uint16_t ati_pll;
    unsigned checksum = 0;
    QTestState *qts = ia64_vpc_start(NULL);
    size_t i;

    qtest_memread(qts, IA64_INT10_ROM_BASE, rom, sizeof(rom));
    g_assert_cmphex(rom[0], ==, 0x55);
    g_assert_cmphex(rom[1], ==, 0xaa);
    g_assert_cmphex(rom[2], ==, IA64_INT10_ROM_SIZE / 512);
    g_assert_cmphex(lduw_le_p(rom + 0x0d), ==,
                    IA64_INT10_ROM_HANDLER_OFFSET);
    g_assert_cmphex(lduw_le_p(rom + 0x13), ==,
                    IA64_INT10_ROM_BASE >> 4);
    g_assert_cmphex(lduw_le_p(rom + 0x18), ==,
                    IA64_INT10_ROM_PCIR_OFFSET);
    g_assert_cmpmem(rom + IA64_INT10_ROM_PCIR_OFFSET, 4, "PCIR", 4);
    g_assert_cmphex(lduw_le_p(rom + IA64_INT10_ROM_PCIR_OFFSET + 4),
                    ==, 0x1002);
    g_assert_cmphex(lduw_le_p(rom + IA64_INT10_ROM_PCIR_OFFSET + 6),
                    ==, 0x5046);
    g_assert_cmpmem(rom + 0x60, 19, "QEMU IA64 VBE INT10", 19);
    /*
     * ATI's drivers validate the ROM by its signature at 30h before following
     * the pointer chain at 48h; PCIR must therefore stay clear of both.
     */
    g_assert_cmpmem(rom + IA64_INT10_ROM_ATI_SIG_OFFSET, 10,
                    " 761295520", 10);
    g_assert_cmpint(IA64_INT10_ROM_ATI_SIG_OFFSET + 10, <=, 0x48);
    g_assert_cmpint(IA64_INT10_ROM_PCIR_OFFSET, >=, 0x4a);
    ati_header = lduw_le_p(rom + 0x48);
    g_assert_cmphex(ati_header, ==, IA64_INT10_ROM_ATI_HEADER_OFFSET);
    ati_pll = lduw_le_p(rom + ati_header + 0x30);
    g_assert_cmphex(ati_pll, ==, IA64_INT10_ROM_ATI_PLL_OFFSET);
    /*
     * PLL values as published by real Rage 128 Pro BIOSes (identical in the
     * XPERT128 retail, Connect3D AGP and generic PCI dumps): XCLK 120.00 MHz,
     * 29.50 MHz reference with divider 65, 125-400 MHz VCO range.
     */
    g_assert_cmpuint(lduw_le_p(rom + ati_pll + 0x08), ==, 12000);
    g_assert_cmpuint(lduw_le_p(rom + ati_pll + 0x0e), ==, 2950);
    g_assert_cmpuint(lduw_le_p(rom + ati_pll + 0x10), ==, 65);
    g_assert_cmpuint(ldl_le_p(rom + ati_pll + 0x12), ==, 12500);
    g_assert_cmpuint(ldl_le_p(rom + ati_pll + 0x16), ==, 40000);
    g_assert_cmpmem(rom + IA64_INT10_ROM_OEM_OFFSET, 13,
                    "QEMU IA64 VBE", 13);
    g_assert_cmphex(lduw_le_p(rom + IA64_INT10_ROM_MODES_OFFSET),
                    ==, 0x111);
    g_assert_cmphex(rom[IA64_INT10_ROM_HANDLER_OFFSET], ==, 0x55);
    g_assert_cmphex(rom[IA64_INT10_ROM_HANDLER_OFFSET + 1], ==, 0x89);
    /* The handler ends in iret (0xcf) and abuts the OEM string at its size. */
    g_assert_cmphex(rom[IA64_INT10_ROM_HANDLER_OFFSET +
                       IA64_INT10_HANDLER_SIZE - 1], ==, 0xcf);
    for (i = 0; i < sizeof(rom); i++) {
        checksum += rom[i];
    }
    g_assert_cmphex(checksum & 0xff, ==, 0);

    qtest_memread(qts, IA64_INT10_VECTOR_ADDR, vector, sizeof(vector));
    g_assert_cmphex(lduw_le_p(vector), ==, IA64_INT10_ROM_HANDLER_OFFSET);
    g_assert_cmphex(lduw_le_p(vector + 2), ==,
                    IA64_INT10_ROM_BASE >> 4);
    vector_linear = lduw_le_p(vector + 2) * 16 + lduw_le_p(vector);
    g_assert_cmphex(vector_linear, ==,
                    IA64_INT10_ROM_BASE + IA64_INT10_ROM_HANDLER_OFFSET);

    qtest_memwrite(qts, IA64_INT10_ROM_BASE, zero, sizeof(zero));
    qtest_memwrite(qts, IA64_INT10_VECTOR_ADDR, zero, sizeof(vector));
    qtest_system_reset(qts);
    qtest_memread(qts, IA64_INT10_ROM_BASE, rom, sizeof(rom));
    qtest_memread(qts, IA64_INT10_VECTOR_ADDR, vector, sizeof(vector));
    g_assert_cmphex(rom[0], ==, 0x55);
    g_assert_cmphex(rom[1], ==, 0xaa);
    g_assert_cmphex(lduw_le_p(vector), ==, IA64_INT10_ROM_HANDLER_OFFSET);
    g_assert_cmphex(lduw_le_p(vector + 2), ==,
                    IA64_INT10_ROM_BASE >> 4);
    qtest_quit(qts);
}

static void test_int10_vbe_for_device(const char *extra_args)
{
    uint8_t response[512];
    TestInt10Registers regs = {
        .ax = 0x4f00,
        .di = 0x0100,
        .es = 0x2000,
    };
    uint32_t modes_linear;
    unsigned checksum = 0;
    size_t length;
    size_t i;
    QTestState *qts = ia64_vpc_start(extra_args);

    length = int10_call(qts, &regs,
                        response, sizeof(response));
    g_assert_cmpuint(length, ==, 256);
    g_assert_cmphex(regs.ax, ==, 0x004f);
    g_assert_cmpmem(response, 4, "VESA", 4);

    regs = (TestInt10Registers) {
        .ax = 0x4f00,
        .di = 0x0100,
        .es = 0x2000,
        .input_signature = IA64_VBE2_SIGNATURE,
    };
    length = int10_call(qts, &regs,
                        response, sizeof(response));
    g_assert_cmpuint(length, ==, 512);
    g_assert_cmphex(regs.ax, ==, 0x004f);
    g_assert_cmpmem(response, 4, "VESA", 4);
    g_assert_cmphex(lduw_le_p(response + 4), ==, 0x0300);
    g_assert_cmphex(lduw_le_p(response + 18), ==, 256);
    modes_linear = int10_far_to_linear(ldl_le_p(response + 14));
    g_assert_cmphex(modes_linear,
                    ==, IA64_INT10_ROM_BASE + IA64_INT10_ROM_MODES_OFFSET);
    g_assert_cmphex(qtest_readw(qts, modes_linear), ==, 0x111);
    g_assert_cmphex(int10_far_to_linear(ldl_le_p(response + 6)),
                    ==, IA64_INT10_ROM_BASE + IA64_INT10_ROM_OEM_OFFSET);

    memset(&regs, 0, sizeof(regs));
    regs.ax = 0x4f01;
    regs.cx = 0x144;
    length = int10_call(qts, &regs,
                        response, sizeof(response));
    g_assert_cmpuint(length, ==, 256);
    g_assert_cmphex(regs.ax, ==, 0x004f);
    g_assert_cmphex(lduw_le_p(response) & 0x80, !=, 0);
    g_assert_cmphex(lduw_le_p(response + 16), ==, 4096);
    g_assert_cmphex(lduw_le_p(response + 18), ==, 1024);
    g_assert_cmphex(lduw_le_p(response + 20), ==, 768);
    g_assert_cmphex(response[25], ==, 32);
    g_assert_cmphex((uint32_t)ldl_le_p(response + 40), ==, 0xf0000000U);

    memset(&regs, 0, sizeof(regs));
    regs.ax = 0x4f02;
    regs.bx = 0x4143;
    length = int10_call(qts, &regs,
                        response, sizeof(response));
    g_assert_cmpuint(length, ==, 0);
    g_assert_cmphex(regs.ax, ==, 0x004f);
    g_assert_cmphex(test_vbe_read(qts, VBE_DISPI_INDEX_XRES), ==, 800);
    g_assert_cmphex(test_vbe_read(qts, VBE_DISPI_INDEX_YRES), ==, 600);
    g_assert_cmphex(test_vbe_read(qts, VBE_DISPI_INDEX_BPP), ==, 32);
    g_assert_cmphex(test_vbe_read(qts, VBE_DISPI_INDEX_ENABLE) & 0x41,
                    ==, 0x41);

    /*
     * A VBE mode-set must also re-enable video output at the VGA attribute
     * controller (Palette-Address-Source, bit 0x20).  Without it QEMU's VGA
     * core forces GMODE_BLANK and the guest desktop, though rendered into
     * VRAM, never reaches the screen.  Reset the attribute flip-flop to the
     * index state (read Input Status 1) and read the index register back.
     */
    qtest_readb(qts, IA64_LEGACY_IO_BASE + VGA_IS1_RC);
    g_assert_cmphex(qtest_readb(qts, IA64_LEGACY_IO_BASE + VGA_ATT_W) &
                    VGA_AR_ENABLE_DISPLAY, ==, VGA_AR_ENABLE_DISPLAY);

    memset(&regs, 0, sizeof(regs));
    regs.ax = 0x4f03;
    length = int10_call(qts, &regs,
                        response, sizeof(response));
    g_assert_cmpuint(length, ==, 0);
    g_assert_cmphex(regs.ax, ==, 0x004f);
    g_assert_cmphex(regs.bx, ==, 0x4143);

    memset(&regs, 0, sizeof(regs));
    regs.ax = 0x4f06;
    regs.bx = 1;
    length = int10_call(qts, &regs,
                        response, sizeof(response));
    g_assert_cmpuint(length, ==, 0);
    g_assert_cmphex(regs.ax, ==, 0x004f);
    g_assert_cmphex(regs.bx, ==, 3200);
    g_assert_cmphex(regs.cx, ==, 800);
    g_assert_cmphex(regs.dx, >, 600);

    memset(&regs, 0, sizeof(regs));
    regs.ax = 0x4f15;
    regs.bx = 1;
    length = int10_call(qts, &regs,
                        response, sizeof(response));
    g_assert_cmpuint(length, ==, 128);
    g_assert_cmphex(regs.ax, ==, 0x004f);
    g_assert_cmphex(response[0], ==, 0x00);
    g_assert_cmphex(response[1], ==, 0xff);
    for (i = 0; i < length; i++) {
        checksum += response[i];
    }
    g_assert_cmphex(checksum & 0xff, ==, 0);
    qtest_quit(qts);
}

static void test_int10_vbe(void)
{
    test_int10_vbe_for_device(NULL);
}

static void test_int10_vbe_std(void)
{
    test_int10_vbe_for_device("-vga std");
}

static void test_int10_legacy_for_device(const char *extra_args)
{
    uint8_t response[2];
    uint8_t marker[16];
    uint8_t actual[sizeof(marker)];
    uint8_t zero[sizeof(marker)] = { 0 };
    TestInt10Registers regs;
    QTestState *qts = ia64_vpc_start(extra_args);
    g_autofree char *tmpdir = NULL;
    g_autofree char *ppm = NULL;
    g_autoptr(GError) error = NULL;
    size_t length;

    memset(&regs, 0, sizeof(regs));
    regs.ax = 0x4f02;
    regs.bx = 0x4143;
    length = int10_call(qts, &regs, response, sizeof(response));
    g_assert_cmpuint(length, ==, 0);
    g_assert_cmphex(regs.ax, ==, 0x004f);

    memset(marker, 0xa5, sizeof(marker));
    qtest_memwrite(qts, IA64_VGA_FB_BASE, marker, sizeof(marker));
    memset(&regs, 0, sizeof(regs));
    regs.ax = 0x0012;
    length = int10_call(qts, &regs, response, sizeof(response));
    g_assert_cmpuint(length, ==, 0);
    g_assert_cmphex(test_vbe_read(qts, VBE_DISPI_INDEX_ENABLE), ==, 0);
    g_assert_cmphex(test_vga_indexed_read(qts, VGA_SEQ_I, VGA_SEQ_D,
                                          VGA_SEQ_MEMORY_MODE), ==, 0x06);
    g_assert_cmphex(test_vga_indexed_read(qts, VGA_CRT_IC, VGA_CRT_DC,
                                          VGA_CRTC_H_DISP), ==, 0x4f);
    g_assert_cmphex(test_vga_indexed_read(qts, VGA_CRT_IC, VGA_CRT_DC,
                                          VGA_CRTC_V_DISP_END), ==, 0xdf);
    g_assert_cmphex(test_vga_indexed_read(qts, VGA_GFX_I, VGA_GFX_D,
                                          VGA_GFX_MISC), ==, 0x05);
    qtest_memread(qts, IA64_VGA_FB_BASE, actual, sizeof(actual));
    g_assert_cmpmem(actual, sizeof(actual), zero, sizeof(zero));
    g_assert_cmphex(qtest_readb(qts, IA64_BDA_VIDEO_MODE), ==, 0x12);
    g_assert_cmphex(qtest_readw(qts, IA64_BDA_VIDEO_COLUMNS), ==, 80);
    g_assert_cmphex(qtest_readw(qts, IA64_BDA_VIDEO_PAGE_SIZE), ==,
                    0xa000);
    g_assert_cmphex(qtest_readb(qts, IA64_BDA_VIDEO_ROWS), ==, 29);
    g_assert_cmphex(qtest_readw(qts, IA64_BDA_CHARACTER_HEIGHT), ==, 16);
    g_assert_cmphex(qtest_readb(qts, IA64_BDA_VIDEO_CONTROL), ==, 0x60);

    /* Match bootvid.dll's planar write path and verify actual scanout. */
    qtest_writeb(qts, IA64_VGA_LEGACY_BASE, 0xff);
    tmpdir = g_dir_make_tmp("ia64-int10-legacy-XXXXXX", &error);
    g_assert_no_error(error);
    g_assert_nonnull(tmpdir);
    ppm = g_build_filename(tmpdir, "mode12.ppm", NULL);
    qtest_qmp_assert_success(qts,
                             "{'execute':'screendump','arguments':"
                             " {'filename':%s}}", ppm);
    test_assert_ppm_pixel(ppm, 640, 480, 0, 0, 0xff, 0xff, 0xff);
    test_assert_ppm_pixel(ppm, 640, 480, 8, 0, 0, 0, 0);

    memset(&regs, 0, sizeof(regs));
    regs.ax = 0x0f00;
    regs.bx = 0xabcd;
    length = int10_call(qts, &regs, response, sizeof(response));
    g_assert_cmpuint(length, ==, 0);
    g_assert_cmphex(regs.ax, ==, 0x5012);
    g_assert_cmphex(regs.bx, ==, 0x00cd);

    memset(&regs, 0, sizeof(regs));
    regs.ax = 0x4f02;
    regs.bx = 0x4143;
    length = int10_call(qts, &regs, response, sizeof(response));
    g_assert_cmpuint(length, ==, 0);
    g_assert_cmphex(regs.ax, ==, 0x004f);
    memset(marker, 0x5a, sizeof(marker));
    qtest_memwrite(qts, IA64_VGA_FB_BASE, marker, sizeof(marker));

    memset(&regs, 0, sizeof(regs));
    regs.ax = 0x0092;
    length = int10_call(qts, &regs, response, sizeof(response));
    g_assert_cmpuint(length, ==, 0);
    qtest_memread(qts, IA64_VGA_FB_BASE, actual, sizeof(actual));
    g_assert_cmpmem(actual, sizeof(actual), marker, sizeof(marker));
    g_assert_cmphex(qtest_readb(qts, IA64_BDA_VIDEO_CONTROL), ==, 0xe0);

    memset(&regs, 0, sizeof(regs));
    regs.ax = 0x0f00;
    length = int10_call(qts, &regs, response, sizeof(response));
    g_assert_cmpuint(length, ==, 0);
    g_assert_cmphex(regs.ax, ==, 0x5012);

    memset(&regs, 0, sizeof(regs));
    regs.ax = 0x4f02;
    regs.bx = 0x4143;
    length = int10_call(qts, &regs, response, sizeof(response));
    g_assert_cmpuint(length, ==, 0);
    g_assert_cmphex(regs.ax, ==, 0x004f);
    g_assert_cmphex(test_vbe_read(qts, VBE_DISPI_INDEX_XRES), ==, 800);
    g_assert_cmphex(test_vbe_read(qts, VBE_DISPI_INDEX_YRES), ==, 600);
    g_assert_cmphex(test_vbe_read(qts, VBE_DISPI_INDEX_BPP), ==, 32);
    g_assert_cmphex(test_vbe_read(qts, VBE_DISPI_INDEX_ENABLE) & 0x41,
                    ==, 0x41);
    memset(&regs, 0, sizeof(regs));
    regs.ax = 0x0f00;
    length = int10_call(qts, &regs, response, sizeof(response));
    g_assert_cmpuint(length, ==, 0);
    g_assert_cmphex(regs.ax, ==, 0x5003);
    qtest_quit(qts);

    g_assert_cmpint(g_unlink(ppm), ==, 0);
    g_assert_cmpint(g_rmdir(tmpdir), ==, 0);
}

static void test_int10_legacy(void)
{
    test_int10_legacy_for_device(NULL);
}

static void test_int10_legacy_std(void)
{
    test_int10_legacy_for_device("-vga std");
}

static void test_acpi_reset_register(void)
{
    QTestState *qts = ia64_vpc_start(NULL);

    qtest_writeb(qts,
                 IA64_LEGACY_IO_BASE + IA64_ACPI_PM_IO_BASE +
                 IA64_ACPI_PM_RESET_OFFSET,
                 IA64_ACPI_PM_RESET_VALUE);
    qtest_qmp_eventwait(qts, "RESET");
    qtest_quit(qts);
}

static uint64_t read_handoff_i8042(QTestState *qts)
{
    IA64VpcHandoff handoff;

    qtest_memread(qts, IA64_FW_HANDOFF_ADDR, &handoff, sizeof(handoff));
    g_assert_cmphex(le64_to_cpu(handoff.Magic), ==, IA64_FW_HANDOFF_MAGIC);
    return le64_to_cpu(handoff.I8042Enabled);
}

static void assert_firmware_handoff(QTestState *qts, uint64_t i8042,
                                    uint64_t cpus, uint64_t nvram,
                                    uint64_t sockets, uint64_t cores,
                                    uint64_t threads)
{
    IA64VpcHandoff handoff;

    g_assert_cmpuint(sizeof(handoff), ==, 128);
    qtest_memread(qts, IA64_FW_HANDOFF_ADDR, &handoff, sizeof(handoff));
    g_assert_cmphex(le64_to_cpu(handoff.Magic), ==, IA64_FW_HANDOFF_MAGIC);
    g_assert_cmphex(le64_to_cpu(handoff.Version), ==,
                    IA64_FW_HANDOFF_VERSION);
    g_assert_cmphex(le64_to_cpu(handoff.RamSize), ==, IA64_TEST_RAM_SIZE);
    g_assert_cmphex(le64_to_cpu(handoff.ConsolePolicy), ==,
                    IA64_FW_CONSOLE_VGA);
    g_assert_cmphex(le64_to_cpu(handoff.IdeDmaEnabled), ==, 1);
    g_assert_cmphex(le64_to_cpu(handoff.DebugPortFlags), ==, 0);
    g_assert_cmphex(le64_to_cpu(handoff.DebugPortBase), ==, 0);
    g_assert_cmphex(le64_to_cpu(handoff.I8042Enabled), ==, i8042);
    g_assert_cmphex(le64_to_cpu(handoff.ProcessorCount), ==, cpus);
    g_assert_cmphex(le64_to_cpu(handoff.NvramPersistent), ==, nvram);
    g_assert_cmphex(le64_to_cpu(handoff.SocketCount), ==, sockets);
    g_assert_cmphex(le64_to_cpu(handoff.CoresPerSocket), ==, cores);
    g_assert_cmphex(le64_to_cpu(handoff.ThreadsPerCore), ==, threads);
    g_assert_cmphex(le64_to_cpu(handoff.MapQuirkDisable), ==,
                    IA64_FW_QUIRK_ACPI_LOW_ISLAND | IA64_FW_QUIRK_SCRATCH_2G |
                    IA64_FW_QUIRK_LOW_BOUNDARIES | IA64_FW_QUIRK_LOW_ANCHOR |
                    IA64_FW_QUIRK_ANCHOR_VERSION_SNIFF);
    g_assert_cmphex(le64_to_cpu(handoff.BootTimeout), ==,
                    IA64_FW_BOOT_TIMEOUT_WAIT_FOREVER);
    /* This suite runs on the 460gx machine, which selects the 460GX personality. */
    g_assert_cmphex(le64_to_cpu(handoff.ChipsetProfile), ==,
                    IA64_FW_CHIPSET_460GX);
}

static void test_firmware_handoff_defaults(void)
{
    static const uint8_t expected_v14[sizeof(IA64VpcHandoff)] = {
        0x51, 0x49, 0x41, 0x36, 0x34, 0x52, 0x41, 0x4d,
        0x0e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00,
        0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x5e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* ChipsetProfile=460GX */
    };
    uint8_t actual[sizeof(IA64VpcHandoff)];
    QTestState *qts = ia64_vpc_start(NULL);

    assert_firmware_handoff(qts, 1, 1, 0, 1, 1, 1);
    qtest_memread(qts, IA64_FW_HANDOFF_ADDR, actual, sizeof(actual));
    g_assert_cmpmem(actual, sizeof(actual),
                    expected_v14, sizeof(expected_v14));
    qtest_quit(qts);
}

/* The zx1 machine writes the zx1 firmware personality. */
static void test_firmware_handoff_zx1(void)
{
    IA64VpcHandoff handoff;
    QTestState *qts = qtest_initf("-machine zx1 -m 256M -S");

    g_assert_cmpuint(sizeof(handoff), ==, 128);
    qtest_memread(qts, IA64_FW_HANDOFF_ADDR, &handoff, sizeof(handoff));
    g_assert_cmphex(le64_to_cpu(handoff.Magic), ==, IA64_FW_HANDOFF_MAGIC);
    g_assert_cmphex(le64_to_cpu(handoff.Version), ==,
                    IA64_FW_HANDOFF_VERSION);
    g_assert_cmphex(le64_to_cpu(handoff.ChipsetProfile), ==,
                    IA64_FW_CHIPSET_ZX1);
    qtest_quit(qts);
}

/*
 * The zx1 LBA advertises an AGP capability exactly where Linux hp-agp
 * (drivers/char/agp/hp-agp.c) reads it: PCI_STATUS(0x06) with the capability
 * list bit set, the cap-list pointer at 0x34 -> 0x60, the AGP capability
 * (id 0x02) at 0x60, AGP status at 0x64, and a writable AGP command at 0x68.
 * These are the config reads hp_zx1_lba_init()/hp_zx1_lba_find_capability()
 * perform against the ioremapped LBA CSR block.
 */
static void test_lba_agp_capability(void)
{
    const uint64_t lba = IA64_LBA_CSR_BASE;
    QTestState *qts = qtest_init("-machine zx1 -m 256M -S");

    /* PCI_STATUS: capability-list bit (0x10) present (reset value 0x02b0). */
    g_assert_cmphex(qtest_readw(qts, lba + PCI_STATUS), ==,
                    IA64_LBA_PCI_STATUS_RESET);
    g_assert_cmphex(qtest_readw(qts, lba + PCI_STATUS) & PCI_STATUS_CAP_LIST,
                    ==, PCI_STATUS_CAP_LIST);
    /* Capability-list pointer points at the AGP capability. */
    g_assert_cmphex(qtest_readb(qts, lba + PCI_CAPABILITY_LIST), ==,
                    IA64_LBA_AGP_CAP_OFFSET);
    /* AGP capability id (0x02) at the advertised offset. */
    g_assert_cmphex(qtest_readb(qts, lba + IA64_LBA_AGP_CAP_OFFSET), ==,
                    PCI_CAP_ID_AGP);
    g_assert_cmphex(qtest_readl(qts, lba + IA64_LBA_AGP_CAP_OFFSET) & 0xff, ==,
                    PCI_CAP_ID_AGP);
    /* AGP status (mode) at cap + 4, from the upstream IOA model. */
    g_assert_cmphex(qtest_readl(qts, lba + IA64_LBA_AGP_CAP_OFFSET + 4), ==,
                    (uint32_t)(IA64_LBA_AGP_CAPABILITY >> 32));
    /* AGP command (cap + 8) is writable, masked to IA64_LBA_AGP_COMMAND_WRITABLE. */
    qtest_writel(qts, lba + IA64_LBA_AGP_CAP_OFFSET + 8, 0xffffffffu);
    g_assert_cmphex(qtest_readl(qts, lba + IA64_LBA_AGP_CAP_OFFSET + 8), ==,
                    (uint32_t)IA64_LBA_AGP_COMMAND_WRITABLE);
    /* Mercury identity registers a driver reads to recognise the bridge. */
    /* FUNCTION_ID (0x00): HP vendor 0x103c, device 0x122e. */
    g_assert_cmphex(qtest_readw(qts, lba + 0x00), ==, IA64_LBA_VENDOR_ID);
    g_assert_cmphex(qtest_readw(qts, lba + 0x02), ==, IA64_LBA_DEVICE_ID);
    /* FUNCTION_CLASS (0x08): host-bridge class 0x060000, revision 0x32. */
    g_assert_cmphex(qtest_readl(qts, lba + 0x08), ==,
                    (uint32_t)(IA64_LBA_REVISION | (IA64_LBA_CLASS_CODE << 8)));
    /* BUS_NUMBER (0x58): secondary/subordinate = the Mercury root bus. */
    g_assert_cmphex(qtest_readw(qts, lba + 0x58), ==,
                    IA64_MERCURY_BUS | (IA64_MERCURY_BUS << 8));

    /* Control/decode registers: real Mercury reset values (AGP mode). */
    /* ARBITRATION_MASK (0x80): reset 0x01; writable 0x7f, but the F bit (0x40)
     * is writable only in six-masters mode (our BUS_MODE is AGP) -> 0x3f. */
    g_assert_cmphex(qtest_readl(qts, lba + 0x80), ==, 0x01);
    qtest_writel(qts, lba + 0x80, 0xffffffffu);
    g_assert_cmphex(qtest_readl(qts, lba + 0x80), ==, 0x3f);
    /* STATUS_CONTROL/SIC (0x108): reset-complete (bit 32) reads set. */
    g_assert_cmphex(qtest_readl(qts, lba + 0x10c) & 1u, ==, 1u);
    /* LMMIO_BASE (0x200) reset 0x80000000; SLAVE_CONTROL (0x278) reset 0x6. */
    g_assert_cmphex(qtest_readl(qts, lba + 0x200), ==, 0x80000000u);
    g_assert_cmphex(qtest_readl(qts, lba + 0x278), ==, 0x6);
    /* BUS_MODE (0x620): AGP bit set. */
    g_assert_cmphex(qtest_readl(qts, lba + 0x620) & 1u, ==, 1u);
    /* CONFIG_ADDRESS (0x40) is a writable selector, masked to 0x00fffffc. */
    qtest_writel(qts, lba + 0x40, 0xffffffffu);
    g_assert_cmphex(qtest_readl(qts, lba + 0x40), ==, 0x00fffffcu);
    /* CONFIG_ADDRESS/DATA reach the Mercury bus: select the graphics adapter
     * (bus IA64_MERCURY_BUS, dev 0, func 0, reg 0) and read its PCI vendor id
     * (ATI 0x1002) through CONFIG_DATA (0x48). */
    qtest_writel(qts, lba + 0x40, (uint32_t)IA64_MERCURY_BUS << 16);
    g_assert_cmphex(qtest_readw(qts, lba + 0x48), ==, 0x1002);
    qtest_quit(qts);
}

/*
 * The SBA/IOC identity registers Linux sba_iommu's ioc_init() reads at the IOC
 * function block (CSR base + 0x1000 FUNC_ID, + 0x1008 FCLASS).  FUNC_ID must
 * read as the zx1 IOC (device 0x122a, vendor 0x103c) so func_id == ZX1_IOC_ID
 * matches and the driver runs ioc_zx1_init(); FCLASS's low byte is the IOC
 * revision (0x23 = 2.3), which must be >= 2.0.  Values from the HP zx1 mio ERS.
 */
static void test_sba_ioc_identity(void)
{
    const uint64_t ioc = IA64_SBA_CSR_BASE + 0x1000;
    QTestState *qts = qtest_init("-machine zx1 -m 256M -S");

    /* 64-bit reads, as the driver performs them. */
    g_assert_cmphex(qtest_readq(qts, ioc + 0x000), ==, IA64_SBA_IOC_FUNC_ID);
    g_assert_cmphex(qtest_readq(qts, ioc + 0x008), ==, IA64_SBA_IOC_FCLASS);
    /* Vendor (HP) and device (zx1 IOC) sub-fields, and the >= 2.0 revision. */
    g_assert_cmphex(qtest_readw(qts, ioc + 0x000), ==, 0x103c);
    g_assert_cmphex(qtest_readw(qts, ioc + 0x002), ==, 0x122a);
    g_assert_cmphex(qtest_readb(qts, ioc + 0x008), ==, 0x23);
    qtest_quit(qts);
}

/*
 * The HP zx1 Mercury (LBA) presents a second PCI root bus.  It is reached
 * through the single segment-0 ECAM window by bus-number dispatch in
 * ia64_pci.c: a config cycle whose bus field equals IA64_MERCURY_BUS is routed
 * to the Mercury root bus instead of PCI0.  With no device on it (before the
 * graphics is moved there) a config read is open-bus all-ones; the machine must
 * boot with the second root present and the config handler must not fault.
 * (The ECAM aperture is IA64_PCI_CONFIG_SIZE = 64 MiB, decoding buses 0..63
 * only, so IA64_MERCURY_BUS must stay <= 0x3f -- enforced at compile time by a
 * QEMU_BUILD_BUG_ON in hw/ia64/ia64_mercury.c.)
 */
/*
 * QLogic ISP12160.  Both XP and Server 2003 match their in-box ql12160.sys
 * on PCI\VEN_1077&DEV_1216&SUBSYS_00071077, so the identity has to be
 * exact.  The mailbox handshake is the first thing that driver does.
 */
/*
 * The QLogic holds the board's SCSI seat at 01:00.0 on the first WXB root
 * (see plans/460gx-i2000-fidelity-plan.md), so its BARs come out of that
 * root's aperture, and it is there by default.
 */
#define IA64_ISP_MMIO_BASE      (IA64_PCI_MMIO_BASE + 0x0c000000ULL)
#define IA64_ISP_SLOT           IA64_460GX_WXB0_SCSI_SLOT
#define IA64_ISP_BUS            IA64_460GX_WXB0_BUS
#define IA64_ISP_REG_ISTATUS    0x0aU
#define IA64_ISP_REG_SEMAPHORE  0x0cU
#define IA64_ISP_REG_MAILBOX0   0x70U
#define IA64_ISP_REG_HOST_CMD   0xc0U
#define IA64_ISP_HC_SET_HOST_INT   0x5000U
#define IA64_ISP_HC_CLEAR_RISC_INT 0x7000U
#define IA64_ISP_ISTATUS_RISC_INT  0x0004U
#define IA64_ISP_SEMAPHORE_LOCK    0x0001U
#define IA64_ISP_MBC_NO_OP         0x0000U
#define IA64_ISP_MBS_COMMAND_COMPLETE 0x4000U

static void test_isp12160_mailbox(void)
{
    const uint64_t cfg = IA64_PCI_CONFIG_BASE +
                         ((uint64_t)IA64_ISP_BUS << 20) +
                         ((uint64_t)IA64_ISP_SLOT << 15);
    QTestState *qts = qtest_init("-machine 460gx -cpu merced -m 256M -S");
    unsigned int i;

    g_assert_cmphex(qtest_readl(qts, cfg), ==, 0x12161077);
    g_assert_cmphex(qtest_readl(qts, cfg + PCI_SUBSYSTEM_VENDOR_ID), ==,
                    0x00071077);

    /* A NOP through the mailbox completes and raises the RISC interrupt. */
    qtest_writew(qts, IA64_ISP_MMIO_BASE + IA64_ISP_REG_MAILBOX0,
                 IA64_ISP_MBC_NO_OP);
    qtest_writew(qts, IA64_ISP_MMIO_BASE + IA64_ISP_REG_HOST_CMD,
                 IA64_ISP_HC_SET_HOST_INT);
    for (i = 0; i < 1000; i++) {
        if (qtest_readw(qts, IA64_ISP_MMIO_BASE + IA64_ISP_REG_SEMAPHORE) &
            IA64_ISP_SEMAPHORE_LOCK) {
            break;
        }
    }
    g_assert_cmphex(qtest_readw(qts, IA64_ISP_MMIO_BASE +
                                IA64_ISP_REG_SEMAPHORE) &
                    IA64_ISP_SEMAPHORE_LOCK, ==, IA64_ISP_SEMAPHORE_LOCK);
    g_assert_cmphex(qtest_readw(qts, IA64_ISP_MMIO_BASE +
                                IA64_ISP_REG_MAILBOX0), ==,
                    IA64_ISP_MBS_COMMAND_COMPLETE);
    g_assert_cmphex(qtest_readw(qts, IA64_ISP_MMIO_BASE +
                                IA64_ISP_REG_ISTATUS) &
                    IA64_ISP_ISTATUS_RISC_INT, ==,
                    IA64_ISP_ISTATUS_RISC_INT);

    /* Releasing the semaphore and acknowledging clears the interrupt. */
    qtest_writew(qts, IA64_ISP_MMIO_BASE + IA64_ISP_REG_SEMAPHORE, 0);
    qtest_writew(qts, IA64_ISP_MMIO_BASE + IA64_ISP_REG_HOST_CMD,
                 IA64_ISP_HC_CLEAR_RISC_INT);
    g_assert_cmphex(qtest_readw(qts, IA64_ISP_MMIO_BASE +
                                IA64_ISP_REG_ISTATUS) &
                    IA64_ISP_ISTATUS_RISC_INT, ==, 0);
    qtest_quit(qts);
}

/*
 * A QLogic RISC image records its own length, in words, at image word 3, and
 * the words up to that length sum to zero.  ql12160.sys rounds its download
 * up to whole transfer chunks, so the adapter also receives whatever the
 * driver's bounce buffer held past the image end; MBC_VERIFY_CHECKSUM must
 * ignore that tail.  Summing it instead made the checksum fail, and the
 * driver's failure path then bugchecked Server 2003 with a STOP 0xD1.
 */
#define IA64_ISP_FW_DMA_ADDR    0x00130000U
#define IA64_ISP_FW_RISC_ADDR   0x1000U
#define IA64_ISP_FW_WORDS       12U     /* declared image length */
#define IA64_ISP_FW_CHUNK       8U      /* words per LOAD RAM, as a driver */
#define IA64_ISP_FW_SENT        (2U * IA64_ISP_FW_CHUNK)
#define IA64_ISP_MBC_VERIFY_CHECKSUM  0x0007U
#define IA64_ISP_MBC_LOAD_RAM_A64     0x0009U

static uint16_t isp_mailbox(QTestState *qts, const uint16_t *in,
                            unsigned int count)
{
    uint16_t status = 0;
    unsigned int i;

    /*
     * Mailbox 0 goes first: until the command word is staged, writes to
     * mailboxes 4 and 5 are queue doorbells rather than command words.
     */
    for (i = 0; i < count; i++) {
        qtest_writew(qts, IA64_ISP_MMIO_BASE + IA64_ISP_REG_MAILBOX0 + i * 2,
                     in[i]);
    }
    qtest_writew(qts, IA64_ISP_MMIO_BASE + IA64_ISP_REG_HOST_CMD,
                 IA64_ISP_HC_SET_HOST_INT);
    for (i = 0; i < 1000; i++) {
        if (qtest_readw(qts, IA64_ISP_MMIO_BASE + IA64_ISP_REG_SEMAPHORE) &
            IA64_ISP_SEMAPHORE_LOCK) {
            break;
        }
    }
    status = qtest_readw(qts, IA64_ISP_MMIO_BASE + IA64_ISP_REG_MAILBOX0);
    qtest_writew(qts, IA64_ISP_MMIO_BASE + IA64_ISP_REG_SEMAPHORE, 0);
    qtest_writew(qts, IA64_ISP_MMIO_BASE + IA64_ISP_REG_HOST_CMD,
                 IA64_ISP_HC_CLEAR_RISC_INT);
    return status;
}

/* Download the image in chunks, exactly as a driver does, and verify it. */
static uint16_t isp_load_and_verify(QTestState *qts, const uint16_t *image)
{
    uint16_t mb[8];
    unsigned int chunk;

    for (chunk = 0; chunk * IA64_ISP_FW_CHUNK < IA64_ISP_FW_SENT; chunk++) {
        uint32_t offset = chunk * IA64_ISP_FW_CHUNK;
        uint32_t dma = IA64_ISP_FW_DMA_ADDR + offset * 2;

        qtest_memwrite(qts, dma, image + offset, IA64_ISP_FW_CHUNK * 2);
        memset(mb, 0, sizeof(mb));
        mb[0] = IA64_ISP_MBC_LOAD_RAM_A64;
        mb[1] = IA64_ISP_FW_RISC_ADDR + offset;
        mb[2] = dma >> 16;
        mb[3] = dma & 0xffffu;
        mb[4] = IA64_ISP_FW_CHUNK;
        g_assert_cmphex(isp_mailbox(qts, mb, 8), ==,
                        IA64_ISP_MBS_COMMAND_COMPLETE);
    }

    memset(mb, 0, sizeof(mb));
    mb[0] = IA64_ISP_MBC_VERIFY_CHECKSUM;
    mb[1] = IA64_ISP_FW_RISC_ADDR;
    return isp_mailbox(qts, mb, 2);
}

static void isp_build_firmware(uint16_t *image)
{
    uint16_t sum = 0;
    unsigned int i;

    memset(image, 0, IA64_ISP_FW_SENT * sizeof(*image));
    image[0] = 0x0804;                      /* entry branch, as a real image */
    image[3] = IA64_ISP_FW_WORDS;           /* the declared length */
    for (i = 4; i < IA64_ISP_FW_WORDS - 1; i++) {
        image[i] = 0x1000 + i;
    }
    for (i = 0; i < IA64_ISP_FW_WORDS - 1; i++) {
        sum += image[i];
    }
    image[IA64_ISP_FW_WORDS - 1] = -sum;    /* image words sum to zero */
    /* The tail past the declared length is bounce-buffer residue. */
    for (i = IA64_ISP_FW_WORDS; i < IA64_ISP_FW_SENT; i++) {
        image[i] = 0xdead;
    }
}

static void test_isp12160_firmware_checksum(void)
{
    const uint64_t cfg = IA64_PCI_CONFIG_BASE +
                         ((uint64_t)IA64_ISP_BUS << 20) +
                         ((uint64_t)IA64_ISP_SLOT << 15);
    QTestState *qts = qtest_init("-machine 460gx -cpu merced -m 256M -S");
    uint16_t image[IA64_ISP_FW_SENT];

    qtest_writew(qts, cfg + PCI_COMMAND,
                 PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);

    isp_build_firmware(image);
    g_assert_cmphex(isp_load_and_verify(qts, image), ==,
                    IA64_ISP_MBS_COMMAND_COMPLETE);
    qtest_quit(qts);

    /* Corrupting a word inside the declared image must still be caught. */
    qts = qtest_init("-machine 460gx -cpu merced -m 256M -S");
    qtest_writew(qts, cfg + PCI_COMMAND,
                 PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
    isp_build_firmware(image);
    image[5] ^= 0x0001;
    g_assert_cmphex(isp_load_and_verify(qts, image), !=,
                    IA64_ISP_MBS_COMMAND_COMPLETE);
    qtest_quit(qts);
}

/*
 * CS4281 audio.  The real i2000 I/O board carries this codec, so the
 * machine can add it with audio=on.  Walk the bring-up a driver performs:
 * release the sound-power reset, start the clock, wait for the AC '97 link
 * to report the codec ready, then read a codec register through the
 * command port.
 */
#define IA64_CS4281_BA0_BASE    (IA64_PCI_MMIO_BASE + 0x01800000ULL)
#define IA64_CS4281_BA0_HISR    0x0000U
#define IA64_CS4281_BA0_HIMR    0x000cU
#define IA64_CS4281_BA0_SSVID   0x03fcU
#define IA64_CS4281_BA0_CLKCR1  0x0400U
#define IA64_CS4281_BA0_SERMC   0x0420U
#define IA64_CS4281_BA0_ACCTL   0x0460U
#define IA64_CS4281_BA0_ACSTS   0x0464U
#define IA64_CS4281_BA0_ACCAD   0x046cU
#define IA64_CS4281_BA0_ACSDA   0x047cU
#define IA64_CS4281_BA0_CWPR    0x03e0U
#define IA64_CS4281_CWPR_KEY    0x4281U
#define IA64_CS4281_BA0_SPMC    0x03ecU
#define IA64_CS4281_BA0_SSPM    0x0740U
#define IA64_CS4281_SPMC_RSTN   (1U << 0)
#define IA64_CS4281_SSPM_ACLEN  (1U << 2)
#define IA64_CS4281_CLKCR1_CLKON  (1U << 25)
#define IA64_CS4281_CLKCR1_DLLRDY (1U << 24)
#define IA64_CS4281_CLKCR1_SWCE   (1U << 5)
#define IA64_CS4281_CLKCR1_DLLP   (1U << 4)
#define IA64_CS4281_ACCTL_CRW   (1U << 4)
#define IA64_CS4281_ACCTL_DCV   (1U << 3)
#define IA64_CS4281_ACCTL_VFRM  (1U << 2)
#define IA64_CS4281_ACCTL_ESYN  (1U << 1)
#define IA64_CS4281_ACSTS_CRDY  (1U << 0)
#define IA64_CS4281_AC97_VENDOR_ID1 0x7cU

static void test_cs4281_codec_access(void)
{
    QTestState *qts = qtest_init("-machine 460gx,audio=on -cpu merced "
                                 "-m 256M -S");
    uint32_t value;

    /* Reset defaults published by BA0. */
    g_assert_cmphex(qtest_readl(qts, IA64_CS4281_BA0_BASE +
                                IA64_CS4281_BA0_HIMR), ==, 0x7fffffff);
    g_assert_cmphex(qtest_readl(qts, IA64_CS4281_BA0_BASE +
                                IA64_CS4281_BA0_SERMC), ==, 0x00010003);
    g_assert_cmphex(qtest_readl(qts, IA64_CS4281_BA0_BASE +
                                IA64_CS4281_BA0_SSVID), ==, 0x42538086);

    /*
     * The PLL only locks once the sound power reset is released; CLKON and
     * DLLRDY are status bits the model derives, not writable ones.
     */
    qtest_writel(qts, IA64_CS4281_BA0_BASE + IA64_CS4281_BA0_CLKCR1,
                 IA64_CS4281_CLKCR1_SWCE | IA64_CS4281_CLKCR1_DLLP);
    g_assert_cmphex(qtest_readl(qts, IA64_CS4281_BA0_BASE +
                                IA64_CS4281_BA0_CLKCR1) &
                    (IA64_CS4281_CLKCR1_DLLRDY |
                     IA64_CS4281_CLKCR1_CLKON), ==, 0);

    /* SPMC is a vendor register: it ignores writes until CWPR is unlocked. */
    qtest_writel(qts, IA64_CS4281_BA0_BASE + IA64_CS4281_BA0_SPMC,
                 IA64_CS4281_SPMC_RSTN);
    g_assert_cmphex(qtest_readl(qts, IA64_CS4281_BA0_BASE +
                                IA64_CS4281_BA0_SPMC), ==, 0);

    qtest_writel(qts, IA64_CS4281_BA0_BASE + IA64_CS4281_BA0_CWPR,
                 IA64_CS4281_CWPR_KEY);
    qtest_writel(qts, IA64_CS4281_BA0_BASE + IA64_CS4281_BA0_SPMC,
                 IA64_CS4281_SPMC_RSTN);
    g_assert_cmphex(qtest_readl(qts, IA64_CS4281_BA0_BASE +
                                IA64_CS4281_BA0_CLKCR1) &
                    (IA64_CS4281_CLKCR1_DLLRDY |
                     IA64_CS4281_CLKCR1_CLKON), ==,
                    IA64_CS4281_CLKCR1_DLLRDY |
                    IA64_CS4281_CLKCR1_CLKON);

    /* Enabling the AC-link and driving a frame makes the codec report ready. */
    qtest_writel(qts, IA64_CS4281_BA0_BASE + IA64_CS4281_BA0_SSPM,
                 IA64_CS4281_SSPM_ACLEN);
    qtest_writel(qts, IA64_CS4281_BA0_BASE + IA64_CS4281_BA0_ACCTL,
                 IA64_CS4281_ACCTL_ESYN | IA64_CS4281_ACCTL_VFRM);
    g_assert_cmphex(qtest_readl(qts, IA64_CS4281_BA0_BASE +
                                IA64_CS4281_BA0_ACSTS) &
                    IA64_CS4281_ACSTS_CRDY, ==, IA64_CS4281_ACSTS_CRDY);

    /* A codec read returns the CS4297A vendor id and clears DCV. */
    qtest_writel(qts, IA64_CS4281_BA0_BASE + IA64_CS4281_BA0_ACCAD,
                 IA64_CS4281_AC97_VENDOR_ID1);
    qtest_writel(qts, IA64_CS4281_BA0_BASE + IA64_CS4281_BA0_ACCTL,
                 IA64_CS4281_ACCTL_ESYN | IA64_CS4281_ACCTL_VFRM |
                 IA64_CS4281_ACCTL_CRW | IA64_CS4281_ACCTL_DCV);
    g_assert_cmphex(qtest_readl(qts, IA64_CS4281_BA0_BASE +
                                IA64_CS4281_BA0_ACSDA), ==, 0x4352);
    value = qtest_readl(qts, IA64_CS4281_BA0_BASE + IA64_CS4281_BA0_ACCTL);
    g_assert_cmphex(value & IA64_CS4281_ACCTL_DCV, ==, 0);

    /* No interrupt is pending after a bare codec access. */
    g_assert_cmphex(qtest_readl(qts, IA64_CS4281_BA0_BASE +
                                IA64_CS4281_BA0_HISR) & 0xff00, ==, 0);
    qtest_quit(qts);
}

/*
 * OHCI root-hub port resume.  Resume signalling takes 20 ms plus a
 * low-speed EOP and a 3 ms recovery time before the port reports itself
 * resumed; a controller-wide USBRESUME or a port reset ends the suspend
 * without the port-resume status change.  A USB device is attached because
 * a port only suspends while something is connected to it.
 */
#define IA64_OHCI_MMIO_BASE     (IA64_PCI_MMIO_BASE + 0x00010000ULL)
#define IA64_OHCI_CONTROL       0x04U
#define IA64_OHCI_INTR_STATUS   0x0cU
#define IA64_OHCI_RH_PORT_1     0x54U
#define IA64_OHCI_USB_RESUME    0x40U
#define IA64_OHCI_USB_SUSPEND   0xc0U
#define IA64_OHCI_INTR_RHSC     (1U << 6)
#define IA64_OHCI_PORT_CCS      (1U << 0)
#define IA64_OHCI_PORT_PES      (1U << 1)
#define IA64_OHCI_PORT_PSS      (1U << 2)
#define IA64_OHCI_PORT_POCI     (1U << 3)
#define IA64_OHCI_PORT_PRS      (1U << 4)
#define IA64_OHCI_PORT_CSC      (1U << 16)
#define IA64_OHCI_PORT_PSSC     (1U << 18)
#define IA64_OHCI_PORT_PRSC     (1U << 20)
#define IA64_OHCI_RESUME_NS     ((20 * NANOSECONDS_PER_SECOND / 1000) + \
                                 (3 * NANOSECONDS_PER_SECOND / 1500000) + \
                                 (3 * NANOSECONDS_PER_SECOND / 1000))

static uint32_t ohci_port_status(QTestState *qts)
{
    return qtest_readl(qts, IA64_OHCI_MMIO_BASE + IA64_OHCI_RH_PORT_1);
}

static void ohci_port_write(QTestState *qts, uint32_t value)
{
    qtest_writel(qts, IA64_OHCI_MMIO_BASE + IA64_OHCI_RH_PORT_1, value);
}

static void ohci_clear_rhsc(QTestState *qts)
{
    qtest_writel(qts, IA64_OHCI_MMIO_BASE + IA64_OHCI_INTR_STATUS,
                 IA64_OHCI_INTR_RHSC);
    g_assert_cmphex(qtest_readl(qts, IA64_OHCI_MMIO_BASE +
                                IA64_OHCI_INTR_STATUS) &
                    IA64_OHCI_INTR_RHSC, ==, 0);
}

static QTestState *ohci_start_suspended_port(void)
{
    /*
     * No "-S": the qtest accelerator never executes guest code, but a
     * stopped VM also freezes QEMU_CLOCK_VIRTUAL, which the resume timer
     * runs on.
     */
    QTestState *qts = qtest_init("-machine ia64-vpc -m 256M "
                                 "-device usb-kbd,bus=usb-bus.0");

    g_assert_cmphex(ohci_port_status(qts) & IA64_OHCI_PORT_CCS, ==,
                    IA64_OHCI_PORT_CCS);
    ohci_port_write(qts, IA64_OHCI_PORT_CSC | IA64_OHCI_PORT_PES);
    g_assert_cmphex(ohci_port_status(qts) & IA64_OHCI_PORT_PES, ==,
                    IA64_OHCI_PORT_PES);

    ohci_port_write(qts, IA64_OHCI_PORT_PSS);
    g_assert_cmphex(ohci_port_status(qts) & IA64_OHCI_PORT_PSS, ==,
                    IA64_OHCI_PORT_PSS);
    ohci_clear_rhsc(qts);
    return qts;
}

static void test_ohci_port_resume(void)
{
    QTestState *qts = ohci_start_suspended_port();
    uint32_t status;

    ohci_port_write(qts, IA64_OHCI_PORT_POCI);
    status = ohci_port_status(qts);
    g_assert_cmphex(status & IA64_OHCI_PORT_PSS, ==, IA64_OHCI_PORT_PSS);
    g_assert_cmphex(status & IA64_OHCI_PORT_PSSC, ==, 0);

    qtest_clock_step(qts, IA64_OHCI_RESUME_NS - 1);
    status = ohci_port_status(qts);
    g_assert_cmphex(status & IA64_OHCI_PORT_PSS, ==, IA64_OHCI_PORT_PSS);
    g_assert_cmphex(status & IA64_OHCI_PORT_PSSC, ==, 0);

    qtest_clock_step(qts, 1);
    status = ohci_port_status(qts);
    g_assert_cmphex(status & IA64_OHCI_PORT_PSS, ==, 0);
    g_assert_cmphex(status & IA64_OHCI_PORT_PES, ==, IA64_OHCI_PORT_PES);
    g_assert_cmphex(status & IA64_OHCI_PORT_PSSC, ==, IA64_OHCI_PORT_PSSC);
    g_assert_cmphex(qtest_readl(qts, IA64_OHCI_MMIO_BASE +
                                IA64_OHCI_INTR_STATUS) &
                    IA64_OHCI_INTR_RHSC, ==, IA64_OHCI_INTR_RHSC);

    ohci_port_write(qts, IA64_OHCI_PORT_PSSC);
    g_assert_cmphex(ohci_port_status(qts) & IA64_OHCI_PORT_PSSC, ==, 0);
    qtest_quit(qts);
}

static void test_ohci_controller_resume(void)
{
    QTestState *qts = ohci_start_suspended_port();
    uint32_t status;

    qtest_writel(qts, IA64_OHCI_MMIO_BASE + IA64_OHCI_CONTROL,
                 IA64_OHCI_USB_SUSPEND);
    ohci_port_write(qts, IA64_OHCI_PORT_POCI);
    qtest_writel(qts, IA64_OHCI_MMIO_BASE + IA64_OHCI_CONTROL,
                 IA64_OHCI_USB_RESUME);

    /* USBRESUME ends the suspend without a port status change. */
    status = ohci_port_status(qts);
    g_assert_cmphex(status & IA64_OHCI_PORT_PES, ==, IA64_OHCI_PORT_PES);
    g_assert_cmphex(status & IA64_OHCI_PORT_PSS, ==, 0);
    g_assert_cmphex(status & IA64_OHCI_PORT_PSSC, ==, 0);
    g_assert_cmphex(qtest_readl(qts, IA64_OHCI_MMIO_BASE +
                                IA64_OHCI_CONTROL), ==,
                    IA64_OHCI_USB_RESUME);
    g_assert_cmphex(qtest_readl(qts, IA64_OHCI_MMIO_BASE +
                                IA64_OHCI_INTR_STATUS) &
                    IA64_OHCI_INTR_RHSC, ==, 0);

    qtest_clock_step(qts, IA64_OHCI_RESUME_NS);
    g_assert_cmphex(ohci_port_status(qts) & IA64_OHCI_PORT_PSSC, ==, 0);
    qtest_quit(qts);
}

static void test_ohci_reset_suspended_port(void)
{
    QTestState *qts = ohci_start_suspended_port();
    uint32_t status;

    ohci_port_write(qts, IA64_OHCI_PORT_POCI);
    ohci_port_write(qts, IA64_OHCI_PORT_PRS);

    /* A reset cancels the pending resume and reports only PRSC. */
    status = ohci_port_status(qts);
    g_assert_cmphex(status & IA64_OHCI_PORT_PES, ==, IA64_OHCI_PORT_PES);
    g_assert_cmphex(status & IA64_OHCI_PORT_PSS, ==, 0);
    g_assert_cmphex(status & IA64_OHCI_PORT_PSSC, ==, 0);
    g_assert_cmphex(status & IA64_OHCI_PORT_PRSC, ==, IA64_OHCI_PORT_PRSC);
    g_assert_cmphex(qtest_readl(qts, IA64_OHCI_MMIO_BASE +
                                IA64_OHCI_INTR_STATUS) &
                    IA64_OHCI_INTR_RHSC, ==, IA64_OHCI_INTR_RHSC);

    qtest_clock_step(qts, IA64_OHCI_RESUME_NS);
    g_assert_cmphex(ohci_port_status(qts) & IA64_OHCI_PORT_PSSC, ==, 0);
    qtest_quit(qts);
}

/*
 * eepro100 CSR windows.  The dword at the Flash CSR spans Flash control
 * (bits 15:0) and EEPROM control (bits 31:16), and the MDI CSR reaches PHY
 * registers 0 to 31, not just 0 to 6.  The NIC is added on a free slot of
 * the root bus that "-device" defaults to and given a BAR by hand, because
 * the machine only assigns BARs to the devices it creates itself.
 */
#define IA64_E100_SLOT          8U
#define IA64_E100_CSR_BASE      0x00000000f6000000ULL
#define IA64_E100_SCB_FLASH     12U
#define IA64_E100_SCB_EEPROM    14U
#define IA64_E100_SCB_CTRL_MDI  16U
#define IA64_E100_EEPROM_CS     0x02U
#define IA64_E100_MDI_READY     (1U << 28)
#define IA64_E100_MDI_OP_READ   (2U << 26)
#define IA64_E100_MDI_PHY_1     (1U << 21)

#define IA64_E100_FLASH_BASE    0x00000000f6100000ULL
#define IA64_E100_SCB_POINTER   4U
#define IA64_E100_EEPROM_SK     0x01U
#define IA64_E100_EEPROM_DI     0x04U
#define IA64_E100_EEPROM_DO     0x08U

static void e100_eeprom_clock(QTestState *qts, uint16_t control)
{
    qtest_writew(qts, IA64_E100_CSR_BASE + IA64_E100_SCB_EEPROM, control);
    qtest_writew(qts, IA64_E100_CSR_BASE + IA64_E100_SCB_EEPROM,
                 control | IA64_E100_EEPROM_SK);
}

/* 93C46 read: start bit, opcode 10, six address bits, sixteen data bits. */
static uint16_t e100_eeprom_read_word(QTestState *qts, uint8_t address)
{
    static const uint8_t opcode[] = { 1, 1, 0 };
    uint16_t value = 0;
    unsigned int i;
    int bit;

    qtest_writew(qts, IA64_E100_CSR_BASE + IA64_E100_SCB_EEPROM,
                 IA64_E100_EEPROM_CS);
    for (i = 0; i < ARRAY_SIZE(opcode); i++) {
        e100_eeprom_clock(qts, IA64_E100_EEPROM_CS |
                          (opcode[i] ? IA64_E100_EEPROM_DI : 0));
    }
    for (bit = 5; bit >= 0; bit--) {
        e100_eeprom_clock(qts, IA64_E100_EEPROM_CS |
                          ((address >> bit) & 1 ? IA64_E100_EEPROM_DI : 0));
    }
    for (i = 0; i < 16; i++) {
        e100_eeprom_clock(qts, IA64_E100_EEPROM_CS);
        value = (value << 1) |
                ((qtest_readw(qts, IA64_E100_CSR_BASE +
                              IA64_E100_SCB_EEPROM) &
                  IA64_E100_EEPROM_DO) ? 1 : 0);
    }
    qtest_writew(qts, IA64_E100_CSR_BASE + IA64_E100_SCB_EEPROM, 0);
    return value;
}

static void test_eepro100_eeprom_map(void)
{
    const uint64_t cfg = IA64_PCI_CONFIG_BASE +
                         ((uint64_t)IA64_MERCURY_BUS << 20) +
                         ((uint64_t)IA64_E100_SLOT << 15);
    QTestState *qts = qtest_init("-machine ia64-vpc -m 256M -S "
                                 "-device i82559c,bus=mercury,addr=8,"
                                 "romfile=,mac=52:54:00:12:34:56");
    uint32_t checksum;
    unsigned int i;

    qtest_writel(qts, cfg + PCI_BASE_ADDRESS_0, IA64_E100_CSR_BASE);
    qtest_writew(qts, cfg + PCI_COMMAND,
                 PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);

    /* The MAC occupies words 0 to 2 as little-endian pairs. */
    g_assert_cmphex(e100_eeprom_read_word(qts, 0), ==, 0x5452);
    g_assert_cmphex(e100_eeprom_read_word(qts, 1), ==, 0x1200);
    g_assert_cmphex(e100_eeprom_read_word(qts, 2), ==, 0x5634);

    /* The 82559-family map: compatibility, controller type and PHY id. */
    g_assert_cmphex(e100_eeprom_read_word(qts, 0x03), ==, 0x0203);
    g_assert_cmphex(e100_eeprom_read_word(qts, 0x05), ==, 0x0201);
    g_assert_cmphex(e100_eeprom_read_word(qts, 0x06), ==, 0x4701);

    /*
     * Word 0x0a carries the id flags; boot-disable is set because this
     * adapter was given no option ROM.
     */
    g_assert_cmphex(e100_eeprom_read_word(qts, 0x0a), ==, 0x4880);

    /* Words 0 to 62 plus the stored checksum come to 0xbaba. */
    checksum = 0;
    for (i = 0; i < 64; i++) {
        checksum += e100_eeprom_read_word(qts, i);
    }
    g_assert_cmphex(checksum & 0xffff, ==, 0xbaba);

    /*
     * The Flash aperture is a separate BAR that contains no Flash storage:
     * it reads zero and does not alias the CSR window.
     */
    qtest_writel(qts, cfg + PCI_BASE_ADDRESS_2, IA64_E100_FLASH_BASE);
    qtest_writel(qts, IA64_E100_CSR_BASE + IA64_E100_SCB_POINTER,
                 0x12345678);
    g_assert_cmphex(qtest_readl(qts, IA64_E100_CSR_BASE +
                                IA64_E100_SCB_POINTER), ==, 0x12345678);
    g_assert_cmphex(qtest_readl(qts, IA64_E100_FLASH_BASE +
                                IA64_E100_SCB_POINTER), ==, 0);
    qtest_writel(qts, IA64_E100_FLASH_BASE + IA64_E100_SCB_POINTER,
                 UINT32_MAX);
    g_assert_cmphex(qtest_readl(qts, IA64_E100_CSR_BASE +
                                IA64_E100_SCB_POINTER), ==, 0x12345678);
    qtest_quit(qts);
}

static void test_eepro100_csr_windows(void)
{
    const uint64_t cfg = IA64_PCI_CONFIG_BASE +
                         ((uint64_t)IA64_MERCURY_BUS << 20) +
                         ((uint64_t)IA64_E100_SLOT << 15);
    QTestState *qts = qtest_init("-machine ia64-vpc -m 256M -S "
                                 "-device i82559c,bus=mercury,addr=8,"
                                 "romfile=");
    uint32_t mdi;

    g_assert_cmphex(qtest_readl(qts, cfg), ==, 0x12298086);
    qtest_writel(qts, cfg + PCI_BASE_ADDRESS_0, IA64_E100_CSR_BASE);
    qtest_writew(qts, cfg + PCI_COMMAND,
                 PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);

    qtest_writew(qts, IA64_E100_CSR_BASE + IA64_E100_SCB_EEPROM,
                 IA64_E100_EEPROM_CS);
    g_assert_cmphex(qtest_readw(qts, IA64_E100_CSR_BASE +
                                IA64_E100_SCB_EEPROM) &
                    IA64_E100_EEPROM_CS, ==, IA64_E100_EEPROM_CS);
    g_assert_cmphex(qtest_readl(qts, IA64_E100_CSR_BASE +
                                IA64_E100_SCB_FLASH) >> 16, ==,
                    qtest_readw(qts, IA64_E100_CSR_BASE +
                                IA64_E100_SCB_EEPROM));
    g_assert_cmphex(qtest_readl(qts, IA64_E100_CSR_BASE +
                                IA64_E100_SCB_FLASH) & 0xffff, ==, 0);

    /* A dword write takes EEPROM control from the same half. */
    qtest_writel(qts, IA64_E100_CSR_BASE + IA64_E100_SCB_FLASH, 0);
    g_assert_cmphex(qtest_readw(qts, IA64_E100_CSR_BASE +
                                IA64_E100_SCB_EEPROM) &
                    IA64_E100_EEPROM_CS, ==, 0);

    /* PHY register 18 holds a non-zero reset default. */
    qtest_writel(qts, IA64_E100_CSR_BASE + IA64_E100_SCB_CTRL_MDI,
                 IA64_E100_MDI_OP_READ | IA64_E100_MDI_PHY_1 | (18U << 16));
    mdi = qtest_readl(qts, IA64_E100_CSR_BASE + IA64_E100_SCB_CTRL_MDI);
    g_assert_cmphex(mdi & IA64_E100_MDI_READY, ==, IA64_E100_MDI_READY);
    g_assert_cmphex(mdi & 0xffff, ==, 0x0001);

    qtest_quit(qts);
}

static void test_mercury_config_dispatch(void)
{
    const uint64_t ecam = IA64_PCI_CONFIG_BASE;
    const uint64_t merc = ecam + ((uint64_t)IA64_MERCURY_BUS << 20);
    QTestState *qts = qtest_init("-machine zx1 -m 256M -S");

    /* The SBA (PCI0 slot 31) reads its HP vendor id -- PCI0 still enumerates. */
    g_assert_cmphex(qtest_readl(qts, ecam + (31ULL << 15)) & 0xffff, ==, 0x103c);
    /*
     * The default AGP graphics master (ATI, vendor 0x1002) lives at slot 0 of
     * the Mercury bus: dispatch routes the config cycle to the second root bus
     * where the device actually is (not to PCI0).
     */
    g_assert_cmphex(qtest_readl(qts, merc +
                    ((uint64_t)IA64_MERCURY_VGA_SLOT << 15)) & 0xffff, ==, 0x1002);
    /* PCI0 slot 5 (the old graphics slot) is now empty on zx1. */
    g_assert_cmphex(qtest_readl(qts, ecam + (5ULL << 15)), ==, 0xffffffffu);
    /* An empty slot on the Mercury bus decodes (dispatch installed) as open bus. */
    g_assert_cmphex(qtest_readl(qts, merc + (1ULL << 15)), ==, 0xffffffffu);
    qtest_quit(qts);
}

/*
 * Real 460GX layout: low DRAM is a single contiguous run from 0 up to the PCI
 * aperture (0xEE000000, ~3.72 GiB); only RAM displaced by that top-of-memory
 * gap spills above 4 GiB.  There is no sub-4 GiB DRAM island and no hole at
 * 2 GiB (the IOSAPIC no longer parks there).  Probe where DRAM actually lands
 * for both the below-aperture and above-4 GiB cases.  The -m values reserve a
 * large address space but qtest only touches a couple of pages, so RSS stays
 * tiny.  Keep in lockstep with ia64_vpc_map_ram() /
 * fw_init_guest_high_ram_ranges().
 */
#define IA64_RAM_AT_2GIB             0x0000000080000000ULL /* was the hole */
#define IA64_HIGH_RAM_BELOW_PCI_BASE 0x0000000080200000ULL
#define IA64_HIGH_RAM_ABOVE_4G_BASE  0x0000000100000000ULL

static void test_ram_high_remap(void)
{
    const uint64_t magic = 0x0123456789abcdefULL;

    /*
     * 2304 MiB fits entirely below the aperture, so DRAM is contiguous across
     * the old 2 GiB seam (both 0x80000000 and 0x80200000 are ordinary backed
     * RAM now) and nothing lands above 4 GiB.
     */
    QTestState *qts = qtest_init("-machine 460gx -m 2304M -S");

    qtest_writeq(qts, IA64_RAM_AT_2GIB, magic);
    g_assert_cmphex(qtest_readq(qts, IA64_RAM_AT_2GIB), ==, magic);
    qtest_writeq(qts, IA64_HIGH_RAM_BELOW_PCI_BASE, magic);
    g_assert_cmphex(qtest_readq(qts, IA64_HIGH_RAM_BELOW_PCI_BASE), ==, magic);
    /* No DRAM above 4 GiB at this size. */
    qtest_writeq(qts, IA64_HIGH_RAM_ABOVE_4G_BASE, magic);
    g_assert_cmphex(qtest_readq(qts, IA64_HIGH_RAM_ABOVE_4G_BASE), !=, magic);
    qtest_quit(qts);

    /*
     * 4096 MiB exceeds the aperture: DRAM is contiguous up to it AND the
     * displaced remainder is remapped above 4 GiB.
     */
    qts = qtest_init("-machine 460gx -m 4096M -S");
    qtest_writeq(qts, IA64_HIGH_RAM_BELOW_PCI_BASE, magic);
    g_assert_cmphex(qtest_readq(qts, IA64_HIGH_RAM_BELOW_PCI_BASE), ==, magic);
    qtest_writeq(qts, IA64_HIGH_RAM_ABOVE_4G_BASE, magic);
    g_assert_cmphex(qtest_readq(qts, IA64_HIGH_RAM_ABOVE_4G_BASE), ==, magic);
    qtest_quit(qts);
}

/*
 * The zx1 machine carves a 1 GiB DRAM hole out of the low band for the SBA
 * "safe IOVA space" window [0x40000000, 0x80000000).  RAM below the hole and
 * from 2 GiB up to the aperture is backed; the hole itself is not; and the
 * 1 GiB displaced by the hole spills above 4 GiB (on top of the ordinary
 * top-of-memory displacement).  The 460gx machine at the same size has no hole
 * -- 0x40000000 is ordinary backed RAM -- which this test asserts as a
 * differential guard.  Keep in lockstep with ia64_vpc_map_ram(),
 * efi_add_low_ram_band() and fw_init_guest_high_ram_ranges().
 */
#define IA64_SBA_HOLE_BASE 0x0000000040000000ULL
#define IA64_SBA_HOLE_LAST 0x000000007ffffff8ULL /* last qword inside the hole */

static void test_ram_hole_zx1(void)
{
    const uint64_t magic = 0x0123456789abcdefULL;
    QTestState *qts;

    /*
     * 4096 MiB on zx1: below the hole is backed, the hole reads back unbacked,
     * 2 GiB resumes backed, and the remainder displaced by both the hole and
     * the top-of-memory gap lands above 4 GiB.
     */
    qts = qtest_init("-machine zx1 -m 4096M -S");

    /* Below the hole: ordinary low RAM. */
    qtest_writeq(qts, IA64_SBA_HOLE_BASE - 8, magic);
    g_assert_cmphex(qtest_readq(qts, IA64_SBA_HOLE_BASE - 8), ==, magic);
    /* The hole itself is not backed by DRAM. */
    qtest_writeq(qts, IA64_SBA_HOLE_BASE, magic);
    g_assert_cmphex(qtest_readq(qts, IA64_SBA_HOLE_BASE), !=, magic);
    qtest_writeq(qts, IA64_SBA_HOLE_LAST, magic);
    g_assert_cmphex(qtest_readq(qts, IA64_SBA_HOLE_LAST), !=, magic);
    /* DRAM resumes at 2 GiB and runs up toward the aperture. */
    qtest_writeq(qts, IA64_RAM_AT_2GIB, magic);
    g_assert_cmphex(qtest_readq(qts, IA64_RAM_AT_2GIB), ==, magic);
    qtest_writeq(qts, IA64_HIGH_RAM_BELOW_PCI_BASE, magic);
    g_assert_cmphex(qtest_readq(qts, IA64_HIGH_RAM_BELOW_PCI_BASE), ==, magic);
    /* The 1 GiB pushed out by the hole is displaced above 4 GiB. */
    qtest_writeq(qts, IA64_HIGH_RAM_ABOVE_4G_BASE, magic);
    g_assert_cmphex(qtest_readq(qts, IA64_HIGH_RAM_ABOVE_4G_BASE), ==, magic);
    qtest_quit(qts);

    /* Differential guard: 460gx at the same size has NO hole at 0x40000000. */
    qts = qtest_init("-machine 460gx -m 4096M -S");
    qtest_writeq(qts, IA64_SBA_HOLE_BASE, magic);
    g_assert_cmphex(qtest_readq(qts, IA64_SBA_HOLE_BASE), ==, magic);
    qtest_writeq(qts, IA64_SBA_HOLE_LAST, magic);
    g_assert_cmphex(qtest_readq(qts, IA64_SBA_HOLE_LAST), ==, magic);
    qtest_quit(qts);

    /*
     * Gate guard: at or below the PCI aperture the hole is NOT carved even on
     * zx1 -- the layout is the contiguous 460gx one, so the firmware's
     * aperture-relative self-placement stays valid.  A 2 GiB guest has ordinary
     * backed RAM across the window and no DRAM above 4 GiB.
     */
    qts = qtest_init("-machine zx1 -m 2048M -S");
    qtest_writeq(qts, IA64_SBA_HOLE_BASE, magic);
    g_assert_cmphex(qtest_readq(qts, IA64_SBA_HOLE_BASE), ==, magic);
    qtest_writeq(qts, IA64_SBA_HOLE_LAST, magic);
    g_assert_cmphex(qtest_readq(qts, IA64_SBA_HOLE_LAST), ==, magic);
    qtest_writeq(qts, IA64_HIGH_RAM_ABOVE_4G_BASE, magic);
    g_assert_cmphex(qtest_readq(qts, IA64_HIGH_RAM_ABOVE_4G_BASE), !=, magic);
    qtest_quit(qts);
}

static void assert_cpu_model_type(const char *cpu_arg, const char *expect_type)
{
    g_autofree char *args = g_strdup_printf("-cpu %s", cpu_arg);
    QTestState *qts = ia64_vpc_start(args);
    g_autoptr(QDict) cpus_resp = NULL;
    QList *cpus;
    QListEntry *entry;
    const char *qom_path = NULL;

    /* The model instantiates and the firmware still hands off on ia64-vpc. */
    assert_firmware_handoff(qts, 1, 1, 0, 1, 1, 1);

    cpus_resp = qtest_qmp(qts, "{'execute':'query-cpus-fast'}");
    g_assert(qdict_haskey(cpus_resp, "return"));
    cpus = qdict_get_qlist(cpus_resp, "return");
    g_assert_cmpuint(qlist_size(cpus), ==, 1);
    QLIST_FOREACH_ENTRY(cpus, entry) {
        qom_path = qdict_get_str(qobject_to(QDict, qlist_entry_obj(entry)),
                                 "qom-path");
        break;
    }
    g_assert_nonnull(qom_path);

    {
        g_autoptr(QDict) type_resp = qtest_qmp(qts,
            "{'execute':'qom-get','arguments':"
            "{'path':%s,'property':'type'}}", qom_path);
        g_assert(qdict_haskey(type_resp, "return"));
        g_assert_cmpstr(qdict_get_str(type_resp, "return"), ==, expect_type);
    }
    qtest_quit(qts);
}

static void test_cpu_merced(void)
{
    /* -cpu merced instantiates the original-Itanium model and boots. */
    assert_cpu_model_type("merced", "merced-ia64-cpu");
}

static void test_cpu_itanium_alias(void)
{
    /* "itanium" is the documented alias for the merced model. */
    assert_cpu_model_type("itanium", "itanium-ia64-cpu");
}

static void test_firmware_handoff_i8042_off(void)
{
    QTestState *qts = qtest_init("-machine 460gx,i8042=off "
                                 "-m 256M -S");

    assert_firmware_handoff(qts, 0, 1, 0, 1, 1, 1);
    qtest_quit(qts);
}

static void test_firmware_handoff_boot_timeout(void)
{
    IA64VpcHandoff handoff;
    /* A finite firmware-boot-timeout overrides the wait-forever default and
     * reaches the OS handoff verbatim, driving the boot manager's countdown. */
    QTestState *qts = qtest_init("-machine 460gx,firmware-boot-timeout=5 "
                                 "-m 256M -S");

    qtest_memread(qts, IA64_FW_HANDOFF_ADDR, &handoff, sizeof(handoff));
    g_assert_cmphex(le64_to_cpu(handoff.Magic), ==, IA64_FW_HANDOFF_MAGIC);
    g_assert_cmphex(le64_to_cpu(handoff.Version), ==, IA64_FW_HANDOFF_VERSION);
    g_assert_cmphex(le64_to_cpu(handoff.BootTimeout), ==, 5);
    qtest_quit(qts);
}

static void test_smp_topology(gconstpointer opaque)
{
    uint64_t count = GPOINTER_TO_UINT(opaque);
    g_autofree char *args = g_strdup_printf("-smp %" PRIu64, count);
    QTestState *qts = ia64_vpc_start(args);
    g_autoptr(QDict) response = NULL;
    QList *cpus;

    assert_firmware_handoff(qts, 1, count, 0, count, 1, 1);
    response = qtest_qmp(qts, "{'execute':'query-cpus-fast'}");
    g_assert(qdict_haskey(response, "return"));
    cpus = qdict_get_qlist(response, "return");
    g_assert_cmpuint(qlist_size(cpus), ==, count);
    qtest_quit(qts);
}

static void test_smp_explicit_topology(void)
{
    QTestState *qts =
        ia64_vpc_start("-smp 4,sockets=1,cores=2,threads=2");

    assert_firmware_handoff(qts, 1, 4, 0, 1, 2, 2);
    qtest_quit(qts);
}

typedef struct TestSmpMulticoreTopology {
    const char *name;
    unsigned sockets;
    unsigned cores;
} TestSmpMulticoreTopology;

static const TestSmpMulticoreTopology smp_multicore_topologies[] = {
    { "4-sockets-2-cores", 4, 2 },
    { "1-socket-8-cores", 1, 8 },
    { "2-sockets-4-cores", 2, 4 },
};

static void test_smp_multicore_topology(gconstpointer opaque)
{
    const TestSmpMulticoreTopology *topology = opaque;
    unsigned count = topology->sockets * topology->cores;
    g_autofree char *args = g_strdup_printf(
        "-smp %u,sockets=%u,cores=%u,threads=1",
        count, topology->sockets, topology->cores);
    QTestState *qts = ia64_vpc_start(args);
    g_autoptr(QDict) response = NULL;
    QList *cpus;

    assert_firmware_handoff(qts, 1, count, 0, topology->sockets,
                            topology->cores, 1);
    response = qtest_qmp(qts, "{'execute':'query-cpus-fast'}");
    g_assert(qdict_haskey(response, "return"));
    cpus = qdict_get_qlist(response, "return");
    g_assert_cmpuint(qlist_size(cpus), ==, count);
    qtest_quit(qts);
}

static void test_smp_rejects_full_alat(void)
{
    const char *argv[] = {
        qtest_qemu_binary(NULL),
        "-machine", "460gx,alat=full",
        "-smp", "2",
        "-display", "none",
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
                            "full ALAT emulation is not SMP-safe"));
}

static bool rtc_value_is_current(uint64_t value)
{
    int64_t now = time(NULL);

    return value >= now - 5 && value <= now + 5;
}

/* MC146818 CMOS RTC at legacy ports 0x70/0x71 in the I/O port space. */
static uint8_t rtc_cmos_read(QTestState *qts, uint8_t reg)
{
    qtest_writeb(qts, IA64_PCI_IO_BASE + 0x70, reg);
    return qtest_readb(qts, IA64_PCI_IO_BASE + 0x71);
}

static uint64_t rtc_cmos_field(uint8_t value, bool binary)
{
    return binary ? value : (uint64_t)(value >> 4) * 10 + (value & 0x0f);
}

static void test_rtc_aligned_read(void)
{
    QTestState *qts = ia64_vpc_start(NULL);
    uint8_t reg_b;
    bool binary;
    struct tm tm = { 0 };
    time_t guest;
    unsigned attempt;

    for (attempt = 0; attempt < 4; attempt++) {
        unsigned spin;
        uint64_t sec;

        for (spin = 0; spin < 1000; spin++) {
            if (!(rtc_cmos_read(qts, 0x0a) & 0x80)) {
                break;
            }
        }
        reg_b = rtc_cmos_read(qts, 0x0b);
        binary = (reg_b & 0x04) != 0;
        g_assert_true((reg_b & 0x02) != 0);   /* 24-hour mode */

        sec = rtc_cmos_field(rtc_cmos_read(qts, 0x00), binary);
        tm.tm_sec = sec;
        tm.tm_min = rtc_cmos_field(rtc_cmos_read(qts, 0x02), binary);
        tm.tm_hour = rtc_cmos_field(rtc_cmos_read(qts, 0x04), binary);
        tm.tm_mday = rtc_cmos_field(rtc_cmos_read(qts, 0x07), binary);
        tm.tm_mon = rtc_cmos_field(rtc_cmos_read(qts, 0x08), binary) - 1;
        tm.tm_year = rtc_cmos_field(rtc_cmos_read(qts, 0x09), binary);
        if (rtc_cmos_field(rtc_cmos_read(qts, 0x00), binary) != sec) {
            continue;                          /* ticked mid-read */
        }
        break;
    }
    tm.tm_year += tm.tm_year < 80 ? 100 : 0;   /* two-digit year pivot */

    guest = mktimegm(&tm);
    g_assert_true(rtc_value_is_current(guest));
    qtest_quit(qts);
}

static void test_nvram_commit_and_restart(void)
{
    const uint64_t test_value = 0x1122334455667788ULL;
    g_autofree char *tmpdir = NULL;
    g_autofree char *path = NULL;
    g_autofree char *quoted_path = NULL;
    g_autofree char *contents = NULL;
    g_autoptr(GError) error = NULL;
    gsize length = 0;
    QTestState *qts;

    tmpdir = g_dir_make_tmp("ia64-vpc-nvram-XXXXXX", &error);
    g_assert_no_error(error);
    g_assert_nonnull(tmpdir);
    path = g_build_filename(tmpdir, "nvram.bin", NULL);
    quoted_path = g_shell_quote(path);

    qts = qtest_initf("-machine 460gx,nvram=%s -m 256M -S",
                      quoted_path);
    qtest_writeq(qts, IA64_NVRAM_BASE, test_value);
    qtest_writeq(qts, IA64_NVRAM_BASE + IA64_NVRAM_COMMIT_OFFSET,
                 IA64_NVRAM_COMMIT_MAGIC);
    qtest_quit(qts);

    g_assert_true(g_file_get_contents(path, &contents, &length, &error));
    g_assert_no_error(error);
    g_assert_cmpuint(length, ==, IA64_NVRAM_SIZE);
    g_assert_cmphex(ldq_le_p(contents), ==, test_value);

    qts = qtest_initf("-machine 460gx,nvram=%s -m 256M -S",
                      quoted_path);
    g_assert_cmphex(qtest_readq(qts, IA64_NVRAM_BASE), ==, test_value);
    qtest_quit(qts);

    g_assert_cmpint(g_unlink(path), ==, 0);
    g_assert_cmpint(g_rmdir(tmpdir), ==, 0);
}

/*
 * realfw mode (phase 5): a synthetic 128 KiB flash image with the
 * architected reset pointer block must be mapped ending at 4 GiB, and the
 * machine-planted PAL stub must appear at its fixed home.  The image places
 * a _FIT_ header and points SALE_ENTRY at an arbitrary bundle inside the
 * image; qtest never runs the CPU, so mapping and content are the contract
 * under test.
 */
/*
 * In realfw mode the 460GX chipset answers CF8/CFC configuration cycles for
 * its own functions on bus CBN.  Each one must report its real identity:
 * a zero vendor id is neither "present" nor the architected "absent"
 * 0xffff, so firmware probing the chipset cannot tell what it found.
 */
#define IA64_REALFW_CF8         0x0cf8U
#define IA64_REALFW_CFC         0x0cfcU

static uint32_t realfw_cfg_readl(QTestState *qts, uint8_t dev, uint8_t fn,
                                 uint8_t reg)
{
    qtest_writel(qts, IA64_LEGACY_IO_BASE +
                 ia64_sparse_io_offset(IA64_REALFW_CF8),
                 0x80000000U | ((uint32_t)dev << 11) |
                 ((uint32_t)fn << 8) | reg);
    return qtest_readl(qts, IA64_LEGACY_IO_BASE +
                       ia64_sparse_io_offset(IA64_REALFW_CFC));
}

/*
 * The interrupt controller follows the platform.  On the i2000 it is the
 * 460GX Programmable Interrupt Device: 64 inputs reporting IOSAPIC version
 * 2.1, which is what gives each PCI root its own block of four INTx lines.
 * zx1 keeps the narrower controller it had.
 */
/*
 * The i2000 reaches its PCI buses through expander bridges: the PXB carries
 * the compatibility bus 0, the two WXBs carry buses 1 and 2, and the GXB
 * carries the AGP bus 3.  Each is a root in its own right, reachable through
 * the same segment-0 ECAM window by its own bus number, and each owns its own
 * block of four interrupt inputs instead of sharing bus 0's.
 */
static void test_460gx_expander_roots(void)
{
    QTestState *qts = qtest_init("-machine 460gx -cpu merced -m 256M -S "
                                 "-device i82559c,bus=wxb0,addr=1,romfile= "
                                 "-device i82559c,bus=wxb1,addr=1,romfile= "
                                 "-device i82559c,bus=gxb,addr=1,romfile=");
    unsigned int bus;

    for (bus = 1; bus <= 3; bus++) {
        uint64_t cfg = IA64_PCI_CONFIG_BASE + ((uint64_t)bus << 20) +
                       (1ULL << 15);

        g_assert_cmphex(qtest_readl(qts, cfg), ==, 0x12298086);
        /* An empty slot on the same root still decodes as open bus. */
        g_assert_cmphex(qtest_readl(qts, cfg + (1ULL << 15)), ==,
                        0xffffffff);
    }

    /* zx1 has no expander roots; its second root is Mercury's bus 0x10. */
    qtest_quit(qts);
    qts = qtest_init("-machine zx1 -m 256M -S");
    g_assert_cmphex(qtest_readl(qts, IA64_PCI_CONFIG_BASE + (1ULL << 20)),
                    ==, 0xffffffff);
    qtest_quit(qts);
}

/*
 * Every BAR must fall inside a producer window of the root the device sits
 * behind.  A device moved onto an expander root whose _CRS does not cover its
 * BARs gets no resources from the guest's PnP arbiter: when that device is the
 * boot controller, Windows bugchecks with STOP 0x7B long before the disk is
 * touched, and nothing in the firmware or the machine notices.  The window
 * table below mirrors roms/ia64-firmware/dsdt-pci-root.asl; the two must be
 * changed together.
 */
typedef struct {
    uint64_t first;
    uint64_t last;
} PCIWindow;

typedef struct {
    unsigned int bus;
    const PCIWindow *mem;
    size_t mem_count;
    const PCIWindow *io;
    size_t io_count;
} PCIRootWindows;

static const PCIWindow pci0_mem[] = { { 0xEE000000, 0xEFFFFFFF } };
static const PCIWindow pci0_io[] = {
    { 0x0000, 0x03AF }, { 0x03E0, 0xAFFF }, { 0xC000, 0xCFFF },
    { 0xF000, 0xFFFF },
};
static const PCIWindow wxb0_mem[] = { { 0xFA000000, 0xFBFFFFFF } };
static const PCIWindow wxb0_io[] = { { 0xB000, 0xBFFF } };
static const PCIWindow wxb1_mem[] = { { 0xFC000000, 0xFDFFFFFF } };
static const PCIWindow wxb1_io[] = { { 0xE000, 0xEFFF } };
static const PCIWindow gxb_mem[] = {
    { 0x000A0000, 0x000BFFFF }, { 0x000C0000, 0x000DFFFF },
    { 0xF0000000, 0xF9FFFFFF },
};
static const PCIWindow gxb_io[] = { { 0x03B0, 0x03DF }, { 0xD000, 0xDFFF } };

static const PCIRootWindows root_windows[] = {
    { 0, pci0_mem, G_N_ELEMENTS(pci0_mem), pci0_io, G_N_ELEMENTS(pci0_io) },
    { IA64_460GX_WXB0_BUS, wxb0_mem, G_N_ELEMENTS(wxb0_mem),
      wxb0_io, G_N_ELEMENTS(wxb0_io) },
    { IA64_460GX_WXB1_BUS, wxb1_mem, G_N_ELEMENTS(wxb1_mem),
      wxb1_io, G_N_ELEMENTS(wxb1_io) },
    { IA64_460GX_GXB_BUS, gxb_mem, G_N_ELEMENTS(gxb_mem),
      gxb_io, G_N_ELEMENTS(gxb_io) },
};

static bool pci_window_contains(const PCIWindow *windows, size_t count,
                                uint64_t address)
{
    size_t i;

    for (i = 0; i < count; i++) {
        if (address >= windows[i].first && address <= windows[i].last) {
            return true;
        }
    }
    return false;
}

static void check_root_window_containment(const char *args)
{
    QTestState *qts = qtest_init(args);
    size_t root;

    for (root = 0; root < G_N_ELEMENTS(root_windows); root++) {
        const PCIRootWindows *rw = &root_windows[root];
        unsigned int slot;

        for (slot = 0; slot < 32; slot++) {
            uint64_t cfg = IA64_PCI_CONFIG_BASE +
                           ((uint64_t)rw->bus << 20) +
                           ((uint64_t)slot << 15);
            unsigned int bar;

            if (qtest_readl(qts, cfg) == 0xffffffff) {
                continue;
            }

            for (bar = 0; bar < 6; bar++) {
                uint32_t value = qtest_readl(qts, cfg + 0x10 + bar * 4);
                uint64_t address;

                if (value == 0) {
                    continue;
                }
                if (value & PCI_BASE_ADDRESS_SPACE_IO) {
                    address = value & PCI_BASE_ADDRESS_IO_MASK;
                    g_assert_true(pci_window_contains(rw->io, rw->io_count,
                                                      address));
                    continue;
                }
                address = value & PCI_BASE_ADDRESS_MEM_MASK;
                g_assert_true(pci_window_contains(rw->mem, rw->mem_count,
                                                  address));
                if ((value & PCI_BASE_ADDRESS_MEM_TYPE_MASK) ==
                    PCI_BASE_ADDRESS_MEM_TYPE_64) {
                    bar++;
                }
            }
        }
    }

    qtest_quit(qts);
}

/*
 * The south bridge carries an 8259 pair, as the real 82468GX does, and the
 * processor reaches it through the interrupt-acknowledge byte in the
 * Processor Interrupt Block.  An IA-64 guest runs the SAPIC and never uses
 * it, but the real SDV firmware does, and the MADT already claims a
 * PC/AT-compatible interrupt space.  With the pair initialized and no
 * interrupt pending, the INTA cycle returns the master's spurious vector.
 *
 * The pair belongs to the bridge, not to the machine: it must answer without
 * realfw mode, and zx1 -- which models no south bridge yet -- must not
 * answer at all.
 */
/*
 * The platform devices that exist only in configuration space: the
 * Programmable Interrupt Device, whose function is the IOSAPIC every
 * interrupt is delivered through, and one Integrated Hot-Plug Controller per
 * WXB bus.  A guest enumerating the buses must find them where the i2000 has
 * them, with no BARs and no interrupt to arbitrate.
 */
/* The AGP bridge's own seat: the host bridge at the top of bus 0. */
#define IA64_AGP_SLOT           31U

/*
 * The machine presents the four PCI buses the i2000's expander bridges carry
 * and no fifth: the chipset's own configuration space -- the SAC, the SDC,
 * the memory cards and the expander ports -- stays firmware-facing.
 *
 * That is a decision, not an omission, and this pins it.  Nothing a guest
 * does reads those registers: an OS takes its resource map from ACPI, and
 * the one driver that touches 460GX chipset config, Linux's i460-agp, binds
 * to the AGP bridge by device ID, which the machine presents at 00:1f.0.
 * The software that does read them is the real SDV firmware, which has them
 * on the realfw path at the bus number it programs itself (CBN, 0xEE) -- a
 * number this machine could not present anyway, because a config address
 * for it falls outside the 64 MB ECAM window and into the I/O block above.
 */
static void test_460gx_no_chipset_bus(void)
{
    QTestState *qts = qtest_init("-machine 460gx -cpu merced -m 256M -S");
    unsigned int bus;

    /* The four buses the expander bridges carry are all there... */
    static const struct {
        unsigned int bus;
        unsigned int slot;
    } occupied[] = {
        { 0, IA64_460GX_PID_SLOT },
        { IA64_460GX_WXB0_BUS, IA64_460GX_WXB0_SCSI_SLOT },
        { IA64_460GX_WXB1_BUS, IA64_460GX_IHPC_SLOT },
        { IA64_460GX_GXB_BUS, IA64_460GX_GXB_VGA_SLOT },
    };
    size_t i;

    for (i = 0; i < G_N_ELEMENTS(occupied); i++) {
        g_assert_cmphex(qtest_readl(qts, IA64_PCI_CONFIG_BASE +
                                    ((uint64_t)occupied[i].bus << 20) +
                                    ((uint64_t)occupied[i].slot << 15)),
                        !=, 0xffffffff);
    }
    /* ...and every bus above them is empty, chipset bus included. */
    for (bus = IA64_460GX_GXB_BUS + 1; bus < 0x40; bus++) {
        unsigned int slot;

        for (slot = 0; slot < 32; slot++) {
            g_assert_cmphex(qtest_readl(qts, IA64_PCI_CONFIG_BASE +
                                        ((uint64_t)bus << 20) +
                                        ((uint64_t)slot << 15)),
                            ==, 0xffffffff);
        }
    }

    /*
     * The AGP bridge the GART path binds to keeps its seat.  On the board it
     * is an expander port on the chipset bus; here it is the host bridge at
     * 00:1f.0, which is where Linux finds it ("Found an AGP 0.0 compliant
     * device at 0000:00:1f.0") and where >4 GB graphics DMA was validated.
     */
    g_assert_cmphex(qtest_readl(qts, IA64_PCI_CONFIG_BASE +
                                ((uint64_t)IA64_AGP_SLOT << 15)),
                    ==, 0x84ea8086);
    qtest_quit(qts);
}

static void test_460gx_platform_identities(void)
{
    QTestState *qts = qtest_init("-machine 460gx -cpu merced -m 256M -S");
    static const struct {
        unsigned int bus;
        unsigned int slot;
        uint16_t device;
        uint16_t class_id;
        uint8_t prog_if;
        uint16_t subsystem_vendor;
        uint16_t subsystem;
    } identities[] = {
        { 0, IA64_460GX_PID_SLOT, 0x123d, PCI_CLASS_SYSTEM_PIC, 0x20, 0, 0 },
        { IA64_460GX_WXB0_BUS, IA64_460GX_IHPC_SLOT, 0x123f,
          PCI_CLASS_SYSTEM_PCI_HOTPLUG, 0, PCI_VENDOR_ID_INTEL, 0x123f },
        { IA64_460GX_WXB1_BUS, IA64_460GX_IHPC_SLOT, 0x123f,
          PCI_CLASS_SYSTEM_PCI_HOTPLUG, 0, PCI_VENDOR_ID_INTEL, 0x123f },
    };
    size_t i;

    for (i = 0; i < G_N_ELEMENTS(identities); i++) {
        uint64_t cfg = IA64_PCI_CONFIG_BASE +
                       ((uint64_t)identities[i].bus << 20) +
                       ((uint64_t)identities[i].slot << 15);
        unsigned int bar;

        g_assert_cmphex(qtest_readl(qts, cfg), ==,
                        ((uint32_t)identities[i].device << 16) |
                        PCI_VENDOR_ID_INTEL);
        g_assert_cmphex(qtest_readw(qts, cfg + PCI_CLASS_DEVICE), ==,
                        identities[i].class_id);
        g_assert_cmphex(qtest_readb(qts, cfg + PCI_CLASS_PROG), ==,
                        identities[i].prog_if);
        g_assert_cmphex(qtest_readw(qts, cfg + PCI_SUBSYSTEM_VENDOR_ID),
                        ==, identities[i].subsystem_vendor);
        g_assert_cmphex(qtest_readw(qts, cfg + PCI_SUBSYSTEM_ID), ==,
                        identities[i].subsystem);
        g_assert_cmphex(qtest_readb(qts, cfg + PCI_INTERRUPT_PIN), ==, 0);
        for (bar = 0; bar < 6; bar++) {
            g_assert_cmphex(qtest_readl(qts, cfg + PCI_BASE_ADDRESS_0 +
                                        bar * 4), ==, 0);
        }
    }
    qtest_quit(qts);

    /* zx1 is a different platform and carries none of them. */
    qts = qtest_init("-machine zx1 -m 256M -S");
    g_assert_cmphex(qtest_readl(qts, IA64_PCI_CONFIG_BASE +
                                ((uint64_t)IA64_460GX_PID_SLOT << 15)),
                    ==, 0xffffffff);
    qtest_quit(qts);
}

static void test_460gx_south_bridge_pic(void)
{
    const uint64_t pic_cmd = IA64_LEGACY_IO_BASE + ia64_sparse_io_offset(0x20);
    const uint64_t pic_data = IA64_LEGACY_IO_BASE + ia64_sparse_io_offset(0x21);
    const uint64_t inta = 0xfefe0000ULL;
    QTestState *qts = qtest_init("-machine 460gx -cpu merced -m 256M -S");

    qtest_writeb(qts, pic_cmd, 0x11);   /* ICW1: cascade, ICW4 to follow */
    qtest_writeb(qts, pic_data, 0x08);  /* ICW2: interrupt vector base 8 */
    qtest_writeb(qts, pic_data, 0x04);  /* ICW3: slave cascaded on IR2 */
    qtest_writeb(qts, pic_data, 0x01);  /* ICW4: 8086 mode */
    g_assert_cmphex(qtest_readb(qts, inta), ==, 0x0f);
    qtest_quit(qts);

    qts = qtest_init("-machine zx1 -m 256M -S");
    g_assert_cmphex(qtest_readb(qts, inta), ==, 0x00);
    qtest_quit(qts);
}

/*
 * The bridge's timer unit and its NMI Status and Control register.  The IFB
 * carries three 82C54-equivalent counters (SSDM 15.4) -- counter 0 on IRQ 0,
 * counter 1 driving the DRAM refresh, counter 2 the speaker tone -- and Nmisc
 * at I/O 0x61 (SSDM 11.2.4.1), whose bits 3:0 are read/write and whose bits
 * 7, 5 and 4 are status.  Both existed only in realfw mode before; they are
 * platform hardware and belong to the bridge, so the machine must have them
 * with no flag, and zx1 -- which models no south bridge -- must not.
 */
#define IA64_PIT_COUNTER0       0x40
#define IA64_PIT_CONTROL        0x43
#define IA64_NMISC_PORT         0x61
#define IA64_NMISC_REFRESH      0x10
#define IA64_NMISC_TIMER2_OUT   0x20

static void test_460gx_south_bridge_timer(void)
{
    const uint64_t counter0 =
        IA64_LEGACY_IO_BASE + ia64_sparse_io_offset(IA64_PIT_COUNTER0);
    const uint64_t control =
        IA64_LEGACY_IO_BASE + ia64_sparse_io_offset(IA64_PIT_CONTROL);
    const uint64_t nmisc =
        IA64_LEGACY_IO_BASE + ia64_sparse_io_offset(IA64_NMISC_PORT);
    QTestState *qts = qtest_init("-machine 460gx -cpu merced -m 256M -S");
    uint8_t first;
    unsigned int i;

    /*
     * Program counter 0 for a periodic square wave and read the count back
     * through a counter-latch command: an unbacked port would answer 0xff to
     * both halves, which is not a count this counter can hold (it was loaded
     * with 0x1000 and only counts down).
     */
    qtest_writeb(qts, control, 0x36);           /* counter 0, LSB+MSB, mode 3 */
    qtest_writeb(qts, counter0, 0x00);
    qtest_writeb(qts, counter0, 0x10);          /* initial count 0x1000 */
    qtest_writeb(qts, control, 0x00);           /* latch counter 0 */
    {
        uint8_t lsb = qtest_readb(qts, counter0);
        uint8_t msb = qtest_readb(qts, counter0);

        g_assert_cmpuint((msb << 8) | lsb, <=, 0x1000);
    }

    /* Nmisc: bits 3:0 are stored, and the status bits above them are not. */
    qtest_writeb(qts, nmisc, 0x0f);
    g_assert_cmphex(qtest_readb(qts, nmisc) & 0x0f, ==, 0x0f);
    qtest_writeb(qts, nmisc, 0x00);
    g_assert_cmphex(qtest_readb(qts, nmisc) & 0x0f, ==, 0x00);
    /* Bit 7 (latched SERR#) and bit 6 (reserved) read zero. */
    g_assert_cmphex(qtest_readb(qts, nmisc) & 0xc0, ==, 0x00);

    /*
     * The refresh toggle must actually toggle: the vendor firmware times a
     * delay by waiting for a full 0->1->0 period, and a port stuck at either
     * value hangs it.  Clock the guest forward rather than spinning.
     */
    first = qtest_readb(qts, nmisc) & IA64_NMISC_REFRESH;
    for (i = 0; i < 100; i++) {
        qtest_clock_step(qts, 20000);
        if ((qtest_readb(qts, nmisc) & IA64_NMISC_REFRESH) != first) {
            break;
        }
    }
    g_assert_cmpuint(i, <, 100);

    /*
     * Bit 5 follows counter 2's output, and port 0x61 bit 0 is that counter's
     * gate -- the speaker path.  Run it in mode 3 with a short count and step
     * the clock: the output must be seen both high and low, which open bus
     * (stuck at 1) cannot do.
     */
    {
        const uint64_t counter2 =
            IA64_LEGACY_IO_BASE + ia64_sparse_io_offset(0x42);
        bool seen_high = false;
        bool seen_low = false;

        qtest_writeb(qts, control, 0xb6);       /* counter 2, LSB+MSB, mode 3 */
        qtest_writeb(qts, counter2, 0x20);
        qtest_writeb(qts, counter2, 0x00);      /* initial count 0x20 */
        qtest_writeb(qts, nmisc, 0x01);         /* gate the counter on */
        for (i = 0; i < 200; i++) {
            if (qtest_readb(qts, nmisc) & IA64_NMISC_TIMER2_OUT) {
                seen_high = true;
            } else {
                seen_low = true;
            }
            if (seen_high && seen_low) {
                break;
            }
            qtest_clock_step(qts, 10000);
        }
        g_assert_true(seen_high);
        g_assert_true(seen_low);
    }
    qtest_quit(qts);

    /* zx1 models no south bridge, so the port is unclaimed: open bus. */
    qts = qtest_init("-machine zx1 -m 256M -S");
    g_assert_cmphex(qtest_readb(qts, nmisc), ==, 0xff);
    qtest_quit(qts);
}

/*
 * The bridge's RTC is a 256-byte part in two 128-byte banks (SSDM 15.5.1).
 * Ports 0x70/0x71 reach the standard bank.  Ports 0x72/0x73 reach the
 * extended bank only while RTCCFG (function 0, config offset C8h) bit 2 is
 * set; with it clear -- the reset state, and where the vendor firmware leaves
 * it -- they alias the standard bank, which is what lets firmware write its
 * CMOS configuration through one pair and read it back through the other.
 * Bits 4 and 3 lock bytes 38h-3Fh of the extended and standard bank, once,
 * until a hardware reset.
 */
#define IA64_RTC_INDEX          0x70
#define IA64_RTC_DATA           0x71
#define IA64_RTC_EXT_INDEX      0x72
#define IA64_RTC_EXT_DATA       0x73
#define IA64_RTC_SCRATCH        0x40    /* user RAM, outside the lock range */
#define IA64_RTCCFG             0xc8
#define IA64_RTCCFG_UPPER_EN    0x04
#define IA64_RTCCFG_LOCK_UPPER  0x10

static void rtc_bank_write(QTestState *qts, uint16_t index_port,
                           uint8_t index, uint8_t value)
{
    qtest_writeb(qts, IA64_LEGACY_IO_BASE + ia64_sparse_io_offset(index_port),
                 index);
    qtest_writeb(qts,
                 IA64_LEGACY_IO_BASE + ia64_sparse_io_offset(index_port + 1),
                 value);
}

static uint8_t rtc_bank_read(QTestState *qts, uint16_t index_port,
                             uint8_t index)
{
    qtest_writeb(qts, IA64_LEGACY_IO_BASE + ia64_sparse_io_offset(index_port),
                 index);
    return qtest_readb(qts,
                       IA64_LEGACY_IO_BASE +
                       ia64_sparse_io_offset(index_port + 1));
}

static void test_460gx_south_bridge_rtc_banks(void)
{
    const uint64_t ifb_cfg = IA64_PCI_CONFIG_BASE +
                             ((uint64_t)IA64_460GX_IFB_SLOT << 15);
    QTestState *qts = qtest_init("-machine 460gx -cpu merced -m 256M -S");

    /* RTCCFG resets to zero, so 0x72/0x73 alias the standard bank. */
    g_assert_cmphex(qtest_readb(qts, ifb_cfg + IA64_RTCCFG), ==, 0x00);
    rtc_bank_write(qts, IA64_RTC_INDEX, IA64_RTC_SCRATCH, 0x5a);
    g_assert_cmphex(rtc_bank_read(qts, IA64_RTC_EXT_INDEX, IA64_RTC_SCRATCH),
                    ==, 0x5a);

    /* With Upper RAM Enable set they reach a bank of their own. */
    qtest_writeb(qts, ifb_cfg + IA64_RTCCFG, IA64_RTCCFG_UPPER_EN);
    g_assert_cmphex(qtest_readb(qts, ifb_cfg + IA64_RTCCFG) &
                    IA64_RTCCFG_UPPER_EN, ==, IA64_RTCCFG_UPPER_EN);
    rtc_bank_write(qts, IA64_RTC_EXT_INDEX, IA64_RTC_SCRATCH, 0xa5);
    g_assert_cmphex(rtc_bank_read(qts, IA64_RTC_EXT_INDEX, IA64_RTC_SCRATCH),
                    ==, 0xa5);
    /* The standard bank kept its own byte: these are two banks, not one. */
    g_assert_cmphex(rtc_bank_read(qts, IA64_RTC_INDEX, IA64_RTC_SCRATCH),
                    ==, 0x5a);

    /* Clearing the bit puts the alias back, and the standard byte reappears. */
    qtest_writeb(qts, ifb_cfg + IA64_RTCCFG, 0x00);
    g_assert_cmphex(rtc_bank_read(qts, IA64_RTC_EXT_INDEX, IA64_RTC_SCRATCH),
                    ==, 0x5a);

    /*
     * Lock Upper RAM Bytes: 38h-3Fh of the extended bank stop reading and
     * writing, and the bit is write-once -- clearing it must not unlock.
     */
    qtest_writeb(qts, ifb_cfg + IA64_RTCCFG, IA64_RTCCFG_UPPER_EN);
    rtc_bank_write(qts, IA64_RTC_EXT_INDEX, 0x38, 0x3c);
    g_assert_cmphex(rtc_bank_read(qts, IA64_RTC_EXT_INDEX, 0x38), ==, 0x3c);
    qtest_writeb(qts, ifb_cfg + IA64_RTCCFG,
                 IA64_RTCCFG_UPPER_EN | IA64_RTCCFG_LOCK_UPPER);
    g_assert_cmphex(rtc_bank_read(qts, IA64_RTC_EXT_INDEX, 0x38), ==, 0xff);
    rtc_bank_write(qts, IA64_RTC_EXT_INDEX, 0x38, 0x11);
    qtest_writeb(qts, ifb_cfg + IA64_RTCCFG, IA64_RTCCFG_UPPER_EN);
    g_assert_cmphex(qtest_readb(qts, ifb_cfg + IA64_RTCCFG) &
                    IA64_RTCCFG_LOCK_UPPER, ==, IA64_RTCCFG_LOCK_UPPER);
    /* Byte 40h is outside the locked range and still answers. */
    g_assert_cmphex(rtc_bank_read(qts, IA64_RTC_EXT_INDEX, IA64_RTC_SCRATCH),
                    ==, 0xa5);

    /*
     * The century byte at 32h is inside the standard bank's user RAM
     * (0Eh-7Fh, SSDM 15.5.1), so it is plain read/write storage.  The realfw
     * path used to model it as a read-only hardware register; that was a
     * deviation, and this pins the documented behaviour so it cannot come
     * back.
     */
    rtc_bank_write(qts, IA64_RTC_INDEX, 0x32, 0x19);
    g_assert_cmphex(rtc_bank_read(qts, IA64_RTC_INDEX, 0x32), ==, 0x19);
    rtc_bank_write(qts, IA64_RTC_INDEX, 0x32, 0x20);
    g_assert_cmphex(rtc_bank_read(qts, IA64_RTC_INDEX, 0x32), ==, 0x20);
    qtest_quit(qts);
}

/*
 * The board's hardware monitors on the bridge's SMBus.  The vendor firmware
 * initialises two chips (0x2C and 0x4E) through the bridge's PIIX4-layout
 * host controller during POST and polls each transaction to completion; an
 * empty bus fails them with a device error and the poll never exits, so the
 * monitors have to be there and have to acknowledge.  Drive one byte-data
 * write and read it back the way the firmware does.
 */
#define IA64_SMB_BASE_PORT      0xfff0
#define IA64_SMB_HSTSTS         0x00
#define IA64_SMB_HSTCNT         0x02
#define IA64_SMB_HSTCMD         0x03
#define IA64_SMB_HSTADD         0x04
#define IA64_SMB_HSTDAT0        0x05
#define IA64_SMB_CNT_BYTE_DATA  0x08
#define IA64_SMB_CNT_START      0x40
#define IA64_SMB_STS_HOST_BUSY  0x01
#define IA64_SMB_STS_INTR       0x02
#define IA64_SMB_STS_DEV_ERR    0x04

/*
 * Each group of four ports occupies its own 4 KB page of the sparse window,
 * so a register block's ports are not contiguous in memory: every register
 * has to be translated from its own port number.
 */
static uint64_t smb_reg(uint8_t reg)
{
    return IA64_LEGACY_IO_BASE +
           ia64_sparse_io_offset(IA64_SMB_BASE_PORT + reg);
}

static uint8_t smb_wait(QTestState *qts)
{
    const uint64_t sts = smb_reg(IA64_SMB_HSTSTS);
    uint8_t value = 0;
    unsigned int i;

    for (i = 0; i < 100; i++) {
        value = qtest_readb(qts, sts);
        if (!(value & IA64_SMB_STS_HOST_BUSY)) {
            break;
        }
    }
    return value;
}

static void smb_start(QTestState *qts, uint8_t address, uint8_t command,
                      uint8_t data, bool read)
{
    qtest_writeb(qts, smb_reg(IA64_SMB_HSTSTS), 0xff);   /* clear status */
    qtest_writeb(qts, smb_reg(IA64_SMB_HSTCMD), command);
    qtest_writeb(qts, smb_reg(IA64_SMB_HSTADD),
                 (address << 1) | (read ? 1 : 0));
    qtest_writeb(qts, smb_reg(IA64_SMB_HSTDAT0), data);
    qtest_writeb(qts, smb_reg(IA64_SMB_HSTCNT),
                 IA64_SMB_CNT_START | IA64_SMB_CNT_BYTE_DATA);
}

static void test_460gx_smbus_hwmon(void)
{
    QTestState *qts = qtest_init("-machine 460gx -cpu merced -m 256M -S");
    unsigned int i;

    /*
     * The firmware finds the controller at 0xFFF0 because the machine
     * programs the bridge's SMBus BAR there, as the real firmware does.
     * Registers the controller does not implement must read zero -- the
     * firmware uses offset 0x0e as a controller-enable check and spins if it
     * floats high.
     */
    g_assert_cmphex(qtest_readb(qts, smb_reg(0x0e)), ==, 0x00);

    /* Both monitors acknowledge: a completed transaction, no device error. */
    for (i = 0; i < 2; i++) {
        const uint8_t address = i == 0 ? 0x2c : 0x4e;

        smb_start(qts, address, 0x13, 0x5a, false);
        g_assert_cmphex(smb_wait(qts) & (IA64_SMB_STS_INTR |
                                         IA64_SMB_STS_DEV_ERR), ==,
                        IA64_SMB_STS_INTR);
    }

    /* The byte written is the byte read back from that register. */
    smb_start(qts, 0x2c, 0x13, 0x00, true);
    g_assert_cmphex(smb_wait(qts) & IA64_SMB_STS_DEV_ERR, ==, 0);
    g_assert_cmphex(qtest_readb(qts, smb_reg(IA64_SMB_HSTDAT0)), ==, 0x5a);

    /* An address with nothing on it reports a device error instead. */
    smb_start(qts, 0x55, 0x00, 0x00, false);
    g_assert_cmphex(smb_wait(qts) & IA64_SMB_STS_DEV_ERR, ==,
                    IA64_SMB_STS_DEV_ERR);
    qtest_quit(qts);
}

/*
 * The chipset answers PCI configuration cycles at CF8/CFC (460GX SSDM 2.3),
 * the mechanism the vendor firmware enumerates through.  It is real hardware
 * and the machine carries it whichever firmware runs.  Device numbers on the
 * bus CBN names are the chipset's own functions (Table 2-1); every other
 * address forwards to the PCI bus.  CBN resets to 0, so until firmware
 * programs it the chipset shadows the compatibility bus -- which is why our
 * firmware programs it, as the vendor firmware does.
 */
#define IA64_CF8_PORT   0xcf8
#define IA64_CFC_PORT   0xcfc
#define IA64_CBN_DEVICE 0x10
#define IA64_CBN_REG    0x40
#define IA64_CBN_BUS    0xee

static void cf8_select(QTestState *qts, uint8_t bus, uint8_t device,
                       uint8_t function, uint8_t reg)
{
    qtest_writel(qts, IA64_LEGACY_IO_BASE + ia64_sparse_io_offset(IA64_CF8_PORT),
                 0x80000000U | ((uint32_t)bus << 16) |
                 ((uint32_t)device << 11) | ((uint32_t)function << 8) |
                 (reg & 0xfc));
}

static uint32_t cf8_readl(QTestState *qts, uint8_t bus, uint8_t device,
                          uint8_t function, uint8_t reg)
{
    cf8_select(qts, bus, device, function, reg);
    return qtest_readl(qts, IA64_LEGACY_IO_BASE +
                       ia64_sparse_io_offset(IA64_CFC_PORT));
}

/*
 * The System Address Controller's register aperture and the platform's
 * diagnostic port.  The firmware arbitrates which processor boots through a
 * semaphore in the SAC block -- it clears bit 7, polls for bit 7, and
 * compares the low seven bits against its own LID.id -- so the block has to
 * answer, and the semaphore has to read granted to id 0.  Chipset hardware,
 * so the machine carries it whichever firmware runs; zx1 has neither.
 */
#define IA64_SAC_BASE      0x00000000feb00000ULL
#define IA64_SAC_BOOT_SEM  0xcc0
#define IA64_POST_PORT     0x80

static void test_460gx_sac_aperture(void)
{
    QTestState *qts = qtest_init("-machine 460gx -cpu merced -m 256M -S");
    const uint64_t post = IA64_LEGACY_IO_BASE +
        ia64_sparse_io_offset(IA64_POST_PORT);

    /* The boot semaphore reads granted (bit 7) to holder id 0. */
    g_assert_cmphex(qtest_readb(qts, IA64_SAC_BASE + IA64_SAC_BOOT_SEM), ==,
                    0x80);
    /* Clearing bit 7, as the firmware does, still reads back granted. */
    qtest_writeb(qts, IA64_SAC_BASE + IA64_SAC_BOOT_SEM, 0x00);
    g_assert_cmphex(qtest_readb(qts, IA64_SAC_BASE + IA64_SAC_BOOT_SEM), ==,
                    0x80);

    /* The rest of the aperture is read/write scratch, not open bus. */
    qtest_writel(qts, IA64_SAC_BASE + 0x100, 0xa55aa55a);
    g_assert_cmphex(qtest_readl(qts, IA64_SAC_BASE + 0x100), ==, 0xa55aa55a);
    g_assert_cmphex(qtest_readl(qts, IA64_SAC_BASE + 0x200), ==, 0);

    /* The diagnostic port latches the last code written to it. */
    qtest_writeb(qts, post, 0xc6);
    g_assert_cmphex(qtest_readb(qts, post), ==, 0xc6);
    qtest_quit(qts);

    qts = qtest_init("-machine zx1 -m 256M -S");
    g_assert_cmphex(qtest_readb(qts, IA64_SAC_BASE + IA64_SAC_BOOT_SEM), ==,
                    0x00);
    g_assert_cmphex(qtest_readb(qts, post), ==, 0xff);
    qtest_quit(qts);
}

static void test_460gx_config_ports(void)
{
    QTestState *qts = qtest_init("-machine 460gx -cpu merced -m 256M -S");

    /* Out of reset CBN is 0 and the chipset sits on the compatibility bus. */
    g_assert_cmphex(cf8_readl(qts, 0, 0x00, 0, PCI_VENDOR_ID), ==, 0x84e08086);

    /* Program CBN through the device reserved for it, as firmware does. */
    cf8_select(qts, 0, IA64_CBN_DEVICE, 0, IA64_CBN_REG);
    qtest_writeb(qts, IA64_LEGACY_IO_BASE + ia64_sparse_io_offset(IA64_CFC_PORT),
                 IA64_CBN_BUS);

    /* The chipset has moved: the SAC answers on CBN ... */
    g_assert_cmphex(cf8_readl(qts, IA64_CBN_BUS, 0x00, 0, PCI_VENDOR_ID), ==,
                    0x84e08086);
    g_assert_cmphex(cf8_readl(qts, IA64_CBN_BUS, 0x04, 0, PCI_VENDOR_ID), ==,
                    0x84e18086);

    /* ... and bus 0 now forwards to the machine's real devices. */
    g_assert_cmphex(cf8_readl(qts, 0, IA64_460GX_IFB_SLOT, 0, PCI_VENDOR_ID),
                    ==, 0x76008086);
    g_assert_cmphex(cf8_readl(qts, 0, IA64_460GX_PID_SLOT, 0, PCI_VENDOR_ID),
                    ==, 0x123d8086);
    /* Devices behind the expander roots are reachable by bus number too. */
    g_assert_cmphex(cf8_readl(qts, IA64_460GX_WXB0_BUS,
                              IA64_460GX_WXB0_SCSI_SLOT, 0, PCI_VENDOR_ID),
                    ==, 0x12161077);

    /* An empty device number reads as absent, not as zero. */
    g_assert_cmphex(cf8_readl(qts, 0, 0x1d, 0, PCI_VENDOR_ID), ==, 0xffffffff);

    /*
     * Port 0xCF9 is the reset control, aliased with byte 1 of the config
     * address: a byte write with RST_CPU set resets the system, while the
     * dword writes software addresses the config register with do not.  The
     * reset re-seeds the chipset store, so CBN coming back as 0 -- the
     * chipset answering on the compatibility bus again -- is what shows the
     * machine went through reset.
     */
    qtest_writeb(qts, IA64_LEGACY_IO_BASE + ia64_sparse_io_offset(0xcf9),
                 0x06);
    g_assert_cmphex(cf8_readl(qts, 0, 0x00, 0, PCI_VENDOR_ID), ==, 0x84e08086);
    g_assert_cmphex(cf8_readl(qts, IA64_CBN_BUS, 0x00, 0, PCI_VENDOR_ID), ==,
                    0xffffffff);
    qtest_quit(qts);

    /* zx1 is a different chipset and answers no configuration cycles here. */
    qts = qtest_init("-machine zx1 -m 256M -S");
    g_assert_cmphex(cf8_readl(qts, 0, 0x00, 0, PCI_VENDOR_ID), ==, 0xffffffff);
    qtest_quit(qts);
}

static void test_460gx_root_window_containment(void)
{
    /*
     * Every optional device on, and each display adapter in turn: the
     * graphics BARs are the largest on the machine and the ones most likely
     * to grow past the GXB root's window.
     */
    check_root_window_containment("-machine 460gx,audio=on,lsi=on,ide=on "
                                  "-cpu merced -m 256M -S");
    check_root_window_containment("-machine 460gx,vga=mach64 "
                                  "-cpu merced -m 256M -S");
    check_root_window_containment("-machine 460gx,vga=nv15gl "
                                  "-cpu merced -m 256M -S");
}

static void test_iosapic_version_per_machine(void)
{
    QTestState *qts = qtest_init("-machine 460gx -cpu merced -m 256M -S");
    uint32_t version;

    version = iosapic_read(qts, 1);
    g_assert_cmphex(version & 0xff, ==, 0x21);
    g_assert_cmpuint((version >> 16) & 0xff, ==, 63);
    qtest_quit(qts);

    qts = qtest_init("-machine zx1 -m 256M -S");
    version = iosapic_read(qts, 1);
    g_assert_cmphex(version & 0xff, ==, 0x11);
    g_assert_cmpuint((version >> 16) & 0xff, ==, 23);
    qtest_quit(qts);
}

static void test_realfw_chipset_identity(void)
{
    g_autofree char *tmpdir = NULL;
    g_autofree char *path = NULL;
    g_autofree char *quoted_path = NULL;
    g_autofree uint8_t *image = NULL;
    const uint64_t image_size = 0x20000;
    const uint64_t base = 0x100000000ULL - image_size;
    const uint64_t fit_addr = base + 0x10000;
    const uint64_t sale_addr = base + 0x8000;
    g_autoptr(GError) error = NULL;
    QTestState *qts;

    tmpdir = g_dir_make_tmp("ia64-vpc-realfw-XXXXXX", &error);
    g_assert_no_error(error);
    path = g_build_filename(tmpdir, "flash.bin", NULL);
    quoted_path = g_shell_quote(path);
    image = g_malloc0(image_size);
    memset(image, 0xff, image_size);
    memcpy(image + (fit_addr - base), "_FIT_   ", 8);
    stq_le_p(image + (fit_addr - base) + 8, 0x0100000000000010ULL);
    stq_le_p(image + image_size - 32, (1ULL << 63) | fit_addr);
    stq_le_p(image + image_size - 24, (1ULL << 63) | sale_addr);
    g_assert_true(g_file_set_contents(path, (char *)image, image_size,
                                      &error));

    qts = qtest_initf("-machine 460gx,realfw=%s -m 256M -S", quoted_path);

    /* SAC at device 00h and 01h, SDC at 04h, Memory Card A at 05h. */
    g_assert_cmphex(realfw_cfg_readl(qts, 0x00, 0, PCI_VENDOR_ID), ==,
                    0x84e08086);
    g_assert_cmphex(realfw_cfg_readl(qts, 0x01, 0, PCI_VENDOR_ID), ==,
                    0x84e08086);
    g_assert_cmphex(realfw_cfg_readl(qts, 0x04, 0, PCI_VENDOR_ID), ==,
                    0x84e18086);
    g_assert_cmphex(realfw_cfg_readl(qts, 0x05, 0, PCI_VENDOR_ID), ==,
                    0x84e38086);

    /* Expander port 0: downstream SAC at function 0, the PXB at function 1. */
    g_assert_cmphex(realfw_cfg_readl(qts, 0x10, 0, PCI_VENDOR_ID), ==,
                    0x84e08086);
    g_assert_cmphex(realfw_cfg_readl(qts, 0x10, 1, PCI_VENDOR_ID), ==,
                    0x84cb8086);

    /* Class code and revision travel in the same dword. */
    g_assert_cmphex(realfw_cfg_readl(qts, 0x00, 0, PCI_REVISION_ID), ==,
                    0x06000003);
    g_assert_cmphex(realfw_cfg_readl(qts, 0x10, 1, PCI_REVISION_ID), ==,
                    0x06000005);

    /* Multifunction devices say so; single-function ones do not. */
    g_assert_cmphex(realfw_cfg_readl(qts, 0x00, 0, PCI_CACHE_LINE_SIZE) &
                    0x00800000, ==, 0x00800000);
    g_assert_cmphex(realfw_cfg_readl(qts, 0x04, 0, PCI_CACHE_LINE_SIZE) &
                    0x00800000, ==, 0);

    /*
     * A device number the chipset does not claim forwards to the PCI bus:
     * the south bridge the firmware scans bus 0 for answers from the real
     * 82468GX at 00:03.0, not from a shadow in the config store.  Its
     * frequency mailbox at register D0h reads back with the done flag set,
     * which is what lets the firmware's frequency detection finish instead
     * of rebooting through 0xCF9 forever (plans/phase5 SESSION 17).
     */
    g_assert_cmphex(realfw_cfg_readl(qts, IA64_460GX_IFB_SLOT, 0,
                                     PCI_VENDOR_ID), ==, 0x76008086);
    g_assert_cmphex(realfw_cfg_readl(qts, IA64_460GX_IFB_SLOT, 0, 0xd0) &
                    0x8000, ==, 0x8000);

    qtest_quit(qts);
    g_assert_cmpint(g_unlink(path), ==, 0);
    g_assert_cmpint(g_rmdir(tmpdir), ==, 0);
}

static void test_realfw_flash_window(void)
{
    g_autofree char *tmpdir = NULL;
    g_autofree char *path = NULL;
    g_autofree char *quoted_path = NULL;
    g_autofree uint8_t *image = NULL;
    const uint64_t image_size = 0x20000;
    const uint64_t base = 0x100000000ULL - image_size;
    const uint64_t fit_addr = base + 0x10000;
    const uint64_t sale_addr = base + 0x8000;
    g_autoptr(GError) error = NULL;
    QTestState *qts;

    tmpdir = g_dir_make_tmp("ia64-vpc-realfw-XXXXXX", &error);
    g_assert_no_error(error);
    path = g_build_filename(tmpdir, "flash.bin", NULL);
    quoted_path = g_shell_quote(path);

    image = g_malloc0(image_size);
    memset(image, 0xff, image_size);
    memcpy(image + (fit_addr - base), "_FIT_   ", 8);
    stq_le_p(image + (fit_addr - base) + 8, 0x0100000000000010ULL);
    stq_le_p(image + image_size - 32, (1ULL << 63) | fit_addr);
    stq_le_p(image + image_size - 24, (1ULL << 63) | sale_addr);
    stq_le_p(image + (sale_addr - base), 0x0123456789abcdefULL);
    g_assert_true(g_file_set_contents(path, (char *)image, image_size,
                                      &error));

    qts = qtest_initf("-machine 460gx,realfw=%s -m 256M -S", quoted_path);
    /* Flash content is visible at its physical home. */
    g_assert_cmphex(qtest_readq(qts, sale_addr), ==, 0x0123456789abcdefULL);
    g_assert_cmphex(qtest_readq(qts, fit_addr) & 0xffffffffffffULL, ==,
                    0x5f5449465fULL | ((uint64_t)' ' << 40));
    /* Reset pointer block at 4 GiB-32/-24. */
    g_assert_cmphex(qtest_readq(qts, 0x100000000ULL - 24), ==,
                    (1ULL << 63) | sale_addr);
    /* The PAL stub's break bundle sits at its fixed home. */
    g_assert_cmphex(qtest_readq(qts, 0xff100000ULL), !=, 0);

    /*
     * The flash is a writable Intel-CFI part.  Read Device ID (0x90) exposes
     * the JEDEC identity the SDV firmware checks: manufacturer 0x89 at byte
     * offset 0, device 0xac (82802AB Firmware Hub) at byte offset 1.
     */
    qtest_writeb(qts, base, 0x90);
    g_assert_cmphex(qtest_readb(qts, base + 0), ==, 0x89);
    g_assert_cmphex(qtest_readb(qts, base + 1), ==, 0xac);

    /*
     * Clear Status (0x50) followed by Read Status (0x70) must leave the
     * WSM-ready bit (0x80) set on an idle part; the firmware treats a part
     * that reads back "not ready" here as dead.
     */
    qtest_writeb(qts, base, 0x50);
    qtest_writeb(qts, base, 0x70);
    g_assert_cmphex(qtest_readb(qts, base) & 0x80, ==, 0x80);

    /* Back to Read Array (0xff): the stored content is intact. */
    qtest_writeb(qts, base, 0xff);
    g_assert_cmphex(qtest_readq(qts, sale_addr), ==, 0x0123456789abcdefULL);

    /*
     * Block lock configuration (0x91).  The command goes to a block's base
     * address and the write that follows carries that block's lock bits at
     * base + 2; the SDV firmware sweeps every block this way to unlock the
     * part before programming it.  The pair has to be absorbed as one
     * two-cycle command -- decoded separately, the second write would be
     * read as a fresh command -- and the part is left in read-array mode.
     * Nothing is locked in this model, so the read-back reports zero.
     */
    qtest_writeb(qts, base, 0x91);
    g_assert_cmphex(qtest_readb(qts, base + 2), ==, 0x00);
    qtest_writeb(qts, base + 2, 0x00);
    g_assert_cmphex(qtest_readq(qts, sale_addr), ==, 0x0123456789abcdefULL);

    /*
     * The south bridge's IDE function is in compatibility mode and decodes
     * the fixed legacy ports the SDV firmware polls.  With no media the
     * empty primary channel reports status 0x00 (BSY clear, no drive) at
     * port 0x1f7 -- not the open-bus 0xff that would hang the firmware's
     * drive detection.
     */
    g_assert_cmphex(qtest_readb(qts, IA64_LEGACY_IO_BASE +
                                ia64_sparse_io_offset(0x1f7)), ==, 0x00);

    /*
     * realfw mode wires an 8259 PIC for the legacy timer tick, reachable
     * through the ExtINT interrupt-acknowledge byte in the Processor
     * Interrupt Block (lsapic base + 0x1e0000).  Initialize the PIC with
     * vector base 8, as the firmware does; with no IRQ pending the INTA cycle
     * returns the master's spurious vector (base | 7 = 0x0f), confirming the
     * ack byte drives the 8259.  The live IRQ-0 -> vector-8 -> ExtINT path is
     * exercised by booting the real firmware.
     */
    {
        const uint64_t pic_cmd =
            IA64_LEGACY_IO_BASE + ia64_sparse_io_offset(0x20);
        const uint64_t pic_data =
            IA64_LEGACY_IO_BASE + ia64_sparse_io_offset(0x21);
        const uint64_t inta = 0xfefe0000ULL;

        qtest_writeb(qts, pic_cmd, 0x11);   /* ICW1: cascade, ICW4 to follow */
        qtest_writeb(qts, pic_data, 0x08);  /* ICW2: interrupt vector base 8 */
        qtest_writeb(qts, pic_data, 0x04);  /* ICW3: slave cascaded on IR2 */
        qtest_writeb(qts, pic_data, 0x01);  /* ICW4: 8086 mode */

        g_assert_cmphex(qtest_readb(qts, inta), ==, 0x0f);
    }

    qtest_quit(qts);

    g_assert_cmpint(g_unlink(path), ==, 0);
    g_assert_cmpint(g_rmdir(tmpdir), ==, 0);
}

/*
 * The generic host helper addresses one bus: it builds config addresses as
 * ecam_alloc_ptr + (devfn << 12) + offset.  Offsetting the base by the bus
 * number therefore points the whole QPCIBus at that bus, which is how the
 * tests reach devices behind the 460GX expander roots.
 */
static void ia64_qpci_init_on_bus(QGenericPCIBus *gbus, QTestState *qts,
                                  unsigned int bus)
{
    qpci_init_generic(gbus, qts, NULL, false);
    gbus->ecam_alloc_ptr = IA64_PCI_CONFIG_BASE + ((uint64_t)bus << 20);
    gbus->gpex_pio_base = IA64_LEGACY_IO_BASE;
}

static void ia64_qpci_init(QGenericPCIBus *gbus, QTestState *qts)
{
    ia64_qpci_init_on_bus(gbus, qts, 0);
}

static void assert_pci_device(QPCIBus *bus, const ExpectedPCIDevice *expected)
{
    QPCIDevice *dev = qpci_device_find(bus,
                                       QPCI_DEVFN(expected->slot, 0));
    unsigned bar;

    g_assert_nonnull(dev);
    g_assert_cmphex(qpci_config_readw(dev, PCI_VENDOR_ID), ==,
                    expected->vendor);
    g_assert_cmphex(qpci_config_readw(dev, PCI_DEVICE_ID), ==,
                    expected->device);
    g_assert_cmphex(qpci_config_readw(dev, PCI_COMMAND), ==,
                    expected->command);
    g_assert_cmphex(qpci_config_readb(dev, PCI_INTERRUPT_LINE), ==,
                    expected->irq_line);
    g_assert_cmphex(qpci_config_readb(dev, PCI_INTERRUPT_PIN), ==,
                    expected->irq_pin);
    for (bar = 0; bar < ARRAY_SIZE(expected->bars); bar++) {
        g_assert_cmphex(qpci_config_readl(dev,
                                         PCI_BASE_ADDRESS_0 + bar * 4),
                        ==, expected->bars[bar]);
    }
    g_free(dev);
}

/*
 * ahci=off removes the AHCI controller at 0:1.0 without renumbering anything
 * else: an installed guest must not see the remaining devices move BDF.
 */
static void test_ahci_off(void)
{
    QTestState *qts = ia64_vpc_start("-machine ahci=off");
    QGenericPCIBus gbus;
    /*
     * Bus 0 after the i2000 relocation: the OHCI at 2, the south bridge at 3
     * and the 82559 at 5.  Graphics went to the GXB root and the SCSI HBA to
     * WXB0; device 4 is the audio slot, filled only with audio=on.
     */
    static const unsigned int kept_slots[] = { 2, 3, 5 };
    unsigned i;

    ia64_qpci_init(&gbus, qts);
    g_assert_null(qpci_device_find(&gbus.bus, QPCI_DEVFN(1, 0)));
    for (i = 0; i < ARRAY_SIZE(kept_slots); i++) {
        QPCIDevice *dev =
            qpci_device_find(&gbus.bus, QPCI_DEVFN(kept_slots[i], 0));

        g_assert_nonnull(dev);
        g_free(dev);
    }
    qtest_quit(qts);
}

/*
 * AHCI is opt-in (ahci=on): the controller then appears at 0:1.0 with its
 * fixed BARs and INTx line.  It is absent from the default machine (see
 * test_ahci_off_default) because Windows IA-64 ships no SATA driver.
 */
static void test_ahci_on(void)
{
    static const ExpectedPCIDevice ahci_dev = {
        .slot = 1, .vendor = 0x8086, .device = 0x2922,
        .command = PCI_COMMAND_IO | PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER,
        .irq_line = 17, .irq_pin = 1,
        .bars = { [4] = 0x0000c101, [5] = 0xee020000 },
    };
    QTestState *qts = ia64_vpc_start("-machine ahci=on");
    QGenericPCIBus gbus;

    ia64_qpci_init(&gbus, qts);
    assert_pci_device(&gbus.bus, &ahci_dev);
    qtest_quit(qts);
}

/* The default machine has no AHCI controller: slot 1 is empty. */
static void test_ahci_off_default(void)
{
    QTestState *qts = ia64_vpc_start(NULL);
    QGenericPCIBus gbus;

    ia64_qpci_init(&gbus, qts);
    g_assert_null(qpci_device_find(&gbus.bus, QPCI_DEVFN(1, 0)));
    qtest_quit(qts);
}

static void test_pci_default_layout(void)
{
    static const ExpectedPCIDevice devices[] = {
        {
            .slot = 2, .vendor = 0x106b, .device = 0x003f,
            .command = PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER,
            .irq_line = 18, .irq_pin = 1,
            .bars = { [0] = 0xee010000 },
        },
        expected_i82557b,
    };
    /*
     * The south bridge is the 82468GX I/O and Firmware Bridge at 00:03: a
     * four-function device carrying the LPC/ISA bridge, the IDE controller,
     * the UHCI host controller and the SMBus controller.  Chipset parts have
     * no subsystem identity.  The UHCI keeps the I/O base and interrupt the
     * discrete PIIX3 function had, so nothing else in the map moves.  Both
     * IDE channels are in compatibility mode and decode the fixed legacy
     * ports, so that function places only its bus-master BAR and takes no
     * PCI interrupt: it uses ISA IRQs 14 and 15 through the bridge.
     */
    static const struct {
        unsigned int function;
        uint16_t device;
        uint16_t class_id;
        uint32_t bar4;
        uint8_t irq_line;
        uint8_t irq_pin;
    } ifb_functions[] = {
        { 0, 0x7600, PCI_CLASS_BRIDGE_ISA, 0, 0, 0 },
        { 1, 0x7601, PCI_CLASS_STORAGE_IDE, 0x0000c001, 0, 0 },
        { 2, 0x7602, PCI_CLASS_SERIAL_USB, 0x0000c121, 18, 4 },
        { 3, 0x7603, PCI_CLASS_SERIAL_SMBUS, 0x0000fff1, 16, 2 },
    };
    /*
     * The SCSI HBA is at 01:00.0 on the first WXB root -- the seat the board
     * gives its QLogic adapter, which is the machine's default.  Its
     * subsystem identity is part of the match ql12160.sys makes, so pin that
     * too, and it has no third BAR where the LSI kept its script RAM.  INTA
     * there is 16 + 1 * 4 + (0 + 0) % 4 = 20.
     */
    static const struct {
        uint32_t reg;
        uint32_t value;
    } wxb0_scsi[] = {
        { PCI_VENDOR_ID, 0x12161077 },
        { PCI_SUBSYSTEM_VENDOR_ID, 0x00071077 },
        { PCI_BASE_ADDRESS_0, 0x0000b001 },
        { PCI_BASE_ADDRESS_1, 0xfa000000 },
        { PCI_BASE_ADDRESS_2, 0x00000000 },
        { PCI_INTERRUPT_LINE, 0x00000114 },
    };
    /*
     * The graphics adapter is no longer on bus 0: it sits at 03:00.0 behind
     * the GXB expander, with the same fixed BARs and INTA on the GXB root's
     * own interrupt block (16 + 3 * 4 + (0 + 0) % 4 = 28).
     */
    static const struct {
        uint32_t reg;
        uint32_t value;
    } gxb_vga[] = {
        { PCI_VENDOR_ID, 0x50461002 },
        { PCI_BASE_ADDRESS_0, 0xf0000008 },
        { PCI_BASE_ADDRESS_1, 0x0000d801 },
        { PCI_BASE_ADDRESS_2, 0xf5000000 },
        { PCI_INTERRUPT_LINE, 0x0000011c },
    };
    QTestState *qts = ia64_vpc_start(NULL);
    QGenericPCIBus gbus;
    unsigned i;

    ia64_qpci_init(&gbus, qts);
    /*
     * Slot 0 holds the Programmable Interrupt Device's single function, and
     * slot 1 (AHCI, ahci=on) is empty by default.
     */
    for (i = 1; i < 8; i++) {
        QPCIDevice *empty = qpci_device_find(&gbus.bus, QPCI_DEVFN(0, i));

        g_assert_null(empty);
    }
    g_assert_null(qpci_device_find(&gbus.bus, QPCI_DEVFN(1, 0)));
    g_assert_null(qpci_device_find(&gbus.bus, QPCI_DEVFN(6, 0)));
    for (i = 0; i < ARRAY_SIZE(devices); i++) {
        assert_pci_device(&gbus.bus, &devices[i]);
    }
    for (i = 0; i < ARRAY_SIZE(gxb_vga); i++) {
        uint64_t cfg = IA64_PCI_CONFIG_BASE +
                       ((uint64_t)IA64_460GX_GXB_BUS << 20);

        g_assert_cmphex(qtest_readl(qts, cfg + gxb_vga[i].reg), ==,
                        gxb_vga[i].value);
    }
    for (i = 0; i < ARRAY_SIZE(wxb0_scsi); i++) {
        uint64_t cfg = IA64_PCI_CONFIG_BASE +
                       ((uint64_t)IA64_460GX_WXB0_BUS << 20);

        g_assert_cmphex(qtest_readl(qts, cfg + wxb0_scsi[i].reg), ==,
                        wxb0_scsi[i].value);
    }
    for (i = 0; i < G_N_ELEMENTS(ifb_functions); i++) {
        QPCIDevice *fn = qpci_device_find(
            &gbus.bus, QPCI_DEVFN(IA64_460GX_IFB_SLOT,
                                  ifb_functions[i].function));

        g_assert_nonnull(fn);
        g_assert_cmphex(qpci_config_readw(fn, PCI_VENDOR_ID), ==,
                        PCI_VENDOR_ID_INTEL);
        g_assert_cmphex(qpci_config_readw(fn, PCI_DEVICE_ID), ==,
                        ifb_functions[i].device);
        g_assert_cmphex(qpci_config_readw(fn, PCI_CLASS_DEVICE), ==,
                        ifb_functions[i].class_id);
        g_assert_cmphex(qpci_config_readw(fn, PCI_SUBSYSTEM_VENDOR_ID),
                        ==, 0);
        g_assert_cmphex(qpci_config_readw(fn, PCI_SUBSYSTEM_ID), ==, 0);
        g_assert_cmphex(qpci_config_readl(fn, PCI_BASE_ADDRESS_4), ==,
                        ifb_functions[i].bar4);
        g_assert_cmpuint(qpci_config_readb(fn, PCI_INTERRUPT_PIN), ==,
                         ifb_functions[i].irq_pin);
        g_assert_cmpuint(qpci_config_readb(fn, PCI_INTERRUPT_LINE), ==,
                         ifb_functions[i].irq_line);
        g_free(fn);
    }

    {
        QGenericPCIBus wxb0;
        QGenericPCIBus wxb1;
        QPCIDevice *scsi;

        ia64_qpci_init_on_bus(&wxb0, qts, IA64_460GX_WXB0_BUS);
        scsi = qpci_device_find(&wxb0.bus,
                                QPCI_DEVFN(IA64_460GX_WXB0_SCSI_SLOT, 0));
        g_assert_nonnull(scsi);
        g_assert_cmphex(qpci_config_readw(scsi, PCI_VENDOR_ID), ==,
                        0x1077);   /* QLogic */
        g_free(scsi);

        /* The second WXB root is the park, and nothing is parked by default. */
        ia64_qpci_init_on_bus(&wxb1, qts, IA64_460GX_WXB1_BUS);
        scsi = qpci_device_find(&wxb1.bus,
                                QPCI_DEVFN(IA64_460GX_WXB1_SCSI_SLOT, 0));
        g_assert_null(scsi);
    }
    assert_pci_device(&gbus.bus, &expected_i82557b);
    qtest_quit(qts);
}

static void test_e1000_resources_survive_reset(void)
{
    QTestState *qts = ia64_vpc_start("-nic user,model=e1000");
    QGenericPCIBus gbus;

    ia64_qpci_init(&gbus, qts);
    assert_pci_device(&gbus.bus, &expected_e1000);
    qtest_system_reset(qts);
    assert_pci_device(&gbus.bus, &expected_e1000);
    qtest_quit(qts);
}

static void test_e1000_intx_route(void)
{
    const uint8_t vector = 0x52;
    const uint32_t rte_low = IA64_IOSAPIC_RTE_BASE + IA64_E1000_GSI * 2;
    QTestState *qts = ia64_vpc_start("-nic user,model=e1000");

    iosapic_write(qts, rte_low, vector | IA64_IOSAPIC_RTE_LEVEL);
    qtest_writel(qts, IA64_E1000_MMIO_BASE + E1000_IMC, UINT32_MAX);
    (void)qtest_readl(qts, IA64_E1000_MMIO_BASE + E1000_ICR);
    qtest_writel(qts, IA64_E1000_MMIO_BASE + E1000_IMS,
                 E1000_IMS_TXDW);
    qtest_writel(qts, IA64_E1000_MMIO_BASE + E1000_ICS,
                 E1000_ICS_TXDW);
    g_assert_cmphex(iosapic_read(qts, rte_low) &
                    IA64_IOSAPIC_RTE_REMOTE_IRR, !=, 0);

    g_assert_cmphex(qtest_readl(qts, IA64_E1000_MMIO_BASE + E1000_ICR) &
                    E1000_ICR_TXDW, !=, 0);
    qtest_writel(qts, IA64_IOSAPIC_BASE + IA64_IOSAPIC_EOI, vector);
    g_assert_cmphex(iosapic_read(qts, rte_low) &
                    IA64_IOSAPIC_RTE_REMOTE_IRR, ==, 0);
    qtest_quit(qts);
}

static bool e1000_wait_tx_done(QTestState *qts, struct e1000_tx_desc *desc)
{
    int i;

    for (i = 0; i < IA64_E1000_TEST_TIMEOUT_MS; i++) {
        qtest_memread(qts, IA64_E1000_TX_DESC_ADDR, desc, sizeof(*desc));
        if (le32_to_cpu(desc->upper.data) & E1000_TXD_STAT_DD) {
            return true;
        }
        qtest_clock_step(qts, 1000);
        g_usleep(1000);
    }
    return false;
}

static bool e1000_wait_rx_done(QTestState *qts, struct e1000_rx_desc *desc)
{
    int i;

    for (i = 0; i < IA64_E1000_TEST_TIMEOUT_MS; i++) {
        qtest_memread(qts, IA64_E1000_RX_DESC_ADDR, desc, sizeof(*desc));
        if (desc->status & E1000_RXD_STAT_DD) {
            return true;
        }
        qtest_clock_step(qts, 1000);
        g_usleep(1000);
    }
    return false;
}

static bool socket_receive_all(int fd, void *buffer, size_t length)
{
    uint8_t *next = buffer;

    while (length != 0) {
        GPollFD poll_fd = {
            .fd = fd,
            .events = G_IO_IN,
        };
        ssize_t received;

        if (g_poll(&poll_fd, 1, IA64_E1000_TEST_TIMEOUT_MS) != 1 ||
            !(poll_fd.revents & G_IO_IN)) {
            return false;
        }
        received = recv(fd, next, length, 0);
        if (received <= 0) {
            return false;
        }
        next += received;
        length -= received;
    }
    return true;
}

static void test_e1000_packet_transfer(void)
{
    static const uint8_t packet[64] = {
        0x52, 0x54, 0x00, 0x12, 0x34, 0x56,
        0x52, 0x54, 0x00, 0x65, 0x43, 0x21,
        0x08, 0x00, 0x45, 0x00, 0x00, 0x32,
        0x12, 0x34, 0x00, 0x00, 0x40, 0x11,
        0x00, 0x00, 0x0a, 0x00, 0x02, 0x0f,
        0x0a, 0x00, 0x02, 0x02,
    };
    struct e1000_tx_desc tx_desc = { 0 };
    struct e1000_rx_desc rx_desc = { 0 };
    uint32_t frame_length;
    uint8_t received[sizeof(packet)];
    uint8_t rx_buffer[sizeof(packet)];
    g_autofree char *args = NULL;
    QTestState *qts;
    int sockets[2];

    g_assert_cmpint(qemu_socketpair(PF_UNIX, SOCK_STREAM, 0, sockets), ==, 0);
    qemu_clear_cloexec(sockets[1]);
    args = g_strdup_printf("-nic socket,fd=%d,model=e1000,"
                           "mac=52:54:00:12:34:56", sockets[1]);
    qts = qtest_initf("-machine 460gx -m 256M %s", args);
    close(sockets[1]);

    qtest_memwrite(qts, IA64_E1000_TX_BUFFER_ADDR, packet, sizeof(packet));
    tx_desc.buffer_addr = cpu_to_le64(IA64_E1000_TX_BUFFER_ADDR);
    tx_desc.lower.data = cpu_to_le32(sizeof(packet) | E1000_TXD_CMD_EOP |
                                    E1000_TXD_CMD_IFCS |
                                    E1000_TXD_CMD_RS);
    qtest_memwrite(qts, IA64_E1000_TX_DESC_ADDR,
                   &tx_desc, sizeof(tx_desc));
    qtest_writel(qts, IA64_E1000_MMIO_BASE + E1000_TDBAL,
                 IA64_E1000_TX_DESC_ADDR);
    qtest_writel(qts, IA64_E1000_MMIO_BASE + E1000_TDBAH, 0);
    qtest_writel(qts, IA64_E1000_MMIO_BASE + E1000_TDLEN,
                 IA64_E1000_RING_SIZE);
    qtest_writel(qts, IA64_E1000_MMIO_BASE + E1000_TDH, 0);
    qtest_writel(qts, IA64_E1000_MMIO_BASE + E1000_TCTL,
                 E1000_TCTL_EN | E1000_TCTL_PSP);
    qtest_writel(qts, IA64_E1000_MMIO_BASE + E1000_TDT, 1);

    g_assert_true(e1000_wait_tx_done(qts, &tx_desc));
    g_assert_true(socket_receive_all(sockets[0], &frame_length,
                                     sizeof(frame_length)));
    g_assert_cmpuint(ntohl(frame_length), ==, sizeof(packet));
    g_assert_true(socket_receive_all(sockets[0], received, sizeof(received)));
    g_assert_cmpmem(received, sizeof(received), packet, sizeof(packet));

    rx_desc.buffer_addr = cpu_to_le64(IA64_E1000_RX_BUFFER_ADDR);
    qtest_memwrite(qts, IA64_E1000_RX_DESC_ADDR,
                   &rx_desc, sizeof(rx_desc));
    qtest_writel(qts, IA64_E1000_MMIO_BASE + E1000_RDBAL,
                 IA64_E1000_RX_DESC_ADDR);
    qtest_writel(qts, IA64_E1000_MMIO_BASE + E1000_RDBAH, 0);
    qtest_writel(qts, IA64_E1000_MMIO_BASE + E1000_RDLEN,
                 IA64_E1000_RING_SIZE);
    qtest_writel(qts, IA64_E1000_MMIO_BASE + E1000_RDH, 0);
    qtest_writel(qts, IA64_E1000_MMIO_BASE + E1000_RCTL,
                 E1000_RCTL_EN | E1000_RCTL_UPE | E1000_RCTL_MPE |
                 E1000_RCTL_BAM | E1000_RCTL_SECRC);
    frame_length = htonl(sizeof(packet));
    g_assert_cmpint(qemu_write_full(sockets[0], &frame_length,
                                    sizeof(frame_length)), ==,
                    sizeof(frame_length));
    g_assert_cmpint(qemu_write_full(sockets[0], packet, sizeof(packet)), ==,
                    sizeof(packet));
    qtest_writel(qts, IA64_E1000_MMIO_BASE + E1000_RDT, 1);
    qtest_clock_step(qts, NANOSECONDS_PER_SECOND);
    g_assert_true(e1000_wait_rx_done(qts, &rx_desc));
    g_assert_cmpuint(le16_to_cpu(rx_desc.length), ==, sizeof(packet));
    qtest_memread(qts, IA64_E1000_RX_BUFFER_ADDR,
                  rx_buffer, sizeof(rx_buffer));
    g_assert_cmpmem(rx_buffer, sizeof(rx_buffer), packet, sizeof(packet));

    qtest_quit(qts);
    close(sockets[0]);
}

static void assert_cmd646_at_slot0(QTestState *qts)
{
    QGenericPCIBus gbus;
    QPCIDevice *dev;

    ia64_qpci_init(&gbus, qts);
    dev = qpci_device_find(&gbus.bus, QPCI_DEVFN(0, 0));
    g_assert_nonnull(dev);
    g_assert_cmphex(qpci_config_readw(dev, PCI_VENDOR_ID), ==, 0x1095);
    g_assert_cmphex(qpci_config_readw(dev, PCI_DEVICE_ID), ==, 0x0646);
    g_assert_cmphex(qpci_config_readw(dev, PCI_CLASS_DEVICE), ==,
                    PCI_CLASS_STORAGE_IDE);
    g_free(dev);
    qtest_quit(qts);
}

/*
 * A hand-attached CMD646 lands where it is told.  Slot 0 of the
 * compatibility bus is the Programmable Interrupt Device's seat on 460gx, so
 * this runs on zx1, where that slot is IDE's platform-anticipated home.
 */
static void test_pci_explicit_cmd646_slot0(void)
{
    assert_cmd646_at_slot0(qtest_initf(
        "-machine zx1 -m 256M -S "
        "-device cmd646-ide,secondary=1,addr=0,bus=pci"));
}

/*
 * On zx1 the ide=on machine option instantiates the same CMD646 at slot 0.
 * On 460gx it has nothing to do: the IDE controller is function 1 of the
 * south bridge, part of the board and not switchable, and slot 0 belongs to
 * the Programmable Interrupt Device, so the option is accepted without
 * effect.
 */
static void test_ide_on_slot0(void)
{
    QTestState *qts;
    QGenericPCIBus gbus;
    QPCIDevice *dev;

    assert_cmd646_at_slot0(qtest_initf("-machine zx1,ide=on -m 256M -S"));

    qts = ia64_vpc_start("-machine ide=on");
    ia64_qpci_init(&gbus, qts);
    dev = qpci_device_find(&gbus.bus, QPCI_DEVFN(IA64_460GX_PID_SLOT, 0));
    g_assert_nonnull(dev);
    g_assert_cmphex(qpci_config_readw(dev, PCI_DEVICE_ID), ==, 0x123d);
    g_free(dev);
    dev = qpci_device_find(&gbus.bus,
                           QPCI_DEVFN(IA64_460GX_IFB_SLOT,
                                      IA64_460GX_IFB_IDE_FUNCTION));
    g_assert_nonnull(dev);
    g_assert_cmphex(qpci_config_readw(dev, PCI_VENDOR_ID), ==,
                    PCI_VENDOR_ID_INTEL);
    g_assert_cmphex(qpci_config_readw(dev, PCI_DEVICE_ID), ==, 0x7601);
    g_free(dev);
    qtest_quit(qts);
}

static void lsi_write_script_insn(QTestState *qts, uint32_t *addr,
                                  uint32_t insn, uint32_t arg)
{
    qtest_writel(qts, *addr, insn);
    qtest_writel(qts, *addr + 4, arg);
    *addr += 8;
}

static bool lsi_run_nodata_command(QTestState *qts, const uint8_t *cdb,
                                   size_t cdb_len, uint8_t *status)
{
    const uint8_t identify = 0x80;
    uint32_t addr = IA64_LSI_SCRIPT_ADDR;
    uint8_t dstat = 0;
    unsigned int i;

    lsi_write_script_insn(qts, &addr, IA64_LSI_SCRIPT_SELECT, 0);
    lsi_write_script_insn(qts, &addr,
                          IA64_LSI_SCRIPT_MOVE(IA64_LSI_PHASE_MO, 1),
                          IA64_LSI_MSGOUT_ADDR);
    lsi_write_script_insn(qts, &addr,
                          IA64_LSI_SCRIPT_MOVE(IA64_LSI_PHASE_CMD,
                                               cdb_len),
                          IA64_LSI_CDB_ADDR);
    lsi_write_script_insn(qts, &addr,
                          IA64_LSI_SCRIPT_MOVE(IA64_LSI_PHASE_ST, 1),
                          IA64_LSI_STATUS_ADDR);
    lsi_write_script_insn(qts, &addr,
                          IA64_LSI_SCRIPT_MOVE(IA64_LSI_PHASE_MI, 1),
                          IA64_LSI_COMPLETE_ADDR);
    lsi_write_script_insn(qts, &addr, IA64_LSI_SCRIPT_DISCONNECT, 0);
    lsi_write_script_insn(qts, &addr, IA64_LSI_SCRIPT_INTERRUPT, 0);

    qtest_memwrite(qts, IA64_LSI_MSGOUT_ADDR, &identify, sizeof(identify));
    qtest_memwrite(qts, IA64_LSI_CDB_ADDR, cdb, cdb_len);
    qtest_writeb(qts, IA64_LSI_STATUS_ADDR, 0xff);
    qtest_writeb(qts, IA64_LSI_COMPLETE_ADDR, 0xff);

    qtest_readb(qts, IA64_LSI_MMIO_BASE + IA64_LSI_REG_DSTAT);
    qtest_readb(qts, IA64_LSI_MMIO_BASE + IA64_LSI_REG_SIST0);
    qtest_readb(qts, IA64_LSI_MMIO_BASE + IA64_LSI_REG_SIST1);
    qtest_writeb(qts, IA64_LSI_MMIO_BASE + IA64_LSI_REG_ISTAT0,
                 IA64_LSI_ISTAT0_INTF);
    qtest_writel(qts, IA64_LSI_MMIO_BASE + IA64_LSI_REG_DSP,
                 IA64_LSI_SCRIPT_ADDR);

    for (i = 0; i < 1000; i++) {
        if (qtest_readb(qts, IA64_LSI_MMIO_BASE + IA64_LSI_REG_ISTAT0) &
            IA64_LSI_ISTAT0_DIP) {
            dstat = qtest_readb(qts,
                                IA64_LSI_MMIO_BASE + IA64_LSI_REG_DSTAT);
            if (dstat & IA64_LSI_DSTAT_SIR) {
                break;
            }
        }
        g_usleep(1000);
    }

    *status = qtest_readb(qts, IA64_LSI_STATUS_ADDR);
    return (dstat & IA64_LSI_DSTAT_SIR) != 0;
}

static void test_lsi_async_nodata_command(void)
{
    const uint8_t test_unit_ready[6] = { 0 };
    const uint8_t synchronize_cache[10] = { 0x35 };
    QTestState *qts;
    uint8_t status;
    unsigned int i;

    qts = ia64_vpc_start_lsi(
        "-blockdev driver=null-co,read-zeroes=on,"
                  "node-name=disk0,size=1048576 "
        "-device scsi-hd,drive=disk0,bus=scsi.0,scsi-id=0");

    /* Consume the initial unit attention before testing async completion. */
    g_assert_true(lsi_run_nodata_command(qts, test_unit_ready,
                                         sizeof(test_unit_ready), &status));
    for (i = 0; i < 8; i++) {
        g_assert_true(lsi_run_nodata_command(qts, synchronize_cache,
                                             sizeof(synchronize_cache),
                                             &status));
        g_assert_cmpuint(status, ==, 0);
    }
    qtest_quit(qts);
}

/*
 * Regression for the lsi_dnad_addr stale-selector leak.  A non-zero Dynamic
 * Block Move Selector (DBMS) left over from an earlier 64-bit direct move must
 * NOT be OR-ed into bits [63:32] of a later plain (non-EN64DBMV) block move --
 * a plain move takes its upper address bits from the Static selector (SBMS).
 * Before the fix a stale DBMS leaked into every subsequent fetch/store,
 * including the SCSI CDB fetch in lsi_do_command(), pushing it to an unmapped
 * >4GB address; on IA-64 XP at 6GB (where above-4GB DMA sets DBMS) this
 * corrupted disk page-ins and bugchecked the guest (STOP 0xF4).
 */
#define IA64_LSI_REG_SBMS            0xb0
#define IA64_LSI_REG_DBMS            0xb4
static void test_lsi_dbms_no_leak(void)
{
    const uint8_t test_unit_ready[6] = { 0 };
    QTestState *qts;
    uint8_t status;

    qts = ia64_vpc_start_lsi(
        "-blockdev driver=null-co,read-zeroes=on,"
                  "node-name=disk0,size=1048576 "
        "-device scsi-hd,drive=disk0,bus=scsi.0,scsi-id=0");

    /* Consume the initial unit attention with a clean selector. */
    g_assert_true(lsi_run_nodata_command(qts, test_unit_ready,
                                         sizeof(test_unit_ready), &status));

    /*
     * Poison DBMS with a high-32 that, if leaked, moves every transfer 4GB up
     * into unmapped space; keep SBMS clear.  The next plain command must
     * consult SBMS (0), never this stale dynamic selector.
     */
    qtest_writel(qts, IA64_LSI_MMIO_BASE + IA64_LSI_REG_SBMS, 0x00000000);
    qtest_writel(qts, IA64_LSI_MMIO_BASE + IA64_LSI_REG_DBMS, 0x00000001);

    /*
     * With the leak the message-out / CDB / status moves all target >4GB
     * holes and the command cannot complete GOOD; the fix ignores DBMS, so
     * the command still selects, transfers and reports GOOD status.
     */
    g_assert_true(lsi_run_nodata_command(qts, test_unit_ready,
                                         sizeof(test_unit_ready), &status));
    g_assert_cmpuint(status, ==, 0);

    qtest_quit(qts);
}

/*
 * Regression for the lsi_memcpy 64-bit MOVE MEMORY fix.  A 64-bit memory move
 * takes bits [63:32] of the destination from the Memory Move Write Selector
 * (MMWS) and of the source from MMRS (LSI53C895A 4-103/4-104).  Before the fix
 * lsi_memcpy() was 32-bit and ignored them, so an above-4GB memory move was
 * truncated to the wrong page -- which broke the Linux sym53c8xx cache snoop
 * test (a MOVE MEMORY) on large-memory guests.
 */
#define IA64_LSI_REG_MMRS            0xa0
#define IA64_LSI_REG_MMWS            0xa4
static void test_lsi_memory_move_mmws(void)
{
    static const uint8_t src_pattern[16] = {
        0x5a, 0x5a, 0x5a, 0x5a, 0x5a, 0x5a, 0x5a, 0x5a,
        0x5a, 0x5a, 0x5a, 0x5a, 0x5a, 0x5a, 0x5a, 0x5a };
    const uint32_t src = IA64_LSI_MSGOUT_ADDR;   /* low source */
    const uint32_t dst = IA64_LSI_CDB_ADDR;      /* low destination */
    uint32_t addr = IA64_LSI_SCRIPT_ADDR;
    QTestState *qts;
    uint8_t dstat = 0;
    unsigned int i;

    qts = ia64_vpc_start_lsi(NULL);   /* a memory move needs no SCSI target */

    qtest_memwrite(qts, src, src_pattern, sizeof(src_pattern));
    qtest_memset(qts, dst, 0xaa, sizeof(src_pattern));

    /*
     * MMWS=1 must push the destination 4GB up, into an unmapped hole on this
     * 256M machine; MMRS=0 keeps the source low.  The low destination must
     * therefore stay untouched.  Without the fix MMWS is ignored and the low
     * destination is overwritten with the source pattern.
     */
    qtest_writel(qts, IA64_LSI_MMIO_BASE + IA64_LSI_REG_MMRS, 0x00000000);
    qtest_writel(qts, IA64_LSI_MMIO_BASE + IA64_LSI_REG_MMWS, 0x00000001);

    /* SCRIPT: MOVE MEMORY (3 dwords: insn, src, dst) then INTERRUPT. */
    qtest_writel(qts, addr, 0xc0000000u | sizeof(src_pattern)); addr += 4;
    qtest_writel(qts, addr, src); addr += 4;
    qtest_writel(qts, addr, dst); addr += 4;
    qtest_writel(qts, addr, IA64_LSI_SCRIPT_INTERRUPT); addr += 4;
    qtest_writel(qts, addr, 0); addr += 4;

    qtest_readb(qts, IA64_LSI_MMIO_BASE + IA64_LSI_REG_DSTAT);
    qtest_readb(qts, IA64_LSI_MMIO_BASE + IA64_LSI_REG_SIST0);
    qtest_readb(qts, IA64_LSI_MMIO_BASE + IA64_LSI_REG_SIST1);
    qtest_writel(qts, IA64_LSI_MMIO_BASE + IA64_LSI_REG_DSP,
                 IA64_LSI_SCRIPT_ADDR);

    for (i = 0; i < 1000; i++) {
        if (qtest_readb(qts, IA64_LSI_MMIO_BASE + IA64_LSI_REG_ISTAT0) &
            IA64_LSI_ISTAT0_DIP) {
            dstat = qtest_readb(qts,
                                IA64_LSI_MMIO_BASE + IA64_LSI_REG_DSTAT);
            if (dstat & IA64_LSI_DSTAT_SIR) {
                break;
            }
        }
        g_usleep(1000);
    }
    g_assert_true((dstat & IA64_LSI_DSTAT_SIR) != 0);

    for (i = 0; i < sizeof(src_pattern); i++) {
        g_assert_cmphex(qtest_readb(qts, dst + i), ==, 0xaa);
    }
    qtest_quit(qts);
}

static void iosapic_select(QTestState *qts, uint32_t reg)
{
    qtest_writel(qts, IA64_IOSAPIC_BASE + IA64_IOSAPIC_IOREGSEL, reg);
}

static uint32_t iosapic_read(QTestState *qts, uint32_t reg)
{
    iosapic_select(qts, reg);
    return qtest_readl(qts, IA64_IOSAPIC_BASE + IA64_IOSAPIC_IOWIN);
}

static void iosapic_write(QTestState *qts, uint32_t reg, uint32_t value)
{
    iosapic_select(qts, reg);
    qtest_writel(qts, IA64_IOSAPIC_BASE + IA64_IOSAPIC_IOWIN, value);
}

static char *find_unattached_child(QTestState *qts, const char *qom_type)
{
    g_autoptr(QDict) response = NULL;
    g_autofree char *child_type = g_strdup_printf("child<%s>", qom_type);
    QList *children;
    QListEntry *entry;

    response = qtest_qmp(qts,
                         "{'execute':'qom-list','arguments':"
                         " {'path':'/machine/unattached'}}");
    g_assert(qdict_haskey(response, "return"));
    children = qdict_get_qlist(response, "return");
    QLIST_FOREACH_ENTRY(children, entry) {
        QDict *child = qobject_to(QDict, qlist_entry_obj(entry));

        if (g_str_equal(qdict_get_str(child, "type"), child_type)) {
            return g_strdup_printf("/machine/unattached/%s",
                                   qdict_get_str(child, "name"));
        }
    }

    g_error("QOM child of type %s was not found", qom_type);
    return NULL;
}

static unsigned count_unattached_children(QTestState *qts,
                                          const char *qom_type)
{
    g_autoptr(QDict) response = NULL;
    g_autofree char *child_type = g_strdup_printf("child<%s>", qom_type);
    QList *children;
    QListEntry *entry;
    unsigned count = 0;

    response = qtest_qmp(qts,
                         "{'execute':'qom-list','arguments':"
                         " {'path':'/machine/unattached'}}");
    g_assert(qdict_haskey(response, "return"));
    children = qdict_get_qlist(response, "return");
    QLIST_FOREACH_ENTRY(children, entry) {
        QDict *child = qobject_to(QDict, qlist_entry_obj(entry));

        if (g_str_equal(qdict_get_str(child, "type"), child_type)) {
            count++;
        }
    }
    return count;
}

/*
 * PS/2 follows the platform.  The 460GX workstations carry a Super-I/O
 * keyboard controller and used it, so 460gx keeps PS/2 and adds no USB HID.
 * The zx1 generation dropped PS/2, so zx1 has no i8042 and gets the USB
 * keyboard and tablet instead; the firmware is told which it has.
 */
static void test_default_input_per_machine(void)
{
    QTestState *qts = qtest_init("-machine 460gx -cpu merced -m 256M -S");

    g_assert_cmpuint(count_unattached_children(qts, "i8042"), ==, 1);
    g_assert_cmpuint(count_unattached_children(qts, "usb-kbd"), ==, 0);
    g_assert_cmpuint(count_unattached_children(qts, "usb-tablet"), ==, 0);
    g_assert_cmpuint(read_handoff_i8042(qts), ==, 1);
    qtest_quit(qts);

    qts = qtest_init("-machine zx1 -m 256M -S");
    g_assert_cmpuint(count_unattached_children(qts, "i8042"), ==, 0);
    g_assert_cmpuint(count_unattached_children(qts, "usb-kbd"), ==, 1);
    g_assert_cmpuint(count_unattached_children(qts, "usb-tablet"), ==, 1);
    g_assert_cmpuint(read_handoff_i8042(qts), ==, 0);
    qtest_quit(qts);

    /* Either default can still be overridden. */
    qts = qtest_init("-machine zx1,i8042=on -m 256M -S");
    g_assert_cmpuint(count_unattached_children(qts, "i8042"), ==, 1);
    g_assert_cmpuint(count_unattached_children(qts, "usb-kbd"), ==, 0);
    g_assert_cmpuint(read_handoff_i8042(qts), ==, 1);
    qtest_quit(qts);
}

static void test_default_usb_input(void)
{
    QTestState *qts = qtest_init("-machine 460gx,i8042=off "
                                 "-m 256M -S");

    g_assert_cmpuint(count_unattached_children(qts, "usb-kbd"), ==, 1);
    g_assert_cmpuint(count_unattached_children(qts, "usb-tablet"), ==, 1);
    g_assert_cmpuint(count_unattached_children(qts, "usb-mouse"), ==, 0);
    qtest_quit(qts);
}

static void test_iosapic_level_remote_irr(void)
{
    const unsigned pin = 23;
    const uint8_t vector = 0x51;
    const uint32_t rte_low = IA64_IOSAPIC_RTE_BASE + pin * 2;
    QTestState *qts = ia64_vpc_start("-nic user,model=e1000");
    g_autofree char *iosapic_path =
        find_unattached_child(qts, "ia64-iosapic");
    uint32_t rte;

    /* Delivery status and Remote IRR are read-only guest-visible bits. */
    iosapic_write(qts, rte_low,
                  vector | IA64_IOSAPIC_RTE_LEVEL |
                  IA64_IOSAPIC_RTE_DELIVERY |
                  IA64_IOSAPIC_RTE_REMOTE_IRR);
    rte = iosapic_read(qts, rte_low);
    g_assert_cmphex(rte & (IA64_IOSAPIC_RTE_DELIVERY |
                          IA64_IOSAPIC_RTE_REMOTE_IRR), ==, 0);

    qtest_set_irq_in(qts, iosapic_path, NULL, pin, 1);
    rte = iosapic_read(qts, rte_low);
    g_assert_cmphex(rte & IA64_IOSAPIC_RTE_REMOTE_IRR, !=, 0);
    g_assert_cmphex(rte & IA64_IOSAPIC_RTE_DELIVERY, ==, 0);

    /* EOI while the level remains asserted immediately redelivers it. */
    qtest_writel(qts, IA64_IOSAPIC_BASE + IA64_IOSAPIC_EOI, vector);
    g_assert_cmphex(iosapic_read(qts, rte_low) &
                    IA64_IOSAPIC_RTE_REMOTE_IRR, !=, 0);

    qtest_set_irq_in(qts, iosapic_path, NULL, pin, 0);
    g_assert_cmphex(iosapic_read(qts, rte_low) &
                    IA64_IOSAPIC_RTE_REMOTE_IRR, !=, 0);
    qtest_writel(qts, IA64_IOSAPIC_BASE + IA64_IOSAPIC_EOI, vector);
    g_assert_cmphex(iosapic_read(qts, rte_low) &
                    IA64_IOSAPIC_RTE_REMOTE_IRR, ==, 0);
    qtest_quit(qts);
}

static uint64_t cpu_sapic_irr_word(QTestState *qts, unsigned word)
{
    g_autofree char *out = qtest_hmp(qts, "info registers");
    char *line = strstr(out, "SAPIC IRR:");
    uint64_t w[4];

    g_assert_nonnull(line);
    g_assert_cmpint(sscanf(line, "SAPIC IRR: %" SCNx64 " %" SCNx64
                           " %" SCNx64 " %" SCNx64,
                           &w[0], &w[1], &w[2], &w[3]), ==, 4);
    g_assert_cmpuint(word, <, 4);
    return w[word];
}

/*
 * SAPIC delivery from the I/O SAPIC is queued to the target vCPU with
 * async_run_on_cpu(), so an immediate IRR readback races the (idle)
 * qtest vCPU thread draining its work queue.  Pulse a disambiguating
 * fence vector through a spare pin and wait for it to appear: the
 * per-CPU work queue is FIFO, so once the fence delivery is visible,
 * every delivery requested before it -- including an erroneous one --
 * is visible too, and both the ==0 and !=0 assertions below are exact.
 * IRR bits are never accepted (no code runs on a qtest vCPU), so each
 * fence needs a vector of its own.
 */
static void iosapic_irr_fence(QTestState *qts, const char *iosapic_path,
                              uint8_t fence_vector)
{
    const unsigned pin = 20;
    const uint32_t rte_low = IA64_IOSAPIC_RTE_BASE + pin * 2;
    const unsigned word = fence_vector / 64;
    const uint64_t bit = 1ULL << (fence_vector % 64);
    gint64 deadline = g_get_monotonic_time() + 15 * G_USEC_PER_SEC;

    iosapic_write(qts, rte_low, fence_vector);
    qtest_set_irq_in(qts, iosapic_path, NULL, pin, 0);
    qtest_set_irq_in(qts, iosapic_path, NULL, pin, 1);
    while (!(cpu_sapic_irr_word(qts, word) & bit)) {
        g_assert_cmpint(g_get_monotonic_time(), <, deadline);
        g_usleep(1000);
    }
}

/*
 * A redirection-table write is not an interrupt request, and a redundant
 * assert of an already-high input is not a new edge: an edge-triggered
 * entry delivers only on a 0->1 input transition (460GX SSDM 248704-001
 * sec 2.6.2).  Windows rewrites RTEs continually during PnP enumeration,
 * so delivering on RTE writes injected interrupts no device had raised.
 */
static void test_iosapic_edge_rte_write_is_not_a_request(void)
{
    const unsigned pin = 21;
    const uint8_t vector = 0x53;
    const uint32_t rte_low = IA64_IOSAPIC_RTE_BASE + pin * 2;
    const unsigned word = vector / 64;
    const uint64_t bit = 1ULL << (vector % 64);
    QTestState *qts = ia64_vpc_start(NULL);
    g_autofree char *iosapic_path =
        find_unattached_child(qts, "ia64-iosapic");

    /* Input already asserted before the route is programmed. */
    qtest_set_irq_in(qts, iosapic_path, NULL, pin, 1);
    iosapic_write(qts, rte_low, vector);
    iosapic_irr_fence(qts, iosapic_path, 0x60);
    g_assert_cmphex(cpu_sapic_irr_word(qts, word) & bit, ==, 0);

    /* Rewriting the entry is not a request either. */
    iosapic_write(qts, rte_low, vector);
    iosapic_irr_fence(qts, iosapic_path, 0x61);
    g_assert_cmphex(cpu_sapic_irr_word(qts, word) & bit, ==, 0);

    /* Nor is a redundant assert of the already-high input. */
    qtest_set_irq_in(qts, iosapic_path, NULL, pin, 1);
    iosapic_irr_fence(qts, iosapic_path, 0x62);
    g_assert_cmphex(cpu_sapic_irr_word(qts, word) & bit, ==, 0);

    /* The 0->1 transition is. */
    qtest_set_irq_in(qts, iosapic_path, NULL, pin, 0);
    qtest_set_irq_in(qts, iosapic_path, NULL, pin, 1);
    iosapic_irr_fence(qts, iosapic_path, 0x63);
    g_assert_cmphex(cpu_sapic_irr_word(qts, word) & bit, !=, 0);
    qtest_quit(qts);
}

static void test_iosapic_lowest_priority(void)
{
    const unsigned pin = 22;
    const uint8_t vector = 0x52;
    const uint32_t rte_low = IA64_IOSAPIC_RTE_BASE + pin * 2;
    QTestState *qts = ia64_vpc_start(NULL);
    g_autofree char *iosapic_path =
        find_unattached_child(qts, "ia64-iosapic");

    iosapic_write(qts, rte_low,
                  vector | IA64_IOSAPIC_RTE_LOWEST |
                  IA64_IOSAPIC_RTE_LEVEL);
    qtest_set_irq_in(qts, iosapic_path, NULL, pin, 1);
    g_assert_cmphex(iosapic_read(qts, rte_low) &
                    IA64_IOSAPIC_RTE_REMOTE_IRR, !=, 0);

    qtest_set_irq_in(qts, iosapic_path, NULL, pin, 0);
    qtest_writel(qts, IA64_IOSAPIC_BASE + IA64_IOSAPIC_EOI, vector);
    qtest_quit(qts);
}

static void test_sparse_io_pm_register(void)
{
    const uint32_t port = IA64_ACPI_PM_IO_BASE + IA64_ACPI_PM1_CNT_OFFSET;
    const uint64_t dense = IA64_LEGACY_IO_BASE + port;
    const uint64_t sparse = IA64_LEGACY_IO_BASE +
                            ia64_sparse_io_offset(port);
    QTestState *qts = ia64_vpc_start(NULL);

    g_assert_cmphex(sparse, ==, 0x00000ffffc801004ULL);

    qtest_writew(qts, dense, 0);
    g_assert_cmphex(qtest_readw(qts, sparse) & 1, ==, 0);

    qtest_writew(qts, sparse, 1);
    g_assert_cmphex(qtest_readw(qts, sparse) & 1, ==, 1);
    g_assert_cmphex(qtest_readw(qts, dense) & 1, ==, 1);

    qtest_writew(qts, sparse, 0);
    g_assert_cmphex(qtest_readw(qts, dense) & 1, ==, 0);
    qtest_quit(qts);
}

static void test_openbus_io_port(void)
{
    /*
     * A legacy I/O port that no device claims floats the bus high: a byte
     * read returns 0xff, not 0x00.  Real SDV firmware byte-reads a Super I/O
     * device-ID register at port 0x2f (and the 0x2e index alongside it) and
     * requires 0xff for an absent chip.  A port a device does answer keeps
     * returning its own value.  (Sparse I/O maps consecutive ports to
     * non-consecutive addresses, so only single-port byte reads are probed.)
     */
    const uint64_t sio2f = IA64_LEGACY_IO_BASE + ia64_sparse_io_offset(0x2f);
    const uint64_t sio2e = IA64_LEGACY_IO_BASE + ia64_sparse_io_offset(0x2e);
    const uint32_t pm_port = IA64_ACPI_PM_IO_BASE + IA64_ACPI_PM1_CNT_OFFSET;
    const uint64_t pm = IA64_LEGACY_IO_BASE + ia64_sparse_io_offset(pm_port);
    QTestState *qts = ia64_vpc_start(NULL);

    g_assert_cmphex(qtest_readb(qts, sio2f), ==, 0xff);
    g_assert_cmphex(qtest_readb(qts, sio2e), ==, 0xff);

    /* A claimed port answers its own value, never the open-bus 0xff. */
    qtest_writew(qts, pm, 0);
    g_assert_cmphex(qtest_readb(qts, pm), !=, 0xff);

    qtest_quit(qts);
}

static bool sapic_irr_has_vector(QTestState *qts, uint8_t vector)
{
    g_autofree char *registers = qtest_hmp(qts, "info registers");
    const char *line = strstr(registers, "SAPIC IRR:");
    uint64_t irr[4];

    g_assert_nonnull(line);
    g_assert_cmpint(sscanf(line, "SAPIC IRR: %" SCNx64 " %" SCNx64
                          " %" SCNx64 " %" SCNx64,
                          &irr[0], &irr[1], &irr[2], &irr[3]), ==, 4);
    return (irr[vector / 64] & BIT_ULL(vector % 64)) != 0;
}

/*
 * SAPIC delivery is asynchronous; a single readback races it under host
 * load.  Spin like the other interrupt tests do.
 */
static bool sapic_wait_irr_vector(QTestState *qts, uint8_t vector)
{
    int i;

    for (i = 0; i < 1000; i++) {
        if (sapic_irr_has_vector(qts, vector)) {
            return true;
        }
        g_usleep(1000);
    }
    return false;
}

static void test_savevm_restores_platform_state(void)
{
    const char *machine = "ia64-vpc";
    const uint64_t ram_addr = 0x00300000;
    const uint64_t saved_ram = 0x0123456789abcdefULL;
    const uint64_t changed_ram = 0xfedcba9876543210ULL;
    const uint64_t saved_nvram = 0x1020304050607080ULL;
    const uint64_t changed_nvram = 0x8877665544332211ULL;
    const uint64_t saved_watchdog = 0xa5a55a5ac3c33c3cULL;
    const uint64_t changed_watchdog = 0x55aa55aa66996699ULL;
    const uint16_t saved_pm_enable = 0x0100;
    const uint16_t changed_pm_enable = 0x0400;
    const uint32_t saved_vram = 0x00112233;
    const uint32_t changed_vram = 0x00aabbcc;
    const uint32_t saved_ati_scratch = 0x13579bdf;
    const uint32_t changed_ati_scratch = 0x2468ace0;
    const unsigned pin = 23;
    const uint8_t saved_vector = 0x55;
    const uint8_t changed_vector = 0x56;
    const uint32_t rte_low = IA64_IOSAPIC_RTE_BASE + pin * 2;
    const uint64_t pm_enable_addr =
        IA64_LEGACY_IO_BASE +
        ia64_sparse_io_offset(IA64_ACPI_PM_IO_BASE +
                              IA64_ACPI_PM1_EVT_EN_OFFSET);
    g_autofree char *tmpdir = NULL;
    g_autofree char *disk_path = NULL;
    g_autofree char *quoted_disk_path = NULL;
    g_autofree char *args = NULL;
    g_autofree char *iosapic_path = NULL;
    g_autofree char *response = NULL;
    g_autoptr(GError) error = NULL;
    uint8_t int10_response[2];
    TestInt10Registers int10_regs;
    QTestState *qts;

    if (!have_qemu_img()) {
        g_test_skip("qemu-img is required for internal snapshot testing");
        return;
    }

    tmpdir = g_dir_make_tmp("ia64-vpc-savevm-XXXXXX", &error);
    g_assert_no_error(error);
    g_assert_nonnull(tmpdir);
    disk_path = g_build_filename(tmpdir, "snapshot.qcow2", NULL);
    g_assert_true(mkimg(disk_path, "qcow2", 64));
    quoted_disk_path = g_shell_quote(disk_path);
    args = g_strdup_printf("-drive file=%s,format=qcow2,if=scsi",
                           quoted_disk_path);

    qts = qtest_initf("-machine %s -m 256M -smp 4 -S %s",
                      machine, args);
    iosapic_path = find_unattached_child(qts, "ia64-iosapic");

    qtest_writeq(qts, ram_addr, saved_ram);
    qtest_writeq(qts, IA64_NVRAM_BASE, saved_nvram);
    qtest_writeq(qts, IA64_WATCHDOG_BASE + IA64_WATCHDOG_CODE_OFFSET,
                 saved_watchdog);
    qtest_writew(qts, pm_enable_addr, saved_pm_enable);
    int10_regs = (TestInt10Registers) {
        .ax = 0x4f02,
        .bx = 0x4143,
    };
    g_assert_cmpuint(int10_call(qts, &int10_regs,
                                int10_response, sizeof(int10_response)), ==, 0);
    g_assert_cmphex(int10_regs.ax, ==, 0x004f);
    qtest_writel(qts, IA64_VGA_FB_BASE, saved_vram);
    qtest_writel(qts, IA64_VGA_MMIO_BASE + IA64_ATI_BIOS_0_SCRATCH,
                 saved_ati_scratch);
    iosapic_write(qts, rte_low,
                  saved_vector | IA64_IOSAPIC_RTE_LEVEL);
    qtest_set_irq_in(qts, iosapic_path, NULL, pin, 1);
    g_assert_true(sapic_wait_irr_vector(qts, saved_vector));
    g_assert_cmphex(iosapic_read(qts, rte_low) &
                    IA64_IOSAPIC_RTE_REMOTE_IRR, !=, 0);

    response = qtest_hmp(qts, "savevm platform-state");
    g_assert_cmpstr(response, ==, "");
    g_clear_pointer(&response, g_free);

    /*
     * Reset first so that the CPU's Local SAPIC state differs as well as
     * the memory-mapped machine and IOSAPIC state.
     */
    qtest_system_reset(qts);
    qtest_writeq(qts, ram_addr, changed_ram);
    qtest_writeq(qts, IA64_NVRAM_BASE, changed_nvram);
    qtest_writeq(qts, IA64_WATCHDOG_BASE + IA64_WATCHDOG_CODE_OFFSET,
                 changed_watchdog);
    qtest_writew(qts, pm_enable_addr, changed_pm_enable);
    int10_regs = (TestInt10Registers) {
        .ax = 0x4f02,
        .bx = 0x4144,
    };
    g_assert_cmpuint(int10_call(qts, &int10_regs,
                                int10_response, sizeof(int10_response)), ==, 0);
    g_assert_cmphex(int10_regs.ax, ==, 0x004f);
    qtest_writel(qts, IA64_VGA_FB_BASE, changed_vram);
    qtest_writel(qts, IA64_VGA_MMIO_BASE + IA64_ATI_BIOS_0_SCRATCH,
                 changed_ati_scratch);
    iosapic_write(qts, rte_low,
                  changed_vector | IA64_IOSAPIC_RTE_LEVEL);
    qtest_set_irq_in(qts, iosapic_path, NULL, pin, 1);
    g_assert_true(sapic_wait_irr_vector(qts, changed_vector));
    g_assert_false(sapic_irr_has_vector(qts, saved_vector));

    response = qtest_hmp(qts, "loadvm platform-state");
    g_assert_cmpstr(response, ==, "");

    g_assert_cmphex(qtest_readq(qts, ram_addr), ==, saved_ram);
    g_assert_cmphex(qtest_readq(qts, IA64_NVRAM_BASE), ==, saved_nvram);
    g_assert_cmphex(qtest_readq(qts, IA64_WATCHDOG_BASE +
                               IA64_WATCHDOG_CODE_OFFSET),
                    ==, saved_watchdog);
    g_assert_cmphex(qtest_readw(qts, pm_enable_addr), ==, saved_pm_enable);
    g_assert_cmphex(test_vbe_read(qts, VBE_DISPI_INDEX_XRES), ==, 800);
    g_assert_cmphex(test_vbe_read(qts, VBE_DISPI_INDEX_YRES), ==, 600);
    g_assert_cmphex(test_vbe_read(qts, VBE_DISPI_INDEX_BPP), ==, 32);
    g_assert_cmphex(test_vbe_read(qts, VBE_DISPI_INDEX_ENABLE) & 0x41,
                    ==, 0x41);
    g_assert_cmphex(qtest_readl(qts, IA64_VGA_FB_BASE), ==, saved_vram);
    g_assert_cmphex(qtest_readl(qts,
                               IA64_VGA_MMIO_BASE +
                               IA64_ATI_BIOS_0_SCRATCH),
                    ==, saved_ati_scratch);
    g_assert_cmphex(iosapic_read(qts, rte_low) & 0xff, ==, saved_vector);
    g_assert_cmphex(iosapic_read(qts, rte_low) &
                    IA64_IOSAPIC_RTE_REMOTE_IRR, !=, 0);
    g_assert_true(sapic_irr_has_vector(qts, saved_vector));
    g_assert_false(sapic_irr_has_vector(qts, changed_vector));

    qtest_quit(qts);
    g_assert_cmpint(g_unlink(disk_path), ==, 0);
    g_assert_cmpint(g_rmdir(tmpdir), ==, 0);
}

/*
 * ATI RAGE 128 (Rage 128 Pro, 1002:5046) device-model regression tests.
 *
 * These drive the emulated adapter directly over its PCI BARs and lock in the
 * fork's ATI fixes and the register behaviour documented in the RAGE 128 PRO
 * Register Reference Guide: the indirect PLL register file, DAC load-sense,
 * MM_INDEX/MM_DATA indirection, the PCI-ROM-BAR ATI-table patch, and 2D solid
 * fills at every supported depth (including the 24bpp path that used to abort
 * on 3-byte pixel accesses).
 */
/* The graphics adapter is at 03:00.0, behind the GXB expander root. */
#define ATI_SLOT                IA64_460GX_GXB_VGA_SLOT

/* Register offsets (RAGE 128 PRO RRG / hw/display/ati_regs.h). */
#define ATI_MM_INDEX            0x0000
#define ATI_MM_DATA             0x0004
#define ATI_CLOCK_CNTL_INDEX    0x0008
#define ATI_CLOCK_CNTL_DATA     0x000c
#define ATI_DAC_CNTL            0x0058
#define ATI_DST_OFFSET          0x1404
#define ATI_DST_PITCH           0x1408
#define ATI_DST_Y_X             0x1438
#define ATI_DST_HEIGHT_WIDTH    0x143c
#define ATI_DP_GUI_MASTER_CNTL  0x146c
#define ATI_DP_BRUSH_FRGD_CLR   0x147c
#define ATI_DP_CNTL             0x16c0
#define ATI_DEFAULT_SC_BR       0x16e8

/* Bit / field values. */
#define ATI_PLL_WR_EN           0x00000080
#define ATI_DAC_CMP_EN          0x00000008
#define ATI_DAC_CMP_OUTPUT      0x00000080
#define ATI_MM_INDEX_VRAM       0x80000000
#define ATI_DP_LEFT_TO_RIGHT    0x00000001
#define ATI_DP_TOP_TO_BOTTOM    0x00000002
#define ATI_GMC_DST_POC         0x00000002 /* DST pitch/offset from registers */
#define ATI_GMC_BRUSH_SOLID     0x000000d0
#define ATI_GMC_ROP3_PATCOPY    0x00f00000

typedef struct {
    QTestState *qts;
    QGenericPCIBus gbus;
    QPCIDevice *dev;
    uint64_t mmio;                     /* BAR2 - MMIO register aperture */
    uint64_t fb;                       /* BAR0 - linear framebuffer      */
} ATITestDev;

static void ati_dev_open(ATITestDev *a, const char *extra)
{
    a->qts = extra ? ia64_vpc_start(extra) : ia64_vpc_start(NULL);
    ia64_qpci_init_on_bus(&a->gbus, a->qts, IA64_460GX_GXB_BUS);
    a->dev = qpci_device_find(&a->gbus.bus, QPCI_DEVFN(ATI_SLOT, 0));
    g_assert_nonnull(a->dev);
    g_assert_cmphex(qpci_config_readw(a->dev, PCI_VENDOR_ID), ==, 0x1002);
    g_assert_cmphex(qpci_config_readw(a->dev, PCI_DEVICE_ID), ==, 0x5046);
    a->mmio = qpci_config_readl(a->dev, PCI_BASE_ADDRESS_2) & 0xfffffff0;
    a->fb = qpci_config_readl(a->dev, PCI_BASE_ADDRESS_0) & 0xfffffff0;
    g_assert_cmphex(a->mmio, ==, 0xf5000000);
    g_assert_cmphex(a->fb, ==, 0xf0000000);
}

static void ati_dev_close(ATITestDev *a)
{
    g_free(a->dev);
    qtest_quit(a->qts);
}

static inline void ati_wr(ATITestDev *a, uint32_t off, uint32_t v)
{
    qtest_writel(a->qts, a->mmio + off, v);
}

static inline uint32_t ati_rd(ATITestDev *a, uint32_t off)
{
    return qtest_readl(a->qts, a->mmio + off);
}

/* Read PLL register 'idx' through the CLOCK_CNTL_INDEX/DATA window. */
static uint32_t ati_pll_rd(ATITestDev *a, uint32_t idx)
{
    ati_wr(a, ATI_CLOCK_CNTL_INDEX, idx & 0x3f);
    return ati_rd(a, ATI_CLOCK_CNTL_DATA);
}

/* Write PLL register 'idx' with PLL_WR_EN asserted. */
static void ati_pll_wr(ATITestDev *a, uint32_t idx, uint32_t v)
{
    ati_wr(a, ATI_CLOCK_CNTL_INDEX, ATI_PLL_WR_EN | (idx & 0x3f));
    ati_wr(a, ATI_CLOCK_CNTL_DATA, v);
}

/*
 * 460GX GXB AGP host bridge + GART.  Lock in exactly what Linux's i460-agp
 * driver inspects to bind (8086:84ea host bridge with an AGP capability,
 * GXBCTL bit1 = 0 for 4 KiB pages, AGPSIZ size_value = 1 for 256 MiB) and the
 * GART SRAM window at 0xFE200000: writing a GATT entry through it reads back,
 * so the driver's create_gatt_table zero+read-back works.  AGPSIZ bit3
 * (BAPBASE_ENABLE) is set, so the aperture base is read from BAPBASE (0x98) --
 * a non-header 64-bit BAR the driver masks to gart_bus_addr; it sits in the
 * platform PCI MMIO hole (below 4 GiB, as the 32-bit r128 AGP_BASE requires).
 */
#define IA64_AGP_BAPBASE        0x98
#define IA64_AGP_GXBCTL         0xa0
#define IA64_AGP_AGPSIZ         0xa2
#define IA64_AGP_GART_WINDOW    0x00000000fe200000ULL
#define IA64_AGP_APERTURE_BASE  0x00000000ee000000ULL

static void test_agp_gxb(void)
{
    QTestState *qts = qtest_init("-machine 460gx -m 256M -S");
    QGenericPCIBus gbus;
    QPCIDevice *dev;
    uint8_t cap;
    uint64_t bapbase;
    bool have_agp_cap = false;

    ia64_qpci_init(&gbus, qts);
    dev = qpci_device_find(&gbus.bus, QPCI_DEVFN(IA64_AGP_SLOT, 0));
    g_assert_nonnull(dev);

    /* i460-agp binds by vendor/device + host-bridge class. */
    g_assert_cmphex(qpci_config_readw(dev, PCI_VENDOR_ID), ==, 0x8086);
    g_assert_cmphex(qpci_config_readw(dev, PCI_DEVICE_ID), ==, 0x84ea);
    g_assert_cmphex(qpci_config_readw(dev, PCI_CLASS_DEVICE), ==,
                    PCI_CLASS_BRIDGE_HOST);

    /* And it requires an AGP capability (id 0x02) or returns -ENODEV. */
    cap = qpci_config_readb(dev, PCI_CAPABILITY_LIST);
    while (cap != 0 && cap != 0xff) {
        if (qpci_config_readb(dev, cap) == PCI_CAP_ID_AGP) {
            have_agp_cap = true;
            break;
        }
        cap = qpci_config_readb(dev, cap + PCI_CAP_LIST_NEXT);
    }
    g_assert_true(have_agp_cap);

    /* 4 KiB GART pages (GXBCTL bit1 clear) and a 256 MiB aperture (AGPSIZ=1). */
    g_assert_cmphex(qpci_config_readb(dev, IA64_AGP_GXBCTL) & 0x02, ==, 0);
    g_assert_cmphex(qpci_config_readb(dev, IA64_AGP_AGPSIZ) & 0x07, ==, 1);

    /* BAPBASE_ENABLE set, and BAPBASE holds the sub-4 GiB aperture base. */
    g_assert_cmphex(qpci_config_readb(dev, IA64_AGP_AGPSIZ) & 0x08, ==, 0x08);
    bapbase = ((uint64_t)qpci_config_readl(dev, IA64_AGP_BAPBASE + 4) << 32) |
              qpci_config_readl(dev, IA64_AGP_BAPBASE);
    g_assert_cmphex(bapbase & ~7ULL, ==, IA64_AGP_APERTURE_BASE);

    /* The GART SRAM window is writable/read-backable at its fixed address. */
    qtest_writel(qts, IA64_AGP_GART_WINDOW + 4 * 7, 0x03001234);
    g_assert_cmphex(qtest_readl(qts, IA64_AGP_GART_WINDOW + 4 * 7), ==,
                    0x03001234);
    /* Parity bit 26 is HW-owned and must not stick. */
    qtest_writel(qts, IA64_AGP_GART_WINDOW + 4 * 8, 0x04000005);
    g_assert_cmphex(qtest_readl(qts, IA64_AGP_GART_WINDOW + 4 * 8), ==,
                    0x00000005);

    g_free(dev);
    qtest_quit(qts);
}

/*
 * agp=off disables the GART: the AGP capability stays present (as on real
 * silicon), but AGPSIZ asserts SRAM_IO_DISABLE (bit4) so i460-agp's
 * fetch_size() bails and the guest keeps to the Rage 128's own PCI GART.
 */
static void test_agp_off(void)
{
    QTestState *qts = qtest_init("-machine 460gx,agp=off -m 3G -S");
    QGenericPCIBus gbus;
    QPCIDevice *dev;
    uint8_t cap;
    bool have_agp_cap = false;

    ia64_qpci_init(&gbus, qts);
    dev = qpci_device_find(&gbus.bus, QPCI_DEVFN(IA64_AGP_SLOT, 0));
    g_assert_nonnull(dev);
    g_assert_cmphex(qpci_config_readw(dev, PCI_DEVICE_ID), ==, 0x84ea);

    /* The AGP capability is unchanged -- only the GART SRAM I/O is off. */
    cap = qpci_config_readb(dev, PCI_CAPABILITY_LIST);
    while (cap != 0 && cap != 0xff) {
        if (qpci_config_readb(dev, cap) == PCI_CAP_ID_AGP) {
            have_agp_cap = true;
            break;
        }
        cap = qpci_config_readb(dev, cap + PCI_CAP_LIST_NEXT);
    }
    g_assert_true(have_agp_cap);

    /* SRAM_IO_DISABLE (bit4) set; size/BAPBASE_ENABLE fields intact. */
    g_assert_cmphex(qpci_config_readb(dev, IA64_AGP_AGPSIZ) & 0x10, ==, 0x10);
    g_assert_cmphex(qpci_config_readb(dev, IA64_AGP_AGPSIZ) & 0x0f, ==, 0x09);

    g_free(dev);
    qtest_quit(qts);
}

/* Config space: the machine reports the documented SVID=vendor/SID=device. */
static void test_ati_config_ids(void)
{
    ATITestDev a;

    ati_dev_open(&a, NULL);
    g_assert_cmphex(qpci_config_readw(a.dev, PCI_SUBSYSTEM_VENDOR_ID), ==,
                    0x1002);
    g_assert_cmphex(qpci_config_readw(a.dev, PCI_SUBSYSTEM_ID), ==, 0x5046);
    ati_dev_close(&a);
}

/*
 * Indirect PLL register file: power-up defaults, PLL_WR_EN gating, the 6-bit
 * index mask, and the PPLL_ATOMIC_UPDATE "update pending" bit (bit 15 of
 * indices 0x03..0x07) that hardware reports as clear once settled.
 */
static void test_ati_pll_regfile(void)
{
    ATITestDev a;

    ati_dev_open(&a, NULL);

    /* documented power-up values */
    g_assert_cmphex(ati_pll_rd(&a, 0x01), ==, 0x000000f7);
    g_assert_cmphex(ati_pll_rd(&a, 0x02), ==, 0x0000cc03);
    g_assert_cmphex(ati_pll_rd(&a, 0x10), ==, 0x7a770000);

    /* a write without PLL_WR_EN is dropped */
    ati_wr(&a, ATI_CLOCK_CNTL_INDEX, 0x02);           /* no WR_EN */
    ati_wr(&a, ATI_CLOCK_CNTL_DATA, 0xdeadbeef);
    g_assert_cmphex(ati_pll_rd(&a, 0x02), ==, 0x0000cc03);

    /* with PLL_WR_EN it sticks; the index masks to 6 bits on read-back */
    ati_pll_wr(&a, 0x02, 0x00001234);
    ati_wr(&a, ATI_CLOCK_CNTL_INDEX, 0x40 | 0x02);    /* high bits ignored */
    g_assert_cmphex(ati_rd(&a, ATI_CLOCK_CNTL_DATA), ==, 0x00001234);

    /* PPLL_ATOMIC_UPDATE: bit 15 of idx 0x03 reads back cleared */
    ati_pll_wr(&a, 0x03, 0x0000800c);
    g_assert_cmphex(ati_pll_rd(&a, 0x03), ==, 0x0000000c);
    /* a non-atomic index keeps bit 15 */
    ati_pll_wr(&a, 0x02, 0x00008111);
    g_assert_cmphex(ati_pll_rd(&a, 0x02), ==, 0x00008111);

    ati_dev_close(&a);
}

/*
 * DAC load-sense: with the comparator enabled the model reports a connected
 * CRT (DAC_CMP_OUTPUT set); with it disabled the bit stays clear.
 */
static void test_ati_dac_load_sense(void)
{
    ATITestDev a;

    ati_dev_open(&a, NULL);
    ati_wr(&a, ATI_DAC_CNTL, 0);
    g_assert_cmphex(ati_rd(&a, ATI_DAC_CNTL) & ATI_DAC_CMP_OUTPUT, ==, 0);
    ati_wr(&a, ATI_DAC_CNTL, ATI_DAC_CMP_EN);
    g_assert_cmphex(ati_rd(&a, ATI_DAC_CNTL) & ATI_DAC_CMP_OUTPUT, ==,
                    ATI_DAC_CMP_OUTPUT);
    ati_dev_close(&a);
}

/* MM_INDEX/MM_DATA indirection to both the register file and to VRAM. */
static void test_ati_mm_index_indirect(void)
{
    ATITestDev a;

    ati_dev_open(&a, NULL);

    /* register indirection: reach DAC_CNTL through MM_DATA */
    ati_wr(&a, ATI_DAC_CNTL, ATI_DAC_CMP_EN);
    ati_wr(&a, ATI_MM_INDEX, ATI_DAC_CNTL);
    g_assert_cmphex(ati_rd(&a, ATI_MM_DATA) & ATI_DAC_CMP_OUTPUT, ==,
                    ATI_DAC_CMP_OUTPUT);

    /* VRAM indirection (MM_INDEX bit 31): write then read back, and confirm
     * it really landed in VRAM as seen through the framebuffer BAR. */
    ati_wr(&a, ATI_MM_INDEX, ATI_MM_INDEX_VRAM | 0x40000);
    ati_wr(&a, ATI_MM_DATA, 0xcafef00d);
    ati_wr(&a, ATI_MM_INDEX, ATI_MM_INDEX_VRAM | 0x40000);
    g_assert_cmphex(ati_rd(&a, ATI_MM_DATA), ==, 0xcafef00d);
    g_assert_cmphex(qtest_readl(a.qts, a.fb + 0x40000), ==, 0xcafef00d);

    ati_dev_close(&a);
}

/*
 * PCI ROM BAR: the machine patches the stock SeaVGABIOS with the ATI tables a
 * native Rage 128 driver validates (ia64_vpc_install_ati_rom_tables): the
 * " 761295520" signature at 0x30, a PCIR structure restated to 1002:5046, and
 * a valid overall checksum over the (grown) declared image.
 */
static void test_ati_rom_bar_tables(void)
{
    ATITestDev a;
    uint32_t rom_bar;
    uint64_t rom_base;
    uint8_t *rom;
    uint32_t declared, pcir, i;
    uint8_t checksum = 0;
    int sig_at = -1;

    ati_dev_open(&a, NULL);
    /*
     * The machine assigns the ROM BAR but deliberately leaves decode OFF (an
     * XP VideoPortGetAccessRanges workaround); readers enable it transiently,
     * exactly as videoprt/pci.sys do around VideoPortGetRomImage.
     */
    rom_bar = qpci_config_readl(a.dev, PCI_ROM_ADDRESS);
    rom_base = rom_bar & 0xfffff800;
    g_assert_cmphex(rom_base, ==, 0xf6000000);
    qpci_config_writel(a.dev, PCI_ROM_ADDRESS, rom_base | 1); /* enable decode */

    rom = g_malloc(0x10000);
    qtest_memread(a.qts, rom_base, rom, 0x10000);
    g_assert_cmphex(rom[0], ==, 0x55);
    g_assert_cmphex(rom[1], ==, 0xaa);
    declared = (uint32_t)rom[2] * 512;
    g_assert_cmpuint(declared, >, 0);
    g_assert_cmpuint(declared, <=, 0x10000);

    /* signature the ATI drivers look for, at the documented 0x30 */
    g_assert_cmpmem(rom + 0x30, 10, " 761295520", 10);
    for (i = 0; i + 10 <= declared; i++) {
        if (memcmp(rom + i, " 761295520", 10) == 0) {
            sig_at = i;
            break;
        }
    }
    g_assert_cmpint(sig_at, ==, 0x30);

    /* PCIR restated to this adapter (EFI 1.10 wants it to match the header) */
    pcir = lduw_le_p(rom + 0x18);
    g_assert_cmpuint(pcir + 0x18, <=, declared);
    g_assert_cmpmem(rom + pcir, 4, "PCIR", 4);
    g_assert_cmphex(lduw_le_p(rom + pcir + 4), ==, 0x1002);
    g_assert_cmphex(lduw_le_p(rom + pcir + 6), ==, 0x5046);
    g_assert_cmphex(lduw_le_p(rom + pcir + 0x10), ==, declared / 512);

    /* the grown image checksums to zero */
    for (i = 0; i < declared; i++) {
        checksum += rom[i];
    }
    g_assert_cmphex(checksum, ==, 0);

    g_free(rom);
    ati_dev_close(&a);
}

/*
 * Program a solid-colour rectangle through the 2D engine and read it back out
 * of VRAM.  Exercises the DP_GUI_MASTER_CNTL datatype decode, the RAGE 128
 * DST_PITCH*bpp byte-stride rule, the brush/ROP fill path and ati_stpix.  At
 * 24bpp this is the case that used to abort in stn_he_p on a 3-byte store.
 */
static void ati_do_fill(ATITestDev *a, unsigned datatype, unsigned bypp,
                        uint32_t pitch_regs, uint32_t color)
{
    const uint32_t dst_off = 0x100000;
    const unsigned width = 32, height = 4;
    unsigned x, y, b;
    uint32_t gmc = (datatype << 8) | ATI_GMC_BRUSH_SOLID |
                   ATI_GMC_ROP3_PATCOPY | ATI_GMC_DST_POC;

    ati_wr(a, ATI_DEFAULT_SC_BR, 0x3fff3fff);         /* no clipping */
    ati_wr(a, ATI_DP_CNTL, ATI_DP_LEFT_TO_RIGHT | ATI_DP_TOP_TO_BOTTOM);
    ati_wr(a, ATI_DST_OFFSET, dst_off);
    ati_wr(a, ATI_DST_PITCH, pitch_regs);
    ati_wr(a, ATI_DP_BRUSH_FRGD_CLR, color);
    ati_wr(a, ATI_DP_GUI_MASTER_CNTL, gmc);
    ati_wr(a, ATI_DST_Y_X, 0);
    /* the DST_HEIGHT_WIDTH write triggers the blit */
    ati_wr(a, ATI_DST_HEIGHT_WIDTH, (height << 16) | width);

    /* every pixel of the rectangle carries the fill colour, byte-exact */
    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++) {
            uint64_t p = a->fb + dst_off + y * (width * bypp) + x * bypp;

            for (b = 0; b < bypp; b++) {
                g_assert_cmphex(qtest_readb(a->qts, p + b), ==,
                                (color >> (b * 8)) & 0xff);
            }
        }
    }
}

static void test_ati_2d_solid_fill(void)
{
    ATITestDev a;

    ati_dev_open(&a, NULL);
    /* DST_PITCH is in units of 8 pixels; byte stride = pitch * bpp. */
    ati_do_fill(&a, 2, 1, 32 / 8,     0x0000005a);     /* 8bpp  */
    ati_do_fill(&a, 3, 2, 32 / 8,     0x00001234);     /* 16bpp */
    ati_do_fill(&a, 6, 4, 32 / 8,     0x11223344);     /* 32bpp */
    ati_dev_close(&a);
}

/*
 * 24bpp fill: the RAGE 128 treats the surface as byte-wide, so the driver
 * pre-triples the pitch register; the model must store 3-byte pixels without
 * tripping stn_he_p (fixed in 605127d/d2140f0) and land them contiguously.
 */
static void test_ati_2d_fill_24bpp(void)
{
    ATITestDev a;

    ati_dev_open(&a, NULL);
    /* 24bpp: driver folds the *3 into the register -> (width/8)*3. */
    ati_do_fill(&a, 5, 3, (32 / 8) * 3, 0x00334455);
    ati_dev_close(&a);
}

/* Extra setup-engine / 2D-engine register offsets. */
#define ATI_DP_BRUSH_BKGD_CLR   0x1478
#define ATI_BRUSH_Y_X           0x1474
#define ATI_BRUSH_DATA0         0x1480
#define ATI_BRUSH_DATA1         0x1484
#define ATI_SRC_OFFSET          0x15ac
#define ATI_SRC_PITCH           0x15b0
#define ATI_SRC_Y_X             0x1434
#define ATI_SC_TOP_LEFT         0x16ec
#define ATI_SCALE_3D_CNTL       0x1a00
#define ATI_SETUP_CNTL          0x1bc4
#define ATI_SU_DDA_BASE         0x1a40      /* per-channel {dx,dy,val}, 12B/ch */
#define ATI_GMC_SRC_POC         0x00000001
#define ATI_ROP3_SRCCOPY        0x00cc0000
#define ATI_ROP3_SRCINVERT      0x00660000
#define ATI_ROP3_SRCPAINT       0x00ee0000

static inline void ati_vram_wr32(ATITestDev *a, uint32_t off, uint32_t v)
{
    qtest_writel(a->qts, a->fb + off, v);
}

static inline uint32_t ati_vram_rd32(ATITestDev *a, uint32_t off)
{
    return qtest_readl(a->qts, a->fb + off);
}

/*
 * Caption gradient: the ati2draa driver programs the 2D setup engine's
 * per-channel colour DDA (0x1a40..) and issues a solid rectangle paint with
 * SCALE_3D_CNTL enabled and SETUP_CNTL COLOR_FCN = Gouraud; the engine then
 * interpolates the colour across the rectangle instead of using the brush.
 * Colour_c(x,y) = val_c + dx_c*(x-x0)*xstep + dy_c*(y-y0), clamped, where
 * xstep is 3 at 24bpp (the DDA advances once per byte) and 1 otherwise.  The
 * test programs a known plane and checks the engine reproduces the formula.
 */
static void ati_run_gradient(ATITestDev *a, unsigned datatype, unsigned bypp,
                             int xstep, const int32_t val[3],
                             const int32_t dx[3], const int32_t dy[3])
{
    const uint32_t dst_off = 0x100000;
    const unsigned w = 32, h = 4, pitch = (bypp == 3) ? (w / 8) * 3 : w / 8;
    const uint32_t stride = pitch * ((bypp == 3) ? 8 : bypp * 8);
    unsigned ch, x, y, b;

    ati_wr(a, ATI_DEFAULT_SC_BR, 0x3fff3fff);
    ati_wr(a, ATI_SC_TOP_LEFT, 0);
    ati_wr(a, ATI_DP_CNTL, ATI_DP_LEFT_TO_RIGHT | ATI_DP_TOP_TO_BOTTOM);
    ati_wr(a, ATI_DST_OFFSET, dst_off);
    ati_wr(a, ATI_DST_PITCH, pitch);
    ati_wr(a, ATI_DP_GUI_MASTER_CNTL, (datatype << 8) | ATI_GMC_DST_POC);
    ati_wr(a, ATI_SCALE_3D_CNTL, 0x40);               /* setup block enable */
    ati_wr(a, ATI_SETUP_CNTL, 4 << 3);                /* COLOR_FCN = Gouraud */
    for (ch = 0; ch < 3; ch++) {
        ati_wr(a, ATI_SU_DDA_BASE + ch * 0xc + 0, (uint32_t)dx[ch]);
        ati_wr(a, ATI_SU_DDA_BASE + ch * 0xc + 4, (uint32_t)dy[ch]);
        ati_wr(a, ATI_SU_DDA_BASE + ch * 0xc + 8, (uint32_t)val[ch]);
    }
    ati_wr(a, ATI_DST_Y_X, 0);
    ati_wr(a, ATI_DST_HEIGHT_WIDTH, (h << 16) | w);   /* triggers the paint */

    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            uint8_t exp[3];                            /* R,G,B */
            uint64_t p = a->fb + dst_off + y * stride + x * bypp;

            for (ch = 0; ch < 3; ch++) {
                int64_t v = (int64_t)val[ch] +
                            (int64_t)dx[ch] * (int)x * xstep +
                            (int64_t)dy[ch] * (int)y;
                int iv = (int)(v >> 16);

                exp[ch] = iv < 0 ? 0 : iv > 255 ? 255 : iv;
            }
            if (bypp >= 3) {                            /* stored B,G,R(,A) */
                g_assert_cmphex(qtest_readb(a->qts, p + 0), ==, exp[2]);
                g_assert_cmphex(qtest_readb(a->qts, p + 1), ==, exp[1]);
                g_assert_cmphex(qtest_readb(a->qts, p + 2), ==, exp[0]);
            } else if (bypp == 2) {                     /* RGB565 */
                uint16_t px = qtest_readw(a->qts, p);
                g_assert_cmpuint((px >> 11) & 0x1f, ==, exp[0] >> 3);
                g_assert_cmpuint((px >> 5) & 0x3f, ==, exp[1] >> 2);
                g_assert_cmpuint(px & 0x1f, ==, exp[2] >> 3);
            }
            (void)b;
        }
    }
}

static void test_ati_gradient_32bpp(void)
{
    ATITestDev a;
    /* horizontal ramp on R, flat G/B (16.16). */
    const int32_t val[3] = { 16 << 16, 64 << 16, 128 << 16 };
    const int32_t dx[3]  = { 1 << 16, 0, 0 };
    const int32_t dy[3]  = { 0, 0, 0 };

    ati_dev_open(&a, NULL);
    ati_run_gradient(&a, 6, 4, 1, val, dx, dy);
    ati_dev_close(&a);
}

static void test_ati_gradient_24bpp(void)
{
    ATITestDev a;
    /* dx is programmed at ~1/3 the slope; the engine multiplies by xstep=3. */
    const int32_t val[3] = { 16 << 16, 64 << 16, 128 << 16 };
    const int32_t dx[3]  = { (1 << 16) / 3, 0, 0 };
    const int32_t dy[3]  = { 0, 0, 0 };

    ati_dev_open(&a, NULL);
    ati_run_gradient(&a, 5, 3, 3, val, dx, dy);
    ati_dev_close(&a);
}

/*
 * 8x8 monochrome pattern brush (PATCOPY, brush type 0): the pattern bit at
 * (x,y) selects foreground vs background.  Regresses the pattern-brush fill.
 */
static void test_ati_pattern_brush(void)
{
    ATITestDev a;
    const uint32_t dst_off = 0x100000;
    const unsigned w = 8, h = 4;
    unsigned x, y;

    ati_dev_open(&a, NULL);
    ati_wr(&a, ATI_DEFAULT_SC_BR, 0x3fff3fff);
    ati_wr(&a, ATI_SC_TOP_LEFT, 0);
    ati_wr(&a, ATI_DP_CNTL, ATI_DP_LEFT_TO_RIGHT | ATI_DP_TOP_TO_BOTTOM);
    ati_wr(&a, ATI_DST_OFFSET, dst_off);
    ati_wr(&a, ATI_DST_PITCH, w / 8);
    ati_wr(&a, ATI_DP_BRUSH_FRGD_CLR, 0xaa);
    ati_wr(&a, ATI_DP_BRUSH_BKGD_CLR, 0xbb);
    ati_wr(&a, ATI_BRUSH_Y_X, 0);
    ati_wr(&a, ATI_BRUSH_DATA0, 0x55555555);          /* each row 0b01010101 */
    ati_wr(&a, ATI_BRUSH_DATA1, 0x55555555);
    /* datatype 8bpp, brush field 0 (8x8 mono), ROP PATCOPY, dst from regs */
    ati_wr(&a, ATI_DP_GUI_MASTER_CNTL,
           (2 << 8) | ATI_GMC_ROP3_PATCOPY | ATI_GMC_DST_POC);
    ati_wr(&a, ATI_DST_Y_X, 0);
    ati_wr(&a, ATI_DST_HEIGHT_WIDTH, (h << 16) | w);

    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            uint8_t exp = (x & 1) ? 0xbb : 0xaa;       /* bit0 set on even x */
            g_assert_cmphex(qtest_readb(a.qts, a.fb + dst_off + y * w + x),
                            ==, exp);
        }
    }
    ati_dev_close(&a);
}

/*
 * Source/destination ROP blits (32bpp): the engine combines a source and the
 * existing destination per the ROP3 code.  Regresses the general-ROP path
 * (SRCINVERT was the XOR-trail case, SRCPAINT the OR case).
 */
static void ati_rop_case(ATITestDev *a, uint32_t rop3, uint32_t sc,
                         uint32_t dc, uint32_t expect)
{
    const uint32_t src_off = 0x200000, dst_off = 0x100000;
    const unsigned w = 8, h = 2;
    const uint32_t stride = (w / 8) * 32;              /* 32bpp byte stride */
    unsigned x, y;

    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            ati_vram_wr32(a, src_off + y * stride + x * 4, sc);
            ati_vram_wr32(a, dst_off + y * stride + x * 4, dc);
        }
    }
    ati_wr(a, ATI_DEFAULT_SC_BR, 0x3fff3fff);
    ati_wr(a, ATI_SC_TOP_LEFT, 0);
    ati_wr(a, ATI_DP_CNTL, ATI_DP_LEFT_TO_RIGHT | ATI_DP_TOP_TO_BOTTOM);
    ati_wr(a, ATI_SRC_OFFSET, src_off);
    ati_wr(a, ATI_SRC_PITCH, w / 8);
    ati_wr(a, ATI_SRC_Y_X, 0);
    ati_wr(a, ATI_DST_OFFSET, dst_off);
    ati_wr(a, ATI_DST_PITCH, w / 8);
    ati_wr(a, ATI_DP_GUI_MASTER_CNTL,
           (6 << 8) | rop3 | ATI_GMC_SRC_POC | ATI_GMC_DST_POC);
    ati_wr(a, ATI_DST_Y_X, 0);
    ati_wr(a, ATI_DST_HEIGHT_WIDTH, (h << 16) | w);

    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            g_assert_cmphex(ati_vram_rd32(a, dst_off + y * stride + x * 4),
                            ==, expect);
        }
    }
}

static void test_ati_rop_src_dst(void)
{
    ATITestDev a;

    ati_dev_open(&a, NULL);
    ati_rop_case(&a, ATI_ROP3_SRCCOPY, 0x11223344, 0x55667788, 0x11223344);
    ati_rop_case(&a, ATI_ROP3_SRCINVERT, 0x11223344, 0x0f0f0f0f, 0x1e2d3c4b);
    ati_rop_case(&a, ATI_ROP3_SRCPAINT, 0x11002200, 0x00330044, 0x11332244);
    ati_dev_close(&a);
}

/*
 * Overlapping same-surface SRCCOPY (a window move / scroll): pixman shears
 * overlapping copies, so the model memmoves each row walking away from the
 * destination.  Copy a strip of distinct-per-row values down by two rows and
 * confirm no row is clobbered before it is read.  Regresses the drag smear.
 */
static void test_ati_overlap_copy(void)
{
    ATITestDev a;
    const uint32_t off = 0x100000;
    const unsigned w = 8, h = 4;
    const uint32_t stride = (w / 8) * 32;
    unsigned x, y;

    ati_dev_open(&a, NULL);
    for (y = 0; y < 6; y++) {                          /* seed 6 distinct rows */
        for (x = 0; x < w; x++) {
            ati_vram_wr32(&a, off + y * stride + x * 4, 0x1000 + y);
        }
    }
    ati_wr(&a, ATI_DEFAULT_SC_BR, 0x3fff3fff);
    ati_wr(&a, ATI_SC_TOP_LEFT, 0);
    ati_wr(&a, ATI_DP_CNTL, ATI_DP_LEFT_TO_RIGHT | ATI_DP_TOP_TO_BOTTOM);
    ati_wr(&a, ATI_SRC_OFFSET, off);
    ati_wr(&a, ATI_SRC_PITCH, w / 8);
    ati_wr(&a, ATI_SRC_Y_X, 0);                        /* src rows 0..3 */
    ati_wr(&a, ATI_DST_OFFSET, off);
    ati_wr(&a, ATI_DST_PITCH, w / 8);
    ati_wr(&a, ATI_DP_GUI_MASTER_CNTL,
           (6 << 8) | ATI_ROP3_SRCCOPY | ATI_GMC_SRC_POC | ATI_GMC_DST_POC);
    ati_wr(&a, ATI_DST_Y_X, 2 << 16);                  /* dst rows 2..5 */
    ati_wr(&a, ATI_DST_HEIGHT_WIDTH, (h << 16) | w);

    /* rows 0,1 untouched; rows 2..5 are the copied old rows 0..3 */
    for (x = 0; x < w; x++) {
        g_assert_cmphex(ati_vram_rd32(&a, off + 0 * stride + x * 4), ==,
                        0x1000);
        g_assert_cmphex(ati_vram_rd32(&a, off + 1 * stride + x * 4), ==,
                        0x1001);
        g_assert_cmphex(ati_vram_rd32(&a, off + 2 * stride + x * 4), ==,
                        0x1000);
        g_assert_cmphex(ati_vram_rd32(&a, off + 3 * stride + x * 4), ==,
                        0x1001);
        g_assert_cmphex(ati_vram_rd32(&a, off + 4 * stride + x * 4), ==,
                        0x1002);
        g_assert_cmphex(ati_vram_rd32(&a, off + 5 * stride + x * 4), ==,
                        0x1003);
    }
    ati_dev_close(&a);
}

/*
 * 14-bit SIGNED destination coordinates (ati_sext14): a caption whose left
 * edge is off-screen is encoded as a negative 14-bit X.  Exercised on the
 * gradient path, which is where it matters (an off-origin caption ramp).
 * Without sign extension DST_X = 0x3ffe reads as +16382, placing the whole
 * rectangle past the right scissor edge so columns 0..3 stay clear; with it
 * the origin is -2 and those columns render, interpolated from x0 = -2.
 */
static void test_ati_sext14_coord(void)
{
    ATITestDev a;
    const uint32_t dst_off = 0x100000;
    const int x0 = -2;
    const int32_t val[3] = { 16 << 16, 64 << 16, 128 << 16 };
    const int32_t dx[3]  = { 1 << 16, 0, 0 };
    unsigned x, ch;

    ati_dev_open(&a, NULL);
    for (x = 0; x < 8; x++) {                          /* clear the row */
        ati_vram_wr32(&a, dst_off + x * 4, 0);
    }
    ati_wr(&a, ATI_DEFAULT_SC_BR, 0x3fff3fff);
    ati_wr(&a, ATI_SC_TOP_LEFT, 0);
    ati_wr(&a, ATI_DP_CNTL, ATI_DP_LEFT_TO_RIGHT | ATI_DP_TOP_TO_BOTTOM);
    ati_wr(&a, ATI_DST_OFFSET, dst_off);
    ati_wr(&a, ATI_DST_PITCH, 8 / 8);
    ati_wr(&a, ATI_DP_GUI_MASTER_CNTL, (6 << 8) | ATI_GMC_DST_POC);
    ati_wr(&a, ATI_SCALE_3D_CNTL, 0x40);
    ati_wr(&a, ATI_SETUP_CNTL, 4 << 3);
    for (ch = 0; ch < 3; ch++) {
        ati_wr(&a, ATI_SU_DDA_BASE + ch * 0xc + 0, (uint32_t)dx[ch]);
        ati_wr(&a, ATI_SU_DDA_BASE + ch * 0xc + 4, 0);
        ati_wr(&a, ATI_SU_DDA_BASE + ch * 0xc + 8, (uint32_t)val[ch]);
    }
    ati_wr(&a, ATI_DST_Y_X, 0x3ffe);                   /* x = -2 (sext14) */
    ati_wr(&a, ATI_DST_HEIGHT_WIDTH, (1 << 16) | 6);   /* covers x -2..3 */

    /* columns 0..3 render with R interpolated from the negative origin */
    for (x = 0; x < 4; x++) {
        int r = 16 + ((int)x - x0);                    /* val_R + dx_R*(x-x0) */
        g_assert_cmphex(ati_vram_rd32(&a, dst_off + x * 4), ==,
                        0xff000000u | (uint32_t)r << 16 | (64 << 8) | 128);
    }
    for (x = 4; x < 8; x++) {                          /* untouched */
        g_assert_cmphex(ati_vram_rd32(&a, dst_off + x * 4), ==, 0);
    }
    ati_dev_close(&a);
}

#define ATI_CLR_CMP_CLR_SRC     0x15c4
#define ATI_CLR_CMP_MASK        0x15cc
#define ATI_CLR_CMP_CNTL        0x15c0
#define ATI_GMC_CLR_CMP_FCN_CLR 0x10000000 /* DP_GUI_MASTER_CNTL bit 28 */

/*
 * A GUI-master-control write with GMC_CLR_CMP_CNTL_DIS (bit 28) must clear the
 * colour-compare function (CLR_CMP_CNTL FN_SRC/FN_DST).  The XFree86 r128
 * driver sets this bit in the base control word of every op, so a colour key
 * enabled by a transparent (window-decoration) blit does not leak into the
 * following text/fill ops.  Without this, a stale key drops keyed pixels and
 * corrupts KDE's terminal text and window frames.
 */
static void test_ati_clr_cmp_clear(void)
{
    ATITestDev a;
    const uint32_t src_off = 0x200000, dst_off = 0x100000;
    const unsigned w = 4, h = 1;
    const uint32_t stride = (w / 8 ? w / 8 : 1) * 32;
    const uint32_t key = 0x00aaaaaa, other = 0x00112233;
    unsigned x;

    ati_dev_open(&a, NULL);

    /* src: [key, other, key, other]; dst pre-cleared to 0 */
    ati_vram_wr32(&a, src_off + 0 * 4, key);
    ati_vram_wr32(&a, src_off + 1 * 4, other);
    ati_vram_wr32(&a, src_off + 2 * 4, key);
    ati_vram_wr32(&a, src_off + 3 * 4, other);
    for (x = 0; x < w; x++) {
        ati_vram_wr32(&a, dst_off + x * 4, 0);
    }

    /* Enable a NEQ colour key on 'key' (draw only where src != key). */
    ati_wr(&a, ATI_CLR_CMP_CLR_SRC, key);
    ati_wr(&a, ATI_CLR_CMP_MASK, 0xffffffff);
    ati_wr(&a, ATI_CLR_CMP_CNTL, 5);              /* FN_SRC = CMP_NEQ */

    ati_wr(&a, ATI_DEFAULT_SC_BR, 0x3fff3fff);
    ati_wr(&a, ATI_SC_TOP_LEFT, 0);
    ati_wr(&a, ATI_DP_CNTL, ATI_DP_LEFT_TO_RIGHT | ATI_DP_TOP_TO_BOTTOM);
    ati_wr(&a, ATI_SRC_OFFSET, src_off);
    ati_wr(&a, ATI_SRC_PITCH, 1);
    ati_wr(&a, ATI_SRC_Y_X, 0);
    ati_wr(&a, ATI_DST_OFFSET, dst_off);
    ati_wr(&a, ATI_DST_PITCH, 1);
    /*
     * This control write carries GMC_CLR_CMP_CNTL_DIS, so it must clear the
     * key first: the copy then draws ALL four source pixels, key ones included.
     */
    ati_wr(&a, ATI_DP_GUI_MASTER_CNTL,
           (6 << 8) | ATI_ROP3_SRCCOPY | ATI_GMC_SRC_POC | ATI_GMC_DST_POC |
           ATI_GMC_CLR_CMP_FCN_CLR);
    ati_wr(&a, ATI_DST_Y_X, 0);
    ati_wr(&a, ATI_DST_HEIGHT_WIDTH, (h << 16) | w);

    /* every source pixel copied, including the two that matched the key */
    g_assert_cmphex(ati_vram_rd32(&a, dst_off + 0 * 4), ==, key);
    g_assert_cmphex(ati_vram_rd32(&a, dst_off + 1 * 4), ==, other);
    g_assert_cmphex(ati_vram_rd32(&a, dst_off + 2 * 4), ==, key);
    g_assert_cmphex(ati_vram_rd32(&a, dst_off + 3 * 4), ==, other);

    (void)stride;
    ati_dev_close(&a);
}

/* PM4 indirect-buffer launch registers and the CCE type-0 packet header. */
#define ATI_PM4_IW_INDOFF       0x0738
#define ATI_PM4_IW_INDSIZE      0x073c
/* CCE_PACKET0(reg, n): type-0, writes n+1 registers from reg. n=0 -> one. */
#define ATI_CCE_PACKET0_1(reg)  ((reg) >> 2)

/*
 * PM4 indirect buffers.  The r128 DRM (unlike the XP inbox driver, which
 * inlines type-3 packets in the ring) batches all 2D work into DMA buffers and
 * only writes the buffer's card VM byte offset and dword length into the ring;
 * writing PM4_IW_INDSIZE makes the CCE fetch and run that buffer as a packet
 * stream.  Build such a buffer in VRAM -- with no GART configured it is read
 * straight from local video memory -- carrying the CCE_PACKET0 register writes
 * for a solid fill, and confirm the fill reaches VRAM.  Critically, nothing
 * may draw until the INDSIZE write fires the launch (writing INDOFF alone is
 * inert), which is exactly the path that was dropped before: the greeter's
 * whole render batch went unexecuted.
 */
static void test_ati_cce_indirect_buffer(void)
{
    ATITestDev a;
    const uint32_t buf_off = 0x200000;   /* indirect buffer, in VRAM  */
    const uint32_t dst_off = 0x100000;   /* fill destination, in VRAM */
    const unsigned width = 32, height = 4;
    const uint32_t color = 0xa1b2c3d4;
    const uint32_t gmc = (6 << 8) | ATI_GMC_BRUSH_SOLID |    /* 32bpp */
                         ATI_GMC_ROP3_PATCOPY | ATI_GMC_DST_POC;
    const uint32_t prog[] = {
        ATI_CCE_PACKET0_1(ATI_DEFAULT_SC_BR),      0x3fff3fff,
        ATI_CCE_PACKET0_1(ATI_DP_CNTL),
            ATI_DP_LEFT_TO_RIGHT | ATI_DP_TOP_TO_BOTTOM,
        ATI_CCE_PACKET0_1(ATI_DST_OFFSET),         dst_off,
        ATI_CCE_PACKET0_1(ATI_DST_PITCH),          32 / 8,
        ATI_CCE_PACKET0_1(ATI_DP_BRUSH_FRGD_CLR),  color,
        ATI_CCE_PACKET0_1(ATI_DP_GUI_MASTER_CNTL), gmc,
        ATI_CCE_PACKET0_1(ATI_DST_Y_X),            0,
        ATI_CCE_PACKET0_1(ATI_DST_HEIGHT_WIDTH),   (height << 16) | width,
    };
    unsigned i, x, y;

    ati_dev_open(&a, NULL);

    /* Lay the packet stream into VRAM and pre-poison the fill target. */
    for (i = 0; i < ARRAY_SIZE(prog); i++) {
        ati_vram_wr32(&a, buf_off + i * 4, prog[i]);
    }
    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++) {
            ati_vram_wr32(&a, dst_off + (y * width + x) * 4, 0xdeadbeef);
        }
    }

    /* INDOFF alone is inert -- the launch is the INDSIZE write. */
    ati_wr(&a, ATI_PM4_IW_INDOFF, buf_off);
    g_assert_cmphex(ati_vram_rd32(&a, dst_off), ==, 0xdeadbeef);

    /* Writing INDSIZE fetches and runs the buffer; the fill lands in VRAM. */
    ati_wr(&a, ATI_PM4_IW_INDSIZE, ARRAY_SIZE(prog));
    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++) {
            g_assert_cmphex(ati_vram_rd32(&a, dst_off + (y * width + x) * 4),
                            ==, color);
        }
    }

    ati_dev_close(&a);
}

/*
 * r128 AGP_BASE (0x0170) and where the r128 MC images the AGP aperture
 * (R128_AGP_OFFSET).  IA64_AGP_GART_WINDOW / IA64_AGP_APERTURE_BASE are shared
 * with test_agp_gxb above.
 */
#define ATI_AGP_BASE            0x0170
#define R128_AGP_OFFSET         0x02000000u

/*
 * End-to-end >4 GiB DMA path: an r128 CCE fetch of an AGP-resident indirect
 * buffer, routed through the 460GX GXB GART to DRAM.  This is the whole reason
 * approach B exists -- prove the two halves compose:
 *
 *   1. r128 AGP forwarding: with AGP_BASE set, a card VM address at/above
 *      R128_AGP_OFFSET maps to AGP bus AGP_BASE + (vm - R128_AGP_OFFSET) and is
 *      issued as a DMA, hitting the GXB IOMMU (ati.c ati_cce_vm_dword);
 *   2. GXB GART: that aperture page is relocated by its GATT entry to the DRAM
 *      page the driver mapped.
 *
 * The indirect buffer (a solid-fill packet stream) lives ONLY in DRAM, reached
 * exclusively through the aperture -- the VM offset used is past the 16 MiB
 * framebuffer, so if either half were broken the CCE would read zeros and the
 * fill would never land.
 */
static void test_agp_gart_dma(void)
{
    ATITestDev a;
    const uint32_t dram_page = 0x08000000;       /* scratch DRAM (128 MiB)  */
    const uint32_t agp_vm = R128_AGP_OFFSET;     /* aperture page 0         */
    const uint32_t dst_off = 0x100000;           /* fill target, in VRAM    */
    const unsigned width = 32, height = 4;
    const uint32_t color = 0xcafe1260;
    const uint32_t gmc = (6 << 8) | ATI_GMC_BRUSH_SOLID |
                         ATI_GMC_ROP3_PATCOPY | ATI_GMC_DST_POC;
    const uint32_t prog[] = {
        ATI_CCE_PACKET0_1(ATI_DEFAULT_SC_BR),      0x3fff3fff,
        ATI_CCE_PACKET0_1(ATI_DP_CNTL),
            ATI_DP_LEFT_TO_RIGHT | ATI_DP_TOP_TO_BOTTOM,
        ATI_CCE_PACKET0_1(ATI_DST_OFFSET),         dst_off,
        ATI_CCE_PACKET0_1(ATI_DST_PITCH),          32 / 8,
        ATI_CCE_PACKET0_1(ATI_DP_BRUSH_FRGD_CLR),  color,
        ATI_CCE_PACKET0_1(ATI_DP_GUI_MASTER_CNTL), gmc,
        ATI_CCE_PACKET0_1(ATI_DST_Y_X),            0,
        ATI_CCE_PACKET0_1(ATI_DST_HEIGHT_WIDTH),   (height << 16) | width,
    };
    unsigned i, x, y;

    ati_dev_open(&a, NULL);

    /* The AGP fetch is a bus-master DMA cycle: enable bus mastering. */
    qpci_config_writew(a.dev, PCI_COMMAND,
                       qpci_config_readw(a.dev, PCI_COMMAND) |
                       PCI_COMMAND_MASTER);

    /* Map aperture page 0 -> the DRAM scratch page: valid | phys[35:12]. */
    qtest_writel(a.qts, IA64_AGP_GART_WINDOW + 0,
                 0x03000000u | (dram_page >> 12));
    /* Point the card's AGP window at the chipset aperture base. */
    ati_wr(&a, ATI_AGP_BASE, (uint32_t)IA64_AGP_APERTURE_BASE);

    /* Indirect buffer lives in DRAM, reachable only through the aperture. */
    for (i = 0; i < ARRAY_SIZE(prog); i++) {
        qtest_writel(a.qts, dram_page + i * 4, prog[i]);
    }
    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++) {
            ati_vram_wr32(&a, dst_off + (y * width + x) * 4, 0xdeadbeef);
        }
    }

    /* Launch: CCE fetches the buffer via AGP -> GART -> DRAM and runs it. */
    ati_wr(&a, ATI_PM4_IW_INDOFF, agp_vm);
    ati_wr(&a, ATI_PM4_IW_INDSIZE, ARRAY_SIZE(prog));
    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++) {
            g_assert_cmphex(ati_vram_rd32(&a, dst_off + (y * width + x) * 4),
                            ==, color);
        }
    }

    ati_dev_close(&a);
}

/*
 * Mach64 3D Rage (DEV_4754) tests.  The adapter is selected with
 * -machine 460gx,vga=mach64; its BAR2 register block and BAR0 framebuffer
 * land on the same fixed windows the machine assigns to any VGA-slot device.
 * A Block-0 register at Mach64 block index r decodes to BAR2 + 0x400 + r*4.
 */
#define M64_REG(r)              (0x400u + (unsigned)(r) * 4u)
#define M64_CONFIG_CHIP_ID      0x38
#define M64_SCRATCH_REG0        0x20
#define M64_DST_OFF_PITCH       0x40
#define M64_DST_Y_X             0x43
#define M64_DST_HEIGHT_WIDTH    0x46
#define M64_SC_LEFT             0xa8
#define M64_SC_RIGHT            0xa9
#define M64_SC_TOP              0xab
#define M64_SC_BOTTOM           0xac
#define M64_DP_BKGD_CLR         0xb0
#define M64_DP_FRGD_CLR         0xb1
#define M64_DP_WRITE_MASK       0xb2
#define M64_DP_PIX_WIDTH        0xb4
#define M64_DP_MIX              0xb5
#define M64_DP_SRC              0xb6

/* Mach64 DP_*_PIX_WIDTH codes (2 = 8bpp, 4 = 16bpp, 6 = 32bpp). */
#define M64_PIX_WIDTH_8BPP      2
#define M64_PIX_WIDTH_16BPP     4
#define M64_PIX_WIDTH_32BPP     6

typedef struct {
    QTestState *qts;
    QGenericPCIBus gbus;
    QPCIDevice *dev;
    uint64_t mmio;
    uint64_t fb;
} Mach64TestDev;

static void mach64_dev_open_id(Mach64TestDev *a, const char *extra,
                               uint16_t dev_id)
{
    a->qts = ia64_vpc_start(extra ?: "-machine vga=mach64");
    /* Every graphics adapter sits on the GXB root, whatever the model. */
    ia64_qpci_init_on_bus(&a->gbus, a->qts, IA64_460GX_GXB_BUS);
    a->dev = qpci_device_find(&a->gbus.bus, QPCI_DEVFN(ATI_SLOT, 0));
    g_assert_nonnull(a->dev);
    g_assert_cmphex(qpci_config_readw(a->dev, PCI_VENDOR_ID), ==, 0x1002);
    g_assert_cmphex(qpci_config_readw(a->dev, PCI_DEVICE_ID), ==, dev_id);
    a->mmio = qpci_config_readl(a->dev, PCI_BASE_ADDRESS_2) & 0xfffffff0;
    a->fb = qpci_config_readl(a->dev, PCI_BASE_ADDRESS_0) & 0xfffffff0;
    g_assert_cmphex(a->mmio, ==, 0xf5000000);
    g_assert_cmphex(a->fb, ==, 0xf0000000);
}

static void mach64_dev_open(Mach64TestDev *a)
{
    /* default adapter is the Rage XL (0x4752). */
    mach64_dev_open_id(a, "-machine vga=mach64", 0x4752);
}

static void mach64_dev_close(Mach64TestDev *a)
{
    g_free(a->dev);
    qtest_quit(a->qts);
}

static inline void m64_wr(Mach64TestDev *a, unsigned reg, uint32_t v)
{
    qtest_writel(a->qts, a->mmio + M64_REG(reg), v);
}

static inline uint32_t m64_rd(Mach64TestDev *a, unsigned reg)
{
    return qtest_readl(a->qts, a->mmio + M64_REG(reg));
}

static void test_mach64_ids(void)
{
    Mach64TestDev a;

    /* Default: Rage XL 0x4752, auto revision 0x27 (the id both XP builds match). */
    mach64_dev_open(&a);
    g_assert_cmphex(qpci_config_readb(a.dev, PCI_REVISION_ID), ==, 0x27);
    /* CONFIG_CHIP_ID: device id in the type field, PCI revision in [31:24]. */
    g_assert_cmphex(m64_rd(&a, M64_CONFIG_CHIP_ID) & 0xffff, ==, 0x4752);
    g_assert_cmphex(m64_rd(&a, M64_CONFIG_CHIP_ID) >> 24, ==, 0x27);
    /* subsystem falls back to vendor/device (set by the machine). */
    g_assert_cmphex(qpci_config_readw(a.dev, PCI_SUBSYSTEM_VENDOR_ID), ==,
                    0x1002);
    g_assert_cmphex(qpci_config_readw(a.dev, PCI_SUBSYSTEM_ID), ==, 0x4752);
    /* a scratch register round-trips. */
    m64_wr(&a, M64_SCRATCH_REG0, 0xdeadbeef);
    g_assert_cmphex(m64_rd(&a, M64_SCRATCH_REG0), ==, 0xdeadbeef);
    mach64_dev_close(&a);

    /* x-device-id=0x4754 selects 3D Rage II with auto revision 0x9A. */
    mach64_dev_open_id(&a, "-machine vga=mach64 -global mach64-vga.x-device-id=0x4754",
                       0x4754);
    g_assert_cmphex(qpci_config_readb(a.dev, PCI_REVISION_ID), ==, 0x9a);
    g_assert_cmphex(m64_rd(&a, M64_CONFIG_CHIP_ID) & 0xffff, ==, 0x4754);
    g_assert_cmphex(qpci_config_readw(a.dev, PCI_SUBSYSTEM_ID), ==, 0x4754);
    mach64_dev_close(&a);
}

static void mach64_do_fill(Mach64TestDev *a, unsigned pixw, unsigned bypp,
                           uint32_t color)
{
    const unsigned width = 32, height = 4;
    const unsigned pitch_px = 32;

    m64_wr(a, M64_DP_PIX_WIDTH, pixw);                 /* DST pixel width */
    m64_wr(a, M64_DST_OFF_PITCH, (pitch_px / 8) << 22);/* offset 0, pitch */
    m64_wr(a, M64_DP_FRGD_CLR, color);
    m64_wr(a, M64_DP_MIX, 0x7u << 16);                 /* FRGD_MIX = SRC */
    m64_wr(a, M64_DP_SRC, 0x1u << 8);                  /* FRGD_SRC = FRGD_CLR */
    m64_wr(a, M64_DP_WRITE_MASK, 0xffffffff);
    m64_wr(a, M64_SC_LEFT, 0);
    m64_wr(a, M64_SC_RIGHT, 0x3fff);
    m64_wr(a, M64_SC_TOP, 0);
    m64_wr(a, M64_SC_BOTTOM, 0x3fff);
    m64_wr(a, M64_DST_Y_X, 0);                          /* x=0, y=0 */
    /* the DST_HEIGHT_WIDTH write (W high, H low) triggers the fill */
    m64_wr(a, M64_DST_HEIGHT_WIDTH, (width << 16) | height);

    for (unsigned y = 0; y < height; y++) {
        for (unsigned x = 0; x < width; x++) {
            uint64_t p = a->fb + y * (pitch_px * bypp) + x * bypp;

            for (unsigned b = 0; b < bypp; b++) {
                g_assert_cmphex(qtest_readb(a->qts, p + b), ==,
                                (color >> (b * 8)) & 0xff);
            }
        }
    }
}

static void test_mach64_2d_solid_fill(void)
{
    Mach64TestDev a;

    mach64_dev_open(&a);
    mach64_do_fill(&a, M64_PIX_WIDTH_8BPP,  1, 0x0000005a);
    mach64_do_fill(&a, M64_PIX_WIDTH_16BPP, 2, 0x00001234);
    mach64_do_fill(&a, M64_PIX_WIDTH_32BPP, 4, 0x11223344);
    mach64_dev_close(&a);
}

/*
 * DDC/EDID monitor detection.  The native Rage XL miniport bit-bangs I2C over
 * "LCD register 7" (LCD_INDEX selects 7, LCD_DATA is the GPIO byte) to read the
 * monitor EDID; without a valid EDID it spins forever in a DAC load-sense
 * fallback.  Drive that same register here as an I2C master and confirm the
 * i2c-ddc slave ACKs and returns a well-formed EDID.  This exercises exactly
 * the register bit model the guest driver depends on, with no guest boot.
 */
#define M64_LCD_INDEX           0x29
#define M64_LCD_DATA            0x2a
#define M64_DDC_INDEX_I2C       7
/*
 * LCD-reg-7 DDC uses the Mach64 direction+state open-drain model split across
 * two bytes of LCD_DATA, each accessed on its own (read-modify-write):
 *   byte 1 (dword bits 13/14) = STATE     (SDA bit5, SCL bit6 within the byte)
 *   byte 3 (dword bits 29/30) = DIRECTION (SDA bit5, SCL bit6; 1 = output)
 * A line is low only when output with state 0; otherwise released.  A state
 * read gives the driven value while output, else the live bus level.
 */
#define M64_DDC_SDA             (1u << 5)      /* within a byte */
#define M64_DDC_SCL             (1u << 6)
#define M64_DDC_STATE_BYTE      1              /* value / live level */
#define M64_DDC_DIR_BYTE        3              /* direction (1 = output) */

/* RMW one byte of LCD_DATA (byte-wise, exactly like the miniport). */
static void ddc_bit(Mach64TestDev *a, unsigned byteoff, uint8_t mask, int set)
{
    uint64_t addr = a->mmio + M64_REG(M64_LCD_DATA) + byteoff;
    uint8_t v = qtest_readb(a->qts, addr);
    v = set ? (v | mask) : (v & ~mask);
    qtest_writeb(a->qts, addr, v);
}

/* Drive a line to `level` (1 = release/high, 0 = pull low): output+state 0 to
 * pull low, input to release. */
static void ddc_line(Mach64TestDev *a, uint8_t mask, int level)
{
    if (level) {
        ddc_bit(a, M64_DDC_DIR_BYTE, mask, 0);      /* input -> released */
    } else {
        ddc_bit(a, M64_DDC_STATE_BYTE, mask, 0);    /* state 0 */
        ddc_bit(a, M64_DDC_DIR_BYTE, mask, 1);      /* output -> pulls low */
    }
}

static void ddc_drive(Mach64TestDev *a, int scl, int sda)
{
    ddc_line(a, M64_DDC_SCL, scl);
    ddc_line(a, M64_DDC_SDA, sda);
}

static int ddc_sda(Mach64TestDev *a)
{
    ddc_bit(a, M64_DDC_DIR_BYTE, M64_DDC_SDA, 0);   /* input: release SDA */
    return !!(qtest_readb(a->qts, a->mmio + M64_REG(M64_LCD_DATA) +
                          M64_DDC_STATE_BYTE) & M64_DDC_SDA);
}

static void i2c_start(Mach64TestDev *a)
{
    ddc_drive(a, 1, 1);
    ddc_drive(a, 1, 0);         /* SDA falls while SCL high */
    ddc_drive(a, 0, 0);
}

static void i2c_stop(Mach64TestDev *a)
{
    ddc_drive(a, 0, 0);
    ddc_drive(a, 1, 0);
    ddc_drive(a, 1, 1);         /* SDA rises while SCL high */
}

/* Clock one bit out; returns nothing. */
static void i2c_wr_bit(Mach64TestDev *a, int b)
{
    ddc_drive(a, 0, b);
    ddc_drive(a, 1, b);         /* slave samples on the rising edge */
    ddc_drive(a, 0, b);
}

/* Release SDA, clock, sample the line the slave now drives. */
static int i2c_rd_bit(Mach64TestDev *a)
{
    int b;
    ddc_drive(a, 0, 1);
    ddc_drive(a, 1, 1);
    b = ddc_sda(a);
    ddc_drive(a, 0, 1);
    return b;
}

/* Returns 1 if the slave ACKed (pulled SDA low on the 9th clock). */
static int i2c_wr_byte(Mach64TestDev *a, uint8_t v)
{
    int i;
    for (i = 7; i >= 0; i--) {
        i2c_wr_bit(a, (v >> i) & 1);
    }
    return i2c_rd_bit(a) == 0;
}

static uint8_t i2c_rd_byte(Mach64TestDev *a, int ack)
{
    uint8_t v = 0;
    int i;
    for (i = 7; i >= 0; i--) {
        v = (v << 1) | i2c_rd_bit(a);
    }
    i2c_wr_bit(a, ack ? 0 : 1);   /* master ACK (0) to continue, NAK (1) to end */
    return v;
}

static void test_mach64_ddc_edid(void)
{
    Mach64TestDev a;
    uint8_t edid[128];
    unsigned sum = 0;
    int i;

    mach64_dev_open(&a);
    m64_wr(&a, M64_LCD_INDEX, M64_DDC_INDEX_I2C);

    /* DDC2B: address the EDID EEPROM at 0xA0, set the byte offset to 0, then
     * repeated-start into a read of all 128 bytes of block 0. */
    i2c_start(&a);
    g_assert_cmpint(i2c_wr_byte(&a, 0xA0), ==, 1);   /* slave must ACK its addr */
    g_assert_cmpint(i2c_wr_byte(&a, 0x00), ==, 1);   /* offset 0 */
    i2c_start(&a);
    g_assert_cmpint(i2c_wr_byte(&a, 0xA1), ==, 1);   /* read */
    for (i = 0; i < 128; i++) {
        edid[i] = i2c_rd_byte(&a, i < 127);
    }
    i2c_stop(&a);

    /* Fixed EDID 1.x header and a correct block checksum. */
    g_assert_cmphex(edid[0], ==, 0x00);
    g_assert_cmphex(edid[1], ==, 0xff);
    g_assert_cmphex(edid[2], ==, 0xff);
    g_assert_cmphex(edid[7], ==, 0x00);
    for (i = 0; i < 128; i++) {
        sum += edid[i];
    }
    g_assert_cmphex(sum & 0xff, ==, 0);

    mach64_dev_close(&a);
}

int main(int argc, char **argv)
{
    unsigned cpus;

    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/ia64-vpc/acpi-reset-register",
                   test_acpi_reset_register);
    qtest_add_func("/ia64-vpc/vga/int10-rom", test_int10_rom);
    qtest_add_func("/ia64-vpc/vga/int10-vbe", test_int10_vbe);
    qtest_add_func("/ia64-vpc/vga/int10-vbe-std", test_int10_vbe_std);
    qtest_add_func("/ia64-vpc/vga/int10-legacy", test_int10_legacy);
    qtest_add_func("/ia64-vpc/vga/int10-legacy-std",
                   test_int10_legacy_std);
    qtest_add_func("/ia64-vpc/ram/high-remap-above-4g", test_ram_high_remap);
    qtest_add_func("/ia64-vpc/ram/hole-zx1", test_ram_hole_zx1);
    qtest_add_func("/ia64-vpc/firmware-handoff/defaults",
                   test_firmware_handoff_defaults);
    qtest_add_func("/ia64-vpc/firmware-handoff/zx1",
                   test_firmware_handoff_zx1);
    qtest_add_func("/ia64-vpc/lba/agp-capability", test_lba_agp_capability);
    qtest_add_func("/ia64-vpc/sba/ioc-identity", test_sba_ioc_identity);
    qtest_add_func("/ia64-vpc/mercury/config-dispatch",
                   test_mercury_config_dispatch);
    qtest_add_func("/ia64-vpc/ahci/off", test_ahci_off);
    qtest_add_func("/ia64-vpc/ahci/off-default", test_ahci_off_default);
    qtest_add_func("/ia64-vpc/ahci/on", test_ahci_on);
    qtest_add_func("/ia64-vpc/cpu/merced", test_cpu_merced);
    qtest_add_func("/ia64-vpc/cpu/itanium-alias", test_cpu_itanium_alias);
    qtest_add_func("/ia64-vpc/firmware-handoff/i8042-off",
                   test_firmware_handoff_i8042_off);
    qtest_add_func("/ia64-vpc/firmware-handoff/boot-timeout",
                   test_firmware_handoff_boot_timeout);
    for (cpus = 1; cpus <= 8; cpus++) {
        g_autofree char *path =
            g_strdup_printf("/ia64-vpc/smp/topology/%u", cpus);

        qtest_add_data_func(path, GUINT_TO_POINTER(cpus), test_smp_topology);
    }
    qtest_add_func("/ia64-vpc/smp/explicit-topology",
                   test_smp_explicit_topology);
    {
        unsigned i;

        for (i = 0; i < G_N_ELEMENTS(smp_multicore_topologies); i++) {
            const TestSmpMulticoreTopology *topology =
                &smp_multicore_topologies[i];
            g_autofree char *path = g_strdup_printf(
                "/ia64-vpc/smp/multicore/%s", topology->name);

            qtest_add_data_func(path, topology,
                                test_smp_multicore_topology);
        }
    }
    qtest_add_func("/ia64-vpc/smp/reject-full-alat",
                   test_smp_rejects_full_alat);
    qtest_add_func("/ia64-vpc/input/default-per-machine",
                   test_default_input_per_machine);
    qtest_add_func("/ia64-vpc/input/default-usb",
                   test_default_usb_input);
    qtest_add_func("/ia64-vpc/rtc/aligned-read", test_rtc_aligned_read);
    qtest_add_func("/ia64-vpc/nvram/commit-and-restart",
                   test_nvram_commit_and_restart);
    qtest_add_func("/ia64-vpc/pci/default-layout", test_pci_default_layout);
    qtest_add_func("/ia64-vpc/pci/explicit-cmd646-slot0",
                   test_pci_explicit_cmd646_slot0);
    qtest_add_func("/ia64-vpc/pci/ide-on-slot0", test_ide_on_slot0);
    qtest_add_func("/ia64-vpc/network/resources-survive-reset",
                   test_e1000_resources_survive_reset);
    qtest_add_func("/ia64-vpc/network/intx-route",
                   test_e1000_intx_route);
    qtest_add_func("/ia64-vpc/network/packet-transfer",
                   test_e1000_packet_transfer);
    qtest_add_func("/ia64-vpc/lsi/async-nodata-command",
                   test_lsi_async_nodata_command);
    qtest_add_func("/ia64-vpc/lsi/dbms-no-leak",
                   test_lsi_dbms_no_leak);
    qtest_add_func("/ia64-vpc/lsi/memory-move-mmws",
                   test_lsi_memory_move_mmws);
    qtest_add_func("/ia64-vpc/iosapic/level-remote-irr",
                   test_iosapic_level_remote_irr);
    qtest_add_func("/ia64-vpc/iosapic/lowest-priority",
                   test_iosapic_lowest_priority);
    qtest_add_func("/ia64-vpc/iosapic/edge-rte-write-not-a-request",
                   test_iosapic_edge_rte_write_is_not_a_request);
    qtest_add_func("/ia64-vpc/sparse-io/openbus", test_openbus_io_port);
    qtest_add_func("/ia64-vpc/sparse-io/pm-register",
                   test_sparse_io_pm_register);
    qtest_add_func("/ia64-vpc/savevm/platform-state",
                   test_savevm_restores_platform_state);
    qtest_add_func("/ia64-vpc/agp/gxb", test_agp_gxb);
    qtest_add_func("/ia64-vpc/realfw/flash-window", test_realfw_flash_window);
    qtest_add_func("/ia64-vpc/agp/off", test_agp_off);
    qtest_add_func("/ia64-vpc/ati/config-ids", test_ati_config_ids);
    qtest_add_func("/ia64-vpc/ati/pll-regfile", test_ati_pll_regfile);
    qtest_add_func("/ia64-vpc/ati/dac-load-sense", test_ati_dac_load_sense);
    qtest_add_func("/ia64-vpc/ati/mm-index-indirect",
                   test_ati_mm_index_indirect);
    qtest_add_func("/ia64-vpc/ati/rom-bar-tables", test_ati_rom_bar_tables);
    qtest_add_func("/ia64-vpc/ati/2d-solid-fill", test_ati_2d_solid_fill);
    qtest_add_func("/ia64-vpc/ati/2d-fill-24bpp", test_ati_2d_fill_24bpp);
    qtest_add_func("/ia64-vpc/ati/gradient-32bpp", test_ati_gradient_32bpp);
    qtest_add_func("/ia64-vpc/ati/gradient-24bpp", test_ati_gradient_24bpp);
    qtest_add_func("/ia64-vpc/ati/pattern-brush", test_ati_pattern_brush);
    qtest_add_func("/ia64-vpc/ati/rop-src-dst", test_ati_rop_src_dst);
    qtest_add_func("/ia64-vpc/ati/overlap-copy", test_ati_overlap_copy);
    qtest_add_func("/ia64-vpc/ati/sext14-coord", test_ati_sext14_coord);
    qtest_add_func("/ia64-vpc/ati/clr-cmp-clear", test_ati_clr_cmp_clear);
    qtest_add_func("/ia64-vpc/ati/cce-indirect-buffer",
                   test_ati_cce_indirect_buffer);
    qtest_add_func("/ia64-vpc/agp/gart-dma", test_agp_gart_dma);
    qtest_add_func("/ia64-vpc/mach64/ids", test_mach64_ids);
    qtest_add_func("/ia64-vpc/mach64/2d-solid-fill",
                   test_mach64_2d_solid_fill);
    qtest_add_func("/ia64-vpc/mach64/ddc-edid", test_mach64_ddc_edid);
    qtest_add_func("/ia64-vpc/eepro100/csr-windows",
                   test_eepro100_csr_windows);
    qtest_add_func("/ia64-vpc/eepro100/eeprom-map",
                   test_eepro100_eeprom_map);
    qtest_add_func("/ia64-vpc/pci/460gx-no-chipset-bus",
                   test_460gx_no_chipset_bus);
    qtest_add_func("/ia64-vpc/pci/460gx-platform-identities",
                   test_460gx_platform_identities);
    qtest_add_func("/ia64-vpc/pci/460gx-config-ports", test_460gx_config_ports);
    qtest_add_func("/ia64-vpc/pci/460gx-sac-aperture", test_460gx_sac_aperture);
    qtest_add_func("/ia64-vpc/pci/460gx-smbus-hwmon", test_460gx_smbus_hwmon);
    qtest_add_func("/ia64-vpc/pci/460gx-south-bridge-rtc-banks",
                   test_460gx_south_bridge_rtc_banks);
    qtest_add_func("/ia64-vpc/pci/460gx-south-bridge-timer",
                   test_460gx_south_bridge_timer);
    qtest_add_func("/ia64-vpc/pci/460gx-south-bridge-pic",
                   test_460gx_south_bridge_pic);
    qtest_add_func("/ia64-vpc/pci/460gx-root-window-containment",
                   test_460gx_root_window_containment);
    qtest_add_func("/ia64-vpc/pci/460gx-expander-roots",
                   test_460gx_expander_roots);
    qtest_add_func("/ia64-vpc/iosapic/version-per-machine",
                   test_iosapic_version_per_machine);
    qtest_add_func("/ia64-vpc/realfw/chipset-identity",
                   test_realfw_chipset_identity);
    qtest_add_func("/ia64-vpc/scsi/isp12160-mailbox",
                   test_isp12160_mailbox);
    qtest_add_func("/ia64-vpc/scsi/isp12160-firmware-checksum",
                   test_isp12160_firmware_checksum);
    qtest_add_func("/ia64-vpc/audio/cs4281-codec",
                   test_cs4281_codec_access);
    qtest_add_func("/ia64-vpc/ohci/port-resume", test_ohci_port_resume);
    qtest_add_func("/ia64-vpc/ohci/controller-resume",
                   test_ohci_controller_resume);
    qtest_add_func("/ia64-vpc/ohci/reset-suspended-port",
                   test_ohci_reset_suspended_port);

    return g_test_run();
}
