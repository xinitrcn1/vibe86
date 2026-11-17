// system64: system instructions and events in IA-32e mode.
//   SYSCALL from ring 3 to LSTAR and SYSRETQ back (EFER.SCE, STAR, FMASK)
//   single-step traps (EFLAGS.TF, DR6.BS)
//   a data breakpoint (DR0, write, 4 bytes: a trap after the store, DR6.B0)
//   an instruction breakpoint (DR1: a fault before the instruction; RF to go on)
//   #DF: a page fault whose gate isn't present (#NP while delivering #PF)
// expect-out: SC
// expect-out: R3
// expect-out: SS
// expect-out: DB
// expect-out: IB
// expect-out: DF
#include "boot64.inc"

main64:
	lea	h_db(%rip), %rax
	mov	$1, %ecx
	mov	$0x8e, %edx
	call	set_gate
	lea	h_df(%rip), %rax
	mov	$8, %ecx
	mov	$0x8e, %edx
	call	set_gate
	lea	h_80(%rip), %rax
	mov	$0x80, %ecx
	mov	$0xee, %edx
	call	set_gate
	// #PF: a gate that isn't present (P = 0)
	lea	h_df(%rip), %rax
	mov	$14, %ecx
	mov	$0x0e, %edx
	call	set_gate

	// --- SYSCALL / SYSRET
	mov	$0xc0000080, %ecx		// EFER.SCE
	rdmsr
	or	$1, %eax
	wrmsr
	mov	$0xc0000081, %ecx		// STAR: SYSRET base 0x18, SYSCALL CS 0x18
	xor	%eax, %eax
	mov	$0x00180018, %edx
	wrmsr
	mov	$0xc0000082, %ecx		// LSTAR
	lea	sc_entry(%rip), %rax
	mov	%rax, %rdx
	shr	$32, %rdx
	wrmsr
	mov	$0xc0000084, %ecx		// FMASK: clear IF, TF
	mov	$0x300, %eax
	xor	%edx, %edx
	wrmsr
	push	$0x20 | 3
	push	$USTACK
	pushfq
	lea	ring3(%rip), %rax
	push	$0x28 | 3
	push	%rax
	iretq

ring3:
	mov	$0x1234, %r12
	syscall
after_sc:
	cmp	$0x5678, %r12			// set by the kernel
	jne	fail
	mov	%cs, %ax
	and	$3, %ax
	cmp	$3, %ax				// back in ring 3
	jne	fail
	int	$0x80

sc_entry:
	// ring 0, CS from STAR, RCX = the return RIP, R11 = RFLAGS
	mov	%cs, %ax
	cmp	$0x18, %ax
	jne	fail
	lea	after_sc(%rip), %rax
	cmp	%rax, %rcx
	jne	fail
	cmp	$0x1234, %r12
	jne	fail
	mov	$0x5678, %r12
	lea	msg_sc(%rip), %rsi
	call	puts
	sysretq

h_80:
	lea	msg_r3(%rip), %rsi
	call	puts
	mov	$KSTACK, %rsp

	// --- single step: three traps, then the handler clears TF
	xor	%r15, %r15
	pushfq
	orq	$0x100, (%rsp)
	popfq
	nop
	nop
	nop
	nop
	nop
	cmp	$3, %r15
	jb	fail
	lea	msg_ss(%rip), %rsi
	call	puts

	// --- data breakpoint on `watch` (DR0, R/W 01 write, LEN 11 = 4 bytes)
	lea	watch(%rip), %rax
	mov	$0x2000, %rax			// a variable in SRAM (the image is ROM)
	mov	%rax, %dr0
	mov	$0x000d0001, %eax
	mov	%rax, %dr7
	movl	$7, 0x2000
	// --- instruction breakpoint (DR1, R/W 00)
	lea	ibp(%rip), %rax
	mov	%rax, %dr1
	mov	$0x4, %eax
	mov	%rax, %dr7
	jmp	ibp
ibp:
	xor	%eax, %eax
	mov	%rax, %dr7

	// --- #DF: touch memory above the mapped 4 GiB; the #PF gate isn't present
	movabs	$0x100000000, %rax
	mov	(%rax), %rax
	jmp	fail

h_db:
	mov	%dr6, %rax
	bt	$14, %rax			// BS: single step
	jnc	1f
	inc	%r15
	cmp	$3, %r15
	jb	9f
	andq	$~0x100, 16(%rsp)		// clear TF in the saved RFLAGS
	jmp	9f
1:	bt	$0, %rax			// B0: the data breakpoint
	jnc	2f
	cmpl	$7, 0x2000			// a trap: the store has happened
	jne	fail
	lea	msg_db(%rip), %rsi
	call	puts
	jmp	9f
2:	bt	$1, %rax			// B1: the instruction breakpoint
	jnc	fail
	lea	ibp(%rip), %rcx
	cmp	(%rsp), %rcx			// a fault: RIP is the breakpoint's
	jne	fail
	orq	$0x10000, 16(%rsp)		// RF: go on without re-triggering
	lea	msg_ib(%rip), %rsi
	call	puts
9:	xor	%eax, %eax
	mov	%rax, %dr6
	iretq

h_df:
	cmpq	$0, (%rsp)			// #DF's error code is 0
	jne	fail
	lea	msg_df(%rip), %rsi
	call	puts
	jmp	pass

watch:
msg_sc:	.asciz	"SC\n"
msg_r3:	.asciz	"R3\n"
msg_ss:	.asciz	"SS\n"
msg_db:	.asciz	"DB\n"
msg_ib:	.asciz	"IB\n"
msg_df:	.asciz	"DF\n"

	BOOT64_END
