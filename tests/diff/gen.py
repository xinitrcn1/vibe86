#!/usr/bin/env python3
"""Random differential test of the integer instruction set (64-bit mode).

    gen.py SEED STEPS OUT.S [LISTING]

Emits OUT.S: a function body (`diff_body`) of STEPS random user-mode integer
instructions at 8/16/32/64-bit sizes, each followed by a call to chk,
which stores the general registers and the *defined* arithmetic flags in
the output array and resets the flags the SDM leaves undefined
for that instruction (so an undefined flag never feeds a later instruction).
The same body runs natively on the host (host.c) and on vsim (sim.S); any
difference in the sequence of states is a bug in the core, its microcode or the
compiler (differential testing: W. M. McKeeman, "Differential Testing for
Software", Digital Technical Journal 10(1), 1998).

Calling convention of diff_body (SysV, used by both wrappers):
    rdi = scratch buffer (256 bytes, initialised by the caller)
    rsi = output array (96 bytes per step: flags, rax..r11, buffer hash)
The body keeps rbp = buffer (no REX: AH..DH work with it), r13 = output pointer, r15 scratch;
the random pool is rax rbx rcx rdx rsi rdi r8-r11; r14 is chk's scratch. rbx, rbp, r12-r15 are
saved/restored around it.

Only results the manual defines are compared: undefined flags are masked
(per instruction), and operands are chosen so no result is undefined
(BSF/BSR get nonzero sources, SHLD/SHRD 16-bit counts stay <= 16, DIV/IDIV
can't overflow, BSWAP is never 16-bit).
"""
import random
import sys

CF, PF, AF, ZF, SF, OF = 0x1, 0x4, 0x10, 0x40, 0x80, 0x800
ALL = CF | PF | AF | ZF | SF | OF

POOL = ["rax", "rbx", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11"]
R32 = {"rax": "eax", "rbx": "ebx", "rcx": "ecx", "rdx": "edx", "rsi": "esi", "rdi": "edi"}
R16 = {"rax": "ax", "rbx": "bx", "rcx": "cx", "rdx": "dx", "rsi": "si", "rdi": "di"}
R8 = {"rax": "al", "rbx": "bl", "rcx": "cl", "rdx": "dl", "rsi": "sil", "rdi": "dil"}
HI8 = ["ah", "bh", "ch", "dh"]
SUF = {8: "b", 16: "w", 32: "l", 64: "q"}

INTERESTING = [0, 1, 2, 0x7f, 0x80, 0xff, 0x100, 0x7fff, 0x8000, 0xffff, 0x10000,
               0x7fffffff, 0x80000000, 0xffffffff, 0x100000000, 0x7fffffffffffffff,
               0x8000000000000000, 0xffffffffffffffff, 0xfffffffffffffffe,
               0x5555555555555555, 0xaaaaaaaaaaaaaaaa, 0x0123456789abcdef]


def reg(r, w):
    if w == 64:
        return "%" + r
    if r.startswith("r") and r[1:].isdigit():
        return "%" + r + {32: "d", 16: "w", 8: "b"}[w]
    return "%" + {32: R32, 16: R16, 8: R8}[w][r]


class Gen:
    def __init__(self, seed):
        self.rnd = random.Random(seed)
        self.out = []
        self.listing = []

    def val(self, w=64):
        r = self.rnd
        v = r.choice(INTERESTING) if r.random() < 0.4 else r.getrandbits(64)
        if r.random() < 0.3:
            v = (v + r.choice([-1, 1])) & (2**64 - 1)
        return v & ((1 << w) - 1)

    def emit(self, *lines, mask=ALL):
        for l in lines:
            self.out.append("\t" + l)
        self.out.append(f"\tpushq\t${mask:#x}\n\tcall\tchk")
        self.listing.append(" ; ".join(lines))

    def r(self, excl=()):
        return self.rnd.choice([p for p in POOL if p not in excl])

    def size(self):
        return self.rnd.choice([8, 16, 32, 64])

    def mem(self, w):
        return f"{self.rnd.randrange(0, 256 - 8, 1 if w == 8 else w // 8)}(%rbp)"

    def operand8(self):
        """two compatible 8-bit registers (AH..DH can't be used with REX)"""
        if self.rnd.random() < 0.25:
            a = self.rnd.choice(HI8)
            b = self.rnd.choice(HI8 + ["al", "bl", "cl", "dl"])
            return "%" + a, "%" + b
        return reg(self.r(), 8), reg(self.r(), 8)

    def regs(self, w):
        if w == 8:
            return self.operand8()
        return reg(self.r(), w), reg(self.r(), w)

    # ---- instruction classes ------------------------------------------------
    def alu(self):
        op = self.rnd.choice(["add", "adc", "sub", "sbb", "and", "or", "xor", "cmp", "test"])
        w = self.size()
        s = SUF[w]
        mask = ALL & ~AF if op in ("and", "or", "xor", "test") else ALL
        k = self.rnd.randrange(4)
        if op == "test" and k == 2:
            k = 0
        a, b = self.regs(w)
        if k == 0:
            self.emit(f"{op}{s}\t{a}, {b}", mask=mask)
        elif k == 1:
            imm = self.val(min(w, 32))
            if w == 64 and imm & 0x80000000:
                imm -= 1 << 32
            self.emit(f"{op}{s}\t${imm if w != 64 else imm}, {b}", mask=mask)
        elif k == 2:
            self.emit(f"{op}{s}\t{self.mem(w)}, {b}", mask=mask)
        else:
            self.emit(f"{op}{s}\t{a}, {self.mem(w)}", mask=mask)

    def unary(self):
        op = self.rnd.choice(["inc", "dec", "neg", "not"])
        w = self.size()
        a, _ = self.regs(w)
        dst = a if self.rnd.random() < 0.7 else self.mem(w)
        self.emit(f"{op}{SUF[w]}\t{dst}")

    def shift(self):
        op = self.rnd.choice(["shl", "shr", "sar", "rol", "ror", "rcl", "rcr"])
        w = self.size()
        a = self.regs(w)[0] if w != 8 else reg(self.r(("rcx",)), 8)
        n = self.rnd.choice([0, 1, 2, w - 1, w, w + 1, 31, 32, 33, 63, 64, 65, 255,
                             self.rnd.randrange(256)])
        eff = n & (63 if w == 64 else 31)
        mask = ALL
        if eff == 0:
            pass                                # flags unaffected
        elif op in ("shl", "shr", "sar"):
            mask &= ~AF
            if eff != 1:
                mask &= ~OF
            if op != "sar" and eff >= w:
                mask &= ~CF
        else:
            if eff != 1:
                mask &= ~OF
        form = self.rnd.randrange(3)
        if form == 0:
            self.emit(f"movb\t${n}, %cl", f"{op}{SUF[w]}\t%cl, {a}", mask=mask)
        elif form == 1 and n == 1:
            self.emit(f"{op}{SUF[w]}\t{a}", mask=mask)
        else:
            self.emit(f"{op}{SUF[w]}\t${n}, {a}", mask=mask)

    def dshift(self):
        op = self.rnd.choice(["shld", "shrd"])
        w = self.rnd.choice([16, 32, 64])
        a = reg(self.r(("rcx",)), w)
        b = reg(self.r(("rcx",)), w)
        n = self.rnd.choice([0, 1, 2, 15, 16, 31, 32, 63, 64, 65, self.rnd.randrange(256)])
        eff = n & (63 if w == 64 else 31)
        if w == 16 and eff > 16:
            n = eff = self.rnd.choice([0, 1, 15, 16])
        mask = ALL
        if eff:
            mask &= ~AF
            if eff != 1:
                mask &= ~OF
        if self.rnd.random() < 0.5:
            self.emit(f"movb\t${n}, %cl", f"{op}{SUF[w]}\t%cl, {a}, {b}", mask=mask)
        else:
            self.emit(f"{op}{SUF[w]}\t${n}, {a}, {b}", mask=mask)

    def mul(self):
        w = self.size()
        k = self.rnd.randrange(4)
        a, b = self.regs(w)
        if w == 8 and k >= 2:
            k = 0
        if k == 0:
            self.emit(f"mul{SUF[w]}\t{a}", mask=CF | OF)
        elif k == 1:
            self.emit(f"imul{SUF[w]}\t{a}", mask=CF | OF)
        elif k == 2:
            self.emit(f"imul{SUF[w]}\t{a}, {b}", mask=CF | OF)
        else:
            imm = self.rnd.choice([0, 1, -1, 2, -2, 127, -128, 0x7fff, -0x8000, 0x12345678 if w > 16 else 0x1234])
            self.emit(f"imul{SUF[w]}\t${imm}, {a}, {b}", mask=CF | OF)

    def div(self):
        """DIV/IDIV with a guarded divisor: never #DE, so the result is defined."""
        w = self.size()
        signed = self.rnd.random() < 0.5
        d = self.r(("rax", "rdx"))
        mask = 0                                    # all flags undefined
        x = self.val(w)
        if w == 8:
            if signed:
                # AX := SignExtend(AL) (CBW): the quotient fits unless AL = 80h, divisor -1
                dv = self.rnd.choice([1, 2, 3, 7, 0x7f, 0x80, 0xfe, 0x81, 0xfd])
                pre = [f"movq\t${x}, %rax", "cbtw", f"movq\t${dv}, %{d}"]
            else:
                # AH < divisor keeps the quotient in 8 bits
                dv = self.rnd.randrange(1, 256)
                ax = (self.rnd.randrange(dv) << 8) | self.rnd.randrange(256)
                pre = [f"movq\t${ax}, %rax", f"movq\t${dv}, %{d}"]
            self.emit(*pre, f"{'idiv' if signed else 'div'}b\t{reg(d, 8)}", mask=mask)
            return
        sx = {16: "cwtd", 32: "cltd", 64: "cqto"}[w]
        if signed:
            # IDIV: dividend sign-extended from rAX (CWD/CDQ/CQO); the quotient
            # always fits unless dividend = MIN and divisor = -1
            if x == 1 << (w - 1):
                x -= 1
            dv = self.rnd.choice([1, 2, 3, 10, 0x7f, (1 << w) - 2, (1 << w) - 3,
                                  1 << (w - 1), (1 << (w - 1)) - 1, self.val(w) | 1])
            if dv == (1 << w) - 1:
                dv = 3
            pre = [f"movabsq\t${x}, %rax", sx]
        else:
            # DIV: rDX < divisor (top bit set) keeps the quotient in range
            dv = self.val(w) | (1 << (w - 1))
            hi = self.val(w) & ((1 << (w - 1)) - 1)
            pre = [f"movabsq\t${x}, %rax", f"movabsq\t${hi}, %rdx"]
        self.emit(*pre, f"movabsq\t${dv}, %{d}",
                  f"{'idiv' if signed else 'div'}{SUF[w]}\t{reg(d, w)}", mask=mask)

    def convert(self):
        op = self.rnd.choice(["cbtw", "cwtl", "cltq", "cwtd", "cltd", "cqto"])
        self.emit(op)

    def movx(self):
        op = self.rnd.choice(["movzbw", "movzbl", "movzbq", "movzwl", "movzwq",
                              "movsbw", "movsbl", "movsbq", "movswl", "movswq", "movslq"])
        sw = {"b": 8, "w": 16, "l": 32}[op[4]]
        dw = {"w": 16, "l": 32, "q": 64}[op[5]]
        src = reg(self.r(), sw) if self.rnd.random() < 0.7 else self.mem(sw)
        self.emit(f"{op}\t{src}, {reg(self.r(), dw)}")

    def bitscan(self):
        op = self.rnd.choice(["bsf", "bsr", "tzcnt", "lzcnt", "popcnt"])
        w = self.rnd.choice([16, 32, 64])
        ra = self.r()
        a, b = reg(ra, w), reg(self.r(), w)
        if op in ("bsf", "bsr"):
            # a zero source leaves the destination undefined: make it nonzero
            v = self.val(w) or 1 << self.rnd.randrange(w)
            self.emit(f"movabsq\t${v}, %{ra}", f"{op}{SUF[w]}\t{a}, {b}", mask=ZF)
        elif op == "popcnt":
            self.emit(f"{op}{SUF[w]}\t{a}, {b}")
        else:
            self.emit(f"{op}{SUF[w]}\t{a}, {b}", mask=ZF | CF)

    def bt(self):
        op = self.rnd.choice(["bt", "bts", "btr", "btc"])
        w = self.rnd.choice([16, 32, 64])
        a, b = self.regs(w)
        if self.rnd.random() < 0.5:
            self.emit(f"{op}{SUF[w]}\t${self.rnd.randrange(256)}, {a}", mask=CF)
        elif self.rnd.random() < 0.5:
            self.emit(f"{op}{SUF[w]}\t{a}, {b}", mask=CF)
        else:
            # register bit offset into memory: offsets reach outside the
            # operand; keep them within the buffer (0..127 bits from 64(%rbp))
            r = self.r()
            self.emit(f"movq\t${self.rnd.randrange(-64, 64)}, %{r}",
                      f"{op}{SUF[w]}\t{reg(r, w)}, 64(%rbp)", mask=CF)

    def misc(self):
        k = self.rnd.randrange(9)
        w = self.size()
        a, b = self.regs(w)
        if k == 0 and w >= 32:
            self.emit(f"bswap\t{a}")
        elif k == 1:
            self.emit(f"xchg{SUF[w]}\t{a}, {b}")
        elif k == 2:
            self.emit(f"xadd{SUF[w]}\t{a}, {b}")
        elif k == 3:
            self.emit(f"cmpxchg{SUF[w]}\t{a}, {b}")
        elif k == 4:
            cc = self.rnd.choice(["o", "no", "b", "ae", "e", "ne", "be", "a", "s", "ns",
                                  "p", "np", "l", "ge", "le", "g"])
            if w == 8:
                self.emit(f"set{cc}\t{a}")
            else:
                self.emit(f"cmov{cc}{SUF[max(w, 16)]}\t{a}, {b}")
        elif k == 5:
            r1, r2, r3 = self.r(), self.r(), self.r()
            sc = self.rnd.choice([1, 2, 4, 8])
            d = self.rnd.choice([0, 1, -1, 0x7fffffff, -0x80000000, 0x1234])
            ww = self.rnd.choice([16, 32, 64])
            self.emit(f"lea{SUF[ww]}\t{d}(%{r1}, %{r2}, {sc}), {reg(r3, ww)}")
        elif k == 6:
            self.emit(f"movbe{SUF[w]}\t{self.mem(w)}, {a}" if w >= 16 else f"xchg{SUF[w]}\t{a}, {b}")
        elif k == 7:
            op = self.rnd.choice(["adcx", "adox"])
            ww = self.rnd.choice([32, 64])
            a, b = self.regs(ww)
            self.emit(f"{op}{SUF[ww]}\t{a}, {b}", mask=CF if op == "adcx" else OF)
        else:
            sw = self.rnd.choice([8, 16, 32, 64])
            dw = {8: self.rnd.choice([32, 64]), 16: 32, 32: 32, 64: 64}[sw]
            src = reg(self.r(), sw) if self.rnd.random() < 0.7 else self.mem(sw)
            self.emit(f"crc32{SUF[sw]}\t{src}, {reg(self.r(), dw)}")

    def reload(self):
        r = self.r()
        self.emit(f"movabsq\t${self.val()}, %{r}")

    def flags(self):
        self.emit(self.rnd.choice(["clc", "stc", "cmc", "sahf", "lahf"]))

    def body(self, steps):
        classes = [(self.alu, 20), (self.unary, 6), (self.shift, 12), (self.dshift, 5),
                   (self.mul, 6), (self.div, 6), (self.convert, 3), (self.movx, 4),
                   (self.bitscan, 5), (self.bt, 4), (self.misc, 12), (self.reload, 6),
                   (self.flags, 2)]
        fns = [f for f, n in classes for _ in range(n)]
        for r in POOL:
            self.out.append(f"\tmovabsq\t${self.val()}, %{r}")
        for _ in range(steps):
            self.rnd.choice(fns)()


HEAD = r"""// generated by tests/diff/gen.py: do not edit
	.text
	// chk: called with the flag mask pushed; keeps the defined flags (the
	// others are cleared, so an undefined flag never reaches a later
	// instruction), appends them and the pool registers to the output array
	// (12 qwords: the flags, the pool registers and a hash of the scratch
	// buffer) and returns with the masked flags in RFLAGS
chk:
	pushfq
	popq	%r15
	andq	8(%rsp), %r15
	movq	%r15, (%r13)
	movq	%rax, 8(%r13)
	movq	%rbx, 16(%r13)
	movq	%rcx, 24(%r13)
	movq	%rdx, 32(%r13)
	movq	%rsi, 40(%r13)
	movq	%rdi, 48(%r13)
	movq	%r8, 56(%r13)
	movq	%r9, 64(%r13)
	movq	%r10, 72(%r13)
	movq	%r11, 80(%r13)
	xorl	%r14d, %r14d
	xorq	0(%rbp), %r14
	rolq	$7, %r14
	xorq	8(%rbp), %r14
	rolq	$7, %r14
	xorq	16(%rbp), %r14
	rolq	$7, %r14
	xorq	24(%rbp), %r14
	rolq	$7, %r14
	xorq	32(%rbp), %r14
	rolq	$7, %r14
	xorq	40(%rbp), %r14
	rolq	$7, %r14
	xorq	48(%rbp), %r14
	rolq	$7, %r14
	xorq	56(%rbp), %r14
	rolq	$7, %r14
	xorq	64(%rbp), %r14
	rolq	$7, %r14
	xorq	72(%rbp), %r14
	rolq	$7, %r14
	xorq	80(%rbp), %r14
	rolq	$7, %r14
	xorq	88(%rbp), %r14
	rolq	$7, %r14
	xorq	96(%rbp), %r14
	rolq	$7, %r14
	xorq	104(%rbp), %r14
	rolq	$7, %r14
	xorq	112(%rbp), %r14
	rolq	$7, %r14
	xorq	120(%rbp), %r14
	rolq	$7, %r14
	xorq	128(%rbp), %r14
	rolq	$7, %r14
	xorq	136(%rbp), %r14
	rolq	$7, %r14
	xorq	144(%rbp), %r14
	rolq	$7, %r14
	xorq	152(%rbp), %r14
	rolq	$7, %r14
	xorq	160(%rbp), %r14
	rolq	$7, %r14
	xorq	168(%rbp), %r14
	rolq	$7, %r14
	xorq	176(%rbp), %r14
	rolq	$7, %r14
	xorq	184(%rbp), %r14
	rolq	$7, %r14
	xorq	192(%rbp), %r14
	rolq	$7, %r14
	xorq	200(%rbp), %r14
	rolq	$7, %r14
	xorq	208(%rbp), %r14
	rolq	$7, %r14
	xorq	216(%rbp), %r14
	rolq	$7, %r14
	xorq	224(%rbp), %r14
	rolq	$7, %r14
	xorq	232(%rbp), %r14
	rolq	$7, %r14
	xorq	240(%rbp), %r14
	rolq	$7, %r14
	xorq	248(%rbp), %r14
	rolq	$7, %r14
	movq	%r14, 88(%r13)
	addq	$96, %r13
	pushq	%r15
	popfq
	ret	$8

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
	xorl	%r14d, %r14d
	pushq	$2
	popfq
"""

TAIL = r"""	cld
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
            for i, l in enumerate(g.listing):
                f.write(f"{i:5d} {l}\n")


if __name__ == "__main__":
    main()
