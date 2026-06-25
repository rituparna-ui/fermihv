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

	/* M3: read an unmapped IPA (0x80000000). With stage-2 enabled this
	 * faults to EL2 (data abort, EC=0x24); the hypervisor faults the 1GiB
	 * block in on demand and returns WITHOUT advancing ELR, so this same
	 * load is retried and now succeeds. */
	movz	x1, #0x8000, lsl #16     /* x1 = 0x80000000 */
	ldr	x2, [x1]
	mov	x0, #0x3333              /* survived the stage-2 fault-in */
	hvc	#0

1:	wfi
	b	1b
