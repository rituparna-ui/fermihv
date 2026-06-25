/*
 * Minimal EL1 guest used by the M4 scheduler demo.
 *
 * It keeps a counter in x20 (a callee-saved register), increments it, passes
 * it to the hypervisor in x0 via HVC, then loops. Because the hypervisor
 * saves and restores the full guest register file across each exit/entry, the
 * counter must keep climbing across re-entries. Two vCPUs run the same code
 * with independent contexts, so their counters advance independently.
 */
.section .text.guest
.global guest_entry
guest_entry:
	mov	x20, #0
1:
	add	x20, x20, #1
	mov	x0, x20
	hvc	#0
	b	1b
