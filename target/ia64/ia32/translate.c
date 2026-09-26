/*
 * IA-32 TCG translation adapter.
 *
 * The decoder and code generator are QEMU's existing x86 implementation.
 * Redirect its CPUX86State references to the private x86 backing state
 * embedded in CPUIA64State.  Architectural register synchronization is
 * performed at instruction-set transitions.
 */

#include "qemu/osdep.h"
#include "cpu.h"
#include "ia32/ia32.h"

#define IA32_TB_FLAG_FAST   IA64_IA32_TB_FAST
#define IA32_TB_FLAG_SIMD_MASKED IA64_IA32_TB_SIMD_MASKED
#define IA32_TB_FLAG_FLAT_MASK  (0xfu << IA64_IA32_TB_FLAT_SHIFT)
#define IA32_TB_FLAG_PSR_DT (1u << 28)
#define IA32_TB_FLAG_PSR_DB (1u << 29)
#define IA32_TB_FLAG_PSR_AC (1u << 30)
#define IA32_TB_FLAG_PSR_IS (1u << 31)

/*
 * cpu_env normally returns CPUIA64State.  Decode-time reads need the
 * private x86 view instead; generated TCG accesses still use tcg_env and
 * work because that view is the first member of CPUIA64State.
 */
#define cpu_env(cpu) (&IA64_CPU(cpu)->env.ia32)
#define X86_TRANSLATOR_ENV(env) ((CPUIA64State *)(env))

#define X86_GEN_HELPER_RAISE_EXCEPTION gen_helper_ia32_raise_exception
#define X86_GEN_HELPER_RSM gen_helper_ia32_rsm
#define X86_TB_FLAGS(flags) \
    ((flags) & ~(IA32_TB_FLAG_FAST | IA32_TB_FLAG_SIMD_MASKED | \
                 IA32_TB_FLAG_FLAT_MASK | IA32_TB_FLAG_PSR_DT | \
                 IA32_TB_FLAG_PSR_DB | IA32_TB_FLAG_PSR_AC | \
                 IA32_TB_FLAG_PSR_IS))
/* ia64_ia32_tb_state(): no check below can fail or trap in this TB. */
#define IA32_FAST(s) (((s)->base.tb->flags & IA32_TB_FLAG_FAST) != 0)
/* Ordinary IA-32 #AC checks run after translation in the segment hook. */
#define X86_MEMOP_ALIGNMENT(s, memop) MO_UNALN
/*
 * Store the current x86 linear IP into CPUIA64State.ip at each instruction, and
 * -- when IA32_IPTRACE is set -- plant a per-instruction trace helper (see
 * helper_ia32_ip_trace).  The getenv check runs once at translation time and is
 * cached, so an unset IA32_IPTRACE plants nothing and costs nothing at runtime.
 */
static int ia64_ia32_iptrace_enabled = -1;
#define X86_GEN_INSN_START(pc) do {                                       \
    tcg_gen_st_i64(tcg_constant_i64((uint32_t)(pc)), tcg_env,             \
                   offsetof(CPUIA64State, ip));                           \
    if (ia64_ia32_iptrace_enabled < 0) {                                  \
        ia64_ia32_iptrace_enabled = getenv("IA32_IPTRACE") != NULL ||     \
                                    getenv("IA32_IPTRACE_BELOW") != NULL || \
                                    getenv("IA32_IPTRACE_SAMPLE") != NULL;  \
    }                                                                     \
    if (ia64_ia32_iptrace_enabled) {                                      \
        gen_helper_ia32_ip_trace(tcg_env);                               \
    }                                                                     \
} while (0)
#define X86_INT3_VECTOR(vector) ((vector) | 0x100)
#define X86_IA32_SYSTEM_ENV 1
#define X86_GEN_CODE_FETCH_CHECK(s) do {                              \
    if (!IA32_FAST(s)) {                                               \
        gen_helper_ia32_code_fetch_check(tcg_env);                     \
    }                                                                  \
} while (0)
#define X86_GEN_CPUID_SERIALIZE(s) do {                               \
    tcg_gen_mb(TCG_MO_ALL | TCG_BAR_SC);                              \
    (s)->base.is_jmp = DISAS_EOB_NEXT;                                \
} while (0)
#define X86_GEN_X87_FOP(s, fop)                                       \
    tcg_gen_st16_i32(tcg_constant_i32(fop), tcg_env,                  \
                     offsetof(CPUX86State, fpop))
#define X86_SPLIT_NEAR_PAGE_END(s) true
#define X86_CODE_FETCH_VALID(env, pc, size) \
    ia64_ia32_code_fetch_valid((env), (uint32_t)(pc), (size))
#define X86_CODE_FETCH_FAULT_PROBES_SECOND_PAGE(env, insn, pc, size) \
    ia64_ia32_code_fetch_fault_probes_second_page(                    \
        (env), (uint32_t)(insn), (uint32_t)(pc), (size))
#define X86_GEN_SEGMENT_ACCESS_CHECK(s, addr, seg, size, access) do { \
    gen_helper_ia32_segment_access(                                  \
        tcg_env, (addr), tcg_constant_i32(seg),                      \
        tcg_constant_i32(size), tcg_constant_i32(access));           \
} while (0)
/* ia64_ia32_tb_state(): the check of seg can only probe the TLB. */
#define IA32_FLAT(s, seg)                                              \
    ((unsigned)(seg) <= R_DS &&                                        \
     ((s)->base.tb->flags & (1u << (IA64_IA32_TB_FLAT_SHIFT + (seg)))))
/* The access that directly follows raises the fault the probe would. */
#define X86_GEN_SINGLE_ACCESS_CHECK(s, addr, seg, size, access) do {  \
    if (!IA32_FLAT(s, seg)) {                                          \
        X86_GEN_SEGMENT_ACCESS_CHECK(s, addr, seg, size, access);      \
    }                                                                  \
} while (0)
#define IA32_VECTOR_EA(decode, n)                                      \
    ((decode)->op[n].has_ea &&                                         \
     ((decode)->op[n].unit == X86_OP_SSE ||                            \
      (decode)->op[n].unit == X86_OP_MMX))
/*
 * The same holds when the first access of the instruction is the load of
 * the operand (gen_load()), or the store of MOV or MOVDQ.  A
 * read-modify-write keeps the probe, which reports a write fault before
 * the load, and so does POP m, whose stack load comes first.  An aligned
 * vector access checks alignment before the TLB, so a misaligned one still
 * takes the helper, whose probe reports the TLB miss first.
 */
#define X86_GEN_DECODED_ACCESS_CHECK(s, decode, addr, seg, size, access) do { \
    bool first_ = false;                                               \
    int vec_ = -1;                                                     \
                                                                       \
    if ((access) == X86_SEG_ACCESS_READ) {                             \
        first_ = ((decode)->op[1].has_ea &&                            \
                  (decode)->op[1].unit == X86_OP_INT) ||               \
                 ((decode)->op[2].has_ea &&                            \
                  (decode)->op[2].unit == X86_OP_INT);                 \
        vec_ = IA32_VECTOR_EA(decode, 1) ? 1 :                         \
               IA32_VECTOR_EA(decode, 2) ? 2 : -1;                     \
    } else if ((access) == X86_SEG_ACCESS_WRITE) {                     \
        first_ = (decode)->e.gen == gen_MOV &&                         \
                 (decode)->op[0].unit == X86_OP_INT;                   \
        vec_ = (decode)->e.gen == gen_MOVDQ &&                         \
               IA32_VECTOR_EA(decode, 0) ? 0 : -1;                     \
    }                                                                  \
    if (!IA32_FLAT(s, seg) || (!first_ && vec_ < 0)) {                 \
        X86_GEN_SEGMENT_ACCESS_CHECK(s, addr, seg, size, access);      \
    } else if (vec_ >= 0 &&                                            \
               sse_needs_alignment(s, decode, (decode)->op[vec_].ot)) { \
        TCGLabel *aligned_ = gen_new_label();                          \
        TCGv low_ = tcg_temp_new();                                    \
                                                                       \
        tcg_gen_andi_tl(low_, (addr), (size) - 1);                     \
        tcg_gen_brcondi_tl(TCG_COND_EQ, low_, 0, aligned_);            \
        X86_GEN_SEGMENT_ACCESS_CHECK(s, addr, seg, size, access);      \
        gen_set_label(aligned_);                                       \
    }                                                                  \
} while (0)
#define X86_GEN_BOUND_ACCESS_CHECK(s, addr, seg, element_size) do {   \
    gen_helper_ia32_bound_access(                                    \
        tcg_env, (addr), tcg_constant_i32(seg),                      \
        tcg_constant_i32(element_size));                              \
} while (0)
#define X86_GEN_FXSTATE_ACCESS_CHECK(s, addr, seg, access) do {       \
    gen_helper_ia32_fxstate_access(                                  \
        tcg_env, (addr), tcg_constant_i32(seg),                      \
        tcg_constant_i32(access));                                    \
} while (0)
#define X86_GEN_LOCK_INTERCEPT_CHECK(s, addr, size)                   \
    gen_helper_ia32_lock_check(tcg_env, (addr), tcg_constant_i32(size))
#define X86_GEN_TAKEN_BRANCH(s) do {                                  \
    if (!IA32_FAST(s)) {                                               \
        gen_helper_ia32_taken_branch(tcg_env);                         \
    }                                                                  \
} while (0)
#define X86_GEN_NOT_TAKEN_BRANCH(s) do {                              \
    if (!IA32_FAST(s)) {                                               \
        gen_helper_ia32_complete_instruction(tcg_env, eip_next_tl(s)); \
    }                                                                  \
} while (0)
#define X86_GEN_DISABLED_FP_CHECK(s, decode) do {                      \
    bool fp_instruction_;                                             \
                                                                      \
    if (IA32_FAST(s)) {                                                \
        break;                                                         \
    }                                                                  \
    fp_instruction_ =                                                 \
        (decode)->e.gen == gen_x87 ||                                 \
        (decode)->e.gen == gen_WAIT ||                                \
        (decode)->e.gen == gen_EMMS ||                                \
        (decode)->e.special == X86_SPECIAL_MMX ||                     \
        (decode)->e.cpuid == X86_FEAT_SSE ||                          \
        (decode)->e.cpuid == X86_FEAT_FXSR ||                         \
        (decode)->op[0].unit == X86_OP_MMX ||                         \
        (decode)->op[0].unit == X86_OP_SSE ||                         \
        (decode)->op[1].unit == X86_OP_MMX ||                         \
        (decode)->op[1].unit == X86_OP_SSE ||                         \
        (decode)->op[2].unit == X86_OP_MMX ||                         \
        (decode)->op[2].unit == X86_OP_SSE;                           \
    gen_helper_ia32_check_disabled_fp(                                \
        tcg_env, tcg_constant_i32(fp_instruction_));                  \
} while (0)
#define X86_GEN_PENDING_FP_CHECK(s, decode) do {                       \
    bool mmx_instruction_ =                                           \
        (decode)->e.gen == gen_EMMS ||                                \
        ((decode)->e.special == X86_SPECIAL_MMX &&                    \
         !((s)->prefix &                                              \
           (PREFIX_REPZ | PREFIX_REPNZ | PREFIX_DATA)));              \
    if (mmx_instruction_) {                                           \
        gen_helper_fwait(tcg_env);                                    \
    }                                                                  \
} while (0)
#define X86_IA32_SSE_INSTRUCTION(decode)                               \
    ((decode)->op[0].unit == X86_OP_SSE ||                             \
     (decode)->op[1].unit == X86_OP_SSE ||                             \
     (decode)->op[2].unit == X86_OP_SSE)
/* ia64_ia32_tb_state(): no SIMD exception can be delivered in this TB. */
#define IA32_SIMD_MASKED(s)                                            \
    (((s)->base.tb->flags & IA32_TB_FLAG_SIMD_MASKED) != 0)
#define X86_GEN_SSE_EXCEPTION_BEGIN(s, decode) do {                    \
    if (X86_IA32_SSE_INSTRUCTION(decode) && !IA32_SIMD_MASKED(s)) {    \
        gen_helper_ia32_sse_exception_begin(tcg_env);                  \
    }                                                                  \
} while (0)
#define X86_GEN_SSE_EXCEPTION_END(s, decode) do {                      \
    if (X86_IA32_SSE_INSTRUCTION(decode) && !IA32_SIMD_MASKED(s)) {    \
        gen_helper_ia32_sse_exception_end(tcg_env);                    \
    }                                                                  \
} while (0)
#define X86_REP_CAN_LOOP(s)                                            \
    (!((s)->base.tb->flags & IA32_TB_FLAG_PSR_DB))
#define X86_REP_FAULT_SETS_RF(s) true
#define X86_REP_FINAL_ITERATION_COMPLETES(s) true
#define X86_GEN_REP_ITERATION(s) do {                                  \
    if (!IA32_FAST(s)) {                                               \
        gen_helper_ia32_rep_iteration(tcg_env);                        \
    }                                                                  \
} while (0)
#define X86_GEN_REP_COMPLETE(s) do {                                   \
    if (!IA32_FAST(s)) {                                               \
        gen_helper_ia32_complete_instruction(tcg_env, eip_next_tl(s)); \
    }                                                                  \
} while (0)
/*
 * An indirect branch commits its runtime target to cpu_eip and invalidates
 * pc_save.  That target is the next IP for instruction-completion traps.
 */
#define X86_IA32_COMPLETION_EIP(s)                                     \
    ((s)->pc_save == -1 ? cpu_eip : eip_next_tl(s))
#define X86_AFTER_INSN_WRITEBACK(s, decode) do {                       \
    bool ss_load_ = ((decode)->e.gen == gen_MOV &&                     \
                     (decode)->e.op0 == X86_TYPE_S &&                  \
                     (decode)->op[0].n == R_SS) ||                     \
                    ((decode)->e.gen == gen_POP &&                     \
                     (decode)->e.op0 == X86_TYPE_SS);                  \
                                                                       \
    /* The MXCSR masks are part of the TB key (IA32_SIMD_MASKED). */    \
    if (((decode)->e.gen == gen_LDMXCSR ||                             \
         (decode)->e.gen == gen_FXRSTOR ||                             \
         (decode)->e.gen == gen_XRSTOR) &&                             \
        (s)->base.is_jmp == DISAS_NEXT) {                              \
        (s)->base.is_jmp = DISAS_EOB_NEXT;                             \
    }                                                                  \
    if (IA32_FAST(s) && !ss_load_) {                                   \
        /*                                                             \
         * Only a far transfer changes CPL (a gate or task switch      \
         * intercepts), and every one ends the TB.                     \
         */                                                            \
        if ((decode)->e.gen == gen_CALLF ||                            \
            (decode)->e.gen == gen_CALLF_m ||                          \
            (decode)->e.gen == gen_JMPF ||                             \
            (decode)->e.gen == gen_JMPF_m ||                           \
            (decode)->e.gen == gen_RETF) {                             \
            gen_helper_ia32_sync_cpl(tcg_env);                         \
        }                                                              \
        break;                                                         \
    }                                                                  \
    /* Helpers inspect lazy flags through CPUX86State.cc_op. */         \
    gen_update_cc_op(s);                                               \
    if (ss_load_) {                                                    \
        TCGv old_eflags_ = tcg_temp_new();                             \
        gen_helper_read_eflags(old_eflags_, tcg_env);                  \
        assume_cc_op(s, CC_OP_EFLAGS);                                 \
        gen_helper_ia32_system_flag(tcg_env, old_eflags_,              \
                                    tcg_constant_i32(3),               \
                                    eip_next_tl(s));                   \
    }                                                                  \
    gen_helper_ia32_complete_instruction(                              \
        tcg_env, X86_IA32_COMPLETION_EIP(s));                          \
} while (0)
#define X86_SYSTEM_INSTRUCTION_INTERCEPT(decode) \
    ((decode)->e.gen == gen_CLTS || \
     (decode)->e.gen == gen_HLT || \
     (decode)->e.gen == gen_IRET || \
     (decode)->e.gen == gen_RDMSR || \
     (decode)->e.gen == gen_RSM || \
     (decode)->e.gen == gen_SYSCALL || \
     (decode)->e.gen == gen_SYSENTER || \
     (decode)->e.gen == gen_SYSEXIT || \
     (decode)->e.gen == gen_SYSRET || \
     (decode)->e.gen == gen_SYSTEM || \
     (decode)->e.gen == gen_WRMSR || \
     ((decode)->e.gen == gen_MOV && \
      ((decode)->e.op0 == X86_TYPE_D || \
       (decode)->e.op1 == X86_TYPE_D || \
       (decode)->e.op0 == X86_TYPE_C)))
#define X86_SKIP_HELPER_INFO
#define tcg_x86_init ia64_ia32_translate_init
#define x86_translate_code ia64_ia32_translate_code

#include "target/i386/tcg/translate.c"
