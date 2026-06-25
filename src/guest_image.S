/* Embed the standalone guest kernel binary (built separately) so the
 * hypervisor can copy it into guest RAM at runtime. */
.section .rodata
.balign 16
.global nano_image_start
.global nano_image_end
nano_image_start:
	.incbin "build/nano.bin"
nano_image_end:
