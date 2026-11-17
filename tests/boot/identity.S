// identity: the processor identity generated from rtl/common (D-013), from
// reset in real-address mode.
//   EDX after reset = CPUID.1:EAX (the signature; reset.toml names it)
//   CPUID.0: the vendor string "VibingVibing", the maximum basic leaf 0Dh
//   CPUID.1:EAX = 00B00F01h (family 1Ah: base 0Fh + extended 0Bh, model 0,
//   stepping 1); POPCNT (ECX[23]) reported, AVX (ECX[28]) and FPU (EDX[0])
//   not (v1 doesn't implement them: features without 'since')
//   CPUID.1:ECX[27] (OSXSAVE) follows CR4.OSXSAVE
//   CPUID.8000_0000h..8000_0004h: the maximum extended leaf, the brand string
//   an unknown leaf and a leaf above the maximum return zeros
// Any mismatch prints 'X' and the step number and fails (exit 3).
// expect-out: VibingVibing
// expect-out: vibe86 Venice-class microcoded processor
// expect-out: OK
	.code16
	.equ	buf, 0x6000			// scratch in low RAM (DS = 0)
	.text
	.globl start
start:
	mov	%edx, %ebp			// the reset value, before anything clobbers it
	xor	%ax, %ax
	mov	%ax, %ds
	mov	%ax, %ss
	mov	$0x7000, %sp

	// vendor
	xor	%eax, %eax
	cpuid
	mov	$1, %di
	cmp	$0xd, %eax
	jne	fail
	mov	%ebx, buf
	mov	%edx, buf + 4
	mov	%ecx, buf + 8
	movb	$0, buf + 12
	mov	$buf, %si
	call	puts
	call	newline

	// signature, and EDX at reset
	mov	$1, %eax
	cpuid
	mov	$2, %di
	cmp	$0x00b00f01, %eax
	jne	fail
	mov	$3, %di
	cmp	%eax, %ebp
	jne	fail
	mov	$4, %di
	bt	$23, %ecx			// POPCNT: implemented
	jnc	fail
	mov	$5, %di
	bt	$28, %ecx			// AVX: not yet
	jc	fail
	mov	$6, %di
	bt	$0, %edx			// FPU: not yet
	jc	fail
	mov	$7, %di
	bt	$27, %ecx			// OSXSAVE = CR4.OSXSAVE = 0
	jc	fail
	mov	$8, %di
	shr	$24, %ebx			// initial APIC ID 0
	jnz	fail
	mov	%cr4, %eax
	or	$1 << 18, %eax			// CR4.OSXSAVE
	mov	%eax, %cr4
	mov	$1, %eax
	cpuid
	mov	$9, %di
	bt	$27, %ecx
	jnc	fail
	mov	%cr4, %eax
	and	$~(1 << 18), %eax
	mov	%eax, %cr4

	// unknown leaves and leaves above the maximum
	mov	$0x0000000e, %eax		// above the maximum basic leaf
	mov	$0x12345678, %ebx
	mov	$0x12345678, %ecx
	mov	$0x12345678, %edx
	cpuid
	mov	$10, %di
	or	%ebx, %eax
	or	%ecx, %eax
	or	%edx, %eax
	jnz	fail
	mov	$0x00000005, %eax		// below the maximum, not defined
	cpuid
	mov	$11, %di
	or	%ebx, %eax
	or	%ecx, %eax
	or	%edx, %eax
	jnz	fail
	mov	$7, %eax			// leaf 7, subleaf 0: max subleaf 1
	xor	%ecx, %ecx
	cpuid
	mov	$12, %di
	cmp	$1, %eax
	jne	fail
	mov	$7, %eax			// leaf 7, subleaf 2: zeros
	mov	$2, %ecx
	cpuid
	mov	$13, %di
	or	%ebx, %eax
	or	%ecx, %eax
	or	%edx, %eax
	jnz	fail

	// extended leaves
	mov	$0x80000000, %eax
	cpuid
	mov	$14, %di
	cmp	$0x80000021, %eax
	jne	fail
	mov	$0x80000001, %eax
	cpuid
	mov	$15, %di
	bt	$29, %edx			// LM
	jnc	fail
	mov	$buf, %di
	mov	$0x80000002, %esi
1:	mov	%esi, %eax
	cpuid
	mov	%eax, (%di)
	mov	%ebx, 4(%di)
	mov	%ecx, 8(%di)
	mov	%edx, 12(%di)
	add	$16, %di
	inc	%esi
	cmp	$0x80000005, %esi
	jne	1b
	movb	$0, buf + 48
	mov	$buf, %si
	call	puts
	call	newline
	mov	$0x80000008, %eax
	cpuid
	mov	$16, %di
	cmp	$0x3934, %ax			// 52 physical, 57 linear address bits
	jne	fail

	mov	$'O', %al
	call	putc
	mov	$'K', %al
	call	putc
	call	newline
	mov	$0x604, %dx
	mov	$0x2000, %ax
	out	%ax, %dx
1:	hlt
	jmp	1b

fail:	mov	$'X', %al
	call	putc
	mov	%di, %ax
	add	$'0', %al
	call	putc
	call	newline
	mov	$0x604, %dx
	mov	$0x3333, %eax			// fail with code 3
	or	$3 << 16, %eax
	out	%eax, %dx
1:	hlt
	jmp	1b

newline:
	mov	$'\n', %al
putc:	push	%dx
	mov	$0x3f8, %dx
	out	%al, %dx
	pop	%dx
	ret
puts:	lodsb
	test	%al, %al
	jz	1f
	call	putc
	jmp	puts
1:	ret

	.org	0xfff0
	.byte	0xe9
	.word	(start - 2f) & 0xffff
2:
	.org	0x10000
