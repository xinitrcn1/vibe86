// far32: 32-bit protected mode decode and transfer regressions.
//   JMP ptr16:16 (66 EA) into a 16-bit code segment and JMP ptr16:32 back;
//   CALL ptr16:32 with RET imm16 far; CALL ptr16:16 (66 9A) to 16-bit code
//     and its 16-bit far return;
//   RET imm16 near with 66h (a 16-bit IP popped, ESP + 2 + imm16);
//   MOV moffs with 67h (a 16-bit offset); MOV r32, Sreg zero-extends;
//   ENTER's 32-bit frame; JMP/CALL through m16:32 and m16:16 in memory;
//   POP SS holds a single-step trap for one instruction;
//   call gates from ring 3: a DPL-0 gate #GP(gate); a gate to a data
//   segment #GP(target); JMP through a gate to a more privileged
//   non-conforming segment #GP(target) (JMP can't change privilege); a
//   gate to a conforming segment keeps CPL 3; a DPL-3 gate to ring 0 works
//   (the target's DPL is checked by the gate rule, not the direct-transfer
//   one) and RET far returns to ring 3.
// expect-out: FAR32 OK
	.set	BASE, 0xffff0000
	.set	GDT, 0x1000
	.set	IDT, 0x2000
	.set	TSSA, 0x30000
	.set	KSTACK, 0x80000
	.set	RSP0, 0x90000
	.set	USTACK, 0xa0000
	.set	LASTVEC, 0x600
	.set	LASTERR, 0x604
	.set	RESUME, 0x608
	.set	NDB, 0x60c
	.set	DBEIPS, 0x610
	.set	FARP, 0x700
	.set	STUB, 0x7000
	.set	STUB2, 0x7100
	.set	gdtr_ram, 0x500
	.set	idtr_ram, 0x510

	.code16
	.text
start:
	cli
	lgdtl	%cs:gdtr_rom
	mov	%cr0, %eax
	or	$1, %eax
	mov	%eax, %cr0
	ljmpl	$0x08, $BASE + pm32

	.code32
pm32:
	mov	$0x10, %ax
	mov	%ax, %ds
	mov	%ax, %es
	mov	%ax, %ss
	mov	$KSTACK, %esp
	mov	$BASE + gdt_rom, %esi
	mov	$GDT, %edi
	mov	$(gdt_end - gdt_rom), %ecx
	rep movsb
	// call gates: 0x38 DPL 3 -> 0x08:gate_entry; 0x40 DPL 0 -> 0x08:gate_entry;
	// 0x50 DPL 3 -> 0x48 (conforming):conf_entry; 0x60 DPL 3 -> 0x10 (data)
	.irp	g, 0x38, 0x40, 0x50, 0x60
	movl	$0x00080000, GDT + \g
	movl	$0x0000ec00, GDT + \g + 4
	.endr
	mov	$BASE + gate_entry, %eax
	call	set_off38
	movb	$0x8c, GDT + 0x40 + 5		// DPL 0
	mov	$BASE + gate_entry, %eax
	mov	%ax, GDT + 0x40
	shr	$16, %eax
	mov	%ax, GDT + 0x46
	mov	$BASE + conf_entry, %eax
	mov	%ax, GDT + 0x50
	movw	$0x48, GDT + 0x52
	shr	$16, %eax
	mov	%ax, GDT + 0x56
	movw	$0x10, GDT + 0x62
	// TSS (0x30)
	movl	$0x00000067 | ((TSSA & 0xffff) << 16), GDT + 0x30
	movl	$0x00008900 | ((TSSA >> 16) & 0xff), GDT + 0x34
	movw	$(gdt_end - gdt_rom - 1), gdtr_ram
	movl	$GDT, gdtr_ram + 2
	lgdt	gdtr_ram
	mov	$TSSA, %edi
	xor	%eax, %eax
	mov	$0x68 / 4, %ecx
	rep stosl
	movl	$RSP0, TSSA + 4
	movl	$0x10, TSSA + 8
	mov	$0x30, %ax
	ltr	%ax
	// IDT: exception stubs 0-31 (DPL 0), 0x80 DPL 3 back to ring 0
	xor	%ecx, %ecx
1:	mov	$BASE + exc_table, %esi
	mov	(%esi, %ecx, 4), %eax
	mov	$0x8e, %edx
	call	set_gate
	inc	%ecx
	cmp	$32, %ecx
	jne	1b
	mov	$BASE + h_back, %eax
	mov	$0x80, %ecx
	mov	$0xee, %edx
	call	set_gate
	movw	$256 * 8 - 1, idtr_ram
	movl	$IDT, idtr_ram + 2
	lidt	idtr_ram
	movl	$0, RESUME

	// ---- JMP ptr16:16 into 16-bit code, ptr16:32 back
	ljmpw	$0x18, $c16
back32:
	cmp	$0x5a5a, %bx
	jne	fail

	// ---- CALL ptr16:32 + RET 4 far
	push	$0x11111111
	mov	%esp, %ebp
	lcall	$0x08, $BASE + far_fn
	add	$4, %ebp
	cmp	%ebp, %esp			// the argument popped
	jne	fail
	// ---- CALL ptr16:16 (66 9A) to 16-bit code and its 16-bit far return,
	// from a stub in low memory (a 16-bit call pushes IP, 16 bits of EIP)
	mov	$BASE + stub2, %esi
	mov	$STUB2, %edi
	mov	$stub2_end - stub2, %ecx
	rep movsb
	mov	%esp, %ebp
	mov	$STUB2, %eax
	jmp	*%eax
stub2_back:
	cmp	%ebp, %esp
	jne	fail
	cmp	$0x1616, %cx
	jne	fail
	cmp	$STUB2 + (stub2_ret - stub2), %di	// the IP f16 found on the stack
	jne	fail
	cmp	$0x08, %si				// and CS
	jne	fail

	// ---- RET imm16 near with 66h: pops IP (2 bytes) then imm16
	mov	$BASE + stub, %esi
	mov	$STUB, %edi
	mov	$stub_end - stub, %ecx
	rep movsb
	mov	%esp, %ebp
	push	$0x22222222
	pushw	$STUB
	.byte	0x66, 0xc2, 4, 0		// retw $4 -> STUB -> stub_back
stub_back:
	cmp	%ebp, %esp
	jne	fail

	// ---- moffs16 (67h), MOV r32, Sreg, ENTER
	movl	$0xcafef00d, 0x1234
	xor	%eax, %eax
	.byte	0x67, 0xa1, 0x34, 0x12		// mov 0x1234(16-bit), %eax
	cmp	$0xcafef00d, %eax
	jne	fail
	mov	$-1, %eax
	mov	%ds, %eax
	cmp	$0x10, %eax
	jne	fail
	mov	%esp, %esi
	enter	$8, $0
	lea	-4(%esi), %eax
	cmp	%eax, %ebp
	jne	fail
	lea	-12(%esi), %eax
	cmp	%eax, %esp
	jne	fail
	leave

	// ---- far JMP/CALL through memory: m16:32 and m16:16
	movl	$BASE + mj1, FARP
	movw	$0x08, FARP + 4
	ljmpl	*FARP
	jmp	fail
mj1:	movw	$mj16, FARP
	movw	$0x18, FARP + 2
	ljmpw	*FARP				// 66 FF /5: m16:16
	jmp	fail
mj2:	cmp	$0x7777, %dx
	jne	fail
	movl	$BASE + mc1, FARP
	movw	$0x08, FARP + 4
	lcalll	*FARP
	cmp	$0x3333, %si
	jne	fail

	// ---- POP SS with TF: no trap at the instruction after it
	movl	$0, NDB
	mov	$BASE + h_db, %eax
	mov	$1, %ecx
	mov	$0x8e, %edx
	call	set_gate
	push	%ss
	pushf
	orl	$0x100, (%esp)
	popf
	nop
	pop	%ss
pss_b:	nop
pss_c:	nop
	pushf
	andl	$~0x100, (%esp)
	popf
	mov	NDB, %ecx
1:	test	%ecx, %ecx
	jz	2f
	dec	%ecx
	cmpl	$BASE + pss_b, DBEIPS(, %ecx, 4)
	je	fail
	jmp	1b
2:	cmpl	$3, NDB
	jb	fail

	// ---- to ring 3 for the call-gate checks
	push	$0x20 | 3
	push	$USTACK
	pushf
	push	$0x28 | 3
	push	$BASE + ring3
	iret

ring3:
	mov	$0x23, %ax			// IRET to ring 3 nulled DS/ES (DPL 0)
	mov	%ax, %ds
	mov	%ax, %es
	// a DPL-0 gate: #GP(gate selector)
	movl	$BASE + 1f, RESUME
	lcall	$0x43, $0
	jmp	fail3
1:	cmpl	$13, LASTVEC
	jne	fail3
	cmpl	$0x40, LASTERR
	jne	fail3
	// a gate to a data segment: #GP(target selector)
	movl	$BASE + 1f, RESUME
	lcall	$0x63, $0
	jmp	fail3
1:	cmpl	$13, LASTVEC
	jne	fail3
	cmpl	$0x10, LASTERR
	jne	fail3
	// JMP through a gate to ring 0 non-conforming: #GP(target selector)
	movl	$BASE + 1f, RESUME
	ljmp	$0x3b, $0
	jmp	fail3
1:	cmpl	$13, LASTVEC
	jne	fail3
	cmpl	$0x08, LASTERR
	jne	fail3
	// a gate to a conforming segment: CPL stays 3
	lcall	$0x53, $0
	cmp	$3, %ebx
	jne	fail3
	// a DPL-3 gate to ring 0 and back
	lcall	$0x3b, $0
	cmp	$0, %ebx
	jne	fail3
	mov	%cs, %ax
	cmp	$0x2b, %ax
	jne	fail3
	int	$0x80

fail3:	int	$0x80				// back to ring 0, then fail
	jmp	fail3

gate_entry:					// via 0x3b: ring 0
	mov	%cs, %ebx
	and	$3, %ebx
	lret
conf_entry:					// via 0x53: conforming, CPL 3
	mov	%cs, %ebx
	and	$3, %ebx
	lret

h_back:
	// the INT 80h frame's EIP: fail3 on success (the INT before it), fail3 + 2 from fail3
	cmpl	$BASE + fail3 + 2, RSP0 - 20
	je	fail
	mov	$KSTACK, %esp
	mov	$BASE + endmsg, %esi
	call	puts
	mov	$0x604, %dx
	mov	$0x2000, %ax
	out	%ax, %dx
	hlt

far_fn:
	lret	$4
mc1:	mov	$0x3333, %si
	lret

h_db:	push	%eax
	push	%ecx
	mov	NDB, %ecx
	cmp	$8, %ecx
	jae	1f
	mov	8(%esp), %eax
	mov	%eax, DBEIPS(, %ecx, 4)
1:	incl	NDB
	cmpl	$16, NDB
	ja	fail
	pop	%ecx
	pop	%eax
	iret

// exceptions: record vector, error code; resume at RESUME (or fail)
	.macro	EXC_STUB n
exc_stub_\n:
	.if	(\n == 8) || (\n >= 10 && \n <= 14) || (\n == 17)
	.else
	push	$0
	.endif
	push	$\n
	jmp	exc_common
	.endm
	.irp	n, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
	EXC_STUB \n
	.endr
exc_common:
	push	%eax
	mov	4(%esp), %eax
	mov	%eax, LASTVEC
	mov	8(%esp), %eax
	mov	%eax, LASTERR
	mov	RESUME, %eax
	test	%eax, %eax
	jz	fail
	mov	%eax, 12(%esp)
	movl	$0, RESUME
	pop	%eax
	add	$8, %esp
	iret

set_off38:
	mov	%ax, GDT + 0x38
	shr	$16, %eax
	mov	%ax, GDT + 0x3e
	ret

set_gate:
	lea	IDT(, %ecx, 8), %edi
	mov	%ax, (%edi)
	movw	$0x08, 2(%edi)
	movb	$0, 4(%edi)
	mov	%dl, 5(%edi)
	shr	$16, %eax
	mov	%ax, 6(%edi)
	ret

puts:	lodsb
	test	%al, %al
	jz	1f
	call	putc
	jmp	puts
1:	ret
putc:	push	%edx
	mov	$0x3f8, %dx
	out	%al, %dx
	pop	%edx
	ret
fail:	mov	$0x604, %dx
	mov	$0x00013333, %eax
	out	%eax, %dx
	hlt

stub:	mov	$BASE + stub_back, %eax		// at STUB: back to the image
	jmp	*%eax
stub_end:
stub2:	lcallw	$0x18, $f16			// at STUB2
stub2_ret:
	mov	$BASE + stub2_back, %eax
	jmp	*%eax
stub2_end:

	.code16
c16:	mov	%cs, %ax
	cmp	$0x18, %ax
	jne	1f
	mov	$0x5a5a, %bx
	ljmpl	$0x08, $BASE + back32		// 66 EA: ptr16:32 from 16-bit code
1:	ljmpl	$0x08, $BASE + fail
f16:	mov	$0x1616, %cx
	mov	%esp, %ebx			// SS is a 32-bit (B = 1) stack
	mov	%ss:(%ebx), %di			// the pushed IP
	mov	%ss:2(%ebx), %si		// and CS
	lret					// 16-bit: IP, CS
mj16:	mov	$0x7777, %dx
	ljmpl	$0x08, $BASE + mj2
	.code32

	.balign	8
exc_table:
	.irp	n, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
	.long	BASE + exc_stub_\n
	.endr
gdt_rom:
	.quad	0
	.quad	0x00cf9b000000ffff		// 0x08: code 32, DPL 0
	.quad	0x00cf93000000ffff		// 0x10: data, DPL 0
	.quad	0xff009bff0000ffff		// 0x18: code 16, base FFFF0000h, limit FFFFh
	.quad	0x00cff3000000ffff		// 0x20: data, DPL 3
	.quad	0x00cffb000000ffff		// 0x28: code 32, DPL 3
	.quad	0				// 0x30: TSS
	.quad	0				// 0x38: call gate DPL 3
	.quad	0				// 0x40: call gate DPL 0
	.quad	0x00cf9f000000ffff		// 0x48: conforming code, DPL 0
	.quad	0				// 0x50: call gate -> 0x48
	.quad	0				// 0x58
	.quad	0				// 0x60: call gate -> data
gdt_end:
gdtr_rom:
	.word	gdt_end - gdt_rom - 1
	.long	BASE + gdt_rom
endmsg:	.asciz	"FAR32 OK\n"

	.org	0xfff0
	.byte	0xe9
	.word	(start - 9f) & 0xffff
9:
	.org	0x10000
