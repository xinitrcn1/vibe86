/* BdsDxe: drawing on the console (ConOut) and reading keys (ConIn) */
#include "bds.h"

void
ui_attr(UINTN a)
{
	gST->ConOut->SetAttribute(gST->ConOut, a);
}

void
ui_at(unsigned row, unsigned col)
{
	gST->ConOut->SetCursorPosition(gST->ConOut, col, row);
}

void
ui_puts(const char *s)
{
	CHAR16 b[96];
	while (*s) {
		unsigned n = 0;
		while (*s && n < ARRAY_SIZE(b) - 1) {
			if (*s == '\n' && n < ARRAY_SIZE(b) - 2)
				b[n++] = '\r';
			unsigned char c = (unsigned char)*s++;
			/* control codes 18h-1Bh draw as arrows (as in code page 437) */
			b[n++] = c == 0x18 ? ARROW_UP : c == 0x19 ? ARROW_DOWN : c == 0x1A ? 0x2192 :
			    c == 0x1B ? 0x2190 : c;
		}
		b[n] = 0;
		gST->ConOut->OutputString(gST->ConOut, b);
	}
}

void
ui_printf(const char *fmt, ...)
{
	char b[256];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(b, sizeof b, fmt, ap);
	va_end(ap);
	ui_puts(b);
}

void
ui_text(unsigned row, unsigned col, UINTN attr, const char *fmt, ...)
{
	char b[96];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(b, sizeof b, fmt, ap);
	va_end(ap);
	ui_attr(attr);
	ui_at(row, col);
	ui_puts(b);
}

void
ui_fill(unsigned row, unsigned col, unsigned n, CHAR16 ch, UINTN attr)
{
	CHAR16 b[81];
	if (col >= 80)
		return;
	if (col + n > 80)
		n = 80 - col;
	/* the bottom-right cell would scroll the screen: leave it out */
	if (row == 24 && col + n == 80)
		n--;
	for (unsigned i = 0; i < n; i++)
		b[i] = ch;
	b[n] = 0;
	ui_attr(attr);
	ui_at(row, col);
	gST->ConOut->OutputString(gST->ConOut, b);
}

void
ui_box(unsigned top, unsigned left, unsigned bottom, unsigned right, UINTN attr, int dbl)
{
	CHAR16 h = dbl ? BOXDRAW_DOUBLE_HORIZONTAL : BOXDRAW_HORIZONTAL;
	CHAR16 v = dbl ? BOXDRAW_DOUBLE_VERTICAL : BOXDRAW_VERTICAL;
	CHAR16 c[2] = { 0, 0 };
	ui_fill(top, left + 1, right - left - 1, h, attr);
	ui_fill(bottom, left + 1, right - left - 1, h, attr);
	for (unsigned r = top + 1; r < bottom; r++) {
		ui_fill(r, left, 1, v, attr);
		ui_fill(r, left + 1, right - left - 1, ' ', attr);
		ui_fill(r, right, 1, v, attr);
	}
	ui_at(top, left);
	c[0] = dbl ? BOXDRAW_DOUBLE_DOWN_RIGHT : BOXDRAW_DOWN_RIGHT;
	gST->ConOut->OutputString(gST->ConOut, c);
	ui_at(top, right);
	c[0] = dbl ? BOXDRAW_DOUBLE_DOWN_LEFT : BOXDRAW_DOWN_LEFT;
	gST->ConOut->OutputString(gST->ConOut, c);
	ui_at(bottom, left);
	c[0] = dbl ? BOXDRAW_DOUBLE_UP_RIGHT : BOXDRAW_UP_RIGHT;
	gST->ConOut->OutputString(gST->ConOut, c);
	if (!(bottom == 24 && right == 79)) {
		ui_at(bottom, right);
		c[0] = dbl ? BOXDRAW_DOUBLE_UP_LEFT : BOXDRAW_UP_LEFT;
		gST->ConOut->OutputString(gST->ConOut, c);
	}
}

void
ui_clear(UINTN attr)
{
	ui_attr(attr);
	gST->ConOut->ClearScreen(gST->ConOut);
}

void
ui_cursor(int on)
{
	gST->ConOut->EnableCursor(gST->ConOut, (BOOLEAN)on);
}

int
ui_key(EFI_INPUT_KEY *k, uint64_t timeout_us)
{
	uint64_t end = timeout_us ? time_us() + timeout_us : 0;
	for (;;) {
		if (gBS->CheckEvent(gST->ConIn->WaitForKey) == EFI_SUCCESS &&
		    gST->ConIn->ReadKeyStroke(gST->ConIn, k) == EFI_SUCCESS)
			return 1;
		if (end && time_us() >= end)
			return 0;
	}
}

void
ui_flush_keys(void)
{
	EFI_INPUT_KEY k;
	while (ui_key(&k, 1))
		;
}
