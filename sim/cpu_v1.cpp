/* vsim: the v1 core (rtl/v1), one core */
#include "cpu.h"
#include "Vv1_core.h"
#include "verilated.h"

namespace {
struct cpu_v1 : cpu {
	std::unique_ptr<Vv1_core> m;

	explicit cpu_v1(VerilatedContext *ctx) : m(new Vv1_core(ctx)) {}
	const char *name() const override { return "v1"; }
	unsigned ncores() const override { return 1; }

	void eval() override
	{
		Vv1_core &v = *m;
		v.clk = clk;
		v.rst_n = rst_n;
		v.bus_ack = bus_ack;
		v.bus_err = bus_err;
		for (int i = 0; i < 4; i++)	/* 128 bits */
			v.bus_rdata[i] = bus_rdata[i];
		v.intr_valid = intr_valid && intr_core == 0;
		v.intr_vector = intr_vector & 0xFF;
		v.nmi = nmi && nmi_core == 0;
		v.mce_valid = mce_valid && mce_core == 0;
		v.mce_type = mce_type & 3;
		v.mce_addr = mce_addr;
		v.eval();
		bus_req = v.bus_req;
		bus_we = v.bus_we;
		bus_io = v.bus_io;
		bus_addr = v.bus_addr;
		bus_size = v.bus_size;
		for (int i = 0; i < 4; i++)
			bus_wdata[i] = v.bus_wdata[i];
		for (int i = 4; i < 16; i++)
			bus_wdata[i] = 0;
		event &e = ev[0];
		e.valid = v.ev_valid;
		e.vector = v.ev_vector;
		e.has_error = v.ev_has_error;
		e.error = v.ev_error;
		e.rip = v.ev_rip;
		retire &r = rt[0];
		r.valid = v.rt_valid;
		r.rip = v.rt_rip;
		r.len = v.rt_len;
		r.uops = v.rt_uops;
		for (int i = 0; i < 16; i++)
			r.bytes[i] = (uint8_t)(v.rt_bytes[i / 4] >> (8 * (i % 4)));
		shutdown[0] = v.shutdown;
		halted[0] = v.halted;
		shutdown_rip[0] = v.shutdown_rip;
	}

	void final() override { m->final(); }
};
} // namespace

std::unique_ptr<cpu>
cpu_new_v1(VerilatedContext *ctx)
{
	return std::unique_ptr<cpu>(new cpu_v1(ctx));
}
