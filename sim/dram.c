/*
 * vsim DDR5 memory subsystem (see dram.h and docs/platform.md).
 *
 * References:
 *  - JEDEC JESD79-5, DDR5 SDRAM standard: bank groups, timing parameters,
 *    refresh (tREFI/tRFC), BL16, the device organizations used below.
 *  - JEDEC JESD400-5, DDR5 Serial Presence Detect (SPD) contents: the SPD
 *    bytes produced by spd_build() and the CRC over bytes 0-509.
 *  - P. Rosenfeld, E. Cooper-Balis, B. Jacob, "DRAMSim2: A Cycle Accurate
 *    Memory System Simulator", IEEE Computer Architecture Letters 10(1),
 *    2011, doi:10.1109/L-CA.2011.4: the per-bank state machine approach
 *    (open row, earliest-issue times per command) that timing() follows,
 *    much simplified for one outstanding request.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dram.h"
#include "spd.h"

/* ---------------------------------------------------------------- DIMMs */

#define geoms spd_geoms
#define speeds spd_speeds
#define trfc1_ns spd_trfc1_ns

void
dram_default_cfg(struct dimm_cfg cfg[DRAM_SLOTS])
{
	memset(cfg, 0, sizeof(struct dimm_cfg) * DRAM_SLOTS);
	cfg[0].present = 1;
	cfg[0].size_gib = 8;
	cfg[0].ecc = 1;
	cfg[0].speed = 4800;
}

int
dram_parse_dimm(const char *s, struct dimm_cfg cfg[DRAM_SLOTS])
{
	char buf[64], *p, *tok;
	struct dimm_cfg c = { 1, 8, 1, 4800, 0 };
	long slot;

	if (strlen(s) >= sizeof buf)
		return -1;
	strcpy(buf, s);
	p = buf;
	tok = strsep(&p, ":");
	slot = strtol(tok, &tok, 10);
	if (*tok || slot < 0 || slot >= DRAM_SLOTS || !p)
		return -1;
	tok = strsep(&p, ":");
	c.size_gib = (unsigned)strtoul(tok, &tok, 10);
	if (*tok == 'G' || *tok == 'g')
		tok++;
	if (*tok)
		return -1;
	{
		int ok = 0;
		for (size_t i = 0; i < sizeof geoms / sizeof geoms[0]; i++)
			ok |= geoms[i].size_gib == c.size_gib;
		if (!ok)
			return -1;
	}
	while (p) {
		tok = strsep(&p, ":");
		if (!strcmp(tok, "noecc"))
			c.ecc = 0;
		else if (!strcmp(tok, "ecc"))
			c.ecc = 1;
		else if (!strcmp(tok, "4800") || !strcmp(tok, "5600"))
			c.speed = (unsigned)atoi(tok);
		else if (!strcmp(tok, "badspd"))
			c.bad_spd = 1;
		else
			return -1;
	}
	cfg[slot] = c;
	return 0;
}

/* ---------------------------------------------------------------- state */

#define PAGE_SHIFT	16
#define PAGE_SIZE	(1u << PAGE_SHIFT)
#define WORDS		(PAGE_SIZE / 8)

struct page {
	uint8_t data[PAGE_SIZE];
	uint8_t valid[WORDS / 8];	/* ECC word seeded (written or scrubbed) */
	uint32_t gen;			/* scrub generation applied */
};

#define MAXBANKS	(2 * 2 * 8 * 4)	/* subchannels x ranks x BGs x banks */

struct bank {
	int64_t row;		/* open row, -1 closed */
	uint64_t act;		/* ps: last ACT */
	uint64_t ready;		/* ps: next command */
	uint64_t refepoch;	/* refresh interval it was opened in */
};

struct slot {
	struct dimm_cfg cfg;
	const struct geom *g;
	const struct speed *sp;
	uint64_t size;
	uint8_t spd[1024];
	/* controller configuration */
	uint64_t creg, base;
	int en, ecc_on;
	/* scrub */
	uint32_t sgen;
	uint64_t swm;		/* bytes scrubbed in generation sgen */
	int clean;		/* a scrub has completed */
	/* timing */
	struct bank bk[MAXBANKS];
	uint64_t bg_col[2][2][8];	/* last column command per subch/rank/BG */
	uint64_t rk_col[2][2];
};

struct inj {
	uint64_t pa;	/* 8-byte aligned */
	int type;	/* 1 CE, 2 UE; 0 free */
};

/* controller states */
enum { ST_RESET, ST_CKE, ST_MRS, ST_ZQ, ST_TRAINED, ST_ONLINE };

struct dram {
	struct slot s[DRAM_SLOTS];
	uint64_t seed;
	/* sparse storage: open addressing on (slot << 48 | page) */
	uint64_t *keys;
	struct page **pages;
	size_t cap, used;
	/* SMBus */
	uint64_t smb_status, smb_data;
	unsigned smb_busy;
	uint64_t smb_cmd;
	/* controller */
	int state, err, errcode;
	unsigned busy;
	int pend_cmd;
	uint64_t mrw, mr[256];
	uint64_t tck_ps, tim0, tim1;
	unsigned cl, trcd, trp, tras, trc, trfc, trefi, tccdl, tccds;
	/* ECC */
	int scrub_busy, scrub_done;
	uint64_t scrub_bytes, scrub_total;
	uint64_t ce, ue, last_addr, last_info;
	struct inj inj[8];
	void (*mce)(void *, int, uint64_t);
	void *mcectx;
	/* statistics */
	uint64_t st_hit, st_miss, st_conf, st_ref, st_reads, st_writes, st_lat;
};

static uint64_t
mix(uint64_t x)
{
	x += 0x9E3779B97F4A7C15ull;
	x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
	x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
	return x ^ (x >> 31);
}

struct dram *
dram_new(const struct dimm_cfg cfg[DRAM_SLOTS], uint64_t seed)
{
	struct dram *d = calloc(1, sizeof *d);
	if (!d)
		return NULL;
	d->seed = seed;
	for (int i = 0; i < DRAM_SLOTS; i++) {
		struct slot *s = &d->s[i];
		s->cfg = cfg[i];
		if (!cfg[i].present)
			continue;
		for (size_t k = 0; k < sizeof geoms / sizeof geoms[0]; k++)
			if (geoms[k].size_gib == cfg[i].size_gib)
				s->g = &geoms[k];
		s->sp = cfg[i].speed == 5600 ? &speeds[1] : &speeds[0];
		if (!s->g) {
			free(d);
			return NULL;
		}
		s->size = (uint64_t)s->g->size_gib << 30;
		spd_build(s->spd, s->g, s->sp, cfg[i].ecc);
		if (cfg[i].bad_spd)
			s->spd[510] ^= 0x5A;
	}
	d->cap = 1024;
	d->keys = calloc(d->cap, sizeof *d->keys);
	d->pages = calloc(d->cap, sizeof *d->pages);
	if (!d->keys || !d->pages) {
		dram_free(d);
		return NULL;
	}
	dram_reset(d);
	return d;
}

void
dram_free(struct dram *d)
{
	if (!d)
		return;
	for (size_t i = 0; i < d->cap; i++)
		free(d->pages ? d->pages[i] : NULL);
	free(d->keys);
	free(d->pages);
	free(d);
}

void
dram_reset(struct dram *d)
{
	d->smb_status = d->smb_data = 0;
	d->smb_busy = 0;
	d->state = ST_RESET;
	d->err = d->errcode = 0;
	d->busy = 0;
	d->pend_cmd = 0;
	d->mrw = 0;
	memset(d->mr, 0, sizeof d->mr);
	d->tck_ps = d->tim0 = d->tim1 = 0;
	d->scrub_busy = d->scrub_done = 0;
	d->scrub_bytes = d->scrub_total = 0;
	d->ce = d->ue = d->last_addr = d->last_info = 0;
	memset(d->inj, 0, sizeof d->inj);
	for (int i = 0; i < DRAM_SLOTS; i++) {
		struct slot *s = &d->s[i];
		s->creg = s->base = 0;
		s->en = s->ecc_on = 0;
		s->swm = 0;
		for (int b = 0; b < MAXBANKS; b++)
			s->bk[b] = (struct bank){ -1, 0, 0, 0 };
		memset(s->bg_col, 0, sizeof s->bg_col);
		memset(s->rk_col, 0, sizeof s->rk_col);
	}
}

/* ---------------------------------------------------------------- storage */

static size_t
slotfor(const struct dram *d, uint64_t key)
{
	return (size_t)(mix(key) & (d->cap - 1));
}

static struct page **
lookup(struct dram *d, uint64_t key)
{
	size_t i = slotfor(d, key);
	while (d->pages[i] && d->keys[i] != key)
		i = (i + 1) & (d->cap - 1);
	return &d->pages[i];
}

static void
grow(struct dram *d)
{
	size_t oc = d->cap;
	uint64_t *ok = d->keys;
	struct page **op = d->pages;

	d->cap *= 2;
	d->keys = calloc(d->cap, sizeof *d->keys);
	d->pages = calloc(d->cap, sizeof *d->pages);
	if (!d->keys || !d->pages) {
		perror("vsim: dram");
		exit(4);
	}
	for (size_t i = 0; i < oc; i++)
		if (op[i]) {
			size_t j = slotfor(d, ok[i]);
			while (d->pages[j])
				j = (j + 1) & (d->cap - 1);
			d->keys[j] = ok[i];
			d->pages[j] = op[i];
		}
	free(ok);
	free(op);
}

/* scrubbed (zero, ECC seeded) at this DIMM offset? */
static int
zeroed(const struct slot *s, uint64_t off)
{
	return s->clean || (s->sgen && off < s->swm);
}

static void
garbage(const struct dram *d, int si, uint64_t off, uint8_t *p, unsigned n)
{
	for (unsigned i = 0; i < n; i += 8) {
		uint64_t v = mix(d->seed ^ ((uint64_t)si << 56) ^ (off + i));
		memcpy(p + i, &v, 8);
	}
}

/* bring a page up to date with the scrub engine */
static void
fixup(struct slot *s, struct page *pg, uint64_t pstart)
{
	if (pg->gen < s->sgen && pstart < s->swm) {
		memset(pg->data, 0, PAGE_SIZE);
		memset(pg->valid, 0xFF, sizeof pg->valid);
		pg->gen = s->sgen;
	}
}

static struct page *
getpage(struct dram *d, int si, uint64_t off, int create)
{
	struct slot *s = &d->s[si];
	uint64_t pstart = off & ~(uint64_t)(PAGE_SIZE - 1);
	uint64_t key = (uint64_t)si << 48 | (off >> PAGE_SHIFT);
	struct page **pp = lookup(d, key);

	if (*pp) {
		fixup(s, *pp, pstart);
		return *pp;
	}
	if (!create)
		return NULL;
	if ((d->used + 1) * 10 > d->cap * 7) {
		grow(d);
		pp = lookup(d, key);
	}
	struct page *pg = malloc(sizeof *pg);
	if (!pg) {
		perror("vsim: dram");
		exit(4);
	}
	if (zeroed(s, pstart)) {
		memset(pg->data, 0, PAGE_SIZE);
		memset(pg->valid, 0xFF, sizeof pg->valid);
		pg->gen = (s->sgen && pstart < s->swm) ? s->sgen : 0;
	} else {
		garbage(d, si, pstart, pg->data, PAGE_SIZE);
		memset(pg->valid, 0, sizeof pg->valid);
		pg->gen = 0;
	}
	*pp = pg;
	{
		size_t i = (size_t)(pp - d->pages);
		d->keys[i] = key;
	}
	d->used++;
	return pg;
}

/* ---------------------------------------------------------------- timing */

static uint64_t
ceil_div(uint64_t a, uint64_t b)
{
	return (a + b - 1) / b;
}

/*
 * Latency of one access (one BL16 burst on one subchannel), in core cycles.
 * Address map inside a DIMM, low to high: 64-byte line, subchannel, bank
 * group, bank, column (4 KiB row per subchannel), rank, row.
 */
#define MC_FRONT_PS	12000	/* controller queue + PHY, each way */

static unsigned
timing(struct dram *d, struct slot *s, uint64_t off, int we, uint64_t now)
{
	const struct geom *g = s->g;
	uint64_t tck = d->tck_ps;
	uint64_t a = off >> 6;
	unsigned sc = a & 1;
	a >>= 1;
	unsigned bg = a & ((1u << g->bg_log2) - 1);
	a >>= g->bg_log2;
	unsigned bk = a & ((1u << g->bk_log2) - 1);
	a >>= g->bk_log2;
	a >>= 6;	/* 64 lines per 4 KiB row */
	unsigned rk = g->ranks == 2 ? (a & 1) : 0;
	if (g->ranks == 2)
		a >>= 1;
	int64_t row = (int64_t)a;
	struct bank *b = &s->bk[((sc * 2 + rk) * 8 + bg) * 4 + bk];

	uint64_t t0 = now * CORE_PS;
	uint64_t t = t0 + MC_FRONT_PS;

	/* all-bank refresh every tREFI per rank, lasting tRFC */
	uint64_t refi = (uint64_t)d->trefi * tck, rfc = (uint64_t)d->trfc * tck;
	uint64_t epoch = t / refi;
	if (t < epoch * refi + rfc) {
		t = epoch * refi + rfc;
		d->st_ref++;
	}
	if (b->row >= 0 && b->refepoch != epoch)
		b->row = -1;	/* refresh precharged the bank */
	if (t < b->ready)
		t = b->ready;

	uint64_t col;
	if (b->row == row) {
		col = t;
		d->st_hit++;
	} else {
		uint64_t act = t;
		if (b->row >= 0) {
			uint64_t pre = t > b->act + (uint64_t)d->tras * tck ? t
			    : b->act + (uint64_t)d->tras * tck;
			act = pre + (uint64_t)d->trp * tck;
			d->st_conf++;
		} else
			d->st_miss++;
		if (act < b->act + (uint64_t)d->trc * tck)
			act = b->act + (uint64_t)d->trc * tck;
		b->act = act;
		b->row = row;
		b->refepoch = epoch;
		col = act + (uint64_t)d->trcd * tck;
	}
	if (col < s->bg_col[sc][rk][bg] + (uint64_t)d->tccdl * tck)
		col = s->bg_col[sc][rk][bg] + (uint64_t)d->tccdl * tck;
	if (col < s->rk_col[sc][rk] + (uint64_t)d->tccds * tck)
		col = s->rk_col[sc][rk] + (uint64_t)d->tccds * tck;
	s->bg_col[sc][rk][bg] = col;
	s->rk_col[sc][rk] = col;
	b->ready = col + (uint64_t)d->tccdl * tck;

	/* data: CL (reads) or CWL = CL - 2 (writes), then BL16 = 8 clocks */
	uint64_t lat = we ? d->cl - 2 : d->cl;
	uint64_t done = col + (lat + 8) * tck + (we ? 0 : MC_FRONT_PS);
	unsigned cyc = (unsigned)ceil_div(done - t0, CORE_PS);
	d->st_lat += cyc;
	return cyc ? cyc : 1;
}

/* ---------------------------------------------------------------- ECC */

static void
ecc_event(struct dram *d, int si, uint64_t pa, int ue)
{
	if (ue)
		d->ue++;
	else
		d->ce++;
	d->last_addr = pa;
	d->last_info = (ue ? 2u : 1u) | (uint64_t)si << 8;
	if (d->mce)
		d->mce(d->mcectx, ue ? 2 : 1, pa);
}

void
dram_set_mce(struct dram *d, void (*fn)(void *, int, uint64_t), void *ctx)
{
	d->mce = fn;
	d->mcectx = ctx;
}

static int
inj_find(struct dram *d, uint64_t pa)
{
	for (int i = 0; i < 8; i++)
		if (d->inj[i].type && d->inj[i].pa == pa)
			return i;
	return -1;
}

/* ---------------------------------------------------------------- memory */

int
dram_mem(struct dram *d, int we, uint64_t pa, unsigned n, uint8_t *data,
    uint64_t now, unsigned *lat)
{
	*lat = 1;
	if (d->state != ST_ONLINE)
		return -1;
	int si = -1;
	for (int i = 0; i < DRAM_SLOTS; i++) {
		struct slot *s = &d->s[i];
		if (s->en && pa >= s->base && pa + n <= s->base + s->size) {
			si = i;
			break;
		}
	}
	if (si < 0)
		return -1;
	struct slot *s = &d->s[si];
	uint64_t off = pa - s->base;
	if ((off & (PAGE_SIZE - 1)) + n > PAGE_SIZE)
		return -1;	/* the core never crosses 64 bytes */
	struct page *pg = getpage(d, si, off, we);
	unsigned po = (unsigned)(off & (PAGE_SIZE - 1));
	uint64_t w0 = off & ~7ull, w1 = (off + n + 7) & ~7ull;

	if (we) {
		d->st_writes++;
		for (uint64_t w = w0; w < w1; w += 8) {
			unsigned wi = (unsigned)((w & (PAGE_SIZE - 1)) >> 3);
			int full = w >= off && w + 8 <= off + n;
			int k = inj_find(d, s->base + w);
			if (s->ecc_on && !full) {
				/* read-modify-write of a partial ECC word */
				if (!(pg->valid[wi >> 3] >> (wi & 7) & 1))
					ecc_event(d, si, s->base + w, 1);
				else if (k >= 0)
					ecc_event(d, si, s->base + w, d->inj[k].type == 2);
			}
			if (k >= 0)
				d->inj[k].type = 0;
			pg->valid[wi >> 3] |= (uint8_t)(1 << (wi & 7));
		}
		memcpy(pg->data + po, data, n);
	} else {
		d->st_reads++;
		if (pg)
			memcpy(data, pg->data + po, n);
		else if (zeroed(s, off & ~(uint64_t)(PAGE_SIZE - 1)))
			memset(data, 0, n);
		else {
			uint8_t tmp[72];
			garbage(d, si, w0, tmp, (unsigned)(w1 - w0));
			memcpy(data, tmp + (off - w0), n);
		}
		if (s->ecc_on) {
			int ue = 0;
			for (uint64_t w = w0; w < w1; w += 8) {
				unsigned wi = (unsigned)((w & (PAGE_SIZE - 1)) >> 3);
				int valid = pg ? (pg->valid[wi >> 3] >> (wi & 7) & 1)
				    : zeroed(s, off & ~(uint64_t)(PAGE_SIZE - 1));
				int k = inj_find(d, s->base + w);
				if (!valid) {
					if (!ue)
						ecc_event(d, si, s->base + w, 1);
					ue = 1;
				} else if (k >= 0) {
					ecc_event(d, si, s->base + w, d->inj[k].type == 2);
					if (d->inj[k].type == 2)	/* uncorrectable: bad data */
						for (uint64_t i = 0; i < 2; i++) {
							uint64_t bit = w + i;
							if (bit >= off && bit < off + n)
								data[bit - off] ^= 0x01;
						}
				}
			}
		}
	}
	*lat = timing(d, s, off, we, now);
	return 0;
}

/* ---------------------------------------------------------------- SMBus */

#define SMB_CYCLES	48	/* one byte read transaction */

int
dram_smb(struct dram *d, int we, uint64_t off, unsigned n, uint8_t *data)
{
	uint64_t v = 0;

	if ((n != 4 && n != 8) || (off & (n - 1)) || off >= SMB_SIZE)
		return -1;
	if (we) {
		memcpy(&v, data, n);
		if (off == 0 && (v >> 31 & 1) && !d->smb_busy) {
			d->smb_cmd = v;
			d->smb_busy = SMB_CYCLES;
			d->smb_status = 1;
		}
		return 0;
	}
	switch (off & ~7ull) {
	case 0x00: v = d->smb_cmd; break;
	case 0x08: v = d->smb_status; break;
	case 0x10: v = d->smb_data; break;
	}
	if (off & 4)
		v >>= 32;
	memcpy(data, &v, n);
	return 0;
}

static void
smb_finish(struct dram *d)
{
	unsigned slot = d->smb_cmd & 0xF;	/* [2:0] HID, [3] segment */
	unsigned addr = (d->smb_cmd >> 4) & 0x3FF;
	struct slot *s = &d->s[slot];

	if (!s->cfg.present) {
		d->smb_status = 2 | 4;	/* DONE | NACK */
		d->smb_data = 0xFF;
	} else {
		d->smb_status = 2;
		d->smb_data = s->spd[addr];
	}
}

/* ---------------------------------------------------------------- controller */

#define MC_ID	0x0001000035524444ull	/* "DDR5", version 1 */

enum {
	E_SEQ = 1,	/* command out of sequence */
	E_TCK = 2,	/* tCK outside a DIMM's tCKAVGmin..max */
	E_TIM = 3,	/* a timing below a DIMM's minimum */
	E_SLOT = 4,	/* slot config doesn't match the DIMM */
	E_MAP = 5,	/* slot base unaligned, outside the window, or overlapping */
	E_NONE = 6,	/* no slot enabled */
	E_MRS = 7,	/* training without mode-register writes */
};

static int
validate(struct dram *d)
{
	int any = 0;
	uint64_t tck = d->tck_ps;

	d->cl = (unsigned)(d->tim0 & 0xFFFF);
	d->trcd = (unsigned)(d->tim0 >> 16 & 0xFFFF);
	d->trp = (unsigned)(d->tim0 >> 32 & 0xFFFF);
	d->tras = (unsigned)(d->tim0 >> 48 & 0xFFFF);
	d->trc = (unsigned)(d->tim1 & 0xFFFF);
	d->trfc = (unsigned)(d->tim1 >> 16 & 0xFFFF);
	d->trefi = (unsigned)(d->tim1 >> 32 & 0xFFFF);
	d->tccdl = (unsigned)(d->tim1 >> 48 & 0xFF);
	d->tccds = (unsigned)(d->tim1 >> 56 & 0xFF);
	for (int i = 0; i < DRAM_SLOTS; i++) {
		struct slot *s = &d->s[i];
		s->en = s->creg & 1;
		s->ecc_on = s->creg >> 1 & 1;
		if (!s->en)
			continue;
		any = 1;
		if (!s->cfg.present || ((s->creg >> 8) & 0x3F) != (unsigned)(63 - __builtin_clzll(s->size)) ||
		    (s->ecc_on && !s->cfg.ecc))
			return E_SLOT;
		if ((s->base & ((1ull << 30) - 1)) || s->base < DRAM_BASE ||
		    s->base + s->size > DRAM_BASE + DRAM_WINDOW)
			return E_MAP;
		for (int j = 0; j < i; j++) {
			struct slot *o = &d->s[j];
			if (o->en && s->base < o->base + o->size && o->base < s->base + s->size)
				return E_MAP;
		}
		const struct speed *sp = s->sp;
		if (tck < sp->tck || tck > sp->tckmax)
			return E_TCK;
		if (d->cl < 20 || (d->cl & 1) || d->cl * tck < sp->taa ||
		    d->trcd * tck < sp->trcd || d->trp * tck < sp->trp ||
		    d->tras * tck < sp->tras || d->trc * tck < sp->trc ||
		    d->trfc * tck < trfc1_ns(s->g->dens_gb) * 1000ull ||
		    d->trefi == 0 || d->trefi * tck > 3900000 || d->trefi <= d->trfc ||
		    d->tccdl < 8 || d->tccdl * tck < 5000 || d->tccds < 8)
			return E_TIM;
	}
	return any ? 0 : E_NONE;
}

static void
mc_command(struct dram *d, unsigned cmd)
{
	static const int from[] = { -1, -2, ST_RESET, -3, ST_MRS, ST_ZQ, ST_TRAINED };
	static const unsigned cost[] = { 0, 100, 50, 10, 200, 2000, 20 };

	if (cmd < 1 || cmd > 6 || d->busy) {
		d->err = 1;
		d->errcode = E_SEQ;
		return;
	}
	int ok;
	if (cmd == 1)
		ok = 1;					/* RESET: always */
	else if (cmd == 3)
		ok = d->state == ST_CKE || d->state == ST_MRS;	/* MRW */
	else
		ok = d->state == from[cmd];
	if (!ok || (d->err && cmd != 1)) {
		d->err = 1;
		if (!d->errcode)
			d->errcode = E_SEQ;
		return;
	}
	d->pend_cmd = (int)cmd;
	d->busy = cost[cmd];
}

static void
mc_complete(struct dram *d)
{
	switch (d->pend_cmd) {
	case 1:
		d->state = ST_RESET;
		d->err = d->errcode = 0;
		break;
	case 2: d->state = ST_CKE; break;
	case 3:
		d->mr[d->mrw & 0xFF] = d->mrw >> 8 & 0xFF;
		d->state = ST_MRS;
		break;
	case 4: d->state = ST_ZQ; break;
	case 5: {
		int e = validate(d);
		if (e) {
			d->err = 1;
			d->errcode = e;
		} else
			d->state = ST_TRAINED;
		break;
	}
	case 6:
		d->state = ST_ONLINE;
		for (int i = 0; i < DRAM_SLOTS; i++)
			for (int b = 0; b < MAXBANKS; b++)
				d->s[i].bk[b] = (struct bank){ -1, 0, 0, 0 };
		break;
	}
	d->pend_cmd = 0;
}

static uint64_t
mc_read(struct dram *d, uint64_t off)
{
	if (off >= 0x100 && off < 0x200) {
		struct slot *s = &d->s[(off - 0x100) >> 4];
		return off & 8 ? s->base : s->creg;
	}
	switch (off) {
	case 0x000: return MC_ID;
	case 0x008:
		return (d->busy ? 1u : 0u) | (uint64_t)d->state << 1 | (uint64_t)d->err << 4 |
		    (uint64_t)d->errcode << 8 | (uint64_t)d->scrub_busy << 16 |
		    (uint64_t)d->scrub_done << 17;
	case 0x018: return d->mrw;
	case 0x020: return d->tck_ps;
	case 0x028: return d->tim0;
	case 0x030: return d->tim1;
	case 0x048: return d->scrub_bytes;
	case 0x050: return d->scrub_total;
	case 0x058: return d->ce;
	case 0x060: return d->ue;
	case 0x068: return d->last_addr;
	case 0x070: return d->last_info;
	case 0x200: return d->st_hit;
	case 0x208: return d->st_miss;
	case 0x210: return d->st_conf;
	case 0x218: return d->st_ref;
	default: return 0;
	}
}

static int
cfg_writable(const struct dram *d)
{
	return d->state < ST_TRAINED && !d->busy;
}

static void
mc_write(struct dram *d, uint64_t off, uint64_t v)
{
	if (off >= 0x100 && off < 0x200) {
		struct slot *s = &d->s[(off - 0x100) >> 4];
		if (cfg_writable(d)) {
			if (off & 8)
				s->base = v;
			else
				s->creg = v & 0x3F03;
		}
		return;
	}
	switch (off) {
	case 0x010: mc_command(d, (unsigned)v); break;
	case 0x018: d->mrw = v & 0xFFFF; break;
	case 0x020: if (cfg_writable(d)) d->tck_ps = v; break;
	case 0x028: if (cfg_writable(d)) d->tim0 = v; break;
	case 0x030: if (cfg_writable(d)) d->tim1 = v; break;
	case 0x040:
		if ((v & 1) && d->state == ST_ONLINE && !d->scrub_busy) {
			d->scrub_total = 0;
			for (int i = 0; i < DRAM_SLOTS; i++) {
				struct slot *s = &d->s[i];
				if (s->en && s->ecc_on) {
					s->sgen++;
					s->swm = 0;
					d->scrub_total += s->size;
				}
			}
			d->scrub_bytes = 0;
			d->scrub_busy = d->scrub_total != 0;
			d->scrub_done = !d->scrub_busy;
		}
		break;
	case 0x078:
		for (int i = 0; i < 8; i++)
			if (!d->inj[i].type || d->inj[i].pa == (v & ~7ull)) {
				d->inj[i].pa = v & ~7ull;
				d->inj[i].type = (v & 3) == 2 ? 2 : (v & 3) ? 1 : 0;
				break;
			}
		break;
	case 0x080:
		if (v & 1)
			d->ce = d->ue = d->last_addr = d->last_info = 0;
		break;
	default: break;
	}
}

int
dram_mc(struct dram *d, int we, uint64_t off, unsigned n, uint8_t *data)
{
	uint64_t v = 0;

	if ((n != 4 && n != 8) || (off & (n - 1)) || off >= MC_SIZE)
		return -1;
	if (we) {
		memcpy(&v, data, n);
		if (n == 4) {	/* 32-bit writes update one half */
			uint64_t old = mc_read(d, off & ~7ull);
			v = off & 4 ? (old & 0xFFFFFFFFull) | v << 32
			    : (old & ~0xFFFFFFFFull) | v;
			if ((off & ~7ull) == 0x010 || (off & ~7ull) == 0x040 ||
			    (off & ~7ull) == 0x078 || (off & ~7ull) == 0x080)
				v = off & 4 ? 0 : (v & 0xFFFFFFFFull);	/* command regs */
		}
		mc_write(d, off & ~7ull, v);
		return 0;
	}
	v = mc_read(d, off & ~7ull);
	if (off & 4)
		v >>= 32;
	memcpy(data, &v, n);
	return 0;
}

/* ---------------------------------------------------------------- tick */

#define SCRUB_BYTES_PER_CYCLE	(1ull << 20)	/* accelerated: see docs */

void
dram_tick(struct dram *d)
{
	if (d->smb_busy && --d->smb_busy == 0)
		smb_finish(d);
	if (d->busy && --d->busy == 0)
		mc_complete(d);
	if (d->scrub_busy) {
		uint64_t left = SCRUB_BYTES_PER_CYCLE;
		for (int i = 0; i < DRAM_SLOTS && left; i++) {
			struct slot *s = &d->s[i];
			if (!s->en || !s->ecc_on || s->swm >= s->size)
				continue;
			uint64_t k = s->size - s->swm < left ? s->size - s->swm : left;
			uint64_t lo = s->base + s->swm;
			s->swm += k;
			left -= k;
			d->scrub_bytes += k;
			for (int j = 0; j < 8; j++)
				if (d->inj[j].type && d->inj[j].pa >= lo && d->inj[j].pa < lo + k)
					d->inj[j].type = 0;
			if (s->swm == s->size)
				s->clean = 1;
		}
		if (d->scrub_bytes >= d->scrub_total) {
			d->scrub_busy = 0;
			d->scrub_done = 1;
		}
	}
}

void
dram_stats(const struct dram *d, char *buf, unsigned len)
{
	uint64_t n = d->st_reads + d->st_writes;
	snprintf(buf, len,
	    "dram: %llu reads, %llu writes, row hit %llu / closed %llu / conflict %llu, "
	    "refresh stalls %llu, mean latency %.1f cycles, %zu x 64 KiB host pages",
	    (unsigned long long)d->st_reads, (unsigned long long)d->st_writes,
	    (unsigned long long)d->st_hit, (unsigned long long)d->st_miss,
	    (unsigned long long)d->st_conf, (unsigned long long)d->st_ref,
	    n ? (double)d->st_lat / (double)n : 0.0, d->used);
}
