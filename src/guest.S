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

/* ------------------------------------------------------------------ */
/* M6b guest: receives virtual timer interrupts.                       */
/* ------------------------------------------------------------------ */
.global guest_virq_entry
guest_virq_entry:
	/* Enable this VM's (virtual) GICv3 CPU interface. Under HCR_EL2.IMO=1
	 * and ICH_HCR_EL2.En=1 these ICC_* writes are redirected to the
	 * virtual interface (ICV_*) by hardware. */
	mov	x0, #1
	msr	icc_sre_el1, x0          /* SRE = 1 */
	isb
	mov	x0, #0xFF
	msr	icc_pmr_el1, x0          /* accept all priorities */
	mov	x0, #1
	msr	icc_igrpen1_el1, x0      /* enable Group 1 (sets VENG1) */

	/* Install the guest's EL1 vector table. */
	adr	x0, guest_el1_vectors
	msr	vbar_el1, x0
	isb

	mov	x21, #0                  /* virtual-tick counter (callee-saved) */
	msr	daifclr, #2              /* now safe to take IRQs */
1:
	wfi
	b	1b

	.balign 2048
guest_el1_vectors:
	.balign 0x80
	b	guest_unhandled          /* 0  Cur EL SP0 sync   */
	.balign 0x80
	b	guest_unhandled          /* 1  irq    */
	.balign 0x80
	b	guest_unhandled          /* 2  fiq    */
	.balign 0x80
	b	guest_unhandled          /* 3  serror */
	.balign 0x80
	b	guest_unhandled          /* 4  Cur EL SPx sync */
	.balign 0x80
	b	guest_irq                /* 5  Cur EL SPx irq  <- virtual timer */
	.balign 0x80
	b	guest_unhandled          /* 6  fiq    */
	.balign 0x80
	b	guest_unhandled          /* 7  serror */
	.balign 0x80
	b	guest_unhandled          /* 8  lower a64 sync */
	.balign 0x80
	b	guest_unhandled          /* 9  irq    */
	.balign 0x80
	b	guest_unhandled          /* 10 fiq    */
	.balign 0x80
	b	guest_unhandled          /* 11 serror */
	.balign 0x80
	b	guest_unhandled          /* 12 lower a32 sync */
	.balign 0x80
	b	guest_unhandled          /* 13 irq    */
	.balign 0x80
	b	guest_unhandled          /* 14 fiq    */
	.balign 0x80
	b	guest_unhandled          /* 15 serror */

guest_irq:
	sub	sp, sp, #32
	stp	x0, x1, [sp]
	stp	x29, x30, [sp, #16]
	mrs	x0, icc_iar1_el1         /* virtual ack -> vINTID */
	add	x21, x21, #1
	msr	icc_eoir1_el1, x0        /* virtual EOI */
	mov	x0, x21
	hvc	#0                       /* report tick count to the hypervisor */
	ldp	x29, x30, [sp, #16]
	ldp	x0, x1, [sp]
	add	sp, sp, #32
	eret

guest_unhandled:
	b	.
