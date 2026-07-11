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
#include "fileDialogue.h"
#include "synthGraphics.h"

#define SYNTH_LAYOUTS_DIR_DEFAULT    "layouts"    // relative to cwd, used until a folder is chosen/persisted
#define SYNTH_MAX_PAGE_TABS          PANEL_MAX_SECTIONS

static char         gLayoutsDir[1024]              = SYNTH_LAYOUTS_DIR_DEFAULT;

static tPanelConfig gSynthPanelConfig              = {0};

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
static int32_t      gPressedTabIndex               = -1; // cosmetic only — see synth_set_pressed_page_tab()

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
        gReDraw          = true;
    }
}

// ── Prev/Next/Sync patch buttons ─────────────────────────────────────────────
// See synth_navigate_preset() (synthComms.h) for what a Prev/Next press
// actually does. Laid out on the Program name row (synth_render() below)
// rather than as a separate row, since they act on "whatever's currently
// loaded" — the same thing that row already shows.
//
// Sync (index 2) isn't preset navigation — it re-requests the current state
// dump (synth_request_state_dump(), synthComms.c) so the app's dial values
// can be checked/refreshed against the real hardware's current front-panel
// settings on demand, rather than only ever updating on connect or a preset
// change. Added 2026-07-08 for exactly this kind of manual verification.
// Shares the same generic press-on-mouse-up index scheme as Prev/Next purely
// because it needed nothing more — mouseHandle.c dispatches by index alone
// and doesn't care how many buttons are in this row.
//
// Labelled "Sync from synth", not just "Sync" — renamed 2026-07-10 after a
// real hardware finding: ANY state dump request (this button, the CC-quiet
// auto-refresh, or an abandoned periodic-poll experiment the same day — see
// midi_arm_state_dump_debounce()'s own comment in midiComms.c) kicks the
// Voyager's own front-panel display OUT of whatever menu it's showing (e.g.
// browsing Sound Category) back to normal. A periodic background poll doing
// that as a surprise was unacceptable and got removed; this button doing it
// is fine BECAUSE it's the owner's own deliberate, explicit request — the
// more specific label is so it's obvious in the UI that pressing it talks
// to the hardware and can interrupt whatever it's showing, not just a
// harmless in-app refresh.
static tRectangle gPrevPatchRect   = {0};
static tRectangle gNextPatchRect   = {0};
static tRectangle gSyncPatchRect   = {0};
static bool       gPatchNavLaidOut = false; // false until synth_render() has placed the rects at least once
static int32_t    gPressedPatchNav = -1;    // cosmetic only — see synth_set_pressed_patch_nav()

int32_t synth_hit_test_patch_nav(tCoord coord) {
    if (!gPatchNavLaidOut) {
        return -1;
    }

    if (within_rectangle(coord, gPrevPatchRect)) {
        return 0;
    }

    if (within_rectangle(coord, gNextPatchRect)) {
        return 1;
    }

    if (within_rectangle(coord, gSyncPatchRect)) {
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
        synth_request_state_dump();
    }
}

void synth_set_pressed_patch_nav(int32_t index) {
    if (index != gPressedPatchNav) {
        gPressedPatchNav = index;
        gReDraw          = true;
    }
}

// ── Program name (click-to-edit) ─────────────────────────────────────────────
static tRectangle gProgNameRect    = {0};
static bool       gProgNameLaidOut = false; // false until synth_render() has placed the rect at least once

bool synth_hit_test_prog_name(tCoord coord) {
    return gProgNameLaidOut && within_rectangle(coord, gProgNameRect);
}

// ── Info row hit-test ─────────────────────────────────────────────────────────
// Mirrors synth_render()'s own Info Row iteration (every dial in a `hidden`
// section, anywhere in the config) rather than synth_current_page_sections()
// above, which deliberately excludes hidden sections — the Info Row is drawn
// on every page, not scoped to gCurrentPage, so its hit-test can't be either.
tPanelDial * synth_hit_test_info_row(tCoord coord) {
    for (uint32_t s = 0; s < gSynthPanelConfig.sectionCount; s++) {
        tPanelSection * section = &gSynthPanelConfig.sections[s];

        if (!section->hidden) {
            continue;
        }

        for (uint32_t d = 0; d < section->dialCount; d++) {
            tPanelDial * dial = &section->dials[d];

            if (within_rectangle(coord, dial->rect)) {
                return dial;
            }
        }
    }

    return NULL;
}

// Inserts '\n' every lineWidth characters of `flat` for display — the same
// display-only wrap extract_moog_name() applies when decoding a name off the
// wire (see that function's own comment for why there's no real separator
// byte to hang a break on), applied here to an in-progress edit buffer
// instead of freshly-decoded bytes. Deliberately simpler than
// extract_moog_name(): a user-typed buffer has no whitespace-collapsing or
// hardware line-boundary quirk to undo.
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
        double     width   = get_text_width(label, tabHeight, eNoCache); // ~8px padding each side
        tRectangle rect    = {{x, origin.coord.y}, {width, tabHeight}};
        bool       active  = strcmp(gPageTabs[i].page, gCurrentPage) == 0;
        bool       pressed = (int32_t)i == gPressedTabIndex;

        // RGB_GREY_7, not RGB_BACKGROUND_GREY — the latter is a dark grey in
        // this build, too low-contrast for draw_button's fixed black text.
        // Pressed (mouse currently down on this tab) takes priority over
        // active — it's transient feedback for the click in progress, shown
        // regardless of which page is actually selected right now.
        tRgb       colour  = pressed ? (tRgb)RGB_GREY_5 : (active ? (tRgb)RGB_GREEN_ON : (tRgb)RGB_GREY_7);
        draw_button(mainArea, rect, label, colour);
        gPageTabs[i].rect = rect;
        x                += width + tabGap;
    }

    return (gPageTabCount > 0) ? (tabHeight + 12.0) : 0.0;
}

// Widens a section's spacing (floor: whatever dialSize/spacing the file gave
// it) to fit its own widest label or discrete-name text — render_text() draws
// unclipped by rectangle width (see internal_render_text() in
// utilsGraphics.cpp), so anything wider than the column it's given visually
// bleeds into the next dial's text. Generic: works for any section from any
// device config, not a fixed value tuned for one device's longest string.
// Idempotent — recomputing against an already-widened spacing yields the
// same result, so calling this every frame is fine.
static double section_required_spacing(tPanelSection * section) {
    const double textHeight = 12.0; // matches the value/label render_text() calls below
    const double padding    = 8.0;
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
    gReDraw = true;
}

// Scans gLayoutsDir for every <device>.txt it contains; a single match is
// used directly (no prompt — nothing to choose), but more than one presents
// a chooser (device name + description, from each file's own "device"/
// "description" lines) and switches gConfigFileName to whichever was picked.
// If the user cancels, whatever gConfigFileName already held is left alone
// rather than blocking startup.
//
// Zero candidates (no folder ever chosen, or the saved one moved/was
// emptied) instead puts up the same folder picker as the "Choose Layouts
// Folder…" menu item, asynchronously — synth_set_layouts_dir() re-enters
// this whole function once the user actually picks something.
static void synth_choose_config_file(void) {
    tPanelConfigCandidate candidates[PANEL_MAX_CANDIDATES];
    uint32_t              count = scan_panel_configs(gLayoutsDir, candidates, PANEL_MAX_CANDIDATES);

    if (count == 0) {
        prompt_choose_layouts_folder();
        return;
    }

    if (count == 1) {
        strncpy(gConfigFileName, candidates[0].filename, sizeof(gConfigFileName) - 1);
        gConfigFileName[sizeof(gConfigFileName) - 1] = '\0';
        return;
    }
    char                  labelBuf[PANEL_MAX_CANDIDATES][200];
    const char *          labels[PANEL_MAX_CANDIDATES];

    for (uint32_t i = 0; i < count; i++) {
        if (candidates[i].description[0] != '\0') {
            snprintf(labelBuf[i], sizeof(labelBuf[i]), "%s \xe2\x80\x94 %s", candidates[i].deviceName, candidates[i].description);
        } else {
            snprintf(labelBuf[i], sizeof(labelBuf[i]), "%s", candidates[i].deviceName);
        }
        labels[i] = labelBuf[i];
    }

    int32_t               chosen = show_device_choice_dialogue("Choose Device",
                                                               "More than one device configuration was found in the layouts folder.",
                                                               labels, count);

    if (chosen >= 0) {
        strncpy(gConfigFileName, candidates[(uint32_t)chosen].filename, sizeof(gConfigFileName) - 1);
        gConfigFileName[sizeof(gConfigFileName) - 1] = '\0';
    }
}

void synth_set_layouts_dir(const char * dir) {
    if (dir && (dir[0] != '\0')) {
        strncpy(gLayoutsDir, dir, sizeof(gLayoutsDir) - 1);
        gLayoutsDir[sizeof(gLayoutsDir) - 1] = '\0';
    }
    synth_choose_config_file();
    synth_reload_panel_config();
}

void synth_init_graphics(void) {
    const char * saved = get_saved_layouts_dir();

    if (saved) {
        strncpy(gLayoutsDir, saved, sizeof(gLayoutsDir) - 1);
        gLayoutsDir[sizeof(gLayoutsDir) - 1] = '\0';
    }
    synth_choose_config_file();
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
    // "(loading…)" only means anything while we're still waiting on the
    // device's first dump reply. Some protocols never send a program name at
    // all (e.g. the Voyager's Panel Dump — see extract_moog_panel_info() in
    // synthComms.c), so once connected an empty progName isn't "still
    // loading", it's "this device doesn't report one" — show the device name
    // instead rather than a permanently-wrong loading message.
    //
    // gDevice.progName may contain embedded '\n's (nameLineWidth in the
    // device's .txt — see the tPanelConfig field comment in panelConfig.h),
    // one per line of the source device's own multi-line display — rendered
    // here as separate rows rather than run together on one line. The space
    // reserved below is always the device's worst-case line count (e.g. 2
    // for the Voyager, whether or not the current name actually uses both),
    // not however many lines happen to be in nm right now — otherwise every
    // row beneath this one (dials, Info row, …) would shift up and down as
    // patches with a one-line vs. two-line name are selected.
    {
        // Click-to-edit (see synth_hit_test_prog_name() above, mouseHandle.c
        // for the click/keyboard handling) — while active, render the edit
        // buffer (with an inserted '|' cursor marker, same idiom G2-Edit's
        // own patch-name field uses) instead of gDevice.progName, wrapped
        // for display the same way a decoded name is (wrap_name_for_display()
        // above) so it visually matches the read-only view it'll return to
        // on commit/cancel.
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
                nm = "(loading\xe2\x80\xa6)";
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
        gProgNameRect    = {{x, y}, {450.0, 32.0 * (double)reservedRows}};
        gProgNameLaidOut = true;

        // Prev/Next patch buttons — see synth_navigate_preset() (synthComms.h)
        // for what they send (always something, even before a current
        // program number is known — see the comment in synth_navigate_preset()
        // for why guessing beats silently refusing to click). Greyed out
        // only while there's no device to send to at all. Widths measured
        // the same way render_page_tabs() sizes its own buttons —
        // draw_button() doesn't clip text to the rect it's given (see
        // internal_render_text()), so an under-measured box would bleed.
        bool           navEnabled   = gDevice.connected;
        const double   navBtnHeight = 26.0 * (2.0 / 3.0);
        double         prevWidth    = get_text_width("< Prev", navBtnHeight, eNoCache);
        double         nextWidth    = get_text_width("Next >", navBtnHeight, eNoCache);
        double         syncWidth    = get_text_width("Sync from synth", navBtnHeight, eNoCache);
        tRgb           prevColour   = (gPressedPatchNav == 0) ? (tRgb)RGB_GREY_5 : (navEnabled ? (tRgb)RGB_GREY_7 : (tRgb)RGB_GREY_3);
        tRgb           nextColour   = (gPressedPatchNav == 1) ? (tRgb)RGB_GREY_5 : (navEnabled ? (tRgb)RGB_GREY_7 : (tRgb)RGB_GREY_3);
        tRgb           syncColour   = (gPressedPatchNav == 2) ? (tRgb)RGB_GREY_5 : (navEnabled ? (tRgb)RGB_GREY_7 : (tRgb)RGB_GREY_3);

        gPrevPatchRect   = {{x + 460.0, y}, {prevWidth, navBtnHeight}};
        gNextPatchRect   = {{x + 460.0 + prevWidth + 12.0, y}, {nextWidth, navBtnHeight}};
        gSyncPatchRect   = {{x + 460.0 + prevWidth + 12.0 + nextWidth + 24.0, y}, {syncWidth, navBtnHeight}};
        draw_button(mainArea, gPrevPatchRect, "< Prev", prevColour);
        draw_button(mainArea, gNextPatchRect, "Next >", nextColour);
        draw_button(mainArea, gSyncPatchRect, "Sync from synth", syncColour);
        gPatchNavLaidOut = true;

        y               += 32.0 * (double)reservedRows;
    }

    // ── Info row: every dial in a `hidden` section, anywhere in the config ────
    // (e.g. the Z1's Category/Voice/Unison*, the Voyager's soundCategory —
    // see layouts/z1.txt / voyager.txt) — shown as "label: value" text
    // rather than a rendered control. Fully generic: this block has no idea
    // what any of these dials mean, so a different device with different
    // (or no) hidden dials needs no change here.
    //
    // Each segment is measured and drawn individually (rather than one
    // concatenated string, as before 2026-07-11) so its on-screen
    // tRectangle can be stored into that dial's own `rect` field —
    // synth_hit_test_info_row() above then lets a panel_dial_needs_value_menu()
    // dial here (e.g. soundCategory) open the same generic dropdown a normal
    // grid dial's own click does (open_dial_value_menu(), menus.c), with no
    // device-specific code: a numeric-only hidden dial just gets a rect
    // nothing ever hit-tests true for a value-menu on, so it stays inert.
    //
    // Wraps onto additional lines once a segment (plus its leading " | "
    // separator) would run past the content area's right edge — added
    // 2026-07-11 once the Voyager's EDIT-menu sweep grew this row to 8
    // entries and the last one or two started rendering off the right edge
    // of the window entirely (invisible AND unclickable, since a rect
    // outside the visible framebuffer is outside the cursor's reachable
    // coordinate space too). A single segment wider than the whole content
    // area on its own still bleeds past the edge even after wrapping —
    // same "render_text() draws unclipped by rectangle width" limitation
    // section_required_spacing()'s own comment above already documents for
    // grid dials; not worth a truncation scheme for a case this unlikely.
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
                    tRectangle sepRect = {{ix, rowY}, {0.0, rowHeight}};

                    render_text(mainArea, sepRect, sep);
                    ix += sepWidth;
                }
                first      = false;

                dial->rect = {{ix, rowY}, {width, rowHeight}};
                render_text(mainArea, dial->rect, pair);
                ix        += width;
            }
        }

        y = rowY + 25.0;
    }

    // ── Active page's dials ─────────────────────────────────────────────────────
    // Layout, colours, ranges and labels all come from layouts/xxxx.txt via
    // panelConfig — this block only supplies the live values and draws. Which
    // section(s) render follows the active page tab, not a fixed section
    // name. A page can hold several sections (e.g. Oscillator's several
    // sections stacked above Filters on the Synthesis page) — each is laid
    // out as its own row, in the file's declaration order, one below the
    // last — UNLESS the page uses explicit col=/row= grid positioning (see
    // tPanelDial.gridCol in panelConfig.h), in which case every section on
    // the page shares one fixed origin instead of stacking, so a dial's own
    // col/row is all that places it (a column can freely mix dials from
    // several sections, e.g. the Voyager's leftmost column carrying both
    // LFO's and Global's dials).
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

        // ── Column headers ───────────────────────────────────────────────
        // Optional per-column title ("columnLabel <col> <text>" in the
        // device's own .txt — see tColumnLabel in panelConfig.h). Reserves
        // one header row's worth of height above row 0 only if the current
        // page declares at least one — a page with none (or a non-grid
        // page entirely) advances y by nothing extra here, rendering
        // exactly as before this existed. Rendered as an actual title, not
        // just another dial label — white and a size up from the 12px
        // dial-label text (RGB_GREY_7, matching every OTHER piece of text
        // on the panel would read as just one more label, not a header),
        // uppercased, with a thin rule underneath to visually cap off the
        // column the way a table header separates from its rows.
        if (pageIsGrid && (cfg->gridColWidth > 0.0)) {
            const double defaultHeaderHeight = 14.0;
            const double padding             = 8.0; // clearance so one column's text/rule doesn't touch the next
            double       headerHeight        = defaultHeaderHeight;
            bool         anyLabel            = false;

            // First pass: render_text() draws unclipped by rectangle width
            // (see section_required_spacing()'s own comment above), and
            // unlike a dial's auto-flow spacing, the grid's column pitch
            // can't be widened per-column to fit an overlong title without
            // shifting every dial in every later column out of alignment —
            // so an overlong header can only be fixed by shrinking the
            // FONT to fit the fixed width instead. Computed once, as the
            // tightest fit across every labelled column on this page, so
            // every header reads at the same size rather than each
            // shrinking independently to whatever bleeds least.
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
                tRectangle     labelRect   = {{colX, y}, {cfg->gridColWidth, headerHeight}};
                set_rgb_colour((tRgb)RGB_WHITE);
                render_text(mainArea, labelRect, upper);

                set_rgb_colour((tRgb)RGB_GREY_5);
                render_line(mainArea, {colX, y + headerHeight + 2.0}, {colX + cfg->gridColWidth - padding, y + headerHeight + 2.0}, 1.0);
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
                tPanelDial * dial    = &section->dials[i];
                uint32_t     dialVal = get_panel_dial_value(dial);
                // Captured before a binary button below shrinks/recentres
                // dial->rect itself — the label text further down still
                // needs to sit relative to where a full-size knob would
                // have been, not the shrunk button.
                double       baseY   = dial->rect.coord.y;

                // Any 2-position named dial (Off/On, but also Filters' Mode
                // "Dual LP"/"HP/LP", Osc 3's Freq Range "Lo"/"Hi", ...)
                // renders as a button showing the current state's name,
                // rather than a knob — a click still flips it (see
                // panel_dial_is_binary()'s own comment in panelConfig.h).
                // Purely a function of the dial's own names=, so any
                // device's dials get this for free, not just Voyager's —
                // shared with mouseHandle.c, which needs the identical test
                // to know these take a single click rather than a drag.
                // Genuine Off/On dials (panel_dial_is_toggle()) additionally
                // get a green background when on — matching gTheme.greenOn
                // used elsewhere — since that IS a meaningful on/off state;
                // a plain grey background for anything else would wrongly
                // imply "HP/LP" or "Lo" mode is somehow "off".
                //
                // A panel_dial_needs_value_menu() dial (>2 positions, no CC —
                // e.g. Filter A/B Pole Select) gets the same textual-button
                // treatment, not a knob — 2026-07-08 user call: a menu-select
                // control reads as a button you click to open a list, not
                // something you'd expect to drag, so it shouldn't look like a
                // knob in the first place.
                if (panel_dial_is_binary(dial) || panel_dial_needs_value_menu(dial)) {
                    const char * name = (dialVal < dial->nameCount) ? dial->names[dialVal] : "?";

                    // draw_button() scales text to fill the WHOLE height of
                    // the rect it's given (internal_render_text) —
                    // dial->rect's dialSize-square shape (sized for a round
                    // knob) scaled text as long as "Dual LP"/"On/Ext" up to
                    // ~36px tall, wildly overrunning a box that width.
                    // Shrinking the button's height and widening it to fit
                    // the WIDEST of its own names (not just whichever's
                    // showing right now, so it doesn't visibly resize as
                    // the user clicks it) makes it read as a compact
                    // rectangular button instead — same idea as
                    // render_page_tabs()'s own width-from-measured-text
                    // sizing above. Mutates dial->rect itself, not a
                    // separate local rect, so the clickable hit area
                    // (mouseHandle.c) always matches what's actually drawn.
                    // buttonHeight matches the 12.0 the dial label/value
                    // text below (further down this loop) renders at, so
                    // the button's own text reads at the same size as every
                    // other label on the panel, not an arbitrarily bigger
                    // one.
                    const double buttonHeight = 12.0;
                    const double padding      = 10.0;
                    double       widest       = 0.0;

                    for (uint32_t n = 0; n < dial->nameCount; n++) {
                        double w = get_text_width(dial->names[n], buttonHeight, eNoCache);

                        if (w > widest) {
                            widest = w;
                        }
                    }

                    dial->rect.coord.y += (section->dialSize - buttonHeight) / 2.0;
                    dial->rect.size     = {widest + padding, buttonHeight};

                    // RGB_GREY_7, not RGB_BACKGROUND_GREY — see
                    // render_page_tabs()'s own identical comment above:
                    // the latter is a dark grey in this build, too
                    // low-contrast for draw_button's fixed black text.
                    tRgb         colour       = panel_dial_is_toggle(dial) && (dialVal != 0)
                                ? (tRgb)RGB_GREEN_ON
                                : (tRgb)RGB_GREY_7;
                    draw_button(mainArea, dial->rect, name, colour);
                } else {
                    render_dial(mainArea, dial->rect, dialVal, dial->max, 0, dial->colour);
                }
                char valBuf[24];

                if (dial->display == dialDisplayNames) {
                    snprintf(valBuf, sizeof(valBuf), "%s",
                             (dialVal < dial->nameCount) ? dial->names[dialVal] : "?");
                } else if (dial->display == dialDisplayRaw) {
                    snprintf(valBuf, sizeof(valBuf), "%u", (unsigned)dialVal);
                } else {
                    snprintf(valBuf, sizeof(valBuf), "%u (%u)", (unsigned)dialVal,
                             (unsigned)get_panel_dial_native_value(dial));
                }

                // Skipped for a binary button or a value-menu button — its
                // face already shows this exact string, so repeating it just
                // below would be a redundant duplicate, not a genuine second
                // piece of information the way it is for a knob.
                if (!panel_dial_is_binary(dial) && !panel_dial_needs_value_menu(dial)) {
                    tRectangle valRect = {{dial->rect.coord.x, baseY + section->dialSize + 4.0},
                        {section->spacing,                              12.0}};
                    set_rgb_colour((tRgb)RGB_GREY_7);
                    render_text(mainArea, valRect, valBuf);
                }
                tRectangle lblRect = {{dial->rect.coord.x, baseY + section->dialSize + 18.0},
                    {section->spacing,                               12.0}};

                // Explicit here, not left over from the valRect render_text
                // above — that one's skipped for a binary button, which
                // otherwise left the last colour draw_button() itself set
                // (black, for the button's own text) in effect for this
                // label too.
                set_rgb_colour((tRgb)RGB_GREY_7);
                render_text(mainArea, lblRect, dial->label);
            }

            if (!pageIsGrid) {
                y += section->dialSize + 46.0; // dial + value/label text + gap before next stacked section
            }
        }
    }
}

#ifdef __cplusplus
}
#endif
