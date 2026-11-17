/*
 * vsim DDR5 DIMMs at the pin level (ddr5.h, docs/ddr5.md).
 *
 * Every rank of x8 DDR5 DRAMs is modelled from Micron's DDR5 SDRAM Product
 * Core Data Sheet (ddr5_sdram_core.pdf, Rev. D 10/2022), cited by table:
 *   Table 140  Command Truth Table: decode()
 *   Table 21   MR0 (burst length, CAS latency; CWL = CL - 2)
 *   Table 23   MR2 (2N mode default, CS assertion duration for MPC)
 *   Table 143  MPC opcodes; "MPC Function Timing" (multicycle CS, tMPC_CS,
 *              tMC_MPC_Setup/Hold) and Tables 462/... (tMPC_Delay = tMRD)
 *   Table 148  "2N Mode": second half of a two-cycle command two clocks later
 *   Table 10   initialization timing, and "Power-up Initialization Sequence"
 *   Table 176  tRFC1, tRFCsb by density; "tREFI and tRFC Parameters"
 *   Tables 428-430, 435-437  DDR5-4800 timing (tCCD, tRRD, tFAW, tRTP, tWR,
 *              tMRW, tMRD, tDLLK, tZQCAL/tZQLAT, tPW_RESET, tXS)
 *   Table 361  DDR5-4800 speed bin (tAA/tRCD/tRP 16 ns, tRAS 32, tRC 48)
 *   "Mode Register Read": MRR data on UI 8-15, odd DQs inverted
 *   "WRITE PATTERN": WRP/WRPA write MR48 instead of the DQ data
 * and the 16Gb DDR5 SDRAM Die Rev A data sheet, Table 2 (x8 addressing).
 * Nanosecond parameters become clocks with the data sheet's round-down
 * integer algorithm ("Using Round-Down Only Integer Number Math"): 0.3%
 * correction, then rounded up.
 *
 * The SPD hubs (spd_eval) follow JESD300-5 (SPD5118 hub, I2C legacy mode,
 * 1-byte addressing, MR11 page select) for the subset ddr5.h documents.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ddr5.h"
#include "spd.h"

#define NEVER		(-((int64_t)1 << 40))
#define RING		128		/* data-bus schedule, > CL + 8 */
#define TCK_PS		416		/* DDR5-4800 */

/* ns (in ps) -> nCK, the data sheet's round-down algorithm */
static int64_t
nck(uint64_t ps)
{
	return (int64_t)((ps * 997 / TCK_PS + 1000) / 1000);
}

static int64_t
max64(int64_t a, int64_t b)
{
	return a > b ? a : b;
}

/* DDR5-4800 timings, nCK (computed once) */
static struct tim {
	int64_t trcd, trp, tras, trc, trrd_s, trrd_l, tfaw, tccd_s, tccd_l;
	int64_t tccd_s_wr, tccd_l_wr, tccd_l_wr2, tccd_rtw, trtp, twr, tppd;
	int64_t tmrw, tmrd, tmpc, tzqcal, tzqlat, tdllk, trefi;
	int64_t tinit1, tinit2, tinit3, tinit4, tinit5, tpw_reset;
	int64_t tmpc_cs_min, tmpc_cs_max, tmc_setup, tmc_hold;
	int clmin;
} T;

static void
tim_init(void)
{
	T.trcd = nck(16000);
	T.trp = nck(16000);
	T.tras = nck(32000);
	T.trc = nck(48000);
	T.trrd_s = 8;
	T.trrd_l = max64(8, nck(5000));
	T.tfaw = max64(32, nck(13333));		/* 1 KB page (x8) */
	T.tccd_s = 8;
	T.tccd_l = max64(8, nck(5000));
	T.tccd_s_wr = 8;
	T.tccd_l_wr = max64(32, nck(20000));
	T.tccd_l_wr2 = max64(16, nck(10000));
	/* CL - CWL + RBL/2 + 2 tCK - read DQS offset (0) + (tRPST - 0.5 tCK) +
	   tWPRE: 2 + 8 + 2 + (1.5 - 0.5) + 2 (Table 429) */
	T.tccd_rtw = 15;
	T.trtp = max64(12, nck(7500));
	T.twr = nck(30000);
	T.tppd = 2;
	T.tmrw = max64(8, nck(5000));
	T.tmrd = max64(16, nck(14000));
	T.tmpc = T.tmrd;			/* tMPC_Delay = tMRD */
	T.tzqcal = nck(1000000);
	T.tzqlat = max64(8, nck(30000));
	T.tdllk = 1536;
	T.trefi = 3900000 / TCK_PS;		/* 3.9 us: an interval, not a minimum */
	T.tinit1 = nck(200000000ull);
	T.tinit2 = nck(10000);
	T.tinit3 = nck(4000000000ull);
	T.tinit4 = nck(2000000);
	T.tinit5 = 3;
	T.tpw_reset = nck(1000000);
	T.tmpc_cs_min = 4;			/* 3.5 nCK: at least 4 sampled clocks */
	T.tmpc_cs_max = 8;
	T.tmc_setup = 3;
	T.tmc_hold = 3;
	T.clmin = (int)nck(16000);		/* tAA 16.0 ns */
}

/* ---------------------------------------------------------------- storage */

#define COLS	1024
#define DEVS	5			/* 4 data + 1 ECC device */

struct pagetab {
	uint64_t *keys;		/* key + 1, 0 = empty */
	uint8_t **pages;	/* COLS * DEVS bytes */
	size_t cap, used;
};

static uint64_t
mix(uint64_t x)
{
	x += 0x9E3779B97F4A7C15ull;
	x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
	x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
	return x ^ (x >> 31);
}

static uint8_t **
pt_find(struct pagetab *t, uint64_t key, int create)
{
	if (create && (t->used + 1) * 2 > t->cap) {
		size_t nc = t->cap ? t->cap * 2 : 1024;
		uint64_t *nk = calloc(nc, sizeof *nk);
		uint8_t **np = calloc(nc, sizeof *np);
		if (!nk || !np) {
			fprintf(stderr, "ddr5: out of memory\n");
			exit(1);
		}
		for (size_t i = 0; i < t->cap; i++) {
			if (!t->keys[i])
				continue;
			size_t j = mix(t->keys[i]) & (nc - 1);
			while (nk[j])
				j = (j + 1) & (nc - 1);
			nk[j] = t->keys[i];
			np[j] = t->pages[i];
		}
		free(t->keys);
		free(t->pages);
		t->keys = nk;
		t->pages = np;
		t->cap = nc;
	}
	if (!t->cap)
		return NULL;
	size_t j = mix(key + 1) & (t->cap - 1);
	while (t->keys[j]) {
		if (t->keys[j] == key + 1)
			return &t->pages[j];
		j = (j + 1) & (t->cap - 1);
	}
	if (!create)
		return NULL;
	t->keys[j] = key + 1;
	t->used++;
	return &t->pages[j];
}

/* ---------------------------------------------------------------- state */

enum { R_RESET, R_INIT, R_CFG, R_READY, R_SR, R_PD };

struct bank {
	int open;
	unsigned row;
	int64_t t_act, t_pre, t_rd, t_wr_end, ap_at, sb_until;
};

struct rank {
	int present;
	unsigned slot, cs;
	const struct geom *g;
	int ecc;
	int st, phase;
	int64_t t_mark;			/* init: when the current phase started */
	int64_t cs_low_since;
	int powered;			/* the first reset is the power-up one */
	int64_t t_reset_low;
	unsigned nops;
	uint8_t mr[256];
	uint8_t mr13_shadow;
	int dllk_set, dll_reset, zq_started, zq_latched, flag_ref;
	int64_t t_dll, t_zqst, t_zqlat, t_cfg_ok, t_sr_exit;
	struct bank bk[8][4];
	int64_t t_act, t_act_bg[8], act_ring[4];
	unsigned act_ri;
	int64_t t_rd, t_rd_bg[8], t_wr, t_wr_bg[8], t_pre;
	int64_t t_mrw, t_mpc, t_ref, ref_busy;
	/* multicycle CS run (MR2:OP[4] = 0) */
	unsigned run;
	uint32_t run_ca;
	int run_stable;
	int64_t run_start;
	int hold_left;
	uint32_t hold_ca;
	/* the first half of a two-cycle command */
	int pend;
	uint32_t pend_ca;
	int64_t pend_t;
	struct pagetab pt;
	/* statistics */
	uint64_t n_act, n_rd, n_wr, n_ref, n_mrw, n_mpc;
};

struct beat2 {
	int valid;
	int kind;			/* 1 read data out, 2 write capture */
	unsigned cs, bg, ba, row, col;	/* write: first column of the pair */
	uint64_t data[2];		/* read: the two beats (40 bits each) */
};

struct sub {
	struct rank rk[DDR5_RANKS];
	uint32_t ca_hist[8];		/* CA of the last 8 clocks */
	uint32_t cs_prev;
	struct beat2 ring[RING];
};

struct hub {
	uint8_t mr[128];
};

struct ddr5_mem {
	struct ddr5_slot_cfg cfg[DDR5_SLOTS];
	const struct geom *g;
	uint8_t spd[DDR5_SLOTS][1024];
	struct hub hub[DDR5_SLOTS];
	struct sub sub[DDR5_SUBS];
	int fast;
	uint64_t seed;
	int nchan, ndimm;		/* populated channels (1, 2), DIMMs per channel (1, 2) */
	int64_t now;
	bool reset_prev[DDR5_CHANNELS];
	uint64_t viol;
	char first[200];
	int trace;
	/* SPD bus */
	int scl_p, sda_p, pull;
	int i2c_on, phase, bits, rw, tgt, wfirst, ack;
	uint8_t sh, offs, page;
};

/* ---------------------------------------------------------------- violations */

static void
viol(struct ddr5_mem *m, int s, int cs, const char *fmt, ...)
	__attribute__((format(printf, 4, 5)));

static void
viol(struct ddr5_mem *m, int s, int cs, const char *fmt, ...)
{
	char msg[160];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(msg, sizeof msg, fmt, ap);
	va_end(ap);
	m->viol++;
	if (!m->first[0])
		snprintf(m->first, sizeof m->first, "sub %d cs %d cycle %lld: %s", s, cs,
		    (long long)m->now, msg);
	if (m->trace)
		fprintf(stderr, "ddr5: sub %d cs %d cycle %lld: %s\n", s, cs, (long long)m->now, msg);
}

/* the init waits the meminit_fast strap scales (docs/die-interface.md) */
static int64_t
slow(const struct ddr5_mem *m, int64_t t)
{
	if (!m->fast)
		return t;
	int64_t v = (t + 1023) / 1024;
	return v < 1 ? 1 : v;
}

static int64_t
trfc1(const struct rank *r)
{
	return nck((uint64_t)spd_trfc1_ns(r->g->dens_gb) * 1000);
}

static int64_t
trfcsb(const struct rank *r)
{
	return nck(r->g->dens_gb >= 32 ? 190000 : 130000);	/* Table 176 */
}

static int
cl_of(const struct rank *r)
{
	return 22 + 2 * (r->mr[0] >> 2);	/* MR0 OP[7:2] (Table 21) */
}

static int
twoN(const struct rank *r)
{
	return !(r->mr[2] & 4);			/* MR2 OP[2] = 0: 2N mode (default) */
}

/* ---------------------------------------------------------------- data */

/*
 * What a DRAM cell holds before anything wrote it: random at power-on. With
 * the die's meminit_fast strap the DIMMs instead start cleared (zero data,
 * zero check bits: the Hsiao code of 0), standing in for the memory clear an
 * ECC platform does before the cores start (AMD's ABL zeroes DRAM so that
 * reads never meet uninitialized ECC) -- clearing 16 GiB line by line would
 * take billions of simulated clocks.
 */
static uint8_t
garbage(const struct ddr5_mem *m, uint64_t key, unsigned col, unsigned dev)
{
	if (m->fast)
		return 0;
	return (uint8_t)mix(m->seed ^ (key << 14) ^ (col * DEVS + dev));
}

static uint64_t
pkey(unsigned bg, unsigned ba, unsigned row)
{
	return (uint64_t)row << 5 | bg << 2 | ba;
}

static uint8_t
rd_byte(struct ddr5_mem *m, struct rank *r, unsigned bg, unsigned ba, unsigned row,
    unsigned col, unsigned dev)
{
	if (dev == 4 && !r->ecc)
		return 0;
	uint64_t k = pkey(bg, ba, row);
	uint8_t **p = pt_find(&r->pt, k, 0);
	if (p && *p)
		return (*p)[col * DEVS + dev];
	return garbage(m, k ^ ((uint64_t)r->slot << 40 | (uint64_t)r->cs << 44), col, dev);
}

static void
wr_byte(struct ddr5_mem *m, struct rank *r, unsigned bg, unsigned ba, unsigned row,
    unsigned col, unsigned dev, uint8_t v)
{
	if (dev == 4 && !r->ecc)
		return;
	uint64_t k = pkey(bg, ba, row);
	uint8_t **p = pt_find(&r->pt, k, 1);
	if (!*p) {
		*p = malloc(COLS * DEVS);
		if (!*p) {
			fprintf(stderr, "ddr5: out of memory\n");
			exit(1);
		}
		uint64_t gk = k ^ ((uint64_t)r->slot << 40 | (uint64_t)r->cs << 44);
		for (unsigned c = 0; c < COLS; c++)
			for (unsigned d = 0; d < DEVS; d++)
				(*p)[c * DEVS + d] = garbage(m, gk, c, d);
	}
	(*p)[col * DEVS + dev] = v;
}

static uint64_t
beat_rd(struct ddr5_mem *m, struct rank *r, unsigned bg, unsigned ba, unsigned row, unsigned col)
{
	uint64_t b = 0;
	for (unsigned d = 0; d < DEVS; d++)
		b |= (uint64_t)rd_byte(m, r, bg, ba, row, col & (COLS - 1), d) << (8 * d);
	return b;
}

static void
beat_wr(struct ddr5_mem *m, struct rank *r, unsigned bg, unsigned ba, unsigned row, unsigned col,
    uint64_t b)
{
	for (unsigned d = 0; d < DEVS; d++)
		wr_byte(m, r, bg, ba, row, col & (COLS - 1), d, (uint8_t)(b >> (8 * d)));
}

static uint64_t
get40(const uint32_t w[3], unsigned beat)
{
	uint64_t lo = (uint64_t)w[0] | (uint64_t)w[1] << 32;
	uint64_t v = beat ? (lo >> 40) | ((uint64_t)w[2] << 24) : lo;
	return v & 0xFFFFFFFFFFull;
}

static void
put80(uint32_t w[3], uint64_t b0, uint64_t b1)
{
	b0 &= 0xFFFFFFFFFFull;
	b1 &= 0xFFFFFFFFFFull;
	w[0] = (uint32_t)b0;
	w[1] = (uint32_t)(b0 >> 32) | (uint32_t)(b1 << 8);
	w[2] = (uint32_t)(b1 >> 24);
}

/* schedule n clocks of a data burst at cycle t0 */
static void
sched(struct ddr5_mem *m, int s, struct rank *r, int64_t t0, int kind, unsigned bg, unsigned ba,
    unsigned row, unsigned col0, int clocks, const uint64_t *rdata)
{
	struct sub *sb = &m->sub[s];
	for (int k = 0; k < clocks; k++) {
		struct beat2 *e = &sb->ring[(t0 + k) % RING];
		if (e->valid) {
			viol(m, s, (int)r->cs, "data bus conflict at cycle %lld (bursts overlap)",
			    (long long)(t0 + k));
			continue;
		}
		e->valid = 1;
		e->kind = kind;
		e->cs = r->cs;
		e->bg = bg;
		e->ba = ba;
		e->row = row;
		e->col = col0 + 2u * (unsigned)k;
		if (rdata) {
			e->data[0] = rdata[2 * k];
			e->data[1] = rdata[2 * k + 1];
		}
	}
}

/* ---------------------------------------------------------------- commands */

static int
ready(struct ddr5_mem *m, int s, struct rank *r, const char *what)
{
	if (r->st == R_READY)
		return 1;
	viol(m, s, (int)r->cs, "%s before initialization is complete (state %d)", what, r->st);
	return 0;
}

static void
check_since(struct ddr5_mem *m, int s, struct rank *r, int64_t since, int64_t min, const char *what)
{
	if (m->now - since < min)
		viol(m, s, (int)r->cs, "%s violated: %lld < %lld nCK", what,
		    (long long)(m->now - since), (long long)min);
}

static int
all_idle(const struct rank *r)
{
	for (int g = 0; g < 8; g++)
		for (int b = 0; b < 4; b++)
			if (r->bk[g][b].open)
				return 0;
	return 1;
}

/* the commands that must wait tMRD after MRW and tMPC_Delay after MPC */
static void
after_mode(struct ddr5_mem *m, int s, struct rank *r)
{
	check_since(m, s, r, r->t_mrw, T.tmrd, "tMRD (MRW to command)");
	check_since(m, s, r, r->t_mpc, T.tmpc, "tMPC_Delay");
}

static void
do_pre(struct ddr5_mem *m, int s, struct rank *r, unsigned g, unsigned b, int64_t at)
{
	struct bank *k = &r->bk[g][b];
	if (!k->open)
		return;				/* precharging an idle bank: a NOP */
	if (at - k->t_act < T.tras)
		viol(m, s, (int)r->cs, "tRAS violated (BG%u BA%u)", g, b);
	if (at - k->t_rd < T.trtp)
		viol(m, s, (int)r->cs, "tRTP violated (BG%u BA%u)", g, b);
	if (at < k->t_wr_end + T.twr)
		viol(m, s, (int)r->cs, "tWR violated (BG%u BA%u)", g, b);
	k->open = 0;
	k->t_pre = at;
	k->ap_at = NEVER;
}

static void
cmd_act(struct ddr5_mem *m, int s, struct rank *r, unsigned g, unsigned b, unsigned row)
{
	struct bank *k = &r->bk[g][b];
	if (!ready(m, s, r, "ACT"))
		return;
	after_mode(m, s, r);
	if (k->open) {
		viol(m, s, (int)r->cs, "ACT to an open bank (BG%u BA%u)", g, b);
		return;
	}
	if (row >> r->g->rows)
		viol(m, s, (int)r->cs, "ACT row %u beyond R%u", row, r->g->rows - 1);
	check_since(m, s, r, k->t_pre, T.trp, "tRP");
	check_since(m, s, r, k->t_act, T.trc, "tRC");
	check_since(m, s, r, r->t_act, T.trrd_s, "tRRD_S");
	check_since(m, s, r, r->t_act_bg[g], T.trrd_l, "tRRD_L");
	check_since(m, s, r, r->act_ring[r->act_ri], T.tfaw, "tFAW");
	if (m->now < r->ref_busy)
		viol(m, s, (int)r->cs, "ACT during tRFC");
	if (m->now < k->sb_until)
		viol(m, s, (int)r->cs, "ACT during tRFCsb (BG%u BA%u)", g, b);
	k->open = 1;
	k->row = row & ((1u << r->g->rows) - 1);
	k->t_act = m->now;
	r->t_act = m->now;
	r->t_act_bg[g] = m->now;
	r->act_ring[r->act_ri] = m->now;
	r->act_ri = (r->act_ri + 1) & 3;
	r->n_act++;
}

static void
cmd_rw(struct ddr5_mem *m, int s, struct rank *r, int wr, int pattern, unsigned g, unsigned b,
    unsigned col, int ap, int bc8, int partial, int64_t tfinal)
{
	struct bank *k = &r->bk[g][b];
	const char *what = wr ? (pattern ? "WRP" : "WR") : "RD";
	if (!ready(m, s, r, what))
		return;
	after_mode(m, s, r);
	if (!k->open) {
		viol(m, s, (int)r->cs, "%s to a closed bank (BG%u BA%u)", what, g, b);
		return;
	}
	check_since(m, s, r, k->t_act, T.trcd, "tRCD");
	if (m->now < r->t_dll + slow(m, T.tdllk))
		viol(m, s, (int)r->cs, "%s before tDLLK after DLL reset", what);
	int cl = cl_of(r);
	if (cl < T.clmin)
		viol(m, s, (int)r->cs, "CL%d below tAA for DDR5-4800 (CL >= %d)", cl, T.clmin);
	if ((r->mr[0] & 3) == 2 || (r->mr[0] & 3) == 3)
		viol(m, s, (int)r->cs, "BL32 is not modelled (MR0 OP[1:0] = %u)", r->mr[0] & 3);
	if (col & 15)
		viol(m, s, (int)r->cs, "%s column %u not aligned to 16 (C[3:0] must be 0)", what, col);
	col &= ~15u;
	int clocks = bc8 && (r->mr[0] & 3) == 1 ? 4 : 8;
	if (!wr) {
		check_since(m, s, r, r->t_rd, T.tccd_s, "tCCD_S (RD to RD)");
		check_since(m, s, r, r->t_rd_bg[g], T.tccd_l, "tCCD_L (RD to RD)");
		check_since(m, s, r, r->t_wr, cl - 2 + 8 + max64(4, nck(2500)), "tCCD_S_WTR");
		check_since(m, s, r, r->t_wr_bg[g], cl - 2 + 8 + max64(16, nck(10000)), "tCCD_L_WTR");
		uint64_t d[16];
		for (int i = 0; i < 2 * clocks; i++)
			d[i] = beat_rd(m, r, g, b, k->row, col + (unsigned)i);
		sched(m, s, r, tfinal + cl, 1, g, b, k->row, col, clocks, d);
		r->t_rd = r->t_rd_bg[g] = m->now;
		k->t_rd = m->now;
		r->n_rd++;
		if (ap)
			k->ap_at = max64(m->now + T.trtp, k->t_act + T.tras);
	} else {
		check_since(m, s, r, r->t_wr, T.tccd_s_wr, "tCCD_S_WR (WR to WR)");
		check_since(m, s, r, r->t_wr_bg[g], partial ? T.tccd_l_wr : T.tccd_l_wr2,
		    partial ? "tCCD_L_WR (WR to WR)" : "tCCD_L_WR2 (WR to WR)");
		check_since(m, s, r, r->t_rd, T.tccd_rtw, "tCCD_RTW (RD to WR)");
		int cwl = cl - 2;
		if (pattern) {
			/* WRITE PATTERN: MR48 into every byte of the burst */
			for (int i = 0; i < 2 * clocks; i++) {
				uint64_t v = 0;
				for (int dv = 0; dv < DEVS; dv++)
					v |= (uint64_t)r->mr[48] << (8 * dv);
				beat_wr(m, r, g, b, k->row, col + (unsigned)i, v);
			}
		} else
			sched(m, s, r, tfinal + cwl, 2, g, b, k->row, col, clocks, NULL);
		r->t_wr = r->t_wr_bg[g] = m->now;
		k->t_wr_end = tfinal + cwl + clocks;
		r->n_wr++;
		if (ap)
			k->ap_at = k->t_wr_end + T.twr;
	}
}

static void
cmd_ref(struct ddr5_mem *m, int s, struct rank *r, int sb, unsigned ba)
{
	if (!ready(m, s, r, sb ? "REFsb" : "REFab"))
		return;
	after_mode(m, s, r);
	if (m->now < r->ref_busy)
		viol(m, s, (int)r->cs, "REF during tRFC");
	if (!sb) {
		for (int g = 0; g < 8; g++)
			for (int b = 0; b < 4; b++) {
				if (r->bk[g][b].open)
					viol(m, s, (int)r->cs, "REFab with BG%d BA%d open", g, b);
				else if (m->now - r->bk[g][b].t_pre < T.trp)
					viol(m, s, (int)r->cs, "tRP before REFab (BG%d BA%d)", g, b);
			}
		r->ref_busy = m->now + trfc1(r);
	} else {
		if (!(r->mr[4] & 0x10))
			viol(m, s, (int)r->cs, "REFsb in normal refresh mode (MR4:OP[4] = 0)");
		for (int g = 0; g < 8; g++) {
			if (r->bk[g][ba].open)
				viol(m, s, (int)r->cs, "REFsb with BG%d BA%u open", g, ba);
			r->bk[g][ba].sb_until = m->now + trfcsb(r);
		}
	}
	if (m->now - r->t_ref > 5 * T.trefi + T.trefi / 2 && r->t_ref != NEVER && !r->flag_ref)
		viol(m, s, (int)r->cs, "refresh overdue: %lld nCK since the last REF (more than 4 postponed)",
		    (long long)(m->now - r->t_ref));
	r->flag_ref = 0;
	r->t_ref = m->now;
	r->n_ref++;
}

static void
cmd_mrw(struct ddr5_mem *m, int s, struct rank *r, unsigned mra, unsigned op)
{
	if (r->st != R_CFG && r->st != R_READY) {
		viol(m, s, (int)r->cs, "MRW in state %d", r->st);
		return;
	}
	if (m->now < r->t_cfg_ok)
		viol(m, s, (int)r->cs, "MRW before tXPR");
	check_since(m, s, r, r->t_mrw, T.tmrw, "tMRW");
	check_since(m, s, r, r->t_mpc, T.tmpc, "tMPC_Delay");
	if (mra == 2) {
		/* OP[2] (2N mode) is read-only: set by MPC (Table 148) */
		op = (op & ~4u) | (r->mr[2] & 4u);
	}
	if (mra == 13) {
		viol(m, s, (int)r->cs, "MRW to MR13 (MPC-only register)");
		return;
	}
	r->mr[mra & 0xFF] = (uint8_t)op;
	r->t_mrw = m->now;
	r->n_mrw++;
}

static void
cmd_mpc(struct ddr5_mem *m, int s, struct rank *r, unsigned op)
{
	if (r->st != R_CFG && r->st != R_READY) {
		viol(m, s, (int)r->cs, "MPC %02Xh in state %d", op, r->st);
		return;
	}
	if (m->now < r->t_cfg_ok)
		viol(m, s, (int)r->cs, "MPC before tXPR");
	check_since(m, s, r, r->t_mpc, T.tmpc, "tMPC_Delay");
	check_since(m, s, r, r->t_mrw, T.tmrd, "tMRD (MRW to MPC)");
	r->t_mpc = m->now;
	r->n_mpc++;
	switch (op) {
	case 0x00: case 0x01: case 0x03:		/* CS/CA training exit/entry */
	case 0x06: case 0x07:				/* DQS oscillator */
	case 0x0A: case 0x0B: case 0x0C:		/* PDA enumerate program mode, ECS */
	case 0x1F:					/* apply VREFCA/VREFCS/RTT */
		if (op != 0x0C && op != 0x06 && op != 0x07 && !r->zq_latched)
			viol(m, s, (int)r->cs, "MPC %02Xh (training) before ZQCal Latch", op);
		return;
	case 0x02:					/* DLL RESET */
		if (!r->dllk_set)
			viol(m, s, (int)r->cs, "DLL RESET before MPC tDLLK/tCCD_L (MR13)");
		r->mr[13] = (uint8_t)((r->mr[13] & 0xF0) | (r->mr13_shadow & 0x0F));
		r->dll_reset = 1;
		r->t_dll = m->now;
		return;
	case 0x05:					/* ZQCal Start */
		if (!r->dll_reset)
			viol(m, s, (int)r->cs, "ZQCal Start before DLL RESET");
		r->zq_started = 1;
		r->t_zqst = m->now;
		return;
	case 0x04:					/* ZQCal Latch */
		if (!r->zq_started)
			viol(m, s, (int)r->cs, "ZQCal Latch without ZQCal Start");
		else if (m->now - r->t_zqst < slow(m, T.tzqcal))
			viol(m, s, (int)r->cs, "ZQCal Latch before tZQCAL");
		r->zq_latched = 1;
		r->t_zqlat = m->now;
		if (r->st == R_CFG && r->dll_reset)
			r->st = R_READY;
		return;
	case 0x08: r->mr[2] &= (uint8_t)~4u; return;	/* SET 2N */
	case 0x09: r->mr[2] |= 4; return;		/* SET 1N */
	case 0x91: return;				/* CS geardown enable */
	}
	if ((op & 0xF0) == 0x80) {			/* tDLLK/tCCD_L into MR13's shadow */
		r->mr13_shadow = (uint8_t)(op & 0x0F);
		r->dllk_set = 1;
		return;
	}
	if ((op & 0xE0) == 0x20 || (op & 0xF0) == 0x40 || (op & 0xF0) == 0x50 ||
	    (op & 0xE0) == 0x60)
		return;					/* RTT settings, PDA IDs: stored nowhere */
	viol(m, s, (int)r->cs, "reserved MPC opcode %02Xh", op);
}

static void
cmd_mrr(struct ddr5_mem *m, int s, struct rank *r, unsigned mra, int64_t tfinal)
{
	if (r->st != R_CFG && r->st != R_READY) {
		viol(m, s, (int)r->cs, "MRR in state %d", r->st);
		return;
	}
	if (!all_idle(r))
		viol(m, s, (int)r->cs, "MRR with banks open");
	/* BL16: UI 0-7 = 0 (no inversion), UI 8-15 = OP[0..7] on every DQ,
	   odd DQs inverted ("Mode Register Read") */
	uint8_t op = r->mr[mra & 0xFF];
	uint64_t d[16];
	for (int ui = 0; ui < 16; ui++) {
		uint64_t v = 0;
		if (ui >= 8) {
			unsigned bit = (op >> (ui - 8)) & 1;
			for (int dq = 0; dq < 40; dq++)
				v |= (uint64_t)(bit ^ (unsigned)(dq & 1)) << dq;
		}
		d[ui] = v;
	}
	sched(m, s, r, tfinal + cl_of(r), 1, 0, 0, 0, 0, 8, d);
}

/* a whole command (the second half too, for two-cycle commands) */
static void
execute(struct ddr5_mem *m, int s, struct rank *r, uint32_t c1, uint32_t c2, int64_t tfinal)
{
#define B(v, i) (((v) >> (i)) & 1u)
	unsigned bg = B(c1, 8) | B(c1, 9) << 1 | B(c1, 10) << 2;
	unsigned ba = B(c1, 6) | B(c1, 7) << 1;
	if (!B(c1, 1)) {				/* two-cycle */
		if (!B(c1, 0)) {			/* ACT */
			unsigned row = (c1 >> 2 & 15) | (c2 & 0x1FFF) << 4 | B(c2, 13) << 17;
			cmd_act(m, s, r, bg, ba, row);
			return;
		}
		unsigned k = c1 >> 2 & 7;		/* CA4..CA2 */
		switch (k) {
		case 0x2: case 0x6: {			/* CA2=L CA3=H: 0x2 WRP (CA4=L) */
			if (k == 0x6) {			/* CA2=L CA3=H CA4=H: RFU */
				viol(m, s, (int)r->cs, "RFU two-cycle command");
				return;
			}
			unsigned col = (c2 >> 1 & 0xFF) << 3;	/* C3..C10 on CA1..CA8 */
			cmd_rw(m, s, r, 1, 1, bg, ba, col & 0x3FF, !B(c2, 10), 0, 0, tfinal);
			return;
		}
		case 0x1:				/* MRW: H L H L L */
			if (!B(c2, 10))
				cmd_mrw(m, s, r, c1 >> 5 & 0xFF, c2 & 0xFF);
			return;
		case 0x5:				/* MRR: H L H L H */
			if (!B(c2, 10))
				cmd_mrr(m, s, r, c1 >> 5 & 0xFF, tfinal);
			return;
		case 0x3: {				/* WR: H L H H L */
			unsigned col = (c2 >> 1 & 0xFF) << 3;
			cmd_rw(m, s, r, 1, 0, bg, ba, col & 0x3FF, !B(c2, 10), !B(c1, 5), !B(c2, 11),
			    tfinal);
			return;
		}
		case 0x7: {				/* RD: H L H H H */
			unsigned col = (c2 & 0x1FF) << 2;	/* C2..C10 on CA0..CA8 */
			cmd_rw(m, s, r, 0, 0, bg, ba, col & 0x3FF, !B(c2, 10), !B(c1, 5), 0, tfinal);
			return;
		}
		default:
			viol(m, s, (int)r->cs, "RFU two-cycle command (CA[4:0] = %02Xh)", c1 & 31);
			return;
		}
	}
	if (!B(c1, 0)) {
		viol(m, s, (int)r->cs, "REXT/reserved command");
		return;
	}
	unsigned k = c1 >> 2 & 7;			/* CA4..CA2, CA0 = CA1 = H */
	switch (k) {
	case 0x0:					/* VREFCA / VREFCS: H H L L L */
		r->mr[B(c1, 12) ? 12 : 11] = (uint8_t)(c1 >> 5 & 0x7F);
		r->t_mpc = m->now;
		return;
	case 0x4:					/* REF/RFM: H H L L H */
		cmd_ref(m, s, r, B(c1, 10), ba);
		return;
	case 0x2:					/* PREab/PREsb: H H L H L */
	case 0x6: {					/* PREpb: H H L H H */
		if (!ready(m, s, r, "PRE"))
			return;
		after_mode(m, s, r);
		check_since(m, s, r, r->t_pre, T.tppd, "tPPD");
		r->t_pre = m->now;
		for (unsigned g = 0; g < 8; g++)
			for (unsigned b = 0; b < 4; b++)
				if (k == 0x6 ? (g == bg && b == ba) : B(c1, 10) ? b == ba : 1)
					do_pre(m, s, r, g, b, m->now);
		return;
	}
	case 0x1:					/* RFU: H H H L L */
		viol(m, s, (int)r->cs, "RFU one-cycle command");
		return;
	case 0x5:					/* SRE/SREF/PDE: H H H L H */
		if (!ready(m, s, r, B(c1, 10) ? "PDE" : "SRE"))
			return;
		if (!B(c1, 10)) {
			if (!all_idle(r))
				viol(m, s, (int)r->cs, "SRE with banks open");
			r->st = R_SR;
		} else
			r->st = R_PD;
		return;
	case 0x3:					/* MPC: H H H H L */
		cmd_mpc(m, s, r, c1 >> 5 & 0xFF);
		return;
	case 0x7:					/* NOP */
		return;
	}
#undef B
}

/* ---------------------------------------------------------------- per clock */

static int
is_mc(uint32_t ca)	/* MPC, VREFCA/VREFCS, NOP: multicycle-CS candidates */
{
	unsigned k = ca & 31;
	return k == 0x0F || k == 0x03 || k == 0x1F;	/* MPC H H H H L, VREF H H L L L, NOP */
}

static void
rank_clock(struct ddr5_mem *m, int s, struct rank *r, int cs_low, uint32_t ca, int reset_n)
{
	struct sub *sb = &m->sub[s];
	int64_t now = m->now;

	if (cs_low && r->cs_low_since == NEVER)
		r->cs_low_since = now;
	if (!cs_low)
		r->cs_low_since = NEVER;

	/* reset and the power-up sequence (Table 10) */
	if (!reset_n) {
		if (r->st != R_RESET) {
			r->st = R_RESET;
			r->t_reset_low = now;
			r->pend = 0;
			r->run = 0;
		}
		return;
	}
	if (r->st == R_RESET) {
		int64_t need = r->powered ? T.tpw_reset : slow(m, T.tinit1);
		if (now - r->t_reset_low < need)
			viol(m, s, (int)r->cs, "RESET_n low %lld nCK (< %s %lld)",
			    (long long)(now - r->t_reset_low), r->powered ? "tPW_RESET" : "tINIT1",
			    (long long)need);
		if (!cs_low || now - r->cs_low_since < T.tinit2)
			viol(m, s, (int)r->cs, "CS_n not LOW tINIT2 before RESET_n HIGH");
		r->powered = 1;
		memset(r->mr, 0, sizeof r->mr);
		/* MR0 = 0: BL16, CL22 (the controller programs CL; Table 21) */
		r->dllk_set = r->dll_reset = r->zq_started = r->zq_latched = 0;
		for (int g = 0; g < 8; g++)
			for (int b = 0; b < 4; b++) {
				r->bk[g][b].open = 0;
				r->bk[g][b].ap_at = NEVER;
			}
		r->st = R_INIT;
		r->phase = 0;
		r->t_mark = now;
		return;
	}
	if (r->st == R_INIT) {
		switch (r->phase) {
		case 0:				/* CS_n LOW for tINIT3 after RESET_n HIGH */
			if (!cs_low) {
				if (now - r->t_mark < slow(m, T.tinit3))
					viol(m, s, (int)r->cs, "CS_n HIGH %lld nCK after RESET_n (< tINIT3)",
					    (long long)(now - r->t_mark));
				r->phase = 1;
				r->t_mark = now;
			}
			return;
		case 1:				/* CS_n HIGH for tINIT4 */
			if (cs_low) {
				if (now - r->t_mark < slow(m, T.tinit4))
					viol(m, s, (int)r->cs, "CS_n LOW during tINIT4");
				r->phase = 2;
				r->nops = 0;
				goto nop;
			}
			return;
		case 2:				/* NOPs for tINIT5, then tXPR */
		nop:
			if (cs_low) {
				if ((ca & 31) != 31)
					viol(m, s, (int)r->cs, "not a NOP during reset exit (CA %04Xh)", ca);
				r->nops++;
				return;
			}
			if (r->nops < T.tinit5)
				viol(m, s, (int)r->cs, "%u NOPs at reset exit (< tINIT5)", r->nops);
			r->st = R_CFG;
			r->t_cfg_ok = now + slow(m, trfc1(r));	/* tXPR = tXS = tRFC1 */
			r->t_mrw = r->t_mpc = NEVER;
			r->t_ref = NEVER;
			return;
		}
	}

	/* the second half of a two-cycle command */
	if (r->pend) {
		int64_t at = r->pend_t + (twoN(r) ? 2 : 1);
		if (now < at) {
			if (cs_low)
				viol(m, s, (int)r->cs, "CS_n LOW between the halves of a 2N command");
			return;
		}
		r->pend = 0;
		if (cs_low) {
			/* CS_n LOW on the second cycle: a cancel (ACT, MRW, WRP) or a
			   non-target ODT command (WR, RD, MRR): nothing executes */
			return;
		}
		int64_t keep = m->now;
		m->now = r->pend_t;		/* timing references: the first cycle */
		execute(m, s, r, r->pend_ca, ca, now);
		m->now = keep;
		return;
	}

	/* self refresh / power down: a NOP exits */
	if (r->st == R_SR || r->st == R_PD) {
		if (cs_low && (ca & 31) == 31) {
			if (r->st == R_SR) {
				r->ref_busy = now + trfc1(r);	/* tXS */
				r->t_ref = now;
			}
			r->st = R_READY;
		} else if (cs_low)
			viol(m, s, (int)r->cs, "command in %s", r->st == R_SR ? "self refresh" : "power down");
		return;
	}

	/* multicycle CS assertion (MR2:OP[4] = 0) for MPC, VREFCA/VREFCS */
	int multi = !(r->mr[2] & 0x10);
	if (r->hold_left > 0) {
		if (ca != r->hold_ca)
			viol(m, s, (int)r->cs, "CA changed during tMC_MPC_Hold");
		r->hold_left--;
	}
	if (r->run) {
		if (cs_low) {
			r->run++;
			if (ca != r->run_ca)
				r->run_stable = 0;
			return;
		}
		/* CS_n rose: the run ended at now - 1 */
		uint32_t c = r->run_ca;
		unsigned len = r->run;
		r->run = 0;
		if ((c & 31) != 31) {
			if (len < T.tmpc_cs_min || len > T.tmpc_cs_max)
				viol(m, s, (int)r->cs, "tMPC_CS: CS_n LOW %u nCK", len);
			if (!r->run_stable)
				viol(m, s, (int)r->cs, "CA not static while CS_n LOW (multicycle MPC)");
			for (int i = 1; i <= T.tmc_setup; i++)
				if (sb->ca_hist[(r->run_start - i) & 7] != c)
					viol(m, s, (int)r->cs, "tMC_MPC_Setup: CA not stable before CS_n LOW");
			r->hold_left = (int)T.tmc_hold;
			r->hold_ca = c;
			int64_t keep = m->now;
			m->now = now - 1;
			execute(m, s, r, c, 0, now - 1);
			m->now = keep;
		}
		return;
	}
	if (!cs_low)
		return;
	if (r->st == R_CFG && now < r->t_cfg_ok && (ca & 31) != 31)
		viol(m, s, (int)r->cs, "command before tXPR");
	if (!(ca & 2)) {				/* CA1 = L: two-cycle */
		r->pend = 1;
		r->pend_ca = ca;
		r->pend_t = now;
		return;
	}
	if (multi && is_mc(ca)) {
		r->run = 1;
		r->run_ca = ca;
		r->run_stable = 1;
		r->run_start = now;
		return;
	}
	if (r->st == R_CFG && (ca & 31) != 0x0F && (ca & 31) != 0x03 && (ca & 31) != 0x1F)
		viol(m, s, (int)r->cs, "only MRR/MRW/MPC/VREF are legal before init completes");
	execute(m, s, r, ca, 0, now);
}

static void
housekeeping(struct ddr5_mem *m, int s, struct rank *r)
{
	/* auto precharge, refresh debt */
	if (r->st != R_READY)
		return;
	for (unsigned g = 0; g < 8; g++)
		for (unsigned b = 0; b < 4; b++) {
			struct bank *k = &r->bk[g][b];
			if (k->open && k->ap_at != NEVER && m->now >= k->ap_at)
				do_pre(m, s, r, g, b, m->now);
		}
	if (r->t_ref == NEVER)
		r->t_ref = m->now;		/* the refresh clock starts at READY */
	if (!r->flag_ref && m->now - r->t_ref > 5 * T.trefi + T.trefi / 2) {
		viol(m, s, (int)r->cs, "refresh overdue: no REF for %lld nCK (more than 4 postponed)",
		    (long long)(m->now - r->t_ref));
		r->flag_ref = 1;
	}
}

static void
sub_clock(struct ddr5_mem *m, int s, const struct ddr5_sub_in *in, struct ddr5_sub_out *out,
    int reset_n)
{
	struct sub *sb = &m->sub[s];
	uint32_t ca = in->ca & 0x3FFF;

	if (!reset_n && in->dq_oe)
		viol(m, s, -1, "the controller drives DQ while RESET_n is LOW");
	for (unsigned c = 0; c < DDR5_RANKS; c++) {
		struct rank *r = &sb->rk[c];
		if (!r->present)
			continue;
		rank_clock(m, s, r, !(in->cs_n >> c & 1), ca, reset_n);
		housekeeping(m, s, r);
	}
	sb->ca_hist[m->now & 7] = ca;

	/* the data bus at this clock */
	memset(out, 0, sizeof *out);
	struct beat2 *e = &sb->ring[m->now % RING];
	if (e->valid) {
		struct rank *r = &sb->rk[e->cs];
		if (e->kind == 1) {
			if (in->dq_oe)
				viol(m, s, (int)e->cs, "bus contention: the controller drives DQ during read data");
			put80(out->dq_i, e->data[0], e->data[1]);
			out->dqs_i = 3;
		} else {
			if (!in->dq_oe || (in->dqs_o & 3) != 3)
				viol(m, s, (int)e->cs, "write data missing (dq_oe %d, dqs_o %u)", in->dq_oe,
				    in->dqs_o & 3);
			for (unsigned bt = 0; bt < 2; bt++)
				if (in->dq_oe && (in->dqs_o >> bt & 1))
					beat_wr(m, r, e->bg, e->ba, e->row, e->col + bt, get40(in->dq_o, bt));
		}
		e->valid = 0;
	}
}

/* ---------------------------------------------------------------- SPD hubs */

/*
 * One SMBus segment, the four slots' SPD5 hubs at 50h + slot (JESD300-5,
 * SPD5118): I2C legacy mode with 1-byte addressing. An address byte with
 * bit 7 = 1 (MemReg) selects NVM byte MR11[2:0] * 128 + (addr & 7Fh), bit 7
 * = 0 a hub register (MR0/MR1 = 51h 18h: device type SPD5118; MR11: page;
 * the others read 0). NVM is write-protected (writes are acknowledged and
 * dropped); register writes other than MR11 are dropped. Reads auto-
 * increment within the addressed 128 bytes. The hubs never stretch SCL; any
 * SCL rate works (bits are sampled on SCL edges).
 */
enum { I_IDLE, I_ADDR, I_WDATA, I_SACK, I_SEND, I_MACK, I_IGNORE };

static uint8_t
hub_read(struct ddr5_mem *m, int t, uint8_t off)
{
	if (off & 0x80)
		return m->spd[t][(m->hub[t].mr[11] & 7) * 128 + (off & 0x7F)];
	return m->hub[t].mr[off & 0x7F];
}

static void
hub_write(struct ddr5_mem *m, int t, uint8_t off, uint8_t v)
{
	if (!(off & 0x80) && off == 11)
		m->hub[t].mr[11] = (uint8_t)((m->hub[t].mr[11] & ~0x0Fu) | (v & 0x07));
}

static void
spd_eval(struct ddr5_mem *m, int scl_o, int sda_o, bool *scl_i, bool *sda_i)
{
	int scl = scl_o;
	int sda = sda_o && !m->pull;
	if (scl && m->scl_p) {
		if (m->sda_p && !sda) {			/* START (or repeated START) */
			m->i2c_on = 1;
			m->phase = I_ADDR;
			m->bits = 0;
			m->sh = 0;
			m->pull = 0;
		} else if (!m->sda_p && sda) {		/* STOP */
			m->i2c_on = 0;
			m->phase = I_IDLE;
			m->pull = 0;
		}
	} else if (scl && !m->scl_p && m->i2c_on) {	/* rising: sample */
		if (m->phase == I_ADDR || m->phase == I_WDATA) {
			m->sh = (uint8_t)(m->sh << 1 | sda);
			m->bits++;
		} else if (m->phase == I_MACK)
			m->ack = !sda;
	} else if (!scl && m->scl_p && m->i2c_on) {	/* falling: advance */
		switch (m->phase) {
		case I_ADDR:
		case I_WDATA:
			if (m->bits < 8)
				break;
			if (m->phase == I_ADDR) {
				unsigned a = m->sh >> 1;
				m->rw = m->sh & 1;
				m->tgt = (a & 0x7C) == 0x50 && m->cfg[a & 3].present ? (int)(a & 3) : -1;
				m->wfirst = 1;
			} else if (m->wfirst) {
				m->offs = m->sh;
				m->wfirst = 0;
			} else {
				hub_write(m, m->tgt, m->offs, m->sh);
				m->offs = (uint8_t)((m->offs & 0x80) | ((m->offs + 1) & 0x7F));
			}
			if (m->tgt < 0) {
				m->phase = I_IGNORE;	/* NACK: nobody pulls */
				break;
			}
			m->pull = 1;
			m->phase = I_SACK;
			break;
		case I_SACK:
			m->pull = 0;
			m->bits = 0;
			m->sh = 0;
			if (m->rw) {
				m->sh = hub_read(m, m->tgt, m->offs);
				m->offs = (uint8_t)((m->offs & 0x80) | ((m->offs + 1) & 0x7F));
				m->pull = !(m->sh & 0x80);
				m->phase = I_SEND;
			} else
				m->phase = I_WDATA;
			break;
		case I_SEND:
			m->bits++;
			if (m->bits < 8)
				m->pull = !((m->sh << m->bits) & 0x80);
			else {
				m->pull = 0;
				m->phase = I_MACK;
			}
			break;
		case I_MACK:
			if (m->ack) {
				m->sh = hub_read(m, m->tgt, m->offs);
				m->offs = (uint8_t)((m->offs & 0x80) | ((m->offs + 1) & 0x7F));
				m->bits = 0;
				m->pull = !(m->sh & 0x80);
				m->phase = I_SEND;
			} else
				m->phase = I_IGNORE;
			break;
		default:
			break;
		}
	}
	m->scl_p = scl;
	m->sda_p = sda;
	*scl_i = scl;
	*sda_i = sda_o && !m->pull;
}

/* ---------------------------------------------------------------- API */

void
ddr5_default_cfg(struct ddr5_slot_cfg cfg[DDR5_SLOTS])
{
	memset(cfg, 0, sizeof(struct ddr5_slot_cfg) * DDR5_SLOTS);
	cfg[0].present = 1;
	cfg[0].size_gib = 16;
	cfg[0].ecc = 1;
	cfg[0].speed = 4800;
}

int
ddr5_parse_slot(const char *s, struct ddr5_slot_cfg cfg[DDR5_SLOTS])
{
	char buf[64], *p, *tok;
	struct ddr5_slot_cfg c = { 1, 16, 1, 4800, 0 };
	if (strlen(s) >= sizeof buf)
		return -1;
	strcpy(buf, s);
	p = buf;
	tok = strsep(&p, ":");
	long slot = strtol(tok, &tok, 10);
	if (*tok || slot < 0 || slot >= DDR5_SLOTS || !p)
		return -1;
	tok = strsep(&p, ":");
	c.size_gib = (unsigned)strtoul(tok, &tok, 10);
	if (*tok == 'G' || *tok == 'g')
		tok++;
	if (*tok || (c.size_gib != 16 && c.size_gib != 32 && c.size_gib != 64))
		return -1;
	while (p) {
		tok = strsep(&p, ":");
		if (!strcmp(tok, "noecc"))
			c.ecc = 0;
		else if (!strcmp(tok, "ecc"))
			c.ecc = 1;
		else if (!strcmp(tok, "4800"))
			c.speed = 4800;
		else if (!strcmp(tok, "badspd"))
			c.bad_spd = 1;
		else
			return -1;
	}
	cfg[slot] = c;
	return 0;
}

struct ddr5_mem *
ddr5_new(const struct ddr5_slot_cfg cfg[DDR5_SLOTS], int fast, uint64_t seed)
{
	if (!T.trcd)
		tim_init();
	struct ddr5_mem *m = calloc(1, sizeof *m);
	if (!m)
		return NULL;
	memcpy(m->cfg, cfg, sizeof m->cfg);
	m->fast = fast;
	m->seed = seed;
	m->trace = getenv("VSIM_DDR5_TRACE") != NULL;
	m->tgt = -1;
	unsigned mask = 0;
	for (int i = 0; i < DDR5_SLOTS; i++)
		if (cfg[i].present)
			mask |= 1u << i;
	/* uniform population: {0}, {0,1}, {0,2}, {0,1,2,3}, all alike */
	if (mask != 1 && mask != 3 && mask != 5 && mask != 15) {
		fprintf(stderr, "ddr5: populate slots 0; 0,1; 0,2; or all four (got mask %x)\n", mask);
		free(m);
		return NULL;
	}
	for (int i = 1; i < DDR5_SLOTS; i++)
		if (cfg[i].present && (cfg[i].size_gib != cfg[0].size_gib || cfg[i].ecc != cfg[0].ecc)) {
			fprintf(stderr, "ddr5: every populated slot must have the same DIMM\n");
			free(m);
			return NULL;
		}
	m->nchan = (mask & 4) ? 2 : 1;
	m->ndimm = (mask & 2) ? 2 : 1;
	for (int k = 0; k < SPD_NGEOMS; k++)
		if (spd_geoms[k].size_gib == cfg[0].size_gib)
			m->g = &spd_geoms[k];
	if (!m->g || m->g->devw != 8) {
		fprintf(stderr, "ddr5: %u GiB: not an x8 module\n", cfg[0].size_gib);
		free(m);
		return NULL;
	}
	for (int i = 0; i < DDR5_SLOTS; i++) {
		if (!cfg[i].present)
			continue;
		spd_build(m->spd[i], m->g, &spd_speeds[0], cfg[i].ecc);
		if (cfg[i].bad_spd)
			m->spd[i][510] ^= 0xFF;
		m->hub[i].mr[0] = 0x51;		/* SPD5118 device type */
		m->hub[i].mr[1] = 0x18;
	}
	for (int s = 0; s < DDR5_SUBS; s++)
		for (int c = 0; c < DDR5_RANKS; c++) {
			struct rank *r = &m->sub[s].rk[c];
			unsigned slot = (unsigned)(s / 2 * 2 + c / 2);
			r->present = cfg[slot].present && (unsigned)(c & 1) < m->g->ranks;
			r->slot = slot;
			r->cs = (unsigned)c;
			r->g = m->g;
			r->ecc = cfg[slot].ecc;
			r->st = R_RESET;
			r->t_reset_low = 0;
			r->cs_low_since = NEVER;
			r->t_act = r->t_rd = r->t_wr = r->t_pre = r->t_mrw = r->t_mpc = NEVER;
			r->t_dll = NEVER;
			r->t_ref = NEVER;
			for (int g = 0; g < 8; g++) {
				r->t_act_bg[g] = r->t_rd_bg[g] = r->t_wr_bg[g] = NEVER;
				for (int b = 0; b < 4; b++) {
					struct bank *k = &r->bk[g][b];
					k->t_act = k->t_pre = k->t_rd = k->t_wr_end = NEVER;
					k->ap_at = NEVER;
					k->sb_until = NEVER;
				}
			}
			for (int i = 0; i < 4; i++)
				r->act_ring[i] = NEVER;
		}
	for (int c = 0; c < DDR5_CHANNELS; c++)
		m->reset_prev[c] = 0;
	m->scl_p = m->sda_p = 1;
	return m;
}

void
ddr5_free(struct ddr5_mem *m)
{
	if (!m)
		return;
	for (int s = 0; s < DDR5_SUBS; s++)
		for (int c = 0; c < DDR5_RANKS; c++) {
			struct pagetab *t = &m->sub[s].rk[c].pt;
			for (size_t i = 0; i < t->cap; i++)
				free(t->pages[i]);
			free(t->keys);
			free(t->pages);
		}
	free(m);
}

void
ddr5_eval(struct ddr5_mem *m, const struct ddr5_pins_in *in, struct ddr5_pins_out *out,
    uint64_t cycle)
{
	m->now = (int64_t)cycle;
	for (int s = 0; s < DDR5_SUBS; s++)
		sub_clock(m, s, &in->sub[s], &out->sub[s], in->reset_n[s / 2]);
	for (int c = 0; c < DDR5_CHANNELS; c++)
		out->alert_n[c] = 1;
	spd_eval(m, in->spd_scl_o, in->spd_sda_o, &out->spd_scl_i, &out->spd_sda_i);
}

/* ---------------------------------------------------------------- the map */

uint64_t
ddr5_size(const struct ddr5_mem *m)
{
	return ((uint64_t)m->g->size_gib << 30) * (uint64_t)(m->nchan * m->ndimm);
}

int
ddr5_map(const struct ddr5_mem *m, uint64_t off, struct ddr5_loc *l)
{
	if (off >= ddr5_size(m))
		return -1;
	unsigned sb = m->nchan == 2 ? 2 : 1;	/* subchannel field bits */
	l->byte = (unsigned)(off & 3);
	unsigned beat = (unsigned)(off >> 2 & 15);
	uint64_t v = off >> 6;
	unsigned sub = (unsigned)(v & ((1u << sb) - 1));
	v >>= sb;
	unsigned colblk = (unsigned)(v & 63);
	v >>= 6;
	l->bg = (unsigned)(v & 7);
	v >>= 3;
	l->ba = (unsigned)(v & 3);
	v >>= 2;
	l->row = (unsigned)(v & ((1u << m->g->rows) - 1));
	v >>= m->g->rows;
	unsigned rank = 0, dimm = 0;
	if (m->g->ranks == 2) {
		rank = (unsigned)(v & 1);
		v >>= 1;
	}
	if (m->ndimm == 2)
		dimm = (unsigned)(v & 1);
	l->sub = sub;				/* 0..3 = ch*2 + subchannel */
	l->cs = dimm * 2 + rank;
	l->col = colblk * 16 + beat;
	return 0;
}

uint8_t
ddr5_hsiao(uint32_t data)
{
	/* column of data bit i: the i-th weight-3 value of 7 bits, increasing */
	static uint8_t col[32];
	if (!col[0]) {
		int n = 0;
		for (unsigned v = 0; v < 128 && n < 32; v++)
			if (__builtin_popcount(v) == 3)
				col[n++] = (uint8_t)v;
	}
	uint8_t cb = 0;
	for (int i = 0; i < 32; i++)
		if (data >> i & 1)
			cb ^= col[i];
	return cb;
}

int
ddr5_phys(struct ddr5_mem *m, int we, unsigned sub, unsigned cs, unsigned bg, unsigned ba,
    unsigned row, unsigned col, unsigned dev, uint8_t *byte)
{
	if (sub >= DDR5_SUBS || cs >= DDR5_RANKS || bg > 7 || ba > 3 || col >= COLS || dev >= DEVS)
		return -1;
	struct rank *r = &m->sub[sub].rk[cs];
	if (!r->present || row >> r->g->rows)
		return -1;
	if (we)
		wr_byte(m, r, bg, ba, row, col, dev, *byte);
	else
		*byte = rd_byte(m, r, bg, ba, row, col, dev);
	return 0;
}

int
ddr5_backdoor(struct ddr5_mem *m, int we, uint64_t off, uint8_t *buf, uint64_t n)
{
	if (off + n > ddr5_size(m) || off + n < off)
		return -1;
	for (uint64_t i = 0; i < n; i++) {
		struct ddr5_loc l;
		ddr5_map(m, off + i, &l);
		struct rank *r = &m->sub[l.sub].rk[l.cs];
		if (!we) {
			buf[i] = rd_byte(m, r, l.bg, l.ba, l.row, l.col, l.byte);
			continue;
		}
		wr_byte(m, r, l.bg, l.ba, l.row, l.col, l.byte, buf[i]);
		/* the beat's ECC device byte: Hsiao check bits of the 4 data bytes */
		uint32_t d = 0;
		for (unsigned b = 0; b < 4; b++)
			d |= (uint32_t)rd_byte(m, r, l.bg, l.ba, l.row, l.col, b) << (8 * b);
		wr_byte(m, r, l.bg, l.ba, l.row, l.col, 4, ddr5_hsiao(d));
	}
	return 0;
}

uint64_t
ddr5_violations(const struct ddr5_mem *m)
{
	return m->viol;
}

const char *
ddr5_first_violation(const struct ddr5_mem *m)
{
	return m->first;
}

int
ddr5_rank_state(const struct ddr5_mem *m, unsigned sub, unsigned cs)
{
	return m->sub[sub].rk[cs].st;
}

uint8_t
ddr5_mr(const struct ddr5_mem *m, unsigned sub, unsigned cs, unsigned mr)
{
	return m->sub[sub].rk[cs].mr[mr & 0xFF];
}

const uint8_t *
ddr5_spd(const struct ddr5_mem *m, unsigned slot)
{
	return slot < DDR5_SLOTS && m->cfg[slot].present ? m->spd[slot] : NULL;
}

void
ddr5_stats(const struct ddr5_mem *m, char *buf, unsigned len)
{
	uint64_t a = 0, rd = 0, wr = 0, ref = 0;
	for (int s = 0; s < DDR5_SUBS; s++)
		for (int c = 0; c < DDR5_RANKS; c++) {
			const struct rank *r = &m->sub[s].rk[c];
			a += r->n_act;
			rd += r->n_rd;
			wr += r->n_wr;
			ref += r->n_ref;
		}
	snprintf(buf, len, "ddr5: %llu ACT, %llu RD, %llu WR, %llu REF, %llu violations",
	    (unsigned long long)a, (unsigned long long)rd, (unsigned long long)wr,
	    (unsigned long long)ref, (unsigned long long)m->viol);
}
