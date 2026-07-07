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
#include "contextMenu.h" // Generic mac-style nested menu mechanism + tMenuItem/tMenuFrame/tContextMenu — see SynthLib

#ifdef __cplusplus
extern "C" {
#endif

// App-specific menu-building helpers (open_X_context_menu()) belong here once
// SynthEdit actually raises a context menu — see G2-Edit's menus.c for the
// pattern: build a static tMenuItem[] table ending in a {NULL, ...} sentinel
// (SynthLib walks items until it finds label == NULL, there's no count), then
// call open_context_menu(). Nothing does that yet, so there's nothing to
// declare here beyond what contextMenu.h already provides.

#ifdef __cplusplus
}
#endif

#endif // __MENUS_H__
