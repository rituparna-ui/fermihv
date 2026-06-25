#ifndef FERMIHV_VGIC_H
#define FERMIHV_VGIC_H

#include <stdint.h>
#include "vcpu.h"

/* Emulated GICv3 distributor + this-PE redistributor (QEMU virt addresses).
 * These are deliberately NOT passthrough-mapped in stage-2, so guest accesses
 * trap to EL2 and are emulated here -- the basis for interrupt isolation and
 * (later) virtual IPIs for SMP. */
#define VGIC_GICD_BASE 0x08000000UL
#define VGIC_GICD_SIZE 0x00010000UL
#define VGIC_GICR_BASE 0x080A0000UL
#define VGIC_GICR_SIZE 0x00020000UL   /* RD_base + SGI_base frames */

/* True if an IPA falls inside the emulated distributor/redistributor. */
int vgic_contains(uint64_t ipa);

/* Reset emulated vGIC state. */
void vgic_reset(void);

/* Emulate the trapped guest GIC MMIO access (decodes ESR_EL2.ISS), then
 * advance the guest PC past it. */
void vgic_mmio(vcpu_t *v, uint64_t ipa);

/* Has the guest enabled this INTID (SGI/PPI) in the emulated GIC? Used to gate
 * virtual interrupt injection. */
int vgic_irq_enabled(uint32_t intid);

#endif /* FERMIHV_VGIC_H */
