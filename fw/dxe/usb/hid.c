/*
 * UsbDxe: HID boot keyboards as Simple Text Input (UEFI 2.10 12.3) and boot
 * mice as Simple Pointer (12.5). The keyboard report is HID 1.11 Appendix
 * B.1 (modifiers, reserved, six usages of the Keyboard/Keypad page, HUT
 * 10), translated for a US layout; held keys repeat after 500 ms at about
 * 30 per second; Caps and Num Lock drive the LEDs (SET_REPORT, HID 7.2.2).
 */
#include "usb.h"

#define REPEAT_DELAY_US	500000
#define REPEAT_US	33333

struct kbd {
	EFI_SIMPLE_TEXT_INPUT_PROTOCOL sti;	/* first: This */
	struct udev *d;
	EFI_INPUT_KEY q[16];
	unsigned h, n;
	unsigned rep_usage;
	uint64_t rep_at;
	uint8_t mods, leds;
	int leds_dirty;
};

struct mouse {
	EFI_SIMPLE_POINTER_PROTOCOL sp;		/* first: This */
	struct udev *d;
	EFI_SIMPLE_POINTER_MODE mode;
	int dx, dy, buttons, changed;
};

static struct kbd *kbds[MAXDEV];
static struct mouse *mice[MAXDEV];
static unsigned nkbds, nmice;

/* ---- keyboards */

static void
key_push(struct kbd *k, UINT16 scan, CHAR16 ch)
{
	if ((!scan && !ch) || k->n == ARRAY_SIZE(k->q))
		return;
	k->q[(k->h + k->n) % ARRAY_SIZE(k->q)] = (EFI_INPUT_KEY){ scan, ch };
	k->n++;
}

/* a Keyboard/Keypad usage (HUT 10) as a UEFI key, US layout */
static EFI_INPUT_KEY
translate(struct kbd *k, unsigned u)
{
	static const char row[] = "1234567890\r\x1b\b\t -=[]\\#;'`,./";
	static const char rowsh[] = "!@#$%^&*()\r\x1b\b\t _+{}|~:\"~<>?";
	static const UINT16 nav[] = { SCAN_INSERT, SCAN_HOME, SCAN_PAGE_UP, SCAN_DELETE, SCAN_END,
	    SCAN_PAGE_DOWN, SCAN_RIGHT, SCAN_LEFT, SCAN_DOWN, SCAN_UP };
	static const UINT16 kpnav[] = { SCAN_END, SCAN_DOWN, SCAN_PAGE_DOWN, SCAN_LEFT, SCAN_NULL,
	    SCAN_RIGHT, SCAN_HOME, SCAN_UP, SCAN_PAGE_UP, SCAN_INSERT, SCAN_DELETE };
	int shift = (k->mods & 0x22) != 0, ctrl = (k->mods & 0x11) != 0;
	int caps = (k->leds & 2) != 0, num = (k->leds & 1) != 0;
	EFI_INPUT_KEY r = { 0, 0 };

	if (u >= 0x04 && u <= 0x1D) {			/* a-z */
		int c = 'a' + (int)(u - 0x04);
		r.UnicodeChar = (CHAR16)(ctrl ? c - 'a' + 1 : (shift ^ caps) ? c - 'a' + 'A' : c);
	} else if (u >= 0x1E && u <= 0x38) {		/* digits, Enter .. / */
		char c = (shift ? rowsh : row)[u - 0x1E];
		if (c == 0x1b)
			r.ScanCode = SCAN_ESC;
		else
			r.UnicodeChar = (unsigned char)c;
	} else if (u >= 0x3A && u <= 0x45) {
		r.ScanCode = (UINT16)(SCAN_F1 + (u - 0x3A));
	} else if (u >= 0x49 && u <= 0x52) {
		r.ScanCode = nav[u - 0x49];
	} else if (u >= 0x59 && u <= 0x63) {		/* keypad 1-9 0 . */
		if (num && !shift)
			r.UnicodeChar = (unsigned char)"1234567890."[u - 0x59];
		else
			r.ScanCode = kpnav[u - 0x59];
	} else {
		switch (u) {
		case 0x54: r.UnicodeChar = '/'; break;
		case 0x55: r.UnicodeChar = '*'; break;
		case 0x56: r.UnicodeChar = '-'; break;
		case 0x57: r.UnicodeChar = '+'; break;
		case 0x58: r.UnicodeChar = '\r'; break;
		case 0x64: r.UnicodeChar = shift ? '|' : '\\'; break;	/* non-US \ | */
		}
	}
	return r;
}

static int
held(const volatile uint8_t *r, unsigned u)
{
	for (int i = 2; i < 8; i++)
		if (r[i] == u)
			return 1;
	return 0;
}

void
key_report(struct udev *d, const volatile uint8_t *r, unsigned n)
{
	struct kbd *k = d->priv;
	if (!k || n < 8 || r[2] == 0x01)	/* short, or ErrorRollOver: ignore */
		return;
	k->mods = r[0];
	for (int i = 2; i < 8; i++) {
		unsigned u = r[i];
		if (u < 4 || held(d->prev, u))
			continue;
		if (u == 0x39 || u == 0x53 || u == 0x47) {	/* Caps, Num, Scroll Lock */
			k->leds ^= u == 0x53 ? 1 : u == 0x39 ? 2 : 4;
			k->leds_dirty = 1;
			continue;
		}
		EFI_INPUT_KEY key = translate(k, u);
		if (!key.ScanCode && !key.UnicodeChar)
			continue;
		key_push(k, key.ScanCode, key.UnicodeChar);
		k->rep_usage = u;
		k->rep_at = time_us() + REPEAT_DELAY_US;
	}
	if (k->rep_usage && !held(r, k->rep_usage))
		k->rep_usage = 0;
	for (int i = 0; i < 8; i++)
		d->prev[i] = r[i];
}

static EFI_STATUS EFIAPI
kbd_reset(EFI_SIMPLE_TEXT_INPUT_PROTOCOL *This, BOOLEAN Ext)
{
	struct kbd *k = (struct kbd *)This;
	usb_poll();
	k->n = 0;
	k->rep_usage = 0;
	return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
kbd_read(EFI_SIMPLE_TEXT_INPUT_PROTOCOL *This, EFI_INPUT_KEY *Key)
{
	struct kbd *k = (struct kbd *)This;
	usb_poll();
	if (!k->n && k->rep_usage) {
		uint64_t now = time_us();
		if ((int64_t)(now - k->rep_at) >= 0) {
			EFI_INPUT_KEY key = translate(k, k->rep_usage);
			key_push(k, key.ScanCode, key.UnicodeChar);
			k->rep_at = now - k->rep_at > REPEAT_US ? now + REPEAT_US : k->rep_at + REPEAT_US;
		}
	}
	if (!k->n)
		return EFI_NOT_READY;
	*Key = k->q[k->h];
	k->h = (k->h + 1) % ARRAY_SIZE(k->q);
	k->n--;
	return EFI_SUCCESS;
}

static VOID EFIAPI
kbd_wait(EFI_EVENT ev, VOID *ctx)
{
	struct kbd *k = ctx;
	usb_poll();
	if (k->n)
		gBS->SignalEvent(ev);
}

/* ---- mice */

void
mouse_report(struct udev *d, const volatile uint8_t *r, unsigned n)
{
	struct mouse *m = d->priv;
	if (!m || n < 3)
		return;
	if ((r[0] & 7) != m->buttons || r[1] || r[2])
		m->changed = 1;
	m->buttons = r[0] & 7;
	m->dx += (int8_t)r[1];
	m->dy += (int8_t)r[2];
}

static EFI_STATUS EFIAPI
mouse_reset(EFI_SIMPLE_POINTER_PROTOCOL *This, BOOLEAN Ext)
{
	struct mouse *m = (struct mouse *)This;
	m->dx = m->dy = 0;
	m->changed = 0;
	return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
mouse_get_state(EFI_SIMPLE_POINTER_PROTOCOL *This, EFI_SIMPLE_POINTER_STATE *State)
{
	struct mouse *m = (struct mouse *)This;
	usb_poll();
	if (!m->changed)
		return EFI_NOT_READY;
	State->RelativeMovementX = m->dx;
	State->RelativeMovementY = m->dy;
	State->RelativeMovementZ = 0;
	State->LeftButton = (BOOLEAN)(m->buttons & 1);
	State->RightButton = (BOOLEAN)((m->buttons >> 1) & 1);
	m->dx = m->dy = 0;
	m->changed = 0;
	return EFI_SUCCESS;
}

static VOID EFIAPI
mouse_wait(EFI_EVENT ev, VOID *ctx)
{
	struct mouse *m = ctx;
	usb_poll();
	if (m->changed)
		gBS->SignalEvent(ev);
}

/* ---- */

void
hid_start(struct udev *d)
{
	if (d->kind == K_KBD) {
		struct kbd *k = calloc(1, sizeof *k);
		if (!k)
			return;
		k->d = d;
		k->leds = 1;			/* Num Lock on */
		k->leds_dirty = 1;
		k->sti.Reset = kbd_reset;
		k->sti.ReadKeyStroke = kbd_read;
		gBS->CreateEvent(EVT_NOTIFY_WAIT, TPL_NOTIFY, kbd_wait, k, &k->sti.WaitForKey);
		d->priv = k;
		kbds[nkbds++] = k;
		hc.info.keyboards++;
		usb_install(d, &gEfiSimpleTextInProtocolGuid, &k->sti, 0, 0);
	} else {
		struct mouse *m = calloc(1, sizeof *m);
		if (!m)
			return;
		m->d = d;
		m->mode = (EFI_SIMPLE_POINTER_MODE){ 1, 1, 0, TRUE, TRUE };
		m->sp.Reset = mouse_reset;
		m->sp.GetState = mouse_get_state;
		m->sp.Mode = &m->mode;
		gBS->CreateEvent(EVT_NOTIFY_WAIT, TPL_NOTIFY, mouse_wait, m, &m->sp.WaitForInput);
		d->priv = m;
		mice[nmice++] = m;
		hc.info.mice++;
		usb_install(d, &gEfiSimplePointerProtocolGuid, &m->sp, 0, 0);
	}
}

void
hid_poll(void)
{
	for (unsigned i = 0; i < nkbds; i++) {
		struct kbd *k = kbds[i];
		if (k->leds_dirty && k->d->kind == K_KBD && hc.ok) {
			uint8_t v = k->leds;
			k->leds_dirty = 0;
			control(k->d, 0x21, 0x09, 0x0200, (int)k->d->iface, &v, 1);
		}
	}
}
