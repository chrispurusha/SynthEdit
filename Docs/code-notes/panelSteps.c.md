# panelSteps.c - notes

## 1. Each step's dials

A `steps` line names only step 1's dials. Step n's dial is the same id with its trailing number moved on by
(n - 1) x `stride` - `a33` with stride 6 gives `a39`, `a45`, ... - found anywhere in the device (they need not be
on the grid's page). An id with no trailing number is used for step 1 alone. A value outside its dial's range
(a dial not loaded from the device yet) shows as an empty bar, and an unloaded `length` dims nothing.

## 2. Geometry

A column of row labels on the left, then one column per step. The bit rows share the height left after the
bars; each bar is a fixed 36 points tall. The step number is shown under every fourth step.

## 3. Painting

A press on a cell turns that bit over, and the drag gives every cell it crosses along the same row that new
state - so a row can be filled or cleared in one stroke. A press in a bar sets that step's value from the
height, and the drag draws the bar's level across the steps the pointer passes. Events arrive sparsely when the
pointer moves fast, so each drag fills every step between the last one painted and this one (the bar level drawn
in a straight line between them). Every change goes through `synth_set_panel_dial_value()`, exactly as turning
the step's own dial would, and only when the value actually changes.
