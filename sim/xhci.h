/*
 * vsim xHCI USB host controller (a PCIe endpoint) with a USB HID boot
 * keyboard on port 1 and a USB HID boot mouse on port 2.
 *
 * Follows Intel, "eXtensible Host Controller Interface for Universal Serial
 * Bus (xHCI)", revision 1.2 (May 2019),
 * https://www.intel.com/content/www/us/en/io/universal-serial-bus/extensible-host-controler-interface-usb-xhci.html
 * for the subset in docs/platform.md "USB (xHCI)"; the devices follow the
 * USB-IF "Device Class Definition for Human Interface Devices (HID)" 1.11
 * (https://www.usb.org/document-library/device-class-definition-hid-111),
 * Appendix B (boot interface), and the "HID Usage Tables"
 * (https://usb.org/document-library/hid-usage-tables-15) for key usages.
 */
#ifndef XHCI_H
#define XHCI_H

#include "pcie.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PCI_DEV_XHCI	0x0C30
#define XHCI_BAR_SIZE	0x10000ull	/* BAR0: 64 KiB */

struct pcidev *xhci_new(void);

/* input to the devices (vsim's window and -K) */
/* a key of the HID Keyboard/Keypad page (0x07): usage 0x04..0xE7; the
 * modifiers 0xE0..0xE7 go to the report's modifier byte */
void xhci_key(struct pcidev *d, unsigned usage, int down);
/* release every key (e.g. the window lost the focus) */
void xhci_key_release_all(struct pcidev *d);
/* relative mouse motion, accumulated until the host fetches a report */
void xhci_mouse_move(struct pcidev *d, int dx, int dy);
/* mouse buttons: bit 0 left, 1 right, 2 middle */
void xhci_mouse_button(struct pcidev *d, unsigned bit, int down);

#ifdef __cplusplus
}
#endif

#endif
