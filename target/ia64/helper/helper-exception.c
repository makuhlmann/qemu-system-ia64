/* IA-64 TCG helper ABI adapters for exception return. */

#include "qemu/osdep.h"
#include "cpu.h"
#include "exec/helper-proto.h"
#include "arch/arch.h"

void helper_raise_exception(CPUIA64State *env, uint32_t exception,
                            uint64_t fault_ip, uint64_t fault_imm,
                            uint32_t fault_slot)
{
    ia64_raise_exception(env, exception, fault_ip, fault_imm, fault_slot);
}

void helper_ia32_unsupported(CPUIA64State *env)
{
    ia64_ia32_unsupported(env);
}

void helper_raise_unaligned(CPUIA64State *env, uint64_t addr,
                            uint64_t isr_access, uint64_t fault_info)
{
    ia64_raise_unaligned(env, addr, isr_access, fault_info);
}

void helper_raise_nat_consumption(CPUIA64State *env, uint64_t isr_access,
                                  uint64_t fault_info)
{
    ia64_raise_nat_consumption(env, isr_access, fault_info);
}

void helper_rfi(CPUIA64State *env, uint64_t fault_ip, uint32_t fault_slot)
{
    ia64_rfi(env, fault_ip, fault_slot);
}

void helper_completion_trap_arm(CPUIA64State *env, uint64_t iipa,
                                uint32_t slot)
{
    ia64_completion_trap_arm(env, iipa, slot, IA64_ISR_CODE_SS);
}

void helper_completion_trap_taken(CPUIA64State *env, uint64_t iipa,
                                  uint32_t slot)
{
    ia64_completion_trap_note(env, iipa, slot,
                              (env->psr & IA64_PSR_TB) ? IA64_ISR_CODE_TB : 0,
                              true);
}
