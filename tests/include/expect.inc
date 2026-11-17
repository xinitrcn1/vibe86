// expect.inc: checks and exception catching for 64-bit tests (include after
// boot64.inc; call expect_init first in main64).
//   EXPECT src, value, id   compare a 64-bit operand (register or memory) with
//                           a constant; on a mismatch print "X<id> <value>"
//                           (id in hex) and fail. Clobbers r15 and flags.
//   CATCH label             the next exception resumes at `label` (in the
//                           same ring) with LASTVEC/LASTERR/LASTRIP set; the
//                           vectors 0-31 are caught (handlers at DPL 0)
//   EXPECT_VEC vec, id      LASTVEC = vec (then clears it)
// Exceptions without a CATCH are failures (printed as "E<vector> <rip>").
	.set	LASTVEC, 0x600
	.set	LASTERR, 0x608
	.set	LASTRIP, 0x610
	.set	RESUME, 0x618
	.set	LASTCS, 0x620
	.set	LASTFL, 0x628
	.set	NEXC, 0x630

	.macro	EXPECT src, val, id
	movabsq	$\val, %r15
	cmpq	%r15, \src
	je	9876f
	movq	\src, %r15
	pushq	%r15
	movl	$\id, %r15d
	jmp	expect_failed
9876:
	.endm

	.macro	CATCH label
	pushq	%rax
	leaq	\label(%rip), %rax
	movq	%rax, RESUME
	popq	%rax
	movq	$-1, LASTVEC
	.endm

	.macro	EXPECT_VEC vec, id
	EXPECT	LASTVEC, \vec, \id
	movq	$-1, LASTVEC
	.endm

	.macro	EXC_STUB n
exc_stub_\n:
	.if	(\n == 8) || (\n >= 10 && \n <= 14) || (\n == 17) || (\n == 21) || (\n == 29) || (\n == 30)
	.else
	pushq	$0
	.endif
	pushq	$\n
	jmp	exc_common
	.endm

	.irp	n, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
	EXC_STUB \n
	.endr

exc_common:
	// [rsp] vector, error code, RIP, CS, RFLAGS, RSP, SS
	push	%rax
	mov	8(%rsp), %rax
	mov	%rax, LASTVEC
	mov	16(%rsp), %rax
	mov	%rax, LASTERR
	mov	24(%rsp), %rax
	mov	%rax, LASTRIP
	mov	32(%rsp), %rax
	mov	%rax, LASTCS
	mov	40(%rsp), %rax
	mov	%rax, LASTFL
	incq	NEXC
	mov	RESUME, %rax
	test	%rax, %rax
	jz	1f
	mov	%rax, 24(%rsp)
	movq	$0, RESUME
	pop	%rax
	add	$16, %rsp
	iretq
1:	mov	$'E', %al
	call	putc
	mov	LASTVEC, %rax
	call	puthex16
	mov	$' ', %al
	call	putc
	mov	LASTRIP, %rax
	call	puthex16
	jmp	fail

expect_failed:
	mov	$'X', %al
	call	putc
	mov	%r15, %rax
	call	puthex16
	mov	$' ', %al
	call	putc
	pop	%rax
	call	puthex16
	jmp	fail

// puthex16: RAX as 16 hex digits (no newline)
puthex16:
	push	%rcx
	push	%rdx
	mov	%rax, %rdx
	mov	$16, %ecx
1:	rol	$4, %rdx
	mov	%edx, %eax
	and	$15, %eax
	cmp	$10, %eax
	jb	2f
	add	$'a' - '0' - 10, %eax
2:	add	$'0', %eax
	call	putc
	dec	%ecx
	jnz	1b
	pop	%rdx
	pop	%rcx
	ret

// expect_init: catch every exception vector 0-31 (DPL 0 interrupt gates)
expect_init:
	push	%rax
	push	%rcx
	push	%rdx
	push	%rsi
	movq	$0, RESUME
	movq	$-1, LASTVEC
	movq	$0, NEXC
	xor	%ecx, %ecx
	lea	exc_table(%rip), %rsi
1:	mov	(%rsi, %rcx, 8), %rax
	mov	$0x8e, %edx
	call	set_gate
	inc	%ecx
	cmp	$32, %ecx
	jne	1b
	pop	%rsi
	pop	%rdx
	pop	%rcx
	pop	%rax
	ret

	.balign	8
exc_table:
	.irp	n, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
	.quad	BASE + exc_stub_\n
	.endr
