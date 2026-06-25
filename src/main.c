#include "uart.h"
#include <stdint.h>

static inline uint64_t read_currentel(void) {
	uint64_t v;
	__asm__ volatile("mrs %0, CurrentEL" : "=r"(v));
	return (v >> 2) & 0x3;
}

/* M0 entry. Called from boot.S on the primary CPU at EL2, MMU off. */
void hv_main(void) {
	uart_init();

	uart_println("");
	uart_println("========================================");
	uart_println(" FermiHV  -  AArch64 type-1 hypervisor");
	uart_println("========================================");

	uint64_t el = read_currentel();
	uart_printf("[BOOT] CurrentEL = EL%u\n", el);

	if (el == 2) {
		uart_println("[BOOT] Running at EL2 (hypervisor) - OK");
	} else {
		uart_printf("[BOOT] WARNING: expected EL2 but at EL%u.\n", el);
		uart_println("[BOOT] Launch QEMU with -machine ...,virtualization=on");
	}

	uart_println("[BOOT] M0 reached: skeleton boots at EL2. Parking CPU.");
}
