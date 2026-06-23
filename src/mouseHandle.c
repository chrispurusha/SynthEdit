/*
 * The Z1-Edit application.
 *
 * Copyright (C) 2025 Chris Turner <chris_purusha@icloud.com>
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
#include "types.h"
#include "globalVars.h"
#include "menus.h"
#include "mouseHandle.h"

static tCoord window_to_logical(void * win, double x, double y) {
    int winW = 0;
    int winH = 0;

    typedef void (*GetWinSizeFn)(void *, int *, int *);
    extern void glfwGetWindowSize(void *, int *, int *);
    glfwGetWindowSize(win, &winW, &winH);

    tCoord coord = {
        .x = (winW > 0) ? (x / winW) * TARGET_FRAME_BUFF_WIDTH : x,
        .y = (winH > 0) ? (y / winH) * TARGET_FRAME_BUFF_HEIGHT : y,
    };
    return coord;
}

void handle_mouse_button(void * win, int button, int action, int mods, double x, double y) {
    (void)mods;

    if (button != 0) {
        return;
    }
    tCoord coord   = window_to_logical(win, x, y);
    bool   pressed = (action == 1);

    if (close_context_menu_if_outside(coord)) {
        return;
    }
    if (handle_context_menu_click(coord)) {
        return;
    }
    (void)pressed;
}

void handle_cursor_pos(void * win, double x, double y) {
    (void)win;
    (void)x;
    (void)y;
}

void handle_key(void * win, int key, int scancode, int action, int mods) {
    (void)win;
    (void)scancode;
    (void)mods;

    if (action == 0) {
        return;
    }
    (void)key;
}

void handle_scroll(void * win, double dx, double dy) {
    (void)win;
    (void)dx;
    (void)dy;
}
