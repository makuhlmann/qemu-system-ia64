/*
 * QLogic ISP12160 host adapter: firmware-side transport.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef IA64_FIRMWARE_FW_ISP12160_H
#define IA64_FIRMWARE_FW_ISP12160_H

#include "fw-base.h"

/*
 * Bring the adapter up: find it on the bus, load and start its firmware,
 * and initialise the request and response rings.  Idempotent; returns
 * false when no adapter is present or the sequence fails, in which case
 * isp12160_present() stays false and the caller falls back to another
 * transport.
 */
BOOLEAN isp12160_initialise(void);
BOOLEAN isp12160_present(void);
UINT64 isp12160_mmio_base(void);

/*
 * Run one CDB against a target and return its SCSI status byte.  Data
 * moves in the direction the caller asks for; DataLength may be zero.
 * Returns false when the adapter did not complete the request at all,
 * which is distinct from a request that completed with a check condition.
 */
BOOLEAN isp12160_command(UINT8 Target, const UINT8 *Cdb, UINTN CdbLength,
                         UINT8 *Data, UINT32 DataLength, BOOLEAN ToDevice,
                         UINT8 *ScsiStatus);

#endif /* IA64_FIRMWARE_FW_ISP12160_H */
