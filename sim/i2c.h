/* vsim on-board I2C controller and its devices (TPM, fan monitor, LM75) */
#ifndef I2C_H
#define I2C_H

#include <stdint.h>

#include "pmc.h"

#ifdef __cplusplus
extern "C" {
#endif

#define I2C_BASE	0xFE008000ull	/* 4 KiB of registers */
#define I2C_FIFO	64
#define I2C_BYTE_CYCLES	32		/* bus time per byte (sim-scaled) */
#define HWMON_ADDR	0x2F
#define LM75_ADDR	0x48

struct i2c {
	uint8_t tx[I2C_FIFO], rx[I2C_FIFO];
	unsigned ntx, nrx, rxpos;
	uint64_t status, busy_until;
	struct pmc *pmc;		/* fans and temperatures */
	uint8_t hw_ptr;			/* hardware monitor register pointer */
	uint8_t lm_ptr, lm_conf;	/* LM75 */
	uint16_t lm_thyst, lm_tos;
};

void i2c_init(struct i2c *c, struct pmc *p);
void i2c_reset(struct i2c *c);
int i2c_reg(struct i2c *c, int we, unsigned off, unsigned n, uint8_t *data, uint64_t now);

#ifdef __cplusplus
}
#endif

#endif
