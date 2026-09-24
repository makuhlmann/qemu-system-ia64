"""PAL procedure and firmware-contract microprograms."""

from __future__ import annotations

import os
import struct
import tempfile

from . import encoding as _enc
from .case import (CaseMetadata, CaseObservation, IA64Case, bind_cases)
from .encoding import (
    IA64_CR_ITM,
    IA64_CR_ITV,
    IA64_EXCP_NONE,
    IA64_MERCED_DTR_COUNT,
    IA64_MERCED_ITR_COUNT,
    IA64_TR_COUNT,
    PAL_AR_IMPLEMENTED_HIGH,
    PAL_AR_IMPLEMENTED_LOW,
    PAL_BRAND_BUFFER,
    PAL_BRAND_INFO,
    PAL_BUS_GET_FEATURES,
    PAL_BUS_SET_FEATURES,
    PAL_CACHE_FLUSH,
    PAL_CACHE_INFO,
    PAL_CACHE_INFO_L0_2,
    PAL_CACHE_INFO_L0_D_1,
    PAL_CACHE_INFO_L0_I_1,
    PAL_CACHE_INFO_L1_D_1,
    PAL_CACHE_INFO_L1_D_2,
    PAL_CACHE_INFO_L1_I_1,
    PAL_CACHE_INFO_L1_I_2,
    PAL_CACHE_INFO_L2_U_1,
    PAL_CACHE_INFO_L2_U_2,
    PAL_CACHE_INIT,
    PAL_CACHE_LINE_INIT,
    PAL_CACHE_PROT_DATA_NONE,
    PAL_CACHE_PROT_INFO,
    PAL_CACHE_PROT_TAG_NONE_L0,
    PAL_CACHE_SHARED_INFO,
    PAL_CACHE_SUMMARY,
    PAL_COPY_BUFFER_ALIGN,
    PAL_COPY_BUFFER_SIZE,
    PAL_COPY_INFO,
    PAL_COPY_PAL,
    PAL_COPY_TARGET,
    PAL_CR_IMPLEMENTED_HIGH,
    PAL_CR_IMPLEMENTED_LOW,
    PAL_CR_READ_SIDE_EFFECT_HIGH,
    PAL_DEBUG_INFO,
    PAL_FIXED_ADDR,
    PAL_FREQ_BASE,
    PAL_FREQ_RATIOS,
    PAL_HALT,
    PAL_HALT_INFO,
    PAL_HALT_INFO_BUFFER,
    PAL_HALT_LIGHT,
    PAL_HALT_LIGHT_INFO,
    PAL_HALT_STATE1_INFO,
    PAL_INSERTABLE_PAGE_SIZE_MASK,
    PAL_INTERRUPT_BLOCK_DEFAULT,
    PAL_IO_BLOCK_DEFAULT,
    PAL_IO_BLOCK_MERCED,
    PAL_LOGICAL_TO_PHYSICAL,
    PAL_MC_CLEAR_LOG,
    PAL_MC_DRAIN,
    PAL_MC_DYNAMIC_STATE,
    PAL_MC_ERROR_INFO,
    PAL_MC_EXPECTED,
    PAL_MC_REGISTER_MEM,
    PAL_MC_RESUME,
    PAL_MEM_ATTRIB,
    PAL_MEM_ATTRIB_WB_UC_UCE_WC,
    PAL_MERCED_INSERTABLE_PAGE_SIZE_MASK,
    PAL_MERCED_PURGE_PAGE_SIZE_MASK,
    PAL_CACHE_INFO_MERCED_L0_D_1,
    PAL_CACHE_INFO_MERCED_L0_I_1,
    PAL_CACHE_INFO_MERCED_L0_I_2,
    PAL_CACHE_INFO_MERCED_L1_U_1,
    PAL_CACHE_INFO_MERCED_L1_U_2,
    PAL_CACHE_INFO_MERCED_L2_U_1,
    PAL_CACHE_INFO_MERCED_L2_U_2,
    PAL_VERSION_VALUE_MERCED,
    PAL_VM_INFO_MERCED_L0_D,
    PAL_VM_INFO_MERCED_L0_I,
    PAL_VM_INFO_MERCED_L1_D,
    PAL_MEM_FOR_TEST,
    PAL_PERF_BUFFER,
    PAL_PERF_MON_INFO,
    PAL_PLATFORM_ADDR,
    PAL_PLATFORM_INTERRUPT_BLOCK,
    PAL_PLATFORM_IO_BLOCK,
    PAL_PMI_ENTRYPOINT,
    PAL_PREFETCH_VIS,
    PAL_PROC_ENTRY,
    PAL_PROC_GET_FEATURES,
    PAL_PROC_SET_FEATURES,
    PAL_PTCE_INFO,
    PAL_PURGE_PAGE_SIZE_MASK,
    PAL_RATIO_16_1,
    PAL_RATIO_4_3,
    PAL_RATIO_8_1,
    PAL_RATIO_16_3,
    PAL_RATIO_2_1,
    PAL_RATIO_4_1,
    PAL_REGISTER_INFO,
    PAL_RSE_INFO,
    PAL_SELF_TEST_STATE_TESTED,
    PAL_TEST_PROC,
    PAL_TR_TEST_IFA,
    PAL_TR_TEST_ITIR,
    PAL_TR_TEST_PTE,
    PAL_TR_VALID_ALL,
    PAL_VERSION,
    PAL_VERSION_VALUE,
    PAL_VIRTUAL_CODE_BASE,
    PAL_VIRTUAL_CODE_ENTRY,
    PAL_VIRTUAL_CODE_ENTRY_PA,
    PAL_VIRTUAL_CODE_PTE,
    PAL_VIRTUAL_ITIR,
    PAL_VIRTUAL_PROC_BASE,
    PAL_VIRTUAL_PROC_ENTRY,
    PAL_VIRTUAL_PROC_PTE,
    PAL_VIRTUAL_PSR,
    PAL_VIRTUAL_RR,
    PAL_VM_INFO,
    PAL_VM_INFO_L0,
    PAL_VM_INFO_L1,
    PAL_VM_PAGE_SIZE,
    PAL_VM_SUMMARY,
    PAL_VM_SUMMARY_INFO_1_MERCED,
    PAL_VM_SUMMARY_INFO_1,
    PAL_VM_SUMMARY_INFO_2,
    PAL_VM_SUMMARY_INFO_2_MERCED,
    PAL_VM_TR_READ,
    add,
    addl,
    adds,
    alloc,
    br_call,
    br_call_indirect,
    br_cond,
    br_indirect,
    br_ret,
    bundle_words,
    cmp_ltu_unc,
    itr_d,
    itr_i,
    ld8,
    ld8_fill_postinc,
    mov_b_gr,
    mov_gr_psr_full,
    mov_m_ar_gr,
    mov_m_gr_cr,
    mov_m_imm_ar,
    mov_rr_write,
    movl_mlx,
    nop_i,
    nop_m,
    pal_break,
    pal_call_program,
    pal_stacked_call_program,
    require_registers,
    rfi_to_gr,
    srlz_i,
    st8,
)


test_pal_halt_light_wakes_on_due_itm = require_registers(
    "pal_halt_light_wakes_on_due_itm", [
        (0x10, 0x00, adds(3, 0xef, 0), nop_i(),
         nop_i()),
        (0x20, 0x00, mov_m_gr_cr(3, IA64_CR_ITV), nop_i(),
         nop_i()),
        (0x30, *movl_mlx(5, 0x200000)),
        # HAL-style arm (WSRV03 halia64 i64itm.s retry_itm_read): re-read
        # ITC after the ITM write and retry while the deadline is no
        # longer in the future -- a missed ITC==ITM equality never fires,
        # and a HALT waiting on it would never wake.
        (0x40, 0x02, mov_m_ar_gr(3, 44), nop_i(),
         nop_i()),
        (0x50, 0x00, nop_m(), add(4, 3, 5), nop_i()),
        (0x60, 0x00, mov_m_gr_cr(4, IA64_CR_ITM), nop_i(),
         nop_i()),
        (0x70, 0x02, mov_m_ar_gr(3, 44), nop_i(),
         nop_i()),
        (0x80, 0x01, nop_m(), cmp_ltu_unc(6, 7, 3, 4), nop_i()),
        (0x90, 0x10, nop_m(), nop_i(),
         br_cond(0x90, 0x40, qp=7)),
        (0xa0, *movl_mlx(28, PAL_HALT_LIGHT)),
        (0xb0, *movl_mlx(19, (1 << 13) | (1 << 14))),
        (0xc0, 0x10, mov_gr_psr_full(19), nop_i(),
         br_call(0, 0xc0, PAL_PROC_ENTRY)),
        (0xd0, 0x10, nop_m(), nop_i(),
         br_cond(0xd0, 0xd0)),
        (PAL_PROC_ENTRY, 0x0a, pal_break(), nop_m(),
         nop_i()),
        (PAL_PROC_ENTRY + 0x10, 0x10, nop_m(), nop_i(),
         br_ret(0)),
        (0x3000, 0x10, nop_m(), adds(31, 0x5a, 0),
         br_cond(0x3000, 0x3010)),
        (0x3010, 0x10, nop_m(), nop_i(),
         br_cond(0x3010, 0x3010)),
    ], {
        "ip": 0x3010,
        "exception": IA64_EXCP_NONE,
        "r8": 0,
        "r31": 0x5a,
    }, entry=0x10)

test_pal_halt_light_stops_at_pal_continuation = require_registers(
    "pal_halt_light_stops_at_pal_continuation", [
        (0x10, *movl_mlx(28, PAL_HALT_LIGHT)),
        (0x20, 0x10, nop_m(), nop_i(),
         br_call(0, 0x20, PAL_PROC_ENTRY)),
        (0x30, 0x00, nop_m(), adds(31, 0x5a, 0),
         nop_i()),
        (0x40, 0x10, nop_m(), nop_i(),
         br_cond(0x40, 0x40)),
        (PAL_PROC_ENTRY, 0x0a, pal_break(), nop_m(),
         nop_i()),
        (PAL_PROC_ENTRY + 0x10, 0x10, nop_m(), nop_i(),
         br_ret(0)),
    ], {
        "ip": PAL_PROC_ENTRY + 0x10,
        "halted": 1,
        "exception": IA64_EXCP_NONE,
        "r8": 0,
        "r31": 0,
    }, entry=0x10)

test_pal_halt_wakes_on_due_itm = require_registers(
    "pal_halt_wakes_on_due_itm", [
        (0x10, 0x00, adds(3, 0xef, 0), nop_i(),
         nop_i()),
        (0x20, 0x00, mov_m_gr_cr(3, IA64_CR_ITV), nop_i(),
         nop_i()),
        (0x30, *movl_mlx(5, 0x200000)),
        # HAL-style arm (WSRV03 halia64 i64itm.s retry_itm_read): re-read
        # ITC after the ITM write and retry while the deadline is no
        # longer in the future -- a missed ITC==ITM equality never fires,
        # and a HALT waiting on it would never wake.
        (0x40, 0x02, mov_m_ar_gr(3, 44), nop_i(),
         nop_i()),
        (0x50, 0x00, nop_m(), add(4, 3, 5), nop_i()),
        (0x60, 0x00, mov_m_gr_cr(4, IA64_CR_ITM), nop_i(),
         nop_i()),
        (0x70, 0x02, mov_m_ar_gr(3, 44), nop_i(),
         nop_i()),
        (0x80, 0x01, nop_m(), cmp_ltu_unc(6, 7, 3, 4), nop_i()),
        (0x90, 0x10, nop_m(), nop_i(),
         br_cond(0x90, 0x40, qp=7)),
        (0xa0, *movl_mlx(28, PAL_HALT)),
        (0xb0, 0x00, nop_m(), addl(29, 1, 0), addl(30, 0, 0)),
        (0xc0, *movl_mlx(19, (1 << 13) | (1 << 14))),
        (0xd0, 0x10, mov_gr_psr_full(19), addl(31, 0, 0),
         br_call(0, 0xd0, PAL_PROC_ENTRY)),
        (0xe0, 0x10, nop_m(), nop_i(),
         br_cond(0xe0, 0xe0)),
        (PAL_PROC_ENTRY, 0x0a, pal_break(), nop_m(),
         nop_i()),
        (PAL_PROC_ENTRY + 0x10, 0x10, nop_m(), nop_i(),
         br_ret(0)),
        (0x3000, 0x10, nop_m(), adds(31, 0x5a, 0),
         br_cond(0x3000, 0x3010)),
        (0x3010, 0x10, nop_m(), nop_i(),
         br_cond(0x3010, 0x3010)),
    ], {
        "ip": 0x3010,
        "exception": IA64_EXCP_NONE,
        "r8": 0,
        "r9": 0,
        "r31": 0x5a,
    }, entry=0x10)

# PAL_HALT and PAL_HALT_LIGHT read the virtual clock.  Under -icount that is
# only allowed in the last instruction of a TB, and the break bundle of the
# PAL stub is followed by its br.ret: QEMU exited with "Bad icount read".
def _icount_case(name, case):
    def run(qemu):
        _enc.run_program(qemu, case.bundles, entry=0x10,
                         expected=case.expected, name=name,
                         icount="shift=0")
    return IA64Case(
        name=name, runner=run, bundles=case.bundles,
        expected=dict(case.expected),
        metadata=CaseMetadata(
            required_features=frozenset({"alat:full", "icount"})),
    )


test_pal_halt_light_stops_at_pal_continuation_icount = _icount_case(
    "pal_halt_light_stops_at_pal_continuation_icount",
    test_pal_halt_light_stops_at_pal_continuation)

test_pal_halt_wakes_on_due_itm_icount = _icount_case(
    "pal_halt_wakes_on_due_itm_icount", test_pal_halt_wakes_on_due_itm)

_pal_cache_flush_patch_low, _pal_cache_flush_patch_high = bundle_words(
    0x11, nop_m(), adds(21, 2, 0), br_cond(0x120, 0x180)
)

test_pal_cache_flush_invalidates_translated_target = require_registers(
    "pal_cache_flush_invalidates_translated_target", [
        (0x10, 0x10, nop_m(), nop_i(),
         br_cond(0x10, 0x120)),
        (0x40, *movl_mlx(16, 0x120)),
        (0x50, *movl_mlx(17, _pal_cache_flush_patch_low)),
        (0x60, *movl_mlx(18, _pal_cache_flush_patch_high)),
        (0x70, 0x00, st8(16, 17), adds(19, 8, 16),
         nop_i()),
        (0x80, 0x00, st8(19, 18), addl(28, PAL_CACHE_FLUSH, 0),
         nop_i()),
        (0x90, 0x00, nop_m(), addl(29, 4, 0),
         addl(30, 3, 0)),
        (0xa0, 0x10, nop_m(), nop_i(),
         br_call(0, 0xa0, PAL_PROC_ENTRY)),
        (0xb0, 0x10, nop_m(), nop_i(),
         br_cond(0xb0, 0x120)),
        (0x120, 0x11, nop_m(), adds(20, 1, 20),
         br_cond(0x120, 0x40)),
        (0x180, 0x10, nop_m(), nop_i(),
         br_cond(0x180, 0x180)),
        (PAL_PROC_ENTRY, 0x0a, pal_break(), nop_m(), nop_i()),
        (PAL_PROC_ENTRY + 0x10, 0x10, nop_m(), nop_i(), br_ret(0)),
    ], {
        "ip": 0x180,
        "exception": IA64_EXCP_NONE,
        "r8": 0,
        "r20": 1,
        "r21": 2,
    }, entry=0x10)



# ── PAL tests ──

test_pal_version = require_registers("pal_version",
    pal_call_program(PAL_VERSION), {"ip": 0x30, "r28": PAL_VERSION, "r8": 0,
    "r9": PAL_VERSION_VALUE, "r10": PAL_VERSION_VALUE}, entry=0x10)

test_pal_version_reserved_arg = require_registers("pal_version_reserved_arg",
    pal_call_program(PAL_VERSION, [(29, 1), (30, 0), (31, 0)]),
    {"ip": 0x60, "r28": PAL_VERSION,
     "r8": (-2 & 0xffffffffffffffff), "r9": 0, "r10": 0, "r11": 0},
    entry=0x10)

test_pal_rse_info = require_registers("pal_rse_info",
    pal_call_program(PAL_RSE_INFO),
    # hints{1:0} = 0: neither modelled generation implements eager RSE modes.
    {"ip": 0x30, "r28": PAL_RSE_INFO, "r8": 0, "r9": 96, "r10": 0},
    entry=0x10)

test_pal_rse_info_reserved_arg = require_registers("pal_rse_info_reserved_arg",
    pal_call_program(PAL_RSE_INFO, [(29, 0), (30, 1), (31, 0)]),
    {"ip": 0x60, "r28": PAL_RSE_INFO,
     "r8": (-2 & 0xffffffffffffffff), "r9": 0, "r10": 0, "r11": 0},
    entry=0x10)

test_pal_vm_summary = require_registers("pal_vm_summary",
    pal_call_program(PAL_VM_SUMMARY),
    {"ip": 0x30, "r28": PAL_VM_SUMMARY, "r8": 0,
    "r9": PAL_VM_SUMMARY_INFO_1, "r10": PAL_VM_SUMMARY_INFO_2}, entry=0x10)

test_pal_vm_summary_reserved_arg = require_registers(
    "pal_vm_summary_reserved_arg",
    pal_call_program(PAL_VM_SUMMARY, [(29, 0), (30, 0), (31, 1)]),
    {"ip": 0x60, "r28": PAL_VM_SUMMARY,
     "r8": (-2 & 0xffffffffffffffff), "r9": 0, "r10": 0, "r11": 0},
    entry=0x10)

test_pal_cache_summary = require_registers("pal_cache_summary",
    pal_call_program(PAL_CACHE_SUMMARY),
    {"ip": 0x30, "r28": PAL_CACHE_SUMMARY, "r8": 0,
    "r9": 3, "r10": 5}, entry=0x10)

test_pal_cache_summary_madison = require_registers(
    "pal_cache_summary_madison",
    pal_call_program(PAL_CACHE_SUMMARY),
    {"ip": 0x30, "r28": PAL_CACHE_SUMMARY, "r8": 0,
     "r9": 3, "r10": 4}, entry=0x10, cpu="madison")

test_pal_cache_summary_reserved_arg = require_registers(
    "pal_cache_summary_reserved_arg",
    pal_call_program(PAL_CACHE_SUMMARY, [(29, 1), (30, 0), (31, 0)]),
    {"ip": 0x60, "r28": PAL_CACHE_SUMMARY,
     "r8": (-2 & 0xffffffffffffffff), "r9": 0, "r10": 0, "r11": 0},
    entry=0x10)

test_pal_cache_info = require_registers("pal_cache_info",
    pal_call_program(PAL_CACHE_INFO, [(29, 0), (30, 1), (31, 0)]),
    {"ip": 0x60, "r28": PAL_CACHE_INFO, "r8": 0,
     "r9": PAL_CACHE_INFO_L0_I_1, "r10": PAL_CACHE_INFO_L0_2,
     "r11": 0}, entry=0x10)

test_pal_cache_info_l0_data = require_registers("pal_cache_info_l0_data",
    pal_call_program(PAL_CACHE_INFO, [(29, 0), (30, 2), (31, 0)]),
    {"ip": 0x60, "r28": PAL_CACHE_INFO, "r8": 0,
     "r9": PAL_CACHE_INFO_L0_D_1, "r10": PAL_CACHE_INFO_L0_2,
     "r11": 0}, entry=0x10)

test_pal_cache_info_l1_data = require_registers(
    "pal_cache_info_l1_data",
    pal_call_program(PAL_CACHE_INFO, [(29, 1), (30, 2), (31, 0)]),
    {"ip": 0x60, "r28": PAL_CACHE_INFO, "r8": 0,
     "r9": PAL_CACHE_INFO_L1_D_1, "r10": PAL_CACHE_INFO_L1_D_2,
     "r11": 0}, entry=0x10)

test_pal_cache_info_l1_instruction = require_registers(
    "pal_cache_info_l1_instruction",
    pal_call_program(PAL_CACHE_INFO, [(29, 1), (30, 1), (31, 0)]),
    {"ip": 0x60, "r28": PAL_CACHE_INFO, "r8": 0,
     "r9": PAL_CACHE_INFO_L1_I_1, "r10": PAL_CACHE_INFO_L1_I_2,
     "r11": 0}, entry=0x10)

test_pal_cache_info_l2_unified = require_registers(
    "pal_cache_info_l2_unified",
    pal_call_program(PAL_CACHE_INFO, [(29, 2), (30, 2), (31, 0)]),
    {"ip": 0x60, "r28": PAL_CACHE_INFO, "r8": 0,
     "r9": PAL_CACHE_INFO_L2_U_1, "r10": PAL_CACHE_INFO_L2_U_2,
     "r11": 0}, entry=0x10)

test_pal_cache_info_invalid = require_registers("pal_cache_info_invalid",
    pal_call_program(PAL_CACHE_INFO, [(29, 3), (30, 1), (31, 0)]),
    {"ip": 0x60, "r28": PAL_CACHE_INFO,
     "r8": (-2 & 0xffffffffffffffff), "r9": 0, "r10": 0, "r11": 0},
    entry=0x10)

test_pal_cache_info_l2_unified_bad_type = require_registers(
    "pal_cache_info_l2_unified_bad_type",
    pal_call_program(PAL_CACHE_INFO, [(29, 2), (30, 1), (31, 0)]),
    {"ip": 0x60, "r28": PAL_CACHE_INFO,
     "r8": (-2 & 0xffffffffffffffff), "r9": 0, "r10": 0, "r11": 0},
    entry=0x10)

test_pal_freq_base = require_registers("pal_freq_base",
    pal_call_program(PAL_FREQ_BASE),
    {"ip": 0x30, "r28": PAL_FREQ_BASE, "r8": 0,
    "r9": 100000000, "r10": 0, "r11": 0}, entry=0x10)

test_pal_freq_base_reserved_arg = require_registers(
    "pal_freq_base_reserved_arg",
    pal_call_program(PAL_FREQ_BASE, [(29, 1), (30, 0), (31, 0)]),
    {"ip": 0x60, "r28": PAL_FREQ_BASE,
     "r8": (-2 & 0xffffffffffffffff), "r9": 0, "r10": 0, "r11": 0},
    entry=0x10)

test_pal_freq_ratios = require_registers("pal_freq_ratios",
    pal_call_program(PAL_FREQ_RATIOS),
    {"ip": 0x30, "r28": PAL_FREQ_RATIOS, "r8": 0,
    "r9": PAL_RATIO_16_1, "r10": PAL_RATIO_16_3,
    "r11": PAL_RATIO_16_1}, entry=0x10)

test_pal_freq_ratios_madison = require_registers(
    "pal_freq_ratios_madison", pal_call_program(PAL_FREQ_RATIOS),
    {"ip": 0x30, "r28": PAL_FREQ_RATIOS, "r8": 0,
     "r9": PAL_RATIO_16_1, "r10": PAL_RATIO_4_1,
     "r11": PAL_RATIO_16_1}, entry=0x10, cpu="madison")

# Regression: a PAL procedure returns its status in GR8; on hardware that
# register write clears the NaT bit.  r8-r11 are PAL *output* registers, so a
# caller may leave one NaT before the call -- the result must not come back NaT.
# (XP's SETUPLDR wedged on exactly this: a `st8 [r30]=r8` of the still-NaT PAL
# status raised NaT Consumption.)  Here r8 is forced NaT with an ld8.fill under
# ar.unat, then PAL_VERSION is called; r8 must be non-NaT afterwards.
test_pal_call_clears_return_reg_nat = require_registers(
    "pal_call_clears_return_reg_nat", [
        (0x10, 0x00, mov_m_imm_ar(36, 1), addl(6, 0x200, 0), nop_i()),
        (0x20, 0x08, ld8_fill_postinc(8, 6, 0), nop_i(), nop_i()),
        (0x30, 0x00, nop_m(), addl(28, PAL_VERSION, 0), nop_i()),
        (0x40, 0x10, nop_m(), nop_i(), br_call(0, 0x40, PAL_PROC_ENTRY)),
        (0x50, 0x10, nop_m(), nop_i(), br_cond(0x50, 0x50)),
        (PAL_PROC_ENTRY, 0x0a, pal_break(), nop_m(), nop_i()),
        (PAL_PROC_ENTRY + 0x10, 0x10, nop_m(), nop_i(), br_ret(0)),
        (0x200, 0x00, 0, 0, 0),
    ], {
        "ip": 0x50,
        "r8": 0,
        "r8_nat": 0,
        "r9": PAL_VERSION_VALUE,
        "exception": IA64_EXCP_NONE,
    }, entry=0x10)

# --- Merced (original Itanium) model-differentiated PAL responses ----------
# 800 MHz core / 133.33 MHz bus (249634-002 datasheet); the ITC counts
# processor clocks, so its ratio is the processor ratio (245473-002).
test_pal_freq_ratios_merced = require_registers(
    "pal_freq_ratios_merced", pal_call_program(PAL_FREQ_RATIOS),
    {"ip": 0x30, "r28": PAL_FREQ_RATIOS, "r8": 0,
     "r9": PAL_RATIO_8_1, "r10": PAL_RATIO_4_3,
     "r11": PAL_RATIO_8_1}, entry=0x10, cpu="merced")

# PAL_FREQ_BASE base clock is the same 100 MHz for merced.
test_pal_freq_base_merced = require_registers(
    "pal_freq_base_merced", pal_call_program(PAL_FREQ_BASE),
    {"ip": 0x30, "r28": PAL_FREQ_BASE, "r8": 0,
     "r9": 100000000, "r10": 0, "r11": 0}, entry=0x10, cpu="merced")

# PAL_VM_SUMMARY reports the asymmetric 8 ITR / 48 DTR file.
test_pal_vm_summary_merced = require_registers(
    "pal_vm_summary_merced", pal_call_program(PAL_VM_SUMMARY),
    {"ip": 0x30, "r28": PAL_VM_SUMMARY, "r8": 0,
     "r9": PAL_VM_SUMMARY_INFO_1_MERCED,
     "r10": PAL_VM_SUMMARY_INFO_2_MERCED},
    entry=0x10, cpu="merced")

# The ITR and DTR files are bounded independently, so PAL_VM_TR_READ accepts
# index 8 as a DTR (48 implemented) and rejects it as an ITR (8 implemented).
# This pins the asymmetry behaviourally, independent of how vm_info_1 packs
# max_itr_entry and max_dtr_entry.
test_pal_vm_tr_read_merced_itr_bound = require_registers(
    "pal_vm_tr_read_merced_itr_bound",
    pal_stacked_call_program(PAL_VM_TR_READ, [IA64_MERCED_ITR_COUNT, 0,
                                              0x2000]),
    {"ip": 0x80, "r28": PAL_VM_TR_READ,
     "r8": (-2 & 0xffffffffffffffff), "r9": 0, "r10": 0, "r11": 0},
    entry=0x10, cpu="merced")

test_pal_vm_tr_read_merced_dtr_bound = require_registers(
    "pal_vm_tr_read_merced_dtr_bound",
    pal_stacked_call_program(PAL_VM_TR_READ, [IA64_MERCED_ITR_COUNT, 1,
                                              0x2000]),
    {"ip": 0x80, "r28": PAL_VM_TR_READ, "r8": 0,
     "r9": 0, "r10": 0, "r11": 0},
    entry=0x10, cpu="merced")

test_pal_vm_tr_read_merced_dtr_limit = require_registers(
    "pal_vm_tr_read_merced_dtr_limit",
    pal_stacked_call_program(PAL_VM_TR_READ, [IA64_MERCED_DTR_COUNT, 1,
                                              0x2000]),
    {"ip": 0x80, "r28": PAL_VM_TR_READ,
     "r8": (-2 & 0xffffffffffffffff), "r9": 0, "r10": 0, "r11": 0},
    entry=0x10, cpu="merced")

# Merced cache hierarchy: 16 KB 4-way 32 B L1I/L1D, 96 KB 6-way 64 B unified
# write-back L2, 4 MB 4-way 64 B unified L3 (245473-002 sec 4.1-4.4,
# 248701-002 sec 2.5.4).  The unified levels are reported on the data type
# only; the instruction type is an invalid argument there.
test_pal_cache_info_merced_l0_i = require_registers(
    "pal_cache_info_merced_l0_i",
    pal_call_program(PAL_CACHE_INFO, [(29, 0), (30, 1), (31, 0)]),
    {"ip": 0x60, "r28": PAL_CACHE_INFO, "r8": 0,
     "r9": PAL_CACHE_INFO_MERCED_L0_I_1,
     "r10": PAL_CACHE_INFO_MERCED_L0_I_2}, entry=0x10, cpu="merced")

test_pal_cache_info_merced_l0_d = require_registers(
    "pal_cache_info_merced_l0_d",
    pal_call_program(PAL_CACHE_INFO, [(29, 0), (30, 2), (31, 0)]),
    {"ip": 0x60, "r28": PAL_CACHE_INFO, "r8": 0,
     "r9": PAL_CACHE_INFO_MERCED_L0_D_1,
     "r10": PAL_CACHE_INFO_MERCED_L0_I_2}, entry=0x10, cpu="merced")

test_pal_cache_info_merced_l1_unified = require_registers(
    "pal_cache_info_merced_l1_unified",
    pal_call_program(PAL_CACHE_INFO, [(29, 1), (30, 2), (31, 0)]),
    {"ip": 0x60, "r28": PAL_CACHE_INFO, "r8": 0,
     "r9": PAL_CACHE_INFO_MERCED_L1_U_1,
     "r10": PAL_CACHE_INFO_MERCED_L1_U_2}, entry=0x10, cpu="merced")

test_pal_cache_info_merced_l1_instruction_invalid = require_registers(
    "pal_cache_info_merced_l1_instruction_invalid",
    pal_call_program(PAL_CACHE_INFO, [(29, 1), (30, 1), (31, 0)]),
    {"ip": 0x60, "r28": PAL_CACHE_INFO,
     "r8": (-2 & 0xffffffffffffffff), "r9": 0, "r10": 0, "r11": 0},
    entry=0x10, cpu="merced")

test_pal_cache_info_merced_l2_unified = require_registers(
    "pal_cache_info_merced_l2_unified",
    pal_call_program(PAL_CACHE_INFO, [(29, 2), (30, 2), (31, 0)]),
    {"ip": 0x60, "r28": PAL_CACHE_INFO, "r8": 0,
     "r9": PAL_CACHE_INFO_MERCED_L2_U_1,
     "r10": PAL_CACHE_INFO_MERCED_L2_U_2}, entry=0x10, cpu="merced")

# Merced TCs: 64-entry ITLB holding the instruction TRs, 32-entry DTLB1 with
# none, 96-entry DTLB2 holding the data TRs, and no second instruction level
# (248701-002 sec 2.5.6).
test_pal_vm_info_merced_l0_instruction = require_registers(
    "pal_vm_info_merced_l0_instruction",
    pal_call_program(PAL_VM_INFO, [(29, 0), (30, 1), (31, 0)]),
    {"ip": 0x60, "r28": PAL_VM_INFO, "r8": 0,
     "r9": PAL_VM_INFO_MERCED_L0_I,
     "r10": PAL_MERCED_INSERTABLE_PAGE_SIZE_MASK}, entry=0x10, cpu="merced")

test_pal_vm_info_merced_l0_data = require_registers(
    "pal_vm_info_merced_l0_data",
    pal_call_program(PAL_VM_INFO, [(29, 0), (30, 2), (31, 0)]),
    {"ip": 0x60, "r28": PAL_VM_INFO, "r8": 0,
     "r9": PAL_VM_INFO_MERCED_L0_D,
     "r10": PAL_MERCED_INSERTABLE_PAGE_SIZE_MASK}, entry=0x10, cpu="merced")

test_pal_vm_info_merced_l1_data = require_registers(
    "pal_vm_info_merced_l1_data",
    pal_call_program(PAL_VM_INFO, [(29, 1), (30, 2), (31, 0)]),
    {"ip": 0x60, "r28": PAL_VM_INFO, "r8": 0,
     "r9": PAL_VM_INFO_MERCED_L1_D,
     "r10": PAL_MERCED_INSERTABLE_PAGE_SIZE_MASK}, entry=0x10, cpu="merced")

test_pal_vm_info_merced_l1_instruction_invalid = require_registers(
    "pal_vm_info_merced_l1_instruction_invalid",
    pal_call_program(PAL_VM_INFO, [(29, 1), (30, 1), (31, 0)]),
    {"ip": 0x60, "r28": PAL_VM_INFO,
     "r8": (-2 & 0xffffffffffffffff), "r9": 0, "r10": 0, "r11": 0},
    entry=0x10, cpu="merced")

# Merced has neither 1 GB nor 4 GB pages for insertion, and takes 4 GB only
# for purges (245473-002 sec 4.7).
test_pal_vm_page_size_merced = require_registers(
    "pal_vm_page_size_merced", pal_call_program(PAL_VM_PAGE_SIZE),
    {"ip": 0x30, "r28": PAL_VM_PAGE_SIZE, "r8": 0,
     "r9": PAL_MERCED_INSERTABLE_PAGE_SIZE_MASK,
     "r10": PAL_MERCED_PURGE_PAGE_SIZE_MASK}, entry=0x10, cpu="merced")

# PAL 8.8.30 is the C2 stepping's firmware version (249720-009).
test_pal_version_merced = require_registers(
    "pal_version_merced", pal_call_program(PAL_VERSION),
    {"ip": 0x30, "r28": PAL_VERSION, "r8": 0,
     "r9": PAL_VERSION_VALUE_MERCED, "r10": PAL_VERSION_VALUE_MERCED},
    entry=0x10, cpu="merced")

# Procedures that post-date Merced return NOT_IMPLEMENTED on the merced model.
test_pal_prefetch_vis_merced_unimplemented = require_registers(
    "pal_prefetch_vis_merced_unimplemented",
    pal_call_program(PAL_PREFETCH_VIS),
    {"ip": 0x30, "r28": PAL_PREFETCH_VIS,
     "r8": (-1 & 0xffffffffffffffff), "r9": 0, "r10": 0, "r11": 0},
    entry=0x10, cpu="merced")

test_pal_cache_shared_info_merced_unimplemented = require_registers(
    "pal_cache_shared_info_merced_unimplemented",
    pal_call_program(PAL_CACHE_SHARED_INFO, [(29, 0), (30, 1), (31, 0)]),
    {"ip": 0x60, "r28": PAL_CACHE_SHARED_INFO,
     "r8": (-1 & 0xffffffffffffffff), "r9": 0, "r10": 0, "r11": 0},
    entry=0x10, cpu="merced")

test_pal_brand_info_merced_unimplemented = require_registers(
    "pal_brand_info_merced_unimplemented",
    pal_stacked_call_program(PAL_BRAND_INFO, [18, 0, 0]),
    {"ip": 0x80, "r28": PAL_BRAND_INFO,
     "r8": (-1 & 0xffffffffffffffff), "r9": 0, "r10": 0, "r11": 0},
    entry=0x10, cpu="merced")

# logical_to_physical is already montecito-only; confirm merced too is
# NOT_IMPLEMENTED (mirrors pal_logical_to_physical_madison_unimplemented).
test_pal_logical_to_physical_merced_unimplemented = require_registers(
    "pal_logical_to_physical_merced_unimplemented",
    pal_call_program(PAL_LOGICAL_TO_PHYSICAL,
                     [(29, 0xffffffffffffffff), (30, 0), (31, 0)]),
    {"ip": 0x60, "r28": PAL_LOGICAL_TO_PHYSICAL,
     "r8": (-1 & 0xffffffffffffffff), "r9": 0, "r10": 0, "r11": 0},
    entry=0x10, cpu="merced")

test_pal_freq_ratios_reserved_arg = require_registers(
    "pal_freq_ratios_reserved_arg",
    pal_call_program(PAL_FREQ_RATIOS, [(29, 0), (30, 1), (31, 0)]),
    {"ip": 0x60, "r28": PAL_FREQ_RATIOS,
     "r8": (-2 & 0xffffffffffffffff), "r9": 0, "r10": 0, "r11": 0},
    entry=0x10)

test_pal_vm_page_size = require_registers("pal_vm_page_size",
    pal_call_program(PAL_VM_PAGE_SIZE),
    {"ip": 0x30, "r28": PAL_VM_PAGE_SIZE, "r8": 0,
    "r9": PAL_INSERTABLE_PAGE_SIZE_MASK, "r10": PAL_PURGE_PAGE_SIZE_MASK},
    entry=0x10)

test_pal_vm_page_size_reserved_arg = require_registers(
    "pal_vm_page_size_reserved_arg",
    pal_call_program(PAL_VM_PAGE_SIZE, [(29, 0), (30, 0), (31, 1)]),
    {"ip": 0x60, "r28": PAL_VM_PAGE_SIZE,
     "r8": (-2 & 0xffffffffffffffff), "r9": 0, "r10": 0, "r11": 0},
    entry=0x10)

test_pal_ptce_info = require_registers("pal_ptce_info",
    pal_call_program(PAL_PTCE_INFO),
    {"ip": 0x30, "r28": PAL_PTCE_INFO, "r8": 0,
     "r9": 0, "r10": (1 << 32) | 1,
     "r11": 0}, entry=0x10)

test_pal_ptce_info_reserved_arg = require_registers(
    "pal_ptce_info_reserved_arg",
    pal_call_program(PAL_PTCE_INFO, [(29, 1), (30, 0), (31, 0)]),
    {"ip": 0x60, "r28": PAL_PTCE_INFO,
     "r8": (-2 & 0xffffffffffffffff), "r9": 0, "r10": 0, "r11": 0},
    entry=0x10)

test_pal_vm_info = require_registers("pal_vm_info",
    pal_call_program(PAL_VM_INFO, [(29, 0), (30, 1), (31, 0)]),
    {"ip": 0x60, "r28": PAL_VM_INFO, "r8": 0,
     "r9": PAL_VM_INFO_L0, "r10": 1 << 12,
     "r11": 0}, entry=0x10)

test_pal_vm_info_l0_data = require_registers("pal_vm_info_l0_data",
    pal_call_program(PAL_VM_INFO, [(29, 0), (30, 2), (31, 0)]),
    {"ip": 0x60, "r28": PAL_VM_INFO, "r8": 0,
     "r9": PAL_VM_INFO_L0, "r10": 1 << 12,
     "r11": 0}, entry=0x10)

test_pal_vm_info_l1_data = require_registers("pal_vm_info_l1_data",
    pal_call_program(PAL_VM_INFO, [(29, 1), (30, 2), (31, 0)]),
    {"ip": 0x60, "r28": PAL_VM_INFO, "r8": 0,
     "r9": PAL_VM_INFO_L1, "r10": PAL_INSERTABLE_PAGE_SIZE_MASK,
     "r11": 0}, entry=0x10)

test_pal_vm_info_l1_instruction = require_registers(
    "pal_vm_info_l1_instruction",
    pal_call_program(PAL_VM_INFO, [(29, 1), (30, 1), (31, 0)]),
    {"ip": 0x60, "r28": PAL_VM_INFO, "r8": 0,
     "r9": PAL_VM_INFO_L1, "r10": PAL_INSERTABLE_PAGE_SIZE_MASK,
     "r11": 0}, entry=0x10)

test_pal_vm_info_l2_invalid = require_registers("pal_vm_info_l2_invalid",
    pal_call_program(PAL_VM_INFO, [(29, 2), (30, 2), (31, 0)]),
    {"ip": 0x60, "r28": PAL_VM_INFO,
     "r8": -2 & 0xffffffffffffffff, "r9": 0, "r10": 0,
     "r11": 0}, entry=0x10)

test_pal_vm_info_invalid = require_registers("pal_vm_info_invalid",
    pal_call_program(PAL_VM_INFO, [(29, 0), (30, 4), (31, 0)]),
    {"ip": 0x60, "r28": PAL_VM_INFO,
     "r8": (-2 & 0xffffffffffffffff), "r9": 0, "r10": 0, "r11": 0},
    entry=0x10)

test_pal_vm_tr_read_dtr = require_registers("pal_vm_tr_read_dtr", [
    (0x10, *movl_mlx(18, PAL_TR_TEST_PTE)),
    (0x20, 0x00, nop_m(), addl(19, PAL_TR_TEST_IFA & ~0xfff, 0),
     nop_i()),
    (0x30, 0x00, nop_m(), addl(7, PAL_TR_TEST_ITIR, 0), addl(5, 5, 0)),
    (0x40, 0x00, mov_m_gr_cr(19, 20), nop_i(), nop_i()),
    (0x50, 0x00, mov_m_gr_cr(7, 21), nop_i(), nop_i()),
    (0x60, 0x00, itr_d(5, 18), nop_i(), nop_i()),
    (0x70, 0x00, nop_m(), alloc(2, 4, 0, 0, 0), nop_i()),
    (0x80, *movl_mlx(28, PAL_VM_TR_READ)),
    (0x90, *movl_mlx(32, PAL_VM_TR_READ)),
    (0xa0, 0x00, nop_m(), addl(33, 5, 0), addl(34, 1, 0)),
    (0xb0, 0x00, nop_m(), addl(35, 0x2000, 0), nop_i()),
    (0xc0, 0x10, nop_m(), nop_i(), br_call(0, 0xc0, PAL_PROC_ENTRY)),
    (0xd0, 0x00, nop_m(), addl(2, 0x2000, 0), nop_i()),
    (0xe0, 0x00, ld8(20, 2), adds(2, 8, 2), nop_i()),
    (0xf0, 0x00, ld8(21, 2), adds(2, 8, 2), nop_i()),
    (0x100, 0x00, ld8(22, 2), adds(2, 8, 2), nop_i()),
    (0x110, 0x00, ld8(23, 2), nop_i(), nop_i()),
    (0x120, 0x10, nop_m(), nop_i(), br_cond(0x120, 0x120)),
    (PAL_PROC_ENTRY, 0x0a, pal_break(), nop_m(), nop_i()),
    (PAL_PROC_ENTRY + 0x10, 0x10, nop_m(), nop_i(), br_ret(0)),
], {"ip": 0x120, "r28": PAL_VM_TR_READ, "r8": 0,
    "r9": PAL_TR_VALID_ALL, "r10": 0, "r11": 0,
    "r20": PAL_TR_TEST_PTE, "r21": PAL_TR_TEST_ITIR,
    "r22": PAL_TR_TEST_IFA, "r23": PAL_TR_TEST_ITIR}, entry=0x10)

test_pal_vm_tr_read_max_dtr = require_registers("pal_vm_tr_read_max_dtr", [
        (0x10, *movl_mlx(18, PAL_TR_TEST_PTE)),
        (0x20, 0x00, nop_m(), addl(19, PAL_TR_TEST_IFA & ~0xfff, 0),
         nop_i()),
        (0x30, 0x00, mov_m_gr_cr(19, 20),
         addl(5, IA64_TR_COUNT - 1, 0), addl(7, PAL_TR_TEST_ITIR, 0)),
        (0x40, 0x00, mov_m_gr_cr(7, 21), nop_i(), nop_i()),
        (0x50, 0x00, itr_d(5, 18), nop_i(), nop_i()),
        (0x60, 0x00, nop_m(), alloc(2, 4, 0, 0, 0), nop_i()),
        (0x70, *movl_mlx(28, PAL_VM_TR_READ)),
        (0x80, *movl_mlx(32, PAL_VM_TR_READ)),
        (0x90, 0x00, nop_m(),
         addl(33, IA64_TR_COUNT - 1, 0), addl(34, 1, 0)),
        (0xa0, 0x00, nop_m(), addl(35, 0x2000, 0), nop_i()),
        (0xb0, 0x10, nop_m(), nop_i(), br_call(0, 0xb0, PAL_PROC_ENTRY)),
        (0xc0, 0x00, nop_m(), addl(2, 0x2000, 0), nop_i()),
        (0xd0, 0x00, ld8(20, 2), adds(2, 8, 2), nop_i()),
        (0xe0, 0x00, ld8(21, 2), adds(2, 8, 2), nop_i()),
        (0xf0, 0x00, ld8(22, 2), adds(2, 8, 2), nop_i()),
        (0x100, 0x00, ld8(23, 2), nop_i(), nop_i()),
        (0x110, 0x10, nop_m(), nop_i(), br_cond(0x110, 0x110)),
        (PAL_PROC_ENTRY, 0x0a, pal_break(), nop_m(), nop_i()),
        (PAL_PROC_ENTRY + 0x10, 0x10, nop_m(), nop_i(), br_ret(0)),
    ], {"ip": 0x110, "r28": PAL_VM_TR_READ, "r8": 0,
        "r9": PAL_TR_VALID_ALL, "r10": 0, "r11": 0,
        "r20": PAL_TR_TEST_PTE, "r21": PAL_TR_TEST_ITIR,
        "r22": PAL_TR_TEST_IFA, "r23": PAL_TR_TEST_ITIR}, entry=0x10)

test_pal_vm_tr_read_empty = require_registers("pal_vm_tr_read_empty",
    pal_stacked_call_program(PAL_VM_TR_READ, [4, 1, 0x2000]),
    {"ip": 0x80, "r28": PAL_VM_TR_READ, "r8": 0,
     "r9": 0, "r10": 0, "r11": 0}, entry=0x10)

test_pal_vm_tr_read_rejects_first_non_tr = require_registers(
    "pal_vm_tr_read_rejects_first_non_tr",
    pal_stacked_call_program(PAL_VM_TR_READ, [IA64_TR_COUNT, 1, 0x2000]),
    {"ip": 0x80, "r28": PAL_VM_TR_READ,
     "r8": (-2 & 0xffffffffffffffff), "r9": 0, "r10": 0, "r11": 0},
    entry=0x10)

test_pal_vm_tr_read_invalid = require_registers("pal_vm_tr_read_invalid",
    pal_stacked_call_program(PAL_VM_TR_READ, [0, 2, 0x2000]),
    {"ip": 0x80, "r28": PAL_VM_TR_READ,
     "r8": (-2 & 0xffffffffffffffff), "r9": 0, "r10": 0, "r11": 0},
    entry=0x10)

test_pal_vm_tr_read_misaligned_buffer = require_registers(
    "pal_vm_tr_read_misaligned_buffer",
    pal_stacked_call_program(PAL_VM_TR_READ, [0, 1, 0x2004]),
    {"ip": 0x80, "r28": PAL_VM_TR_READ,
     "r8": (-2 & 0xffffffffffffffff), "r9": 0, "r10": 0, "r11": 0},
    entry=0x10)

test_pal_proc_entry_virtual_itr = require_registers(
    "pal_proc_entry_virtual_itr", [
        (0x10, *movl_mlx(18, PAL_VIRTUAL_CODE_PTE)),
        (0x20, *movl_mlx(22, PAL_VIRTUAL_PROC_PTE)),
        (0x30, *movl_mlx(20, PAL_VIRTUAL_CODE_BASE)),
        (0x40, *movl_mlx(21, PAL_VIRTUAL_PROC_BASE)),
        (0x50, *movl_mlx(25, PAL_VIRTUAL_RR)),
        (0x60, 0x00, mov_rr_write(25, 21), nop_i(), nop_i()),
        (0x70, 0x00, srlz_i(), nop_i(), nop_i()),
        (0x80, 0x00, mov_m_gr_cr(20, 20), adds(7, PAL_VIRTUAL_ITIR, 0),
         nop_i()),
        (0x90, 0x00, mov_m_gr_cr(7, 21), adds(5, 5, 0), nop_i()),
        (0xa0, 0x00, itr_i(5, 18), nop_i(), nop_i()),
        (0xb0, 0x00, mov_m_gr_cr(21, 20), adds(6, 6, 0), nop_i()),
        (0xc0, 0x00, itr_i(6, 22), nop_i(), nop_i()),
        (0xd0, *movl_mlx(23, PAL_VIRTUAL_CODE_ENTRY)),
        (0xe0, *movl_mlx(24, PAL_VIRTUAL_PROC_ENTRY)),
        (0xf0, *movl_mlx(19, PAL_VIRTUAL_PSR)),
        (0x100, 0x00, nop_m(), mov_b_gr(7, 23), nop_i()),
        *rfi_to_gr(0x110, 19, 23),
        (PAL_VIRTUAL_CODE_ENTRY_PA, *movl_mlx(28, PAL_VERSION)),
        (PAL_VIRTUAL_CODE_ENTRY_PA + 0x10, 0x00, nop_m(), mov_b_gr(7, 24),
         nop_i()),
        (PAL_VIRTUAL_CODE_ENTRY_PA + 0x20, 0x10, nop_m(), nop_i(),
         br_call_indirect(0, 7)),
        (PAL_VIRTUAL_CODE_ENTRY_PA + 0x30, 0x10, nop_m(), nop_i(),
         br_cond(PAL_VIRTUAL_CODE_ENTRY + 0x30,
                 PAL_VIRTUAL_CODE_ENTRY + 0x30)),
        (PAL_PROC_ENTRY, 0x0a, pal_break(), nop_m(), nop_i()),
        (PAL_PROC_ENTRY + 0x10, 0x10, nop_m(), nop_i(), br_ret(0)),
    ], {
        "ip": PAL_VIRTUAL_CODE_ENTRY + 0x30,
        "r28": PAL_VERSION,
        "r8": 0,
        "r9": PAL_VERSION_VALUE,
        "r10": PAL_VERSION_VALUE,
        "r11": 0,
    }, entry=0x10)

test_pal_prefetch_vis = require_registers("pal_prefetch_vis",
    pal_call_program(PAL_PREFETCH_VIS),
    {"ip": 0x30, "r28": PAL_PREFETCH_VIS, "r8": 0,
     "r9": ((1 << 0) | (1 << 1)), "r10": 0}, entry=0x10)

test_pal_prefetch_vis_reserved_arg = require_registers(
    "pal_prefetch_vis_reserved_arg",
    pal_call_program(PAL_PREFETCH_VIS, [(29, 0), (30, 1), (31, 0)]),
    {"ip": 0x60, "r28": PAL_PREFETCH_VIS,
     "r8": (-2 & 0xffffffffffffffff), "r9": 0, "r10": 0, "r11": 0},
    entry=0x10)

test_pal_cache_flush = require_registers("pal_cache_flush",
    pal_call_program(PAL_CACHE_FLUSH, [(29, 3), (30, 1), (31, 0)]),
    {"ip": 0x60, "r28": PAL_CACHE_FLUSH, "r8": 0, "r9": 0, "r10": 0},
    entry=0x10)

test_pal_cache_flush_coherent_icache = require_registers(
    "pal_cache_flush_coherent_icache",
    pal_call_program(PAL_CACHE_FLUSH, [(29, 4), (30, 3), (31, 0)]),
    {"ip": 0x60, "r28": PAL_CACHE_FLUSH, "r8": 0,
     "r9": 0, "r10": 0, "r11": 0}, entry=0x10)

test_pal_cache_flush_bad_type = require_registers(
    "pal_cache_flush_bad_type",
    pal_call_program(PAL_CACHE_FLUSH, [(29, 0), (30, 0), (31, 0)]),
    {"ip": 0x60, "r28": PAL_CACHE_FLUSH,
     "r8": (-2 & 0xffffffffffffffff), "r9": 0, "r10": 0, "r11": 0},
    entry=0x10)

test_pal_cache_flush_bad_operation = require_registers(
    "pal_cache_flush_bad_operation",
    pal_call_program(PAL_CACHE_FLUSH, [(29, 3), (30, 4), (31, 0)]),
    {"ip": 0x60, "r28": PAL_CACHE_FLUSH,
     "r8": (-2 & 0xffffffffffffffff), "r9": 0, "r10": 0, "r11": 0},
    entry=0x10)

test_pal_cache_init = require_registers("pal_cache_init",
    pal_call_program(PAL_CACHE_INIT, [(29, 0), (30, 3), (31, 0)]),
    {"ip": 0x60, "r28": PAL_CACHE_INIT, "r8": 0, "r9": 0, "r10": 0,
     "r11": 0}, entry=0x10)

test_pal_cache_init_invalid = require_registers("pal_cache_init_invalid",
    pal_call_program(PAL_CACHE_INIT, [(29, 0), (30, 3), (31, 2)]),
    {"ip": 0x60, "r28": PAL_CACHE_INIT,
     "r8": (-2 & 0xffffffffffffffff), "r9": 0, "r10": 0, "r11": 0},
    entry=0x10)

test_pal_cache_prot_info = require_registers("pal_cache_prot_info",
    pal_call_program(PAL_CACHE_PROT_INFO, [(29, 0), (30, 1), (31, 0)]),
    {"ip": 0x60, "r28": PAL_CACHE_PROT_INFO, "r8": 0,
     "r9": PAL_CACHE_PROT_DATA_NONE | (PAL_CACHE_PROT_TAG_NONE_L0 << 32),
     "r10": 0, "r11": 0}, entry=0x10)

test_pal_cache_prot_info_invalid = require_registers(
    "pal_cache_prot_info_invalid",
    pal_call_program(PAL_CACHE_PROT_INFO, [(29, 0), (30, 3), (31, 0)]),
    {"ip": 0x60, "r28": PAL_CACHE_PROT_INFO,
     "r8": (-2 & 0xffffffffffffffff), "r9": 0, "r10": 0, "r11": 0},
    entry=0x10)

test_pal_cache_prot_info_unified_bad_type = require_registers(
    "pal_cache_prot_info_unified_bad_type",
    pal_call_program(PAL_CACHE_PROT_INFO, [(29, 2), (30, 1), (31, 0)]),
    {"ip": 0x60, "r28": PAL_CACHE_PROT_INFO,
     "r8": (-2 & 0xffffffffffffffff), "r9": 0, "r10": 0, "r11": 0},
    entry=0x10)

test_pal_mem_attrib = require_registers("pal_mem_attrib",
    pal_call_program(PAL_MEM_ATTRIB),
    {"ip": 0x30, "r28": PAL_MEM_ATTRIB, "r8": 0,
     "r9": PAL_MEM_ATTRIB_WB_UC_UCE_WC, "r10": 0, "r11": 0}, entry=0x10)

test_pal_mem_attrib_reserved_arg = require_registers(
    "pal_mem_attrib_reserved_arg",
    pal_call_program(PAL_MEM_ATTRIB, [(29, 1), (30, 0), (31, 0)]),
    {"ip": 0x60, "r28": PAL_MEM_ATTRIB,
     "r8": (-2 & 0xffffffffffffffff), "r9": 0, "r10": 0, "r11": 0},
    entry=0x10)

test_pal_bus_get_features = require_registers("pal_bus_get_features",
    pal_call_program(PAL_BUS_GET_FEATURES),
    {"ip": 0x30, "r28": PAL_BUS_GET_FEATURES, "r8": 0,
     "r9": 0, "r10": 0, "r11": 0}, entry=0x10)

test_pal_bus_get_features_reserved_arg = require_registers(
    "pal_bus_get_features_reserved_arg",
    pal_call_program(PAL_BUS_GET_FEATURES, [(29, 0), (30, 1), (31, 0)]),
    {"ip": 0x60, "r28": PAL_BUS_GET_FEATURES,
     "r8": (-2 & 0xffffffffffffffff), "r9": 0, "r10": 0, "r11": 0},
    entry=0x10)

test_pal_bus_set_features = require_registers("pal_bus_set_features",
    pal_call_program(PAL_BUS_SET_FEATURES, [(29, 0x1234), (30, 0), (31, 0)]),
    {"ip": 0x60, "r28": PAL_BUS_SET_FEATURES, "r8": 0,
     "r9": 0, "r10": 0, "r11": 0}, entry=0x10)

test_pal_bus_set_features_invalid = require_registers(
    "pal_bus_set_features_invalid",
    pal_call_program(PAL_BUS_SET_FEATURES, [(29, 0), (30, 1), (31, 0)]),
    {"ip": 0x60, "r28": PAL_BUS_SET_FEATURES,
     "r8": (-2 & 0xffffffffffffffff), "r9": 0, "r10": 0, "r11": 0},
    entry=0x10)

test_pal_proc_set_features = require_registers("pal_proc_set_features",
    pal_call_program(PAL_PROC_SET_FEATURES, [(29, 0x55), (30, 0), (31, 0)]),
    {"ip": 0x60, "r28": PAL_PROC_SET_FEATURES, "r8": 0,
     "r9": 0, "r10": 0, "r11": 0}, entry=0x10)

test_pal_proc_set_features_invalid = require_registers(
    "pal_proc_set_features_invalid",
    pal_call_program(PAL_PROC_SET_FEATURES, [(29, 0), (30, 0), (31, 1)]),
    {"ip": 0x60, "r28": PAL_PROC_SET_FEATURES,
     "r8": (-2 & 0xffffffffffffffff), "r9": 0, "r10": 0, "r11": 0},
    entry=0x10)

test_pal_proc_get_features = require_registers("pal_proc_get_features",
    pal_call_program(PAL_PROC_GET_FEATURES),
    {"ip": 0x30, "r28": PAL_PROC_GET_FEATURES, "r8": 0,
     "r9": 0, "r10": 0, "r11": 0}, entry=0x10)

test_pal_proc_get_features_reserved_arg = require_registers(
    "pal_proc_get_features_reserved_arg",
    pal_call_program(PAL_PROC_GET_FEATURES, [(29, 0), (30, 0), (31, 1)]),
    {"ip": 0x60, "r28": PAL_PROC_GET_FEATURES,
     "r8": (-2 & 0xffffffffffffffff), "r9": 0, "r10": 0, "r11": 0},
    entry=0x10)

test_pal_proc_get_features_montecito_next_set = require_registers(
    "pal_proc_get_features_montecito_next_set",
    pal_call_program(PAL_PROC_GET_FEATURES, [(29, 0), (30, 16), (31, 0)]),
    {"ip": 0x60, "r28": PAL_PROC_GET_FEATURES,
     "r8": 1, "r9": 0, "r10": 0, "r11": 0}, entry=0x10)

test_pal_proc_get_features_montecito_set18 = require_registers(
    "pal_proc_get_features_montecito_set18",
    pal_call_program(PAL_PROC_GET_FEATURES, [(29, 0), (30, 18), (31, 0)]),
    {"ip": 0x60, "r28": PAL_PROC_GET_FEATURES,
     "r8": 0, "r9": 1 << 18, "r10": 1 << 18, "r11": 0}, entry=0x10)

test_pal_proc_get_features_montecito_beyond_max = require_registers(
    "pal_proc_get_features_montecito_beyond_max",
    pal_call_program(PAL_PROC_GET_FEATURES, [(29, 0), (30, 19), (31, 0)]),
    {"ip": 0x60, "r28": PAL_PROC_GET_FEATURES,
     "r8": (-8 & 0xffffffffffffffff), "r9": 0, "r10": 0, "r11": 0},
    entry=0x10)

test_pal_proc_get_features_madison_beyond_max = require_registers(
    "pal_proc_get_features_madison_beyond_max",
    pal_call_program(PAL_PROC_GET_FEATURES, [(29, 0), (30, 17), (31, 0)]),
    {"ip": 0x60, "r28": PAL_PROC_GET_FEATURES,
     "r8": (-8 & 0xffffffffffffffff), "r9": 0, "r10": 0, "r11": 0},
    entry=0x10, cpu="madison")

# Itanium 2 implements set 16 and no feature in it; the HP zx1 firmware stops
# its boot when the call fails.
test_pal_proc_get_features_madison_set16 = require_registers(
    "pal_proc_get_features_madison_set16",
    pal_call_program(PAL_PROC_GET_FEATURES, [(29, 0), (30, 16), (31, 0)]),
    {"ip": 0x60, "r28": PAL_PROC_GET_FEATURES, "r8": 0,
     "r9": 0, "r10": 0, "r11": 0}, entry=0x10, cpu="madison")

test_pal_proc_get_features_merced_beyond_max = require_registers(
    "pal_proc_get_features_merced_beyond_max",
    pal_call_program(PAL_PROC_GET_FEATURES, [(29, 0), (30, 16), (31, 0)]),
    {"ip": 0x60, "r28": PAL_PROC_GET_FEATURES,
     "r8": (-8 & 0xffffffffffffffff), "r9": 0, "r10": 0, "r11": 0},
    entry=0x10, cpu="merced")

# A feature that cannot be set is ignored, so the call still succeeds.
test_pal_proc_set_features_madison_set16 = require_registers(
    "pal_proc_set_features_madison_set16",
    pal_call_program(PAL_PROC_SET_FEATURES, [(29, 0x48), (30, 16), (31, 0)]),
    {"ip": 0x60, "r28": PAL_PROC_SET_FEATURES, "r8": 0,
     "r9": 0, "r10": 0, "r11": 0}, entry=0x10, cpu="madison")

test_pal_proc_set_features_merced_beyond_max = require_registers(
    "pal_proc_set_features_merced_beyond_max",
    pal_call_program(PAL_PROC_SET_FEATURES, [(29, 0x48), (30, 16), (31, 0)]),
    {"ip": 0x60, "r28": PAL_PROC_SET_FEATURES,
     "r8": (-8 & 0xffffffffffffffff), "r9": 0, "r10": 0, "r11": 0},
    entry=0x10, cpu="merced")

# Implementation-specific index the HP zx1 SAL_B needs (SDM Vol. 2 table 11-12).
test_pal_impl_proc_response_timeout = require_registers(
    "pal_impl_proc_response_timeout",
    pal_call_program(0x213, [(29, 0x1f), (30, 0), (31, 0)]),
    {"ip": 0x60, "r28": 0x213, "r8": 0,
     "r9": 0, "r10": 0, "r11": 0}, entry=0x10, cpu="madison")

test_pal_impl_proc_response_timeout_merced = require_registers(
    "pal_impl_proc_response_timeout_merced",
    pal_call_program(0x213, [(29, 0x1f), (30, 0), (31, 0)]),
    {"ip": 0x60, "r28": 0x213,
     "r8": (-1 & 0xffffffffffffffff), "r9": 0, "r10": 0, "r11": 0},
    entry=0x10, cpu="merced")

test_pal_logical_to_physical_current = require_registers(
    "pal_logical_to_physical_current",
    pal_call_program(PAL_LOGICAL_TO_PHYSICAL,
                     [(29, 0xffffffffffffffff), (30, 0), (31, 0)]),
    {"ip": 0x60, "r28": PAL_LOGICAL_TO_PHYSICAL, "r8": 0,
     "r9": 1 | (1 << 16) | (1 << 32), "r10": 0, "r11": 0},
    entry=0x10)

test_pal_logical_to_physical_multicore_thread = require_registers(
    "pal_logical_to_physical_multicore_thread",
    pal_call_program(PAL_LOGICAL_TO_PHYSICAL,
                     [(29, 3), (30, 0), (31, 0)]),
    {"ip": 0x60, "r28": PAL_LOGICAL_TO_PHYSICAL, "r8": 0,
     "r9": 4 | (2 << 16) | (2 << 32),
     "r10": 1 | (1 << 32), "r11": 3},
    entry=0x10, alat=None, smp="4,sockets=1,cores=2,threads=2")

test_pal_logical_to_physical_madison_unimplemented = require_registers(
    "pal_logical_to_physical_madison_unimplemented",
    pal_call_program(PAL_LOGICAL_TO_PHYSICAL,
                     [(29, 0xffffffffffffffff), (30, 0), (31, 0)]),
    {"ip": 0x60, "r28": PAL_LOGICAL_TO_PHYSICAL,
     "r8": (-1 & 0xffffffffffffffff), "r9": 0, "r10": 0, "r11": 0},
    entry=0x10, cpu="madison")

test_pal_cache_shared_info_single_thread = require_registers(
    "pal_cache_shared_info_single_thread",
    pal_call_program(PAL_CACHE_SHARED_INFO, [(29, 0), (30, 1), (31, 0)]),
    {"ip": 0x60, "r28": PAL_CACHE_SHARED_INFO, "r8": 0,
     "r9": 1, "r10": 0, "r11": 0}, entry=0x10)

test_pal_cache_shared_info_sibling_thread = require_registers(
    "pal_cache_shared_info_sibling_thread",
    pal_call_program(PAL_CACHE_SHARED_INFO, [(29, 2), (30, 2), (31, 1)]),
    {"ip": 0x60, "r28": PAL_CACHE_SHARED_INFO, "r8": 0,
     "r9": 2, "r10": 1, "r11": 1},
    entry=0x10, alat=None, smp="4,sockets=1,cores=2,threads=2")

test_pal_brand_info_string = require_registers(
    "pal_brand_info_string", [
        (0x10, 0x00, nop_m(), alloc(2, 4, 0, 0, 0), nop_i()),
        (0x20, *movl_mlx(28, PAL_BRAND_INFO)),
        (0x30, *movl_mlx(32, PAL_BRAND_INFO)),
        (0x40, *movl_mlx(33, 0)),
        (0x50, *movl_mlx(34, PAL_BRAND_BUFFER)),
        (0x60, *movl_mlx(35, 0)),
        (0x70, 0x10, nop_m(), nop_i(), br_call(0, 0x70, PAL_PROC_ENTRY)),
        (0x80, *movl_mlx(2, PAL_BRAND_BUFFER)),
        (0x90, 0x00, ld8(20, 2), nop_i(), nop_i()),
        (0xa0, 0x10, nop_m(), nop_i(), br_cond(0xa0, 0xa0)),
        (PAL_PROC_ENTRY, 0x0a, pal_break(), nop_m(), nop_i()),
        (PAL_PROC_ENTRY + 0x10, 0x10, nop_m(), nop_i(), br_ret(0)),
    ], {"ip": 0xa0, "r28": PAL_BRAND_INFO, "r8": 0,
        "r9": len("QEMU Montecito-compatible IA-64 CPU 1.60GHz 24MB"),
        "r10": 0, "r11": 0,
        "r20": int.from_bytes(b"QEMU Mon", "little")}, entry=0x10)

test_pal_brand_info_frequency = require_registers(
    "pal_brand_info_frequency",
    pal_stacked_call_program(PAL_BRAND_INFO, [16, 0, 0]),
    {"ip": 0x80, "r28": PAL_BRAND_INFO, "r8": 0,
     "r9": 1600000000, "r10": 0, "r11": 0}, entry=0x10)

test_pal_brand_info_cache = require_registers(
    "pal_brand_info_cache",
    pal_stacked_call_program(PAL_BRAND_INFO, [17, 0, 0]),
    {"ip": 0x80, "r28": PAL_BRAND_INFO, "r8": 0,
     "r9": 24 * 1024 * 1024, "r10": 0, "r11": 0}, entry=0x10)

test_pal_brand_info_bus = require_registers(
    "pal_brand_info_bus",
    pal_stacked_call_program(PAL_BRAND_INFO, [18, 0, 0]),
    {"ip": 0x80, "r28": PAL_BRAND_INFO, "r8": 0,
     "r9": 533333333, "r10": 0, "r11": 0}, entry=0x10)

# No IBR/DBR matching is implemented, so no breakpoint register pairs are
# advertised.  See the comment on pal_debug_info() in arch/pal.c.
test_pal_debug_info = require_registers("pal_debug_info",
    pal_call_program(PAL_DEBUG_INFO),
    {"ip": 0x30, "r28": PAL_DEBUG_INFO, "r8": 0,
     "r9": 0, "r10": 0, "r11": 0}, entry=0x10)

test_pal_debug_info_reserved_arg = require_registers(
    "pal_debug_info_reserved_arg",
    pal_call_program(PAL_DEBUG_INFO, [(29, 1), (30, 0), (31, 0)]),
    {"ip": 0x60, "r28": PAL_DEBUG_INFO,
     "r8": (-2 & 0xffffffffffffffff), "r9": 0, "r10": 0, "r11": 0},
    entry=0x10)

test_pal_register_info_application_implemented = require_registers(
    "pal_register_info_application_implemented",
    pal_call_program(PAL_REGISTER_INFO, [(29, 0), (30, 0), (31, 0)]),
    {"ip": 0x60, "r28": PAL_REGISTER_INFO, "r8": 0,
     "r9": PAL_AR_IMPLEMENTED_LOW, "r10": PAL_AR_IMPLEMENTED_HIGH,
     "r11": 0}, entry=0x10)

test_pal_register_info_application_side_effects = require_registers(
    "pal_register_info_application_side_effects",
    pal_call_program(PAL_REGISTER_INFO, [(29, 1), (30, 0), (31, 0)]),
    {"ip": 0x60, "r28": PAL_REGISTER_INFO, "r8": 0,
     "r9": 0, "r10": 0, "r11": 0}, entry=0x10)

test_pal_register_info_control_implemented = require_registers(
    "pal_register_info_control_implemented",
    pal_call_program(PAL_REGISTER_INFO, [(29, 2), (30, 0), (31, 0)]),
    {"ip": 0x60, "r28": PAL_REGISTER_INFO, "r8": 0,
     "r9": PAL_CR_IMPLEMENTED_LOW, "r10": PAL_CR_IMPLEMENTED_HIGH,
     "r11": 0}, entry=0x10)

test_pal_register_info_control_side_effects = require_registers(
    "pal_register_info_control_side_effects",
    pal_call_program(PAL_REGISTER_INFO, [(29, 3), (30, 0), (31, 0)]),
    {"ip": 0x60, "r28": PAL_REGISTER_INFO, "r8": 0,
     "r9": 0, "r10": PAL_CR_READ_SIDE_EFFECT_HIGH, "r11": 0},
    entry=0x10)

test_pal_register_info_invalid_request = require_registers(
    "pal_register_info_invalid_request",
    pal_call_program(PAL_REGISTER_INFO, [(29, 4), (30, 0), (31, 0)]),
    {"ip": 0x60, "r28": PAL_REGISTER_INFO,
     "r8": (-2 & 0xffffffffffffffff), "r9": 0, "r10": 0, "r11": 0},
    entry=0x10)

test_pal_register_info_reserved_arg = require_registers(
    "pal_register_info_reserved_arg",
    pal_call_program(PAL_REGISTER_INFO, [(29, 0), (30, 1), (31, 0)]),
    {"ip": 0x60, "r28": PAL_REGISTER_INFO,
     "r8": (-2 & 0xffffffffffffffff), "r9": 0, "r10": 0, "r11": 0},
    entry=0x10)

test_pal_perf_mon_info = require_registers("pal_perf_mon_info", [
    (0x10, *movl_mlx(29, PAL_PERF_BUFFER)),
    (0x20, 0x00, nop_m(), addl(30, 0, 0), addl(31, 0, 0)),
    (0x30, 0x00, nop_m(), addl(28, PAL_PERF_MON_INFO, 0), nop_i()),
    (0x40, 0x10, nop_m(), nop_i(), br_call(0, 0x40, PAL_PROC_ENTRY)),
    (0x50, *movl_mlx(2, PAL_PERF_BUFFER)),
    (0x60, 0x00, ld8(20, 2), adds(2, 8, 2), nop_i()),
    (0x70, 0x00, ld8(21, 2), adds(2, 0x18, 2), nop_i()),
    (0x80, 0x00, ld8(22, 2), adds(2, 8, 2), nop_i()),
    (0x90, 0x00, ld8(23, 2), adds(2, 0x18, 2), nop_i()),
    (0xa0, 0x00, ld8(24, 2), adds(2, 0x20, 2), nop_i()),
    (0xb0, 0x00, ld8(25, 2), nop_i(), nop_i()),
    (0xc0, 0x10, nop_m(), nop_i(), br_cond(0xc0, 0xc0)),
    (PAL_PROC_ENTRY, 0x0a, pal_break(), nop_m(), nop_i()),
    (PAL_PROC_ENTRY + 0x10, 0x10, nop_m(), nop_i(), br_ret(0)),
], {"ip": 0xc0, "r28": PAL_PERF_MON_INFO, "r8": 0,
    "r9": 0x08123004, "r10": 0, "r11": 0,
    "r20": 0x3fff, "r21": 0, "r22": 0x3ffff, "r23": 0,
    "r24": 0xf0, "r25": 0xf0},
    entry=0x10)

test_pal_perf_mon_info_bad_buffer = require_registers(
    "pal_perf_mon_info_bad_buffer",
    pal_call_program(PAL_PERF_MON_INFO,
                     [(29, PAL_PERF_BUFFER + 4), (30, 0), (31, 0)]),
    {"ip": 0x60, "r28": PAL_PERF_MON_INFO,
     "r8": (-2 & 0xffffffffffffffff), "r9": 0, "r10": 0, "r11": 0},
    entry=0x10)

test_pal_perf_mon_info_reserved_arg = require_registers(
    "pal_perf_mon_info_reserved_arg",
    pal_call_program(PAL_PERF_MON_INFO,
                     [(29, PAL_PERF_BUFFER), (30, 1), (31, 0)]),
    {"ip": 0x60, "r28": PAL_PERF_MON_INFO,
     "r8": (-2 & 0xffffffffffffffff), "r9": 0, "r10": 0, "r11": 0},
    entry=0x10)

test_pal_fixed_addr = require_registers("pal_fixed_addr",
    pal_call_program(PAL_FIXED_ADDR),
    {"ip": 0x30, "r28": PAL_FIXED_ADDR, "r8": 0,
     "r9": 0, "r10": 0, "r11": 0}, entry=0x10)

# PAL_FIXED_ADDR returns the processor's geographic id, the value PALE_RESET
# hands SAL in GR33 (SDM vol. 2 11.2.2).  The board sets it per socket (the
# 460gx board's second socket is id 3); here the command line sets it.
test_pal_fixed_addr_geographic_id = require_registers(
    "pal_fixed_addr_geographic_id",
    pal_call_program(PAL_FIXED_ADDR),
    {"ip": 0x30, "r28": PAL_FIXED_ADDR, "r8": 0,
     "r9": 3, "r10": 0, "r11": 0}, entry=0x10,
    extra_args=("-global", "ia64-cpu.geographic-id=3"))

# PALE_RESET calls SALE_ENTRY twice on every processor (SDM vol. 2 11.2.1,
# 11.2.2): function RECOVERY_CHECK (3) with cr.iva = 0 and GR36 = PAL_RESET's
# return address, then, after SAL returns there, function RESET (0) with the
# PAL IVT in cr.iva and GR36 = the PAL authentication procedure.  A 64 KiB
# flash image's SALE_ENTRY branches to RAM code that records the first call,
# returns to GR36 through its uncacheable alias (bit 63 set, as the zx1 SAL_A
# does), and on the second call loads both calls' values into registers.
SALE_FLASH_BASE = 0xFFFF0000
SALE_FLASH_SIZE = 0x10000
SALE_ENTRY_ADDR = SALE_FLASH_BASE + 0x200
SALE_CODE = 0x1000
SALE_DATA = 0x2000
SALE_PAL_PROC = 0xFF100000
SALE_PAL_RESET_RETURN = 0xFF100020
SALE_PAL_RESET_IVT = 0xFF300000


def _sale_mii(address, slot0, slot1=None):
    return (address, 0x01, slot0, _enc.nop_i() if slot1 is None else slot1,
            _enc.nop_i())


def _sale_two_call_bundles():
    nop_m = _enc.nop_m()
    bundles = [
        # r2 = function; RECOVERY_CHECK goes to 0x1100, anything else 0x1200.
        _sale_mii(0x1000, nop_m, _enc.extr_u(2, 20, 0, 8)),
        _sale_mii(0x1010, nop_m, _enc.cmp_eq_imm(6, 7, 3, 2)),
        (0x1020, 0x11, nop_m, _enc.nop_i(), _enc.br_cond(0x1020, 0x1100, qp=6)),
        (0x1030, 0x11, nop_m, _enc.nop_i(), _enc.br_cond(0x1030, 0x1200)),
        # First call: store GR20, GR36, cr.iva, GR33; return to GR36 | bit 63.
        (0x1100, 0x05, *_enc.movl_mlx(3, SALE_DATA)[1:]),
        _sale_mii(0x1110, _enc.st8(3, 20)),
        _sale_mii(0x1120, nop_m, _enc.addl(3, 8, 3)),
        _sale_mii(0x1130, _enc.st8(3, 36)),
        _sale_mii(0x1140, nop_m, _enc.addl(3, 8, 3)),
        _sale_mii(0x1150, _enc.mov_m_cr_gr(2, 2)),
        _sale_mii(0x1160, _enc.st8(3, 2)),
        _sale_mii(0x1170, nop_m, _enc.addl(3, 8, 3)),
        _sale_mii(0x1180, _enc.st8(3, 33)),
        (0x1190, 0x05, *_enc.movl_mlx(2, 1 << 63)[1:]),
        _sale_mii(0x11a0, nop_m, _enc.or_reg(2, 2, 36)),
        _sale_mii(0x11b0, nop_m, _enc.mov_br_gr(6, 2)),
        (0x11c0, 0x11, nop_m, _enc.nop_i(), _enc.br_indirect(6)),
        # Second call: r8-r10, r2 = first GR20, GR36, cr.iva, GR33;
        # r11, r14, r15, r3 = the same now.
        (0x1200, 0x05, *_enc.movl_mlx(3, SALE_DATA)[1:]),
        _sale_mii(0x1210, _enc.ld8(8, 3)),
        _sale_mii(0x1220, nop_m, _enc.addl(3, 8, 3)),
        _sale_mii(0x1230, _enc.ld8(9, 3)),
        _sale_mii(0x1240, nop_m, _enc.addl(3, 8, 3)),
        _sale_mii(0x1250, _enc.ld8(10, 3)),
        _sale_mii(0x1260, nop_m, _enc.addl(3, 8, 3)),
        _sale_mii(0x1270, _enc.ld8(2, 3)),
        _sale_mii(0x1280, nop_m, _enc.or_reg(11, 20, 0)),
        _sale_mii(0x1290, nop_m, _enc.or_reg(14, 36, 0)),
        _sale_mii(0x12a0, _enc.mov_m_cr_gr(15, 2)),
        _sale_mii(0x12b0, nop_m, _enc.or_reg(3, 33, 0)),
        (0x12c0, 0x11, nop_m, _enc.nop_i(), _enc.br_cond(0x12c0, 0x12c0)),
    ]
    return bundles


def _sale_flash_image():
    image = bytearray(b"\xff" * SALE_FLASH_SIZE)
    fit = 0x100
    image[fit:fit + 8] = b"_FIT_   "
    struct.pack_into("<Q", image, fit + 8, 0x0100000000000001)
    low, high = _enc.bundle_words(
        *_enc.brl_cond_mlx(SALE_ENTRY_ADDR, SALE_CODE))
    struct.pack_into("<QQ", image, SALE_ENTRY_ADDR - SALE_FLASH_BASE,
                     low, high)
    struct.pack_into("<Q", image, SALE_FLASH_SIZE - 32,
                     (1 << 63) | (SALE_FLASH_BASE + fit))
    struct.pack_into("<Q", image, SALE_FLASH_SIZE - 24,
                     (1 << 63) | SALE_ENTRY_ADDR)
    return bytes(image)


SALE_TWO_CALL_EXPECTED = {
    "ip": 0x12c0,
    "r8": 3, "r9": SALE_PAL_RESET_RETURN, "r10": 0, "r2": 0,
    "r11": 0, "r14": SALE_PAL_PROC, "r15": SALE_PAL_RESET_IVT, "r3": 0,
}


def _sale_entry_two_calls(qemu):
    with tempfile.TemporaryDirectory(prefix="ia64-sale-") as tmpdir:
        flash = os.path.join(tmpdir, "flash.bin")
        with open(flash, "wb") as f:
            f.write(_sale_flash_image())
        _enc.run_program(qemu, _sale_two_call_bundles(),
                         entry=None,
                         expected=SALE_TWO_CALL_EXPECTED,
                         name="sale_entry_two_calls",
                         extra_args=("-bios", flash))


test_sale_entry_two_calls = IA64Case(
    name="sale_entry_two_calls", runner=_sale_entry_two_calls,
    bundles=tuple(tuple(b) for b in _sale_two_call_bundles()),
    expected=dict(SALE_TWO_CALL_EXPECTED),
    metadata=CaseMetadata(required_features=frozenset({"alat:full"})),
)

test_pal_fixed_addr_reserved_arg = require_registers(
    "pal_fixed_addr_reserved_arg",
    pal_call_program(PAL_FIXED_ADDR, [(29, 1), (30, 0), (31, 0)]),
    {"ip": 0x60, "r28": PAL_FIXED_ADDR,
     "r8": (-2 & 0xffffffffffffffff), "r9": 0, "r10": 0, "r11": 0},
    entry=0x10)

test_pal_platform_addr_interrupt = require_registers(
    "pal_platform_addr_interrupt",
    pal_call_program(PAL_PLATFORM_ADDR,
                     [(29, PAL_PLATFORM_INTERRUPT_BLOCK),
                      (30, PAL_INTERRUPT_BLOCK_DEFAULT), (31, 0)]),
    {"ip": 0x60, "r28": PAL_PLATFORM_ADDR, "r8": 0,
     "r9": 0, "r10": 0, "r11": 0}, entry=0x10)

test_pal_platform_addr_ignores_bit63 = require_registers(
    "pal_platform_addr_ignores_bit63",
    pal_call_program(PAL_PLATFORM_ADDR,
                     [(29, PAL_PLATFORM_INTERRUPT_BLOCK),
                      (30, PAL_INTERRUPT_BLOCK_DEFAULT | (1 << 63)),
                      (31, 0)]),
    {"ip": 0x60, "r28": PAL_PLATFORM_ADDR, "r8": 0,
     "r9": 0, "r10": 0, "r11": 0}, entry=0x10)

# The I/O block is the top 64 MB of the processor's implemented physical
# address space (SDM vol. 2 11.2.2 leaves the address to the implementation;
# the HP zx1 firmware asks an Itanium 2 for 3_FFFF_FC00_0000).  Each model
# takes its own address and refuses the other one.
test_pal_platform_addr_io = require_registers(
    "pal_platform_addr_io",
    pal_call_program(PAL_PLATFORM_ADDR,
                     [(29, PAL_PLATFORM_IO_BLOCK),
                      (30, PAL_IO_BLOCK_DEFAULT), (31, 0)]),
    {"ip": 0x60, "r28": PAL_PLATFORM_ADDR, "r8": 0,
     "r9": 0, "r10": 0, "r11": 0}, entry=0x10)

test_pal_platform_addr_io_merced_address = require_registers(
    "pal_platform_addr_io_merced_address",
    pal_call_program(PAL_PLATFORM_ADDR,
                     [(29, PAL_PLATFORM_IO_BLOCK),
                      (30, PAL_IO_BLOCK_MERCED), (31, 0)]),
    {"ip": 0x60, "r28": PAL_PLATFORM_ADDR,
     "r8": (-3 & 0xffffffffffffffff), "r9": 0, "r10": 0, "r11": 0},
    entry=0x10)

test_pal_platform_addr_io_merced = require_registers(
    "pal_platform_addr_io_merced",
    pal_call_program(PAL_PLATFORM_ADDR,
                     [(29, PAL_PLATFORM_IO_BLOCK),
                      (30, PAL_IO_BLOCK_MERCED), (31, 0)]),
    {"ip": 0x60, "r28": PAL_PLATFORM_ADDR, "r8": 0,
     "r9": 0, "r10": 0, "r11": 0}, entry=0x10, cpu="merced")

test_pal_platform_addr_io_merced_rejects_itanium2 = require_registers(
    "pal_platform_addr_io_merced_rejects_itanium2",
    pal_call_program(PAL_PLATFORM_ADDR,
                     [(29, PAL_PLATFORM_IO_BLOCK),
                      (30, PAL_IO_BLOCK_DEFAULT), (31, 0)]),
    {"ip": 0x60, "r28": PAL_PLATFORM_ADDR,
     "r8": (-3 & 0xffffffffffffffff), "r9": 0, "r10": 0, "r11": 0},
    entry=0x10, cpu="merced")

test_pal_platform_addr_bad_type = require_registers(
    "pal_platform_addr_bad_type",
    pal_call_program(PAL_PLATFORM_ADDR,
                     [(29, 2), (30, PAL_INTERRUPT_BLOCK_DEFAULT), (31, 0)]),
    {"ip": 0x60, "r28": PAL_PLATFORM_ADDR,
     "r8": (-2 & 0xffffffffffffffff), "r9": 0, "r10": 0, "r11": 0},
    entry=0x10)

test_pal_platform_addr_unmapped = require_registers(
    "pal_platform_addr_unmapped",
    pal_call_program(PAL_PLATFORM_ADDR,
                     [(29, PAL_PLATFORM_INTERRUPT_BLOCK),
                      (30, 0x200000), (31, 0)]),
    {"ip": 0x60, "r28": PAL_PLATFORM_ADDR,
     "r8": (-3 & 0xffffffffffffffff), "r9": 0, "r10": 0, "r11": 0},
    entry=0x10)

test_pal_mc_clear_log = require_registers("pal_mc_clear_log",
    pal_call_program(PAL_MC_CLEAR_LOG, [(29, 0), (30, 0), (31, 0)]),
    {"ip": 0x60, "r28": PAL_MC_CLEAR_LOG, "r8": 0,
     "r9": 0, "r10": 0, "r11": 0}, entry=0x10)

test_pal_copy_info = require_registers("pal_copy_info",
    pal_call_program(PAL_COPY_INFO, [(29, 0), (30, 0), (31, 0)]),
    {"ip": 0x60, "r28": PAL_COPY_INFO, "r8": 0,
     "r9": PAL_COPY_BUFFER_SIZE, "r10": PAL_COPY_BUFFER_ALIGN, "r11": 0},
    entry=0x10)

test_pal_copy_info_bad_type = require_registers("pal_copy_info_bad_type",
    pal_call_program(PAL_COPY_INFO, [(29, 2), (30, 0), (31, 0)]),
    {"ip": 0x60, "r28": PAL_COPY_INFO,
     "r8": (-2 & 0xffffffffffffffff), "r9": 0, "r10": 0, "r11": 0},
    entry=0x10)

test_pal_copy_info_ia32_unsupported = require_registers(
    "pal_copy_info_ia32_unsupported",
    pal_call_program(PAL_COPY_INFO, [(29, 1), (30, 1), (31, 0)]),
    {"ip": 0x60, "r28": PAL_COPY_INFO,
     "r8": (-3 & 0xffffffffffffffff), "r9": 0, "r10": 0, "r11": 0},
    entry=0x10)

test_pal_copy_info_platform_for_ia64 = require_registers(
    "pal_copy_info_platform_for_ia64",
    pal_call_program(PAL_COPY_INFO, [(29, 0), (30, 1), (31, 0)]),
    {"ip": 0x60, "r28": PAL_COPY_INFO,
     "r8": (-2 & 0xffffffffffffffff), "r9": 0, "r10": 0, "r11": 0},
    entry=0x10)

# PAL_FIRMWARE_REGISTER (hw/ia64/ia64_vpc_abi.h): the project firmware's
# registration with the PAL emulation.  A record without the magic -- any
# other firmware using the index for its own PAL -- reads as not implemented.
PAL_FIRMWARE_REGISTER = 0x200
FW_REGISTRATION_MAGIC = 0x4752574634364149
FW_REGISTRATION_ADDR = 0x4000

test_pal_firmware_register_rejects_unknown_record = require_registers(
    "pal_firmware_register_rejects_unknown_record",
    pal_call_program(PAL_FIRMWARE_REGISTER, [(29, 0x10), (30, 64), (31, 0)]),
    {"ip": 0x60, "r28": PAL_FIRMWARE_REGISTER,
     "r8": (-1 & 0xffffffffffffffff), "r9": 0, "r10": 0, "r11": 0},
    entry=0x10)


def _pal_firmware_register_program(words, size):
    program = [(0x10, *movl_mlx(16, FW_REGISTRATION_ADDR))]
    addr = 0x20
    for word in words:
        program.append((addr, *movl_mlx(17, word)))
        program.append((addr + 0x10, 0x00, st8(16, 17), adds(16, 8, 16),
                        nop_i()))
        addr += 0x20
    program += [
        (addr, 0x00, nop_m(), addl(28, PAL_FIRMWARE_REGISTER, 0), nop_i()),
        (addr + 0x10, *movl_mlx(29, FW_REGISTRATION_ADDR)),
        (addr + 0x20, 0x00, nop_m(), addl(30, size, 0), addl(31, 0, 0)),
        (addr + 0x30, 0x10, nop_m(), nop_i(),
         br_call(0, addr + 0x30, PAL_PROC_ENTRY)),
        (addr + 0x40, 0x10, nop_m(), nop_i(),
         br_cond(addr + 0x40, addr + 0x40)),
        (PAL_PROC_ENTRY, 0x0a, pal_break(), nop_m(), nop_i()),
        (PAL_PROC_ENTRY + 0x10, 0x10, nop_m(), nop_i(), br_ret(0)),
    ]
    return program, addr + 0x40


# The layout the machine's no-firmware entry state already registers.
_FW_REGISTRATION_WORDS = [
    FW_REGISTRATION_MAGIC, 0x100000, 0x100000, 0x108000,
    0x102000, 0x102020, 0x102040, 0x07e00000,
]
_register_ok, _register_ok_ip = _pal_firmware_register_program(
    _FW_REGISTRATION_WORDS, 64)
test_pal_firmware_register_accepts_record = require_registers(
    "pal_firmware_register_accepts_record", _register_ok,
    {"ip": _register_ok_ip, "r28": PAL_FIRMWARE_REGISTER,
     "r8": 0, "r9": 0, "r10": 0, "r11": 0},
    entry=0x10)

_register_short, _register_short_ip = _pal_firmware_register_program(
    _FW_REGISTRATION_WORDS, 56)
test_pal_firmware_register_rejects_short_record = require_registers(
    "pal_firmware_register_rejects_short_record", _register_short,
    {"ip": _register_short_ip, "r28": PAL_FIRMWARE_REGISTER,
     "r8": (-1 & 0xffffffffffffffff), "r9": 0, "r10": 0, "r11": 0},
    entry=0x10)

test_pal_copy_pal_entry_callable = require_registers(
    "pal_copy_pal_entry_callable", [
        (0x10, 0x00, nop_m(), alloc(2, 4, 0, 0, 0), nop_i()),
        (0x20, *movl_mlx(28, PAL_COPY_PAL)),
        (0x30, *movl_mlx(32, PAL_COPY_PAL)),
        (0x40, *movl_mlx(33, PAL_COPY_TARGET | (1 << 63))),
        (0x50, *movl_mlx(34, PAL_COPY_BUFFER_SIZE)),
        (0x60, *movl_mlx(35, 0)),
        (0x70, 0x10, nop_m(), nop_i(),
         br_call(0, 0x70, PAL_PROC_ENTRY)),
        (0x80, *movl_mlx(28, PAL_VERSION)),
        (0x90, 0x00, nop_m(), addl(29, 0, 0), addl(30, 0, 0)),
        (0xa0, 0x10, nop_m(), addl(31, 0, 0),
         br_call(0, 0xa0, PAL_COPY_TARGET)),
        (0xb0, 0x10, nop_m(), nop_i(),
         br_cond(0xb0, 0xb0)),
        (PAL_PROC_ENTRY, 0x0a, pal_break(), nop_m(), nop_i()),
        (PAL_PROC_ENTRY + 0x10, 0x10, nop_m(), nop_i(),
         br_ret(0)),
    ],
    {"ip": 0xb0, "r28": PAL_VERSION, "r8": 0,
     "r9": PAL_VERSION_VALUE, "r10": PAL_VERSION_VALUE, "r11": 0},
    entry=0x10)

test_pal_copy_pal_ap_entry_callable = require_registers(
    "pal_copy_pal_ap_entry_callable", [
        (0x10, 0x00, nop_m(), alloc(2, 4, 0, 0, 0), nop_i()),
        (0x20, *movl_mlx(28, PAL_COPY_PAL)),
        (0x30, *movl_mlx(32, PAL_COPY_PAL)),
        (0x40, *movl_mlx(33, PAL_COPY_TARGET | (1 << 63))),
        (0x50, *movl_mlx(34, PAL_COPY_BUFFER_SIZE)),
        (0x60, *movl_mlx(35, 1)),
        (0x70, 0x10, nop_m(), nop_i(),
         br_call(0, 0x70, PAL_PROC_ENTRY)),
        (0x80, *movl_mlx(28, PAL_VERSION)),
        (0x90, 0x00, nop_m(), addl(29, 0, 0), addl(30, 0, 0)),
        (0xa0, 0x10, nop_m(), addl(31, 0, 0),
         br_call(0, 0xa0, PAL_COPY_TARGET)),
        (0xb0, 0x10, nop_m(), nop_i(),
         br_cond(0xb0, 0xb0)),
        (PAL_PROC_ENTRY, 0x0a, pal_break(), nop_m(), nop_i()),
        (PAL_PROC_ENTRY + 0x10, 0x10, nop_m(), nop_i(),
         br_ret(0)),
        (PAL_COPY_TARGET, 0x0a, pal_break(), nop_m(), nop_i()),
        (PAL_COPY_TARGET + 0x10, 0x10, nop_m(), nop_i(),
         br_ret(0)),
    ],
    {"ip": 0xb0, "r28": PAL_VERSION, "r8": 0,
     "r9": PAL_VERSION_VALUE, "r10": PAL_VERSION_VALUE, "r11": 0},
    entry=0x10)

# Regression: the relocated PAL entry that pal_copy_pal writes must return via
# a plain branch (br.many b0), not br.ret.  Real firmware reaches a static PAL
# procedure at the relocated entry by a *plain* branch (br) without pushing a
# frame; a br.ret in the stub would pop the caller's frame and corrupt its
# stacked registers (observed with real SDV firmware).  This test relocates
# PAL, invokes PAL_VERSION at the relocated entry via a *plain* branch (b1,
# with the return in b0), and then reads the relocated return bundle back: its
# first word must be the br.many encoding (0x0000000100000011), never the
# br.ret encoding (0x0000000100000010).  See target/ia64/arch/pal.c
# pal_copy_pal and 64703dd.
test_pal_copy_pal_relocated_entry_plain_branch = require_registers(
    "pal_copy_pal_relocated_entry_plain_branch", [
        # Relocate PAL to PAL_COPY_TARGET (standard stacked call).
        (0x10, 0x00, nop_m(), alloc(2, 4, 0, 0, 0), nop_i()),
        (0x20, *movl_mlx(28, PAL_COPY_PAL)),
        (0x30, *movl_mlx(33, PAL_COPY_TARGET | (1 << 63))),
        (0x40, *movl_mlx(34, PAL_COPY_BUFFER_SIZE)),
        (0x50, *movl_mlx(35, 0)),
        (0x60, 0x10, nop_m(), nop_i(),
         br_call(0, 0x60, PAL_PROC_ENTRY)),
        # Invoke PAL_VERSION at the relocated entry via a plain branch: b1 is
        # the entry, b0 is the return (0xd0); no frame is pushed.
        (0x70, *movl_mlx(28, PAL_VERSION)),
        (0x80, *movl_mlx(7, PAL_COPY_TARGET)),
        (0x90, 0x00, nop_m(), mov_b_gr(1, 7), nop_i()),
        (0xa0, *movl_mlx(7, 0xd0)),
        (0xb0, 0x00, nop_m(), mov_b_gr(0, 7), nop_i()),
        (0xc0, 0x10, nop_m(), nop_i(), br_indirect(1)),
        # Read the relocated return bundle back and check it is br.many.
        (0xd0, *movl_mlx(5, PAL_COPY_TARGET + 0x10)),
        (0xe0, 0x00, ld8(6, 5), nop_i(), nop_i()),
        (0xf0, 0x10, nop_m(), nop_i(), br_cond(0xf0, 0xf0)),
        (PAL_PROC_ENTRY, 0x0a, pal_break(), nop_m(), nop_i()),
        (PAL_PROC_ENTRY + 0x10, 0x10, nop_m(), nop_i(), br_ret(0)),
    ],
    {"ip": 0xf0, "r6": 0x0000000100000011, "r28": PAL_VERSION, "r8": 0,
     "r9": PAL_VERSION_VALUE, "r10": PAL_VERSION_VALUE, "r11": 0},
    entry=0x10)

test_pal_copy_pal_bad_alloc = require_registers("pal_copy_pal_bad_alloc",
    pal_stacked_call_program(PAL_COPY_PAL,
                             [PAL_COPY_TARGET, PAL_COPY_BUFFER_SIZE - 1, 0]),
    {"ip": 0x80, "r28": PAL_COPY_PAL,
     "r8": (-2 & 0xffffffffffffffff), "r9": 0, "r10": 0, "r11": 0},
    entry=0x10)

test_pal_copy_pal_bad_alignment = require_registers(
    "pal_copy_pal_bad_alignment",
    pal_stacked_call_program(PAL_COPY_PAL,
                             [PAL_COPY_TARGET + 0x20,
                              PAL_COPY_BUFFER_SIZE, 0]),
    {"ip": 0x80, "r28": PAL_COPY_PAL,
     "r8": (-2 & 0xffffffffffffffff), "r9": 0, "r10": 0, "r11": 0},
    entry=0x10)

test_pal_copy_pal_bad_processor = require_registers(
    "pal_copy_pal_bad_processor",
    pal_stacked_call_program(PAL_COPY_PAL,
                             [PAL_COPY_TARGET, PAL_COPY_BUFFER_SIZE, 2]),
    {"ip": 0x80, "r28": PAL_COPY_PAL,
     "r8": (-2 & 0xffffffffffffffff), "r9": 0, "r10": 0, "r11": 0},
    entry=0x10)

test_pal_halt_info = require_registers("pal_halt_info", [
    (0x10, 0x00, nop_m(), alloc(2, 4, 0, 0, 0), nop_i()),
    (0x20, *movl_mlx(28, PAL_HALT_INFO)),
    (0x30, *movl_mlx(32, PAL_HALT_INFO)),
    (0x40, *movl_mlx(33, PAL_HALT_INFO_BUFFER)),
    (0x50, *movl_mlx(34, 0)),
    (0x60, *movl_mlx(35, 0)),
    (0x70, 0x10, nop_m(), nop_i(), br_call(0, 0x70, PAL_PROC_ENTRY)),
    (0x80, *movl_mlx(2, PAL_HALT_INFO_BUFFER)),
    (0x90, 0x00, ld8(20, 2), adds(2, 8, 2), nop_i()),
    (0xa0, 0x00, ld8(21, 2), adds(2, 0x30, 2), nop_i()),
    (0xb0, 0x00, ld8(22, 2), nop_i(), nop_i()),
    (0xc0, 0x10, nop_m(), nop_i(), br_cond(0xc0, 0xc0)),
    (PAL_PROC_ENTRY, 0x0a, pal_break(), nop_m(), nop_i()),
    (PAL_PROC_ENTRY + 0x10, 0x10, nop_m(), nop_i(), br_ret(0)),
], {"ip": 0xc0, "r28": PAL_HALT_INFO, "r8": 0,
    "r9": 0, "r10": 0, "r11": 0,
    "r20": PAL_HALT_LIGHT_INFO, "r21": PAL_HALT_STATE1_INFO, "r22": 0},
    entry=0x10)

test_pal_halt_invalid_state = require_registers("pal_halt_invalid_state",
    pal_call_program(PAL_HALT, [(29, 0), (30, 0), (31, 0)]),
    {"ip": 0x60, "r28": PAL_HALT,
     "r8": (-2 & 0xffffffffffffffff), "r9": 0, "r10": 0, "r11": 0},
    entry=0x10)

test_pal_halt_reserved_arg = require_registers("pal_halt_reserved_arg",
    pal_call_program(PAL_HALT, [(29, 1), (30, 0), (31, 1)]),
    {"ip": 0x60, "r28": PAL_HALT,
     "r8": (-2 & 0xffffffffffffffff), "r9": 0, "r10": 0, "r11": 0},
    entry=0x10)

test_pal_halt_info_bad_buffer = require_registers(
    "pal_halt_info_bad_buffer",
    pal_stacked_call_program(PAL_HALT_INFO,
                             [PAL_HALT_INFO_BUFFER + 4, 0, 0]),
    {"ip": 0x80, "r28": PAL_HALT_INFO,
     "r8": (-2 & 0xffffffffffffffff), "r9": 0, "r10": 0, "r11": 0},
    entry=0x10)

test_pal_halt_info_reserved_arg = require_registers(
    "pal_halt_info_reserved_arg",
    pal_stacked_call_program(PAL_HALT_INFO, [PAL_HALT_INFO_BUFFER, 1, 0]),
    {"ip": 0x80, "r28": PAL_HALT_INFO,
     "r8": (-2 & 0xffffffffffffffff), "r9": 0, "r10": 0, "r11": 0},
    entry=0x10)

test_pal_mc_drain = require_registers("pal_mc_drain",
    pal_call_program(PAL_MC_DRAIN),
    {"ip": 0x30, "r28": PAL_MC_DRAIN, "r8": 0, "r9": 0, "r10": 0},
    entry=0x10)

test_pal_mc_drain_reserved_arg = require_registers(
    "pal_mc_drain_reserved_arg",
    pal_call_program(PAL_MC_DRAIN, [(29, 1), (30, 0), (31, 0)]),
    {"ip": 0x60, "r28": PAL_MC_DRAIN,
     "r8": (-2 & 0xffffffffffffffff), "r9": 0, "r10": 0, "r11": 0},
    entry=0x10)

test_pal_mc_expected = require_registers("pal_mc_expected",
    pal_call_program(PAL_MC_EXPECTED, [(29, 1), (30, 0), (31, 0)]),
    {"ip": 0x60, "r28": PAL_MC_EXPECTED, "r8": 0,
     "r9": 0, "r10": 0, "r11": 0}, entry=0x10)

test_pal_mc_dynamic_state_empty = require_registers(
    "pal_mc_dynamic_state_empty",
    pal_call_program(PAL_MC_DYNAMIC_STATE, [(29, 0), (30, 0), (31, 0)]),
    {"ip": 0x60, "r28": PAL_MC_DYNAMIC_STATE, "r8": 0,
     "r9": 0, "r10": 0, "r11": 0}, entry=0x10)

test_pal_mc_dynamic_state_empty_aligned_offset = require_registers(
    "pal_mc_dynamic_state_empty_aligned_offset",
    pal_call_program(PAL_MC_DYNAMIC_STATE, [(29, 8), (30, 0), (31, 0)]),
    {"ip": 0x60, "r28": PAL_MC_DYNAMIC_STATE, "r8": 0,
     "r9": 0, "r10": 0, "r11": 0}, entry=0x10)

test_pal_mc_dynamic_state_bad_offset = require_registers(
    "pal_mc_dynamic_state_bad_offset",
    pal_call_program(PAL_MC_DYNAMIC_STATE, [(29, 4), (30, 0), (31, 0)]),
    {"ip": 0x60, "r28": PAL_MC_DYNAMIC_STATE,
     "r8": (-2 & 0xffffffffffffffff), "r9": 0, "r10": 0, "r11": 0},
    entry=0x10)

test_pal_mc_dynamic_state_reserved_arg = require_registers(
    "pal_mc_dynamic_state_reserved_arg",
    pal_call_program(PAL_MC_DYNAMIC_STATE, [(29, 0), (30, 0), (31, 1)]),
    {"ip": 0x60, "r28": PAL_MC_DYNAMIC_STATE,
     "r8": (-2 & 0xffffffffffffffff), "r9": 0, "r10": 0, "r11": 0},
    entry=0x10)

test_pal_mc_error_info_map_empty = require_registers(
    "pal_mc_error_info_map_empty",
    pal_call_program(PAL_MC_ERROR_INFO, [(29, 0), (30, 0), (31, 0)]),
    {"ip": 0x60, "r28": PAL_MC_ERROR_INFO,
     "r8": (-6 & 0xffffffffffffffff), "r9": 0, "r10": 0, "r11": 0},
    entry=0x10)

test_pal_mc_error_info_structure_empty = require_registers(
    "pal_mc_error_info_structure_empty",
    pal_call_program(PAL_MC_ERROR_INFO, [(29, 2), (30, 1 << 8), (31, 0)]),
    {"ip": 0x60, "r28": PAL_MC_ERROR_INFO,
     "r8": (-6 & 0xffffffffffffffff), "r9": 0, "r10": 0, "r11": 0},
    entry=0x10)

test_pal_mc_error_info_bad_index = require_registers(
    "pal_mc_error_info_bad_index",
    pal_call_program(PAL_MC_ERROR_INFO, [(29, 3), (30, 0), (31, 0)]),
    {"ip": 0x60, "r28": PAL_MC_ERROR_INFO,
     "r8": (-2 & 0xffffffffffffffff), "r9": 0, "r10": 0, "r11": 0},
    entry=0x10)

test_pal_mc_error_info_bad_level = require_registers(
    "pal_mc_error_info_bad_level",
    pal_call_program(PAL_MC_ERROR_INFO, [(29, 2), (30, 0x300), (31, 0)]),
    {"ip": 0x60, "r28": PAL_MC_ERROR_INFO,
     "r8": (-2 & 0xffffffffffffffff), "r9": 0, "r10": 0, "r11": 0},
    entry=0x10)

test_pal_mc_resume_no_context = require_registers(
    "pal_mc_resume_no_context",
    pal_call_program(PAL_MC_RESUME, [(29, 0), (30, 0x2000), (31, 0)]),
    {"ip": 0x60, "r28": PAL_MC_RESUME,
     "r8": (-3 & 0xffffffffffffffff), "r9": 0, "r10": 0, "r11": 0},
    entry=0x10)

# save_ptr has the alignment and size rules of the PAL_MC_REGISTER_MEM
# address (SDM Vol 2 PAL_MC_RESUME), which firmware passes with the
# uncacheable bit 63 set.
test_pal_mc_resume_uc_save_ptr_no_context = require_registers(
    "pal_mc_resume_uc_save_ptr_no_context",
    pal_call_program(PAL_MC_RESUME,
                     [(29, 0), (30, (1 << 63) | 0x2000), (31, 0)]),
    {"ip": 0x60, "r28": PAL_MC_RESUME,
     "r8": (-3 & 0xffffffffffffffff), "r9": 0, "r10": 0, "r11": 0},
    entry=0x10)

test_pal_mc_resume_new_context_no_context = require_registers(
    "pal_mc_resume_new_context_no_context",
    pal_call_program(PAL_MC_RESUME, [(29, 1), (30, 0x2000), (31, 1)]),
    {"ip": 0x60, "r28": PAL_MC_RESUME,
     "r8": (-3 & 0xffffffffffffffff), "r9": 0, "r10": 0, "r11": 0},
    entry=0x10)

test_pal_mc_resume_bad_args = require_registers(
    "pal_mc_resume_bad_args",
    pal_call_program(PAL_MC_RESUME, [(29, 2), (30, 0x2000), (31, 0)]),
    {"ip": 0x60, "r28": PAL_MC_RESUME,
     "r8": (-2 & 0xffffffffffffffff), "r9": 0, "r10": 0, "r11": 0},
    entry=0x10)

test_pal_mc_resume_bad_save_ptr = require_registers(
    "pal_mc_resume_bad_save_ptr",
    pal_call_program(PAL_MC_RESUME, [(29, 0), (30, 0x2100), (31, 0)]),
    {"ip": 0x60, "r28": PAL_MC_RESUME,
     "r8": (-2 & 0xffffffffffffffff), "r9": 0, "r10": 0, "r11": 0},
    entry=0x10)

test_pal_mc_register_mem = require_registers("pal_mc_register_mem",
    pal_call_program(PAL_MC_REGISTER_MEM, [(29, 0x2000), (30, 0), (31, 0)]),
    {"ip": 0x60, "r28": PAL_MC_REGISTER_MEM, "r8": 0,
     "r9": 0, "r10": 0, "r11": 0}, entry=0x10)

# The min-state save area must be uncacheable (SDM Vol.2 11.3.2.3), so
# firmware passes the address with bit 63 (the uncacheable attribute) set.
# That bit is masked, not rejected: a 512-byte-aligned address with bit 63
# set must succeed.  Real SDV firmware fatal-spins on the wrong -2 status.
test_pal_mc_register_mem_uncacheable = require_registers(
    "pal_mc_register_mem_uncacheable",
    pal_call_program(PAL_MC_REGISTER_MEM,
                     [(29, (1 << 63) | 0x3ff84000), (30, 0), (31, 0)]),
    {"ip": 0x60, "r28": PAL_MC_REGISTER_MEM, "r8": 0,
     "r9": 0, "r10": 0, "r11": 0}, entry=0x10)

test_pal_cache_line_init = require_registers("pal_cache_line_init",
    pal_call_program(PAL_CACHE_LINE_INIT,
                     [(29, 0x4000), (30, 0x1234), (31, 0)]),
    {"ip": 0x60, "r28": PAL_CACHE_LINE_INIT, "r8": 0,
     "r9": 0, "r10": 0, "r11": 0}, entry=0x10)

test_pal_pmi_entrypoint = require_registers("pal_pmi_entrypoint",
    pal_call_program(PAL_PMI_ENTRYPOINT, [(29, 0x5000), (30, 0), (31, 0)]),
    {"ip": 0x60, "r28": PAL_PMI_ENTRYPOINT, "r8": 0,
     "r9": 0, "r10": 0, "r11": 0}, entry=0x10)

test_pal_mem_for_test = require_registers("pal_mem_for_test",
    pal_call_program(PAL_MEM_FOR_TEST, [(29, 0), (30, 0), (31, 0)]),
    {"ip": 0x60, "r28": PAL_MEM_FOR_TEST, "r8": 0,
     "r9": 0, "r10": 1, "r11": 0}, entry=0x10)

test_pal_test_proc_healthy = require_registers("pal_test_proc_healthy",
    pal_stacked_call_program(PAL_TEST_PROC, [0x2000, 0, 1]),
    {"ip": 0x80, "r28": PAL_TEST_PROC, "r8": 0,
     "r9": PAL_SELF_TEST_STATE_TESTED, "r10": 0, "r11": 0}, entry=0x10)

test_pal_test_proc_missing_cacheable_attr = require_registers(
    "pal_test_proc_missing_cacheable_attr",
    pal_stacked_call_program(PAL_TEST_PROC, [0x2000, 0, 0]),
    {"ip": 0x80, "r28": PAL_TEST_PROC,
     "r8": (-2 & 0xffffffffffffffff), "r9": 0, "r10": 0, "r11": 0},
    entry=0x10)

test_pal_test_proc_bad_address = require_registers(
    "pal_test_proc_bad_address",
    pal_stacked_call_program(PAL_TEST_PROC, [1 << 63, 0, 1]),
    {"ip": 0x80, "r28": PAL_TEST_PROC,
     "r8": (-2 & 0xffffffffffffffff), "r9": 0, "r10": 0, "r11": 0},
    entry=0x10)

test_pal_test_proc_bad_attributes = require_registers(
    "pal_test_proc_bad_attributes",
    pal_stacked_call_program(PAL_TEST_PROC, [0x2000, 0, 1 << 16]),
    {"ip": 0x80, "r28": PAL_TEST_PROC,
     "r8": (-2 & 0xffffffffffffffff), "r9": 0, "r10": 0, "r11": 0},
    entry=0x10)

test_pal_unknown = require_registers("pal_unknown",
    pal_call_program(0xffff),
    {"ip": 0x30, "r28": 0xffff, "r8": (-1 & 0xffffffffffffffff)},
    entry=0x10)

GROUP = 'pal'
CASE_NAMES = (

    'pal_bus_get_features',
    'pal_bus_get_features_reserved_arg',
    'pal_bus_set_features',
    'pal_bus_set_features_invalid',
    'pal_brand_info_bus',
    'pal_brand_info_cache',
    'pal_brand_info_frequency',
    'pal_brand_info_string',
    'pal_cache_flush',
    'pal_cache_flush_bad_operation',
    'pal_cache_flush_bad_type',
    'pal_cache_flush_coherent_icache',
    'pal_cache_flush_invalidates_translated_target',
    'pal_cache_info',
    'pal_cache_info_invalid',
    'pal_cache_info_l0_data',
    'pal_cache_info_l1_data',
    'pal_cache_info_l1_instruction',
    'pal_cache_info_l2_unified',
    'pal_cache_info_l2_unified_bad_type',
    'pal_cache_init',
    'pal_cache_init_invalid',
    'pal_cache_line_init',
    'pal_cache_prot_info',
    'pal_cache_prot_info_invalid',
    'pal_cache_prot_info_unified_bad_type',
    'pal_cache_summary',
    'pal_cache_summary_madison',
    'pal_cache_summary_reserved_arg',
    'pal_cache_shared_info_single_thread',
    'pal_cache_shared_info_sibling_thread',
    'pal_copy_info',
    'pal_copy_info_bad_type',
    'pal_copy_info_ia32_unsupported',
    'pal_copy_info_platform_for_ia64',
    'pal_copy_pal_bad_alignment',
    'pal_copy_pal_bad_alloc',
    'pal_copy_pal_bad_processor',
    'pal_copy_pal_ap_entry_callable',
    'pal_copy_pal_entry_callable',
    'pal_copy_pal_relocated_entry_plain_branch',
    'pal_debug_info',
    'pal_debug_info_reserved_arg',
    'pal_fixed_addr',
    'pal_fixed_addr_geographic_id',
    'pal_fixed_addr_reserved_arg',
    'pal_firmware_register_accepts_record',
    'pal_firmware_register_rejects_short_record',
    'pal_firmware_register_rejects_unknown_record',
    'pal_freq_base',
    'pal_freq_base_reserved_arg',
    'pal_freq_ratios',
    'pal_call_clears_return_reg_nat',
    'pal_freq_ratios_madison',
    'pal_freq_ratios_merced',
    'pal_freq_base_merced',
    'pal_vm_summary_merced',
    'pal_cache_info_merced_l0_i',
    'pal_cache_info_merced_l0_d',
    'pal_cache_info_merced_l1_unified',
    'pal_cache_info_merced_l1_instruction_invalid',
    'pal_cache_info_merced_l2_unified',
    'pal_vm_info_merced_l0_instruction',
    'pal_vm_info_merced_l0_data',
    'pal_vm_info_merced_l1_data',
    'pal_vm_info_merced_l1_instruction_invalid',
    'pal_vm_page_size_merced',
    'pal_version_merced',
    'pal_vm_tr_read_merced_itr_bound',
    'pal_vm_tr_read_merced_dtr_bound',
    'pal_vm_tr_read_merced_dtr_limit',
    'pal_prefetch_vis_merced_unimplemented',
    'pal_cache_shared_info_merced_unimplemented',
    'pal_brand_info_merced_unimplemented',
    'pal_logical_to_physical_merced_unimplemented',
    'pal_freq_ratios_reserved_arg',
    'pal_halt_info',
    'pal_halt_info_bad_buffer',
    'pal_halt_info_reserved_arg',
    'pal_halt_invalid_state',
    'pal_halt_light_stops_at_pal_continuation',
    'pal_halt_light_stops_at_pal_continuation_icount',
    'pal_halt_light_wakes_on_due_itm',
    'pal_halt_reserved_arg',
    'pal_halt_wakes_on_due_itm',
    'pal_halt_wakes_on_due_itm_icount',
    'pal_logical_to_physical_current',
    'pal_logical_to_physical_multicore_thread',
    'pal_logical_to_physical_madison_unimplemented',
    'pal_mc_clear_log',
    'pal_mc_drain',
    'pal_mc_drain_reserved_arg',
    'pal_mc_dynamic_state_bad_offset',
    'pal_mc_dynamic_state_empty',
    'pal_mc_dynamic_state_empty_aligned_offset',
    'pal_mc_dynamic_state_reserved_arg',
    'pal_mc_error_info_bad_index',
    'pal_mc_error_info_bad_level',
    'pal_mc_error_info_map_empty',
    'pal_mc_error_info_structure_empty',
    'pal_mc_expected',
    'pal_mc_register_mem',
    'pal_mc_register_mem_uncacheable',
    'pal_mc_resume_bad_args',
    'pal_mc_resume_bad_save_ptr',
    'pal_mc_resume_new_context_no_context',
    'pal_mc_resume_no_context',
    'pal_mc_resume_uc_save_ptr_no_context',
    'pal_mem_attrib',
    'pal_mem_attrib_reserved_arg',
    'pal_mem_for_test',
    'pal_perf_mon_info',
    'pal_perf_mon_info_bad_buffer',
    'pal_perf_mon_info_reserved_arg',
    'pal_platform_addr_bad_type',
    'pal_platform_addr_ignores_bit63',
    'pal_platform_addr_interrupt',
    'pal_platform_addr_io',
    'pal_platform_addr_io_merced',
    'pal_platform_addr_io_merced_address',
    'pal_platform_addr_io_merced_rejects_itanium2',
    'pal_platform_addr_unmapped',
    'pal_pmi_entrypoint',
    'pal_prefetch_vis',
    'pal_prefetch_vis_reserved_arg',
    'pal_proc_entry_virtual_itr',
    'pal_impl_proc_response_timeout',
    'pal_impl_proc_response_timeout_merced',
    'pal_proc_get_features',
    'pal_proc_get_features_madison_beyond_max',
    'pal_proc_get_features_madison_set16',
    'pal_proc_get_features_merced_beyond_max',
    'pal_proc_get_features_montecito_beyond_max',
    'pal_proc_get_features_montecito_next_set',
    'pal_proc_get_features_montecito_set18',
    'pal_proc_get_features_reserved_arg',
    'pal_proc_set_features',
    'pal_proc_set_features_madison_set16',
    'pal_proc_set_features_merced_beyond_max',
    'pal_proc_set_features_invalid',
    'pal_ptce_info',
    'pal_ptce_info_reserved_arg',
    'pal_register_info_application_implemented',
    'pal_register_info_application_side_effects',
    'pal_register_info_control_implemented',
    'pal_register_info_control_side_effects',
    'pal_register_info_invalid_request',
    'pal_register_info_reserved_arg',
    'pal_rse_info',
    'pal_rse_info_reserved_arg',
    'pal_test_proc_bad_address',
    'pal_test_proc_bad_attributes',
    'pal_test_proc_healthy',
    'pal_test_proc_missing_cacheable_attr',
    'pal_unknown',
    'pal_version',
    'pal_version_reserved_arg',
    'pal_vm_info',
    'pal_vm_info_invalid',
    'pal_vm_info_l0_data',
    'pal_vm_info_l1_data',
    'pal_vm_info_l1_instruction',
    'pal_vm_info_l2_invalid',
    'pal_vm_page_size',
    'pal_vm_page_size_reserved_arg',
    'pal_vm_summary',
    'pal_vm_summary_reserved_arg',
    'pal_vm_tr_read_dtr',
    'pal_vm_tr_read_empty',
    'pal_vm_tr_read_invalid',
    'pal_vm_tr_read_max_dtr',
    'pal_vm_tr_read_misaligned_buffer',
    'pal_vm_tr_read_rejects_first_non_tr',
    'sale_entry_two_calls',
)

CASE_METADATA = {
}

CASE_ALIASES = {
}

CASES = bind_cases(GROUP, CASE_NAMES, globals(),
                   aliases=CASE_ALIASES,
                   metadata=CASE_METADATA)
