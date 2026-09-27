/* SPDX-License-Identifier: GPL-2.0-or-later */
/* IA-64 EFI firmware entry point (GCC assembler syntax).
 *
 * The linker script links at VMA=0x100000, so all symbols (`__gp`,
 * firmware_main, etc.) are already at correct runtime addresses.
 * No GP manual offset computation is needed.
 */

#include "hw/ia64/ia64_vpc_abi.h"
#include "fw-sal-init.h"

    /*
     * The buffer PAL copies itself into (PAL_COPY_PAL, SAL 3.2.3 step 9):
     * the image's first page, which the memory map's EfiPalCode descriptor
     * covers.  The SAL system table's PAL_PROC is the copy.
     */
    .section .text.pal_buffer, "ax"
    .global fw_pal_buffer
fw_pal_buffer:
    .skip IA64_FW_PAL_BUFFER_SIZE

    .section .text._start, "ax"
    /*
     * The boot memory stacks live at the top of installed low RAM.  The ABI
     * constants come from hw/ia64/ia64_vpc_abi.h (this file is assembled
     * with the C preprocessor); the per-CPU stride below (shl 17 = 128 KiB)
     * equals IA64_FW_CPU_STACK_SIZE, enforced by a static assert there.
     */
    .equ FW_PCI_MMIO_BASE, IA64_PCI_MMIO_BASE
    .equ FW_ZX1_MEMORY0_END, IA64_ZX1_MEMORY0_END
    .equ FW_CHIPSET_ZX1, IA64_FW_CHIPSET_ZX1
    .equ FW_LOW_RAM_MIN, IA64_FW_LOW_RAM_MIN
    .equ FW_LOW_RAM_ALIGN_MASK, (IA64_FW_LOW_RAM_ALIGN - 1)
    .equ FW_AP_RELEASE_BLOCK, IA64_FW_AP_RELEASE_BLOCK
    .equ FW_AP_PRESENT_WORD, (IA64_FW_AP_RELEASE_BLOCK + IA64_FW_AP_PRESENT_OFF)
    /* The release vector's bit in cr.irr3 (vectors 192-255). */
    .equ FW_AP_RELEASE_IRR3_BIT, (IA64_FW_AP_RELEASE_VECTOR - 192)
    .equ FW_LINK_BASE, IA64_FW_LINK_BASE
    /* The shadow sits this far below low_ram_end (IA64_FW_IMAGE_BASE_FOR). */
    .equ FW_SHADOW_BELOW_END, (IA64_FW_CPU_ASSIST_SIZE + \
                               IA64_FW_ACPI_REGION_SIZE + IA64_FW_IMAGE_SPAN)
    .equ FW_EARLY_RSE_BELOW_END, (IA64_FW_CPU_ASSIST_SIZE - \
                                  IA64_FW_EARLY_RSE_OFFSET)
    .equ FW_EARLY_RSE_SIZE, IA64_FW_EARLY_RSE_SIZE
    /* cr.dcr at the shadow entry: defer data and page-not-present faults. */
    .equ FW_DCR_BOOT, 0x300
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

    /*
     * The PAL component the flash image's FIT names (its PAL_A and PAL_B
     * entries): the PAL emulation's entry stub.  Nothing here calls it; PAL
     * is the emulator's, reached through GR34 and PAL_COPY_PAL.
     */
    .section .text.fit_pal, "ax"
    .global fw_fit_pal
    .type fw_fit_pal, @function
    .proc fw_fit_pal
    .explicit
fw_fit_pal:
    break.m 0x100000
    ;;
    br.many b0
    ;;
    .endp fw_fit_pal
    .size fw_fit_pal, . - fw_fit_pal

    /*
     * SALE_ENTRY (the flash image's reset pointer block names this): the
     * firmware's SAL_A stage, executing in place from the flash window at
     * the top of 4 GiB with the architected PALE_RESET exit state (SDM
     * vol. 2 11.2.2): bank 1 selected, CFM.sof = 96, no stack, no RSE
     * backing store, cr.iva pointing at PAL's capture table, GR33 the
     * geographic processor id.  Every processor arrives here together.
     *
     * The boot processor copies the image body to its RAM-top shadow
     * (IA64_FW_IMAGE_BASE_FOR), applies the self-relocation fixup table
     * there and enters it at start_after_pal.  The others wait for the
     * release IPI the shadow sends (IA64_FW_AP_RELEASE_VECTOR) and enter
     * the shadow's _start, which sends them down start_secondary.  Nothing
     * here reaches for an absolute address of its own: link-time values
     * are turned into flash offsets against the link base.
     */
    .section .text.flash_entry, "ax"
    .global fw_flash_entry
    .type fw_flash_entry, @function
    .proc fw_flash_entry
    .explicit
fw_flash_entry:
    /*
     * PALE_RESET calls SALE_ENTRY twice (SDM vol. 2 11.2.1): first with
     * function RECOVERY_CHECK (3) in bank-1 GR20, then with RESET (0).  This
     * firmware has no recovery to make: return to PAL_RESET through GR36 at
     * once, before anything is touched.
     */
    dep r2 = r20, r0, 0, 8
    ;;
    cmp.eq p6, p7 = 3, r2
    mov b6 = r36
    ;;
(p6) br.cond.sptk.few b6
    ;;
    /* INIT (2) comes from PALE_INIT in bank 0 (SDM vol. 2 11.4.2). */
    cmp.eq p6, p7 = 2, r2
    ;;
(p6) br.cond.spnt.few fw_flash_init
    ;;
    bsw.0
    ;;
    /*
     * GR34 is the PAL_PROC address PAL hands every processor here.  Keep it
     * in b2, which this stage and every C callee preserve: the shadow calls
     * PAL through it to register itself with PAL.
     */
    mov b2 = r34
    ;;
1:
    mov r14 = ip
    ;;
    movl r15 = 1b
    ;;
    movl r16 = FW_LINK_BASE
    ;;
    sub r15 = r15, r16
    ;;
    sub r7 = r14, r15           /* r7 = flash base of the image body */
    ;;

    /*
     * A bootstrap RSE backing store and memory stack in the DRAM every
     * machine has (the first 128 MiB, FW_LOW_RAM_MIN), laid out as the
     * CPU-assist region of a minimum machine; the boot processor moves to
     * the real region once it knows how much memory there is.
     *
     * The processor's LID is the geographic id PAL hands over in GR33, as
     * the vendor SDV firmware sets it (ids 0 and 3 on that board).
     * The id indexes this processor's bootstrap slots, and id 0 is the
     * boot processor.
     */
    dep r14 = r33, r0, 24, 8
    ;;
    mov cr.lid = r14
    movl r17 = FW_LOW_RAM_MIN
    ;;
    extr.u r21 = r14, 24, 8     /* r21 = processor id */
    movl r23 = FW_EARLY_RSE_BELOW_END
    ;;
    sub r26 = r17, r23
    shl r25 = r21, 15           /* FW_EARLY_RSE_SIZE = 32 KiB per CPU */
    shl r27 = r21, 17           /* FW_CPU_STACK_SIZE = 128 KiB per CPU */
    ;;
    add r26 = r26, r25
    sub r12 = r17, r27
    mov ar.rsc = r0
    ;;
    mov ar.bspstore = r26
    adds r12 = -16, r12
    ;;
    mov ar.rnat = r0
    movl r27 = 3
    ;;
    mov ar.rsc = r27
    cmp.ne p6, p7 = r21, r0
    ;;
(p6) br.cond.sptk.few flash_wait_shadow
    ;;

    /*
     * Boot processor: forget which processors waited before this reset.
     * The waiting processors mark themselves again (flash_wait_shadow).
     */
    movl r14 = FW_AP_PRESENT_WORD
    ;;
    st8 [r14] = r0
    ;;

    /* Ask the hardware what this machine is and how much DRAM it has. */
    alloc r2 = ar.pfs, 0, 0, 1, 0
    adds r12 = -32, r12         /* a frame of our own above the callee's */
    ;;
    adds r32 = 16, r12          /* &chipset, past the callee's scratch area */
    ;;
    br.call.sptk.many b0 = fw_flash_probe
    ;;
    adds r14 = 16, r12
    mov r4 = r8                 /* r4 = installed DRAM, bytes */
    ;;
    ld8 r3 = [r14]              /* r3 = chipset (IA64_FW_CHIPSET_*) */
    movl r19 = FW_PCI_MMIO_BASE
    movl r22 = FW_LOW_RAM_ALIGN_MASK
    ;;
    /* The zx1 DRAM run at 0 ends at the I/O virtual region (mio ERS 2.1). */
    cmp.eq p6, p7 = FW_CHIPSET_ZX1, r3
    movl r18 = FW_ZX1_MEMORY0_END
    ;;
(p6) mov r19 = r18
    ;;
    cmp.ltu p8, p9 = r19, r8
    mov r17 = r8
    ;;
(p8) mov r17 = r19
    ;;
    andcm r17 = r17, r22        /* r17 = low_ram_end */
    ;;

    /*
     * The shadow base, and the boot processor's real early RSE slot and
     * boot stack at the top of low RAM.  The bases live in preserved
     * registers (r4-r7) across the relocator call, which is free to
     * clobber r14-r31.
     */
    movl r23 = FW_SHADOW_BELOW_END
    ;;
    sub r24 = r17, r23
    movl r25 = 0xfffff
    ;;
    andcm r5 = r24, r25         /* r5 = shadow base */
    movl r23 = FW_EARLY_RSE_BELOW_END
    movl r16 = FW_LINK_BASE     /* the probe call clobbered r16 */
    ;;
    sub r6 = r5, r16            /* r6 = shadow base - link base */
    sub r26 = r17, r23
    ;;
    flushrs
    ;;
    mov ar.rsc = r0
    ;;
    mov ar.bspstore = r26
    ;;
    mov ar.rnat = r0
    movl r27 = 3
    ;;
    mov ar.rsc = r27
    adds r12 = -16, r17
    ;;

    /* Copy the body: its byte length is patched in by fw-flash.py. */
    movl r15 = fw_flash_body_size
    ;;
    sub r15 = r15, r16
    ;;
    add r15 = r15, r7
    ;;
    ld8 r28 = [r15]
    mov r29 = r7
    mov r30 = r5
    ;;
    mov r7 = r28                /* r7 = body length (flash base is done) */
2:
    cmp.eq p6, p7 = r28, r0
    ;;
(p6) br.cond.dpnt.few 3f
    ;;
    ld8 r31 = [r29], 8
    ;;
    st8 [r30] = r31, 8
    adds r28 = -8, r28
    ;;
    br.cond.sptk.few 2b
    ;;
3:
    /* fw_flash_relocate(shadow, shadow - link base, body length). */
    alloc r2 = ar.pfs, 0, 0, 3, 0
    ;;
    mov r32 = r5
    mov r33 = r6
    mov r34 = r7
    ;;
    br.call.sptk.many b0 = fw_flash_relocate
    ;;
    sync.i
    ;;
    srlz.i
    ;;

    /*
     * Enter the shadow as the flat image was entered, with what the probe
     * found: r4 = DRAM bytes, r3 = chipset.  The application processors
     * stay parked until the platform layer releases them.
     */
    movl r15 = start_after_pal
    ;;
    add r15 = r15, r6
    ;;
    mov b6 = r15
    mov b0 = r0
    ;;
    br.sptk.many b6
    ;;

    /*
     * Application processors: wait for the boot processor's release IPI,
     * read the shadow's _start (release block +0) and low_ram_end (+8),
     * move the RSE backing store to this processor's real early slot, and
     * run the shadow's reset entry, which sends them down start_secondary.
     * External interrupts are disabled here, so poll the vector's request
     * bit; then acknowledge it (cr.ivr, cr.eoi), so that it is not still
     * pending when the shadow's rendezvous loop reads cr.ivr.
     *
     * While it waits, the processor keeps its bit set in the release block's
     * present word (IA64_FW_AP_PRESENT_OFF): the boot processor clears the
     * word at its reset entry and then waits for every marked processor to
     * check in.  The bit is set with cmpxchg8, because the other waiting
     * processors update the same word.
     */
flash_wait_shadow:
    movl r14 = FW_AP_PRESENT_WORD
    movl r24 = 1
    ;;
    shl r24 = r24, r21          /* this processor's present bit */
4:
    mov r15 = cr.irr3
    ;;
    tbit.nz p6, p7 = r15, FW_AP_RELEASE_IRR3_BIT
    ;;
(p6) br.cond.dpnt.few 5f
    ;;
    ld8 r16 = [r14]
    ;;
    and r17 = r16, r24
    ;;
    cmp.ne p6, p7 = r17, r0
    ;;
(p6) br.cond.sptk.few 4b
    ;;
    or r18 = r16, r24
    mov ar.ccv = r16
    ;;
    cmpxchg8.acq r17 = [r14], r18, ar.ccv
    ;;
    br.cond.sptk.few 4b
    ;;
5:
    mov r15 = cr.ivr
    ;;
    srlz.d
    ;;
    mov cr.eoi = r0
    ;;
    srlz.d
    movl r14 = FW_AP_RELEASE_BLOCK
    ;;
    ld8 r15 = [r14]
    adds r16 = 8, r14
    movl r22 = FW_LOW_RAM_ALIGN_MASK
    ;;
    ld8 r17 = [r16]
    movl r23 = FW_EARLY_RSE_BELOW_END
    ;;
    andcm r17 = r17, r22        /* low_ram_end */
    ;;
    sub r26 = r17, r23
    ;;
    add r26 = r26, r25          /* this processor's early RSE slot */
    ;;
    flushrs
    ;;
    mov ar.rsc = r0
    ;;
    mov ar.bspstore = r26
    ;;
    mov ar.rnat = r0
    movl r27 = 3
    ;;
    mov ar.rsc = r27
    mov b6 = r15                /* the shadow's _start */
    ;;
    br.sptk.many b6
    ;;
    .endp fw_flash_entry
    .size fw_flash_entry, . - fw_flash_entry

    /*
     * INIT: the shadow keeps its SAL_INIT entry in the SAL part of the
     * min-state save area PALE_INIT hands over in GR17.  With no area or no
     * entry there is no state to resume, and the processor stops here.
     */
    .proc fw_flash_init
    .explicit
fw_flash_init:
    adds r24 = IA64_FW_MINSTATE_SAL_INIT_OFF, r17
    cmp.eq p6, p7 = r0, r17
    ;;
(p7) ld8 r24 = [r24]
(p6) mov r24 = r0
    ;;
    cmp.eq p6, p7 = r0, r24
    mov b6 = r24
    ;;
(p7) br.cond.sptk.few b6
    ;;
1:
    br.cond.sptk.few 1b
    ;;
    .endp fw_flash_init

    /* Byte length of the image body to shadow; fw-flash.py fills it in. */
    .align 8
    .global fw_flash_body_size
fw_flash_body_size:
    .quad 0

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
     * SAL_INIT (SAL spec 245359-007 5.1-5.4), reached from SALE_ENTRY with
     * the PALE_INIT exit state: bank 0, PSR.mc = 1, physical addressing, GR17
     * the min-state save area.  PAL saved every static register there, so
     * this code uses static registers only and leaves the register stack and
     * its backing store as PALE_INIT left them: OS_INIT gets them so (5.3).
     */
    .section .text.fw_sal_init, "ax"
    .global fw_sal_init
    .type fw_sal_init, @function
    .proc fw_sal_init
    .explicit
fw_sal_init:
    adds r2 = IA64_FW_MINSTATE_OS_OWNED_OFF, r17
    movl r21 = mFwSalInit
    ;;
    ld8 r3 = [r2]
    ld8 r4 = [r21]
    mov r22 = r17
    mov r10 = r0
    ;;
    /* A processor in SAL boot rendezvous stays there (5.1). */
    cmp.eq p6, p0 = r0, r3
(p6) br.cond.spnt.few fw_sal_init_resume
    ;;
    /* No OS_INIT registered: warm boot (5.1). */
    cmp.eq p6, p0 = r0, r4
(p6) br.cond.spnt.few fw_sal_init_warm_boot
    adds r2 = FW_SAL_INIT_ENTERED, r21
    ;;
    /* The first processor in is the monarch (5.2). */
    fetchadd8.acq r3 = [r2], 1
    adds r4 = FW_SAL_INIT_MONARCH_ENTRY, r21
    adds r5 = FW_SAL_INIT_SLAVE_ENTRY, r21
    ;;
    cmp.eq p6, p7 = r0, r3
    ;;
(p7) mov r4 = r5
    ;;
    adds r5 = FW_SAL_INIT_LENGTH_CS, r4
    adds r6 = FW_SAL_INIT_GP, r4
    ;;
    ld8 r3 = [r4]
    ld8 r5 = [r5]
    ld8 r1 = [r6]
    ;;
    /* The code and its checksum byte sum to zero, or no jump (5.1). */
    tbit.z p6, p0 = r5, 32
    extr.u r6 = r5, 0, 32
    extr.u r7 = r5, 40, 8
    mov r9 = r3
(p6) br.cond.sptk.few 3f
    ;;
1:
    cmp.eq p6, p0 = r0, r6
(p6) br.cond.spnt.few 2f
    ;;
    ld1 r8 = [r9], 1
    adds r6 = -1, r6
    ;;
    add r7 = r7, r8
    br.cond.sptk.few 1b
    ;;
2:
    zxt1 r7 = r7
    ;;
    cmp.ne p6, p0 = r0, r7
(p6) br.cond.spnt.few fw_sal_init_warm_boot
    ;;
3:
    /* OS_INIT hand-off (5.3). */
    adds r2 = FW_SAL_INIT_PAL_PROC, r21
    ;;
    ld8 r8 = [r2], 8
    ;;
    ld8 r9 = [r2], 8
    ;;
    ld8 r10 = [r2]
    mov r11 = r0
    movl r12 = fw_sal_init_return
    mov b6 = r3
    ;;
    br.sptk.many b6
    ;;
fw_sal_init_return:
    /* OS_INIT hands back (5.4): GR8 0 resume, -1 warm boot; GR10, GR22. */
    movl r21 = mFwSalInit
    ;;
    adds r2 = FW_SAL_INIT_ENTERED, r21
    cmp.eq p6, p0 = -1, r8
    ;;
    fetchadd8.rel r3 = [r2], -1
(p6) br.cond.spnt.few fw_sal_init_warm_boot
    ;;
fw_sal_init_resume:
    movl r21 = mFwSalInit
    ;;
    adds r2 = FW_SAL_INIT_PAL_PROC, r21
    mov r28 = 26                /* PAL_MC_RESUME */
    mov r29 = r0
    mov r30 = r22
    mov r31 = r10
    ;;
    ld8 r2 = [r2]
    ;;
    mov b6 = r2
    ;;
    br.call.sptk.many b0 = b6
    ;;
    /* PAL_MC_RESUME returns only on an error. */
fw_sal_init_warm_boot:
    movl r21 = mFwSalInit
    ;;
    adds r2 = FW_SAL_INIT_RESET_CONTROL, r21
    ;;
    ld8 r3 = [r2], 8
    ;;
    ld8 r4 = [r2]
    ;;
    st1 [r3] = r4
    ;;
4:
    br.cond.sptk.few 4b
    ;;
    .endp fw_sal_init
    .size fw_sal_init, . - fw_sal_init

    /*
     * SALE_PMI (SAL spec 245359-007 6.3; SDM Vol. 2 11.5.2-11.5.3): physical
     * mode, bank 0, PSR.ic = 0, GR24 the PMI vector.  It may use GR24-25,
     * GR30-31, the predicates and BR1, and returns through BR0.  Only the
     * PMI pin (vector 0) has a source: on the 460GX board the IFB's SMI#,
     * for the FADT's SMI command port.  Every processor takes the pin, and
     * what each does here gives the same result in any order.
     * PAL_PMI_ENTRYPOINT wants the entry 256-byte aligned.
     */
    .section .text.fw_sal_pmi, "ax"
    .align 256
    .global fw_sal_pmi
    .type fw_sal_pmi, @function
    .proc fw_sal_pmi
    .explicit
fw_sal_pmi:
    movl r30 = mFwSalPmiIoBase
    cmp.ne p6, p0 = r0, r24
(p6) br.cond.spnt.few fw_sal_pmi_return
    ;;
    ld8 r30 = [r30]
    movl r31 = IA64_460GX_ACPI_PM_IO_BASE + IA64_460GX_ACPI_GLBSTS_OFFSET
    ;;
    cmp.eq p6, p0 = r0, r30
(p6) br.cond.spnt.few fw_sal_pmi_return
    add r31 = r31, r30
    ;;
    /* An APMC write set APM_STS; clear it and read the command. */
    ld2 r24 = [r31]
    ;;
    tbit.z p6, p0 = r24, 3
(p6) br.cond.spnt.few fw_sal_pmi_eos
    mov r24 = IA64_460GX_GLBSTS_APM_STS
    ;;
    st2 [r31] = r24
    movl r31 = IA64_460GX_SMI_CMD_PORT
    ;;
    add r31 = r31, r30
    ;;
    ld1 r24 = [r31]
    movl r31 = IA64_460GX_ACPI_PM_IO_BASE
    mov r25 = IA64_460GX_ACPI_ENABLE_CMD
    ;;
    add r31 = r31, r30
    cmp.eq p6, p0 = r24, r25
    mov r25 = IA64_460GX_ACPI_DISABLE_CMD
    ;;
    cmp.eq p7, p0 = r24, r25
    ;;
    /*
     * ACPI_ENABLE as the vendor handler answers it (SAL_B run-time
     * 0x3FF1A4C0): PM1 status cleared, the power button enabled in PM1_EN,
     * SCI_EN set in PM1_CNT.  ACPI_DISABLE clears SCI_EN.
     */
(p6) mov r25 = -1
(p7) adds r31 = 4, r31
    ;;
(p6) st2 [r31] = r25, 2
    ;;
(p6) ld2 r25 = [r31]
    ;;
(p6) dep r25 = -1, r25, 8, 1
    ;;
(p6) st2 [r31] = r25, 2
    ;;
(p6) ld2 r25 = [r31]
    ;;
(p6) or r25 = 1, r25
    ;;
(p6) st2 [r31] = r25
(p7) ld2 r25 = [r31]
    ;;
(p7) and r25 = -2, r25
    ;;
(p7) st2 [r31] = r25
    ;;
fw_sal_pmi_eos:
    /* EOS: SMI# goes inactive, and a cause still pending raises it again. */
    movl r31 = IA64_460GX_ACPI_PM_IO_BASE + IA64_460GX_ACPI_GLBCTL_OFFSET
    ;;
    add r31 = r31, r30
    ;;
    ld2 r24 = [r31]
    ;;
    or r24 = IA64_460GX_GLBCTL_EOS, r24
    ;;
    st2 [r31] = r24
    ;;
fw_sal_pmi_return:
    br.sptk.few b0
    ;;
    .endp fw_sal_pmi
    .size fw_sal_pmi, . - fw_sal_pmi

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
     * A bootstrap memory stack at the top of the DRAM every machine has;
     * the C side moves it to the top of installed low RAM once it holds
     * what the flash stage probed (r4 = DRAM bytes, r3 = chipset).
     */
    movl r17 = FW_LOW_RAM_MIN
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

    /* The reset PAL_PROC address (b2, from fw_flash_entry), for the C side. */
    movl r14 = mFwResetPalProc
    mov r15 = b2
    ;;
    st8 [r14] = r15
    ;;

    /*
     * Preserve the reset return pointer across the ABI calls, then move the
     * firmware stack to the top of installed low RAM.
     */
    mov r5 = b0
    ;;
    br.call.sptk.many b0 = fw_shadow_control_registers
    ;;
    alloc r2 = ar.pfs, 0, 0, 2, 0
    ;;
    mov r32 = r4
    mov r33 = r3
    ;;
    br.call.sptk.many b0 = fw_platform_set_probed
    ;;
    br.call.sptk.many b0 = fw_boot_stack_top
    ;;
    /*
     * The region top may be the exact end of installed RAM, and the psABI
     * lets callees use the 16-byte scratch area [sp, sp+16).  Start sp 16
     * bytes below the region top so that scratch area stays inside RAM.
     */
    adds r12 = -16, r8
    mov r3 = r5
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

    /*
     * The control registers a processor holds while it runs this image:
     * cr.iva at the image's own IVT, cr.dcr deferring speculative faults,
     * ar.k0 naming the image base for the SAL runtime.  PAL leaves them at
     * its own values (the machine's capture IVT, zero); the flat-image
     * loader used to seed them.  Leaf, clobbers r14 only.
     */
    .section .text.fw_shadow_control_registers, "ax"
    .type fw_shadow_control_registers, @function
    .proc fw_shadow_control_registers
    .explicit
fw_shadow_control_registers:
    movl r14 = __fw_ivt
    ;;
    mov cr.iva = r14
    movl r14 = FW_DCR_BOOT
    ;;
    mov cr.dcr = r14
    movl r14 = __fw_image_start
    ;;
    mov ar.k0 = r14
    ;;
    srlz.i
    ;;
    br.ret.sptk.many b0
    ;;
    .endp fw_shadow_control_registers
    .size fw_shadow_control_registers, . - fw_shadow_control_registers

    .section .text.start_secondary, "ax"
    .type start_secondary, @function
    .proc start_secondary
    .explicit
start_secondary:
    movl r1 = __gp
    ;;
    mov r4 = b0
    ;;
    br.call.sptk.many b0 = fw_shadow_control_registers
    ;;
    mov b0 = r4
    ;;
    mov r14 = cr.lid
    ;;
    extr.u r14 = r14, 24, 8
    ;;
    shl r15 = r14, 17
    movl r19 = FW_LOW_RAM_MIN
    ;;
    sub r12 = r19, r15
    ;;
    adds r12 = -16, r12
    ;;
    alloc r2 = ar.pfs, 0, 3, 2, 0
    ;;
    mov r32 = r14
    mov r35 = r14
    ;;
    br.call.sptk.many b0 = fw_ap_stack_top
    ;;
    adds r12 = -16, r8
    mov r35 = r32
    mov r36 = b2                /* the reset PAL_PROC address */
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
    movl r14 = mFwPalProc
    ;;
    ld8 r14 = [r14]
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
    movl r14 = mFwPalProc
    ;;
    ld8 r14 = [r14]
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

    /*
     * PAL procedure calls through a given PAL_PROC address:
     * fw_pal_call_at(entry, index, arg1, arg2, arg3, results) for a static
     * procedure (a plain branch, SDM vol. 2 11.10.2), and
     * fw_pal_stacked_call_at with the same arguments for a stacked one (a
     * br.call with the index and arguments as outputs).  Both return r8,
     * the status, and store r9-r11 through results unless it is NULL.
     */
    .section .text.fw_pal_call_at, "ax"
    .global fw_pal_call_at
    .type fw_pal_call_at, @function
    .proc fw_pal_call_at
    .explicit
fw_pal_call_at:
    alloc r38 = ar.pfs, 6, 2, 0, 0
    ;;
    mov r39 = b0
    mov r28 = r33
    mov r29 = r34
    mov r30 = r35
    mov r31 = r36
    ;;
    mov b6 = r32
    movl r14 = 1f
    ;;
    mov b0 = r14
    ;;
    br.sptk.many b6
    ;;
1:
    cmp.eq p6, p7 = r37, r0
    ;;
(p6) br.cond.dpnt.few 2f
    ;;
    st8 [r37] = r9
    adds r15 = 8, r37
    ;;
    st8 [r15] = r10
    adds r15 = 16, r37
    ;;
    st8 [r15] = r11
    ;;
2:
    mov b0 = r39
    mov ar.pfs = r38
    ;;
    br.ret.sptk.many b0
    ;;
    .endp fw_pal_call_at
    .size fw_pal_call_at, . - fw_pal_call_at

    .section .text.fw_pal_stacked_call_at, "ax"
    .global fw_pal_stacked_call_at
    .type fw_pal_stacked_call_at, @function
    .proc fw_pal_stacked_call_at
    .explicit
fw_pal_stacked_call_at:
    alloc r38 = ar.pfs, 6, 2, 4, 0
    ;;
    mov r39 = b0
    mov r28 = r33
    mov r40 = r33
    mov r41 = r34
    mov r42 = r35
    mov r43 = r36
    ;;
    mov b6 = r32
    ;;
    br.call.sptk.many b0 = b6
    ;;
    cmp.eq p6, p7 = r37, r0
    ;;
(p6) br.cond.dpnt.few 2f
    ;;
    st8 [r37] = r9
    adds r15 = 8, r37
    ;;
    st8 [r15] = r10
    adds r15 = 16, r37
    ;;
    st8 [r15] = r11
    ;;
2:
    mov b0 = r39
    mov ar.pfs = r38
    ;;
    br.ret.sptk.many b0
    ;;
    .endp fw_pal_stacked_call_at
    .size fw_pal_stacked_call_at, . - fw_pal_stacked_call_at

    /*
     * PAL_LOGICAL_TO_PHYSICAL (index 2Ah) for the calling processor
     * (proc_number -1): r8 the status, *Info the first return.
     */
    .section .text.fw_pal_logical_to_physical, "ax"
    .global fw_pal_logical_to_physical
    .type fw_pal_logical_to_physical, @function
    .proc fw_pal_logical_to_physical
    .explicit
fw_pal_logical_to_physical:
    alloc r34 = ar.pfs, 1, 2, 0, 0
    ;;
    mov r33 = b0
    movl r28 = 0x2a
    mov r29 = -1
    mov r30 = r0
    mov r31 = r0
    ;;
    movl r14 = mFwPalProc
    ;;
    ld8 r14 = [r14]
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
    mov b0 = r33
    mov ar.pfs = r34
    ;;
    br.ret.sptk.many b0
    ;;
    .endp fw_pal_logical_to_physical
    .size fw_pal_logical_to_physical, . - fw_pal_logical_to_physical

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
