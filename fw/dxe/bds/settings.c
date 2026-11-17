/*
 * BdsDxe: the setup settings, kept as the non-volatile UEFI variable
 * "Setup" (as AMI Aptio firmware keeps its setup data) through RuntimeDxe,
 * which stores it in the board's NVRAM.
 */
#include "bds.h"

struct settings cfg;
static EFI_GUID setup_guid = SETUP_GUID;

void
settings_defaults(struct settings *s)
{
	memset(s, 0, sizeof *s);
	s->version = SETUP_VERSION;
	s->timeout = 5;			/* 50 ms: the simulator runs ~1/150 real time */
	s->usb = 1;
	s->boot_first = BOOT_REMOVABLE;
	s->boot_second = BOOT_FWCFG;
	s->boot_third = BOOT_SHELL;
}

int
settings_load(void)
{
	struct settings s;
	UINTN n = sizeof s;
	UINT32 attr;
	if (gRT->GetVariable(L"Setup", &setup_guid, &attr, &n, &s) == EFI_SUCCESS && n == sizeof s &&
	    s.version == SETUP_VERSION) {
		cfg = s;
		return 0;
	}
	settings_defaults(&cfg);
	return -1;
}

EFI_STATUS
settings_save(void)
{
	return gRT->SetVariable(L"Setup", &setup_guid, EFI_VARIABLE_NON_VOLATILE |
	    EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS, sizeof cfg, &cfg);
}
