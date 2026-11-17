# vibe86 platform: the socket, the boards and the simulator

This file describes the **package** (what is in the socket besides the
core), the **boards** that `vsim` models, and their physical map. The
addresses aren't architectural, but they follow PC conventions where x86
software expects them: the reset vector at the top of the 4 GiB space, the
MSI window at `0xFEE0_0000`, COM1 at I/O port `0x3F8`, POST codes on port
`0x80`, reset control on port `0xCF9`.

The board model (DDR5 memory subsystem, PCIe, IOMMU, USB, VGA, I2C, TPM,
PMC) is ported from the vercpu project's `sim/` (`~/vercpu`, commit
`936c3cb`); only the map, the I/O space, the MSI format and the core pins
changed.

## The socket

vibe86 is a server part in a custom socket. Like current Xeon and EPYC
packages, the memory controllers, the PCIe root complex and the IOMMU are
on the package (the **uncore**), so the DDR5 channels and PCIe lanes are
socket pins. The boundary inside `vsim` is drawn one level further in: the
core talks to the uncore over **one bus** (below), and the uncore and the
board are C models.

| Pin group (socket) | On a real part | In `vsim` |
|---|---|---|
| DDR5 channels | 16 DIMM slots: 8 channels × 2 DIMMs per channel, each channel two 32-bit subchannels + 8 ECC bits each (JESD79-5) | `dram.c`: the memory controller (registers at `0xFE00_4000`), per-bank DDR5 timing, ECC; 16 slots, populated with `-m` |
| SPD SMBus / I3C | the sideband to the DIMMs' SPD hubs (JESD300-5 hub, JESD400-5 contents) | `dram.c`: SPD SMBus controller at `0xFE00_3000`, two segments of 8 |
| PCIe lanes | root ports, x16/x8 bifurcation | `pcie.c`: host bridge + 8 root ports (one per slot), ECAM, BAR windows, MSI; `iommu.c` in front of DMA |
| eSPI / LPC to the board | the legacy I/O devices of a PC: COM ports, POST code port, reset control | `board.c` `board_io`: COM1 (16550) at `0x3F8`, POST code port `0x80`, reset control `0xCF9` |
| SPI flash | boot firmware, mapped below 4 GiB | `board.c`: 16 MiB EEPROM at `0xFF00_0000`, reset vector `0xFFFF_FFF0` |
| PECI / thermal (THERMTRIP#, PROCHOT#) | package temperature to the BMC, thermal shutdown | `pmc.c`: die and board temperatures, hot and critical trips (critical: power off, vsim exits 5) |
| SMBus / I2C (board management) | TPM, fan controller, temperature sensors | `i2c.c`: I2C controller at `0xFE00_8000`; TPM (`tpm.c`, libtpms), fan monitor, LM75 |
| RESET#, PWRGOOD | cold and warm reset | `vsim` holds the core's `rst_n` low for one clock at power-on and on SYSCON/`0xCF9` resets; the PMC records the reset cause |
| NMI | the board's NMI pin | `nmi` core pin, raised by SYSCON `0xAAAA` |
| MCERR#, CATERR# | machine-check signalling | `mce_*` core pins: the DDR5 controller's corrected/uncorrected ECC errors, or SYSCON `0xBBxx` |
| MSIs (inbound memory writes to `0xFEEx_xxxx`) | interrupt messages from PCIe devices and the IOMMU, delivered to a local APIC | `board.c` `msi_write`: the destination APIC id and vector go to the core's `intr_*` pins |

### The core-uncore bus

The core (vsim `-C`, `rtl/<version>`) reaches everything over one bus, one
transaction at a time, held until acknowledged (the die interface,
`rtl/common/pins.toml`, generated into each core's port list and vsim's
`struct cpu`; D-013):

- **memory**: 1–16 bytes (log2 size 0–4) at any address the target
  accepts, or a whole 64-byte line (size 6) of cacheable memory for a core
  with caches. Read data is up to 512 bits, byte 0 = the lowest address.
  DRAM transactions take the DDR5 timing model's latency; everything else
  answers in the same cycle. An address nothing decodes is a **bus error**
  (`bus_err` with the acknowledge); what the core does with it is the core
  version's business (`rtl/<version>/README.md`).
- **I/O** (`bus_io`, for `IN`/`OUT`): the port number in `bus_addr`, 1, 2 or
  4 bytes. Ports nothing decodes read as all ones and ignore writes, as on a
  PC; only a bad size is a bus error.

Uncore events (MSIs, NMI, machine checks) wait in a 64-entry queue that
`vsim` drains into the core's `intr_*`, `nmi` and `mce_*` pins, one event per
cycle (a full queue drops events).

The core reports to `vsim` on trace pins: an instruction retired (RIP,
length, bytes, micro-ops), an exception or interrupt delivered (vector,
error code, RIP), **shutdown** (a triple fault, Intel SDM Vol. 3A §6.15,
"Interrupt 8—Double Fault Exception") and **halted** (`HLT`).

## The boards

| Part | Server board (`-b server`, default) | Test board (`-b test`) |
|---|---|---|
| Memory | 16 DDR5 RDIMM slots (default one 8 GiB DDR5-4800 ECC RDIMM in slot 0, `-m`); DRAM decodes only after firmware trains the controller | 2 MiB SRAM at `0x0000_0000`, usable at reset; no DRAM |
| Firmware | 16 MiB EEPROM (SPI flash) | the same |
| Serial | COM1 (16550) on I/O port `0x3F8`, MMIO alias at `0xFE00_0000` | the same |
| PCIe | root complex, 8 slots, IOMMU; DMA test endpoint in slot 0 (`-P`), xHCI (USB keyboard + mouse) in slot 1 (`-X`) | the same |
| Display | VGA: 80×25 text, 640×480 / 800×600 32 bpp; the "monitor" is an SDL3 window | the same |
| I2C | TPM 2.0 (debug), fan monitor, LM75 | the same |
| Power | PMC: power, reset cause, rails, thermal trips, watchdog, suspend; 2 fans | the same |

**There is no vibe86 firmware yet.** The server board's DRAM is decoded only
once firmware has read the SPDs and trained the DDR5 controller (the
bring-up sequence below), so until that firmware exists the tests run on
the **test board**, whose SRAM works from reset.

## Physical map

| Address | Size | Device | Access |
|---|---|---|---|
| `0x0000_0000` | 2 MiB | SRAM (**test board only**) | any size |
| `0xC000_0000` | 512 MiB | PCIe 32-bit memory window (BARs) | as the device allows |
| `0xE000_0000` | 256 MiB | PCIe ECAM: 256 buses × 1 MiB | 1, 2 or 4 B |
| `0xF000_0000` | 16 MiB | VGA VRAM | any size |
| `0xFE00_0000` | 8 B | COM1 MMIO alias (16550 registers, reg-shift 0) | bytes only |
| `0xFE00_1000` | 16 B | SYSCON: write `0x5555` power off, `0x3333 \| code << 16` fail, `0x7777` reset, `0xAAAA` pulse NMI, `0xBB00 \| type` report a machine check (type 1 corrected, 2 uncorrected) at the address in SYSCON + 8 | 4 or 8 B |
| `0xFE00_2000` | 4 B | STRAPS (read-only): bit 0 `ECC_SKIP` | 4 or 8 B |
| `0xFE00_3000` | 0x18 | SPD SMBus controller (**server**) | 4 or 8 B |
| `0xFE00_4000` | 0x300 | DDR5 memory controller (**server**) | 4 or 8 B |
| `0xFE00_5000` | 4 KiB | QPORT enqueue portal (both boards) | `enq` only |
| `0xFE00_6000` | 0x80 | QPORT control (both boards) | 8 B |
| `0xFE00_7000` | 4 KiB | IOMMU registers (both boards) | 8 B |
| `0xFE00_8000` | 0x30 | I2C controller (both boards) | 4 or 8 B |
| `0xFE00_9000` | 0x98 | PMC (both boards) | 4 or 8 B |
| `0xFE00_A000` | 4 KiB | NVRAM: firmware settings (both boards) | any size |
| `0xFE01_0000` | 0x140 | VGA registers (both boards) | 4 or 8 B |
| `0xFEE0_0000` | 1 MiB | MSI window: a 4-byte write is an interrupt message ([below](#msis-nmi-machine-checks)) | 4 B writes; reads return 0 |
| `0xFF00_0000` | 16 MiB | EEPROM (firmware); the reset vector `0xFFFF_FFF0` is its last 16 bytes | read any size; writes fault |
| `0x1_0000_0000` | 1 TiB window | DRAM (**server**): the DIMMs at the bases firmware programs | any size (scalar accesses naturally aligned) |
| `0x200_0000_0000` | 64 GiB | PCIe 64-bit memory window (prefetchable BARs) | as the device allows |

Any other address is a bus error. The DRAM window is a bus error until the
controller is online, and inside the window only the enabled DIMMs decode.
The firmware is expected to pack the DIMMs in slot order from
`0x1_0000_0000`, so the default board would have DRAM at
`0x1_0000_0000`–`0x2_FFFF_FFFF`. (Memory below 4 GiB on the server board, as a
PC has it, needs a remap the controller doesn't do yet.)

**The flash image** given on the vsim command line is placed so that its
**last** byte is at `0xFFFF_FFFF`, as PC flash images are: a 64 KiB image
occupies `0xFFFF_0000`–`0xFFFF_FFFF`, and its last 16 bytes hold the code
the core runs first. The rest of the EEPROM reads `0xFF` (erased).

## I/O ports

| Port | Device |
|---|---|
| `0x80` | POST code: the last byte written is kept (reads return it); `vsim -t` prints each code, `-v` the last one on exit |
| `0x3F8`–`0x3FF` | COM1: 16550 (THR/RBR, IER, IIR/FCR, LCR, MCR, LSR, MSR, SCR; DLAB divisor latches), transmit to `vsim`'s stdout, receive from stdin or the window. A 2- or 4-byte access goes to consecutive registers, one byte each |
| `0xCF9` | reset control: a write with bit 2 (`RST_CPU`) set resets the board (as SYSCON `0x7777`); reads return 0 |
| others | nothing decodes them: reads return all ones, writes are ignored |

The 16550 register set is National Semiconductor's (*PC16550D Universal
Asynchronous Receiver/Transmitter with FIFOs*, datasheet, 1995); the reset
control register at `0xCF9` follows Intel's PCH datasheets (e.g. *Intel
C620 Series Chipset Platform Controller Hub Datasheet*, doc. 336067, 2018,
"Reset Control Register").


## Memory

### DIMMs

Each slot holds nothing or one DDR5 RDIMM with two 32-bit subchannels, plus
8 ECC bits per subchannel on ECC modules (side-band SECDED). The
organizations follow JEDEC JESD79-5 densities:

| Size | Organization | Dies | Bank groups × banks | Rows |
|---|---|---|---|---|
| 8 GiB | 1Rx16 | 16 Gb | 4 × 4 | 64 K |
| 16 GiB | 1Rx8 | 16 Gb | 8 × 4 | 64 K |
| 32 GiB | 2Rx8 | 16 Gb | 8 × 4 | 64 K |
| 64 GiB | 2Rx8 | 32 Gb | 8 × 4 | 128 K |

| Speed | tCK | CL-tRCD-tRP | tRAS | tRC | tRFC1 (16 / 32 Gb) |
|---|---|---|---|---|---|
| DDR5-4800 | 416 ps | 40-40-40 | 32 ns | 48.64 ns | 295 / 410 ns |
| DDR5-5600 | 357 ps | 46-45-45 | 32 ns | 48.07 ns | 295 / 410 ns |

These are approximations of the JEDEC speed bins, good enough for the model.

### SPD

Every DIMM has an SPD hub with 1024 bytes laid out after JEDEC JESD400-5
(DDR5 SPD). The model fills these fields:

| Bytes | Field | Example (8 GiB, 4800, ECC) |
|---|---|---|
| 0, 1 | SPD size / revision | `0x30`, `0x10` |
| 2 | key byte: DRAM type | `0x12` DDR5 SDRAM |
| 3 | module type [3:0] | `0x01` RDIMM |
| 4 | dies per package [7:5], density [4:0] (4 = 16 Gb, 6 = 32 Gb) | `0x04` |
| 5 | column bits − 10 [7:5], row bits − 16 [4:0] | `0x00` |
| 6 | I/O width [7:5] (0 x4, 1 x8, 2 x16) | `0x40` |
| 7 | bank groups log2 [7:5], banks per group log2 [2:0] | `0x42` |
| 20–21 / 22–23 | tCKAVGmin / tCKAVGmax, ps | 416 / 1010 |
| 24–28 | CAS latencies supported (bit n = CL 20 + 2n) | |
| 30–41 | tAAmin, tRCDmin, tRPmin, tRASmin, tRCmin, tWRmin, ps | |
| 42–47 | tRFC1min, tRFC2min, tRFCsbmin, ns | 295, 160, 130 |
| 234 | package ranks − 1 [5:3] | `0x00` |
| 235 | subchannels − 1 [6:5], ECC bits [4:3] (2 = 8), subchannel width [2:0] (2 = 32) | `0x32` (`0x22` without ECC) |
| 510–511 | CRC-16 (polynomial 0x1021, init 0) over bytes 0–509 | |
| 521–550 | part number, ASCII | `VSIM-DDR5-8G-1Rx16-4800-ECC` |

### SPD SMBus controller (`0xFE00_3000`)

DDR5 SPD hubs answer at 7-bit addresses `0x50 | HID` (HID 0–7) on each
SMBus segment. The board has two segments, so slot = segment × 8 + HID.

| Offset | Register | |
|---|---|---|
| `0x00` | CMD (w) | [3:0] slot (= [3] segment, [2:0] HID), [13:4] SPD byte offset, [31] GO: read one byte |
| `0x08` | STATUS (r) | [0] BUSY, [1] DONE, [2] NACK (no DIMM) |
| `0x10` | DATA (r) | the byte read (0xFF after a NACK) |

A transaction takes 48 core cycles.

### Memory controller (`0xFE00_4000`)

64-bit registers; 32-bit accesses read or write one half.

| Offset | Register | |
|---|---|---|
| `0x000` | ID (r) | `0x0001_0000_3552_4444` ("DDR5", version 1) |
| `0x008` | STATUS (r) | [0] BUSY, [3:1] STATE (0 reset, 1 CKE, 2 MRS, 3 ZQ, 4 trained, 5 online), [4] ERR, [15:8] ERRCODE, [16] SCRUB_BUSY, [17] SCRUB_DONE |
| `0x010` | CMD (w) | 1 RESET, 2 CKE, 3 MRW, 4 ZQCAL, 5 TRAIN, 6 ONLINE |
| `0x018` | MRW | [7:0] mode register, [15:8] value, written by CMD MRW |
| `0x020` | TCK_PS | DRAM clock period, ps |
| `0x028` | TIM0 | [15:0] CL, [31:16] tRCD, [47:32] tRP, [63:48] tRAS, in clocks |
| `0x030` | TIM1 | [15:0] tRC, [31:16] tRFC, [47:32] tREFI, [55:48] tCCD_L, [63:56] tCCD_S |
| `0x040` | SCRUB (w) | 1: start the ECC init/scrub engine (online only) |
| `0x048` / `0x050` | SCRUB_DONE / SCRUB_TOTAL (r) | bytes |
| `0x058` / `0x060` | CE / UE (r) | corrected / uncorrected error counts |
| `0x068` | ERR_ADDR (r) | physical address of the last error (8-byte word) |
| `0x070` | ERR_INFO (r) | [0] CE, [1] UE, [11:8] slot |
| `0x078` | INJECT (w) | physical address \| type: 1 single-bit (CE), 2 double-bit (UE); up to 8 pending |
| `0x080` | ECC_CLEAR (w) | 1: clear CE, UE, ERR_ADDR, ERR_INFO |
| `0x100 + 16n` | SLOT_CFG n | [0] enable, [1] ECC enable, [13:8] log2 size |
| `0x108 + 16n` | SLOT_BASE n | physical base, 1 GiB aligned |
| `0x200`–`0x218` | statistics (r) | row hits, closed-row activates, row conflicts, refresh stalls |

**Bring-up** (the JESD79-5 order, much abbreviated): program TCK_PS, TIM0,
TIM1 and the slots, then RESET → CKE → MRW (one or more) → ZQCAL → TRAIN →
ONLINE, waiting for BUSY = 0 after each. A command in the wrong state sets
ERR with code 1, and only RESET clears an error. The configuration registers
are writable only before TRAIN succeeds. TRAIN checks the configuration
against each enabled DIMM's SPD and fails with:

| Code | Meaning |
|---|---|
| 1 | command out of sequence |
| 2 | TCK_PS outside a DIMM's tCKAVGmin–tCKAVGmax |
| 3 | a timing below a DIMM's minimum (CL must also be even and ≥ 20; tREFI ≤ 3.9 µs; tCCD_L ≥ max(8 nCK, 5 ns); tCCD_S ≥ 8) |
| 4 | SLOT_CFG doesn't match the DIMM (absent, wrong size, ECC on a non-ECC DIMM) |
| 5 | SLOT_BASE unaligned, outside the window, or overlapping |
| 6 | no slot enabled |

The mode-register values are recorded but not interpreted.

**ECC.** With ECC enabled on a slot, every 8-byte word carries ECC. Power-on
DRAM content is random and its ECC isn't valid, so:

- a read of a word that was never written or scrubbed is an uncorrected
  error (UE): it's counted and the random data is returned;
- an aligned 8-byte (or larger) write seeds a word's ECC; a smaller write
  into an unseeded word is a read-modify-write and also counts a UE (the word
  is then seeded);
- the **scrub engine** zeroes every ECC-enabled DIMM and seeds its ECC. It
  is accelerated for simulation: 1 MiB per core cycle, so 8 GiB takes 8192
  cycles (the real 8 GiB at DDR5-4800 is about 0.2 s). Storage is lazy, so
  scrubbing allocates no host memory;
- injected errors hit the next reads of that word. CE: the data is
  corrected and the CE count increments. UE: two data bits come back flipped
  and the UE count increments. Writing the word or scrubbing it removes the
  injection.

Each counted error is also reported to the core as a machine check (the
`mce_*` pins); what the core does with it is the core version's business.
A UE does not fault the access.

### DDR5 timing model

DRAM accesses get their bus acknowledge after a modeled latency. Every other
device still answers in the same cycle. vsim performs the access when the
core first presents it and holds `bus_ack` low for the latency. The core
side of the bus is unchanged.

- **Clocks.** One vsim cycle is 833 ps (a 1.2 GHz core). DRAM times are
  `clocks × TCK_PS`. At DDR5-4800 the core:DRAM clock ratio is 1:2.
- **Address map** inside a DIMM, low to high: 64-byte line, subchannel, bank
  group, bank, column (4 KiB of row per subchannel), rank, row.
- **Banks.** Open-page policy with per-bank state: open row, last ACT,
  earliest next command. This follows DRAMSim2's per-bank model, simplified
  to one outstanding request [Rosenfeld11].
  - row hit: CAS only;
  - closed row: ACT + tRCD;
  - conflict: PRE (no earlier than ACT + tRAS) + tRP + ACT + tRCD;
  - ACT-to-ACT in a bank: at least tRC;
  - column commands: tCCD_L apart in a bank group, tCCD_S within a rank.
- **Data.** Reads take CL, writes CWL = CL − 2, then BL16 = 8 clocks. The
  controller adds 12 ns of queue/PHY time each way (on writes, only on the
  way in: they're posted).
- **Refresh.** All-bank refresh per rank every tREFI blocks the rank for
  tRFC and closes its rows.

On the default board a closed-row read takes about 73 cycles and a row hit
about 53 (measured with vercpu's `tests/board/dram-timing.s`). `vsim -v` prints the controller
statistics on exit.

**References.** JEDEC JESD79-5, *DDR5 SDRAM* (organizations, timing
parameters, refresh, BL16, init order). JEDEC JESD400-5, *DDR5 Serial
Presence Detect (SPD) Contents* (SPD fields and CRC). [Rosenfeld11]
P. Rosenfeld, E. Cooper-Balis, B. Jacob, "DRAMSim2: A Cycle Accurate Memory
System Simulator", *IEEE Computer Architecture Letters* 10(1), 2011,
doi:10.1109/L-CA.2011.4.


## Running

```
cmake -S . -B build && cmake --build build -j
build/sim/vsim -n -b test -e prog.elf          # headless, test board, program in SRAM
build/sim/vsim -n -b test -t -e prog.elf 2>trace.txt   # per-instruction retire trace
build/sim/vsim -b test flash.bin               # VGA window, serial on stdout
```

| Option | |
|---|---|
| `[flash.bin]` | the EEPROM image, placed so its last byte is at `0xFFFF_FFFF`. Optional with `-e` or `-l` (the EEPROM is then erased: the program must put code at the reset vector itself) |
| `-b server\|test` | board model (default server) |
| `-C version` | the core version in the socket (default: the latest, `rtl/<version>`) |
| `-n` | headless: no window (serial is always on stdout) |
| `-d` | debug board: ECC_SKIP jumper fitted unless `-J ecc-check` |
| `-J ecc-skip\|ecc-check` | set the jumper explicitly |
| `-P slot\|none` | PCIe slot of the DMA test endpoint (default 0) |
| `-X slot\|none` | PCIe slot of the xHCI USB controller (default 1); a setup error if it is `-P`'s slot |
| `-K script` / `-K @file` | inject keys and mouse input on a cycle schedule ([below](#key-injection--k)), headless or in the window |
| `-m slot:size[:noecc][:5600][:badspd]` | a DIMM in a slot (size `8G`, `16G`, `32G`, `64G`; default ECC, DDR5-4800). Repeatable. The first `-m` replaces the default population; `-m none` leaves all slots empty. `badspd` corrupts the SPD CRC |
| `-c cycles` | stop after this many cycles (headless default 100 M) |
| `-H cycles` | stop once the core has been halted (`HLT`) this many cycles without an interrupt (default 1 M) |
| `-t` | trace on stderr: every retired instruction (cycle, RIP, bytes, micro-ops), every exception or interrupt (vector, error code, RIP), every POST code |
| `-v` | print the last POST code and the memory-controller statistics on exit |
| `-x` / `-s n` / `-S file.bmp` | window: close at power-off / scale of the VGA picture / save the window on exit |
| `-V file.bmp` | save the VGA picture on exit (headless too) |
| `-Y file.txt` | save the VGA text cells (25 lines) on exit |
| `-T file` | keep the debug TPM's NV state in this file (loaded if it exists, saved on exit); default: the fixed debug state, fresh every run |
| `-N file` | keep the NVRAM (firmware settings) in this file; default: erased every run |
| `-l file@addr` | copy a raw image into memory before reset (repeatable): the test board's SRAM or the EEPROM range |
| `-e file.elf` | load an ELF64 x86-64 executable (repeatable): each `PT_LOAD` segment goes to its physical address (`p_paddr`), zero-filled past `p_filesz`, with the same limits as `-l`. The entry point isn't used: the core starts at the reset vector, so the program links a jump at `0xFFFF_FFF0` (ELF: *System V Application Binary Interface, AMD64 Architecture Processor Supplement*, and the generic gABI's program headers) |

The server board's DRAM can't be preloaded: it isn't decoded until firmware
has brought the memory controller online and programmed the DIMM bases.

`vsim` exit status: 0 power off, 1 failure code from SYSCON, 2 shutdown
(triple fault; RIP on stderr), 3 cycle limit, 4 setup error, 5 thermal
shutdown (the PMC's critical trip), 6 halted (`HLT` for `-H` cycles with
nothing to wake the core). A SYSCON reset (`0x7777`) or a `0xCF9` reset
resets the core, the controller and the SMBus; DRAM contents are kept.

**Serial (COM1) always goes to stdout.** In window mode the one window is
the **monitor**: only the VGA picture, always shown at 4:3 like a monitor
(the 720×400 text mode is stretched to fill it), scaled by `-s` and
letterboxed when the window isn't 4:3; the board's state (running, powered
off, halted, shutdown, …) is in the window title. Key presses and releases
in the window go to the **USB keyboard** and mouse motion and buttons to
the **USB mouse** (on the xHCI controller); with `-X none`, text typed in
the window goes to COM1's receiver instead (Enter, Backspace, Tab and Esc
as CR, BS, TAB, ESC). Terminal input on stdin, when it is a tty, always
goes to COM1. **Pause** is the power button (PMC). `-x` closes the window at
power-off. `-S file.bmp` saves the window on exit, which also works with
`SDL_VIDEO_DRIVER=offscreen`.

Headless (`-n`), **stdin feeds COM1's receiver**: a terminal, a pipe or a
file. Bytes move in as the receive FIFO has room, so piped input isn't
lost; `\n` becomes `\r`.

### NVRAM (`0xFE00_A000`)

4 KiB of battery-backed RAM for firmware settings, on both boards, any
access size. `vsim -N file` loads it from the file (if the file exists) and
saves it there whenever software writes SYSCON and on exit. Without `-N`,
it starts erased (0xFF) every run.


### QPORT test portal (`0xFE00_5000`, control `0xFE00_6000`)

A stand-in for a device work queue, on both boards, kept from vercpu so a
future enqueue-store instruction (x86's `ENQCMD`, Intel SDM Vol. 2) can be
tested; v1 has no way to issue an `enq`. Its portal page accepts only `enq` (a 64-byte atomic
write tagged with a PASID); any other access to it is a bus error, and so is
an `enq` anywhere else. The queue holds **4** descriptors: an `enq` while it
is full gets *retry*; otherwise it is accepted and appended.

| Offset (control) | Access | Contents |
|---|---|---|
| `0x00` | ro | COUNT: descriptors queued |
| `0x08` | wo | POP: any write drops the head |
| `0x10` | ro | HEAD_PASID: the PASID of the head |
| `0x18` | ro | CAPACITY = 4 |
| `0x20` | ro | ACCEPTED: total accepted |
| `0x28` | ro | RETRIED: total refused |
| `0x40`–`0x78` | ro | HEAD: the head descriptor, 8 dwords |

Reading HEAD or HEAD_PASID with an empty queue returns 0.

QPORT also sends a completion MSI: when `MSI_ADDR` (`0x30`) is nonzero, each
POP writes `MSI_DATA` (`0x38`) to `MSI_ADDR`, normally an address in the MSI
window.

### MSIs, NMI, machine checks

A 4-byte write into the MSI window `0xFEE0_0000`–`0xFEEF_FFFF` is an
interrupt message in the x86 MSI format (Intel SDM Vol. 3A §11.11,
"Message Signalled Interrupts"): address bits 19:12 are the destination
APIC id, data bits 7:0 the vector. Only fixed delivery to one physical
destination is modelled; redirection hint, destination mode, delivery mode
and trigger mode bits are ignored. Vectors 0–15 are reserved and dropped.
The core with that APIC id (vibe86 v1 has one, id 0) gets the vector on its
`intr_*` pins; a message to an id with no core is dropped. Reads of the
window return 0; other sizes are bus errors.

SYSCON + 8 is an 8-byte read/write register holding the address for
`0xBBxx`. The DDR5 controller reports every corrected and uncorrected ECC
error it counts as a machine check with the error's address. SYSCON's
`0xAAAA` and `0xBBxx` commands let tests raise an NMI or a machine check on
either board.


## PCIe

Both boards have a PCIe root complex. Config space is **ECAM** at
`0xE000_0000`: the address of bus b, device d, function f, register r is
`0xE000_0000 + (b << 20 | d << 15 | f << 12 | r)`, with 1-, 2- and 4-byte
accesses (naturally aligned). A function that doesn't exist reads all ones;
writes to it are ignored. Header and capability layouts follow the PCI
Express Base Specification [PCIe5]; this section lists what the model
implements. The vendor ID `0x7663` is a placeholder, not PCI-SIG assigned.

| Function | What |
|---|---|
| 00:00.0 | host bridge: device `0x0001`, class 06/00/00 |
| 00:01.0 … 00:08.0 | root ports for slots 0–7: device `0x0002`, class 06/04/00, type 1 header; bus numbers (0x18–0x1A), memory base/limit (0x20/0x22), 64-bit prefetchable base/limit (0x24–0x2C), command (MSE, BME, parity, SERR, INTx disable); PCIe capability at 0x40 (root port, slot implemented, physical slot number = k) |
| b:00.0 | the endpoint in slot k, on the root port's secondary bus b; nothing else exists below a root port |

A root port forwards a memory access to its slot when its command register
has memory space enabled and the address is inside its memory window or
its prefetchable window; the endpoint answers when its own memory space is
enabled and a BAR covers the address. Anything unclaimed in the two BAR
windows is a bus error.

**DMA** from an endpoint needs bus master enabled at the endpoint and its
root port, and a programmed secondary bus number (the requester ID is
`secondary bus << 8`). Every DMA request goes through the **IOMMU**
(registers at `0xFE00_7000`; its programming model, page-table and
device-context formats are vercpu's, `~/vercpu/docs/isa/21-iommu.md`, not
Intel VT-d's yet) to physical memory: SRAM
or DRAM, plus EEPROM reads. There are no caches, so DMA is coherent. An
endpoint's **MSI** is a 4-byte DMA write of its MSI data to its MSI
address, normally in the MSI window, through the same IOMMU.

### The DMA test endpoint

`vsim -P slot` puts it in slot 0–7 (default 0), `-P none` leaves every
slot empty.

| Config | Contents |
|---|---|
| 0x00 | vendor `0x7663`, device `0x0D0A`; class 08/80/00 |
| 0x10 / 0x14 | BAR0: a 64-bit, non-prefetchable 4 KiB memory BAR |
| 0x40 | MSI capability: 64-bit address, one vector; control 0x42 (bit 0 enable), address 0x44/0x48, data 0x4C |
| 0x60 | PCIe capability (endpoint) |
| 0x100 | PASID extended capability: 20-bit PASIDs, privileged mode; control 0x106 (bit 0 enable, bit 2 privileged enable) |
| 0x108 | page request extended capability: control 0x10C (bit 0 enable), capacity 1 |

BAR0 registers (8-byte accesses):

| Offset | Register |
|---|---|
| 0x00 | ID (ro): `0x54414D44` ("DMAT") |
| 0x08 / 0x10 / 0x18 | SRC / DST (I/O virtual addresses) / LEN (bytes) |
| 0x20 | PASID: [19:0] PASID, [30] privileged, [31] PASID valid (used only if the PASID capability enables it) |
| 0x28 | CTRL: write [0] start, [1] MSI when the transfer ends, [2] fill DST with PATTERN instead of copying |
| 0x30 | STATUS (ro): 0 idle, 1 busy, 2 done, 3 error, 4 waiting for a page response |
| 0x38 | DONE (ro): bytes transferred |
| 0x40 | PRGI (ro): the page-request group index of the last page request |
| 0x48 | PATTERN |

The endpoint moves up to 64 bytes per core cycle, each request inside one
4 KiB page of both buffers. A not-present page with page requests enabled
(both in the endpoint and the IOMMU device context) makes it wait (status
4) for a `PAGE_RESP`: on success it retries, on failure it stops with an
error. Writes to SRC … PATTERN are ignored while it is busy or waiting.

## USB (xHCI)

An xHCI host controller after Intel, *eXtensible Host Controller Interface
for Universal Serial Bus (xHCI)*, revision 1.2, May 2019
([xHCI 1.2](https://www.intel.com/content/www/us/en/io/universal-serial-bus/extensible-host-controler-interface-usb-xhci.html)),
with a HID boot keyboard and a HID boot mouse after USB-IF, *Device Class
Definition for Human Interface Devices (HID)*, version 1.11
([HID 1.11](https://www.usb.org/document-library/device-class-definition-hid-111),
incl. Appendix B, the boot interface) and the USB-IF *HID Usage Tables*
([HUT](https://usb.org/document-library/hid-usage-tables-15), Keyboard/Keypad
page 0x07). Standard requests follow USB-IF, *Universal Serial Bus
Specification*, revision 2.0, chapter 9. Model: `sim/xhci.c`.

`vsim -X slot` puts the controller in PCIe slot 0–7 (default **1**, on both
boards), `-X none` leaves it out; `-X` and `-P` can't name the same slot.

| Config | Contents |
|---|---|
| 0x00 | vendor `0x7663` (placeholder), device `0x0C30`; class 0C/03/30 (xHCI) |
| 0x10 / 0x14 | BAR0: 64-bit, non-prefetchable, **64 KiB** |
| 0x40 | MSI capability, as the DMA test endpoint's (64-bit, one vector) |
| 0x60 / 0x61 | SBRN `0x20` (USB 2.0), FLADJ `0x20` |
| 0x70 | PCIe capability (endpoint) |

### Registers (BAR0)

32-bit registers; 8-byte accesses are two 32-bit ones (low half first);
byte and 16-bit reads also work. Unlisted offsets read 0 and ignore writes.

| Offset | Register | Model |
|---|---|---|
| `0x00` | CAPLENGTH / HCIVERSION | `0x20`, `0x0100` |
| `0x04` | HCSPARAMS1 | MaxSlots 8, MaxIntrs 1, MaxPorts 4 |
| `0x08` | HCSPARAMS2 | IST 0, ERST Max 2 (≤ 4 segments), no scratchpads |
| `0x10` | HCCPARAMS1 | AC64 = 1, CSZ = 0 (32-byte contexts), PPC = 0 (ports always powered), xECP = `0x800` |
| `0x14` / `0x18` | DBOFF / RTSOFF | `0x2000` / `0x1000` |
| `0x800` | Supported Protocol capability (xHCI 1.2 §7.2) | "USB " 2.00, ports 1–4, default speed IDs, slot type 0 |
| `0x20` | USBCMD | R/S, HCRST, INTE (others stored) |
| `0x24` | USBSTS | HCH, HSE, EINT, PCD (RW1C), CNR (64 cycles after HCRST), HCE |
| `0x28` | PAGESIZE | 1 (4 KiB) |
| `0x34` | DNCTRL | stored |
| `0x38` | CRCR | RCS, CS, CA, CRR; the pointer reads 0 |
| `0x50` | DCBAAP | |
| `0x58` | CONFIG | MaxSlotsEn |
| `0x420 + 0x10 (n − 1)` | PORTSC n (1–4) | CCS, PED, PR, PLS, PP, speed, PIC, LWS, CSC/PEC/WRC/OCC/PRC/PLC/CEC (RW1C), wake enables |
| `0x1000` | MFINDEX | microframes (125 µs = 150,000 cycles at 1.2 GHz) while running |
| `0x1020` | IMAN | IP (RW1C), IE |
| `0x1024` / `0x1028` | IMOD / ERSTSZ | stored / segments |
| `0x1030` | ERSTBA | writing it (re)initializes the event ring |
| `0x1038` | ERDP | dequeue pointer, EHB (RW1C) |
| `0x2000 + 4 n` | doorbell n | 0: command ring (target 0); slot n: target = DCI |

**Ports.** Port 1 has the keyboard and port 2 the mouse, both full speed
(PORTSC speed 1); ports 3 and 4 are empty. After HCRST a connected port
has CCS and CSC, PED = 0, PLS = Polling; an empty one PLS = RxDetect.
Writing PR resets the port for 2000 cycles (sim-scaled), then PR = 0,
PED = 1, PLS = U0 and PRC = 1, and the device is back in its default
state (address 0, unconfigured). A change bit going 0 → 1 while the
controller runs sets USBSTS.PCD and posts a Port Status Change Event;
setting R/S posts one for every port with a change bit already set.

**Event ring.** One interrupter. The segment table (ERSTSZ ≤ 4 entries,
each 16–4096 TRBs) is read through DMA. A bad table sets USBSTS.HCE. Events
are written with the producer cycle state, which toggles after the last
segment. The ring is full when the next position would be ERDP; events
then wait in the controller (64 deep) until software advances ERDP. When
events are pending and EHB = 0 the controller sets EHB, IMAN.IP and
USBSTS.EINT, and sends the MSI if IMAN.IE and USBCMD.INTE are set. Polling
needs no interrupts.

**Commands** (completion codes as in §6.4.5): No Op; Enable Slot (9 No
Slots Available); Disable Slot; Address Device incl. BSR (Default state,
no address) — input context A0 and A1, root hub port from the slot context,
4 USB Transaction Error if no device answers there, 19 Context State Error
from the wrong slot state; Configure Endpoint incl. DC (Drop/Add flags);
Evaluate Context (A0: max exit latency, interrupter target; A1: EP0 max
packet size); Reset Endpoint (Halted → Stopped); Stop Endpoint (a TD in
progress gets a Stopped transfer event); Set TR Dequeue Pointer (Stopped
or Error). Other command types complete with 5 TRB Error, and 11 Slot Not
Enabled / 12 Endpoint Not Enabled as the spec says. Command ring
stop/abort (CRCR.CS/CA) posts 24 Command Ring Stopped. The controller
writes the output device context (slot state, address, context entries,
endpoint state, TR dequeue pointer when stopped or halted). Contexts are
32 bytes.

**Transfers.** Link TRBs (toggle cycle), No Op, and Event Data TRBs
(EDTLA) on any transfer ring. EP0: Setup (IDT, 8 bytes), Data (IN/OUT, IDT,
chained Normal TRBs) and Status stages; a short IN data stage gives
13 Short Packet (with ISP or IOC) and skips to the Status stage; a STALL
from the device gives 6 Stall Error on the data or status TRB and halts
EP0 (Reset Endpoint and Set TR Dequeue Pointer recover it). Interrupt IN:
a Normal TRB waits (NAK) until the device has a report; a packet shorter
than the TRB completes it with 13 Short Packet and the residual length;
TDs can span chained TRBs. Transfer events carry the residual length. All
memory accesses are DMA through `pcie_dma` (bus master enable and the
IOMMU apply; a failed DMA sets USBSTS.HSE and halts the controller). The
controller fetches at most 8 TRBs per cycle.

### The devices

| | Keyboard (port 1) | Mouse (port 2) |
|---|---|---|
| Device | USB 1.1, full speed, class 0, bMaxPacketSize0 8, VID `0x7663` (placeholder), PID 1 | the same, PID 2 |
| Strings | 1 "vibe86", 2 "USB Keyboard" (language `0x0409`) | 1 "vibe86", 2 "USB Mouse" |
| Configuration 1 | one interface: class 3 (HID), subclass 1 (boot), protocol 1; HID 1.11 descriptor; EP 0x81 interrupt IN, 8 bytes, bInterval 10 | protocol 2, otherwise the same |
| Report descriptor | HID 1.11 Appendix B.1 (63 bytes) | Appendix B.2 (50 bytes) |
| Report | 8 bytes: modifiers, reserved, 6 key usages (HUT page 7); ErrorRollOver (`0x01` × 6) with more than 6 keys | 3 bytes: buttons (1 left, 2 right, 4 middle), dx, dy (each clamped to ±127; the rest stays for the next report) |

Requests: GET_STATUS (device, interface, endpoint), CLEAR/SET_FEATURE
(DEVICE_REMOTE_WAKEUP; CLEAR_FEATURE ENDPOINT_HALT), GET_DESCRIPTOR
(device, configuration, string; HID and report descriptors with an
interface recipient), GET/SET_CONFIGURATION, GET/SET_INTERFACE (alternate
0), and the HID class requests GET_REPORT, GET/SET_IDLE, GET/SET_PROTOCOL
and SET_REPORT (the keyboard's LED output report). Anything else STALLs.
The keyboard queues one report per change (32 deep); with an idle rate set
(default 500 ms, HID 1.11 §7.2.4; one idle unit = 4 ms = 4.8 M cycles) it
resends its report. The mouse accumulates motion until the host fetches a
report and queues button changes so that a click isn't lost. Devices send
nothing until configured.

**Input.** In the window, SDL scancodes are already HID usages, so key
presses and releases go straight to the keyboard (Pause excepted); mouse
motion is relative window pixels. When the window loses the focus every
key and button is released.

### Key injection (`-K`)

`-K "items"` or `-K @file`: whitespace-separated items `delay:key`, run on
the board's cycle counter in headless and window mode. `delay` is in cycles
after the previous item ended (decimal, suffix `k` = 10³ or `M` = 10⁶).
`key` is:

| Key | Does |
|---|---|
| `F1`–`F12` `Up` `Down` `Left` `Right` `Enter` `Esc` `Tab` `Backspace` `Space` `Home` `End` `PgUp` `PgDn` `Insert` `Delete` `CapsLock` `LShift` `RShift` `LCtrl` `RCtrl` `LAlt` `RAlt` `LGui` `RGui` (any case) | press, release 100,000 cycles later |
| one character, e.g. `a`, `A`, `7`, `?` | the same, with Shift if a US keyboard needs it |
| `text:string` | each character in turn, 100,000 cycles apart (no spaces: use `Space`) |
| `+key` / `-key` | press only / release only (hold Shift, hold a key for typematic repeat) |
| `mouse:dx,dy` | mouse motion |
| `button:1`, `2`, `3` | click the left, right, middle button |

Without a controller (`-X none`) key presses go to COM1 instead: the
character, CR for Enter, BS, TAB, ESC, and xterm sequences for the others
(`ESC [ A` for Up, `ESC O P` for F1, `ESC [ 15 ~` for F5, …).

```
build/sim/vsim -b test -K "20M:F2 2M:Down 2M:Enter 1M:text:hello" flash.bin
```

## On-board devices: VGA, I2C, TPM, PMC

All four are on both boards; their registers are little-endian and, except
VRAM and the palette, 8-byte registers that also accept 4-byte accesses.

### VGA (`0xFE01_0000`, VRAM `0xF000_0000`)

| Offset | Access | Register |
|---|---|---|
| `0x00` | ro | ID = `0x30414756` ("VGA0") |
| `0x08` | rw | MODE: 0 off, 1 text 80×25, 2 graphics 640×480, 3 graphics 800×600 |
| `0x10` / `0x18` / `0x20` | ro | WIDTH / HEIGHT in pixels (text: 720 × 400, 80 × 25 cells of 9 × 16 dots) / STRIDE in bytes (text: 160 per row of cells; graphics: width × 4) |
| `0x28` | rw | BASE: VRAM offset of the visible page (16-byte aligned) — page flipping |
| `0x30` | rw | CURSOR: [7:0] column, [15:8] row, [16] visible |
| `0x38` | wo | TTY: teletype one character at the cursor in TTYATTR: CR, LF, BS, wraps and scrolls |
| `0x40` | rw | TTYATTR (default `0x07`) |
| `0x48` | wo | CLEAR: text: every cell = space in this attribute, cursor home; graphics: every pixel of the visible page = this colour |
| `0x50` | ro | FRAME: frames since power-on, one per 1,000,000 cycles (sim-scaled) |
| `0x58` | ro | VRAM size (16 MiB) |
| `0x100`–`0x13C` | rw | palette: 16 × 32-bit `0x00RRGGBB` (default: the 16 CGA colours) |

Text mode: cell (row r, column c) is 2 bytes at `BASE + 160 r + 2 c`:
character (code page 437, the VGA 8×16 font: SeaBIOS's `vgafont16`, from
Joseph Gil's public-domain fntcol16 collection, in `sim/vgafont.h`; like VGA
hardware, the ninth dot of a cell repeats the eighth for the line-drawing
characters `0xC0`–`0xDF` and is blank for the others), then attribute (foreground [3:0], background
[7:4], palette indices). The `-Y` dump writes box drawing as `+ - |`, blocks
as `#` and arrows as `^ v < >`. Graphics: pixel
(x, y) is a 32-bit `0x00RRGGBB` at `BASE + y × STRIDE + 4x`.

### I2C controller (`0xFE00_8000`)

A message engine: queue the bytes to write, then one command does START,
address + W, the bytes, and (if reading) a repeated START, address + R, the
reads, STOP. The bus is busy for (1 + writes + reads) × 32 cycles.

| Offset | Access | Register |
|---|---|---|
| `0x00` | wo | CMD: [6:0] address, [15:8] bytes to write (from TX), [23:16] bytes to read (≤ 64), [31] GO |
| `0x08` | ro | STATUS: [0] busy, [1] done, [2] NACK (no device), [15:8] bytes written |
| `0x10` | wo | TX: queue one byte (64 deep) |
| `0x18` | ro | RX: the next byte read |
| `0x20` | wo | RESET: empty TX/RX, clear STATUS |
| `0x28` | ro | LEVEL: [7:0] TX bytes queued, [15:8] RX bytes left |

| Address | Device |
|---|---|
| `0x2E` | the debug TPM (below) |
| `0x2F` | the fan / hardware monitor (below) |
| `0x48` | LM75-style temperature sensor (the board temperature), registers as in the NXP LM75B datasheet: pointer 0 temperature (11-bit, 0.125 °C, MSB first), 1 configuration, 2 Thyst, 3 Tos (9-bit, 0.5 °C; defaults 75 °C / 80 °C). The OS output pin isn't wired |

Any other address NACKs.

**Fan / hardware monitor (`0x2F`)** — byte registers with an auto-incrementing
pointer (the first written byte):

| Reg | Access | Contents |
|---|---|---|
| `0x00` / `0x01` | ro | ID `0x56`, revision 1 |
| `0x02` | rw | [0] automatic mode (default 1) |
| `0x03` | ro | number of fans (2) |
| `0x04` | ro | fan failed, bit per fan: duty ≥ 32 and below 300 RPM for 1,000,000 cycles |
| `0x05` / `0x06` | ro | die / board temperature, °C (signed) |
| `0x10` / `0x11` | rw | PWM duty of fan 0 / 1 (0–255; writes ignored in automatic mode) |
| `0x20`–`0x23` | ro | tachometers: fan 0 RPM (MSB, LSB), fan 1 RPM |
| `0x30` / `0x31` / `0x32` | rw | automatic curve: T1, T2 (°C), PWM1: duty PWM1 up to T1, rising linearly to 255 at T2 (defaults 40, 80, 64) |

A fan's RPM follows 12000 × duty / 255 with a 200,000-cycle time constant
(sim-scaled).

### TPM (`0x2E` on the I2C bus)

A TPM 2.0 from libtpms, linked into vsim, behind the I2C register interface
of the TCG *PC Client Platform TPM Profile (PTP) Specification*; commands
are those of the TCG *TPM 2.0 Library Specification*.

> **Debug TPM — not secure.** It is manufactured from a fixed permanent
> state, `sim/tpm-debug.permall` (made once by `sim/tpmgen.c`, embedded in
> vsim as `sim/tpm_debug_state.h`), so its endorsement, platform and storage
> seeds — and every primary key derived from them — are the same on every
> run and published in this repository. vsim prints a warning when the TPM
> starts. With `-T file` the NV state is loaded from and saved to that file;
> without it every run starts from the debug state.

Implemented registers (locality 0 only; one-byte register address):
`TPM_LOC_SEL` 0x00, `TPM_ACCESS` 0x04 (requestUse, activeLocality,
tpmRegValidSts), `TPM_INT_ENABLE` 0x08 (stored; no interrupts are raised),
`TPM_INT_CAPABILITY` 0x14 (0), `TPM_STS` 0x18 (stsValid, commandReady,
tpmGo, dataAvail, Expect, selfTestDone, responseRetry; burstCount 32;
tpmFamily = TPM 2.0), `TPM_DATA_FIFO` 0x24, `TPM_I2C_INTERFACE_CAPABILITY`
0x30 (only the interface type), `TPM_I2C_DEVICE_ADDRESS` 0x38,
`TPM_DATA_CSUM_ENABLE` 0x40 (stored; no checksums), `TPM_DID_VID` 0x48 =
`0x0001FFFF` (placeholder IDs, not an assigned vendor), `TPM_RID` 0x4C = 1.
A board reset is `TPM_Init`: the TPM must get `TPM2_Startup` again.

Command flow: `TPM_ACCESS` ← requestUse; `TPM_STS` ← commandReady; write
the command to the FIFO in bursts of ≤ 32 bytes; `TPM_STS` ← tpmGo; poll
`TPM_STS` for dataAvail; read the response (its size is in the header);
`TPM_STS` ← commandReady.

### PMC (`0xFE00_9000`)

| Offset | Access | Register |
|---|---|---|
| `0x00` | ro | ID = `0x30434d50` ("PMC0") |
| `0x08` | wo | CMD: 1 power off, 2 reset, 3 suspend, 4 power-button press (for tests) |
| `0x10` | ro | RESET_CAUSE: 0 power-on, 1 software (SYSCON or PMC), 3 watchdog |
| `0x18` | rw1c | STATUS: [0] power button, [1] hot trip, [2] woke from suspend, [3] suspended (ro) |
| `0x20` | ro | die temperature, m°C |
| `0x28` / `0x30` | rw | hot / critical trip, m°C (defaults 85000 / 105000) |
| `0x38` / `0x40` | rw | MSI address / data for PMC events (0: none) — an address in the MSI window |
| `0x48` | ro | VCORE, mV (850, drooping to 800 when hot) |
| `0x50` / `0x58` / `0x60` | ro | VDDQ 1100, 12 V 12000, 3.3 V 3300 (mV) |
| `0x68` | rw | temperature offset (m°C, signed; a test hook added to every reading) |
| `0x70` | rw | watchdog timeout in cycles (0 = off); expiry resets the board (cause 3) |
| `0x78` | wo | watchdog kick |
| `0x80` | rw | fan-failure injection, bit per fan (test hook) |
| `0x88` | rw | wake timer for suspend, cycles (0 = only the power button wakes) |
| `0x90` | ro | board temperature, m°C |

**Thermal model** (every 1024 cycles): the die approaches
25 °C + (20 + 70 a) / (1 + 1.5 f) °C with a 2,000,000-cycle time constant,
where a is the bus accesses per cycle (0–1) and f the mean fan speed
(fraction of 12000 RPM). The board temperature is 40 % of the way from 25 °C
to the die's. Crossing the hot trip sets STATUS.hot and sends the MSI
(re-armed 5 °C below); the critical trip powers the board off (vsim exits 5).
Power-button presses, hot trips and wake-ups each send the MSI.

**Suspend** stops the core's clock (the board keeps running); the power
button (Pause in window mode, CMD 4 is only usable while running) or the wake
timer resumes the core where it stopped, with STATUS.wake set.

## References

- [SDM] Intel Corporation, *Intel 64 and IA-32 Architectures Software
  Developer's Manual*, Combined Volumes 1–4, order no. 325462, September
  2026 edition. <https://www.intel.com/sdm>. Vol. 3A: reset state and the
  reset vector (ch. 10, "Processor Management and Initialization"), MSI
  format (§11.11), exceptions and the triple-fault shutdown (ch. 6).
- [JESD79-5] JEDEC, *DDR5 SDRAM*, JESD79-5, 2020 (and later revisions).
  <https://www.jedec.org/standards-documents/docs/jesd79-5>
- [JESD400-5] JEDEC, *DDR5 Serial Presence Detect (SPD) Contents*,
  JESD400-5, 2021. <https://www.jedec.org/standards-documents/docs/jesd400-5>
- [JESD300-5] JEDEC, *SPD5 Hub and Serial Presence Detect Device with
  Integrated Temperature Sensor*, JESD300-5, 2020.
  <https://www.jedec.org/standards-documents/docs/jesd300-5>
- [Rosenfeld11] P. Rosenfeld, E. Cooper-Balis, B. Jacob, "DRAMSim2: A Cycle
  Accurate Memory System Simulator", *IEEE Computer Architecture Letters*
  10(1), 2011, doi:10.1109/L-CA.2011.4.
- [PCIe5] PCI-SIG, *PCI Express Base Specification*, Revision 5.0, 2019.
  <https://pcisig.com/specifications>
- [xHCI 1.2] Intel Corporation, *eXtensible Host Controller Interface for
  Universal Serial Bus (xHCI)*, Revision 1.2, May 2019.
- [HID 1.11] USB-IF, *Device Class Definition for Human Interface Devices
  (HID)*, Version 1.11, 2001; [HUT] USB-IF, *HID Usage Tables*, 1.5, 2024;
  USB-IF, *Universal Serial Bus Specification*, Revision 2.0, 2000.
  <https://www.usb.org/documents>
- [PTP] Trusted Computing Group, *TCG PC Client Platform TPM Profile (PTP)
  Specification*, Family "2.0"; [TPM2] TCG, *Trusted Platform Module Library
  Specification*, Family "2.0". <https://trustedcomputinggroup.org/resources/>
- [16550] National Semiconductor, *PC16550D Universal Asynchronous
  Receiver/Transmitter with FIFOs*, datasheet, 1995.
- [C620] Intel Corporation, *Intel C620 Series Chipset Platform Controller
  Hub Datasheet*, document 336067, 2018 (reset control register `0xCF9`).
- [LM75B] NXP Semiconductors, *LM75B Digital temperature sensor and thermal
  watchdog*, product data sheet, rev. 6.1, 2015.
- [ELF] *System V Application Binary Interface, AMD64 Architecture Processor
  Supplement* (H. J. Lu, M. Matz, M. Girkar, J. Hubička, A. Jaeger, M.
  Mitchell, eds.), version 1.0, 2018. <https://gitlab.com/x86-psABIs/x86-64-ABI>
