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
#include "synthlibDefs.h"
#include "types.h"
#include "globalVars.h"
#include "utils.h"
#include "utilsGraphics.h"
#include "synthGraphics.h"
#include "mouseHandle.h"
#include "menus.h"
#include "midiComms.h"
#include "misc.h"
#include "graphics.h"

static float gContentScale = 2.0f;

static void setup_projection(GLFWwindow * win);

// ── GLFW callbacks ────────────────────────────────────────────────────────────

void framebuffer_size_callback(GLFWwindow * window, int width, int height) {
    // Update the OpenGL viewport to match the current framebuffer size
    glViewport(0, 0, width, height);

    set_render_width(width);   // Inform utilsGraphics
    set_render_height(height); // Inform utilsGraphics
    gGlobalGuiScale = (double)gContentScale * (double)width / (double)TARGET_FRAME_BUFF_WIDTH;

    // Configure a 2D orthographic projection in framebuffer pixels
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, width, height, 0, -1, 1);

    // Restore the model-view matrix ready for normal rendering
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    gReDraw         = true;
}

void window_size_callback(GLFWwindow * window, int width, int height) {
    save_window_size(width);
}

void window_pos_callback(GLFWwindow * window, int x, int y) {
    save_window_pos(x, y);
}

void resize_window(int w, int h) {
    glfwSetWindowSize((GLFWwindow *)gWindow, w, h);
}

void reposition_window(int x, int y) {
    glfwSetWindowPos((GLFWwindow *)gWindow, x, y);
}

void window_close_callback(GLFWwindow * window) {
    gReDraw = false;

    glfwSetFramebufferSizeCallback((GLFWwindow *)gWindow, NULL);
    glfwSetWindowCloseCallback((GLFWwindow *)gWindow, NULL);
    glfwSetKeyCallback((GLFWwindow *)gWindow, NULL);
    glfwSetCharCallback((GLFWwindow *)gWindow, NULL);
    glfwSetCursorPosCallback((GLFWwindow *)gWindow, NULL);
    glfwSetMouseButtonCallback((GLFWwindow *)gWindow, NULL);
    glfwSetScrollCallback((GLFWwindow *)gWindow, NULL);

    glfwSetWindowShouldClose((GLFWwindow *)gWindow, GLFW_TRUE);
    glfwPostEmptyEvent();
}

void set_window_title(const char * filePath) {
    char         newTitle[100] = {0};
    const char * filename      = strrchr(filePath, '/');

    if (filename) {
        filename += 1;  // Skip the slash
    } else {
        filename = filePath;
    }
    snprintf(newTitle, sizeof(newTitle), "%s - %s", WINDOW_TITLE, filename);
    glfwSetWindowTitle((GLFWwindow *)gWindow, newTitle);
}

void error_callback(int error, const char * description) {
    LOG_ERROR("GLFW error [%d]: %s\n", error, description);
}

static void window_refresh_cb(GLFWwindow * win) {
    (void)win;
    gReDraw = true;
}

static void mouse_button_cb(GLFWwindow * win, int button, int action, int mods) {
    double x = 0.0;
    double y = 0.0;

    glfwGetCursorPos(win, &x, &y);
    handle_mouse_button(win, button, action, mods, x, y);
    gReDraw = true;
}

static void cursor_pos_cb(GLFWwindow * win, double x, double y) {
    handle_cursor_pos(win, x, y);
}

static void key_cb(GLFWwindow * win, int key, int scancode, int action, int mods) {
    handle_key(win, key, scancode, action, mods);
    gReDraw = true;
}

static void scroll_cb(GLFWwindow * win, double dx, double dy) {
    handle_scroll(win, dx, dy);
    gReDraw = true;
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

    //gGlobalGuiScale = (winW > 0) ? (double)fbW / (double)winW : 1.0;

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
    char title[128] = {0};

    snprintf(title, sizeof(title), "%s - Build %s %s", WINDOW_TITLE, __DATE__, __TIME__);

    glfwSetErrorCallback(error_callback);

    if (!glfwInit()) {
        exit(EXIT_FAILURE);
    }
    //register_glfw_wake_cb(wake_glfw);
    //register_full_patch_change_notify_cb(notify_full_patch_change);
    //topbar_init_controls();

    glfwWindowHint(GLFW_COCOA_RETINA_FRAMEBUFFER, GLFW_TRUE);
    glfwWindowHint(GLFW_COCOA_GRAPHICS_SWITCHING, GLFW_TRUE);  // Needed for Intel systems with discrete graphics
    gWindow = (void *)glfwCreateWindow(TARGET_FRAME_BUFF_WIDTH / 4, TARGET_FRAME_BUFF_HEIGHT / 4, title, NULL, NULL);

    if (!gWindow) {
        glfwTerminate();
        exit(EXIT_FAILURE);
    }
    glfwSetWindowSizeLimits((GLFWwindow *)gWindow, TARGET_FRAME_BUFF_WIDTH / 8, TARGET_FRAME_BUFF_HEIGHT / 8, GLFW_DONT_CARE, GLFW_DONT_CARE);
    glfwSetWindowAspectRatio((GLFWwindow *)gWindow, TARGET_FRAME_BUFF_WIDTH, TARGET_FRAME_BUFF_HEIGHT);

    glfwMakeContextCurrent((GLFWwindow *)gWindow);

    {
        int fbWidth  = 0;
        int fbHeight = 0;
        glfwGetFramebufferSize((GLFWwindow *)gWindow, &fbWidth, &fbHeight);
        framebuffer_size_callback((GLFWwindow *)gWindow, fbWidth, fbHeight);
    }

    glfwSetFramebufferSizeCallback((GLFWwindow *)gWindow, framebuffer_size_callback);
    glfwSetWindowSizeCallback((GLFWwindow *)gWindow, window_size_callback);
    glfwSetWindowPosCallback((GLFWwindow *)gWindow, window_pos_callback);
    glfwSwapInterval(1);
    glfwSetWindowCloseCallback((GLFWwindow *)gWindow, window_close_callback);
    glfwSetKeyCallback((GLFWwindow *)gWindow, key_cb);
    glfwSetCursorPosCallback((GLFWwindow *)gWindow, cursor_pos_cb);
    glfwSetMouseButtonCallback((GLFWwindow *)gWindow, mouse_button_cb);
    glfwSetScrollCallback((GLFWwindow *)gWindow, scroll_cb);
    glfwSetWindowRefreshCallback((GLFWwindow *)gWindow, window_refresh_cb);

    glEnable(GL_BLEND);               // TODO - Assess if G2 edit could benefit from this
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    init_font();                      // TODO - G2 edit could benefit from this if we're loading multiple fonts
    synth_init_graphics();            // TODO - do we need to call this, since it's currently empty?

    register_midi_wake_cb(wake_glfw); // TODO - this doesn't belong in here
}
// ── Render frame ──────────────────────────────────────────────────────────────

static void render_frame(GLFWwindow * win) {
    setup_projection(win);

    glClearColor(0.15f, 0.15f, 0.15f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    double     logW = (double)get_render_width() / gGlobalGuiScale;
    double     logH = (double)get_render_height() / gGlobalGuiScale;

    // LCD area: 2× the raw 240×64 pixel size, centred in left half of virtual space
    //double     lcdDispW = LCD_WIDTH * 2.0;
    //double     lcdDispH = LCD_HEIGHT * 2.0;
    //double     lcdX     = (logW / 2.0 - lcdDispW) / 2.0;
    //double     lcdY     = 10.0;
    tRectangle area = {{0.0, 0.0}, {logW, logH}};

    synth_render(area);
    render_context_menu();

    glfwSwapBuffers(win);
}

// ── do_graphics_loop ──────────────────────────────────────────────────────────

void do_graphics_loop(void) {
    GLFWwindow * win = (GLFWwindow *)gWindow;

    while (!gQuitAll && !glfwWindowShouldClose(win)) {
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
