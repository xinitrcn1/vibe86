"""Vector instructions in ucc (D-015): the SDM's SIMD pseudocode lowered to
v1's lane-serial micro-ops.

Three parts:

* Operation sections. A vector page's Operation holds one section per
  encoding ("ADDPS (128-bit Legacy SSE Version)", "VADDPS (VEX.128 Encoded
  Version)", "VADDPS (EVEX Encoded Versions) When SRC2 Operand is a
  Register"); `split_sections` cuts the text at those headers and
  `pick_section` chooses the one a form runs. Text before the first header
  (helper definitions) belongs to every section.

* Text repairs before parsing: the manual elides lanes ("DEST[7:0] := ...;
  (* Repeat add operation for 2nd through 15th byte *) DEST[127:120] :=
  ...") -- `expand_repeats` interpolates the missing statements from the
  first and the last (their constants are arithmetic sequences); and
  "DEST[MAXVL-1:128] (Unmodified)" is a no-op.

* `VectorMixin`, mixed into lower.Lowerer: vector operands (`vreg`: an
  XMM/YMM/ZMM register, `vmem`: a vector in memory, `vslice`: a lazy bit
  range of either, at a static or dynamic offset), element reads (VGET or
  a load), element writes (VSET or a store), wide copies in 64-bit chunks,
  DEST[MAXVL-1:VL] := 0 (VZUP), and floating-point typing: elements of an
  FP page (PS/SS -> f32, PD/SD -> f64, PH/SH -> f16) carry their type, so
  + - * / comparisons, SQRT, MIN/MAX and the Convert_* helpers become FP
  micro-ops. A vector destination register is written through a vector
  temporary and copied at the end, after FPCHK: the sources are read as
  they were when the instruction started (the manual's statements assign
  lanes one by one, but mean them at once -- PSHUFD xmm1, xmm1 reads its
  source after writing lane 0), and an exception leaves the destination
  untouched.
"""
import re

from sdm import Bin, Call, Name, Num, Slice

MAXVL = 512

# ---------------------------------------------------------------- sections

HEADER = re.compile(
    r"^\s*(\(V\))?V?[A-Z][A-Z0-9]{1,}[^():=;]*"
    r"(\(([^()]*((?i:version|versions|instruction|encoded|form|legacy|vex|evex|bit|operand))[^()]*)\)"
    r"([^:=;]*)|\s+Instruction\s+With\s+\d+\s*-\s*bit\s+Operands\s*:)\s*$")


def is_header(ln, mnems):
    """A section header: the line starts with one of the page's mnemonics
    ('PUNPCKLBW', 'PSLLW (xmm, imm8)', 'VMOVQ (7E) With Memory Source',
    'MOVQ Instruction When ...'), or has the usual '(... Version)' shape."""
    if ":=" in ln or ";" in ln:
        return False
    if HEADER.match(ln):
        return True
    m = re.match(r"^\s*(\(V\))?([A-Z][A-Z0-9]+)\b(.*)$", ln)
    if not m or not mnems or m.group(2) not in mnems:
        return False
    rest = m.group(3).strip()
    return rest == "" or rest == ":" or rest.startswith(("(", "Instruction", "-", "With", "When")) \
        or bool(re.search(r"\([A-Za-z0-9/ ]+\)\s*$", rest))     # 'PINSRW dest, src, imm8 (SSE)'


def split_sections(text, mnems=None):
    """[(header, body)] (header None: the preamble); a page without headers
    is one section. A header's continuation line (lower case) joins it."""
    lines = text.split("\n")
    out, cur, body = [], None, []
    i = 0
    while i < len(lines):
        ln = lines[i]
        if is_header(ln, mnems):
            if body or cur is not None:
                out.append((cur, "\n".join(body)))
            cur, body = ln.strip(), []
            while i + 1 < len(lines) and re.match(r"^\s*[a-z][a-z ]+[a-z]+.*:\s*$", lines[i + 1]) \
                    and ":=" not in lines[i + 1]:
                cur += " " + lines[i + 1].strip()
                i += 1
        else:
            body.append(ln)
        i += 1
    out.append((cur, "\n".join(body)))
    return out


def page_mnemonics(stem, forms):
    """The mnemonics a page's section headers start with."""
    ms = set()
    for p in re.split(r"[_ ]", stem):
        if re.match(r"^[A-Z][A-Z0-9]+$", p):
            ms |= {p, "V" + p}
    for f in forms:
        ms |= {f.mnem.upper(), "V" + f.mnem.upper()}
    return ms


def form_encoding(f):
    """'legacy', 'vex' or 'evex' (forms.py sets f.enc)."""
    return getattr(f, "enc", "legacy")


def pick_section(sections, f):
    """The section text a form runs (the preamble prepended), or None if the
    page has no sections."""
    if len(sections) == 1 and sections[0][0] is None:
        return sections[0][1]
    pre = sections[0][1] if sections[0][0] is None else ""
    cands = [(h, b) for h, b in sections if h is not None]
    enc = form_encoding(f)
    mn = f.mnem.upper()

    def mnems(h):
        head = h[3:] if h.startswith("(V)") else h
        head = head.split("(", 1)[0]
        out = set()
        for alt in re.split(r"/", head):
            m = re.match(r"\s*(\(V\))?([A-Z][A-Z0-9]+)", alt)
            if m:
                out.add(m.group(2))
        return out

    def score(h):
        hs = h.upper()
        names = mnems(h)
        s = 0
        if enc == "legacy":
            if mn in names or ("(V)" + mn) in h:
                s += 10
            if "LEGACY" in hs:
                s += 5
            if ("VEX" in hs or "EVEX" in hs) and "LEGACY" not in hs:
                s -= 20
        else:
            vmn = mn if mn.startswith("V") else "V" + mn
            if vmn in names or mn in names or h.startswith("(V)"):
                s += 10
            if enc == "evex":
                s += 8 if "EVEX" in hs else -8
                if f.mem is True and "MEMORY" in hs:
                    s += 3
                if f.mem is False and "REGISTER" in hs:
                    s += 3
            else:
                if "EVEX" in hs:
                    s -= 20
                if "VEX" in hs:
                    s += 5
                vl = getattr(f, "vl", 128)
                if f"{vl}" in hs:
                    s += 3
        # 'With 64-bit Operands' (MMX) / 'With 128-bit Operands' (SSE)
        if re.search(r"64\s*-\s*BIT OPERAND", hs) and enc == "legacy":
            s -= 15
        if re.search(r"128\s*-\s*BIT OPERAND", hs):
            s += 3
        # 'When Destination Operand is an XMM Register' / '... is r/m64'
        dst_vec = bool(f.operands) and f.operands[0].kind in ("vreg", "vrm")
        if "DESTINATION" in hs and ("XMM" in hs or "MMX" in hs) and "MEMORY" not in hs:
            s += 3 if dst_vec else -6
        if "DESTINATION" in hs and ("R/M" in hs or "GENERAL" in hs):
            s += 3 if not dst_vec else -6
        if "SOURCE OPERAND IS" in hs:
            s += 3 if not dst_vec else -6
        # 'PSLLW (xmm, imm8)': the header's operand list against the form's
        ol = re.match(r"^\(?V?\)?[A-Z0-9]+\s*\(([^()]*)\)", h)
        if ol and "," in ol.group(1) or (ol and re.match(r"^\s*(xmm|ymm|zmm|r/?m?\d*|m\d+)", ol.group(1))):
            hops = [re.sub(r"\d+$", "", x.strip()) for x in ol.group(1).split(",")]
            fops = [re.sub(r"\d+$", "", o.text.split("/")[0]) if o.text else "" for o in f.operands]
            if [x.split("/")[0] for x in hops] == fops:
                s += 6
                if [x for x in hops] == [re.sub(r"\d+(?=/|$)", "", o.text) for o in f.operands]:
                    s += 1
            elif any(x.startswith(("xmm", "ymm", "zmm", "imm")) for x in hops):
                s -= 6
        if "MEMORY" in hs:
            src_mem = "SOURCE OPERAND IS MEMORY" in hs or "MEMORY SOURCE" in hs or \
                "SOURCE OPERAND IS MEMORY LOCATION" in hs
            dst_mem = "DESTINATION OPERAND IS MEMORY" in hs or "MEMORY DEST" in hs or \
                "OPERAND IS MEMORY LOCATION" in hs and "DESTINATION" in hs and "SOURCE OPERAND IS XMM" in hs
            if f.mem and src_mem and dst_vec and "SOURCE OPERAND IS XMM" not in hs:
                s += 6
            elif f.mem and dst_mem and not dst_vec:
                s += 6
            elif f.mem and dst_mem and dst_vec and f.operands[0].kind == "vrm":
                s += 6
            else:
                s -= 3
        if "XMM REGISTERS" in hs or "REGISTER SOURCE AND DESTINATION" in hs:
            s += 5 if not f.mem else -5
        if "MMX" in hs and "XMM" not in hs and enc == "legacy":
            s -= 12
        if "(SSE)" in hs and enc == "legacy":
            s += 3
        # load / store forms (MOVAPS xmm, m128 vs MOVAPS m128, xmm)
        store = bool(f.operands) and f.operands[0].kind in ("vrm",) and f.mem
        if "STORE" in hs:
            s += 4 if store else -4
        if "LOAD" in hs or "REGISTER-COPY" in hs:
            s += 2 if not store else -2
        return s

    best = max(cands, key=lambda c: score(c[0]))
    if score(best[0]) <= 0:
        # the legacy form's Operation is the text before the first header (PEXTRW)
        if enc == "legacy" and pre.strip():
            return pre
        return None
    return pre + "\n" + best[1]


# ---------------------------------------------------------------- text repairs

UNMODIFIED = re.compile(r"^\s*DEST\s*\[[^\]]*\]\s*\(\s*[Uu]nmodified\s*\)\s*;?\s*$", re.M)
REPEAT = re.compile(r"\(\*\s*(Repeat|Continue)[^*]*\*\)", re.I)
NUM = re.compile(r"\b\d+\b")


def expand_repeats(text):
    """Fill in the lanes the manual elides between a first and a last
    statement (or block of statements: PCMPEQB's IF/THEN/ELSE) with a
    '(* Repeat ... *)' / '(* Continue ... *)' comment."""
    lines = text.split("\n")
    out = []
    i = 0
    while i < len(lines):
        ln = lines[i]
        if REPEAT.search(ln) and out and i + 1 < len(lines):
            done = False
            for k in range(1, 7):
                if len(out) < k or i + k >= len(lines) + 0 and i + k > len(lines) - 1 + 1:
                    break
                first = "\n".join(out[-k:])
                last = "\n".join(lines[i + 1:i + 1 + k])
                gen = interpolate(first, last)
                if gen is not None:
                    for g in gen:
                        out.extend(g.split("\n"))
                    i += 1
                    done = True
                    break
            if done:
                continue
            i += 1                       # an elision we can't fill: drop the comment
            continue
        out.append(ln)
        i += 1
    return "\n".join(out)


def interpolate(first, last):
    """The statements between `first` (lane 0) and `last` (lane n-1)."""
    if NUM.sub("#", first).strip() != NUM.sub("#", last).strip():
        return None
    a = [int(x) for x in NUM.findall(first)]
    b = [int(x) for x in NUM.findall(last)]
    if len(a) < 2 or len(a) != len(b):
        return None
    # the lane count: from the first bit range, DEST[h:l] ... DEST[H:L]
    h, l, H, L = a[0], a[1], b[0], b[1]
    width = h - l + 1
    if width <= 0 or (L - l) % width:
        return None
    n = (L - l) // width + 1
    if n < 3:
        return None
    steps = []
    for x, y in zip(a, b):
        if (y - x) % (n - 1):
            return None
        steps.append((y - x) // (n - 1))
    parts = NUM.split(first)
    gen = []
    for k in range(1, n - 1):
        s = parts[0]
        for j, (x, d) in enumerate(zip(a, steps)):
            s += str(x + d * k) + parts[j + 1]
        gen.append(s)
    return gen


TYPOS = [
    (re.compile(r"\[(\d+)z\s*:"), r"[\1:"),                                  # SRC[127z:96)
    # a comment that wraps: '; refer to Table 15-4 in the Intel® 64 and IA-32
    # Architectures' + 'Software Developer's Manual, Volume 1'
    (re.compile(r";\s*refer to [^\n]*(\n\s*Software Developer.s Manual[^\n]*)?"), ";"),
    (re.compile(r"\bBITWISE\s+(AND|OR|XOR)\b"), r"\1"),
    (re.compile(r"(?s)(Intel\s+)?C/C\+\+\s+Compiler\s+Intrinsic.*$"), ""),
    (re.compile(r"([\]\)\w])[ \t]+(FI|ELSE)\b"), r"\1; \2"),      # 'THEN X := Y ELSE ... FI'
    (re.compile(r";\s*;"), ";"),
    (re.compile(r"\b(DEST|SRC\d?|TEMP\w*)\s*\(\s*(\d+)\s*:\s*(\d+)\s*\]"), r"\1[\2:\3]"),  # SRC(7:0]
    (re.compile(r"\b(DEST|SRC\d?|TEMP\w*)\s*\[\s*(\d+)\s*:\s*(\d+)\s*\)"), r"\1[\2:\3]"),  # DEST[7:0)
    (re.compile(r"−"), "-"),
    (re.compile(r"∗"), "*"),
]


def join_continued(text):
    """'DEST[i+31:i] :=' with its expression on the next line (CVTPD2DQ)."""
    return re.sub(r":=[ \t]*\n\s*", ":= ", text)


FOOTNOTE = re.compile(r"^\s*\d+\.\s+[A-Z](?!.*:=).*$", re.M)  # '1. ModRM.MOD = 011B required'


def balance(text):
    """A statement missing its closing parenthesis ('X := F(A + B;')."""
    out = []
    for ln in text.split("\n"):
        d = ln.count("(") - ln.count(")")
        if d > 0 and ":=" in ln:
            t = ln.rstrip()
            ln = (t[:-1] + ")" * d + ";") if t.endswith(";") else t + ")" * d
        out.append(ln)
    return "\n".join(out)


ELEM = {"byte": 8, "word": 16, "dword": 32, "qword": 64}


def elements(text):
    """'DEST.word[SEL]' (an element by index) as a bit range, and the lower
    case operand names some pages use ('src', 'dest', 'src1')."""
    def el(m):
        w, i = ELEM[m.group(2)], m.group(3)
        return f"{m.group(1)}[{w} * ({i}) + {w - 1}:{w} * ({i})]"
    text = re.sub(r"\b(\w+)\.(byte|word|dword|qword)\[([^\]]+)\]", el, text)
    text = re.sub(r"\bsrc(\d?)\b", lambda m: "SRC" + m.group(1), text)
    return re.sub(r"\bdest\b", "DEST", text)


def repair(text):
    text = UNMODIFIED.sub("", text)
    text = FOOTNOTE.sub("", join_continued(text))
    for rx, rp in TYPOS:
        text = rx.sub(rp, text)
    return expand_repeats(balance(elements(text)))


# page-local helper definitions: 'Select4(SRC, control) {...}' and the
# braceless 'LOGICAL_LEFT_SHIFT_WORDS(SRC, COUNT_SRC)' + its statements
FUNC_HEAD = re.compile(r"^\s*([A-Za-z][A-Za-z0-9_]*[A-Z0-9_][A-Za-z0-9_]*)\s*\(\s*([A-Za-z_][A-Za-z0-9_]*"
                       r"(\s*,\s*[A-Za-z_][A-Za-z0-9_]*)*)?\s*\)\s*(\{)?\s*$")


def extract_functions(text, mnems=None):
    """(text without the definitions, {name: (params, body text)})."""
    lines = text.split("\n")
    out, funcs = [], {}
    i = 0
    while i < len(lines):
        m = FUNC_HEAD.match(lines[i])
        if not m or HEADER.match(lines[i]) or m.group(1).upper() in ("IF", "FOR", "WHILE", "CASE"):
            out.append(lines[i])
            i += 1
            continue
        name = m.group(1)
        params = [p.strip() for p in (m.group(2) or "").split(",") if p.strip()]
        braced = bool(m.group(4)) or (i + 1 < len(lines) and lines[i + 1].strip() == "{")
        body = []
        i += 1
        if braced:
            if lines[i - 1].strip().endswith("{") is False and i < len(lines) and lines[i].strip() == "{":
                i += 1
            depth = 1
            while i < len(lines):
                ln = lines[i]
                depth += ln.count("{") - ln.count("}")
                if depth <= 0:
                    body.append(ln[:ln.rfind("}")])
                    i += 1
                    break
                body.append(ln)
                i += 1
        else:
            while i < len(lines) and not FUNC_HEAD.match(lines[i]) and not is_header(lines[i], mnems):
                body.append(lines[i])
                i += 1
        txt = "\n".join(body)
        txt = re.sub(r"\bRETURN\s*\(?\s*([A-Za-z_][^;\n]*?)\s*\)?\s*;?\s*$", r"RESULT := \1;", txt,
                     flags=re.M)
        funcs[name] = (params, txt)
    return "\n".join(out), funcs


# ---------------------------------------------------------------- lowering

FP_PAGE = re.compile(r"^V?(?!P)[A-Z0-9]*?(PS|PD|PH|SS|SD|SH)$")
FP_ET = {"S": "f32", "D": "f64", "H": "f16"}
ET_BITS = {"f16": 16, "bf16": 16, "f32": 32, "f64": 64}
# FCMP predicates for the pseudocode's comparisons (signaling where the
# manual's MIN/MAX/... say a QNaN raises invalid: < <= are signaling)
FPRED = {"=": 0, "<": 1, "<=": 2, "<>": 4, "!=": 4, "≠": 4}


def page_et(f):
    """The FP element type of a form's page, or None (integer / mixed)."""
    et = f.cfg.get("et")
    if et:
        return et
    m = FP_PAGE.match(f.mnem.upper())
    if m:
        return FP_ET[m.group(1)[1]]
    return None


class VectorMixin:
    """lower.Lowerer methods for vector operands (see the module docstring)."""

    # ------------------------------------------------------------ setup

    def vsetup(self):
        self.vdest = None              # (DEST VSEL, the temp it's written through)
        self.vwritten = []             # static DEST bit ranges written so far
        self.vfree = [f"VT{i}" for i in reversed(range(8))]
        self.fp_et = page_et(self.f)
        self.fp_used = False
        self.vec_form = any(o.kind in ("vreg", "vrm") for o in self.f.operands)

    def vstatic(self, n):
        if n == "MAXVL":
            return MAXVL
        if n in ("ZERO_FILL", "ZeroExtend") and (self.vec_form or self.fp_et):
            return 0                     # as in r32[31:16] := ZERO_FILL
        if n == "VL" and self.vec_form:
            return getattr(self.f, "vl", 128)
        return None

    def vprologue(self):
        """The SSE checks: #UD with CR0.EM or without CR4.OSFXSR, #NM with
        CR0.TS (Intel SDM Vol. 2, the SIMD exception classes, 2.5)."""
        if not self.vec_form or getattr(self.f, "enc", "legacy") != "legacy":
            return
        if "SIMD_Check_Legacy" in self.lib:
            self.inline("SIMD_Check_Legacy", [], None)
        al = self.f.cfg.get("align")
        if al and self.f.mem:
            # an aligned vector access (MOVAPS, MOVDQA, MOVNTPS...): #GP(0) if the
            # effective address isn't a multiple of the operand size (the pages'
            # exception sections, Intel SDM Vol. 2 2.5.1)
            n = int(al) if str(al).isdigit() else 16
            t = self.tmp()
            ok = self.r.label()
            self.E("AND", 64, dst=t, a="EA", b="K", imm=n - 1)
            self.E("CMP", 64, a=t, b="ZERO")
            self.E("JU", cc=self.ucc("EQ"), imm=ok)
            self.E("EXC", 32, a="ZERO", imm=13 | (1 << 8), note="#GP(0)")
            self.r.place(ok)

    def vepilogue(self):
        if self.fp_used:
            self.E("FPCHK")
        if self.vdest is not None:
            dst, tmp = self.vdest
            self.E("VMOVR", 64, dst=self.vs(dst), a=self.vs(tmp))

    # ------------------------------------------------------------ operands

    def voperand(self, o):
        V = self.V
        if o.kind == "vreg":
            return V("vreg", o.size, sel=o.vsel)
        if o.kind == "vrm":
            if self.f.mem:
                return V("vmem", o.msize or o.size, addr=V("sel", 64, sel="EA"))
            return V("vreg", o.size, sel="VRM")
        return None

    def is_vec(self, v):
        return v is not None and v.kind in ("vreg", "vmem", "vslice")

    def vview(self, v):
        """(base, static offset, dynamic offset sel or None, width) of a vector value."""
        if v.kind == "vslice":
            return v.base, v.k, v.sel or None, v.w
        return v, 0, None, v.w

    # ------------------------------------------------------------ reads

    def vslice(self, v, lo, width):
        """v[lo + width - 1 : lo] (lo: an int or a sel V)."""
        base, off, doff, _ = self.vview(v)
        V = self.V
        if isinstance(lo, int):
            off += lo
        else:
            d = lo.sel if doff is None else self.vadd(doff, lo.sel)
            doff = d
        s = V("vslice", width, base=base, k=off, sel=doff or "")
        if width <= 64:
            return self.vread(s)
        return s

    def vadd(self, a, b):
        t = self.tmp()
        self.E("ADD", 64, dst=t, a=a, b=b)
        return t

    @staticmethod
    def vs(n):
        """An assembler vector selector name (VSEL)."""
        return n if n.startswith("V:") else "V:" + n

    def vread(self, s):
        """An element (<= 64 bits) of a vector slice into a scalar temp."""
        base, off, doff, width = self.vview(s)
        V = self.V
        w = width if width in (8, 16, 32, 64) else 64
        t = self.tmp()
        if base.kind == "vreg":
            sel = base.sel
            if self.vdest is not None and sel == self.vdest[0] and self.voverlap(off, width, doff):
                sel = self.vdest[1]
            self.E("VGET", w, dst=t, a=doff or "ZERO", b=self.vs(sel), imm=off)
        else:
            if doff is None:
                if off % 8 == 0:
                    a, imm = self.addr_of(base.addr)
                    self.E("LD", w, dst=t, a=a, imm=imm + off // 8, seg=8)
                else:
                    raise self.Err("a memory element at a bit offset")
            else:
                at = self.tmp()
                self.E("SHR", 64, dst=at, a=doff, b="K", imm=3)
                a, imm = self.addr_of(base.addr)
                self.E("ADD", 64, dst=at, a=at, b=a)
                self.E("LD", w, dst=t, a=at, imm=imm + off // 8, seg=8)
        if w != width:
            self.E("SLICE", 64, dst=t, a=t, imm=0 | (width << 8))
        r = V("sel", width, sel=t)
        if self.fp_et and width == ET_BITS.get(self.fp_et):
            r.fp = self.fp_et
        return r

    def voverlap(self, off, width, doff):
        if doff is not None:
            return bool(self.vwritten)
        return any(lo < off + width and off < hi for lo, hi in self.vwritten)

    def vshift(self, v, amount):
        """(vector >> amount): a slice starting at the amount."""
        base, off, doff, width = self.vview(v)
        if amount.kind == "k":
            return self.V("vslice", width, base=base, k=off + amount.k, sel=doff or "")
        (s,), _ = self.srcs(amount, w=64)
        d = s if doff is None else self.vadd(doff, s)
        return self.V("vslice", width, base=base, k=off, sel=d)

    # ------------------------------------------------------------ writes

    def vtemp_var(self, n):
        """A vector variable of the pseudocode (TMP_DEST, TMP_SRC2) in a VT."""
        if not self.vfree:
            raise self.Err("out of vector temporaries")
        v = self.V("vreg", MAXVL, sel=self.vfree.pop())
        self.vars[n] = v
        return v

    def vstore_dyn(self, base, lo, width, v):
        """base[lo + width - 1 : lo] := v with lo a run-time value."""
        if base.kind != "vreg" or width not in (8, 16, 32, 64):
            raise self.Err("a dynamic vector store")
        sel = self.vdest_sel(base)
        (s,), imm = self.srcs(v, w=width)
        if s == "K":
            k = self.tmp()
            self.E("MOV", 64, dst=k, a="K", imm=imm)
            s = k
        (o,), oimm = self.srcs(lo, w=64)
        if o == "K":
            o = "ZERO"
        else:
            oimm = 0
        self.E("VSET", width, dst=self.vs(sel), a=s, b=o, imm=oimm)
        self.vwritten.append((0, MAXVL))      # somewhere: later reads see the new value

    def vdest_sel(self, base):
        """The VSEL to write for a destination register (through a temp)."""
        if base.sel.startswith("VT"):
            return base.sel                  # a vector temporary: written directly
        if self.vdest is None:
            if not self.vfree:
                raise self.Err("out of vector temporaries")
            tmp = self.vfree.pop()
            self.E("VMOVR", 64, dst=self.vs(tmp), a=self.vs(base.sel), note="DEST's old value")
            self.vdest = (base.sel, tmp)
        if base.sel != self.vdest[0]:
            raise self.Err("two vector destinations")
        return self.vdest[1]

    def vwrite(self, base, lo, width, v):
        """base[lo + width - 1 : lo] := v (scalar or vector value); lo static."""
        if v is None:
            raise self.Err("no value to store")
        if v.kind == "k" and width > 64:
            # a wide constant (DEST[127:32] := 0...0H): 64 bits at a time
            for c in range(0, width, 64):
                n = min(64, width - c)
                self.vwrite(base, lo + c, n, self.V("k", n, k=(v.k >> c) & ((1 << n) - 1)))
            return
        if self.is_vec(v):
            # a wide copy, 64 bits at a time
            for c in range(0, width, 64):
                n = min(64, width - c)
                e = self.vslice(v, c, n) if v.kind != "vslice" or n <= 64 else None
                self.vwrite(base, lo + c, n, e)
            return
        if base.kind == "vreg":
            sel = self.vdest_sel(base)
            if v.kind == "k" and v.k == 0:
                s = "ZERO"
            else:
                (s,), imm = self.srcs(v, w=max(8, min(width, 64)))
                if s == "K":
                    k = self.tmp()
                    self.E("MOV", 64, dst=k, a="K", imm=imm)
                    s = k
            if width in (8, 16, 32, 64):
                self.E("VSET", width, dst=self.vs(sel), a=s, b="ZERO", imm=lo)
            else:
                raise self.Err(f"a {width}-bit vector element write")
            self.vwritten.append((lo, lo + width))
        else:
            if lo % 8 or width not in (8, 16, 32, 64):
                raise self.Err("a memory element at a bit offset")
            a, imm = self.addr_of(base.addr)
            (s,), kimm = self.srcs(v, w=width)
            if s == "K":
                k = self.tmp()
                self.E("MOV", 64, dst=k, a="K", imm=kimm)
                s = k
            self.E("ST", width, a=a, b=s, imm=imm + lo // 8, seg=8)

    def vstore_slice(self, base, h, l, v):
        width = h - l + 1
        if base.kind == "vreg" and l >= getattr(self.f, "vl", 128) and h == MAXVL - 1:
            if not (v.kind == "k" and v.k == 0):
                raise self.Err("upper vector bits := non-zero")
            sel = self.vdest_sel(base)
            self.E("VZUP", 64, dst=self.vs(sel), imm=l)
            return
        self.vwrite(base, l, width, v)

    # ------------------------------------------------------------ whole-vector logic

    def vlogic(self, op, a, b):
        """a op b (AND OR XOR, or NOT a when b is None) over a whole vector, in
        64-bit chunks, into a vector temporary."""
        width = a.w if self.is_vec(a) else b.w
        if not self.vfree:
            raise self.Err("out of vector temporaries")
        t = self.vfree.pop()
        for c in range(0, width, 64):
            n = min(64, width - c)
            x = self.vslice(a, c, n) if self.is_vec(a) else a
            if op == "NOT":
                r = self.tmp()
                (s,), _ = self.srcs(x, w=n)
                self.E("NOT", n, dst=r, a=s)
            else:
                y = self.vslice(b, c, n) if self.is_vec(b) else b
                (sa, sb), imm = self.srcs(x, y, w=n)
                r = self.tmp()
                self.E(op, n, dst=r, a=sa, b=sb, imm=imm)
            self.E("VSET", n, dst=self.vs(t), a=r, b="ZERO", imm=c)
        return self.V("vreg", width, sel=t)

    def vmul(self, a, b, want):
        """An integer product at twice the elements' width (PMULHW, PMULUDQ,
        PMADDWD): signed with the page option 'signed', else unsigned."""
        w = max(x.w for x in (a, b) if x.kind != "k")
        dw = min(2 * w, 64)
        signed = bool(self.f.cfg.get("signed"))
        a2, b2 = self.widen(a, dw, signed=signed), self.widen(b, dw, signed=signed)
        (sa, sb), imm = self.srcs(a2, b2, w=dw)
        t = self.tmp()
        self.E("IMULT" if signed else "MULU", dw, dst=t, a=sa, b=sb, imm=imm)
        return self.V("sel", dw, sel=t)

    def scalar_var(self, n):
        """A scalar variable first assigned by bit field (TEMP0[31:0] := ...)."""
        t = self.tmp(keep=True)
        self.E("MOV", 64, dst=t, a="ZERO")
        v = self.V("sel", 64, sel=t)
        self.vars[n] = v
        return v

    # ------------------------------------------------------------ floating point

    def fp_binop(self, op, a, b, want):
        et = a.fp or b.fp
        w = ET_BITS[et]
        (sa, sb), imm = self.srcs(a, b, w=w)
        if "K" in (sa, sb):
            raise self.Err("FP constant operand")
        t = self.tmp()
        self.E({"+": "FADD", "-": "FSUB", "*": "FMUL", "/": "FDIV"}[op], w, dst=t, a=sa, b=sb,
               et=self.ETN[et])
        self.fp_used = True
        r = self.V("sel", w, sel=t)
        r.fp = et
        return r

    def fp_cmp(self, op, a, b):
        """A 0/1 temp: the FP comparison a op b (FCMP predicate)."""
        et = a.fp or b.fp
        w = ET_BITS[et]
        if op in (">", ">="):
            a, b, op = b, a, {">": "<", ">=": "<="}[op]
        (sa, sb), _ = self.srcs(a, b, w=w)
        t = self.tmp()
        self.E("FCMP", w, dst=t, a=sa, b=sb, et=self.ETN[et], imm=FPRED[op])
        self.fp_used = True
        return self.V("sel", 1, sel=t)

    def fp_unary(self, op, a, et, x=0, sz=None, imm=0, rnd=4):
        w = sz or ET_BITS[et]
        (s,), _ = self.srcs(a, w=64)
        t = self.tmp()
        self.E(op, w, dst=t, a=s, et=self.ETN[et], x=x, imm=imm, rnd=rnd)
        self.fp_used = True
        return t

    CONV = re.compile(r"^Convert_(Integer|Single_Precision|Double_Precision|Half_Precision)"
                      r"(_Floating_Point)?_To_(Integer|Single_Precision|Double_Precision|Half_Precision)"
                      r"(_Floating_Point)?(64|32)?(_Truncate)?$")
    PREC = {"Single_Precision": "f32", "Double_Precision": "f64", "Half_Precision": "f16"}

    def vcall(self, e, want):
        """Vector/FP helpers; None if `e` isn't one."""
        V = self.V
        f = e.f
        m = self.CONV.match(f)
        if m:
            frm, to = m.group(1), m.group(3)
            arg = self.eval(e.args[0])
            if frm == "Integer":
                et = self.PREC[to]
                t = self.fp_unary("ICVTF", arg, et, sz=arg.w if arg.w in (32, 64) else 32)
                r = V("sel", ET_BITS[et], sel=t)
                r.fp = et
                return r
            if to == "Integer":
                et = self.PREC[frm]
                iw = int(m.group(5)) if m.group(5) else (want if want in (32, 64) else 32)
                t = self.fp_unary("FCVTIT" if m.group(6) else "FCVTI", arg, et, sz=iw)
                return V("sel", iw, sel=t)
            et, src = self.PREC[to], self.PREC[frm]
            t = self.fp_unary("FCVT", arg, et, x=self.ETN[src], sz=ET_BITS[et])
            r = V("sel", ET_BITS[et], sel=t)
            r.fp = et
            return r
        if f == "APPROXIMATE" and self.fp_et:
            # RCPPS/RSQRTPS: an approximation with relative error <= 1.5 * 2^-12
            # (Intel SDM Vol. 2); v1 returns the correctly rounded value, which
            # is within it, and like the manual says raises no exceptions
            # (x bit 3: the flags aren't accumulated). AMD's and Intel's tables
            # differ, so differential tests skip these.
            arg = e.args[0]
            et = self.fp_et
            if isinstance(arg, Bin) and arg.op == "/":
                num, den = arg.a, arg.b
                sq = isinstance(den, Call) and den.f == "SQRT"
                d = self.eval(den.args[0] if sq else den)
                (sd,), _ = self.srcs(d, w=ET_BITS[et])
                if sq:
                    t0 = self.tmp()
                    self.E("FSQRT", ET_BITS[et], dst=t0, a=sd, et=self.ETN[et], x=8)
                    sd = t0
                one = self.tmp()
                self.E("MOV", 64, dst=one, a="K", imm=0x3F800000 if et == "f32" else 0)
                if et == "f64":
                    self.E("KHI", 64, dst=one, imm=0x3FF00000)
                t = self.tmp()
                self.E("FDIV", ET_BITS[et], dst=t, a=one, b=sd, et=self.ETN[et], x=8)
                r = V("sel", ET_BITS[et], sel=t)
                r.fp = et
                return r
            return None
        if f == "FPCompare" and len(e.args) == 3:
            # FPCompare(a, b, predicate): 1 if the CMPPS/CMPSD predicate (imm8,
            # Intel SDM Vol. 2 Table 3-1) holds, the predicate a run-time value
            a, b = self.eval(e.args[0]), self.eval(e.args[1])
            et = a.fp or b.fp or self.fp_et
            p = self.eval(e.args[2], 8)
            w = ET_BITS[et]
            (sa, sb), _ = self.srcs(a, b, w=w)
            (sp,), pimm = self.srcs(p, w=8)
            t = self.tmp()
            if sp == "K":
                self.E("FCMP", w, dst=t, a=sa, b=sb, et=self.ETN[et], imm=pimm & 31)
            else:
                self.E("FCMP", w, dst=t, a=sa, b=sb, c=sp, et=self.ETN[et], x=4)
            self.fp_used = True
            return V("sel", 1, sel=t)
        if f == "CompareFlags" and len(e.args) == 3:
            # CompareFlags(a, b, signaling): (U)COMISS/SD's ZF PF CF (FCMP x = 1
            # signaling: a QNaN raises invalid; x = 2 quiet)
            a, b = self.eval(e.args[0]), self.eval(e.args[1])
            et = a.fp or b.fp or self.fp_et
            sig = self.static_val(e.args[2])
            w = ET_BITS[et]
            (sa, sb), _ = self.srcs(a, b, w=w)
            self.E("FCMP", w, a=sa, b=sb, et=self.ETN[et], x=1 if sig else 2)
            self.fp_used = True
            return self.V("k", 1, k=0)
        if f == "SQRT":
            if not self.fp_et:
                return None
            a = self.eval(e.args[0])
            et = a.fp or self.fp_et
            t = self.fp_unary("FSQRT", a, et)
            r = V("sel", ET_BITS[et], sel=t)
            r.fp = et
            return r
        if f in ("MIN", "MAX") and len(e.args) == 2 and self.fp_et:
            a, b = self.eval(e.args[0]), self.eval(e.args[1])
            if a.fp or b.fp:
                et = a.fp or b.fp
                w = ET_BITS[et]
                (sa, sb), _ = self.srcs(a, b, w=w)
                t = self.tmp()
                self.E("FMIN" if f == "MIN" else "FMAX", w, dst=t, a=sa, b=sb, et=self.ETN[et])
                self.fp_used = True
                r = V("sel", w, sel=t)
                r.fp = et
                return r
            return None
        # saturation (Intel SDM Vol. 1, 4.6.3 / Vol. 2 PADDS, PACKSS...)
        sat = re.match(r"^SaturateTo(Signed|UnSigned|Unsigned)(Byte|Word)$", f)
        if sat and isinstance(e.args[0], Bin) and e.args[0].op in ("+", "-"):
            w = 8 if sat.group(2) == "Byte" else 16
            signed = sat.group(1) == "Signed"
            a, b = self.eval(e.args[0].a, w), self.eval(e.args[0].b, w)
            (sa, sb), imm = self.srcs(a, b, w=w)
            op = {(True, "+"): "ADDS", (True, "-"): "SUBS", (False, "+"): "ADDUS",
                  (False, "-"): "SUBUS"}[(signed, e.args[0].op)]
            t = self.tmp()
            self.E(op, w, dst=t, a=sa, b=sb, imm=imm)
            return V("sel", w, sel=t)
        pack = re.match(r"^SaturateSigned(Word|Dword)To(Signed|Unsigned)(Byte|Word)$", f)
        if pack:
            w = 16 if pack.group(1) == "Word" else 32
            a = self.eval(e.args[0], w)
            (s,), imm = self.srcs(a, w=w)
            t = self.tmp()
            self.E("PACKSS" if pack.group(2) == "Signed" else "PACKUS", w, dst=t, a=s, imm=imm)
            return V("sel", w // 2, sel=t)
        if f == "ABS" and isinstance(e.args[0], Bin) and e.args[0].op == "-":
            # |a - b| of unsigned elements (PSADBW): max - min, no wraparound
            a, b = self.eval(e.args[0].a), self.eval(e.args[0].b)
            w = max(a.w, b.w)
            (sa, sb), _ = self.srcs(a, b, w=w)
            hi, lo = self.tmp(), self.tmp()
            self.E("MAXU", w, dst=hi, a=sa, b=sb)
            self.E("MINU", w, dst=lo, a=sa, b=sb)
            self.E("SUB", w, dst=hi, a=hi, b=lo)
            return V("sel", w, sel=hi)
        return None
