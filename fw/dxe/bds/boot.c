/*
 * BdsDxe: the boot manager (UEFI 2.10 3.1-3.5). Boot options are found
 * each boot, in the spirit of 3.5 "Boot Mechanisms": every Simple File
 * System with the removable-media boot file \EFI\BOOT\BOOTX64.EFI, and the
 * built-in EFI Shell from the firmware volume. Setup orders them by kind.
 */
#include "bds.h"
#include "devname.h"
#include "vibe86csm.h"

struct boot_option boot_options[MAX_BOOT_OPTIONS];
int nboot_options;

static EFI_GUID name_guid = VIBE86_DEVICE_NAME_GUID;
static const EFI_GUID fwcfg_guid = VIBE86_FWCFG_VOLUME_GUID;
static const EFI_GUID shell_guid = FILE_SHELL_GUID;
static const EFI_GUID ffs2_guid = EFI_FIRMWARE_FILE_SYSTEM2_GUID;
static EFI_GUID ready_to_boot = EFI_EVENT_GROUP_READY_TO_BOOT;

const char *
boot_type_name(int t)
{
	switch (t) {
	case BOOT_REMOVABLE: return "UEFI removable media";
	case BOOT_FWCFG: return "fw_cfg volume";
	case BOOT_LEGACY: return "Legacy (CSM) devices";
	case BOOT_SHELL: return "Built-in EFI Shell";
	}
	return "Disabled";
}

/* the display name of the device a path leads to (or of a parent) */
static void
device_name(EFI_DEVICE_PATH_PROTOCOL *path, char *buf, size_t n)
{
	EFI_DEVICE_PATH_PROTOCOL *p = path;
	EFI_HANDLE h;
	VIBE86_DEVICE_NAME *dn;
	if (gBS->LocateDevicePath(&name_guid, &p, &h) == EFI_SUCCESS &&
	    gBS->HandleProtocol(h, &name_guid, (void **)&dn) == EFI_SUCCESS) {
		strncpy0(buf, dn->name, n);
		return;
	}
	dp_to_text(path, buf, n);
}

static int
has_fwcfg_node(const EFI_DEVICE_PATH_PROTOCOL *p)
{
	for (; !dp_is_end(p); p = dp_next(p))
		if (p->Type == HARDWARE_DEVICE_PATH && p->SubType == HW_VENDOR_DP &&
		    guid_eq(&((const VENDOR_DEVICE_PATH *)p)->Guid, &fwcfg_guid))
			return 1;
	return 0;
}

static int
file_exists(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs, const CHAR16 *name)
{
	EFI_FILE_PROTOCOL *root, *f;
	if (fs->OpenVolume(fs, &root) != EFI_SUCCESS)
		return 0;
	EFI_STATUS s = root->Open(root, &f, (CHAR16 *)name, EFI_FILE_MODE_READ, 0);
	if (s == EFI_SUCCESS)
		f->Close(f);
	root->Close(root);
	return s == EFI_SUCCESS;
}

static void
add_option(const char *desc, int type, EFI_DEVICE_PATH_PROTOCOL *path)
{
	if (nboot_options == MAX_BOOT_OPTIONS)
		return;
	struct boot_option *o = &boot_options[nboot_options++];
	strncpy0(o->desc, desc, sizeof o->desc);
	o->type = type;
	o->path = path;
}

void
boot_scan(void)
{
	static const CHAR16 removable[] = L"\\EFI\\BOOT\\BOOTX64.EFI";
	for (int i = 0; i < nboot_options; i++)
		free(boot_options[i].path);
	nboot_options = 0;
	EFI_HANDLE *hs;
	UINTN n;
	if (gBS->LocateHandleBuffer(ByProtocol, &gEfiSimpleFileSystemProtocolGuid, 0, &n, &hs) == EFI_SUCCESS) {
		for (UINTN i = 0; i < n; i++) {
			EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs;
			EFI_DEVICE_PATH_PROTOCOL *dp = dp_of(hs[i]);
			if (!dp || gBS->HandleProtocol(hs[i], &gEfiSimpleFileSystemProtocolGuid, (void **)&fs))
				continue;
			if (!file_exists(fs, removable))
				continue;
			char name[56], desc[64];
			device_name(dp, name, sizeof name);
			snprintf(desc, sizeof desc, "UEFI: %s", name);
			add_option(desc, has_fwcfg_node(dp) ? BOOT_FWCFG : BOOT_REMOVABLE,
			    dp_append_file(dp, removable));
		}
		free(hs);
	}
	/* legacy boot options: block devices with a boot sector, through the CSM */
	static EFI_GUID csm_guid = VIBE86_CSM_PROTOCOL_GUID;
	VIBE86_CSM_PROTOCOL *csm;
	if (cfg.csm && gBS->LocateProtocol(&csm_guid, 0, (void **)&csm) == EFI_SUCCESS &&
	    gBS->LocateHandleBuffer(ByProtocol, &gEfiBlockIoProtocolGuid, 0, &n, &hs) == EFI_SUCCESS) {
		for (UINTN i = 0; i < n; i++) {
			EFI_DEVICE_PATH_PROTOCOL *dp = dp_of(hs[i]);
			if (!dp || !csm->Bootable(csm, hs[i]))
				continue;
			char name[56], desc[64];
			device_name(dp, name, sizeof name);
			snprintf(desc, sizeof desc, "Legacy: %s", name);
			add_option(desc, BOOT_LEGACY, dp_dup(dp));
			boot_options[nboot_options - 1].handle = hs[i];
		}
		free(hs);
	}
#pragma pack(push, 1)
	struct {
		MEDIA_FW_VOL_DEVICE_PATH fv;
		MEDIA_FW_VOL_FILEPATH_DEVICE_PATH file;
		EFI_DEVICE_PATH_PROTOCOL end;
	} sp;
#pragma pack(pop)
	dp_set_node(&sp.fv.Header, MEDIA_DEVICE_PATH, MEDIA_PIWG_FW_VOL_DP, sizeof sp.fv);
	sp.fv.FvName = ffs2_guid;
	dp_set_node(&sp.file.Header, MEDIA_DEVICE_PATH, MEDIA_PIWG_FW_FILE_DP, sizeof sp.file);
	sp.file.FvFileName = shell_guid;
	dp_set_end(&sp.end);
	add_option("UEFI: Built-in EFI Shell", BOOT_SHELL, dp_dup(&sp.fv.Header));
}

static VOID EFIAPI
noop(EFI_EVENT e, VOID *ctx)
{
}

EFI_STATUS
boot_run(const struct boot_option *o)
{
	EFI_HANDLE h;
	char text[160];
	dp_to_text(o->path, text, sizeof text);
	DEBUG("BDS: booting %s: %s\n", o->desc, text);
	post_code(POST_BDS_BOOT);
	if (o->type == BOOT_LEGACY) {
		static EFI_GUID csm_guid = VIBE86_CSM_PROTOCOL_GUID;
		VIBE86_CSM_PROTOCOL *csm;
		EFI_STATUS s = gBS->LocateProtocol(&csm_guid, 0, (void **)&csm);
		if (!EFI_ERROR(s)) {
			ui_clear(A_NORMAL);
			s = csm->LegacyBoot(csm, o->handle);
		}
		ui_attr(A_NORMAL);
		ui_printf("Legacy boot of \"%s\" failed: %r\n", o->desc, s);
		return s;
	}
	EFI_STATUS s = gBS->LoadImage(TRUE, gImageHandle, o->path, 0, 0, &h);
	if (EFI_ERROR(s)) {
		ui_attr(A_NORMAL);
		ui_printf("Boot option \"%s\" failed to load: %r\n", o->desc, s);
		return s;
	}
	/* UEFI 2.10 7.1: the ReadyToBoot event group, before each boot option */
	EFI_EVENT ev;
	if (gBS->CreateEventEx(EVT_NOTIFY_SIGNAL, TPL_CALLBACK, noop, 0, &ready_to_boot, &ev) == EFI_SUCCESS) {
		gBS->SignalEvent(ev);
		gBS->CloseEvent(ev);
	}
	ui_attr(A_NORMAL);
	ui_cursor(1);
	s = gBS->StartImage(h, 0, 0);
	DEBUG("BDS: %s returned %r\n", o->desc, s);
	gBS->SetWatchdogTimer(0, 0, 0, 0);
	return s;
}

void
boot_manager(void)
{
	UINT8 order[4] = { cfg.boot_first, cfg.boot_second, cfg.boot_third, cfg.boot_fourth };
	boot_scan();
	for (int k = 0; k < 4; k++) {
		if (order[k] >= BOOT_NTYPES)
			continue;
		for (int i = 0; i < nboot_options; i++)
			if (boot_options[i].type == order[k])
				boot_run(&boot_options[i]);
		boot_scan();	/* an option may have changed the devices */
	}
	ui_attr(A_NORMAL);
	ui_puts("\nReboot and Select proper Boot device\n"
	    "or Insert Boot Media in selected Boot device and press a key\n");
	EFI_INPUT_KEY k;
	ui_flush_keys();
	ui_key(&k, 0);
	gRT->ResetSystem(EfiResetCold, EFI_SUCCESS, 0, 0);
	for (;;)
		;
}
