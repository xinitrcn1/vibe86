// real-int: real-mode interrupt delivery through the IVT, to a handler in a
// high segment (the new CS base is the full selector * 16: F000h -> F0000h),
// with FLAGS, CS and IP pushed and IRET back; INT n and an external
// interrupt (the 8254's IRQ 0 through the 8259, vector 08h).
// expect-out: I1
// expect-out: T0
	.code16
	.text
start:
	cli
	xor	%ax, %ax
	mov	%ax, %ds
	mov	%ax, %ss
	mov	$0x7000, %sp
	// IVT[60h] and IVT[08h] -> F000h:handler: after reset CS is F000h with
	// base FFFF0000h, but delivery loads CS real-mode style (base F0000h),
	// where the board's PAM alias shows the flash's last 128 KiB
	movw	$h60, 0x60 * 4
	movw	%cs, 0x60 * 4 + 2
	movw	$h08, 0x08 * 4
	movw	%cs, 0x08 * 4 + 2
	int	$0x60
	cmp	$0x1234, %bx
	jne	fail
	// the PIC: ICW1-4, vectors 08h-0Fh, IRQ 0 unmasked
	mov	$0x11, %al
	out	%al, $0x20
	mov	$0x08, %al
	out	%al, $0x21
	mov	$0x04, %al
	out	%al, $0x21
	mov	$0x01, %al
	out	%al, $0x21
	mov	$0xfe, %al
	out	%al, $0x21
	// the PIT: channel 0, mode 2, a short period
	mov	$0x34, %al
	out	%al, $0x43
	mov	$100, %al
	out	%al, $0x40
	xor	%al, %al
	out	%al, $0x40
	xor	%cx, %cx
	sti
1:	hlt
	cmp	$0, %cx
	je	1b
	cli
	mov	$0x604, %dx
	mov	$0x2000, %ax
	out	%ax, %dx
	hlt

h60:
	mov	$0x1234, %bx
	mov	$'I', %al
	call	putc
	mov	$'1', %al
	call	putc
	mov	$'\n', %al
	call	putc
	iret

h08:
	inc	%cx
	mov	$'T', %al
	call	putc
	mov	$'0', %al
	call	putc
	mov	$'\n', %al
	call	putc
	mov	$0x20, %al			// EOI
	out	%al, $0x20
	iret

putc:
	push	%dx
	mov	$0x3f8, %dx
	out	%al, %dx
	pop	%dx
	ret

fail:
	mov	$0x604, %dx
	mov	$0x00013333, %eax
	out	%eax, %dx
	hlt

	.org	0xfff0
	.byte	0xe9
	.word	(start - 9f) & 0xffff
9:
	.org	0x10000
