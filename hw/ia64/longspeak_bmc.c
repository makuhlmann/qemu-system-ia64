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
 */

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "hw/ipmi/ipmi.h"
#include "qom/object.h"
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

static bool longspeak_bmc_fru(uint8_t *cmd, unsigned int cmd_len,
                              RspBuffer *rsp)
{
    unsigned int offset, count, i;

    if (cmd_len < 3 || cmd[2] < LONGSPEAK_BMC_FRU_FIRST ||
        cmd[2] >= LONGSPEAK_BMC_FRU_FIRST + LONGSPEAK_BMC_FRU_DEVICES) {
        return false;
    }

    if (cmd[1] == LONGSPEAK_BMC_CMD_FRU_INFO) {
        rsp_buffer_push(rsp, LONGSPEAK_BMC_FRU_SIZE & 0xff);
        rsp_buffer_push(rsp, LONGSPEAK_BMC_FRU_SIZE >> 8);
        rsp_buffer_push(rsp, 0); /* addressed by byte */
        return true;
    }

    if (cmd_len < 6) {
        rsp_buffer_set_error(rsp, IPMI_CC_REQUEST_DATA_LENGTH_INVALID);
        return true;
    }

    offset = cmd[3] | cmd[4] << 8;
    if (offset >= LONGSPEAK_BMC_FRU_SIZE) {
        rsp_buffer_set_error(rsp, IPMI_CC_PARM_OUT_OF_RANGE);
        return true;
    }

    count = MIN(cmd[5], LONGSPEAK_BMC_FRU_SIZE - offset);
    rsp_buffer_push(rsp, count);
    for (i = 0; i < count; i++) {
        rsp_buffer_push(rsp, 0);
    }
    return true;
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
                cmd[1] == LONGSPEAK_BMC_CMD_FRU_READ) {
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
