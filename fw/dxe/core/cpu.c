/*
 * DXE core: the CPU's descriptor tables and exception reporting. The GDT
 * (same selectors as SEC: layout.h) lives in runtime-services memory since
 * code that runs after ExitBootServices, such as CSMWrap's 16-bit thunk,
 * returns to long mode through it. Exceptions print the registers and the
 * image the faulting RIP is in, on the debug console and COM1, and stop.
 */
#include "dxe.h"
#include "layout.h"

struct frame {
	uint64_t r15, r14, r13, r12, r11, r10, r9, r8, rbp, rdi, rsi, rdx, rcx, rbx, rax;
	uint64_t vector, error, rip, cs, rflags, rsp, ss;
};

struct __attribute__((packed)) dtr {
	uint16_t limit;
	uint64_t base;
};

struct idt_gate {
	uint16_t off0, sel;
	uint8_t ist, attr;
	uint16_t off1;
	uint32_t off2, zero;
};

extern uint64_t exc_stubs[32];
void irq_ignore(void);
void cpu_load_gdt(struct dtr *d);

static struct idt_gate idt[256];
static void (*crash_hook)(void);

static const char *const exc_names[32] = {
	"#DE divide error", "#DB debug", "NMI", "#BP breakpoint", "#OF overflow", "#BR bound",
	"#UD invalid opcode", "#NM device not available", "#DF double fault", "coprocessor overrun",
	"#TS invalid TSS", "#NP segment not present", "#SS stack fault", "#GP general protection",
	"#PF page fault", "15", "#MF x87 error", "#AC alignment check", "#MC machine check",
	"#XM SIMD exception", "#VE virtualization", "#CP control protection",
};

void
cpu_set_crash_hook(void (*hook)(void))
{
	crash_hook = hook;
}

static void
both(const char *fmt, ...)
{
	char buf[160];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	DEBUG("%s", buf);
	com1_puts(buf);
}

void
cpu_exception(struct frame *f)
{
	uint64_t cr2, cr3, base = 0;
	__asm__ volatile("mov %%cr2, %0; mov %%cr3, %1" : "=r"(cr2), "=r"(cr3));
	post_code(POST_DXE_EXCEPTION);
	const char *img = image_name_of(f->rip, &base);
	both("\n!!!! X64 exception %lu: %s (error %lx) !!!!\n", f->vector,
	    f->vector < 32 && exc_names[f->vector] ? exc_names[f->vector] : "reserved", f->error);
	both("RIP %016lx", f->rip);
	if (img)
		both(" (%s + %lx)", img, f->rip - base);
	both("  CS %lx  RFLAGS %lx\n", f->cs, f->rflags);
	both("RAX %016lx RBX %016lx RCX %016lx\n", f->rax, f->rbx, f->rcx);
	both("RDX %016lx RSI %016lx RDI %016lx\n", f->rdx, f->rsi, f->rdi);
	both("RBP %016lx RSP %016lx R8  %016lx\n", f->rbp, f->rsp, f->r8);
	both("R9  %016lx R10 %016lx R11 %016lx\n", f->r9, f->r10, f->r11);
	both("R12 %016lx R13 %016lx R14 %016lx\n", f->r12, f->r13, f->r14);
	both("R15 %016lx CR2 %016lx CR3 %016lx\n", f->r15, cr2, cr3);
	if (crash_hook)
		crash_hook();
	for (;;) {
		cpu_cli();
		cpu_halt();
	}
}

void
cpu_init(void)
{
	static const uint64_t gdt_init[] = {
		0,
		0x00cf9b000000ffffull,	/* 08: 32-bit code */
		0x00cf93000000ffffull,	/* 10: data */
		0x00af9b000000ffffull,	/* 18: 64-bit code */
		0x008f9b000000ffffull,	/* 20: 16-bit code (for thunks) */
		0x008f93000000ffffull,	/* 28: 16-bit data */
	};
	uint64_t *gdt = core_alloc_rt(sizeof gdt_init);
	memcpy(gdt, gdt_init, sizeof gdt_init);
	struct dtr g = { sizeof gdt_init - 1, (uint64_t)(uintptr_t)gdt };
	cpu_load_gdt(&g);

	for (unsigned v = 0; v < 256; v++) {
		uint64_t h = v < 32 ? exc_stubs[v] : (uint64_t)(uintptr_t)irq_ignore;
		idt[v] = (struct idt_gate){ (uint16_t)h, SEL_CODE64, 0, 0x8E, (uint16_t)(h >> 16),
		    (uint32_t)(h >> 32), 0 };
	}
	struct dtr i = { sizeof idt - 1, (uint64_t)(uintptr_t)idt };
	__asm__ volatile("lidt %0" :: "m"(i));
	/*
	 * The local APIC in virtual-wire mode (MP Specification 1.4 3.6.2.2;
	 * Intel SDM Vol. 3A 12.5.1): software-enabled (SVR bit 8, spurious
	 * vector FFh), LINT0 ExtINT so the 8259's INTR reaches the core, LINT1
	 * NMI. The APIC leaves reset enabled with its LVTs masked, which would
	 * keep legacy interrupts (the CSM's timer) out.
	 */
	uint32_t id[4];
	cpuid(1, 0, id);
	uint64_t apic = rdmsr(MSR_IA32_APIC_BASE_ADDR);
	if ((id[3] & (1u << 9)) && (apic & (1u << 11)) && !(apic & (1u << 10))) {
		uint64_t a = apic & ~0xFFFull;
		mmio_w32(a + 0xF0, 0x1FF);	/* SVR */
		mmio_w32(a + 0x350, 0x700);	/* LVT LINT0: ExtINT */
		mmio_w32(a + 0x360, 0x400);	/* LVT LINT1: NMI */
		DEBUG("DXE: local APIC at %lx, virtual-wire mode\n", a);
	}
	/* CR0.NE, WP; CR4: OSFXSR stays clear (no SSE on v1) */
	uint64_t cr0;
	__asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
	cr0 |= (1u << 5) | (1u << 16);
	__asm__ volatile("mov %0, %%cr0" :: "r"(cr0));
}
