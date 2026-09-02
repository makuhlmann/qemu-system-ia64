/*
 * Cirrus Logic CS4281 PCI audio controller
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_AUDIO_CS4281_H
#define HW_AUDIO_CS4281_H

#include "hw/pci/pci_device.h"

#define TYPE_CS4281 "cs4281"
OBJECT_DECLARE_SIMPLE_TYPE(CS4281State, CS4281)

#define CS4281_BA0_SIZE 0x1000
#define CS4281_BA1_SIZE 0x10000

#endif
