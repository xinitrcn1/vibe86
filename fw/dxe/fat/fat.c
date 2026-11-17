/*
 * FatDxe: the Simple File System protocol (UEFI 2.10 13.4-13.5) on FAT12,
 * FAT16 and FAT32 volumes (Microsoft, "Microsoft Extensible Firmware
 * Initiative FAT32 File System Specification", version 1.03, 2000: the BPB
 * 3.1-3.3, the FAT type by cluster count 3.5, directory entries 6, long
 * names 7), as UEFI 2.10 13.3 requires of firmware for removable media.
 *
 * A Block I/O device holds a volume either directly (a "superfloppy", like
 * a floppy disk) or in the primary partitions of an MBR (UEFI 2.10 5.2.1);
 * partitions get a child handle with Block I/O and an HD() node. The file
 * system is read-only: writes return EFI_WRITE_PROTECTED.
 */
#include "uefilib.h"
#include "devname.h"

#define FAT_MAX_BYTES	(8u << 20)	/* the FAT is read whole: up to 8 MiB */

struct vol {
	EFI_SIMPLE_FILE_SYSTEM_PROTOCOL sfs;	/* first: This */
	EFI_BLOCK_IO_PROTOCOL *bio;
	UINT32 media_id;
	UINT64 start;			/* the volume's first block on the device */
	UINT32 bps, spc, cluster;	/* bytes per sector, sectors per cluster, bytes per cluster */
	int type;			/* 12, 16, 32 */
	UINT64 fat_sec, root_sec, data_sec, total_sec;
	UINT32 root_entries, root_cluster, nclusters;
	UINT8 *fat;
	char label[12];
	VIBE86_DEVICE_NAME name;
};

struct file {
	EFI_FILE_PROTOCOL fp;		/* first: This */
	struct vol *v;
	int dir, root;
	UINT32 first;			/* first cluster (0: FAT12/16 root) */
	UINT64 size, pos;
	UINT8 attr;
	CHAR16 name[256];
	EFI_TIME mtime;
	/* sequential-read cache: the cluster holding pos */
	UINT64 cpos;
	UINT32 ccl;
};

static EFI_FILE_PROTOCOL file_template;

/* ---- blocks */

static EFI_STATUS
read_bytes(struct vol *v, UINT64 off, void *buf, UINTN n)
{
	UINT32 bs = v->bio->Media->BlockSize;
	UINT8 tmp[4096];
	UINT8 *b = buf;
	if (bs > sizeof tmp)
		return EFI_UNSUPPORTED;
	while (n) {
		UINT64 lba = off / bs;
		UINTN o = off % bs;
		if (!o && n >= bs) {
			UINTN whole = n / bs * bs;
			EFI_STATUS s = v->bio->ReadBlocks(v->bio, v->media_id, v->start + lba, whole, b);
			if (EFI_ERROR(s))
				return s;
			b += whole;
			off += whole;
			n -= whole;
			continue;
		}
		EFI_STATUS s = v->bio->ReadBlocks(v->bio, v->media_id, v->start + lba, bs, tmp);
		if (EFI_ERROR(s))
			return s;
		UINTN k = MIN(n, bs - o);
		memcpy(b, tmp + o, k);
		b += k;
		off += k;
		n -= k;
	}
	return EFI_SUCCESS;
}

/* ---- the FAT */

static UINT32
fat_next(struct vol *v, UINT32 c)
{
	switch (v->type) {
	case 12: {
		UINT32 o = c + c / 2;
		UINT32 w = v->fat[o] | v->fat[o + 1] << 8;
		w = (c & 1) ? w >> 4 : w & 0xFFF;
		return w >= 0xFF8 ? 0x0FFFFFFF : w;
	}
	case 16: {
		UINT32 w = v->fat[2 * c] | v->fat[2 * c + 1] << 8;
		return w >= 0xFFF8 ? 0x0FFFFFFF : w;
	}
	default: {
		UINT32 w;
		memcpy(&w, v->fat + 4 * c, 4);
		return w & 0x0FFFFFFF;
	}
	}
}

static int
cluster_ok(struct vol *v, UINT32 c)
{
	return c >= 2 && c < v->nclusters + 2;
}

static UINT64
cluster_off(struct vol *v, UINT32 c)
{
	return (v->data_sec + (UINT64)(c - 2) * v->spc) * v->bps;
}

/*
 * Read n bytes at byte offset pos of a cluster chain (or of the fixed root
 * directory of FAT12/16). Returns the bytes read.
 */
static UINTN
chain_read(struct file *f, UINT64 pos, void *buf, UINTN n)
{
	struct vol *v = f->v;
	if (f->root && v->type != 32) {
		UINT64 size = (UINT64)v->root_entries * 32;
		if (pos >= size)
			return 0;
		n = (UINTN)MIN(n, size - pos);
		return EFI_ERROR(read_bytes(v, v->root_sec * v->bps + pos, buf, n)) ? 0 : n;
	}
	UINT32 c = f->first;
	UINT64 cp = 0;
	if (f->ccl && f->cpos <= pos) {		/* resume from the cached cluster */
		c = f->ccl;
		cp = f->cpos;
	}
	while (cluster_ok(v, c) && cp + v->cluster <= pos) {
		c = fat_next(v, c);
		cp += v->cluster;
	}
	UINTN done = 0;
	UINT8 *b = buf;
	while (done < n && cluster_ok(v, c)) {
		f->ccl = c;
		f->cpos = cp;
		UINT64 o = pos + done - cp;
		UINTN k = (UINTN)MIN(n - done, v->cluster - o);
		/* coalesce contiguous clusters into one read */
		UINT32 last = c;
		UINTN run = k;
		while (done + run < n) {
			UINT32 nx = fat_next(v, last);
			if (nx != last + 1 || !cluster_ok(v, nx))
				break;
			last = nx;
			run += MIN(n - done - run, (UINTN)v->cluster);
		}
		if (EFI_ERROR(read_bytes(v, cluster_off(v, c) + o, b + done, run)))
			break;
		done += run;
		/* move to the cluster after the run */
		UINT64 end = pos + done;
		while (cluster_ok(v, c) && cp + v->cluster <= end) {
			c = fat_next(v, c);
			cp += v->cluster;
		}
	}
	return done;
}

/* ---- directories */

struct dirent_info {
	CHAR16 name[256];
	UINT8 attr;
	UINT32 first;
	UINT32 size;
	UINT16 date, time;
};

static UINT8
lfn_sum(const UINT8 *n83)
{
	UINT8 s = 0;
	for (int i = 0; i < 11; i++)
		s = (UINT8)(((s & 1) << 7) + (s >> 1) + n83[i]);
	return s;
}

/* the next directory entry at *pos (byte offset); 1 if found, 0 at the end */
static int
dir_next(struct file *d, UINT64 *pos, struct dirent_info *e)
{
	CHAR16 lfn[256];
	int have_lfn = 0;
	UINT8 lfn_ck = 0;
	for (;;) {
		UINT8 de[32];
		if (chain_read(d, *pos, de, 32) != 32)
			return 0;
		*pos += 32;
		if (de[0] == 0)
			return 0;
		if (de[0] == 0xE5) {
			have_lfn = 0;
			continue;
		}
		if ((de[11] & 0x3F) == 0x0F) {		/* a long-name entry */
			unsigned seq = de[0] & 0x3F;
			if (seq < 1 || seq > 20)
				continue;
			if (de[0] & 0x40) {
				memset(lfn, 0, sizeof lfn);
				have_lfn = 1;
				lfn_ck = de[13];
			}
			static const UINT8 off[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
			for (int i = 0; i < 13; i++) {
				unsigned k = (seq - 1) * 13 + i;
				if (k < 255)
					lfn[k] = (CHAR16)(de[off[i]] | de[off[i] + 1] << 8);
			}
			continue;
		}
		if (de[11] & 0x08) {			/* the volume label */
			have_lfn = 0;
			continue;
		}
		if (have_lfn && lfn_ck == lfn_sum(de)) {
			for (int i = 0; i < 255; i++)
				if (lfn[i] == 0xFFFF)
					lfn[i] = 0;
			lfn[255] = 0;
			wcsncpy0(e->name, lfn, 256);
		} else {
			/* the 8.3 name, with the NT lower-case flags (byte 12) */
			unsigned n = 0;
			for (int i = 0; i < 8 && de[i] != ' '; i++)
				e->name[n++] = (CHAR16)((de[12] & 0x08) && de[i] >= 'A' && de[i] <= 'Z' ? de[i] + 32 : (de[0] == 0x05 && !i ? 0xE5 : de[i]));
			if (de[8] != ' ') {
				e->name[n++] = '.';
				for (int i = 8; i < 11 && de[i] != ' '; i++)
					e->name[n++] = (CHAR16)((de[12] & 0x10) && de[i] >= 'A' && de[i] <= 'Z' ? de[i] + 32 : de[i]);
			}
			e->name[n] = 0;
		}
		have_lfn = 0;
		e->attr = de[11];
		e->first = (UINT32)(de[26] | de[27] << 8) | (d->v->type == 32 ? (UINT32)(de[20] | de[21] << 8) << 16 : 0);
		memcpy(&e->size, de + 28, 4);
		e->time = (UINT16)(de[22] | de[23] << 8);
		e->date = (UINT16)(de[24] | de[25] << 8);
		return 1;
	}
}

static void
fat_time(EFI_TIME *t, UINT16 date, UINT16 time)
{
	memset(t, 0, sizeof *t);
	t->Year = (UINT16)(1980 + (date >> 9));
	t->Month = (UINT8)((date >> 5) & 15);
	t->Day = (UINT8)(date & 31);
	t->Hour = (UINT8)(time >> 11);
	t->Minute = (UINT8)((time >> 5) & 63);
	t->Second = (UINT8)((time & 31) * 2);
	t->TimeZone = EFI_UNSPECIFIED_TIMEZONE;
}

static struct file *
new_file(struct vol *v)
{
	struct file *f = calloc(1, sizeof *f);
	if (f) {
		f->fp = file_template;
		f->v = v;
	}
	return f;
}

static struct file *
open_root(struct vol *v)
{
	struct file *f = new_file(v);
	if (!f)
		return 0;
	f->dir = f->root = 1;
	f->first = v->type == 32 ? v->root_cluster : 0;
	f->attr = EFI_FILE_DIRECTORY;
	return f;
}

/* ---- the file protocol */

#define FILE_OF(p) ((struct file *)(p))

static EFI_STATUS EFIAPI
f_close(EFI_FILE_PROTOCOL *This)
{
	free(This);
	return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
f_open(EFI_FILE_PROTOCOL *This, EFI_FILE_PROTOCOL **New, CHAR16 *Name, UINT64 Mode, UINT64 Attr)
{
	struct file *cur = FILE_OF(This);
	if (!New || !Name)
		return EFI_INVALID_PARAMETER;
	if (Mode & (EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE))
		return EFI_WRITE_PROTECTED;
	struct file *f;
	const CHAR16 *p = Name;
	if (*p == '\\') {
		f = open_root(cur->v);
		while (*p == '\\')
			p++;
	} else {
		f = new_file(cur->v);
		if (f)
			*f = *cur;
	}
	if (!f)
		return EFI_OUT_OF_RESOURCES;
	f->ccl = 0;
	while (*p) {
		CHAR16 comp[256];
		unsigned n = 0;
		while (*p && *p != '\\' && n < 255)
			comp[n++] = *p++;
		comp[n] = 0;
		while (*p == '\\')
			p++;
		if (!n || !wcscmp(comp, L"."))
			continue;
		if (!f->dir) {
			free(f);
			return EFI_NOT_FOUND;
		}
		UINT64 pos = 0;
		struct dirent_info e;
		int found = 0;
		while (dir_next(f, &pos, &e)) {
			if (!wcsicmp(e.name, comp)) {
				found = 1;
				break;
			}
		}
		if (!found) {
			/* ".." at the root of FAT12/16 has no entry: stay at the root */
			if (!wcscmp(comp, L"..") && f->root)
				continue;
			free(f);
			return EFI_NOT_FOUND;
		}
		f->dir = (e.attr & 0x10) != 0;
		f->root = f->dir && e.first == 0;	/* ".." to the root */
		f->first = f->root ? (f->v->type == 32 ? f->v->root_cluster : 0) : e.first;
		f->size = f->dir ? 0 : e.size;
		f->attr = e.attr & 0x37;
		f->pos = 0;
		f->ccl = 0;
		wcsncpy0(f->name, e.name, 256);
		fat_time(&f->mtime, e.date, e.time);
	}
	*New = &f->fp;
	return EFI_SUCCESS;
}

static UINTN
info_size(const CHAR16 *name)
{
	return SIZE_OF_EFI_FILE_INFO + (wcslen(name) + 1) * 2;
}

static void
fill_info(EFI_FILE_INFO *i, const CHAR16 *name, UINT64 size, UINT8 attr, const EFI_TIME *t, struct vol *v)
{
	i->Size = info_size(name);
	i->FileSize = size;
	i->PhysicalSize = ALIGN_UP(size, v->cluster);
	i->CreateTime = *t;
	i->LastAccessTime = *t;
	i->ModificationTime = *t;
	i->Attribute = (attr & 0x37) | EFI_FILE_READ_ONLY;
	wcsncpy0(i->FileName, name, 256);
}

static EFI_STATUS EFIAPI
f_read(EFI_FILE_PROTOCOL *This, UINTN *Size, VOID *Buf)
{
	struct file *f = FILE_OF(This);
	if (!Size || (*Size && !Buf))
		return EFI_INVALID_PARAMETER;
	if (!f->dir) {
		if (f->pos >= f->size) {
			*Size = 0;
			return f->pos > f->size ? EFI_DEVICE_ERROR : EFI_SUCCESS;
		}
		UINTN n = (UINTN)MIN(*Size, f->size - f->pos);
		n = chain_read(f, f->pos, Buf, n);
		f->pos += n;
		*Size = n;
		return EFI_SUCCESS;
	}
	/* a directory: the next entry as EFI_FILE_INFO (13.5.6), skipping . and .. */
	for (;;) {
		UINT64 pos = f->pos;
		struct dirent_info e;
		if (!dir_next(f, &pos, &e)) {
			*Size = 0;
			return EFI_SUCCESS;
		}
		if (!wcscmp(e.name, L".") || !wcscmp(e.name, L"..")) {
			f->pos = pos;
			continue;
		}
		UINTN need = info_size(e.name);
		if (*Size < need) {
			*Size = need;
			return EFI_BUFFER_TOO_SMALL;
		}
		EFI_TIME t;
		fat_time(&t, e.date, e.time);
		fill_info(Buf, e.name, (e.attr & 0x10) ? 0 : e.size, e.attr, &t, f->v);
		f->pos = pos;
		*Size = need;
		return EFI_SUCCESS;
	}
}

static EFI_STATUS EFIAPI
f_write(EFI_FILE_PROTOCOL *This, UINTN *Size, VOID *Buf)
{
	return EFI_WRITE_PROTECTED;
}

static EFI_STATUS EFIAPI
f_delete(EFI_FILE_PROTOCOL *This)
{
	free(This);
	return 2;	/* EFI_WARN_DELETE_FAILURE: read-only */
}

static EFI_STATUS EFIAPI
f_get_position(EFI_FILE_PROTOCOL *This, UINT64 *Pos)
{
	struct file *f = FILE_OF(This);
	if (f->dir)
		return EFI_UNSUPPORTED;
	*Pos = f->pos;
	return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
f_set_position(EFI_FILE_PROTOCOL *This, UINT64 Pos)
{
	struct file *f = FILE_OF(This);
	if (f->dir) {
		if (Pos)
			return EFI_UNSUPPORTED;
		f->pos = 0;
		return EFI_SUCCESS;
	}
	f->pos = Pos == ~0ull ? f->size : Pos;
	return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
f_get_info(EFI_FILE_PROTOCOL *This, EFI_GUID *Type, UINTN *Size, VOID *Buf)
{
	struct file *f = FILE_OF(This);
	struct vol *v = f->v;
	if (!Type || !Size)
		return EFI_INVALID_PARAMETER;
	if (guid_eq(Type, &gEfiFileInfoGuid)) {
		const CHAR16 *name = f->root ? L"" : f->name;
		UINTN need = info_size(name);
		if (*Size < need) {
			*Size = need;
			return EFI_BUFFER_TOO_SMALL;
		}
		fill_info(Buf, name, f->size, f->attr, &f->mtime, v);
		*Size = need;
		return EFI_SUCCESS;
	}
	if (guid_eq(Type, &gEfiFileSystemInfoGuid)) {
		CHAR16 label[12];
		wcs_from_ascii(label, v->label, 12);
		UINTN need = SIZE_OF_EFI_FILE_SYSTEM_INFO + (wcslen(label) + 1) * 2;
		if (*Size < need) {
			*Size = need;
			return EFI_BUFFER_TOO_SMALL;
		}
		EFI_FILE_SYSTEM_INFO *i = Buf;
		i->Size = need;
		i->ReadOnly = TRUE;
		i->VolumeSize = (UINT64)v->nclusters * v->cluster;
		UINT64 freec = 0;
		for (UINT32 c = 2; c < v->nclusters + 2; c++)
			if (!fat_next(v, c))
				freec++;
		i->FreeSpace = freec * v->cluster;
		i->BlockSize = v->cluster;
		wcsncpy0(i->VolumeLabel, label, 12);
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
	struct vol *v = (struct vol *)This;
	if (!Root)
		return EFI_INVALID_PARAMETER;
	if (v->media_id != v->bio->Media->MediaId || !v->bio->Media->MediaPresent)
		return EFI_MEDIA_CHANGED;
	struct file *f = open_root(v);
	if (!f)
		return EFI_OUT_OF_RESOURCES;
	*Root = &f->fp;
	return EFI_SUCCESS;
}

/* ---- mounting */

/* a FAT boot sector? Fills the volume geometry; 0 if it is one */
static int
parse_bpb(struct vol *v, const UINT8 *b)
{
	UINT32 bps = b[11] | b[12] << 8, spc = b[13], rsvd = b[14] | b[15] << 8, nfats = b[16];
	UINT32 rootent = b[17] | b[18] << 8, tot16 = b[19] | b[20] << 8, fsz16 = b[22] | b[23] << 8;
	UINT32 tot32, fsz32;
	memcpy(&tot32, b + 32, 4);
	memcpy(&fsz32, b + 36, 4);
	if (b[510] != 0x55 || b[511] != 0xAA)
		return -1;
	if ((bps != 512 && bps != 1024 && bps != 2048 && bps != 4096) || !spc || (spc & (spc - 1)) ||
	    !rsvd || !nfats || (b[0] != 0xEB && b[0] != 0xE9))
		return -1;
	UINT32 fsz = fsz16 ? fsz16 : fsz32, tot = tot16 ? tot16 : tot32;
	UINT32 rootsec = (rootent * 32 + bps - 1) / bps;
	if (!fsz || !tot || tot <= rsvd + nfats * fsz + rootsec)
		return -1;
	UINT32 clusters = (tot - rsvd - nfats * fsz - rootsec) / spc;
	v->bps = bps;
	v->spc = spc;
	v->cluster = bps * spc;
	v->type = clusters < 4085 ? 12 : clusters < 65525 ? 16 : 32;
	v->fat_sec = rsvd;
	v->root_sec = rsvd + (UINT64)nfats * fsz;
	v->data_sec = v->root_sec + rootsec;
	v->total_sec = tot;
	v->root_entries = rootent;
	v->nclusters = clusters;
	if (v->type == 32)
		memcpy(&v->root_cluster, b + 44, 4);
	const UINT8 *lab = v->type == 32 ? b + 71 : b + 43;
	UINT8 sig = v->type == 32 ? b[66] : b[38];
	memset(v->label, 0, sizeof v->label);
	if (sig == 0x29) {
		memcpy(v->label, lab, 11);
		for (int i = 10; i >= 0 && v->label[i] == ' '; i--)
			v->label[i] = 0;
		if (!strcmp(v->label, "NO NAME"))
			v->label[0] = 0;
	}
	UINT64 fat_bytes = (UINT64)fsz * bps;
	if (fat_bytes > FAT_MAX_BYTES)
		return -1;
	return 0;
}

static void
mount(EFI_HANDLE dev, EFI_BLOCK_IO_PROTOCOL *bio, UINT64 start, const char *what)
{
	struct vol *v = calloc(1, sizeof *v);
	UINT8 *b = malloc(4096);
	if (!v || !b)
		return;
	v->bio = bio;
	v->media_id = bio->Media->MediaId;
	v->start = start;
	if (bio->Media->BlockSize > 4096 ||
	    EFI_ERROR(bio->ReadBlocks(bio, v->media_id, start, bio->Media->BlockSize, b)) || parse_bpb(v, b)) {
		free(b);
		free(v);
		return;
	}
	free(b);
	UINT64 fat_bytes = ((v->type == 12 ? (v->nclusters + 2) * 3 / 2 + 1 :
	    (UINT64)(v->nclusters + 2) * (v->type / 8)) + v->bps - 1) / v->bps * v->bps;
	v->fat = malloc(fat_bytes);
	if (!v->fat || EFI_ERROR(read_bytes(v, v->fat_sec * v->bps, v->fat, fat_bytes))) {
		free(v->fat);
		free(v);
		return;
	}
	v->sfs.Revision = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_REVISION;
	v->sfs.OpenVolume = open_volume;
	EFI_STATUS s = gBS->InstallProtocolInterface(&dev, &gEfiSimpleFileSystemProtocolGuid,
	    EFI_NATIVE_INTERFACE, &v->sfs);
	DEBUG("FatDxe: FAT%d volume%s%s on %s: %u clusters of %u bytes: %r\n", v->type,
	    v->label[0] ? " " : "", v->label, what, v->nclusters, v->cluster, s);
}

/* ---- partitions (MBR, UEFI 2.10 5.2.1) */

struct part {
	EFI_BLOCK_IO_PROTOCOL bio;	/* first: This */
	EFI_BLOCK_IO_MEDIA media;
	EFI_BLOCK_IO_PROTOCOL *parent;
	UINT64 start;
};

static EFI_STATUS EFIAPI
part_read(EFI_BLOCK_IO_PROTOCOL *This, UINT32 MediaId, EFI_LBA Lba, UINTN Size, VOID *Buf)
{
	struct part *p = (struct part *)This;
	if (Lba > p->media.LastBlock || Size / p->media.BlockSize > p->media.LastBlock - Lba + 1)
		return EFI_INVALID_PARAMETER;
	return p->parent->ReadBlocks(p->parent, MediaId, p->start + Lba, Size, Buf);
}

static EFI_STATUS EFIAPI
part_write(EFI_BLOCK_IO_PROTOCOL *This, UINT32 MediaId, EFI_LBA Lba, UINTN Size, VOID *Buf)
{
	struct part *p = (struct part *)This;
	if (Lba > p->media.LastBlock || Size / p->media.BlockSize > p->media.LastBlock - Lba + 1)
		return EFI_INVALID_PARAMETER;
	return p->parent->WriteBlocks(p->parent, MediaId, p->start + Lba, Size, Buf);
}

static EFI_STATUS EFIAPI
part_reset(EFI_BLOCK_IO_PROTOCOL *This, BOOLEAN Ext)
{
	return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
part_flush(EFI_BLOCK_IO_PROTOCOL *This)
{
	struct part *p = (struct part *)This;
	return p->parent->FlushBlocks(p->parent);
}

static int
scan_mbr(EFI_HANDLE dev, EFI_BLOCK_IO_PROTOCOL *bio, const UINT8 *b)
{
	if (b[510] != 0x55 || b[511] != 0xAA)
		return 0;
	int found = 0;
	for (int i = 0; i < 4; i++) {
		const UINT8 *e = b + 446 + 16 * i;
		UINT32 start, count;
		memcpy(&start, e + 8, 4);
		memcpy(&count, e + 12, 4);
		if (!e[4] || e[4] == 0xEE || !count || (e[0] & 0x7F) ||
		    start + (UINT64)count > bio->Media->LastBlock + 1)
			continue;
		struct part *p = calloc(1, sizeof *p);
		p->parent = bio;
		p->start = start;
		p->media = *bio->Media;
		p->media.LogicalPartition = TRUE;
		p->media.LastBlock = count - 1;
		p->bio = (EFI_BLOCK_IO_PROTOCOL){ bio->Revision, &p->media, part_reset, part_read, part_write, part_flush };
		HARDDRIVE_DEVICE_PATH hd;
		memset(&hd, 0, sizeof hd);
		dp_set_node(&hd.Header, MEDIA_DEVICE_PATH, MEDIA_HARDDRIVE_DP, sizeof hd);
		hd.PartitionNumber = (UINT32)i + 1;
		hd.PartitionStart = start;
		hd.PartitionSize = count;
		memcpy(hd.Signature, b + 440, 4);
		hd.MBRType = 1;
		hd.SignatureType = 1;
		EFI_DEVICE_PATH_PROTOCOL *path = dp_append_node(dp_of(dev), &hd.Header);
		EFI_HANDLE h = 0;
		gBS->InstallMultipleProtocolInterfaces(&h, &gEfiDevicePathProtocolGuid, path,
		    &gEfiBlockIoProtocolGuid, &p->bio, (void *)0);
		char what[24];
		snprintf(what, sizeof what, "partition %d", i + 1);
		mount(h, &p->bio, 0, what);
		found = 1;
	}
	return found;
}

static void
probe(EFI_HANDLE h)
{
	EFI_BLOCK_IO_PROTOCOL *bio;
	EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *sfs;
	if (gBS->HandleProtocol(h, &gEfiBlockIoProtocolGuid, (void **)&bio) ||
	    !gBS->HandleProtocol(h, &gEfiSimpleFileSystemProtocolGuid, (void **)&sfs))
		return;
	if (bio->Media->LogicalPartition || !bio->Media->MediaPresent || bio->Media->BlockSize > 4096)
		return;
	UINT8 *b = malloc(4096);
	if (!b || EFI_ERROR(bio->ReadBlocks(bio, bio->Media->MediaId, 0, bio->Media->BlockSize, b))) {
		free(b);
		return;
	}
	/* a FAT boot sector is a superfloppy; else an MBR with partitions */
	struct vol tmp;
	if (parse_bpb(&tmp, b) == 0)
		mount(h, bio, 0, "the whole device");
	else
		scan_mbr(h, bio, b);
	free(b);
}

static void *reg;

static VOID EFIAPI
on_block_io(EFI_EVENT ev, VOID *ctx)
{
	EFI_HANDLE h;
	UINTN sz = sizeof h;
	while (gBS->LocateHandle(ByRegisterNotify, 0, reg, &sz, &h) == EFI_SUCCESS) {
		probe(h);
		sz = sizeof h;
	}
}

EFI_STATUS EFIAPI
efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st)
{
	uefi_init(image, st);
	EFI_HANDLE *hs;
	UINTN n;
	if (gBS->LocateHandleBuffer(ByProtocol, &gEfiBlockIoProtocolGuid, 0, &n, &hs) == EFI_SUCCESS) {
		for (UINTN i = 0; i < n; i++)
			probe(hs[i]);
		free(hs);
	}
	/* Block I/O that appears later (partitions we add are probed directly) */
	EFI_EVENT ev;
	gBS->CreateEvent(EVT_NOTIFY_SIGNAL, TPL_CALLBACK, on_block_io, 0, &ev);
	gBS->RegisterProtocolNotify(&gEfiBlockIoProtocolGuid, ev, &reg);
	return EFI_SUCCESS;
}
