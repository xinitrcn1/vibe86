/* uefilib: the DXE drivers' and applications' helpers (uefilib.h) */
#include "uefilib.h"

EFI_HANDLE gImageHandle;
EFI_SYSTEM_TABLE *gST;
EFI_BOOT_SERVICES *gBS;
EFI_RUNTIME_SERVICES *gRT;

EFI_GUID gEfiDevicePathProtocolGuid = EFI_DEVICE_PATH_PROTOCOL_GUID;
EFI_GUID gEfiLoadedImageProtocolGuid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
EFI_GUID gEfiSimpleTextInProtocolGuid = EFI_SIMPLE_TEXT_INPUT_PROTOCOL_GUID;
EFI_GUID gEfiSimpleTextOutProtocolGuid = EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL_GUID;
EFI_GUID gEfiSimplePointerProtocolGuid = EFI_SIMPLE_POINTER_PROTOCOL_GUID;
EFI_GUID gEfiGraphicsOutputProtocolGuid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
EFI_GUID gEfiBlockIoProtocolGuid = EFI_BLOCK_IO_PROTOCOL_GUID;
EFI_GUID gEfiDiskIoProtocolGuid = EFI_DISK_IO_PROTOCOL_GUID;
EFI_GUID gEfiSimpleFileSystemProtocolGuid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
EFI_GUID gEfiFileInfoGuid = EFI_FILE_INFO_ID;
EFI_GUID gEfiFileSystemInfoGuid = EFI_FILE_SYSTEM_INFO_ID;
EFI_GUID gEfiSerialIoProtocolGuid = EFI_SERIAL_IO_PROTOCOL_GUID;
EFI_GUID gEfiPciIoProtocolGuid = EFI_PCI_IO_PROTOCOL_GUID;
EFI_GUID gEfiGlobalVariableGuid = EFI_GLOBAL_VARIABLE;

void
uefi_init(EFI_HANDLE image, EFI_SYSTEM_TABLE *st)
{
	gImageHandle = image;
	gST = st;
	gBS = st->BootServices;
	gRT = st->RuntimeServices;
}

void *
malloc(size_t n)
{
	void *p;
	if (EFI_ERROR(gBS->AllocatePool(EfiBootServicesData, n ? n : 1, &p)))
		return 0;
	return p;
}

void *
calloc(size_t n, size_t m)
{
	void *p = malloc(n * m);
	if (p)
		memset(p, 0, n * m);
	return p;
}

void *
rt_calloc(size_t n)
{
	void *p;
	if (EFI_ERROR(gBS->AllocatePool(EfiRuntimeServicesData, n ? n : 1, &p)))
		return 0;
	memset(p, 0, n);
	return p;
}

void
free(void *p)
{
	if (p)
		gBS->FreePool(p);
}

void *
dma_alloc_pages(UINTN pages)
{
	EFI_PHYSICAL_ADDRESS a = 0xFFFFFFFF;
	if (EFI_ERROR(gBS->AllocatePages(AllocateMaxAddress, EfiBootServicesData, pages, &a)))
		return 0;
	memset((void *)(uintptr_t)a, 0, pages * EFI_PAGE_SIZE);
	return (void *)(uintptr_t)a;
}

struct pbuf {
	CHAR16 b[128];
	unsigned n;
};

static void
pflush(struct pbuf *p)
{
	p->b[p->n] = 0;
	if (p->n && gST && gST->ConOut)
		gST->ConOut->OutputString(gST->ConOut, p->b);
	p->n = 0;
}

static void
pout(void *ctx, int c)
{
	struct pbuf *p = ctx;
	if (c == '\n') {
		if (p->n + 2 >= ARRAY_SIZE(p->b))
			pflush(p);
		p->b[p->n++] = '\r';
	}
	p->b[p->n++] = (CHAR16)c;
	if (p->n + 2 >= ARRAY_SIZE(p->b))
		pflush(p);
}

void
vPrint(const char *fmt, va_list ap)
{
	struct pbuf p;
	p.n = 0;
	fmt_format(pout, &p, fmt, ap);
	pflush(&p);
}

void
Print(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vPrint(fmt, ap);
	va_end(ap);
}

EFI_DEVICE_PATH_PROTOCOL *
dp_dup(const EFI_DEVICE_PATH_PROTOCOL *p)
{
	UINTN n = dp_size(p);
	EFI_DEVICE_PATH_PROTOCOL *d = n ? malloc(n) : 0;
	if (d)
		memcpy(d, p, n);
	return d;
}

EFI_DEVICE_PATH_PROTOCOL *
dp_append_node(const EFI_DEVICE_PATH_PROTOCOL *p, const EFI_DEVICE_PATH_PROTOCOL *node)
{
	UINTN a = p ? dp_size(p) - 4 : 0, b = dp_node_len(node);
	UINT8 *d = malloc(a + b + 4);
	if (!d)
		return 0;
	if (a)
		memcpy(d, p, a);
	memcpy(d + a, node, b);
	dp_set_end((EFI_DEVICE_PATH_PROTOCOL *)(d + a + b));
	return (EFI_DEVICE_PATH_PROTOCOL *)d;
}

EFI_DEVICE_PATH_PROTOCOL *
dp_append_file(const EFI_DEVICE_PATH_PROTOCOL *p, const CHAR16 *file)
{
	UINTN len = (wcslen(file) + 1) * 2;
	FILEPATH_DEVICE_PATH *n = malloc(4 + len);
	if (!n)
		return 0;
	dp_set_node(&n->Header, MEDIA_DEVICE_PATH, MEDIA_FILEPATH_DP, 4 + len);
	memcpy(n->PathName, file, len);
	EFI_DEVICE_PATH_PROTOCOL *r = dp_append_node(p, &n->Header);
	free(n);
	return r;
}

EFI_DEVICE_PATH_PROTOCOL *
dp_of(EFI_HANDLE h)
{
	EFI_DEVICE_PATH_PROTOCOL *p;
	if (EFI_ERROR(gBS->HandleProtocol(h, &gEfiDevicePathProtocolGuid, (void **)&p)))
		return 0;
	return p;
}
