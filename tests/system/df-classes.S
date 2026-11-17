// df-classes: which exception pairs become a double fault (Intel SDM
// Vol. 3A 7.15, "Interrupt 8", Tables 7-4 and 7-5), in IA-32e mode:
//   contributory then contributory     -> #DF   (#GP, its gate not present: #NP)
//   benign then contributory           -> the second, serially (#UD, gate not present: #NP)
//   page fault then page fault         -> #DF   (#PF delivered onto an unmapped IST stack)
//   page fault then contributory       -> #DF   (#PF's gate selects the TSS: #GP)
//   contributory then page fault       -> #PF, serially (#GP onto an unmapped IST stack)
//   a gate's selector not a 64-bit code segment -> #GP(selector + EXT)
// with the error codes: #DF 0, #NP/#GP for an IDT entry (vector * 8 + 2 + EXT).
// expect-out: DF OK
#include "boot64.inc"
#include "expect.inc"
	.set	UNMAPPED, 0x200001000		// PDPT entry 8: not present
	.set	IST1, TSS + 0x24

	// gate byte offsets: +4 IST, +5 type/P
	.macro	GATE_P vec, present
	.if	\present
	orb	$0x80, IDT + \vec * 16 + 5
	.else
	andb	$0x7f, IDT + \vec * 16 + 5
	.endif
	.endm

main64:
	call	expect_init
	movabsq	$UNMAPPED, %rax
	mov	%rax, IST1

	// ---- #GP then #NP (both contributory): #DF, error code 0
	GATE_P	13, 0
	CATCH	1f
	mov	$0x1238, %ax
	mov	%ax, %ds			// #GP(1238h) -> its gate isn't present
1:	EXPECT_VEC 8, 0x01
	EXPECT	LASTERR, 0, 0x02
	GATE_P	13, 1
	mov	$0x10, %ax
	mov	%ax, %ds

	// ---- #UD (benign) then #NP: #NP for IDT entry 6, EXT
	GATE_P	6, 0
	CATCH	1f
	ud2
1:	EXPECT_VEC 11, 0x10
	EXPECT	LASTERR, 6 * 8 + 2 + 1, 0x11
	GATE_P	6, 1

	// ---- #PF then #PF: the #PF gate's IST stack is unmapped
	movb	$1, IDT + 14 * 16 + 4
	CATCH	1f
	movabsq	0x100000000, %rax		// #PF (PDPT entry 4 not present)
1:	EXPECT_VEC 8, 0x20
	EXPECT	LASTERR, 0, 0x21
	movb	$0, IDT + 14 * 16 + 4

	// ---- #PF then #GP: the #PF gate's code selector is the TSS
	movw	$0x30, IDT + 14 * 16 + 2
	CATCH	1f
	movabsq	0x100000000, %rax
1:	EXPECT_VEC 8, 0x30
	movw	$0x18, IDT + 14 * 16 + 2

	// ---- a gate whose selector isn't a 64-bit code segment: #GP(sel + EXT);
	// after #UD (benign) it is delivered serially
	movw	$0x30, IDT + 6 * 16 + 2		// the TSS
	CATCH	1f
	ud2
1:	EXPECT_VEC 13, 0x50
	EXPECT	LASTERR, 0x31, 0x51
	movw	$0x08, IDT + 6 * 16 + 2		// 32-bit code: not for IA-32e
	CATCH	1f
	ud2
1:	EXPECT_VEC 13, 0x52
	EXPECT	LASTERR, 0x09, 0x53
	movw	$0x18, IDT + 6 * 16 + 2

	// ---- #GP then #PF: serially, the #PF is delivered
	movb	$1, IDT + 13 * 16 + 4
	CATCH	1f
	mov	$0x1238, %ax
	mov	%ax, %ds
1:	EXPECT_VEC 14, 0x40
	mov	%cr2, %rax
	EXPECT	%rax, UNMAPPED - 8, 0x41	// the first push of the frame
	movb	$0, IDT + 13 * 16 + 4
	mov	$0x10, %ax
	mov	%ax, %ds

	lea	endmsg(%rip), %rsi
	call	puts
	jmp	pass

endmsg:	.asciz	"DF OK\n"
	BOOT64_END
