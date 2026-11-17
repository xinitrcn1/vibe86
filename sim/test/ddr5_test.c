/*
 * ddr5_test: the pin-level DDR5 DIMM model (sim/ddr5.c) driven the way a
 * memory controller drives it. Command encodings: Micron DDR5 SDRAM core
 * data sheet Table 140; the power-up sequence: Table 10 and the text before
 * it. Every check prints PASS/FAIL; exit status 1 on any failure.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ddr5.h"

static int fails;
#define CHECK(c, ...) do { if (c) printf("PASS: " __VA_ARGS__); \
	else { printf("FAIL: " __VA_ARGS__); fails++; } printf("\n"); } while (0)

struct drv {
	struct ddr5_mem *m;
	struct ddr5_pins_in in;
	struct ddr5_pins_out out;
	uint64_t cyc;
	int twoN;
	/* captured read data: up to 16 beats per subchannel */
	uint64_t rd[DDR5_SUBS][16];
	int nrd[DDR5_SUBS];
};

static void
clk(struct drv *d)
{
	ddr5_eval(d->m, &d->in, &d->out, d->cyc++);
	for (int s = 0; s < DDR5_SUBS; s++)
		for (int b = 0; b < 2; b++)
			if (d->out.sub[s].dqs_i >> b & 1 && d->nrd[s] < 16) {
				const uint32_t *w = d->out.sub[s].dq_i;
				uint64_t lo = (uint64_t)w[0] | (uint64_t)w[1] << 32;
				uint64_t v = b ? (lo >> 40) | ((uint64_t)w[2] << 24) : lo;
				d->rd[s][d->nrd[s]++] = v & 0xFFFFFFFFFFull;
			}
}

static void
idle(struct drv *d, int n)
{
	for (int s = 0; s < DDR5_SUBS; s++) {
		d->in.sub[s].cs_n = 0xF;
		d->in.sub[s].dq_oe = 0;
		d->in.sub[s].dqs_o = 0;
	}
	while (n-- > 0)
		clk(d);
}

/* one-cycle command on subchannel s, rank cs */
static void
cmd1(struct drv *d, int s, int cs, uint32_t ca)
{
	d->in.sub[s].ca = ca;
	d->in.sub[s].cs_n = 0xF & ~(1u << cs);
	clk(d);
	d->in.sub[s].cs_n = 0xF;
}

/* two-cycle command: the second half one (1N) or two (2N) clocks later */
static void
cmd2(struct drv *d, int s, int cs, uint32_t c1, uint32_t c2)
{
	d->in.sub[s].ca = c1;
	d->in.sub[s].cs_n = 0xF & ~(1u << cs);
	clk(d);
	d->in.sub[s].cs_n = 0xF;
	if (d->twoN)
		clk(d);
	d->in.sub[s].ca = c2;
	clk(d);
}

/* multicycle MPC (MR2:OP[4] = 0): CA static 3 clocks, CS_n LOW 4, CA static 3 */
static void
mpc_multi(struct drv *d, int s, int cs, unsigned op)
{
	uint32_t ca = 0x0F | op << 5;
	d->in.sub[s].ca = ca;
	idle(d, 3);
	d->in.sub[s].cs_n = 0xF & ~(1u << cs);
	for (int i = 0; i < 4; i++)
		clk(d);
	d->in.sub[s].cs_n = 0xF;
	idle(d, 3);
}

static uint32_t ca_act(unsigned bg, unsigned ba, unsigned row) { return (row & 15) << 2 | ba << 6 | bg << 8; }
static uint32_t ca_act2(unsigned row) { return (row >> 4) & 0x1FFF; }
static uint32_t ca_rd(unsigned bg, unsigned ba) { return 0x1D | 1u << 5 | ba << 6 | bg << 8; }
static uint32_t ca_rd2(unsigned col, int ap) { return ((col >> 2) & 0x1FF) | (ap ? 0 : 1u) << 10; }
static uint32_t ca_wr(unsigned bg, unsigned ba) { return 0x0D | 1u << 5 | ba << 6 | bg << 8; }
static uint32_t ca_wr2(unsigned col, int ap) { return ((col >> 3) & 0xFF) << 1 | (ap ? 0 : 1u) << 10 | 1u << 11; }
static uint32_t ca_mrw(unsigned mra) { return 0x05 | mra << 5; }
static uint32_t ca_mrr(unsigned mra) { return 0x15 | mra << 5; }
#define CA_PREAB	0x0B
#define CA_REFAB	(0x13 | 1u << 9)
#define CA_NOP		0x3FFF
static uint32_t ca_prepb(unsigned bg, unsigned ba) { return 0x1B | ba << 6 | bg << 8; }

/* the power-up sequence on every subchannel (fast strap) */
static void
power_up(struct drv *d)
{
	for (int s = 0; s < DDR5_SUBS; s++)
		d->in.sub[s].cs_n = 0;			/* CS_n LOW from before RESET_n rises */
	d->in.reset_n[0] = d->in.reset_n[1] = 0;
	for (int i = 0; i < 600; i++)			/* tINIT1/1024 = 469 */
		clk(d);
	d->in.reset_n[0] = d->in.reset_n[1] = 1;
	for (int i = 0; i < 9500; i++)			/* tINIT3/1024 = 9362 */
		clk(d);
	idle(d, 10);					/* tINIT4/1024 = 5 */
	for (int s = 0; s < DDR5_SUBS; s++) {
		d->in.sub[s].ca = CA_NOP;
		d->in.sub[s].cs_n = 0;
	}
	for (int i = 0; i < 5; i++)			/* NOPs: tINIT5 = 3 */
		clk(d);
	idle(d, 10);					/* tXPR/1024 */
}

static void
configure(struct drv *d, int s, int cs)
{
	mpc_multi(d, s, cs, 0x80);			/* tDLLK/tCCD_L into MR13 */
	idle(d, 40);
	mpc_multi(d, s, cs, 0x02);			/* DLL RESET */
	idle(d, 40);
	mpc_multi(d, s, cs, 0x05);			/* ZQCal Start */
	idle(d, 40);
	mpc_multi(d, s, cs, 0x04);			/* ZQCal Latch */
	idle(d, 80);
	cmd2(d, s, cs, ca_mrw(0), 0x09 << 2);		/* MR0: BL16, CL40 */
	idle(d, 40);
}

static struct drv *
mk(int slots_mask, unsigned size)
{
	struct ddr5_slot_cfg cfg[DDR5_SLOTS] = { 0 };
	for (int i = 0; i < DDR5_SLOTS; i++)
		if (slots_mask >> i & 1)
			cfg[i] = (struct ddr5_slot_cfg){ 1, size, 1, 4800, 0 };
	struct drv *d = calloc(1, sizeof *d);
	d->m = ddr5_new(cfg, 1, 42);
	d->twoN = 1;
	for (int s = 0; s < DDR5_SUBS; s++)
		d->in.sub[s].cs_n = 0xF;
	d->in.spd_scl_o = d->in.spd_sda_o = 1;
	return d;
}

static void
drop(struct drv *d)
{
	ddr5_free(d->m);
	free(d);
}

static uint64_t
pattern(unsigned i, unsigned salt)
{
	uint32_t data = 0x9E3779B9u * (i + 1) ^ salt;
	return (uint64_t)data | (uint64_t)ddr5_hsiao(data) << 32;
}

/* WR at (bg, ba, col) with 16 beats of pattern(salt); returns the cycle of its first half */
static uint64_t
write_burst(struct drv *d, int s, int cs, unsigned bg, unsigned ba, unsigned col, unsigned salt)
{
	uint64_t t0 = d->cyc;
	cmd2(d, s, cs, ca_wr(bg, ba), ca_wr2(col, 0));
	uint64_t tf = t0 + (d->twoN ? 2 : 1);
	while (d->cyc < tf + 38)			/* CWL = CL - 2 = 38 */
		clk(d);
	for (unsigned k = 0; k < 8; k++) {
		uint32_t w[3];
		uint64_t b0 = pattern(2 * k, salt), b1 = pattern(2 * k + 1, salt);
		w[0] = (uint32_t)b0;
		w[1] = (uint32_t)(b0 >> 32) | (uint32_t)(b1 << 8);
		w[2] = (uint32_t)(b1 >> 24);
		memcpy(d->in.sub[s].dq_o, w, sizeof w);
		d->in.sub[s].dq_oe = 1;
		d->in.sub[s].dqs_o = 3;
		clk(d);
	}
	d->in.sub[s].dq_oe = 0;
	d->in.sub[s].dqs_o = 0;
	return t0;
}

/* RD; waits for and returns the number of beats captured */
static int
read_burst(struct drv *d, int s, int cs, unsigned bg, unsigned ba, unsigned col, int ap)
{
	d->nrd[s] = 0;
	cmd2(d, s, cs, ca_rd(bg, ba), ca_rd2(col, ap));
	idle(d, 40 + 8 + 2);
	return d->nrd[s];
}

static int
match(struct drv *d, int s, unsigned salt)
{
	for (unsigned i = 0; i < 16; i++)
		if (d->rd[s][i] != pattern(i, salt))
			return 0;
	return 1;
}

/* ---------------------------------------------------------------- I2C */

static int half = 3;

static void
bus(struct drv *d, int scl, int sda)
{
	d->in.spd_scl_o = scl;
	d->in.spd_sda_o = sda;
	for (int i = 0; i < half; i++)
		clk(d);
}

static void i2c_start(struct drv *d) { bus(d, 1, 1); bus(d, 1, 0); bus(d, 0, 0); }
static void i2c_stop(struct drv *d) { bus(d, 0, 0); bus(d, 1, 0); bus(d, 1, 1); }

static int
i2c_wbyte(struct drv *d, unsigned v)
{
	for (int i = 7; i >= 0; i--) {
		int b = v >> i & 1;
		bus(d, 0, b);
		bus(d, 1, b);
		bus(d, 0, b);
	}
	bus(d, 0, 1);
	bus(d, 1, 1);
	int ack = !d->out.spd_sda_i;
	bus(d, 0, 1);
	return ack;
}

static unsigned
i2c_rbyte(struct drv *d, int ack)
{
	unsigned v = 0;
	for (int i = 0; i < 8; i++) {
		bus(d, 0, 1);
		bus(d, 1, 1);
		v = v << 1 | d->out.spd_sda_i;
		bus(d, 0, 1);
	}
	bus(d, 0, !ack);
	bus(d, 1, !ack);
	bus(d, 0, !ack);
	return v;
}

static int
spd_byte(struct drv *d, unsigned hid, unsigned off, unsigned *v)
{
	unsigned a = 0x50 | hid;
	i2c_start(d);
	if (!i2c_wbyte(d, a << 1)) {
		i2c_stop(d);
		return -1;
	}
	i2c_wbyte(d, 0x0B);			/* MR11: page */
	i2c_wbyte(d, off >> 7);
	i2c_stop(d);
	i2c_start(d);
	i2c_wbyte(d, a << 1);
	i2c_wbyte(d, 0x80 | (off & 0x7F));
	i2c_start(d);				/* repeated START */
	i2c_wbyte(d, a << 1 | 1);
	*v = i2c_rbyte(d, 0);
	i2c_stop(d);
	return 0;
}

/* ---------------------------------------------------------------- tests */

static int
has(struct drv *d, const char *what)
{
	return strstr(ddr5_first_violation(d->m), what) != NULL;
}

int
main(void)
{
	/* Hsiao (39,32): data bit i's column = the i-th weight-3 7-bit value */
	CHECK(ddr5_hsiao(1) == 0x07 && ddr5_hsiao(2) == 0x0B && ddr5_hsiao(3) == 0x0C &&
	    ddr5_hsiao(0x80000000u) == 0x62 && ddr5_hsiao(0) == 0, "Hsiao check bits (vectors)");

	/* -------- a full sequence, 2 channels x 2 x 32 GiB 2Rx8 */
	struct drv *d = mk(0xF, 32);
	CHECK(d->m != NULL && ddr5_size(d->m) == 128ull << 30, "4 x 32 GiB: 128 GiB of DRAM");
	power_up(d);
	CHECK(ddr5_violations(d->m) == 0, "power-up sequence clean (%s)", ddr5_first_violation(d->m));
	CHECK(ddr5_rank_state(d->m, 0, 0) == 2, "rank in configuration state after reset exit");
	for (int s = 0; s < DDR5_SUBS; s++)
		for (int cs = 0; cs < 4; cs++)
			configure(d, s, cs);
	idle(d, 10);
	CHECK(ddr5_violations(d->m) == 0, "MPC DLL reset, ZQCal, MRW CL40 clean (%s)",
	    ddr5_first_violation(d->m));
	CHECK(ddr5_rank_state(d->m, 1, 3) == 3 && ddr5_mr(d->m, 1, 3, 0) == 0x24,
	    "ranks ready, MR0 = 24h (BL16, CL40)");

	/* MRR of MR0: UI 8-15 carry OP[7:0], odd DQs inverted */
	d->nrd[0] = 0;
	cmd2(d, 0, 0, ca_mrr(0), 0);
	idle(d, 52);
	int ok = d->nrd[0] == 16;
	for (int ui = 0; ui < 16 && ok; ui++) {
		uint64_t want = 0;
		if (ui >= 8)
			for (int dq = 0; dq < 40; dq++)
				want |= (uint64_t)((0x24 >> (ui - 8) & 1) ^ (dq & 1)) << dq;
		ok = d->rd[0][ui] == want;
	}
	CHECK(ok, "MRR MR0 data layout");
	idle(d, 40);

	/* ACT, WR, RD, PRE on several banks and ranks (2N mode) */
	cmd2(d, 0, 0, ca_act(0, 0, 0x1234), ca_act2(0x1234));
	idle(d, 8);
	cmd2(d, 0, 0, ca_act(1, 2, 0x0042), ca_act2(0x0042));
	idle(d, 40);
	write_burst(d, 0, 0, 0, 0, 0x30, 1);
	idle(d, 12);
	write_burst(d, 0, 0, 1, 2, 0x100, 2);
	idle(d, 80);
	CHECK(read_burst(d, 0, 0, 0, 0, 0x30, 0) == 16 && match(d, 0, 1), "write/read BG0 BA0 row 1234h");
	idle(d, 12);
	CHECK(read_burst(d, 0, 0, 1, 2, 0x100, 1) == 16 && match(d, 0, 2), "write/read BG1 BA2 + auto precharge");
	idle(d, 100);
	cmd1(d, 0, 0, ca_prepb(0, 0));
	idle(d, 40);
	CHECK(ddr5_violations(d->m) == 0, "traffic clean (%s)", ddr5_first_violation(d->m));

	/* rank 1 of DIMM 1 on channel 1 subchannel b, then refresh */
	cmd2(d, 3, 3, ca_act(7, 3, 0xFFFF), ca_act2(0xFFFF));
	idle(d, 40);
	write_burst(d, 3, 3, 7, 3, 0x3F0, 3);
	idle(d, 80);
	CHECK(read_burst(d, 3, 3, 7, 3, 0x3F0, 0) == 16 && match(d, 3, 3), "write/read sub 3 cs 3 BG7 BA3 row FFFFh col 3F0h");
	idle(d, 100);
	cmd1(d, 3, 3, CA_PREAB);
	idle(d, 40);
	cmd1(d, 3, 3, CA_REFAB);
	idle(d, 720);					/* tRFC1 = 708 */
	cmd2(d, 3, 3, ca_act(7, 3, 0xFFFF), ca_act2(0xFFFF));
	idle(d, 40);
	CHECK(read_burst(d, 3, 3, 7, 3, 0x3F0, 1) == 16 && match(d, 3, 3), "data kept across REFab");
	idle(d, 100);
	CHECK(ddr5_violations(d->m) == 0, "refresh clean (%s)", ddr5_first_violation(d->m));

	/* 1N mode */
	mpc_multi(d, 2, 1, 0x09);			/* SET 1N */
	idle(d, 40);
	d->twoN = 0;
	/* (only rank 1 of subchannel 2 is in 1N: use it alone) */
	cmd2(d, 2, 1, ca_act(3, 1, 0x777), ca_act2(0x777));
	idle(d, 40);
	write_burst(d, 2, 1, 3, 1, 0x200, 4);
	idle(d, 80);
	CHECK(read_burst(d, 2, 1, 3, 1, 0x200, 1) == 16 && match(d, 2, 4), "1N mode write/read");
	idle(d, 100);
	CHECK(ddr5_violations(d->m) == 0, "1N clean (%s)", ddr5_first_violation(d->m));
	d->twoN = 1;

	/* backdoor: preload by DRAM offset, read through the pins */
	uint8_t buf[64];
	for (int i = 0; i < 64; i++)
		buf[i] = (uint8_t)(0xA0 + i);
	/* offset: subchannel 1, column block 5, BG 2, BA 1, row 0x0ABC, rank 1, DIMM 1 */
	uint64_t off = (uint64_t)1 << 6 | 5ull << 8 | 2ull << 14 | 1ull << 17 | 0xABCull << 19 |
	    1ull << 35 | 1ull << 36;
	CHECK(ddr5_backdoor(d->m, 1, off, buf, 64) == 0, "backdoor write");
	struct ddr5_loc l;
	ddr5_map(d->m, off, &l);
	CHECK(l.sub == 1 && l.cs == 3 && l.bg == 2 && l.ba == 1 && l.row == 0xABC && l.col == 80,
	    "map: sub 1 cs 3 BG2 BA1 row ABCh col 80");
	cmd2(d, 1, 3, ca_act(2, 1, 0xABC), ca_act2(0xABC));
	idle(d, 40);
	read_burst(d, 1, 3, 2, 1, 80, 1);
	ok = d->nrd[1] == 16;
	for (int b = 0; b < 16 && ok; b++) {
		uint32_t w = (uint32_t)buf[4 * b] | (uint32_t)buf[4 * b + 1] << 8 |
		    (uint32_t)buf[4 * b + 2] << 16 | (uint32_t)buf[4 * b + 3] << 24;
		ok = d->rd[1][b] == ((uint64_t)w | (uint64_t)ddr5_hsiao(w) << 32);
	}
	CHECK(ok, "backdoor data and Hsiao CB read through the pins");
	idle(d, 100);
	CHECK(ddr5_violations(d->m) == 0, "backdoor sequence clean (%s)", ddr5_first_violation(d->m));
	char st[128];
	ddr5_stats(d->m, st, sizeof st);
	printf("     %s\n", st);

	/* SPD over the SMBus, two SCL rates */
	for (half = 3; half >= 1; half -= 2) {
		unsigned v = 0, v2 = 0, pn = 0;
		int r0 = spd_byte(d, 0, 2, &v);
		int r1 = spd_byte(d, 3, 235, &v2);
		spd_byte(d, 1, 521, &pn);
		CHECK(r0 == 0 && v == 0x12 && r1 == 0 && v2 == ddr5_spd(d->m, 3)[235] && pn == 'V',
		    "SPD reads (SCL half period %d clocks): byte 2 = %02Xh, slot 3 byte 235 = %02Xh, PN '%c'",
		    half, v, v2, pn);
	}
	drop(d);

	/* an absent slot NACKs */
	d = mk(0x1, 16);
	half = 2;
	unsigned v;
	CHECK(spd_byte(d, 1, 0, &v) == -1, "absent slot 51h NACKs its address");
	CHECK(spd_byte(d, 0, 4, &v) == 0 && v == ddr5_spd(d->m, 0)[4], "slot 0 present (byte 4 = %02Xh)", v);
	CHECK(ddr5_size(d->m) == 16ull << 30, "1 x 16 GiB 1Rx8");
	drop(d);

	/* non-uniform population is refused */
	{
		struct ddr5_slot_cfg cfg[DDR5_SLOTS] = { { 1, 16, 1, 4800, 0 }, { 1, 32, 1, 4800, 0 } };
		struct ddr5_mem *m = ddr5_new(cfg, 1, 1);
		CHECK(m == NULL, "mixed 16 GiB + 32 GiB refused");
	}

	/* -------- adversarial: each must be flagged */
	d = mk(0x1, 16);
	power_up(d);
	cmd2(d, 0, 0, ca_act(0, 0, 1), ca_act2(1));
	CHECK(ddr5_violations(d->m) > 0 && has(d, "before initialization"), "ACT before init flagged: %s",
	    ddr5_first_violation(d->m));
	drop(d);

	d = mk(0x1, 16);
	power_up(d);
	mpc_multi(d, 0, 0, 0x02);			/* DLL RESET without the MR13 MPC */
	CHECK(has(d, "DLL RESET before MPC tDLLK"), "DLL reset order flagged: %s", ddr5_first_violation(d->m));
	drop(d);

	d = mk(0x1, 16);
	power_up(d);
	configure(d, 0, 0);
	cmd2(d, 0, 0, ca_act(0, 0, 5), ca_act2(5));
	idle(d, 10);
	read_burst(d, 0, 0, 0, 0, 0, 0);
	CHECK(has(d, "tRCD"), "tRCD violation flagged: %s", ddr5_first_violation(d->m));
	drop(d);

	d = mk(0x1, 16);
	power_up(d);
	configure(d, 0, 0);
	read_burst(d, 0, 0, 4, 2, 0, 0);
	CHECK(has(d, "closed bank"), "RD to a closed bank flagged: %s", ddr5_first_violation(d->m));
	CHECK(d->nrd[0] == 0, "no data for a RD to a closed bank");
	drop(d);

	d = mk(0x1, 16);
	for (int s = 0; s < DDR5_SUBS; s++)
		d->in.sub[s].cs_n = 0;
	d->in.sub[0].dq_oe = 1;				/* write data while RESET_n is LOW */
	clk(d);
	CHECK(has(d, "while RESET_n is LOW"), "DQ driven during reset flagged: %s", ddr5_first_violation(d->m));
	drop(d);

	d = mk(0x1, 16);
	d->in.reset_n[0] = 0;
	for (int i = 0; i < 600; i++)
		clk(d);
	d->in.reset_n[0] = 1;				/* CS_n was HIGH: not LOW tINIT2 before */
	clk(d);
	CHECK(has(d, "tINIT2"), "CS_n HIGH at RESET_n rise flagged: %s", ddr5_first_violation(d->m));
	drop(d);

	d = mk(0x1, 16);
	d->in.sub[0].cs_n = d->in.sub[1].cs_n = 0;	/* the DIMM spans subchannels 0 and 1 */
	d->in.reset_n[0] = 0;
	for (int i = 0; i < 600; i++)
		clk(d);
	d->in.reset_n[0] = 1;
	clk(d);
	idle(d, 10);					/* CS_n HIGH long before tINIT3 */
	CHECK(has(d, "tINIT3"), "CS_n HIGH before tINIT3 flagged: %s", ddr5_first_violation(d->m));
	drop(d);

	d = mk(0x1, 16);
	power_up(d);
	configure(d, 0, 0);
	/* tFAW (1 KB page) = 32 nCK = 4 x tRRD_S at DDR5-4800: a fifth ACT inside
	   the window also breaks tRRD_S. 4 ACTs 8 clocks apart, the 5th 7 later */
	for (unsigned g = 0; g < 5; g++) {
		cmd2(d, 0, 0, ca_act(g, 0, 9), ca_act2(9));
		idle(d, g < 3 ? 5 : 4);
	}
	CHECK(ddr5_violations(d->m) == 2 && has(d, "tRRD_S"),
	    "5th ACT inside tFAW flagged (tRRD_S and tFAW: %llu violations)",
	    (unsigned long long)ddr5_violations(d->m));
	drop(d);

	d = mk(0x1, 16);
	power_up(d);
	configure(d, 0, 0);
	idle(d, 6 * 9375);				/* no REF for > 5 tREFI */
	CHECK(has(d, "refresh overdue"), "refresh overdue flagged: %s", ddr5_first_violation(d->m));
	drop(d);

	d = mk(0x1, 16);
	power_up(d);
	configure(d, 0, 0);
	cmd2(d, 0, 0, ca_act(0, 0, 9), ca_act2(9));
	idle(d, 40);
	cmd2(d, 0, 0, ca_wr(0, 0), ca_wr2(0, 0));	/* WR without the data */
	idle(d, 60);
	CHECK(has(d, "write data missing"), "missing write data flagged: %s", ddr5_first_violation(d->m));
	drop(d);

	printf("%s: %d failure(s)\n", fails ? "FAIL" : "PASS", fails);
	return fails != 0;
}
