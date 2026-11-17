# The vibe86 package and its pinout (D-019)

The processor is **one die** in an LGA socket: the cores with their
caches and local APICs, the JTAG TAP and debug module, the trace encoder,
the DDR5 memory controllers and the memory init engine, the address fabric
and host bridge, the system link to the chipset, the inter-socket links
and the system management unit (SMU), all on the same die (D-014's
monolithic die, kept: the user decided against a compute/I-O die split
before the pinout was frozen).

What this adds to D-014: DDR5 balls for eight channels, interrupts and
machine checks as system-link messages instead of sideband pins, and the
balls a server part needs to be powered, sequenced, cooled, managed and
identified.

The ball map is `rtl/common/package.toml`: every ball group, its count,
its rail or the signals it carries. The logic signals are
`rtl/common/pins.toml` as before; each of those names its ball group.
`tools/gen/package.py` checks the two against each other and the power
budget (test `tools/package`).

## Ball budget (LGA-4096)

| Group | Balls | Notes |
|---|---:|---|
| DDR5 channels 0–7 | 1168 | 146 per channel (below); channels 0–1 are implemented, 2–7 reserved |
| SPD / I3C buses 0–1 | 4 | bus 0: channels 0–3, bus 1: channels 4–7 (reserved) |
| System link (to the chipset) | 66 | x16: 16 TX + 16 RX differential pairs, REFCLK pair |
| Inter-socket links 0–1 | 130 | 2 × x16, REFCLK pair; reserved |
| Clock, power sequencing | 4 | REFCLK pair, PWROK, RESET_L |
| Voltage regulator control | 4 | SVC, SVD, SVT, VR_HOT_L |
| Thermal | 2 | THERMTRIP_L, PROCHOT_L |
| Errors | 5 | MCERR_L, CATERR_L, ERROR_L[2:0] |
| Sideband (BMC) | 3 | SB_SCL, SB_SDA, SB_ALERT_L |
| Identification | 12 | SOCKET_ID[2:0], PKG_TYPE[3:0], PKG_VER[3:0], SKT_OCC_L |
| JTAG | 5 | TCK, TMS, TDI, TDO, TRST_L |
| Trace port | 18 | TRACECLK, TRACECTL, TRACEDATA[15:0] |
| Test straps | 1 | TEST_MEMINIT_FAST (boards tie it low) |
| Power | 1228 | five rails (below) |
| Ground (VSS) | 1320 | more than all supply balls together |
| Reserved (RSVD) | 126 | unconnected; for later revisions |
| **Total** | **4096** | |

`tools/gen/package.py` computes the total and fails the build's test if a
group's count, a rail's current per contact or the ground count is off.

### DDR5 (per channel, 146 balls)

Two 40-bit subchannels (32 DQ + 8 CB, ECC), two DIMMs per channel, as
docs/die-interface.md describes the signals: per subchannel DQ/CB 40,
DQS_t/DQS_c 10 (five ×8 byte lanes), CA[13:0] 14, CS_n 4 (two DIMMs × two
ranks), CK_t/CK_c 4 (one pair per DIMM): 72; per channel 2 × 72 plus
RESET_n and ALERT_n. Eight channels are the server class (AMD's SP5 has
twelve, Intel's LGA-4677 eight). The v1 die implements channels 0 and 1; the
balls of channels 2–7 are assigned and reserved: their outputs idle (CS_n
high, CA and DQ undriven, RESET_n low) and their inputs are ignored. The
SPD buses follow the DDR5 SPD hub's address space: 50h–57h is eight
slots, so channels 0–3 share bus 0 and channels 4–7 bus 1 (JESD300-5; the
hubs are I3C Basic targets with I2C fallback).

### Power and ground

| Rail | Voltage | EDC | Balls | A per ball |
|---|---|---:|---:|---:|
| VDDCR_CPU | 0.60–1.20 V (SVID) | 450 A | 900 | 0.50 |
| VDDCR_SOC | 0.75–1.05 V (SVID) | 120 A | 240 | 0.50 |
| VDDIO_MEM | 1.1 V (DDR5 VDDQ) | 32 A | 64 | 0.50 |
| VDD_18 | 1.8 V | 10 A | 20 | 0.50 |
| VDD_18_S5 | 1.8 V standby | 2 A | 4 | 0.50 |
| VSS | 0 V | | 1320 | |

Design rule: at most 0.5 A per LGA contact at the rail's electrical
design current (EDC), and at least as many VSS balls as supply balls (the
return current; and every signal ball needs a nearby ground for its
reference). The checker enforces both. VDDCR_CPU and VDDCR_SOC come from
voltage regulators the package controls (below); VDD_18_S5 is the standby
rail that keeps PWROK, the sideband and the identification balls alive
while the main rails are off.

## Signals

Directions are the package's. `_L` is active low; open-drain signals are
modelled as an `_o` (0 pulls low) / `_i` (the line's level) pair, like the
SPD pins.

### Clock, reset and power sequencing

| Ball | Dir | Meaning |
|---|---|---|
| REFCLK_P/N | in | the reference clock (`clk`): one sim cycle |
| PWROK | in | the board's power-good: every package rail is in regulation. Low holds the whole package in reset |
| RESET_L | in | the platform reset (`rst_n`): held low for ≥ 1 ms after PWROK on a real board |

The system management unit (SMU, on the die's standby domain, powered from
VDD_18_S5)
sequences the package:

1. **Off**: PWROK low. Everything but the SMU's standby logic is in reset;
   the SVID bus idles.
2. **Rails up**: PWROK rises (the VRs came up at their boot voltage). The
   SMU sets each controlled rail's operating voltage over SVID and waits
   for every VR's acknowledgement and settled status. A VR that NAKs (or
   answers with bad parity) three times, or doesn't settle within 2^18
   clocks, leaves the die off until PWROK falls, with ERROR_L[2] asserted
   (a fatal error) and the sideband's power state reading 7. (No POST
   code: the fabric that would send one is still in reset.)
3. **Reset**: rails good, waiting for RESET_L to go high.
4. **Memory init**: the memory init engine (D-014) trains DRAM.
5. **Run**: the cores leave reset.

PWROK falling at any point goes back to 1 at once; RESET_L falling goes
back to 3 (the rails stay up: a warm reset).

### Voltage regulator control (SVID-like)

A three-wire serial bus from the SMU to the VRs, modelled on Intel's SVID
and AMD's SVI (both closed specifications; this protocol is our own and
fully specified here):

| Ball | Dir | Meaning |
|---|---|---|
| SVC | out | the bus clock: the SMU drives it, one bit per two `clk` cycles (data changes when SVC is low) |
| SVD | inout (open drain) | data: frames from the SMU, the VR's acknowledge and read data |
| SVT | in | telemetry/alert from the VRs (open drain, wired-OR): low = a VR wants attention (over-current, over-temperature, settled after a slow change) |
| VR_HOT_L | in | a VR is hot (also wired to PROCHOT_L on most boards) |

Frame: a start (SVD falling while SVC is high), 4-bit VR address, 5-bit
command, 8-bit payload, an even-parity bit over those 17 bits, then a
turnaround bit (nobody drives) and the VR's 2-bit acknowledge (01 ACK,
11 NAK -- also what an address with no VR reads; 10 = ACK and busy: the
VR will pull SVT when settled), for reads 8 data bits and their even
parity from the VR, and a stop (SVD rising while SVC is high). Every bit
is MSB first; the driver changes SVD while SVC is low, the receiver
samples it when SVC rises. The bus idles with both lines high.
Addresses: 0 VDDCR_CPU, 1 VDDCR_SOC. Commands: 01h SetVID-fast (payload:
VID, 5 mV steps from 0.25 V: V = 0.25 + 0.005·VID), 02h SetVID-slow,
03h SetPS (power state 0–3), 04h SetVR-enable, 10h GetReg (payload: the
register: 00h status, 01h output current in A, 02h temperature in °C,
03h VID). The VR model is `sim/vr.c`.

### Thermal

| Ball | Dir | Meaning |
|---|---|---|
| THERMTRIP_L | out (open drain) | the die reached its trip temperature (115 °C): the package stops at once (cores and memory controllers held in reset) and keeps THERMTRIP_L low until PWROK falls. The board must cut the main rails |
| PROCHOT_L | inout (open drain) | out: the die is at or above its control limit (100 °C); in: the board (typically a VR's VR_HOT) demands throttling. Either way the cores throttle: they start one instruction in eight cycles until it is released |

The temperature is the die's thermal sensor (at the cores' hot spot). Temperature is the
physical world, so the simulator supplies it (a model input, not a ball:
`tsense`, in units of 1/8 °C; vsim `-Z` sets it, default 45 °C); the
sensor, its comparisons, the trip latch and the throttle are RTL.

### Machine checks and errors

| Ball | Dir | Meaning |
|---|---|---|
| MCERR_L | out (open drain) | pulsed low for 16 clocks when a core signals a machine check (#MC or a machine-check shutdown) |
| CATERR_L | out (open drain) | a catastrophic error, held until reset: a core shut down with MCG_STATUS.MCIP set (a machine check during a machine check, SDM Vol. 3B 17.3.1.2), or a thermal trip |
| ERROR_L[0] | out (open drain) | a corrected error was logged (ECC corrected in DRAM): pulsed low for 16 clocks |
| ERROR_L[1] | out (open drain) | an uncorrected, recoverable error was logged: pulsed |
| ERROR_L[2] | out (open drain) | a fatal error: held until reset (an uncorrectable error the core couldn't take as #MC, a power-sequencing failure) |

Machine checks are no longer pins: the memory controllers report ECC
errors to the core whose access saw them on the die, and the chipset can
report one over the system link (an MCE message).

### Sideband management bus (to a BMC)

| Ball | Dir | Meaning |
|---|---|---|
| SB_SCL | inout (open drain) | clock, driven by the BMC |
| SB_SDA | inout (open drain) | data |
| SB_ALERT_L | out (open drain) | SMBALERT#: the SMU has news (a thermal threshold crossed, an error logged, PROCHOT) |

An SMBus 3.x target (System Management Bus Specification 3.2) on standby
power, so a BMC reads it with the host off; the balls are I3C-ready (MIPI
I3C Basic 1.1.1 is backward compatible: an I3C controller talks to it as a
legacy I2C target). Two target addresses, after AMD's APML split:

- **4Ch, temperature interface** (like SB-TSI): 01h/10h the die's
  temperature (integer °C, eighths in 7:5), 02h status (bit 0 PROCHOT,
  1 THERMTRIP, 2 above the high limit, 3 below the low limit), 07h/08h
  high/low limits, 09h configuration (bit 7: mask the alert).
- **3Ch, management interface** (like SB-RMI): 00h interface revision,
  01h socket ID, 02h package type, 03h package version, 04h the last POST
  code, 05h power state (the sequencer's step), 06h error status (bits:
  ERROR_L[2:0], MCERR, CATERR, THERMTRIP), write 1 to clear the pulsed
  ones' sticky bits; 08h–0Fh the last machine check's MC0_STATUS (bytes 0–7),
  10h–17h its MC0_ADDR, 20h the cores' count, 21h–22h the VRs' last
  status.

### Identification

| Ball | Dir | Meaning |
|---|---|---|
| SOCKET_ID[2:0] | in (strap) | the socket's number on the board: the cores' APIC IDs are `socket × 16 + core`; socket 0's core 0 is the bootstrap processor |
| PKG_TYPE[3:0] | out | the package type, wired in the package (1 = this LGA-4096 package). Also CPUID Fn8000_0001_EBX[31:28] PkgType (AMD APM Vol. 3, Appendix E) |
| PKG_VER[3:0] | out | the pinout's version, wired in the package (1 = this document). A board checks it before PWROK |
| SKT_OCC_L | out | socket occupied: tied to VSS in the package |

A board (or BMC) reads PKG_TYPE/PKG_VER/SKT_OCC_L on standby power before
it powers the main rails; firmware tells parts apart by CPUID (family,
model, stepping, PkgType) and the SMU's registers.

### JTAG and trace

JTAG (IEEE 1149.1): TCK, TMS, TDI, TDO, TRST_L, unchanged (docs/jtag.md);
the TAP is on the die as before.

The **trace port** streams the cores' execution for an external trace
probe: TRACECLK (= the core clock), TRACECTL (high: TRACEDATA carries a
packet unit) and TRACEDATA[15:0]. The die's trace encoder turns
retirement into packets (a branch-trace format after Intel PT's: a PSB
sync every 4096 bytes, TIP with the target of every taken branch,
exception/interrupt packets with vector and RIP, an overflow packet when
the FIFO drops data). It is enabled through the debug module (JTAG).
`tools/trace` decodes a capture (vsim `-R file`).

The retirement and event ports vsim's harness uses (`rt_*`, `ev_*`,
`shutdown`, `halted`, `shutdown_rip`) are **simulation observation
ports**, not balls: pins.toml marks them `sim = true`.

## Interrupts on the system link

The chipset's interrupt wires become messages, as HyperTransport carried
them (HyperTransport I/O Link Specification 3.10, 8.5 interrupts, 8.6
system management): a VW (virtual wire) message for INTR (asserted,
with the 8259's vector, or deasserted), NMI, SMI and INIT; INTA back to the
chipset when a core took the ExtINT; MSIs stay memory writes to
FEEx_xxxxh. The fabric turns them into messages on the die's APIC bus
(D-017). The die no longer has INTR/NMI/SMI/MCE pins.
