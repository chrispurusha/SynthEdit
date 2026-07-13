/*
 * The SynthEdit application.
 *
 * Copyright (C) 2026 Chris Turner <chris_purusha@icloud.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef __SYNTH_GRAPHICS_H__
#define __SYNTH_GRAPHICS_H__

#include "types.h"
#include "panelConfig.h"

#ifdef __cplusplus
extern "C" {
#endif

// Called once after OpenGL context is ready — loads xxxx.txt from the saved
// layouts folder preference (see get_saved_layouts_dir() in misc.h), or
// "layouts" (relative to cwd) if no preference has been set yet.
void synth_init_graphics(void);

// Render the full editor UI into the current frame
void synth_render(tRectangle area);

// Points the layouts folder at `dir`, persists nothing itself (the caller —
// the "Choose Layouts Folder…" menu action — owns persistence), and reloads
// xxxx.txt from the new location immediately.
void synth_set_layouts_dir(const char * dir);

// Current layouts folder / active <device>.txt filename (e.g. "z1.txt") —
// exposed for the native "Devices" menu (misc.mm) to list scan_panel_
// configs() candidates against and mark which one is currently loaded.
const char * synth_layouts_dir(void);
const char * synth_current_device_config(void);

// Switches to a different <device>.txt already known to be in the current
// layouts folder (see scan_panel_configs()) — e.g. picked from the native
// "Devices" menu. No-op if filename is already the one loaded. Resets
// gDevice.connected first: whatever was connected under the PREVIOUS
// config's identity means nothing once a different device's protocol is
// loaded, so the UI shouldn't keep showing it as connected while a fresh
// identity scan runs — then re-scans MIDI so the newly-loaded device can
// be (re)detected. Persists the choice (set_saved_device_config(), misc.h)
// so it's also what synth_init_graphics() defaults to next launch.
void synth_switch_device_config(const char * filename);

// The full parsed config, for callers that need named lists (see
// get_panel_list_item()/get_panel_list_count()) rather than a specific dial.
tPanelConfig * synth_panel_config(void);

// Every section belonging to whichever page is currently showing on screen
// (see synth_set_current_page()), in layout-file order — that order is what
// determines top-to-bottom stacking when rendered (see synth_render()) and
// the order mouse handling should search when hit-testing/dragging. Writes at
// most maxSections pointers into outSections and returns how many were
// written; each section's dial rects stay valid until the next
// synth_render() call.
uint32_t synth_current_page_sections(tPanelSection * outSections[], uint32_t maxSections);

const char * synth_current_page(void);
void synth_set_current_page(const char * page);

// Hit-tests the page-tab row laid out during the last synth_render() call —
// returns the tab index under coord, or -1 if none. Pure hit-test, no side
// effect; used on mouse-down (to arm a tab without actioning it yet) and
// again on mouse-up (to confirm the release landed back on the same tab).
int32_t synth_hit_test_page_tab(tCoord coord);

// Switches to the page at gPageTabs[index] (see synth_hit_test_page_tab()).
// Actions the tab — call only on mouse-up, once the release has been
// confirmed to land back on the tab that was pressed.
void synth_action_page_tab(int32_t index);

// Purely cosmetic: which tab (if any) render_page_tabs() should draw in its
// pressed shade. mouseHandle.c is the source of truth for whether a press
// actually still counts (see gPressedTab there) — this just mirrors that for
// rendering. -1 = none pressed.
void synth_set_pressed_page_tab(int32_t index);

// Prev/Next patch buttons, laid out on the Program name row during the last
// synth_render() call (see synth_navigate_preset() in synthComms.h for what
// they actually do and why "current patch" can be unknown). Mirrors the page
// tab functions above: hit-test returns 0 for Prev, 1 for Next, -1 for
// neither; action fires the corresponding synth_navigate_preset() call;
// pressed-state is cosmetic only, same -1-means-none convention.
int32_t synth_hit_test_patch_nav(tCoord coord);
void synth_action_patch_nav(int32_t index);
void synth_set_pressed_patch_nav(int32_t index);

// The program name text block, laid out on the Program name row during the
// last synth_render() call — a click starts inline editing (gProgNameEdit,
// globalVars.h; see mouseHandle.c). No press/release split the way
// gPressedValueMenuDial needs (panel_dial_needs_value_menu()'s own comment,
// mouseHandle.c) — there's no menu here that a same-click release could
// prematurely dismiss, so entering edit mode happens directly on press.
bool synth_hit_test_prog_name(tCoord coord);

// Hit-tests every dial belonging to a `hidden` section — the Info Row (see
// synth_render()'s own comment) — anywhere in the config, not just whatever
// page is currently showing (unlike synth_current_page_sections(), which
// deliberately excludes hidden sections entirely). Mirrors the Info Row's
// own render-time iteration exactly so a click lands on the segment it
// visually corresponds to; each segment's tRectangle is (re)computed and
// stored into that dial's own `rect` field during the last synth_render()
// call, the same rect field a grid dial's own layout_panel_section() uses —
// safe to share since a hidden-section dial is never grid-laid-out.
// Returns NULL if nothing hit. Callers should still check
// panel_dial_needs_value_menu() before acting on the result, same as the
// generic per-page dial hit-test in mouseHandle.c does — not every hidden
// dial is necessarily a selectable-by-menu one (e.g. a purely numeric
// read-only field).
tPanelDial * synth_hit_test_info_row(tCoord coord);

#ifdef __cplusplus
}
#endif

#endif // __SYNTH_GRAPHICS_H__
