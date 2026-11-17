// power/vr-stuck: VR 1 (VDDCR_SOC) acknowledges SetVID busy and never
// settles: the SMU gives up after 2^18 clocks and the die stays off.
// vsim-args: -b test -c 600000 -v -W vr1:stuck
// expect-exit: 3
// expect-no-out: SEQ OK
// expect-err: the cores never left reset
#include "seq.inc"
