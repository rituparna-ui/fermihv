#include "uart.h"
#include "mmio.h"
#include <stdarg.h>

/* PL011 bring-up, identical sequence to the proven fermi-os driver:
 * 24 MHz reference clock, 115200 baud -> IBRD=13, FBRD=2. */
void uart_init(void) {
	mmio_write32(UART_CR, 0x0);          /* disable while configuring */
	mmio_write32(UART_ICR, 0x7FF);       /* clear pending interrupts  */
	mmio_write32(UART_IBRD, 13);
	mmio_write32(UART_FBRD, 2);
	mmio_write32(UART_LCRH, (1 << 4) | (1 << 5) | (1 << 6)); /* FIFO, 8N1 */
	mmio_write32(UART_CR, (1 << 0) | (1 << 8) | (1 << 9));   /* EN|TXE|RXE */
}

void uart_putc(char c) {
	while (mmio_read32(UART_FR) & (1 << 5)) { /* wait while TXFF */
	}
	mmio_write32(UART_DR, (uint32_t)(uint8_t)c);
}

/* Non-blocking read of the real console: returns the next input byte, or -1 if
 * the RX FIFO is empty (FR.RXFE set). Used to forward host keystrokes into a
 * guest's emulated UART. */
int uart_getc_nonblock(void) {
	if (mmio_read32(UART_FR) & (1 << 4))   /* RXFE: receive FIFO empty */
		return -1;
	return (int)(mmio_read32(UART_DR) & 0xFF);
}

void uart_puts(const char *s) {
	while (*s) {
		if (*s == '\n')
			uart_putc('\r');
		uart_putc(*s++);
	}
}

void uart_println(const char *s) {
	uart_puts(s);
	uart_putc('\r');
	uart_putc('\n');
}

void uart_puthex(uint64_t v) {
	uart_puts("0x");
	int started = 0;
	for (int i = 60; i >= 0; i -= 4) {
		uint8_t nib = (v >> i) & 0xF;
		if (nib || started || i == 0) {
			uart_putc(nib < 10 ? '0' + nib : 'a' + nib - 10);
			started = 1;
		}
	}
}

void uart_putdec(uint64_t v) {
	if (v == 0) {
		uart_putc('0');
		return;
	}
	char buf[20];
	int i = 0;
	while (v) {
		buf[i++] = '0' + (v % 10);
		v /= 10;
	}
	while (i--)
		uart_putc(buf[i]);
}

void uart_printf(const char *fmt, ...) {
	va_list args;
	va_start(args, fmt);
	while (*fmt) {
		if (*fmt != '%') {
			if (*fmt == '\n')
				uart_putc('\r');
			uart_putc(*fmt++);
			continue;
		}
		fmt++;
		switch (*fmt) {
		case 's': {
			const char *s = va_arg(args, const char *);
			uart_puts(s ? s : "(null)");
			break;
		}
		case 'd': {
			int64_t val = va_arg(args, int64_t);
			if (val < 0) {
				uart_putc('-');
				uart_putdec((uint64_t)(-val));
			} else {
				uart_putdec((uint64_t)val);
			}
			break;
		}
		case 'u':
			uart_putdec(va_arg(args, uint64_t));
			break;
		case 'x':
			uart_puthex(va_arg(args, uint64_t));
			break;
		case 'p':
			uart_puthex((uint64_t)va_arg(args, void *));
			break;
		case 'c':
			uart_putc((char)va_arg(args, int));
			break;
		case '%':
			uart_putc('%');
			break;
		case '\0':
			goto done;
		default:
			uart_putc('%');
			uart_putc(*fmt);
			break;
		}
		fmt++;
	}
done:
	va_end(args);
}
