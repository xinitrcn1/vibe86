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
		/* the die interface (rtl/common/pins.toml): v1 is one core, core 0 */
		die_in(*this, *m, 0);
		m->eval();
		die_out(*this, *m, 0);
	}

	void final() override { m->final(); }
};
} // namespace

std::unique_ptr<cpu>
cpu_new_v1(VerilatedContext *ctx)
{
	return std::unique_ptr<cpu>(new cpu_v1(ctx));
}
