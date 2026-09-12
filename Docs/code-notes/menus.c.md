# menus.c notes

The longer comments from `menus.c`, moved here 2026-09-12 so the code reads cleanly. The code points at each as `// notes §k`. Verbatim and in file order; each is titled by what it documents.

## 1. file scope

The generic nested context menu (open_context_menu(), handle_context_menu_click(),
update_context_menu_hover(), render_context_menu(), gContextMenu) now lives
in SynthLib (see contextMenu.c/h) — SynthEdit used to carry its own
single-level flat-grid duplicate of the same names here, which started
colliding with SynthLib's richer nested-flyout types once SynthLib picked
up the menu system. This file is the reserved home for SynthEdit-specific
menu-building helpers; see menus.h.

## 2. `gMenuDial`

Which dial the currently-open value-picker menu belongs to — read back
inside action_set_dial_value() below, same "app keeps its own menu
context" idea contextMenu.h's own header comment describes for G2-Edit's
tMenuContext, just a single pointer since there's only one kind of menu
here so far.

## 3. in `open_dial_value_menu()`

A single-column menu (columns=0, the old fixed value here) grows one
row per item with no limit — fine for a handful of positions, but
Voyager's pgmShaping1Src/2Src (43 names each) or soundCategory (32)
render a menu roughly 1000px/750px tall, well past any reasonable
window height. open_context_menu()'s own clamp_menu_to_screen() only
REPOSITIONS a menu frame, it can't shrink one that's simply too tall
to fit — items past the window edge stay off-screen and unreachable
(2026-07-11 user report: couldn't see all of pgmShaping1Src's list,
and saw a stray-looking hover highlight near the cutoff, most likely
a symptom of hit-testing rows that were rendered off the visible
window rather than a real highlight bug). Capping rows per column and
wrapping into more columns once a list is longer than that keeps every
menu within a sane height regardless of item count — 12 rows per
column (a plain constant here, not tied to actual window height,
since menus.c has no reason to reach into window-size APIs for this)
comfortably fits any real window, and 1 column for anything at or
under that (every OTHER dial's list in this file) renders identically
to before this existed.
