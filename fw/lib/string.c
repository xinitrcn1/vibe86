/* fwlib: memory and string functions (the compiler also calls mem*) */
#include "fwlib.h"

/*
 * REP MOVS/STOS with the widest element: vibe86 v1 commits each REP
 * iteration separately (D-005), so 8-byte elements are ~8x faster.
 */
void *
memcpy(void *d, const void *s, size_t n)
{
	void *r = d;
	size_t q = n >> 3;
	__asm__ volatile("rep movsq" : "+D"(d), "+S"(s), "+c"(q) :: "memory");
	n &= 7;
	__asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(n) :: "memory");
	return r;
}

void *
memmove(void *d, const void *s, size_t n)
{
	unsigned char *dp = d;
	const unsigned char *sp = s;
	if (dp <= sp || dp >= sp + n)
		return memcpy(d, s, n);
	/* overlapping, destination above: copy backwards */
	dp += n - 1;
	sp += n - 1;
	__asm__ volatile("std; rep movsb; cld" : "+D"(dp), "+S"(sp), "+c"(n) :: "memory");
	return d;
}

void *
memset(void *d, int c, size_t n)
{
	void *r = d;
	uint64_t v = (uint8_t)c * 0x0101010101010101ull;
	size_t q = n >> 3;
	__asm__ volatile("rep stosq" : "+D"(d), "+c"(q) : "a"(v) : "memory");
	n &= 7;
	__asm__ volatile("rep stosb" : "+D"(d), "+c"(n) : "a"(v) : "memory");
	return r;
}

int
memcmp(const void *a, const void *b, size_t n)
{
	const unsigned char *x = a, *y = b;
	for (; n; n--, x++, y++)
		if (*x != *y)
			return *x - *y;
	return 0;
}

size_t
strlen(const char *s)
{
	size_t n = 0;
	while (s[n])
		n++;
	return n;
}

int
strcmp(const char *a, const char *b)
{
	while (*a && *a == *b)
		a++, b++;
	return (unsigned char)*a - (unsigned char)*b;
}

int
strncmp(const char *a, const char *b, size_t n)
{
	for (; n; n--, a++, b++) {
		if (*a != *b)
			return (unsigned char)*a - (unsigned char)*b;
		if (!*a)
			return 0;
	}
	return 0;
}

char *
strncpy0(char *d, const char *s, size_t n)
{
	size_t i = 0;
	if (!n)
		return d;
	for (; i + 1 < n && s[i]; i++)
		d[i] = s[i];
	d[i] = 0;
	return d;
}

size_t
wcslen(const CHAR16 *s)
{
	size_t n = 0;
	while (s[n])
		n++;
	return n;
}

int
wcscmp(const CHAR16 *a, const CHAR16 *b)
{
	while (*a && *a == *b)
		a++, b++;
	return (int)*a - (int)*b;
}

int
to_upper(int c)
{
	return c >= 'a' && c <= 'z' ? c - 32 : c;
}

int
wcsnicmp(const CHAR16 *a, const CHAR16 *b, size_t n)
{
	for (; n; n--, a++, b++) {
		int x = to_upper(*a), y = to_upper(*b);
		if (x != y)
			return x - y;
		if (!x)
			return 0;
	}
	return 0;
}

int
wcsicmp(const CHAR16 *a, const CHAR16 *b)
{
	return wcsnicmp(a, b, (size_t)-1);
}

CHAR16 *
wcsncpy0(CHAR16 *d, const CHAR16 *s, size_t n)
{
	size_t i = 0;
	if (!n)
		return d;
	for (; i + 1 < n && s[i]; i++)
		d[i] = s[i];
	d[i] = 0;
	return d;
}

void
wcs_from_ascii(CHAR16 *d, const char *s, size_t n)
{
	size_t i = 0;
	if (!n)
		return;
	for (; i + 1 < n && s[i]; i++)
		d[i] = (unsigned char)s[i];
	d[i] = 0;
}

void
ascii_from_wcs(char *d, const CHAR16 *s, size_t n)
{
	size_t i = 0;
	if (!n)
		return;
	for (; i + 1 < n && s[i]; i++)
		d[i] = s[i] < 0x80 ? (char)s[i] : '?';
	d[i] = 0;
}

int
guid_eq(const EFI_GUID *a, const EFI_GUID *b)
{
	const uint64_t *x = (const uint64_t *)a, *y = (const uint64_t *)b;
	return x[0] == y[0] && x[1] == y[1];
}

/* CRC-32 (ISO 3309 / ITU-T V.42, reflected polynomial EDB88320h), as UEFI's
   CalculateCrc32 and the table headers use it; bitwise: no table in flash */
uint32_t
crc32(const void *data, size_t n)
{
	const uint8_t *p = data;
	uint32_t c = 0xFFFFFFFFu;
	while (n--) {
		c ^= *p++;
		for (int k = 0; k < 8; k++)
			c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
	}
	return ~c;
}

static int
digit(int c, int base)
{
	int v = c >= '0' && c <= '9' ? c - '0' :
	    c >= 'a' && c <= 'z' ? c - 'a' + 10 :
	    c >= 'A' && c <= 'Z' ? c - 'A' + 10 : 99;
	return v < base ? v : -1;
}

uint64_t
str_to_u64(const char *s, const char **end, int base)
{
	uint64_t v = 0;
	if (base == 0) {
		base = 10;
		if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
			base = 16, s += 2;
	} else if (base == 16 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
		s += 2;
	for (int d; (d = digit(*s, base)) >= 0; s++)
		v = v * (unsigned)base + (unsigned)d;
	if (end)
		*end = s;
	return v;
}

uint64_t
wcs_to_u64(const CHAR16 *s, const CHAR16 **end, int base)
{
	uint64_t v = 0;
	if (base == 0) {
		base = 10;
		if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
			base = 16, s += 2;
	} else if (base == 16 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
		s += 2;
	for (int d; *s < 128 && (d = digit(*s, base)) >= 0; s++)
		v = v * (unsigned)base + (unsigned)d;
	if (end)
		*end = s;
	return v;
}
