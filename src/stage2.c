#include "stage2.h"
#include "uart.h"

/*
 * Stage-2 configuration (4KiB granule, 39-bit IPA):
 *   IPA size  = 64 - T0SZ = 39 bits  (T0SZ = 25)  -> 512 GiB
 *   Start lvl = 1 (SL0 = 0b01): a single 512-entry level-1 table where each
 *               entry is a 1 GiB block descriptor. bits[38:30] index it, so
 *               one 4KiB page covers the whole 39-bit IPA space. No table
 *               concatenation, no lower levels -> the simplest correct setup.
 *
 * VTCR_EL2 fields:
 *   T0SZ=25, SL0=1, IRGN0=WBWA, ORGN0=WBWA, SH0=InnerShareable,
 *   TG0=4KiB, PS=40-bit, bit31 RES1.
 */
#define VTCR_EL2_VALUE 0x80023559UL

/*
 * Stage-2 level-1 block descriptor attributes (low bits OR'd with the
 * 1GiB-aligned output address):
 *   [1:0]  = 0b01    block descriptor
 *   [5:2]  = 0b1111  MemAttr: Normal Inner+Outer Write-Back cacheable
 *   [7:6]  = 0b11    S2AP: read/write from EL1&0
 *   [9:8]  = 0b11    SH: Inner Shareable
 *   [10]   = 1       AF (access flag)
 */
#define S2_BLOCK_NORMAL 0x7FDUL

/* Device-nGnRnE stage-2 block: MemAttr[5:2]=0, S2AP[7:6]=11 (RW), AF[10]=1,
 * SH=0, block descriptor. Used to pass through MMIO (GIC, UART, ...). */
#define S2_BLOCK_DEVICE 0x4C1UL

/* The level-1 stage-2 table. 512 entries * 8 bytes = one 4KiB page. */
static uint64_t s2_l1[512] __attribute__((aligned(4096)));

void stage2_map_1gb(uint64_t ipa) {
	uint64_t idx = (ipa >> 30) & 0x1FF;
	s2_l1[idx] = (ipa & ~0x3FFFFFFFUL) | S2_BLOCK_NORMAL;

	/* Publish the table write, then invalidate stage-1&2 TLB for EL1. */
	__asm__ volatile("dsb ish");
	__asm__ volatile("tlbi vmalls12e1");
	__asm__ volatile("dsb ish");
	__asm__ volatile("isb");
}

void stage2_init(void) {
	for (int i = 0; i < 512; i++)
		s2_l1[i] = 0;

	/* Map the 1GiB block that holds the guest's code and stack (the
	 * hypervisor image + .bss live in [0x40000000, 0x80000000)). */
	stage2_map_1gb(0x40000000UL);

	__asm__ volatile("msr vtcr_el2,  %0" ::"r"(VTCR_EL2_VALUE));
	__asm__ volatile("isb");

	uart_printf("[M3] stage-2 ON: vtcr=%x vttbr=%x (mapped 1GiB @0x40000000)\n",
	            VTCR_EL2_VALUE, (uint64_t)s2_l1);
}

uint64_t stage2_vttbr(void) {
	return (uint64_t)s2_l1; /* VMID 0 in bits[63:48] */
}

void stage2_map_1gb_device(uint64_t ipa) {
	uint64_t idx = (ipa >> 30) & 0x1FF;
	s2_l1[idx] = (ipa & ~0x3FFFFFFFUL) | S2_BLOCK_DEVICE;
	__asm__ volatile("dsb ish");
	__asm__ volatile("tlbi vmalls12e1");
	__asm__ volatile("dsb ish");
	__asm__ volatile("isb");
}

/* --- per-VM stage-2 tables for multi-tenancy ---
 * Each VM gets its own L1 table mapping one 1GiB guest-IPA block to a private
 * host-PA block (Normal memory) and a distinct VMID, so two VMs can use the
 * same guest address yet be physically isolated; VMID-tagged TLB entries make
 * VTTBR switches flush-free. */
static uint64_t s2_vm[2][512] __attribute__((aligned(4096)));

uint64_t stage2_build_vm(int vm, uint64_t ipa, uint64_t pa, int vmid) {
	for (int i = 0; i < 512; i++)
		s2_vm[vm][i] = 0;
	s2_vm[vm][(ipa >> 30) & 0x1FF] = (pa & ~0x3FFFFFFFUL) | S2_BLOCK_NORMAL;
	__asm__ volatile("dsb ish");
	__asm__ volatile("tlbi vmalls12e1");
	__asm__ volatile("dsb ish");
	__asm__ volatile("isb");
	return (uint64_t)&s2_vm[vm][0] | ((uint64_t)vmid << 48);
}
