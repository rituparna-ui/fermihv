#include "vvirtio.h"
#include "exception.h"
#include "uart.h"

/*
 * Legacy (version 1) virtio-mmio console. Register map (offsets):
 *   0x000 MagicValue   "virt"   0x004 Version      1
 *   0x008 DeviceID     3 (con)  0x00c VendorID
 *   0x010 DeviceFeat            0x028 GuestPageSize
 *   0x030 QueueSel              0x034 QueueNumMax
 *   0x038 QueueNum              0x03c QueueAlign
 *   0x040 QueuePFN              0x050 QueueNotify
 *   0x070 Status
 * The TX virtqueue (split) lives at QueuePFN*GuestPageSize in guest memory:
 *   desc[QueueNum] (16B each), then the avail ring at +QueueNum*16.
 */
static uint32_t v_status;
static uint32_t v_page_size = 4096;
static uint32_t v_queue_num;
static uint32_t v_queue_pfn;
static uint16_t v_last_avail;

int virtio_contains(uint64_t ipa) {
	return ipa >= VIRTIO_BASE && ipa < VIRTIO_BASE + VIRTIO_SIZE;
}

void virtio_reset(void) {
	v_status = 0;
	v_page_size = 4096;
	v_queue_num = 0;
	v_queue_pfn = 0;
	v_last_avail = 0;
}

/* Process the TX queue: emit each newly-available buffer to the console.
 * Identity stage-2 -> guest physical address == host physical address. */
static void virtio_process_tx(void) {
	if (!v_queue_pfn || !v_queue_num)
		return;
	uint64_t base = (uint64_t)v_queue_pfn * v_page_size;
	uint64_t avail = base + (uint64_t)v_queue_num * 16;
	uint16_t avail_idx = *(volatile uint16_t *)(avail + 2);

	while (v_last_avail != avail_idx) {
		uint16_t slot = v_last_avail % v_queue_num;
		uint16_t didx = *(volatile uint16_t *)(avail + 4 + 2 * slot);
		uint64_t desc = base + (uint64_t)didx * 16;
		uint64_t addr = *(volatile uint64_t *)(desc + 0);
		uint32_t len = *(volatile uint32_t *)(desc + 8);
		for (uint32_t i = 0; i < len; i++)
			uart_putc(*(volatile char *)(addr + i));
		v_last_avail++;
	}
}

void virtio_mmio(vcpu_t *v, uint64_t ipa) {
	uint64_t iss = ESR_ISS(v->exit_esr);
	if (!((iss >> 24) & 1)) {
		v->pc += 4;
		return;
	}
	int wnr = (iss >> 6) & 1;
	int srt = (iss >> 16) & 0x1F;
	uint64_t off = ipa - VIRTIO_BASE;

	if (wnr) {
		uint32_t val = (srt == 31) ? 0 : (uint32_t)v->x[srt];
		switch (off) {
		case 0x028: v_page_size = val; break;     /* GuestPageSize */
		case 0x038: v_queue_num = val; break;      /* QueueNum      */
		case 0x040: v_queue_pfn = val; break;      /* QueuePFN      */
		case 0x070: v_status = val; break;         /* Status        */
		case 0x050: virtio_process_tx(); break;    /* QueueNotify   */
		default: break;                            /* QueueSel/Align/ACK: ignore */
		}
	} else {
		uint32_t val = 0;
		switch (off) {
		case 0x000: val = 0x74726976; break;       /* "virt"   */
		case 0x004: val = 1; break;                /* Version (legacy) */
		case 0x008: val = 3; break;                /* DeviceID: console */
		case 0x00c: val = 0x554d4551; break;       /* VendorID "QEMU"  */
		case 0x010: val = 0; break;                /* DeviceFeatures   */
		case 0x034: val = 8; break;                /* QueueNumMax      */
		case 0x040: val = v_queue_pfn; break;
		case 0x070: val = v_status; break;
		default: val = 0; break;
		}
		if (srt != 31)
			v->x[srt] = val;
	}
	v->pc += 4;
}
