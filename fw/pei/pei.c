/*
 * PEI: the pre-EFI initialization phase (PI 1.8 Vol. 1). It runs in place
 * from flash (layout.h PEI_BASE) on SEC's temporary stack, with no writable
 * data of its own, and:
 *
 *   1. finds the memory the platform reports: QEMU fw_cfg "etc/e820" (vsim
 *      serves it on the test board) or, failing that, the CMOS sizes at
 *      30h/31h and 34h/35h. The firmware doesn't train DRAM (fw/README.md
 *      "Memory"); the legacy hole A_0000h-F_FFFFh is never RAM to UEFI;
 *   2. takes the top of RAM below 4 GiB as the permanent memory of the
 *      hand-off (the PHIT range, PI Vol. 3 5.3), builds the final page
 *      tables and the DXE stack there;
 *   3. loads the DXE core from FV_MAIN (PE32+, relocated), describes the
 *      system in a HOB list (PI Vol. 3 ch. 5) and enters the DXE core on the
 *      new stack with the HOB list as its argument.
 */
#include "fwlib.h"
#include "layout.h"
#include "pe.h"
#include "vibe86fw.h"
#include "x86.h"

#define PEI_MEMORY_MAX	0x00800000ull	/* the hand-off range: up to 8 MiB */
#define DXE_STACK_SIZE	0x00020000ull
#define PT_PML4_ENTRIES	5		/* 5 x 512 GiB identity-mapped */
#define LEGACY_LO	0x000A0000ull
#define LEGACY_HI	0x00100000ull

struct pei {
	uint64_t mem_bottom, mem_top;	/* the PHIT range */
	uint64_t free_bottom, free_top;
	uint64_t hob_end;		/* the next free byte of the HOB list */
	EFI_HOB_HANDOFF_INFO_TABLE *phit;
	VIBE86_PLATFORM_INFO info;
};

static void __attribute__((noreturn))
fatal(const char *msg)
{
	post_code(POST_PEI_FATAL);
	DEBUG("PEI: fatal: %s\n", msg);
	com1_puts("vibe86 firmware: PEI: ");
	com1_puts(msg);
	com1_puts("\n");
	for (;;) {
		cpu_cli();
		cpu_halt();
	}
}

/* ---- memory discovery */

static void
add_ram(struct pei *p, uint64_t base, uint64_t len)
{
	/* page-align inward and cut out the legacy hole */
	uint64_t end = ALIGN_DOWN(base + len, EFI_PAGE_SIZE);
	base = ALIGN_UP(base, EFI_PAGE_SIZE);
	if (end <= base)
		return;
	if (base < LEGACY_HI && end > LEGACY_LO) {
		if (base < LEGACY_LO)
			add_ram(p, base, LEGACY_LO - base);
		if (end > LEGACY_HI)
			add_ram(p, LEGACY_HI, end - LEGACY_HI);
		return;
	}
	if (p->info.nram == PLATFORM_MAX_RAM)
		return;
	p->info.ram[p->info.nram].base = base;
	p->info.ram[p->info.nram].length = end - base;
	p->info.nram++;
	if (base < (1ull << 32))
		p->info.ram_below_4g += MIN(end, 1ull << 32) - base;
	if (end > (1ull << 32))
		p->info.ram_above_4g += end - MAX(base, 1ull << 32);
}

static uint8_t
cmos(uint8_t reg)
{
	outb(RTC_PORT, reg | 0x80);	/* NMI stays masked */
	return inb(RTC_PORT + 1);
}

static void
find_memory(struct pei *p)
{
	uint32_t size;
	uint16_t sel;
	p->info.fwcfg = (uint32_t)fwcfg_present();
	if (p->info.fwcfg && fwcfg_find("etc/e820", &size, &sel) == 0 && size >= 20) {
		/* QEMU's E820 entries: u64 address, u64 length, u32 type (1 = RAM) */
		uint8_t buf[20 * 16];
		uint32_t n = MIN(size, (uint32_t)sizeof buf) / 20;
		fwcfg_read(sel, buf, n * 20);
		for (uint32_t i = 0; i < n; i++) {
			uint64_t a, l;
			uint32_t t;
			memcpy(&a, buf + 20 * i, 8);
			memcpy(&l, buf + 20 * i + 8, 8);
			memcpy(&t, buf + 20 * i + 16, 4);
			DEBUG("PEI: e820 %016lx-%016lx type %u\n", a, a + l - 1, t);
			if (t == 1)
				add_ram(p, a, l);
		}
	}
	if (!p->info.nram) {
		/* CMOS (as QEMU and SeaBIOS use it): 30h/31h KiB above 1 MiB up
		   to 16 MiB... ; 34h/35h 64-KiB units above 16 MiB */
		uint64_t ext = (uint64_t)(cmos(0x30) | cmos(0x31) << 8) * 1024;
		uint64_t hi = (uint64_t)(cmos(0x34) | cmos(0x35) << 8) * 65536;
		DEBUG("PEI: CMOS memory: ext %lu KiB, above 16M %lu KiB\n", ext >> 10, hi >> 10);
		add_ram(p, 0, 640 * 1024);
		if (hi)
			add_ram(p, LEGACY_HI, 16 * 1024 * 1024 - LEGACY_HI + hi);
		else if (ext)
			add_ram(p, LEGACY_HI, ext);
	}
	if (!p->info.nram)
		fatal("no memory reported (fw_cfg etc/e820 or CMOS)");
}

/* ---- the hand-off range and the HOB list */

static void *
alloc_top(struct pei *p, uint64_t size, uint64_t align)
{
	uint64_t a = ALIGN_DOWN(p->free_top - size, align);
	if (a < p->free_bottom)
		fatal("out of permanent memory");
	p->free_top = a;
	p->phit->EfiFreeMemoryTop = a;
	return (void *)(uintptr_t)a;
}

static void *
hob_add(struct pei *p, UINT16 type, UINT16 len)
{
	len = (UINT16)ALIGN_UP(len, 8);
	if (p->hob_end + len + sizeof(EFI_HOB_GENERIC_HEADER) > p->free_top)
		fatal("HOB list overflow");
	EFI_HOB_GENERIC_HEADER *h = (void *)(uintptr_t)p->hob_end;
	memset(h, 0, len);
	h->HobType = type;
	h->HobLength = len;
	p->hob_end += len;
	/* keep an end-of-list HOB after it */
	EFI_HOB_GENERIC_HEADER *e = (void *)(uintptr_t)p->hob_end;
	e->HobType = EFI_HOB_TYPE_END_OF_HOB_LIST;
	e->HobLength = sizeof *e;
	e->Reserved = 0;
	p->phit->EfiEndOfHobList = p->hob_end;
	p->free_bottom = p->hob_end + sizeof *e;
	p->phit->EfiFreeMemoryBottom = p->free_bottom;
	return h;
}

static void
hob_resource(struct pei *p, UINT32 type, UINT64 base, UINT64 len)
{
	EFI_HOB_RESOURCE_DESCRIPTOR *r = hob_add(p, EFI_HOB_TYPE_RESOURCE_DESCRIPTOR, sizeof *r);
	r->ResourceType = type;
	r->ResourceAttribute = EFI_RESOURCE_ATTRIBUTE_PRESENT | EFI_RESOURCE_ATTRIBUTE_INITIALIZED |
	    (type == EFI_RESOURCE_SYSTEM_MEMORY ? EFI_RESOURCE_ATTRIBUTE_TESTED |
	    EFI_RESOURCE_ATTRIBUTE_WRITE_BACK_CACHEABLE : EFI_RESOURCE_ATTRIBUTE_UNCACHEABLE);
	r->PhysicalStart = base;
	r->ResourceLength = len;
}

static void
hob_alloc(struct pei *p, const EFI_GUID *name, UINT64 base, UINT64 len, UINT32 type)
{
	EFI_HOB_MEMORY_ALLOCATION *m = hob_add(p, EFI_HOB_TYPE_MEMORY_ALLOCATION, sizeof *m);
	if (name)
		m->AllocDescriptor.Name = *name;
	m->AllocDescriptor.MemoryBaseAddress = base;
	m->AllocDescriptor.MemoryLength = len;
	m->AllocDescriptor.MemoryType = type;
}

static void
setup_handoff(struct pei *p)
{
	/* the topmost RAM range below 4 GiB */
	uint64_t best_base = 0, best_end = 0;
	for (uint32_t i = 0; i < p->info.nram; i++) {
		uint64_t b = p->info.ram[i].base, e = MIN(b + p->info.ram[i].length, 1ull << 32);
		if (b < e && e > best_end && b >= LEGACY_HI) {
			best_base = b;
			best_end = e;
		}
	}
	if (best_end - best_base < 2 * PEI_MEMORY_MAX)
		fatal("not enough memory below 4 GiB (the test board needs vsim -M 32 or more)");
	p->mem_top = best_end;
	p->mem_bottom = best_end - PEI_MEMORY_MAX;
	p->free_top = p->mem_top;
	/* the PHIT HOB starts the list at the bottom of the range */
	p->phit = (void *)(uintptr_t)p->mem_bottom;
	memset(p->phit, 0, sizeof *p->phit);
	p->phit->Header.HobType = EFI_HOB_TYPE_HANDOFF;
	p->phit->Header.HobLength = sizeof *p->phit;
	p->phit->Version = EFI_HOB_HANDOFF_TABLE_VERSION;
	p->phit->BootMode = BOOT_WITH_FULL_CONFIGURATION;
	p->phit->EfiMemoryTop = p->mem_top;
	p->phit->EfiMemoryBottom = p->mem_bottom;
	p->phit->EfiFreeMemoryTop = p->free_top;
	p->hob_end = p->mem_bottom + sizeof *p->phit;
	p->free_bottom = p->hob_end;
	EFI_HOB_GENERIC_HEADER *e = (void *)(uintptr_t)p->hob_end;
	e->HobType = EFI_HOB_TYPE_END_OF_HOB_LIST;
	e->HobLength = sizeof *e;
	p->phit->EfiEndOfHobList = p->hob_end;
	DEBUG("PEI: hand-off memory %08lx-%08lx\n", p->mem_bottom, p->mem_top - 1);
}

/* ---- paging: identity map with 1-GiB pages (Intel SDM Vol. 3A 5.5.4,
   Table 5-16; CPUID.80000001h:EDX.Page1GB), A and D preset so the walker
   needn't write them */

static uint64_t
build_page_tables(struct pei *p)
{
	uint32_t r[4];
	cpuid(0x80000001, 0, r);
	if (!(r[3] & (1u << 26)))
		fatal("the CPU has no 1-GiB pages");
	uint64_t *pml4 = alloc_top(p, (1 + PT_PML4_ENTRIES) * EFI_PAGE_SIZE, EFI_PAGE_SIZE);
	memset(pml4, 0, (1 + PT_PML4_ENTRIES) * EFI_PAGE_SIZE);
	for (unsigned i = 0; i < PT_PML4_ENTRIES; i++) {
		uint64_t *pdpt = pml4 + 512 * (i + 1);
		pml4[i] = (uint64_t)(uintptr_t)pdpt | 0x23;		/* P, RW, A */
		for (unsigned j = 0; j < 512; j++)
			pdpt[j] = ((uint64_t)i << 39 | (uint64_t)j << 30) | 0xE3;	/* P RW A D PS */
	}
	hob_alloc(p, 0, (uint64_t)(uintptr_t)pml4, (1 + PT_PML4_ENTRIES) * EFI_PAGE_SIZE,
	    EfiBootServicesData);
	return (uint64_t)(uintptr_t)pml4;
}

/* ---- the DXE core */

static uint64_t
load_dxe_core(struct pei *p, uint64_t *image_base, uint64_t *image_size)
{
	static const EFI_GUID core_guid = FILE_DXE_CORE_GUID;
	static const EFI_GUID module_guid = EFI_HOB_MEMORY_ALLOC_MODULE_GUID;
	const EFI_FIRMWARE_VOLUME_HEADER *fv = fv_check(FV_MAIN_BASE);
	if (!fv)
		fatal("FV_MAIN is missing or corrupt");
	uint64_t cookie = 0;
	const EFI_FFS_FILE_HEADER *f;
	while ((f = fv_next_file(fv, &cookie)) && f->Type != EFI_FV_FILETYPE_DXE_CORE)
		;
	if (!f || !guid_eq(&f->Name, &core_guid))
		fatal("no DXE core in FV_MAIN");
	UINT32 size;
	const void *pe = ffs_section(f, EFI_SECTION_PE32, &size);
	const IMAGE_NT_HEADERS64 *nt = pe ? pe_headers(pe, size) : 0;
	if (!nt)
		fatal("the DXE core isn't a PE32+ image");
	uint64_t isize = ALIGN_UP(nt->OptionalHeader.SizeOfImage, EFI_PAGE_SIZE);
	void *dest = alloc_top(p, isize, EFI_PAGE_SIZE);
	uint64_t entry;
	if (pe_load(pe, size, dest, &entry))
		fatal("the DXE core couldn't be relocated");
	EFI_HOB_MEMORY_ALLOCATION_MODULE *m = hob_add(p, EFI_HOB_TYPE_MEMORY_ALLOCATION, sizeof *m);
	m->MemoryAllocationHeader.Name = module_guid;
	m->MemoryAllocationHeader.MemoryBaseAddress = (uint64_t)(uintptr_t)dest;
	m->MemoryAllocationHeader.MemoryLength = isize;
	m->MemoryAllocationHeader.MemoryType = EfiBootServicesCode;
	m->ModuleName = core_guid;
	m->EntryPoint = entry;
	*image_base = (uint64_t)(uintptr_t)dest;
	*image_size = isize;
	DEBUG("PEI: DXE core at %p, entry %lx\n", dest, entry);
	return entry;
}

static void
cpu_info(struct pei *p)
{
	uint32_t r[4];
	char vendor[13];
	cpuid(0, 0, r);
	memcpy(vendor, &r[1], 4);
	memcpy(vendor + 4, &r[3], 4);
	memcpy(vendor + 8, &r[2], 4);
	vendor[12] = 0;
	cpuid(0x80000000, 0, r);
	if (r[0] >= 0x80000004) {
		for (unsigned i = 0; i < 3; i++) {
			cpuid(0x80000002 + i, 0, r);
			memcpy(p->info.cpu_brand + 16 * i, r, 16);
		}
		p->info.cpu_brand[48] = 0;
	} else
		memcpy(p->info.cpu_brand, vendor, 13);
	DEBUG("PEI: CPU %s \"%s\"\n", vendor, p->info.cpu_brand);
}

void
pei_main(uint64_t temp_base, uint64_t temp_size, uint64_t tsc_at_reset)
{
	struct pei p;
	static const EFI_GUID platform_guid = VIBE86_PLATFORM_HOB_GUID;
	static const EFI_GUID stack_guid = EFI_HOB_MEMORY_ALLOC_STACK_GUID;

	post_code(POST_PEI_START);
	memset(&p, 0, sizeof p);
	p.info.tsc_at_reset = tsc_at_reset;
	com1_init();
	DEBUG("\nvibe86 firmware %s: PEI (temporary RAM %lx+%lx)\n", FW_VERSION_STR, temp_base,
	    temp_size);
	cpu_info(&p);

	post_code(POST_PEI_MEMORY);
	find_memory(&p);
	setup_handoff(&p);

	EFI_HOB_CPU *cpu = hob_add(&p, EFI_HOB_TYPE_CPU, sizeof *cpu);
	cpu->SizeOfMemorySpace = 52;		/* rtl/common/cpuid.toml phys_addr_bits */
	cpu->SizeOfIoSpace = 16;
	for (uint32_t i = 0; i < p.info.nram; i++)
		hob_resource(&p, EFI_RESOURCE_SYSTEM_MEMORY, p.info.ram[i].base, p.info.ram[i].length);
	hob_resource(&p, EFI_RESOURCE_FIRMWARE_DEVICE, FLASH_BASE, FLASH_SIZE);
	hob_resource(&p, EFI_RESOURCE_MEMORY_MAPPED_IO, ECAM_BASE, PLAT_MAP_ECAM_SIZE);
	hob_resource(&p, EFI_RESOURCE_MEMORY_MAPPED_IO, PCI_MEM32_BASE, PCI_MEM32_SIZE);
	hob_resource(&p, EFI_RESOURCE_MEMORY_MAPPED_IO, VGA_VRAM, VGA_VRAM_SIZE);
	hob_resource(&p, EFI_RESOURCE_MEMORY_MAPPED_IO, MMIO_BASE, 0x00100000);
	hob_resource(&p, EFI_RESOURCE_MEMORY_MAPPED_IO, PCI_MEM64_BASE, PCI_MEM64_SIZE);
	EFI_HOB_FIRMWARE_VOLUME *fvh = hob_add(&p, EFI_HOB_TYPE_FV, sizeof *fvh);
	fvh->BaseAddress = FV_MAIN_BASE;
	fvh->Length = FV_MAIN_SIZE;

	uint64_t cr3 = build_page_tables(&p);
	void *stack = alloc_top(&p, DXE_STACK_SIZE, EFI_PAGE_SIZE);
	hob_alloc(&p, &stack_guid, (uint64_t)(uintptr_t)stack, DXE_STACK_SIZE, EfiBootServicesData);

	post_code(POST_PEI_DXE_LOAD);
	uint64_t core_base, core_size;
	uint64_t entry = load_dxe_core(&p, &core_base, &core_size);

	/* the platform HOB last: it records the time of the hand-off */
	EFI_HOB_GUID_TYPE *g = hob_add(&p, EFI_HOB_TYPE_GUID_EXTENSION,
	    sizeof *g + sizeof(VIBE86_PLATFORM_INFO));
	g->Name = platform_guid;
	p.info.tsc_at_dxe = rdtsc();
	memcpy(g + 1, &p.info, sizeof p.info);

	DEBUG("PEI: %lu MiB below 4 GiB, %lu MiB above; entering DXE\n", p.info.ram_below_4g >> 20,
	    p.info.ram_above_4g >> 20);
	uint64_t sp = (uint64_t)(uintptr_t)stack + DXE_STACK_SIZE - 32;
	__asm__ volatile(
	    "mov %0, %%cr3\n\t"
	    "mov %1, %%rsp\n\t"
	    "call *%2\n\t"
	    "1: cli; hlt; jmp 1b"
	    :: "r"(cr3), "r"(sp), "r"(entry), "c"(p.phit) : "memory");
	__builtin_unreachable();
}
