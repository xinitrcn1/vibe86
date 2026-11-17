// msr: the model-specific registers v1 adds (64-bit mode, CPL 0)
//   MTRRCAP and MCG_CAP read-only values; MTRRs: default type, variable and
//     fixed ranges accept valid types and #GP on reserved bits and types
//     (Intel SDM Vol. 3A 12.11); IA32_PAT accepts UC- (7) but not 2/3;
//   MC0_STATUS takes only 0 (Vol. 3B 17.3.2.2);
//   IA32_TSC_ADJUST follows a TSC write, and a TSC_ADJUST write moves the
//     TSC (Vol. 3B 18.17.3);
//   EFER reserved bits and LME with paging on: #GP; a non-canonical FS_BASE:
//     #GP; an unknown MSR: #GP;
//   the AMD core performance counters count retired instructions and RDPMC
//     reads them (AMD64 APM Vol. 2 13.2);
//   CLZERO zeroes the aligned 64-byte line of rAX (AMD64 APM Vol. 3);
//   PREFETCH (0F 0D /0, /5) never faults, even at a non-canonical address;
//   CPUID reports MTRR, MCE, MCA, 3DNowPrefetch, OSVW, TOPOEXT, PERFCORE,
//     CLZERO.
// expect-out: MSR OK
#include "boot64.inc"
#include "expect.inc"
	.set	SCR, 0x40000

	.macro	GPWR msr, hi, lo
	CATCH	9877f
	mov	$\msr, %ecx
	mov	$\hi, %edx
	mov	$\lo, %eax
	wrmsr
	jmp	fail
9877:	EXPECT_VEC 13, 0x1000 + \@
	.endm

	.macro	WR msr, hi, lo
	mov	$\msr, %ecx
	mov	$\hi, %edx
	mov	$\lo, %eax
	wrmsr
	.endm

	.macro	RD msr
	mov	$\msr, %ecx
	rdmsr
	shl	$32, %rdx
	or	%rdx, %rax
	.endm

main64:
	call	expect_init

	RD	0xfe
	EXPECT	%rax, 0x508, 0x01		// MTRRCAP: 8 variable, fixed, WC
	RD	0x179
	EXPECT	%rax, 0x101, 0x02		// MCG_CAP: one bank, MCG_CTL
	GPWR	0xfe, 0, 0
	GPWR	0x179, 0, 0

	// MTRRs
	WR	0x2ff, 0, 0xc06
	RD	0x2ff
	EXPECT	%rax, 0xc06, 0x10
	GPWR	0x2ff, 0, 0xc02			// type 2: reserved
	GPWR	0x2ff, 0, 0xd06			// bit 8: reserved
	WR	0x200, 0, 0x80000006		// PHYSBASE0: WB at 2 GiB
	RD	0x200
	EXPECT	%rax, 0x80000006, 0x11
	GPWR	0x200, 0, 0x80000007		// type 7: reserved for MTRRs
	GPWR	0x201, 0, 0x80000801		// PHYSMASK0 bit 0: reserved
	WR	0x201, 0x000fffff, 0x80000800
	RD	0x201
	EXPECT	%rax, 0x000fffff80000800, 0x12
	WR	0x250, 0x06060606, 0x06060606
	RD	0x250
	EXPECT	%rax, 0x0606060606060606, 0x13
	GPWR	0x26f, 0x06060606, 0x06060603	// a fixed range with type 3
	GPWR	0x26f, 0x07060606, 0x06060606	// type 7 (UC-) is PAT-only
	// PAT
	WR	0x277, 0x00070406, 0x00070407	// UC- accepted
	RD	0x277
	EXPECT	%rax, 0x0007040600070407, 0x14
	GPWR	0x277, 0x00070406, 0x00070402
	WR	0x277, 0x00070406, 0x00070406

	// MCA bank 0
	GPWR	0x401, 0, 1
	WR	0x401, 0, 0
	RD	0x401
	EXPECT	%rax, 0, 0x20
	WR	0x400, 0xffffffff, 0xffffffff	// MC0_CTL
	RD	0x400
	EXPECT	%rax, 0xffffffffffffffff, 0x21

	// TSC and IA32_TSC_ADJUST
	RD	0x3b
	EXPECT	%rax, 0, 0x30
	WR	0x10, 0x1000, 0			// TSC := 1000_0000_0000h
	RD	0x3b				// TSC_ADJUST: + (1000_0000_0000h - old TSC)
	mov	%rax, %rbx
	shr	$40, %rbx			// about 10h << 40, a little less
	EXPECT	%rbx, 0xf, 0x31
	WR	0x3b, 0, 0			// back: the TSC drops by as much
	rdtsc
	shl	$32, %rdx
	or	%rdx, %rax
	shr	$32, %rax
	EXPECT	%rax, 0, 0x32			// the TSC is small again

	// EFER, FS_BASE, unknown
	RD	0xc0000080
	mov	%rax, %r12
	mov	%eax, %eax
	CATCH	1f
	mov	$0xc0000080, %ecx
	mov	%r12d, %eax
	or	$0x2, %eax			// bit 1: reserved
	xor	%edx, %edx
	wrmsr
	jmp	fail
1:	EXPECT_VEC 13, 0x40
	CATCH	1f
	mov	$0xc0000080, %ecx
	mov	%r12d, %eax
	and	$~0x100, %eax			// clear LME with paging on
	xor	%edx, %edx
	wrmsr
	jmp	fail
1:	EXPECT_VEC 13, 0x41
	GPWR	0xc0000100, 0x00800000, 0	// FS_BASE not canonical
	GPWR	0x12345, 0, 0
	CATCH	1f
	mov	$0x12345, %ecx
	rdmsr
	jmp	fail
1:	EXPECT_VEC 13, 0x42

	// performance counter 0: retired instructions, OS and USR
	WR	0xc0010201, 0, 0
	WR	0xc0010200, 0, 0x4300c0
	nop
	nop
	nop
	nop
	nop
	WR	0xc0010200, 0, 0		// stop
	xor	%ecx, %ecx
	rdpmc
	// the 5 NOPs, the stopping MOVs and WRMSR: 5 <= count <= 12
	mov	%rax, %rbx
	cmp	$5, %rbx
	setb	%cl
	cmp	$12, %rbx
	seta	%dl
	or	%dl, %cl
	movzbq	%cl, %rcx
	EXPECT	%rcx, 0, 0x50
	RD	0xc0010001			// legacy PerfCtr... alias: PERF_CTL1
	EXPECT	%rax, 0, 0x51
	CATCH	1f
	mov	$6, %ecx
	rdpmc
	jmp	fail
1:	EXPECT_VEC 13, 0x52

	// CLZERO: the aligned line of rAX
	mov	$SCR, %rdi
	mov	$-1, %rax
	mov	$24, %ecx
	rep stosq				// SCR .. SCR + 191 all ones
	mov	$SCR + 0x47, %rax
	clzero
	EXPECT	SCR + 0x38, -1, 0x60
	EXPECT	SCR + 0x40, 0, 0x61
	EXPECT	SCR + 0x78, 0, 0x62
	EXPECT	SCR + 0x80, -1, 0x63

	// PREFETCH never faults
	mov	$0x8000000000000000, %rax
	prefetch (%rax)
	.byte	0x0f, 0x0d, 0x28		// 0F 0D /5 [rax]
	prefetchw (%rax)

	// CPUID
	mov	$1, %eax
	cpuid
	and	$(1 << 12) | (1 << 7) | (1 << 14), %edx
	EXPECT	%rdx, (1 << 12) | (1 << 7) | (1 << 14), 0x70
	mov	$0x80000001, %eax
	cpuid
	and	$(1 << 8) | (1 << 9) | (1 << 22) | (1 << 23), %ecx
	EXPECT	%rcx, (1 << 8) | (1 << 9) | (1 << 22) | (1 << 23), 0x71
	mov	$0x80000008, %eax
	cpuid
	and	$1, %ebx
	EXPECT	%rbx, 1, 0x72

	lea	endmsg(%rip), %rsi
	call	puts
	jmp	pass

endmsg:	.asciz	"MSR OK\n"
	BOOT64_END
