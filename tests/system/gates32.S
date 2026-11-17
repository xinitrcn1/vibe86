// gates32: protected-mode (32-bit) far transfers.
//   IRET to ring 3
//   ring 3 -> CALL FAR through a DPL-3 call gate -> ring 0 on the TSS stack,
//     one parameter copied; RET FAR 4 back to ring 3
//   ring 3 -> CALL FAR through a task gate -> task B (the old task's state
//     saved in its TSS, B's link = A, NT set); B's IRET returns to task A
// expect-out: CG
// expect-out: TS
// expect-out: BK
	.set	BASE, 0xffff0000
	.set	GDT, 0x1000
	.set	TSSA, 0x30000
	.set	TSSB, 0x31000
	.set	KSTACK, 0x80000
	.set	RSP0, 0x90000
	.set	USTACK, 0xa0000
	.set	BSTACK, 0x70000

	.code16
	.text
start:
	cli
	lgdtl	%cs:gdtr_rom
	mov	%cr0, %eax
	or	$1, %eax
	mov	%eax, %cr0
	ljmpl	$0x08, $BASE + pm32

	.code32
pm32:
	mov	$0x10, %ax
	mov	%ax, %ds
	mov	%ax, %es
	mov	%ax, %ss
	mov	$KSTACK, %esp
	// a writable GDT in SRAM
	mov	$BASE + gdt_rom, %esi
	mov	$GDT, %edi
	mov	$(gdt_end - gdt_rom), %ecx
	rep movsb
	// call gate 0x38: DPL 3, 0x08:gate_entry, 1 parameter
	mov	$BASE + gate_entry, %eax
	mov	%ax, GDT + 0x38
	movw	$0x08, GDT + 0x3a
	movb	$1, GDT + 0x3c
	movb	$0xec, GDT + 0x3d		// P, DPL 3, 32-bit call gate
	shr	$16, %eax
	mov	%ax, GDT + 0x3e
	// TSS A (0x28) and B (0x30): base, limit 0x67, available 32-bit TSS
	movl	$0x00000067 | ((TSSA & 0xffff) << 16), GDT + 0x28
	movl	$0x00008900 | ((TSSA >> 16) & 0xff), GDT + 0x2c
	movl	$0x00000067 | ((TSSB & 0xffff) << 16), GDT + 0x30
	movl	$0x00008900 | ((TSSB >> 16) & 0xff), GDT + 0x34
	// task gate 0x40 -> TSS B, DPL 3
	movl	$0x00300000, GDT + 0x40
	movl	$0x0000e500, GDT + 0x44
	movw	$(gdt_end - gdt_rom - 1), gdtr_ram
	movl	$GDT, gdtr_ram + 2
	lgdt	gdtr_ram
	// TSS A: ring-0 stack; TSS B: the second task
	mov	$TSSA, %edi
	xor	%eax, %eax
	mov	$(TSSB - TSSA + 0x68) / 4, %ecx	// both TSSs
	rep stosl
	movl	$RSP0, TSSA + 4
	movl	$0x10, TSSA + 8
	movl	$BASE + task_b, TSSB + 0x20	// EIP
	movl	$2, TSSB + 0x24			// EFLAGS
	movl	$BSTACK, TSSB + 0x38		// ESP
	movl	$0x10, TSSB + 0x48		// ES
	movl	$0x08, TSSB + 0x4c		// CS
	movl	$0x10, TSSB + 0x50		// SS
	movl	$0x10, TSSB + 0x54		// DS
	movl	$0x10, TSSB + 0x58		// FS
	movl	$0x10, TSSB + 0x5c		// GS
	mov	$0x28, %ax
	ltr	%ax
	// to ring 3
	push	$0x20 | 3			// SS
	push	$USTACK				// ESP
	pushf
	push	$0x18 | 3			// CS
	push	$BASE + ring3			// EIP
	iret

ring3:
	mov	%cs, %ax
	and	$3, %ax
	cmp	$3, %ax
	jne	fail3
	push	$0x55				// the gate's parameter
	lcall	$0x38 | 3, $0
	cmp	$USTACK, %esp			// RET FAR 4 released the parameter
	jne	fail3
	cmp	$0x5a, %ebx
	jne	fail3
	lcall	$0x40 | 3, $0			// task gate -> task B
	cmp	$0x1234, %esi			// task A's registers are back
	jne	fail3
	mov	%cs, %ax
	and	$3, %ax
	cmp	$3, %ax
	jne	fail3
	mov	$'B', %al
	mov	$'K', %ah
	lcall	$0x38 | 3, $0			// print and power off from ring 0
fail3:
	jmp	fail3

// ring 0, on TSS A's stack: EIP, CS, the parameter, ESP, SS
gate_entry:
	cmpl	$0x18 | 3, 4(%esp)
	jne	fail
	cmpl	$0x20 | 3, 16(%esp)
	jne	fail
	cmp	$RSP0 - 20, %esp
	jne	fail
	cmp	$'B', %al
	je	done
	cmpl	$0x55, 8(%esp)
	jne	fail
	mov	$'C', %al
	call	putc
	mov	$'G', %al
	call	putc
	mov	$'\n', %al
	call	putc
	mov	$0x5a, %ebx
	mov	$0x1234, %esi
	lret	$4

done:
	call	putc
	mov	%ah, %al
	call	putc
	mov	$'\n', %al
	call	putc
	mov	$0x604, %dx
	mov	$0x2000, %ax
	out	%ax, %dx
	hlt

task_b:
	pushf
	pop	%eax
	bt	$14, %eax			// NT: called as a task
	jnc	fail
	movzwl	TSSB, %eax			// the link: task A
	cmp	$0x28, %eax
	jne	fail
	mov	$'T', %al
	call	putc
	mov	$'S', %al
	call	putc
	mov	$'\n', %al
	call	putc
	xor	%esi, %esi
	iret					// NT = 1: back to task A

putc:
	push	%edx
	mov	$0x3f8, %dx
	out	%al, %dx
	pop	%edx
	ret

fail:
	mov	$0x604, %dx
	mov	$0x00013333, %eax
	out	%eax, %dx
	hlt

	.balign	8
gdt_rom:
	.quad	0
	.quad	0x00cf9b000000ffff		// 0x08: code, DPL 0
	.quad	0x00cf93000000ffff		// 0x10: data, DPL 0
	.quad	0x00cffb000000ffff		// 0x18: code, DPL 3
	.quad	0x00cff3000000ffff		// 0x20: data, DPL 3
	.quad	0, 0				// 0x28, 0x30: TSS A, B
	.quad	0				// 0x38: call gate
	.quad	0				// 0x40: task gate
gdt_end:
gdtr_rom:
	.word	gdt_end - gdt_rom - 1
	.long	BASE + gdt_rom
	.set	gdtr_ram, 0x500

	.org	0xfff0
	.byte	0xe9
	.word	(start - 9f) & 0xffff
9:
	.org	0x10000
