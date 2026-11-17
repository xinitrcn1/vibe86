// ioapic: interrupts from the I/O die (D-017).
//   the I/O APIC at FEC0_0000h: version 0017_0020h (24 entries), ID 8 (the
//     MADT's), entries masked after reset
//   the 8254's IRQ 0 on the I/O APIC's pin 2 (the MADT's override), edge-
//     triggered, vector 30h: one rising edge, one interrupt, as an MSI over
//     the I/O link into the local APIC
//   the same pin level-triggered: while the 8254's output stays high every
//     EOI (the local APIC's EOI message to the I/O APIC's EOI register)
//     clears Remote IRR and the interrupt comes again; masked (by the
//     handler, at the third), it stops
//   a device's MSI (QPORT's completion MSI, vector 31h) through the MSI
//     window, the I/O link and the APIC bus
// expect-out: IOAPIC OK
// vsim-args: -b test -c 40000000
#include "boot64.inc"
#include "expect.inc"
	.set	LOG, 0x8000
	.set	LOGN, 0x8100
	.set	IOA, 0xfec00000
	.set	QPORT_CTL, 0xfe006000

	.macro	HANDLER v
h_\v:	push	%rax
	mov	LOGN, %rax
	movb	$\v, LOG(%rax)
	incq	LOGN
	mov	$0xfee000b0, %eax		// EOI
	movl	$0, (%rax)
	pop	%rax
	iretq
	.endm

	.macro	WAITLOG n, id
	mov	$200000, %ecx
1:	cmpq	$\n, LOGN
	jae	2f
	dec	%ecx
	jnz	1b
2:	EXPECT	LOGN, \n, \id
	.endm

	// I/O APIC register r <- v / -> eax
	.macro	IOW r, v
	movl	$\r, (%rbx)
	movl	$\v, 0x10(%rbx)
	.endm
	.macro	IOR r
	movl	$\r, (%rbx)
	movl	0x10(%rbx), %eax
	.endm

	// the 8254's channel 0 in mode 0 (OUT goes high at the terminal count
	// and stays high) with a count of n
	.macro	PIT0 n
	mov	$0x30, %al
	out	%al, $0x43
	mov	$(\n & 0xff), %al
	out	%al, $0x40
	mov	$(\n >> 8), %al
	out	%al, $0x40
	.endm

main64:
	call	expect_init
	lea	h_0x30(%rip), %rax
	mov	$0x30, %ecx
	mov	$0x8e, %edx
	call	set_gate
	lea	h_0x31(%rip), %rax
	mov	$0x31, %ecx
	mov	$0x8e, %edx
	call	set_gate
	movq	$0, LOGN
	mov	$0xfee00000, %eax
	movl	$0x1ff, 0xf0(%rax)		// the local APIC software-enabled
	mov	$IOA, %ebx

	// ---- registers
	IOR	1
	EXPECT	%rax, 0x00170020, 0x01
	IOR	0
	EXPECT	%rax, 0x08000000, 0x02
	IOR	0x14				// pin 2, low half: masked
	EXPECT	%rax, 0x10000, 0x03

	// ---- edge: the 8254 on pin 2, vector 30h, physical destination 0
	sti
	IOW	0x15, 0
	IOW	0x14, 0x30
	PIT0	200
	WAITLOG	1, 0x04
	movzbl	LOG, %eax
	EXPECT	%rax, 0x30, 0x05
	mov	$20000, %ecx			// OUT stays high: no second edge
1:	dec	%ecx
	jnz	1b
	EXPECT	LOGN, 1, 0x06

	// ---- level, active high: OUT is still high, so it fires at once and
	// again after every EOI. The redelivery beats the interrupted
	// instruction (only STI and MOV SS hold interrupts off for one
	// instruction, IRET doesn't: Intel SDM Vol. 3A "Masking Maskable
	// Hardware Interrupts"), so the main line can't count: vector 32h's handler masks the
	// pin itself before its third EOI, and then nothing more comes
	lea	h_level(%rip), %rax
	mov	$0x32, %ecx
	mov	$0x8e, %edx
	call	set_gate
	IOW	0x14, 0x8032			// vector 32h, trigger mode level (bit 15)
	WAITLOG	3, 0x07
	mov	$20000, %ecx
1:	dec	%ecx
	jnz	1b
	EXPECT	LOGN, 3, 0x0a			// masked: no fourth
	movzbl	LOG+2, %eax
	EXPECT	%rax, 0x32, 0x0b
	movq	$0, LOGN

	// ---- a device's MSI: QPORT's completion MSI on a POP
	mov	$QPORT_CTL, %edx
	movq	$0x31, 0x38(%rdx)		// MSI data: vector 31h, fixed, edge
	mov	$0xfee00000, %eax		// MSI address: APIC ID 0, physical
	mov	%rax, 0x30(%rdx)
	movq	$0, 0x08(%rdx)			// POP
	WAITLOG	1, 0x08
	movzbl	LOG, %eax
	EXPECT	%rax, 0x31, 0x09

	lea	m_ok(%rip), %rsi
	call	puts
	jmp	pass

	HANDLER	0x30
	HANDLER	0x31

h_level:					// vector 32h: log, mask pin 2 at the third
	push	%rax
	push	%rdx
	mov	LOGN, %rax
	movb	$0x32, LOG(%rax)
	incq	LOGN
	cmpq	$3, LOGN
	jb	1f
	mov	$IOA, %edx
	movl	$0x14, (%rdx)
	movl	$0x18032, 0x10(%rdx)		// masked (bit 16), level, vector 32h
1:	mov	$0xfee000b0, %eax		// EOI: Remote IRR clears; unmasked, it comes again
	movl	$0, (%rax)
	pop	%rdx
	pop	%rax
	iretq

m_ok:	.asciz	"IOAPIC OK\n"
	BOOT64_END
