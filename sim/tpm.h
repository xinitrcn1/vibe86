/* vsim debug TPM 2.0 (libtpms) on the I2C bus, TCG PTP I2C register set */
#ifndef TPM_H
#define TPM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TPM_I2C_ADDR	0x2E

/* where the NV state is kept: NULL = the fixed debug state, not saved */
void tpm_set_state_file(const char *path);
void tpm_reset(void);		/* the board's reset line (TPM_Init) */
void tpm_free(void);		/* saves the state file, if any */
/* one I2C write / read transaction to the TPM (first written byte = register) */
int tpm_i2c_write(const uint8_t *d, unsigned n);
int tpm_i2c_read(uint8_t *d, unsigned n);

#ifdef __cplusplus
}
#endif

#endif
