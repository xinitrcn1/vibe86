/*
 * DXE core: image services (UEFI 2.10 7.4) and the driver dispatcher.
 *
 * LoadImage takes PE32+ images from a buffer, from the firmware volume
 * (an Fv()/FvFile() device path, PI 1.8 Vol. 3 3.2.2) or from a file on a
 * Simple File System (a media file path node). StartImage remembers the
 * caller's context so that Exit can return to it from any depth.
 *
 * The dispatcher (PI 1.8 Vol. 2 ch. 10, simplified) starts the DRIVER files
 * of FV_MAIN in volume order; mkfd.py places them in dependency order, so
 * no dependency expressions are evaluated.
 */
#include "dxe.h"

#define IMAGE_MAGIC	0x474D4949u	/* "IIMG" */

struct image {
	uint32_t magic;
	list_t link;
	EFI_LOADED_IMAGE_PROTOCOL li;
	EFI_HANDLE handle;
	EFI_DEVICE_PATH_PROTOCOL *full_path;	/* LoadedImageDevicePath */
	uint64_t base, pages, entry;
	UINT16 subsystem;
	int started, running;
	jmp_ctx exit_ctx;
	EFI_STATUS exit_status;
	UINTN exit_data_size;
	CHAR16 *exit_data;
	char name[48];
};

static list_t images = { &images, &images };
static EFI_GUID li_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
static EFI_GUID lidp_guid = EFI_LOADED_IMAGE_DEVICE_PATH_PROTOCOL_GUID;
static EFI_GUID dp_guid = EFI_DEVICE_PATH_PROTOCOL_GUID;
static EFI_GUID sfs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
static EFI_GUID finfo_guid = EFI_FILE_INFO_ID;

static struct image *
image_of(EFI_HANDLE h)
{
	EFI_LOADED_IMAGE_PROTOCOL *li;
	if (EFI_ERROR(core_handle_protocol(h, &li_guid, (void **)&li)))
		return 0;
	struct image *im = CONTAINER_OF(li, struct image, li);
	return im->magic == IMAGE_MAGIC ? im : 0;
}

const char *
image_name_of(uint64_t rip, uint64_t *base)
{
	list_for_each(it, &images) {
		struct image *im = CONTAINER_OF(it, struct image, link);
		if (rip >= im->base && rip < im->base + im->pages * EFI_PAGE_SIZE) {
			*base = im->base;
			return im->name;
		}
	}
	return 0;
}

static EFI_DEVICE_PATH_PROTOCOL *
dp_dup(const EFI_DEVICE_PATH_PROTOCOL *p)
{
	UINTN n = dp_size(p);
	EFI_DEVICE_PATH_PROTOCOL *d = n ? core_alloc(n) : 0;
	if (d)
		memcpy(d, p, n);
	return d;
}

/* ---- finding an image's bytes from a device path */

static const EFI_GUID fv_main_name = EFI_FIRMWARE_FILE_SYSTEM2_GUID;

static EFI_STATUS
read_fv_file(const EFI_GUID *name, const void **buf, UINTN *size)
{
	const EFI_FIRMWARE_VOLUME_HEADER *fv = fv_check(FV_MAIN_BASE);
	uint64_t cookie = 0;
	const EFI_FFS_FILE_HEADER *f;
	if (!fv)
		return EFI_NOT_FOUND;
	while ((f = fv_next_file(fv, &cookie)))
		if (guid_eq(&f->Name, name)) {
			UINT32 s;
			const void *pe = ffs_section(f, EFI_SECTION_PE32, &s);
			if (!pe)
				return EFI_LOAD_ERROR;
			*buf = pe;
			*size = s;
			return EFI_SUCCESS;
		}
	return EFI_NOT_FOUND;
}

static EFI_STATUS
read_file(EFI_DEVICE_PATH_PROTOCOL *path, void **buf, UINTN *size, EFI_HANDLE *dev)
{
	EFI_DEVICE_PATH_PROTOCOL *rest = path;
	EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *sfs;
	EFI_FILE_PROTOCOL *root, *f;
	EFI_STATUS s = core_locate_device_path(&sfs_guid, &rest, dev);
	if (EFI_ERROR(s))
		return EFI_NOT_FOUND;
	if (EFI_ERROR(s = core_handle_protocol(*dev, &sfs_guid, (void **)&sfs)))
		return s;
	/* the file path nodes, concatenated */
	CHAR16 name[260];
	UINTN n = 0;
	for (; !dp_is_end(rest); rest = dp_next(rest)) {
		if (rest->Type != MEDIA_DEVICE_PATH || rest->SubType != MEDIA_FILEPATH_DP)
			return EFI_NOT_FOUND;
		const CHAR16 *p = ((FILEPATH_DEVICE_PATH *)rest)->PathName;
		UINTN len = (dp_node_len(rest) - 4) / 2;
		if (n && name[n - 1] != '\\' && p[0] != '\\' && n < ARRAY_SIZE(name) - 1)
			name[n++] = '\\';
		for (UINTN i = 0; i < len && p[i] && n < ARRAY_SIZE(name) - 1; i++)
			name[n++] = p[i];
	}
	name[n] = 0;
	if (!n)
		return EFI_NOT_FOUND;
	if (EFI_ERROR(s = sfs->OpenVolume(sfs, &root)))
		return s;
	s = root->Open(root, &f, name, EFI_FILE_MODE_READ, 0);
	root->Close(root);
	if (EFI_ERROR(s))
		return s;
	UINT8 ibuf[SIZE_OF_EFI_FILE_INFO + 520];
	UINTN isz = sizeof ibuf;
	if (EFI_ERROR(s = f->GetInfo(f, &finfo_guid, &isz, ibuf))) {
		f->Close(f);
		return s;
	}
	UINTN fsize = ((EFI_FILE_INFO *)ibuf)->FileSize;
	void *b;
	if (EFI_ERROR(core_allocate_pool(EfiBootServicesData, fsize ? fsize : 1, &b))) {
		f->Close(f);
		return EFI_OUT_OF_RESOURCES;
	}
	UINTN got = fsize;
	s = f->Read(f, &got, b);
	f->Close(f);
	if (EFI_ERROR(s) || got != fsize) {
		core_free_pool(b);
		return EFI_ERROR(s) ? s : EFI_DEVICE_ERROR;
	}
	*buf = b;
	*size = fsize;
	return EFI_SUCCESS;
}

/* ---- the image services */

EFI_STATUS
image_load_buffer(EFI_HANDLE parent, EFI_DEVICE_PATH_PROTOCOL *path, const void *buf, UINTN size,
    EFI_HANDLE *out)
{
	const IMAGE_NT_HEADERS64 *nt = pe_headers(buf, size);
	if (!nt)
		return EFI_LOAD_ERROR;
	UINT16 sub = nt->OptionalHeader.Subsystem;
	EFI_MEMORY_TYPE ct, dt;
	switch (sub) {
	case IMAGE_SUBSYSTEM_EFI_APPLICATION:
		ct = EfiLoaderCode, dt = EfiLoaderData;
		break;
	case IMAGE_SUBSYSTEM_EFI_BOOT_SERVICE_DRIVER:
		ct = EfiBootServicesCode, dt = EfiBootServicesData;
		break;
	case IMAGE_SUBSYSTEM_EFI_RUNTIME_DRIVER:
		ct = EfiRuntimeServicesCode, dt = EfiRuntimeServicesData;
		break;
	default:
		return EFI_UNSUPPORTED;
	}
	UINTN pages = EFI_SIZE_TO_PAGES(nt->OptionalHeader.SizeOfImage);
	EFI_PHYSICAL_ADDRESS a;
	if (EFI_ERROR(core_allocate_pages(AllocateAnyPages, ct, pages, &a)))
		return EFI_OUT_OF_RESOURCES;
	uint64_t entry;
	if (pe_load(buf, size, (void *)(uintptr_t)a, &entry)) {
		core_free_pages(a, pages);
		return EFI_LOAD_ERROR;
	}
	struct image *im = core_alloc(sizeof *im);
	if (!im) {
		core_free_pages(a, pages);
		return EFI_OUT_OF_RESOURCES;
	}
	im->magic = IMAGE_MAGIC;
	im->base = a;
	im->pages = pages;
	im->entry = entry;
	im->subsystem = sub;
	im->li.Revision = EFI_LOADED_IMAGE_PROTOCOL_REVISION;
	im->li.ParentHandle = parent;
	im->li.SystemTable = gST;
	im->li.ImageBase = (void *)(uintptr_t)a;
	im->li.ImageSize = nt->OptionalHeader.SizeOfImage;
	im->li.ImageCodeType = ct;
	im->li.ImageDataType = dt;
	/* DeviceHandle and FilePath: the device part of the path and the rest */
	if (path) {
		EFI_DEVICE_PATH_PROTOCOL *rest = path;
		EFI_HANDLE dev;
		im->full_path = dp_dup(path);
		if (core_locate_device_path(&dp_guid, &rest, &dev) == EFI_SUCCESS) {
			im->li.DeviceHandle = dev;
			im->li.FilePath = dp_dup(rest);
		} else
			im->li.FilePath = dp_dup(path);
		dp_to_text(path, im->name, sizeof im->name);
	}
	list_add_tail(&images, &im->link);
	EFI_STATUS s = core_install_protocol(&im->handle, &li_guid, EFI_NATIVE_INTERFACE, &im->li);
	if (!EFI_ERROR(s) && im->full_path)
		core_install_protocol(&im->handle, &lidp_guid, EFI_NATIVE_INTERFACE, im->full_path);
	*out = im->handle;
	DEBUG("DXE: loaded %s at %lx (entry %lx)\n", im->name[0] ? im->name : "image", a, entry);
	return s;
}

static EFI_STATUS EFIAPI
core_load_image(BOOLEAN BootPolicy, EFI_HANDLE ParentImageHandle, EFI_DEVICE_PATH_PROTOCOL *DevicePath,
    VOID *SourceBuffer, UINTN SourceSize, EFI_HANDLE *ImageHandle)
{
	if (!ImageHandle || !handle_valid(ParentImageHandle))
		return EFI_INVALID_PARAMETER;
	if (SourceBuffer)
		return image_load_buffer(ParentImageHandle, DevicePath, SourceBuffer, SourceSize, ImageHandle);
	if (!DevicePath)
		return EFI_INVALID_PARAMETER;
	/* an FvFile() node: a file of the firmware volume */
	for (EFI_DEVICE_PATH_PROTOCOL *n = DevicePath; !dp_is_end(n); n = dp_next(n))
		if (n->Type == MEDIA_DEVICE_PATH && n->SubType == MEDIA_PIWG_FW_FILE_DP) {
			const void *b;
			UINTN sz;
			EFI_STATUS s = read_fv_file(&((MEDIA_FW_VOL_FILEPATH_DEVICE_PATH *)n)->FvFileName, &b, &sz);
			if (EFI_ERROR(s))
				return s;
			return image_load_buffer(ParentImageHandle, DevicePath, b, sz, ImageHandle);
		}
	void *b;
	UINTN sz;
	EFI_HANDLE dev;
	EFI_STATUS s = read_file(DevicePath, &b, &sz, &dev);
	if (EFI_ERROR(s))
		return s;
	s = image_load_buffer(ParentImageHandle, DevicePath, b, sz, ImageHandle);
	core_free_pool(b);
	return s;
}

static void
unload(struct image *im)
{
	DEBUG("DXE: unloading %s\n", im->name);
	list_del(&im->link);
	handle_remove_all(im->handle);
	core_free_pages(im->base, im->pages);
	core_free(im->full_path);
	core_free(im->li.FilePath);
	im->magic = 0;
	core_free(im);
}

EFI_STATUS EFIAPI
core_start_image(EFI_HANDLE ImageHandle, UINTN *ExitDataSize, CHAR16 **ExitData)
{
	struct image *im = image_of(ImageHandle);
	if (!im || im->started)
		return EFI_INVALID_PARAMETER;
	im->started = im->running = 1;
	jmp_ctx saved = im->exit_ctx;
	EFI_STATUS s;
	if (core_setjmp(&im->exit_ctx) == 0) {
		EFI_IMAGE_ENTRY_POINT ep = (EFI_IMAGE_ENTRY_POINT)(uintptr_t)im->entry;
		s = ep(ImageHandle, gST);
		im->exit_status = s;
		im->exit_data_size = 0;
		im->exit_data = 0;
	}
	/* returned or exited */
	(void)saved;
	im->running = 0;
	s = im->exit_status;
	if (ExitDataSize)
		*ExitDataSize = im->exit_data_size;
	if (ExitData)
		*ExitData = im->exit_data;
	if (im->subsystem == IMAGE_SUBSYSTEM_EFI_APPLICATION || EFI_ERROR(s))
		unload(im);
	return s;
}

static EFI_STATUS EFIAPI
core_exit(EFI_HANDLE ImageHandle, EFI_STATUS ExitStatus, UINTN ExitDataSize, CHAR16 *ExitData)
{
	struct image *im = image_of(ImageHandle);
	if (!im)
		return EFI_INVALID_PARAMETER;
	if (!im->started) {
		unload(im);
		return EFI_SUCCESS;
	}
	if (!im->running)
		return EFI_INVALID_PARAMETER;
	im->exit_status = ExitStatus;
	im->exit_data_size = ExitData ? ExitDataSize : 0;
	im->exit_data = ExitData;
	core_longjmp(&im->exit_ctx, 1);
}

static EFI_STATUS EFIAPI
core_unload_image(EFI_HANDLE ImageHandle)
{
	struct image *im = image_of(ImageHandle);
	if (!im)
		return EFI_INVALID_PARAMETER;
	if (im->started) {
		if (!im->li.Unload)
			return EFI_UNSUPPORTED;
		EFI_STATUS s = im->li.Unload(ImageHandle);
		if (EFI_ERROR(s))
			return s;
	}
	unload(im);
	return EFI_SUCCESS;
}

void
image_init_services(EFI_BOOT_SERVICES *bs)
{
	bs->LoadImage = core_load_image;
	bs->StartImage = core_start_image;
	bs->Exit = core_exit;
	bs->UnloadImage = core_unload_image;
}

/* the DXE core itself is an image too (its handle is gDxeCoreImage) */
void
image_init(const void *hobs)
{
	static const EFI_GUID module_guid = EFI_HOB_MEMORY_ALLOC_MODULE_GUID;
	for (const EFI_HOB_MEMORY_ALLOCATION_MODULE *m = hob_next(hobs, EFI_HOB_TYPE_MEMORY_ALLOCATION); m;
	    m = hob_next((const UINT8 *)m + m->Header.HobLength, EFI_HOB_TYPE_MEMORY_ALLOCATION)) {
		if (!guid_eq(&m->MemoryAllocationHeader.Name, &module_guid))
			continue;
		struct image *im = core_alloc(sizeof *im);
		im->magic = IMAGE_MAGIC;
		im->base = m->MemoryAllocationHeader.MemoryBaseAddress;
		im->pages = m->MemoryAllocationHeader.MemoryLength / EFI_PAGE_SIZE;
		im->entry = m->EntryPoint;
		im->subsystem = IMAGE_SUBSYSTEM_EFI_BOOT_SERVICE_DRIVER;
		im->started = im->running = 1;
		im->li.Revision = EFI_LOADED_IMAGE_PROTOCOL_REVISION;
		im->li.SystemTable = gST;
		im->li.ImageBase = (void *)(uintptr_t)im->base;
		im->li.ImageSize = m->MemoryAllocationHeader.MemoryLength;
		im->li.ImageCodeType = EfiBootServicesCode;
		im->li.ImageDataType = EfiBootServicesData;
		strncpy0(im->name, "DxeCore", sizeof im->name);
		list_add_tail(&images, &im->link);
		core_install_protocol(&im->handle, &li_guid, EFI_NATIVE_INTERFACE, &im->li);
		gDxeCoreImage = im->handle;
		return;
	}
}

/* ---- the dispatcher */

#pragma pack(push, 1)
struct fv_file_path {
	MEDIA_FW_VOL_DEVICE_PATH fv;
	MEDIA_FW_VOL_FILEPATH_DEVICE_PATH file;
	EFI_DEVICE_PATH_PROTOCOL end;
};
#pragma pack(pop)

void
dispatch_drivers(const void *hobs)
{
	const EFI_FIRMWARE_VOLUME_HEADER *fv = fv_check(FV_MAIN_BASE);
	uint64_t cookie = 0;
	const EFI_FFS_FILE_HEADER *f;
	(void)hobs;
	if (!fv) {
		DEBUG("DXE: no FV_MAIN\n");
		return;
	}
	while ((f = fv_next_file(fv, &cookie))) {
		if (f->Type != EFI_FV_FILETYPE_DRIVER)
			continue;
		UINT32 size, usize;
		const void *pe = ffs_section(f, EFI_SECTION_PE32, &size);
		const CHAR16 *ui = ffs_section(f, EFI_SECTION_USER_INTERFACE, &usize);
		char name[48];
		if (ui)
			ascii_from_wcs(name, ui, sizeof name);
		else
			snprintf(name, sizeof name, "%g", &f->Name);
		if (!pe)
			continue;
		struct fv_file_path *dp = core_alloc(sizeof *dp);
		dp_set_node(&dp->fv.Header, MEDIA_DEVICE_PATH, MEDIA_PIWG_FW_VOL_DP, sizeof dp->fv);
		dp->fv.FvName = fv_main_name;
		dp_set_node(&dp->file.Header, MEDIA_DEVICE_PATH, MEDIA_PIWG_FW_FILE_DP, sizeof dp->file);
		dp->file.FvFileName = f->Name;
		dp_set_end(&dp->end);
		EFI_HANDLE h;
		EFI_STATUS s = image_load_buffer(gDxeCoreImage, (EFI_DEVICE_PATH_PROTOCOL *)dp, pe, size, &h);
		core_free(dp);
		if (EFI_ERROR(s)) {
			DEBUG("DXE: %s: load failed: %r\n", name, s);
			continue;
		}
		struct image *im = image_of(h);
		strncpy0(im->name, name, sizeof im->name);
		s = core_start_image(h, 0, 0);
		DEBUG("DXE: %s: %r\n", name, s);
	}
}
