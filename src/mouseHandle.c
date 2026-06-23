/*
 * The Z1-Edit application.
 *
 * Copyright (C) 2025 Chris Turner <chris_purusha@icloud.com>
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
#include "z1Graphics.h"
#include "mouseHandle.h"

// ── GLFW constants (avoids pulling GLFW header into C) ────────────────────────
#define GLFW_CURSOR            0x00033001
#define GLFW_CURSOR_NORMAL     0x00034001
#define GLFW_CURSOR_DISABLED   0x00034003
#define GLFW_PRESS             1
#define GLFW_RELEASE           0

extern void glfwSetInputMode(void *, int, int);
extern void glfwSetCursorPos(void *, double, double);

// ── Dial drag state ───────────────────────────────────────────────────────────
typedef enum {
    eDragNone,
    eDragFilter1,
} tDragTarget;

static tDragTarget gDragTarget  = eDragNone;
static double      gDragStartX  = 0.0;
static double      gDragStartY  = 0.0;
static uint8_t     gDragStartVal = 0;

// ── Coordinate helpers ────────────────────────────────────────────────────────

static tCoord window_to_logical(void * win, double x, double y) {
    int winW = 0;
    int winH = 0;
    extern void glfwGetWindowSize(void *, int *, int *);
    glfwGetWindowSize(win, &winW, &winH);
    return (tCoord){
               .x = (winW > 0) ? (x / winW) * TARGET_FRAME_BUFF_WIDTH : x,
               .y = (winH > 0) ? (y / winH) * TARGET_FRAME_BUFF_HEIGHT : y,
    };
}

// Scale a window-space delta to logical-space delta
static double delta_to_logical(void * win, double winDelta, bool isX) {
    int winW = 0;
    int winH = 0;
    extern void glfwGetWindowSize(void *, int *, int *);
    glfwGetWindowSize(win, &winW, &winH);
    if (isX) {
        return (winW > 0) ? (winDelta / winW) * TARGET_FRAME_BUFF_WIDTH : winDelta;
    } else {
        return (winH > 0) ? (winDelta / winH) * TARGET_FRAME_BUFF_HEIGHT : winDelta;
    }
}

// ── Dial value helpers ────────────────────────────────────────────────────────

static uint8_t clamp_u8(int v) {
    if (v < 0) return 0;
    if (v > 127) return 127;
    return (uint8_t)v;
}

static void set_filter1_cutoff(uint8_t value) {
    if (value == gDevice.filter1Cutoff) {
        return;
    }
    gDevice.filter1Cutoff       = value;
    gDevice.filter1CutoffNative = (uint8_t)(value * 99UL / 127);
    midi_send_cc(gDevice.id, 0x55, value);
    atomic_store(&gReDraw, true);
}

// ── Public handlers ───────────────────────────────────────────────────────────

void handle_mouse_button(void * win, int button, int action, int mods, double x, double y) {
    (void)mods;
    if (button != 0) {
        return;
    }
    tCoord coord   = window_to_logical(win, x, y);
    bool   pressed = (action == GLFW_PRESS);

    // Release: always end drag and restore cursor
    if (!pressed && (gDragTarget != eDragNone)) {
        glfwSetInputMode(win, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
        glfwSetCursorPos(win, gDragStartX, gDragStartY);
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

    // Hit-test filter 1 dial
    if (gDevice.connected && within_rectangle(coord, z1_filter1_dial_rect())) {
        gDragTarget   = eDragFilter1;
        gDragStartX   = x;
        gDragStartY   = y;
        gDragStartVal = gDevice.filter1Cutoff;
        glfwSetInputMode(win, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
    }
}

void handle_cursor_pos(void * win, double x, double y) {
    if (gDragTarget == eDragNone) {
        return;
    }
    // In CURSOR_DISABLED mode x/y are unbounded virtual coords
    double dy = delta_to_logical(win, gDragStartY - y, false);
    // 200 logical units = full 0-127 sweep
    int newVal = (int)gDragStartVal + (int)(dy * 127.0 / 200.0);

    if (gDragTarget == eDragFilter1) {
        set_filter1_cutoff(clamp_u8(newVal));
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
