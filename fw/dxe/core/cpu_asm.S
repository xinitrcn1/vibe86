/*
 * The DXE core's exception entry stubs (Intel SDM Vol. 3A 7.12 "Exception
 * and Interrupt Handling", 64-bit stack frame 7.14.2): each pushes a dummy
 * error code where the CPU pushes none, then its vector, saves the general
 * registers and calls cpu_exception(frame) (cpu.c).
 */
	.text
	.macro	STUB vec, err
	.globl	exc_stub_\vec
exc_stub_\vec:
	.if	\err == 0
	push	$0
	.endif
	push	$\vec
	jmp	exc_common
	.endm

	STUB 0, 0
	STUB 1, 0
	STUB 2, 0
	STUB 3, 0
	STUB 4, 0
	STUB 5, 0
	STUB 6, 0
	STUB 7, 0
	STUB 8, 1
	STUB 9, 0
	STUB 10, 1
	STUB 11, 1
	STUB 12, 1
	STUB 13, 1
	STUB 14, 1
	STUB 15, 0
	STUB 16, 0
	STUB 17, 1
	STUB 18, 0
	STUB 19, 0
	STUB 20, 0
	STUB 21, 1
	STUB 22, 0
	STUB 23, 0
	STUB 24, 0
	STUB 25, 0
	STUB 26, 0
	STUB 27, 0
	STUB 28, 0
	STUB 29, 1
	STUB 30, 1
	STUB 31, 0

exc_common:
	push	%rax
	push	%rbx
	push	%rcx
	push	%rdx
	push	%rsi
	push	%rdi
	push	%rbp
	push	%r8
	push	%r9
	push	%r10
	push	%r11
	push	%r12
	push	%r13
	push	%r14
	push	%r15
	mov	%rsp, %rcx
	mov	%rsp, %rbx
	and	$-16, %rsp
	sub	$32, %rsp
	call	cpu_exception
	mov	%rbx, %rsp
	pop	%r15
	pop	%r14
	pop	%r13
	pop	%r12
	pop	%r11
	pop	%r10
	pop	%r9
	pop	%r8
	pop	%rbp
	pop	%rdi
	pop	%rsi
	pop	%rdx
	pop	%rcx
	pop	%rbx
	pop	%rax
	add	$16, %rsp
	iretq

	/* other vectors: a spurious or unexpected interrupt is ignored */
	.globl	irq_ignore
irq_ignore:
	iretq

	/* load the GDT and reload the segment registers: cpu_load_gdt(gdtr) */
	.globl	cpu_load_gdt
cpu_load_gdt:
	lgdt	(%rcx)
	mov	$0x10, %ax
	mov	%ax, %ds
	mov	%ax, %es
	mov	%ax, %ss
	mov	%ax, %fs
	mov	%ax, %gs
	pop	%rax
	push	$0x18
	push	%rax
	lretq

	.data
	.globl	exc_stubs
	.balign	8
exc_stubs:
	.irp	v, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
	.quad	exc_stub_\v
	.endr
