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
static uint32_t v_int_status;   /* virtio InterruptStatus */
static int v_irq;               /* completion IRQ pending for the scheduler */

int virtio_contains(uint64_t ipa) {
	return ipa >= VIRTIO_BASE && ipa < VIRTIO_BASE + VIRTIO_SIZE;
}

void virtio_reset(void) {
	v_status = 0;
	v_page_size = 4096;
	v_queue_num = 0;
	v_queue_pfn = 0;
	v_last_avail = 0;
	v_int_status = 0;
	v_irq = 0;
}

/* Returns 1 (once) if the device has raised a completion interrupt. */
int virtio_take_irq(void) {
	if (v_irq) {
		v_irq = 0;
		return 1;
	}
	return 0;
}

/* Process the TX queue: emit each newly-available buffer to the console.
 * Identity stage-2 -> guest physical address == host physical address. */
static void virtio_process_tx(void) {
	if (!v_queue_pfn || !v_queue_num)
		return;
	uint64_t base = (uint64_t)v_queue_pfn * v_page_size;
	uint64_t avail = base + (uint64_t)v_queue_num * 16;
	uint16_t avail_idx = *(volatile uint16_t *)(avail + 2);

	/* Legacy used ring: aligned up to QueueAlign (4096) after the avail ring. */
	uint64_t used = (avail + 6 + 2 * (uint64_t)v_queue_num + 4095) & ~4095ULL;
	int consumed = 0;

	while (v_last_avail != avail_idx) {
		uint16_t slot = v_last_avail % v_queue_num;
		uint16_t didx = *(volatile uint16_t *)(avail + 4 + 2 * slot);
		uint64_t desc = base + (uint64_t)didx * 16;
		uint64_t addr = *(volatile uint64_t *)(desc + 0);
		uint32_t len = *(volatile uint32_t *)(desc + 8);
		for (uint32_t i = 0; i < len; i++)
			uart_putc(*(volatile char *)(addr + i));
		/* used.ring[used_idx % num] = { id=didx, len=0 } */
		uint16_t uidx = *(volatile uint16_t *)(used + 2);
		uint64_t ue = used + 4 + 8 * (uidx % v_queue_num);
		*(volatile uint32_t *)(ue + 0) = didx;
		*(volatile uint32_t *)(ue + 4) = 0;
		*(volatile uint16_t *)(used + 2) = uidx + 1;
		v_last_avail++;
		consumed = 1;
	}
	if (consumed) {
		v_int_status |= 1;   /* used-buffer notification */
		v_irq = 1;           /* signal the scheduler to inject the device IRQ */
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
		case 0x064: v_int_status &= ~val; break;   /* InterruptACK  */
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
		case 0x060: val = v_int_status; break;     /* InterruptStatus */
		case 0x070: val = v_status; break;
		default: val = 0; break;
		}
		if (srt != 31)
			v->x[srt] = val;
	}
	v->pc += 4;
}

/* ================================================================== */
/* virtio-blk: a second device type -- block read/write over a backing  */
/* RAM disk, using the standard header/data/status descriptor chain.    */
/* ================================================================== */
#define VBLK_BASE 0x0A000200UL
#define VBLK_SIZE 0x0200UL
#define VBLK_SECTORS 16
#define VBLK_SECSZ   512

static uint8_t vblk_disk[VBLK_SECTORS * VBLK_SECSZ];
static uint32_t b_status, b_page_size = 4096, b_queue_num, b_queue_pfn;
static uint16_t b_last_avail;
static uint32_t b_int_status;
static int b_irq;

int vblk_contains(uint64_t ipa) {
	return ipa >= VBLK_BASE && ipa < VBLK_BASE + VBLK_SIZE;
}

void vblk_reset(void) {
	b_status = 0; b_page_size = 4096; b_queue_num = 0; b_queue_pfn = 0;
	b_last_avail = 0; b_int_status = 0; b_irq = 0;
	for (unsigned i = 0; i < sizeof(vblk_disk); i++)
		vblk_disk[i] = 0;
}

int vblk_take_irq(void) {
	if (b_irq) { b_irq = 0; return 1; }
	return 0;
}

/* Read the first `n` bytes of sector 0 into `out` (hypervisor god's-eye view). */
void vblk_peek(char *out, int n) {
	for (int i = 0; i < n; i++)
		out[i] = (char)vblk_disk[i];
}

/* Pre-load `n` bytes into sector 0 (hypervisor seeds the disk). */
void vblk_poke(const char *in, int n) {
	for (int i = 0; i < n; i++)
		vblk_disk[i] = (uint8_t)in[i];
}

static void vblk_process(void) {
	if (!b_queue_pfn || !b_queue_num)
		return;
	uint64_t base = (uint64_t)b_queue_pfn * b_page_size;
	uint64_t avail = base + (uint64_t)b_queue_num * 16;
	uint64_t used = (avail + 6 + 2 * (uint64_t)b_queue_num + 4095) & ~4095ULL;
	uint16_t avail_idx = *(volatile uint16_t *)(avail + 2);

	while (b_last_avail != avail_idx) {
		uint16_t head = *(volatile uint16_t *)(avail + 4 +
		                 2 * (b_last_avail % b_queue_num));
		/* desc[head] = request header (type, _, sector) */
		uint64_t hd = base + (uint64_t)head * 16;
		uint64_t haddr = *(volatile uint64_t *)(hd + 0);
		uint16_t hnext = *(volatile uint16_t *)(hd + 14);
		uint32_t type = *(volatile uint32_t *)(haddr + 0);
		uint64_t sector = *(volatile uint64_t *)(haddr + 8);
		/* desc[hnext] = data buffer */
		uint64_t dd = base + (uint64_t)hnext * 16;
		uint64_t daddr = *(volatile uint64_t *)(dd + 0);
		uint32_t dlen = *(volatile uint32_t *)(dd + 8);
		uint16_t dnext = *(volatile uint16_t *)(dd + 14);
		/* desc[dnext] = status byte */
		uint64_t sd = base + (uint64_t)dnext * 16;
		uint64_t saddr = *(volatile uint64_t *)(sd + 0);

		uint8_t st = 0; /* OK */
		uint64_t off = sector * VBLK_SECSZ;
		if (off + dlen <= sizeof(vblk_disk)) {
			if (type == 1) { /* VIRTIO_BLK_T_OUT (write) */
				for (uint32_t i = 0; i < dlen; i++)
					vblk_disk[off + i] = *(volatile uint8_t *)(daddr + i);
			} else {         /* VIRTIO_BLK_T_IN (read) */
				for (uint32_t i = 0; i < dlen; i++)
					*(volatile uint8_t *)(daddr + i) = vblk_disk[off + i];
			}
		} else {
			st = 1; /* IOERR */
		}
		*(volatile uint8_t *)saddr = st;

		uint16_t uidx = *(volatile uint16_t *)(used + 2);
		uint64_t ue = used + 4 + 8 * (uidx % b_queue_num);
		*(volatile uint32_t *)(ue + 0) = head;
		*(volatile uint32_t *)(ue + 4) = dlen;
		*(volatile uint16_t *)(used + 2) = uidx + 1;
		b_last_avail++;
		b_int_status |= 1;
		b_irq = 1;
	}
}

void vblk_mmio(vcpu_t *v, uint64_t ipa) {
	uint64_t iss = ESR_ISS(v->exit_esr);
	if (!((iss >> 24) & 1)) { v->pc += 4; return; }
	int wnr = (iss >> 6) & 1;
	int srt = (iss >> 16) & 0x1F;
	uint64_t off = ipa - VBLK_BASE;
	if (wnr) {
		uint32_t val = (srt == 31) ? 0 : (uint32_t)v->x[srt];
		switch (off) {
		case 0x028: b_page_size = val; break;
		case 0x038: b_queue_num = val; break;
		case 0x040: b_queue_pfn = val; break;
		case 0x070: b_status = val; break;
		case 0x064: b_int_status &= ~val; break;
		case 0x050: vblk_process(); break;
		default: break;
		}
	} else {
		uint32_t val = 0;
		switch (off) {
		case 0x000: val = 0x74726976; break;
		case 0x004: val = 1; break;
		case 0x008: val = 2; break;                 /* DeviceID: block */
		case 0x00c: val = 0x554d4551; break;
		case 0x010: val = 0; break;
		case 0x034: val = 8; break;
		case 0x040: val = b_queue_pfn; break;
		case 0x060: val = b_int_status; break;
		case 0x070: val = b_status; break;
		case 0x100: val = VBLK_SECTORS; break;       /* config: capacity (lo) */
		case 0x104: val = 0; break;                  /* capacity (hi) */
		default: val = 0; break;
		}
		if (srt != 31)
			v->x[srt] = val;
	}
	v->pc += 4;
}
