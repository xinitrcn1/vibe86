/*
 * CsmDxe: the firmware's Compatibility Support Module. In the PI model
 * (Intel, "Compatibility Support Module Specification" 0.98, 2007) a CSM
 * is a 16-bit legacy BIOS image (here fw/csm/csm16.S, a raw file of
 * FV_MAIN) plus the 32/64-bit code that loads it and boots legacy devices.
 * This one's BIOS services are C (bios.c) running on the UEFI drivers,
 * entered from the 16-bit image through a mode-switching thunk.
 *
 * A legacy boot (VIBE86_CSM_PROTOCOL.LegacyBoot):
 *  1. claims conventional memory below 640 KiB so UEFI never allocates it;
 *  2. opens F_0000h-F_FFFFh (Legacy Region 2: read and write to RAM) and
 *     copies the image there; its header gets the firmware's CR3 and IDTR,
 *     the dispatcher's address and a stack;
 *  3. builds the interrupt vector table, the BIOS data area (IBM PC/AT
 *     Technical Reference; equipment word, memory size, video state,
 *     keyboard buffer pointers, the tick count from the RTC) and a 1 KiB
 *     EBDA below 640 KiB;
 *  4. puts the display in 80x25 text mode, signals the LegacyBoot event
 *     group (UEFI 2.10 7.1, EFI_EVENT_LEGACY_BOOT_GUID) instead of
 *     ExitBootServices: the UEFI drivers keep running under the BIOS;
 *  5. reprograms the 8259s to vectors 08h and 70h with IRQ0 (and the
 *     cascade) unmasked and the 8254's channel 0 to 18.2 Hz;
 *  6. reads the boot sector to 0000:7C00 and enters it in real mode with
 *     DL = the drive (BIOS Boot Specification 1.01, 6.5.1).
 */
#include "csm.h"
#include "pe.h"
#include "layout.h"
#include "vibe86fw.h"

struct csm16_header *csm16;
EFI_BLOCK_IO_PROTOCOL *boot_bio;
UINT8 boot_drive;
unsigned geo_cyl, geo_heads, geo_spt;

static VIBE86_CSM_PROTOCOL csm;

static const void *
find_csm16(UINT32 *size)
{
	static const EFI_GUID g = FILE_CSM16_GUID;
	const EFI_FIRMWARE_VOLUME_HEADER *fv = fv_check(FV_MAIN_BASE);
	uint64_t cookie = 0;
	const EFI_FFS_FILE_HEADER *f;
	while (fv && (f = fv_next_file(fv, &cookie)))
		if (guid_eq(&f->Name, &g))
			return ffs_section(f, EFI_SECTION_RAW, size);
	return 0;
}

/* the boot sector, read into buf (one block) */
static int
read_sector0(EFI_BLOCK_IO_PROTOCOL *bio, UINT8 *buf)
{
	if (!bio->Media->MediaPresent || bio->Media->BlockSize != 512)
		return -1;
	return bio->ReadBlocks(bio, bio->Media->MediaId, 0, 512, buf) == EFI_SUCCESS ? 0 : -1;
}

static BOOLEAN EFIAPI
csm_bootable(VIBE86_CSM_PROTOCOL *This, EFI_HANDLE h)
{
	EFI_BLOCK_IO_PROTOCOL *bio;
	UINT8 *b;
	if (gBS->HandleProtocol(h, &gEfiBlockIoProtocolGuid, (void **)&bio) || bio->Media->LogicalPartition)
		return FALSE;
	if (!(b = malloc(512)))
		return FALSE;
	BOOLEAN ok = read_sector0(bio, b) == 0 && b[510] == 0x55 && b[511] == 0xAA;
	free(b);
	return ok;
}

/* floppy geometries by size (PC formats: 360K/720K/1.2M/1.44M/2.88M) */
static int
floppy_geometry(UINT64 blocks)
{
	static const struct { UINT32 blocks; UINT16 cyl; UINT8 heads, spt; } f[] = {
		{ 720, 40, 2, 9 }, { 1440, 80, 2, 9 }, { 2400, 80, 2, 15 }, { 2880, 80, 2, 18 },
		{ 5760, 80, 2, 36 },
	};
	for (unsigned i = 0; i < ARRAY_SIZE(f); i++)
		if (f[i].blocks == blocks) {
			geo_cyl = f[i].cyl;
			geo_heads = f[i].heads;
			geo_spt = f[i].spt;
			return 1;
		}
	return 0;
}

static void
claim_low_memory(void)
{
	unsigned missed = 0;
	for (EFI_PHYSICAL_ADDRESS a = 0; a < 0xA0000; a += EFI_PAGE_SIZE) {
		EFI_PHYSICAL_ADDRESS p = a;
		if (gBS->AllocatePages(AllocateAddress, EfiBootServicesData, 1, &p) != EFI_SUCCESS)
			missed++;
	}
	if (missed)
		DEBUG("CsmDxe: %u pages below 640 KiB were already in use\n", missed);
}

static void
build_ivt_bda(void)
{
	/* the IVT is at address 0: launder the pointer so the compiler can't
	   treat it as a null dereference */
	uintptr_t zero = 0;
	__asm__("" : "+r"(zero));
	volatile UINT16 *ivt = (volatile UINT16 *)zero;
	for (unsigned v = 0; v < 256; v++) {
		UINT16 off = csm16->iret_only;
		if (v < 32 && csm16->vectors[v])
			off = csm16->vectors[v];
		else if (v >= 0x70 && v < 0x78)
			off = csm16->irq_slave;
		ivt[2 * v] = off;
		ivt[2 * v + 1] = 0xF000;
	}
	/* INT 1Dh (video parameters), 1Fh and 43h (fonts), 41h/46h (fixed
	   disk parameters): no tables */
	static const unsigned none[] = { 0x1D, 0x1F, 0x41, 0x43, 0x46 };
	for (unsigned i = 0; i < ARRAY_SIZE(none); i++)
		ivt[2 * none[i]] = ivt[2 * none[i] + 1] = 0;

	volatile UINT8 *bda = BDA(0);
	for (unsigned i = 0; i < 256; i++)
		bda[i] = 0;
	BDA16(0x00) = 0x3F8;				/* COM1 */
	BDA16(0x0E) = EBDA_SEG;
	UINT16 equip = 0x0020 | 0x0200;			/* 80x25 colour, one serial port */
	if (boot_drive == 0x00)
		equip |= 0x0001;			/* a diskette drive (one) */
	BDA16(0x10) = equip;
	BDA16(0x13) = 639;				/* KiB below the EBDA */
	BDA16(0x1A) = 0x1E;				/* keyboard buffer head, tail */
	BDA16(0x1C) = 0x1E;
	BDA16(0x80) = 0x1E;				/* buffer start, end */
	BDA16(0x82) = 0x3E;
	*BDA(0x49) = 3;					/* video mode 03h */
	BDA16(0x4A) = 80;
	BDA16(0x4C) = 4096;
	BDA16(0x60) = 0x0607;				/* cursor shape */
	BDA16(0x63) = 0x3D4;
	*BDA(0x65) = 0x29;
	*BDA(0x75) = boot_drive == 0x80 ? 1 : 0;	/* hard disks */
	*BDA(0x84) = 24;				/* rows - 1 */
	BDA16(0x85) = 16;				/* character height */
	*BDA(0x87) = 0x60;
	*BDA(0x89) = 0x11;
	/* the EBDA: its size in KiB first */
	volatile UINT8 *ebda = (volatile UINT8 *)(uintptr_t)(EBDA_SEG * 16);
	for (unsigned i = 0; i < 1024; i++)
		ebda[i] = 0;
	ebda[0] = 1;
	/* the tick count: the RTC's time of day (18.2065 ticks a second) */
	EFI_TIME t;
	if (gRT->GetTime(&t, 0) == EFI_SUCCESS) {
		UINT32 s = t.Hour * 3600u + t.Minute * 60u + t.Second;
		BDA32(0x6C) = (UINT32)((UINT64)s * 1193182 / 65536);
	}
}

static void
pic_pit_legacy(void)
{
	/* 8259A: ICW1-ICW4 with the PC/AT vector bases; mask all but IRQ0, IRQ2 */
	outb(0x20, 0x11);
	outb(0xA0, 0x11);
	outb(0x21, 0x08);
	outb(0xA1, 0x70);
	outb(0x21, 0x04);
	outb(0xA1, 0x02);
	outb(0x21, 0x01);
	outb(0xA1, 0x01);
	outb(0x21, 0xFA);
	outb(0xA1, 0xFF);
	/* 8254 channel 0: mode 3, count 65536 (18.2 Hz) */
	outb(0x43, 0x36);
	outb(0x40, 0);
	outb(0x40, 0);
}

static EFI_STATUS EFIAPI
csm_legacy_boot(VIBE86_CSM_PROTOCOL *This, EFI_HANDLE h)
{
	UINT32 size;
	const void *img = find_csm16(&size);
	if (!img || size != CSM_SIZE)
		return EFI_NOT_FOUND;
	if (gBS->HandleProtocol(h, &gEfiBlockIoProtocolGuid, (void **)&boot_bio))
		return EFI_INVALID_PARAMETER;
	UINT8 *sec = malloc(512);
	if (!sec || read_sector0(boot_bio, sec) || sec[510] != 0x55 || sec[511] != 0xAA) {
		free(sec);
		return EFI_NOT_FOUND;
	}
	free(sec);
	boot_drive = floppy_geometry(boot_bio->Media->LastBlock + 1) ? 0x00 : 0x80;
	if (boot_drive == 0x80) {
		/* a hard disk: the usual LBA-assist translation, 255 heads x 63 */
		geo_heads = 255;
		geo_spt = 63;
		UINT64 c = (boot_bio->Media->LastBlock + 1) / (255 * 63);
		geo_cyl = c > 1024 ? 1024 : (unsigned)c;
	}
	DEBUG("CsmDxe: legacy boot from drive %02x (%u/%u/%u)\n", boot_drive, geo_cyl, geo_heads, geo_spt);

	/* the 16-bit image in shadow RAM at F_0000h */
	static EFI_GUID lr_guid = EFI_LEGACY_REGION2_PROTOCOL_GUID;
	EFI_LEGACY_REGION2_PROTOCOL *lr;
	UINT32 gran;
	BOOLEAN on = TRUE;
	if (gBS->LocateProtocol(&lr_guid, 0, (void **)&lr) ||
	    lr->Decode(lr, 0xE0000, 0x20000, &gran, &on) || lr->UnLock(lr, 0xE0000, 0x20000, &gran))
		return EFI_UNSUPPORTED;
	memcpy((void *)(uintptr_t)CSM_BASE, img, CSM_SIZE);
	csm16 = (struct csm16_header *)(uintptr_t)CSM_BASE;
	if (memcmp(csm16->sig, "VIBE86CS", 8))
		return EFI_LOAD_ERROR;
	UINT64 cr3;
	__asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
	__asm__ volatile("sidt %0" : "=m"(csm16->idtr64));
	csm16->cr3 = cr3;
	csm16->dispatch = (UINT64)(uintptr_t)csm_dispatch;
	UINT8 *stack = dma_alloc_pages(16);
	if (!stack || cr3 >= (1ull << 32))
		return EFI_OUT_OF_RESOURCES;
	csm16->rsp64 = (UINT64)(uintptr_t)stack + 16 * EFI_PAGE_SIZE;
	csm16->boot_dl = boot_drive;

	claim_low_memory();
	build_ivt_bda();
	bios_init();

	/* the legacy boot event group: drivers prepare, but keep running */
	static EFI_GUID legacy_boot = EFI_EVENT_LEGACY_BOOT_GUID;
	EFI_EVENT ev;
	extern VOID EFIAPI csm_noop(EFI_EVENT e, VOID *c);
	if (gBS->CreateEventEx(EVT_NOTIFY_SIGNAL, TPL_CALLBACK, csm_noop, 0, &legacy_boot, &ev) == EFI_SUCCESS) {
		gBS->SignalEvent(ev);
		gBS->CloseEvent(ev);
	}

	bios_load_boot_sector();
	post_code(POST_CSM_BOOT);
	DEBUG("CsmDxe: entering the boot sector\n");
	cpu_cli();
	pic_pit_legacy();
	void (*boot)(void) = (void (*)(void))(uintptr_t)(CSM_BASE + csm16->boot64);
	boot();
	return EFI_DEVICE_ERROR;	/* not reached */
}

VOID EFIAPI
csm_noop(EFI_EVENT e, VOID *c)
{
}

EFI_STATUS EFIAPI
efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st)
{
	static EFI_GUID g = VIBE86_CSM_PROTOCOL_GUID;
	uefi_init(image, st);
	UINT32 size;
	if (!find_csm16(&size) || size != CSM_SIZE) {
		DEBUG("CsmDxe: no CSM16 image\n");
		return EFI_NOT_FOUND;
	}
	csm.Bootable = csm_bootable;
	csm.LegacyBoot = csm_legacy_boot;
	EFI_HANDLE h = 0;
	return gBS->InstallProtocolInterface(&h, &g, EFI_NATIVE_INTERFACE, &csm);
}
