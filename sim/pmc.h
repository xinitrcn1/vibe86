/* vsim power-management controller: power, reset cause, thermal model, fans */
#ifndef PMC_H
#define PMC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PMC_BASE	0xFE009000ull	/* 4 KiB of registers */
#define PMC_FANS	2
#define PMC_TICK	1024		/* the thermal model steps every this many cycles */

enum { RC_POWERON = 0, RC_SOFT = 1, RC_WATCHDOG = 3 };
/* STATUS bits */
enum { PS_BUTTON = 1, PS_HOT = 2, PS_WAKE = 4, PS_SUSPENDED = 8 };

struct pmc {
	/* thermal model, milli-degrees C */
	double temp, amb;
	uint64_t win_access, last_access;	/* bus accesses in this window */
	int64_t temp_offset;
	int64_t trip_hot, trip_crit;
	int hot_armed;
	/* power */
	uint64_t status, reset_cause, next_cause;
	uint64_t msi_addr, msi_data;
	int suspended;
	uint64_t wake_after;			/* cycles; 0 = only the power button */
	uint64_t susp_left;
	uint64_t wdog_timeout, wdog_left;
	int critical;				/* critical trip: power off */
	int started;				/* past the power-on reset */
	/* fans (driven by the I2C hardware monitor) */
	int fan_auto;
	unsigned duty[PMC_FANS];		/* 0..255 */
	double rpm[PMC_FANS];
	uint64_t fail_timer[PMC_FANS];
	unsigned fail_inject;			/* bit i: fan i is broken */
	unsigned curve_t1, curve_t2, curve_pwm1;	/* auto curve: degrees C, duty */
	/* the board's MSI path */
	int (*msi)(void *ctx, uint64_t addr, uint64_t data);
	void *msictx;
	/* requests for the board: power off / reset */
	int req_off, req_reset;
};

void pmc_init(struct pmc *p);
/* on a board reset: keeps the reset cause the reset is for */
void pmc_reset(struct pmc *p);
/* called every PMC_TICK cycles with the board's bus-access count */
void pmc_tick(struct pmc *p, uint64_t accesses);
int pmc_reg(struct pmc *p, int we, unsigned off, unsigned n, uint8_t *data);
void pmc_button(struct pmc *p);
/* die and board temperatures in milli-degrees C; fan RPM */
int64_t pmc_die_mc(const struct pmc *p);
int64_t pmc_board_mc(const struct pmc *p);
unsigned pmc_fan_rpm(const struct pmc *p, int i);
int pmc_fan_failed(const struct pmc *p, int i);
void pmc_set_duty(struct pmc *p, int i, unsigned duty);

#ifdef __cplusplus
}
#endif

#endif
