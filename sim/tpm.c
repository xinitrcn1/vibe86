/*
 * vsim debug TPM: a TPM 2.0 (libtpms) behind the I2C register interface of
 * the TCG PC Client Platform TPM Profile (PTP) Specification.
 *
 * DEBUG TPM: the permanent state (and so the endorsement, platform and
 * storage seeds) is fixed and published in sim/tpm-debug.permall; the keys
 * it derives are the same on every run and are not secret.
 *
 * Implemented registers (one-byte register address, docs/platform.md):
 * TPM_LOC_SEL 0x00, TPM_ACCESS 0x04, TPM_INT_ENABLE 0x08,
 * TPM_INT_CAPABILITY 0x14, TPM_STS 0x18, TPM_DATA_FIFO 0x24,
 * TPM_I2C_INTERFACE_CAPABILITY 0x30, TPM_I2C_DEVICE_ADDRESS 0x38,
 * TPM_DATA_CSUM_ENABLE 0x40, TPM_DID_VID 0x48, TPM_RID 0x4C.
 * Locality 0 only, no interrupts, no data checksums.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libtpms/tpm_error.h>
#include <libtpms/tpm_library.h>

#include "tpm.h"
#include "tpm_debug_state.h"

#define BUF	4096
enum { ST_IDLE, ST_READY, ST_RECEPTION, ST_COMPLETION };
/* TPM_STS bits */
#define STS_VALID	0x80
#define STS_CMD_READY	0x40
#define STS_GO		0x20
#define STS_DATA_AVAIL	0x10
#define STS_EXPECT	0x08
#define STS_SELFTEST	0x04
#define STS_RETRY	0x02
#define BURST		32

static const char *state_file;
static unsigned char *perm;
static uint32_t permlen;
static int up;			/* libtpms initialised */

static struct {
	uint8_t reg;		/* register pointer */
	int active;		/* locality 0 active */
	int state;
	uint8_t cmd[BUF], resp[BUF];
	uint32_t clen, rlen, rpos;
	uint32_t int_enable, csum_enable;
} t;

static TPM_RESULT
nv_init(void)
{
	return 0;
}

static TPM_RESULT
nv_load(unsigned char **d, uint32_t *l, uint32_t n, const char *name)
{
	(void)n;
	if (strcmp(name, "permall") || !perm)
		return TPM_RETRY;
	*d = malloc(permlen);
	memcpy(*d, perm, permlen);
	*l = permlen;
	return 0;
}

static TPM_RESULT
nv_store(const unsigned char *d, uint32_t l, uint32_t n, const char *name)
{
	(void)n;
	if (!strcmp(name, "permall")) {
		free(perm);
		perm = malloc(l);
		memcpy(perm, d, l);
		permlen = l;
	}
	return 0;
}

static TPM_RESULT
nv_del(uint32_t n, const char *name, TPM_BOOL mustexist)
{
	(void)n; (void)name; (void)mustexist;
	return 0;
}

void
tpm_set_state_file(const char *path)
{
	state_file = path;
}

static void
start(void)
{
	struct libtpms_callbacks cb = {
		.sizeOfStruct = sizeof cb,
		.tpm_nvram_init = nv_init,
		.tpm_nvram_loaddata = nv_load,
		.tpm_nvram_storedata = nv_store,
		.tpm_nvram_deletename = nv_del,
	};
	FILE *f;

	if (up)
		return;
	if (!perm && state_file && (f = fopen(state_file, "rb"))) {
		perm = malloc(1 << 20);
		permlen = (uint32_t)fread(perm, 1, 1 << 20, f);
		fclose(f);
	}
	if (!perm) {
		permlen = sizeof tpm_debug_permall;
		perm = malloc(permlen);
		memcpy(perm, tpm_debug_permall, permlen);
	}
	fprintf(stderr, "vsim: DEBUG TPM - not secure: fixed, published seeds (sim/tpm-debug.permall)%s%s\n",
	    state_file ? "; NV state in " : "", state_file ? state_file : "");
	TPMLIB_ChooseTPMVersion(TPMLIB_TPM_VERSION_2);
	TPMLIB_RegisterCallbacks(&cb);
	if (TPMLIB_MainInit() != 0) {
		fprintf(stderr, "vsim: TPM: libtpms failed to start\n");
		return;
	}
	up = 1;
}

void
tpm_reset(void)
{
	/* the board's reset is TPM_Init: the TPM restarts, NV state stays */
	if (up) {
		TPMLIB_Terminate();
		up = 0;
	}
	memset(&t, 0, sizeof t);
}

void
tpm_free(void)
{
	FILE *f;

	if (up)
		TPMLIB_Terminate();
	up = 0;
	if (state_file && perm && (f = fopen(state_file, "wb"))) {
		fwrite(perm, 1, permlen, f);
		fclose(f);
	}
	free(perm);
	perm = NULL;
}

static uint32_t
cmd_size(void)
{
	if (t.clen < 6)
		return 0;
	return (uint32_t)t.cmd[2] << 24 | (uint32_t)t.cmd[3] << 16 |
	    (uint32_t)t.cmd[4] << 8 | t.cmd[5];
}

static void
execute(void)
{
	unsigned char *r = NULL;
	uint32_t rl = 0, rs = 0;

	start();
	if (up && TPMLIB_Process(&r, &rl, &rs, t.cmd, t.clen) == 0 && rl <= BUF) {
		memcpy(t.resp, r, rl);
		t.rlen = rl;
	} else {
		/* TPM_RC_FAILURE */
		static const uint8_t fail[] = { 0x80, 0x01, 0, 0, 0, 10, 0, 0, 1, 1 };
		memcpy(t.resp, fail, sizeof fail);
		t.rlen = sizeof fail;
	}
	free(r);
	t.rpos = 0;
	t.state = ST_COMPLETION;
}

static uint8_t
sts_byte(void)
{
	uint8_t s = STS_VALID | STS_SELFTEST;
	uint32_t want = cmd_size();

	if (t.state == ST_READY)
		s |= STS_CMD_READY;
	if (t.state == ST_COMPLETION && t.rpos < t.rlen)
		s |= STS_DATA_AVAIL;
	if (t.state == ST_RECEPTION && (t.clen < 6 || t.clen < want))
		s |= STS_EXPECT;
	return s;
}

int
tpm_i2c_write(const uint8_t *d, unsigned n)
{
	if (!n)
		return 0;
	t.reg = d[0];
	d++;
	n--;
	if (!n)
		return 0;		/* a pointer write before a read */
	switch (t.reg) {
	case 0x00:			/* TPM_LOC_SEL: locality 0 only */
		break;
	case 0x04:			/* TPM_ACCESS */
		if (d[0] & 0x02)
			t.active = 1;	/* requestUse */
		if (d[0] & 0x20)
			t.active = 0;	/* activeLocality: relinquish */
		break;
	case 0x08:
		memcpy(&t.int_enable, d, n < 4 ? n : 4);
		break;
	case 0x18:			/* TPM_STS */
		if (d[0] & STS_CMD_READY) {
			t.state = ST_READY;
			t.clen = t.rlen = t.rpos = 0;
		}
		if ((d[0] & STS_GO) && t.state == ST_RECEPTION && t.clen >= 10 &&
		    t.clen == cmd_size())
			execute();
		if ((d[0] & STS_RETRY) && t.state == ST_COMPLETION)
			t.rpos = 0;
		break;
	case 0x24:			/* TPM_DATA_FIFO */
		if (t.state != ST_READY && t.state != ST_RECEPTION)
			break;
		t.state = ST_RECEPTION;
		for (unsigned i = 0; i < n && t.clen < BUF; i++)
			t.cmd[t.clen++] = d[i];
		break;
	case 0x40:
		memcpy(&t.csum_enable, d, n < 4 ? n : 4);
		break;
	default:
		break;
	}
	return 0;
}

int
tpm_i2c_read(uint8_t *d, unsigned n)
{
	uint8_t reg[4] = { 0 };

	switch (t.reg) {
	case 0x24:
		for (unsigned i = 0; i < n; i++)
			d[i] = (t.state == ST_COMPLETION && t.rpos < t.rlen) ? t.resp[t.rpos++] : 0xFF;
		return 0;
	case 0x00: break;
	case 0x04: reg[0] = 0x80 | (t.active ? 0x20 : 0); break;
	case 0x08: memcpy(reg, &t.int_enable, 4); break;
	case 0x14: break;			/* no interrupts */
	case 0x18:
		reg[0] = sts_byte();
		reg[1] = BURST;
		reg[3] = 0x04;			/* tpmFamily = TPM 2.0 */
		break;
	case 0x30: reg[0] = 0x02; break;	/* interface type: I2C */
	case 0x38: reg[0] = TPM_I2C_ADDR; break;
	case 0x40: memcpy(reg, &t.csum_enable, 4); break;
	case 0x48:				/* DID 0x0001, VID 0xFFFF: placeholders */
		reg[0] = 0xFF; reg[1] = 0xFF; reg[2] = 0x01; reg[3] = 0x00;
		break;
	case 0x4C: reg[0] = 0x01; break;
	default: break;
	}
	for (unsigned i = 0; i < n; i++)
		d[i] = i < 4 ? reg[i] : 0;
	return 0;
}
