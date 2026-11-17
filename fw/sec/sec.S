/*
 * SEC: the security phase (PI 1.8 Vol. 1 ch. 13 "SEC"). The core comes out
 * of reset in real mode at F000:FFF0 (CS base FFFF_0000h; Intel SDM Vol. 3A
 * 10.1.4 "First Instruction Executed"); this code switches to protected mode
 * and then IA-32e mode with the first 4 GiB identity-mapped (1-GiB pages,
 * SDM Vol. 3A 10.8.5 "Initializing IA-32e Mode"), sets up a stack in
 * temporary RAM and calls the PEI image's entry point.
 *
 * The GDT stays in flash: its descriptors have the accessed bit set, so the
 * core never writes to it (SDM Vol. 3A 3.4.5.1).
 *
 * This is the last 64 KiB of the flash image (layout.h SEC_BASE).
 */
#include "layout.h"

	.set	POST, 0x80

	.code16
	.text
sec_start:
	cli
	cld
	mov	$0x01, %al
	out	%al, $POST
	in	$0x92, %al		/* fast A20 on (port 92h, bit 1) */
	or	$0x02, %al
	and	$0xFE, %al
	out	%al, $0x92
	lgdtl	%cs:(gdtr - sec_start)
	mov	%cr0, %eax
	or	$1, %eax		/* PE */
	mov	%eax, %cr0
	ljmpl	$SEL_CODE32, $SEC_BASE + (pm32 - sec_start)

	.code32
pm32:
	mov	$SEL_DATA, %ax
	mov	%ax, %ds
	mov	%ax, %es
	mov	%ax, %ss
	mov	%ax, %fs
	mov	%ax, %gs
	mov	$TEMP_STACK_TOP, %esp
	mov	$0x02, %al
	out	%al, $POST

	/* page tables in temporary RAM: PML4[0] -> PDPT, PDPT[0..3] 1-GiB pages */
	mov	$SEC_PML4, %edi
	xor	%eax, %eax
	mov	$0x2000 / 4, %ecx
	rep stosl
	movl	$SEC_PDPT | 0x23, SEC_PML4		/* P, RW, A */
	movl	$0x00000000 | 0xE3, SEC_PDPT + 0	/* P, RW, A, D, PS */
	movl	$0x40000000 | 0xE3, SEC_PDPT + 8
	movl	$0x80000000 | 0xE3, SEC_PDPT + 16
	movl	$0xC0000000 | 0xE3, SEC_PDPT + 24

	mov	%cr4, %eax
	or	$0x20, %eax		/* PAE */
	mov	%eax, %cr4
	mov	$SEC_PML4, %eax
	mov	%eax, %cr3
	mov	$0xC0000080, %ecx	/* IA32_EFER.LME */
	rdmsr
	or	$0x100, %eax
	wrmsr
	mov	%cr0, %eax
	or	$0x80000000, %eax	/* PG: IA-32e mode active */
	mov	%eax, %cr0
	ljmp	$SEL_CODE64, $SEC_BASE + (lm64 - sec_start)

	.code64
lm64:
	mov	$SEL_DATA, %ax
	mov	%ax, %ds
	mov	%ax, %es
	mov	%ax, %ss
	mov	$0x03, %al
	out	%al, $POST

	/* the PEI image's entry: PE32+ at PEI_BASE (e_lfanew at 3Ch,
	   AddressOfEntryPoint at 28h into the NT headers) */
	mov	$PEI_BASE, %esi
	cmpw	$0x5A4D, (%rsi)
	jne	no_pei
	mov	0x3C(%rsi), %eax
	add	%rsi, %rax
	cmpl	$0x00004550, (%rax)
	jne	no_pei
	mov	0x28(%rax), %eax
	add	%rsi, %rax

	/* pei_main(temp RAM base, temp RAM size, TSC at SEC entry) */
	mov	$TEMP_STACK_TOP, %rsp
	mov	$TEMP_RAM_BASE, %ecx
	mov	$TEMP_RAM_SIZE, %edx
	push	%rax
	rdtsc
	shl	$32, %rdx
	or	%rdx, %rax
	mov	%rax, %r8
	pop	%rax
	mov	$TEMP_RAM_SIZE, %edx
	sub	$32, %rsp		/* the callee's register home area (MS x64 ABI) */
	call	*%rax
no_pei:
	mov	$0xEE, %al
	out	%al, $POST
1:	cli
	hlt
	jmp	1b

	.balign	8
gdt:
	.quad	0
	.quad	0x00cf9b000000ffff	/* 08: 32-bit code, accessed */
	.quad	0x00cf93000000ffff	/* 10: data, accessed */
	.quad	0x00af9b000000ffff	/* 18: 64-bit code, accessed */
gdt_end:
gdtr:
	.word	gdt_end - gdt - 1
	.long	SEC_BASE + (gdt - sec_start)

	/* the reset vector: F000:FFF0 */
	.org	0xfff0
	.byte	0xe9
	.word	(sec_start - 9f) & 0xffff
9:
	.org	0x10000
