/*
 * The SynthEdit application.
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
#include "synthComms.h"
#include "misc.h"
#include "synthGraphics.h"

#define SYNTH_LAYOUTS_DIR_DEFAULT    "layouts"    // relative to cwd, used until a folder is chosen/persisted

static char         gLayoutsDir[1024] = SYNTH_LAYOUTS_DIR_DEFAULT;

static tPanelConfig gSynthPanelConfig = {0};

tPanelSection * synth_filters_section(void) {
    return find_panel_section(&gSynthPanelConfig, "synth", "filters");
}

tPanelConfig * synth_panel_config(void) {
    return &gSynthPanelConfig;
}

static void synth_reload_panel_config(void) {
    char path[1152];

    snprintf(path, sizeof(path), "%s/z1.txt", gLayoutsDir);  // TODO - select synth on load

    if (!load_panel_config(path, &gSynthPanelConfig)) {
        LOG_ERROR("Synth: couldn't load '%s' — filter dials will not render\n", path);
    } else {
        synth_bind_panel_dials(synth_filters_section());
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
        render_text(mainArea, r, "No synth detected — connect and press Cmd+R to scan");
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
    // Category/voiceMode/unisonType names come from xxxx.txt's named lists, not
    // hardcoded here — this block only knows the field values, not their text.
    {
        char         infoBuf[128];
        const char * uniStr = "Unison: off";
        char         uniBuf[48];

        if (gDevice.unisonOn && (gDevice.unisonType > 0)) {
            snprintf(uniBuf, sizeof(uniBuf), "Unison: %s  %u cent%s",
                     get_panel_list_item(&gSynthPanelConfig, "unisonType", gDevice.unisonType - 1),
                     (unsigned)gDevice.unisonDetune,
                     gDevice.unisonDetune == 1 ? "" : "s");
            uniStr = uniBuf;
        }
        snprintf(infoBuf, sizeof(infoBuf), "%s  |  %s  |  %s",
                 get_panel_list_item(&gSynthPanelConfig, "category", gDevice.category),
                 get_panel_list_item(&gSynthPanelConfig, "voiceMode", gDevice.voiceMode),
                 uniStr);

        tRectangle   r      = {{x, y}, {500.0, 13.0}};
        set_rgb_colour((tRgb)RGB_GREY_7);
        render_text(mainArea, r, infoBuf);
        y += 25.0;
    }

    // ── Filter dials ──────────────────────────────────────────────────────────
    // Layout, colours, ranges and labels all come from layouts/xxxx.txt via
    // panelConfig — this block only supplies the live values and draws.
    {
        tPanelSection * section = synth_filters_section();

        if (section) {
            layout_panel_section(section, (tRectangle){{x, y}, {0, 0}});

            for (uint32_t i = 0; i < section->dialCount; i++) {
                tPanelDial * dial    = &section->dials[i];
                uint32_t     dialVal = get_panel_dial_value(dial);

                render_dial(mainArea, dial->rect, dialVal, dial->max, 0, dial->colour);

                char         valBuf[24];

                if (dial->display == dialDisplayNames) {
                    snprintf(valBuf, sizeof(valBuf), "%s",
                             (dialVal < dial->nameCount) ? dial->names[dialVal] : "?");
                } else if (dial->display == dialDisplayRaw) {
                    snprintf(valBuf, sizeof(valBuf), "%u", (unsigned)dialVal);
                } else {
                    snprintf(valBuf, sizeof(valBuf), "%u (%u)", (unsigned)dialVal,
                             (unsigned)get_panel_dial_native_value(dial));
                }
                tRectangle   valRect = {{dial->rect.coord.x, dial->rect.coord.y + section->dialSize + 4.0},
                    {section->spacing,                                           12.0}};
                set_rgb_colour((tRgb)RGB_GREY_7);
                render_text(mainArea, valRect, valBuf);

                tRectangle   lblRect = {{dial->rect.coord.x, dial->rect.coord.y + section->dialSize + 18.0},
                    {section->spacing,                                            12.0}};
                render_text(mainArea, lblRect, dial->label);
            }
        }
    }
}

#ifdef __cplusplus
}
#endif
