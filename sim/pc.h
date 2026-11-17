/*
 * vsim: the PC-compatible devices of the board's platform controller (what
 * a PCH/southbridge provides): 8254 PIT, two 8259 PICs, the MC146818 RTC and
 * CMOS, ports 61h and 92h, the ACPI PM registers, QEMU's fw_cfg interface
 * and the debug console. docs/platform.md "Legacy PC devices".
 */
#ifndef PC_H
#define PC_H
#include "vibe86_platform.h"	/* rtl/common/platform.toml */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* the core clock vsim's timers count in (one board_tick per core cycle) */
#define CORE_HZ		PLAT_CLOCK_CORE_HZ
#define PIT_HZ		PLAT_CLOCK_PIT_HZ
#define PMTMR_HZ	PLAT_CLOCK_PMTMR_HZ

#define PORT_PIT	PLAT_PORTS_PIT
#define PORT_PIC1	PLAT_PORTS_PIC1
#define PORT_PIC2	PLAT_PORTS_PIC2
#define PORT_ELCR	PLAT_PORTS_ELCR
#define PORT_RTC	PLAT_PORTS_RTC
#define PORT_SYSB	PLAT_PORTS_SYSB		/* NMI status and control */
#define PORT_A20	PLAT_PORTS_A20		/* system control port A (fast A20, INIT) */
#define PORT_DEBUGCON	PLAT_PORTS_DEBUGCON		/* QEMU-style debug console */
#define PORT_FWCFG	PLAT_PORTS_FWCFG		/* selector (16-bit), data 0x511 */
#define PORT_FWCFG_DMA	PLAT_PORTS_FWCFG_DMA		/* DMA address, big-endian, 0x514..0x51B */
#define PORT_PM		PLAT_PORTS_PM1		/* ACPI PM1a event (0x600), control (0x604), timer (0x608) */

struct pit_ch {
	uint16_t reload, count, latch;
	uint8_t mode, rw, access_lo, latched, latch_lo, null, out, gate;
	uint8_t wr_lo, wr_hi_next;
	uint8_t status, status_latched;
};

struct pic8259 {
	uint8_t irr, isr, imr, base, icw, icw4, special, rr, autoeoi, init, elcr;
	uint8_t level;			/* input lines (edge detection) */
};

#define FWCFG_MAXFILES	32
struct fwcfg_file {
	char name[56];
	uint16_t sel;
	uint8_t *data;
	uint32_t size;
};

struct pc {
	struct pit_ch pit[3];
	uint64_t ram_high;		/* DRAM above 4 GiB (CMOS 5Bh-5Dh) */
	/* every ISA IRQ line also goes here (the board wires them to the I/O
	   APIC, as a PC wires them to both interrupt controllers) */
	void (*irq_hook)(void *ctx, unsigned irq, int level);
	void *irq_ctx;
	int fw_dma_pending;		/* a fw_cfg DMA request to run at the next tick */
	uint64_t pit_acc;		/* fractional PIT clocks (CORE_HZ units) */
	struct pic8259 pic[2];
	uint8_t cmos[128], cmos_idx, nmi_mask;
	uint64_t rtc_acc, rtc_div;	/* RTC periodic and seconds */
	uint8_t sysb;			/* port 61h */
	uint8_t a20;			/* port 92h */
	/* ACPI PM */
	uint16_t pm1_sts, pm1_en, pm1_cnt;
	uint64_t pmtmr_acc;
	uint32_t pmtmr;
	/* fw_cfg */
	uint16_t fw_sel;
	uint32_t fw_off;
	uint64_t fw_dma;
	struct fwcfg_file files[FWCFG_MAXFILES];
	unsigned nfiles;
	uint8_t *dir;			/* FW_CFG_FILE_DIR */
	uint32_t dirsize;
	uint64_t ram_size;		/* below the PCI hole */
	unsigned ncpus;
	/* the board's memory, for fw_cfg DMA */
	int (*mem)(void *ctx, int we, uint64_t pa, unsigned n, uint8_t *data);
	void *memctx;
	void (*debugcon)(void *ctx, uint8_t c);
	void *dbgctx;
	int power_off;			/* set by PM1a_CNT (S5) */
};

void pc_init(struct pc *p, uint64_t ram_size, unsigned ncpus);
void pc_reset(struct pc *p);
/* one core clock */
void pc_tick(struct pc *p);
/* an I/O access the PC devices decode: 1 handled, 0 not theirs */
int pc_io(struct pc *p, int we, uint16_t port, unsigned n, uint8_t *data);
/* add a fw_cfg file (copied) */
int pc_fwcfg_add(struct pc *p, const char *name, const void *data, uint32_t size);
/* an ISA IRQ line (0-15) */
void pc_irq(struct pc *p, unsigned irq, int level);
/* the PIC pair's interrupt request, and INTA: the vector it supplies */
int pc_pic_intr(const struct pc *p);
uint8_t pc_pic_inta(struct pc *p);

void pc_ram_high(struct pc *p, uint64_t bytes);

#ifdef __cplusplus
}
#endif

#endif
