/* vsim board model: EEPROM, 16550 UART (COM1), system controller, board
 * straps, I/O ports, the MSI window; the devices are in their own files */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board.h"

static void mce_cb(void *ctx, int type, uint64_t pa);
static int dma_mem(void *ctx, int we, uint64_t pa, unsigned n, uint8_t *data);
static int dma_msi(void *ctx, uint64_t pa, uint64_t v);

int
board_init(struct board *b, int kind, const char *path, const struct dimm_cfg *dimms)
{
	FILE *f;
	size_t n;
	struct dimm_cfg def[DRAM_SLOTS];

	memset(b, 0, sizeof *b);
	memset(b->nvram, 0xFF, NVRAM_SIZE);	/* erased */
	b->kind = kind;
	b->eeprom = malloc(EEPROM_SIZE);
	if (!b->eeprom)
		return -1;
	memset(b->eeprom, 0xFF, EEPROM_SIZE);
	if (kind == BOARD_TEST && !(b->sram = calloc(1, SRAM_SIZE)))
		return -1;
	if (vga_init(&b->vga))
		return -1;
	b->pmc.msi = board_msi;
	b->pmc.msictx = b;
	pmc_init(&b->pmc);
	i2c_init(&b->i2c, &b->pmc);
	if (kind == BOARD_SERVER) {
		if (!dimms) {
			dram_default_cfg(def);
			dimms = def;
		}
		if (!(b->dram = dram_new(dimms, 0x5EED0F0DDEADBEEFull)))
			return -1;
		dram_set_mce(b->dram, mce_cb, b);
	}
	/* the image's last byte is the EEPROM's, at 0xFFFF_FFFF (the reset
	   vector is its last 16 bytes, as with PC flash images) */
	if (path && *path) {
		uint8_t *img = malloc(EEPROM_SIZE);
		if (!img)
			return -1;
		if (!(f = fopen(path, "rb"))) {
			fprintf(stderr, "vsim: %s: %s\n", path, strerror(errno));
			free(img);
			return -1;
		}
		n = fread(img, 1, EEPROM_SIZE, f);
		if (fgetc(f) != EOF) {
			fprintf(stderr, "vsim: %s: larger than the %llu MiB EEPROM\n", path,
			    (unsigned long long)(EEPROM_SIZE >> 20));
			fclose(f);
			free(img);
			return -1;
		}
		fclose(f);
		if (n == 0) {
			fprintf(stderr, "vsim: %s: empty image\n", path);
			free(img);
			return -1;
		}
		memcpy(b->eeprom + EEPROM_SIZE - n, img, n);
		free(img);
	}
	if (board_pci_slots(b, 0, 1))
		return -1;
	b->power = PWR_ON;
	return 0;
}

int
board_pci_slots(struct board *b, int dma_slot, int xhci_slot)
{
	pcie_free(b->pcie);
	b->xhci = NULL;
	if (dma_slot >= 0 && dma_slot == xhci_slot)
		return -1;
	if (!(b->pcie = pcie_new(dma_slot, dma_mem, dma_msi, b)))
		return -1;
	if (xhci_slot >= 0) {
		struct pcidev *x = xhci_new();
		if (pcie_attach(b->pcie, xhci_slot, x)) {
			free(x);
			return -1;
		}
		b->xhci = x;
	}
	return 0;
}

int
board_nvram_file(struct board *b, const char *path)
{
	b->nvram_file = path;
	FILE *f = fopen(path, "rb");
	if (!f)
		return errno == ENOENT ? 0 : -1;
	size_t n = fread(b->nvram, 1, NVRAM_SIZE, f);
	fclose(f);
	(void)n;
	return 0;
}

void
board_nvram_flush(struct board *b)
{
	if (!b->nvram_file || !b->nvram_dirty)
		return;
	FILE *f = fopen(b->nvram_file, "wb");
	if (f) {
		fwrite(b->nvram, 1, NVRAM_SIZE, f);
		fclose(f);
	}
	b->nvram_dirty = 0;
}

void
board_free(struct board *b)
{
	board_nvram_flush(b);
	free(b->eeprom);
	free(b->sram);
	dram_free(b->dram);
	pcie_free(b->pcie);
	b->pcie = NULL;
	b->xhci = NULL;
	vga_free(&b->vga);
	tpm_free();
	b->eeprom = b->sram = NULL;
	b->dram = NULL;
}

void
board_reset(struct board *b)
{
	b->power = PWR_ON;
	memset(&b->qport, 0, sizeof b->qport);
	b->evh = b->evn = 0;
	b->syscon_mce_addr = 0;
	b->post = 0;
	if (b->dram)
		dram_reset(b->dram);
	if (b->pcie)
		pcie_reset(b->pcie);
	vga_reset(&b->vga);
	pmc_reset(&b->pmc);
	i2c_reset(&b->i2c);
	tpm_reset();
}

int
board_suspended(const struct board *b)
{
	return b->pmc.suspended;
}

void
board_tick(struct board *b)
{
	b->now++;
	if (b->dram)
		dram_tick(b->dram);
	if (b->pcie)
		pcie_tick(b->pcie);
	if ((b->now & (PMC_TICK - 1)) == 0) {
		vga_tick(&b->vga, b->now);
		pmc_tick(&b->pmc, b->accesses);
		if (b->pmc.critical)
			b->power = PWR_THERMAL;
		else if (b->pmc.req_off)
			b->power = PWR_OFF;
		else if (b->pmc.req_reset)
			b->power = PWR_RESET;
		b->pmc.req_off = b->pmc.req_reset = 0;
	}
}

void
uart_rx_push(struct uart16550 *u, uint8_t c)
{
	if (u->rxt - u->rxh < sizeof u->rx)
		u->rx[u->rxt++ % sizeof u->rx] = c;
}

static int
uart_access(struct uart16550 *u, int we, unsigned reg, uint8_t *d)
{
	int dlab = u->lcr & 0x80;

	if (we) {
		switch (reg) {
		case 0:
			if (dlab)
				u->dll = *d;
			else if (u->tx)
				u->tx(u->txctx, *d);
			break;
		case 1:
			if (dlab)
				u->dlm = *d;
			else
				u->ier = *d & 0x0F;
			break;
		case 2: u->fcr = *d; break;
		case 3: u->lcr = *d; break;
		case 4: u->mcr = *d; break;
		case 7: u->scr = *d; break;
		default: break;	/* LSR, MSR: read-only, writes ignored */
		}
		return 0;
	}
	switch (reg) {
	case 0:
		if (dlab)
			*d = u->dll;
		else if (u->rxh != u->rxt)
			*d = u->rx[u->rxh++ % sizeof u->rx];
		else
			*d = 0;
		break;
	case 1: *d = dlab ? u->dlm : u->ier; break;
	case 2: *d = (u->fcr & 1) ? 0xC1 : 0x01; break;	/* no interrupt pending */
	case 3: *d = u->lcr; break;
	case 4: *d = u->mcr; break;
	case 5: *d = 0x60 | (u->rxh != u->rxt); break;	/* THRE | TEMT | DR */
	case 6: *d = 0xB0; break;			/* DCD, DSR, CTS */
	default: *d = u->scr; break;
	}
	return 0;
}

static void
ev_push(struct board *b, const struct board_event *e)
{
	if (b->evn == BOARD_EVQ) {
		b->ev_dropped++;
		return;
	}
	b->evq[(b->evh + b->evn) % BOARD_EVQ] = *e;
	b->evn++;
}

int
board_event(struct board *b, struct board_event *e)
{
	if (!b->evn)
		return 0;
	*e = b->evq[b->evh];
	b->evh = (b->evh + 1) % BOARD_EVQ;
	b->evn--;
	return 1;
}

static void
mce_cb(void *ctx, int type, uint64_t pa)
{
	struct board_event e = { .kind = EV_MCE, .type = (unsigned)type, .addr = pa };
	ev_push(ctx, &e);
}

/*
 * A write to the MSI window (Intel SDM Vol. 3A, "Message Signalled
 * Interrupts"): the destination APIC id is address bits 19:12, the vector
 * data bits 7:0 (fixed delivery; the other fields are ignored). Vectors 0-15
 * are reserved and dropped. Returns 0, or -1 if addr isn't in the window.
 */
static int msi_write(struct board *b, uint64_t addr, uint64_t v);

int
board_msi(void *ctx, uint64_t addr, uint64_t data)
{
	return msi_write(ctx, addr, data);
}

static int
msi_write(struct board *b, uint64_t addr, uint64_t v)
{
	if (addr < MSI_BASE || addr >= MSI_BASE + MSI_SIZE)
		return -1;
	struct board_event e = { .kind = EV_MSI, .core = (unsigned)((addr >> 12) & 0xFF),
	    .vector = (unsigned)(v & 0xFF) };
	if (e.vector >= 16)
		ev_push(b, &e);
	return 0;
}

int
board_access(struct board *b, int we, uint64_t addr, unsigned size, uint8_t *data,
    unsigned *lat)
{
	uint64_t n = 1ull << size;

	*lat = 1;
	if (size > 6 || addr + n < addr)
		return -1;
	if (b->pcie) {
		int r = pcie_access(b->pcie, we, addr, size, data);
		if (r != 1)
			return r;
	}
	b->accesses++;
	if (addr >= VGA_VRAM && addr + n <= VGA_VRAM + VGA_VRAM_SIZE)
		return vga_mem(&b->vga, we, addr - VGA_VRAM, (unsigned)n, data);
	if (addr >= VGA_REGS && addr + n <= VGA_REGS + 0x1000)
		return vga_reg(&b->vga, we, (unsigned)(addr - VGA_REGS), (unsigned)n, data);
	if (addr >= I2C_BASE && addr + n <= I2C_BASE + 0x1000)
		return i2c_reg(&b->i2c, we, (unsigned)(addr - I2C_BASE), (unsigned)n, data, b->now);
	if (addr >= PMC_BASE && addr + n <= PMC_BASE + 0x1000)
		return pmc_reg(&b->pmc, we, (unsigned)(addr - PMC_BASE), (unsigned)n, data);
	if (addr >= NVRAM_BASE && addr + n <= NVRAM_BASE + NVRAM_SIZE) {
		uint8_t *p = b->nvram + (addr - NVRAM_BASE);
		if (we) {
			memcpy(p, data, n);
			b->nvram_dirty = 1;
		} else
			memcpy(data, p, n);
		return 0;
	}
	if (b->dram) {
		if (addr >= DRAM_BASE && addr < DRAM_BASE + DRAM_WINDOW)
			return dram_mem(b->dram, we, addr, (unsigned)n, data, b->now, lat);
		if (addr >= SMB_BASE && addr < SMB_BASE + SMB_SIZE)
			return dram_smb(b->dram, we, addr - SMB_BASE, (unsigned)n, data);
		if (addr >= MC_BASE && addr < MC_BASE + MC_SIZE)
			return dram_mc(b->dram, we, addr - MC_BASE, (unsigned)n, data);
	}
	if (addr >= EEPROM_BASE && addr + n <= EEPROM_BASE + EEPROM_SIZE) {
		if (we)
			return -1;	/* read-only to the core for now */
		memcpy(data, b->eeprom + (addr - EEPROM_BASE), n);
		return 0;
	}
	if (b->sram && addr >= SRAM_BASE && addr + n <= SRAM_BASE + SRAM_SIZE) {
		if (we)
			memcpy(b->sram + (addr - SRAM_BASE), data, n);
		else
			memcpy(data, b->sram + (addr - SRAM_BASE), n);
		return 0;
	}
	if (addr >= UART0_BASE && addr < UART0_BASE + 8) {
		if (size != 0)
			return -1;
		return uart_access(&b->uart, we, (unsigned)(addr - UART0_BASE), data);
	}
	if (addr >= MSI_BASE && addr + n <= MSI_BASE + MSI_SIZE) {
		if (size != 2)
			return -1;	/* MSIs are dword writes */
		if (!we) {
			memset(data, 0, n);
			return 0;
		}
		uint32_t v;
		memcpy(&v, data, 4);		/* little-endian host */
		return msi_write(b, addr, v);
	}
	if (addr == SYSCON_BASE + 8 && size == 3) {
		if (we)
			memcpy(&b->syscon_mce_addr, data, 8);
		else
			memcpy(data, &b->syscon_mce_addr, 8);
		return 0;
	}
	if (addr == SYSCON_BASE && (size == 2 || size == 3)) {
		board_nvram_flush(b);	/* power off / reset: settings are safe */
		if (!we) {
			memset(data, 0, n);
			return 0;
		}
		uint32_t v = (uint32_t)data[0] | (uint32_t)data[1] << 8 |
		    (uint32_t)data[2] << 16 | (uint32_t)data[3] << 24;
		switch (v & 0xFFFF) {
		case 0x5555: b->power = PWR_OFF; break;
		case 0x3333: b->power = PWR_FAIL; b->failcode = v >> 16; break;
		case 0x7777: b->power = PWR_RESET; break;
		case SYSCON_NMI: {
			struct board_event e = { .kind = EV_NMI };
			ev_push(b, &e);
			break;
		}
		default:
			if ((v & 0xFF00) == SYSCON_MCE && (v & 0xFF) >= 1 && (v & 0xFF) <= 2) {
				struct board_event e = { .kind = EV_MCE, .type = v & 0xFF,
				    .addr = b->syscon_mce_addr };
				ev_push(b, &e);
			}
			break;
		}
		return 0;
	}
	if (addr >= QPORT_CTL && addr < QPORT_CTL + 0x80 && size == 3 && !(addr & 7)) {
		struct qport *q = &b->qport;
		unsigned off = (unsigned)(addr - QPORT_CTL);
		uint64_t v = 0;
		if (we) {
			uint64_t w;
			memcpy(&w, data, 8);
			if (off == 0x30) {
				q->msi_addr = w;
				return 0;
			}
			if (off == 0x38) {
				q->msi_data = w;
				return 0;
			}
			if (off != 0x08)
				return -1;
			if (q->count) {
				q->head = (q->head + 1) % QPORT_DEPTH;
				q->count--;
			}
			if (q->msi_addr)
				msi_write(b, q->msi_addr, q->msi_data);
			return 0;
		}
		switch (off) {
		case 0x00: v = q->count; break;
		case 0x08: v = 0; break;
		case 0x10: v = q->count ? q->pasid[q->head] : 0; break;
		case 0x18: v = QPORT_DEPTH; break;
		case 0x20: v = q->accepted; break;
		case 0x28: v = q->retried; break;
		case 0x30: v = q->msi_addr; break;
		case 0x38: v = q->msi_data; break;
		default:
			if (off >= 0x40 && q->count)
				memcpy(&v, q->desc[q->head] + (off - 0x40), 8);
			break;
		}
		memcpy(data, &v, 8);		/* little-endian host */
		return 0;
	}
	if (addr == STRAPS_BASE && (size == 2 || size == 3)) {
		if (we)
			return -1;
		memset(data, 0, n);
		memcpy(data, &b->straps, 4);	/* little-endian host */
		return 0;
	}
	return -1;
}

/*
 * The I/O space: the ports a PC-compatible board decodes on its LPC/eSPI
 * bus (docs/platform.md "I/O ports"). A multi-byte access to COM1 goes to
 * consecutive registers, one byte each, as the 8-bit device would see it.
 */
int
board_io(struct board *b, int we, uint16_t port, unsigned size, uint8_t *data)
{
	unsigned n = 1u << size;

	if (size > 2)
		return -1;
	b->accesses++;
	for (unsigned i = 0; i < n; i++) {
		uint16_t p = (uint16_t)(port + i);
		uint8_t *d = data + i;
		if (p >= PORT_COM1 && p < PORT_COM1 + 8)
			uart_access(&b->uart, we, p - PORT_COM1, d);
		else if (p == PORT_POST) {
			if (we) {
				b->post = *d;
				b->nposts++;
				if (b->post_cb)
					b->post_cb(b->post_ctx, *d);
			} else
				*d = b->post;
		} else if (p == PORT_RST) {
			/* reset control register: bit 2 (RST_CPU) starts a reset;
			   bit 1 (SYS_RST) selects hard vs soft, both are a board
			   reset here */
			if (we) {
				if (*d & 4) {
					board_nvram_flush(b);
					b->power = PWR_RESET;
				}
			} else
				*d = 0;
		} else if (!we)
			*d = 0xFF;	/* nothing decodes the port */
	}
	return 0;
}

int
board_enq(struct board *b, uint64_t addr, const uint8_t data[64], uint32_t pasid, int *retry)
{
	struct qport *q = &b->qport;

	if (addr < QPORT_BASE || addr >= QPORT_BASE + 0x1000 || (addr & 63))
		return -1;
	if (q->count == QPORT_DEPTH) {
		q->retried++;
		*retry = 1;
		return 0;
	}
	unsigned t = (q->head + q->count) % QPORT_DEPTH;
	memcpy(q->desc[t], data, 64);
	q->pasid[t] = pasid;
	q->count++;
	q->accepted++;
	*retry = 0;
	return 0;
}

/*
 * DMA from PCIe devices (through the IOMMU): physical memory only (EEPROM
 * reads, SRAM, DRAM), no timing. Interrupt-file writes go through dma_msi.
 */
static int
dma_mem(void *ctx, int we, uint64_t pa, unsigned n, uint8_t *data)
{
	struct board *b = ctx;
	unsigned lat;
	if (pa + n < pa)
		return -1;
	if (b->dram && pa >= DRAM_BASE && pa + n <= DRAM_BASE + DRAM_WINDOW)
		return dram_mem(b->dram, we, pa, n, data, b->now, &lat);
	if (!we && pa >= EEPROM_BASE && pa + n <= EEPROM_BASE + EEPROM_SIZE) {
		memcpy(data, b->eeprom + (pa - EEPROM_BASE), n);
		return 0;
	}
	if (b->sram && pa >= SRAM_BASE && pa + n <= SRAM_BASE + SRAM_SIZE) {
		if (we)
			memcpy(b->sram + (pa - SRAM_BASE), data, n);
		else
			memcpy(data, b->sram + (pa - SRAM_BASE), n);
		return 0;
	}
	return -1;
}

static int
dma_msi(void *ctx, uint64_t pa, uint64_t v)
{
	return msi_write(ctx, pa, v);
}

int
board_preload(struct board *b, uint64_t addr, const uint8_t *data, size_t n)
{
	if (addr + n < addr)
		goto bad;
	if (b->sram && addr >= SRAM_BASE && addr + n <= SRAM_BASE + SRAM_SIZE) {
		memcpy(b->sram + (addr - SRAM_BASE), data, n);
		return 0;
	}
	if (addr >= EEPROM_BASE && addr + n <= EEPROM_BASE + EEPROM_SIZE) {
		memcpy(b->eeprom + (addr - EEPROM_BASE), data, n);
		return 0;
	}
bad:
	fprintf(stderr, "vsim: can't preload 0x%llx bytes at 0x%llx: only the test board's "
	    "SRAM (0x%llx, %llu KiB) and the EEPROM (0x%llx-0xffffffff) can be preloaded\n",
	    (unsigned long long)n, (unsigned long long)addr,
	    (unsigned long long)SRAM_BASE, (unsigned long long)(SRAM_SIZE >> 10),
	    (unsigned long long)EEPROM_BASE);
	return -1;
}
