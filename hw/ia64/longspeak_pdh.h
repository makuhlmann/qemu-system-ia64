/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Longs Peak (HP zx1 board) PDH devices below the flash.
 */

#ifndef HW_IA64_LONGSPEAK_PDH_H
#define HW_IA64_LONGSPEAK_PDH_H

#include "hw/core/sysbus.h"
#include "system/memory.h"
#include "qom/object.h"

#define TYPE_LONGSPEAK_PDH "longspeak-pdh"
OBJECT_DECLARE_SIMPLE_TYPE(LongspeakPDHState, LONGSPEAK_PDH)

/* The register blocks, in sysbus MMIO order after the NVM and the SRAM. */
typedef enum LongspeakPDHBlockId {
    LONGSPEAK_PDH_DEV5B,
    LONGSPEAK_PDH_PRESENCE_BLOCK,
    LONGSPEAK_PDH_UART_BLOCK,
    LONGSPEAK_PDH_DILLON_BLOCK,
    LONGSPEAK_PDH_BLOCKS,
} LongspeakPDHBlockId;

typedef struct LongspeakPDHBlock {
    MemoryRegion mr;
    LongspeakPDHState *pdh;
    LongspeakPDHBlockId id;
    hwaddr base;
    /* Offsets already reported as unimplemented, one bit per offset. */
    unsigned long *unimp_read;
    unsigned long *unimp_write;
} LongspeakPDHBlock;

struct LongspeakPDHState {
    SysBusDevice parent_obj;

    MemoryRegion nvm;
    MemoryRegion sram;
    LongspeakPDHBlock block[LONGSPEAK_PDH_BLOCKS];

    uint32_t sockets;              /* processors present, from -smp */

    uint8_t post;                  /* FF5C_0018 */
    uint64_t scratch0;             /* FF5F_0020 */
    uint64_t checkin;              /* FF5F_0068 */
    uint8_t semaphore;             /* bit 0 held, bits 7:1 holder id */
};

/* sysbus MMIO indexes */
#define LONGSPEAK_PDH_MMIO_NVM     0
#define LONGSPEAK_PDH_MMIO_SRAM    1
#define LONGSPEAK_PDH_MMIO_BLOCK0  2

#endif /* HW_IA64_LONGSPEAK_PDH_H */
