/*
 * BdsDxe: boot device selection (PI 1.8 Vol. 2 12.2 "BDS Architectural
 * Protocol"; UEFI 2.10 ch. 3 "Boot Manager"): the POST screen, setup, the
 * boot menu and the boot manager.
 */
#ifndef BDS_H
#define BDS_H

#include "uefilib.h"
#include "vibe86fw.h"
#include "x86.h"

/* settings (the "Setup" variable, settings.c) */
#define SETUP_GUID EFI_GUID_DEF(0xec87d643, 0xeba4, 0x4bb5, 0xa1, 0xe5, 0x3f, 0x3e, 0x36, 0xb2, 0x0d, 0xa9)
#define SETUP_VERSION	2
struct settings {
	UINT8 version;
	UINT8 quiet_boot;	/* fewer POST messages */
	UINT16 timeout;		/* setup prompt, units of 10 ms */
	UINT8 usb;		/* USB keyboard/mouse/storage */
	UINT8 graphics_post;	/* reserved */
	UINT8 boot_first;	/* index into the boot option types (BOOT_*) of #1 */
	UINT8 boot_second;
	UINT8 boot_third;
	UINT8 boot_fourth;
	UINT8 csm;		/* the CSM: legacy boot options */
	UINT8 pad[5];
};
enum { BOOT_REMOVABLE, BOOT_FWCFG, BOOT_LEGACY, BOOT_SHELL, BOOT_NTYPES, BOOT_DISABLED = 0xFF };
extern struct settings cfg;
void settings_defaults(struct settings *s);
int settings_load(void);		/* 0 ok, -1 defaults */
EFI_STATUS settings_save(void);

/* boot options (boot.c) */
struct boot_option {
	char desc[64];
	int type;			/* BOOT_* */
	EFI_DEVICE_PATH_PROTOCOL *path;	/* what LoadImage gets */
	EFI_HANDLE handle;		/* BOOT_LEGACY: the Block I/O device */
};
#define MAX_BOOT_OPTIONS 16
extern struct boot_option boot_options[MAX_BOOT_OPTIONS];
extern int nboot_options;
void boot_scan(void);
EFI_STATUS boot_run(const struct boot_option *o);	/* returns when the image exits */
void boot_manager(void) __attribute__((noreturn));
const char *boot_type_name(int t);

/* the text UI on ConOut (ui.c) */
#define A_NORMAL	EFI_TEXT_ATTR(EFI_LIGHTGRAY, EFI_BLACK)
#define A_BRIGHT	EFI_TEXT_ATTR(EFI_WHITE, EFI_BLACK)
#define A_BAR		EFI_TEXT_ATTR(EFI_WHITE, EFI_BLUE)
#define A_BODY		EFI_TEXT_ATTR(EFI_BLUE, EFI_LIGHTGRAY)
#define A_INFO		EFI_TEXT_ATTR(EFI_BLACK, EFI_LIGHTGRAY)
#define A_SEL		EFI_TEXT_ATTR(EFI_WHITE, EFI_LIGHTGRAY)
#define A_TABSEL	EFI_TEXT_ATTR(EFI_BLUE, EFI_LIGHTGRAY)
#define A_POP		EFI_TEXT_ATTR(EFI_WHITE, EFI_BLUE)
#define A_POPSEL	EFI_TEXT_ATTR(EFI_BLUE, EFI_LIGHTGRAY)
void ui_attr(UINTN a);
void ui_at(unsigned row, unsigned col);
void ui_puts(const char *s);			/* at the cursor */
void ui_printf(const char *fmt, ...);
void ui_text(unsigned row, unsigned col, UINTN attr, const char *fmt, ...);
void ui_fill(unsigned row, unsigned col, unsigned n, CHAR16 ch, UINTN attr);
void ui_box(unsigned top, unsigned left, unsigned bottom, unsigned right, UINTN attr, int dbl);
void ui_clear(UINTN attr);
void ui_cursor(int on);
int ui_key(EFI_INPUT_KEY *k, uint64_t timeout_us);	/* 0 on timeout */
void ui_flush_keys(void);

/* POST information (bds.c) */
extern char cpu_brand[49];
extern UINT64 mem_total;
extern unsigned usb_keyboards, usb_mice, usb_storage;

/* screens */
void setup_run(void);
int boot_menu(void);		/* the chosen option index, -1 none, -2 setup */
void shell_hint(void);

#endif
