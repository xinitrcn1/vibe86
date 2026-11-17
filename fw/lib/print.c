/* fwlib: a small printf (see fwlib.h for the conversions) */
#include "fwlib.h"

static const char *const status_names[] = {
	"Success", "Load Error", "Invalid Parameter", "Unsupported", "Bad Buffer Size",
	"Buffer Too Small", "Not Ready", "Device Error", "Write Protected",
	"Out of Resources", "Volume Corrupt", "Volume Full", "No Media", "Media changed",
	"Not Found", "Access Denied", "No Response", "No mapping", "Time out",
	"Not started", "Already started", "Aborted", "ICMP Error", "TFTP Error",
	"Protocol Error", "Incompatible Version", "Security Violation", "CRC Error",
	"End of Media", "", "", "End of File", "Invalid Language", "Compromised Data",
};

const char *
efi_status_str(EFI_STATUS s)
{
	UINTN code = s & ~EFI_ERR;
	if (!EFI_ERROR(s))
		return s == 0 ? "Success" : "Warning";
	if (code < ARRAY_SIZE(status_names) && status_names[code][0])
		return status_names[code];
	return "Error";
}

static void
out_str(fmt_out_fn out, void *ctx, const char *s, int wide, int width, int prec, int left)
{
	int len = 0;
	if (!s)
		s = "(null)", wide = 0;
	if (wide) {
		const CHAR16 *w = (const CHAR16 *)s;
		while (w[len] && (prec < 0 || len < prec))
			len++;
	} else
		while (s[len] && (prec < 0 || len < prec))
			len++;
	if (!left)
		for (int i = len; i < width; i++)
			out(ctx, ' ');
	for (int i = 0; i < len; i++)
		out(ctx, wide ? ((const CHAR16 *)s)[i] : (unsigned char)s[i]);
	if (left)
		for (int i = len; i < width; i++)
			out(ctx, ' ');
}

static void
out_num(fmt_out_fn out, void *ctx, uint64_t v, int neg, int base, int upper, int width, int zero,
    int left)
{
	char buf[24];
	int n = 0;
	const char *dig = upper ? "0123456789ABCDEF" : "0123456789abcdef";
	do {
		buf[n++] = dig[v % (unsigned)base];
		v /= (unsigned)base;
	} while (v);
	int len = n + neg;
	if (!left && !zero)
		for (int i = len; i < width; i++)
			out(ctx, ' ');
	if (neg)
		out(ctx, '-');
	if (!left && zero)
		for (int i = len; i < width; i++)
			out(ctx, '0');
	while (n)
		out(ctx, buf[--n]);
	if (left)
		for (int i = len; i < width; i++)
			out(ctx, ' ');
}

void
fmt_format(fmt_out_fn out, void *ctx, const char *f, va_list ap)
{
	for (; *f; f++) {
		if (*f != '%') {
			out(ctx, (unsigned char)*f);
			continue;
		}
		f++;
		int left = 0, zero = 0, width = 0, prec = -1, lng = 0;
		for (;; f++) {
			if (*f == '-')
				left = 1;
			else if (*f == '0')
				zero = 1;
			else
				break;
		}
		if (*f == '*') {
			width = va_arg(ap, int);
			f++;
		} else
			while (*f >= '0' && *f <= '9')
				width = width * 10 + (*f++ - '0');
		if (*f == '.') {
			f++;
			prec = 0;
			if (*f == '*') {
				prec = va_arg(ap, int);
				f++;
			} else
				while (*f >= '0' && *f <= '9')
					prec = prec * 10 + (*f++ - '0');
		}
		while (*f == 'l' || *f == 'z' || *f == 'h') {
			if (*f != 'h')
				lng++;
			f++;
		}
		switch (*f) {
		case 'd':
		case 'i': {
			int64_t v = lng ? va_arg(ap, int64_t) : va_arg(ap, int);
			out_num(out, ctx, v < 0 ? (uint64_t)-v : (uint64_t)v, v < 0, 10, 0, width, zero, left);
			break;
		}
		case 'u':
		case 'x':
		case 'X': {
			uint64_t v = lng ? va_arg(ap, uint64_t) : va_arg(ap, unsigned);
			out_num(out, ctx, v, 0, *f == 'u' ? 10 : 16, *f == 'X', width, zero, left);
			break;
		}
		case 'p':
			out(ctx, '0');
			out(ctx, 'x');
			out_num(out, ctx, (uint64_t)(uintptr_t)va_arg(ap, void *), 0, 16, 0, width, zero, left);
			break;
		case 'c':
			out(ctx, va_arg(ap, int));
			break;
		case 's':
			out_str(out, ctx, va_arg(ap, const char *), lng > 0, width, prec, left);
			break;
		case 'r':
			out_str(out, ctx, efi_status_str(va_arg(ap, EFI_STATUS)), 0, width, prec, left);
			break;
		case 'g': {
			const EFI_GUID *g = va_arg(ap, const EFI_GUID *);
			out_num(out, ctx, g->Data1, 0, 16, 1, 8, 1, 0);
			out(ctx, '-');
			out_num(out, ctx, g->Data2, 0, 16, 1, 4, 1, 0);
			out(ctx, '-');
			out_num(out, ctx, g->Data3, 0, 16, 1, 4, 1, 0);
			for (int i = 0; i < 8; i++) {
				if (i == 0 || i == 2)
					out(ctx, '-');
				out_num(out, ctx, g->Data4[i], 0, 16, 1, 2, 1, 0);
			}
			break;
		}
		case '%':
			out(ctx, '%');
			break;
		case 0:
			return;
		default:
			out(ctx, '%');
			out(ctx, *f);
		}
	}
}

struct sbuf {
	void *buf;
	size_t n, len;
	int wide;
};

static void
sbuf_out(void *ctx, int c)
{
	struct sbuf *b = ctx;
	if (b->len + 1 < b->n) {
		if (b->wide)
			((CHAR16 *)b->buf)[b->len] = (CHAR16)c;
		else
			((char *)b->buf)[b->len] = (char)c;
	}
	b->len++;
}

int
vsnprintf(char *buf, size_t n, const char *fmt, va_list ap)
{
	struct sbuf b = { buf, n, 0, 0 };
	fmt_format(sbuf_out, &b, fmt, ap);
	if (n)
		buf[b.len < n ? b.len : n - 1] = 0;
	return (int)b.len;
}

int
snprintf(char *buf, size_t n, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int r = vsnprintf(buf, n, fmt, ap);
	va_end(ap);
	return r;
}

int
vswprintf(CHAR16 *buf, size_t n, const char *fmt, va_list ap)
{
	struct sbuf b = { buf, n, 0, 1 };
	fmt_format(sbuf_out, &b, fmt, ap);
	if (n)
		buf[b.len < n ? b.len : n - 1] = 0;
	return (int)b.len;
}

int
swprintf(CHAR16 *buf, size_t n, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int r = vswprintf(buf, n, fmt, ap);
	va_end(ap);
	return r;
}
