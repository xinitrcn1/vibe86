// string-seg: the default segment of string instructions and XLAT is DS
// (Intel SDM Vol. 1 3.7.5), whatever ModRM the previous instruction had.
// v1 once took it from the previous instruction's ModRM byte, so after
// XCHG CX, DX (87 CA: r/m 010, which as a 16-bit memory form is [BP+SI],
// default SS) a CMPSB read SS:SI: MikeOS's boot sector, which does exactly
// that, never found KERNEL.BIN. Segment overrides must still apply.
// expect-out: OK
	.code16
	.text
start:
	cli
	cld
	xor	%ax, %ax
	mov	%ax, %ss
	mov	$0x7000, %sp
	// DS = 0700h, ES = 0900h hold 'D'; SS = 0800h holds 'S' (offset 100h)
	mov	$0x0700, %ax
	mov	%ax, %ds
	movb	$'D', 0x100
	movb	$'X', 0x180			// XLAT table entry: DS:[BX + AL]
	mov	$0x0900, %ax
	mov	%ax, %es
	movb	$'D', %es:0x100
	mov	$0x0800, %ax
	mov	%ax, %ss
	movb	$'S', %ss:0x100
	movb	$'Y', %ss:0x180
	mov	$0x7000, %sp

	// LODSB after XCHG CX, DX (ModRM CAh)
	mov	$0x100, %si
	.byte	0x87, 0xca
	lodsb
	cmp	$'D', %al
	jne	fail
	// CMPSB after an instruction with a [BP+DI] ModRM (mod 11 or memory)
	mov	$0x100, %si
	mov	$0x100, %di
	mov	$0x100, %bp
	mov	(%bp,%di), %cl			// a real SS:[BP+DI] access (ModRM 03h)
	cmpsb
	jne	fail
	// XLAT after XCHG CX, DX
	mov	$0x100, %bx
	mov	$0x80, %al
	.byte	0x87, 0xca
	xlat
	cmp	$'X', %al
	jne	fail
	// an override still wins: SS: LODSB
	mov	$0x100, %si
	.byte	0x36
	lodsb
	cmp	$'S', %al
	jne	fail

	mov	$0x3f8, %dx
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
