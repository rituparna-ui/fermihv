/* Embed the fermi-os guest kernel image so the hypervisor can place it in
 * guest RAM at its native 0x40000000 at runtime (avoiding a ROM-load overlap
 * with QEMU's auto-generated DTB at the RAM base). */
.section .rodata
.balign 16
.global fermios_image_start
.global fermios_image_end
fermios_image_start:
	.incbin "build/fermios.bin"
fermios_image_end:
