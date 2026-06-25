#include "vdev.h"
#include "uart.h"
#include "exception.h"

/* PL011 register offsets the guest might touch. */
#define VUART_DR 0x00   /* data register      */
#define VUART_FR 0x18   /* flag register      */

int vuart_contains(uint64_t ipa) {
	return ipa >= VUART_BASE && ipa < VUART_BASE + VUART_SIZE;
}

static void vuart_write(uint64_t off, uint64_t val) {
	if (off == VUART_DR)
		uart_putc((char)(val & 0xFF));   /* forward to the real console */
	/* CR / IBRD / FBRD / LCRH / ICR writes: accept and ignore */
}

static uint64_t vuart_read(uint64_t off) {
	if (off == VUART_FR)
		return 0;   /* TXFF=0, RXFE=0: never full, claim no input */
	return 0;
}

/*
 * Decode an ESR_EL2 data-abort instruction syndrome and emulate the access.
 * ISS layout for a Data Abort with valid syndrome:
 *   [24]    ISV  syndrome valid
 *   [23:22] SAS  access size (0=B,1=H,2=W,3=D)
 *   [20:16] SRT  transfer register (Xt); 31 = xzr/wzr
 *   [6]     WnR  1=write, 0=read
 */
void mmio_emulate(vcpu_t *v, uint64_t ipa) {
	uint64_t iss = ESR_ISS(v->exit_esr);
	int isv = (iss >> 24) & 1;

	if (!isv) {
		/* No decoded syndrome (e.g. load/store pair). We can't emulate it
		 * generically here; skip the instruction so we don't spin. */
		uart_printf("[M5] DABT without ISV (iss=%x) at ipa=%x, skipping\n",
		            iss, ipa);
		v->pc += 4;
		return;
	}

	int wnr = (iss >> 6) & 1;
	int srt = (iss >> 16) & 0x1F;
	uint64_t off = ipa - VUART_BASE;

	if (wnr) {
		uint64_t val = (srt == 31) ? 0 : v->x[srt];
		vuart_write(off, val);
	} else {
		uint64_t val = vuart_read(off);
		if (srt != 31)
			v->x[srt] = val;
	}

	/* We fully emulated the access -> resume at the next instruction. */
	v->pc += 4;
}
