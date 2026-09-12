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
// Notes: Docs/code-notes/synthGraphics.c.md - "// notes §k" refers there.

#ifdef __cplusplus
extern "C" {
#endif

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#define GL_SILENCE_DEPRECATION    1
#include <GLFW/glfw3.h>
#pragma clang diagnostic pop

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "defs.h"
#include "synthlibDefs.h"
#include "types.h"
#include "globalVars.h"
#include "utilsGraphics.h"
#include "panelConfig.h"
#include "synthComms.h"
#include "synthBackup.h"
#include "midiComms.h"
#include "misc.h"
#include "bankBrowser.h"
#include "synthGraphics.h"
#include "mouseHandle.h"

#define SYNTH_LAYOUTS_DIR_DEFAULT    "layouts"    // relative to cwd, used until a folder is chosen/persisted
#define SYNTH_MAX_PAGE_TABS          PANEL_MAX_SECTIONS

static char         gLayoutsDir[1024]              = SYNTH_LAYOUTS_DIR_DEFAULT;

static tPanelConfig gSynthPanelConfig              = {0};

tPanelConfig * synth_panel_config(void) {
    return &gSynthPanelConfig;
}

// notes §1
typedef struct {
    char       page[PANEL_ID_LEN];
    tRectangle rect;
} tPageTab;

static tPageTab     gPageTabs[SYNTH_MAX_PAGE_TABS] = {0};
static uint32_t     gPageTabCount                  = 0;
static char         gCurrentPage[PANEL_ID_LEN]     = {0};
static int32_t      gPressedTabIndex               = -1; // cosmetic only — see synth_set_pressed_page_tab()

const char * synth_current_page(void) {
    return gCurrentPage;
}

void synth_set_current_page(const char * page) {
    if (page && (page[0] != '\0')) {
        strncpy(gCurrentPage, page, sizeof(gCurrentPage) - 1);
        gCurrentPage[sizeof(gCurrentPage) - 1] = '\0';
        synthlib_request_redraw();
    }
}

uint32_t synth_current_page_sections(tPanelSection * outSections[], uint32_t maxSections) {
    uint32_t count = 0;

    for (uint32_t i = 0; (i < gSynthPanelConfig.sectionCount) && (count < maxSections); i++) {
        tPanelSection * section = &gSynthPanelConfig.sections[i];

        if (!section->hidden && (strcmp(section->page, gCurrentPage) == 0)) {
            outSections[count++] = section;
        }
    }

    return count;
}

int32_t synth_hit_test_page_tab(tCoord coord) {
    for (uint32_t i = 0; i < gPageTabCount; i++) {
        if (within_rectangle(coord, gPageTabs[i].rect)) {
            return (int32_t)i;
        }
    }

    return -1;
}

void synth_action_page_tab(int32_t index) {
    if ((index >= 0) && ((uint32_t)index < gPageTabCount)) {
        synth_set_current_page(gPageTabs[index].page);
    }
}

void synth_set_pressed_page_tab(int32_t index) {
    if (index != gPressedTabIndex) {
        gPressedTabIndex = index;
        synthlib_request_redraw();
    }
}

// notes §2
static tRectangle gPrevPatchRect   = {0};
static tRectangle gNextPatchRect   = {0};
static tRectangle gSyncPatchRect   = {0};
static bool       gPatchNavLaidOut = false; // false until synth_render() has placed the rects at least once
static int32_t    gPressedPatchNav = -1;    // cosmetic only — see synth_set_pressed_patch_nav()

int32_t synth_hit_test_patch_nav(tCoord coord) {
    if (!gPatchNavLaidOut) {
        return -1;
    }

    // notes §3
    if ((gDevice.currentProgram >= 0) && within_rectangle(coord, draw_button_bounds(gPrevPatchRect))) {
        return 0;
    }

    if ((gDevice.currentProgram >= 0) && within_rectangle(coord, draw_button_bounds(gNextPatchRect))) {
        return 1;
    }

    if (within_rectangle(coord, draw_button_bounds(gSyncPatchRect))) {
        return 2;
    }
    return -1;
}

void synth_action_patch_nav(int32_t index) {
    if (index == 0) {
        synth_navigate_preset(-1);
    } else if (index == 1) {
        synth_navigate_preset(1);
    } else if (index == 2) {
        // notes §4
        if (!synth_dump_patch_in_flight()) {
            synth_request_state_dump();
        }
    }
}

void synth_set_pressed_patch_nav(int32_t index) {
    if (index != gPressedPatchNav) {
        gPressedPatchNav = index;
        synthlib_request_redraw();
    }
}

// ── Program name (click-to-edit) ─────────────────────────────────────────────
static tRectangle gProgNameRect    = {0};
static bool       gProgNameLaidOut = false; // false until synth_render() has placed the rect at least once

bool synth_hit_test_prog_name(tCoord coord) {
    return gProgNameLaidOut && within_rectangle(coord, gProgNameRect);
}

// notes §5
static void wrap_name_for_display(const char * flat, uint32_t lineWidth, char * out, size_t outSize) {
    size_t outLen    = 0;
    size_t lineChars = 0;

    for (const char * p = flat; (*p != '\0') && (outLen + 1 < outSize); p++) {
        out[outLen++] = *p;
        lineChars++;

        if ((lineWidth > 0) && (lineChars == lineWidth) && (*(p + 1) != '\0') && (outLen + 1 < outSize)) {
            out[outLen++] = '\n';
            lineChars     = 0;
        }
    }

    out[outLen] = '\0';
}

// notes §6
static void synth_decode_hilo_dial(const tPanelDial * dial, uint32_t rawValue, int32_t * outHigh, int32_t * outLow) {
    uint32_t width       = (dial->dumpBitWidth > 0) ? dial->dumpBitWidth : 16;
    uint32_t half        = 1u << (width - 1);
    int32_t  signedRaw   = (rawValue >= half) ? (int32_t)(rawValue - (half * 2)) : (int32_t)rawValue;
    int32_t  adjusted    = signedRaw - dial->hiLoOffset;
    int32_t  coarse      = (int32_t)dial->hiLoCoarseScale;
    int32_t  fine        = (int32_t)dial->hiLoFineScale;
    int32_t  fineSpan    = (fine > 0) ? (coarse / fine) : 0;            // e.g. 1024/8 = 128 distinct LOW steps per HIGH step
    // notes §7
    int32_t  mod         = (coarse > 0) ? ((adjusted % coarse) + coarse) % coarse : 0;
    int32_t  high        = (coarse > 0) ? (adjusted - mod) / coarse : 0;
    int32_t  lowUnsigned = (fine > 0) ? mod / fine : 0;                  // always in [0, fineSpan-1]

    if ((fineSpan > 0) && (lowUnsigned >= fineSpan / 2)) {
        *outLow  = lowUnsigned - fineSpan;
        *outHigh = high + 1;
    } else {
        *outLow  = lowUnsigned;
        *outHigh = high;
    }
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
    // notes §8
    const double tabHeight = 18.0;
    const double tabGap    = 6.0;
    double       x         = origin.coord.x;

    for (uint32_t i = 0; i < gPageTabCount; i++) {
        char       label[PANEL_LABEL_LEN];

        strncpy(label, gPageTabs[i].page, sizeof(label) - 1);
        label[sizeof(label) - 1] = '\0';

        if (label[0] != '\0') {
            label[0] = (char)toupper((unsigned char)label[0]);
        }
        // notes §9
        double     width   = get_text_width(label, tabHeight, eNoCache); // ~8px padding each side
        tRectangle rect    = {{x, origin.coord.y}, {width, tabHeight}};
        bool       active  = strcmp(gPageTabs[i].page, gCurrentPage) == 0;
        bool       pressed = (int32_t)i == gPressedTabIndex;

        // notes §10
        tRgb       colour  = pressed ? (tRgb)RGB_GREY_5 : (active ? (tRgb)RGB_GREEN_ON : (tRgb)RGB_GREY_7);
        draw_button(mainArea, rect, label, colour);
        // draw_button() draws DRAW_BUTTON_MARGIN larger bottom/right than `rect`
        // and the tab's only other use of this rect is hit-testing — store the
        // true drawn bounds so those edge pixels click (was the small `rect`).
        gPageTabs[i].rect = draw_button_bounds(rect);
        register_click_region(gPageTabs[i].rect, eClickLayerPanel, page_tab_click_handler, (void *)(intptr_t)i);
        x                += width + tabGap;
    }

    return (gPageTabCount > 0) ? (tabHeight + 12.0) : 0.0;
}

// notes §11
#define BUTTON_TEXT_PADDING    10.0

// notes §12
#define MIN_BUTTON_GAP    8.0

// notes §13
static double section_required_spacing(tPanelSection * section) {
    const double textHeight = 12.0; // matches the value/label render_text() calls below
    const double padding    = BUTTON_TEXT_PADDING + MIN_BUTTON_GAP;
    double       required   = section->spacing;

    for (uint32_t i = 0; i < section->dialCount; i++) {
        tPanelDial * dial   = &section->dials[i];
        double       labelW = get_text_width(dial->label, textHeight, eNoCache) + padding;

        if (labelW > required) {
            required = labelW;
        }

        if (dial->display == dialDisplayNames) {
            for (uint32_t n = 0; n < dial->nameCount; n++) {
                double nameW = get_text_width(dial->names[n], textHeight, eNoCache) + padding;

                if (nameW > required) {
                    required = nameW;
                }
            }
        }
    }

    return required;
}

static char gConfigFileName[64] = "z1.txt"; // fallback if the layouts dir can't be scanned at all

static void synth_reload_panel_config(void) {
    char path[1152];

    snprintf(path, sizeof(path), "%s/%s", gLayoutsDir, gConfigFileName);

    if (!load_panel_config(path, &gSynthPanelConfig)) {
        LOG_ERROR("Synth: couldn't load '%s' — dials will not render\n", path);
    }
    // The MIDI ports chosen for THIS device, which may be on a different interface from the last one
    // - see midi_set_port_scope(). Here rather than in synth_switch_device_config() because this is
    // the one path every configuration load takes, the first at start-up included.
    midi_set_port_scope(gConfigFileName);
    // notes §14
    gCurrentPage[0] = '\0';

    // notes §15
    if (gSynthPanelConfig.stateRequestSysExLen > 3) {
        gDevice.moogDeviceId = gSynthPanelConfig.stateRequestSysEx[3];
    }
    // notes §16
    synth_backup_reload_name_cache_for_device();
    synthlib_request_redraw();
}

// notes §17
static tPanelConfigCandidate sPendingChooserCandidates[PANEL_MAX_CANDIDATES];
static uint32_t              sPendingChooserCount = 0;

// notes §18
static void on_startup_device_chosen(bool confirmed, uint32_t bank1Indexed, uint32_t location1Indexed) {
    (void)bank1Indexed;

    if (confirmed) {
        uint32_t index = location1Indexed - 1;

        if (index < sPendingChooserCount) {
            strncpy(gConfigFileName, sPendingChooserCandidates[index].filename, sizeof(gConfigFileName) - 1);
            gConfigFileName[sizeof(gConfigFileName) - 1] = '\0';
            set_saved_device_config(gConfigFileName);
        }
    }
    // notes §19
    synth_reload_panel_config();
}

// notes §20
static bool synth_choose_config_file(void) {
    tPanelConfigCandidate candidates[PANEL_MAX_CANDIDATES];
    uint32_t              count = scan_panel_configs(gLayoutsDir, candidates, PANEL_MAX_CANDIDATES);

    if (count == 0) {
        prompt_choose_layouts_folder();
        return true;
    }

    if (count == 1) {
        strncpy(gConfigFileName, candidates[0].filename, sizeof(gConfigFileName) - 1);
        gConfigFileName[sizeof(gConfigFileName) - 1] = '\0';
        set_saved_device_config(gConfigFileName);
        return true;
    }
    const char *          saved = get_saved_device_config();

    if (saved) {
        for (uint32_t i = 0; i < count; i++) {
            if (strcmp(candidates[i].filename, saved) == 0) {
                strncpy(gConfigFileName, saved, sizeof(gConfigFileName) - 1);
                gConfigFileName[sizeof(gConfigFileName) - 1] = '\0';
                set_saved_device_config(gConfigFileName);
                return true;
            }
        }
    }
    tBankBrowserItem      items[PANEL_MAX_CANDIDATES];
    char                  labelBuf[PANEL_MAX_CANDIDATES][200];

    sPendingChooserCount = count;

    for (uint32_t i = 0; i < count; i++) {
        sPendingChooserCandidates[i] = candidates[i];

        if (candidates[i].description[0] != '\0') {
            snprintf(labelBuf[i], sizeof(labelBuf[i]), "%s - %s", candidates[i].deviceName, candidates[i].description); // ASCII hyphen - a UTF-8 em dash renders as "???", see appMenuBar.c's own comment
        } else {
            snprintf(labelBuf[i], sizeof(labelBuf[i]), "%s", candidates[i].deviceName);
        }
        // notes §21
        items[i]                     = (tBankBrowserItem){
            labelBuf[i], 0, 1, i + 1
        };
    }

    open_bank_browser("Choose Device", "More than one device configuration was found in the layouts folder.",
                      "Choose", items, count, NULL, 0, on_startup_device_chosen);
    return false;
}

void synth_set_layouts_dir(const char * dir) {
    if (dir && (dir[0] != '\0')) {
        strncpy(gLayoutsDir, dir, sizeof(gLayoutsDir) - 1);
        gLayoutsDir[sizeof(gLayoutsDir) - 1] = '\0';
    }

    if (synth_choose_config_file()) {
        synth_reload_panel_config();
    }
}

const char * synth_layouts_dir(void) {
    return gLayoutsDir;
}

const char * synth_current_device_config(void) {
    return gConfigFileName;
}

void synth_switch_device_config(const char * filename) {
    if (!filename || (filename[0] == '\0') || (strcmp(filename, gConfigFileName) == 0)) {
        return;
    }
    strncpy(gConfigFileName, filename, sizeof(gConfigFileName) - 1);
    gConfigFileName[sizeof(gConfigFileName) - 1] = '\0';
    synth_reload_panel_config();
    set_saved_device_config(gConfigFileName);
    // notes §22
    midi_request_reconnect();
    synthlib_request_redraw();
}

void synth_init_graphics(void) {
    const char * saved = get_saved_layouts_dir();

    if (saved) {
        strncpy(gLayoutsDir, saved, sizeof(gLayoutsDir) - 1);
        gLayoutsDir[sizeof(gLayoutsDir) - 1] = '\0';
    }

    if (synth_choose_config_file()) {
        synth_reload_panel_config();
    }
}

// notes §23
static void synth_render_sweep_status_row(void) {
    uint32_t current     = 0;
    uint32_t total       = 0;
    uint32_t actionCount = 0;

    if (  !synth_backup_get_export_progress(&current, &total, &actionCount)
       || !synth_backup_export_progress_is_name_sweep()) {
        return;
    }
    double   renderW     = get_render_width() / gGlobalGuiScale;
    double   renderH     = get_render_height() / gGlobalGuiScale;
    double   margin      = 10.0;
    double   textH       = 12.0; // matches the nav buttons' own text height, so the row sits level
    double   barW        = 160.0;
    double   barH        = 6.0;
    unsigned pct         = (total > 0) ? (unsigned)(((uint64_t)current * 100) / total) : 0;
    char     lineBuf[80];

    snprintf(lineBuf, sizeof(lineBuf), "Fetching preset names... %u%% (%u of %u, %u found)",
             pct, (unsigned)current, (unsigned)total, (unsigned)actionCount);

    // notes §24
    double   textX       = margin;
    double   textY       = renderH - margin - textH;

    if (gPatchNavLaidOut) {
        textX = gSyncPatchRect.coord.x + gSyncPatchRect.size.w + 24.0;
        textY = gSyncPatchRect.coord.y;
    }
    double   textW       = get_text_width(lineBuf, textH, eNoCache);
    double   barX        = textX + textW + 10.0;
    double   available   = renderW - margin - barX;

    set_rgb_colour((tRgb)RGB_GREY_7);
    render_text(mainArea, (tRectangle){{textX, textY}, {textW, textH}}, lineBuf);

    // Drop the bar rather than let it run off the right edge on a narrow window - the percentage is
    // already in the text, so the bar is the redundant half of the pair.
    if (available >= 40.0) {
        double barY = textY + ((textH - barH) / 2.0);

        barW = fmin(barW, available);
        set_rgb_colour((tRgb)RGB_GREY_2);
        render_rectangle(mainArea, (tRectangle){{barX, barY}, {barW, barH}});
        set_rgb_colour((tRgb)RGB_GREEN_ON);
        render_rectangle(mainArea, (tRectangle){{barX, barY}, {barW * ((double)pct / 100.0), barH}});
    }
}

static void synth_render_backup_progress(void) {
    uint32_t     current     = 0;
    uint32_t     total       = 0;
    uint32_t     actionCount = 0;
    const char * title;
    const char * verb;

    if (synth_backup_get_export_progress(&current, &total, &actionCount)) {
        if (synth_backup_export_progress_is_name_sweep()) {
            // Handled by synth_render_sweep_status_row() instead — see that
            // function's own comment. Added 2026-07-14.
            return;
        } else {
            title = "Backing Up Bank";
            verb  = "written";
        }
    } else if (synth_backup_get_restore_progress(&current, &total, &actionCount)) {
        title = "Restoring Bank";
        verb  = "sent";
    } else {
        return;
    }
    double       renderW     = get_render_width() / gGlobalGuiScale;
    double       renderH     = get_render_height() / gGlobalGuiScale;
    double       boxW        = 360.0;
    double       boxH        = 90.0;
    double       boxX        = (renderW - boxW) / 2.0;
    double       boxY        = (renderH - boxH) / 2.0;
    double       margin      = 10.0;
    double       titleH      = 24.0;

    // Background overlay to de-emphasise content beneath the dialog
    set_rgb_colour((tRgb)RGB_GREY_2);
    render_rectangle(mainArea, (tRectangle){{0.0, 0.0}, {renderW, renderH}});

    // Dialog box
    set_rgb_colour((tRgb)RGB_GREY_5);
    render_rectangle_with_border(mainArea, (tRectangle){{boxX, boxY}, {boxW, boxH}});

    // Title bar
    set_rgb_colour((tRgb)RGB_GREY_3);
    render_rectangle(mainArea, (tRectangle){{boxX, boxY}, {boxW, titleH}});
    set_rgb_colour((tRgb)RGB_BLACK);
    render_text(mainArea, (tRectangle){{boxX + margin, boxY + 6.0}, {BLANK_SIZE, STANDARD_TEXT_HEIGHT}}, title);

    char         lineBuf[128];

    snprintf(lineBuf, sizeof(lineBuf), "Preset %u of %u", (unsigned)current, (unsigned)total);
    set_rgb_colour((tRgb)RGB_WHITE);
    render_text(mainArea, (tRectangle){{boxX + margin, boxY + titleH + margin}, {BLANK_SIZE, STANDARD_TEXT_HEIGHT}}, lineBuf);

    snprintf(lineBuf, sizeof(lineBuf), "%u %s so far", (unsigned)actionCount, verb);
    render_text(mainArea, (tRectangle){{boxX + margin, boxY + titleH + margin + STANDARD_TEXT_HEIGHT + 6.0}, {BLANK_SIZE, STANDARD_TEXT_HEIGHT}}, lineBuf);

    // Progress bar
    double       barY = boxY + boxH - margin - 8.0;
    double       barW = boxW - margin * 2.0;
    double       frac = (total > 0) ? ((double)current / (double)total) : 0.0;

    // notes §25
    set_rgb_colour((tRgb)RGB_GREY_2);
    render_rectangle(mainArea, (tRectangle){{boxX + margin, barY}, {barW, 8.0}});
    set_rgb_colour((tRgb)RGB_GREEN_ON);
    render_rectangle(mainArea, (tRectangle){{boxX + margin, barY}, {barW * frac, 8.0}});
}

void synth_render(tRectangle area) {
    double x = area.coord.x + 30.0;
    double y = area.coord.y + 20.0;

    // ── Page tabs ──────────────────────────────────────────────────────────────
    y += render_page_tabs((tRectangle){{x, y}, {0, 0}});

    // notes §26
    {
        // notes §27
        char nameBuf[SYNTH_PROG_NAME_MAXLEN * 2]; // headroom for the inserted cursor + wrap '\n's

        if (gProgNameEdit.active) {
            char   withCursor[SYNTH_PROG_NAME_MAXLEN + 1];
            size_t len = strlen(gProgNameEdit.buffer);
            size_t cp  = (gProgNameEdit.cursorPos <= len) ? gProgNameEdit.cursorPos : len;

            memcpy(withCursor, gProgNameEdit.buffer, cp);
            withCursor[cp] = '|';
            memcpy(&withCursor[cp + 1], &gProgNameEdit.buffer[cp], len - cp + 1);
            wrap_name_for_display(withCursor, synth_panel_config()->nameLineWidth, nameBuf, sizeof(nameBuf));
            set_rgb_colour((tRgb)RGB_GREEN_ON); // flags edit mode, same colour render_page_tabs() uses for "active"
        } else {
            const char * nm;

            if (gDevice.progName[0] != '\0') {
                nm = gDevice.progName;
            } else if (gDevice.connected) {
                nm = synth_panel_config()->deviceName;
            } else {
                nm = "Not connected";
            }
            strncpy(nameBuf, nm, sizeof(nameBuf) - 1);
            nameBuf[sizeof(nameBuf) - 1] = '\0';
            set_rgb_colour((tRgb)RGB_WHITE);
        }
        tPanelConfig * cfg          = synth_panel_config();
        uint32_t       maxFieldLen  = (cfg->panelNameLen > cfg->presetNameLen) ? cfg->panelNameLen : cfg->presetNameLen;
        uint32_t       reservedRows = (cfg->nameLineWidth > 0)
                                     ? ((maxFieldLen + cfg->nameLineWidth - 1) / cfg->nameLineWidth)
                                     : 1;

        if (reservedRows == 0) {
            reservedRows = 1;
        }
        char *         line         = strtok(nameBuf, "\n");
        uint32_t       row          = 0;

        while ((line != NULL) && (row < reservedRows)) {
            tRectangle r = {{x, y + (row * 32.0)}, {450.0, 26.0}};

            render_text(mainArea, r, line);
            line = strtok(NULL, "\n");
            row++;
        }
        gProgNameRect    = (tRectangle){{
                                            x, y
                                        }, {
                                            450.0, 32.0 * (double)reservedRows
                                        }
        };
        gProgNameLaidOut = true;
        register_click_region(gProgNameRect, eClickLayerPanel, prog_name_click_handler, NULL);

        // notes §28
        bool         navEnabled      = gDevice.connected;
        bool         prevNextEnabled = navEnabled && (gDevice.currentProgram >= 0);
        // notes §29
        const double navBtnHeight    = 12.0;
        double       prevWidth       = get_text_width("< Prev", navBtnHeight, eNoCache);
        double       nextWidth       = get_text_width("Next >", navBtnHeight, eNoCache);
        double       syncWidth       = get_text_width("Sync from synth", navBtnHeight, eNoCache);
        tRgb         prevColour      = (gPressedPatchNav == 0) ? (tRgb)RGB_GREY_5 : (prevNextEnabled ? (tRgb)RGB_GREY_7 : (tRgb)RGB_GREY_3);
        tRgb         nextColour      = (gPressedPatchNav == 1) ? (tRgb)RGB_GREY_5 : (prevNextEnabled ? (tRgb)RGB_GREY_7 : (tRgb)RGB_GREY_3);
        tRgb         syncColour      = (gPressedPatchNav == 2) ? (tRgb)RGB_GREY_5 : (navEnabled ? (tRgb)RGB_GREY_7 : (tRgb)RGB_GREY_3);

        gPrevPatchRect   = (tRectangle){{
                                            x + 460.0, y
                                        }, {
                                            prevWidth, navBtnHeight
                                        }
        };
        gNextPatchRect   = (tRectangle){{
                                            x + 460.0 + prevWidth + 12.0, y
                                        }, {
                                            nextWidth, navBtnHeight
                                        }
        };
        gSyncPatchRect   = (tRectangle){{
                                            x + 460.0 + prevWidth + 12.0 + nextWidth + 24.0, y
                                        }, {
                                            syncWidth, navBtnHeight
                                        }
        };
        draw_button(mainArea, gPrevPatchRect, "< Prev", prevColour);
        draw_button(mainArea, gNextPatchRect, "Next >", nextColour);
        draw_button(mainArea, gSyncPatchRect, "Sync from synth", syncColour);
        gPatchNavLaidOut = true;

        // notes §30
        if (prevNextEnabled) {
            // draw_button_bounds(): register the true drawn rect (see draw_button),
            // matching synth_hit_test_patch_nav()'s own wrapped checks above.
            register_click_region(draw_button_bounds(gPrevPatchRect), eClickLayerPanel, patch_nav_click_handler, (void *)(intptr_t)0);
            register_click_region(draw_button_bounds(gNextPatchRect), eClickLayerPanel, patch_nav_click_handler, (void *)(intptr_t)1);
        }

        if (navEnabled) {
            register_click_region(draw_button_bounds(gSyncPatchRect), eClickLayerPanel, patch_nav_click_handler, (void *)(intptr_t)2);
        }
        y               += 32.0 * (double)reservedRows;
    }

    // notes §31
    {
        const double rowHeight   = 13.0;
        const double lineAdvance = 18.0;                              // vertical gap between wrapped Info Row lines
        const double rightEdge   = area.coord.x + area.size.w - 20.0; // small right margin
        double       ix          = x;
        double       rowY        = y;
        bool         first       = true;

        set_rgb_colour((tRgb)RGB_GREY_7);

        for (uint32_t s = 0; s < gSynthPanelConfig.sectionCount; s++) {
            tPanelSection * section = &gSynthPanelConfig.sections[s];

            if (!section->hidden) {
                continue;
            }

            for (uint32_t d = 0; d < section->dialCount; d++) {
                tPanelDial * dial     = &section->dials[d];
                uint32_t     dialVal  = get_panel_dial_value(dial);
                char         valStr[32];

                if (dial->display == dialDisplayNames) {
                    snprintf(valStr, sizeof(valStr), "%s", (dialVal < dial->nameCount) ? dial->names[dialVal] : "?");
                } else {
                    snprintf(valStr, sizeof(valStr), "%u", (unsigned)dialVal);
                }
                char         pair[64];
                snprintf(pair, sizeof(pair), "%s: %s", dial->label, valStr);

                double       width    = get_text_width(pair, rowHeight, eNoCache);
                const char * sep      = "  |  ";
                double       sepWidth = get_text_width(sep, rowHeight, eNoCache);
                double       needed   = first ? width : (sepWidth + width);

                if (!first && ((ix + needed) > rightEdge)) {
                    ix    = x;
                    rowY += lineAdvance;
                    first = true; // no leading separator right after a wrap
                }

                if (!first) {
                    tRectangle sepRect = (tRectangle){{
                                                          ix, rowY
                                                      }, {
                                                          0.0, rowHeight
                                                      }
                    };

                    render_text(mainArea, sepRect, sep);
                    ix += sepWidth;
                }
                first      = false;

                dial->rect = (tRectangle){{
                                              ix, rowY
                                          }, {
                                              width, rowHeight
                                          }
                };
                render_text(mainArea, dial->rect, pair);
                // notes §32
                register_click_region(dial->rect, eClickLayerPanel, panel_dial_press_click_handler, dial);
                ix        += width;
            }
        }

        y = rowY + 25.0;
    }

    // notes §33
    {
        tPanelSection * sections[PANEL_MAX_SECTIONS];
        uint32_t        sectionCount = synth_current_page_sections(sections, PANEL_MAX_SECTIONS);
        tPanelConfig *  cfg          = synth_panel_config();
        bool            pageIsGrid   = false;

        for (uint32_t sIdx = 0; (sIdx < sectionCount) && !pageIsGrid; sIdx++) {
            for (uint32_t i = 0; i < sections[sIdx]->dialCount; i++) {
                if (sections[sIdx]->dials[i].gridCol >= 0) {
                    pageIsGrid = true;
                    break;
                }
            }
        }

        // notes §34
        if (pageIsGrid && (cfg->gridColWidth > 0.0)) {
            const double defaultHeaderHeight = 14.0;
            const double padding             = 8.0; // clearance so one column's text/rule doesn't touch the next
            double       headerHeight        = defaultHeaderHeight;
            bool         anyLabel            = false;

            // notes §35
            for (uint32_t li = 0; li < cfg->columnLabelCount; li++) {
                tColumnLabel * columnLabel  = &cfg->columnLabels[li];

                if (strcmp(columnLabel->page, gCurrentPage) != 0) {
                    continue;
                }
                anyLabel                 = true;

                char           upper[PANEL_LABEL_LEN];
                strncpy(upper, columnLabel->label, sizeof(upper) - 1);
                upper[sizeof(upper) - 1] = '\0';

                for (char * p = upper; *p != '\0'; p++) {
                    *p = (char)toupper((unsigned char)*p);
                }

                double         naturalWidth = get_text_width(upper, defaultHeaderHeight, eNoCache);
                double         available    = cfg->gridColWidth - padding;

                if ((naturalWidth > available) && (naturalWidth > 0.0)) {
                    double fitHeight = defaultHeaderHeight * (available / naturalWidth);

                    if (fitHeight < headerHeight) {
                        headerHeight = fitHeight;
                    }
                }
            }

            // Second pass: actually draw, at the size settled on above.
            for (uint32_t li = 0; li < cfg->columnLabelCount; li++) {
                tColumnLabel * columnLabel = &cfg->columnLabels[li];

                if (strcmp(columnLabel->page, gCurrentPage) != 0) {
                    continue;
                }
                char           upper[PANEL_LABEL_LEN];
                strncpy(upper, columnLabel->label, sizeof(upper) - 1);
                upper[sizeof(upper) - 1] = '\0';

                for (char * p = upper; *p != '\0'; p++) {
                    *p = (char)toupper((unsigned char)*p);
                }

                double         colX        = x + ((double)columnLabel->col * cfg->gridColWidth);
                tRectangle     labelRect   = (tRectangle){{
                                                              colX, y
                                                          }, {
                                                              cfg->gridColWidth, headerHeight
                                                          }
                };
                set_rgb_colour((tRgb)RGB_WHITE);
                render_text(mainArea, labelRect, upper);

                set_rgb_colour((tRgb)RGB_GREY_5);
                render_line(mainArea, (tCoord){colX, y + headerHeight + 2.0}, (tCoord){colX + cfg->gridColWidth - padding, y + headerHeight + 2.0}, 1.0);
            }

            if (anyLabel) {
                y += headerHeight + 8.0;
            }
        }

        for (uint32_t sIdx = 0; sIdx < sectionCount; sIdx++) {
            tPanelSection * section = sections[sIdx];

            section->spacing = section_required_spacing(section);
            layout_panel_section(section, (tRectangle){{x, y}, {0, 0}}, cfg->gridColWidth, cfg->gridRowHeight);

            for (uint32_t i = 0; i < section->dialCount; i++) {
                tPanelDial * dial     = &section->dials[i];
                uint32_t     dialVal  = get_panel_dial_value(dial);
                // notes §36
                bool         disabled = panel_dial_is_disabled(dial, cfg);
                // notes §37
                double       baseY    = dial->rect.coord.y;
                // notes §38
                bool         isToggle = panel_dial_is_toggle(dial) && !dial->asMenu;

                // notes §39
                if (panel_dial_is_binary(dial) || panel_dial_needs_value_menu(dial)) {
                    // notes §40
                    const char * name = isToggle ? dial->label
                                : (dialVal < dial->nameCount) ? dial->names[dialVal]
                                                                                     : "?";

                    // notes §41
                    const double buttonHeight = 12.0;
                    const double padding      = BUTTON_TEXT_PADDING; // must match section_required_spacing()'s own use of this constant — see its comment
                    double       widest       = 0.0;

                    if (isToggle) {
                        widest = get_text_width(dial->label, buttonHeight, eNoCache);
                    } else {
                        for (uint32_t n = 0; n < dial->nameCount; n++) {
                            double w = get_text_width(dial->names[n], buttonHeight, eNoCache);

                            if (w > widest) {
                                widest = w;
                            }
                        }
                    }
                    dial->rect.coord.y += (section->dialSize - buttonHeight) / 2.0;
                    dial->rect.size     = (tSize){
                        widest + padding, buttonHeight
                    };

                    // notes §42
                    tRgb colour = disabled ? (tRgb)RGB_GREY_3
                                : (isToggle && (dialVal != 0)) ? (tRgb)RGB_GREEN_ON
                                : panel_dial_needs_value_menu(dial) ? dial->colour
                                : (tRgb)RGB_GREY_7;
                    draw_button(mainArea, dial->rect, name, colour);
                } else {
                    render_dial(mainArea, dial->rect, dialVal, dial->max, 0, disabled ? (tRgb)RGB_GREY_3 : dial->colour);
                }

                // notes §43
                if (!disabled && !dial->readOnly) {
                    // notes §44
                    register_click_region(panel_dial_hit_rect(dial), eClickLayerPanel,
                                          panel_dial_press_click_handler, dial);
                }
                char valBuf[48]; // 24 was enough for every existing display mode, but not dialDisplaySignedHiLo's "High: N (or M)  Low: K" text (see that branch's own comment below)

                if (dial->display == dialDisplayNames) {
                    snprintf(valBuf, sizeof(valBuf), "%s",
                             (dialVal < dial->nameCount) ? dial->names[dialVal] : "?");
                } else if (dial->display == dialDisplayRaw) {
                    snprintf(valBuf, sizeof(valBuf), "%u", (unsigned)dialVal);
                } else if (dial->display == dialDisplaySignedHiLo) {
                    int32_t high      = 0;
                    int32_t low       = 0;

                    synth_decode_hilo_dial(dial, dialVal, &high, &low);
                    // notes §45
                    int32_t highAlias = (high < 0) ? (high + 64) : (high - 64);

                    snprintf(valBuf, sizeof(valBuf), "High: %d (or %d)  Low: %d", (int)high, (int)highAlias, (int)low);
                } else if (dial->display == dialDisplayNote) {
                    // See dialDisplayNote's own comment in panelConfig.h —
                    // dialVal IS the MIDI note number already (0-127), just
                    // formatted as a note name instead of a bare integer.
                    static const char * kNoteNames[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
                    int32_t             octave         = ((int32_t)dialVal / 12) - 1;

                    snprintf(valBuf, sizeof(valBuf), "%s%d", kNoteNames[dialVal % 12], (int)octave);
                } else if (dial->display == dialDisplaySigned) {
                    // notes §46
                    snprintf(valBuf, sizeof(valBuf), "%d", (int)dialVal - dial->displayOffset);
                } else if (dial->display == dialDisplayCcNative) {
                    // notes §47
                    uint32_t nativeShown = (dial->dumpNativeMax != 0)
                                ? ((dial->max > 1)
                                   ? ((dialVal * dial->dumpNativeMax) + ((dial->max - 1) / 2)) / (dial->max - 1)
                                   : 0)
                                : get_panel_dial_native_value(dial);

                    snprintf(valBuf, sizeof(valBuf), "%u (%u)", (unsigned)dialVal, (unsigned)nativeShown);
                } else {
                    snprintf(valBuf, sizeof(valBuf), "%u (%u)", (unsigned)dialVal,
                             (unsigned)get_panel_dial_native_value(dial));
                }

                // notes §48
                if (!panel_dial_is_binary(dial) && !panel_dial_needs_value_menu(dial)) {
                    tRectangle valRect = (tRectangle){{
                                                          dial->rect.coord.x, baseY + section->dialSize + 4.0
                                                      },
                                                      {
                                                          section->spacing, 12.0
                                                      }
                    };
                    set_rgb_colour((tRgb)RGB_GREY_7);
                    render_text(mainArea, valRect, valBuf);
                }
                // notes §49
                bool   isSelfExplanatoryButton = (panel_dial_is_binary(dial) || panel_dial_needs_value_menu(dial))
                                                 && dial->noLabel;
                // notes §50
                bool   isButtonDial            = panel_dial_is_binary(dial) || panel_dial_needs_value_menu(dial)
                                                 || panel_dial_is_toggle(dial);
                double lblY                    = baseY + section->dialSize + (isButtonDial ? 4.0 : 18.0);

                // notes §51
                if (!isToggle && !isSelfExplanatoryButton) {
                    tRectangle lblRect = (tRectangle){{
                                                          dial->rect.coord.x, lblY
                                                      },
                                                      {
                                                          section->spacing, 12.0
                                                      }
                    };

                    // notes §52
                    set_rgb_colour((tRgb)RGB_GREY_7);
                    render_text(mainArea, lblRect, dial->label);
                }
            }

            if (!pageIsGrid) {
                y += section->dialSize + 46.0; // dial + value/label text + gap before next stacked section
            }
        }
    }
    // Drawn last so it paints over everything else this function just laid
    // out — a no-op unless a bulk Backup/Restore sweep is actually running.
    synth_render_backup_progress();
    // Same "drawn last" reasoning — a no-op unless a name sweep (background
    // prefetch or explicit Load/Store click) is actually running.
    synth_render_sweep_status_row();
}

#ifdef __cplusplus
}
#endif
