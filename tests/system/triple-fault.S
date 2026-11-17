// triple-fault: a fault while delivering #DF shuts the processor down
// (Intel SDM Vol. 3A 7.15, "Interrupt 8": "the processor enters shutdown
// mode"); vsim exits with status 2. #GP's and #DF's gates aren't present.
// expect-exit: 2
#include "boot64.inc"

main64:
	andb	$0x7f, IDT + 13 * 16 + 5
	andb	$0x7f, IDT + 8 * 16 + 5
	mov	$0x1238, %ax
	mov	%ax, %ds			// #GP -> #NP -> #DF -> #NP: shutdown
	jmp	pass				// never reached

	BOOT64_END
