/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * IA-64 execution-engine access boundary.
 *
 * Architecture code uses this interface instead of including accelerator
 * headers.  Unit tests may provide a mock implementation without linking the
 * TCG helper ABI or the softmmu load/store implementation.
 */

#ifndef TARGET_IA64_EXEC_ACCESS_H
#define TARGET_IA64_EXEC_ACCESS_H

#include "cpu.h"
#include "exec/memopidx.h"
#include "exec/mmu-access-type.h"
#include "qemu/int128.h"

bool ia64_exec_is_parallel(CPUIA64State *env);
int ia64_exec_mmu_index(CPUIA64State *env, bool ifetch);
G_NORETURN void ia64_exec_exit_atomic(CPUIA64State *env, uintptr_t ra);

uint64_t ia64_exec_load_data(CPUIA64State *env, uint64_t addr,
                             unsigned size, bool big_endian, uintptr_t ra);
void ia64_exec_store_data(CPUIA64State *env, uint64_t addr, uint64_t value,
                          unsigned size, bool big_endian, uintptr_t ra);
uint64_t ia64_exec_load_mmuidx(CPUIA64State *env, uint64_t addr,
                               unsigned size, bool big_endian, int mmu_idx,
                               uintptr_t ra);
uint64_t ia64_exec_rse_store_collection(CPUIA64State *env, uint64_t addr,
                                        uint64_t value, uint64_t defined,
                                        bool big_endian, int mmu_idx,
                                        uint64_t *previous, uintptr_t ra);
void ia64_exec_store_mmuidx(CPUIA64State *env, uint64_t addr, uint64_t value,
                            unsigned size, bool big_endian, int mmu_idx,
                            uintptr_t ra);
Int128 ia64_exec_load_16(CPUIA64State *env, uint64_t addr, MemOpIdx oi,
                         uintptr_t ra);
void ia64_exec_store_16(CPUIA64State *env, uint64_t addr, Int128 value,
                        MemOpIdx oi, uintptr_t ra);

uint64_t ia64_exec_cmpxchg(CPUIA64State *env, uint64_t addr, uint64_t cmp,
                           uint64_t value, unsigned size, bool big_endian,
                           MemOpIdx oi, uintptr_t ra);
Int128 ia64_exec_cmpxchg_16(CPUIA64State *env, uint64_t addr, Int128 cmp,
                            Int128 value, bool big_endian, MemOpIdx oi,
                            uintptr_t ra);

void ia64_exec_probe_write(CPUIA64State *env, uint64_t addr, int size,
                           int mmu_idx, uintptr_t ra);
bool ia64_exec_probe_host(CPUIA64State *env, uint64_t addr, int size,
                          MMUAccessType access_type, int mmu_idx,
                          void **host, uintptr_t ra);
void *ia64_exec_direct_host(CPUIA64State *env, uint64_t addr,
                            MMUAccessType access_type, int mmu_idx);
int ia64_exec_load_hit_speculation(CPUIA64State *env, uint64_t addr,
                                   int mmu_idx);
bool ia64_exec_probe_writeback_ram(CPUIA64State *env, uint64_t addr,
                                   int size, MMUAccessType access_type,
                                   bool *direct, uintptr_t ra);
bool ia64_exec_probe_writeback(CPUIA64State *env, uint64_t addr,
                               int size, MMUAccessType access_type,
                               uintptr_t ra);
bool ia64_exec_advanced_load_allowed(CPUIA64State *env, uint64_t addr,
                                     int mmu_idx);
bool ia64_exec_debug_read(CPUState *cs, uint64_t addr, void *buffer,
                          size_t size);
bool ia64_exec_physical_rw(uint64_t addr, void *buffer, size_t size,
                           bool is_write);
void ia64_exec_invalidate_phys_range(CPUIA64State *env, uint64_t addr,
                                     uint64_t length);

#endif /* TARGET_IA64_EXEC_ACCESS_H */
