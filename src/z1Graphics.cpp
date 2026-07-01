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

static const char * kCategoryNames[]    = {"Synth-Hard", "Synth-Soft", "E.Piano",    "Organ",
                                           "Strings",       "Brass",      "Wind",       "Bell/Mallt",
                                           "Guitar",        "Bass",       "Perc/Drums", "Vocal",
                                           "S.E./Natrl",    "Synth-Lead", "Synth-Pad",  "Synth-Comp",
                                           "Digital",       "User",       };

// Dial rectangles updated each frame; read by mouseHandle for hit-testing
static tRectangle   gFilter1DialRect    = {{0}};
static tRectangle   gFilter1ResDialRect = {{0}};
static tRectangle   gFilter2DialRect    = {{0}};
static tRectangle   gFilter2ResDialRect = {{0}};

tRectangle z1_filter1_dial_rect(void) {
    return gFilter1DialRect;
}

tRectangle z1_filter1_res_dial_rect(void) {
    return gFilter1ResDialRect;
}

tRectangle z1_filter2_dial_rect(void) {
    return gFilter2DialRect;
}

tRectangle z1_filter2_res_dial_rect(void) {
    return gFilter2ResDialRect;
}

void z1_init_graphics(void) {
}

void z1_render(tRectangle area) {
    if (!gDevice.connected) {
        tRectangle r = {{area.coord.x + 40.0, area.coord.y + 40.0}, {400.0, 14.0}};
        set_rgb_colour((tRgb){0.8, 0.4, 0.4});
        render_text(mainArea, r, "No Z1 detected — connect and press Cmd+R to scan");
        return;
    }
    double x = area.coord.x + 60.0;
    double y = area.coord.y + 40.0;

    // ── Program name ──────────────────────────────────────────────────────────
    {
        tRectangle   r  = {{x, y}, {900.0, 52.0}};
        const char * nm = (gDevice.progName[0] != '\0') ? gDevice.progName : "(loading\xe2\x80\xa6)";
        set_rgb_colour((tRgb)RGB_WHITE);
        render_text(mainArea, r, nm);
        y += 64.0;
    }

    // ── Info row: Category / Voice mode / Unison ──────────────────────────────
    {
        static const char * voiceModeNames[]  = {"Mono Multi", "Mono Single", "Poly"};
        static const char * unisonTypeNames[] = {"2 voices", "3 voices", "6 voices"};

        uint8_t             vm                = (gDevice.voiceMode < 3) ? gDevice.voiceMode : 2;
        char                infoBuf[128];
        const char *        uniStr            = "Unison: off";
        char                uniBuf[48];

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

        tRectangle          r                 = {{x, y}, {1000.0, 26.0}};
        set_rgb_colour((tRgb)RGB_GREY_7);
        render_text(mainArea, r, infoBuf);
        y += 50.0;
    }

    // ── Filter dials ──────────────────────────────────────────────────────────
    {
        const double dialSz  = 80.0;
        const double spacing = 100.0;
        const tRgb   f1Col   = {0.2, 0.6, 1.0};
        const tRgb   f2Col   = {0.3, 0.9, 0.5};

        typedef struct {
            tRectangle * rect;
            uint8_t      cc;
            uint8_t      native;
            tRgb         col;
            const char * label;
        } tDialInfo;
        tDialInfo    dials[] = {
            {&gFilter1DialRect,    gDevice.filter1Cutoff,    gDevice.filter1CutoffNative, f1Col, "F1 Cutoff"},
            {&gFilter1ResDialRect, gDevice.filter1Resonance, gDevice.filter1ResNative,    f1Col, "F1 Res"   },
            {&gFilter2DialRect,    gDevice.filter2Cutoff,    gDevice.filter2CutoffNative, f2Col, "F2 Cutoff"},
            {&gFilter2ResDialRect, gDevice.filter2Resonance, gDevice.filter2ResNative,    f2Col, "F2 Res"   }, };

        for (int i = 0; i < 4; i++) {
            double     dx       = x + i * spacing;
            tRectangle dialRect = {{dx, y}, {dialSz, dialSz}};
            *dials[i].rect = dialRect;

            set_rgb_colour(dials[i].col);
            render_dial(mainArea, dialRect, (uint32_t)dials[i].cc, 127, 0, dials[i].col);

            char       valBuf[24];
            snprintf(valBuf, sizeof(valBuf), "%u (%u)", (unsigned)dials[i].cc, (unsigned)dials[i].native);
            tRectangle valRect  = {{dx, y + dialSz + 4.0}, {spacing, 20.0}};
            set_rgb_colour((tRgb)RGB_GREY_7);
            render_text(mainArea, valRect, valBuf);

            tRectangle lblRect  = {{dx, y + dialSz + 24.0}, {spacing, 20.0}};
            render_text(mainArea, lblRect, dials[i].label);
        }
    }
}

#ifdef __cplusplus
}
#endif
