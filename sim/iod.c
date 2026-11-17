/*
 * vsim: the I/O die on the far side of the die's I/O link; iod.h explains
 * the model. Packets (docs/die-interface.md): a header flit ([7:0] opcode,
 * [15:8] tag, [18:16] log2 size, [22:19] data flits), an address flit and
 * data flits (the transfer's bytes from the address up, 8 per flit).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <ucontext.h>

#include "board.h"
#include "iod.h"

enum { OP_MRD = 1, OP_MWR = 2, OP_IORD = 3, OP_IOWR = 4, OP_CPL = 8, OP_CPLE = 9 };

#define TXQ	4096		/* flits waiting for the die */

/* VSIM_LINK_TRACE=1: every packet on stderr */
static int ltrace = -1;
#define LT(...) do { if (ltrace < 0) ltrace = getenv("VSIM_LINK_TRACE") != NULL; \
	if (ltrace) fprintf(stderr, "link: " __VA_ARGS__); } while (0)
#define STACK	(512 * 1024)

struct iod {
	struct board *b;
	/* die -> iod: the packet being assembled */
	uint64_t pk[10];
	unsigned pkn, pkneed;
	/* iod -> die: flits, the head offered on rx */
	uint64_t q[TXQ];
	unsigned qh, qn;
	int offered;
	/* the board's coroutine: created once with makecontext/swapcontext,
	   then switched with _setjmp/_longjmp, which don't save the signal mask
	   (swapcontext's sigprocmask call per switch cost a syscall every
	   cycle) -- the usual coroutine technique (R. S. Engelschall, "Portable
	   Multithreading", USENIX ATC 2000) */
	ucontext_t main_ctx, dev_ctx;
	jmp_buf main_jb, dev_jb;
	void *stack;
	int in_dev;		/* running in the coroutine */
	int started;
	int wait;		/* a DMA read is waiting for its completion */
	uint8_t tag;		/* the waiting read's tag */
	uint8_t data[64];
	int err;
	/* statistics */
	uint64_t requests, dma_reads, dma_writes, dma_errors, bad_packets;
};

static uint64_t
hdr(unsigned op, unsigned tag, unsigned size, unsigned nflits)
{
	return op | (uint64_t)tag << 8 | (uint64_t)size << 16 | (uint64_t)nflits << 19;
}

static int
push(struct iod *d, uint64_t f)
{
	LT("iod -> die flit %016llx\n", (unsigned long long)f);
	if (d->qn == TXQ) {
		fprintf(stderr, "vsim: I/O link: the die isn't taking flits (queue full)\n");
		return -1;
	}
	d->q[(d->qh + d->qn) % TXQ] = f;
	d->qn++;
	return 0;
}

static unsigned
nflits_of(unsigned size)
{
	return (1u << size) <= 8 ? 1 : (1u << size) / 8;
}

/* a request from the die: answer it from the board */
static void
serve(struct iod *d)
{
	unsigned op = d->pk[0] & 0xFF, tag = (d->pk[0] >> 8) & 0xFF;
	unsigned size = (d->pk[0] >> 16) & 7;
	uint64_t addr = d->pk[1];
	uint8_t buf[64] = {};
	unsigned n = 1u << size, lat = 1;
	int r;

	d->requests++;
	for (unsigned i = 0; i + 2 < d->pkn && i < 8; i++)
		memcpy(buf + 8 * i, &d->pk[2 + i], 8);		/* little-endian host */
	switch (op) {
	case OP_MRD:
	case OP_MWR:
		r = board_access(d->b, op == OP_MWR, addr, size, buf, &lat);
		if (op == OP_MWR)
			return;					/* posted */
		break;
	case OP_IORD:
	case OP_IOWR:
		r = board_io(d->b, op == OP_IOWR, (uint16_t)addr, size, buf);
		break;
	default:
		d->bad_packets++;
		return;
	}
	if (r) {
		push(d, hdr(OP_CPLE, tag, size, 0));
		return;
	}
	if (op == OP_IOWR) {
		push(d, hdr(OP_CPL, tag, size, 0));
		return;
	}
	unsigned nf = nflits_of(size);
	push(d, hdr(OP_CPL, tag, size, nf));
	for (unsigned i = 0; i < nf; i++) {
		uint64_t f = 0;
		memcpy(&f, buf + 8 * i, n < 8 ? n : 8);
		push(d, f);
	}
}

/* a completion for our DMA read: wake the device */
static void
completion(struct iod *d)
{
	unsigned op = d->pk[0] & 0xFF, tag = (d->pk[0] >> 8) & 0xFF;
	if (!d->wait || tag != d->tag) {
		d->bad_packets++;
		return;
	}
	d->err = op == OP_CPLE;
	for (unsigned i = 0; i + 1 < d->pkn && i < 8; i++)
		memcpy(d->data + 8 * i, &d->pk[1 + i], 8);
	d->wait = 0;
}

void
iod_cycle(struct iod *d, int die_tx_valid, uint64_t die_tx_flit, int die_rx_ready,
    int *die_tx_ready, int *die_rx_valid, uint64_t *die_rx_flit)
{
	/* the flit we offered moved at the last edge if the die was ready */
	if (d->offered && die_rx_ready) {
		d->qh = (d->qh + 1) % TXQ;
		d->qn--;
	}
	/* we always take the die's flit */
	*die_tx_ready = 1;
	if (die_tx_valid) {
		if (d->pkn == 0) {
			unsigned op = die_tx_flit & 0xFF;
			unsigned nf = (die_tx_flit >> 19) & 0xF;
			/* requests carry an address flit; completions don't */
			d->pkneed = (op == OP_CPL || op == OP_CPLE) ? 1 + nf : 2 + nf;
		}
		d->pk[d->pkn++] = die_tx_flit;
		if (d->pkn == d->pkneed) {
			unsigned op = d->pk[0] & 0xFF;
			LT("die -> iod op %02x tag %02llx size %llu addr %llx data %llx\n", op,
			    (unsigned long long)(d->pk[0] >> 8 & 0xFF), (unsigned long long)(d->pk[0] >> 16 & 7),
			    (unsigned long long)(d->pkn > 1 ? d->pk[1] : 0),
			    (unsigned long long)(d->pkn > 2 ? d->pk[2] : 0));
			if (op == OP_CPL || op == OP_CPLE)
				completion(d);
			else
				serve(d);
			d->pkn = 0;
		}
	}
	d->offered = d->qn != 0;
	*die_rx_valid = d->offered;
	*die_rx_flit = d->offered ? d->q[d->qh] : 0;
}

/* ---------------------------------------------------------------- DMA */

int
iod_dma(void *ctx, int we, uint64_t pa, unsigned n, uint8_t *data)
{
	struct iod *d = ctx;
	if (!d->in_dev) {
		/* a device model DMAing outside its tick (from a register write):
		   it must defer the transfer to its tick, as fw_cfg does */
		fprintf(stderr, "vsim: DMA outside a device tick (pa %llx): a device model bug\n",
		    (unsigned long long)pa);
		return -1;
	}
	while (n) {
		/* naturally aligned power-of-two pieces inside a 64-byte line */
		unsigned size = 6;
		while ((1u << size) > n || (pa & ((1u << size) - 1)))
			size--;
		unsigned c = 1u << size, nf = nflits_of(size);
		uint8_t tag = (uint8_t)(0x80 | (d->tag + 1) % 0x80);
		d->tag = tag;
		if (we) {
			push(d, hdr(OP_MWR, tag, size, nf));
			push(d, pa);
			for (unsigned i = 0; i < nf; i++) {
				uint64_t f = 0;
				memcpy(&f, data + 8 * i, c < 8 ? c : 8);
				push(d, f);
			}
			d->dma_writes++;
		} else {
			push(d, hdr(OP_MRD, tag, size, 0));
			push(d, pa);
			d->wait = 1;
			d->dma_reads++;
			if (!_setjmp(d->dev_jb))		/* until the completion */
				_longjmp(d->main_jb, 1);
			if (d->err) {
				d->dma_errors++;
				return -1;
			}
			memcpy(data, d->data, c);
		}
		pa += c;
		data += c;
		n -= c;
	}
	return 0;
}

static void
dev_main(void)
{
	extern struct iod *iod_current;
	struct iod *d = iod_current;
	/* the first switch came by swapcontext; every later one by _longjmp */
	if (!_setjmp(d->dev_jb))
		swapcontext(&d->dev_ctx, &d->main_ctx);
	for (;;) {
		board_tick(d->b);
		if (!_setjmp(d->dev_jb))
			_longjmp(d->main_jb, 1);
	}
}

struct iod *iod_current;

void
iod_board_tick(struct iod *d)
{
	if (d->wait)
		return;			/* a device waits for DRAM: board time stops */
	if (!d->started) {
		getcontext(&d->dev_ctx);
		d->dev_ctx.uc_stack.ss_sp = d->stack;
		d->dev_ctx.uc_stack.ss_size = STACK;
		d->dev_ctx.uc_link = NULL;
		iod_current = d;
		makecontext(&d->dev_ctx, dev_main, 0);
		swapcontext(&d->main_ctx, &d->dev_ctx);	/* runs to its first _setjmp */
		d->started = 1;
	}
	d->in_dev = 1;
	if (!_setjmp(d->main_jb))
		_longjmp(d->dev_jb, 1);
	d->in_dev = 0;
}

struct iod *
iod_new(struct board *b)
{
	struct iod *d = calloc(1, sizeof *d);
	if (!d || !(d->stack = malloc(STACK))) {
		free(d);
		return NULL;
	}
	d->b = b;
	return d;
}

void
iod_free(struct iod *d)
{
	if (d) {
		free(d->stack);
		free(d);
	}
}

void
iod_reset(struct iod *d)
{
	d->pkn = 0;
	d->qh = d->qn = 0;
	d->offered = 0;
	/* a device waiting on a read gets an error; the coroutine keeps its
	   stack (the board resets its devices itself) */
	if (d->wait) {
		d->err = 1;
		d->wait = 0;
	}
}

void
iod_stats(const struct iod *d, char *buf, unsigned len)
{
	snprintf(buf, len, "I/O link: %llu requests, DMA %llu reads %llu writes %llu errors, %llu bad packets",
	    (unsigned long long)d->requests, (unsigned long long)d->dma_reads,
	    (unsigned long long)d->dma_writes, (unsigned long long)d->dma_errors,
	    (unsigned long long)d->bad_packets);
}
