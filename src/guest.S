/*
 * A minimal bare-metal "guest" that runs at EL1. For M2 there is no stage-2
 * yet, so it shares the hypervisor's flat physical address space. It simply
 * issues two hypercalls (HVC) with a marker value in x0, then parks in WFI.
 * Each HVC traps to EL2 (lower-EL AArch64 sync vector), proving the world
 * switch host -> guest -> host works.
 */
.section .text.guest
.global guest_entry
guest_entry:
	mov	x0, #0x1111      /* hypercall arg / marker */
	hvc	#0

	mov	x0, #0x2222
	hvc	#0

1:	wfi
	b	1b
