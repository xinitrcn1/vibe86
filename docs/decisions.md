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
TSC_ADJUST and CET_SS. `tools/ucc/targets/<v>/features` lists it; CPUID
reports exactly what a version implements, so partial versions stay honest.

SVM and AVX512-BMM are AMD-only: their semantics come from the AMD64
Architecture Programmer's Manual (Vol. 2 "System Programming", Vol. 3/4
instruction references), not the Intel SDM.

## D-008: microcode-heavy system architecture

The hardware keeps a TLB, segment descriptor caches and a micro-trap
mechanism; page walks, descriptor loads, privilege checks and exception
delivery are microcode routines (as the Intel 80386 did segment loads in
microcode). This keeps the RTL small and puts the architecture in one place:
the compiled specification.

## D-009: v1's floating-point unit is behavioural

v1's x87/SSE/AVX floating-point micro-ops are computed by Berkeley SoftFloat
3e (J. R. Hauser, http://www.jhauser.us/arithmetic/SoftFloat.html) through
Verilator DPI-C: bit-exact IEEE 754-2019 (and x87 80-bit extended), not
synthesizable. A later core version replaces it with FP datapaths (e.g. a
fused multiply-add after Montoye, Hokenek & Runyon, "Design of the IBM RISC
System/6000 floating-point execution unit", IBM J. R&D 34(1), 1990,
doi:10.1147/rd.341.0059).

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
