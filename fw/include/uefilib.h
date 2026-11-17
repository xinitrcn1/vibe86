/*
 * uefilib.h: what the firmware's DXE drivers and applications share on top
 * of fwlib: the image's handle and tables, pool allocation, console
 * printing, protocol and device path helpers (fw/lib/uefi.c).
 */
#ifndef UEFILIB_H
#define UEFILIB_H

#include "fwlib.h"
#include "efi.h"

extern EFI_HANDLE gImageHandle;
extern EFI_SYSTEM_TABLE *gST;
extern EFI_BOOT_SERVICES *gBS;
extern EFI_RUNTIME_SERVICES *gRT;

void uefi_init(EFI_HANDLE image, EFI_SYSTEM_TABLE *st);

void *malloc(size_t n);			/* boot-services data */
void *calloc(size_t n, size_t m);
void *rt_calloc(size_t n);		/* runtime-services data */
void free(void *p);
void *dma_alloc_pages(UINTN pages);	/* page-aligned, zeroed, below 4 GiB */

/* print to the console (ConOut), ASCII format (fwlib print.c) */
void Print(const char *fmt, ...);
void vPrint(const char *fmt, va_list ap);

/* device paths */
EFI_DEVICE_PATH_PROTOCOL *dp_append_node(const EFI_DEVICE_PATH_PROTOCOL *p, const EFI_DEVICE_PATH_PROTOCOL *node);
EFI_DEVICE_PATH_PROTOCOL *dp_append_file(const EFI_DEVICE_PATH_PROTOCOL *p, const CHAR16 *file);
EFI_DEVICE_PATH_PROTOCOL *dp_dup(const EFI_DEVICE_PATH_PROTOCOL *p);
EFI_DEVICE_PATH_PROTOCOL *dp_of(EFI_HANDLE h);	/* the handle's device path or NULL */

/* GUIDs every module uses */
extern EFI_GUID gEfiDevicePathProtocolGuid;
extern EFI_GUID gEfiLoadedImageProtocolGuid;
extern EFI_GUID gEfiSimpleTextInProtocolGuid;
extern EFI_GUID gEfiSimpleTextOutProtocolGuid;
extern EFI_GUID gEfiSimplePointerProtocolGuid;
extern EFI_GUID gEfiGraphicsOutputProtocolGuid;
extern EFI_GUID gEfiBlockIoProtocolGuid;
extern EFI_GUID gEfiDiskIoProtocolGuid;
extern EFI_GUID gEfiSimpleFileSystemProtocolGuid;
extern EFI_GUID gEfiFileInfoGuid;
extern EFI_GUID gEfiFileSystemInfoGuid;
extern EFI_GUID gEfiSerialIoProtocolGuid;
extern EFI_GUID gEfiPciIoProtocolGuid;
extern EFI_GUID gEfiGlobalVariableGuid;

#endif
