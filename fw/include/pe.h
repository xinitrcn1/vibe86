/* pe.h: PE32+ loading (fw/lib/pe.c) and firmware volume walking (fw/lib/fv.c) */
#ifndef PE_H
#define PE_H

#include "pi.h"

/* the NT headers of a PE32+ x64 image, or NULL */
const IMAGE_NT_HEADERS64 *pe_headers(const void *file, size_t size);
/* copy the image to dest (SizeOfImage bytes, page-aligned) and relocate it
   there; *entry = its entry point. 0, or -1 if the image is malformed */
int pe_load(const void *file, size_t size, void *dest, uint64_t *entry);

/* firmware volumes (PI 1.8 Vol. 3 ch. 3): iterate over the FFS files of a
   volume; *cookie = 0 to start. Returns the next file header or NULL */
const EFI_FIRMWARE_VOLUME_HEADER *fv_check(uint64_t base);
const EFI_FFS_FILE_HEADER *fv_next_file(const EFI_FIRMWARE_VOLUME_HEADER *fv, uint64_t *cookie);
/* a section of a file by type: its data and size, or NULL */
const void *ffs_section(const EFI_FFS_FILE_HEADER *f, UINT8 type, UINT32 *size);
UINT32 ffs_size(const EFI_FFS_FILE_HEADER *f);

#endif
