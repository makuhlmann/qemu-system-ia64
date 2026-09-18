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
 */

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "hw/ipmi/ipmi.h"
#include "qom/object.h"
#include "longspeak_pdh.h"

#define LONGSPEAK_BMC_NETFN_TOKEN 0x32
#define LONGSPEAK_BMC_TOKEN_FIRST 0x01
#define LONGSPEAK_BMC_TOKEN_LAST  0x03

typedef struct LongspeakBmcClass {
    IPMIBmcClass parent_class;

    void (*parent_handle_command)(IPMIBmc *s, uint8_t *cmd,
                                  unsigned int cmd_len,
                                  unsigned int max_cmd_len, uint8_t msg_id);
} LongspeakBmcClass;

DECLARE_CLASS_CHECKERS(LongspeakBmcClass, LONGSPEAK_BMC, TYPE_LONGSPEAK_BMC)

static void longspeak_bmc_handle_command(IPMIBmc *s, uint8_t *cmd,
                                         unsigned int cmd_len,
                                         unsigned int max_cmd_len,
                                         uint8_t msg_id)
{
    LongspeakBmcClass *bc = LONGSPEAK_BMC_GET_CLASS(s);
    uint8_t rsp[3 + 0xff];
    unsigned int len;

    if (cmd_len < 4 || (cmd[0] >> 2) != LONGSPEAK_BMC_NETFN_TOKEN ||
        cmd[1] < LONGSPEAK_BMC_TOKEN_FIRST ||
        cmd[1] > LONGSPEAK_BMC_TOKEN_LAST) {
        bc->parent_handle_command(s, cmd, cmd_len, max_cmd_len, msg_id);
        return;
    }

    len = cmd[3];
    memset(rsp, 0, 3 + len);
    rsp[0] = cmd[0] | 0x04;
    rsp[1] = cmd[1];
    IPMI_INTERFACE_GET_CLASS(s->intf)->handle_rsp(s->intf, msg_id, rsp,
                                                  3 + len);
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
