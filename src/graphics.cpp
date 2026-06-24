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

#ifdef __cplusplus
extern "C" {
#endif

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#define GL_SILENCE_DEPRECATION    1
#include <GLFW/glfw3.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#pragma clang diagnostic pop

#include "defs.h"
#include "types.h"
#include "globalVars.h"
#include "utils.h"
#include "utilsGraphics.h"
#include "z1Graphics.h"
#include "mouseHandle.h"
#include "menus.h"
#include "midiComms.h"
#include "misc.h"
#include "graphics.h"


static void setup_projection(GLFWwindow * win);

// ── GLFW callbacks ────────────────────────────────────────────────────────────

static void framebuffer_size_cb(GLFWwindow * win, int w, int h) {
    (void)w;
    (void)h;
    setup_projection(win);
    atomic_store(&gReDraw, true);
}

static void window_refresh_cb(GLFWwindow * win) {
    (void)win;
    atomic_store(&gReDraw, true);
}

static void mouse_button_cb(GLFWwindow * win, int button, int action, int mods) {
    double x = 0.0;
    double y = 0.0;

    glfwGetCursorPos(win, &x, &y);
    handle_mouse_button(win, button, action, mods, x, y);
    atomic_store(&gReDraw, true);
}

static void cursor_pos_cb(GLFWwindow * win, double x, double y) {
    handle_cursor_pos(win, x, y);
}

static void key_cb(GLFWwindow * win, int key, int scancode, int action, int mods) {
    handle_key(win, key, scancode, action, mods);
    atomic_store(&gReDraw, true);
}

static void scroll_cb(GLFWwindow * win, double dx, double dy) {
    handle_scroll(win, dx, dy);
    atomic_store(&gReDraw, true);
}

// ── Wake (called from MIDI thread) ───────────────────────────────────────────

void wake_glfw(void) {
    glfwPostEmptyEvent();
}

// ── Projection ────────────────────────────────────────────────────────────────

static void setup_projection(GLFWwindow * win) {
    int fbW  = 0;
    int fbH  = 0;

    glfwGetFramebufferSize(win, &fbW, &fbH);

    int winW = 0;
    int winH = 0;
    glfwGetWindowSize(win, &winW, &winH);

    gGlobalGuiScale = (winW > 0) ? (double)fbW / (double)winW : 1.0;

    set_render_width(fbW);
    set_render_height(fbH);

    glViewport(0, 0, fbW, fbH);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0.0, fbW, fbH, 0.0, -1.0, 1.0);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
}

// ── Font ──────────────────────────────────────────────────────────────────────

static int init_font(void) {
    static const char * fontPaths[] = {"/System/Library/Fonts/Supplemental/Arial.ttf",
                                       "/System/Library/Fonts/Helvetica.ttc",
                                       "/System/Library/Fonts/SFNSMono.ttf",
                                       NULL};

    for (int i = 0; fontPaths[i] != NULL; i++) {
        if (preload_glyph_textures(fontPaths[i], 72.0)) {
            LOG_DEBUG("Loaded font: %s\n", fontPaths[i]);
            return EXIT_SUCCESS;
        }
    }

    LOG_ERROR("Could not load any system font\n");
    return EXIT_FAILURE;
}

// ── init_graphics ─────────────────────────────────────────────────────────────

void init_graphics(void) {
    int           windowWidth  = 0;
    int           windowHeight = 0;
    GLFWmonitor * monitor      = NULL;
    float         xScale       = 1.0f;
    float         yScale       = 1.0f;

    if (!glfwInit()) {
        LOG_ERROR("glfwInit failed\n");
        exit(EXIT_FAILURE);
    }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 2);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 1);
    glfwWindowHint(GLFW_COCOA_RETINA_FRAMEBUFFER, GLFW_TRUE);
    glfwWindowHint(GLFW_COCOA_GRAPHICS_SWITCHING, GLFW_TRUE);

    monitor      = glfwGetPrimaryMonitor();

    int           x = 0, y = 0, width = 0, height = 0;
    glfwGetMonitorWorkarea(monitor, &x, &y, &width, &height);
    glfwGetMonitorContentScale(monitor, &xScale, &yScale);
    windowWidth  = (int)((double)TARGET_FRAME_BUFF_WIDTH / xScale);
    windowHeight = (int)((double)TARGET_FRAME_BUFF_HEIGHT / yScale);

    if ((windowWidth > width) || (windowHeight > height)) {
        double widthSF  = (double)windowWidth / (double)width;
        double heightSF = (double)windowHeight / (double)height;
        double sf       = (heightSF >= widthSF) ? heightSF : widthSF;
        windowWidth  = (int)((double)windowWidth / sf);
        windowHeight = (int)((double)windowHeight / sf);
    }
    GLFWwindow *  win = glfwCreateWindow(windowWidth, windowHeight, WINDOW_TITLE, NULL, NULL);

    if (win == NULL) {
        LOG_ERROR("glfwCreateWindow failed\n");
        glfwTerminate();
        exit(EXIT_FAILURE);
    }
    gWindow = win;

    glfwMakeContextCurrent(win);
    glfwSwapInterval(1);

    glfwSetFramebufferSizeCallback(win, framebuffer_size_cb);
    glfwSetWindowRefreshCallback(win, window_refresh_cb);
    glfwSetMouseButtonCallback(win, mouse_button_cb);
    glfwSetCursorPosCallback(win, cursor_pos_cb);
    glfwSetKeyCallback(win, key_cb);
    glfwSetScrollCallback(win, scroll_cb);

    setup_projection(win);
    glfwSetWindowSizeLimits(win, windowWidth, windowHeight, GLFW_DONT_CARE, GLFW_DONT_CARE);
    glfwSetWindowAspectRatio(win, windowWidth, windowHeight);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    init_font();
    z1_init_graphics();

    register_midi_wake_cb(wake_glfw);
}

// ── Render frame ──────────────────────────────────────────────────────────────

static void render_frame(GLFWwindow * win) {
    setup_projection(win);

    glClearColor(0.15f, 0.15f, 0.15f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    double     logW     = (double)get_render_width();
    double     logH     = TARGET_FRAME_BUFF_HEIGHT;

    // LCD area: 2× the raw 240×64 pixel size, centred in left half of virtual space
    //double     lcdDispW = LCD_WIDTH * 2.0;
    //double     lcdDispH = LCD_HEIGHT * 2.0;
    //double     lcdX     = (logW / 2.0 - lcdDispW) / 2.0;
    //double     lcdY     = 10.0;
    tRectangle area = {{0.0, 0.0}, {logW, logH}};

    z1_render(area);
    render_context_menu();

    glfwSwapBuffers(win);
}

// ── do_graphics_loop ──────────────────────────────────────────────────────────

void do_graphics_loop(void) {
    GLFWwindow * win = (GLFWwindow *)gWindow;

    while (!atomic_load(&gQuitAll) && !glfwWindowShouldClose(win)) {
        bool reDraw = atomic_exchange(&gReDraw, false);

        if (reDraw) {
            render_frame(win);
        }
        glfwWaitEvents();
    }
}

// ── clean_up_graphics ─────────────────────────────────────────────────────────

void clean_up_graphics(void) {
    free_textures();
    glfwDestroyWindow((GLFWwindow *)gWindow);
    gWindow = NULL;
    glfwTerminate();
}

#ifdef __cplusplus
}
#endif
