/* DDR5 module organizations, speed bins and SPD contents (spd.c) */
#ifndef SPD_H
#define SPD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* device organization of a module size */
struct geom {
	unsigned size_gib, devw, dens_gb, dens_code, bg_log2, bk_log2, rows, cols, ranks;
};
/* speed bins, in ps (tRFC in ns) */
struct speed {
	unsigned mts, tck, tckmax, taa, trcd, trp, tras, trc, twr;
};

#define SPD_NGEOMS	4
#define SPD_NSPEEDS	2
extern const struct geom spd_geoms[SPD_NGEOMS];
extern const struct speed spd_speeds[SPD_NSPEEDS];

unsigned spd_trfc1_ns(unsigned dens_gb);
/* JEDEC SPD CRC-16 (polynomial 1021h, initial 0) */
unsigned spd_crc(const uint8_t *p, int n);
/* JESD400-5 contents for a module: base configuration + module block subset */
void spd_build(uint8_t spd[1024], const struct geom *g, const struct speed *sp, int ecc);

#ifdef __cplusplus
}
#endif

#endif
