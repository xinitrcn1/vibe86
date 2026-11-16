"""The prose oracle: what v1 assumes wherever the SDM's pseudocode is English.

The manual's Operation sections mix expressions with prose conditions ('IF
stack not large enough for a 8-byte return address', 'IF near call') and
describe flags in prose ('The OF, SF, ZF, AF, CF, and PF flags are set
according to the result.'). Every such phrase ucc meets must match a rule
here; the rules are therefore the complete list of v1's architectural
assumptions (64-bit mode, CPL 0, flat segments, no CET shadow stacks,
LAHF/SAHF available, ...), and an unknown phrase is a compile error rather
than a guess.

The flag prose is read the way Heule et al. had to treat the manual's flag
descriptions when learning x86-64 semantics ("Stratified Synthesis:
Automatically Learning the x86-64 Instruction Set", PLDI 2016,
doi:10.1145/2908080.2908121): 'undefined' flags are left unchanged by v1 and
are masked out when comparing against hardware.
"""

import re

from uop import STATUS


def norm(text):
    t = text.replace(" - ", "-").replace(" -", "-").replace("- ", "-")
    t = re.sub(r"\s+", " ", t).strip().lower()
    t = t.replace("( ", "(").replace(" )", ")").replace(" ,", ",").replace(" .", ".")
    t = t.replace(" [", "[").replace("[ ", "[").replace(" ]", "]").replace(" :", ":").replace(": ", ":")
    return t


# conditions: phrase (regex on the normalized prose) -> function(form) giving
# True/False, or a string of pseudocode to parse in its place
def lma(f):
    """IA-32e mode: static in real mode (no) and 64-bit mode (yes)."""
    return {0: False, 3: True}.get(f.mode, "IA32_EFER.LMA = 1")


CONDITIONS = [
    (r"64-bit mode", lambda f: f.mode == 3),
    (r"ia-32e mode( active)?|ia-64 mode|ia32_efer\.lma = 1", lma),
    (r"non-64-bit mode|not in 64-bit mode", lambda f: f.mode != 3),
    (r"compatibility mode", lambda f: False if f.mode in (0, 3) else "IA32_EFER.LMA = 1"),
    (r"\(?real-address mode\)?|in real-address mode|pe = 0|cr0\.pe = 0", lambda f: f.mode == 0),
    (r"\(?virtual-8086 mode\)?|vm = 1", lambda f: "VM = 1" if f.mode == 1 else False),
    (r"\(?protected mode\)?|in protected mode",
     lambda f: {0: False, 3: False, 2: "IA32_EFER.LMA = 0",
                1: "IA32_EFER.LMA = 0 AND VM = 0"}[f.mode]),
    (r"pe = 1", lambda f: f.mode != 0),
    (r"vm = 0", lambda f: "VM = 0" if f.mode == 1 else True),
    (r"cpl = 0", lambda f: True if f.mode == 0 else "CPL = 0"),
    (r"cpl > 0|cpl != 0", lambda f: False if f.mode == 0 else "CPL > 0"),
    (r"cpl > iopl", lambda f: False if f.mode == 0 else "CPL > IOPL"),
    (r"cpl <= iopl", lambda f: True if f.mode == 0 else "CPL <= IOPL"),
    (r"(near )?call", lambda f: f.mnem == "CALL"),
    (r"near relative call", lambda f: f.mnem == "CALL" and f.opc == 0xE8),
    (r"near absolute call", lambda f: f.mnem == "CALL" and f.opc == 0xFF),
    (r"far call", lambda f: False),
    (r"far jump", lambda f: False),
    (r"near jump", lambda f: f.mnem == "JMP"),
    (r"near relative jump", lambda f: f.mnem == "JMP" and f.opc in (0xEB, 0xE9)),
    (r"instruction = near return", lambda f: f.opc in (0xC3, 0xC2)),
    (r"instruction = far return", lambda f: f.opc in (0xCB, 0xCA)),
    (r"instruction has immediate operand", lambda f: bool(f.imm)),
    (r"stack not large enough for an? \d+-byte return address", lambda f: False),
    (r"top \d+ bytes of stack not within stack limits", lambda f: False),
    (r"temp[re]?ip (is )?not within code segment limits?", lambda f: False),
    (r".*not within code segment limits?", lambda f: False),
    (r"shadowstackenabled\(cpl\)( and dest != 0)?", lambda f: False),
    (r"shadowstackenabled\(cpl\) and.*", lambda f: False),
    (r"endbranchenabled\(cpl\)", lambda f: False),
    (r"condition( true)?", lambda f: ("CC", f.cc)),
    (r"\(?byte operation\)?", lambda f: f.osz == 8),
    (r"\(?byte (move|store|load|comparison|compare|scan)\)?", lambda f: f.osz == 8),
    (r"\(?word (move|store|load|comparison|compare|scan)\)?", lambda f: f.osz == 16),
    (r"\(?doubleword (move|store|load|comparison|compare|scan)\)?", lambda f: f.osz == 32),
    (r"\(?quadword (move|store|load|comparison|compare|scan)\)?", lambda f: f.osz == 64),
    (r"instruction is (sal or shl|shl or sal)", lambda f: f.mnem in ("SAL", "SHL")),
    (r"instruction is sar", lambda f: f.mnem == "SAR"),
    (r"instruction is shr", lambda f: f.mnem == "SHR"),
    (r"instruction is rol", lambda f: f.mnem == "ROL"),
    (r"instruction is int3", lambda f: f.mnem == "INT3"),
    (r"instruction is lidt", lambda f: f.mnem == "LIDT"),
    (r"instruction is ror", lambda f: f.mnem == "ROR"),
    (r"instruction is rcl", lambda f: f.mnem == "RCL"),
    (r"cpuid\.80000001h:ecx(\.lahf_sahf_64)?\[0\] = 1;?", lambda f: True),
    (r"\(operandsize = 32, instruction = cwde\)", lambda f: f.osz == 32),
    (r"64-bit mode and operandsize = 64", lambda f: f.mode == 3 and f.osz == 64),
    (r"\(?operandsize = 64\)? and \(?64-bit mode\)?", lambda f: f.mode == 3 and f.osz == 64),
    (r"\(?operandsize = 32 and ia-32e mode( active)?\)?",
     lambda f: False if f.osz != 32 else lma(f)),
    (r"64-bit mode and operandsize = 32", lambda f: f.mode == 3 and f.osz == 32),
    (r"dest is( a)? register", lambda f: not f.mem),
    (r"dest is( a)? memory( location)?", lambda f: bool(f.mem)),
    (r"src is( a)? register", lambda f: not f.mem),
    (r"lock prefix.*", lambda f: False),
    (r"tf = 1.*", lambda f: False),
]

# statements in prose (or calls the core doesn't implement and never needs
# because the condition guarding them is statically false)
STATEMENTS = [
    (r"shadowstack\w*\(.*\)", "skip"),
    (r"service pending interrupts.*", "skip"),
    (r"\(\* .* \*\)", "skip"),
    (r":", "skip"),
    (r"enter halt state", "HLT"),
]


def condition(text, form):
    """True/False, ('CC', cc) or None (unknown)."""
    t = norm(text)
    for pat, fn in CONDITIONS:
        if re.fullmatch(pat, t):
            return fn(form)
    return None


def statement(text):
    t = norm(text)
    for pat, what in STATEMENTS:
        if re.fullmatch(pat, t):
            return what
    return None


# ----------------------------------------------------------------- flags prose

FLAG_WORDS = {"of": "OF", "sf": "SF", "zf": "ZF", "af": "AF", "cf": "CF", "pf": "PF"}


def flags_spec(prose):
    """{flag: 'result' | 'clear' | 'set' | 'undefined' | 'unaffected' | 'other'}.

    Sentences are classified by their verb phrase; the flags they name get
    that action. A flag the prose describes some other way ('The CF flag
    contains the value of the last bit shifted out') is 'other': the
    pseudocode must assign it, or ucc reports it.
    """
    spec = {}
    if not prose or prose.strip().lower().startswith("none"):
        return {f: "unaffected" for f in STATUS}
    text = re.sub(r"\s+", " ", prose)
    text = re.sub(r"flags?\.\d", "flags.", text)      # footnote marks: 'cleared.2'
    for sent in re.split(r"(?<=[.;])\s+", text):
        s = sent.lower()
        names = [FLAG_WORDS[w] for w in re.findall(r"\b(of|sf|zf|af|cf|pf)\b", s)]
        if not names:
            if "all other flags are unaffected" in s or "other flags are unaffected" in s:
                for f in STATUS:
                    spec.setdefault(f, "unaffected")
            continue
        if "undefined" in s:
            act = "undefined"
        elif "not affected" in s or "unaffected" in s or "are not modified" in s:
            act = "unaffected"
        elif re.search(r"\bif\b|\bwhen\b|contains", s):
            act = "other"         # conditional: the pseudocode or the ALU decides
        elif "according to the result" in s or "set according to" in s:
            act = "result"
        elif "cleared" in s or "set to 0" in s or "are 0" in s:
            act = "clear"
        elif re.search(r"\bis set\b|\bset to 1\b|\bare set\b", s) and "if" not in s:
            act = "set"
        else:
            act = "other"
        # 'The OF and CF flags are cleared; the SF, ZF, and PF flags are set
        # according to the result.' -- split on ';' already; a sentence that
        # names flags under two verbs is split at ' the '
        for f in names:
            spec.setdefault(f, act)
    for f in STATUS:
        spec.setdefault(f, "unaffected")
    return spec
