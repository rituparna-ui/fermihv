#include "vdev.h"
#include "uart.h"
#include "exception.h"

/* PL011 register offsets the guest might touch. */
#define VUART_DR   0x00   /* data register         */
#define VUART_FR   0x18   /* flag register         */
#define VUART_IMSC 0x38   /* interrupt mask set/clr */
#define VUART_RIS  0x3C   /* raw interrupt status   */
#define VUART_MIS  0x40   /* masked interrupt status*/
#define VUART_ICR  0x44   /* interrupt clear        */

#define PL011_RXIM (1u << 4)   /* receive interrupt (mask/status bit) */

static uint32_t v_imsc;        /* guest's interrupt mask */
static int v_rx = -1;          /* pending RX byte, -1 if none */

int vuart_contains(uint64_t ipa) {
	return ipa >= VUART_BASE && ipa < VUART_BASE + VUART_SIZE;
}

/* Feed a host keystroke into the emulated UART's 1-byte RX FIFO. */
void vuart_push_rx(int c) {
	v_rx = c & 0xFF;
}

/* True if a receive interrupt is asserted (byte pending AND guest unmasked RX). */
int vuart_rx_irq_pending(void) {
	return (v_rx >= 0) && (v_imsc & PL011_RXIM);
}

static void vuart_write(uint64_t off, uint64_t val) {
	switch (off) {
	case VUART_DR:   uart_putc((char)(val & 0xFF)); break; /* to real console */
	case VUART_IMSC: v_imsc = (uint32_t)val; break;
	case VUART_ICR:  break;   /* RX clears on DR read; nothing else latched */
	default: break;           /* CR/IBRD/FBRD/LCRH: accept and ignore */
	}
}

static uint64_t vuart_read(uint64_t off) {
	switch (off) {
	case VUART_DR: {                       /* read a received byte */
		int c = v_rx;
		v_rx = -1;
		return (c < 0) ? 0 : (uint32_t)c;
	}
	case VUART_FR:                         /* TXFE=1; RXFE=1 iff no byte */
		return (1u << 7) | ((v_rx < 0) ? (1u << 4) : 0);
	case VUART_IMSC: return v_imsc;
	case VUART_RIS:  return (v_rx >= 0) ? PL011_RXIM : 0;
	case VUART_MIS:  return ((v_rx >= 0) ? PL011_RXIM : 0) & v_imsc;
	/* PL011 PrimeCell identification registers (PeriphID0-3, PCellID0-3).
	 * Linux's AMBA bus reads these to match a driver; returning 0 makes the
	 * pl011 probe defer forever. These are QEMU's PL011 values
	 * (periphid 0x00141011, cellid 0xb105f00d). */
	case 0xFE0: return 0x11;
	case 0xFE4: return 0x10;
	case 0xFE8: return 0x14;
	case 0xFEC: return 0x00;
	case 0xFF0: return 0x0d;
	case 0xFF4: return 0xf0;
	case 0xFF8: return 0x05;
	case 0xFFC: return 0xb1;
	default: return 0;
	}
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
