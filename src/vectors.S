/*
 * EL2 exception vectors.
 *
 * 16 entries x 128 bytes, 2KiB-aligned:
 *   0x000  Current EL with SP_EL0   -> el2_common (hypervisor self-faults)
 *   0x200  Current EL with SP_ELx   -> el2_common (e.g. the M1 BRK demo)
 *   0x400  Lower EL, AArch64        -> __guest_exit (a guest trapped to us)
 *   0x600  Lower EL, AArch32        -> el2_common (unused; we run A64 guests)
 *
 * The guest-exit stubs free up x0/x1, fetch the current vcpu pointer from
 * TPIDR_EL2 (set by __guest_enter), record which vector fired, and branch to
 * the save path in world.S.
 */
#include "vcpu_offsets.h"

#define FRAME_SIZE 288   /* 31 GPRs (248) + elr/spsr/esr/far (32) -> 16-aligned */

/* Hypervisor-self exception: build a trap frame and call the C dispatcher. */
.macro VEC idx
	.balign 0x80
	sub	sp, sp, #FRAME_SIZE
	stp	x0, x1, [sp, #0]
	mov	x0, #\idx
	b	el2_common
.endm

/* Guest exit: stash x0/x1, load vcpu*, record vector index, go save state. */
.macro VEC_GUEST idx
	.balign 0x80
	sub	sp, sp, #16
	stp	x0, x1, [sp]
	mrs	x0, tpidr_el2
	mov	x1, #\idx
	str	x1, [x0, #VCPU_EXIT_REASON]
	b	__guest_exit
.endm

.section .text.vectors
.balign 2048
.global el2_vector_table
el2_vector_table:
	VEC 0          /* Cur EL SP0   : sync   */
	VEC 1          /* Cur EL SP0   : irq    */
	VEC 2          /* Cur EL SP0   : fiq    */
	VEC 3          /* Cur EL SP0   : serror */
	VEC 4          /* Cur EL SPx   : sync   */
	VEC 5          /* Cur EL SPx   : irq    */
	VEC 6          /* Cur EL SPx   : fiq    */
	VEC 7          /* Cur EL SPx   : serror */
	VEC_GUEST 8    /* Lower aarch64: sync   */
	VEC_GUEST 9    /* Lower aarch64: irq    */
	VEC_GUEST 10   /* Lower aarch64: fiq    */
	VEC_GUEST 11   /* Lower aarch64: serror */
	VEC 12         /* Lower aarch32: sync   */
	VEC 13         /* Lower aarch32: irq    */
	VEC 14         /* Lower aarch32: fiq    */
	VEC 15         /* Lower aarch32: serror */

.section .text
.global el2_common
el2_common:
	/* x0 = vector index, x0/x1 already saved at [sp,#0] */
	stp	x2,  x3,  [sp, #16]
	stp	x4,  x5,  [sp, #32]
	stp	x6,  x7,  [sp, #48]
	stp	x8,  x9,  [sp, #64]
	stp	x10, x11, [sp, #80]
	stp	x12, x13, [sp, #96]
	stp	x14, x15, [sp, #112]
	stp	x16, x17, [sp, #128]
	stp	x18, x19, [sp, #144]
	stp	x20, x21, [sp, #160]
	stp	x22, x23, [sp, #176]
	stp	x24, x25, [sp, #192]
	stp	x26, x27, [sp, #208]
	stp	x28, x29, [sp, #224]
	str	x30,      [sp, #240]

	mrs	x1, elr_el2
	str	x1, [sp, #248]
	mrs	x1, spsr_el2
	str	x1, [sp, #256]
	mrs	x1, esr_el2
	str	x1, [sp, #264]
	mrs	x1, far_el2
	str	x1, [sp, #272]

	mov	x1, sp
	bl	el2_exception_dispatch

	ldr	x1, [sp, #248]
	msr	elr_el2, x1
	ldr	x1, [sp, #256]
	msr	spsr_el2, x1

	ldp	x2,  x3,  [sp, #16]
	ldp	x4,  x5,  [sp, #32]
	ldp	x6,  x7,  [sp, #48]
	ldp	x8,  x9,  [sp, #64]
	ldp	x10, x11, [sp, #80]
	ldp	x12, x13, [sp, #96]
	ldp	x14, x15, [sp, #112]
	ldp	x16, x17, [sp, #128]
	ldp	x18, x19, [sp, #144]
	ldp	x20, x21, [sp, #160]
	ldp	x22, x23, [sp, #176]
	ldp	x24, x25, [sp, #192]
	ldp	x26, x27, [sp, #208]
	ldp	x28, x29, [sp, #224]
	ldr	x30,      [sp, #240]
	ldp	x0,  x1,  [sp, #0]

	add	sp, sp, #FRAME_SIZE
	eret
