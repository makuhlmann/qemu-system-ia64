/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Internal interfaces shared by the IA-64 TCG translation modules.
 */

#ifndef TARGET_IA64_TRANSLATE_TRANSLATE_H
#define TARGET_IA64_TRANSLATE_TRANSLATE_H

#include "exec/translator.h"
#include "tcg/tcg.h"

#include "target/ia64/cpu.h"
#include "target/ia64/decode/operand.h"

#define IA64_DISAS_EXIT DISAS_TARGET_0

#define IA64_TB_FLAG_DT           (1u << 0)
#define IA64_TB_FLAG_IT           (1u << 1)
#define IA64_TB_FLAG_RI_SHIFT     2
#define IA64_TB_FLAG_RI_MASK      (3u << IA64_TB_FLAG_RI_SHIFT)
#define IA64_TB_FLAG_PSR_SUPPRESS (1u << 4)
#define IA64_TB_FLAG_PSR_IC       (1u << 5)
#define IA64_TB_FLAG_BE           (1u << 6)
#define IA64_TB_FLAG_GROUP_START  (1u << 7)
#define IA64_TB_FLAG_PSR_AC       (1u << 8)
#define IA64_TB_FLAG_PSR_SS       (1u << 11)
#define IA64_TB_FLAG_PSR_TB       (1u << 12)
/* No GR NaT bit is set at TB entry. */
#define IA64_TB_FLAG_NAT_CLEAR    (1u << 13)
/* PSR.dfl (bit 18) at TB entry. */
#define IA64_TB_FLAG_PSR_DFL      (1u << 14)

/* The cs_base of an IA-64 TB holds CFM.sof and CFM.sol at entry. */
#define IA64_TB_CS_BASE_SOL_SHIFT 8

/* NaT bits of r0-r31 in the first word of the GR NaT file. */
#define IA64_STATIC_GR_NAT_MASK   0xffffffffULL
/*
 * PSR.dt selects the memory index of an IA-32 TB (ia64_cpu_mmu_index).
 * Bit 28 is HF_AVX_EN, which the IA-32 engine never sets.
 */
#define IA64_TB_FLAG_IA32_PSR_DT  (1u << 28)
#define IA64_TB_FLAG_IA32_PSR_DB  (1u << 29)
#define IA64_TB_FLAG_IA32_PSR_AC  (1u << 30)
#define IA64_TB_FLAG_PSR_IS       (1u << 31)
#define IA64_TB_FLAG_CPL_SHIFT    9
#define IA64_TB_FLAG_CPL_MASK     (3u << IA64_TB_FLAG_CPL_SHIFT)

typedef struct IA64TranslationMemoryState {
    int mmu_idx;
    bool be_data;
    /* PSR.ac at TB entry: valid for alignment checks until psr_ac_modified. */
    bool psr_ac;
    bool full_alat;
    uint64_t nat_known_clear[2];
    /* nat_known_clear where the current instruction may leave the TB. */
    uint64_t nat_known_at_exit[2];
    /* nat_known_clear before the current instruction. */
    uint64_t nat_known_before[2];
} IA64TranslationMemoryState;

typedef struct IA64TranslationRestartState {
    uint8_t start_slot;
    /*
     * PSR.ri as generated code has stored it (current_ri, when known), and
     * the restart point it stands for (logical_ri): the next instruction
     * boundary.  Only code that can observe PSR.ri gets the two synced.
     */
    uint8_t current_ri;
    bool current_ri_known;
    uint8_t logical_ri;
    bool track_iipa;
    /*
     * Set once an ssm/rsm/mov-psr in the current bundle may have changed
     * PSR.ic, so the IIPA note for a later slot cannot trust the TB flag and
     * must test PSR.ic at run time.  While false, PSR.ic equals the TB flag.
     */
    bool psr_ic_modified;
    /*
     * Set once an ssm/rsm/sum/rum/mov-psr in the current bundle may have
     * changed PSR.ac, so a later ld/st in the same bundle cannot trust the TB
     * flag and must test PSR.ac at run time.  While false, PSR.ac == psr_ac.
     */
    bool psr_ac_modified;
    bool track_psr_suppression;
    bool exit_after_bundle;
    bool instruction_group_start;
    bool next_instruction_group_start;
} IA64TranslationRestartState;

typedef struct IA64TranslationBranchState {
    uint8_t goto_tb_slots;
    TCGLabel *counted_self_label;
    TCGv_i64 counted_self_budget;
    uint64_t counted_self_ip;
    /* NaT-known facts at the label, and whether the back edge rotates. */
    uint64_t counted_self_nat_known[2];
    bool counted_self_rotates;
    /* The frame and PSR.dfl facts at the label. */
    bool counted_self_frame_known;
    uint8_t counted_self_frame_sof;
    uint8_t counted_self_frame_sol;
    bool counted_self_dfl_known;
    bool counted_self_dfl;
    bool cloop_zero_st1_valid;
    bool cloop_zero_st1_release;
    uint8_t cloop_zero_st1_base;
    uint8_t cloop_zero_st1_slot;
} IA64TranslationBranchState;

typedef struct DisasContext {
    DisasContextBase base;
    CPUIA64State *env;
    IA64TranslationMemoryState memory;
    IA64TranslationRestartState restart;
    IA64TranslationBranchState branch;
    /*
     * PSR.cpl is part of the TB flags (it selects the mmu index), so a TB
     * only ever executes at the privilege level it was translated for and
     * privilege checks can be resolved at translation time.  The single
     * instruction that can change CPL without leaving the TB is a
     * predicated epc (the unpredicated form exits); it clears cpl_known
     * for the rest of the TB.
     */
    uint8_t cpl;
    bool cpl_known;
    /*
     * CFM.sof loaded by the most recent stacked-register frame check
     * whose defining load certainly executed (or installed as a constant
     * by alloc).  Valid until an instruction that can change the frame.
     */
    TCGv_i32 cfm_sof;
    bool cfm_sof_valid;
    /*
     * Largest SOF a dominating, certainly-executed frame check has already
     * proven in this TB.  Checks for the same or a smaller SOF need no new
     * branch or exception path.  Reset whenever CFM.SOF may change.
     */
    uint8_t cfm_sof_checked;
    /*
     * CFM.sof and CFM.sol here.  They are in the TB key, so they are known
     * at entry and after alloc and cover.  Another instruction that can
     * change the frame clears frame_known: from then on the frame checks
     * load CFM.sof, and no exit takes a goto_tb link, because the TB it
     * reaches was chosen for one frame.
     */
    bool frame_known;
    uint8_t frame_sof;
    uint8_t frame_sol;
    /*
     * Every instruction so far in the TB is one of
     * ia64_insn_keeps_tb_key(): the key state at an exit is then fixed by
     * the TB's own key, and a srlz.d may link to the next TB.
     */
    bool key_static;
    /*
     * An instruction in the TB set part of the TB key to a run-time value
     * (ia64_insn_sets_tb_key_at_run_time()): exits after it look the next
     * TB up, because a link was chosen for one value.
     */
    bool key_dynamic;
    /*
     * PSR.dfl here, while psr_dfl_known: it is in the TB key, and only
     * ssm/rsm (followed exactly), mov psr.l and a break into firmware
     * change it inside a TB.
     */
    bool psr_dfl_known;
    bool psr_dfl;
    /*
     * PSR.ss and PSR.tb at TB entry.  Either one makes the TB translate a
     * single instruction, in slot trap_slot, and note its completion traps
     * (ia64_completion_trap_note()).
     */
    bool psr_ss;
    bool psr_tb;
    uint8_t trap_slot;
} DisasContext;

typedef enum IA64GenResult {
    IA64_GEN_UNHANDLED,
    IA64_GEN_CONTINUE,
    IA64_GEN_NORETURN,
} IA64GenResult;

typedef enum Ia64NatConsumptionKind {
    IA64_NAT_ACCESS,
    IA64_NAT_NON_ACCESS,
} Ia64NatConsumptionKind;

typedef enum IA64FPRegisterLoadFormat {
    IA64_FP_REGISTER_LOAD_DOUBLE,
    IA64_FP_REGISTER_LOAD_SINGLE,
    IA64_FP_REGISTER_LOAD_SIGNIFICAND,
} IA64FPRegisterLoadFormat;

typedef enum IA64BranchTargetKind {
    IA64_BRANCH_TARGET_DIRECT,
    IA64_BRANCH_TARGET_TCG,
    IA64_BRANCH_TARGET_CURRENT,
} IA64BranchTargetKind;

typedef struct IA64BranchCompletion {
    IA64BranchTargetKind target_kind;
    uint64_t direct_target;
    TCGv_i64 tcg_target;
    uint64_t completed_ip;
    bool record_iipa;
    bool track_psr_suppression;
} IA64BranchCompletion;

extern TCGv_i64 cpu_ip;
extern TCGv_i64 cpu_gr[IA64_GR_COUNT];
extern TCGv_i64 cpu_pr[IA64_PR_COUNT];
extern TCGv_i64 cpu_br[IA64_BR_COUNT];
extern TCGv_i64 cpu_psr;

TCGv_i64 ia64_gr_src(uint8_t reg);
TCGv_i64 ia64_gen_fr_sig_read(uint8_t reg);
TCGv_i64 ia64_fr_significand_src(uint8_t reg);
TCGv_i64 ia64_gen_fr_nat_read(uint8_t reg);
TCGv_i64 ia64_gen_gr_nat_read(uint8_t reg);
void ia64_gen_gr_nat_clear(uint8_t reg);
void ia64_gen_alloc(DisasContext *ctx, const Ia64Instruction *insn,
                    uint8_t r1, uint32_t sof, uint32_t sol, uint32_t sor);
void ia64_gen_br_call(DisasContext *ctx, uint8_t link, uint64_t next_ip,
                      TCGv_i64 target);
void ia64_gen_gr_nat_set(uint8_t reg);
void ia64_gen_gr_nat_assign(uint8_t reg, TCGv_i64 bit);
void ia64_gen_gr_nat_from_1(uint8_t dst, uint8_t src);
void ia64_gen_gr_nat_from_2(uint8_t dst, uint8_t src1, uint8_t src2);
bool ia64_nat_result_is_known_clear(const DisasContext *ctx,
                                    const Ia64Instruction *insn);
void ia64_gen_fr_nat_from_gr(uint8_t dst, uint8_t src);
void ia64_gen_fr_mov(uint8_t reg, TCGv_i64 value);
void ia64_gen_fr_mov_sig(uint8_t reg, TCGv_i64 value);
void ia64_gen_fr_load(uint8_t reg, TCGv_i64 addr, int mmu_idx, MemOp memop,
                      IA64FPRegisterLoadFormat format);
void ia64_gen_fr_set_nat(uint8_t reg);
void ia64_gen_predicate_test_write(const Ia64Instruction *insn,
                                   TCGv_i64 cond, TCGv_i64 not_cond);
void ia64_gen_gr_write_nat_clear(uint8_t reg, TCGv_i64 value);
void ia64_gen_check_nat_register(const Ia64Instruction *insn, uint8_t reg);
void ia64_gen_check_nat_consumption(const Ia64Instruction *insn,
                                    uint8_t reg, uint64_t isr_access,
                                    Ia64NatConsumptionKind kind);
void ia64_gen_gr_nat_from_1_or_unimplemented_va(DisasContext *ctx,
                                                uint8_t dst, uint8_t src);
MemOp ia64_data_memop(DisasContext *ctx, MemOp memop);

/*
 * The window in which the model handles a misaligned reference with
 * PSR.ac = 0 (IA64CPUClass.unaligned_windows); window 0 means that only a
 * 4 KiB crossing faults.  span is the number of bytes the reference covers.
 */
typedef struct IA64UnalignedWindow {
    uint32_t window;
    uint32_t span;
    bool uc_crosses_8;      /* a UC/WC target also faults across 8 bytes */
} IA64UnalignedWindow;

IA64UnalignedWindow ia64_unaligned_window(const Ia64Instruction *insn,
                                          uint32_t size);
void ia64_gen_check_alignment_access(const Ia64Instruction *insn,
                                     TCGv_i64 addr, uint32_t size,
                                     bool always_fault,
                                     uint64_t isr_access);
void ia64_gen_check_alignment(const Ia64Instruction *insn, TCGv_i64 addr,
                              uint32_t size, bool always_fault,
                              bool is_write);
void ia64_gen_invalidate_alat_store(DisasContext *ctx, TCGv_i64 addr,
                                    uint32_t size);
void ia64_gen_check_fr_nat_consumption(const Ia64Instruction *insn,
                                       uint8_t reg, uint64_t isr_access);
void ia64_gen_memory_acquire(const Ia64Instruction *insn);
void ia64_gen_memory_release(const Ia64Instruction *insn);
bool ia64_ar_is_simple(uint32_t ar);
void ia64_gen_read_simple_ar(TCGv_i64 value, uint32_t ar);
void ia64_gen_write_simple_ar(uint32_t ar, TCGv_i64 value);
void ia64_gen_validate_ar_access(const Ia64Instruction *insn,
                                 TCGv_i64 value, bool write);
bool ia64_ar_access_reads_clock(uint32_t ar_num);
bool ia64_clock_access_needs_io(const DisasContext *ctx);
void ia64_gen_check_pfs_write(const Ia64Instruction *insn, TCGv_i64 value);
void ia64_gen_check_cr_read(const Ia64Instruction *insn);
void ia64_gen_validate_tpr_write(TCGv_i64 result, const Ia64Instruction *insn,
                                 TCGv_i64 value);
bool ia64_gen_validate_interruption_cr_write(TCGv_i64 result,
                                             const Ia64Instruction *insn,
                                             TCGv_i64 value);
void ia64_gen_check_rse_ar_mode(const Ia64Instruction *insn, TCGv_i64 value,
                                bool write);
void ia64_gen_validate_cr_access(TCGv_i64 result,
                                 const Ia64Instruction *insn,
                                 TCGv_i64 value, bool write);
bool ia64_cr_is_read_only(uint32_t cr_num);
bool ia64_cr_write_reads_clock(uint32_t cr_num);
void ia64_gen_raise_exception(uint32_t exception, uint64_t fault_ip,
                              uint64_t fault_imm, uint32_t fault_slot);
void ia64_gen_check_register_index(const Ia64Instruction *insn,
                                   TCGv_i64 index, uint32_t count);
void ia64_gen_check_reserved_bits(const Ia64Instruction *insn,
                                  TCGv_i64 value, uint64_t allowed);
void ia64_gen_write_user_mask(TCGv_i64 value);
void ia64_gen_exit_to_completed(DisasContext *ctx, uint64_t ip,
                                uint64_t completed_ip, bool record_iipa,
                                bool track_psr_suppression);
void ia64_gen_exit_to_slot_completed(DisasContext *ctx, uint64_t ip,
                                     uint8_t slot, uint64_t completed_ip,
                                     bool record_iipa,
                                     bool track_psr_suppression);
void ia64_note_tb_key_effect(DisasContext *ctx, const Ia64Instruction *insn);
void ia64_gen_link_or_exit_slot_completed(DisasContext *ctx, uint64_t ip,
                                          uint8_t slot,
                                          uint64_t completed_ip,
                                          bool record_iipa,
                                          bool track_psr_suppression,
                                          TCGv_i32 main_loop);
void ia64_gen_exit_or_lookup_slot_completed(DisasContext *ctx, uint64_t ip,
                                            uint8_t slot,
                                            uint64_t completed_ip,
                                            bool record_iipa,
                                            bool track_psr_suppression,
                                            TCGv_i32 main_loop);
bool ia64_insn_is_yielding_pause(const DisasContext *ctx,
                                 const Ia64Instruction *insn);
void ia64_gen_yield_to_slot_completed(DisasContext *ctx, uint64_t ip,
                                      uint8_t slot, uint64_t completed_ip,
                                      bool record_iipa,
                                      bool track_psr_suppression);
void ia64_gen_sync_ip_for_helper(const Ia64Instruction *insn);
void ia64_gen_note_stacked_gr_write(uint8_t reg);
void ia64_update_frame_tracking(DisasContext *ctx,
                                const Ia64Instruction *insn);
bool ia64_insn_must_start_group(const Ia64Instruction *insn);
bool ia64_insn_must_end_group(const Ia64Instruction *insn);
bool ia64_insn_requires_slot2(const Ia64Instruction *insn);
bool ia64_insn_has_invalid_fp_pair(const Ia64Instruction *insn);
bool ia64_insn_has_illegal_register(const Ia64Instruction *insn);
bool ia64_insn_has_reserved_mask_field(const Ia64Instruction *insn);
bool ia64_insn_is_empty_hint(const Ia64Instruction *insn);
void ia64_prepare_self_counted_loop(
    DisasContext *ctx, uint8_t template_code,
    const IA64TemplateInfo *template_info, uint64_t *slots,
    uint64_t bundle_ip);
void ia64_gen_advance_restart_point(DisasContext *ctx, uint64_t bundle_ip,
                                    uint8_t slot, bool mlx_long);
void ia64_gen_set_ri_tracked(DisasContext *ctx, uint8_t slot);
void ia64_gen_clear_ri(void);
void ia64_gen_save_fault_slot_for_exit(DisasContext *ctx);
void ia64_gen_store_instruction_group_start(bool group_start);
void ia64_gen_goto_tb_group(DisasContext *ctx, uint64_t dest,
                            bool group_start);
typedef enum IA64PureKind {
    IA64_PURE_NONE,
    IA64_PURE_GR,
    IA64_PURE_PR,
} IA64PureKind;

IA64PureKind ia64_integer_pure_kind(const Ia64Instruction *insn);
void ia64_update_nat_known(DisasContext *ctx,
                           const Ia64Instruction *insn);
bool ia64_gen_insn(DisasContext *ctx, const Ia64Instruction *insn,
                   bool record_iipa);
void ia64_gen_check_privileged(DisasContext *ctx,
                               const Ia64Instruction *insn);
void ia64_gen_check_branch(DisasContext *ctx, TCGv_i64 failed,
                           uint64_t target, uint64_t completed_ip,
                           bool record_iipa,
                           bool track_psr_suppression);
bool ia64_is_pal_proc_break(CPUIA64State *env, uint64_t address);
bool ia64_is_pal_reset_return_break(CPUIA64State *env, uint64_t address);
bool ia64_is_firmware_debug_break(CPUIA64State *env, uint64_t address,
                                  uint64_t imm);
bool ia64_is_sal_runtime_break(CPUIA64State *env, uint64_t address,
                               uint64_t imm);

bool ia64_gen_completed_direct_branch(DisasContext *ctx, TCGLabel *skip,
                                      uint64_t target,
                                      uint64_t completed_ip,
                                      bool record_iipa,
                                      bool track_psr_suppression);
void ia64_gen_lookup_tcg_completed(DisasContext *ctx, TCGv_i64 ip,
                                   uint64_t completed_ip, bool record_iipa,
                                   bool track_psr_suppression);
bool ia64_gen_zero_st1_cloop(DisasContext *ctx,
                             const Ia64Instruction *insn,
                             uint64_t target, TCGLabel *l_nobr,
                             bool record_iipa,
                             bool track_psr_suppression);
bool ia64_gen_self_counted_loop(DisasContext *ctx, uint64_t target,
                                uint64_t completed_ip, bool record_iipa,
                                bool track_psr_suppression);
void ia64_gen_goto_completed(DisasContext *ctx, uint64_t ip,
                             uint64_t completed_ip, bool record_iipa,
                             bool track_psr_suppression);
void ia64_gen_lookup_current_completed(DisasContext *ctx,
                                       uint64_t completed_ip,
                                       bool record_iipa,
                                       bool track_psr_suppression);
IA64GenResult ia64_gen_complete_branch(
    DisasContext *ctx, TCGLabel *skip,
    const IA64BranchCompletion *completion);

IA64GenResult ia64_gen_branch(DisasContext *ctx,
                              const Ia64Instruction *insn,
                              TCGLabel *skip, bool record_iipa,
                              bool track_psr_suppression);
IA64GenResult ia64_gen_integer(DisasContext *ctx,
                               const Ia64Instruction *insn);
IA64GenResult ia64_gen_system(DisasContext *ctx,
                              const Ia64Instruction *insn,
                              TCGLabel *skip, bool record_iipa,
                              bool track_psr_suppression);
IA64GenResult ia64_gen_memory(DisasContext *ctx,
                              const Ia64Instruction *insn,
                              TCGLabel *skip, bool record_iipa,
                              bool track_psr_suppression);
IA64GenResult ia64_gen_fp(DisasContext *ctx,
                          const Ia64Instruction *insn);
IA64GenResult ia64_gen_simd(DisasContext *ctx,
                            const Ia64Instruction *insn);
void ia64_translate_code(CPUState *cs, TranslationBlock *tb,
                         int *max_insns, vaddr pc, void *host_pc);
void ia64_translate_init(void);

#endif /* TARGET_IA64_TRANSLATE_TRANSLATE_H */
