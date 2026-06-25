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

/* ------------------------------------------------------------------ */
/* M13 guest: drives the EMULATED GICv3 (GICD/GICR MMIO trap+emulate). */
/* ------------------------------------------------------------------ */
.global guest_vgic_entry
guest_vgic_entry:
	/* GICD_CTLR = ARE_NS | EnableGrp1  (MMIO -> traps to the vGIC) */
	movz	x2, #0x0800, lsl #16     /* 0x08000000 GICD */
	mov	w0, #0x12
	str	w0, [x2]

	/* GICR_WAKER: clear ProcessorSleep (emulated -> reports awake) */
	movz	x3, #0x080A, lsl #16     /* 0x080A0000 GICR RD_base */
	str	wzr, [x3, #0x14]

	/* GICR SGI frame: all Group 1, enable INTID 27 (virtual timer PPI) */
	movz	x4, #0x080B, lsl #16     /* 0x080B0000 GICR SGI_base */
	mov	w0, #0xFFFFFFFF
	str	w0, [x4, #0x80]          /* IGROUPR0 */
	mov	w0, #1
	lsl	w0, w0, #27
	str	w0, [x4, #0x100]         /* ISENABLER0: enable INTID 27 */

	/* CPU interface (system register / ICV under IMO + ICH_HCR_EL2.En) */
	mov	x0, #1
	msr	icc_sre_el1, x0
	isb
	mov	x0, #0xFF
	msr	icc_pmr_el1, x0
	mov	x0, #1
	msr	icc_igrpen1_el1, x0

	adr	x0, gvgic_vectors
	msr	vbar_el1, x0
	isb
	mov	x21, #0
	msr	daifclr, #2
1:
	wfi
	b	1b

	.balign 2048
gvgic_vectors:
	.balign 0x80
	b	gvgic_bad
	.balign 0x80
	b	gvgic_bad
	.balign 0x80
	b	gvgic_bad
	.balign 0x80
	b	gvgic_bad
	.balign 0x80
	b	gvgic_bad
	.balign 0x80
	b	gvgic_irq                /* idx 5: Cur EL SPx IRQ */
	.balign 0x80
	b	gvgic_bad
	.balign 0x80
	b	gvgic_bad
	.balign 0x80
	b	gvgic_bad
	.balign 0x80
	b	gvgic_bad
	.balign 0x80
	b	gvgic_bad
	.balign 0x80
	b	gvgic_bad
	.balign 0x80
	b	gvgic_bad
	.balign 0x80
	b	gvgic_bad
	.balign 0x80
	b	gvgic_bad
	.balign 0x80
	b	gvgic_bad

gvgic_irq:
	sub	sp, sp, #32
	stp	x0, x1, [sp]
	stp	x29, x30, [sp, #16]
	mrs	x0, icc_iar1_el1         /* virtual ack -> vINTID (from list register) */
	add	x21, x21, #1
	msr	icc_eoir1_el1, x0
	mov	x0, x21
	hvc	#0
	ldp	x29, x30, [sp, #16]
	ldp	x0, x1, [sp]
	add	sp, sp, #32
	eret

gvgic_bad:
	b	.

/* ------------------------------------------------------------------ */
/* M14 guest: two vCPUs; vCPU0 sends a virtual IPI (SGI) to vCPU1.      */
/* x0 on entry = this vCPU's id (seeded by the hypervisor).            */
/* ------------------------------------------------------------------ */
.global guest_ipi_entry
guest_ipi_entry:
	mov	x22, x0                  /* save cpu id */
	mov	x0, #1
	msr	icc_sre_el1, x0
	isb
	mov	x0, #0xFF
	msr	icc_pmr_el1, x0
	mov	x0, #1
	msr	icc_igrpen1_el1, x0
	adr	x0, gipi_vectors
	msr	vbar_el1, x0
	isb
	msr	daifclr, #2
	cbnz	x22, gipi_wait           /* id != 0 -> receiver */

	/* sender (vCPU0): generate SGI INTID 1, IRM=1 (all PEs except self) */
	mov	x0, #1
	lsl	x0, x0, #24              /* INTID 1 in bits[27:24] */
	mov	x1, #1
	lsl	x1, x1, #40              /* IRM = 1 */
	orr	x0, x0, x1
	msr	icc_sgi1r_el1, x0        /* traps to EL2 (ICH_HCR_EL2.TC) */
	mov	x0, #0x5E0D              /* "sent" marker */
	hvc	#0
gipi_wait:
	wfi
	b	gipi_wait

	.balign 2048
gipi_vectors:
	.balign 0x80
	b	gipi_bad
	.balign 0x80
	b	gipi_bad
	.balign 0x80
	b	gipi_bad
	.balign 0x80
	b	gipi_bad
	.balign 0x80
	b	gipi_bad
	.balign 0x80
	b	gipi_irq                 /* idx 5: Cur EL SPx IRQ */
	.balign 0x80
	b	gipi_bad
	.balign 0x80
	b	gipi_bad
	.balign 0x80
	b	gipi_bad
	.balign 0x80
	b	gipi_bad
	.balign 0x80
	b	gipi_bad
	.balign 0x80
	b	gipi_bad
	.balign 0x80
	b	gipi_bad
	.balign 0x80
	b	gipi_bad
	.balign 0x80
	b	gipi_bad
	.balign 0x80
	b	gipi_bad

gipi_irq:
	sub	sp, sp, #32
	stp	x0, x1, [sp]
	stp	x29, x30, [sp, #16]
	mrs	x0, icc_iar1_el1         /* received vINTID (the SGI) */
	msr	icc_eoir1_el1, x0
	hvc	#0                       /* report INTID in x0 to the hypervisor */
	ldp	x29, x30, [sp, #16]
	ldp	x0, x1, [sp]
	add	sp, sp, #32
	eret

gipi_bad:
	b	.

/* ------------------------------------------------------------------ */
/* M15 guest: dual-core. vCPU0 = producer (does work, periodically      */
/* IPIs vCPU1 then yields); vCPU1 = consumer (does work, counts IPIs).  */
/* x0 on entry = this vCPU's id. Both are time-sliced by the EL2 timer. */
/* ------------------------------------------------------------------ */
.global guest_smp_entry
guest_smp_entry:
	mov	x22, x0                  /* cpu id */
	mov	x0, #1
	msr	icc_sre_el1, x0
	isb
	mov	x0, #0xFF
	msr	icc_pmr_el1, x0
	mov	x0, #1
	msr	icc_igrpen1_el1, x0
	adr	x0, gsmp_vectors
	msr	vbar_el1, x0
	isb
	mov	x20, #0                  /* work-units counter */
	mov	x21, #0                  /* IPIs received (consumer) */
	msr	daifclr, #2
	cbnz	x22, gsmp_consumer

gsmp_producer:                           /* vCPU0 */
	add	x20, x20, #1
	and	x1, x20, #0x1FFFFF       /* every 2,097,152 work units */
	cbnz	x1, gsmp_producer
	mov	x0, #1
	lsl	x0, x0, #24              /* SGI INTID 1 */
	mov	x1, #1
	lsl	x1, x1, #40              /* IRM = all-but-self */
	orr	x0, x0, x1
	msr	icc_sgi1r_el1, x0        /* -> traps to EL2, routed to vCPU1 */
	b	gsmp_producer

gsmp_consumer:                           /* vCPU1 */
	add	x20, x20, #1
	b	gsmp_consumer

	.balign 2048
gsmp_vectors:
	.balign 0x80
	b	gsmp_bad
	.balign 0x80
	b	gsmp_bad
	.balign 0x80
	b	gsmp_bad
	.balign 0x80
	b	gsmp_bad
	.balign 0x80
	b	gsmp_bad
	.balign 0x80
	b	gsmp_irq                 /* idx 5: Cur EL SPx IRQ */
	.balign 0x80
	b	gsmp_bad
	.balign 0x80
	b	gsmp_bad
	.balign 0x80
	b	gsmp_bad
	.balign 0x80
	b	gsmp_bad
	.balign 0x80
	b	gsmp_bad
	.balign 0x80
	b	gsmp_bad
	.balign 0x80
	b	gsmp_bad
	.balign 0x80
	b	gsmp_bad
	.balign 0x80
	b	gsmp_bad
	.balign 0x80
	b	gsmp_bad

gsmp_irq:
	sub	sp, sp, #16
	str	x0, [sp]
	mrs	x0, icc_iar1_el1
	add	x21, x21, #1             /* count received IPIs */
	msr	icc_eoir1_el1, x0
	ldr	x0, [sp]
	add	sp, sp, #16
	eret

gsmp_bad:
	b	.

/* ------------------------------------------------------------------ */
/* M16 guest: a tenant. Writes a VM-specific magic to guest IPA         */
/* 0x40001000, reads it back, and reports (x0=id, x1=value). Each VM    */
/* runs an identical copy from its own private host RAM block, so the   */
/* same IPA is physically distinct per VM. x0 on entry = VM id.         */
/* This blob is position-independent: copied to each VM's RAM by the HV.*/
/* ------------------------------------------------------------------ */
.global guest_tenant_start
.global guest_tenant_entry
.global guest_tenant_end
guest_tenant_start:
guest_tenant_entry:
	mov	x20, x0                  /* VM id */
	movz	x1, #0x4000, lsl #16     /* 0x40000000 */
	movk	x1, #0x1000              /* 0x40001000 (private scratch IPA) */
	movz	x2, #0xA5A5, lsl #16     /* 0xA5A50000 */
	orr	x2, x2, x20              /* magic = 0xA5A50000 | id */
	str	x2, [x1]
	dsb	sy
	ldr	x3, [x1]                 /* read it back */
	mov	x0, x20
	mov	x1, x3
	hvc	#0                       /* report (x0=id, x1=magic) */
1:
	wfi
	b	1b
guest_tenant_end:

/* ------------------------------------------------------------------ */
/* M18 guest: a tenant that drives its OWN emulated vGIC. Configures the */
/* (emulated, per-VM) distributor, enables INTID 27, and services the    */
/* timer interrupts the hypervisor injects, reporting a count via HVC.   */
/* Position-independent: the HV copies it into each VM's private RAM.    */
/* ------------------------------------------------------------------ */
.global guest_tvgic_start
.global guest_tvgic_entry
.global guest_tvgic_end
guest_tvgic_start:
guest_tvgic_entry:
	movz	x2, #0x0800, lsl #16     /* GICD_CTLR = ARE_NS|EnableGrp1 */
	mov	w0, #0x12
	str	w0, [x2]
	movz	x3, #0x080A, lsl #16     /* GICR_WAKER: clear ProcessorSleep */
	str	wzr, [x3, #0x14]
	movz	x4, #0x080B, lsl #16     /* GICR SGI frame */
	mov	w0, #0xFFFFFFFF
	str	w0, [x4, #0x80]          /* IGROUPR0 */
	mov	w0, #1
	lsl	w0, w0, #27
	str	w0, [x4, #0x100]         /* ISENABLER0: enable INTID 27 */
	mov	x0, #1
	msr	icc_sre_el1, x0
	isb
	mov	x0, #0xFF
	msr	icc_pmr_el1, x0
	mov	x0, #1
	msr	icc_igrpen1_el1, x0
	adr	x0, tvgic_vectors
	msr	vbar_el1, x0
	isb
	mov	x21, #0
	msr	daifclr, #2
1:
	wfi
	b	1b

	.balign 2048
tvgic_vectors:
	.balign 0x80
	b	tvgic_bad
	.balign 0x80
	b	tvgic_bad
	.balign 0x80
	b	tvgic_bad
	.balign 0x80
	b	tvgic_bad
	.balign 0x80
	b	tvgic_bad
	.balign 0x80
	b	tvgic_irq                /* idx 5: Cur EL SPx IRQ */
	.balign 0x80
	b	tvgic_bad
	.balign 0x80
	b	tvgic_bad
	.balign 0x80
	b	tvgic_bad
	.balign 0x80
	b	tvgic_bad
	.balign 0x80
	b	tvgic_bad
	.balign 0x80
	b	tvgic_bad
	.balign 0x80
	b	tvgic_bad
	.balign 0x80
	b	tvgic_bad
	.balign 0x80
	b	tvgic_bad
	.balign 0x80
	b	tvgic_bad

tvgic_irq:
	sub	sp, sp, #32
	stp	x0, x1, [sp]
	stp	x29, x30, [sp, #16]
	mrs	x0, icc_iar1_el1
	add	x21, x21, #1
	msr	icc_eoir1_el1, x0
	mov	x0, x21
	hvc	#0
	ldp	x29, x30, [sp, #16]
	ldp	x0, x1, [sp]
	add	sp, sp, #32
	eret

tvgic_bad:
	b	.
guest_tvgic_end:

/* ------------------------------------------------------------------ */
/* M22 guest: guest-driven SMP. The primary (vCPU0) brings up the       */
/* secondary (vCPU1) via PSCI CPU_ON (HVC); the secondary then sends an  */
/* SGI back to the primary, which receives it on its CPU interface.      */
/* ------------------------------------------------------------------ */
.global guest_psci_smp_entry
guest_psci_smp_entry:                    /* primary vCPU0 */
	mov	x0, #1
	msr	icc_sre_el1, x0
	isb
	mov	x0, #0xFF
	msr	icc_pmr_el1, x0
	mov	x0, #1
	msr	icc_igrpen1_el1, x0
	adr	x0, gpsci_vectors
	msr	vbar_el1, x0
	isb
	msr	daifclr, #2

	/* PSCI CPU_ON(target=1, entry=gpsci_secondary, ctx=0xCAFE) via HVC */
	movz	x0, #0xC400, lsl #16
	movk	x0, #0x0003              /* 0xC4000003 = CPU_ON */
	mov	x1, #1                   /* target CPU 1 */
	adr	x2, gpsci_secondary      /* secondary entry (guest IPA) */
	movz	x3, #0xCAFE              /* context id */
	hvc	#0
1:
	wfi
	b	1b

gpsci_secondary:                         /* secondary vCPU1, x0 = context id */
	mov	x19, x0
	mov	x0, #1
	msr	icc_sre_el1, x0
	isb
	mov	x0, #0xFF
	msr	icc_pmr_el1, x0
	mov	x0, #1
	msr	icc_igrpen1_el1, x0
	/* send SGI INTID 3 to all-but-self (-> the primary) */
	mov	x0, #3
	lsl	x0, x0, #24
	mov	x1, #1
	lsl	x1, x1, #40
	orr	x0, x0, x1
	msr	icc_sgi1r_el1, x0
2:
	wfi
	b	2b

	.balign 2048
gpsci_vectors:
	.balign 0x80
	b	gpsci_bad
	.balign 0x80
	b	gpsci_bad
	.balign 0x80
	b	gpsci_bad
	.balign 0x80
	b	gpsci_bad
	.balign 0x80
	b	gpsci_bad
	.balign 0x80
	b	gpsci_irq                /* idx 5: Cur EL SPx IRQ (primary) */
	.balign 0x80
	b	gpsci_bad
	.balign 0x80
	b	gpsci_bad
	.balign 0x80
	b	gpsci_bad
	.balign 0x80
	b	gpsci_bad
	.balign 0x80
	b	gpsci_bad
	.balign 0x80
	b	gpsci_bad
	.balign 0x80
	b	gpsci_bad
	.balign 0x80
	b	gpsci_bad
	.balign 0x80
	b	gpsci_bad
	.balign 0x80
	b	gpsci_bad

gpsci_irq:
	sub	sp, sp, #32
	stp	x0, x1, [sp]
	stp	x29, x30, [sp, #16]
	mrs	x0, icc_iar1_el1
	msr	icc_eoir1_el1, x0
	hvc	#0                       /* report received INTID (x0) */
	ldp	x29, x30, [sp, #16]
	ldp	x0, x1, [sp]
	add	sp, sp, #32
	eret

gpsci_bad:
	b	.

/* ------------------------------------------------------------------ */
/* M23 guest: drives the emulated virtio-console. Builds a 1-descriptor */
/* split virtqueue pointing at a message, programs the virtio-mmio       */
/* registers, and notifies -- the hypervisor prints the queued bytes.    */
/* ------------------------------------------------------------------ */
.global guest_virtio_entry
guest_virtio_entry:
	movz	x10, #0x4002, lsl #16    /* queue page @ guest IPA 0x40020000 */
	adr	x11, vvmsg               /* message buffer (guest IPA) */
	/* strlen -> x3 */
	mov	x3, #0
1:	ldrb	w0, [x11, x3]
	cbz	w0, 2f
	add	x3, x3, #1
	b	1b
2:	/* desc[0] = { addr=x11, len=x3, flags=0, next=0 } */
	str	x11, [x10, #0]
	str	w3, [x10, #8]
	str	wzr, [x10, #12]
	/* avail ring @ +16 (QueueNum=1): flags=0, idx=1, ring[0]=0 */
	add	x4, x10, #16
	strh	wzr, [x4, #0]
	mov	w0, #1
	strh	w0, [x4, #2]
	strh	wzr, [x4, #4]
	dsb	sy
	/* virtio-mmio handshake @ 0x0A000000 */
	movz	x5, #0x0A00, lsl #16
	mov	w0, #4096
	str	w0, [x5, #0x28]          /* GuestPageSize */
	str	wzr, [x5, #0x30]         /* QueueSel = 0  */
	mov	w0, #1
	str	w0, [x5, #0x38]          /* QueueNum = 1  */
	mov	w0, #4096
	str	w0, [x5, #0x3c]          /* QueueAlign    */
	movz	w0, #0x0004, lsl #16
	movk	w0, #0x0020             /* PFN = 0x40020 (0x40020000 >> 12) */
	str	w0, [x5, #0x40]          /* QueuePFN      */
	mov	w0, #0xF
	str	w0, [x5, #0x70]          /* Status = ACK|DRIVER|FEATURES_OK|DRIVER_OK */
	str	wzr, [x5, #0x50]         /* QueueNotify 0 -> device emits the buffer */
	hvc	#0
3:	wfi
	b	3b

	.balign 8
vvmsg:
	.asciz	"Hello from a guest, printed through an EMULATED virtio-console device!\n"
