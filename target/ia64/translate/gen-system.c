/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * IA-64 system register, RSE and translation-control generation.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"

#include "exec/helper-proto.h"
#include "exec/helper-gen.h"
#include "tcg/tcg-op.h"

#include "target/ia64/translate/translate.h"

/*
 * CR numbers whose write behaviour in ia64_write_cr is a plain store into
 * env->cr[] -- no timer rearm, no TLB/TB flush, no SAPIC re-evaluation, no
 * atomics -- and whose value nothing consumes at translation time.  These
 * can be written inline and, critically, do not need to end the
 * translation block: they are the interruption resources every handler
 * prologue and epilogue writes (IIP/IPSR/IFS/IFA/ISR/IIM/ITIR/IIPA/IHA),
 * plus DCR and the plain-stored vector registers.
 */
static bool ia64_cr_write_is_plain_store(uint32_t cr_num)
{
    switch (cr_num) {
    case IA64_CR_DCR:
    case IA64_CR_IPSR:
    case IA64_CR_ISR:
    case IA64_CR_IIP:
    case IA64_CR_IFA:
    case IA64_CR_ITIR:
    case IA64_CR_IIPA:
    case IA64_CR_IFS:
    case IA64_CR_IIM:
    case IA64_CR_IHA:
    case IA64_CR_PMV:
    case IA64_CR_CMCV:
        return true;
    default:
        return false;
    }
}

/*
 * CR numbers ia64_system_read_cr serves straight from env->cr[]: the plain
 * set above plus ITM/IVA/PTA/TPR, whose special behaviour is write-only.
 * LID (atomic), IVR (acknowledge side effect) and IRR0-3 (live interrupt
 * state) must keep going through the helper.
 */
static bool ia64_cr_read_is_plain_load(uint32_t cr_num)
{
    return ia64_cr_write_is_plain_store(cr_num) ||
           cr_num == IA64_CR_ITM || cr_num == IA64_CR_IVA ||
           cr_num == IA64_CR_PTA || cr_num == IA64_CR_SAPIC_TPR;
}

IA64GenResult ia64_gen_system(DisasContext *ctx,
                              const Ia64Instruction *insn,
                              TCGLabel *skip, bool record_iipa,
                              bool track_psr_suppression)
{
    const IA64SystemOperands *op = &insn->operands.system;
    const uint64_t next_ip = insn->address + 16;

    switch (insn->opcode) {
    case IA64_OP_MOV_BRGR:
        ia64_gen_gr_write_nat_clear(op->destination, cpu_br[op->source & 7]);
        break;
    case IA64_OP_MOV_GRBR:
        ia64_gen_check_nat_register(insn, op->destination);
        tcg_gen_mov_i64(cpu_br[op->source & 7],
                        ia64_gr_src(op->destination));
        break;
    case IA64_OP_MOV_PRGR:
        if (op->destination != 0) {
            TCGv_i64 packed = tcg_temp_new_i64();

            gen_helper_read_pr(packed, tcg_env);
            ia64_gen_gr_write_nat_clear(op->destination, packed);
        }
        break;
    case IA64_OP_MOV_GRPR:
        ia64_gen_check_nat_register(insn, op->destination);
        gen_helper_write_pr(tcg_env, ia64_gr_src(op->destination),
                            tcg_constant_i64(op->immediate));
        break;
    case IA64_OP_MOV_PR_ROT_IMM:
        gen_helper_write_pr(tcg_env, tcg_constant_i64(op->immediate),
                            tcg_constant_i64(0xffffffffffff0000ULL));
        break;
    case IA64_OP_MOV_ARGR:
        /*
         * The application-register access check applies to the read itself, so
         * it must run even when the destination is r0 (a discarded result).
         */
        if (!ia64_ar_is_simple(op->source)) {
            ia64_gen_validate_ar_access(insn, tcg_constant_i64(0), false);
        }
        if (op->destination != 0) {
            TCGv_i64 val = tcg_temp_new_i64();

            if (ia64_ar_is_simple(op->source)) {
                ia64_gen_read_simple_ar(val, op->source);
            } else {
                if (ia64_ar_access_reads_clock(op->source) &&
                    ia64_clock_access_needs_io(ctx)) {
                    translator_io_start(&ctx->base);
                }
                if (op->source == IA64_AR_ITC) {
                    gen_helper_itc_read(val, tcg_env);
                } else {
                    gen_helper_read_ar(val, tcg_env,
                                       tcg_constant_i32(op->source));
                }
            }
            ia64_gen_gr_write_nat_clear(op->destination, val);
        }
        break;
    case IA64_OP_MOV_GRAR:
        ia64_gen_check_nat_register(insn, op->destination);
        if (ia64_ar_is_simple(op->source)) {
            if (op->source == 40 || op->source == 64) {
                ia64_gen_validate_ar_access(insn, ia64_gr_src(op->destination),
                                            true);
            }
            ia64_gen_write_simple_ar(op->source, ia64_gr_src(op->destination));
        } else {
            ia64_gen_validate_ar_access(insn,
                                        ia64_gr_src(op->destination), true);
            if (ia64_ar_access_reads_clock(op->source)) {
                translator_io_start(&ctx->base);
                ctx->restart.exit_after_bundle = true;
            }
            gen_helper_write_ar(tcg_env, tcg_constant_i32(op->source),
                                ia64_gr_src(op->destination));
        }
        break;
    case IA64_OP_MOV_IMMAR:
        if (ia64_ar_is_simple(op->source)) {
            if (op->source == 40 || op->source == 64) {
                ia64_gen_validate_ar_access(
                    insn, tcg_constant_i64(op->immediate), true);
            }
            ia64_gen_write_simple_ar(op->source,
                                     tcg_constant_i64(op->immediate));
        } else {
            ia64_gen_validate_ar_access(
                insn, tcg_constant_i64(op->immediate), true);
            if (ia64_ar_access_reads_clock(op->source)) {
                translator_io_start(&ctx->base);
                ctx->restart.exit_after_bundle = true;
            }
            gen_helper_write_ar(tcg_env, tcg_constant_i32(op->source),
                                tcg_constant_i64(op->immediate));
        }
        break;
    case IA64_OP_MOV_CRGR:
    {
        /* The CR access check applies even when the destination is r0. */
        TCGv_i64 checked = tcg_temp_new_i64();

        ia64_gen_validate_cr_access(checked, insn,
                                    tcg_constant_i64(0), false);
        if (op->destination != 0) {
            TCGv_i64 val = tcg_temp_new_i64();

            if (ia64_cr_read_is_plain_load(op->source)) {
                tcg_gen_ld_i64(val, tcg_env,
                               offsetof(CPUIA64State, cr) +
                               op->source * sizeof(uint64_t));
            } else {
                gen_helper_read_cr(val, tcg_env,
                                   tcg_constant_i32(op->source));
            }
            ia64_gen_gr_write_nat_clear(op->destination, val);
        }
        break;
    }
    case IA64_OP_MOV_GRCR:
    {
        TCGv_i64 checked = tcg_temp_new_i64();

        if (ia64_cr_is_read_only(op->source)) {
            ia64_gen_raise_exception(IA64_EXCP_ILLEGAL, insn->address,
                                      insn->raw, insn->slot);
            if (skip == NULL) {
                return IA64_GEN_NORETURN;
            }
            break;
        }
        ia64_gen_check_nat_register(insn, op->destination);
        ia64_gen_validate_cr_access(checked, insn,
                                    ia64_gr_src(op->destination), true);
        if (ia64_cr_write_is_plain_store(op->source)) {
            /*
             * validate_cr_access already faulted or masked the value, so
             * the store is all that remains of ia64_write_cr's default
             * case.  Not ending the TB here removes a translation break
             * from every interruption prologue and epilogue.
             */
            tcg_gen_st_i64(checked, tcg_env,
                           offsetof(CPUIA64State, cr) +
                           op->source * sizeof(uint64_t));
            if (op->source == IA64_CR_IIPA) {
                /*
                 * mov to CR.IIPA re-establishes the last-executed-bundle
                 * latch that a subsequent interruption reports as IIPA.
                 */
                tcg_gen_st_i64(checked, tcg_env,
                               offsetof(CPUIA64State,
                                        last_successful_bundle));
            }
        } else if (op->source == IA64_CR_SAPIC_LID) {
            /* Atomic store for cross-CPU readers; no translation effect. */
            gen_helper_write_cr(tcg_env, tcg_constant_i32(op->source),
                                checked);
        } else {
            /*
             * ITM/IVA/PTA/TPR/EOI/ITV and any unlisted number: timer
             * rearm, TLB/TB flush or interrupt re-evaluation -- keep the
             * helper and end the TB so the pending-interrupt check runs.
             */
            if (ia64_cr_write_reads_clock(op->source)) {
                translator_io_start(&ctx->base);
            }
            gen_helper_write_cr(tcg_env, tcg_constant_i32(op->source),
                                checked);
            ctx->restart.exit_after_bundle = true;
        }
        break;
    }
    case IA64_OP_MOV_RRGR: {
        TCGv_i64 val = tcg_temp_new_i64();
        ia64_gen_check_nat_register(insn, op->source);
        gen_helper_mov_rrgr_read(val, tcg_env, ia64_gr_src(op->source));
        ia64_gen_gr_write_nat_clear(op->destination, val);
        break;
    }
    case IA64_OP_MOV_GRRR:
    {
        TCGv_i64 checked = tcg_temp_new_i64();

        ia64_gen_check_nat_register(insn, op->source);
        ia64_gen_check_nat_register(insn, op->destination);
        gen_helper_validate_rr_value(
            checked, tcg_env, ia64_gr_src(op->destination),
            tcg_constant_i64(insn->address),
            tcg_constant_i64(insn->raw),
            tcg_constant_i32(insn->slot));
        gen_helper_mov_grrr_write(tcg_env, ia64_gr_src(op->source),
                                  checked);
        ctx->restart.exit_after_bundle = true;
        break;
    }
    case IA64_OP_MOV_PKRGR: {
        TCGv_i64 val = tcg_temp_new_i64();
        gen_helper_mov_pkrgr_read(val, tcg_env, tcg_constant_i32(op->source));
        ia64_gen_gr_write_nat_clear(op->destination, val);
        break;
    }
    case IA64_OP_MOV_PKRGR_INDEXED: {
        TCGv_i64 val = tcg_temp_new_i64();
        ia64_gen_check_nat_register(insn, op->register_index);
        ia64_gen_check_register_index(insn, ia64_gr_src(op->register_index),
                                      IA64_PKR_COUNT);
        gen_helper_mov_pkrgr_indexed_read(
            val, tcg_env, ia64_gr_src(op->register_index));
        ia64_gen_gr_write_nat_clear(op->destination, val);
        break;
    }
    case IA64_OP_MOV_GRPKR:
        ia64_gen_check_nat_register(insn, op->destination);
        ia64_gen_check_reserved_bits(insn, ia64_gr_src(op->destination),
                                     ia64_pkr_mask(ctx->env));
        gen_helper_mov_grpkr_write(tcg_env, tcg_constant_i32(op->source),
                                   ia64_gr_src(op->destination));
        ctx->restart.exit_after_bundle = true;
        break;
    case IA64_OP_MOV_GRPKR_INDEXED:
        ia64_gen_check_nat_register(insn, op->register_index);
        ia64_gen_check_nat_register(insn, op->destination);
        ia64_gen_check_register_index(insn, ia64_gr_src(op->register_index),
                                      IA64_PKR_COUNT);
        ia64_gen_check_reserved_bits(insn, ia64_gr_src(op->destination),
                                     ia64_pkr_mask(ctx->env));
        gen_helper_mov_grpkr_indexed_write(
            tcg_env, ia64_gr_src(op->register_index),
            ia64_gr_src(op->destination));
        ctx->restart.exit_after_bundle = true;
        break;
    case IA64_OP_MOV_UMGR: {
        TCGv_i64 val = tcg_temp_new_i64();
        tcg_gen_andi_i64(val, cpu_psr, IA64_PSR_UM_WRITABLE_MASK);
        ia64_gen_gr_write_nat_clear(op->destination, val);
        break;
    }
    case IA64_OP_MOV_GRUM: {
        ia64_gen_check_nat_register(insn, op->destination);
        ia64_gen_check_reserved_bits(insn, ia64_gr_src(op->destination),
                                     IA64_PSR_UM_WRITABLE_MASK);
        ia64_gen_write_user_mask(ia64_gr_src(op->destination));
        ctx->restart.exit_after_bundle = true;
        break;
    }
    case IA64_OP_MOV_IBRGR: {
        TCGv_i64 val = tcg_temp_new_i64();
        gen_helper_read_ibr(val, tcg_env, tcg_constant_i32(op->source - 12));
        ia64_gen_gr_write_nat_clear(op->destination, val);
        break;
    }
    case IA64_OP_MOV_GRIBR:
        ia64_gen_check_nat_register(insn, op->destination);
        gen_helper_write_ibr(tcg_env, tcg_constant_i32(op->source - 12),
                             ia64_gr_src(op->destination));
        break;
    case IA64_OP_MOV_IBRGR_INDEXED:
    case IA64_OP_MOV_DBRGR_INDEXED: {
        TCGv_i64 index64 = tcg_temp_new_i64();
        TCGv_i32 index = tcg_temp_new_i32();
        TCGv_i64 val = tcg_temp_new_i64();

        ia64_gen_check_nat_register(insn, op->register_index);
        ia64_gen_check_register_index(
            insn, ia64_gr_src(op->register_index),
            insn->opcode == IA64_OP_MOV_IBRGR_INDEXED ?
            IA64_IBR_COUNT : IA64_DBR_COUNT);
        tcg_gen_mov_i64(index64, ia64_gr_src(op->register_index));
        tcg_gen_extrl_i64_i32(index, index64);
        if (insn->opcode == IA64_OP_MOV_IBRGR_INDEXED) {
            gen_helper_read_ibr(val, tcg_env, index);
        } else {
            gen_helper_read_dbr(val, tcg_env, index);
        }
        ia64_gen_gr_write_nat_clear(op->destination, val);
        break;
    }
    case IA64_OP_MOV_GRIBR_INDEXED:
    case IA64_OP_MOV_GRDBR_INDEXED: {
        TCGv_i64 index64 = tcg_temp_new_i64();
        TCGv_i32 index = tcg_temp_new_i32();

        ia64_gen_check_nat_register(insn, op->register_index);
        ia64_gen_check_nat_register(insn, op->destination);
        ia64_gen_check_register_index(
            insn, ia64_gr_src(op->register_index),
            insn->opcode == IA64_OP_MOV_GRIBR_INDEXED ?
            IA64_IBR_COUNT : IA64_DBR_COUNT);
        tcg_gen_mov_i64(index64, ia64_gr_src(op->register_index));
        tcg_gen_extrl_i64_i32(index, index64);
        if (insn->opcode == IA64_OP_MOV_GRIBR_INDEXED) {
            gen_helper_write_ibr(tcg_env, index, ia64_gr_src(op->destination));
        } else {
            gen_helper_write_dbr(tcg_env, index, ia64_gr_src(op->destination));
        }
        break;
    }
    case IA64_OP_MOV_DBRGR: {
        TCGv_i64 val = tcg_temp_new_i64();
        gen_helper_read_dbr(val, tcg_env, tcg_constant_i32(op->source - 8));
        ia64_gen_gr_write_nat_clear(op->destination, val);
        break;
    }
    case IA64_OP_MOV_GRDBR:
        ia64_gen_check_nat_register(insn, op->destination);
        gen_helper_write_dbr(tcg_env, tcg_constant_i32(op->source - 8),
                             ia64_gr_src(op->destination));
        break;
    case IA64_OP_MOV_PMCGR: {
        TCGv_i64 val = tcg_temp_new_i64();
        gen_helper_read_pmc(val, tcg_env, tcg_constant_i32(op->source - 32));
        ia64_gen_gr_write_nat_clear(op->destination, val);
        break;
    }
    case IA64_OP_MOV_GRPMC:
        ia64_gen_check_nat_register(insn, op->destination);
        gen_helper_write_pmc(tcg_env, tcg_constant_i32(op->source - 32),
                            ia64_gr_src(op->destination));
        break;
    case IA64_OP_MOV_PMCGR_INDEXED: {
        TCGv_i64 val = tcg_temp_new_i64();
        ia64_gen_check_nat_register(insn, op->register_index);
        ia64_gen_check_register_index(insn, ia64_gr_src(op->register_index),
                                      IA64_PMC_COUNT);
        gen_helper_read_pmc_indexed(
            val, tcg_env, ia64_gr_src(op->register_index));
        ia64_gen_gr_write_nat_clear(op->destination, val);
        break;
    }
    case IA64_OP_MOV_GRPMC_INDEXED:
        ia64_gen_check_nat_register(insn, op->register_index);
        ia64_gen_check_nat_register(insn, op->destination);
        ia64_gen_check_register_index(insn, ia64_gr_src(op->register_index),
                                      IA64_PMC_COUNT);
        gen_helper_write_pmc_indexed(tcg_env, ia64_gr_src(op->register_index),
                                     ia64_gr_src(op->destination));
        break;
    case IA64_OP_MOV_PMDGR: {
        TCGv_i64 val = tcg_temp_new_i64();
        gen_helper_read_pmd_checked(
            val, tcg_env, tcg_constant_i64(op->source - 64),
            tcg_constant_i64(insn->address),
            tcg_constant_i64(insn->raw),
            tcg_constant_i32(insn->slot));
        ia64_gen_gr_write_nat_clear(op->destination, val);
        break;
    }
    case IA64_OP_MOV_GRPMD:
        ia64_gen_check_nat_register(insn, op->destination);
        gen_helper_write_pmd(tcg_env, tcg_constant_i32(op->source - 64),
                            ia64_gr_src(op->destination));
        break;
    case IA64_OP_MOV_PMDGR_INDEXED: {
        TCGv_i64 val = tcg_temp_new_i64();
        ia64_gen_check_nat_register(insn, op->register_index);
        ia64_gen_check_register_index(insn, ia64_gr_src(op->register_index),
                                      IA64_PMD_COUNT);
        gen_helper_read_pmd_checked(
            val, tcg_env, ia64_gr_src(op->register_index),
            tcg_constant_i64(insn->address),
            tcg_constant_i64(insn->raw),
            tcg_constant_i32(insn->slot));
        ia64_gen_gr_write_nat_clear(op->destination, val);
        break;
    }
    case IA64_OP_MOV_GRPMD_INDEXED:
        ia64_gen_check_nat_register(insn, op->register_index);
        ia64_gen_check_nat_register(insn, op->destination);
        ia64_gen_check_register_index(insn, ia64_gr_src(op->register_index),
                                      IA64_PMD_COUNT);
        gen_helper_write_pmd_indexed(tcg_env, ia64_gr_src(op->register_index),
                                     ia64_gr_src(op->destination));
        break;
    case IA64_OP_MOV_CPUID: {
        TCGv_i64 val = tcg_temp_new_i64();
        gen_helper_read_cr(val, tcg_env, tcg_constant_i32(13));
        ia64_gen_gr_write_nat_clear(op->destination, val);
        break;
    }
    case IA64_OP_MOV_CPUID_INDEXED: {
        TCGv_i64 val = tcg_temp_new_i64();
        ia64_gen_check_nat_register(insn, op->register_index);
        ia64_gen_check_register_index(insn, ia64_gr_src(op->register_index), 5);
        gen_helper_read_cpuid(val, tcg_env, ia64_gr_src(op->register_index));
        ia64_gen_gr_write_nat_clear(op->destination, val);
        break;
    }
    case IA64_OP_MOV_DAHRGR_INDEXED: {
        TCGv_i64 val = tcg_temp_new_i64();
        ia64_gen_check_nat_register(insn, op->register_index);
        ia64_gen_check_register_index(insn, ia64_gr_src(op->register_index), 8);
        gen_helper_read_dahr_indexed(
            val, tcg_env, ia64_gr_src(op->register_index));
        ia64_gen_gr_write_nat_clear(op->destination, val);
        break;
    }
    case IA64_OP_MOV_MSRGR: {
        TCGv_i64 val = tcg_temp_new_i64();
        ia64_gen_check_nat_register(insn, op->register_index);
        gen_helper_read_msr(val, tcg_env, ia64_gr_src(op->register_index));
        ia64_gen_gr_write_nat_clear(op->destination, val);
        break;
    }
    case IA64_OP_MOV_GRMSR:
        ia64_gen_check_nat_register(insn, op->register_index);
        ia64_gen_check_nat_register(insn, op->destination);
        /*
         * MSR writes are plain stores into env->msr[]; nothing reads them
         * at translation time, so no TB exit is needed.
         */
        gen_helper_write_msr(tcg_env, ia64_gr_src(op->register_index),
                             ia64_gr_src(op->destination));
        break;
    case IA64_OP_MOV_IP:
    case IA64_OP_MOV_CURRENT_IP:
        if (op->destination != 0) {
            tcg_gen_movi_i64(cpu_gr[op->destination], insn->address);
            ia64_gen_gr_nat_clear(op->destination);
        }
        break;
    case IA64_OP_SSM:
        gen_helper_ssm(tcg_env, tcg_constant_i64(op->immediate));
        ctx->restart.exit_after_bundle = true;
        break;
    case IA64_OP_RSM:
        gen_helper_rsm(tcg_env, tcg_constant_i64(op->immediate));
        ctx->restart.exit_after_bundle = true;
        break;
    case IA64_OP_MOV_PSRGR: {
        TCGv_i64 val = tcg_temp_new_i64();
        gen_helper_mov_psrgr_read(val, tcg_env, tcg_constant_i32(0));
        ia64_gen_gr_write_nat_clear(op->destination, val);
        break;
    }
    case IA64_OP_MOV_GRPSR:
        ia64_gen_check_nat_register(insn, op->destination);
        gen_helper_mov_psr_write(tcg_env, ia64_gr_src(op->destination),
                                 tcg_constant_i32(op->immediate != 0));
        ctx->restart.exit_after_bundle = true;
        break;
    case IA64_OP_BSW0:
        gen_helper_set_psr_bn(tcg_env, tcg_constant_i32(0));
        if (skip == NULL) {
            ia64_gen_exit_to_completed(ctx, next_ip, insn->address,
                                       record_iipa,
                                       track_psr_suppression);
            return IA64_GEN_NORETURN;
        }
        break;
    case IA64_OP_BSW1:
        gen_helper_set_psr_bn(tcg_env, tcg_constant_i32(1));
        if (skip == NULL) {
            ia64_gen_exit_to_completed(ctx, next_ip, insn->address,
                                       record_iipa,
                                       track_psr_suppression);
            return IA64_GEN_NORETURN;
        }
        break;
    case IA64_OP_ALLOC: {
        uint64_t packed = (uint64_t)op->immediate;
        uint32_t sof  = (packed >> 0) & 0x7f;
        uint32_t sol  = (packed >> 7) & 0x7f;
        uint32_t sor  = (packed >> 14) & 0x0f;
        if (sof > 96 || sol > sof || (sor << 3) > sof) {
            ia64_gen_raise_exception(IA64_EXCP_ILLEGAL, insn->address,
                                      insn->raw, insn->slot);
            return IA64_GEN_NORETURN;
        }
        gen_helper_alloc_rse(tcg_env,
                              tcg_constant_i32(op->destination),
                              tcg_constant_i32(sof | (sol << 7) |
                                               (sor << 14)),
                              tcg_constant_i64(insn->address),
                              tcg_constant_i32(insn->slot));
        break;
    }
    case IA64_OP_COVER:
        gen_helper_cover_rse(tcg_env);
        break;
    case IA64_OP_FLUSHRS:
        gen_helper_flushrs_rse(tcg_env);
        break;
    case IA64_OP_LOADRS:
        gen_helper_loadrs_rse(tcg_env,
                              tcg_constant_i64(insn->address),
                              tcg_constant_i64(insn->raw),
                              tcg_constant_i32(insn->slot));
        break;
    case IA64_OP_ITR_D:
        ia64_gen_check_nat_register(insn, op->source);
        ia64_gen_check_nat_register(insn, op->register_index);
        ia64_gen_check_register_index(
            insn, ia64_gr_src(op->register_index),
            ia64_env_cpu_class(ctx->env)->dtr_count);
        ia64_gen_sync_ip_for_helper(insn);
        gen_helper_itr_insert(tcg_env, ia64_gr_src(op->source),
                               ia64_gr_src(op->register_index),
                               tcg_constant_i32(1),
                               tcg_constant_i64(insn->raw),
                               tcg_constant_i32(insn->slot));
        ctx->restart.exit_after_bundle = true;
        break;
    case IA64_OP_ITR_I:
        ia64_gen_check_nat_register(insn, op->source);
        ia64_gen_check_nat_register(insn, op->register_index);
        ia64_gen_check_register_index(
            insn, ia64_gr_src(op->register_index),
            ia64_env_cpu_class(ctx->env)->itr_count);
        ia64_gen_sync_ip_for_helper(insn);
        gen_helper_itr_insert(tcg_env, ia64_gr_src(op->source),
                               ia64_gr_src(op->register_index),
                               tcg_constant_i32(0),
                               tcg_constant_i64(insn->raw),
                               tcg_constant_i32(insn->slot));
        ia64_gen_exit_to_slot_completed(ctx, insn->address, insn->slot + 1,
                                        insn->address,
                                        record_iipa,
                                        track_psr_suppression);
        if (skip == NULL) {
            return IA64_GEN_NORETURN;
        }
        break;
    case IA64_OP_PTR_D:
        ia64_gen_check_nat_register(insn, op->register_index);
        ia64_gen_check_nat_register(insn, op->source);
        ia64_gen_sync_ip_for_helper(insn);
        gen_helper_ptr_purge(tcg_env, ia64_gr_src(op->register_index),
                              ia64_gr_src(op->source),
                              tcg_constant_i32(1));
        ctx->restart.exit_after_bundle = true;
        break;
    case IA64_OP_PTR_I:
        ia64_gen_check_nat_register(insn, op->register_index);
        ia64_gen_check_nat_register(insn, op->source);
        ia64_gen_sync_ip_for_helper(insn);
        gen_helper_ptr_purge(tcg_env, ia64_gr_src(op->register_index),
                              ia64_gr_src(op->source),
                              tcg_constant_i32(0));
        ctx->restart.exit_after_bundle = true;
        break;
    case IA64_OP_ITC_D:
        ia64_gen_check_nat_register(insn, op->source);
        ia64_gen_sync_ip_for_helper(insn);
        gen_helper_itc_insert(tcg_env, ia64_gr_src(op->source),
                              tcg_constant_i32(1),
                              tcg_constant_i64(insn->raw),
                              tcg_constant_i32(insn->slot));
        ctx->restart.exit_after_bundle = true;
        break;
    case IA64_OP_ITC_I:
        ia64_gen_check_nat_register(insn, op->source);
        ia64_gen_sync_ip_for_helper(insn);
        gen_helper_itc_insert(tcg_env, ia64_gr_src(op->source),
                              tcg_constant_i32(0),
                              tcg_constant_i64(insn->raw),
                              tcg_constant_i32(insn->slot));
        ia64_gen_exit_to_slot_completed(ctx, insn->address, insn->slot + 1,
                                        insn->address,
                                        record_iipa,
                                        track_psr_suppression);
        if (skip == NULL) {
            return IA64_GEN_NORETURN;
        }
        break;
    case IA64_OP_PTC_L:
        ia64_gen_check_nat_register(insn, op->register_index);
        ia64_gen_check_nat_register(insn, op->source);
        ia64_gen_sync_ip_for_helper(insn);
        gen_helper_ptc_purge(tcg_env, ia64_gr_src(op->register_index),
                              ia64_gr_src(op->source),
                              tcg_constant_i32(0));
        ctx->restart.exit_after_bundle = true;
        break;
    case IA64_OP_PTC_G:
        ia64_gen_check_nat_register(insn, op->register_index);
        ia64_gen_check_nat_register(insn, op->source);
        ia64_gen_sync_ip_for_helper(insn);
        gen_helper_ptc_purge(tcg_env, ia64_gr_src(op->register_index),
                              ia64_gr_src(op->source),
                              tcg_constant_i32(1));
        ctx->restart.exit_after_bundle = true;
        break;
    case IA64_OP_PTC_E:
        ia64_gen_check_nat_register(insn, op->register_index);
        ia64_gen_sync_ip_for_helper(insn);
        gen_helper_ptc_purge(tcg_env, ia64_gr_src(op->register_index),
                              tcg_constant_i64(0),
                              tcg_constant_i32(2));
        ctx->restart.exit_after_bundle = true;
        break;
    case IA64_OP_PTC_GA:
        ia64_gen_check_nat_register(insn, op->register_index);
        ia64_gen_check_nat_register(insn, op->source);
        ia64_gen_sync_ip_for_helper(insn);
        gen_helper_ptc_purge(tcg_env, ia64_gr_src(op->register_index),
                              ia64_gr_src(op->source),
                              tcg_constant_i32(3));
        ctx->restart.exit_after_bundle = true;
        break;
    case IA64_OP_NOP:
    case IA64_OP_HINT_I:
    case IA64_OP_HINT_B:
    case IA64_OP_HINT_F:
    case IA64_OP_HINT_X:
        break;
    case IA64_OP_HINT_M:
        if (insn->hint_m_reg_increment && op->register_index != 0) {
            tcg_gen_add_i64(cpu_gr[op->register_index],
                            cpu_gr[op->register_index],
                            cpu_gr[op->source]);
            ia64_gen_note_stacked_gr_write(op->register_index);
        } else if (insn->imm_base_update && op->register_index != 0) {
            tcg_gen_addi_i64(cpu_gr[op->register_index],
                             cpu_gr[op->register_index], op->immediate);
            ia64_gen_note_stacked_gr_write(op->register_index);
        }
        break;
    case IA64_OP_CLRRRB:
    case IA64_OP_CLRRRB_PR:
        gen_helper_clrrrb_rse(
            tcg_env,
            tcg_constant_i32(insn->opcode == IA64_OP_CLRRRB_PR));
        if (skip == NULL) {
            ia64_gen_exit_to_completed(ctx, next_ip, insn->address,
                                       record_iipa,
                                       track_psr_suppression);
            return IA64_GEN_NORETURN;
        }
        break;
    case IA64_OP_VMSW:
        /*
         * Models without the virtualization extensions do not decode vmsw
         * at all: the encoding is reserved and raises an Illegal Operation
         * fault regardless of privilege.  Models with the extensions treat
         * vmsw as a privileged instruction, so PSR.cpl > 0 raises a
         * Privileged Operation fault first; at cpl 0 a Virtualization
         * fault is reported because this implementation provides no
         * virtual-machine environment.
         */
        if (ia64_env_cpu_class(ctx->env)->has_virtualization) {
            ia64_gen_check_privileged(ctx, insn);
            ia64_gen_raise_exception(IA64_EXCP_VIRTUALIZATION,
                                      insn->address, insn->raw, insn->slot);
        } else {
            ia64_gen_raise_exception(IA64_EXCP_ILLEGAL,
                                      insn->address, insn->raw, insn->slot);
        }
        if (skip == NULL) {
            return IA64_GEN_NORETURN;
        }
        break;
    case IA64_OP_RUM:
    {
        TCGv_i64 value = tcg_temp_new_i64();

        tcg_gen_andi_i64(value, cpu_psr,
                         ~(op->immediate & IA64_PSR_UM_WRITABLE_MASK));
        ia64_gen_write_user_mask(value);
        ctx->restart.exit_after_bundle = true;
        break;
    }
    case IA64_OP_SUM_UM:
    {
        TCGv_i64 value = tcg_temp_new_i64();

        tcg_gen_ori_i64(value, cpu_psr,
                        op->immediate & IA64_PSR_UM_WRITABLE_MASK);
        ia64_gen_write_user_mask(value);
        ctx->restart.exit_after_bundle = true;
        break;
    }
    case IA64_OP_BRP:
        break;
    case IA64_OP_FSETC:
        ia64_gen_sync_ip_for_helper(insn);
        gen_helper_fsetc(tcg_env, tcg_constant_i32(op->auxiliary1),
                         tcg_constant_i32(op->source),
                         tcg_constant_i32(op->register_index));
        break;
    case IA64_OP_FCLRF:
        gen_helper_fclrf(tcg_env, tcg_constant_i32(op->auxiliary1));
        break;
    case IA64_OP_FCHKF: {
        TCGv_i64 taken = tcg_temp_new_i64();
        gen_helper_fchkf(taken, tcg_env, tcg_constant_i32(op->auxiliary1));
        ia64_gen_check_branch(ctx, taken, insn->address + op->immediate,
                              insn->address, record_iipa,
                              track_psr_suppression);
        break;
    }
    case IA64_OP_BREAK:
        if (op->auxiliary1 == 0 &&
            ia64_is_firmware_debug_break(ctx->env, insn->address,
                                         op->immediate)) {
            TCGv_i32 handled = tcg_temp_new_i32();
            TCGLabel *architected_break = gen_new_label();

            if (op->immediate == 0x100002) {
                gen_helper_firmware_debug_enter(
                    handled, tcg_env, tcg_constant_i64(insn->address));
            } else if (op->immediate == 0x100003) {
                gen_helper_firmware_debug_save(handled, tcg_env);
            } else {
                gen_helper_firmware_debug_restore(handled, tcg_env);
            }
            tcg_gen_brcondi_i32(TCG_COND_EQ, handled, 0,
                                architected_break);
            if (op->immediate == 0x100003) {
                ia64_gen_exit_to_completed(ctx, next_ip, insn->address,
                                           record_iipa,
                                           track_psr_suppression);
            } else {
                tcg_gen_exit_tb(NULL, 0);
            }
            gen_set_label(architected_break);
            ia64_gen_raise_exception(IA64_EXCP_BREAK, insn->address,
                                      op->immediate, insn->slot);
            return IA64_GEN_NORETURN;
        } else if (op->auxiliary1 == 0 &&
                   (op->immediate == 0x100005 ||
                    op->immediate == 0x100006) &&
                   ia64_is_sal_runtime_break(ctx->env, insn->address,
                                             op->immediate)) {
            TCGv_i32 handled = tcg_temp_new_i32();
            TCGLabel *architected_break = gen_new_label();

            if (op->immediate == 0x100005) {
                gen_helper_sal_runtime_enter(handled, tcg_env);
            } else {
                gen_helper_sal_runtime_exit(handled, tcg_env);
            }
            tcg_gen_brcondi_i32(TCG_COND_EQ, handled, 0,
                                architected_break);
            tcg_gen_exit_tb(NULL, 0);
            gen_set_label(architected_break);
            ia64_gen_raise_exception(IA64_EXCP_BREAK, insn->address,
                                      op->immediate, insn->slot);
            return IA64_GEN_NORETURN;
        } else if (op->immediate == 0x100001) {
            gen_helper_fpswa_dispatch(tcg_env);
        } else if ((op->immediate & 0x100000) &&
            ia64_is_pal_proc_break(ctx->env, insn->address)) {
            TCGv_i32 flags = tcg_temp_new_i32();
            TCGLabel *no_exit = gen_new_label();

            gen_helper_pal_dispatch(flags, tcg_env);
            tcg_gen_andi_i32(flags, flags,
                             IA64_PAL_DISPATCH_HALTED |
                             IA64_PAL_DISPATCH_EXIT_TB);
            tcg_gen_brcondi_i32(TCG_COND_EQ, flags, 0, no_exit);
            ia64_gen_exit_to_completed(ctx, next_ip, insn->address,
                                       record_iipa,
                                       track_psr_suppression);
            gen_set_label(no_exit);
        } else {
            ia64_gen_raise_exception(IA64_EXCP_BREAK, insn->address,
                                      op->immediate, insn->slot);
            if (skip == NULL) {
                return IA64_GEN_NORETURN;
            }
        }
        break;
    case IA64_OP_RFI:
        gen_helper_rfi(tcg_env, tcg_constant_i64(insn->address),
                       tcg_constant_i32(insn->slot));
        tcg_gen_lookup_and_goto_ptr();
        if (skip == NULL) {
            return IA64_GEN_NORETURN;
        }
        break;
    case IA64_OP_EPC:
        gen_helper_epc(tcg_env, tcg_constant_i64(insn->address),
                       tcg_constant_i64(insn->raw),
                       tcg_constant_i32(insn->slot));
        /*
         * A predicated epc continues this TB with a possibly-raised CPL,
         * so translation-time privilege elision must stop here.
         */
        ctx->cpl_known = false;
        if (skip == NULL) {
            ia64_gen_exit_to_completed(ctx, next_ip, insn->address,
                                       record_iipa,
                                       track_psr_suppression);
            return IA64_GEN_NORETURN;
        }
        break;
    case IA64_OP_SRLZ:
        gen_helper_tlb_serialize(tcg_env, tcg_constant_i32(1),
                                 tcg_constant_i32(1));
        ia64_gen_exit_to_slot_completed(ctx, insn->address, insn->slot + 1,
                                        insn->address,
                                        record_iipa,
                                        track_psr_suppression);
        if (skip == NULL) {
            return IA64_GEN_NORETURN;
        }
        break;
    case IA64_OP_SRLZ_D:
        gen_helper_tlb_serialize(tcg_env, tcg_constant_i32(1),
                                 tcg_constant_i32(0));
        ia64_gen_exit_to_slot_completed(ctx, insn->address, insn->slot + 1,
                                        insn->address,
                                        record_iipa,
                                        track_psr_suppression);
        if (skip == NULL) {
            return IA64_GEN_NORETURN;
        }
        break;
    case IA64_OP_MF:
    case IA64_OP_MF_A:
        tcg_gen_mb(TCG_MO_ALL);
        break;
    case IA64_OP_TPA: {
        TCGv_i64 val = tcg_temp_new_i64();

        ia64_gen_check_nat_consumption(insn, op->register_index, 0,
                                       IA64_NAT_NON_ACCESS);
        ia64_gen_sync_ip_for_helper(insn);
        gen_helper_tpa(val, tcg_env, ia64_gr_src(op->register_index));
        if (op->destination != 0) {
            ia64_gen_gr_write_nat_clear(op->destination, val);
        }
        break;
    }
    case IA64_OP_SYNC_I:
    case IA64_OP_FWB:
        break;
    case IA64_OP_TAK:
    case IA64_OP_THASH:
    case IA64_OP_TTAG: {
        TCGv_i64 result = tcg_temp_new_i64();

        if (insn->opcode == IA64_OP_TAK) {
            ia64_gen_check_nat_consumption(insn, op->register_index, 3,
                                           IA64_NAT_NON_ACCESS);
            gen_helper_tak(result, tcg_env, ia64_gr_src(op->register_index));
            if (op->destination != 0) {
                ia64_gen_gr_write_nat_clear(op->destination, result);
            }
        } else if (insn->opcode == IA64_OP_THASH) {
            gen_helper_thash(result, tcg_env, ia64_gr_src(op->register_index));
            if (op->destination != 0) {
                /*
                 * Derive the result NaT from the source before overwriting r1:
                 * thash may name the same GR for source and destination, and an
                 * earlier write would hide an unimplemented input address.
                 */
                ia64_gen_gr_nat_from_1_or_unimplemented_va(ctx, op->destination,
                                                           op->register_index);
                tcg_gen_mov_i64(cpu_gr[op->destination], result);
            }
        } else {
            gen_helper_ttag(result, tcg_env, ia64_gr_src(op->register_index));
            if (op->destination != 0) {
                /* Same source==destination ordering hazard as thash above. */
                ia64_gen_gr_nat_from_1_or_unimplemented_va(ctx, op->destination,
                                                           op->register_index);
                tcg_gen_mov_i64(cpu_gr[op->destination], result);
            }
        }
        break;
    }
    default:
        return IA64_GEN_UNHANDLED;
    }
    return IA64_GEN_CONTINUE;
}
