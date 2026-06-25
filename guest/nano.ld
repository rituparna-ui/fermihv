/* Standalone guest kernel ("nano"). Linked and loaded at guest-physical
 * 0x48000000 -- distinct from the hypervisor (0x40000000) so the identity
 * stage-2 map needs no relocation. The hypervisor copies this image there
 * and erets to _start at EL1. */
ENTRY(_start)

SECTIONS
{
	. = 0x48000000;

	.text : {
		KEEP(*(.text.boot))
		*(.text*)
	}
	.rodata : { *(.rodata*) }
	.data   : { *(.data*) }
	.bss (NOLOAD) : {
		__bss_start = .;
		*(.bss*)
		*(COMMON)
		__bss_end = .;
	}

	. = ALIGN(16);
	. = . + 0x4000;        /* 16 KiB guest stack */
	__stack_top = .;
}
