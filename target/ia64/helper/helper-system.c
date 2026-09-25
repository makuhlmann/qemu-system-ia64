/* IA-64 TCG helper ABI adapters for system-register operations. */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "cpu.h"
#include "exec/helper-proto.h"
#include "arch/arch.h"
#include "arch/system.h"

/*
 * hint @pause under round-robin TCG (ia64_insn_is_yielding_pause): the
 * translator has stored the state after the hint, so return to the vCPU loop,
 * which runs the next vCPU.
 */
void helper_yield(CPUIA64State *env)
{
    CPUState *cs = env_cpu(env);

    cs->exception_index = EXCP_YIELD;
    cpu_loop_exit(cs);
}

/*
 * Full-speed IP trace (debug facility).  When the environment variable
 * IA64_IPTRACE lists bundle IPs (comma-separated hex), the translator plants
 * a call to this helper on those bundles; it logs the IP and a general
 * register slice under -d int.  This is the way to observe deep firmware
 * boot states that the gdbstub cannot reach in reasonable time (a software
 * or hardware breakpoint slows the boot to POST 0x98 past several minutes).
 * Zero overhead when IA64_IPTRACE is unset.  Note bundle IPs are 16-byte
 * aligned; an objdump slot label like ...c9c is slot 2 of bundle ...c90.
 */
void helper_ia64_ip_trace(CPUIA64State *env)
{
    g_autoptr(GString) s = g_string_new(NULL);
    unsigned r;

    g_string_append_printf(s, "ia64-IPTRACE ip=%016" PRIx64 " b0=%016" PRIx64,
                           env->ip, env->br[IA64_BR_RETURN_LINK]);
    /* Return values (r8-r11), status/index (r28-r31), first stacked
     * registers (r32-r43) - the slice needed to follow firmware call
     * results and frame arguments. */
    for (r = 8; r <= 11; r++) {
        g_string_append_printf(s, " r%u=%016" PRIx64, r, env->gr[r]);
    }
    for (r = 28; r <= 43; r++) {
        g_string_append_printf(s, " r%u=%016" PRIx64, r, env->gr[r]);
    }
    qemu_log_mask(CPU_LOG_INT, "%s\n", s->str);
}

/*
 * Store-address trace (debug facility).  Planted after IA-64 stores only when
 * IA64_STTRACE is set (translation-time gate, so zero overhead otherwise).
 * IA64_STTRACE="lo:hi" (hex) logs the first N stores whose address lands in
 * [lo,hi) with the storing IA-64 IP and a slice of GPRs -- used to catch what
 * overwrites a physical window (e.g. the video-ROM body 0xC0800 under realfw).
 * IA64_STTRACE_MAX overrides the default hit cap.
 */
void helper_ia64_st_trace(CPUIA64State *env, uint64_t addr)
{
    static bool cfg_read;
    static uint64_t lo, hi;
    static unsigned cap = 16;
    static unsigned hits;

    if (!cfg_read) {
        const char *e = getenv("IA64_STTRACE");
        const char *colon = e ? strchr(e, ':') : NULL;
        const char *m = getenv("IA64_STTRACE_MAX");

        if (colon) {
            lo = strtoull(e, NULL, 16);
            hi = strtoull(colon + 1, NULL, 16);
        }
        if (m) {
            cap = (unsigned)strtoul(m, NULL, 0);
        }
        cfg_read = true;
    }
    if (hi > lo && addr >= lo && addr < hi && hits < cap) {
        hits++;
        fprintf(stderr, "ia64-STTRACE store addr=%016" PRIx64
                " ip=%016" PRIx64 " b0=%016" PRIx64 "\n",
                addr, env->ip, env->br[IA64_BR_RETURN_LINK]);
    }
}

/*
 * Region-gated single-step trace (debug facility).  IA64_TRACE="lo:hi" (hex)
 * makes the translator plant helper_ia64_trace_rec on every bundle whose IP is
 * in [lo,hi); each records a full snapshot (IP, packed predicates, all 128 GRs,
 * b0-b7) into a ring buffer (IA64_TRACE_RING entries, default 16384).  When the
 * bundle at IA64_TRACE_TRIG (hex) executes, helper_ia64_trace_dump prints the
 * last IA64_TRACE_DUMP (default 500) recorded snapshots in chronological order,
 * one-shot.  This lets the firmware's reset-decision path be observed with the
 * live compare operands that static disassembly + RSE-unwinding cannot pin.
 * Zero overhead when IA64_TRACE is unset (translation-time gate).
 */
struct ia64_trace_ent {
    uint64_t ip;
    uint64_t pr;
    uint64_t gr[IA64_GR_COUNT];
    uint64_t br[IA64_BR_COUNT];
};
static struct ia64_trace_ent *ia64_trace_ring;
static unsigned ia64_trace_sz;
static unsigned ia64_trace_pos;
static unsigned ia64_trace_cnt;

void helper_ia64_trace_rec(CPUIA64State *env)
{
    struct ia64_trace_ent *e;

    if (ia64_trace_ring == NULL) {
        const char *n = getenv("IA64_TRACE_RING");

        ia64_trace_sz = n ? (unsigned)strtoul(n, NULL, 0) : 16384;
        if (ia64_trace_sz < 16) {
            ia64_trace_sz = 16;
        }
        ia64_trace_ring = g_malloc0(sizeof(*ia64_trace_ring) * ia64_trace_sz);
    }
    e = &ia64_trace_ring[ia64_trace_pos % ia64_trace_sz];
    e->ip = env->ip;
    e->pr = ia64_system_read_pr(env);
    memcpy(e->gr, env->gr, sizeof(e->gr));
    memcpy(e->br, env->br, sizeof(e->br));
    ia64_trace_pos++;
    ia64_trace_cnt++;
}

void helper_ia64_trace_dump(CPUIA64State *env)
{
    static bool dumped;
    static unsigned seen;
    const char *sk;
    unsigned skip;
    const char *dn;
    unsigned n, dump_n, start, i, r, rmax;

    if (dumped || ia64_trace_ring == NULL) {
        return;
    }
    /* IA64_TRACE_SKIP: ignore this many trigger hits before dumping. */
    sk = getenv("IA64_TRACE_SKIP");
    skip = sk ? (unsigned)strtoul(sk, NULL, 0) : 0;
    if (seen++ < skip) {
        return;
    }
    dumped = true;
    n = ia64_trace_cnt < ia64_trace_sz ? ia64_trace_cnt : ia64_trace_sz;
    dn = getenv("IA64_TRACE_DUMP");
    dump_n = dn ? (unsigned)strtoul(dn, NULL, 0) : 500;
    if (dump_n > n) {
        dump_n = n;
    }
    /* IA64_TRACE_REGS: highest GR index to print per entry (default 47). */
    {
        const char *rr = getenv("IA64_TRACE_REGS");
        rmax = rr ? (unsigned)strtoul(rr, NULL, 0) : 47;
        if (rmax >= IA64_GR_COUNT) {
            rmax = IA64_GR_COUNT - 1;
        }
    }
    start = ia64_trace_pos - dump_n;
    fprintf(stderr, "=== IA64_TRACE dump: last %u of %u recorded, "
            "trigger ip=0x%" PRIx64 " ===\n", dump_n, ia64_trace_cnt, env->ip);
    for (i = 0; i < dump_n; i++) {
        struct ia64_trace_ent *e =
            &ia64_trace_ring[(start + i) % ia64_trace_sz];
        g_autoptr(GString) s = g_string_new(NULL);

        g_string_append_printf(s, "T ip=%010" PRIx64 " pr=%016" PRIx64
                               " b0=%010" PRIx64, e->ip, e->pr,
                               e->br[IA64_BR_RETURN_LINK]);
        for (r = 1; r <= rmax; r++) {
            g_string_append_printf(s, " r%u=%" PRIx64, r, e->gr[r]);
        }
        fprintf(stderr, "%s\n", s->str);
    }
}

/*
 * Full-speed IA-32 IP trace (debug facility).  When IA32_IPTRACE=<hex> names an
 * x86 linear IP, the translator plants a call to this helper on every x86
 * instruction (X86_GEN_INSN_START); it keeps a ring of the most recent x86 IPs
 * and, the first time the trigger IP executes, dumps that history plus the x86
 * register file under -d int.  This is how to recover the control-flow path the
 * IA-32 engine took to reach an unexpected x86 address (for example the SDV
 * "Emult" BIOS falling into its guard HLT), which the fault-only trace
 * (-d ia32_fault) cannot show.  Zero overhead when IA32_IPTRACE is unset (the
 * helper is not planted).
 */
void helper_ia32_ip_trace(CPUIA64State *env)
{
    static uint32_t ring[1024];
    static unsigned pos;
    static uint32_t trigger;
    static bool trigger_read;
    static bool dumped;
    static unsigned post;      /* IA32_IPTRACE_POST: log this many IPs after hit */
    static bool post_read;
    static unsigned post_max;
    static uint32_t below;     /* IA32_IPTRACE_BELOW: hit on first ip < this */
    static bool below_read;
    static uint32_t sample;    /* IA32_IPTRACE_SAMPLE: log every Nth ip */
    static bool sample_read;
    static unsigned sampled;
    static uint32_t minpos;    /* IA32_IPTRACE_MINPOS: arm trigger only after N insns */
    static bool minpos_read;
    uint32_t ip = (uint32_t)env->ip;

    if (!sample_read) {
        const char *e = getenv("IA32_IPTRACE_SAMPLE");
        sample = e ? (uint32_t)strtoul(e, NULL, 0) : 0;
        sample_read = true;
    }
    if (!minpos_read) {
        const char *e = getenv("IA32_IPTRACE_MINPOS");
        minpos = e ? (uint32_t)strtoul(e, NULL, 0) : 0;
        minpos_read = true;
    }
    if (sample && (pos % sample) == 0 && sampled < 400) {
        sampled++;
        fprintf(stderr,
                "ia32-IPTRACE sample #%u ip=%08x cs.base=%08x eax=%08x "
                "ebx=%08x ecx=%08x edx=%08x\n",
                pos, ip, (uint32_t)env->ia32.segs[R_CS].base,
                (uint32_t)env->ia32.regs[0], (uint32_t)env->ia32.regs[3],
                (uint32_t)env->ia32.regs[1], (uint32_t)env->ia32.regs[2]);
    }

    if (!trigger_read) {
        const char *e = getenv("IA32_IPTRACE");

        trigger = e ? (uint32_t)strtoul(e, NULL, 16) : 0;
        trigger_read = true;
    }
    if (!post_read) {
        const char *e = getenv("IA32_IPTRACE_POST");

        post_max = e ? (unsigned)strtoul(e, NULL, 0) : 0;
        post_read = true;
    }
    if (!below_read) {
        const char *e = getenv("IA32_IPTRACE_BELOW");

        below = e ? (uint32_t)strtoul(e, NULL, 16) : 0;
        below_read = true;
    }
    ring[pos++ & (ARRAY_SIZE(ring) - 1)] = ip;
    if (((trigger && ip == trigger) || (below && ip < below)) &&
        pos >= minpos && !dumped) {
        g_autoptr(GString) s = g_string_new(NULL);
        unsigned n = pos < ARRAY_SIZE(ring) ? pos : ARRAY_SIZE(ring);
        unsigned i;

        dumped = true;
        post = post_max;
        g_string_append_printf(s,
            "ia32-IPTRACE hit ip=%08x eip=%08x cs.sel=%04x cs.base=%08x "
            "ds.base=%08x ss.base=%08x "
            "eax=%08x ecx=%08x edx=%08x ebx=%08x "
            "esp=%08x ebp=%08x esi=%08x edi=%08x; preceding %u x86 IPs:",
            ip, (uint32_t)env->ia32.eip,
            (unsigned)env->ia32.segs[R_CS].selector,
            (uint32_t)env->ia32.segs[R_CS].base,
            (uint32_t)env->ia32.segs[R_DS].base,
            (uint32_t)env->ia32.segs[R_SS].base,
            (uint32_t)env->ia32.regs[0], (uint32_t)env->ia32.regs[1],
            (uint32_t)env->ia32.regs[2], (uint32_t)env->ia32.regs[3],
            (uint32_t)env->ia32.regs[4], (uint32_t)env->ia32.regs[5],
            (uint32_t)env->ia32.regs[6], (uint32_t)env->ia32.regs[7], n);
        for (i = 0; i < n; i++) {
            unsigned idx = (pos - n + i) & (ARRAY_SIZE(ring) - 1);

            g_string_append_printf(s, "%s%08x",
                                   (i % 8) ? " " : "\n  ", ring[idx]);
        }
        /*
         * stderr, not the -d int log: the dump is one-shot (guarded by !dumped)
         * plus a bounded post window, so it is rule-7-safe, and it lets this run
         * without -d int -- which would otherwise log the post-runaway fault
         * storm and fill the disk.
         */
        fprintf(stderr, "%s\n", s->str);
    } else if (dumped && post > 0) {
        post--;
        fprintf(stderr,
            "ia32-IPTRACE post ip=%08x eip=%08x cs.base=%08x esp=%08x\n",
            ip, (uint32_t)env->ia32.eip,
            (uint32_t)env->ia32.segs[R_CS].base,
            (uint32_t)env->ia32.regs[4]);
    }
}

uint64_t helper_read_pr(CPUIA64State *env)
{
    return ia64_system_read_pr(env);
}

void helper_epc(CPUIA64State *env, uint64_t fault_ip, uint64_t raw,
                uint32_t fault_slot)
{
    ia64_system_epc(env, fault_ip, raw, fault_slot);
}

void helper_write_pr(CPUIA64State *env, uint64_t value, uint64_t mask)
{
    ia64_system_write_pr(env, value, mask);
}

uint64_t helper_read_ar(CPUIA64State *env, uint32_t ar_num)
{
    return ia64_system_read_ar(env, ar_num);
}

void helper_validate_ar_access(CPUIA64State *env, uint64_t value,
                               uint32_t ar_num, uint32_t write,
                               uint64_t fault_ip, uint64_t raw,
                               uint32_t slot)
{
    ia64_system_validate_ar_access(env, value, ar_num, write,
                                   fault_ip, raw, slot);
}

void helper_write_ar(CPUIA64State *env, uint32_t ar_num, uint64_t value)
{
    ia64_system_write_ar(env, ar_num, value);
}

uint64_t helper_read_cr(CPUIA64State *env, uint32_t cr_num)
{
    return ia64_system_read_cr(env, cr_num);
}

void helper_write_cr(CPUIA64State *env, uint32_t cr_num, uint64_t value)
{
    ia64_write_cr(env, cr_num, value);
}

uint64_t helper_validate_cr_access(CPUIA64State *env, uint64_t value,
                                   uint32_t cr_num, uint32_t write,
                                   uint64_t fault_ip, uint64_t raw,
                                   uint32_t slot)
{
    return ia64_system_validate_cr_access(env, value, cr_num, write,
                                          fault_ip, raw, slot);
}

uint64_t helper_read_cpuid(CPUIA64State *env, uint64_t index)
{
    return ia64_system_read_cpuid(env, index);
}

uint64_t helper_read_dahr_indexed(CPUIA64State *env, uint64_t index)
{
    return ia64_system_read_dahr_indexed(env, index);
}

uint64_t helper_read_msr(CPUIA64State *env, uint64_t index)
{
    return ia64_system_read_msr(env, index);
}

void helper_write_msr(CPUIA64State *env, uint64_t index, uint64_t value)
{
    ia64_system_write_msr(env, index, value);
}

uint64_t helper_read_dbr(CPUIA64State *env, uint32_t index)
{
    return ia64_system_read_dbr(env, index);
}

void helper_write_dbr(CPUIA64State *env, uint32_t index, uint64_t value)
{
    ia64_system_write_dbr(env, index, value);
}

uint64_t helper_read_ibr(CPUIA64State *env, uint32_t index)
{
    return ia64_system_read_ibr(env, index);
}

void helper_write_ibr(CPUIA64State *env, uint32_t index, uint64_t value)
{
    ia64_system_write_ibr(env, index, value);
}

uint64_t helper_read_pmc(CPUIA64State *env, uint32_t index)
{
    return ia64_system_read_pmc(env, index);
}

void helper_write_pmc(CPUIA64State *env, uint32_t index, uint64_t value)
{
    ia64_system_write_pmc(env, index, value);
}

uint64_t helper_read_pmc_indexed(CPUIA64State *env, uint64_t index)
{
    return ia64_system_read_pmc_indexed(env, index);
}

void helper_write_pmc_indexed(CPUIA64State *env, uint64_t index,
                              uint64_t value)
{
    ia64_system_write_pmc_indexed(env, index, value);
}

uint64_t helper_read_pmd(CPUIA64State *env, uint32_t index)
{
    return ia64_system_read_pmd(env, index);
}

uint64_t helper_read_pmd_checked(CPUIA64State *env, uint64_t index)
{
    return ia64_system_read_pmd_checked(env, index);
}

void helper_write_pmd(CPUIA64State *env, uint32_t index, uint64_t value)
{
    ia64_system_write_pmd(env, index, value);
}

uint64_t helper_read_pmd_indexed(CPUIA64State *env, uint64_t index)
{
    return ia64_system_read_pmd_indexed(env, index);
}

void helper_write_pmd_indexed(CPUIA64State *env, uint64_t index,
                              uint64_t value)
{
    ia64_system_write_pmd_indexed(env, index, value);
}

void helper_st_spill_unat(CPUIA64State *env, uint32_t reg, uint64_t addr)
{
    ia64_system_st_spill_unat(env, reg, addr);
}

void helper_clear_psr_fault_suppression(CPUIA64State *env)
{
    ia64_system_clear_psr_fault_suppression(env);
}

void helper_set_psr_bn(CPUIA64State *env, uint32_t bank1)
{
    ia64_system_set_psr_bn(env, bank1);
}

void helper_ssm(CPUIA64State *env, uint64_t imm)
{
    ia64_system_ssm(env, imm);
}

void helper_rsm(CPUIA64State *env, uint64_t imm)
{
    ia64_system_rsm(env, imm);
}

uint64_t helper_mov_psrgr_read(CPUIA64State *env, uint32_t unused)
{
    return ia64_system_mov_psrgr_read(env, unused);
}

void helper_mov_psr_write(CPUIA64State *env, uint64_t value, uint32_t unused)
{
    ia64_system_mov_psr_write(env, value, unused);
}

uint64_t helper_mov_rrgr_read(CPUIA64State *env, uint64_t rr_addr)
{
    return ia64_system_mov_rrgr_read(env, rr_addr);
}

uint64_t helper_validate_rr_value(CPUIA64State *env, uint64_t value,
                                  uint64_t fault_ip, uint64_t raw,
                                  uint32_t slot)
{
    return ia64_system_validate_rr_value(env, value, fault_ip, raw, slot);
}

void helper_mov_grrr_write(CPUIA64State *env, uint64_t rr_addr,
                           uint64_t value)
{
    ia64_system_mov_grrr_write(env, rr_addr, value);
}

uint64_t helper_mov_pkrgr_read(CPUIA64State *env, uint32_t pkr_num)
{
    return ia64_system_mov_pkrgr_read(env, pkr_num);
}

uint64_t helper_mov_pkrgr_indexed_read(CPUIA64State *env, uint64_t pkr_num)
{
    return ia64_system_mov_pkrgr_indexed_read(env, pkr_num);
}

void helper_mov_grpkr_write(CPUIA64State *env, uint32_t pkr_num,
                            uint64_t value)
{
    ia64_system_mov_grpkr_write(env, pkr_num, value);
}

void helper_mov_grpkr_indexed_write(CPUIA64State *env, uint64_t pkr_num,
                                    uint64_t value)
{
    ia64_system_mov_grpkr_indexed_write(env, pkr_num, value);
}
