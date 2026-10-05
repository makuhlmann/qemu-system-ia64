#ifndef HW_ISA_SMSC_LPC47B27X_H
#define HW_ISA_SMSC_LPC47B27X_H

#include "hw/core/sysbus.h"
#include "hw/isa/isa.h"
#include "qom/object.h"

#define TYPE_SMSC_LPC47B27X "smsc-lpc47b27x"
OBJECT_DECLARE_SIMPLE_TYPE(SMSCLPC47B27xState, SMSC_LPC47B27X)

/*
 * Reset to the state the board firmware's chipset-init script leaves (see
 * hw/isa/smsc_lpc47b27x.c) instead of the chip's own.
 */
#define SMSC_LPC47B27X_PROP_FIRMWARE_INIT "firmware-init"

/*
 * The board's devices behind the chip's logical devices: the chip then
 * decodes their I/O ports and routes their interrupts.  UART1 is a SerialMM
 * mapped in the ISA I/O space; the keyboard controller is an i8042.
 */
void smsc_lpc47b27x_attach_uart1(SMSCLPC47B27xState *s, SysBusDevice *uart);
void smsc_lpc47b27x_attach_kbc(SMSCLPC47B27xState *s, ISADevice *kbc);

#endif
