"""Interrupt, timer, and interruption-delivery microprograms."""

from __future__ import annotations

from .case import (CaseMetadata, CaseObservation, bind_cases)
from .encoding import (
    DTR_PTE_UC,
    DTR_PTE_WB,
    HIGH_TR_BASE,
    IA32_TEST_CSD,
    IA32_TEST_DSD,
    IA32_TEST_GDTD,
    IA64_ALT_DTLB_VECTOR,
    IA64_ALT_ITLB_VECTOR,
    IA64_BREAK_VECTOR,
    IA64_CR_ITM,
    IA64_CR_ITV,
    IA64_CR_SAPIC_EOI,
    IA64_CR_SAPIC_IRR3,
    IA64_CR_SAPIC_IVR,
    IA64_CR_SAPIC_TPR,
    IA64_DATA_NESTED_TLB_VECTOR,
    IA64_DCR_BE,
    IA64_DCR_LC,
    IA64_DCR_PP,
    IA64_DISABLED_FP_VECTOR,
    IA64_EXCP_BREAK,
    IA64_EXCP_DISABLED_ISA_TRANSITION,
    IA64_EXCP_ILLEGAL,
    IA64_EXCP_NONE,
    IA64_EXCP_RESERVED_TEMPLATE,
    IA64_EXCP_UNALIGNED,
    IA64_GENERAL_VECTOR,
    IA64_GENEX_UNIMPL_INST_ADDR,
    IA64_IMPL_PA_BITS,
    IA64_IA32_EXCEPTION_VECTOR,
    IA64_IA32_INTERCEPT_VECTOR,
    IA64_ISR_EI_SHIFT,
    IA64_ISR_CODE_SS,
    IA64_ISR_CODE_TB,
    IA64_ISR_CODE_UI,
    IA64_ISR_IR,
    IA64_ISR_NI,
    IA64_ISR_R,
    IA64_ISR_RS,
    IA64_ISR_X,
    IA64_ITC_TICKS_PER_MILLISECOND,
    IA64_LOWER_PRIV_TRANSFER_VECTOR,
    IA64_PSR_AC,
    IA64_PSR_BE,
    IA64_PSR_BN,
    IA64_PSR_CPL3,
    IA64_PSR_DB,
    IA64_PSR_DFH,
    IA64_PSR_DFL,
    IA64_PSR_DI,
    IA64_PSR_DT,
    IA64_PSR_I,
    IA64_PSR_IC,
    IA64_PSR_IS,
    IA64_PSR_IT,
    IA64_PSR_MC,
    IA64_PSR_MFH,
    IA64_PSR_MFL,
    IA64_PSR_PK,
    IA64_PSR_PP,
    IA64_PSR_RT,
    IA64_PSR_SI,
    IA64_PSR_SP,
    IA64_PSR_SS,
    IA64_PSR_TB,
    IA64_PSR_UP,
    IA64_TPR_MMI,
    IA64_TAKEN_BRANCH_VECTOR,
    IA64_SINGLE_STEP_VECTOR,
    IA64_UNALIGNED_VECTOR,
    IA64_VECTOR_MASKED,
    LOW_VECTOR_TR_PTE,
    add,
    addl,
    adds,
    alloc,
    br_call,
    br_cloop,
    br_cond,
    br_indirect,
    br_ret,
    break_f,
    break_m,
    break_x_mlx,
    bsw0,
    bsw1,
    cmp4_eq_imm,
    cmp4_eq_unc_imm,
    cmp4_ltu_imm,
    cmp_eq_and,
    cmp_ltu_unc,
    cover_b,
    dep,
    dtr_setup_bundles,
    extr_u,
    getf_sig,
    illegal_m,
    ia32_bundle,
    ia32_environment_bundles,
    itc_d,
    itr_i,
    ld2,
    ld2_bias,
    ld2_c_clr,
    ld2_sa,
    ld8,
    mov_ar,
    mov_ar_lc,
    mov_br_gr,
    mov_gr_psr_full,
    mov_lc_gr,
    mov_m_ar_gr,
    mov_m_cr_gr,
    mov_m_gr_ar,
    mov_m_gr_cr,
    mov_m_psr_gr,
    mov_dbr_indexed_write,
    mov_ibr_indexed_write,
    mov_pkr_indexed,
    mov_pkr_indexed_read,
    movl_mlx,
    nop_b,
    nop_i,
    nop_m,
    pal_break,
    raw_bundle,
    require_exception,
    require_registers,
    rfi_b,
    rfi_to_gr,
    rsm,
    run_program,
    srlz_d,
    srlz_i,
    ssm,
    setf_sig,
    st1_postinc,
    st2,
    st8,
    st8_postinc,
)

FOUR_K_ITIR = 12 << 2

# addl has a signed 22-bit immediate.  Five model milliseconds stay within that
# range for our 200 ktick/ms model ITC while leaving enough setup margin for a
# test to reach a steady translated loop before the interrupt fires.
IA64_ITC_ADDL_DELAY_TICKS = 5 * IA64_ITC_TICKS_PER_MILLISECOND

test_rfi_target_rse_fill_fault_uses_restored_psr = require_registers(
    "rfi_target_rse_fill_fault_uses_restored_psr", [
        (0x10, *movl_mlx(3, HIGH_TR_BASE + 0x10000)),
        (0x20, 0x00, mov_ar(3, 18), nop_i(), nop_i()),
        (0x30, *movl_mlx(20, (1 << 63) | 1)),
        (0x40, 0x00, mov_m_gr_cr(20, 23), nop_i(), nop_i()),
        (0x50, *movl_mlx(20, 0x200)),
        (0x60, 0x00, mov_m_gr_cr(20, 19), nop_i(), nop_i()),
        (0x70, *movl_mlx(20, IA64_PSR_IC | IA64_PSR_DT | IA64_PSR_RT)),
        (0x80, 0x00, mov_m_gr_cr(20, 16), nop_i(), nop_i()),
        (0x90, 0x10, nop_m(), nop_i(), rfi_b()),
        (IA64_ALT_DTLB_VECTOR, 0x00, mov_m_cr_gr(31, 17), nop_i(), nop_i()),
        (IA64_ALT_DTLB_VECTOR + 0x10, 0x00, mov_m_cr_gr(30, 20),
         nop_i(), nop_i()),
        (IA64_ALT_DTLB_VECTOR + 0x20, 0x10, nop_m(), nop_i(),
         br_cond(IA64_ALT_DTLB_VECTOR + 0x20,
                 IA64_ALT_DTLB_VECTOR + 0x20)),
        (IA64_DATA_NESTED_TLB_VECTOR, 0x10, nop_m(), adds(29, 1, 0),
         br_cond(IA64_DATA_NESTED_TLB_VECTOR,
                 IA64_DATA_NESTED_TLB_VECTOR)),
    ], {
        "ip": IA64_ALT_DTLB_VECTOR + 0x20,
        "exception": IA64_EXCP_NONE,
        "r29": 0,
        "r30": HIGH_TR_BASE + 0xfff8,
        "r31": IA64_ISR_R | IA64_ISR_RS | IA64_ISR_IR,
    }, entry=0x10)

test_rfi_retries_interrupted_current_frame_fill = require_registers(
    "rfi_retries_interrupted_current_frame_fill", [
        (0x10, *movl_mlx(3, 0x4000fe8)),
        (0x20, *movl_mlx(4, 0x1122334455667788)),
        (0x30, 0x00, st8(3, 4), nop_i(), nop_i()),
        (0x40, *movl_mlx(3, 0x4000ff0)),
        (0x50, *movl_mlx(4, 0x8877665544332211)),
        (0x60, 0x00, st8(3, 4), nop_i(), nop_i()),
        (0x70, *movl_mlx(3, 0x4000ff8)),
        (0x80, 0x00, st8(3, 0), nop_i(), nop_i()),
        (0x90, *movl_mlx(3, HIGH_TR_BASE + 0x10000)),
        (0xa0, 0x00, mov_ar(3, 18), nop_i(), nop_i()),
        (0xb0, *movl_mlx(20, (1 << 63) | 2)),
        (0xc0, 0x00, mov_m_gr_cr(20, 23), nop_i(), nop_i()),
        (0xd0, *movl_mlx(20, 0x200)),
        (0xe0, 0x00, mov_m_gr_cr(20, 19), nop_i(), nop_i()),
        (0xf0, *movl_mlx(20, IA64_PSR_IC | IA64_PSR_DT | IA64_PSR_RT)),
        (0x100, 0x00, mov_m_gr_cr(20, 16), nop_i(), nop_i()),
        (0x110, 0x10, nop_m(), nop_i(), rfi_b()),
        (0x200, 0x00, nop_m(), adds(8, 0, 32), nop_i()),
        (0x210, 0x00, nop_m(), adds(9, 0, 33), nop_i()),
        (0x220, 0x10, nop_m(), nop_i(), br_cond(0x220, 0x220)),
        (IA64_ALT_DTLB_VECTOR, *movl_mlx(18, LOW_VECTOR_TR_PTE)),
        (IA64_ALT_DTLB_VECTOR + 0x10, 0x00, nop_m(),
         adds(7, FOUR_K_ITIR, 0), nop_i()),
        (IA64_ALT_DTLB_VECTOR + 0x20, 0x00, mov_m_gr_cr(7, 21),
         nop_i(), nop_i()),
        (IA64_ALT_DTLB_VECTOR + 0x30, 0x00, itc_d(18), nop_i(), nop_i()),
        (IA64_ALT_DTLB_VECTOR + 0x40, 0x10, nop_m(), nop_i(), rfi_b()),
    ], {
        "ip": 0x220,
        "exception": IA64_EXCP_NONE,
        "r8": 0x1122334455667788,
        "r9": 0x8877665544332211,
        "cfm_sof": 2,
        "cfm_sol": 0,
    }, entry=0x10)

test_unimplemented_physical_instruction_traps = require_registers(
    "unimplemented_physical_instruction_traps", [
        (0x10, *movl_mlx(3, 1 << IA64_IMPL_PA_BITS)),
        (0x20, *movl_mlx(19, IA64_PSR_IC)),
        (0x30, 0x00, mov_gr_psr_full(19), nop_i(), nop_i()),
        (0x40, 0x00, srlz_d(), nop_i(), nop_i()),
        (0x50, 0x00, nop_m(), mov_br_gr(7, 3), nop_i()),
        (0x60, 0x11, nop_m(), nop_i(), br_indirect(7)),
        (IA64_LOWER_PRIV_TRANSFER_VECTOR, 0x00,
         mov_m_cr_gr(8, 19), nop_i(), nop_i()),
        (IA64_LOWER_PRIV_TRANSFER_VECTOR + 0x10, 0x00,
         mov_m_cr_gr(9, 17), nop_i(), nop_i()),
        (IA64_LOWER_PRIV_TRANSFER_VECTOR + 0x20, 0x10,
         nop_m(), nop_i(),
         br_cond(IA64_LOWER_PRIV_TRANSFER_VECTOR + 0x20,
                 IA64_LOWER_PRIV_TRANSFER_VECTOR + 0x20)),
    ], {
        "ip": IA64_LOWER_PRIV_TRANSFER_VECTOR + 0x20,
        "exception": IA64_EXCP_NONE,
        "r8": 0,
        "r9": IA64_GENEX_UNIMPL_INST_ADDR | IA64_ISR_X,
    }, entry=0x10)

def test_ar_itc_advances_in_guest_loop(qemu):
    result = run_program(qemu, [
        (0x10, 0x02, mov_m_ar_gr(16, 44), nop_i(),
         addl(8, 4095, 0)),
        (0x20, 0x02, nop_m(), mov_lc_gr(8),
         nop_i()),
        (0x30, 0x10, nop_m(), nop_i(),
         br_cloop(0x30, 0x30)),
        (0x40, 0x02, mov_m_ar_gr(17, 44), nop_i(),
         nop_i()),
        (0x50, 0x10, nop_m(), nop_i(),
         br_cond(0x50, 0x50)),
    ], entry=0x10, terminal_ip=0x50)
    state = result.state
    if state.gr[17] <= state.gr[16]:
        raise RuntimeError(
            "ar_itc_advances_in_guest_loop failed: "
            f"r16={state.gr[16]!r} r17={state.gr[17]!r}\n"
            f"{result.register_output}")

def test_cloop_zero_st1_timer_interrupts_batched_loop(qemu):
    result = run_program(qemu, [
        (0x10, *movl_mlx(2, 0x8000)),
        (0x20, *movl_mlx(8, 0x100000000)),
        (0x30, 0x02, nop_m(), mov_lc_gr(8),
         nop_i()),
        (0x40, 0x00, adds(3, 0xef, 0), nop_i(),
         nop_i()),
        (0x50, 0x00, mov_m_gr_cr(3, IA64_CR_ITV), nop_i(),
         nop_i()),
        # HAL-style arm (WSRV03 halia64 i64itm.s retry_itm_read): re-read
        # ITC after the ITM write and retry while the deadline is no
        # longer in the future.
        (0x60, 0x02, mov_m_ar_gr(3, 44), nop_i(),
         nop_i()),
        (0x70, 0x00, addl(4, 10 * IA64_ITC_TICKS_PER_MILLISECOND, 3),
         nop_i(), nop_i()),
        (0x80, 0x00, mov_m_gr_cr(4, IA64_CR_ITM), nop_i(),
         nop_i()),
        (0x90, 0x02, mov_m_ar_gr(3, 44), nop_i(),
         nop_i()),
        (0xa0, 0x01, nop_m(), cmp_ltu_unc(6, 7, 3, 4),
         nop_i()),
        (0xb0, 0x10, nop_m(), nop_i(),
         br_cond(0xb0, 0x60, qp=7)),
        (0xc0, *movl_mlx(19, (1 << 13) | (1 << 14))),
        (0xd0, 0x08, mov_gr_psr_full(19), srlz_d(),
         nop_i()),
        (0xe0, 0x10, st1_postinc(2, 0, 1), nop_i(),
         br_cloop(0xe0, 0xe0)),
        (0xf0, 0x10, nop_m(), nop_i(),
         br_cond(0xf0, 0xf0)),
        (0x3000, 0x02, nop_m(), mov_ar_lc(9),
         nop_i()),
        (0x3010, 0x00, nop_m(), adds(8, 0, 2),
         nop_i()),
        (0x3020, 0x10, nop_m(), nop_i(),
         br_cond(0x3020, 0x3020)),
    ], entry=0x10, terminal_ip=0x3020, timeout=8.0)
    state = result.state
    advanced = state.gr[8] - 0x8000
    if (state.ip != 0x3020 or
        state.exception != IA64_EXCP_NONE or
        advanced <= 0 or
        state.gr[9] >= 0x100000000):
        raise RuntimeError(
            "cloop_zero_st1_timer_interrupts_batched_loop failed: "
            f"advanced={advanced!r} lc={state.gr[9]!r} "
            f"ip={state.ip!r} exception={state.exception!r}\n"
            f"{result.register_output}")

test_mov_to_ivr_illegal = require_exception(
    "mov_to_ivr_illegal", [
        (0x10, 0x00, mov_m_gr_cr(0, IA64_CR_SAPIC_IVR), nop_i(),
         nop_i()),
    ], IA64_EXCP_ILLEGAL, fault_ip=0x10)

test_mov_to_irr_illegal = require_exception(
    "mov_to_irr_illegal", [
        (0x10, 0x00, mov_m_gr_cr(0, IA64_CR_SAPIC_IRR3), nop_i(),
         nop_i()),
    ], IA64_EXCP_ILLEGAL, fault_ip=0x10)

test_mov_to_read_only_cr_predicate_false = require_registers(
    "mov_to_read_only_cr_predicate_false", [
        (0x10, 0x00, mov_m_gr_cr(0, IA64_CR_SAPIC_IVR, qp=1),
         nop_i(), nop_i()),
        (0x20, 0x00, mov_m_gr_cr(0, IA64_CR_SAPIC_IRR3, qp=1),
         nop_i(), nop_i()),
        (0x30, 0x10, nop_m(), nop_i(),
         br_cond(0x30, 0x30)),
    ], {
        "ip": 0x30,
        "exception": IA64_EXCP_NONE,
    }, entry=0x10)

test_async_timer_interrupt_enters_ivt = require_registers(
    "async_timer_interrupt_enters_ivt", [
        (0x10, 0x00, adds(3, 0xef, 0), nop_i(),
         nop_i()),
        (0x20, 0x00, mov_m_gr_cr(3, IA64_CR_ITV), nop_i(),
         nop_i()),
        (0x30, 0x00, mov_m_gr_ar(0, 44), nop_i(),
         nop_i()),
        (0x40, 0x00, mov_m_gr_cr(0, IA64_CR_ITM), nop_i(),
         nop_i()),
        (0x50, *movl_mlx(19, (1 << 13) | (1 << 14))),
        (0x60, 0x10, mov_gr_psr_full(19), nop_i(),
         br_cond(0x60, 0x60)),
        (0x3000, 0x10, nop_m(), adds(31, 0x55, 0),
         br_cond(0x3000, 0x3010)),
        (0x3010, 0x10, nop_m(), nop_i(),
         br_cond(0x3010, 0x3010)),
    ], {
        "ip": 0x3010,
        "exception": IA64_EXCP_NONE,
        "r31": 0x55,
    }, entry=0x10)

test_async_timer_interrupt_records_boundary_ri = require_registers(
    "async_timer_interrupt_records_boundary_ri", [
        (0x10, 0x00, adds(3, 0xef, 0), nop_i(),
         nop_i()),
        (0x20, 0x00, mov_m_gr_cr(3, IA64_CR_ITV), nop_i(),
         nop_i()),
        (0x30, 0x00, mov_m_gr_ar(0, 44), nop_i(),
         nop_i()),
        (0x40, 0x00, mov_m_gr_cr(0, IA64_CR_ITM), nop_i(),
         nop_i()),
        (0x50, *movl_mlx(19, (1 << 13) | (1 << 14))),
        (0x60, 0x10, mov_gr_psr_full(19), nop_i(),
         br_cond(0x60, 0x60)),
        (0x3000, 0x00, mov_m_cr_gr(31, 16), nop_i(),
         nop_i()),
        (0x3010, 0x10, mov_m_cr_gr(30, 19), nop_i(),
         br_cond(0x3010, 0x3020)),
        (0x3020, 0x10, nop_m(), nop_i(),
         br_cond(0x3020, 0x3020)),
    ], {
        "ip": 0x3020,
        "exception": IA64_EXCP_NONE,
        "r30": 0x60,
        "r31": (1 << 13) | (1 << 14),
    }, entry=0x10)

def test_async_timer_interrupt_never_resumes_mlx_slot2(qemu):
    program = [
        (0x10, 0x00, adds(3, 0xef, 0), nop_i(), nop_i()),
        (0x20, 0x00, mov_m_gr_cr(3, IA64_CR_ITV), nop_i(), nop_i()),
        # HAL-style arm (WSRV03 halia64 i64itm.s retry_itm_read): re-read
        # ITC after the ITM write and retry while the deadline is no
        # longer in the future.
        (0x30, 0x02, mov_m_ar_gr(3, 44), nop_i(), nop_i()),
        (0x40, 0x00,
         addl(4, 10 * IA64_ITC_TICKS_PER_MILLISECOND, 3),
         nop_i(), nop_i()),
        (0x50, 0x00, mov_m_gr_cr(4, IA64_CR_ITM), nop_i(), nop_i()),
        (0x60, 0x02, mov_m_ar_gr(3, 44), nop_i(), nop_i()),
        (0x70, 0x01, nop_m(), cmp_ltu_unc(6, 7, 3, 4), nop_i()),
        (0x80, 0x10, nop_m(), nop_i(), br_cond(0x80, 0x30, qp=7)),
        (0x90, *movl_mlx(19, (1 << 13) | (1 << 14))),
        (0xa0, 0x08, mov_gr_psr_full(19), srlz_d(), nop_i()),
    ]
    mlx_addresses = set()
    loop_addresses = set()
    loop_start = 0xb0
    address = loop_start

    # Alternate fully occupied bundles with MLX bundles.  A host timer kick
    # at a bundle boundary must never combine the following MLX address with
    # the completed preceding bundle's slot-2 restart state.
    for index in range(32):
        loop_addresses.add(address)
        program.append((address, 0x01,
                        adds(22, index, 0),
                        adds(23, index + 1, 0),
                        adds(24, index + 2, 0)))
        address += 0x10
        mlx_addresses.add(address)
        loop_addresses.add(address)
        program.append((address, *movl_mlx(25, index)))
        address += 0x10

    loop_addresses.add(address)
    program.extend([
        (address, 0x10, nop_m(), nop_i(),
         br_cond(address, loop_start)),
        (0x3000, 0x00, mov_m_cr_gr(30, 19), nop_i(), nop_i()),
        (0x3010, 0x10, mov_m_cr_gr(31, 16), nop_i(),
         br_cond(0x3010, 0x3010)),
    ])

    result = run_program(qemu, program, entry=0x10, terminal_ip=0x3010,
                         timeout=8.0)
    state = result.state
    interrupted_ip = state.gr[30]
    interrupted_ri = (state.gr[31] >> 41) & 3
    if (state.ip != 0x3010 or
        state.exception != IA64_EXCP_NONE or
        interrupted_ip not in loop_addresses or
        (interrupted_ip in mlx_addresses and interrupted_ri not in (0, 1))):
        raise RuntimeError(
            "async_timer_interrupt_never_resumes_mlx_slot2 failed: "
            f"iip={interrupted_ip:#x} ri={interrupted_ri} "
            f"ip={state.ip!r} exception={state.exception!r}\n"
            f"{result.register_output}")


def test_repeated_timer_rfi_preserves_word_rmw(qemu):
    iterations = 0x100001
    result = run_program(qemu, [
        (0x10, *movl_mlx(2, 0x8000)),
        (0x20, 0x00, st2(2, 0), nop_i(), nop_i()),
        (0x30, *movl_mlx(3, 0x8010)),
        (0x40, 0x00, st8(3, 0), nop_i(), nop_i()),
        # HAL-style arm (WSRV03 halia64 i64itm.s retry_itm_read): restart
        # from the ITC rebase if the deadline stopped being in the future
        # during a host stall -- a missed ITC==ITM equality never fires.
        (0x50, 0x00, mov_m_gr_ar(0, 44), nop_i(), nop_i()),
        (0x60, *movl_mlx(4, IA64_ITC_TICKS_PER_MILLISECOND)),
        (0x70, 0x00, mov_m_gr_cr(4, IA64_CR_ITM), nop_i(), nop_i()),
        (0x80, 0x00, adds(9, 0xef, 0), nop_i(), nop_i()),
        (0x90, 0x00, mov_m_gr_cr(9, IA64_CR_ITV), nop_i(), nop_i()),
        (0xa0, 0x02, mov_m_ar_gr(10, 44), nop_i(), nop_i()),
        (0xb0, 0x01, nop_m(), cmp_ltu_unc(6, 7, 10, 4), nop_i()),
        (0xc0, 0x10, nop_m(), nop_i(), br_cond(0xc0, 0x50, qp=7)),
        (0xd0, *movl_mlx(8, iterations - 1)),
        (0xe0, 0x02, nop_m(), mov_lc_gr(8), nop_i()),
        (0xf0, *movl_mlx(19, (1 << 13) | (1 << 14) | (1 << 44))),
        (0x100, 0x08, mov_gr_psr_full(19), srlz_d(), nop_i()),
        (0x110, 0x00, ld2(4, 2), nop_i(), nop_i()),
        (0x120, 0x00, nop_m(), adds(4, 1, 4), nop_i()),
        (0x130, 0x10, st2(2, 4), nop_i(), br_cloop(0x130, 0x110)),
        # The counted loop is a wall-clock deadline the iothread-serviced
        # ITM cannot promise to meet under host load; hold PSR.i until the
        # two required handler runs have actually happened.
        (0x140, 0x00, ld8(10, 3), nop_i(), nop_i()),
        (0x150, 0x01, nop_m(), cmp4_ltu_imm(6, 7, 1, 10), nop_i()),
        (0x160, 0x10, nop_m(), nop_i(), br_cond(0x160, 0x140, qp=7)),
        (0x170, 0x00, rsm(1 << 14), nop_i(), nop_i()),
        (0x180, 0x00, ld2(8, 2), nop_i(), nop_i()),
        (0x190, 0x00, ld8(9, 3), nop_i(), nop_i()),
        (0x1a0, 0x10, nop_m(), nop_i(), br_cond(0x1a0, 0x1a0)),

        (0x3000, 0x00, mov_m_cr_gr(16, IA64_CR_SAPIC_IVR),
         nop_i(), nop_i()),
        # The second required interrupt exists only if this re-arm lands in
        # the future too; retry like the initial arm.  p8/p9 and r21 stay
        # clear of the interrupted flow's p6/p7 spin.
        (0x3010, 0x00, mov_m_ar_gr(17, 44), nop_i(), nop_i()),
        (0x3020, *movl_mlx(20, IA64_ITC_TICKS_PER_MILLISECOND)),
        (0x3030, 0x00, nop_m(), add(17, 17, 20), nop_i()),
        (0x3040, 0x00, mov_m_gr_cr(17, IA64_CR_ITM), nop_i(), nop_i()),
        (0x3050, 0x02, mov_m_ar_gr(21, 44), nop_i(), nop_i()),
        (0x3060, 0x01, nop_m(), cmp_ltu_unc(8, 9, 21, 17), nop_i()),
        (0x3070, 0x10, nop_m(), nop_i(), br_cond(0x3070, 0x3010, qp=9)),
        (0x3080, *movl_mlx(18, 0x8010)),
        (0x3090, 0x00, ld8(19, 18), nop_i(), nop_i()),
        (0x30a0, 0x00, nop_m(), adds(19, 1, 19), nop_i()),
        (0x30b0, 0x00, st8(18, 19), nop_i(), nop_i()),
        (0x30c0, 0x10, mov_m_gr_cr(0, IA64_CR_SAPIC_EOI), nop_i(),
         rfi_b()),
    ], entry=0x10, terminal_ip=0x1a0, timeout=8.0)
    state = result.state
    expected = iterations & 0xffff
    if (state.ip != 0x1a0 or
        state.exception != IA64_EXCP_NONE or
        state.gr[8] != expected or
        state.gr[9] < 2):
        raise RuntimeError(
            "repeated_timer_rfi_preserves_word_rmw failed: "
            f"counter={state.gr[8]!r} expected={expected!r} "
            f"interrupts={state.gr[9]!r} ip={state.ip!r} "
            f"exception={state.exception!r}\n{result.register_output}")


def test_timer_interrupt_preserves_banked_word_rmw(qemu):
    iterations = 0x100001
    result = run_program(qemu, [
        (0x10, *movl_mlx(2, 0x8000)),
        (0x20, 0x00, st2(2, 0), nop_i(), nop_i()),
        (0x30, *movl_mlx(3, 0x8010)),
        (0x40, 0x00, st8(3, 0), nop_i(), nop_i()),
        (0x50, 0x00, adds(4, 0xef, 0), nop_i(), nop_i()),
        (0x60, 0x00, mov_m_gr_cr(4, IA64_CR_ITV), nop_i(), nop_i()),
        (0x70, *movl_mlx(5, IA64_ITC_TICKS_PER_MILLISECOND)),
        # Arm the way the HAL does (WSRV03 halia64 i64itm.s retry_itm_read):
        # re-read ITC after the ITM write and retry while the deadline is no
        # longer in the future.  The ITC==ITM equality is an edge; a value
        # that went stale during a host stall would otherwise never fire.
        (0x80, 0x02, mov_m_ar_gr(4, 44), nop_i(), nop_i()),
        (0x90, 0x00, nop_m(), add(4, 4, 5), nop_i()),
        (0xa0, 0x00, mov_m_gr_cr(4, IA64_CR_ITM), nop_i(), nop_i()),
        (0xb0, 0x02, mov_m_ar_gr(6, 44), nop_i(), nop_i()),
        (0xc0, 0x01, nop_m(), cmp_ltu_unc(6, 7, 6, 4), nop_i()),
        (0xd0, 0x10, nop_m(), nop_i(), br_cond(0xd0, 0x80, qp=7)),
        (0xe0, *movl_mlx(8, iterations - 1)),
        (0xf0, 0x02, nop_m(), mov_lc_gr(8), nop_i()),
        (0x100, *movl_mlx(7, IA64_PSR_IC | IA64_PSR_I | IA64_PSR_BN)),
        (0x110, 0x10, nop_m(), nop_i(), bsw1()),
        (0x120, 0x08, mov_gr_psr_full(7), srlz_d(), nop_i()),
        (0x130, 0x10, nop_m(), nop_i(), br_cond(0x130, 0x1fe0)),
        (0x1fe0, 0x00, nop_m(), adds(22, 0, 2), nop_i()),
        # Match the page-table accounting sequence: a banked address and
        # loaded value survive an interruptible host-TB boundary before the
        # 16-bit store.  Splitting the two bundles across an 8 KiB page makes
        # that boundary deterministic; the handler clobbers the other bank.
        (0x1ff0, 0x08, ld2_bias(21, 22), adds(19, 1, 21), nop_i()),
        (0x2000, 0x10, st2(22, 19), nop_i(),
         br_cloop(0x2000, 0x1ff0)),
        # The counted loop is a wall-clock deadline the iothread-serviced
        # ITM cannot promise to meet under host load; hold PSR.i until the
        # handler has actually run once.
        (0x2010, 0x00, ld8(10, 3), nop_i(), nop_i()),
        (0x2020, 0x01, nop_m(), cmp4_eq_imm(6, 7, 0, 10), nop_i()),
        (0x2030, 0x10, nop_m(), nop_i(), br_cond(0x2030, 0x2010, qp=6)),
        (0x2040, 0x00, rsm(IA64_PSR_I), nop_i(), nop_i()),
        (0x2050, 0x00, ld2(8, 2), nop_i(), nop_i()),
        (0x2060, 0x00, ld8(9, 3), nop_i(), nop_i()),
        (0x2070, 0x10, nop_m(), nop_i(), br_cond(0x2070, 0x2070)),

        (0x3000, 0x00, mov_m_cr_gr(16, IA64_CR_SAPIC_IVR),
         nop_i(), nop_i()),
        (0x3010, 0x02, mov_m_ar_gr(17, 44), nop_i(), nop_i()),
        (0x3020, *movl_mlx(20, IA64_ITC_TICKS_PER_MILLISECOND)),
        (0x3030, 0x00, nop_m(), add(17, 17, 20), nop_i()),
        (0x3040, 0x00, mov_m_gr_cr(17, IA64_CR_ITM), nop_i(), nop_i()),
        (0x3050, *movl_mlx(18, 0x8010)),
        (0x3060, 0x00, ld8(19, 18), nop_i(), nop_i()),
        (0x3070, 0x00, nop_m(), adds(19, 1, 19), nop_i()),
        (0x3080, 0x00, st8(18, 19), adds(21, 0x55, 0),
         adds(22, 0x66, 0)),
        (0x3090, 0x10, mov_m_gr_cr(0, IA64_CR_SAPIC_EOI), nop_i(),
         rfi_b()),
    ], entry=0x10, terminal_ip=0x2070, timeout=8.0)
    state = result.state
    expected = iterations & 0xffff
    if (state.ip != 0x2070 or
        state.exception != IA64_EXCP_NONE or
        state.psr != IA64_PSR_IC | IA64_PSR_BN or
        state.gr[8] != expected or
        state.gr[9] < 1):
        raise RuntimeError(
            "timer_interrupt_preserves_banked_word_rmw failed: "
            f"counter={state.gr[8]!r} expected={expected!r} "
            f"interrupts={state.gr[9]!r} ip={state.ip!r} "
            f"exception={state.exception!r}\n{result.register_output}")


def test_timer_cover_rfi_preserves_large_frame_halfword_rmw(qemu):
    result = run_program(qemu, [
        # Place a 73-register frame beside an RNAT collection boundary.
        # Derive r91 from a movl temporary exactly as the page-table mapping
        # helper derives its PFN-database bias.  Recompute r44 from that high
        # local for every interrupted RMW so an RSE restore cannot silently
        # preserve the cached address while corrupting the bias itself.
        (0x10, *movl_mlx(2, 0x1001e8)),
        (0x20, 0x00, mov_ar(2, 18), nop_i(), nop_i()),
        (0x30, 0x00, nop_m(), alloc(100, 73, 70, 0, 0), nop_i()),
        (0x40, *movl_mlx(80, 0x7ff8)),
        (0x50, 0x01, nop_m(), adds(91, 8, 80), nop_i()),
        (0x60, 0x00, st2(91, 0), nop_i(), nop_i()),
        (0x70, *movl_mlx(13, 0x8010)),
        (0x80, 0x00, st8(13, 0), nop_i(), nop_i()),
        (0x90, *movl_mlx(5, 0x8020)),
        (0xa0, 0x00, st8(5, 0), nop_i(), nop_i()),
        (0xb0, 0x00, adds(3, 0xef, 0), nop_i(), nop_i()),
        (0xc0, 0x00, mov_m_gr_cr(3, IA64_CR_ITV), nop_i(), nop_i()),
        # HAL-style arm (WSRV03 halia64 i64itm.s retry_itm_read): re-read
        # ITC after the ITM write and retry while the deadline is no longer
        # in the future -- a missed ITC==ITM equality never fires.
        (0xd0, 0x02, mov_m_ar_gr(3, 44), nop_i(), nop_i()),
        (0xe0, 0x00,
         addl(4, 10 * IA64_ITC_TICKS_PER_MILLISECOND, 3),
         nop_i(), nop_i()),
        (0xf0, 0x00, mov_m_gr_cr(4, IA64_CR_ITM), nop_i(), nop_i()),
        (0x100, 0x02, mov_m_ar_gr(3, 44), nop_i(), nop_i()),
        (0x110, 0x01, nop_m(), cmp_ltu_unc(6, 7, 3, 4), nop_i()),
        (0x120, 0x10, nop_m(), nop_i(), br_cond(0x120, 0xd0, qp=7)),
        (0x130, *movl_mlx(7, IA64_PSR_IC | IA64_PSR_I | IA64_PSR_BN)),
        (0x140, 0x10, nop_m(), nop_i(), bsw1()),
        (0x150, 0x08, mov_gr_psr_full(7), srlz_d(), nop_i()),
        (0x160, 0x00, nop_m(), adds(47, 0, 0), nop_i()),
        (0x170, 0x10, nop_m(), nop_i(), br_cond(0x170, 0x1fb0)),

        # Match the non-atomic page-table entry accounting sequence.  The
        # unfinished instruction group crosses an 8 KiB boundary between
        # the checked load/add and store, making an interrupt there
        # deterministic once the timer is due.
        (0x1fb0, 0x01, nop_m(), adds(44, 0, 91), nop_i()),
        (0x1fc0, 0x03, ld2_sa(29, 44), nop_i(), nop_i()),
        (0x1fd0, 0x01, nop_m(), nop_i(), nop_i()),
        (0x1fe0, 0x01, nop_m(), nop_i(), nop_i()),
        (0x1ff0, 0x00, ld2_c_clr(29, 44), adds(31, 1, 29),
         addl(52, 2, 0)),
        (0x2000, 0x0b, adds(47, 1, 47), st2(44, 31), nop_i()),
        (0x2010, 0x00, ld8(9, 13), nop_i(), nop_i()),
        (0x2020, 0x01, nop_m(), cmp4_eq_imm(6, 7, 0, 9), nop_i()),
        (0x2030, 0x10, nop_m(), nop_i(),
         br_cond(0x2030, 0x1fb0, qp=6)),
        (0x2040, 0x00, rsm(IA64_PSR_I), nop_i(), nop_i()),
        (0x2050, 0x00, ld2(8, 44), adds(10, 0, 44),
         adds(11, 0, 47)),
        (0x2060, 0x00, ld8(12, 5), adds(14, 0, 91), nop_i()),
        (0x2070, 0x10, nop_m(), nop_i(), br_cond(0x2070, 0x2070)),

        # A realistic low-level handler covers the interrupted frame, calls
        # code with a full-size nested frame (forcing RSE spill/reload),
        # unwinds to the vector's zero frame, and only then executes rfi.
        (0x3000, 0x18, nop_m(), nop_m(), cover_b()),
        (0x3010, 0x00, mov_m_ar_gr(20, 64), nop_i(), nop_i()),
        (0x3020, 0x10, nop_m(), nop_i(),
         br_call(0, 0x3020, 0x4000)),
        (0x3030, 0x00, mov_m_gr_ar(20, 64), nop_i(), nop_i()),
        (0x3040, 0x10, mov_m_gr_cr(0, IA64_CR_SAPIC_EOI), nop_i(),
         rfi_b()),

        (0x4000, 0x00, nop_m(), alloc(36, 96, 88, 0, 0), nop_i()),
        (0x4010, 0x00, mov_m_cr_gr(16, IA64_CR_SAPIC_IVR), nop_i(),
         nop_i()),
        (0x4020, 0x00, mov_m_cr_gr(21, 19), nop_i(), nop_i()),
        (0x4030, *movl_mlx(23, 0x2000)),
        (0x4040, 0x01, nop_m(), cmp4_eq_unc_imm(6, 7, 0, 0),
         nop_i()),
        (0x4050, 0x01, nop_m(), cmp_eq_and(6, 7, 23, 21), nop_i()),
        (0x4060, 0x02, mov_m_ar_gr(17, 44), nop_i(), nop_i()),
        (0x4070, *movl_mlx(24, 10 * IA64_ITC_TICKS_PER_MILLISECOND)),
        (0x4080, 0x00, nop_m(), add(17, 17, 24), nop_i()),
        (0x4090, 0x00, mov_m_gr_cr(17, IA64_CR_ITM), nop_i(), nop_i()),
        (0x40a0, *movl_mlx(22, 0x8020)),
        (0x40b0, 0x00, st8(22, 21, qp=6), nop_i(), nop_i()),
        (0x40c0, *movl_mlx(18, 0x8010)),
        (0x40d0, 0x00, ld8(19, 18, qp=6), nop_i(), nop_i()),
        (0x40e0, 0x00, nop_m(), adds(19, 1, 19, qp=6), nop_i()),
        (0x40f0, 0x00, st8(18, 19, qp=6), nop_i(), nop_i()),
        (0x4100, 0x00, mov_m_gr_ar(36, 64), nop_i(), nop_i()),
        (0x4110, 0x10, nop_m(), nop_i(), br_ret(0)),
    ], entry=0x10, terminal_ip=0x2070, timeout=10.0,
       alat=None, smp="4")
    state = result.state
    expected = state.gr[11] & 0xffff
    if (state.ip != 0x2070 or
        state.exception != IA64_EXCP_NONE or
        state.psr != IA64_PSR_IC | IA64_PSR_BN or
        state.gr[8] != expected or
        state.gr[9] < 1 or
        state.gr[10] != 0x8000 or
        state.gr[14] != 0x8000 or
        state.gr[12] != 0x2000 or
        state.cfm.get("sof") != 73 or
        state.cfm.get("sol") != 70):
        raise RuntimeError(
            "timer_cover_rfi_preserves_large_frame_halfword_rmw failed: "
            f"counter={state.gr[8]!r} expected={expected!r} "
            f"interrupts={state.gr[9]!r} address={state.gr[10]!r} "
            f"bias={state.gr[14]!r} "
            f"iip={state.gr[12]!r} "
            f"sof={state.cfm.get('sof')!r} sol={state.cfm.get('sol')!r} "
            f"ip={state.ip!r} exception={state.exception!r}\n"
            f"{result.register_output}")


def test_rse_large_frame_timer_rfi_preserves_high_caller_local(qemu):
    sentinels = {
        53: 0x1111111111111153,
        54: 0x2222222222222254,
        55: 0x3333333333333355,
        76: 0x4444444444444476,
        91: 0x5555555555555591,
        98: 0x6666666666666698,
    }
    iterations = 0x100001
    result = run_program(qemu, [
        (0x10, *movl_mlx(2, 0x100000)),
        (0x20, 0x00, mov_ar(2, 18), nop_i(), nop_i()),
        # Shift the caller's bottom-of-frame before constructing the exact
        # 73-register kernel frame.  Its high locals then wrap around the
        # 96-entry physical stacked-register file.
        (0x30, 0x00, nop_m(), alloc(40, 45, 41, 0, 0), nop_i()),
        (0x40, 0x10, nop_m(), nop_i(), br_call(0, 0x40, 0x100)),

        (0x100, 0x00, nop_m(), alloc(100, 73, 70, 0, 0), nop_i()),
        (0x110, *movl_mlx(53, sentinels[53])),
        (0x120, *movl_mlx(54, sentinels[54])),
        (0x130, *movl_mlx(55, sentinels[55])),
        (0x140, *movl_mlx(76, sentinels[76])),
        (0x150, *movl_mlx(91, sentinels[91])),
        (0x160, *movl_mlx(98, sentinels[98])),
        (0x170, *movl_mlx(3, 0x8010)),
        (0x180, 0x00, st8(3, 0), nop_i(), nop_i()),
        (0x190, 0x00, nop_m(), nop_i(), nop_i()),
        (0x1a0, 0x10, nop_m(), nop_i(), br_call(0, 0x1a0, 0x300)),
        (0x1b0, 0x00, ld8(2, 3), adds(8, 0, 53), adds(9, 0, 54)),
        (0x1c0, 0x00, nop_m(), adds(10, 0, 55), adds(11, 0, 76)),
        (0x1d0, 0x00, nop_m(), adds(14, 0, 91), adds(15, 0, 98)),
        (0x1e0, 0x10, nop_m(), nop_i(), br_cond(0x1e0, 0x1e0)),

        (0x300, 0x00, nop_m(), alloc(36, 96, 88, 0, 0), nop_i()),
        (0x310, 0x00, adds(4, 0xef, 0), nop_i(), nop_i()),
        (0x320, 0x00, mov_m_gr_cr(4, IA64_CR_ITV), nop_i(), nop_i()),
        # HAL-style arm (WSRV03 halia64 i64itm.s retry_itm_read): re-read
        # ITC after the ITM write and retry while the deadline is no longer
        # in the future -- a missed ITC==ITM equality never fires.
        (0x330, 0x02, mov_m_ar_gr(2, 44), nop_i(), nop_i()),
        (0x340, 0x00,
         addl(4, IA64_ITC_TICKS_PER_MILLISECOND, 2), nop_i(), nop_i()),
        (0x350, 0x00, mov_m_gr_cr(4, IA64_CR_ITM), nop_i(), nop_i()),
        (0x360, 0x02, mov_m_ar_gr(2, 44), nop_i(), nop_i()),
        (0x370, 0x01, nop_m(), cmp_ltu_unc(6, 7, 2, 4), nop_i()),
        (0x380, 0x10, nop_m(), nop_i(), br_cond(0x380, 0x330, qp=7)),
        (0x390, *movl_mlx(8, iterations - 1)),
        (0x3a0, 0x02, nop_m(), mov_lc_gr(8), nop_i()),
        (0x3b0, *movl_mlx(7, (1 << 13) | (1 << 14) | (1 << 44))),
        (0x3c0, 0x08, mov_gr_psr_full(7), srlz_d(), nop_i()),
        (0x3d0, 0x10, nop_m(), adds(10, 1, 10),
         br_cloop(0x3d0, 0x3d0)),
        # The ITM deadline is serviced by the iothread and delivered via
        # async_run_on_cpu, so under host load it can lag well past the
        # counted loop.  Keep the large frame open with PSR.i set until
        # the handler has run at least once; a fixed iteration count is
        # a wall-clock deadline no emulated timer can promise to meet.
        (0x3e0, *movl_mlx(25, 0x8010)),
        (0x3f0, 0x00, ld8(26, 25), nop_i(), nop_i()),
        (0x400, 0x01, nop_m(), cmp4_eq_imm(6, 7, 0, 26), nop_i()),
        (0x410, 0x10, nop_m(), nop_i(), br_cond(0x410, 0x3f0, qp=6)),
        (0x420, 0x00, rsm(1 << 14), nop_i(), nop_i()),
        (0x430, 0x00, mov_m_gr_ar(36, 64), nop_i(), nop_i()),
        (0x440, 0x10, nop_m(), nop_i(), br_ret(0)),

        (0x3000, 0x00, mov_m_cr_gr(16, IA64_CR_SAPIC_IVR),
         nop_i(), nop_i()),
        (0x3010, 0x02, mov_m_ar_gr(17, 44), nop_i(), nop_i()),
        (0x3020, *movl_mlx(20, IA64_ITC_TICKS_PER_MILLISECOND)),
        (0x3030, 0x00, nop_m(), add(17, 17, 20), nop_i()),
        (0x3040, 0x00, mov_m_gr_cr(17, IA64_CR_ITM), nop_i(), nop_i()),
        (0x3050, *movl_mlx(18, 0x8010)),
        (0x3060, 0x00, ld8(19, 18), nop_i(), nop_i()),
        (0x3070, 0x00, nop_m(), adds(19, 1, 19), nop_i()),
        (0x3080, 0x00, st8(18, 19), nop_i(), nop_i()),
        (0x3090, 0x10, mov_m_gr_cr(0, IA64_CR_SAPIC_EOI), nop_i(),
         rfi_b()),
    ], entry=0x10, terminal_ip=0x1e0, timeout=8.0)
    state = result.state
    observed = {
        53: state.gr[8],
        54: state.gr[9],
        55: state.gr[10],
        76: state.gr[11],
        91: state.gr[14],
        98: state.gr[15],
    }
    if (state.ip != 0x1e0 or
        state.exception != IA64_EXCP_NONE or
        observed != sentinels or
        state.gr[2] < 1 or
        state.cfm.get("sof") != 73 or
        state.cfm.get("sol") != 70):
        raise RuntimeError(
            "rse_large_frame_timer_rfi_preserves_high_caller_local failed: "
            f"caller_locals={observed!r} expected={sentinels!r} "
            f"interrupts={state.gr[2]!r} sof={state.cfm.get('sof')!r} "
            f"sol={state.cfm.get('sol')!r} ip={state.ip!r} "
            f"exception={state.exception!r}\n{result.register_output}")

def test_timer_interrupt_exits_chained_loop_after_virtual_deadline(qemu):
    result = run_program(qemu, [
        (0x10, 0x00, adds(3, 0xef, 0), nop_i(),
         nop_i()),
        (0x20, 0x00, mov_m_gr_cr(3, IA64_CR_ITV), nop_i(),
         nop_i()),
        # HAL-style arm (WSRV03 halia64 i64itm.s retry_itm_read): re-read
        # ITC after the ITM write and retry while the deadline is no
        # longer in the future.
        (0x30, 0x02, mov_m_ar_gr(3, 44), nop_i(),
         nop_i()),
        (0x40, 0x00, addl(4, 10 * IA64_ITC_TICKS_PER_MILLISECOND, 3),
         nop_i(), nop_i()),
        (0x50, 0x00, mov_m_gr_cr(4, IA64_CR_ITM), nop_i(),
         nop_i()),
        (0x60, 0x02, mov_m_ar_gr(3, 44), nop_i(),
         nop_i()),
        (0x70, 0x01, nop_m(), cmp_ltu_unc(6, 7, 3, 4),
         nop_i()),
        (0x80, 0x10, nop_m(), nop_i(),
         br_cond(0x80, 0x30, qp=7)),
        (0x90, *movl_mlx(19, (1 << 13) | (1 << 14))),
        (0xa0, 0x08, mov_gr_psr_full(19), srlz_d(),
         nop_i()),
        (0xb0, 0x10, nop_m(), nop_i(),
         br_cond(0xb0, 0xb0)),
        (0x3000, 0x02, mov_m_cr_gr(30, IA64_CR_ITM),
         nop_i(), nop_i()),
        (0x3010, 0x02, mov_m_ar_gr(31, 44),
         nop_i(), nop_i()),
        (0x3020, 0x10, nop_m(), nop_i(),
         br_cond(0x3020, 0x3020)),
    ], entry=0x10, terminal_ip=0x3020, timeout=8.0,
       poll_initial_s=0.100, poll_max_s=0.100)
    state = result.state
    # No polls== or upper-latency bound: delivery lag under host load is
    # unbounded.  The regression this guards -- a TB-chained loop that
    # never yields to the timer -- still fails as a runner timeout.
    if (state.ip != 0x3020 or
        state.exception != IA64_EXCP_NONE or
        state.gr[31] < state.gr[30]):
        raise RuntimeError(
            "timer_interrupt_exits_chained_loop_after_virtual_deadline "
            f"failed: itm={state.gr[30]!r} itc={state.gr[31]!r} "
            f"polls={result.polls} ip={state.ip!r} "
            f"exception={state.exception!r}\n{result.register_output}")

test_async_timer_interrupt_preserves_bank1_grs = require_registers(
    "async_timer_interrupt_preserves_bank1_grs", [
        (0x10, *movl_mlx(16, 0x1116)),
        (0x20, *movl_mlx(27, 0x1127)),
        (0x30, 0x10, nop_m(), nop_i(),
         bsw1()),
        (0x40, *movl_mlx(16, 0x2216)),
        (0x50, *movl_mlx(27, 0x2227)),
        (0x60, 0x00, adds(3, 0xef, 0), nop_i(),
         nop_i()),
        (0x70, 0x00, mov_m_gr_cr(3, IA64_CR_ITV), nop_i(),
         nop_i()),
        (0x80, 0x00, mov_m_gr_ar(0, 44), nop_i(),
         nop_i()),
        (0x90, 0x00, mov_m_gr_cr(0, IA64_CR_ITM), nop_i(),
         nop_i()),
        (0xa0, *movl_mlx(19, (1 << 13) | (1 << 14) | (1 << 44))),
        (0xb0, 0x10, mov_gr_psr_full(19), nop_i(),
         br_cond(0xb0, 0xb0)),
        (0x3000, 0x00, mov_m_cr_gr(5, IA64_CR_SAPIC_IVR), nop_i(),
         nop_i()),
        (0x3010, *movl_mlx(16, 0xa016)),
        (0x3020, *movl_mlx(27, 0xa027)),
        (0x3030, 0x10, nop_m(), nop_i(),
         bsw1()),
        (0x3040, 0x00, nop_m(), adds(2, 0, 16),
         nop_i()),
        (0x3050, 0x00, nop_m(), adds(3, 0, 27),
         nop_i()),
        (0x3060, 0x10, nop_m(), nop_i(),
         bsw0()),
        (0x3070, *movl_mlx(4, 0x100)),
        (0x3080, 0x00, mov_m_gr_cr(4, 19), nop_i(),
         nop_i()),
        (0x3090, 0x10, mov_m_gr_cr(0, IA64_CR_SAPIC_EOI), nop_i(),
         rfi_b()),
        (0x100, 0x00, nop_m(), adds(8, 0, 16),
         nop_i()),
        (0x110, 0x00, nop_m(), adds(9, 0, 27),
         nop_i()),
        (0x120, 0x10, nop_m(), nop_i(),
         br_cond(0x120, 0x120)),
    ], {
        "ip": 0x120,
        "exception": IA64_EXCP_NONE,
        "psr": (1 << 13) | (1 << 14) | (1 << 44),
        "r2": 0x2216,
        "r3": 0x2227,
        "r8": 0x2216,
        "r9": 0x2227,
        "r16": 0x2216,
        "r27": 0x2227,
    }, entry=0x10)

test_tpr_preserves_mmi_and_mic = require_registers(
    "tpr_preserves_mmi_and_mic", [
        (0x10, *movl_mlx(3, 0x12345678900100ff)),
        (0x20, 0x00, mov_m_gr_cr(3, IA64_CR_SAPIC_TPR), nop_i(),
         nop_i()),
        (0x30, 0x00, mov_m_cr_gr(31, IA64_CR_SAPIC_TPR), nop_i(),
         nop_i()),
        (0x40, 0x10, nop_m(), nop_i(),
         br_cond(0x40, 0x40)),
    ], {
        "ip": 0x40,
        "exception": IA64_EXCP_NONE,
        "r31": IA64_TPR_MMI | 0xf0,
    }, entry=0x10)

test_tpr_mmi_masks_timer_until_cleared = require_registers(
    "tpr_mmi_masks_timer_until_cleared", [
        (0x10, *movl_mlx(3, IA64_TPR_MMI)),
        (0x20, 0x00, mov_m_gr_cr(3, IA64_CR_SAPIC_TPR), nop_i(),
         nop_i()),
        (0x30, 0x00, adds(4, 0xef, 0), nop_i(),
         nop_i()),
        (0x40, 0x00, mov_m_gr_cr(4, IA64_CR_ITV), nop_i(),
         nop_i()),
        (0x50, 0x00, mov_m_gr_ar(0, 44), nop_i(),
         nop_i()),
        (0x60, 0x00, mov_m_gr_cr(0, IA64_CR_ITM), nop_i(),
         nop_i()),
        (0x70, *movl_mlx(19, (1 << 13) | (1 << 14))),
        (0x80, 0x00, mov_gr_psr_full(19), nop_i(),
         nop_i()),
        (0x90, 0x00, nop_m(), adds(8, 0x2a, 0),
         nop_i()),
        (0xa0, 0x00, mov_m_gr_cr(0, IA64_CR_SAPIC_TPR), nop_i(),
         nop_i()),
        (0xb0, 0x10, nop_m(), nop_i(),
         br_cond(0xb0, 0xb0)),
        (0x3000, 0x10, nop_m(), adds(31, 0x63, 0),
         br_cond(0x3000, 0x3010)),
        (0x3010, 0x10, nop_m(), nop_i(),
         br_cond(0x3010, 0x3010)),
    ], {
        "ip": 0x3010,
        "exception": IA64_EXCP_NONE,
        "r8": 0x2a,
        "r31": 0x63,
    }, entry=0x10)

def sapic_nested_timer_priority_program(first_vector, second_vector):
    psr_ic_i = (1 << 13) | (1 << 14)

    return [
        (0x10, *movl_mlx(3, first_vector)),
        (0x20, 0x00, mov_m_gr_cr(3, IA64_CR_ITV), nop_i(),
         nop_i()),
        # HAL-style arm (WSRV03 halia64 i64itm.s retry_itm_read): re-read
        # ITC after the ITM write and retry while the deadline is no
        # longer in the future.
        (0x30, 0x02, mov_m_ar_gr(4, 44), nop_i(),
         nop_i()),
        (0x40, 0x00, adds(4, 0x1000, 4), nop_i(),
         nop_i()),
        (0x50, 0x00, mov_m_gr_cr(4, IA64_CR_ITM), nop_i(),
         nop_i()),
        (0x60, 0x02, mov_m_ar_gr(6, 44), nop_i(),
         nop_i()),
        (0x70, 0x01, nop_m(), cmp_ltu_unc(6, 7, 6, 4),
         nop_i()),
        (0x80, 0x10, nop_m(), nop_i(),
         br_cond(0x80, 0x30, qp=7)),
        (0x90, *movl_mlx(19, psr_ic_i)),
        (0xa0, 0x08, mov_gr_psr_full(19), srlz_d(),
         nop_i()),
        (0xb0, 0x10, nop_m(), nop_i(),
         br_cond(0xb0, 0xb0)),
        (0x3000, 0x00, mov_m_cr_gr(5, IA64_CR_SAPIC_IVR),
         adds(31, 1, 31), nop_i()),
        (0x3010, 0x00, nop_m(), cmp4_eq_imm(6, 7, 1, 31),
         nop_i()),
        (0x3020, 0x10, nop_m(), nop_i(),
         br_cond(0x3020, 0x3100, qp=7)),
        (0x3030, *movl_mlx(3, second_vector)),
        (0x3040, 0x00, mov_m_gr_cr(3, IA64_CR_ITV), nop_i(),
         nop_i()),
        (0x3050, 0x02, mov_m_ar_gr(4, 44), nop_i(),
         nop_i()),
        (0x3060, 0x00, adds(4, 0x1000, 4), nop_i(),
         nop_i()),
        (0x3070, 0x00, mov_m_gr_cr(4, IA64_CR_ITM), nop_i(),
         nop_i()),
        (0x3080, 0x02, mov_m_ar_gr(6, 44), nop_i(),
         nop_i()),
        (0x3090, 0x01, nop_m(), cmp_ltu_unc(6, 7, 6, 4),
         nop_i()),
        (0x30a0, 0x10, nop_m(), nop_i(),
         br_cond(0x30a0, 0x3050, qp=7)),
        (0x30b0, *movl_mlx(19, psr_ic_i)),
        (0x30c0, 0x08, mov_gr_psr_full(19), srlz_d(),
         nop_i()),
        (0x30d0, 0x10, nop_m(), nop_i(),
         br_cond(0x30d0, 0x30d0)),
        (0x3100, 0x00, nop_m(), adds(8, 0x5a, 0),
         nop_i()),
        (0x3110, 0x10, nop_m(), nop_i(),
         br_cond(0x3110, 0x3110)),
    ]

test_sapic_extint_masks_external_until_eoi = require_registers(
    "sapic_extint_masks_external_until_eoi",
    sapic_nested_timer_priority_program(0x00, 0xf0),
    {
        "ip": 0x30d0,
        "exception": IA64_EXCP_NONE,
        "r5": 0x00,
        "r8": 0,
        "r31": 1,
    }, entry=0x10)

test_sapic_same_class_higher_vector_preempts = require_registers(
    "sapic_same_class_higher_vector_preempts",
    sapic_nested_timer_priority_program(0xf0, 0xf1),
    {
        "ip": 0x3110,
        "exception": IA64_EXCP_NONE,
        "r5": 0xf1,
        "r8": 0x5a,
        "r31": 2,
    }, entry=0x10)

test_masked_itv_discards_due_timer = require_registers(
    "masked_itv_discards_due_timer", [
        (0x10, *movl_mlx(3, IA64_VECTOR_MASKED | 0xef)),
        (0x20, 0x00, mov_m_gr_cr(3, IA64_CR_ITV), nop_i(),
         nop_i()),
        (0x30, 0x00, mov_m_gr_ar(0, 44), nop_i(),
         nop_i()),
        (0x40, *movl_mlx(4, 0x1000)),
        (0x50, 0x00, mov_m_gr_cr(4, IA64_CR_ITM), nop_i(),
         nop_i()),
        (0x60, *movl_mlx(8, 0x10000)),
        (0x70, 0x02, nop_m(), mov_lc_gr(8),
         nop_i()),
        (0x80, 0x10, nop_m(), nop_i(),
         br_cloop(0x80, 0x80)),
        (0x90, 0x00, adds(3, 0xef, 0), nop_i(),
         nop_i()),
        (0xa0, 0x00, mov_m_gr_cr(3, IA64_CR_ITV), nop_i(),
         nop_i()),
        (0xb0, *movl_mlx(19, (1 << 13) | (1 << 14))),
        (0xc0, 0x00, mov_gr_psr_full(19), nop_i(),
         nop_i()),
        (0xd0, 0x10, nop_m(), nop_i(),
         br_cond(0xd0, 0xd0)),
        (0x3000, 0x10, nop_m(), adds(31, 0x64, 0),
         br_cond(0x3000, 0x3000)),
    ], {
        "ip": 0xd0,
        "exception": IA64_EXCP_NONE,
        "r31": 0,
    }, entry=0x10)

test_invalid_itv_vector_is_ignored = require_registers(
    "invalid_itv_vector_is_ignored", [
        (0x10, *movl_mlx(3, 1)),
        (0x20, 0x00, mov_m_gr_cr(3, IA64_CR_ITV), nop_i(),
         nop_i()),
        (0x30, 0x00, mov_m_gr_ar(0, 44), nop_i(),
         nop_i()),
        (0x40, *movl_mlx(4, 0x1000)),
        (0x50, 0x00, mov_m_gr_cr(4, IA64_CR_ITM), nop_i(),
         nop_i()),
        (0x60, *movl_mlx(8, 0x10000)),
        (0x70, 0x02, nop_m(), mov_lc_gr(8),
         nop_i()),
        (0x80, 0x10, nop_m(), nop_i(),
         br_cloop(0x80, 0x80)),
        (0x90, *movl_mlx(19, (1 << 13) | (1 << 14))),
        (0xa0, 0x00, mov_gr_psr_full(19), nop_i(),
         nop_i()),
        (0xb0, 0x10, nop_m(), nop_i(),
         br_cond(0xb0, 0xb0)),
        (0x3000, 0x10, nop_m(), adds(31, 0x66, 0),
         br_cond(0x3000, 0x3000)),
    ], {
        "ip": 0xb0,
        "exception": IA64_EXCP_NONE,
        "r31": 0,
    }, entry=0x10)

test_past_itm_does_not_fire = require_registers(
    "past_itm_does_not_fire", [
        (0x10, *movl_mlx(4, 0x200000)),
        (0x20, 0x00, adds(3, 0xef, 0), nop_i(),
         nop_i()),
        (0x30, 0x00, mov_m_gr_ar(4, 44), nop_i(),
         nop_i()),
        (0x40, 0x00, mov_m_gr_cr(3, IA64_CR_ITV), nop_i(),
         nop_i()),
        (0x50, 0x00, mov_m_gr_cr(0, IA64_CR_ITM), nop_i(),
         nop_i()),
        (0x60, *movl_mlx(19, (1 << 13) | (1 << 14))),
        (0x70, 0x10, mov_gr_psr_full(19), nop_i(),
         br_cond(0x70, 0x80)),
        (0x80, 0x10, nop_m(), nop_i(),
         br_cond(0x80, 0x80)),
        (0x3000, 0x10, nop_m(), adds(31, 0x55, 0),
         br_cond(0x3000, 0x3000)),
    ], {
        "ip": 0x80,
        "exception": IA64_EXCP_NONE,
    }, entry=0x10)

test_past_rearmed_itm_does_not_interrupt = require_registers(
    "past_rearmed_itm_does_not_interrupt", [
        (0x10, 0x00, adds(3, 0xef, 0), nop_i(),
         nop_i()),
        (0x20, 0x00, mov_m_gr_cr(3, IA64_CR_ITV), nop_i(),
         nop_i()),
        (0x30, 0x00, mov_m_gr_ar(0, 44), nop_i(),
         nop_i()),
        (0x40, 0x00, mov_m_gr_cr(0, IA64_CR_ITM), nop_i(),
         nop_i()),
        (0x50, *movl_mlx(19, (1 << 13) | (1 << 14))),
        (0x60, 0x10, mov_gr_psr_full(19), nop_i(),
         br_cond(0x60, 0x70)),
        (0x70, 0x10, nop_m(), nop_i(),
         br_cond(0x70, 0x70)),
        (0x3000, 0x00, mov_m_cr_gr(5, IA64_CR_SAPIC_IVR),
         adds(31, 1, 31),
         nop_i()),
        (0x3010, 0x00, nop_m(), cmp4_eq_imm(6, 7, 1, 31),
         nop_i()),
        (0x3020, 0x10, nop_m(), nop_i(),
         br_cond(0x3020, 0x3080, qp=7)),
        (0x3030, *movl_mlx(4, 0x200000)),
        (0x3040, 0x00, mov_m_gr_ar(4, 44), nop_i(),
         nop_i()),
        (0x3050, *movl_mlx(6, 0x100)),
        (0x3060, 0x00, mov_m_gr_cr(6, IA64_CR_ITM), nop_i(),
         nop_i()),
        (0x3070, 0x10, mov_m_gr_cr(0, IA64_CR_SAPIC_EOI), nop_i(),
         rfi_b()),
        (0x3080, 0x00, mov_m_gr_cr(0, IA64_CR_SAPIC_EOI),
         adds(8, 0x2a, 0),
         nop_i()),
        (0x3090, 0x10, nop_m(), nop_i(),
         br_cond(0x3090, 0x3090)),
    ], {
        "ip": 0x70,
        "exception": IA64_EXCP_NONE,
        "r31": 1,
    }, entry=0x10)

test_future_itm_rearm_preserves_pended_timer_irr = require_registers(
    "future_itm_rearm_preserves_pended_timer_irr", [
        (0x10, 0x00, adds(3, 0xef, 0), nop_i(),
         nop_i()),
        (0x20, 0x00, mov_m_gr_cr(3, IA64_CR_ITV), nop_i(),
         nop_i()),
        (0x30, 0x00, mov_m_gr_ar(4, 44), nop_i(),
         nop_i()),
        (0x40, 0x00, mov_m_gr_cr(4, IA64_CR_ITM), nop_i(),
         nop_i()),
        (0x50, *movl_mlx(5, 0x100000000)),
        (0x60, 0x00, add(5, 4, 5), nop_i(),
         nop_i()),
        (0x70, 0x00, mov_m_gr_cr(5, IA64_CR_ITM), nop_i(),
         nop_i()),
        (0x80, *movl_mlx(19, (1 << 13) | (1 << 14))),
        (0x90, 0x00, mov_gr_psr_full(19), nop_i(),
         nop_i()),
        (0xa0, 0x10, nop_m(), adds(8, 0x2a, 0),
         br_cond(0xa0, 0xa0)),
        (0x3000, 0x10, nop_m(), adds(31, 0x66, 0),
         br_cond(0x3000, 0x3010)),
        (0x3010, 0x10, nop_m(), nop_i(),
         br_cond(0x3010, 0x3010)),
    ], {
        "ip": 0x3010,
        "exception": IA64_EXCP_NONE,
        "r31": 0x66,
    }, entry=0x10)

test_masking_itv_preserves_pended_timer_irr = require_registers(
    "masking_itv_preserves_pended_timer_irr", [
        (0x10, 0x00, adds(3, 0xef, 0), nop_i(),
         nop_i()),
        (0x20, 0x00, mov_m_gr_cr(3, IA64_CR_ITV), nop_i(),
         nop_i()),
        (0x30, 0x00, mov_m_gr_ar(4, 44), nop_i(),
         nop_i()),
        (0x40, 0x00, mov_m_gr_cr(4, IA64_CR_ITM), nop_i(),
         nop_i()),
        (0x50, *movl_mlx(3, IA64_VECTOR_MASKED | 0xef)),
        (0x60, 0x00, mov_m_gr_cr(3, IA64_CR_ITV), nop_i(),
         nop_i()),
        (0x70, *movl_mlx(19, (1 << 13) | (1 << 14))),
        (0x80, 0x00, mov_gr_psr_full(19), nop_i(),
         nop_i()),
        (0x90, 0x10, nop_m(), adds(8, 0x2a, 0),
         br_cond(0x90, 0x90)),
        (0x3000, 0x10, nop_m(), adds(31, 0x67, 0),
         br_cond(0x3000, 0x3010)),
        (0x3010, 0x10, nop_m(), nop_i(),
         br_cond(0x3010, 0x3010)),
    ], {
        "ip": 0x3010,
        "exception": IA64_EXCP_NONE,
        "r31": 0x67,
    }, entry=0x10)


def test_future_itm_rearm_uses_latest_deadline(qemu):
    result = run_program(qemu, [
        (0x10, 0x00, adds(3, 0xef, 0), nop_i(), nop_i()),
        (0x20, 0x00, mov_m_gr_cr(3, IA64_CR_ITV), nop_i(), nop_i()),
        (0x30, 0x02, mov_m_ar_gr(3, 44), nop_i(), nop_i()),
        (0x40, 0x00, addl(4, IA64_ITC_ADDL_DELAY_TICKS, 3),
         nop_i(), nop_i()),
        (0x50, 0x00, mov_m_gr_cr(4, IA64_CR_ITM), nop_i(), nop_i()),
        (0x60, *movl_mlx(6, IA64_ITC_ADDL_DELAY_TICKS)),
        (0x70, 0x00, nop_m(), add(5, 4, 6), nop_i()),
        (0x80, 0x00, mov_m_gr_cr(5, IA64_CR_ITM), nop_i(), nop_i()),
        (0x90, *movl_mlx(19, IA64_PSR_IC | IA64_PSR_I)),
        (0xa0, 0x08, mov_gr_psr_full(19), srlz_d(), nop_i()),
        (0xb0, 0x10, nop_m(), nop_i(), br_cond(0xb0, 0xb0)),
        (0x3000, 0x00, mov_m_cr_gr(6, IA64_CR_SAPIC_IVR),
         nop_i(), nop_i()),
        (0x3010, 0x02, mov_m_ar_gr(8, 44), nop_i(), nop_i()),
        (0x3020, 0x00, mov_m_cr_gr(9, IA64_CR_ITM), nop_i(), nop_i()),
        (0x3030, 0x10, nop_m(), nop_i(), br_cond(0x3030, 0x3030)),
    ], entry=0x10, terminal_ip=0x3030)
    state = result.state
    if (state.exception != IA64_EXCP_NONE or
        state.gr[6] != 0xef or
        state.gr[8] < state.gr[5] or
        state.gr[9] != state.gr[5]):
        raise RuntimeError(
            "future_itm_rearm_uses_latest_deadline failed: "
            f"exception={state.exception!r} vector={state.gr[6]!r} "
            f"old_itm={state.gr[4]!r} "
            f"new_itm={state.gr[5]!r} fired_itc={state.gr[8]!r} "
            f"fired_itm={state.gr[9]!r}\n{result.register_output}")


def test_past_itm_reprogram_cancels_future_deadline(qemu):
    result = run_program(qemu, [
        (0x10, 0x00, adds(3, 0xef, 0), nop_i(), nop_i()),
        (0x20, 0x00, mov_m_gr_cr(3, IA64_CR_ITV), nop_i(), nop_i()),
        (0x30, 0x02, mov_m_ar_gr(3, 44), nop_i(), nop_i()),
        (0x40, 0x00, addl(4, IA64_ITC_ADDL_DELAY_TICKS, 3),
         nop_i(), nop_i()),
        (0x50, 0x00, mov_m_gr_cr(4, IA64_CR_ITM), nop_i(), nop_i()),
        (0x60, 0x00, adds(5, -1, 3), nop_i(), nop_i()),
        (0x70, 0x00, mov_m_gr_cr(5, IA64_CR_ITM), nop_i(), nop_i()),
        (0x80, *movl_mlx(7, IA64_ITC_ADDL_DELAY_TICKS)),
        (0x90, 0x00, nop_m(), add(7, 4, 7), nop_i()),
        (0xa0, *movl_mlx(19, IA64_PSR_IC | IA64_PSR_I)),
        (0xb0, 0x08, mov_gr_psr_full(19), srlz_d(), nop_i()),
        (0xc0, 0x02, mov_m_ar_gr(6, 44), nop_i(), nop_i()),
        (0xd0, 0x00, nop_m(), cmp_ltu_unc(6, 7, 6, 7), nop_i()),
        (0xe0, 0x10, nop_m(), nop_i(), br_cond(0xe0, 0xc0, qp=6)),
        (0xf0, 0x10, nop_m(), nop_i(), br_cond(0xf0, 0xf0)),
        (0x3000, 0x00, mov_m_cr_gr(9, IA64_CR_SAPIC_IVR),
         nop_i(), nop_i()),
        (0x3010, 0x00, nop_m(), adds(8, 1, 8), nop_i()),
        (0x3020, 0x10, mov_m_gr_cr(0, IA64_CR_SAPIC_EOI), nop_i(), rfi_b()),
    ], entry=0x10, terminal_ip=0xf0)
    state = result.state
    if (state.exception != IA64_EXCP_NONE or
        state.gr[6] < state.gr[7] or state.gr[8] != 0):
        raise RuntimeError(
            "past_itm_reprogram_cancels_future_deadline failed: "
            f"exception={state.exception!r} old_itm={state.gr[4]!r} "
            f"cancel_itm={state.gr[5]!r} "
            f"wait_itc={state.gr[7]!r} final_itc={state.gr[6]!r} "
            f"interrupts={state.gr[8]!r}\n"
            f"{result.register_output}")


INTERRUPTION_PSR_INPUT = (
    IA64_PSR_UP | IA64_PSR_MFL | IA64_PSR_MFH |
    IA64_PSR_AC | IA64_PSR_IC | IA64_PSR_I | IA64_PSR_PK |
    IA64_PSR_DFL | IA64_PSR_DFH | IA64_PSR_SP |
    IA64_PSR_DI | IA64_PSR_SI | IA64_PSR_DT | IA64_PSR_RT |
    IA64_PSR_MC
)

INTERRUPTION_PSR_EXPECTED = (
    IA64_PSR_BE | IA64_PSR_UP | IA64_PSR_MFL | IA64_PSR_MFH |
    IA64_PSR_PK | IA64_PSR_DT | IA64_PSR_PP | IA64_PSR_RT |
    IA64_PSR_MC
)

test_exception_entry_initializes_psr = require_registers(
    "exception_entry_initializes_psr", [
        (0x10, *movl_mlx(16, IA64_DCR_BE | IA64_DCR_PP)),
        (0x20, *movl_mlx(19, INTERRUPTION_PSR_INPUT)),
        (0x30, 0x00, mov_m_gr_cr(16, 0), adds(31, 0x60, 0),
         nop_i()),
        *rfi_to_gr(0x40, 19, 31),
        (0x60, 0x00, srlz_d(), nop_i(),
         nop_i()),
        (0x70, 0x00, break_m(0x42), nop_i(),
         nop_i()),
        (IA64_BREAK_VECTOR, 0x00, mov_m_psr_gr(29), nop_i(),
         nop_i()),
        (IA64_BREAK_VECTOR + 0x10, 0x00, mov_m_cr_gr(30, 16),
         nop_i(), nop_i()),
        (IA64_BREAK_VECTOR + 0x20, 0x10, nop_m(), nop_i(),
         br_cond(IA64_BREAK_VECTOR + 0x20, IA64_BREAK_VECTOR + 0x20)),
    ], {
        "ip": IA64_BREAK_VECTOR + 0x20,
        "exception": IA64_EXCP_NONE,
        "r29": INTERRUPTION_PSR_EXPECTED,
        "r30": INTERRUPTION_PSR_INPUT,
    }, entry=0x10)

test_mov_pkr_does_not_alias_interruption_crs = require_registers(
    "mov_pkr_does_not_alias_interruption_crs", [
        (0x10, 0x00, addl(2, 0x1234, 0), nop_i(),
         nop_i()),
        (0x20, 0x00, mov_m_gr_cr(2, 19), nop_i(),
         nop_i()),
        (0x30, 0x00, addl(3, 0x103, 0), addl(4, 0x5601, 0),
         nop_i()),
        (0x40, 0x00, mov_pkr_indexed(3, 4, bit36=1), nop_i(),
         nop_i()),
        (0x50, 0x09, mov_m_cr_gr(5, 19), mov_pkr_indexed_read(6, 3, bit36=1),
         nop_i()),
        (0x60, 0x10, nop_m(), nop_i(),
         br_cond(0x60, 0x60)),
    ], {
        "ip": 0x60,
        "exception": IA64_EXCP_NONE,
        "r5": 0x1234,
        "r6": 0x5601,
    }, entry=0x10)

test_rfi_resumes_at_ipsr_ri_slot = require_registers(
    "rfi_resumes_at_ipsr_ri_slot", [
        (0x10, *movl_mlx(19, 1 << 41)),
        (0x20, *movl_mlx(20, 0x60)),
        (0x30, 0x00, mov_m_gr_cr(19, 16), nop_i(),
         nop_i()),
        (0x40, 0x00, mov_m_gr_cr(20, 19), nop_i(),
         nop_i()),
        (0x50, 0x10, nop_m(), nop_i(),
         rfi_b()),
        (0x60, 0x10, adds(30, 0x11, 0), adds(31, 0x22, 0),
         br_cond(0x60, 0x70)),
        (0x70, 0x00, mov_m_psr_gr(29), nop_i(),
         nop_i()),
        (0x80, 0x10, nop_m(), nop_i(),
         br_cond(0x80, 0x80)),
    ], {
        "ip": 0x80,
        "exception": IA64_EXCP_NONE,
        "r29": 0,
        "r30": 0,
        "r31": 0x22,
    }, entry=0x10)

test_mov_from_psr_does_not_copy_execution_slot_to_rfi = require_registers(
    "mov_from_psr_does_not_copy_execution_slot_to_rfi", [
        (0x10, *movl_mlx(20, 0x60)),
        (0x20, 0x08, nop_m(), mov_m_psr_gr(19), nop_i()),
        (0x30, 0x08, mov_m_gr_cr(19, 16), mov_m_gr_cr(20, 19),
         nop_i()),
        (0x40, 0x10, nop_m(), nop_i(), rfi_b()),
        (0x60, 0x10, adds(30, 0x11, 0), adds(31, 0x22, 0),
         br_cond(0x60, 0x70)),
        (0x70, 0x10, nop_m(), nop_i(),
         br_cond(0x70, 0x70)),
    ], {
        "ip": 0x70,
        "exception": IA64_EXCP_NONE,
        "r19": 0,
        "r30": 0x11,
        "r31": 0x22,
    }, entry=0x10)

test_rfi_ignores_iip_low_bits = require_registers(
    "rfi_ignores_iip_low_bits", [
        (0x10, *movl_mlx(19, 1 << 41)),
        (0x20, *movl_mlx(20, 0x6f)),
        (0x30, 0x00, mov_m_gr_cr(19, 16), nop_i(),
         nop_i()),
        (0x40, 0x00, mov_m_gr_cr(20, 19), nop_i(),
         nop_i()),
        (0x50, 0x10, nop_m(), nop_i(),
         rfi_b()),
        (0x60, 0x10, adds(30, 0x11, 0), adds(31, 0x22, 0),
         br_cond(0x60, 0x70)),
        (0x70, 0x10, nop_m(), nop_i(),
         br_cond(0x70, 0x70)),
    ], {
        "ip": 0x70,
        "exception": IA64_EXCP_NONE,
        "r30": 0,
        "r31": 0x22,
    }, entry=0x10)

# ── Exception tests ──

test_exception_break = require_exception("exception_break", [
    (0x10, 0x11, nop_m(), break_m(0x42), nop_m()),
], IA64_EXCP_BREAK, fault_ip=0x10, fault_imm=0x42)

test_exception_syscall_break = require_exception("exception_syscall_break", [
    (0x10, 0x11, nop_m(), pal_break(), nop_m()),
], IA64_EXCP_BREAK, fault_ip=0x10, fault_imm=0x100000)

test_exception_break_f = require_exception("exception_break_f", [
    (0x10, 0x0d, nop_m(), break_f(0x42), nop_i()),
], IA64_EXCP_BREAK, fault_ip=0x10, fault_imm=0x42)

test_exception_break_x = require_registers("exception_break_x", [
    (0x100000, *movl_mlx(2, 1 << 13)),
    (0x100010, 0x10, mov_gr_psr_full(2), nop_i(),
     br_cond(0x100010, 0x10)),
    (0x10, *break_x_mlx(0x34b630b4b820032b)),
    (IA64_BREAK_VECTOR, 0x00, nop_m(), nop_i(), nop_i()),
    (IA64_BREAK_VECTOR + 0x10, 0x00, nop_m(), nop_i(), nop_i()),
    (IA64_BREAK_VECTOR + 0x20, 0x10, nop_m(), nop_i(),
     br_cond(IA64_BREAK_VECTOR + 0x20, IA64_BREAK_VECTOR + 0x20)),
], {
    "ip": IA64_BREAK_VECTOR + 0x20,
    "exception": IA64_EXCP_NONE,
    "fault_ip": 0x10,
    "fault_imm": 0x34b630b4b820032b,
}, entry=0x100000)

test_exception_records_slot_ri = require_registers(
    "exception_records_slot_ri", [
        (0x10, *movl_mlx(19, 1 << 13)),
        (0x20, 0x10, mov_gr_psr_full(19), nop_i(),
         br_cond(0x20, 0x30)),
        (0x30, 0x00, srlz_d(), nop_i(),
         nop_i()),
        (0x40, 0x11, nop_m(), break_m(0x42), nop_m()),
        (IA64_BREAK_VECTOR, 0x00, mov_m_cr_gr(16, 16), nop_i(),
         nop_i()),
        (IA64_BREAK_VECTOR + 0x10, 0x00, mov_m_cr_gr(17, 17), nop_i(),
         nop_i()),
        (IA64_BREAK_VECTOR + 0x20, 0x00, mov_m_psr_gr(18), nop_i(),
         nop_i()),
        (IA64_BREAK_VECTOR + 0x30, 0x10, nop_m(), nop_i(),
         br_cond(IA64_BREAK_VECTOR + 0x30, IA64_BREAK_VECTOR + 0x30)),
    ], {
        "ip": IA64_BREAK_VECTOR + 0x30,
        "exception": IA64_EXCP_NONE,
        "r16": (1 << 13) | (1 << 41),
        "r17": (1 << 41),
        "r18": 0,
    }, entry=0x10)

test_break_preserves_ifa_and_records_iim_isr = require_registers(
    "break_preserves_ifa_and_records_iim_isr", [
        (0x10, *movl_mlx(2, 1 << 13)),
        (0x20, *movl_mlx(20, 0x1111222233334444)),
        (0x30, 0x00, mov_m_gr_cr(20, 20), nop_i(),
         nop_i()),
        (0x40, 0x10, mov_gr_psr_full(2), nop_i(),
         br_cond(0x40, 0x50)),
        (0x50, 0x00, srlz_d(), nop_i(),
         nop_i()),
        (0x60, 0x11, nop_m(), break_m(0x42), nop_m()),
        (IA64_BREAK_VECTOR, 0x00, mov_m_cr_gr(8, 20), nop_i(),
         nop_i()),
        (IA64_BREAK_VECTOR + 0x10, 0x00, mov_m_cr_gr(9, 24), nop_i(),
         nop_i()),
        (IA64_BREAK_VECTOR + 0x20, 0x00, mov_m_cr_gr(10, 17), nop_i(),
         nop_i()),
        (IA64_BREAK_VECTOR + 0x30, 0x10, nop_m(), nop_i(),
         br_cond(IA64_BREAK_VECTOR + 0x30, IA64_BREAK_VECTOR + 0x30)),
    ], {
        "ip": IA64_BREAK_VECTOR + 0x30,
        "exception": IA64_EXCP_NONE,
        "r8": 0x1111222233334444,
        "r9": 0x42,
        "r10": (1 << 41),
    }, entry=0x10)

test_iipa_reports_previous_successful_bundle_for_slot0_fault = require_registers(
    "iipa_reports_previous_successful_bundle_for_slot0_fault", [
        (0x10, *movl_mlx(2, 1 << 13)),
        (0x20, 0x10, mov_gr_psr_full(2), nop_i(),
         br_cond(0x20, 0x40)),
        (0x40, 0x00, nop_m(), nop_i(), nop_i()),
        (0x50, 0x00, break_m(0x42), nop_i(), nop_i()),
        (IA64_BREAK_VECTOR, 0x00, mov_m_cr_gr(8, 22), nop_i(), nop_i()),
        (IA64_BREAK_VECTOR + 0x10, 0x10, nop_m(), nop_i(),
         br_cond(IA64_BREAK_VECTOR + 0x10, IA64_BREAK_VECTOR + 0x10)),
    ], {
        "ip": IA64_BREAK_VECTOR + 0x10,
        "exception": IA64_EXCP_NONE,
        "r8": 0x40,
    }, entry=0x10)

test_iipa_reports_current_bundle_after_prior_slot_success = require_registers(
    "iipa_reports_current_bundle_after_prior_slot_success", [
        (0x10, *movl_mlx(2, 1 << 13)),
        (0x20, 0x10, mov_gr_psr_full(2), nop_i(),
         br_cond(0x20, 0x40)),
        (0x40, 0x11, nop_m(), break_m(0x42), nop_m()),
        (IA64_BREAK_VECTOR, 0x00, mov_m_cr_gr(8, 22), nop_i(), nop_i()),
        (IA64_BREAK_VECTOR + 0x10, 0x10, nop_m(), nop_i(),
         br_cond(IA64_BREAK_VECTOR + 0x10, IA64_BREAK_VECTOR + 0x10)),
    ], {
        "ip": IA64_BREAK_VECTOR + 0x10,
        "exception": IA64_EXCP_NONE,
        "r8": 0x40,
    }, entry=0x10)

test_mov_to_iipa_establishes_last_ip_for_rfi_to_fault = require_registers(
    "mov_to_iipa_establishes_last_ip_for_rfi_to_fault", [
        (0x10, *movl_mlx(2, 1 << 13)),
        (0x20, 0x10, mov_gr_psr_full(2), nop_i(),
         br_cond(0x20, 0x40)),
        (0x40, 0x00, nop_m(), nop_i(), nop_i()),
        (0x50, 0x00, break_m(0x42), nop_i(), nop_i()),
        (0x90, 0x00, break_m(0x43), nop_i(), nop_i()),
        (IA64_BREAK_VECTOR, 0x00, mov_m_cr_gr(9, 24), nop_i(), nop_i()),
        (IA64_BREAK_VECTOR + 0x10, 0x00,
         cmp4_eq_unc_imm(6, 7, 0x42, 9), nop_i(), nop_i()),
        (IA64_BREAK_VECTOR + 0x20, 0x10, nop_m(), nop_i(),
         br_cond(IA64_BREAK_VECTOR + 0x20, IA64_BREAK_VECTOR + 0x50, qp=6)),
        (IA64_BREAK_VECTOR + 0x30, 0x00, mov_m_cr_gr(8, 22), nop_i(),
         nop_i()),
        (IA64_BREAK_VECTOR + 0x40, 0x10, nop_m(), nop_i(),
         br_cond(IA64_BREAK_VECTOR + 0x40, IA64_BREAK_VECTOR + 0x40)),
        (IA64_BREAK_VECTOR + 0x50, *movl_mlx(19, 1 << 13)),
        (IA64_BREAK_VECTOR + 0x60, *movl_mlx(20, 0x90)),
        (IA64_BREAK_VECTOR + 0x70, *movl_mlx(21, 0x1230)),
        (IA64_BREAK_VECTOR + 0x80, 0x00, mov_m_gr_cr(21, 22), nop_i(),
         nop_i()),
        (IA64_BREAK_VECTOR + 0x90, 0x00, mov_m_gr_cr(19, 16), nop_i(),
         nop_i()),
        (IA64_BREAK_VECTOR + 0xa0, 0x00, mov_m_gr_cr(20, 19), nop_i(),
         nop_i()),
        (IA64_BREAK_VECTOR + 0xb0, 0x10, nop_m(), nop_i(), rfi_b()),
    ], {
        "ip": IA64_BREAK_VECTOR + 0x40,
        "exception": IA64_EXCP_NONE,
        "r8": 0x1230,
    }, entry=0x10)

test_iipa_preserved_for_rfi_to_fault = require_registers(
    "iipa_preserved_for_rfi_to_fault", [
        (0x10, *movl_mlx(2, 1 << 13)),
        (0x20, 0x10, mov_gr_psr_full(2), nop_i(),
         br_cond(0x20, 0x40)),
        (0x40, 0x00, nop_m(), nop_i(), nop_i()),
        (0x50, 0x00, break_m(0x42), nop_i(), nop_i()),
        (0x90, 0x00, break_m(0x43), nop_i(), nop_i()),
        (IA64_BREAK_VECTOR, 0x00, mov_m_cr_gr(9, 24), nop_i(), nop_i()),
        (IA64_BREAK_VECTOR + 0x10, 0x00,
         cmp4_eq_unc_imm(6, 7, 0x42, 9), nop_i(), nop_i()),
        (IA64_BREAK_VECTOR + 0x20, 0x10, nop_m(), nop_i(),
         br_cond(IA64_BREAK_VECTOR + 0x20, IA64_BREAK_VECTOR + 0x50, qp=6)),
        (IA64_BREAK_VECTOR + 0x30, 0x00, mov_m_cr_gr(8, 22), nop_i(),
         nop_i()),
        (IA64_BREAK_VECTOR + 0x40, 0x10, nop_m(), nop_i(),
         br_cond(IA64_BREAK_VECTOR + 0x40, IA64_BREAK_VECTOR + 0x40)),
        (IA64_BREAK_VECTOR + 0x50, *movl_mlx(19, 1 << 13)),
        (IA64_BREAK_VECTOR + 0x60, *movl_mlx(20, 0x90)),
        (IA64_BREAK_VECTOR + 0x70, 0x00, mov_m_gr_cr(19, 16), nop_i(),
         nop_i()),
        (IA64_BREAK_VECTOR + 0x80, 0x00, mov_m_gr_cr(20, 19), nop_i(),
         nop_i()),
        (IA64_BREAK_VECTOR + 0x90, 0x10, nop_m(), nop_i(), rfi_b()),
    ], {
        "ip": IA64_BREAK_VECTOR + 0x40,
        "exception": IA64_EXCP_NONE,
        "r8": 0x40,
    }, entry=0x10)

test_exception_clears_ifs_keeps_cfm = require_registers(
    "exception_clears_ifs_keeps_cfm", [
        (0x10, *movl_mlx(2, 1 << 13)),
        (0x20, 0x10, mov_gr_psr_full(2), nop_i(),
         br_cond(0x20, 0x40)),
        (0x40, 0x00, alloc(38, 7, 4, 0, 0), nop_i(),
         nop_i()),
        (0x50, 0x11, nop_m(), break_m(0x42), nop_m()),
        (IA64_BREAK_VECTOR, 0x00, mov_m_cr_gr(8, 23), nop_i(),
         nop_i()),
        (IA64_BREAK_VECTOR + 0x10, 0x10, nop_m(), nop_i(),
         br_cond(IA64_BREAK_VECTOR + 0x10, IA64_BREAK_VECTOR + 0x10)),
    ], {
        "ip": IA64_BREAK_VECTOR + 0x10,
        "exception": IA64_EXCP_NONE,
        "r8": 0,
        "cfm_sof": 7,
        "cfm_sol": 4,
    }, entry=0x10)

test_cover_saves_interrupted_cfm_to_ifs = require_registers(
    "cover_saves_interrupted_cfm_to_ifs", [
        (0x10, *movl_mlx(2, 1 << 13)),
        (0x20, 0x10, mov_gr_psr_full(2), nop_i(),
         br_cond(0x20, 0x40)),
        (0x40, 0x00, alloc(38, 7, 4, 0, 0), nop_i(),
         nop_i()),
        (0x50, 0x11, nop_m(), break_m(0x42), nop_m()),
        (IA64_BREAK_VECTOR, 0x00, mov_m_cr_gr(8, 23), nop_i(),
         nop_i()),
        (IA64_BREAK_VECTOR + 0x10, 0x10, nop_m(), nop_i(),
         cover_b()),
        (IA64_BREAK_VECTOR + 0x20, 0x00, mov_m_cr_gr(9, 23), nop_i(),
         nop_i()),
        (IA64_BREAK_VECTOR + 0x30, 0x10, nop_m(), nop_i(),
         br_cond(IA64_BREAK_VECTOR + 0x30, IA64_BREAK_VECTOR + 0x30)),
    ], {
        "ip": IA64_BREAK_VECTOR + 0x30,
        "exception": IA64_EXCP_NONE,
        "r8": 0,
        "r9": (1 << 63) | 0x207,
        "cfm_sof": 0,
        "cfm_sol": 0,
    }, entry=0x10)

test_rfi_restores_interrupted_bsp_after_cover = require_registers(
    "rfi_restores_interrupted_bsp_after_cover", [
        (0x10, *movl_mlx(2, 1 << 13)),
        (0x20, 0x10, mov_gr_psr_full(2), nop_i(),
         br_cond(0x20, 0x40)),
        (0x40, *movl_mlx(3, 0x100000)),
        (0x50, 0x00, mov_ar(3, 18), nop_i(),
         nop_i()),
        (0x60, 0x00, alloc(38, 7, 4, 0, 0), nop_i(),
         nop_i()),
        (0x70, 0x00, mov_m_ar_gr(8, 17), nop_i(),
         nop_i()),
        (0x80, 0x00, break_m(0x42), nop_i(),
         nop_i()),
        (0x90, 0x00, mov_m_ar_gr(9, 17), nop_i(),
         nop_i()),
        (0xa0, 0x10, nop_m(), nop_i(),
         br_cond(0xa0, 0xa0)),
        (IA64_BREAK_VECTOR, 0x10, nop_m(), nop_i(),
         cover_b()),
        (IA64_BREAK_VECTOR + 0x10, *movl_mlx(20, 0x90)),
        (IA64_BREAK_VECTOR + 0x20, 0x00, mov_m_gr_cr(20, 19), nop_i(),
         nop_i()),
        (IA64_BREAK_VECTOR + 0x30, 0x10, nop_m(), nop_i(),
         rfi_b()),
    ], {
        "ip": 0xa0,
        "exception": IA64_EXCP_NONE,
        "r8": 0x100000,
        "r9": 0x100000,
        "cfm_sof": 7,
        "cfm_sol": 4,
    }, entry=0x10)

test_nested_exception_keeps_handler_return_state = require_registers(
    "nested_exception_keeps_handler_return_state", [
        (0x10, *movl_mlx(2, 1 << 13)),
        (0x20, *movl_mlx(3, IA64_BREAK_VECTOR + 0x50)),
        (0x30, 0x10, mov_gr_psr_full(2), nop_i(),
         br_cond(0x30, 0x40)),
        (0x40, 0x11, nop_m(), break_m(0x42), nop_m()),
        (IA64_BREAK_VECTOR, 0x08, nop_m(), cmp4_eq_imm(1, 2, 0, 6),
         nop_i()),
        (IA64_BREAK_VECTOR + 0x10, 0x10, nop_m(), nop_i(),
         br_cond(IA64_BREAK_VECTOR + 0x10, IA64_BREAK_VECTOR + 0x30, qp=1)),
        (IA64_BREAK_VECTOR + 0x20, 0x10, nop_m(), nop_i(),
         br_cond(IA64_BREAK_VECTOR + 0x20, IA64_BREAK_VECTOR + 0x70, qp=2)),
        (IA64_BREAK_VECTOR + 0x30, 0x10, mov_gr_psr_full(2), adds(6, 1, 0),
         br_cond(IA64_BREAK_VECTOR + 0x30, IA64_BREAK_VECTOR + 0x40)),
        (IA64_BREAK_VECTOR + 0x40, 0x11, nop_m(), break_m(0x43), nop_m()),
        (IA64_BREAK_VECTOR + 0x50, 0x00, mov_m_cr_gr(4, 16), nop_i(),
         nop_i()),
        (IA64_BREAK_VECTOR + 0x60, 0x10, mov_m_cr_gr(5, 19), nop_i(),
         br_cond(IA64_BREAK_VECTOR + 0x60, IA64_BREAK_VECTOR + 0xa0)),
        (IA64_BREAK_VECTOR + 0xa0, 0x10, nop_m(), nop_i(),
         br_cond(IA64_BREAK_VECTOR + 0xa0, IA64_BREAK_VECTOR + 0xa0)),
        (IA64_BREAK_VECTOR + 0x70, 0x00, mov_m_gr_cr(0, 16), nop_i(),
         nop_i()),
        (IA64_BREAK_VECTOR + 0x80, 0x00, mov_m_gr_cr(3, 19), nop_i(),
         nop_i()),
        (IA64_BREAK_VECTOR + 0x90, 0x10, nop_m(), nop_i(),
         rfi_b()),
    ], {
        "ip": IA64_BREAK_VECTOR + 0xa0,
        "exception": IA64_EXCP_NONE,
        "r4": 0,
        "r5": IA64_BREAK_VECTOR + 0x50,
        "r6": 1,
    }, entry=0x10)

test_nested_exception_keeps_handler_interruption_state = require_registers(
    "nested_exception_keeps_handler_interruption_state", [
        (0x10, *movl_mlx(2, 1 << 13)),
        (0x20, *movl_mlx(3, IA64_BREAK_VECTOR + 0x180)),
        (0x30, *movl_mlx(20, 1 << 41)),
        (0x40, 0x00, mov_m_gr_cr(20, 17), nop_i(), nop_i()),
        (0x50, *movl_mlx(20, 0x12345600 | (16 << 2))),
        (0x60, 0x00, mov_m_gr_cr(20, 21), nop_i(), nop_i()),
        (0x70, *movl_mlx(20, 0x9999aaaabbbbcccc)),
        (0x80, 0x00, mov_m_gr_cr(20, 25), nop_i(), nop_i()),
        (0x90, 0x10, mov_gr_psr_full(2), nop_i(),
         br_cond(0x90, 0xa0)),
        (0xa0, 0x00, srlz_d(), nop_i(), nop_i()),
        (0xb0, 0x11, nop_m(), break_m(0x42), nop_m()),
        (IA64_BREAK_VECTOR, 0x08, nop_m(), cmp4_eq_imm(1, 2, 0, 31),
         nop_i()),
        (IA64_BREAK_VECTOR + 0x10, 0x10, nop_m(), nop_i(),
         br_cond(IA64_BREAK_VECTOR + 0x10, IA64_BREAK_VECTOR + 0x30, qp=1)),
        (IA64_BREAK_VECTOR + 0x20, 0x10, nop_m(), nop_i(),
         br_cond(IA64_BREAK_VECTOR + 0x20, IA64_BREAK_VECTOR + 0x100, qp=2)),
        (IA64_BREAK_VECTOR + 0x30, 0x10, mov_gr_psr_full(2), adds(31, 1, 0),
         br_cond(IA64_BREAK_VECTOR + 0x30, IA64_BREAK_VECTOR + 0x40)),
        (IA64_BREAK_VECTOR + 0x40, 0x00, srlz_d(), nop_i(), nop_i()),
        (IA64_BREAK_VECTOR + 0x50, 0x11, nop_m(), break_m(0x43), nop_m()),
        (IA64_BREAK_VECTOR + 0x100, *movl_mlx(20, 1 << 42)),
        (IA64_BREAK_VECTOR + 0x110, 0x00, mov_m_gr_cr(20, 17), nop_i(),
         nop_i()),
        (IA64_BREAK_VECTOR + 0x120,
         *movl_mlx(20, 0x87654300 | (20 << 2))),
        (IA64_BREAK_VECTOR + 0x130, 0x00, mov_m_gr_cr(20, 21), nop_i(),
         nop_i()),
        (IA64_BREAK_VECTOR + 0x140, *movl_mlx(20, 0xdeadbeef10002500)),
        (IA64_BREAK_VECTOR + 0x150, 0x00, mov_m_gr_cr(20, 25), nop_i(),
         nop_i()),
        (IA64_BREAK_VECTOR + 0x160, 0x00, mov_m_gr_cr(0, 16), nop_i(),
         nop_i()),
        (IA64_BREAK_VECTOR + 0x170, 0x10, mov_m_gr_cr(3, 19), nop_i(),
         rfi_b()),
        (IA64_BREAK_VECTOR + 0x180, 0x00, mov_m_cr_gr(4, 17), nop_i(),
         nop_i()),
        (IA64_BREAK_VECTOR + 0x190, 0x00, mov_m_cr_gr(5, 21), nop_i(),
         nop_i()),
        (IA64_BREAK_VECTOR + 0x1a0, 0x00, mov_m_cr_gr(6, 25), nop_i(),
         nop_i()),
        (IA64_BREAK_VECTOR + 0x1b0, 0x10, nop_m(), nop_i(),
         br_cond(IA64_BREAK_VECTOR + 0x1b0, IA64_BREAK_VECTOR + 0x1b0)),
    ], {
        "ip": IA64_BREAK_VECTOR + 0x1b0,
        "exception": IA64_EXCP_NONE,
        "r4": (1 << 42),
        "r5": 0x87654300 | (20 << 2),
        "r6": 0xdeadbeef10002500,
    }, entry=0x10)

test_exception_illegal = require_exception("exception_illegal", [
    (0x10, 0x11, nop_m(),
     illegal_m(), nop_m()),
], IA64_EXCP_ILLEGAL, fault_ip=0x10)

test_exception_illegal_enters_general_vector = require_registers(
        "exception_illegal_enters_general_vector", [
        (0x10, *movl_mlx(2, 1 << 13)),
        (0x20, 0x10, mov_gr_psr_full(2), nop_i(),
         br_cond(0x20, 0x30)),
        (0x30, 0x00, srlz_d(), nop_i(), nop_i()),
        (0x40, 0x11, nop_m(), illegal_m(), nop_m()),
        (IA64_GENERAL_VECTOR, 0x00, mov_m_cr_gr(8, 17), nop_i(),
         nop_i()),
        (IA64_GENERAL_VECTOR + 0x10, 0x10, nop_m(), nop_i(),
         br_cond(IA64_GENERAL_VECTOR + 0x10,
                 IA64_GENERAL_VECTOR + 0x10)),
    ], {
        "ip": IA64_GENERAL_VECTOR + 0x10,
        "exception": IA64_EXCP_NONE,
        "r8": (1 << 41),
    }, entry=0x10)

test_exception_reserved_template = require_exception(
    "exception_reserved_template",
    [(0x10, 0x1f, nop_m(), nop_m(), nop_m())],
    IA64_EXCP_RESERVED_TEMPLATE, fault_ip=0x10,
)

test_exception_unaligned = require_exception(
    "exception_unaligned",
    [
        (0x10, 0x00, nop_m(), adds(3, 0xff9, 0), nop_i()),
        (0x20, 0x00, ld8(4, 3), nop_i(), nop_i()),
    ],
    IA64_EXCP_UNALIGNED, fault_ip=0x20,
)

test_exception_unaligned_sets_ifa_isr = require_registers(
    "exception_unaligned_sets_ifa_isr",
    [
        (0x10, 0x00, nop_m(), adds(3, 0xff9, 0), nop_i()),
        (0x20, 0x00, ssm(1 << 13), nop_i(), nop_i()),
        (0x30, 0x00, srlz_d(), nop_i(), nop_i()),
        (0x40, 0x00, ld8(4, 3), nop_i(), nop_i()),
        (0x5a00, 0x00, mov_m_cr_gr(14, 20), nop_i(), nop_i()),
        (0x5a10, 0x00, mov_m_cr_gr(15, 17), nop_i(), nop_i()),
        (0x5a20, 0x10, nop_m(), nop_i(), br_cond(0x5a20, 0x5a20)),
    ],
    {"ip": 0x5a20, "r14": 0xff9, "r15": IA64_ISR_R},
)

# SDM Vol. 2 Table 8-1 specifies no CR.IFA write when PSR.ic is clear.
test_exception_unaligned_ic0_preserves_ifa = require_registers(
    "exception_unaligned_ic0_preserves_ifa",
    [
        (0x10, *movl_mlx(19, 0x1122334455667788)),
        (0x20, 0x00, mov_m_gr_cr(19, 20), adds(3, 0xff9, 0), nop_i()),
        (0x30, 0x00, ld8(4, 3), nop_i(), nop_i()),
        (IA64_UNALIGNED_VECTOR, 0x00, mov_m_cr_gr(14, 20),
         nop_i(), nop_i()),
        (IA64_UNALIGNED_VECTOR + 0x10, 0x00, mov_m_cr_gr(15, 17),
         nop_i(), nop_i()),
        (IA64_UNALIGNED_VECTOR + 0x20, 0x10, nop_m(), nop_i(),
         br_cond(IA64_UNALIGNED_VECTOR + 0x20,
                 IA64_UNALIGNED_VECTOR + 0x20)),
    ],
    {
        "ip": IA64_UNALIGNED_VECTOR + 0x20,
        "r14": 0x1122334455667788,
        "r15": IA64_ISR_R | IA64_ISR_NI,
    },
)

test_exception_unaligned_slot1_uses_psr_ri = require_registers(
    "exception_unaligned_slot1_uses_psr_ri",
    [
        (0x10, 0x00, nop_m(), adds(3, 0xff9, 0), nop_i()),
        (0x20, 0x00, ssm(1 << 13), nop_i(), nop_i()),
        (0x30, 0x00, srlz_d(), nop_i(), nop_i()),
        (0x40, 0x08, nop_m(), ld8(4, 3), nop_i()),
        (IA64_UNALIGNED_VECTOR, 0x00, mov_m_cr_gr(14, 20),
         nop_i(), nop_i()),
        (IA64_UNALIGNED_VECTOR + 0x10, 0x00, mov_m_cr_gr(15, 17),
         nop_i(), nop_i()),
        (IA64_UNALIGNED_VECTOR + 0x20, 0x00, mov_m_cr_gr(16, 19),
         nop_i(), nop_i()),
        (IA64_UNALIGNED_VECTOR + 0x30, 0x10, nop_m(), nop_i(),
         br_cond(IA64_UNALIGNED_VECTOR + 0x30,
                 IA64_UNALIGNED_VECTOR + 0x30)),
    ],
    {
        "ip": IA64_UNALIGNED_VECTOR + 0x30,
        "r14": 0xff9,
        "r15": IA64_ISR_R | (1 << IA64_ISR_EI_SHIFT),
        "r16": 0x40,
    },
)

# A misaligned access with PSR.ac clear that stays within a 4 KiB page must
# NOT fault (SDM Vol.2 5.5.4): r3 = 0x301 (misaligned for ld8, 0x301+8 = 0x309
# does not cross the page), so the ld8 completes and reads the 8 bytes at
# 0x301.  Reset PSR is 0 (physical mode, PSR.ac clear), so this exercises the
# translation-time PSR.ac-clear alignment path directly.
test_unaligned_ac_clear_no_page_cross_loads = require_registers(
    "unaligned_ac_clear_no_page_cross_loads",
    [
        (0x10, 0x00, addl(3, 0x301, 0), nop_i(), nop_i()),
        (0x20, 0x00, ld8(9, 3), nop_i(), nop_i()),
        (0x30, 0x10, nop_m(), nop_i(), br_cond(0x30, 0x30)),
        raw_bundle(0x300, 0x0807060504030201, 0x1817161514131211),
    ],
    {"ip": 0x30, "r9": 0x1108070605040302},
)

# The same misaligned, non-page-crossing access DOES fault when PSR.ac is set.
test_unaligned_ac_set_no_page_cross_faults = require_exception(
    "unaligned_ac_set_no_page_cross_faults",
    [
        (0x10, 0x00, ssm(IA64_PSR_AC), nop_i(), nop_i()),
        (0x20, 0x00, srlz_d(), nop_i(), nop_i()),
        (0x30, 0x00, addl(3, 0x301, 0), nop_i(), nop_i()),
        (0x40, 0x00, ld8(4, 3), nop_i(), nop_i()),
        raw_bundle(0x300, 0x00, 0x00),
    ],
    IA64_EXCP_UNALIGNED, fault_ip=0x40,
)

test_counted_self_loop_fault_has_slot1_ri = require_registers(
    "counted_self_loop_fault_has_slot1_ri", [
        *dtr_setup_bundles(0x10, HIGH_TR_BASE, 0x400000),
        (0x70, *movl_mlx(3, HIGH_TR_BASE + 0xfff8)),
        (0x80, 0x00, adds(8, 1, 0), nop_i(), nop_i()),
        (0x90, 0x02, nop_m(), mov_lc_gr(8), nop_i()),
        (0xa0, *movl_mlx(19, IA64_PSR_IC | IA64_PSR_DT)),
        (0xb0, 0x00, mov_gr_psr_full(19), nop_i(), nop_i()),
        (0xc0, 0x19, nop_m(), st8_postinc(3, 0, 8),
         br_cloop(0xc0, 0xc0)),
        (IA64_ALT_DTLB_VECTOR, 0x02, mov_m_cr_gr(31, 16),
         extr_u(31, 31, 41, 2), nop_i()),
        (IA64_ALT_DTLB_VECTOR + 0x10, 0x10, nop_m(), nop_i(),
         br_cond(IA64_ALT_DTLB_VECTOR + 0x10,
                 IA64_ALT_DTLB_VECTOR + 0x10)),
    ], {
        "ip": IA64_ALT_DTLB_VECTOR + 0x10,
        "exception": IA64_EXCP_NONE,
        "r31": 1,
    }, entry=0x10)

test_ia32_instruction_intercept_records_prefix_and_opcode = \
    require_registers(
        "ia32_instruction_intercept_records_prefix_and_opcode", [
            *ia32_environment_bundles(0x700, 0x10),
            (0x10, *movl_mlx(2, IA64_PSR_IC)),
            (0x20, 0x00, mov_gr_psr_full(2), nop_i(), nop_i()),
            (0x30, 0x00, srlz_d(), nop_i(), nop_i()),
            (0x40, *movl_mlx(8, 0x100)),
            (0x50, 0x00, nop_m(), mov_br_gr(7, 8), nop_i()),
            (0x60, 0x10, nop_m(), nop_i(),
             br_indirect(7, btype=1)),
            ia32_bundle(0x100, bytes.fromhex("66 0f 06")),
            (IA64_IA32_INTERCEPT_VECTOR, 0x00,
             mov_m_cr_gr(8, 19), nop_i(), nop_i()),
            (IA64_IA32_INTERCEPT_VECTOR + 0x10, 0x00,
             mov_m_cr_gr(9, 17), nop_i(), nop_i()),
            (IA64_IA32_INTERCEPT_VECTOR + 0x20, 0x00,
             mov_m_cr_gr(10, 24), nop_i(), nop_i()),
            (IA64_IA32_INTERCEPT_VECTOR + 0x30, 0x00,
             mov_m_cr_gr(11, 22), nop_i(), nop_i()),
            (IA64_IA32_INTERCEPT_VECTOR + 0x40, 0x10,
             nop_m(), nop_i(),
             br_cond(IA64_IA32_INTERCEPT_VECTOR + 0x40,
                     IA64_IA32_INTERCEPT_VECTOR + 0x40)),
        ], {
            "ip": IA64_IA32_INTERCEPT_VECTOR + 0x40,
            "r8": 0x100,
            "r9": 0x1002,
            "r10": 0x060f,
            "r11": 0x100,
            "exception": IA64_EXCP_NONE,
        }, entry=0x700, cpu="madison")

test_ia32_illegal_x87_opcode_intercepts_with_cr0_em = require_registers(
    "ia32_illegal_x87_opcode_intercepts_with_cr0_em", [
        *ia32_environment_bundles(0x700, 0x10),
        # CFLG.em must not hide an illegal x87 opcode behind DNA.
        (0x10, *movl_mlx(3, 1 << 2)),
        (0x20, 0x00, mov_m_gr_ar(3, 27), nop_i(), nop_i()),
        (0x30, *movl_mlx(2, IA64_PSR_IC)),
        (0x40, 0x00, mov_gr_psr_full(2), nop_i(), nop_i()),
        (0x50, 0x00, srlz_d(), nop_i(), nop_i()),
        (0x60, *movl_mlx(8, 0x100)),
        (0x70, 0x00, nop_m(), mov_br_gr(7, 8), nop_i()),
        (0x80, 0x10, nop_m(), nop_i(), br_indirect(7, btype=1)),
        ia32_bundle(0x100, bytes.fromhex("d9 d1")),  # illegal D9 /2, rm=1
        (IA64_IA32_INTERCEPT_VECTOR, 0x00,
         mov_m_cr_gr(8, 19), nop_i(), nop_i()),
        (IA64_IA32_INTERCEPT_VECTOR + 0x10, 0x00,
         mov_m_cr_gr(9, 17), nop_i(), nop_i()),
        (IA64_IA32_INTERCEPT_VECTOR + 0x20, 0x00,
         mov_m_cr_gr(10, 24), nop_i(), nop_i()),
        (IA64_IA32_INTERCEPT_VECTOR + 0x30, 0x00,
         mov_m_cr_gr(11, 22), nop_i(), nop_i()),
        (IA64_IA32_INTERCEPT_VECTOR + 0x40, 0x10,
         nop_m(), nop_i(),
         br_cond(IA64_IA32_INTERCEPT_VECTOR + 0x40,
                 IA64_IA32_INTERCEPT_VECTOR + 0x40)),
    ], {
        "ip": IA64_IA32_INTERCEPT_VECTOR + 0x40,
        "r8": 0x100,
        "r9": 0,
        "r10": 0xd1d9,
        "r11": 0x100,
        "exception": IA64_EXCP_NONE,
    }, entry=0x700, cpu="madison")

test_ia32_fisttp_intercepts_before_cr0_em = require_registers(
    "ia32_fisttp_intercepts_before_cr0_em", [
        *ia32_environment_bundles(
            0x700, 0x10, csd=IA32_TEST_CSD | (1 << 62)),
        # FISTTP post-dates Madison; CFLG.em must not turn it into DNA.
        (0x10, *movl_mlx(3, 1 << 2)),
        (0x20, 0x00, mov_m_gr_ar(3, 27), nop_i(), nop_i()),
        (0x30, *movl_mlx(2, IA64_PSR_IC)),
        (0x40, 0x00, mov_gr_psr_full(2), nop_i(), nop_i()),
        (0x50, 0x00, srlz_d(), nop_i(), nop_i()),
        (0x60, *movl_mlx(8, 0x100)),
        (0x70, 0x00, nop_m(), mov_br_gr(7, 8), nop_i()),
        (0x80, 0x10, nop_m(), nop_i(), br_indirect(7, btype=1)),
        ia32_bundle(0x100, bytes.fromhex(
            "db 0d 00 02 00 00")),  # fisttp dword ptr [0x200]
        raw_bundle(0x200, 0, 0),
        (IA64_IA32_INTERCEPT_VECTOR, 0x00,
         mov_m_cr_gr(8, 19), nop_i(), nop_i()),
        (IA64_IA32_INTERCEPT_VECTOR + 0x10, 0x00,
         mov_m_cr_gr(9, 17), nop_i(), nop_i()),
        (IA64_IA32_INTERCEPT_VECTOR + 0x20, 0x00,
         mov_m_cr_gr(10, 24), nop_i(), nop_i()),
        (IA64_IA32_INTERCEPT_VECTOR + 0x30, 0x00,
         mov_m_cr_gr(11, 22), nop_i(), nop_i()),
        (IA64_IA32_INTERCEPT_VECTOR + 0x40, 0x10,
         nop_m(), nop_i(),
         br_cond(IA64_IA32_INTERCEPT_VECTOR + 0x40,
                 IA64_IA32_INTERCEPT_VECTOR + 0x40)),
    ], {
        "ip": IA64_IA32_INTERCEPT_VECTOR + 0x40,
        "r8": 0x100,
        "r9": (1 << 2) | (1 << 1),
        "r10": 0x02000ddb,
        "r11": 0x100,
        "exception": IA64_EXCP_NONE,
    }, entry=0x700, cpu="madison")

test_ia32_amd_prefetch_opcode_intercepts = require_registers(
    "ia32_amd_prefetch_opcode_intercepts", [
        *ia32_environment_bundles(0x700, 0x10),
        (0x10, *movl_mlx(2, IA64_PSR_IC)),
        (0x20, 0x00, mov_gr_psr_full(2), nop_i(), nop_i()),
        (0x30, 0x00, srlz_d(), nop_i(), nop_i()),
        (0x40, *movl_mlx(8, 0x100)),
        (0x50, 0x00, nop_m(), mov_br_gr(7, 8), nop_i()),
        (0x60, 0x10, nop_m(), nop_i(), br_indirect(7, btype=1)),
        ia32_bundle(0x100, bytes.fromhex(
            "0f 0d 06 00 02 "    # AMD 3DNow! prefetch [0x200]
            "0f b8 00 03")),     # jmpe 0x300 (must not execute)
        raw_bundle(0x200, 0, 0),
        (0x300, 0x10, nop_m(), nop_i(), br_cond(0x300, 0x300)),
        (IA64_IA32_INTERCEPT_VECTOR, 0x00,
         mov_m_cr_gr(8, 19), nop_i(), nop_i()),
        (IA64_IA32_INTERCEPT_VECTOR + 0x10, 0x00,
         mov_m_cr_gr(9, 17), nop_i(), nop_i()),
        (IA64_IA32_INTERCEPT_VECTOR + 0x20, 0x00,
         mov_m_cr_gr(10, 24), nop_i(), nop_i()),
        (IA64_IA32_INTERCEPT_VECTOR + 0x30, 0x00,
         mov_m_cr_gr(11, 22), nop_i(), nop_i()),
        (IA64_IA32_INTERCEPT_VECTOR + 0x40, 0x10,
         nop_m(), nop_i(),
         br_cond(IA64_IA32_INTERCEPT_VECTOR + 0x40,
                 IA64_IA32_INTERCEPT_VECTOR + 0x40)),
    ], {
        "ip": IA64_IA32_INTERCEPT_VECTOR + 0x40,
        "r8": 0x100,
        "r9": 0,
        "r10": 0x0d0f,
        "r11": 0x100,
        "exception": IA64_EXCP_NONE,
    }, entry=0x700, cpu="madison")

test_ia32_ldmxcsr_rejects_reserved_madison_bit = require_registers(
    "ia32_ldmxcsr_rejects_reserved_madison_bit", [
        *ia32_environment_bundles(
            0x700, 0x10, csd=IA32_TEST_CSD | (1 << 62)),
        (0x10, *movl_mlx(3, 0x1f80 << 32)),
        (0x20, 0x00, mov_m_gr_ar(3, 21), nop_i(), nop_i()),
        # Protected mode and CFLG.fxsr (IA-32 CR4.OSFXSR).
        (0x30, *movl_mlx(3, 1 | ((1 << 9) << 32))),
        (0x40, 0x00, mov_m_gr_ar(3, 27), nop_i(), nop_i()),
        (0x50, *movl_mlx(2, IA64_PSR_IC)),
        (0x60, 0x00, mov_gr_psr_full(2), nop_i(), nop_i()),
        (0x70, 0x00, srlz_d(), nop_i(), nop_i()),
        (0x80, *movl_mlx(8, 0x100)),
        (0x90, 0x00, nop_m(), mov_br_gr(7, 8), nop_i()),
        (0xa0, 0x10, nop_m(), nop_i(), br_indirect(7, btype=1)),
        ia32_bundle(0x100, bytes.fromhex(
            "0f ae 15 00 03 00 00")),  # ldmxcsr [0x300]
        raw_bundle(0x300, 0x1fc0, 0),   # DAZ (bit 6) is reserved on Madison.
        (IA64_IA32_EXCEPTION_VECTOR, 0x00,
         mov_m_cr_gr(20, 19), nop_i(), nop_i()),
        (IA64_IA32_EXCEPTION_VECTOR + 0x10, 0x00,
         mov_m_cr_gr(21, 17), nop_i(), nop_i()),
        (IA64_IA32_EXCEPTION_VECTOR + 0x20, 0x00,
         mov_m_cr_gr(22, 22), nop_i(), nop_i()),
        (IA64_IA32_EXCEPTION_VECTOR + 0x30, 0x00,
         mov_m_ar_gr(23, 21), nop_i(), nop_i()),
        (IA64_IA32_EXCEPTION_VECTOR + 0x40, 0x10,
         nop_m(), nop_i(),
         br_cond(IA64_IA32_EXCEPTION_VECTOR + 0x40,
                 IA64_IA32_EXCEPTION_VECTOR + 0x40)),
    ], {
        "ip": IA64_IA32_EXCEPTION_VECTOR + 0x40,
        "r20": 0x100,
        "r21": 13 << 16,
        "r22": 0x100,
        "r23": 0x1f80 << 32,
        "exception": IA64_EXCP_NONE,
    }, entry=0x700, cpu="madison")

test_ia32_fxrstor_reserved_mxcsr_is_precise = require_registers(
    "ia32_fxrstor_reserved_mxcsr_is_precise", [
        *ia32_environment_bundles(
            0x700, 0x10, csd=IA32_TEST_CSD | (1 << 62)),
        (0x10, *movl_mlx(3, (0x1f80 << 32) | 0x037f)),
        (0x20, 0x00, mov_m_gr_ar(3, 21), nop_i(), nop_i()),
        (0x30, *movl_mlx(3, 0x55550000 | (3 << 11))),
        (0x40, 0x00, mov_m_gr_ar(3, 28), nop_i(), nop_i()),
        (0x50, *movl_mlx(
            3, (0x123 << 48) | (0x4567 << 32) | 0x89abcdef)),
        (0x60, 0x00, mov_m_gr_ar(3, 29), nop_i(), nop_i()),
        (0x70, *movl_mlx(3, (0x2468 << 32) | 0x76543210)),
        (0x80, 0x00, mov_m_gr_ar(3, 30), nop_i(), nop_i()),
        (0x90, *movl_mlx(3, 1 | ((1 << 9) << 32))),
        (0xa0, 0x00, mov_m_gr_ar(3, 27), nop_i(), nop_i()),
        (0xb0, *movl_mlx(2, IA64_PSR_IC)),
        (0xc0, 0x00, mov_gr_psr_full(2), nop_i(), nop_i()),
        (0xd0, 0x00, srlz_d(), nop_i(), nop_i()),
        (0xe0, *movl_mlx(8, 0x200)),
        (0xf0, 0x00, nop_m(), mov_br_gr(7, 8), nop_i()),
        (0x100, 0x10, nop_m(), nop_i(), br_indirect(7, btype=1)),
        ia32_bundle(0x200, bytes.fromhex(
            "0f ae 0d 00 03 00 00")),  # fxrstor [0x300]
        # A different FCW precedes an invalid MXCSR.  Neither may commit.
        raw_bundle(0x300, 0x0123, 0),
        raw_bundle(0x310, 0, 0x1fc0),
        (IA64_IA32_EXCEPTION_VECTOR, 0x00,
         mov_m_cr_gr(20, 19), nop_i(), nop_i()),
        (IA64_IA32_EXCEPTION_VECTOR + 0x10, 0x00,
         mov_m_cr_gr(21, 17), nop_i(), nop_i()),
        (IA64_IA32_EXCEPTION_VECTOR + 0x20, 0x00,
         mov_m_cr_gr(22, 22), nop_i(), nop_i()),
        (IA64_IA32_EXCEPTION_VECTOR + 0x30, 0x00,
         mov_m_ar_gr(23, 21), nop_i(), nop_i()),
        (IA64_IA32_EXCEPTION_VECTOR + 0x40, 0x00,
         mov_m_ar_gr(24, 28), nop_i(), nop_i()),
        (IA64_IA32_EXCEPTION_VECTOR + 0x50, 0x00,
         mov_m_ar_gr(25, 29), nop_i(), nop_i()),
        (IA64_IA32_EXCEPTION_VECTOR + 0x60, 0x00,
         mov_m_ar_gr(26, 30), nop_i(), nop_i()),
        (IA64_IA32_EXCEPTION_VECTOR + 0x70, 0x10,
         nop_m(), nop_i(),
         br_cond(IA64_IA32_EXCEPTION_VECTOR + 0x70,
                 IA64_IA32_EXCEPTION_VECTOR + 0x70)),
    ], {
        "ip": IA64_IA32_EXCEPTION_VECTOR + 0x70,
        "r20": 0x200,
        "r21": 13 << 16,
        "r22": 0x200,
        "r23": (0x1f80 << 32) | 0x037f,
        "r24": 0x55550000 | (3 << 11),
        "r25": (0x123 << 48) | (0x4567 << 32) | 0x89abcdef,
        "r26": (0x2468 << 32) | 0x76543210,
        "exception": IA64_EXCP_NONE,
    }, entry=0x700, cpu="madison")

test_ia32_ibr_instruction_breakpoint_fault = require_registers(
    "ia32_ibr_instruction_breakpoint_fault", [
        *ia32_environment_bundles(0x700, 0x10),
        (0x10, *movl_mlx(4, 0)),
        (0x20, *movl_mlx(5, 0x100)),
        (0x30, 0x00, mov_ibr_indexed_write(4, 5), nop_i(), nop_i()),
        (0x40, 0x00, nop_m(), adds(4, 1, 0), nop_i()),
        (0x50, *movl_mlx(5, 0x81000000ffffffff)),
        (0x60, 0x00, mov_ibr_indexed_write(4, 5), nop_i(), nop_i()),
        (0x70, 0x00, srlz_i(), nop_i(), nop_i()),
        (0x80, *movl_mlx(2, IA64_PSR_IC | IA64_PSR_DB)),
        (0x90, 0x00, mov_gr_psr_full(2), nop_i(), nop_i()),
        (0xa0, 0x00, srlz_d(), nop_i(), nop_i()),
        (0xb0, *movl_mlx(6, 0x100)),
        (0xc0, 0x00, nop_m(), mov_br_gr(7, 6), nop_i()),
        (0xd0, 0x10, nop_m(), nop_i(), br_indirect(7, btype=1)),
        ia32_bundle(0x100, b"\x90"),
        (IA64_IA32_EXCEPTION_VECTOR, 0x00,
         mov_m_cr_gr(8, 19), nop_i(), nop_i()),
        (IA64_IA32_EXCEPTION_VECTOR + 0x10, 0x00,
         mov_m_cr_gr(9, 22), nop_i(), nop_i()),
        (IA64_IA32_EXCEPTION_VECTOR + 0x20, 0x00,
         mov_m_cr_gr(10, 17), nop_i(), nop_i()),
        (IA64_IA32_EXCEPTION_VECTOR + 0x30, 0x10,
         nop_m(), nop_i(),
         br_cond(IA64_IA32_EXCEPTION_VECTOR + 0x30,
                 IA64_IA32_EXCEPTION_VECTOR + 0x30)),
    ], {
        "ip": IA64_IA32_EXCEPTION_VECTOR + 0x30,
        "r8": 0x100,
        "r9": 0x100,
        "r10": (1 << 32) | (1 << 16),
        "exception": IA64_EXCP_NONE,
    }, entry=0x700, cpu="madison")

test_ia32_psr_ss_traps_after_one_instruction_and_resumes = \
    require_registers(
        "ia32_psr_ss_traps_after_one_instruction_and_resumes", [
            *ia32_environment_bundles(
                0x700, 0x10,
                csd=IA32_TEST_CSD | (1 << 62),
                ssd=IA32_TEST_DSD | (1 << 62)),
            (0x10, *movl_mlx(12, 0x400)),
            (0x20, *movl_mlx(
                2, IA64_PSR_IC | IA64_PSR_IS | IA64_PSR_SS)),
            (0x30, *movl_mlx(3, 0x100)),
            *rfi_to_gr(0x40, 2, 3),
            # PUSH and LEA use paths that are not covered by the IA-64
            # completion hook.  PSR.ss must still stop after PUSH.
            ia32_bundle(0x100, bytes.fromhex(
                "55 "           # push ebp
                "8d 6c 24 90 "  # lea ebp,[esp-0x70]
                "0f 0b")),      # UD2 intercept
            (IA64_IA32_EXCEPTION_VECTOR, 0x00,
             mov_m_cr_gr(20, 16), adds(4, 1, 4), nop_i()),
            (IA64_IA32_EXCEPTION_VECTOR + 0x10, 0x00,
             mov_m_cr_gr(5, 19), dep(20, 0, 20, 40, 1), nop_i()),
            (IA64_IA32_EXCEPTION_VECTOR + 0x20, 0x00,
             mov_m_cr_gr(6, 22), nop_i(), nop_i()),
            (IA64_IA32_EXCEPTION_VECTOR + 0x30, 0x00,
             mov_m_gr_cr(20, 16), nop_i(), nop_i()),
            (IA64_IA32_EXCEPTION_VECTOR + 0x40, 0x10,
             nop_m(), nop_i(), rfi_b()),
            (IA64_IA32_INTERCEPT_VECTOR, 0x00,
             mov_m_cr_gr(7, 19), nop_i(), nop_i()),
            (IA64_IA32_INTERCEPT_VECTOR + 0x10, 0x10,
             nop_m(), nop_i(),
             br_cond(IA64_IA32_INTERCEPT_VECTOR + 0x10,
                     IA64_IA32_INTERCEPT_VECTOR + 0x10)),
        ], {
            "ip": IA64_IA32_INTERCEPT_VECTOR + 0x10,
            "r4": 1,
            "r5": 0x101,
            "r6": 0x101,
            "r7": 0x105,
            "r12": 0x3fc,
            "r13": 0x38c,
            "exception": IA64_EXCP_NONE,
        }, entry=0x700, cpu="madison")

test_ia32_eflag_tf_traps_after_one_instruction_and_resumes = \
    require_registers(
        "ia32_eflag_tf_traps_after_one_instruction_and_resumes", [
            *ia32_environment_bundles(
                0x700, 0x10,
                csd=IA32_TEST_CSD | (1 << 62),
                ssd=IA32_TEST_DSD | (1 << 62)),
            (0x10, *movl_mlx(12, 0x400)),
            (0x20, *movl_mlx(3, (1 << 8) | 2)),
            (0x30, 0x00, mov_m_gr_ar(3, 24), nop_i(), nop_i()),
            (0x40, *movl_mlx(2, IA64_PSR_IC | IA64_PSR_IS)),
            (0x50, *movl_mlx(3, 0x100)),
            *rfi_to_gr(0x60, 2, 3),
            ia32_bundle(0x100, bytes.fromhex(
                "55 "
                "8d 6c 24 90 "
                "0f 0b")),
            (IA64_IA32_EXCEPTION_VECTOR, 0x00,
             mov_m_cr_gr(5, 19), adds(4, 1, 4), nop_i()),
            (IA64_IA32_EXCEPTION_VECTOR + 0x10, 0x00,
             mov_m_cr_gr(6, 22), adds(20, 2, 0), nop_i()),
            (IA64_IA32_EXCEPTION_VECTOR + 0x20, 0x00,
             mov_m_gr_ar(20, 24), nop_i(), nop_i()),
            (IA64_IA32_EXCEPTION_VECTOR + 0x30, 0x10,
             nop_m(), nop_i(), rfi_b()),
            (IA64_IA32_INTERCEPT_VECTOR, 0x00,
             mov_m_cr_gr(7, 19), nop_i(), nop_i()),
            (IA64_IA32_INTERCEPT_VECTOR + 0x10, 0x10,
             nop_m(), nop_i(),
             br_cond(IA64_IA32_INTERCEPT_VECTOR + 0x10,
                     IA64_IA32_INTERCEPT_VECTOR + 0x10)),
        ], {
            "ip": IA64_IA32_INTERCEPT_VECTOR + 0x10,
            "r4": 1,
            "r5": 0x101,
            "r6": 0x101,
            "r7": 0x105,
            "r12": 0x3fc,
            "r13": 0x38c,
            "exception": IA64_EXCP_NONE,
        }, entry=0x700, cpu="madison")

test_ia32_flag_writeback_does_not_enable_single_step = \
    require_registers(
        "ia32_flag_writeback_does_not_enable_single_step", [
            *ia32_environment_bundles(
                0x700, 0x10,
                csd=IA32_TEST_CSD | (1 << 62),
                ssd=IA32_TEST_DSD | (1 << 62)),
            (0x10, *movl_mlx(12, 0x400)),
            (0x20, *movl_mlx(2, IA64_PSR_IC | IA64_PSR_IS)),
            (0x30, *movl_mlx(3, 0x100)),
            *rfi_to_gr(0x40, 2, 3),
            # This loader-style prologue used to copy a SUB operand into
            # EFLAGS and synthesize EFLAG.tf.
            ia32_bundle(0x100, bytes.fromhex(
                "55 "                 # push ebp
                "8d 6c 24 90 "        # lea ebp,[esp-0x70]
                "81 ec 64 01 00 00 "  # sub esp,0x164
                "0f 0b")),            # UD2 intercept
            (IA64_IA32_EXCEPTION_VECTOR, 0x10,
             nop_m(), nop_i(),
             br_cond(IA64_IA32_EXCEPTION_VECTOR,
                     IA64_IA32_EXCEPTION_VECTOR)),
            (IA64_IA32_INTERCEPT_VECTOR, 0x00,
             mov_m_ar_gr(4, 24), nop_i(), nop_i()),
            (IA64_IA32_INTERCEPT_VECTOR + 0x10, 0x00,
             mov_m_cr_gr(7, 19), nop_i(), nop_i()),
            (IA64_IA32_INTERCEPT_VECTOR + 0x20, 0x10,
             nop_m(), nop_i(),
             br_cond(IA64_IA32_INTERCEPT_VECTOR + 0x20,
                     IA64_IA32_INTERCEPT_VECTOR + 0x20)),
        ], {
            "ip": IA64_IA32_INTERCEPT_VECTOR + 0x20,
            "r4": 2,
            "r7": 0x10b,
            "r12": 0x298,
            "r13": 0x38c,
            "exception": IA64_EXCP_NONE,
        }, entry=0x700, cpu="madison")

test_ia32_pop_edx_does_not_intercept_as_pop_ss = require_registers(
    "ia32_pop_edx_does_not_intercept_as_pop_ss", [
        *ia32_environment_bundles(
            0x700, 0x10,
            csd=IA32_TEST_CSD | (1 << 62),
            ssd=IA32_TEST_DSD | (1 << 62)),
        (0x10, *movl_mlx(12, 0x400)),
        (0x20, *movl_mlx(2, IA64_PSR_IC | IA64_PSR_IS)),
        (0x30, *movl_mlx(3, 0x100)),
        *rfi_to_gr(0x40, 2, 3),
        # R_EDX and R_SS both have the numeric value 2.  Opcode operand types
        # must distinguish this ordinary POP from the POP SS system-flag trap.
        ia32_bundle(0x100, bytes.fromhex(
            "6a 5c "  # push 0x5c
            "5a "     # pop edx
            "0f 0b")),  # UD2 instruction intercept
        (IA64_IA32_INTERCEPT_VECTOR, 0x00,
         mov_m_cr_gr(20, 17), nop_i(), nop_i()),
        (IA64_IA32_INTERCEPT_VECTOR + 0x10, 0x00,
         mov_m_cr_gr(21, 19), nop_i(), nop_i()),
        (IA64_IA32_INTERCEPT_VECTOR + 0x20, 0x00,
         mov_m_cr_gr(22, 22), nop_i(), nop_i()),
        (IA64_IA32_INTERCEPT_VECTOR + 0x30, 0x00,
         mov_m_cr_gr(23, 24), nop_i(), nop_i()),
        (IA64_IA32_INTERCEPT_VECTOR + 0x40, 0x10,
         nop_m(), nop_i(),
         br_cond(IA64_IA32_INTERCEPT_VECTOR + 0x40,
                 IA64_IA32_INTERCEPT_VECTOR + 0x40)),
    ], {
        "ip": IA64_IA32_INTERCEPT_VECTOR + 0x40,
        "r10": 0x5c,
        "r12": 0x400,
        "r20": 0x6,
        "r21": 0x103,
        "r22": 0x103,
        "r23": 0x0b0f,
        "exception": IA64_EXCP_NONE,
    }, entry=0x700, cpu="madison")

test_ia32_pop_ss_system_flag_intercept_is_post_instruction = \
    require_registers(
        "ia32_pop_ss_system_flag_intercept_is_post_instruction", [
            *ia32_environment_bundles(
                0x700, 0x10,
                csd=IA32_TEST_CSD | (1 << 62),
                ssd=IA32_TEST_DSD | (1 << 62)),
            (0x10, *movl_mlx(12, 0x400)),
            (0x20, *movl_mlx(2, IA64_PSR_IC | IA64_PSR_IS)),
            (0x30, *movl_mlx(3, 0x100)),
            *rfi_to_gr(0x40, 2, 3),
            ia32_bundle(0x100, bytes.fromhex(
                "16 "     # push ss
                "17 "     # pop ss
                "0f 0b")),  # must not execute
            (IA64_IA32_INTERCEPT_VECTOR, 0x00,
             mov_m_cr_gr(20, 17), nop_i(), nop_i()),
            (IA64_IA32_INTERCEPT_VECTOR + 0x10, 0x00,
             mov_m_cr_gr(21, 19), nop_i(), nop_i()),
            (IA64_IA32_INTERCEPT_VECTOR + 0x20, 0x00,
             mov_m_cr_gr(22, 22), nop_i(), nop_i()),
            (IA64_IA32_INTERCEPT_VECTOR + 0x30, 0x00,
             mov_m_cr_gr(23, 24), nop_i(), nop_i()),
            (IA64_IA32_INTERCEPT_VECTOR + 0x40, 0x10,
             nop_m(), nop_i(),
             br_cond(IA64_IA32_INTERCEPT_VECTOR + 0x40,
                     IA64_IA32_INTERCEPT_VECTOR + 0x40)),
        ], {
            "ip": IA64_IA32_INTERCEPT_VECTOR + 0x40,
            "r12": 0x400,
            "r20": 0x2c000,
            "r21": 0x102,
            "r22": 0x101,
            "r23": 0x2,
            "exception": IA64_EXCP_NONE,
        }, entry=0x700, cpu="madison")

test_ia32_ibr_precedes_start_page_instruction_tlb_fault = require_registers(
    "ia32_ibr_precedes_start_page_instruction_tlb_fault", [
        *ia32_environment_bundles(
            0x900, 0x10,
            csd=IA32_TEST_CSD | (0xf << 48) | (1 << 62) | (1 << 63)),
        # Map a 64 KiB IVT window, leaving the IA-32 target unmapped.
        (0x10, *movl_mlx(18, 0x20000 | DTR_PTE_WB)),
        (0x20, *movl_mlx(19, 0x20000)),
        (0x30, 0x00, mov_m_gr_cr(19, 20),
         adds(21, 16 << 2, 0), nop_i()),
        (0x40, 0x00, mov_m_gr_cr(21, 21),
         adds(10, 5, 0), nop_i()),
        (0x50, 0x00, itr_i(10, 18), nop_i(), nop_i()),
        (0x60, *movl_mlx(4, 0)),
        (0x70, *movl_mlx(5, 0x300123)),
        (0x80, 0x00, mov_ibr_indexed_write(4, 5), nop_i(), nop_i()),
        (0x90, 0x00, nop_m(), adds(4, 1, 0), nop_i()),
        (0xa0, *movl_mlx(5, 0x81000000ffffffff)),
        (0xb0, 0x00, mov_ibr_indexed_write(4, 5), nop_i(), nop_i()),
        (0xc0, 0x00, srlz_i(), nop_i(), nop_i()),
        (0xd0, *movl_mlx(4, 0x20000)),
        (0xe0, 0x00, mov_m_gr_cr(4, 2), nop_i(), nop_i()),
        (0xf0, *movl_mlx(
            2, IA64_PSR_IC | IA64_PSR_IT | IA64_PSR_IS | IA64_PSR_DB)),
        (0x100, *movl_mlx(3, 0x300123)),
        *rfi_to_gr(0x110, 2, 3),
        (0x20000 + IA64_IA32_EXCEPTION_VECTOR, 0x00,
         mov_m_cr_gr(8, 19), nop_i(), nop_i()),
        (0x20000 + IA64_IA32_EXCEPTION_VECTOR + 0x10, 0x00,
         mov_m_cr_gr(9, 22), nop_i(), nop_i()),
        (0x20000 + IA64_IA32_EXCEPTION_VECTOR + 0x20, 0x00,
         mov_m_cr_gr(10, 17), nop_i(), nop_i()),
        (0x20000 + IA64_IA32_EXCEPTION_VECTOR + 0x30, 0x10,
         nop_m(), nop_i(),
         br_cond(0x20000 + IA64_IA32_EXCEPTION_VECTOR + 0x30,
                 0x20000 + IA64_IA32_EXCEPTION_VECTOR + 0x30)),
    ], {
        "ip": 0x20000 + IA64_IA32_EXCEPTION_VECTOR + 0x30,
        "r8": 0x300123,
        "r9": 0x300123,
        "r10": (1 << 32) | (1 << 16),
        "exception": IA64_EXCP_NONE,
    }, entry=0x900, cpu="madison")

test_ia32_dbr_overlap_raises_post_instruction_trap = require_registers(
    "ia32_dbr_overlap_raises_post_instruction_trap", [
        *ia32_environment_bundles(0x700, 0x10),
        (0x10, *movl_mlx(4, 0)),
        (0x20, *movl_mlx(5, 0x202)),
        (0x30, 0x00, mov_dbr_indexed_write(4, 5), nop_i(), nop_i()),
        (0x40, 0x00, nop_m(), adds(4, 1, 0), nop_i()),
        (0x50, *movl_mlx(5, 0x81000000ffffffff)),
        (0x60, 0x00, mov_dbr_indexed_write(4, 5), nop_i(), nop_i()),
        (0x70, *movl_mlx(2, IA64_PSR_IC | IA64_PSR_DB)),
        (0x80, 0x00, mov_gr_psr_full(2), nop_i(), nop_i()),
        (0x90, 0x00, srlz_d(), nop_i(), nop_i()),
        (0xa0, *movl_mlx(6, 0x100)),
        (0xb0, 0x00, nop_m(), mov_br_gr(7, 6), nop_i()),
        (0xc0, 0x10, nop_m(), nop_i(), br_indirect(7, btype=1)),
        ia32_bundle(0x100, bytes.fromhex(
            "66 a1 00 02")),  # mov eax,dword ptr [0x200]
        ia32_bundle(0x200, bytes.fromhex(
            "78 56 34 12 00 00 00 00 00 00 00 00 00 00 00 00")),
        (IA64_IA32_EXCEPTION_VECTOR, 0x00,
         mov_m_cr_gr(8, 19), nop_i(), nop_i()),
        (IA64_IA32_EXCEPTION_VECTOR + 0x10, 0x00,
         mov_m_cr_gr(9, 22), nop_i(), nop_i()),
        (IA64_IA32_EXCEPTION_VECTOR + 0x20, 0x00,
         mov_m_cr_gr(10, 17), nop_i(), nop_i()),
        (IA64_IA32_EXCEPTION_VECTOR + 0x30, 0x10,
         nop_m(), nop_i(),
         br_cond(IA64_IA32_EXCEPTION_VECTOR + 0x30,
                 IA64_IA32_EXCEPTION_VECTOR + 0x30)),
    ], {
        "ip": IA64_IA32_EXCEPTION_VECTOR + 0x30,
        "r8": 0x104,
        "r9": 0x104,
        "r10": (1 << 16) | (1 << 4),
        "exception": IA64_EXCP_NONE,
    }, entry=0x700, cpu="madison")

test_ia32_locked_rmw_triggers_read_data_breakpoint = require_registers(
    "ia32_locked_rmw_triggers_read_data_breakpoint", [
        *ia32_environment_bundles(0x700, 0x10),
        (0x10, *movl_mlx(4, 0)),
        (0x20, *movl_mlx(5, 0x208)),
        (0x30, 0x00, mov_dbr_indexed_write(4, 5), nop_i(), nop_i()),
        (0x40, 0x00, nop_m(), adds(4, 1, 0), nop_i()),
        # Read-only DBR0, enabled at PL0 with an exact 32-bit mask.
        (0x50, *movl_mlx(5, 0x81000000ffffffff)),
        (0x60, 0x00, mov_dbr_indexed_write(4, 5), nop_i(), nop_i()),
        (0x70, *movl_mlx(8, 0x11223344)),  # eax
        (0x80, *movl_mlx(2, IA64_PSR_IC | IA64_PSR_DB)),
        (0x90, 0x00, mov_gr_psr_full(2), nop_i(), nop_i()),
        (0xa0, 0x00, srlz_d(), nop_i(), nop_i()),
        (0xb0, *movl_mlx(6, 0x100)),
        (0xc0, 0x00, nop_m(), mov_br_gr(7, 6), nop_i()),
        (0xd0, 0x10, nop_m(), nop_i(), br_indirect(7, btype=1)),
        ia32_bundle(0x100, bytes.fromhex(
            "66 87 06 08 02 "  # xchg eax,dword ptr [0x208]
            "0f b8 00 03")),   # jmpe 0x300 if no debug trap
        ia32_bundle(0x200, bytes.fromhex(
            "00 00 00 00 00 00 00 00 78 56 34 12 00 00 00 00")),
        (0x300, 0x10, nop_m(), nop_i(), br_cond(0x300, 0x300)),
        (IA64_IA32_EXCEPTION_VECTOR, 0x00,
         mov_m_cr_gr(20, 19), nop_i(), nop_i()),
        (IA64_IA32_EXCEPTION_VECTOR + 0x10, 0x00,
         mov_m_cr_gr(21, 17), nop_i(), nop_i()),
        (IA64_IA32_EXCEPTION_VECTOR + 0x20, 0x00,
         mov_m_cr_gr(22, 22), nop_i(), nop_i()),
        (IA64_IA32_EXCEPTION_VECTOR + 0x30, 0x00,
         nop_m(), addl(3, 0x208, 0), nop_i()),
        (IA64_IA32_EXCEPTION_VECTOR + 0x40, 0x00,
         ld8(23, 3), nop_i(), nop_i()),
        (IA64_IA32_EXCEPTION_VECTOR + 0x50, 0x10,
         nop_m(), nop_i(),
         br_cond(IA64_IA32_EXCEPTION_VECTOR + 0x50,
                 IA64_IA32_EXCEPTION_VECTOR + 0x50)),
    ], {
        "ip": IA64_IA32_EXCEPTION_VECTOR + 0x50,
        "r8": 0x12345678,
        "r20": 0x105,
        "r21": (1 << 16) | (1 << 4),
        "r22": 0x105,
        "r23": 0x11223344,
        "exception": IA64_EXCP_NONE,
    }, entry=0x700, cpu="madison")

test_ia32_data_tlb_fault_precedes_alignment_check = require_registers(
    "ia32_data_tlb_fault_precedes_alignment_check", [
        *ia32_environment_bundles(0x700, 0x10),
        (0x10, *movl_mlx(
            2, IA64_PSR_IC | IA64_PSR_DT | IA64_PSR_AC)),
        (0x20, 0x00, mov_gr_psr_full(2), nop_i(), nop_i()),
        (0x30, 0x00, srlz_d(), nop_i(), nop_i()),
        (0x40, *movl_mlx(8, 0x100)),
        (0x50, 0x00, nop_m(), mov_br_gr(7, 8), nop_i()),
        (0x60, 0x10, nop_m(), nop_i(), br_indirect(7, btype=1)),
        ia32_bundle(0x100, bytes.fromhex(
            "a1 01 20")),  # mov ax,word ptr [0x2001]
        (IA64_ALT_DTLB_VECTOR, 0x00,
         mov_m_cr_gr(20, 19), nop_i(), nop_i()),
        (IA64_ALT_DTLB_VECTOR + 0x10, 0x00,
         mov_m_cr_gr(21, 17), nop_i(), nop_i()),
        (IA64_ALT_DTLB_VECTOR + 0x20, 0x00,
         mov_m_cr_gr(22, 20), nop_i(), nop_i()),
        (IA64_ALT_DTLB_VECTOR + 0x30, 0x00,
         mov_m_cr_gr(23, 22), nop_i(), nop_i()),
        (IA64_ALT_DTLB_VECTOR + 0x40, 0x10,
         nop_m(), nop_i(),
         br_cond(IA64_ALT_DTLB_VECTOR + 0x40,
                 IA64_ALT_DTLB_VECTOR + 0x40)),
    ], {
        "ip": IA64_ALT_DTLB_VECTOR + 0x40,
        "r20": 0x100,
        "r21": IA64_ISR_R,
        "r22": 0x2001,
        "r23": 0x100,
        "exception": IA64_EXCP_NONE,
    }, entry=0x700, cpu="madison")

test_ia32_data_access_wraps_at_4g = require_registers(
    "ia32_data_access_wraps_at_4g", [
        *ia32_environment_bundles(
            0x700, 0x10,
            csd=IA32_TEST_CSD | (1 << 62),
            dsd=IA32_TEST_DSD | (0xf << 48) | (1 << 63),
            ssd=IA32_TEST_DSD | (0xf << 48) | (1 << 63)),
        *dtr_setup_bundles(0x10, 0xfffff000, 0x400000,
                           page_shift=12, slot=5),
        *dtr_setup_bundles(0x70, 0, 0x401000,
                           page_shift=12, slot=6),
        (0xd0, *movl_mlx(
            2, IA64_PSR_IC | IA64_PSR_DT | IA64_PSR_IS)),
        (0xe0, *movl_mlx(3, 0x300)),
        *rfi_to_gr(0xf0, 2, 3),
        ia32_bundle(0x300, bytes.fromhex(
            "a1 ff ff ff ff "     # mov eax,dword ptr [0xffffffff]
            "0f b8 00 05 00 00")),  # jmpe 0x500
        ia32_bundle(0x400ff0, bytes.fromhex(
            "00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 11")),
        ia32_bundle(0x401000, bytes.fromhex("22 33 44")),
        (0x500, 0x10, nop_m(), nop_i(), br_cond(0x500, 0x500)),
    ], {
        "ip": 0x500,
        "r8": 0x44332211,
        "exception": IA64_EXCP_NONE,
    }, entry=0x700, cpu="madison")

test_ia32_data_breakpoint_matches_wrapped_byte_at_4g = require_registers(
    "ia32_data_breakpoint_matches_wrapped_byte_at_4g", [
        *ia32_environment_bundles(
            0x900, 0x10,
            csd=IA32_TEST_CSD | (1 << 62),
            dsd=IA32_TEST_DSD | (0xf << 48) | (1 << 63),
            ssd=IA32_TEST_DSD | (0xf << 48) | (1 << 63)),
        *dtr_setup_bundles(0x10, 0xfffff000, 0x400000,
                           page_shift=12, slot=5),
        *dtr_setup_bundles(0x70, 0, 0x401000,
                           page_shift=12, slot=6),
        (0xd0, *movl_mlx(4, 0)),
        (0xe0, *movl_mlx(5, 0)),
        (0xf0, 0x00, mov_dbr_indexed_write(4, 5), nop_i(), nop_i()),
        (0x100, 0x00, nop_m(), adds(4, 1, 0), nop_i()),
        (0x110, *movl_mlx(5, 0x81000000ffffffff)),
        (0x120, 0x00, mov_dbr_indexed_write(4, 5), nop_i(), nop_i()),
        (0x130, *movl_mlx(
            2, IA64_PSR_IC | IA64_PSR_DT | IA64_PSR_DB |
            IA64_PSR_IS)),
        (0x140, *movl_mlx(3, 0x300)),
        *rfi_to_gr(0x150, 2, 3),
        ia32_bundle(0x300, bytes.fromhex(
            "a1 ff ff ff ff "     # mov eax,dword ptr [0xffffffff]
            "0f b8 00 05 00 00")),  # jmpe 0x500 (must not execute)
        ia32_bundle(0x400ff0, bytes.fromhex(
            "00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 11")),
        ia32_bundle(0x401000, bytes.fromhex("22 33 44")),
        (IA64_IA32_EXCEPTION_VECTOR, 0x00,
         mov_m_cr_gr(20, 19), nop_i(), nop_i()),
        (IA64_IA32_EXCEPTION_VECTOR + 0x10, 0x00,
         mov_m_cr_gr(21, 22), nop_i(), nop_i()),
        (IA64_IA32_EXCEPTION_VECTOR + 0x20, 0x00,
         mov_m_cr_gr(22, 17), nop_i(), nop_i()),
        (IA64_IA32_EXCEPTION_VECTOR + 0x30, 0x10,
         nop_m(), nop_i(),
         br_cond(IA64_IA32_EXCEPTION_VECTOR + 0x30,
                 IA64_IA32_EXCEPTION_VECTOR + 0x30)),
        (0x500, 0x10, nop_m(), nop_i(), br_cond(0x500, 0x500)),
    ], {
        "ip": IA64_IA32_EXCEPTION_VECTOR + 0x30,
        "r20": 0x305,
        "r21": 0x305,
        "r22": (1 << 16) | (1 << 4),
        "exception": IA64_EXCP_NONE,
    }, entry=0x900, cpu="madison")

test_ia32_instruction_fetch_wraps_at_4g = require_registers(
    "ia32_instruction_fetch_wraps_at_4g", [
        *ia32_environment_bundles(
            0x900, 0x10,
            csd=IA32_TEST_CSD | (0xf << 48) | (1 << 62) | (1 << 63)),
        (0x10, *movl_mlx(18, 0x400000 | DTR_PTE_WB)),
        (0x20, *movl_mlx(19, 0xfffff000)),
        (0x30, 0x00, mov_m_gr_cr(19, 20),
         adds(21, FOUR_K_ITIR, 0), nop_i()),
        (0x40, 0x00, mov_m_gr_cr(21, 21), adds(10, 5, 0), nop_i()),
        (0x50, 0x00, itr_i(10, 18), nop_i(), nop_i()),
        (0x60, 0x00, srlz_i(), nop_i(), nop_i()),
        (0x70, *movl_mlx(18, 0x401000 | DTR_PTE_WB)),
        (0x80, *movl_mlx(19, 0)),
        (0x90, 0x00, mov_m_gr_cr(19, 20),
         adds(21, FOUR_K_ITIR, 0), nop_i()),
        (0xa0, 0x00, mov_m_gr_cr(21, 21), adds(10, 6, 0), nop_i()),
        (0xb0, 0x00, itr_i(10, 18), nop_i(), nop_i()),
        (0xc0, 0x00, srlz_i(), nop_i(), nop_i()),
        (0xd0, *movl_mlx(
            2, IA64_PSR_IC | IA64_PSR_IT | IA64_PSR_IS)),
        (0xe0, *movl_mlx(3, 0xfffffffd)),
        *rfi_to_gr(0xf0, 2, 3),
        ia32_bundle(0x400ff0, bytes.fromhex(
            "00 00 00 00 00 00 00 00 00 00 00 00 00 2e 2e 2e")),
        ia32_bundle(0x401000, bytes.fromhex(
            "b8 78 56 34 12 "  # mov eax,0x12345678 after CS overrides
            "0f b8 00 05 00 00")),  # jmpe 0x500
        (0x401500, 0x10, nop_m(), nop_i(), br_cond(0x500, 0x500)),
    ], {
        "ip": 0x500,
        "r8": 0x12345678,
        "exception": IA64_EXCP_NONE,
    }, entry=0x900, cpu="madison")

test_ia32_instruction_tlb_fault_records_unaligned_instruction_start = \
    require_registers(
        "ia32_instruction_tlb_fault_records_unaligned_instruction_start", [
            *ia32_environment_bundles(
                0x900, 0x10,
                csd=IA32_TEST_CSD | (0xf << 48) | (1 << 62) | (1 << 63)),
            (0x10, *movl_mlx(18, 0x20000 | DTR_PTE_WB)),
            (0x20, *movl_mlx(19, 0x20000)),
            (0x30, 0x00, mov_m_gr_cr(19, 20),
             adds(21, FOUR_K_ITIR, 0), nop_i()),
            (0x40, 0x00, mov_m_gr_cr(21, 21),
             adds(10, 5, 0), nop_i()),
            (0x50, 0x00, itr_i(10, 18), nop_i(), nop_i()),
            (0x60, 0x00, srlz_i(), nop_i(), nop_i()),
            (0x70, *movl_mlx(4, 0x20000)),
            (0x80, 0x00, mov_m_gr_cr(4, 2), nop_i(), nop_i()),
            (0x90, *movl_mlx(
                2, IA64_PSR_IC | IA64_PSR_IT | IA64_PSR_IS)),
            (0xa0, *movl_mlx(3, 0x200123)),
            *rfi_to_gr(0xb0, 2, 3),
            (0x20000 + IA64_ALT_ITLB_VECTOR, 0x00,
             mov_m_cr_gr(8, 19), nop_i(), nop_i()),
            (0x20000 + IA64_ALT_ITLB_VECTOR + 0x10, 0x00,
             mov_m_cr_gr(9, 22), nop_i(), nop_i()),
            (0x20000 + IA64_ALT_ITLB_VECTOR + 0x20, 0x00,
             mov_m_cr_gr(10, 20), nop_i(), nop_i()),
            (0x20000 + IA64_ALT_ITLB_VECTOR + 0x30, 0x00,
             mov_m_cr_gr(11, 17), nop_i(), nop_i()),
            (0x20000 + IA64_ALT_ITLB_VECTOR + 0x40, 0x10,
             nop_m(), nop_i(),
             br_cond(0x20000 + IA64_ALT_ITLB_VECTOR + 0x40,
                     0x20000 + IA64_ALT_ITLB_VECTOR + 0x40)),
        ], {
            "ip": 0x20000 + IA64_ALT_ITLB_VECTOR + 0x40,
            "r8": 0x200123,
            "r9": 0x200123,
            "r10": 0x200120,
            "r11": IA64_ISR_X,
            "exception": IA64_EXCP_NONE,
        }, entry=0x900, cpu="madison")

test_ia32_cross_page_instruction_tlb_fault_records_instruction_start = \
    require_registers(
        "ia32_cross_page_instruction_tlb_fault_records_instruction_start", [
            *ia32_environment_bundles(
                0x900, 0x10,
                csd=IA32_TEST_CSD | (0xf << 48) | (1 << 62) | (1 << 63)),
            (0x10, *movl_mlx(18, 0x400000 | DTR_PTE_WB)),
            (0x20, *movl_mlx(19, 0x201000)),
            (0x30, 0x00, mov_m_gr_cr(19, 20),
             adds(21, FOUR_K_ITIR, 0), nop_i()),
            (0x40, 0x00, mov_m_gr_cr(21, 21),
             adds(10, 5, 0), nop_i()),
            (0x50, 0x00, itr_i(10, 18), nop_i(), nop_i()),
            (0x60, 0x00, srlz_i(), nop_i(), nop_i()),
            (0x70, *movl_mlx(18, 0x20000 | DTR_PTE_WB)),
            (0x80, *movl_mlx(19, 0x20000)),
            (0x90, 0x00, mov_m_gr_cr(19, 20),
             adds(21, FOUR_K_ITIR, 0), nop_i()),
            (0xa0, 0x00, mov_m_gr_cr(21, 21),
             adds(10, 6, 0), nop_i()),
            (0xb0, 0x00, itr_i(10, 18), nop_i(), nop_i()),
            (0xc0, 0x00, srlz_i(), nop_i(), nop_i()),
            (0xd0, *movl_mlx(4, 0x20000)),
            (0xe0, 0x00, mov_m_gr_cr(4, 2), nop_i(), nop_i()),
            (0xf0, *movl_mlx(
                2, IA64_PSR_IC | IA64_PSR_IT | IA64_PSR_IS)),
            (0x100, *movl_mlx(3, 0x201ffb)),
            *rfi_to_gr(0x110, 2, 3),
            ia32_bundle(0x400ff0, bytes.fromhex(
                "00 00 00 00 00 00 00 00 00 00 00 90 90 90 90 0f")),
            (0x20000 + IA64_ALT_ITLB_VECTOR, 0x00,
             mov_m_cr_gr(8, 19), nop_i(), nop_i()),
            (0x20000 + IA64_ALT_ITLB_VECTOR + 0x10, 0x00,
             mov_m_cr_gr(9, 22), nop_i(), nop_i()),
            (0x20000 + IA64_ALT_ITLB_VECTOR + 0x20, 0x00,
             mov_m_cr_gr(10, 20), nop_i(), nop_i()),
            (0x20000 + IA64_ALT_ITLB_VECTOR + 0x30, 0x00,
             mov_m_cr_gr(11, 17), nop_i(), nop_i()),
            (0x20000 + IA64_ALT_ITLB_VECTOR + 0x40, 0x10,
             nop_m(), nop_i(),
             br_cond(0x20000 + IA64_ALT_ITLB_VECTOR + 0x40,
                     0x20000 + IA64_ALT_ITLB_VECTOR + 0x40)),
        ], {
            "ip": 0x20000 + IA64_ALT_ITLB_VECTOR + 0x40,
            "r8": 0x201fff,
            "r9": 0x201fff,
            "r10": 0x202000,
            "r11": IA64_ISR_X,
            "exception": IA64_EXCP_NONE,
        }, entry=0x900, cpu="madison")

test_ia32_cross_page_cs_limit_at_boundary_precedes_second_page_tlb = \
    require_registers(
        "ia32_cross_page_cs_limit_at_boundary_precedes_second_page_tlb", [
            *ia32_environment_bundles(
                0x900, 0x10,
                csd=(IA32_TEST_CSD & ~(0xfffff << 32)) |
                    (0x2000 << 32) | (1 << 62) | 0x200000),
            # Map the first instruction page at a discontiguous PA.
            (0x10, *movl_mlx(18, 0x400000 | DTR_PTE_WB)),
            (0x20, *movl_mlx(19, 0x201000)),
            (0x30, 0x00, mov_m_gr_cr(19, 20),
             adds(21, FOUR_K_ITIR, 0), nop_i()),
            (0x40, 0x00, mov_m_gr_cr(21, 21),
             adds(10, 5, 0), nop_i()),
            (0x50, 0x00, itr_i(10, 18), nop_i(), nop_i()),
            # A 64 KiB IVT mapping includes the IA-32 exception vector.
            (0x60, *movl_mlx(18, 0x20000 | DTR_PTE_WB)),
            (0x70, *movl_mlx(19, 0x20000)),
            (0x80, 0x00, mov_m_gr_cr(19, 20),
             adds(21, 16 << 2, 0), nop_i()),
            (0x90, 0x00, mov_m_gr_cr(21, 21),
             adds(10, 6, 0), nop_i()),
            (0xa0, 0x00, itr_i(10, 18), nop_i(), nop_i()),
            (0xb0, 0x00, srlz_i(), nop_i(), nop_i()),
            (0xc0, *movl_mlx(4, 0x20000)),
            (0xd0, 0x00, mov_m_gr_cr(4, 2), nop_i(), nop_i()),
            (0xe0, *movl_mlx(
                2, IA64_PSR_IC | IA64_PSR_IT | IA64_PSR_IS)),
            (0xf0, *movl_mlx(3, 0x201ffd)),
            *rfi_to_gr(0x100, 2, 3),
            ia32_bundle(0x400ff0, bytes.fromhex(
                "00 00 00 00 00 00 00 00 00 00 00 00 00 c7 05 00")),
            (0x20000 + IA64_IA32_EXCEPTION_VECTOR, 0x00,
             mov_m_cr_gr(8, 19), nop_i(), nop_i()),
            (0x20000 + IA64_IA32_EXCEPTION_VECTOR + 0x10, 0x00,
             mov_m_cr_gr(9, 17), nop_i(), nop_i()),
            (0x20000 + IA64_IA32_EXCEPTION_VECTOR + 0x20, 0x00,
             mov_m_cr_gr(10, 22), nop_i(), nop_i()),
            (0x20000 + IA64_IA32_EXCEPTION_VECTOR + 0x30, 0x10,
             nop_m(), nop_i(),
             br_cond(0x20000 + IA64_IA32_EXCEPTION_VECTOR + 0x30,
                     0x20000 + IA64_IA32_EXCEPTION_VECTOR + 0x30)),
        ], {
            "ip": 0x20000 + IA64_IA32_EXCEPTION_VECTOR + 0x30,
            "r8": 0x201ffd,
            "r9": 13 << 16,
            "r10": 0x201ffd,
            "exception": IA64_EXCP_NONE,
        }, entry=0x900, cpu="madison")

test_ia32_cross_page_tlb_precedes_later_cs_limit = require_registers(
    "ia32_cross_page_tlb_precedes_later_cs_limit", [
        *ia32_environment_bundles(
            0x900, 0x10,
            csd=(IA32_TEST_CSD & ~(0xfffff << 32)) |
                (0x2001 << 32) | (1 << 62) | 0x200000),
        (0x10, *movl_mlx(18, 0x400000 | DTR_PTE_WB)),
        (0x20, *movl_mlx(19, 0x201000)),
        (0x30, 0x00, mov_m_gr_cr(19, 20),
         adds(21, FOUR_K_ITIR, 0), nop_i()),
        (0x40, 0x00, mov_m_gr_cr(21, 21),
         adds(10, 5, 0), nop_i()),
        (0x50, 0x00, itr_i(10, 18), nop_i(), nop_i()),
        # A 4 KiB IVT mapping includes the alternate ITLB vector.
        (0x60, *movl_mlx(18, 0x20000 | DTR_PTE_WB)),
        (0x70, *movl_mlx(19, 0x20000)),
        (0x80, 0x00, mov_m_gr_cr(19, 20),
         adds(21, FOUR_K_ITIR, 0), nop_i()),
        (0x90, 0x00, mov_m_gr_cr(21, 21),
         adds(10, 6, 0), nop_i()),
        (0xa0, 0x00, itr_i(10, 18), nop_i(), nop_i()),
        (0xb0, 0x00, srlz_i(), nop_i(), nop_i()),
        (0xc0, *movl_mlx(4, 0x20000)),
        (0xd0, 0x00, mov_m_gr_cr(4, 2), nop_i(), nop_i()),
        (0xe0, *movl_mlx(
            2, IA64_PSR_IC | IA64_PSR_IT | IA64_PSR_IS)),
        (0xf0, *movl_mlx(3, 0x201ffd)),
        *rfi_to_gr(0x100, 2, 3),
        ia32_bundle(0x400ff0, bytes.fromhex(
            "00 00 00 00 00 00 00 00 00 00 00 00 00 c7 05 00")),
        (0x20000 + IA64_ALT_ITLB_VECTOR, 0x00,
         mov_m_cr_gr(8, 19), nop_i(), nop_i()),
        (0x20000 + IA64_ALT_ITLB_VECTOR + 0x10, 0x00,
         mov_m_cr_gr(9, 22), nop_i(), nop_i()),
        (0x20000 + IA64_ALT_ITLB_VECTOR + 0x20, 0x00,
         mov_m_cr_gr(10, 20), nop_i(), nop_i()),
        (0x20000 + IA64_ALT_ITLB_VECTOR + 0x30, 0x00,
         mov_m_cr_gr(11, 17), nop_i(), nop_i()),
        (0x20000 + IA64_ALT_ITLB_VECTOR + 0x40, 0x10,
         nop_m(), nop_i(),
         br_cond(0x20000 + IA64_ALT_ITLB_VECTOR + 0x40,
                 0x20000 + IA64_ALT_ITLB_VECTOR + 0x40)),
    ], {
        "ip": 0x20000 + IA64_ALT_ITLB_VECTOR + 0x40,
        "r8": 0x201ffd,
        "r9": 0x201ffd,
        "r10": 0x202000,
        "r11": IA64_ISR_X,
        "exception": IA64_EXCP_NONE,
    }, entry=0x900, cpu="madison")

test_ia32_stack_access_wraps_at_4g = require_registers(
    "ia32_stack_access_wraps_at_4g", [
        *ia32_environment_bundles(
            0x900, 0x10,
            csd=IA32_TEST_CSD | (1 << 62),
            dsd=IA32_TEST_DSD | (0xf << 48) | (1 << 63),
            ssd=IA32_TEST_DSD | (0xf << 48) | (1 << 62) | (1 << 63)),
        *dtr_setup_bundles(0x10, 0xfffff000, 0x400000,
                           page_shift=12, slot=5),
        *dtr_setup_bundles(0x70, 0, 0x401000,
                           page_shift=12, slot=6),
        (0xd0, *movl_mlx(8, 0x44332211)),
        (0xe0, *movl_mlx(12, 3)),
        (0xf0, *movl_mlx(
            2, IA64_PSR_IC | IA64_PSR_DT | IA64_PSR_IS)),
        (0x100, *movl_mlx(3, 0x300)),
        *rfi_to_gr(0x110, 2, 3),
        ia32_bundle(0x300, bytes.fromhex(
            "50 "                 # push eax at 0xffffffff..2
            "5b "                 # pop ebx from the wrapped operand
            "0f b8 00 05 00 00")),  # jmpe 0x500
        ia32_bundle(0x400ff0, bytes.fromhex(
            "00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00")),
        ia32_bundle(0x401000, bytes.fromhex("00 00 00")),
        (0x500, 0x10, nop_m(), nop_i(), br_cond(0x500, 0x500)),
    ], {
        "ip": 0x500,
        "r11": 0x44332211,
        "r12": 3,
        "exception": IA64_EXCP_NONE,
    }, entry=0x900, cpu="madison")

test_ia32_x87_data_access_wraps_at_4g = require_registers(
    "ia32_x87_data_access_wraps_at_4g", [
        *ia32_environment_bundles(
            0x700, 0x10,
            csd=IA32_TEST_CSD | (1 << 62),
            dsd=IA32_TEST_DSD | (0xf << 48) | (1 << 63),
            ssd=IA32_TEST_DSD | (0xf << 48) | (1 << 63)),
        *dtr_setup_bundles(0x10, 0xfffff000, 0x400000,
                           page_shift=12, slot=5),
        *dtr_setup_bundles(0x70, 0, 0x401000,
                           page_shift=12, slot=6),
        (0xd0, *movl_mlx(
            2, IA64_PSR_IC | IA64_PSR_DT | IA64_PSR_IS)),
        (0xe0, *movl_mlx(3, 0x300)),
        *rfi_to_gr(0xf0, 2, 3),
        ia32_bundle(0x300, bytes.fromhex(
            "db 2d fb ff ff ff "   # fld tbyte ptr [0xfffffffb]
            "d9 1d 00 01 00 00 "   # fstp dword ptr [0x100]
            "a1 00 01 00")),       # first four bytes of mov eax,[0x100]
        ia32_bundle(0x310, bytes.fromhex(
            "00 0f b8 00 05 00 00")),  # jmpe 0x500
        ia32_bundle(0x400ff0, bytes.fromhex(
            "00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00")),
        ia32_bundle(0x401000, bytes.fromhex("00 00 80 ff 3f")),
        (0x500, 0x10, nop_m(), nop_i(), br_cond(0x500, 0x500)),
    ], {
        "ip": 0x500,
        "r8": 0x3f800000,
        "exception": IA64_EXCP_NONE,
    }, entry=0x700, cpu="madison")

test_ia32_gdt_descriptor_read_triggers_data_breakpoint = require_registers(
    "ia32_gdt_descriptor_read_triggers_data_breakpoint", [
        *ia32_environment_bundles(0x700, 0x10),
        # CFLG.pe selects protected-mode descriptor loading.
        (0x10, *movl_mlx(3, 1)),
        (0x20, 0x00, mov_m_gr_ar(3, 27), nop_i(), nop_i()),
        (0x30, *movl_mlx(4, 0)),
        (0x40, *movl_mlx(5, 0x400)),
        (0x50, 0x00, mov_dbr_indexed_write(4, 5), nop_i(), nop_i()),
        (0x60, 0x00, nop_m(), adds(4, 1, 0), nop_i()),
        (0x70, *movl_mlx(5, 0x81000000ffffffff)),
        (0x80, 0x00, mov_dbr_indexed_write(4, 5), nop_i(), nop_i()),
        (0x90, *movl_mlx(2, IA64_PSR_IC | IA64_PSR_DB)),
        (0xa0, 0x00, mov_gr_psr_full(2), nop_i(), nop_i()),
        (0xb0, 0x00, srlz_d(), nop_i(), nop_i()),
        (0xc0, *movl_mlx(6, 0x100)),
        (0xd0, 0x00, nop_m(), mov_br_gr(7, 6), nop_i()),
        (0xe0, 0x10, nop_m(), nop_i(), br_indirect(7, btype=1)),
        ia32_bundle(0x100, bytes.fromhex(
            "b8 00 04 "  # mov ax,0x400
            "8e d8")),   # mov ds,ax
        # Flat, present, writable, accessed, DPL0 data descriptor.
        ia32_bundle(0x400, bytes.fromhex("ff ff 00 00 00 93 cf 00")),
        (IA64_IA32_EXCEPTION_VECTOR, 0x00,
         mov_m_cr_gr(20, 19), nop_i(), nop_i()),
        (IA64_IA32_EXCEPTION_VECTOR + 0x10, 0x00,
         mov_m_cr_gr(21, 22), nop_i(), nop_i()),
        (IA64_IA32_EXCEPTION_VECTOR + 0x20, 0x00,
         mov_m_cr_gr(22, 17), nop_i(), nop_i()),
        (IA64_IA32_EXCEPTION_VECTOR + 0x30, 0x10,
         nop_m(), nop_i(),
         br_cond(IA64_IA32_EXCEPTION_VECTOR + 0x30,
                 IA64_IA32_EXCEPTION_VECTOR + 0x30)),
    ], {
        "ip": IA64_IA32_EXCEPTION_VECTOR + 0x30,
        "r8": 0x400,
        "r16": 0x400,
        "r20": 0x105,
        "r21": 0x105,
        "r22": (1 << 16) | (1 << 4),
        "exception": IA64_EXCP_NONE,
    }, entry=0x700, cpu="madison")

test_ia32_gdt_descriptor_read_wraps_at_4g = require_registers(
    "ia32_gdt_descriptor_read_wraps_at_4g", [
        *ia32_environment_bundles(0x700, 0x10),
        # Protected mode makes MOV Sreg consult the GDT.
        (0x10, *movl_mlx(3, 1)),
        (0x20, 0x00, mov_m_gr_ar(3, 27), nop_i(), nop_i()),
        # Selector 8 adds its index to this base and wraps to address zero.
        (0x30, *movl_mlx(31, IA32_TEST_GDTD | 0xfffffff8)),
        (0x40, *movl_mlx(2, IA64_PSR_IC)),
        (0x50, 0x00, mov_gr_psr_full(2), nop_i(), nop_i()),
        (0x60, 0x00, srlz_d(), nop_i(), nop_i()),
        (0x70, *movl_mlx(6, 0x100)),
        (0x80, 0x00, nop_m(), mov_br_gr(7, 6), nop_i()),
        (0x90, 0x10, nop_m(), nop_i(), br_indirect(7, btype=1)),
        # Flat, present, writable, accessed, DPL0 data descriptor.
        ia32_bundle(0, bytes.fromhex("ff ff 00 00 00 93 cf 00")),
        ia32_bundle(0x100, bytes.fromhex(
            "b8 08 00 "      # mov ax,8
            "8e d8 "         # mov ds,ax
            "0f b8 00 05")), # jmpe 0x500
        (0x500, 0x10, nop_m(), nop_i(), br_cond(0x500, 0x500)),
    ], {
        "ip": 0x500,
        "r16": 8,
        "exception": IA64_EXCP_NONE,
    }, entry=0x700, cpu="madison")

test_ia32_gdt_reference_rejects_non_system_descriptor = require_registers(
    "ia32_gdt_reference_rejects_non_system_descriptor", [
        *ia32_environment_bundles(0x700, 0x10),
        # Protected mode makes MOV Sreg consult the GDT.
        (0x10, *movl_mlx(3, 1)),
        (0x20, 0x00, mov_m_gr_ar(3, 27), nop_i(), nop_i()),
        # A present code descriptor is not a valid GDTD (s must be zero).
        (0x30, *movl_mlx(31, IA32_TEST_CSD)),
        (0x40, *movl_mlx(2, IA64_PSR_IC)),
        (0x50, 0x00, mov_gr_psr_full(2), nop_i(), nop_i()),
        (0x60, 0x00, srlz_d(), nop_i(), nop_i()),
        (0x70, *movl_mlx(6, 0x100)),
        (0x80, 0x00, nop_m(), mov_br_gr(7, 6), nop_i()),
        (0x90, 0x10, nop_m(), nop_i(), br_indirect(7, btype=1)),
        ia32_bundle(0x100, bytes.fromhex(
            "b8 03 04 "      # mov ax,0x403
            "8e d8 "         # mov ds,ax: GPFault(0x400)
            "0f b8 00 05")), # jmpe 0x500 (must not execute)
        # Valid DPL3 data descriptor if the invalid GDTD were accepted.
        ia32_bundle(0x400, bytes.fromhex("ff ff 00 00 00 f3 cf 00")),
        (IA64_IA32_EXCEPTION_VECTOR, 0x00,
         mov_m_cr_gr(20, 19), nop_i(), nop_i()),
        (IA64_IA32_EXCEPTION_VECTOR + 0x10, 0x00,
         mov_m_cr_gr(21, 17), nop_i(), nop_i()),
        (IA64_IA32_EXCEPTION_VECTOR + 0x20, 0x00,
         mov_m_cr_gr(22, 22), nop_i(), nop_i()),
        (IA64_IA32_EXCEPTION_VECTOR + 0x30, 0x10,
         nop_m(), nop_i(),
         br_cond(IA64_IA32_EXCEPTION_VECTOR + 0x30,
                 IA64_IA32_EXCEPTION_VECTOR + 0x30)),
        (0x500, 0x10, nop_m(), nop_i(), br_cond(0x500, 0x500)),
    ], {
        "ip": IA64_IA32_EXCEPTION_VECTOR + 0x30,
        "r20": 0x103,
        "r21": (13 << 16) | 0x400,
        "r22": 0x103,
        "exception": IA64_EXCP_NONE,
    }, entry=0x700, cpu="madison")

test_ia32_ldt_reference_requires_present_system_descriptor = \
    require_registers(
        "ia32_ldt_reference_requires_present_system_descriptor", [
            *ia32_environment_bundles(0x700, 0x10),
            (0x10, *movl_mlx(3, 1)),
            (0x20, 0x00, mov_m_gr_ar(3, 27), nop_i(), nop_i()),
            # An LDTD with sufficient limit, but p=0.
            (0x30, *movl_mlx(30, 0x0000ffff00000000)),
            # Give the cached LDT an otherwise coherent non-null selector.
            (0x40, *movl_mlx(17, 0x28 << 32)),
            (0x50, *movl_mlx(2, IA64_PSR_IC)),
            (0x60, 0x00, mov_gr_psr_full(2), nop_i(), nop_i()),
            (0x70, 0x00, srlz_d(), nop_i(), nop_i()),
            (0x80, *movl_mlx(6, 0x100)),
            (0x90, 0x00, nop_m(), mov_br_gr(7, 6), nop_i()),
            (0xa0, 0x10, nop_m(), nop_i(), br_indirect(7, btype=1)),
            ia32_bundle(0x100, bytes.fromhex(
                "b8 07 04 "      # mov ax,0x407 (TI=1, RPL=3)
                "8e d8 "         # mov ds,ax: GPFault(0x404)
                "0f b8 00 05")), # jmpe 0x500 (must not execute)
            ia32_bundle(0x400, bytes.fromhex(
                "ff ff 00 00 00 f3 cf 00")),
            (IA64_IA32_EXCEPTION_VECTOR, 0x00,
             mov_m_cr_gr(20, 19), nop_i(), nop_i()),
            (IA64_IA32_EXCEPTION_VECTOR + 0x10, 0x00,
             mov_m_cr_gr(21, 17), nop_i(), nop_i()),
            (IA64_IA32_EXCEPTION_VECTOR + 0x20, 0x00,
             mov_m_cr_gr(22, 22), nop_i(), nop_i()),
            (IA64_IA32_EXCEPTION_VECTOR + 0x30, 0x10,
             nop_m(), nop_i(),
             br_cond(IA64_IA32_EXCEPTION_VECTOR + 0x30,
                     IA64_IA32_EXCEPTION_VECTOR + 0x30)),
            (0x500, 0x10, nop_m(), nop_i(), br_cond(0x500, 0x500)),
        ], {
            "ip": IA64_IA32_EXCEPTION_VECTOR + 0x30,
            "r20": 0x103,
            "r21": (13 << 16) | 0x404,
            "r22": 0x103,
            "exception": IA64_EXCP_NONE,
        }, entry=0x700, cpu="madison")

test_ia32_gate_intercept_reports_concurrent_debug_traps = require_registers(
    "ia32_gate_intercept_reports_concurrent_debug_traps", [
        *ia32_environment_bundles(0x700, 0x10),
        # Protected mode makes the far jump consult the GDT.
        (0x10, *movl_mlx(3, 1)),
        (0x20, 0x00, mov_m_gr_ar(3, 27), nop_i(), nop_i()),
        (0x30, *movl_mlx(3, (1 << 8) | 2)),  # EFLAG.tf
        (0x40, 0x00, mov_m_gr_ar(3, 24), nop_i(), nop_i()),
        (0x50, *movl_mlx(4, 0)),
        (0x60, *movl_mlx(5, 0x400)),
        (0x70, 0x00, mov_dbr_indexed_write(4, 5), nop_i(), nop_i()),
        (0x80, 0x00, nop_m(), adds(4, 1, 0), nop_i()),
        (0x90, *movl_mlx(5, 0x81000000ffffffff)),
        (0xa0, 0x00, mov_dbr_indexed_write(4, 5), nop_i(), nop_i()),
        (0xb0, *movl_mlx(
            2, IA64_PSR_IC | IA64_PSR_DB | IA64_PSR_TB | IA64_PSR_IS)),
        (0xc0, *movl_mlx(3, 0x100)),
        *rfi_to_gr(0xd0, 2, 3),
        ia32_bundle(0x100, bytes.fromhex(
            "ea 00 00 00 04")),  # jmp far 0x400:0 through a call gate
        # Present 16-bit call gate descriptor (type 4).
        ia32_bundle(0x400, bytes.fromhex(
            "00 00 00 00 00 84 00 00")),
        (IA64_IA32_INTERCEPT_VECTOR, 0x00,
         mov_m_cr_gr(8, 19), nop_i(), nop_i()),
        (IA64_IA32_INTERCEPT_VECTOR + 0x10, 0x00,
         mov_m_cr_gr(9, 22), nop_i(), nop_i()),
        (IA64_IA32_INTERCEPT_VECTOR + 0x20, 0x00,
         mov_m_cr_gr(10, 20), nop_i(), nop_i()),
        (IA64_IA32_INTERCEPT_VECTOR + 0x30, 0x00,
         mov_m_cr_gr(11, 24), nop_i(), nop_i()),
        (IA64_IA32_INTERCEPT_VECTOR + 0x40, 0x00,
         mov_m_cr_gr(12, 17), nop_i(), nop_i()),
        (IA64_IA32_INTERCEPT_VECTOR + 0x50, 0x10,
         nop_m(), nop_i(),
         br_cond(IA64_IA32_INTERCEPT_VECTOR + 0x50,
                 IA64_IA32_INTERCEPT_VECTOR + 0x50)),
    ], {
        "ip": IA64_IA32_INTERCEPT_VECTOR + 0x50,
        "r8": 0x105,
        "r9": 0x100,
        "r10": 0x400,
        "r11": 0x0000840000000000,
        "r12": (1 << 16) | (1 << 14) | (1 << 4) | (1 << 3) | (1 << 2),
        "exception": IA64_EXCP_NONE,
    }, entry=0x700, cpu="madison")

test_ia32_jmpe_reports_concurrent_data_breakpoint = require_registers(
    "ia32_jmpe_reports_concurrent_data_breakpoint", [
        *ia32_environment_bundles(0x700, 0x10),
        (0x10, *movl_mlx(3, (1 << 8) | 2)),  # EFLAG.tf
        (0x20, 0x00, mov_m_gr_ar(3, 24), nop_i(), nop_i()),
        (0x30, *movl_mlx(4, 0)),
        (0x40, *movl_mlx(5, 0x200)),
        (0x50, 0x00, mov_dbr_indexed_write(4, 5), nop_i(), nop_i()),
        (0x60, 0x00, nop_m(), adds(4, 1, 0), nop_i()),
        (0x70, *movl_mlx(5, 0x81000000ffffffff)),
        (0x80, 0x00, mov_dbr_indexed_write(4, 5), nop_i(), nop_i()),
        (0x90, *movl_mlx(
            2, IA64_PSR_IC | IA64_PSR_DB | IA64_PSR_TB | IA64_PSR_IS)),
        (0xa0, *movl_mlx(3, 0x100)),
        *rfi_to_gr(0xb0, 2, 3),
        ia32_bundle(0x100, bytes.fromhex(
            "0f 00 36 00 02")),  # jmpe word ptr [0x200]
        ia32_bundle(0x200, bytes.fromhex("00 03")),
        (IA64_IA32_EXCEPTION_VECTOR, 0x00,
         mov_m_cr_gr(8, 19), nop_i(), nop_i()),
        (IA64_IA32_EXCEPTION_VECTOR + 0x10, 0x00,
         mov_m_cr_gr(9, 22), nop_i(), nop_i()),
        (IA64_IA32_EXCEPTION_VECTOR + 0x20, 0x00,
         mov_m_cr_gr(10, 17), nop_i(), nop_i()),
        (IA64_IA32_EXCEPTION_VECTOR + 0x30, 0x10,
         nop_m(), nop_i(),
         br_cond(IA64_IA32_EXCEPTION_VECTOR + 0x30,
                 IA64_IA32_EXCEPTION_VECTOR + 0x30)),
    ], {
        "ip": IA64_IA32_EXCEPTION_VECTOR + 0x30,
        "r1": 0x105,
        "r8": 0x300,
        "r9": 0x100,
        "r10": (1 << 16) | (1 << 4) | (1 << 3) | (1 << 2),
        "exception": IA64_EXCP_NONE,
    }, entry=0x700, cpu="madison")

test_ia32_cflg_io_clear_denies_failed_iopl = require_registers(
    "ia32_cflg_io_clear_denies_failed_iopl", [
        *ia32_environment_bundles(0x700, 0x10),
        *dtr_setup_bundles(0x10, 0x100000000, 0x400000,
                           pte_flags=DTR_PTE_UC | (3 << 7)),
        (0x70, *movl_mlx(3, 0x100000000)),
        (0x80, 0x00, mov_m_gr_ar(3, 0), nop_i(), nop_i()),
        # CFLG.pe=1 and CFLG.io=0; EFLAGS.IOPL remains zero.
        (0x90, *movl_mlx(3, 1)),
        (0xa0, 0x00, mov_m_gr_ar(3, 27), nop_i(), nop_i()),
        # Only rfi can restore the high PSR.CPL bits; mov psr.l cannot.
        (0xb0, *movl_mlx(
            2, IA64_PSR_IC | IA64_PSR_DT | IA64_PSR_IS |
            IA64_PSR_CPL3)),
        (0xc0, *movl_mlx(3, 0x300)),
        *rfi_to_gr(0xd0, 2, 3),
        ia32_bundle(0x300, bytes.fromhex(
            "e4 01 "          # in al,1
            "0f b8 00 05")),  # jmpe 0x500 (must not execute)
        ia32_bundle(0x400000, bytes.fromhex("00 5a")),
        (IA64_IA32_EXCEPTION_VECTOR, 0x00,
         mov_m_cr_gr(20, 19), nop_i(), nop_i()),
        (IA64_IA32_EXCEPTION_VECTOR + 0x10, 0x00,
         mov_m_cr_gr(21, 17), nop_i(), nop_i()),
        (IA64_IA32_EXCEPTION_VECTOR + 0x20, 0x00,
         mov_m_cr_gr(22, 22), nop_i(), nop_i()),
        (IA64_IA32_EXCEPTION_VECTOR + 0x30, 0x10,
         nop_m(), nop_i(),
         br_cond(IA64_IA32_EXCEPTION_VECTOR + 0x30,
                 IA64_IA32_EXCEPTION_VECTOR + 0x30)),
        (0x500, 0x10, nop_m(), nop_i(), br_cond(0x500, 0x500)),
    ], {
        "ip": IA64_IA32_EXCEPTION_VECTOR + 0x30,
        "r20": 0x300,
        "r21": 13 << 16,
        "r22": 0x300,
        "exception": IA64_EXCP_NONE,
    }, entry=0x700, cpu="madison")

test_ia32_tss_io_bitmap_allows_and_denies_ports = require_registers(
    "ia32_tss_io_bitmap_allows_and_denies_ports", [
        *ia32_environment_bundles(0x700, 0x10),
        *dtr_setup_bundles(0x10, 0x100000000, 0x400000,
                           pte_flags=DTR_PTE_UC | (3 << 7)),
        *dtr_setup_bundles(0x70, 0x1000, 0x1000,
                           page_shift=12, slot=6,
                           pte_flags=DTR_PTE_UC | (3 << 7)),
        (0xd0, *movl_mlx(3, 0x100000000)),
        (0xe0, 0x00, mov_m_gr_ar(3, 0), nop_i(), nop_i()),
        # Present 386 TSS at 0x1000 with a limit through bitmap byte 1.
        (0xf0, *movl_mlx(
            3, (1 << 59) | (9 << 52) | (0x81 << 32) | 0x1000)),
        (0x100, 0x00, mov_m_gr_ar(3, 1), nop_i(), nop_i()),
        # CFLG.pe=1 and CFLG.io=1; EFLAGS.IOPL remains zero.
        (0x110, *movl_mlx(3, (1 << 6) | 1)),
        (0x120, 0x00, mov_m_gr_ar(3, 27), nop_i(), nop_i()),
        (0x130, *movl_mlx(
            2, IA64_PSR_IC | IA64_PSR_DT | IA64_PSR_IS |
            IA64_PSR_CPL3)),
        (0x140, *movl_mlx(3, 0x300)),
        *rfi_to_gr(0x150, 2, 3),
        ia32_bundle(0x300, bytes.fromhex(
            "e4 01 "          # in al,1: bitmap bit 1 is clear
            "e4 02 "          # in al,2: bitmap bit 2 is set
            "0f b8 00 05")),  # jmpe 0x500 (must not execute)
        # The 386 TSS I/O-map base word is at TSS offset 0x66.
        ia32_bundle(0x1060, bytes.fromhex(
            "00 00 00 00 00 00 80 00")),
        # Permit port 1 and deny port 2.
        ia32_bundle(0x1080, bytes.fromhex("04 00")),
        ia32_bundle(0x400000, bytes.fromhex("00 5a 6b")),
        (IA64_IA32_EXCEPTION_VECTOR, 0x00,
         mov_m_cr_gr(20, 19), nop_i(), nop_i()),
        (IA64_IA32_EXCEPTION_VECTOR + 0x10, 0x00,
         mov_m_cr_gr(21, 17), nop_i(), nop_i()),
        (IA64_IA32_EXCEPTION_VECTOR + 0x20, 0x00,
         mov_m_cr_gr(22, 22), nop_i(), nop_i()),
        (IA64_IA32_EXCEPTION_VECTOR + 0x30, 0x10,
         nop_m(), nop_i(),
         br_cond(IA64_IA32_EXCEPTION_VECTOR + 0x30,
                 IA64_IA32_EXCEPTION_VECTOR + 0x30)),
        (0x500, 0x10, nop_m(), nop_i(), br_cond(0x500, 0x500)),
    ], {
        "ip": IA64_IA32_EXCEPTION_VECTOR + 0x30,
        "r8": 0x5a,
        "r20": 0x302,
        "r21": 13 << 16,
        "r22": 0x302,
        "exception": IA64_EXCP_NONE,
    }, entry=0x700, cpu="madison")

test_ia32_tss_reference_rejects_non_system_descriptor = require_registers(
    "ia32_tss_reference_rejects_non_system_descriptor", [
        *ia32_environment_bundles(0x700, 0x10),
        *dtr_setup_bundles(0x10, 0x100000000, 0x400000,
                           pte_flags=DTR_PTE_UC | (3 << 7)),
        *dtr_setup_bundles(0x70, 0x1000, 0x1000,
                           page_shift=12, slot=6,
                           pte_flags=DTR_PTE_UC | (3 << 7)),
        (0xd0, *movl_mlx(3, 0x100000000)),
        (0xe0, 0x00, mov_m_gr_ar(3, 0), nop_i(), nop_i()),
        # Present 386 TSS shape, but s=1 makes TSSD non-system.
        (0xf0, *movl_mlx(
            3, (1 << 59) | (1 << 56) | (9 << 52) |
            (0x81 << 32) | 0x1000)),
        (0x100, 0x00, mov_m_gr_ar(3, 1), nop_i(), nop_i()),
        (0x110, *movl_mlx(3, (1 << 6) | 1)),
        (0x120, 0x00, mov_m_gr_ar(3, 27), nop_i(), nop_i()),
        (0x130, *movl_mlx(
            2, IA64_PSR_IC | IA64_PSR_DT | IA64_PSR_IS |
            IA64_PSR_CPL3)),
        (0x140, *movl_mlx(3, 0x300)),
        *rfi_to_gr(0x150, 2, 3),
        ia32_bundle(0x300, bytes.fromhex(
            "e4 01 "          # in al,1: GPFault(0)
            "0f b8 00 05")),  # jmpe 0x500 (must not execute)
        ia32_bundle(0x1060, bytes.fromhex(
            "00 00 00 00 00 00 80 00")),
        ia32_bundle(0x1080, bytes.fromhex("00 00")),
        ia32_bundle(0x400000, bytes.fromhex("00 5a")),
        (IA64_IA32_EXCEPTION_VECTOR, 0x00,
         mov_m_cr_gr(20, 19), nop_i(), nop_i()),
        (IA64_IA32_EXCEPTION_VECTOR + 0x10, 0x00,
         mov_m_cr_gr(21, 17), nop_i(), nop_i()),
        (IA64_IA32_EXCEPTION_VECTOR + 0x20, 0x00,
         mov_m_cr_gr(22, 22), nop_i(), nop_i()),
        (IA64_IA32_EXCEPTION_VECTOR + 0x30, 0x10,
         nop_m(), nop_i(),
         br_cond(IA64_IA32_EXCEPTION_VECTOR + 0x30,
                 IA64_IA32_EXCEPTION_VECTOR + 0x30)),
        (0x500, 0x10, nop_m(), nop_i(), br_cond(0x500, 0x500)),
    ], {
        "ip": IA64_IA32_EXCEPTION_VECTOR + 0x30,
        "r20": 0x300,
        "r21": 13 << 16,
        "r22": 0x300,
        "exception": IA64_EXCP_NONE,
    }, entry=0x700, cpu="madison")

test_ia32_unaligned_gdt_obeys_psr_ac = require_registers(
    "ia32_unaligned_gdt_obeys_psr_ac", [
        *ia32_environment_bundles(0x700, 0x10),
        # CFLG.pe selects protected-mode descriptor loading.
        (0x10, *movl_mlx(3, 1)),
        (0x20, 0x00, mov_m_gr_ar(3, 27), nop_i(), nop_i()),
        # The GDT cache descriptor in GR31 has base 1.
        (0x30, *movl_mlx(31, IA32_TEST_GDTD | 1)),
        (0x40, *movl_mlx(2, IA64_PSR_IC | IA64_PSR_AC)),
        (0x50, 0x00, mov_gr_psr_full(2), nop_i(), nop_i()),
        (0x60, 0x00, srlz_d(), nop_i(), nop_i()),
        (0x70, *movl_mlx(6, 0x100)),
        (0x80, 0x00, nop_m(), mov_br_gr(7, 6), nop_i()),
        (0x90, 0x10, nop_m(), nop_i(), br_indirect(7, btype=1)),
        ia32_bundle(0x100, bytes.fromhex(
            "b8 00 04 "  # mov ax,0x400
            "8e d8")),   # mov ds,ax
        # Descriptor 0x400 starts at the deliberately unaligned GDT+0x400.
        ia32_bundle(0x400, bytes.fromhex(
            "00 ff ff 00 00 00 93 cf 00")),
        (IA64_IA32_EXCEPTION_VECTOR, 0x00,
         mov_m_cr_gr(20, 19), nop_i(), nop_i()),
        (IA64_IA32_EXCEPTION_VECTOR + 0x10, 0x00,
         mov_m_cr_gr(21, 17), nop_i(), nop_i()),
        (IA64_IA32_EXCEPTION_VECTOR + 0x20, 0x00,
         mov_m_cr_gr(22, 20), nop_i(), nop_i()),
        (IA64_IA32_EXCEPTION_VECTOR + 0x30, 0x00,
         mov_m_cr_gr(23, 22), nop_i(), nop_i()),
        (IA64_IA32_EXCEPTION_VECTOR + 0x40, 0x10,
         nop_m(), nop_i(),
         br_cond(IA64_IA32_EXCEPTION_VECTOR + 0x40,
                 IA64_IA32_EXCEPTION_VECTOR + 0x40)),
    ], {
        "ip": IA64_IA32_EXCEPTION_VECTOR + 0x40,
        "r8": 0x400,
        "r20": 0x103,
        "r21": 17 << 16,
        "r22": 0x401,
        "r23": 0x103,
        "exception": IA64_EXCP_NONE,
    }, entry=0x700, cpu="madison")

test_ia32_high_iobase_read_triggers_data_breakpoint = require_registers(
    "ia32_high_iobase_read_triggers_data_breakpoint", [
        *ia32_environment_bundles(0x700, 0x10),
        *dtr_setup_bundles(0x10, 0x100000000, 0x400000,
                           pte_flags=DTR_PTE_UC),
        (0x70, *movl_mlx(3, 0x100000000)),
        (0x80, 0x00, mov_m_gr_ar(3, 0), nop_i(), nop_i()),
        (0x90, *movl_mlx(4, 0)),
        (0xa0, *movl_mlx(5, 0x100000001)),
        (0xb0, 0x00, mov_dbr_indexed_write(4, 5), nop_i(), nop_i()),
        (0xc0, 0x00, nop_m(), adds(4, 1, 0), nop_i()),
        (0xd0, *movl_mlx(5, 0x81ffffffffffffff)),
        (0xe0, 0x00, mov_dbr_indexed_write(4, 5), nop_i(), nop_i()),
        (0xf0, *movl_mlx(2, IA64_PSR_IC | IA64_PSR_DT | IA64_PSR_DB)),
        (0x100, 0x00, mov_gr_psr_full(2), nop_i(), nop_i()),
        (0x110, 0x00, srlz_d(), nop_i(), nop_i()),
        (0x120, *movl_mlx(6, 0x300)),
        (0x130, 0x00, nop_m(), mov_br_gr(7, 6), nop_i()),
        (0x140, 0x10, nop_m(), nop_i(), br_indirect(7, btype=1)),
        ia32_bundle(0x300, bytes.fromhex("e4 01")),  # in al,1
        ia32_bundle(0x400000, bytes.fromhex("00 5a")),
        (IA64_IA32_EXCEPTION_VECTOR, 0x00,
         mov_m_cr_gr(20, 19), nop_i(), nop_i()),
        (IA64_IA32_EXCEPTION_VECTOR + 0x10, 0x00,
         mov_m_cr_gr(21, 22), nop_i(), nop_i()),
        (IA64_IA32_EXCEPTION_VECTOR + 0x20, 0x00,
         mov_m_cr_gr(22, 17), nop_i(), nop_i()),
        (IA64_IA32_EXCEPTION_VECTOR + 0x30, 0x10,
         nop_m(), nop_i(),
         br_cond(IA64_IA32_EXCEPTION_VECTOR + 0x30,
                 IA64_IA32_EXCEPTION_VECTOR + 0x30)),
    ], {
        "ip": IA64_IA32_EXCEPTION_VECTOR + 0x30,
        "r8": 0x5a,
        "r20": 0x302,
        "r21": 0x302,
        "r22": (1 << 16) | (1 << 4),
        "exception": IA64_EXCP_NONE,
    }, entry=0x700, cpu="madison")

test_ia32_io_access_crosses_port_ffff_without_wrapping = require_registers(
    "ia32_io_access_crosses_port_ffff_without_wrapping", [
        *ia32_environment_bundles(0x700, 0x10),
        *dtr_setup_bundles(0x10, 0x103fff000, 0x400000,
                           page_shift=12, slot=5, pte_flags=DTR_PTE_UC),
        *dtr_setup_bundles(0x70, 0x104000000, 0x401000,
                           page_shift=12, slot=6, pte_flags=DTR_PTE_UC),
        (0xd0, *movl_mlx(3, 0x100000000)),
        (0xe0, 0x00, mov_m_gr_ar(3, 0), nop_i(), nop_i()),
        (0xf0, *movl_mlx(10, 0xffff)),  # edx
        (0x100, *movl_mlx(2, IA64_PSR_IC | IA64_PSR_DT)),
        (0x110, 0x00, mov_gr_psr_full(2), nop_i(), nop_i()),
        (0x120, 0x00, srlz_d(), nop_i(), nop_i()),
        (0x130, *movl_mlx(6, 0x300)),
        (0x140, 0x00, nop_m(), mov_br_gr(7, 6), nop_i()),
        (0x150, 0x10, nop_m(), nop_i(), br_indirect(7, btype=1)),
        ia32_bundle(0x300, bytes.fromhex(
            "66 ed "        # in eax,dx
            "0f b8 00 05")),  # jmpe 0x500
        ia32_bundle(0x400ff0, bytes.fromhex(
            "00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 11")),
        ia32_bundle(0x401000, bytes.fromhex("22 33 44")),
        (0x500, 0x10, nop_m(), nop_i(), br_cond(0x500, 0x500)),
    ], {
        "ip": 0x500,
        "r8": 0x44332211,
        "exception": IA64_EXCP_NONE,
    }, entry=0x700, cpu="madison")

test_ia32_io_eflag_ac_does_not_check_alignment = require_registers(
    "ia32_io_eflag_ac_does_not_check_alignment", [
        *ia32_environment_bundles(0x700, 0x10),
        *dtr_setup_bundles(0x10, 0x100000000, 0x400000,
                           pte_flags=DTR_PTE_UC | (3 << 7)),
        (0x70, *movl_mlx(3, 0x100000000)),
        (0x80, 0x00, mov_m_gr_ar(3, 0), nop_i(), nop_i()),
        # CFLG.pe and CFLG.am; EFLAG.ac with IOPL=3.
        (0x90, *movl_mlx(3, (1 << 18) | 1)),
        (0xa0, 0x00, mov_m_gr_ar(3, 27), nop_i(), nop_i()),
        (0xb0, *movl_mlx(3, (1 << 18) | (3 << 12) | 2)),
        (0xc0, 0x00, mov_m_gr_ar(3, 24), nop_i(), nop_i()),
        (0xd0, *movl_mlx(
            2, IA64_PSR_IC | IA64_PSR_DT | IA64_PSR_IS |
            IA64_PSR_CPL3)),
        (0xe0, *movl_mlx(3, 0x300)),
        *rfi_to_gr(0xf0, 2, 3),
        ia32_bundle(0x300, bytes.fromhex(
            "e5 01 "          # in ax,1
            "0f b8 00 05")),  # jmpe 0x500
        ia32_bundle(0x400000, bytes.fromhex("00 11 22")),
        (0x500, 0x10, nop_m(), nop_i(), br_cond(0x500, 0x500)),
    ], {
        "ip": 0x500,
        "r8": 0x2211,
        "exception": IA64_EXCP_NONE,
    }, entry=0x700, cpu="madison")

test_ia32_io_psr_ac_unaligned_word_sets_ifa = require_registers(
    "ia32_io_psr_ac_unaligned_word_sets_ifa", [
        *ia32_environment_bundles(0x700, 0x10),
        *dtr_setup_bundles(0x10, 0x100000000, 0x400000,
                           pte_flags=DTR_PTE_UC),
        (0x70, *movl_mlx(3, 0x100000000)),
        (0x80, 0x00, mov_m_gr_ar(3, 0), nop_i(), nop_i()),
        (0x90, *movl_mlx(
            2, IA64_PSR_IC | IA64_PSR_DT | IA64_PSR_AC)),
        (0xa0, 0x00, mov_gr_psr_full(2), nop_i(), nop_i()),
        (0xb0, 0x00, srlz_d(), nop_i(), nop_i()),
        (0xc0, *movl_mlx(6, 0x300)),
        (0xd0, 0x00, nop_m(), mov_br_gr(7, 6), nop_i()),
        (0xe0, 0x10, nop_m(), nop_i(), br_indirect(7, btype=1)),
        ia32_bundle(0x300, bytes.fromhex("e5 01")),  # in ax,1
        ia32_bundle(0x400000, bytes.fromhex("00 11 22")),
        (IA64_IA32_EXCEPTION_VECTOR, 0x00,
         mov_m_cr_gr(20, 19), nop_i(), nop_i()),
        (IA64_IA32_EXCEPTION_VECTOR + 0x10, 0x00,
         mov_m_cr_gr(21, 17), nop_i(), nop_i()),
        (IA64_IA32_EXCEPTION_VECTOR + 0x20, 0x00,
         mov_m_cr_gr(22, 20), nop_i(), nop_i()),
        (IA64_IA32_EXCEPTION_VECTOR + 0x30, 0x00,
         mov_m_cr_gr(23, 22), nop_i(), nop_i()),
        (IA64_IA32_EXCEPTION_VECTOR + 0x40, 0x10,
         nop_m(), nop_i(),
         br_cond(IA64_IA32_EXCEPTION_VECTOR + 0x40,
                 IA64_IA32_EXCEPTION_VECTOR + 0x40)),
    ], {
        "ip": IA64_IA32_EXCEPTION_VECTOR + 0x40,
        "r20": 0x300,
        "r21": 17 << 16,
        "r22": 0x100000001,
        "r23": 0x300,
        "exception": IA64_EXCP_NONE,
    }, entry=0x700, cpu="madison")

test_ia32_rep_dbr_traps_after_matching_iteration = require_registers(
    "ia32_rep_dbr_traps_after_matching_iteration", [
        *ia32_environment_bundles(0x700, 0x10),
        (0x10, *movl_mlx(4, 0)),
        (0x20, *movl_mlx(5, 0x201)),
        (0x30, 0x00, mov_dbr_indexed_write(4, 5), nop_i(), nop_i()),
        (0x40, 0x00, nop_m(), adds(4, 1, 0), nop_i()),
        (0x50, *movl_mlx(5, 0x81000000ffffffff)),
        (0x60, 0x00, mov_dbr_indexed_write(4, 5), nop_i(), nop_i()),
        (0x70, *movl_mlx(9, 3)),       # ecx
        (0x80, *movl_mlx(14, 0x200)),  # esi
        (0x90, *movl_mlx(15, 0x300)),  # edi
        (0xa0, *movl_mlx(2, IA64_PSR_IC | IA64_PSR_DB)),
        (0xb0, 0x00, mov_gr_psr_full(2), nop_i(), nop_i()),
        (0xc0, 0x00, srlz_d(), nop_i(), nop_i()),
        (0xd0, *movl_mlx(6, 0x100)),
        (0xe0, 0x00, nop_m(), mov_br_gr(7, 6), nop_i()),
        (0xf0, 0x10, nop_m(), nop_i(), br_indirect(7, btype=1)),
        ia32_bundle(0x100, bytes.fromhex("f3 a4")),  # rep movsb
        ia32_bundle(0x200, bytes.fromhex("11 22 33")),
        ia32_bundle(0x300, b""),
        (IA64_IA32_EXCEPTION_VECTOR, 0x00,
         mov_m_cr_gr(8, 19), nop_i(), nop_i()),
        (IA64_IA32_EXCEPTION_VECTOR + 0x10, 0x00,
         mov_m_cr_gr(10, 17), nop_i(), nop_i()),
        (IA64_IA32_EXCEPTION_VECTOR + 0x20, 0x00,
         mov_m_cr_gr(11, 22), nop_i(), nop_i()),
        (IA64_IA32_EXCEPTION_VECTOR + 0x30, 0x00,
         mov_m_ar_gr(12, 24), nop_i(), nop_i()),
        (IA64_IA32_EXCEPTION_VECTOR + 0x40, 0x00,
         nop_m(), addl(3, 0x300, 0), nop_i()),
        (IA64_IA32_EXCEPTION_VECTOR + 0x50, 0x00,
         ld2(13, 3), nop_i(), nop_i()),
        (IA64_IA32_EXCEPTION_VECTOR + 0x60, 0x10,
         nop_m(), nop_i(),
         br_cond(IA64_IA32_EXCEPTION_VECTOR + 0x60,
                 IA64_IA32_EXCEPTION_VECTOR + 0x60)),
    ], {
        "ip": IA64_IA32_EXCEPTION_VECTOR + 0x60,
        "r8": 0x100,
        "r9": 1,
        "r10": (1 << 16) | (1 << 4),
        "r11": 0x100,
        "r12": (1 << 16) | 2,
        "r13": 0x2211,
        "r14": 0x202,
        "r15": 0x302,
        "exception": IA64_EXCP_NONE,
    }, entry=0x700, cpu="madison")

test_ia32_rep_final_iteration_trap_advances_ip_and_clears_rf = \
    require_registers(
        "ia32_rep_final_iteration_trap_advances_ip_and_clears_rf", [
            *ia32_environment_bundles(0x700, 0x10),
            (0x10, *movl_mlx(4, 0)),
            (0x20, *movl_mlx(5, 0x200)),
            (0x30, 0x00, mov_dbr_indexed_write(4, 5), nop_i(), nop_i()),
            (0x40, 0x00, nop_m(), adds(4, 1, 0), nop_i()),
            (0x50, *movl_mlx(5, 0x81000000ffffffff)),
            (0x60, 0x00, mov_dbr_indexed_write(4, 5), nop_i(), nop_i()),
            (0x70, *movl_mlx(9, 1)),       # ecx: one final iteration
            (0x80, *movl_mlx(14, 0x200)),  # esi
            (0x90, *movl_mlx(15, 0x300)),  # edi
            (0xa0, *movl_mlx(2, IA64_PSR_IC | IA64_PSR_DB)),
            (0xb0, 0x00, mov_gr_psr_full(2), nop_i(), nop_i()),
            (0xc0, 0x00, srlz_d(), nop_i(), nop_i()),
            (0xd0, *movl_mlx(6, 0x100)),
            (0xe0, 0x00, nop_m(), mov_br_gr(7, 6), nop_i()),
            (0xf0, 0x10, nop_m(), nop_i(), br_indirect(7, btype=1)),
            ia32_bundle(0x100, bytes.fromhex("f3 a4")),  # rep movsb
            ia32_bundle(0x200, bytes.fromhex("5a")),
            ia32_bundle(0x300, b""),
            (IA64_IA32_EXCEPTION_VECTOR, 0x00,
             mov_m_cr_gr(8, 19), nop_i(), nop_i()),
            (IA64_IA32_EXCEPTION_VECTOR + 0x10, 0x00,
             mov_m_cr_gr(10, 17), nop_i(), nop_i()),
            (IA64_IA32_EXCEPTION_VECTOR + 0x20, 0x00,
             mov_m_cr_gr(11, 22), nop_i(), nop_i()),
            (IA64_IA32_EXCEPTION_VECTOR + 0x30, 0x00,
             mov_m_ar_gr(12, 24), nop_i(), nop_i()),
            (IA64_IA32_EXCEPTION_VECTOR + 0x40, 0x10,
             nop_m(), nop_i(),
             br_cond(IA64_IA32_EXCEPTION_VECTOR + 0x40,
                     IA64_IA32_EXCEPTION_VECTOR + 0x40)),
        ], {
            "ip": IA64_IA32_EXCEPTION_VECTOR + 0x40,
            "r8": 0x102,
            "r9": 0,
            "r10": (1 << 16) | (1 << 4),
            "r11": 0x102,
            "r12": 2,
            "r14": 0x201,
            "r15": 0x301,
            "exception": IA64_EXCP_NONE,
        }, entry=0x700, cpu="madison")

test_ia32_rep_fault_sets_rf_for_restart = require_registers(
    "ia32_rep_fault_sets_rf_for_restart", [
        *ia32_environment_bundles(
            0x700, 0x10, dsd=0x093000ff00000000),
        (0x10, *movl_mlx(9, 2)),          # ecx
        (0x20, *movl_mlx(14, 0x100)),     # esi, just beyond DS.limit
        (0x30, *movl_mlx(15, 0x80)),      # edi
        (0x40, *movl_mlx(2, IA64_PSR_IC)),
        (0x50, 0x00, mov_gr_psr_full(2), nop_i(), nop_i()),
        (0x60, 0x00, srlz_d(), nop_i(), nop_i()),
        (0x70, *movl_mlx(6, 0x100)),
        (0x80, 0x00, nop_m(), mov_br_gr(7, 6), nop_i()),
        (0x90, 0x10, nop_m(), nop_i(), br_indirect(7, btype=1)),
        ia32_bundle(0x100, bytes.fromhex("f3 a4")),  # rep movsb
        (IA64_IA32_EXCEPTION_VECTOR, 0x00,
         mov_m_cr_gr(8, 19), nop_i(), nop_i()),
        (IA64_IA32_EXCEPTION_VECTOR + 0x10, 0x00,
         mov_m_cr_gr(10, 17), nop_i(), nop_i()),
        (IA64_IA32_EXCEPTION_VECTOR + 0x20, 0x00,
         mov_m_cr_gr(11, 22), nop_i(), nop_i()),
        (IA64_IA32_EXCEPTION_VECTOR + 0x30, 0x00,
         mov_m_ar_gr(12, 24), nop_i(), nop_i()),
        (IA64_IA32_EXCEPTION_VECTOR + 0x40, 0x10,
         nop_m(), nop_i(),
         br_cond(IA64_IA32_EXCEPTION_VECTOR + 0x40,
                 IA64_IA32_EXCEPTION_VECTOR + 0x40)),
    ], {
        "ip": IA64_IA32_EXCEPTION_VECTOR + 0x40,
        "r8": 0x100,
        "r9": 2,
        "r10": 13 << 16,
        "r11": 0x100,
        "r12": (1 << 16) | 2,
        "r14": 0x100,
        "r15": 0x80,
        "exception": IA64_EXCP_NONE,
    }, entry=0x700, cpu="madison")

test_ia32_stack_push_triggers_data_breakpoint = require_registers(
    "ia32_stack_push_triggers_data_breakpoint", [
        *ia32_environment_bundles(0x700, 0x10),
        (0x10, *movl_mlx(4, 0)),
        (0x20, *movl_mlx(5, 0x202)),
        (0x30, 0x00, mov_dbr_indexed_write(4, 5), nop_i(), nop_i()),
        (0x40, 0x00, nop_m(), adds(4, 1, 0), nop_i()),
        (0x50, *movl_mlx(5, 0x41000000ffffffff)),
        (0x60, 0x00, mov_dbr_indexed_write(4, 5), nop_i(), nop_i()),
        (0x70, *movl_mlx(8, 0x1234)),   # eax
        (0x80, *movl_mlx(12, 0x204)),   # esp
        (0x90, *movl_mlx(2, IA64_PSR_IC | IA64_PSR_DB)),
        (0xa0, 0x00, mov_gr_psr_full(2), nop_i(), nop_i()),
        (0xb0, 0x00, srlz_d(), nop_i(), nop_i()),
        (0xc0, *movl_mlx(6, 0x100)),
        (0xd0, 0x00, nop_m(), mov_br_gr(7, 6), nop_i()),
        (0xe0, 0x10, nop_m(), nop_i(), br_indirect(7, btype=1)),
        ia32_bundle(0x100, b"\x50"),  # push ax
        (IA64_IA32_EXCEPTION_VECTOR, 0x00,
         mov_m_cr_gr(20, 19), nop_i(), nop_i()),
        (IA64_IA32_EXCEPTION_VECTOR + 0x10, 0x00,
         mov_m_cr_gr(21, 17), nop_i(), nop_i()),
        (IA64_IA32_EXCEPTION_VECTOR + 0x20, 0x00,
         mov_m_cr_gr(22, 22), nop_i(), nop_i()),
        (IA64_IA32_EXCEPTION_VECTOR + 0x30, 0x00,
         nop_m(), addl(3, 0x202, 0), nop_i()),
        (IA64_IA32_EXCEPTION_VECTOR + 0x40, 0x00,
         ld2(23, 3), nop_i(), nop_i()),
        (IA64_IA32_EXCEPTION_VECTOR + 0x50, 0x10,
         nop_m(), nop_i(),
         br_cond(IA64_IA32_EXCEPTION_VECTOR + 0x50,
                 IA64_IA32_EXCEPTION_VECTOR + 0x50)),
    ], {
        "ip": IA64_IA32_EXCEPTION_VECTOR + 0x50,
        "r8": 0x1234,
        "r12": 0x202,
        "r20": 0x101,
        "r21": (1 << 16) | (1 << 4),
        "r22": 0x101,
        "r23": 0x1234,
        "exception": IA64_EXCP_NONE,
    }, entry=0x700, cpu="madison")

test_ia32_maskmovq_triggers_data_breakpoint = require_registers(
    "ia32_maskmovq_triggers_data_breakpoint", [
        *ia32_environment_bundles(0x700, 0x10),
        (0x10, *movl_mlx(4, 0)),
        (0x20, *movl_mlx(5, 0x201)),
        (0x30, 0x00, mov_dbr_indexed_write(4, 5), nop_i(), nop_i()),
        (0x40, 0x00, nop_m(), adds(4, 1, 0), nop_i()),
        (0x50, *movl_mlx(5, 0x41000000ffffffff)),
        (0x60, 0x00, mov_dbr_indexed_write(4, 5), nop_i(), nop_i()),
        (0x70, *movl_mlx(5, 0x8877665544332211)),
        (0x80, 0x00, setf_sig(8, 5), nop_i(), nop_i()),   # mm0
        (0x90, *movl_mlx(6, 0x8000)),
        (0xa0, 0x00, setf_sig(9, 6), nop_i(), nop_i()),   # mm1 mask
        (0xb0, *movl_mlx(15, 0x200)),                     # edi
        (0xc0, *movl_mlx(2, IA64_PSR_IC | IA64_PSR_DB)),
        (0xd0, 0x00, mov_gr_psr_full(2), nop_i(), nop_i()),
        (0xe0, 0x00, srlz_d(), nop_i(), nop_i()),
        (0xf0, *movl_mlx(3, 0x300)),
        (0x100, 0x00, nop_m(), mov_br_gr(7, 3), nop_i()),
        (0x110, 0x10, nop_m(), nop_i(), br_indirect(7, btype=1)),
        ia32_bundle(0x200, bytes.fromhex("00 00")),
        ia32_bundle(0x300, bytes.fromhex(
            "0f f7 c1")),  # maskmovq mm0,mm1
        (IA64_IA32_EXCEPTION_VECTOR, 0x00,
         mov_m_cr_gr(20, 19), nop_i(), nop_i()),
        (IA64_IA32_EXCEPTION_VECTOR + 0x10, 0x00,
         mov_m_cr_gr(21, 17), nop_i(), nop_i()),
        (IA64_IA32_EXCEPTION_VECTOR + 0x20, 0x00,
         mov_m_cr_gr(22, 22), nop_i(), nop_i()),
        (IA64_IA32_EXCEPTION_VECTOR + 0x30, 0x00,
         nop_m(), addl(3, 0x200, 0), nop_i()),
        (IA64_IA32_EXCEPTION_VECTOR + 0x40, 0x00,
         ld2(23, 3), nop_i(), nop_i()),
        (IA64_IA32_EXCEPTION_VECTOR + 0x50, 0x10,
         nop_m(), nop_i(),
         br_cond(IA64_IA32_EXCEPTION_VECTOR + 0x50,
                 IA64_IA32_EXCEPTION_VECTOR + 0x50)),
    ], {
        "ip": IA64_IA32_EXCEPTION_VECTOR + 0x50,
        "r15": 0x200,
        "r20": 0x303,
        "r21": (1 << 16) | (1 << 4),
        "r22": 0x303,
        "r23": 0x2200,
        "exception": IA64_EXCP_NONE,
    }, entry=0x700, cpu="madison")

test_ia32_unaligned_movaps_raises_gpf = require_registers(
    "ia32_unaligned_movaps_raises_gpf", [
        *ia32_environment_bundles(0x700, 0x10),
        # CFLG.fxsr and CFLG.mmxex (IA-32 CR4 bits 9 and 10).
        (0x10, *movl_mlx(3, ((1 << 9) | (1 << 10)) << 32)),
        (0x20, 0x00, mov_m_gr_ar(3, 27), nop_i(), nop_i()),
        (0x30, *movl_mlx(2, IA64_PSR_IC)),
        (0x40, 0x00, mov_gr_psr_full(2), nop_i(), nop_i()),
        (0x50, 0x00, srlz_d(), nop_i(), nop_i()),
        (0x60, *movl_mlx(8, 0x100)),
        (0x70, 0x00, nop_m(), mov_br_gr(7, 8), nop_i()),
        (0x80, 0x10, nop_m(), nop_i(), br_indirect(7, btype=1)),
        ia32_bundle(0x100, bytes.fromhex(
            "0f 28 06 01 02")),  # movaps xmm0,xmmword ptr [0x201]
        ia32_bundle(0x200, b""),
        (IA64_IA32_EXCEPTION_VECTOR, 0x00,
         mov_m_cr_gr(20, 19), nop_i(), nop_i()),
        (IA64_IA32_EXCEPTION_VECTOR + 0x10, 0x00,
         mov_m_cr_gr(21, 17), nop_i(), nop_i()),
        (IA64_IA32_EXCEPTION_VECTOR + 0x20, 0x00,
         mov_m_cr_gr(22, 22), nop_i(), nop_i()),
        (IA64_IA32_EXCEPTION_VECTOR + 0x30, 0x10,
         nop_m(), nop_i(),
         br_cond(IA64_IA32_EXCEPTION_VECTOR + 0x30,
                 IA64_IA32_EXCEPTION_VECTOR + 0x30)),
    ], {
        "ip": IA64_IA32_EXCEPTION_VECTOR + 0x30,
        "r20": 0x100,
        "r21": 13 << 16,
        "r22": 0x100,
        "exception": IA64_EXCP_NONE,
    }, entry=0x700, cpu="madison")

test_ia32_unmasked_sse_exception_is_precise_vector19 = require_registers(
    "ia32_unmasked_sse_exception_is_precise_vector19", [
        *ia32_environment_bundles(0x700, 0x10),
        # MXCSR: mask every exception except divide-by-zero.
        (0x10, *movl_mlx(3, 0x1d80 << 32)),
        (0x20, 0x00, mov_m_gr_ar(3, 21), nop_i(), nop_i()),
        # CFLG.fxsr and CFLG.mmxex (IA-32 CR4 bits 9 and 10).
        (0x30, *movl_mlx(4, ((1 << 9) | (1 << 10)) << 32)),
        (0x40, 0x00, mov_m_gr_ar(4, 27), nop_i(), nop_i()),
        (0x50, *movl_mlx(5, 0x112233443f800000)),
        (0x60, 0x00, setf_sig(16, 5), nop_i(), nop_i()),
        (0x70, 0x00, setf_sig(18, 0), nop_i(), nop_i()),
        (0x80, *movl_mlx(2, IA64_PSR_IC)),
        (0x90, 0x00, mov_gr_psr_full(2), nop_i(), nop_i()),
        (0xa0, 0x00, srlz_d(), nop_i(), nop_i()),
        (0xb0, *movl_mlx(8, 0x100)),
        (0xc0, 0x00, nop_m(), mov_br_gr(7, 8), nop_i()),
        (0xd0, 0x10, nop_m(), nop_i(), br_indirect(7, btype=1)),
        ia32_bundle(0x100, bytes.fromhex("f3 0f 5e c1")),  # divss xmm0,xmm1
        (IA64_IA32_EXCEPTION_VECTOR, 0x00,
         mov_m_cr_gr(8, 19), nop_i(), nop_i()),
        (IA64_IA32_EXCEPTION_VECTOR + 0x10, 0x00,
         mov_m_cr_gr(9, 17), nop_i(), nop_i()),
        (IA64_IA32_EXCEPTION_VECTOR + 0x20, 0x00,
         mov_m_cr_gr(10, 22), nop_i(), nop_i()),
        (IA64_IA32_EXCEPTION_VECTOR + 0x30, 0x00,
         mov_m_ar_gr(11, 28), nop_i(), nop_i()),
        (IA64_IA32_EXCEPTION_VECTOR + 0x40, 0x00,
         getf_sig(12, 16), nop_i(), nop_i()),
        (IA64_IA32_EXCEPTION_VECTOR + 0x50, 0x10,
         nop_m(), nop_i(),
         br_cond(IA64_IA32_EXCEPTION_VECTOR + 0x50,
                 IA64_IA32_EXCEPTION_VECTOR + 0x50)),
    ], {
        "ip": IA64_IA32_EXCEPTION_VECTOR + 0x50,
        "r8": 0x100,
        "r9": 19 << 16,
        "r10": 0x100,
        "r11": 1 << 34,
        "r12": 0x112233443f800000,
        "exception": IA64_EXCP_NONE,
    }, entry=0x700, cpu="madison")

test_ia32_masked_sse_exception_commits_result_and_status = \
    require_registers(
        "ia32_masked_sse_exception_commits_result_and_status", [
            *ia32_environment_bundles(0x700, 0x10),
            # MXCSR reset masks all six numeric exceptions.
            (0x10, *movl_mlx(3, 0x1f80 << 32)),
            (0x20, 0x00, mov_m_gr_ar(3, 21), nop_i(), nop_i()),
            (0x30, *movl_mlx(4, ((1 << 9) | (1 << 10)) << 32)),
            (0x40, 0x00, mov_m_gr_ar(4, 27), nop_i(), nop_i()),
            (0x50, *movl_mlx(5, 0x112233443f800000)),
            (0x60, 0x00, setf_sig(16, 5), nop_i(), nop_i()),
            (0x70, 0x00, setf_sig(18, 0), nop_i(), nop_i()),
            (0x80, *movl_mlx(2, IA64_PSR_IC)),
            (0x90, 0x00, mov_gr_psr_full(2), nop_i(), nop_i()),
            (0xa0, 0x00, srlz_d(), nop_i(), nop_i()),
            (0xb0, *movl_mlx(8, 0x100)),
            (0xc0, 0x00, nop_m(), mov_br_gr(7, 8), nop_i()),
            (0xd0, 0x10, nop_m(), nop_i(), br_indirect(7, btype=1)),
            ia32_bundle(0x100, bytes.fromhex(
                "f3 0f 5e c1 "  # divss xmm0,xmm1
                "0f b8 00 02")),  # jmpe 0x200
            (0x200, 0x00, getf_sig(8, 16), nop_i(), nop_i()),
            (0x210, 0x00, mov_m_ar_gr(9, 28), nop_i(), nop_i()),
            (0x220, 0x10, nop_m(), nop_i(), br_cond(0x220, 0x220)),
        ], {
            "ip": 0x220,
            "r8": 0x112233447f800000,
            "r9": 1 << 34,
            "exception": IA64_EXCP_NONE,
        }, entry=0x700, cpu="madison")

test_ia32_unmasked_sse_exception_is_masked_without_cflg_mmxex = \
    require_registers(
        "ia32_unmasked_sse_exception_is_masked_without_cflg_mmxex", [
            *ia32_environment_bundles(0x700, 0x10),
            # MXCSR unmasks divide-by-zero, but CFLG.mmxex remains clear.
            (0x10, *movl_mlx(3, 0x1d80 << 32)),
            (0x20, 0x00, mov_m_gr_ar(3, 21), nop_i(), nop_i()),
            (0x30, *movl_mlx(4, (1 << 9) << 32)),
            (0x40, 0x00, mov_m_gr_ar(4, 27), nop_i(), nop_i()),
            (0x50, *movl_mlx(5, 0x112233443f800000)),
            (0x60, 0x00, setf_sig(16, 5), nop_i(), nop_i()),
            (0x70, 0x00, setf_sig(18, 0), nop_i(), nop_i()),
            (0x80, *movl_mlx(2, IA64_PSR_IC)),
            (0x90, 0x00, mov_gr_psr_full(2), nop_i(), nop_i()),
            (0xa0, 0x00, srlz_d(), nop_i(), nop_i()),
            (0xb0, *movl_mlx(8, 0x100)),
            (0xc0, 0x00, nop_m(), mov_br_gr(7, 8), nop_i()),
            (0xd0, 0x10, nop_m(), nop_i(), br_indirect(7, btype=1)),
            ia32_bundle(0x100, bytes.fromhex(
                "f3 0f 5e c1 "  # divss xmm0,xmm1
                "0f b8 00 02")),  # jmpe 0x200
            (0x200, 0x00, getf_sig(8, 16), nop_i(), nop_i()),
            (0x210, 0x00, mov_m_ar_gr(9, 28), nop_i(), nop_i()),
            (0x220, 0x10, nop_m(), nop_i(), br_cond(0x220, 0x220)),
        ], {
            "ip": 0x220,
            "r8": 0x112233447f800000,
            "r9": 1 << 34,
            "exception": IA64_EXCP_NONE,
        }, entry=0x700, cpu="madison")

test_ia32_lock_check_allows_aligned_writeback_xchg = require_registers(
    "ia32_lock_check_allows_aligned_writeback_xchg", [
        *ia32_environment_bundles(0x700, 0x10),
        (0x10, *movl_mlx(2, IA64_DCR_LC)),
        (0x20, 0x00, mov_m_gr_cr(2, 0), nop_i(), nop_i()),
        (0x30, *movl_mlx(3, IA64_PSR_IC)),
        (0x40, 0x00, mov_gr_psr_full(3), nop_i(), nop_i()),
        (0x50, 0x00, srlz_d(), nop_i(), nop_i()),
        (0x60, *movl_mlx(8, 0x11223344)),
        (0x70, *movl_mlx(6, 0x100)),
        (0x80, 0x00, nop_m(), mov_br_gr(7, 6), nop_i()),
        (0x90, 0x10, nop_m(), nop_i(), br_indirect(7, btype=1)),
        ia32_bundle(0x100, bytes.fromhex(
            "66 87 06 08 02 "  # xchg eax,dword ptr [0x208]
            "0f b8 00 03")),   # jmpe 0x300
        ia32_bundle(0x200, bytes.fromhex(
            "00 00 00 00 00 00 00 00 78 56 34 12 00 00 00 00")),
        (0x300, 0x10, nop_m(), nop_i(), br_cond(0x300, 0x300)),
    ], {
        "ip": 0x300,
        "r1": 0x109,
        "r8": 0x12345678,
        "exception": IA64_EXCP_NONE,
    }, entry=0x700, cpu="madison")

test_ia32_lock_intercept_on_8byte_boundary_crossing = require_registers(
    "ia32_lock_intercept_on_8byte_boundary_crossing", [
        *ia32_environment_bundles(0x700, 0x10),
        (0x10, *movl_mlx(2, IA64_DCR_LC)),
        (0x20, 0x00, mov_m_gr_cr(2, 0), nop_i(), nop_i()),
        (0x30, *movl_mlx(3, IA64_PSR_IC)),
        (0x40, 0x00, mov_gr_psr_full(3), nop_i(), nop_i()),
        (0x50, 0x00, srlz_d(), nop_i(), nop_i()),
        (0x60, *movl_mlx(6, 0x100)),
        (0x70, 0x00, nop_m(), mov_br_gr(7, 6), nop_i()),
        (0x80, 0x10, nop_m(), nop_i(), br_indirect(7, btype=1)),
        ia32_bundle(0x100, bytes.fromhex(
            "66 87 06 07 02")),  # xchg eax,dword ptr [0x207]
        (IA64_IA32_INTERCEPT_VECTOR, 0x00,
         mov_m_cr_gr(8, 19), nop_i(), nop_i()),
        (IA64_IA32_INTERCEPT_VECTOR + 0x10, 0x00,
         mov_m_cr_gr(9, 17), nop_i(), nop_i()),
        (IA64_IA32_INTERCEPT_VECTOR + 0x20, 0x00,
         mov_m_cr_gr(10, 20), nop_i(), nop_i()),
        (IA64_IA32_INTERCEPT_VECTOR + 0x30, 0x00,
         mov_m_cr_gr(11, 22), nop_i(), nop_i()),
        (IA64_IA32_INTERCEPT_VECTOR + 0x40, 0x10,
         nop_m(), nop_i(),
         br_cond(IA64_IA32_INTERCEPT_VECTOR + 0x40,
                 IA64_IA32_INTERCEPT_VECTOR + 0x40)),
    ], {
        "ip": IA64_IA32_INTERCEPT_VECTOR + 0x40,
        "r8": 0x100,
        "r9": 4 << 16,
        "r10": 0x207,
        "r11": 0x100,
        "exception": IA64_EXCP_NONE,
    }, entry=0x700, cpu="madison")

test_ia32_invalid_csd_faults_at_target_fetch = require_registers(
    "ia32_invalid_csd_faults_at_target_fetch", [
        (0x10, *movl_mlx(2, IA64_PSR_IC)),
        (0x20, 0x00, mov_gr_psr_full(2), nop_i(), nop_i()),
        (0x30, 0x00, srlz_d(), nop_i(), nop_i()),
        (0x40, *movl_mlx(8, 0x100)),
        (0x50, 0x00, nop_m(), mov_br_gr(7, 8), nop_i()),
        (0x60, 0x10, nop_m(), nop_i(), br_indirect(7, btype=1)),
        ia32_bundle(0x100, b"\x90"),
        (IA64_IA32_EXCEPTION_VECTOR, 0x00,
         mov_m_cr_gr(8, 19), nop_i(), nop_i()),
        (IA64_IA32_EXCEPTION_VECTOR + 0x10, 0x00,
         mov_m_cr_gr(9, 17), nop_i(), nop_i()),
        (IA64_IA32_EXCEPTION_VECTOR + 0x20, 0x00,
         mov_m_cr_gr(10, 22), nop_i(), nop_i()),
        (IA64_IA32_EXCEPTION_VECTOR + 0x30, 0x10,
         nop_m(), nop_i(),
         br_cond(IA64_IA32_EXCEPTION_VECTOR + 0x30,
                 IA64_IA32_EXCEPTION_VECTOR + 0x30)),
    ], {
        "ip": IA64_IA32_EXCEPTION_VECTOR + 0x30,
        "r8": 0x100,
        "r9": 13 << 16,
        "r10": 0x100,
        "exception": IA64_EXCP_NONE,
    }, entry=0x10, cpu="madison")

test_ia32_instruction_crossing_cs_limit_faults = require_registers(
    "ia32_instruction_crossing_cs_limit_faults", [
        *ia32_environment_bundles(
            0x700, 0x10, csd=0x09b0010000000000),
        (0x10, *movl_mlx(2, IA64_PSR_IC)),
        (0x20, 0x00, mov_gr_psr_full(2), nop_i(), nop_i()),
        (0x30, 0x00, srlz_d(), nop_i(), nop_i()),
        (0x40, *movl_mlx(8, 0x100)),
        (0x50, 0x00, nop_m(), mov_br_gr(7, 8), nop_i()),
        (0x60, 0x10, nop_m(), nop_i(), br_indirect(7, btype=1)),
        ia32_bundle(0x100, bytes.fromhex("66 90")),
        (IA64_IA32_EXCEPTION_VECTOR, 0x00,
         mov_m_cr_gr(8, 19), nop_i(), nop_i()),
        (IA64_IA32_EXCEPTION_VECTOR + 0x10, 0x00,
         mov_m_cr_gr(9, 17), nop_i(), nop_i()),
        (IA64_IA32_EXCEPTION_VECTOR + 0x20, 0x00,
         mov_m_cr_gr(10, 22), nop_i(), nop_i()),
        (IA64_IA32_EXCEPTION_VECTOR + 0x30, 0x10,
         nop_m(), nop_i(),
         br_cond(IA64_IA32_EXCEPTION_VECTOR + 0x30,
                 IA64_IA32_EXCEPTION_VECTOR + 0x30)),
    ], {
        "ip": IA64_IA32_EXCEPTION_VECTOR + 0x30,
        "r8": 0x100,
        "r9": 13 << 16,
        "r10": 0x100,
        "exception": IA64_EXCP_NONE,
    }, entry=0x700, cpu="madison")

test_ia32_ds_operand_crossing_limit_faults = require_registers(
    "ia32_ds_operand_crossing_limit_faults", [
        *ia32_environment_bundles(
            0x700, 0x10, dsd=0x0930020200000000),
        (0x10, *movl_mlx(2, IA64_PSR_IC)),
        (0x20, 0x00, mov_gr_psr_full(2), nop_i(), nop_i()),
        (0x30, 0x00, srlz_d(), nop_i(), nop_i()),
        (0x40, *movl_mlx(8, 0x100)),
        (0x50, 0x00, nop_m(), mov_br_gr(7, 8), nop_i()),
        (0x60, 0x10, nop_m(), nop_i(), br_indirect(7, btype=1)),
        ia32_bundle(0x100, bytes.fromhex(
            "66 a1 01 02 00 00")),  # mov eax,dword ptr [0x201]
        (IA64_IA32_EXCEPTION_VECTOR, 0x00,
         mov_m_cr_gr(8, 19), nop_i(), nop_i()),
        (IA64_IA32_EXCEPTION_VECTOR + 0x10, 0x00,
         mov_m_cr_gr(9, 17), nop_i(), nop_i()),
        (IA64_IA32_EXCEPTION_VECTOR + 0x20, 0x00,
         mov_m_cr_gr(10, 22), nop_i(), nop_i()),
        (IA64_IA32_EXCEPTION_VECTOR + 0x30, 0x10,
         nop_m(), nop_i(),
         br_cond(IA64_IA32_EXCEPTION_VECTOR + 0x30,
                 IA64_IA32_EXCEPTION_VECTOR + 0x30)),
    ], {
        "ip": IA64_IA32_EXCEPTION_VECTOR + 0x30,
        "r8": 0x100,
        "r9": 13 << 16,
        "r10": 0x100,
        "exception": IA64_EXCP_NONE,
    }, entry=0x700, cpu="madison")

test_ia32_read_only_ds_rejects_store = require_registers(
    "ia32_read_only_ds_rejects_store", [
        *ia32_environment_bundles(
            0x700, 0x10, dsd=0x0910ffff00000000),
        (0x10, *movl_mlx(2, IA64_PSR_IC)),
        (0x20, 0x00, mov_gr_psr_full(2), nop_i(), nop_i()),
        (0x30, 0x00, srlz_d(), nop_i(), nop_i()),
        (0x40, *movl_mlx(8, 0x100)),
        (0x50, 0x00, nop_m(), mov_br_gr(7, 8), nop_i()),
        (0x60, 0x10, nop_m(), nop_i(), br_indirect(7, btype=1)),
        ia32_bundle(0x100, bytes.fromhex(
            "66 a3 00 02 00 00")),  # mov dword ptr [0x200],eax
        (IA64_IA32_EXCEPTION_VECTOR, 0x00,
         mov_m_cr_gr(8, 19), nop_i(), nop_i()),
        (IA64_IA32_EXCEPTION_VECTOR + 0x10, 0x00,
         mov_m_cr_gr(9, 17), nop_i(), nop_i()),
        (IA64_IA32_EXCEPTION_VECTOR + 0x20, 0x00,
         mov_m_cr_gr(10, 22), nop_i(), nop_i()),
        (IA64_IA32_EXCEPTION_VECTOR + 0x30, 0x10,
         nop_m(), nop_i(),
         br_cond(IA64_IA32_EXCEPTION_VECTOR + 0x30,
                 IA64_IA32_EXCEPTION_VECTOR + 0x30)),
    ], {
        "ip": IA64_IA32_EXCEPTION_VECTOR + 0x30,
        "r8": 0x100,
        "r9": 13 << 16,
        "r10": 0x100,
        "exception": IA64_EXCP_NONE,
    }, entry=0x700, cpu="madison")

test_ia32_ss_dpl_mismatch_faults_push = require_registers(
    "ia32_ss_dpl_mismatch_faults_push", [
        *ia32_environment_bundles(
            0x700, 0x10, ssd=0x0f30ffff00000000),
        (0x10, *movl_mlx(2, IA64_PSR_IC)),
        (0x20, 0x00, mov_gr_psr_full(2), nop_i(), nop_i()),
        (0x30, 0x00, srlz_d(), nop_i(), nop_i()),
        (0x40, *movl_mlx(8, 0x100)),
        (0x50, 0x00, nop_m(), mov_br_gr(7, 8), nop_i()),
        (0x60, 0x10, nop_m(), nop_i(), br_indirect(7, btype=1)),
        ia32_bundle(0x100, b"\x50"),  # push ax
        (IA64_IA32_EXCEPTION_VECTOR, 0x00,
         mov_m_cr_gr(8, 19), nop_i(), nop_i()),
        (IA64_IA32_EXCEPTION_VECTOR + 0x10, 0x00,
         mov_m_cr_gr(9, 17), nop_i(), nop_i()),
        (IA64_IA32_EXCEPTION_VECTOR + 0x20, 0x00,
         mov_m_cr_gr(10, 22), nop_i(), nop_i()),
        (IA64_IA32_EXCEPTION_VECTOR + 0x30, 0x10,
         nop_m(), nop_i(),
         br_cond(IA64_IA32_EXCEPTION_VECTOR + 0x30,
                 IA64_IA32_EXCEPTION_VECTOR + 0x30)),
    ], {
        "ip": IA64_IA32_EXCEPTION_VECTOR + 0x30,
        "r8": 0x100,
        "r9": 12 << 16,
        "r10": 0x100,
        "exception": IA64_EXCP_NONE,
    }, entry=0x700, cpu="madison")

test_ia32_x87_operand_crossing_ds_limit_faults = require_registers(
    "ia32_x87_operand_crossing_ds_limit_faults", [
        *ia32_environment_bundles(
            0x700, 0x10, dsd=0x0930020500000000),
        (0x10, *movl_mlx(2, IA64_PSR_IC)),
        (0x20, 0x00, mov_gr_psr_full(2), nop_i(), nop_i()),
        (0x30, 0x00, srlz_d(), nop_i(), nop_i()),
        (0x40, *movl_mlx(8, 0x100)),
        (0x50, 0x00, nop_m(), mov_br_gr(7, 8), nop_i()),
        (0x60, 0x10, nop_m(), nop_i(), br_indirect(7, btype=1)),
        ia32_bundle(0x100, bytes.fromhex(
            "db 2e 00 02")),  # fld tbyte ptr [0x200]
        (IA64_IA32_EXCEPTION_VECTOR, 0x00,
         mov_m_cr_gr(8, 19), nop_i(), nop_i()),
        (IA64_IA32_EXCEPTION_VECTOR + 0x10, 0x00,
         mov_m_cr_gr(9, 17), nop_i(), nop_i()),
        (IA64_IA32_EXCEPTION_VECTOR + 0x20, 0x00,
         mov_m_cr_gr(10, 22), nop_i(), nop_i()),
        (IA64_IA32_EXCEPTION_VECTOR + 0x30, 0x10,
         nop_m(), nop_i(),
         br_cond(IA64_IA32_EXCEPTION_VECTOR + 0x30,
                 IA64_IA32_EXCEPTION_VECTOR + 0x30)),
    ], {
        "ip": IA64_IA32_EXCEPTION_VECTOR + 0x30,
        "r8": 0x100,
        "r9": 13 << 16,
        "r10": 0x100,
        "exception": IA64_EXCP_NONE,
    }, entry=0x700, cpu="madison")

test_ia32_fxsave_checks_entire_512_byte_segment_operand = \
    require_registers(
        "ia32_fxsave_checks_entire_512_byte_segment_operand", [
            *ia32_environment_bundles(
                0x700, 0x10,
                csd=IA32_TEST_CSD | (1 << 62),
                dsd=0x0930030300000000),
            (0x10, *movl_mlx(2, IA64_PSR_IC)),
            (0x20, 0x00, mov_gr_psr_full(2), nop_i(), nop_i()),
            (0x30, 0x00, srlz_d(), nop_i(), nop_i()),
            (0x40, *movl_mlx(8, 0x100)),
            (0x50, 0x00, nop_m(), mov_br_gr(7, 8), nop_i()),
            (0x60, 0x10, nop_m(), nop_i(), br_indirect(7, btype=1)),
            ia32_bundle(0x100, bytes.fromhex(
                "0f ae 05 00 03 00 00 "  # fxsave [0x300]
                "0f b8 00 02 00 00")),   # jmpe 0x200 if no #GP
            (0x200, 0x10, nop_m(), nop_i(), br_cond(0x200, 0x200)),
            (IA64_IA32_EXCEPTION_VECTOR, 0x00,
             mov_m_cr_gr(8, 19), nop_i(), nop_i()),
            (IA64_IA32_EXCEPTION_VECTOR + 0x10, 0x00,
             mov_m_cr_gr(9, 17), nop_i(), nop_i()),
            (IA64_IA32_EXCEPTION_VECTOR + 0x20, 0x00,
             mov_m_cr_gr(10, 22), nop_i(), nop_i()),
            (IA64_IA32_EXCEPTION_VECTOR + 0x30, 0x10,
             nop_m(), nop_i(),
             br_cond(IA64_IA32_EXCEPTION_VECTOR + 0x30,
                     IA64_IA32_EXCEPTION_VECTOR + 0x30)),
        ], {
            "ip": IA64_IA32_EXCEPTION_VECTOR + 0x30,
            "r8": 0x100,
            "r9": 13 << 16,
            "r10": 0x100,
            "exception": IA64_EXCP_NONE,
        }, entry=0x700, cpu="madison")

test_ia32_bound_checks_second_element_against_segment_limit = \
    require_registers(
        "ia32_bound_checks_second_element_against_segment_limit", [
            *ia32_environment_bundles(
                0x700, 0x10, dsd=0x0930030100000000),
            (0x10, *movl_mlx(2, IA64_PSR_IC)),
            (0x20, 0x00, mov_gr_psr_full(2), nop_i(), nop_i()),
            (0x30, 0x00, srlz_d(), nop_i(), nop_i()),
            (0x40, *movl_mlx(8, 0x100)),
            (0x50, 0x00, nop_m(), mov_br_gr(7, 8), nop_i()),
            (0x60, 0x10, nop_m(), nop_i(), br_indirect(7, btype=1)),
            ia32_bundle(0x100, bytes.fromhex(
                "b8 05 00 "       # mov ax,5
                "62 06 00 03 "    # upper word crosses DS.limit
                "0f b8 00 02")),  # jmpe 0x200 if no #GP
            (0x200, 0x10, nop_m(), nop_i(), br_cond(0x200, 0x200)),
            ia32_bundle(0x300, bytes.fromhex("00 00 0a 00")),
            (IA64_IA32_EXCEPTION_VECTOR, 0x00,
             mov_m_cr_gr(8, 19), nop_i(), nop_i()),
            (IA64_IA32_EXCEPTION_VECTOR + 0x10, 0x00,
             mov_m_cr_gr(9, 17), nop_i(), nop_i()),
            (IA64_IA32_EXCEPTION_VECTOR + 0x20, 0x00,
             mov_m_cr_gr(10, 22), nop_i(), nop_i()),
            (IA64_IA32_EXCEPTION_VECTOR + 0x30, 0x10,
             nop_m(), nop_i(),
             br_cond(IA64_IA32_EXCEPTION_VECTOR + 0x30,
                     IA64_IA32_EXCEPTION_VECTOR + 0x30)),
        ], {
            "ip": IA64_IA32_EXCEPTION_VECTOR + 0x30,
            "r8": 0x103,
            "r9": 13 << 16,
            "r10": 0x103,
            "exception": IA64_EXCP_NONE,
        }, entry=0x700, cpu="madison")

test_ia32_sti_system_flag_intercept_is_post_instruction = \
    require_registers(
        "ia32_sti_system_flag_intercept_is_post_instruction", [
            *ia32_environment_bundles(0x700, 0x10),
            (0x10, *movl_mlx(3, 1 << 8)),
            (0x20, 0x00, mov_m_gr_ar(3, 27), nop_i(), nop_i()),
            (0x30, *movl_mlx(2, IA64_PSR_IC)),
            (0x40, 0x00, mov_gr_psr_full(2), nop_i(), nop_i()),
            (0x50, 0x00, srlz_d(), nop_i(), nop_i()),
            (0x60, *movl_mlx(8, 0x100)),
            (0x70, 0x00, nop_m(), mov_br_gr(7, 8), nop_i()),
            (0x80, 0x10, nop_m(), nop_i(),
             br_indirect(7, btype=1)),
            ia32_bundle(0x100, b"\xfb"),  # sti
            (IA64_IA32_INTERCEPT_VECTOR, 0x00,
             mov_m_cr_gr(8, 19), nop_i(), nop_i()),
            (IA64_IA32_INTERCEPT_VECTOR + 0x10, 0x00,
             mov_m_cr_gr(9, 17), nop_i(), nop_i()),
            (IA64_IA32_INTERCEPT_VECTOR + 0x20, 0x00,
             mov_m_cr_gr(10, 24), nop_i(), nop_i()),
            (IA64_IA32_INTERCEPT_VECTOR + 0x30, 0x00,
             mov_m_cr_gr(11, 22), nop_i(), nop_i()),
            (IA64_IA32_INTERCEPT_VECTOR + 0x40, 0x10,
             nop_m(), nop_i(),
             br_cond(IA64_IA32_INTERCEPT_VECTOR + 0x40,
                     IA64_IA32_INTERCEPT_VECTOR + 0x40)),
        ], {
            "ip": IA64_IA32_INTERCEPT_VECTOR + 0x40,
            "r8": 0x101,
            "r9": 0x24000,
            "r10": 2,
            "r11": 0x100,
            "exception": IA64_EXCP_NONE,
        }, entry=0x700, cpu="madison")

test_ia32_int3_clears_rf_and_psr_id_after_completion = require_registers(
    "ia32_int3_clears_rf_and_psr_id_after_completion", [
        *ia32_environment_bundles(0x700, 0x10),
        (0x10, *movl_mlx(3, (1 << 16) | 2)),
        (0x20, 0x00, mov_m_gr_ar(3, 24), nop_i(), nop_i()),
        (0x30, *movl_mlx(
            2, IA64_PSR_IC | IA64_PSR_IS | (1 << 37))),
        (0x40, *movl_mlx(3, 0x100)),
        *rfi_to_gr(0x50, 2, 3),
        ia32_bundle(0x100, bytes.fromhex("cc")),  # int3
        (IA64_IA32_EXCEPTION_VECTOR, 0x00,
         mov_m_cr_gr(8, 19), nop_i(), nop_i()),
        (IA64_IA32_EXCEPTION_VECTOR + 0x10, 0x00,
         mov_m_cr_gr(9, 22), nop_i(), nop_i()),
        (IA64_IA32_EXCEPTION_VECTOR + 0x20, 0x00,
         mov_m_cr_gr(10, 17), nop_i(), nop_i()),
        (IA64_IA32_EXCEPTION_VECTOR + 0x30, 0x00,
         mov_m_ar_gr(11, 24), nop_i(), nop_i()),
        (IA64_IA32_EXCEPTION_VECTOR + 0x40, 0x00,
         mov_m_cr_gr(12, 16), nop_i(), nop_i()),
        (IA64_IA32_EXCEPTION_VECTOR + 0x50, 0x02,
         nop_m(), extr_u(12, 12, 37, 1), nop_i()),
        (IA64_IA32_EXCEPTION_VECTOR + 0x60, 0x10,
         nop_m(), nop_i(),
         br_cond(IA64_IA32_EXCEPTION_VECTOR + 0x60,
                 IA64_IA32_EXCEPTION_VECTOR + 0x60)),
    ], {
        "ip": IA64_IA32_EXCEPTION_VECTOR + 0x60,
        "r8": 0x101,
        "r9": 0x100,
        "r10": 3 << 16,
        "r11": 2,
        "r12": 0,
        "exception": IA64_EXCP_NONE,
    }, entry=0x700, cpu="madison")

test_br_ia_preserves_rf_and_psr_id_until_target_completes = \
    require_registers(
        "br_ia_preserves_rf_and_psr_id_until_target_completes", [
            *ia32_environment_bundles(0x700, 0x10),
            (0x10, *movl_mlx(3, (1 << 16) | 2)),
            (0x20, 0x00, mov_m_gr_ar(3, 24), nop_i(), nop_i()),
            (0x30, *movl_mlx(4, 0x200)),
            (0x40, 0x00, nop_m(), mov_br_gr(7, 4), nop_i()),
            # Only rfi can restore PSR.id.  Make br.ia the first restart
            # instruction so ordinary IA-64 completion cannot clear it.
            (0x50, *movl_mlx(2, IA64_PSR_IC | (1 << 37))),
            (0x60, *movl_mlx(3, 0x100)),
            *rfi_to_gr(0x70, 2, 3),
            (0x100, 0x16, br_indirect(7, btype=1), nop_b(), nop_b()),
            ia32_bundle(0x200, bytes.fromhex("0f 0b")),  # UD2 intercept
            (IA64_IA32_INTERCEPT_VECTOR, 0x00,
             mov_m_cr_gr(8, 16), nop_i(), nop_i()),
            (IA64_IA32_INTERCEPT_VECTOR + 0x10, 0x02,
             mov_m_ar_gr(10, 24), extr_u(9, 8, 37, 1), nop_i()),
            (IA64_IA32_INTERCEPT_VECTOR + 0x20, 0x10,
             nop_m(), nop_i(),
             br_cond(IA64_IA32_INTERCEPT_VECTOR + 0x20,
                     IA64_IA32_INTERCEPT_VECTOR + 0x20)),
        ], {
            "ip": IA64_IA32_INTERCEPT_VECTOR + 0x20,
            "r9": 1,
            "r10": (1 << 16) | 2,
            "exception": IA64_EXCP_NONE,
        }, entry=0x700, cpu="madison")

test_br_ia_unimplemented_target_preserves_64bit_iip = require_registers(
    "br_ia_unimplemented_target_preserves_64bit_iip", [
        *ia32_environment_bundles(0x700, 0x10),
        (0x10, *movl_mlx(2, IA64_PSR_IC | IA64_PSR_TB)),
        (0x20, 0x00, mov_gr_psr_full(2), nop_i(), nop_i()),
        (0x30, 0x00, srlz_d(), nop_i(), nop_i()),
        (0x40, *movl_mlx(8, (1 << IA64_IMPL_PA_BITS) | 0x100)),
        (0x50, 0x00, nop_m(), mov_br_gr(7, 8), nop_i()),
        (0x60, 0x10, nop_m(), nop_i(), br_indirect(7, btype=1)),
        (IA64_LOWER_PRIV_TRANSFER_VECTOR, 0x00,
         mov_m_cr_gr(8, 19), nop_i(), nop_i()),
        (IA64_LOWER_PRIV_TRANSFER_VECTOR + 0x10, 0x00,
         mov_m_cr_gr(9, 22), nop_i(), nop_i()),
        (IA64_LOWER_PRIV_TRANSFER_VECTOR + 0x20, 0x00,
         mov_m_cr_gr(10, 17), nop_i(), nop_i()),
        (IA64_LOWER_PRIV_TRANSFER_VECTOR + 0x30, 0x00,
         mov_m_cr_gr(11, 16), nop_i(), nop_i()),
        (IA64_LOWER_PRIV_TRANSFER_VECTOR + 0x40, 0x02,
         nop_m(), extr_u(12, 11, 34, 1), nop_i()),
        (IA64_LOWER_PRIV_TRANSFER_VECTOR + 0x50, 0x10,
         nop_m(), nop_i(),
         br_cond(IA64_LOWER_PRIV_TRANSFER_VECTOR + 0x50,
                 IA64_LOWER_PRIV_TRANSFER_VECTOR + 0x50)),
    ], {
        "ip": IA64_LOWER_PRIV_TRANSFER_VECTOR + 0x50,
        "r8": (1 << IA64_IMPL_PA_BITS) | 0x100,
        "r9": 0x60,
        "r10": IA64_ISR_CODE_UI | IA64_ISR_CODE_TB |
               (2 << IA64_ISR_EI_SHIFT),
        "r12": 1,
        "exception": IA64_EXCP_NONE,
    }, entry=0x700, cpu="madison")

test_br_ia_taken_branch_trap_precedes_single_step = require_registers(
    "br_ia_taken_branch_trap_precedes_single_step", [
        *ia32_environment_bundles(0x700, 0x10),
        (0x10, *movl_mlx(8, 0x100)),
        (0x20, 0x00, nop_m(), mov_br_gr(7, 8), nop_i()),
        (0x30, *movl_mlx(
            2, IA64_PSR_IC | IA64_PSR_TB | IA64_PSR_SS)),
        (0x40, *movl_mlx(3, 0x80)),
        *rfi_to_gr(0x50, 2, 3),
        (0x80, 0x16, br_indirect(7, btype=1), nop_b(), nop_b()),
        (IA64_TAKEN_BRANCH_VECTOR, 0x00,
         mov_m_cr_gr(8, 19), nop_i(), nop_i()),
        (IA64_TAKEN_BRANCH_VECTOR + 0x10, 0x00,
         mov_m_cr_gr(9, 22), nop_i(), nop_i()),
        (IA64_TAKEN_BRANCH_VECTOR + 0x20, 0x00,
         mov_m_cr_gr(10, 17), nop_i(), nop_i()),
        (IA64_TAKEN_BRANCH_VECTOR + 0x30, 0x00,
         mov_m_cr_gr(11, 16), nop_i(), nop_i()),
        (IA64_TAKEN_BRANCH_VECTOR + 0x40, 0x02,
         nop_m(), extr_u(12, 11, 34, 1), nop_i()),
        (IA64_TAKEN_BRANCH_VECTOR + 0x50, 0x10,
         nop_m(), nop_i(),
         br_cond(IA64_TAKEN_BRANCH_VECTOR + 0x50,
                 IA64_TAKEN_BRANCH_VECTOR + 0x50)),
    ], {
        "ip": IA64_TAKEN_BRANCH_VECTOR + 0x50,
        "r8": 0x100,
        "r9": 0x80,
        "r10": IA64_ISR_CODE_TB | IA64_ISR_CODE_SS,
        "r12": 1,
        "exception": IA64_EXCP_NONE,
    }, entry=0x700, cpu="madison")

test_br_ia_single_step_trap = require_registers(
    "br_ia_single_step_trap", [
        *ia32_environment_bundles(0x700, 0x10),
        (0x10, *movl_mlx(8, 0x100)),
        (0x20, 0x00, nop_m(), mov_br_gr(7, 8), nop_i()),
        (0x30, *movl_mlx(2, IA64_PSR_IC | IA64_PSR_SS)),
        (0x40, *movl_mlx(3, 0x80)),
        *rfi_to_gr(0x50, 2, 3),
        (0x80, 0x16, br_indirect(7, btype=1), nop_b(), nop_b()),
        (IA64_SINGLE_STEP_VECTOR, 0x00,
         mov_m_cr_gr(8, 19), nop_i(), nop_i()),
        (IA64_SINGLE_STEP_VECTOR + 0x10, 0x00,
         mov_m_cr_gr(9, 22), nop_i(), nop_i()),
        (IA64_SINGLE_STEP_VECTOR + 0x20, 0x00,
         mov_m_cr_gr(10, 17), nop_i(), nop_i()),
        (IA64_SINGLE_STEP_VECTOR + 0x30, 0x00,
         mov_m_cr_gr(11, 16), nop_i(), nop_i()),
        (IA64_SINGLE_STEP_VECTOR + 0x40, 0x02,
         nop_m(), extr_u(12, 11, 34, 1), nop_i()),
        (IA64_SINGLE_STEP_VECTOR + 0x50, 0x10,
         nop_m(), nop_i(),
         br_cond(IA64_SINGLE_STEP_VECTOR + 0x50,
                 IA64_SINGLE_STEP_VECTOR + 0x50)),
    ], {
        "ip": IA64_SINGLE_STEP_VECTOR + 0x50,
        "r8": 0x100,
        "r9": 0x80,
        "r10": IA64_ISR_CODE_SS,
        "r12": 1,
        "exception": IA64_EXCP_NONE,
    }, entry=0x700, cpu="madison")

test_rfi_to_ia32_clears_fault_suppression_but_preserves_psr_id = \
    require_registers(
        "rfi_to_ia32_clears_fault_suppression_but_preserves_psr_id", [
            *ia32_environment_bundles(0x700, 0x10),
            # id remains until one IA-32 instruction completes; da/dd/ed/ia
            # are cleared before the target instruction begins.
            (0x10, *movl_mlx(
                2, IA64_PSR_IC | IA64_PSR_IS |
                    (1 << 37) | (1 << 38) | (1 << 39) |
                    (1 << 43) | (1 << 45))),
            (0x20, *movl_mlx(3, 0x100)),
            *rfi_to_gr(0x30, 2, 3),
            ia32_bundle(0x100, bytes.fromhex("0f 0b")),  # UD2 intercept
            (IA64_IA32_INTERCEPT_VECTOR, 0x00,
             mov_m_cr_gr(8, 16), nop_i(), nop_i()),
            (IA64_IA32_INTERCEPT_VECTOR + 0x10, 0x02,
             nop_m(), extr_u(9, 8, 37, 1), nop_i()),
            (IA64_IA32_INTERCEPT_VECTOR + 0x20, 0x02,
             nop_m(), extr_u(10, 8, 38, 2), nop_i()),
            (IA64_IA32_INTERCEPT_VECTOR + 0x30, 0x02,
             nop_m(), extr_u(11, 8, 43, 1), nop_i()),
            (IA64_IA32_INTERCEPT_VECTOR + 0x40, 0x02,
             nop_m(), extr_u(12, 8, 45, 1), nop_i()),
            (IA64_IA32_INTERCEPT_VECTOR + 0x50, 0x10,
             nop_m(), nop_i(),
             br_cond(IA64_IA32_INTERCEPT_VECTOR + 0x50,
                     IA64_IA32_INTERCEPT_VECTOR + 0x50)),
        ], {
            "ip": IA64_IA32_INTERCEPT_VECTOR + 0x50,
            "r9": 1,
            "r10": 0,
            "r11": 0,
            "r12": 0,
            "exception": IA64_EXCP_NONE,
        }, entry=0x700, cpu="madison")

test_rfi_to_ia32_unimplemented_target_preserves_64bit_iip = \
    require_registers(
        "rfi_to_ia32_unimplemented_target_preserves_64bit_iip", [
            *ia32_environment_bundles(0x700, 0x10),
            (0x10, *movl_mlx(2, IA64_PSR_IC | IA64_PSR_IS)),
            (0x20, *movl_mlx(
                3, (1 << IA64_IMPL_PA_BITS) | 0x100)),
            *rfi_to_gr(0x30, 2, 3),
            (IA64_LOWER_PRIV_TRANSFER_VECTOR, 0x00,
             mov_m_cr_gr(8, 19), nop_i(), nop_i()),
            (IA64_LOWER_PRIV_TRANSFER_VECTOR + 0x10, 0x00,
             mov_m_cr_gr(9, 22), nop_i(), nop_i()),
            (IA64_LOWER_PRIV_TRANSFER_VECTOR + 0x20, 0x00,
             mov_m_cr_gr(10, 17), nop_i(), nop_i()),
            (IA64_LOWER_PRIV_TRANSFER_VECTOR + 0x30, 0x00,
             mov_m_cr_gr(11, 16), nop_i(), nop_i()),
            (IA64_LOWER_PRIV_TRANSFER_VECTOR + 0x40, 0x02,
             nop_m(), extr_u(12, 11, 34, 1), nop_i()),
            (IA64_LOWER_PRIV_TRANSFER_VECTOR + 0x50, 0x10,
             nop_m(), nop_i(),
             br_cond(IA64_LOWER_PRIV_TRANSFER_VECTOR + 0x50,
                     IA64_LOWER_PRIV_TRANSFER_VECTOR + 0x50)),
        ], {
            "ip": IA64_LOWER_PRIV_TRANSFER_VECTOR + 0x50,
            "r8": (1 << IA64_IMPL_PA_BITS) | 0x100,
            "r9": 0x40,
            "r10": IA64_ISR_CODE_UI | (2 << IA64_ISR_EI_SHIFT),
            "r12": 1,
            "exception": IA64_EXCP_NONE,
        }, entry=0x700, cpu="madison")

test_ia32_taken_branch_clears_rf_and_psr_id = require_registers(
    "ia32_taken_branch_clears_rf_and_psr_id", [
        *ia32_environment_bundles(0x700, 0x10),
        (0x10, *movl_mlx(3, (1 << 16) | 2)),
        (0x20, 0x00, mov_m_gr_ar(3, 24), nop_i(), nop_i()),
        (0x30, *movl_mlx(
            2, IA64_PSR_IC | IA64_PSR_IS | (1 << 37))),
        (0x40, *movl_mlx(3, 0x100)),
        *rfi_to_gr(0x50, 2, 3),
        ia32_bundle(0x100, bytes.fromhex(
            "eb 02 "       # jmp 0x104
            "90 90 "       # skipped
            "0f 0b")),     # ud2 -> instruction intercept
        (IA64_IA32_INTERCEPT_VECTOR, 0x00,
         mov_m_ar_gr(8, 24), nop_i(), nop_i()),
        (IA64_IA32_INTERCEPT_VECTOR + 0x10, 0x00,
         mov_m_cr_gr(9, 16), nop_i(), nop_i()),
        (IA64_IA32_INTERCEPT_VECTOR + 0x20, 0x00,
         nop_m(), extr_u(9, 9, 37, 1), nop_i()),
        (IA64_IA32_INTERCEPT_VECTOR + 0x30, 0x10,
         nop_m(), nop_i(),
         br_cond(IA64_IA32_INTERCEPT_VECTOR + 0x30,
                 IA64_IA32_INTERCEPT_VECTOR + 0x30)),
    ], {
        "ip": IA64_IA32_INTERCEPT_VECTOR + 0x30,
        "r8": 2,
        "r9": 0,
        "exception": IA64_EXCP_NONE,
    }, entry=0x700, cpu="madison")

test_ia32_not_taken_branch_clears_rf_and_psr_id = require_registers(
    "ia32_not_taken_branch_clears_rf_and_psr_id", [
        *ia32_environment_bundles(0x700, 0x10),
        # Break on the instruction following a non-taken JNZ.  Both RF and
        # PSR.id suppress the first fetch and must be cleared when JNZ
        # completes, so the following NOP raises an instruction debug fault.
        (0x10, *movl_mlx(4, 0)),
        (0x20, *movl_mlx(5, 0x102)),
        (0x30, 0x00, mov_ibr_indexed_write(4, 5), nop_i(), nop_i()),
        (0x40, 0x00, nop_m(), adds(4, 1, 0), nop_i()),
        (0x50, *movl_mlx(5, 0x81000000ffffffff)),
        (0x60, 0x00, mov_ibr_indexed_write(4, 5), nop_i(), nop_i()),
        (0x70, 0x00, srlz_i(), nop_i(), nop_i()),
        (0x80, *movl_mlx(3, (1 << 16) | (1 << 6) | 2)),
        (0x90, 0x00, mov_m_gr_ar(3, 24), nop_i(), nop_i()),
        (0xa0, *movl_mlx(
            2, IA64_PSR_IC | IA64_PSR_IS | IA64_PSR_DB | (1 << 37))),
        (0xb0, *movl_mlx(3, 0x100)),
        *rfi_to_gr(0xc0, 2, 3),
        ia32_bundle(0x100, bytes.fromhex(
            "75 02 "       # JNZ is not taken because ZF=1
            "90 90 "       # breakpoint at the first NOP
            "0f 0b")),     # fallback instruction intercept
        (IA64_IA32_EXCEPTION_VECTOR, 0x00,
         mov_m_ar_gr(8, 24), nop_i(), nop_i()),
        (IA64_IA32_EXCEPTION_VECTOR + 0x10, 0x00,
         mov_m_cr_gr(9, 16), nop_i(), nop_i()),
        (IA64_IA32_EXCEPTION_VECTOR + 0x20, 0x02,
         nop_m(), extr_u(9, 9, 37, 1), nop_i()),
        (IA64_IA32_EXCEPTION_VECTOR + 0x30, 0x00,
         mov_m_cr_gr(10, 19), nop_i(), nop_i()),
        (IA64_IA32_EXCEPTION_VECTOR + 0x40, 0x00,
         mov_m_cr_gr(11, 22), nop_i(), nop_i()),
        (IA64_IA32_EXCEPTION_VECTOR + 0x50, 0x00,
         mov_m_cr_gr(12, 17), nop_i(), nop_i()),
        (IA64_IA32_EXCEPTION_VECTOR + 0x60, 0x10,
         nop_m(), nop_i(),
         br_cond(IA64_IA32_EXCEPTION_VECTOR + 0x60,
                 IA64_IA32_EXCEPTION_VECTOR + 0x60)),
    ], {
        "ip": IA64_IA32_EXCEPTION_VECTOR + 0x60,
        "r8": (1 << 6) | 2,
        "r9": 0,
        "r10": 0x102,
        "r11": 0x102,
        "r12": (1 << 32) | (1 << 16),
        "exception": IA64_EXCP_NONE,
    }, entry=0x700, cpu="madison")

test_ia32_dfh_faults_first_target_instruction = require_registers(
    "ia32_dfh_faults_first_target_instruction", [
        *ia32_environment_bundles(0x700, 0x10),
        (0x10, *movl_mlx(2, IA64_PSR_IC | IA64_PSR_DFH)),
        (0x20, 0x00, mov_gr_psr_full(2), nop_i(), nop_i()),
        (0x30, 0x00, srlz_d(), nop_i(), nop_i()),
        (0x40, *movl_mlx(8, 0x100)),
        (0x50, 0x00, nop_m(), mov_br_gr(7, 8), nop_i()),
        (0x60, 0x10, nop_m(), nop_i(), br_indirect(7, btype=1)),
        ia32_bundle(0x100, b"\x90"),
        (IA64_DISABLED_FP_VECTOR, 0x00,
         mov_m_cr_gr(8, 19), nop_i(), nop_i()),
        (IA64_DISABLED_FP_VECTOR + 0x10, 0x00,
         mov_m_cr_gr(9, 17), nop_i(), nop_i()),
        (IA64_DISABLED_FP_VECTOR + 0x20, 0x00,
         mov_m_cr_gr(10, 24), nop_i(), nop_i()),
        (IA64_DISABLED_FP_VECTOR + 0x30, 0x00,
         mov_m_cr_gr(11, 22), nop_i(), nop_i()),
        (IA64_DISABLED_FP_VECTOR + 0x40, 0x10,
         nop_m(), nop_i(),
         br_cond(IA64_DISABLED_FP_VECTOR + 0x40,
                 IA64_DISABLED_FP_VECTOR + 0x40)),
    ], {
        "ip": IA64_DISABLED_FP_VECTOR + 0x40,
        "r8": 0x100,
        "r9": 2,
        "r11": 0x100,
        "exception": IA64_EXCP_NONE,
    }, entry=0x700, cpu="madison")

test_ia32_dfl_faults_first_x87_instruction = require_registers(
    "ia32_dfl_faults_first_x87_instruction", [
        *ia32_environment_bundles(0x700, 0x10),
        (0x10, *movl_mlx(2, IA64_PSR_IC | IA64_PSR_DFL)),
        (0x20, 0x00, mov_gr_psr_full(2), nop_i(), nop_i()),
        (0x30, 0x00, srlz_d(), nop_i(), nop_i()),
        (0x40, *movl_mlx(8, 0x100)),
        (0x50, 0x00, nop_m(), mov_br_gr(7, 8), nop_i()),
        (0x60, 0x10, nop_m(), nop_i(), br_indirect(7, btype=1)),
        ia32_bundle(0x100, bytes.fromhex(
            "b8 34 12 "  # mov ax,0x1234
            "d9 e8")),   # fld1
        (IA64_DISABLED_FP_VECTOR, 0x00,
         mov_m_cr_gr(8, 19), nop_i(), nop_i()),
        (IA64_DISABLED_FP_VECTOR + 0x10, 0x00,
         mov_m_cr_gr(9, 17), nop_i(), nop_i()),
        (IA64_DISABLED_FP_VECTOR + 0x20, 0x00,
         mov_m_cr_gr(10, 24), nop_i(), nop_i()),
        (IA64_DISABLED_FP_VECTOR + 0x30, 0x00,
         mov_m_cr_gr(11, 22), nop_i(), nop_i()),
        (IA64_DISABLED_FP_VECTOR + 0x40, 0x10,
         nop_m(), nop_i(),
         br_cond(IA64_DISABLED_FP_VECTOR + 0x40,
                 IA64_DISABLED_FP_VECTOR + 0x40)),
    ], {
        "ip": IA64_DISABLED_FP_VECTOR + 0x40,
        "r8": 0x103,
        "r9": 1,
        "r11": 0x103,
        "exception": IA64_EXCP_NONE,
    }, entry=0x700, cpu="madison")

test_ia32_psr_ac_unaligned_dword_sets_ifa = require_registers(
    "ia32_psr_ac_unaligned_dword_sets_ifa", [
        *ia32_environment_bundles(0x700, 0x10),
        (0x10, *movl_mlx(2, IA64_PSR_IC | IA64_PSR_AC)),
        (0x20, 0x00, mov_gr_psr_full(2), nop_i(), nop_i()),
        (0x30, 0x00, srlz_d(), nop_i(), nop_i()),
        (0x40, *movl_mlx(8, 0x100)),
        (0x50, 0x00, nop_m(), mov_br_gr(7, 8), nop_i()),
        (0x60, 0x10, nop_m(), nop_i(), br_indirect(7, btype=1)),
        ia32_bundle(0x100, bytes.fromhex("66 a1 01 02 00 00")),
        (IA64_IA32_EXCEPTION_VECTOR, 0x00,
         mov_m_cr_gr(8, 19), nop_i(), nop_i()),
        (IA64_IA32_EXCEPTION_VECTOR + 0x10, 0x00,
         mov_m_cr_gr(9, 17), nop_i(), nop_i()),
        (IA64_IA32_EXCEPTION_VECTOR + 0x20, 0x00,
         mov_m_cr_gr(10, 20), nop_i(), nop_i()),
        (IA64_IA32_EXCEPTION_VECTOR + 0x30, 0x00,
         mov_m_cr_gr(11, 22), nop_i(), nop_i()),
        (IA64_IA32_EXCEPTION_VECTOR + 0x40, 0x10,
         nop_m(), nop_i(),
         br_cond(IA64_IA32_EXCEPTION_VECTOR + 0x40,
                 IA64_IA32_EXCEPTION_VECTOR + 0x40)),
    ], {
        "ip": IA64_IA32_EXCEPTION_VECTOR + 0x40,
        "r8": 0x100,
        "r9": 17 << 16,
        "r10": 0x201,
        "r11": 0x100,
        "exception": IA64_EXCP_NONE,
    }, entry=0x700, cpu="madison")

test_ia32_psr_ac_unaligned_xchg_sets_ifa = require_registers(
    "ia32_psr_ac_unaligned_xchg_sets_ifa", [
        *ia32_environment_bundles(0x700, 0x10),
        (0x10, *movl_mlx(2, IA64_PSR_IC | IA64_PSR_AC)),
        (0x20, 0x00, mov_gr_psr_full(2), nop_i(), nop_i()),
        (0x30, 0x00, srlz_d(), nop_i(), nop_i()),
        (0x40, *movl_mlx(8, 0x100)),
        (0x50, 0x00, nop_m(), mov_br_gr(7, 8), nop_i()),
        (0x60, 0x10, nop_m(), nop_i(), br_indirect(7, btype=1)),
        ia32_bundle(0x100, bytes.fromhex(
            "66 87 06 01 02")),  # xchg eax,dword ptr [0x201]
        (IA64_IA32_EXCEPTION_VECTOR, 0x00,
         mov_m_cr_gr(8, 19), nop_i(), nop_i()),
        (IA64_IA32_EXCEPTION_VECTOR + 0x10, 0x00,
         mov_m_cr_gr(9, 17), nop_i(), nop_i()),
        (IA64_IA32_EXCEPTION_VECTOR + 0x20, 0x00,
         mov_m_cr_gr(10, 20), nop_i(), nop_i()),
        (IA64_IA32_EXCEPTION_VECTOR + 0x30, 0x00,
         mov_m_cr_gr(11, 22), nop_i(), nop_i()),
        (IA64_IA32_EXCEPTION_VECTOR + 0x40, 0x10,
         nop_m(), nop_i(),
         br_cond(IA64_IA32_EXCEPTION_VECTOR + 0x40,
                 IA64_IA32_EXCEPTION_VECTOR + 0x40)),
    ], {
        "ip": IA64_IA32_EXCEPTION_VECTOR + 0x40,
        "r8": 0x100,
        "r9": 17 << 16,
        "r10": 0x201,
        "r11": 0x100,
        "exception": IA64_EXCP_NONE,
    }, entry=0x700, cpu="madison")

test_ia32_vip_vif_code_fetch_faults_before_instruction = require_registers(
    "ia32_vip_vif_code_fetch_faults_before_instruction", [
        *ia32_environment_bundles(0x700, 0x10),
        (0x10, *movl_mlx(3, (1 << 0) | (1 << 33))),
        (0x20, 0x00, mov_m_gr_ar(3, 27), nop_i(), nop_i()),
        (0x30, *movl_mlx(3, (1 << 19) | (1 << 20) | 2)),
        (0x40, 0x00, mov_m_gr_ar(3, 24), nop_i(), nop_i()),
        (0x50, *movl_mlx(
            2, IA64_PSR_IC | IA64_PSR_IS | IA64_PSR_CPL3)),
        (0x60, *movl_mlx(3, 0x100)),
        *rfi_to_gr(0x70, 2, 3),
        ia32_bundle(0x100, b"\x90"),
        (IA64_IA32_EXCEPTION_VECTOR, 0x00,
         mov_m_cr_gr(8, 17), nop_i(), nop_i()),
        (IA64_IA32_EXCEPTION_VECTOR + 0x10, 0x00,
         mov_m_cr_gr(9, 19), nop_i(), nop_i()),
        (IA64_IA32_EXCEPTION_VECTOR + 0x20, 0x00,
         mov_m_cr_gr(10, 22), nop_i(), nop_i()),
        (IA64_IA32_EXCEPTION_VECTOR + 0x30, 0x10,
         nop_m(), nop_i(),
         br_cond(IA64_IA32_EXCEPTION_VECTOR + 0x30,
                 IA64_IA32_EXCEPTION_VECTOR + 0x30)),
    ], {
        "ip": IA64_IA32_EXCEPTION_VECTOR + 0x30,
        "r8": 13 << 16,
        "r9": 0x100,
        "r10": 0x100,
        "exception": IA64_EXCP_NONE,
    }, entry=0x700, cpu="madison")

test_ia32_vip_vif_precedes_start_page_instruction_tlb_fault = \
    require_registers(
        "ia32_vip_vif_precedes_start_page_instruction_tlb_fault", [
            *ia32_environment_bundles(
                0x900, 0x10,
                csd=IA32_TEST_CSD | (0xf << 48) |
                    (1 << 62) | (1 << 63)),
            # Enable protected-mode virtual interrupts with VIP and VIF set.
            (0x10, *movl_mlx(3, (1 << 0) | (1 << 33))),
            (0x20, 0x00, mov_m_gr_ar(3, 27), nop_i(), nop_i()),
            (0x30, *movl_mlx(3, (1 << 19) | (1 << 20) | 2)),
            (0x40, 0x00, mov_m_gr_ar(3, 24), nop_i(), nop_i()),
            # Map a 64 KiB IVT window, leaving the IA-32 target unmapped.
            (0x50, *movl_mlx(18, 0x20000 | DTR_PTE_WB)),
            (0x60, *movl_mlx(19, 0x20000)),
            (0x70, 0x00, mov_m_gr_cr(19, 20),
             adds(21, 16 << 2, 0), nop_i()),
            (0x80, 0x00, mov_m_gr_cr(21, 21),
             adds(10, 5, 0), nop_i()),
            (0x90, 0x00, itr_i(10, 18), nop_i(), nop_i()),
            (0xa0, 0x00, srlz_i(), nop_i(), nop_i()),
            (0xb0, *movl_mlx(4, 0x20000)),
            (0xc0, 0x00, mov_m_gr_cr(4, 2), nop_i(), nop_i()),
            (0xd0, *movl_mlx(
                2, IA64_PSR_IC | IA64_PSR_IT | IA64_PSR_IS |
                    IA64_PSR_CPL3)),
            (0xe0, *movl_mlx(3, 0x300123)),
            *rfi_to_gr(0xf0, 2, 3),
            (0x20000 + IA64_IA32_EXCEPTION_VECTOR, 0x00,
             mov_m_cr_gr(8, 19), nop_i(), nop_i()),
            (0x20000 + IA64_IA32_EXCEPTION_VECTOR + 0x10, 0x00,
             mov_m_cr_gr(9, 17), nop_i(), nop_i()),
            (0x20000 + IA64_IA32_EXCEPTION_VECTOR + 0x20, 0x00,
             mov_m_cr_gr(10, 22), nop_i(), nop_i()),
            (0x20000 + IA64_IA32_EXCEPTION_VECTOR + 0x30, 0x10,
             nop_m(), nop_i(),
             br_cond(0x20000 + IA64_IA32_EXCEPTION_VECTOR + 0x30,
                     0x20000 + IA64_IA32_EXCEPTION_VECTOR + 0x30)),
        ], {
            "ip": 0x20000 + IA64_IA32_EXCEPTION_VECTOR + 0x30,
            "r8": 0x300123,
            "r9": 13 << 16,
            "r10": 0x300123,
            "exception": IA64_EXCP_NONE,
        }, entry=0x900, cpu="madison")

test_rfi_to_ia32_taken_branch_trap_records_byte_ips = require_registers(
    "rfi_to_ia32_taken_branch_trap_records_byte_ips",
    [
        *ia32_environment_bundles(0x700, 0x10),
        (0x10, *movl_mlx(
            2, IA64_PSR_IC | IA64_PSR_IS | IA64_PSR_TB)),
        (0x20, *movl_mlx(3, 0x100)),
        *rfi_to_gr(0x30, 2, 3),
        ia32_bundle(0x100, bytes.fromhex("eb 02")),
        (IA64_IA32_EXCEPTION_VECTOR, 0x00,
         mov_m_cr_gr(8, 19), nop_i(), nop_i()),
        (IA64_IA32_EXCEPTION_VECTOR + 0x10, 0x00,
         mov_m_cr_gr(9, 17), nop_i(), nop_i()),
        (IA64_IA32_EXCEPTION_VECTOR + 0x20, 0x00,
         mov_m_cr_gr(10, 24), nop_i(), nop_i()),
        (IA64_IA32_EXCEPTION_VECTOR + 0x30, 0x00,
         mov_m_cr_gr(11, 22), nop_i(), nop_i()),
        (IA64_IA32_EXCEPTION_VECTOR + 0x40, 0x10,
         nop_m(), nop_i(),
         br_cond(IA64_IA32_EXCEPTION_VECTOR + 0x40,
                 IA64_IA32_EXCEPTION_VECTOR + 0x40)),
    ], {
        "ip": IA64_IA32_EXCEPTION_VECTOR + 0x40,
        "r8": 0x104,
        "r9": 0x10004,
        "r11": 0x100,
        "exception": IA64_EXCP_NONE,
    }, entry=0x700, cpu="madison")

test_rfi_montecito_native_ia32_disabled_fault = require_registers(
    "rfi_montecito_native_ia32_disabled_fault", [
        (0x10, *movl_mlx(2, IA64_PSR_IS)),
        (0x20, *movl_mlx(3, 0x1234567800000045)),
        (0x30, 0x00, mov_m_gr_cr(2, 16), nop_i(), nop_i()),
        (0x40, 0x00, mov_m_gr_cr(3, 19), nop_i(), nop_i()),
        (0x50, *movl_mlx(4, IA64_PSR_IC)),
        (0x60, 0x00, mov_gr_psr_full(4), nop_i(), nop_i()),
        (0x70, 0x00, srlz_d(), nop_i(), nop_i()),
        (0x80, 0x11, nop_m(), nop_i(), rfi_b()),
        (IA64_GENERAL_VECTOR, 0x00, mov_m_cr_gr(8, 19), nop_i(), nop_i()),
        (IA64_GENERAL_VECTOR + 0x10, 0x00, mov_m_cr_gr(9, 17), nop_i(),
         nop_i()),
        (IA64_GENERAL_VECTOR + 0x20, 0x10, nop_m(), nop_i(),
         br_cond(IA64_GENERAL_VECTOR + 0x20,
                 IA64_GENERAL_VECTOR + 0x20)),
    ], {
        "ip": IA64_GENERAL_VECTOR + 0x20,
        "r8": 0x80,
        "r9": 0x40 | (2 << IA64_ISR_EI_SHIFT),
        "exception": IA64_EXCP_NONE,
    }, entry=0x10)

test_rfi_montecito_uncollected_transition_preserves_target = \
    require_registers(
        "rfi_montecito_uncollected_transition_preserves_target", [
            (0x10, *movl_mlx(5, 0x10000)),
            (0x20, 0x00, mov_m_gr_cr(5, 2), nop_i(), nop_i()),
            (0x30, *movl_mlx(2, IA64_PSR_IS)),
            (0x40, *movl_mlx(3, 0x1234567800000045)),
            (0x50, 0x00, mov_m_gr_cr(2, 16), nop_i(), nop_i()),
            (0x60, 0x00, mov_m_gr_cr(3, 19), nop_i(), nop_i()),
            (0x70, 0x11, nop_m(), nop_i(), rfi_b()),
            (0x15400, 0x00, mov_m_cr_gr(8, 19), nop_i(), nop_i()),
            (0x15410, 0x00, mov_m_cr_gr(9, 16), nop_i(), nop_i()),
            (0x15420, 0x00, mov_m_cr_gr(10, 17), nop_i(), nop_i()),
            (0x15430, 0x10, nop_m(), nop_i(), br_cond(0x15430, 0x15430)),
        ], {
            "ip": 0x15430,
            "r8": 0x1234567800000045,
            "r9": IA64_PSR_IS,
            "r10": 0x40 | IA64_ISR_NI | (2 << IA64_ISR_EI_SHIFT),
            "fault_code": IA64_EXCP_DISABLED_ISA_TRANSITION,
            "fault_ip": 0x70,
            "exception": IA64_EXCP_NONE,
        }, entry=0x10)

GROUP = 'interrupt'
CASE_NAMES = (

    'ar_itc_advances_in_guest_loop',
    'async_timer_interrupt_enters_ivt',
    'async_timer_interrupt_never_resumes_mlx_slot2',
    'async_timer_interrupt_preserves_bank1_grs',
    'async_timer_interrupt_records_boundary_ri',
    'br_ia_preserves_rf_and_psr_id_until_target_completes',
    'br_ia_single_step_trap',
    'br_ia_taken_branch_trap_precedes_single_step',
    'br_ia_unimplemented_target_preserves_64bit_iip',
    'break_preserves_ifa_and_records_iim_isr',
    'cloop_zero_st1_timer_interrupts_batched_loop',
    'counted_self_loop_fault_has_slot1_ri',
    'cover_saves_interrupted_cfm_to_ifs',
    'exception_break',
    'exception_break_f',
    'exception_break_x',
    'exception_clears_ifs_keeps_cfm',
    'exception_entry_initializes_psr',
    'exception_illegal',
    'exception_illegal_enters_general_vector',
    'exception_records_slot_ri',
    'exception_reserved_template',
    'exception_syscall_break',
    'exception_unaligned',
    'exception_unaligned_ic0_preserves_ifa',
    'exception_unaligned_sets_ifa_isr',
    'exception_unaligned_slot1_uses_psr_ri',
    'unaligned_ac_clear_no_page_cross_loads',
    'unaligned_ac_set_no_page_cross_faults',
    'future_itm_rearm_preserves_pended_timer_irr',
    'future_itm_rearm_uses_latest_deadline',
    'iipa_preserved_for_rfi_to_fault',
    'iipa_reports_current_bundle_after_prior_slot_success',
    'iipa_reports_previous_successful_bundle_for_slot0_fault',
    'ia32_dfh_faults_first_target_instruction',
    'ia32_fisttp_intercepts_before_cr0_em',
    'ia32_dbr_overlap_raises_post_instruction_trap',
    'ia32_eflag_tf_traps_after_one_instruction_and_resumes',
    'ia32_flag_writeback_does_not_enable_single_step',
    'ia32_pop_edx_does_not_intercept_as_pop_ss',
    'ia32_pop_ss_system_flag_intercept_is_post_instruction',
    'ia32_data_access_wraps_at_4g',
    'ia32_data_breakpoint_matches_wrapped_byte_at_4g',
    'ia32_data_tlb_fault_precedes_alignment_check',
    'ia32_cflg_io_clear_denies_failed_iopl',
    'ia32_cross_page_cs_limit_at_boundary_precedes_second_page_tlb',
    'ia32_cross_page_instruction_tlb_fault_records_instruction_start',
    'ia32_cross_page_tlb_precedes_later_cs_limit',
    'ia32_dfl_faults_first_x87_instruction',
    'ia32_ds_operand_crossing_limit_faults',
    'ia32_gate_intercept_reports_concurrent_debug_traps',
    'ia32_gdt_descriptor_read_triggers_data_breakpoint',
    'ia32_gdt_descriptor_read_wraps_at_4g',
    'ia32_gdt_reference_rejects_non_system_descriptor',
    'ia32_high_iobase_read_triggers_data_breakpoint',
    'ia32_ibr_precedes_start_page_instruction_tlb_fault',
    'ia32_int3_clears_rf_and_psr_id_after_completion',
    'ia32_illegal_x87_opcode_intercepts_with_cr0_em',
    'ia32_amd_prefetch_opcode_intercepts',
    'ia32_instruction_intercept_records_prefix_and_opcode',
    'ia32_instruction_fetch_wraps_at_4g',
    'ia32_instruction_tlb_fault_records_unaligned_instruction_start',
    'ia32_instruction_crossing_cs_limit_faults',
    'ia32_ibr_instruction_breakpoint_fault',
    'ia32_invalid_csd_faults_at_target_fetch',
    'ia32_io_access_crosses_port_ffff_without_wrapping',
    'ia32_io_eflag_ac_does_not_check_alignment',
    'ia32_io_psr_ac_unaligned_word_sets_ifa',
    'ia32_jmpe_reports_concurrent_data_breakpoint',
    'ia32_ldt_reference_requires_present_system_descriptor',
    'ia32_lock_check_allows_aligned_writeback_xchg',
    'ia32_lock_intercept_on_8byte_boundary_crossing',
    'ia32_locked_rmw_triggers_read_data_breakpoint',
    'ia32_ldmxcsr_rejects_reserved_madison_bit',
    'ia32_masked_sse_exception_commits_result_and_status',
    'ia32_maskmovq_triggers_data_breakpoint',
    'ia32_psr_ac_unaligned_dword_sets_ifa',
    'ia32_psr_ac_unaligned_xchg_sets_ifa',
    'ia32_psr_ss_traps_after_one_instruction_and_resumes',
    'ia32_read_only_ds_rejects_store',
    'ia32_rep_dbr_traps_after_matching_iteration',
    'ia32_rep_fault_sets_rf_for_restart',
    'ia32_rep_final_iteration_trap_advances_ip_and_clears_rf',
    'ia32_fxrstor_reserved_mxcsr_is_precise',
    'ia32_ss_dpl_mismatch_faults_push',
    'ia32_stack_access_wraps_at_4g',
    'ia32_stack_push_triggers_data_breakpoint',
    'ia32_sti_system_flag_intercept_is_post_instruction',
    'ia32_not_taken_branch_clears_rf_and_psr_id',
    'ia32_taken_branch_clears_rf_and_psr_id',
    'ia32_tss_io_bitmap_allows_and_denies_ports',
    'ia32_tss_reference_rejects_non_system_descriptor',
    'ia32_unmasked_sse_exception_is_masked_without_cflg_mmxex',
    'ia32_unmasked_sse_exception_is_precise_vector19',
    'ia32_unaligned_gdt_obeys_psr_ac',
    'ia32_unaligned_movaps_raises_gpf',
    'ia32_vip_vif_code_fetch_faults_before_instruction',
    'ia32_vip_vif_precedes_start_page_instruction_tlb_fault',
    'ia32_x87_data_access_wraps_at_4g',
    'ia32_x87_operand_crossing_ds_limit_faults',
    'ia32_fxsave_checks_entire_512_byte_segment_operand',
    'ia32_bound_checks_second_element_against_segment_limit',
    'invalid_itv_vector_is_ignored',
    'masked_itv_discards_due_timer',
    'masking_itv_preserves_pended_timer_irr',
    'mov_from_psr_does_not_copy_execution_slot_to_rfi',
    'mov_pkr_does_not_alias_interruption_crs',
    'mov_to_iipa_establishes_last_ip_for_rfi_to_fault',
    'mov_to_irr_illegal',
    'mov_to_ivr_illegal',
    'mov_to_read_only_cr_predicate_false',
    'nested_exception_keeps_handler_interruption_state',
    'nested_exception_keeps_handler_return_state',
    'past_itm_does_not_fire',
    'past_itm_reprogram_cancels_future_deadline',
    'past_rearmed_itm_does_not_interrupt',
    'rfi_ignores_iip_low_bits',
    'rfi_restores_interrupted_bsp_after_cover',
    'rfi_resumes_at_ipsr_ri_slot',
    'rfi_retries_interrupted_current_frame_fill',
    'rfi_target_rse_fill_fault_uses_restored_psr',
    'rfi_montecito_native_ia32_disabled_fault',
    'rfi_montecito_uncollected_transition_preserves_target',
    'rfi_to_ia32_clears_fault_suppression_but_preserves_psr_id',
    'rfi_to_ia32_taken_branch_trap_records_byte_ips',
    'rfi_to_ia32_unimplemented_target_preserves_64bit_iip',
    'rse_large_frame_timer_rfi_preserves_high_caller_local',
    'repeated_timer_rfi_preserves_word_rmw',
    'timer_cover_rfi_preserves_large_frame_halfword_rmw',
    'timer_interrupt_preserves_banked_word_rmw',
    'sapic_extint_masks_external_until_eoi',
    'sapic_same_class_higher_vector_preempts',
    'timer_interrupt_exits_chained_loop_after_virtual_deadline',
    'tpr_mmi_masks_timer_until_cleared',
    'tpr_preserves_mmi_and_mic',
    'unimplemented_physical_instruction_traps',
)

CASE_METADATA = {
    'async_timer_interrupt_enters_ivt': CaseMetadata(nonterminal_effect_loop=True),
    'async_timer_interrupt_preserves_bank1_grs': CaseMetadata(nonterminal_effect_loop=True),
    'async_timer_interrupt_records_boundary_ri': CaseMetadata(nonterminal_effect_loop=True),
    'future_itm_rearm_preserves_pended_timer_irr': CaseMetadata(nonterminal_effect_loop=True),
    'invalid_itv_vector_is_ignored': CaseMetadata(nonterminal_effect_loop=True),
    'masked_itv_discards_due_timer': CaseMetadata(nonterminal_effect_loop=True),
    'masking_itv_preserves_pended_timer_irr': CaseMetadata(nonterminal_effect_loop=True),
    'past_itm_does_not_fire': CaseMetadata(nonterminal_effect_loop=True),
    'rfi_target_rse_fill_fault_uses_restored_psr': CaseMetadata(nonterminal_effect_loop=True),
}

CASE_ALIASES = {
}

CASES = bind_cases(GROUP, CASE_NAMES, globals(),
                   aliases=CASE_ALIASES,
                   metadata=CASE_METADATA)
