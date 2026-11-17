# The vibe86 die interface (v1 die)

The die's external pins, as `rtl/common/pins.toml` describes them and every
core version's die top (`rtl/<v>/<v>_die.sv`) implements (D-014). The die is
the core plus the **uncore** in RTL (`rtl/uncore/`): the address fabric, two
DDR5 memory controllers, the memory init engine, the I/O-link endpoint, the
host bridge's memory decoding and the JTAG TAP. Outside the die, `vsim`
models the DIMMs (pin level), the I/O die / chipset (PCIe, the PC legacy
devices, fw_cfg, ...) behind the I/O link, the SPD hubs, and a JTAG probe.

Sources: Micron *DDR5 SDRAM Product Core Data Sheet* (ddr5_sdram_core.pdf,
Rev. D 10/2022) — Table 140 (Command Truth Table), Table 21 (MR0), Table 143
(MPC), Table 10 and "Power-up Initialization Sequence", Tables 361 and
405–407 (DDR5-4800 speed bin and timings), Table 176 (tRFC); Micron *16Gb
DDR5 SDRAM Die Rev A* (Tables 1, 2, 4); IEEE Std 1149.1-2013 (JTAG); the
JESD79-5 standard these follow. The PDFs are not in git (`work/`).

## Clocks and reset

| Pin | Dir | Meaning |
|---|---|---|
| `clk` | in | the reference clock; the core, the fabric and the memory controllers run on it. The DDR5 CK the die forwards to the DIMMs is this clock (1:1): one sim cycle is one tCK |
| `rst_n` | in | RESET_L (cold/warm reset). The memory init engine runs after it; the x86 core leaves reset when DRAM is ready |
| `meminit_fast` | in | strap (simulation): scale the JEDEC initialization waits (tINIT1, tINIT3, tINIT4, tXPR, tDLLK, tZQCAL) down by 1024. As DRAM vendors' simulation models' fast-init modes; real boards tie it low |

## DDR5 (2 channels × 2 subchannels)

Per subchannel `ddr<c><s>_` with c ∈ {0,1}, s ∈ {a,b} (four instances), the
host side of a DDR5 DIMM's subchannel driven directly (as on an unbuffered
DIMM; an RDIMM's register (JESD82-511) would sit between, and is not
modelled until its datasheet is in hand):

| Pin | Dir | Width | Meaning |
|---|---|---|---|
| `cs_n` | out | 4 | chip selects: DIMM 0 rank 0/1, DIMM 1 rank 0/1 (active low) |
| `ca` | out | 14 | CA[13:0], sampled on the CK rising edge (Table 140 encodings; 2-cycle commands use two consecutive clocks) |
| `dq_o` | out | 80 | write data: two beats per clock (beat 0 in bits 39:0, beat 1 in 79:40), each beat DQ[31:0] + CB[7:0] |
| `dq_oe` | out | 1 | the die drives DQ (writes) |
| `dqs_o` | out | 2 | write strobe DQS_t per beat (1 = toggling for that beat) |
| `dq_i` | in | 80 | read data, same layout |
| `dqs_i` | in | 2 | read strobe from the DRAMs, one bit per beat |

Per channel `ddr<c>_`:

| Pin | Dir | Width | Meaning |
|---|---|---|---|
| `reset_n` | out | 1 | the DRAMs' RESET_n (both subchannels, both DIMMs) |
| `alert_n` | in | 1 | ALERT_n (wired-OR: CRC/parity errors, MBIST busy) |

DQ/DQS timing: after RD at clock T, the DRAM drives `dqs_i`/`dq_i` for BL16
= 8 clocks starting at T + CL (preamble omitted: the strobe bits mark valid
beats); after WR at T the die drives write data from T + CWL (CWL = CL − 2).
BL16 on a 32-bit subchannel is one 64-byte line.

## SPD sideband

| Pin | Dir | Meaning |
|---|---|---|
| `spd_scl_o`, `spd_sda_o` | out | open drain: 0 pulls the line low |
| `spd_scl_i`, `spd_sda_i` | in | the bus levels (wired-AND) |

The SPD hubs (JESD300-5) of the four slots are on this one segment
(addresses 50h–53h: channel × 2 + DIMM).

## The I/O link

Two unidirectional 64-bit flit channels with valid/ready handshakes (a
flit moves when both are high). Packets are a header flit, an address flit
and 0–8 data flits:

| Pin | Dir | Width | Meaning |
|---|---|---|---|
| `iol_tx_valid`, `iol_tx_flit` | out | 1, 64 | die → I/O die |
| `iol_tx_ready` | in | 1 | |
| `iol_rx_valid`, `iol_rx_flit` | in | 1, 64 | I/O die → die |
| `iol_rx_ready` | out | 1 | |

Header flit: `[7:0]` opcode, `[15:8]` tag, `[18:16]` log2 size (0–6),
`[22:19]` number of data flits, `[31:23]` reserved, `[63:32]` byte enables
for ≤ 4-byte… (reserved; sizes are naturally aligned).

| Opcode | Direction | Meaning |
|---|---|---|
| 01h MRD | either | memory read (address flit); answered by CPL with data |
| 02h MWR | either | memory write (address + data flits); posted, no completion |
| 03h IORD | die → I/O | port read |
| 04h IOWR | die → I/O | port write; answered by CPL (no data) — I/O writes are non-posted |
| 08h CPL | either | completion for `tag`, with data flits for reads |
| 09h CPLE | either | completion with error (unsupported request / bus error) |

Requests from the die carry tags 00h–7Fh, inbound requests (device DMA)
80h–FFh. Inbound memory requests are routed by the fabric to DRAM.

## Interrupt sideband, debug and trace

`intr_valid`/`intr_vector`/`intr_ack` (the 8259's INTR/INTA, MSIs), `nmi`,
`smi`, `mce_*`: unchanged (pins.toml). JTAG: `tck`, `tms`, `tdi`, `trst_n`
in, `tdo` out (IEEE 1149.1 TAP; IDCODE, BYPASS, and the debug module's
instructions: docs/jtag.md). The trace port (`rt_*`, `ev_*`, `shutdown`,
`halted`, `shutdown_rip`) is unchanged.
