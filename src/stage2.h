#ifndef FERMIHV_STAGE2_H
#define FERMIHV_STAGE2_H

#include <stdint.h>

/* Build the stage-2 page tables, map the guest's initial RAM window, and
 * program VTCR_EL2 / VTTBR_EL2. Call before enabling HCR_EL2.VM. */
void stage2_init(void);

/* Identity-map one 1GiB stage-2 block (IPA == PA) as Normal cacheable RW,
 * then invalidate the stage-1&2 TLB. Used both at init and to fault-in
 * blocks on demand from the data/instruction abort handler. */
void stage2_map_1gb(uint64_t ipa);

#endif /* FERMIHV_STAGE2_H */
