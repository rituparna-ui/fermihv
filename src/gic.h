#ifndef FERMIHV_GIC_H
#define FERMIHV_GIC_H

#include <stdint.h>

/* GICv3 on the QEMU `virt` machine. */
#define GICD_BASE 0x08000000UL
#define GICR_BASE 0x080A0000UL

#define GICD_CTLR        (GICD_BASE + 0x0000)
#define GICD_CTLR_ENABLE_G1NS (1U << 1)
#define GICD_CTLR_ARE_NS      (1U << 4)

#define GICR_WAKER       (GICR_BASE + 0x0014)
#define GICR_WAKER_PROCESSOR_SLEEP (1U << 1)
#define GICR_WAKER_CHILDREN_ASLEEP (1U << 2)

#define GICR_SGI_BASE    (GICR_BASE + 0x10000)
#define GICR_IGROUPR0    (GICR_SGI_BASE + 0x0080)
#define GICR_IGRPMODR0   (GICR_SGI_BASE + 0x0D00)
#define GICR_ISENABLER0  (GICR_SGI_BASE + 0x0100)

#define GIC_SPURIOUS     1023
/* INTIDs >= 1020 are architectural specials (1023 = spurious); a real
 * interrupt to ack/EOI has INTID < 1020. */
#define GIC_SPURIOUS_INTID 1020

/* Bring up the GICv3 for use from EL2: enable the system-register CPU
 * interface, the distributor, wake the redistributor, and enable Group 1. */
void gic_init_el2(void);

/* Enable an SGI/PPI (INTID < 32) at this PE's redistributor. */
void gic_enable_ppi(uint32_t intid);

/* Acknowledge the highest-priority pending Group-1 IRQ (ICC_IAR1_EL1). */
uint64_t gic_ack(void);

/* Signal end-of-interrupt (ICC_EOIR1_EL1). */
void gic_eoi(uint64_t iar);

#endif /* FERMIHV_GIC_H */
