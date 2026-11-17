/* vsim board model: everything outside the core (docs/platform.md) */
#ifndef BOARD_H
#define BOARD_H
#include "vibe86_platform.h"	/* rtl/common/platform.toml */

#include <stddef.h>
#include <stdint.h>

#include "dram.h"
#include "pcie.h"
#include "i2c.h"
#include "pmc.h"
#include "tpm.h"
#include "vga.h"
#include "xhci.h"
#include "pc.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Physical map (docs/platform.md). The EEPROM (SPI flash) sits at the top
 * of the 4 GiB space so that the x86 reset vector, 0xFFFF_FFF0, is its last
 * 16 bytes; board and uncore registers are in the MMIO block below the MSI
 * window, as on PC-compatible platforms.
 */
#define SRAM_BASE	0x00000000ull	/* test board only */
#define SRAM_SIZE	(2ull << 20)	/* the default; vsim -M sets board_ram_size */
#define MMIO_BASE	PLAT_MAP_MMIO_BASE	/* board / uncore register block */
#define UART0_BASE	0xFE000000ull	/* MMIO alias of COM1 */
#define SYSCON_BASE	0xFE001000ull
#define STRAPS_BASE	0xFE002000ull
#define QPORT_BASE	0xFE005000ull	/* enqueue test portal page */
#define QPORT_CTL	0xFE006000ull	/* its control registers */
#define QPORT_DEPTH	4
#define NVRAM_BASE	0xFE00A000ull
#define NVRAM_SIZE	4096
#define MSI_BASE	PLAT_MAP_MSI_BASE	/* MSI window: APIC id addr[19:12], vector data[7:0] */
#define MSI_SIZE	PLAT_MAP_MSI_SIZE
#define EEPROM_SIZE	PLAT_MAP_FLASH_SIZE
#define EEPROM_BASE	PLAT_MAP_FLASH_BASE	/* 0xFF00_0000 */
#define RESET_VECTOR	PLAT_MAP_RESET_VECTOR
#define SYSCON_NMI	0xAAAAu
#define SYSCON_MCE	0xBB00u		/* | type */
/* server board only: SMB_BASE, MC_BASE and the DRAM window (dram.h) */

/* I/O ports (IN/OUT) */
#define PORT_POST	PLAT_PORTS_POST		/* POST code */
#define PORT_COM1	PLAT_PORTS_COM1		/* 16550, 8 registers */
#define PORT_RST	PLAT_PORTS_RESET		/* reset control: bit 2 resets */
#define PORT_PWR	0x604		/* power control: 2000h off, 3333h|code<<16 fail */

#define STRAP_ECC_SKIP	0x1u	/* jumper: skip the cold-boot ECC check */

enum { PWR_ON, PWR_OFF, PWR_FAIL, PWR_RESET, PWR_THERMAL };

/*
 * BOARD_SERVER: the server board (docs/platform.md): EEPROM, COM1, syscon,
 *   straps, the SPD SMBus, the DDR5 memory controller and 16 DIMM slots.
 *   VGA, the I2C controller (TPM, fan monitor, LM75), the PMC and PCIe (with
 *   the xHCI USB controller) are on both boards.
 * BOARD_TEST: a small synthetic board for the tests: the same devices plus
 *   2 MiB of SRAM at SRAM_BASE (no DRAM, so no firmware is needed).
 */
enum { BOARD_SERVER, BOARD_TEST };

struct uart16550 {
	uint8_t ier, lcr, mcr, scr, dll, dlm, fcr;
	uint8_t rx[256];
	unsigned rxh, rxt;
	void (*tx)(void *ctx, uint8_t c);
	void *txctx;
};

/* QPORT: a 4-deep enqueue test portal (docs/platform.md) */
struct qport {
	uint8_t desc[QPORT_DEPTH][64];
	uint32_t pasid[QPORT_DEPTH];
	unsigned head, count;
	uint64_t accepted, retried;
	uint64_t msi_addr, msi_data;	/* completion MSI, sent on each POP */
};

/* events the board sends to the core, one per cycle (vsim drains them) */
enum { EV_MSI = 1, EV_NMI, EV_MCE };
struct board_event {
	int kind;
	unsigned core;		/* EV_MSI: destination APIC id */
	unsigned vector;	/* EV_MSI */
	unsigned type;		/* EV_MCE: 1 corrected, 2 uncorrected */
	uint64_t addr;		/* EV_MCE */
};
#define BOARD_EVQ	64

struct board {
	uint8_t *eeprom;	/* EEPROM_SIZE bytes, erased = 0xFF */
	int kind;		/* BOARD_* */
	uint8_t *sram;		/* BOARD_TEST: ram_size bytes of RAM at 0 */
	uint64_t ram_size;
	struct pc pc;		/* the legacy PC devices (pc.h) */
	uint32_t cf8;		/* PCI configuration address (CF8h) */
	void (*debugcon)(void *ctx, uint8_t c);
	void *dbgctx;
	uint32_t straps;
	struct uart16550 uart;
	int power;		/* PWR_* */
	unsigned failcode;	/* PWR_FAIL: code written to syscon */
	struct dram *dram;	/* BOARD_SERVER */
	uint64_t now;		/* cycles since power-on (board_tick) */
	struct qport qport;
	uint64_t syscon_mce_addr;	/* SYSCON + 8 */
	struct board_event evq[BOARD_EVQ];
	unsigned evh, evn;
	uint64_t ev_dropped;
	struct pcie *pcie;	/* PCIe root complex + IOMMU (both boards) */
	struct pcidev *xhci;	/* the USB controller in a PCIe slot, or NULL */
	uint64_t accesses;	/* bus accesses (the PMC's activity input) */
	struct vga vga;
	struct pmc pmc;
	struct i2c i2c;
	uint8_t nvram[NVRAM_SIZE];	/* battery-backed setup storage */
	const char *nvram_file;	/* -N: kept here across runs */
	int nvram_dirty;
	uint8_t post;		/* last POST code (port 0x80) */
	unsigned nposts;	/* POST codes written */
	void (*post_cb)(void *ctx, uint8_t code);	/* -t: report each one */
	void *post_ctx;
};

/*
 * dimms: the slot population of the server board (NULL: the default).
 * eeprom_image: NULL or "" for an erased EEPROM (vsim -e/-l fill memory).
 */
int board_init(struct board *b, int kind, const char *eeprom_image,
    const struct dimm_cfg *dimms);
void board_free(struct board *b);
/* -N: load the NVRAM from path (missing = erased) and save it back there */
int board_nvram_file(struct board *b, const char *path);
void board_nvram_flush(struct board *b);
/* the board's reset line (SYSCON / 0xCF9 reset): devices reset, memory stays */
void board_reset(struct board *b);
/* one core clock */
void board_tick(struct board *b);
/*
 * One memory access; size = log2 bytes (0..6); returns 0, or -1 = bus error.
 * *lat = core cycles until the access is acknowledged: 1 = this cycle (every
 * device except DRAM), more for DRAM (the DDR5 timing model).
 */
int board_access(struct board *b, int we, uint64_t addr, unsigned size,
    uint8_t *data, unsigned *lat);
/*
 * One I/O-space access (IN/OUT); size = log2 bytes (0..2). Unclaimed ports
 * read as all ones and ignore writes, as on a PC; returns 0, or -1 for a
 * bad size.
 */
int board_io(struct board *b, int we, uint16_t port, unsigned size, uint8_t *data);
/* the test board's RAM size (set before board_init; default SRAM_SIZE) */
extern uint64_t board_ram_size;
/* a maskable interrupt the board's PIC is requesting (1, and its vector),
   and the core's acknowledgement of it (INTA) */
int board_pic_pending(struct board *b, uint8_t *vector);
void board_pic_ack(struct board *b);
/*
 * An enq: one atomic 64-byte write tagged with a PASID. Returns 0 with
 * *retry = 0 (accepted) or 1 (queue full), or -1 = bus error (not a portal).
 */
int board_enq(struct board *b, uint64_t addr, const uint8_t data[64],
    uint32_t pasid, int *retry);
/* put the DMA test endpoint and the xHCI controller in PCIe slots 0..7, or
 * -1 = none (default 0 and 1); they must differ */
int board_pci_slots(struct board *b, int dma_slot, int xhci_slot);
/* send an MSI (a write to the MSI window); -1 if addr isn't in it */
int board_msi(void *b, uint64_t addr, uint64_t data);
/* the PMC has suspended the board: the core isn't clocked */
int board_suspended(const struct board *b);
/* the next event for the core; returns 0 if none */
int board_event(struct board *b, struct board_event *e);
/*
 * Copy an image into board memory before reset (vsim -l, -e): the test
 * board's SRAM or the EEPROM range. Returns 0, or -1 with a message on
 * stderr (e.g. the server board's DRAM, which isn't decoded until firmware
 * brings the memory controller online).
 */
int board_preload(struct board *b, uint64_t addr, const uint8_t *data, size_t n);
void uart_rx_push(struct uart16550 *u, uint8_t c);

#ifdef __cplusplus
}
#endif

#endif
