/* SPDX-License-Identifier: GPL-2.0-or-later */
/* IA-64 EFI firmware linker script. Linked at the machine-model load address. */

ENTRY(_start)

/*
 * The link base is overridable (ld --defsym FW_LINK_BASE=...) so the build
 * can produce alternate links whose binary diff yields the self-relocation
 * fixup table (see fw-fixups.py) - phase 2.2 of the firmware rework.  The
 * shipping image stays linked at the historical 0x100000.
 */
PROVIDE(FW_LINK_BASE = 0x100000);
__fw_link_base = FW_LINK_BASE;

SECTIONS
{
    . = __fw_link_base;
    __fw_image_start = .;

    .text.boot : {
        *(.text._start)
        *(.text.pal_proc_entry)
        *(.text.start_after_pal)
        *(.text.start_secondary)
        *(.text.fw_pal_halt_light)
        *(.text.fw_pal_freq_ratios)
    }

    . = ALIGN(0x2000);
    __runtime_code_start = .;

    /*
     * The SAL runtime stub trio leads the runtime code region: Windows
     * Server 2003's HAL requires the SST SalProc address to fall inside an
     * EfiRuntimeServicesCode descriptor.  The machine recognises the stubs
     * by fixed physical address (IA64_FW_SAL_RUNTIME_ENTRY_PA et al.), so
     * pin the region base.
     */
    .text.sal_runtime : {
        *(.text.sal_runtime)
    }

    /*
     * The firmware IVT, pinned at image base + 0x8000 (naturally 32 KB
     * aligned since the shadow base is 1 MB aligned) so the machine can
     * seed cr.iva as base + IA64_FW_IVT_OFFSET without knowing the link
     * layout.  Zero-filled: an all-zero bundle is a break instruction, the
     * historical machine-side IVT fill.  Lives inside the runtime-code
     * descriptor (handlers execute here).
     */
    . = __fw_link_base + 0x8000;
    .fw_ivt : {
        __fw_ivt = .;
        QUAD(0);
        . = __fw_ivt + 0x8000;
    }

    .text : {
        *(.text*)
        *(.plt*)
    }

    . = ALIGN(0x2000);
    __runtime_data_start = .;

    . = ALIGN(16);

    .rodata : {
        *(.rodata*)
    }

    . = ALIGN(16);

    .opd : {
        *(.opd)
    }

    . = ALIGN(16);

    .IA_64.unwind_info : {
        *(.IA_64.unwind_info*)
    }

    . = ALIGN(16);

    .IA_64.unwind : {
        *(.IA_64.unwind*)
    }

    /*
     * Reserved space for the self-relocation fixup table.  The three build
     * links leave it zero; fw-fixups.py injects the table into the final
     * binary at this offset, so the table's presence never perturbs the
     * layout it describes.  Its content is offsets, not addresses, hence
     * position-independent.
     */
    . = ALIGN(16);
    .fw_fixups : {
        __fw_fixups_start = .;
        QUAD(0);
        . = __fw_fixups_start + 0x6000;
        __fw_fixups_end = .;
    }

    .data : {
        *(.data*)
    }

    . = ALIGN(16);

    .got : {
        __gp_start = .;
        *(.got*)
    }

    . = ALIGN(16);

    .bss : {
        _bss_start = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(8);
        _bss_end = .;
    }

    _end = .;
}

/*
 * The machine hardcodes the SAL runtime stub addresses (target/ia64/cpu.h),
 * expressed link-base-relative so the alternate fixup-extraction links pass.
 */
ASSERT(sal_runtime_entry == __fw_link_base + 0x2000,
       "sal_runtime_entry must sit at IA64_FW_SAL_RUNTIME_ENTRY_PA (base+0x2000)")
ASSERT(sal_runtime_return == __fw_link_base + 0x2020,
       "sal_runtime_return must sit at IA64_FW_SAL_RUNTIME_RETURN_PA (base+0x2020)")
ASSERT(sal_dispatch_block == __fw_link_base + 0x2040,
       "sal_dispatch_block must sit at IA64_FW_SAL_DISPATCH_BLOCK_PA (base+0x2040)")
ASSERT(__fw_ivt == __fw_link_base + 0x8000,
       "the firmware IVT must sit at image base + IA64_FW_IVT_OFFSET")
/* The machine reserves IA64_FW_IMAGE_SPAN (4 MiB) for image + bss. */
ASSERT(_end - __fw_link_base <= 0x400000,
       "firmware image + bss exceed IA64_FW_IMAGE_SPAN")
