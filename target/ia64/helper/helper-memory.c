/* IA-64 TCG helper ABI adapters for atomic memory operations. */

#include "qemu/osdep.h"
#include "cpu.h"
#include "exec/helper-proto.h"
#include "arch/arch.h"
#include "exec-access.h"

/*
 * Report whether a misaligned data reference is exempt from the alignment
 * fault that PSR.ac (== EFLAG.ac) would otherwise raise.
 *
 * SDM Vol.2, "I/O port space": unaligned references to the I/O port space are
 * NOT detected as Unaligned Data Reference / IA-32 AlignmentCheck faults,
 * regardless of PSR.ac; that space is mandated uncacheable (UC).  More
 * generally, the platform (chipset / QEMU MMIO handler) decomposes an unaligned
 * access to a non-writeback target, so only writeback-cacheable RAM -- which
 * QEMU would otherwise satisfy silently -- needs the software alignment check.
 * Exempt every non-writeback-RAM (UC / MMIO) target from the PSR.ac fault.
 *
 * The probe fills the DTLB and can raise a translation/permission fault -- or,
 * for a NaT page, a NaT Consumption fault -- which correctly outranks the
 * Unaligned Data Reference fault (SDM Vol.2, Table 5-3).  Probe with the real
 * access direction so any such fault records the correct ISR.r/ISR.w bit.
 * Semaphore and ld16/st16 alignment (which fault regardless of PSR.ac) is
 * handled at the always-fault site and never reaches here.
 */
uint64_t helper_ia64_alignment_exempt(CPUIA64State *env, uint64_t addr,
                                      uint32_t size, uint32_t is_write)
{
    MMUAccessType access = is_write ? MMU_DATA_STORE : MMU_DATA_LOAD;
    bool direct;

    return !ia64_exec_probe_writeback_ram(env, addr, size, access,
                                          &direct, GETPC());
}

/*
 * An Itanium 2 unaligned reference inside its window still faults when it
 * crosses an 8-byte boundary on a UC or WC target (251110-003 sec 5.5).  The
 * probe raises any higher-priority fault of the reference first.
 */
uint64_t helper_ia64_unaligned_uncacheable(CPUIA64State *env, uint64_t addr,
                                           uint32_t size, uint32_t is_write)
{
    return !ia64_exec_probe_writeback(env, addr, size,
                                      is_write ? MMU_DATA_STORE :
                                                 MMU_DATA_LOAD,
                                      GETPC());
}

uint64_t helper_cmpxchg(CPUIA64State *env, uint64_t addr, uint64_t cmp,
                        uint64_t val, uint32_t size)
{
    return ia64_memory_cmpxchg(env, addr, cmp, val, size, GETPC());
}

uint64_t helper_cmp8xchg16(CPUIA64State *env, uint64_t addr, uint64_t cmp,
                           uint64_t val, uint64_t csd)
{
    return ia64_memory_cmp8xchg16(env, addr, cmp, val, csd, GETPC());
}
