// real-hello: the first instructions after reset, in real-address mode:
// print "OK" on COM1 and power off through port 604h.
// expect-out: OK
	.code16
	.text
	.globl start
start:
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

	// the reset vector: FFFF:FFF0 is 16 bytes before the end of the image
	.org	0xfff0
	.byte	0xe9			// jmp rel16, wrapping within the 64K segment
	.word	(start - 2f) & 0xffff
2:
	.org	0x10000
