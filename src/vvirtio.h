#ifndef FERMIHV_VVIRTIO_H
#define FERMIHV_VVIRTIO_H

#include <stdint.h>
#include "vcpu.h"

/* An emulated legacy virtio-mmio console device. Left unmapped in stage-2 so
 * guest register accesses trap and are emulated here; on a TX queue notify we
 * walk the guest's split virtqueue and write the buffered bytes to the real
 * console. (Single VM / identity stage-2 for now: guest IPA == host PA.) */
#define VIRTIO_BASE 0x0A000000UL
#define VIRTIO_SIZE 0x0200UL

int virtio_contains(uint64_t ipa);
void virtio_reset(void);
void virtio_mmio(int vm, vcpu_t *v, uint64_t ipa);
void virtio_set_offset(int vm, uint64_t off); /* guest->host translation */

/* Returns 1 (once) if VM `vm`'s device raised a completion interrupt. */
int virtio_take_irq(int vm);

/* --- virtio-blk (block device over a backing RAM disk) --- */
#define VBLK_BASE 0x0A000200UL
int vblk_contains(uint64_t ipa);
void vblk_reset(void);
void vblk_mmio(vcpu_t *v, uint64_t ipa);
int vblk_take_irq(void);
void vblk_peek(char *out, int n);   /* hypervisor's view of disk sector 0 */
void vblk_poke(const char *in, int n); /* hypervisor seeds disk sector 0 */

#endif /* FERMIHV_VVIRTIO_H */
