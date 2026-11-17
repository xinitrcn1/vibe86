// icache: the instruction cache (D-018) stays coherent, as x86 requires
// (Intel SDM Vol. 3A 11.6 "Self-Modifying Code"). A routine in DRAM,
// "mov $n, %eax; ret", is called (so it is cached), then its immediate is
// rewritten and the routine called again, each time expecting the new n:
//   1. an ordinary store through the same linear address;
//   2. a store through another linear address of the same physical page
//      (PDPT[4] maps 4 GiB onto physical 0): the cache is physical;
//   3. a call through that alias after a store through the first mapping;
//   4. device DMA: the DMA test endpoint (slot 0, docs/platform.md) copies
//      a new routine over it, through the fabric's inbound write snoop;
//   5. WBINVD, then the same routine again.
// expect-out: ICACHE OK
#include "boot64.inc"
#include "expect.inc"

	.set	CODE, 0x50000
	.set	SRC, 0x60000
	.set	ALIAS, 0x100000000
	.set	RP_CFG, 0xE0080000		// 00:10.0, slot 0's root port (ECAM)
	.set	EP_CFG, 0xE0100000		// 01:00.0, the DMA test endpoint
	.set	BAR0, 0xC0000000

main64:
	call	expect_init
	// mov $1, %eax; ret
	movl	$0x000001b8, CODE
	movw	$0xc300, CODE + 4
	mov	$CODE, %rbx
	call	*%rbx
	EXPECT	%rax, 1, 0x01
	call	*%rbx				// now from the cache
	EXPECT	%rax, 1, 0x02

	// 1. a store through the same address
	movb	$2, CODE + 1
	call	*%rbx
	EXPECT	%rax, 2, 0x03

	// 2. a store through an alias of the physical page
	movq	$0x00000087, PDPT + 4 * 8	// 4 GiB -> physical 0, 1-GiB page
	mov	%cr3, %rax
	mov	%rax, %cr3
	movabs	$ALIAS + CODE + 1, %rcx
	movb	$3, (%rcx)
	call	*%rbx
	EXPECT	%rax, 3, 0x04

	// 3. the alias executes what the first mapping wrote
	movabs	$ALIAS + CODE, %rdx
	call	*%rdx				// caches nothing new: same physical line
	EXPECT	%rax, 3, 0x05
	movb	$4, CODE + 1
	call	*%rdx
	EXPECT	%rax, 4, 0x06

	// 4. device DMA: SRC holds "mov $5, %eax; ret"; the endpoint copies it
	movl	$0x000005b8, SRC
	movw	$0xc300, SRC + 4
	mov	$RP_CFG, %edi			// (32-bit displacements above 2 GiB sign-extend)
	movl	$0x00010100, 0x18(%rdi)		// buses: primary 0, secondary 1, subordinate 1
	movl	$0xC000C000, 0x20(%rdi)		// memory window C000_0000h-C00F_FFFFh
	movw	$0x0006, 0x04(%rdi)		// memory space, bus master
	mov	$EP_CFG, %edi
	movl	$BAR0, 0x10(%rdi)		// BAR0 (64-bit)
	movl	$0, 0x14(%rdi)
	movw	$0x0006, 0x04(%rdi)
	mov	$BAR0, %esi
	movq	$SRC, 0x08(%rsi)
	movq	$CODE, 0x10(%rsi)
	movq	$6, 0x18(%rsi)
	movq	$1, 0x28(%rsi)			// start
	mov	$100000, %ecx
1:	cmpq	$2, 0x30(%rsi)			// STATUS: done
	je	2f
	loop	1b
2:	EXPECT	0x30(%rsi), 2, 0x07
	call	*%rbx
	EXPECT	%rax, 5, 0x08

	// 5. WBINVD flushes; the routine still runs
	wbinvd
	call	*%rbx
	EXPECT	%rax, 5, 0x09

	lea	msg(%rip), %rsi
	call	puts
	jmp	pass

msg:	.asciz	"ICACHE OK\n"

	BOOT64_END
