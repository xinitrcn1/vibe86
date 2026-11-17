// vex: the VEX prefix as v1 decodes it for the VEX-encoded GPR instructions
// (BMI1, BMI2), 64-bit mode (Intel SDM Vol. 2A 2.3 "Intel AVX"):
//   a 66h, F2h, F3h, LOCK or REX prefix before VEX is #UD (2.3.2);
//   VEX.L = 1 on an LZ form is #UD; a reserved VEX map (mmmmm = 4) is #UD;
//   RORX with VEX.vvvv != 1111b (it has no vvvv operand) is #UD;
//   results: ANDN, PDEP/PEXT, MULX with DEST1 = DEST2 (the high half), RORX,
//   SHLX/SARX/SHRX with the count masked, BZHI's CF, BLSMSK's ZF;
//   VEX.R/B extend the registers (r8-r15), VEX.vvvv reaches r8-r15.
// expect-out: VEX OK
#include "boot64.inc"
#include "expect.inc"

	.macro	UD bytes:vararg
	CATCH	9877f
	.byte	\bytes
	jmp	fail
9877:	EXPECT_VEC 6, 0x1000 + \@
	.endm

main64:
	call	expect_init

	// c4 e2 78 f2 c1: ANDN eax, eax, ecx (VEX.LZ.0F38.W0 F2 /r) -- valid
	mov	$0xF0F0, %eax
	mov	$0xFF00, %ecx
	.byte	0xc4, 0xe2, 0x78, 0xf2, 0xc1	// andn %ecx, %eax, %eax
	EXPECT	%rax, 0x0F00, 0x01

	UD	0x66, 0xc4, 0xe2, 0x78, 0xf2, 0xc1
	UD	0xf3, 0xc4, 0xe2, 0x78, 0xf2, 0xc1
	UD	0xf2, 0xc4, 0xe2, 0x78, 0xf2, 0xc1
	UD	0xf0, 0xc4, 0xe2, 0x78, 0xf2, 0xc1
	UD	0x40, 0xc4, 0xe2, 0x78, 0xf2, 0xc1
	UD	0xc4, 0xe2, 0x7c, 0xf2, 0xc1		// VEX.L = 1
	UD	0xc4, 0xe4, 0x78, 0xf2, 0xc1		// map 4: reserved
	UD	0xc4, 0xe3, 0x73, 0xf0, 0xc1, 0x05	// RORX with vvvv = 0001b
	UD	0xc5, 0xf8, 0xf2, 0xc1			// VEX.0F F2: nothing there

	// PDEP / PEXT
	mov	$0b101, %rax
	mov	$0xF0, %rcx
	pdepq	%rcx, %rax, %rdx
	EXPECT	%rdx, 0x50, 0x10
	mov	$0x50, %rax
	pextq	%rcx, %rax, %rdx
	EXPECT	%rdx, 0b101, 0x11
	// r8-r15 through VEX.R (dest), VEX.B (r/m) and vvvv
	mov	$0xFF00FF00FF00FF00, %r9
	mov	$0x00000000FFFFFFFF, %r10
	pextq	%r10, %r9, %r11
	EXPECT	%r11, 0xFF00FF00, 0x12
	// MULX: rdx * src; DEST1 = DEST2 keeps the high half
	mov	$-1, %rdx
	mov	$2, %rcx
	mulxq	%rcx, %r8, %r8
	EXPECT	%r8, 1, 0x20
	mulxq	%rcx, %r9, %r10			// r10 high, r9 low
	EXPECT	%r10, 1, 0x21
	EXPECT	%r9, 0xfffffffffffffffe, 0x22
	// RORX, flags untouched
	stc
	mov	$1, %eax
	rorxl	$1, %eax, %ebx
	setc	%cl
	EXPECT	%rbx, 0x80000000, 0x30
	movzbq	%cl, %rcx
	EXPECT	%rcx, 1, 0x31
	// SHLX/SARX/SHRX: the count masked to 6 bits (64-bit)
	mov	$65, %rcx
	mov	$1, %rax
	shlxq	%rcx, %rax, %rbx
	EXPECT	%rbx, 2, 0x40
	mov	$0x8000000000000000, %rax
	sarxq	%rcx, %rax, %rbx
	EXPECT	%rbx, 0xc000000000000000, 0x41
	shrxq	%rcx, %rax, %rbx
	EXPECT	%rbx, 0x4000000000000000, 0x42
	// BZHI: index >= the size sets CF and keeps the source
	mov	$70, %rcx
	mov	$0x1234, %rax
	bzhiq	%rcx, %rax, %rbx
	setc	%dl
	movzbq	%dl, %rdx
	EXPECT	%rbx, 0x1234, 0x50
	EXPECT	%rdx, 1, 0x51
	mov	$8, %rcx
	bzhiq	%rcx, %rax, %rbx
	EXPECT	%rbx, 0x34, 0x52
	// BLSMSK: ZF always cleared, CF = the source was zero
	xor	%eax, %eax
	blsmskl	%eax, %ebx
	setz	%cl
	setc	%dl
	movzbq	%cl, %rcx
	movzbq	%dl, %rdx
	EXPECT	%rbx, 0xffffffff, 0x60
	EXPECT	%rcx, 0, 0x61
	EXPECT	%rdx, 1, 0x62

	lea	endmsg(%rip), %rsi
	call	puts
	jmp	pass

endmsg:	.asciz	"VEX OK\n"
	BOOT64_END
