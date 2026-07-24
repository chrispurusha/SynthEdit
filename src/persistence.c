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

// Small settings persistence that doesn't need Objective-C/Cocoa — goes
// through SynthLib's prefs.h (a plain "key=value" text file under a per-OS
// standard config directory) instead of NSUserDefaults, same reasoning as
// every other native-Cocoa-mechanism retirement in this pass.

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "defs.h"
#include "types.h"
#include "globalVars.h"
#include "graphics.h"
#include "misc.h"
#include "prefs.h"

// Must run before any other prefs_get_*/prefs_set_*/prefs_has_key call — see this function's own
// declaration comment (misc.h) for why it's called separately from, and well before,
// load_saved_settings() below.
void init_settings(void) {
    prefs_init("SynthEdit");
}

void save_dial_mode(int mode) {
    prefs_set_int("dialMode", mode);
}

void save_window_size(int w) {
    prefs_set_int("windowWidth", w);
}

void save_window_pos(int x, int y) {
    prefs_set_int("windowX", x);
    prefs_set_int("windowY", y);
}

// No security-scoped bookmark needed any more (App Sandbox removed) — a plain saved path survives
// relaunch fine, the same as every other prefs.h-backed setting here.
const char * get_saved_layouts_dir(void) {
    return prefs_get_string("layoutsDir", NULL);
}

void set_saved_layouts_dir(const char * path) {
    if (!path || (path[0] == '\0')) {
        return;
    }
    prefs_set_string("layoutsDir", path);
}

const char * get_saved_device_config(void) {
    return prefs_get_string("lastDeviceConfig", NULL);
}

void set_saved_device_config(const char * filename) {
    if (!filename || (filename[0] == '\0')) {
        return;
    }
    prefs_set_string("lastDeviceConfig", filename);
}

// Restores window/zoom/dial-mode/layouts-dir state saved from a previous run. Called once at
// startup from setup_main_menu() (misc.mm) — prefs_init() must run before this (also there), so
// the settings file is loaded before any of these get_* calls.
void load_saved_settings(void) {
    gDialMode = (tDialMode)prefs_get_int("dialMode", (long)gDialMode);

    long savedW = prefs_get_int("windowWidth", 0);

    if (savedW > 0) {
        int savedH = (int)savedW * TARGET_FRAME_BUFF_HEIGHT / TARGET_FRAME_BUFF_WIDTH;

        resize_window((int)savedW, savedH);
    }

    if (prefs_has_key("windowX") && prefs_has_key("windowY")) {
        int savedX = (int)prefs_get_int("windowX", 0);
        int savedY = (int)prefs_get_int("windowY", 0);

        reposition_window(savedX, savedY);
    }
}

// Key BASE for get_last_backup_folder()/set_last_backup_folder() below —
// see get_last_backup_folder()'s own comment (misc.h) for the deviceKey
// scoping scheme built on top of this.
static const char *const kLastBackupFolderKeyBase = "lastBackupFolder";

// Builds the actual per-device prefs key — see deviceKey's own comment in
// misc.h. NULL/empty deviceKey collapses to the bare base key
// (pre-2026-07-14 behaviour, and the fallback for any caller with no device
// context).
static const char * backup_folder_key(const char * deviceKey) {
    static char buf[128];

    if (!deviceKey || (deviceKey[0] == '\0')) {
        return kLastBackupFolderKeyBase;
    }
    snprintf(buf, sizeof(buf), "%s.%s", kLastBackupFolderKeyBase, deviceKey);
    return buf;
}

const char * get_last_backup_folder(const char * deviceKey) {
    const char * saved = prefs_get_string(backup_folder_key(deviceKey), NULL);

    if (!saved) {
        return NULL;
    }
    struct stat  st;

    if ((stat(saved, &st) != 0) || !S_ISDIR(st.st_mode)) {
        return NULL; // moved/deleted since last use — fall back to the system default rather than pointing at nothing
    }
    return saved;
}

void set_last_backup_folder(const char * deviceKey, const char * path) {
    if (!path || (path[0] == '\0')) {
        return;
    }
    prefs_set_string(backup_folder_key(deviceKey), path);
}
