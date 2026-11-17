"""Reading .sdm files and parsing the SDM's pseudocode.

The pseudocode notation is described in the Intel SDM Vol. 2A, section 3.1.1.9
("Operation Section"): := assignment, IF/THEN/ELSE/FI, WHILE/DO/OD, FOR/TO/
DO/ROF, CASE/OF/ESAC, (* comments *), #XX(code) for exceptions, register
names, DEST/SRC operand names and bit fields [hi:lo]. The text in the manual
is typeset prose rather than a formal language, so the parser is lenient:

  * a statement ends at ';' or at the end of its line;
  * an IF without THEN takes the rest of its line as the condition;
  * 'ELSE IF' opens a nested IF; an ELSE that finds its IF already has an
    ELSE extends the trailing ELSE-IF chain (the manual closes some chains
    early, e.g. DIV);
  * a condition or statement that isn't an expression becomes Prose, which
    the oracle (oracle.py) interprets or rejects.

Leniency of this kind is what Reid needed for ARM's ASL too (FMCAD 2016,
doi:10.1109/FMCAD.2016.7886675); every place the text can't be read is
reported, and fixed by an override (tools/ucc/overrides/) rather than
silently.
"""

import re
from dataclasses import dataclass, field


# ------------------------------------------------------------------ .sdm files

@dataclass
class Page:
    name: str
    title: str
    source: str
    opcodes: list = field(default_factory=list)   # 5-tuples
    encoding: dict = field(default_factory=dict)  # op/en -> [operand texts]
    operation: str = ""
    flags: str = ""
    path: str = ""
    overridden: set = field(default_factory=set)  # sections replaced by an override


def read_sdm(path):
    sec = None
    title, source = "", ""
    parts = {"opcodes": [], "encoding": [], "operation": [], "flags": []}
    present = set()
    with open(path) as f:
        for line in f:
            line = line.rstrip("\n")
            if line.startswith("#") and sec is None:
                if title or not line.startswith("# "):
                    continue
                head = line[2:]
                if " -- " in head:
                    title, source = head.split(" -- ", 1)
                else:
                    title = head
                continue
            if line.startswith("@") and line[1:].strip() in parts:
                sec = line[1:].strip()
                present.add(sec)
                continue
            if sec:
                parts[sec].append(line)
    name = title.split("—")[0].strip() if title else path
    p = Page(name=name, title=title, source=source, path=path)
    for r in parts["opcodes"]:
        if r.strip() and not r.startswith("#"):
            p.opcodes.append([c.strip() for c in r.split("|")])
    for r in parts["encoding"]:
        if r.strip() and not r.startswith("#"):
            cols = [c.strip() for c in r.split("|")]
            p.encoding[cols[0]] = cols[1:]
    p.operation = "\n".join(parts["operation"])
    p.flags = " ".join(x.strip() for x in parts["flags"])
    p._present = present
    return p


def apply_override(page, ov):
    """Replace the sections present in the override page."""
    for sec in ov._present:
        if sec == "opcodes":
            page.opcodes = ov.opcodes
        elif sec == "encoding":
            page.encoding = ov.encoding
        elif sec == "operation":
            page.operation = ov.operation
        elif sec == "flags":
            page.flags = ov.flags
        page.overridden.add(sec)
    return page


# ------------------------------------------------------------------ lexer

class ParseError(Exception):
    pass


@dataclass
class Tok:
    kind: str    # 'id', 'num', 'op', 'nl', 'kw', 'exc', 'str', 'eof'
    text: str
    line: int
    val: int = 0
    col: int = 0


KEYWORDS = {"IF", "THEN", "ELSE", "FI", "WHILE", "DO", "OD", "FOR", "TO", "DOWNTO", "ROF", "ENDFOR",
            "CASE", "OF", "ESAC", "REPEAT", "UNTIL", "BREAK", "RETURN"}
WORD_OPS = {"AND", "OR", "XOR", "NOT", "MOD", "and", "or", "SignedModulus"}
REGS8H = {"AH", "BH", "CH", "DH"}

# typographic characters in the PDF text
NORMALIZE = {"–": "-", "−": "-", "—": "-", "∗": "*", "×": "*", "≠": "!=", "≤": "<=",
             "≥": ">=", "←": ":=", "’": "'", "‘": "'", "“": '"', "”": '"', " ": " ",
             "<>": "!=", "≪": "<<", "≫": ">>"}

TOKEN = re.compile(r"""
    (?P<ws>[ \t]+)
  | (?P<nl>\n)
  | (?P<exc>\#[A-Z]{2}(?![A-Za-z]))
  | (?P<str>"[^"\n]*")
  | (?P<op>:=|!=|<=|>=|<<|>>|==|[-+*/=<>()\[\]:,;.{}&|^~!?'])
  | (?P<word>[A-Za-z_0-9][A-Za-z_0-9]*)
  | (?P<other>.)
""", re.X)

HEX = re.compile(r"^[0-9A-F]+H$")
BIN = re.compile(r"^[01]+B$")


def number(word):
    """The value of a numeric literal: 1FH, 0000FFFFH, 10B, 64, 0x10."""
    if word in REGS8H:
        return None
    if HEX.match(word) and (word[0].isdigit() or len(word) >= 3):
        try:
            return int(word[:-1], 16)
        except ValueError:
            return None
    if BIN.match(word) and len(word) > 2 and word[0] in "01" and all(c in "01" for c in word[:-1]):
        return int(word[:-1], 2)
    if word.isdigit():
        return int(word)
    if re.match(r"^0x[0-9A-Fa-f]+$", word):
        return int(word, 16)
    return None


def strip_comments(text):
    out, i, depth = [], 0, 0
    while i < len(text):
        if text.startswith("(*", i):
            depth += 1
            i += 2
            continue
        if depth and text.startswith("*)", i):
            depth -= 1
            i += 2
            continue
        if depth:
            if text[i] == "\n":
                out.append("\n")    # keep line structure
            i += 1
            continue
        out.append(text[i])
        i += 1
    return "".join(out)


def lex(text):
    for k, v in NORMALIZE.items():
        text = text.replace(k, v)
    text = strip_comments(text)
    toks, line = [], 1
    linestart = 0
    for m in TOKEN.finditer(text):
        kind = m.lastgroup
        s = m.group()
        col = m.start() - linestart
        if kind == "ws":
            continue
        if kind == "nl":
            line += 1
            linestart = m.end()
            if toks and toks[-1].kind != "nl":
                toks.append(Tok("nl", "\n", line - 1))
            continue
        if kind == "word":
            n = number(s)
            if n is not None:
                toks.append(Tok("num", s, line, n, col))
            elif s in KEYWORDS:
                toks.append(Tok("kw", s, line, 0, col))
            else:
                toks.append(Tok("id", s, line, 0, col))
            continue
        if kind == "other":
            toks.append(Tok("op", s, line))
            continue
        toks.append(Tok(kind, s, line))
    toks.append(Tok("nl", "\n", line))
    toks.append(Tok("eof", "", line))
    return toks


# ------------------------------------------------------------------ AST

@dataclass
class Num:
    v: int


@dataclass
class Name:
    n: str


@dataclass
class Bin:
    op: str
    a: object
    b: object


@dataclass
class Un:
    op: str
    a: object


@dataclass
class Call:
    f: str
    args: list


@dataclass
class Slice:
    e: object
    hi: object
    lo: object


@dataclass
class Index:          # Memory[addr], X[i]
    e: object
    i: object


@dataclass
class Seg:            # SS:RSP (segment-qualified address)
    seg: str
    addr: object


@dataclass
class Prose:
    text: str


@dataclass
class Assign:
    dst: object
    src: object
    line: int = 0


@dataclass
class If:
    cond: object
    then: list
    els: list = None
    line: int = 0
    closed: bool = False
    col: int = 0


@dataclass
class While:
    cond: object
    body: list
    line: int = 0


@dataclass
class For:
    var: str
    lo: object
    hi: object
    down: bool
    body: list
    line: int = 0


@dataclass
class Raise:
    exc: str
    code: object = None
    line: int = 0


@dataclass
class Do:              # a statement-level call or a prose statement
    e: object
    line: int = 0


@dataclass
class Case:
    subject: object
    arms: list   # [(value expr | None for default, [stmts])]
    line: int = 0


SEGMENTS = {"CS", "DS", "ES", "FS", "GS", "SS"}


# ------------------------------------------------------------------ expressions

class Expr:
    """Precedence climbing over a token slice; raises ParseError."""

    PREC = [("OR", "or", "||"), ("AND", "and", "&&"), ("|",), ("XOR", "^"), ("&",),
            ("=", "==", "!="), ("<", ">", "<=", ">="), ("<<", ">>"),
            ("+", "-"), ("*", "/", "MOD", "SignedModulus")]

    def __init__(self, toks):
        self.t = toks
        self.i = 0

    def peek(self, k=0):
        j = self.i + k
        return self.t[j] if j < len(self.t) else Tok("eof", "", 0)

    def take(self):
        tok = self.peek()
        self.i += 1
        return tok

    def expect(self, text):
        tok = self.take()
        if tok.text != text:
            raise ParseError(f"expected '{text}', got '{tok.text}'")

    def parse(self):
        e = self.binary(0)
        if self.i != len(self.t):
            raise ParseError(f"unexpected '{self.peek().text}'")
        return e

    def binary(self, level):
        if level == len(self.PREC):
            return self.unary()
        e = self.binary(level + 1)
        while True:
            tok = self.peek()
            if tok.kind in ("op", "id") and tok.text in self.PREC[level]:
                self.take()
                op = tok.text.upper() if tok.text in ("and", "or") else tok.text
                op = {"||": "OR", "&&": "AND", "==": "=", "&": "AND", "|": "OR",
                      "^": "XOR"}.get(op, op)
                e = Bin(op, e, self.binary(level + 1))
            else:
                return e

    def unary(self):
        tok = self.peek()
        if tok.text in ("-", "NOT", "~", "!"):
            self.take()
            return Un("-" if tok.text == "-" else "NOT", self.unary())
        return self.postfix(self.primary())

    def postfix(self, e):
        while True:
            tok = self.peek()
            if tok.text == "[":
                self.take()
                a = self.binary(0)
                if self.peek().text == ":":
                    self.take()
                    b = self.binary(0)
                    self.expect("]")
                    e = Slice(e, a, b)
                else:
                    self.expect("]")
                    e = Index(e, a)
            elif tok.text == ":" and isinstance(e, Name) and e.n in SEGMENTS:
                self.take()
                e = Seg(e.n, self.unary())
            elif tok.text == ":" and isinstance(e, (Name, Slice)):
                # register pairs: DX:AX, EDX:EAX, RDX:RAX (a concatenation)
                nxt = self.peek(1)
                if nxt.kind == "id":
                    self.take()
                    e = Call("Concat", [e, self.postfix(self.primary())])
                else:
                    return e
            else:
                return e

    def primary(self):
        tok = self.take()
        if tok.kind == "num":
            return Num(tok.val)
        if tok.text == "(":
            e = self.binary(0)
            self.expect(")")
            return e
        if tok.text == "[":            # NEG's 'DEST := [- (DEST)]'
            e = self.binary(0)
            self.expect("]")
            return e
        if tok.kind == "id":
            name = tok.text
            # dotted names: CPUID.80000001H:ECX, CR4.PCIDE, RFLAGS.IF
            while self.peek().text == "." and self.peek(1).kind in ("id", "num", "kw"):
                self.take()
                name += "." + self.take().text
            if self.peek().text == "(":
                self.take()
                args = []
                if self.peek().text != ")":
                    while True:
                        args.append(self.binary(0))
                        if self.peek().text == ",":
                            self.take()
                            continue
                        break
                self.expect(")")
                return Call(name, args)
            return Name(name)
        raise ParseError(f"unexpected '{tok.text}'")


def parse_expr(toks):
    toks = [t for t in toks if t.kind != "nl"]
    if not toks:
        raise ParseError("empty expression")
    return Expr(toks).parse()


def expr_or_prose(toks):
    try:
        return parse_expr(toks)
    except ParseError:
        return Prose(" ".join(t.text for t in toks if t.kind != "nl"))


# ------------------------------------------------------------------ statements

class Parser:
    def __init__(self, text):
        self.t = lex(text)
        self.i = 0

    def peek(self, k=0):
        return self.t[min(self.i + k, len(self.t) - 1)]

    def take(self):
        tok = self.t[self.i]
        self.i += 1
        return tok

    def skip_nl(self):
        while self.peek().kind == "nl" or self.peek().text == ";":
            self.i += 1

    def at_kw(self, *kws):
        tok = self.peek()
        return tok.kind == "kw" and tok.text in kws

    def parse(self):
        body = self.block(("eof",))
        return body

    def block(self, stops):
        """Statements until one of the stop keywords (not consumed)."""
        out = []
        while True:
            self.skip_nl()
            tok = self.peek()
            if tok.kind == "eof" or (tok.kind == "kw" and tok.text in stops):
                return out
            st = self.statement(stops)
            if st is not None:
                out.append(st)

    def line_end(self):
        """Index of the end of the current logical line (';' or newline)."""
        j = self.i
        while self.t[j].kind not in ("nl", "eof") and self.t[j].text != ";":
            j += 1
        return j

    def statement(self, stops):
        tok = self.peek()
        if tok.kind == "kw":
            if tok.text == "IF":
                return self.if_stmt()
            if tok.text == "WHILE":
                return self.while_stmt()
            if tok.text == "FOR":
                return self.for_stmt()
            if tok.text == "CASE":
                return self.case_stmt()
            if tok.text in ("ELSE", "FI", "OD", "ROF", "ENDFOR", "ESAC", "DO", "THEN"):
                # a stray keyword where the enclosing construct doesn't want it
                raise ParseError(f"line {tok.line}: unexpected {tok.text}")
            if tok.text in ("BREAK", "RETURN"):
                self.take()
                return Do(Call(tok.text, []), tok.line)
        if tok.kind == "exc":
            self.take()
            code = None
            if self.peek().text == "(":
                self.take()
                j = self.i
                depth = 1
                while depth:
                    if self.t[j].text == "(":
                        depth += 1
                    elif self.t[j].text == ")":
                        depth -= 1
                    j += 1
                code = expr_or_prose(self.t[self.i:j - 1])
                self.i = j
            self.end_stmt()
            return Raise(tok.text[1:], code, tok.line)
        # an assignment or a call, up to ';' or the end of the line (a line
        # ending in an operator continues on the next)
        start = self.i
        j = self.line_end()
        while self.t[j].kind == "nl" and j > start and self.t[j - 1].text in (
                "+", "-", "*", "AND", "OR", ":=", "(", ","):
            self.t[j].kind = "ws"
            j = self.line_end_from(j + 1)
        toks = [t for t in self.t[start:j] if t.kind != "ws"]
        self.i = j
        self.end_stmt()
        if not toks:
            return None
        # assignment: find ':=' at depth 0
        depth = 0
        for k, t in enumerate(toks):
            if t.text in ("(", "["):
                depth += 1
            elif t.text in (")", "]"):
                depth -= 1
            elif t.text == ":=" and depth == 0:
                return Assign(expr_or_prose(toks[:k]), expr_or_prose(toks[k + 1:]), toks[0].line)
            elif t.text == "=" and depth == 0 and k == 1 and len(toks) > 2 and toks[0].kind == "id":
                # 'tempSsEIP = ShadowStackPop4B();' (assignment written with '=')
                return Assign(expr_or_prose(toks[:k]), expr_or_prose(toks[k + 1:]), toks[0].line)
        return Do(expr_or_prose(toks), toks[0].line)

    def line_end_from(self, j):
        while self.t[j].kind not in ("nl", "eof") and self.t[j].text != ";":
            j += 1
        return j

    def end_stmt(self):
        if self.peek().text == ";":
            self.take()

    def condition(self, then_kw="THEN"):
        """Tokens of a condition: to THEN, or to the end of the line when the
        next line doesn't continue it (AND/OR/...)."""
        toks = []
        while True:
            tok = self.peek()
            if tok.kind == "eof":
                raise ParseError("condition runs off the end")
            if tok.kind == "kw" and tok.text == then_kw:
                self.take()
                return toks
            if tok.text == ";":
                self.take()
                j = self.i
                while self.t[j].kind == "nl":
                    j += 1
                if self.t[j].kind == "kw" and self.t[j].text == then_kw:
                    self.i = j + 1
                    return toks
                return toks
            if tok.kind == "nl":
                nxt = self.peek(1)
                if (nxt.kind == "kw" and nxt.text == then_kw) or nxt.text in (
                        "AND", "OR", "and", "or", "(", ")") or (toks and toks[-1].text in (
                        "AND", "OR", "and", "or", "(", ",", "=", "+", "-")):
                    self.take()
                    continue
                self.take()
                return toks
            toks.append(self.take())

    def if_stmt(self, col=None):
        tok = self.take()                # IF
        line = tok.line
        cond = expr_or_prose(self.condition())
        node = If(cond, [], None, line, col=tok.col if col is None else col)
        node.then = self.block(("ELSE", "FI"))
        self.else_part(node)
        return node

    def else_part(self, node):
        """After THEN-block: ELSE ... FI, or FI. Handles ELSE IF chains."""
        tok = self.peek()
        if tok.kind == "kw" and tok.text == "FI":
            self.take()
            self.end_stmt()
            node.closed = True
            return
        if tok.kind == "kw" and tok.text == "ELSE":
            self.take()
            if self.peek().kind == "kw" and self.peek().text == "IF":
                inner = self.if_stmt(col=tok.col)   # an ELSE IF sits at its ELSE's column
                node.els = [inner]
                # a chain closed early: 'ELSE IF a THEN x FI; ELSE IF b ...'
                while True:
                    self.skip_nl()
                    if self.at_kw("ELSE") and self.peek().col >= node.col:
                        # an ELSE indented at least as far as this IF continues
                        # its chain; one further out belongs to an enclosing IF
                        tail = node
                        while tail.els and len(tail.els) == 1 and isinstance(tail.els[0], If) \
                                and tail.els[0].els is not None:
                            tail = tail.els[0]
                        target = tail.els[0] if tail.els and isinstance(tail.els[0], If) else tail
                        if target.els is not None:
                            # the chain is complete: this ELSE belongs to an
                            # enclosing IF (PUSH closes its inner chain early)
                            return
                        etok = self.take()
                        if self.at_kw("IF"):
                            target.els = [self.if_stmt(col=etok.col)]
                        else:
                            target.els = self.block(("FI", "ELSE"))
                        continue
                    if self.at_kw("FI") and self.peek().col >= node.col:
                        # an extra FI closing this chain (DIV); one further
                        # out belongs to an enclosing IF
                        self.take()
                        self.end_stmt()
                        node.closed = True
                    return
            node.els = self.block(("FI", "ELSE"))
            if self.at_kw("ELSE"):
                raise ParseError(f"line {self.peek().line}: second ELSE")
            if self.at_kw("FI"):
                self.take()
                self.end_stmt()
                node.closed = True
            return
        # no FI: the block ended at the end of the enclosing construct
        return

    def while_stmt(self):
        line = self.take().line
        cond = expr_or_prose(self.condition("DO"))
        body = self.block(("OD",))
        if self.at_kw("OD"):
            self.take()
            self.end_stmt()
        return While(cond, body, line)

    def for_stmt(self):
        line = self.take().line
        toks = self.condition("DO")
        # 'FOR i := 0 TO 7' / 'FOR i := 7 DOWNTO 0'
        k = next((n for n, t in enumerate(toks) if t.text in ("TO", "DOWNTO")), None)
        if k is None or len(toks) < 3 or toks[1].text != ":=":
            raise ParseError(f"line {line}: FOR")
        body = self.block(("ROF", "OD", "ENDFOR"))     # the vector pages close FOR with ENDFOR
        if self.at_kw("ROF", "OD", "ENDFOR"):
            self.take()
            self.end_stmt()
        return For(toks[0].text, parse_expr(toks[2:k]), parse_expr(toks[k + 1:]),
                   toks[k].text == "DOWNTO", body, line)

    def case_stmt(self):
        line = self.take().line
        subj = expr_or_prose(self.condition("OF"))
        arms = []
        while True:
            self.skip_nl()
            if self.at_kw("ESAC"):
                self.take()
                self.end_stmt()
                break
            # 'value: statements' or 'DEFAULT: statements'
            j = self.i
            while self.t[j].text != ":" and self.t[j].kind not in ("nl", "eof"):
                j += 1
            if self.t[j].text != ":":
                raise ParseError(f"line {self.peek().line}: CASE arm")
            label = self.t[self.i:j]
            self.i = j + 1
            val = None if (len(label) == 1 and label[0].text.upper() == "DEFAULT") \
                else expr_or_prose(label)
            body = []
            while True:
                self.skip_nl()
                if self.at_kw("ESAC"):
                    break
                # the next arm starts with 'expr :' on a fresh line
                k = self.i
                while self.t[k].kind not in ("nl", "eof") and self.t[k].text not in (":", ":=", ";"):
                    k += 1
                if self.t[k].text == ":" and self.t[k + 1].text != "=":
                    break
                body.append(self.statement(("ESAC",)))
            arms.append((val, [b for b in body if b is not None]))
        return Case(subj, arms, line)


def parse(text):
    return Parser(text).parse()


def dump(node, ind=0):
    """A readable rendering of the AST (ucc -v)."""
    pad = "  " * ind
    if isinstance(node, list):
        return "\n".join(dump(s, ind) for s in node)
    if isinstance(node, If):
        s = f"{pad}IF {node.cond}\n{dump(node.then, ind + 1)}"
        if node.els is not None:
            s += f"\n{pad}ELSE\n{dump(node.els, ind + 1)}"
        return s
    if isinstance(node, While):
        return f"{pad}WHILE {node.cond}\n{dump(node.body, ind + 1)}"
    return f"{pad}{node}"
