/* vsim: the core versions it can put in the socket (-C) */
#include <cstdio>
#include <cstring>
#include "cpu.h"

std::unique_ptr<cpu> cpu_new_v1(VerilatedContext *ctx);

/* oldest first; the last one is the latest, vsim's default */
static const struct {
	const char *name;
	std::unique_ptr<cpu> (*make)(VerilatedContext *);
} designs[] = {
	{ "v1", cpu_new_v1 },
};

std::unique_ptr<cpu>
cpu_new(const char *name, VerilatedContext *ctx)
{
	for (const auto &d : designs)
		if (!std::strcmp(d.name, name))
			return d.make(ctx);
	std::fprintf(stderr, "vsim: unknown core '%s' (known: %s)\n", name, cpu_names());
	return nullptr;
}

const char *
cpu_names(void)
{
	static char buf[128];
	buf[0] = 0;
	for (const auto &d : designs) {
		if (buf[0])
			std::strcat(buf, " ");
		std::strcat(buf, d.name);
	}
	return buf;
}

const char *
cpu_default(void)
{
	return designs[sizeof designs / sizeof designs[0] - 1].name;
}
