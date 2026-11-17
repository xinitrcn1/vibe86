// power/seq: the die's power sequence (D-019, docs/package.md): PWROK from
// the VRs (sim/vr.c), the SMU sets VDDCR_CPU to 1.00 V (VID 150, from its
// 0.95 V boot voltage: a busy acknowledge, SVT when settled, a status read)
// and VDDCR_SOC to 0.90 V (already there: a plain ACK), then RESET_L lets
// the die run. vsim checks that no instruction retires before VDDCR_CPU is
// at the voltage the die set.
// vsim-args: -b test -c 2000000 -v
// expect-out: SEQ OK
// expect-err: VRs: VDDCR_CPU 1000 mV, VDDCR_SOC 900 mV; 3 frames, 0 NAKs, 0 parity errors
#include "seq.inc"
