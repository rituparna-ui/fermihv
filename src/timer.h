#ifndef FERMIHV_TIMER_H
#define FERMIHV_TIMER_H

#include <stdint.h>

/* EL2 physical (hypervisor) timer PPI on the QEMU virt GICv3. */
#define HYP_TIMER_PPI 26

/* Read CNTFRQ and enable the timer PPI at the redistributor. */
void hyptimer_init(void);

/* Arm the CNTHP_EL2 timer to fire every `interval_ms`. */
void hyptimer_start(uint64_t interval_ms);

/* Disable the timer. */
void hyptimer_stop(void);

/* Called from the EL2 IRQ path on a HYP_TIMER_PPI: count + re-arm. */
void hyptimer_on_irq(void);

/* Total ticks observed since start. */
uint64_t hyptimer_ticks(void);

#endif /* FERMIHV_TIMER_H */
