/*
 * vsim IOMMU: device directory, process directory, two-stage translation
 * with vercpu's PTE format, fault and page-request queues, commands, MSI
 * remapping. Ported from vercpu (~/vercpu, commit 936c3cb), whose
 * docs/isa/21-iommu.md is the spec (page tables: its 18-privileged.md
 * "Paging" and 20-hypervisor-interrupts.md 5.1); MSIs are now x86 MSI
 * window writes (board.h MSI_BASE). Not Intel VT-d (yet).
 * There is no IOTLB: every request walks.
 */
#include <string.h>

#include "iommu.h"

#define MSI_LO	0xFEE00000ull	/* the MSI window (board.h MSI_BASE) */
#define MSI_HI	0xFEF00000ull

/* walk results */
enum { W_OK, W_FAULT, W_NOTPRESENT, W_BUSERR };

struct dctx {
	uint64_t d[8];
};
#define DC_V	(1ull << 0)
#define DC_S1	(1ull << 1)
#define DC_S2	(1ull << 2)
#define DC_PASID (1ull << 3)
#define DC_PRI	(1ull << 4)
#define DC_MSIP	(1ull << 5)

void
iommu_reset(struct iommu *io)
{
	io->ctrl = io->ddtp = io->cqb = io->cqh = io->cqt = 0;
	io->fqb = io->fqh = io->fqt = io->pqb = io->pqh = io->pqt = 0;
	io->fq_msi_addr = io->fq_msi_data = io->pq_msi_addr = io->pq_msi_data = 0;
	io->cq_msi_addr = io->cq_msi_data = io->status = 0;
}

static int
rd64(struct iommu *io, uint64_t pa, uint64_t *v)
{
	uint8_t b[8];
	if (io->mem(io->ctx, 0, pa, 8, b))
		return -1;
	memcpy(v, b, 8);	/* little-endian host */
	return 0;
}

static int
wr64(struct iommu *io, uint64_t pa, uint64_t v)
{
	uint8_t b[8];
	memcpy(b, &v, 8);
	return io->mem(io->ctx, 1, pa, 8, b);
}

/* ------------------------------------------------------------------ walks */

/*
 * Stage 2 (20 section 5.1): gpa -> pa for a read or write. Implicit stage-1
 * table accesses use the same rules (DEFER grants both).
 */
static int
stage2(struct iommu *io, const struct dctx *dc, uint64_t gpa, int write, uint64_t *pa)
{
	uint64_t a = (dc->d[1] & ((1ull << 40) - 1)) << 12;
	for (int lvl = 4; lvl >= 0; lvl--) {
		uint64_t pte, addr = a + (((gpa >> (12 + 9 * lvl)) & 511) << 3);
		if (rd64(io, addr, &pte))
			return W_BUSERR;
		if (!(pte & 1))
			return W_NOTPRESENT;
		unsigned perm = (pte >> 1) & 7;
		if ((pte & (1ull << 11)) || perm == 7 || perm == 5 || ((pte >> 9) & 3) == 3)
			return W_FAULT;
		uint64_t base = pte & 0x000FFFFFFFFFF000ull;
		if (perm == 0) {
			if (lvl == 0 || (pte & 0x00F00000000007D0ull))
				return W_FAULT;
			a = base;
			continue;
		}
		uint64_t low = (1ull << (12 + 9 * lvl)) - 1;
		if (lvl == 4 || (base & low) || !(pte & 0x10) || ((pte >> 52) & 15))
			return W_FAULT;
		int r = perm == 1 || perm == 2 || perm == 4 || perm == 6;
		int w = perm == 2 || perm == 6;
		if (write ? !w : !r)
			return W_FAULT;
		if (!(pte & 0x40) || (write && !(pte & 0x80))) {
			pte |= 0x40 | (write ? 0x80 : 0);
			if (wr64(io, addr, pte))
				return W_BUSERR;
		}
		*pa = (base & ~low) | (gpa & low);
		return W_OK;
	}
	return W_FAULT;
}

/* a table access address: guest-physical when S2 */
static int
tbl(struct iommu *io, const struct dctx *dc, uint64_t a, int write, uint64_t *pa, int *s2f)
{
	if (!(dc->d[0] & DC_S2)) {
		*pa = a;
		return W_OK;
	}
	int r = stage2(io, dc, a, write, pa);
	if (r != W_OK)
		*s2f = 1;
	return r;
}

/*
 * Stage 1 (18 "Paging", 21 section 5 step 3): iova -> gpa (or pa without S2).
 * *s2f = the failure was in stage 2 (of a table access); *fgpa its address.
 */
static int
stage1(struct iommu *io, const struct dctx *dc, uint64_t pc0, uint64_t pc1,
    const struct io_req *r, uint64_t *out, int *s2f, uint64_t *fgpa)
{
	uint64_t va = r->iova;
	int khalf = (va >> 56) & 1;
	if (khalf && (va >> 57) != 0x7F)
		return W_FAULT;
	uint64_t a = ((khalf ? pc1 : pc0) & ((1ull << 40) - 1)) << 12;
	int sum = (pc0 >> 61) & 1;
	for (int lvl = 4; lvl >= 0; lvl--) {
		uint64_t pte, pa, addr = a + (((va >> (12 + 9 * lvl)) & 511) << 3);
		int t = tbl(io, dc, addr, 0, &pa, s2f);
		if (t != W_OK) {
			*fgpa = addr;
			return t;
		}
		if (rd64(io, pa, &pte))
			return W_BUSERR;
		if (!(pte & 1))
			return W_NOTPRESENT;
		unsigned perm = (pte >> 1) & 7;
		if ((pte & (1ull << 11)) || perm == 6 || perm == 7 || ((pte >> 9) & 3) == 3)
			return W_FAULT;
		uint64_t base = pte & 0x000FFFFFFFFFF000ull;
		if (perm == 0) {
			if (lvl == 0 || (pte & 0x00F00000000007D0ull))
				return W_FAULT;
			a = base;
			continue;
		}
		uint64_t low = (1ull << (12 + 9 * lvl)) - 1;
		if (lvl == 4 || (base & low))
			return W_FAULT;
		int rd = perm == 1 || perm == 2 || perm == 4;	/* X gives no data, JIT faults */
		int wr = perm == 2;
		if (r->write ? !wr : !rd)
			return W_FAULT;
		int u = (pte >> 4) & 1;
		if (!r->priv && !u)
			return W_FAULT;
		if (r->priv && u && !sum)
			return W_FAULT;
		if (!(pte & 0x40) || (r->write && !(pte & 0x80))) {
			/* A/D: a stage-1 PTE update needs stage-2 write */
			uint64_t wpa;
			int t2 = tbl(io, dc, addr, 1, &wpa, s2f);
			if (t2 != W_OK) {
				*fgpa = addr;
				return t2;
			}
			pte |= 0x40 | (r->write ? 0x80 : 0);
			if (wr64(io, wpa, pte))
				return W_BUSERR;
		}
		*out = (base & ~low) | (va & low);
		return W_OK;
	}
	return W_FAULT;
}

/* ------------------------------------------------------------------ queues */

static uint64_t
qsize(uint64_t b)
{
	unsigned l = b & 31;
	if (l < 4)
		l = 4;
	if (l > 16)
		l = 16;
	return 1ull << l;
}

static uint64_t
qbase(uint64_t b)
{
	return b & ~0xFFFull;
}

static void
send_msi(struct iommu *io, uint64_t addr, uint64_t data)
{
	if (addr)
		io->msi(io->ctx, addr, data);
}

static void
fault(struct iommu *io, const struct io_req *r, unsigned cause, uint64_t gpa)
{
	io->faults++;
	if (!(io->ctrl & 4))
		return;
	uint64_t n = qsize(io->fqb), t = io->fqt % n;
	if ((t + 1) % n == io->fqh % n) {
		io->status |= 1;
		return;
	}
	uint64_t a = qbase(io->fqb) + t * 32;
	uint64_t d0 = (cause & 0xFFF) | (uint64_t)(r->write != 0) << 12 |
	    (uint64_t)(r->pv != 0) << 13 | (uint64_t)(r->priv != 0) << 14 |
	    (uint64_t)(r->devid & 0xFFFF) << 16 | (uint64_t)(r->pasid & 0xFFFFF) << 32;
	wr64(io, a, d0);
	wr64(io, a + 8, r->iova);
	wr64(io, a + 16, gpa);
	wr64(io, a + 24, 0);
	io->fqt = (t + 1) % n;
	send_msi(io, io->fq_msi_addr, io->fq_msi_data);
}

static void
page_request(struct iommu *io, const struct io_req *r)
{
	io->page_requests++;
	if (!(io->ctrl & 8)) {
		/* no queue: the request is lost; tell the device it failed */
		io->resp(io->ctx, r->devid, r->pasid, r->prgi, 0);
		return;
	}
	uint64_t n = qsize(io->pqb), t = io->pqt % n;
	if ((t + 1) % n == io->pqh % n) {
		io->status |= 2;
		io->resp(io->ctx, r->devid, r->pasid, r->prgi, 0);
		return;
	}
	uint64_t a = qbase(io->pqb) + t * 16;
	uint64_t d0 = (r->prgi & 0x1FF) | (uint64_t)(r->write != 0) << 12 |
	    (uint64_t)(r->pv != 0) << 13 | (uint64_t)(r->priv != 0) << 14 |
	    (uint64_t)(r->devid & 0xFFFF) << 16 | (uint64_t)(r->pasid & 0xFFFFF) << 32;
	wr64(io, a, d0);
	wr64(io, a + 8, r->iova & ~0xFFFull);
	io->pqt = (t + 1) % n;
	send_msi(io, io->pq_msi_addr, io->pq_msi_data);
}

static void
run_commands(struct iommu *io)
{
	if (!(io->ctrl & 2) || (io->status & 4))
		return;
	uint64_t n = qsize(io->cqb);
	while (io->cqh % n != io->cqt % n) {
		uint64_t a = qbase(io->cqb) + (io->cqh % n) * 16, d0, d1;
		if (rd64(io, a, &d0) || rd64(io, a + 8, &d1)) {
			io->status |= 4;
			return;
		}
		io->commands++;
		switch (d0 & 0xFF) {
		case 1: case 2: case 3: case 4:
			break;			/* no IOTLB or directory caches */
		case 5:				/* IOFENCE */
			if (d0 & (1u << 8)) {
				uint8_t b[4];
				uint32_t v = (uint32_t)(d0 >> 32);
				memcpy(b, &v, 4);
				io->mem(io->ctx, 1, d1, 4, b);
			}
			if (d0 & (1u << 9))
				send_msi(io, io->cq_msi_addr, io->cq_msi_data);
			break;
		case 6:				/* PAGE_RESP */
			io->resp(io->ctx, (unsigned)(d0 >> 16) & 0xFFFF,
			    (unsigned)(d0 >> 32) & 0xFFFFF, (unsigned)(d1 & 0x1FF),
			    ((d0 >> 52) & 15) == 0);
			break;
		default:
			io->status |= 4;
			return;
		}
		io->cqh = (io->cqh + 1) % n;
	}
}

int
iommu_reg(struct iommu *io, int we, unsigned off, uint64_t *v)
{
	uint64_t *r = NULL;
	switch (off) {
	case 0x00:
		if (we)
			return 0;
		*v = 1 | 1u << 8 | 1u << 9 | 1u << 10 | 1u << 11 | 1u << 12 |
		    20u << 15 | 16u << 20;
		return 0;
	case 0x08: r = &io->ctrl; break;
	case 0x10: r = &io->ddtp; break;
	case 0x18: r = &io->cqb; break;
	case 0x20:
		if (!we)
			*v = io->cqh;
		return 0;
	case 0x28:
		if (!we) {
			*v = io->cqt;
			return 0;
		}
		io->cqt = *v % qsize(io->cqb);
		run_commands(io);
		return 0;
	case 0x30: r = &io->fqb; break;
	case 0x38: r = &io->fqh; break;
	case 0x40:
		if (!we)
			*v = io->fqt;
		return 0;
	case 0x48: r = &io->pqb; break;
	case 0x50: r = &io->pqh; break;
	case 0x58:
		if (!we)
			*v = io->pqt;
		return 0;
	case 0x60: r = &io->fq_msi_addr; break;
	case 0x68: r = &io->fq_msi_data; break;
	case 0x70: r = &io->pq_msi_addr; break;
	case 0x78: r = &io->pq_msi_data; break;
	case 0x80: r = &io->cq_msi_addr; break;
	case 0x88: r = &io->cq_msi_data; break;
	case 0x90:
		if (we) {
			io->status &= ~*v;
			run_commands(io);
		} else
			*v = io->status;
		return 0;
	default:
		return -1;
	}
	if (!we) {
		*v = *r;
		return 0;
	}
	switch (off) {
	case 0x08:
		*r = *v & 0xE;
		run_commands(io);
		break;
	case 0x10: *r = *v & ~0xFFCull; break;
	case 0x18: case 0x30: case 0x48:
		*r = *v & ~0xFE0ull;
		break;
	default: *r = *v; break;
	}
	return 0;
}

/* ------------------------------------------------------------------ DMA */

static int
is_msi(uint64_t a)
{
	return a >= MSI_LO && a < MSI_HI;
}

static int
deliver_msi(struct iommu *io, uint64_t pa, const uint8_t *data, unsigned n)
{
	uint64_t v = 0;
	memcpy(&v, data, n > 8 ? 8 : n);
	return io->msi(io->ctx, pa, v);
}

int
iommu_dma(struct iommu *io, const struct io_req *r, uint8_t *data, unsigned n)
{
	unsigned mode = io->ddtp & 3;
	uint64_t pa;

	if (mode == 0) {
		if (r->write && is_msi(r->iova))
			return deliver_msi(io, r->iova, data, n) ? IO_FAULT : IO_OK;
		return io->mem(io->ctx, r->write, r->iova, n, data) ? IO_FAULT : IO_OK;
	}
	if (mode != 2) {
		io->faults++;
		return IO_FAULT;		/* blocked: no record */
	}
	/* device context */
	struct dctx dc;
	uint64_t root, bus = (r->devid >> 8) & 0xFF, fn = r->devid & 0xFF;
	if (rd64(io, (io->ddtp & ~0xFFFull) + bus * 8, &root) || !(root & 1)) {
		fault(io, r, 1, 0);
		return IO_FAULT;
	}
	for (int i = 0; i < 8; i++)
		if (rd64(io, (root & ~0xFFFull) + fn * 64 + i * 8, &dc.d[i])) {
			fault(io, r, 1, 0);
			return IO_FAULT;
		}
	if (!(dc.d[0] & DC_V)) {
		fault(io, r, 1, 0);
		return IO_FAULT;
	}
	/* MSI remap: a write to the context's MSI page */
	if (r->write && (dc.d[4] & 1) && (r->iova & ~0xFFFull) == (dc.d[4] & ~0xFFFull)) {
		if (deliver_msi(io, (dc.d[5] & ~0xFFFull) | (r->iova & 0xFFF), data, n)) {
			fault(io, r, 10, 0);
			return IO_FAULT;
		}
		return IO_OK;
	}
	int s2f = 0, w;
	uint64_t fgpa = 0, gpa = r->iova;
	int pri = (dc.d[0] & DC_PRI) && r->pri_ok;
	if (dc.d[0] & DC_S1) {
		if (r->pv && !(dc.d[0] & DC_PASID)) {
			fault(io, r, 2, 0);
			return IO_FAULT;
		}
		unsigned pasid = r->pv ? r->pasid : 0;
		/* process directory: root by PASID[19:9], leaf by PASID[8:0] */
		uint64_t pd, pdpa, pc0, pc1, lpa;
		int dummy = 0;
		if (!(dc.d[2] & 1) ||
		    tbl(io, &dc, (dc.d[2] & ~0xFFFull) + ((pasid >> 9) & 0x7FF) * 8, 0, &pdpa, &dummy) ||
		    rd64(io, pdpa, &pd) || !(pd & 1) ||
		    tbl(io, &dc, (pd & ~0xFFFull) + (pasid & 511) * 16, 0, &lpa, &dummy) ||
		    rd64(io, lpa, &pc0) || rd64(io, lpa + 8, &pc1) || !((pc0 >> 60) & 1)) {
			fault(io, r, 2, 0);
			return IO_FAULT;
		}
		w = stage1(io, &dc, pc0, pc1, r, &gpa, &s2f, &fgpa);
		if (w != W_OK) {
			if (w == W_NOTPRESENT && pri) {
				page_request(io, r);
				return IO_PRI;
			}
			if (w == W_BUSERR)
				fault(io, r, 7, 0);
			else if (s2f)
				fault(io, r, r->write ? 6 : 5, fgpa);
			else
				fault(io, r, r->write ? 4 : 3, 0);
			return IO_FAULT;
		}
	}
	if (dc.d[0] & DC_S2) {
		w = stage2(io, &dc, gpa, r->write, &pa);
		if (w != W_OK) {
			if (w == W_NOTPRESENT && pri) {
				page_request(io, r);
				return IO_PRI;
			}
			fault(io, r, w == W_BUSERR ? 7 : (r->write ? 6 : 5), w == W_BUSERR ? 0 : gpa);
			return IO_FAULT;
		}
	} else
		pa = gpa;
	if (is_msi(pa)) {
		if (!r->write || !(dc.d[0] & DC_MSIP) || deliver_msi(io, pa, data, n)) {
			fault(io, r, 10, 0);
			return IO_FAULT;
		}
		return IO_OK;
	}
	if (io->mem(io->ctx, r->write, pa, n, data)) {
		fault(io, r, 7, 0);
		return IO_FAULT;
	}
	return IO_OK;
}
