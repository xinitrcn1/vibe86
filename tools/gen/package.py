#!/usr/bin/env python3
"""package: check the package's ball map against its pins (D-019).

    package.py rtl/common/package.toml rtl/common/pins.toml [--table]

Checks, failing with a message per problem:
  - the ball groups (each instance counted) add up to [package] balls;
  - every power rail carries at most amps_per_ball at its EDC;
  - VSS has at least as many balls as all supply rails together;
  - every pin of pins.toml is a simulation observation port (sim = true)
    or names a ball group that exists (group = "...");
  - every signal group has a pin (a group without one is a typo or
    something the RTL doesn't model), but analog ones (logic = false).
--table prints the ball budget as a Markdown table (docs/package.md).
"""

import sys
import tomllib


def balls_of(g):
    return g["balls"] * len(g.get("instances", [None]))


def check(pkg, pins):
    errs = []
    groups = {g["name"]: g for g in pkg["group"]}
    total = sum(balls_of(g) for g in pkg["group"])
    if total != pkg["package"]["balls"]:
        errs.append(f"the groups have {total} balls, the package {pkg['package']['balls']}")
    supply = 0
    for g in pkg["group"]:
        if g["kind"] == "power":
            supply += g["balls"]
            a = g["edc_amps"] / g["balls"]
            if a > pkg["package"]["amps_per_ball"] + 1e-9:
                errs.append(f"{g['name']}: {a:.2f} A per ball at EDC "
                            f"(at most {pkg['package']['amps_per_ball']})")
    vss = sum(g["balls"] for g in pkg["group"] if g["kind"] == "ground")
    if vss < supply:
        errs.append(f"VSS has {vss} balls, the supply rails {supply}")
    used = set()
    for p in pins["pin"]:
        if p.get("sim"):
            continue
        g = p.get("group")
        if g is None:
            errs.append(f"pin {p['name']}: no ball group (group = ...) and not sim = true")
        elif g not in groups:
            errs.append(f"pin {p['name']}: no ball group '{g}'")
        elif groups[g]["kind"] != "signal":
            errs.append(f"pin {p['name']}: group '{g}' is not a signal group")
        else:
            used.add(g)
    for g in pkg["group"]:
        if g["kind"] == "signal" and g.get("logic", True) and g["name"] not in used:
            errs.append(f"signal group {g['name']} carries no pin of pins.toml")
    return errs


def table(pkg):
    out = ["| Group | Kind | Balls |", "|---|---|---:|"]
    for g in pkg["group"]:
        n = len(g.get("instances", [None]))
        name = g["name"] + (f" × {n}" if n > 1 else "")
        out.append(f"| {name} | {g['kind']} | {balls_of(g)} |")
    out.append(f"| **total** | | **{sum(balls_of(g) for g in pkg['group'])}** |")
    return "\n".join(out)


def main(argv):
    if len(argv) < 3:
        sys.exit(__doc__)
    with open(argv[1], "rb") as f:
        pkg = tomllib.load(f)
    with open(argv[2], "rb") as f:
        pins = tomllib.load(f)
    if "--table" in argv:
        print(table(pkg))
    errs = check(pkg, pins)
    for e in errs:
        print(f"package: {e}", file=sys.stderr)
    return 1 if errs else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
