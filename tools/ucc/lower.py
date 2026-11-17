"""Lowering SDM pseudocode to v1 micro-ops for one instruction form.

The Operation of an instruction is interpreted *abstractly* over a
partially-known state: everything the form fixes (OperandSize, where DEST
and SRC live, the instruction's name, 64-bit mode, CPL 0) is a compile-time
value, so the conditions on it fold and only their taken branch is emitted;
everything else (register and memory contents, RFLAGS) is a run-time value,
and code is emitted to compute it. That is an online partial evaluator in
the sense of Jones, Gomard & Sestoft, "Partial Evaluation and Automatic
Program Generation", Prentice Hall 1993 (ch. 4-5), with micro-ops as the
residual language.

Values carry a bit width; the operation size of a micro-op is the width of
its result. Constants and the (pre-extended) immediate adapt to the width of
the other operand, the way the manual reads.
"""

from dataclasses import dataclass

import oracle
import sdm
from sdm import (Assign, Bin, Call, Case, Do, For, If, Index, Name, Num, Prose,
                 Raise, Seg, Slice, Un, While)
from uop import FLAG, FMASK, GPR64, NTEMPS, NTRAPTEMPS, SEGS, SPECIAL, STATUS, UCC_NEG


class LowerError(Exception):
    pass


class NeedSpec(Exception):
    """The routine depends on a decode-time parameter the form doesn't fix
    yet: compile one variant per value (polyvariant specialization)."""

    def __init__(self, param, values):
        super().__init__(f"needs {param}")
        self.param = param
        self.values = values


ASZ_VALUES = {0: [16, 32], 1: [16, 32], 2: [32, 16], 3: [64, 32]}

# bit fields of control registers and MSRs the manual names (Intel SDM Vol. 3A
# 2.5 "Control Registers", 2.2.1 "Extended Feature Enable Register")
FIELDS = {
    "CR0": {"PE": 0, "MP": 1, "EM": 2, "TS": 3, "ET": 4, "NE": 5, "WP": 16, "AM": 18,
            "NW": 29, "CD": 30, "PG": 31},
    "CR4": {"VME": 0, "PVI": 1, "TSD": 2, "DE": 3, "PSE": 4, "PAE": 5, "MCE": 6, "PGE": 7,
            "PCE": 8, "OSFXSR": 9, "OSXMMEXCPT": 10, "UMIP": 11, "LA57": 12, "VMXE": 13,
            "SMXE": 14, "FSGSBASE": 16, "PCIDE": 17, "OSXSAVE": 18, "SMEP": 20, "SMAP": 21,
            "PKE": 22, "CET": 23, "PKS": 24},
    "EFER": {"SCE": 0, "LME": 8, "LMA": 10, "NXE": 11, "SVME": 12, "LMSLE": 13, "FFXSR": 14,
             "TCE": 15},
}
FIELDS["IA32_EFER"] = FIELDS["EFER"]
FIELDS["CR4"]["FRED"] = 32

# the manual's MSR names (Intel SDM Vol. 4) -> the core's special registers
MSR_ALIAS = {"IA32_EFER": "EFER", "IA32_STAR": "STAR", "IA32_LSTAR": "LSTAR",
             "IA32_CSTAR": "CSTAR", "IA32_FMASK": "FMASK", "IA32_FS_BASE": "FS_BASE",
             "IA32_GS_BASE": "GS_BASE", "IA32_KERNEL_GS_BASE": "KERNEL_GS_BASE",
             "IA32_SYSENTER_CS": "SYSENTER_CS", "IA32_SYSENTER_ESP": "SYSENTER_ESP",
             "IA32_SYSENTER_EIP": "SYSENTER_EIP", "IA32_TSC_AUX": "TSC_AUX",
             "IA32_PAT": "PAT", "IA32_APIC_BASE": "APIC_BASE", "IA32_PL0_SSP": "PL0_SSP",
             "IA32_PL3_SSP": "PL3_SSP", "IA32_U_CET": "U_CET", "IA32_S_CET": "S_CET",
             "IA32_TSC_ADJUST": "TSC_ADJUST", "IA32_XSS": "XSS"}

# segment register fields: the descriptor cache's attributes are
# {G, D/B, L, AVL, P, DPL[1:0], S, Type[3:0]} (bits 11..0)
SEG_ATTR_FIELDS = {"Type": (0, 4), "S": (4, 1), "DPL": (5, 2), "P": (7, 1), "AVL": (8, 1),
                   "L": (9, 1), "D": (10, 1), "B": (10, 1), "G": (11, 1)}
SEG_PARTS = {"Selector": "SEL", "Base": "BASE", "Limit": "LIMIT"}
SEG_REGS = ("ES", "CS", "SS", "DS", "FS", "GS", "LDTR", "TR")


VECTORS = {"DE": 0, "DB": 1, "BP": 3, "OF": 4, "BR": 5, "UD": 6, "NM": 7, "DF": 8,
           "TS": 10, "NP": 11, "SS": 12, "GP": 13, "PF": 14, "MF": 16, "AC": 17,
           "MC": 18, "XM": 19, "VE": 20, "CP": 21}
ERRCODE = {"DF", "TS", "NP", "SS", "GP", "PF", "AC", "CP"}

# register names -> (selector, width)
REGS = {}
for i, n in enumerate(GPR64):
    REGS[n] = (n, 64)
_legacy = ["AX", "CX", "DX", "BX", "SP", "BP", "SI", "DI"]
for i, n in enumerate(_legacy):
    REGS["E" + n] = (GPR64[i], 32)
    REGS[n] = (GPR64[i], 16)
for i, n in enumerate(["AL", "CL", "DL", "BL"]):
    REGS[n] = (GPR64[i], 8)
for n in ["AH", "CH", "DH", "BH"]:
    REGS[n] = (n, 8)
for i, n in enumerate(["SPL", "BPL", "SIL", "DIL"]):
    REGS[n] = (GPR64[4 + i], 8)
for i in range(8, 16):
    REGS[f"R{i}D"] = (f"R{i}", 32)
    REGS[f"R{i}W"] = (f"R{i}", 16)
    REGS[f"R{i}B"] = (f"R{i}", 8)
    REGS[f"R{i}L"] = (f"R{i}", 8)

SIZE_OF = {1: 8, 8: 8, 16: 16, 32: 32, 64: 64}
U_ARCH = set(GPR64) | {"AH", "CH", "DH", "BH", "REG", "RM", "NRIP", "FLAGS"}


def opsize(w):
    for s in (8, 16, 32, 64):
        if w <= s:
            return s
    raise LowerError(f"width {w} wider than 64")


@dataclass
class V:
    """A value: a constant, a selector holding `w` meaningful bits, or a pair."""
    kind: str             # 'k', 'sel', 'pair', 'mem'
    w: int = 64
    k: int = 0
    sel: str = ""
    adapt: bool = False   # takes the width of what it meets (constants, IMM)
    hi: object = None
    lo: object = None
    addr: object = None   # for 'mem' lvalues
    seg: str = "EASEG"    # for 'mem': the segment (SEGS), EASEG = the operand's

    def __repr__(self):
        if self.kind == "k":
            return f"#{self.k:x}"
        if self.kind == "pair":
            return f"{self.hi}:{self.lo}"
        return f"{self.sel}/{self.w}"


def K(v, w=64, adapt=True):
    return V("k", w, k=v, adapt=adapt)


class Routine:
    """Micro-ops of one routine, with labels resolved at assembly time."""

    def __init__(self, name):
        self.name = name
        self.code = []         # dicts; 'label' entries mark positions
        self.nlab = 0

    def label(self):
        self.nlab += 1
        return f"{self.name}#L{self.nlab}"

    def place(self, lab):
        self.code.append({"label": lab})

    def emit(self, op, sz=64, dst="ZERO", a="ZERO", b="ZERO", fmask=0, cc=0, x=0, imm=0, note="",
             **kw):
        u = dict(op=op, sz=opsize(sz) if sz else 64, dst=dst, a=a, b=b, c=kw.pop("c", "ZERO"),
                 fmask=fmask, cc=cc, x=x, imm=imm, note=note)
        u.update(kw)       # seg, vec, et, vl, kmode, kreg, rnd
        self.code.append(u)


def assigned_names(node, out=None):
    """The variable names a pseudocode tree assigns to (DEST := ...)."""
    if out is None:
        out = set()
    if isinstance(node, list):
        for n in node:
            assigned_names(n, out)
        return out
    if isinstance(node, Assign):
        d = node.dst
        while isinstance(d, (Slice, Index)) and not isinstance(d, Name):
            d = getattr(d, "e", None)
        if isinstance(d, Name):
            out.add(d.n)
    if hasattr(node, "__dataclass_fields__"):
        for k in node.__dataclass_fields__:
            v = getattr(node, k)
            if isinstance(v, (list,)) or hasattr(v, "__dataclass_fields__"):
                assigned_names(v, out)
    return out


class Lowerer:
    def __init__(self, form, page, ast, flags, library, name=None):
        self.f = form
        self.page = page
        self.ast = ast
        self.flags = flags             # prose spec
        self.lib = library             # name -> parsed body (Push, Pop, ...)
        self.r = Routine(name or form.name)
        # micro-trap routines (WALK) run in the middle of other routines: they
        # get their own temporaries
        trap = getattr(form, "trap", False)
        rng = range(NTEMPS, NTEMPS + NTRAPTEMPS) if trap else range(NTEMPS)
        self.free = [f"T{i}" for i in reversed(rng)]
        self.lib_live = {}
        self.protected = set()     # the temporaries of callers of an inlined function
        self.vars = {}                 # pseudocode variable -> V
        self.stmt_temps = []
        self.mem_cache = {}            # 'DEST'/'SRC' memory operand -> V loaded
        self.assigned_flags = set()
        self.producer = None
        self.binding = {}
        self.ret = None
        self.uses_rem = False
        self.loop_ends = []
        self.lib_live = {}
        self.native_flags = 0          # flags a primitive (ShiftOp, BitScan) set itself
        self.find_flag_assignments(ast)
        self.bind_operands()
        self.liveness(ast)
        self.producer = self.find_producer(ast)
        # 'cleared' flags come out of the producer only for a logic op (AND/OR/XOR
        # clear OF and CF); otherwise they're cleared explicitly at the end
        self.logic_producer = bool(self.producer) and all(
            isinstance(n.src, Bin) and n.src.op in ("AND", "OR", "XOR") for n in self.producer_nodes)
        self.result_mask = self.result_flags()

    # ------------------------------------------------------------ setup

    def find_flag_assignments(self, body):
        def walk(b):
            for s in b:
                if isinstance(s, Assign) and isinstance(s.dst, Name) and s.dst.n in FMASK:
                    self.assigned_flags.add(s.dst.n)
                if isinstance(s, Assign) and isinstance(s.dst, Prose) and s.dst.text in FMASK:
                    self.assigned_flags.add(s.dst.text)
                for sub in self.children(s):
                    walk(sub)
        walk(body)

    def liveness(self, body):
        """The last statement (in program order) that mentions each name, so a
        pseudocode variable's temporary is freed after it; a use inside a
        loop keeps it until the loop ends."""
        self.pos = {}
        self.last = {}
        counter = [0]

        def names(x, out):
            if isinstance(x, Name):
                out.add(x.n)
            elif isinstance(x, list):
                for y in x:
                    names(y, out)
            elif hasattr(x, "__dataclass_fields__"):
                for f in x.__dataclass_fields__:
                    v = getattr(x, f)
                    if not isinstance(v, (list,)) or f not in ("then", "els", "body", "arms"):
                        names(v, out)
            return out

        def walk(b, loop_end=None):
            for s in b:
                counter[0] += 1
                me = counter[0]
                if isinstance(s, (While, For)):
                    # names in the loop live to its end
                    start = me
                    inner = set()
                    walk(s.body)
                    end = counter[0]
                    names(s.body, inner)
                    names(s.cond if isinstance(s, While) else [s.lo, s.hi], inner)
                    for n in inner:
                        self.last[n] = max(self.last.get(n, 0), end)
                    self.pos[id(s)] = end
                    continue
                for n in names(s if not self.children(s) else
                               (s.cond if isinstance(s, If) else s), set()):
                    self.last[n] = max(self.last.get(n, 0), me)
                for sub in self.children(s):
                    walk(sub)
                self.pos[id(s)] = counter[0]
        walk(body)

    def free_dead(self, s):
        p = self.pos.get(id(s))
        if p is None or getattr(self, "_inlining", 0):
            return
        for n in list(self.vars):
            if n in ("RESULT",):
                continue
            if self.last.get(n, 1 << 30) <= p:
                v = self.vars.pop(n)
                for t in ([v.hi, v.lo] if v.kind == "pair" else [v]):
                    if t is not None and t.kind == "sel" and t.sel.startswith("T") \
                            and t.sel not in self.free and t.sel not in self.protected:
                        self.free.append(t.sel)

    @staticmethod
    def children(s):
        if isinstance(s, If):
            return [s.then] + ([s.els] if s.els else [])
        if isinstance(s, (While, For)):
            return [s.body]
        if isinstance(s, Case):
            return [a[1] for a in s.arms]
        return []

    def result_flags(self):
        mask = 0
        for fl, act in self.flags.items():
            if fl in self.assigned_flags:
                continue
            if act == "result" or (act == "clear" and self.logic_producer):
                mask |= FMASK[fl]
        return mask

    ARITH = ("+", "-", "AND", "OR", "XOR")

    def find_producer(self, body):
        prod = self._find_producer(body)
        nodes = []

        def walk(b):
            for st in b:
                if id(st) in prod:
                    nodes.append(st)
                for sub in self.children(st):
                    walk(sub)
        walk(body)
        self.producer_nodes = nodes
        return prod

    def _find_producer(self, body):
        """The assignment whose arithmetic sets the 'according to the result'
        flags: the one before ModifyStatusFlags, else the last arithmetic
        assignment of the Operation."""
        last = [None]
        marker = []

        def arith(e):
            return (isinstance(e, Bin) and e.op in self.ARITH) or \
                (isinstance(e, Un) and e.op in ("-",))

        def walk(b):
            for s in b:
                if isinstance(s, Do) and isinstance(s.e, (Name, Call)) and \
                        getattr(s.e, "n", getattr(s.e, "f", "")) == "ModifyStatusFlags":
                    marker.append(last[0])
                if isinstance(s, Assign) and arith(s.src) and not (
                        isinstance(s.dst, Name) and s.dst.n in FMASK):
                    last[0] = s
                for sub in self.children(s):
                    walk(sub)
        walk(body)
        # every assignment a ModifyStatusFlags follows (the same comparison can
        # appear on several paths), else the last arithmetic assignment
        return set(map(id, marker)) if marker else ({id(last[0])} if last[0] else set())

    def bind_operands(self):
        ops = self.f.operands
        n = len(ops)
        b = {}
        if n >= 1:
            b["DEST"] = 0
        if n == 1:
            b["SRC"] = 0
            b["SRC1"] = 0
        if n == 2:
            b["SRC"] = 1
            b["SRC1"] = 0
            b["SRC2"] = 1
            b["COUNT"] = 1
            b["BitBase"] = 0
            b["BitOffset"] = 1
        if n == 3:
            b["DEST1"] = 0
            b["DEST2"] = 1
            b["COUNT"] = 2
            b["SRC1"] = 1
            b["SRC2"] = 2
            b["SRC"] = 1
        self.binding = b

    # ------------------------------------------------------------ temps

    def tmp(self, keep=False):
        if not self.free:
            raise LowerError("out of micro temporaries")
        t = self.free.pop()
        if not keep:
            self.stmt_temps.append(t)
        return t

    def release_stmt(self):
        keep = {v.sel for v in self.vars.values() if v.kind == "sel"}
        keep |= {p.sel for v in self.vars.values() if v.kind == "pair" for p in (v.hi, v.lo)}
        keep |= {v.sel for v in self.mem_cache.values() if v and v.kind == "sel"}
        keep |= self.protected
        for t in self.stmt_temps:
            if t not in keep and t not in self.free:
                self.free.append(t)
        self.stmt_temps = []

    # ------------------------------------------------------------ emission helpers

    def E(self, *a, **k):
        if k.get("a") == "K" and k.get("b") == "K":
            # one immediate per micro-op: K as both sources reads it twice
            raise LowerError(f"{a[0]}: both sources are the immediate")
        self.r.emit(*a, **k)

    def to_sel(self, v, w=None):
        """A selector holding v (constants become K or are materialized)."""
        w = w or v.w
        if v.kind == "sel":
            return v.sel
        if v.kind == "k":
            val = v.k & ((1 << 64) - 1)
            sx = val - (1 << 64) if val >> 63 else val
            if w <= 32:
                m = val & ((1 << w) - 1)
                return ("K", m - (1 << 32) if m >> 31 else m)
            if -(1 << 31) <= sx < (1 << 31):
                return ("K", sx)
            t = self.tmp()
            lo = val & 0xFFFFFFFF
            self.E("MOV", 64, dst=t, a="K", imm=lo - (1 << 32) if lo >> 31 else lo,
                   note="materialize")
            self.E("KHI", 64, dst=t, imm=val >> 32)
            return t
        if v.kind == "mem":
            return self.to_sel(self.load(v), w)
        raise LowerError(f"can't use {v} as one value")

    def srcs(self, *vals, w):
        """Selectors for operands; K (immediate) at most once per uop."""
        out = []
        kimm = None
        for v in vals:
            s = self.to_sel(self.widen(v, w), w)
            if isinstance(s, tuple):
                if kimm is not None and kimm != s[1]:
                    t = self.tmp()
                    self.E("MOV", w, dst=t, a="K", imm=s[1])
                    s = t
                else:
                    kimm = s[1]
                    s = "K"
            out.append(s)
        return out, (kimm or 0)

    def widen(self, v, w, signed=False):
        """v as a w-bit value (zero- or sign-extended)."""
        if v.kind == "k" or v.adapt:
            return V(v.kind, w, k=v.k, sel=v.sel, adapt=v.adapt)
        if v.kind == "mem":
            v = self.load(v)
        if v.kind == "pair":
            if w <= v.lo.w:
                return v.lo
            raise LowerError(f"pair {v} used as {w}-bit value")
        if v.w >= w:
            return v
        if v.w == 1:
            return V("sel", w, sel=v.sel)     # booleans are kept as 0/1
        t = self.tmp()
        self.E("SEXT" if signed else "ZEXT", opsize(v.w), dst=t, a=v.sel,
               x=[8, 16, 32, 64].index(opsize(w)))
        return V("sel", w, sel=t)

    def load(self, v):
        if v.kind != "mem":
            return v
        t = self.tmp()
        a, imm = self.addr_of(v.addr)
        self.E("LD", v.w, dst=t, a=a, imm=imm, seg=SEGS[v.seg])
        return V("sel", v.w, sel=t)

    def addr_of(self, addr):
        """(selector, displacement) for an address value."""
        if addr.kind == "k":
            s = self.to_sel(addr, 64)
            if isinstance(s, tuple):
                return "ZERO", s[1]
            return s, 0
        if addr.kind == "sel":
            return addr.sel, 0
        raise LowerError(f"address {addr}")

    # ------------------------------------------------------------ names

    def need(self, param, values):
        if param in self.f.spec:
            return self.f.spec[param]
        raise NeedSpec(param, values)

    def static(self, name):
        n = name
        m = self.f.mode
        if m == 4:
            # system routines (DELIVER, WALK) run in any mode: only the
            # operand size is static
            if n in ("OperandSize", "Operandsize", "OpSize"):
                return 64
            if n in ("TRUE", "FALSE"):
                return int(n == "TRUE")
            return None
        if n in ("OperandSize", "Operandsize", "OpSize"):
            return self.f.osz
        if n in ("AddressSize", "AddrSize"):
            return self.need("AddressSize", ASZ_VALUES[m])
        if n in ("StackAddrSize", "StackAddressSize"):
            return 64 if m == 3 else self.need("StackAddrSize", [16, 32])
        if n == "PE" or n == "CR0.PE":
            return int(m != 0)
        if n in ("VM", "EFLAGS.VM", "RFLAGS.VM") and m != 1:
            return 0
        if n in ("IA32_EFER.LMA", "EFER.LMA") and m in (0, 3):
            return int(m == 3)
        if n in ("CS.L",) and m in (0, 1, 2):
            return 0
        if n in ("CS.L",):
            return 1
        if n == "CR4.FRED":
            return 0            # v1 has no FRED (CR4.FRED is reserved)
        if n == "Mode64" and m != 4:
            return int(m == 3)
        if n == "CodeSize":
            return {0: 16, 1: 16, 2: 32, 3: 64}[m]
        if n == "NumberOfOperands":
            return len([o for o in self.f.operands])
        if n in ("TRUE",):
            return 1
        if n in ("FALSE",):
            return 0
        if n == "CPL" and m == 0:
            return 0
        # static facts about the form, for the overrides
        if n == "MemoryOperand":
            return int(bool(self.f.mem))
        if n == "RegisterOffset":
            return int(len(self.f.operands) > 1 and self.f.operands[1].kind == "reg")
        if n == "CrNum":
            if "crnum" in self.f.hints and self.f.page.startswith("MOV_Move_to_from_Debug"):
                return self.f.hints["crnum"]
            if "crnum" in self.f.hints:
                return self.f.hints["crnum"]
            return None
        if n == "CrDest":
            return int(bool(self.f.operands) and self.f.operands[0].kind == "cr")
        if n == "FarJump":
            return int(any(o.kind in ("farptr", "farmem") for o in self.f.operands))
        if n == "FarPtrImm":
            return int(any(o.kind == "farptr" for o in self.f.operands))
        if n == "SegOfLoad":
            return {"LDS": 3, "LES": 0, "LFS": 4, "LGS": 5, "LSS": 2}.get(self.f.mnem)
        if n == "LoopKind":
            return {"LOOP": 0, "LOOPE": 1, "LOOPZ": 1, "LOOPNE": 2, "LOOPNZ": 2}.get(self.f.mnem)
        if n.startswith("Is_"):
            # Is_<MNEMONIC>: 1 in the form of that mnemonic (pages with several)
            return int(self.f.mnem == n[3:])
        if n == "IsGS":
            return int(self.f.mnem in ("RDGSBASE", "WRGSBASE"))
        if n == "IsVERW":
            return int(self.f.mnem == "VERW")
        if n == "IsLSL":
            return int(self.f.mnem == "LSL")
        if n == "SregDest":
            return int(bool(self.f.operands) and self.f.operands[0].kind == "sreg")
        if n == "SegmentOperand":
            return int(any(o.kind == "seg" for o in self.f.operands))
        if n == "Sreg":
            if "sreg" in self.f.hints:
                return self.f.hints["sreg"]
            for o in self.f.operands:
                if o.kind == "seg":
                    return {"ES": 0, "CS": 1, "SS": 2, "DS": 3, "FS": 4, "GS": 5}[o.reg]
            return None
        if n == "RepPrefix":
            return int(self.f.rep is not None)
        if n == "RepNE":
            return int(self.f.rep == "F2")
        if n == "OperandBytes":
            return self.f.osz // 8
        if n == "Signed":
            return int(self.page.name in ("IMUL", "IDIV"))
        return None

    def operand(self, idx, role):
        o = self.f.operands[idx]
        w = o.size or self.f.osz
        if w > 64:
            w = 64          # a wide memory operand (m128, m512) is used through its address
        if o.kind in ("rm",) and not self.f.mem:
            return V("sel", w, sel="RM")
        if o.kind in ("rm", "mem"):
            return V("mem", w, addr=V("sel", 64, sel="EA"))
        if o.kind == "reg":
            return V("sel", w, sel="REG")
        if o.kind == "opreg":
            return V("sel", w, sel="RM")
        if o.kind == "vvvv":
            return V("sel", w, sel="VVVV")
        if o.kind in ("imm", "rel"):
            return V("sel", self.f.osz if o.kind == "imm" else 64, sel="IMM", adapt=True)
        if o.kind == "fixed":
            s, rw = REGS[o.reg]
            return V("sel", rw, sel=s)
        if o.kind == "one":
            return K(1)
        if o.kind == "port":
            return V("sel", 16, sel="RDX")
        if o.kind == "sreg":
            return V("sel", 16, sel="SEGREG")
        if o.kind == "farptr":
            return V("sel", 64, sel="IMM", adapt=True)
        if o.kind == "farmem":
            return V("mem", self.f.osz, addr=V("sel", 64, sel="EA"))
        if o.kind == "seg":
            return V("special", 64, sel=o.reg)
        if o.kind == "cr":
            return V("sel", 64, sel="CRn")
        raise LowerError(f"operand kind {o.kind}")

    def rname(self, n):
        """Resolve a name to a V (rvalue/lvalue location)."""
        if n in self.vars:
            return self.vars[n]
        if n in self.binding and self.binding[n] < len(self.f.operands):
            return self.operand(self.binding[n], n)
        if n in ("RIP", "EIP", "IP"):
            return V("sel", 64 if n == "RIP" or self.f.osz == 64 else {"EIP": 32, "IP": 16}[n],
                     sel="NRIP")
        if n in REGS:
            s, w = REGS[n]
            return V("sel", w, sel=s)
        if n in ("rSI", "rDI", "rCX", "rBX"):
            # the string registers at the address size: SI/ESI/RSI
            return V("sel", self.static("AddressSize"), sel={"rSI": "RSI", "rDI": "RDI",
                                                              "rCX": "RCX", "rBX": "RBX"}[n])
        if n == "IMM2":
            return V("sel", 8, sel="IMM2")
        if n == "sBP":
            return V("sel", self.static("StackAddrSize"), sel="RBP")
        if n == "imm8" or n == "imm16":
            for i, o in enumerate(self.f.operands):
                if o.kind == "imm":
                    return self.operand(i, n)
        if n == "sSP":
            # the stack pointer at the stack-address size: SP/ESP/RSP
            return V("sel", self.static("StackAddrSize"), sel="RSP")
        if n == "SregValue":
            for o in self.f.operands:
                if o.kind == "seg":
                    return V("special", 64, sel=o.reg)
            if any(o.kind == "sreg" for o in self.f.operands):
                return V("sel", 16, sel="SEGREG")
        if n == "FarSelector":
            return V("sel", 16, sel="IMM2")
        if n in ("TRAPVA", "TRAPX"):
            return V("sel", 64, sel=n)
        if n.lower() == "accumulator":
            return V("sel", self.f.osz, sel="RAX")
        if n in ("RFLAGS", "EFLAGS", "FLAGS"):
            return V("sel", {"RFLAGS": 64, "EFLAGS": 32, "FLAGS": 16}[n], sel="FLAGS")
        if n in MSR_ALIAS:
            return V("special", 64, sel=MSR_ALIAS[n])
        if "." in n and n.split(".")[0] in SEG_REGS:
            reg, _, fld = n.partition(".")
            if fld in SEG_PARTS:
                return V("special", 64 if fld != "Selector" else 16, sel=f"{reg}_{SEG_PARTS[fld]}")
            if fld in SEG_ATTR_FIELDS:
                lo, w = SEG_ATTR_FIELDS[fld]
                return V("segfield", w, sel=f"{reg}_ATTR", k=lo)
        if "." in n:
            reg, _, fld = n.partition(".")
            if reg in FIELDS and fld in FIELDS[reg]:
                return V("field", 1, sel=reg if reg != "IA32_EFER" else "EFER", k=FIELDS[reg][fld])
            if reg in ("EFLAGS", "RFLAGS") and fld in FLAG:
                return V("flag", 1, sel=fld)
        if n in SPECIAL:
            return V("special", 64, sel=n)
        if n.replace(".", "_") in SPECIAL:
            return V("special", 64, sel=n.replace(".", "_"))
        return None

    # ------------------------------------------------------------ expressions

    def eval(self, e, want=None):
        """Lower an expression; `want` is the width the context expects."""
        if isinstance(e, Num):
            return K(e.v)
        if isinstance(e, Name):
            st = self.static(e.n)
            if st is not None:
                return K(st)
            if e.n == "IOPL":
                # the two-bit I/O privilege level, RFLAGS[13:12]
                t = self.tmp()
                self.E("SLICE", 64, dst=t, a="FLAGS", imm=12 | (2 << 8))
                return V("sel", 8, sel=t)
            if e.n in FLAG or e.n == "INTF":
                t = self.tmp()
                self.E("GETF", 8, dst=t, imm=FLAG[{"INTF": "IF"}.get(e.n, e.n)])
                return V("sel", 1, sel=t)
            v = self.rname(e.n)
            if v is None:
                raise LowerError(f"unknown name '{e.n}'")
            if v.kind == "special":
                t = self.tmp()
                self.E("RDS", 64, dst=t, imm=SPECIAL[v.sel])
                return V("sel", 64, sel=t)
            if v.kind == "segfield":
                t = self.tmp()
                self.E("RDS", 64, dst=t, imm=SPECIAL[v.sel])
                self.E("SLICE", 64, dst=t, a=t, imm=v.k | (v.w << 8))
                return V("sel", v.w, sel=t)
            if v.kind == "field":
                t = self.tmp()
                self.E("RDS", 64, dst=t, imm=SPECIAL[v.sel])
                self.E("SLICE", 64, dst=t, a=t, imm=v.k | (1 << 8))
                return V("sel", 1, sel=t)
            if v.kind == "flag":
                t = self.tmp()
                self.E("GETF", 8, dst=t, imm=FLAG[v.sel])
                return V("sel", 1, sel=t)
            return v
        if isinstance(e, Prose):
            t = oracle.norm(e.text)
            if t in ("remainder", "rem"):
                return V("sel", self.f.osz, sel="REM")
            raise LowerError(f"prose value '{e.text}'")
        if isinstance(e, Un):
            a = self.eval(e.a, want)
            if a.kind == "k":
                return K(-a.k if e.op == "-" else ~a.k)
            w = want or a.w
            if a.adapt:
                a = self.widen(a, w)
            w = opsize(max(a.w, 8)) if not want else want
            t = self.tmp()
            (s,), imm = self.srcs(a, w=w)
            self.E("NEG" if e.op == "-" else "NOT", w, dst=t, a=s, imm=imm,
                   fmask=self.flag_mask_for(e))
            return V("sel", w, sel=t)
        if isinstance(e, Bin):
            return self.binop(e, want)
        if isinstance(e, Call):
            return self.call(e, want)
        if isinstance(e, Slice):
            return self.slice(e)
        if isinstance(e, (Index, Seg)):
            return self.memref(e, want)
        raise LowerError(f"expression {e}")

    def memref(self, e, want):
        """Memory[addr], SS:RSP, Memory64[addr] as a memory location."""
        w = want or self.f.osz
        if isinstance(e, Seg):
            addr = self.eval(e.addr, 64)
            return V("mem", w, addr=self.as_addr(addr), seg=self.seg_of(e.seg))
        if isinstance(e.e, Name) and e.e.n.startswith(("Memory", "Linear", "Physical")):
            # Memory[SEG:addr] / Memory[addr] (the operand segment), Linear[addr]
            # (descriptor tables, the IDT), Physical[addr] (page tables); a size suffix
            # gives the width (Memory64, Linear16, ...)
            kind = next(k for k in ("Memory", "Linear", "Physical") if e.e.n.startswith(k))
            sfx = e.e.n[len(kind):]
            if sfx:
                w = int(sfx)
            idx = e.i
            seg = {"Memory": "EASEG", "Linear": "LINEAR", "Physical": "PHYS"}[kind]
            if isinstance(idx, Seg):
                seg = self.seg_of(idx.seg)
                idx = idx.addr
            addr = self.eval(idx, 64)
            if kind == "Memory" and isinstance(idx, Name) and addr.kind == "mem":
                # Memory[DEST] with DEST a memory operand (m16:32): that
                # operand, not a load through its value (far JMP/CALL and
                # LxS read their offset twice until tests/isa/decode64)
                return V("mem", w, addr=addr.addr, seg=addr.seg)
            return V("mem", w, addr=self.as_addr(addr), seg=seg)
        raise LowerError(f"index {e}")

    @staticmethod
    def seg_of(name):
        """'DS:' in the manual's string operands means DS unless overridden."""
        return "EASEG" if name == "DS" else name

    def as_addr(self, a):
        if a.kind == "mem":
            a = self.load(a)
        if a.kind == "sel" and a.w < 64 and not a.adapt:
            a = self.widen(a, 64)
        return a

    ALU = {"+": "ADD", "-": "SUB", "AND": "AND", "OR": "OR", "XOR": "XOR",
           "<<": "SHL", ">>": "SHR"}
    CMPS = {"=": "EQ", "!=": "NE", "<": "LTU", ">": "GTU", "<=": "LEU", ">=": "GEU"}

    def flag_mask_for(self, e):
        if self.cur_producer is not None and self.cur_producer.src is e:
            return self.result_mask
        return 0

    def binop(self, e, want):
        op = e.op
        if op in self.CMPS or op in ("AND", "OR") and self.is_bool(e):
            # a boolean in value position: 0/1
            t = self.tmp()
            lf, lend = self.r.label(), self.r.label()
            self.branch(e, lf, False)
            self.E("MOV", 8, dst=t, a="K", imm=1)
            self.E("JMP", imm=lend)
            self.r.place(lf)
            self.E("MOV", 8, dst=t, a="ZERO")
            self.r.place(lend)
            return V("sel", 1, sel=t)
        if op in ("*", "/", "MOD", "SignedModulus"):
            sv = self.static_val(e) if op in ("*", "/") else None
            if sv is not None:
                return K(sv)
            return self.muldiv(e, want)
        # ADC / SBB idioms: DEST + SRC + CF, DEST - (SRC + CF)
        fm = self.flag_mask_for(e)
        if op == "+" and isinstance(e.b, Name) and e.b.n == "CF" and isinstance(e.a, Bin) \
                and e.a.op == "+":
            return self.alu("ADC", e.a.a, e.a.b, want, fm)
        if op == "-" and isinstance(e.b, Bin) and e.b.op == "+" and isinstance(e.b.b, Name) \
                and e.b.b.n == "CF":
            return self.alu("SBB", e.a, e.b.a, want, fm)
        if op not in self.ALU:
            raise LowerError(f"operator {op}")
        # an operand's width is the context for the other one's SignExtend /
        # ZeroExtend ('SRC1 - SignExtend(SRC2)' is at SRC1's width)
        a = self.eval(e.a, want)
        # (a memory operand too: 'CMP m8, r8' compared at 64 bits, its SF and
        # OF from bit 63, until tests/diff found it)
        sib = want or (a.w if a.kind in ("sel", "mem") and not a.adapt else None)
        b = self.eval(e.b, sib)
        if want is None and isinstance(e.a, Call) and e.a.f in ("SignExtend", "ZeroExtend") \
                and b.kind == "sel" and not b.adapt:
            a = self.eval(e.a, b.w)
        if a.kind == "k" and b.kind == "k" and not fm:
            return K(self.fold(op, a.k, b.k))
        return self.alu(self.ALU[op], a, b, want, fm)

    @staticmethod
    def fold(op, a, b):
        return {"+": a + b, "-": a - b, "AND": a & b, "OR": a | b, "XOR": a ^ b,
                "<<": a << b, ">>": a >> b}[op]

    def alu(self, op, a, b, want, fm=0, dst=None):
        if not isinstance(a, V):
            a = self.eval(a, want)
        if not isinstance(b, V):
            b = self.eval(b, want)
        fixed = [v.w for v in (a, b) if not v.adapt and v.kind != "k"]
        if op in ("SHL", "SHR", "SAR"):
            fixed = [a.w] if not a.adapt and a.kind != "k" else []
        w = max(fixed) if fixed else (want or 64)
        if want and op not in ("SHL", "SHR", "SAR") and want > w and not fixed:
            w = want
        w = opsize(w)
        if op in ("SHL", "SHR", "SAR"):
            (sa,), ia = self.srcs(a, w=w)
            (sb,), ib = self.srcs(b, w=8)
            if sa == "K" and sb == "K":
                t = self.tmp()
                self.E("MOV", w, dst=t, a="K", imm=ia)
                sa = t
            imm = ia if sa == "K" else ib
        else:
            (sa, sb), imm = self.srcs(a, b, w=w)
        t = dst or self.tmp()
        self.E(op, w, dst=t, a=sa, b=sb, imm=imm, fmask=fm)
        return V("sel", w, sel=t)

    def is_bool(self, e):
        return isinstance(e, Bin) and (e.op in self.CMPS or (
            e.op in ("AND", "OR") and (self.is_bool(e.a) or self.is_bool(e.b))))

    # ------------------------------------------------------------ multiply / divide

    def muldiv(self, e, want):
        op = e.op
        signed = self.page.name in ("IMUL", "IDIV") or op == "SignedModulus"
        if op == "*":
            a = self.eval(e.a)
            b = self.eval(e.b)
            w = max(v.w for v in (a, b) if not v.adapt and v.kind != "k") \
                if any(not v.adapt and v.kind != "k" for v in (a, b)) else self.f.osz
            w = opsize(w)
            (sa, sb), imm = self.srcs(a, b, w=w)
            t = self.tmp()
            self.E("MULS" if signed else "MULU", w, dst=t, a=sa, b=sb, imm=imm,
                   fmask=FMASK["CF"] | FMASK["OF"])
            h = self.tmp()
            self.E("MOV", w, dst=h, a="HI")
            return V("pair", 2 * w, hi=V("sel", w, sel=h), lo=V("sel", w, sel=t))
        # division: dividend is a pair (DX:AX) or a double-width register (AX)
        dvd = self.eval(e.a)
        dvs = self.eval(e.b)
        w = opsize(dvs.w if not dvs.adapt else self.f.osz)
        if dvd.kind == "pair":
            hi, lo = dvd.hi, dvd.lo
        elif dvd.kind == "sel" and dvd.w == 2 * w and dvd.sel in GPR64[:4] and w == 8:
            # AX at byte size: AH:AL
            hi = V("sel", 8, sel=["AH", "CH", "DH", "BH"][GPR64.index(dvd.sel)])
            lo = V("sel", 8, sel=dvd.sel)
        else:
            raise LowerError(f"dividend {dvd}")
        (sh,), _ = self.srcs(hi, w=w)
        self.E("MOV", w, dst="HI", a=sh)
        (sl, sd), imm = self.srcs(lo, dvs, w=w)
        q = self.tmp()
        self.E("DIVS" if signed else "DIVU", w, dst=q, a=sl, b=sd, imm=imm)
        if op in ("MOD", "SignedModulus"):
            return V("sel", w, sel="REM")
        # the divider raises #DE itself when the quotient doesn't fit in w
        # bits, so the quotient is a w-bit value, zero-extended to 2w
        return V("sel", w, sel=q)

    # ------------------------------------------------------------ calls

    def call(self, e, want):
        f = e.f
        if f in ("SignExtend", "ZeroExtend"):
            v = self.eval(e.args[0])
            w = want or 64
            if v.adapt or v.kind == "k":
                return V(v.kind, w, k=v.k, sel=v.sel, adapt=True) if f == "SignExtend" or \
                    v.kind == "k" else self.widen(V(v.kind, self.f.osz, sel=v.sel), w)
            return self.widen(v, w, signed=(f == "SignExtend"))
        if f in ("TruncateToOperandSize",):
            v = self.eval(e.args[0])
            if v.kind == "pair":
                return v.lo
            return V(v.kind, self.f.osz, k=v.k, sel=v.sel) if v.kind != "mem" else v
        if f == "Concat":
            hi = self.eval(e.args[0])
            lo = self.eval(e.args[1])
            return V("pair", hi.w + lo.w, hi=hi, lo=lo)
        if f in ("MSB", "LSB"):
            v = self.eval(e.args[0])
            if v.kind == "mem":
                v = self.load(v)
            t = self.tmp()
            pos = (v.w - 1) if f == "MSB" else 0
            (s,), imm = self.srcs(v, w=opsize(v.w))
            self.E("SLICE", 64, dst=t, a=s, imm=pos | (1 << 8))
            return V("sel", 1, sel=t)
        if f == "Bit":
            base = self.eval(e.args[0])
            off = self.eval(e.args[1])
            if base.kind == "mem":
                base = self.load(base)
            w = opsize(base.w)
            t = self.tmp()
            (sb, so), imm = self.srcs(base, off, w=w)
            self.E("SHR", w, dst=t, a=sb, b=so, imm=imm)
            self.E("AND", 8, dst=t, a=t, b="K", imm=1)
            return V("sel", 1, sel=t)
        if f in ("SAR", "SHL", "SHR") and len(e.args) == 2:
            a = self.eval(e.args[0], want)
            b = self.eval(e.args[1])
            return self.alu(f, a, b, want)
        if f == "EffectiveAddress":
            return V("sel", 64, sel="EA")
        if f in self.lib:
            return self.inline(f, e.args, want)
        if f in ("DivU", "DivS"):
            hi, lo, d = (self.eval(a) for a in e.args)
            w = opsize(max(x.w for x in (hi, lo, d) if not x.adapt and x.kind != "k"))
            (sh,), _ = self.srcs(hi, w=w)
            self.E("MOV", w, dst="HI", a=sh)
            (sl, sd), imm = self.srcs(lo, d, w=w)
            q = self.tmp()
            self.E("DIVU" if f == "DivU" else "DIVS", w, dst=q, a=sl, b=sd, imm=imm)
            return V("sel", w, sel=q)
        if f in ("MulU", "MulS", "IMulTrunc"):
            a, b = (self.eval(x) for x in e.args)
            w = opsize(max([x.w for x in (a, b) if not x.adapt and x.kind != "k"] or [self.f.osz]))
            (sa, sb), imm = self.srcs(a, b, w=w)
            t = self.tmp()
            op = {"MulU": "MULU", "MulS": "MULS", "IMulTrunc": "IMULT"}[f]
            self.E(op, w, dst=t, a=sa, b=sb, imm=imm, fmask=FMASK["CF"] | FMASK["OF"])
            return V("sel", w, sel=t)
        if f == "InPort":
            # InPort(port): a value read from the I/O space at the operand size
            port = self.widen(self.eval(e.args[0], 16), 64)
            sp = self.to_sel(port, 64)
            if isinstance(sp, tuple):
                k = self.tmp()
                self.E("MOV", 64, dst=k, a="K", imm=sp[1])
                sp = k
            t = self.tmp()
            self.E("IN", self.f.osz, dst=t, a=sp)
            return V("sel", self.f.osz, sel=t)
        if f in ("HighHalf",):
            return V("sel", self.f.osz, sel="HI")
        if f in ("Remainder",):
            return V("sel", self.f.osz, sel="REM")
        if f in ("ShiftOp",):
            # ShiftOp(op, dest, count): the core's x86 shifter, flags per fmask
            kind = e.args[0].n
            d = self.eval(e.args[1])
            c = self.eval(e.args[2])
            w = opsize(d.w if not d.adapt else self.f.osz)
            (sd, sc), imm = self.srcs(d, c, w=w)
            t = self.tmp()
            self.E(kind, w, dst=t, a=sd, b=sc, imm=imm, fmask=self.result_mask_all())
            self.native_flags |= self.result_mask_all()
            return V("sel", w, sel=t)
        if f == "Crc32":
            # Crc32(acc, data): CRC-32C (Castagnoli) of data's bytes into acc
            acc = self.widen(self.eval(e.args[0]), 64)
            d = self.eval(e.args[1])
            if d.kind == "mem":
                d = self.load(d)
            w = opsize(d.w if not d.adapt else 32)
            (sa, sd), _ = self.srcs(acc, d, w=w)
            t = self.tmp()
            self.E("CRC32", w, dst=t, a=sa, b=sd)
            return V("sel", 32, sel=t)
        if f == "ApicRead":
            # ApicRead(msr): an x2APIC register or IA32_TSC_DEADLINE (#GP if illegal)
            a = self.widen(self.eval(e.args[0], 64), 64)
            sa = self.to_sel(a, 64)
            if isinstance(sa, tuple):
                k = self.tmp()
                self.E("MOV", 64, dst=k, a="K", imm=sa[1])
                sa = k
            t = self.tmp()
            self.E("APIC", 64, dst=t, a=sa, x=0)
            return V("sel", 64, sel=t)
        if f == "Random":
            # Random(): the random number source (RDRAND, RDSEED); CF := valid
            t = self.tmp()
            self.E("RDRAND", self.f.osz, dst=t, fmask=0x3F)
            self.native_flags |= 0x3F
            return V("sel", self.f.osz, sel=t)
        if f == "AddOverflowCarry":
            # ADOX: DEST + SRC + OF, the carry out into OF only
            a = self.eval(e.args[0])
            b = self.eval(e.args[1])
            if a.kind == "mem":
                a = self.load(a)
            w = opsize(a.w)
            (sa, sb), imm = self.srcs(a, b, w=w)
            t = self.tmp()
            self.E("ADC", w, dst=t, a=sa, b=sb, imm=imm, x=1, fmask=0x20)
            self.native_flags |= 0x20
            return V("sel", w, sel=t)
        if f == "DoubleShift":
            # DoubleShift(SHLD|SHRD, dest, src, count): the core's double shifter
            kind = e.args[0].n
            d = self.eval(e.args[1])
            sv = self.eval(e.args[2])
            c = self.eval(e.args[3])
            w = opsize(d.w if not d.adapt else self.f.osz)
            if d.kind == "mem":
                d = self.load(d)
            (sd, ss), _ = self.srcs(d, sv, w=w)
            cv = self.widen(c, 64) if c.kind == "sel" and not c.adapt else c
            sc = self.to_sel(cv, 64)
            if isinstance(sc, tuple):
                k = self.tmp()
                self.E("MOV", 64, dst=k, a="K", imm=sc[1])
                sc = k
            t = self.tmp()
            m = self.result_mask_all()
            self.E(kind, w, dst=t, a=sd, b=ss, c=sc, fmask=m)
            self.native_flags |= m
            return V("sel", w, sel=t)
        if f == "AluOp":
            # AluOp(OP, a, b): one of the core's ALU operations (rtl/v1/v1_alu.sv)
            # at the operand size; flags as the page's Flags Affected says
            kind = e.args[0].n
            a = self.eval(e.args[1])
            b = self.eval(e.args[2])
            if a.kind == "mem":
                a = self.load(a)
            if b.kind == "mem":
                b = self.load(b)
            w = self.f.osz
            (sa, sb), imm = self.srcs(a, b, w=w)
            t = self.tmp()
            m = self.result_mask_all() | self.clear_mask()
            self.E(kind, w, dst=t, a=sa, b=sb, imm=imm, fmask=m)
            self.native_flags |= m
            return V("sel", w, sel=t)
        if f in ("BitScan",):
            kind = e.args[0].n
            v = self.eval(e.args[1])
            if v.kind == "mem":
                v = self.load(v)
            w = opsize(v.w if not v.adapt else self.f.osz)
            (s,), imm = self.srcs(v, w=w)
            t = self.tmp()
            m = self.result_mask_all() | self.clear_mask()
            self.E(kind, w, dst=t, a=s, imm=imm, fmask=m)
            self.native_flags |= m
            return V("sel", w, sel=t)
        if f == "Port":
            return V("port", self.f.osz, addr=self.eval(e.args[0], 16))
        if f == "ReadMSR" or f == "WriteMSR":
            raise LowerError(f"{f} is a statement")
        raise LowerError(f"function '{f}'")

    def clear_mask(self):
        return sum(FMASK[f] for f, a in self.flags.items() if a == "clear")

    def result_mask_all(self):
        """Every status flag the prose doesn't call unaffected/undefined."""
        m = 0
        for fl, act in self.flags.items():
            if act not in ("unaffected", "undefined") and fl not in self.assigned_flags:
                m |= FMASK[fl]
        return m

    def call_sub(self, fname, args):
        """A micro-subroutine: the arguments go in A0-A3, then UCALL; the
        callee runs in its own bank of temporaries."""
        params, _ = self.lib[fname]
        if len(args) > 4:
            raise LowerError(f"{fname}: more than 4 arguments")
        vals = []
        for a in args:
            v = self.eval(a, 64)
            if v.kind == "mem":
                v = self.load(v)
            vals.append(self.widen(v, 64) if v.kind == "sel" else v)
        for i, v in enumerate(vals):
            (s,), imm = self.srcs(v, w=64)
            self.E("MOV", 64, dst=f"A{i}", a=s, imm=imm)
        self.E("UCALL", imm=f"SUB:{fname}")
        return None

    def inline(self, fname, args, want):
        """Library functions (Push, Pop, ...) are pseudocode too, inlined in
        a scope of their own: their locals don't see or clobber the caller's,
        and their temporaries are freed when they return (all but RESULT)."""
        params, body = self.lib[fname]
        argv = []
        for p, a in zip(params, args):
            v = self.eval(a, self.f.osz)
            if v.kind == "mem":
                v = self.load(v)
            if v.kind == "sel" and v.sel in U_ARCH:
                t = self.tmp(keep=True)
                self.E("MOV", opsize(v.w), dst=t, a=v.sel)
                v = V("sel", v.w, sel=t)
            elif v.kind == "sel" and v.sel in self.stmt_temps:
                self.stmt_temps.remove(v.sel)       # the argument lives through the call
            argv.append((p, v))
        caller = self.vars
        saved_prot = set(self.protected)
        for v in caller.values():
            for t in ([v.hi, v.lo] if v.kind == "pair" else [v]):
                if t is not None and t.kind == "sel":
                    self.protected.add(t.sel)
        # parameters are passed by value: a parameter the body assigns gets
        # its own copy when the argument is a caller's variable (or one of
        # its enclosing callers'), which would otherwise be overwritten
        written = assigned_names(body)
        for i, (p, v) in enumerate(argv):
            if p in written and v.kind == "sel" and v.sel in self.protected:
                t = self.tmp(keep=True)
                self.E("MOV", opsize(v.w), dst=t, a=v.sel)
                argv[i] = (p, V("sel", v.w, sel=t))
        saved_live = (self.pos, self.last, getattr(self, "_inlining", 0))
        self.vars = dict(argv)
        if fname not in self.lib_live:
            self.liveness(body)
            self.lib_live[fname] = (self.pos, self.last)
        self.pos, self.last = self.lib_live[fname]
        self._inlining = 0
        try:
            self.block(body)
        finally:
            res = self.vars.get("RESULT")
            keep = set()
            if res is not None:
                keep = {res.sel} if res.kind == "sel" else ({res.hi.sel, res.lo.sel}
                                                            if res.kind == "pair" else set())
            # the caller's variables (an argument can be one of them) stay
            for v in caller.values():
                for t in ([v.hi, v.lo] if v.kind == "pair" else [v]):
                    if t is not None and t.kind == "sel":
                        keep.add(t.sel)
            for n, v in self.vars.items():
                for t in ([v.hi, v.lo] if v.kind == "pair" else [v]):
                    if t is not None and t.kind == "sel" and t.sel.startswith("T") \
                            and t.sel not in keep and t.sel not in self.free:
                        self.free.append(t.sel)
            self.vars = caller
            self.protected = saved_prot
            self.pos, self.last, self._inlining = saved_live
        if res is not None and res.kind == "sel" and res.sel.startswith("T"):
            self.stmt_temps.append(res.sel)         # freed with the caller's statement
        return res

    def _old_inline(self, fname, args, want):
        params, body = self.lib[fname]
        saved = {}
        for p, a in zip(params, args):
            saved[p] = self.vars.get(p)
            v = self.eval(a, self.f.osz)
            if v.kind == "mem":
                v = self.load(v)
            if v.kind == "sel" and v.sel in U_ARCH:
                # arguments are values: snapshot a register (PUSH RSP pushes
                # the value RSP had before the instruction)
                t = self.tmp()
                self.E("MOV", opsize(v.w), dst=t, a=v.sel)
                v = V("sel", v.w, sel=t)
            self.vars[p] = v
        saved["RESULT"] = self.vars.get("RESULT")
        self.vars.pop("RESULT", None)
        self._inlining = getattr(self, "_inlining", 0) + 1
        self.block(body)
        self._inlining -= 1
        res = self.vars.get("RESULT")
        for p, v in saved.items():
            if v is None:
                self.vars.pop(p, None)
            else:
                self.vars[p] = v
        return res

    def slice(self, e):
        v = self.eval(e.e)
        hi = self.eval(e.hi)
        lo = self.eval(e.lo)
        if hi.kind != "k" or lo.kind != "k":
            raise LowerError(f"dynamic bit field {e}")
        h, l = max(hi.k, lo.k), min(hi.k, lo.k)
        width = h - l + 1
        if v.kind == "pair":
            if l == 0 and width <= v.lo.w:
                return V("sel", width, sel=v.lo.sel) if width == v.lo.w else self._slice(v.lo, l, width)
            if l == v.lo.w and width == v.hi.w:
                return v.hi
            if l == 0 and width == v.w:
                return v
            raise LowerError(f"slice {e} of a pair")
        if v.kind == "mem":
            v = self.load(v)
        if l == 0 and width in (8, 16, 32, 64) and v.kind == "sel":
            return V("sel", width, sel=v.sel)
        return self._slice(v, l, width)

    def _slice(self, v, l, width):
        t = self.tmp()
        (s,), imm = self.srcs(v, w=opsize(v.w))
        self.E("SLICE", 64, dst=t, a=s, imm=l | (width << 8))
        return V("sel", width, sel=t)

    # ------------------------------------------------------------ conditions

    def static_cond(self, e):
        """True/False if the condition is known at compile time, else None
        (or ('CC', cc) from the oracle)."""
        if isinstance(e, Prose):
            r = oracle.condition(e.text, self.f)
            if r is None:
                # prose with expression parts: substitute known phrases
                r = self.prose_expr(e.text)
                if r is None:
                    raise LowerError(f"prose condition '{e.text}'")
            return r
        if isinstance(e, Name):
            if e.n == "condition":
                return ("CC", self.f.cc)
            st = self.static(e.n)
            if st is not None:
                return bool(st)
            if self.rname(e.n) is not None:
                return None
            r = oracle.condition(e.n, self.f)
            if r is not None:
                return r
            return None
        if isinstance(e, Bin):
            if e.op in ("AND", "OR"):
                a = self.static_cond(e.a)
                b = self.static_cond(e.b)
                if isinstance(a, str) or isinstance(b, str):
                    return None if not (a is False or b is False or a is True or b is True) \
                        else self._mix(e.op, a, b)
                if e.op == "AND":
                    if a is False or b is False:
                        return False
                    if a is True:
                        return b
                    if b is True:
                        return a
                else:
                    if a is True or b is True:
                        return True
                    if a is False:
                        return b
                    if b is False:
                        return a
                return None
            if e.op in self.CMPS:
                a = self.static_val(e.a)
                b = self.static_val(e.b)
                if a is not None and b is not None:
                    return {"=": a == b, "!=": a != b, "<": a < b, ">": a > b,
                            "<=": a <= b, ">=": a >= b}[e.op]
        if isinstance(e, Un) and e.op == "NOT":
            a = self.static_cond(e.a)
            if isinstance(a, bool):
                return not a
        if isinstance(e, Call) and e.f in ("ShadowStackEnabled", "EndbranchEnabled"):
            return False        # CET: not in v1 yet (CPUID doesn't report it)
        return None

    @staticmethod
    def _mix(op, a, b):
        """AND/OR of a static truth value and an oracle pseudocode string."""
        if op == "AND":
            if a is False or b is False:
                return False
            return a if b is True else b
        if a is True or b is True:
            return True
        return a if b is False else b

    def prose_expr(self, text):
        t = oracle.norm(text)
        for pat, fn in oracle.CONDITIONS:
            if pat.startswith("("):
                continue
        # 'X and Y' where X, Y are known phrases or expressions
        for sep in (" and ", " or "):
            if sep in t:
                parts = t.split(sep)
                vals = []
                for p in parts:
                    r = oracle.condition(p, self.f)
                    if r is None:
                        try:
                            r = self.static_cond(sdm.parse_expr(sdm.lex(p.upper())[:-2]))
                        except Exception:
                            r = None
                    if r is None:
                        return None
                    vals.append(r)
                if all(isinstance(v, bool) for v in vals):
                    return all(vals) if sep == " and " else any(vals)
        return None

    def static_val(self, e):
        if isinstance(e, Num):
            return e.v
        if isinstance(e, Name):
            if e.n in self.vars and self.vars[e.n].kind == "k":
                return self.vars[e.n].k        # a constant argument of an inlined function
            return self.static(e.n)
        if isinstance(e, Bin) and e.op in ("+", "-", "*", "/"):
            a, b = self.static_val(e.a), self.static_val(e.b)
            if a is not None and b is not None:
                return {"+": a + b, "-": a - b, "*": a * b, "/": a // b if b else None}[e.op]
        return None

    def branch(self, e, target, when):
        """Jump to target if condition e evaluates to `when`."""
        s = self.static_cond(e)
        if isinstance(s, str):
            # the oracle rewrote prose into pseudocode
            self.branch(sdm.parse_expr(sdm.lex(s)[:-2]), target, when)
            return
        if s is True or s is False:
            if s == when:
                self.E("JMP", imm=target)
            return
        if isinstance(s, tuple) and s[0] == "CC":
            cc = s[1] if when else s[1] ^ 1
            self.E("JCC", cc=cc, imm=target)
            return
        if isinstance(e, Bin) and e.op in ("AND", "OR"):
            if (e.op == "AND") != when:
                # AND, jump-if-false (or OR, jump-if-true): either side decides
                self.branch(e.a, target, when)
                self.branch(e.b, target, when)
            else:
                skip = self.r.label()
                self.branch(e.a, skip, not when)
                self.branch(e.b, target, when)
                self.r.place(skip)
            return
        if isinstance(e, Un) and e.op == "NOT":
            self.branch(e.a, target, not when)
            return
        if isinstance(e, Call) and e.f in ("SignedLess", "SignedGreater"):
            a = self.eval(e.args[0])
            b = self.eval(e.args[1])
            fixed = [v.w for v in (a, b) if not v.adapt and v.kind != "k"]
            w = opsize(max(fixed) if fixed else 64)
            (sa, sb), imm = self.srcs(a, b, w=w)
            self.E("CMP", w, a=sa, b=sb, imm=imm)
            c = "LTS" if e.f == "SignedLess" else "GTS"
            self.E("JU", cc=self.ucc(c if when else UCC_NEG[c]), imm=target)
            return
        if isinstance(e, Bin) and e.op in self.CMPS:
            a = self.eval(e.a)
            b = self.eval(e.b)
            fixed = [v.w for v in (a, b) if not v.adapt and v.kind != "k"]
            w = opsize(max(fixed) if fixed else 64)
            if a.kind == "pair" or b.kind == "pair":
                raise LowerError(f"compare of a pair: {e}")
            if a.kind == "k" and b.kind == "k":
                # two constants (an inlined function's constant argument): fold
                m = (1 << w) - 1
                x, y = a.k & m, b.k & m
                r = {"=": x == y, "!=": x != y, "<": x < y, ">": x > y, "<=": x <= y,
                     ">=": x >= y}.get(e.op)
                if r is not None:
                    if r == when:
                        self.E("JMP", imm=target)
                    return
            (sa, sb), imm = self.srcs(a, b, w=w)
            self.E("CMP", w, a=sa, b=sb, imm=imm)
            c = self.CMPS[e.op]
            self.E("JU", cc=self.ucc(c if when else UCC_NEG[c]), imm=target)
            self.release_stmt_partial()
            return
        # a value: true if non-zero
        v = self.eval(e)
        if v.kind == "k":
            if bool(v.k) == when:
                self.E("JMP", imm=target)
            return
        w = opsize(v.w if not v.adapt else 64)
        (sa,), imm = self.srcs(v, w=w)
        self.E("CMP", w, a=sa, b="ZERO", imm=imm)
        self.E("JU", cc=self.ucc("NE" if when else "EQ"), imm=target)

    def release_stmt_partial(self):
        pass

    @staticmethod
    def ucc(name):
        from uop import UCC
        return UCC[name]

    # ------------------------------------------------------------ assignment

    def assign(self, lhs, rhs_e, node=None):
        # flags
        if isinstance(lhs, Prose) and lhs.text in FMASK:
            lhs = Name(lhs.text)
        if isinstance(lhs, Name) and lhs.n in FLAG:
            if isinstance(rhs_e, Call) and rhs_e.f == "BitwiseXNOR" and lhs.n == "PF":
                v = self.eval(rhs_e.args[0])
                (s,), imm = self.srcs(v, w=8)
                self.E("SZP", 8, a=s, imm=imm, fmask=FMASK["PF"])
                return
            v = self.eval(rhs_e, 1)
            if v.kind == "k":
                self.E("SETF", 8, a="ONE" if v.k & 1 else "ZERO", imm=FLAG[lhs.n],
                       note=f"{lhs.n} := {v.k & 1}")
                return
            (s,), imm = self.srcs(v, w=8)
            self.E("SETF", 8, a=s, imm=FLAG[lhs.n])
            return
        if isinstance(lhs, Call) and lhs.f in ("RFLAGS", "EFLAGS"):
            raise LowerError("flag-list assignment (override it)")
        want = self.lvalue_width(lhs)
        v = self.eval(rhs_e, want)
        self.store(lhs, v)

    def lvalue_width(self, lhs):
        if isinstance(lhs, Name):
            if lhs.n in self.vars:
                return self.vars[lhs.n].w
            r = self.rname(lhs.n)
            if r is not None and r.kind != "special":
                return r.w
            if r is not None:
                return 64
            return None
        if isinstance(lhs, Call) and lhs.f == "Concat":
            a = self.lvalue_width(lhs.args[0])
            b = self.lvalue_width(lhs.args[1])
            return (a or 0) + (b or 0)
        if isinstance(lhs, Slice):
            hi, lo = self.static_val(lhs.hi), self.static_val(lhs.lo)
            if hi is not None and lo is not None:
                return abs(hi - lo) + 1
        if isinstance(lhs, (Index, Seg)):
            return self.memref(lhs, None).w
        return None

    def store(self, lhs, v):
        if isinstance(lhs, Name):
            n = lhs.n
            if n in self.vars or (self.rname(n) is None and self.static(n) is None and n not in FLAG):
                self.store_var(n, v)
                return
            loc = self.rname(n)
            if loc.kind == "flag":
                (s,), imm = self.srcs(self.widen(v, 8) if v.kind == "sel" else v, w=8)
                if s == "K":
                    s = "ONE" if imm & 1 else "ZERO"
                self.E("SETF", 8, a=s, imm=FLAG[loc.sel])
                return
            if loc.kind == "segfield":
                self.store_segfield(n, loc, v)
                return
            if loc.kind == "special":
                v = self.widen(v, 64) if v.kind == "sel" else v
                (s,), imm = self.srcs(v, w=64)
                if s == "K":
                    if imm == 0:
                        s = "ZERO"
                    else:
                        k = self.tmp()
                        self.E("MOV", 64, dst=k, a="K", imm=imm)
                        s = k
                self.E("WRS", 64, a=s, imm=SPECIAL[loc.sel])
                return
            if loc.kind == "mem":
                self.store_mem(loc, v)
                self.mem_cache[n] = v if v.kind == "sel" else None
                return
            self.store_loc(loc, v)
            return
        if isinstance(lhs, Call) and lhs.f == "Concat":
            hi = self.eval(lhs.args[0])
            lo = self.eval(lhs.args[1])
            if v.kind != "pair":
                raise LowerError(f"pair assignment from {v}")
            self.store_any(lhs.args[1], lo, v.lo)
            self.store_any(lhs.args[0], hi, v.hi)
            return
        if isinstance(lhs, Slice):
            self.store_slice(lhs, v)
            return
        if isinstance(lhs, (Index, Seg)):
            loc = self.memref(lhs, None)
            self.store_mem(loc, v)
            return
        raise LowerError(f"can't assign to {lhs}")

    def store_segfield(self, n, loc, v):
        """CS.Type := 11 and the like: a bit field of the descriptor cache's
        attributes. Setting G scales the limit written before it, as a
        descriptor's limit is in 4-KByte units when G = 1."""
        t = self.tmp()
        self.E("RDS", 64, dst=t, imm=SPECIAL[loc.sel])
        (s,), imm = self.srcs(self.widen(v, 64) if v.kind != "k" else v, w=64)
        if s == "K":
            if imm == 0:
                s = "ZERO"
            else:
                k = self.tmp()
                self.E("MOV", 64, dst=k, a="K", imm=imm)
                s = k
        self.E("INSERT", 64, dst=t, a=s, imm=loc.k | (loc.w << 8))
        self.E("WRS", 64, a=t, imm=SPECIAL[loc.sel])
        if n.endswith(".G"):
            seg = loc.sel[:-5]
            lim = self.tmp()
            skip = self.r.label()
            self.E("CMP", 64, a=s, b="ZERO")
            self.E("JU", cc=self.ucc("EQ"), imm=skip)
            self.E("RDS", 64, dst=lim, imm=SPECIAL[f"{seg}_LIMIT"])
            self.E("SHL", 64, dst=lim, a=lim, b="K", imm=12)
            self.E("OR", 64, dst=lim, a=lim, b="K", imm=0xFFF)
            self.E("WRS", 64, a=lim, imm=SPECIAL[f"{seg}_LIMIT"])
            self.r.place(skip)

    def store_any(self, lhs_e, loc, v):
        if loc.kind == "mem":
            self.store_mem(loc, v)
        else:
            self.store_loc(loc, v)

    def store_var(self, n, v):
        old = self.vars.get(n)
        if v.kind == "pair":
            hi = self.tmp(keep=True)
            lo = self.tmp(keep=True)
            self.E("MOV", v.hi.w, dst=hi, a=self.to_sel(v.hi))
            self.E("MOV", v.lo.w, dst=lo, a=self.to_sel(v.lo))
            self.vars[n] = V("pair", v.w, hi=V("sel", v.hi.w, sel=hi), lo=V("sel", v.lo.w, sel=lo))
            return
        if v.kind == "mem":
            v = self.load(v)
        w = v.w if not v.adapt else 64
        if old is not None and old.kind == "sel" and old.sel.startswith("T"):
            t = old.sel
            w = max(w, old.w)
        else:
            t = self.tmp(keep=True)
        (s,), imm = self.srcs(v, w=opsize(w))
        if s != t:
            self.retarget_or_move(t, s, imm, opsize(w))
        self.vars[n] = V("sel", w, sel=t)

    def retarget_or_move(self, dst, s, imm, w):
        """Write the last uop's result to dst directly when it produced s."""
        last = self.r.code[-1] if self.r.code else None
        if last and "op" in last and last.get("dst") == s and s.startswith("T") \
                and last["op"] not in ("MOV",) and s in self.stmt_temps and last["sz"] == w:
            last["dst"] = dst
            return
        self.E("MOV", w, dst=dst, a=s, imm=imm)

    def store_loc(self, loc, v):
        if v.kind == "pair":
            # a double-width product into one register (AX := AL * SRC)
            if loc.w == v.w and v.lo.w == loc.w // 2:
                t = self.tmp()
                self.E("MOV", v.hi.w, dst=t, a=v.hi.sel)
                self.E("SHL", loc.w, dst=t, a=t, b="K", imm=v.lo.w)
                lo = self.widen(v.lo, loc.w)
                self.E("OR", loc.w, dst=loc.sel, a=t, b=lo.sel)
                return
            v = v.lo
        if loc.sel == "NRIP" and loc.w < 64:
            loc = V("sel", 64, sel="NRIP")
        w = opsize(loc.w)
        (s,), imm = self.srcs(v, w=w)
        self.retarget_or_move(loc.sel, s, imm, w)

    def store_mem(self, loc, v):
        if v.kind == "pair":
            v = v.lo
        a, disp = self.addr_of(loc.addr)
        (s,), imm = self.srcs(v, w=opsize(loc.w))
        if s == "K":
            t = self.tmp()
            self.E("MOV", opsize(loc.w), dst=t, a="K", imm=imm)
            s = t
        self.E("ST", loc.w, a=a, b=s, imm=disp, seg=SEGS[loc.seg])

    def store_slice(self, lhs, v):
        hi, lo = self.static_val(lhs.hi), self.static_val(lhs.lo)
        if hi is None or lo is None:
            raise LowerError("dynamic bit-field assignment")
        h, l = max(hi, lo), min(hi, lo)
        width = h - l + 1
        base = lhs.e
        if not isinstance(base, Name):
            raise LowerError(f"bit-field of {base}")
        loc = self.rname(base.n) if base.n not in self.vars else self.vars[base.n]
        if loc is None:
            raise LowerError(f"bit-field of unknown {base.n}")
        if loc.kind == "mem":
            cur = self.load(loc)
        else:
            cur = loc
        t = self.tmp()
        self.E("MOV", opsize(cur.w), dst=t, a=self.to_sel(cur))
        (s,), imm = self.srcs(v, w=64)
        if s == "K":
            # INSERT's imm is the field: a constant source goes through a temp
            if imm == 0:
                s = "ZERO"
            else:
                k = self.tmp()
                self.E("MOV", 64, dst=k, a="K", imm=imm)
                s = k
        self.E("INSERT", 64, dst=t, a=s, imm=l | (width << 8))
        tv = V("sel", cur.w, sel=t)
        if loc.kind == "mem":
            self.store_mem(loc, tv)
        elif base.n in self.vars:
            self.vars[base.n] = V("sel", loc.w, sel=t)
            self.stmt_temps.remove(t)
        else:
            self.store_loc(loc, tv)

    # ------------------------------------------------------------ statements

    def block(self, body):
        for s in body:
            self.stmt(s)
            self.free_dead(s)

    def stmt(self, s):
        if isinstance(s, Assign):
            self.cur_producer = s if id(s) in self.producer else None
            if isinstance(s.dst, Name) and s.dst.n == "RESULT":
                v = self.eval(s.src, self.f.osz)
                if v.kind == "mem":
                    v = self.load(v)
                    return
                self.vars["RESULT"] = v
                # keep the result's temp alive past this statement
                if v.kind == "sel" and v.sel in self.stmt_temps:
                    self.stmt_temps.remove(v.sel)
                return
            self.assign(s.dst, s.src, s)
            self.cur_producer = None
            self.release_stmt()
            return
        if isinstance(s, If):
            st = self.static_cond(s.cond)
            if st is True:
                self.block(s.then)
                return
            if st is False:
                self.block(s.els or [])
                return
            lelse, lend = self.r.label(), self.r.label()
            self.branch(s.cond, lelse, False)
            self.release_stmt()
            self.block(s.then)
            if s.els:
                self.E("JMP", imm=lend)
            self.r.place(lelse)
            if s.els:
                self.block(s.els)
                self.r.place(lend)
            return
        if isinstance(s, While):
            ltop, lend = self.r.label(), self.r.label()
            self.r.place(ltop)
            st = self.static_cond(s.cond)
            if st is False:
                return
            if st is not True:
                self.branch(s.cond, lend, False)
                self.release_stmt()
            self.loop_ends.append(lend)
            self.block(s.body)
            self.loop_ends.pop()
            self.E("JMP", imm=ltop)
            self.r.place(lend)
            return
        if isinstance(s, Raise):
            vec = VECTORS.get(s.exc)
            if vec is None:
                raise LowerError(f"exception #{s.exc}")
            imm = vec
            a = "ZERO"
            if s.exc in ERRCODE:
                imm |= 1 << 8
                if s.code is not None and not isinstance(s.code, Prose):
                    v = self.eval(s.code, 32)
                    (a,), k = self.srcs(v, w=32)
                    if a == "K":
                        if k:
                            raise LowerError("constant non-zero error code")
                        a = "ZERO"
            if s.exc in ("BP", "OF") or (s.exc == "DB" and self.f.mnem == "INT1"):
                imm |= 1 << 9          # a trap: delivered with the next RIP
            self.E("EXC", 32, a=a, imm=imm, note=f"#{s.exc}")
            return
        if isinstance(s, Do):
            e = s.e
            if isinstance(e, Name) and e.n == "ModifyStatusFlags":
                return
            if isinstance(e, Call) and e.f == "ModifyStatusFlags":
                return
            if isinstance(e, Call) and e.f in self.lib.get("__subroutines__", ()):
                self.call_sub(e.f, e.args)
                self.release_stmt()
                return
            if isinstance(e, Call) and e.f in self.lib:
                self.inline(e.f, e.args, None)
                self.release_stmt()
                return
            if isinstance(e, Call) and e.f in self.SPECIAL_STMTS:
                getattr(self, self.SPECIAL_STMTS[e.f])(e)
                self.release_stmt()
                return
            if isinstance(e, Name) and e.n in ("SHUTDOWN", "HLT", "Halt", "Shutdown", "Commit",
                                               "COMMIT", "TrapReturn", "InhibitInterrupts",
                                               "Done", "CpuidRead"):
                if e.n == "Done":
                    # leave the routine: the rest of the Operation is skipped
                    self.E("JMP", imm=self.done_label)
                    return
                if e.n == "InhibitInterrupts":
                    self.E("WRS", 64, a="ONE", imm=SPECIAL["INT_SHADOW"])
                    return
                self.E({"SHUTDOWN": "SHUTDOWN", "Shutdown": "SHUTDOWN", "HLT": "HLT",
                        "Halt": "HLT", "Commit": "COMMIT", "COMMIT": "COMMIT",
                        "TrapReturn": "URET", "CpuidRead": "CPUID"}[e.n])
                return
            if isinstance(e, Call) and e.f == "BREAK":
                if not self.loop_ends:
                    raise LowerError("BREAK outside a loop")
                self.E("JMP", imm=self.loop_ends[-1])
                return
            text = e.text if isinstance(e, Prose) else str(e)
            what = oracle.statement(text) if isinstance(e, Prose) else \
                oracle.statement(f"{e.f}()") if isinstance(e, Call) else None
            if what == "skip":
                return
            if what == "HLT":
                self.E("HLT")
                return
            raise LowerError(f"statement '{text}'")
        if isinstance(s, For):
            lo, hi = self.static_val(s.lo), self.static_val(s.hi)
            if lo is None or hi is None:
                raise LowerError("FOR with run-time bounds")
            rng = range(hi, lo - 1, -1) if s.down else range(lo, hi + 1)
            for i in rng:
                self.vars[s.var] = K(i)
                self._static_vars = getattr(self, "_static_vars", {})
                self.block(s.body)
            self.vars.pop(s.var, None)
            return
        if isinstance(s, Case):
            raise LowerError("CASE (override it)")
        raise LowerError(f"statement {s}")

    SPECIAL_STMTS = {
        "Out": "s_out", "In": "s_in", "WriteSpecial": "s_wrs", "ReadSpecial": "s_rds",
        "SetFlag": "s_setflag", "StoreFlags": "s_storeflags", "Jump": "s_jump",
        "RaiseVector": "s_raise_vec", "TlbWrite": "s_tlbw", "TlbInvalidate": "s_tlbinv",
        "SetSegment": "s_setseg", "SetSegmentReal": "s_setseg_real",
        "ApicWrite": "s_apicwr",
    }

    SEGNAMES = ["ES", "CS", "SS", "DS", "FS", "GS", "LDTR", "TR"]

    def s_setseg(self, e):
        # SetSegment(n, selector, base, limit, attributes): load a descriptor cache
        n = self.static_val(e.args[0])
        if n is None:
            # a run-time segment number (LoadSegment as a subroutine): WRS with
            # the special register's number in b (x = 1)
            nv = self.widen(self.eval(e.args[0], 64), 64)
            ix = self.tmp()
            self.E("SHL", 64, dst=ix, a=self.to_sel(nv, 64), b="K", imm=2)
            self.E("ADD", 64, dst=ix, a=ix, b="K", imm=SPECIAL["ES_SEL"])
            for i, a in enumerate(e.args[1:]):
                v = self.widen(self.eval(a, 64), 64)
                sv = self.to_sel(v, 64)
                if isinstance(sv, tuple):
                    if sv[1] == 0:
                        sv = "ZERO"
                    else:
                        k = self.tmp()
                        self.E("MOV", 64, dst=k, a="K", imm=sv[1])
                        sv = k
                if i:
                    self.E("ADD", 64, dst=ix, a=ix, b="ONE")
                self.E("WRS", 64, a=sv, b=ix, x=1)
            return
        seg = self.SEGNAMES[n]
        for part, a in zip(("SEL", "BASE", "LIMIT", "ATTR"), e.args[1:]):
            v = self.widen(self.eval(a, 64), 64)
            s = self.to_sel(v, 64)
            if isinstance(s, tuple):
                if s[1] == 0:
                    s = "ZERO"
                else:
                    t = self.tmp()
                    self.E("MOV", 64, dst=t, a="K", imm=s[1])
                    s = t
            self.E("WRS", 64, a=s, imm=SPECIAL[f"{seg}_{part}"])
        if seg == "CS":
            # a CS load changes the decode mode: nothing else is fetched with the old one
            pass

    def s_setseg_real(self, e):
        # SetSegmentReal(n, selector): real-address / virtual-8086 mode, base = selector * 16
        n = self.static_val(e.args[0])
        if n is None:
            nv = self.widen(self.eval(e.args[0], 64), 64)
            ix = self.tmp()
            self.E("SHL", 64, dst=ix, a=self.to_sel(nv, 64), b="K", imm=2)
            self.E("ADD", 64, dst=ix, a=ix, b="K", imm=SPECIAL["ES_SEL"])
            v = self.widen(self.eval(e.args[1], 16), 64)
            sv = self.to_sel(v, 64)
            b = self.tmp()
            self.E("AND", 64, dst=b, a=sv if not isinstance(sv, tuple) else "K",
                   b="K" if not isinstance(sv, tuple) else "ZERO", imm=0xFFFF)
            self.E("WRS", 64, a=b, b=ix, x=1)
            self.E("SHL", 64, dst=b, a=b, b="K", imm=4)
            self.E("ADD", 64, dst=ix, a=ix, b="ONE")
            self.E("WRS", 64, a=b, b=ix, x=1)
            return
        seg = self.SEGNAMES[n]
        v = self.widen(self.eval(e.args[1], 16), 64)
        s = self.to_sel(v, 64)
        if isinstance(s, tuple):
            t = self.tmp()
            self.E("MOV", 64, dst=t, a="K", imm=s[1])
            s = t
        b = self.tmp()
        self.E("AND", 64, dst=b, a=s, b="K", imm=0xFFFF)
        self.E("WRS", 64, a=b, imm=SPECIAL[f"{seg}_SEL"])
        self.E("SHL", 64, dst=b, a=b, b="K", imm=4)
        self.E("WRS", 64, a=b, imm=SPECIAL[f"{seg}_BASE"])

    def s_tlbw(self, e):
        # TlbWrite(linear, image, size): install a translation (image: the page's
        # physical address with the combined W, U, D, G and NX bits as in a PTE)
        la = self.widen(self.eval(e.args[0], 64), 64)
        im = self.widen(self.eval(e.args[1], 64), 64)
        size = self.widen(self.eval(e.args[2], 64), 64)
        (sa, sb), _ = self.srcs(la, im, w=64)
        sc = self.to_sel(size, 64)
        if isinstance(sc, tuple):
            t = self.tmp()
            self.E("MOV", 64, dst=t, a="K", imm=sc[1])
            sc = t
        self.E("TLBW", 64, a=sa, b=sb, c=sc)

    def s_tlbinv(self, e):
        # TlbInvalidate(kind, address, pcid): kind 0 all, 1 non-global, 2 page,
        # 3 PCID, 4 page in PCID
        kind = self.static_val(e.args[0])
        a = self.widen(self.eval(e.args[1], 64), 64) if len(e.args) > 1 else K(0)
        b = self.widen(self.eval(e.args[2], 64), 64) if len(e.args) > 2 else K(0)
        sa = self.to_sel(a, 64)
        sb = self.to_sel(b, 64)
        if isinstance(sa, tuple):
            sa = "ZERO"
        if isinstance(sb, tuple):
            sb = "ZERO"
        self.E("TLBINV", 64, a=sa, b=sb, x=kind)

    def s_out(self, e):
        port = self.eval(e.args[0], 16)
        val = self.eval(e.args[1], self.f.osz)
        w = opsize(val.w if not val.adapt else self.f.osz)
        pw = self.widen(port, 64) if not port.adapt else port
        (sp,), ip = self.srcs(pw, w=64)
        if sp == "K":
            t = self.tmp()
            self.E("MOV", 64, dst=t, a="K", imm=ip)
            sp = t
        (sv,), iv = self.srcs(val, w=w)
        self.E("OUT", w, a=sp, b=sv, imm=iv)

    def s_in(self, e):
        # In(dest, port)
        port = self.eval(e.args[1], 16)
        loc = self.eval(e.args[0])
        pw = self.widen(port, 64) if not port.adapt else port
        (sp,), ip = self.srcs(pw, w=64)
        if sp == "K":
            t = self.tmp()
            self.E("MOV", 64, dst=t, a="K", imm=ip)
            sp = t
        self.E("IN", loc.w, dst=loc.sel, a=sp)

    def s_wrs(self, e):
        reg = e.args[0].n
        v = self.eval(e.args[1], 64)
        v = self.widen(v, 64)
        s = self.to_sel(v, 64)
        if isinstance(s, tuple):
            t = self.tmp()
            self.E("MOV", 64, dst=t, a="K", imm=s[1])
            s = t
        self.E("WRS", 64, a=s, imm=SPECIAL[reg])

    def s_rds(self, e):
        reg = e.args[1].n
        loc = self.eval(e.args[0])
        self.E("RDS", 64, dst=loc.sel, imm=SPECIAL[reg])

    def s_apicwr(self, e):
        # ApicWrite(msr, value): write the local APIC's MSR (#GP if illegal)
        ops = []
        for x in e.args[:2]:
            v = self.widen(self.eval(x, 64), 64)
            sv = self.to_sel(v, 64)
            if isinstance(sv, tuple):
                k = self.tmp()
                self.E("MOV", 64, dst=k, a="K", imm=sv[1])
                sv = k
            ops.append(sv)
        self.E("APIC", 64, a=ops[0], b=ops[1], x=1)

    def s_jump(self, e):
        v = self.eval(e.args[0], 64)
        (s,), imm = self.srcs(v, w=64)
        self.E("MOV", 64, dst="NRIP", a=s, imm=imm)

    def s_raise_vec(self, e):
        # RaiseVector(v): software interrupt INT n (a trap)
        v = self.eval(e.args[0], 8)
        (s,), imm = self.srcs(v, w=32)
        if s == "K":
            self.E("EXC", 32, a="ZERO", imm=(imm & 0xFF) | (1 << 9))
        else:
            self.E("EXC", 32, a=s, imm=(1 << 9) | (1 << 10))   # vector from a

    def s_setflag(self, e):
        name = {"INTF": "IF"}.get(e.args[0].n, e.args[0].n)
        v = self.eval(e.args[1], 1)
        (s,), imm = self.srcs(v, w=8)
        if s == "K":
            s = "ONE" if imm & 1 else "ZERO"
        self.E("SETF", 8, a=s, imm=FLAG[name])

    def s_storeflags(self, e):
        # StoreFlags(value, mask): RFLAGS := (RFLAGS AND NOT mask) OR (value AND mask)
        v = self.eval(e.args[0], 64)
        m = self.eval(e.args[1], 64)
        v = self.widen(v, 64)
        (s,), kv = self.srcs(v, w=64)
        if s == "K":
            # a constant value: one immediate per micro-op, so it can't share
            # the AND with a constant mask (StoreFlags(2, mask) in SMI entry)
            s = self.tmp()
            self.E("MOV", 64, dst=s, a="K", imm=kv)
        t = self.tmp()
        f = self.tmp()
        self.E("MOV", 64, dst=f, a="FLAGS")
        if m.kind == "k":
            self.E("AND", 64, dst=t, a=s, b="K", imm=self.k32(m.k))
            self.E("AND", 64, dst=f, a=f, b="K", imm=self.k32(~m.k))
        else:
            m = self.widen(m, 64)
            ms = self.to_sel(m, 64)
            self.E("AND", 64, dst=t, a=s, b=ms)
            self.E("ANDN", 64, dst=f, a=ms, b=f)       # FLAGS AND NOT mask
        self.E("OR", 64, dst=f, a=f, b=t)
        self.E("MOV", 64, dst="FLAGS", a=f)

    @staticmethod
    def k32(v):
        v &= 0xFFFFFFFF
        return v - (1 << 32) if v >> 31 else v

    # ------------------------------------------------------------ driver

    def lower(self):
        self.cur_producer = None
        self.done_label = self.r.label()
        sub = getattr(self.f, "subroutine", None)
        if sub is not None:
            # parameters arrive in A0-A3: copy them (a nested call reuses A0-A3)
            for i, p in enumerate(sub):
                t = self.tmp(keep=True)
                self.E("MOV", 64, dst=t, a=f"A{i}")
                self.vars[p] = V("sel", 64, sel=t)
            self.block(self.ast)
            self.r.place(self.done_label)
            self.E("URET")
            return self.r
        self.block(self.ast)
        self.r.place(self.done_label)
        # prose 'cleared'/'set' flags nobody produced or assigned
        for fl, act in self.flags.items():
            if fl in self.assigned_flags or self.native_flags & FMASK[fl]:
                continue
            if (act == "clear" and not self.logic_producer) or act == "set":
                self.E("SETF", 8, a="ONE" if act == "set" else "ZERO", imm=FLAG[fl])
        if not getattr(self.f, "trap", False):
            self.E("END")
        return self.r
