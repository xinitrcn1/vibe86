/*
 * UsbDxe: the driver's entry. It takes every xHCI controller (PCI class
 * 0C/03/30) among the PCI I/O handles, turns on its memory space and bus
 * mastering through PciIo->Attributes (which also enables the root port
 * above it: the vibe86 root ports enforce BME), and gives each USB device a
 * handle with the controller's device path plus a USB() node.
 */
#include "usb.h"

#define MAX_HC 1

static EFI_GUID usb_info_guid = VIBE86_USB_INFO_GUID;

EFI_HANDLE
usb_install(struct udev *d, EFI_GUID *g1, void *i1, EFI_GUID *g2, void *i2)
{
	USB_DEVICE_PATH n;
	dp_set_node(&n.Header, MESSAGING_DEVICE_PATH, MSG_USB_DP, sizeof n);
	n.ParentPortNumber = (UINT8)(d->port - 1);
	n.InterfaceNumber = (UINT8)d->iface;
	EFI_DEVICE_PATH_PROTOCOL *p = dp_append_node(hc.path, &n.Header);
	EFI_HANDLE h = 0;
	EFI_STATUS s;
	if (g2)
		s = gBS->InstallMultipleProtocolInterfaces(&h, &gEfiDevicePathProtocolGuid, p, g1, i1, g2, i2, (void *)0);
	else
		s = gBS->InstallMultipleProtocolInterfaces(&h, &gEfiDevicePathProtocolGuid, p, g1, i1, (void *)0);
	if (EFI_ERROR(s))
		DEBUG("USB: port %d: install: %r\n", d->port, s);
	if (hc.info.nlines < ARRAY_SIZE(hc.info.lines))
		snprintf(hc.info.lines[hc.info.nlines++], sizeof hc.info.lines[0], "Port %d: %s (%s)",
		    d->port, d->name, d->kind == K_KBD ? "keyboard" : d->kind == K_MOUSE ? "mouse" :
		    "mass storage");
	return h;
}

static VOID EFIAPI
on_exit_boot_services(EFI_EVENT e, VOID *ctx)
{
	xhci_halt();
}

static VOID EFIAPI
on_timer(EFI_EVENT e, VOID *ctx)
{
	usb_poll();
}

EFI_STATUS EFIAPI
efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st)
{
	uefi_init(image, st);
	EFI_HANDLE *hs;
	UINTN n;
	if (gBS->LocateHandleBuffer(ByProtocol, &gEfiPciIoProtocolGuid, 0, &n, &hs) != EFI_SUCCESS)
		return EFI_NOT_FOUND;
	EFI_PCI_IO_PROTOCOL *io = 0;
	for (UINTN i = 0; i < n; i++) {
		EFI_PCI_IO_PROTOCOL *p;
		UINT32 class;
		if (gBS->HandleProtocol(hs[i], &gEfiPciIoProtocolGuid, (void **)&p))
			continue;
		p->Pci.Read(p, EfiPciIoWidthUint32, 8, 1, &class);
		if ((class >> 8) != 0x0C0330)
			continue;
		io = p;
		hc.path = dp_of(hs[i]);
		break;
	}
	free(hs);
	if (!io) {
		DEBUG("USB: no xHCI controller\n");
		return EFI_NOT_FOUND;
	}
	io->Attributes(io, EfiPciIoAttributeOperationEnable,
	    EFI_PCI_IO_ATTRIBUTE_MEMORY | EFI_PCI_IO_ATTRIBUTE_BUS_MASTER, 0);
	/* BAR0's address from the BAR registers (64-bit memory BAR) */
	UINT32 lo, hi;
	io->Pci.Read(io, EfiPciIoWidthUint32, 0x10, 1, &lo);
	io->Pci.Read(io, EfiPciIoWidthUint32, 0x14, 1, &hi);
	uint64_t bar0 = ((uint64_t)hi << 32 | lo) & ~0xFull;
	hc.info.controllers = 1;
	int r = xhci_init(bar0);
	if (r < 0) {
		DEBUG("USB: xHCI controller at %lx: error %d\n", bar0, r);
		return EFI_DEVICE_ERROR;
	}
	DEBUG("USB: xHCI at %lx: %d device(s) with drivers\n", bar0, r);
	EFI_HANDLE h = 0;
	gBS->InstallProtocolInterface(&h, &usb_info_guid, EFI_NATIVE_INTERFACE, &hc.info);
	EFI_EVENT ev;
	gBS->CreateEvent(EVT_SIGNAL_EXIT_BOOT_SERVICES, TPL_NOTIFY, on_exit_boot_services, 0, &ev);
	gBS->CreateEvent(EVT_TIMER | EVT_NOTIFY_SIGNAL, TPL_NOTIFY, on_timer, 0, &ev);
	gBS->SetTimer(ev, TimerPeriodic, 100000);	/* 10 ms */
	return EFI_SUCCESS;
}
