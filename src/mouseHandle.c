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

#include "defs.h"
#include "types.h"
#include "globalVars.h"
#include "utilsGraphics.h"
#include "menus.h"
#include "midiComms.h"
#include "z1Comms.h"
#include "z1Graphics.h"
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
typedef enum {
    eDragNone,
    eDragFilter1Type,
    eDragFilter1Cutoff,
    eDragFilter1Res,
    eDragFilter2Type,
    eDragFilter2Cutoff,
    eDragFilter2Res,
} tDragTarget;

static tDragTarget gDragTarget    = eDragNone;
static double      gDragStartX    = 0.0;   // cursor position at press — used for restore on release
static double      gDragStartY    = 0.0;
static double      gDragPrevX     = 0.0;   // cursor position at previous cursor_pos call — incremental delta
static double      gDragPrevY     = 0.0;
static double      gDragTypeAccum = 0.0;   // sub-step accumulator for discrete type dial


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

// ── Dial value helpers ────────────────────────────────────────────────────────

static uint8_t clamp_u8(int v) {
    if (v < 0) {
        return 0;
    }

    if (v > 127) {
        return 127;
    }
    return (uint8_t)v;
}

static uint8_t clamp_type(int v) {
    if (v < 1) {
        return 1;
    }

    if (v > 5) {
        return 5;
    }
    return (uint8_t)v;
}

static void set_filter_param(uint8_t * ccField, uint8_t * nativeField, uint8_t cc, uint8_t value) {
    if (value == *ccField) {
        return;
    }
    *ccField     = value;
    *nativeField = (uint8_t)(value * 99UL / 127);
    midi_send_cc(gDevice.id, cc, value);
    atomic_store(&gReDraw, true);
}

static void set_filter1_type(uint8_t v) {
    if (v == gDevice.filter1Type) {
        return;
    }
    gDevice.filter1Type = v;
    z1_send_parameter_change(Z1_PARAM_GROUP_PROG, Z1_PARAM_FILTER1_TYPE, v);
    atomic_store(&gReDraw, true);
}

static void set_filter2_type(uint8_t v) {
    if (v == gDevice.filter2Type) {
        return;
    }
    gDevice.filter2Type = v;
    z1_send_parameter_change(Z1_PARAM_GROUP_PROG, Z1_PARAM_FILTER2_TYPE, v);
    atomic_store(&gReDraw, true);
}

static void set_filter1_cutoff(uint8_t v) {
    set_filter_param(&gDevice.filter1Cutoff, &gDevice.filter1CutoffNative, 0x55, v);
}

static void set_filter1_res(uint8_t v) {
    set_filter_param(&gDevice.filter1Resonance, &gDevice.filter1ResNative, 0x56, v);
}

static void set_filter2_cutoff(uint8_t v) {
    set_filter_param(&gDevice.filter2Cutoff, &gDevice.filter2CutoffNative, 0x58, v);
}

static void set_filter2_res(uint8_t v) {
    set_filter_param(&gDevice.filter2Resonance, &gDevice.filter2ResNative, 0x59, v);
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
    if (!pressed && (gDragTarget != eDragNone)) {
        if (gDialMode != eDialModeRotary) {
            glfwSetInputMode(win, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
            glfwSetCursorPos(win, gDragStartX, gDragStartY);
        }
        gDragTarget = eDragNone;
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
    // Hit-test filter dials
    tDragTarget hitTarget = eDragNone;

    if (gDevice.connected) {
        if (within_rectangle(coord, z1_filter1_type_dial_rect())) {
            hitTarget = eDragFilter1Type;
        } else if (within_rectangle(coord, z1_filter1_dial_rect())) {
            hitTarget = eDragFilter1Cutoff;
        } else if (within_rectangle(coord, z1_filter1_res_dial_rect())) {
            hitTarget = eDragFilter1Res;
        } else if (within_rectangle(coord, z1_filter2_type_dial_rect())) {
            hitTarget = eDragFilter2Type;
        } else if (within_rectangle(coord, z1_filter2_dial_rect())) {
            hitTarget = eDragFilter2Cutoff;
        } else if (within_rectangle(coord, z1_filter2_res_dial_rect())) {
            hitTarget = eDragFilter2Res;
        }
    }

    if (hitTarget != eDragNone) {
        gDragTarget    = hitTarget;
        gDragStartX    = x;
        gDragStartY    = y;
        gDragPrevX     = x;
        gDragPrevY     = y;
        gDragTypeAccum = 0.0;

        if (gDialMode != eDialModeRotary) {
            glfwSetInputMode(win, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        }
    }
}

void handle_cursor_pos(void * win, double x, double y) {
    if (gDragTarget == eDragNone) {
        return;
    }

    // Type dials: always use vertical delta with sub-step accumulator (no CC)
    if ((gDragTarget == eDragFilter1Type) || (gDragTarget == eDragFilter2Type)) {
        double dy   = delta_to_logical(win, gDragPrevY - y, false);
        gDragPrevY      = y;
        gDragTypeAccum += dy / 30.0;     // ~30 logical px per step
        int    step = (int)gDragTypeAccum;
        gDragTypeAccum -= (double)step;  // keep remainder

        if (step != 0) {
            if (gDragTarget == eDragFilter1Type) {
                set_filter1_type(clamp_type((int)gDevice.filter1Type + step));
            } else {
                set_filter2_type(clamp_type((int)gDevice.filter2Type + step));
            }
        }
        return;
    }
    // Current CC value for the dragged dial (for delta modes)
    uint8_t    curVal   = 0;

    switch (gDragTarget) {
        case eDragFilter1Cutoff: curVal = gDevice.filter1Cutoff;
            break;
        case eDragFilter1Res:    curVal = gDevice.filter1Resonance;
            break;
        case eDragFilter2Cutoff: curVal = gDevice.filter2Cutoff;
            break;
        case eDragFilter2Res:    curVal = gDevice.filter2Resonance;
            break;
        default:                 return;
    }
    // Dial rect for rotary mode
    tRectangle dialRect = {0};

    switch (gDragTarget) {
        case eDragFilter1Cutoff: dialRect = z1_filter1_dial_rect();
            break;
        case eDragFilter1Res:    dialRect = z1_filter1_res_dial_rect();
            break;
        case eDragFilter2Cutoff: dialRect = z1_filter2_dial_rect();
            break;
        case eDragFilter2Res:    dialRect = z1_filter2_res_dial_rect();
            break;
        default:                 return;
    }
    int        newVal   = 0;

    if (gDialMode == eDialModeRotary) {
        tCoord logCoord = window_to_logical(win, x, y);
        double angle    = calculate_mouse_angle(logCoord, dialRect);
        newVal = (int)angle_to_value(angle, 128);
    } else if (gDialMode == eDialModeVertical) {
        double dy = delta_to_logical(win, gDragPrevY - y, false);
        gDragPrevY = y;
        newVal     = (int)curVal + (int)(dy * 127.0 / 200.0);
    } else {
        double dx = delta_to_logical(win, x - gDragPrevX, true);
        gDragPrevX = x;
        newVal     = (int)curVal + (int)(dx * 127.0 / 200.0);
    }

    switch (gDragTarget) {
        case eDragFilter1Cutoff: set_filter1_cutoff(clamp_u8(newVal));
            break;
        case eDragFilter1Res:    set_filter1_res(clamp_u8(newVal));
            break;
        case eDragFilter2Cutoff: set_filter2_cutoff(clamp_u8(newVal));
            break;
        case eDragFilter2Res:    set_filter2_res(clamp_u8(newVal));
            break;
        default:                                                        break;
    }
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

    if (!gDevice.connected) {
        return;
    }

    // Scroll anywhere nudges the filter dial when no drag is active
    if (gDragTarget == eDragNone) {
        int newVal = (int)gDevice.filter1Cutoff + (int)dy;
        set_filter1_cutoff(clamp_u8(newVal));
    }
}
