#include "fwlib.h"
#include "x86.h"

void
dxe_main(void *hobs)
{
	DEBUG("DXE: hello, hobs at %p\n", hobs);
	com1_puts("vibe86 DXE stub\n");
	outw(PM1_PORT + 4, 0x2000);
	for (;;)
		cpu_halt();
}
