/*
 * FwCfgFsDxe: a read-only Simple File System (UEFI 2.10 13.4-13.5) over the
 * files the platform offers through QEMU's fw_cfg interface (QEMU
 * docs/specs/fw_cfg.rst) under "opt/vibe86/fs/", the "opt/" name space
 * being the one fw_cfg reserves for users. vsim serves them with
 *
 *     vsim -f opt/vibe86/fs/EFI/BOOT/BOOTX64.EFI=work/csmwrapx64.efi ...
 *
 * so a loader can be handed to the firmware without a disk image, as OVMF's
 * QemuKernelLoaderFsDxe does for -kernel. Directories are implied by the
 * names' slashes. The volume's device path is VenHw(fw_cfg volume GUID).
 */
#include "uefilib.h"
#include "devname.h"

#define PREFIX		"opt/vibe86/fs/"
#define MAX_ENTRIES	32

struct ent {
	char path[56];		/* below PREFIX, '/' separated */
	uint32_t size;
	uint16_t sel;
};

static struct ent ents[MAX_ENTRIES];
static unsigned nents;

struct file {
	EFI_FILE_PROTOCOL fp;
	char path[56];		/* "" = root; directories have no trailing slash */
	int dir;
	const struct ent *e;
	uint8_t *data;		/* the file's bytes, read at open */
	uint64_t pos;
	unsigned dir_next;	/* directories: the next child to return */
};

static EFI_FILE_PROTOCOL file_template;

static int
eqi(const char *a, const char *b, size_t n)
{
	for (size_t i = 0; i < n; i++)
		if (to_upper((unsigned char)a[i]) != to_upper((unsigned char)b[i]))
			return 0;
	return 1;
}

/* is `path` a directory (some entry below it)? */
static int
is_dir(const char *path)
{
	size_t n = strlen(path);
	if (!n)
		return 1;
	for (unsigned i = 0; i < nents; i++)
		if (strlen(ents[i].path) > n && eqi(ents[i].path, path, n) && ents[i].path[n] == '/')
			return 1;
	return 0;
}

static const struct ent *
find(const char *path)
{
	for (unsigned i = 0; i < nents; i++)
		if (strlen(ents[i].path) == strlen(path) && eqi(ents[i].path, path, strlen(path)))
			return &ents[i];
	return 0;
}

/* the k-th distinct child name of directory `dir`: its name, and whether it
   is a directory; 0 past the end */
static int
child(const char *dir, unsigned k, char *name, int *isdir, const struct ent **e)
{
	size_t n = strlen(dir);
	unsigned idx = 0;
	for (unsigned i = 0; i < nents; i++) {
		const char *p = ents[i].path;
		if (n) {
			if (strlen(p) <= n || !eqi(p, dir, n) || p[n] != '/')
				continue;
			p += n + 1;
		}
		const char *slash = p;
		while (*slash && *slash != '/')
			slash++;
		size_t len = (size_t)(slash - p);
		/* skip a name an earlier entry already gave */
		int dup = 0;
		for (unsigned j = 0; j < i && !dup; j++) {
			const char *q = ents[j].path;
			if (n) {
				if (strlen(q) <= n || !eqi(q, dir, n) || q[n] != '/')
					continue;
				q += n + 1;
			}
			if (!strncmp(q, p, len) && (q[len] == '/' || !q[len]))
				dup = 1;
		}
		if (dup)
			continue;
		if (idx++ == k) {
			memcpy(name, p, len);
			name[len] = 0;
			*isdir = *slash == '/';
			*e = *slash ? 0 : &ents[i];
			return 1;
		}
	}
	return 0;
}

static struct file *
new_file(const char *path)
{
	struct file *f = calloc(1, sizeof *f);
	if (!f)
		return 0;
	f->fp = file_template;
	strncpy0(f->path, path, sizeof f->path);
	return f;
}

static EFI_STATUS EFIAPI
f_close(EFI_FILE_PROTOCOL *This)
{
	struct file *f = (struct file *)This;
	free(f->data);
	free(f);
	return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
f_open(EFI_FILE_PROTOCOL *This, EFI_FILE_PROTOCOL **New, CHAR16 *Name, UINT64 Mode, UINT64 Attr)
{
	struct file *cur = (struct file *)This;
	if (!New || !Name)
		return EFI_INVALID_PARAMETER;
	if (Mode & (EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE))
		return EFI_WRITE_PROTECTED;
	char path[56];
	strncpy0(path, Name[0] == '\\' ? "" : cur->path, sizeof path);
	for (const CHAR16 *p = Name; *p;) {
		while (*p == '\\')
			p++;
		char comp[56];
		unsigned n = 0;
		while (*p && *p != '\\' && n < sizeof comp - 1)
			comp[n++] = *p < 0x80 ? (char)*p++ : (p++, '?');
		comp[n] = 0;
		if (!n || !strcmp(comp, "."))
			continue;
		if (!strcmp(comp, "..")) {
			char *s = path + strlen(path);
			while (s > path && *s != '/')
				s--;
			*s = 0;
			continue;
		}
		size_t l = strlen(path);
		if (l + (l ? 1 : 0) + n >= sizeof path)
			return EFI_NOT_FOUND;
		if (l)
			path[l++] = '/';
		memcpy(path + l, comp, n + 1);
	}
	struct file *f = new_file(path);
	if (!f)
		return EFI_OUT_OF_RESOURCES;
	if (is_dir(path)) {
		f->dir = 1;
	} else if ((f->e = find(path))) {
		f->data = malloc(f->e->size ? f->e->size : 1);
		if (!f->data) {
			free(f);
			return EFI_OUT_OF_RESOURCES;
		}
		fwcfg_read(f->e->sel, f->data, f->e->size);
	} else {
		free(f);
		return EFI_NOT_FOUND;
	}
	*New = &f->fp;
	return EFI_SUCCESS;
}

static UINTN
fill_info(void *buf, UINTN size, const char *name, int dir, uint64_t fsize)
{
	UINTN need = SIZE_OF_EFI_FILE_INFO + (strlen(name) + 1) * 2;
	if (size < need)
		return need;
	EFI_FILE_INFO *i = buf;
	memset(i, 0, SIZE_OF_EFI_FILE_INFO);
	i->Size = need;
	i->FileSize = fsize;
	i->PhysicalSize = fsize;
	i->Attribute = EFI_FILE_READ_ONLY | (dir ? EFI_FILE_DIRECTORY : 0);
	wcs_from_ascii(i->FileName, name, strlen(name) + 1);
	return need;
}

static EFI_STATUS EFIAPI
f_read(EFI_FILE_PROTOCOL *This, UINTN *Size, VOID *Buf)
{
	struct file *f = (struct file *)This;
	if (!Size)
		return EFI_INVALID_PARAMETER;
	if (!f->dir) {
		uint64_t left = f->pos < f->e->size ? f->e->size - f->pos : 0;
		UINTN n = (UINTN)MIN(*Size, left);
		memcpy(Buf, f->data + f->pos, n);
		f->pos += n;
		*Size = n;
		return EFI_SUCCESS;
	}
	char name[56];
	int isdir;
	const struct ent *e;
	if (!child(f->path, f->dir_next, name, &isdir, &e)) {
		*Size = 0;
		return EFI_SUCCESS;
	}
	UINTN need = fill_info(Buf, *Size, name, isdir, e ? e->size : 0);
	if (need > *Size) {
		*Size = need;
		return EFI_BUFFER_TOO_SMALL;
	}
	f->dir_next++;
	*Size = need;
	return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
f_write(EFI_FILE_PROTOCOL *This, UINTN *Size, VOID *Buf)
{
	return EFI_WRITE_PROTECTED;
}

static EFI_STATUS EFIAPI
f_delete(EFI_FILE_PROTOCOL *This)
{
	f_close(This);
	return 2;	/* EFI_WARN_DELETE_FAILURE */
}

static EFI_STATUS EFIAPI
f_get_position(EFI_FILE_PROTOCOL *This, UINT64 *Pos)
{
	struct file *f = (struct file *)This;
	if (f->dir)
		return EFI_UNSUPPORTED;
	*Pos = f->pos;
	return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
f_set_position(EFI_FILE_PROTOCOL *This, UINT64 Pos)
{
	struct file *f = (struct file *)This;
	if (f->dir) {
		if (Pos)
			return EFI_UNSUPPORTED;
		f->dir_next = 0;
		return EFI_SUCCESS;
	}
	f->pos = Pos == ~0ull ? f->e->size : Pos;
	return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
f_get_info(EFI_FILE_PROTOCOL *This, EFI_GUID *Type, UINTN *Size, VOID *Buf)
{
	struct file *f = (struct file *)This;
	if (!Type || !Size)
		return EFI_INVALID_PARAMETER;
	if (guid_eq(Type, &gEfiFileInfoGuid)) {
		const char *name = f->path;
		for (const char *p = f->path; *p; p++)
			if (*p == '/')
				name = p + 1;
		UINTN need = fill_info(Buf, *Size, name, f->dir, f->dir ? 0 : f->e->size);
		if (need > *Size) {
			*Size = need;
			return EFI_BUFFER_TOO_SMALL;
		}
		*Size = need;
		return EFI_SUCCESS;
	}
	if (guid_eq(Type, &gEfiFileSystemInfoGuid)) {
		static const CHAR16 label[] = L"FWCFG";
		UINTN need = SIZE_OF_EFI_FILE_SYSTEM_INFO + sizeof label;
		if (*Size < need) {
			*Size = need;
			return EFI_BUFFER_TOO_SMALL;
		}
		EFI_FILE_SYSTEM_INFO *i = Buf;
		i->Size = need;
		i->ReadOnly = TRUE;
		i->VolumeSize = 0;
		for (unsigned k = 0; k < nents; k++)
			i->VolumeSize += ents[k].size;
		i->FreeSpace = 0;
		i->BlockSize = 1;
		memcpy(i->VolumeLabel, label, sizeof label);
		*Size = need;
		return EFI_SUCCESS;
	}
	return EFI_UNSUPPORTED;
}

static EFI_STATUS EFIAPI
f_set_info(EFI_FILE_PROTOCOL *This, EFI_GUID *Type, UINTN Size, VOID *Buf)
{
	return EFI_WRITE_PROTECTED;
}

static EFI_STATUS EFIAPI
f_flush(EFI_FILE_PROTOCOL *This)
{
	return EFI_SUCCESS;
}

static EFI_FILE_PROTOCOL file_template = {
	EFI_FILE_PROTOCOL_REVISION, f_open, f_close, f_delete, f_read, f_write, f_get_position,
	f_set_position, f_get_info, f_set_info, f_flush,
};

static EFI_STATUS EFIAPI
open_volume(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *This, EFI_FILE_PROTOCOL **Root)
{
	struct file *f = new_file("");
	if (!f)
		return EFI_OUT_OF_RESOURCES;
	f->dir = 1;
	*Root = &f->fp;
	return EFI_SUCCESS;
}

static EFI_SIMPLE_FILE_SYSTEM_PROTOCOL sfs = { EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_REVISION, open_volume };
static VIBE86_DEVICE_NAME name = { "QEMU fw_cfg volume" };

#pragma pack(push, 1)
static struct {
	VENDOR_DEVICE_PATH ven;
	EFI_DEVICE_PATH_PROTOCOL end;
} path;
#pragma pack(pop)

EFI_STATUS EFIAPI
efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st)
{
	uefi_init(image, st);
	if (!fwcfg_present())
		return EFI_NOT_FOUND;
	char n[56];
	uint32_t size;
	uint16_t sel;
	size_t pl = strlen(PREFIX);
	for (uint32_t i = 0; fwcfg_dir_entry(i, n, &size, &sel) == 0 && nents < MAX_ENTRIES; i++) {
		if (strncmp(n, PREFIX, pl) || !n[pl])
			continue;
		strncpy0(ents[nents].path, n + pl, sizeof ents[nents].path);
		ents[nents].size = size;
		ents[nents].sel = sel;
		DEBUG("FwCfgFsDxe: %s (%u bytes)\n", ents[nents].path, size);
		nents++;
	}
	if (!nents)
		return EFI_NOT_FOUND;	/* no volume to offer */
	static const EFI_GUID g = VIBE86_FWCFG_VOLUME_GUID;
	static EFI_GUID name_guid = VIBE86_DEVICE_NAME_GUID;
	dp_set_node(&path.ven.Header, HARDWARE_DEVICE_PATH, HW_VENDOR_DP, sizeof path.ven);
	path.ven.Guid = g;
	dp_set_end(&path.end);
	EFI_HANDLE h = 0;
	return gBS->InstallMultipleProtocolInterfaces(&h, &gEfiDevicePathProtocolGuid, &path,
	    &gEfiSimpleFileSystemProtocolGuid, &sfs, &name_guid, &name, (void *)0);
}
