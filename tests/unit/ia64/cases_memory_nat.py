"""Memory, ALAT, atomic, and NaT microprograms."""

from __future__ import annotations

from .case import (CaseMetadata, CaseObservation, bind_cases)
from .encoding import (
    ADV_UC_LOAD_BUNDLE,
    ADV_UC_LOAD_DATA,
    ADV_UC_LOAD_VA,
    CHECK_LOAD_DATA,
    DTR_PTE_NATPAGE,
    DTR_PTE_UC,
    HIGH_TR_BASE,
    IA64_ALT_DTLB_VECTOR,
    IA64_BREAK_VECTOR,
    IA64_EXCP_ILLEGAL,
    IA64_EXCP_NAT_CONSUMPTION,
    IA64_DCR_DM,
    IA64_EXCP_NONE,
    IA64_EXCP_UNALIGNED,
    IA64_EXCP_UNSUPPORTED_DATA_REFERENCE,
    IA64_GENERAL_VECTOR,
    IA64_GENEX_UNIMPL_DATA_ADDR,
    IA64_IMPL_PA_BITS,
    IA64_ISR_CODE_REG_NAT,
    IA64_ISR_ED,
    IA64_ISR_EI_SHIFT,
    IA64_ISR_NA,
    IA64_ISR_NI,
    IA64_ISR_R,
    IA64_ISR_SP,
    IA64_ISR_W,
    IA64_NAT_CONSUMPTION_VECTOR,
    IA64_PHYS_UC_BIT,
    IA64_PSR_AC,
    IA64_PSR_BE,
    IA64_PSR_DT,
    IA64_PSR_ED,
    IA64_PSR_IC,
    IA64_PSR_IT,
    IA64_UNALIGNED_VECTOR,
    IA64_UNSUPPORTED_DATA_REFERENCE_VECTOR,
    LOW_VECTOR_ITIR,
    LOW_VECTOR_TR_PTE,
    PTE_ED,
    addl,
    adds,
    alloc,
    alloc_m,
    bitfield,
    br_call,
    br_cloop,
    br_cond,
    br_ctop_many,
    br_ret,
    break_m,
    bsw0,
    bsw1,
    bundle_words,
    chk_a_clr_m,
    chk_a_nc_m,
    chk_s_i,
    chk_s_m,
    cmp4_eq_imm,
    cover_b,
    cmp4_eq_unc_imm,
    cmp8xchg16_acq,
    cmp8xchg16_rel,
    cmp_eq_and,
    cmp_ge_or,
    cmpxchg4,
    cmpxchg4_acq,
    cmpxchg_rel,
    czx1_r,
    dtr_setup_bundles,
    fc_i,
    flushrs_enc,
    fetchadd4_acq,
    fetchadd4_rel,
    invala,
    invala_e_gr,
    itr_i,
    ld1,
    ld16,
    ld16_acq,
    ld1_acq,
    ld1_postinc,
    ld1_reg_postinc,
    ld1_sa_postinc,
    ld2,
    ld2_c_clr,
    ld2_c_clr_reg_update,
    ld2_sa,
    ld4,
    ld4_a,
    ld4_bias,
    ld4_c_clr,
    ld4_c_clr_acq,
    ld4_c_nc,
    ld8,
    ld8_a,
    ld8_c_clr,
    ld8_c_clr_acq,
    ld8_c_nc,
    ld8_fill_postinc,
    ld8_postinc,
    ld8_s,
    ld8_s_hint,
    ld8_s_postinc,
    ld8_sa,
    lfetch,
    loadrs_enc,
    lfetch_postinc,
    lfetch_reg_postinc,
    load_mem,
    load_mem_postinc,
    load_mem_reg_postinc,
    mov_ar_lc,
    mov_b_gr,
    mov_cpuid,
    mov_gr_psr_full,
    mov_grpmc_indexed,
    mov_i_imm_ar,
    mov_lc_gr,
    mov_m_ar_gr,
    mov_m_cr_gr,
    mov_m_gr_ar,
    mov_m_gr_cr,
    mov_m_gr_psr_um,
    mov_m_gr_psrl,
    mov_m_imm_ar,
    mov_m_psr_gr,
    mov_pkr_indexed,
    mov_pr_rot_imm,
    mov_rr_read,
    movl_mlx,
    mux1_rev,
    nop_b,
    nop_i,
    nop_m,
    nop_x,
    or_reg,
    pack2_sss,
    pmpy2,
    pshl2,
    pshl4,
    pshr2,
    pshr4,
    raw_bundle,
    register_nat_consumption_test,
    require_exception,
    require_registers,
    rfi_b,
    rfi_to_gr,
    rum,
    run_program,
    srlz_d,
    srlz_i,
    ssm,
    st16,
    st1_postinc,
    st2,
    st4,
    st4_postinc,
    st4_rel,
    st8,
    st8_postinc,
    st8_rel,
    st8_spill_postinc,
    store_mem,
    store_mem_postinc,
    sum_um,
    tbit_z,
    tbit_z_and,
    tbit_z_or,
    tnat_nz_and,
    tnat_nz_or,
    tnat_z_unc,
    xchg,
    xchg4,
)


test_alloc_clears_destination_nat = require_registers(
    "alloc_clears_destination_nat", [
        (0x10, 0x00, mov_m_imm_ar(36, 1), addl(6, 0x200, 0),
         nop_i()),
        (0x20, 0x00, nop_m(), alloc(1, 6, 4, 0, 0),
         nop_i()),
        (0x30, 0x08, ld8_fill_postinc(34, 6, 0), nop_i(),
         nop_i()),
        (0x40, 0x00, nop_m(), adds(35, 0, 34),
         nop_i()),
        (0x50, 0x00, nop_m(), nop_i(), nop_i()),
        (0x60, 0x00, nop_m(), alloc(34, 6, 4, 0, 0),
         nop_i()),
        (0x70, 0x00, nop_m(), nop_i(), nop_i()),
        (0x80, 0x00, nop_m(), nop_i(), nop_i()),
        (0x90, 0x00, mov_m_gr_ar(34, 64), nop_i(),
         nop_i()),
        (0xa0, 0x10, nop_m(), nop_i(),
         br_cond(0xa0, 0xa0)),
        (0x200, 0x00, 0, 0,
         0),
    ], {
        "ip": 0xa0,
        "exception": IA64_EXCP_NONE,
        "r34_nat": 0,
        "r35_nat": 1,
    }, entry=0x10)

test_ld1_acq_decode = require_registers("ld1_acq_decode", [
    (0x10, 0x00, addl(3, 0x100, 0), nop_i(),
     nop_i()),
    (0x20, 0x08, nop_m(), ld1_acq(4, 3),
     nop_i()),
    (0x30, 0x10, nop_m(), nop_i(),
     br_cond(0x30, 0x30)),
    (0x100, 0x00, 0x5a, 0,
     0),
], {"ip": 0x30, "r4": 0x40}, entry=0x10)

LD4_BIAS_DATA = bundle_words(0x00, 0x091a2b3c, 0, 0)[0] & 0xffffffff

test_ld4_bias_decode = require_registers("ld4_bias_decode", [
    (0x10, 0x00, addl(3, 0x100, 0), nop_i(),
     nop_i()),
    (0x20, 0x08, nop_m(), ld4_bias(4, 3),
     nop_i()),
    (0x30, 0x10, nop_m(), nop_i(),
     br_cond(0x30, 0x30)),
    (0x100, 0x00, 0x091a2b3c, 0,
     0),
], {"ip": 0x30, "r4": LD4_BIAS_DATA}, entry=0x10)
CHECK_LOAD_MISMATCH_DATA = bundle_words(0x00, 0x0fedcba987654321, 0, 0)[0]

test_ld8_c_nc_hit_preserves_target = require_registers(
    "ld8_c_nc_hit_preserves_target", [
        (0x10, 0x00, addl(3, 0x100, 0), nop_i(),
         nop_i()),
        (0x20, 0x00, ld8_a(4, 3), nop_i(),
         nop_i()),
        (0x30, *movl_mlx(4, 0x55)),
        (0x40, 0x00, ld8_c_nc(4, 3), nop_i(),
         nop_i()),
        (0x50, 0x10, nop_m(), nop_i(),
         br_cond(0x50, 0x50)),
        (0x100, 0x00, 0x123456789abcdef0, 0,
         0),
    ], {"ip": 0x50, "r4": 0x55}, entry=0x10)

test_ld8_c_nc_hit_consumes_nat_base = require_exception(
    "ld8_c_nc_hit_consumes_nat_base", [
        (0x10, 0x00, addl(3, 0x100, 0), nop_i(),
         nop_i()),
        (0x20, 0x00, ld8_a(4, 3), nop_i(),
         nop_i()),
        (0x30, 0x00, mov_m_imm_ar(36, 1), addl(6, 0x200, 0),
         nop_i()),
        (0x40, 0x08, ld8_fill_postinc(5, 6, 0), nop_i(),
         nop_i()),
        (0x50, 0x00, ld8_c_nc(4, 5), nop_i(),
         nop_i()),
        (0x100, 0x00, 0x123456789abcdef0, 0,
         0),
        (0x200, 0x00, 0x100, 0,
         0),
    ], IA64_EXCP_NAT_CONSUMPTION, fault_ip=0x50, entry=0x10)

test_ld8_c_clr_hit_clears_entry = require_registers(
    "ld8_c_clr_hit_clears_entry", [
        (0x10, 0x00, addl(3, 0x100, 0), nop_i(),
         nop_i()),
        (0x20, 0x00, ld8_a(4, 3), nop_i(),
         nop_i()),
        (0x30, *movl_mlx(4, 0x55)),
        (0x40, 0x00, ld8_c_clr(4, 3), nop_i(),
         nop_i()),
        (0x50, *movl_mlx(4, 0xaa)),
        (0x60, 0x00, ld8_c_nc(4, 3), nop_i(),
         nop_i()),
        (0x70, 0x10, nop_m(), nop_i(),
         br_cond(0x70, 0x70)),
        (0x100, 0x00, 0x123456789abcdef0, 0,
         0),
    ], {"ip": 0x70, "r4": CHECK_LOAD_DATA}, entry=0x10)

test_ld4_c_clr_hit_clears_entry = require_registers(
    "ld4_c_clr_hit_clears_entry", [
        (0x10, *movl_mlx(2, 0xfeedface89abcdef)),
        (0x20, 0x00, addl(3, 0x200, 0), nop_i(),
         nop_i()),
        (0x30, 0x00, st8(3, 2), nop_i(),
         nop_i()),
        (0x40, 0x00, ld4_a(4, 3), nop_i(),
         nop_i()),
        (0x50, 0x00, nop_m(), adds(4, 0x55, 0),
         nop_i()),
        # On an ALAT hit, this implementation takes the architecturally
        # permitted option of leaving the target unchanged.  The .c.clr
        # operation must still remove the four-byte entry (SDM Vol. 1,
        # 4.4.5.3).
        (0x60, 0x00, ld4_c_clr(4, 3), nop_i(),
         nop_i()),
        (0x70, 0x00, nop_m(), adds(5, 0, 4),
         nop_i()),
        (0x80, 0x00, nop_m(), adds(4, 0x66, 0),
         nop_i()),
        # The cleared entry makes this check miss and reload exactly four
        # bytes, zero-extending the result into the general register.
        (0x90, 0x00, ld4_c_nc(4, 3), nop_i(),
         nop_i()),
        (0xa0, 0x10, nop_m(), nop_i(),
         br_cond(0xa0, 0xa0)),
    ], {
        "ip": 0xa0,
        "exception": IA64_EXCP_NONE,
        "r4": 0x89abcdef,
        "r5": 0x55,
    }, entry=0x10)

test_zero_alat_check_load_always_reloads = require_registers(
    "zero_alat_check_load_always_reloads", [
        (0x10, 0x00, addl(3, 0x100, 0), nop_i(),
         nop_i()),
        (0x20, 0x00, ld8_a(4, 3), nop_i(),
         nop_i()),
        (0x30, *movl_mlx(4, 0x55)),
        (0x40, 0x00, ld8_c_nc(4, 3), nop_i(),
         nop_i()),
        (0x50, 0x10, nop_m(), nop_i(),
         br_cond(0x50, 0x50)),
        (0x100, 0x00, 0x123456789abcdef0, 0,
         0),
    ], {"ip": 0x50, "r4": CHECK_LOAD_DATA}, entry=0x10, alat=None)

test_zero_alat_chk_a_always_branches = require_registers(
    "zero_alat_chk_a_always_branches", [
        (0x10, 0x00, addl(3, 0x100, 0), nop_i(),
         nop_i()),
        (0x20, 0x00, ld8_a(22, 3), nop_i(),
         nop_i()),
        (0x30, 0x00, chk_a_nc_m(22, 0x30, 0x50), adds(4, 1, 0),
         nop_i()),
        (0x40, 0x10, nop_m(), nop_i(),
         br_cond(0x40, 0x40)),
        (0x50, 0x10, nop_m(), nop_i(),
         br_cond(0x50, 0x50)),
        (0x100, 0x00, 0x123456789abcdef0, 0,
         0),
    ], {"ip": 0x50, "r4": 0}, entry=0x10, alat=None)

test_ld8_sa_failure_invalidates_old_entry = require_registers(
    "ld8_sa_failure_invalidates_old_entry", [
        (0x10, 0x00, addl(3, 0x100, 0), addl(5, 0x105, 0),
         nop_i()),
        (0x20, 0x00, ld8_a(4, 3), nop_i(),
         nop_i()),
        (0x30, 0x00, sum_um(0x8), nop_i(),
         nop_i()),
        (0x40, 0x00, ld8_sa(4, 5), nop_i(),
         nop_i()),
        (0x50, *movl_mlx(4, 0x55)),
        (0x60, 0x00, ld8_c_nc(4, 3), nop_i(),
         nop_i()),
        (0x70, 0x10, nop_m(), nop_i(),
         br_cond(0x70, 0x70)),
        (0x100, 0x00, 0x123456789abcdef0, 0,
         0),
    ], {"ip": 0x70, "r4": CHECK_LOAD_DATA}, entry=0x10)

test_ld8_a_uc_zeroes_target_and_skips_alat = require_registers(
    "ld8_a_uc_zeroes_target_and_skips_alat", [
        *dtr_setup_bundles(0x10, HIGH_TR_BASE, 0x400000,
                           pte_flags=DTR_PTE_UC),
        (0x70, *movl_mlx(2, ADV_UC_LOAD_VA)),
        (0x80, *movl_mlx(19, (1 << 13) | (1 << 17))),
        (0x90, 0x08, mov_gr_psr_full(19), srlz_d(),
         nop_i()),
        (0xa0, 0x00, ld8_a(4, 2), nop_i(),
         nop_i()),
        (0xb0, 0x00, nop_m(), adds(5, 0, 4),
         nop_i()),
        (0xc0, 0x00, ld8_c_nc(4, 2), nop_i(),
         nop_i()),
        (0xd0, 0x10, nop_m(), nop_i(),
         br_cond(0xd0, 0xd0)),
        ADV_UC_LOAD_BUNDLE,
    ], {"ip": 0xd0, "r4": ADV_UC_LOAD_DATA, "r5": 0,
        "exception": IA64_EXCP_NONE}, entry=0x10)

test_ld8_s_uc_defers = require_registers(
    "ld8_s_uc_defers", [
        *dtr_setup_bundles(0x10, HIGH_TR_BASE, 0x400000,
                           pte_flags=DTR_PTE_UC),
        (0x70, *movl_mlx(2, ADV_UC_LOAD_VA)),
        (0x80, *movl_mlx(19, (1 << 13) | (1 << 17))),
        (0x90, 0x08, mov_gr_psr_full(19), srlz_d(),
         nop_i()),
        (0xa0, 0x00, ld8_s(4, 2), nop_i(),
         nop_i()),
        (0xb0, 0x00, nop_m(), nop_i(), nop_i()),
        (0xc0, 0x00, nop_m(), nop_i(), nop_i()),
        (0xd0, 0x10, nop_m(), nop_i(),
         br_cond(0xd0, 0xd0)),
        ADV_UC_LOAD_BUNDLE,
    ], {"ip": 0xd0, "r4_nat": 1,
        "exception": IA64_EXCP_NONE}, entry=0x10)

test_ld8_c_nc_address_mismatch_reloads = require_registers(
    "ld8_c_nc_address_mismatch_reloads", [
        (0x10, 0x00, addl(3, 0x100, 0), addl(5, 0x110, 0),
         nop_i()),
        (0x20, 0x00, ld8_a(4, 3), nop_i(),
         nop_i()),
        (0x30, *movl_mlx(4, 0x55)),
        (0x40, 0x00, ld8_c_nc(4, 5), nop_i(),
         nop_i()),
        (0x50, 0x10, nop_m(), nop_i(),
         br_cond(0x50, 0x50)),
        (0x100, 0x00, 0x123456789abcdef0, 0,
         0),
        (0x110, 0x00, 0x0fedcba987654321, 0,
         0),
    ], {"ip": 0x50, "r4": CHECK_LOAD_MISMATCH_DATA}, entry=0x10)

test_ld8_c_clr_address_mismatch_reloads = require_registers(
    "ld8_c_clr_address_mismatch_reloads", [
        (0x10, 0x00, addl(3, 0x100, 0), addl(5, 0x110, 0),
         nop_i()),
        (0x20, 0x00, ld8_a(4, 3), nop_i(),
         nop_i()),
        (0x30, *movl_mlx(4, 0x55)),
        (0x40, 0x00, ld8_c_clr(4, 5), nop_i(),
         nop_i()),
        (0x50, 0x10, nop_m(), nop_i(),
         br_cond(0x50, 0x50)),
        (0x100, 0x00, 0x123456789abcdef0, 0,
         0),
        (0x110, 0x00, 0x0fedcba987654321, 0,
         0),
    ], {"ip": 0x50, "r4": CHECK_LOAD_MISMATCH_DATA}, entry=0x10)

test_ld16_loads_gr_and_csd = require_registers("ld16_loads_gr_and_csd", [
    (0x10, 0x00, addl(3, 0x104, 0), addl(4, 0x10c, 0),
     nop_i()),
    (0x20, *movl_mlx(16, 0x0123456789abcdef)),
    (0x30, *movl_mlx(17, 0xfedcba9876543210)),
    (0x40, 0x00, st8(3, 16), nop_i(),
     nop_i()),
    (0x50, 0x00, st8(4, 17), nop_i(),
     nop_i()),
    (0x60, 0x00, ld16(8, 3), nop_i(),
     nop_i()),
    (0x70, 0x00, mov_m_ar_gr(9, 25), nop_i(),
     nop_i()),
    (0x80, 0x10, nop_m(), nop_i(),
     br_cond(0x80, 0x80)),
], {
    "ip": 0x80,
    "r8": 0x0123456789abcdef,
    "r9": 0xfedcba9876543210,
    "exception": IA64_EXCP_NONE,
}, entry=0x10)

test_ld16_acq_hint_decode = require_registers("ld16_acq_hint_decode", [
    (0x10, 0x00, addl(3, 0x100, 0), addl(4, 0x108, 0),
     nop_i()),
    (0x20, *movl_mlx(16, 0x1111222233334444)),
    (0x30, *movl_mlx(17, 0x5555666677778888)),
    (0x40, 0x00, st8(3, 16), nop_i(),
     nop_i()),
    (0x50, 0x00, st8(4, 17), nop_i(),
     nop_i()),
    (0x60, 0x00, ld16_acq(8, 3, hint=2), nop_i(),
     nop_i()),
    (0x70, 0x00, mov_m_ar_gr(9, 25), nop_i(),
     nop_i()),
    (0x80, 0x10, nop_m(), nop_i(),
     br_cond(0x80, 0x80)),
], {
    "ip": 0x80,
    "r8": 0x1111222233334444,
    "r9": 0x5555666677778888,
    "exception": IA64_EXCP_NONE,
}, entry=0x10)

test_st16_stores_gr_and_csd = require_registers("st16_stores_gr_and_csd", [
    (0x10, 0x00, addl(3, 0x204, 0), addl(4, 0x20c, 0),
     nop_i()),
    (0x20, *movl_mlx(15, 0x0123456789abcdef)),
    (0x30, *movl_mlx(5, 0xfedcba9876543210)),
    (0x40, 0x00, mov_m_gr_ar(5, 25), nop_i(),
     nop_i()),
    (0x50, 0x00, st16(3, 15), nop_i(),
     nop_i()),
    (0x60, 0x00, ld8(29, 3), nop_i(),
     nop_i()),
    (0x70, 0x00, ld8(30, 4), nop_i(),
     nop_i()),
    (0x80, 0x10, nop_m(), nop_i(),
     br_cond(0x80, 0x80)),
], {
    "ip": 0x80,
    "r29": 0x0123456789abcdef,
    "r30": 0xfedcba9876543210,
    "exception": IA64_EXCP_NONE,
}, entry=0x10)

test_st16_rel_stores_gr_and_csd = require_registers(
    "st16_rel_stores_gr_and_csd", [
        (0x10, 0x00, addl(3, 0x200, 0), addl(4, 0x208, 0),
         nop_i()),
        (0x20, *movl_mlx(15, 0x1020304050607080)),
        (0x30, *movl_mlx(5, 0x8877665544332211)),
        (0x40, 0x00, mov_m_gr_ar(5, 25), nop_i(),
         nop_i()),
        (0x50, 0x00, st16(3, 15, x6=0x21), nop_i(),
         nop_i()),
        (0x60, 0x00, ld8(29, 3), nop_i(),
         nop_i()),
        (0x70, 0x00, ld8(30, 4), nop_i(),
         nop_i()),
        (0x80, 0x10, nop_m(), nop_i(),
         br_cond(0x80, 0x80)),
    ], {
        "ip": 0x80,
        "r29": 0x1020304050607080,
        "r30": 0x8877665544332211,
        "exception": IA64_EXCP_NONE,
    }, entry=0x10)


def montecito_uc_memory_fault_test(name, fault_bundle, address,
                                   expected_isr):
    return require_registers(name, [
        (0x10, *movl_mlx(2, IA64_PSR_IC)),
        (0x20, 0x00, mov_gr_psr_full(2), nop_i(), nop_i()),
        (0x30, 0x00, srlz_d(), nop_i(), nop_i()),
        (0x40, *movl_mlx(3, address)),
        (0x50, *movl_mlx(4, 0x1122334455667788)),
        (0x60, *fault_bundle),
        (IA64_UNSUPPORTED_DATA_REFERENCE_VECTOR, 0x00,
         mov_m_cr_gr(14, 20), nop_i(), nop_i()),
        (IA64_UNSUPPORTED_DATA_REFERENCE_VECTOR + 0x10, 0x00,
         mov_m_cr_gr(15, 17), nop_i(), nop_i()),
        (IA64_UNSUPPORTED_DATA_REFERENCE_VECTOR + 0x20, 0x10,
         nop_m(), nop_i(),
         br_cond(IA64_UNSUPPORTED_DATA_REFERENCE_VECTOR + 0x20,
                 IA64_UNSUPPORTED_DATA_REFERENCE_VECTOR + 0x20)),
    ], {
        "ip": IA64_UNSUPPORTED_DATA_REFERENCE_VECTOR + 0x20,
        "exception": IA64_EXCP_NONE,
        "fault_code": IA64_EXCP_UNSUPPORTED_DATA_REFERENCE,
        "fault_ip": 0x60,
        "r14": address,
        "r15": expected_isr,
    }, entry=0x10)


MONTECITO_UC_16BYTE_ADDRESS = IA64_PHYS_UC_BIT | 0x2000

test_ld16_uc_unsupported_data_reference = montecito_uc_memory_fault_test(
    "ld16_uc_unsupported_data_reference",
    (0x00, ld16(8, 3), nop_i(), nop_i()),
    MONTECITO_UC_16BYTE_ADDRESS, IA64_ISR_R)

test_st16_uc_unsupported_data_reference = montecito_uc_memory_fault_test(
    "st16_uc_unsupported_data_reference",
    (0x00, st16(3, 4), nop_i(), nop_i()),
    MONTECITO_UC_16BYTE_ADDRESS, IA64_ISR_W)

test_cmp8xchg16_uc_unsupported_data_reference = \
    montecito_uc_memory_fault_test(
        "cmp8xchg16_uc_unsupported_data_reference",
        (0x00, cmp8xchg16_acq(8, 3, 4), nop_i(), nop_i()),
        MONTECITO_UC_16BYTE_ADDRESS + 8, IA64_ISR_R | IA64_ISR_W)

# Madison clears CPUID[4].ao, so the 16-byte atomic encodings are reserved.
test_ld16_madison_illegal_operation = require_exception(
    "ld16_madison_illegal_operation", [
        (0x10, 0x00, ld16(8, 3), nop_i(), nop_i()),
    ], IA64_EXCP_ILLEGAL, fault_ip=0x10, cpu="madison")

test_st16_madison_illegal_operation = require_exception(
    "st16_madison_illegal_operation", [
        (0x10, 0x00, st16(3, 4), nop_i(), nop_i()),
    ], IA64_EXCP_ILLEGAL, fault_ip=0x10, cpu="madison")

test_cmp8xchg16_madison_illegal_operation = require_exception(
    "cmp8xchg16_madison_illegal_operation", [
        (0x10, 0x00, cmp8xchg16_acq(8, 3, 4), nop_i(), nop_i()),
    ], IA64_EXCP_ILLEGAL, fault_ip=0x10, cpu="madison")

test_memory_order_completers_decode = require_registers(
    "memory_order_completers_decode", [
        (0x10, 0x00, addl(3, 0x200, 0), addl(4, 0x300, 0),
         nop_i()),
        (0x20, *movl_mlx(10, 0x0102030405060708)),
        (0x30, 0x00, nop_m(), addl(11, 10, 0),
         nop_i()),
        (0x40, 0x00, st8_rel(3, 10), nop_i(),
         nop_i()),
        (0x50, 0x00, st4(4, 11), nop_i(),
         nop_i()),
        (0x60, 0x00, ld8_c_clr_acq(12, 3), nop_i(),
         nop_i()),
        (0x70, 0x00, fetchadd4_rel(13, 4, 1), nop_i(),
         nop_i()),
        (0x80, 0x00, ld8(14, 4), nop_i(),
         nop_i()),
        (0x90, 0x10, nop_m(), nop_i(),
         br_cond(0x90, 0x90)),
    ], {
        "ip": 0x90,
        "r12": 0x0102030405060708,
        "r13": 10,
        "r14": 11,
        "exception": IA64_EXCP_NONE,
    }, entry=0x10)

test_data_big_endian_load_store = require_registers(
    "data_big_endian_load_store", [
        (0x10, 0x00, addl(3, 0x200, 0), addl(4, 0x201, 0),
         addl(5, 0x202, 0)),
        (0x20, 0x00, addl(6, 0x203, 0), nop_i(),
         nop_i()),
        (0x30, *movl_mlx(16, 0x11223344)),
        (0x40, 0x00, sum_um(IA64_PSR_BE), nop_i(),
         nop_i()),
        (0x50, 0x08, st4(3, 16), ld4(17, 3),
         nop_i()),
        (0x60, 0x00, rum(IA64_PSR_BE), nop_i(),
         nop_i()),
        (0x70, 0x08, ld1(18, 3), ld1(19, 4),
         nop_i()),
        (0x80, 0x08, ld1(20, 5), ld1(21, 6),
         nop_i()),
        (0x90, 0x10, nop_m(), nop_i(),
         br_cond(0x90, 0x90)),
    ], {
        "ip": 0x90,
        "r17": 0x11223344,
        "r18": 0x11,
        "r19": 0x22,
        "r20": 0x33,
        "r21": 0x44,
        "exception": IA64_EXCP_NONE,
    }, entry=0x10)

test_data_big_endian_cmpxchg4 = require_registers(
    "data_big_endian_cmpxchg4", [
        (0x10, 0x00, addl(3, 0x200, 0), addl(4, 0x201, 0),
         addl(5, 0x202, 0)),
        (0x20, 0x00, addl(6, 0x203, 0), nop_i(),
         nop_i()),
        (0x30, *movl_mlx(10, 0x01020304)),
        (0x40, *movl_mlx(16, 0x01020304)),
        (0x50, *movl_mlx(18, 0x11223344)),
        (0x60, 0x00, sum_um(IA64_PSR_BE), nop_i(),
         nop_i()),
        (0x70, 0x00, st4(3, 16), nop_i(),
         nop_i()),
        (0x80, 0x00, mov_m_gr_ar(10, 32), nop_i(),
         nop_i()),
        (0x90, 0x00, cmpxchg4_acq(17, 3, 18), nop_i(),
         nop_i()),
        (0xa0, 0x00, rum(IA64_PSR_BE), nop_i(),
         nop_i()),
        (0xb0, 0x08, ld1(19, 3), ld1(20, 4),
         nop_i()),
        (0xc0, 0x08, ld1(21, 5), ld1(22, 6),
         nop_i()),
        (0xd0, 0x10, nop_m(), nop_i(),
         br_cond(0xd0, 0xd0)),
    ], {
        "ip": 0xd0,
        "r17": 0x01020304,
        "r19": 0x11,
        "r20": 0x22,
        "r21": 0x33,
        "r22": 0x44,
        "exception": IA64_EXCP_NONE,
    }, entry=0x10)

test_store_invalidates_advanced_load = require_registers(
    "store_invalidates_advanced_load", [
        (0x10, 0x00, addl(3, 0x100, 0), nop_i(),
         nop_i()),
        (0x20, 0x00, ld8_a(4, 3), nop_i(),
         nop_i()),
        (0x30, *movl_mlx(5, 0xfeedfacecafebeef)),
        (0x40, 0x00, st8(3, 5), nop_i(),
         nop_i()),
        (0x50, *movl_mlx(4, 0xaa)),
        (0x60, 0x00, ld8_c_nc(4, 3), nop_i(),
         nop_i()),
        (0x70, 0x10, nop_m(), nop_i(),
         br_cond(0x70, 0x70)),
        (0x100, 0x00, 0x123456789abcdef0, 0,
         0),
    ], {"ip": 0x70, "r4": 0xfeedfacecafebeef}, entry=0x10)

test_fc_invalidates_advanced_load = require_registers(
    "fc_invalidates_advanced_load", [
        (0x10, 0x00, addl(3, 0x100, 0), nop_i(), nop_i()),
        (0x20, *movl_mlx(5, 0x123456789abcdef0)),
        (0x30, 0x00, st8(3, 5), nop_i(), nop_i()),
        (0x40, 0x00, ld8_a(4, 3), nop_i(), nop_i()),
        (0x50, *movl_mlx(4, 0xaa)),
        # fc removes the cache line holding [0x100] -> ALAT collision, so the
        # outstanding advanced load must be invalidated and ld.c must reload.
        (0x60, 0x00, fc_i(3), nop_i(), nop_i()),
        (0x70, 0x00, ld8_c_nc(4, 3), nop_i(), nop_i()),
        (0x80, 0x10, nop_m(), nop_i(), br_cond(0x80, 0x80)),
    ], {"ip": 0x80, "r4": 0x123456789abcdef0}, entry=0x10)

test_semaphore_ops_invalidate_advanced_loads = require_registers(
    "semaphore_ops_invalidate_advanced_loads", [
        (0x10, 0x00, addl(3, 0x200, 0), addl(4, 0x10, 0),
         nop_i()),
        (0x20, 0x08, st4(3, 4), ld4_a(5, 3),
         nop_i()),
        (0x30, 0x00, fetchadd4_acq(7, 3, 1, hint=3, ignored=0xf),
         addl(5, 0xaa, 0),
         nop_i()),
        (0x40, 0x00, ld4_c_nc(5, 3), addl(3, 0x210, 0),
         addl(4, 0x20, 0)),

        (0x50, 0x08, st4(3, 4), ld4_a(8, 3),
         addl(6, 0x33, 0)),
        (0x60, 0x00, xchg4(9, 3, 6), addl(8, 0xbb, 0),
         nop_i()),
        (0x70, 0x00, ld4_c_nc(8, 3), addl(3, 0x220, 0),
         addl(4, 0x40, 0)),

        (0x80, 0x08, st4(3, 4), ld4_a(10, 3),
         addl(6, 0x55, 0)),
        (0x90, 0x00, mov_m_imm_ar(32, 0x40), addl(10, 0xcc, 0),
         nop_i()),
        (0xa0, 0x00, cmpxchg4_acq(11, 3, 6), nop_i(),
         nop_i()),
        (0xb0, 0x00, ld4_c_nc(10, 3), addl(3, 0x230, 0),
         addl(4, 0x60, 0)),

        (0xc0, 0x08, st4(3, 4), ld4_a(12, 3),
         addl(6, 0x66, 0)),
        (0xd0, 0x00, mov_m_imm_ar(32, 0x61), addl(12, 0xdd, 0),
         nop_i()),
        (0xe0, 0x00, cmpxchg4_acq(13, 3, 6), nop_i(),
         nop_i()),
        (0xf0, 0x10, ld4_c_nc(12, 3), nop_i(),
         br_cond(0xf0, 0x100)),
        (0x100, 0x10, nop_m(), nop_i(),
         br_cond(0x100, 0x100)),
    ], {"ip": 0x100, "r5": 0x11, "r7": 0x10,
        "r8": 0x33, "r9": 0x20, "r10": 0x55, "r11": 0x40,
        "r12": 0xdd, "r13": 0x60}, entry=0x10)

test_xchg4_result_base_alias_invalidates_alat = require_registers(
    "xchg4_result_base_alias_invalidates_alat", [
        (0x10, 0x00, addl(3, 0x200, 0), addl(4, 0x11, 0),
         nop_i()),
        (0x20, 0x08, st4(3, 4), ld4_a(5, 3),
         addl(6, 0x22, 0)),
        (0x30, 0x00, xchg4(3, 3, 6), addl(5, 0xaa, 0),
         nop_i()),
        (0x40, 0x00, addl(7, 0x200, 0), nop_i(),
         nop_i()),
        (0x50, 0x00, ld4_c_nc(5, 7), nop_i(),
         nop_i()),
        (0x60, 0x00, ld4(8, 7), nop_i(),
         nop_i()),
        (0x70, 0x10, nop_m(), nop_i(),
         br_cond(0x70, 0x70)),
    ], {"ip": 0x70, "r3": 0x11, "r5": 0x22, "r8": 0x22},
    entry=0x10)

test_fetchadd4_result_base_alias_invalidates_alat = require_registers(
    "fetchadd4_result_base_alias_invalidates_alat", [
        (0x10, 0x00, addl(3, 0x200, 0), addl(4, 0x10, 0),
         nop_i()),
        (0x20, 0x08, st4(3, 4), ld4_a(5, 3),
         nop_i()),
        (0x30, 0x00, fetchadd4_acq(3, 3, 1), addl(5, 0xaa, 0),
         nop_i()),
        (0x40, 0x00, addl(7, 0x200, 0), nop_i(),
         nop_i()),
        (0x50, 0x00, ld4_c_nc(5, 7), nop_i(),
         nop_i()),
        (0x60, 0x00, ld4(8, 7), nop_i(),
         nop_i()),
        (0x70, 0x10, nop_m(), nop_i(),
         br_cond(0x70, 0x70)),
    ], {"ip": 0x70, "r3": 0x10, "r5": 0x11, "r8": 0x11},
    entry=0x10)

test_cmpxchg4_result_base_alias_success_invalidates_alat = require_registers(
    "cmpxchg4_result_base_alias_success_invalidates_alat", [
        (0x10, 0x00, addl(3, 0x200, 0), addl(4, 0x40, 0),
         nop_i()),
        (0x20, 0x08, st4(3, 4), ld4_a(5, 3),
         addl(6, 0x55, 0)),
        (0x30, 0x00, mov_m_imm_ar(32, 0x40), nop_i(),
         nop_i()),
        (0x40, 0x00, cmpxchg4_acq(3, 3, 6), addl(5, 0xaa, 0),
         nop_i()),
        (0x50, 0x00, addl(7, 0x200, 0), nop_i(),
         nop_i()),
        (0x60, 0x00, ld4_c_nc(5, 7), nop_i(),
         nop_i()),
        (0x70, 0x00, ld4(8, 7), nop_i(),
         nop_i()),
        (0x80, 0x10, nop_m(), nop_i(),
         br_cond(0x80, 0x80)),
    ], {"ip": 0x80, "r3": 0x40, "r5": 0x55, "r8": 0x55},
    entry=0x10)

test_cmpxchg4_result_base_alias_failure_keeps_alat = require_registers(
    "cmpxchg4_result_base_alias_failure_keeps_alat", [
        (0x10, 0x00, addl(3, 0x200, 0), addl(4, 0x40, 0),
         nop_i()),
        (0x20, 0x08, st4(3, 4), ld4_a(5, 3),
         addl(6, 0x55, 0)),
        (0x30, 0x00, mov_m_imm_ar(32, 0x41), nop_i(),
         nop_i()),
        (0x40, 0x00, cmpxchg4_acq(3, 3, 6), addl(5, 0xaa, 0),
         nop_i()),
        (0x50, 0x00, addl(7, 0x200, 0), nop_i(),
         nop_i()),
        (0x60, 0x00, ld4_c_nc(5, 7), nop_i(),
         nop_i()),
        (0x70, 0x00, ld4(8, 7), nop_i(),
         nop_i()),
        (0x80, 0x10, nop_m(), nop_i(),
         br_cond(0x80, 0x80)),
    ], {"ip": 0x80, "r3": 0x40, "r5": 0xaa, "r8": 0x40},
    entry=0x10)

test_cmpxchg4_full_ar_ccv_compare = require_registers(
    "cmpxchg4_full_ar_ccv_compare", [
        (0x10, 0x00, addl(3, 0x200, 0), addl(4, 0x40, 0),
         nop_i()),
        (0x20, *movl_mlx(9, 0x100000040)),
        (0x30, 0x08, st4(3, 4), ld4_a(5, 3),
         addl(6, 0x55, 0)),
        (0x40, 0x00, mov_m_gr_ar(9, 32), addl(5, 0xaa, 0),
         nop_i()),
        (0x50, 0x00, cmpxchg4_acq(10, 3, 6), nop_i(),
         nop_i()),
        (0x60, 0x00, ld4_c_nc(5, 3), nop_i(),
         nop_i()),
        (0x70, 0x00, ld4(8, 3), nop_i(),
         nop_i()),
        (0x80, 0x10, nop_m(), nop_i(),
         br_cond(0x80, 0x80)),
    ], {"ip": 0x80, "r5": 0xaa, "r8": 0x40, "r10": 0x40},
    entry=0x10)

test_semaphore_ops_clear_result_nat = require_registers(
    "semaphore_ops_clear_result_nat", [
        (0x10, 0x00, mov_m_imm_ar(36, 1), addl(6, 0x200, 0),
         nop_i()),

        (0x20, *movl_mlx(3, 0x300)),
        (0x30, 0x00, addl(4, 0x44, 0), nop_i(),
         nop_i()),
        (0x40, 0x08, st4(3, 4), ld8_fill_postinc(7, 6, 0),
         nop_i()),
        (0x50, 0x00, fetchadd4_acq(7, 3, 1), nop_i(),
         nop_i()),
        (0x60, 0x00, nop_m(), nop_i(), nop_i()),
        (0x70, 0x00, nop_m(), nop_i(), nop_i()),

        (0x80, *movl_mlx(3, 0x310)),
        (0x90, 0x00, addl(4, 0x55, 0), addl(8, 0x66, 0),
         nop_i()),
        (0xa0, 0x08, st4(3, 4), ld8_fill_postinc(9, 6, 0),
         nop_i()),
        (0xb0, 0x00, xchg4(9, 3, 8), nop_i(),
         nop_i()),
        (0xc0, 0x00, nop_m(), nop_i(), nop_i()),
        (0xd0, 0x00, nop_m(), nop_i(), nop_i()),

        (0xe0, *movl_mlx(3, 0x320)),
        (0xf0, 0x00, addl(4, 0x77, 0), addl(10, 0x88, 0),
         nop_i()),
        (0x100, 0x08, st4(3, 4), ld8_fill_postinc(11, 6, 0),
         nop_i()),
        (0x110, 0x00, mov_m_gr_ar(4, 32), nop_i(),
         nop_i()),
        (0x120, 0x00, cmpxchg4_acq(11, 3, 10), nop_i(),
         nop_i()),
        (0x130, 0x00, nop_m(), nop_i(), nop_i()),
        (0x140, 0x00, nop_m(), nop_i(), nop_i()),

        (0x150, *movl_mlx(3, 0x330)),
        (0x160, 0x00, addl(4, 0x99, 0), addl(10, 0xaa, 0),
         nop_i()),
        (0x170, 0x08, st4(3, 4), ld8_fill_postinc(12, 6, 0),
         nop_i()),
        (0x180, 0x00, mov_m_imm_ar(32, 0x98), nop_i(),
         nop_i()),
        (0x190, 0x00, cmpxchg4_acq(12, 3, 10), nop_i(),
         nop_i()),
        (0x1a0, 0x00, nop_m(), nop_i(), nop_i()),
        (0x1b0, 0x00, nop_m(), nop_i(), nop_i()),

        (0x1c0, 0x10, nop_m(), nop_i(),
         br_cond(0x1c0, 0x1c0)),
        (0x200, 0x00, 0, 0,
         0),
    ], {"ip": 0x1c0, "r7": 0x44, "r7_nat": 0,
        "r9": 0x55, "r9_nat": 0,
        "r11": 0x77, "r11_nat": 0,
        "r12": 0x99, "r12_nat": 0}, entry=0x10)

test_fetchadd4_unaligned_sets_read_write_isr = require_registers(
    "fetchadd4_unaligned_sets_read_write_isr", [
        (0x10, 0x00, addl(3, 0x101, 0), nop_i(),
         nop_i()),
        (0x20, 0x00, ssm(1 << 13), nop_i(),
         nop_i()),
        (0x30, 0x00, srlz_d(), nop_i(),
         nop_i()),
        (0x40, 0x00, fetchadd4_acq(7, 3, 1), nop_i(),
         nop_i()),
        (0x5a00, 0x00, mov_m_cr_gr(14, 20), nop_i(),
         nop_i()),
        (0x5a10, 0x00, mov_m_cr_gr(15, 17), nop_i(),
         nop_i()),
        (0x5a20, 0x10, nop_m(), nop_i(),
         br_cond(0x5a20, 0x5a20)),
    ], {
        "ip": 0x5a20,
        "exception": IA64_EXCP_NONE,
        "r14": 0x101,
        "r15": IA64_ISR_R | IA64_ISR_W,
    }, entry=0x10)

test_fetchadd4_nat_base_sets_read_write_isr = require_registers(
    "fetchadd4_nat_base_sets_read_write_isr", [
        (0x10, 0x00, mov_m_imm_ar(36, 1), addl(6, 0x200, 0),
         nop_i()),
        (0x20, 0x08, ld8_fill_postinc(3, 6, 0), nop_i(),
         nop_i()),
        (0x30, 0x00, ssm(1 << 13), nop_i(),
         nop_i()),
        (0x40, 0x00, srlz_d(), nop_i(),
         nop_i()),
        (0x50, 0x00, fetchadd4_acq(7, 3, 1), nop_i(),
         nop_i()),
        (0x5600, 0x00, mov_m_cr_gr(14, 20), nop_i(),
         nop_i()),
        (0x5610, 0x00, mov_m_cr_gr(15, 17), nop_i(),
         nop_i()),
        (0x5620, 0x10, nop_m(), nop_i(),
         br_cond(0x5620, 0x5620)),
        (0x200, 0x00, 0, 0,
         0),
    ], {
        "ip": 0x5620,
        "exception": IA64_EXCP_NONE,
        "r14": 0,
        # fetchadd is an access instruction: ISR.na stays 0
        # (SDM Vol.2 rev 1.1 Table 5-1).
        "r15": IA64_ISR_CODE_REG_NAT | IA64_ISR_R | IA64_ISR_W,
    }, entry=0x10)

NORMAL_LOAD_DATA = bundle_words(0x00, 0xdead, 0, 0)[0]

test_integer_nat_propagates_and_clears = require_registers(
    "integer_nat_propagates_and_clears", [
        (0x10, 0x00, mov_m_imm_ar(36, 1), addl(4, 0x200, 0),
         nop_i()),
        (0x20, 0x08, ld8_fill_postinc(5, 4, 0), nop_i(),
         nop_i()),
        (0x30, 0x00, nop_m(), adds(6, 1, 5),
         adds(7, 1, 0)),
        (0x40, 0x00, nop_m(), nop_i(), nop_i()),
        (0x50, 0x00, nop_m(), nop_i(), nop_i()),
        (0x60, 0x10, nop_m(), nop_i(),
         br_cond(0x60, 0x60)),
        (0x200, 0x00, 0, 0,
         0),
    ], {"ip": 0x60, "r6_nat": 1, "r7_nat": 0}, entry=0x10)

test_normal_load_clears_stale_nat = require_registers(
    "normal_load_clears_stale_nat", [
        (0x10, 0x00, mov_m_imm_ar(36, 1), addl(6, 0x200, 0),
         nop_i()),
        (0x20, 0x08, ld8_fill_postinc(5, 6, 0), nop_i(),
         nop_i()),
        (0x30, 0x08, ld8(5, 6), nop_i(),
         nop_i()),
        (0x40, 0x10, nop_m(), nop_i(),
         br_cond(0x40, 0x40)),
        (0x200, 0x00, 0xdead, 0,
         0),
    ], {"ip": 0x40, "r5": NORMAL_LOAD_DATA, "r5_nat": 0},
    entry=0x10)

test_integer_compare_nat_source_rules = require_registers(
    "integer_compare_nat_source_rules", [
        (0x10, 0x00, mov_m_imm_ar(36, 1), addl(8, 0x200, 0),
         nop_i()),
        (0x20, 0x08, ld8_fill_postinc(3, 8, 0), nop_i(),
         nop_i()),
        (0x30, 0x00, cmp4_eq_imm(6, 7, 1, 3), nop_i(),
         nop_i()),
        (0x40, 0x00, cmp_ge_or(8, 9, 3), nop_i(),
         nop_i()),
        (0x50, 0x00, cmp4_eq_unc_imm(10, 0, 0, 0), nop_i(),
         nop_i()),
        (0x60, 0x00, cmp4_eq_unc_imm(11, 0, 0, 0), nop_i(),
         nop_i()),
        (0x70, 0x00, cmp_eq_and(10, 11, 0, 3), nop_i(),
         nop_i()),
        (0x80, 0x00, nop_m(), adds(4, 1, 0, qp=6),
         adds(5, 1, 0, qp=7)),
        (0x90, 0x00, nop_m(), adds(12, 1, 0, qp=8),
         adds(13, 1, 0, qp=9)),
        (0xa0, 0x00, nop_m(), adds(14, 1, 0, qp=10),
         adds(15, 1, 0, qp=11)),
        (0xb0, 0x10, nop_m(), nop_i(),
         br_cond(0xb0, 0xb0)),
        (0x200, 0x00, 0, 0,
         0),
    ], {
        "ip": 0xb0,
        "r4": 0,
        "r5": 0,
        "r12": 0,
        "r13": 0,
        "r14": 0,
        "r15": 0,
        "exception": IA64_EXCP_NONE,
    }, entry=0x10)

test_tbit_nat_source_rules = require_registers(
    "tbit_nat_source_rules", [
        (0x10, 0x00, mov_m_imm_ar(36, 1), addl(8, 0x200, 0),
         nop_i()),
        (0x20, 0x08, ld8_fill_postinc(3, 8, 0), nop_i(),
         nop_i()),
        (0x30, 0x00, nop_m(), tbit_z(6, 7, 3, 0),
         nop_i()),
        (0x40, 0x00, nop_m(), tbit_z_or(8, 9, 3, 0),
         nop_i()),
        (0x50, 0x00, cmp4_eq_unc_imm(10, 0, 0, 0), nop_i(),
         nop_i()),
        (0x60, 0x00, cmp4_eq_unc_imm(11, 0, 0, 0), nop_i(),
         nop_i()),
        (0x70, 0x00, nop_m(), tbit_z_and(10, 11, 3, 0),
         nop_i()),
        (0x80, 0x00, nop_m(), adds(4, 1, 0, qp=6),
         adds(5, 1, 0, qp=7)),
        (0x90, 0x00, nop_m(), adds(12, 1, 0, qp=8),
         adds(13, 1, 0, qp=9)),
        (0xa0, 0x00, nop_m(), adds(14, 1, 0, qp=10),
         adds(15, 1, 0, qp=11)),
        (0xb0, 0x10, nop_m(), nop_i(),
         br_cond(0xb0, 0xb0)),
        (0x200, 0x00, 0, 0,
         0),
    ], {
        "ip": 0xb0,
        "r4": 0,
        "r5": 0,
        "r12": 0,
        "r13": 0,
        "r14": 0,
        "r15": 0,
        "exception": IA64_EXCP_NONE,
    }, entry=0x10)

test_normal_load_consumes_nat_base = require_exception(
    "normal_load_consumes_nat_base", [
        (0x10, 0x00, mov_m_imm_ar(36, 1), addl(6, 0x200, 0),
         nop_i()),
        (0x20, 0x08, ld8_fill_postinc(3, 6, 0), nop_i(),
         nop_i()),
        (0x30, 0x08, ld1(4, 3), nop_i(),
         nop_i()),
        (0x200, 0x00, 0, 0,
         0),
    ], IA64_EXCP_NAT_CONSUMPTION, fault_ip=0x30, entry=0x10)

test_nat_consumption_sets_ifa_isr = require_registers(
    "nat_consumption_sets_ifa_isr", [
        (0x10, 0x00, mov_m_imm_ar(36, 1), addl(6, 0x200, 0),
         nop_i()),
        (0x20, 0x08, ld8_fill_postinc(3, 6, 0), nop_i(),
         nop_i()),
        (0x30, 0x00, ssm(1 << 13), nop_i(),
         nop_i()),
        (0x40, 0x00, srlz_d(), nop_i(),
         nop_i()),
        (0x50, 0x08, ld1(4, 3), nop_i(),
         nop_i()),
        (0x5600, 0x00, mov_m_cr_gr(14, 20), nop_i(),
         nop_i()),
        (0x5610, 0x00, mov_m_cr_gr(15, 17), nop_i(),
         nop_i()),
        (0x5620, 0x10, nop_m(), nop_i(),
         br_cond(0x5620, 0x5620)),
        (0x200, 0x00, 0, 0,
         0),
    ], {"ip": 0x5620, "exception": IA64_EXCP_NONE, "r14": 0,
        # A load is an access instruction: ISR.na is reserved for
        # fc/lfetch/probe/tpa/tak (SDM Vol.2 rev 1.1 Table 5-1).
        "r15": IA64_ISR_CODE_REG_NAT | IA64_ISR_R},
    entry=0x10)

test_nat_consumption_ic0_preserves_ifa = register_nat_consumption_test(
    "nat_consumption_ic0_preserves_ifa",
    (0x00, mov_m_gr_ar(16, 65), nop_i(), nop_i()),
    IA64_ISR_NI | (1 << IA64_ISR_EI_SHIFT), enable_ic=False,
    initial_ifa=0x1122334455667788)

test_nat_store_data_consumption_is_access = require_registers(
    "nat_store_data_consumption_is_access", [
        (0x10, 0x00, mov_m_imm_ar(36, 1), addl(6, 0x200, 0),
         nop_i()),
        (0x20, 0x08, ld8_fill_postinc(5, 6, 0), addl(7, 0x208, 0),
         nop_i()),
        (0x30, 0x00, ssm(1 << 13), nop_i(),
         nop_i()),
        (0x40, 0x00, srlz_d(), nop_i(),
         nop_i()),
        (0x50, 0x00, st8(7, 5), nop_i(),
         nop_i()),
        (0x5600, 0x00, mov_m_cr_gr(14, 20), nop_i(),
         nop_i()),
        (0x5610, 0x00, mov_m_cr_gr(15, 17), nop_i(),
         nop_i()),
        (0x5620, 0x10, nop_m(), nop_i(),
         br_cond(0x5620, 0x5620)),
        (0x200, 0x00, 0, 0,
         0),
    ], {"ip": 0x5620, "exception": IA64_EXCP_NONE, "r14": 0,
        "r15": IA64_ISR_CODE_REG_NAT | IA64_ISR_W}, entry=0x10)

test_speculative_load_defers_nat_base = require_registers(
    "speculative_load_defers_nat_base", [
        (0x10, 0x00, mov_m_imm_ar(36, 1), addl(6, 0x200, 0),
         nop_i()),
        (0x20, 0x08, ld8_fill_postinc(3, 6, 0), nop_i(),
         nop_i()),
        (0x30, 0x08, ld8_s(4, 3), nop_i(),
         nop_i()),
        (0x40, 0x00, nop_m(), nop_i(), nop_i()),
        (0x50, 0x00, nop_m(), nop_i(), nop_i()),
        (0x60, 0x10, nop_m(), nop_i(),
         br_cond(0x60, 0x60)),
        (0x200, 0x00, 0, 0,
         0),
    ], {"ip": 0x60, "exception": IA64_EXCP_NONE, "r4_nat": 1},
    entry=0x10)

test_speculative_load_defers_psr_ed = require_registers(
    "speculative_load_defers_psr_ed", [
        (0x10, *movl_mlx(16, IA64_PSR_ED)),
        (0x20, 0x00, addl(3, 0x200, 0), nop_i(),
         nop_i()),
        (0x30, *movl_mlx(17, 0x100)),
        (0x40, 0x00, mov_m_gr_cr(16, 16), nop_i(),
         nop_i()),
        (0x50, 0x00, mov_m_gr_cr(17, 19), nop_i(),
         nop_i()),
        (0x60, 0x10, nop_m(), nop_i(),
         rfi_b()),
        (0x100, 0x00, ld8_s(4, 3), nop_i(),
         nop_i()),
        (0x110, 0x00, nop_m(), nop_i(), nop_i()),
        (0x120, 0x00, nop_m(), nop_i(), nop_i()),
        (0x130, 0x10, nop_m(), nop_i(),
         br_cond(0x130, 0x130)),
        (0x200, 0x00, 0x12345678, 0,
         0),
    ], {"ip": 0x130, "exception": IA64_EXCP_NONE, "r4_nat": 1},
    entry=0x10)

test_speculative_load_no_recovery_tlb_miss_faults = require_registers(
    "speculative_load_no_recovery_tlb_miss_faults", [
        (0x10, *movl_mlx(2, 0xa000000100020000)),
        (0x20, *movl_mlx(19, (1 << 13) | (1 << 17))),
        (0x30, 0x00, mov_gr_psr_full(19), nop_i(),
         nop_i()),
        (0x40, 0x00, srlz_d(), nop_i(),
         nop_i()),
        (0x50, 0x00, ld8_s(4, 2), nop_i(),
         nop_i()),
        (IA64_ALT_DTLB_VECTOR, 0x00, mov_m_cr_gr(31, 17),
         nop_i(), nop_i()),
        (IA64_ALT_DTLB_VECTOR + 0x10, 0x10, nop_m(), nop_i(),
         br_cond(IA64_ALT_DTLB_VECTOR + 0x10,
                 IA64_ALT_DTLB_VECTOR + 0x10)),
    ], {
        "ip": IA64_ALT_DTLB_VECTOR + 0x10,
        "exception": IA64_EXCP_NONE,
        "r31": IA64_ISR_R | IA64_ISR_SP,
    }, entry=0x10)

test_speculative_load_handler_psr_ed_defers_retry = require_registers(
    "speculative_load_handler_psr_ed_defers_retry", [
        (0x10, *movl_mlx(2, 0xa000000100020000)),
        (0x20, *movl_mlx(19, IA64_PSR_IC | IA64_PSR_DT)),
        (0x30, 0x00, mov_gr_psr_full(19), nop_i(),
         nop_i()),
        (0x40, 0x00, srlz_d(), nop_i(),
         nop_i()),
        (0x50, 0x00, ld8_s_postinc(4, 2, 8), nop_i(),
         nop_i()),
        (0x60, 0x00, mov_m_psr_gr(8), nop_i(),
         nop_i()),
        (0x70, 0x00, nop_m(), nop_i(), nop_i()),
        (0x80, 0x00, nop_m(), tbit_z(3, 4, 8, 43),
         nop_i()),
        (0x90, 0x00, nop_m(), addl(9, 1, 0, qp=3),
         addl(10, 1, 0, qp=4)),
        (0xa0, 0x10, nop_m(), nop_i(),
         br_cond(0xa0, 0xa0)),
        (IA64_ALT_DTLB_VECTOR, 0x00, mov_m_cr_gr(20, 16),
         nop_i(), nop_i()),
        (IA64_ALT_DTLB_VECTOR + 0x10, *movl_mlx(21, IA64_PSR_ED)),
        (IA64_ALT_DTLB_VECTOR + 0x20, 0x00, nop_m(),
         or_reg(20, 20, 21), nop_i()),
        (IA64_ALT_DTLB_VECTOR + 0x30, 0x00, mov_m_gr_cr(20, 16),
         nop_i(), nop_i()),
        (IA64_ALT_DTLB_VECTOR + 0x40, 0x10, nop_m(), nop_i(),
         rfi_b()),
    ], {
        "ip": 0xa0,
        "exception": IA64_EXCP_NONE,
        "r2": 0xa000000100020008,
        "r4_nat": 1,
        "r9": 1,
        "r10": 0,
    }, entry=0x10)

test_speculative_unaligned_no_recovery_faults = require_registers(
    "speculative_unaligned_no_recovery_faults", [
        (0x10, 0x00, nop_m(), addl(3, 0x104, 0),
         nop_i()),
        (0x20, *movl_mlx(19, (1 << 13) | (1 << 3))),
        (0x30, 0x00, mov_gr_psr_full(19), nop_i(),
         nop_i()),
        (0x40, 0x00, srlz_d(), nop_i(),
         nop_i()),
        (0x50, 0x00, ld8_s(4, 3), nop_i(),
         nop_i()),
        (IA64_UNALIGNED_VECTOR, 0x00, mov_m_cr_gr(31, 17),
         nop_i(), nop_i()),
        (IA64_UNALIGNED_VECTOR + 0x10, 0x10, nop_m(), nop_i(),
         br_cond(IA64_UNALIGNED_VECTOR + 0x10,
                 IA64_UNALIGNED_VECTOR + 0x10)),
    ], {
        "ip": IA64_UNALIGNED_VECTOR + 0x10,
        "exception": IA64_EXCP_NONE,
        "r31": IA64_ISR_R | IA64_ISR_SP,
    }, entry=0x10)

test_speculative_unimplemented_physical_unaligned_defers = require_registers(
    "speculative_unimplemented_physical_unaligned_defers", [
        (0x10, *movl_mlx(3, 0x76520ec5b2369f9e)),
        (0x20, *movl_mlx(19, IA64_PSR_IC | IA64_PSR_AC)),
        (0x30, 0x00, mov_gr_psr_full(19), nop_i(), nop_i()),
        (0x40, 0x00, srlz_d(), nop_i(), nop_i()),
        (0x50, 0x00, ld8_s(4, 3), nop_i(), nop_i()),
        (0x60, 0x10, nop_m(), nop_i(), br_cond(0x60, 0x60)),
    ], {
        "ip": 0x60,
        "exception": IA64_EXCP_NONE,
        "r4_nat": 1,
    }, entry=0x10)

test_unimplemented_physical_load_faults = require_registers(
    "unimplemented_physical_load_faults", [
        (0x10, *movl_mlx(3, 1 << IA64_IMPL_PA_BITS)),
        (0x20, *movl_mlx(19, IA64_PSR_IC)),
        (0x30, 0x00, mov_gr_psr_full(19), nop_i(), nop_i()),
        (0x40, 0x00, srlz_d(), nop_i(), nop_i()),
        (0x50, 0x00, ld8(4, 3), nop_i(), nop_i()),
        (IA64_GENERAL_VECTOR, 0x00, mov_m_cr_gr(8, 19), nop_i(), nop_i()),
        (IA64_GENERAL_VECTOR + 0x10, 0x00, mov_m_cr_gr(9, 17),
         nop_i(), nop_i()),
        (IA64_GENERAL_VECTOR + 0x20, 0x00, mov_m_cr_gr(10, 20),
         nop_i(), nop_i()),
        (IA64_GENERAL_VECTOR + 0x30, 0x10, nop_m(), nop_i(),
         br_cond(IA64_GENERAL_VECTOR + 0x30,
                 IA64_GENERAL_VECTOR + 0x30)),
    ], {
        "ip": IA64_GENERAL_VECTOR + 0x30,
        "exception": IA64_EXCP_NONE,
        "r8": 0x50,
        "r9": IA64_GENEX_UNIMPL_DATA_ADDR | IA64_ISR_R,
        "r10": 1 << IA64_IMPL_PA_BITS,
    }, entry=0x10)

# 245320-002 sec 3.2: Merced implements 54 virtual address bits, so
# VA{60:51} must sign-extend VA{50}.  A region-0 address with bit 51 set and
# bit 50 clear is unimplemented there and takes a General Exception with
# ISR.code 43, while Itanium 2 implements the full VA{60:0} and merely misses
# in the TLB.
MERCED_UNIMPLEMENTED_VA = 1 << 51

test_unimplemented_virtual_load_faults_merced = require_registers(
    "unimplemented_virtual_load_faults_merced", [
        (0x10, *movl_mlx(3, MERCED_UNIMPLEMENTED_VA)),
        (0x20, *movl_mlx(19, IA64_PSR_IC | IA64_PSR_DT)),
        (0x30, 0x00, mov_gr_psr_full(19), nop_i(), nop_i()),
        (0x40, 0x00, srlz_d(), nop_i(), nop_i()),
        (0x50, 0x00, ld8(4, 3), nop_i(), nop_i()),
        (IA64_GENERAL_VECTOR, 0x00, mov_m_cr_gr(8, 19), nop_i(), nop_i()),
        (IA64_GENERAL_VECTOR + 0x10, 0x00, mov_m_cr_gr(9, 17),
         nop_i(), nop_i()),
        (IA64_GENERAL_VECTOR + 0x20, 0x00, mov_m_cr_gr(10, 20),
         nop_i(), nop_i()),
        (IA64_GENERAL_VECTOR + 0x30, 0x10, nop_m(), nop_i(),
         br_cond(IA64_GENERAL_VECTOR + 0x30,
                 IA64_GENERAL_VECTOR + 0x30)),
    ], {
        "ip": IA64_GENERAL_VECTOR + 0x30,
        "exception": IA64_EXCP_NONE,
        "r8": 0x50,
        "r9": IA64_GENEX_UNIMPL_DATA_ADDR | IA64_ISR_R,
        "r10": MERCED_UNIMPLEMENTED_VA,
    }, entry=0x10, cpu="merced")

# The same address on Itanium 2 is implemented, so it reaches translation and
# takes an Alternate Data TLB fault instead.
test_unimplemented_virtual_load_translates_madison = require_registers(
    "unimplemented_virtual_load_translates_madison", [
        (0x10, *movl_mlx(3, MERCED_UNIMPLEMENTED_VA)),
        (0x20, *movl_mlx(19, IA64_PSR_IC | IA64_PSR_DT)),
        (0x30, 0x00, mov_gr_psr_full(19), nop_i(), nop_i()),
        (0x40, 0x00, srlz_d(), nop_i(), nop_i()),
        (0x50, 0x00, ld8(4, 3), nop_i(), nop_i()),
        (IA64_ALT_DTLB_VECTOR, 0x00, mov_m_cr_gr(10, 20), nop_i(), nop_i()),
        (IA64_ALT_DTLB_VECTOR + 0x10, 0x10, nop_m(), nop_i(),
         br_cond(IA64_ALT_DTLB_VECTOR + 0x10,
                 IA64_ALT_DTLB_VECTOR + 0x10)),
    ], {
        "ip": IA64_ALT_DTLB_VECTOR + 0x10,
        "exception": IA64_EXCP_NONE,
        "r10": MERCED_UNIMPLEMENTED_VA,
    }, entry=0x10, cpu="madison")

test_unimplemented_physical_precludes_unaligned = require_registers(
    "unimplemented_physical_precludes_unaligned", [
        (0x10, *movl_mlx(3, (1 << IA64_IMPL_PA_BITS) | 1)),
        (0x20, *movl_mlx(19, IA64_PSR_IC | IA64_PSR_AC)),
        (0x30, 0x00, mov_gr_psr_full(19), nop_i(), nop_i()),
        (0x40, 0x00, srlz_d(), nop_i(), nop_i()),
        (0x50, 0x00, ld8(4, 3), nop_i(), nop_i()),
        (IA64_GENERAL_VECTOR, 0x00, mov_m_cr_gr(8, 19), nop_i(), nop_i()),
        (IA64_GENERAL_VECTOR + 0x10, 0x00, mov_m_cr_gr(9, 17),
         nop_i(), nop_i()),
        (IA64_GENERAL_VECTOR + 0x20, 0x00, mov_m_cr_gr(10, 20),
         nop_i(), nop_i()),
        (IA64_GENERAL_VECTOR + 0x30, 0x10, nop_m(), nop_i(),
         br_cond(IA64_GENERAL_VECTOR + 0x30,
                 IA64_GENERAL_VECTOR + 0x30)),
    ], {
        "ip": IA64_GENERAL_VECTOR + 0x30,
        "exception": IA64_EXCP_NONE,
        "r8": 0x50,
        "r9": IA64_GENEX_UNIMPL_DATA_ADDR | IA64_ISR_R,
        "r10": (1 << IA64_IMPL_PA_BITS) | 1,
    }, entry=0x10)

test_speculative_recovery_unaligned_defers = require_registers(
    "speculative_recovery_unaligned_defers", [
        (0x10, *movl_mlx(18, LOW_VECTOR_TR_PTE | PTE_ED)),
        (0x20, 0x00, nop_m(), addl(3, 0x104, 0),
         nop_i()),
        (0x30, *movl_mlx(19, (1 << 13) | (1 << 36) | (1 << 3))),
        (0x40, 0x00, adds(7, LOW_VECTOR_ITIR, 0), adds(5, 5, 0),
         nop_i()),
        (0x50, 0x00, mov_m_gr_cr(7, 21), mov_m_gr_cr(0, 20),
         nop_i()),
        (0x60, 0x00, itr_i(5, 18), nop_i(),
         nop_i()),
        (0x70, 0x00, srlz_i(), adds(31, 0x430, 0),
         nop_i()),
        *rfi_to_gr(0x80, 19, 31),
        (0x4000430, 0x00, ld8_s(31, 3), nop_i(),
         nop_i()),
        (0x4000440, 0x00, nop_m(), nop_i(), nop_i()),
        (0x4000450, 0x00, nop_m(), nop_i(), nop_i()),
        (0x4000460, 0x10, nop_m(), nop_i(),
         br_cond(0x4000460, 0x460)),
    ], {
        "ip": 0x460,
        "exception": IA64_EXCP_NONE,
        "r31_nat": 1,
    }, entry=0x10)

test_ws2003_cmd646_unaligned_check_load_sets_ed = require_registers(
    "ws2003_cmd646_unaligned_check_load_sets_ed", [
        (0x10, *movl_mlx(18, LOW_VECTOR_TR_PTE | PTE_ED)),
        (0x20, 0x00, nop_m(), addl(3, 0x101, 0),
         nop_i()),
        (0x30, *movl_mlx(19, IA64_PSR_IC | IA64_PSR_IT | IA64_PSR_AC)),
        (0x40, 0x00, adds(7, 16 << 2, 0), adds(5, 5, 0),
         nop_i()),
        (0x50, 0x00, mov_m_gr_cr(7, 21), mov_m_gr_cr(0, 20),
         nop_i()),
        (0x60, 0x00, itr_i(5, 18), nop_i(),
         nop_i()),
        (0x70, 0x00, srlz_i(), adds(31, 0x430, 0),
         nop_i()),
        *rfi_to_gr(0x80, 19, 31),
        (0x4000430, 0x00, ld2_sa(30, 3), nop_i(),
         nop_i()),
        (0x4000440, 0x00, ld2_c_clr(30, 3), nop_i(),
         nop_i()),
        (0x4000000 + IA64_UNALIGNED_VECTOR, 0x00,
         mov_m_cr_gr(14, 20),
         nop_i(), nop_i()),
        (0x4000000 + IA64_UNALIGNED_VECTOR + 0x10, 0x00,
         mov_m_cr_gr(15, 17),
         nop_i(), nop_i()),
        (0x4000000 + IA64_UNALIGNED_VECTOR + 0x20, 0x00,
         mov_m_cr_gr(16, 22),
         nop_i(), nop_i()),
        (0x4000000 + IA64_UNALIGNED_VECTOR + 0x30, 0x10,
         nop_m(), nop_i(),
         br_cond(IA64_UNALIGNED_VECTOR + 0x30,
                 IA64_UNALIGNED_VECTOR + 0x30)),
    ], {
        "ip": IA64_UNALIGNED_VECTOR + 0x30,
        "exception": IA64_EXCP_NONE,
        "r14": 0x101,
        "r15": IA64_ISR_R | IA64_ISR_ED,
        "r16": 0x430,
    }, entry=0x10)

test_ld8_s_d2_hint_decode = require_registers("ld8_s_d2_hint_decode", [
    (0x10, 0x00, addl(3, 0x100, 0), nop_i(),
     nop_i()),
    (0x20, 0x00, ld8_s_hint(4, 3, 2), nop_i(),
     nop_i()),
    (0x30, 0x00, nop_m(), nop_i(), nop_i()),
    (0x40, 0x00, nop_m(), nop_i(), nop_i()),
    (0x50, 0x10, nop_m(), nop_i(),
     br_cond(0x50, 0x50)),
    (0x100, 0x00, 0x1122334455667788, 0,
     0),
], {"ip": 0x50, "exception": IA64_EXCP_NONE, "r4_nat": 1}, entry=0x10)

test_mov_crgr_clears_stale_nat = require_registers(
    "mov_crgr_clears_stale_nat", [
        (0x10, 0x00, addl(3, 0x104, 0), addl(5, 0x200, 0),
         nop_i()),
        (0x20, 0x00, sum_um(0x8), addl(16, 0x188, 0),
         nop_i()),
        (0x30, 0x00, ld8_s(28, 3), nop_i(),
         nop_i()),
        (0x40, 0x00, mov_m_gr_cr(16, 20), nop_i(),
         nop_i()),
        (0x50, 0x00, mov_m_cr_gr(28, 20), nop_i(),
         nop_i()),
        (0x60, 0x00, st8(5, 28), nop_i(),
         nop_i()),
        (0x70, 0x00, ld8(31, 5), nop_i(),
         nop_i()),
        (0x80, 0x00, nop_m(), nop_i(), nop_i()),
        (0x90, 0x00, nop_m(), nop_i(), nop_i()),
        (0xa0, 0x10, nop_m(), nop_i(),
         br_cond(0xa0, 0xa0)),
    ], {"ip": 0xa0, "exception": IA64_EXCP_NONE, "r28_nat": 0,
        "r31": 0x188}, entry=0x10)

test_ld1_postinc_decode = require_registers("ld1_postinc_decode", [
    (0x10, 0x00, addl(3, 0x100, 0), nop_i(),
     nop_i()),
    (0x20, 0x08, ld1_postinc(4, 3, 1), nop_i(),
     nop_i()),
    (0x30, 0x10, nop_m(), nop_i(),
     br_cond(0x30, 0x30)),
    (0x100, 0x00, 0x5a, 0,
     0),
], {"ip": 0x30, "r3": 0x101, "r4": 0x40}, entry=0x10)

test_ld1_reg_postinc_decode = require_registers(
    "ld1_reg_postinc_decode", [
        (0x10, 0x00, addl(3, 0x100, 0), addl(5, 1, 0),
         nop_i()),
        (0x20, 0x08, ld1_reg_postinc(4, 3, 5), nop_i(),
         nop_i()),
        (0x30, 0x10, nop_m(), nop_i(),
         br_cond(0x30, 0x30)),
        (0x100, 0x00, 0x5a, 0,
        0),
    ], {"ip": 0x30, "r3": 0x101, "r4": 0x40}, entry=0x10)

test_ld1_reg_postinc_uses_old_increment = require_registers(
    "ld1_reg_postinc_uses_old_increment", [
        (0x10, 0x00, addl(3, 0x100, 0), addl(5, 1, 0),
         nop_i()),
        (0x20, 0x08, ld1_reg_postinc(5, 3, 5), nop_i(),
         nop_i()),
        (0x30, 0x10, nop_m(), nop_i(),
         br_cond(0x30, 0x30)),
        (0x100, 0x00, 0x5a, 0,
         0),
    ], {"ip": 0x30, "r3": 0x101, "r5": 0x40}, entry=0x10)

test_ld_reg_postinc_same_target_illegal = require_exception(
    "ld_reg_postinc_same_target_illegal", [
        (0x10, 0x08, ld1_reg_postinc(3, 3, 5), nop_i(),
         nop_i()),
    ], IA64_EXCP_ILLEGAL, fault_ip=0x10)

test_ld_imm_postinc_same_target_illegal = require_exception(
    "ld_imm_postinc_same_target_illegal", [
        (0x10, 0x08, ld1_postinc(3, 3, 0), nop_i(),
         nop_i()),
    ], IA64_EXCP_ILLEGAL, fault_ip=0x10)

test_ld_postinc_same_target_predicated_false = require_registers(
    "ld_postinc_same_target_predicated_false", [
        (0x10, 0x00, addl(3, 0x100, 0), nop_i(),
         nop_i()),
        (0x20, 0x08, ld1_postinc(3, 3, 0, qp=1), nop_i(),
         nop_i()),
        (0x30, 0x10, nop_m(), nop_i(),
         br_cond(0x30, 0x30)),
        (0x100, 0x00, 0x5a, 0,
         0),
    ], {"ip": 0x30, "r3": 0x100, "exception": IA64_EXCP_NONE}, entry=0x10)

test_ld1_sa_postinc_decode = require_registers("ld1_sa_postinc_decode", [
    (0x10, 0x00, addl(3, 0x100, 0), nop_i(),
     nop_i()),
    (0x20, 0x08, ld1_sa_postinc(4, 3, 2), nop_i(),
     nop_i()),
    (0x30, 0x00, nop_m(), nop_i(), nop_i()),
    (0x40, 0x00, nop_m(), nop_i(), nop_i()),
    (0x50, 0x10, nop_m(), nop_i(),
     br_cond(0x50, 0x50)),
    (0x100, 0x00, 0x5a, 0,
     0),
], {"ip": 0x50, "r3": 0x102, "r4_nat": 1},
   entry=0x10)

test_ld8_nt1_postinc_decode = require_registers("ld8_nt1_postinc_decode", [
    (0x10, 0x00, addl(3, 0x100, 0), nop_i(),
     nop_i()),
    (0x20, 0x08, ld8_postinc(4, 3, 8, hint=1), nop_i(),
     nop_i()),
    (0x30, 0x08, ld8_s_postinc(5, 3, 8, hint=1), nop_i(),
     nop_i()),
    (0x40, 0x10, nop_m(), nop_i(),
     br_cond(0x40, 0x40)),
    (0x100, 0x00, 0x12345678, 0,
     0),
], {"ip": 0x40, "r3": 0x110, "exception": IA64_EXCP_NONE}, entry=0x10)

MEMORY_HINT_LD2_DATA = bundle_words(0x00, 0x1234, 0, 0)[0] & 0xffff
MEMORY_HINT_XCHG_OLD = bundle_words(0x00, 0x12, 0, 0)[0] & 0xff
MEMORY_HINT_CMPXCHG_OLD = bundle_words(0x00, 0x3344, 0, 0)[0] & 0xffff
MEMORY_HINT_LD4_ACQ_DATA = bundle_words(0x00, 0x778899aa, 0, 0)[0] & 0xffffffff

test_memory_cache_hints_decode = require_registers(
    "memory_cache_hints_decode", [
        (0x10, 0x00, addl(3, 0x100, 0), addl(5, 2, 0),
         nop_i()),
        (0x20, 0x00, ld2_c_clr_reg_update(4, 3, 5, hint=3), addl(6, 0xab, 0),
         addl(8, 0x200, 0)),
        (0x30, 0x00, xchg(0, 7, 8, 6, hint=3), nop_i(),
         nop_i()),
        (0x40, 0x00, ld1(9, 8), addl(10, 0x210, 0),
         addl(11, 0x5566, 0)),
        (0x50, 0x00, addl(14, MEMORY_HINT_CMPXCHG_OLD, 0), nop_i(),
         nop_i()),
        (0x60, 0x00, mov_m_gr_ar(14, 32), nop_i(),
         nop_i()),
        (0x70, 0x00, cmpxchg_rel(1, 12, 10, 11, hint=1), nop_i(),
         nop_i()),
        (0x80, 0x00, ld2(13, 10), addl(15, 0x220, 0),
         nop_i()),
        (0x90, 0x10, ld4_c_clr_acq(16, 15, hint=3), nop_i(),
         br_cond(0x90, 0xa0)),
        (0xa0, 0x10, nop_m(), nop_i(),
         br_cond(0xa0, 0xa0)),
        (0x100, 0x00, 0x1234, 0,
         0),
        (0x200, 0x00, 0x12, 0,
         0),
        (0x210, 0x00, 0x3344, 0,
         0),
        (0x220, 0x00, 0x778899aa, 0,
         0),
    ], {
        "ip": 0xa0,
        "r3": 0x102,
        "r4": MEMORY_HINT_LD2_DATA,
        "r7": MEMORY_HINT_XCHG_OLD,
        "r9": 0xab,
        "r12": MEMORY_HINT_CMPXCHG_OLD,
        "r13": 0x5566,
        "r16": MEMORY_HINT_LD4_ACQ_DATA,
        "exception": IA64_EXCP_NONE,
    }, entry=0x10)

test_ld8_fill_st8_spill_postinc_decode = require_registers(
    "ld8_fill_st8_spill_postinc_decode", [
        (0x10, 0x00, addl(3, 0x100, 0), nop_i(),
         nop_i()),
        (0x20, *movl_mlx(16, 0x123456789abcdef0)),
        (0x30, 0x00, st8_spill_postinc(3, 16, 16), nop_i(),
         nop_i()),
        (0x40, *movl_mlx(16, 0)),
        (0x50, 0x00, addl(3, 0x100, 0), nop_i(),
         nop_i()),
        (0x60, 0x00, ld8_fill_postinc(17, 3, 16), nop_i(),
         nop_i()),
        (0x70, 0x10, nop_m(), nop_i(),
         br_cond(0x70, 0x70)),
    ], {
        "ip": 0x70,
        "r3": 0x110,
        "r16": 0,
        "r17": 0x123456789abcdef0,
    }, entry=0x10)

test_ld8_fill_restores_unat_bit = require_registers(
    "ld8_fill_restores_unat_bit", [
        (0x10, *movl_mlx(9, 1 << 32)),
        (0x20, 0x00, mov_m_gr_ar(9, 36), addl(3, 0x100, 0),
         nop_i()),
        (0x30, 0x00, ld8_fill_postinc(17, 3, 0), nop_i(),
         nop_i()),
        (0x40, 0x10, nop_m(), nop_i(),
         br_cond(0x40, 0x40)),
    ], {
        "ip": 0x40,
        "r17_nat": 1,
        "ar_unat": 1 << 32,
    }, entry=0x10)

test_st8_spill_updates_unat_bit = require_registers(
    "st8_spill_updates_unat_bit", [
        (0x10, *movl_mlx(9, 1 << 32)),
        (0x20, 0x00, mov_m_gr_ar(9, 36), addl(3, 0x100, 0),
         nop_i()),
        (0x30, *movl_mlx(16, 0x123456789abcdef0)),
        (0x40, 0x00, st8_spill_postinc(3, 16, 16), nop_i(),
         nop_i()),
        (0x50, 0x10, nop_m(), nop_i(),
         br_cond(0x50, 0x50)),
    ], {"ip": 0x50, "ar_unat": 0}, entry=0x10)

test_integer_postinc_imm9_decode = require_registers(
    "integer_postinc_imm9_decode", [
        (0x10, 0x00, addl(3, 0x300, 0), nop_i(),
         nop_i()),
        (0x20, *movl_mlx(16, 0x8877665544332211)),
        (0x30, 0x00, st8_spill_postinc(3, 16, 176), nop_i(),
         nop_i()),
        (0x40, 0x00, nop_m(), adds(4, 0, 3),
         nop_i()),
        (0x50, 0x00, addl(3, 0x300, 0), nop_i(),
         nop_i()),
        (0x60, 0x00, ld8_fill_postinc(17, 3, -200), nop_i(),
         nop_i()),
        (0x70, 0x10, nop_m(), nop_i(),
         br_cond(0x70, 0x70)),
    ], {
        "ip": 0x70,
        "r3": 0x238,
        "r4": 0x3b0,
        "r17": 0x8877665544332211,
    }, entry=0x10)

test_st1_postinc_decode = require_registers("st1_postinc_decode", [
    (0x10, 0x00, addl(3, 0x100, 0), adds(4, 0x5a, 0),
     nop_i()),
    (0x20, 0x08, st1_postinc(3, 4, -1), nop_i(),
     nop_i()),
    (0x30, 0x10, nop_m(), nop_i(),
     br_cond(0x30, 0x30)),
], {"ip": 0x30, "r3": 0xff}, entry=0x10)

test_st8_postinc_same_base_value_uses_old_base = require_registers(
    "st8_postinc_same_base_value_uses_old_base", [
        (0x10, 0x00, addl(3, 0x200, 0), addl(5, 0x200, 0),
         nop_i()),
        (0x20, 0x08, st8_postinc(3, 3, 8), nop_i(),
         nop_i()),
        (0x30, 0x00, ld8(4, 5), nop_i(),
         nop_i()),
        (0x40, 0x10, nop_m(), nop_i(),
         br_cond(0x40, 0x40)),
    ], {
        "ip": 0x40,
        "r3": 0x208,
        "r4": 0x200,
        "exception": IA64_EXCP_NONE,
    }, entry=0x10)

test_cmpxchg4_uses_ar_ccv = require_registers("cmpxchg4_uses_ar_ccv", [
    (0x10, 0x00, addl(3, 0x200, 0), addl(4, 0xff, 0),
     nop_i()),
    (0x20, 0x00, st4(3, 4), addl(6, 0xf7, 0),
     nop_i()),
    (0x30, 0x00, mov_m_gr_ar(4, 32), nop_i(),
     nop_i()),
    (0x40, 0x00, cmpxchg4(5, 3, 6), nop_i(),
     nop_i()),
    (0x50, 0x00, load_mem(0x02, 7, 3), nop_i(),
     nop_i()),
    (0x60, 0x10, nop_m(), nop_i(),
     br_cond(0x60, 0x60)),
], {
    "ip": 0x60,
    "r5": 0xff,
    "r7": 0xf7,
    "ar_ccv": 0xff,
}, entry=0x10)

test_xchg4_decode = require_registers("xchg4_decode", [
    (0x10, 0x00, addl(3, 0x200, 0), addl(4, 0xff, 0),
     nop_i()),
    (0x20, 0x00, st4(3, 4), addl(6, 0xf7, 0),
     nop_i()),
    (0x30, 0x00, xchg4(5, 3, 6), nop_i(),
     nop_i()),
    (0x40, 0x00, load_mem(0x02, 7, 3), nop_i(),
     nop_i()),
    (0x50, 0x10, nop_m(), nop_i(),
     br_cond(0x50, 0x50)),
], {"ip": 0x50, "r5": 0xff, "r7": 0xf7}, entry=0x10)

test_cmpxchg4_repeated_word_updates = require_registers(
    "cmpxchg4_repeated_word_updates", [
        (0x10, *movl_mlx(3, 0x200)),
        (0x20, *movl_mlx(4, 0xffffffff)),
        (0x30, *movl_mlx(6, 0xfffffffe)),
        (0x40, *movl_mlx(7, 0xfffffffc)),
        (0x50, *movl_mlx(8, 0xfffffff8)),
        (0x60, 0x00, st4(3, 4), nop_i(),
         nop_i()),
        (0x70, 0x00, mov_m_gr_ar(4, 32), nop_i(),
         nop_i()),
        (0x80, 0x00, cmpxchg4(5, 3, 6), nop_i(),
         nop_i()),
        (0x90, 0x00, mov_m_gr_ar(6, 32), nop_i(),
         nop_i()),
        (0xa0, 0x00, cmpxchg4(9, 3, 7), nop_i(),
         nop_i()),
        (0xb0, 0x00, mov_m_gr_ar(7, 32), nop_i(),
         nop_i()),
        (0xc0, 0x00, cmpxchg4(10, 3, 8), nop_i(),
         nop_i()),
        (0xd0, 0x00, load_mem(0x02, 11, 3), nop_i(),
         nop_i()),
        (0xe0, 0x10, nop_m(), nop_i(),
         br_cond(0xe0, 0xe0)),
    ], {
        "ip": 0xe0,
        "r5": 0xffffffff,
        "r9": 0xfffffffe,
        "r10": 0xfffffffc,
        "r11": 0xfffffff8,
    }, entry=0x10)

test_cmp8xchg16_acq_stores_pair = require_registers(
    "cmp8xchg16_acq_stores_pair", [
        (0x10, *movl_mlx(20, 0x200)),
        (0x20, *movl_mlx(21, 0x208)),
        (0x30, *movl_mlx(4, 0x1111111122222222)),
        (0x40, *movl_mlx(5, 0x3333333344444444)),
        (0x50, *movl_mlx(6, 0x5555555566666666)),
        (0x60, *movl_mlx(7, 0x7777777788888888)),
        (0x70, 0x09, st8(20, 4), st8(21, 5),
         nop_i()),
        (0x80, 0x09, mov_m_gr_ar(5, 32), mov_m_gr_ar(7, 25),
         nop_i()),
        (0x90, 0x00, cmp8xchg16_acq(8, 21, 6, hint=3), nop_i(),
         nop_i()),
        (0xa0, 0x08, ld8(9, 20), ld8(10, 21),
         nop_i()),
        (0xb0, 0x10, nop_m(), nop_i(),
         br_cond(0xb0, 0xb0)),
    ], {
        "ip": 0xb0,
        "r8": 0x3333333344444444,
        "r9": 0x5555555566666666,
        "r10": 0x7777777788888888,
    }, entry=0x10)

test_cmp8xchg16_rel_mismatch_keeps_pair = require_registers(
    "cmp8xchg16_rel_mismatch_keeps_pair", [
        (0x10, *movl_mlx(20, 0x240)),
        (0x20, *movl_mlx(21, 0x248)),
        (0x30, *movl_mlx(4, 0xaaaaaaaa55555555)),
        (0x40, *movl_mlx(5, 0xbbbbbbbb66666666)),
        (0x50, *movl_mlx(6, 0xcccccccc77777777)),
        (0x60, *movl_mlx(7, 0xdddddddd88888888)),
        (0x70, 0x09, st8(20, 4), st8(21, 5),
         nop_i()),
        (0x80, 0x09, mov_m_gr_ar(6, 32), mov_m_gr_ar(7, 25),
         nop_i()),
        (0x90, 0x00, cmp8xchg16_rel(8, 21, 6, hint=1), nop_i(),
         nop_i()),
        (0xa0, 0x08, ld8(9, 20), ld8(10, 21),
         nop_i()),
        (0xb0, 0x10, nop_m(), nop_i(),
         br_cond(0xb0, 0xb0)),
    ], {
        "ip": 0xb0,
        "r8": 0xbbbbbbbb66666666,
        "r9": 0xaaaaaaaa55555555,
        "r10": 0xbbbbbbbb66666666,
    }, entry=0x10)

test_cmp8xchg16_unaligned = require_exception(
    "cmp8xchg16_unaligned", [
        (0x10, 0x00, addl(3, 0x204, 0), addl(2, 1, 0),
         nop_i()),
        (0x20, 0x00, cmp8xchg16_acq(4, 3, 2), nop_i(),
         nop_i()),
    ],
    IA64_EXCP_UNALIGNED, fault_ip=0x20,
)

test_cmp8xchg16_natpage_consumption = require_registers(
    "cmp8xchg16_natpage_consumption", [
        *dtr_setup_bundles(0x10, HIGH_TR_BASE, 0x400000,
                           pte_flags=DTR_PTE_NATPAGE),
        (0x70, *movl_mlx(19, IA64_PSR_IC | IA64_PSR_DT)),
        (0x80, 0x00, mov_gr_psr_full(19), nop_i(), nop_i()),
        (0x90, 0x00, srlz_d(), nop_i(), nop_i()),
        (0xa0, *movl_mlx(3, HIGH_TR_BASE + 0x208)),
        (0xb0, *movl_mlx(4, 0x1122334455667788)),
        (0xc0, 0x00, cmp8xchg16_acq(8, 3, 4), nop_i(), nop_i()),
        (IA64_NAT_CONSUMPTION_VECTOR, 0x00,
         mov_m_cr_gr(14, 20), nop_i(), nop_i()),
        (IA64_NAT_CONSUMPTION_VECTOR + 0x10, 0x00,
         mov_m_cr_gr(15, 17), nop_i(), nop_i()),
        (IA64_NAT_CONSUMPTION_VECTOR + 0x20, 0x10,
         nop_m(), nop_i(),
         br_cond(IA64_NAT_CONSUMPTION_VECTOR + 0x20,
                 IA64_NAT_CONSUMPTION_VECTOR + 0x20)),
    ], {
        "ip": IA64_NAT_CONSUMPTION_VECTOR + 0x20,
        "exception": IA64_EXCP_NONE,
        "fault_code": IA64_EXCP_NAT_CONSUMPTION,
        "fault_ip": 0xc0,
        "r14": HIGH_TR_BASE + 0x208,
        "r15": 0x20 | IA64_ISR_R | IA64_ISR_W,
    }, entry=0x10)

test_lfetch_decode = require_registers("lfetch_decode", [
    (0x10, 0x00, addl(3, 0x100, 0), addl(4, 0x180, 0),
     nop_i()),
    (0x20, 0x08, lfetch(3), lfetch(4, 0x2d),
     nop_i()),
    (0x30, 0x08, lfetch_postinc(3, 64), lfetch_postinc(4, 128, 0x2d, 1),
     nop_i()),
    (0x40, 0x08, adds(5, 0x20, 0), lfetch_reg_postinc(3, 5, 0x2e, 2),
     nop_i()),
    (0x50, 0x10, nop_m(), nop_i(),
    br_cond(0x50, 0x50)),
], {"ip": 0x50, "r3": 0x160, "r4": 0x200}, entry=0x10)

test_tnat_unc_same_pred_pred_false_illegal = require_exception(
    "tnat_unc_same_pred_pred_false_illegal",
    [(0x10, 0x00, nop_m(), tnat_z_unc(6, 6, 0, qp=7), nop_i())],
    IA64_EXCP_ILLEGAL,
    fault_ip=0x10,
)

test_tnat_nz_or_decode = require_registers("tnat_nz_or_decode", [
    (0x10, 0x00, nop_m(), adds(3, 0x104, 0),
     nop_i()),
    (0x20, 0x00, sum_um(0x8), nop_i(),
     nop_i()),
    (0x30, 0x00, ld8_s(15, 3), nop_i(),
     nop_i()),
    (0x40, 0x00, nop_m(), tnat_nz_or(7, 0, 15),
     nop_i()),
    (0x50, 0x00, nop_m(), adds(31, 1, 0, qp=7),
     nop_i()),
    (0x60, 0x10, nop_m(), nop_i(),
     br_cond(0x60, 0x60)),
], {"ip": 0x60, "r31": 1}, entry=0x10)

test_tnat_nz_and_ignored_bits_decode = require_registers(
    "tnat_nz_and_ignored_bits_decode", [
        (0x10, 0x00, cmp4_eq_imm(5, 31, 0, 0), adds(3, 0x104, 0),
         nop_i()),
        (0x20, 0x00, sum_um(0x8), nop_i(),
         nop_i()),
        (0x30, 0x00, ld8_s(15, 3), nop_i(),
         nop_i()),
        (0x40, 0x00, nop_m(), tnat_nz_and(5, 31, 15, ignored=0x0d),
         nop_i()),
        (0x50, 0x00, nop_m(), adds(31, 1, 0, qp=5),
         nop_i()),
        (0x60, 0x10, nop_m(), nop_i(),
         br_cond(0x60, 0x60)),
    ], {"ip": 0x60, "r31": 1}, entry=0x10)

test_chk_s_m_branches_on_nat = require_registers(
    "chk_s_m_branches_on_nat", [
        (0x10, *movl_mlx(9, 1 << 32)),
        (0x20, 0x00, mov_m_gr_ar(9, 36), addl(3, 0x100, 0),
         nop_i()),
        (0x30, 0x00, ld8_fill_postinc(17, 3, 0), nop_i(),
         nop_i()),
        (0x40, 0x08, nop_m(), chk_s_m(17, 0x40, 0x60),
         nop_i()),
        (0x50, 0x00, adds(4, 1, 0), nop_i(),
         nop_i()),
        (0x60, 0x10, nop_m(), nop_i(),
         br_cond(0x60, 0x60)),
    ], {"ip": 0x60, "r4": 0}, entry=0x10)

test_chk_s_i_long_branch_on_stacked_nat = require_registers(
    "chk_s_i_long_branch_on_stacked_nat", [
        (0x10, 0x00, alloc_m(45, 24, 16, 0, 0), nop_i(),
         nop_i()),
        (0x20, *movl_mlx(9, 1 << 32)),
        (0x30, 0x00, mov_m_gr_ar(9, 36), addl(3, 0x100, 0),
         nop_i()),
        (0x40, 0x00, ld8_fill_postinc(33, 3, 0), nop_i(),
         nop_i()),
        # The displacement fields deliberately overlap the hint.i pattern.
        (0x50, 0x08, nop_m(), nop_m(),
         chk_s_i(33, 0x50, 0x601f0)),
        (0x60, 0x10, nop_m(), adds(4, 1, 0),
         br_cond(0x60, 0x80)),
        (0x80, 0x10, nop_m(), nop_i(),
         br_cond(0x80, 0x80)),
        (0x601f0, 0x10, nop_m(), adds(5, 1, 0),
         br_cond(0x601f0, 0x60200)),
        (0x60200, 0x10, nop_m(), nop_i(),
         br_cond(0x60200, 0x60200)),
    ], {
        "ip": 0x60200,
        "exception": IA64_EXCP_NONE,
        "r4": 0,
        "r5": 1,
        "r33_nat": 1,
    }, entry=0x10)

test_chk_a_nc_m_decode = require_registers("chk_a_nc_m_decode", [
    (0x10, 0x00, addl(3, 0x100, 0), nop_i(),
     nop_i()),
    (0x20, 0x00, ld8_a(27, 3), nop_i(),
     nop_i()),
    (0x30, 0x00, chk_a_nc_m(27, 0x30, 0x50), adds(31, 0x56, 0),
     nop_i()),
    (0x40, 0x10, nop_m(), nop_i(),
     br_cond(0x40, 0x40)),
    (0x100, 0x00, 0x123456789abcdef0, 0,
     0),
], {"ip": 0x40, "exception": IA64_EXCP_NONE, "r31": 0x56}, entry=0x10)

test_mlx_chk_a_clr_nop_x_decode = require_registers(
    "mlx_chk_a_clr_nop_x_decode", [
        (0x10, 0x00, addl(3, 0x100, 0), nop_i(),
         nop_i()),
        (0x20, 0x00, ld8_a(22, 3), nop_i(),
         nop_i()),
        (0x30, 0x04, chk_a_clr_m(22, 0x30, 0x50), 0,
         nop_x()),
        (0x40, 0x10, nop_m(), adds(31, 0x55, 0),
         br_cond(0x40, 0x50)),
        (0x50, 0x10, nop_m(), nop_i(),
         br_cond(0x50, 0x50)),
        (0x100, 0x00, 0x123456789abcdef0, 0,
         0),
    ], {"ip": 0x50, "exception": IA64_EXCP_NONE, "r31": 0x55},
    entry=0x10)

test_chk_a_m_branches_on_miss = require_registers(
    "chk_a_m_branches_on_miss", [
        (0x10, 0x00, chk_a_nc_m(27, 0x10, 0x30), nop_i(),
         nop_i()),
        (0x20, 0x00, adds(4, 1, 0), nop_i(),
         nop_i()),
        (0x30, 0x10, nop_m(), nop_i(),
         br_cond(0x30, 0x30)),
    ], {"ip": 0x30, "r4": 0}, entry=0x10)

test_chk_a_clr_removes_entry = require_registers(
    "chk_a_clr_removes_entry", [
        (0x10, 0x00, addl(3, 0x100, 0), nop_i(),
         nop_i()),
        (0x20, 0x00, ld8_a(22, 3), nop_i(),
         nop_i()),
        (0x30, 0x00, chk_a_clr_m(22, 0x30, 0x60), nop_i(),
         nop_i()),
        (0x40, 0x00, chk_a_nc_m(22, 0x40, 0x60), adds(4, 1, 0),
         nop_i()),
        (0x50, 0x00, adds(5, 1, 0), nop_i(),
         nop_i()),
        (0x60, 0x10, nop_m(), nop_i(),
         br_cond(0x60, 0x60)),
        (0x100, 0x00, 0x123456789abcdef0, 0,
         0),
    ], {"ip": 0x60, "r4": 0, "r5": 0}, entry=0x10)

test_invala_e_gr_invalidates_selected_register = require_registers(
    "invala_e_gr_invalidates_selected_register", [
        (0x10, 0x00, addl(3, 0x100, 0), nop_i(),
         nop_i()),
        (0x20, 0x00, ld8_a(22, 3), nop_i(),
         nop_i()),
        (0x30, 0x00, ld8_a(23, 3), nop_i(),
         nop_i()),
        (0x40, 0x00, invala_e_gr(22), nop_i(),
         nop_i()),
        (0x50, 0x00, chk_a_nc_m(22, 0x50, 0x90), adds(4, 1, 0),
         nop_i()),
        (0x60, 0x00, adds(6, 1, 0), nop_i(),
         nop_i()),
        (0x70, 0x10, nop_m(), nop_i(),
         br_cond(0x70, 0x70)),
        (0x90, 0x00, chk_a_nc_m(23, 0x90, 0xc0), adds(5, 1, 0),
         nop_i()),
        (0xa0, 0x10, nop_m(), nop_i(),
         br_cond(0xa0, 0xa0)),
        (0xc0, 0x00, adds(7, 1, 0), nop_i(),
         nop_i()),
        (0xd0, 0x10, nop_m(), nop_i(),
         br_cond(0xd0, 0xd0)),
        (0x100, 0x00, 0x123456789abcdef0, 0,
         0),
    ], {"ip": 0xa0, "r4": 0, "r5": 1, "r6": 0, "r7": 0},
    entry=0x10)

test_alat_reloading_register_does_not_leave_duplicate = require_registers(
    "alat_reloading_register_does_not_leave_duplicate", [
        (0x10, 0x00, addl(3, 0x100, 0), addl(5, 0x110, 0),
         nop_i()),
        (0x20, 0x00, ld8_a(21, 3), nop_i(),
         nop_i()),
        (0x30, 0x00, ld8_a(22, 5), nop_i(),
         nop_i()),
        (0x40, 0x00, invala_e_gr(21), addl(5, 0x120, 0),
         nop_i()),
        (0x50, 0x00, ld8_a(22, 5), nop_i(),
         nop_i()),
        (0x60, 0x00, invala_e_gr(22), nop_i(),
         nop_i()),
        (0x70, 0x00, chk_a_nc_m(22, 0x70, 0xa0), nop_i(),
         nop_i()),
        (0x80, 0x00, adds(4, 1, 0), nop_i(),
         nop_i()),
        (0x90, 0x10, nop_m(), nop_i(),
         br_cond(0x90, 0x90)),
        (0xa0, 0x10, nop_m(), nop_i(),
         br_cond(0xa0, 0xa0)),
        (0x100, 0x00, 0x1111111111111111, 0,
         0),
        (0x110, 0x00, 0x2222222222222222, 0,
         0),
        (0x120, 0x00, 0x3333333333333333, 0,
         0),
    ], {"ip": 0xa0, "r4": 0}, entry=0x10)

test_invala_clears_all_alat_entries = require_registers(
    "invala_clears_all_alat_entries", [
        (0x10, 0x00, addl(3, 0x100, 0), nop_i(),
         nop_i()),
        (0x20, 0x00, ld8_a(22, 3), nop_i(),
         nop_i()),
        (0x30, 0x00, ld8_a(23, 3), nop_i(),
         nop_i()),
        (0x40, 0x00, invala(), nop_i(),
         nop_i()),
        (0x50, 0x00, chk_a_nc_m(22, 0x50, 0x80), adds(4, 1, 0),
         nop_i()),
        (0x60, 0x00, adds(6, 1, 0), nop_i(),
         nop_i()),
        (0x70, 0x10, nop_m(), nop_i(),
         br_cond(0x70, 0x70)),
        (0x80, 0x00, chk_a_nc_m(23, 0x80, 0xb0), adds(5, 1, 0),
         nop_i()),
        (0x90, 0x00, adds(7, 1, 0), nop_i(),
         nop_i()),
        (0xa0, 0x10, nop_m(), nop_i(),
         br_cond(0xa0, 0xa0)),
        (0xb0, 0x10, nop_m(), nop_i(),
         br_cond(0xb0, 0xb0)),
        (0x100, 0x00, 0x123456789abcdef0, 0,
         0),
    ], {"ip": 0xb0, "r4": 0, "r5": 0, "r6": 0, "r7": 0},
    entry=0x10)


test_st4_variants_preserve_adjacent_halfword = require_registers(
    "st4_variants_preserve_adjacent_halfword", [
        (0x10, *movl_mlx(2, 0x4000)),
        (0x20, *movl_mlx(3, 0xa1b2c3d455667788)),
        (0x30, *movl_mlx(4, 0x01234567deadbeef)),

        (0x40, 0x00, st8(2, 3), nop_i(), nop_i()),
        (0x50, 0x00, st4(2, 4), nop_i(), nop_i()),
        (0x60, 0x00, ld8(8, 2), nop_i(), nop_i()),

        (0x70, 0x00, st8(2, 3), nop_i(), nop_i()),
        (0x80, 0x00, st4_rel(2, 4), nop_i(), nop_i()),
        (0x90, 0x00, ld8(9, 2), nop_i(), nop_i()),

        (0xa0, 0x00, st8(2, 3), adds(5, 0, 2), nop_i()),
        (0xb0, 0x00, st4_postinc(5, 4, 4), nop_i(), nop_i()),
        (0xc0, 0x00, ld8(10, 2), nop_i(), nop_i()),
        (0xd0, 0x10, nop_m(), nop_i(), br_cond(0xd0, 0xd0)),
    ], {
        "ip": 0xd0,
        "exception": IA64_EXCP_NONE,
        "r5": 0x4004,
        "r8": 0xa1b2c3d4deadbeef,
        "r9": 0xa1b2c3d4deadbeef,
        "r10": 0xa1b2c3d4deadbeef,
    }, entry=0x10)


# SDM Vol 3, Table 4-29 (integer load/store x6 opcode extensions): spill and
# fill exist only as the 8-byte forms (ld8.fill x6=0x1b, st8.spill x6=0x3b).
# The x6 values 0x18-0x1a and 0x38-0x3a are blank in the table, so every
# instruction format that shares the integer load/store x6 space must raise
# an Illegal Operation fault for them.  The no-update (M1/M4) and
# imm-base-update (M3/M5) forms are covered separately so a future decoder
# split per format keeps faulting on the reserved values.
def reserved_memory_x6_test(name, slot0):
    return require_exception(name, [
        (0x10, 0x00, slot0, nop_i(), nop_i()),
    ], IA64_EXCP_ILLEGAL, fault_ip=0x10)

test_load_x6_18_reserved_illegal_operation = reserved_memory_x6_test(
    "load_x6_18_reserved_illegal_operation", load_mem(0x18, 8, 3))

test_load_x6_19_reserved_illegal_operation = reserved_memory_x6_test(
    "load_x6_19_reserved_illegal_operation", load_mem(0x19, 8, 3))

test_load_x6_1a_reserved_illegal_operation = reserved_memory_x6_test(
    "load_x6_1a_reserved_illegal_operation", load_mem(0x1a, 8, 3))

test_load_postinc_x6_18_reserved_illegal_operation = reserved_memory_x6_test(
    "load_postinc_x6_18_reserved_illegal_operation",
    load_mem_postinc(0x18, 8, 3, 8))

test_load_postinc_x6_19_reserved_illegal_operation = reserved_memory_x6_test(
    "load_postinc_x6_19_reserved_illegal_operation",
    load_mem_postinc(0x19, 8, 3, 8))

test_load_postinc_x6_1a_reserved_illegal_operation = reserved_memory_x6_test(
    "load_postinc_x6_1a_reserved_illegal_operation",
    load_mem_postinc(0x1a, 8, 3, 8))

# The register base-update form (M2) shares the same x6 space; stores have
# no register base-update form, so only the load side applies.
test_load_reg_postinc_x6_18_reserved_illegal_operation = \
    reserved_memory_x6_test(
        "load_reg_postinc_x6_18_reserved_illegal_operation",
        load_mem_reg_postinc(0x18, 8, 3, 4))

test_load_reg_postinc_x6_19_reserved_illegal_operation = \
    reserved_memory_x6_test(
        "load_reg_postinc_x6_19_reserved_illegal_operation",
        load_mem_reg_postinc(0x19, 8, 3, 4))

test_load_reg_postinc_x6_1a_reserved_illegal_operation = \
    reserved_memory_x6_test(
        "load_reg_postinc_x6_1a_reserved_illegal_operation",
        load_mem_reg_postinc(0x1a, 8, 3, 4))

test_store_x6_38_reserved_illegal_operation = reserved_memory_x6_test(
    "store_x6_38_reserved_illegal_operation", store_mem(0x38, 3, 4))

test_store_x6_39_reserved_illegal_operation = reserved_memory_x6_test(
    "store_x6_39_reserved_illegal_operation", store_mem(0x39, 3, 4))

test_store_x6_3a_reserved_illegal_operation = reserved_memory_x6_test(
    "store_x6_3a_reserved_illegal_operation", store_mem(0x3a, 3, 4))

test_store_postinc_x6_38_reserved_illegal_operation = reserved_memory_x6_test(
    "store_postinc_x6_38_reserved_illegal_operation",
    store_mem_postinc(0x38, 3, 4, 8))

test_store_postinc_x6_39_reserved_illegal_operation = reserved_memory_x6_test(
    "store_postinc_x6_39_reserved_illegal_operation",
    store_mem_postinc(0x39, 3, 4, 8))

test_store_postinc_x6_3a_reserved_illegal_operation = reserved_memory_x6_test(
    "store_postinc_x6_3a_reserved_illegal_operation",
    store_mem_postinc(0x3a, 3, 4, 8))

test_bsw_restores_banked_nat = require_registers(
    "bsw_restores_banked_nat", [
        (0x10, 0x00, mov_m_imm_ar(36, 1), addl(6, 0x200, 0),
         nop_i()),
        (0x20, 0x08, ld8_fill_postinc(16, 6, 0), nop_i(),
         nop_i()),
        (0x30, 0x13, nop_m(), nop_b(), bsw1()),
        # Clear the bank-1 r16 NaT while bank 0's NaT is saved.  Switching
        # back must restore the saved NaT bit together with the GR bank
        # (SDM Vol. 2, 3.3.7).
        (0x40, *movl_mlx(16, 0x55)),
        (0x50, 0x13, nop_m(), nop_b(), bsw0()),
        (0x60, 0x10, nop_m(), nop_i(),
         br_cond(0x60, 0x60)),
        (0x200, 0x00, 0, 0, 0),
    ], {
        "ip": 0x60,
        "exception": IA64_EXCP_NONE,
        "psr": 0,
        "r16_nat": 1,
    }, entry=0x10)

test_cloop_zero_st1_invalidates_alat_range = require_registers(
    "cloop_zero_st1_invalidates_alat_range", [
        (0x10, *movl_mlx(2, 0x8000)),
        (0x20, 0x00, adds(3, 8, 2), nop_i(),
         nop_i()),
        (0x30, 0x00, ld8_a(22, 3), nop_i(),
         nop_i()),
        (0x40, 0x00, adds(8, 15, 0), nop_i(),
         nop_i()),
        (0x50, 0x02, nop_m(), mov_lc_gr(8),
         nop_i()),
        (0x60, 0x10, st1_postinc(2, 0, 1), nop_i(),
         br_cloop(0x60, 0x60)),
        (0x70, 0x00, chk_a_nc_m(22, 0x70, 0xa0), adds(4, 1, 0),
         nop_i()),
        (0x80, 0x00, adds(5, 1, 0), nop_i(),
         nop_i()),
        (0x90, 0x10, nop_m(), nop_i(),
         br_cond(0x90, 0x90)),
        (0xa0, 0x10, nop_m(), nop_i(),
         br_cond(0xa0, 0xa0)),
    ], {
        "ip": 0xa0,
        "exception": IA64_EXCP_NONE,
        "r2": 0x8010,
        "r4": 0,
        "r5": 0,
    }, entry=0x10)

test_cloop_zero_st1_clears_cross_page_range = require_registers(
    "cloop_zero_st1_clears_cross_page_range", [
        (0x10, *movl_mlx(2, 0x7ff0)),
        (0x20, 0x00, adds(4, 0xff, 0),
         addl(8, 8224 - 1, 0), nop_i()),
        (0x30, 0x02, nop_m(), mov_lc_gr(8), nop_i()),
        (0x40, 0x10, st1_postinc(2, 4, 1), nop_i(),
         br_cloop(0x40, 0x40)),

        (0x50, *movl_mlx(2, 0x7ff0)),
        (0x60, 0x00, addl(8, 8224 - 1, 0), nop_i(), nop_i()),
        (0x70, 0x02, nop_m(), mov_lc_gr(8), nop_i()),
        (0x80, 0x10, st1_postinc(2, 0, 1), nop_i(),
         br_cloop(0x80, 0x80)),

        (0x90, *movl_mlx(2, 0x7ff0)),
        (0xa0, 0x00, addl(8, 8224 - 1, 0), adds(10, 0, 0), nop_i()),
        (0xb0, 0x02, nop_m(), mov_lc_gr(8), nop_i()),
        (0xc0, 0x10, ld1_postinc(11, 2, 1), or_reg(10, 10, 11),
         br_cloop(0xc0, 0xc0)),
        (0xd0, 0x02, nop_m(), mov_ar_lc(9), nop_i()),
        (0xe0, 0x10, nop_m(), nop_i(), br_cond(0xe0, 0xe0)),
    ], {
        "ip": 0xe0,
        "exception": IA64_EXCP_NONE,
        "r2": 0xa010,
        "r9": 0,
        "r10": 0,
    }, entry=0x10)


def test_speculative_stacked_nat_survives_backing_store_switch(qemu):
    """A deferred speculative load's NaT must survive a software backing-store
    switch.  Move to BSPSTORE makes AR.RNAT undefined and drops every internal
    partial collection, so the architected save and restore of BSPSTORE and
    RNAT around the switch is the only path back for a spilled NaT bit.  This
    is the sequence an operating system runs on a kernel entry, and it is the
    one case the call and interrupt variants above never reach.  Sweeping the
    initial collection index covers both a bit that already reached the
    backing store and a bit still held in a partial AR.RNAT collection."""
    for collect_bit in range(63):
        bspstore = 0x100000 + collect_bit * 8
        run_program(qemu, [
            (0x10, *movl_mlx(2, bspstore)),
            (0x20, 0x01, mov_m_gr_ar(2, 18), nop_i(),
             nop_i()),
            (0x30, 0x00, alloc_m(43, 20, 13, 0, 0), nop_i(),
             nop_i()),
            (0x40, *movl_mlx(9, 1)),
            (0x50, 0x00, mov_m_gr_ar(9, 36), addl(3, 0x300, 0),
             nop_i()),
            (0x60, 0x08, ld8_fill_postinc(40, 3, 0), nop_i(),
             nop_i()),
            (0x70, 0x00, ld8_s_postinc(32, 40, 8), nop_i(),
             nop_i()),
            (0x80, 0x10, nop_m(), nop_i(),
             br_call(0, 0x80, 0x200)),
            (0x90, 0x08, nop_m(), chk_s_m(32, 0x90, 0xb0),
             nop_i()),
            (0xa0, 0x10, nop_m(), adds(8, 1, 0),
             br_cond(0xa0, 0xc0)),
            (0xb0, 0x10, nop_m(), adds(10, 1, 0),
             br_cond(0xb0, 0xc0)),
            (0xc0, 0x10, nop_m(), nop_i(),
             br_cond(0xc0, 0xc0)),
            # Spill the caller frame into the original store, switch to a
            # second store and drive the RSE against it, then restore the
            # architected pointer pair before returning.
            (0x200, 0x01, alloc_m(34, 96, 88, 0, 0), nop_i(),
             nop_i()),
            (0x210, 0x01, flushrs_enc(), nop_i(),
             nop_i()),
            (0x220, 0x00, mov_m_ar_gr(20, 18), nop_i(),
             nop_i()),
            (0x230, 0x00, mov_m_ar_gr(21, 19), nop_i(),
             nop_i()),
            (0x240, *movl_mlx(22, 0x200000)),
            (0x250, 0x00, mov_m_gr_ar(22, 18), nop_i(),
             nop_i()),
            (0x260, 0x18, nop_m(), nop_m(),
             cover_b()),
            (0x270, 0x00, flushrs_enc(), nop_i(),
             nop_i()),
            (0x280, 0x00, mov_m_gr_ar(20, 18), nop_i(),
             nop_i()),
            (0x290, 0x00, mov_m_gr_ar(21, 19), nop_i(),
             nop_i()),
            (0x2a0, 0x10, nop_m(), nop_i(),
             br_ret(0)),
            (0x300, 0x00, 0x400, 0,
             0),
        ], entry=0x10, terminal_ip=0xc0, expected={
            "exception": IA64_EXCP_NONE,
            "r8": 0,
            "r10": 1,
            "r32_nat": 1,
        }, name=(
            "speculative_stacked_nat_survives_backing_store_switch_"
            f"collect_bit_{collect_bit}"))


def test_ld2_bias_st2_raw_large_frame_sequence(qemu):
    result = run_program(qemu, [
        (0x10, *movl_mlx(2, 0x8000)),
        (0x20, 0x00, st2(2, 0), nop_i(), nop_i()),
        (0x30, *movl_mlx(3, 0x8010)),
        (0x40, 0x00, st8(3, 0), nop_i(), nop_i()),
        (0x50, 0x00, nop_m(), alloc(100, 73, 70, 0, 0), nop_i()),
        (0x60, *movl_mlx(88, 0x8010)),
        (0x70, *movl_mlx(5, 13)),
        (0x80, 0x02, nop_m(), mov_lc_gr(5), nop_i()),
        (0x90, 0x10, nop_m(), nop_i(), bsw1()),
        (0xa0, *movl_mlx(22, 0x8000)),

        # Preserve the original instruction words, template stops, and the
        # unrelated speculative load around the 16-bit accounting update.
        # Fourteen iterations match the number of live entries observed in
        # the failing leaf table.
        raw_bundle(0xb0, 0x093010882c00a80b, 0x0004000000420054),
        raw_bundle(0xc0, 0x067011882c4c0018, 0x2000000000207160),
        (0xd0, 0x11, nop_m(), nop_i(), br_cloop(0xd0, 0xb0)),
        (0xe0, 0x01, ld2(8, 22), nop_i(), nop_i()),
        (0xf0, 0x11, nop_m(), nop_i(), br_cond(0xf0, 0xf0)),
    ], entry=0x10, terminal_ip=0xf0, expected={
        "exception": IA64_EXCP_NONE,
        "r8": 14,
    }, name="ld2_bias_st2_raw_large_frame_sequence")
    if result.state.gr[8] != 14:
        raise RuntimeError(
            "ld2_bias_st2_raw_large_frame_sequence failed: "
            f"counter={result.state.gr[8]!r}\n{result.register_output}")

test_mov_ar_nat_source_consumes = register_nat_consumption_test(
    "mov_ar_nat_source_consumes",
    (0x00, mov_m_gr_ar(16, 65), nop_i(), nop_i()),
    1 << IA64_ISR_EI_SHIFT)

test_mov_br_nat_source_consumes = register_nat_consumption_test(
    "mov_br_nat_source_consumes",
    (0x09, nop_m(), nop_m(), mov_b_gr(0, 16)),
    2 << IA64_ISR_EI_SHIFT)

test_mov_pr_nat_source_consumes = register_nat_consumption_test(
    "mov_pr_nat_source_consumes",
    (0x00,
     bitfield(3, 33, 3) | bitfield(16, 13, 7) | bitfield(0x7f, 6, 7),
     nop_i(), nop_i()))

test_mov_cr_nat_source_consumes = register_nat_consumption_test(
    "mov_cr_nat_source_consumes",
    (0x00, mov_m_gr_cr(16, 0), nop_i(), nop_i()))

test_mov_psr_nat_source_consumes = register_nat_consumption_test(
    "mov_psr_nat_source_consumes",
    (0x00, mov_m_gr_psrl(16), nop_i(), nop_i()))

test_mov_um_nat_source_consumes = register_nat_consumption_test(
    "mov_um_nat_source_consumes",
    (0x00, mov_m_gr_psr_um(16), nop_i(), nop_i()))

test_mov_rr_nat_index_consumes = register_nat_consumption_test(
    "mov_rr_nat_index_consumes",
    (0x00, mov_rr_read(17, 16), nop_i(), nop_i()))

test_mov_pkr_nat_index_consumes = register_nat_consumption_test(
    "mov_pkr_nat_index_consumes",
    (0x00, mov_pkr_indexed(16, 17, bit36=1), nop_i(), nop_i()))

test_mov_pmc_nat_value_consumes = register_nat_consumption_test(
    "mov_pmc_nat_value_consumes",
    (0x00, mov_grpmc_indexed(3, 16), nop_i(), nop_i()))

test_mov_cpuid_nat_index_consumes = register_nat_consumption_test(
    "mov_cpuid_nat_index_consumes",
    (0x00, mov_cpuid(17, 16), nop_i(), nop_i()))

test_fc_nat_source_consumes_non_access = register_nat_consumption_test(
    "fc_nat_source_consumes_non_access",
    (0x00, fc_i(16), nop_i(), nop_i()),
    IA64_ISR_NA | IA64_ISR_R | 1)

test_simd_helper_nat_propagates = require_registers("simd_helper_nat_propagates", [
    (0x10, 0x00, mov_m_imm_ar(36, 1), addl(4, 0x200, 0),
     nop_i()),
    (0x20, 0x08, ld8_fill_postinc(5, 4, 0), nop_i(),
     nop_i()),
    (0x30, *movl_mlx(6, 0x0001000200030004)),
    (0x40, 0x02, nop_m(), pmpy2(7, 5, 6),
     mux1_rev(8, 5)),
    (0x50, 0x02, nop_m(), czx1_r(9, 5),
     pack2_sss(10, 5, 6)),
    (0x60, 0x00, nop_m(), nop_i(), nop_i()),
    (0x70, 0x00, nop_m(), nop_i(), nop_i()),
    (0x80, 0x00, nop_m(), nop_i(), nop_i()),
    (0x90, 0x00, nop_m(), nop_i(), nop_i()),
    (0xa0, 0x10, nop_m(), nop_i(),
     br_cond(0xa0, 0xa0)),
    (0x200, 0x00, 0, 0,
     0),
], {
    "ip": 0xa0,
    "r7_nat": 1,
    "r8_nat": 1,
    "r9_nat": 1,
    "r10_nat": 1,
}, entry=0x10)

test_pshr_nat_propagates = require_registers("pshr_nat_propagates", [
    (0x10, 0x00, mov_m_imm_ar(36, 1), addl(4, 0x200, 0),
     nop_i()),
    (0x20, 0x08, ld8_fill_postinc(5, 4, 0), nop_i(),
     nop_i()),
    (0x30, 0x00, nop_m(), pshr4(6, 5, 1),
     pshr2(7, 0, 5, variable=True)),
    (0x40, 0x00, nop_m(), nop_i(), nop_i()),
    (0x50, 0x00, nop_m(), nop_i(), nop_i()),
    (0x60, 0x10, nop_m(), nop_i(),
     br_cond(0x60, 0x60)),
    (0x200, 0x00, 0, 0,
     0),
], {
    "ip": 0x60,
    "r6_nat": 1,
    "r7_nat": 1,
}, entry=0x10)

test_pshl_nat_propagates = require_registers("pshl_nat_propagates", [
    (0x10, 0x00, mov_m_imm_ar(36, 1), addl(4, 0x200, 0),
     nop_i()),
    (0x20, 0x08, ld8_fill_postinc(5, 4, 0), nop_i(),
     nop_i()),
    (0x30, 0x00, addl(6, 4, 0), nop_i(),
     nop_i()),
    (0x40, 0x00, nop_m(), pshl4(7, 5, 6),
     pshl2(8, 0, 5)),
    (0x50, 0x00, nop_m(), nop_i(), nop_i()),
    (0x60, 0x00, nop_m(), nop_i(), nop_i()),
    (0x70, 0x10, nop_m(), nop_i(),
     br_cond(0x70, 0x70)),
    (0x200, 0x00, 0, 0,
     0),
], {
    "ip": 0x70,
    "r7_nat": 1,
    "r8_nat": 1,
}, entry=0x10)

test_firmware_unaligned_load_assist = require_registers(
    "firmware_unaligned_load_assist",
    [
        (0x10, *movl_mlx(20, 0x1122334455667788)),
        (0x20, *movl_mlx(21, 0x99aabbccddeeff00)),
        (0x30, 0x00, addl(3, 0x100, 0), nop_i(), nop_i()),
        (0x40, 0x0a, st8(3, 20), adds(3, 8, 3), nop_i()),
        (0x50, 0x0a, st8(3, 21), adds(3, -4, 3), nop_i()),
        (0x60, 0x00, addl(2, 0x108000, 0), nop_i(), nop_i()),
        (0x70, 0x00, mov_m_gr_cr(2, 2), nop_i(), nop_i()),
        (0x80, 0x00, ssm((1 << 13) | (1 << 3)), nop_i(), nop_i()),
        (0x90, 0x0a, ld8(22, 3), adds(23, 1, 0), nop_i()),
        (0xa0, 0x10, nop_m(), nop_i(), br_cond(0xa0, 0xa0)),
    ],
    {
        "ip": 0xa0,
        "exception": IA64_EXCP_NONE,
        "r22": 0xddeeff0011223344,
        "r23": 1,
    },
)

test_firmware_unaligned_store_assist = require_registers(
    "firmware_unaligned_store_assist",
    [
        (0x10, *movl_mlx(20, 0x1122334455667788)),
        (0x20, *movl_mlx(21, 0x99aabbccddeeff00)),
        (0x30, *movl_mlx(24, 0xaabbccddeeff0011)),
        (0x40, 0x00, addl(3, 0x100, 0), nop_i(), nop_i()),
        (0x50, 0x0a, st8(3, 20), adds(3, 8, 3), nop_i()),
        (0x60, 0x0a, st8(3, 21), adds(3, -4, 3), nop_i()),
        (0x70, 0x00, addl(2, 0x108000, 0), nop_i(), nop_i()),
        (0x80, 0x00, mov_m_gr_cr(2, 2), nop_i(), nop_i()),
        (0x90, 0x00, ssm((1 << 13) | (1 << 3)), nop_i(), nop_i()),
        (0xa0, 0x0a, st8(3, 24), adds(25, 1, 0), nop_i()),
        (0xb0, 0x00, adds(3, -4, 3), nop_i(), nop_i()),
        (0xc0, 0x0a, ld8(26, 3), adds(3, 8, 3), nop_i()),
        (0xd0, 0x00, ld8(27, 3), nop_i(), nop_i()),
        (0xe0, 0x10, nop_m(), nop_i(), br_cond(0xe0, 0xe0)),
    ],
    {
        "ip": 0xe0,
        "exception": IA64_EXCP_NONE,
        "r25": 1,
        "r26": 0xeeff001155667788,
        "r27": 0x99aabbccaabbccdd,
    },
)

test_firmware_unaligned_speculative_load_assist = require_registers(
    "firmware_unaligned_speculative_load_assist",
    [
        (0x10, *movl_mlx(20, 0x1122334455667788)),
        (0x20, *movl_mlx(21, 0x99aabbccddeeff00)),
        (0x30, 0x00, addl(3, 0x300, 0), addl(5, 0x304, 0),
         nop_i()),
        (0x40, 0x0a, st8(3, 20), adds(3, 8, 3), nop_i()),
        (0x50, 0x0a, st8(3, 21), adds(3, -8, 3), nop_i()),
        (0x60, 0x00, addl(2, 0x108000, 0), nop_i(), nop_i()),
        (0x70, 0x00, mov_m_gr_cr(2, 2), nop_i(), nop_i()),
        (0x80, 0x00, ssm((1 << 13) | (1 << 3)), nop_i(), nop_i()),
        (0x90, 0x00, ld8_s(4, 5), nop_i(), nop_i()),
        (0xa0, 0x00, nop_m(), adds(8, 0, 4), nop_i()),
        (0xb0, 0x00, nop_m(), nop_i(), nop_i()),
        (0xc0, 0x00, nop_m(), nop_i(), nop_i()),
        (0xd0, 0x00, ld8_a(6, 3), nop_i(), nop_i()),
        (0xe0, 0x00, nop_m(), addl(6, 0x55, 0), nop_i()),
        (0xf0, 0x00, ld8_a(6, 5), nop_i(), nop_i()),
        (0x100, 0x00, nop_m(), adds(11, 0, 6), nop_i()),
        (0x110, 0x00, nop_m(), addl(6, 0x77, 0), nop_i()),
        (0x120, 0x00, ld8_c_nc(6, 3), nop_i(), nop_i()),
        (0x130, 0x00, ld8_a(7, 3), nop_i(), nop_i()),
        (0x140, 0x00, nop_m(), addl(7, 0x66, 0), nop_i()),
        (0x150, 0x00, ld8_sa(7, 5), nop_i(), nop_i()),
        (0x160, 0x00, nop_m(), adds(12, 0, 7), nop_i()),
        (0x170, 0x00, nop_m(), nop_i(), nop_i()),
        (0x180, 0x00, nop_m(), nop_i(), nop_i()),
        (0x190, 0x00, nop_m(), addl(7, 0x88, 0), nop_i()),
        (0x1a0, 0x00, ld8_c_nc(7, 3), nop_i(), nop_i()),
        (0x1b0, 0x10, nop_m(), nop_i(), br_cond(0x1b0, 0x1b0)),
    ],
    {
        "ip": 0x1b0,
        "exception": IA64_EXCP_NONE,
        "r6": 0x1122334455667788,
        "r7": 0x1122334455667788,
        "r8": 0xddeeff0011223344,
        "r8_nat": 0,
        "r11": 0x55,
        "r12": 0x66,
        "r12_nat": 0,
    },
)

test_firmware_unaligned_virtual_load_assist = require_registers(
    "firmware_unaligned_virtual_load_assist",
    [
        (0x10, *movl_mlx(20, 0x1122334455667788)),
        (0x20, *movl_mlx(21, 0x99aabbccddeeff00)),
        (0x30, 0x00, addl(3, 0x300, 0), nop_i(), nop_i()),
        (0x40, 0x0a, st8(3, 20), adds(3, 8, 3), nop_i()),
        (0x50, 0x00, st8(3, 21), nop_i(), nop_i()),
        *dtr_setup_bundles(0x60, 0xe000000000000304, 0x304),
        (0xc0, *movl_mlx(5, 0xe000000000000304)),
        (0xd0, 0x00, addl(2, 0x108000, 0), nop_i(), nop_i()),
        (0xe0, 0x00, mov_m_gr_cr(2, 2), nop_i(), nop_i()),
        (0xf0, 0x00, ssm((1 << 17) | (1 << 13) | (1 << 3)),
         nop_i(), nop_i()),
        (0x100, 0x00, ld8_s(22, 5), nop_i(), nop_i()),
        (0x110, 0x00, nop_m(), adds(23, 1, 0), nop_i()),
        (0x120, 0x10, nop_m(), nop_i(), br_cond(0x120, 0x120)),
    ],
    {
        "ip": 0x120,
        "exception": IA64_EXCP_NONE,
        "r22": 0xddeeff0011223344,
        "r22_nat": 0,
        "r23": 1,
    },
)

test_speculative_unaligned_defers = require_registers(
    "speculative_unaligned_defers",
    [
        (0x10, 0x00, nop_m(), addl(3, 0x104, 0), nop_i()),
        (0x20, 0x00, sum_um(0x8), nop_i(), nop_i()),
        (0x30, 0x00, ld8_s(4, 3), nop_i(), nop_i()),
        (0x40, 0x00, nop_m(), nop_i(), nop_i()),
        (0x50, 0x00, nop_m(), nop_i(), nop_i()),
        (0x60, 0x10, nop_m(), nop_i(), br_cond(0x60, 0x60)),
    ],
    {"ip": 0x60, "r4_nat": 1},
)

_br_ctop_spec_data = [i + 1 for i in range(131)] + [0]
test_br_ctop_long_speculative_load_pipeline = require_registers(
    "br_ctop_long_speculative_load_pipeline", [
        *dtr_setup_bundles(0x10, HIGH_TR_BASE, 0x400000),
        (0x70, *movl_mlx(20, HIGH_TR_BASE + 0x8000)),
        (0x80, *movl_mlx(5, 130)),
        (0x90, 0x01, nop_m(), nop_i(), nop_i()),
        (0xa0, 0x00, alloc_m(9, 32, 32, 4, 0),
         mov_i_imm_ar(66, 1), mov_lc_gr(5)),
        (0xb0, *movl_mlx(19, (1 << 13) | (1 << 17))),
        (0xc0, 0x08, mov_gr_psr_full(19), srlz_d(), nop_i()),
        (0xd0, 0x00, nop_m(), mov_pr_rot_imm(0x10000), nop_i()),
        (0xe0, 0x00, ld8_s_postinc(32, 20, 8, qp=16), nop_i(), nop_i()),
        (0xf0, 0x10, nop_m(), adds(8, 0, 34, qp=18),
         br_ctop_many(0xf0, 0xe0)),
        (0x100, 0x10, nop_m(), nop_i(), br_cond(0x100, 0x100)),
        *(raw_bundle(0x408000 + i * 8, _br_ctop_spec_data[i],
                     _br_ctop_spec_data[i + 1])
          for i in range(0, len(_br_ctop_spec_data), 2)),
    ], {"exception": IA64_EXCP_NONE, "ip": 0x100,
        "r8": 129, "r20": HIGH_TR_BASE + 0x8418}, entry=0x10)

GROUP = 'memory-nat'
# A control-speculative load whose page has no ED bit and that runs with
# instruction translation off (PSR.it == 0, as the OS loaders and early kernel
# do) must still defer a deferrable TLB fault when the matching DCR mask bit is
# set: DCR-based deferral is independent of PSR.it and of the code page's ED
# bit.  Without this, XP's SETUPLDR/NTOSKRNL ld.s over a NaTVal pointer faults
# instead of deferring, cascading into a break loop.  The pre-existing
# speculative_load_defers_psr_ed case sets PSR.it and ED as well, so it did not
# exercise the DCR-only path.
test_speculative_load_defers_via_dcr_without_ed = require_registers(
    "speculative_load_defers_via_dcr_without_ed", [
        (0x10, *movl_mlx(20, IA64_DCR_DM)),
        (0x20, 0x00, mov_m_gr_cr(20, 0), nop_i(), nop_i()),
        (0x30, *movl_mlx(2, 0xa000000100020000)),
        (0x40, *movl_mlx(19, IA64_PSR_IC | IA64_PSR_DT)),
        (0x50, 0x00, mov_gr_psr_full(19), nop_i(), nop_i()),
        (0x60, 0x00, srlz_d(), nop_i(), nop_i()),
        (0x70, 0x00, ld8_s(4, 2), nop_i(), nop_i()),
        (0x80, 0x10, nop_m(), nop_i(), br_cond(0x80, 0x80)),
    ], {
        "ip": 0x80,
        "exception": IA64_EXCP_NONE,
        "r4_nat": 1,
    }, entry=0x10)


# --- Speculative stacked-register NaT survival sweeps -----------------------
#
# A control-speculative NaT in a stacked register must survive being spilled
# to and refilled from the backing store.  BSPSTORE is swept across all 63
# slot positions of a 512-byte NaT collection group, so every alignment of
# the NaT bit inside AR.RNAT and of the collection-word boundary relative to
# the spill/reload window is exercised.  The first variant forces the
# physical-file wrap with two deep calls; the second interposes the
# KiFlushRse shape (cover; flushrs; loadrs; rfi) inside a break handler,
# the exact sequence whose AR.RNAT handling caused the 2003 SMP
# c00002c9 crashes (dbb19b4, 2a2e9aa).  Expectations are
# implementation-neutral: they assert only that chk.s still branches.

def _spec_nat_call_return_case(collect_bit):
    bspstore = 0x100000 + collect_bit * 8
    name = f"spec_stacked_nat_survives_call_return_bit_{collect_bit:02d}"
    return require_registers(name, [
        (0x10, *movl_mlx(2, bspstore)),
        (0x20, 0x01, mov_m_gr_ar(2, 18), nop_i(),
         nop_i()),
        (0x30, 0x00, alloc_m(43, 20, 13, 0, 0), nop_i(),
         nop_i()),
        (0x40, *movl_mlx(9, 1)),
        (0x50, 0x00, mov_m_gr_ar(9, 36), addl(3, 0x300, 0),
         nop_i()),
        (0x60, 0x08, ld8_fill_postinc(40, 3, 0), nop_i(),
         nop_i()),
        (0x70, 0x00, ld8_s_postinc(32, 40, 8), nop_i(),
         nop_i()),
        (0x80, 0x10, nop_m(), nop_i(),
         br_call(0, 0x80, 0x200)),
        (0x90, 0x10, nop_m(), nop_i(),
         br_call(0, 0x90, 0x200)),
        (0xa0, 0x08, nop_m(), chk_s_m(32, 0xa0, 0xc0),
         nop_i()),
        (0xb0, 0x10, nop_m(), adds(8, 1, 0),
         br_cond(0xb0, 0xd0)),
        (0xc0, 0x10, nop_m(), adds(10, 1, 0),
         br_cond(0xc0, 0xd0)),
        (0xd0, 0x10, nop_m(), nop_i(),
         br_cond(0xd0, 0xd0)),
        (0x200, 0x00, alloc_m(34, 96, 88, 0, 0), nop_i(),
         nop_i()),
        (0x210, 0x00, mov_m_gr_ar(34, 64), nop_i(),
         nop_i()),
        (0x220, 0x10, nop_m(), nop_i(),
         br_ret(0)),
        (0x300, 0x00, 0x400, 0,
         0),
    ], {
        "ip": 0xd0,
        "exception": IA64_EXCP_NONE,
        "r8": 0,
        "r10": 1,
        "r32_nat": 1,
    }, entry=0x10)


def _spec_nat_interrupt_flush_case(collect_bit):
    bspstore = 0x100000 + collect_bit * 8
    name = f"spec_stacked_nat_survives_interrupt_flush_bit_{collect_bit:02d}"
    return require_registers(name, [
        (0x10, *movl_mlx(2, bspstore)),
        (0x20, 0x01, mov_m_gr_ar(2, 18), nop_i(),
         nop_i()),
        (0x30, *movl_mlx(19, IA64_PSR_IC)),
        (0x40, 0x10, mov_gr_psr_full(19), nop_i(),
         br_cond(0x40, 0x50)),
        (0x50, 0x00, alloc_m(43, 20, 13, 0, 0), nop_i(),
         nop_i()),
        (0x60, 0x00, addl(3, 0x300, 0), nop_i(),
         nop_i()),
        (0x70, 0x08, ld8_fill_postinc(40, 3, 0), nop_i(),
         nop_i()),
        (0x80, 0x00, ld8_s_postinc(32, 40, 8), nop_i(),
         nop_i()),
        (0x90, 0x10, nop_m(), nop_i(),
         br_call(0, 0x90, 0x200)),
        (0xa0, 0x08, nop_m(), chk_s_m(32, 0xa0, 0xc0),
         nop_i()),
        (0xb0, 0x10, nop_m(), adds(8, 1, 0),
         br_cond(0xb0, 0xd0)),
        (0xc0, 0x10, nop_m(), adds(10, 1, 0),
         br_cond(0xc0, 0xd0)),
        (0xd0, 0x10, nop_m(), nop_i(),
         br_cond(0xd0, 0xd0)),
        (0x200, 0x00, alloc_m(34, 96, 88, 0, 0), nop_i(),
         nop_i()),
        (0x210, 0x00, break_m(0x42), nop_i(),
         nop_i()),
        (0x220, 0x10, nop_m(), nop_i(),
         br_ret(0)),
        (IA64_BREAK_VECTOR, 0x18, nop_m(), nop_m(),
         cover_b()),
        (IA64_BREAK_VECTOR + 0x10, 0x00, flushrs_enc(), nop_i(),
         nop_i()),
        (IA64_BREAK_VECTOR + 0x20, 0x00, loadrs_enc(), nop_i(),
         nop_i()),
        (IA64_BREAK_VECTOR + 0x30, *movl_mlx(20, 0x220)),
        (IA64_BREAK_VECTOR + 0x40, 0x00,
         mov_m_gr_cr(20, 19), nop_i(), nop_i()),
        (IA64_BREAK_VECTOR + 0x50, 0x10, nop_m(), nop_i(),
         rfi_b()),
        (0x300, 0x00, 0x400, 0,
         0),
    ], {
        "ip": 0xd0,
        "exception": IA64_EXCP_NONE,
        "r8": 0,
        "r10": 1,
        "r32_nat": 1,
    }, entry=0x10)


_SPEC_NAT_SWEEP_NAMES = []
for _bit in range(63):
    for _builder in (_spec_nat_call_return_case, _spec_nat_interrupt_flush_case):
        _case = _builder(_bit)
        globals()["test_" + _case.name] = _case
        _SPEC_NAT_SWEEP_NAMES.append(_case.name)


# A counted self-loop re-executes its bundle through a TCG back edge inside
# one translation block.  A base register whose NaT was known clear *above*
# the loop can be made NaT by the loop body itself, so the body's
# consumption check must not be elided on the strength of the pre-loop
# fact: iteration 2 consumes the NaT the body created in iteration 1.
test_self_loop_nat_consumption_not_elided = require_exception(
    "self_loop_nat_consumption_not_elided", [
        (0x10, 0x00, addl(5, 0x300, 0), adds(8, 1, 0),
         nop_i()),
        (0x20, *movl_mlx(3, 1 << 61)),
        (0x30, 0x02, nop_m(), mov_lc_gr(8),
         nop_i()),
        # ld8 consumes r5 as a base; ld8.s from an unimplemented address
        # NaTs r5 for the next iteration.
        (0x40, 0x18, ld8(7, 5), ld8_s(5, 3),
         br_cloop(0x40, 0x40)),
    ], IA64_EXCP_NAT_CONSUMPTION, fault_ip=0x40, entry=0x10)

CASE_NAMES = tuple(_SPEC_NAT_SWEEP_NAMES) + (

    'self_loop_nat_consumption_not_elided',


    'alat_reloading_register_does_not_leave_duplicate',
    'alloc_clears_destination_nat',
    'br_ctop_long_speculative_load_pipeline',
    'bsw_restores_banked_nat',
    'chk_a_clr_removes_entry',
    'chk_a_m_branches_on_miss',
    'chk_a_nc_m_decode',
    'chk_s_i_long_branch_on_stacked_nat',
    'chk_s_m_branches_on_nat',
    'cloop_zero_st1_clears_cross_page_range',
    'cloop_zero_st1_invalidates_alat_range',
    'cmp8xchg16_acq_stores_pair',
    'cmp8xchg16_madison_illegal_operation',
    'cmp8xchg16_natpage_consumption',
    'cmp8xchg16_rel_mismatch_keeps_pair',
    'cmp8xchg16_uc_unsupported_data_reference',
    'cmp8xchg16_unaligned',
    'cmpxchg4_full_ar_ccv_compare',
    'cmpxchg4_repeated_word_updates',
    'cmpxchg4_result_base_alias_failure_keeps_alat',
    'cmpxchg4_result_base_alias_success_invalidates_alat',
    'cmpxchg4_uses_ar_ccv',
    'data_big_endian_cmpxchg4',
    'data_big_endian_load_store',
    'fc_invalidates_advanced_load',
    'fc_nat_source_consumes_non_access',
    'fetchadd4_nat_base_sets_read_write_isr',
    'fetchadd4_result_base_alias_invalidates_alat',
    'fetchadd4_unaligned_sets_read_write_isr',
    'firmware_unaligned_load_assist',
    'firmware_unaligned_speculative_load_assist',
    'firmware_unaligned_store_assist',
    'firmware_unaligned_virtual_load_assist',
    'integer_compare_nat_source_rules',
    'integer_nat_propagates_and_clears',
    'integer_postinc_imm9_decode',
    'invala_clears_all_alat_entries',
    'invala_e_gr_invalidates_selected_register',
    'ld16_acq_hint_decode',
    'ld16_loads_gr_and_csd',
    'ld16_madison_illegal_operation',
    'ld16_uc_unsupported_data_reference',
    'ld1_acq_decode',
    'ld1_postinc_decode',
    'ld1_reg_postinc_decode',
    'ld1_reg_postinc_uses_old_increment',
    'ld1_sa_postinc_decode',
    'ld2_bias_st2_raw_large_frame_sequence',
    'ld4_c_clr_hit_clears_entry',
    'ld4_bias_decode',
    'ld8_a_uc_zeroes_target_and_skips_alat',
    'ld8_c_clr_address_mismatch_reloads',
    'ld8_c_clr_hit_clears_entry',
    'ld8_c_nc_address_mismatch_reloads',
    'ld8_c_nc_hit_consumes_nat_base',
    'ld8_c_nc_hit_preserves_target',
    'ld8_fill_restores_unat_bit',
    'ld8_fill_st8_spill_postinc_decode',
    'ld8_nt1_postinc_decode',
    'ld8_s_d2_hint_decode',
    'ld8_s_uc_defers',
    'ld8_sa_failure_invalidates_old_entry',
    'ld_imm_postinc_same_target_illegal',
    'ld_postinc_same_target_predicated_false',
    'ld_reg_postinc_same_target_illegal',
    'lfetch_decode',
    'load_postinc_x6_18_reserved_illegal_operation',
    'load_postinc_x6_19_reserved_illegal_operation',
    'load_postinc_x6_1a_reserved_illegal_operation',
    'load_reg_postinc_x6_18_reserved_illegal_operation',
    'load_reg_postinc_x6_19_reserved_illegal_operation',
    'load_reg_postinc_x6_1a_reserved_illegal_operation',
    'load_x6_18_reserved_illegal_operation',
    'load_x6_19_reserved_illegal_operation',
    'load_x6_1a_reserved_illegal_operation',
    'memory_cache_hints_decode',
    'memory_order_completers_decode',
    'mlx_chk_a_clr_nop_x_decode',
    'mov_ar_nat_source_consumes',
    'mov_br_nat_source_consumes',
    'mov_cpuid_nat_index_consumes',
    'mov_cr_nat_source_consumes',
    'mov_crgr_clears_stale_nat',
    'mov_pkr_nat_index_consumes',
    'mov_pmc_nat_value_consumes',
    'mov_pr_nat_source_consumes',
    'mov_psr_nat_source_consumes',
    'mov_rr_nat_index_consumes',
    'mov_um_nat_source_consumes',
    'nat_consumption_ic0_preserves_ifa',
    'nat_consumption_sets_ifa_isr',
    'nat_store_data_consumption_is_access',
    'normal_load_clears_stale_nat',
    'normal_load_consumes_nat_base',
    'pshl_nat_propagates',
    'pshr_nat_propagates',
    'semaphore_ops_clear_result_nat',
    'semaphore_ops_invalidate_advanced_loads',
    'simd_helper_nat_propagates',
    'speculative_load_defers_nat_base',
    'speculative_load_defers_via_dcr_without_ed',
    'speculative_load_defers_psr_ed',
    'speculative_load_handler_psr_ed_defers_retry',
    'speculative_load_no_recovery_tlb_miss_faults',
    'speculative_recovery_unaligned_defers',
    'speculative_unimplemented_physical_unaligned_defers',
    'speculative_unaligned_defers',
    'speculative_stacked_nat_survives_backing_store_switch',
    'speculative_unaligned_no_recovery_faults',
    'st16_madison_illegal_operation',
    'st16_rel_stores_gr_and_csd',
    'st16_stores_gr_and_csd',
    'st16_uc_unsupported_data_reference',
    'st1_postinc_decode',
    'st4_variants_preserve_adjacent_halfword',
    'st8_postinc_same_base_value_uses_old_base',
    'st8_spill_updates_unat_bit',
    'store_invalidates_advanced_load',
    'store_postinc_x6_38_reserved_illegal_operation',
    'store_postinc_x6_39_reserved_illegal_operation',
    'store_postinc_x6_3a_reserved_illegal_operation',
    'store_x6_38_reserved_illegal_operation',
    'store_x6_39_reserved_illegal_operation',
    'store_x6_3a_reserved_illegal_operation',
    'tbit_nat_source_rules',
    'tnat_nz_and_ignored_bits_decode',
    'tnat_nz_or_decode',
    'tnat_unc_same_pred_pred_false_illegal',
    'unimplemented_physical_load_faults',
    'unimplemented_virtual_load_faults_merced',
    'unimplemented_virtual_load_translates_madison',
    'unimplemented_physical_precludes_unaligned',
    'ws2003_cmd646_unaligned_check_load_sets_ed',
    'xchg4_decode',
    'xchg4_result_base_alias_invalidates_alat',
    'zero_alat_check_load_always_reloads',
    'zero_alat_chk_a_always_branches',
)

CASE_METADATA = {
    'tnat_nz_and_ignored_bits_decode': CaseMetadata(observation=CaseObservation.TNAT_PREDICATE),
    'tnat_nz_or_decode': CaseMetadata(observation=CaseObservation.TNAT_PREDICATE),
    'tnat_unc_same_pred_pred_false_illegal': CaseMetadata(observation=CaseObservation.TNAT_PREDICATE),
}

CASE_ALIASES = {
}

CASES = bind_cases(GROUP, CASE_NAMES, globals(),
                   aliases=CASE_ALIASES,
                   metadata=CASE_METADATA)
