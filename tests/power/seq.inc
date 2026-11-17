// The body of the power-sequencing tests: once the die runs, print SEQ OK
// and power off. Whether it gets to run is what each test checks.
#include "boot64.inc"

main64:
	lea	msg(%rip), %rsi
	call	puts
	jmp	pass
msg:	.asciz	"SEQ OK\n"

	BOOT64_END
