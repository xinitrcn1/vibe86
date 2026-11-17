# JTAG: the die's test access port and debug module

The die's `tck`, `tms`, `tdi`, `trst_n` and `tdo` pins (docs/die-interface.md)
lead to an IEEE Std 1149.1-2013 test access port (`rtl/uncore/jtag_tap.sv`)
whose debug data registers reach a debug module in the core's clock domain
(`rtl/uncore/jtag_dm.sv`). Through it a probe can halt, single-step and
resume the core, read and write its registers while it's halted, and read
and write physical memory and MMIO through the die's fabric. `vsim -D PORT`
is such a probe: it speaks OpenOCD's `remote_bitbang` protocol, so a real
OpenOCD can drive the simulated die.

References: IEEE Std 1149.1-2013, *IEEE Standard for Test Access Port and
Boundary-Scan Architecture*; JEDEC JEP106 (*Standard Manufacturer's
Identification Code*); the *OpenOCD User's Guide* ("remote_bitbang",
"Debug Adapter Configuration", "JTAG Commands": `irscan`, `drscan`);
C. E. Cummings, "Clock Domain Crossing (CDC) Design & Verification Techniques
Using SystemVerilog", SNUG Boston 2008.

## The TAP

- The standard 16-state controller on TCK's rising edge (1149.1 Figure 6-1).
  TRST_n resets it asynchronously; five TCK cycles with TMS high reset it
  from any state. Test-Logic-Reset selects IDCODE.
- TDO changes on TCK's falling edge and is 0 outside Shift-IR/Shift-DR.
  Registers capture and shift on the rising edge; instructions and the
  debug registers' parallel outputs update on the falling edge in Update-IR
  and Update-DR.
- Without a probe, vsim holds TRST_n low, as boards pull it down. RESET also
  resets the TAP at power-on.

**Instruction register:** 5 bits; Capture-IR loads `00001b` (the low bits
`01b`, clause 7.1.1).

| IR | Name | DR length | Use |
|---|---|---|---|
| `00001` | IDCODE | 32 | the device identification (clause 8.13) |
| `10000` | DBGCTL | 32 | halt, resume, step; status |
| `10001` | DBGREG | 73 | core register access |
| `10010` | SBAADDR | 67 | system bus address and size |
| `10011` | SBADATA | 66 | system bus data, command and status |
| `11111` | BYPASS | 1 | also every unassigned code (clause 8.4); captures 0 |

There is no boundary-scan register yet (SAMPLE/PRELOAD/EXTEST): the die's
pins are modelled at the signal level but not scannable.

### IDCODE

`10B8_6001h`, from `rtl/common/platform.toml [jtag]`:

| Bits | Field | Value |
|---|---|---|
| 31:28 | version | 1 |
| 27:12 | part number | 0B86h |
| 11:8 | manufacturer bank (JEP106 continuation count) | 0 |
| 7:1 | manufacturer code without the parity bit | 00h |
| 0 | 1 (clause 8.13.1) | 1 |

Manufacturer code 00h in bank 0 is not assigned by JEP106, whose tables
start at 01h in every bank. The IDCODE therefore can't be read as any real
company's. It also avoids the all-ones pattern 7Fh/bank 0 (IDCODE 0FFh),
which 1149.1 reserves so that a chain's end can be found by shifting ones.

## The debug registers

All are shifted least significant bit first.

**DBGCTL** (32 bits).

On Update-DR the low bits are commands; writing zero does nothing:

| Bit | Command |
|---|---|
| 0 | haltreq: request a halt; it stays requested until resume |
| 1 | resume: drop the halt request |
| 2 | step: while halted, run one instruction and halt again |
| 3 | clear the sticky errors (sba_err, reg_err, dropped) |

On Capture-DR it reports:

| Bit | Status |
|---|---|
| 0 | halted |
| 1 | haltreq (the halt request is asserted) |
| 2 | busy: a request is still in the core clock domain |
| 3 | sba_err (sticky) |
| 4 | reg_err (sticky): register access while not halted |
| 5 | dropped (sticky): an Update-DR arrived while busy and was ignored |
| 31:28 | version: 1 |

**DBGREG** (73 bits): `{we[72], sel[71:64], data[63:0]}`.
- Update-DR asks for the access: a write of `data` when `we` = 1, else a
  read of `sel`. The core must be halted (else reg_err).
- Capture-DR returns `{0, sel, rdata}`, the last read's result. So a read
  takes two scans: ask, wait until DBGCTL.busy is 0, then capture. The
  capturing scan may already ask for the next register.
- `sel`: 0–15 the GPRs (RAX, RCX, RDX, RBX, RSP, RBP, RSI, RDI, R8–R15),
  16 RIP, 17 RFLAGS, 80h+n special register n (`tools/ucc/targets/<v>/uop.py`
  SPECIAL: CR0 = 80h, CR3 = 83h, the segment descriptor caches, ...).

**SBAADDR** (67 bits): `{size[66:64], addr[63:0]}`.
- A physical address and log2 of the access size (0–3: 1, 2, 4, 8 bytes;
  larger values mean 3).
- Capture returns the current values.

**SBADATA** (66 bits): `{cmd/status[65:64], data[63:0]}`.
- On Update-DR, `cmd` 01b reads at SBAADDR, 10b writes `data` there, and
  00b/11b do nothing (so scanning with 00b only reads the last result).
- On Capture-DR, bits 65:64 are `{sba_err, busy}` and bits 63:0 are the
  last read's data, little-endian from the address.

## Clock domains

The TAP runs on TCK; the debug module runs on the core clock. An Update-DR
of DBGCTL, DBGREG, or SBADATA with a command:
1. stores the request's payload in TCK-domain registers;
2. toggles `req_tog`;
3. leaves the payload untouched until `ack_tog` comes back (synchronized
   through two flip-flops on TCK).

The debug module synchronizes `req_tog` through two flip-flops. It then
reads the stable payload directly, writes the result registers, and toggles
`ack_tog`. Status bits (halted, haltreq, errors) cross through two
flip-flops on TCK. This is Cummings's multi-cycle-path formulation with
acknowledge feedback.

`busy` is `req_tog ≠ synchronized ack_tog`. A probe polls DBGCTL until busy
is 0 before trusting a result. An update made while busy is dropped and
reported, never half-applied. `tb_jtag` runs the protocol at TCK periods
of 3.4×, 1.4× and 0.6× the core clock's.

The debug module is reset by TRST_n (synchronized), not by RESET_L. A halt
request therefore survives a system reset, and the core halts at the reset
vector (assert haltreq, then SRST).

## The core and fabric ports (what the die top connects)

`jtag_dm` drives the core's debug ports (v1_core's port list):

- `dbg_req`: level; the core halts at the next instruction boundary and
  stays halted while it's 1. It answers with `dbg_halted`.
- `dbg_step`: a one-cycle pulse, only sent while `dbg_halted`. The core
  runs one instruction (dropping `dbg_halted` meanwhile) and halts again.
- `dbg_reg_sel`, `dbg_reg_rdata`: the selected register, read one cycle
  after `sel` is set.
- `dbg_reg_we`, `dbg_reg_wdata`: a one-cycle write pulse, only while halted.

The system-bus master goes into the fabric:
- outputs `dm_req, dm_we, dm_addr[63:0], dm_size[2:0], dm_wdata[63:0]`,
  with `dm_req` held until `dm_ack`;
- inputs `dm_ack` (one cycle), `dm_err`, `dm_rdata[63:0]`.

Addresses are physical and routed as the core's are (DRAM or the I/O link).

## Using it

```
vsim -n -D 9824 ... program.bin &
openocd -f sim/openocd/vibe86.cfg          # finds the TAP: IDCODE 0x10b86001
```

`sim/openocd/vibe86.cfg` defines Tcl procs over `irscan`/`drscan`:
`vibe86_status`, `vibe86_halt`, `vibe86_resume`, `vibe86_step`,
`vibe86_reg SEL`, and `vibe86_read64 ADDR_LO ADDR_HI`. OpenOCD isn't
installed on the build server. `tests/jtag/bitbang.py` speaks the protocol
itself:
- `jtag/bitbang-tap` runs it against the TAP and debug module alone
  (`tap_harness`);
- `jtag/bitbang` runs it against vsim and the die (disabled until the die
  top drives the JTAG pins).

The `remote_bitbang` server (`sim/jtag_bitbang.cpp`) handles one character
per operation:
- `0`–`7` set TCK, TMS, TDI;
- `R` reads TDO;
- `r`–`u` set TRST and SRST (SRST holds RESET_L);
- `B`/`b` (the LED) are ignored;
- `Q` closes the connection.

It applies at most one pin change per simulated cycle.
