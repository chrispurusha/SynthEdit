# panelGraph.c - notes

## 2. Points from dials

Each point's x is computed left to right, so a `t=` point (an envelope time) moves on from wherever the
previous point ended up; dragging such a point sets its time dial to the distance from that previous point.
Values are read in display positions (`get_panel_dial_value()`), against `synth_dial_max()`, so a dial
shown on a pageVariant's tab uses that tab's range.

## 3. Locking to one axis

With Shift held the direction is decided once the pointer has moved a few points from where it was pressed,
by whichever way it moved further, and kept for the rest of the drag - so a point can be slid along in time
without its level drifting, or the other way round.
