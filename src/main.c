#include "uart.h"
#include "exception.h"
#include "vcpu.h"
#include "gic.h"
#include "timer.h"
#include "mmio.h"
#include <stdint.h>

static inline uint64_t read_currentel(void) {
	uint64_t v;
	__asm__ volatile("mrs %0, CurrentEL" : "=r"(v));
	return (v >> 2) & 0x3;
}

/* Entry from boot.S on the primary CPU at EL2, MMU off. */
void hv_main(void) {
	uart_init();

	uart_println("");
	uart_println("========================================");
	uart_println(" FermiHV  -  AArch64 type-1 hypervisor");
	uart_println("========================================");

	uint64_t el = read_currentel();
	uart_printf("[BOOT] CurrentEL = EL%u\n", el);
	if (el == 2)
		uart_println("[BOOT] Running at EL2 (hypervisor) - OK");
	else
		uart_println("[BOOT] WARNING: not at EL2 (need virtualization=on)");

	/* M1: install the EL2 vector table and prove the trap path works. */
	exceptions_init();
	uart_println("[M1] VBAR_EL2 installed.");
	uart_println("[M1] Triggering deliberate BRK #0xBEEF ...");
	__asm__ volatile("brk #0xBEEF");
	uart_println("[M1] Resumed after BRK -> EL2 trap+recover works.");

	/* M2/M3/M4/M5: stage-2, world switch, scheduler, and MMIO emulation. */
	uart_println("[HV] Starting vCPU scheduler...");
	sched_demo();

	/* M6a: take a real timer interrupt at EL2 via the GICv3. */
	uart_println("[M6a] EL2 physical timer interrupts via GICv3:");
	gic_init_el2();
	hyptimer_init();

	/* Route physical IRQs to EL2 (HCR_EL2.IMO). Without this they target
	 * EL1 and, since we execute at EL2, are never taken (just stay pending). */
	uint64_t hcr;
	__asm__ volatile("mrs %0, hcr_el2" : "=r"(hcr));
	hcr |= (1UL << 4); /* IMO */
	__asm__ volatile("msr hcr_el2, %0" ::"r"(hcr));
	__asm__ volatile("isb");

	hyptimer_start(100); /* 100 ms */
	__asm__ volatile("msr daifclr, #2"); /* unmask IRQ at EL2 */
	while (hyptimer_ticks() < 5)
		__asm__ volatile("wfi");
	__asm__ volatile("msr daifset, #2"); /* mask IRQ */
	hyptimer_stop();
	uart_printf("[M6a] received %u EL2 timer ticks; IRQ plumbing works.\n",
	            hyptimer_ticks());

	/* M6b: deliver those ticks into the guest as virtual interrupts. */
	virq_demo(5);

	/* M13: software vGIC -- guest drives an emulated GICD/GICR distributor. */
	vgic_demo(5);

	/* M14: SMP groundwork -- virtual IPI between two vCPUs via the vGIC. */
	smp_demo();

	/* M15: a running dual-core guest -- preemptive 2-vCPU scheduler + IPIs. */
	smp_sched_demo();

	/* M16: multi-tenancy -- two isolated VMs running concurrently. */
	mtenant_demo();

	/* M18: per-VM vGIC -- two isolated VMs each on its own emulated GIC. */
	mtenant_vgic_demo();

	/* M7: load and boot a separately-built guest kernel image. */
	uart_println("[M7] booting a separately-built guest kernel:");
	real_guest_demo();

	/* Boot a guest based on what the loader placed at 0x41000000:
	 *  - Linux Image (arm64 magic at +56), else
	 *  - fermi-os (non-zero first word). QEMU's auto-DTB sits at 0x40000000. */
	uint32_t lmagic = *(volatile uint32_t *)(0x41000000UL + 56);
	uint32_t marker = *(volatile uint32_t *)0x46000000UL;
	if (marker == 0x0FE33108UL) {
		linux_vgic_boot();  /* Linux on the emulated vGIC (./run.sh linux-vgic) */
	} else if (marker == 0x0FE33109UL) {
		mtenant_os_demo();  /* Linux + fermi-os co-tenants (./run.sh mtenant-os) */
	} else if (lmagic == 0x644d5241UL) {
		linux_boot();
	} else if (marker == 0x0FE33105UL) {
		fermios_boot();   /* embedded fermi-os image, selected by ./run.sh fermios */
	} else if (marker == 0x0FE33106UL) {
		fermios_vgic_boot();  /* fermi-os on the emulated vGIC (./run.sh fermios-vgic) */
	} else if (marker == 0x0FE33107UL) {
		mtenant_real_demo();  /* real OS + tenant, concurrent isolated (./run.sh mtenant-real) */
	} else {
		uart_println("[HV] no guest image loaded; demos only.");
	}

	uart_println("[HV] Done. Parking CPU.");
}
