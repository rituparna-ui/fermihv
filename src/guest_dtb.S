/* Embed the guest device tree blob so the hypervisor can place it in guest
 * RAM at a controlled address (independent of QEMU loader quirks). */
.section .rodata
.balign 16
.global guest_dtb_start
.global guest_dtb_end
guest_dtb_start:
	.incbin "build/guest.dtb"
guest_dtb_end:
