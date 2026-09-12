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
// Notes: Docs/code-notes/mouseHandle.h.md - "// notes §k" refers there.

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

// notes §1
void panel_dial_press_click_handler(tCoord coord, eClickPhase phase, void * userData);

// notes §2
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
