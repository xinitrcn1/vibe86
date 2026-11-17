/*
 * devname.h: a firmware-private protocol that gives a device a display
 * name for the boot manager and setup ("USB Floppy: vibe86 USB Floppy",
 * "QEMU fw_cfg volume"). The interface is the NUL-terminated ASCII name.
 */
#ifndef DEVNAME_H
#define DEVNAME_H

#include "efi.h"

#define VIBE86_DEVICE_NAME_GUID EFI_GUID_DEF(0x3f0c9a71, 0x5b2e, 0x4c6d, 0x9e, 0x11, 0x76, 0x62, 0x64, 0x65, 0x76, 0x6e)

typedef struct {
	char name[64];
} VIBE86_DEVICE_NAME;

/* the vendor GUID of the fw_cfg volume's device path node */
#define VIBE86_FWCFG_VOLUME_GUID EFI_GUID_DEF(0x3f0c9a72, 0x5b2e, 0x4c6d, 0x9e, 0x11, 0x66, 0x77, 0x63, 0x66, 0x67, 0x00)

/* the USB driver's summary for the POST screen (installed once) */
#define VIBE86_USB_INFO_GUID EFI_GUID_DEF(0x3f0c9a73, 0x5b2e, 0x4c6d, 0x9e, 0x11, 0x75, 0x73, 0x62, 0x69, 0x6e, 0x66)
typedef struct {
	UINT32 controllers, keyboards, mice, storage, hubs, other;
	char lines[8][64];		/* one per device, for setup */
	UINT32 nlines;
} VIBE86_USB_INFO;

#endif
