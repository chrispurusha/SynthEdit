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

#ifndef __MENUS_H__
#define __MENUS_H__

#include "types.h"
#include "panelConfig.h" // tPanelDial
#include "contextMenu.h" // Generic mac-style nested menu mechanism + tMenuItem/tMenuFrame/tContextMenu — see SynthLib

#ifdef __cplusplus
extern "C" {
#endif

// Opens a value-picker menu listing dial's own names[], for a discrete
// selector with no CC at all (panel_dial_needs_value_menu() in
// panelConfig.h) — picking an item calls synth_set_panel_dial_value() with
// that item's display index, same as any other dial change, so it patches
// the cached dump and resends exactly once with the final value rather than
// once per intermediate step a drag gesture would otherwise pass through.
void open_dial_value_menu(tCoord coord, tPanelDial * dial);

#ifdef __cplusplus
}
#endif

#endif // __MENUS_H__
