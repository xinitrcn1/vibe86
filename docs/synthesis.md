# Synthesis notes for rtl/v1

v1 is written to be simulated first (Verilator) and synthesized later. This
file records what lint checks, what is outside the synthesizable subset or
will synthesize badly, and what to replace it with. It was written from
reading the RTL and from Verilator's lint; **yosys is not installed on the
build server yet** (it needs root: `pkg install yosys`), so every item here
should be validated with a real synthesis run (yosys with an SV front end such
as yosys-slang or sv2v, or a commercial tool) before it is relied on.

References: IEEE 1800-2017 *SystemVerilog* (the language); IEEE 1364.1-2002
*Verilog Register Transfer Level Synthesis* (the synthesizable subset: no
delays, no `initial` in RTL, loops with static bounds, no `real`); the Yosys
manual (C. Wolf, *Yosys Open SYnthesis Suite*, https://yosyshq.readthedocs.io,
"Verilog/SystemVerilog support"); M. D. Ercegovac and T. Lang, *Digital
Arithmetic*, Morgan Kaufmann, 2004 (iterative division and square root);
S. F. Oberman and M. J. Flynn, "Division algorithms and implementations",
IEEE Trans. Computers 46(8), 1997, https://doi.org/10.1109/12.609274.

## Lint

`ctest -R rtl/v1/lint` runs `verilator -Wall --lint-only` over the core
(top `v1_core`, the generated packages included) and fails on any warning.
The normal build (`verilate`, the testbenches) no longer passes `-Wno-WIDTH`,
`-Wno-UNUSED` or `-Wno-fatal`: every width is explicit.

Waivers, all scoped and commented:

| Where | Rule | Why |
|---|---|---|
| generated packages (`tools/ucc`, `tools/gen/rtlgen.py`) | UNUSEDPARAM | tables of constants (micro-op encodings, special-register numbers, CPUID bits); a core uses some |
| `v1_core.sv`: `attr`, `cond(f)`, `u`, `seg_check`'s `sattr`; `v1_alu.sv`: `cond(f)`; `v1_fp_pkg.sv`: whole-struct operands | UNUSEDSIGNAL | a value passed whole of which a function reads some fields |
| `rtl/v1/v1.vlt`: `mce_*`, `bus_rdata[511:128]` | UNUSEDSIGNAL | die pins v1 doesn't use yet (no machine-check architecture; 16-byte transactions on the 512-bit bus) |
| `rtl/v1/v1.vlt`: the bus logic's 128-to-512-bit and byte-index widths | WIDTHEXPAND/TRUNC | that logic is being replaced by the die fabric (DDR5, JTAG); the waivers go with it |
| `tb/tb_fpu.sv` test vectors | WIDTHEXPAND | narrower formats zero-extended into the 80-bit operand on purpose |

Found and fixed by lint: two unsized constants inside concatenations (IEEE
1800-2017 11.4.12 makes them illegal; tools disagree on their width), a
2-bit index into the 16-entry GPR array for AH-BH, a 20-bit micro-address
indexing a 32,917-entry ROM, dead state (`plock`, `intr_pend`, `intr_vec`),
an unused FPU `add_core` argument pair, a hidden declaration, and the FPU
package's file name (`v1_fp_pkg.sv`).

## Constructs checked

Not present in the RTL (`v1_core.sv`, `v1_alu.sv`, `v1_fp_pkg.sv`): `initial`
blocks, delays, `real`, `$display` outside `ifdef V1_DEBUG`, X assignments,
latches (Verilator reports none), multiple drivers. All loops have constant
bounds (they unroll).

Legal SystemVerilog that some synthesis front ends reject; check with the
chosen tool:

| Construct | Where | Note |
|---|---|---|
| `return` inside a task called from `always_ff` | `v1_core.sv:495` (`raise`), `:582` (`wr_special`) | IEEE 1800-2017 13.3; older yosys front ends don't accept it. Rewrite as if/else if needed |
| early `return` in functions | `rd`, `seg_check`, `tlb_look`, FPU functions | widely supported; yosys only with a full SV front end |
| `inside` set membership | `v1_core.sv:1140`, `:1159` | IEEE 1800-2017 11.4.13; yosys needs yosys-slang or sv2v |
| packed structs, `typedef enum`, packages | everywhere | same |

## Hardware that synthesizes badly

Everything below is combinational in one cycle today. It simulates fine, but
in silicon it would set the clock period or the area. The replacements keep
the micro-op interface: the sequencer already waits on multi-cycle memory
micro-ops, so a multi-cycle ALU/FPU op needs only a `busy` handshake.

| What | Where | Problem | Replacement |
|---|---|---|---|
| 128/64-bit integer divide and remainder, unsigned and signed | `v1_alu.sv:284-298` (DIV, IDIV) | a 64-stage subtract-and-select array, ~the area of a small core | iterative radix-4 SRT or non-restoring divider, ~32-64 cycles, early exit on small quotients (Oberman & Flynn 1997) |
| `count % (n + 1)` for RCL/RCR (9, 17, 33, 65) | `v1_alu.sv:219` | a real divider by a non-power-of-two | a small lookup table: the count is at most 63 |
| `count % n` for ROL/ROR | `v1_alu.sv:198` | n is a power of two, so this is a mask; make it explicit (`cnt & (n - 1)`) so no tool builds a divider | |
| 64×64 → 128-bit multiply | `v1_alu.sv:268`, `v1_fp_pkg.sv:340` | a full single-cycle multiplier at 128-bit output | a 2-3 stage pipelined Booth/Wallace multiplier, shared between the ALU and the FPU |
| 192/64-bit significand divide | `v1_fp_pkg.sv:382-383` (FDIV) | larger than the integer divider | the same iterative SRT unit as square root (radix-4, shared; Ercegovac & Lang ch. 5-6) |
| 256-bit bit-by-bit integer square root, 128 unrolled steps | `v1_fp_pkg.sv:607` (`isqrt256`, FSQRT) | 128 cascaded 258-bit compares/subtracts | iterative radix-4 SRT square root sharing the divider |
| leading-zero counts as priority loops | `v1_fp_pkg.sv:101`, `:165`; `v1_alu.sv:320-340` | correct but deep | a log-depth LZC/priority tree |
| 256-bit alignment shifts and adds in `add_core`/`fma_core` | `v1_fp_pkg.sv:457ff`, `:532ff` | wide barrel shifters | standard FMA datapath (one 3:2 compressor + 161-bit adder for f64, LZA) |

The FPU (`v1_fp_pkg.sv`) isn't instantiated by the core yet (D-009): the
rows above apply when it is.

## Storage

| What | Where | Note |
|---|---|---|
| microcode ROM, 32,917 × 128 bits (~4.2 Mbit) | `v1_core.sv:183` (`UROM` from `v1_ucode_pkg`) | infers a ROM; read it registered (one cycle) from a ROM macro or block RAM, which costs the sequencer a pipeline stage |
| special registers `sp[256]` × 64 | `v1_core.sv:48` | written from many places in one `always_ff` and reset in a loop (`:608`): it becomes 16 Kbit of flip-flops with many write ports. Keep architectural registers that need reset as flops and move the rest (scratch, rarely written MSRs) into a RAM without reset |
| banked temps `tb[4][32]`, `tt[32]` | `v1_core.sv:42-43` | register files (2 read ports, 1 write): RAM-able |
| 64-entry fully associative TLB | `v1_core.sv:300` (`tlb_look`, used twice) | two parallel 64-way CAM lookups (fetch and data); fine at this size, a split I/D TLB is the usual shape |
| one `always_ff` with asynchronous reset for all state | `v1_core.sv:652` | arrays under an asynchronous reset can't map to RAM; reset only the control state |
