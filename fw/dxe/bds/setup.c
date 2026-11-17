/*
 * BdsDxe: setup, in the style of AMI Aptio's text setup: a title bar, tabs
 * (Main, Advanced, Boot, Security, Save & Exit), the items on the left with
 * the selected item's help on the right, popups for choices, F9 defaults,
 * F10 save and exit. It draws through ConOut, so it shows on the display and
 * (console redirection) on the serial terminal. The layout and the item
 * model come from the vercpu firmware's setup (~/vercpu/fw/dxe/setup.c).
 */
#include "bds.h"
#include "devname.h"

#define BODY_TOP	2
#define BODY_BOT	22
#define SPLIT		54
#define LABEL_COL	2
#define VALUE_COL	30

static struct settings ed;	/* the settings being edited */
static int dirty;

enum { K_BLANK, K_HEAD, K_TEXT, K_OPT, K_NUM, K_SUB, K_ACT };

struct page;
struct item {
	int kind;
	const char *label;
	const char *help;
	UINT8 *val;			/* K_OPT, K_NUM */
	const char *const *choices;	/* K_OPT */
	int nchoices, min, max;		/* K_OPT, K_NUM */
	void (*text)(char *buf, int n, int arg);	/* K_TEXT: the value */
	const struct page *sub;		/* K_SUB */
	int (*act)(int arg);		/* K_ACT: returns 1 to leave setup */
	int arg;
};

struct page {
	const char *title;
	struct item *items;
	int n;
};

static const char *const en_dis[] = { "Disabled", "Enabled" };

/* ---------------------------------------------------------------- values */

static void t_vendor(char *b, int n, int a) { snprintf(b, (size_t)n, "vibe86 project"); }
static void t_version(char *b, int n, int a) { snprintf(b, (size_t)n, "%s %s", FW_NAME, FW_VERSION_STR); }
static void t_build(char *b, int n, int a) { snprintf(b, (size_t)n, "%s", __DATE__); }
static void t_uefi(char *b, int n, int a) { snprintf(b, (size_t)n, "2.10; PI 1.8"); }
static void t_access(char *b, int n, int a) { snprintf(b, (size_t)n, "Administrator"); }
static void t_cpu(char *b, int n, int a) { snprintf(b, (size_t)n, "%.26s", cpu_brand); }
static void t_cpu2(char *b, int n, int a) { snprintf(b, (size_t)n, "%s", strlen(cpu_brand) > 26 ? cpu_brand + 26 : ""); }
static void t_clock(char *b, int n, int a) { snprintf(b, (size_t)n, "%lu MHz", (unsigned long)(TSC_HZ / 1000000)); }
static void t_cores(char *b, int n, int a) { snprintf(b, (size_t)n, "1"); }
static void t_mem(char *b, int n, int a) { snprintf(b, (size_t)n, "%lu MB", (unsigned long)((mem_total + (1 << 19)) >> 20)); }
static void t_csm(char *b, int n, int a) { snprintf(b, (size_t)n, "vibe86 CSM16 %s", FW_VERSION_STR); }
static void t_none(char *b, int n, int a) { snprintf(b, (size_t)n, "Not Installed"); }
static void t_unsup(char *b, int n, int a) { snprintf(b, (size_t)n, "Not Supported"); }

static void
t_date(char *b, int n, int a)
{
	EFI_TIME t;
	if (gRT->GetTime(&t, 0) == EFI_SUCCESS) {
		if (a)
			snprintf(b, (size_t)n, "[%02u:%02u:%02u]", t.Hour, t.Minute, t.Second);
		else
			snprintf(b, (size_t)n, "[%02u/%02u/%04u]", t.Month, t.Day, t.Year);
	} else
		b[0] = 0;
}

/* the PMC's sensors (docs/platform.md "PMC") */
static void
t_sensor(char *b, int n, int a)
{
	static const unsigned reg[] = { 0x20, 0x90, 0x48, 0x50, 0x58, 0x60 };
	uint64_t v = mmio_r64(PMC_BASE + reg[a]);
	if (a < 2)
		snprintf(b, (size_t)n, "+%lu.%lu C", (unsigned long)(v / 1000), (unsigned long)(v % 1000 / 100));
	else
		snprintf(b, (size_t)n, "+%lu.%03lu V", (unsigned long)(v / 1000), (unsigned long)(v % 1000));
}

/* the fan monitor at I2C address 2Fh: tachometers at 20h-23h, RPM big-endian */
static void
t_fan(char *b, int n, int a)
{
	uint64_t i2c = I2C_BASE;
	mmio_w64(i2c + 0x20, 1);			/* RESET */
	mmio_w64(i2c + 0x10, 0x20 + 2 * (unsigned)a);	/* TX: the register pointer */
	mmio_w64(i2c + 0x00, 0x2Fu | 1u << 8 | 2u << 16 | 1u << 31);
	uint64_t st = 0;
	for (int i = 0; i < 100000 && ((st = mmio_r64(i2c + 0x08)) & 1); i++)
		;
	if (!(st & 2) || (st & 4)) {
		snprintf(b, (size_t)n, "N/A");
		return;
	}
	unsigned hi = (unsigned)mmio_r64(i2c + 0x18) & 0xFF, lo = (unsigned)mmio_r64(i2c + 0x18) & 0xFF;
	snprintf(b, (size_t)n, "%u RPM", hi << 8 | lo);
}

/* PCI functions behind the root ports: the arg-th PCI I/O handle on a bus > 0 */
static void
t_pci(char *b, int n, int a)
{
	EFI_HANDLE *hs;
	UINTN cnt;
	b[0] = 0;
	if (gBS->LocateHandleBuffer(ByProtocol, &gEfiPciIoProtocolGuid, 0, &cnt, &hs))
		return;
	int k = 0;
	for (UINTN i = 0; i < cnt; i++) {
		EFI_PCI_IO_PROTOCOL *io;
		UINTN s, bus, d, f;
		gBS->HandleProtocol(hs[i], &gEfiPciIoProtocolGuid, (void **)&io);
		io->GetLocation(io, &s, &bus, &d, &f);
		if (!bus || k++ != a)
			continue;
		UINT32 id;
		io->Pci.Read(io, EfiPciIoWidthUint32, 0, 1, &id);
		static EFI_GUID ng = VIBE86_DEVICE_NAME_GUID;
		VIBE86_DEVICE_NAME *dn;
		if (gBS->HandleProtocol(hs[i], &ng, (void **)&dn) == EFI_SUCCESS)
			snprintf(b, (size_t)n, "%04x:%04x %.30s", id & 0xFFFF, id >> 16, dn->name);
		break;
	}
	free(hs);
}

static void
t_usb(char *b, int n, int a)
{
	static EFI_GUID g = VIBE86_USB_INFO_GUID;
	VIBE86_USB_INFO *u;
	b[0] = 0;
	if (gBS->LocateProtocol(&g, 0, (void **)&u))
		return;
	if (a < 0)
		snprintf(b, (size_t)n, "%u Keyboard, %u Mouse, %u Floppy", u->keyboards, u->mice, u->storage);
	else if ((UINT32)a < u->nlines)
		snprintf(b, (size_t)n, "%.48s", u->lines[a]);
}

static void
t_tpm(char *b, int n, int a)
{
	/* TPM_DID_VID at 48h over I2C address 2Eh (TCG PTP, I2C interface) */
	uint64_t i2c = I2C_BASE;
	mmio_w64(i2c + 0x20, 1);
	mmio_w64(i2c + 0x10, 0x48);
	mmio_w64(i2c + 0x00, 0x2Eu | 1u << 8 | 4u << 16 | 1u << 31);
	uint64_t st = 0;
	for (int i = 0; i < 100000 && ((st = mmio_r64(i2c + 0x08)) & 1); i++)
		;
	snprintf(b, (size_t)n, (st & 2) && !(st & 4) ? "TPM 2.0 (debug, not secure)" : "Not present");
}

/* ---------------------------------------------------------------- pages */

static int act_save_exit(int);
static int act_discard_exit(int);
static int act_save_reset(int);
static int act_discard_reset(int);
static int act_defaults(int);
static int act_boot(int);

#define BLANK		{ K_BLANK, "", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }
#define HEAD(l)		{ K_HEAD, l, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }
#define TEXT(l, f, a)	{ K_TEXT, l, 0, 0, 0, 0, 0, 0, f, 0, 0, a }
#define OPT(l, h, v, c)	{ K_OPT, l, h, v, c, sizeof c / sizeof c[0], 0, 0, 0, 0, 0, 0 }
#define NUM(l, h, v, lo, hi) { K_NUM, l, h, v, 0, 0, lo, hi, 0, 0, 0, 0 }
#define SUB(l, h, p)	{ K_SUB, l, h, 0, 0, 0, 0, 0, 0, p, 0, 0 }
#define ACT(l, h, f, a)	{ K_ACT, l, h, 0, 0, 0, 0, 0, 0, 0, f, a }
#define PAGE(t, a)	{ t, a, sizeof a / sizeof a[0] }

static struct item hwmon_items[] = {
	HEAD("Hardware Monitor"), BLANK,
	TEXT("CPU Temperature", t_sensor, 0), TEXT("System Temperature", t_sensor, 1),
	TEXT("Fan 0 Speed", t_fan, 0), TEXT("Fan 1 Speed", t_fan, 1),
	TEXT("VCORE", t_sensor, 2), TEXT("VDDQ", t_sensor, 3), TEXT("+12V", t_sensor, 4),
	TEXT("+3.3V", t_sensor, 5),
};
static struct item pci_items[] = {
	HEAD("PCI Subsystem Settings"), BLANK,
	TEXT("Device 1", t_pci, 0), TEXT("Device 2", t_pci, 1), TEXT("Device 3", t_pci, 2),
	TEXT("Device 4", t_pci, 3), TEXT("Device 5", t_pci, 4), TEXT("Device 6", t_pci, 5),
	TEXT("Device 7", t_pci, 6), TEXT("Device 8", t_pci, 7),
};
static struct item usb_items[] = {
	HEAD("USB Configuration"), BLANK,
	TEXT("USB Devices:", t_usb, -1), BLANK,
	TEXT("", t_usb, 0), TEXT("", t_usb, 1), TEXT("", t_usb, 2), TEXT("", t_usb, 3),
	BLANK,
	OPT("USB Support", "Enables the xHCI controller and the USB keyboard, mouse and "
	    "mass storage in the firmware (from the next boot).", &ed.usb, en_dis),
};
static struct item tpm_items[] = {
	HEAD("Trusted Computing"), BLANK,
	TEXT("TPM Device", t_tpm, 0),
	TEXT("Measured Boot", t_unsup, 0),
};
static struct item csm_items[] = {
	HEAD("Compatibility Support Module Configuration"), BLANK,
	TEXT("CSM16 Module Version", t_csm, 0), BLANK,
	OPT("CSM Support", "Enables the legacy BIOS (CSM): block devices with a boot "
	    "sector become legacy boot options, booted in real mode.", &ed.csm, en_dis),
};
static const struct page hwmon_page = PAGE("Hardware Monitor", hwmon_items);
static const struct page csm_page = PAGE("CSM Configuration", csm_items);
static const struct page pci_page = PAGE("PCI Subsystem Settings", pci_items);
static const struct page usb_page = PAGE("USB Configuration", usb_items);
static const struct page tpm_page = PAGE("Trusted Computing", tpm_items);

static struct item main_items[] = {
	HEAD("BIOS Information"),
	TEXT("BIOS Vendor", t_vendor, 0),
	TEXT("Core Version", t_version, 0),
	TEXT("Compliancy", t_uefi, 0),
	TEXT("Build Date", t_build, 0),
	TEXT("Access Level", t_access, 0),
	BLANK,
	HEAD("Processor Information"),
	TEXT("Processor", t_cpu, 0),
	TEXT("", t_cpu2, 0),
	TEXT("Processor Speed", t_clock, 0),
	TEXT("Processor Cores", t_cores, 0),
	BLANK,
	TEXT("Total Memory", t_mem, 0),
	BLANK,
	TEXT("System Date", t_date, 0),
	TEXT("System Time", t_date, 1),
};
static struct item adv_items[] = {
	SUB("Hardware Monitor", "Temperatures, fan speeds and voltages.", &hwmon_page),
	SUB("PCI Subsystem Settings", "The PCI Express devices behind the root ports.", &pci_page),
	SUB("USB Configuration", "USB devices and USB support.", &usb_page),
	SUB("Trusted Computing", "The TPM.", &tpm_page),
	SUB("CSM Configuration", "The Compatibility Support Module: legacy boot.", &csm_page),
};
static const char *boot_choices[BOOT_NTYPES + 1];
static struct item boot_items[] = {
	HEAD("Boot Configuration"),
	NUM("Setup Prompt Timeout", "Time to wait for the setup and boot menu keys, in "
	    "units of 10 ms (the simulator runs about 150x slower than real time).", 0, 0, 250),
	OPT("Quiet Boot", "Shows fewer POST messages.", &ed.quiet_boot, en_dis),
	BLANK,
	HEAD("Boot Option Priorities"),
	{ K_OPT, "Boot Option #1", "Sets the system boot order.", &ed.boot_first, boot_choices, BOOT_NTYPES + 1, 0, 0, 0, 0, 0, 1 },
	{ K_OPT, "Boot Option #2", "Sets the system boot order.", &ed.boot_second, boot_choices, BOOT_NTYPES + 1, 0, 0, 0, 0, 0, 1 },
	{ K_OPT, "Boot Option #3", "Sets the system boot order.", &ed.boot_third, boot_choices, BOOT_NTYPES + 1, 0, 0, 0, 0, 0, 1 },
	{ K_OPT, "Boot Option #4", "Sets the system boot order.", &ed.boot_fourth, boot_choices, BOOT_NTYPES + 1, 0, 0, 0, 0, 0, 1 },
};
static struct item sec_items[] = {
	HEAD("Password Description"),
	TEXT("Administrator Password", t_none, 0),
	TEXT("User Password", t_none, 0),
	BLANK,
	TEXT("Secure Boot", t_unsup, 0),
	SUB("Trusted Computing", "The TPM.", &tpm_page),
};
#define MAX_EXIT 24
static struct item exit_items[MAX_EXIT];
static int nexit;

static struct page tabs[] = {
	{ "Main", main_items, ARRAY_SIZE(main_items) },
	{ "Advanced", adv_items, ARRAY_SIZE(adv_items) },
	{ "Boot", boot_items, ARRAY_SIZE(boot_items) },
	{ "Security", sec_items, ARRAY_SIZE(sec_items) },
	{ "Save & Exit", exit_items, 0 },
};

static void
build_exit_page(void)
{
	static const struct item fixed[] = {
		HEAD("Save Options"),
		ACT("Save Changes and Exit", "Exit setup after saving the changes.", act_save_exit, 0),
		ACT("Discard Changes and Exit", "Exit setup without saving any changes.", act_discard_exit, 0),
		ACT("Save Changes and Reset", "Reset the system after saving the changes.", act_save_reset, 0),
		ACT("Discard Changes and Reset", "Reset the system without saving any changes.", act_discard_reset, 0),
		BLANK,
		HEAD("Default Options"),
		ACT("Restore Defaults", "Restore/load default values for all the setup options.", act_defaults, 0),
		BLANK,
		HEAD("Boot Override"),
	};
	nexit = 0;
	for (unsigned i = 0; i < ARRAY_SIZE(fixed); i++)
		exit_items[nexit++] = fixed[i];
	boot_scan();
	for (int i = 0; i < nboot_options && nexit < MAX_EXIT; i++)
		exit_items[nexit++] = (struct item)ACT(boot_options[i].desc,
		    "Boot this device now (the changes are kept for this boot).", act_boot, i);
	tabs[4].n = nexit;
}

/* ---------------------------------------------------------------- drawing */

static int
selectable(const struct item *it)
{
	return it->kind == K_OPT || it->kind == K_NUM || it->kind == K_SUB || it->kind == K_ACT;
}

static void
value_of(const struct item *it, char *b, int n)
{
	b[0] = 0;
	switch (it->kind) {
	case K_TEXT:
		it->text(b, n, it->arg);
		break;
	case K_OPT:
		if (it->val)
			snprintf(b, (size_t)n, "[%s]", *it->val < it->nchoices ? it->choices[*it->val] :
			    it->choices[it->nchoices - 1]);
		break;
	case K_NUM:
		snprintf(b, (size_t)n, "[%u]", it->val ? *it->val : (unsigned)ed.timeout);
		break;
	}
}

static void
draw_frame(int tab, const char *title)
{
	ui_fill(0, 0, 80, ' ', A_BAR);
	ui_text(0, 11, A_BAR, "Aptio-style Setup Utility - Copyright (C) 2026 The vibe86 project");
	ui_fill(1, 0, 80, ' ', A_BAR);
	unsigned col = 1;
	for (int i = 0; i < (int)ARRAY_SIZE(tabs); i++) {
		ui_text(1, col, i == tab ? A_TABSEL : A_BAR, " %s ", tabs[i].title);
		col += (unsigned)strlen(tabs[i].title) + 3;
	}
	(void)title;
	for (unsigned r = BODY_TOP; r <= BODY_BOT; r++) {
		ui_fill(r, 0, SPLIT, ' ', A_BODY);
		ui_fill(r, SPLIT, 1, BOXDRAW_VERTICAL, A_BODY);
		ui_fill(r, SPLIT + 1, 80 - SPLIT - 1, ' ', A_BODY);
	}
	ui_fill(BODY_BOT + 1, 0, 80, ' ', A_BAR);
	ui_text(BODY_BOT + 1, 1, A_BAR, "Version %s %s. Copyright (C) 2026 The vibe86 project.", FW_NAME,
	    FW_VERSION_STR);
}

static void
draw_keys(void)
{
	static const char *const keys[] = {
		"\x1b\x1a: Select Screen", "\x18\x19: Select Item", "Enter: Select", "+/-: Change Opt.",
		"F1: General Help", "F9: Optimized Defaults", "F10: Save & Exit", "ESC: Exit",
	};
	ui_fill(BODY_TOP + 10, SPLIT + 1, 80 - SPLIT - 1, BOXDRAW_HORIZONTAL, A_BODY);
	for (unsigned i = 0; i < ARRAY_SIZE(keys); i++)
		ui_text(BODY_TOP + 11 + i, SPLIT + 2, A_BODY, "%s", keys[i]);
}

static void
draw_help(const struct item *it)
{
	for (unsigned r = BODY_TOP; r < BODY_TOP + 10; r++)
		ui_fill(r, SPLIT + 1, 80 - SPLIT - 1, ' ', A_BODY);
	if (!it || !it->help)
		return;
	/* word-wrap into the help column */
	const char *s = it->help;
	unsigned w = 80 - SPLIT - 3, row = BODY_TOP;
	while (*s && row < BODY_TOP + 10) {
		unsigned n = (unsigned)strlen(s);
		if (n > w) {
			n = w;
			while (n && s[n] != ' ')
				n--;
			if (!n)
				n = w;
		}
		ui_text(row++, SPLIT + 2, A_BODY, "%.*s", (int)n, s);
		s += n;
		while (*s == ' ')
			s++;
	}
}

static void
draw_items(const struct page *p, int sel, int top)
{
	char v[64];
	int rows = BODY_BOT - BODY_TOP + 1;
	for (int r = 0; r < rows; r++) {
		int i = top + r;
		unsigned row = BODY_TOP + (unsigned)r;
		ui_fill(row, 0, SPLIT, ' ', A_BODY);
		if (i >= p->n)
			continue;
		const struct item *it = &p->items[i];
		UINTN a = it->kind == K_HEAD ? A_BODY : it->kind == K_TEXT ? A_INFO : A_BODY;
		if (i == sel)
			a = A_SEL;
		if (it->kind == K_SUB)
			ui_text(row, LABEL_COL - 1, a, "\x10 %s", it->label);
		else
			ui_text(row, LABEL_COL, a, "%s", it->label);
		value_of(it, v, sizeof v);
		if (v[0])
			ui_text(row, VALUE_COL, i == sel ? A_SEL : A_INFO, "%.23s", v);
	}
}

/* a popup list: returns the choice or -1 */
static int
popup(const char *title, const char *const *ch, int n, int cur)
{
	unsigned w = 34, h = (unsigned)n + 3, top = (25 - h) / 2, left = (80 - w) / 2;
	int sel = cur;
	for (;;) {
		ui_box(top, left, top + h - 1, left + w, A_POP, 0);
		ui_text(top, left + (w - (unsigned)strlen(title)) / 2, A_POP, "%s", title);
		for (int i = 0; i < n; i++) {
			ui_fill(top + 1 + (unsigned)i, left + 1, w - 1, ' ', i == sel ? A_POPSEL : A_POP);
			ui_text(top + 1 + (unsigned)i, left + 2, i == sel ? A_POPSEL : A_POP, "%.30s", ch[i]);
		}
		EFI_INPUT_KEY k;
		ui_key(&k, 0);
		if (k.ScanCode == SCAN_UP && sel > 0)
			sel--;
		else if (k.ScanCode == SCAN_DOWN && sel < n - 1)
			sel++;
		else if (k.ScanCode == SCAN_ESC)
			return -1;
		else if (k.UnicodeChar == CHAR_CARRIAGE_RETURN)
			return sel;
	}
}

static int
confirm(const char *title, const char *q)
{
	static const char *const yn[] = { "Yes", "No" };
	(void)q;
	return popup(title, yn, 2, 0) == 0;
}

/* ---------------------------------------------------------------- actions */

static int
act_save_exit(int a)
{
	if (!confirm("Save & Exit Setup", "Save configuration and exit?"))
		return 0;
	cfg = ed;
	settings_save();
	return 1;
}

static int
act_discard_exit(int a)
{
	if (!confirm("Exit Without Saving", "Quit without saving?"))
		return 0;
	return 1;
}

static int
act_save_reset(int a)
{
	if (!confirm("Save & reset", "Save configuration and reset?"))
		return 0;
	cfg = ed;
	settings_save();
	gRT->ResetSystem(EfiResetCold, EFI_SUCCESS, 0, 0);
	return 1;
}

static int
act_discard_reset(int a)
{
	if (!confirm("Reset Without Saving", "Reset without saving?"))
		return 0;
	gRT->ResetSystem(EfiResetCold, EFI_SUCCESS, 0, 0);
	return 1;
}

static int
act_defaults(int a)
{
	if (!confirm("Load Optimized Defaults", "Load Optimized Defaults?"))
		return 0;
	settings_defaults(&ed);
	dirty = 1;
	return 0;
}

static int boot_now = -1;

static int
act_boot(int a)
{
	cfg = ed;
	boot_now = a;
	return 1;
}

/* ---------------------------------------------------------------- the loop */

static int
run_page(const struct page *p, int tab)
{
	int sel = 0, top = 0;
	while (sel < p->n && !selectable(&p->items[sel]))
		sel++;
	if (sel == p->n)
		sel = -1;
	for (;;) {
		draw_items(p, sel, top);
		draw_help(sel >= 0 ? &p->items[sel] : 0);
		EFI_INPUT_KEY k;
		ui_key(&k, 0);
		const struct item *it = sel >= 0 ? &p->items[sel] : 0;
		if (k.ScanCode == SCAN_LEFT)
			return -1;
		if (k.ScanCode == SCAN_RIGHT)
			return -2;
		if (k.ScanCode == SCAN_ESC)
			return -3;
		if (k.ScanCode == SCAN_F9) {
			act_defaults(0);
			continue;
		}
		if (k.ScanCode == SCAN_F10) {
			if (act_save_exit(0))
				return 1;
			continue;
		}
		if (k.ScanCode == SCAN_UP || k.ScanCode == SCAN_DOWN) {
			int d = k.ScanCode == SCAN_UP ? -1 : 1, i = sel;
			do
				i += d;
			while (i >= 0 && i < p->n && !selectable(&p->items[i]));
			if (i >= 0 && i < p->n)
				sel = i;
			int rows = BODY_BOT - BODY_TOP + 1;
			if (sel < top)
				top = sel;
			if (sel >= top + rows)
				top = sel - rows + 1;
			continue;
		}
		if (!it)
			continue;
		int ch = k.UnicodeChar;
		if (it->kind == K_OPT && (ch == '+' || ch == '-')) {
			int v = *it->val >= it->nchoices ? it->nchoices - 1 : *it->val;
			v = (v + (ch == '+' ? 1 : it->nchoices - 1)) % it->nchoices;
			*it->val = (UINT8)(it->arg == 1 && v == BOOT_NTYPES ? BOOT_DISABLED : v);
			dirty = 1;
		} else if (it->kind == K_NUM && (ch == '+' || ch == '-')) {
			int v = (int)ed.timeout + (ch == '+' ? 1 : -1);
			if (v >= it->min && v <= it->max)
				ed.timeout = (UINT16)v;
			dirty = 1;
		} else if (ch == CHAR_CARRIAGE_RETURN) {
			if (it->kind == K_OPT) {
				int v = popup(it->label, it->choices, it->nchoices,
				    *it->val >= it->nchoices ? it->nchoices - 1 : *it->val);
				if (v >= 0) {
					*it->val = (UINT8)(it->arg == 1 && v == BOOT_NTYPES ? BOOT_DISABLED : v);
					dirty = 1;
				}
				draw_frame(tab, 0);
				draw_keys();
			} else if (it->kind == K_SUB) {
				draw_frame(tab, it->sub->title);
				draw_keys();
				int r = run_page(it->sub, tab);
				if (r == 1)
					return 1;
				draw_frame(tab, 0);
				draw_keys();
			} else if (it->kind == K_ACT) {
				int r = it->act(it->arg);
				if (r)
					return 1;
				draw_frame(tab, 0);
				draw_keys();
			}
		}
	}
}

void
setup_run(void)
{
	ed = cfg;
	dirty = 0;
	boot_now = -1;
	for (int i = 0; i < BOOT_NTYPES; i++)
		boot_choices[i] = boot_type_name(i);
	boot_choices[BOOT_NTYPES] = "Disabled";
	build_exit_page();
	ui_cursor(0);
	int tab = 0;
	for (;;) {
		draw_frame(tab, 0);
		draw_keys();
		int r = run_page(&tabs[tab], tab);
		if (r == 1)
			break;
		if (r == -1)
			tab = (tab + (int)ARRAY_SIZE(tabs) - 1) % (int)ARRAY_SIZE(tabs);
		else if (r == -2)
			tab = (tab + 1) % (int)ARRAY_SIZE(tabs);
		else if (r == -3) {
			if (act_discard_exit(0))
				break;
		}
	}
	ui_clear(A_NORMAL);
	if (boot_now >= 0)
		boot_run(&boot_options[boot_now]);
}
