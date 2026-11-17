/*
 * ref_x86.c: the host x86-64 CPU as the reference model of v1's datapath
 * (DPI-C, for tb_fpu_ref and tb_alu_ref): each function runs the real
 * instruction -- SSE scalar arithmetic under an MXCSR with the requested
 * rounding, DAZ and FTZ, its exception flags read back; CRC32, SHLD/SHRD,
 * ADCX/ADOX with their flags -- so the comparison is against x86 hardware,
 * not against a second model written from the same reading of the manual
 * (differential testing: W. M. McKeeman, "Differential Testing for
 * Software", Digital Technical Journal 10(1), 1998).
 *
 * Needs an x86-64 host with SSE4.2 and FMA (any x86-64 from 2013 on).
 */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

static unsigned
mxcsr_for(int rm, int daz, int ftz)
{
	/* all exceptions masked; RC bits 14:13; DAZ bit 6; FTZ bit 15 */
	return 0x1f80u | ((unsigned)rm << 13) | (daz ? 0x40u : 0) | (ftz ? 0x8000u : 0);
}

/* op: 0 add, 1 sub, 2 mul, 3 div, 4 sqrt, 5 fma (a * b + c), 6 min, 7 max */
#define SSE_OP(T, SFX)							\
	do {									\
		unsigned m = mxcsr_for(rm, daz, ftz), o = 0;			\
		T xa = {a}, xb = {b}, xc = {c};				\
		switch (op) {							\
		case 0: __asm__ volatile("ldmxcsr %2\n\tadd" SFX " %1, %0\n\tstmxcsr %3"	\
			: "+x"(xa) : "x"(xb), "m"(m), "m"(o)); break;		\
		case 1: __asm__ volatile("ldmxcsr %2\n\tsub" SFX " %1, %0\n\tstmxcsr %3"	\
			: "+x"(xa) : "x"(xb), "m"(m), "m"(o)); break;		\
		case 2: __asm__ volatile("ldmxcsr %2\n\tmul" SFX " %1, %0\n\tstmxcsr %3"	\
			: "+x"(xa) : "x"(xb), "m"(m), "m"(o)); break;		\
		case 3: __asm__ volatile("ldmxcsr %2\n\tdiv" SFX " %1, %0\n\tstmxcsr %3"	\
			: "+x"(xa) : "x"(xb), "m"(m), "m"(o)); break;		\
		case 4: __asm__ volatile("ldmxcsr %1\n\tsqrt" SFX " %0, %0\n\tstmxcsr %2"	\
			: "+x"(xa) : "m"(m), "m"(o)); break;			\
		case 5: __asm__ volatile("ldmxcsr %3\n\tvfmadd231" SFX " %2, %1, %0\n\tstmxcsr %4" \
			: "+x"(xc) : "x"(xa), "x"(xb), "m"(m), "m"(o)); xa = xc; break; \
		case 6: __asm__ volatile("ldmxcsr %2\n\tmin" SFX " %1, %0\n\tstmxcsr %3"	\
			: "+x"(xa) : "x"(xb), "m"(m), "m"(o)); break;		\
		case 7: __asm__ volatile("ldmxcsr %2\n\tmax" SFX " %1, %0\n\tstmxcsr %3"	\
			: "+x"(xa) : "x"(xb), "m"(m), "m"(o)); break;		\
		}								\
		*r = xa[0];							\
		*flags = (int)(o & 0x3f);					\
		unsigned dflt = 0x1f80;						\
		__asm__ volatile("ldmxcsr %0" : : "m"(dflt));			\
	} while (0)

void
ref_f32(int op, int rm, int daz, int ftz, unsigned a, unsigned b, unsigned c,
	unsigned *r, int *flags)
{
	/* bit patterns through integer <-> xmm moves, never through C floats */
	typedef unsigned u32x4 __attribute__((vector_size(16)));
	SSE_OP(u32x4, "ss");
}

void
ref_f64(int op, int rm, int daz, int ftz, unsigned long long a, unsigned long long b,
	unsigned long long c, unsigned long long *r, int *flags)
{
	typedef unsigned long long u64x2 __attribute__((vector_size(16)));
	SSE_OP(u64x2, "sd");
}

/* ---- integer: the flags as RFLAGS bits (CF 0, PF 2, AF 4, ZF 6, SF 7, OF 11) */

unsigned
ref_crc32(unsigned crc, unsigned long long data, int bytes)
{
	unsigned long long c = crc;
	switch (bytes) {
	case 1: __asm__("crc32b %b1, %k0" : "+r"(c) : "r"(data)); break;
	case 2: __asm__("crc32w %w1, %k0" : "+r"(c) : "r"(data)); break;
	case 4: __asm__("crc32l %k1, %k0" : "+r"(c) : "r"(data)); break;
	default: __asm__("crc32q %1, %0" : "+r"(c) : "r"(data)); break;
	}
	return (unsigned)c;
}

/* SHLD/SHRD dst, src, count at 16/32/64 bits; flags_in as RFLAGS */
unsigned long long
ref_shxd(int left, int bits, unsigned long long dst, unsigned long long src, int count,
	 unsigned long long flags_in, unsigned long long *flags_out)
{
	unsigned long long d = dst, f = flags_in;
	unsigned char cl = (unsigned char)count;
#define SHX(INS)								\
	__asm__("push %2\n\tpopfq\n\t" INS "\n\tpushfq\n\tpop %1"		\
		: "+r"(d), "+r"(f) : "r"(f), "r"(src), "c"(cl) : "cc")
	if (left) {
		if (bits == 16) SHX("shldw %%cl, %w3, %w0");
		else if (bits == 32) SHX("shldl %%cl, %k3, %k0");
		else SHX("shldq %%cl, %3, %0");
	} else {
		if (bits == 16) SHX("shrdw %%cl, %w3, %w0");
		else if (bits == 32) SHX("shrdl %%cl, %k3, %k0");
		else SHX("shrdq %%cl, %3, %0");
	}
	*flags_out = f;
	return d;
}

/* ADCX (ox = 0) / ADOX (ox = 1) at 32/64 bits */
unsigned long long
ref_adx(int ox, int bits, unsigned long long a, unsigned long long b,
	unsigned long long flags_in, unsigned long long *flags_out)
{
	unsigned long long d = a, f = flags_in;
#define ADX(INS)								\
	__asm__("push %2\n\tpopfq\n\t" INS "\n\tpushfq\n\tpop %1"		\
		: "+r"(d), "+r"(f) : "r"(f), "r"(b) : "cc")
	if (ox) {
		if (bits == 32) ADX("adoxl %k3, %k0"); else ADX("adoxq %3, %0");
	} else {
		if (bits == 32) ADX("adcxl %k3, %k0"); else ADX("adcxq %3, %0");
	}
	*flags_out = f;
	return d;
}

#ifdef __cplusplus
}
#endif
