/*
 * fwlib: walking a firmware volume (PI 1.8 Vol. 3: 3.2.1 the volume header,
 * 2.2.3 FFS file headers, 8-byte aligned; 2.2.4 sections, 4-byte aligned).
 * fw/tools/mkfd.py writes the volumes.
 */
#include "fwlib.h"
#include "pe.h"

static const EFI_GUID ffs2 = EFI_FIRMWARE_FILE_SYSTEM2_GUID;

const EFI_FIRMWARE_VOLUME_HEADER *
fv_check(uint64_t base)
{
	const EFI_FIRMWARE_VOLUME_HEADER *fv = (const void *)(uintptr_t)base;
	if (fv->Signature != EFI_FVH_SIGNATURE || !guid_eq(&fv->FileSystemGuid, &ffs2))
		return 0;
	uint16_t sum = 0;
	const uint16_t *w = (const uint16_t *)fv;
	for (unsigned i = 0; i < fv->HeaderLength / 2u; i++)
		sum = (uint16_t)(sum + w[i]);
	return sum == 0 ? fv : 0;
}

UINT32
ffs_size(const EFI_FFS_FILE_HEADER *f)
{
	return FV_SIZE24(f->Size);
}

const EFI_FFS_FILE_HEADER *
fv_next_file(const EFI_FIRMWARE_VOLUME_HEADER *fv, uint64_t *cookie)
{
	uint64_t off = *cookie ? *cookie : fv->HeaderLength;
	for (;;) {
		off = ALIGN_UP(off, 8);
		if (off + sizeof(EFI_FFS_FILE_HEADER) > fv->FvLength)
			return 0;
		const EFI_FFS_FILE_HEADER *f = (const void *)((const UINT8 *)fv + off);
		UINT32 size = ffs_size(f);
		if (size == 0xFFFFFF || size < sizeof *f)
			return 0;		/* erased: the end of the files */
		off += size;
		*cookie = off;
		if (f->Type == EFI_FV_FILETYPE_FFS_PAD)
			continue;
		return f;
	}
}

const void *
ffs_section(const EFI_FFS_FILE_HEADER *f, UINT8 type, UINT32 *size)
{
	UINT32 fsize = ffs_size(f), off = sizeof *f;
	while (off + sizeof(EFI_COMMON_SECTION_HEADER) <= fsize) {
		const EFI_COMMON_SECTION_HEADER *s = (const void *)((const UINT8 *)f + off);
		UINT32 ssize = FV_SIZE24(s->Size);
		if (ssize < sizeof *s || off + ssize > fsize)
			return 0;
		if (s->Type == type) {
			if (size)
				*size = ssize - (UINT32)sizeof *s;
			return s + 1;
		}
		off = (UINT32)ALIGN_UP(off + ssize, 4);
	}
	return 0;
}
