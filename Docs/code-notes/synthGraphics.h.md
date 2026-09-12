# synthGraphics.h notes

The longer comments from `synthGraphics.h`, moved here 2026-09-12 so the code reads cleanly. The code points at each as `// notes §k`. Verbatim and in file order; each is titled by what it documents.

## 1. `synth_switch_device_config()`

Switches to a different <device>.txt already known to be in the current
layouts folder (see scan_panel_configs()) — e.g. picked from the native
"Devices" menu. No-op if filename is already the one loaded. Resets
gDevice.connected first: whatever was connected under the PREVIOUS
config's identity means nothing once a different device's protocol is
loaded, so the UI shouldn't keep showing it as connected while a fresh
identity scan runs — then re-scans MIDI so the newly-loaded device can
be (re)detected. Persists the choice (set_saved_device_config(), misc.h)
so it's also what synth_init_graphics() defaults to next launch.

## 2. `synth_current_page_sections()`

Every section belonging to whichever page is currently showing on screen
(see synth_set_current_page()), in layout-file order — that order is what
determines top-to-bottom stacking when rendered (see synth_render()) and
the order mouse handling should search when hit-testing/dragging. Writes at
most maxSections pointers into outSections and returns how many were
written; each section's dial rects stay valid until the next
synth_render() call.

## 3. `synth_hit_test_page_tab()`

Hit-tests the page-tab row laid out during the last synth_render() call —
returns the tab index under coord, or -1 if none. Pure hit-test, no side
effect; used on mouse-down (to arm a tab without actioning it yet) and
again on mouse-up (to confirm the release landed back on the same tab).

## 4. `synth_set_pressed_page_tab()`

Purely cosmetic: which tab (if any) render_page_tabs() should draw in its
pressed shade. mouseHandle.c is the source of truth for whether a press
actually still counts (see gPressedTab there) — this just mirrors that for
rendering. -1 = none pressed.

## 5. `synth_hit_test_patch_nav()`

Prev/Next patch buttons, laid out on the Program name row during the last
synth_render() call (see synth_navigate_preset() in synthComms.h for what
they actually do and why "current patch" can be unknown). Mirrors the page
tab functions above: hit-test returns 0 for Prev, 1 for Next, -1 for
neither; action fires the corresponding synth_navigate_preset() call;
pressed-state is cosmetic only, same -1-means-none convention.

## 6. `synth_hit_test_prog_name()`

The program name text block, laid out on the Program name row during the
last synth_render() call — a click starts inline editing (gProgNameEdit,
globalVars.h; see mouseHandle.c). No press/release split the way
gPressedValueMenuDial needs (panel_dial_needs_value_menu()'s own comment,
mouseHandle.c) — there's no menu here that a same-click release could
prematurely dismiss, so entering edit mode happens directly on press.
