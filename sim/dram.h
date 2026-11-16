/*
 * vsim DDR5 memory subsystem of the server board: 16 DIMM slots, their SPD
 * hubs on a two-segment SMBus, the memory controller (init sequence, slot
 * decode, ECC with a scrub engine and error injection), sparse host storage
 * and a cycle-approximate DDR5 timing model.  Register maps and the model are
 * documented in docs/platform.md.
 */
#ifndef DRAM_H
#define DRAM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DRAM_BASE	0x100000000ull		/* 4 GiB */
#define DRAM_WINDOW	(1ull << 40)		/* 1 TiB: 16 slots x 64 GiB */
#define DRAM_SLOTS	16
#define SMB_BASE	0xFE003000ull
#define SMB_SIZE	0x18
#define MC_BASE		0xFE004000ull
#define MC_SIZE		0x300

/* the modeled core clock: one vsim cycle */
#define CORE_PS		833			/* 1.2 GHz */

struct dimm_cfg {
	int present;
	unsigned size_gib;	/* 8, 16, 32 or 64 */
	int ecc;		/* side-band SECDED (x80 bus) */
	unsigned speed;		/* MT/s: 4800 or 5600 */
	int bad_spd;		/* corrupt the SPD CRC (tests) */
};

struct dram;

/* parse "slot:size[:noecc][:5600][:badspd]" into cfg[]; returns 0 or -1 */
int dram_parse_dimm(const char *s, struct dimm_cfg cfg[DRAM_SLOTS]);
void dram_default_cfg(struct dimm_cfg cfg[DRAM_SLOTS]);

struct dram *dram_new(const struct dimm_cfg cfg[DRAM_SLOTS], uint64_t seed);
void dram_free(struct dram *d);
/* board reset: the controller and SMBus reset; DRAM contents are kept */
void dram_reset(struct dram *d);
void dram_tick(struct dram *d);
/* machine-check report: called for every corrected (type 1) or uncorrected
 * (type 2) ECC error the controller counts */
void dram_set_mce(struct dram *d, void (*fn)(void *ctx, int type, uint64_t pa), void *ctx);

/*
 * Accesses at cycle `now`. Return 0, or -1 for a bus error. *lat = cycles
 * until the bus acknowledges (1 = this cycle).
 */
int dram_mem(struct dram *d, int we, uint64_t pa, unsigned n, uint8_t *data,
    uint64_t now, unsigned *lat);
int dram_mc(struct dram *d, int we, uint64_t off, unsigned n, uint8_t *data);
int dram_smb(struct dram *d, int we, uint64_t off, unsigned n, uint8_t *data);

/* one-line summary of controller statistics (vsim -v) */
void dram_stats(const struct dram *d, char *buf, unsigned len);

#ifdef __cplusplus
}
#endif

#endif
