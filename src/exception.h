#ifndef FERMIHV_EXCEPTION_H
#define FERMIHV_EXCEPTION_H

#include <stdint.h>

/* Must match the trap-frame layout built in vectors.S (FRAME_SIZE = 288). */
struct el2_frame {
	uint64_t x[31];   /* x0 .. x30        [sp + 0]   */
	uint64_t elr;     /* ELR_EL2          [sp + 248] */
	uint64_t spsr;    /* SPSR_EL2         [sp + 256] */
	uint64_t esr;     /* ESR_EL2          [sp + 264] */
	uint64_t far;     /* FAR_EL2          [sp + 272] */
};

/* ESR_EL2.EC exception-class encodings we care about. */
#define EC_UNKNOWN   0x00
#define EC_WFx       0x01
#define EC_SIMD_FP   0x07  /* trapped FP/SIMD access (CPTR_EL2)            */
#define EC_HVC64     0x16  /* HVC from AArch64 (guest hypercall)          */
#define EC_SMC64     0x17
#define EC_SYSREG    0x18  /* trapped MSR/MRS/system instruction          */
#define EC_IABT_LOW  0x20  /* instruction abort from lower EL (stage-2)   */
#define EC_IABT_CUR  0x21
#define EC_PC_ALIGN  0x22
#define EC_DABT_LOW  0x24  /* data abort from lower EL (stage-2 / MMIO)   */
#define EC_DABT_CUR  0x25
#define EC_SP_ALIGN  0x26
#define EC_SERROR    0x2F
#define EC_BRK       0x3C  /* BRK instruction (AArch64)                   */

#define ESR_EC(esr)  (((esr) >> 26) & 0x3F)
#define ESR_ISS(esr) ((esr) & 0x1FFFFFF)

void exceptions_init(void);
const char *ec_name(uint64_t ec);
void el2_exception_dispatch(uint64_t vector_index, struct el2_frame *f);

#endif /* FERMIHV_EXCEPTION_H */
