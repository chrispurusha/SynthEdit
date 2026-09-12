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
// Notes: Docs/code-notes/synthGraphics.h.md - "// notes §k" refers there.

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

// notes §1
void synth_switch_device_config(const char * filename);

// The full parsed config, for callers that need named lists (see
// get_panel_list_item()/get_panel_list_count()) rather than a specific dial.
tPanelConfig * synth_panel_config(void);

// notes §2
uint32_t synth_current_page_sections(tPanelSection * outSections[], uint32_t maxSections);

const char * synth_current_page(void);
void synth_set_current_page(const char * page);

// notes §3
int32_t synth_hit_test_page_tab(tCoord coord);

// Switches to the page at gPageTabs[index] (see synth_hit_test_page_tab()).
// Actions the tab — call only on mouse-up, once the release has been
// confirmed to land back on the tab that was pressed.
void synth_action_page_tab(int32_t index);

// notes §4
void synth_set_pressed_page_tab(int32_t index);

// notes §5
int32_t synth_hit_test_patch_nav(tCoord coord);
void synth_action_patch_nav(int32_t index);
void synth_set_pressed_patch_nav(int32_t index);

// notes §6
bool synth_hit_test_prog_name(tCoord coord);

#ifdef __cplusplus
}
#endif

#endif // __SYNTH_GRAPHICS_H__
