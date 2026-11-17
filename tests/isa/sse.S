// sse: the legacy SSE/SSE2 gating, faults and a few results with known values
// (Intel SDM Vol. 1 sec. 13.1 and Vol. 3A sec. 2.5 / 13.1; Vol. 2 'SIMD
// Floating-Point Exceptions' of each page; IEEE 754-2008):
//   #UD with CR4.OSFXSR = 0 and with CR0.EM = 1; #NM with CR0.TS = 1;
//   #GP(0) for a misaligned MOVAPS / MOVDQA, none for MOVUPS;
//   ADDPS / MULPD / DIVSS / SQRTSD / CVTTSS2SI / PADDUSB / PMADDWD / PSHUFD /
//   PSRLDQ / PMOVMSKB results; MXCSR: ZE set by a masked 1/0, the integer
//   indefinite for an out-of-range CVTTSD2SI; an unmasked ZE raises #XM (19)
//   with CR4.OSXMMEXCPT = 1 and #UD without, the flag set either way;
//   UCOMISS / COMISS flags, COMISS of a QNaN sets IE, UCOMISS doesn't.
// expect-out: SSE OK
#include "boot64.inc"
#include "expect.inc"

	.set	M, 0x40000			// 16-byte aligned scratch

main64:
	call	expect_init
	// ---- gating: OSFXSR = 0 -> #UD
	CATCH	1f
	addps	%xmm0, %xmm0
1:	EXPECT_VEC 6, 0x01
	mov	%cr4, %rax
	or	$0x200, %rax			// OSFXSR
	mov	%rax, %cr4
	mov	%cr0, %rax
	or	$4, %rax			// EM -> #UD
	mov	%rax, %cr0
	CATCH	1f
	addps	%xmm0, %xmm0
1:	EXPECT_VEC 6, 0x02
	mov	%cr0, %rax
	and	$~4, %rax
	or	$8, %rax			// TS -> #NM
	mov	%rax, %cr0
	CATCH	1f
	addps	%xmm0, %xmm0
1:	EXPECT_VEC 7, 0x03
	clts

	// ---- alignment
	CATCH	1f
	movaps	M + 4, %xmm1
1:	EXPECT_VEC 13, 0x04
	CATCH	1f
	movdqa	%xmm1, M + 8
1:	EXPECT_VEC 13, 0x05
	movabsq	$0x3f8000003f800000, %rax	// 1.0f x 2
	mov	%rax, M + 4
	mov	%rax, M + 12
	movups	M + 4, %xmm1
	movq	%xmm1, %rax
	EXPECT	%rax, 0x3f8000003f800000, 0x06

	// ---- packed and scalar FP
	movabsq	$0x4000000040400000, %rax	// {3.0f, 2.0f}
	movq	%rax, %xmm2
	addps	%xmm1, %xmm2			// {4.0f, 3.0f}
	movq	%xmm2, %rax
	EXPECT	%rax, 0x4040000040800000, 0x07
	movabsq	$0x4008000000000000, %rax	// 3.0
	movq	%rax, %xmm3
	mulpd	%xmm3, %xmm3			// 9.0
	movq	%xmm3, %rax
	EXPECT	%rax, 0x4022000000000000, 0x08
	sqrtsd	%xmm3, %xmm4			// 3.0
	movq	%xmm4, %rax
	EXPECT	%rax, 0x4008000000000000, 0x09
	movl	$0x40490fdb, %eax		// pi
	movd	%eax, %xmm5
	cvttss2si %xmm5, %ecx
	EXPECT	%rcx, 3, 0x0a
	movq	$0, M
	stmxcsr	M
	EXPECT	M, 0x1fa0, 0x0b			// PE from the truncation (+mask bits)
	xorps	%xmm6, %xmm6
	movl	$0x3f800000, %eax
	movd	%eax, %xmm7
	divss	%xmm6, %xmm7			// 1/0 = +inf, ZE
	movd	%xmm7, %eax
	EXPECT	%rax, 0x7f800000, 0x0c
	movq	$0, M
	stmxcsr	M
	EXPECT	M, 0x1fa4, 0x0d
	movabsq	$0x43e0000000000000, %rax	// 2^63: out of range
	movq	%rax, %xmm0
	cvttsd2si %xmm0, %rdx
	EXPECT	%rdx, 0x8000000000000000, 0x0e
	movq	$0, M
	stmxcsr	M
	EXPECT	M, 0x1fa5, 0x0f			// + IE

	// ---- unmasked ZE: #XM with OSXMMEXCPT, #UD without; ZE set either way
	movl	$0x1d80, M			// ZM clear, flags clear
	ldmxcsr	M
	movl	$0x3f800000, %eax		// 1.0 again (xmm7 is +inf: inf/0 is exact)
	movd	%eax, %xmm7
	CATCH	1f
	divss	%xmm6, %xmm7
1:	EXPECT_VEC 6, 0x10
	movq	$0, M
	stmxcsr	M
	EXPECT	M, 0x1d84, 0x11
	mov	%cr4, %rax
	or	$0x400, %rax			// OSXMMEXCPT
	mov	%rax, %cr4
	movl	$0x1d80, M
	ldmxcsr	M
	movd	%xmm7, %eax
	CATCH	1f
	divss	%xmm6, %xmm7
1:	EXPECT_VEC 19, 0x12
	movd	%xmm7, %ecx
	cmp	%eax, %ecx			// the destination is unchanged
	jne	fail
	movl	$0x1f80, M
	ldmxcsr	M

	// ---- integer
	movabsq	$0xf0f0f0f001020304, %rax
	movq	%rax, %xmm1
	movabsq	$0x2020202010101010, %rax
	movq	%rax, %xmm2
	paddusb	%xmm2, %xmm1
	movq	%xmm1, %rax
	EXPECT	%rax, 0xffffffff11121314, 0x13
	movabsq	$0x0002000100040003, %rax	// words 3 4 1 2
	movq	%rax, %xmm1
	movabsq	$0xffff000a00030002, %rax	// words 2 3 10 -1
	movq	%rax, %xmm2
	pmaddwd	%xmm2, %xmm1			// 3*2+4*3 = 18, 1*10 + 2*-1 = 8
	movq	%xmm1, %rax
	EXPECT	%rax, 0x0000000800000012, 0x14
	movabsq	$0x4444444433333333, %rax
	movq	%rax, %xmm1
	pshufd	$0x1b, %xmm1, %xmm2		// reversed: dwords 0 0 3333 4444 -> 4444 3333 0 0
	pshufd	$0xee, %xmm2, %xmm3		// the high qword twice
	movq	%xmm3, %rax
	EXPECT	%rax, 0x3333333344444444, 0x15
	movabsq	$0x8877665544332211, %rax
	movq	%rax, %xmm1
	psrldq	$3, %xmm1
	movq	%xmm1, %rax
	EXPECT	%rax, 0x0000008877665544, 0x16
	pmovmskb %xmm1, %ecx
	EXPECT	%rcx, 0x10, 0x17

	// ---- (U)COMISS
	movl	$0x3f800000, %eax
	movd	%eax, %xmm1			// 1.0
	movl	$0x40000000, %eax
	movd	%eax, %xmm2			// 2.0
	comiss	%xmm2, %xmm1			// 1 < 2: CF
	pushfq
	popq	%rax
	and	$0x8d5, %eax
	EXPECT	%rax, 0x01, 0x18
	movl	$0x7fc00000, %eax
	movd	%eax, %xmm3			// QNaN
	ucomiss	%xmm3, %xmm1			// unordered: ZF PF CF, no IE
	pushfq
	popq	%rax
	and	$0x8d5, %eax
	EXPECT	%rax, 0x45, 0x19
	movq	$0, M
	stmxcsr	M
	EXPECT	M, 0x1f80, 0x1a
	comiss	%xmm3, %xmm1			// IE
	movq	$0, M
	stmxcsr	M
	EXPECT	M, 0x1f81, 0x1b

	lea	endmsg(%rip), %rsi
	call	puts
	jmp	pass

endmsg:	.asciz	"SSE OK\n"
	BOOT64_END
