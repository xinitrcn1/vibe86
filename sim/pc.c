/*
 * vsim: legacy PC devices (pc.h).
 *
 * The register interfaces follow the parts' datasheets as PC firmware and
 * operating systems use them:
 *   8254 PIT: Intel 82C54 CHMOS Programmable Interval Timer, 231244-006;
 *   8259A PIC: Intel 8259A Programmable Interrupt Controller, 231468-003;
 *   RTC/CMOS: Motorola MC146818A Real-Time Clock Plus RAM; the PC CMOS
 *     layout (memory size bytes 15h-18h, 30h-31h, 34h-35h, 5Bh-5Dh) as
 *     QEMU's hw/rtc/mc146818rtc.c and SeaBIOS read them;
 *   ACPI PM1 and PM timer: ACPI Specification 6.5, 4.8 "ACPI Hardware
 *     Model" (fixed hardware registers);
 *   fw_cfg: QEMU, docs/specs/fw_cfg.rst ("QEMU Firmware Configuration
 *     (fw_cfg) Device"), including its DMA interface.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "pc.h"

static uint8_t
bcd(unsigned v)
{
	return (uint8_t)(((v / 10) << 4) | (v % 10));
}

static unsigned
unbcd(uint8_t v)
{
	return (v >> 4) * 10 + (v & 15);
}

/* ---------------------------------------------------------------- 8259 */

static void
pic_update_level(struct pic8259 *c, unsigned line, int level)
{
	uint8_t m = (uint8_t)(1u << line);
	if (c->elcr & m) {
		if (level)
			c->irr |= m;
		else
			c->irr &= (uint8_t)~m;
	} else if (level && !(c->level & m))
		c->irr |= m;		/* edge-triggered: a rising edge */
	if (level)
		c->level |= m;
	else
		c->level &= (uint8_t)~m;
}

/* the highest-priority request that would interrupt the current service */
static int
pic_pending(const struct pic8259 *c, int slave_line)
{
	uint8_t req = (uint8_t)(c->irr & ~c->imr);
	if (slave_line >= 0)
		req |= (uint8_t)(1u << slave_line);
	for (int i = 0; i < 8; i++) {
		if (c->isr & (1u << i))
			return -1;	/* an equal or higher priority is in service */
		if (req & (1u << i))
			return i;
	}
	return -1;
}

int
pc_pic_intr(const struct pc *p)
{
	int s = pic_pending(&p->pic[1], -1);
	return pic_pending(&p->pic[0], s >= 0 && !(p->pic[0].imr & 4) ? 2 : -1) >= 0;
}

static uint8_t
pic_ack(struct pic8259 *c, int line)
{
	uint8_t m = (uint8_t)(1u << line);
	if (!(c->elcr & m))
		c->irr &= (uint8_t)~m;
	if (!c->autoeoi)
		c->isr |= m;
	return (uint8_t)(c->base + line);
}

uint8_t
pc_pic_inta(struct pc *p)
{
	int s = pic_pending(&p->pic[1], -1);
	int m = pic_pending(&p->pic[0], s >= 0 && !(p->pic[0].imr & 4) ? 2 : -1);
	if (m < 0)
		return (uint8_t)(p->pic[0].base + 7);	/* spurious IRQ 7 */
	if (m == 2 && s >= 0) {
		if (!p->pic[0].autoeoi)
			p->pic[0].isr |= 4;
		return pic_ack(&p->pic[1], s);
	}
	return pic_ack(&p->pic[0], m);
}

static void
pic_write(struct pic8259 *c, unsigned a, uint8_t v)
{
	if (a == 0) {
		if (v & 0x10) {			/* ICW1 */
			c->init = 2;
			c->icw4 = v & 1;
			c->icw = v;
			c->imr = 0;
			c->isr = 0;
			c->autoeoi = 0;
			c->rr = 0;
			return;
		}
		if (v & 0x08) {			/* OCW3 */
			if (v & 2)
				c->rr = v & 1;
			return;
		}
		/* OCW2: EOI */
		switch (v >> 5) {
		case 1: case 5:			/* non-specific EOI */
			for (int i = 0; i < 8; i++)
				if (c->isr & (1u << i)) {
					c->isr &= (uint8_t)~(1u << i);
					break;
				}
			break;
		case 3: case 7:			/* specific EOI */
			c->isr &= (uint8_t)~(1u << (v & 7));
			break;
		default:
			break;
		}
		return;
	}
	switch (c->init) {
	case 2:					/* ICW2 */
		c->base = v & 0xF8;
		c->init = (c->icw & 2) ? (c->icw4 ? 4 : 0) : 3;
		return;
	case 3:					/* ICW3 */
		c->init = c->icw4 ? 4 : 0;
		return;
	case 4:					/* ICW4 */
		c->autoeoi = (v >> 1) & 1;
		c->init = 0;
		return;
	default:
		c->imr = v;			/* OCW1 */
	}
}

static uint8_t
pic_read(struct pic8259 *c, unsigned a)
{
	if (a == 1)
		return c->imr;
	return c->rr ? c->isr : c->irr;
}

void
pc_irq(struct pc *p, unsigned irq, int level)
{
	if (irq < 8)
		pic_update_level(&p->pic[0], irq, level);
	else if (irq < 16)
		pic_update_level(&p->pic[1], irq - 8, level);
}

/* ---------------------------------------------------------------- 8254 */

static uint32_t
pit_reload(const struct pit_ch *c)
{
	return c->reload ? c->reload : 0x10000;
}

static void
pit_clock(struct pc *p, unsigned i)
{
	struct pit_ch *c = &p->pit[i];
	uint8_t old = c->out;
	if (c->null) {
		c->count = (uint16_t)pit_reload(c);
		c->null = 0;
		c->out = c->mode == 0 ? 0 : 1;
		return;
	}
	if (!c->gate && (c->mode == 0 || c->mode == 2 || c->mode == 3 || c->mode == 4))
		return;
	switch (c->mode) {
	case 0:
	case 4:
		c->count--;
		if (c->count == 0)
			c->out = c->mode == 0 ? 1 : 0;
		else if (c->mode == 4)
			c->out = 1;
		break;
	case 2:
	case 6:
		if (c->count <= 1) {
			c->out = 0;
			c->count = (uint16_t)pit_reload(c);
		} else {
			c->out = 1;
			c->count--;
		}
		break;
	case 3:
	case 7: {
		uint32_t n = c->count ? c->count : 0x10000;
		if (n <= 2) {
			c->out = !c->out;
			c->count = (uint16_t)pit_reload(c);
		} else
			c->count = (uint16_t)(n - 2);
		break;
	}
	default:
		c->count--;
	}
	if (i == 0 && c->out != old)
		pc_irq(p, 0, c->out);
}

static void
pit_write(struct pc *p, unsigned a, uint8_t v)
{
	if (a == 3) {
		unsigned i = v >> 6;
		if (i == 3) {			/* read-back */
			for (unsigned k = 0; k < 3; k++) {
				if (!(v & (2u << k)))
					continue;
				struct pit_ch *c = &p->pit[k];
				if (!(v & 0x20) && !c->latched) {
					c->latch = c->count;
					c->latched = 2;
					c->latch_lo = 1;
				}
				if (!(v & 0x10)) {
					c->status = (uint8_t)((c->out << 7) | (c->null << 6) |
					    (c->rw << 4) | (c->mode << 1));
					c->status_latched = 1;
				}
			}
			return;
		}
		struct pit_ch *c = &p->pit[i];
		unsigned rw = (v >> 4) & 3;
		if (rw == 0) {			/* counter latch */
			if (!c->latched) {
				c->latch = c->count;
				c->latched = c->rw == 3 ? 2 : 1;
				c->latch_lo = 1;
			}
			return;
		}
		c->rw = (uint8_t)rw;
		c->mode = (v >> 1) & 7;
		if (c->mode > 5)
			c->mode &= 3;
		c->wr_lo = 1;
		c->access_lo = 1;
		c->out = c->mode == 0 ? 0 : 1;
		return;
	}
	struct pit_ch *c = &p->pit[a];
	switch (c->rw) {
	case 1:
		c->reload = v;
		c->null = 1;
		break;
	case 2:
		c->reload = (uint16_t)(v << 8);
		c->null = 1;
		break;
	default:
		if (c->wr_lo) {
			c->reload = (uint16_t)((c->reload & 0xFF00) | v);
			c->wr_lo = 0;
		} else {
			c->reload = (uint16_t)((c->reload & 0x00FF) | (v << 8));
			c->wr_lo = 1;
			c->null = 1;
		}
	}
}

static uint8_t
pit_read(struct pc *p, unsigned a)
{
	if (a == 3)
		return 0xFF;
	struct pit_ch *c = &p->pit[a];
	if (c->status_latched) {
		c->status_latched = 0;
		return c->status;
	}
	uint16_t v = c->latched ? c->latch : c->count;
	uint8_t r;
	switch (c->rw) {
	case 1: r = (uint8_t)v; if (c->latched) c->latched = 0; break;
	case 2: r = (uint8_t)(v >> 8); if (c->latched) c->latched = 0; break;
	default:
		if (c->access_lo) {
			r = (uint8_t)v;
			c->access_lo = 0;
			if (c->latched)
				c->latched = 1;
		} else {
			r = (uint8_t)(v >> 8);
			c->access_lo = 1;
			if (c->latched)
				c->latched = 0;
		}
	}
	return r;
}

/* ---------------------------------------------------------------- RTC */

static void
rtc_set_time(struct pc *p, time_t t)
{
	struct tm tm;
	gmtime_r(&t, &tm);
	p->cmos[0x00] = bcd((unsigned)tm.tm_sec);
	p->cmos[0x02] = bcd((unsigned)tm.tm_min);
	p->cmos[0x04] = bcd((unsigned)tm.tm_hour);
	p->cmos[0x06] = bcd((unsigned)tm.tm_wday + 1);
	p->cmos[0x07] = bcd((unsigned)tm.tm_mday);
	p->cmos[0x08] = bcd((unsigned)tm.tm_mon + 1);
	p->cmos[0x09] = bcd((unsigned)tm.tm_year % 100);
	p->cmos[0x32] = bcd(19 + (unsigned)tm.tm_year / 100);	/* century */
}

static void
rtc_second(struct pc *p)
{
	struct tm tm;
	memset(&tm, 0, sizeof tm);
	tm.tm_sec = (int)unbcd(p->cmos[0x00]);
	tm.tm_min = (int)unbcd(p->cmos[0x02]);
	tm.tm_hour = (int)unbcd(p->cmos[0x04]);
	tm.tm_mday = (int)unbcd(p->cmos[0x07]);
	tm.tm_mon = (int)unbcd(p->cmos[0x08]) - 1;
	tm.tm_year = (int)(unbcd(p->cmos[0x32]) * 100 + unbcd(p->cmos[0x09])) - 1900;
	time_t t = timegm(&tm) + 1;
	rtc_set_time(p, t);
	if (p->cmos[0x0B] & 0x10) {		/* update-ended interrupt */
		p->cmos[0x0C] |= 0x90;
		pc_irq(p, 8, 1);
		pc_irq(p, 8, 0);
	}
}

static void
cmos_mem_size(struct pc *p)
{
	uint64_t ext = p->ram_size > (1ull << 20) ? (p->ram_size - (1ull << 20)) >> 10 : 0;
	if (ext > 0xFFFF)
		ext = 0xFFFF;
	p->cmos[0x15] = 0x80;			/* 640 KiB base memory */
	p->cmos[0x16] = 0x02;
	p->cmos[0x17] = p->cmos[0x30] = (uint8_t)ext;
	p->cmos[0x18] = p->cmos[0x31] = (uint8_t)(ext >> 8);
	uint64_t hi = p->ram_size > (16ull << 20) ? (p->ram_size - (16ull << 20)) >> 16 : 0;
	if (hi > 0xFFFF)
		hi = 0xFFFF;
	p->cmos[0x34] = (uint8_t)hi;
	p->cmos[0x35] = (uint8_t)(hi >> 8);
	/* DRAM above 4 GiB in 64 KiB units (5Bh-5Dh, as QEMU and SeaBIOS use) */
	uint64_t above = p->ram_high >> 16;
	p->cmos[0x5B] = (uint8_t)above;
	p->cmos[0x5C] = (uint8_t)(above >> 8);
	p->cmos[0x5D] = (uint8_t)(above >> 16);
	p->cmos[0x5F] = (uint8_t)(p->ncpus - 1);
}

/* DRAM above 4 GiB (set after pc_init: the CMOS size bytes follow) */
void
pc_ram_high(struct pc *p, uint64_t bytes)
{
	p->ram_high = bytes;
	cmos_mem_size(p);
}

/* ---------------------------------------------------------------- fw_cfg */

enum { FW_SIGNATURE = 0x00, FW_ID = 0x01, FW_RAM_SIZE = 0x03, FW_NB_CPUS = 0x05,
       FW_MAX_CPUS = 0x0F, FW_FILE_DIR = 0x19, FW_FILE_FIRST = 0x20 };

static void
fw_item(struct pc *p, uint16_t sel, const uint8_t **data, uint32_t *size)
{
	static uint8_t tmp[8];
	*data = NULL;
	*size = 0;
	switch (sel) {
	case FW_SIGNATURE:
		*data = (const uint8_t *)"QEMU";
		*size = 4;
		return;
	case FW_ID:
		memset(tmp, 0, 4);
		tmp[0] = 3;			/* traditional interface + DMA */
		*data = tmp;
		*size = 4;
		return;
	case FW_RAM_SIZE:
		for (int i = 0; i < 8; i++)
			tmp[i] = (uint8_t)(p->ram_size >> (8 * i));
		*data = tmp;
		*size = 8;
		return;
	case FW_NB_CPUS:
	case FW_MAX_CPUS:
		tmp[0] = (uint8_t)p->ncpus;
		tmp[1] = 0;
		*data = tmp;
		*size = 2;
		return;
	case FW_FILE_DIR:
		*data = p->dir;
		*size = p->dirsize;
		return;
	default:
		if (sel >= FW_FILE_FIRST && sel < FW_FILE_FIRST + p->nfiles) {
			*data = p->files[sel - FW_FILE_FIRST].data;
			*size = p->files[sel - FW_FILE_FIRST].size;
		}
	}
}

static void
fw_rebuild_dir(struct pc *p)
{
	free(p->dir);
	p->dirsize = 4 + 64 * p->nfiles;
	p->dir = calloc(1, p->dirsize);
	uint32_t n = p->nfiles;
	p->dir[0] = (uint8_t)(n >> 24);
	p->dir[1] = (uint8_t)(n >> 16);
	p->dir[2] = (uint8_t)(n >> 8);
	p->dir[3] = (uint8_t)n;
	for (unsigned i = 0; i < p->nfiles; i++) {
		uint8_t *e = p->dir + 4 + 64 * i;
		uint32_t sz = p->files[i].size;
		e[0] = (uint8_t)(sz >> 24);
		e[1] = (uint8_t)(sz >> 16);
		e[2] = (uint8_t)(sz >> 8);
		e[3] = (uint8_t)sz;
		e[4] = (uint8_t)(p->files[i].sel >> 8);
		e[5] = (uint8_t)p->files[i].sel;
		memcpy(e + 8, p->files[i].name, 56);
	}
}

int
pc_fwcfg_add(struct pc *p, const char *name, const void *data, uint32_t size)
{
	if (p->nfiles == FWCFG_MAXFILES || strlen(name) > 55)
		return -1;
	struct fwcfg_file *f = &p->files[p->nfiles];
	memset(f, 0, sizeof *f);
	strcpy(f->name, name);
	f->sel = (uint16_t)(FW_FILE_FIRST + p->nfiles);
	f->data = malloc(size ? size : 1);
	memcpy(f->data, data, size);
	f->size = size;
	p->nfiles++;
	fw_rebuild_dir(p);
	return 0;
}

static uint32_t
be32(const uint8_t *b)
{
	return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
}

/* a DMA request: control, length, address (big-endian) at p->fw_dma */
static void
fw_dma_run(struct pc *p)
{
	uint8_t d[16];
	if (!p->mem || p->mem(p->memctx, 0, p->fw_dma, 16, d))
		return;
	uint32_t ctl = be32(d), len = be32(d + 4);
	uint64_t addr = (uint64_t)be32(d + 8) << 32 | be32(d + 12);
	int err = 0;
	if (ctl & 8) {
		p->fw_sel = (uint16_t)(ctl >> 16);
		p->fw_off = 0;
	}
	const uint8_t *data;
	uint32_t size;
	fw_item(p, p->fw_sel, &data, &size);
	if (ctl & 2) {				/* read */
		uint8_t buf[256];
		while (len) {
			unsigned n = len > sizeof buf ? sizeof buf : len;
			for (unsigned i = 0; i < n; i++)
				buf[i] = (data && p->fw_off + i < size) ? data[p->fw_off + i] : 0;
			if (p->mem(p->memctx, 1, addr, n, buf)) {
				err = 1;
				break;
			}
			p->fw_off += n;
			addr += n;
			len -= n;
		}
	} else if (ctl & 4) {			/* skip */
		p->fw_off += len;
	} else if (ctl & 16)
		err = 1;			/* writes aren't supported */
	uint8_t done[4] = { 0, 0, 0, (uint8_t)err };
	p->mem(p->memctx, 1, p->fw_dma, 4, done);
}

/* ---------------------------------------------------------------- the devices */

void
pc_init(struct pc *p, uint64_t ram_size, unsigned ncpus)
{
	memset(p, 0, sizeof *p);
	p->ram_size = ram_size;
	p->ncpus = ncpus ? ncpus : 1;
	fw_rebuild_dir(p);
	rtc_set_time(p, time(NULL));
	pc_reset(p);
}

void
pc_reset(struct pc *p)
{
	memset(p->pit, 0, sizeof p->pit);
	for (int i = 0; i < 3; i++) {
		p->pit[i].gate = i != 2;
		p->pit[i].rw = 3;
		p->pit[i].access_lo = 1;
		p->pit[i].wr_lo = 1;
	}
	memset(p->pic, 0, sizeof p->pic);
	p->pic[0].imr = p->pic[1].imr = 0xFF;
	p->cmos[0x0A] = 0x26;
	p->cmos[0x0B] = 0x02;
	p->cmos[0x0C] = 0;
	p->cmos[0x0D] = 0x80;
	cmos_mem_size(p);
	p->sysb = 0;
	p->a20 = 2;
	p->pm1_sts = p->pm1_en = p->pm1_cnt = 0;
	p->fw_sel = 0;
	p->fw_off = 0;
	p->power_off = 0;
}

void
pc_tick(struct pc *p)
{
	/* fw_cfg DMA: the device's DMA engine works in its own time (the guest
	   polls the control word); its memory accesses go over the I/O link */
	if (p->fw_dma_pending) {
		p->fw_dma_pending = 0;
		fw_dma_run(p);
	}
	p->pit_acc += PIT_HZ;
	if (p->pit_acc >= CORE_HZ) {
		p->pit_acc -= CORE_HZ;
		p->pit[2].gate = p->sysb & 1;
		for (unsigned i = 0; i < 3; i++)
			pit_clock(p, i);
	}
	p->pmtmr_acc += PMTMR_HZ;
	if (p->pmtmr_acc >= CORE_HZ) {
		p->pmtmr_acc -= CORE_HZ;
		p->pmtmr = (p->pmtmr + 1) & 0xFFFFFF;
		if (p->pmtmr == 0)
			p->pm1_sts |= 1;	/* TMR_STS */
	}
	/* the RTC: periodic interrupts at 32768 >> (rate - 1) Hz, and seconds */
	unsigned rate = p->cmos[0x0A] & 15;
	if (rate) {
		uint64_t hz = rate <= 2 ? (32768u >> (rate + 6)) : (32768u >> (rate - 1));
		p->rtc_acc += hz;
		if (p->rtc_acc >= CORE_HZ) {
			p->rtc_acc -= CORE_HZ;
			p->cmos[0x0C] |= 0x40;
			if (p->cmos[0x0B] & 0x40) {
				p->cmos[0x0C] |= 0x80;
				pc_irq(p, 8, 1);
				pc_irq(p, 8, 0);
			}
		}
	}
	if (++p->rtc_div >= CORE_HZ) {
		p->rtc_div = 0;
		if (!(p->cmos[0x0B] & 0x80))
			rtc_second(p);
	}
}

int
pc_io(struct pc *p, int we, uint16_t port, unsigned n, uint8_t *d)
{
	if (port >= PORT_PIT && port < PORT_PIT + 4) {
		for (unsigned i = 0; i < n; i++) {
			if (we)
				pit_write(p, (port - PORT_PIT + i) & 3, d[i]);
			else
				d[i] = pit_read(p, (port - PORT_PIT + i) & 3);
		}
		return 1;
	}
	if ((port & ~1u) == PORT_PIC1 || (port & ~1u) == PORT_PIC2) {
		struct pic8259 *c = &p->pic[(port & ~1u) == PORT_PIC2];
		if (we)
			pic_write(c, port & 1, d[0]);
		else
			d[0] = pic_read(c, port & 1);
		return 1;
	}
	if (port == PORT_ELCR || port == PORT_ELCR + 1) {
		for (unsigned i = 0; i < n && port + i <= PORT_ELCR + 1; i++) {
			struct pic8259 *c = &p->pic[port + i - PORT_ELCR];
			if (we)
				c->elcr = d[i] & (port + i == PORT_ELCR ? 0xF8 : 0xDE);
			else
				d[i] = c->elcr;
		}
		return 1;
	}
	if (port == PORT_RTC || port == PORT_RTC + 1) {
		if (port == PORT_RTC) {
			if (we) {
				p->cmos_idx = d[0] & 0x7F;
				p->nmi_mask = d[0] >> 7;
			} else
				d[0] = 0xFF;
			return 1;
		}
		uint8_t i = p->cmos_idx;
		if (we) {
			if (i == 0x0C || i == 0x0D)
				return 1;		/* read-only */
			p->cmos[i] = d[0];
		} else {
			d[0] = p->cmos[i];
			if (i == 0x0C) {
				p->cmos[0x0C] = 0;	/* reading C clears the flags */
				pc_irq(p, 8, 0);
			}
		}
		return 1;
	}
	if (port == PORT_SYSB) {
		if (we)
			p->sysb = d[0] & 0x0F;
		else {
			static uint64_t refresh;
			refresh++;
			d[0] = (uint8_t)((p->sysb & 0x0F) | ((refresh >> 3) & 1) << 4 |
			    (p->pit[2].out << 5));
		}
		return 1;
	}
	if (port == PORT_A20) {
		if (we)
			p->a20 = d[0] & 2;
		else
			d[0] = p->a20;
		return 1;
	}
	if (port == PORT_DEBUGCON) {
		if (we) {
			if (p->debugcon)
				p->debugcon(p->dbgctx, d[0]);
		} else
			d[0] = 0xE9;		/* the debug console's presence */
		return 1;
	}
	if (port == PORT_FWCFG && n == 2) {
		if (we) {
			p->fw_sel = (uint16_t)(d[0] | d[1] << 8);
			p->fw_off = 0;
		} else
			memset(d, 0, 2);
		return 1;
	}
	if (port == PORT_FWCFG + 1) {
		const uint8_t *data;
		uint32_t size;
		fw_item(p, p->fw_sel, &data, &size);
		for (unsigned i = 0; i < n; i++) {
			if (!we)
				d[i] = (data && p->fw_off < size) ? data[p->fw_off] : 0;
			p->fw_off++;
		}
		return 1;
	}
	if (port >= PORT_FWCFG_DMA && port < PORT_FWCFG_DMA + 8) {
		static const uint8_t sig[8] = { 'Q', 'E', 'M', 'U', ' ', 'C', 'F', 'G' };
		unsigned o = port - PORT_FWCFG_DMA;
		if (!we) {
			for (unsigned i = 0; i < n; i++)
				d[i] = o + i < 8 ? sig[o + i] : 0;
			return 1;
		}
		/* big-endian address: the high half at 514h, the low at 518h (which starts it) */
		uint64_t v = 0;
		for (unsigned i = 0; i < n; i++)
			v = v << 8 | d[i];
		if (o == 0 && n == 4)
			p->fw_dma = (p->fw_dma & 0xFFFFFFFFull) | v << 32;
		else if (o == 4 && n == 4) {
			p->fw_dma = (p->fw_dma & ~0xFFFFFFFFull) | v;
			p->fw_dma_pending = 1;	/* the transfer runs in the next tick */
		} else if (o == 0 && n == 8) {
			p->fw_dma = v;
			p->fw_dma_pending = 1;
		}
		return 1;
	}
	if (port >= PORT_PM && port < PORT_PM + 12) {
		unsigned o = port - PORT_PM;
		uint32_t v = 0;
		if (we)
			for (unsigned i = 0; i < n; i++)
				v |= (uint32_t)d[i] << (8 * i);
		switch (o) {
		case 0:				/* PM1a_STS (write 1 to clear), PM1a_EN */
			if (we) {
				p->pm1_sts &= (uint16_t)~v;
				if (n == 4)
					p->pm1_en = (uint16_t)(v >> 16);
			} else {
				uint32_t r = p->pm1_sts | (uint32_t)p->pm1_en << 16;
				for (unsigned i = 0; i < n; i++)
					d[i] = (uint8_t)(r >> (8 * i));
			}
			return 1;
		case 2:
			if (we)
				p->pm1_en = (uint16_t)v;
			else
				for (unsigned i = 0; i < n; i++)
					d[i] = (uint8_t)(p->pm1_en >> (8 * i));
			return 1;
		case 4:				/* PM1a_CNT: SCI_EN, SLP_TYP, SLP_EN */
			if (we) {
				p->pm1_cnt = (uint16_t)(v & 0x1C01);
				if ((v & 0x2000) && ((v >> 10) & 7) == 0)
					p->power_off = 1;	/* S5 (the DSDT's \_S5 is 0) */
			} else
				for (unsigned i = 0; i < n; i++)
					d[i] = (uint8_t)(p->pm1_cnt >> (8 * i));
			return 1;
		case 8:				/* PM timer, 24 bits */
			if (!we)
				for (unsigned i = 0; i < n; i++)
					d[i] = (uint8_t)(p->pmtmr >> (8 * i));
			return 1;
		default:
			if (!we)
				memset(d, 0, n);
			return 1;
		}
	}
	return 0;
}
