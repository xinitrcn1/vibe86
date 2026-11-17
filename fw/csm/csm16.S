/*
 * CSM16: the 16-bit part of the firmware's Compatibility Support Module,
 * the legacy BIOS image CsmDxe (fw/dxe/csm) copies to F_0000h-F_FFFFh
 * (shadow RAM under the i440FX PAM registers) before a legacy boot.
 *
 * It is small on purpose: the hardware is not a PC's (the disk is a USB
 * floppy behind xHCI, the keyboard is USB, the display controller has no
 * legacy VGA registers), so the BIOS services are written in C on top of
 * the UEFI drivers, which stay alive for a legacy boot (UEFI 2.10 7.4.6:
 * a legacy boot signals the LegacyBoot event group, not ExitBootServices).
 * Each service interrupt enters a thunk here that saves the caller's
 * registers on its stack, switches real mode -> protected mode -> IA-32e
 * mode (Intel SDM Vol. 3A 10.8.5, 10.9), calls CsmDxe's dispatcher with the
 * register frame, and returns the same way (SDM Vol. 3A 10.9.2 "Switching
 * Back to Real-Address Mode"). The firmware's page tables and IDT serve the
 * 64-bit side. What stays in 16-bit code is what must run without the
 * thunk: IRQ0 (the 8254 tick, BDA 46Ch), default IRQ handlers (EOI), and
 * the jump to the boot sector.
 *
 * Interrupts are off during a thunk. A service that has to wait (INT 16h
 * AH=00h with no key) returns with `retry` set; the thunk then opens an
 * interrupt window (STI; NOP; CLI) and calls the dispatcher again, so the
 * timer keeps ticking.
 *
 * The image is 64 KiB at F000:0000 (linked at 0, LIN(x) its linear address);
 * the header at offset 0 is struct csm16_header in fw/dxe/csm/csm.h.
 */
	.set	BASE, 0xF0000
#define LIN(x)	(BASE + ((x) - csm_start))
#define OFF(x)	((x) - csm_start)

	.set	SEL_CODE32, 0x08
	.set	SEL_DATA, 0x10
	.set	SEL_CODE64, 0x18
	.set	SEL_CODE16, 0x20
	.set	SEL_DATA16, 0x28

	.text
	.code16
csm_start:
/* ---- the header (csm.h struct csm16_header) */
	.ascii	"VIBE86CS"		/* 00 signature */
vectors:				/* 08 INT 00h-1Fh handler offsets (0: default) */
	.word	0, 0, 0, 0, 0, 0, 0, 0
	.word	OFF(irq0), OFF(irq_master), OFF(irq_master), OFF(irq_master)
	.word	OFF(irq_master), OFF(irq_master), OFF(irq_master), OFF(irq_master)
	.word	OFF(int10), OFF(int11), OFF(int12), OFF(int13)
	.word	OFF(int14), OFF(int15), OFF(int16), OFF(int17)
	.word	OFF(int18), OFF(int19), OFF(int1a), 0
	.word	0, 0, OFF(dpt), 0	/* 1Eh: the diskette parameter table */
	.word	OFF(irq_slave)		/* 48 INT 70h-77h */
	.word	OFF(iret_only)		/* 4A everything else */
	.word	OFF(boot64)		/* 4C 64-bit entry: the legacy boot */
	.word	OFF(config_table)	/* 4E INT 15h AH=C0h */
	.quad	0			/* 50 cr3: the firmware's page tables */
	.quad	0			/* 58 dispatch: void dispatch(u32 frame) */
	.quad	0			/* 60 rsp64: the dispatcher's stack top */
idtr64:	.word	0			/* 68 the firmware's IDTR */
	.quad	0
	.word	0, 0, 0
retry:	.byte	0			/* 78 set by the dispatcher: wait and call again */
boot_dl: .byte	0			/* 79 the boot drive */
boot_flag: .byte 0			/* 7A the thunk's return goes to the boot sector */
	.byte	0
save_sp: .word	0			/* 7C */
save_ss: .word	0			/* 7E */
frame_lin: .long 0			/* 80 the register frame's linear address */
	.long	0

	.balign	8
gdt:
	.quad	0
	.quad	0x00cf9b000000ffff	/* 08: 32-bit code */
	.quad	0x00cf93000000ffff	/* 10: data */
	.quad	0x00af9b000000ffff	/* 18: 64-bit code */
	.quad	0x00009b0f0000ffff	/* 20: 16-bit code, base F_0000h, 64 KiB */
	.quad	0x000093000000ffff	/* 28: 16-bit data, base 0, 64 KiB */
gdt_end:
gdtr:	.word	gdt_end - gdt - 1	/* 10 bytes: LGDT in 64-bit mode reads them all */
	.long	LIN(gdt)
	.long	0
rm_idtr: .word	0x3FF
	.long	0

/* ---- service interrupts: vector number, then the thunk */
	.macro	SERVICE name, vec
\name:
	pushw	$\vec
	jmp	thunk
	.endm
	SERVICE	int10, 0x10
	SERVICE	int11, 0x11
	SERVICE	int12, 0x12
	SERVICE	int13, 0x13
	SERVICE	int14, 0x14
	SERVICE	int15, 0x15
	SERVICE	int16, 0x16
	SERVICE	int17, 0x17
	SERVICE	int18, 0x18
	SERVICE	int19, 0x19
	SERVICE	int1a, 0x1A

/*
 * The frame, low to high: EDI ESI EBP ESP EBX EDX ECX EAX (PUSHAD), GS FS
 * ES DS, the vector, then the INT's IP CS FLAGS (csm.h struct rm_frame).
 */
thunk:
	pushw	%ds
	pushw	%es
	pushw	%fs
	pushw	%gs
	pushal
again:
	cli
	movw	%ss, %cs:OFF(save_ss)
	movw	%sp, %cs:OFF(save_sp)
	xorl	%eax, %eax
	movw	%ss, %ax
	shll	$4, %eax
	xorl	%ebx, %ebx
	movw	%sp, %bx
	addl	%ebx, %eax
	movl	%eax, %cs:OFF(frame_lin)
	lgdtl	%cs:OFF(gdtr)
	movl	%cr0, %eax
	orb	$1, %al			/* PE */
	movl	%eax, %cr0
	ljmpl	$SEL_CODE32, $LIN(pm32)

	.code32
pm32:
	movw	$SEL_DATA, %ax
	movw	%ax, %ds
	movw	%ax, %es
	movw	%ax, %ss
	movl	%cr4, %eax
	orl	$0x20, %eax		/* PAE */
	movl	%eax, %cr4
	movl	LIN(csm_start) + 0x50, %eax
	movl	%eax, %cr3
	movl	$0xC0000080, %ecx	/* IA32_EFER.LME */
	rdmsr
	orl	$0x100, %eax
	wrmsr
	movl	%cr0, %eax
	orl	$0x80000000, %eax	/* PG: IA-32e mode */
	movl	%eax, %cr0
	ljmp	$SEL_CODE64, $LIN(lm64)

	.code64
lm64:
	movw	$SEL_DATA, %ax
	movw	%ax, %ds
	movw	%ax, %es
	movw	%ax, %ss
	movq	LIN(csm_start) + 0x60, %rsp
	lidt	LIN(idtr64)
	movl	LIN(frame_lin), %ecx
	subq	$32, %rsp		/* the callee's home area (MS x64) */
	call	*LIN(csm_start) + 0x58
to_real:
	/* back: 64-bit -> 32-bit compatibility code -> paging off */
	pushq	$SEL_CODE32
	pushq	$LIN(pm32_back)
	lretq

	/* the legacy boot: CsmDxe calls this (never returns) */
boot64:
	cli
	lgdt	LIN(gdtr)
	movb	$1, LIN(boot_flag)
	jmp	to_real

	.code32
pm32_back:
	movl	%cr0, %eax
	andl	$0x7FFFFFFF, %eax	/* PG off: leaves IA-32e mode */
	movl	%eax, %cr0
	movl	$0xC0000080, %ecx
	rdmsr
	andl	$~0x100, %eax
	wrmsr
	movl	%cr4, %eax
	andl	$~0x20, %eax
	movl	%eax, %cr4
	movw	$SEL_DATA16, %ax	/* real-mode-like limits in the descriptor caches */
	movw	%ax, %ds
	movw	%ax, %es
	movw	%ax, %ss
	movw	%ax, %fs
	movw	%ax, %gs
	ljmp	$SEL_CODE16, $OFF(pm16_back)

	.code16
pm16_back:
	movl	%cr0, %eax
	andb	$0xFE, %al		/* PE off */
	movl	%eax, %cr0
	ljmp	$0xF000, $OFF(rm_back)
rm_back:
	lidtl	%cs:OFF(rm_idtr)
	cmpb	$0, %cs:OFF(boot_flag)
	jne	boot16
	movw	%cs:OFF(save_ss), %ss
	movw	%cs:OFF(save_sp), %sp
	cmpb	$0, %cs:OFF(retry)
	je	done
	movb	$0, %cs:OFF(retry)
	sti				/* the interrupt window of a waiting service */
	nop
	nop
	cli
	jmp	again
done:
	popal
	popw	%gs
	popw	%fs
	popw	%es
	popw	%ds
	addw	$2, %sp			/* the vector */
	iret

/* the boot sector at 0000:7C00, DL = the boot drive (BIOS Boot
   Specification 1.01, 6.5.1) */
boot16:
	movb	$0, %cs:OFF(boot_flag)
	xorw	%ax, %ax
	movw	%ax, %ds
	movw	%ax, %es
	movw	%ax, %fs
	movw	%ax, %gs
	movw	%ax, %ss
	movw	$0x7C00, %sp
	movb	%cs:OFF(boot_dl), %dl
	xorl	%ebx, %ebx
	xorl	%ecx, %ecx
	xorl	%esi, %esi
	xorl	%edi, %edi
	xorl	%ebp, %ebp
	sti
	ljmp	$0, $0x7C00

/* ---- IRQ0: the 8254 tick (18.2 Hz): BDA 46Ch, midnight at 1800B0h ticks
   (BDA 470h), INT 1Ch, EOI */
irq0:
	pushw	%ds
	pushw	%ax
	xorw	%ax, %ax
	movw	%ax, %ds
	incl	0x46C
	cmpl	$0x1800B0, 0x46C
	jb	1f
	movl	$0, 0x46C
	movb	$1, 0x470
1:	int	$0x1C
	movb	$0x20, %al
	outb	%al, $0x20
	popw	%ax
	popw	%ds
	iret

irq_master:
	pushw	%ax
	movb	$0x20, %al
	outb	%al, $0x20
	popw	%ax
	iret

irq_slave:
	pushw	%ax
	movb	$0x20, %al
	outb	%al, $0xA0
	outb	%al, $0x20
	popw	%ax
	iret

iret_only:
	iret

/* the diskette parameter table (INT 1Eh; IBM PC AT Technical Reference) */
dpt:
	.byte	0xAF, 0x02, 0x25, 0x02, 18, 0x1B, 0xFF, 0x6C, 0xF6, 0x0F, 0x08

/* the system configuration table (INT 15h AH=C0h): length, model FCh
   (AT), submodel 01h, revision, feature bytes (2nd 8259, RTC) */
config_table:
	.word	8
	.byte	0xFC, 0x01, 0x00, 0x70, 0x00, 0x00, 0x00, 0x00

/* a far jump to F000:FFF0 is a reset (CF9h) */
reset16:
	movb	$0x06, %al
	movw	$0xCF9, %dx
	outb	%al, %dx
1:	hlt
	jmp	1b

	.org	0xFFF0
	ljmp	$0xF000, $OFF(reset16)
	.ascii	"11/17/26"		/* the BIOS date at F000:FFF5 */
	.byte	0
	.byte	0xFC			/* the model byte at F000:FFFE: AT */
	.byte	0
