/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The BMC of the Longs Peak (HP zx1 board).
 *
 * HP's firmware keeps a set of "tokens" in its BMC and configures nothing
 * until it can read them: it polls tokens 0x13 to 0x15, then reads token 0,
 * and halts with POST 0x0012A0 "error reading bmc first boot token" if that
 * one is refused.  The commands are HP's own -- IPMI network function 0x32 is
 * controller-specific and carries no defined payload (IPMI v2.0 table 5-1) --
 * so only their shape is known from the traffic: one byte of token number and
 * one of length, answered with that many bytes.  The content is checked: all
 * zeros is accepted, 0xAA is not.
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
 */

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/units.h"
#include "hw/core/boards.h"
#include "hw/ipmi/ipmi.h"
#include "qom/object.h"
#include "hw/ia64/ia64_vpc_abi.h"
#include "longspeak_pdh.h"

#define LONGSPEAK_BMC_NETFN_TOKEN   0x32
#define LONGSPEAK_BMC_TOKEN_FIRST   0x01
#define LONGSPEAK_BMC_TOKEN_LAST    0x03

#define LONGSPEAK_BMC_NETFN_STORAGE 0x0a
#define LONGSPEAK_BMC_CMD_FRU_INFO  0x10
#define LONGSPEAK_BMC_CMD_FRU_READ  0x11

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

#define LONGSPEAK_BMC_FRU0          0x00
#define LONGSPEAK_BMC_FRU0_SIZE     256
#define LONGSPEAK_BMC_BOARD_OFF     16
#define LONGSPEAK_BMC_BOARD_SIZE    32
#define LONGSPEAK_BMC_PRODUCT_OFF   48
#define LONGSPEAK_BMC_PRODUCT_SIZE  128

#define LONGSPEAK_BMC_SPD_SIZE      256

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

static uint8_t *longspeak_bmc_fru_field(uint8_t *p, const char *text)
{
    size_t len = strlen(text);

    *p++ = 0xc0 | len;          /* 8-bit ASCII (IPMI FRU sec 13) */
    memcpy(p, text, len);
    return p + len;
}

static void longspeak_bmc_fru_sum(uint8_t *area, unsigned int size)
{
    uint8_t sum = 0;
    unsigned int i;

    for (i = 0; i < size - 1; i++) {
        sum += area[i];
    }
    area[size - 1] = -sum;
}

static void longspeak_bmc_board_fru(uint8_t *fru)
{
    uint8_t *area, *p;

    QEMU_BUILD_BUG_ON(LONGSPEAK_BMC_PRODUCT_OFF + LONGSPEAK_BMC_PRODUCT_SIZE >
                      LONGSPEAK_BMC_FRU0_SIZE);
    QEMU_BUILD_BUG_ON(IA64_PDH_BMC_PRODUCT_ID_OFFSET + 4 >
                      LONGSPEAK_BMC_PRODUCT_SIZE);

    memset(fru, 0, LONGSPEAK_BMC_FRU0_SIZE);
    fru[0] = 0x01;
    fru[2] = 8 / 8;
    fru[3] = LONGSPEAK_BMC_BOARD_OFF / 8;
    fru[4] = LONGSPEAK_BMC_PRODUCT_OFF / 8;
    longspeak_bmc_fru_sum(fru, 8);

    area = fru + 8;                             /* chassis: type "other" */
    area[0] = 0x01;
    area[1] = 8 / 8;
    area[2] = 0x01;
    p = longspeak_bmc_fru_field(area + 3, "");
    p = longspeak_bmc_fru_field(p, "");
    *p = 0xc1;                  /* no more fields */
    longspeak_bmc_fru_sum(area, 8);

    area = fru + LONGSPEAK_BMC_BOARD_OFF;       /* board */
    area[0] = 0x01;
    area[1] = LONGSPEAK_BMC_BOARD_SIZE / 8;
    p = longspeak_bmc_fru_field(area + 6, "QEMU");
    p = longspeak_bmc_fru_field(p, "Longs Peak");
    p = longspeak_bmc_fru_field(p, "");
    p = longspeak_bmc_fru_field(p, "");
    *p = 0xc1;                  /* no more fields */
    longspeak_bmc_fru_sum(area, LONGSPEAK_BMC_BOARD_SIZE);

    area = fru + LONGSPEAK_BMC_PRODUCT_OFF;     /* product */
    area[0] = 0x01;
    area[1] = LONGSPEAK_BMC_PRODUCT_SIZE / 8;
    p = longspeak_bmc_fru_field(area + 3, "QEMU");
    p = longspeak_bmc_fru_field(p, "Longs Peak");
    p = longspeak_bmc_fru_field(p, "");
    p = longspeak_bmc_fru_field(p, "");
    p = longspeak_bmc_fru_field(p, "");
    *p = 0xc1;                  /* no more fields */
    stl_le_p(area + IA64_PDH_BMC_PRODUCT_ID_OFFSET,
             IA64_PDH_BMC_PRODUCT_ID);
    longspeak_bmc_fru_sum(area, LONGSPEAK_BMC_PRODUCT_SIZE);
}

/*
 * A JEDEC SPD for a registered ECC PC2100 module (SPD revision 1.2).  The
 * firmware reads the whole 128 bytes, rereads byte 63 and compares the two
 * (FFF62010), so only a consistent image passes.
 */
static void longspeak_bmc_spd(const LongspeakBmcDimm *dimm, uint8_t *spd)
{
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
    spd[9] = 0x75;                  /* 7.5 ns cycle: PC2100 */
    spd[10] = 0x54;
    spd[11] = 0x02;                 /* ECC */
    spd[12] = 0x82;                 /* refresh every 7.8 us, self refresh */
    spd[13] = 8;                    /* device width */
    spd[14] = 8;                    /* checking width */
    spd[16] = 0x0e;                 /* burst lengths 2, 4 and 8 */
    spd[17] = 4;                    /* banks per device */
    spd[18] = 0x0c;                 /* CAS latency 2 and 2.5 */
    spd[20] = 0x02;                 /* registered */
    spd[21] = 0x26;                 /* registered, one PLL, FET */
    spd[22] = 0xc0;                 /* device attributes */
    spd[23] = 0x75;                 /* one latency down: the same cycle */
    spd[24] = 0x54;
    spd[27] = 0x50;                 /* tRP 20 ns */
    spd[28] = 0x3c;                 /* tRRD 15 ns */
    spd[29] = 0x50;                 /* tRCD 20 ns */
    spd[30] = 0x2d;                 /* tRAS 45 ns */
    spd[31] = dimm->density;
    spd[62] = 0x12;                 /* SPD revision 1.2 */
    for (i = 0; i < 63; i++) {
        sum += spd[i];
    }
    spd[63] = sum;
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

static bool longspeak_bmc_token(uint8_t *cmd, unsigned int cmd_len,
                                RspBuffer *rsp)
{
    unsigned int i;

    if (cmd_len < 4 || cmd[1] < LONGSPEAK_BMC_TOKEN_FIRST ||
        cmd[1] > LONGSPEAK_BMC_TOKEN_LAST) {
        return false;
    }

    for (i = 0; i < cmd[3]; i++) {
        rsp_buffer_push(rsp, 0);
    }
    return true;
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

static bool longspeak_bmc_fru(uint8_t *cmd, unsigned int cmd_len,
                              RspBuffer *rsp)
{
    uint8_t image[LONGSPEAK_BMC_FRU0_SIZE];
    const LongspeakBmcDimm *dimm;
    int slot;

    if (cmd_len < 3) {
        return false;
    }

    /*
     * The machine's memory decides which slots carry a module.  It is read
     * here and not held in the device because a subclass of ipmi-bmc-sim has
     * no instance state of its own: the parent's structure is private.
     */
    slot = longspeak_bmc_dimm_slot_of(cmd[2]);
    dimm = slot < 0 ? NULL
                    : longspeak_bmc_slot(current_machine->ram_size, slot);

    if (cmd[1] == LONGSPEAK_BMC_CMD_FRU_STATUS) {
        if (!dimm) {
            return false;       /* only a populated slot answers it */
        }
        rsp_buffer_push(rsp, 0x01);
        return true;
    }

    if (cmd[2] == LONGSPEAK_BMC_FRU0) {
        longspeak_bmc_board_fru(image);
        return longspeak_bmc_fru_image(cmd, cmd_len, rsp, image,
                                       LONGSPEAK_BMC_FRU0_SIZE);
    }

    if (cmd[2] >= LONGSPEAK_BMC_FRU_FIRST &&
        cmd[2] < LONGSPEAK_BMC_FRU_FIRST + LONGSPEAK_BMC_FRU_DEVICES) {
        return longspeak_bmc_fru_image(cmd, cmd_len, rsp, NULL,
                                       LONGSPEAK_BMC_FRU_SIZE);
    }

    if (!dimm) {
        return false;           /* no such device, or an empty slot */
    }
    longspeak_bmc_spd(dimm, image);
    return longspeak_bmc_fru_image(cmd, cmd_len, rsp, image,
                                   LONGSPEAK_BMC_SPD_SIZE);
}

static void longspeak_bmc_handle_command(IPMIBmc *s, uint8_t *cmd,
                                         unsigned int cmd_len,
                                         unsigned int max_cmd_len,
                                         uint8_t msg_id)
{
    LongspeakBmcClass *bc = LONGSPEAK_BMC_GET_CLASS(s);
    RspBuffer rsp = { };
    bool handled = false;

    if (cmd_len >= 2 && (cmd[0] & 0x03) == 0) {
        rsp_buffer_push(&rsp, cmd[0] | 0x04);
        rsp_buffer_push(&rsp, cmd[1]);
        rsp_buffer_push(&rsp, 0);

        switch (cmd[0] >> 2) {
        case LONGSPEAK_BMC_NETFN_TOKEN:
            handled = longspeak_bmc_token(cmd, cmd_len, &rsp);
            break;
        case LONGSPEAK_BMC_NETFN_STORAGE:
            if (cmd[1] == LONGSPEAK_BMC_CMD_FRU_INFO ||
                cmd[1] == LONGSPEAK_BMC_CMD_FRU_READ ||
                cmd[1] == LONGSPEAK_BMC_CMD_FRU_STATUS) {
                handled = longspeak_bmc_fru(cmd, cmd_len, &rsp);
            }
            break;
        default:
            break;
        }
    }

    if (!handled) {
        bc->parent_handle_command(s, cmd, cmd_len, max_cmd_len, msg_id);
        return;
    }
    IPMI_INTERFACE_GET_CLASS(s->intf)->handle_rsp(s->intf, msg_id, rsp.buffer,
                                                  rsp.len);
}

static void longspeak_bmc_class_init(ObjectClass *oc, const void *data)
{
    IPMIBmcClass *bk = IPMI_BMC_CLASS(oc);
    LongspeakBmcClass *bc = LONGSPEAK_BMC_CLASS(oc);

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
