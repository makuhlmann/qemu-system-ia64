#ifndef HW_I8259_H
#define HW_I8259_H

/* i8259.c */

typedef struct PICCommonState PICCommonState;

extern PICCommonState *isa_pic;

/*
 * i8259_init()
 *
 * Create a i8259 device on an ISA @bus,
 * connect its output to @parent_irq_in,
 * return an (allocated) array of 16 input IRQs.
 */
qemu_irq *i8259_init(ISABus *bus, qemu_irq parent_irq_in);

/*
 * Create an emulated master/slave pair without changing the legacy isa_pic
 * global.  If @master_pic is non-NULL, return the master PIC through it.
 * The returned input array is allocated as for i8259_init().
 *
 * The master/slave association is immutable construction topology.  It is
 * rebuilt by board code and is not part of migration state.
 */
qemu_irq *i8259_init_pair(ISABus *bus, qemu_irq parent_irq_in,
                          PICCommonState **master_pic);
qemu_irq *kvm_i8259_init(ISABus *bus);
int pic_get_output(PICCommonState *s);
int pic_read_irq(PICCommonState *s);

#endif
