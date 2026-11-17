#!/usr/bin/env python3
"""Compare the differential test's host and vsim runs (gen.py).

    diff.py HOST_EXE VSIM IMAGE LISTING

host.c prints the state (flags, rax..r11) after every step, the scratch
buffer and 'end' as hex lines; sim.S writes the same in binary to vsim's
debug console. The first difference is reported with the instructions
around it and the state before.
"""
import os
import struct
import subprocess
import sys
import tempfile


def main():
    host, vsim, image, listing = sys.argv[1:5]
    h = subprocess.run([host], capture_output=True, text=True, check=True).stdout.split()
    with tempfile.TemporaryDirectory() as d:
        dbg = os.path.join(d, "debugcon")
        p = subprocess.run([vsim, "-n", "-b", "test", "-c", "60000000", "-g", dbg, image],
                           capture_output=True, text=True, timeout=1200)
        raw = open(dbg, "rb").read() if os.path.exists(dbg) else b""
    q = struct.unpack(f"<{len(raw) // 8}Q", raw[:len(raw) // 8 * 8])
    nbuf = 32
    s = [f"{v:016x}" for v in q[:len(q) - nbuf]] + [f"m{v:016x}" for v in q[len(q) - nbuf:]]
    if "end" in p.stdout:
        s.append("end")
    lst = open(listing).read().splitlines()
    names = ["flags", "rax", "rbx", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11", "buffer hash"]
    nstate = 12 * len(lst)
    for i, (a, b) in enumerate(zip(h, s)):
        if a != b:
            if i >= nstate:
                print(f"scratch buffer differs at qword {i - nstate}: host {a} vsim {b}")
            else:
                step, r = divmod(i, 12)
                print(f"step {step}: {names[r]} differs: host {a} vsim {b}")
                for l in lst[max(0, step - 3):step + 1]:
                    print("   " + l)
                prev = h[12 * (step - 1):12 * step] if step else []
                print("   state before (host): " + " ".join(f"{n}={v}" for n, v in zip(names, prev)))
            print("--- vsim stderr\n" + p.stderr[-1500:])
            return 1
    if len(h) != len(s) or p.returncode != 0:
        print(f"vsim printed {len(s)} lines (host {len(h)}), exit {p.returncode}")
        print("--- vsim stderr\n" + p.stderr[-1500:])
        return 1
    print(f"{len(lst)} steps agree")
    return 0


if __name__ == "__main__":
    sys.exit(main())
