/*
 * FermiHV boot entry.
 *
 * QEMU `virt,virtualization=on` enters the `-kernel` image at EL2 with the
 * MMU and caches off, on the primary CPU only. We do the bare minimum to
 * get into C: point SP at the top of our reserved stack, zero the BSS, then
 * branch to hv_main(). DAIF is masked on reset, so exceptions stay off until
 * M1 installs a real EL2 vector table.
 */
.section .text.boot
.global _start
_start:
	// Park any secondary CPUs (MPIDR_EL1.Aff0 != 0) in a wfe loop.
	mrs	x0, mpidr_el1
	and	x0, x0, #0xff
	cbnz	x0, secondary_park

	// Set up the EL2 stack (identity-mapped: VA == PA).
	ldr	x0, =__stack_top
	mov	sp, x0

	// Zero the BSS [__bss_start, __bss_end).
	ldr	x0, =__bss_start
	ldr	x1, =__bss_end
1:
	cmp	x0, x1
	b.hs	2f
	str	xzr, [x0], #8
	b	1b
2:
	bl	hv_main

	// hv_main should not return; if it does, fall through to park.
hv_hang:
	wfi
	b	hv_hang

secondary_park:
	wfe
	b	secondary_park
