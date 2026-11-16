/* vsim power-management controller (docs/platform.md, "PMC") */
#include <string.h>

#include "pmc.h"

#define FAN_MAX_RPM	12000.0
#define FAN_TAU		200000.0	/* cycles: fan spin-up/down time constant */
#define THERM_TAU	2000000.0	/* cycles: die temperature time constant */
#define FAIL_RPM	300.0
#define FAIL_CYCLES	1000000ull

void
pmc_init(struct pmc *p)
{
	int (*msi)(void *, uint64_t, uint64_t) = p->msi;
	void *ctx = p->msictx;

	memset(p, 0, sizeof *p);
	p->msi = msi;
	p->msictx = ctx;
	p->amb = 25000.0;
	p->temp = p->amb;
	pmc_reset(p);
	p->started = 0;		/* the board's first reset is still the power-on */
}

void
pmc_reset(struct pmc *p)
{
	/* the first reset after power-on (vsim starts with one) keeps RC_POWERON */
	p->reset_cause = p->started ? p->next_cause : RC_POWERON;
	p->started = 1;
	p->next_cause = RC_SOFT;
	p->status = 0;
	p->trip_hot = 85000;
	p->trip_crit = 105000;
	p->hot_armed = 1;
	p->msi_addr = p->msi_data = 0;
	p->suspended = 0;
	p->wake_after = p->susp_left = 0;
	p->wdog_timeout = p->wdog_left = 0;
	p->temp_offset = 0;
	p->fan_auto = 1;
	p->curve_t1 = 40;
	p->curve_t2 = 80;
	p->curve_pwm1 = 64;
	p->req_off = p->req_reset = 0;
	p->critical = 0;
}

int64_t
pmc_die_mc(const struct pmc *p)
{
	return (int64_t)p->temp + p->temp_offset;
}

int64_t
pmc_board_mc(const struct pmc *p)
{
	return (int64_t)(p->amb + (p->temp - p->amb) * 0.4) + p->temp_offset;
}

unsigned
pmc_fan_rpm(const struct pmc *p, int i)
{
	return (unsigned)(p->rpm[i] + 0.5);
}

int
pmc_fan_failed(const struct pmc *p, int i)
{
	return p->fail_timer[i] >= FAIL_CYCLES;
}

void
pmc_set_duty(struct pmc *p, int i, unsigned duty)
{
	if (i >= 0 && i < PMC_FANS)
		p->duty[i] = duty > 255 ? 255 : duty;
}

static void
event(struct pmc *p, uint64_t bit)
{
	p->status |= bit;
	if (p->msi_addr && p->msi)
		p->msi(p->msictx, p->msi_addr, p->msi_data);
}

void
pmc_button(struct pmc *p)
{
	event(p, PS_BUTTON);
	if (p->suspended) {
		p->suspended = 0;
		event(p, PS_WAKE);
	}
}

void
pmc_tick(struct pmc *p, uint64_t accesses)
{
	double act, cool = 0, target, frac;
	int64_t die;

	/* fans */
	for (int i = 0; i < PMC_FANS; i++) {
		if (p->fan_auto) {
			int64_t t = pmc_die_mc(p) / 1000;
			unsigned d;
			if (t <= (int64_t)p->curve_t1)
				d = p->curve_pwm1;
			else if (t >= (int64_t)p->curve_t2 || p->curve_t2 <= p->curve_t1)
				d = 255;
			else
				d = p->curve_pwm1 + (unsigned)((255 - p->curve_pwm1) *
				    (t - p->curve_t1) / (p->curve_t2 - p->curve_t1));
			p->duty[i] = d;
		}
		double tgt = (p->fail_inject >> i & 1) ? 0 : FAN_MAX_RPM * p->duty[i] / 255.0;
		p->rpm[i] += (tgt - p->rpm[i]) * (PMC_TICK / FAN_TAU);
		if (p->duty[i] >= 32 && p->rpm[i] < FAIL_RPM)
			p->fail_timer[i] += PMC_TICK;
		else
			p->fail_timer[i] = 0;
		cool += p->rpm[i] / FAN_MAX_RPM;
	}
	frac = cool / PMC_FANS;

	/* die temperature: activity (bus accesses per cycle) heats, fans cool */
	act = (double)(accesses - p->last_access) / PMC_TICK;
	p->last_access = accesses;
	if (act > 1)
		act = 1;
	target = p->amb + (20000.0 + 70000.0 * act) / (1.0 + 1.5 * frac);
	p->temp += (target - p->temp) * (PMC_TICK / THERM_TAU);

	die = pmc_die_mc(p);
	if (p->hot_armed && die >= p->trip_hot) {
		p->hot_armed = 0;
		event(p, PS_HOT);
	} else if (!p->hot_armed && die < p->trip_hot - 5000)
		p->hot_armed = 1;
	if (die >= p->trip_crit)
		p->critical = 1;

	/* suspend, watchdog */
	if (p->suspended && p->wake_after) {
		if (p->susp_left <= PMC_TICK) {
			p->suspended = 0;
			event(p, PS_WAKE);
		} else
			p->susp_left -= PMC_TICK;
	}
	if (p->wdog_timeout && !p->suspended) {
		if (p->wdog_left <= PMC_TICK) {
			p->next_cause = RC_WATCHDOG;
			p->req_reset = 1;
			p->wdog_left = p->wdog_timeout;
		} else
			p->wdog_left -= PMC_TICK;
	}
}

int
pmc_reg(struct pmc *p, int we, unsigned off, unsigned n, uint8_t *data)
{
	uint64_t x = 0;

	if ((n != 4 && n != 8) || (off & 7))
		return -1;
	if (we) {
		memcpy(&x, data, n);
		switch (off) {
		case 0x08:		/* CMD */
			switch (x) {
			case 1: p->req_off = 1; break;
			case 2: p->next_cause = RC_SOFT; p->req_reset = 1; break;
			case 3:
				p->suspended = 1;
				p->susp_left = p->wake_after;
				break;
			case 4: pmc_button(p); break;
			default: break;
			}
			break;
		case 0x18: p->status &= ~(x & (PS_BUTTON | PS_HOT | PS_WAKE)); break;
		case 0x28: p->trip_hot = (int64_t)x; break;
		case 0x30: p->trip_crit = (int64_t)x; break;
		case 0x38: p->msi_addr = x; break;
		case 0x40: p->msi_data = x; break;
		case 0x68: p->temp_offset = (int64_t)x; break;
		case 0x70: p->wdog_timeout = p->wdog_left = x; break;
		case 0x78: p->wdog_left = p->wdog_timeout; break;
		case 0x80: p->fail_inject = (unsigned)x & ((1u << PMC_FANS) - 1); break;
		case 0x88: p->wake_after = x; break;
		default: break;
		}
		return 0;
	}
	switch (off) {
	case 0x00: x = 0x30434d50; break;		/* "PMC0" */
	case 0x10: x = p->reset_cause; break;
	case 0x18: x = p->status | (p->suspended ? PS_SUSPENDED : 0); break;
	case 0x20: x = (uint64_t)pmc_die_mc(p); break;
	case 0x28: x = (uint64_t)p->trip_hot; break;
	case 0x30: x = (uint64_t)p->trip_crit; break;
	case 0x38: x = p->msi_addr; break;
	case 0x40: x = p->msi_data; break;
	case 0x48: {			/* VCORE: droops under load */
		double t = (p->temp - p->amb) / 90000.0;
		x = (uint64_t)(850 - 50 * (t > 1 ? 1 : t < 0 ? 0 : t));
		break;
	}
	case 0x50: x = 1100; break;	/* VDDQ, DDR5 */
	case 0x58: x = 12000; break;
	case 0x60: x = 3300; break;
	case 0x68: x = (uint64_t)p->temp_offset; break;
	case 0x70: x = p->wdog_timeout; break;
	case 0x80: x = p->fail_inject; break;
	case 0x88: x = p->wake_after; break;
	case 0x90: x = (uint64_t)pmc_board_mc(p); break;
	default: x = 0; break;
	}
	memset(data, 0, n);
	memcpy(data, &x, n);
	return 0;
}
