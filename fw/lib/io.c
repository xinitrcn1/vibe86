/*
 * fwlib: the debug console, COM1, fw_cfg and time.
 */
#include "fwlib.h"
#include "x86.h"

/* ---- the debug console: QEMU's "isa-debugcon" protocol on port 402h,
   which reads back E9h when present (vsim -g file captures it) */

void
dbg_putc(int c)
{
	if (c == '\n')
		outb(DEBUGCON, '\r');
	outb(DEBUGCON, (uint8_t)c);
}

static void
dbg_out(void *ctx, int c)
{
	(void)ctx;
	dbg_putc(c < 0x80 ? c : '?');
}

void
dbg_vprintf(const char *fmt, va_list ap)
{
	fmt_format(dbg_out, 0, fmt, ap);
}

void
dbg_printf(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	dbg_vprintf(fmt, ap);
	va_end(ap);
}

/* ---- COM1: 16550 registers (PC16550D datasheet, Table II) */
#define UART_THR	0
#define UART_RBR	0
#define UART_IER	1
#define UART_FCR	2
#define UART_LCR	3
#define UART_MCR	4
#define UART_LSR	5
#define UART_DLL	0
#define UART_DLM	1
#define LSR_DR		0x01
#define LSR_THRE	0x20

void
com1_init(void)
{
	outb(COM1 + UART_IER, 0);
	outb(COM1 + UART_LCR, 0x80);		/* DLAB */
	outb(COM1 + UART_DLL, 1);		/* 115200 baud (1.8432 MHz / 16 / 1) */
	outb(COM1 + UART_DLM, 0);
	outb(COM1 + UART_LCR, 0x03);		/* 8N1 */
	outb(COM1 + UART_FCR, 0x07);		/* FIFOs on, cleared */
	outb(COM1 + UART_MCR, 0x03);		/* DTR, RTS */
}

void
com1_putc(int c)
{
	for (int i = 0; i < 100000 && !(inb(COM1 + UART_LSR) & LSR_THRE); i++)
		;
	outb(COM1 + UART_THR, (uint8_t)c);
}

int
com1_getc(void)
{
	if (inb(COM1 + UART_LSR) & LSR_DR)
		return inb(COM1 + UART_RBR);
	return -1;
}

void
com1_puts(const char *s)
{
	for (; *s; s++) {
		if (*s == '\n')
			com1_putc('\r');
		com1_putc(*s);
	}
}

/* ---- fw_cfg (QEMU docs/specs/fw_cfg.rst): selector 510h (16-bit), data
   511h, DMA address 514h (big-endian, written high half then low half) */
#define FW_CFG_SIGNATURE	0x0000
#define FW_CFG_ID		0x0001
#define FW_CFG_FILE_DIR		0x0019
#define FW_CFG_DMA_CTL_ERROR	0x01
#define FW_CFG_DMA_CTL_READ	0x02
#define FW_CFG_DMA_CTL_SELECT	0x08

static uint32_t
bswap32(uint32_t v)
{
	return __builtin_bswap32(v);
}

int
fwcfg_present(void)
{
	char sig[4];
	outw(FWCFG_PORT, FW_CFG_SIGNATURE);
	for (int i = 0; i < 4; i++)
		sig[i] = (char)inb(FWCFG_PORT + 1);
	return sig[0] == 'Q' && sig[1] == 'E' && sig[2] == 'M' && sig[3] == 'U';
}

static int
fwcfg_has_dma(void)
{
	uint32_t id = 0;
	outw(FWCFG_PORT, FW_CFG_ID);
	for (int i = 0; i < 4; i++)
		id |= (uint32_t)inb(FWCFG_PORT + 1) << (8 * i);
	return (id & 2) != 0;
}

void
fwcfg_read(uint16_t select, void *buf, uint32_t size)
{
	if (fwcfg_has_dma() && (uintptr_t)buf < (1ull << 32)) {
		/* FWCfgDmaAccess, big-endian, 8-byte aligned (on the stack) */
		volatile struct {
			uint32_t control, length;
			uint64_t address;
		} __attribute__((aligned(8))) acc;
		acc.control = bswap32((uint32_t)select << 16 | FW_CFG_DMA_CTL_SELECT | FW_CFG_DMA_CTL_READ);
		acc.length = bswap32(size);
		acc.address = __builtin_bswap64((uint64_t)(uintptr_t)buf);
		uint64_t a = (uint64_t)(uintptr_t)&acc;
		outl(FWCFG_DMA_PORT, bswap32((uint32_t)(a >> 32)));
		outl(FWCFG_DMA_PORT + 4, bswap32((uint32_t)a));
		while (bswap32(acc.control) & ~FW_CFG_DMA_CTL_ERROR)
			;
		return;
	}
	outw(FWCFG_PORT, select);
	uint8_t *p = buf;
	for (uint32_t i = 0; i < size; i++)
		p[i] = inb(FWCFG_PORT + 1);
}

int
fwcfg_dir_entry(uint32_t n, char name[56], uint32_t *size, uint16_t *select)
{
	uint8_t b[64];
	outw(FWCFG_PORT, FW_CFG_FILE_DIR);
	uint32_t count = 0;
	for (int i = 0; i < 4; i++)
		count = count << 8 | inb(FWCFG_PORT + 1);
	if (n >= count)
		return -1;
	for (uint32_t k = 0; k <= n; k++)
		for (int i = 0; i < 64; i++)
			b[i] = inb(FWCFG_PORT + 1);
	*size = (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
	*select = (uint16_t)(b[4] << 8 | b[5]);
	memcpy(name, b + 8, 56);
	name[55] = 0;
	return 0;
}

int
fwcfg_find(const char *name, uint32_t *size, uint16_t *select)
{
	char n[56];
	if (!fwcfg_present())
		return -1;
	for (uint32_t i = 0; fwcfg_dir_entry(i, n, size, select) == 0; i++)
		if (!strcmp(n, name))
			return 0;
	return -1;
}

/* ---- time: the invariant TSC counts at platform.toml clock.tsc_hz */

uint64_t
time_us(void)
{
	return rdtsc() / (TSC_HZ / 1000000);
}

void
udelay(uint64_t us)
{
	uint64_t end = rdtsc() + us * (TSC_HZ / 1000000);
	while (rdtsc() < end)
		cpu_pause();
}
