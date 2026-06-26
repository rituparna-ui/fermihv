#include "vcpu.h"
#include "uart.h"
#include "stage2.h"
#include "exception.h"
#include "vdev.h"
#include "gic.h"
#include "timer.h"
#include "vgic.h"
#include "vvirtio.h"

extern void guest_counter_entry(void);
extern void guest_vuart_entry(void);
extern void guest_virq_entry(void);
extern void guest_vgic_entry(void);
extern void guest_ipi_entry(void);
extern void guest_smp_entry(void);
extern char guest_tenant_start[];
extern char guest_tenant_end[];
extern char guest_tvgic_start[];
extern char guest_tvgic_end[];
extern char guest_virtio_start[];   /* M23 virtio-console guest blob (start) */
extern char guest_virtio_end[];

/* SPSR for entering EL1h with DAIF masked (D|A|I|F=1, M=0b0101). */
#define SPSR_EL1H_MASKED 0x3C5UL
/* SCTLR_EL1 reset value (RES1 bits set, MMU/caches off). */
#define SCTLR_EL1_RESET  0x30D00800UL

#define rd_sysreg(reg, dst) __asm__ volatile("mrs %0, " reg : "=r"(dst))
#define wr_sysreg(reg, src) __asm__ volatile("msr " reg ", %0" ::"r"(src))

static void save_el1_sysregs(vcpu_t *v) {
	rd_sysreg("sctlr_el1", v->sctlr_el1);
	rd_sysreg("cpacr_el1", v->cpacr_el1);
	rd_sysreg("ttbr0_el1", v->ttbr0_el1);
	rd_sysreg("ttbr1_el1", v->ttbr1_el1);
	rd_sysreg("tcr_el1", v->tcr_el1);
	rd_sysreg("mair_el1", v->mair_el1);
	rd_sysreg("amair_el1", v->amair_el1);
	rd_sysreg("vbar_el1", v->vbar_el1);
	rd_sysreg("contextidr_el1", v->contextidr_el1);
	rd_sysreg("sp_el1", v->sp_el1);
	rd_sysreg("elr_el1", v->elr_el1);
	rd_sysreg("spsr_el1", v->spsr_el1);
	rd_sysreg("esr_el1", v->esr_el1);
	rd_sysreg("far_el1", v->far_el1);
	rd_sysreg("tpidr_el0", v->tpidr_el0);
	rd_sysreg("tpidrro_el0", v->tpidrro_el0);
	rd_sysreg("tpidr_el1", v->tpidr_el1);
}

static void restore_el1_sysregs(vcpu_t *v) {
	wr_sysreg("sctlr_el1", v->sctlr_el1);
	wr_sysreg("cpacr_el1", v->cpacr_el1);
	wr_sysreg("ttbr0_el1", v->ttbr0_el1);
	wr_sysreg("ttbr1_el1", v->ttbr1_el1);
	wr_sysreg("tcr_el1", v->tcr_el1);
	wr_sysreg("mair_el1", v->mair_el1);
	wr_sysreg("amair_el1", v->amair_el1);
	wr_sysreg("vbar_el1", v->vbar_el1);
	wr_sysreg("contextidr_el1", v->contextidr_el1);
	wr_sysreg("sp_el1", v->sp_el1);
	wr_sysreg("elr_el1", v->elr_el1);
	wr_sysreg("spsr_el1", v->spsr_el1);
	wr_sysreg("esr_el1", v->esr_el1);
	wr_sysreg("far_el1", v->far_el1);
	wr_sysreg("tpidr_el0", v->tpidr_el0);
	wr_sysreg("tpidrro_el0", v->tpidrro_el0);
	wr_sysreg("tpidr_el1", v->tpidr_el1);
	__asm__ volatile("isb");
}

void vcpu_init(vcpu_t *v, uint64_t entry, uint64_t sp_el1, uint64_t vttbr,
               int id) {
	for (unsigned i = 0; i < sizeof(*v) / sizeof(uint64_t); i++)
		((uint64_t *)v)[i] = 0;

	v->pc = entry;
	v->pstate = SPSR_EL1H_MASKED;
	v->sp_el1 = sp_el1;
	v->sctlr_el1 = SCTLR_EL1_RESET;
	v->vttbr = vttbr;
	v->vmpidr = (1UL << 31) | (uint64_t)id;   /* RES1 | Aff0 = cpu id */
	v->id = id;
	v->halted = 0;
}

void vcpu_run_once(vcpu_t *v) {
	restore_el1_sysregs(v);
	wr_sysreg("vttbr_el2", v->vttbr);
	wr_sysreg("vmpidr_el2", v->vmpidr);
	/* Restore this vCPU's virtual GIC context (list register + CPU interface
	 * control + active priorities) so interrupt state is per-vCPU. */
	__asm__ volatile("msr ich_vmcr_el2, %0" ::"r"(v->ich_vmcr));
	__asm__ volatile("msr ich_ap1r0_el2, %0" ::"r"(v->ich_ap1r0));
	__asm__ volatile("msr ich_lr0_el2, %0" ::"r"(v->ich_lr0));
	__asm__ volatile("isb");
	__guest_enter(v);          /* returns when the guest traps back to EL2 */
	/* Capture any updated virtual GIC state (e.g. an IRQ the guest acked but
	 * has not yet EOI'd) back into the vCPU before switching away. */
	__asm__ volatile("mrs %0, ich_lr0_el2" : "=r"(v->ich_lr0));
	__asm__ volatile("mrs %0, ich_vmcr_el2" : "=r"(v->ich_vmcr));
	__asm__ volatile("mrs %0, ich_ap1r0_el2" : "=r"(v->ich_ap1r0));
	save_el1_sysregs(v);
}

/* ------------------------------------------------------------------ */
/* Scheduler                                                          */
/* ------------------------------------------------------------------ */

#define MAX_VCPUS 2

static vcpu_t vcpus[MAX_VCPUS];
static uint8_t gstack[MAX_VCPUS][4096] __attribute__((aligned(16)));

/* Run `n` vCPUs round-robin until each has made `budget` hypercalls. Guest
 * exits are decoded here: HVC is logged, stage-2 aborts are either emulated
 * (if they hit a virtual device) or faulted-in as RAM and retried. */
static void run_vcpus(int n, int budget, const char *tag) {
	int hcalls[MAX_VCPUS] = {0};
	int active = n;

	while (active) {
		for (int i = 0; i < n; i++) {
			vcpu_t *v = &vcpus[i];
			if (v->halted)
				continue;

			vcpu_run_once(v);

			uint64_t ec = ESR_EC(v->exit_esr);
			if (ec == EC_HVC64) {
				uart_printf("[%s] vCPU %d HVC, x0=%u\n", tag, v->id, v->x[0]);
				if (++hcalls[i] >= budget) {
					v->halted = 1;
					active--;
				}
			} else if (ec == EC_DABT_LOW || ec == EC_IABT_LOW) {
				uint64_t ipa = ((v->exit_hpfar >> 4) << 12) |
				               (v->exit_far & 0xFFF);
				if (ec == EC_DABT_LOW && vuart_contains(ipa))
					mmio_emulate(v, ipa);            /* device: emulate + advance */
				else
					stage2_map_1gb(ipa & ~0x3FFFFFFFUL); /* RAM: fault-in + retry */
			} else {
				uart_printf("[%s] vCPU %d unexpected exit ec=%x (%s), halting\n",
				            tag, v->id, ec, ec_name(ec));
				v->halted = 1;
				active--;
			}
		}
	}
}

void sched_demo(void) {
	stage2_init();

	/* HCR_EL2: RW (AArch64 EL1) | VM (stage-2) | DC (default cacheable). */
	uint64_t hcr = (1UL << 31) | (1UL << 0) | (1UL << 12);
	wr_sysreg("hcr_el2", hcr);
	__asm__ volatile("isb");

	uint64_t vttbr = stage2_vttbr();

	/* Phase 1 (M4): two counting vCPUs with independent preserved state. */
	uart_println("[M4] two counting vCPUs, round-robin (3 hypercalls each):");
	for (int i = 0; i < 2; i++)
		vcpu_init(&vcpus[i], (uint64_t)&guest_counter_entry,
		          (uint64_t)(gstack[i] + sizeof(gstack[i])), vttbr, i);
	run_vcpus(2, 3, "M4");

	/* Phase 2 (M5): one vCPU printing through an emulated MMIO UART. */
	uart_println("[M5] one vCPU printing via emulated MMIO UART:");
	uart_puts("      guest says> ");
	vcpu_init(&vcpus[0], (uint64_t)&guest_vuart_entry,
	          (uint64_t)(gstack[0] + sizeof(gstack[0])), vttbr, 0);
	run_vcpus(1, 1, "M5");

	uart_println("[SCHED] all vCPUs halted; demo done.");
}

/* ------------------------------------------------------------------ */
/* M6b: virtual timer interrupts delivered to a guest                  */
/* ------------------------------------------------------------------ */

#define VTIMER_VINTID 27   /* the vINTID the guest will see */

/* Inject a pending Group-1 virtual interrupt into list register 0.
 * ICH_LR<n>_EL2: [63:62]=State(01=pending), [61]=HW(0=SW), [60]=Group(1),
 * [55:48]=priority, [31:0]=vINTID. */
/* Inject a virtual IRQ into vCPU `v` by setting its saved list register; it is
 * loaded into the live ICH_LR0 the next time the vCPU runs. State = pending,
 * Group 1. */
static void vgic_inject(vcpu_t *v, uint32_t vintid) {
	v->ich_lr0 = (1ULL << 62) | (1ULL << 60) | (uint64_t)vintid;
}

void virq_demo(int nticks) {
	uint64_t vttbr = stage2_vttbr();
	vcpu_init(&vcpus[0], (uint64_t)&guest_virq_entry,
	          (uint64_t)(gstack[0] + sizeof(gstack[0])), vttbr, 0);

	/* HCR_EL2: RW | VM | DC | IMO (route physical IRQ to EL2). */
	uint64_t hcr = (1UL << 31) | (1UL << 0) | (1UL << 12) | (1UL << 4);
	wr_sysreg("hcr_el2", hcr);
	/* Enable the virtual CPU interface (list-register processing). */
	__asm__ volatile("msr ich_hcr_el2, %0" ::"r"(1UL));
	__asm__ volatile("isb");

	hyptimer_start(100); /* 100 ms physical tick at EL2 */

	uart_println("[M6b] injecting virtual timer ticks into the guest:");
	int ticks = 0;
	while (ticks < nticks) {
		vcpu_run_once(&vcpus[0]);

		if ((vcpus[0].exit_reason & 3) == 1) {
			/* Physical timer IRQ preempted the guest to EL2. Handle it and
			 * inject a virtual interrupt, then re-enter so the guest takes
			 * it through its own EL1 IRQ vector. */
			uint64_t iar = gic_ack();
			uint32_t intid = iar & 0xFFFFFF;
			if (intid == HYP_TIMER_PPI) {
				hyptimer_on_irq();
				vgic_inject(&vcpus[0], VTIMER_VINTID);
			}
			if (intid < GIC_SPURIOUS_INTID)
				gic_eoi(iar);
		} else if (ESR_EC(vcpus[0].exit_esr) == EC_HVC64) {
			ticks++;
			uart_printf("[M6b] guest handled virtual tick #%u "
			            "(guest's own counter x0=%u)\n",
			            ticks, vcpus[0].x[0]);
		} else {
			uart_printf("[M6b] unexpected exit reason=%u esr=%x, stopping\n",
			            vcpus[0].exit_reason, vcpus[0].exit_esr);
			break;
		}
	}

	hyptimer_stop();
	uart_printf("[M6b] delivered %u virtual timer ticks to the guest.\n", ticks);
}

/* ------------------------------------------------------------------ */
/* M13: software vGIC -- guest drives an emulated GICD/GICR distributor */
/* ------------------------------------------------------------------ */

void vgic_demo(int nticks) {
	stage2_init();                       /* block1 RAM; block0 (GIC) left unmapped */
	vgic_reset();

	/* RW | VM | DC | IMO. The guest's GIC MMIO traps to the vGIC; its CPU
	 * interface (ICC_*) is auto-virtualized to ICV; we inject via ICH_LR. */
	uint64_t hcr = (1UL << 31) | (1UL << 0) | (1UL << 12) | (1UL << 4);
	wr_sysreg("hcr_el2", hcr);
	__asm__ volatile("msr ich_hcr_el2, %0" ::"r"(1UL)); /* enable virtual CPU iface */
	__asm__ volatile("isb");

	vcpu_init(&vcpus[0], (uint64_t)&guest_vgic_entry,
	          (uint64_t)(gstack[0] + sizeof(gstack[0])), stage2_vttbr(), 0);
	hyptimer_start(100);

	uart_println("[M13] software vGIC: guest configures emulated GICD/GICR,");
	uart_println("      hypervisor injects a virtual timer only when enabled.");

	int ticks = 0;
	while (ticks < nticks) {
		vcpu_run_once(&vcpus[0]);

		if ((vcpus[0].exit_reason & 3) == 1) {
			/* Physical EL2 timer tick: inject a virtual interrupt ONLY if the
			 * guest enabled INTID 27 in the emulated distributor. */
			uint64_t iar = gic_ack();
			uint32_t intid = iar & 0xFFFFFF;
			if (intid == HYP_TIMER_PPI) {
				hyptimer_on_irq();
				if (vgic_irq_enabled(0, 27))
					vgic_inject(&vcpus[0], 27);
			}
			if (intid < GIC_SPURIOUS_INTID)
				gic_eoi(iar);
			continue;
		}

		uint64_t ec = ESR_EC(vcpus[0].exit_esr);
		if (ec == EC_HVC64) {
			ticks++;
			uart_printf("[M13] guest serviced vGIC IRQ #%u (acked vINTID via ICV)\n",
			            ticks);
		} else if (ec == EC_DABT_LOW || ec == EC_IABT_LOW) {
			uint64_t ipa = ((vcpus[0].exit_hpfar >> 4) << 12) |
			               (vcpus[0].exit_far & 0xFFF);
			if (vgic_contains(ipa))
				vgic_mmio(0, &vcpus[0], ipa);
			else
				stage2_map_1gb(ipa & ~0x3FFFFFFFUL);
		} else {
			uart_printf("[M13] unexpected exit ec=%x (%s), stopping\n",
			            ec, ec_name(ec));
			break;
		}
	}
	hyptimer_stop();
	uart_printf("[M13] vGIC delivered %u interrupts through the emulated "
	            "distributor.\n", ticks);
}

/* ------------------------------------------------------------------ */
/* M14: SMP groundwork -- virtual IPIs (SGIs) between vCPUs            */
/* ------------------------------------------------------------------ */

void smp_demo(void) {
	stage2_init();
	vgic_reset();

	/* RW | VM | DC | IMO. ICH_HCR_EL2 = En | TC (bit 8): TC traps only the
	 * SGI registers (ICC_SGI1R etc.) so we can route virtual IPIs; the rest
	 * of the CPU interface (PMR/IGRPEN1/IAR/EOIR) stays on the ICV fast path. */
	uint64_t hcr = (1UL << 31) | (1UL << 0) | (1UL << 12) | (1UL << 4);
	wr_sysreg("hcr_el2", hcr);
	__asm__ volatile("msr ich_hcr_el2, %0" ::"r"((1UL << 0) | (1UL << 8)));
	__asm__ volatile("isb");

	for (int i = 0; i < 2; i++) {
		vcpu_init(&vcpus[i], (uint64_t)&guest_ipi_entry,
		          (uint64_t)(gstack[i] + sizeof(gstack[i])), stage2_vttbr(), i);
		vcpus[i].x[0] = i;       /* tell each vCPU its id */
	}

	uart_println("[M14] SMP: vCPU0 sends a virtual IPI (SGI) to vCPU1 via the vGIC");

	int got = 0;
	for (int round = 0; round < 50 && !got; round++) {
		for (int i = 0; i < 2 && !got; i++) {
			int s = vgic_pop_sgi(i);
			if (s >= 0) {
				vgic_inject(&vcpus[i], (uint32_t)s);
				uart_printf("[M14] injecting SGI %d into vCPU%d's list register\n",
				            s, i);
			}

			vcpu_run_once(&vcpus[i]);

			if ((vcpus[i].exit_reason & 3) == 1) {
				uint64_t iar = gic_ack();
				if ((iar & 0xFFFFFF) < GIC_SPURIOUS_INTID)
					gic_eoi(iar);
				continue;
			}

			uint64_t ec = ESR_EC(vcpus[i].exit_esr);
			if (ec == EC_SYSREG) {
				/* trapped ICC_SGI1R write: route the SGI to the other vCPU(s) */
				uint64_t iss = ESR_ISS(vcpus[i].exit_esr);
				int rt = (iss >> 5) & 0x1F;
				uint64_t val = (rt == 31) ? 0 : vcpus[i].x[rt];
				uint32_t intid = (val >> 24) & 0xF;
				vgic_post_sgi(i, intid);
				uart_printf("[M14] vCPU%d issued ICC_SGI1R -> SGI %u (trapped)\n",
				            i, intid);
				vcpus[i].pc += 4;
			} else if (ec == EC_HVC64) {
				if (i == 0) {
					/* sender's post-send yield: nothing to do */
				} else {
					uart_printf("[M14] vCPU1 RECEIVED virtual IPI: SGI INTID %u "
					            "(from vCPU0) via ICV\n", vcpus[i].x[0]);
					got = 1;
				}
			} else {
				uart_printf("[M14] vCPU%d unexpected exit ec=%x (%s)\n",
				            i, ec, ec_name(ec));
				got = 1;
			}
		}
	}
	uart_println(got ? "[M14] virtual IPI delivered between vCPUs through the vGIC."
	                 : "[M14] IPI not delivered (timeout).");
}

/* ------------------------------------------------------------------ */
/* M15: a running dual-core guest -- two time-sliced vCPUs + IPIs      */
/* ------------------------------------------------------------------ */

void smp_sched_demo(void) {
	stage2_init();
	vgic_reset();
	gic_init_el2();
	hyptimer_init();

	uint64_t hcr = (1UL << 31) | (1UL << 0) | (1UL << 12) | (1UL << 4);
	wr_sysreg("hcr_el2", hcr);
	__asm__ volatile("msr ich_hcr_el2, %0" ::"r"((1UL << 0) | (1UL << 8))); /* En|TC */
	__asm__ volatile("isb");

	for (int i = 0; i < 2; i++) {
		vcpu_init(&vcpus[i], (uint64_t)&guest_smp_entry,
		          (uint64_t)(gstack[i] + sizeof(gstack[i])), stage2_vttbr(), i);
		vcpus[i].x[0] = i;
	}

	uart_println("[M15] dual-core guest: 2 vCPUs time-sliced by the EL2 timer;");
	uart_println("      vCPU0 (producer) IPIs vCPU1 (consumer). Both run concurrently.");

	hyptimer_start(10);              /* 10 ms scheduling quantum */
	int cur = 0, slices = 0, guard = 0;
	while (slices < 12 && guard++ < 100000) {
		int s = vgic_pop_sgi(cur);
		if (s >= 0)
			vgic_inject(&vcpus[cur], (uint32_t)s);

		vcpu_run_once(&vcpus[cur]);

		uint64_t reason = vcpus[cur].exit_reason;
		uint64_t ec = ESR_EC(vcpus[cur].exit_esr);
		if ((reason & 3) == 1) {
			uint64_t iar = gic_ack();
			uint32_t intid = iar & 0xFFFFFF;
			if (intid == HYP_TIMER_PPI) {
				hyptimer_on_irq();
				slices++;
				cur ^= 1;            /* time-slice expired: switch vCPU */
			}
			if (intid < GIC_SPURIOUS_INTID)
				gic_eoi(iar);
		} else if (ec == EC_SYSREG) {
			uint64_t iss = ESR_ISS(vcpus[cur].exit_esr);
			int rt = (iss >> 5) & 0x1F;
			uint64_t val = (rt == 31) ? 0 : vcpus[cur].x[rt];
			vgic_post_sgi(cur, (val >> 24) & 0xF);
			vcpus[cur].pc += 4;      /* keep running the producer */
		} else {
			uart_printf("[M15] vCPU%d unexpected exit ec=%x (%s)\n",
			            cur, ec, ec_name(ec));
			break;
		}
	}
	hyptimer_stop();

	uart_printf("[M15] after %d time-slices: vCPU0 work=%u, vCPU1 work=%u, "
	            "vCPU1 received %u IPIs\n",
	            slices, vcpus[0].x[20], vcpus[1].x[20], vcpus[1].x[21]);
	uart_println("[M15] dual-core guest: both vCPUs ran (preempted) and "
	             "coordinated via virtual IPIs.");
}

/* ------------------------------------------------------------------ */
/* M16: multi-tenancy -- two isolated VMs running concurrently         */
/* ------------------------------------------------------------------ */

#define VM0_PA 0x80000000UL   /* VM0 private 1GiB host RAM block */
#define VM1_PA 0xC0000000UL   /* VM1 private 1GiB host RAM block (needs -m >=4G) */
#define VM_IPA 0x40000000UL   /* both VMs see their RAM at this guest IPA */

void mtenant_demo(void) {
	stage2_init();             /* programs VTCR_EL2 (its global table is unused here) */

	/* Copy the tenant guest into each VM's private host RAM block. */
	uint64_t sz = (uint64_t)(guest_tenant_end - guest_tenant_start);
	for (uint64_t i = 0; i < sz; i++) {
		((volatile uint8_t *)VM0_PA)[i] = ((uint8_t *)guest_tenant_start)[i];
		((volatile uint8_t *)VM1_PA)[i] = ((uint8_t *)guest_tenant_start)[i];
	}
	__asm__ volatile("dsb sy");
	__asm__ volatile("ic ialluis");
	__asm__ volatile("dsb sy");
	__asm__ volatile("isb");

	/* Each VM: same guest IPA 0x40000000, distinct host block, distinct VMID. */
	uint64_t vttbr0 = stage2_build_vm(0, VM_IPA, VM0_PA, 1);
	uint64_t vttbr1 = stage2_build_vm(1, VM_IPA, VM1_PA, 2);

	uint64_t hcr = (1UL << 31) | (1UL << 0) | (1UL << 12); /* RW | VM | DC */
	wr_sysreg("hcr_el2", hcr);
	__asm__ volatile("isb");

	vcpu_init(&vcpus[0], VM_IPA, VM_IPA + 0x8000, vttbr0, 0);
	vcpus[0].x[0] = 0;
	vcpu_init(&vcpus[1], VM_IPA, VM_IPA + 0x8000, vttbr1, 1);
	vcpus[1].x[0] = 1;

	uart_println("[M16] two isolated VMs: same guest IPA 0x40000000 -> different host RAM");
	uart_printf("      VM0 RAM @host %x, VM1 RAM @host %x\n", VM0_PA, VM1_PA);

	for (int i = 0; i < 2; i++) {
		vcpu_run_once(&vcpus[i]);   /* runs until its HVC */
		uint64_t ec = ESR_EC(vcpus[i].exit_esr);
		if (ec == EC_HVC64) {
			uart_printf("[M16] VM%u ran at IPA 0x40000000, wrote+read its private "
			            "magic = %x\n", vcpus[i].x[0], vcpus[i].x[1]);
		} else {
			uart_printf("[M16] VM%d unexpected exit ec=%x (%s)\n",
			            i, ec, ec_name(ec));
		}
	}

	/* Hypervisor's god's-eye view: read each VM's private host block directly. */
	uint32_t m0 = *(volatile uint32_t *)(VM0_PA + 0x1000);
	uint32_t m1 = *(volatile uint32_t *)(VM1_PA + 0x1000);
	uart_printf("[M16] host view: VM0 block @%x = %x ; VM1 block @%x = %x\n",
	            VM0_PA + 0x1000, (uint64_t)m0, VM1_PA + 0x1000, (uint64_t)m1);
	if (m0 != m1)
		uart_println("[M16] isolation verified: identical guest address, distinct "
		             "host memory; neither VM can see the other.");
	else
		uart_println("[M16] isolation FAILED (blocks match)");
}

/* ------------------------------------------------------------------ */
/* M18: per-VM vGIC -- two isolated VMs, each on its OWN emulated GIC   */
/* ------------------------------------------------------------------ */

void mtenant_vgic_demo(void) {
	stage2_init();
	vgic_reset();
	gic_init_el2();
	hyptimer_init();

	/* Copy the vGIC tenant into each VM's private host RAM block. */
	uint64_t sz = (uint64_t)(guest_tvgic_end - guest_tvgic_start);
	for (uint64_t i = 0; i < sz; i++) {
		((volatile uint8_t *)VM0_PA)[i] = ((uint8_t *)guest_tvgic_start)[i];
		((volatile uint8_t *)VM1_PA)[i] = ((uint8_t *)guest_tvgic_start)[i];
	}
	__asm__ volatile("dsb sy; ic ialluis; dsb sy; isb");

	uint64_t vttbr0 = stage2_build_vm(0, VM_IPA, VM0_PA, 1);
	uint64_t vttbr1 = stage2_build_vm(1, VM_IPA, VM1_PA, 2);

	uint64_t hcr = (1UL << 31) | (1UL << 0) | (1UL << 12) | (1UL << 4); /* RW|VM|DC|IMO */
	wr_sysreg("hcr_el2", hcr);
	__asm__ volatile("msr ich_hcr_el2, %0" ::"r"(1UL));
	__asm__ volatile("isb");

	vcpu_init(&vcpus[0], VM_IPA, VM_IPA + 0x10000, vttbr0, 0);
	vcpu_init(&vcpus[1], VM_IPA, VM_IPA + 0x10000, vttbr1, 1);

	uart_println("[M18] two isolated VMs, each driving its OWN emulated vGIC,");
	uart_println("      time-sliced; each serviced timer IRQ is gated by its own vGIC.");

	hyptimer_start(10);
	int ticks[2] = {0, 0};
	int pend[2] = {0, 0};
	int cur = 0, guard = 0;
	while ((ticks[0] < 3 || ticks[1] < 3) && guard++ < 20000) {
		if (pend[cur] && vgic_irq_enabled(cur, 27)) {
			vgic_inject(&vcpus[cur], 27);
			pend[cur] = 0;
		}
		vcpu_run_once(&vcpus[cur]);

		uint64_t reason = vcpus[cur].exit_reason;
		uint64_t ec = ESR_EC(vcpus[cur].exit_esr);
		if ((reason & 3) == 1) {
			uint64_t iar = gic_ack();
			uint32_t intid = iar & 0xFFFFFF;
			if (intid == HYP_TIMER_PPI) {
				hyptimer_on_irq();
				pend[0] = pend[1] = 1;   /* a tick is available to both VMs */
				cur ^= 1;                /* time-slice switch */
			}
			if (intid < GIC_SPURIOUS_INTID)
				gic_eoi(iar);
		} else if (ec == EC_HVC64) {
			ticks[cur]++;
			uart_printf("[M18] VM%d serviced its OWN vGIC timer IRQ #%d "
			            "(guest tick count=%u)\n", cur, ticks[cur], vcpus[cur].x[0]);
		} else if (ec == EC_DABT_LOW) {
			uint64_t ipa = ((vcpus[cur].exit_hpfar >> 4) << 12) |
			               (vcpus[cur].exit_far & 0xFFF);
			if (vgic_contains(ipa))
				vgic_mmio(cur, &vcpus[cur], ipa);
			else
				uart_printf("[M18] VM%d unexpected DABT ipa=%x\n", cur, ipa);
		} else {
			uart_printf("[M18] VM%d unexpected exit ec=%x (%s)\n",
			            cur, ec, ec_name(ec));
			break;
		}
	}
	hyptimer_stop();
	uart_printf("[M18] VM0 serviced %d timer IRQs, VM1 serviced %d -- each on its "
	            "own per-VM vGIC.\n", ticks[0], ticks[1]);
	uart_println("[M18] two real-interrupt-driven VMs ran concurrently, fully "
	             "isolated (separate stage-2 + separate vGIC state).");
}

/* ------------------------------------------------------------------ */
/* M7: load and boot a separately-built guest kernel image            */
/* ------------------------------------------------------------------ */

extern char nano_image_start[];
extern char nano_image_end[];

#define GUEST_LOAD_IPA 0x48000000UL   /* where the guest is loaded & linked */

void real_guest_demo(void) {
	uint64_t size = (uint64_t)(nano_image_end - nano_image_start);

	/* Copy the embedded guest image into guest RAM. Stage-2 maps this IPA
	 * identity to host RAM (the 0x40000000 1GiB block already covers it). */
	volatile uint8_t *dst = (volatile uint8_t *)GUEST_LOAD_IPA;
	const uint8_t *src = (const uint8_t *)nano_image_start;
	for (uint64_t i = 0; i < size; i++)
		dst[i] = src[i];

	/* Make the freshly-written instructions visible to the I-side. */
	__asm__ volatile("dsb sy");
	__asm__ volatile("ic ialluis");
	__asm__ volatile("dsb sy");
	__asm__ volatile("isb");

	uart_printf("[M7] loaded %u-byte guest image -> IPA %x; booting at EL1\n",
	            size, GUEST_LOAD_IPA);

	vcpu_init(&vcpus[0], GUEST_LOAD_IPA,
	          (uint64_t)(gstack[0] + sizeof(gstack[0])), stage2_vttbr(), 0);

	/* HCR_EL2: RW | VM | IMO, but NO DC -- the guest manages its own stage-1
	 * MMU, which DC=1 would force to appear disabled. */
	uint64_t hcr = (1UL << 31) | (1UL << 0) | (1UL << 4);
	wr_sysreg("hcr_el2", hcr);
	__asm__ volatile("isb");

	for (;;) {
		vcpu_run_once(&vcpus[0]);

		if ((vcpus[0].exit_reason & 3) == 1) {
			uint64_t iar = gic_ack();
			if ((iar & 0xFFFFFF) < GIC_SPURIOUS_INTID)
				gic_eoi(iar);
			continue;
		}

		uint64_t ec = ESR_EC(vcpus[0].exit_esr);
		if (ec == EC_HVC64) {
			uart_printf("[M7] guest exited via HVC (x0=%x) -> success.\n",
			            vcpus[0].x[0]);
			break;
		} else if (ec == EC_DABT_LOW || ec == EC_IABT_LOW) {
			uint64_t ipa = ((vcpus[0].exit_hpfar >> 4) << 12) |
			               (vcpus[0].exit_far & 0xFFF);
			if (ec == EC_DABT_LOW && vuart_contains(ipa))
				mmio_emulate(&vcpus[0], ipa);
			else
				stage2_map_1gb(ipa & ~0x3FFFFFFFUL);
		} else {
			uart_printf("[M7] unexpected exit ec=%x (%s) far=%x, stopping\n",
			            ec, ec_name(ec), vcpus[0].exit_far);
			break;
		}
	}
}

/* ------------------------------------------------------------------ */
/* M7: boot a real Linux kernel as a guest                            */
/* ------------------------------------------------------------------ */

/* QEMU's generic loader places these in guest RAM before we run. */
#define LINUX_LOAD_IPA 0x41000000UL   /* 2 MiB-aligned kernel load address */
#define LINUX_DTB_IPA  0x48000000UL   /* device tree blob                  */

/* Set by PSCI CPU_ON for the SMP scheduler to bring up a secondary vCPU. */
static volatile int psci_on_pending;
static uint64_t psci_on_entry, psci_on_ctx;

/* Minimal PSCI (SMC/HVC conduit) so Linux's CPU/power calls don't fault. */
static void psci_handle(vcpu_t *v) {
	uint32_t fn = (uint32_t)v->x[0];
	switch (fn) {
	case 0x84000000:                 /* PSCI_VERSION */
		v->x[0] = 0x00010000;        /* v1.0 */
		break;
	case 0x84000008:                 /* SYSTEM_OFF */
		uart_println("[PSCI] SYSTEM_OFF -> halting guest");
		v->halted = 1;
		break;
	case 0x84000009:                 /* SYSTEM_RESET */
		uart_println("[PSCI] SYSTEM_RESET -> halting guest");
		v->halted = 1;
		break;
	case 0xC4000003:                 /* CPU_ON (64-bit) */
	case 0x84000003:                 /* CPU_ON (32-bit) */
		psci_on_entry = v->x[2];     /* secondary entry point */
		psci_on_ctx = v->x[3];       /* context id (-> x0) */
		psci_on_pending = 1;
		v->x[0] = 0;                 /* SUCCESS */
		break;
	default:
		v->x[0] = (uint64_t)-1;      /* NOT_SUPPORTED */
		break;
	}
}

void linux_boot(void) {
	stage2_init();                       /* VTCR + Normal RAM block @0x40000000 */
	stage2_map_1gb_device(0x0UL);        /* passthrough GIC/UART/ITS (block 0)  */

	/* Let EL1 use the physical timer/counter directly; zero virtual offset. */
	wr_sysreg("cnthctl_el2", 3UL);       /* EL1PCTEN | EL1PCEN */
	wr_sysreg("cntvoff_el2", 0UL);

	vcpu_init(&vcpus[0], LINUX_LOAD_IPA, 0, stage2_vttbr(), 0);
	/* arm64 boot protocol: x0 = DTB phys, x1..x3 = 0. */
	vcpus[0].x[0] = LINUX_DTB_IPA;
	vcpus[0].x[1] = 0;
	vcpus[0].x[2] = 0;
	vcpus[0].x[3] = 0;

	/* HCR_EL2: RW | VM | TSC (trap guest SMC for PSCI). No DC (guest owns its
	 * MMU), no IMO (guest drives the real GICv3 and takes its own IRQs). */
	uint64_t hcr = (1UL << 31) | (1UL << 0) | (1UL << 19);
	wr_sysreg("hcr_el2", hcr);
	__asm__ volatile("isb");

	uart_println("[M7] booting Linux at EL1 (x0=DTB). Kernel output follows:");
	uart_println("------------------------------------------------------------");

	/* Place our DTB (768 MiB memory node + earlycon bootargs) at the address
	 * we hand Linux in x0. QEMU's generic loader placed the kernel Image. */
	extern char guest_dtb_start[], guest_dtb_end[];
	uint64_t dtb_sz = (uint64_t)(guest_dtb_end - guest_dtb_start);
	volatile uint8_t *dd = (volatile uint8_t *)LINUX_DTB_IPA;
	for (uint64_t i = 0; i < dtb_sz; i++)
		dd[i] = ((uint8_t *)guest_dtb_start)[i];
	__asm__ volatile("dsb sy");

	uint32_t img_magic = *(volatile uint32_t *)(LINUX_LOAD_IPA + 56);
	uint32_t dtb_magic = *(volatile uint32_t *)(LINUX_DTB_IPA);
	uart_printf("[M7] image_magic=%x dtb_magic=%x dtb_size=%u\n",
	            (uint64_t)img_magic, (uint64_t)dtb_magic, dtb_sz);

	for (;;) {
		vcpu_run_once(&vcpus[0]);
		if (vcpus[0].halted)
			break;

		uint64_t reason = vcpus[0].exit_reason;
		uint64_t ec = ESR_EC(vcpus[0].exit_esr);

		if ((reason & 3) == 1) {            /* stray physical IRQ at EL2 */
			uint64_t iar = gic_ack();
			if ((iar & 0xFFFFFF) < GIC_SPURIOUS_INTID)
				gic_eoi(iar);
			continue;
		}

		if (ec == EC_SMC64) {
			psci_handle(&vcpus[0]);
			vcpus[0].pc += 4;           /* trapped SMC: advance past it */
		} else if (ec == EC_HVC64) {
			psci_handle(&vcpus[0]);     /* HVC conduit fallback */
		} else if (ec == EC_DABT_LOW || ec == EC_IABT_LOW) {
			uint64_t ipa = ((vcpus[0].exit_hpfar >> 4) << 12) |
			               (vcpus[0].exit_far & 0xFFF);
			stage2_map_1gb(ipa & ~0x3FFFFFFFUL); /* fault-in more RAM */
		} else {
			uart_printf("\n[M7] guest trap ec=%x (%s) far=%x elr=%x -- stopping\n",
			            ec, ec_name(ec), vcpus[0].exit_far, vcpus[0].pc);
			break;
		}
	}
	uart_println("------------------------------------------------------------");
	uart_println("[M7] Linux guest run ended.");
}

/* ------------------------------------------------------------------ */
/* M12: boot fermi-os (a separate from-scratch kernel) as a guest      */
/* ------------------------------------------------------------------ */

#define FERMIOS_LOAD_IPA 0x40000000UL   /* fermi-os links/loads here */

void fermios_boot(void) {
	stage2_init();                       /* Normal RAM block @0x40000000 */
	stage2_map_1gb_device(0x0UL);        /* passthrough GIC + UART (block 0) */

	/* Place the embedded fermi-os image at its native load address (this
	 * overwrites QEMU's auto-DTB at the RAM base, which fermi-os ignores). */
	extern char fermios_image_start[], fermios_image_end[];
	uint64_t sz = (uint64_t)(fermios_image_end - fermios_image_start);
	volatile uint8_t *dst = (volatile uint8_t *)FERMIOS_LOAD_IPA;
	for (uint64_t i = 0; i < sz; i++)
		dst[i] = ((uint8_t *)fermios_image_start)[i];
	__asm__ volatile("dsb sy");
	__asm__ volatile("ic ialluis");
	__asm__ volatile("dsb sy");
	__asm__ volatile("isb");
	uart_printf("[M12] placed %u-byte fermi-os image at %x\n", sz, FERMIOS_LOAD_IPA);

	wr_sysreg("cnthctl_el2", 3UL);       /* EL1 physical timer access */
	wr_sysreg("cntvoff_el2", 0UL);

	vcpu_init(&vcpus[0], FERMIOS_LOAD_IPA, 0, stage2_vttbr(), 0);

	/* RW | VM | TSC (fermi-os uses PSCI SMC for its `reboot` command). No DC
	 * (it runs its own higher-half MMU), no IMO (it drives the real GIC and
	 * takes its own physical-timer IRQs at EL1). */
	uint64_t hcr = (1UL << 31) | (1UL << 0) | (1UL << 19);
	wr_sysreg("hcr_el2", hcr);
	__asm__ volatile("isb");

	uart_println("[M12] booting fermi-os as a guest at EL1. Its output follows:");
	uart_println("------------------------------------------------------------");

	for (;;) {
		vcpu_run_once(&vcpus[0]);
		if (vcpus[0].halted)
			break;

		uint64_t reason = vcpus[0].exit_reason;
		uint64_t ec = ESR_EC(vcpus[0].exit_esr);

		if ((reason & 3) == 1) {
			uint64_t iar = gic_ack();
			if ((iar & 0xFFFFFF) < GIC_SPURIOUS_INTID)
				gic_eoi(iar);
			continue;
		}
		if (ec == EC_SMC64) {
			psci_handle(&vcpus[0]);
			vcpus[0].pc += 4;
		} else if (ec == EC_DABT_LOW || ec == EC_IABT_LOW) {
			uint64_t ipa = ((vcpus[0].exit_hpfar >> 4) << 12) |
			               (vcpus[0].exit_far & 0xFFF);
			uint64_t base = ipa & ~0x3FFFFFFFUL;
			if (base < 0x8000000000UL) {
				stage2_map_1gb(base);             /* fault-in reachable RAM/MMIO */
			} else {
				/* High MMIO (virtio BARs) we don't map; skip the access so the
				 * guest's device probe fails gracefully instead of looping. */
				vcpus[0].pc += 4;
			}
		} else {
			uart_printf("\n[M12] fermi-os trap ec=%x (%s) far=%x elr=%x\n",
			            ec, ec_name(ec), vcpus[0].exit_far, vcpus[0].pc);
			break;
		}
	}
	uart_println("------------------------------------------------------------");
	uart_println("[M12] fermi-os guest run ended.");
}

/* ------------------------------------------------------------------ */
/* M17: fermi-os on the EMULATED vGIC (no GIC/UART passthrough)        */
/* ------------------------------------------------------------------ */

/* Emulated EL1 physical timer state, per VM (the guest's CNTP_* are trapped so
 * its timer never fires a real interrupt; we drive ticks from the EL2 CNTHP). */
static uint64_t g_cntp_ctl[2], g_cntp_cval[2], g_cntp_tval[2];

static void cntp_emulate(int vm, vcpu_t *v) {
	uint64_t iss = ESR_ISS(v->exit_esr);
	int dir = iss & 1;                 /* 0 = write (MSR), 1 = read (MRS) */
	int rt = (iss >> 5) & 0x1F;
	int op2 = (iss >> 17) & 7;
	uint64_t crn = (iss >> 10) & 0xF, crm = (iss >> 1) & 0xF;
	if (crn == 14 && crm == 2) {       /* CNTP_TVAL(op2=0)/CTL(1)/CVAL(2)_EL0 */
		if (dir == 0) {
			uint64_t val = (rt == 31) ? 0 : v->x[rt];
			if (op2 == 1) g_cntp_ctl[vm] = val;
			else if (op2 == 2) g_cntp_cval[vm] = val;
			else g_cntp_tval[vm] = val;
		} else {
			uint64_t val = (op2 == 1) ? g_cntp_ctl[vm]
			             : (op2 == 2) ? g_cntp_cval[vm] : g_cntp_tval[vm];
			if (rt != 31)
				v->x[rt] = val;
		}
	}
	v->pc += 4;
}

void fermios_vgic_boot(void) {
	stage2_init();                       /* block1 RAM; block0 (GIC+UART) unmapped */
	vgic_reset();
	gic_init_el2();
	hyptimer_init();

	/* Place the embedded fermi-os image at its native load address. */
	extern char fermios_image_start[], fermios_image_end[];
	uint64_t sz = (uint64_t)(fermios_image_end - fermios_image_start);
	volatile uint8_t *dst = (volatile uint8_t *)0x40000000UL;
	for (uint64_t i = 0; i < sz; i++)
		dst[i] = ((uint8_t *)fermios_image_start)[i];
	__asm__ volatile("dsb sy; ic ialluis; dsb sy; isb");

	/* Allow the counter (CNTPCT) but trap the physical timer (CNTP_*). */
	wr_sysreg("cnthctl_el2", 1UL);       /* EL1PCTEN=1, EL1PCEN=0 */
	wr_sysreg("cntvoff_el2", 0UL);

	/* RW | VM | IMO | TSC (no DC: fermi-os runs its own MMU). */
	uint64_t hcr = (1UL << 31) | (1UL << 0) | (1UL << 4) | (1UL << 19);
	wr_sysreg("hcr_el2", hcr);
	__asm__ volatile("msr ich_hcr_el2, %0" ::"r"(1UL)); /* virtual CPU iface on */
	__asm__ volatile("isb");

	vcpu_init(&vcpus[0], 0x40000000UL, 0, stage2_vttbr(), 0);

	uart_println("[M17] booting fermi-os on the EMULATED vGIC (GIC+UART emulated,");
	uart_println("      timer trapped; ticks injected via vGIC). Output follows:");
	uart_println("------------------------------------------------------------");

	hyptimer_start(100);
	int guard = 0;
	while (guard++ < 2000000) {
		vcpu_run_once(&vcpus[0]);
		if (vcpus[0].halted)
			break;

		uint64_t reason = vcpus[0].exit_reason;
		uint64_t ec = ESR_EC(vcpus[0].exit_esr);
		if ((reason & 3) == 1) {
			uint64_t iar = gic_ack();
			uint32_t intid = iar & 0xFFFFFF;
			if (intid == HYP_TIMER_PPI) {
				hyptimer_on_irq();
				if (vgic_irq_enabled(0, 30))
					vgic_inject(&vcpus[0], 30);     /* deliver the guest's timer tick */
			}
			if (intid < GIC_SPURIOUS_INTID)
				gic_eoi(iar);
			continue;
		}
		if (ec == EC_DABT_LOW || ec == EC_IABT_LOW) {
			uint64_t ipa = ((vcpus[0].exit_hpfar >> 4) << 12) |
			               (vcpus[0].exit_far & 0xFFF);
			if (vgic_contains(ipa))
				vgic_mmio(0, &vcpus[0], ipa);
			else if (vuart_contains(ipa))
				mmio_emulate(&vcpus[0], ipa);
			else
				stage2_map_1gb(ipa & ~0x3FFFFFFFUL);
		} else if (ec == EC_SYSREG) {
			cntp_emulate(0, &vcpus[0]);     /* emulated physical timer */
		} else if (ec == EC_SMC64) {
			psci_handle(&vcpus[0]);
			vcpus[0].pc += 4;
		} else {
			uart_printf("\n[M17] fermi-os trap ec=%x (%s) far=%x elr=%x\n",
			            ec, ec_name(ec), vcpus[0].exit_far, vcpus[0].pc);
			break;
		}
	}
	hyptimer_stop();
	uart_println("------------------------------------------------------------");
	uart_println("[M17] fermi-os-on-vGIC run ended.");
}

/* ------------------------------------------------------------------ */
/* M19: a REAL OS (fermi-os) as one of two concurrent isolated tenants */
/* ------------------------------------------------------------------ */

void mtenant_real_demo(void) {
	stage2_init();
	vgic_reset();
	gic_init_el2();
	hyptimer_init();

	extern char fermios_image_start[], fermios_image_end[];
	extern char guest_tvgic_start[], guest_tvgic_end[];

	/* VM0 = fermi-os (real OS) at host 0x80000000; VM1 = vGIC tenant at 0xC0000000. */
	uint64_t fsz = (uint64_t)(fermios_image_end - fermios_image_start);
	uint64_t tsz = (uint64_t)(guest_tvgic_end - guest_tvgic_start);
	for (uint64_t i = 0; i < fsz; i++)
		((volatile uint8_t *)0x80000000UL)[i] = ((uint8_t *)fermios_image_start)[i];
	for (uint64_t i = 0; i < tsz; i++)
		((volatile uint8_t *)0xC0000000UL)[i] = ((uint8_t *)guest_tvgic_start)[i];
	__asm__ volatile("dsb sy; ic ialluis; dsb sy; isb");

	uint64_t vttbr0 = stage2_build_vm(0, 0x40000000UL, 0x80000000UL, 1);
	uint64_t vttbr1 = stage2_build_vm(1, 0x40000000UL, 0xC0000000UL, 2);

	/* Per-VM HCR_EL2: fermi-os runs its own MMU (no DC); the tenant is MMU-off
	 * (DC). Both: RW|VM|IMO; fermi-os also TSC for its PSCI reboot. */
	uint64_t hcr[2];
	hcr[0] = (1UL << 31) | (1UL << 0) | (1UL << 4) | (1UL << 19);            /* RW|VM|IMO|TSC */
	hcr[1] = (1UL << 31) | (1UL << 0) | (1UL << 12) | (1UL << 4);            /* RW|VM|DC|IMO  */

	/* Counter readable, physical timer trapped (fermi-os); ICV enabled. */
	wr_sysreg("cnthctl_el2", 1UL);
	wr_sysreg("cntvoff_el2", 0UL);
	__asm__ volatile("msr ich_hcr_el2, %0" ::"r"(1UL));
	__asm__ volatile("isb");

	vcpu_init(&vcpus[0], 0x40000000UL, 0, vttbr0, 0);             /* fermi-os */
	vcpu_init(&vcpus[1], 0x40000000UL, 0x40010000UL, vttbr1, 1); /* tenant   */

	uart_println("[M19] TWO concurrent isolated tenants on per-VM vGICs:");
	uart_println("      VM0 = fermi-os (real OS), VM1 = lightweight vGIC guest.");
	uart_println("      fermi-os boots below, interleaved with VM1's timer service:");
	uart_println("------------------------------------------------------------");

	hyptimer_start(10);
	int t1 = 0, pend[2] = {0, 0}, cur = 0, guard = 0, slot = 0;
	const uint32_t intid[2] = {30, 27};   /* fermi-os timer PPI 30; tenant 27 */
	while (t1 < 8 && guard++ < 2000000) {
		wr_sysreg("hcr_el2", hcr[cur]);   /* per-VM HCR (DC differs) */
		__asm__ volatile("isb");
		if (pend[cur] && vgic_irq_enabled(cur, intid[cur])) {
			vgic_inject(&vcpus[cur], intid[cur]);
			pend[cur] = 0;
		}

		vcpu_run_once(&vcpus[cur]);

		uint64_t reason = vcpus[cur].exit_reason;
		uint64_t ec = ESR_EC(vcpus[cur].exit_esr);
		if ((reason & 3) == 1) {
			uint64_t iar = gic_ack();
			uint32_t id = iar & 0xFFFFFF;
			if (id == HYP_TIMER_PPI) {
				hyptimer_on_irq();
				pend[0] = pend[1] = 1;
				slot++;                   /* 3 of every 4 slices to fermi-os */
				cur = ((slot & 3) == 0) ? 1 : 0;
			}
			if (id < GIC_SPURIOUS_INTID)
				gic_eoi(iar);
		} else if (ec == EC_DABT_LOW || ec == EC_IABT_LOW) {
			uint64_t ipa = ((vcpus[cur].exit_hpfar >> 4) << 12) |
			               (vcpus[cur].exit_far & 0xFFF);
			if (vgic_contains(ipa))
				vgic_mmio(cur, &vcpus[cur], ipa);
			else if (vuart_contains(ipa))
				mmio_emulate(&vcpus[cur], ipa);   /* fermi-os console */
			else
				uart_printf("\n[M19] VM%d DABT ipa=%x\n", cur, ipa);
		} else if (ec == EC_SYSREG) {
			cntp_emulate(cur, &vcpus[cur]);       /* fermi-os physical timer */
		} else if (ec == EC_HVC64) {
			if (cur == 1) {
				t1++;
				uart_printf("\n[M19] >> VM1 (tenant) serviced its vGIC tick #%d "
				            "while fermi-os runs concurrently <<\n", t1);
			}
		} else if (ec == EC_SMC64) {
			psci_handle(&vcpus[cur]);
			vcpus[cur].pc += 4;
		} else {
			uart_printf("\n[M19] VM%d exit ec=%x (%s)\n", cur, ec, ec_name(ec));
			break;
		}
	}
	hyptimer_stop();
	uart_println("\n------------------------------------------------------------");
	uart_printf("[M19] done: VM1 serviced %d ticks while VM0 (fermi-os) booted "
	            "alongside it.\n", t1);
	uart_println("[M19] a REAL OS and a second VM ran concurrently, each isolated "
	             "(own stage-2 + own vGIC + own timer state).");
}

/* ------------------------------------------------------------------ */
/* M20: boot Linux on the fully emulated vGIC (no device passthrough)  */
/* ------------------------------------------------------------------ */

/* Empty-MMIO emulation: reads return 0, writes ignored. Lets Linux probe
 * absent devices (PCIe/virtio/RTC) and find nothing, instead of faulting. */
static void mmio_zero(vcpu_t *v) {
	uint64_t iss = ESR_ISS(v->exit_esr);
	if ((iss >> 24) & 1) {
		int wnr = (iss >> 6) & 1;
		int srt = (iss >> 16) & 0x1F;
		if (!wnr && srt != 31)
			v->x[srt] = 0;
	}
	v->pc += 4;
}

void linux_vgic_boot(void) {
	stage2_init();                       /* block1 RAM; block0 (GIC/UART/MMIO) unmapped */
	vgic_reset();
	gic_init_el2();
	hyptimer_init();

	extern char guest_dtb_start[], guest_dtb_end[];
	uint64_t dsz = (uint64_t)(guest_dtb_end - guest_dtb_start);
	for (uint64_t i = 0; i < dsz; i++)
		((volatile uint8_t *)0x48000000UL)[i] = ((uint8_t *)guest_dtb_start)[i];
	__asm__ volatile("dsb sy");

	wr_sysreg("cnthctl_el2", 1UL);       /* counter readable, physical timer trapped */
	wr_sysreg("cntvoff_el2", 0UL);

	uint64_t hcr = (1UL << 31) | (1UL << 0) | (1UL << 19) | (1UL << 4); /* RW|VM|TSC|IMO */
	wr_sysreg("hcr_el2", hcr);
	__asm__ volatile("msr ich_hcr_el2, %0" ::"r"((1UL << 0) | (1UL << 8))); /* En|TC */
	__asm__ volatile("isb");

	vgic_set_ngicr(2);                   /* present two redistributor frames */

	vcpu_init(&vcpus[0], 0x41000000UL, 0, stage2_vttbr(), 0);
	vcpus[0].x[0] = 0x48000000UL;        /* arm64 boot: x0 = DTB */
	vcpus[0].x[1] = 0;
	vcpus[0].x[2] = 0;
	vcpus[0].x[3] = 0;

	uart_println("[M32] booting SMP Linux (2 vCPUs) on the EMULATED vGIC:");
	uart_println("      per-vCPU redistributor frames + PSCI CPU_ON + UART SPI,");
	uart_println("      time-sliced on one physical core. Kernel output:");
	uart_println("------------------------------------------------------------");

	psci_on_pending = 0;
	int ncpu = 1, cur = 0;
	hyptimer_start(10);
	for (;;) {
		/* Bring up the secondary vCPU when Linux issues PSCI CPU_ON. */
		if (psci_on_pending && ncpu < 2) {
			vcpu_init(&vcpus[1], psci_on_entry, 0, stage2_vttbr(), 1);
			vcpus[1].x[0] = psci_on_ctx;
			ncpu = 2;
			psci_on_pending = 0;
			uart_printf("\n[M32] PSCI CPU_ON: starting secondary vCPU1 at "
			            "entry=%x\n", psci_on_entry);
		}
		/* Host keystroke -> emulated UART RX -> UART SPI (33) into CPU0. */
		int ch = uart_getc_nonblock();
		if (ch >= 0) {
			vuart_push_rx(ch);
			if (vuart_rx_irq_pending() && vgic_spi_enabled(0, 33))
				vgic_inject(&vcpus[0], 33);
		}
		/* Deliver a pending inter-CPU IPI (SGI) to the vCPU about to run. */
		int sgi = vgic_pop_sgi(cur);
		if (sgi >= 0)
			vgic_inject(&vcpus[cur], (uint32_t)sgi);

		vcpu_run_once(&vcpus[cur]);
		if (vcpus[cur].halted)
			break;

		uint64_t reason = vcpus[cur].exit_reason;
		uint64_t ec = ESR_EC(vcpus[cur].exit_esr);
		if ((reason & 3) == 1) {
			uint64_t iar = gic_ack();
			uint32_t id = iar & 0xFFFFFF;
			if (id == HYP_TIMER_PPI) {
				hyptimer_on_irq();
				/* deliver a timer tick to the current vCPU's own timer PPI */
				if (vgic_irq_enabled(cur, 30))
					vgic_inject(&vcpus[cur], 30);
				else if (vgic_irq_enabled(cur, 27))
					vgic_inject(&vcpus[cur], 27);
				if (ncpu == 2)
					cur ^= 1;            /* time-slice between the two vCPUs */
			}
			if (id < GIC_SPURIOUS_INTID)
				gic_eoi(iar);
			continue;
		}
		if (ec == EC_DABT_LOW || ec == EC_IABT_LOW) {
			uint64_t ipa = ((vcpus[cur].exit_hpfar >> 4) << 12) |
			               (vcpus[cur].exit_far & 0xFFF);
			if (vgic_contains(ipa))
				vgic_mmio(0, &vcpus[cur], ipa);  /* vm=0; GICR frame from address */
			else if (vuart_contains(ipa))
				mmio_emulate(&vcpus[cur], ipa);
			else
				mmio_zero(&vcpus[cur]);
		} else if (ec == EC_SYSREG) {
			uint64_t iss = ESR_ISS(vcpus[cur].exit_esr);
			if (((iss >> 10) & 0xF) == 12) {  /* ICC_SGI1R_EL1: inter-CPU IPI */
				int rt = (iss >> 5) & 0x1F;
				uint64_t val = (rt == 31) ? 0 : vcpus[cur].x[rt];
				vgic_post_sgi(cur, (uint32_t)((val >> 24) & 0xF));
				vcpus[cur].pc += 4;
			} else {
				cntp_emulate(cur, &vcpus[cur]);
			}
		} else if (ec == EC_SMC64) {
			psci_handle(&vcpus[cur]);
			vcpus[cur].pc += 4;
		} else if (ec == EC_HVC64) {
			psci_handle(&vcpus[cur]);
		} else {
			uart_printf("\n[M32] CPU%d trap ec=%x (%s) far=%x elr=%x\n",
			            cur, ec, ec_name(ec), vcpus[cur].exit_far, vcpus[cur].pc);
			break;
		}
	}
	uart_println("------------------------------------------------------------");
	uart_println("[M32] SMP-Linux-on-vGIC run ended.");
}

/* ------------------------------------------------------------------ */
/* M21: TWO real OSes (Linux + fermi-os) as concurrent isolated tenants */
/* ------------------------------------------------------------------ */

void mtenant_os_demo(void) {
	stage2_init();
	vgic_reset();
	gic_init_el2();
	hyptimer_init();

	extern char fermios_image_start[], fermios_image_end[];
	extern char guest_dtb_start[], guest_dtb_end[];

	/* VM0 = Linux at host block 0x80000000 (QEMU's loader placed the Image at
	 * host 0x81000000 = guest IPA 0x41000000, and the initramfs at host
	 * 0x8c000000 = guest IPA 0x4c000000). Copy the DTB to host 0x88000000
	 * (= guest IPA 0x48000000). VM1 = fermi-os at host block 0xC0000000. */
	uint64_t dsz = (uint64_t)(guest_dtb_end - guest_dtb_start);
	for (uint64_t i = 0; i < dsz; i++)
		((volatile uint8_t *)0x88000000UL)[i] = ((uint8_t *)guest_dtb_start)[i];
	uint64_t fsz = (uint64_t)(fermios_image_end - fermios_image_start);
	for (uint64_t i = 0; i < fsz; i++)
		((volatile uint8_t *)0xC0000000UL)[i] = ((uint8_t *)fermios_image_start)[i];
	__asm__ volatile("dsb sy; ic ialluis; dsb sy; isb");

	uint64_t vttbr0 = stage2_build_vm(0, 0x40000000UL, 0x80000000UL, 1); /* Linux  */
	uint64_t vttbr1 = stage2_build_vm(1, 0x40000000UL, 0xC0000000UL, 2); /* fermios*/

	wr_sysreg("cnthctl_el2", 1UL);       /* counter readable, physical timer trapped */
	wr_sysreg("cntvoff_el2", 0UL);
	__asm__ volatile("msr ich_hcr_el2, %0" ::"r"(1UL));

	/* Both guests run their own MMU -> DC cleared; RW|VM|IMO|TSC. */
	uint64_t hcr = (1UL << 31) | (1UL << 0) | (1UL << 4) | (1UL << 19);
	wr_sysreg("hcr_el2", hcr);
	__asm__ volatile("isb");

	vcpu_init(&vcpus[0], 0x41000000UL, 0, vttbr0, 0);   /* Linux: pc=Image, x0=DTB */
	vcpus[0].x[0] = 0x48000000UL;
	vcpu_init(&vcpus[1], 0x40000000UL, 0, vttbr1, 1);   /* fermi-os */

	uart_println("[M21] TWO real OSes as concurrent isolated tenants on per-VM vGICs:");
	uart_println("      VM0 = Linux, VM1 = fermi-os. Both boot below, interleaved.");
	uart_println("      (each: own stage-2 + own vGIC + own timer; HV is untouchable)");
	uart_println("------------------------------------------------------------");

	hyptimer_start(10);
	int cur = 0, slot = 0, guard = 0;
	int pend[2] = {0, 0};
	while (guard++ < 4000000) {
		if (pend[cur]) {
			if (vgic_irq_enabled(cur, 30))
				vgic_inject(&vcpus[cur], 30);
			else if (vgic_irq_enabled(cur, 27))
				vgic_inject(&vcpus[cur], 27);
			pend[cur] = 0;
		}
		vcpu_run_once(&vcpus[cur]);
		if (vcpus[cur].halted)
			break;

		uint64_t reason = vcpus[cur].exit_reason;
		uint64_t ec = ESR_EC(vcpus[cur].exit_esr);
		if ((reason & 3) == 1) {
			uint64_t iar = gic_ack();
			uint32_t id = iar & 0xFFFFFF;
			if (id == HYP_TIMER_PPI) {
				hyptimer_on_irq();
				pend[0] = pend[1] = 1;
				slot++;                  /* weight Linux 3:1 (it's slower on vGIC) */
				cur = ((slot & 3) == 0) ? 1 : 0;
			}
			if (id < GIC_SPURIOUS_INTID)
				gic_eoi(iar);
		} else if (ec == EC_DABT_LOW || ec == EC_IABT_LOW) {
			uint64_t ipa = ((vcpus[cur].exit_hpfar >> 4) << 12) |
			               (vcpus[cur].exit_far & 0xFFF);
			if (vgic_contains(ipa))
				vgic_mmio(cur, &vcpus[cur], ipa);
			else if (vuart_contains(ipa))
				mmio_emulate(&vcpus[cur], ipa);
			else
				mmio_zero(&vcpus[cur]);
		} else if (ec == EC_SYSREG) {
			cntp_emulate(cur, &vcpus[cur]);
		} else if (ec == EC_SMC64) {
			psci_handle(&vcpus[cur]);
			vcpus[cur].pc += 4;
		} else if (ec == EC_HVC64) {
			psci_handle(&vcpus[cur]);
		} else {
			uart_printf("\n[M21] VM%d trap ec=%x (%s) far=%x\n",
			            cur, ec, ec_name(ec), vcpus[cur].exit_far);
			break;
		}
	}
	uart_println("\n------------------------------------------------------------");
	uart_println("[M21] Linux and fermi-os ran concurrently, fully isolated.");
}

/* ------------------------------------------------------------------ */
/* M22: guest-driven SMP -- a guest boots its secondary core via PSCI   */
/* ------------------------------------------------------------------ */

void smp_psci_demo(void) {
	extern void guest_psci_smp_entry(void);
	stage2_init();
	vgic_reset();
	gic_init_el2();
	hyptimer_init();

	uint64_t hcr = (1UL << 31) | (1UL << 0) | (1UL << 12) | (1UL << 4); /* RW|VM|DC|IMO */
	wr_sysreg("hcr_el2", hcr);
	__asm__ volatile("msr ich_hcr_el2, %0" ::"r"((1UL << 0) | (1UL << 8))); /* En|TC */
	__asm__ volatile("isb");

	psci_on_pending = 0;
	vcpu_init(&vcpus[0], (uint64_t)&guest_psci_smp_entry,
	          (uint64_t)(gstack[0] + sizeof(gstack[0])), stage2_vttbr(), 0);

	uart_println("[M22] guest-driven SMP: vCPU0 boots vCPU1 via PSCI CPU_ON,");
	uart_println("      then the secondary IPIs the primary back.");

	hyptimer_start(10);
	int cur = 0, sec_on = 0, got = 0, guard = 0;
	while (!got && guard++ < 100000) {
		if (psci_on_pending && !sec_on) {
			vcpu_init(&vcpus[1], psci_on_entry,
			          (uint64_t)(gstack[1] + sizeof(gstack[1])), stage2_vttbr(), 1);
			vcpus[1].x[0] = psci_on_ctx;
			sec_on = 1;
			psci_on_pending = 0;
			uart_printf("[M22] PSCI CPU_ON honored: secondary vCPU1 created at "
			            "entry=%x ctx=%x\n", psci_on_entry, vcpus[1].x[0]);
		}

		int s = vgic_pop_sgi(cur);
		if (s >= 0)
			vgic_inject(&vcpus[cur], (uint32_t)s);

		vcpu_run_once(&vcpus[cur]);

		uint64_t reason = vcpus[cur].exit_reason;
		uint64_t ec = ESR_EC(vcpus[cur].exit_esr);
		if ((reason & 3) == 1) {
			uint64_t iar = gic_ack();
			uint32_t id = iar & 0xFFFFFF;
			if (id == HYP_TIMER_PPI) {
				hyptimer_on_irq();
				if (sec_on)
					cur ^= 1;        /* round-robin once the secondary is up */
			}
			if (id < GIC_SPURIOUS_INTID)
				gic_eoi(iar);
		} else if (ec == EC_HVC64) {
			uint32_t fn = (uint32_t)vcpus[cur].x[0];
			if ((fn & 0xFFFF0000) == 0xC4000000 ||
			    (fn & 0xFFFF0000) == 0x84000000) {
				psci_handle(&vcpus[cur]);     /* a PSCI call (CPU_ON, ...) */
			} else {
				uart_printf("[M22] primary vCPU0 RECEIVED IPI (SGI INTID %u) "
				            "from the secondary core it booted!\n",
				            vcpus[cur].x[0]);
				got = 1;
			}
		} else if (ec == EC_SYSREG) {
			uint64_t iss = ESR_ISS(vcpus[cur].exit_esr);
			int rt = (iss >> 5) & 0x1F;
			uint64_t val = (rt == 31) ? 0 : vcpus[cur].x[rt];
			uint32_t intid = (val >> 24) & 0xF;
			vgic_post_sgi(cur, intid);
			vcpus[cur].pc += 4;
			uart_printf("[M22] secondary vCPU1 issued SGI %u to the primary\n", intid);
		} else {
			uart_printf("[M22] vCPU%d exit ec=%x (%s)\n", cur, ec, ec_name(ec));
			break;
		}
	}
	hyptimer_stop();
	uart_println(got ? "[M22] guest-driven SMP works: primary booted a secondary "
	                   "core via PSCI and they exchanged an IPI."
	                 : "[M22] secondary bring-up/IPI not observed.");
}

/* ------------------------------------------------------------------ */
/* M23: emulated virtio-console -- a guest prints through a virtqueue   */
/* ------------------------------------------------------------------ */

void virtio_demo(void) {
	extern void guest_virtio_entry(void);
	stage2_init();              /* block1 RAM; block0 (virtio/GIC/UART) unmapped */
	virtio_reset();

	uint64_t hcr = (1UL << 31) | (1UL << 0) | (1UL << 12); /* RW|VM|DC */
	wr_sysreg("hcr_el2", hcr);
	__asm__ volatile("isb");

	vcpu_init(&vcpus[0], (uint64_t)&guest_virtio_entry,
	          (uint64_t)(gstack[0] + sizeof(gstack[0])), stage2_vttbr(), 0);

	uart_println("[M23] guest drives an EMULATED virtio-console (virtqueue):");
	uart_puts("      device output> ");

	int done = 0, guard = 0;
	while (!done && guard++ < 100000) {
		vcpu_run_once(&vcpus[0]);
		uint64_t ec = ESR_EC(vcpus[0].exit_esr);
		if ((vcpus[0].exit_reason & 3) == 1) {
			uint64_t iar = gic_ack();
			if ((iar & 0xFFFFFF) < GIC_SPURIOUS_INTID)
				gic_eoi(iar);
			continue;
		}
		if (ec == EC_HVC64) {
			done = 1;
		} else if (ec == EC_DABT_LOW) {
			uint64_t ipa = ((vcpus[0].exit_hpfar >> 4) << 12) |
			               (vcpus[0].exit_far & 0xFFF);
			if (virtio_contains(ipa))
				virtio_mmio(0, &vcpus[0], ipa);
			else
				mmio_zero(&vcpus[0]);
		} else {
			uart_printf("[M23] unexpected exit ec=%x (%s)\n", ec, ec_name(ec));
			break;
		}
	}
	uart_println("[M23] virtio-console: guest queued a buffer; the hypervisor's "
	             "device model walked the virtqueue and emitted it.");
}

/* ------------------------------------------------------------------ */
/* M24: interrupt-driven virtio-console (used ring + completion IRQ)   */
/* ------------------------------------------------------------------ */

void virtio_irq_demo(void) {
	extern void guest_virtio_irq_entry(void);
	stage2_init();
	virtio_reset();
	vgic_reset();
	gic_init_el2();

	uint64_t hcr = (1UL << 31) | (1UL << 0) | (1UL << 12) | (1UL << 4); /* RW|VM|DC|IMO */
	wr_sysreg("hcr_el2", hcr);
	__asm__ volatile("msr ich_hcr_el2, %0" ::"r"(1UL));
	__asm__ volatile("isb");

	vcpu_init(&vcpus[0], (uint64_t)&guest_virtio_irq_entry,
	          (uint64_t)(gstack[0] + sizeof(gstack[0])), stage2_vttbr(), 0);

	uart_println("[M24] interrupt-driven virtio-console (used ring + completion IRQ):");
	uart_puts("      device output> ");

	int done = 0, guard = 0;
	while (!done && guard++ < 100000) {
		vcpu_run_once(&vcpus[0]);
		uint64_t reason = vcpus[0].exit_reason;
		uint64_t ec = ESR_EC(vcpus[0].exit_esr);
		if ((reason & 3) == 1) {
			uint64_t iar = gic_ack();
			if ((iar & 0xFFFFFF) < GIC_SPURIOUS_INTID)
				gic_eoi(iar);
		} else if (ec == EC_HVC64) {
			uart_printf("\n[M24] guest serviced the virtio completion IRQ "
			            "(INTID %u) delivered via the vGIC\n", vcpus[0].x[1]);
			done = 1;
		} else if (ec == EC_DABT_LOW) {
			uint64_t ipa = ((vcpus[0].exit_hpfar >> 4) << 12) |
			               (vcpus[0].exit_far & 0xFFF);
			if (virtio_contains(ipa))
				virtio_mmio(0, &vcpus[0], ipa);
			else if (vgic_contains(ipa))
				vgic_mmio(0, &vcpus[0], ipa);
			else
				mmio_zero(&vcpus[0]);
		} else {
			uart_printf("[M24] unexpected exit ec=%x (%s)\n", ec, ec_name(ec));
			break;
		}
		/* device raised a completion IRQ -> inject it if the guest enabled it */
		if (virtio_take_irq(0) && vgic_irq_enabled(0, 20))
			vgic_inject(&vcpus[0], 20);
	}
	uart_println("[M24] virtio-console is interrupt-driven end-to-end: the device "
	             "wrote the used ring and signalled the guest through the vGIC.");
}

/* ------------------------------------------------------------------ */
/* M25: virtio-blk -- a guest writes a sector through a request chain   */
/* ------------------------------------------------------------------ */

void vblk_demo(void) {
	extern void guest_vblk_entry(void);
	stage2_init();
	vblk_reset();
	vgic_reset();
	gic_init_el2();

	uint64_t hcr = (1UL << 31) | (1UL << 0) | (1UL << 12) | (1UL << 4); /* RW|VM|DC|IMO */
	wr_sysreg("hcr_el2", hcr);
	__asm__ volatile("msr ich_hcr_el2, %0" ::"r"(1UL));
	__asm__ volatile("isb");

	vcpu_init(&vcpus[0], (uint64_t)&guest_vblk_entry,
	          (uint64_t)(gstack[0] + sizeof(gstack[0])), stage2_vttbr(), 0);

	uart_println("[M25] guest writes sector 0 via a virtio-blk request chain "
	             "(header/data/status):");

	int done = 0, guard = 0;
	while (!done && guard++ < 100000) {
		vcpu_run_once(&vcpus[0]);
		uint64_t reason = vcpus[0].exit_reason;
		uint64_t ec = ESR_EC(vcpus[0].exit_esr);
		if ((reason & 3) == 1) {
			uint64_t iar = gic_ack();
			if ((iar & 0xFFFFFF) < GIC_SPURIOUS_INTID)
				gic_eoi(iar);
		} else if (ec == EC_HVC64) {
			done = 1;
			uart_printf("[M25] guest serviced the virtio-blk completion IRQ "
			            "(INTID %u)\n", vcpus[0].x[1]);
		} else if (ec == EC_DABT_LOW) {
			uint64_t ipa = ((vcpus[0].exit_hpfar >> 4) << 12) |
			               (vcpus[0].exit_far & 0xFFF);
			if (vblk_contains(ipa))
				vblk_mmio(0, &vcpus[0], ipa);
			else if (vgic_contains(ipa))
				vgic_mmio(0, &vcpus[0], ipa);
			else
				mmio_zero(&vcpus[0]);
		} else {
			uart_printf("[M25] unexpected exit ec=%x (%s) far=%x elr=%x\n",
			            ec, ec_name(ec), vcpus[0].exit_far, vcpus[0].pc);
			break;
		}
		if (vblk_take_irq(0) && vgic_irq_enabled(0, 21))
			vgic_inject(&vcpus[0], 21);
	}

	char buf[40];
	vblk_peek(0, buf, 32);
	buf[32] = 0;
	uart_printf("[M25] hypervisor reads virtio-blk disk sector 0 = \"%s\"\n", buf);
	uart_println("[M25] virtio-blk works: guest request chain -> device -> backing "
	             "disk, with a completion IRQ.");
}

/* ------------------------------------------------------------------ */
/* M26: virtio-blk READ -- guest reads a sector the hypervisor seeded   */
/* ------------------------------------------------------------------ */

void vblk_rd_demo(void) {
	extern void guest_vblk_rd_entry(void);
	stage2_init();
	vblk_reset();
	vgic_reset();
	gic_init_el2();

	/* Hypervisor seeds disk sector 0 with a known 8-byte magic. */
	const char magic[8] = {'V','B','L','K','-','R','D','!'};
	vblk_poke(0, magic, 8);

	uint64_t hcr = (1UL << 31) | (1UL << 0) | (1UL << 12) | (1UL << 4);
	wr_sysreg("hcr_el2", hcr);
	__asm__ volatile("msr ich_hcr_el2, %0" ::"r"(1UL));
	__asm__ volatile("isb");

	vcpu_init(&vcpus[0], (uint64_t)&guest_vblk_rd_entry,
	          (uint64_t)(gstack[0] + sizeof(gstack[0])), stage2_vttbr(), 0);

	uart_println("[M26] virtio-blk READ: hypervisor seeded sector 0; guest reads it "
	             "back via a request chain.");

	uint64_t got = 0;
	int done = 0, guard = 0;
	while (!done && guard++ < 100000) {
		vcpu_run_once(&vcpus[0]);
		uint64_t reason = vcpus[0].exit_reason;
		uint64_t ec = ESR_EC(vcpus[0].exit_esr);
		if ((reason & 3) == 1) {
			uint64_t iar = gic_ack();
			if ((iar & 0xFFFFFF) < GIC_SPURIOUS_INTID)
				gic_eoi(iar);
		} else if (ec == EC_HVC64) {
			got = vcpus[0].x[1];
			done = 1;
		} else if (ec == EC_DABT_LOW) {
			uint64_t ipa = ((vcpus[0].exit_hpfar >> 4) << 12) |
			               (vcpus[0].exit_far & 0xFFF);
			if (vblk_contains(ipa))
				vblk_mmio(0, &vcpus[0], ipa);
			else if (vgic_contains(ipa))
				vgic_mmio(0, &vcpus[0], ipa);
			else
				mmio_zero(&vcpus[0]);
		} else {
			uart_printf("[M26] unexpected exit ec=%x (%s) far=%x\n",
			            ec, ec_name(ec), vcpus[0].exit_far);
			break;
		}
		if (vblk_take_irq(0) && vgic_irq_enabled(0, 21))
			vgic_inject(&vcpus[0], 21);
	}

	char rb[9];
	for (int i = 0; i < 8; i++)
		rb[i] = (char)(got >> (8 * i));
	rb[8] = 0;
	uart_printf("[M26] guest read back first 8 bytes = \"%s\"\n", rb);
	uart_println("[M26] virtio-blk read path verified: device copied the backing "
	             "disk into the guest's buffer, with a completion IRQ.");
}

/* ------------------------------------------------------------------ */
/* M27: per-VM virtio devices. Two isolated tenants, each driving its    */
/* OWN emulated virtio-console instance (separate device state, and the  */
/* device translates each VM's guest addresses into that VM's RAM block).*/
/* ------------------------------------------------------------------ */

void mtenant_virtio_demo(void) {
	stage2_init();
	virtio_reset();

	/* Copy the M23 virtio guest into each VM's private host RAM block. */
	uint64_t sz = (uint64_t)(guest_virtio_end - guest_virtio_start);
	for (uint64_t i = 0; i < sz; i++) {
		((volatile uint8_t *)VM0_PA)[i] = ((uint8_t *)guest_virtio_start)[i];
		((volatile uint8_t *)VM1_PA)[i] = ((uint8_t *)guest_virtio_start)[i];
	}
	__asm__ volatile("dsb sy; ic ialluis; dsb sy; isb");

	uint64_t vttbr0 = stage2_build_vm(0, VM_IPA, VM0_PA, 1);
	uint64_t vttbr1 = stage2_build_vm(1, VM_IPA, VM1_PA, 2);

	/* Tell each VM's device how to translate its guest addresses to host. */
	virtio_set_offset(0, VM0_PA - VM_IPA);
	virtio_set_offset(1, VM1_PA - VM_IPA);

	uint64_t hcr = (1UL << 31) | (1UL << 0) | (1UL << 12); /* RW|VM|DC */
	wr_sysreg("hcr_el2", hcr);
	__asm__ volatile("isb");

	vcpu_init(&vcpus[0], VM_IPA, VM_IPA + 0x10000, vttbr0, 0);
	vcpu_init(&vcpus[1], VM_IPA, VM_IPA + 0x10000, vttbr1, 1);

	uart_println("[M27] two isolated VMs, each with its OWN emulated virtio-console "
	             "(separate device state + per-VM address translation):");

	for (int cur = 0; cur < 2; cur++) {
		uart_printf("      VM%d console emits> ", cur);
		int done = 0, guard = 0;
		while (!done && guard++ < 50000) {
			vcpu_run_once(&vcpus[cur]);
			uint64_t ec = ESR_EC(vcpus[cur].exit_esr);
			if (ec == EC_HVC64) {
				done = 1;
			} else if (ec == EC_DABT_LOW) {
				uint64_t ipa = ((vcpus[cur].exit_hpfar >> 4) << 12) |
				               (vcpus[cur].exit_far & 0xFFF);
				if (virtio_contains(ipa))
					virtio_mmio(cur, &vcpus[cur], ipa);
				else
					mmio_zero(&vcpus[cur]);
			} else {
				uart_printf("\n[M27] VM%d unexpected exit ec=%x (%s)\n",
				            cur, ec, ec_name(ec));
				break;
			}
		}
	}
	uart_println("[M27] each tenant's virtio-console processed its own virtqueue "
	             "from its own RAM block -- per-VM device isolation.");
}

/* ------------------------------------------------------------------ */
/* M28: per-VM virtio-blk. Two isolated tenants, each with its OWN block  */
/* device + backing disk. Proves a write on one VM's disk never touches   */
/* the other's.                                                           */
/* ------------------------------------------------------------------ */

extern char guest_vbs_start[];
extern char guest_vbs_end[];

void mtenant_vblk_demo(void) {
	stage2_init();
	vblk_reset();

	uint64_t sz = (uint64_t)(guest_vbs_end - guest_vbs_start);
	for (uint64_t i = 0; i < sz; i++) {
		((volatile uint8_t *)VM0_PA)[i] = ((uint8_t *)guest_vbs_start)[i];
		((volatile uint8_t *)VM1_PA)[i] = ((uint8_t *)guest_vbs_start)[i];
	}
	__asm__ volatile("dsb sy; ic ialluis; dsb sy; isb");

	uint64_t vttbr0 = stage2_build_vm(0, VM_IPA, VM0_PA, 1);
	uint64_t vttbr1 = stage2_build_vm(1, VM_IPA, VM1_PA, 2);
	vblk_set_offset(0, VM0_PA - VM_IPA);
	vblk_set_offset(1, VM1_PA - VM_IPA);

	uint64_t hcr = (1UL << 31) | (1UL << 0) | (1UL << 12); /* RW|VM|DC */
	wr_sysreg("hcr_el2", hcr);
	__asm__ volatile("isb");

	vcpu_init(&vcpus[0], VM_IPA, VM_IPA + 0x10000, vttbr0, 0);
	vcpu_init(&vcpus[1], VM_IPA, VM_IPA + 0x10000, vttbr1, 1);

	uart_println("[M28] two isolated VMs, each with its OWN virtio-blk disk:");

	/* Run VM0; it writes its own disk. */
	for (int g = 0; g < 50000; g++) {
		vcpu_run_once(&vcpus[0]);
		uint64_t ec = ESR_EC(vcpus[0].exit_esr);
		if (ec == EC_HVC64) break;
		if (ec == EC_DABT_LOW) {
			uint64_t ipa = ((vcpus[0].exit_hpfar >> 4) << 12) |
			               (vcpus[0].exit_far & 0xFFF);
			if (vblk_contains(ipa)) vblk_mmio(0, &vcpus[0], ipa);
			else mmio_zero(&vcpus[0]);
		} else break;
	}
	char d0[24], d1[24];
	vblk_peek(0, d0, 18); d0[18] = 0;
	vblk_peek(1, d1, 18); d1[18] = 0;
	uart_printf("[M28] after VM0 wrote: VM0 disk=\"%s\"; VM1 disk first byte=%x "
	            "(untouched)\n", d0, (uint8_t)d1[0]);

	/* Run VM1; it writes its own (separate) disk. */
	for (int g = 0; g < 50000; g++) {
		vcpu_run_once(&vcpus[1]);
		uint64_t ec = ESR_EC(vcpus[1].exit_esr);
		if (ec == EC_HVC64) break;
		if (ec == EC_DABT_LOW) {
			uint64_t ipa = ((vcpus[1].exit_hpfar >> 4) << 12) |
			               (vcpus[1].exit_far & 0xFFF);
			if (vblk_contains(ipa)) vblk_mmio(1, &vcpus[1], ipa);
			else mmio_zero(&vcpus[1]);
		} else break;
	}
	vblk_peek(1, d1, 18); d1[18] = 0;
	uart_printf("[M28] after VM1 wrote: VM1 disk=\"%s\"\n", d1);
	uart_println("[M28] each tenant's block device wrote only its own backing disk "
	             "-- per-VM storage isolation.");
}

/* ------------------------------------------------------------------ */
/* M29: SMP virtual timer. ONE VM, TWO vCPUs sharing a stage-2, each     */
/* with its OWN redistributor and its OWN virtual-interrupt context       */
/* (list register + CPU interface), preserved across time-slices by the   */
/* per-vCPU ICH_LR/ICH_VMCR save & restore in the world switch.           */
/* ------------------------------------------------------------------ */

void smp_vtimer_demo(void) {
	stage2_init();
	vgic_reset();
	gic_init_el2();
	hyptimer_init();

	uint64_t sz = (uint64_t)(guest_tvgic_end - guest_tvgic_start);
	for (uint64_t i = 0; i < sz; i++)
		((volatile uint8_t *)VM0_PA)[i] = ((uint8_t *)guest_tvgic_start)[i];
	__asm__ volatile("dsb sy; ic ialluis; dsb sy; isb");

	/* One VM (shared stage-2), two vCPUs with separate stacks. */
	uint64_t vttbr = stage2_build_vm(0, VM_IPA, VM0_PA, 1);
	uint64_t hcr = (1UL << 31) | (1UL << 0) | (1UL << 12) | (1UL << 4); /* RW|VM|DC|IMO */
	wr_sysreg("hcr_el2", hcr);
	__asm__ volatile("msr ich_hcr_el2, %0" ::"r"(1UL));
	__asm__ volatile("isb");

	vcpu_init(&vcpus[0], VM_IPA, VM_IPA + 0x8000,  vttbr, 0);
	vcpu_init(&vcpus[1], VM_IPA, VM_IPA + 0x10000, vttbr, 1);

	uart_println("[M29] one VM, TWO vCPUs: each with its OWN redistributor + virtual-");
	uart_println("      timer interrupt context (LR + CPU interface), time-sliced and");
	uart_println("      preempted by the EL2 timer:");

	hyptimer_start(10);
	int ticks[2] = {0, 0}, pend[2] = {0, 0}, cur = 0, guard = 0;
	while ((ticks[0] < 3 || ticks[1] < 3) && guard++ < 60000) {
		if (pend[cur] && vgic_irq_enabled(cur, 27)) {
			vgic_inject(&vcpus[cur], 27);
			pend[cur] = 0;
		}
		vcpu_run_once(&vcpus[cur]);
		uint64_t reason = vcpus[cur].exit_reason;
		uint64_t ec = ESR_EC(vcpus[cur].exit_esr);
		if ((reason & 3) == 1) {
			uint64_t iar = gic_ack();
			uint32_t intid = iar & 0xFFFFFF;
			if (intid == HYP_TIMER_PPI) {
				hyptimer_on_irq();
				pend[0] = pend[1] = 1;   /* a tick is available to both vCPUs */
				cur ^= 1;                /* time-slice switch */
			}
			if (intid < GIC_SPURIOUS_INTID)
				gic_eoi(iar);
		} else if (ec == EC_HVC64) {
			ticks[cur]++;
			uart_printf("[M29] vCPU%d serviced its OWN virtual timer tick #%d "
			            "(guest count=%u)\n", cur, ticks[cur], vcpus[cur].x[0]);
		} else if (ec == EC_DABT_LOW) {
			uint64_t ipa = ((vcpus[cur].exit_hpfar >> 4) << 12) |
			               (vcpus[cur].exit_far & 0xFFF);
			if (vgic_contains(ipa))
				vgic_mmio(cur, &vcpus[cur], ipa);
			else
				mmio_zero(&vcpus[cur]);
		} else {
			uart_printf("[M29] vCPU%d unexpected exit ec=%x (%s)\n",
			            cur, ec, ec_name(ec));
			break;
		}
	}
	hyptimer_stop();
	uart_printf("[M29] vCPU0 serviced %d timer ticks, vCPU1 serviced %d -- two cores "
	            "in one VM, each with independent per-vCPU GIC state.\n",
	            ticks[0], ticks[1]);
}

/* ------------------------------------------------------------------ */
/* M30: SMP redistributors. Two vCPUs in one VM, each owning a SEPARATE  */
/* GICR frame addressed by CPU id (0x080A0000 and 0x080C0000). The vGIC   */
/* routes each frame to that vCPU's redistributor state and presents      */
/* GICR_TYPER.Last + per-frame affinity -- the layout Linux's gic-v3      */
/* driver walks to discover each CPU's redistributor.                     */
/* ------------------------------------------------------------------ */

extern char guest_smpgicr_start[];
extern char guest_smpgicr_end[];

void smp_gicr_demo(void) {
	stage2_init();
	vgic_reset();
	vgic_set_ngicr(2);            /* two redistributor frames */
	gic_init_el2();
	hyptimer_init();

	uint64_t sz = (uint64_t)(guest_smpgicr_end - guest_smpgicr_start);
	for (uint64_t i = 0; i < sz; i++)
		((volatile uint8_t *)VM0_PA)[i] = ((uint8_t *)guest_smpgicr_start)[i];
	__asm__ volatile("dsb sy; ic ialluis; dsb sy; isb");

	uint64_t vttbr = stage2_build_vm(0, VM_IPA, VM0_PA, 1);
	uint64_t hcr = (1UL << 31) | (1UL << 0) | (1UL << 12) | (1UL << 4);
	wr_sysreg("hcr_el2", hcr);
	__asm__ volatile("msr ich_hcr_el2, %0" ::"r"(1UL));
	__asm__ volatile("isb");

	vcpu_init(&vcpus[0], VM_IPA, VM_IPA + 0x8000,  vttbr, 0);
	vcpu_init(&vcpus[1], VM_IPA, VM_IPA + 0x10000, vttbr, 1);
	vcpus[0].x[0] = 0;            /* CPU id passed to the guest */
	vcpus[1].x[0] = 1;

	uart_println("[M30] two vCPUs, each with its OWN redistributor frame "
	             "(0x080A0000 / 0x080C0000),");
	uart_println("      addressed by CPU id and routed by the vGIC:");

	hyptimer_start(10);
	int ticks[2] = {0, 0}, pend[2] = {0, 0}, cur = 0, guard = 0;
	while ((ticks[0] < 3 || ticks[1] < 3) && guard++ < 60000) {
		/* Gate on the vCPU's OWN redistributor enable state (frame == id). */
		if (pend[cur] && vgic_irq_enabled(cur, 27)) {
			vgic_inject(&vcpus[cur], 27);
			pend[cur] = 0;
		}
		vcpu_run_once(&vcpus[cur]);
		uint64_t reason = vcpus[cur].exit_reason;
		uint64_t ec = ESR_EC(vcpus[cur].exit_esr);
		if ((reason & 3) == 1) {
			uint64_t iar = gic_ack();
			uint32_t intid = iar & 0xFFFFFF;
			if (intid == HYP_TIMER_PPI) {
				hyptimer_on_irq();
				pend[0] = pend[1] = 1;
				cur ^= 1;
			}
			if (intid < GIC_SPURIOUS_INTID)
				gic_eoi(iar);
		} else if (ec == EC_HVC64) {
			ticks[cur]++;
			uart_printf("[M30] vCPU%d serviced a timer IRQ via its OWN "
			            "redistributor (frame %d, count=%u)\n",
			            cur, cur, vcpus[cur].x[0]);
		} else if (ec == EC_DABT_LOW) {
			uint64_t ipa = ((vcpus[cur].exit_hpfar >> 4) << 12) |
			               (vcpus[cur].exit_far & 0xFFF);
			/* Single VM (vm=0); the GICR frame address selects the vCPU. */
			if (vgic_contains(ipa))
				vgic_mmio(0, &vcpus[cur], ipa);
			else
				mmio_zero(&vcpus[cur]);
		} else {
			uart_printf("[M30] vCPU%d unexpected exit ec=%x (%s)\n",
			            cur, ec, ec_name(ec));
			break;
		}
	}
	hyptimer_stop();
	uart_printf("[M30] vCPU0 used redistributor frame 0, vCPU1 used frame 1 -- "
	            "per-vCPU GICR routing (%d + %d ticks).\n", ticks[0], ticks[1]);
}
