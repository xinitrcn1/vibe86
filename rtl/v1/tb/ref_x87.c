/*
 * ref_x87.c: the host's x87 as the reference model of v1's x87 unit
 * (DPI-C, for tb_x87_ref): each call runs the real instruction from FNINIT
 * with the requested control word (all exceptions masked), on 80-bit
 * operands, and returns the result(s) and the status word (exception flags
 * and C0-C3) -- differential testing against hardware (W. M. McKeeman,
 * "Differential Testing for Software", Digital Technical Journal 10(1),
 * 1998).
 *
 * op (a = ST(0), b = ST(1) where two operands):
 *   0 add 1 sub (a-b) 2 mul 3 div (a/b) 4 sqrt 5 frndint 6 fscale 7 fprem
 *   8 fprem1 9 fxtract (r = significand, r2 = exponent) 10 f2xm1 11 fyl2x
 *   (b log2 a) 12 fyl2xp1 13 fpatan (atan(b/a)) 14 fsin 15 fcos 16 fptan
 *   (r = tan) 17 fcom 18 fucom 19 fxam
 *   20 load f32 (alo) 21 load f64 22 fild m16 23 fild m32 24 fild m64
 *   25 fbld (alo, ahi) 26 fst m32 (r.lo) 27 fst m64 28 fist m16 29 fist
 *   m32 30 fistp m64 31 fbstp (r) 32 fisttp m64 33 fld constant (alo: 0
 *   fld1 1 fldl2t 2 fldl2e 3 fldpi 4 fldlg2 5 fldln2 6 fldz)
 */
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

struct f80 {
	uint64_t lo;
	uint16_t hi;
} __attribute__((packed));

#define PRE "fninit\n\tfldcw %[cw]\n\t"

void
ref_x87(int op, int fcw, uint64_t alo, int ahi, uint64_t blo, int bhi,
    uint64_t *rlo, int *rhi, uint64_t *r2lo, int *r2hi, int *fsw)
{
	struct f80 a = { alo, (uint16_t)ahi }, b = { blo, (uint16_t)bhi }, r = { 0, 0 }, r2 = { 0, 0 };
	uint16_t cw = (uint16_t)(fcw | 0x3f), sw = 0;
	uint64_t m = alo;
	uint32_t m32 = (uint32_t)alo;
	uint16_t m16 = (uint16_t)alo;

#define BIN(ins) __asm__ volatile(PRE "fldt %[b]\n\tfldt %[a]\n\t" ins "\n\tfnstsw %[sw]\n\t" \
	"fstpt %[r]\n\tfstp %%st(0)" : [r] "=m"(r), [sw] "=m"(sw) : [a] "m"(a), [b] "m"(b), \
	[cw] "m"(cw))
#define UN(ins) __asm__ volatile(PRE "fldt %[a]\n\t" ins "\n\tfnstsw %[sw]\n\tfstpt %[r]" \
	: [r] "=m"(r), [sw] "=m"(sw) : [a] "m"(a), [cw] "m"(cw))
	switch (op) {
	case 0: BIN("fadd %%st(1), %%st"); break;
	case 1: BIN("fsub %%st(1), %%st"); break;
	case 2: BIN("fmul %%st(1), %%st"); break;
	case 3: BIN("fdiv %%st(1), %%st"); break;
	case 4: UN("fsqrt"); break;
	case 5: UN("frndint"); break;
	case 6: BIN("fscale"); break;
	case 7: BIN("fprem"); break;
	case 8: BIN("fprem1"); break;
	case 9:
		__asm__ volatile(PRE "fldt %[a]\n\tfxtract\n\tfnstsw %[sw]\n\tfstpt %[r]\n\tfstpt %[r2]"
		    : [r] "=m"(r), [r2] "=m"(r2), [sw] "=m"(sw) : [a] "m"(a), [cw] "m"(cw));
		break;
	case 10: UN("f2xm1"); break;
	case 11:
	case 12:
	case 13:
		/* ST(1) = b, ST(0) = a; the result replaces ST(1), then a pop */
		if (op == 11)
			__asm__ volatile(PRE "fldt %[b]\n\tfldt %[a]\n\tfyl2x\n\tfnstsw %[sw]\n\tfstpt %[r]"
			    : [r] "=m"(r), [sw] "=m"(sw) : [a] "m"(a), [b] "m"(b), [cw] "m"(cw));
		else if (op == 12)
			__asm__ volatile(PRE "fldt %[b]\n\tfldt %[a]\n\tfyl2xp1\n\tfnstsw %[sw]\n\tfstpt %[r]"
			    : [r] "=m"(r), [sw] "=m"(sw) : [a] "m"(a), [b] "m"(b), [cw] "m"(cw));
		else
			__asm__ volatile(PRE "fldt %[b]\n\tfldt %[a]\n\tfpatan\n\tfnstsw %[sw]\n\tfstpt %[r]"
			    : [r] "=m"(r), [sw] "=m"(sw) : [a] "m"(a), [b] "m"(b), [cw] "m"(cw));
		break;
	case 14: UN("fsin"); break;
	case 15: UN("fcos"); break;
	case 16:
		/* FPTAN pushes 1.0 over the tangent (when in range) */
		__asm__ volatile(PRE "fldt %[a]\n\tfptan\n\tfnstsw %[sw]\n\tfstpt %[r2]\n\tfstpt %[r]"
		    : [r] "=m"(r), [r2] "=m"(r2), [sw] "=m"(sw) : [a] "m"(a), [cw] "m"(cw));
		break;
	case 17: BIN("fcom %%st(1)"); break;
	case 18: BIN("fucom %%st(1)"); break;
	case 19: UN("fxam"); break;
	case 20:
		__asm__ volatile(PRE "flds %[m]\n\tfnstsw %[sw]\n\tfstpt %[r]"
		    : [r] "=m"(r), [sw] "=m"(sw) : [m] "m"(m32), [cw] "m"(cw));
		break;
	case 21:
		__asm__ volatile(PRE "fldl %[m]\n\tfnstsw %[sw]\n\tfstpt %[r]"
		    : [r] "=m"(r), [sw] "=m"(sw) : [m] "m"(m), [cw] "m"(cw));
		break;
	case 22:
		__asm__ volatile(PRE "filds %[m]\n\tfnstsw %[sw]\n\tfstpt %[r]"
		    : [r] "=m"(r), [sw] "=m"(sw) : [m] "m"(m16), [cw] "m"(cw));
		break;
	case 23:
		__asm__ volatile(PRE "fildl %[m]\n\tfnstsw %[sw]\n\tfstpt %[r]"
		    : [r] "=m"(r), [sw] "=m"(sw) : [m] "m"(m32), [cw] "m"(cw));
		break;
	case 24:
		__asm__ volatile(PRE "fildll %[m]\n\tfnstsw %[sw]\n\tfstpt %[r]"
		    : [r] "=m"(r), [sw] "=m"(sw) : [m] "m"(m), [cw] "m"(cw));
		break;
	case 25:
		__asm__ volatile(PRE "fbld %[a]\n\tfnstsw %[sw]\n\tfstpt %[r]"
		    : [r] "=m"(r), [sw] "=m"(sw) : [a] "m"(a), [cw] "m"(cw));
		break;
	case 26: {
		uint32_t o = 0;
		__asm__ volatile(PRE "fldt %[a]\n\tfstps %[o]\n\tfnstsw %[sw]"
		    : [o] "=m"(o), [sw] "=m"(sw) : [a] "m"(a), [cw] "m"(cw));
		r.lo = o;
		break;
	}
	case 27:
		__asm__ volatile(PRE "fldt %[a]\n\tfstpl %[o]\n\tfnstsw %[sw]"
		    : [o] "=m"(r.lo), [sw] "=m"(sw) : [a] "m"(a), [cw] "m"(cw));
		break;
	case 28: {
		uint16_t o = 0;
		__asm__ volatile(PRE "fldt %[a]\n\tfistps %[o]\n\tfnstsw %[sw]"
		    : [o] "=m"(o), [sw] "=m"(sw) : [a] "m"(a), [cw] "m"(cw));
		r.lo = o;
		break;
	}
	case 29: {
		uint32_t o = 0;
		__asm__ volatile(PRE "fldt %[a]\n\tfistpl %[o]\n\tfnstsw %[sw]"
		    : [o] "=m"(o), [sw] "=m"(sw) : [a] "m"(a), [cw] "m"(cw));
		r.lo = o;
		break;
	}
	case 30:
		__asm__ volatile(PRE "fldt %[a]\n\tfistpll %[o]\n\tfnstsw %[sw]"
		    : [o] "=m"(r.lo), [sw] "=m"(sw) : [a] "m"(a), [cw] "m"(cw));
		break;
	case 31:
		__asm__ volatile(PRE "fldt %[a]\n\tfbstp %[o]\n\tfnstsw %[sw]"
		    : [o] "=m"(r), [sw] "=m"(sw) : [a] "m"(a), [cw] "m"(cw));
		break;
	case 32:
		__asm__ volatile(PRE "fldt %[a]\n\tfisttpll %[o]\n\tfnstsw %[sw]"
		    : [o] "=m"(r.lo), [sw] "=m"(sw) : [a] "m"(a), [cw] "m"(cw));
		break;
	case 33:
		switch (alo) {
		case 0: __asm__ volatile(PRE "fld1\n\tfnstsw %[sw]\n\tfstpt %[r]" : [r] "=m"(r), [sw] "=m"(sw) : [cw] "m"(cw)); break;
		case 1: __asm__ volatile(PRE "fldl2t\n\tfnstsw %[sw]\n\tfstpt %[r]" : [r] "=m"(r), [sw] "=m"(sw) : [cw] "m"(cw)); break;
		case 2: __asm__ volatile(PRE "fldl2e\n\tfnstsw %[sw]\n\tfstpt %[r]" : [r] "=m"(r), [sw] "=m"(sw) : [cw] "m"(cw)); break;
		case 3: __asm__ volatile(PRE "fldpi\n\tfnstsw %[sw]\n\tfstpt %[r]" : [r] "=m"(r), [sw] "=m"(sw) : [cw] "m"(cw)); break;
		case 4: __asm__ volatile(PRE "fldlg2\n\tfnstsw %[sw]\n\tfstpt %[r]" : [r] "=m"(r), [sw] "=m"(sw) : [cw] "m"(cw)); break;
		case 5: __asm__ volatile(PRE "fldln2\n\tfnstsw %[sw]\n\tfstpt %[r]" : [r] "=m"(r), [sw] "=m"(sw) : [cw] "m"(cw)); break;
		default: __asm__ volatile(PRE "fldz\n\tfnstsw %[sw]\n\tfstpt %[r]" : [r] "=m"(r), [sw] "=m"(sw) : [cw] "m"(cw)); break;
		}
		break;
	}
	*rlo = r.lo;
	*rhi = r.hi;
	*r2lo = r2.lo;
	*r2hi = r2.hi;
	*fsw = sw;
}

#ifdef __cplusplus
}
#endif
