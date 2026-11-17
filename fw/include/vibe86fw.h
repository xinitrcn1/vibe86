/*
 * vibe86fw.h: what the firmware's own phases and modules share beyond the
 * UEFI/PI interfaces: the version, PEI's platform-information HOB, the
 * GUIDs of the firmware's files and protocols, and the POST codes.
 */
#ifndef VIBE86FW_H
#define VIBE86FW_H

#include "pi.h"

#define FW_VENDOR		L"vibe86"
#define FW_NAME			"vibe86 UEFI"
#define FW_VERSION_STR		"0.1"
#define FW_REVISION		0x00000001	/* EFI_SYSTEM_TABLE.FirmwareRevision */

/* the platform-information GUID HOB PEI leaves for DXE */
#define VIBE86_PLATFORM_HOB_GUID EFI_GUID_DEF(0x7a1f3b10, 0x86f0, 0x4e2a, 0x9b, 0x61, 0x76, 0x69, 0x62, 0x65, 0x38, 0x36)
#define PLATFORM_MAX_RAM	16
typedef struct {
	UINT64 tsc_at_reset;		/* SEC's first TSC reading */
	UINT64 tsc_at_dxe;		/* PEI's last */
	UINT32 fwcfg;			/* QEMU fw_cfg interface present */
	UINT32 nram;
	struct {
		UINT64 base, length;
	} ram[PLATFORM_MAX_RAM];	/* the RAM the platform reported (e820) */
	UINT64 ram_below_4g;		/* bytes */
	UINT64 ram_above_4g;
	char cpu_brand[49];
} VIBE86_PLATFORM_INFO;

/* EDK2-compatible names for the memory allocation HOBs of the stack and
   the DXE core (PI 1.8 Vol. 3 5.4.2-5.4.3) */
#define EFI_HOB_MEMORY_ALLOC_STACK_GUID EFI_GUID_DEF(0x4ed4bf27, 0x4092, 0x42e9, 0x80, 0x7d, 0x52, 0x7b, 0x1d, 0x00, 0xc9, 0xbd)
#define EFI_HOB_MEMORY_ALLOC_MODULE_GUID EFI_GUID_DEF(0xf8e21975, 0x0899, 0x4f58, 0xa4, 0xbe, 0x55, 0x25, 0xa9, 0xc6, 0xd7, 0x7a)

/* the firmware's FFS files (fw/tools/mkfd.py names them) */
#define FILE_DXE_CORE_GUID EFI_GUID_DEF(0xd6a2cb7f, 0x6a18, 0x4e2f, 0xb4, 0x3c, 0x76, 0x62, 0x38, 0x36, 0x00, 0x01)
#define FILE_ACPI_TABLES_GUID EFI_GUID_DEF(0xd6a2cb7f, 0x6a18, 0x4e2f, 0xb4, 0x3c, 0x76, 0x62, 0x38, 0x36, 0x00, 0x10)
#define FILE_ACPI_RSDP_GUID EFI_GUID_DEF(0xd6a2cb7f, 0x6a18, 0x4e2f, 0xb4, 0x3c, 0x76, 0x62, 0x38, 0x36, 0x00, 0x11)
#define FILE_ACPI_LOADER_GUID EFI_GUID_DEF(0xd6a2cb7f, 0x6a18, 0x4e2f, 0xb4, 0x3c, 0x76, 0x62, 0x38, 0x36, 0x00, 0x12)
#define FILE_SHELL_GUID EFI_GUID_DEF(0xd6a2cb7f, 0x6a18, 0x4e2f, 0xb4, 0x3c, 0x76, 0x62, 0x38, 0x36, 0x00, 0x20)

/* POST codes on port 80h (vsim -t, -v) */
#define POST_SEC_START		0x01
#define POST_PEI_START		0x10
#define POST_PEI_MEMORY		0x11
#define POST_PEI_DXE_LOAD	0x12
#define POST_PEI_FATAL		0x1F
#define POST_DXE_START		0x20
#define POST_DXE_DRIVERS	0x21
#define POST_DXE_EXCEPTION	0x2F
#define POST_BDS_START		0x30
#define POST_BDS_SETUP		0x31
#define POST_BDS_BOOT		0x32
#define POST_BDS_SHELL		0x33
#define POST_EXIT_BOOT_SERVICES	0x40

#endif
