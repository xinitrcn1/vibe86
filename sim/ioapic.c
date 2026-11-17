/* vsim: the I/O APIC (ioapic.h) */
#include <string.h>

#include "ioapic.h"

#define R_MASK		(1ull << 16)
#define R_TRIG		(1ull << 15)	/* 1 level */
#define R_RIRR		(1ull << 14)	/* Remote IRR (read-only) */
#define R_POL		(1ull << 13)	/* 1 active low */
#define R_DS		(1ull << 12)	/* delivery status (read-only) */
#define R_RW		(0xFF000000000000FFull | 0x7FFull | R_POL | R_TRIG | R_MASK)

void
ioapic_reset(struct ioapic *a, uint8_t id)
{
	a->sel = 0;
	a->id = id;
	for (unsigned i = 0; i < IOAPIC_PINS; i++)
		a->redir[i] = R_MASK;		/* masked after reset (82093AA 3.2.4) */
}

static int
asserted(const struct ioapic *a, unsigned pin)
{
	int lv = (a->level >> pin) & 1;
	return (a->redir[pin] & R_POL) ? !lv : lv;
}

/* the interrupt message of an entry (Intel SDM Vol. 3A 12.11.1-2) */
static void
send(struct ioapic *a, unsigned pin)
{
	uint64_t r = a->redir[pin];
	uint64_t addr = 0xFEE00000ull | ((r >> 56) & 0xFF) << 12 | ((r >> 11) & 1) << 2;
	uint32_t data = (uint32_t)(r & 0xFF) | (uint32_t)((r >> 8) & 7) << 8 |
	    1u << 14 | (uint32_t)((r & R_TRIG) ? 1 : 0) << 15;
	if (r & R_TRIG)
		a->redir[pin] |= R_RIRR;	/* until the EOI */
	a->sent++;
	if (a->msi)
		a->msi(a->ctx, addr, data);
}

/* a level-triggered entry fires while asserted and not waiting for its EOI */
static void
level_check(struct ioapic *a, unsigned pin)
{
	uint64_t r = a->redir[pin];
	if ((r & R_TRIG) && !(r & (R_MASK | R_RIRR)) && asserted(a, pin))
		send(a, pin);
}

void
ioapic_set(struct ioapic *a, unsigned pin, int level)
{
	if (pin >= IOAPIC_PINS)
		return;
	int was = asserted(a, pin);
	a->level = (a->level & ~(1u << pin)) | (uint32_t)(level != 0) << pin;
	int now = asserted(a, pin);
	if (a->redir[pin] & R_TRIG)
		level_check(a, pin);
	else if (now && !was && !(a->redir[pin] & R_MASK))
		send(a, pin);			/* edge-triggered: the rising edge */
}

static uint32_t
reg_read(struct ioapic *a, uint8_t r)
{
	if (r == 0x00)
		return (uint32_t)a->id << 24;
	if (r == 0x01)
		return 0x00170020;		/* 24 entries (max 17h), version 20h */
	if (r == 0x02)
		return (uint32_t)a->id << 24;	/* arbitration ID */
	if (r >= 0x10 && r < 0x10 + 2 * IOAPIC_PINS) {
		uint64_t v = a->redir[(r - 0x10) / 2];
		return (r & 1) ? (uint32_t)(v >> 32) : (uint32_t)v;
	}
	return 0;
}

static void
reg_write(struct ioapic *a, uint8_t r, uint32_t v)
{
	if (r == 0x00) {
		a->id = (v >> 24) & 0xF;
		return;
	}
	if (r >= 0x10 && r < 0x10 + 2 * IOAPIC_PINS) {
		unsigned pin = (r - 0x10) / 2;
		uint64_t old = a->redir[pin], nv;
		if (r & 1)
			nv = (old & 0xFFFFFFFFull) | (uint64_t)(v & 0xFF000000u) << 32;
		else
			nv = (old & ~0xFFFFFFFFull) | (v & (R_RW & 0xFFFFFFFFull)) |
			    (old & (R_RIRR | R_DS));
		if (!(nv & R_TRIG))
			nv &= ~R_RIRR;
		a->redir[pin] = nv;
		level_check(a, pin);		/* unmasking an asserted level input */
	}
}

int
ioapic_mmio(struct ioapic *a, int we, unsigned off, unsigned n, uint8_t *data)
{
	uint32_t v = 0;
	if (off == 0x00 && (n == 1 || n == 4)) {
		if (we)
			a->sel = data[0];
		else {
			memset(data, 0, n);
			data[0] = a->sel;
		}
		return 0;
	}
	if (n != 4)
		return -1;
	if (we)
		memcpy(&v, data, 4);		/* little-endian host */
	if (off == 0x10) {
		if (we)
			reg_write(a, a->sel, v);
		else {
			v = reg_read(a, a->sel);
			memcpy(data, &v, 4);
		}
		return 0;
	}
	if (off == 0x40) {			/* EOI: the vector in [7:0] */
		if (we)
			for (unsigned i = 0; i < IOAPIC_PINS; i++)
				if ((a->redir[i] & 0xFF) == (v & 0xFF) && (a->redir[i] & R_RIRR)) {
					a->redir[i] &= ~R_RIRR;
					level_check(a, i);
				}
		else
			memset(data, 0, 4);
		return 0;
	}
	if (!we)
		memset(data, 0, 4);
	return 0;
}
