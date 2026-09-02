/*
 * Microsoft OS 1.0 descriptor tests
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"

#include "hw/usb/usb.h"
#include "hw/usb/desc.h"
#include "qemu/bswap.h"

static void assert_utf16le_ascii(const uint8_t *data, const char *text)
{
    size_t i;

    for (i = 0; text[i]; i++) {
        g_assert_cmphex(lduw_le_p(data + 2 * i), ==, (uint8_t)text[i]);
    }
    g_assert_cmphex(lduw_le_p(data + 2 * i), ==, 0);
}

static void test_selective_suspend(void)
{
    static const USBDescMSOS msos = {
        .SelectiveSuspendEnabled = true,
    };
    static const USBDesc desc = {
        .msos = &msos,
    };
    USBPacket packet = { 0 };
    uint8_t data[128] = { 0 };
    const uint8_t *prop = data + 10;

    g_assert_cmpint(usb_desc_msos(&desc, &packet, 0x0005,
                                 data, sizeof(data)), ==, 0);
    g_assert_cmpuint(packet.actual_length, ==, 76);
    g_assert_cmpuint(ldl_le_p(data), ==, 76);
    g_assert_cmphex(lduw_le_p(data + 4), ==, 0x0100);
    g_assert_cmphex(lduw_le_p(data + 6), ==, 0x0005);
    g_assert_cmpuint(lduw_le_p(data + 8), ==, 1);

    g_assert_cmpuint(ldl_le_p(prop), ==, 66);
    g_assert_cmpuint(ldl_le_p(prop + 4), ==, 4);
    g_assert_cmpuint(lduw_le_p(prop + 8), ==, 48);
    assert_utf16le_ascii(prop + 10, "SelectiveSuspendEnabled");
    g_assert_cmpuint(ldl_le_p(prop + 58), ==, 4);
    g_assert_cmpuint(ldl_le_p(prop + 62), ==, 1);
}

static void test_label(void)
{
    static const USBDescMSOS msos = {
        .Label = L"USB",
    };
    static const USBDesc desc = {
        .msos = &msos,
    };
    USBPacket packet = { 0 };
    uint8_t data[128] = { 0 };
    const uint8_t *prop = data + 10;

    g_assert_cmpint(usb_desc_msos(&desc, &packet, 0x0005,
                                 data, sizeof(data)), ==, 0);
    g_assert_cmpuint(packet.actual_length, ==, 44);
    g_assert_cmpuint(ldl_le_p(data), ==, 44);
    g_assert_cmpuint(lduw_le_p(data + 8), ==, 1);

    g_assert_cmpuint(ldl_le_p(prop), ==, 34);
    g_assert_cmpuint(ldl_le_p(prop + 4), ==, 1);
    g_assert_cmpuint(lduw_le_p(prop + 8), ==, 12);
    assert_utf16le_ascii(prop + 10, "Label");
    g_assert_cmpuint(ldl_le_p(prop + 22), ==, 8);
    assert_utf16le_ascii(prop + 26, "USB");
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/usb/msos/selective-suspend", test_selective_suspend);
    g_test_add_func("/usb/msos/label", test_label);
    return g_test_run();
}
