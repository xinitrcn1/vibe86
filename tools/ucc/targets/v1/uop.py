"""The v1 micro-operation ISA: the contract between ucc and rtl/v1.

One micro-op (uop) is a 72-bit horizontal word:

    [71:65] op      operation (OPS below)
    [64:63] sz      operand size: 0 = 8, 1 = 16, 2 = 32, 3 = 64 bits
    [62:57] dst     destination selector (SEL below)
    [56:51] a       first source selector
    [50:45] b       second source selector
    [44:39] fmask   RFLAGS bits this uop may write: OF SF ZF AF PF CF (bit 5..0)
    [38:35] cc      x86 condition (JCC, SETCC, CMOVCC) or micro-condition (JU)
    [34:32] x       op-specific small field (ZEXT/SEXT: destination size)
    [31:0]  imm     constant (selector K), uop address (jumps), special
                    register number (RDS/WRS), vector (EXC), bit field (SLICE)

Micro-ops run one at a time (rtl/v1 is not pipelined) against a *working*
copy of the architectural registers and flags; END commits the working
copy, a fault discards it (precise exceptions by checkpointing: Hwu & Patt,
"Checkpoint Repair for Out-of-order Execution Machines", ISCA 1987,
doi:10.1145/30350.30352). The format follows the horizontal microcode of
Wilkes & Stringer, "Micro-programming and the design of the control circuits
in an electronic digital computer", Proc. Cambridge Philosophical Society
49(2), 1953 (doi:10.1017/S0305004100028322): every field drives one part of
the datapath directly.

`ucc.py --pkg FILE` writes the SystemVerilog package rtl/v1 includes.
"""

WIDTH = 72
UADDR_W = 13   # micro-op address bits (8192-entry ROM)

# operation codes (keep in step with rtl/v1: the package is generated)
OPS = [
    "NOP",
    # ALU, dst := a op b at sz; RFLAGS bits in fmask written with the x86 result flags
    "ADD", "ADC", "SUB", "SBB", "AND", "OR", "XOR",
    "SHL", "SHR", "SAR", "ROL", "ROR", "RCL", "RCR",  # b = count (masked as x86 does)
    "NEG", "NOT", "MOV",
    "MULU", "MULS",        # dst := low half of a*b; HI := high half; CF=OF := high half significant
    "IMULT",               # dst := a*b truncated; CF=OF := truncation lost information (2/3-operand IMUL)
    "DIVU", "DIVS",        # dst := (HI:a) / b, REM := remainder, at sz; #DE on b = 0 or overflow
    "ZEXT", "SEXT",        # dst (at size x) := extend(a at sz)
    "SETCC",               # dst := cc(RFLAGS) ? 1 : 0
    "BSF", "BSR", "TZCNT", "LZCNT", "POPCNT", "BSWAP",
    "SZP",                 # SF, ZF, PF (fmask) := of a at sz
    "GETF",                # dst := RFLAGS[imm] (one bit)
    "SETF",                # RFLAGS[imm] := a[0]
    "SLICE",               # dst := (a >> imm[7:0]) & ones(imm[15:8])
    "INSERT",              # dst := dst with bits [imm[7:0] +: imm[15:8]] := a
    "KHI",                 # dst := dst[31:0] | imm << 32
    "CMP",                 # micro-flags := compare(a, b) at sz (RFLAGS untouched)
    # sequencing
    "JMP",                 # upc := imm
    "JCC",                 # upc := imm if x86 condition cc holds on the working RFLAGS
    "JU",                  # upc := imm if micro-condition cc holds on the micro-flags
    "END",                 # instruction done: commit, RIP := NRIP
    "COMMIT",              # commit the working state without ending (REP iterations)
    # memory and I/O
    "LD",                  # dst := mem[a + sext(imm)] (sz)
    "ST",                  # mem[a + sext(imm)] := b (sz)
    "IN",                  # dst := io[a] (sz <= 32)
    "OUT",                 # io[a] := b
    # system
    "RDS",                 # dst := special[imm]
    "WRS",                 # special[imm] := a
    "EXC",                 # raise vector imm[7:0]; error code a if imm[8]
    "HLT",                 # wait for an interrupt
    "SHUTDOWN",            # triple fault: stop
]
OP = {n: i for i, n in enumerate(OPS)}
assert len(OPS) <= 128

# operand selectors
SEL = {}
GPR64 = ["RAX", "RCX", "RDX", "RBX", "RSP", "RBP", "RSI", "RDI",
         "R8", "R9", "R10", "R11", "R12", "R13", "R14", "R15"]
for i, n in enumerate(GPR64):
    SEL[n] = i
for i, n in enumerate(["AH", "CH", "DH", "BH"]):
    SEL[n] = 16 + i
NTEMPS = 16
for i in range(NTEMPS):
    SEL[f"T{i}"] = 20 + i
SEL.update({
    "REG": 36,    # ModRM.reg (+REX.R); AH..BH for sz 8 without REX
    "RM": 37,     # ModRM.rm (+REX.B) register, or the register in the opcode (+rb/+rw/+rd)
    "IMM": 38,    # the immediate, sign-extended to 64 bits
    "EA": 39,     # the memory operand's effective address (with the FS/GS base)
    "NRIP": 40,   # next RIP (write: a jump)
    "RIP": 41,    # this instruction's RIP
    "FLAGS": 42,  # RFLAGS
    "K": 43,      # imm, sign-extended
    "ZERO": 44,
    "HI": 45,     # the multiplier's high half / the divider's high dividend
    "REM": 46,    # the divider's remainder
    "IMM2": 47,   # second immediate (ENTER's ib)
    "ONE": 48,    # the constant 1
})
NSEL = 64

# x86 condition codes (the low nibble of Jcc/SETcc/CMOVcc)
CC = {"O": 0, "NO": 1, "B": 2, "C": 2, "NAE": 2, "AE": 3, "NB": 3, "NC": 3,
      "E": 4, "Z": 4, "NE": 5, "NZ": 5, "BE": 6, "NA": 6, "A": 7, "NBE": 7,
      "S": 8, "NS": 9, "P": 10, "PE": 10, "NP": 11, "PO": 11,
      "L": 12, "NGE": 12, "GE": 13, "NL": 13, "LE": 14, "NG": 14, "G": 15, "NLE": 15}

# micro-conditions for JU, on the flags CMP left (unsigned/signed compare)
UCC = {"EQ": 0, "NE": 1, "LTU": 2, "GEU": 3, "LEU": 4, "GTU": 5,
       "LTS": 6, "GES": 7, "LES": 8, "GTS": 9, "ALWAYS": 15}
UCC_NEG = {"EQ": "NE", "NE": "EQ", "LTU": "GEU", "GEU": "LTU", "LEU": "GTU",
           "GTU": "LEU", "LTS": "GES", "GES": "LTS", "LES": "GTS", "GTS": "LES"}

# RFLAGS bit positions
FLAG = {"CF": 0, "PF": 2, "AF": 4, "ZF": 6, "SF": 7, "TF": 8, "IF": 9, "DF": 10,
        "OF": 11, "IOPL": 12, "NT": 14, "RF": 16, "VM": 17, "AC": 18, "VIF": 19,
        "VIP": 20, "ID": 21}
# fmask bit for each status flag
FMASK = {"CF": 1 << 0, "PF": 1 << 1, "AF": 1 << 2, "ZF": 1 << 3, "SF": 1 << 4, "OF": 1 << 5}
STATUS = ["CF", "PF", "AF", "ZF", "SF", "OF"]

# special registers (RDS/WRS)
SPECIAL = {
    "CR0": 0, "CR2": 2, "CR3": 3, "CR4": 4, "CR8": 8,
    "EFER": 16, "FS_BASE": 17, "GS_BASE": 18, "KERNEL_GS_BASE": 19,
    "TSC": 20, "STAR": 21, "LSTAR": 22, "CSTAR": 23, "FMASK": 24,
    "TSC_AUX": 25, "APIC_BASE": 26, "PAT": 27, "MISC_ENABLE": 28,
    "IDTR_BASE": 32, "IDTR_LIMIT": 33, "GDTR_BASE": 34, "GDTR_LIMIT": 35,
    "CS": 40, "SS": 41, "DS": 42, "ES": 43, "FS": 44, "GS": 45, "LDTR": 46, "TR": 47,
    "CPL": 48,
    "FAULT_RIP": 56,   # delivery: the RIP of the faulting instruction / the next one for traps
    "FAULT_ERR": 57,   # delivery: the error code
    "FAULT_VEC": 58,   # delivery: the vector, bit 8 = has an error code
    "UCOUNT": 59,      # uops retired (a performance counter)
}

SIZES = {8: 0, 16: 1, 32: 2, 64: 3}


def encode(op, sz=64, dst=0, a=0, b=0, fmask=0, cc=0, x=0, imm=0):
    assert 0 <= imm < (1 << 32) or -(1 << 31) <= imm < 0, imm
    w = OP[op]
    w = (w << 2) | SIZES[sz]
    w = (w << 6) | dst
    w = (w << 6) | a
    w = (w << 6) | b
    w = (w << 6) | fmask
    w = (w << 4) | cc
    w = (w << 3) | x
    w = (w << 32) | (imm & 0xFFFFFFFF)
    return w


def sv_package(target="v1"):
    out = [f"// generated by tools/ucc/targets/{target}/uop.py: the {target} micro-op ISA (do not edit)",
           f"package {target}_uop_pkg;",
           f"  localparam int UOP_W = {WIDTH};",
           "  typedef enum logic [6:0] {"]
    out.append(",\n".join(f"    U_{n} = 7'd{i}" for i, n in enumerate(OPS)))
    out.append("  } uop_op_e;")
    out.append("  typedef struct packed {\n    uop_op_e op;\n    logic [1:0] sz;\n"
               "    logic [5:0] dst;\n    logic [5:0] a;\n    logic [5:0] b;\n"
               "    logic [5:0] fmask;\n    logic [3:0] cc;\n    logic [2:0] x;\n"
               "    logic [31:0] imm;\n  } uop_t;")
    for n, v in SEL.items():
        out.append(f"  localparam logic [5:0] S_{n} = 6'd{v};")
    for n, v in UCC.items():
        out.append(f"  localparam logic [3:0] UC_{n} = 4'd{v};")
    for n, v in SPECIAL.items():
        out.append(f"  localparam logic [31:0] SP_{n} = 32'd{v};")
    for n, v in FLAG.items():
        out.append(f"  localparam int F_{n} = {v};")
    out.append("endpackage")
    return "\n".join(out) + "\n"


