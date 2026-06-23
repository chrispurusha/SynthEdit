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

#ifdef __cplusplus
extern "C" {
#endif

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#define GL_SILENCE_DEPRECATION    1
#include <GLFW/glfw3.h>
#pragma clang diagnostic pop

#include "defs.h"
#include "types.h"
#include "globalVars.h"
#include "utilsGraphics.h"
#include "z1Graphics.h"

void z1_init_graphics(void) {
    // Initialise Z1-specific textures and resources here
}

void z1_render(tRectangle area) {
    // Placeholder — renders a centred status label
    double      cx       = area.coord.x + area.size.w * 0.5;
    double      textH    = 24.0;
    tRectangle  textRect = {{cx - 200.0, area.coord.y + 40.0}, {400.0, textH}};

    set_rgb_colour((tRgb)RGB_WHITE);
    render_text(mainArea, textRect, "Z1 Edit — work in progress");

    if (gDevice.connected) {
        tRectangle  devRect = {{cx - 200.0, area.coord.y + 80.0}, {400.0, textH}};
        set_rgb_colour((tRgb){0.4, 1.0, 0.4});
        render_text(mainArea, devRect, "Korg Z1 connected");
    } else {
        tRectangle  devRect = {{cx - 200.0, area.coord.y + 80.0}, {400.0, textH}};
        set_rgb_colour((tRgb){0.8, 0.4, 0.4});
        render_text(mainArea, devRect, "No Z1 detected");
    }
}

#ifdef __cplusplus
}
#endif
