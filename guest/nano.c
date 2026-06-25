/*
 * "nano" — a tiny standalone guest kernel for FermiHV.
 *
 * It is built as its own image (own linker script, entry, address space),
 * loaded into guest RAM by the hypervisor, and started at EL1 with the MMU
 * off. It prints through the emulated PL011 UART, then builds and enables
 * its OWN stage-1 page tables. Surviving that proves nested translation:
 * guest-VA -> guest-IPA (this MMU) -> host-PA (the hypervisor's stage-2).
 */
#include <stdint.h>

#define UART_DR 0x09000000UL

static void putc(char c) {
	*(volatile uint32_t *)UART_DR = (uint32_t)(uint8_t)c;
}
static void puts(const char *s) {
	while (*s) {
		if (*s == '\n')
			putc('\r');
		putc(*s++);
	}
}

static void hvc(uint64_t a0) {
	register uint64_t x0 __asm__("x0") = a0;
	__asm__ volatile("hvc #0" : "+r"(x0)::"memory");
}

/* One level-1 stage-1 table: 512 x 1 GiB identity blocks. */
static uint64_t l1[512] __attribute__((aligned(4096)));

/* 1 GiB block descriptors (4KiB granule, level 1):
 *   [1:0]=01 block, AttrIndx[4:2], AP[7:6]=00 (RW EL1), SH[9:8], AF[10]=1. */
#define BLK_NORMAL(addr) ((addr) | (1UL << 10) | (3UL << 8) | (0UL << 2) | 1UL)
#define BLK_DEVICE(addr) ((addr) | (1UL << 10) | (0UL << 8) | (1UL << 2) | 1UL)

void nano_main(void) {
	puts("[nano] hello! a separately-built guest kernel, loaded by FermiHV, "
	     "running at EL1.\n");

	/* Build an identity map: block 0 (devices, incl. UART) = Device memory,
	 * all higher blocks (RAM) = Normal write-back. */
	l1[0] = BLK_DEVICE(0x0UL);
	for (uint64_t i = 1; i < 512; i++)
		l1[i] = BLK_NORMAL(i << 30);

	/* MAIR: Attr0 = Normal WB RW-alloc (0xFF), Attr1 = Device-nGnRnE (0x00). */
	uint64_t mair = 0x00000000000000FFUL;
	/* TCR: T0SZ=25 (39-bit VA), WBWA, Inner-Shareable, 4KiB, EPD1=1,
	 * IPS=40-bit. */
	uint64_t tcr = 25UL | (1UL << 8) | (1UL << 10) | (3UL << 12) |
	               (1UL << 23) | (2UL << 32);

	__asm__ volatile("msr mair_el1, %0" ::"r"(mair));
	__asm__ volatile("msr tcr_el1, %0" ::"r"(tcr));
	__asm__ volatile("msr ttbr0_el1, %0" ::"r"((uint64_t)l1));
	__asm__ volatile("isb");
	__asm__ volatile("tlbi vmalle1");
	__asm__ volatile("dsb nsh");
	__asm__ volatile("isb");

	puts("[nano] enabling my own stage-1 MMU (TTBR0_EL1)...\n");

	uint64_t sctlr;
	__asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
	sctlr |= (1UL << 0) | (1UL << 2) | (1UL << 12); /* M | C | I */
	__asm__ volatile("dsb sy");
	__asm__ volatile("msr sctlr_el1, %0" ::"r"(sctlr));
	__asm__ volatile("isb");

	puts("[nano] stage-1 MMU is ON and I am still running -> nested "
	     "translation works!\n");

	hvc(0x7777); /* tell the hypervisor we finished successfully */

	for (;;)
		__asm__ volatile("wfi");
}
