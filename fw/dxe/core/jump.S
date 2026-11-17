/*
 * core_setjmp/core_longjmp for StartImage/Exit (UEFI 2.10 7.4.2, 7.4.5):
 * the callee-saved registers of the Microsoft x64 convention (RBX, RBP,
 * RDI, RSI, R12-R15, RSP; XMM6-15 too, but the firmware has no SSE).
 * The context is the dxe.h jmp_ctx: rbx rsp rbp rdi rsi r12 r13 r14 r15 rip.
 */
	.text
	.globl	core_setjmp
core_setjmp:			/* rcx = ctx */
	mov	%rbx, 0x00(%rcx)
	lea	8(%rsp), %rax		/* the caller's rsp after the return */
	mov	%rax, 0x08(%rcx)
	mov	%rbp, 0x10(%rcx)
	mov	%rdi, 0x18(%rcx)
	mov	%rsi, 0x20(%rcx)
	mov	%r12, 0x28(%rcx)
	mov	%r13, 0x30(%rcx)
	mov	%r14, 0x38(%rcx)
	mov	%r15, 0x40(%rcx)
	mov	(%rsp), %rax
	mov	%rax, 0x48(%rcx)
	xor	%eax, %eax
	ret

	.globl	core_longjmp
core_longjmp:			/* rcx = ctx, edx = value */
	mov	%edx, %eax
	test	%eax, %eax
	jnz	1f
	inc	%eax
1:	mov	0x00(%rcx), %rbx
	mov	0x08(%rcx), %rsp
	mov	0x10(%rcx), %rbp
	mov	0x18(%rcx), %rdi
	mov	0x20(%rcx), %rsi
	mov	0x28(%rcx), %r12
	mov	0x30(%rcx), %r13
	mov	0x38(%rcx), %r14
	mov	0x40(%rcx), %r15
	jmp	*0x48(%rcx)
