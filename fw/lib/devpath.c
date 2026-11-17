/* fwlib: device path nodes and their text form ([UEFI] 10.3 and 10.6.1.6) */
#include "fwlib.h"

UINTN
dp_node_len(const EFI_DEVICE_PATH_PROTOCOL *n)
{
	return n->Length[0] | n->Length[1] << 8;
}

int
dp_is_end(const EFI_DEVICE_PATH_PROTOCOL *n)
{
	return n->Type == END_DEVICE_PATH_TYPE && n->SubType == END_ENTIRE_DEVICE_PATH_SUBTYPE;
}

EFI_DEVICE_PATH_PROTOCOL *
dp_next(const EFI_DEVICE_PATH_PROTOCOL *n)
{
	return (EFI_DEVICE_PATH_PROTOCOL *)((const UINT8 *)n + dp_node_len(n));
}

UINTN
dp_size(const EFI_DEVICE_PATH_PROTOCOL *p)
{
	const EFI_DEVICE_PATH_PROTOCOL *n = p;
	if (!p)
		return 0;
	while (!dp_is_end(n)) {
		if (dp_node_len(n) < 4)
			break;			/* malformed: stop at it */
		n = dp_next(n);
	}
	return (UINTN)((const UINT8 *)n - (const UINT8 *)p) + 4;
}

void
dp_set_node(EFI_DEVICE_PATH_PROTOCOL *n, UINT8 type, UINT8 sub, UINTN len)
{
	n->Type = type;
	n->SubType = sub;
	n->Length[0] = (UINT8)len;
	n->Length[1] = (UINT8)(len >> 8);
}

void
dp_set_end(EFI_DEVICE_PATH_PROTOCOL *n)
{
	dp_set_node(n, END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE, 4);
}

struct tb {
	char *buf;
	size_t n, len;
};

static void
tb_add(struct tb *t, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	if (t->len < t->n)
		t->len += (size_t)vsnprintf(t->buf + t->len, t->n - t->len, fmt, ap);
	va_end(ap);
}

void
dp_to_text(const EFI_DEVICE_PATH_PROTOCOL *p, char *buf, size_t n)
{
	struct tb t = { buf, n, 0 };
	if (n)
		buf[0] = 0;
	for (int first = 1; p && !dp_is_end(p) && dp_node_len(p) >= 4; p = dp_next(p), first = 0) {
		if (!first)
			tb_add(&t, "/");
		switch (p->Type << 8 | p->SubType) {
		case HARDWARE_DEVICE_PATH << 8 | HW_PCI_DP: {
			const PCI_DEVICE_PATH *d = (const void *)p;
			tb_add(&t, "Pci(0x%x,0x%x)", d->Device, d->Function);
			break;
		}
		case HARDWARE_DEVICE_PATH << 8 | HW_MEMMAP_DP: {
			const MEMMAP_DEVICE_PATH *d = (const void *)p;
			tb_add(&t, "MemoryMapped(0x%x,0x%lx,0x%lx)", d->MemoryType, d->StartingAddress,
			    d->EndingAddress);
			break;
		}
		case HARDWARE_DEVICE_PATH << 8 | HW_VENDOR_DP:
		case MESSAGING_DEVICE_PATH << 8 | MSG_VENDOR_DP:
		case MEDIA_DEVICE_PATH << 8 | MEDIA_VENDOR_DP: {
			const VENDOR_DEVICE_PATH *d = (const void *)p;
			tb_add(&t, "%s(%g)", p->Type == HARDWARE_DEVICE_PATH ? "VenHw" :
			    p->Type == MESSAGING_DEVICE_PATH ? "VenMsg" : "VenMedia", &d->Guid);
			break;
		}
		case ACPI_DEVICE_PATH << 8 | ACPI_DP: {
			const ACPI_HID_DEVICE_PATH *d = (const void *)p;
			if (d->HID == EISA_PNP_ID(0x0A03) || d->HID == EISA_PNP_ID(0x0A08))
				tb_add(&t, "PciRoot(0x%x)", d->UID);
			else if (d->HID == EISA_PNP_ID(0x0501))
				tb_add(&t, "Serial(0x%x)", d->UID);
			else
				tb_add(&t, "Acpi(PNP%04X,0x%x)", d->HID >> 16, d->UID);
			break;
		}
		case MESSAGING_DEVICE_PATH << 8 | MSG_USB_DP: {
			const USB_DEVICE_PATH *d = (const void *)p;
			tb_add(&t, "USB(0x%x,0x%x)", d->ParentPortNumber, d->InterfaceNumber);
			break;
		}
		case MESSAGING_DEVICE_PATH << 8 | MSG_UART_DP: {
			const UART_DEVICE_PATH *d = (const void *)p;
			tb_add(&t, "Uart(%lu,%u,N,1)", d->BaudRate, d->DataBits);
			break;
		}
		case MEDIA_DEVICE_PATH << 8 | MEDIA_HARDDRIVE_DP: {
			const HARDDRIVE_DEVICE_PATH *d = (const void *)p;
			tb_add(&t, "HD(%u,MBR,0x%x,0x%lx,0x%lx)", d->PartitionNumber,
			    *(const UINT32 *)d->Signature, d->PartitionStart, d->PartitionSize);
			break;
		}
		case MEDIA_DEVICE_PATH << 8 | MEDIA_FILEPATH_DP: {
			const FILEPATH_DEVICE_PATH *d = (const void *)p;
			UINTN len = (dp_node_len(p) - 4) / 2;
			for (UINTN i = 0; i < len && d->PathName[i]; i++)
				tb_add(&t, "%c", d->PathName[i] < 0x80 ? d->PathName[i] : '?');
			break;
		}
		case MEDIA_DEVICE_PATH << 8 | MEDIA_PIWG_FW_FILE_DP: {
			const MEDIA_FW_VOL_FILEPATH_DEVICE_PATH *d = (const void *)p;
			tb_add(&t, "FvFile(%g)", &d->FvFileName);
			break;
		}
		case MEDIA_DEVICE_PATH << 8 | MEDIA_PIWG_FW_VOL_DP: {
			const MEDIA_FW_VOL_DEVICE_PATH *d = (const void *)p;
			tb_add(&t, "Fv(%g)", &d->FvName);
			break;
		}
		default:
			tb_add(&t, "Path(%u,%u)", p->Type, p->SubType);
		}
	}
}
