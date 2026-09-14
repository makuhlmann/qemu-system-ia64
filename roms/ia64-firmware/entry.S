/* SPDX-License-Identifier: GPL-2.0-or-later */
/* IA-64 EFI firmware entry point (GCC assembler syntax).
 *
 * The linker script links at VMA=0x100000, so all symbols (`__gp`,
 * firmware_main, etc.) are already at correct runtime addresses.
 * No GP manual offset computation is needed.
 */

#include "hw/ia64/ia64_vpc_abi.h"

    .section .text._start, "ax"
    /*
     * The boot memory stacks live at the top of installed low RAM.  The ABI
     * constants come from hw/ia64/ia64_vpc_abi.h (this file is assembled
     * with the C preprocessor); the per-CPU stride below (shl 17 = 128 KiB)
     * equals IA64_FW_CPU_STACK_SIZE, enforced by a static assert there.
     */
    .equ FW_HANDOFF_ADDR, IA64_FW_HANDOFF_ADDR
    .equ FW_HANDOFF_MAGIC, IA64_FW_HANDOFF_MAGIC
    .equ FW_HANDOFF_RAMSIZE_OFFSET, IA64_FW_HANDOFF_RAMSIZE_OFFSET
    .equ FW_PCI_MMIO_BASE, IA64_PCI_MMIO_BASE
    .equ FW_LOW_RAM_MIN, IA64_FW_LOW_RAM_MIN
    .equ FW_LOW_RAM_ALIGN_MASK, (IA64_FW_LOW_RAM_ALIGN - 1)
    .global _start
    .type _start, @function
    .proc _start
    .explicit
_start:
    /* Secondary processors resume here when the platform wake IPI arrives. */
    mov r14 = cr.lid
    ;;
    cmp.ne p6, p7 = r14, r0
    ;;
(p6) br.cond.sptk.few start_secondary
    ;;

    /* start_after_pal derives the RAM-top boot stack from the handoff. */
    br.cond.sptk.few start_after_pal
    ;;
    .org 0x60
    .endp _start
    .size _start, . - _start

    .section .text.pal_proc_entry, "ax"
    .global pal_proc_entry
    .type pal_proc_entry, @function
    .proc pal_proc_entry
    .explicit
pal_proc_entry:
    break.m 0x100000
    ;;
    br.many b0
    ;;
    .endp pal_proc_entry
    .size pal_proc_entry, . - pal_proc_entry

    /*
     * The SAL runtime stub trio lives in its own section, placed first in
     * the runtime code region: Windows Server 2003's HAL only records a
     * virtual SAL entry when the SST SalProc address falls inside an
     * EfiRuntimeServicesCode descriptor.
     */
    .section .text.sal_runtime, "ax"

    /*
     * SAL_PROC runtime entry (SST Entrypoint.SalProc).  Callable in
     * physical or virtual mode (SAL spec 3.1): the machine recognises the
     * break by the stub's physical address, re-enters the firmware's C
     * dispatcher physically on a firmware-owned stack, and resumes the
     * caller via sal_runtime_return.  Keeping the stub as a leaf preserves
     * all eight input registers; results return in r8-r11.
     */
    .global sal_runtime_entry
    .type sal_runtime_entry, @function
    .proc sal_runtime_entry
sal_runtime_entry:
    break.m 0x100005
    ;;
    br.ret.sptk.many b0
    ;;
    .endp sal_runtime_entry
    .size sal_runtime_entry, . - sal_runtime_entry

    /* Return trampoline the C dispatcher's br.ret lands on. */
    .global sal_runtime_return
    .type sal_runtime_return, @function
    .proc sal_runtime_return
sal_runtime_return:
    break.m 0x100006
    ;;
    br.ret.sptk.many b0
    ;;
    .endp sal_runtime_return
    .size sal_runtime_return, . - sal_runtime_return

    /*
     * Dispatch handshake block (4 quads, filled by efi_init_sal_tables):
     * +0x00 C dispatcher entry (physical), +0x08 firmware GP,
     * +0x10 physical stack top, +0x18 physical RSE backing store base.
     */
    .align 16
    .global sal_dispatch_block
sal_dispatch_block:
    .skip 0x20

    /*
     * The machine implements the arithmetic behind this architected
     * firmware ABI.  Keeping the bridge as a leaf preserves all eight input
     * registers and returns the four-word result directly in r8-r11.
     */
    .section .text.fpswa_emulation_entry, "ax"
    .global fpswa_emulation_entry
    .type fpswa_emulation_entry, @function
    .proc fpswa_emulation_entry
    .explicit
fpswa_emulation_entry:
    break.m 0x100001
    ;;
    br.ret.sptk.many b0
    ;;
    .endp fpswa_emulation_entry
    .size fpswa_emulation_entry, . - fpswa_emulation_entry

    /* Copied into an IVT slot while an EFI debug callback is registered. */
    .section .text.fw_debug_ivt_stub, "ax"
    .align 16
    .global fw_debug_ivt_stub_start
    .global fw_debug_ivt_stub_end
    .type fw_debug_ivt_stub_start, @function
    .proc fw_debug_ivt_stub_start
    .explicit
fw_debug_ivt_stub_start:
    movl r16 = fw_debug_exception_entry
    ;;
    break.m 0x100002
    ;;
fw_debug_ivt_stub_end:
    .endp fw_debug_ivt_stub_start
    .size fw_debug_ivt_stub_start, . - fw_debug_ivt_stub_start

    /*
     * The target bridge supplies the exception type, context record, a
     * private stack, and the processor index in r16-r18.  cover preserves
     * the interrupted frame in cr.ifs before calling ordinary C code.
     */
    .section .text.fw_debug_exception_entry, "ax"
    .align 16
    .global fw_debug_exception_entry
    .type fw_debug_exception_entry, @function
    .proc fw_debug_exception_entry
    .explicit
fw_debug_exception_entry:
    cover
    ;;
    break.m 0x100003
    ;;
    .align 16
    mov r19 = r18
    movl r1 = __gp
    ;;
    alloc r18 = ar.pfs, 0, 3, 3, 0
    ;;
    mov r35 = r16
    mov r36 = r17
    mov r37 = r19
    ;;
    br.call.sptk.many b0 = fw_debug_support_dispatch
    ;;
    break.m 0x100004
    ;;
    break.m 0
    ;;
    .endp fw_debug_exception_entry
    .size fw_debug_exception_entry, . - fw_debug_exception_entry

    .section .text.start_after_pal, "ax"
    .type start_after_pal, @function
    .proc start_after_pal
    .explicit
start_after_pal:

    /*
     * Derive the RAM-top boot stack from the machine handoff block (falling
     * back to the minimum machine when the handoff is absent).  The C side
     * (fw_boot_stack_top) recomputes the same value once it is running.
     */
    movl r14 = FW_HANDOFF_ADDR
    ;;
    ld8 r15 = [r14]
    adds r16 = FW_HANDOFF_RAMSIZE_OFFSET, r14
    ;;
    ld8 r17 = [r16]
    movl r18 = FW_HANDOFF_MAGIC
    ;;
    cmp.eq p6, p7 = r15, r18
    movl r19 = FW_PCI_MMIO_BASE
    ;;
(p7) movl r17 = FW_LOW_RAM_MIN
    movl r20 = FW_LOW_RAM_ALIGN_MASK
    ;;
    cmp.ltu p8, p9 = r19, r17
    ;;
(p8) mov r17 = r19
    ;;
    andcm r17 = r17, r20
    ;;
    adds r12 = -16, r17
    ;;


    /*
     * QEMU reloads the firmware image on reset, but the raw binary does not
     * contain the trailing NOBITS .bss.  Clear it here so guest-requested and
     * monitor-requested resets start with the same firmware state as a fresh
     * process.
     */
    movl r14 = _bss_start
    ;;
    movl r15 = _bss_end
    ;;
1:
    cmp.ltu p6, p7 = r14, r15
    ;;
(p7) br.cond.dpnt.few 2f
    ;;
    st8 [r14] = r0, 8
    ;;
    br.cond.sptk.few 1b
    ;;
2:
    ;;

    /* GP = linker-defined __gp (already at runtime VMA=0x100000 address) */
    movl r1 = __gp
    ;;

    /*
     * Preserve the reset return pointer across the ABI call, then move the
     * firmware stack to the top of installed low RAM.
     */
    mov r4 = b0
    ;;
    br.call.sptk.many b0 = fw_boot_stack_top
    ;;
    /*
     * The region top may be the exact end of installed RAM, and the psABI
     * lets callees use the 16-byte scratch area [sp, sp+16).  Start sp 16
     * bytes below the region top so that scratch area stays inside RAM.
     */
    adds r12 = -16, r8
    mov r3 = r4
    ;;

    /* Alloc: 0 ins, 13 locals, 3 outs → output regs r45-r47
     * After br.call these become callee's input r32-r34. */
    alloc r2 = ar.pfs, 0, 13, 3, 0
    ;;

    /* Pass args: gp, stack region top, boot_b0 */
    mov r45 = r1               /* arg0 → callee r32 = gp      */
    mov r46 = r8               /* arg1 → callee r33 = stack region top */
    mov r47 = r3               /* arg2 → callee r34 = boot_b0 */
    ;;

    br.call.sptk.many b0 = firmware_main
    ;;

    /* firmware_main should not return */
    break 0
    ;;
    .endp start_after_pal
    .size start_after_pal, . - start_after_pal

    .section .text.start_secondary, "ax"
    .type start_secondary, @function
    .proc start_secondary
    .explicit
start_secondary:
    movl r1 = __gp
    ;;
    mov r14 = cr.lid
    ;;
    extr.u r14 = r14, 24, 8
    ;;
    shl r15 = r14, 17
    movl r16 = FW_HANDOFF_ADDR
    ;;
    ld8 r17 = [r16]
    adds r18 = FW_HANDOFF_RAMSIZE_OFFSET, r16
    ;;
    ld8 r19 = [r18]
    movl r20 = FW_HANDOFF_MAGIC
    ;;
    cmp.eq p6, p7 = r17, r20
    movl r21 = FW_PCI_MMIO_BASE
    ;;
(p7) movl r19 = FW_LOW_RAM_MIN
    movl r22 = FW_LOW_RAM_ALIGN_MASK
    ;;
    cmp.ltu p8, p9 = r21, r19
    ;;
(p8) mov r19 = r21
    ;;
    andcm r19 = r19, r22
    ;;
    sub r12 = r19, r15
    ;;
    adds r12 = -16, r12
    ;;
    alloc r2 = ar.pfs, 0, 3, 1, 0
    ;;
    mov r32 = r14
    mov r35 = r14
    ;;
    br.call.sptk.many b0 = fw_ap_stack_top
    ;;
    adds r12 = -16, r8
    mov r35 = r32
    ;;
    br.call.sptk.many b0 = firmware_ap_main
    ;;
    break 0
    ;;
    .endp start_secondary
    .size start_secondary, . - start_secondary

    .section .text.fw_pal_halt_light, "ax"
    .global fw_pal_halt_light
    .type fw_pal_halt_light, @function
    .proc fw_pal_halt_light
    .explicit
fw_pal_halt_light:
    alloc r33 = ar.pfs, 0, 2, 0, 0
    ;;
    mov r32 = b0
    movl r28 = 0x1d
    ;;
    movl r14 = pal_proc_entry
    ;;
    mov b6 = r14
    movl r14 = 1f
    ;;
    mov b0 = r14
    ;;
    br.sptk.many b6
    ;;
1:
    mov b0 = r32
    mov ar.pfs = r33
    ;;
    br.ret.sptk.many b0
    ;;
    .endp fw_pal_halt_light
    .size fw_pal_halt_light, . - fw_pal_halt_light

    /* PAL_FREQ_RATIOS (index 0Eh): processor, bus and ITC ratios to the
     * base frequency, each (numerator << 32) | denominator. */
    .section .text.fw_pal_freq_ratios, "ax"
    .global fw_pal_freq_ratios
    .type fw_pal_freq_ratios, @function
    .proc fw_pal_freq_ratios
    .explicit
fw_pal_freq_ratios:
    alloc r36 = ar.pfs, 3, 2, 0, 0
    ;;
    mov r35 = b0
    movl r28 = 0xe
    mov r29 = r0
    mov r30 = r0
    mov r31 = r0
    ;;
    movl r14 = pal_proc_entry
    ;;
    mov b6 = r14
    movl r14 = 1f
    ;;
    mov b0 = r14
    ;;
    br.sptk.many b6
    ;;
1:
    st8 [r32] = r9
    st8 [r33] = r10
    ;;
    st8 [r34] = r11
    mov b0 = r35
    mov ar.pfs = r36
    ;;
    br.ret.sptk.many b0
    ;;
    .endp fw_pal_freq_ratios
    .size fw_pal_freq_ratios, . - fw_pal_freq_ratios

    .section .text.fw_ebc_bridge, "ax"

    /* A generated thunk passes its firmware-owned context in r8. */
    .global fw_ebc_capture_context
    .type fw_ebc_capture_context, @function
    .proc fw_ebc_capture_context
    .explicit
fw_ebc_capture_context:
    br.ret.sptk.many b0
    ;;
    .endp fw_ebc_capture_context
    .size fw_ebc_capture_context, . - fw_ebc_capture_context

    /*
     * Invoke an IA-64 function descriptor from an EBC argument frame.
     * The first eight natural-width arguments are loaded into the register
     * stack.  The complete frame is copied to a temporary native stack so
     * that additional arguments remain at their ABI-defined locations.
     */
    .global fw_ebc_call_native
    .type fw_ebc_call_native, @function
    .proc fw_ebc_call_native
    .explicit
fw_ebc_call_native:
    alloc loc1 = ar.pfs, 3, 9, 8, 0
    ;;
    mov loc0 = b0
    mov loc2 = r12
    mov loc3 = r1
    sub loc4 = in2, in1
    ;;
    sub loc5 = r12, loc4
    mov loc6 = in1
    mov loc8 = loc4
    ;;
    and loc5 = -16, loc5
    ;;
    mov loc7 = loc5
    ;;
1:
    cmp.eq p6, p7 = loc8, r0
    ;;
(p6) br.cond.dpnt.few 2f
    ;;
    ld1 r14 = [loc6], 1
    ;;
    st1 [loc7] = r14, 1
    add loc8 = -1, loc8
    ;;
    br.cond.sptk.few 1b
    ;;
2:
    mov r14 = loc5
    ;;
    ld8 out0 = [r14], 8
    ;;
    ld8 out1 = [r14], 8
    ;;
    ld8 out2 = [r14], 8
    ;;
    ld8 out3 = [r14], 8
    ;;
    ld8 out4 = [r14], 8
    ;;
    ld8 out5 = [r14], 8
    ;;
    ld8 out6 = [r14], 8
    ;;
    ld8 out7 = [r14], 8
    ;;
    add r12 = 48, loc5
    mov r14 = in0
    ;;
    ld8 r15 = [r14], 8
    ;;
    ld8 r16 = [r14]
    ;;
    mov b6 = r15
    mov r1 = r16
    ;;
    br.call.sptk.many b0 = b6
    ;;
    mov r12 = loc2
    mov r1 = loc3
    mov b0 = loc0
    mov ar.pfs = loc1
    ;;
    br.ret.sptk.many b0
    ;;
    .endp fw_ebc_call_native
    .size fw_ebc_call_native, . - fw_ebc_call_native
