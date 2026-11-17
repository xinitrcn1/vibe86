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

#include "vibe86_die.h"

class VerilatedContext;

struct cpu : die_pins {
	/* the pins: struct die_pins, generated from rtl/common/pins.toml
	 * (vibe86_die.h): the core-uncore bus (one request at a time, held
	 * until acknowledged; 512-bit data), the routed interrupt inputs
	 * (intr_*, nmi, mce_* with *_core naming the core) and the per-core
	 * outputs (intr_ack[], ev[], rt[], shutdown[], halted[]). */

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
