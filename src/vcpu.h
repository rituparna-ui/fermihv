#ifndef FERMIHV_VCPU_H
#define FERMIHV_VCPU_H

#include <stdint.h>
#include <stddef.h>
#include "vcpu_offsets.h"

/*
 * Per-vCPU state. The first block (through exit_hpfar) is touched by the
 * world-switch assembly at the offsets in vcpu_offsets.h. The EL1 system
 * register bank below it is saved/restored purely in C.
 */
typedef struct vcpu {
	/* --- touched by world.S / vectors.S (keep order & offsets) --- */
	uint64_t x[31];       /* general purpose x0..x30 */
	uint64_t sp_el0;      /* guest SP_EL0            */
	uint64_t pc;          /* guest PC   (ELR_EL2)    */
	uint64_t pstate;      /* guest PSTATE (SPSR_EL2) */
	uint64_t hyp_sp;      /* saved EL2 SP for return */
	uint64_t exit_reason; /* vector index of the exit */
	uint64_t exit_esr;    /* ESR_EL2  at exit */
	uint64_t exit_far;    /* FAR_EL2  at exit */
	uint64_t exit_hpfar;  /* HPFAR_EL2 at exit */

	/* --- FP/SIMD state (saved/restored by world.S) --- */
	uint64_t _pad0;                 /* align q[] to 16 (offset 320) */
	unsigned __int128 q[32];        /* q0..q31 */
	uint64_t fpsr, fpcr;

	/* --- guest EL1 system registers (C-managed) --- */
	uint64_t sctlr_el1, cpacr_el1;
	uint64_t ttbr0_el1, ttbr1_el1, tcr_el1, mair_el1, amair_el1;
	uint64_t vbar_el1, contextidr_el1;
	uint64_t sp_el1, elr_el1, spsr_el1, esr_el1, far_el1;
	uint64_t tpidr_el0, tpidrro_el0, tpidr_el1;

	/* --- per-vCPU virtual GIC context (saved/restored across switches) --- */
	uint64_t ich_lr[4];   /* List Registers 0-3: pending/active virtual IRQs */
	uint64_t ich_vmcr;    /* virtual CPU interface control (VENG/VPMR/...)  */
	uint64_t ich_ap1r0;   /* virtual active priorities (group 1)            */

	/* --- VM config / bookkeeping --- */
	uint64_t vttbr;       /* stage-2 base + VMID for this vCPU */
	uint64_t vmpidr;      /* virtual MPIDR_EL1 (affinity) seen by the guest */
	int      id;
	int      halted;
} vcpu_t;

_Static_assert(offsetof(vcpu_t, x) == VCPU_X, "x offset");
_Static_assert(offsetof(vcpu_t, sp_el0) == VCPU_SP_EL0, "sp_el0 offset");
_Static_assert(offsetof(vcpu_t, pc) == VCPU_PC, "pc offset");
_Static_assert(offsetof(vcpu_t, pstate) == VCPU_PSTATE, "pstate offset");
_Static_assert(offsetof(vcpu_t, hyp_sp) == VCPU_HYP_SP, "hyp_sp offset");
_Static_assert(offsetof(vcpu_t, exit_reason) == VCPU_EXIT_REASON, "exit_reason");
_Static_assert(offsetof(vcpu_t, exit_esr) == VCPU_EXIT_ESR, "exit_esr offset");
_Static_assert(offsetof(vcpu_t, exit_far) == VCPU_EXIT_FAR, "exit_far offset");
_Static_assert(offsetof(vcpu_t, exit_hpfar) == VCPU_EXIT_HPFAR, "exit_hpfar");
_Static_assert(offsetof(vcpu_t, q) == VCPU_FP, "fp q offset");
_Static_assert(offsetof(vcpu_t, fpsr) == VCPU_FPSR, "fpsr offset");
_Static_assert(offsetof(vcpu_t, fpcr) == VCPU_FPCR, "fpcr offset");

/* Defined in world.S: load guest context and eret; returns here (via
 * __guest_exit) once the guest traps back to EL2. */
extern void __guest_enter(vcpu_t *v);

/* Initialise a vCPU to start executing at `entry` in EL1h. */
void vcpu_init(vcpu_t *v, uint64_t entry, uint64_t sp_el1, uint64_t vttbr,
               int id);

/* Restore guest sysregs + stage-2, enter the guest, and on exit save guest
 * sysregs. Exit info is left in v->exit_* for the caller to handle. */
void vcpu_run_once(vcpu_t *v);

/* M4 demo: run two vCPUs round-robin and show independent preserved state. */
void sched_demo(void);

/* M6b demo: deliver `nticks` virtual timer interrupts to a guest. */
void virq_demo(int nticks);

/* M13 demo: guest drives an emulated GICv3 distributor (software vGIC). */
void vgic_demo(int nticks);

/* M14 demo: virtual IPI (SGI) from one vCPU to another via the vGIC. */
void smp_demo(void);

/* M15 demo: a running dual-core guest -- two time-sliced vCPUs + IPIs. */
void smp_sched_demo(void);

/* M16 demo: two isolated VMs running concurrently (multi-tenancy). */
void mtenant_demo(void);

/* M18 demo: two isolated VMs each on its OWN per-VM emulated vGIC. */
void mtenant_vgic_demo(void);

/* M19 demo: a real OS (fermi-os) as one of two concurrent isolated tenants. */
void mtenant_real_demo(void);

/* M7 demo: load a separately-built guest kernel image and boot it at EL1. */
void real_guest_demo(void);

/* M7: boot a real Linux kernel (image+DTB placed in RAM by the loader). */
void linux_boot(void);

/* M20: boot Linux on the fully emulated vGIC (no device passthrough). */
void linux_vgic_boot(void);

/* M21: Linux + fermi-os as two concurrent isolated co-tenants. */
void mtenant_os_demo(void);

/* M22 demo: guest-driven SMP -- a guest boots its secondary via PSCI CPU_ON. */
void smp_psci_demo(void);

/* M23 demo: a guest prints through an emulated virtio-console device. */
void virtio_demo(void);

/* M24 demo: interrupt-driven virtio-console (used ring + completion IRQ). */
void virtio_irq_demo(void);

/* M25 demo: virtio-blk -- a guest writes a sector through a request chain. */
void vblk_demo(void);

/* M26 demo: virtio-blk READ -- guest reads a sector the hypervisor seeded. */
void vblk_rd_demo(void);

/* M27 demo: per-VM virtio -- two isolated tenants, each with its own console. */
void mtenant_virtio_demo(void);

/* M28 demo: per-VM virtio-blk -- two isolated tenants, each with its own disk. */
void mtenant_vblk_demo(void);

/* M29 demo: SMP virtual timer -- two vCPUs in one VM, per-vCPU GIC context. */
void smp_vtimer_demo(void);

/* M30 demo: SMP redistributors -- two vCPUs, each with its own GICR frame. */
void smp_gicr_demo(void);

/* M12: boot fermi-os (loaded at 0x40000000) as a guest. */
void fermios_boot(void);

/* M17: boot fermi-os on the fully emulated vGIC (no GIC/UART passthrough). */
void fermios_vgic_boot(void);

#endif /* FERMIHV_VCPU_H */
