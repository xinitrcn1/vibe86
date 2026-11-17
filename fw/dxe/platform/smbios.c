/*
 * PlatformDxe: the SMBIOS tables (DMTF DSP0134, "System Management BIOS
 * Reference Specification", 3.6.0, 2022): types 0 BIOS, 1 System, 2
 * Baseboard, 3 Chassis, 4 Processor, 16 Physical Memory Array, 17 Memory
 * Device, 19 Memory Array Mapped Address, 32 System Boot, 127 End, with
 * both a 64-bit (3.0, "_SM3_") and a 32-bit (2.1, "_SM_") entry point
 * (DSP0134 5.2), all below 4 GiB, published in the system table (UEFI 2.10
 * 4.6, SMBIOS3_TABLE_GUID and SMBIOS_TABLE_GUID).
 */
#include "uefilib.h"
#include "vibe86fw.h"
#include "x86.h"
#include "layout.h"

static uint8_t *tbl;
static unsigned len, nstruct, maxsize;
static uint16_t handle;

/* a structure: the formatted area, then its strings (each NUL-terminated,
   the set ending in an extra NUL) */
static void
add(const void *formatted, unsigned flen, const char *const *strs, unsigned nstrs)
{
	unsigned start = len;
	memcpy(tbl + len, formatted, flen);
	tbl[len + 2] = (uint8_t)handle;
	tbl[len + 3] = (uint8_t)(handle >> 8);
	handle++;
	len += flen;
	for (unsigned i = 0; i < nstrs; i++) {
		size_t l = strlen(strs[i]);
		memcpy(tbl + len, strs[i], l + 1);
		len += (unsigned)l + 1;
	}
	if (!nstrs)
		tbl[len++] = 0;
	tbl[len++] = 0;
	nstruct++;
	if (len - start > maxsize)
		maxsize = len - start;
}

static uint8_t
sum(const uint8_t *p, unsigned n)
{
	uint8_t s = 0;
	while (n--)
		s = (uint8_t)(s + *p++);
	return s;
}

void
smbios_install(void)
{
	EFI_PHYSICAL_ADDRESS a = 0xFFFFFFFF;
	if (gBS->AllocatePages(AllocateMaxAddress, EfiRuntimeServicesData, 2, &a))
		return;
	tbl = (uint8_t *)(uintptr_t)a + 64;	/* the entry points first */
	memset((void *)(uintptr_t)a, 0, 2 * EFI_PAGE_SIZE);

	/* the RAM, for types 17 and 19 */
	UINTN msz = 0, key, dsz;
	UINT32 dver;
	uint64_t ram = 0;
	gBS->GetMemoryMap(&msz, 0, &key, &dsz, &dver);
	msz += 4 * dsz;
	EFI_MEMORY_DESCRIPTOR *mm = malloc(msz);
	if (mm && gBS->GetMemoryMap(&msz, mm, &key, &dsz, &dver) == EFI_SUCCESS)
		for (UINTN i = 0; i < msz / dsz; i++) {
			EFI_MEMORY_DESCRIPTOR *d = (void *)((uint8_t *)mm + i * dsz);
			if (d->Type != EfiMemoryMappedIO && d->Type != EfiReservedMemoryType &&
			    d->Type != EfiMemoryMappedIOPortSpace)
				ram += d->NumberOfPages * EFI_PAGE_SIZE;
		}
	free(mm);
	ram = ALIGN_UP(ram, 1 << 20);
	uint32_t ram_mb = (uint32_t)(ram >> 20);

	/* CPU brand */
	char brand[49] = "vibe86";
	uint32_t r[4];
	cpuid(0x80000000, 0, r);
	if (r[0] >= 0x80000004) {
		for (unsigned i = 0; i < 3; i++) {
			cpuid(0x80000002 + i, 0, r);
			memcpy(brand + 16 * i, r, 16);
		}
		brand[48] = 0;
	}
	cpuid(1, 0, r);
	uint32_t sig = r[0];

	{	/* type 0: BIOS information */
		uint8_t t[26] = { 0, 26 };
		t[4] = 1;			/* vendor */
		t[5] = 2;			/* version */
		t[6] = 0x00;			/* starting segment: none (UEFI) */
		t[7] = 0x00;
		t[8] = 3;			/* release date */
		t[9] = (FD_SIZE >> 16) - 1;	/* ROM size in 64 KiB - 1 */
		t[10] = 0x80;			/* characteristics: bit 7 PCI */
		t[11] = 0x08;			/* bit 11 upgradeable (flash) */
		t[18] = 0x03;			/* ext. 1: ACPI, USB legacy */
		t[19] = 0x0C;			/* ext. 2: UEFI, enable targeted content distribution */
		t[20] = 0;			/* major release */
		t[21] = 1;			/* minor release */
		t[22] = 0xFF;			/* EC: none */
		t[23] = 0xFF;
		uint16_t ext = (uint16_t)(FD_SIZE >> 20);	/* extended ROM size in MiB */
		t[24] = (uint8_t)ext;
		t[25] = (uint8_t)(ext >> 8);
		const char *s[] = { "vibe86", FW_NAME " " FW_VERSION_STR, "11/17/2026" };
		add(t, sizeof t, s, 3);
	}
	{	/* type 1: system information */
		uint8_t t[27] = { 1, 27 };
		t[4] = 1;
		t[5] = 2;
		t[6] = 3;
		t[7] = 4;
		static const uint8_t uuid[16] = { 0x76, 0x69, 0x62, 0x65, 0x38, 0x36, 0x00, 0x01, 0x80,
		    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01 };
		memcpy(t + 8, uuid, 16);
		t[24] = 0x06;			/* wake-up type: power switch */
		t[25] = 5;			/* SKU */
		t[26] = 6;			/* family */
		const char *s[] = { "vibe86", "vibe86 test board (vsim)", "1.0", "VSIM-0001", "VIBE86-TEST",
		    "vibe86" };
		add(t, sizeof t, s, 6);
	}
	{	/* type 2: baseboard */
		uint8_t t[15] = { 2, 15 };
		t[4] = 1;
		t[5] = 2;
		t[6] = 3;
		t[7] = 4;
		t[8] = 0;
		t[9] = 0x01;			/* feature flags: hosting board */
		t[10] = 0;
		t[11] = (uint8_t)(handle + 1);	/* the chassis (type 3) comes next */
		t[12] = (uint8_t)((handle + 1) >> 8);
		t[13] = 0x0A;			/* board type: motherboard */
		t[14] = 0;
		const char *s[] = { "vibe86", "vibe86 test board", "1.0", "VSIM-0001" };
		add(t, sizeof t, s, 4);
	}
	{	/* type 3: chassis */
		uint8_t t[22] = { 3, 22 };
		t[4] = 1;
		t[5] = 0x17;			/* rack mount chassis */
		t[6] = 2;
		t[7] = 3;
		t[8] = 4;
		t[9] = t[10] = t[11] = 0x03;	/* boot-up, power, thermal state: safe */
		t[12] = 0x03;			/* security: none */
		const char *s[] = { "vibe86", "1.0", "VSIM-0001" };
		add(t, sizeof t, s, 3);
	}
	{	/* type 4: processor */
		uint8_t t[48] = { 4, 48 };
		t[4] = 1;			/* socket designation */
		t[5] = 0x03;			/* central processor */
		t[6] = 0xFE;			/* family: see family 2 */
		t[7] = 2;			/* manufacturer */
		memcpy(t + 8, &sig, 4);		/* ID: CPUID 1 EAX, then EDX */
		cpuid(1, 0, r);
		memcpy(t + 12, &r[3], 4);
		t[16] = 3;			/* version (brand string) */
		t[17] = 0x88;			/* voltage: 0.85 V (bit 7 + value x10) */
		uint16_t mhz = (uint16_t)(TSC_HZ / 1000000);
		t[18] = 100;			/* external clock MHz */
		t[20] = (uint8_t)mhz;		/* max speed */
		t[21] = (uint8_t)(mhz >> 8);
		t[22] = (uint8_t)mhz;		/* current speed */
		t[23] = (uint8_t)(mhz >> 8);
		t[24] = 0x41;			/* status: populated, enabled */
		t[25] = 0x06;			/* upgrade: none (other) */
		t[26] = t[27] = t[28] = t[29] = t[30] = t[31] = 0xFF;	/* no cache handles */
		t[32] = 0;			/* serial, asset, part: none */
		t[33] = 0;
		t[34] = 0;
		t[35] = 1;			/* core count */
		t[36] = 1;			/* cores enabled */
		t[37] = 1;			/* thread count */
		t[38] = 0x04;			/* characteristics: 64-bit capable */
		t[40] = 0x6B;			/* family 2: AMD Zen processor family (6Bh) */
		t[42] = 1;			/* core count 2 */
		t[44] = 1;
		t[46] = 1;
		const char *s[] = { "CPU0", "VibingVibing", brand };
		add(t, sizeof t, s, 3);
	}
	unsigned array_handle = handle;
	{	/* type 16: physical memory array */
		uint8_t t[23] = { 16, 23 };
		t[4] = 0x03;			/* location: system board */
		t[5] = 0x03;			/* use: system memory */
		t[6] = 0x03;			/* error correction: none */
		uint32_t maxkb = 0x80000000u;	/* see the extended field */
		memcpy(t + 7, &maxkb, 4);
		t[11] = 0xFE;			/* error information: not provided */
		t[12] = 0xFF;
		t[13] = 1;			/* devices */
		uint64_t ext = (uint64_t)16 << 40;	/* 16 TiB: 16 DIMM slots of the server board */
		memcpy(t + 15, &ext, 8);
		add(t, sizeof t, 0, 0);
	}
	{	/* type 17: memory device (the test board's SRAM) */
		uint8_t t[40] = { 17, 40 };
		t[4] = (uint8_t)array_handle;
		t[5] = (uint8_t)(array_handle >> 8);
		t[6] = 0xFE;			/* no error information */
		t[7] = 0xFF;
		t[8] = 64;			/* total width */
		t[10] = 64;			/* data width */
		uint16_t size = ram_mb < 0x7FFF ? (uint16_t)ram_mb : 0x7FFF;
		t[12] = (uint8_t)size;
		t[13] = (uint8_t)(size >> 8);
		t[14] = 0x0B;			/* form factor: proprietary card */
		t[16] = 1;			/* device locator */
		t[17] = 2;			/* bank locator */
		t[18] = 0x02;			/* type: unknown (SRAM model) */
		t[19] = 0x04;			/* type detail: static column... */
		t[23] = 3;			/* manufacturer */
		const char *s[] = { "DIMM0", "BANK 0", "vibe86" };
		add(t, sizeof t, s, 3);
	}
	{	/* type 19: memory array mapped address */
		uint8_t t[31] = { 19, 31 };
		uint32_t start = 0, end = (uint32_t)(ram / 1024 - 1);
		memcpy(t + 4, &start, 4);
		memcpy(t + 8, &end, 4);
		t[12] = (uint8_t)array_handle;
		t[13] = (uint8_t)(array_handle >> 8);
		t[14] = 1;			/* partition width */
		add(t, sizeof t, 0, 0);
	}
	{	/* type 32: system boot information */
		uint8_t t[11] = { 32, 11 };
		add(t, sizeof t, 0, 0);
	}
	{	/* type 127: end of table */
		uint8_t t[4] = { 127, 4 };
		add(t, sizeof t, 0, 0);
	}

	uint8_t *ep2 = (uint8_t *)(uintptr_t)a, *ep3 = ep2 + 32;
	/* 2.1 entry point (DSP0134 5.2.1) */
	memcpy(ep2, "_SM_", 4);
	ep2[5] = 0x1F;
	ep2[6] = 3;
	ep2[7] = 6;
	ep2[8] = (uint8_t)maxsize;
	ep2[9] = (uint8_t)(maxsize >> 8);
	memcpy(ep2 + 16, "_DMI_", 5);
	ep2[22] = (uint8_t)len;
	ep2[23] = (uint8_t)(len >> 8);
	uint32_t ta = (uint32_t)(uintptr_t)tbl;
	memcpy(ep2 + 24, &ta, 4);
	ep2[28] = (uint8_t)nstruct;
	ep2[29] = (uint8_t)(nstruct >> 8);
	ep2[30] = 0x36;
	ep2[21] = (uint8_t)-sum(ep2 + 16, 15);
	ep2[4] = (uint8_t)-sum(ep2, 0x1F);
	/* 3.0 entry point (DSP0134 5.2.2) */
	memcpy(ep3, "_SM3_", 5);
	ep3[6] = 0x18;
	ep3[7] = 3;
	ep3[8] = 6;
	ep3[10] = 1;
	memcpy(ep3 + 12, &len, 4);
	uint64_t ta64 = ta;
	memcpy(ep3 + 16, &ta64, 8);
	ep3[5] = (uint8_t)-sum(ep3, 0x18);

	static EFI_GUID g2 = SMBIOS_TABLE_GUID, g3 = SMBIOS3_TABLE_GUID;
	gBS->InstallConfigurationTable(&g3, ep3);
	gBS->InstallConfigurationTable(&g2, ep2);
	DEBUG("PlatformDxe: SMBIOS 3.6, %u structures, %u bytes\n", nstruct, len);
}
