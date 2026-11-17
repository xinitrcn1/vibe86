// power/vr-parity: VR 0 answers its status read with bad parity: the SMU
// reads it three times and leaves the die off.
// vsim-args: -b test -c 400000 -v -W vr0:parity
// expect-exit: 3
// expect-no-out: SEQ OK
// expect-err: the cores never left reset
// expect-err: 4 frames
#include "seq.inc"
