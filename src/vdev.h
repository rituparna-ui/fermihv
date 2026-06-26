#ifndef FERMIHV_VDEV_H
#define FERMIHV_VDEV_H

#include <stdint.h>
#include "vcpu.h"

/* The guest sees a PL011-like UART at this IPA. It is deliberately left
 * unmapped in stage-2, so every guest access faults to EL2 for emulation. */
#define VUART_BASE 0x09000000UL
#define VUART_SIZE 0x1000UL

/* True if an IPA falls inside the emulated UART window. */
int vuart_contains(uint64_t ipa);

/* Push a host input byte into the emulated UART; query whether it is now
 * asserting a receive interrupt (byte pending and RX unmasked by the guest). */
void vuart_push_rx(int c);
int vuart_rx_irq_pending(void);

/* Emulate the trapped guest load/store at `ipa` by decoding ESR_EL2.ISS in
 * v->exit_esr, performing the device side effect, writing back any loaded
 * value into the guest register file, and advancing the guest PC past the
 * faulting instruction. */
void mmio_emulate(vcpu_t *v, uint64_t ipa);

#endif /* FERMIHV_VDEV_H */
