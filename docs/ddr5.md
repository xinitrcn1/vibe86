# The DDR5 DIMM model (sim/ddr5.c)

`vsim`'s memory is the far side of the die's DDR5 pins
(docs/die-interface.md): two channels × two subchannels, two slots per
channel, each slot a 1Rx8 or 2Rx8 DIMM of x8 DDR5 dies. The model is
evaluated once per CK rising edge and sees only the pins: CS_n[3:0],
CA[13:0], DQ/CB (two beats per clock), DQS, RESET_n, ALERT_n, and the SPD
SMBus. The API, the clocking convention and the DRAM address map are in
`sim/ddr5.h`; `tests` → `sim/ddr5` exercises it.

Sources: Micron *DDR5 SDRAM Product Core Data Sheet* (ddr5_sdram_core.pdf,
Rev. D 10/2022) and *16Gb DDR5 SDRAM Die Rev A* (Rev. D 02/2023); JEDEC
JESD300-5 (SPD5 hub) and JESD400-5 (SPD contents) for the SPD side. The
PDFs are not in the repository (`work/`).

## Organisation

| Module | Dies | Ranks | Per rank and subchannel |
|---|---|---|---|
| 16 GiB | 16 Gb x8 | 1 | 4 data devices + 1 ECC device |
| 32 GiB | 16 Gb x8 | 2 | the same |
| 64 GiB | 32 Gb x8 (R0–R16) | 2 | the same |

x8 addressing (die data sheet Table 2): 8 bank groups × 4 banks, R0–R15
(R16 at 32 Gb), C0–C9, 1 KB page. Device d (0–3) carries DQ[8d+7:8d], the
ECC device CB[7:0]. A BL16 burst covers 16 columns of each device: one
64-byte line plus 16 CB bytes. The RDIMM's RCD is transparent (not
modelled): CS_n/CA reach the DRAMs unchanged. Populations must be uniform
(slots {0}, {0,1}, {0,2} or all four, the same module everywhere), as the
controller's address map requires.

## What is modelled (core data sheet)

| Feature | Where |
|---|---|
| Every command of the Command Truth Table: ACT, RD/RDA, WR/WRA (BL* burst chop, WR_Partial, AP), WRP/WRPA, MRW/MRR (CW), REFab/REFsb (and RFM as REF, MR58:OP[0] = 0), PREab/PREsb/PREpb, SRE/SREF, PDE, MPC, VREFCA/VREFCS, NOP, DES; RFU/REXT flagged | Table 140 |
| Two-cycle commands in 1N and 2N mode (2N is the reset default; the second half two clocks later; CS_n LOW between the halves flagged); CS_n LOW on the second cycle = cancel / non-target | Table 148, "1N/2N Mode Clarifications", Table 140 notes 8, 11 |
| Multicycle CS assertion for MPC/VREF while MR2:OP[4] = 0: tMPC_CS (3.5–8 nCK), CA static tMC_MPC_Setup/Hold (3 nCK) | "MPC Function Timing", Table 462 |
| MPC: DLL reset (applies MR13's shadow), ZQCal Start/Latch, SET 1N/2N, tDLLK/tCCD_L into MR13, training entries/exits (accepted), RTT/PDA codes (accepted) | Table 143 |
| Power-up: RESET_n LOW ≥ tINIT1 (tPW_RESET afterwards), CS_n LOW tINIT2 before RESET_n HIGH, CS_n LOW tINIT3 after, HIGH for tINIT4, NOPs ≥ tINIT5, tXPR (= tXS = tRFC1); only MRR/MRW/MPC/VREF until ZQ calibration latches; DLL reset needs the MR13 MPC first, ZQCal Start needs the DLL reset, training needs ZQCal Latch | Table 10, "Power-up Initialization Sequence" 7a–7c |
| MR0 burst length and CAS latency (CWL = CL − 2); CL below tAA (16.0 ns) flagged | Table 21, Table 361 |
| MRR data: BL16, UI 0–7 = 0, UI 8–15 = OP[0..7] on every DQ with odd DQs inverted | "Mode Register Read" |
| WRITE PATTERN: MR48 into every byte of the burst, no DQ data | "WRITE PATTERN and WRITE PATTERN AUTO PRECHARGE Commands" |
| Refresh: REFab needs all banks precharged (tRP met), busy tRFC1; REFsb (fine-granularity mode only, MR4:OP[4]) busies that bank in every BG for tRFCsb; more than 4 postponed refreshes (5 tREFI without REF) flagged | Table 176, "tREFI and tRFC Parameters" |
| DDR5-4800 timing checks: tRCD, tRP, tRAS, tRC, tRRD_S, tRRD_L, tFAW (1 KB), tCCD_S, tCCD_L, tCCD_S_WR, tCCD_L_WR / _WR2 (partial / full), tCCD_S/L_WTR, tCCD_RTW, tRTP, tWR (from the end of the write burst), tPPD, tMRW, tMRD, tMPC_Delay, tZQCAL, tDLLK, tRFC1, tRFCsb | Tables 428–430, 435–437 |
| Data bus: read data driven at CL, write data sampled at CWL (DQS marks beats); overlapping bursts, the controller driving DQ during read data or during reset, and missing write data are flagged | "READ Operations", "Write Operation" |
| Self refresh / power down: entry (SRE needs idle banks), exit with a NOP (tXS before the next REF/ACT) | Table 140, Table 437 |

Clock conversion: the data sheet's round-down integer algorithm (0.3%
correction, then rounded up; "Using Round-Down Only Integer Number
Math"), at tCK = 416 ps. The meminit_fast strap scales tINIT1, tINIT3,
tINIT4, tXPR, tDLLK and tZQCAL by 1/1024 (docs/die-interface.md).

Timing references: command-to-command timings are measured between the
first (CS_n-LOW) cycles of commands; CL and CWL from the final cycle of
the two-cycle READ/WRITE (the data sheet defines CWL "from final cycle of
the WRITE command" and CWL = CL − 2).

## Simplifications

- No DQS preamble/postamble waveforms, read DQS offset, CRC, DM, ODT,
  write leveling or training patterns: DQS is one bit per beat. Training
  MPCs are accepted and do nothing.
- BL32 is flagged (not modelled); C[3:0] of RD/WR must be 0 (sequential
  burst from a 16-column boundary).
- All devices of a rank act in lockstep (no per-DRAM addressability).
- ALERT_n stays HIGH (no CA parity, CRC or MBIST).
- tXP after power-down exit and self-refresh exit CS_n pulse widths
  (tCSH_SRexit, tCSL_SRexit) are not checked.
- Unwritten DRAM reads back deterministic pseudo-random data (DRAM
  powers up undefined); CB of an unwritten location is equally random.
- At DDR5-4800 with 1 KB pages tFAW (32 nCK) equals 4 × tRRD_S, so a tFAW
  violation is always also a tRRD_S violation.

## SPD hubs

One SMBus segment with the four slots' SPD5 hubs at 50h + slot (slot =
channel × 2 + DIMM), a bit-level I2C slave on the open-drain pins (the
bus level is the wired-AND of the die's drive and the hubs'). JESD300-5
subset: I2C legacy mode, 1-byte addressing; an address byte with bit 7
set (MemReg) selects NVM byte MR11[2:0] × 128 + (addr & 7Fh), bit 7 clear
a hub register (MR0/MR1 = 51h/18h, the SPD5118 device type; MR11 = the
page; others read 0). NVM is write-protected (writes are ACKed and
dropped). Reads auto-increment within the 128 bytes. START, repeated
START, STOP, ACK/NACK; an empty slot NACKs its address. The hubs never
stretch SCL and sample on SCL edges, so any SCL rate works. The contents
are `spd_build()` (sim/spd.c), JESD400-5 base configuration bytes.
