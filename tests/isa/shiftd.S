// shiftd: SHLD/SHRD (SeaBIOS needed SHRD; it was missing) at 16, 32 and 64
// bits, CL, imm8 and memory forms, around the count boundaries: 1, a nibble,
// size-1, 32 in a 64-bit operand, counts masked to 5/6 bits (33 -> 1,
// 65 -> 1), and for 16 bits a count of 16 (all of the source). CF is the
// last bit shifted out. A masked count of 0 changes neither the destination
// nor the flags. (Counts above 16 for a 16-bit operand are undefined.)
// expect-out: SHD OK
#include "boot64.inc"
#include "expect.inc"
	.set	SCR, 0x40000

	// SHT op, suffix, dst, src, count, dest, source, result, cf, id: the CL,
	// imm8 and memory forms; RBX/SCR hold the destination (16-bit: the upper
	// bits 5A5A...), RSI the source
	.macro	SHT op, sfx, dst, src, count, dest, source, result, cf, id
	.irp	form, 0, 1, 2
	movabsq	$0x5a5a5a5a5a5a0000, %rbx
	.ifc	\sfx, q
	movabsq	$\dest, %rbx
	.endif
	.ifc	\sfx, l
	mov	$\dest, %ebx
	.endif
	.ifc	\sfx, w
	or	$\dest, %rbx
	.endif
	movabsq	$\source, %rsi
	mov	$\count, %cl
	.if	\cf
	clc
	.else
	stc
	.endif
	.if	\form == 0
	\op\sfx	%cl, \src, \dst
	.endif
	.if	\form == 1
	\op\sfx	$\count, \src, \dst
	.endif
	.if	\form == 2
	mov	%rbx, SCR
	\op\sfx	%cl, \src, SCR
	mov	SCR, %rbx
	.endif
	setc	%al
	movzbq	%al, %rax
	EXPECT	%rbx, \result, \id + \form * 2
	EXPECT	%rax, \cf, \id + \form * 2 + 1
	.endr
	.endm

main64:
	call	expect_init
	SHT	shld, q, %rbx, %rsi, 1,  0x0123456789abcdef, 0xfedcba9876543210, 0x02468acf13579bdf, 0, 0x100
	SHT	shld, q, %rbx, %rsi, 4,  0x0123456789abcdef, 0xfedcba9876543210, 0x123456789abcdeff, 0, 0x110
	SHT	shld, q, %rbx, %rsi, 31, 0x0123456789abcdef, 0xfedcba9876543210, 0xc4d5e6f7ff6e5d4c, 1, 0x120
	SHT	shld, q, %rbx, %rsi, 32, 0x0123456789abcdef, 0xfedcba9876543210, 0x89abcdeffedcba98, 1, 0x130
	SHT	shld, q, %rbx, %rsi, 63, 0x0123456789abcdef, 0xfedcba9876543210, 0xff6e5d4c3b2a1908, 1, 0x140
	SHT	shld, q, %rbx, %rsi, 65, 0x0123456789abcdef, 0xfedcba9876543210, 0x02468acf13579bdf, 0, 0x150
	SHT	shrd, q, %rbx, %rsi, 1,  0x0123456789abcdef, 0xfedcba9876543210, 0x0091a2b3c4d5e6f7, 1, 0x160
	SHT	shrd, q, %rbx, %rsi, 31, 0x0123456789abcdef, 0xfedcba9876543210, 0xeca8642002468acf, 0, 0x170
	SHT	shrd, q, %rbx, %rsi, 32, 0x0123456789abcdef, 0xfedcba9876543210, 0x7654321001234567, 1, 0x180
	SHT	shrd, q, %rbx, %rsi, 63, 0x0123456789abcdef, 0xfedcba9876543210, 0xfdb97530eca86420, 0, 0x190
	SHT	shld, l, %ebx, %esi, 1,  0x89abcdef, 0x76543210, 0x13579bde, 1, 0x200
	SHT	shld, l, %ebx, %esi, 4,  0x89abcdef, 0x76543210, 0x9abcdef7, 0, 0x210
	SHT	shld, l, %ebx, %esi, 31, 0x89abcdef, 0x76543210, 0xbb2a1908, 1, 0x220
	SHT	shld, l, %ebx, %esi, 33, 0x89abcdef, 0x76543210, 0x13579bde, 1, 0x230
	SHT	shrd, l, %ebx, %esi, 1,  0x89abcdef, 0x76543210, 0x44d5e6f7, 1, 0x240
	SHT	shrd, l, %ebx, %esi, 31, 0x89abcdef, 0x76543210, 0xeca86421, 0, 0x250
	SHT	shld, w, %bx, %si, 1,  0x8bcd, 0x1234, 0x5a5a5a5a5a5a179a, 1, 0x300
	SHT	shld, w, %bx, %si, 15, 0x8bcd, 0x1234, 0x5a5a5a5a5a5a891a, 0, 0x310
	SHT	shld, w, %bx, %si, 16, 0x8bcd, 0x1234, 0x5a5a5a5a5a5a1234, 1, 0x320
	SHT	shrd, w, %bx, %si, 4,  0x8bcd, 0x1234, 0x5a5a5a5a5a5a48bc, 1, 0x330
	SHT	shrd, w, %bx, %si, 15, 0x8bcd, 0x1234, 0x5a5a5a5a5a5a2469, 0, 0x340
	SHT	shrd, w, %bx, %si, 16, 0x8bcd, 0x1234, 0x5a5a5a5a5a5a1234, 1, 0x350
	// a masked count of 0: no change, flags kept (CF and ZF)
	movabsq	$0x0123456789abcdef, %rbx
	movabsq	$0xfedcba9876543210, %rsi
	stc
	mov	$64, %cl
	shld	%cl, %rsi, %rbx
	setc	%al
	movzbq	%al, %rax
	EXPECT	%rbx, 0x0123456789abcdef, 0x01
	EXPECT	%rax, 1, 0x02
	mov	$0x89abcdef, %ebx
	xor	%eax, %eax			// ZF = 1
	shrd	$32, %esi, %ebx
	setz	%al
	EXPECT	%rbx, 0x89abcdef, 0x03
	EXPECT	%rax, 1, 0x04
	// SF, ZF, PF from the result (count 1: OF = sign change)
	mov	$0x40000000, %ebx
	xor	%esi, %esi
	shld	$1, %esi, %ebx			// 0x80000000: SF, OF
	seto	%al
	sets	%ah
	movzwq	%ax, %rax
	EXPECT	%rax, 0x0101, 0x05
	mov	$1, %ebx
	shrd	$1, %esi, %ebx			// 0: ZF, PF, CF
	setz	%al
	setp	%ah
	movzwq	%ax, %rax
	EXPECT	%rax, 0x0101, 0x06
	lea	endmsg(%rip), %rsi
	call	puts
	jmp	pass

endmsg:	.asciz	"SHD OK\n"
	BOOT64_END
