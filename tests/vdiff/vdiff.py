#!/usr/bin/env python3
"""Compare the SSE/SSE2 differential test's host and vsim runs (vgen.py).

    diff.py HOST_EXE VSIM IMAGE LISTING

host.c prints the state (xmm0-7, MXCSR, flags, rax) after every step, the scratch
buffer and 'end' as hex lines; sim.S writes the same in binary to vsim's
debug console. The first difference is reported with the instructions
around it and the state before.
"""
import os
import struct
import subprocess
import sys
import tempfile


N = 19                                  # qwords per step (vgen.py chk)


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
    names = [f"xmm{i // 2}.{'lo' if i % 2 == 0 else 'hi'}" for i in range(16)] + ["mxcsr", "flags", "rax"]
    nstate = N * len(lst)
    for i, (a, b) in enumerate(zip(h, s)):
        if a != b:
            if i >= nstate:
                print(f"scratch buffer differs at qword {i - nstate}: host {a} vsim {b}")
            else:
                step, r = divmod(i, N)
                print(f"step {step}: {names[r]} differs: host {a} vsim {b}")
                for l in lst[max(0, step - 3):step + 1]:
                    print("   " + l)
                prev = h[N * (step - 1):N * step] if step else []
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
