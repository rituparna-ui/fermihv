#include "vcpu.h"
#include "uart.h"
#include "stage2.h"
#include "exception.h"
#include "vdev.h"
#include "gic.h"
#include "timer.h"

extern void guest_counter_entry(void);
extern void guest_vuart_entry(void);
extern void guest_virq_entry(void);

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
	v->id = id;
	v->halted = 0;
}

void vcpu_run_once(vcpu_t *v) {
	restore_el1_sysregs(v);
	wr_sysreg("vttbr_el2", v->vttbr);
	__asm__ volatile("isb");
	__guest_enter(v);          /* returns when the guest traps back to EL2 */
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
static void vgic_inject(uint32_t vintid) {
	uint64_t lr = (1ULL << 62) | (1ULL << 60) | (uint64_t)vintid;
	__asm__ volatile("msr ich_lr0_el2, %0" ::"r"(lr));
	__asm__ volatile("isb");
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
				vgic_inject(VTIMER_VINTID);
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
	case 0xC4000003:                 /* CPU_ON (64-bit): single vCPU only */
	case 0x84000003:
		v->x[0] = (uint64_t)-2;      /* INVALID_PARAMETERS */
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
