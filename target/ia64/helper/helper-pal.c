/* IA-64 TCG helper ABI adapter for PAL dispatch. */

#include "qemu/osdep.h"
#include "cpu.h"
#include "exec/helper-proto.h"
#include "arch/arch.h"

uint32_t helper_pal_dispatch(CPUIA64State *env)
{
    return ia64_pal_dispatch(env, GETPC());
}

void helper_pal_reset_return(CPUIA64State *env)
{
    ia64_cpu_pal_reset_return(env);
}
