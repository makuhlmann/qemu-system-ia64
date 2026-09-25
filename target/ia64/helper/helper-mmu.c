/* IA-64 TCG helper ABI adapters for MMU serialization. */

#include "qemu/osdep.h"
#include "cpu.h"
#include "exec/helper-proto.h"
#include "arch/arch.h"

void helper_tlb_serialize(CPUIA64State *env, uint32_t include_data,
                          uint32_t include_inst)
{
    ia64_tlb_serialize(env, include_data, include_inst);
}

void helper_fc(CPUIA64State *env, uint64_t addr)
{
    ia64_mmu_fc(env, addr);
}

void helper_itr_insert(CPUIA64State *env, uint64_t pte, uint64_t slot_reg,
                       uint32_t is_data, uint64_t raw, uint32_t fault_slot)
{
    ia64_mmu_itr_insert(env, pte, slot_reg, is_data, raw, fault_slot);
}

void helper_ptr_purge(CPUIA64State *env, uint64_t ifa, uint64_t size_reg,
                      uint32_t is_data)
{
    ia64_mmu_ptr_purge(env, ifa, size_reg, is_data);
}

void helper_ptc_purge(CPUIA64State *env, uint64_t va, uint64_t size_reg,
                      uint32_t mode)
{
    ia64_mmu_ptc_purge(env, va, size_reg, mode);
}

uint64_t helper_tpa(CPUIA64State *env, uint64_t va)
{
    return ia64_mmu_tpa(env, va);
}

uint64_t helper_probe(CPUIA64State *env, uint64_t va, uint32_t is_write,
                      uint64_t access_level)
{
    return ia64_mmu_probe(env, va, is_write, access_level);
}

void helper_probe_fault(CPUIA64State *env, uint64_t va, uint32_t is_write,
                        uint32_t is_rw, uint64_t access_level)
{
    ia64_mmu_probe_fault(env, va, is_write, is_rw, access_level);
}

void helper_lfetch_fault(CPUIA64State *env, uint64_t va,
                         uint64_t fault_info, uint32_t hint)
{
    ia64_mmu_lfetch_fault(env, va, fault_info, hint);
}

void helper_check_semaphore_access(CPUIA64State *env, uint64_t va)
{
    ia64_mmu_check_semaphore_access(env, va);
}

void helper_check_montecito_16byte_access(CPUIA64State *env, uint64_t va,
                                          uint32_t is_write)
{
    ia64_mmu_check_montecito_16byte_access(env, va, is_write);
}

uint64_t helper_speculative_probe(CPUIA64State *env, uint64_t va,
                                  uint32_t is_write, uint32_t is_ifetch,
                                  uint32_t size, uint32_t window,
                                  uint32_t span)
{
    return ia64_mmu_speculative_probe(env, va, is_write, is_ifetch, size,
                                      window, span);
}

uint64_t helper_advanced_load_allowed(CPUIA64State *env, uint64_t va)
{
    return ia64_mmu_advanced_load_allowed(env, va);
}

uint64_t helper_tak(CPUIA64State *env, uint64_t va)
{
    return ia64_mmu_tak(env, va);
}

uint64_t helper_thash(CPUIA64State *env, uint64_t va)
{
    return ia64_mmu_thash(env, va);
}

uint64_t helper_ttag(CPUIA64State *env, uint64_t va)
{
    return ia64_mmu_ttag(env, va);
}

void helper_itc_insert(CPUIA64State *env, uint64_t pte, uint32_t is_data,
                       uint64_t raw, uint32_t fault_slot)
{
    ia64_mmu_itc_insert(env, pte, is_data, raw, fault_slot);
}
