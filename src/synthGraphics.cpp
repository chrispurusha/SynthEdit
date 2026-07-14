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
#include "synthBackup.h"
#include "midiComms.h"
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

    // Prev/Next need a known gDevice.currentProgram to mean anything (see
    // synth_navigate_preset()'s own comment, synthComms.c) — disabled here
    // at the hit-test level, not just cosmetically greyed, so a click
    // genuinely does nothing (no press-highlight, no navigation) rather
    // than silently guessing a relative step from an assumed slot 0.
    // Fixed 2026-07-13 per owner report: with the OLD "default to 0 when
    // unknown" behaviour, Prev/Next always jumped to/from slot 0 on a
    // fresh connect (since nothing else sets currentProgram — Load Patch
    // from Bank wasn't working either), reading as "Prev/Next always
    // starts at the first patch." Sync (index 2) is unaffected — it needs
    // no known program to mean something.
    if ((gDevice.currentProgram >= 0) && within_rectangle(coord, gPrevPatchRect)) {
        return 0;
    }

    if ((gDevice.currentProgram >= 0) && within_rectangle(coord, gNextPatchRect)) {
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
        // Skipped while a dump-only dial change or program-name edit is
        // already mid fetch-then-patch (synth_dump_patch_in_flight(),
        // synthComms.c) — firing a SECOND request here raced the first
        // one's reply, found 2026-07-11 by dragging Headphone Volume then
        // immediately pressing this button: the second reply landed after
        // the first had already applied+sent the new value, got decoded
        // normally (nothing left to skip it), and overwrote the display
        // back to the pre-drag value even though the actual write had gone
        // out correctly. The in-flight request's own reply already refreshes
        // everything this button wants, so there's nothing to gain by
        // sending a duplicate — and real hardware next to no cost either way,
        // since the outstanding request is seconds away at most.
        if (!synth_dump_patch_in_flight()) {
            synth_request_state_dump();
        }
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

// Decodes a dialDisplaySignedHiLo dial's raw dump value (an unsigned
// 0..2^dumpBitWidth-1 wire value, read normally via the existing generic
// bitfield extraction) into its coarse (HIGH) and fine (LOW) components —
// see that enum value's own long comment (panelConfig.h) for the field this
// was derived against (Voyager's PGM Shaping 1/2 Fixed Value) and the full
// worked examples that validated this exact formula against real hardware,
// 2026-07-11/12.
//
// The tricky part isn't the arithmetic itself (adjusted = signed_raw -
// hiLoOffset; HIGH = adjusted / hiLoCoarseScale; LOW = the remainder) — it's
// that a naive floor(adjusted / coarseScale) lands LOW in [0, coarseScale/
// fineScale - 1] (e.g. 0..127), not the SIGNED range the real front panel
// actually shows (-64..+63) — the two ranges disagree by exactly one full
// LOW span (128) for any `adjusted` that would need LOW >= coarseScale/
// fineScale/2. Confirmed empirically: H=0,L=-64 (a real, reachable,
// hardware-verified position) computes as adjusted=-512, naive
// floor(-512/1024)=-1 with remainder 512 -> naive LOW=512/8=64 — a value
// LOW can never actually show (its own confirmed range tops out at +63).
// The fix is the same "if the naive fine value is in the UPPER half of its
// own span, it's really NEGATIVE and the coarse value is one higher" carry
// adjustment the real hardware itself performs when LOW overflows past +63
// into HIGH+1 (owner observed this directly on real hardware, 2026-07-12).
static void synth_decode_hilo_dial(const tPanelDial * dial, uint32_t rawValue, int32_t * outHigh, int32_t * outLow) {
    uint32_t width       = (dial->dumpBitWidth > 0) ? dial->dumpBitWidth : 16;
    uint32_t half        = 1u << (width - 1);
    int32_t  signedRaw   = (rawValue >= half) ? (int32_t)(rawValue - (half * 2)) : (int32_t)rawValue;
    int32_t  adjusted    = signedRaw - dial->hiLoOffset;
    int32_t  coarse      = (int32_t)dial->hiLoCoarseScale;
    int32_t  fine        = (int32_t)dial->hiLoFineScale;
    int32_t  fineSpan    = (fine > 0) ? (coarse / fine) : 0;            // e.g. 1024/8 = 128 distinct LOW steps per HIGH step
    // Python-style "always non-negative" remainder, not C's truncate-toward-
    // zero one — see this function's own header comment for why a plain `%`
    // here would land LOW outside its real -64..+63 range for any negative
    // `adjusted`.
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
    // 18.0 = 1.5x navBtnHeight (12.0, "Sync from synth"'s own text height,
    // see its own comment further down this file) — was a flat 24.0 (2x),
    // shrunk 2026-07-13 on owner request so the page tabs read closer in
    // scale to the nav/sync buttons on the same row rather than dominating
    // them.
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

// Shared between section_required_spacing() below and the binary/value-menu
// button-rendering block further down this file (search BUTTON_TEXT_PADDING)
// — both need the EXACT same padding a button's own text gets, not just a
// similar one. Used to be two independently-chosen numbers (8.0 here,
// 10.0 there) that quietly drifted apart: an auto-flow section's spacing
// this function widens to fit its own widest button was computed 2px
// narrower than the button that same text actually rendered at, so the
// button's right edge bled into the next dial's slot by that same 2px —
// found 2026-07-13 on the Z1's Porta Mode/Porta Time pair ("FINGERED"
// pushing Porta Time's knob rect partially under Porta Mode's button).
// One named constant instead of two hand-typed literals means this can't
// silently re-drift the next time either call site gets tweaked.
#define BUTTON_TEXT_PADDING    10.0

// Extra clearance section_required_spacing() reserves BETWEEN adjacent
// buttons, on top of BUTTON_TEXT_PADDING — added 2026-07-13 investigating a
// real overlap report on the Z1's Voice/Unison row, AFTER the
// BUTTON_TEXT_PADDING unification above. Live-logged numbers showed no
// actual overlap in the numbers themselves (Voice's own button ended a mere
// ~4px before Unison's started, same tight single-digit-pixel margin on
// every other button pair on the page) — get_text_width() sums per-glyph
// advance widths to measure a string, which is a good estimate but not
// guaranteed pixel-identical to whatever the GPU actually rasterizes for
// that exact string (kerning, hinting, antialiasing edge bleed). With only
// ~4px of slack, that estimate only has to be off by a couple of percent for
// the drawn button to visually touch or bleed into its neighbour even though
// the measured rects don't overlap. This constant is deliberately NOT added
// to a button's own rect.size (kept snug, matching BUTTON_TEXT_PADDING
// exactly, so an individual button doesn't look oversized on its own — see
// that constant's own comment) — only to the shared PITCH between dials in
// section_required_spacing(), so there's always a visible gap regardless of
// small measurement/rendering discrepancies like this one.
#define MIN_BUTTON_GAP    8.0

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
    gReDraw = true;
}

// Scans gLayoutsDir for every <device>.txt it contains; a single match is
// used directly (no prompt — nothing to choose), but more than one first
// tries the persisted "lastDeviceConfig" preference (get_saved_device_
// config(), misc.h) before ever prompting — added 2026-07-13 per owner
// request, so a returning user isn't asked to re-pick every single launch.
// Only falls through to the actual chooser (device name + description, from
// each file's own "device"/"description" lines) if there's no saved choice
// yet, or the saved filename isn't among what's actually here right now
// (e.g. that device.txt was removed/renamed since). If the user cancels the
// chooser, whatever gConfigFileName already held is left alone rather than
// blocking startup. Every path that settles on a filename persists it via
// set_saved_device_config() — including the saved-preference fast path,
// which is a harmless re-write of what's already there.
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
        set_saved_device_config(gConfigFileName);
        return;
    }
    const char *          saved = get_saved_device_config();

    if (saved) {
        for (uint32_t i = 0; i < count; i++) {
            if (strcmp(candidates[i].filename, saved) == 0) {
                strncpy(gConfigFileName, saved, sizeof(gConfigFileName) - 1);
                gConfigFileName[sizeof(gConfigFileName) - 1] = '\0';
                set_saved_device_config(gConfigFileName);
                return;
            }
        }
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
                                                               labels, count, 0);

    if (chosen >= 0) {
        strncpy(gConfigFileName, candidates[(uint32_t)chosen].filename, sizeof(gConfigFileName) - 1);
        gConfigFileName[sizeof(gConfigFileName) - 1] = '\0';
        set_saved_device_config(gConfigFileName);
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
    // Whatever was connected under the PREVIOUS config's SysEx identity
    // means nothing once a different device's protocol is loaded — don't
    // leave the UI showing a stale "connected" state while the fresh scan
    // below runs. synth_on_connected() (synthComms.c) does the matching
    // dial-state reset already, once/if a real identity reply for the
    // NEWLY loaded device actually arrives.
    gDevice.connected                            = false;
    midi_scan_devices();
    gReDraw                                      = true;
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

// ── Bulk operation progress overlay ─────────────────────────────────────────
// Shown while a Backup > Bank (Individual Files) export or Restore > Bank
// (Individual Files) sweep is running — same idea as G2-Edit's own
// render_bank_backup_progress()/render_bank_restore_progress() (graphics.cpp
// there), collapsed into ONE overlay here since SynthEdit only ever has one
// such sweep active at a time (both directions guard against overlapping —
// see synth_backup_bank_to_folder()/synth_backup_restore_folder()'s own
// comments, synthBackup.h) rather than G2's own backup/restore-can-run-
// independently shape. Purely visual — doesn't block mouse input to
// whatever's underneath, same as G2-Edit's own version; the bulk sweeps
// themselves already refuse to start a second overlapping one, so there's
// nothing unsafe about a stray click elsewhere while this is showing.
// Small, non-blocking status-row indicator for a name sweep (Korg-style
// Load/Store Patch from/to Bank's 256-request sweep, or the equivalent Moog
// batch name sweep) — unlike synth_render_backup_progress() below (a
// blocking, screen-dimming modal, appropriate for a real bank export/
// restore where the user has explicitly asked to wait), a name sweep can
// now also be started silently in the background while the user keeps
// working (synth_backup_flush_background_prefetch(), synthBackup.c) — a
// modal would defeat the entire point. 2026-07-14 user request ("We don't
// necessarily need the live picker at this point... percentage on status
// bar please") also covers the EXPLICIT click case: the native picker now
// opens immediately regardless of sweep progress (korg_sweep_show_picker(),
// synthBackup.c — showing "---" for anything not yet swept), so this row is
// purely informational background-fill progress, not something blocking
// the picker the way the old modal (still used by Bank export/Restore
// below) used to.
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
    double   textH       = 13.0;
    double   barW        = 160.0;
    double   barH        = 6.0;
    unsigned pct         = (total > 0) ? (unsigned)(((uint64_t)current * 100) / total) : 0;
    char     lineBuf[80];

    snprintf(lineBuf, sizeof(lineBuf), "Fetching preset names... %u%% (%u of %u, %u found)",
             pct, (unsigned)current, (unsigned)total, (unsigned)actionCount);

    double   textY       = renderH - margin - textH;
    double   barY        = textY - 4.0 - barH;

    set_rgb_colour((tRgb)RGB_GREY_2);
    render_rectangle(mainArea, {{margin, barY}, {barW, barH}});
    set_rgb_colour((tRgb)RGB_GREEN_ON);
    render_rectangle(mainArea, {{margin, barY}, {barW * ((double)pct / 100.0), barH}});

    set_rgb_colour((tRgb)RGB_GREY_7);
    render_text(mainArea, {{margin, textY}, {renderW - (margin * 2.0), textH}}, lineBuf);
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
    render_rectangle(mainArea, {{0.0, 0.0}, {renderW, renderH}});

    // Dialog box
    set_rgb_colour((tRgb)RGB_GREY_5);
    render_rectangle_with_border(mainArea, {{boxX, boxY}, {boxW, boxH}});

    // Title bar
    set_rgb_colour((tRgb)RGB_GREY_3);
    render_rectangle(mainArea, {{boxX, boxY}, {boxW, titleH}});
    set_rgb_colour((tRgb)RGB_BLACK);
    render_text(mainArea, {{boxX + margin, boxY + 6.0}, {BLANK_SIZE, STANDARD_TEXT_HEIGHT}}, title);

    char         lineBuf[128];

    snprintf(lineBuf, sizeof(lineBuf), "Preset %u of %u", (unsigned)current, (unsigned)total);
    set_rgb_colour((tRgb)RGB_WHITE);
    render_text(mainArea, {{boxX + margin, boxY + titleH + margin}, {BLANK_SIZE, STANDARD_TEXT_HEIGHT}}, lineBuf);

    snprintf(lineBuf, sizeof(lineBuf), "%u %s so far", (unsigned)actionCount, verb);
    render_text(mainArea, {{boxX + margin, boxY + titleH + margin + STANDARD_TEXT_HEIGHT + 6.0}, {BLANK_SIZE, STANDARD_TEXT_HEIGHT}}, lineBuf);

    // Progress bar
    double       barY = boxY + boxH - margin - 8.0;
    double       barW = boxW - margin * 2.0;
    double       frac = (total > 0) ? ((double)current / (double)total) : 0.0;

    // RGB_GREY_9 (G2-Edit's own track colour) doesn't exist in SynthEdit's
    // own synthlibDefs.h palette variant (see the #ifdef G2_EDIT split
    // there) — RGB_GREY_2 used instead, darker so it reads clearly against
    // both the box's own RGB_GREY_5 and the green fill.
    set_rgb_colour((tRgb)RGB_GREY_2);
    render_rectangle(mainArea, {{boxX + margin, barY}, {barW, 8.0}});
    set_rgb_colour((tRgb)RGB_GREEN_ON);
    render_rectangle(mainArea, {{boxX + margin, barY}, {barW * frac, 8.0}});
}

void synth_render(tRectangle area) {
    double x = area.coord.x + 30.0;
    double y = area.coord.y + 20.0;

    // ── Page tabs ──────────────────────────────────────────────────────────────
    y += render_page_tabs({{x, y}, {0, 0}});

    // ── Program name ──────────────────────────────────────────────────────────
    // While disconnected, show "Not connected" instead of a name. Some
    // protocols never send a program name at all once connected (e.g. the
    // Voyager's Panel Dump — see extract_moog_panel_info() in synthComms.c),
    // so an empty progName after connecting isn't "still loading", it's
    // "this device doesn't report one" — show the device name instead.
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
        gProgNameRect    = {{x, y}, {450.0, 32.0 * (double)reservedRows}};
        gProgNameLaidOut = true;

        // Prev/Next patch buttons — see synth_navigate_preset() (synthComms.h)
        // for what they send. Prev/Next specifically ALSO need a known
        // gDevice.currentProgram (fixed 2026-07-13 — see synth_hit_test_
        // patch_nav()'s own comment for the "always jumps to slot 0"
        // problem this solves); Sync doesn't need one. navEnabled here only
        // covers the "no device at all" case shared by all three buttons —
        // prevNextEnabled below adds the extra Prev/Next-only condition on
        // top of it. Widths measured the same way render_page_tabs() sizes
        // its own buttons — draw_button() doesn't clip text to the rect
        // it's given (see internal_render_text()), so an under-measured box
        // would bleed.
        bool         navEnabled      = gDevice.connected;
        bool         prevNextEnabled = navEnabled && (gDevice.currentProgram >= 0);
        // 12.0, not the old 26.0 * (2.0 / 3.0) (~17px) — matches
        // buttonHeight, the size every dial's own value-menu/toggle button
        // face and label text renders at elsewhere in this file (2026-07-11
        // user request: these three should read at the same size as the
        // panel's own dial labels, not a bespoke larger one).
        const double navBtnHeight    = 12.0;
        double       prevWidth       = get_text_width("< Prev", navBtnHeight, eNoCache);
        double       nextWidth       = get_text_width("Next >", navBtnHeight, eNoCache);
        double       syncWidth       = get_text_width("Sync from synth", navBtnHeight, eNoCache);
        tRgb         prevColour      = (gPressedPatchNav == 0) ? (tRgb)RGB_GREY_5 : (prevNextEnabled ? (tRgb)RGB_GREY_7 : (tRgb)RGB_GREY_3);
        tRgb         nextColour      = (gPressedPatchNav == 1) ? (tRgb)RGB_GREY_5 : (prevNextEnabled ? (tRgb)RGB_GREY_7 : (tRgb)RGB_GREY_3);
        tRgb         syncColour      = (gPressedPatchNav == 2) ? (tRgb)RGB_GREY_5 : (navEnabled ? (tRgb)RGB_GREY_7 : (tRgb)RGB_GREY_3);

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
                tPanelDial * dial     = &section->dials[i];
                uint32_t     dialVal  = get_panel_dial_value(dial);
                // See disabledUnlessDialId's own comment in panelConfig.h —
                // greyed out and non-interactive (mouseHandle.c) unless the
                // gating dial's current value matches. Checked once here so
                // both the button and knob render paths below can use it.
                bool         disabled = panel_dial_is_disabled(dial, cfg);
                // Captured before a binary button below shrinks/recentres
                // dial->rect itself — the label text further down still
                // needs to sit relative to where a full-size knob would
                // have been, not the shrunk button.
                double       baseY    = dial->rect.coord.y;
                // A structurally genuine Off/On pair (panel_dial_is_toggle())
                // that's opted into asMenu (panelConfig.h) is styled as a
                // value-menu button instead — label+green/grey toggle
                // treatment excluded — so every use of "is this a toggle for
                // rendering purposes" below (button face text/colour further
                // down, and the below-button label line's own toggle
                // shortcut near the end of this loop) goes through this one
                // flag rather than re-deriving it, so both stay consistent.
                bool         isToggle = panel_dial_is_toggle(dial) && !dial->asMenu;

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
                    // Genuine Off/On toggles show their own LABEL on the
                    // button face ("Glide", not "Off"/"On") instead of the
                    // current state's name — the button's colour (green vs
                    // grey, below) already carries the on/off state, so
                    // repeating it as text would be redundant. Everything
                    // else through this branch (a value-menu button, or a
                    // binary-but-not-Off/On pair like Filter Mode's Dual
                    // LP/HP-LP) just shows its own current-value name alone
                    // — 2026-07-11 user call: names are expected to be
                    // self-explanatory on their own (bake any needed context
                    // straight into names=, e.g. Freq Range uses "Freq
                    // Lo"/"Freq Hi" as its actual names rather than the code
                    // prefixing a separate label on), so a code-level label
                    // prefix ("Mode Dual LP", "Filter A Poles 2 Pole") is
                    // unwanted duplication, not a genuine aid. The separate
                    // label line beneath the button is still skipped for
                    // ALL of these (toggle, binary, or value-menu — further
                    // down this loop), since the button's own face already
                    // says enough on its own. isToggle itself (declared
                    // above, alongside disabled/baseY) already excludes an
                    // asMenu dial.
                    const char * name = isToggle ? dial->label
                                : (dialVal < dial->nameCount) ? dial->names[dialVal]
                                                                                     : "?";

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
                    // sizing above. A toggle's own label is a single fixed
                    // string (not one of several names it might cycle
                    // through), so its own width is simply measured
                    // directly rather than scanning dial->names. Mutates
                    // dial->rect itself, not a separate local rect, so the
                    // clickable hit area (mouseHandle.c) always matches what's
                    // actually drawn. buttonHeight matches the 12.0 the dial
                    // label/value text below (further down this loop)
                    // renders at, so the button's own text reads at the same
                    // size as every other label on the panel, not an
                    // arbitrarily bigger one.
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
                    dial->rect.size     = {widest + padding, buttonHeight};

                    // RGB_GREY_7, not RGB_BACKGROUND_GREY — see
                    // render_page_tabs()'s own identical comment above:
                    // the latter is a dark grey in this build, too
                    // low-contrast for draw_button's fixed black text.
                    // disabled (see panel_dial_is_disabled() above) wins over
                    // the on/off green — a greyed-out button shouldn't still
                    // read as "on".
                    //
                    // A panel_dial_needs_value_menu() button opens a
                    // dropdown whose items are backgrounded in dial->colour
                    // (open_dial_value_menu(), menus.c) — the button itself
                    // now matches, 2026-07-13 user request, so it visually
                    // ties to the menu it opens. A plain binary (2-position,
                    // no dropdown — e.g. Dual LP/HP-LP) has no menu colour to
                    // match, so it stays RGB_GREY_7 same as an off toggle.
                    // draw_button() itself now picks black/white label text
                    // per contrasting_text_colour(), so an arbitrarily dark
                    // dial->colour here stays readable the same way the
                    // dropdown's own items do.
                    tRgb colour = disabled ? (tRgb)RGB_GREY_3
                                : (isToggle && (dialVal != 0)) ? (tRgb)RGB_GREEN_ON
                                : panel_dial_needs_value_menu(dial) ? dial->colour
                                : (tRgb)RGB_GREY_7;
                    draw_button(mainArea, dial->rect, name, colour);
                } else {
                    render_dial(mainArea, dial->rect, dialVal, dial->max, 0, disabled ? (tRgb)RGB_GREY_3 : dial->colour);
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
                    // Always shows BOTH equally-valid High readings, not
                    // just one — 2026-07-12 finding (see
                    // dialDisplaySignedHiLo's own panelConfig.h comment):
                    // EVERY High value has exactly one alias (High and
                    // High-64, or High and High+64, whichever lands in
                    // range) that produces bit-for-bit identical wire data,
                    // not just a narrow subset — so there's no "unambiguous
                    // case" to silently trust and no way to ever pick the
                    // definitively "right" one from the dump alone. Rather
                    // than silently showing one possibly-wrong number (the
                    // earlier version of this code), or always showing an
                    // uninformative "?" (equally true for every value, so
                    // no better), every render honestly shows both.
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
                    // See dialDisplaySigned's own comment in panelConfig.h —
                    // dialVal is the raw unsigned count; displayOffset
                    // recentres it to the signed range the synth's own
                    // front panel shows (e.g. 0-198 -> -99..+99).
                    snprintf(valBuf, sizeof(valBuf), "%d", (int)dialVal - dial->displayOffset);
                } else if (dial->display == dialDisplayCcNative) {
                    // "X (Y)" — X is the primary display value (0..max-1,
                    // the number actually sent/received on the wire — see
                    // synth_set_panel_dial_value()), Y is a SEPARATE,
                    // display-only number scaled via dumpNativeMax, purely
                    // so the on-screen reading matches the synth's own
                    // front-panel number too (e.g. the Z1's Filter Cutoff:
                    // wire is 0-127, but the synth's own LCD tops out at 99
                    // — see f1cut's own comment in z1.txt for the full
                    // derivation). Computed FRESH from the already-correct
                    // display value every render, not read from dial->
                    // nativeValue — that field is only ever populated by
                    // the nativeMax!=0 branches in apply_dial_wire_value()/
                    // synth_set_panel_dial_value(), which f1cut and its
                    // siblings deliberately do NOT use any more (nativeMax
                    // driving the actual CC byte was the real bug fixed
                    // 2026-07-13); reusing dumpNativeMax here instead keeps
                    // this purely a cosmetic, read-only second number that
                    // can never again silently feed back into what gets
                    // sent on the wire. Falls back to the older nativeValue-
                    // based reading only for a hypothetical dial that sets
                    // display=ccnative without dumpNativeMax — none exist in
                    // any device file as of this writing.
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
                // Skipped for a genuine Off/On toggle unconditionally — its
                // button face already shows dial->label, colour conveys the
                // state, so this would just repeat the exact same text. For
                // any OTHER button-style dial (binary or value-menu), only
                // skipped when the file explicitly marks it noLabel=1 (its
                // own current-value text is already self-explanatory on its
                // own, e.g. Filter A/B Pole Select's "2 Pole") — the DEFAULT
                // is to still show the label, since most value-menu buttons
                // are NOT self-explanatory without it (e.g. "Lower Key" on
                // Voyager's Menu Settings page means nothing without
                // knowing it's Keyboard Mode).
                bool   isSelfExplanatoryButton = (panel_dial_is_binary(dial) || panel_dial_needs_value_menu(dial))
                                                 && dial->noLabel;
                // A button-style dial never gets the valRect line above (its
                // face already shows the value) — its own label, when shown
                // at all, moves up into that now-empty +4.0 slot instead of
                // sitting at the lower +18.0 one a knob's label uses (a knob
                // needs +18.0 precisely BECAUSE +4.0 is already taken by its
                // value line). 2026-07-11 user request: don't leave a blank
                // gap above the label just because there's nothing else to
                // put there.
                bool   isButtonDial            = panel_dial_is_binary(dial) || panel_dial_needs_value_menu(dial)
                                                 || panel_dial_is_toggle(dial);
                double lblY                    = baseY + section->dialSize + (isButtonDial ? 4.0 : 18.0);

                // !isToggle, not !panel_dial_is_toggle(dial) — an asMenu
                // dial is structurally a genuine Off/On pair but no longer
                // shows its label on the button face (its face shows the
                // current value instead, see isToggle's own declaration
                // above), so it needs this label line same as any other
                // value-menu button, unlike a real toggle.
                if (!isToggle && !isSelfExplanatoryButton) {
                    tRectangle lblRect = {{dial->rect.coord.x, lblY},
                        {section->spacing,   12.0}};

                    // Explicit here, not left over from the valRect render_text
                    // above — that one's skipped for a binary button, which
                    // otherwise left the last colour draw_button() itself set
                    // (black, for the button's own text) in effect for this
                    // label too.
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
