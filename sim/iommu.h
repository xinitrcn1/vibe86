/* vsim IOMMU model (vercpu docs/isa/21-iommu.md; docs/platform.md "PCIe") */
#ifndef IOMMU_H
#define IOMMU_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IOMMU_BASE	0xFE007000ull
#define IOMMU_SIZE	0x1000ull

/* a DMA request (21 section 1) */
struct io_req {
	unsigned devid;		/* requester ID */
	int pv;			/* has a PASID */
	unsigned pasid;
	int priv;		/* privileged (supervisor) request */
	int write;
	int pri_ok;		/* the device has page requests enabled */
	unsigned prgi;		/* the device's request-group index */
	uint64_t iova;
};

enum { IO_OK, IO_FAULT, IO_PRI };

struct iommu {
	uint64_t ctrl, ddtp, cqb, cqh, cqt, fqb, fqh, fqt, pqb, pqh, pqt;
	uint64_t fq_msi_addr, fq_msi_data, pq_msi_addr, pq_msi_data;
	uint64_t cq_msi_addr, cq_msi_data, status;
	/* the platform: memory (physical), interrupt files, page responses */
	int (*mem)(void *ctx, int we, uint64_t pa, unsigned n, uint8_t *data);
	int (*msi)(void *ctx, uint64_t pa, uint64_t value);	/* -1: not a file page */
	void (*resp)(void *ctx, unsigned devid, unsigned pasid, unsigned prgi, int ok);
	void *ctx;
	uint64_t faults, page_requests, commands;
};

void iommu_reset(struct iommu *io);
/* a register access (8 bytes); returns 0 or -1 */
int iommu_reg(struct iommu *io, int we, unsigned off, uint64_t *v);
/*
 * One DMA access of n bytes that doesn't cross a 4 KiB page: translate it and
 * read or write memory. A write that is an MSI is delivered instead.
 * Returns IO_OK, IO_FAULT (recorded, the device gets an error) or IO_PRI (a
 * page request was posted; the device waits for PAGE_RESP).
 */
int iommu_dma(struct iommu *io, const struct io_req *r, uint8_t *data, unsigned n);

#ifdef __cplusplus
}
#endif

#endif
