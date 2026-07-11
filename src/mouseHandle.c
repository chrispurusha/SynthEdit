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

#include "defs.h"
#include "synthlibDefs.h"
#include "types.h"
#include "globalVars.h"
#include "utilsGraphics.h"
#include "menus.h"
#include "synthComms.h"
#include "synthGraphics.h"
#include "mouseHandle.h"

// ── GLFW constants (avoids pulling GLFW header into C) ────────────────────────
#define GLFW_CURSOR             0x00033001
#define GLFW_CURSOR_NORMAL      0x00034001
#define GLFW_CURSOR_DISABLED    0x00034003
#define GLFW_PRESS              1
#define GLFW_RELEASE            0
#define GLFW_REPEAT             2
#define GLFW_KEY_ESCAPE         256
#define GLFW_KEY_ENTER          257
#define GLFW_KEY_KP_ENTER       335
#define GLFW_KEY_BACKSPACE      259
#define GLFW_KEY_DELETE         261
#define GLFW_KEY_RIGHT          262
#define GLFW_KEY_LEFT           263
#define GLFW_KEY_HOME           268
#define GLFW_KEY_END            269

extern void glfwSetInputMode(void *, int, int);
extern void glfwGetWindowSize(void *, int *, int *);
extern void glfwGetCursorPos(void *, double *, double *);

// ── Dial drag state ───────────────────────────────────────────────────────────
// Deliberately just a pointer into whichever tPanelDial was hit — this file
// has no knowledge of what any given dial *is* (cutoff, resonance, routing,
// ...). That all comes from the descriptor parsed out of the layout file
// (see panelConfig.h/synthComms.c) and is looked up generically by rect.
static tPanelDial * gDraggedDial          = NULL;
static double       gDragPrevX            = 0.0; // cursor position at previous cursor_pos call — incremental delta
static double       gDragPrevY            = 0.0;
static int          gDragSkipCount        = 0;   // skip first N cursor_pos events after CURSOR_DISABLED — covers stale events + transition event

// Page tab press state — actions on mouse-up, not mouse-down (standard
// button behaviour: press-and-drag-off cancels the click). -1 = no tab
// currently pressed.
static int32_t      gPressedTab           = -1;
static double       gDragTypeAccum        = 0.0; // sub-step accumulator for discrete (named) dials

// Same press-on-mouse-up convention as gPressedTab above, for the Prev/Next
// patch buttons (see synth_hit_test_patch_nav() in synthGraphics.h).
static int32_t      gPressedPatchNav      = -1;

// A panel_dial_is_binary() dial (rendered as a button, not a knob — either
// a power button or a plain named one, see synthGraphics.cpp) takes a
// single click rather than the drag gesture every other dial uses:
// pressing one arms it here instead of gDraggedDial, and releasing still
// over the same dial flips it (0<->1 — the only two states a binary dial
// has), same press-on-mouse-up convention as gPressedTab/gPressedPatchNav
// above. A dial that looks like a button but only responded to a knob's
// drag gesture wasn't actually clickable in practice with no synth
// connected to confirm otherwise.
static tPanelDial * gPressedToggleDial    = NULL;

// Same press-arms/release-fires shape as gPressedToggleDial above, for a
// panel_dial_needs_value_menu() dial — a discrete selector with no CC at
// all. Must open the menu on RELEASE, not press: opening it on press means
// the very same click's own release immediately dismisses it again (the
// "Dismiss context menu" check at the top of the release path closes
// whatever's active on ANY release) before a second click could ever reach
// it. Real bug hit and fixed 2026-07-08 building this.
static tPanelDial * gPressedValueMenuDial = NULL;

// Given a dial that was just hit on press — by EITHER hit-test source, the
// generic per-page grid loop or synth_hit_test_info_row() (both in
// handle_mouse_button() below) — arms whichever interaction it needs: a
// value-menu dropdown (3+ named positions, no CC), a click-to-toggle (2
// named positions, e.g. On/Off or, as of 2026-07-11, any 2-way enum like
// triggerMode's Single/Multi Trigger — panel_dial_is_binary() doesn't
// require the names to literally be "Off"/"On"), or a plain drag (anything
// else, e.g. a raw-numeric Info Row dial like midiClkDivider). Which
// hit-test found the dial doesn't change which of these three it needs —
// that's entirely the dial's own descriptor — so both call sites share
// this instead of each re-implementing the same 3-way branch. Previously
// only the per-page grid path had all three; the Info Row path only had
// the value-menu case, silently leaving a 2-option Info Row dial (like
// triggerMode) or a raw-numeric one (like midiClkDivider) with no click
// behaviour at all.
static void arm_dial_press(void * win, tPanelDial * dial, double x, double y) {
    if (panel_dial_needs_value_menu(dial)) {
        // Opens on RELEASE, not here — see gPressedValueMenuDial's own
        // comment above for why.
        gPressedValueMenuDial = dial;
        return;
    }

    if (panel_dial_is_binary(dial)) {
        gPressedToggleDial = dial;
        return;
    }
    gDraggedDial   = dial;
    gDragPrevX     = x;
    gDragPrevY     = y;
    gDragTypeAccum = 0.0;

    if (gDialMode != eDialModeRotary) {
        gDragSkipCount = 3;
        glfwSetInputMode(win, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
    }
}

// ── Program name edit (gProgNameEdit, globalVars.h) ──────────────────────────
// Shared by the Enter-key commit path (handle_key() below) and the
// click-outside-the-field commit path (handle_mouse_button() below) — see
// the latter's own comment for why a stray click commits rather than
// silently discarding or being ignored.
static void synth_commit_prog_name_edit(void) {
    gProgNameEdit.active = false;
    synth_set_program_name(gProgNameEdit.buffer);
}

// ── Coordinate helpers ────────────────────────────────────────────────────────

static tCoord window_to_logical(void * win, double x, double y) {
    int winW = 0;
    int winH = 0;

    glfwGetWindowSize(win, &winW, &winH);
    return (tCoord){
        .x = (winW > 0) ? (x / winW) * (get_render_width() / gGlobalGuiScale) : x,
        .y = (winH > 0) ? (y / winH) * (get_render_height() / gGlobalGuiScale) : y,
    };
}

// Supplied for SynthLib's contextMenu.c to link against — see mouseHandle.h.
void get_global_gui_scaled_mouse_coord(tCoord * coord) {
    double x = 0.0;
    double y = 0.0;

    glfwGetCursorPos(gWindow, &x, &y);
    *coord = window_to_logical(gWindow, x, y);
}

// Scale a window-space delta to logical-space delta
static double delta_to_logical(void * win, double winDelta, bool isX) {
    int winW = 0;
    int winH = 0;

    glfwGetWindowSize(win, &winW, &winH);

    if (isX) {
        return (winW > 0) ? (winDelta / winW) * (get_render_width() / gGlobalGuiScale) : winDelta;
    } else {
        return (winH > 0) ? (winDelta / winH) * (get_render_height() / gGlobalGuiScale) : winDelta;
    }
}

// Clamps to the dial's own display-space range [0, max-1] — the one thing
// every dial has in common, regardless of what it controls.
static uint32_t clamp_dial_value(int32_t v, uint32_t max) {
    if (v < 0) {
        return 0;
    }

    if ((max > 0) && ((uint32_t)v >= max)) {
        return max - 1;
    }
    return (uint32_t)v;
}

// ── Public handlers ───────────────────────────────────────────────────────────

void handle_mouse_button(void * win, int button, int action, int mods, double x, double y) {
    (void)mods;

    if (button != 0) {
        return;
    }
    tCoord coord   = window_to_logical(win, x, y);
    bool   pressed = (action == GLFW_PRESS);

    // Release: end drag; restore cursor only for modes that hid it. GLFW's
    // cocoa backend already restores the cursor to wherever it was when
    // CURSOR_DISABLED was entered (see updateCursorMode() in
    // cocoa_window.m) as soon as we switch back to NORMAL — an explicit
    // glfwSetCursorPos() here on top of that was redundant, and two
    // independent warps in a row (GLFW's automatic one, then ours) risked
    // landing a pixel or two off from each other, enough to spill into a
    // neighbouring dial given how tightly packed these are (40px dial,
    // 10px gap). Clear gDraggedDial before switching cursor mode, not
    // after — belt and braces against any reentrant callback.
    if (!pressed && gDraggedDial) {
        gDragSkipCount = 0;
        gDraggedDial   = NULL;

        if (gDialMode != eDialModeRotary) {
            glfwSetInputMode(win, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
        }
        return;
    }

    // Dismiss context menu
    if (gContextMenu.active) {
        handle_context_menu_click(coord); // closes the menu whether the click landed on an item or outside it
        return;
    }

    // A press outside the program-name field while mid-edit commits
    // whatever's typed so far — Enter/Escape (handle_key() below) are the
    // deliberate paths, but a stray click elsewhere in the UI shouldn't
    // strand the user in a half-finished edit with no visible way out.
    // Consumes the click (returns rather than falling through to whatever
    // else it landed on) — matches common text-field-blur convention, and
    // avoids the same click both committing a name AND, say, pressing Next.
    // A press back on the field itself falls through unhandled here; the
    // "Program name" press-check further down just re-arms the same active
    // state, a harmless no-op.
    if (pressed && gProgNameEdit.active && !synth_hit_test_prog_name(coord)) {
        synth_commit_prog_name_edit();
        return;
    }

    if (!pressed) {
        // Action a pressed tab only if release lands back on it — matches
        // standard button behaviour (press-and-drag-off cancels the click).
        if ((gPressedTab >= 0) && (synth_hit_test_page_tab(coord) == gPressedTab)) {
            synth_action_page_tab(gPressedTab);
        }
        gPressedTab           = -1;
        synth_set_pressed_page_tab(-1);

        if ((gPressedPatchNav >= 0) && (synth_hit_test_patch_nav(coord) == gPressedPatchNav)) {
            synth_action_patch_nav(gPressedPatchNav);
        }
        gPressedPatchNav      = -1;
        synth_set_pressed_patch_nav(-1);

        // Same press-and-release-on-the-same-target convention as above —
        // flips the dial only if the release also lands back on it.
        if (gPressedToggleDial && within_rectangle(coord, gPressedToggleDial->rect)) {
            uint32_t current = get_panel_dial_value(gPressedToggleDial);

            synth_set_panel_dial_value(gPressedToggleDial, current ? 0 : 1);
        }
        gPressedToggleDial    = NULL;

        // Opening the menu here, on release, is required, not just
        // convention-matching — the very next line of this same function
        // (the "Dismiss context menu" check above) unconditionally closes
        // whatever menu is gContextMenu.active on ANY release. Opening it
        // during press instead meant the release ending that same click
        // immediately dismissed the menu before it was ever visible to a
        // second click. Real bug hit and fixed 2026-07-08 building this.
        if (gPressedValueMenuDial && within_rectangle(coord, gPressedValueMenuDial->rect)) {
            open_dial_value_menu(coord, gPressedValueMenuDial);
        }
        gPressedValueMenuDial = NULL;
        return;
    }
    // Page-tab row is checked before dial hit-testing so a tab press can't
    // also be misread as a drag start. Just arms gPressedTab here — the
    // action itself fires on release above, not here on press.
    gPressedTab      = synth_hit_test_page_tab(coord);

    if (gPressedTab >= 0) {
        synth_set_pressed_page_tab(gPressedTab);
        return;
    }
    // Same reasoning as the page-tab row above — checked before dial
    // hit-testing so a Prev/Next press can't be misread as a drag start.
    gPressedPatchNav = synth_hit_test_patch_nav(coord);

    if (gPressedPatchNav >= 0) {
        synth_set_pressed_patch_nav(gPressedPatchNav);
        return;
    }

    // Program name field — checked before dial hit-testing, same reasoning
    // as the page-tab row and Prev/Next/Sync buttons above. No press/release
    // split the way gPressedValueMenuDial needs (see its own comment below)
    // — there's no menu here a same-click release could prematurely
    // dismiss, so edit mode starts immediately on press.
    if (synth_hit_test_prog_name(coord)) {
        gProgNameEdit.active    = true;

        // Seed from the FLAT name — gDevice.progName may contain
        // nameLineWidth's own display-only '\n's (see extract_moog_name(),
        // synthComms.c), stripped back out here since the wire format has
        // no real line breaks, only synth_render()'s own wrap-for-display.
        uint32_t o = 0;

        for (const char * p = gDevice.progName; (*p != '\0') && ((o + 1) < sizeof(gProgNameEdit.buffer)); p++) {
            if (*p != '\n') {
                gProgNameEdit.buffer[o++] = *p;
            }
        }

        gProgNameEdit.buffer[o] = '\0';
        gProgNameEdit.cursorPos = o;
        return;
    }
    // Info Row dials (e.g. Voyager's soundCategory) — a hidden-section dial
    // is deliberately excluded from synth_current_page_sections() (see that
    // function's own comment), so it never reaches the generic per-page
    // loop below and needs its own hit-test instead. arm_dial_press() above
    // gives it the same 3-way value-menu/toggle/drag treatment a normal
    // grid dial gets — release-time handling (toggle flip, menu open) is
    // already generic over gPressedToggleDial/gPressedValueMenuDial/
    // gDraggedDial regardless of which hit-test armed them, so nothing
    // else needs to change to support this.
    tPanelDial *    infoRowHit   = synth_hit_test_info_row(coord);

    if (infoRowHit) {
        arm_dial_press(win, infoRowHit, x, y);
        return;
    }
    // Hit-test every section on the current panel page generically —
    // whatever dial (if any) is under the cursor, by rect alone. No dial ids
    // referenced here. Not gated on gDevice.connected: dials are bound to
    // gDevice regardless of whether a real synth is talking to us, so the GUI
    // stays testable (dragging updates gDevice/tries a MIDI send that quietly
    // no-ops) even with nothing plugged in.
    tPanelDial *    hit          = NULL;
    tPanelSection * sections[PANEL_MAX_SECTIONS];
    uint32_t        sectionCount = synth_current_page_sections(sections, PANEL_MAX_SECTIONS);

    for (uint32_t s = 0; (s < sectionCount) && !hit; s++) {
        int32_t hitIdx = hit_test_panel_section(sections[s], coord);

        if (hitIdx >= 0) {
            hit = &sections[s]->dials[hitIdx];
        }
    }

    if (hit) {
        arm_dial_press(win, hit, x, y);
    }
}

void handle_cursor_pos(void * win, double x, double y) {
    if (!gDraggedDial) {
        return;
    }

    if (gDragSkipCount > 0) {
        gDragPrevX = x;
        gDragPrevY = y;
        gDragSkipCount--;
        return;
    }
    uint32_t range  = gDraggedDial->max;
    int32_t  newVal = (int32_t)get_panel_dial_value(gDraggedDial);

    if (gDialMode == eDialModeRotary) {
        tCoord logCoord = window_to_logical(win, x, y);
        double angle    = calculate_mouse_angle(logCoord, gDraggedDial->rect);
        newVal = (int32_t)angle_to_value(angle, range);
    } else if (gDraggedDial->display == dialDisplayNames) {
        // Discrete/stepped control (few positions): accumulate delta into
        // whole-step increments rather than mapping delta directly to value.
        double  delta = 0.0;

        if (gDialMode == eDialModeHorizontal) {
            delta      = delta_to_logical(win, x - gDragPrevX, true);
            gDragPrevX = x;
        } else {
            delta      = delta_to_logical(win, gDragPrevY - y, false);
            gDragPrevY = y;
        }
        gDragTypeAccum += delta / 30.0;
        int32_t step  = (int32_t)gDragTypeAccum;
        gDragTypeAccum -= (double)step;
        newVal         += step;
    } else if (gDialMode == eDialModeVertical) {
        double dy = delta_to_logical(win, gDragPrevY - y, false);
        gDragPrevY = y;
        newVal    += (int32_t)(dy * (double)(range - 1) / 200.0);
    } else {
        double dx = delta_to_logical(win, x - gDragPrevX, true);
        gDragPrevX = x;
        newVal    += (int32_t)(dx * (double)(range - 1) / 200.0);
    }
    synth_set_panel_dial_value(gDraggedDial, clamp_dial_value(newVal, range));
}

void handle_key(void * win, int key, int scancode, int action, int mods) {
    (void)win;
    (void)scancode;
    (void)mods;

    if (!gProgNameEdit.active) {
        return;
    }

    if ((action != GLFW_PRESS) && (action != GLFW_REPEAT)) {
        return;
    }
    size_t   len       = strlen(gProgNameEdit.buffer);
    uint32_t cursorPos = (gProgNameEdit.cursorPos <= len) ? gProgNameEdit.cursorPos : (uint32_t)len;

    if (key == GLFW_KEY_BACKSPACE) {
        if (cursorPos > 0) {
            memmove(&gProgNameEdit.buffer[cursorPos - 1], &gProgNameEdit.buffer[cursorPos], len - cursorPos + 1);
            gProgNameEdit.cursorPos = cursorPos - 1;
        }
    } else if (key == GLFW_KEY_DELETE) {
        if (cursorPos < len) {
            memmove(&gProgNameEdit.buffer[cursorPos], &gProgNameEdit.buffer[cursorPos + 1], len - cursorPos);
        }
    } else if (key == GLFW_KEY_LEFT) {
        if (cursorPos > 0) {
            gProgNameEdit.cursorPos = cursorPos - 1;
        }
    } else if (key == GLFW_KEY_RIGHT) {
        if (cursorPos < len) {
            gProgNameEdit.cursorPos = cursorPos + 1;
        }
    } else if (key == GLFW_KEY_HOME) {
        gProgNameEdit.cursorPos = 0;
    } else if (key == GLFW_KEY_END) {
        gProgNameEdit.cursorPos = (uint32_t)len;
    } else if ((key == GLFW_KEY_ENTER) || (key == GLFW_KEY_KP_ENTER)) {
        synth_commit_prog_name_edit();
    } else if (key == GLFW_KEY_ESCAPE) {
        // Cancel — discard edits
        gProgNameEdit.active = false;
    }
    gReDraw = true;
}

void handle_char(void * win, unsigned int codepoint) {
    (void)win;

    if (!gProgNameEdit.active) {
        return;
    }
    uint32_t maxLen    = synth_effective_name_maxlen();
    size_t   len       = strlen(gProgNameEdit.buffer);

    if ((codepoint < 0x20) || (codepoint > 0x7E) || (len >= maxLen) || ((len + 1) >= sizeof(gProgNameEdit.buffer))) {
        return;
    }
    uint32_t cursorPos = (gProgNameEdit.cursorPos <= len) ? gProgNameEdit.cursorPos : (uint32_t)len;

    memmove(&gProgNameEdit.buffer[cursorPos + 1], &gProgNameEdit.buffer[cursorPos], len - cursorPos + 1);
    gProgNameEdit.buffer[cursorPos] = (char)codepoint;
    gProgNameEdit.cursorPos         = cursorPos + 1;
    gReDraw                         = true;
}

void handle_scroll(void * win, double dx, double dy) {
    (void)win;
    (void)dx;

    if (gDraggedDial) {
        return;
    }
    // Deliberate remaining exception: with no drag active, scroll always
    // nudges one shortcut dial — not something generalizable from a
    // rect-based hit-test, since handle_scroll isn't given a cursor position
    // to test against. Which dial (if any) is entirely up to the device's own
    // <device>.txt ("scrollDial <id>" — empty/absent means no shortcut).
    // Only applies while the page holding that dial's section is actually
    // active — no-ops harmlessly otherwise.
    tPanelConfig *  cfg          = synth_panel_config();

    if (cfg->scrollDialId[0] == '\0') {
        return;
    }
    tPanelSection * sections[PANEL_MAX_SECTIONS];
    uint32_t        sectionCount = synth_current_page_sections(sections, PANEL_MAX_SECTIONS);
    tPanelDial *    dial         = NULL;

    for (uint32_t s = 0; (s < sectionCount) && !dial; s++) {
        dial = find_panel_dial(sections[s], cfg->scrollDialId);
    }

    if (dial) {
        int32_t newVal = (int32_t)get_panel_dial_value(dial) + (int32_t)dy;
        synth_set_panel_dial_value(dial, clamp_dial_value(newVal, dial->max));
    }
}
