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
// Notes: Docs/code-notes/graphics.c.md - "// notes §k" refers there.

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
#include "synthlibWindow.h"
#include "synthlibPopups.h"
#include "midiPortDialog.h"
#include "synthGraphics.h"
#include "panelConfig.h"
#include "mouseHandle.h"
#include "menus.h"
#include "midiComms.h"
#include "synthComms.h"
#include "synthBackup.h"
#include "misc.h"
#include "graphics.h"
#include "contextMenu.h"
#include <strings.h>
#include "appMenuBar.h"
#include "fileBrowser.h"
#include "bankBrowser.h"
#include "alertDialog.h"
#include "synthlibHost.h"
#include "synthlibScale.h"
#include "synthlibPersistence.h"
#include "inputState.h"

#include <stdio.h>
#include <unistd.h>

// notes §1
#define STB_IMAGE_WRITE_IMPLEMENTATION
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#include "../SynthLib/ThirdParty/glfw/deps/stb_image_write.h"
#pragma clang diagnostic pop

static void setup_projection(GLFWwindow * win);

// ── GLFW callbacks ────────────────────────────────────────────────────────────

void resize_window(int w, int h) {
    glfwSetWindowSize((GLFWwindow *)synthlib_window(), w, h);
}

void reposition_window(int x, int y) {
    glfwSetWindowPos((GLFWwindow *)synthlib_window(), x, y);
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
    glfwSetWindowTitle((GLFWwindow *)synthlib_window(), newTitle);
}

// notes §2
static void on_window_focus(bool focused) {
    if (!focused) {
        set_modifier_state((uint32_t)eModifierNone);
    }
}

static void on_window_refresh(void) {
    synthlib_request_redraw();
}

// notes §3


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

    render_backend_set_surface(fbW, fbH);
}


// ── Font ──────────────────────────────────────────────────────────────────────

static int init_font(void) {
    static const char * fontPaths[] = {
        "/System/Library/Fonts/Supplemental/Arial.ttf",
        "/System/Library/Fonts/Helvetica.ttc",
        "/System/Library/Fonts/SFNSMono.ttf",
        NULL
    };

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

    // notes §4
    synthlib_popups_set_menu_bar(gAppMenuBar, app_menu_bar_rect);
    synthlib_popups_register(midi_port_dialog_popup(), 1);

    synthlib_window_create(&(tSynthLibWindowConfig){
        .title        = title,
        .targetWidth  = TARGET_FRAME_BUFF_WIDTH,
        .targetHeight = TARGET_FRAME_BUFF_HEIGHT,
        .dialMode     = eDialModeVertical,
        .theme        = (tSynthLibTheme){
            .topBarHeight   = TOP_BAR_HEIGHT,
            .orange1        = (tRgb)RGB_ORANGE_1,
            .orange2        = (tRgb)RGB_ORANGE_2,
            .greenOn        = (tRgb)RGB_GREEN_ON,
            .backgroundGrey = (tRgb)RGB_BACKGROUND_GREY,
        },
        .mouseCoord   = get_global_gui_scaled_mouse_coord,
        .handlers     = &(const tSynthLibInputHandlers){
            .mouseButton   = handle_mouse_button,
            .cursorPos     = handle_cursor_pos,
            .key           = handle_key,
            .character     = handle_char,
            .scroll        = handle_scroll,
            .windowFocus   = on_window_focus,
            .windowRefresh = on_window_refresh,
        },
    }, NULL);

    init_font();                      // TODO - G2 edit could benefit from this if we're loading multiple fonts
    synth_init_graphics();            // TODO - do we need to call this, since it's currently empty?

    register_midi_wake_cb(wake_glfw); // TODO - this doesn't belong in here
}

// ── Render frame ──────────────────────────────────────────────────────────────

static void render_frame(GLFWwindow * win) {
    setup_projection(win);

    render_backend_clear((tRgb){0.15, 0.15, 0.15});

    clear_click_regions();

    double     logW = (double)get_render_width() / gGlobalGuiScale;
    double     logH = (double)get_render_height() / gGlobalGuiScale;

    // LCD area: 2× the raw 240×64 pixel size, centred in left half of virtual space
    //double     lcdDispW = LCD_WIDTH * 2.0;
    //double     lcdDispH = LCD_HEIGHT * 2.0;
    //double     lcdX     = (logW / 2.0 - lcdDispW) / 2.0;
    //double     lcdY     = 10.0;
    tRectangle area = {{0.0, MENU_BAR_HEIGHT}, {logW, logH - MENU_BAR_HEIGHT}};

    synth_render(area);
    // The BAR stays here rather than moving into the coordinator: it is chrome, and anything that
    // floats does so above it. Everything that pops UP goes through the coordinator, back to front
    // by layer instead of by the order of these calls — see synthlibPopups.h.
    render_menu_bar(gAppMenuBar, app_menu_bar_rect());
    synthlib_popups_render();

    // Submits the frame's one vertex array and puts it on screen. This was a render_backend_flush()
    // followed by glfwSwapBuffers() — the last GLFW call in this loop, and one that cannot exist
    // under Metal, where the window is created with no context to swap. See utilsGraphics.h.
    render_present();
}

// notes §5
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

// Does `wanted` appear anywhere in `label`, case-insensitively? Matched anywhere rather than only
// at the front because menu labels can carry a leading marker ("* " for a current selection), and a
// leading-substring match could then never name the thing being selected.
static bool backdoor_label_contains(const char * label, const char * wanted) {
    size_t wantedLength = strlen(wanted);
    size_t labelLength  = strlen(label);
    size_t at           = 0;

    if ((wantedLength == 0) || (wantedLength > labelLength)) {
        return false;
    }

    for (at = 0; at <= (labelLength - wantedLength); at++) {
        if (strncasecmp(label + at, wanted, wantedLength) == 0) {
            return true;
        }
    }

    return false;
}

// notes §6
static void backdoor_menu(const char * arg) {
    char        part[3][64] = {0};
    uint32_t    partCount   = 0;
    uint32_t    b           = 0;
    tMenuItem * items       = NULL;

    {
        const char * p = arg;

        while ((partCount < 3) && (*p != '\0')) {
            const char * slash  = strchr(p, '/');
            size_t       length = (slash != NULL) ? (size_t)(slash - p) : strlen(p);

            while ((length > 0) && (*p == ' ')) {
                p++;
                length--;
            }

            while ((length > 0) && (p[length - 1] == ' ')) {
                length--;
            }

            if (length >= sizeof(part[0])) {
                length = sizeof(part[0]) - 1;
            }
            memcpy(part[partCount], p, length);
            part[partCount][length] = '\0';
            partCount++;

            if (slash == NULL) {
                break;
            }
            p                       = slash + 1;
        }
    }

    if (partCount == 0) {
        backdoor_write_result("ERROR: expected 'MENU <bar>[/<item>[/<subitem>]]'\n");
        return;
    }

    for (b = 0; gAppMenuBar[b].label != NULL; b++) {
        if (backdoor_label_contains(gAppMenuBar[b].label, part[0]) == true) {
            break;
        }
    }

    if (gAppMenuBar[b].label == NULL) {
        backdoor_write_result("ERROR: no such menu\n");
        return;
    }
    // Populates gContextMenu with the items that menu would show RIGHT NOW, which is what makes
    // state-dependent labels ("Use Metal Renderer") matchable at all.
    gAppMenuBar[b].open((tCoord){0.0, 0.0});
    items = (gContextMenu.depth > 0) ? gContextMenu.frame[0].items : NULL;

    {
        uint32_t level = 1;

        while ((level < partCount) && (items != NULL)) {
            uint32_t i     = 0;
            bool     found = false;

            for (i = 0; items[i].label != NULL; i++) {
                if (backdoor_label_contains(items[i].label, part[level]) == false) {
                    continue;
                }
                found = true;

                if (level == (partCount - 1)) {
                    if (items[i].subMenu != NULL) {
                        items = items[i].subMenu;
                        break;
                    }
                    {
                        void (*action)(int index) = items[i].action;

                        // action() callbacks read gContextMenu.items[index].param, so point that at
                        // the array the item lives in — the same thing a real click does.
                        gContextMenu.items = items;

                        if (action == NULL) {
                            close_context_menu();
                            backdoor_write_result("ERROR: item is disabled\n");
                            return;
                        }
                        action((int)i);
                        close_context_menu();
                        synthlib_request_redraw();
                        backdoor_write_result("OK\n");
                        return;
                    }
                }

                if (items[i].subMenu == NULL) {
                    close_context_menu();
                    backdoor_write_result("ERROR: that item has no submenu\n");
                    return;
                }
                items = items[i].subMenu;
                break;
            }

            if (found == false) {
                close_context_menu();
                backdoor_write_result("ERROR: no such item\n");
                return;
            }
            level++;
        }
    }

    // Nothing left to click: list what the level we reached contains.
    {
        char     list[2048] = {0};
        size_t   used       = 0;
        uint32_t i          = 0;

        used += (size_t)snprintf(list + used, sizeof(list) - used, "OK\n");

        for (i = 0; (items != NULL) && (items[i].label != NULL) && (used < sizeof(list)); i++) {
            used += (size_t)snprintf(list + used, sizeof(list) - used, "%s%s\n",
                                     items[i].label, (items[i].subMenu != NULL) ? " >" : "");
        }

        close_context_menu();
        backdoor_write_result(list);
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

    // notes §7
    if (!render_backend_read_pixels_rgb(0, 0, w, h, pixels)) {
        free(pixels);
        backdoor_write_result("ERROR: frame read-back failed\n");
        return;
    }
    // Read-back origin is bottom-left; PNGs are conventionally read top-down.
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
        synthlib_request_redraw();
        backdoor_write_result("OK\n");
    } else if (strcmp(cmd, "SET") == 0) {
        // notes §8
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
        synthlib_request_redraw();
        backdoor_write_result("OK\n");
    } else if (strcmp(cmd, "DEVICE") == 0) {
        synth_switch_device_config(arg);
        backdoor_write_result("OK\n");
    } else if (strcmp(cmd, "SYNC") == 0) {
        // notes §9
        if (!synth_dump_patch_in_flight()) {
            synth_request_state_dump();
        }
        backdoor_write_result("OK\n");
    } else if (strcmp(cmd, "ALERT") == 0) {
        // notes §10
        if (!alert_dialog_active()) {
            backdoor_write_result("ERROR: no dialogue is open\n");
        } else {
            bool confirm = (arg[0] == '\0') || (strcasecmp(arg, "ok") == 0) || (strcasecmp(arg, "confirm") == 0);

            handle_alert_dialog_key(confirm ? GLFW_KEY_ENTER : GLFW_KEY_ESCAPE, GLFW_PRESS);
            synthlib_request_redraw();
            backdoor_write_result(confirm ? "OK\nconfirmed\n" : "OK\ncancelled\n");
        }
    } else if (strcmp(cmd, "DUMP") == 0) {
        char dump[16384];

        backdoor_dump_state(dump, sizeof(dump));
        backdoor_write_result(dump);
    } else if (strcmp(cmd, "SCREENSHOT") == 0) {
        backdoor_screenshot(win, arg);
    } else if (strcmp(cmd, "MENU") == 0) {
        backdoor_menu(arg);
    } else if (strcmp(cmd, "KORGSELECT") == 0) {
        // notes §11
        uint32_t bank = 0;
        uint32_t prog = 0;

        if (sscanf(arg, "%u %u", &bank, &prog) != 2) {
            backdoor_write_result("ERROR: expected 'KORGSELECT <bank 0|1> <prog 1-128>'\n");
            return;
        }
        synth_korg_select_program((uint8_t)bank, prog);
        backdoor_write_result("OK\n");
    } else if (strcmp(cmd, "RESTOREEDITBUFFER") == 0) {
        // notes §12
        if (arg[0] == '\0') {
            backdoor_write_result("ERROR: expected 'RESTOREEDITBUFFER <path>'\n");
            return;
        }
        synth_backup_restore_edit_buffer_from_path(arg);
        backdoor_write_result("OK\n");
    } else if (strcmp(cmd, "RESTOREPATCHTOBANK") == 0) {
        // notes §13
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
        // notes §14
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
    GLFWwindow * win = (GLFWwindow *)synthlib_window();

    while (!synthlib_quit_requested() && !glfwWindowShouldClose(win)) {
        // notes §15
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
        // notes §16
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
        // notes §17
        synth_backup_flush_pending_save();
        // notes §18
        synthlib_popups_tick();

        // A gesture whose release went missing never survives a frame: a stuck gDraggedDial means
        // every mouse move keeps editing that dial and sending it to the synth.
        recover_lost_dial_drag(synthlib_window());
        // See this whole mechanism's own header comment (backdoor_poll()
        // above) — cheap no-op check every iteration when idle.
        backdoor_poll(win);

        bool reDraw = synthlib_consume_redraw();

        if (reDraw) {
            render_frame(win);
        }
        glfwWaitEventsTimeout(0.05);
    }
}

// ── clean_up_graphics ─────────────────────────────────────────────────────────

void clean_up_graphics(void) {
    free_textures();
    glfwDestroyWindow((GLFWwindow *)synthlib_window());
    synthlib_set_window(NULL);
    glfwTerminate();
}

#ifdef __cplusplus
}
#endif
