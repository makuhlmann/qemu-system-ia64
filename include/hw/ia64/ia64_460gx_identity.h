/*
 * Intel 460GX platform devices that are present in PCI configuration space
 * but carry no software-visible behaviour of their own.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_IA64_IA64_460GX_IDENTITY_H
#define HW_IA64_IA64_460GX_IDENTITY_H

/*
 * The Intel 683053 Programmable Interrupt Device.  Its function -- the
 * 64-input SAPIC message block at IA64_IOSAPIC_BASE -- is modelled by
 * TYPE_IA64_IOSAPIC; this is the same chip's face in configuration space,
 * which a 460GX platform carries on the compatibility bus (SSDM 1.7.2).
 */
#define TYPE_IA64_460GX_PID  "ia64-460gx-pid"

/*
 * The Intel 82466GX Integrated Hot-Plug Controller, one per WXB PCI bus.
 * The i2000's expansion slots are hot-plug capable; nothing here implements
 * that, so the controller is present but idle.
 */
#define TYPE_IA64_460GX_IHPC "ia64-460gx-ihpc"

#endif /* HW_IA64_IA64_460GX_IDENTITY_H */
