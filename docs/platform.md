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
| DDR5 channels | the die's DDR5 pins: 2 channels × 2 subchannels (32 DQ + 8 CB), 2 DIMM slots per channel (docs/die-interface.md) | the memory controllers are RTL on the die (`rtl/uncore/ddr5_sub.sv`, D-014); `ddr5.c`: the DIMMs at the pin level, populated with `-m` |
| SPD SMBus / I3C | the sideband to the DIMMs' SPD hubs (JESD300-5 hub, JESD400-5 contents) | the die's SPD pins, mastered by its memory init engine (`meminit.sv`); `ddr5.c`/`spd.c`: the hubs at 50h–53h |
| PCIe lanes | root ports, x16/x8 bifurcation | `pcie.c`: host bridge + 8 root ports (one per slot), ECAM, BAR windows, MSI; `iommu.c` in front of DMA |
| eSPI / LPC to the board | the legacy I/O devices of a PC: COM ports, POST code port, reset control | `board.c` `board_io`: COM1 (16550) at `0x3F8`, POST code port `0x80`, reset control `0xCF9` |
| SPI flash | boot firmware, mapped below 4 GiB | `board.c`: 16 MiB EEPROM at `0xFF00_0000`, reset vector `0xFFFF_FFF0` |
| PECI / thermal (THERMTRIP#, PROCHOT#) | package temperature to the BMC, thermal shutdown | `pmc.c`: die and board temperatures, hot and critical trips (critical: power off, vsim exits 5) |
| SMBus / I2C (board management) | TPM, fan controller, temperature sensors | `i2c.c`: I2C controller at `0xFE00_8000`; TPM (`tpm.c`, libtpms), fan monitor, LM75 |
| RESET#, PWRGOOD | cold and warm reset | `vsim` holds the core's `rst_n` low for one clock at power-on and on SYSCON/`0xCF9` resets; the PMC records the reset cause |
| NMI | the board's NMI pin | `nmi` core pin, raised by SYSCON `0xAAAA` |
| MCERR#, CATERR# | machine-check signalling | `mce_*` core pins: the DDR5 controller's corrected/uncorrected ECC errors, or SYSCON `0xBBxx` |
| MSIs (inbound memory writes to `0xFEEx_xxxx`) | interrupt messages from PCIe devices and the IOMMU, delivered to a local APIC | `board.c` `msi_write`: the destination APIC id and vector go to the core's `intr_*` pins |

### The die's pins

The die (`rtl/<version>/<version>_die.sv`: the core and `rtl/uncore/`)
reaches the system only through pins (docs/die-interface.md, D-014): the
DDR5 channels, the SPD SMBus, an I/O link to the I/O die (split
transactions over 64-bit flits: everything that isn't DRAM — MMIO, ports,
the flash, other PCI configuration — and device DMA into DRAM), JTAG, the
interrupt sideband (INTR/INTA, NMI, SMI, MCERR) and a trace port (an
instruction retired, an exception or interrupt delivered, shutdown,
halted). `sim/iod.c` is the I/O die; the board's devices behind it run in a
coroutine that a DMA read suspends until the die's completion returns.

Inside the die the core reaches the fabric over one request/acknowledge
bus (`bus_*`: 1-16 bytes, or a 64-byte line read of 512 bits for the
caches' fills) and keeps two caches in front of it (D-018): a 64 KiB
instruction cache and a 256 KiB write-through data cache, both 64-byte
lines, physically tagged, for DRAM (outside A_0000h-F_FFFFh) and the flash.
The fabric reports every DRAM write (any master: the core, the JTAG debug
module, device DMA) to the core on a snoop port, which keeps both caches
coherent; software never needs to flush them.

Memory is ready before the core runs: the die's init engine (the stand-in
for AMD's PSP) reads the SPDs, initializes the DRAMs (JEDEC power-up
sequence) and posts D0h–D3h on port 80h, then releases the core; errors
post E1h–E4h and the core stays in reset. With the `meminit_fast` strap
(vsim `-F 1`, the default) the waits are scaled by 1/1024 and the DIMMs
start cleared (standing in for the platform's ECC memory clear).

## The boards

Both boards (`-b server`, the default, and `-b test`) carry the same
devices: four DIMM slots on the die's channels (default one 16 GiB 1Rx8
ECC DIMM in slot 0, `-m slot:size`), a 16 MiB EEPROM (SPI flash), COM1,
PCIe (8 slots, IOMMU, the DMA test endpoint in slot 0 `-P`, xHCI in slot 1
`-X` with a USB keyboard, mouse and optional floppy `-U`), VGA (80×25 text,
640×480 / 800×600 32 bpp, an SDL3 window), I2C (TPM 2.0 debug, fan monitor,
LM75) and a PMC (power, reset cause, rails, thermal trips, watchdog,
suspend, 2 fans). The test board has the ECC_SKIP strap fitted.

## Physical map

| Address | Size | Device | Access |
|---|---|---|---|
| `0x0000_0000` | to TOLM (2 GiB) | DRAM (the die's DIMMs); A_0000h–B_FFFFh is SMRAM for the core in SMM, C_0000h–F_FFFFh follows the PAM registers (the die's fabric decodes both) | any size within a 64-byte line |
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
| `0x1_0000_0000` | DRAM size − TOLM | DRAM above TOLM, hoisted above 4 GiB (as AMD's data fabric) | any size within a 64-byte line |
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

The memory system is on the die (D-014): two DDR5 channels of two
subchannels each, an RTL controller per subchannel, an init engine that
plays AMD's PSP, and the fabric's map (DRAM below TOLM and hoisted above 4
GiB). The DIMMs are pin-level C models (`sim/ddr5.c`) that check every
command and timing against Micron's DDR5 core data sheet and count
violations; their organisation, address map, SPD contents and ECC are in
docs/ddr5.md and sim/ddr5.h, the controller's in docs/die-interface.md.

## Running

```
cmake -S . -B build && cmake --build build -j
build/sim/vsim -n -b test -e prog.elf          # headless, test board, program in DRAM
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
| `-l file@addr` | copy a raw image into memory before reset (repeatable): DRAM (through the DIMMs' backdoor, at the address the fabric maps) or the EEPROM range |
| `-e file.elf` | load an ELF64 x86-64 executable (repeatable): each `PT_LOAD` segment goes to its physical address (`p_paddr`), zero-filled past `p_filesz`, with the same limits as `-l`. The entry point isn't used: the core starts at the reset vector, so the program links a jump at `0xFFFF_FFF0` (ELF: *System V Application Binary Interface, AMD64 Architecture Processor Supplement*, and the generic gABI's program headers) |

`-m slot:size` populates the DIMM slots (slot = channel × 2 + DIMM, 16G or
32G); `-F 0|1` sets the die's `meminit_fast` strap (default 1).

`vsim` exit status: 0 power off, 1 failure code from SYSCON, 2 shutdown
(triple fault; RIP on stderr), 3 cycle limit, 4 setup error, 5 thermal
shutdown (the PMC's critical trip), 6 halted (`HLT` for `-H` cycles with
nothing to wake the core). A SYSCON reset (`0x7777`) or a `0xCF9` reset
resets the die (its init engine runs again); DRAM contents are kept.

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
Intel VT-d's yet) and then over the I/O link into the die, whose fabric
serves it from DRAM (D-014; anything else is an unsupported request).
There are no caches, so DMA is coherent. An
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
