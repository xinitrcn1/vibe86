/*
 * DXE core: memory services (UEFI 2.10 7.2). The memory map is a sorted
 * array of ranges with a UEFI memory type; AllocatePages takes the highest
 * free range below 4 GiB (top-down, as UEFI implementations do, so low
 * memory stays free for legacy users), and pool allocations come from
 * per-type pages in power-of-two size classes.
 */
#include "dxe.h"

#define MAX_RANGES	512

struct range {
	uint64_t start, pages;
	uint32_t type;
	uint64_t attr;
};

static struct range map[MAX_RANGES];
static unsigned nmap;
static UINTN map_key = 1;

static const char *const type_names[] = {
	"Reserved", "LoaderCode", "LoaderData", "BS_Code", "BS_Data", "RT_Code", "RT_Data",
	"Available", "Unusable", "ACPI_Recl", "ACPI_NVS", "MMIO", "MMIO_Port", "PalCode",
	"Persistent", "Unaccepted",
};

static void
map_insert(unsigned i, uint64_t start, uint64_t pages, uint32_t type, uint64_t attr)
{
	if (nmap == MAX_RANGES) {
		DEBUG("DXE: memory map full\n");
		return;
	}
	memmove(&map[i + 1], &map[i], (nmap - i) * sizeof map[0]);
	map[i] = (struct range){ start, pages, type, attr };
	nmap++;
}

static void
map_merge(void)
{
	for (unsigned i = 0; i + 1 < nmap;) {
		struct range *a = &map[i], *b = &map[i + 1];
		if (a->type == b->type && a->attr == b->attr && a->start + a->pages * EFI_PAGE_SIZE == b->start) {
			a->pages += b->pages;
			memmove(b, b + 1, (nmap - i - 2) * sizeof map[0]);
			nmap--;
		} else
			i++;
	}
}

/* add a range that isn't in the map yet */
static void
map_add(uint64_t start, uint64_t pages, uint32_t type, uint64_t attr)
{
	unsigned i = 0;
	while (i < nmap && map[i].start < start)
		i++;
	map_insert(i, start, pages, type, attr);
	map_merge();
	map_key++;
}

/*
 * Change the type of [start, start + pages) which must lie entirely in
 * ranges of type `from` (or any type if from < 0). Returns 0, or -1.
 */
static int
map_set(uint64_t start, uint64_t pages, int from, uint32_t to)
{
	uint64_t end = start + pages * EFI_PAGE_SIZE;
	/* check coverage */
	uint64_t a = start;
	for (unsigned i = 0; i < nmap && a < end; i++) {
		uint64_t s = map[i].start, e = s + map[i].pages * EFI_PAGE_SIZE;
		if (e <= a)
			continue;
		if (s > a || (from >= 0 && map[i].type != (uint32_t)from))
			return -1;
		a = e;
	}
	if (a < end)
		return -1;
	/* split the ranges at start and end, then retype */
	for (unsigned i = 0; i < nmap; i++) {
		uint64_t s = map[i].start, e = s + map[i].pages * EFI_PAGE_SIZE;
		if (s < start && e > start) {
			map[i].pages = (start - s) / EFI_PAGE_SIZE;
			map_insert(i + 1, start, (e - start) / EFI_PAGE_SIZE, map[i].type, map[i].attr);
			continue;
		}
		if (s < end && e > end) {
			map[i].pages = (end - s) / EFI_PAGE_SIZE;
			map_insert(i + 1, end, (e - end) / EFI_PAGE_SIZE, map[i].type, map[i].attr);
		}
	}
	for (unsigned i = 0; i < nmap; i++)
		if (map[i].start >= start && map[i].start < end)
			map[i].type = to;
	map_merge();
	map_key++;
	return 0;
}

static int
valid_alloc_type(EFI_MEMORY_TYPE t)
{
	return (t > EfiReservedMemoryType && t < EfiConventionalMemory) ||
	    t == EfiACPIReclaimMemory || t == EfiACPIMemoryNVS || t == EfiReservedMemoryType ||
	    (uint32_t)t >= 0x80000000u;
}

EFI_STATUS EFIAPI
core_allocate_pages(EFI_ALLOCATE_TYPE Type, EFI_MEMORY_TYPE MemoryType, UINTN Pages,
    EFI_PHYSICAL_ADDRESS *Memory)
{
	if (!Memory || Type >= MaxAllocateType || !valid_alloc_type(MemoryType) || gBootServicesDone)
		return EFI_INVALID_PARAMETER;
	if (!Pages)
		return EFI_INVALID_PARAMETER;
	if (Type == AllocateAddress) {
		if (*Memory & (EFI_PAGE_SIZE - 1))
			return EFI_INVALID_PARAMETER;
		return map_set(*Memory, Pages, EfiConventionalMemory, MemoryType) ? EFI_NOT_FOUND : EFI_SUCCESS;
	}
	uint64_t limit = Type == AllocateMaxAddress ? *Memory + 1 : (1ull << 32);
	uint64_t size = Pages * EFI_PAGE_SIZE;
	for (int pass = 0; pass < 2; pass++) {
		/* top-down: below 4 GiB first; AnyPages may then go above */
		for (int i = (int)nmap - 1; i >= 0; i--) {
			if (map[i].type != EfiConventionalMemory)
				continue;
			uint64_t s = map[i].start, e = s + map[i].pages * EFI_PAGE_SIZE;
			if (e > limit)
				e = ALIGN_DOWN(limit, EFI_PAGE_SIZE);
			if (e < s + size)
				continue;
			uint64_t a = e - size;
			if (map_set(a, Pages, EfiConventionalMemory, MemoryType) == 0) {
				*Memory = a;
				return EFI_SUCCESS;
			}
		}
		if (Type != AllocateAnyPages)
			break;
		limit = ~0ull;
	}
	return EFI_OUT_OF_RESOURCES;
}

EFI_STATUS EFIAPI
core_free_pages(EFI_PHYSICAL_ADDRESS Memory, UINTN Pages)
{
	if (Memory & (EFI_PAGE_SIZE - 1) || !Pages)
		return EFI_INVALID_PARAMETER;
	/* every page must be allocated (not free, not MMIO) */
	uint64_t end = Memory + Pages * EFI_PAGE_SIZE;
	for (unsigned i = 0; i < nmap; i++) {
		uint64_t s = map[i].start, e = s + map[i].pages * EFI_PAGE_SIZE;
		if (e > Memory && s < end && (map[i].type == EfiConventionalMemory ||
		    map[i].type == EfiMemoryMappedIO))
			return EFI_NOT_FOUND;
	}
	return map_set(Memory, Pages, -1, EfiConventionalMemory) ? EFI_NOT_FOUND : EFI_SUCCESS;
}

EFI_STATUS EFIAPI
core_get_memory_map(UINTN *MemoryMapSize, EFI_MEMORY_DESCRIPTOR *MemoryMap, UINTN *MapKey,
    UINTN *DescriptorSize, UINT32 *DescriptorVersion)
{
	if (!MemoryMapSize)
		return EFI_INVALID_PARAMETER;
	UINTN need = nmap * sizeof(EFI_MEMORY_DESCRIPTOR);
	if (DescriptorSize)
		*DescriptorSize = sizeof(EFI_MEMORY_DESCRIPTOR);
	if (DescriptorVersion)
		*DescriptorVersion = EFI_MEMORY_DESCRIPTOR_VERSION;
	if (*MemoryMapSize < need) {
		*MemoryMapSize = need;
		return EFI_BUFFER_TOO_SMALL;
	}
	if (!MemoryMap)
		return EFI_INVALID_PARAMETER;
	for (unsigned i = 0; i < nmap; i++) {
		EFI_MEMORY_DESCRIPTOR *d = &MemoryMap[i];
		d->Type = map[i].type;
		d->PhysicalStart = map[i].start;
		d->VirtualStart = 0;
		d->NumberOfPages = map[i].pages;
		d->Attribute = map[i].attr;
		if (d->Type == EfiRuntimeServicesCode || d->Type == EfiRuntimeServicesData)
			d->Attribute |= EFI_MEMORY_RUNTIME;
	}
	*MemoryMapSize = need;
	if (MapKey)
		*MapKey = map_key;
	return EFI_SUCCESS;
}

EFI_STATUS
mem_check_key(UINTN key)
{
	return key == map_key ? EFI_SUCCESS : EFI_INVALID_PARAMETER;
}

void
mem_dump(void)
{
	for (unsigned i = 0; i < nmap; i++)
		DEBUG("  %-10s %016lx-%016lx %8lx pages\n",
		    map[i].type < ARRAY_SIZE(type_names) ? type_names[map[i].type] : "OEM",
		    map[i].start, map[i].start + map[i].pages * EFI_PAGE_SIZE - 1, map[i].pages);
}

/* ---- pool: classes of 32 << k bytes (k = 0..7, up to 4 KiB - header) from
   pages of the pool's type; larger blocks get pages of their own */

#define POOL_MAGIC	0x4C4F4F50u	/* "POOL" */
#define POOL_CLASSES	7		/* 32 .. 2048 */
#define POOL_TYPES	(EfiMaxMemoryType)

struct pool_hdr {
	uint32_t magic;
	uint32_t type;
	uint64_t size;		/* class index (< POOL_CLASSES) or pages for big blocks | 1<<63 */
};

struct pool_free {
	struct pool_free *next;
};

static struct pool_free *free_lists[POOL_TYPES][POOL_CLASSES];

EFI_STATUS EFIAPI
core_allocate_pool(EFI_MEMORY_TYPE PoolType, UINTN Size, VOID **Buffer)
{
	if (!Buffer || !valid_alloc_type(PoolType) || (uint32_t)PoolType >= POOL_TYPES)
		return EFI_INVALID_PARAMETER;
	if (gBootServicesDone)
		return EFI_UNSUPPORTED;
	UINTN need = Size + sizeof(struct pool_hdr);
	unsigned k = 0;
	while (k < POOL_CLASSES && (32u << k) < need)
		k++;
	if (k == POOL_CLASSES) {
		EFI_PHYSICAL_ADDRESS a;
		UINTN pages = EFI_SIZE_TO_PAGES(need);
		EFI_STATUS s = core_allocate_pages(AllocateAnyPages, PoolType, pages, &a);
		if (EFI_ERROR(s))
			return EFI_OUT_OF_RESOURCES;
		struct pool_hdr *h = (void *)(uintptr_t)a;
		*h = (struct pool_hdr){ POOL_MAGIC, PoolType, pages | 1ull << 63 };
		*Buffer = h + 1;
		return EFI_SUCCESS;
	}
	struct pool_free **fl = &free_lists[PoolType][k];
	if (!*fl) {
		EFI_PHYSICAL_ADDRESS a;
		if (EFI_ERROR(core_allocate_pages(AllocateAnyPages, PoolType, 1, &a)))
			return EFI_OUT_OF_RESOURCES;
		for (UINTN o = 0; o + (32u << k) <= EFI_PAGE_SIZE; o += 32u << k) {
			struct pool_free *f = (void *)(uintptr_t)(a + o);
			f->next = *fl;
			*fl = f;
		}
	}
	struct pool_hdr *h = (void *)*fl;
	*fl = (*fl)->next;
	*h = (struct pool_hdr){ POOL_MAGIC, PoolType, k };
	*Buffer = h + 1;
	return EFI_SUCCESS;
}

EFI_STATUS EFIAPI
core_free_pool(VOID *Buffer)
{
	if (!Buffer)
		return EFI_INVALID_PARAMETER;
	struct pool_hdr *h = (struct pool_hdr *)Buffer - 1;
	if (h->magic != POOL_MAGIC || h->type >= POOL_TYPES)
		return EFI_INVALID_PARAMETER;
	h->magic = 0;
	if (h->size >> 63)
		return core_free_pages((uint64_t)(uintptr_t)h, h->size & ~(1ull << 63));
	if (h->size >= POOL_CLASSES)
		return EFI_INVALID_PARAMETER;
	struct pool_free *f = (void *)h;
	f->next = free_lists[h->type][h->size];
	free_lists[h->type][h->size] = f;
	return EFI_SUCCESS;
}

void *
core_alloc(UINTN size)
{
	void *p;
	if (EFI_ERROR(core_allocate_pool(EfiBootServicesData, size, &p)))
		return 0;
	memset(p, 0, size);
	return p;
}

void *
core_alloc_rt(UINTN size)
{
	void *p;
	if (EFI_ERROR(core_allocate_pool(EfiRuntimeServicesData, size, &p)))
		return 0;
	memset(p, 0, size);
	return p;
}

void
core_free(void *p)
{
	if (p)
		core_free_pool(p);
}

/* ---- the initial map from the HOB list */

void
mem_init(const void *hobs)
{
	const EFI_HOB_HANDOFF_INFO_TABLE *phit = hobs;
	/* system memory */
	for (const EFI_HOB_RESOURCE_DESCRIPTOR *r = hob_next(hobs, EFI_HOB_TYPE_RESOURCE_DESCRIPTOR); r;
	    r = hob_next((const UINT8 *)r + r->Header.HobLength, EFI_HOB_TYPE_RESOURCE_DESCRIPTOR)) {
		if (r->ResourceType == EFI_RESOURCE_SYSTEM_MEMORY)
			map_add(r->PhysicalStart, r->ResourceLength / EFI_PAGE_SIZE, EfiConventionalMemory,
			    EFI_MEMORY_UC | EFI_MEMORY_WC | EFI_MEMORY_WT | EFI_MEMORY_WB);
		else if (r->ResourceType == EFI_RESOURCE_FIRMWARE_DEVICE)
			map_add(r->PhysicalStart, r->ResourceLength / EFI_PAGE_SIZE, EfiMemoryMappedIO,
			    EFI_MEMORY_UC);
	}
	/* the HOB list itself */
	uint64_t hb = (uint64_t)(uintptr_t)phit;
	uint64_t he = ALIGN_UP(phit->EfiEndOfHobList + sizeof(EFI_HOB_GENERIC_HEADER), EFI_PAGE_SIZE);
	map_set(hb, (he - hb) / EFI_PAGE_SIZE, EfiConventionalMemory, EfiBootServicesData);
	/* PEI's allocations */
	for (const EFI_HOB_MEMORY_ALLOCATION *m = hob_next(hobs, EFI_HOB_TYPE_MEMORY_ALLOCATION); m;
	    m = hob_next((const UINT8 *)m + m->Header.HobLength, EFI_HOB_TYPE_MEMORY_ALLOCATION)) {
		const EFI_HOB_MEMORY_ALLOCATION_HEADER *a = &m->AllocDescriptor;
		uint64_t s = ALIGN_DOWN(a->MemoryBaseAddress, EFI_PAGE_SIZE);
		uint64_t e = ALIGN_UP(a->MemoryBaseAddress + a->MemoryLength, EFI_PAGE_SIZE);
		map_set(s, (e - s) / EFI_PAGE_SIZE, EfiConventionalMemory, a->MemoryType);
	}
}
