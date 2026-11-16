/* vsim PCIe root complex, the IOMMU in front of it, and the DMA test
 * endpoint (docs/platform.md "PCIe") */
#ifndef PCIE_H
#define PCIE_H

#include <stdint.h>

#include "iommu.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ECAM_BASE	0xE0000000ull	/* 256 buses x 1 MiB */
#define ECAM_SIZE	0x10000000ull
#define PCI_MEM32_BASE	0xC0000000ull	/* 32-bit BAR window */
#define PCI_MEM32_SIZE	0x20000000ull
#define PCI_MEM64_BASE	0x20000000000ull	/* 64-bit BAR window */
#define PCI_MEM64_SIZE	(64ull << 30)
#define PCI_SLOTS	8
#define PCI_VENDOR	0x7663		/* placeholder, not PCI-SIG assigned */
#define PCI_DEV_HOST	0x0001
#define PCI_DEV_RP	0x0002
#define PCI_DEV_DMAT	0x0D0A

struct pcifn {
	uint8_t cfg[4096];
	uint8_t wm[4096];	/* writable bits */
};

struct pcie;

/* an endpoint in a slot */
struct pcidev {
	struct pcifn f;
	int (*bar)(struct pcidev *d, int we, unsigned bar, uint64_t off, unsigned n,
	    uint8_t *data);
	void (*tick)(struct pcidev *d);
	void (*reset)(struct pcidev *d);
	void (*resp)(struct pcidev *d, unsigned prgi, int ok);	/* a PAGE_RESP */
	struct pcie *pc;
	int slot;
	uint64_t bar_size;	/* BAR0's size (0: 4 KiB) */
};

struct pcie {
	struct pcifn host;
	struct pcifn rp[PCI_SLOTS];
	struct pcidev *slot[PCI_SLOTS];
	struct iommu iommu;
	/* the board: physical memory and interrupt-file pages */
	int (*mem)(void *ctx, int we, uint64_t pa, unsigned n, uint8_t *data);
	int (*msi)(void *ctx, uint64_t pa, uint64_t value);
	void *ctx;
	uint64_t dma_bytes, dma_errors;
};

/* dma_slot: where the DMA test endpoint goes (-1: none) */
struct pcie *pcie_new(int dma_slot, int (*mem)(void *, int, uint64_t, unsigned, uint8_t *),
    int (*msi)(void *, uint64_t, uint64_t), void *ctx);
void pcie_free(struct pcie *pc);
/* put another endpoint (e.g. xhci_new()) in an empty slot; the root complex
 * owns it from then on (freed with free()). Returns 0, or -1 if taken */
int pcie_attach(struct pcie *pc, int slot, struct pcidev *d);
void pcie_reset(struct pcie *pc);
void pcie_tick(struct pcie *pc);
/* an access from the core in ECAM, a BAR window or the IOMMU registers;
 * returns 1 if the address isn't PCIe's, else 0 / -1 (bus error) */
int pcie_access(struct pcie *pc, int we, uint64_t addr, unsigned size, uint8_t *data);

/* for endpoints: DMA (any length; split at 4 KiB pages). A request through
 * the IOMMU; returns IO_OK, IO_FAULT or IO_PRI (the device must wait). */
int pcie_dma(struct pcidev *d, int we, uint64_t iova, uint8_t *data, unsigned n,
    int pv, unsigned pasid, int priv, unsigned prgi);
/* send the endpoint's MSI if enabled; returns 0 if sent */
int pcie_msi(struct pcidev *d);

#ifdef __cplusplus
}
#endif

#endif
