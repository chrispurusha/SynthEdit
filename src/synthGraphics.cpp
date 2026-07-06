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

#include <ctype.h>
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
#define SYNTH_MAX_PAGE_TABS          PANEL_MAX_SECTIONS

static char         gLayoutsDir[1024]              = SYNTH_LAYOUTS_DIR_DEFAULT;

static tPanelConfig gSynthPanelConfig              = {0};

tPanelSection * synth_filters_section(void) {
    return find_panel_section(&gSynthPanelConfig, "synthesis", "filters");
}

tPanelConfig * synth_panel_config(void) {
    return &gSynthPanelConfig;
}

// ── Page tabs ─────────────────────────────────────────────────────────────────
// One tab per distinct "page" value across gSynthPanelConfig's sections (today
// that's "synthesis"/"effects", but nothing here names either specifically —
// add a page to the file and a tab appears for it automatically).
typedef struct {
    char       page[PANEL_ID_LEN];
    tRectangle rect;
} tPageTab;

static tPageTab     gPageTabs[SYNTH_MAX_PAGE_TABS] = {0};
static uint32_t     gPageTabCount                  = 0;
static char         gCurrentPage[PANEL_ID_LEN]     = {0};

const char * synth_current_page(void) {
    return gCurrentPage;
}

void synth_set_current_page(const char * page) {
    if (page && (page[0] != '\0')) {
        strncpy(gCurrentPage, page, sizeof(gCurrentPage) - 1);
        gCurrentPage[sizeof(gCurrentPage) - 1] = '\0';
        gReDraw                                = true;
    }
}

uint32_t synth_current_page_sections(tPanelSection * outSections[], uint32_t maxSections) {
    uint32_t count = 0;

    for (uint32_t i = 0; (i < gSynthPanelConfig.sectionCount) && (count < maxSections); i++) {
        if (strcmp(gSynthPanelConfig.sections[i].page, gCurrentPage) == 0) {
            outSections[count++] = &gSynthPanelConfig.sections[i];
        }
    }

    return count;
}

bool synth_handle_page_tab_click(tCoord coord) {
    for (uint32_t i = 0; i < gPageTabCount; i++) {
        if (within_rectangle(coord, gPageTabs[i].rect)) {
            synth_set_current_page(gPageTabs[i].page);
            return true;
        }
    }

    return false;
}

// Rebuilds gPageTabs from the config's distinct page names and renders them
// as a button row at `origin`, returning the height consumed. Defaults
// gCurrentPage to the first page seen if it isn't set (or no longer exists).
static double render_page_tabs(tRectangle origin) {
    gPageTabCount = 0;

    for (uint32_t i = 0; i < gSynthPanelConfig.sectionCount; i++) {
        const char * page  = gSynthPanelConfig.sections[i].page;
        bool         known = false;

        for (uint32_t t = 0; t < gPageTabCount; t++) {
            if (strcmp(gPageTabs[t].page, page) == 0) {
                known = true;
                break;
            }
        }

        if (!known && (gPageTabCount < SYNTH_MAX_PAGE_TABS)) {
            strncpy(gPageTabs[gPageTabCount].page, page, sizeof(gPageTabs[gPageTabCount].page) - 1);
            gPageTabCount++;
        }
    }

    if ((gCurrentPage[0] == '\0') && (gPageTabCount > 0)) {
        synth_set_current_page(gPageTabs[0].page);
    }
    const double tabHeight = 24.0;
    const double tabGap    = 6.0;
    double       x         = origin.coord.x;

    for (uint32_t i = 0; i < gPageTabCount; i++) {
        char       label[PANEL_LABEL_LEN];

        strncpy(label, gPageTabs[i].page, sizeof(label) - 1);
        label[sizeof(label) - 1] = '\0';

        if (label[0] != '\0') {
            label[0] = (char)toupper((unsigned char)label[0]);
        }
        // Measure at tabHeight, not an arbitrary font size — draw_button()
        // scales rendered text to the full height of the rect it's given
        // (internal_render_text: scaleFactor = rectangle.size.h / ...), so
        // the width measurement has to match that same height or the box
        // ends up too narrow for what actually gets drawn.
        double     width  = get_text_width(label, tabHeight, eNoCache); // ~8px padding each side
        tRectangle rect   = {{x, origin.coord.y}, {width, tabHeight}};
        bool       active = strcmp(gPageTabs[i].page, gCurrentPage) == 0;

        // RGB_GREY_7, not RGB_BACKGROUND_GREY — the latter is a dark grey in
        // this build, too low-contrast for draw_button's fixed black text.
        draw_button(mainArea, rect, label, active ? (tRgb)RGB_GREEN_ON : (tRgb)RGB_GREY_7);
        gPageTabs[i].rect        = rect;
        x                       += width + tabGap;
    }

    return (gPageTabCount > 0) ? (tabHeight + 12.0) : 0.0;
}

static void synth_reload_panel_config(void) {
    char path[1152];

    snprintf(path, sizeof(path), "%s/z1.txt", gLayoutsDir);  // TODO - select synth on load

    if (!load_panel_config(path, &gSynthPanelConfig)) {
        LOG_ERROR("Synth: couldn't load '%s' — dials will not render\n", path);
    } else {
        for (uint32_t s = 0; s < gSynthPanelConfig.sectionCount; s++) {
            synth_bind_panel_dials(&gSynthPanelConfig.sections[s]);
        }
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

    // ── Page tabs ──────────────────────────────────────────────────────────────
    y += render_page_tabs({{x, y}, {0, 0}});

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

    // ── Active page's dials ─────────────────────────────────────────────────────
    // Layout, colours, ranges and labels all come from layouts/xxxx.txt via
    // panelConfig — this block only supplies the live values and draws. Which
    // section(s) render follows the active page tab, not a fixed section
    // name. A page can hold several sections (e.g. Oscillator's several
    // sections stacked above Filters on the Synthesis page) — each is laid
    // out as its own row, in the file's declaration order, one below the
    // last.
    {
        tPanelSection * sections[PANEL_MAX_SECTIONS];
        uint32_t        sectionCount = synth_current_page_sections(sections, PANEL_MAX_SECTIONS);

        for (uint32_t sIdx = 0; sIdx < sectionCount; sIdx++) {
            tPanelSection * section = sections[sIdx];

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

            y += section->dialSize + 46.0; // dial + value/label text + gap before next stacked section
        }
    }
}

#ifdef __cplusplus
}
#endif
