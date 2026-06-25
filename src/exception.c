#include "exception.h"
#include "uart.h"

extern char el2_vector_table[];

static const char *vector_name(uint64_t idx) {
	switch (idx) {
	case 0: case 1: case 2: case 3:  return "CurEL/SP0";
	case 4: case 5: case 6: case 7:  return "CurEL/SPx";
	case 8: case 9: case 10: case 11: return "LowerEL/A64";
	default:                          return "LowerEL/A32";
	}
}

const char *ec_name(uint64_t ec) {
	switch (ec) {
	case EC_UNKNOWN:  return "UNKNOWN";
	case EC_WFx:      return "WFx";
	case EC_SIMD_FP:  return "FP/SIMD-trap";
	case EC_HVC64:    return "HVC64";
	case EC_SMC64:    return "SMC64";
	case EC_SYSREG:   return "MSR/MRS-trap";
	case EC_IABT_LOW: return "IABT-lower";
	case EC_IABT_CUR: return "IABT-curEL";
	case EC_PC_ALIGN: return "PC-align";
	case EC_DABT_LOW: return "DABT-lower";
	case EC_DABT_CUR: return "DABT-curEL";
	case EC_SP_ALIGN: return "SP-align";
	case EC_SERROR:   return "SError";
	case EC_BRK:      return "BRK";
	default:          return "?";
	}
}

void exceptions_init(void) {
	__asm__ volatile("msr vbar_el2, %0" ::"r"(el2_vector_table));
	__asm__ volatile("isb");
}

void el2_exception_dispatch(uint64_t vector_index, struct el2_frame *f) {
	uint64_t ec = ESR_EC(f->esr);

	uart_printf("[EL2-EXC] %s idx=%u ec=%x (%s) iss=%x\n",
	            vector_name(vector_index), vector_index, ec, ec_name(ec),
	            ESR_ISS(f->esr));
	uart_printf("          elr=%x spsr=%x far=%x\n", f->elr, f->spsr, f->far);

	switch (ec) {
	case EC_BRK:
		/* Software breakpoint: skip the 4-byte BRK and resume. This proves
		 * the full save -> decode -> mutate-frame -> restore -> eret loop. */
		uart_printf("          BRK #%x -> advancing ELR, recovering\n",
		            ESR_ISS(f->esr) & 0xFFFF);
		f->elr += 4;
		return;
	default:
		uart_println("[EL2-EXC] unhandled exception, halting");
		for (;;)
			__asm__ volatile("wfi");
	}
}
