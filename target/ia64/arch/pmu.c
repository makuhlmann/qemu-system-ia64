/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The generic performance counters PMC/PMD 4-7 (SDM Vol. 2 7.2.1-7.2.3).
 *
 * They count the two events every model provides and PAL_PERF_MON_INFO
 * names: processor cycles and retired instructions.  Cycles are the
 * processor clock, which the ITC runs at too; the model retires one
 * instruction per cycle.  The counts are brought up to date lazily, at every
 * change of the state that enables a counter (PSR, PMC0, PMC4-7) and before
 * every PMD access, and a timer on the virtual clock lands on the next
 * overflow that interrupts.
 */

#include "qemu/osdep.h"
#include "qemu/timer.h"
#include "cpu.h"

#define IA64_PMU_FIRST 4
#define IA64_PMU_LAST  7

static uint64_t ia64_pmu_cycles(CPUIA64State *env, int64_t ns)
{
    return ia64_itc_ns_to_ticks(env, ns);
}

/* PMC[i] selects an event this model counts on PMD[i], at some level. */
static bool ia64_pmu_configured(CPUIA64State *env, const IA64PmuLayout *pmu,
                                int i)
{
    uint64_t pmc = env->pmc[i];
    unsigned es = (pmc >> IA64_PMC_ES_SHIFT) & pmu->es_mask;

    if (!(pmc & IA64_PMC_PLM_MASK)) {
        return false;
    }
    switch (es) {
    case IA64_PMU_EVENT_CPU_CYCLES:
        return IA64_PMU_CYCLE_COUNTERS & (1u << i);
    case IA64_PMU_EVENT_INST_RETIRED:
        return ia64_env_cpu_class(env)->pal->perf_retired_mask & (1u << i);
    default:
        return false;
    }
}

/*
 * Generic Monitor Enable[i] (SDM Vol. 2 7.2.1), with the instruction set
 * mask of PMC bits 25:24 (245320-003 Figure 6-13, 251110-003 Table 10-5)
 * and, on Itanium 2, the PMU enable in PMC4 (251110-003 10.3.1).
 */
static bool ia64_pmu_enabled(CPUIA64State *env, const IA64PmuLayout *pmu,
                             int i)
{
    uint64_t pmc = env->pmc[i];
    unsigned is = (env->psr & IA64_PSR_IS) != 0;

    if (env->pmc[0] & IA64_PMC0_FR) {
        return false;
    }
    if (pmu->pmc4_enable && !(env->pmc[4] & pmu->pmc4_enable)) {
        return false;
    }
    if (!(pmc & (1ULL << ia64_psr_cpl(env->psr)))) {
        return false;
    }
    if (pmc & (1ULL << (IA64_PMC_ISM_SHIFT + is))) {
        return false;
    }
    return env->psr & ((pmc & IA64_PMC_PM) ? IA64_PSR_PP : IA64_PSR_UP);
}

/*
 * A carry out of the count sets the counter's overflow bit in PMC0 and,
 * with PMC.oi, freezes the PMU and pends the PMV vector unless PMV.m is set
 * (SDM Vol. 2 7.2.2).
 */
static void ia64_pmu_overflow(CPUIA64State *env, int i)
{
    uint64_t pmv = env->cr[IA64_CR_PMV];
    uint8_t vector = pmv & 0xff;

    env->pmc[0] |= 1ULL << i;
    if (!(env->pmc[i] & IA64_PMC_OI)) {
        return;
    }
    env->pmc[0] |= IA64_PMC0_FR;
    if (!(pmv & IA64_VECTOR_MASKED) &&
        ia64_external_interrupt_vector_valid(vector)) {
        ia64_sapic_set_irq(env_cpu(env), vector);
    }
}

static void ia64_pmu_add(CPUIA64State *env, const IA64PmuLayout *pmu, int i,
                         uint64_t events)
{
    uint64_t mask = MAKE_64BIT_MASK(0, pmu->count_bits);
    uint64_t count = (env->pmd[i] & mask) + events;
    uint64_t value = (env->pmd[i] & ~mask) | (count & mask);
    bool overflow = count > mask;

    if (overflow && pmu->overflow_bit) {
        value |= 1ULL << pmu->count_bits;
    }
    env->pmd[i] = ia64_pmu_register_value(&pmu->pmd[i], value);
    if (overflow) {
        ia64_pmu_overflow(env, i);
    }
}

void ia64_pmu_sync(CPUIA64State *env)
{
    IA64CPU *cpu = env_archcpu(env);
    const IA64PmuLayout *pmu = ia64_env_cpu_class(env)->pmu;
    uint64_t now_cycles, events;
    int64_t now, deadline = -1;
    uint8_t counting = 0;
    int i;

    if (pmu == NULL || (!env->pmu.configured && !env->pmu.counting)) {
        return;
    }
    now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    now_cycles = ia64_pmu_cycles(env, now);
    if (env->pmu.counting && now > env->pmu.sync_ns) {
        events = now_cycles - ia64_pmu_cycles(env, env->pmu.sync_ns);
        for (i = IA64_PMU_FIRST; i <= IA64_PMU_LAST; i++) {
            if (env->pmu.counting & (1u << i)) {
                ia64_pmu_add(env, pmu, i, events);
            }
        }
    }
    env->pmu.sync_ns = now;

    for (i = IA64_PMU_FIRST; i <= IA64_PMU_LAST; i++) {
        uint64_t mask = MAKE_64BIT_MASK(0, pmu->count_bits);
        uint64_t match;
        int64_t ns;

        if (!(env->pmu.configured & (1u << i)) ||
            !ia64_pmu_enabled(env, pmu, i)) {
            continue;
        }
        counting |= 1u << i;
        if (!(env->pmc[i] & IA64_PMC_OI)) {
            continue;
        }
        match = now_cycles + (mask - (env->pmd[i] & mask)) + 1;
        if (match > ia64_itc_ns_to_ticks(env, INT64_MAX)) {
            continue;
        }
        ns = ia64_itc_ticks_to_ns(env, match);
        if (deadline < 0 || ns < deadline) {
            deadline = ns;
        }
    }
    env->pmu.counting = counting;
    if (cpu->pmu_timer == NULL) {
        return;
    }
    if (deadline >= 0) {
        timer_mod(cpu->pmu_timer, deadline);
    } else {
        timer_del(cpu->pmu_timer);
    }
}

/* After a PMC write: which counters now select an event they count. */
void ia64_pmu_configure(CPUIA64State *env)
{
    const IA64PmuLayout *pmu = ia64_env_cpu_class(env)->pmu;
    uint8_t configured = 0;
    int i;

    for (i = IA64_PMU_FIRST; pmu != NULL && i <= IA64_PMU_LAST; i++) {
        if (ia64_pmu_configured(env, pmu, i)) {
            configured |= 1u << i;
        }
    }
    env->pmu.configured = configured;
}

void ia64_pmu_reset(CPUIA64State *env)
{
    IA64CPU *cpu = env_archcpu(env);

    env->pmu.counting = 0;
    env->pmu.sync_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    ia64_pmu_configure(env);
    if (cpu->pmu_timer != NULL) {
        timer_del(cpu->pmu_timer);
    }
    ia64_pmu_sync(env);
}

static void ia64_pmu_timer_work(CPUState *cs, run_on_cpu_data data)
{
    ia64_pmu_sync(&ia64_cpu_from_cpu_state(cs)->env);
}

void ia64_pmu_timer_cb(void *opaque)
{
    CPUState *cs = CPU(opaque);

    if (qemu_cpu_is_self(cs)) {
        ia64_pmu_timer_work(cs, RUN_ON_CPU_NULL);
    } else {
        async_run_on_cpu(cs, ia64_pmu_timer_work, RUN_ON_CPU_NULL);
    }
}
