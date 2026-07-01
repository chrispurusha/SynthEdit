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

#ifndef __Z1_GRAPHICS_H__
#define __Z1_GRAPHICS_H__

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

// Called once after OpenGL context is ready
void z1_init_graphics(void);

// Render the full Z1 editor UI into the current frame
void z1_render(tRectangle area);

// Returns last-rendered dial rectangles (for hit-testing)
tRectangle z1_filter_routing_dial_rect(void);
tRectangle z1_filter1_type_dial_rect(void);
tRectangle z1_filter1_dial_rect(void);
tRectangle z1_filter1_res_dial_rect(void);
tRectangle z1_filter2_type_dial_rect(void);
tRectangle z1_filter2_dial_rect(void);
tRectangle z1_filter2_res_dial_rect(void);

#ifdef __cplusplus
}
#endif

#endif // __Z1_GRAPHICS_H__
