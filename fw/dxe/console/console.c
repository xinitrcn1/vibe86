/*
 * ConsoleDxe: the system console (UEFI 2.10 12.1 "Console I/O"):
 *
 *  - a terminal on the Serial I/O device: VT100 output (cursor addressing,
 *    SGR colours; box drawing as ASCII), xterm/VT220 input decoding and the
 *    VT100+ function keys of console redirection (ESC 1 .. ESC 0 = F1-F10,
 *    ESC ! = F11, ESC @ = F12), as UEFI 2.10 12.8 and the terminal types of
 *    11.11 describe;
 *  - splitters: ConOut writes to the display's and the terminal's Simple
 *    Text Output protocols; ConIn merges the terminal with every Simple Text
 *    Input protocol that appears later (USB keyboards), found through a
 *    protocol notification.
 *
 * The splitters are installed on their own handle and put into the system
 * table (ConIn, ConOut, StdErr).
 */
#include "uefilib.h"
#include "x86.h"

#define MAX_OUT	4
#define MAX_IN	8

/* ---------------------------------------------------------------- terminal */

static EFI_SERIAL_IO_PROTOCOL *sio;
static SIMPLE_TEXT_OUTPUT_MODE term_mode;
static EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL term_out;
static int term_attr_sent = -1;

static void
term_send(const char *s, UINTN n)
{
	sio->Write(sio, &n, (void *)s);
}

static void
term_sends(const char *s)
{
	term_send(s, strlen(s));
}

static char
ascii_of(CHAR16 c)
{
	if (c >= 0x20 && c < 0x7F)
		return (char)c;
	switch (c) {
	case 0x2500: case 0x2550: return '-';
	case 0x2502: case 0x2551: return '|';
	case 0x2191: case 0x25B2: return '^';
	case 0x2193: case 0x25BC: return 'v';
	case 0x2192: case 0x25BA: return '>';
	case 0x2190: case 0x25C4: return '<';
	case 0x2588: case 0x2591: case 0x2592: case 0x2593: return '#';
	case 0x00B7: case 0x2022: return '*';
	}
	if (c >= 0x2500 && c <= 0x257F)
		return '+';
	return '?';
}

static void
term_attr(void)
{
	/* ANSI colours are ordered black red green yellow blue magenta cyan
	   white; UEFI's (CGA's) black blue green cyan red magenta brown gray */
	static const char map[8] = { 0, 4, 2, 6, 1, 5, 3, 7 };
	if (term_mode.Attribute == term_attr_sent)
		return;
	char b[24];
	int fg = term_mode.Attribute & 15, bg = (term_mode.Attribute >> 4) & 7;
	snprintf(b, sizeof b, "\033[0;%s3%d;4%dm", fg & 8 ? "1;" : "", map[fg & 7], map[bg]);
	term_sends(b);
	term_attr_sent = term_mode.Attribute;
}

static EFI_STATUS EFIAPI
term_output_string(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, CHAR16 *s)
{
	char buf[128];
	UINTN n = 0;
	term_attr();
	for (; *s; s++) {
		CHAR16 c = *s;
		if (c == '\r') {
			term_mode.CursorColumn = 0;
			buf[n++] = '\r';
		} else if (c == '\n') {
			if (term_mode.CursorRow < 24)
				term_mode.CursorRow++;
			buf[n++] = '\n';
		} else if (c == '\b') {
			if (term_mode.CursorColumn)
				term_mode.CursorColumn--;
			buf[n++] = '\b';
		} else {
			buf[n++] = ascii_of(c);
			if (++term_mode.CursorColumn == 80) {
				term_mode.CursorColumn = 0;
				if (term_mode.CursorRow < 24)
					term_mode.CursorRow++;
			}
		}
		if (n == sizeof buf) {
			term_send(buf, n);
			n = 0;
		}
	}
	if (n)
		term_send(buf, n);
	return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
term_test_string(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, CHAR16 *s)
{
	return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
term_query_mode(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, UINTN Mode, UINTN *Cols, UINTN *Rows)
{
	if (Mode)
		return EFI_UNSUPPORTED;
	*Cols = 80;
	*Rows = 25;
	return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
term_clear_screen(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This)
{
	term_attr();
	term_sends("\033[2J\033[1;1H");
	term_mode.CursorColumn = term_mode.CursorRow = 0;
	return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
term_set_mode(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, UINTN Mode)
{
	return Mode ? EFI_UNSUPPORTED : term_clear_screen(This);
}

static EFI_STATUS EFIAPI
term_set_attribute(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, UINTN Attr)
{
	term_mode.Attribute = (INT32)(Attr & 0x7F);
	return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
term_set_cursor_position(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, UINTN Col, UINTN Row)
{
	if (Col >= 80 || Row >= 25)
		return EFI_UNSUPPORTED;
	if ((INT32)Col == term_mode.CursorColumn && (INT32)Row == term_mode.CursorRow)
		return EFI_SUCCESS;
	char b[16];
	snprintf(b, sizeof b, "\033[%u;%uH", (unsigned)Row + 1, (unsigned)Col + 1);
	term_sends(b);
	term_mode.CursorColumn = (INT32)Col;
	term_mode.CursorRow = (INT32)Row;
	return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
term_enable_cursor(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, BOOLEAN Visible)
{
	if (Visible != term_mode.CursorVisible)
		term_sends(Visible ? "\033[?25h" : "\033[?25l");
	term_mode.CursorVisible = Visible;
	return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
term_reset(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, BOOLEAN Ext)
{
	term_mode.Attribute = EFI_TEXT_ATTR(EFI_LIGHTGRAY, EFI_BLACK);
	return EFI_SUCCESS;
}

/* input: a byte queue from the UART and the escape-sequence decoder */
static UINT8 rx[64];
static unsigned rx_n;
static uint64_t esc_since;

static void
term_fill(void)
{
	UINTN n = sizeof rx - rx_n;
	if (!n)
		return;
	sio->Read(sio, &n, rx + rx_n);
	if (n && !rx_n)
		esc_since = time_us();
	rx_n += (unsigned)n;
}

static void
rx_drop(unsigned k)
{
	memmove(rx, rx + k, rx_n - k);
	rx_n -= k;
	esc_since = time_us();
}

static const struct {
	const char *seq;
	UINT16 scan;
} esc_keys[] = {
	{ "[A", SCAN_UP }, { "[B", SCAN_DOWN }, { "[C", SCAN_RIGHT }, { "[D", SCAN_LEFT },
	{ "[H", SCAN_HOME }, { "[F", SCAN_END }, { "OH", SCAN_HOME }, { "OF", SCAN_END },
	{ "[1~", SCAN_HOME }, { "[4~", SCAN_END }, { "[2~", SCAN_INSERT }, { "[3~", SCAN_DELETE },
	{ "[5~", SCAN_PAGE_UP }, { "[6~", SCAN_PAGE_DOWN },
	{ "OP", SCAN_F1 }, { "OQ", SCAN_F2 }, { "OR", SCAN_F3 }, { "OS", SCAN_F4 },
	{ "[11~", SCAN_F1 }, { "[12~", SCAN_F2 }, { "[13~", SCAN_F3 }, { "[14~", SCAN_F4 },
	{ "[15~", SCAN_F5 }, { "[17~", SCAN_F6 }, { "[18~", SCAN_F7 }, { "[19~", SCAN_F8 },
	{ "[20~", SCAN_F9 }, { "[21~", SCAN_F10 }, { "[23~", SCAN_F11 }, { "[24~", SCAN_F12 },
	/* VT100+ (console redirection) */
	{ "1", SCAN_F1 }, { "2", SCAN_F2 }, { "3", SCAN_F3 }, { "4", SCAN_F4 }, { "5", SCAN_F5 },
	{ "6", SCAN_F6 }, { "7", SCAN_F7 }, { "8", SCAN_F8 }, { "9", SCAN_F9 }, { "0", SCAN_F10 },
	{ "!", SCAN_F11 }, { "@", SCAN_F12 },
};

/* the next key from the terminal: 1 if *k is set */
static int
term_key(EFI_INPUT_KEY *k)
{
	term_fill();
	if (!rx_n)
		return 0;
	if (rx[0] != 0x1B) {
		UINT8 c = rx[0];
		rx_drop(1);
		k->ScanCode = SCAN_NULL;
		k->UnicodeChar = c == 0x7F ? CHAR_BACKSPACE : c == '\n' ? CHAR_CARRIAGE_RETURN : c;
		return 1;
	}
	/* an escape sequence: match a known one, wait for more, or give ESC */
	int partial = 0;
	for (unsigned i = 0; i < ARRAY_SIZE(esc_keys); i++) {
		unsigned l = (unsigned)strlen(esc_keys[i].seq);
		unsigned have = rx_n - 1 < l ? rx_n - 1 : l;
		if (memcmp(rx + 1, esc_keys[i].seq, have))
			continue;
		if (have == l) {
			rx_drop(1 + l);
			k->ScanCode = esc_keys[i].scan;
			k->UnicodeChar = 0;
			return 1;
		}
		partial = 1;
	}
	if (partial && time_us() - esc_since < 30000)
		return 0;
	rx_drop(1);
	k->ScanCode = SCAN_ESC;
	k->UnicodeChar = 0;
	return 1;
}

/* ---------------------------------------------------------------- ConOut */

static EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *outs[MAX_OUT];
static unsigned nouts;
static SIMPLE_TEXT_OUTPUT_MODE con_mode;
static EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL con_out;

#define FOR_OUTS(call) do { EFI_STATUS r_ = EFI_SUCCESS; \
	for (unsigned i_ = 0; i_ < nouts; i_++) { EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *o = outs[i_]; \
		EFI_STATUS s_ = (call); if (EFI_ERROR(s_)) r_ = s_; } return r_; } while (0)

static void
sync_mode(void)
{
	if (nouts) {
		con_mode.CursorColumn = outs[0]->Mode->CursorColumn;
		con_mode.CursorRow = outs[0]->Mode->CursorRow;
	}
}

static EFI_STATUS EFIAPI
con_output_string(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, CHAR16 *s)
{
	EFI_STATUS r = EFI_SUCCESS;
	for (unsigned i = 0; i < nouts; i++) {
		EFI_STATUS st = outs[i]->OutputString(outs[i], s);
		if (st != EFI_SUCCESS)
			r = st;
	}
	sync_mode();
	return r;
}

static EFI_STATUS EFIAPI
con_test_string(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, CHAR16 *s)
{
	FOR_OUTS(o->TestString(o, s));
}

static EFI_STATUS EFIAPI
con_query_mode(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, UINTN Mode, UINTN *Cols, UINTN *Rows)
{
	if (Mode)
		return EFI_UNSUPPORTED;
	*Cols = 80;
	*Rows = 25;
	return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
con_set_mode(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, UINTN Mode)
{
	if (Mode)
		return EFI_UNSUPPORTED;
	con_mode.CursorColumn = con_mode.CursorRow = 0;
	FOR_OUTS(o->SetMode(o, 0));
}

static EFI_STATUS EFIAPI
con_set_attribute(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, UINTN Attr)
{
	con_mode.Attribute = (INT32)Attr;
	FOR_OUTS(o->SetAttribute(o, Attr));
}

static EFI_STATUS EFIAPI
con_clear_screen(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This)
{
	con_mode.CursorColumn = con_mode.CursorRow = 0;
	FOR_OUTS(o->ClearScreen(o));
}

static EFI_STATUS EFIAPI
con_set_cursor_position(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, UINTN Col, UINTN Row)
{
	if (Col >= 80 || Row >= 25)
		return EFI_UNSUPPORTED;
	con_mode.CursorColumn = (INT32)Col;
	con_mode.CursorRow = (INT32)Row;
	FOR_OUTS(o->SetCursorPosition(o, Col, Row));
}

static EFI_STATUS EFIAPI
con_enable_cursor(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, BOOLEAN Visible)
{
	con_mode.CursorVisible = Visible;
	FOR_OUTS(o->EnableCursor(o, Visible));
}

static EFI_STATUS EFIAPI
con_reset(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, BOOLEAN Ext)
{
	con_mode.Attribute = EFI_TEXT_ATTR(EFI_LIGHTGRAY, EFI_BLACK);
	con_mode.CursorColumn = con_mode.CursorRow = 0;
	FOR_OUTS(o->Reset(o, Ext));
}

/* ---------------------------------------------------------------- ConIn */

static EFI_SIMPLE_TEXT_INPUT_PROTOCOL *ins[MAX_IN];
static unsigned nins;
static EFI_SIMPLE_TEXT_INPUT_PROTOCOL con_in;
static EFI_INPUT_KEY pending;
static int have_pending;
static void *in_reg;

static int
poll_key(EFI_INPUT_KEY *k)
{
	if (have_pending) {
		*k = pending;
		return 1;
	}
	if (sio && term_key(k))
		return 1;
	for (unsigned i = 0; i < nins; i++)
		if (ins[i]->ReadKeyStroke(ins[i], k) == EFI_SUCCESS)
			return 1;
	return 0;
}

static EFI_STATUS EFIAPI
con_read_key(EFI_SIMPLE_TEXT_INPUT_PROTOCOL *This, EFI_INPUT_KEY *Key)
{
	if (!Key)
		return EFI_INVALID_PARAMETER;
	if (!poll_key(Key))
		return EFI_NOT_READY;
	have_pending = 0;
	return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
con_in_reset(EFI_SIMPLE_TEXT_INPUT_PROTOCOL *This, BOOLEAN Ext)
{
	EFI_INPUT_KEY k;
	have_pending = 0;
	rx_n = 0;
	for (unsigned i = 0; i < nins; i++)
		ins[i]->Reset(ins[i], Ext);
	while (poll_key(&k))
		have_pending = 0;
	return EFI_SUCCESS;
}

static VOID EFIAPI
wait_for_key(EFI_EVENT ev, VOID *ctx)
{
	if (!have_pending && poll_key(&pending))
		have_pending = 1;
	if (have_pending)
		gBS->SignalEvent(ev);
}

static VOID EFIAPI
on_new_input(EFI_EVENT ev, VOID *ctx)
{
	EFI_HANDLE h;
	UINTN sz = sizeof h;
	while (gBS->LocateHandle(ByRegisterNotify, 0, in_reg, &sz, &h) == EFI_SUCCESS) {
		EFI_SIMPLE_TEXT_INPUT_PROTOCOL *in;
		if (h == ctx)
			continue;	/* our own splitter */
		if (gBS->HandleProtocol(h, &gEfiSimpleTextInProtocolGuid, (void **)&in) == EFI_SUCCESS &&
		    nins < MAX_IN && in != &con_in) {
			ins[nins++] = in;
			DEBUG("ConsoleDxe: input device added\n");
		}
		sz = sizeof h;
	}
}

EFI_STATUS EFIAPI
efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st)
{
	uefi_init(image, st);

	/* the outputs present now: the display */
	EFI_HANDLE *hs;
	UINTN n;
	if (gBS->LocateHandleBuffer(ByProtocol, &gEfiSimpleTextOutProtocolGuid, 0, &n, &hs) == EFI_SUCCESS) {
		for (UINTN i = 0; i < n && nouts < MAX_OUT - 1; i++)
			gBS->HandleProtocol(hs[i], &gEfiSimpleTextOutProtocolGuid, (void **)&outs[nouts++]);
		free(hs);
	}
	/* the terminal on the serial port */
	if (gBS->LocateProtocol(&gEfiSerialIoProtocolGuid, 0, (void **)&sio) == EFI_SUCCESS) {
		term_mode = (SIMPLE_TEXT_OUTPUT_MODE){ 1, 0, EFI_TEXT_ATTR(EFI_LIGHTGRAY, EFI_BLACK), 0, 0, TRUE };
		term_out = (EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL){ term_reset, term_output_string, term_test_string,
		    term_query_mode, term_set_mode, term_set_attribute, term_clear_screen,
		    term_set_cursor_position, term_enable_cursor, &term_mode };
		outs[nouts++] = &term_out;
	}

	con_mode = (SIMPLE_TEXT_OUTPUT_MODE){ 1, 0, EFI_TEXT_ATTR(EFI_LIGHTGRAY, EFI_BLACK), 0, 0, TRUE };
	con_out = (EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL){ con_reset, con_output_string, con_test_string,
	    con_query_mode, con_set_mode, con_set_attribute, con_clear_screen, con_set_cursor_position,
	    con_enable_cursor, &con_mode };
	con_in.Reset = con_in_reset;
	con_in.ReadKeyStroke = con_read_key;
	gBS->CreateEvent(EVT_NOTIFY_WAIT, TPL_NOTIFY, wait_for_key, 0, &con_in.WaitForKey);

	EFI_HANDLE h = 0;
	EFI_STATUS s = gBS->InstallMultipleProtocolInterfaces(&h, &gEfiSimpleTextOutProtocolGuid, &con_out,
	    &gEfiSimpleTextInProtocolGuid, &con_in, (void *)0);
	if (EFI_ERROR(s))
		return s;
	/* keyboards that come later */
	EFI_EVENT ev;
	gBS->CreateEvent(EVT_NOTIFY_SIGNAL, TPL_CALLBACK, on_new_input, h, &ev);
	gBS->RegisterProtocolNotify(&gEfiSimpleTextInProtocolGuid, ev, &in_reg);
	on_new_input(ev, h);

	gST->ConsoleOutHandle = h;
	gST->ConOut = &con_out;
	gST->StandardErrorHandle = h;
	gST->StdErr = &con_out;
	gST->ConsoleInHandle = h;
	gST->ConIn = &con_in;
	gST->Hdr.CRC32 = 0;
	gBS->CalculateCrc32(gST, gST->Hdr.HeaderSize, &gST->Hdr.CRC32);
	DEBUG("ConsoleDxe: %u output(s), terminal %s\n", nouts, sio ? "on COM1" : "none");
	return EFI_SUCCESS;
}
