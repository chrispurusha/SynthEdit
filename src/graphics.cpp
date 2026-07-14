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
#include "panelConfig.h"
#include "mouseHandle.h"
#include "menus.h"
#include "midiComms.h"
#include "synthComms.h"
#include "synthBackup.h"
#include "misc.h"
#include "graphics.h"

#include <stdio.h>
#include <unistd.h>

// stb_image_write is already bundled as a GLFW build dependency — reused
// here (single-header, so just #include-with-implementation) rather than
// pulling in a second PNG-writing library, purely for backdoor_screenshot()
// below.
#define STB_IMAGE_WRITE_IMPLEMENTATION
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#include "../SynthLib/ThirdParty/glfw/deps/stb_image_write.h"
#pragma clang diagnostic pop

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

static void char_cb(GLFWwindow * win, unsigned int codepoint) {
    handle_char(win, codepoint);
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

    // Tells SynthLib's utilsGraphics.cpp which colours/metrics this app
    // uses, without it needing to include this app's defs.h — see
    // configure_synthlib_theme(). Never called here before now: gTheme
    // (utilsGraphics.cpp) defaults to all-zero, so draw_power_button()'s
    // "green when on, grey when off" rendered both states as identical
    // black — the on/off toggle dials' value was changing correctly, only
    // the colour never visibly reflected it.
    configure_synthlib_theme((tSynthLibTheme){
        .topBarHeight   = TOP_BAR_HEIGHT,
        .orange1        = (tRgb)RGB_ORANGE_1,
        .orange2        = (tRgb)RGB_ORANGE_2,
        .greenOn        = (tRgb)RGB_GREEN_ON,
        .backgroundGrey = (tRgb)RGB_BACKGROUND_GREY,
    });

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
    glfwSetCharCallback((GLFWwindow *)gWindow, char_cb);
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

// ── Backdoor test-control channel ───────────────────────────────────────────
// Added 2026-07-13 at the owner's explicit request: a way for Claude to
// drive AND independently verify the running app — set the current page,
// poke a dial's value, switch device, or capture a screenshot — without a
// real mouse/window click, and without relying on a headless launch
// actually getting a GLFW paint event (unreliable in practice — several
// attempts this same session produced zero rendered frames when launched
// from a background shell with no window focus). This polls a plain text
// command file once per loop iteration (cheap: one access() check when
// idle) rather than opening a socket or spawning a listener thread — the
// existing ~50ms glfwWaitEventsTimeout() cadence already gives it a home
// with no new concurrency to reason about. Always active, not gated behind
// an env var/flag: this is a single-user local desktop app with no network
// exposure, and the command surface is narrow (page switch, dial set,
// device switch, one-shot state dump, screenshot) — nothing here can do
// anything a real mouse click couldn't already do.
//
// Command file format: one command per file, first line only —
// "<COMMAND> <rest of line as its argument>". Commands:
//   PAGE <page id>          — synth_set_current_page()
//   SET <dialId> <value>    — find_panel_dial_anywhere() + synth_set_panel_dial_value()
//   DEVICE <filename>       — synth_switch_device_config()
//   SYNC                    — synth_request_state_dump(), same as the real
//                             "Sync from synth" button — async, poll with
//                             a later DUMP to see the result once it lands
//   DUMP                    — current page + every dial on it: id, label,
//                             current value, rect (x/y/w/h) — the same
//                             numbers a screenshot would otherwise require
//                             visual inspection to read off
//   SCREENSHOT <path>       — forces an immediate render (not queued —
//                             synchronous, so the capture below always
//                             reflects whatever PAGE/SET/DEVICE command
//                             most recently ran, not a stale frame) then
//                             glReadPixels()+stbi_write_png() to path
// Writes "OK\n" (DUMP/nothing else appends its own text after) or
// "ERROR: <reason>\n" to the result file once the command's fully handled,
// then deletes the command file — so a caller can poll for the command
// file's disappearance (or just the result file's own presence/mtime) to
// know when it's safe to read the result.
// App Sandbox (com.apple.security.app-sandbox — SynthEdit.entitlements)
// makes a hardcoded plain "/tmp/..." path silently unreachable: fopen() on
// one just returns NULL, no error, no crash — confirmed the hard way
// (2026-07-13) testing this exact mechanism against a real running
// instance that never responded. synth_temp_dir() (misc.h) resolves to
// this app's own container tmp folder instead, which sandboxing DOES
// allow unrestricted read/write to.
static const char * backdoor_cmd_path(void) {
    static char path[1088];

    snprintf(path, sizeof(path), "%ssynthedit_cmd.txt", synth_temp_dir());
    return path;
}

static const char * backdoor_result_path(void) {
    static char path[1088];

    snprintf(path, sizeof(path), "%ssynthedit_result.txt", synth_temp_dir());
    return path;
}

static void backdoor_write_result(const char * text) {
    FILE * f = fopen(backdoor_result_path(), "w");

    if (f) {
        fputs(text, f);
        fclose(f);
    }
}

static void backdoor_screenshot(GLFWwindow * win, const char * path) {
    render_frame(win); // synchronous — see this whole block's own header comment for why

    int       w      = get_render_width();
    int       h      = get_render_height();

    if ((w <= 0) || (h <= 0)) {
        backdoor_write_result("ERROR: zero-size framebuffer\n");
        return;
    }
    uint8_t * pixels = (uint8_t *)malloc((size_t)w * (size_t)h * 3);

    if (!pixels) {
        backdoor_write_result("ERROR: out of memory\n");
        return;
    }
    glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, pixels);
    // OpenGL's origin is bottom-left; PNGs are conventionally read top-down.
    stbi_flip_vertically_on_write(1);

    int       ok     = stbi_write_png(path, w, h, 3, pixels, w * 3);

    free(pixels);

    if (ok) {
        backdoor_write_result("OK\n");
    } else {
        backdoor_write_result("ERROR: stbi_write_png failed\n");
    }
}

static void backdoor_dump_state(char * out, size_t outMax) {
    size_t          used         = 0;
    tPanelSection * sections[PANEL_MAX_SECTIONS];
    uint32_t        sectionCount = synth_current_page_sections(sections, PANEL_MAX_SECTIONS);

    used += (size_t)snprintf(out + used, outMax - used, "OK\npage=%s\n", synth_current_page());

    for (uint32_t s = 0; (s < sectionCount) && (used < outMax); s++) {
        tPanelSection * section = sections[s];

        for (uint32_t d = 0; (d < section->dialCount) && (used < outMax); d++) {
            tPanelDial * dial = &section->dials[d];

            used += (size_t)snprintf(out + used, outMax - used,
                                     "section=%s id=%s label=\"%s\" value=%u rect=%.1f,%.1f,%.1f,%.1f\n",
                                     section->section, dial->id, dial->label, get_panel_dial_value(dial),
                                     dial->rect.coord.x, dial->rect.coord.y, dial->rect.size.w, dial->rect.size.h);
        }
    }
}

static void backdoor_dispatch(const char * cmd, const char * arg, GLFWwindow * win) {
    if (strcmp(cmd, "PAGE") == 0) {
        synth_set_current_page(arg);
        gReDraw = true;
        backdoor_write_result("OK\n");
    } else if (strcmp(cmd, "SET") == 0) {
        // Parsed manually (not sscanf's "%Ns" with a hand-typed width) so
        // the copy bound always tracks PANEL_ID_LEN itself — a literal
        // width here already drifted silently out of sync once already
        // today when PANEL_ID_LEN moved 16->24 (found via this exact
        // command, see PANEL_ID_LEN's own comment in panelConfig.h).
        char         dialId[PANEL_ID_LEN] = {0};
        const char * sep                  = strchr(arg, ' ');

        if (!sep) {
            backdoor_write_result("ERROR: expected 'SET <dialId> <value>'\n");
            return;
        }
        size_t       idLen                = (size_t)(sep - arg);

        if (idLen >= sizeof(dialId)) {
            idLen = sizeof(dialId) - 1;
        }
        memcpy(dialId, arg, idLen);
        dialId[idLen] = '\0';

        uint32_t     value                = 0;

        if (sscanf(sep + 1, "%u", &value) != 1) {
            backdoor_write_result("ERROR: expected 'SET <dialId> <value>'\n");
            return;
        }
        tPanelDial * dial                 = find_panel_dial_anywhere(synth_panel_config(), dialId);

        if (!dial) {
            char msg[128];

            snprintf(msg, sizeof(msg), "ERROR: no dial '%s' in the current device config\n", dialId);
            backdoor_write_result(msg);
            return;
        }
        synth_set_panel_dial_value(dial, value);
        gReDraw       = true;
        backdoor_write_result("OK\n");
    } else if (strcmp(cmd, "DEVICE") == 0) {
        synth_switch_device_config(arg);
        backdoor_write_result("OK\n");
    } else if (strcmp(cmd, "SYNC") == 0) {
        // Same "Sync from synth" round trip the real button triggers
        // (synth_hit_test_patch_nav() index 2, synthGraphics.cpp) — re-
        // requests the current live state dump so a caller can verify a
        // just-SET value actually took (and was correctly re-decoded) on
        // real hardware, not just that the app's own local state changed.
        // Async: the reply lands over MIDI some ms later, so this only
        // confirms the REQUEST was sent, not that the dump has arrived —
        // a caller should wait briefly, then poll with DUMP to see the
        // result once it lands.
        if (!synth_dump_patch_in_flight()) {
            synth_request_state_dump();
        }
        backdoor_write_result("OK\n");
    } else if (strcmp(cmd, "DUMP") == 0) {
        char dump[16384];

        backdoor_dump_state(dump, sizeof(dump));
        backdoor_write_result(dump);
    } else if (strcmp(cmd, "SCREENSHOT") == 0) {
        backdoor_screenshot(win, arg);
    } else if (strcmp(cmd, "KORGSELECT") == 0) {
        // Testing-only command, added 2026-07-14 to verify the new Z1
        // Load-Patch-from-Bank wire mechanism (Bank Select + Program
        // Change) directly, bypassing the "Load Patch from Bank…" menu's
        // own name-sweep + native modal picker — that picker would hang
        // this backdoor's own poll loop (a blocking NSAlert has no
        // headless way to click through it). SAFE to test unattended:
        // selecting a program only changes what's LIVE/playing, same as
        // choosing a preset on the front panel — nothing stored is
        // overwritten (contrast KORGWRITE, deliberately NOT exposed here
        // the same way, since a Program Write Request DOES overwrite a
        // real stored slot and needs the owner's own explicit go-ahead
        // each time, not a scriptable backdoor command).
        uint32_t bank = 0;
        uint32_t prog = 0;

        if (sscanf(arg, "%u %u", &bank, &prog) != 2) {
            backdoor_write_result("ERROR: expected 'KORGSELECT <bank 0|1> <prog 1-128>'\n");
            return;
        }
        synth_korg_select_program((uint8_t)bank, prog);
        backdoor_write_result("OK\n");
    } else if (strcmp(cmd, "RESTOREEDITBUFFER") == 0) {
        // Testing-only command, added 2026-07-14 to verify the new Korg-
        // style "Restore Edit Buffer" mechanism (synthBackup.c) directly,
        // bypassing "File > Open File…"'s own NSOpenPanel —
        // same "a native modal has no headless way to click through it"
        // reasoning KORGSELECT above already gives. Safe to test
        // unattended for the same reason KORGSELECT is — see synth_
        // backup_restore_edit_buffer_from_path()'s own comment, synthBackup.h.
        if (arg[0] == '\0') {
            backdoor_write_result("ERROR: expected 'RESTOREEDITBUFFER <path>'\n");
            return;
        }
        synth_backup_restore_edit_buffer_from_path(arg);
        backdoor_write_result("OK\n");
    } else if (strcmp(cmd, "RESTOREPATCHTOBANK") == 0) {
        // Testing-only command, added 2026-07-14 — same reasoning
        // RESTOREEDITBUFFER above gives, but for the "Load Patch File to
        // Bank Slot" mechanism (now dual-device, Moog and Korg), which has
        // TWO native modals (file picker, then a named-slot picker) plus a
        // confirmation, none scriptable headlessly. UNLIKE RESTOREEDITBUFFER,
        // this DOES write to a stored slot — only ever exercise this
        // against tools/z1_emulator.swift or tools/voyager_emulator.swift,
        // never a real connected device (see synth_backup_restore_patch_
        // to_bank_from_path()'s own comment, synthBackup.h).
        uint32_t bank          = 0;
        uint32_t prog          = 0;
        char     filePath[512] = {0};

        if (sscanf(arg, "%511s %u %u", filePath, &bank, &prog) != 3) {
            backdoor_write_result("ERROR: expected 'RESTOREPATCHTOBANK <path> <bank 0|1> <prog 1-128>'\n");
            return;
        }
        synth_backup_restore_patch_to_bank_from_path(filePath, (uint8_t)bank, prog);
        backdoor_write_result("OK\n");
    } else if (strcmp(cmd, "BACKUPPATCHNUMBER") == 0) {
        // Testing-only command, added 2026-07-14 to verify the new Z1 "Save
        // Patch by Number to File…" mechanism (synth_backup_patch_by_number_korg(),
        // synthBackup.c) directly, bypassing choose_korg_preset_number()'s own
        // native modal picker — same "no headless way to click through it"
        // reasoning KORGSELECT/RESTOREEDITBUFFER above already give. The
        // eventual save-location NSSavePanel (open_file_write_dialogue_async())
        // still can't be scripted headlessly either, so this only confirms
        // the request went out correctly and a sensible default filename got
        // computed — check the app's own debug log / z1_emulator's request
        // log for that. Safe to test unattended: this only READS a stored
        // program, nothing is written.
        uint32_t bank = 0;
        uint32_t prog = 0;

        if (sscanf(arg, "%u %u", &bank, &prog) != 2) {
            backdoor_write_result("ERROR: expected 'BACKUPPATCHNUMBER <bank 0|1> <prog 1-128>'\n");
            return;
        }
        synth_backup_patch_by_number_korg((uint8_t)bank, prog);
        backdoor_write_result("OK\n");
    } else {
        char msg[128];

        snprintf(msg, sizeof(msg), "ERROR: unknown command '%s'\n", cmd);
        backdoor_write_result(msg);
    }
}

static void backdoor_poll(GLFWwindow * win) {
    const char * cmdPath   = backdoor_cmd_path();

    if (access(cmdPath, F_OK) != 0) {
        return;
    }
    FILE *       f         = fopen(cmdPath, "r");

    if (!f) {
        return;
    }
    char         line[512] = {0};

    if (!fgets(line, sizeof(line), f)) {
        line[0] = '\0';
    }
    fclose(f);
    remove(cmdPath);

    size_t       len       = strlen(line);

    while ((len > 0) && ((line[len - 1] == '\n') || (line[len - 1] == '\r'))) {
        line[--len] = '\0';
    }
    char         cmd[32]   = {0};
    char *       space     = strchr(line, ' ');

    if (space) {
        size_t cmdLen = (size_t)(space - line);

        if (cmdLen >= sizeof(cmd)) {
            cmdLen = sizeof(cmd) - 1;
        }
        memcpy(cmd, line, cmdLen);
        cmd[cmdLen] = '\0';
        backdoor_dispatch(cmd, space + 1, win);
    } else {
        strncpy(cmd, line, sizeof(cmd) - 1);
        backdoor_dispatch(cmd, "", win);
    }
}

// ── do_graphics_loop ──────────────────────────────────────────────────────────

void do_graphics_loop(void) {
    GLFWwindow * win = (GLFWwindow *)gWindow;

    while (!gQuitAll && !glfwWindowShouldClose(win)) {
        // Commits any quantized switch/selector dial's debounced CC once
        // it's gone quiet for CC_DEBOUNCE_MS (synthComms.c) — needs a
        // periodic check even with no real GLFW event, hence
        // glfwWaitEventsTimeout() below rather than glfwWaitEvents()'s
        // indefinite block. Cheap: a handful of integer comparisons per
        // dial, not worth gating behind "is anything actually pending".
        synth_flush_pending_cc();
        // Same per-frame check, for a dump-only dial's debounced outgoing
        // patch-and-resend (e.g. Voyager's Headphone Volume) — see
        // hasPendingDumpSend's own comment in panelConfig.h.
        synth_flush_pending_dump_sends();
        // Sends a Korg-style Parameter Change that was deferred because a
        // name-sweep request was in flight at the time — see
        // synth_flush_pending_param_send()'s own comment (synthComms.h).
        synth_flush_pending_param_send();
        // Advances an in-progress Backup > Bank (Individual Files)… sweep
        // (next preset request, or a per-preset timeout) — see
        // synth_backup_flush_bank_to_folder()'s own comment (synthBackup.h).
        synth_backup_flush_bank_to_folder();
        // Advances an in-progress Korg-style (Z1) Load/Store Patch from/to
        // Bank name sweep — see synth_backup_flush_korg_name_sweep()'s own
        // comment (synthBackup.h).
        synth_backup_flush_korg_name_sweep();
        // Silently starts a name sweep (Korg or Moog, whichever the
        // connected device uses) shortly after connecting, so it's already
        // partly (or fully) cached by the time the user clicks Load/Store
        // Patch from Bank — see synth_backup_flush_background_prefetch()'s
        // own comment (synthBackup.h).
        synth_backup_flush_background_prefetch();
        // Advances an in-progress Restore > Bank (Individual Files)… sweep
        // (paced sends, no reply to wait for) — see
        // synth_backup_flush_restore_folder()'s own comment (synthBackup.h).
        synth_backup_flush_restore_folder();
        // Korg-style (Z1) counterpart to the above — see
        // synth_backup_flush_korg_restore_folder()'s own comment (synthBackup.h).
        synth_backup_flush_korg_restore_folder();
        // Finishes an in-progress "Store Patch to Bank…" fetch once its
        // fresh Panel Dump reply has landed — see
        // synth_backup_flush_store()'s own comment (synthBackup.h).
        synth_backup_flush_store();
        // Tracks which context-menu item the mouse is currently over and
        // requests a redraw when it changes — contextMenu.h's own comment
        // (SynthLib) explicitly documents this as required "once per frame
        // from the main render loop"; SynthEdit never actually called it
        // anywhere until now (2026-07-11 user report: the hover highlight on
        // a value-menu dropdown didn't track the mouse the way it does on
        // G2-Edit). A no-op cheap early-return when no menu is open
        // (gContextMenu.active check lives inside the function itself), so
        // unconditional every frame is fine — also needed for the
        // hover-dwell submenu-opening timer to elapse even while the mouse
        // sits still over a flyout parent, per that same header comment.
        update_context_menu_hover();
        // See this whole mechanism's own header comment (backdoor_poll()
        // above) — cheap no-op check every iteration when idle.
        backdoor_poll(win);

        bool reDraw = atomic_exchange(&gReDraw, false);

        if (reDraw) {
            render_frame(win);
        }
        glfwWaitEventsTimeout(0.05);
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
