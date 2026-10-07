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
#include "panelGraph.h"
#include "synthBackup.h"
#include "midiComms.h"
#include "misc.h"
#include "bankBrowser.h"
#include "synthGraphics.h"
#include "mouseHandle.h"

#define SYNTH_LAYOUTS_DIR_DEFAULT    "layouts"    // relative to cwd, used until a folder is chosen/persisted
#define SYNTH_MAX_PAGE_TABS          PANEL_MAX_SECTIONS

static char           gLayoutsDir[1024]                   = SYNTH_LAYOUTS_DIR_DEFAULT;

static tPanelConfig   gSynthPanelConfig                   = {0};

tPanelConfig * synth_panel_config(void) {
    return &gSynthPanelConfig;
}

// notes §1
typedef struct {
    char       prefix[PANEL_PAGE_LEN]; // the page path down to this tab, e.g. "Program|EXi 1"
    uint32_t   level;                  // 0 = top row
    tRectangle rect;
} tPageTab;

// notes §53
typedef struct {
    char prefix[PANEL_PAGE_LEN];
    char page[PANEL_PAGE_LEN];
} tLastPageUnder;

static tPageTab       gPageTabs[SYNTH_MAX_PAGE_TABS]      = {0};
static uint32_t       gPageTabCount                       = 0;
static char           gCurrentPage[PANEL_PAGE_LEN]        = {0};
static tLastPageUnder gLastPageUnder[SYNTH_MAX_PAGE_TABS] = {0};
static uint32_t       gLastPageUnderCount                 = 0;
static int32_t        gPressedTabIndex                    = -1; // cosmetic only — see synth_set_pressed_page_tab()

const char * synth_current_page(void) {
    return gCurrentPage;
}

// The first `level` + 1 names of `page`'s path; false if it has fewer.
static bool page_path_prefix(const char * page, uint32_t level, char * out, size_t outSize) {
    const char * end = page;

    for (uint32_t l = 0; ; l++) {
        while ((*end != '\0') && (*end != PANEL_PAGE_SEPARATOR)) {
            end++;
        }

        if (l == level) {
            break;
        }

        if (*end == '\0') {
            return false;
        }
        end++;
    }

    size_t       len = (size_t)(end - page);

    if (len >= outSize) {
        len = outSize - 1;
    }
    memcpy(out, page, len);
    out[len] = '\0';
    return true;
}


// notes §56
#define SYNTH_MAX_PAGES    (PANEL_MAX_SECTIONS * 2)
static char     gPages[SYNTH_MAX_PAGES][PANEL_PAGE_LEN];
static uint32_t gPageCount = 0;

static void add_page(const char * page) {
    for (uint32_t i = 0; i < gPageCount; i++) {
        if (strcmp(gPages[i], page) == 0) {
            return;
        }
    }

    if (gPageCount < SYNTH_MAX_PAGES) {
        strncpy(gPages[gPageCount], page, PANEL_PAGE_LEN - 1);
        gPages[gPageCount][PANEL_PAGE_LEN - 1] = '\0';
        gPageCount++;
    }
}

static void build_page_list(void) {
    gPageCount = 0;

    for (uint32_t i = 0; i < gSynthPanelConfig.sectionCount; i++) {
        const char * page = gSynthPanelConfig.sections[i].page;

        add_page(page);

        for (uint32_t v = 0; v < gSynthPanelConfig.pageVariantCount; v++) {
            const tPageVariant * variant = &gSynthPanelConfig.pageVariants[v];

            if (panel_page_is_under(page, variant->base)) {
                char mapped[PANEL_PAGE_LEN];

                snprintf(mapped, sizeof(mapped), "%s%s", variant->variant, page + strlen(variant->base));
                add_page(mapped);
            }
        }
    }
}

// The page whose sections `page` shows (itself, or its base for a variant page); returns the variant, or -1.
static int32_t resolve_page(const char * page, char * out, size_t outSize) {
    for (uint32_t v = 0; v < gSynthPanelConfig.pageVariantCount; v++) {
        const tPageVariant * variant = &gSynthPanelConfig.pageVariants[v];

        if (panel_page_is_under(page, variant->variant)) {
            snprintf(out, outSize, "%s%s", variant->base, page + strlen(variant->variant));
            return (int32_t)v;
        }
    }

    snprintf(out, outSize, "%s", page);
    return -1;
}

// notes §57
static int32_t variant_dump_delta(const tPanelSection * section, int32_t variant) {
    if ((variant < 0) || !panel_page_is_under(section->page, gSynthPanelConfig.pageVariants[variant].base)) {
        return 0;
    }
    return gSynthPanelConfig.pageVariants[variant].dumpDelta;
}

// notes §58
static int32_t show_if_dial_value(int32_t dumpOffset) {
    for (uint32_t i = 0; i < gSynthPanelConfig.sectionCount; i++) {
        tPanelSection * section = &gSynthPanelConfig.sections[i];

        for (uint32_t d = 0; d < section->dialCount; d++) {
            const tPanelDial * dial = &section->dials[d];

            if ((dial->dumpOffset == dumpOffset) && (dial->dumpShift == 0) && (dial->dumpBitWidth == 0)) {
                return (int32_t)get_panel_dial_value(dial) + dial->storageOffset;
            }
        }
    }

    return -1;
}

static bool section_shown(const tPanelSection * section, int32_t variant) {
    if (section->showIfOffset < 0) {
        return true;
    }
    int32_t value = synth_dump_byte(section->showIfOffset + variant_dump_delta(section, variant));

    if (value < 0) {
        value = show_if_dial_value(section->showIfOffset);
    }
    return (value < 0) || ((uint32_t)value == section->showIfValue);
}

// notes §61
static bool variant_absent(int32_t variant) {
    if ((variant < 0) || (gSynthPanelConfig.pageVariants[variant].showIfOffset < 0)) {
        return false;
    }
    const tPageVariant * v     = &gSynthPanelConfig.pageVariants[variant];
    int32_t              value = synth_dump_byte(v->showIfOffset);

    if (value < 0) {
        value = show_if_dial_value(v->showIfOffset);
    }
    return (value >= 0) && ((value < v->showIfMin) || (value > v->showIfMax));
}

// The tab `prefix` names a pageVariant whose slot is absent.
static bool variant_tab_absent(const char * prefix) {
    for (uint32_t v = 0; v < gSynthPanelConfig.pageVariantCount; v++) {
        if (strcmp(gSynthPanelConfig.pageVariants[v].variant, prefix) == 0) {
            return variant_absent((int32_t)v);
        }
    }

    return false;
}

// A page with at least one section showing.
static bool page_shown(const char * page) {
    char    base[PANEL_PAGE_LEN];
    int32_t variant = resolve_page(page, base, sizeof(base));

    if (variant_absent(variant)) {
        return false;
    }

    for (uint32_t i = 0; i < gSynthPanelConfig.sectionCount; i++) {
        const tPanelSection * section = &gSynthPanelConfig.sections[i];

        if (!section->hidden && (strcmp(section->page, base) == 0) && section_shown(section, variant)) {
            return true;
        }
    }

    return false;
}

// A tab's text: its own name, plus the value its tabLabel line reads from the dump ("EXi 1: AL-1").
static void tab_label(const char * prefix, char * out, size_t outSize) {
    const char * name    = strrchr(prefix, PANEL_PAGE_SEPARATOR);
    char         base[PANEL_PAGE_LEN];
    int32_t      variant = resolve_page(prefix, base, sizeof(base));

    snprintf(out, outSize, "%s", name ? name + 1 : prefix);

    for (uint32_t i = 0; i < gSynthPanelConfig.tabLabelCount; i++) {
        const tTabLabel * label = &gSynthPanelConfig.tabLabels[i];

        if (strcmp(label->tab, base) == 0) {
            int32_t delta = (variant >= 0) ? gSynthPanelConfig.pageVariants[variant].dumpDelta : 0;
            int32_t value = synth_dump_byte(label->dumpOffset + delta);

            if ((value >= 0) && ((uint32_t)value < label->nameCount)) {
                size_t len = strlen(out);

                snprintf(out + len, outSize - len, ": %s", label->names[value]);
            }
        }
    }
}

static bool page_is_listed(const char * page) {
    for (uint32_t i = 0; i < gPageCount; i++) {
        if (strcmp(gPages[i], page) == 0) {
            return true;
        }
    }

    return false;
}

static bool page_exists(const char * page) {
    for (uint32_t i = 0; i < gPageCount; i++) {
        if (strcmp(gPages[i], page) == 0) {
            return true;
        }
    }

    for (uint32_t i = 0; i < gPageCount; i++) {
        if (panel_page_is_under(gPages[i], page)) {
            return true; // a tab whose pages are all hidden (notes §57)
        }
    }

    return panel_mode_for_tab(&gSynthPanelConfig, page) >= 0; // notes §55
}

// The page a click on the tab for `prefix` opens: the one last shown under it, else the first under it.
static const char * page_for_prefix(const char * prefix) {
    for (uint32_t i = 0; i < gLastPageUnderCount; i++) {
        if ((strcmp(gLastPageUnder[i].prefix, prefix) == 0) && page_shown(gLastPageUnder[i].page)) {
            return gLastPageUnder[i].page;
        }
    }

    for (uint32_t i = 0; i < gPageCount; i++) {
        if (panel_page_is_under(gPages[i], prefix) && page_shown(gPages[i])) {
            return gPages[i];
        }
    }

    return page_exists(prefix) ? prefix : NULL;
}

static void remember_page(const char * page) {
    char prefix[PANEL_PAGE_LEN];

    for (uint32_t level = 0; page_path_prefix(page, level, prefix, sizeof(prefix)); level++) {
        uint32_t i = 0;

        while ((i < gLastPageUnderCount) && (strcmp(gLastPageUnder[i].prefix, prefix) != 0)) {
            i++;
        }

        if (i == gLastPageUnderCount) {
            if (gLastPageUnderCount >= SYNTH_MAX_PAGE_TABS) {
                return;
            }
            gLastPageUnderCount++;
            strncpy(gLastPageUnder[i].prefix, prefix, sizeof(gLastPageUnder[i].prefix) - 1);
        }
        strncpy(gLastPageUnder[i].page, page, sizeof(gLastPageUnder[i].page) - 1);
    }
}

void synth_set_current_page(const char * page) {
    if (page && (page[0] != '\0')) {
        strncpy(gCurrentPage, page, sizeof(gCurrentPage) - 1);
        gCurrentPage[sizeof(gCurrentPage) - 1] = '\0';
        remember_page(gCurrentPage);

        char base[PANEL_PAGE_LEN];

        synth_set_active_page_variant(resolve_page(gCurrentPage, base, sizeof(base)));
        synthlib_request_redraw();
    }
}

uint32_t synth_current_page_sections(tPanelSection * outSections[], uint32_t maxSections) {
    uint32_t count   = 0;
    char     page[PANEL_PAGE_LEN];

    int32_t  variant = resolve_page(gCurrentPage, page, sizeof(page));

    for (uint32_t i = 0; (i < gSynthPanelConfig.sectionCount) && (count < maxSections); i++) {
        tPanelSection * section = &gSynthPanelConfig.sections[i];

        if (!section->hidden && (strcmp(section->page, page) == 0) && section_shown(section, variant)) {
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
        tPageTab * tab = &gPageTabs[index];

        synth_set_current_page(page_for_prefix(tab->prefix));

        if (tab->level == 0) {
            // notes §54
            int32_t mode = panel_mode_for_tab(&gSynthPanelConfig, tab->prefix);

            if (mode >= 0) {
                synth_send_device_mode((uint32_t)mode);
            }
        }
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

// Rebuilds gPageTabs from the config's page paths and renders one button row per level - the top row
// always, each row below it only for the tab selected above. Returns the height consumed. Defaults
// gCurrentPage to the first page seen if it isn't set (or no longer exists), and follows a mode the
// device reported (synth_take_reported_mode()) to that mode's top-level tab.
static double render_page_tabs(tRectangle origin) {
    gPageTabCount = 0;

    if ((gPageCount > 0) && !page_exists(gCurrentPage)) {
        synth_set_current_page(gPages[0]);
    }
    char                parentTab[PANEL_PAGE_LEN];
    char                lastLevel[PANEL_PAGE_LEN];
    uint32_t            depth                         = 0;

    while (page_path_prefix(gCurrentPage, depth + 1, lastLevel, sizeof(lastLevel))) {
        depth++;
    }

    // notes §57
    if (  (depth > 0) && page_is_listed(gCurrentPage) && !page_shown(gCurrentPage) && page_path_prefix(gCurrentPage, depth - 1, parentTab, sizeof(parentTab))
       && (strcmp(gCurrentPage, parentTab) != 0)) {
        const char * page = page_for_prefix(parentTab);

        if (page && (strcmp(page, gCurrentPage) != 0)) {
            synth_set_current_page(page);
        }
    }
    int32_t             reported                      = synth_take_reported_mode();

    if (reported >= 0) {
        const char * tab  = panel_mode_tab_name(&gSynthPanelConfig, (uint32_t)reported);
        const char * page = tab ? page_for_prefix(tab) : NULL;

        if (page) {
            synth_set_current_page(page);
        }
    }
    // notes §8
    static const double kTabHeight[PANEL_PAGE_LEVELS] = {18.0, 15.0, 13.0};
    const double        tabGap                        = 6.0;
    double              y                             = origin.coord.y;

    for (uint32_t level = 0; level < PANEL_PAGE_LEVELS; level++) {
        char     parent[PANEL_PAGE_LEN] = "";
        uint32_t rowStart               = gPageTabCount;

        if ((level > 0) && !page_path_prefix(gCurrentPage, level - 1, parent, sizeof(parent))) {
            break;
        }

        // notes §55
        for (uint32_t m = 0; (level == 0) && (m < gSynthPanelConfig.modeTabCount) && (gPageTabCount < SYNTH_MAX_PAGE_TABS); m++) {
            strncpy(gPageTabs[gPageTabCount].prefix, gSynthPanelConfig.modeTabs[m].tab, sizeof(gPageTabs[gPageTabCount].prefix) - 1);
            gPageTabs[gPageTabCount].level = 0;
            gPageTabCount++;
        }

        for (uint32_t i = 0; i < gPageCount; i++) {
            const char * page  = gPages[i];
            char         prefix[PANEL_PAGE_LEN];
            bool         known = false;

            if (((level > 0) && !panel_page_is_under(page, parent)) || !page_path_prefix(page, level, prefix, sizeof(prefix))) {
                continue;
            }

            if ((strcmp(prefix, page) == 0) && !page_shown(page)) {
                continue; // notes §57
            }

            if (variant_tab_absent(prefix)) {
                continue; // notes §61
            }

            for (uint32_t t = rowStart; t < gPageTabCount; t++) {
                if (strcmp(gPageTabs[t].prefix, prefix) == 0) {
                    known = true;
                    break;
                }
            }

            if (!known && (gPageTabCount < SYNTH_MAX_PAGE_TABS)) {
                strncpy(gPageTabs[gPageTabCount].prefix, prefix, sizeof(gPageTabs[gPageTabCount].prefix) - 1);
                gPageTabs[gPageTabCount].level = level;
                gPageTabCount++;
            }
        }

        if (gPageTabCount == rowStart) {
            break;
        }
        double tabHeight = kTabHeight[level];
        double x         = origin.coord.x;

        for (uint32_t i = rowStart; i < gPageTabCount; i++) {
            char       label[PANEL_LABEL_LEN];

            tab_label(gPageTabs[i].prefix, label, sizeof(label));

            if (label[0] != '\0') {
                label[0] = (char)toupper((unsigned char)label[0]);
            }
            // notes §9
            double     width   = get_text_width(label, tabHeight, eNoCache);   // ~8px padding each side
            tRectangle rect    = {{x, y}, {width, tabHeight}};
            bool       active  = panel_page_is_under(gCurrentPage, gPageTabs[i].prefix);
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

        y += tabHeight + tabGap;
    }

    return (gPageTabCount > 0) ? (y - origin.coord.y - tabGap + 12.0) : 0.0;
}

// notes §11
#define BUTTON_TEXT_PADDING    10.0

// notes §12
#define MIN_BUTTON_GAP         8.0

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
    gCurrentPage[0]     = '\0';
    gLastPageUnderCount = 0;
    build_page_list();
    synth_set_active_page_variant(-1);

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

// An unavailable button is its label alone, dimmed - no face, no outline. On the dark palette a
// disabled face is the background grey, and draw_button() then picks white text: brighter than an
// enabled button's, which is the opposite of unavailable.
static void draw_nav_button(tRectangle rect, const char * label, bool enabled, tRgb face) {
    if (enabled) {
        draw_button(mainArea, rect, label, face);
        return;
    }
    set_rgb_colour((tRgb)RGB_GREY_5);
    render_text(mainArea, (tRectangle){
        {rect.coord.x + DRAW_BUTTON_MARGIN, rect.coord.y + DRAW_BUTTON_MARGIN}, {BLANK_SIZE, rect.size.h}
    }, label);
}

// Which preset the synth is on, and how sure that is - types.h notes §5.
static void draw_current_program(double x, double y, double height) {
    static const char *const kQualifier[] = {
        [eProgramUnknown]           = "",
        [eProgramFromProgramChange] = " (unconfirmed)",
        [eProgramMatchedByName]     = " (by name)",
        [eProgramConfirmed]         = "",
    };
    char                     label[64];
    const tPanelConfig *     cfg          = synth_panel_config();

    if (!gDevice.connected || (!cfg->moogStyleDump && (cfg->bankCount == 0))) {
        return;
    }

    if (gDevice.currentProgram < 0) {
        if (!gDevice.programChangeTransmitOff) {
            return;
        }
        snprintf(label, sizeof(label), "Program Change Transmit is off on the synth"); // notes §59
    } else if (cfg->bankCount > 0) {
        uint32_t bank = (uint32_t)gDevice.currentProgram / 128;

        snprintf(label, sizeof(label), "%s%03d%s", (bank < cfg->bankCount) ? cfg->banks[bank].name : "?",
                 (int)(gDevice.currentProgram % 128), kQualifier[gDevice.programCertainty]);
    } else {
        snprintf(label, sizeof(label), "Preset %d%s", (int)gDevice.currentProgram + 1, kQualifier[gDevice.programCertainty]);
    }
    set_rgb_colour((gDevice.programCertainty == eProgramConfirmed) ? (tRgb)RGB_WHITE : (tRgb)RGB_GREY_5);
    render_text(mainArea, (tRectangle){
        {x + DRAW_BUTTON_MARGIN, y + DRAW_BUTTON_MARGIN}, {BLANK_SIZE, height}
    }, label);
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
        tPanelConfig * cfg             = synth_panel_config();
        uint32_t       maxFieldLen     = (cfg->panelNameLen > cfg->presetNameLen) ? cfg->panelNameLen : cfg->presetNameLen;
        uint32_t       reservedRows    = (cfg->nameLineWidth > 0)
                                     ? ((maxFieldLen + cfg->nameLineWidth - 1) / cfg->nameLineWidth)
                                     : 1;

        if (reservedRows == 0) {
            reservedRows = 1;
        }
        char *         line            = strtok(nameBuf, "\n");
        uint32_t       row             = 0;

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
        bool           navEnabled      = gDevice.connected;
        bool           prevNextEnabled = navEnabled && (gDevice.currentProgram >= 0);
        // notes §29
        const double   navBtnHeight    = 12.0;
        double         prevWidth       = get_text_width("< Prev", navBtnHeight, eNoCache);
        double         nextWidth       = get_text_width("Next >", navBtnHeight, eNoCache);
        double         syncWidth       = get_text_width("Sync from synth", navBtnHeight, eNoCache);
        tRgb           prevColour      = (gPressedPatchNav == 0) ? (tRgb)RGB_GREY_5 : (tRgb)RGB_GREY_7;
        tRgb           nextColour      = (gPressedPatchNav == 1) ? (tRgb)RGB_GREY_5 : (tRgb)RGB_GREY_7;
        tRgb           syncColour      = (gPressedPatchNav == 2) ? (tRgb)RGB_GREY_5 : (tRgb)RGB_GREY_7;

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
        draw_nav_button(gPrevPatchRect, "< Prev", prevNextEnabled, prevColour);
        draw_nav_button(gNextPatchRect, "Next >", prevNextEnabled, nextColour);
        draw_nav_button(gSyncPatchRect, "Sync from synth", navEnabled, syncColour);
        draw_current_program(gSyncPatchRect.coord.x + syncWidth + 24.0, y, navBtnHeight);
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

        if ((sectionCount == 0) && (gCurrentPage[0] != '\0')) {
            char label[PANEL_LABEL_LEN * 2];
            char note[sizeof(label) + 32];

            tab_label(gCurrentPage, label, sizeof(label));
            snprintf(note, sizeof(note), "Nothing to edit here yet (%s)", label);
            set_rgb_colour((tRgb)RGB_GREY_7);
            render_text(mainArea, (tRectangle){{x, y + 20.0}, {400.0, 14.0}}, note);
        }
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

            // notes §60
            if (section->graph.present) {
                y += panel_graph_render(mainArea, section, (tCoord){x, y}) + 24.0;
                continue;
            }
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
                    const char * name         = isToggle ? dial->label
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
                    tRgb         colour       = disabled ? (tRgb)RGB_GREY_3
                                : (isToggle && (dialVal != 0)) ? (tRgb)RGB_GREEN_ON
                                : panel_dial_needs_value_menu(dial) ? dial->colour
                                : (tRgb)RGB_GREY_7;
                    draw_button(mainArea, dial->rect, name, colour);
                } else {
                    render_dial(mainArea, dial->rect, dialVal, synth_dial_max(dial), 0, disabled ? (tRgb)RGB_GREY_3 : dial->colour);
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
