// apic: the local APIC (D-017; Intel SDM Vol. 3A ch. 12) in xAPIC and
// x2APIC modes.
//   xAPIC MMIO at FEE0_0000h: version 0006_0015h, ID 0; SVR software enable
//   CR8 is TPR[7:4] both ways
//   a self-IPI (ICR, shorthand self) is delivered; EOI clears its ISR bit
//   TPR masks a class at or below it (the vector waits in IRR) until TPR drops
//   two pending vectors are taken highest class first
//   a self-IPI with vector 5 sets ESR's send-illegal-vector bit (5)
//   the timer: one-shot (current count 0 after), periodic (3 interrupts)
//   x2APIC (IA32_APIC_BASE.EXTD): RDMSR of the ID and version, #GP for a
//   read of EOI (write-only), #GP for the x2APIC -> xAPIC transition,
//   SELF IPI (83Fh), the TSC-deadline timer (IA32_TSC_DEADLINE)
// expect-out: XAPIC
// expect-out: TIMER
// expect-out: X2APIC
// expect-out: APIC OK
// vsim-args: -b test -c 40000000
#include "boot64.inc"
#include "expect.inc"
	.set	LOG, 0x8000			// vectors in the order taken
	.set	LOGN, 0x8100
	.set	X2, 0x8108			// handlers EOI through x2APIC MSRs

	// a handler: log the vector, EOI
	.macro	HANDLER v
h_\v:	push	%rax
	push	%rcx
	push	%rdx
	mov	LOGN, %rax
	movb	$\v, LOG(%rax)
	incq	LOGN
	cmpq	$0, X2
	jne	1f
	mov	$0xfee000b0, %eax		// xAPIC EOI
	movl	$0, (%rax)
	jmp	2f
1:	mov	$0x80b, %ecx			// x2APIC EOI
	xor	%eax, %eax
	xor	%edx, %edx
	wrmsr
2:	pop	%rdx
	pop	%rcx
	pop	%rax
	iretq
	.endm

	// wait until LOGN reaches n (bounded)
	.macro	WAITLOG n, id
	mov	$200000, %ecx
1:	cmpq	$\n, LOGN
	jae	2f
	dec	%ecx
	jnz	1b
2:	EXPECT	LOGN, \n, \id
	.endm

	.macro	GATE v
	lea	h_\v(%rip), %rax
	mov	$\v, %ecx
	mov	$0x8e, %edx
	call	set_gate
	.endm

main64:
	call	expect_init
	GATE	0x40
	GATE	0x41
	GATE	0x45
	GATE	0x50
	GATE	0x51
	GATE	0x52
	GATE	0x60
	GATE	0x81
	movq	$0, LOGN
	movq	$0, X2
	mov	$0xfee00000, %ebx

	// ---- xAPIC registers
	movl	0x30(%rbx), %eax
	EXPECT	%rax, 0x00060015, 0x01
	movl	0x20(%rbx), %eax
	EXPECT	%rax, 0, 0x02
	movl	0xf0(%rbx), %eax
	EXPECT	%rax, 0xff, 0x03		// software-disabled at reset
	movl	$0x1ff, 0xf0(%rbx)		// enable, spurious vector FFh
	mov	$5, %rax			// CR8 -> TPR
	mov	%rax, %cr8
	movl	0x80(%rbx), %eax
	EXPECT	%rax, 0x50, 0x04
	movl	$0x30, 0x80(%rbx)		// TPR -> CR8
	mov	%cr8, %rax
	EXPECT	%rax, 3, 0x05
	movl	$0, 0x80(%rbx)

	// ---- a self-IPI; EOI clears ISR
	sti
	movl	$0x40040, 0x300(%rbx)		// vector 40h, shorthand self
	WAITLOG	1, 0x06
	movzbl	LOG, %eax
	EXPECT	%rax, 0x40, 0x07
	movl	0x120(%rbx), %eax		// ISR[95:64]: vector 40h is bit 0
	EXPECT	%rax, 0, 0x08

	// ---- TPR masks class 4 at TPR 50h; the vector waits in IRR
	movl	$0x50, 0x80(%rbx)
	movl	$0x40045, 0x300(%rbx)
	mov	$2000, %ecx
1:	dec	%ecx
	jnz	1b
	EXPECT	LOGN, 1, 0x09
	movl	0x220(%rbx), %eax		// IRR[95:64]: 45h is bit 5
	and	$0x20, %eax
	EXPECT	%rax, 0x20, 0x0a
	movl	$0, 0x80(%rbx)
	WAITLOG	2, 0x0b
	movzbl	LOG + 1, %eax
	EXPECT	%rax, 0x45, 0x0c

	// ---- priority: 41h and 81h pending, 81h first
	cli
	movl	$0x40041, 0x300(%rbx)
	movl	$0x40081, 0x300(%rbx)
	mov	$2000, %ecx
1:	dec	%ecx
	jnz	1b
	sti
	WAITLOG	4, 0x0d
	movzbl	LOG + 2, %eax
	EXPECT	%rax, 0x81, 0x0e
	movzbl	LOG + 3, %eax
	EXPECT	%rax, 0x41, 0x0f

	// ---- ESR: an illegal vector
	movl	$0x40005, 0x300(%rbx)
	movl	$0, 0x280(%rbx)			// latch the errors
	movl	0x280(%rbx), %eax
	EXPECT	%rax, 0x20, 0x10
	lea	m_xapic(%rip), %rsi
	call	puts

	// ---- the timer: one-shot, divide by 1
	movl	$0xb, 0x3e0(%rbx)
	movl	$0x50, 0x320(%rbx)
	movl	$1000, 0x380(%rbx)
	WAITLOG	5, 0x11
	movzbl	LOG + 4, %eax
	EXPECT	%rax, 0x50, 0x12
	movl	0x390(%rbx), %eax
	EXPECT	%rax, 0, 0x13
	// periodic: three interrupts, then masked
	movl	$0x20051, 0x320(%rbx)
	movl	$30000, 0x380(%rbx)		// a period well above the handler's time (DDR5)
	WAITLOG	8, 0x14
	movl	$0x10051, 0x320(%rbx)
	movzbl	LOG + 7, %eax
	EXPECT	%rax, 0x51, 0x15
	mov	$3000, %ecx			// any period already pending lands first
1:	dec	%ecx
	jnz	1b
	movq	$0, LOGN			// a fresh log for the x2APIC part
	lea	m_timer(%rip), %rsi
	call	puts

	// ---- x2APIC
	mov	$0x1b, %ecx
	rdmsr
	or	$0xc00, %eax
	wrmsr
	movq	$1, X2
	mov	$0x802, %ecx			// ID
	rdmsr
	EXPECT	%rax, 0, 0x16
	mov	$0x803, %ecx			// version
	rdmsr
	EXPECT	%rax, 0x00060015, 0x17
	CATCH	1f
	mov	$0x80b, %ecx			// EOI is write-only: #GP
	rdmsr
1:	EXPECT_VEC 13, 0x18
	mov	$0x1b, %ecx			// x2APIC -> xAPIC: #GP
	rdmsr
	and	$~0x400, %eax
	CATCH	1f
	wrmsr
1:	EXPECT_VEC 13, 0x19
	mov	$0x83f, %ecx			// SELF IPI
	mov	$0x60, %eax
	xor	%edx, %edx
	wrmsr
	WAITLOG	1, 0x1a
	movzbl	LOG, %eax
	EXPECT	%rax, 0x60, 0x1b
	// the TSC-deadline timer
	mov	$0x832, %ecx			// LVT timer: TSC-deadline mode, vector 52h
	mov	$0x40052, %eax
	xor	%edx, %edx
	wrmsr
	rdtsc
	add	$3000, %eax
	adc	$0, %edx
	mov	$0x6e0, %ecx
	wrmsr
	WAITLOG	2, 0x1c
	movzbl	LOG + 1, %eax
	EXPECT	%rax, 0x52, 0x1d
	mov	$0x6e0, %ecx			// disarmed after it fired
	rdmsr
	EXPECT	%rax, 0, 0x1e
	lea	m_x2(%rip), %rsi
	call	puts
	lea	m_ok(%rip), %rsi
	call	puts
	jmp	pass

	HANDLER	0x40
	HANDLER	0x41
	HANDLER	0x45
	HANDLER	0x50
	HANDLER	0x51
	HANDLER	0x52
	HANDLER	0x60
	HANDLER	0x81

m_xapic: .asciz	"XAPIC\n"
m_timer: .asciz	"TIMER\n"
m_x2:	.asciz	"X2APIC\n"
m_ok:	.asciz	"APIC OK\n"
	BOOT64_END
