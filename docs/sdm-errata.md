# Errata in Intel's specifications found while compiling them

Places where Intel's documentation, read as the executable specification
(D-002), is wrong or can't be read as written. Each one is fixed by an
override in `tools/ucc/targets/<v>/overrides/` (or noted as not affecting
us) rather than by editing the extracted text, so the extracted pages stay
the manual's own words. SDM = Intel 64 and IA-32 Architectures Software
Developer's Manual, Vol. 2, order no. 325383-093US (September 2026).

| # | Where | What the text says | What is meant | Kind | Handled by |
|---|---|---|---|---|---|
| E-1 | SDM, DIV, Operation | `AL := temp; AH := AX MOD SRC;` (and likewise for DX:AX, EDX:EAX, RDX:RAX) | the remainder of the *original* dividend: read in order, AX already holds the quotient | sequential semantics | override `DIV.sdm` |
| E-2 | SDM, IDIV, Operation | as E-1, and `IF (temp > 7FH) or (temp < 80H)` | the range check compares signed values; the hexadecimal constants read as unsigned make every positive quotient fail | signedness | override `IDIV.sdm` |
| E-3 | SDM, POP, Operation | `ELSE StackAddrSize = 16 THEN` | `ELSE IF StackAddrSize = 16 THEN` | syntax | override `POP.sdm` |
| E-4 | SDM, CMPXCHG, Operation / Flags Affected | flags "set according to the results of the comparison operation", but the Operation has no comparison that produces CF/OF/SF/AF/PF | the flags of `accumulator - DEST`, as CMP | missing statement | override `CMPXCHG.sdm` |
| E-5 | SDM, PUSH, Operation | an `ELSE IF` chain closed by `FI;` before its last `ELSE` | the chain continues | nesting | parser leniency (sdm.py) and override `PUSH.sdm` |
| E-6 | SDM, MOVS/STOS/LODS/CMPS/SCAS, Operation | `IF DF = 0` with no `THEN`, `FI;` before `ELSE`, prose labels (`64-bit Mode:`) | two structured branches | syntax | overrides for each |
| E-7 | SDM, CMOVcc, opcode table | `REX.W + 0F 4F /r CMOVG r64, r/m64` has 64-bit mode `V/N.E.` and compat `N/A` | `Valid` / `N.E.` like the other rows | table | tolerated (forms.py accepts `V/N.E.`) |
| E-8 | SDM, MOVSX/MOVSXD, opcode table | no `MOVSX r16, r/m16` row; `MOVSXD r16, r/m16` listed | (a 16-bit MOVSXD is a plain move; not used by compilers) | table | `MOVSXD r16` skipped in the isa file |
| E-9 | SDM, IMUL, Operation | `TMP_XP` is "a signed integer at twice the width of the SRC" (a comment carries the semantics) | a double-width product | semantics in a comment | override `IMUL.sdm` |
| E-10 | SDM, MOV (Sreg), Operation | the segment-load rules are introduced by a prose sentence in the Operation section | | prose | override `MOV.sdm` (Sreg rows separate) |
| E-11 | Intel Intrinsics Guide, `_mm512_set_epi8` | `dst[511:503] := e63` | `dst[511:504] := e63` (bytes are 8 bits) | off by one | reported by the author; not in Vol. 2 (the intrinsic sections aren't compiled), fix needed if the Intrinsics Guide is ever used as a source |
| E-12 | SDM, TZCNT and LZCNT, opcode tables | laid out as Opcode/Op-En/64-32 bit/CPUID columns without an Instruction column | | table layout | opcode overrides |
| E-13 | SDM, CMC | no Operation section | `CF := NOT CF` (Description) | missing | override `CMC.sdm` |
| E-14 | SDM, RDMSR, opcode table | the 0F 32 row has no Instruction column (it shares the table with the VEX MSR_IMM form) | `0F 32 \| RDMSR \| ZO \| Valid \| Valid` | table layout | opcode override |
| E-15 | SDM, LFENCE and MFENCE, opcode tables | Opcode/Op-En/64-32 bit/CPUID layout without an Instruction column | | table layout | opcode overrides |
| E-16 | SDM, SETcc, opcode table | rows like `0F 94 \| SETE r/m8` without `/r` (or `/0`), though the instruction has a ModRM byte | | table | forms.py infers ModRM from an r/m operand |
| E-17 | SDM, CWD/CDQ/CQO, Operation | `DX := SignExtend(AX);` (and EDX/EAX, RDX/RAX) | DX:AX := SignExtend(AX): DX receives the sign only | notation | override `CWD_CDQ_CQO.sdm` |
| E-18 | SDM, CMPXCHG, Operation | `ELSE accumulator := TEMP; DEST := TEMP;` | for a register destination nothing is written (a 32-bit write would zero-extend the register in 64-bit mode; Intel and AMD processors leave it); the write-back is the memory operand's locked cycle | semantics | override `CMPXCHG.sdm` |
