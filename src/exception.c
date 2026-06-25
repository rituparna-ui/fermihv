#include "exception.h"
#include "uart.h"
#include "stage2.h"

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
	case EC_HVC64:
		/* Guest hypercall. imm16 is in ISS; arguments by convention in
		 * x0.. . ELR_EL2 already points past the HVC, so do NOT advance. */
		uart_printf("          HVC #%x from guest, x0=%x -> handled, resuming\n",
		            ESR_ISS(f->esr) & 0xFFFF, f->x[0]);
		return;
	case EC_DABT_LOW:
	case EC_IABT_LOW: {
		/* Stage-2 abort from the guest. HPFAR_EL2 carries the faulting IPA
		 * (bits[39:4] = IPA[47:12]); FAR_EL2 has the guest VA. Fault in the
		 * containing 1GiB block and retry (do NOT advance ELR). */
		uint64_t hpfar;
		__asm__ volatile("mrs %0, hpfar_el2" : "=r"(hpfar));
		uint64_t ipa = (hpfar >> 4) << 12;
		uint64_t base = ipa & ~0x3FFFFFFFUL;
		uart_printf("          stage-2 %s: guest_va=%x ipa=%x\n",
		            ec == EC_DABT_LOW ? "data-abort" : "insn-abort",
		            f->far, ipa);
		uart_printf("          faulting in 1GiB block @%x and retrying\n", base);
		stage2_map_1gb(base);
		return;
	}
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
