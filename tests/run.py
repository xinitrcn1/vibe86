#!/usr/bin/env python3
"""Run one test image on vsim and check the outcome.

    run.py VSIM IMAGE.bin SOURCE

The source's comment lines say what to expect:
    // expect-exit: N          vsim's exit status (default 0: powered off)
    // expect-out: TEXT        a line the serial port (COM1) must print (contained in it)
    // vsim-args: ARGS         extra vsim arguments (default: -b test); ${VAR}
                               expands from the environment (ACPI, DATA)
"""
import os
import re
import subprocess
import sys


def main():
    vsim, image, src = sys.argv[1:4]
    text = open(src).read()
    exit_want = int((re.findall(r"//\s*expect-exit:\s*(\d+)", text) or ["0"])[0])
    outs = re.findall(r"//\s*expect-out:\s*(.*)", text)
    args = (re.findall(r"//\s*vsim-args:\s*(.*)", text) or ["-b test -c 2000000"])[0]
    args = re.sub(r"\$\{(\w+)\}", lambda m: os.environ.get(m.group(1), ""), args).split()
    p = subprocess.run([vsim, "-n"] + args + [image], capture_output=True, text=True,
                       timeout=1500)
    ok = True
    if p.returncode != exit_want:
        print(f"exit {p.returncode}, expected {exit_want}")
        ok = False
    lines = p.stdout.replace("\r", "\n").splitlines()
    for o in outs:
        if not any(o.strip() in l for l in lines):
            print(f"missing output line: {o.strip()!r}")
            ok = False
    if not ok:
        print("--- stdout\n" + p.stdout[-2000:] + "\n--- stderr\n" + p.stderr[-2000:])
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
