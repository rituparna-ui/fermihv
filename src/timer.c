#include "timer.h"
#include "gic.h"
#include "uart.h"

static volatile uint64_t t_freq = 0;
static volatile uint64_t t_interval = 0;
static volatile uint64_t t_ticks = 0;

void hyptimer_init(void) {
	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(t_freq));
	gic_enable_ppi(HYP_TIMER_PPI);
	uart_printf("[TIMER] CNTFRQ = %u Hz; hyp timer PPI %u enabled\n",
	            t_freq, (uint64_t)HYP_TIMER_PPI);
}

void hyptimer_start(uint64_t interval_ms) {
	t_interval = t_freq * interval_ms / 1000;
	t_ticks = 0;

	/* Absolute deadline against the physical counter (no drift accumulation). */
	uint64_t now;
	__asm__ volatile("mrs %0, cntpct_el0" : "=r"(now));
	__asm__ volatile("msr cnthp_cval_el2, %0" ::"r"(now + t_interval));
	__asm__ volatile("msr cnthp_ctl_el2, %0" ::"r"(1UL)); /* ENABLE, IMASK=0 */
	__asm__ volatile("isb");
}

void hyptimer_stop(void) {
	__asm__ volatile("msr cnthp_ctl_el2, %0" ::"r"(0UL));
	__asm__ volatile("isb");
}

void hyptimer_on_irq(void) {
	t_ticks++;
	uint64_t cval;
	__asm__ volatile("mrs %0, cnthp_cval_el2" : "=r"(cval));
	cval += t_interval;
	__asm__ volatile("msr cnthp_cval_el2, %0" ::"r"(cval));
}

uint64_t hyptimer_ticks(void) {
	return t_ticks;
}
