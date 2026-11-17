/*
 * DDR5 module organizations, speed bins and SPD contents, shared by the
 * DIMM models (ddr5.c; dram.c until it goes).
 *
 * References: JEDEC JESD400-5 (DDR5 SPD contents, CRC over bytes 0-509);
 * Micron DDR5 SDRAM Product Core Data Sheet (Rev. D 10/2022): Table 176
 * (tRFC1/tRFC2), Table 361 and Tables 428-430 (DDR5-4800 speed bin and
 * timings); Micron 16Gb DDR5 SDRAM Die Rev A, Table 2 (x8: 8 BG x 4 BA,
 * R0-R15, C0-C9, 1KB page).
 */
#include <stdio.h>
#include <string.h>

#include "spd.h"

/* device organization of each supported module size (JESD79-5 densities) */
const struct geom spd_geoms[SPD_NGEOMS] = {
	/* 8 GiB: 1Rx16, 16 Gb dies: 4 bank groups x 4 banks */
	{ 8, 16, 16, 4, 2, 2, 16, 10, 1 },
	/* 16 GiB: 1Rx8, 16 Gb: 8 bank groups x 4 banks */
	{ 16, 8, 16, 4, 3, 2, 16, 10, 1 },
	/* 32 GiB: 2Rx8, 16 Gb */
	{ 32, 8, 16, 4, 3, 2, 16, 10, 2 },
	/* 64 GiB: 2Rx8, 32 Gb: 128 K rows */
	{ 64, 8, 32, 6, 3, 2, 17, 10, 2 },
};

/* speed bins, in ps: the -B bins. DDR5-4800B: CL-nRCD-nRP 40-39-39, tAA =
   tRCD = tRP = 16.000 ns, tRAS 32.000 ns, tRC 48.000 ns (Micron core data
   sheet Table 361; die Rev A Table 1), tWR 30 ns (Table 429) */
const struct speed spd_speeds[SPD_NSPEEDS] = {
	{ 4800, 416, 1010, 16000, 16000, 16000, 32000, 48000, 30000 },
	{ 5600, 357, 1010, 16422, 16065, 16065, 32000, 48065, 30000 },	/* 46-45-45 */
};

unsigned
spd_trfc1_ns(unsigned dens_gb)
{
	return dens_gb >= 32 ? 410 : 295;
}

/* JEDEC SPD CRC-16: polynomial 0x1021, initial value 0 */
unsigned
spd_crc(const uint8_t *p, int n)
{
	unsigned crc = 0;
	while (--n >= 0) {
		crc ^= (unsigned)*p++ << 8;
		for (int i = 0; i < 8; i++)
			crc = crc & 0x8000 ? (crc << 1) ^ 0x1021 : crc << 1;
	}
	return crc & 0xFFFF;
}

static void
put16(uint8_t *p, unsigned v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

/* the SPD contents (JESD400-5 base configuration + module block subset) */
void
spd_build(uint8_t spd[1024], const struct geom *g, const struct speed *sp, int ecc)
{
	memset(spd, 0, 1024);
	spd[0] = 0x30;			/* SPD device: 1024 bytes */
	spd[1] = 0x10;			/* SPD revision 1.0 */
	spd[2] = 0x12;			/* key byte: DDR5 SDRAM */
	spd[3] = 0x01;			/* module type: RDIMM */
	spd[4] = (uint8_t)g->dens_code;	/* monolithic die, density */
	spd[5] = (uint8_t)((g->cols - 10) << 5 | (g->rows - 16));
	spd[6] = g->devw == 4 ? 0x00 : g->devw == 8 ? 0x20 : 0x40;
	spd[7] = (uint8_t)(g->bg_log2 << 5 | g->bk_log2);
	put16(spd + 20, sp->tck);	/* tCKAVGmin, ps */
	put16(spd + 22, sp->tckmax);	/* tCKAVGmax, ps */
	/* CAS latencies supported: bit n = CL 20 + 2n, up to the bin's CL */
	unsigned clmax = (sp->taa + sp->tck - 1) / sp->tck;
	for (unsigned cl = 20; cl <= clmax + 1 && cl < 60; cl += 2)
		spd[24 + (cl - 20) / 2 / 8] |= (uint8_t)(1 << ((cl - 20) / 2 % 8));
	put16(spd + 30, sp->taa);
	put16(spd + 32, sp->trcd);
	put16(spd + 34, sp->trp);
	put16(spd + 36, sp->tras);
	put16(spd + 38, sp->trc);
	put16(spd + 40, sp->twr);
	put16(spd + 42, spd_trfc1_ns(g->dens_gb));		/* tRFC1, ns */
	put16(spd + 44, g->dens_gb >= 32 ? 220 : 160);	/* tRFC2 */
	put16(spd + 46, g->dens_gb >= 32 ? 190 : 130);	/* tRFCsb */
	spd[234] = (uint8_t)((g->ranks - 1) << 3);	/* package ranks per channel */
	/* 2 subchannels (bits 6:5 = 01), 8-bit ECC extension (4:3 = 10), x32 */
	spd[235] = ecc ? 0x32 : 0x22;
	put16(spd + 510, spd_crc(spd, 510));
	/* manufacturing block: part number at 521..550 */
	char pn[31];
	snprintf(pn, sizeof pn, "VSIM-DDR5-%uG-%uRx%u-%u%s", g->size_gib, g->ranks,
	    g->devw, sp->mts, ecc ? "-ECC" : "");
	memset(spd + 521, ' ', 30);
	memcpy(spd + 521, pn, strlen(pn));
}

