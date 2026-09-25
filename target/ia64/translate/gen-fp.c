/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * IA-64 floating-point instruction generation.
 */

#include "qemu/osdep.h"

#include "exec/helper-proto.h"
#include "exec/helper-gen.h"
#include "tcg/tcg-op.h"

#include "target/ia64/translate/translate.h"

static TCGv_i32 ia64_fp_context(const Ia64Instruction *insn)
{
    const IA64FloatingOperands *op = &insn->operands.floating;

    return tcg_constant_i32(IA64_FP_CONTEXT(op->status_field, op->precision));
}

static void ia64_gen_xma(const Ia64Instruction *insn, uint32_t mode)
{
    const IA64FloatingOperands *op = &insn->operands.floating;
    TCGv_i64 all_sig = tcg_temp_new_i64();
    TCGv_i64 test = tcg_temp_new_i64();
    TCGv_i64 low = tcg_temp_new_i64();
    TCGv_i64 high = tcg_temp_new_i64();
    TCGLabel *slow = gen_new_label();
    TCGLabel *done = gen_new_label();

    tcg_gen_and_i64(all_sig, ia64_gen_fr_sig_read(op->source2),
                    ia64_gen_fr_sig_read(op->auxiliary1));
    tcg_gen_or_i64(test, ia64_gen_fr_nat_read(op->source2),
                   ia64_gen_fr_nat_read(op->auxiliary1));
    if (mode != 3) {
        tcg_gen_and_i64(all_sig, all_sig,
                        ia64_gen_fr_sig_read(op->source1));
        tcg_gen_or_i64(test, test, ia64_gen_fr_nat_read(op->source1));
    }
    tcg_gen_xori_i64(all_sig, all_sig, 1);
    tcg_gen_or_i64(test, test, all_sig);
    tcg_gen_brcondi_i64(TCG_COND_NE, test, 0, slow);

    if (mode == 1) {
        tcg_gen_muls2_i64(low, high, ia64_fr_significand_src(op->source2),
                          ia64_fr_significand_src(op->auxiliary1));
    } else {
        tcg_gen_mulu2_i64(low, high, ia64_fr_significand_src(op->source2),
                          ia64_fr_significand_src(op->auxiliary1));
    }
    if (mode != 3) {
        tcg_gen_add2_i64(low, high, low, high,
                         ia64_fr_significand_src(op->source1),
                         tcg_constant_i64(0));
    }
    ia64_gen_fr_mov_sig(op->destination, mode == 0 ? low : high);
    tcg_gen_br(done);

    gen_set_label(slow);
    gen_helper_xma(tcg_env, tcg_constant_i32(op->destination),
                   tcg_constant_i32(mode == 3 ? 0 : op->source1),
                   tcg_constant_i32(op->source2),
                   tcg_constant_i32(op->auxiliary1), tcg_constant_i32(mode));
    gen_set_label(done);
}

IA64GenResult ia64_gen_fp(DisasContext *ctx,
                          const Ia64Instruction *insn)
{
    const IA64FloatingOperands *op = &insn->operands.floating;

    switch (insn->opcode) {
    case IA64_OP_FADD:
        gen_helper_fadd(tcg_env, tcg_constant_i32(op->destination),
                        tcg_constant_i32(op->source1),
                        tcg_constant_i32(op->source2), ia64_fp_context(insn));
        break;
    case IA64_OP_FSUB:
        gen_helper_fsub(tcg_env, tcg_constant_i32(op->destination),
                        tcg_constant_i32(op->source1),
                        tcg_constant_i32(op->source2), ia64_fp_context(insn));
        break;
    case IA64_OP_FMPY:
        gen_helper_fmpy(tcg_env, tcg_constant_i32(op->destination),
                        tcg_constant_i32(op->source1),
                        tcg_constant_i32(op->source2), ia64_fp_context(insn));
        break;
    case IA64_OP_FMA:
        gen_helper_fma4(tcg_env, tcg_constant_i32(op->destination),
                        tcg_constant_i32(op->source1),
                        tcg_constant_i32(op->source2),
                        tcg_constant_i32(op->auxiliary1),
                        ia64_fp_context(insn));
        break;
    case IA64_OP_XMA_L:
        ia64_gen_xma(insn, 0);
        break;
    case IA64_OP_XMA_H:
        ia64_gen_xma(insn, 1);
        break;
    case IA64_OP_XMA_HU:
        ia64_gen_xma(insn, 2);
        break;
    case IA64_OP_XMPY_HU:
        ia64_gen_xma(insn, 3);
        break;
    case IA64_OP_FMIN:
        gen_helper_fmin(tcg_env, tcg_constant_i32(op->destination),
                        tcg_constant_i32(op->source1),
                        tcg_constant_i32(op->source2), ia64_fp_context(insn));
        break;
    case IA64_OP_FMAX:
        gen_helper_fmax(tcg_env, tcg_constant_i32(op->destination),
                        tcg_constant_i32(op->source1),
                        tcg_constant_i32(op->source2), ia64_fp_context(insn));
        break;
    case IA64_OP_FAMIN:
        gen_helper_famin(tcg_env, tcg_constant_i32(op->destination),
                         tcg_constant_i32(op->source1),
                         tcg_constant_i32(op->source2), ia64_fp_context(insn));
        break;
    case IA64_OP_FAMAX:
        gen_helper_famax(tcg_env, tcg_constant_i32(op->destination),
                         tcg_constant_i32(op->source1),
                         tcg_constant_i32(op->source2), ia64_fp_context(insn));
        break;
    case IA64_OP_FRCPA:
        gen_helper_frcpa(tcg_env, tcg_constant_i32(op->destination),
                         tcg_constant_i32(op->auxiliary1),
                         tcg_constant_i32(op->source1),
                         tcg_constant_i32(op->source2), ia64_fp_context(insn));
        break;
    case IA64_OP_FPRCPA:
        gen_helper_fprcpa(tcg_env, tcg_constant_i32(op->destination),
                          tcg_constant_i32(op->auxiliary2),
                          tcg_constant_i32(op->source1),
                          tcg_constant_i32(op->source2),
                          ia64_fp_context(insn));
        break;
    case IA64_OP_FCMP:
        gen_helper_fcmp(tcg_env,
                        tcg_constant_i32(op->auxiliary1),
                        tcg_constant_i32(op->auxiliary2),
                        tcg_constant_i32(op->source1),
                        tcg_constant_i32(op->source2),
                        tcg_constant_i32(op->immediate), ia64_fp_context(insn));
        break;
    case IA64_OP_FMS:
        gen_helper_fms(tcg_env, tcg_constant_i32(op->destination),
                       tcg_constant_i32(op->source1),
                       tcg_constant_i32(op->source2),
                       tcg_constant_i32(op->auxiliary1), ia64_fp_context(insn));
        break;
    case IA64_OP_FNMA:
        gen_helper_fnma4(tcg_env, tcg_constant_i32(op->destination),
                         tcg_constant_i32(op->source1),
                         tcg_constant_i32(op->source2),
                         tcg_constant_i32(op->auxiliary1),
                         ia64_fp_context(insn));
        break;
    case IA64_OP_FSELECT:
        gen_helper_fselect(tcg_env, tcg_constant_i32(op->destination),
                           tcg_constant_i32(op->source1),
                           tcg_constant_i32(op->source2),
                           tcg_constant_i32(op->auxiliary1));
        break;
    case IA64_OP_FNORM:
        gen_helper_fnorm(tcg_env, tcg_constant_i32(op->destination),
                         tcg_constant_i32(op->source1),
                         tcg_constant_i32(op->source2), ia64_fp_context(insn));
        break;
    case IA64_OP_FPRSQRTA:
        gen_helper_fprsqrta(tcg_env, tcg_constant_i32(op->destination),
                            tcg_constant_i32(op->auxiliary2),
                            tcg_constant_i32(op->source2),
                            ia64_fp_context(insn));
        break;
    case IA64_OP_FRSQRTA:
        gen_helper_frsqrta(tcg_env, tcg_constant_i32(op->destination),
                           tcg_constant_i32(op->auxiliary2),
                           tcg_constant_i32(op->source2),
                           ia64_fp_context(insn));
        break;
    case IA64_OP_FPACK:
        gen_helper_fpack(tcg_env, tcg_constant_i32(op->destination),
                         tcg_constant_i32(op->source1),
                         tcg_constant_i32(op->source2));
        break;
    case IA64_OP_FAND:
        gen_helper_flogical_and(tcg_env, tcg_constant_i32(op->destination),
                                tcg_constant_i32(op->source1),
                                tcg_constant_i32(op->source2));
        break;
    case IA64_OP_FANDCM:
        gen_helper_flogical_andcm(tcg_env, tcg_constant_i32(op->destination),
                                  tcg_constant_i32(op->source1),
                                  tcg_constant_i32(op->source2));
        break;
    case IA64_OP_FOR:
        gen_helper_flogical_or(tcg_env, tcg_constant_i32(op->destination),
                               tcg_constant_i32(op->source1),
                               tcg_constant_i32(op->source2));
        break;
    case IA64_OP_FXOR:
        gen_helper_flogical_xor(tcg_env, tcg_constant_i32(op->destination),
                                tcg_constant_i32(op->source1),
                                tcg_constant_i32(op->source2));
        break;
    case IA64_OP_FSWAP:
        gen_helper_fswap(tcg_env, tcg_constant_i32(op->destination),
                         tcg_constant_i32(op->source1),
                         tcg_constant_i32(op->source2),
                         tcg_constant_i32(0));
        break;
    case IA64_OP_FSWAP_NL:
        gen_helper_fswap(tcg_env, tcg_constant_i32(op->destination),
                         tcg_constant_i32(op->source1),
                         tcg_constant_i32(op->source2),
                         tcg_constant_i32(1));
        break;
    case IA64_OP_FSWAP_NR:
        gen_helper_fswap(tcg_env, tcg_constant_i32(op->destination),
                         tcg_constant_i32(op->source1),
                         tcg_constant_i32(op->source2),
                         tcg_constant_i32(2));
        break;
    case IA64_OP_FMIX_LR:
        gen_helper_fmix(tcg_env, tcg_constant_i32(op->destination),
                        tcg_constant_i32(op->source1),
                        tcg_constant_i32(op->source2),
                        tcg_constant_i32(0));
        break;
    case IA64_OP_FMIX_R:
        gen_helper_fmix(tcg_env, tcg_constant_i32(op->destination),
                        tcg_constant_i32(op->source1),
                        tcg_constant_i32(op->source2),
                        tcg_constant_i32(1));
        break;
    case IA64_OP_FMIX_L:
        gen_helper_fmix(tcg_env, tcg_constant_i32(op->destination),
                        tcg_constant_i32(op->source1),
                        tcg_constant_i32(op->source2),
                        tcg_constant_i32(2));
        break;
    case IA64_OP_FSXT_R:
        gen_helper_fsxt(tcg_env, tcg_constant_i32(op->destination),
                        tcg_constant_i32(op->source1),
                        tcg_constant_i32(op->source2),
                        tcg_constant_i32(0));
        break;
    case IA64_OP_FSXT_L:
        gen_helper_fsxt(tcg_env, tcg_constant_i32(op->destination),
                        tcg_constant_i32(op->source1),
                        tcg_constant_i32(op->source2),
                        tcg_constant_i32(1));
        break;
    case IA64_OP_FPMERGE:
        gen_helper_fpmerge(tcg_env, tcg_constant_i32(op->destination),
                           tcg_constant_i32(op->source1),
                           tcg_constant_i32(op->source2),
                           tcg_constant_i32(0));
        break;
    case IA64_OP_FPMERGE_S:
        gen_helper_fpmerge(tcg_env, tcg_constant_i32(op->destination),
                           tcg_constant_i32(op->source1),
                           tcg_constant_i32(op->source2),
                           tcg_constant_i32(1));
        break;
    case IA64_OP_FPMERGE_SE:
        gen_helper_fpmerge(tcg_env, tcg_constant_i32(op->destination),
                           tcg_constant_i32(op->source1),
                           tcg_constant_i32(op->source2),
                           tcg_constant_i32(2));
        break;
    case IA64_OP_FPMIN:
        gen_helper_fpminmax(tcg_env, tcg_constant_i32(op->destination),
                            tcg_constant_i32(op->source1),
                            tcg_constant_i32(op->source2),
                            tcg_constant_i32(0),
                            tcg_constant_i32(0),
                            ia64_fp_context(insn));
        break;
    case IA64_OP_FPMAX:
        gen_helper_fpminmax(tcg_env, tcg_constant_i32(op->destination),
                            tcg_constant_i32(op->source1),
                            tcg_constant_i32(op->source2),
                            tcg_constant_i32(1),
                            tcg_constant_i32(0),
                            ia64_fp_context(insn));
        break;
    case IA64_OP_FPAMIN:
        gen_helper_fpminmax(tcg_env, tcg_constant_i32(op->destination),
                            tcg_constant_i32(op->source1),
                            tcg_constant_i32(op->source2),
                            tcg_constant_i32(0),
                            tcg_constant_i32(1),
                            ia64_fp_context(insn));
        break;
    case IA64_OP_FPAMAX:
        gen_helper_fpminmax(tcg_env, tcg_constant_i32(op->destination),
                            tcg_constant_i32(op->source1),
                            tcg_constant_i32(op->source2),
                            tcg_constant_i32(1),
                            tcg_constant_i32(1),
                            ia64_fp_context(insn));
        break;
    case IA64_OP_FPCMP:
        gen_helper_fpcmp(tcg_env, tcg_constant_i32(op->destination),
                         tcg_constant_i32(op->source1),
                         tcg_constant_i32(op->source2),
                         tcg_constant_i32(op->immediate),
                         ia64_fp_context(insn));
        break;
    case IA64_OP_FPCVT:
        gen_helper_fpcvt(tcg_env, tcg_constant_i32(op->destination),
                         tcg_constant_i32(op->source1),
                         tcg_constant_i32(op->immediate & 1),
                         tcg_constant_i32((op->immediate >> 1) & 1),
                         ia64_fp_context(insn));
        break;
    case IA64_OP_FPMA:
        gen_helper_fpma(tcg_env, tcg_constant_i32(op->destination),
                        tcg_constant_i32(op->source1),
                        tcg_constant_i32(op->source2),
                        tcg_constant_i32(op->auxiliary1),
                        tcg_constant_i32(0),
                        ia64_fp_context(insn));
        break;
    case IA64_OP_FPMS:
        gen_helper_fpma(tcg_env, tcg_constant_i32(op->destination),
                        tcg_constant_i32(op->source1),
                        tcg_constant_i32(op->source2),
                        tcg_constant_i32(op->auxiliary1),
                        tcg_constant_i32(1),
                        ia64_fp_context(insn));
        break;
    case IA64_OP_FPNMA:
        gen_helper_fpma(tcg_env, tcg_constant_i32(op->destination),
                        tcg_constant_i32(op->source1),
                        tcg_constant_i32(op->source2),
                        tcg_constant_i32(op->auxiliary1),
                        tcg_constant_i32(2),
                        ia64_fp_context(insn));
        break;
    case IA64_OP_FMOV:
        gen_helper_fmov(tcg_env, tcg_constant_i32(op->destination),
                        tcg_constant_i32(op->source1));
        break;
    case IA64_OP_FCVT_XF:
        gen_helper_fcvt_xf(tcg_env, tcg_constant_i32(op->destination),
                           tcg_constant_i32(op->source1));
        break;
    case IA64_OP_FCVT_FX:
    case IA64_OP_FCVT_FXU:
        gen_helper_fcvt_fx(tcg_env, tcg_constant_i32(op->destination),
                           tcg_constant_i32(op->source1),
                           tcg_constant_i32(insn->opcode == IA64_OP_FCVT_FXU),
                           tcg_constant_i32((op->immediate >> 1) & 1),
                           ia64_fp_context(insn));
        break;
    case IA64_OP_GETF_D:
        if (op->destination != 0) {
            gen_helper_getf(cpu_gr[op->destination], tcg_env,
                            tcg_constant_i32(op->source1),
                            tcg_constant_i32(0));
            ia64_gen_gr_nat_assign(op->destination,
                                   ia64_gen_fr_nat_read(op->source1));
        }
        break;
    case IA64_OP_GETF_S:
        if (op->destination != 0) {
            gen_helper_getf(cpu_gr[op->destination], tcg_env,
                            tcg_constant_i32(op->source1),
                            tcg_constant_i32(1));
            ia64_gen_gr_nat_assign(op->destination,
                                   ia64_gen_fr_nat_read(op->source1));
        }
        break;
    case IA64_OP_GETF_SIG:
        if (op->destination != 0) {
            TCGLabel *slow = gen_new_label();
            TCGLabel *done = gen_new_label();

            tcg_gen_brcondi_i64(TCG_COND_EQ,
                                ia64_gen_fr_sig_read(op->source1), 0, slow);
            tcg_gen_mov_i64(cpu_gr[op->destination],
                            ia64_fr_significand_src(op->source1));
            tcg_gen_br(done);
            gen_set_label(slow);
            gen_helper_getf(cpu_gr[op->destination], tcg_env,
                            tcg_constant_i32(op->source1),
                            tcg_constant_i32(2));
            gen_set_label(done);
            ia64_gen_gr_nat_assign(op->destination,
                                   ia64_gen_fr_nat_read(op->source1));
        }
        break;
    case IA64_OP_GETF_EXP: {
        if (op->destination != 0) {
            gen_helper_getf(cpu_gr[op->destination], tcg_env,
                            tcg_constant_i32(op->source1),
                            tcg_constant_i32(3));
            ia64_gen_gr_nat_assign(op->destination,
                                   ia64_gen_fr_nat_read(op->source1));
        }
        break;
    }
    case IA64_OP_SETF_D:
        ia64_gen_fr_mov(op->destination, ia64_gr_src(op->source1));
        ia64_gen_fr_nat_from_gr(op->destination, op->source1);
        break;
    case IA64_OP_SETF_S: {
        gen_helper_setf_s(tcg_env, tcg_constant_i32(op->destination),
                          ia64_gr_src(op->source1));
        ia64_gen_fr_nat_from_gr(op->destination, op->source1);
        break;
    }
    case IA64_OP_SETF_EXP:
        gen_helper_setf_exp(tcg_env, tcg_constant_i32(op->destination),
                            ia64_gr_src(op->source1));
        ia64_gen_fr_nat_from_gr(op->destination, op->source1);
        break;
    case IA64_OP_SETF_SIG:
        ia64_gen_fr_mov_sig(op->destination, ia64_gr_src(op->source1));
        ia64_gen_fr_nat_from_gr(op->destination, op->source1);
        break;
    case IA64_OP_FCLASS:
        gen_helper_fclass(tcg_env, tcg_constant_i32(op->auxiliary1),
                          tcg_constant_i32(op->auxiliary2),
                          tcg_constant_i32(op->source1),
                          tcg_constant_i32(op->immediate));
        break;
    case IA64_OP_FMERGE:
        gen_helper_fmerge_ns(tcg_env, tcg_constant_i32(op->destination),
                             tcg_constant_i32(op->source1),
                             tcg_constant_i32(op->source2));
        break;
    case IA64_OP_FMERGE_S:
        gen_helper_fmerge_s(tcg_env, tcg_constant_i32(op->destination),
                            tcg_constant_i32(op->source1),
                            tcg_constant_i32(op->source2));
        break;
    case IA64_OP_FMERGE_SE:
        gen_helper_fmerge_se(tcg_env, tcg_constant_i32(op->destination),
                             tcg_constant_i32(op->source1),
                             tcg_constant_i32(op->source2));
        break;
    default:
        return IA64_GEN_UNHANDLED;
    }
    return IA64_GEN_CONTINUE;
}
