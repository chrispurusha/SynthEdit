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

// The "synthesis"/"filters" section from xxxx.txt, laid out during the last
// synth_render() call — dial rects are valid for hit-testing until the next
// render. Always resolves to this section regardless of which page is
// currently active on screen: incoming device state must update it either way.
tPanelSection * synth_filters_section(void);

// The full parsed config, for callers that need named lists (see
// get_panel_list_item()/get_panel_list_count()) rather than a specific dial.
tPanelConfig * synth_panel_config(void);

// The section for whichever page is currently showing on screen (see
// synth_set_current_page()), or NULL if the config has no sections yet.
// This is what mouse handling should hit-test/drag against — unlike
// synth_filters_section(), it changes with the active page.
tPanelSection * synth_current_page_section(void);

const char * synth_current_page(void);
void synth_set_current_page(const char * page);

// Hit-tests the page-tab row laid out during the last synth_render() call.
// If a tab was hit, switches the current page and returns true (so the
// caller can treat the click as consumed rather than also dial-hit-testing).
bool synth_handle_page_tab_click(tCoord coord);

#ifdef __cplusplus
}
#endif

#endif // __SYNTH_GRAPHICS_H__
