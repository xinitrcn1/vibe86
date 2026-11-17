// pc: the PC-compatible platform SeaBIOS relies on (sim/pc.c, board.c).
//   A PCI-to-PCI root port's memory, I/O and prefetchable windows set to
//     base 0 (SeaBIOS does this while sizing bridges) shadow nothing: RAM,
//     legacy ports (POST, CMOS, COM1) and fw_cfg still answer -- PCI is
//     decoded last;
//   PAM (i440FX 59h): F0000-FFFFF read from flash and writes dropped (0),
//     write-only RAM (2), read/write RAM (3), read-only RAM (1);
//   fw_cfg: signature, ID (traditional + DMA), the file directory naming
//     etc/e820, a DMA read of the signature and DMA's own signature;
//   CMOS: memory sizes for the default 16 GiB DIMM with DRAM below TOLM
//     (2 GiB) and the rest above 4 GiB (D-014): base 640 KiB, extended
//     capped at FFFFh KiB, 16 MiB..TOLM and above 4 GiB in 64 KiB units;
//   8259 + 8254: a one-shot (mode 0) counter is one rising edge on IRQ 0:
//     delivered (INTA through intr_ack) exactly once.
// vsim-args: -b test -c 20000000
// expect-out: COM1 after the windows
// expect-out: PC OK
#include "boot64.inc"
#include "expect.inc"
	.set	BUF, 0x40000
	.set	DMA, 0x41000
	.set	NIRQ, 0x700

	// PCI configuration (CF8h/CFCh) bus 0: CFGW dev, fn, reg, size (b/w/l), value
	.macro	CFGW dev, fn, reg, sfx, val
	mov	$0x80000000 | (\dev << 11) | (\fn << 8) | (\reg & 0xfc), %eax
	mov	$0xcf8, %dx
	out	%eax, %dx
	mov	$0xcfc + (\reg & 3), %dx
	mov	$\val, %eax
	.ifc	\sfx, b
	out	%al, %dx
	.endif
	.ifc	\sfx, w
	out	%ax, %dx
	.endif
	.ifc	\sfx, l
	out	%eax, %dx
	.endif
	.endm
	.macro	CFGR dev, fn, reg
	mov	$0x80000000 | (\dev << 11) | (\fn << 8) | (\reg & 0xfc), %eax
	mov	$0xcf8, %dx
	out	%eax, %dx
	mov	$0xcfc, %dx
	in	%dx, %eax
	.endm
	.macro	CMOS reg
	mov	$\reg, %al
	out	%al, $0x70
	in	$0x71, %al
	movzbq	%al, %rax
	.endm
	.macro	FWSEL key
	mov	$0x510, %dx
	mov	$\key, %ax
	out	%ax, %dx
	mov	$0x511, %dx
	.endm

main64:
	call	expect_init

	// ---- root port 00:10.0: every window at base 0, decoding enabled
	CFGR	0x10, 0, 0x00
	cmp	$0xffffffff, %eax
	je	fail				// the root port must exist
	CFGW	0x10, 0, 0x1c, w, 0xf000	// I/O base 0, limit FFFFh
	CFGW	0x10, 0, 0x20, l, 0xfff00000	// memory base 0, limit FFFFFh+
	CFGW	0x10, 0, 0x24, l, 0xfff00001	// prefetchable, 64-bit
	CFGW	0x10, 0, 0x28, l, 0		// upper base
	CFGW	0x10, 0, 0x2c, l, 0		// upper limit
	CFGW	0x10, 0, 0x04, w, 0x0007	// I/O, memory, bus master
	movq	$0x1234, BUF
	EXPECT	BUF, 0x1234, 0x01
	mov	$0x5a, %al
	out	%al, $0x80
	in	$0x80, %al
	movzbq	%al, %rax
	EXPECT	%rax, 0x5a, 0x02
	lea	msg_com(%rip), %rsi
	call	puts

	// ---- CMOS memory sizes (one 16 GiB DIMM: 2 GiB below TOLM, 14 GiB above 4 GiB)
	CMOS	0x15
	EXPECT	%rax, 0x80, 0x10
	CMOS	0x16
	EXPECT	%rax, 0x02, 0x11
	CMOS	0x30
	EXPECT	%rax, 0xff, 0x12
	CMOS	0x31
	EXPECT	%rax, 0xff, 0x13
	CMOS	0x34
	EXPECT	%rax, 0x00, 0x14
	CMOS	0x35
	EXPECT	%rax, 0x7f, 0x15		// (2 GiB - 16 MiB) / 64 KiB = 7F00h
	CMOS	0x5b
	EXPECT	%rax, 0x00, 0x16
	CMOS	0x5c
	EXPECT	%rax, 0x80, 0x17
	CMOS	0x5d
	EXPECT	%rax, 0x03, 0x18		// (16 - 2) GiB / 64 KiB = 3_8000h

	// windows off again
	CFGW	0x10, 0, 0x04, w, 0
	CFGW	0x10, 0, 0x20, l, 0x0000fff0
	CFGW	0x10, 0, 0x1c, w, 0x00f0
	CFGW	0x10, 0, 0x24, l, 0x0001fff1

	// ---- PAM: F0000-FFFFF (59h bits 5:4)
	movzbq	0xf0000, %rbx			// PAM 0: the flash alias
	movabsq	$BASE, %rax			// the image's first byte, at F0000 too
	movzbq	(%rax), %rcx
	cmp	%rcx, %rbx
	jne	fail
	movb	$0x77, 0xf0000			// dropped
	movzbq	0xf0000, %rax
	cmp	%rcx, %rax
	jne	fail
	CFGW	0, 0, 0x59, b, 0x20		// write-only RAM
	movb	$0x77, 0xf0000
	movzbq	0xf0000, %rax
	cmp	%rcx, %rax			// still reads the flash
	jne	fail
	CFGW	0, 0, 0x59, b, 0x30		// read/write RAM
	movzbq	0xf0000, %rax
	EXPECT	%rax, 0x77, 0x20		// the write-only write landed
	movb	$0x66, 0xf0000
	movzbq	0xf0000, %rax
	EXPECT	%rax, 0x66, 0x21
	CFGW	0, 0, 0x59, b, 0x10		// read-only RAM
	movb	$0x55, 0xf0000			// dropped
	movzbq	0xf0000, %rax
	EXPECT	%rax, 0x66, 0x22
	CFGW	0, 0, 0x59, b, 0x00

	// ---- fw_cfg
	FWSEL	1				// ID
	in	%dx, %al
	movzbq	%al, %rax
	EXPECT	%rax, 3, 0x30			// traditional + DMA
	mov	$0x514, %dx			// DMA register: "QEMU CFG"
	in	%dx, %eax
	EXPECT	%rax, 0x554d4551, 0x31
	FWSEL	0x19				// the file directory
	mov	$BUF, %edi
	mov	$4, %ecx
	rep insb
	mov	BUF, %eax
	bswap	%eax				// big-endian count
	test	%eax, %eax
	jz	fail
	mov	%eax, %r12d
1:	mov	$BUF, %edi			// 64-byte entries: size, select, reserved, name
	mov	$64, %ecx
	rep insb
	movabsq	$0x303238652f637465, %rax	// "etc/e820" (as a little-endian qword)
	cmp	%rax, BUF + 8
	je	2f
	dec	%r12d
	jnz	1b
	jmp	fail
2:	mov	BUF, %eax
	bswap	%eax
	test	%eax, %eax			// its size: entries of 20 bytes
	jz	fail
	// DMA: select 0 and read the 4-byte signature
	movq	$0, BUF
	movl	$0x0a000000, DMA		// control: select (key 0) | read, big-endian
	movl	$0x04000000, DMA + 4		// length 4
	movl	$0, DMA + 8			// address, high
	mov	$BUF, %eax
	bswap	%eax
	mov	%eax, DMA + 12			// low
	mov	$0x514, %dx
	xor	%eax, %eax
	out	%eax, %dx			// address high (big-endian)
	mov	$DMA, %eax
	bswap	%eax
	mov	$0x518, %dx
	out	%eax, %dx			// address low: starts it
	// the device clears the control field when the transfer is done (QEMU
	// docs/specs/fw_cfg.rst; the DMA runs over the I/O link, D-014)
	mov	$1000000, %ecx
1:	mov	DMA, %eax
	test	%eax, %eax
	jz	2f
	dec	%ecx
	jnz	1b
2:	EXPECT	%rax, 0, 0x31			// the control field cleared: done, no error
	EXPECT	BUF, 0x554d4551, 0x32
	movl	DMA, %eax
	EXPECT	%rax, 0, 0x33			// control cleared: done, no error

	// ---- PIC + PIT: one edge, one interrupt
	lea	h_irq0(%rip), %rax
	mov	$0x20, %ecx
	mov	$0x8e, %edx
	call	set_gate
	movq	$0, NIRQ
	mov	$0x11, %al			// ICW1-4: vectors 20h-27h, IRQ 0 only
	out	%al, $0x20
	mov	$0x20, %al
	out	%al, $0x21
	mov	$0x04, %al
	out	%al, $0x21
	mov	$0x01, %al
	out	%al, $0x21
	mov	$0xfe, %al
	out	%al, $0x21
	mov	$0x30, %al			// channel 0, lo/hi, mode 0 (one shot)
	out	%al, $0x43
	mov	$0x00, %al
	out	%al, $0x40
	mov	$0x01, %al			// 256 ticks
	out	%al, $0x40
	sti
	mov	$100000, %ecx
1:	pause
	dec	%ecx
	jnz	1b
	cli
	EXPECT	NIRQ, 1, 0x40

	lea	endmsg(%rip), %rsi
	call	puts
	jmp	pass

h_irq0:
	push	%rax
	incq	NIRQ
	mov	$0x20, %al			// EOI
	out	%al, $0x20
	pop	%rax
	iretq

msg_com: .asciz	"COM1 after the windows\n"
endmsg:	.asciz	"PC OK\n"
	BOOT64_END
