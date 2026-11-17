// apic.inc: firmware's part in delivering the 8259's interrupts once the
// processor has a local APIC (D-017): virtual-wire mode (MultiProcessor
// Specification 1.4, 3.6.2.2) -- the APIC software-enabled, LINT0 in ExtINT
// mode, LINT1 NMI. At reset the APIC is software-disabled with every LVT
// entry masked (Intel SDM Vol. 3A 12.4.7.1), so until this runs the 8259
// reaches no one. Through the x2APIC MSRs, which work in any mode (real
// mode can't address FEE0_0000h); clobbers EAX, ECX, EDX.
	.macro	VIRTUAL_WIRE
	mov	$0x1b, %ecx			// IA32_APIC_BASE: EN | EXTD (x2APIC)
	rdmsr
	or	$0xc00, %eax
	wrmsr
	mov	$0x80f, %ecx			// SVR: enabled, spurious vector FFh
	mov	$0x1ff, %eax
	xor	%edx, %edx
	wrmsr
	mov	$0x835, %ecx			// LVT LINT0: ExtINT
	mov	$0x700, %eax
	wrmsr
	mov	$0x836, %ecx			// LVT LINT1: NMI
	mov	$0x400, %eax
	wrmsr
	.endm
