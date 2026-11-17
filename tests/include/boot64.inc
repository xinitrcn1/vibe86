// boot64.inc: reset -> protected mode -> IA-32e mode, then `main64` (in the
// including file) with:
//   a flat GDT copied to SRAM (0x08 32-bit code, 0x10 data, 0x18 64-bit code,
//   0x20 user data, 0x28 user 64-bit code, 0x30 TSS loaded with RSP0 = RSP0),
//   4 GiB identity-mapped with user-accessible 1-GiB pages,
//   an empty 64-bit IDT at IDT (set_gate fills it), RSP = KSTACK.
// Helpers: puts (RSI: string), putc (AL), set_gate (ECX vector, RAX handler,
// EDX type/attributes), pass (power off), fail (fail code 1).
// The image is 64 KiB at FFFF0000h; include this file first and end the
// including file with BOOT64_END.
	.set	BASE, 0xffff0000
	.set	SRAM_GDT, 0x1000
	.set	GDTR64, 0x500
	.set	IDTR64, 0x510
	.set	PML4, 0x10000
	.set	PDPT, 0x11000
	.set	IDT, 0x20000
	.set	TSS, 0x30000
	.set	KSTACK, 0x80000
	.set	RSP0, 0x90000
	.set	USTACK, 0xa0000

	.code16
	.text
start:
	cli
	lgdtl	%cs:gdtr16
	mov	%cr0, %eax
	or	$1, %eax
	mov	%eax, %cr0
	ljmpl	$0x08, $BASE + pm32_entry

	.code32
pm32_entry:
	mov	$0x10, %ax
	mov	%ax, %ds
	mov	%ax, %es
	mov	%ax, %ss
	mov	$KSTACK, %esp
	mov	$PML4, %edi
	xor	%eax, %eax
	mov	$0x2000 / 4, %ecx
	rep stosl
	movl	$PDPT | 7, PML4
	movl	$0x00000087, PDPT + 0
	movl	$0x40000087, PDPT + 8
	movl	$0x80000087, PDPT + 16
	movl	$0xc0000087, PDPT + 24
	mov	%cr4, %eax
	or	$0x20, %eax
	mov	%eax, %cr4
	mov	$PML4, %eax
	mov	%eax, %cr3
	mov	$0xc0000080, %ecx
	rdmsr
	or	$0x100, %eax
	wrmsr
	mov	%cr0, %eax
	or	$0x80000000, %eax
	mov	%eax, %cr0
	ljmp	$0x18, $BASE + lm64_entry

	.code64
lm64_entry:
	mov	$0x10, %ax
	mov	%ax, %ss
	mov	%ax, %ds
	mov	%ax, %es
	mov	$KSTACK, %rsp
	lea	gdt64(%rip), %rsi
	mov	$SRAM_GDT, %rdi
	mov	$(gdt64_end - gdt64), %ecx
	rep movsb
	movw	$(gdt64_end - gdt64 - 1), GDTR64
	movq	$SRAM_GDT, GDTR64 + 2
	lgdt	GDTR64
	movabs	$0x0000890000000067 | (TSS << 16), %rax
	mov	%rax, SRAM_GDT + 0x30
	movq	$0, SRAM_GDT + 0x38
	movq	$RSP0, TSS + 4
	mov	$0x30, %ax
	ltr	%ax
	mov	$IDT, %rdi
	xor	%eax, %eax
	mov	$256 * 16 / 8, %ecx
	rep stosq
	movw	$256 * 16 - 1, IDTR64
	movq	$IDT, IDTR64 + 2
	lidt	IDTR64
	jmp	main64

set_gate:
	push	%rcx
	shl	$4, %rcx
	add	$IDT, %rcx
	mov	%ax, (%rcx)
	movw	$0x18, 2(%rcx)
	movb	$0, 4(%rcx)
	mov	%dl, 5(%rcx)
	shr	$16, %rax
	mov	%ax, 6(%rcx)
	shr	$16, %rax
	mov	%eax, 8(%rcx)
	movl	$0, 12(%rcx)
	pop	%rcx
	ret

puts:
	push	%rdx
	mov	$0x3f8, %dx
1:	lodsb
	test	%al, %al
	jz	2f
	out	%al, %dx
	jmp	1b
2:	pop	%rdx
	ret

putc:
	push	%rdx
	mov	$0x3f8, %dx
	out	%al, %dx
	pop	%rdx
	ret

pass:
	mov	$0x604, %dx
	mov	$0x2000, %ax
	out	%ax, %dx
1:	hlt
	jmp	1b

fail:
	mov	$'F', %al
	call	putc
	mov	$'\n', %al
	call	putc
	mov	$0x604, %dx
	mov	$0x00013333, %eax
	out	%eax, %dx
1:	hlt
	jmp	1b

	.balign	8
gdt64:
	.quad	0
	.quad	0x00cf9b000000ffff		// 0x08: 32-bit code
	.quad	0x00cf93000000ffff		// 0x10: data
	.quad	0x00af9b000000ffff		// 0x18: 64-bit code
	.quad	0x00cff3000000ffff		// 0x20: user data (DPL 3)
	.quad	0x00affb000000ffff		// 0x28: user 64-bit code (DPL 3)
	.quad	0, 0				// 0x30: TSS
gdt64_end:
gdtr16:
	.word	gdt64_end - gdt64 - 1
	.long	BASE + gdt64

	.macro	BOOT64_END
	.org	0xfff0
	.byte	0xe9
	.word	(start - 9f) & 0xffff
9:
	.org	0x10000
	.endm
