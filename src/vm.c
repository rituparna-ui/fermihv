#include "vm.h"
#include "uart.h"
#include "stage2.h"
#include <stdint.h>

/* Guest EL1 stack (shared physical RAM for now; isolated via stage-2 in M3). */
static uint8_t guest_stack[8192] __attribute__((aligned(16)));

extern void guest_entry(void);

/* SPSR_EL2 value to enter EL1h with DAIF masked:
 *   D|A|I|F = 1 (bits 9..6 -> 0x3C0), M[3:0] = 0b0101 (EL1h) -> 0x3C5. */
#define SPSR_EL1H_MASKED 0x3C5UL

void vm_run_guest(void) {
	uint64_t entry = (uint64_t)&guest_entry;
	uint64_t sp = (uint64_t)(guest_stack + sizeof(guest_stack));

	/* Bring up stage-2 translation before enabling HCR_EL2.VM. */
	stage2_init();

	uart_printf("[M2] entering guest at EL1: entry=%x sp_el1=%x\n", entry, sp);

	__asm__ volatile(
		/* HCR_EL2: RW (EL1=AArch64) | VM (stage-2 on) | DC (default
		 * cacheable, so the guest's MMU-off accesses are Normal memory). */
		"mov   x9, #1\n"
		"lsl   x9, x9, #31\n"      /* RW (bit 31) */
		"orr   x9, x9, #(1 << 0)\n"   /* VM */
		"orr   x9, x9, #(1 << 12)\n"  /* DC */
		"msr   hcr_el2, x9\n"
		/* guest stack + return state */
		"msr   sp_el1, %[sp]\n"
		"mov   x9, %[spsr]\n"
		"msr   spsr_el2, x9\n"
		"msr   elr_el2, %[entry]\n"
		"isb\n"
		"eret\n"
		:
		: [entry] "r"(entry), [sp] "r"(sp), [spsr] "r"(SPSR_EL1H_MASKED)
		: "x9", "memory");

	__builtin_unreachable();
}
