// string-io: INS/OUTS with REP (SeaBIOS needed REP INSB; it was missing):
// reading fw_cfg's signature ("QEMU", data port 511h), forwards (DF = 0)
// and backwards (DF = 1), a count of 0 (no port access: the next byte read
// is still the first), the 67h address size (ECX/EDI: RCX's upper half is
// not part of the count; the 32-bit writes zero-extend), and REP OUTSB both
// ways to COM1.
// expect-out: IO-fwd
// expect-out: dwb-OI
// expect-out: IO OK
#include "boot64.inc"
#include "expect.inc"
	.set	BUF, 0x40000

	.macro	SELECT key
	mov	$0x510, %dx
	mov	$\key, %ax
	out	%ax, %dx
	mov	$0x511, %dx
	.endm

main64:
	call	expect_init
	cld
	// forwards
	movq	$0, BUF
	SELECT	0
	mov	$BUF, %edi
	mov	$4, %ecx
	rep insb
	EXPECT	BUF, 0x554d4551, 0x01		// "QEMU"
	EXPECT	%rcx, 0, 0x02
	EXPECT	%rdi, BUF + 4, 0x03
	// backwards
	movq	$0, BUF
	SELECT	0
	mov	$BUF + 3, %edi
	mov	$4, %ecx
	std
	rep insb
	cld
	EXPECT	BUF, 0x51454d55, 0x04		// "UMEQ"
	EXPECT	%rdi, BUF - 1, 0x05
	// a count of 0 reads nothing
	movq	$0, BUF
	SELECT	0
	mov	$BUF, %edi
	xor	%ecx, %ecx
	rep insb
	EXPECT	BUF, 0, 0x06
	insb					// the first byte still: 'Q'
	EXPECT	BUF, 0x51, 0x07
	// 67h: ECX and EDI; RCX's upper half isn't part of the count
	movq	$0, BUF
	SELECT	0
	movabsq	$0x0000000100000004, %rcx
	movabsq	$0x1234567800000000 + BUF, %rdi
	.byte	0x67, 0xf3, 0x6c		// rep insb (address size 32)
	EXPECT	BUF, 0x554d4551, 0x08
	EXPECT	%rcx, 0, 0x09			// ECX written: zero-extended
	EXPECT	%rdi, BUF + 4, 0x0a		// EDI written: zero-extended
	// OUTSB forwards and backwards
	lea	fwd(%rip), %rsi
	mov	$0x3f8, %dx
	mov	$7, %ecx
	rep outsb
	lea	bwd_end - 1(%rip), %rsi
	mov	$6, %ecx
	std
	rep outsb
	cld
	mov	$'\n', %al
	call	putc
	lea	endmsg(%rip), %rsi
	call	puts
	jmp	pass

fwd:	.ascii	"IO-fwd\n"
bwd:	.ascii	"IO-bwd"
bwd_end:
endmsg:	.asciz	"IO OK\n"
	BOOT64_END
