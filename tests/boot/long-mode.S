// long-mode: the full x86 boot path, from reset.
//   real mode -> LGDT, CR0.PE, far JMP -> 32-bit protected mode
//   -> PAE page tables (1-GByte pages), EFER.LME, CR0.PG, far JMP -> 64-bit mode
//   -> a 64-bit IDT: INT3 (#BP, a trap) and DIV by zero (#DE, a fault)
//   -> LTR, IRETQ to ring 3, INT 80h back to ring 0 through a DPL-3 gate
//      (the stack switches to TSS.RSP0)
// expect-out: PM
// expect-out: LM
// expect-out: BP
// expect-out: DE
// expect-out: R3
	.set	BASE, 0xffff0000		// the image's linear address
	.set	SRAM_GDT, 0x1000
	.set	PML4, 0x10000
	.set	PDPT, 0x11000
	.set	IDT, 0x20000
	.set	TSS, 0x30000
	.set	KSTACK, 0x80000
	.set	RSP0, 0x90000
	.set	USTACK, 0xa0000
	.set	gdtr64, 0x500			// pseudo-descriptors in SRAM (the image is ROM)
	.set	idtr64, 0x510

	.code16
	.text
start:
	cli
	lgdtl	%cs:gdtr
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
	mov	$'P', %al
	call	putc32
	mov	$'M', %al
	call	putc32
	mov	$'\n', %al
	call	putc32
	// identity-map the first 4 GiB with 1-GiB pages, user-accessible
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
	or	$0x20, %eax			// PAE
	mov	%eax, %cr4
	mov	$PML4, %eax
	mov	%eax, %cr3
	mov	$0xc0000080, %ecx		// IA32_EFER.LME
	rdmsr
	or	$0x100, %eax
	wrmsr
	mov	%cr0, %eax
	or	$0x80000000, %eax		// PG: IA-32e mode becomes active
	mov	%eax, %cr0
	ljmp	$0x18, $BASE + lm64

putc32:
	mov	$0x3f8, %dx
	out	%al, %dx
	ret

	.code64
lm64:
	mov	$0x10, %ax
	mov	%ax, %ss
	mov	$KSTACK, %rsp
	lea	msg_lm(%rip), %rsi
	call	puts
	// the GDT has to be writable (LTR marks the TSS busy): copy it to SRAM
	lea	gdt(%rip), %rsi
	mov	$SRAM_GDT, %rdi
	mov	$(gdt_end - gdt), %ecx
	rep movsb
	movw	$(gdt_end - gdt - 1), gdtr64
	movq	$SRAM_GDT, gdtr64 + 2
	lgdt	gdtr64
	// the TSS's descriptor (0x30): base TSS, limit 0x67, available 64-bit TSS
	movabs	$0x0000890000000067 | (TSS << 16), %rax
	mov	%rax, SRAM_GDT + 0x30
	movq	$0, SRAM_GDT + 0x38
	movq	$RSP0, TSS + 4			// TSS.RSP0
	mov	$0x30, %ax
	ltr	%ax
	// the IDT
	mov	$IDT, %rdi
	xor	%eax, %eax
	mov	$256 * 16 / 8, %ecx
	rep stosq
	lea	h_de(%rip), %rax
	mov	$0, %ecx
	mov	$0x8e, %edx			// interrupt gate, DPL 0
	call	set_gate
	lea	h_bp(%rip), %rax
	mov	$3, %ecx
	mov	$0xee, %edx			// DPL 3
	call	set_gate
	lea	h_80(%rip), %rax
	mov	$0x80, %ecx
	mov	$0xee, %edx
	call	set_gate
	movw	$256 * 16 - 1, idtr64
	movq	$IDT, idtr64 + 2
	lidt	idtr64
	int3
	xor	%ecx, %ecx
	mov	$1, %eax
	xor	%edx, %edx
	div	%ecx				// #DE: the handler skips the 2-byte DIV
	// to ring 3
	push	$0x20 | 3			// SS
	push	$USTACK				// RSP
	pushfq
	lea	ring3(%rip), %rax
	push	$0x28 | 3			// CS
	push	%rax				// RIP
	iretq

ring3:
	int	$0x80
1:	jmp	1b

// set_gate: vector %ecx -> handler %rax, type/attributes %edx
set_gate:
	shl	$4, %rcx
	add	$IDT, %rcx
	mov	%ax, (%rcx)
	movw	$0x18, 2(%rcx)
	mov	%dl, 5(%rcx)
	shr	$16, %rax
	mov	%ax, 6(%rcx)
	shr	$16, %rax
	mov	%eax, 8(%rcx)
	ret

puts:
	mov	$0x3f8, %dx
1:	lodsb
	test	%al, %al
	jz	2f
	out	%al, %dx
	jmp	1b
2:	ret

h_bp:
	lea	msg_bp(%rip), %rsi
	call	puts
	iretq

h_de:
	lea	msg_de(%rip), %rsi
	call	puts
	addq	$2, (%rsp)			// skip the DIV
	iretq

h_80:
	// from ring 3: CPL 0 again, on TSS.RSP0 with SS, RSP, RFLAGS, CS, RIP pushed
	cmp	$RSP0 - 5 * 8, %rsp
	jne	fail
	cmpq	$0x2b, 8(%rsp)			// the interrupted CS: ring 3
	jne	fail
	lea	msg_r3(%rip), %rsi
	call	puts
	mov	$0x604, %dx
	mov	$0x2000, %ax
	out	%ax, %dx
1:	hlt
	jmp	1b

fail:
	lea	msg_fail(%rip), %rsi
	call	puts
	mov	$0x604, %dx
	mov	$0x00013333, %eax		// fail, code 1
	out	%eax, %dx
	hlt

msg_lm:	.asciz	"LM\n"
msg_bp:	.asciz	"BP\n"
msg_de:	.asciz	"DE\n"
msg_r3:	.asciz	"R3\n"
msg_fail: .asciz "FAIL\n"

	.balign	8
gdt:
	.quad	0
	.quad	0x00cf9b000000ffff		// 0x08: 32-bit code
	.quad	0x00cf93000000ffff		// 0x10: data
	.quad	0x00af9b000000ffff		// 0x18: 64-bit code
	.quad	0x00cff3000000ffff		// 0x20: user data (DPL 3)
	.quad	0x00affb000000ffff		// 0x28: user 64-bit code (DPL 3)
	.quad	0, 0				// 0x30: the TSS (filled in)
gdt_end:
gdtr:
	.word	gdt_end - gdt - 1
	.long	BASE + gdt

	.org	0xfff0
	.byte	0xe9				// jmp rel16, within the 64K segment
	.word	(start - 2f) & 0xffff
2:
	.org	0x10000
