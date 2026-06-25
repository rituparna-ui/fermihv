/* FermiHV linker script.
 *
 * The hypervisor runs at EL2 with the MMU OFF for now (identity / flat),
 * so the virtual address == physical address == load address. QEMU's
 * `virt` machine places RAM at 0x40000000 and the `-kernel` loader drops
 * the ELF there and enters at _start in EL2.
 *
 * We split LOAD segments by permission so the ELF never advertises an
 * RWX segment (keeps the linker quiet and is good hygiene).
 */
ENTRY(_start)

/* The hypervisor lives high in RAM (0x70000000) so a guest (Linux) can own
 * the low RAM at 0x40000000 that its load address and DTB memory node
 * expect. QEMU loads the ELF at its physical link address and enters _start
 * at EL2. With -m 2G, RAM is 0x40000000..0xBFFFFFFF, so 0x70000000 is valid. */
HV_LOAD_ADDR = 0x70000000;

PHDRS
{
	text   PT_LOAD FLAGS(5);  /* R + X */
	rodata PT_LOAD FLAGS(4);  /* R     */
	data   PT_LOAD FLAGS(6);  /* R + W */
}

SECTIONS
{
	. = HV_LOAD_ADDR;

	.text ALIGN(4096) : {
		__text_start = .;
		KEEP(*(.text.boot))      /* _start must come first */
		. = ALIGN(2048);
		KEEP(*(.text.vectors))   /* VBAR_EL2 table (M1+), 2KiB-aligned */
		*(.text*)
		__text_end = .;
	} :text

	.rodata ALIGN(4096) : {
		*(.rodata*)
	} :rodata

	.data ALIGN(4096) : {
		*(.data*)
	} :data

	.bss ALIGN(4096) (NOLOAD) : {
		__bss_start = .;
		*(.bss*)
		*(COMMON)
		__bss_end = .;
	} :data

	/* 64 KiB EL2 boot/hypervisor stack, grows downward from __stack_top */
	. = ALIGN(4096);
	. = . + 0x10000;
	__stack_top = .;

	. = ALIGN(4096);
	__hv_end = .;            /* everything past here is free physical RAM */
}
