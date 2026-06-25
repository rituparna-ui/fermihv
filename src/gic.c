#include "gic.h"
#include "mmio.h"
#include "uart.h"

void gic_init_el2(void) {
	/* Enable the system-register CPU interface at EL2 (and allow EL1 use). */
	uint64_t sre;
	__asm__ volatile("mrs %0, icc_sre_el2" : "=r"(sre));
	sre |= (1UL << 0) | (1UL << 3); /* SRE | Enable(lower EL) */
	__asm__ volatile("msr icc_sre_el2, %0" ::"r"(sre));
	__asm__ volatile("isb");

	__asm__ volatile("mrs %0, icc_sre_el1" : "=r"(sre));
	sre |= 1UL;
	__asm__ volatile("msr icc_sre_el1, %0" ::"r"(sre));
	__asm__ volatile("isb");

	/* Distributor: affinity routing + Group 1 NS. */
	mmio_write32(GICD_CTLR, GICD_CTLR_ARE_NS | GICD_CTLR_ENABLE_G1NS);

	/* Wake this PE's redistributor. */
	uint32_t waker = mmio_read32(GICR_WAKER);
	waker &= ~GICR_WAKER_PROCESSOR_SLEEP;
	mmio_write32(GICR_WAKER, waker);
	while (mmio_read32(GICR_WAKER) & GICR_WAKER_CHILDREN_ASLEEP) {
	}

	/* SGIs/PPIs -> Group 1 NS. */
	mmio_write32(GICR_IGROUPR0, 0xFFFFFFFF);
	mmio_write32(GICR_IGRPMODR0, 0x00000000);

	/* CPU interface: accept all priorities, enable Group 1. */
	__asm__ volatile("msr icc_pmr_el1, %0" ::"r"(0xFFUL));
	__asm__ volatile("msr icc_igrpen1_el1, %0" ::"r"(1UL));
	__asm__ volatile("isb");

	uart_println("[GIC] GICv3 initialised for EL2");
}

void gic_enable_ppi(uint32_t intid) {
	mmio_write32(GICR_ISENABLER0, 1U << (intid & 31));
}

uint64_t gic_ack(void) {
	uint64_t iar;
	__asm__ volatile("mrs %0, icc_iar1_el1" : "=r"(iar));
	return iar;
}

void gic_eoi(uint64_t iar) {
	__asm__ volatile("msr icc_eoir1_el1, %0" ::"r"(iar));
}
