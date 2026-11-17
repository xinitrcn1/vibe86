// decode64: decoder and form-selection regressions in 64-bit mode.
//   opcodes invalid in 64-bit mode raise #UD -- they used to run as the
//     opcode 8 below them (EA as E8 CALL, D5 as D0 ROR, 06 as 00 ADD...);
//   90: NOP, REX.W NOP, 66 NOP, XCHG r8/r9 with REX.B, PAUSE (F3, 66 F3);
//   0F 1F multi-byte NOPs with 66h / REX.W;
//   mandatory 66h: ADCX (with REX.R/REX.W), CLWB, CLFLUSHOPT; ADOX;
//   CRC32 r32, r/m16 (66 F2: the source has the operand size), r/m8, r/m64;
//   MOV r32/r64, Sreg (zero-extended selector) and MOV m16, Sreg (2 bytes
//     at any operand size);
//   MOV moffs with 64-bit and (67h) 32-bit offsets, AL/AX/EAX/RAX;
//   SETcc on registers (incl. SPL/DIL with REX) and memory;
//   far JMP/CALL through m16:32, m16:64 (REX.W) and m16:16 (66h);
//   MOV to/from CR with 66h (ignored);
//   ENTER with a 64-bit frame; CMPXCHG32 with a register destination.
// expect-out: DEC64 OK
#include "boot64.inc"
#include "expect.inc"
	.set	SCR, 0x40000
	.set	FARP, 0x40100
	.set	CRC12, 0x8caa3b9f		// CRC-32C of "12" from FFFFFFFFh, not inverted

	.macro	UD bytes:vararg
	CATCH	9877f
	.byte	\bytes
	jmp	fail
9877:	EXPECT_VEC 6, 0x1000 + \@
	.endm

main64:
	call	expect_init

	// ---- invalid in 64-bit mode: #UD (not the opcode 8 below)
	UD	0x06				// PUSH ES (00 ADD)
	UD	0x07				// POP ES
	UD	0x0e				// PUSH CS (08 OR)
	UD	0x16
	UD	0x17
	UD	0x1e
	UD	0x1f
	UD	0x27				// DAA (20 AND)
	UD	0x2f				// DAS
	UD	0x37				// AAA (30 XOR)
	UD	0x3f				// AAS (38 CMP)
	UD	0x60				// PUSHA
	UD	0x61				// POPA
	UD	0xce				// INTO (C8 ENTER)
	UD	0xd4, 0x0a			// AAM
	UD	0xd5, 0x0a			// AAD (D0 ROR)
	UD	0xd6				// SALC
	UD	0xea, 0, 0, 0, 0, 0x08, 0	// JMP ptr16:32 (E8 CALL)
	UD	0x9a, 0, 0, 0, 0, 0x08, 0	// CALL ptr16:32
	UD	0x0f, 0x38, 0xf6, 0xc3		// 0F 38 F6 without 66/F3
	// +r opcodes still fold onto their base
	movabsq	$0x1111, %rdi
	.byte	0x57				// push %rdi
	.byte	0x41, 0x58			// pop %r8
	EXPECT	%r8, 0x1111, 0x11

	// ---- 90 and friends
	movabsq	$0xaaaa, %rax
	movabsq	$0xbbbb, %r8
	movabsq	$0xcccc, %r9
	.byte	0x90				// nop
	.byte	0x48, 0x90			// REX.W nop
	.byte	0x66, 0x90			// 66 nop
	.byte	0xf3, 0x90			// pause
	.byte	0x66, 0xf3, 0x90		// pause
	.byte	0x66, 0x0f, 0x1f, 0x44, 0, 0	// nopw 0(%rax,%rax)
	.byte	0x48, 0x0f, 0x1f, 0x40, 0	// REX.W nop
	EXPECT	%rax, 0xaaaa, 0x20
	.byte	0x41, 0x90			// xchg %eax, %r8d
	EXPECT	%rax, 0xbbbb, 0x21
	EXPECT	%r8, 0xaaaa, 0x22
	.byte	0x49, 0x91			// xchg %rax, %r9
	EXPECT	%rax, 0xcccc, 0x23
	EXPECT	%r9, 0xbbbb, 0x24

	// ---- mandatory 66h
	stc
	mov	$5, %ebx
	mov	$7, %r11d
	.byte	0x66, 0x44, 0x0f, 0x38, 0xf6, 0xdb	// adcx %ebx, %r11d
	EXPECT	%r11, 13, 0x30
	movabsq	$0xffffffffffffffff, %rsi
	mov	$1, %edi
	clc
	.byte	0x66, 0x48, 0x0f, 0x38, 0xf6, 0xfe	// adcx %rsi, %rdi
	setc	%al
	movzbq	%al, %rax
	EXPECT	%rdi, 0, 0x31
	EXPECT	%rax, 1, 0x32
	// ADOX: OF in and out, CF untouched
	mov	$0x7fffffff, %eax
	add	%eax, %eax			// OF = 1, CF = 0
	mov	$1, %ebx
	mov	$2, %ecx
	adox	%ebx, %ecx			// 1 + 2 + OF
	setc	%dl
	seto	%dh
	movzwq	%dx, %rdx
	EXPECT	%rcx, 4, 0x33
	EXPECT	%rdx, 0, 0x34
	mov	$-1, %ecx
	xor	%eax, %eax			// OF = 0
	stc
	adox	%ebx, %ecx			// carry out into OF
	setc	%dl
	seto	%dh
	movzwq	%dx, %rdx
	EXPECT	%rcx, 0, 0x35
	EXPECT	%rdx, 0x0101, 0x36
	clwb	SCR
	clflushopt SCR
	clflush	SCR

	// ---- CRC32 (CRC-32C): known values
	mov	$0xffffffff, %eax
	movabsq	$0x3938373635343332, %rbx	// "23456789"
	mov	$'1', %cl
	crc32b	%cl, %eax
	crc32q	%rbx, %rax			// 64-bit destination form
	not	%eax
	EXPECT	%rax, 0xe3069283, 0x40		// CRC-32C("123456789")
	mov	$0xffffffff, %eax
	mov	$0x3231, %bx			// "12"
	crc32w	%bx, %eax			// 66 F2 0F 38 F1
	mov	$0x36353433, %ebx		// "3456"
	crc32l	%ebx, %eax
	mov	$0x3837, %bx			// "78"
	crc32w	%bx, %eax
	mov	$'9', %bl
	crc32b	%bl, %eax
	not	%eax
	EXPECT	%rax, 0xe3069283, 0x41
	movw	$0x3231, SCR
	mov	$0xffffffff, %r10d
	crc32w	SCR, %r10d			// 66 F2 REX 0F 38 F1, memory
	EXPECT	%r10, CRC12, 0x42

	// ---- MOV Sreg
	movabsq	$0xffffffffffffffff, %rax
	mov	%ds, %eax			// 8C D8: zero-extended to 64 bits
	EXPECT	%rax, 0x10, 0x50
	movabsq	$0xffffffffffffffff, %rax
	mov	%ds, %rax			// REX.W 8C
	EXPECT	%rax, 0x10, 0x51
	movabsq	$0xffffffffffffffff, %rax
	mov	%ds, %ax			// 66 8C: 16 bits written
	EXPECT	%rax, 0xffffffffffff0010, 0x52
	movq	$-1, SCR
	movw	%ds, SCR			// 8C /m: two bytes whatever the size
	.byte	0x48, 0x8c, 0x1c, 0x25		// mov %ds, SCR (REX.W)
	.long	SCR + 8
	EXPECT	SCR, 0xffffffffffff0010, 0x53
	movq	$-1, SCR + 8
	.byte	0x48, 0x8c, 0x1c, 0x25
	.long	SCR + 8
	EXPECT	SCR + 8, 0xffffffffffff0010, 0x54

	// ---- moffs
	movabsq	$0x1122334455667788, %rax
	movabsq	%rax, 0x40200			// REX.W A3, 64-bit offset
	xor	%eax, %eax
	movabsb	0x40200, %al			// A0
	EXPECT	%rax, 0x88, 0x60
	movabsw	0x40200, %ax			// 66 A1
	EXPECT	%rax, 0x7788, 0x61
	movabsl	0x40200, %eax			// A1
	EXPECT	%rax, 0x55667788, 0x62
	movabsq	0x40200, %rax
	EXPECT	%rax, 0x1122334455667788, 0x63
	xor	%eax, %eax
	.byte	0x67, 0xa1			// mov 0x40204(%eax-size moffs), %eax
	.long	0x40204
	EXPECT	%rax, 0x11223344, 0x64
	mov	$0xabcd, %eax
	.byte	0x67, 0x66, 0xa3		// mov %ax, moffs32
	.long	0x40206
	EXPECT	0x40200, 0xabcd334455667788, 0x65

	// ---- SETcc: registers (incl. SPL/DIL/R12B) and memory
	mov	$1, %eax
	cmp	$2, %eax			// 1 - 2: CF, SF, not ZF
	movabsq	$0xffffffffffffffff, %rdi
	setb	%dil
	setz	%r12b
	setl	SCR
	setge	SCR + 1
	EXPECT	%rdi, 0xffffffffffffff01, 0x70
	movzbq	%r12b, %r12
	EXPECT	%r12, 0, 0x71
	movzwq	SCR, %rax
	EXPECT	%rax, 0x0001, 0x72
	mov	$0x80, %al
	add	%al, %al			// ZF, CF, OF, PF
	seto	%bl
	setp	%bh
	sets	%cl
	seta	%ch
	movzwq	%bx, %rbx
	movzwq	%cx, %rcx
	EXPECT	%rbx, 0x0101, 0x73
	EXPECT	%rcx, 0x0000, 0x74

	// ---- far JMP/CALL through memory: m16:32, m16:64 and m16:16
	lea	fj1(%rip), %rax
	mov	%eax, FARP
	movw	$0x18, FARP + 4
	ljmpl	*FARP				// FF /5: m16:32 (default in 64-bit mode)
	jmp	fail
fj1:	lea	fj2(%rip), %rax
	mov	%rax, FARP
	movw	$0x18, FARP + 8
	.byte	0x48, 0xff, 0x2c, 0x25		// REX.W FF /5: ljmpq *FARP (m16:64)
	.long	FARP
	jmp	fail
fj2:	lea	fc1(%rip), %rax
	mov	%eax, FARP
	movw	$0x18, FARP + 4
	lcalll	*FARP				// FF /3 m16:32: pushes CS and EIP, 4 bytes each
	jmp	fc2
fc1:	movzwq	4(%rsp), %rax
	EXPECT	%rax, 0x18, 0x80
	lretl
fc2:	lea	fc3(%rip), %rax
	mov	%rax, FARP
	movw	$0x18, FARP + 8
	.byte	0x48, 0xff, 0x1c, 0x25		// REX.W FF /3: lcallq *FARP (m16:64)
	.long	FARP
	jmp	fc4
fc3:	movzwq	8(%rsp), %rax			// CS and RIP, 8 bytes each
	EXPECT	%rax, 0x18, 0x81
	lretq
fc4:	// m16:16 (66h): offset 16 bits -- a target below 64 KiB (code copied there)
	lea	far16(%rip), %rsi
	mov	$0x7000, %edi
	mov	$far16_end - far16, %ecx
	rep movsb
	movw	$0x7000, FARP
	movw	$0x18, FARP + 2
	.byte	0x66, 0xff, 0x2c, 0x25		// ljmpw *FARP
	.long	FARP
	jmp	fail
back16:

	// ---- MOV CR with 66h (operand size ignored: 64-bit)
	mov	%cr4, %rax
	.byte	0x66, 0x0f, 0x20, 0xe3		// mov %cr4, %rbx (66h)
	cmp	%rax, %rbx
	jne	fail
	.byte	0x66, 0x0f, 0x22, 0xe3		// mov %rbx, %cr4 (66h)
	mov	%cr4, %rcx
	cmp	%rax, %rcx
	jne	fail

	// ---- ENTER: a 64-bit frame
	mov	%rsp, %r13
	movabsq	$0x1234567812345678, %rbp
	enter	$16, $0
	mov	%r13, %rax
	sub	$8, %rax
	cmp	%rax, %rbp			// RBP = the slot of the old RBP
	jne	fail
	EXPECT	(%rbp), 0x1234567812345678, 0x91
	lea	-24(%r13), %rax
	cmp	%rax, %rsp			// 8 for RBP, 16 for locals
	jne	fail
	leave
	cmp	%r13, %rsp
	jne	fail

	// ---- CMPXCHG32, register destination, comparison fails: the
	// destination is not written (upper half kept), EAX zero-extended
	movabsq	$0x0932562600000005, %r8
	movabsq	$0xffffffff00000004, %rax
	movabsq	$0xb91751dacdbd47f3, %rsi
	cmpxchgl %esi, %r8d
	setz	%cl
	movzbq	%cl, %rcx
	EXPECT	%r8, 0x0932562600000005, 0xa0
	EXPECT	%rax, 0x5, 0xa1
	EXPECT	%rcx, 0, 0xa2
	// succeeds: destination written (zero-extended), ZF
	cmpxchgl %esi, %r8d
	setz	%cl
	movzbq	%cl, %rcx
	EXPECT	%r8, 0xcdbd47f3, 0xa3
	EXPECT	%rcx, 1, 0xa4
	// the destination is the accumulator itself: always equal
	mov	$0x4b01, %eax
	mov	$0x1234, %edi
	cmpxchgw %di, %ax
	setz	%cl
	movzbq	%cl, %rcx
	EXPECT	%rax, 0x1234, 0xa5
	EXPECT	%rcx, 1, 0xa6

	lea	endmsg(%rip), %rsi
	call	puts
	jmp	pass

	.code16
far16:	// at 0x7000, CS = 0x18 (64-bit): this is 64-bit code again
	.code64
	movabsq	$BASE + back16, %rax
	jmp	*%rax
far16_end:

endmsg:	.asciz	"DEC64 OK\n"
	BOOT64_END
