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
#define VGIC_GICR_STRIDE 0x00020000UL /* per-vCPU RD_base + SGI_base */
#define VGIC_MAX_VCPUS 4
#define VGIC_GICR_SIZE (VGIC_MAX_VCPUS * VGIC_GICR_STRIDE)

/* True if an IPA falls inside the emulated distributor/redistributor. */
int vgic_contains(uint64_t ipa);

/* Reset emulated vGIC state. */
void vgic_reset(void);

/* Emulate the trapped guest GIC MMIO access for VM `vm` (decodes
 * ESR_EL2.ISS), then advance the guest PC past it. */
void vgic_mmio(int vm, vcpu_t *v, uint64_t ipa);

/* Has VM `vm`'s guest enabled this INTID (SGI/PPI) in its emulated GIC? */
int vgic_irq_enabled(int vm, uint32_t intid);

/* Has VM `vm`'s guest enabled this SPI (INTID >= 32) in its GICD? */
int vgic_spi_enabled(int vm, uint32_t intid);

/* --- virtual SGIs (inter-processor interrupts) --- */

/* Set the number of redistributor frames presented (GICR_TYPER.Last is set on
 * the last one). Defaults to 1. */
void vgic_set_ngicr(int n);

/* Route an SGI sent by vCPU `self` (a trapped ICC_SGI1R write, IRM=1 /
 * all-but-self semantics): mark `intid` pending on every other vCPU. */
void vgic_post_sgi(int self, uint32_t intid);

/* Pop the lowest pending SGI for `vcpu` (clearing it), or -1 if none. */
int vgic_pop_sgi(int vcpu);

#endif /* FERMIHV_VGIC_H */
