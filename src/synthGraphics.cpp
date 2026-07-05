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
#include <string.h>
#include "defs.h"
#include "synthlibDefs.h"
#include "types.h"
#include "globalVars.h"
#include "utilsGraphics.h"
#include "panelConfig.h"
#include "misc.h"
#include "synthGraphics.h"

#define SYNTH_LAYOUTS_DIR_DEFAULT    "layouts"    // relative to cwd, used until a folder is chosen/persisted

static char         gLayoutsDir[1024] = SYNTH_LAYOUTS_DIR_DEFAULT;

static const char * kCategoryNames[]  = {"Synth-Hard", "Synth-Soft", "E.Piano",    "Organ",
                                         "Strings",     "Brass",      "Wind",       "Bell/Mallt",
                                         "Guitar",      "Bass",       "Perc/Drums", "Vocal",
                                         "S.E./Natrl",  "Synth-Lead", "Synth-Pad",  "Synth-Comp",
                                         "Digital",     "User",       };

static tPanelConfig gSynthPanelConfig = {0};

tPanelSection * synth_filters_section(void) {
    return find_panel_section(&gSynthPanelConfig, "synth", "filters");
}

// Live value + native value for a dial, keyed by the id it's given in
// layouts/z1.txt. The descriptor only describes what a control is (position,
// range, label, display style) — the current value stays here, since it's
// protocol/device state, not layout data.
typedef struct {
    uint32_t dialVal;
    uint32_t nativeVal;
} tDialLiveValue;

static tDialLiveValue synth_dial_live_value(const char * id) {
    uint8_t f1t = (gDevice.filter1Type >= 1 && gDevice.filter1Type <= 5) ? gDevice.filter1Type : 1;
    uint8_t f2t = (gDevice.filter2Type >= 1 && gDevice.filter2Type <= 5) ? gDevice.filter2Type : 1;
    uint8_t fr  = (gDevice.filterRouting <= 2) ? gDevice.filterRouting : 0;
    uint8_t fl  = gDevice.filter2Link & 0x01;

    if (strcmp(id, "route") == 0) {
        return (tDialLiveValue){
            (uint32_t)fr, 0
        };
    } else if (strcmp(id, "f2link") == 0) {
        return (tDialLiveValue){
            (uint32_t)fl, 0
        };
    } else if (strcmp(id, "f1type") == 0) {
        return (tDialLiveValue){
            (uint32_t)(f1t - 1), 0
        };
    } else if (strcmp(id, "f1trim") == 0) {
        return (tDialLiveValue){
            gDevice.filter1InputTrim, 0
        };
    } else if (strcmp(id, "f1cut") == 0) {
        return (tDialLiveValue){
            gDevice.filter1Cutoff, gDevice.filter1CutoffNative
        };
    } else if (strcmp(id, "f1res") == 0) {
        return (tDialLiveValue){
            gDevice.filter1Resonance, gDevice.filter1ResNative
        };
    } else if (strcmp(id, "f2type") == 0) {
        return (tDialLiveValue){
            (uint32_t)(f2t - 1), 0
        };
    } else if (strcmp(id, "f2trim") == 0) {
        return (tDialLiveValue){
            gDevice.filter2InputTrim, 0
        };
    } else if (strcmp(id, "f2cut") == 0) {
        return (tDialLiveValue){
            gDevice.filter2Cutoff, gDevice.filter2CutoffNative
        };
    } else if (strcmp(id, "f2res") == 0) {
        return (tDialLiveValue){
            gDevice.filter2Resonance, gDevice.filter2ResNative
        };
    }
    return (tDialLiveValue){
        0, 0
    };
}

static void synth_reload_panel_config(void) {
    char path[1152];

    snprintf(path, sizeof(path), "%s/z1.txt", gLayoutsDir);

    if (!load_panel_config(path, &gSynthPanelConfig)) {
        LOG_ERROR("z1: couldn't load '%s' — filter dials will not render\n", path);
    }
    gReDraw = true;
}

void synth_set_layouts_dir(const char * dir) {
    if (dir && (dir[0] != '\0')) {
        strncpy(gLayoutsDir, dir, sizeof(gLayoutsDir) - 1);
        gLayoutsDir[sizeof(gLayoutsDir) - 1] = '\0';
    }
    synth_reload_panel_config();
}

void synth_init_graphics(void) {
    const char * saved = get_saved_layouts_dir();

    if (saved) {
        strncpy(gLayoutsDir, saved, sizeof(gLayoutsDir) - 1);
        gLayoutsDir[sizeof(gLayoutsDir) - 1] = '\0';
    }
    synth_reload_panel_config();
}

void synth_render(tRectangle area) {
    if (!gDevice.connected) {
        tRectangle r = {{area.coord.x + 300.0, area.coord.y + 2.0}, {800.0, 28.0}};
        set_rgb_colour((tRgb){0.8, 0.4, 0.4});
        render_text(mainArea, r, "No Z1 detected — connect and press Cmd+R to scan");
        //return;
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
    // Layout, colours, ranges and labels all come from layouts/z1.txt via
    // panelConfig — this block only supplies the live values and draws.
    {
        tPanelSection * section = synth_filters_section();

        if (section) {
            layout_panel_section(section, (tRectangle){{x, y}, {0, 0}});

            for (uint32_t i = 0; i < section->dialCount; i++) {
                tPanelDial *   dial    = &section->dials[i];
                tDialLiveValue live    = synth_dial_live_value(dial->id);

                render_dial(mainArea, dial->rect, live.dialVal, dial->max, 0, dial->colour);

                char           valBuf[24];

                if (dial->display == dialDisplayNames) {
                    snprintf(valBuf, sizeof(valBuf), "%s",
                             (live.dialVal < dial->nameCount) ? dial->names[live.dialVal] : "?");
                } else if (dial->display == dialDisplayRaw) {
                    snprintf(valBuf, sizeof(valBuf), "%u", (unsigned)live.dialVal);
                } else {
                    snprintf(valBuf, sizeof(valBuf), "%u (%u)", (unsigned)live.dialVal, (unsigned)live.nativeVal);
                }
                tRectangle     valRect = {{dial->rect.coord.x, dial->rect.coord.y + section->dialSize + 4.0},
                    {section->spacing,                                           12.0}};
                set_rgb_colour((tRgb)RGB_GREY_7);
                render_text(mainArea, valRect, valBuf);

                tRectangle     lblRect = {{dial->rect.coord.x, dial->rect.coord.y + section->dialSize + 18.0},
                    {section->spacing,                                            12.0}};
                render_text(mainArea, lblRect, dial->label);
            }
        }
    }
}

#ifdef __cplusplus
}
#endif
