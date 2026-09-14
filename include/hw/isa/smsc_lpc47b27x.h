#ifndef HW_ISA_SMSC_LPC47B27X_H
#define HW_ISA_SMSC_LPC47B27X_H

#include "qom/object.h"

#define TYPE_SMSC_LPC47B27X "smsc-lpc47b27x"
OBJECT_DECLARE_SIMPLE_TYPE(SMSCLPC47B27xState, SMSC_LPC47B27X)

/* UART2 (LDN 5, 2F8h IRQ 3) reported fitted and active. */
#define SMSC_LPC47B27X_PROP_UART2 "uart2"

#endif
