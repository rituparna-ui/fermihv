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
ASFLAGS := -ffreestanding -nostdlib -nostartfiles -fno-pic -g -MMD -MP -Isrc -I.
LDFLAGS := -nostdlib -T linker.ld

C_SRCS  := $(wildcard src/*.c)
S_SRCS  := $(wildcard src/*.S)
OBJS    := $(patsubst src/%.c,$(BUILD)/%.o,$(C_SRCS)) \
           $(patsubst src/%.S,$(BUILD)/%.o,$(S_SRCS))
DEPS    := $(OBJS:.o=.d)

# --- standalone guest kernel ("nano"), built as its own image and embedded
# into the hypervisor via src/guest_image.S (.incbin). ---
GUEST_DIR   := guest
GUEST_BIN   := $(BUILD)/nano.bin
GUEST_FLAGS := -ffreestanding -nostdlib -nostartfiles -fno-pic -mstrict-align \
               -mgeneral-regs-only -O2 -g -Wl,--no-warn-rwx-segments

$(GUEST_BIN): $(GUEST_DIR)/nano_boot.S $(GUEST_DIR)/nano.c $(GUEST_DIR)/nano.ld
	@mkdir -p $(BUILD)
	@echo "GUEST $@"
	@$(CC) $(GUEST_FLAGS) -Wl,-T,$(GUEST_DIR)/nano.ld -o $(BUILD)/nano.elf \
		$(GUEST_DIR)/nano_boot.S $(GUEST_DIR)/nano.c
	@$(CROSS)objcopy -O binary $(BUILD)/nano.elf $@

# guest_image.S incbin's the guest binary, so it must exist first.
$(BUILD)/guest_image.o: $(GUEST_BIN)

# Guest device tree blob, compiled from the checked-in source. We also build
# a tiny initramfs (static /init) and inject its location into the DTB so the
# Linux guest reaches userspace instead of panicking for lack of a rootfs.
INITRD_ADDR  := 0x4c000000
GUEST_INIT   := $(BUILD)/init
GUEST_INITRD := $(BUILD)/initramfs.cpio.gz
GUEST_DTB    := $(BUILD)/guest.dtb

$(GUEST_INIT): $(GUEST_DIR)/init.c
	@mkdir -p $(BUILD)
	@echo "INIT $@"
	@$(CC) -static -no-pie -nostdlib -nostartfiles -fno-pic -O2 -o $@ $<

$(GUEST_INITRD): $(GUEST_INIT)
	@echo "INITRD $@"
	@rm -rf $(BUILD)/initrd && mkdir -p $(BUILD)/initrd/dev
	@cp $(GUEST_INIT) $(BUILD)/initrd/init && chmod +x $(BUILD)/initrd/init
	@mknod $(BUILD)/initrd/dev/console c 5 1 2>/dev/null || true
	@cd $(BUILD)/initrd && find . | cpio -o -H newc 2>/dev/null | gzip > ../initramfs.cpio.gz

$(GUEST_DTB): $(GUEST_DIR)/guest.dts $(GUEST_INITRD)
	@mkdir -p $(BUILD)
	@echo "DTC $@"
	@sz=$$(stat -c%s $(GUEST_INITRD)); \
	 end=$$(printf '0x%x' $$(( $(INITRD_ADDR) + sz )) ); \
	 sed "s|stdout-path = \"/pl011@9000000\";|stdout-path = \"/pl011@9000000\";\n\t\tlinux,initrd-start = <$(INITRD_ADDR)>;\n\t\tlinux,initrd-end = <$$end>;|" \
	     $(GUEST_DIR)/guest.dts > $(BUILD)/guest.full.dts; \
	 dtc -I dts -O dtb $(BUILD)/guest.full.dts -o $@ 2>/dev/null
$(BUILD)/guest_dtb.o: $(GUEST_DTB)

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
