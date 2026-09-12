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
// Notes: Docs/code-notes/mouseHandle.c.md - "// notes §k" refers there.

#include <stdint.h>

#include "defs.h"
#include "synthlibPopups.h"
#include "synthlibDefs.h"
#include "types.h"
#include "globalVars.h"
#include "utilsGraphics.h"
#include "menus.h"
#include "synthComms.h"
#include "synthGraphics.h"
#include "mouseHandle.h"
#include "appMenuBar.h"
#include "fileBrowser.h"
#include "bankBrowser.h"
#include "alertDialog.h"
#include "geometry.h"    // dial_drag_pixels_for_full_range() — the shared Shift-slows-the-drag policy
#include "inputState.h"

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
extern int glfwGetMouseButton(void *, int);   // for recover_lost_dial_drag()
extern void glfwGetWindowSize(void *, int *, int *);
extern void glfwGetCursorPos(void *, double *, double *);

// notes §1
static tPanelDial * gDraggedDial          = NULL;
// notes §2
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

// notes §3
static tPanelDial * gPressedToggleDial    = NULL;

// notes §4
static tPanelDial * gPressedValueMenuDial = NULL;

// notes §5
static void arm_dial_press(tPanelDial * dial, tCoord coord) {
    // notes §6
    if (dial->readOnly) {
        return;
    }

    // notes §7
    if (panel_dial_is_disabled(dial, synth_panel_config())) {
        return;
    }

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
    gDragPrevX     = coord.x;
    gDragPrevY     = coord.y;
    gDragTypeAccum = 0.0;

    if (synthlib_dial_mode() != eDialModeRotary) {
        gDragSkipCount = 3;
        glfwSetInputMode(synthlib_window(), GLFW_CURSOR, GLFW_CURSOR_DISABLED);
    }
}

// notes §8
static void synth_commit_prog_name_edit(void) {
    gProgNameEdit.active = false;
    synth_set_program_name(gProgNameEdit.buffer);
}

// ── Coordinate helpers ────────────────────────────────────────────────────────

// notes §9

// Supplied for SynthLib's contextMenu.c to link against — see mouseHandle.h.
void get_global_gui_scaled_mouse_coord(tCoord * coord) {
    synthlib_mouse_coord(coord);   // see inputState.h
}

// Scale a window-space delta to logical-space delta
// delta_to_logical() removed: the cursor handler receives logical coordinates now, so a delta of
// two of them is already logical. See gDragPrevX/gDragPrevY.

// notes §10

// notes §11

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

void panel_dial_press_click_handler(tCoord coord, eClickPhase phase, void * userData) {
    (void)coord;

    if (phase != eClickPress) {
        return; // release is handled entirely by the global armed-state check in handle_mouse_button()
    }
    tCoord at = {0};

    get_global_gui_scaled_mouse_coord(&at);   // logical, to match what the drag stores
    arm_dial_press((tPanelDial *)userData, at);
}

void page_tab_click_handler(tCoord coord, eClickPhase phase, void * userData) {
    (void)coord;

    if (phase != eClickPress) {
        return; // action fires on release, via the global armed-state check in handle_mouse_button()
    }
    int32_t index = (int32_t)(intptr_t)userData;

    gPressedTab = index;
    synth_set_pressed_page_tab(index);
}

void patch_nav_click_handler(tCoord coord, eClickPhase phase, void * userData) {
    (void)coord;

    if (phase != eClickPress) {
        return; // action fires on release, via the global armed-state check in handle_mouse_button()
    }
    int32_t index = (int32_t)(intptr_t)userData;

    gPressedPatchNav = index;
    synth_set_pressed_patch_nav(index);
}

void prog_name_click_handler(tCoord coord, eClickPhase phase, void * userData) {
    (void)coord;
    (void)userData;

    if (phase != eClickPress) {
        return;
    }
    gProgNameEdit.active    = true;

    // notes §12
    uint32_t o = 0;

    for (const char * p = gDevice.progName; (*p != '\0') && ((o + 1) < sizeof(gProgNameEdit.buffer)); p++) {
        if (*p != '\n') {
            gProgNameEdit.buffer[o++] = *p;
        }
    }

    gProgNameEdit.buffer[o] = '\0';
    gProgNameEdit.cursorPos = o;
}

// ── Public handlers ───────────────────────────────────────────────────────────

// Ends a dial drag. Both the real mouse release and recover_lost_dial_drag() call this, so the two
// cannot drift apart. Clears gDraggedDial BEFORE switching cursor mode, not after — belt and braces
// against any reentrant callback.
static void end_dial_drag(void * win) {
    gDragSkipCount = 0;
    gDraggedDial   = NULL;

    if (synthlib_dial_mode() != eDialModeRotary) {
        glfwSetInputMode(win, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
    }
}

// notes §13
void recover_lost_dial_drag(void * win) {
    if (gDraggedDial == NULL) {
        return;
    }

    if (glfwGetMouseButton(win, 0) == GLFW_PRESS) {   // left button genuinely still held
        return;
    }
    LOG_DEBUG("Recovering a dial drag whose release never arrived\n");
    end_dial_drag(win);
}

// The coordinate arrives already scaled and the button already decoded — SynthLib's shim does both,
// and updates the modifier state before this runs. See tSynthLibInputHandlers in synthlibWindow.h.
void handle_mouse_button(tCoord coord, tMouseButton button, int mods) {
    (void)mods;

    if ((button != mouseButtonLeftDown) && (button != mouseButtonLeftUp)) {
        return;   // left button only
    }
    bool pressed = (button == mouseButtonLeftDown);

    // notes §14
    if (synthlib_popups_dispatch_click(coord, pressed ? mouseButtonLeftDown : mouseButtonLeftUp)) {
        synthlib_request_redraw();
        return;
    }

    // notes §15
    if (!pressed && gDraggedDial) {
        end_dial_drag(synthlib_window());
        return;
    }
    // notes §16

    // Dismiss context menu
    if (gContextMenu.active) {
        // notes §17
        if (!pressed && within_rectangle(coord, app_menu_bar_rect())) {
            return;
        }
        handle_context_menu_click(coord); // closes the menu whether the click landed on an item or outside it
        return;
    }

    // notes §18
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

        // notes §19
        if (gPressedToggleDial && within_rectangle(coord, panel_dial_hit_rect(gPressedToggleDial))) {
            uint32_t current = get_panel_dial_value(gPressedToggleDial);

            synth_set_panel_dial_value(gPressedToggleDial, current ? 0 : 1);
        }
        gPressedToggleDial    = NULL;

        // notes §20
        if (gPressedValueMenuDial && within_rectangle(coord, panel_dial_hit_rect(gPressedValueMenuDial))) {
            open_dial_value_menu(coord, gPressedValueMenuDial);
        }
        gPressedValueMenuDial = NULL;
        return;
    }

    // notes §21
    if (dispatch_click_region(coord, eClickPress)) {
        return;
    }
    // notes §22
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
        arm_dial_press(hit, coord);
    }
}

void handle_cursor_pos(tCoord coord) {
    if (!gDraggedDial) {
        return;
    }

    if (gDragSkipCount > 0) {
        gDragPrevX = coord.x;
        gDragPrevY = coord.y;
        gDragSkipCount--;
        return;
    }
    uint32_t range  = gDraggedDial->max;
    int32_t  newVal = (int32_t)get_panel_dial_value(gDraggedDial);

    if (synthlib_dial_mode() == eDialModeRotary) {
        double angle = calculate_mouse_angle(coord, gDraggedDial->rect);
        newVal = (int32_t)angle_to_value(angle, range);
    } else if (gDraggedDial->display == dialDisplayNames) {
        // Discrete/stepped control (few positions): accumulate delta into
        // whole-step increments rather than mapping delta directly to value.
        double  delta = 0.0;

        if (synthlib_dial_mode() == eDialModeHorizontal) {
            delta      = coord.x - gDragPrevX;
            gDragPrevX = coord.x;
        } else {
            delta      = gDragPrevY - coord.y;
            gDragPrevY = coord.y;
        }
        gDragTypeAccum += delta / 30.0;
        int32_t step  = (int32_t)gDragTypeAccum;
        gDragTypeAccum -= (double)step;
        newVal         += step;
    } else if (synthlib_dial_mode() == eDialModeVertical) {
        // Shift = a slower drag over more pixels — see pixels_for_full_range() for the mapping and
        // for why its floor is what it is. The Clock Div bug that shaped it (Shift speeding the drag
        // up on a narrow dial, 2026-07-12) is recorded there too.
        double  pixelsForFullRange = dial_drag_pixels_for_full_range(range);
        double  dy                 = gDragPrevY - coord.y;

        gDragPrevY      = coord.y;
        // notes §23
        gDragTypeAccum += dy * (double)(range - 1) / pixelsForFullRange;
        int32_t step               = (int32_t)gDragTypeAccum;

        gDragTypeAccum -= (double)step;
        newVal         += step;
    } else {
        double  pixelsForFullRange = dial_drag_pixels_for_full_range(range);
        double  dx                 = coord.x - gDragPrevX;

        gDragPrevX      = coord.x;
        gDragTypeAccum += dx * (double)(range - 1) / pixelsForFullRange;
        int32_t step               = (int32_t)gDragTypeAccum;

        gDragTypeAccum -= (double)step;
        newVal         += step;
    }
    synth_set_panel_dial_value(gDraggedDial, clamp_dial_value(newVal, range));
}

void handle_key(int key, int scancode, int action, int mods) {
    (void)scancode;

    // Same cascade as the clicks, and gone the same way — including the Escape precedence that let
    // the alert's bank-picker dropdown close before the dialog under it. See synthlibPopups.h.
    if (synthlib_popups_dispatch_key(key, mods, action)) {
        synthlib_request_redraw();
        return;
    }

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
    synthlib_request_redraw();
}

void handle_char(unsigned int codepoint) {
    if (synthlib_popups_dispatch_char(codepoint)) {
        synthlib_request_redraw();
        return;
    }

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
    synthlib_request_redraw();
}

void handle_scroll(double dx, double dy) {
    (void)dx;

    if (synthlib_popups_dispatch_scroll(dy)) {
        return;
    }

    if (gDraggedDial) {
        return;
    }
    // notes §24
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

    if (dial && !dial->readOnly && !panel_dial_is_disabled(dial, cfg)) {
        int32_t newVal = (int32_t)get_panel_dial_value(dial) + (int32_t)dy;
        synth_set_panel_dial_value(dial, clamp_dial_value(newVal, dial->max));
    }
}
