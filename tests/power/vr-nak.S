// power/vr-nak: VR 0 (VDDCR_CPU) NAKs every command: the SMU tries SetVID
// three times and leaves the die off (docs/package.md), so nothing runs.
// vsim-args: -b test -c 400000 -v -W vr0:nak
// expect-exit: 3
// expect-no-out: SEQ OK
// expect-err: the cores never left reset
// expect-err: 3 frames, 3 NAKs
#include "seq.inc"
