#include "vcpu.h"
#include "uart.h"
#include "stage2.h"
#include "exception.h"

extern void guest_entry(void);

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
/* M4 demo scheduler                                                  */
/* ------------------------------------------------------------------ */

#define NUM_VCPUS 2
#define HVC_BUDGET 3            /* hypercalls each vCPU makes before halting */

static vcpu_t vcpus[NUM_VCPUS];
static uint8_t gstack[NUM_VCPUS][4096] __attribute__((aligned(16)));

void sched_demo(void) {
	stage2_init();

	/* HCR_EL2: RW (AArch64 EL1) | VM (stage-2) | DC (default cacheable). */
	uint64_t hcr = (1UL << 31) | (1UL << 0) | (1UL << 12);
	wr_sysreg("hcr_el2", hcr);
	__asm__ volatile("isb");

	uint64_t vttbr = stage2_vttbr();
	uint64_t entry = (uint64_t)&guest_entry;
	for (int i = 0; i < NUM_VCPUS; i++)
		vcpu_init(&vcpus[i], entry,
		          (uint64_t)(gstack[i] + sizeof(gstack[i])), vttbr, i);

	uart_println("[M4] scheduler: 2 vCPUs, round-robin, independent state");

	int hcalls[NUM_VCPUS] = {0};
	int active = NUM_VCPUS;
	while (active) {
		for (int i = 0; i < NUM_VCPUS; i++) {
			vcpu_t *v = &vcpus[i];
			if (v->halted)
				continue;

			vcpu_run_once(v);

			uint64_t ec = ESR_EC(v->exit_esr);
			if (ec == EC_HVC64) {
				uart_printf("[M4] vCPU %d hvc #%d -> guest counter x0=%u\n",
				            v->id, hcalls[i] + 1, v->x[0]);
				if (++hcalls[i] >= HVC_BUDGET) {
					v->halted = 1;
					active--;
					uart_printf("[M4] vCPU %d reached budget, halting\n",
					            v->id);
				}
			} else if (ec == EC_DABT_LOW || ec == EC_IABT_LOW) {
				uint64_t ipa = (v->exit_hpfar >> 4) << 12;
				stage2_map_1gb(ipa & ~0x3FFFFFFFUL); /* fault-in, retry */
			} else {
				uart_printf("[M4] vCPU %d unexpected exit ec=%x (%s), halting\n",
				            v->id, ec, ec_name(ec));
				v->halted = 1;
				active--;
			}
		}
	}

	uart_println("[M4] all vCPUs halted; scheduler done.");
}
