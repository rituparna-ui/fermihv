#include "vgic.h"
#include "exception.h"

/*
 * Minimal software GICv3 distributor/redistributor, now with PER-VM state so
 * several isolated tenants can each drive their own emulated GIC. Single
 * vCPU per VM for the multi-tenant case; the SGI path (per-vCPU) supports SMP
 * within a single VM.
 */
#define GICR_SGI_OFF 0x10000UL

struct vgic_state {
	uint32_t gicd_ctlr;
	uint32_t enabled;    /* SGI/PPI enable bitmap (INTID 0..31) */
	uint32_t group;      /* GICR_IGROUPR0 */
};

static struct vgic_state vg[VGIC_MAX_VCPUS];          /* indexed by VM */
static uint32_t g_sgi_pending[VGIC_MAX_VCPUS];        /* per-vCPU SGIs (SMP) */

int vgic_contains(uint64_t ipa) {
	return (ipa >= VGIC_GICD_BASE && ipa < VGIC_GICD_BASE + VGIC_GICD_SIZE) ||
	       (ipa >= VGIC_GICR_BASE && ipa < VGIC_GICR_BASE + VGIC_GICR_SIZE);
}

void vgic_reset(void) {
	for (int i = 0; i < VGIC_MAX_VCPUS; i++) {
		vg[i].gicd_ctlr = 0;
		vg[i].enabled = 0;
		vg[i].group = 0;
		g_sgi_pending[i] = 0;
	}
}

int vgic_irq_enabled(int vm, uint32_t intid) {
	return intid < 32 && (vg[vm].enabled & (1u << intid));
}

static uint32_t reg_read(int vm, uint64_t ipa) {
	if (ipa >= VGIC_GICR_BASE) {
		uint64_t off = ipa - VGIC_GICR_BASE;
		switch (off) {
		case 0x0014: return 0;            /* GICR_WAKER: awake */
		case 0x0008: return 1u << 4;      /* GICR_TYPER lo: Last; PLPIS=0 */
		case 0xFFE8: return 0x30;         /* GICR_PIDR2: GICv3 (ArchRev=3) */
		case GICR_SGI_OFF + 0x0080: return vg[vm].group;    /* IGROUPR0   */
		case GICR_SGI_OFF + 0x0100: return vg[vm].enabled;  /* ISENABLER0 */
		default: return 0;
		}
	}
	uint64_t off = ipa - VGIC_GICD_BASE;
	switch (off) {
	case 0x0000: return vg[vm].gicd_ctlr; /* GICD_CTLR (RWP=0) */
	case 0x0004: return 0x0007 | (9u << 19); /* GICD_TYPER: 256 INTIDs, IDbits=9 */
	case 0x0008: return 0;                /* GICD_IIDR */
	case 0xFFE8: return 0x30;             /* GICD_PIDR2: GICv3 */
	default: return 0;
	}
}

static void reg_write(int vm, uint64_t ipa, uint32_t val) {
	if (ipa >= VGIC_GICR_BASE) {
		uint64_t off = ipa - VGIC_GICR_BASE;
		switch (off) {
		case GICR_SGI_OFF + 0x0080: vg[vm].group = val; break;     /* IGROUPR0   */
		case GICR_SGI_OFF + 0x0100: vg[vm].enabled |= val; break;  /* ISENABLER0 */
		case GICR_SGI_OFF + 0x0180: vg[vm].enabled &= ~val; break; /* ICENABLER0 */
		default: break;
		}
		return;
	}
	uint64_t off = ipa - VGIC_GICD_BASE;
	if (off == 0x0000)
		vg[vm].gicd_ctlr = val;           /* GICD_CTLR */
}

void vgic_mmio(int vm, vcpu_t *v, uint64_t ipa) {
	uint64_t iss = ESR_ISS(v->exit_esr);
	if (!((iss >> 24) & 1)) {
		v->pc += 4;
		return;
	}
	int wnr = (iss >> 6) & 1;
	int srt = (iss >> 16) & 0x1F;
	if (wnr) {
		uint32_t val = (srt == 31) ? 0 : (uint32_t)v->x[srt];
		reg_write(vm, ipa, val);
	} else {
		uint32_t val = reg_read(vm, ipa);
		if (srt != 31)
			v->x[srt] = val;
	}
	v->pc += 4;
}

void vgic_post_sgi(int self, uint32_t intid) {
	for (int i = 0; i < VGIC_MAX_VCPUS; i++)
		if (i != self)
			g_sgi_pending[i] |= (1u << (intid & 0x1F));
}

int vgic_pop_sgi(int vcpu) {
	if (vcpu < 0 || vcpu >= VGIC_MAX_VCPUS || g_sgi_pending[vcpu] == 0)
		return -1;
	for (int b = 0; b < 16; b++) {
		if (g_sgi_pending[vcpu] & (1u << b)) {
			g_sgi_pending[vcpu] &= ~(1u << b);
			return b;
		}
	}
	return -1;
}
