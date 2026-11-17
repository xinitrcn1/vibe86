/*
 * vsim: the voltage regulators on the SVID-like bus; vr.h explains the
 * model, docs/package.md the protocol.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vr.h"

enum { NVR = 2 };
enum { CMD_SETVID_FAST = 0x01, CMD_SETVID_SLOW = 0x02, CMD_SETPS = 0x03, CMD_SETVR = 0x04,
       CMD_GETREG = 0x10 };
enum { ACK = 1, ACK_BUSY = 2, NAK = 3 };
enum { F_NONE, F_NAK, F_STUCK, F_PARITY };

#define POWER_ON_CLOCKS	64	/* boot ramp before power-good */
#define SLOW_CLOCKS	8	/* clocks per VID step: SetVID-slow */
#define FAST_CLOCKS	2	/* SetVID-fast */

struct vr {
	unsigned vid, target, min, max, boot;
	int set;		/* the die has set a VID */
	int alert;		/* SVT pulled low until status is read */
	unsigned slew, slew_clocks;
	unsigned ps;
	int fault;
	unsigned amps, temp;
};

struct vrs {
	struct vr vr[NVR];
	uint64_t power_clock;	/* clocks since power-on */
	int powered;
	/* the bus decoder */
	int prev_svc, prev_line;
	int in_frame;
	unsigned n;		/* the bit slot (the die's numbering) */
	uint32_t shift;
	uint32_t resp;		/* response bits, MSB first */
	unsigned resp_len;
	int drive;		/* our SVD pull: 1 released */
	/* statistics */
	uint64_t frames, naks, parity_errors;
};

static const char *rail_name[NVR] = { "VDDCR_CPU", "VDDCR_SOC" };

struct vrs *
vr_new(void)
{
	struct vrs *m = calloc(1, sizeof *m);
	if (!m)
		return NULL;
	/* V = 0.25 + 0.005 * VID */
	m->vr[0] = (struct vr){ .boot = 140, .min = 70, .max = 190, .amps = 0, .temp = 40 };   /* 0.95 V; 0.60-1.20 V */
	m->vr[1] = (struct vr){ .boot = 130, .min = 100, .max = 160, .amps = 0, .temp = 40 };  /* 0.90 V; 0.75-1.05 V */
	m->prev_svc = m->prev_line = 1;
	m->drive = 1;
	return m;
}

void
vr_free(struct vrs *m)
{
	free(m);
}

int
vr_fault(struct vrs *m, const char *spec)
{
	unsigned r;
	char kind[16];
	if (sscanf(spec, "vr%u:%15s", &r, kind) != 2 || r >= NVR)
		return -1;
	if (!strcmp(kind, "nak"))
		m->vr[r].fault = F_NAK;
	else if (!strcmp(kind, "stuck"))
		m->vr[r].fault = F_STUCK;
	else if (!strcmp(kind, "parity"))
		m->vr[r].fault = F_PARITY;
	else
		return -1;
	return 0;
}

void
vr_power_on(struct vrs *m)
{
	m->powered = 1;
	m->power_clock = 0;
	for (unsigned r = 0; r < NVR; r++) {
		struct vr *v = &m->vr[r];
		v->vid = v->target = v->boot;
		v->set = 0;
		v->alert = 0;
		v->ps = 0;
	}
}

int
vr_power_good(const struct vrs *m)
{
	return m->powered && m->power_clock >= POWER_ON_CLOCKS;
}

int
vr_cpu_rail_set(const struct vrs *m)
{
	const struct vr *v = &m->vr[0];
	return v->set && v->vid == v->target;
}

unsigned
vr_mv(const struct vrs *m, unsigned r)
{
	return 250 + 5 * m->vr[r].vid;
}

static int
parity(uint32_t x)
{
	return __builtin_parity(x);
}

/* a frame from the die: address[3:0] command[4:0] payload[7:0] parity */
static void
frame(struct vrs *m, uint32_t f)
{
	unsigned addr = f >> 14 & 0xF, cmd = f >> 9 & 0x1F, pay = f >> 1 & 0xFF;
	unsigned ack = ACK, data = 0;
	int rd = 0;

	m->frames++;
	m->resp = 0;
	m->resp_len = 0;
	if (addr >= NVR)
		return;			/* nobody answers: the line stays released (11) */
	struct vr *v = &m->vr[addr];
	if (parity(f & 0x3FFFF)) {
		m->parity_errors++;
		ack = NAK;
	} else if (v->fault == F_NAK) {
		ack = NAK;
	} else {
		switch (cmd) {
		case CMD_SETVID_FAST:
		case CMD_SETVID_SLOW:
			if (pay < v->min || pay > v->max) {
				ack = NAK;
				break;
			}
			v->set = 1;
			v->target = pay;
			v->slew_clocks = cmd == CMD_SETVID_FAST ? FAST_CLOCKS : SLOW_CLOCKS;
			v->slew = 0;
			ack = v->vid == pay && v->fault != F_STUCK ? ACK : ACK_BUSY;
			if (v->fault == F_STUCK)
				v->target = v->vid;	/* it never moves, never settles */
			if (ack == ACK_BUSY && v->fault == F_STUCK)
				v->set = 0;
			break;
		case CMD_SETPS:
			v->ps = pay & 3;
			break;
		case CMD_SETVR:
			break;
		case CMD_GETREG:
			rd = 1;
			switch (pay) {
			case 0x00:	/* status: [0] settled, [1] alert */
				data = (v->vid == v->target) | v->alert << 1;
				v->alert = 0;
				break;
			case 0x01: data = v->amps; break;
			case 0x02: data = v->temp; break;
			case 0x03: data = v->vid; break;
			default: ack = NAK; rd = 0; break;
			}
			break;
		default:
			ack = NAK;
			break;
		}
	}
	if (ack == NAK)
		m->naks++;
	m->resp = ack;
	m->resp_len = 2;
	if (rd && ack != NAK) {
		unsigned p = parity(data) ^ (v->fault == F_PARITY);
		m->resp = m->resp << 9 | data << 1 | p;
		m->resp_len = 11;
	}
}

void
vr_cycle(struct vrs *m, int svc, int svd_die, int *svd, int *svt)
{
	int line = svd_die && m->drive;

	if (m->powered)
		m->power_clock++;
	/* the rails slew toward their targets */
	int alert = 0;
	for (unsigned r = 0; r < NVR; r++) {
		struct vr *v = &m->vr[r];
		if (v->vid != v->target && ++v->slew >= v->slew_clocks) {
			v->slew = 0;
			v->vid += v->vid < v->target ? 1 : -1;
			if (v->vid == v->target)
				v->alert = 1;	/* settled: pull SVT */
		}
		alert |= v->alert;
	}

	if (svc && m->prev_svc && m->prev_line && !line) {
		/* start: SVD fell while SVC was high */
		m->in_frame = 1;
		m->n = 0;
		m->shift = 0;
		m->resp_len = 0;
	} else if (svc && m->prev_svc && !m->prev_line && line) {
		m->in_frame = 0;	/* stop */
		m->drive = 1;
	} else if (m->in_frame && svc && !m->prev_svc) {
		/* SVC rose: slot n's bit is on the line */
		if (m->n <= 17) {
			m->shift = m->shift << 1 | (unsigned)line;
			if (m->n == 17)
				frame(m, m->shift);
		}
		m->n++;
	} else if (m->in_frame && !svc && m->prev_svc) {
		/* SVC fell: slot n begins; ours from 19 on */
		m->drive = 1;
		if (m->n >= 19 && m->n - 19 < m->resp_len)
			m->drive = m->resp >> (m->resp_len - 1 - (m->n - 19)) & 1;
	}
	m->prev_svc = svc;
	m->prev_line = svd_die && m->drive;
	*svd = m->drive;
	*svt = !alert;
}

void
vr_stats(const struct vrs *m, char *buf, unsigned len)
{
	snprintf(buf, len, "VRs: %s %u mV, %s %u mV; %llu frames, %llu NAKs, %llu parity errors",
	    rail_name[0], vr_mv(m, 0), rail_name[1], vr_mv(m, 1),
	    (unsigned long long)m->frames, (unsigned long long)m->naks,
	    (unsigned long long)m->parity_errors);
}
