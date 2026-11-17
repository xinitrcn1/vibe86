#!/usr/bin/env python3
"""Regression tests for the SDM extractor and the microcode compiler (ucc).

    test_ucc.py            (environment: VIBE86_EXTRACTED, VIBE86_UCODE_PKG)

Every test here is a bug the compiler had once, pinned down with the inputs
around it (not only the one that failed):

  * the pseudocode parser's ELSE / ELSE IF / FI chains (DIV, WALK, PUSH);
  * inlined library functions clobbering their callers' variables;
  * the decoder tables: which form an instruction's bytes select (NOP vs
    XCHG vs PAUSE, mandatory 66h, operand-size keys of MOV CR/Sreg, RET
    imm16, CRC32 r/m16, moffs, SETcc's ModRM, ...), checked through a model
    of v1_core's lookup over the generated v1_ucode_pkg.sv;
  * the extractor's opcode-table layouts (stacked 'Opcode/Instruction'
    tables, 'Opcode*/', tables without an instruction column);
  * the microcode ROM fitting the micro-address.
"""
import os
import re
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path[:0] = [os.path.join(ROOT, "tools", "ucc"), os.path.join(ROOT, "tools", "ucc", "targets", "v1"),
                os.path.join(ROOT, "tools", "sdm")]

import sdm                      # noqa: E402
import forms as F               # noqa: E402
import lower as L               # noqa: E402
import oracle                   # noqa: E402
import uop as U                 # noqa: E402

EXTRACTED = os.environ.get("VIBE86_EXTRACTED", "")
UCODE_PKG = os.environ.get("VIBE86_UCODE_PKG", "")


# ------------------------------------------------------------------ parser

def chain(node):
    """The arms of an IF / ELSE IF chain: [(condition, then-block)], else-block."""
    arms = []
    while True:
        arms.append((node.cond, node.then))
        if node.els and len(node.els) == 1 and isinstance(node.els[0], sdm.If):
            node = node.els[0]
            continue
        return arms, node.els


def assigns(block):
    return [(s.dst.n, s.src.v) for s in block if isinstance(s, sdm.Assign)
            and isinstance(s.dst, sdm.Name) and isinstance(s.src, sdm.Num)]


class Parser(unittest.TestCase):
    def test_else_if_chain_closed_by_inner_fi(self):
        # DIV: each ELSE IF arm ends with its own FI, then one FI for all
        t = sdm.parse("""IF A = 1
    THEN X := 1;
    ELSE IF A = 2
        THEN X := 2;
        FI;
    ELSE IF A = 3
        THEN X := 3;
        FI;
    ELSE IF A = 4
        THEN X := 4;
        FI;
FI;
Y := 5;""")
        self.assertEqual(len(t), 2)
        arms, els = chain(t[0])
        self.assertEqual([assigns(b) for _, b in arms], [[("X", i)] for i in (1, 2, 3, 4)])
        self.assertFalse(els)
        self.assertEqual(assigns(t[1:]), [("Y", 5)])

    def test_outer_else_after_inner_fi(self):
        # WALK: an ELSE at the outer IF's column after the inner IF's FI
        t = sdm.parse("""IF A = 1
    THEN
        IF B = 1
            THEN X := 1;
        FI;
    ELSE X := 2;
FI;""")
        self.assertEqual(len(t), 1)
        outer = t[0]
        self.assertEqual(assigns(outer.els), [("X", 2)])
        inner = outer.then[0]
        self.assertIsInstance(inner, sdm.If)
        self.assertFalse(inner.els)

    def test_inner_else_stays_inner(self):
        # PUSH: the inner IF has its own ELSE; the outer ELSE is the outer's
        t = sdm.parse("""IF A = 1
    THEN
        IF B = 1
            THEN X := 1;
            ELSE X := 3;
        FI;
    ELSE X := 2;
FI;""")
        outer = t[0]
        inner = outer.then[0]
        self.assertEqual(assigns(inner.els), [("X", 3)])
        self.assertEqual(assigns(outer.els), [("X", 2)])

    def test_nested_chains(self):
        # an ELSE IF chain inside an arm of another chain
        t = sdm.parse("""IF A = 1
    THEN
        IF B = 1 THEN X := 1;
        ELSE IF B = 2 THEN X := 2;
        ELSE X := 3;
        FI;
    ELSE IF A = 2
        THEN X := 4;
    ELSE X := 5;
FI;
Z := 9;""")
        self.assertEqual(len(t), 2)
        arms, els = chain(t[0])
        self.assertEqual(len(arms), 2)
        inner_arms, inner_els = chain(arms[0][1][0])
        self.assertEqual([assigns(b) for _, b in inner_arms], [[("X", 1)], [("X", 2)]])
        self.assertEqual(assigns(inner_els), [("X", 3)])
        self.assertEqual(assigns(arms[1][1]), [("X", 4)])
        self.assertEqual(assigns(els), [("X", 5)])

    @unittest.skipUnless(EXTRACTED, "VIBE86_EXTRACTED not set")
    def test_div_page(self):
        page = sdm.read_sdm(os.path.join(EXTRACTED, "DIV.sdm"))
        t = sdm.parse(page.operation)
        ifs = [s for s in t if isinstance(s, sdm.If)]
        self.assertEqual(len(ifs), 2)              # SRC = 0, and the size chain
        arms, els = chain(ifs[1])
        self.assertEqual(len(arms), 4)             # 8, 16, 32, 64
        for _, then in arms:
            inner = [s for s in then if isinstance(s, sdm.If)]
            self.assertEqual(len(inner), 1)
            self.assertTrue(inner[0].els)          # #DE or the result


# ------------------------------------------------------------ inlining

def run(code, regs):
    """Interpret a straight-line micro-op routine (the ops these tests use)."""
    st = dict(regs)
    get = lambda u, s: u["imm"] if s == "K" else st.get(s, 0)   # noqa: E731
    for u in code:
        if "label" in u or u["op"] == "END":
            continue
        a, b = get(u, u["a"]), get(u, u["b"])
        m = (1 << u["sz"]) - 1
        r = {"MOV": a, "ADD": a + b, "SUB": a - b, "AND": a & b, "OR": a | b,
             "XOR": a ^ b, "SHL": a << (b & 63), "SHR": (a & m) >> (b & 63)}[u["op"]]
        st[u["dst"]] = r & m
    return st


def lower(op, lib):
    lib = dict(lib, __subroutines__=set())
    page = sdm.Page(name="T", title="T", source="")
    f = F.Form(page="T", mnem="T", text="T", opcol="", open="", osz=32, mode=3)
    return L.Lowerer(f, page, sdm.parse(op), oracle.flags_spec("None."), lib).lower().code


class Widths(unittest.TestCase):
    def test_extend_takes_a_memory_operands_width(self):
        # 'temp := SRC1 - SignExtend(SRC2)' (CMP): at SRC1's width also when
        # SRC1 is in memory (it was 64 bits: SF/OF from bit 63)
        for osz in (8, 16, 32):
            page = sdm.Page(name="T", title="T", source="")
            f = F.Form(page="T", mnem="T", text="T", opcol="", open="", osz=osz, mode=3)
            f.operands = [F.Operand("rm", osz, text="r/m"), F.Operand("reg", osz, text="r")]
            f.mem = True
            code = L.Lowerer(f, page, sdm.parse("temp := SRC1 - SignExtend(SRC2);\nModifyStatusFlags;"),
                             oracle.flags_spec("The CF, OF, SF, ZF, AF, and PF flags are set according to the result."),
                             {"__subroutines__": set()}).lower().code
            subs = [u for u in code if u.get("op") == "SUB"]
            self.assertEqual([u["sz"] for u in subs], [osz], code)


    def test_memory_of_a_memory_operand_is_the_operand(self):
        # 'off := Memory[DEST]' with DEST = m16:32 (far JMP/CALL, LxS, BOUND):
        # one load at the operand's address, not a load through the offset
        page = sdm.Page(name="T", title="T", source="")
        f = F.Form(page="T", mnem="T", text="T", opcol="", open="", osz=32, mode=3)
        f.operands = [F.Operand("farmem", 48, text="m16:32")]
        f.mem = True
        code = L.Lowerer(f, page, sdm.parse("off := Memory[DEST];\nRAX := off;"),
                         oracle.flags_spec("None."), {"__subroutines__": set()}).lower().code
        loads = [u for u in code if u.get("op") == "LD"]
        self.assertEqual(len(loads), 1, code)
        self.assertEqual(loads[0]["a"], "EA")


class Inline(unittest.TestCase):
    LIB = {
        # assigns its parameter: must not reach the caller's variable
        "Mix": (["X", "Y"], sdm.parse("a := X + 3;\nX := 0;\nRESULT := a XOR Y;\n")),
        # calls Mix with its own parameter and local: two levels deep
        "Outer": (["P"], sdm.parse("q := P + 1;\nr := Mix(q, P);\nP := 7;\nRESULT := q + r;\n")),
    }

    def test_parameter_assignment_is_local(self):
        code = lower("t := EAX + 1;\nu := ECX AND 0FFH;\nv := Mix(t, u);\n"
                     "EBX := t + u + v;\nEDX := t;", self.LIB)
        st = run(code, {"RAX": 10, "RCX": 0x1234})
        t, u = 11, 0x34
        v = (t + 3) ^ u
        self.assertEqual(st["RDX"], t)
        self.assertEqual(st["RBX"], t + u + v)

    def test_nested_inlining_keeps_every_callers_variables(self):
        code = lower("t := EAX + 1;\nw := Outer(t);\nEBX := t;\nEDX := w;\n"
                     "ESI := Mix(t, w);\nEDI := t + w;", self.LIB)
        st = run(code, {"RAX": 100})
        t = 101
        q = t + 1
        r = (q + 3) ^ t
        w = q + r
        self.assertEqual(st["RBX"], t)
        self.assertEqual(st["RDX"], w)
        self.assertEqual(st["RSI"], (t + 3) ^ w)
        self.assertEqual(st["RDI"], t + w)

    def test_argument_used_after_the_call(self):
        # the argument temp lives through the call and is read afterwards
        code = lower("t := EAX XOR 55H;\nv := Mix(t, t);\nEBX := v;\nECX := t;", self.LIB)
        st = run(code, {"RAX": 0x1200})
        t = 0x1255
        self.assertEqual(st["RCX"], t)
        self.assertEqual(st["RBX"], (t + 3) ^ t)


# ------------------------------------------------------------ decode tables

class Table:
    """dec_attr and uentry of a generated v1_ucode_pkg.sv, and v1_core's
    decode of instruction bytes into their keys (rtl/v1/v1_core.sv: prefix
    decode, calc_osz, asz_now, plus_base, the S_FINISH lookup)."""

    def __init__(self, path):
        with open(path) as fh:
            text = fh.read()
        self.attr = []
        for m in re.finditer(r"14'b([01?_]+): d = '\{(.*?)\};", text):
            f = {k: int(v, 2 if base == "b" else 10)
                 for k, base, v in re.findall(r"(\w+): \d+'([db])(\w+)", m.group(2))}
            self.attr.append((m.group(1).replace("_", ""), f))
        self.modrm = [m.group(1).replace("_", "") for m in re.finditer(r"11'b([01?_]+): return 1'b1;", text)]
        self.entry = [(m.group(1).replace("_", ""), m.group(2))
                      for m in re.finditer(r"24'b([01?_]+): return \d+'d\d+;\s*// (.*)", text)]
        self.uaddr_w = int(re.search(r"localparam int UADDR_W = (\d+);", text).group(1))
        self.urom_n = int(re.search(r"localparam int UROM_N = (\d+);", text).group(1))

    @staticmethod
    def match(pat, bits):
        return len(pat) == len(bits) and all(p in ("?", b) for p, b in zip(pat, bits))

    def dec_attr(self, m64, mp, opc, digit):
        bits = f"{m64:b}{mp:02b}{opc:08b}{digit:03b}"
        for p, a in self.attr:
            if self.match(p, bits):
                return a
        return {"valid": 0}

    def dec_modrm(self, m64, mp, opc):
        return any(self.match(p, f"{m64:b}{mp:02b}{opc:08b}") for p in self.modrm)

    def decode(self, hexbytes, mode=3, ssz=1):
        """bytes -> the form (comment of the matching uentry line) or 'UD'."""
        b = bytes.fromhex(hexbytes.replace(" ", ""))
        i = 0
        p66 = p67 = False
        prep = 0
        rex = None
        while True:
            x = b[i]
            if x in (0x66, 0x67, 0xF0, 0xF2, 0xF3, 0x2E, 0x36, 0x3E, 0x26, 0x64, 0x65):
                p66 |= x == 0x66
                p67 |= x == 0x67
                if x in (0xF2, 0xF3):
                    prep = 2 if x == 0xF2 else 1
                rex = None                       # a REX must come last
                i += 1
            elif mode == 3 and 0x40 <= x <= 0x4F:
                rex = x
                i += 1
            else:
                break
        mp = 0
        if b[i] == 0x0F:
            i += 1
            mp = 1
            if b[i] in (0x38, 0x3A):
                mp = 2 if b[i] == 0x38 else 3
                i += 1
        opc = b[i]
        i += 1
        m64 = int(mode == 3)
        base = opc
        if not (self.dec_attr(m64, mp, opc, 0)["valid"] or self.dec_modrm(m64, mp, opc)) \
                and self.dec_attr(m64, mp, opc & 0xF8, 0).get("plus_r"):
            base = opc & 0xF8
        modrm = b[i] if self.dec_modrm(m64, mp, base) and i < len(b) else 0
        digit = (modrm >> 3) & 7
        at = self.dec_attr(m64, mp, base, digit)
        if not at["valid"]:
            return "UD"
        w = bool(rex and rex & 8)
        cls = at["cls"]
        if mode == 3:
            osz = 3 if cls == 2 or w else (1 if p66 else (3 if cls == 1 else 2))
        elif mode == 2:
            osz = 1 if p66 else 2
        else:
            osz = 2 if p66 else 1
        asz = {3: 1 if p67 else 2, 2: 0 if p67 else 1}.get(mode, 1 if p67 else 0)
        pk = 0
        if at["pfx_sens"]:
            pk = prep if prep else (3 if at.get("p66") and p66 else 0)
        mem = int((at["modrm"] and (modrm >> 6) != 3) or bool(at["moffs"]))
        sub90 = int(mp == 0 and opc == 0x90 and not (rex and rex & 1))
        mode2 = {0: 0, 1: 1, 2: 2, 3: 3}[mode]
        key = f"{sub90}{mode2:02b}{mp:02b}{base:08b}{digit:03b}{pk:02b}{osz:02b}{mem}{asz:02b}{ssz}"
        for p, name in self.entry:
            if self.match(p, key):
                return name
        return "UD"


@unittest.skipUnless(UCODE_PKG and os.path.exists(UCODE_PKG), "VIBE86_UCODE_PKG not set")
class Decode(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.t = Table(UCODE_PKG)

    def form(self, hexbytes, want, mode=3):
        got = self.t.decode(hexbytes, mode)
        self.assertTrue(got.startswith(want) if want != "UD" else got == "UD",
                        f"{hexbytes} (mode {mode}): {got!r}, expected {want!r}")

    def test_rom_fits_the_micro_address(self):
        self.assertLessEqual(self.t.urom_n, 1 << self.t.uaddr_w)
        self.assertEqual(self.t.uaddr_w, U.UADDR_W)

    def test_nop_xchg_pause(self):
        self.form("90", "NOP")
        self.form("41 90", "XCHG")              # REX.B: XCHG r8, rAX
        self.form("49 90", "XCHG")              # REX.WB: XCHG r8, rAX (64-bit)
        self.form("48 90", "NOP")               # REX.W alone is still NOP
        self.form("66 90", "NOP")
        self.form("66 0F 1F 00", "NOP")
        self.form("48 0F 1F 44 00 00", "NOP")
        self.form("0F 1F 40 00", "NOP")
        self.form("F3 90", "PAUSE")
        self.form("91", "XCHG")
        self.form("90", "NOP", mode=0)
        self.form("F3 90", "PAUSE", mode=0)

    def test_mandatory_66(self):
        self.form("66 0F 38 F6 C3", "ADCX r32")
        self.form("66 44 0F 38 F6 DB", "ADCX r32")    # REX.R
        self.form("66 48 0F 38 F6 FE", "ADCX r64")
        self.form("66 49 0F 38 F6 F2", "ADCX r64")
        self.form("F3 0F 38 F6 C3", "ADOX r32")
        self.form("F3 48 0F 38 F6 C3", "ADOX r64")
        self.form("0F 38 F6 C3", "UD")                # no prefix: nothing
        self.form("66 0F 38 F6 C3", "ADCX r32", mode=0)
        self.form("66 0F 38 F6 C3", "ADCX r32", mode=2)
        self.form("66 F3 90", "PAUSE")
        self.form("F3 48 90", "PAUSE")
        self.form("66 0F AE 30", "CLWB")
        self.assertIn("66 0F AE /7", self.t.decode("66 0F AE 38"))   # CLFLUSHOPT
        self.assertIn("NP 0F AE /7", self.t.decode("0F AE 38"))   # CLFLUSH
        self.form("66 0F 38 F8 00", "MOVDIR64B")
        self.form("66 0F 38 82 00", "INVPCID")

    def test_66_is_operand_size_elsewhere(self):
        # in groups without a 66h-mandatory form, 66h stays the size prefix
        self.form("66 0F C7 F0", "RDRAND r16")
        self.form("0F C7 F0", "RDRAND r32")
        self.form("48 0F C7 F0", "RDRAND r64")
        self.form("66 0F 38 F0 00", "MOVBE r16")
        self.form("66 F3 0F B8 C0", "POPCNT r16")
        self.form("F3 0F B8 C0", "POPCNT r32")

    def test_crc32_operand_sizes(self):
        self.form("F2 0F 38 F0 C0", "CRC32 r32, r/m8")
        self.form("F2 48 0F 38 F0 C0", "CRC32 r64, r/m8")
        self.form("66 F2 0F 38 F1 C0", "CRC32 r32, r/m16")
        self.form("F2 66 0F 38 F1 C0", "CRC32 r32, r/m16")
        self.form("F2 0F 38 F1 C0", "CRC32 r32, r/m32")
        self.form("F2 48 0F 38 F1 C0", "CRC32 r64, r/m64")
        self.form("66 F2 44 0F 38 F1 55 0A", "CRC32 r32, r/m16")
        self.form("0F 38 F1 00", "MOVBE m32, r32")

    def test_ret_imm16_operand_size(self):
        self.form("C2 08 00", "RET")
        self.form("66 C2 08 00", "RET")
        self.form("C2 08 00", "RET", mode=0)
        self.form("66 C2 08 00", "RET", mode=0)
        self.form("CA 08 00", "RET imm16 [CA iw] m3 o32")
        # far returns default to 32 bits in 64-bit mode (the near ones are 64)
        self.form("CB", "RET [CB] m3 o32")
        self.form("48 CB", "RET [CB] m3 o64")
        self.form("66 CB", "RET [CB] m3 o16")
        self.form("C3", "RET [C3] m3 o64")
        self.form("66 C3", "RET [C3] m3 o64")

    def test_mov_cr_dr_sreg_ignore_operand_size(self):
        for pfx in ("", "66 ", "48 "):
            self.form(pfx + "0F 20 C0", "MOV r64, CR0")
            self.form(pfx + "0F 22 D8", "MOV CR0–CR7, r64")
            self.form(pfx + "0F 21 F8", "MOV r64, DR0–DR7")
            self.form(pfx + "0F 23 C0", "MOV DR0")
        self.form("0F 20 C0", "MOV r32, CR0", mode=2)
        self.form("66 0F 20 C0", "MOV r32, CR0", mode=2)
        self.form("8E D8", "MOV Sreg")
        self.form("66 8E D8", "MOV Sreg")
        self.form("8E D8", "MOV Sreg", mode=0)
        # 8C: the register form at every size (zero-extended selector), the
        # memory form a 16-bit store whatever the operand size
        self.form("66 8C D8", "MOV r/m16, Sreg [8C /r] m3 o16:r")
        self.form("8C D8", "MOV r16/r32/m16, Sreg2 [8C /r] m3 o32:r")
        self.form("48 8C D8", "MOV r64/m16, Sreg2 [REX.W + 8C /r] m3 o64:r")
        for pfx in ("", "66 ", "48 "):
            self.form(pfx + "8C 18", "MOV r/m16, Sreg [8C /r] m3 o16:m")
        self.form("8C D8", "MOV r16/r32/m16, Sreg2 [8C /r] m2 o32:r", mode=2)
        self.form("8C 18", "MOV r/m16, Sreg [8C /r] m2 o16:m", mode=2)

    def test_far_pointers(self):
        self.form("EA 00 00 00 F0 08 00", "JMP ptr16:32", mode=2)
        self.form("66 EA 00 00 F0 08", "JMP ptr16:16", mode=2)
        self.form("EA 00 00 F0 08", "JMP ptr16:16", mode=0)
        self.form("9A 00 00 00 F0 08 00", "CALL ptr16:32", mode=2)
        self.form("FF 28", "JMP m16:32")
        self.form("66 FF 28", "JMP m16:16")
        self.form("48 FF 28", "JMP m16:64")
        self.form("FF 18", "CALL m16:32")
        self.form("48 FF 18", "CALL m16:64")
        self.form("66 FF 18", "CALL m16:16")
        self.form("FF 18", "CALL m16:32", mode=2)
        self.form("FF 18", "CALL m16:16", mode=0)
        self.form("FF 10", "CALL r/m64")                # near: forced 64-bit
        self.form("FF 20", "JMP r/m64")
        self.form("EA 00 00 00 F0 08 00", "UD")       # invalid in 64-bit mode

    def test_moffs(self):
        self.form("A0 00 00 00 00 00 00 00 00", "MOV AL, moffs8")
        self.form("A1 00 00 00 00 00 00 00 00", "MOV EAX, moffs32")
        self.form("48 A3 00 00 00 00 00 00 00 00", "MOV moffs64")
        self.form("67 A1 00 00 00 00", "MOV EAX, moffs32")
        self.form("A1 00 00", "MOV AX, moffs16", mode=0)

    def test_setcc_has_modrm(self):
        for opc in range(0x90, 0xA0):
            self.form(f"0F {opc:02X} C0", "SET")
            self.form(f"0F {opc:02X} 00", "SET")
        self.assertTrue(self.t.dec_modrm(1, 1, 0x94))

    def test_tables_without_instruction_column(self):
        self.form("0F 32", "RDMSR")
        self.form("0F AE E8", "LFENCE")
        self.form("0F AE F0", "MFENCE")
        self.form("0F AE F8", "SFENCE")
        self.form("F3 0F BC C0", "TZCNT")
        self.form("F3 0F BD C0", "LZCNT")
        self.form("0F BC C0", "BSF")
        self.form("0F BD C0", "BSR")

    def test_lar_lsl_footnote_operands(self):
        self.form("0F 02 C0", "LAR")
        self.form("0F 02 00", "LAR")
        self.form("48 0F 03 C0", "LSL")
        self.form("66 0F 03 00", "LSL")

    def test_legacy_instructions_seabios_needed(self):
        self.form("F3 6C", "INS")
        self.form("F3 6E", "OUTS")
        self.form("0F A4 C0 04", "SHLD")
        self.form("0F AD C0", "SHRD")
        self.form("0F 09", "WBINVD")
        self.form("F3 0F 09", "WBNOINVD")
        self.form("0F 08", "INVD")
        self.form("99", "CDQ")
        self.form("66 99", "CWD")
        self.form("48 99", "CQO")
        self.form("C8 10 00 01", "ENTER")          # 64-bit: operand size 64
        self.form("C8 10 00 01", "ENTER imm16, imm8 [C8 iw ib] m2 o32", mode=2)
        self.form("66 C8 10 00 01", "ENTER imm16, imm8 [C8 iw ib] m2 o16", mode=2)
        self.form("C8 10 00 01", "ENTER imm16, imm8 [C8 iw ib] m0 o16", mode=0)
        self.form("D5 0A", "AAD", mode=2)
        self.form("D4 0A", "AAM", mode=2)
        # invalid in 64-bit mode: #UD, not the opcode 8-aligned below them
        for b in ("06", "07", "0E", "16", "17", "1E", "1F", "27", "2F", "37", "3F",
                  "60", "61", "62 00", "82 C0 01", "9A 00 00 00 00 00 00", "C4 C0", "C5 C0",
                  "CE", "D4 0A", "D5 0A", "D6", "EA 00 00 00 00 00 00"):
            self.form(b, "UD")
        self.form("57", "PUSH r64")                 # +r opcodes still fold
        self.form("41 5F", "POP r64")
        self.form("BF 01 00 00 00", "MOV r32, imm32")
        self.form("0F CF", "BSWAP r32")


# --------------------------------------------------------------- extractor

class Extractor(unittest.TestCase):
    def test_stacked_rows(self):
        import extract
        body = [(l, 0) for l in """ADCX—Unsigned Integer Addition of Two Operands With Carry Flag
 Opcode/                               Op/     64/32bit     CPUID       Description
 Instruction                           En      Mode         Feature
                                               Support      Flag
 66 0F 38 F6 /r                        RM      V/V          ADX         Unsigned addition of r32 with CF, r/m32 to r32, writes CF.
 ADCX r32, r/m32
 66 REX.w 0F 38 F6 /r                  RM      V/N.E.       ADX         Unsigned addition of r64 with CF, r/m64 to r64, writes CF.
 ADCX r64, r/m64

                                                     Instruction Operand Encoding
""".splitlines()]
        rows = extract.opcode_rows_stacked(body)
        self.assertEqual(rows, [["66 0F 38 F6 /r", "ADCX r32, r/m32", "RM", "Valid", "Valid"],
                                ["66 REX.w 0F 38 F6 /r", "ADCX r64, r/m64", "RM", "Valid", "N.E."]])

    def page(self, name):
        return sdm.read_sdm(os.path.join(EXTRACTED, name + ".sdm"))

    @unittest.skipUnless(EXTRACTED, "VIBE86_EXTRACTED not set")
    def test_extracted_layouts(self):
        rows = self.page("ADCX").opcodes
        self.assertEqual([r[1] for r in rows], ["ADCX r32, r/m32", "ADCX r64, r/m64"])
        rows = self.page("ADDPS").opcodes
        self.assertEqual(len(rows), 6)
        self.assertTrue(any(r[0].startswith("EVEX.512") and "{er}" in r[1] for r in rows))
        rows = self.page("RDRAND").opcodes                  # 'Opcode*/'
        self.assertEqual([r[1] for r in rows], ["RDRAND r16", "RDRAND r32", "RDRAND r64"])
        self.assertEqual(rows[2][3:], ["Valid", "N.E."] if rows[2][4] == "N.E." else rows[2][3:])
        rows = self.page("CLAC").opcodes
        self.assertEqual(rows, [["NP 0F 01 CA", "CLAC", "ZO", "Valid", "Valid"]])
        for name in ("VPDPBUSD", "MOVDIR64B", "PREFETCHh", "WBNOINVD", "INVPCID"):
            for r in self.page(name).opcodes:
                self.assertEqual(len(r), 5, f"{name}: {r}")
                self.assertTrue(r[1], f"{name}: no instruction column: {r}")
                self.assertIn(r[3], ("Valid", "Invalid", "N.E.", "N.S."), f"{name}: {r}")

    def test_forms_of_tricky_rows(self):
        def forms(rows, enc, cfg=None):
            page = sdm.Page(name="X", title="X", source="", opcodes=rows, encoding=enc)
            fs, errs = F.make_forms(page, cfg or {})
            self.assertEqual(errs, [])
            return fs
        # lowercase REX.w (ADCX's table)
        fs = forms([["66 REX.w 0F 38 F6 /r", "ADCX r64, r/m64", "RM", "Valid", "N.E."]],
                   {"RM": ["ModRM:reg (r, w)", "ModRM:r/m (r)"]})
        self.assertTrue(all(f.rexw and f.osz == 64 for f in fs))
        # r16/r32/r64 (a register of the address size) and a 512-bit memory operand
        fs = forms([["66 0F 38 F8 /r", "MOVDIR64B r16/r32/r64, m512", "A", "Valid", "Valid"]],
                   {"A": ["ModRM:reg (w)", "ModRM:r/m (r)"]})
        self.assertTrue(fs and all(f.osz <= 64 for f in fs))
        # two Op/En rows named 'O' (XCHG): the +rd register is the opcode's
        fs = forms([["90+rd", "XCHG EAX, r32", "O", "Valid", "Valid"],
                    ["90+rd", "XCHG r32, EAX", "O", "Valid", "Valid"]],
                   {"O": ["opcode + rd (r, w)", "AX/EAX/RAX (r, w)"]})
        for f in fs:
            self.assertEqual(sorted(o.kind for o in f.operands), ["fixed", "opreg"], f.text)
        # 8C at operand size 32/64: register forms only
        fs = forms([["8C /r", "MOV r16/r32/m16, Sreg2", "MR", "Valid", "Valid"]],
                   {"MR": ["ModRM:r/m (w)", "ModRM:reg (r)"]})
        self.assertTrue(fs and all(f.hints.get("regform") and f.osz == 32 for f in fs))
        # CRC32 r32, r/m16: the source has the operand size (isa option oszsrc)
        fs = forms([["F2 0F 38 F1 /r", "CRC32 r32, r/m16", "RM", "Valid", "Valid"]],
                   {"RM": ["ModRM:reg (r, w)", "ModRM:r/m (r)"]}, {"oszsrc": True})
        self.assertTrue(fs and all(f.osz == 16 for f in fs))


if __name__ == "__main__":
    unittest.main()
