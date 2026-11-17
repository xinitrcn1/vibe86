#!/usr/bin/env python3
"""Random differential test of SSE and SSE2 (legacy encodings), 64-bit mode.

    vgen.py SEED STEPS OUT.S [LISTING]

Emits OUT.S: `diff_body`, STEPS random SSE/SSE2 instructions on xmm0-xmm7
(memory operands from a 16-byte aligned scratch buffer at rbp), each
followed by `chk`, which records xmm0-xmm7, MXCSR (its sticky exception
flags too), RFLAGS and RAX. The same body runs on the host (host.c) and on
vsim (sim.S) and vdiff.py compares them (differential testing: W. M.
McKeeman, "Differential Testing for Software", Digital Technical Journal
10(1), 1998). Operands mix random bit patterns with the floating-point
special values (+-0, +-inf, QNaN and SNaN, denormals, the largest and
smallest normals), and LDMXCSR changes the rounding mode, DAZ and FTZ at
random (exceptions stay masked: an unmasked one is tested elsewhere).

Left out: RCPPS/RCPSS/RSQRTPS/RSQRTSS (their approximations differ between
implementations), MOVNT* (as the plain moves), MASKMOVDQU.

Layout of a step's record (19 qwords): xmm0-xmm7 (16 bytes each), MXCSR,
RFLAGS (arithmetic flags), RAX.
"""
import random
import struct
import sys

XMM = [f"%xmm{i}" for i in range(8)]
F32 = [0x00000000, 0x80000000, 0x3f800000, 0xbf800000, 0x7f800000, 0xff800000, 0x7fc00000,
       0xffc00000, 0x7fa00000, 0x00000001, 0x807fffff, 0x00800000, 0x7f7fffff, 0x4b800000,
       0xcf000000, 0x4f000000, 0x3effffff, 0x40490fdb]
F64 = [0x0000000000000000, 0x8000000000000000, 0x3ff0000000000000, 0xbff0000000000000,
       0x7ff0000000000000, 0xfff0000000000000, 0x7ff8000000000000, 0x7ff4000000000000,
       0x0000000000000001, 0x800fffffffffffff, 0x0010000000000000, 0x7fefffffffffffff,
       0x43e0000000000000, 0xc3e0000000000000, 0x41e0000000000000, 0x400921fb54442d18]

FP_PS = ["addps", "subps", "mulps", "divps", "minps", "maxps", "sqrtps", "andps", "andnps",
         "orps", "xorps", "unpcklps", "unpckhps", "cvtps2pd", "cvtdq2ps", "cvtps2dq", "cvttps2dq"]
FP_PD = ["addpd", "subpd", "mulpd", "divpd", "minpd", "maxpd", "sqrtpd", "andpd", "andnpd",
         "orpd", "xorpd", "unpcklpd", "unpckhpd", "cvtpd2ps", "cvtpd2dq", "cvttpd2dq", "cvtdq2pd"]
FP_SS = ["addss", "subss", "mulss", "divss", "minss", "maxss", "sqrtss", "cvtss2sd"]
FP_SD = ["addsd", "subsd", "mulsd", "divsd", "minsd", "maxsd", "sqrtsd", "cvtsd2ss"]
INT = ["paddb", "paddw", "paddd", "paddq", "paddsb", "paddsw", "paddusb", "paddusw", "psubb",
       "psubw", "psubd", "psubq", "psubsb", "psubsw", "psubusb", "psubusw", "pand", "pandn",
       "por", "pxor", "pavgb", "pavgw", "pcmpeqb", "pcmpeqw", "pcmpeqd", "pcmpgtb", "pcmpgtw",
       "pcmpgtd", "pmaxsw", "pminsw", "pmaxub", "pminub", "pmulhw", "pmulhuw", "pmullw",
       "pmuludq", "pmaddwd", "psadbw", "packsswb", "packssdw", "packuswb", "punpcklbw",
       "punpcklwd", "punpckldq", "punpcklqdq", "punpckhbw", "punpckhwd", "punpckhdq",
       "punpckhqdq", "psllw", "pslld", "psllq", "psrlw", "psrld", "psrlq", "psraw", "psrad"]
SHIFT_IMM = ["psllw", "pslld", "psllq", "psrlw", "psrld", "psrlq", "psraw", "psrad"]
MOVES = ["movaps", "movups", "movapd", "movupd", "movdqa", "movdqu"]


class Gen:
    def __init__(self, seed):
        self.r = random.Random(seed)
        self.out = []
        self.listing = []

    def emit(self, *lines):
        for ln in lines:
            self.out.append("\t" + ln)
        self.out.append("\tcall\tchk")
        self.listing.append(" ; ".join(lines))

    def x(self):
        return self.r.choice(XMM)

    def mem(self, align=16):
        # 16 slots of 16 bytes in the 256-byte buffer
        return f"{self.r.randrange(0, 256 - 16, align)}(%rbp)"

    def src(self, align=16):
        return self.x() if self.r.random() < 0.6 else self.mem(align)

    def step(self):
        r = self.r
        k = r.random()
        if k < 0.04:
            # MXCSR: rounding control, DAZ, FTZ at random, all exceptions masked
            v = 0x1f80 | (r.randrange(4) << 13) | (r.randrange(2) << 6) | (r.randrange(2) << 15)
            self.emit(f"movl\t${v:#x}, 248(%rbp)", "ldmxcsr\t248(%rbp)")
            return
        if k < 0.10:
            # fresh special values into a register
            if r.random() < 0.5:
                vals = [r.choice(F32) if r.random() < 0.7 else r.getrandbits(32) for _ in range(4)]
                b = struct.pack("<4I", *vals)
            else:
                vals = [r.choice(F64) if r.random() < 0.7 else r.getrandbits(64) for _ in range(2)]
                b = struct.pack("<2Q", *vals)
            q = struct.unpack("<2Q", b)
            self.emit(f"movabsq\t${q[0]:#x}, %rax", "movq\t%rax, 224(%rbp)",
                      f"movabsq\t${q[1]:#x}, %rax", "movq\t%rax, 232(%rbp)",
                      f"movdqa\t224(%rbp), {self.x()}")
            return
        if k < 0.30:
            op = r.choice(FP_PS + FP_PD)
            self.emit(f"{op}\t{self.src()}, {self.x()}")
            return
        if k < 0.40:
            op = r.choice(FP_SS + FP_SD)
            m = 4 if op in FP_SS else 8
            self.emit(f"{op}\t{self.x() if r.random() < 0.6 else self.mem(m)}, {self.x()}")
            return
        if k < 0.46:
            op = r.choice(["cmpps", "cmppd", "cmpss", "cmpsd"])
            s = self.src() if op.endswith("p" + op[-1]) else self.x()
            self.emit(f"{op}\t${r.randrange(8)}, {s}, {self.x()}")
            return
        if k < 0.50:
            op = r.choice(["comiss", "ucomiss", "comisd", "ucomisd"])
            self.emit(f"{op}\t{self.x()}, {self.x()}")
            return
        if k < 0.56:
            op = r.choice(["cvtss2si", "cvttss2si", "cvtsd2si", "cvttsd2si"])
            dst = r.choice(["%eax", "%rax"])
            self.emit(f"{op}\t{self.x()}, {dst}")
            return
        if k < 0.59:
            op = r.choice(["cvtsi2ss", "cvtsi2sd"])
            w = r.choice(["l", "q"])
            self.emit(f"movabsq\t${r.getrandbits(64):#x}, %rax",
                      f"{op}{w}\t{'%eax' if w == 'l' else '%rax'}, {self.x()}")
            return
        if k < 0.80:
            op = r.choice(INT)
            if op in SHIFT_IMM and r.random() < 0.5:
                self.emit(f"{op}\t${r.choice([0, 1, 7, 15, 16, 31, 32, 63, 64, 200])}, {self.x()}")
            elif op in SHIFT_IMM and r.random() < 0.5:
                c = r.choice([0, 1, 3, 15, 16, 31, 33, 63, 64, 0x100000001])
                self.emit(f"movabsq\t${c:#x}, %rax", "movq\t%rax, %xmm7", f"{op}\t%xmm7, {self.x()}")
            else:
                self.emit(f"{op}\t{self.src()}, {self.x()}")
            return
        if k < 0.86:
            op = r.choice(["pshufd", "pshufhw", "pshuflw", "shufps", "shufpd"])
            self.emit(f"{op}\t${r.randrange(256)}, {self.src()}, {self.x()}")
            return
        if k < 0.89:
            op = r.choice(["pslldq", "psrldq"])
            self.emit(f"{op}\t${r.choice([0, 1, 4, 8, 15, 16, 200])}, {self.x()}")
            return
        if k < 0.93:
            op = r.choice(MOVES)
            if r.random() < 0.5:
                self.emit(f"{op}\t{self.src()}, {self.x()}")
            else:
                self.emit(f"{op}\t{self.x()}, {self.mem()}")
            return
        if k < 0.96:
            c = r.choice([
                lambda: f"movss\t{self.x()}, {self.x()}",
                lambda: f"movss\t{self.mem(4)}, {self.x()}",
                lambda: f"movss\t{self.x()}, {self.mem(4)}",
                lambda: f"movsd\t{self.x()}, {self.x()}",
                lambda: f"movsd\t{self.mem(8)}, {self.x()}",
                lambda: f"movhlps\t{self.x()}, {self.x()}",
                lambda: f"movlhps\t{self.x()}, {self.x()}",
                lambda: f"movhps\t{self.mem(8)}, {self.x()}",
                lambda: f"movlps\t{self.x()}, {self.mem(8)}",
                lambda: f"movhpd\t{self.x()}, {self.mem(8)}",
                lambda: f"movlpd\t{self.mem(8)}, {self.x()}",
            ])
            self.emit(c())
            return
        c = r.choice([
            lambda: (f"movd\t{self.x()}, %eax",),
            lambda: (f"movq\t{self.x()}, %rax",),
            lambda: (f"movabsq\t${r.getrandbits(64):#x}, %rax", f"movd\t%eax, {self.x()}"),
            lambda: (f"movabsq\t${r.getrandbits(64):#x}, %rax", f"movq\t%rax, {self.x()}"),
            lambda: (f"movq\t{self.x()}, {self.x()}",),
            lambda: (f"movq\t{self.mem(8)}, {self.x()}",),
            lambda: (f"movmskps\t{self.x()}, %eax",),
            lambda: (f"movmskpd\t{self.x()}, %eax",),
            lambda: (f"pmovmskb\t{self.x()}, %eax",),
            lambda: (f"pextrw\t${r.randrange(8)}, {self.x()}, %eax",),
            lambda: (f"movabsq\t${r.getrandbits(64):#x}, %rax",
                     f"pinsrw\t${r.randrange(8)}, %eax, {self.x()}"),
            lambda: (f"pinsrw\t${r.randrange(8)}, {self.mem(2)}, {self.x()}",),
        ])
        self.emit(*c())

    def body(self, n):
        for _ in range(n):
            self.step()


HEAD = r"""	.text
	# chk: record xmm0-7, MXCSR, RFLAGS, RAX at r13 (19 qwords); RFLAGS is
	# preserved, as a step that doesn't write flags sees the previous ones
chk:
	pushfq
	movdqu	%xmm0, 0(%r13)
	movdqu	%xmm1, 16(%r13)
	movdqu	%xmm2, 32(%r13)
	movdqu	%xmm3, 48(%r13)
	movdqu	%xmm4, 64(%r13)
	movdqu	%xmm5, 80(%r13)
	movdqu	%xmm6, 96(%r13)
	movdqu	%xmm7, 112(%r13)
	stmxcsr	128(%r13)
	movl	$0, 132(%r13)
	movq	(%rsp), %r14
	andq	$0x8d5, %r14
	movq	%r14, 136(%r13)
	movq	%rax, 144(%r13)
	leaq	152(%r13), %r13
	popfq				# the body's RFLAGS again: chk's own ANDs
	ret				# mustn't reach the next step's record

	.globl	diff_body
diff_body:
	pushq	%rbx
	pushq	%rbp
	pushq	%r12
	pushq	%r13
	pushq	%r14
	pushq	%r15
	movq	%rdi, %rbp
	movq	%rsi, %r13
	movl	$0x1f80, 248(%rbp)
	ldmxcsr	248(%rbp)
	movdqa	0(%rbp), %xmm0
	movdqa	16(%rbp), %xmm1
	movdqa	32(%rbp), %xmm2
	movdqa	48(%rbp), %xmm3
	movdqa	64(%rbp), %xmm4
	movdqa	80(%rbp), %xmm5
	movdqa	96(%rbp), %xmm6
	movdqa	112(%rbp), %xmm7
	xorl	%eax, %eax
"""

TAIL = r"""	movl	$0x1f80, 248(%rbp)
	ldmxcsr	248(%rbp)
	popq	%r15
	popq	%r14
	popq	%r13
	popq	%r12
	popq	%rbp
	popq	%rbx
	ret
"""


def main():
    seed, steps, out = int(sys.argv[1]), int(sys.argv[2]), sys.argv[3]
    g = Gen(seed)
    g.body(steps)
    with open(out, "w") as f:
        f.write(HEAD + "\n".join(g.out) + "\n" + TAIL)
        f.write(f"\t.balign\t8\n\t.globl\tdiff_steps\ndiff_steps:\n\t.quad\t{len(g.listing)}\n")
    if len(sys.argv) > 4:
        with open(sys.argv[4], "w") as f:
            for i, ln in enumerate(g.listing):
                f.write(f"{i:5d} {ln}\n")


if __name__ == "__main__":
    main()
