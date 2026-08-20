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

#ifndef __MOUSE_HANDLE_H__
#define __MOUSE_HANDLE_H__

#include "types.h"
#include "panelConfig.h"
#include "clickRegion.h"

#ifdef __cplusplus
extern "C" {
#endif

// Normalised input handlers, registered with SynthLib via tSynthLibInputHandlers — the coordinate
// arrives already scaled and the button already decoded. See synthlibWindow.h.
void handle_mouse_button(tCoord coord, tMouseButton button, int mods);
void handle_cursor_pos(tCoord coord);
void handle_key(int key, int scancode, int action, int mods);

// Ends a dial drag whose mouse release never arrived. Call once per frame — it no-ops unless the
// drag really is stuck. Without it, a lost release leaves every later mouse move editing that dial
// and transmitting it to the synth. See its definition.
void recover_lost_dial_drag(void * win);
void handle_char(unsigned int codepoint);
void handle_scroll(double dx, double dy);

// Arms a panel dial's press state (gDraggedDial/gPressedToggleDial/
// gPressedValueMenuDial, private to mouseHandle.c) via arm_dial_press() —
// registered by synthGraphics.cpp's synth_render() as each visible, enabled
// dial's click region (both the main per-page grid and the Info Row). Release
// is handled entirely by the existing global armed-state check in
// handle_mouse_button(), not by this handler — that logic resolves whatever
// was pressed regardless of what's under the cursor now, so it isn't a
// per-widget dispatch target.
void panel_dial_press_click_handler(tCoord coord, eClickPhase phase, void * userData);

// Same shape as panel_dial_press_click_handler above, for the three other
// press-arms-release-fires widgets — gPressedTab/gPressedPatchNav are private
// to mouseHandle.c, same reason these live here rather than in
// synthGraphics.cpp alongside the render code that registers them. userData
// carries the tab/nav index as a plain integer value (not a pointer
// dereference) via (void *)(intptr_t)index — synth_render() doesn't keep a
// stable, addressable per-tab/per-button context struct the way a dial does.
void page_tab_click_handler(tCoord coord, eClickPhase phase, void * userData);
void patch_nav_click_handler(tCoord coord, eClickPhase phase, void * userData);

// Program name field has no press/release split (see its own comment in
// mouseHandle.c) — starts editing immediately on press, same as
// panel_dial_press_click_handler ignores anything but eClickPress.
void prog_name_click_handler(tCoord coord, eClickPhase phase, void * userData);

// Supplied for SynthLib's contextMenu.c to link against — current mouse
// position in the same logical (render-scaled) space menu coords are opened
// in. See contextMenu.h.
void get_global_gui_scaled_mouse_coord(tCoord * coord);

#ifdef __cplusplus
}
#endif

#endif // __MOUSE_HANDLE_H__
