/*
 * ISP12160 activation-token helpers
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"

#include "hw/scsi/isp12160.h"
#include "qemu/bswap.h"

static bool isp12160_qemu_activation_parameters(uint16_t variant,
                                                uint8_t version[3],
                                                uint16_t token[4])
{
    switch (variant) {
    case ISP12160_VARIANT_MAILBOX:
        version[0] = 0;
        version[1] = 0;
        version[2] = 1;
        token[0] = ISP12160_QEMU_MAILBOX_TOKEN_WORD0;
        token[1] = ISP12160_QEMU_MAILBOX_TOKEN_WORD1;
        token[2] = ISP12160_QEMU_MAILBOX_TOKEN_WORD2;
        token[3] = ISP12160_QEMU_MAILBOX_TOKEN_WORD3;
        return true;

    case ISP12160_VARIANT_QUEUE:
        version[0] = 0;
        version[1] = 1;
        version[2] = 0;
        token[0] = ISP12160_QEMU_QUEUE_TOKEN_WORD0;
        token[1] = ISP12160_QEMU_QUEUE_TOKEN_WORD1;
        token[2] = ISP12160_QEMU_QUEUE_TOKEN_WORD2;
        token[3] = ISP12160_QEMU_QUEUE_TOKEN_WORD3;
        return true;

    case ISP12160_VARIANT_SCSI:
        version[0] = 0;
        version[1] = 2;
        version[2] = 0;
        token[0] = ISP12160_QEMU_SCSI_TOKEN_WORD0;
        token[1] = ISP12160_QEMU_SCSI_TOKEN_WORD1;
        token[2] = ISP12160_QEMU_SCSI_TOKEN_WORD2;
        token[3] = ISP12160_QEMU_SCSI_TOKEN_WORD3;
        return true;

    default:
        return false;
    }
}

bool isp12160_qemu_activation_generate(uint16_t variant, uint8_t *activation,
                                       size_t length)
{
    uint8_t version[3];
    uint16_t token[4];
    unsigned int i;

    if (!activation || length != ISP12160_QEMU_ACTIVATION_BYTES ||
        !isp12160_qemu_activation_parameters(variant, version, token)) {
        return false;
    }

    activation[0] = version[0];
    activation[1] = version[1];
    activation[2] = version[2];
    activation[3] = 0;
    stw_le_p(activation + 4, ISP12160_QEMU_ACTIVATION_RISC_ADDR);
    for (i = 0; i < ARRAY_SIZE(token); i++) {
        stw_le_p(activation + ISP12160_QEMU_ACTIVATION_HEADER_BYTES + i * 2,
                 token[i]);
    }
    return true;
}

bool isp12160_qemu_activation_validate(uint16_t variant,
                                       const uint8_t *activation,
                                       size_t length)
{
    uint8_t expected[ISP12160_QEMU_ACTIVATION_BYTES];

    return activation && length == sizeof(expected) &&
           isp12160_qemu_activation_generate(variant, expected,
                                             sizeof(expected)) &&
           !memcmp(activation, expected, sizeof(expected));
}
