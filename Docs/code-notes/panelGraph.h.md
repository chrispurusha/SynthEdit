# panelGraph.h - notes

## 1. Press, drag, release

`panel_graph_press()` picks up the nearest movable point of any graph among `sections` (the current page's)
and returns true if it did; `panel_graph_drag()` then moves it, `panel_graph_release()` drops it. One point
is dragged at a time, process-wide - the same as a dial.
