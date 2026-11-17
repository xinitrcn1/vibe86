// v86: virtual-8086 mode and I/O permissions.
//   a 32-bit kernel IRETs to virtual-8086 mode (VM in the popped EFLAGS,
//   IOPL 0; ES, DS, FS, GS popped too); the v86 code
//     writes COM1 (allowed by the TSS's I/O permission bitmap)
//     CLI           -> #GP(0) (IOPL-sensitive)
//     OUT 80h       -> #GP(0) (the bitmap denies port 80h)
//     INT 21h       -> #GP(0) (INT n is IOPL-sensitive without VME)
//   each #GP reaches the ring-0 handler on the TSS stack with the v86 frame
//   (error code, EIP, CS, EFLAGS with VM, ESP, SS, ES, DS, FS, GS), which
//   returns to virtual-8086 mode with IRET.
// expect-out: V
// expect-out: GP
// expect-out: IO
// expect-out: OK
	.set	BASE, 0xffff0000
	.set	GDT, 0x1000
	.set	IDT, 0x2000
	.set	TSS, 0x30000
	.set	KSTACK, 0x80000
	.set	RSP0, 0x90000
	.set	V86CODE, 0x7000

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
	mov	$BASE + gdt_rom, %esi
	mov	$GDT, %edi
	mov	$(gdt_end - gdt_rom), %ecx
	rep movsb
	// TSS 0x28: base TSS, limit 0x68 + 8 KiB bitmap
	movl	$0x00002068 | ((TSS & 0xffff) << 16), GDT + 0x28
	movl	$0x00008900 | ((TSS >> 16) & 0xff), GDT + 0x2c
	movw	$(gdt_end - gdt_rom - 1), gdtr_ram
	movl	$GDT, gdtr_ram + 2
	lgdt	gdtr_ram
	// the TSS: ring-0 stack, I/O bitmap at +68h: all ports denied but COM1
	mov	$TSS, %edi
	xor	%eax, %eax
	mov	$0x68 / 4, %ecx
	rep stosl
	mov	$0xffffffff, %eax
	mov	$0x2000 / 4, %ecx
	rep stosl
	movl	$RSP0, TSS + 4
	movl	$0x10, TSS + 8
	movw	$0x68, TSS + 0x66
	movb	$0x00, TSS + 0x68 + 0x3f8 / 8		// ports 3F8h-3FFh
	mov	$0x28, %ax
	ltr	%ax
	// the IDT: #GP -> gp_handler (32-bit interrupt gate, DPL 0)
	mov	$IDT, %edi
	xor	%eax, %eax
	mov	$256 * 2, %ecx
	rep stosl
	mov	$BASE + gp_handler, %eax
	mov	%ax, IDT + 13 * 8
	movw	$0x08, IDT + 13 * 8 + 2
	movw	$0x8e00, IDT + 13 * 8 + 4
	shr	$16, %eax
	mov	%ax, IDT + 13 * 8 + 6
	movw	$256 * 8 - 1, idtr_ram
	movl	$IDT, idtr_ram + 2
	lidt	idtr_ram
	// the v86 code below 1 MiB
	mov	$BASE + v86_code, %esi
	mov	$V86CODE, %edi
	mov	$(v86_end - v86_code), %ecx
	rep movsb
	// IRET to virtual-8086 mode: GS FS DS ES SS ESP EFLAGS CS EIP
	push	$0
	push	$0
	push	$0
	push	$0
	push	$0x0600				// SS (v86 stack at 0x6000:FFFE... 0x6000)
	push	$0x0ffe				// ESP
	push	$0x00020002			// EFLAGS: VM, IOPL 0
	push	$V86CODE >> 4			// CS
	push	$0				// EIP
	iret

// #GP from virtual-8086 mode: error code, EIP, CS, EFLAGS, ESP, SS, ES, DS, FS, GS
gp_handler:
	mov	$0x10, %ax			// delivery from v86 nulls DS, ES, FS, GS
	mov	%ax, %ds
	mov	%ax, %es
	cmpl	$0, (%esp)
	jne	fail
	testl	$0x20000, 12(%esp)		// VM set in the saved EFLAGS
	jz	fail
	cmp	$RSP0 - 40, %esp
	jne	fail
	mov	8(%esp), %eax			// CS:IP -> the faulting opcode
	shl	$4, %eax
	add	4(%esp), %eax
	movzbl	(%eax), %eax
	cmp	$0xfa, %al			// CLI
	jne	1f
	mov	$'G', %al
	call	putc
	mov	$'P', %al
	call	putc
	mov	$'\n', %al
	call	putc
	incl	4(%esp)
	jmp	9f
1:	cmp	$0xe6, %al			// OUT imm8, AL
	jne	2f
	mov	$'I', %al
	call	putc
	mov	$'O', %al
	call	putc
	mov	$'\n', %al
	call	putc
	addl	$2, 4(%esp)
	jmp	9f
2:	cmp	$0xcd, %al			// INT n
	jne	fail
	mov	$'O', %al
	call	putc
	mov	$'K', %al
	call	putc
	mov	$'\n', %al
	call	putc
	mov	$0x604, %dx
	mov	$0x2000, %ax
	out	%ax, %dx
	hlt
9:	add	$4, %esp			// the error code
	iret

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

	.code16
v86_code:
	mov	$0x3f8, %dx
	mov	$'V', %al
	out	%al, %dx			// allowed: COM1 is in the bitmap
	mov	$'\n', %al
	out	%al, %dx
	cli					// #GP
	out	%al, $0x80			// #GP: the bitmap denies 80h
	int	$0x21				// #GP
1:	jmp	1b
v86_end:

	.balign	8
gdt_rom:
	.quad	0
	.quad	0x00cf9b000000ffff		// 0x08: code, DPL 0
	.quad	0x00cf93000000ffff		// 0x10: data, DPL 0
	.quad	0, 0				// 0x18, 0x20
	.quad	0				// 0x28: TSS
gdt_end:
gdtr_rom:
	.word	gdt_end - gdt_rom - 1
	.long	BASE + gdt_rom
	.set	gdtr_ram, 0x500
	.set	idtr_ram, 0x510

	.org	0xfff0
	.byte	0xe9
	.word	(start - 9f) & 0xffff
9:
	.org	0x10000
