#ifndef FERMIHV_MMIO_H
#define FERMIHV_MMIO_H

#include <stdint.h>

static inline void mmio_write32(uint64_t addr, uint32_t val) {
	*(volatile uint32_t *)addr = val;
}

static inline uint32_t mmio_read32(uint64_t addr) {
	return *(volatile uint32_t *)addr;
}

#endif /* FERMIHV_MMIO_H */
