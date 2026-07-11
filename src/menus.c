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

// The generic nested context menu (open_context_menu(), handle_context_menu_click(),
// update_context_menu_hover(), render_context_menu(), gContextMenu) now lives
// in SynthLib (see contextMenu.c/h) — SynthEdit used to carry its own
// single-level flat-grid duplicate of the same names here, which started
// colliding with SynthLib's richer nested-flyout types once SynthLib picked
// up the menu system. This file is the reserved home for SynthEdit-specific
// menu-building helpers; see menus.h.

#include "defs.h"
#include "synthlibDefs.h"
#include "types.h"
#include "globalVars.h"
#include "synthComms.h"
#include "menus.h"

// Which dial the currently-open value-picker menu belongs to — read back
// inside action_set_dial_value() below, same "app keeps its own menu
// context" idea contextMenu.h's own header comment describes for G2-Edit's
// tMenuContext, just a single pointer since there's only one kind of menu
// here so far.
static tPanelDial * gMenuDial = NULL;

// PANEL_MAX_NAMES + 1 for the {NULL, ...} sentinel item — see menus.h's own
// comment / G2-Edit's menus.c for why SynthLib walks items until label==NULL
// rather than taking an explicit count.
static tMenuItem    gDialMenuItems[PANEL_MAX_NAMES + 1];

static void action_set_dial_value(int index) {
    if (gMenuDial != NULL) {
        synth_set_panel_dial_value(gMenuDial, (uint32_t)gContextMenu.items[index].param);
    }
    gContextMenu.active = false;
    gReDraw             = true;
}

void open_dial_value_menu(tCoord coord, tPanelDial * dial) {
    if (!dial) {
        return;
    }
    uint32_t n = (dial->nameCount < PANEL_MAX_NAMES) ? dial->nameCount : PANEL_MAX_NAMES;

    for (uint32_t i = 0; i < n; i++) {
        gDialMenuItems[i] = (tMenuItem){
            dial->names[i], dial->colour, action_set_dial_value, i, NULL
        };
    }

    gDialMenuItems[n] = (tMenuItem){
        NULL, (tRgb)RGB_BLACK, NULL, 0, NULL
    };

    gMenuDial         = dial;

    // A single-column menu (columns=0, the old fixed value here) grows one
    // row per item with no limit — fine for a handful of positions, but
    // Voyager's pgmShaping1Src/2Src (43 names each) or soundCategory (32)
    // render a menu roughly 1000px/750px tall, well past any reasonable
    // window height. open_context_menu()'s own clamp_menu_to_screen() only
    // REPOSITIONS a menu frame, it can't shrink one that's simply too tall
    // to fit — items past the window edge stay off-screen and unreachable
    // (2026-07-11 user report: couldn't see all of pgmShaping1Src's list,
    // and saw a stray-looking hover highlight near the cutoff, most likely
    // a symptom of hit-testing rows that were rendered off the visible
    // window rather than a real highlight bug). Capping rows per column and
    // wrapping into more columns once a list is longer than that keeps every
    // menu within a sane height regardless of item count — 12 rows per
    // column (a plain constant here, not tied to actual window height,
    // since menus.c has no reason to reach into window-size APIs for this)
    // comfortably fits any real window, and 1 column for anything at or
    // under that (every OTHER dial's list in this file) renders identically
    // to before this existed.
    const uint32_t maxRowsPerColumn = 12;
    uint32_t       columns          = (n + maxRowsPerColumn - 1) / maxRowsPerColumn;

    open_context_menu(coord, gDialMenuItems, columns, 0.0);
}
