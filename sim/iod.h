/*
 * vsim: the I/O die at the far end of the die's I/O link (docs/die-interface.md,
 * D-014). It answers the die's requests (MRD/MWR to MMIO and the flash,
 * IORD/IOWR to ports) from the board model, and carries the board's device
 * DMA into the die as MRD/MWR requests (the die's fabric serves them from
 * DRAM), returning read data from the die's completions.
 *
 * The board's devices are written as if DMA were a synchronous call; they
 * run here in a coroutine (POSIX makecontext/swapcontext, as FreeBSD's libc
 * provides) that a DMA read suspends until its completion arrives over the
 * link, so device time stops while it waits, as a device stalled on a
 * memory read would. The die's own requests are served outside the
 * coroutine, so they never wait for a device (the core may be waiting on an
 * MMIO completion while a device waits for DRAM).
 */
#ifndef IOD_H
#define IOD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct board;
struct iod;

struct iod *iod_new(struct board *b);
void iod_free(struct iod *d);
/* link reset (the die's RESET_L): queues and the coroutine start over */
void iod_reset(struct iod *d);

/*
 * One cycle, between the die's evaluation with the clock low and its rising
 * edge. In: the die's outputs (tx_valid/tx_flit; rx_ready for the flit we
 * offered). Out: tx_ready (we take the die's flit at the edge) and the flit
 * we offer (rx_valid/rx_flit) for the next edge; the handshakes that the
 * edge completes are applied by the next call.
 */
void iod_cycle(struct iod *d, int die_tx_valid, uint64_t die_tx_flit, int die_rx_ready,
    int *die_tx_ready, int *die_rx_valid, uint64_t *die_rx_flit);

/* the board's devices for one cycle (board_tick in the coroutine; skipped
   while a device waits for a DMA read) */
void iod_board_tick(struct iod *d);

/* DMA from a board device (the board's dma hook): 0, or -1 if the die
   answered with an error (no DRAM there) */
int iod_dma(void *ctx, int we, uint64_t pa, unsigned n, uint8_t *data);

/* a posted 4-byte write into the die (an MSI: Intel SDM Vol. 3A 12.11),
   from any context */
void iod_msi(struct iod *d, uint64_t addr, uint32_t data);

/* the chipset's interrupt wires and machine checks as link messages
   (D-019, docs/die-interface.md): iod_intr every cycle with the 8259's
   INTR level and vector (a VW message goes out when it changes, and after
   each INTA the die sends back, which also acks the 8259); NMI, SMI and
   INIT are edges; iod_mce reports a machine check (type 1 corrected,
   2 uncorrected) */
enum { VW_INTR = 0, VW_NMI = 1, VW_SMI = 2, VW_INIT = 3 };
void iod_intr(struct iod *d, int pending, uint8_t vector);
void iod_vw(struct iod *d, unsigned wire, int asserted, uint8_t vector);
void iod_mce(struct iod *d, unsigned type, uint64_t addr);

/* statistics: requests served, DMA packets, errors */
void iod_stats(const struct iod *d, char *buf, unsigned len);

#ifdef __cplusplus
}
#endif

#endif
