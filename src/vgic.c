#include "vgic.h"
#include "exception.h"

/*
 * A deliberately minimal software GICv3 distributor/redistributor: enough for
 * a guest's GIC driver to enable an interrupt and for us to gate virtual
 * interrupt injection on it. Single vCPU / single redistributor for now.
 *
 *   GICD @ 0x08000000 : CTLR, TYPER, IIDR (+ SPI enable/priority ignored)
 *   GICR @ 0x080A0000 : RD_base (WAKER, TYPER) and SGI_base @ +0x10000
 *                       (IGROUPR0, ISENABLER0, ICENABLER0, IPRIORITYR, ICFGR)
 */
#define GICR_SGI_OFF 0x10000UL

static uint32_t g_gicd_ctlr;
static uint32_t g_enabled;   /* SGI/PPI enable bitmap (INTID 0..31) */
static uint32_t g_group;     /* GICR_IGROUPR0 */

int vgic_contains(uint64_t ipa) {
	return (ipa >= VGIC_GICD_BASE && ipa < VGIC_GICD_BASE + VGIC_GICD_SIZE) ||
	       (ipa >= VGIC_GICR_BASE && ipa < VGIC_GICR_BASE + VGIC_GICR_SIZE);
}

void vgic_reset(void) {
	g_gicd_ctlr = 0;
	g_enabled = 0;
	g_group = 0;
}

int vgic_irq_enabled(uint32_t intid) {
	return intid < 32 && (g_enabled & (1u << intid));
}

static uint32_t reg_read(uint64_t ipa) {
	if (ipa >= VGIC_GICR_BASE) {
		uint64_t off = ipa - VGIC_GICR_BASE;
		switch (off) {
		case 0x0014: return 0;            /* GICR_WAKER: awake, children awake */
		case 0x0008: return 1u << 4;      /* GICR_TYPER lo: Last redistributor */
		case GICR_SGI_OFF + 0x0080: return g_group;    /* IGROUPR0   */
		case GICR_SGI_OFF + 0x0100: return g_enabled;  /* ISENABLER0 */
		default: return 0;
		}
	}
	uint64_t off = ipa - VGIC_GICD_BASE;
	switch (off) {
	case 0x0000: return g_gicd_ctlr;      /* GICD_CTLR  */
	case 0x0004: return 0;                /* GICD_TYPER: ITLinesNumber=0 (32 INTIDs) */
	default: return 0;                    /* IIDR and the rest read as 0 */
	}
}

static void reg_write(uint64_t ipa, uint32_t val) {
	if (ipa >= VGIC_GICR_BASE) {
		uint64_t off = ipa - VGIC_GICR_BASE;
		switch (off) {
		case GICR_SGI_OFF + 0x0080: g_group = val; break;    /* IGROUPR0   */
		case GICR_SGI_OFF + 0x0100: g_enabled |= val; break; /* ISENABLER0 */
		case GICR_SGI_OFF + 0x0180: g_enabled &= ~val; break;/* ICENABLER0 */
		default: break;                                      /* WAKER/prio/cfg: ignore */
		}
		return;
	}
	uint64_t off = ipa - VGIC_GICD_BASE;
	if (off == 0x0000)
		g_gicd_ctlr = val;                /* GICD_CTLR */
	/* SPI enable/priority/route ignored (demo uses a PPI). */
}

void vgic_mmio(vcpu_t *v, uint64_t ipa) {
	uint64_t iss = ESR_ISS(v->exit_esr);
	if (!((iss >> 24) & 1)) {             /* no decoded syndrome */
		v->pc += 4;
		return;
	}
	int wnr = (iss >> 6) & 1;
	int srt = (iss >> 16) & 0x1F;
	if (wnr) {
		uint32_t val = (srt == 31) ? 0 : (uint32_t)v->x[srt];
		reg_write(ipa, val);
	} else {
		uint32_t val = reg_read(ipa);
		if (srt != 31)
			v->x[srt] = val;
	}
	v->pc += 4;
}
