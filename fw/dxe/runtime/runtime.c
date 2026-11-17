/*
 * RuntimeDxe: the UEFI runtime services (UEFI 2.10 ch. 8) for the vibe86
 * board, in runtime-services memory so they outlive ExitBootServices:
 *
 *  - time: the MC146818 RTC in the platform controller (CMOS registers 0-9,
 *    32h century; Motorola MC146818A datasheet; BCD or binary per register
 *    B DM, 24-hour per register B 24/12);
 *  - reset: port CF9h (Intel PCH reset control, RST_CPU | SYS_RST) for cold
 *    and warm resets, ACPI PM1a_CNT S5 (SLP_EN, SLP_TYP 0 as the DSDT's _S5
 *    says) for shutdown;
 *  - variables: kept in memory; non-volatile ones are also written to the
 *    board's 4 KiB battery-backed NVRAM (docs/platform.md "NVRAM", vsim -N),
 *    in a store of length-prefixed records with a checksum.
 *
 * SetVirtualAddressMap isn't supported: the firmware's services stay at
 * their physical addresses (as they are identity-mapped by the loaders that
 * use them, such as CSMWrap).
 */
#include "uefilib.h"
#include "x86.h"

/* ---- time */

static uint8_t
cmos_r(uint8_t r)
{
	outb(RTC_PORT, r | 0x80);
	return inb(RTC_PORT + 1);
}

static void
cmos_w(uint8_t r, uint8_t v)
{
	outb(RTC_PORT, r | 0x80);
	outb(RTC_PORT + 1, v);
}

static uint8_t
from_bcd(uint8_t v, int bcd)
{
	return bcd ? (uint8_t)((v >> 4) * 10 + (v & 15)) : v;
}

static uint8_t
to_bcd(uint8_t v, int bcd)
{
	return bcd ? (uint8_t)((v / 10) << 4 | v % 10) : v;
}

static EFI_STATUS EFIAPI
rt_get_time(EFI_TIME *Time, EFI_TIME_CAPABILITIES *Cap)
{
	if (!Time)
		return EFI_INVALID_PARAMETER;
	for (int i = 0; i < 100000 && (cmos_r(0x0A) & 0x80); i++)	/* UIP */
		;
	uint8_t b = cmos_r(0x0B);
	int bcd = !(b & 0x04);
	uint8_t hour = cmos_r(0x04);
	int pm = !(b & 0x02) && (hour & 0x80);
	hour = from_bcd(hour & 0x7F, bcd);
	if (!(b & 0x02))
		hour = (uint8_t)(hour % 12 + (pm ? 12 : 0));
	memset(Time, 0, sizeof *Time);
	Time->Second = from_bcd(cmos_r(0x00), bcd);
	Time->Minute = from_bcd(cmos_r(0x02), bcd);
	Time->Hour = hour;
	Time->Day = from_bcd(cmos_r(0x07), bcd);
	Time->Month = from_bcd(cmos_r(0x08), bcd);
	Time->Year = (UINT16)(from_bcd(cmos_r(0x32), bcd) * 100 + from_bcd(cmos_r(0x09), bcd));
	Time->TimeZone = EFI_UNSPECIFIED_TIMEZONE;
	if (Cap) {
		Cap->Resolution = 1;
		Cap->Accuracy = 50000000;	/* 50 ppm */
		Cap->SetsToZero = FALSE;
	}
	return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
rt_set_time(EFI_TIME *t)
{
	if (!t || t->Month < 1 || t->Month > 12 || t->Day < 1 || t->Day > 31 || t->Hour > 23 ||
	    t->Minute > 59 || t->Second > 59 || t->Year < 1900 || t->Year > 9999)
		return EFI_INVALID_PARAMETER;
	uint8_t b = cmos_r(0x0B);
	int bcd = !(b & 0x04);
	cmos_w(0x0B, b | 0x80 | 0x02);		/* SET, 24-hour */
	cmos_w(0x00, to_bcd(t->Second, bcd));
	cmos_w(0x02, to_bcd(t->Minute, bcd));
	cmos_w(0x04, to_bcd(t->Hour, bcd));
	cmos_w(0x07, to_bcd(t->Day, bcd));
	cmos_w(0x08, to_bcd(t->Month, bcd));
	cmos_w(0x09, to_bcd((uint8_t)(t->Year % 100), bcd));
	cmos_w(0x32, to_bcd((uint8_t)(t->Year / 100), bcd));
	cmos_w(0x0B, (b & ~0x80) | 0x02);
	return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
rt_get_wakeup_time(BOOLEAN *Enabled, BOOLEAN *Pending, EFI_TIME *Time)
{
	return EFI_UNSUPPORTED;
}

static EFI_STATUS EFIAPI
rt_set_wakeup_time(BOOLEAN Enable, EFI_TIME *Time)
{
	return EFI_UNSUPPORTED;
}

/* ---- reset */

static VOID EFIAPI
rt_reset_system(EFI_RESET_TYPE Type, EFI_STATUS Status, UINTN DataSize, VOID *Data)
{
	if (Type == EfiResetShutdown) {
		outw(PM1_PORT + 4, 0x2000);	/* SLP_EN, SLP_TYP = S5 */
	} else {
		outb(RESET_PORT, 0x02);
		outb(RESET_PORT, Type == EfiResetCold ? 0x0E : 0x06);	/* FULL_RST | RST_CPU | SYS_RST */
	}
	for (;;)
		cpu_halt();
}

/* ---- variables */

#define VAR_MAX_BYTES	(64 * 1024)
#define NV_MAGIC	0x3153564Eu	/* "NVS1" */
#define NV_MAX_DATA	1024

struct var {
	struct var *next;
	EFI_GUID guid;
	UINT32 attr;
	UINT16 name_size;	/* bytes, with the NUL */
	UINT16 data_size;
	CHAR16 *name;
	UINT8 *data;
};

static struct var *vars;
static UINT8 *heap;		/* runtime memory for the variables */
static UINTN heap_used, heap_size;
static UINT32 high_count;

/* a bump allocator in runtime memory (freed space is reused only by a
   variable of the same or smaller size; the store is small) */
static void *
var_alloc(UINTN n)
{
	n = ALIGN_UP(n, 8);
	if (heap_used + n > heap_size)
		return 0;
	void *p = heap + heap_used;
	heap_used += n;
	return p;
}

static struct var *
var_find(const CHAR16 *name, const EFI_GUID *g)
{
	for (struct var *v = vars; v; v = v->next)
		if (v->data_size && guid_eq(&v->guid, g) && !wcscmp(v->name, name))
			return v;
	return 0;
}

/* NVRAM layout: u32 magic, u16 bytes of records, u16 sum of those bytes,
   then records: u32 attributes, u16 name size, u16 data size, GUID, name,
   data, padded to 4 */
static void
nv_flush(void)
{
	volatile UINT8 *nv = (volatile UINT8 *)(uintptr_t)NVRAM_BASE;
	UINT8 buf[NVRAM_SIZE];
	UINTN n = 8;
	for (struct var *v = vars; v; v = v->next) {
		if (!(v->attr & EFI_VARIABLE_NON_VOLATILE) || !v->data_size)
			continue;
		UINTN rec = ALIGN_UP(24u + v->name_size + v->data_size, 4);
		if (n + rec > sizeof buf)
			break;
		memcpy(buf + n, &v->attr, 4);
		memcpy(buf + n + 4, &v->name_size, 2);
		memcpy(buf + n + 6, &v->data_size, 2);
		memcpy(buf + n + 8, &v->guid, 16);
		memcpy(buf + n + 24, v->name, v->name_size);
		memcpy(buf + n + 24 + v->name_size, v->data, v->data_size);
		memset(buf + n + 24 + v->name_size + v->data_size, 0,
		    rec - 24 - v->name_size - v->data_size);
		n += rec;
	}
	UINT32 magic = NV_MAGIC;
	UINT16 len = (UINT16)(n - 8), sum = 0;
	for (UINTN i = 8; i < n; i++)
		sum = (UINT16)(sum + buf[i]);
	memcpy(buf, &magic, 4);
	memcpy(buf + 4, &len, 2);
	memcpy(buf + 6, &sum, 2);
	for (UINTN i = 0; i < n; i++)
		nv[i] = buf[i];
	/* any SYSCON access makes vsim save its -N file now */
	(void)mmio_r32(SYSCON_BASE);
}

static EFI_STATUS
var_store(const CHAR16 *name, const EFI_GUID *g, UINT32 attr, UINTN size, const void *data)
{
	struct var *v = var_find(name, g);
	if (v && size <= v->data_size) {
		memcpy(v->data, data, size);
		v->data_size = (UINT16)size;
		v->attr = attr;
		return EFI_SUCCESS;
	}
	UINTN ns = (wcslen(name) + 1) * 2;
	struct var *nv = var_alloc(sizeof *nv);
	CHAR16 *nm = var_alloc(ns);
	UINT8 *d = var_alloc(size);
	if (!nv || !nm || !d)
		return EFI_OUT_OF_RESOURCES;
	if (v)
		v->data_size = 0;	/* replaced: the old record is dead */
	memcpy(nm, name, ns);
	memcpy(d, data, size);
	*nv = (struct var){ vars, *g, attr, (UINT16)ns, (UINT16)size, nm, d };
	vars = nv;
	return EFI_SUCCESS;
}

static void
nv_load(void)
{
	const volatile UINT8 *nv = (const volatile UINT8 *)(uintptr_t)NVRAM_BASE;
	UINT8 buf[NVRAM_SIZE];
	for (UINTN i = 0; i < sizeof buf; i++)
		buf[i] = nv[i];
	UINT32 magic;
	UINT16 len, sum, s = 0;
	memcpy(&magic, buf, 4);
	memcpy(&len, buf + 4, 2);
	memcpy(&sum, buf + 6, 2);
	if (magic != NV_MAGIC || len > sizeof buf - 8)
		return;
	for (UINTN i = 8; i < 8u + len; i++)
		s = (UINT16)(s + buf[i]);
	if (s != sum) {
		DEBUG("RuntimeDxe: NVRAM checksum mismatch, variables reset\n");
		return;
	}
	/* records are stored newest first; load oldest first to keep the order */
	UINTN offs[128], n = 0;
	for (UINTN o = 8; o + 24 <= 8u + len && n < ARRAY_SIZE(offs);) {
		UINT16 ns, ds;
		memcpy(&ns, buf + o + 4, 2);
		memcpy(&ds, buf + o + 6, 2);
		offs[n++] = o;
		o += ALIGN_UP(24u + ns + ds, 4);
	}
	while (n--) {
		UINTN o = offs[n];
		UINT32 attr;
		UINT16 ns, ds;
		EFI_GUID g;
		memcpy(&attr, buf + o, 4);
		memcpy(&ns, buf + o + 4, 2);
		memcpy(&ds, buf + o + 6, 2);
		memcpy(&g, buf + o + 8, 16);
		CHAR16 name[128];
		if (ns > sizeof name || ns < 2)
			continue;
		memcpy(name, buf + o + 24, ns);
		name[ns / 2 - 1] = 0;
		var_store(name, &g, attr, ds, buf + o + 24 + ns);
	}
}

static EFI_STATUS EFIAPI
rt_get_variable(CHAR16 *Name, EFI_GUID *Guid, UINT32 *Attributes, UINTN *DataSize, VOID *Data)
{
	if (!Name || !Guid || !DataSize)
		return EFI_INVALID_PARAMETER;
	struct var *v = var_find(Name, Guid);
	if (!v)
		return EFI_NOT_FOUND;
	if (*DataSize < v->data_size) {
		*DataSize = v->data_size;
		return EFI_BUFFER_TOO_SMALL;
	}
	if (!Data)
		return EFI_INVALID_PARAMETER;
	memcpy(Data, v->data, v->data_size);
	*DataSize = v->data_size;
	if (Attributes)
		*Attributes = v->attr;
	return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
rt_get_next_variable_name(UINTN *NameSize, CHAR16 *Name, EFI_GUID *Guid)
{
	if (!NameSize || !Name || !Guid)
		return EFI_INVALID_PARAMETER;
	struct var *v = vars;
	if (Name[0]) {
		v = var_find(Name, Guid);
		if (!v)
			return EFI_INVALID_PARAMETER;
		v = v->next;
	}
	while (v && !v->data_size)
		v = v->next;
	if (!v)
		return EFI_NOT_FOUND;
	if (*NameSize < v->name_size) {
		*NameSize = v->name_size;
		return EFI_BUFFER_TOO_SMALL;
	}
	memcpy(Name, v->name, v->name_size);
	*NameSize = v->name_size;
	*Guid = v->guid;
	return EFI_SUCCESS;
}

static int boot_services_gone;

static EFI_STATUS EFIAPI
rt_set_variable(CHAR16 *Name, EFI_GUID *Guid, UINT32 Attributes, UINTN DataSize, VOID *Data)
{
	if (!Name || !Name[0] || !Guid || (DataSize && !Data) || DataSize > NV_MAX_DATA)
		return EFI_INVALID_PARAMETER;
	if (Attributes & ~(UINT32)(EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS |
	    EFI_VARIABLE_RUNTIME_ACCESS))
		return EFI_UNSUPPORTED;		/* authenticated variables aren't */
	if (Attributes && !(Attributes & EFI_VARIABLE_BOOTSERVICE_ACCESS))
		return EFI_INVALID_PARAMETER;
	if (boot_services_gone && Attributes && !(Attributes & EFI_VARIABLE_RUNTIME_ACCESS))
		return EFI_INVALID_PARAMETER;
	struct var *v = var_find(Name, Guid);
	int nv = (v && (v->attr & EFI_VARIABLE_NON_VOLATILE)) || (Attributes & EFI_VARIABLE_NON_VOLATILE);
	if (!DataSize || !Attributes) {
		if (!v)
			return EFI_NOT_FOUND;
		v->data_size = 0;
	} else {
		if (v && v->attr != Attributes)
			return EFI_INVALID_PARAMETER;
		EFI_STATUS s = var_store(Name, Guid, Attributes, DataSize, Data);
		if (EFI_ERROR(s))
			return s;
	}
	if (nv)
		nv_flush();
	return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
rt_query_variable_info(UINT32 Attributes, UINT64 *Max, UINT64 *Remaining, UINT64 *MaxVar)
{
	if (!Max || !Remaining || !MaxVar)
		return EFI_INVALID_PARAMETER;
	*Max = (Attributes & EFI_VARIABLE_NON_VOLATILE) ? NVRAM_SIZE - 8 : heap_size;
	*Remaining = heap_size - heap_used;
	*MaxVar = NV_MAX_DATA;
	return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
rt_get_next_high_monotonic_count(UINT32 *HighCount)
{
	if (!HighCount)
		return EFI_INVALID_PARAMETER;
	*HighCount = ++high_count;
	return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
rt_set_virtual_address_map(UINTN MemoryMapSize, UINTN DescriptorSize, UINT32 DescriptorVersion,
    EFI_MEMORY_DESCRIPTOR *VirtualMap)
{
	return EFI_UNSUPPORTED;
}

static EFI_STATUS EFIAPI
rt_convert_pointer(UINTN DebugDisposition, VOID **Address)
{
	return EFI_UNSUPPORTED;
}

static EFI_STATUS EFIAPI
rt_update_capsule(EFI_CAPSULE_HEADER **H, UINTN N, EFI_PHYSICAL_ADDRESS S)
{
	return EFI_UNSUPPORTED;
}

static EFI_STATUS EFIAPI
rt_query_capsule(EFI_CAPSULE_HEADER **H, UINTN N, UINT64 *Max, EFI_RESET_TYPE *T)
{
	return EFI_UNSUPPORTED;
}

static VOID EFIAPI
on_exit_boot_services(EFI_EVENT e, VOID *ctx)
{
	boot_services_gone = 1;
}

EFI_STATUS EFIAPI
efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st)
{
	uefi_init(image, st);
	heap_size = VAR_MAX_BYTES;
	heap = rt_calloc(heap_size);
	if (!heap)
		return EFI_OUT_OF_RESOURCES;
	nv_load();
	EFI_RUNTIME_SERVICES *rt = gRT;
	rt->GetTime = rt_get_time;
	rt->SetTime = rt_set_time;
	rt->GetWakeupTime = rt_get_wakeup_time;
	rt->SetWakeupTime = rt_set_wakeup_time;
	rt->SetVirtualAddressMap = rt_set_virtual_address_map;
	rt->ConvertPointer = rt_convert_pointer;
	rt->GetVariable = rt_get_variable;
	rt->GetNextVariableName = rt_get_next_variable_name;
	rt->SetVariable = rt_set_variable;
	rt->GetNextHighMonotonicCount = rt_get_next_high_monotonic_count;
	rt->ResetSystem = rt_reset_system;
	rt->UpdateCapsule = rt_update_capsule;
	rt->QueryCapsuleCapabilities = rt_query_capsule;
	rt->QueryVariableInfo = rt_query_variable_info;
	rt->Hdr.CRC32 = 0;
	gBS->CalculateCrc32(rt, rt->Hdr.HeaderSize, &rt->Hdr.CRC32);
	EFI_EVENT ev;
	gBS->CreateEvent(EVT_SIGNAL_EXIT_BOOT_SERVICES, TPL_NOTIFY, on_exit_boot_services, 0, &ev);
	EFI_TIME t;
	rt_get_time(&t, 0);
	DEBUG("RuntimeDxe: %04u-%02u-%02u %02u:%02u:%02u, variable store %u KiB\n", t.Year, t.Month,
	    t.Day, t.Hour, t.Minute, t.Second, (unsigned)(heap_size >> 10));
	return EFI_SUCCESS;
}
