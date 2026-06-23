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

#ifndef __MOUSE_HANDLE_H__
#define __MOUSE_HANDLE_H__

#ifdef __cplusplus
extern "C" {
#endif

void handle_mouse_button(void * win, int button, int action, int mods, double x, double y);
void handle_cursor_pos(void * win, double x, double y);
void handle_key(void * win, int key, int scancode, int action, int mods);
void handle_scroll(void * win, double dx, double dy);

#ifdef __cplusplus
}
#endif

#endif // __MOUSE_HANDLE_H__
