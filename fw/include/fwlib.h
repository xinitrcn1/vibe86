/*
 * fwlib.h: the firmware's freestanding library, linked into every image
 * (PEI, the DXE core, the drivers, the applications). Nothing here keeps
 * state, so PEI, which runs from flash with no writable data, can use it.
 */
#ifndef FWLIB_H
#define FWLIB_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include "efi.h"

#define ARRAY_SIZE(a)	(sizeof(a) / sizeof((a)[0]))
#define ALIGN_UP(x, a)	(((x) + (a) - 1) & ~((uint64_t)(a) - 1))
#define ALIGN_DOWN(x, a) ((x) & ~((uint64_t)(a) - 1))
#define MIN(a, b)	((a) < (b) ? (a) : (b))
#define MAX(a, b)	((a) > (b) ? (a) : (b))
#define CONTAINER_OF(p, type, member) ((type *)((char *)(p) - offsetof(type, member)))

/* string.c */
void *memcpy(void *d, const void *s, size_t n);
void *memmove(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
char *strncpy0(char *d, const char *s, size_t n);	/* always terminated */
size_t wcslen(const CHAR16 *s);
int wcscmp(const CHAR16 *a, const CHAR16 *b);
int wcsicmp(const CHAR16 *a, const CHAR16 *b);
int wcsnicmp(const CHAR16 *a, const CHAR16 *b, size_t n);
CHAR16 *wcsncpy0(CHAR16 *d, const CHAR16 *s, size_t n);
void wcs_from_ascii(CHAR16 *d, const char *s, size_t n);
void ascii_from_wcs(char *d, const CHAR16 *s, size_t n);
int to_upper(int c);
int guid_eq(const EFI_GUID *a, const EFI_GUID *b);
uint32_t crc32(const void *data, size_t n);
uint64_t str_to_u64(const char *s, const char **end, int base);
uint64_t wcs_to_u64(const CHAR16 *s, const CHAR16 **end, int base);

/* print.c: %d %i %u %x %X %p %c %s (char *) %ls (CHAR16 *) %r (EFI_STATUS)
 * %g (EFI_GUID *); flags - 0, width, .precision, length l ll z */
typedef void (*fmt_out_fn)(void *ctx, int c);
void fmt_format(fmt_out_fn out, void *ctx, const char *fmt, va_list ap);
int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap);
int snprintf(char *buf, size_t n, const char *fmt, ...);
int vswprintf(CHAR16 *buf, size_t n, const char *fmt, va_list ap);	/* fmt is ASCII */
int swprintf(CHAR16 *buf, size_t n, const char *fmt, ...);
const char *efi_status_str(EFI_STATUS s);

/* debug.c: the debug log on the debug console (port 402h, vsim -g) */
void dbg_putc(int c);
void dbg_printf(const char *fmt, ...);
void dbg_vprintf(const char *fmt, va_list ap);
#define DEBUG(...)	dbg_printf(__VA_ARGS__)

/* serial.c: COM1, a 16550 (National Semiconductor PC16550D datasheet) */
void com1_init(void);
void com1_putc(int c);
int com1_getc(void);		/* -1 if nothing */
void com1_puts(const char *s);

/* fwcfg.c: QEMU's firmware configuration interface (QEMU docs/specs/fw_cfg.rst) */
int fwcfg_present(void);
/* find a file by name: its size and selector; 0 if found, -1 if not */
int fwcfg_find(const char *name, uint32_t *size, uint16_t *select);
/* read size bytes of an item from offset 0 (DMA when the interface has it) */
void fwcfg_read(uint16_t select, void *buf, uint32_t size);
/* the n-th directory entry (name 56 bytes): 0, or -1 past the end */
int fwcfg_dir_entry(uint32_t n, char name[56], uint32_t *size, uint16_t *select);

/* time.c: the invariant TSC (platform.toml clock.tsc_hz) */
uint64_t time_us(void);
void udelay(uint64_t us);

/* devpath.c: device path nodes ([UEFI] 10.3) */
UINTN dp_node_len(const EFI_DEVICE_PATH_PROTOCOL *n);
int dp_is_end(const EFI_DEVICE_PATH_PROTOCOL *n);
EFI_DEVICE_PATH_PROTOCOL *dp_next(const EFI_DEVICE_PATH_PROTOCOL *n);
UINTN dp_size(const EFI_DEVICE_PATH_PROTOCOL *p);	/* including the end node */
void dp_set_end(EFI_DEVICE_PATH_PROTOCOL *n);
void dp_set_node(EFI_DEVICE_PATH_PROTOCOL *n, UINT8 type, UINT8 sub, UINTN len);
/* text form, e.g. PciRoot(0x0)/Pci(0x0,0x10)/USB(0x2,0x0): into buf (ASCII) */
void dp_to_text(const EFI_DEVICE_PATH_PROTOCOL *p, char *buf, size_t n);

#endif
