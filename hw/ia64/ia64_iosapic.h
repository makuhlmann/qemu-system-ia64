/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_IA64_IOSAPIC_H
#define HW_IA64_IOSAPIC_H

#include "hw/core/sysbus.h"
#include "hw/core/irq.h"

#define TYPE_IA64_IOSAPIC "ia64-iosapic"
OBJECT_DECLARE_SIMPLE_TYPE(IA64IOSapicState, IA64_IOSAPIC)

/*
 * Redirection-table size.  The array is sized for the largest part we model,
 * the 460GX Programmable Interrupt Device's 64 inputs; how many a given
 * machine actually presents, and the version byte it reports, come from the
 * "num-pins" and "version" properties.  IA64_IOSAPIC_NUM_PINS is the default:
 * sixteen ISA inputs plus one four-line PCI root, and four spare.
 */
#define IA64_IOSAPIC_MAX_PINS 64
#define IA64_IOSAPIC_NUM_PINS 24
#define IA64_IOSAPIC_VERSION  0x11
/*
 * Which part's register face the "face" property gives: the zx1 machine's
 * stand-in at FEC0_0000, the 460GX Programmable Interrupt Device, or the
 * I/O SAPIC of a zx1 ioa.
 */
#define IA64_IOSAPIC_FACE_STANDIN  0
#define IA64_IOSAPIC_FACE_PID      1
#define IA64_IOSAPIC_FACE_IOA      2
/*
 * The platform's target for a redirectable (lowest-priority) interrupt:
 * sets *id and *eid and returns true, or returns false to leave the
 * entry's own destination.
 */
typedef bool (*IA64IOSapicRedirect)(void *opaque, uint8_t *id, uint8_t *eid);
void ia64_iosapic_set_redirect(DeviceState *dev, IA64IOSapicRedirect redirect,
                               void *opaque);
/* The 460GX PID: 64 inputs, IOSAPIC version 2.1. */
#define IA64_IOSAPIC_460GX_PINS    64
#define IA64_IOSAPIC_460GX_VERSION 0x21

#endif
