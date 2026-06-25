#ifndef FERMIHV_VCPU_OFFSETS_H
#define FERMIHV_VCPU_OFFSETS_H

/* Byte offsets into struct vcpu for the fields touched by the world-switch
 * assembly (world.S, vectors.S). Verified against the C struct with
 * _Static_assert in vcpu.h, so a layout drift fails the build. */
#define VCPU_X            0    /* x[0..30], 31 * 8 = 248 bytes */
#define VCPU_SP_EL0     248
#define VCPU_PC         256    /* guest PC  (ELR_EL2) */
#define VCPU_PSTATE     264    /* guest PSTATE (SPSR_EL2) */
#define VCPU_HYP_SP     272    /* saved EL2 SP for the return path */
#define VCPU_EXIT_REASON 280   /* vector index that caused the exit */
#define VCPU_EXIT_ESR   288
#define VCPU_EXIT_FAR   296
#define VCPU_EXIT_HPFAR 304
/* FP/SIMD state (saved/restored across the world switch). 320 is 16-aligned
 * for `stp q,q`; q0..q31 occupy 512 bytes. */
#define VCPU_FP         320
#define VCPU_FPSR       832
#define VCPU_FPCR       840

#endif /* FERMIHV_VCPU_OFFSETS_H */
