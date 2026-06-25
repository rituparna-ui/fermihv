/* Guest entry. The hypervisor erets here at EL1 with the MMU off. */
.section .text.boot
.global _start
_start:
	ldr	x0, =__stack_top
	mov	sp, x0

	ldr	x0, =__bss_start
	ldr	x1, =__bss_end
1:	cmp	x0, x1
	b.hs	2f
	str	xzr, [x0], #8
	b	1b
2:	bl	nano_main
3:	wfi
	b	3b
