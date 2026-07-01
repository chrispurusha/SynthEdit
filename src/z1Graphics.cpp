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

static const char * kCategoryNames[]     = {"Synth-Hard", "Synth-Soft", "E.Piano",    "Organ",
                                            "Strings",        "Brass",      "Wind",       "Bell/Mallt",
                                            "Guitar",         "Bass",       "Perc/Drums", "Vocal",
                                            "S.E./Natrl",     "Synth-Lead", "Synth-Pad",  "Synth-Comp",
                                            "Digital",        "User",       };

static const char * kFilterTypeNames[]   = {"LPF", "HPF", "BPF", "BRF", "2BPF"};

// Dial rectangles updated each frame; read by mouseHandle for hit-testing
static tRectangle   gFilter1TypeDialRect = {{0}};
static tRectangle   gFilter1DialRect     = {{0}};
static tRectangle   gFilter1ResDialRect  = {{0}};
static tRectangle   gFilter2TypeDialRect = {{0}};
static tRectangle   gFilter2DialRect     = {{0}};
static tRectangle   gFilter2ResDialRect  = {{0}};

tRectangle z1_filter1_type_dial_rect(void) {
    return gFilter1TypeDialRect;
}

tRectangle z1_filter1_dial_rect(void) {
    return gFilter1DialRect;
}

tRectangle z1_filter1_res_dial_rect(void) {
    return gFilter1ResDialRect;
}

tRectangle z1_filter2_type_dial_rect(void) {
    return gFilter2TypeDialRect;
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
        tRectangle r = {{area.coord.x + 20.0, area.coord.y + 20.0}, {400.0, 14.0}};
        set_rgb_colour((tRgb){0.8, 0.4, 0.4});
        render_text(mainArea, r, "No Z1 detected — connect and press Cmd+R to scan");
        return;
    }
    double x = area.coord.x + 30.0;
    double y = area.coord.y + 20.0;

    // ── Program name ──────────────────────────────────────────────────────────
    {
        tRectangle   r  = {{x, y}, {450.0, 26.0}};
        const char * nm = (gDevice.progName[0] != '\0') ? gDevice.progName : "(loading\xe2\x80\xa6)";
        set_rgb_colour((tRgb)RGB_WHITE);
        render_text(mainArea, r, nm);
        y += 32.0;
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

        tRectangle          r                 = {{x, y}, {500.0, 13.0}};
        set_rgb_colour((tRgb)RGB_GREY_7);
        render_text(mainArea, r, infoBuf);
        y += 25.0;
    }

    // ── Filter dials ──────────────────────────────────────────────────────────
    // Layout: [F1 Type] [F1 Cut] [F1 Res] <gap> [F2 Type] [F2 Cut] [F2 Res]
    {
        const double dialSz  = 40.0;
        const double spacing = 50.0;
        const double gap     = 20.0;    // extra space between F1 and F2 groups
        const tRgb   f1Col   = {0.2, 0.6, 1.0};
        const tRgb   f2Col   = {0.3, 0.9, 0.5};

        uint8_t      f1t     = (gDevice.filter1Type >= 1 && gDevice.filter1Type <= 5)
                      ? gDevice.filter1Type : 1;
        uint8_t      f2t     = (gDevice.filter2Type >= 1 && gDevice.filter2Type <= 5)
                      ? gDevice.filter2Type : 1;

        typedef struct {
            tRectangle * rect;
            uint32_t     dialVal;    // 0-based value for render_dial
            uint32_t     nativeVal;  // for cutoff/res; 0 for type dials
            uint32_t     dialMax;    // max for render_dial: 4 for type, 127 for cutoff/res
            tRgb         col;
            const char * label;
            bool         isType;
        } tDialInfo;

        tDialInfo    dials[] = {
            {&gFilter1TypeDialRect, (uint32_t)(f1t - 1),                                0,   4, f1Col, "F1 Type", true },
            {&gFilter1DialRect,     gDevice.filter1Cutoff,    gDevice.filter1CutoffNative, 127, f1Col, "F1 Cut",  false},
            {&gFilter1ResDialRect,  gDevice.filter1Resonance, gDevice.filter1ResNative,    127, f1Col, "F1 Res",  false},
            {&gFilter2TypeDialRect, (uint32_t)(f2t - 1),                                0,   4, f2Col, "F2 Type", true },
            {&gFilter2DialRect,     gDevice.filter2Cutoff,    gDevice.filter2CutoffNative, 127, f2Col, "F2 Cut",  false},
            {&gFilter2ResDialRect,  gDevice.filter2Resonance, gDevice.filter2ResNative,    127, f2Col, "F2 Res",  false}, };

        for (int i = 0; i < 6; i++) {
            double     groupGap = (i >= 3) ? gap : 0.0;
            double     dx       = x + i * spacing + groupGap;
            tRectangle dialRect = {{dx, y}, {dialSz, dialSz}};
            *dials[i].rect = dialRect;

            render_dial(mainArea, dialRect, dials[i].dialVal, dials[i].dialMax, 0, dials[i].col);

            char       valBuf[24];

            if (dials[i].isType) {
                snprintf(valBuf, sizeof(valBuf), "%s", kFilterTypeNames[dials[i].dialVal]);
            } else {
                snprintf(valBuf, sizeof(valBuf), "%u (%u)", (unsigned)dials[i].dialVal,
                         (unsigned)dials[i].nativeVal);
            }
            tRectangle valRect  = {{dx, y + dialSz + 4.0}, {spacing, 12.0}};
            set_rgb_colour((tRgb)RGB_GREY_7);
            render_text(mainArea, valRect, valBuf);

            tRectangle lblRect  = {{dx, y + dialSz + 18.0}, {spacing, 12.0}};
            render_text(mainArea, lblRect, dials[i].label);
        }
    }
}

#ifdef __cplusplus
}
#endif
