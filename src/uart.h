#ifndef FERMIHV_UART_H
#define FERMIHV_UART_H

#include <stdint.h>

/* PL011 UART0 on the QEMU `virt` machine. */
#define UART_BASE 0x09000000UL
#define UART_DR   (UART_BASE + 0x00)
#define UART_FR   (UART_BASE + 0x18)
#define UART_IBRD (UART_BASE + 0x24)
#define UART_FBRD (UART_BASE + 0x28)
#define UART_LCRH (UART_BASE + 0x2C)
#define UART_CR   (UART_BASE + 0x30)
#define UART_ICR  (UART_BASE + 0x44)

void uart_init(void);
void uart_putc(char c);
int uart_getc_nonblock(void);   /* real-console input byte, or -1 if none */
void uart_puts(const char *s);
void uart_println(const char *s);
void uart_puthex(uint64_t v);
void uart_putdec(uint64_t v);

/* Supported specifiers: %s %d %u %x %p %c %% */
void uart_printf(const char *fmt, ...);

#endif /* FERMIHV_UART_H */
