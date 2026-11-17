/*
 * fwlib: loading PE32+ images (Microsoft "PE Format": the optional header,
 * section table and base relocations, ".reloc" IMAGE_REL_BASED_DIR64), as
 * UEFI 2.10 2.1.1 requires of UEFI images.
 */
#include "fwlib.h"
#include "pe.h"

const IMAGE_NT_HEADERS64 *
pe_headers(const void *file, size_t size)
{
	const IMAGE_DOS_HEADER *dos = file;
	if (size < sizeof *dos || dos->e_magic != IMAGE_DOS_SIGNATURE)
		return 0;
	if (dos->e_lfanew + sizeof(IMAGE_NT_HEADERS64) > size)
		return 0;
	const IMAGE_NT_HEADERS64 *nt = (const void *)((const UINT8 *)file + dos->e_lfanew);
	if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
	    nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
		return 0;
	return nt;
}

int
pe_load(const void *file, size_t size, void *dest, uint64_t *entry)
{
	const IMAGE_NT_HEADERS64 *nt = pe_headers(file, size);
	if (!nt)
		return -1;
	const IMAGE_OPTIONAL_HEADER64 *oh = &nt->OptionalHeader;
	UINT8 *img = dest;
	if (oh->SizeOfHeaders > size || oh->SizeOfHeaders > oh->SizeOfImage)
		return -1;
	memset(img, 0, oh->SizeOfImage);
	memcpy(img, file, oh->SizeOfHeaders);
	const IMAGE_SECTION_HEADER *sh = (const void *)((const UINT8 *)&nt->OptionalHeader +
	    nt->FileHeader.SizeOfOptionalHeader);
	for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; i++) {
		UINT32 raw = MIN(sh[i].SizeOfRawData, sh[i].VirtualSize ? sh[i].VirtualSize : sh[i].SizeOfRawData);
		if ((UINT64)sh[i].VirtualAddress + MAX(raw, sh[i].VirtualSize) > oh->SizeOfImage ||
		    (UINT64)sh[i].PointerToRawData + raw > size)
			return -1;
		memcpy(img + sh[i].VirtualAddress, (const UINT8 *)file + sh[i].PointerToRawData, raw);
	}
	/* base relocations */
	int64_t delta = (int64_t)((uint64_t)(uintptr_t)img - oh->ImageBase);
	if (delta && oh->NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_BASERELOC) {
		const IMAGE_DATA_DIRECTORY *rd = &oh->DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
		UINT32 off = 0;
		if (!rd->Size && (nt->FileHeader.Characteristics & 1))	/* RELOCS_STRIPPED */
			return -1;
		while (off + sizeof(IMAGE_BASE_RELOCATION) <= rd->Size) {
			const IMAGE_BASE_RELOCATION *b = (const void *)(img + rd->VirtualAddress + off);
			if (b->SizeOfBlock < sizeof *b)
				break;
			const UINT16 *r = (const UINT16 *)(b + 1);
			unsigned n = (b->SizeOfBlock - sizeof *b) / 2;
			for (unsigned k = 0; k < n; k++) {
				unsigned type = r[k] >> 12, o = r[k] & 0xFFF;
				if (type == IMAGE_REL_BASED_DIR64) {
					uint64_t *p = (uint64_t *)(img + b->VirtualAddress + o);
					*p += (uint64_t)delta;
				} else if (type != IMAGE_REL_BASED_ABSOLUTE)
					return -1;
			}
			off += b->SizeOfBlock;
		}
	}
	if (entry)
		*entry = (uint64_t)(uintptr_t)img + oh->AddressOfEntryPoint;
	return 0;
}
