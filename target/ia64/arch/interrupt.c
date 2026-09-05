/*
 * IA-64 Local SAPIC and interval timer architecture operations.
 */

#include "qemu/osdep.h"
#include "qemu/atomic.h"
#include "qemu/timer.h"
#include "cpu.h"
#include "exec/cpu-common.h"
#include "arch/arch.h"
#include "trace.h"

/* ---- Local SAPIC helpers ---- */

static int sapic_find_isr(CPUIA64State *env);

CPUState *ia64_cpu_by_sapic_id(uint8_t id, uint8_t eid)
{
    CPUState *cs;
    uint64_t lid = ia64_sapic_lid(id, eid);

    CPU_FOREACH(cs) {
        IA64CPU *cpu = ia64_cpu_from_cpu_state(cs);

        if ((qatomic_read(&cpu->env.cr[IA64_CR_SAPIC_LID]) &
             (IA64_SAPIC_LID_ID_MASK | IA64_SAPIC_LID_EID_MASK)) == lid) {
            return cs;
        }
    }
    return NULL;
}

static bool sapic_vector_active(const uint64_t bitmap[4], int vector)
{
    return bitmap[vector / 64] & (1ULL << (vector % 64));
}

static int sapic_vector_priority(int vector)
{
    if (vector == 2) {
        return 257;
    }
    if (vector == 0) {
        return 256;
    }
    if (vector >= 16) {
        return vector;
    }
    return -1;
}

static int sapic_vector_priority_class(int vector)
{
    return vector >= 16 ? vector >> 4 : -1;
}

static bool sapic_vector_unmasked(CPUIA64State *env, int vector)
{
    uint64_t tpr = env->cr[IA64_CR_SAPIC_TPR];
    int isr = sapic_find_isr(env);
    int vector_priority = sapic_vector_priority(vector);
    int vector_class = sapic_vector_priority_class(vector);

    if (vector_priority <= sapic_vector_priority(isr)) {
        return false;
    }

    if (vector == 2) {
        return true;
    }
    if (vector == 0) {
        return !(tpr & IA64_TPR_MMI);
    }
    return !(tpr & IA64_TPR_MMI) &&
           ((uint64_t)vector_class > ((tpr & IA64_TPR_MIC_MASK) >> 4));
}

static int sapic_find_highest_normal(const uint64_t bitmap[4])
{
    int word;

    for (word = 3; word >= 0; word--) {
        uint64_t active = bitmap[word];

        if (word == 0) {
            active &= ~0xffffULL;
        }
        if (active) {
            return word * 64 + 63 - clz64(active);
        }
    }
    return IA64_SPURIOUS_VECTOR;
}

static int sapic_find_irr(CPUIA64State *env)
{
    int vector;

    if (sapic_vector_active(env->interrupt.sapic_irr, 2) &&
        sapic_vector_unmasked(env, 2)) {
        return 2;
    }
    if (sapic_vector_active(env->interrupt.sapic_irr, 0) &&
        sapic_vector_unmasked(env, 0)) {
        return 0;
    }
    vector = sapic_find_highest_normal(env->interrupt.sapic_irr);
    if (vector != IA64_SPURIOUS_VECTOR &&
        sapic_vector_unmasked(env, vector)) {
        return vector;
    }
    return IA64_SPURIOUS_VECTOR;
}

static int sapic_find_isr(CPUIA64State *env)
{
    if (sapic_vector_active(env->interrupt.sapic_isr, 2)) {
        return 2;
    }
    if (sapic_vector_active(env->interrupt.sapic_isr, 0)) {
        return 0;
    }
    return sapic_find_highest_normal(env->interrupt.sapic_isr);
}

void ia64_sapic_update_interrupt(CPUIA64State *env)
{
    CPUState *cs = env_cpu(env);

    if (sapic_find_irr(env) != IA64_SPURIOUS_VECTOR) {
        cpu_set_interrupt(cs, CPU_INTERRUPT_HARD);
        qemu_cpu_kick(cs);
    } else {
        cpu_reset_interrupt(cs, CPU_INTERRUPT_HARD);
    }
}

bool ia64_sapic_has_pending(CPUIA64State *env)
{
    int vector = sapic_find_irr(env);
    return vector != IA64_SPURIOUS_VECTOR;
}

static int ia64_itv_vector(CPUIA64State *env)
{
    uint8_t vector = env->cr[IA64_CR_ITV] & 0xFF;

    return ia64_external_interrupt_vector_valid(vector) ? vector : -1;
}

static bool ia64_itv_masked(CPUIA64State *env)
{
    return env->cr[IA64_CR_ITV] & IA64_VECTOR_MASKED;
}

static bool ia64_itm_interrupt_active(CPUIA64State *env)
{
    int vector = ia64_itv_vector(env);

    return vector >= 0 && sapic_vector_active(env->interrupt.sapic_isr, vector);
}

void ia64_itc_advance_pending_itm(CPUIA64State *env)
{
    ia64_itc_sync(env);

    /*
     * Once the timer interrupt is in service, the guest has not armed the next
     * ITM yet.  Expose at least the first tick after the matched deadline so a
     * handler cannot observe the equality as still pending.
     */
    if (!env->interrupt.itm_armed &&
        env->interrupt.itm_last_match_valid &&
        ia64_itm_interrupt_active(env) &&
        (int64_t)(env->ar_itc - (env->interrupt.itm_last_match + 1)) < 0) {
        uint64_t ticks = env->interrupt.itm_last_match + 1 - env->ar_itc;

        env->ar_itc += ticks;
        env->interrupt.itc_delta += (int64_t)ticks * IA64_ITC_NS_PER_TICK;
    }
}

int ia64_sapic_accept(CPUIA64State *env)
{
    int vector = sapic_find_irr(env);
    if (vector != IA64_SPURIOUS_VECTOR) {
        int idx = vector / 64;
        int bit = vector % 64;
        env->interrupt.sapic_irr[idx] &= ~(1ULL << bit);
        env->interrupt.sapic_isr[idx] |= (1ULL << bit);
        trace_ia64_sapic_vector(env_cpu(env)->cpu_index, "accept", vector);
        ia64_sapic_update_interrupt(env);
    }
    return vector;
}

void ia64_sapic_eoi(CPUIA64State *env)
{
    int vector = sapic_find_isr(env);
    if (vector != IA64_SPURIOUS_VECTOR) {
        int idx = vector / 64;
        int bit = vector % 64;
        env->interrupt.sapic_isr[idx] &= ~(1ULL << bit);
        trace_ia64_sapic_vector(env_cpu(env)->cpu_index, "eoi", vector);
    }
    ia64_sapic_update_interrupt(env);
}

int ia64_sapic_get_ivr(CPUIA64State *env)
{
    return ia64_sapic_accept(env);
}

static void ia64_sapic_set_irq_work(CPUState *cs, run_on_cpu_data data)
{
    IA64CPU *cpu = ia64_cpu_from_cpu_state(cs);
    uint8_t vector = data.host_int;
    int idx = vector / 64;
    int bit = vector % 64;

    cpu->env.interrupt.sapic_irr[idx] |= (1ULL << bit);
    trace_ia64_sapic_vector(cs->cpu_index, "raise", vector);
    ia64_sapic_update_interrupt(&cpu->env);
}

void ia64_sapic_set_irq(CPUState *cs, uint8_t vector)
{
    run_on_cpu_data data = RUN_ON_CPU_HOST_INT(vector);

    if (qemu_cpu_is_self(cs)) {
        ia64_sapic_set_irq_work(cs, data);
    } else {
        async_run_on_cpu(cs, ia64_sapic_set_irq_work, data);
    }
}

/*
 * LINT0/LINT1: the two external interrupt pins wired straight to the
 * processor.  Each is steered by its Local Redirection Register (SDM vol 2
 * 5.8.3.9): the mask bit discards occurrences, the delivery mode picks the
 * vector to pend (ExtINT is vector 0, NMI vector 2, INT the vector field),
 * and the trigger mode decides whether the pending indication follows the
 * pin (level) or latches on an inactive-to-active edge.  On the 460GX the
 * south bridge's 8259 pair drives LINT0, which is how the SDV firmware runs
 * its legacy tick (it programs LRR0 = 0x8700: level ExtINT) and why the XP
 * HAL masks both pins (LRR = 0x10000) before it enables interrupts.
 */
#define IA64_LRR_VECTOR_MASK    0xffULL
#define IA64_LRR_DM_SHIFT       8
#define IA64_LRR_DM_MASK        7ULL
#define IA64_LRR_DM_INT         0
#define IA64_LRR_DM_NMI         4
#define IA64_LRR_DM_EXTINT      7
#define IA64_LRR_TM             (1ULL << 15)
#define IA64_LRR_M              (1ULL << 16)

static int ia64_lrr_vector(uint64_t lrr)
{
    switch ((lrr >> IA64_LRR_DM_SHIFT) & IA64_LRR_DM_MASK) {
    case IA64_LRR_DM_INT: {
        int vector = lrr & IA64_LRR_VECTOR_MASK;

        return ia64_external_interrupt_vector_valid(vector) ? vector : -1;
    }
    case IA64_LRR_DM_NMI:
        return 2;
    case IA64_LRR_DM_EXTINT:
        return 0;
    default:
        /* PMI and INIT delivery through a LINT pin are not modelled. */
        return -1;
    }
}

static void ia64_lint_update(CPUIA64State *env, int pin, bool rising)
{
    uint64_t lrr = env->cr[IA64_CR_LRR0 + pin];
    int vector = ia64_lrr_vector(lrr);
    bool level = (lrr & IA64_LRR_TM) != 0;

    if (vector < 0) {
        return;
    }
    if (level) {
        /*
         * A level pin pends its vector while asserted and unmasked and
         * withdraws it otherwise (deassertion clears the indication, and a
         * masked pin's occurrences are not pended).
         */
        if (env->interrupt.lint_level[pin] && !(lrr & IA64_LRR_M)) {
            env->interrupt.sapic_irr[vector / 64] |= 1ULL << (vector % 64);
        } else {
            env->interrupt.sapic_irr[vector / 64] &= ~(1ULL << (vector % 64));
        }
    } else if (rising && !(lrr & IA64_LRR_M)) {
        env->interrupt.sapic_irr[vector / 64] |= 1ULL << (vector % 64);
    }
    ia64_sapic_update_interrupt(env);
}

void ia64_lint_lrr_written(CPUIA64State *env, int pin)
{
    ia64_lint_update(env, pin, false);
}

static void ia64_cpu_set_lint_work(CPUState *cs, run_on_cpu_data data)
{
    IA64CPU *cpu = ia64_cpu_from_cpu_state(cs);
    int pin = data.host_int >> 1;
    bool level = data.host_int & 1;
    bool rising = level && !cpu->env.interrupt.lint_level[pin];

    cpu->env.interrupt.lint_level[pin] = level;
    ia64_lint_update(&cpu->env, pin, rising);
}

void ia64_cpu_set_lint(CPUState *cs, int pin, int level)
{
    run_on_cpu_data data = RUN_ON_CPU_HOST_INT((pin << 1) | !!level);

    if (qemu_cpu_is_self(cs)) {
        ia64_cpu_set_lint_work(cs, data);
    } else {
        async_run_on_cpu(cs, ia64_cpu_set_lint_work, data);
    }
}

static void ia64_itm_raise(CPUIA64State *env, uint64_t itm_value)
{
    IA64CPU *cpu = container_of(env, IA64CPU, env);
    int vector = ia64_itv_vector(env);

    if (vector < 0) {
        return;
    }

    if (env->interrupt.itm_last_match_valid &&
        env->interrupt.itm_last_match == itm_value) {
        ia64_sapic_update_interrupt(env);
        return;
    }

    env->interrupt.itm_last_match = itm_value;
    env->interrupt.itm_last_match_valid = true;
    trace_ia64_itm(CPU(cpu)->cpu_index, "fire", env->ar_itc, itm_value,
                   vector);
    ia64_sapic_set_irq(CPU(cpu), vector);
}

static bool ia64_itm_update_pending(CPUIA64State *env, uint64_t itc,
                                    uint64_t itm_value, bool was_armed)
{
    int64_t delta_ticks = (int64_t)(itm_value - itc);
    int vector = ia64_itv_vector(env);

    if (vector < 0) {
        return true;
    }

    if (ia64_itv_masked(env)) {
        return true;
    }

    if (delta_ticks > 0) {
        return false;
    }

    /*
     * The architecture raises an interval timer interrupt when ITC equals
     * ITM.  A freshly programmed value already behind the current ITC has
     * missed that equality and must not synthesize an interrupt.  If this
     * value was armed while it was still in the future, a late QEMU timer
     * callback still represents the equality that elapsed in virtual time.
     */
    if (delta_ticks == 0 || was_armed) {
        ia64_itm_raise(env, itm_value);
    }
    return true;
}

static void ia64_itm_timer_work(CPUState *cs, run_on_cpu_data data)
{
    IA64CPU *cpu = ia64_cpu_from_cpu_state(cs);
    uint64_t itm;
    bool was_armed;

    (void)data;
    if (!cpu->env.interrupt.itm_armed) {
        return;
    }
    was_armed = cpu->env.interrupt.itm_armed;
    cpu->env.interrupt.itm_armed = false;
    itm = cpu->env.cr[IA64_CR_ITM];

    ia64_itc_advance_pending_itm(&cpu->env);

    if (!ia64_itm_update_pending(&cpu->env, cpu->env.ar_itc, itm,
                                 was_armed)) {
        ia64_itm_update(&cpu->env, itm);
    }
}

void ia64_itm_timer_cb(void *opaque)
{
    CPUState *cs = CPU(opaque);

    if (qemu_cpu_is_self(cs)) {
        ia64_itm_timer_work(cs, RUN_ON_CPU_NULL);
    } else {
        async_run_on_cpu(cs, ia64_itm_timer_work, RUN_ON_CPU_NULL);
    }
}

void ia64_itm_update(CPUIA64State *env, uint64_t itm_value)
{
    IA64CPU *cpu = container_of(env, IA64CPU, env);
    uint64_t itc;
    int64_t delta_ticks;
    int64_t delay_ns;
    int64_t deadline_ns;
    bool was_armed = env->interrupt.itm_armed &&
                     env->interrupt.itm_armed_value == itm_value;

    ia64_itc_advance_pending_itm(env);
    itc = env->ar_itc;
    delta_ticks = (int64_t)(itm_value - itc);

    timer_del(cpu->itm_timer);
    env->interrupt.itm_armed = false;

    if (ia64_itm_update_pending(env, itc, itm_value, was_armed)) {
        ia64_itc_advance_pending_itm(env);
        return;
    }

    if (delta_ticks > INT64_MAX / IA64_ITC_NS_PER_TICK) {
        delay_ns = INT64_MAX;
    } else {
        delay_ns = delta_ticks * IA64_ITC_NS_PER_TICK;
    }
    env->interrupt.itm_armed = true;
    env->interrupt.itm_armed_value = itm_value;
    deadline_ns = delay_ns > INT64_MAX - env->interrupt.itc_delta ?
                  INT64_MAX : env->interrupt.itc_delta + delay_ns;
    trace_ia64_itm(CPU(cpu)->cpu_index, "arm", itc, itm_value,
                   ia64_itv_vector(env));
    timer_mod(cpu->itm_timer, deadline_ns);
}

void ia64_itc_sync(CPUIA64State *env)
{
    int64_t now = ia64_itc_clock_ns();
    int64_t elapsed = now - env->interrupt.itc_delta;

    if (elapsed > 0) {
        uint64_t ticks = (uint64_t)(elapsed / IA64_ITC_NS_PER_TICK);

        if (ticks != 0) {
            env->ar_itc += ticks;
            env->interrupt.itc_delta += (int64_t)ticks * IA64_ITC_NS_PER_TICK;
        }
    }
}

void ia64_itc_check_timer(CPUIA64State *env)
{
    bool was_armed;

    ia64_itc_advance_pending_itm(env);
    was_armed = env->interrupt.itm_armed &&
                env->interrupt.itm_armed_value == env->cr_itm;

    if (was_armed && (int64_t)(env->cr_itm - env->ar_itc) <= 0) {
        env->interrupt.itm_armed = false;
    }
    ia64_itm_update_pending(env, env->ar_itc, env->cr_itm, was_armed);
    ia64_itc_advance_pending_itm(env);
}

void ia64_itc_enter_halt(CPUIA64State *env)
{
    ia64_itc_advance_pending_itm(env);
    ia64_itm_update(env, env->cr_itm);
}

bool ia64_cpu_has_work(CPUState *cs)
{
    IA64CPU *cpu = ia64_cpu_from_cpu_state(cs);
    CPUIA64State *env = &cpu->env;
    bool nmi_pending = (env->interrupt.sapic_irr[0] & (1ULL << 2)) != 0;
    bool interrupts_enabled = (env->psr & IA64_PSR_I) ||
                              (cs->halted && env->interrupt.pal_halt_wake);

    /*
     * ia64_sapic_update_interrupt() maintains CPU_INTERRUPT_HARD whenever
     * IRR, ISR or TPR changes, and the ITM callback does the same when its
     * deadline expires.  Do not rescan IRR or reschedule the timer from this
     * exec-loop hot path.  PAL_HALT_LIGHT wakes only for an interrupt that
     * is actually deliverable; PSR.i does not mask NMI vector 2.
     */
    return cpu_test_interrupt(cs, CPU_INTERRUPT_HARD) &&
           (interrupts_enabled || nmi_pending);
}


uint64_t ia64_interrupt_itc_read(CPUIA64State *env)
{
    return ia64_itc_read(env);
}
