/*
 * The Z1-Edit application.
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

#ifdef __cplusplus
extern "C" {
#endif

// tMenuItem and tContextMenu are defined in types.h
extern tContextMenu gContextMenu;

void open_context_menu(tCoord coord, tMenuItem * items, uint32_t count, uint32_t columns, double cellWidth);
bool handle_context_menu_click(tCoord coord);
bool close_context_menu_if_outside(tCoord coord);
void render_context_menu(void);

#ifdef __cplusplus
}
#endif

#endif // __MENUS_H__
