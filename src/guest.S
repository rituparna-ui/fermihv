/*
 * Guests used by the scheduler demo.
 *
 * guest_counter_entry (M4): keeps a counter in callee-saved x20, increments
 *   it, hands it to the hypervisor via HVC, and loops. Proves register state
 *   survives world switches.
 *
 * guest_vuart_entry (M5): writes a string, one byte at a time, to a UART at
 *   0x09000000. That IPA is unmapped in stage-2, so each store traps to EL2
 *   as a data abort and is emulated (byte forwarded to the real console).
 */
.section .text.guest

.global guest_counter_entry
guest_counter_entry:
	mov	x20, #0
1:
	add	x20, x20, #1
	mov	x0, x20
	hvc	#0
	b	1b

.global guest_vuart_entry
guest_vuart_entry:
	movz	x2, #0x0900, lsl #16     /* x2 = 0x09000000 (emulated UART) */
	adr	x1, vmsg
2:
	ldrb	w0, [x1], #1
	cbz	w0, 3f
	str	w0, [x2]                 /* MMIO store -> stage-2 abort -> emulated */
	b	2b
3:
	hvc	#0                       /* signal completion */
4:
	wfi
	b	4b

	.balign 4
vmsg:
	.asciz	"Hello from the guest via an emulated UART (MMIO trap-and-emulate)!\n"
