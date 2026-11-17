/*
 * PlatformDxe: the board-specific pieces of DXE:
 *
 *  - the PC chipset defaults: 8259 PICs initialised and masked (Intel 8259A
 *    data sheet; vectors 68h/70h as EDK2's Legacy8259 uses), the PIIX4
 *    power-management I/O space at 600h (82371AB data sheet 3.4.2.2 PMBA,
 *    PMREGMISC), where the FADT puts the PM1 registers;
 *  - the ACPI tables (acpi.c), SMBIOS (smbios.c);
 *  - the Legacy Region 2 protocol (PI 1.8 Vol. 5 ch. 15) on the i440FX's
 *    PAM registers (Intel 82441FX data sheet 290549-001, 3.2.17 PAM0-PAM6):
 *    C_0000h-F_FFFFh in 16 KiB pieces (F_0000h-F_FFFFh as one 64 KiB piece),
 *    each with read enable (RE, bit 0) and write enable (WE, bit 1).
 */
#include "uefilib.h"
#include "x86.h"

void acpi_install(void);
void smbios_install(void);

/* ---- Legacy Region 2 */

#define PAM_BASE	0x59

/* the PAM register and nibble shift for an address in C_0000h-F_FFFFh */
static void
pam_of(UINT32 a, unsigned *reg, unsigned *shift)
{
	if (a >= 0xF0000) {
		*reg = PAM_BASE;
		*shift = 4;
		return;
	}
	unsigned k = (a - 0xC0000) >> 14;	/* 16 KiB pieces from C_0000h */
	*reg = PAM_BASE + 1 + k / 2;
	*shift = (k & 1) ? 4 : 0;
}

static EFI_STATUS
pam_update(UINT32 start, UINT32 len, UINT32 *gran, uint8_t clear, uint8_t set)
{
	if (start < 0xC0000 || len == 0 || start + len > 0x100000 || start + len < start)
		return EFI_INVALID_PARAMETER;
	for (UINT32 a = start & ~0x3FFFu; a < start + len; a += a >= 0xF0000 ? 0x10000 : 0x4000) {
		unsigned reg, sh;
		pam_of(a, &reg, &sh);
		uint8_t v = pci_r8(0, 0, 0, reg);
		v = (uint8_t)((v & ~(clear << sh)) | (set << sh));
		pci_w8(0, 0, 0, reg, v);
		if (a >= 0xF0000)
			break;
	}
	if (gran)
		*gran = start >= 0xF0000 ? 0x10000 : 0x4000;
	return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
lr_decode(EFI_LEGACY_REGION2_PROTOCOL *This, UINT32 Start, UINT32 Length, UINT32 *Gran, BOOLEAN *On)
{
	if (!On)
		return EFI_INVALID_PARAMETER;
	return pam_update(Start, Length, Gran, *On ? 0 : 1, *On ? 1 : 0);
}

static EFI_STATUS EFIAPI
lr_lock(EFI_LEGACY_REGION2_PROTOCOL *This, UINT32 Start, UINT32 Length, UINT32 *Gran)
{
	return pam_update(Start, Length, Gran, 2, 0);
}

static EFI_STATUS EFIAPI
lr_boot_lock(EFI_LEGACY_REGION2_PROTOCOL *This, UINT32 Start, UINT32 Length, UINT32 *Gran)
{
	/* the PAM registers can't be locked against later changes */
	return EFI_UNSUPPORTED;
}

static EFI_STATUS EFIAPI
lr_unlock(EFI_LEGACY_REGION2_PROTOCOL *This, UINT32 Start, UINT32 Length, UINT32 *Gran)
{
	return pam_update(Start, Length, Gran, 0, 2);
}

static EFI_LEGACY_REGION_DESCRIPTOR lr_desc[13 * 3];

static EFI_STATUS EFIAPI
lr_get_info(EFI_LEGACY_REGION2_PROTOCOL *This, UINT32 *Count, EFI_LEGACY_REGION_DESCRIPTOR **Desc)
{
	if (!Count || !Desc)
		return EFI_INVALID_PARAMETER;
	unsigned n = 0;
	for (UINT32 a = 0xC0000; a < 0x100000; a += a >= 0xF0000 ? 0x10000 : 0x4000) {
		unsigned reg, sh;
		pam_of(a, &reg, &sh);
		unsigned v = (pci_r8(0, 0, 0, reg) >> sh) & 3;
		UINT32 len = a >= 0xF0000 ? 0x10000 : 0x4000;
		lr_desc[n++] = (EFI_LEGACY_REGION_DESCRIPTOR){ a, len,
		    (v & 1) ? LegacyRegionDecoded : LegacyRegionNotDecoded, len };
		lr_desc[n++] = (EFI_LEGACY_REGION_DESCRIPTOR){ a, len,
		    (v & 2) ? LegacyRegionWriteEnabled : LegacyRegionWriteDisabled, len };
		lr_desc[n++] = (EFI_LEGACY_REGION_DESCRIPTOR){ a, len, LegacyRegionNotLocked, len };
	}
	*Count = n;
	*Desc = lr_desc;
	return EFI_SUCCESS;
}

static EFI_LEGACY_REGION2_PROTOCOL legacy_region = { lr_decode, lr_lock, lr_boot_lock, lr_unlock,
	lr_get_info };

/* ---- chipset defaults */

static void
init_pics(void)
{
	/* ICW1 (edge, cascade, ICW4), ICW2 vector base, ICW3 cascade on IR2, ICW4 8086 mode */
	outb(0x20, 0x11);
	outb(0xA0, 0x11);
	outb(0x21, 0x68);
	outb(0xA1, 0x70);
	outb(0x21, 0x04);
	outb(0xA1, 0x02);
	outb(0x21, 0x01);
	outb(0xA1, 0x01);
	outb(0x21, 0xFF);	/* everything masked: the firmware polls */
	outb(0xA1, 0xFF);
}

static void
init_piix_pm(void)
{
	/* 00:01.3: PMBA = 600h (I/O), PMREGMISC.PMIOSE */
	pci_w32(0, 1, 3, 0x40, PM1_PORT | 1);
	pci_w8(0, 1, 3, 0x80, 0x01);
}

EFI_STATUS EFIAPI
efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st)
{
	static EFI_GUID lr_guid = EFI_LEGACY_REGION2_PROTOCOL_GUID;
	uefi_init(image, st);
	init_pics();
	init_piix_pm();
	acpi_install();
	smbios_install();
	EFI_HANDLE h = 0;
	gBS->InstallProtocolInterface(&h, &lr_guid, EFI_NATIVE_INTERFACE, &legacy_region);
	return EFI_SUCCESS;
}
