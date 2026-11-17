/*
 * vibe86csm.h: the firmware-private protocol of CsmDxe, the Compatibility
 * Support Module (fw/dxe/csm), which the boot manager uses for legacy
 * boot options (the role of EDK2's EFI_LEGACY_BIOS_PROTOCOL.LegacyBoot).
 */
#ifndef VIBE86CSM_H
#define VIBE86CSM_H

#include "efi.h"

#define VIBE86_CSM_PROTOCOL_GUID EFI_GUID_DEF(0x3f0c9a74, 0x5b2e, 0x4c6d, 0x9e, 0x11, 0x63, 0x73, 0x6d, 0x31, 0x36, 0x00)
#define EFI_EVENT_LEGACY_BOOT_GUID EFI_GUID_DEF(0x2a571201, 0x4966, 0x47f6, 0x8b, 0x86, 0xf3, 0x1e, 0x41, 0xf3, 0x2f, 0x10)
/* the 16-bit image's FFS file (fw/tools/mkfd.py, raw) */
#define FILE_CSM16_GUID EFI_GUID_DEF(0xd6a2cb7f, 0x6a18, 0x4e2f, 0xb4, 0x3c, 0x76, 0x62, 0x38, 0x36, 0x00, 0x30)

typedef struct VIBE86_CSM_PROTOCOL {
	/* 1 if the device's first block is a legacy boot sector (55h AAh) */
	BOOLEAN (EFIAPI *Bootable)(struct VIBE86_CSM_PROTOCOL *This, EFI_HANDLE BlockIoHandle);
	/* boot it in real mode; returns only on an error */
	EFI_STATUS (EFIAPI *LegacyBoot)(struct VIBE86_CSM_PROTOCOL *This, EFI_HANDLE BlockIoHandle);
} VIBE86_CSM_PROTOCOL;

#endif
