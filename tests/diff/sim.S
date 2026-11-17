// sim.S: the vsim half of the differential test (gen.py): boots to 64-bit
// mode (boot64.inc), runs the generated diff_body (DIFF_BODY, included) and
// writes the state after every step and the scratch buffer to the debug
// console (binary), which diff.py compares with host.c's output.
#include "boot64.inc"
	.set	BUF, 0x100000
	.set	OUT, 0x110000

main64:
	mov	$BUF, %rdi
	xor	%ecx, %ecx
1:	imul	$0x9d, %ecx, %eax
	add	$7, %eax
	mov	%al, (%rdi, %rcx)
	inc	%ecx
	cmp	$256, %ecx
	jne	1b
	mov	$BUF, %rdi
	mov	$OUT, %rsi
	call	diff_body
	// the states and the scratch buffer, in binary, to the debug console
	// (port 402h: vsim -g) in one string instruction
	mov	$OUT, %rsi
	mov	diff_steps(%rip), %rcx
	imul	$96, %rcx
	mov	$0x402, %dx
	cld
	rep outsb
	mov	$BUF, %rsi
	mov	$256, %ecx
	rep outsb
	lea	endmsg(%rip), %rsi
	call	puts
	jmp	pass

endmsg:	.asciz	"end\n"

#include DIFF_BODY
	BOOT64_END
