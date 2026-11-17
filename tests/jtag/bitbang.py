#!/usr/bin/env python3
"""jtag/bitbang: vsim's JTAG probe end to end, without OpenOCD.

    bitbang.py VSIM PLATFORM_TOML [--stub]

Starts vsim headless with -D (OpenOCD's remote_bitbang protocol) on a
program that spins at the reset vector, connects as a remote_bitbang client
and checks, through the die's TAP (docs/jtag.md):
  the IR capture pattern (00001b), IDCODE after Test-Logic-Reset (the value
  rtl/common/platform.toml [jtag] describes), BYPASS (one bit, captures 0),
  and DBGCTL's version field. With --stub (VSIM is tap_harness, the TAP
  and debug module with stub core and fabric: tap_top.sv) also the debug
  path: halt, a register read, a system bus read, resume.
"""
import os
import socket
import subprocess
import sys
import tempfile
import time
import tomllib


class Probe:
    """A remote_bitbang client: one character per pin operation."""

    def __init__(self, port):
        deadline = time.time() + 60
        while True:
            try:
                self.s = socket.create_connection(("127.0.0.1", port), timeout=60)
                break
            except OSError:
                if time.time() > deadline:
                    raise
                time.sleep(0.2)
        self.out = bytearray()

    def clock(self, tms, tdi=0, read=False):
        """one TCK cycle; with read, sample TDO before the rising edge"""
        self.out += b"%d" % (tms << 1 | tdi)            # TCK low
        if read:
            self.out += b"R"
        self.out += b"%d" % (4 | tms << 1 | tdi)        # TCK high

    def flush(self, nread=0):
        self.s.sendall(bytes(self.out))
        self.out.clear()
        got = b""
        while len(got) < nread:
            got += self.s.recv(nread - len(got))
        return [1 if c == ord("1") else 0 for c in got]

    def reset(self):
        self.out += b"r"                                 # TRST and SRST released
        for _ in range(5):
            self.clock(1)
        self.clock(0)                                    # Run-Test/Idle

    def shift(self, ir, bits, n):
        """from Run-Test/Idle: shift n bits (LSB first) of IR or DR; returns the bits out"""
        for t in ([1, 1, 0, 0] if ir else [1, 0, 0]):
            self.clock(t)
        for i in range(n):
            self.clock(1 if i == n - 1 else 0, (bits >> i) & 1, read=True)
        self.clock(1)                                    # Update
        self.clock(0)                                    # Run-Test/Idle
        out = self.flush(n)
        return sum(b << i for i, b in enumerate(out))


def ctl_wait(pr, irw):
    """DBGCTL after the pending request completed"""
    pr.shift(True, 0b10000, irw)
    for _ in range(1000):
        s = pr.shift(False, 0, 32)
        if not s & 4:                                    # busy
            return s
    raise RuntimeError("DBGCTL busy forever")


def debug_checks(pr, irw):
    """tap_top's stubs: register n reads n; the bus returns addr ^ 5A5A..."""
    fails = []
    pr.shift(True, 0b10000, irw)
    pr.shift(False, 0b0001, 32)                          # haltreq
    s = ctl_wait(pr, irw)
    if s & 3 != 3:
        fails.append(f"halt: DBGCTL {s:08x}")
    pr.shift(True, 0b10001, irw)
    pr.shift(False, 16 << 64, 73)                        # read register 16
    ctl_wait(pr, irw)
    pr.shift(True, 0b10001, irw)
    got = pr.shift(False, 16 << 64, 73)
    if got & (1 << 64) - 1 != 16:
        fails.append(f"register 16 read {got & (1 << 64) - 1:x}, want 10")
    ctl_wait(pr, irw)
    pr.shift(True, 0b10010, irw)
    pr.shift(False, 3 << 64 | 0x12345678, 67)            # SBAADDR: 8 bytes at 12345678h
    pr.shift(True, 0b10011, irw)
    pr.shift(False, 1 << 64, 66)                         # read
    ctl_wait(pr, irw)
    pr.shift(True, 0b10011, irw)
    got = pr.shift(False, 0, 66)
    want = 0x12345678 ^ 0x5A5A5A5A5A5A5A5A
    if got & (1 << 64) - 1 != want or got >> 64:
        fails.append(f"SBA read {got:x}, want {want:x} with status 0")
    pr.shift(True, 0b10000, irw)
    pr.shift(False, 0b0010, 32)                          # resume
    s = ctl_wait(pr, irw)
    if s & 3:
        fails.append(f"resume: DBGCTL {s:08x}")
    return fails


def main():
    vsim, plat_toml = sys.argv[1], sys.argv[2]
    with open(plat_toml, "rb") as f:
        j = tomllib.load(f)["jtag"]
    idcode = (j["idcode_version"] << 28 | j["idcode_part"] << 12 |
              j["idcode_mfr_bank"] << 8 | j["idcode_mfr_code"] << 1 | 1)
    with tempfile.TemporaryDirectory() as tmp:
        img = bytearray(b"\xff" * 0x10000)
        img[0xFFF0:0xFFF2] = b"\xeb\xfe"                 # jmp $ at the reset vector
        path = os.path.join(tmp, "spin.bin")
        with open(path, "wb") as f:
            f.write(img)
        s = socket.socket()
        s.bind(("127.0.0.1", 0))
        port = s.getsockname()[1]
        s.close()
        p = subprocess.Popen([vsim, "-n", "-b", "test", "-c", "2000000000", "-H", "2000000000",
                              "-D", str(port), path],
                             stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        fails = []
        try:
            pr = Probe(port)
            pr.reset()
            pr.flush()
            got = pr.shift(False, 0, 32)
            if got != idcode:
                fails.append(f"IDCODE after reset: {got:08x}, want {idcode:08x}")
            got = pr.shift(True, 0b11111, j["ir_width"])
            if got != 0b00001:
                fails.append(f"IR capture: {got:05b}, want 00001")
            got = pr.shift(False, 0xB3, 9)               # BYPASS (selected above)
            if got != 0xB3 << 1:
                fails.append(f"BYPASS: {got:03x}, want {0xB3 << 1:03x}")
            pr.shift(True, 0b10000, j["ir_width"])         # DBGCTL
            got = pr.shift(False, 0, 32)
            if got >> 28 != 1:
                fails.append(f"DBGCTL version: {got >> 28}, want 1 (DBGCTL {got:08x})")
            if "--stub" in sys.argv:
                fails += debug_checks(pr, j["ir_width"])
            pr.out += b"Q"
            pr.flush()
        finally:
            p.terminate()
            p.wait(timeout=30)
    for m in fails:
        print("FAIL", m)
    if fails:
        return 1
    print(f"jtag/bitbang: IDCODE {idcode:08x}, IR capture, BYPASS, DBGCTL: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
