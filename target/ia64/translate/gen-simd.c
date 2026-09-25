/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * IA-64 packed integer and multimedia instruction generation.
 */

#include "qemu/osdep.h"

#include "exec/helper-proto.h"
#include "exec/helper-gen.h"
#include "tcg/tcg-op.h"

#include "target/ia64/translate/translate.h"

static void ia64_gen_saturate_signed_i64(TCGv_i64 value, int bits)
{
    int64_t min_value = -(1LL << (bits - 1));
    int64_t max_value = (1LL << (bits - 1)) - 1;
    TCGLabel *ge_min = gen_new_label();
    TCGLabel *le_max = gen_new_label();

    tcg_gen_brcondi_i64(TCG_COND_GE, value, min_value, ge_min);
    tcg_gen_movi_i64(value, min_value);
    gen_set_label(ge_min);
    tcg_gen_brcondi_i64(TCG_COND_LE, value, max_value, le_max);
    tcg_gen_movi_i64(value, max_value);
    gen_set_label(le_max);
}

typedef struct IA64SIMDLanePlan {
    unsigned bits;
    unsigned count;
    uint64_t mask;
} IA64SIMDLanePlan;

static IA64SIMDLanePlan ia64_simd_lane_plan(unsigned bits)
{
    return (IA64SIMDLanePlan) {
        .bits = bits,
        .count = 64 / bits,
        .mask = (1ULL << bits) - 1,
    };
}

static void ia64_simd_sign_extend(TCGv_i64 lane, unsigned bits)
{
    switch (bits) {
    case 8:
        tcg_gen_ext8s_i64(lane, lane);
        break;
    case 16:
        tcg_gen_ext16s_i64(lane, lane);
        break;
    case 32:
        tcg_gen_ext32s_i64(lane, lane);
        break;
    default:
        g_assert_not_reached();
    }
}

static TCGv_i64 ia64_simd_extract_lane(TCGv_i64 source,
                                       const IA64SIMDLanePlan *plan,
                                       unsigned lane, bool sign_extend)
{
    TCGv_i64 value = tcg_temp_new_i64();

    tcg_gen_extract_i64(value, source, lane * plan->bits, plan->bits);
    if (sign_extend) {
        ia64_simd_sign_extend(value, plan->bits);
    }
    return value;
}

static void ia64_simd_insert_lane(TCGv_i64 result, TCGv_i64 value,
                                  const IA64SIMDLanePlan *plan,
                                  unsigned lane)
{
    tcg_gen_andi_i64(value, value, plan->mask);
    tcg_gen_shli_i64(value, value, lane * plan->bits);
    tcg_gen_or_i64(result, result, value);
}

/*
 * Write a SIMD result through the normal GR path.  An r0 destination is
 * ignored here (matching every other GR writer in this translator); the
 * old env-writing helpers stored through r0's slot in env->gr and
 * corrupted the r0 global.  The stacked-register dirty note is taken
 * care of by the ia64_gen_gr_nat_from_* call that every site emits
 * afterwards.
 */
static void ia64_simd_write_gr(uint8_t reg, TCGv_i64 value)
{
    if (reg != 0) {
        tcg_gen_mov_i64(cpu_gr[reg], value);
    }
}

static void ia64_gen_pshr(const Ia64Instruction *insn, int lane_bits,
                          bool unsigned_shift)
{
    const IA64SimdOperands *op = &insn->operands.simd;
    const IA64SIMDLanePlan plan = ia64_simd_lane_plan(lane_bits);
    TCGv_i64 result;
    TCGv_i64 count;
    TCGv_i64 clamped_count;

    if (op->destination == 0) {
        return;
    }

    result = tcg_temp_new_i64();
    count = tcg_temp_new_i64();
    clamped_count = tcg_temp_new_i64();

    if (op->immediate >= 0) {
        tcg_gen_movi_i64(count, op->immediate);
    } else {
        tcg_gen_mov_i64(count, ia64_gr_src(op->source1));
    }
    tcg_gen_movcond_i64(TCG_COND_GTU, clamped_count, count,
                        tcg_constant_i64(lane_bits),
                        tcg_constant_i64(lane_bits), count);

    tcg_gen_movi_i64(result, 0);
    for (int i = 0; i < plan.count; i++) {
        TCGv_i64 lane = ia64_simd_extract_lane(
            ia64_gr_src(op->source2), &plan, i, !unsigned_shift);

        if (!unsigned_shift) {
            tcg_gen_sar_i64(lane, lane, clamped_count);
        } else {
            tcg_gen_shr_i64(lane, lane, clamped_count);
        }
        ia64_simd_insert_lane(result, lane, &plan, i);
    }

    tcg_gen_mov_i64(cpu_gr[op->destination], result);
    if (op->immediate >= 0) {
        ia64_gen_gr_nat_from_1(op->destination, op->source2);
    } else {
        ia64_gen_gr_nat_from_2(op->destination, op->source1, op->source2);
    }
}

static void ia64_gen_pshl(const Ia64Instruction *insn, int lane_bits)
{
    const IA64SimdOperands *op = &insn->operands.simd;
    const IA64SIMDLanePlan plan = ia64_simd_lane_plan(lane_bits);
    TCGv_i64 result;
    TCGv_i64 count;
    TCGv_i64 clamped_count;

    if (op->destination == 0) {
        return;
    }

    result = tcg_temp_new_i64();
    count = tcg_temp_new_i64();
    clamped_count = tcg_temp_new_i64();

    if (op->immediate >= 0) {
        tcg_gen_movi_i64(count, op->immediate);
    } else {
        tcg_gen_mov_i64(count, ia64_gr_src(op->source2));
    }
    tcg_gen_movcond_i64(TCG_COND_GTU, clamped_count, count,
                        tcg_constant_i64(lane_bits),
                        tcg_constant_i64(lane_bits), count);

    tcg_gen_movi_i64(result, 0);
    for (int i = 0; i < plan.count; i++) {
        TCGv_i64 lane = ia64_simd_extract_lane(
            ia64_gr_src(op->source1), &plan, i, false);

        tcg_gen_shl_i64(lane, lane, clamped_count);
        ia64_simd_insert_lane(result, lane, &plan, i);
    }

    tcg_gen_mov_i64(cpu_gr[op->destination], result);
    if (op->immediate >= 0) {
        ia64_gen_gr_nat_from_1(op->destination, op->source1);
    } else {
        ia64_gen_gr_nat_from_2(op->destination, op->source1, op->source2);
    }
}


IA64GenResult ia64_gen_simd(DisasContext *ctx,
                            const Ia64Instruction *insn)
{
    const IA64SimdOperands *op = &insn->operands.simd;
    TCGv_i64 helper_result;

    (void)ctx;

    switch (insn->opcode) {
    case IA64_OP_PADD1:
    case IA64_OP_PADD2:
    case IA64_OP_PADD4:
    case IA64_OP_PSUB1:
    case IA64_OP_PSUB2:
    case IA64_OP_PSUB4: {
        IA64SIMDLanePlan plan;

        if (insn->opcode == IA64_OP_PADD1 || insn->opcode == IA64_OP_PSUB1) {
            plan = ia64_simd_lane_plan(8);
        } else if (insn->opcode == IA64_OP_PADD2 ||
                   insn->opcode == IA64_OP_PSUB2) {
            plan = ia64_simd_lane_plan(16);
        } else {
            plan = ia64_simd_lane_plan(32);
        }
        bool is_sub = (insn->opcode == IA64_OP_PSUB1 ||
                       insn->opcode == IA64_OP_PSUB2 ||
                       insn->opcode == IA64_OP_PSUB4);
        const unsigned saturation = op->immediate & 3;
        TCGv_i64 result = tcg_temp_new_i64();

        tcg_gen_movi_i64(result, 0);
        for (int i = 0; i < plan.count; ++i) {
            TCGv_i64 lane_a = ia64_simd_extract_lane(
                ia64_gr_src(op->source1), &plan, i,
                plan.bits < 32 && saturation == 1);
            TCGv_i64 lane_b = ia64_simd_extract_lane(
                ia64_gr_src(op->source2), &plan, i,
                plan.bits < 32 &&
                (saturation == 1 || saturation == 3));
            TCGv_i64 lane_res = tcg_temp_new_i64();

            if (is_sub) {
                tcg_gen_sub_i64(lane_res, lane_a, lane_b);
            } else {
                tcg_gen_add_i64(lane_res, lane_a, lane_b);
            }
            if (saturation != 0 && plan.bits < 32) {
                int64_t min_value = 0;
                int64_t max_value = plan.mask;
                TCGLabel *ge_min = gen_new_label();
                TCGLabel *le_max = gen_new_label();

                if (saturation == 1) {
                    min_value = -(1LL << (plan.bits - 1));
                    max_value = (1LL << (plan.bits - 1)) - 1;
                }

                tcg_gen_brcondi_i64(TCG_COND_GE, lane_res, min_value,
                                    ge_min);
                tcg_gen_movi_i64(lane_res, min_value);
                gen_set_label(ge_min);
                tcg_gen_brcondi_i64(TCG_COND_LE, lane_res, max_value,
                                    le_max);
                tcg_gen_movi_i64(lane_res, max_value);
                gen_set_label(le_max);
            }
            ia64_simd_insert_lane(result, lane_res, &plan, i);
        }
        tcg_gen_mov_i64(cpu_gr[op->destination], result);
        ia64_gen_gr_nat_from_2(op->destination, op->source1, op->source2);
        break;
    }
    case IA64_OP_PSHLADD2: {
        const IA64SIMDLanePlan plan = ia64_simd_lane_plan(16);
        TCGv_i64 result = tcg_temp_new_i64();

        tcg_gen_movi_i64(result, 0);
        for (int i = 0; i < plan.count; ++i) {
            TCGv_i64 lane_a = ia64_simd_extract_lane(
                ia64_gr_src(op->source1), &plan, i, true);
            TCGv_i64 lane_b = ia64_simd_extract_lane(
                ia64_gr_src(op->source2), &plan, i, true);
            TCGv_i64 lane_res = tcg_temp_new_i64();
            TCGv_i64 shifted = tcg_temp_new_i64();
            TCGv_i64 shift_sat = tcg_temp_new_i64();

            tcg_gen_shli_i64(shifted, lane_a, op->immediate);
            tcg_gen_mov_i64(shift_sat, shifted);
            ia64_gen_saturate_signed_i64(shift_sat, 16);
            tcg_gen_add_i64(lane_res, shifted, lane_b);
            ia64_gen_saturate_signed_i64(lane_res, 16);
            /* SDM Vol 3 pshladd: a saturated shift is not added to y. */
            tcg_gen_movcond_i64(TCG_COND_NE, lane_res, shift_sat, shifted,
                                shift_sat, lane_res);
            ia64_simd_insert_lane(result, lane_res, &plan, i);
        }
        tcg_gen_mov_i64(cpu_gr[op->destination], result);
        ia64_gen_gr_nat_from_2(op->destination, op->source1, op->source2);
        break;
    }
    case IA64_OP_PSHRADD2: {
        const IA64SIMDLanePlan plan = ia64_simd_lane_plan(16);
        TCGv_i64 result = tcg_temp_new_i64();

        tcg_gen_movi_i64(result, 0);
        for (int i = 0; i < plan.count; ++i) {
            TCGv_i64 lane_a = ia64_simd_extract_lane(
                ia64_gr_src(op->source1), &plan, i, true);
            TCGv_i64 lane_b = ia64_simd_extract_lane(
                ia64_gr_src(op->source2), &plan, i, true);
            TCGv_i64 lane_res = tcg_temp_new_i64();

            tcg_gen_sari_i64(lane_res, lane_a, op->immediate);
            tcg_gen_add_i64(lane_res, lane_res, lane_b);
            ia64_gen_saturate_signed_i64(lane_res, 16);
            ia64_simd_insert_lane(result, lane_res, &plan, i);
        }
        tcg_gen_mov_i64(cpu_gr[op->destination], result);
        ia64_gen_gr_nat_from_2(op->destination, op->source1, op->source2);
        break;
    }
    case IA64_OP_PSHR2:
        ia64_gen_pshr(insn, 16, false);
        break;
    case IA64_OP_PSHR2_U:
        ia64_gen_pshr(insn, 16, true);
        break;
    case IA64_OP_PSHR4:
        ia64_gen_pshr(insn, 32, false);
        break;
    case IA64_OP_PSHR4_U:
        ia64_gen_pshr(insn, 32, true);
        break;
    case IA64_OP_PAVG1:
    case IA64_OP_PAVG2:
    case IA64_OP_PAVGSUB1:
    case IA64_OP_PAVGSUB2: {
        uint32_t sel;

        if (insn->opcode == IA64_OP_PAVG1) {
            sel = op->immediate ? 4 : 0;
        } else if (insn->opcode == IA64_OP_PAVG2) {
            sel = op->immediate ? 5 : 1;
        } else if (insn->opcode == IA64_OP_PAVGSUB1) {
            sel = 2;
        } else {
            sel = 3;
        }
        helper_result = tcg_temp_new_i64();
        gen_helper_simd_pavg(helper_result, tcg_constant_i32(sel),
                               ia64_gr_src(op->source1),
                               ia64_gr_src(op->source2));
        ia64_simd_write_gr(op->destination, helper_result);
        ia64_gen_gr_nat_from_2(op->destination, op->source1, op->source2);
        break;
    }
    case IA64_OP_PCMP1_EQ:
    case IA64_OP_PCMP1_GT:
    case IA64_OP_PCMP2_EQ:
    case IA64_OP_PCMP2_GT:
    case IA64_OP_PCMP4_EQ:
    case IA64_OP_PCMP4_GT: {
        uint32_t sel;

        if (insn->opcode == IA64_OP_PCMP1_EQ) {
            sel = 0;
        } else if (insn->opcode == IA64_OP_PCMP1_GT) {
            sel = 1;
        } else if (insn->opcode == IA64_OP_PCMP2_EQ) {
            sel = 2;
        } else if (insn->opcode == IA64_OP_PCMP2_GT) {
            sel = 3;
        } else if (insn->opcode == IA64_OP_PCMP4_EQ) {
            sel = 4;
        } else {
            sel = 5;
        }
        helper_result = tcg_temp_new_i64();
        gen_helper_simd_pcmp(helper_result, tcg_constant_i32(sel),
                               ia64_gr_src(op->source1),
                               ia64_gr_src(op->source2));
        ia64_simd_write_gr(op->destination, helper_result);
        ia64_gen_gr_nat_from_2(op->destination, op->source1, op->source2);
        break;
    }
    case IA64_OP_PMAX1_U:
    case IA64_OP_PMAX2:
    case IA64_OP_PMIN1_U:
    case IA64_OP_PMIN2: {
        uint32_t sel;

        if (insn->opcode == IA64_OP_PMAX1_U) {
            sel = 0;
        } else if (insn->opcode == IA64_OP_PMIN1_U) {
            sel = 1;
        } else if (insn->opcode == IA64_OP_PMAX2) {
            sel = 2;
        } else {
            sel = 3;
        }
        helper_result = tcg_temp_new_i64();
        gen_helper_simd_pminmax(helper_result, tcg_constant_i32(sel),
                               ia64_gr_src(op->source1),
                               ia64_gr_src(op->source2));
        ia64_simd_write_gr(op->destination, helper_result);
        ia64_gen_gr_nat_from_2(op->destination, op->source1, op->source2);
        break;
    }
    case IA64_OP_PMPY2_L:
    case IA64_OP_PMPY2_R:
    case IA64_OP_PMPYSH2:
    case IA64_OP_PMPYSH2_U: {
        uint32_t sel;

        if (insn->opcode == IA64_OP_PMPY2_L) {
            sel = 0;
        } else if (insn->opcode == IA64_OP_PMPY2_R) {
            sel = 1;
        } else if (insn->opcode == IA64_OP_PMPYSH2) {
            sel = 2;
        } else {
            sel = 3;
        }
        helper_result = tcg_temp_new_i64();
        gen_helper_simd_pmpy(helper_result, tcg_constant_i32(sel),
                             ia64_gr_src(op->source1),
                             ia64_gr_src(op->source2),
                             tcg_constant_i32(op->immediate));
        ia64_simd_write_gr(op->destination, helper_result);
        ia64_gen_gr_nat_from_2(op->destination, op->source1, op->source2);
        break;
    }
    case IA64_OP_PSHL2:
        ia64_gen_pshl(insn, 16);
        break;
    case IA64_OP_PSHL4:
        ia64_gen_pshl(insn, 32);
        break;
    case IA64_OP_PSAD1:
        helper_result = tcg_temp_new_i64();
        gen_helper_simd_psad1(helper_result,
                              ia64_gr_src(op->source1),
                              ia64_gr_src(op->source2));
        ia64_simd_write_gr(op->destination, helper_result);
        ia64_gen_gr_nat_from_2(op->destination, op->source1, op->source2);
        break;
    case IA64_OP_MUX1:
    case IA64_OP_MUX2:
        helper_result = tcg_temp_new_i64();
        gen_helper_simd_mux(helper_result,
                            tcg_constant_i32(
                                insn->opcode == IA64_OP_MUX1 ? 0 : 1),
                            ia64_gr_src(op->source1),
                            tcg_constant_i32(op->immediate));
        ia64_simd_write_gr(op->destination, helper_result);
        ia64_gen_gr_nat_from_1(op->destination, op->source1);
        break;
    case IA64_OP_MIX1_L:
    case IA64_OP_MIX1_R:
    case IA64_OP_MIX2_L:
    case IA64_OP_MIX2_R:
    case IA64_OP_MIX4_L:
    case IA64_OP_MIX4_R: {
        uint32_t sel;

        if (insn->opcode == IA64_OP_MIX1_L) {
            sel = 0;
        } else if (insn->opcode == IA64_OP_MIX1_R) {
            sel = 1;
        } else if (insn->opcode == IA64_OP_MIX2_L) {
            sel = 2;
        } else if (insn->opcode == IA64_OP_MIX2_R) {
            sel = 3;
        } else if (insn->opcode == IA64_OP_MIX4_L) {
            sel = 4;
        } else {
            sel = 5;
        }
        helper_result = tcg_temp_new_i64();
        gen_helper_simd_mix(helper_result, tcg_constant_i32(sel),
                               ia64_gr_src(op->source1),
                               ia64_gr_src(op->source2));
        ia64_simd_write_gr(op->destination, helper_result);
        ia64_gen_gr_nat_from_2(op->destination, op->source1, op->source2);
        break;
    }
    case IA64_OP_PACK2_SSS:
    case IA64_OP_PACK2_USS:
    case IA64_OP_PACK4_SSS: {
        uint32_t sel;

        if (insn->opcode == IA64_OP_PACK2_SSS) {
            sel = 0;
        } else if (insn->opcode == IA64_OP_PACK2_USS) {
            sel = 1;
        } else {
            sel = 2;
        }
        helper_result = tcg_temp_new_i64();
        gen_helper_simd_pack(helper_result, tcg_constant_i32(sel),
                               ia64_gr_src(op->source1),
                               ia64_gr_src(op->source2));
        ia64_simd_write_gr(op->destination, helper_result);
        ia64_gen_gr_nat_from_2(op->destination, op->source1, op->source2);
        break;
    }
    case IA64_OP_UNPACK1_H:
    case IA64_OP_UNPACK1_L:
    case IA64_OP_UNPACK2_H:
    case IA64_OP_UNPACK2_L:
    case IA64_OP_UNPACK4_H:
    case IA64_OP_UNPACK4_L: {
        uint32_t sel;

        if (insn->opcode == IA64_OP_UNPACK1_H) {
            sel = 0;
        } else if (insn->opcode == IA64_OP_UNPACK1_L) {
            sel = 1;
        } else if (insn->opcode == IA64_OP_UNPACK2_H) {
            sel = 2;
        } else if (insn->opcode == IA64_OP_UNPACK2_L) {
            sel = 3;
        } else if (insn->opcode == IA64_OP_UNPACK4_H) {
            sel = 4;
        } else {
            sel = 5;
        }
        helper_result = tcg_temp_new_i64();
        gen_helper_simd_unpack(helper_result, tcg_constant_i32(sel),
                               ia64_gr_src(op->source1),
                               ia64_gr_src(op->source2));
        ia64_simd_write_gr(op->destination, helper_result);
        ia64_gen_gr_nat_from_2(op->destination, op->source1, op->source2);
        break;
    }
    case IA64_OP_SUM:
        helper_result = tcg_temp_new_i64();
        gen_helper_simd_sum(helper_result,
                            ia64_gr_src(op->source1),
                            ia64_gr_src(op->source2));
        ia64_simd_write_gr(op->destination, helper_result);
        ia64_gen_gr_nat_from_2(op->destination, op->source1, op->source2);
        break;
    case IA64_OP_CZX1_L:
    case IA64_OP_CZX1_R:
    case IA64_OP_CZX2_L:
    case IA64_OP_CZX2_R: {
        uint32_t sel;

        if (insn->opcode == IA64_OP_CZX1_L) {
            sel = 0;
        } else if (insn->opcode == IA64_OP_CZX1_R) {
            sel = 1;
        } else if (insn->opcode == IA64_OP_CZX2_L) {
            sel = 2;
        } else {
            sel = 3;
        }
        helper_result = tcg_temp_new_i64();
        gen_helper_simd_czx(helper_result, tcg_constant_i32(sel),
                            ia64_gr_src(op->source2));
        ia64_simd_write_gr(op->destination, helper_result);
        ia64_gen_gr_nat_from_1(op->destination, op->source2);
        break;
    }
    default:
        return IA64_GEN_UNHANDLED;
    }
    return IA64_GEN_CONTINUE;
}
