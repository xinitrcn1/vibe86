/*
 * The DXE core's entry point (PI 1.8 Vol. 2 ch. 2 "DXE Foundation"): from
 * PEI's HOB list it builds the memory map, the system table and the boot
 * services, dispatches the DXE drivers of FV_MAIN and hands over to the BDS
 * architectural protocol (the boot manager, fw/dxe/bds).
 */
#include "dxe.h"

#define EFI_BDS_ARCH_PROTOCOL_GUID EFI_GUID_DEF(0x665E3FF6, 0x46CC, 0x11d4, 0x9A, 0x38, 0x00, 0x90, 0x27, 0x3F, 0xC1, 0x4D)
#define EFI_HOB_LIST_GUID EFI_GUID_DEF(0x7739F24C, 0x93D7, 0x11D4, 0x9A, 0x3A, 0x00, 0x90, 0x27, 0x3F, 0xC1, 0x4D)

typedef struct EFI_BDS_ARCH_PROTOCOL {
	VOID (EFIAPI *Entry)(struct EFI_BDS_ARCH_PROTOCOL *This);
} EFI_BDS_ARCH_PROTOCOL;

EFI_SYSTEM_TABLE *gST;
EFI_BOOT_SERVICES gBootServices;
EFI_RUNTIME_SERVICES *gRT;
EFI_HANDLE gDxeCoreImage;
int gBootServicesDone;
const VIBE86_PLATFORM_INFO *gPlatformInfo;
static UINT64 monotonic;

/* ---- HOBs */

const void *
hob_next(const void *h, UINT16 type)
{
	const EFI_HOB_GENERIC_HEADER *g = h;
	for (; g && g->HobType != EFI_HOB_TYPE_END_OF_HOB_LIST;
	    g = (const void *)((const UINT8 *)g + g->HobLength)) {
		if (g->HobLength < sizeof *g)
			return 0;
		if (!type || g->HobType == type)
			return g;
	}
	return 0;
}

const void *
hob_guid(const void *hobs, const EFI_GUID *g)
{
	for (const EFI_HOB_GUID_TYPE *h = hob_next(hobs, EFI_HOB_TYPE_GUID_EXTENSION); h;
	    h = hob_next((const UINT8 *)h + h->Header.HobLength, EFI_HOB_TYPE_GUID_EXTENSION))
		if (guid_eq(&h->Name, g))
			return h + 1;
	return 0;
}

/* ---- miscellaneous boot services (UEFI 2.10 7.5) */

static void
table_crc(EFI_TABLE_HEADER *h)
{
	h->CRC32 = 0;
	h->CRC32 = crc32(h, h->HeaderSize);
}

static EFI_STATUS EFIAPI
core_calculate_crc32(VOID *Data, UINTN DataSize, UINT32 *Crc32)
{
	if (!Data || !DataSize || !Crc32)
		return EFI_INVALID_PARAMETER;
	*Crc32 = crc32(Data, DataSize);
	return EFI_SUCCESS;
}

static VOID EFIAPI
core_copy_mem(VOID *Destination, VOID *Source, UINTN Length)
{
	memmove(Destination, Source, Length);
}

static VOID EFIAPI
core_set_mem(VOID *Buffer, UINTN Size, UINT8 Value)
{
	memset(Buffer, Value, Size);
}

static EFI_STATUS EFIAPI
core_get_next_monotonic_count(UINT64 *Count)
{
	if (!Count)
		return EFI_INVALID_PARAMETER;
	*Count = monotonic++;
	return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
core_stall(UINTN Microseconds)
{
	udelay(Microseconds);
	return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
core_set_watchdog_timer(UINTN Timeout, UINT64 WatchdogCode, UINTN DataSize, CHAR16 *WatchdogData)
{
	/* the boot manager doesn't arm one, so there is nothing to disarm;
	   arming one isn't supported */
	return Timeout ? EFI_UNSUPPORTED : EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
core_install_configuration_table(EFI_GUID *Guid, VOID *Table)
{
	if (!Guid)
		return EFI_INVALID_PARAMETER;
	UINTN n = gST->NumberOfTableEntries;
	EFI_CONFIGURATION_TABLE *t = gST->ConfigurationTable;
	for (UINTN i = 0; i < n; i++)
		if (guid_eq(&t[i].VendorGuid, Guid)) {
			if (Table)
				t[i].VendorTable = Table;
			else {
				memmove(&t[i], &t[i + 1], (n - i - 1) * sizeof *t);
				gST->NumberOfTableEntries--;
			}
			table_crc(&gST->Hdr);
			return EFI_SUCCESS;
		}
	if (!Table)
		return EFI_NOT_FOUND;
	EFI_CONFIGURATION_TABLE *nt = core_alloc_rt((n + 1) * sizeof *nt);
	if (!nt)
		return EFI_OUT_OF_RESOURCES;
	if (n)
		memcpy(nt, t, n * sizeof *nt);
	nt[n].VendorGuid = *Guid;
	nt[n].VendorTable = Table;
	gST->ConfigurationTable = nt;
	gST->NumberOfTableEntries = n + 1;
	core_free(t);
	table_crc(&gST->Hdr);
	return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
core_exit_boot_services(EFI_HANDLE ImageHandle, UINTN MapKey)
{
	static const EFI_GUID ebs = EFI_EVENT_GROUP_EXIT_BOOT_SERVICES;
	if (gBootServicesDone)
		return EFI_SUCCESS;
	if (EFI_ERROR(mem_check_key(MapKey)))
		return EFI_INVALID_PARAMETER;
	post_code(POST_EXIT_BOOT_SERVICES);
	DEBUG("DXE: ExitBootServices\n");
	event_signal_group(&ebs);
	gBootServicesDone = 1;
	cpu_cli();
	/* UEFI 2.10 7.4.6: these system table fields become NULL */
	gST->ConsoleInHandle = 0;
	gST->ConIn = 0;
	gST->ConsoleOutHandle = 0;
	gST->ConOut = 0;
	gST->StandardErrorHandle = 0;
	gST->StdErr = 0;
	gST->BootServices = 0;
	table_crc(&gST->Hdr);
	return EFI_SUCCESS;
}

/* ---- runtime services before RuntimeDxe fills them in */

static EFI_STATUS EFIAPI
rt_unsupported(void)
{
	return EFI_UNSUPPORTED;
}

static void
init_tables(void)
{
	EFI_BOOT_SERVICES *bs = &gBootServices;
	bs->Hdr.Signature = EFI_BOOT_SERVICES_SIGNATURE;
	bs->Hdr.Revision = EFI_2_100_SYSTEM_TABLE_REVISION;
	bs->Hdr.HeaderSize = sizeof *bs;
	bs->AllocatePages = core_allocate_pages;
	bs->FreePages = core_free_pages;
	bs->GetMemoryMap = core_get_memory_map;
	bs->AllocatePool = core_allocate_pool;
	bs->FreePool = core_free_pool;
	bs->InstallConfigurationTable = core_install_configuration_table;
	bs->ExitBootServices = core_exit_boot_services;
	bs->GetNextMonotonicCount = core_get_next_monotonic_count;
	bs->Stall = core_stall;
	bs->SetWatchdogTimer = core_set_watchdog_timer;
	bs->CalculateCrc32 = core_calculate_crc32;
	bs->CopyMem = core_copy_mem;
	bs->SetMem = core_set_mem;
	event_init_services(bs);
	handle_init_services(bs);
	image_init_services(bs);
	table_crc(&bs->Hdr);

	gRT = core_alloc_rt(sizeof *gRT);
	gRT->Hdr.Signature = EFI_RUNTIME_SERVICES_SIGNATURE;
	gRT->Hdr.Revision = EFI_2_100_SYSTEM_TABLE_REVISION;
	gRT->Hdr.HeaderSize = sizeof *gRT;
	void **fn = (void **)&gRT->GetTime;
	for (; fn <= (void **)&gRT->QueryVariableInfo; fn++)
		*fn = (void *)(uintptr_t)rt_unsupported;
	table_crc(&gRT->Hdr);

	gST = core_alloc_rt(sizeof *gST);
	static const CHAR16 vendor[] = FW_VENDOR;
	CHAR16 *v = core_alloc_rt(sizeof vendor);
	memcpy(v, vendor, sizeof vendor);
	gST->Hdr.Signature = EFI_SYSTEM_TABLE_SIGNATURE;
	gST->Hdr.Revision = EFI_2_100_SYSTEM_TABLE_REVISION;
	gST->Hdr.HeaderSize = sizeof *gST;
	gST->FirmwareVendor = v;
	gST->FirmwareRevision = FW_REVISION;
	gST->RuntimeServices = gRT;
	gST->BootServices = bs;
	table_crc(&gST->Hdr);
}

void
dxe_main(void *hobs)
{
	static const EFI_GUID platform_guid = VIBE86_PLATFORM_HOB_GUID;
	static EFI_GUID hob_list_guid = EFI_HOB_LIST_GUID;
	static EFI_GUID bds_guid = EFI_BDS_ARCH_PROTOCOL_GUID;

	post_code(POST_DXE_START);
	DEBUG("DXE: core started, HOB list at %p\n", hobs);
	gPlatformInfo = hob_guid(hobs, &platform_guid);
	mem_init(hobs);
	cpu_init();
	init_tables();
	image_init(hobs);
	core_install_configuration_table(&hob_list_guid, hobs);
	DEBUG("DXE: memory map:\n");
	mem_dump();

	post_code(POST_DXE_DRIVERS);
	dispatch_drivers(hobs);

	EFI_BDS_ARCH_PROTOCOL *bds;
	gBootServices.LocateProtocol(&bds_guid, 0, (void **)&bds);
	if (!bds) {
		DEBUG("DXE: no BDS\n");
		com1_puts("vibe86 firmware: DXE: no boot device selection driver\n");
		for (;;)
			cpu_halt();
	}
	bds->Entry(bds);
	DEBUG("DXE: BDS returned\n");
	for (;;)
		cpu_halt();
}
