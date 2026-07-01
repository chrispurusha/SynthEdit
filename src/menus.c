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

#include "defs.h"
#include "synthlibDefs.h"
#include "types.h"
#include "globalVars.h"
#include "utilsGraphics.h"
#include "utils.h"
#include "menus.h"

tContextMenu gContextMenu = {0};

void open_context_menu(tCoord coord, tMenuItem * items, uint32_t count, uint32_t columns, double cellWidth) {
    gContextMenu.active    = true;
    gContextMenu.coord     = coord;
    gContextMenu.items     = items;
    gContextMenu.count     = count;
    gContextMenu.columns   = (columns > 0) ? columns : 1;
    gContextMenu.cellWidth = (cellWidth > 0.0) ? cellWidth : 120.0;
    atomic_store(&gReDraw, true);
}

bool close_context_menu_if_outside(tCoord coord) {
    if (!gContextMenu.active) {
        return false;
    }
    uint32_t   rows     = (gContextMenu.count + gContextMenu.columns - 1) / gContextMenu.columns;
    double     menuW    = gContextMenu.cellWidth * gContextMenu.columns;
    double     cellH    = 20.0;
    double     menuH    = cellH * rows;
    tRectangle menuRect = {gContextMenu.coord, {menuW, menuH}};

    if (!within_rectangle(coord, menuRect)) {
        gContextMenu.active = false;
        atomic_store(&gReDraw, true);
        return true;
    }
    return false;
}

bool handle_context_menu_click(tCoord coord) {
    if (!gContextMenu.active) {
        return false;
    }
    double cellH = 20.0;

    for (uint32_t i = 0; i < gContextMenu.count; i++) {
        uint32_t   col      = i % gContextMenu.columns;
        uint32_t   row      = i / gContextMenu.columns;
        tRectangle itemRect = {
            {
                gContextMenu.coord.x + col * gContextMenu.cellWidth,
                gContextMenu.coord.y + row * cellH
            },
            {gContextMenu.cellWidth, cellH}
        };

        if (within_rectangle(coord, itemRect)) {
            if (gContextMenu.items[i].action != NULL) {
                gContextMenu.items[i].action(gContextMenu.items[i].index);
            }
            gContextMenu.active = false;
            atomic_store(&gReDraw, true);
            return true;
        }
    }

    return false;
}

void render_context_menu(void) {
    if (!gContextMenu.active) {
        return;
    }
    double cellH = 20.0;

    for (uint32_t i = 0; i < gContextMenu.count; i++) {
        uint32_t   col      = i % gContextMenu.columns;
        uint32_t   row      = i / gContextMenu.columns;
        tRectangle itemRect = {
            {
                gContextMenu.coord.x + col * gContextMenu.cellWidth,
                gContextMenu.coord.y + row * cellH
            },
            {gContextMenu.cellWidth, cellH}
        };
        tRectangle textRect = {
            {itemRect.coord.x + 4.0, itemRect.coord.y + 5.0},
            {                   0.0,                    9.0}
        };

        set_rgb_colour(gContextMenu.items[i].colour);
        render_rectangle(mainArea, itemRect);
        set_rgb_colour((tRgb)RGB_WHITE);
        render_text(mainArea, textRect, (char *)gContextMenu.items[i].label);
    }
}
