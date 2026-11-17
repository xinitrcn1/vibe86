/*
 * PlatformDxe: the ACPI tables. fw/acpi builds them (FACS, FADT, MADT, MCFG,
 * RSDT, XSDT, DSDT, RSDP) together with a QEMU bios-linker-loader script
 * (QEMU hw/acpi/bios-linker-loader.c: 128-byte commands ALLOCATE,
 * ADD_POINTER, ADD_CHECKSUM), which this file runs as OVMF's
 * QemuFwCfgAcpiPlatformDxe does: the files come from fw_cfg ("etc/table-
 * loader", "etc/acpi/tables", "etc/acpi/rsdp": vsim -f) when the platform
 * offers them, else from FV_MAIN, where the build put the same three files.
 * The RSDP goes into the system table under the ACPI 2.0 and 1.0 GUIDs
 * (UEFI 2.10 4.6).
 */
#include "uefilib.h"
#include "pe.h"
#include "vibe86fw.h"
#include "layout.h"

#define CMD_ALLOCATE	1
#define CMD_ADD_POINTER	2
#define CMD_ADD_CHECKSUM 3
#define MAX_BLOBS	8

struct blob {
	char name[56];
	uint8_t *data;
	uint32_t size;
};

static struct blob blobs[MAX_BLOBS];
static unsigned nblobs;

static int
use_fwcfg(void)
{
	uint32_t s;
	uint16_t sel;
	return fwcfg_find("etc/table-loader", &s, &sel) == 0;
}

/* a loader file's bytes: from fw_cfg, or the FV raw file with that role */
static const void *
fetch(const char *name, uint32_t *size, int fwcfg)
{
	if (fwcfg) {
		uint16_t sel;
		if (fwcfg_find(name, size, &sel))
			return 0;
		void *b = malloc(*size ? *size : 1);
		if (b)
			fwcfg_read(sel, b, *size);
		return b;
	}
	static const EFI_GUID tables = FILE_ACPI_TABLES_GUID, rsdp = FILE_ACPI_RSDP_GUID,
	    loader = FILE_ACPI_LOADER_GUID;
	const EFI_GUID *g = !strcmp(name, "etc/acpi/tables") ? &tables : !strcmp(name, "etc/acpi/rsdp") ?
	    &rsdp : !strcmp(name, "etc/table-loader") ? &loader : 0;
	const EFI_FIRMWARE_VOLUME_HEADER *fv = fv_check(FV_MAIN_BASE);
	if (!g || !fv)
		return 0;
	uint64_t cookie = 0;
	const EFI_FFS_FILE_HEADER *f;
	while ((f = fv_next_file(fv, &cookie)))
		if (guid_eq(&f->Name, g))
			return ffs_section(f, EFI_SECTION_RAW, size);
	return 0;
}

static struct blob *
blob(const char *name)
{
	for (unsigned i = 0; i < nblobs; i++)
		if (!strncmp(blobs[i].name, name, 56))
			return &blobs[i];
	return 0;
}

void
acpi_install(void)
{
	int fw = use_fwcfg();
	uint32_t lsize;
	const uint8_t *loader = fetch("etc/table-loader", &lsize, fw);
	if (!loader) {
		DEBUG("PlatformDxe: no ACPI table loader script\n");
		return;
	}
	for (uint32_t off = 0; off + 128 <= lsize; off += 128) {
		const uint8_t *c = loader + off;
		uint32_t cmd;
		memcpy(&cmd, c, 4);
		if (cmd == CMD_ALLOCATE && nblobs < MAX_BLOBS) {
			struct blob *b = &blobs[nblobs];
			memcpy(b->name, c + 4, 56);
			b->name[55] = 0;
			uint32_t align;
			memcpy(&align, c + 60, 4);
			const void *src = fetch(b->name, &b->size, fw);
			if (!src) {
				DEBUG("PlatformDxe: ACPI: %s missing\n", b->name);
				return;
			}
			/* ACPI NVS: the tables include the FACS (ACPI 6.5 5.2.10) */
			EFI_PHYSICAL_ADDRESS a = 0xFFFFFFFF;
			if (align > EFI_PAGE_SIZE ||
			    gBS->AllocatePages(AllocateMaxAddress, EfiACPIMemoryNVS, EFI_SIZE_TO_PAGES(b->size), &a)) {
				DEBUG("PlatformDxe: ACPI: can't place %s\n", b->name);
				return;
			}
			b->data = (uint8_t *)(uintptr_t)a;
			memset(b->data, 0, EFI_SIZE_TO_PAGES(b->size) * EFI_PAGE_SIZE);
			memcpy(b->data, src, b->size);
			nblobs++;
		} else if (cmd == CMD_ADD_POINTER) {
			char dn[56], sn[56];
			memcpy(dn, c + 4, 56);
			memcpy(sn, c + 60, 56);
			dn[55] = sn[55] = 0;
			uint32_t poff;
			memcpy(&poff, c + 116, 4);
			uint8_t psize = c[120];
			struct blob *d = blob(dn), *s = blob(sn);
			if (!d || !s || poff + psize > d->size || (psize != 1 && psize != 2 && psize != 4 && psize != 8))
				continue;
			uint64_t v = 0;
			memcpy(&v, d->data + poff, psize);
			v += (uint64_t)(uintptr_t)s->data;
			memcpy(d->data + poff, &v, psize);
		} else if (cmd == CMD_ADD_CHECKSUM) {
			char fn[56];
			memcpy(fn, c + 4, 56);
			fn[55] = 0;
			uint32_t res, start, len;
			memcpy(&res, c + 60, 4);
			memcpy(&start, c + 64, 4);
			memcpy(&len, c + 68, 4);
			struct blob *b = blob(fn);
			if (!b || res >= b->size || start + (uint64_t)len > b->size)
				continue;
			b->data[res] = 0;
			uint8_t sum = 0;
			for (uint32_t i = 0; i < len; i++)
				sum = (uint8_t)(sum + b->data[start + i]);
			b->data[res] = (uint8_t)-sum;
		}
	}
	struct blob *r = blob("etc/acpi/rsdp");
	if (!r) {
		DEBUG("PlatformDxe: ACPI: no RSDP\n");
		return;
	}
	static EFI_GUID acpi20 = EFI_ACPI_20_TABLE_GUID, acpi10 = EFI_ACPI_TABLE_GUID;
	gBS->InstallConfigurationTable(&acpi20, r->data);
	gBS->InstallConfigurationTable(&acpi10, r->data);
	DEBUG("PlatformDxe: ACPI tables from %s, RSDP at %p\n", fw ? "fw_cfg" : "the firmware volume",
	    r->data);
}
