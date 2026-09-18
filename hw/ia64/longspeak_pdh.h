/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Longs Peak (HP zx1 board) PDH devices below the flash.
 */

#ifndef HW_IA64_LONGSPEAK_PDH_H
#define HW_IA64_LONGSPEAK_PDH_H

#include "hw/ia64/ia64_vpc_abi.h"
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
    MemoryRegion container;        /* holds mr, and any device in the block */
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
    uint8_t semaphore;             /* bit 0 held, bits 7:1 holder id */
    uint64_t reg[IA64_PDH_DILLON_REGS];        /* FF5F_0000 - FF5F_0090 */
    uint64_t control;              /* FF5F_1000 */
    uint64_t scratch1;             /* FF5F_1038 */
    uint64_t misc;                 /* FF5F_31C0 */

    /* The two PDH UARTs, FF5E_0000 and FF5E_2000. */
    DeviceState *uart[IA64_PDH_UARTS];
};

/* sysbus MMIO indexes */
#define LONGSPEAK_PDH_MMIO_NVM     0
#define LONGSPEAK_PDH_MMIO_SRAM    1
#define LONGSPEAK_PDH_MMIO_BLOCK0  2

#endif /* HW_IA64_LONGSPEAK_PDH_H */
