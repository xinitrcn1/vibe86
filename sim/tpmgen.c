/*
 * tpmgen: manufacture a TPM 2.0 with libtpms and write its permanent state.
 *
 *   ./tpmgen tpm-debug.permall
 *
 * libtpms draws the seeds (endorsement, platform, storage) at random when it
 * manufactures; vsim's debug TPM always starts from the state this wrote
 * once, which is in the repository, so its keys are the same on every run
 * and are NOT secret (docs/platform.md, "TPM").
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libtpms/tpm_error.h>
#include <libtpms/tpm_library.h>

static unsigned char *perm;
static uint32_t permlen;

static TPM_RESULT
nv_init(void)
{
	return 0;
}

static TPM_RESULT
nv_load(unsigned char **d, uint32_t *l, uint32_t n, const char *name)
{
	(void)d; (void)l; (void)n; (void)name;
	return TPM_RETRY;	/* nothing stored: manufacture */
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

int
main(int argc, char **argv)
{
	struct libtpms_callbacks cb = {
		.sizeOfStruct = sizeof cb,
		.tpm_nvram_init = nv_init,
		.tpm_nvram_loaddata = nv_load,
		.tpm_nvram_storedata = nv_store,
		.tpm_nvram_deletename = nv_del,
	};
	FILE *f;

	if (argc != 2) {
		fprintf(stderr, "usage: tpmgen out.permall\n");
		return 2;
	}
	TPMLIB_ChooseTPMVersion(TPMLIB_TPM_VERSION_2);
	TPMLIB_RegisterCallbacks(&cb);
	if (TPMLIB_MainInit() || !perm) {
		fprintf(stderr, "tpmgen: manufacture failed\n");
		return 1;
	}
	TPMLIB_Terminate();
	if (!(f = fopen(argv[1], "wb")) || fwrite(perm, 1, permlen, f) != permlen) {
		perror(argv[1]);
		return 1;
	}
	fclose(f);
	printf("tpmgen: %u bytes\n", permlen);
	return 0;
}
