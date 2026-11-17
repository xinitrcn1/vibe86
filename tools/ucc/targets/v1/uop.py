"""The v1 micro-operation ISA: the contract between ucc and rtl/v1.

One micro-op (uop) is a 128-bit horizontal word (fields from the top):

    op     8   operation (OPS below)
    sz     2   scalar operand size: 0 = 8, 1 = 16, 2 = 32, 3 = 64 bits
    dst    7   destination selector (SEL; vector ops: VSEL)
    a      7   first source
    b      7   second source
    c      7   third source (FMA, ternary logic, select)
    fmask  6   RFLAGS bits the uop may write: OF SF ZF AF PF CF (bit 5..0)
    cc     4   x86 condition (JCC, SETCC) or micro-condition (JU)
    seg    4   memory uops: 0-5 ES CS SS DS FS GS, 6 = linear, 7 = physical,
               8 = the instruction's operand segment (prefix, else DS or SS)
    x      4   op-specific (ZEXT/SEXT destination size, access kind, ...)
    vec    1   apply the op lane-wise over vector registers
    et     4   element type (ET below) for vector and floating-point uops
    vl     2   vector length: 0 = 128, 1 = 256, 2 = 512 bits, 3 = scalar element
    kmode  2   write mask: 0 none, 1 the instruction's (EVEX.aaa, EVEX.z), 2 = kreg
    kreg   3   mask register for kmode 2 (k0 means all ones)
    rnd    3   FP rounding: 0-3 RN RD RU RZ, 4 = MXCSR/FCW, 5 = EVEX.b static rounding
    imm    32  constant (selector K), uop address (jumps), special register,
               vector, bit field, shuffle control

Micro-ops run one at a time (rtl/v1 isn't pipelined) on a *working* copy of
the architectural registers; END commits it and a fault discards it (D-005:
Hwu & Patt, "Checkpoint Repair for Out-of-order Execution Machines", ISCA
1987, doi:10.1145/30350.30352). The format is horizontal microcode in the
sense of Wilkes & Stringer, "Micro-programming and the design of the control
circuits in an electronic digital computer", Proc. Cambridge Philosophical
Society 49(2), 1953 (doi:10.1017/S0305004100028322).

Memory uops go through segmentation (base, limit and type checks in
hardware outside 64-bit mode) and the TLB; a TLB miss is a *micro-trap*: the
core saves the micro-PC and runs the WALK routine (compiled like everything
else, from routines/WALK.sdm), which installs the translation (TLBW) or
raises #PF, and returns (URET) to retry the access (D-008).
"""

WIDTH = 128
UADDR_W = 20      # micro-op address bits (1M micro-ops)

OPS = [
    "NOP",
    # integer ALU: dst := a op b at sz (or per element with vec); flags per fmask
    "ADD", "ADC", "SUB", "SBB", "AND", "OR", "XOR", "ANDN",
    "SHL", "SHR", "SAR", "ROL", "ROR", "RCL", "RCR",   # b = count, masked as x86 does
    "SHLD", "SHRD",        # dst := double shift of (a, b) by c
    "NEG", "NOT", "MOV",
    "MULU", "MULS",        # dst := low half of a*b; HI := high half; CF=OF := high half significant
    "IMULT",               # dst := a*b truncated; CF=OF := it didn't fit
    "MULH",                # dst := high half (x: 0 unsigned, 1 signed, 2 high 52 bits for IFMA)
    "DIVU", "DIVS",        # dst := (HI:a) / b, REM := remainder; #DE on b = 0 or overflow
    "ZEXT", "SEXT",        # dst (at size x) := extend(a at sz)
    "SETCC",               # dst := cc(RFLAGS) ? 1 : 0
    "BSF", "BSR", "TZCNT", "LZCNT", "POPCNT", "BSWAP",
    "PDEP", "PEXT", "BEXTR", "BZHI", "BLSI", "BLSMSK", "BLSR",
    "SZP",                 # SF, ZF, PF (fmask) := of a at sz
    "GETF",                # dst := RFLAGS[imm]
    "SETF",                # RFLAGS[imm] := a[0]
    "SLICE",               # dst := (a >> imm[7:0]) & ones(imm[15:8])
    "INSERT",              # dst := dst with bits [imm[7:0] +: imm[15:8]] := a
    "KHI",                 # dst := dst[31:0] | imm << 32
    "CMP",                 # micro-flags := compare(a, b) at sz (RFLAGS untouched)
    "SEL",                 # dst := c[0] ? a : b
    # element-wise extras (also usable scalar)
    "MIN", "MAX", "MINU", "MAXU", "ABS", "AVGU",
    "ADDS", "SUBS", "ADDUS", "SUBUS",   # saturating
    "PACKSS", "PACKUS",    # narrowing with saturation (x: source element type)
    "CLMUL",               # carry-less multiply (imm selects halves)
    "TERNLOG",             # dst := bitwise imm8 function of (dst, a, b)
    "GF2P8MUL", "GF2P8AFF",
    "AESENC", "AESENCL", "AESDEC", "AESDECL", "AESIMC", "AESKGA",
    "SHA1RNDS4", "SHA1NEXTE", "SHA1MSG1", "SHA1MSG2", "SHA256RNDS2", "SHA256MSG1", "SHA256MSG2",
    "DOT",                 # dst := a dot-product step (x selects the VNNI/BF16/FP16 variant)
    "BMM",                 # AVX512-BMM bit-matrix multiply (x: variant)
    "CRC32",
    # floating point (et: f16 bf16 f32 f64 f80), rounding per rnd; exceptions to MXCSR/FSW
    "FADD", "FSUB", "FMUL", "FDIV", "FSQRT", "FMA",   # FMA: dst := a*b + c (x: negations)
    "FMIN", "FMAX", "FCMP",        # FCMP: dst/mask := predicate imm[4:0]; COMI forms set RFLAGS
    "FCVT",                # dst (et) := a (element type x)
    "FCVTI", "FCVTIT",     # float -> integer (rounded / truncated); x: integer type
    "ICVTF",               # integer (x) -> float (et)
    "FRND",                # round to integral (imm: mode / precision, ROUNDSS/VRNDSCALE)
    "FGETEXP", "FGETMANT", "FSCALE", "FRCP", "FRSQRT", "FFIXUP", "FRANGE", "FREDUCE", "FCLASS",
    "FXAM", "FPREM", "FTRIG", "FLOG", "FEXP",   # x87 transcendental/partial remainder (x: variant)
    # vectors and lanes
    "VGET",                # dst (scalar) := element imm of vector a (et)
    "VSET",                # vector dst element imm := scalar a (et)
    "VBCAST",              # every element of dst := scalar a, or element 0 of vector a
    "VPERM",               # dst := elements of a selected by index vector b (x: 2-source)
    "VSHUF",               # dst := imm8 shuffle of a/b within 128-bit lanes (x: pattern kind)
    "VALIGN",              # dst := concat(a, b) >> imm elements (x: per-128 lane)
    "VUNPCK",              # interleave (x: low/high)
    "VCOMPRESS", "VEXPAND",
    "VSHIFTB",             # byte shift of 128-bit lanes (PSLLDQ/PSRLDQ)
    "VCONFLICT", "VP2ISECT", "VPMULTISHIFT",
    "VMOVMSK",             # scalar dst := sign bits of a's elements
    "KOP",                 # mask-register op (x: and or xor andn not shl shr add unpck)
    "KTEST",               # RFLAGS ZF/CF from masks
    "VCMPK",               # mask dst := integer compare of elements (imm: predicate)
    "VTESTK",              # mask dst := (a AND b) != 0 per element (x: test / testn)
    "VTOK", "KTOV",        # mask <-> vector (VPMOVM2x / VPMOVx2M)
    "VZEROUPPER", "VZEROALL",
    # x87 stack
    "FPUSH", "FPOP", "FXCH",          # TOP adjust and tag update
    # sequencing
    "JMP", "JCC", "JU", "JX",          # JX: upc := imm if a != 0
    "UCALL", "URET",                   # micro-subroutines and micro-trap return
    "END",                 # instruction done: commit, RIP := NRIP
    "COMMIT",              # commit without ending (REP iterations)
    # memory and I/O
    "LD",                  # dst := mem[seg: a + sext(imm)] (sz; x: access kind)
    "ST",                  # mem[seg: a + sext(imm)] := b
    "VLD",                 # vector dst := mem (vl/et; masked elements don't fault)
    "VST",
    "LDP", "STP",          # physical (page walks, descriptor tables are linear: seg 6)
    "LOCK", "UNLOCK",      # bus lock for read-modify-write
    "IN", "OUT",
    "TLBW",                # install a translation: a = linear page, b = PTE image, c = page size
    "TLBINV",              # x: 0 all, 1 non-global, 2 page a, 3 PCID b, 4 page a in PCID b
    "CLFLUSH",             # cache maintenance (a no-op without caches, kept for ordering)
    # system
    "RDS", "WRS",          # special registers (SPECIAL), imm = number; x = 1: number from a
    "SEGLD",               # load a segment's descriptor cache: imm = segment, a = selector, b:c = descriptor
    "EXC",                 # raise vector imm[7:0]; error code a if imm[8]; imm[9] trap; imm[10] vector from a
    "HLT", "SHUTDOWN",
    "RDRAND",              # dst := random; CF := valid
    "CPUID",               # EAX, EBX, ECX, EDX := CPUID(EAX, ECX) (rtl/common/cpuid.toml)
    "SERIALIZE",
]
OP = {n: i for i, n in enumerate(OPS)}
assert len(OPS) <= 256

# ---------------------------------------------------------------- selectors

SEL = {}
GPR64 = ["RAX", "RCX", "RDX", "RBX", "RSP", "RBP", "RSI", "RDI",
         "R8", "R9", "R10", "R11", "R12", "R13", "R14", "R15"]
for i, n in enumerate(GPR64):
    SEL[n] = i
for i, n in enumerate(["AH", "CH", "DH", "BH"]):
    SEL[n] = 16 + i
NTEMPS = 32          # T0..T31 for instruction routines (and DELIVER)
NTRAPTEMPS = 32      # T32..T63 for micro-trap routines (WALK), which interrupt others
for i in range(NTEMPS + NTRAPTEMPS):
    SEL[f"T{i}"] = 20 + i
SEL.update({
    "REG": 84,    # ModRM.reg (+REX.R/EVEX.R'); AH..BH for sz 8 without REX
    "RM": 85,     # ModRM.rm (+REX.B) register, or the register in the opcode
    "VVVV": 86,   # VEX/EVEX.vvvv general register (BMI)
    "IMM": 87,    # the immediate, sign-extended to 64 bits (or zero-extended: immz)
    "EA": 88,     # the memory operand's offset (before the segment base)
    "NRIP": 89,   # next RIP (write: a jump)
    "RIP": 90,    # this instruction's RIP
    "FLAGS": 91,  # RFLAGS
    "K": 92,      # imm, sign-extended
    "ZERO": 93,
    "ONE": 94,
    "HI": 95,     # the multiplier's high half / the divider's high dividend
    "REM": 96,    # the divider's remainder
    "IMM2": 97,   # second immediate (ENTER's ib, EXTRQ/INSERTQ)
    "SEGREG": 98, # the segment register named by ModRM.reg (MOV Sreg): its selector
    "TRAPVA": 99, # micro-trap: the linear address that missed / faulted
    "TRAPX": 100, # micro-trap: access kind (bit 0 write, 1 user, 2 fetch, 3 shadow stack)
    # micro-subroutine arguments: shared by all call levels (T0-T31 are banked:
    # UCALL gives the callee a fresh bank, URET returns to the caller's)
    "A0": 101, "A1": 102, "A2": 103, "A3": 104,
})
USTACK = 4           # micro-subroutine nesting depth

# vector-side selectors (for vec uops, VLD/VST, VGET/VSET, KOP)
VSEL = {}
for i in range(32):
    VSEL[f"ZMM{i}"] = i
for i in range(8):
    VSEL[f"VT{i}"] = 32 + i         # 512-bit vector temporaries
VSEL.update({"VREG": 40, "VRM": 41, "VVVV": 42, "VIS4": 43})   # decoded (VIS4: imm[7:4])
for i in range(8):
    VSEL[f"ST{i}"] = 48 + i         # x87 ST(i), relative to TOP
for i in range(8):
    VSEL[f"K{i}"] = 56 + i          # opmask registers
VSEL.update({"KREG": 64, "KRM": 65, "KVVVV": 66, "KAAA": 67})   # decoded mask operands
for i in range(4):
    VSEL[f"KT{i}"] = 68 + i         # mask temporaries
NSEL = 128

# element types
ET = {"i8": 0, "i16": 1, "i32": 2, "i64": 3, "u8": 4, "u16": 5, "u32": 6, "u64": 7,
      "f16": 8, "bf16": 9, "f32": 10, "f64": 11, "f80": 12, "i128": 13}
ET_BITS = {"i8": 8, "i16": 16, "i32": 32, "i64": 64, "u8": 8, "u16": 16, "u32": 32,
           "u64": 64, "f16": 16, "bf16": 16, "f32": 32, "f64": 64, "f80": 80, "i128": 128}
VL = {128: 0, 256: 1, 512: 2, "scalar": 3}

SEGS = {"ES": 0, "CS": 1, "SS": 2, "DS": 3, "FS": 4, "GS": 5, "LINEAR": 6, "PHYS": 7, "EASEG": 8}

# x86 condition codes
CC = {"O": 0, "NO": 1, "B": 2, "C": 2, "NAE": 2, "AE": 3, "NB": 3, "NC": 3,
      "E": 4, "Z": 4, "NE": 5, "NZ": 5, "BE": 6, "NA": 6, "A": 7, "NBE": 7,
      "S": 8, "NS": 9, "P": 10, "PE": 10, "NP": 11, "PO": 11,
      "L": 12, "NGE": 12, "GE": 13, "NL": 13, "LE": 14, "NG": 14, "G": 15, "NLE": 15}

# micro-conditions for JU, on the flags CMP left
UCC = {"EQ": 0, "NE": 1, "LTU": 2, "GEU": 3, "LEU": 4, "GTU": 5,
       "LTS": 6, "GES": 7, "LES": 8, "GTS": 9, "ALWAYS": 15}
UCC_NEG = {"EQ": "NE", "NE": "EQ", "LTU": "GEU", "GEU": "LTU", "LEU": "GTU",
           "GTU": "LEU", "LTS": "GES", "GES": "LTS", "LES": "GTS", "GTS": "LES"}

FLAG = {"CF": 0, "PF": 2, "AF": 4, "ZF": 6, "SF": 7, "TF": 8, "IF": 9, "DF": 10,
        "OF": 11, "IOPL": 12, "NT": 14, "RF": 16, "VM": 17, "AC": 18, "VIF": 19,
        "VIP": 20, "ID": 21}
FMASK = {"CF": 1 << 0, "PF": 1 << 1, "AF": 1 << 2, "ZF": 1 << 3, "SF": 1 << 4, "OF": 1 << 5}
STATUS = ["CF", "PF", "AF", "ZF", "SF", "OF"]

# special registers (RDS/WRS imm)
SPECIAL = {
    "CR0": 0, "CR2": 2, "CR3": 3, "CR4": 4, "CR8": 8,
    "DR0": 9, "DR1": 10, "DR2": 11, "DR3": 12, "DR6": 13, "DR7": 14,
    "EFER": 16, "FS_BASE": 17, "GS_BASE": 18, "KERNEL_GS_BASE": 19,
    "TSC": 20, "STAR": 21, "LSTAR": 22, "CSTAR": 23, "FMASK": 24,
    "TSC_AUX": 25, "APIC_BASE": 26, "PAT": 27, "MISC_ENABLE": 28,
    "SYSENTER_CS": 29, "SYSENTER_ESP": 30, "SYSENTER_EIP": 31,
    "IDTR_BASE": 32, "IDTR_LIMIT": 33, "GDTR_BASE": 34, "GDTR_LIMIT": 35,
    "XCR0": 36, "XSS": 37, "MXCSR": 38, "FCW": 39, "FSW": 40, "FTW": 41,
    "FIP": 42, "FDP": 43, "FOP": 44, "PKRU": 45, "TSC_ADJUST": 46,
    "U_CET": 47, "S_CET": 48, "PL0_SSP": 49, "PL1_SSP": 50, "PL2_SSP": 51,
    "PL3_SSP": 52, "ISST_ADDR": 53, "SSP": 54,
    "SMBASE": 55,      # SMRAM state save base (AMD APM Vol. 2 10.2.3); reset 3_0000h
    "SMM": 63,         # bit 0: in SMM (SMIACT#, NMIs and SMIs held); bit 1: SMI taken in HLT
    "CPL": 56,
    "FAULT_RIP": 57,   # delivery: RIP to push (the faulting one, or the next for traps)
    "FAULT_ERR": 58,
    "FAULT_VEC": 59,   # bit 8: has an error code; bit 9: software interrupt; bit 10: external
    "UCOUNT": 60,      # micro-ops retired
    "INT_SHADOW": 62,  # write 1: no interrupts (or debug traps) before the next instruction
                       # completes (MOV SS, POP SS, STI; Intel SDM Vol. 3A 7.8.3)
    "MODE": 61,        # read-only: bit 0 PE, 1 LMA, 2 CS.L, 3 CS.D, 4 VM, 5 PG
    # segment descriptor caches: 64 + 4*seg + {0 selector, 1 base, 2 limit, 3 attributes}
    # (seg 0-5 ES CS SS DS FS GS, 6 LDTR, 7 TR)
    # SVM state (VMCB pointer, host save area) starts at 128
    "VM_HSAVE_PA": 128, "VMCB": 129,
}
for i, s in enumerate(["ES", "CS", "SS", "DS", "FS", "GS", "LDTR", "TR"]):
    for j, part in enumerate(["SEL", "BASE", "LIMIT", "ATTR"]):
        SPECIAL[f"{s}_{part}"] = 64 + 4 * i + j
    SPECIAL[s] = 64 + 4 * i           # a segment register's name reads its selector

SIZES = {8: 0, 16: 1, 32: 2, 64: 3}


def encode(op, sz=64, dst=0, a=0, b=0, c=0, fmask=0, cc=0, seg=8, x=0, vec=0, et=0, vl=0,
           kmode=0, kreg=0, rnd=4, imm=0):
    assert 0 <= imm < (1 << 32) or -(1 << 31) <= imm < 0, imm
    fields = [(OP[op], 8), (SIZES[sz], 2), (dst, 7), (a, 7), (b, 7), (c, 7), (fmask, 6),
              (cc, 4), (seg, 4), (x, 4), (vec, 1), (et, 4), (vl, 2), (kmode, 2), (kreg, 3),
              (rnd, 3), (imm & 0xFFFFFFFF, 32)]
    w = 0
    n = 0
    for v, bits in fields:
        assert 0 <= v < (1 << bits), (op, v, bits)
        w = (w << bits) | v
        n += bits
    return w << (WIDTH - n)


FIELDS_SV = """    uop_op_e op;
    logic [1:0] sz;
    logic [6:0] dst;
    logic [6:0] a;
    logic [6:0] b;
    logic [6:0] c;
    logic [5:0] fmask;
    logic [3:0] cc;
    logic [3:0] seg;
    logic [3:0] x;
    logic vec;
    logic [3:0] et;
    logic [1:0] vl;
    logic [1:0] kmode;
    logic [2:0] kreg;
    logic [2:0] rnd;
    logic [31:0] imm;
    logic [WIDTH_PAD-1:0] pad;"""


def sv_package(target="v1"):
    used = 8 + 2 + 7 * 4 + 6 + 4 + 4 + 4 + 1 + 4 + 2 + 2 + 3 + 3 + 32
    out = [f"// generated by tools/ucc/targets/{target}/uop.py: the {target} micro-op ISA (do not edit)",
           f"package {target}_uop_pkg;",
           f"  localparam int UOP_W = {WIDTH};",
           f"  localparam int WIDTH_PAD = {WIDTH - used};",
           "  typedef enum logic [7:0] {"]
    out.append(",\n".join(f"    U_{n} = 8'd{i}" for i, n in enumerate(OPS)))
    out.append("  } uop_op_e;")
    out.append("  typedef struct packed {\n" + FIELDS_SV + "\n  } uop_t;")
    for n, v in SEL.items():
        out.append(f"  localparam logic [6:0] S_{n} = 7'd{v};")
    for n, v in VSEL.items():
        out.append(f"  localparam logic [6:0] V_{n} = 7'd{v};")
    for n, v in UCC.items():
        out.append(f"  localparam logic [3:0] UC_{n} = 4'd{v};")
    for n, v in SPECIAL.items():
        out.append(f"  localparam logic [31:0] SP_{n} = 32'd{v};")
    for n, v in FLAG.items():
        out.append(f"  localparam int F_{n} = {v};")
    for n, v in ET.items():
        out.append(f"  localparam logic [3:0] ET_{n.upper()} = 4'd{v};")
    for n, v in SEGS.items():
        out.append(f"  localparam logic [3:0] SEG_{n} = 4'd{v};")
    out.append("endpackage")
    return "\n".join(out) + "\n"
