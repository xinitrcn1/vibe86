/*
 * tap_harness: vsim's JTAG probe server (sim/jtag_bitbang.cpp) driving the
 * Verilated JTAG TAP and debug module (tap_top.sv) in the same per-cycle
 * loop vsim uses, so tests/jtag/bitbang.py can test the protocol and the
 * TAP before the die top exists. Arguments as vsim's; only -D PORT and
 * -c CYCLES matter.
 */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

#include "Vtap_top.h"
#include "jtag_bitbang.h"
#include "verilated.h"

int
main(int argc, char **argv)
{
	int port = 0;
	unsigned long long max = 2000000000ull;
	for (int i = 1; i < argc - 1; i++) {
		if (!std::strcmp(argv[i], "-D"))
			port = std::atoi(argv[i + 1]);
		else if (!std::strcmp(argv[i], "-c"))
			max = std::strtoull(argv[i + 1], nullptr, 0);
	}
	if (!port) {
		std::fprintf(stderr, "usage: tap_harness -D port [-c cycles] ...\n");
		return 2;
	}
	auto ctx = std::make_unique<VerilatedContext>();
	auto top = std::make_unique<Vtap_top>(ctx.get());
	jtag_bitbang jb;
	if (jtag_bitbang_open(&jb, port))
		return 4;
	top->trst_n = 0;			/* power-on reset of the TAP */
	top->clk = 0;
	top->eval();
	top->clk = 1;
	top->eval();
	for (unsigned long long c = 0; c < max; c++) {
		jtag_bitbang_poll(&jb, top->tdo);
		top->tck = jb.tck;
		top->tms = jb.tms;
		top->tdi = jb.tdi;
		top->trst_n = !jb.trst;
		top->clk = 0;
		top->eval();
		top->clk = 1;
		top->eval();
	}
	top->final();
	jtag_bitbang_close(&jb);
	return 0;
}
