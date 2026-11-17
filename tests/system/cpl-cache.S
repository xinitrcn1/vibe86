// cpl-cache: the cache-control instructions SeaBIOS needed (WBINVD was
// missing) at CPL 0, and their privilege checks at CPL 3: WBINVD, INVD,
// WBNOINVD #GP(0); CLFLUSH, CLFLUSHOPT, CLWB allowed (they need only a
// readable address).
// expect-out: CPL OK
#include "boot64.inc"
#include "expect.inc"

main64:
	call	expect_init
	lea	h_back(%rip), %rax
	mov	$0x80, %ecx
	mov	$0xee, %edx			// DPL 3: ring 3 returns through it
	call	set_gate
	wbinvd
	invd
	wbnoinvd
	clflush	0x40000
	push	$0x20 | 3
	push	$USTACK
	pushfq
	lea	ring3(%rip), %rax
	push	$0x28 | 3
	push	%rax
	iretq

ring3:
	CATCH	1f
	wbinvd
1:	EXPECT_VEC 13, 0x01
	EXPECT	LASTERR, 0, 0x02
	CATCH	1f
	invd
1:	EXPECT_VEC 13, 0x03
	CATCH	1f
	wbnoinvd
1:	EXPECT_VEC 13, 0x04
	EXPECT	NEXC, 3, 0x05
	clflush	USTACK - 8
	clflushopt USTACK - 8
	clwb	USTACK - 8
	EXPECT	NEXC, 3, 0x06			// no exception for those
	int	$0x80

h_back:
	mov	$KSTACK, %rsp
	lea	endmsg(%rip), %rsi
	call	puts
	jmp	pass

endmsg:	.asciz	"CPL OK\n"
	BOOT64_END
