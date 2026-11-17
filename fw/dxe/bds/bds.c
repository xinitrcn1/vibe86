/*
 * BdsDxe: the BDS architectural protocol's entry (PI 1.8 Vol. 2 12.2). The
 * DXE core calls it once every driver has run: the POST screen, the setup
 * prompt (DEL/F2 setup, F11/F12 boot menu), then the boot manager.
 */
#include "bds.h"
#include "devname.h"

#define EFI_BDS_ARCH_PROTOCOL_GUID EFI_GUID_DEF(0x665E3FF6, 0x46CC, 0x11d4, 0x9A, 0x38, 0x00, 0x90, 0x27, 0x3F, 0xC1, 0x4D)
#define EFI_HOB_LIST_GUID EFI_GUID_DEF(0x7739F24C, 0x93D7, 0x11D4, 0x9A, 0x3A, 0x00, 0x90, 0x27, 0x3F, 0xC1, 0x4D)

typedef struct EFI_BDS_ARCH_PROTOCOL {
	VOID (EFIAPI *Entry)(struct EFI_BDS_ARCH_PROTOCOL *This);
} EFI_BDS_ARCH_PROTOCOL;

char cpu_brand[49];
UINT64 mem_total;
unsigned usb_keyboards, usb_mice, usb_storage;

static const VIBE86_PLATFORM_INFO *
platform_info(void)
{
	static const EFI_GUID hob_guid = EFI_HOB_LIST_GUID;
	static const EFI_GUID pg = VIBE86_PLATFORM_HOB_GUID;
	const EFI_HOB_GENERIC_HEADER *h = 0;
	for (UINTN i = 0; i < gST->NumberOfTableEntries; i++)
		if (guid_eq(&gST->ConfigurationTable[i].VendorGuid, &hob_guid))
			h = gST->ConfigurationTable[i].VendorTable;
	for (; h && h->HobType != EFI_HOB_TYPE_END_OF_HOB_LIST; h = (const void *)((const UINT8 *)h + h->HobLength))
		if (h->HobType == EFI_HOB_TYPE_GUID_EXTENSION &&
		    guid_eq(&((const EFI_HOB_GUID_TYPE *)h)->Name, &pg))
			return (const void *)((const EFI_HOB_GUID_TYPE *)h + 1);
	return 0;
}

static void
gather(void)
{
	const VIBE86_PLATFORM_INFO *pi = platform_info();
	if (pi) {
		memcpy(cpu_brand, pi->cpu_brand, sizeof cpu_brand);
		mem_total = pi->ram_below_4g + pi->ram_above_4g;
	}
	static EFI_GUID usb_guid = VIBE86_USB_INFO_GUID;
	VIBE86_USB_INFO *u;
	if (gBS->LocateProtocol(&usb_guid, 0, (void **)&u) == EFI_SUCCESS) {
		usb_keyboards = u->keyboards;
		usb_mice = u->mice;
		usb_storage = u->storage;
	}
}

static void
post_screen(int nv_ok)
{
	ui_clear(A_NORMAL);
	ui_cursor(0);
	ui_text(0, 0, A_BRIGHT, "%s %s", FW_NAME, FW_VERSION_STR);
	ui_text(0, 30, A_NORMAL, "Copyright (C) 2026 The vibe86 project");
	ui_text(1, 0, A_NORMAL, "Firmware build: %s %s", __DATE__, __TIME__);
	ui_text(3, 0, A_NORMAL, "CPU   : %s", cpu_brand);
	ui_text(4, 0, A_NORMAL, " Speed: %lu MHz", (unsigned long)(TSC_HZ / 1000000));
	ui_text(5, 0, A_NORMAL, "Total Memory: %lu MB", (unsigned long)((mem_total + (1 << 19)) >> 20));
	ui_text(7, 0, A_NORMAL, "USB Devices total: %u Keyboard, %u Mouse, %u Floppy", usb_keyboards,
	    usb_mice, usb_storage);
	if (!nv_ok)
		ui_text(9, 0, A_NORMAL, "NVRAM: no saved settings, the defaults are loaded.");
	ui_text(11, 0, A_BRIGHT, "Press <DEL> or <F2> to enter setup, <F11> or <F12> for the boot menu.");
	ui_at(12, 0);
}

/* the setup prompt: which hotkey (SCAN_* / CHAR), 0 if none came in time */
static int
prompt(void)
{
	EFI_INPUT_KEY k;
	uint64_t end = time_us() + (uint64_t)cfg.timeout * 10000;
	for (;;) {
		uint64_t now = time_us();
		if (!ui_key(&k, now < end ? end - now : 1))
			return 0;
		if (k.ScanCode == SCAN_DELETE || k.ScanCode == SCAN_F2)
			return SCAN_F2;
		if (k.ScanCode == SCAN_F11 || k.ScanCode == SCAN_F12)
			return SCAN_F11;
	}
}

static VOID EFIAPI
bds_entry(EFI_BDS_ARCH_PROTOCOL *This)
{
	post_code(POST_BDS_START);
	int nv = settings_load();
	gather();
	if (!gST->ConOut) {
		DEBUG("BDS: no console\n");
		boot_manager();
	}
	post_screen(nv == 0);
	DEBUG("BDS: setup prompt (%u0 ms)\n", cfg.timeout);
	int k = prompt();
	for (;;) {
		if (k == SCAN_F2) {
			post_code(POST_BDS_SETUP);
			setup_run();
		} else if (k == SCAN_F11) {
			int i = boot_menu();
			if (i == -2) {
				k = SCAN_F2;
				continue;
			}
			if (i >= 0) {
				boot_run(&boot_options[i]);
				continue;	/* back to the menu when it returns */
			}
		}
		ui_clear(A_NORMAL);
		boot_manager();
	}
}

static EFI_BDS_ARCH_PROTOCOL bds = { bds_entry };

EFI_STATUS EFIAPI
efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st)
{
	static EFI_GUID bds_guid = EFI_BDS_ARCH_PROTOCOL_GUID;
	uefi_init(image, st);
	EFI_HANDLE h = 0;
	return gBS->InstallProtocolInterface(&h, &bds_guid, EFI_NATIVE_INTERFACE, &bds);
}
