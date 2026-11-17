# vibe86 design decisions

Each decision has an id that commits, code comments and the thesis can
refer to. Superseded decisions stay, marked as such.

## D-001 (superseded by D-007): v1 resets into flat 64-bit mode

~~To bootstrap quickly, v1 starts at 0xFFFF_FFF0 in a non-architectural flat
64-bit state.~~ Superseded: the processor has segmentation, rings and paging
from the start (D-007).

## D-002: the Intel SDM's pseudocode is the specification

The microcode is compiled from the Operation sections of the Intel 64 and
IA-32 Architectures Software Developer's Manual, Vol. 2 (order no. 325383;
extracted from revision 093US, September 2026) by `tools/ucc`. Where the
manual's text can't be compiled as written, a per-version override
(`tools/ucc/targets/<v>/overrides/`) replaces that section and says why.
This follows Reid, "Trustworthy Specifications of ARM v8-A and v8-M System
Level Architecture", FMCAD 2016, doi:10.1109/FMCAD.2016.7886675.

## D-003: microcode by partial evaluation, one routine per opcode-table row

Each opcode-table row (and each operand kind it allows: register or
memory) gets its own routine, with the row's static facts folded in (Futamura
1971, doi:10.1023/A:1010095604496; Jones, Gomard & Sestoft, *Partial
Evaluation and Automatic Program Generation*, 1993). The decoder's tables
are derived from the same rows.

## D-004: microcode is versioned with the core

`tools/ucc/targets/<vN>/` is the microcode of core `rtl/<vN>`: its micro-op
ISA, its instruction list, its overrides and routines. `ucc --target`
defaults to the latest version.

## D-005: precise exceptions by checkpointing the working state

Micro-ops update a working copy of the GPRs, RFLAGS and RIP; END commits it
and a fault discards it (Hwu & Patt, "Checkpoint Repair for Out-of-order
Execution Machines", ISCA 1987, doi:10.1145/30350.30352). REP string
iterations commit individually.

## D-006: the board and socket

vsim's board model comes from vercpu (sim/), re-mapped for x86
(docs/platform.md). The DDR5 controller, SPD SMBus, PCIe root complex and
IOMMU are the package's uncore; the core reaches them over one core-uncore
bus.

## D-007: the ISA target is AMD EPYC "Venice" (Zen 6)

The processor implements the instruction set of AMD's EPYC Venice, from
reset:

- real mode at reset (CS.base = FFFF_0000H, IP = FFF0H), protected mode,
  IA-32e (compatibility and 64-bit) mode; segmentation with descriptor
  tables, rings 0-3, SYSCALL/SYSRET, SYSENTER/SYSEXIT;
- paging: 32-bit, PAE, 4-level and 5-level (LA57), PCID/INVPCID, NX, SMEP,
  SMAP, PKU, UMIP, global pages, A/D bits;
- x87, MMX, SSE-SSE4.2, SSE4A, AVX, AVX2, FMA, F16C, AES, VAES, PCLMULQDQ,
  VPCLMULQDQ, GFNI, SHA, BMI1/2, ADX, ABM, MOVBE, CX16, RDRAND/RDSEED,
  FSGSBASE, RDPID, MOVDIRI/MOVDIR64B, CLFLUSHOPT, CLWB, CLZERO, XSAVE
  (OPT/C/S);
- AVX-512 F, CD, BW, DQ, VL, IFMA, VBMI, VBMI2, VNNI, BITALG, VPOPCNTDQ,
  BF16, VP2INTERSECT, FP16; AVX-IFMA, AVX-NE-CONVERT, AVX-VNNI-INT8;
  AVX512-BMM;
- CET shadow stacks, SVM (with NPT).

The feature list is the EPYC-Venice CPU model in QEMU's
`target/i386/cpu.c` (patch series "[PATCH v2 0/4] Add Support for AMD EPYC
Zen 6 CPU Model", qemu-devel, 25 Aug 2026), which is EPYC-Turin plus
AVX512-FP16, AVX-IFMA, AVX-NE-CONVERT, AVX-VNNI-INT8, AVX512-BMM, ERAPS,
TSC_ADJUST and CET_SS. `rtl/common/cpuid.toml` lists it (D-013), each
feature with the first core version that implements it; CPUID reports
exactly what a version implements, so partial versions stay honest.

SVM and AVX512-BMM are AMD-only: their semantics come from the AMD64
Architecture Programmer's Manual (Vol. 2 "System Programming", Vol. 3/4
instruction references), not the Intel SDM. Both are deferred until the rest
of the ISA is done.

## D-008: microcode-heavy system architecture

The hardware keeps a TLB, segment descriptor caches and a micro-trap
mechanism; page walks, descriptor loads, privilege checks and exception
delivery are microcode routines (as the Intel 80386 did segment loads in
microcode). This keeps the RTL small and puts the architecture in one place:
the compiled specification.

## D-009 (revised): v1 has a floating-point datapath in RTL

~~v1's floating-point micro-ops are computed by Berkeley SoftFloat through
DPI-C.~~ Revised: v1 computes floating point in RTL (`rtl/v1/v1_fp_pkg.sv`),
so the microcode stays one micro-op per FP operation instead of emulating
IEEE 754 in micro-ops. The unit unpacks f16, bf16, f32, f64 and x87's f80
into a common wide format, computes add, multiply, fused multiply-add,
divide and square root exactly enough to round correctly (guard, round and
sticky bits), and rounds in all four IEEE 754-2019 modes with x86's rules:
the MXCSR/x87 exception flags (IE, DE, ZE, OE, UE, PE), DAZ/FTZ, the x86
"QNaN floating-point indefinite" and NaN propagation (Intel SDM Vol. 1,
4.8.3 and 11.5, Table 4-7). Division and square root are single-cycle
behavioural operators on wide integers in v1; a later version iterates them
(SRT division and square root: Ercegovac & Lang, *Digital Arithmetic*,
Morgan Kaufmann 2004, ch. 5-6). The FMA follows the single-rounding
structure of Montoye, Hokenek & Runyon, "Design of the IBM RISC System/6000
floating-point execution unit", IBM J. R&D 34(1), 1990,
doi:10.1147/rd.341.0059.

## D-010: vector instructions by lane-loop recognition

The SDM writes vector instructions as loops over lanes (`FOR j := 0 TO
KL-1; i := j * 32; IF k1[j] OR *no writemask* ...`). ucc recognizes a lane
loop whose body is element-wise and emits one vector micro-op (element type,
vector length, write mask, zeroing); a body it doesn't recognize is unrolled
over the lanes with element extract/insert micro-ops, so every instruction
compiles and recognition only affects speed. This is loop idiom recognition
over the specification, cf. the vectorization of loops in Allen & Kennedy,
*Optimizing Compilers for Modern Architectures*, Morgan Kaufmann 2001
(ch. 2, 5).

## D-011: #DF and shutdown in hardware

The core classifies an exception raised while delivering another as the
Intel SDM Vol. 3A, 7.15 (Interrupt 8, Tables 7-4 and 7-5) does: benign,
contributory (#DE, #TS, #NP, #SS, #GP) or page fault; two contributory
exceptions, or a page fault followed by a page fault or a contributory
exception, become #DF; an exception while delivering #DF shuts the
processor down (triple fault). Otherwise the second exception is delivered
after the first is abandoned.

## D-012: system transfers in microcode

Far CALL/RET/JMP through code segments, call gates (with a stack switch
and parameter copy) and task gates/TSS descriptors, task switches (a
microcode routine saving and loading the TSS, Vol. 3A ch. 9), SYSCALL/
SYSRET and SYSENTER/SYSEXIT are compiled from the manual's Operation
sections (with overrides where the manual's prose needs it). The debug
registers DR0-DR3/DR6/DR7 are architectural state; the core checks
instruction breakpoints at decode and data breakpoints on memory micro-ops,
and raises single-step traps (EFLAGS.TF) after an instruction, as Vol. 3A
ch. 19 "Debug, Branch Profile, TSX, and Intel Resource Director
Technology" describes.

## D-013: shared descriptions in rtl/common, generated

What every core version must agree on with the others and with the rest
of the system is described once, as data, in `rtl/common/*.toml`, and
`tools/gen/rtlgen.py` generates the SystemVerilog and C from it into the
build tree (`build/rtl/common/`, not in git):

| Description | What | Generated |
|---|---|---|
| `cpuid.toml` | identity (vendor `VibingVibing`, family 1Ah, brand, address sizes) and the Venice feature baseline, each bit with the version that first implements it | `vibe86_cpuid_pkg.sv`: `cpuid_<v>(leaf, subleaf)`, read by the `CPUID` micro-op |
| `reset.toml` | architectural state after RESET# (Intel SDM Vol. 3A Table 10-1, AMD APM Vol. 2 Table 14-1) | `vibe86_reset_pkg.sv`, `vibe86_reset_apply.svh` |
| `platform.toml` | clocks, socket pin groups and widths, the physical map (LAPIC, IOAPIC, HPET, MSI, ECAM, flash), legacy I/O ports, MSR addresses and defaults | `vibe86_platform_pkg.sv`, `vibe86_platform.h` (vsim's board), and the ACPI tables (`mktables.py --platform`) |
| `pins.toml` | the die interface: every external pin of a core version, its direction, width and meaning | `vibe86_die_pkg.sv`, `vibe86_die_ports.svh` (each core's port list), `vibe86_die.h` (vsim's `struct cpu` and the copy to a Verilated model) |

A core includes the generated port list and applies the reset include, so
a description and a core can't drift apart: a register the core lacks or a
pin it names differently fails the build. Generated SV and C come from the
same file, so vsim's addresses, the firmware's ACPI tables and the RTL's
agree by construction. This is the "single source of truth" register/
interface description practice of hardware generators such as SystemRDL
(Accellera, *SystemRDL 2.0 Register Description Language*, 2018) and
Chisel's parameterized generators (J. Bachrach et al., "Chisel:
Constructing Hardware in a Scala Embedded Language", DAC 2012,
https://doi.org/10.1145/2228360.2228584), reduced to a TOML file and a
small script.

## D-014: a pin-accurate die: DDR5, an I/O link, JTAG; memory initialized before the core

The die (`rtl/<v>/<v>_die.sv`: the core plus `rtl/uncore/`) talks to the
system only through pins a real server part has (docs/die-interface.md):

- **DDR5**: two channels of two 32+8-bit subchannels with their DRAMs'
  own CS_n/CA/DQ/DQS/RESET_n/ALERT_n, driven by four RTL memory
  controllers (`ddr5_sub.sv`) from Micron's *DDR5 SDRAM Product Core
  Data Sheet* (Rev. D 10/2022; JESD79-5): the command truth table, the
  power-up sequence, mode registers, MPC, refresh and the DDR5-4800
  timings, open pages, per-beat Hsiao SEC-DED ECC (Hsiao 1970). The
  DIMMs are C models at the same pin level (`sim/ddr5.c`) that flag every
  protocol or timing violation. An RDIMM's register (JESD82-511) is not
  modelled: CA reaches the DRAMs directly, as on an unbuffered module.
- **Memory initialization before the core**, as AMD's PSP does on EPYC:
  `meminit.sv` reads the SPD hubs over the SPD SMBus pins, checks the
  population (channel 0 alone or both channels alike), runs RESET_n and
  starts the controllers' JEDEC initialization, posts its progress on port
  80h (D0h-D3h, errors E1h-E4h) and only then releases the x86 core. The
  user chose this over x86 firmware training so that firmware (SeaBIOS,
  ours) finds DRAM at reset. A `meminit_fast` strap scales the JEDEC waits
  by 1/1024 in simulation (tINIT3 alone is 9.6M clocks); vsim -F sets it.
- **An I/O link** replaces the held request/acknowledge bus: two 64-bit
  flit channels with valid/ready handshakes and split transactions
  (MRD/MWR/IORD/IOWR/CPL/CPLE), like the die-to-I/O-die links of chiplet
  servers. Device DMA comes into the die over it and is served from DRAM
  by the fabric. vsim's `iod.c` is the I/O die: the board's devices run in
  a coroutine that a DMA read suspends until its completion returns.
- **The address fabric** (`fabric.sv`) owns the physical map: DRAM below
  TOLM (2 GiB) and hoisted above 4 GiB (as AMD's data fabric), SMRAM at
  A_0000h for the core in SMM, the PAM shadow, and the i440FX host
  bridge's registers (bus 0 device 0's configuration is answered on the
  die), everything else over the link.
- **JTAG**: an IEEE 1149.1 TAP and a debug module (halt, step, registers,
  system-bus access) — docs/jtag.md; vsim -D serves OpenOCD's
  remote_bitbang protocol.
- **Machine checks**: an uncorrectable ECC error in DRAM reaches the core as
  #MC (with CR4.MCE) or shuts it down (SDM Vol. 3A 17.10).

The test board's SRAM and the C memory controller (`sim/dram.c`) are gone:
every board runs from DIMMs. The cost is simulation speed — every DRAM
access is a DDR5 transaction (ACT/RD at CL40) instead of a one-cycle
board access.

## D-018: v1's caches: 64 KiB I-cache, 256 KiB write-through D-cache, coherent by a write snoop

With DRAM behind pin-level DDR5 controllers (D-014) every uncached access
costs a full DDR5 transaction, and the firmware took ~7x more cycles to
boot than on the old SRAM board. v1 gets two L1 caches, the smallest
change that removes most of that:

- **Instruction cache**: 64 KiB, direct-mapped, 1024 lines of 64 bytes,
  physically indexed and tagged, behind the existing 16-byte fetch line
  buffer. A miss reads the whole line (bus size 6, 512 bits).
- **Data cache**: 256 KiB, direct-mapped, 4096 lines of 64 bytes,
  physically indexed and tagged; **write-through**, updated on a write
  hit, allocated on a read miss only. A load costs one lookup cycle on a
  hit, a store one lookup cycle before its (still synchronous) write.
- **What is cached**: DRAM below TOLM except A_0000h-F_FFFFh (SMRAM and
  the PAM-steered shadow region change meaning under the core), DRAM
  hoisted above 4 GiB below the 64-bit PCI window, and the flash
  (read-only). Everything else (MMIO, the APIC page, VRAM, port I/O) is
  uncached, as the MTRRs/PAT of a PC would make it. The ranges are fixed
  (rtl/common/platform.toml): v1's MTRRs and PAT hold what software writes
  but don't steer the caches. CPUID Fn8000_001D describes both caches.
- **Coherence**: x86 keeps instruction fetches coherent with stores
  (self-modifying code, Intel SDM Vol. 3A 11.6) and caches coherent with
  DMA. The fabric already sees every DRAM write, so it reports each one's
  line to the core (`ic_inv`, `ic_inv_line`, with `ic_inv_core` for the
  core's own); the I-cache invalidates on every write, the D-cache on
  writes that weren't its own (those it updated). Because the D-cache is
  write-through, DRAM is always current and device reads need no snoop.
  A fill that a write overtakes is not kept. INVD, WBINVD (WBNOINVD),
  CLFLUSH(OPT), INIT and reset flush both caches.
- **Arrays** are synchronous RAMs with a one-cycle registered read (the
  data cache's with byte write enables); only the valid bits are flops.

Not yet: write-back (it needs the fabric to snoop device reads), MTRR/PAT
memory types and CR0.CD, set associativity, an L2. The CPUID cache
description (Fn8000_001Dh) comes with TOPOEXT.

## D-019: the frozen pinout: eight DDR5 channels, interrupts as messages, management balls

Before the external pinout is frozen, the user asked for a server-class
package (docs/package.md has the pinout; `rtl/common/package.toml` the
ball map, checked by `tools/gen/package.py`). The die stays one die
(D-014): a compute/I-O die split was drafted and then dropped at the
user's request before the pinout froze.

- **Eight DDR5 channels' balls**, two implemented; the rest reserved and
  idle. Two SPD buses (the SPD hub's eight addresses per bus).
- **Interrupts are messages.** No INTR/NMI/SMI/MCE pins: the chipset
  sends virtual wires (INTR with its vector, NMI, SMI, INIT) over the
  system link and INTA goes back, as HyperTransport did (HT I/O Link Spec
  3.10, 8.5); MSIs stay memory writes. On the die they are APIC-bus
  messages (D-017). Machine checks from the chipset are MCE messages.
- **Inter-socket links**: two x16 links' balls, reserved.
- **Management balls**: PWROK and RESET_L with a sequencer in the SMU
  (rails -> reset -> memory init -> run); a three-wire VR control bus after
  SVID/SVI (our own protocol, specified in docs/package.md, since both are
  closed) with a VR model in vsim; THERMTRIP_L and bidirectional
  PROCHOT_L on a temperature sensor whose physical input the simulator
  supplies; MCERR_L, CATERR_L, ERROR_L[2:0]; an SMBus 3.x sideband target
  for a BMC (temperature and management interfaces after AMD's APML
  split; I3C-compatible balls); SOCKET_ID straps (APIC IDs socket x 16 +
  core), PKG_TYPE and PKG_VER wired in the package, SKT_OCC_L; JTAG kept;
  a trace port (16-bit, branch-trace packets after Intel PT's) next to it.
- **Power**: five rails (cores and uncore on separate SVID-controlled
  domains), at most 0.5 A per contact at EDC and at least as many VSS balls
  as supply balls, in an LGA-4096 budget with 126 reserved balls.

vsim's harness ports (retirement, events, shutdown) stay, marked as
simulation observation, not balls.
