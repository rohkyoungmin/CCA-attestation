#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/timer/system_timer.h>
#include <zephyr/arch/arm64/timer.h>

/*
 * Lightweight Realm timer for the mini-shell profile.
 *
 * The goal here is not full periodic scheduling accuracy yet; it is to
 * provide just enough clock/cycle plumbing for Zephyr shell core +
 * dummy backend to execute commands synchronously from the app-owned
 * UART loop. Timeouts are left as a no-op until we need richer timer
 * semantics for background services.
 */

void sys_clock_set_timeout(int32_t ticks, bool idle)
{
	ARG_UNUSED(ticks);
	ARG_UNUSED(idle);
}

uint32_t sys_clock_elapsed(void)
{
	return 0U;
}

uint32_t sys_clock_cycle_get_32(void)
{
	return (uint32_t)arm_arch_timer_count();
}

#ifdef CONFIG_TIMER_HAS_64BIT_CYCLE_COUNTER
uint64_t sys_clock_cycle_get_64(void)
{
	return arm_arch_timer_count();
}
#endif

static int sys_clock_driver_init(void)
{
	return 0;
}

SYS_INIT(sys_clock_driver_init, PRE_KERNEL_2, CONFIG_SYSTEM_CLOCK_INIT_PRIORITY);
