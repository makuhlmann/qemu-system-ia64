/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The BMC of the Longs Peak (HP zx1 board).
 *
 * HP's firmware keeps copies of its settings in the BMC, as "tokens": the
 * console devices, the boot timeout, the language and more, each a small
 * value under a 16-bit number.  Token 0x500 is the BMC's own and reads 18
 * once the firmware has loaded the others; a refused read of it halts POST
 * with 0x0012A0 "error reading bmc first boot token", and any other value is
 * a "bmc first boot", on which the firmware sets the clock to 1998-01-01
 * (FFF5AB1C calls FFF3E290) and loads the tokens again.  A BMC that forgets
 * them is therefore a new BMC on every power-on.
 *
 * The commands are HP's own -- IPMI network function 0x32 carries no defined
 * payload (IPMI v2.0 table 5-1) -- and the firmware's trace strings name
 * them.  Each request starts with the token number, little-endian.  Both the
 * reader (FFF53640) and the writer (FFF53F60) take the size of the value from
 * GET_TOKEN_INFO, not from their caller, and move a value that does not fit
 * in one message in parts.
 *
 * Each processor has an information area of its own behind the BMC, in FRU
 * device 0x20 + n, with a second device at 0x24 + n.  The area is not an IPMI
 * FRU but HP's own 128-byte record: the firmware reads byte 0x16 first and
 * gives up on a zero ("WARNING: No SMBUS Info for CPU n"), otherwise reads
 * the record and checks it.  Refusing the device is what it calls POST
 * 0x0002E5 "access error on processor info area".  The devices are modelled
 * unprogrammed, a state the firmware handles ("WARNING: No Programmed
 * Processors intalled, bypassing ratio set!"); the record itself has no
 * published layout.
 *
 * The DIMM slots are FRU devices too.  The firmware picks the slot table for
 * the board from the product id in the BMC's own FRU (FFF62880 accepts 257 to
 * 260 for the twelve-slot board and 261 for the four-slot one; FFF5A110 takes
 * the id from bytes 115 to 118 of the product area), then reads a JEDEC SPD
 * from the FRU device of each slot (FFF96CD8 gives the twelve devices) and
 * asks HP's own storage command 0xd0 about each of them.  Without the SPD the
 * firmware reports "SPD found no memory DIMMs" and halts the cell.
 *
 * Command 0xd0 answers four bytes for a populated slot.  The firmware keeps
 * them with its memory configuration in the NVM (FFEE5660 stores them at
 * entry +24) and compares them at the next boot (FFEE5240, FFF45100): the
 * same bytes in every slot make it reuse the saved configuration ("memory
 * unchanged--use saved config from NVM"), so they identify the module.  No HP
 * document names the value; the model answers the module's serial number
 * (SPD bytes 95-98), made from the slot and the module size.
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/module.h"
#include "qemu/units.h"
#include "qemu/cutils.h"
#include "qemu/timer.h"
#include "qemu/uuid.h"
#include "system/system.h"
#include "hw/core/boards.h"
#include "hw/ipmi/ipmi.h"
#include "qom/object.h"
#include "hw/ia64/ia64_vpc_abi.h"
#include "longspeak_pdh.h"
#include "trace.h"

#define LONGSPEAK_BMC_NETFN_TOKEN       0x32
#define LONGSPEAK_BMC_TOKEN_INFO        0x01
#define LONGSPEAK_BMC_TOKEN_READ        0x02
#define LONGSPEAK_BMC_TOKEN_WRITE       0x03
#define LONGSPEAK_BMC_TOKEN_READ_PART   0x08
#define LONGSPEAK_BMC_TOKEN_WRITE_PART  0x09
#define LONGSPEAK_BMC_TOKEN_VERIFY      0x0a
#define LONGSPEAK_BMC_TOKEN_UPDATE      0x0b
#define LONGSPEAK_BMC_CC_TOKEN_CHECKSUM 0x70

#define LONGSPEAK_BMC_NETFN_STORAGE 0x0a
#define LONGSPEAK_BMC_CMD_FRU_INFO  0x10
#define LONGSPEAK_BMC_CMD_FRU_READ  0x11
#define LONGSPEAK_BMC_CMD_SDR_TIME  0x28
#define LONGSPEAK_BMC_CMD_SEL_INFO      0x40
#define LONGSPEAK_BMC_CMD_SEL_ALLOC     0x41
#define LONGSPEAK_BMC_CMD_SEL_RESERVE   0x42
#define LONGSPEAK_BMC_CMD_SEL_GET       0x43
#define LONGSPEAK_BMC_CMD_SEL_ADD       0x44
#define LONGSPEAK_BMC_CMD_SEL_CLEAR     0x47
#define LONGSPEAK_BMC_CMD_SEL_GET_TIME  0x48
#define LONGSPEAK_BMC_CMD_SEL_SET_TIME  0x49

#define LONGSPEAK_BMC_NETFN_CHASSIS         0x00
#define LONGSPEAK_BMC_CMD_CHASSIS_CAPS      0x00
#define LONGSPEAK_BMC_CMD_CHASSIS_STATUS    0x01
#define LONGSPEAK_BMC_CMD_RESTART_CAUSE     0x07
#define LONGSPEAK_BMC_CMD_BOOT_OPTIONS      0x09
#define LONGSPEAK_BMC_CMD_POH               0x0f
#define LONGSPEAK_BMC_NETFN_SENSOR          0x04
#define LONGSPEAK_BMC_CMD_EVENT_RECEIVER    0x01
#define LONGSPEAK_BMC_CMD_PLATFORM_EVENT    0x02
#define LONGSPEAK_BMC_CMD_PEF_CAPS          0x10
#define LONGSPEAK_BMC_NETFN_APP             0x06
#define LONGSPEAK_BMC_CMD_DEVICE_ID         0x01
#define LONGSPEAK_BMC_CMD_DEVICE_GUID       0x08
#define LONGSPEAK_BMC_CMD_CHANNEL_INFO      0x42

/* The two sets of per-processor devices are four apart, so four each. */
#define LONGSPEAK_BMC_FRU_FIRST     0x20
#define LONGSPEAK_BMC_FRU_DEVICES   8
#define LONGSPEAK_BMC_FRU_SIZE      128

typedef struct LongspeakBmcClass {
    IPMIBmcClass parent_class;

    void (*parent_handle_command)(IPMIBmc *s, uint8_t *cmd,
                                  unsigned int cmd_len,
                                  unsigned int max_cmd_len, uint8_t msg_id);
} LongspeakBmcClass;

DECLARE_CLASS_CHECKERS(LongspeakBmcClass, LONGSPEAK_BMC, TYPE_LONGSPEAK_BMC)

/*
 * HP's own storage command: the firmware asks it about every FRU device it
 * finds, and takes an answer for "present" (FFF45100 sends it, FFEE5240
 * compares the answer with the saved configuration).
 */
#define LONGSPEAK_BMC_CMD_FRU_STATUS 0xd0

/*
 * FRU device 0 in the rx2600's layout (rx2600 capture 2026-10-04, BMC-3):
 * 512 bytes, an internal-use area, the chassis, board and product areas, and
 * 0xFF after them.
 */
#define LONGSPEAK_BMC_FRU0          0x00
#define LONGSPEAK_BMC_FRU0_SIZE     512
#define LONGSPEAK_BMC_INTERNAL_OFF  8
#define LONGSPEAK_BMC_CHASSIS_OFF   72
#define LONGSPEAK_BMC_CHASSIS_SIZE  32
#define LONGSPEAK_BMC_BOARD_OFF     104
#define LONGSPEAK_BMC_BOARD_SIZE    120
#define LONGSPEAK_BMC_PRODUCT_OFF   224
#define LONGSPEAK_BMC_PRODUCT_SIZE  128

#define LONGSPEAK_BMC_SPD_SIZE      256

/*
 * FRU device 5, the I/O riser.  SAL_B reads its first byte and takes the
 * board with the I/O backplane when the BMC answers: ropes 0 to 4 (4
 * double-wide) and 6, the rx2600's; without it, the core I/O ropes 0 and 1
 * alone (FFEAB2F0, tables from FFF7_F160).  A byte read from device 6 would
 * add rope 7.  On the rx2600 device 6 has an area of 256 bytes that cannot
 * be read, cc CEh [inferred: the MP card's, which that machine lacks], and
 * the riser 256 bytes: a zero internal-use area at 8 and a board area at 72
 * whose length byte says 128 but whose fields fill 120 as on the system
 * board (rx2600 capture 2026-10-04, BMC-3 and FRU 5).
 */
#define LONGSPEAK_BMC_FRU_IO        0x05
#define LONGSPEAK_BMC_FRU_IO_SIZE   256
#define LONGSPEAK_BMC_FRU_IO_BOARD_OFF 72
#define LONGSPEAK_BMC_FRU_IO_BOARD_UNITS 16
#define LONGSPEAK_BMC_FRU_IO_FILE_ID 0x10
#define LONGSPEAK_BMC_FRU_MP        0x06
#define LONGSPEAK_BMC_FRU_MP_SIZE   256
#define LONGSPEAK_BMC_CC_NO_RESPONSE 0xce

/* The DIMM slots 0A, 0B, 1A ... 5B and the FRU device of each (FFF96CD8). */
static const uint8_t longspeak_bmc_dimm_dev[] = {
    0x80, 0x81, 0x88, 0x89, 0x82, 0x83, 0x8a, 0x8b, 0x84, 0x85, 0x8c, 0x8d
};

/*
 * The firmware keys its table of supported modules on the SPD geometry
 * (FF45_E2C8: byte 4 + byte 5 << 12 + byte 3 << 4 + byte 17 << 8), and
 * accepts these three with eleven column addresses and four banks.
 */
typedef struct LongspeakBmcDimm {
    uint64_t size;
    uint8_t rows;
    uint8_t ranks;
    uint8_t density;            /* SPD byte 31, the rank size in 4 MiB */
} LongspeakBmcDimm;

static const LongspeakBmcDimm longspeak_bmc_modules[] = {
    { 1 * GiB,   13, 2, 0x80 },
    { 512 * MiB, 13, 1, 0x80 },
    { 256 * MiB, 12, 1, 0x40 },
};

/* A text field of a fixed width, padded with NULs as on the rx2600. */
static uint8_t *longspeak_bmc_fru_padded(uint8_t *p, const char *text,
                                         unsigned int width)
{
    *p++ = 0xc0 | width;        /* 8-bit ASCII (IPMI FRU sec 13) */
    memset(p, 0, width);
    memcpy(p, text, MIN(strlen(text), width));
    return p + width;
}

typedef struct LongspeakBmcFruField {
    const char *text;
    uint8_t width;
} LongspeakBmcFruField;

static uint8_t *longspeak_bmc_fru_fields(uint8_t *p,
                                         const LongspeakBmcFruField *f,
                                         unsigned int n)
{
    unsigned int i;

    for (i = 0; i < n; i++) {
        p = longspeak_bmc_fru_padded(p, f[i].text, f[i].width);
    }
    return p;
}

/*
 * HP gives every field of its areas a fixed width, and SAL_B reads them at
 * fixed offsets (its dumps FFF5BBC0, FFF5C370; SMBIOS type 1 takes the
 * product name from offset 6, with the NULs as spaces).  The type/length
 * bytes stay right, so an IPMI FRU 1.0 parser reads the same fields.
 *
 * Chassis: part number 11, serial number 12.  Board: manufacturer 10,
 * product 32, serial number 16, part number 11, the FRU file id, revision 8,
 * engineering date 4, artwork 2, then 16 binary bytes ("Fru Info").
 * Product: manufacturer 2 ("hp" on HP's boards), product 32, part number 11,
 * version 6, serial number 20, asset tag 32, the FRU file id at 112 and the
 * product id, a custom field, at 114 (its value at 115 to 118, FFF5A110).
 * The FRU file id is one BCD-plus byte, 11h on the rx2600.  The serial
 * and part numbers are the model's own.
 */
static const LongspeakBmcFruField longspeak_bmc_chassis_fields[] = {
    { "", 11 }, { "", 12 },
};

static const LongspeakBmcFruField longspeak_bmc_board_fields[] = {
    { "QE", 10 }, { "Longs Peak", 32 }, { "", 16 }, { "", 11 },
};

static const LongspeakBmcFruField longspeak_bmc_board_late_fields[] = {
    { "", 8 }, { "", 4 }, { "", 2 },
};

static const LongspeakBmcFruField longspeak_bmc_riser_fields[] = {
    { "QE", 10 }, { "Longs Peak I/O riser", 32 }, { "", 16 }, { "", 11 },
};

static const LongspeakBmcFruField longspeak_bmc_product_fields[] = {
    { "QE", 2 }, { "Longs Peak", 32 }, { "QE-LP1", 11 }, { "", 6 },
    { "QE00000001", 20 }, { "", 32 },
};

#define LONGSPEAK_BMC_CHASSIS_RACK      0x17
#define LONGSPEAK_BMC_FRU_FILE_TL       0x41
#define LONGSPEAK_BMC_FRU_FILE_ID       0x11
#define LONGSPEAK_BMC_BOARD_INFO_OFF    98
#define LONGSPEAK_BMC_BOARD_INFO_SIZE   16
#define LONGSPEAK_BMC_PRODUCT_FILE_ID   112

static void longspeak_bmc_fru_sum(uint8_t *area, unsigned int size)
{
    uint8_t sum = 0;
    unsigned int i;

    for (i = 0; i < size - 1; i++) {
        sum += area[i];
    }
    area[size - 1] = -sum;
}

/* The board fields fill 120 bytes whatever the length byte says. */
static void longspeak_bmc_board_area(uint8_t *area, uint8_t units,
                                     const LongspeakBmcFruField *fields,
                                     uint8_t file_id)
{
    uint8_t *p;

    area[0] = 0x01;             /* language and date 0 */
    area[1] = units;
    p = longspeak_bmc_fru_fields(area + 6, fields, 4);
    *p++ = LONGSPEAK_BMC_FRU_FILE_TL;
    *p++ = file_id;
    p = longspeak_bmc_fru_fields(p, longspeak_bmc_board_late_fields,
                                 ARRAY_SIZE(longspeak_bmc_board_late_fields));
    assert(p == area + LONGSPEAK_BMC_BOARD_INFO_OFF);
    *p = LONGSPEAK_BMC_BOARD_INFO_SIZE;     /* binary */
    p += 1 + LONGSPEAK_BMC_BOARD_INFO_SIZE;
    *p = 0xc1;
    longspeak_bmc_fru_sum(area, LONGSPEAK_BMC_BOARD_SIZE);
}

static void longspeak_bmc_riser_fru(uint8_t *fru)
{
    QEMU_BUILD_BUG_ON(LONGSPEAK_BMC_FRU_IO_BOARD_OFF +
                      LONGSPEAK_BMC_FRU_IO_BOARD_UNITS * 8 >
                      LONGSPEAK_BMC_FRU_IO_SIZE);

    memset(fru, 0, LONGSPEAK_BMC_FRU_IO_SIZE);
    fru[0] = 0x01;
    fru[1] = LONGSPEAK_BMC_INTERNAL_OFF / 8;
    fru[3] = LONGSPEAK_BMC_FRU_IO_BOARD_OFF / 8;
    longspeak_bmc_fru_sum(fru, 8);
    longspeak_bmc_board_area(fru + LONGSPEAK_BMC_FRU_IO_BOARD_OFF,
                             LONGSPEAK_BMC_FRU_IO_BOARD_UNITS,
                             longspeak_bmc_riser_fields,
                             LONGSPEAK_BMC_FRU_IO_FILE_ID);
}

static void longspeak_bmc_board_fru(uint8_t *fru)
{
    uint8_t *area, *p;

    QEMU_BUILD_BUG_ON(LONGSPEAK_BMC_PRODUCT_OFF + LONGSPEAK_BMC_PRODUCT_SIZE >
                      LONGSPEAK_BMC_FRU0_SIZE);
    QEMU_BUILD_BUG_ON(IA64_PDH_BMC_PRODUCT_ID_OFFSET + 5 >
                      LONGSPEAK_BMC_PRODUCT_SIZE);

    memset(fru, 0xff, LONGSPEAK_BMC_FRU0_SIZE);
    memset(fru, 0, LONGSPEAK_BMC_PRODUCT_OFF + LONGSPEAK_BMC_PRODUCT_SIZE);
    fru[0] = 0x01;
    fru[1] = LONGSPEAK_BMC_INTERNAL_OFF / 8;
    fru[2] = LONGSPEAK_BMC_CHASSIS_OFF / 8;
    fru[3] = LONGSPEAK_BMC_BOARD_OFF / 8;
    fru[4] = LONGSPEAK_BMC_PRODUCT_OFF / 8;
    longspeak_bmc_fru_sum(fru, 8);
    fru[LONGSPEAK_BMC_INTERNAL_OFF] = 0x01;

    area = fru + LONGSPEAK_BMC_CHASSIS_OFF;
    area[0] = 0x01;
    area[1] = LONGSPEAK_BMC_CHASSIS_SIZE / 8;
    area[2] = LONGSPEAK_BMC_CHASSIS_RACK;
    p = longspeak_bmc_fru_fields(area + 3, longspeak_bmc_chassis_fields,
                                 ARRAY_SIZE(longspeak_bmc_chassis_fields));
    *p = 0xc1;                  /* no more fields */
    longspeak_bmc_fru_sum(area, LONGSPEAK_BMC_CHASSIS_SIZE);

    QEMU_BUILD_BUG_ON(ARRAY_SIZE(longspeak_bmc_board_fields) != 4 ||
                      ARRAY_SIZE(longspeak_bmc_riser_fields) != 4);
    longspeak_bmc_board_area(fru + LONGSPEAK_BMC_BOARD_OFF,
                             LONGSPEAK_BMC_BOARD_SIZE / 8,
                             longspeak_bmc_board_fields,
                             LONGSPEAK_BMC_FRU_FILE_ID);

    area = fru + LONGSPEAK_BMC_PRODUCT_OFF;
    area[0] = 0x01;
    area[1] = LONGSPEAK_BMC_PRODUCT_SIZE / 8;
    p = longspeak_bmc_fru_fields(area + 3, longspeak_bmc_product_fields,
                                 ARRAY_SIZE(longspeak_bmc_product_fields));
    assert(p == area + LONGSPEAK_BMC_PRODUCT_FILE_ID);
    *p++ = LONGSPEAK_BMC_FRU_FILE_TL;
    *p++ = LONGSPEAK_BMC_FRU_FILE_ID;
    *p++ = 0x04;                /* the product id: four binary bytes */
    assert(p == area + IA64_PDH_BMC_PRODUCT_ID_OFFSET);
    stl_le_p(p, IA64_PDH_BMC_PRODUCT_ID);
    p[4] = 0xc1;
    longspeak_bmc_fru_sum(area, LONGSPEAK_BMC_PRODUCT_SIZE);
}

static uint32_t longspeak_bmc_dimm_serial(const LongspeakBmcDimm *dimm,
                                          unsigned int slot)
{
    return (uint32_t)(dimm->size >> 20) << 8 | slot;
}

/*
 * A JEDEC SPD for a registered ECC PC2100 module with x4 devices, as the
 * rx2600's HP A6746-60001 (512 MB, 256 Mbit x4 devices; rx2600 capture
 * 2026-10-04, MAN-3): its timing bytes, SPD revision 0, and 0xFF in the
 * bytes nobody wrote.  The manufacturer data are the model's: no JEDEC id,
 * a part number of its own and the serial number.  The firmware reads the
 * whole 128 bytes, rereads byte 63 and compares the two (FFF62010), so only
 * a consistent image passes.
 */
static void longspeak_bmc_spd(const LongspeakBmcDimm *dimm, unsigned int slot,
                              uint8_t *spd)
{
    g_autofree char *part = g_strdup_printf("QE-DDR266-%uM",
                                            (unsigned)(dimm->size >> 20));
    uint8_t sum = 0;
    unsigned int i;

    memset(spd, 0, LONGSPEAK_BMC_SPD_SIZE);
    spd[0] = 0x80;                  /* bytes written by the manufacturer */
    spd[1] = 0x08;                  /* 256 bytes total */
    spd[2] = 0x07;                  /* DDR SDRAM */
    spd[3] = dimm->rows;
    spd[4] = 11;                    /* column addresses */
    spd[5] = dimm->ranks;
    spd[6] = 72;                    /* module data width, ECC */
    spd[8] = 0x04;                  /* SSTL 2.5 V */
    spd[9] = 0x70;                  /* 7.0 ns at CAS latency 2.5 */
    spd[10] = 0x75;                 /* tAC 0.75 ns */
    spd[11] = 0x02;                 /* ECC */
    spd[12] = 0x82;                 /* refresh every 7.8 us, self refresh */
    spd[13] = 4;                    /* device width */
    spd[14] = 4;                    /* checking width */
    spd[15] = 0x01;                 /* tCCD one clock */
    spd[16] = 0x0e;                 /* burst lengths 2, 4 and 8 */
    spd[17] = 4;                    /* banks per device */
    spd[18] = 0x0c;                 /* CAS latency 2 and 2.5 */
    spd[19] = 0x01;                 /* CS latency 0 */
    spd[20] = 0x02;                 /* write latency 1 */
    spd[21] = 0x26;                 /* registered, one PLL, FET */
    spd[22] = 0x80;                 /* device attributes */
    spd[23] = 0x75;                 /* 7.5 ns at CAS latency 2 */
    spd[24] = 0x75;
    spd[27] = 0x50;                 /* tRP 20 ns */
    spd[28] = 0x3c;                 /* tRRD 15 ns */
    spd[29] = 0x50;                 /* tRCD 20 ns */
    spd[30] = 0x2d;                 /* tRAS 45 ns */
    spd[31] = dimm->density;
    spd[32] = 0x90;                 /* address and command setup 0.9 ns */
    spd[33] = 0x90;                 /* and hold */
    spd[34] = 0x50;                 /* data setup 0.5 ns */
    spd[35] = 0x50;                 /* and hold */
    spd[41] = 0x41;                 /* tRC 65 ns */
    spd[42] = 0x4b;                 /* tRFC 75 ns */
    spd[43] = 0x30;                 /* tCK maximum 12 ns */
    spd[44] = 0x32;                 /* tDQSQ 0.5 ns */
    spd[45] = 0x75;                 /* tQHS 0.75 ns */
    for (i = 0; i < 63; i++) {
        sum += spd[i];
    }
    spd[63] = sum;
    memset(spd + 73, ' ', 18);
    memcpy(spd + 73, part, MIN(strlen(part), 18));
    stl_le_p(&spd[95], longspeak_bmc_dimm_serial(dimm, slot));
    memset(spd + 99, 0xff, LONGSPEAK_BMC_SPD_SIZE - 99);
}

/*
 * The slots take pairs of equal modules, so fill the pairs with the largest
 * module that the memory left over allows.
 */
static const LongspeakBmcDimm *longspeak_bmc_slot(uint64_t memory,
                                                  unsigned int slot)
{
    unsigned int pair;
    unsigned int i;

    for (pair = 0; pair <= slot / 2; pair++) {
        for (i = 0; i < ARRAY_SIZE(longspeak_bmc_modules); i++) {
            const LongspeakBmcDimm *dimm = &longspeak_bmc_modules[i];

            if (2 * dimm->size <= memory) {
                if (pair == slot / 2) {
                    return dimm;
                }
                memory -= 2 * dimm->size;
                break;
            }
        }
        if (i == ARRAY_SIZE(longspeak_bmc_modules)) {
            return NULL;
        }
    }
    return NULL;
}

static int longspeak_bmc_dimm_slot_of(uint8_t device)
{
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(longspeak_bmc_dimm_dev); i++) {
        if (longspeak_bmc_dimm_dev[i] == device) {
            return i;
        }
    }
    return -1;
}

typedef struct LongspeakBmcToken {
    uint16_t id;
    uint8_t flags;
    uint8_t size;
} LongspeakBmcToken;

/*
 * GET_TOKEN_INFO flags as the rx2600's BMC answers them (capture 2026-10-04,
 * BMC-7).  With bit 6 the value starts with a checksum byte that makes the
 * sum of all its bytes zero, and the size counts it: SAL_B checks the sum of
 * a READ (FFF53870, "Token transmit checksum error on read") and puts the
 * byte in front of what it writes (FFF54190, FFF543F0).
 */
#define LONGSPEAK_BMC_TOKEN_PLAIN       0x27
#define LONGSPEAK_BMC_TOKEN_SUMMED      0x67
#define LONGSPEAK_BMC_TOKEN_SYSTEM_ID   0x07
#define LONGSPEAK_BMC_TOKEN_CHECKSUM    0x40

#define P LONGSPEAK_BMC_TOKEN_PLAIN
#define S LONGSPEAK_BMC_TOKEN_SUMMED

/*
 * The rx2600's tokens.  SAL_B's table at FFF96FB8 holds 40 bytes a token,
 * the BMC token at +0 (0 for a token kept only in the NVM) and the size of
 * the value at +2; the name at +24 is the next entry's.  The others are the
 * BMC's own; SAL_B writes 0xD01 and the UUID in 0xD02 (FFF55790).
 */
static const LongspeakBmcToken longspeak_bmc_tokens[] = {
    { 0x0067, P,   1 },     /* WAKE_LAN */
    { 0x0500, P,   1 },     /* first boot */
    { 0x0501, P,   1 },     /* BMC_FLSH */
    { 0x0502, P,   1 },     /* E_BUZZER */
    { 0x0503, P,   1 },     /* LAN_1GB */
    { 0x0504, P,   1 },     /* MANG_LAN */
    { 0x0505, P,   1 },     /* SERIAL_1 */
    { 0x0506, P,   1 },     /* SERIAL_2 */
    { 0x0508, P,   1 },
    { 0x0509, P,   1 },     /* BMC_OP_M */
    { 0x050a, P,   1 },
    { 0x0510, P,   1 },
    { 0x0511, P,   1 },     /* SPR_SPEC */
    { 0x0512, P,   1 },
    { 0x0513, P,   1 },     /* DEBUG */
    { 0x0514, P,   1 },     /* SPARE_01 to SPARE_12 */
    { 0x0515, P,   1 },
    { 0x0516, P,   1 },
    { 0x0517, P,   1 },
    { 0x0518, P,   1 },
    { 0x0519, P,   1 },
    { 0x051a, P,   1 },
    { 0x051b, P,   1 },
    { 0x051c, P,   1 },
    { 0x051d, P,   1 },
    { 0x051e, P,   1 },
    { 0x051f, P,   1 },
    { 0x0900, S,   9 },     /* SPEEDY_B */
    { 0x0901, S,   9 },     /* SPEEDY_D */
    { 0x0920, S,   3 },     /* CPU_MON */
    { 0x0921, S,   3 },     /* CELL_MON */
    { 0x0922, P,   1 },     /* MON_ALGO */
    { 0x0928, P,   2 },
    { 0x0929, S,   2 },
    { 0x092a, S,   2 },
    { 0x0940, S,   5 },     /* EFI_LANG */
    { 0x0941, S,   3 },     /* EFI_TIME */
    { 0x0942, P,   8 },     /* EFI_DEBG */
    { 0x0a00, P,   2 },     /* ROM_RELS */
    { 0x0a01, P,   4 },     /* ROM_DATE */
    { 0x0a02, P,   8 },
    { 0x0a03, P,   2 },     /* SALA_REV */
    { 0x0a04, P,   2 },     /* SALB_REV */
    { 0x0a05, P,   2 },     /* ACPI_REV */
    { 0x0a06, P,   2 },     /* EFI_REV */
    { 0x0a07, P,   2 },     /* EFI_SPEC */
    { 0x0a08, P,   2 },     /* EFI_INTL */
    { 0x0a09, P,   2 },     /* POSE_REV */
    { 0x0a0a, P,   2 },     /* IPMI_REV */
    { 0x0a0b, P,   8 },     /* BIOS_REV */
    { 0x0a0c, P,   8 },     /* GSP_REV */
    { 0x0a0d, P,   2 },     /* SAL_NVMR */
    { 0x0a0e, P,   2 },     /* EFI_NVMR */
    { 0x0a20, P,  64 },     /* FW_REV_S */
    { 0x0a21, P,  10 },     /* DATE_STR */
    { 0x0b00, S,   2 },
    { 0x0b01, P,   1 },
    { 0x0c00, S,  81 },
    { 0x0c10, S,  41 },     /* CONDEV00 to CONDEV05 */
    { 0x0c11, S,  41 },
    { 0x0c12, S,  41 },
    { 0x0c13, S,  41 },
    { 0x0c14, S,  41 },
    { 0x0c15, S,  41 },
    { 0x0d00, S, 129 },
    { 0x0d01, S, 129 },
    { 0x0d02, LONGSPEAK_BMC_TOKEN_SYSTEM_ID, 16 },
};

#undef P
#undef S

/*
 * The values a new BMC holds where they are not zero, as the rx2600's BMC
 * kept them; their meaning is not known.  0x929, 0x92A, 0xB00, 0xC00 and
 * 0xD00 fail their checksum there, and SAL_B boots past that.
 */
typedef struct LongspeakBmcTokenDefault {
    uint16_t id;
    uint8_t fill;
    uint8_t first;
} LongspeakBmcTokenDefault;

static const LongspeakBmcTokenDefault longspeak_bmc_token_defaults[] = {
    { 0x0508, 0x00, 0x03 },
    { 0x0510, 0x00, 0xd2 },
    { 0x0928, 0xff, 0xff },
    { 0x0929, 0xff, 0xff },
    { 0x092a, 0xff, 0xff },
    { 0x0b00, 0xff, 0xff },
    { 0x0b01, 0xff, 0xff },
    { 0x0c00, 0xff, 0xff },
    { 0x0d00, 0xff, 0xff },
};

/* The token, and where its value starts in the store. */
static const LongspeakBmcToken *longspeak_bmc_token_find(uint16_t id,
                                                         unsigned int *base)
{
    unsigned int i, at = 0;

    for (i = 0; i < ARRAY_SIZE(longspeak_bmc_tokens); i++) {
        if (longspeak_bmc_tokens[i].id == id) {
            *base = at;
            return &longspeak_bmc_tokens[i];
        }
        at += longspeak_bmc_tokens[i].size;
    }
    return NULL;
}

/*
 * The system UUID is the BMC's own, in token 0xD02 in EFI GUID byte order
 * (rx2600 capture 2026-10-04, BMC-3: `67F52C74-E8E5-11D7-...` reads
 * `74 2C F5 67 E5 E8 D7 11 ...`).  SAL_B reads it for SMBIOS type 1 and
 * `info sys` (FFF54FE0) and writes it only from FFF55790, its "set system
 * id" path, so a BMC without one reports a zero UUID.  The model's BMC
 * comes with a UUID of its own, kept like the other tokens; -uuid replaces
 * it.
 */
#define LONGSPEAK_BMC_TOKEN_UUID    0x0d02

static const QemuUUID longspeak_bmc_uuid = {
    .data = { 0x4c, 0x6f, 0x6e, 0x67, 0x73, 0x50, 0x45, 0x61,
              0x8b, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01 },
};

void longspeak_bmc_tokens_reset(uint8_t *tokens)
{
    const LongspeakBmcToken *token;
    unsigned int i, base;

    memset(tokens, 0, LONGSPEAK_BMC_TOKEN_BYTES);
    for (i = 0; i < ARRAY_SIZE(longspeak_bmc_token_defaults); i++) {
        const LongspeakBmcTokenDefault *d = &longspeak_bmc_token_defaults[i];

        token = longspeak_bmc_token_find(d->id, &base);
        assert(token != NULL);
        memset(tokens + base, d->fill, token->size);
        tokens[base] = d->first;
    }
}

void longspeak_bmc_tokens_init(uint8_t *tokens)
{
    const LongspeakBmcToken *token;
    QemuUUID uuid;
    unsigned int base;

    token = longspeak_bmc_token_find(LONGSPEAK_BMC_TOKEN_UUID, &base);
    assert(token != NULL && token->size == sizeof(uuid.data));
    if (qemu_uuid_set) {
        uuid = qemu_uuid;
    } else if (buffer_is_zero(tokens + base, token->size)) {
        uuid = longspeak_bmc_uuid;
    } else {
        return;
    }
    uuid = qemu_uuid_bswap(uuid);
    memcpy(tokens + base, uuid.data, token->size);
}

/* The byte that makes the sum of a value with a checksum zero. */
static uint8_t longspeak_bmc_token_checksum(const uint8_t *data,
                                            unsigned int len)
{
    uint8_t sum = 0;

    while (len--) {
        sum += *data++;
    }
    return -sum;
}

/*
 * Token 0 answers GET_TOKEN_INFO with size 0, a token the BMC does not keep
 * cc CBh.  A READ or WRITE of a value that fails its checksum answers cc
 * 70h; the READ still returns the value.  VERIFY (0Ah, SAL_B's
 * VERIFY_TOKEN_CHECKSUM) checks a value before SAL_B reads it in parts
 * (FFF53B4C); UPDATE (0Bh, UPDATE_TOKEN_CHECKSUM) sets the checksum byte
 * from the rest of the value, and SAL_B compares the byte it answers with
 * its own after a write in parts (FFF5474C).  The two bytes VERIFY answers
 * are not read by SAL_B; the stored and the computed checksum are inferred.
 */
static bool longspeak_bmc_token(IPMIBmc *s, uint8_t *cmd,
                                unsigned int cmd_len, RspBuffer *rsp)
{
    LongspeakPDHState *pdh = (LongspeakPDHState *)
        object_dynamic_cast(OBJECT(s)->parent, TYPE_LONGSPEAK_PDH);
    const LongspeakBmcToken *token = NULL;
    unsigned int base = 0, size, offset, count, i;
    uint16_t id;
    uint8_t *value;
    bool summed;

    switch (cmd[1]) {
    case LONGSPEAK_BMC_TOKEN_INFO:
    case LONGSPEAK_BMC_TOKEN_READ:
    case LONGSPEAK_BMC_TOKEN_WRITE:
    case LONGSPEAK_BMC_TOKEN_READ_PART:
    case LONGSPEAK_BMC_TOKEN_WRITE_PART:
    case LONGSPEAK_BMC_TOKEN_VERIFY:
    case LONGSPEAK_BMC_TOKEN_UPDATE:
        break;
    default:
        return false;
    }
    if (cmd_len < 4) {
        rsp_buffer_set_error(rsp, IPMI_CC_REQUEST_DATA_LENGTH_INVALID);
        return true;
    }
    id = lduw_le_p(cmd + 2);
    if (pdh != NULL) {
        token = longspeak_bmc_token_find(id, &base);
    }
    if (token == NULL) {
        if (id == 0 && cmd[1] == LONGSPEAK_BMC_TOKEN_INFO) {
            rsp_buffer_push(rsp, 0);
            rsp_buffer_push(rsp, 0);
        } else {
            rsp_buffer_set_error(rsp, IPMI_CC_REQ_ENTRY_NOT_PRESENT);
        }
        return true;
    }
    size = token->size;
    value = pdh->bmc_tokens + base;
    summed = token->flags & LONGSPEAK_BMC_TOKEN_CHECKSUM;

    switch (cmd[1]) {
    case LONGSPEAK_BMC_TOKEN_INFO:
        rsp_buffer_push(rsp, token->flags);
        rsp_buffer_push(rsp, size);
        return true;
    case LONGSPEAK_BMC_TOKEN_READ:
        for (i = 0; i < size; i++) {
            rsp_buffer_push(rsp, value[i]);
        }
        if (summed && longspeak_bmc_token_checksum(value, size) != 0) {
            rsp_buffer_set_error(rsp, LONGSPEAK_BMC_CC_TOKEN_CHECKSUM);
        }
        return true;
    case LONGSPEAK_BMC_TOKEN_WRITE:
        if (cmd_len != 4 + size) {
            rsp_buffer_set_error(rsp, IPMI_CC_REQUEST_DATA_LENGTH_INVALID);
        } else if (summed &&
                   longspeak_bmc_token_checksum(cmd + 4, size) != 0) {
            rsp_buffer_set_error(rsp, LONGSPEAK_BMC_CC_TOKEN_CHECKSUM);
        } else {
            memcpy(value, cmd + 4, size);
        }
        return true;
    case LONGSPEAK_BMC_TOKEN_VERIFY:
    case LONGSPEAK_BMC_TOKEN_UPDATE:
        if (!summed) {
            rsp_buffer_set_error(rsp, IPMI_CC_INVALID_DATA_FIELD);
            return true;
        }
        if (cmd[1] == LONGSPEAK_BMC_TOKEN_UPDATE) {
            value[0] = longspeak_bmc_token_checksum(value + 1, size - 1);
            rsp_buffer_push(rsp, value[0]);
            return true;
        }
        rsp_buffer_push(rsp, value[0]);
        rsp_buffer_push(rsp, longspeak_bmc_token_checksum(value + 1, size - 1));
        if (longspeak_bmc_token_checksum(value, size) != 0) {
            rsp_buffer_set_error(rsp, LONGSPEAK_BMC_CC_TOKEN_CHECKSUM);
        }
        return true;
    }

    /* The partial commands: a 16-bit offset and a count follow the token. */
    if (cmd_len < 7) {
        rsp_buffer_set_error(rsp, IPMI_CC_REQUEST_DATA_LENGTH_INVALID);
        return true;
    }
    offset = lduw_le_p(cmd + 4);
    count = cmd[6];
    if (offset + count > size) {
        rsp_buffer_set_error(rsp, IPMI_CC_PARM_OUT_OF_RANGE);
        return true;
    }
    if (cmd[1] == LONGSPEAK_BMC_TOKEN_READ_PART) {
        for (i = 0; i < count; i++) {
            rsp_buffer_push(rsp, value[offset + i]);
        }
    } else if (cmd_len != 7 + count) {
        rsp_buffer_set_error(rsp, IPMI_CC_REQUEST_DATA_LENGTH_INVALID);
    } else if (count > 0) {
        memcpy(value + offset, cmd + 7, count);
    }
    return true;
}

/*
 * In the file: a tag, then each token's number, size and value, up to a zero
 * number.  A record of a token this BMC does not keep, or of another size, is
 * left out on load, so the file outlives a change of the table; a value that
 * lacks only its checksum byte gets one.
 */
#define LONGSPEAK_BMC_STORE_TAG "BMCTOKEN"
#define LONGSPEAK_BMC_STORE_TAG_LEN 8
#define LONGSPEAK_BMC_STORE_RECORD 3

void longspeak_bmc_tokens_save(const uint8_t *tokens, uint8_t *area)
{
    uint8_t *p = area + LONGSPEAK_BMC_STORE_TAG_LEN;
    unsigned int i;

    memset(area, 0, LONGSPEAK_PDH_STORE_BMC);
    memcpy(area, LONGSPEAK_BMC_STORE_TAG, LONGSPEAK_BMC_STORE_TAG_LEN);
    for (i = 0; i < ARRAY_SIZE(longspeak_bmc_tokens); i++) {
        const LongspeakBmcToken *token = &longspeak_bmc_tokens[i];

        stw_le_p(p, token->id);
        p[2] = token->size;
        memcpy(p + LONGSPEAK_BMC_STORE_RECORD, tokens, token->size);
        tokens += token->size;
        p += LONGSPEAK_BMC_STORE_RECORD + token->size;
    }
}

void longspeak_bmc_tokens_load(uint8_t *tokens, const uint8_t *area)
{
    const uint8_t *p = area + LONGSPEAK_BMC_STORE_TAG_LEN;
    const uint8_t *end = area + LONGSPEAK_PDH_STORE_BMC;
    const LongspeakBmcToken *token;
    unsigned int base;

    if (memcmp(area, LONGSPEAK_BMC_STORE_TAG,
               LONGSPEAK_BMC_STORE_TAG_LEN) != 0) {
        return;                 /* a new BMC */
    }
    while (end - p >= LONGSPEAK_BMC_STORE_RECORD && lduw_le_p(p) != 0 &&
           end - p >= LONGSPEAK_BMC_STORE_RECORD + p[2]) {
        token = longspeak_bmc_token_find(lduw_le_p(p), &base);
        if (token != NULL && token->size == p[2]) {
            memcpy(tokens + base, p + LONGSPEAK_BMC_STORE_RECORD, p[2]);
        } else if (token != NULL &&
                   (token->flags & LONGSPEAK_BMC_TOKEN_CHECKSUM) &&
                   token->size == p[2] + 1) {
            /* A file from before the checksum bytes: the value alone. */
            memcpy(tokens + base + 1, p + LONGSPEAK_BMC_STORE_RECORD, p[2]);
            tokens[base] = longspeak_bmc_token_checksum(tokens + base + 1,
                                                        p[2]);
        }
        p += LONGSPEAK_BMC_STORE_RECORD + p[2];
    }
}

static bool longspeak_bmc_fru_image(uint8_t *cmd, unsigned int cmd_len,
                                    RspBuffer *rsp, const uint8_t *image,
                                    unsigned int size)
{
    unsigned int offset, count, i;

    if (cmd[1] == LONGSPEAK_BMC_CMD_FRU_INFO) {
        rsp_buffer_push(rsp, size & 0xff);
        rsp_buffer_push(rsp, size >> 8);
        rsp_buffer_push(rsp, 0); /* addressed by byte */
        return true;
    }

    if (cmd_len < 6) {
        rsp_buffer_set_error(rsp, IPMI_CC_REQUEST_DATA_LENGTH_INVALID);
        return true;
    }

    offset = cmd[3] | cmd[4] << 8;
    if (offset >= size) {
        rsp_buffer_set_error(rsp, IPMI_CC_PARM_OUT_OF_RANGE);
        return true;
    }

    count = MIN(cmd[5], size - offset);
    rsp_buffer_push(rsp, count);
    for (i = 0; i < count; i++) {
        rsp_buffer_push(rsp, image ? image[offset + i] : 0);
    }
    return true;
}

/*
 * A device the BMC does not have answers cc CBh, also to command 0xd0 in the
 * DIMM range 80h-BFh, where 86h and 87h answer as 88h and 89h; 0xd0 on any
 * other device answers cc CCh (rx2600 capture 2026-10-04, BMC-4 to 6).  The
 * processor devices are there for the processors the machine has.
 */
static bool longspeak_bmc_fru(uint8_t *cmd, unsigned int cmd_len,
                              RspBuffer *rsp)
{
    uint8_t image[LONGSPEAK_BMC_FRU0_SIZE];
    const LongspeakBmcDimm *dimm;
    uint8_t device;
    unsigned int i;
    int slot;

    if (cmd_len < 3) {
        return false;
    }

    device = cmd[2];
    if (cmd[1] == LONGSPEAK_BMC_CMD_FRU_STATUS &&
        (device == 0x86 || device == 0x87)) {
        device += 2;
    }

    /*
     * The machine's memory decides which slots carry a module.  It is read
     * here and not held in the device because a subclass of ipmi-bmc-sim has
     * no instance state of its own: the parent's structure is private.
     */
    slot = longspeak_bmc_dimm_slot_of(device);
    dimm = slot < 0 ? NULL
                    : longspeak_bmc_slot(current_machine->ram_size, slot);

    if (cmd[1] == LONGSPEAK_BMC_CMD_FRU_STATUS) {
        uint32_t serial;

        if (!dimm) {
            rsp_buffer_set_error(rsp, device >= 0x80 && device < 0xc0 ?
                                      IPMI_CC_REQ_ENTRY_NOT_PRESENT :
                                      IPMI_CC_INVALID_DATA_FIELD);
            return true;
        }
        serial = longspeak_bmc_dimm_serial(dimm, slot);
        for (i = 0; i < 4; i++) {
            rsp_buffer_push(rsp, serial >> (8 * i));
        }
        return true;
    }

    if (device == LONGSPEAK_BMC_FRU0) {
        longspeak_bmc_board_fru(image);
        return longspeak_bmc_fru_image(cmd, cmd_len, rsp, image,
                                       LONGSPEAK_BMC_FRU0_SIZE);
    }

    if (device == LONGSPEAK_BMC_FRU_IO) {
        longspeak_bmc_riser_fru(image);
        return longspeak_bmc_fru_image(cmd, cmd_len, rsp, image,
                                       LONGSPEAK_BMC_FRU_IO_SIZE);
    }

    if (device == LONGSPEAK_BMC_FRU_MP) {
        if (cmd[1] == LONGSPEAK_BMC_CMD_FRU_READ) {
            rsp_buffer_set_error(rsp, LONGSPEAK_BMC_CC_NO_RESPONSE);
            return true;
        }
        return longspeak_bmc_fru_image(cmd, cmd_len, rsp, NULL,
                                       LONGSPEAK_BMC_FRU_MP_SIZE);
    }

    if (device >= LONGSPEAK_BMC_FRU_FIRST &&
        device < LONGSPEAK_BMC_FRU_FIRST + LONGSPEAK_BMC_FRU_DEVICES &&
        (device - LONGSPEAK_BMC_FRU_FIRST) % (LONGSPEAK_BMC_FRU_DEVICES / 2) <
        current_machine->smp.cpus) {
        return longspeak_bmc_fru_image(cmd, cmd_len, rsp, NULL,
                                       LONGSPEAK_BMC_FRU_SIZE);
    }

    if (!dimm) {
        rsp_buffer_set_error(rsp, IPMI_CC_REQ_ENTRY_NOT_PRESENT);
        return true;
    }
    longspeak_bmc_spd(dimm, slot, image);
    return longspeak_bmc_fru_image(cmd, cmd_len, rsp, image,
                                   LONGSPEAK_BMC_SPD_SIZE);
}

/*
 * Where the rx2600's BMC answers otherwise than the IPMI simulator (rx2600
 * capture 2026-10-04, BMC-1 and the row after it).  It keeps no channel
 * information, restart cause, boot options, event receiver or PEF, and its
 * device GUID is all zero.  Its chassis has an intrusion sensor, a front
 * panel lockout and a diagnostic interrupt, and was last powered on after an
 * AC failure.  It counts the power-on time in 5-minute units, and nothing
 * sets the clock of its SDR repository, which counts seconds from the start
 * of the BMC; the BMC runs on standby power, so both count from the start of
 * the machine here.
 */
static bool longspeak_bmc_ipmi(uint8_t *cmd, RspBuffer *rsp)
{
    uint64_t seconds = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) /
                       NANOSECONDS_PER_SECOND;
    unsigned int i;

    switch ((cmd[0] >> 2) << 8 | cmd[1]) {
    case LONGSPEAK_BMC_NETFN_APP << 8 | LONGSPEAK_BMC_CMD_DEVICE_ID:
        rsp_buffer_push(rsp, IA64_PDH_BMC_DEVICE_ID);
        rsp_buffer_push(rsp, IA64_PDH_BMC_DEVICE_REV);
        rsp_buffer_push(rsp, IA64_PDH_BMC_FW_MAJOR);
        rsp_buffer_push(rsp, IA64_PDH_BMC_FW_MINOR);
        rsp_buffer_push(rsp, IA64_PDH_BMC_IPMI_VERSION);
        rsp_buffer_push(rsp, IA64_PDH_BMC_DEVICE_SUPPORT);
        for (i = 0; i < 3; i++) {
            rsp_buffer_push(rsp, IA64_PDH_BMC_MANUFACTURER >> (8 * i));
        }
        rsp_buffer_push(rsp, IA64_PDH_BMC_IPMI_PRODUCT & 0xff);
        rsp_buffer_push(rsp, IA64_PDH_BMC_IPMI_PRODUCT >> 8);
        return true;
    case LONGSPEAK_BMC_NETFN_APP << 8 | LONGSPEAK_BMC_CMD_DEVICE_GUID:
        for (i = 0; i < 16; i++) {
            rsp_buffer_push(rsp, 0);
        }
        return true;
    case LONGSPEAK_BMC_NETFN_APP << 8 | LONGSPEAK_BMC_CMD_CHANNEL_INFO:
        rsp_buffer_set_error(rsp, IPMI_CC_INVALID_CMD);
        return true;
    case LONGSPEAK_BMC_NETFN_CHASSIS << 8 | LONGSPEAK_BMC_CMD_CHASSIS_CAPS:
        rsp_buffer_push(rsp, 0x07);
        for (i = 0; i < 4; i++) {
            rsp_buffer_push(rsp, 0x20);     /* FRU, SDR, SEL, SM: the BMC */
        }
        return true;
    case LONGSPEAK_BMC_NETFN_CHASSIS << 8 | LONGSPEAK_BMC_CMD_CHASSIS_STATUS:
        rsp_buffer_push(rsp, 0x01);         /* on; stays off after AC loss */
        rsp_buffer_push(rsp, 0x01);         /* last event: AC failed */
        rsp_buffer_push(rsp, 0x00);
        return true;
    case LONGSPEAK_BMC_NETFN_CHASSIS << 8 | LONGSPEAK_BMC_CMD_RESTART_CAUSE:
    case LONGSPEAK_BMC_NETFN_CHASSIS << 8 | LONGSPEAK_BMC_CMD_BOOT_OPTIONS:
    case LONGSPEAK_BMC_NETFN_SENSOR << 8 | LONGSPEAK_BMC_CMD_EVENT_RECEIVER:
    case LONGSPEAK_BMC_NETFN_SENSOR << 8 | LONGSPEAK_BMC_CMD_PEF_CAPS:
        rsp_buffer_set_error(rsp, IPMI_CC_COMMAND_INVALID_FOR_LUN);
        return true;
    case LONGSPEAK_BMC_NETFN_CHASSIS << 8 | LONGSPEAK_BMC_CMD_POH:
        rsp_buffer_push(rsp, 5);
        for (i = 0; i < 4; i++) {
            rsp_buffer_push(rsp, (seconds / 300) >> (8 * i));
        }
        return true;
    case LONGSPEAK_BMC_NETFN_STORAGE << 8 | LONGSPEAK_BMC_CMD_SDR_TIME:
        for (i = 0; i < 4; i++) {
            rsp_buffer_push(rsp, seconds >> (8 * i));
        }
        return true;
    default:
        return false;
    }
}

/*
 * The event log.  The board has one BMC behind its four interfaces, but the
 * model has an IPMI simulator for each (longspeak_pdh.c), so the log lives in
 * the PDH, as the tokens do: what the firmware logs through one interface,
 * a reader finds through another.  Its shape is the rx2600's (capture
 * 2026-10-04, BMC-9): SEL version 01h, 1023 records of 16 bytes, record ids
 * 10h apart from 10h, allocation information and reservations.  The BMC as
 * event receiver logs each platform event message as a system event record.
 * The log is lost at power-off, which the rx2600's is not.
 */
static uint32_t longspeak_bmc_sel_now(const LongspeakPDHState *pdh)
{
    return pdh->bmc_sel_time_offset +
           qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / NANOSECONDS_PER_SECOND;
}

static uint16_t longspeak_bmc_sel_id(unsigned int index)
{
    return (index + 1) * LONGSPEAK_BMC_SEL_RECORD;
}

static uint8_t *longspeak_bmc_sel_add(LongspeakPDHState *pdh,
                                      const uint8_t *record)
{
    uint32_t now = longspeak_bmc_sel_now(pdh);
    uint8_t *entry;

    if (pdh->bmc_sel_count == LONGSPEAK_BMC_SEL_RECORDS) {
        return NULL;
    }
    entry = pdh->bmc_sel[pdh->bmc_sel_count];
    memcpy(entry, record, LONGSPEAK_BMC_SEL_RECORD);
    stw_le_p(entry, longspeak_bmc_sel_id(pdh->bmc_sel_count));
    if (entry[2] < 0xe0) {          /* OEM types E0h-FFh have no time */
        stl_le_p(entry + 3, now);
    }
    pdh->bmc_sel_count++;
    pdh->bmc_sel_last_add = now;
    return entry;
}

/* Reservation 0 is none (IPMI v2.0 31.4). */
static bool longspeak_bmc_sel_reserved(const LongspeakPDHState *pdh,
                                       const uint8_t *cmd)
{
    return pdh->bmc_sel_reservation != 0 &&
           lduw_le_p(cmd + 2) == pdh->bmc_sel_reservation;
}

static bool longspeak_bmc_sel(IPMIBmc *s, uint8_t *cmd, unsigned int cmd_len,
                              RspBuffer *rsp)
{
    LongspeakPDHState *pdh = (LongspeakPDHState *)
        object_dynamic_cast(OBJECT(s)->parent, TYPE_LONGSPEAK_PDH);
    unsigned int free_units, index, offset, count, i;
    uint8_t record[LONGSPEAK_BMC_SEL_RECORD] = { 0 };
    const uint8_t *entry;
    uint16_t id;

    if (pdh == NULL) {
        return false;
    }
    free_units = LONGSPEAK_BMC_SEL_RECORDS - pdh->bmc_sel_count;

    if ((cmd[0] >> 2) == LONGSPEAK_BMC_NETFN_SENSOR) {
        if (cmd[1] != LONGSPEAK_BMC_CMD_PLATFORM_EVENT) {
            return false;
        }
        if (cmd_len < 10) {
            rsp_buffer_set_error(rsp, IPMI_CC_REQUEST_DATA_LENGTH_INVALID);
            return true;
        }
        record[2] = 0x02;           /* system event record */
        record[7] = cmd[2];         /* generator id */
        memcpy(record + 9, cmd + 3, 7);
        if (!longspeak_bmc_sel_add(pdh, record)) {
            rsp_buffer_set_error(rsp, IPMI_CC_OUT_OF_SPACE);
        }
        return true;
    }

    switch (cmd[1]) {
    case LONGSPEAK_BMC_CMD_SEL_INFO:
        rsp_buffer_push(rsp, 0x01);
        rsp_buffer_push(rsp, pdh->bmc_sel_count & 0xff);
        rsp_buffer_push(rsp, pdh->bmc_sel_count >> 8);
        rsp_buffer_push(rsp, (free_units * LONGSPEAK_BMC_SEL_RECORD) & 0xff);
        rsp_buffer_push(rsp, (free_units * LONGSPEAK_BMC_SEL_RECORD) >> 8);
        for (i = 0; i < 4; i++) {
            rsp_buffer_push(rsp, pdh->bmc_sel_last_add >> (8 * i));
        }
        for (i = 0; i < 4; i++) {
            rsp_buffer_push(rsp, pdh->bmc_sel_last_erase >> (8 * i));
        }
        rsp_buffer_push(rsp, 0x03);     /* reserve, allocation information */
        return true;
    case LONGSPEAK_BMC_CMD_SEL_ALLOC:
        rsp_buffer_push(rsp, LONGSPEAK_BMC_SEL_RECORDS & 0xff);
        rsp_buffer_push(rsp, LONGSPEAK_BMC_SEL_RECORDS >> 8);
        rsp_buffer_push(rsp, LONGSPEAK_BMC_SEL_RECORD);
        rsp_buffer_push(rsp, 0);
        for (i = 0; i < 2; i++) {
            rsp_buffer_push(rsp, free_units & 0xff);
            rsp_buffer_push(rsp, free_units >> 8);
        }
        rsp_buffer_push(rsp, 1);        /* one unit is the largest record */
        return true;
    case LONGSPEAK_BMC_CMD_SEL_RESERVE:
        if (++pdh->bmc_sel_reservation == 0) {
            pdh->bmc_sel_reservation = 1;
        }
        rsp_buffer_push(rsp, pdh->bmc_sel_reservation & 0xff);
        rsp_buffer_push(rsp, pdh->bmc_sel_reservation >> 8);
        return true;
    case LONGSPEAK_BMC_CMD_SEL_GET:
        if (cmd_len < 8) {
            rsp_buffer_set_error(rsp, IPMI_CC_REQUEST_DATA_LENGTH_INVALID);
            return true;
        }
        id = lduw_le_p(cmd + 4);
        offset = cmd[6];
        /* A read of part of a record needs the reservation (IPMI 31.5). */
        if ((offset != 0 || cmd[7] != 0xff) &&
            !longspeak_bmc_sel_reserved(pdh, cmd)) {
            rsp_buffer_set_error(rsp, IPMI_CC_INVALID_RESERVATION);
            return true;
        }
        if (id == 0x0000) {
            index = 0;
        } else if (id == 0xffff) {
            index = pdh->bmc_sel_count - 1;
        } else if (id % LONGSPEAK_BMC_SEL_RECORD == 0) {
            index = id / LONGSPEAK_BMC_SEL_RECORD - 1;
        } else {
            index = LONGSPEAK_BMC_SEL_RECORDS;
        }
        if (pdh->bmc_sel_count == 0 || index >= pdh->bmc_sel_count) {
            rsp_buffer_set_error(rsp, IPMI_CC_REQ_ENTRY_NOT_PRESENT);
            return true;
        }
        if (offset >= LONGSPEAK_BMC_SEL_RECORD) {
            rsp_buffer_set_error(rsp, IPMI_CC_PARM_OUT_OF_RANGE);
            return true;
        }
        count = MIN(cmd[7], LONGSPEAK_BMC_SEL_RECORD - offset);
        id = index + 1 < pdh->bmc_sel_count ? longspeak_bmc_sel_id(index + 1)
                                             : 0xffff;
        rsp_buffer_push(rsp, id & 0xff);
        rsp_buffer_push(rsp, id >> 8);
        entry = pdh->bmc_sel[index];
        for (i = 0; i < count; i++) {
            rsp_buffer_push(rsp, entry[offset + i]);
        }
        return true;
    case LONGSPEAK_BMC_CMD_SEL_ADD:
        if (cmd_len < 2 + LONGSPEAK_BMC_SEL_RECORD) {
            rsp_buffer_set_error(rsp, IPMI_CC_REQUEST_DATA_LENGTH_INVALID);
            return true;
        }
        entry = longspeak_bmc_sel_add(pdh, cmd + 2);
        if (!entry) {
            rsp_buffer_set_error(rsp, IPMI_CC_OUT_OF_SPACE);
            return true;
        }
        rsp_buffer_push(rsp, entry[0]);
        rsp_buffer_push(rsp, entry[1]);
        return true;
    case LONGSPEAK_BMC_CMD_SEL_CLEAR:
        if (cmd_len < 8) {
            rsp_buffer_set_error(rsp, IPMI_CC_REQUEST_DATA_LENGTH_INVALID);
            return true;
        }
        if (!longspeak_bmc_sel_reserved(pdh, cmd)) {
            rsp_buffer_set_error(rsp, IPMI_CC_INVALID_RESERVATION);
            return true;
        }
        if (cmd[4] != 'C' || cmd[5] != 'L' || cmd[6] != 'R' ||
            (cmd[7] != 0xaa && cmd[7] != 0x00)) {
            rsp_buffer_set_error(rsp, IPMI_CC_INVALID_DATA_FIELD);
            return true;
        }
        if (cmd[7] == 0xaa) {
            pdh->bmc_sel_count = 0;
            pdh->bmc_sel_last_erase = longspeak_bmc_sel_now(pdh);
        }
        rsp_buffer_push(rsp, 0x01);     /* erasure completed */
        return true;
    case LONGSPEAK_BMC_CMD_SEL_GET_TIME:
        for (i = 0; i < 4; i++) {
            rsp_buffer_push(rsp, longspeak_bmc_sel_now(pdh) >> (8 * i));
        }
        return true;
    case LONGSPEAK_BMC_CMD_SEL_SET_TIME:
        if (cmd_len < 6) {
            rsp_buffer_set_error(rsp, IPMI_CC_REQUEST_DATA_LENGTH_INVALID);
            return true;
        }
        pdh->bmc_sel_time_offset = (int64_t)ldl_le_p(cmd + 2) -
            (int64_t)(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) /
                      NANOSECONDS_PER_SECOND);
        return true;
    default:
        return false;
    }
}

static void longspeak_bmc_handle_command(IPMIBmc *s, uint8_t *cmd,
                                         unsigned int cmd_len,
                                         unsigned int max_cmd_len,
                                         uint8_t msg_id)
{
    LongspeakBmcClass *bc = LONGSPEAK_BMC_GET_CLASS(s);
    RspBuffer rsp = { };
    bool handled = false;

    if (cmd_len >= 2) {
        uint32_t data = 0;
        unsigned int i;

        for (i = 2; i < MIN(cmd_len, 6); i++) {
            data |= (uint32_t)cmd[i] << (8 * (i - 2));
        }
        trace_longspeak_bmc_command(cmd[0] >> 2, cmd[1], cmd_len, data);
    }
    if (cmd_len >= 2 && (cmd[0] & 0x03) == 0) {
        rsp_buffer_push(&rsp, cmd[0] | 0x04);
        rsp_buffer_push(&rsp, cmd[1]);
        rsp_buffer_push(&rsp, 0);

        switch (cmd[0] >> 2) {
        case LONGSPEAK_BMC_NETFN_TOKEN:
            handled = longspeak_bmc_token(s, cmd, cmd_len, &rsp);
            break;
        case LONGSPEAK_BMC_NETFN_STORAGE:
            if (cmd[1] == LONGSPEAK_BMC_CMD_FRU_INFO ||
                cmd[1] == LONGSPEAK_BMC_CMD_FRU_READ ||
                cmd[1] == LONGSPEAK_BMC_CMD_FRU_STATUS) {
                handled = longspeak_bmc_fru(cmd, cmd_len, &rsp);
            } else {
                handled = longspeak_bmc_sel(s, cmd, cmd_len, &rsp);
            }
            break;
        case LONGSPEAK_BMC_NETFN_SENSOR:
            handled = longspeak_bmc_sel(s, cmd, cmd_len, &rsp);
            break;
        default:
            break;
        }
        if (!handled) {
            handled = longspeak_bmc_ipmi(cmd, &rsp);
        }
    }

    if (!handled) {
        bc->parent_handle_command(s, cmd, cmd_len, max_cmd_len, msg_id);
        return;
    }
    trace_longspeak_bmc_response(cmd[0] >> 2, cmd[1], rsp.buffer[2], rsp.len);
    IPMI_INTERFACE_GET_CLASS(s->intf)->handle_rsp(s->intf, msg_id, rsp.buffer,
                                                  rsp.len);
}

static void longspeak_bmc_class_init(ObjectClass *oc, const void *data)
{
    IPMIBmcClass *bk = IPMI_BMC_CLASS(oc);
    LongspeakBmcClass *bc = LONGSPEAK_BMC_CLASS(oc);
    unsigned int i, bytes = 0;

    for (i = 0; i < ARRAY_SIZE(longspeak_bmc_tokens); i++) {
        bytes += longspeak_bmc_tokens[i].size;
    }
    assert(bytes <= LONGSPEAK_BMC_TOKEN_BYTES);
    assert(LONGSPEAK_BMC_STORE_TAG_LEN + bytes + 2 +
           LONGSPEAK_BMC_STORE_RECORD * ARRAY_SIZE(longspeak_bmc_tokens) <=
           LONGSPEAK_PDH_STORE_BMC);

    bc->parent_handle_command = bk->handle_command;
    bk->handle_command = longspeak_bmc_handle_command;
}

static const TypeInfo longspeak_bmc_info = {
    .name          = TYPE_LONGSPEAK_BMC,
    .parent        = TYPE_IPMI_BMC_SIMULATOR,
    .class_size    = sizeof(LongspeakBmcClass),
    .class_init    = longspeak_bmc_class_init,
};

static void longspeak_bmc_register_types(void)
{
    type_register_static(&longspeak_bmc_info);
}

type_init(longspeak_bmc_register_types)
