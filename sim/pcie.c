/*
 * vsim PCIe root complex: ECAM config space, a host bridge at 00:00.0, one
 * root port per slot at 00:01.0 .. 00:08.0, BAR windows, bus mastering and
 * MSI through the IOMMU. Plus the DMA test endpoint.
 *
 * Header and capability offsets follow the PCI Express Base Specification
 * (PCI-SIG); the exact layout we model is in docs/platform.md "PCIe".
 */
#include <stdlib.h>
#include <string.h>

#include "pcie.h"

/* ------------------------------------------------------------------ config */

static void
put(struct pcifn *f, unsigned off, unsigned n, uint32_t v, uint32_t wm)
{
	for (unsigned i = 0; i < n; i++) {
		f->cfg[off + i] = (uint8_t)(v >> (8 * i));
		f->wm[off + i] = (uint8_t)(wm >> (8 * i));
	}
}

static uint32_t
get(const struct pcifn *f, unsigned off, unsigned n)
{
	uint32_t v = 0;
	for (unsigned i = 0; i < n; i++)
		v |= (uint32_t)f->cfg[off + i] << (8 * i);
	return v;
}

/*
 * The i440FX SMRAM register (72h; 82441FX datasheet 3.2.21): C_BASE_SEG
 * 010b (A0000h-BFFFFh) fixed, G_SMRAME (bit 3) enables SMRAM, D_OPEN (6)
 * makes it visible outside SMM, D_CLS (5), D_LCK (4). Setting D_LCK clears
 * D_OPEN and makes D_OPEN, D_LCK and G_SMRAME read-only until reset.
 */
static void
smram_written(struct pcifn *f)
{
	if (f->cfg[0x72] & 0x10) {
		f->cfg[0x72] &= (uint8_t)~0x40;
		f->wm[0x72] = 0x20;		/* only D_CLS stays writable */
	}
}

static void
cfg_write(struct pcifn *f, unsigned off, unsigned n, const uint8_t *d)
{
	for (unsigned i = 0; i < n && off + i < 4096; i++)
		f->cfg[off + i] = (uint8_t)((f->cfg[off + i] & ~f->wm[off + i]) |
		    (d[i] & f->wm[off + i]));
}

/*
 * Bus 0 looks like QEMU's "pc" machine to firmware (SeaBIOS, docs/platform.md
 * "PC compatibility"): an Intel 82441FX (i440FX) host bridge with its PAM
 * registers (Intel 82441FX PCI and Memory Controller datasheet, 290549-001,
 * 3.2.17 "PAM0-PAM6"), and an 82371SB PIIX3 ISA bridge with PIIX4
 * (82371AB) power-management function at device 1.
 */
static void
init_host(struct pcifn *f)
{
	memset(f, 0, sizeof *f);
	put(f, 0x00, 2, 0x8086, 0);
	put(f, 0x02, 2, 0x1237, 0);	/* 82441FX */
	put(f, 0x04, 2, 0x0006, 0x0006);
	put(f, 0x08, 4, 0x06000002, 0);	/* class 06/00/00, revision 2 */
	for (unsigned r = 0x59; r <= 0x5F; r++)
		put(f, r, 1, 0, 0x33);	/* PAM0-PAM6: RE/WE per half */
	put(f, 0x72, 1, 0x02, 0x78);	/* SMRAM: see smram_written() */
}

static void
init_piix_isa(struct pcifn *f)
{
	memset(f, 0, sizeof *f);
	put(f, 0x00, 2, 0x8086, 0);
	put(f, 0x02, 2, 0x7000, 0);	/* 82371SB PIIX3 ISA */
	put(f, 0x04, 2, 0x0007, 0);
	put(f, 0x08, 4, 0x06010000, 0);	/* class 06/01/00: ISA bridge */
	put(f, 0x0E, 1, 0x80, 0);	/* multi-function */
	put(f, 0x60, 4, 0x80808080, 0x8F8F8F8F);	/* PIRQRC[A-D]: disabled */
}

static void
init_piix_pm(struct pcifn *f)
{
	memset(f, 0, sizeof *f);
	put(f, 0x00, 2, 0x8086, 0);
	put(f, 0x02, 2, 0x7113, 0);	/* 82371AB PIIX4 ACPI */
	put(f, 0x04, 2, 0x0000, 0x0001);
	put(f, 0x08, 4, 0x06800003, 0);	/* class 06/80/00, revision 3 */
	put(f, 0x3D, 1, 0x01, 0);	/* INTA# (the SCI) */
	put(f, 0x40, 4, 0x00000001, 0x0000FFC0);	/* PMBA: I/O */
	put(f, 0x80, 1, 0x00, 0x01);	/* PMREGMISC: PM I/O enable */
	put(f, 0x90, 4, 0x00000001, 0x0000FFF0);	/* SMBBA */
	put(f, 0xD2, 1, 0x00, 0x0F);	/* SMBHSTCFG */
	/* DEVACTB (58h): device activity enables; bit 25 APMC_EN makes a write
	   to the APM control port B2h raise SMI (board_io) */
	put(f, 0x58, 4, 0, 0x3FFFFFFF);
}

static void
init_rp(struct pcifn *f, int slot)
{
	memset(f, 0, sizeof *f);
	put(f, 0x00, 2, PCI_VENDOR, 0);
	put(f, 0x02, 2, PCI_DEV_RP, 0);
	put(f, 0x04, 2, 0x0000, 0x0546);	/* MSE, BME, parity, SERR, INTx disable */
	put(f, 0x06, 2, 0x0010, 0);		/* capability list */
	put(f, 0x08, 4, 0x06040001, 0);	/* class 06/04/00: PCI-PCI bridge */
	put(f, 0x0E, 1, 0x01, 0);		/* type 1 header */
	put(f, 0x18, 4, 0, 0x00FFFFFF);	/* primary, secondary, subordinate */
	put(f, 0x20, 2, 0, 0xFFF0);		/* memory base / limit */
	put(f, 0x22, 2, 0, 0xFFF0);
	put(f, 0x24, 2, 0x0001, 0xFFF0);	/* prefetchable base / limit, 64-bit */
	put(f, 0x26, 2, 0x0001, 0xFFF0);
	put(f, 0x28, 4, 0, 0xFFFFFFFF);	/* prefetchable upper base / limit */
	put(f, 0x2C, 4, 0, 0xFFFFFFFF);
	put(f, 0x34, 1, 0x40, 0);		/* capabilities */
	put(f, 0x3E, 2, 0, 0x0043);		/* bridge control */
	put(f, 0x40, 2, 0x0010, 0);		/* PCIe capability */
	put(f, 0x42, 2, 0x0142, 0);		/* v2, root port, slot implemented */
	put(f, 0x54, 4, (uint32_t)slot << 19, 0);	/* physical slot number */
}

static void
init_dmat(struct pcifn *f)
{
	memset(f, 0, sizeof *f);
	put(f, 0x00, 2, PCI_VENDOR, 0);
	put(f, 0x02, 2, PCI_DEV_DMAT, 0);
	put(f, 0x04, 2, 0x0000, 0x0546);
	put(f, 0x06, 2, 0x0010, 0);
	put(f, 0x08, 4, 0x08800001, 0);	/* class 08/80/00: other system peripheral */
	put(f, 0x10, 4, 0x00000004, 0xFFFFF000);	/* BAR0: 4 KiB, 64-bit memory */
	put(f, 0x14, 4, 0, 0xFFFFFFFF);
	put(f, 0x2C, 4, (uint32_t)PCI_DEV_DMAT << 16 | PCI_VENDOR, 0);
	put(f, 0x34, 1, 0x40, 0);
	put(f, 0x40, 2, 0x6005, 0);		/* MSI, next 0x60 */
	put(f, 0x42, 2, 0x0080, 0x0001);	/* 64-bit address; enable */
	put(f, 0x44, 4, 0, 0xFFFFFFFC);
	put(f, 0x48, 4, 0, 0xFFFFFFFF);
	put(f, 0x4C, 2, 0, 0xFFFF);
	put(f, 0x60, 2, 0x0010, 0);		/* PCIe capability */
	put(f, 0x62, 2, 0x0002, 0);		/* v2, endpoint */
	put(f, 0x100, 4, 0x001B | 1u << 16 | 0x108u << 20, 0);	/* PASID, next PRI */
	put(f, 0x104, 2, 20u << 8 | 4, 0);	/* 20-bit PASIDs, privileged mode */
	put(f, 0x106, 2, 0, 0x0005);		/* enable, privileged enable */
	put(f, 0x108, 4, 0x0013 | 1u << 16, 0);	/* page request interface */
	put(f, 0x10C, 2, 0, 0x0001);		/* enable */
	put(f, 0x110, 4, 1, 0);		/* outstanding page request capacity */
	put(f, 0x114, 4, 0, 0xFFFFFFFF);	/* allocation */
}

/* ------------------------------------------------------------------ DMA test endpoint */

enum { DT_IDLE, DT_BUSY, DT_DONE, DT_ERROR, DT_WAIT };

struct dmat {
	struct pcidev d;	/* first */
	uint64_t src, dst, len, pasid, ctrl, status, done, prgi, pattern;
	unsigned next_prgi, wait_prgi;
};

static int
dmat_bar(struct pcidev *d, int we, unsigned bar, uint64_t off, unsigned n, uint8_t *data)
{
	struct dmat *t = (struct dmat *)d;
	uint64_t v = 0, *r = NULL;
	if (bar != 0 || n != 8 || (off & 7))
		return -1;
	if (we)
		memcpy(&v, data, 8);
	switch (off) {
	case 0x00:
		v = 0x54414D44;		/* "DMAT" */
		break;
	case 0x08: r = &t->src; break;
	case 0x10: r = &t->dst; break;
	case 0x18: r = &t->len; break;
	case 0x20: r = &t->pasid; break;
	case 0x28:
		if (we) {
			if (t->status == DT_BUSY || t->status == DT_WAIT)
				return 0;
			t->ctrl = v & 6;
			if (v & 1) {
				t->done = 0;
				t->status = t->len ? DT_BUSY : DT_DONE;
				if (!t->len && (t->ctrl & 2))
					pcie_msi(d);
			}
			return 0;
		}
		v = t->ctrl;
		break;
	case 0x30: if (we) return 0; v = t->status; break;
	case 0x38: if (we) return 0; v = t->done; break;
	case 0x40: if (we) return 0; v = t->prgi; break;
	case 0x48: r = &t->pattern; break;
	default:
		return we ? 0 : (memset(data, 0, 8), 0);
	}
	if (r) {
		if (we) {
			if (t->status != DT_BUSY && t->status != DT_WAIT)
				*r = v;
			return 0;
		}
		v = *r;
	}
	memcpy(data, &v, 8);
	return 0;
}

static void
dmat_finish(struct dmat *t, int st)
{
	t->status = st;
	if (t->ctrl & 2)
		pcie_msi(&t->d);
}

static int
dmat_req(struct dmat *t, int we, uint64_t a, uint8_t *buf, unsigned n)
{
	int pv = (t->pasid >> 31) & 1, priv = (t->pasid >> 30) & 1;
	int r = pcie_dma(&t->d, we, a, buf, n, pv, (unsigned)(t->pasid & 0xFFFFF), priv,
	    t->next_prgi);
	if (r == IO_PRI) {
		t->wait_prgi = t->next_prgi;
		t->prgi = t->next_prgi;
		t->next_prgi = (t->next_prgi + 1) & 511;
		t->status = DT_WAIT;
	} else if (r == IO_FAULT)
		dmat_finish(t, DT_ERROR);
	return r;
}

static void
dmat_tick(struct pcidev *d)
{
	struct dmat *t = (struct dmat *)d;
	uint8_t buf[64];
	if (t->status != DT_BUSY)
		return;
	uint64_t s = t->src + t->done, ds = t->dst + t->done;
	uint64_t n = t->len - t->done;
	if (n > 64)
		n = 64;
	if (n > 4096 - (s & 4095))
		n = 4096 - (s & 4095);
	if (n > 4096 - (ds & 4095))
		n = 4096 - (ds & 4095);
	if (t->ctrl & 4) {			/* fill with PATTERN */
		for (unsigned i = 0; i < n; i++)
			buf[i] = (uint8_t)(t->pattern >> (8 * ((t->done + i) & 7)));
	} else if (dmat_req(t, 0, s, buf, (unsigned)n) != IO_OK)
		return;
	if (dmat_req(t, 1, ds, buf, (unsigned)n) != IO_OK)
		return;
	t->done += n;
	if (t->done == t->len)
		dmat_finish(t, DT_DONE);
}

static void
dmat_resp(struct pcidev *d, unsigned prgi, int ok)
{
	struct dmat *t = (struct dmat *)d;
	if (t->status != DT_WAIT || prgi != t->wait_prgi)
		return;
	if (ok)
		t->status = DT_BUSY;		/* retry the request */
	else
		dmat_finish(t, DT_ERROR);
}

static void
dmat_reset(struct pcidev *d)
{
	struct dmat *t = (struct dmat *)d;
	init_dmat(&d->f);
	t->src = t->dst = t->len = t->pasid = t->ctrl = t->status = 0;
	t->done = t->prgi = t->pattern = 0;
	t->next_prgi = t->wait_prgi = 0;
}

static struct pcidev *
dmat_new(void)
{
	struct dmat *t = calloc(1, sizeof *t);
	if (!t)
		return NULL;
	t->d.bar = dmat_bar;
	t->d.tick = dmat_tick;
	t->d.reset = dmat_reset;
	t->d.resp = dmat_resp;
	dmat_reset(&t->d);
	return &t->d;
}

/* ------------------------------------------------------------------ root complex */

static unsigned
secbus(const struct pcie *pc, int k)
{
	return pc->rp[k].cfg[0x19];
}

static void
iommu_resp(void *ctx, unsigned devid, unsigned pasid, unsigned prgi, int ok)
{
	struct pcie *pc = ctx;
	(void)pasid;
	for (int k = 0; k < PCI_SLOTS; k++)
		if (pc->slot[k] && secbus(pc, k) && devid == secbus(pc, k) << 8 &&
		    pc->slot[k]->resp)
			pc->slot[k]->resp(pc->slot[k], prgi, ok);
}

static int
io_mem(void *ctx, int we, uint64_t pa, unsigned n, uint8_t *d)
{
	struct pcie *pc = ctx;
	return pc->mem(pc->ctx, we, pa, n, d);
}

static int
io_msi(void *ctx, uint64_t pa, uint64_t v)
{
	struct pcie *pc = ctx;
	return pc->msi(pc->ctx, pa, v);
}

struct pcie *
pcie_new(int dma_slot, int (*mem)(void *, int, uint64_t, unsigned, uint8_t *),
    int (*msi)(void *, uint64_t, uint64_t), void *ctx)
{
	struct pcie *pc = calloc(1, sizeof *pc);
	if (!pc)
		return NULL;
	pc->mem = mem;
	pc->msi = msi;
	pc->ctx = ctx;
	pc->iommu.mem = io_mem;
	pc->iommu.msi = io_msi;
	pc->iommu.resp = iommu_resp;
	pc->iommu.ctx = pc;
	if (dma_slot >= 0 && dma_slot < PCI_SLOTS) {
		if (!(pc->slot[dma_slot] = dmat_new())) {
			free(pc);
			return NULL;
		}
		pc->slot[dma_slot]->pc = pc;
		pc->slot[dma_slot]->slot = dma_slot;
	}
	pcie_reset(pc);
	return pc;
}

int
pcie_attach(struct pcie *pc, int slot, struct pcidev *d)
{
	if (!d || slot < 0 || slot >= PCI_SLOTS || pc->slot[slot])
		return -1;
	pc->slot[slot] = d;
	d->pc = pc;
	d->slot = slot;
	d->reset(d);
	return 0;
}

void
pcie_free(struct pcie *pc)
{
	if (!pc)
		return;
	for (int k = 0; k < PCI_SLOTS; k++)
		free(pc->slot[k]);
	free(pc);
}

void
pcie_reset(struct pcie *pc)
{
	init_host(&pc->host);
	init_piix_isa(&pc->piix_isa);
	init_piix_pm(&pc->piix_pm);
	for (int k = 0; k < PCI_SLOTS; k++) {
		init_rp(&pc->rp[k], k);
		if (pc->slot[k])
			pc->slot[k]->reset(pc->slot[k]);
	}
	iommu_reset(&pc->iommu);
}

void
pcie_tick(struct pcie *pc)
{
	for (int k = 0; k < PCI_SLOTS; k++)
		if (pc->slot[k] && pc->slot[k]->tick)
			pc->slot[k]->tick(pc->slot[k]);
}

/* the function a config address selects, or NULL */
static struct pcifn *
cfg_fn(struct pcie *pc, uint64_t off)
{
	unsigned bus = (unsigned)(off >> 20) & 0xFF, dev = (unsigned)(off >> 15) & 31,
	    fn = (unsigned)(off >> 12) & 7;
	if (bus == 0) {
		if (dev == 1)
			return fn == 0 ? &pc->piix_isa : fn == 3 ? &pc->piix_pm : NULL;
		if (fn)
			return NULL;
		if (dev == 0)
			return &pc->host;
		if (dev >= PCI_RP_DEV0 && dev < PCI_RP_DEV0 + PCI_SLOTS)
			return &pc->rp[dev - PCI_RP_DEV0];
		return NULL;
	}
	for (int k = 0; k < PCI_SLOTS; k++)
		if (secbus(pc, k) == bus)
			return (dev == 0 && fn == 0 && pc->slot[k]) ? &pc->slot[k]->f : NULL;
	return NULL;
}

static int
cfg_access(struct pcie *pc, int we, uint64_t off, unsigned size, uint8_t *data)
{
	unsigned n = 1u << size, reg = (unsigned)(off & 0xFFF);
	if (size > 3 || (reg & (n - 1)))
		return -1;
	struct pcifn *f = cfg_fn(pc, off);
	if (!f) {
		if (!we)
			memset(data, 0xFF, n);	/* no device: all ones */
		return 0;
	}
	if (we) {
		cfg_write(f, reg, n, data);
		if (f == &pc->host && reg <= 0x72 && reg + n > 0x72)
			smram_written(f);
	} else
		memcpy(data, f->cfg + reg, n);
	return 0;
}

int
pcie_cfg(struct pcie *pc, int we, unsigned bus, unsigned dev, unsigned fn, unsigned reg,
    unsigned size, uint8_t *data)
{
	uint64_t off = (uint64_t)(bus & 0xFF) << 20 | (uint64_t)(dev & 31) << 15 |
	    (uint64_t)(fn & 7) << 12 | (reg & 0xFFF);
	return cfg_access(pc, we, off, size, data);
}

static uint64_t
bar0(const struct pcifn *f)
{
	return ((uint64_t)get(f, 0x14, 4) << 32) | (get(f, 0x10, 4) & ~0xFull);
}

/* does root port k forward a memory access at addr to its secondary side? */
static int
rp_forwards(const struct pcifn *rp, uint64_t a)
{
	if (!(get(rp, 0x04, 2) & 2))
		return 0;
	uint64_t mb = (uint64_t)(get(rp, 0x20, 2) & 0xFFF0) << 16;
	uint64_t ml = ((uint64_t)(get(rp, 0x22, 2) & 0xFFF0) << 16) | 0xFFFFF;
	if (mb <= ml && a >= mb && a <= ml)
		return 1;
	uint64_t pb = ((uint64_t)(get(rp, 0x24, 2) & 0xFFF0) << 16) |
	    (uint64_t)get(rp, 0x28, 4) << 32;
	uint64_t pl = ((uint64_t)(get(rp, 0x26, 2) & 0xFFF0) << 16) | 0xFFFFF |
	    (uint64_t)get(rp, 0x2C, 4) << 32;
	return pb <= pl && a >= pb && a <= pl;
}

int
pcie_access(struct pcie *pc, int we, uint64_t addr, unsigned size, uint8_t *data)
{
	unsigned n = 1u << size;
	if (addr >= ECAM_BASE && addr < ECAM_BASE + ECAM_SIZE)
		return cfg_access(pc, we, addr - ECAM_BASE, size, data);
	if (addr >= IOMMU_BASE && addr < IOMMU_BASE + IOMMU_SIZE) {
		uint64_t v = 0;
		if (size != 3 || (addr & 7))
			return -1;
		if (we)
			memcpy(&v, data, 8);
		if (iommu_reg(&pc->iommu, we, (unsigned)(addr - IOMMU_BASE), &v))
			return -1;
		if (!we)
			memcpy(data, &v, 8);
		return 0;
	}
	/* BARs: the board decodes PCI last, so like a PC host bridge every
	 * address nothing else claimed goes to the root ports, whose windows
	 * forward it (firmware places BARs anywhere in the PCI hole below 4 GiB,
	 * e.g. SeaBIOS just below the IOAPIC, not only in PCI_MEM32/64) */
	{
		for (int k = 0; k < PCI_SLOTS; k++) {
			struct pcidev *d = pc->slot[k];
			if (!d || !(get(&d->f, 0x04, 2) & 2) || !rp_forwards(&pc->rp[k], addr))
				continue;
			uint64_t b = bar0(&d->f);
			uint64_t sz = d->bar_size ? d->bar_size : 4096;
			if (b && addr >= b && addr + n <= b + sz)
				return d->bar(d, we, 0, addr - b, n, data);
		}
		if ((addr >= PCI_MEM32_BASE && addr < PCI_MEM32_BASE + PCI_MEM32_SIZE) ||
		    (addr >= PCI_MEM64_BASE && addr < PCI_MEM64_BASE + PCI_MEM64_SIZE))
			return -1;		/* unclaimed in a window: unsupported request */
	}
	return 1;
}

int
pcie_dma(struct pcidev *d, int we, uint64_t iova, uint8_t *data, unsigned n,
    int pv, unsigned pasid, int priv, unsigned prgi)
{
	struct pcie *pc = d->pc;
	int k = d->slot;
	/* bus master enable at the endpoint and its root port */
	if (!(get(&d->f, 0x04, 2) & 4) || !(get(&pc->rp[k], 0x04, 2) & 4) || !secbus(pc, k)) {
		pc->dma_errors++;
		return IO_FAULT;
	}
	struct io_req r = { 0 };
	r.devid = secbus(pc, k) << 8;
	r.pv = pv && (get(&d->f, 0x106, 2) & 1);
	r.pasid = r.pv ? pasid : 0;
	r.priv = r.pv && priv && (get(&d->f, 0x106, 2) & 4);
	r.write = we;
	r.pri_ok = get(&d->f, 0x10C, 2) & 1;
	r.prgi = prgi;
	while (n) {
		unsigned c = n > 64 ? 64 : n;
		if (c > 4096 - (iova & 4095))
			c = (unsigned)(4096 - (iova & 4095));
		r.iova = iova;
		int res = iommu_dma(&pc->iommu, &r, data, c);
		if (res != IO_OK) {
			if (res == IO_FAULT)
				pc->dma_errors++;
			return res;
		}
		pc->dma_bytes += c;
		iova += c;
		data += c;
		n -= c;
	}
	return IO_OK;
}

int
pcie_msi(struct pcidev *d)
{
	const struct pcifn *f = &d->f;
	if (!(get(f, 0x42, 2) & 1))
		return -1;
	uint64_t a = get(f, 0x44, 4) | (uint64_t)get(f, 0x48, 4) << 32;
	uint8_t b[4];
	uint32_t v = get(f, 0x4C, 2);
	memcpy(b, &v, 4);
	return pcie_dma(d, 1, a, b, 4, 0, 0, 0, 0) == IO_OK ? 0 : -1;
}
