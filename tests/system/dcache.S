// dcache: the data cache (D-018) is invisible to software and coherent
// with device DMA: write-through, updated on a write hit, filled on a read
// miss, invalidated by device writes through the fabric's snoop.
//   1. loads after stores: the same line, other offsets, other sizes;
//   2. a store hit then a load through another linear alias of the page;
//   3. conflicting lines (256 KiB apart: the same index) evict each other;
//   4. a misaligned load and store across a line boundary;
//   5. device DMA reads what the core wrote (write-through) ...
//   6. ... and the core reads what the device wrote (the snoop);
//   7. WBINVD, then the data are still there;
//   8. CPUID Fn8000_001D reports the two caches.
// expect-out: DCACHE OK
#include "boot64.inc"
#include "expect.inc"

	.set	BUF, 0x50000
	.set	FAR, BUF + 0x40000		// the same cache index
	.set	SRC, 0x60000
	.set	DST, 0x61000
	.set	ALIAS, 0x100000000
	.set	RP_CFG, 0xE0080000		// 00:10.0, slot 0's root port (ECAM)
	.set	EP_CFG, 0xE0100000		// 01:00.0, the DMA test endpoint
	.set	BAR0, 0xC0000000

main64:
	call	expect_init
	// 1. fill, hit, update
	movabs	$0x1122334455667788, %rax
	mov	%rax, BUF
	mov	BUF, %rbx			// a miss: the line is filled
	EXPECT	%rbx, 0x1122334455667788, 0x01
	mov	BUF, %rbx			// a hit
	EXPECT	%rbx, 0x1122334455667788, 0x02
	movb	$0xAA, BUF + 1			// a store hit updates the line
	movzwl	BUF, %ebx
	EXPECT	%rbx, 0xAA88, 0x03
	movl	$0xCAFEF00D, BUF + 60
	movl	BUF + 60, %ebx
	EXPECT	%rbx, 0xCAFEF00D, 0x04

	// 2. an alias of the physical page sees the update
	movq	$0x00000087, PDPT + 4 * 8	// 4 GiB -> physical 0, 1-GiB page
	mov	%cr3, %rax
	mov	%rax, %cr3
	movabs	$ALIAS + BUF, %rcx
	movw	$0x5A5A, 2(%rcx)
	mov	BUF, %rbx
	EXPECT	%rbx, 0x112233445A5AAA88, 0x05

	// 3. a conflicting line, then back
	movq	$0x77, FAR
	mov	FAR, %rbx
	EXPECT	%rbx, 0x77, 0x06
	mov	BUF, %rbx
	EXPECT	%rbx, 0x112233445A5AAA88, 0x07

	// 4. across a line boundary (bytes 60-67 of BUF + 0)
	movabs	$0x0102030405060708, %rax
	mov	%rax, BUF + 60
	mov	BUF + 60, %rbx
	EXPECT	%rbx, 0x0102030405060708, 0x08
	mov	BUF + 64, %ebx
	EXPECT	%rbx, 0x01020304, 0x09

	// 5./6. the DMA test endpoint copies SRC -> DST
	mov	$RP_CFG, %edi			// (32-bit displacements above 2 GiB sign-extend)
	movl	$0x00010100, 0x18(%rdi)
	movl	$0xC000C000, 0x20(%rdi)
	movw	$0x0006, 0x04(%rdi)
	mov	$EP_CFG, %edi
	movl	$BAR0, 0x10(%rdi)
	movl	$0, 0x14(%rdi)
	movw	$0x0006, 0x04(%rdi)
	movq	$0x1111, DST
	mov	DST, %rbx			// DST is cached now
	EXPECT	%rbx, 0x1111, 0x0A
	mov	SRC, %rbx			// SRC too, then written: through to memory
	movabs	$0x0BADC0DE600DF00D, %rax
	mov	%rax, SRC
	mov	$BAR0, %esi
	movq	$SRC, 0x08(%rsi)
	movq	$DST, 0x10(%rsi)
	movq	$8, 0x18(%rsi)
	movq	$1, 0x28(%rsi)
	mov	$100000, %ecx
1:	cmpq	$2, 0x30(%rsi)
	je	2f
	loop	1b
2:	EXPECT	0x30(%rsi), 2, 0x0B
	mov	DST, %rbx			// the device's write invalidated the line
	EXPECT	%rbx, 0x0BADC0DE600DF00D, 0x0C

	// 7. WBINVD
	wbinvd
	mov	BUF, %rbx
	EXPECT	%rbx, 0x112233445A5AAA88, 0x0D
	mov	DST, %rbx
	EXPECT	%rbx, 0x0BADC0DE600DF00D, 0x0E

	// 8. CPUID Fn8000_001D (AMD64 APM Vol. 3) describes the caches: L1
	//    data (type 1) 4096 sets x 1 way x 64 B, L1 instruction (type 2)
	//    1024 x 1 x 64 B, then the null subleaf
	mov	$0x8000001d, %eax
	xor	%ecx, %ecx
	cpuid
	EXPECT	%rax, 0x121, 0x10
	EXPECT	%rbx, 0x3f, 0x11
	EXPECT	%rcx, 0xfff, 0x12
	mov	$0x8000001d, %eax
	mov	$1, %ecx
	cpuid
	EXPECT	%rax, 0x122, 0x13
	EXPECT	%rcx, 0x3ff, 0x14
	mov	$0x8000001d, %eax
	mov	$2, %ecx
	cpuid
	EXPECT	%rax, 0, 0x15

	lea	msg(%rip), %rsi
	call	puts
	jmp	pass

msg:	.asciz	"DCACHE OK\n"

	BOOT64_END
