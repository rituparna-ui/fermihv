#include "uart.h"
#include "exception.h"
#include "vm.h"
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

	/* M2: drop to a bare EL1 guest and trap its hypercalls. */
	uart_println("[M2] Launching first EL1 guest (world switch)...");
	vm_run_guest();

	uart_println("[BOOT] M1 reached. Parking CPU.");
}
