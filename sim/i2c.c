/*
 * vsim on-board I2C controller (docs/platform.md, "I2C") and two of its
 * devices: an LM75-style temperature sensor (register set of the NXP LM75B
 * datasheet) and the board's fan/hardware monitor. The TPM is in tpm.c.
 */
#include <string.h>

#include "i2c.h"
#include "tpm.h"

void
i2c_init(struct i2c *c, struct pmc *p)
{
	memset(c, 0, sizeof *c);
	c->pmc = p;
	i2c_reset(c);
}

void
i2c_reset(struct i2c *c)
{
	c->ntx = c->nrx = c->rxpos = 0;
	c->status = 0;
	c->busy_until = 0;
	c->hw_ptr = 0;
	c->lm_ptr = 0;
	c->lm_conf = 0;
	c->lm_tos = 80 << 8;		/* 80 C, 9-bit value in D15..D7 */
	c->lm_thyst = 75 << 8;
}

/* ---- the fan / hardware monitor (0x2F): byte registers, auto-increment */

static uint8_t
hw_get(struct i2c *c, uint8_t r)
{
	struct pmc *p = c->pmc;
	int64_t t;

	switch (r) {
	case 0x00: return 0x56;
	case 0x01: return 0x01;
	case 0x02: return (uint8_t)p->fan_auto;
	case 0x03: return PMC_FANS;
	case 0x04: {
		uint8_t f = 0;
		for (int i = 0; i < PMC_FANS; i++)
			f |= (uint8_t)(pmc_fan_failed(p, i) << i);
		return f;
	}
	case 0x05: t = pmc_die_mc(p) / 1000; return (uint8_t)(int8_t)(t > 127 ? 127 : t);
	case 0x06: t = pmc_board_mc(p) / 1000; return (uint8_t)(int8_t)(t > 127 ? 127 : t);
	case 0x10: case 0x11: return (uint8_t)p->duty[r - 0x10];
	case 0x20: case 0x22: return (uint8_t)(pmc_fan_rpm(p, (r - 0x20) / 2) >> 8);
	case 0x21: case 0x23: return (uint8_t)pmc_fan_rpm(p, (r - 0x21) / 2);
	case 0x30: return (uint8_t)p->curve_t1;
	case 0x31: return (uint8_t)p->curve_t2;
	case 0x32: return (uint8_t)p->curve_pwm1;
	default: return 0;
	}
}

static void
hw_set(struct i2c *c, uint8_t r, uint8_t v)
{
	struct pmc *p = c->pmc;

	switch (r) {
	case 0x02: p->fan_auto = v & 1; break;
	case 0x10: case 0x11: if (!p->fan_auto) pmc_set_duty(p, r - 0x10, v); break;
	case 0x30: p->curve_t1 = v; break;
	case 0x31: p->curve_t2 = v; break;
	case 0x32: p->curve_pwm1 = v; break;
	default: break;
	}
}

/* ---- LM75 (0x48): pointer 0 temp, 1 conf, 2 Thyst, 3 Tos; MSB first */

static uint16_t
lm_temp(struct i2c *c)
{
	/* 11-bit two's complement in D15..D5, 0.125 C per LSB */
	int64_t eighths = pmc_board_mc(c->pmc) / 125;
	return (uint16_t)((uint16_t)(int16_t)eighths << 5);
}

static void
lm_read(struct i2c *c, uint8_t *d, unsigned n)
{
	uint16_t v;

	for (unsigned i = 0; i < n; i++) {
		switch (c->lm_ptr & 3) {
		case 0: v = lm_temp(c); break;
		case 1: d[i] = c->lm_conf; continue;
		case 2: v = c->lm_thyst; break;
		default: v = c->lm_tos; break;
		}
		d[i] = (i & 1) ? (uint8_t)v : (uint8_t)(v >> 8);
	}
}

static void
lm_write(struct i2c *c, const uint8_t *d, unsigned n)
{
	if (!n)
		return;
	c->lm_ptr = d[0] & 3;
	if (n == 1)
		return;
	switch (c->lm_ptr) {
	case 1: c->lm_conf = d[1] & 0x1F; break;
	case 2: if (n >= 3) c->lm_thyst = (uint16_t)((d[1] << 8 | d[2]) & 0xFF80); break;
	case 3: if (n >= 3) c->lm_tos = (uint16_t)((d[1] << 8 | d[2]) & 0xFF80); break;
	default: break;		/* the temperature is read-only */
	}
}

/* ---- the controller */

static int
xfer(struct i2c *c, unsigned addr, unsigned nw, unsigned nr)
{
	if (addr != TPM_I2C_ADDR && addr != HWMON_ADDR && addr != LM75_ADDR)
		return -1;			/* address NACK */
	if (nw) {
		switch (addr) {
		case TPM_I2C_ADDR: tpm_i2c_write(c->tx, nw); break;
		case LM75_ADDR: lm_write(c, c->tx, nw); break;
		default:
			c->hw_ptr = c->tx[0];
			for (unsigned i = 1; i < nw; i++)
				hw_set(c, c->hw_ptr++, c->tx[i]);
			break;
		}
	}
	if (nr) {
		switch (addr) {
		case TPM_I2C_ADDR: tpm_i2c_read(c->rx, nr); break;
		case LM75_ADDR: lm_read(c, c->rx, nr); break;
		default:
			for (unsigned i = 0; i < nr; i++)
				c->rx[i] = hw_get(c, c->hw_ptr++);
			break;
		}
	}
	return 0;
}

int
i2c_reg(struct i2c *c, int we, unsigned off, unsigned n, uint8_t *data, uint64_t now)
{
	uint64_t x = 0;
	int busy = now < c->busy_until;

	if ((n != 4 && n != 8) || (off & 7))
		return -1;
	if (we) {
		memcpy(&x, data, n);
		switch (off) {
		case 0x00: {			/* CMD */
			unsigned addr = x & 0x7F, nw = (x >> 8) & 0xFF, nr = (x >> 16) & 0xFF;
			if (!(x >> 31 & 1) || busy)
				break;
			if (nw > c->ntx)
				nw = c->ntx;
			if (nr > I2C_FIFO)
				nr = I2C_FIFO;
			c->nrx = c->rxpos = 0;
			if (xfer(c, addr, nw, nr)) {
				c->status = 2 | 4;		/* done, NACK */
			} else {
				c->nrx = nr;
				c->status = 2 | (uint64_t)nw << 8;
			}
			c->ntx = 0;
			c->busy_until = now + (1 + nw + nr) * I2C_BYTE_CYCLES;
			break;
		}
		case 0x10:
			if (c->ntx < I2C_FIFO)
				c->tx[c->ntx++] = (uint8_t)x;
			break;
		case 0x20:
			c->ntx = c->nrx = c->rxpos = 0;
			c->status = 0;
			break;
		default:
			break;
		}
		return 0;
	}
	switch (off) {
	case 0x08:
		x = busy ? 1 : c->status;
		break;
	case 0x18:
		x = (!busy && c->rxpos < c->nrx) ? c->rx[c->rxpos++] : 0;
		break;
	case 0x28:
		x = c->ntx | (uint64_t)(c->nrx - c->rxpos) << 8;
		break;
	default:
		x = 0;
		break;
	}
	memset(data, 0, n);
	memcpy(data, &x, n);
	return 0;
}
