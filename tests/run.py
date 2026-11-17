#!/usr/bin/env python3
"""Run one test image on vsim and check the outcome.

    run.py VSIM IMAGE.bin SOURCE

The source's comment lines say what to expect:
    // expect-exit: N          vsim's exit status (default 0: powered off)
    // expect-out: TEXT        a line the serial port (COM1) must print (contained in it)
    // expect-vga: TEXT        a line of the VGA text screen at exit (vsim -Y; contained in it)
    // vsim-args: ARGS         extra vsim arguments (default: -b test; shell quoting); ${VAR}
                               expands from the environment (ACPI, DATA, WORK, ...)
    // requires: FILE          skip the test (exit 77) unless FILE exists; ${VAR} expands
"""
import os
import re
import shlex
import subprocess
import sys
import tempfile


def expand(s):
    return re.sub(r"\$\{(\w+)\}", lambda m: os.environ.get(m.group(1), ""), s)


def main():
    vsim, image, src = sys.argv[1:4]
    text = open(src).read()
    for req in re.findall(r"//\s*requires:\s*(.*)", text):
        if not os.path.exists(expand(req.strip())):
            print(f"skipped: {expand(req.strip())} is missing")
            return 77
    exit_want = int((re.findall(r"//\s*expect-exit:\s*(\d+)", text) or ["0"])[0])
    outs = re.findall(r"//\s*expect-out:\s*(.*)", text)
    vgas = re.findall(r"//\s*expect-vga:\s*(.*)", text)
    args = (re.findall(r"//\s*vsim-args:\s*(.*)", text) or ["-b test -c 2000000"])[0]
    args = shlex.split(expand(args))
    with tempfile.TemporaryDirectory() as tmp:
        vga_txt = os.path.join(tmp, "vga.txt")
        if vgas:
            args = ["-Y", vga_txt] + args
        p = subprocess.run([vsim, "-n"] + args + [image], capture_output=True, text=True,
                           errors="replace", timeout=21600)  # ctest's TIMEOUT is the real limit
        screen = open(vga_txt, errors="replace").read().splitlines() if vgas and os.path.exists(vga_txt) else []
    ok = True
    if p.returncode != exit_want:
        print(f"exit {p.returncode}, expected {exit_want}")
        ok = False
    lines = p.stdout.replace("\r", "\n").splitlines()
    for o in outs:
        if not any(o.strip() in l for l in lines):
            print(f"missing output line: {o.strip()!r}")
            ok = False
    for v in vgas:
        if not any(v.strip() in l for l in screen):
            print(f"missing VGA line: {v.strip()!r}")
            ok = False
    if not ok:
        print("--- stdout\n" + p.stdout[-3000:] + "\n--- stderr\n" + p.stderr[-2000:])
        if vgas:
            print("--- VGA\n" + "\n".join(screen))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
