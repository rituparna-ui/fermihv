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
/* Per-VM virtio-console state so each tenant drives its own device. host_off
 * translates a guest physical address to the host physical address of that
 * VM's RAM block (0 for an identity-mapped single VM). */
#define VIRTIO_NVMS 2
struct vcon {
	uint32_t status, page_size, queue_num, queue_pfn, int_status;
	uint16_t last_avail;
	int irq;
	uint64_t host_off;
};
static struct vcon vc[VIRTIO_NVMS];

int virtio_contains(uint64_t ipa) {
	return ipa >= VIRTIO_BASE && ipa < VIRTIO_BASE + VIRTIO_SIZE;
}

void virtio_reset(void) {
	for (int i = 0; i < VIRTIO_NVMS; i++) {
		struct vcon *c = &vc[i];
		c->status = c->queue_num = c->queue_pfn = c->int_status = 0;
		c->last_avail = 0;
		c->irq = 0;
		c->page_size = 4096;
		c->host_off = 0;
	}
}

/* Set VM `vm`'s guest->host address offset (host_base - guest_base). */
void virtio_set_offset(int vm, uint64_t off) {
	vc[vm].host_off = off;
}

/* Returns 1 (once) if VM `vm`'s device raised a completion interrupt. */
int virtio_take_irq(int vm) {
	if (vc[vm].irq) {
		vc[vm].irq = 0;
		return 1;
	}
	return 0;
}

/* Process VM `vm`'s TX queue: emit each newly-available buffer to the console.
 * Guest physical addresses are translated to host via host_off. */
static void virtio_process_tx(int vm) {
	struct vcon *c = &vc[vm];
	uint64_t o = c->host_off;
	if (!c->queue_pfn || !c->queue_num)
		return;
	uint64_t base = (uint64_t)c->queue_pfn * c->page_size + o;
	uint64_t avail = base + (uint64_t)c->queue_num * 16;
	uint16_t avail_idx = *(volatile uint16_t *)(avail + 2);
	uint64_t used = (base + (uint64_t)c->queue_num * 16 + 6 +
	                 2 * (uint64_t)c->queue_num + 4095) & ~4095ULL;
	int consumed = 0;

	while (c->last_avail != avail_idx) {
		uint16_t slot = c->last_avail % c->queue_num;
		uint16_t didx = *(volatile uint16_t *)(avail + 4 + 2 * slot);
		uint64_t desc = base + (uint64_t)didx * 16;
		uint64_t addr = *(volatile uint64_t *)(desc + 0) + o;
		uint32_t len = *(volatile uint32_t *)(desc + 8);
		for (uint32_t i = 0; i < len; i++)
			uart_putc(*(volatile char *)(addr + i));
		uint16_t uidx = *(volatile uint16_t *)(used + 2);
		uint64_t ue = used + 4 + 8 * (uidx % c->queue_num);
		*(volatile uint32_t *)(ue + 0) = didx;
		*(volatile uint32_t *)(ue + 4) = 0;
		*(volatile uint16_t *)(used + 2) = uidx + 1;
		c->last_avail++;
		consumed = 1;
	}
	if (consumed) {
		c->int_status |= 1;
		c->irq = 1;
	}
}

void virtio_mmio(int vm, vcpu_t *v, uint64_t ipa) {
	struct vcon *c = &vc[vm];
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
		case 0x028: c->page_size = val; break;
		case 0x038: c->queue_num = val; break;
		case 0x040: c->queue_pfn = val; break;
		case 0x070: c->status = val; break;
		case 0x064: c->int_status &= ~val; break;
		case 0x050: virtio_process_tx(vm); break;
		default: break;
		}
	} else {
		uint32_t val = 0;
		switch (off) {
		case 0x000: val = 0x74726976; break;
		case 0x004: val = 1; break;
		case 0x008: val = 3; break;
		case 0x00c: val = 0x554d4551; break;
		case 0x010: val = 0; break;
		case 0x034: val = 8; break;
		case 0x040: val = c->queue_pfn; break;
		case 0x060: val = c->int_status; break;
		case 0x070: val = c->status; break;
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

/* Per-VM virtio-blk: each tenant gets its own device state and backing disk,
 * and the device translates the VM's guest addresses to its host RAM block. */
#define VBLK_NVMS 2
struct vblk {
	uint32_t status, page_size, queue_num, queue_pfn, int_status;
	uint16_t last_avail;
	int irq;
	uint64_t host_off;
	uint8_t disk[VBLK_SECTORS * VBLK_SECSZ];
};
static struct vblk vb[VBLK_NVMS];

int vblk_contains(uint64_t ipa) {
	return ipa >= VBLK_BASE && ipa < VBLK_BASE + VBLK_SIZE;
}

void vblk_reset(void) {
	for (int k = 0; k < VBLK_NVMS; k++) {
		struct vblk *b = &vb[k];
		b->status = b->queue_num = b->queue_pfn = b->int_status = 0;
		b->last_avail = 0; b->irq = 0; b->page_size = 4096; b->host_off = 0;
		for (unsigned i = 0; i < sizeof(b->disk); i++)
			b->disk[i] = 0;
	}
}

/* Set VM `vm`'s guest->host address offset (host_base - guest_base). */
void vblk_set_offset(int vm, uint64_t off) {
	vb[vm].host_off = off;
}

int vblk_take_irq(int vm) {
	if (vb[vm].irq) { vb[vm].irq = 0; return 1; }
	return 0;
}

/* Read the first `n` bytes of VM `vm`'s sector 0 (hypervisor god's-eye view). */
void vblk_peek(int vm, char *out, int n) {
	for (int i = 0; i < n; i++)
		out[i] = (char)vb[vm].disk[i];
}

/* Pre-load `n` bytes into VM `vm`'s sector 0 (hypervisor seeds the disk). */
void vblk_poke(int vm, const char *in, int n) {
	for (int i = 0; i < n; i++)
		vb[vm].disk[i] = (uint8_t)in[i];
}

static void vblk_process(int vm) {
	struct vblk *b = &vb[vm];
	uint64_t o = b->host_off;
	if (!b->queue_pfn || !b->queue_num)
		return;
	uint64_t base = (uint64_t)b->queue_pfn * b->page_size + o;
	uint64_t avail = base + (uint64_t)b->queue_num * 16;
	uint64_t used = (base + (uint64_t)b->queue_num * 16 + 6 +
	                 2 * (uint64_t)b->queue_num + 4095) & ~4095ULL;
	uint16_t avail_idx = *(volatile uint16_t *)(avail + 2);

	while (b->last_avail != avail_idx) {
		uint16_t head = *(volatile uint16_t *)(avail + 4 +
		                 2 * (b->last_avail % b->queue_num));
		uint64_t hd = base + (uint64_t)head * 16;
		uint64_t haddr = *(volatile uint64_t *)(hd + 0) + o;
		uint16_t hnext = *(volatile uint16_t *)(hd + 14);
		uint32_t type = *(volatile uint32_t *)(haddr + 0);
		uint64_t sector = *(volatile uint64_t *)(haddr + 8);
		uint64_t dd = base + (uint64_t)hnext * 16;
		uint64_t daddr = *(volatile uint64_t *)(dd + 0) + o;
		uint32_t dlen = *(volatile uint32_t *)(dd + 8);
		uint16_t dnext = *(volatile uint16_t *)(dd + 14);
		uint64_t sd = base + (uint64_t)dnext * 16;
		uint64_t saddr = *(volatile uint64_t *)(sd + 0) + o;

		uint8_t st = 0;
		uint64_t doff = sector * VBLK_SECSZ;
		if (doff + dlen <= sizeof(b->disk)) {
			if (type == 1) { /* write */
				for (uint32_t i = 0; i < dlen; i++)
					b->disk[doff + i] = *(volatile uint8_t *)(daddr + i);
			} else {         /* read */
				for (uint32_t i = 0; i < dlen; i++)
					*(volatile uint8_t *)(daddr + i) = b->disk[doff + i];
			}
		} else {
			st = 1;
		}
		*(volatile uint8_t *)saddr = st;

		uint16_t uidx = *(volatile uint16_t *)(used + 2);
		uint64_t ue = used + 4 + 8 * (uidx % b->queue_num);
		*(volatile uint32_t *)(ue + 0) = head;
		*(volatile uint32_t *)(ue + 4) = dlen;
		*(volatile uint16_t *)(used + 2) = uidx + 1;
		b->last_avail++;
		b->int_status |= 1;
		b->irq = 1;
	}
}

void vblk_mmio(int vm, vcpu_t *v, uint64_t ipa) {
	struct vblk *b = &vb[vm];
	uint64_t iss = ESR_ISS(v->exit_esr);
	if (!((iss >> 24) & 1)) { v->pc += 4; return; }
	int wnr = (iss >> 6) & 1;
	int srt = (iss >> 16) & 0x1F;
	uint64_t off = ipa - VBLK_BASE;
	if (wnr) {
		uint32_t val = (srt == 31) ? 0 : (uint32_t)v->x[srt];
		switch (off) {
		case 0x028: b->page_size = val; break;
		case 0x038: b->queue_num = val; break;
		case 0x040: b->queue_pfn = val; break;
		case 0x070: b->status = val; break;
		case 0x064: b->int_status &= ~val; break;
		case 0x050: vblk_process(vm); break;
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
		case 0x040: val = b->queue_pfn; break;
		case 0x060: val = b->int_status; break;
		case 0x070: val = b->status; break;
		case 0x100: val = VBLK_SECTORS; break;
		case 0x104: val = 0; break;
		default: val = 0; break;
		}
		if (srt != 31)
			v->x[srt] = val;
	}
	v->pc += 4;
}
