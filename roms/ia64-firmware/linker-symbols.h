/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef IA64_FIRMWARE_LINKER_SYMBOLS_H
#define IA64_FIRMWARE_LINKER_SYMBOLS_H

extern UINT8 __runtime_code_start;
extern UINT8 __runtime_data_start;
extern UINT8 _end;
extern UINT8 pal_proc_entry[];
extern UINT8 sal_runtime_entry[];
extern UINT8 sal_runtime_return[];
extern UINT8 sal_dispatch_block[];

#endif
