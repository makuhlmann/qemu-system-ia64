/*
 * LSI Fusion-MPT host adapter (53C1030): firmware-side transport.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef IA64_FIRMWARE_FW_MPT_H
#define IA64_FIRMWARE_FW_MPT_H

#include "fw-base.h"
#include "fw-storage.h"

/* I/O controllers the firmware drives: two two-function adapters. */
#define MPT_MAX_IOCS    4U

/*
 * Bring every 53C1030 function on the bus to the operational state, in bus
 * order.  Idempotent; returns the number of controllers that came up.
 */
UINTN mpt_initialise(void);
UINTN mpt_ioc_count(void);
UINT64 mpt_mmio_base(UINTN Ioc);
BOOLEAN mpt_location(UINTN Ioc, PCI_DEVICE_LOCATION *Location);

/*
 * Run one CDB against a target of controller Ioc and return its SCSI status
 * byte.  Returns false when the controller did not complete the request,
 * including a target that is not there, which is distinct from a request
 * that completed with a check condition.
 */
BOOLEAN mpt_command(UINTN Ioc, UINT8 Target, const UINT8 *Cdb,
                    UINTN CdbLength, UINT8 *Data, UINT32 DataLength,
                    BOOLEAN ToDevice, UINT8 *ScsiStatus);

/* The same, with the outcome the SCSI Pass Thru reports. */
typedef enum {
    MptResultGood,
    MptResultTargetStatus,      /* completed; *ScsiStatus is not GOOD */
    MptResultNoDevice,          /* no target at that ID */
    MptResultTimeout,
    MptResultError,
} MPT_RESULT;

MPT_RESULT mpt_execute(UINTN Ioc, UINT8 Target, const UINT8 *Cdb,
                       UINTN CdbLength, UINT8 *Data, UINT32 DataLength,
                       BOOLEAN ToDevice, UINT8 *ScsiStatus);

/*
 * Return every controller to the ready state before the OS takes over, so
 * that none still owns reply frames in firmware memory.
 */
void mpt_stop_all(void);

#endif /* IA64_FIRMWARE_FW_MPT_H */
