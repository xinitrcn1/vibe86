// smm: system management mode on the PC platform (AMD64 APM Vol. 2 ch. 10;
// i440FX SMRAM register 72h; PIIX4 APM ports B2h/B3h and DEVACTB.APMC_EN).
//   RSM outside SMM is #UD
//   an APMC write (B2h) raises SMI; the first handler runs at the reset
//   SMBASE 3_0000h + 8000h and relocates SMBASE to A_0000h (the save area's
//   SMBASE field), and says so in APMS (B3h)
//   SMRAM (A0000h-BFFFFh) is RAM while D_OPEN, and invisible (all ones)
//   outside SMM once D_OPEN is clear; the second handler runs from it
//   the handler changes the saved RAX: RSM restores the changed value
//   an SMI from 32-bit protected mode returns to it (CR0, CS, the flat
//   segments come back from the save area)
// expect-out: UD
// expect-out: RELOC
// expect-out: CLOSED
// expect-out: RAX
// expect-out: PM32
// expect-out: OK
	.code16
	.equ	counter, 0x7000
	.equ	udflag, 0x7004
	.text
	.globl start
start:
	cli
	xor	%ax, %ax
	mov	%ax, %ds
	mov	%ax, %ss
	mov	$0x6000, %sp

	// RSM outside SMM: #UD (IVT vector 6 -> ud_handler, which skips it)
	movw	$ud_handler, 6 * 4
	movw	%cs, 6 * 4 + 2
	movb	$0, udflag
	rsm
	mov	$1, %di
	cmpb	$1, udflag
	jne	fail
	mov	$'U', %al
	call	putc
	mov	$'D', %al
	call	putc
	call	newline

	// PIIX4 PM (00:01.3) DEVACTB, 58h: APMC_EN (bit 25)
	mov	$0x80000b58, %eax
	mov	$0xcf8, %dx
	out	%eax, %dx
	mov	$0xcfc, %dx
	in	%dx, %eax
	or	$1 << 25, %eax
	out	%eax, %dx
	// i440FX (00:00.0) SMRAM, 72h: G_SMRAME | D_OPEN, C_BASE_SEG A0000h
	mov	$0x80000070, %eax
	mov	$0xcf8, %dx
	out	%eax, %dx
	mov	$0xcfe, %dx
	mov	$0x4a, %al
	out	%al, %dx

	// the handlers: h1 at 3_0000h + 8000h, h2 at A_0000h + 8000h (SMRAM, open)
	push	%cs
	pop	%ds
	mov	$0x3800, %ax
	mov	%ax, %es
	xor	%di, %di
	mov	$h1, %si
	mov	$h1_end - h1, %cx
	cld
	rep movsb
	mov	$0xa800, %ax
	mov	%ax, %es
	xor	%di, %di
	mov	$h2, %si
	mov	$h2_end - h2, %cx
	rep movsb
	xor	%ax, %ax
	mov	%ax, %ds
	movl	$0, counter

	// SMI 1: relocation
	xor	%al, %al
	out	%al, $0xb3
	out	%al, $0xb2
	nop
	in	$0xb3, %al
	mov	$2, %di
	cmp	$0x5a, %al
	jne	fail
	mov	$reloc_msg, %si
	call	puts

	// close SMRAM (G_SMRAME only): outside SMM A_0000h reads all ones
	mov	$0x80000070, %eax
	mov	$0xcf8, %dx
	out	%eax, %dx
	mov	$0xcfe, %dx
	mov	$0x0a, %al
	out	%al, %dx
	mov	$0xa000, %ax
	mov	%ax, %es
	mov	$3, %di
	cmpb	$0xff, %es:0x8000
	jne	fail
	mov	$closed_msg, %si
	call	puts

	// SMI 2 from real mode: the handler (in SMRAM) sets the saved RAX
	mov	$0x1111, %eax
	out	%al, $0xb2
	nop
	mov	$4, %di
	cmp	$0x12345678, %eax
	jne	fail
	mov	$5, %di
	cmpl	$1, counter
	jne	fail
	mov	$rax_msg, %si
	call	puts

	// 32-bit protected mode, flat
	lgdtl	%cs:gdtr
	mov	%cr0, %eax
	or	$1, %eax
	mov	%eax, %cr0
	ljmpl	$8, $0xffff0000 + pm32

fail:	mov	$'X', %al
	call	putc
	mov	%di, %ax
	add	$'0', %al
	call	putc
	call	newline
	mov	$0x604, %dx
	mov	$0x3333, %eax
	or	$3 << 16, %eax
	out	%eax, %dx
1:	hlt
	jmp	1b

ud_handler:
	push	%bp
	mov	%sp, %bp
	addw	$2, 2(%bp)		// skip RSM (0F AA)
	movb	$1, udflag
	pop	%bp
	iret

newline:
	mov	$'\n', %al
putc:	push	%dx
	mov	$0x3f8, %dx
	out	%al, %dx
	pop	%dx
	ret
puts:	push	%ds
	push	%cs
	pop	%ds
1:	lodsb
	test	%al, %al
	jz	2f
	call	putc
	jmp	1b
2:	pop	%ds
	ret

reloc_msg:	.asciz	"RELOC\n"
closed_msg:	.asciz	"CLOSED\n"
rax_msg:	.asciz	"RAX\n"

	// SMM handlers: 16-bit code, CS base = SMBASE, DS base 0, limits 4 GiB
	// (67h: 32-bit addresses in 16-bit code)
h1:	.byte	0x67, 0x66, 0xc7, 0x05		// movl $0xa0000, (0x3ff00): the save
	.long	0x3ff00, 0xa0000		// area's SMBASE field (3_0000h + FF00h)
	mov	$0x5a, %al
	out	%al, $0xb3
	rsm
h1_end:
h2:	.byte	0x67, 0x66, 0xff, 0x05		// incl (0x7000): the counter
	.long	0x7000
	.byte	0x67, 0x66, 0xc7, 0x05		// movl $0x12345678, (0xafff8): the
	.long	0xafff8, 0x12345678		// saved RAX (A_0000h + FFF8h)
	rsm
h2_end:

	.p2align 3
gdt:	.quad	0
	.quad	0x00cf9b000000ffff		// 08: code, 32-bit, flat
	.quad	0x00cf93000000ffff		// 10: data, flat
gdtr:	.word	23
	.long	0xffff0000 + gdt

	.code32
pm32:	mov	$0x10, %ax
	mov	%ax, %ds
	mov	%ax, %es
	mov	%ax, %ss
	mov	$0x6000, %esp
	// SMI 3 from protected mode: back in protected mode, same CS
	mov	$0x2222, %eax
	out	%al, $0xb2
	nop
	cmp	$0x12345678, %eax
	jne	pfail
	cmpl	$2, counter
	jne	pfail
	mov	%cs, %bx
	cmp	$8, %bx
	jne	pfail
	mov	%cr0, %ebx
	test	$1, %ebx
	jz	pfail
	mov	$0x3f8, %dx
	mov	$'P', %al
	out	%al, %dx
	mov	$'M', %al
	out	%al, %dx
	mov	$'3', %al
	out	%al, %dx
	mov	$'2', %al
	out	%al, %dx
	mov	$'\n', %al
	out	%al, %dx
	mov	$'O', %al
	out	%al, %dx
	mov	$'K', %al
	out	%al, %dx
	mov	$'\n', %al
	out	%al, %dx
	mov	$0x604, %dx
	mov	$0x2000, %ax
	out	%ax, %dx
1:	hlt
	jmp	1b
pfail:	mov	$0x3f8, %dx
	mov	$'X', %al
	out	%al, %dx
	mov	$'P', %al
	out	%al, %dx
	mov	$'\n', %al
	out	%al, %dx
	mov	$0x604, %dx
	mov	$0x33333, %eax
	out	%eax, %dx
1:	hlt
	jmp	1b

	.code16
	.org	0xfff0
	.byte	0xe9
	.word	(start - 2f) & 0xffff
2:
	.org	0x10000
