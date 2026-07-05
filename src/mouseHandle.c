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

extern void glfwSetInputMode(void *, int, int);
extern void glfwSetCursorPos(void *, double, double);
extern void glfwGetWindowSize(void *, int *, int *);

// ── Dial drag state ───────────────────────────────────────────────────────────
// Deliberately just a pointer into whichever tPanelDial was hit — this file
// has no knowledge of what any given dial *is* (cutoff, resonance, routing,
// ...). That all comes from the descriptor parsed out of the layout file
// (see panelConfig.h/synthComms.c) and is looked up generically by rect.
static tPanelDial * gDraggedDial   = NULL;
static double       gDragStartX    = 0.0;   // cursor position at press — used for restore on release
static double       gDragStartY    = 0.0;
static double       gDragPrevX     = 0.0;   // cursor position at previous cursor_pos call — incremental delta
static double       gDragPrevY     = 0.0;
static int          gDragSkipCount = 0;     // skip first N cursor_pos events after CURSOR_DISABLED — covers stale events + transition event
static double       gDragTypeAccum = 0.0;   // sub-step accumulator for discrete (named) dials

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

    // Release: end drag; restore cursor only for modes that hid it
    if (!pressed && gDraggedDial) {
        gDragSkipCount = 0;

        if (gDialMode != eDialModeRotary) {
            glfwSetInputMode(win, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
            glfwSetCursorPos(win, gDragStartX, gDragStartY);
        }
        gDraggedDial   = NULL;
        return;
    }

    // Dismiss context menu
    if (close_context_menu_if_outside(coord)) {
        return;
    }

    if (handle_context_menu_click(coord)) {
        return;
    }

    if (!pressed) {
        return;
    }
    // Hit-test the current panel section generically — whatever dial (if
    // any) is under the cursor, by rect alone. No dial ids referenced here.
    tPanelDial * hit = NULL;

    if (gDevice.connected) {
        tPanelSection * section = synth_filters_section();
        int32_t         hitIdx  = section ? hit_test_panel_section(section, coord) : -1;

        if (hitIdx >= 0) {
            hit = &section->dials[hitIdx];
        }
    }

    if (hit) {
        gDraggedDial   = hit;
        gDragStartX    = x;
        gDragStartY    = y;
        gDragPrevX     = x;
        gDragPrevY     = y;
        gDragTypeAccum = 0.0;

        if (gDialMode != eDialModeRotary) {
            gDragSkipCount = 3;
            glfwSetInputMode(win, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        }
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
    (void)key;
    (void)action;
}

void handle_scroll(void * win, double dx, double dy) {
    (void)win;
    (void)dx;

    if (!gDevice.connected || gDraggedDial) {
        return;
    }
    // Deliberate remaining exception: with no drag active, scroll always
    // nudges "f1cut" specifically — a synth-editor shortcut, not something
    // generalizable from a rect-based hit-test, since handle_scroll isn't
    // given a cursor position to test against.
    tPanelSection * section = synth_filters_section();
    tPanelDial *    dial    = section ? find_panel_dial(section, "f1cut") : NULL;

    if (dial) {
        int32_t newVal = (int32_t)get_panel_dial_value(dial) + (int32_t)dy;
        synth_set_panel_dial_value(dial, clamp_dial_value(newVal, dial->max));
    }
}
