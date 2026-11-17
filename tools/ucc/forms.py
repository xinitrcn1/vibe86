"""Instruction forms: one per opcode-table row the core implements.

A row of the SDM's opcode table ('REX.W + 81 /0 id | ADD r/m64, imm32 | MI |
Valid | N.E.') says how an instruction is encoded and what its operands are.
ucc specializes the instruction's Operation for every row, and for every
operand kind the row allows (a ModRM r/m operand is a register or memory),
so each microcode routine runs with OperandSize, the operands' locations and
the instruction's name known; that is partial evaluation of the manual's
pseudocode with respect to its static inputs (Futamura, "Partial Evaluation
of Computation Process -- An Approach to a Compiler-Compiler", Systems,
Computers, Controls 2(5), 1971; reprinted in Higher-Order and Symbolic
Computation 12(4), 1999, doi:10.1023/A:1010095604496).

The decoder tables (which opcodes have a ModRM byte, how long their
immediates are, their default operand size in 64-bit mode) are derived from
the same rows (Intel SDM Vol. 2A, ch. 2 "Instruction Format", and appendix A
"Opcode Map").
"""

import re
from dataclasses import dataclass, field

MAP_NAMES = {0: "1-byte", 1: "0F", 2: "0F38", 3: "0F3A"}


@dataclass
class Operand:
    kind: str        # rm reg opreg mem imm rel acc fixed one port cr
    size: int = 0    # bits (0: n/a)
    reg: str = ""    # for fixed operands: the register's name (AL, CL, DX, ...)
    text: str = ""


@dataclass
class Form:
    page: str
    mnem: str
    text: str
    opcol: str
    open: str
    map: int = 0
    opc: int = 0
    digit: int = None
    modrm: bool = False
    plus_r: bool = False
    imm: list = field(default_factory=list)   # immediate byte counts, in order
    rel: bool = False
    rexw: bool = False
    mprefix: str = None      # F3 / F2 / 66 / NP (mandatory) or None
    osz: int = 32
    okey: str = "any"        # '16' '32' '64' 'any': decoder operand size this row answers to
    operands: list = field(default_factory=list)
    mem: bool = None         # specialization: the r/m operand is memory (True) or a register
    rep: str = None          # specialization for string instructions: None, 'F3', 'F2'
    cc: int = None           # Jcc/SETcc/CMOVcc condition
    vclass: str = "d32"      # d32, d64 (default 64, 66 -> 16), f64 (always 64)
    hints: dict = field(default_factory=dict)
    cfg: dict = field(default_factory=dict)  # the .isa line's options
    mode: int = 3            # decode mode: 0 real, 1 16-bit protected/v86, 2 32-bit, 3 64-bit
    spec: dict = field(default_factory=dict) # on-demand specializations (AddressSize, ...)
    routine: str = ""        # the (possibly shared) routine this form runs

    @property
    def name(self):
        s = f"{self.text} [{self.opcol}] m{self.mode} o{self.osz}"
        if "crnum" in self.hints or "sreg" in self.hints:
            s += f" r{self.digit}"
        for k, v in sorted(self.spec.items()):
            s += f" {k}={v}"
        if self.mem is not None:
            s += ":m" if self.mem else ":r"
        if self.rep:
            s += ":" + {"F3": "rep", "F2": "repne"}[self.rep]
        return s

    def key(self):
        return (self.mode, self.map, self.opc, self.digit, self.prefix_key(), self.okey, self.mem,
                tuple(sorted(self.spec.items())), self.hints.get("sub90"))

    def prefix_key(self):
        if self.mprefix in ("F3", "F2", "66"):
            return self.mprefix
        if self.rep:
            return self.rep
        return None


class FormError(Exception):
    pass


OPCODE_TOKEN = re.compile(r"[0-9A-F]{2}\+?$")


def parse_opcode(f, col):
    """Fill map/opc/digit/modrm/imm/rexw/mprefix from the opcode column."""
    s = col.replace("REX.w", "REX.W")
    s = s.replace("REX.W +", "REX.W ").replace("REX.R +", "REX.R ").replace("REX +", "REX ")
    s = re.sub(r"\+\s*(r[bwdo])\b", r"+\1", s)   # 'B8+ rd' -> 'B8+rd'
    s = re.sub(r"([0-9A-F]{2})\s*\+r", r"\1 +r", s)
    s = re.sub(r"/(\d|r)", r" /\1", s)
    toks = s.split()
    bytes_ = []
    for t in toks:
        if t == "REX.W":
            f.rexw = True
        elif t in ("REX", "REX.R"):
            pass
        elif t == "NP":
            f.mprefix = "NP"
        elif t in ("NFx",):
            pass
        elif t.startswith("+r"):
            f.plus_r = True
        elif t == "/r":
            f.modrm = True
        elif re.match(r"^/\d$", t):
            f.modrm = True
            f.digit = int(t[1])
        elif t in ("ib", "cb"):
            f.imm.append(1)
            f.rel = f.rel or t[0] == "c"
        elif t in ("iw", "cw"):
            f.imm.append(2)
            f.rel = f.rel or t[0] == "c"
        elif t in ("id", "cd"):
            f.imm.append(4)
            f.rel = f.rel or t[0] == "c"
        elif t == "io":
            f.imm.append(8)
        elif t == "cp":
            f.imm.append(6)             # ptr16:32: offset, then the selector
            f.hints["farptr"] = True
        elif OPCODE_TOKEN.match(t):
            if t.endswith("+"):
                f.plus_r = True
                t = t[:-1]
            bytes_.append(int(t, 16))
        else:
            raise FormError(f"opcode token '{t}' in '{col}'")
    # mandatory prefixes come before 0F (or before the opcode for F3 90)
    while len(bytes_) > 1 and bytes_[0] in (0xF3, 0xF2, 0x66):
        f.mprefix = {0xF3: "F3", 0xF2: "F2", 0x66: "66"}[bytes_[0]]
        bytes_ = bytes_[1:]
    if not bytes_:
        raise FormError(f"no opcode in '{col}'")
    if bytes_[0] == 0x0F:
        if len(bytes_) >= 2 and bytes_[1] == 0x38:
            f.map, rest = 2, bytes_[2:]
        elif len(bytes_) >= 2 and bytes_[1] == 0x3A:
            f.map, rest = 3, bytes_[2:]
        else:
            f.map, rest = 1, bytes_[1:]
    else:
        f.map, rest = 0, bytes_
    if len(rest) == 2 and rest[1] >= 0xC0:
        # a fixed register-form ModRM byte: 'NP 0F AE F8' (SFENCE) is 0F AE /7, mod = 11
        f.modrm = True
        f.digit = (rest[1] >> 3) & 7
        f.hints["regform"] = True
        rest = rest[:1]
    if len(rest) != 1:
        raise FormError(f"opcode '{col}' isn't one byte after the escape")
    f.opc = rest[0]


SIZED = re.compile(r"^(r/m|r|m|imm|rel|moffs)(8|16|32|64|128|256|512)$")
FIXED = {"AL": 8, "CL": 8, "DL": 8, "BL": 8, "AX": 16, "DX": 16, "EAX": 32, "RAX": 64}


def parse_operands(f, enc):
    ins = f.text.replace(" ,", ",")
    # footnote marks on operands: 'MOV Sreg, r/m162', 'MOV r64/m16, Sreg2'
    ins = re.sub(r"\b(Sreg|r/m16|r/m64|r/m32)\d\b", r"\1", ins)
    ins = re.sub(r"\bmoffs(8|16|64)3\b", r"moffs\1", ins)
    # 'MOV r16/r32/m16, Sreg', 'MOV r64/m16, Sreg': the register forms of
    # 8C at operand size 32/64 (the selector zero-extended); its memory form
    # is the 'MOV r/m16, Sreg' row's, which stores 16 bits at any size
    if re.search(r",\s*Sreg\b", ins) and re.search(r"\br(16/r32|64)/m16\b", ins):
        ins = re.sub(r"\br16/r32/m16\b", "r32", ins)
        ins = re.sub(r"\br64/m16\b", "r64", ins)
        f.hints["regform"] = True
    # 'LAR r16, r16/m16', 'SMSW r32/m16': a register or a 16-bit memory operand
    ins = re.sub(r"\br(16|32|64)/m16\d?\b", "r/m16", ins)
    # 'MOVDIR64B r16/r32/r64, m512': a register of the address size
    ins = ins.replace("r16/r32/r64", "rasz")
    parts = ins.split(None, 1)
    f.mnem = parts[0]
    ops = [o.strip() for o in parts[1].split(",")] if len(parts) > 1 else []
    # description text pdftotext ran into the column ('JB rel32, -bit mode.')
    ops = [o for o in ops if not re.search(r"[a-z]{3,}", o.replace("imm", "").replace("rel", "").replace("ptr", "").replace("Sreg", "").replace("moffs", ""))]
    roles = enc.get(f.open, [])
    for i, o in enumerate(ops):
        role = roles[i] if i < len(roles) else ""
        m = SIZED.match(o)
        if m:
            k, sz = m.group(1), int(m.group(2))
            if k == "r":
                kind = "opreg" if "opcode" in role else "reg"
                if "r/m" in role:
                    kind = "rm"
                if f.plus_r and not f.modrm:
                    # '90+rd XCHG EAX, r32': the register is the opcode's
                    # (the table's two 'O' encodings of XCHG share a name,
                    # so the role can be the other operand's)
                    kind = "opreg"
            elif k == "r/m":
                kind = "rm"
            elif k == "m":
                kind = "mem"
            elif k == "imm":
                kind = "imm"
            elif k == "rel":
                kind = "rel"
            elif k == "moffs":
                # a memory operand whose offset is the immediate (A0-A3)
                kind = "mem"
                f.hints["moffs"] = True
            else:
                raise FormError(f"operand '{o}'")
            f.operands.append(Operand(kind, sz, text=o))
        elif o == "rasz":
            f.operands.append(Operand("reg", 0, text=o))
        elif o in ("m",) or re.match(r"^m\d+&\d+$", o):
            f.operands.append(Operand("mem", 0, text=o))
        elif o in FIXED:
            if o == "DX" and f.mnem in ("IN", "OUT"):
                f.operands.append(Operand("port", 16, "DX", o))
            else:
                f.operands.append(Operand("fixed", FIXED[o], o, o))
        elif re.match(r"^ptr16:(16|32)$", o):
            f.operands.append(Operand("farptr", int(o[6:]), text=o))
            f.hints["farptr"] = True
        elif re.match(r"^m16:(16|32|64)$", o):
            f.operands.append(Operand("farmem", int(o[4:]), text=o))
        elif o in ("ES", "CS", "SS", "DS", "FS", "GS"):
            f.operands.append(Operand("seg", 16, o, o))
        elif o == "Sreg":
            f.operands.append(Operand("sreg", 16, text=o))
        elif o == "1":
            f.operands.append(Operand("one", 8, text=o))
        elif re.match(r"^CR[0-8]", o):
            f.operands.append(Operand("cr", 64, text=o))
        elif re.match(r"^DR[0-7]", o):
            f.operands.append(Operand("cr", 64, text=o))     # same form shape as MOV CRn
        else:
            raise FormError(f"operand '{o}'")


SUFFIX = {"B": 8, "W": 16, "D": 32, "Q": 64}


def operand_size(f):
    if f.cfg.get("oszsrc"):
        # CRC32 r32, r/m16: the source (r/m) operand has the operand size
        # (66h selects r/m16); an r/m8 source takes the destination's
        rm = [o for o in f.operands if o.kind in ("rm", "mem") and o.size]
        if rm and rm[-1].size != 8:
            return rm[-1].size
    for o in f.operands:
        if o.kind in ("rm", "reg", "opreg", "mem", "fixed") and o.size and o.size <= 64:
            return o.size
    for o in f.operands:
        if o.kind in ("farptr", "farmem"):
            return min(o.size, 64)
    for o in f.operands:
        if o.kind in ("imm", "rel") and o.size == 16 and not f.cfg.get("immnotsize"):
            return 16
    if "osz" in f.hints:
        return f.hints["osz"]
    return None


def is_far(f):
    """A far transfer (JMP/CALL m16:xx or ptr16:xx, RET far: CA/CB): its
    default operand size in 64-bit mode is 32, not the near forms' 64."""
    return any(o.kind in ("farptr", "farmem") for o in f.operands) or \
        (f.map == 0 and f.opc in (0xCA, 0xCB))


def make_forms(page, cfg):
    """All the forms of `page` the configuration selects.

    cfg: dict with optional 'only' (regex over the instruction column),
    'skip' (regex), 'osz' (mnemonic -> size map for rows without sized
    operands), 'class' (d32/d64/f64 override).
    """
    forms, errors = [], []
    for row in page.opcodes:
        if len(row) < 4:
            continue
        if len(row) >= 4 and " " in row[2] and row[2].split()[-1] in ("Valid", "Invalid", "N.E."):
            # columns run together by pdftotext: 'MRC Valid | Valid | Shift ...'
            row = [row[0], row[1], row[2].split()[0], row[2].split()[-1], row[3]]
        opcol, ins, open_, m64 = row[0], row[1], row[2], row[3]
        mleg = row[4] if len(row) > 4 else ""
        ok64 = m64.startswith("Valid") or m64 in ("V", "V/V", "V/N.E.")
        okleg = mleg.startswith("Valid") or m64 in ("V/V",) or mleg == "V"
        if cfg.get("mode64only"):
            okleg = False
        if not (ok64 or okleg):
            continue
        # only=/skip= match 'opcode | instruction'
        row_text = f"{opcol} | {ins}"
        if cfg.get("only") and not re.search(cfg["only"], row_text):
            continue
        if cfg.get("skip") and re.search(cfg["skip"], row_text):
            continue
        f = Form(page=page.name, mnem="", text=ins, opcol=opcol, open=open_, cfg=cfg)
        try:
            parse_opcode(f, opcol)
            parse_operands(f, page.encoding)
        except FormError as e:
            errors.append(f"{page.name}: '{opcol} | {ins}': {e}")
            continue
        sizes = cfg.get("osz", {})
        if f.mnem in sizes:
            f.hints["osz"] = sizes[f.mnem]
        elif f.mnem[-1] in SUFFIX and f.mnem[:-1] in cfg.get("suffixed", ()):
            f.hints["osz"] = SUFFIX[f.mnem[-1]]
        sz = operand_size(f)
        far = is_far(f)
        if cfg.get("class") == "f64" and not far:
            sz = 64             # near branches: 64-bit operand size, 66h ignored
        if sz is None:
            sz = 64 if cfg.get("class") in ("d64", "f64") else 32
            if f.rexw:
                sz = 64
        if f.rexw:
            sz = max(sz, 64) if sz in (32, 64) or not f.operands else sz
        f.osz = sz
        for o in f.operands:
            if o.kind in ("rel",):
                o.size = 64
        # MOV Sreg, r/m: one form per segment register (ModRM.reg), so its
        # routine knows statically which descriptor cache it loads
        crx = any(o.kind == "cr" for o in f.operands) and cfg.get("crreg") and f.digit is None
        if (f.operands and f.operands[0].kind == "sreg" and cfg.get("sreg")) or crx:
            drx = crx and any(o.text.startswith("DR") for o in f.operands)
            for d in ((0, 1, 2, 3, 4, 5, 6, 7) if drx else (0, 2, 3, 4) if crx else (0, 1, 2, 3, 4, 5)):
                g = Form(**{**f.__dict__, "operands": list(f.operands), "imm": list(f.imm),
                            "hints": dict(f.hints), "spec": {}})
                g.digit = d
                g.hints["crnum" if crx else "sreg"] = d
                if ok64:
                    forms.append(g)
                if okleg:
                    for mode in (0, 1, 2):
                        h = Form(**{**g.__dict__, "operands": list(g.operands), "imm": [],
                                    "hints": dict(g.hints), "spec": {}})
                        h.mode = mode
                        h.osz = 32 if crx else 16
                        forms.append(h)
            continue
        if (f.page in ("Jcc", "SETcc", "CMOVcc")) or cfg.get("cc"):
            f.cc = f.opc & 0xF
        # an r/m operand means a ModRM byte, even where the opcode column
        # doesn't say /r (SETcc: '0F 94 | SETE r/m8')
        if any(o.kind == "rm" or (o.kind == "mem" and not f.hints.get("moffs"))
               for o in f.operands):
            f.modrm = True
        f.hints["sized"] = operand_size(f) is not None
        if (f.map, f.opc) == (0, 0x90):
            # 90h is NOP/PAUSE without REX.B and XCHG with a register otherwise
            f.hints["sub90"] = 0 if f.plus_r else 1
        if ok64 and is_far(f) and operand_size(f) is None:
            # RET far (CB, CA iw): one row for every size -- 66h 16, the
            # default 32, REX.W 64
            for sz in (16, 32, 64):
                g = Form(**{**f.__dict__, "operands": [Operand(**o.__dict__) for o in f.operands],
                            "imm": list(f.imm), "hints": dict(f.hints), "spec": {}})
                g.osz = sz
                forms.append(g)
        elif ok64:
            forms.append(f)
        if okleg and not f.rexw:
            # 16- and 32-bit code (real mode, 16-bit and 32-bit protected/compatibility
            # mode): operand size from the row, or every legal size for rows that
            # don't say (RET, PUSHF, Jcc rel32 is 32-bit, rel16 16-bit)
            lsz = operand_size(f)
            if lsz is None and not cfg.get("immnotsize"):
                for o in f.operands:
                    if o.kind in ("imm", "rel") and o.text.endswith(("16", "32")):
                        lsz = int(o.text[-2:])
            sizes = [lsz] if lsz in (8, 16, 32) else ([16, 32] if lsz is None else [])
            for mode in (0, 1, 2):
                for sz in sizes:
                    g = Form(**{**f.__dict__, "operands": [Operand(**o.__dict__) for o in f.operands],
                                "imm": list(f.imm), "hints": dict(f.hints), "spec": {}})
                    g.mode = mode
                    g.osz = sz
                    for o in g.operands:
                        if o.kind == "rel":
                            o.size = sz
                    forms.append(g)
    return forms, errors


def assign_classes(forms, cfg_of):
    """Default operand size per (map, opc, digit): d32 / d64 / f64."""
    groups = {}
    for f in forms:
        groups.setdefault((f.map, f.opc, f.digit), []).append(f)
    for k, fs in groups.items():
        fs64 = [f for f in fs if f.mode == 3] or fs
        cls = None
        for f in fs:
            c = cfg_of(f).get("class")
            if c:
                cls = c
        if cls is None:
            cls = "d32"
            if any(f.osz == 64 and not f.rexw for f in fs64 if f.osz != 8):
                cls = "d64"
        for f in fs:
            # a far JMP/CALL (m16:16/32/64) has the default operand size 32 in
            # 64-bit mode, whatever the page's near forms do (Vol. 2A JMP/CALL)
            f.vclass = "d32" if cls == "f64" and is_far(f) else cls
            if f.osz == 8 or "crnum" in f.hints or "sreg" in f.hints or cfg_of(f).get("anyosz"):
                f.okey = "any"          # MOV CRn ignores the operand size
            elif f.vclass == "f64" and f.mode == 3:
                f.okey = "any"
            else:
                f.okey = str(f.osz)


def specialize(forms, cfg_of):
    """Split forms on the operand kinds a row allows (r/m: register or
    memory) and on REP prefixes for string instructions."""
    out = []
    for f in forms:
        variants = [f]
        if cfg_of(f).get("anymod"):
            # the r/m operand is a register whatever ModRM.mod says (MOV CRn)
            f.mem = False
            f.hints["anymod"] = True
        elif any(o.kind == "rm" for o in f.operands):
            variants = []
            for mem in (False, True):
                g = Form(**{**f.__dict__, "operands": list(f.operands), "imm": list(f.imm),
                            "hints": dict(f.hints), "spec": dict(f.spec)})
                g.mem = mem
                if mem and len(g.operands) > 1 and g.operands[1].kind == "sreg":
                    g.okey = "any"      # MOV m16, Sreg: a 16-bit store at any operand size
                variants.append(g)
        elif any(o.kind == "mem" for o in f.operands) or cfg_of(f).get("memonly"):
            f.mem = True
        elif f.hints.get("regform"):
            f.mem = False
        reps = cfg_of(f).get("rep")
        if reps:
            vv = []
            for v in variants:
                for r in [None] + reps:
                    g = Form(**{**v.__dict__, "operands": list(v.operands), "imm": list(v.imm),
                                "hints": dict(v.hints), "spec": dict(v.spec)})
                    g.rep = r
                    vv.append(g)
            variants = vv
        out.extend(variants)
    return out


def dedupe(forms):
    """Rows that encode the same thing (aliases like JZ/JE, the operand-order
    duplicates of XCHG): the first row wins."""
    seen, out, dups = {}, [], []
    for f in forms:
        k = f.key()
        if k in seen:
            dups.append((f, seen[k]))
            continue
        seen[k] = f
        out.append(f)
    return out, dups
