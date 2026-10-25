# panelSteps.h - notes

## 3. Press, drag, release

`panel_steps_press()` takes a press on any step grid among `sections` (the current page's) and returns true if
it did; `panel_steps_drag()` paints on from there and `panel_steps_release()` ends it. One drag at a time,
process-wide - the same as a dial or a graph point.
