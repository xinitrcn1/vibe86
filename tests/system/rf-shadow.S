// rf-shadow: RFLAGS.RF and the MOV SS interrupt shadow (Intel SDM Vol. 3A
// 18.3.1.1 "Instruction-Breakpoint Exception Condition", 7.8.3 "Masking
// Exceptions and Interrupts When Switching Stacks").
//   An instruction breakpoint (DR0) faults before the instruction; the
//   handler returns with IRETQ and RF = 1 in the frame, so the instruction
//   completes once without a second #DB (RF was cleared by an IRETQ that
//   restored it once); RF clears after it, so a second pass faults again.
//   Single-stepping (TF) over MOV SS: no trap after the MOV SS (the trap
//   is held until the instruction after it), traps after the others.
// expect-out: RF OK
#include "boot64.inc"
#include "expect.inc"
	.set	NDB, 0x700
	.set	DBRIPS, 0x708			// up to 8 RIPs

main64:
	call	expect_init
	lea	h_db(%rip), %rax
	mov	$1, %ecx
	mov	$0x8e, %edx
	call	set_gate

	// ---- instruction breakpoint, twice through the same instruction
	movq	$0, NDB
	lea	bp(%rip), %rax
	mov	%rax, %dr0
	mov	$0x401, %eax			// L0, execute, 1 byte
	mov	%rax, %dr7
	xor	%ecx, %ecx
again:
bp:	inc	%ecx				// faults once per pass
	cmp	$2, %ecx
	jne	again
	xor	%eax, %eax
	mov	%rax, %dr7
	EXPECT	NDB, 2, 0x01			// one #DB per pass, not a loop
	EXPECT	%rcx, 2, 0x02
	lea	bp(%rip), %rax
	cmp	%rax, DBRIPS
	jne	fail
	cmp	%rax, DBRIPS + 8
	jne	fail

	// ---- TF over MOV SS
	movq	$0, NDB
	mov	%ss, %bx
	pushfq
	orq	$0x100, (%rsp)
	popfq
	nop
s_a:	mov	%bx, %ss
s_b:	nop
s_c:	nop
s_d:	pushfq
	andq	$~0x100, (%rsp)
	popfq
	// the traps' RIPs: none may be s_b (right after MOV SS); s_c must be there
	mov	NDB, %rcx
	lea	s_b(%rip), %rdx
	lea	s_c(%rip), %rsi
	xor	%edi, %edi
	xor	%r8d, %r8d
1:	test	%rcx, %rcx
	jz	2f
	dec	%rcx
	mov	DBRIPS(, %rcx, 8), %rax
	cmp	%rdx, %rax
	jne	3f
	inc	%edi
3:	cmp	%rsi, %rax
	jne	1b
	inc	%r8d
	jmp	1b
2:	EXPECT	%rdi, 0, 0x10			// no trap between MOV SS and the next
	EXPECT	%r8, 1, 0x11			// the trap after it
	cmpq	$3, NDB
	jb	fail

	lea	endmsg(%rip), %rsi
	call	puts
	jmp	pass

// #DB: record RIP, clear DR6, return with RF = 1
h_db:
	push	%rax
	push	%rcx
	mov	NDB, %rcx
	cmp	$8, %rcx
	jae	1f
	mov	16(%rsp), %rax
	mov	%rax, DBRIPS(, %rcx, 8)
1:	incq	NDB
	cmpq	$16, NDB
	ja	fail				// a #DB storm: RF didn't work
	xor	%eax, %eax
	mov	%rax, %dr6
	orq	$0x10000, 32(%rsp)		// RF in the frame's RFLAGS
	pop	%rcx
	pop	%rax
	iretq

endmsg:	.asciz	"RF OK\n"
	BOOT64_END
