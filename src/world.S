/*
 * World switch between the EL2 hypervisor and an EL1 guest.
 *
 * __guest_enter(vcpu*) is called from C. It:
 *   1. pushes the hypervisor's callee-saved regs + LR onto the EL2 stack and
 *      records that SP in vcpu->hyp_sp,
 *   2. stashes the vcpu pointer in TPIDR_EL2 so the exit path can find it,
 *   3. loads the guest's return state (ELR/SPSR/SP_EL0) and GP registers,
 *   4. erets into the guest at EL1.
 *
 * It has no `ret` of its own: the guest re-enters EL2 through the lower-EL
 * vectors (vectors.S), which jump to __guest_exit. __guest_exit saves the
 * guest GP regs + exit syndrome into the vcpu, restores the hypervisor's
 * callee-saved regs/SP, and `ret`s -- which returns control to the C caller
 * of __guest_enter as if it had returned normally.
 *
 * EL1 system registers are saved/restored in C (vcpu.c), not here.
 */
#include "vcpu_offsets.h"

.section .text
.global __guest_enter
__guest_enter:                 /* x0 = vcpu* */
	sub	sp, sp, #96
	stp	x19, x20, [sp, #0]
	stp	x21, x22, [sp, #16]
	stp	x23, x24, [sp, #32]
	stp	x25, x26, [sp, #48]
	stp	x27, x28, [sp, #64]
	stp	x29, x30, [sp, #80]

	msr	tpidr_el2, x0          /* current vcpu pointer */
	mov	x1, sp
	str	x1, [x0, #VCPU_HYP_SP]

	/* guest return state */
	ldr	x1, [x0, #VCPU_PC]
	msr	elr_el2, x1
	ldr	x1, [x0, #VCPU_PSTATE]
	msr	spsr_el2, x1
	ldr	x1, [x0, #VCPU_SP_EL0]
	msr	sp_el0, x1
	isb

	/* restore guest FP/SIMD (x0 still = vcpu base) */
	add	x1, x0, #VCPU_FP
	ldp	q0,  q1,  [x1, #0]
	ldp	q2,  q3,  [x1, #32]
	ldp	q4,  q5,  [x1, #64]
	ldp	q6,  q7,  [x1, #96]
	ldp	q8,  q9,  [x1, #128]
	ldp	q10, q11, [x1, #160]
	ldp	q12, q13, [x1, #192]
	ldp	q14, q15, [x1, #224]
	ldp	q16, q17, [x1, #256]
	ldp	q18, q19, [x1, #288]
	ldp	q20, q21, [x1, #320]
	ldp	q22, q23, [x1, #352]
	ldp	q24, q25, [x1, #384]
	ldp	q26, q27, [x1, #416]
	ldp	q28, q29, [x1, #448]
	ldp	q30, q31, [x1, #480]
	ldr	x1, [x0, #VCPU_FPSR]
	msr	fpsr, x1
	ldr	x1, [x0, #VCPU_FPCR]
	msr	fpcr, x1

	/* restore guest GP regs; x0 is reloaded last since it is the base ptr */
	ldr	x1,  [x0, #8]
	ldp	x2,  x3,  [x0, #16]
	ldp	x4,  x5,  [x0, #32]
	ldp	x6,  x7,  [x0, #48]
	ldp	x8,  x9,  [x0, #64]
	ldp	x10, x11, [x0, #80]
	ldp	x12, x13, [x0, #96]
	ldp	x14, x15, [x0, #112]
	ldp	x16, x17, [x0, #128]
	ldp	x18, x19, [x0, #144]
	ldp	x20, x21, [x0, #160]
	ldp	x22, x23, [x0, #176]
	ldp	x24, x25, [x0, #192]
	ldp	x26, x27, [x0, #208]
	ldp	x28, x29, [x0, #224]
	ldr	x30,      [x0, #240]
	ldr	x0,       [x0, #0]
	eret

/*
 * Entered from the lower-EL vector stubs with:
 *   x0 = vcpu* (from TPIDR_EL2), guest x0/x1 saved at [sp], guest x2..x30 live.
 */
.global __guest_exit
__guest_exit:
	/* save guest x2..x30 */
	stp	x2,  x3,  [x0, #16]
	stp	x4,  x5,  [x0, #32]
	stp	x6,  x7,  [x0, #48]
	stp	x8,  x9,  [x0, #64]
	stp	x10, x11, [x0, #80]
	stp	x12, x13, [x0, #96]
	stp	x14, x15, [x0, #112]
	stp	x16, x17, [x0, #128]
	stp	x18, x19, [x0, #144]
	stp	x20, x21, [x0, #160]
	stp	x22, x23, [x0, #176]
	stp	x24, x25, [x0, #192]
	stp	x26, x27, [x0, #208]
	stp	x28, x29, [x0, #224]
	str	x30,      [x0, #240]

	/* recover guest x0/x1 from the stub's stack slot */
	ldp	x2, x3, [sp]
	str	x2, [x0, #0]
	str	x3, [x0, #8]

	/* save guest return state + exit syndrome */
	mrs	x1, sp_el0
	str	x1, [x0, #VCPU_SP_EL0]
	mrs	x1, elr_el2
	str	x1, [x0, #VCPU_PC]
	mrs	x1, spsr_el2
	str	x1, [x0, #VCPU_PSTATE]
	mrs	x1, esr_el2
	str	x1, [x0, #VCPU_EXIT_ESR]
	mrs	x1, far_el2
	str	x1, [x0, #VCPU_EXIT_FAR]
	mrs	x1, hpfar_el2
	str	x1, [x0, #VCPU_EXIT_HPFAR]

	/* save guest FP/SIMD (x0 = vcpu base) */
	add	x1, x0, #VCPU_FP
	stp	q0,  q1,  [x1, #0]
	stp	q2,  q3,  [x1, #32]
	stp	q4,  q5,  [x1, #64]
	stp	q6,  q7,  [x1, #96]
	stp	q8,  q9,  [x1, #128]
	stp	q10, q11, [x1, #160]
	stp	q12, q13, [x1, #192]
	stp	q14, q15, [x1, #224]
	stp	q16, q17, [x1, #256]
	stp	q18, q19, [x1, #288]
	stp	q20, q21, [x1, #320]
	stp	q22, q23, [x1, #352]
	stp	q24, q25, [x1, #384]
	stp	q26, q27, [x1, #416]
	stp	q28, q29, [x1, #448]
	stp	q30, q31, [x1, #480]
	mrs	x1, fpsr
	str	x1, [x0, #VCPU_FPSR]
	mrs	x1, fpcr
	str	x1, [x0, #VCPU_FPCR]

	/* switch back to the hypervisor stack and restore callee-saved regs */
	ldr	x1, [x0, #VCPU_HYP_SP]
	mov	sp, x1
	ldp	x19, x20, [sp, #0]
	ldp	x21, x22, [sp, #16]
	ldp	x23, x24, [sp, #32]
	ldp	x25, x26, [sp, #48]
	ldp	x27, x28, [sp, #64]
	ldp	x29, x30, [sp, #80]
	add	sp, sp, #96
	ret                            /* return into vcpu_run_once() */
