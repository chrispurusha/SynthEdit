# menus.h notes

The longer comments from `menus.h`, moved here 2026-09-12 so the code reads cleanly. The code points at each as `// notes §k`. Verbatim and in file order; each is titled by what it documents.

## 1. `open_dial_value_menu()`

Opens a value-picker menu listing dial's own names[], for a discrete
selector with no CC at all (panel_dial_needs_value_menu() in
panelConfig.h) — picking an item calls synth_set_panel_dial_value() with
that item's display index, same as any other dial change, so it patches
the cached dump and resends exactly once with the final value rather than
once per intermediate step a drag gesture would otherwise pass through.
