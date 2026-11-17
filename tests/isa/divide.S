// divide: sign extension into rDX and division (the SeaBIOS e820 bug: CDQ
// compiled from the manual's 'EDX := SignExtend(EAX)' copied EAX into EDX,
// SDM errata E-17), at every operand size and around the boundaries:
//   CBW/CWDE/CDQE, CWD/CDQ/CQO with the sign bit set and clear, upper bits
//   of RDX/RAX that must be kept (16-bit) or zeroed (32-bit);
//   IDIV of negative dividends by positive and negative divisors (quotient
//   rounds toward zero, the remainder takes the dividend's sign);
//   DIV with rDX:rAX dividends; #DE for a zero divisor and for quotients
//   that don't fit (INT_MIN / -1, 8-bit AX / 1), with the registers unchanged.
// expect-out: DIV OK
#include "boot64.inc"
#include "expect.inc"

main64:
	call	expect_init

	// ---- sign extension
	movabsq	$0x1111111111118080, %rax
	cbw					// AX := SignExtend(AL)
	EXPECT	%rax, 0x111111111111ff80, 0x01
	cwtl					// EAX := SignExtend(AX), upper zeroed
	EXPECT	%rax, 0x00000000ffffff80, 0x02
	cltq					// RAX := SignExtend(EAX)
	EXPECT	%rax, 0xffffffffffffff80, 0x03
	movabsq	$0x2222222222227fff, %rax
	movabsq	$0x3333333333333333, %rdx
	cwtd					// DX := sign of AX, upper RDX kept
	EXPECT	%rdx, 0x3333333333330000, 0x04
	EXPECT	%rax, 0x2222222222227fff, 0x05
	movw	$0x8000, %ax
	cwtd
	EXPECT	%rdx, 0x333333333333ffff, 0x06
	movabsq	$0x4444444480000000, %rax
	movabsq	$0x5555555555555555, %rdx
	cltd					// EDX := sign of EAX, upper RDX zeroed
	EXPECT	%rdx, 0x00000000ffffffff, 0x07
	EXPECT	%rax, 0x4444444480000000, 0x08	// CDQ doesn't touch RAX
	movabsq	$0x444444447fffffff, %rax
	cltd
	EXPECT	%rdx, 0, 0x09
	movabsq	$0x8000000000000000, %rax
	cqto
	EXPECT	%rdx, 0xffffffffffffffff, 0x0a
	movabsq	$0x7fffffffffffffff, %rax
	cqto
	EXPECT	%rdx, 0, 0x0b

	// ---- IDIV: -7 / 2 = -3 rem -1; 7 / -2 = -3 rem 1; -7 / -2 = 3 rem -1
	mov	$-7, %eax
	cltd
	mov	$2, %ecx
	idiv	%ecx
	EXPECT	%rax, 0xfffffffd, 0x10
	EXPECT	%rdx, 0xffffffff, 0x11
	mov	$7, %eax
	cltd
	mov	$-2, %ecx
	idiv	%ecx
	EXPECT	%rax, 0xfffffffd, 0x12
	EXPECT	%rdx, 1, 0x13
	mov	$-7, %rax
	cqto
	mov	$-2, %rcx
	idiv	%rcx
	EXPECT	%rax, 3, 0x14
	EXPECT	%rdx, 0xffffffffffffffff, 0x15
	movabsq	$0x1234567812345678, %rax
	mov	$-7, %ax			// 16-bit: upper bits of RAX/RDX stay
	cwtd
	mov	$3, %cx
	idiv	%cx
	EXPECT	%rax, 0x123456781234fffe, 0x16
	EXPECT	%rdx, 0xffffffffffffffff, 0x17	// DX := remainder -1
	mov	$-100, %ax			// 8-bit: AL quotient, AH remainder
	mov	$7, %cl
	idiv	%cl
	EXPECT	%rax, 0x123456781234fef2, 0x18	// AH = -2, AL = -14
	// the e820 computation SeaBIOS does: a 64-bit size / 1024 in 32-bit halves
	mov	$0x9fc00, %eax
	cltd
	mov	$1024, %ecx
	idiv	%ecx
	EXPECT	%rax, 0x27f, 0x19
	EXPECT	%rdx, 0, 0x1a
	// MIN / 1 fits; MIN / 2
	movabsq	$0x8000000000000000, %rax
	cqto
	mov	$1, %rcx
	idiv	%rcx
	EXPECT	%rax, 0x8000000000000000, 0x1b
	movabsq	$0x8000000000000000, %rax
	cqto
	mov	$2, %rcx
	idiv	%rcx
	EXPECT	%rax, 0xc000000000000000, 0x1c

	// ---- DIV with a full rDX:rAX dividend
	movabsq	$0x0000000000000001, %rdx
	movabsq	$0x0000000000000000, %rax	// 2^64
	movabsq	$0x0000000100000000, %rcx
	div	%rcx
	EXPECT	%rax, 0x100000000, 0x20
	EXPECT	%rdx, 0, 0x21
	mov	$0xfffffffe, %edx
	mov	$0xffffffff, %eax
	mov	$0xffffffff, %ecx
	div	%ecx				// (2^64 - 2^32 - 1) / (2^32 - 1)
	EXPECT	%rax, 0xffffffff, 0x22
	EXPECT	%rdx, 0xfffffffe, 0x23

	// ---- #DE: zero divisor, and quotients that don't fit; nothing written
	movabsq	$0x8000000000000000, %rax
	cqto
	mov	$-1, %rcx
	CATCH	1f
	idiv	%rcx				// INT64_MIN / -1
1:	EXPECT_VEC 0, 0x30
	EXPECT	%rax, 0x8000000000000000, 0x31
	EXPECT	%rdx, 0xffffffffffffffff, 0x32
	mov	$0x80000000, %eax
	cltd
	mov	$-1, %ecx
	CATCH	1f
	idiv	%ecx				// INT32_MIN / -1
1:	EXPECT_VEC 0, 0x33
	mov	$0x8000, %ax
	cwtd
	mov	$-1, %cx
	CATCH	1f
	idiv	%cx
1:	EXPECT_VEC 0, 0x34
	mov	$0xff80, %ax			// -128 / -1 = 128: doesn't fit in AL
	mov	$-1, %cl
	CATCH	1f
	idiv	%cl
1:	EXPECT_VEC 0, 0x35
	mov	$0x100, %ax			// 256 / 1: doesn't fit in AL
	mov	$1, %cl
	CATCH	1f
	div	%cl
1:	EXPECT_VEC 0, 0x36
	mov	$5, %rdx			// rDX >= divisor
	mov	$5, %rcx
	CATCH	1f
	div	%rcx
1:	EXPECT_VEC 0, 0x37
	xor	%ecx, %ecx
	CATCH	1f
3:	div	%ecx
1:	EXPECT_VEC 0, 0x38
	lea	3b(%rip), %rax			// #DE is a fault: RIP of the DIV
	cmp	%rax, LASTRIP
	jne	fail
	lea	endmsg(%rip), %rsi
	call	puts
	jmp	pass

endmsg:	.asciz	"DIV OK\n"
	BOOT64_END
