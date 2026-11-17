/*
 * vsim DDR5 DIMMs at the pin level: the far side of the die's DDR5 pins
 * (docs/die-interface.md, rtl/common/pins.toml; docs/ddr5.md).
 *
 * The die has 2 channels x 2 subchannels; each channel has 2 DIMM slots.
 * A DIMM is 1Rx8 or 2Rx8 of x8 DDR5 dies (16 Gb: 8 bank groups x 4 banks,
 * R0-R15, C0-C9, 1 KB page; 32 Gb: R0-R16): per rank and subchannel four
 * data devices (device d carries byte d of each beat, DQ[8d+7:8d]) and one
 * ECC device (CB[7:0]). Every DRAM is modelled from Micron's DDR5 SDRAM
 * Product Core Data Sheet (Rev. D 10/2022): the Command Truth Table
 * (Table 140), mode registers (Tables 21, 23), MPC (Table 143), the
 * power-up/reset sequence (Table 10 and the text before it), refresh
 * (Table 176) and the DDR5-4800 timing tables (Tables 428-430, 435-437).
 * The module's RCD (an RDIMM register) is transparent: the die's CS_n/CA
 * reach the DRAMs unchanged, one clock per CK.
 *
 * Clocking: ddr5_eval() is one CK rising edge (one sim cycle). It samples
 * the die's outputs as they are at that edge and produces dq_i/dqs_i, which
 * the die samples at its next edge.
 *
 *   READ (final command cycle sampled at edge Tf): the beats of burst clock
 *     k (k = 0..7 for BL16, 0..3 for BC8) are output by the eval of edge
 *     Tf + CL + k (dqs_i = 3 on those clocks, 0 otherwise).
 *   WRITE (final command cycle at edge Tf): the DRAM samples dq_o where
 *     dqs_o marks valid beats at edges Tf + CWL + k (CWL = CL - 2).
 *   The final cycle of a two-cycle command is the clock after the CS_n-low
 *   clock in 1N mode, two clocks after it in 2N mode (the reset default;
 *   "2N Mode", Table 148). The data sheet defines CWL from the final cycle
 *   of the WRITE ("Write Operation"); CL is "from the READ command" and
 *   CWL = CL - 2 (Table 21 note 2): CL is counted from the same final cycle.
 *
 * DRAM address map (the die's memory controller uses the same one; vsim
 * preloads through it). Every populated slot must have the same
 * organisation (uniform population); 1, 2 or 4 slots. The DRAM space is a
 * byte offset `off`:
 *
 *   off[5:0]    byte within the 64-byte line: one BL16 burst on a 32-bit
 *               subchannel; beat b (0..15) carries bytes 4b..4b+3, byte
 *               4b+d on device d (DQ[8d+7:8d]); burst column = C[9:4]*16 + b
 *   off[7:6]    subchannel: 0 = ch0 a, 1 = ch0 b, 2 = ch1 a, 3 = ch1 b
 *   off[13:8]   column C[9:4] (RD/WR carry C[3:0] = 0)
 *   off[16:14]  bank group BG[2:0]
 *   off[18:17]  bank BA[1:0]
 *   off[34:19]  row R[15:0] (off[35:19] R[16:0] with 32 Gb dies)
 *   next bit    rank (2-rank DIMMs only)
 *   next bits   DIMM (slot 0/1 of the channel), when both are populated
 *
 * With only channel 0 populated (slots 0 and/or 1), the subchannel field
 * is off[6] alone and everything above shifts down by one bit.
 *
 * ECC: CB[6:0] of a beat = the Hsiao (39,32) SEC-DED check bits of its 32
 * data bits (M. Y. Hsiao, "A Class of Optimal Minimum Odd-weight-column
 * SEC-DED Codes", IBM J. Res. Dev. 14(4), 1970); CB[7] = 0. The DRAMs store
 * CB like data; ddr5_hsiao() is the code the die uses (the backdoor writes
 * correct CB). Data bit i's check column is the i-th 7-bit value of weight
 * 3 in increasing numeric order; the check bits are the identity columns.
 */
#ifndef DDR5_H
#define DDR5_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DDR5_CHANNELS	2
#define DDR5_SUBS	4		/* 2 channels x 2 subchannels: ch*2 + sub */
#define DDR5_SLOTS	4		/* ch*2 + DIMM */
#define DDR5_RANKS	4		/* per subchannel: CS_n bit = DIMM*2 + rank */

struct ddr5_slot_cfg {
	int present;
	unsigned size_gib;	/* 16 (1Rx8), 32 (2Rx8) or 64 (2Rx8, 32 Gb) */
	int ecc;		/* 1: the ECC device is fitted (CB), 0: CB reads 0 */
	unsigned speed;		/* 4800 (the timings checked are DDR5-4800's) */
	int bad_spd;		/* corrupt the SPD CRC (tests) */
};

/* one subchannel's pins (struct die_pins::ddr5_subchannel) */
struct ddr5_sub_in {
	uint32_t cs_n;		/* [3:0] */
	uint32_t ca;		/* [13:0] */
	uint32_t dq_o[3];	/* 80 bits: beat 0 in 39:0, beat 1 in 79:40 */
	bool dq_oe;
	uint32_t dqs_o;		/* [1:0]: beat 0, beat 1 */
};
struct ddr5_sub_out {
	uint32_t dq_i[3];
	uint32_t dqs_i;
};
struct ddr5_pins_in {
	struct ddr5_sub_in sub[DDR5_SUBS];
	bool reset_n[DDR5_CHANNELS];
	bool spd_scl_o, spd_sda_o;	/* open drain: 0 pulls low */
};
struct ddr5_pins_out {
	struct ddr5_sub_out sub[DDR5_SUBS];
	bool alert_n[DDR5_CHANNELS];
	bool spd_scl_i, spd_sda_i;	/* the bus levels */
};

struct ddr5_mem;

/* "slot:size[:noecc][:badspd]", slot 0..3 = channel*2 + DIMM; 0 or -1 */
int ddr5_parse_slot(const char *s, struct ddr5_slot_cfg cfg[DDR5_SLOTS]);
/* the default: one 16 GiB 1Rx8 ECC DIMM in slot 0 */
void ddr5_default_cfg(struct ddr5_slot_cfg cfg[DDR5_SLOTS]);
/*
 * The DIMMs. fast: the die's meminit_fast strap, which scales the
 * initialization waits (tINIT1, tINIT3, tINIT4, tXPR, tDLLK, tZQCAL) by
 * 1/1024. Returns NULL (message on stderr) for a non-uniform population.
 */
struct ddr5_mem *ddr5_new(const struct ddr5_slot_cfg cfg[DDR5_SLOTS], int fast, uint64_t seed);
void ddr5_free(struct ddr5_mem *m);
/* one CK rising edge (cycle numbers increase by 1 per call) */
void ddr5_eval(struct ddr5_mem *m, const struct ddr5_pins_in *in, struct ddr5_pins_out *out,
    uint64_t cycle);

/* the DRAM space: bytes (the sum of the populated DIMMs) */
uint64_t ddr5_size(const struct ddr5_mem *m);
/* where an offset lives (the map above); 0, or -1 beyond the DRAM */
struct ddr5_loc {
	unsigned sub, cs, bg, ba, row, col, byte;	/* col: the beat's column, byte: device 0..3 */
};
int ddr5_map(const struct ddr5_mem *m, uint64_t off, struct ddr5_loc *l);
/* backdoor (vsim -l/-e preload, debugging): n bytes at off; writes also set
   CB to the Hsiao check bits of each touched beat. 0, or -1 out of range */
int ddr5_backdoor(struct ddr5_mem *m, int we, uint64_t off, uint8_t *buf, uint64_t n);
/* one device byte by its physical location (device 4 = the ECC device) */
int ddr5_phys(struct ddr5_mem *m, int we, unsigned sub, unsigned cs, unsigned bg, unsigned ba,
    unsigned row, unsigned col, unsigned dev, uint8_t *byte);
/* Hsiao (39,32) check bits of a 32-bit beat (CB[6:0]; CB[7] = 0) */
uint8_t ddr5_hsiao(uint32_t data);

/* protocol and timing violations seen (the model flags, never fixes) */
uint64_t ddr5_violations(const struct ddr5_mem *m);
/* the first violation's message (for tests), or "" */
const char *ddr5_first_violation(const struct ddr5_mem *m);
/* a rank's state: 0 reset, 1 initializing, 2 configuration (MRW/MPC
   only), 3 ready, 4 self refresh, 5 power down */
int ddr5_rank_state(const struct ddr5_mem *m, unsigned sub, unsigned cs);
/* a rank's mode register (as MRR would read it) */
uint8_t ddr5_mr(const struct ddr5_mem *m, unsigned sub, unsigned cs, unsigned mr);
/* the SPD contents of a slot (1024 bytes), NULL if empty */
const uint8_t *ddr5_spd(const struct ddr5_mem *m, unsigned slot);
/* statistics line (vsim -v) */
void ddr5_stats(const struct ddr5_mem *m, char *buf, unsigned len);

#ifdef __cplusplus
}

/* C++ glue: the die's generated pin struct (vibe86_die.h) <-> the model */
#ifdef VIBE86_DIE_H
static inline void
ddr5_eval_pins(ddr5_mem *m, die_pins &p, uint64_t cycle)
{
	ddr5_pins_in in = {};
	ddr5_pins_out out = {};
	for (unsigned s = 0; s < DDR5_SUBS; s++) {
		in.sub[s].cs_n = p.ddr[s].cs_n;
		in.sub[s].ca = p.ddr[s].ca;
		for (unsigned w = 0; w < 3; w++)
			in.sub[s].dq_o[w] = p.ddr[s].dq_o[w];
		in.sub[s].dq_oe = p.ddr[s].dq_oe;
		in.sub[s].dqs_o = p.ddr[s].dqs_o;
	}
	for (unsigned c = 0; c < DDR5_CHANNELS; c++)
		in.reset_n[c] = p.ddrch[c].reset_n;
	in.spd_scl_o = p.spd[0].scl_o;	/* SPD bus 0: channels 0-3 (D-019) */
	in.spd_sda_o = p.spd[0].sda_o;
	ddr5_eval(m, &in, &out, cycle);
	for (unsigned s = 0; s < DDR5_SUBS; s++) {
		for (unsigned w = 0; w < 3; w++)
			p.ddr[s].dq_i[w] = out.sub[s].dq_i[w];
		p.ddr[s].dqs_i = out.sub[s].dqs_i;
	}
	for (unsigned c = 0; c < DDR5_CHANNELS; c++)
		p.ddrch[c].alert_n = out.alert_n[c];
	p.spd[0].scl_i = out.spd_scl_i;
	p.spd[0].sda_i = out.spd_sda_i;
	p.spd[1].scl_i = 1;		/* bus 1 (channels 4-7): only its pull-ups */
	p.spd[1].sda_i = 1;
}
#endif
#endif

#endif
