# FermiHV build. Run INSIDE the toolchain container (aarch64-linux-gnu-*).
# From the host, use ./run.sh which wraps this in a container snapshotted
# from the running `osdev` container.

CROSS   := aarch64-linux-gnu-
CC      := $(CROSS)gcc
LD      := $(CROSS)ld

BUILD   := build
TARGET  := $(BUILD)/fermihv.elf

# -mgeneral-regs-only: forbid the compiler from using FP/SIMD registers so we
# don't have to enable the FP trap path (CPTR_EL2) at boot. uart_printf uses
# varargs but only integer/pointer args, so GP regs suffice.
CFLAGS  := -ffreestanding -nostdlib -nostartfiles -fno-pic -mstrict-align \
           -mgeneral-regs-only -Wall -Wextra -O2 -g -MMD -MP -Isrc
ASFLAGS := -ffreestanding -nostdlib -nostartfiles -fno-pic -g -MMD -MP -Isrc
LDFLAGS := -nostdlib -T linker.ld

C_SRCS  := $(wildcard src/*.c)
S_SRCS  := $(wildcard src/*.S)
OBJS    := $(patsubst src/%.c,$(BUILD)/%.o,$(C_SRCS)) \
           $(patsubst src/%.S,$(BUILD)/%.o,$(S_SRCS))
DEPS    := $(OBJS:.o=.d)

# QEMU: virt machine, GICv3, virtualization extensions ON -> enters at EL2.
QEMU         := qemu-system-aarch64
QEMU_MACHINE := virt,gic-version=3,virtualization=on
QEMU_FLAGS   := -machine $(QEMU_MACHINE) -cpu cortex-a72 -m 2G -nographic -kernel $(TARGET)

.PHONY: all run debug clean
all: $(TARGET)

$(TARGET): $(OBJS) linker.ld
	@mkdir -p $(BUILD)
	@echo "LD  $@"
	@$(LD) $(LDFLAGS) -o $@ $(OBJS)

$(BUILD)/%.o: src/%.c
	@mkdir -p $(BUILD)
	@echo "CC  $<"
	@$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: src/%.S
	@mkdir -p $(BUILD)
	@echo "AS  $<"
	@$(CC) $(ASFLAGS) -x assembler-with-cpp -c $< -o $@

run: all
	$(QEMU) $(QEMU_FLAGS)

# Wait for GDB on :1234, start paused.
debug: all
	$(QEMU) $(QEMU_FLAGS) -s -S

clean:
	rm -rf $(BUILD)

-include $(DEPS)
