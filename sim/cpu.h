/*
 * vsim's view of the processor in the socket: the core-side pins of a
 * Verilated model (rtl/<version>), the same for every core version. vsim -C
 * picks the version (cpu_new). An adapter (cpu_v1.cpp, ...) copies the
 * inputs into its model, evaluates it and copies the outputs back.
 *
 * The package boundary (docs/package.md, D-019): the die's balls -- DDR5
 * to the DIMMs (ddr5.c), the SPD buses, the I/O link to the chipset
 * (iod.c), JTAG -- plus the simulation observation ports. Interrupts and
 * machine checks from the chipset are I/O-link messages (MSIs, virtual
 * wires, MCE), not pins.
 */
#ifndef CPU_H
#define CPU_H

#include <cstdint>
#include <memory>

#include "vibe86_die.h"

class VerilatedContext;

struct cpu : die_pins {
	/* the pins: struct die_pins, generated from rtl/common/pins.toml
	 * (vibe86_die.h): the balls and the per-core observation outputs
	 * (ev[], rt[], shutdown[], halted[]). */

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
