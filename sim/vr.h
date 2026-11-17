/*
 * vsim: the board's voltage regulators on the die's SVID-like bus (D-019,
 * docs/package.md "Voltage regulator control"): VR 0 VDDCR_CPU, VR 1
 * VDDCR_SOC. They come up at their boot voltage some clocks after power-on
 * and then report power-good (the board's PWROK). On the bus they decode
 * the die's frames at the pin level (SVC, SVD open drain, SVT the alert)
 * and answer: SetVID slews the rail (busy acknowledge, SVT low when it has
 * settled), GetReg reads status, current, temperature and VID. Anything a
 * VR doesn't accept (an unknown command, a VID out of its range, a parity
 * error) is NAKed; an address with no VR reads as NAK (the released line).
 *
 * Faults for tests (vsim -W): "vr0:nak" (VR 0 NAKs everything), "vr1:stuck"
 * (VR 1 never settles), "vr0:parity" (VR 0's read data has bad parity).
 */
#ifndef VR_H
#define VR_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct vrs;

struct vrs *vr_new(void);
void vr_free(struct vrs *m);
/* "vrN:nak|stuck|parity": 0, or -1 for a bad spec */
int vr_fault(struct vrs *m, const char *spec);
/* power-on: the rails start ramping to their boot voltage */
void vr_power_on(struct vrs *m);
/* one clock: the die's SVC and SVD outputs (its open-drain pull: 0 low);
   returns the VRs' SVD pull (*svd: 0 low) and SVT level (*svt: 0 alert) */
void vr_cycle(struct vrs *m, int svc, int svd_die, int *svd, int *svt);
/* the board's PWROK: every rail is in regulation */
int vr_power_good(const struct vrs *m);
/* the core rail is at the voltage the die set (it set one, and it settled) */
int vr_cpu_rail_set(const struct vrs *m);
/* millivolts of rail r, for messages */
unsigned vr_mv(const struct vrs *m, unsigned r);
void vr_stats(const struct vrs *m, char *buf, unsigned len);

#ifdef __cplusplus
}
#endif

#endif
