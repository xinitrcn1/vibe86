/*
 * host.c: the native half of the differential test (vgen.py): runs the
 * generated diff_body on the host CPU and prints the state after every step
 * and the scratch buffer, exactly as sim.S prints them on vsim.
 */
#include <stdint.h>
#include <stdio.h>

void diff_body(uint8_t *buf, uint64_t *out);
extern const uint64_t diff_steps;	/* the step count */

static _Alignas(16) uint8_t buf[256];
static uint64_t out[19 << 14];

int main(void)
{
	unsigned long n = diff_steps;

	for (int i = 0; i < 256; i++)
		buf[i] = (uint8_t)(i * 0x9d + 7);
	diff_body(buf, out);
	for (unsigned long i = 0; i < 19 * n; i++)
		printf("%016llx\n", (unsigned long long)out[i]);
	for (int i = 0; i < 256; i += 8)
		printf("m%016llx\n", (unsigned long long)*(uint64_t *)&buf[i]);
	printf("end\n");
	return 0;
}
