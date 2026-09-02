/*
 * ISP12160 IOCB parser
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_SCSI_ISP12160_IOCB_H
#define HW_SCSI_ISP12160_IOCB_H

#include "hw/scsi/isp12160_abi.h"
#include "qapi/error.h"

typedef enum ISP12160IOCBDirection {
    ISP12160_IOCB_DIRECTION_NONE,
    ISP12160_IOCB_DIRECTION_FROM_DEVICE,
    ISP12160_IOCB_DIRECTION_TO_DEVICE,
    ISP12160_IOCB_DIRECTION_UNKNOWN,
} ISP12160IOCBDirection;

typedef struct ISP12160IOCBSegment {
    uint64_t address;
    uint32_t length;
} ISP12160IOCBSegment;

typedef struct ISP12160IOCBCommand {
    uint32_t handle;
    uint16_t timeout;
    uint16_t control_flags;
    uint16_t segment_count;
    uint8_t entry_count;
    uint8_t channel;
    uint8_t target;
    uint8_t lun;
    uint8_t cdb_length;
    uint8_t cdb[ISP12160_IOCB_CDB_BYTES];
    ISP12160IOCBDirection direction;
    uint64_t transfer_length;
} ISP12160IOCBCommand;

typedef struct ISP12160IOCBStatus {
    uint32_t handle;
    uint32_t residual_length;
    uint16_t scsi_status;
    uint16_t completion_status;
    uint16_t state_flags;
    uint16_t status_flags;
    uint16_t time;
    uint8_t sense_length;
    uint8_t sense[ISP12160_IOCB_SENSE_BYTES];
} ISP12160IOCBStatus;

/* Parse a command chain without changing outputs on failure. */
bool isp12160_iocb_parse_32(const uint8_t *entries, size_t entry_count,
                            ISP12160IOCBCommand *command,
                            ISP12160IOCBSegment *segments,
                            size_t segment_capacity, Error **errp);

bool isp12160_iocb_parse_a64(const uint8_t *entries, size_t entry_count,
                             ISP12160IOCBCommand *command,
                             ISP12160IOCBSegment *segments,
                             size_t segment_capacity, Error **errp);

/* Build one 64-byte status entry without performing DMA. */
bool isp12160_iocb_build_status(uint8_t *entry, size_t entry_size,
                                const ISP12160IOCBStatus *status,
                                Error **errp);

#endif /* HW_SCSI_ISP12160_IOCB_H */
