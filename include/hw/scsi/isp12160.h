/*
 * QLogic ISP12160 mailbox, queue, and SCSI models
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_SCSI_ISP12160_H
#define HW_SCSI_ISP12160_H

#include "hw/scsi/isp12160_abi.h"
#include "qom/object.h"

#define TYPE_ISP12160_MAILBOX "isp12160-mailbox"
#define TYPE_ISP12160_QUEUE "isp12160-queue"
#define TYPE_ISP12160_SCSI "isp12160-scsi"
OBJECT_DECLARE_SIMPLE_TYPE(ISP12160State, ISP12160_MAILBOX)

/* Activation data contains a version, RISC address, and four token words. */
bool isp12160_qemu_activation_generate(uint16_t variant, uint8_t *activation,
                                       size_t length);
bool isp12160_qemu_activation_validate(uint16_t variant,
                                       const uint8_t *activation,
                                       size_t length);

#endif /* HW_SCSI_ISP12160_H */
