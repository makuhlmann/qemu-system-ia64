/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * What SAL_INIT (entry.S, fw_sal_init) reads at INIT time, where it runs
 * without a stack or a register stack of its own: the OS_INIT registration
 * of SAL_SET_VECTORS, the hand-off values of SAL spec 245359-007 5.3 and the
 * platform reset for a warm boot.  Offsets for the assembler, the layout for C.
 */
#ifndef FW_SAL_INIT_H
#define FW_SAL_INIT_H

#define FW_SAL_INIT_VALID           0x00
#define FW_SAL_INIT_MONARCH_ENTRY   0x08
#define FW_SAL_INIT_SLAVE_ENTRY     0x20
/* Entry, gp and length_cs of one OS_INIT procedure. */
#define FW_SAL_INIT_GP              0x08
#define FW_SAL_INIT_LENGTH_CS       0x10
#define FW_SAL_INIT_PAL_PROC        0x38
#define FW_SAL_INIT_RESET_CONTROL   0x50
#define FW_SAL_INIT_ENTERED         0x60

#ifndef __ASSEMBLER__
typedef struct {
    UINT64 Valid;
    UINT64 MonarchEntry;
    UINT64 MonarchGp;
    UINT64 MonarchLengthCs;
    UINT64 SlaveEntry;
    UINT64 SlaveGp;
    UINT64 SlaveLengthCs;
    UINT64 PalProc;
    UINT64 SalProc;
    UINT64 SalGp;
    UINT64 ResetControl;
    UINT64 ResetValue;
    /* Processors in OS_INIT; the first one in is the monarch. */
    UINT64 Entered;
} FW_SAL_INIT_BLOCK;

_Static_assert(__builtin_offsetof(FW_SAL_INIT_BLOCK, MonarchEntry) ==
               FW_SAL_INIT_MONARCH_ENTRY, "FW_SAL_INIT_MONARCH_ENTRY");
_Static_assert(__builtin_offsetof(FW_SAL_INIT_BLOCK, SlaveEntry) ==
               FW_SAL_INIT_SLAVE_ENTRY, "FW_SAL_INIT_SLAVE_ENTRY");
_Static_assert(__builtin_offsetof(FW_SAL_INIT_BLOCK, MonarchLengthCs) -
               __builtin_offsetof(FW_SAL_INIT_BLOCK, MonarchEntry) ==
               FW_SAL_INIT_LENGTH_CS, "FW_SAL_INIT_LENGTH_CS");
_Static_assert(__builtin_offsetof(FW_SAL_INIT_BLOCK, PalProc) ==
               FW_SAL_INIT_PAL_PROC, "FW_SAL_INIT_PAL_PROC");
_Static_assert(__builtin_offsetof(FW_SAL_INIT_BLOCK, ResetControl) ==
               FW_SAL_INIT_RESET_CONTROL, "FW_SAL_INIT_RESET_CONTROL");
_Static_assert(__builtin_offsetof(FW_SAL_INIT_BLOCK, Entered) ==
               FW_SAL_INIT_ENTERED, "FW_SAL_INIT_ENTERED");

extern FW_SAL_INIT_BLOCK mFwSalInit;
#endif

#endif
