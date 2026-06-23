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

#include <stdio.h>
#include "defs.h"
#include "types.h"
#include "globalVars.h"
#include "utilsGraphics.h"
#include "z1Graphics.h"

static const char * kCategoryNames[] = {
    "Synth-Hard", "Synth-Soft", "E.Piano",    "Organ",
    "Strings",    "Brass",      "Wind",        "Bell/Mallt",
    "Guitar",     "Bass",       "Perc/Drums",  "Vocal",
    "S.E./Natrl", "Synth-Lead", "Synth-Pad",   "Synth-Comp",
    "Digital",    "User",
};

// Dial rectangle updated each frame; read by mouseHandle for hit-testing
static tRectangle gFilter1DialRect = {{0}};

tRectangle z1_filter1_dial_rect(void) {
    return gFilter1DialRect;
}

void z1_init_graphics(void) {
}

void z1_render(tRectangle area) {
    if (!gDevice.connected) {
        tRectangle r = {{area.coord.x + 40.0, area.coord.y + 40.0}, {800.0, 28.0}};
        set_rgb_colour((tRgb){0.8, 0.4, 0.4});
        render_text(mainArea, r, "No Z1 detected — connect and press Cmd+R to scan");
        return;
    }

    double x = area.coord.x + 60.0;
    double y = area.coord.y + 40.0;

    // ── Program name ──────────────────────────────────────────────────────────
    {
        tRectangle r    = {{x, y}, {900.0, 52.0}};
        const char * nm = (gDevice.progName[0] != '\0') ? gDevice.progName : "(loading\xe2\x80\xa6)";
        set_rgb_colour((tRgb)RGB_WHITE);
        render_text(mainArea, r, nm);
        y += 64.0;
    }

    // ── Info row: Category / Voice mode / Unison ──────────────────────────────
    {
        static const char * voiceModeNames[] = {"Mono Multi", "Mono Single", "Poly"};
        static const char * unisonTypeNames[] = {"2 voices", "3 voices", "6 voices"};

        uint8_t     vm  = (gDevice.voiceMode < 3) ? gDevice.voiceMode : 2;
        char        infoBuf[128];
        const char * uniStr = "Unison: off";
        char         uniBuf[48];

        if (gDevice.unisonOn && (gDevice.unisonType > 0)) {
            uint8_t ut = (gDevice.unisonType - 1) < 3 ? (gDevice.unisonType - 1) : 2;
            snprintf(uniBuf, sizeof(uniBuf), "Unison: %s  %u cent%s",
                     unisonTypeNames[ut],
                     (unsigned)gDevice.unisonDetune,
                     gDevice.unisonDetune == 1 ? "" : "s");
            uniStr = uniBuf;
        }

        snprintf(infoBuf, sizeof(infoBuf), "%s  |  %s  |  %s",
                 (gDevice.category < 18) ? kCategoryNames[gDevice.category] : "?",
                 voiceModeNames[vm],
                 uniStr);

        tRectangle r = {{x, y}, {1000.0, 26.0}};
        set_rgb_colour((tRgb)RGB_GREY_7);
        render_text(mainArea, r, infoBuf);
        y += 50.0;
    }

    // ── Filter 1 Cutoff dial ──────────────────────────────────────────────────
    {
        double     dialSz   = 80.0;
        tRectangle dialRect = {{x, y}, {dialSz, dialSz}};
        gFilter1DialRect = dialRect;

        set_rgb_colour((tRgb){0.2, 0.6, 1.0});
        render_dial(mainArea, dialRect,
                    (uint32_t)gDevice.filter1Cutoff, 127, 0,
                    (tRgb){0.2, 0.6, 1.0});

        // CC value and native (SysEx) value below dial
        char valBuf[32];
        snprintf(valBuf, sizeof(valBuf), "%u  (%u)",
                 (unsigned)gDevice.filter1Cutoff,
                 (unsigned)gDevice.filter1CutoffNative);
        tRectangle valRect = {{x, y + dialSz + 4.0}, {dialSz * 2.0, 20.0}};
        set_rgb_colour((tRgb)RGB_GREY_7);
        render_text(mainArea, valRect, valBuf);

        tRectangle lblRect = {{x, y + dialSz + 24.0}, {dialSz * 2.0, 20.0}};
        render_text(mainArea, lblRect, "Filter 1 Cutoff");
    }
}

#ifdef __cplusplus
}
#endif
