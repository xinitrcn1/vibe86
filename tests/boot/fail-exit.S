// fail-exit: a test can report failure: a dword write of 3333h | code << 16
// to the power-control port (604h) stops vsim with exit status 1.
// expect-exit: 1
	.code16
	.text
start:
	mov	$0x604, %dx
	mov	$0x00073333, %eax
	out	%eax, %dx
1:	hlt
	jmp	1b

	.org	0xfff0
	.byte	0xe9
	.word	(start - 2f) & 0xffff
2:
	.org	0x10000
