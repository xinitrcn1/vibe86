/*
 * vsim's view of the processor in the socket: the core-side pins of a
 * Verilated model (rtl/<version>), the same for every core version. vsim -C
 * picks the version (cpu_new). An adapter (cpu_v1.cpp, ...) copies the
 * inputs into its model, evaluates it and copies the outputs back.
 *
 * The package boundary (docs/platform.md, "The socket"): the DDR5 channels,
 * the SPD SMBus, the PCIe root complex and the IOMMU are the package's
 * uncore and are modelled in C (dram.c, pcie.c, iommu.c); the core reaches
 * them, and everything on the board, over the one bus below. Interrupts come
 * back as MSIs (vector), NMI and machine-check pins.
 */
#ifndef CPU_H
#define CPU_H

#include <cstdint>
#include <memory>

class VerilatedContext;

struct cpu {
	static constexpr unsigned MAXCORES = 8;

	/* inputs */
	bool clk = false, rst_n = false;
	/* the core-uncore bus: one request at a time, held until acknowledged */
	bool bus_ack = false, bus_err = false;
	uint32_t bus_rdata[16] = {};	/* 512 bits: a line (v1 uses 128) */
	/* uncore events, one-cycle pulses, and the core they go to */
	bool intr_valid = false;	/* a maskable interrupt request (level): vector */
	unsigned intr_core = 0, intr_vector = 0;
	bool nmi = false;
	unsigned nmi_core = 0;
	bool mce_valid = false;
	unsigned mce_core = 0, mce_type = 0;
	uint64_t mce_addr = 0;

	/* outputs */
	bool bus_req = false, bus_we = false;
	bool bus_io = false;		/* I/O space (IN/OUT): bus_addr is the port */
	bool intr_ack[MAXCORES] = {};	/* the core took the maskable interrupt (INTA) */
	uint64_t bus_addr = 0;
	unsigned bus_size = 0;		/* log2 bytes: 0..4, 6 for a line */
	uint32_t bus_wdata[16] = {};
	/* per core: an exception or interrupt delivered, an instruction retired */
	struct event {
		bool valid, has_error;
		unsigned vector;
		uint32_t error;
		uint64_t rip;		/* of the faulting/interrupted instruction */
	} ev[MAXCORES] = {};
	struct retire {
		bool valid;
		uint64_t rip;
		unsigned len;		/* 1..15 */
		uint8_t bytes[16];
		unsigned uops;		/* micro-ops it took */
	} rt[MAXCORES] = {};
	/* per core: shut down (triple fault), halted (HLT) */
	bool shutdown[MAXCORES] = {}, halted[MAXCORES] = {};
	uint64_t shutdown_rip[MAXCORES] = {};

	virtual ~cpu() = default;
	virtual const char *name() const = 0;
	virtual unsigned ncores() const = 0;
	virtual void eval() = 0;
	virtual void final() = 0;
};

/* the versions vsim knows; nullptr (and a message) for an unknown name */
std::unique_ptr<cpu> cpu_new(const char *name, VerilatedContext *ctx);
const char *cpu_names(void);
const char *cpu_default(void);	/* the latest version */

#endif
