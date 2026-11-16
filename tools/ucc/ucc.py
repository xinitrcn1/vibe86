#!/usr/bin/env python3
"""ucc: compile the Intel SDM's pseudocode into microcode for a vibe86 core.

    ucc.py [--target vN] --sdm tools/sdm/extracted --out vN_ucode.svh
           [--listing vN.lst] [--report vN-report.md] [--pkg vN_uop_pkg.sv]

A *target* is one core version's microcode: targets/<vN>/ holds
  * uop.py      the micro-op ISA of that core (format, selectors, ops),
  * isa         which SDM pages (and which rows of them) the core implements,
  * overrides/  normalized replacements for sections of the manual the
                compiler can't read as written (each says why),
  * lib.sdm     the manual's helper functions (Push, Pop, ...) as pseudocode,
  * routines/   microcode with no instruction page (exception delivery).
The compiler itself (sdm.py, forms.py, oracle.py, lower.py) is shared.
--target defaults to the latest version in targets/.

Outputs: the microcode ROM, the decoder's tables and the entry-point map as
a SystemVerilog include, a listing, and a report of what was compiled from
the manual verbatim and what needed an override.
"""

import argparse
import os
import re
import shlex
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
TARGETS = os.path.join(HERE, "targets")


def versions():
    vs = [d for d in os.listdir(TARGETS) if re.fullmatch(r"v\d+", d)]
    return sorted(vs, key=lambda v: int(v[1:]))


def pick_target(argv):
    """The target (core version) from --target, else the latest; its
    directory goes first on the import path so `import uop` gets its ISA."""
    t = None
    for i, a in enumerate(argv):
        if a == "--target" and i + 1 < len(argv):
            t = argv[i + 1]
        elif a.startswith("--target="):
            t = a.split("=", 1)[1]
    t = t or versions()[-1]
    if not os.path.isdir(os.path.join(TARGETS, t)):
        sys.exit(f"ucc: no target '{t}' (have: {' '.join(versions())})")
    return t


TARGET = pick_target(sys.argv)
TDIR = os.path.join(TARGETS, TARGET)
sys.path.insert(0, HERE)
sys.path.insert(0, TDIR)

import forms as F       # noqa: E402
import lower as L       # noqa: E402
import oracle           # noqa: E402
import sdm              # noqa: E402
import uop as U         # noqa: E402


def read_isa(path):
    pages = []
    with open(path) as f:
        for n, line in enumerate(f, 1):
            line = line.split("#", 1)[0].strip()
            if not line:
                continue
            parts = shlex.split(line)
            cfg = {}
            for p in parts[1:]:
                k, _, v = p.partition("=")
                if k in ("osz",):
                    cfg[k] = {a: int(b) for a, b in (x.split(":") for x in v.split(","))}
                elif k in ("rep", "suffixed"):
                    cfg[k] = v.split(",")
                elif k in ("memonly", "immz", "cc"):
                    cfg[k] = True
                else:
                    cfg[k] = v
            pages.append((parts[0], cfg))
    return pages


def read_library(path):
    """lib.sdm: '@function Name(P1, P2)' then its pseudocode."""
    lib = {}
    cur, params, lines = None, [], []
    with open(path) as f:
        for line in f:
            m = re.match(r"^@function\s+(\w+)\((.*)\)\s*$", line)
            if m:
                if cur:
                    lib[cur] = (params, sdm.parse("".join(lines)))
                cur = m.group(1)
                params = [p.strip() for p in m.group(2).split(",") if p.strip()]
                lines = []
                continue
            if line.startswith("#"):
                continue
            lines.append(line)
    if cur:
        lib[cur] = (params, sdm.parse("".join(lines)))
    return lib


class Image:
    def __init__(self):
        self.routines = []      # (name, Routine)
        self.addr = {}          # label/routine -> uaddr
        self.words = []
        self.listing = []

    def add(self, name, r):
        if any(n == name for n, _ in self.routines):
            raise ValueError(f"two routines named '{name}'")
        self.routines.append((name, r))

    def assemble(self):
        pc = 0
        for name, r in self.routines:
            self.addr[name] = pc
            for c in r.code:
                if "label" in c:
                    self.addr[c["label"]] = pc
                else:
                    pc += 1
        for name, r in self.routines:
            self.listing.append(f"\n{self.addr[name]:5d}  {name}:")
            for c in r.code:
                if "label" in c:
                    self.listing.append(f"       {c['label'].split('#')[1]}:")
                    continue
                c = dict(c)
                if c["op"] in ("JMP", "JCC", "JU") and isinstance(c["imm"], str):
                    c["imm"] = self.addr[c["imm"]]
                sel = lambda s: U.SEL[s]        # noqa: E731
                w = U.encode(c["op"], c["sz"], sel(c["dst"]), sel(c["a"]), sel(c["b"]),
                             c["fmask"], c["cc"], c["x"], c["imm"])
                self.listing.append(f"{len(self.words):5d}  {fmt(c)}")
                self.words.append(w)
        return self.words


def fmt(c):
    s = f"{c['op']:<8} sz{c['sz']:<3}"
    parts = []
    if c["dst"] != "ZERO" or c["op"] in ("MOV",):
        parts.append(f"{c['dst']}")
    for k in ("a", "b"):
        if c[k] != "ZERO" or c["op"] in ("CMP",):
            parts.append(c[k])
    s += ", ".join(parts)
    if c["imm"]:
        s += f"  imm={c['imm']:#x}" if isinstance(c["imm"], int) else f"  ->{c['imm']}"
    if c["fmask"]:
        s += "  flags=" + "".join(f[0] for f in U.STATUS if c["fmask"] & U.FMASK[f])
    if c["op"] in ("JCC", "SETCC"):
        s += f"  cc={c['cc']}"
    if c["op"] == "JU":
        s += "  u" + {v: k for k, v in U.UCC.items()}[c["cc"]]
    if c.get("note"):
        s += f"  ; {c['note']}"
    return s


# ---------------------------------------------------------------- decoder tables

def imm_kind(fs):
    """Immediate length class of one (map, opc, digit) from its rows."""
    by = {}
    for f in fs:
        by.setdefault(f.okey, set()).add(sum(f.imm))
    lens = set().union(*by.values())
    if lens <= {0}:
        return 0
    if len(lens) == 1:
        n = lens.pop()
        return {1: 1, 2: 2, 3: 3, 4: 4, 8: 7}[n]
    d = {k: max(v) for k, v in by.items()}
    if d.get("16") == 2 and d.get("32") == 4 and d.get("64") == 8:
        return 6    # iw / id / io (MOV r64, imm64)
    if d.get("16", 2) == 2 and d.get("32", 4) == 4 and d.get("64", 4) == 4:
        return 5    # iw / id (iz)
    raise ValueError(f"immediate sizes {d} for {fs[0].name}")


def sv_tables(forms, image, cfg_of):
    out = []
    groups = {}
    for f in forms:
        groups.setdefault((f.map, f.opc), []).append(f)
    out.append("// decoder attributes per (map, opcode[, ModRM.reg])")
    out.append("function automatic dattr_t dec_attr(input logic [1:0] map, "
               "input logic [7:0] opc, input logic [2:0] digit);")
    out.append("  dattr_t d;")
    out.append("  d = '0;")
    out.append("  casez ({map, opc, digit})")
    for (m, o), fs in sorted(groups.items()):
        digits = {f.digit for f in fs}
        per = {}
        if None in digits or len(digits) == 1:
            per[None] = fs
        else:
            for f in fs:
                per.setdefault(f.digit, []).append(f)
        for dg, gfs in sorted(per.items(), key=lambda x: -1 if x[0] is None else x[0]):
            modrm = any(f.modrm for f in gfs)
            ik = imm_kind(gfs)
            cls = {"d32": 0, "d64": 1, "f64": 2}[gfs[0].vclass]
            sens = any(f.prefix_key() for f in fs)
            immz = any(cfg_of(f).get("immz") for f in gfs)
            plus_r = any(f.plus_r for f in gfs)
            dbits = "???" if dg is None else format(dg, "03b")
            out.append(f"    13'b{m:02b}_{o:08b}_{dbits}: d = '{{valid: 1'b1, modrm: 1'b{int(modrm)}, "
                       f"imm: 3'd{ik}, immz: 1'b{int(immz)}, cls: 2'd{cls}, pfx_sens: 1'b{int(sens)}, "
                       f"plus_r: 1'b{int(plus_r)}}};")
    out.append("    default: d = '0;")
    out.append("  endcase")
    out.append("  return d;")
    out.append("endfunction")
    out.append("")
    out.append("// microcode entry point of an instruction form")
    out.append("function automatic logic [V1_UADDR_W-1:0] uentry(input logic [1:0] map, "
               "input logic [7:0] opc, input logic [2:0] digit, input logic [1:0] pfx, "
               "input logic [1:0] osz, input logic mem, input logic sub90);")
    out.append("  casez ({sub90, map, opc, digit, pfx, osz, mem})")
    order = sorted(forms, key=lambda f: (f.map, f.opc, f.digit is None, f.digit or 0,
                                         f.prefix_key() is None, f.okey == "any"))
    for f in order:
        sub = "1" if f.hints.get("sub90") else ("0" if (f.map, f.opc) == (0, 0x90) else "?")
        dg = "???" if f.digit is None else format(f.digit, "03b")
        pk = {None: "00", "F3": "01", "F2": "10", "66": "11"}[f.prefix_key()]
        oz = {"any": "??", "16": "01", "32": "10", "64": "11"}[f.okey]
        mm = "?" if f.mem is None else str(int(f.mem))
        out.append(f"    18'b{sub}_{f.map:02b}_{f.opc:08b}_{dg}_{pk}_{oz}_{mm}: "
                   f"return {U.UADDR_W}'d{image.addr[f.name]};  // {f.name}")
    out.append(f"    default: return {U.UADDR_W}'d{image.addr['UD']};")
    out.append("  endcase")
    out.append("endfunction")
    return out




def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[1])
    ap.add_argument("--target", default=TARGET,
                    help=f"core version (default: the latest, {versions()[-1]})")
    ap.add_argument("--sdm", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--listing")
    ap.add_argument("--report")
    ap.add_argument("--pkg")
    ap.add_argument("--keep-going", action="store_true",
                    help="leave forms that don't compile out (they decode as #UD)")
    ap.add_argument("-v", action="store_true")
    args = ap.parse_args()

    lib = read_library(os.path.join(TDIR, "lib.sdm"))
    pages = read_isa(os.path.join(TDIR, "isa"))
    all_forms, errors, report = [], [], []
    asts, pageobj = {}, {}
    for stem, cfg in pages:
        path = os.path.join(args.sdm, stem + ".sdm")
        ovp = os.path.join(TDIR, "overrides", stem + ".sdm")
        if not os.path.exists(path):
            if os.path.exists(ovp):
                path = ovp
            else:
                errors.append(f"{stem}: no such page")
                continue
        page = sdm.read_sdm(path)
        if os.path.exists(ovp) and path != ovp:
            sdm.apply_override(page, sdm.read_sdm(ovp))
        try:
            ast = sdm.parse(page.operation)
        except sdm.ParseError as e:
            errors.append(f"{stem}: Operation doesn't parse: {e}")
            continue
        fs, errs = F.make_forms(page, cfg)
        errors += errs
        asts[page.name] = ast
        pageobj[page.name] = (page, stem)
        all_forms += fs
    cfg_of = lambda f: f.cfg   # noqa: E731
    F.assign_classes(all_forms, cfg_of)
    spec = F.specialize(all_forms, cfg_of)
    spec, dups = F.dedupe(spec)

    image = Image()
    # 0: invalid opcode
    ud = L.Routine("UD")
    ud.emit("EXC", 32, imm=6, note="#UD")
    image.add("UD", ud)
    # vibe86 routines (exception delivery, ...)
    for fn in sorted(os.listdir(os.path.join(TDIR, "routines"))):
        if not fn.endswith(".sdm"):
            continue
        page = sdm.read_sdm(os.path.join(TDIR, "routines", fn))
        ast = sdm.parse(page.operation)
        f = F.Form(page=page.name, mnem=page.name, text=page.name, opcol="", open="",
                   osz=64)
        lw = L.Lowerer(f, page, ast, oracle.flags_spec("None."), lib, name=page.name)
        try:
            image.add(page.name, lw.lower())
        except L.LowerError as e:
            errors.append(f"{page.name}: {e}")
    compiled = []
    per_page = {}
    for f in spec:
        page, stem = pageobj[f.page]
        lw = L.Lowerer(f, page, asts[f.page], oracle.flags_spec(page.flags), lib)
        try:
            r = lw.lower()
        except (L.LowerError, sdm.ParseError, KeyError, ValueError) as e:
            msg = f"{f.name}: {e}"
            errors.append(msg)
            per_page.setdefault(f.page, []).append(("fail", f, str(e)))
            continue
        image.add(f.name, r)
        compiled.append(f)
        per_page.setdefault(f.page, []).append(("ok", f, len([c for c in r.code if "op" in c])))

    if errors and not args.keep_going:
        for e in errors:
            print(f"ucc: {e}", file=sys.stderr)
        print(f"ucc: {len(errors)} errors", file=sys.stderr)
        return 1
    for e in errors:
        print(f"ucc: warning: {e}", file=sys.stderr)

    words = image.assemble()
    if len(words) > (1 << U.UADDR_W):
        print(f"ucc: {len(words)} uops don't fit in the ROM", file=sys.stderr)
        return 1

    out = [f"// generated by tools/ucc/ucc.py --target {TARGET} from the Intel SDM's pseudocode "
           "(do not edit)",
           f"// {len(words)} micro-ops, {len(compiled)} instruction forms",
           f"package {TARGET}_ucode_pkg;",
           f"  import {TARGET}_uop_pkg::*;",
           f"  localparam int UADDR_W = {U.UADDR_W};",
           f"  localparam int UROM_N = {len(words)};",
           f"  localparam logic [UADDR_W-1:0] UADDR_UD = {image.addr['UD']};",
           f"  localparam logic [UADDR_W-1:0] UADDR_DELIVER = {image.addr['DELIVER']};",
           "  // decoder attributes: imm 0 none, 1/2/3/4 bytes, 5 iw/id (z), 6 iw/id/io (v), 7 8 bytes;",
           "  // cls 0 default 32-bit operands, 1 default 64 (66h -> 16), 2 always 64",
           "  typedef struct packed {",
           "    logic valid; logic modrm; logic [2:0] imm; logic immz; logic [1:0] cls;",
           "    logic pfx_sens; logic plus_r;",
           "  } dattr_t;",
           f"  localparam logic [UOP_W-1:0] UROM [UROM_N] = '{{"]
    out.append(",\n".join(f"    {U.WIDTH}'h{w:0{(U.WIDTH + 3) // 4}x}" for w in words))
    out.append("  };")
    out.append("")
    out += ["  " + x for x in sv_tables(compiled, image, cfg_of)]
    out.append("endpackage")
    with open(args.out, "w") as fo:
        fo.write("\n".join(out) + "\n")
    if args.pkg:
        with open(args.pkg, "w") as fo:
            fo.write(U.sv_package(TARGET))
    if args.listing:
        with open(args.listing, "w") as fo:
            fo.write("\n".join(image.listing) + "\n")
    if args.report:
        write_report(args.report, pageobj, per_page, dups, errors, len(words))
    print(f"ucc: {len(compiled)} forms, {len(words)} uops, {len(errors)} left out",
          file=sys.stderr)
    return 0


def write_report(path, pageobj, per_page, dups, errors, nwords):
    lines = ["# ucc report", "",
             f"{nwords} micro-ops. Per instruction page: forms compiled, and whether "
             "the page's sections came from the manual verbatim or from an override "
             "(`tools/ucc/overrides/`).", "",
             "| Page | Forms | uops (avg) | Operation | Flags | Opcodes |",
             "|---|---|---|---|---|---|"]
    for name, (page, stem) in sorted(pageobj.items()):
        res = per_page.get(name, [])
        ok = [r for r in res if r[0] == "ok"]
        avg = sum(r[2] for r in ok) / len(ok) if ok else 0
        src = lambda s: "override" if s in page.overridden else "SDM"   # noqa: E731
        lines.append(f"| {name} | {len(ok)}/{len(res)} | {avg:.1f} | {src('operation')} | "
                     f"{src('flags')} | {src('opcodes')} |")
    if errors:
        lines += ["", "## Left out", ""] + [f"- {e}" for e in errors]
    with open(path, "w") as fo:
        fo.write("\n".join(lines) + "\n")


if __name__ == "__main__":
    sys.exit(main())
