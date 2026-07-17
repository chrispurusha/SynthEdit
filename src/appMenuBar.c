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

#include <stddef.h>
#include <string.h>
#include <stdio.h>

#include "defs.h"
#include "synthlibDefs.h"
#include "types.h"
#include "utilsGraphics.h"
#include "contextMenu.h"
#include "globalVars.h"
#include "panelConfig.h"
#include "synthComms.h"
#include "synthGraphics.h"
#include "synthBackup.h"
#include "midiComms.h"
#include "misc.h"
#include "graphics.h"
#include "fileBrowser.h"
#include "appMenuBar.h"

// ── File menu ────────────────────────────────────────────────────────────────
// Every single-patch operation lives here regardless of whether the other end is a file or a bank
// slot, matching this menu's own pre-port structure (misc.mm, retired): Open/Load paired, then
// Save/Store paired, then the three "no edit buffer involved" file<->slot operations.

static void action_open_file(int index) {
    (void)index;
    synth_backup_restore_edit_buffer();
}

static void action_load_patch_from_bank(int index) {
    (void)index;
    synth_backup_start_name_sweep(eNameSweepPurposeLoad);
}

static void action_save_patch(int index) {
    (void)index;
    synth_backup_current_patch();
}

static void action_store_patch_to_bank(int index) {
    (void)index;
    synth_backup_start_name_sweep(eNameSweepPurposeStore);
}

static void action_restore_patch(int index) {
    (void)index;
    synth_backup_restore_patch();
}

static void action_restore_patch_to_bank(int index) {
    (void)index;
    synth_backup_restore_patch_to_bank();
}

// "Save Patch by Number to File..." flyout — a flat 1-128 (Moog) or A001-B128 (Korg) grid of bare
// numbers, no names (contrast the richer Load/Store Patch pickers, synthBackup.c, which use
// SynthLib's bankBrowser.h instead — this one is meant to be quick). Rebuilt fresh every time the
// File menu itself opens (build_preset_number_items(), called from open_file_menu() below) so it
// always reflects whichever device is currently connected. Same multi-column-list-of-many-items
// pattern src/menus.c's own open_dial_value_menu() already uses (12 rows per column before
// wrapping into more columns) for exactly this shape of problem.
#define PRESET_NUMBER_ITEM_COUNT    256 // Korg's 2 banks x 128; a Moog-style device only ever fills the first 128
static tMenuItem gPresetNumberItems[PRESET_NUMBER_ITEM_COUNT + 1];
static char      gPresetNumberLabels[PRESET_NUMBER_ITEM_COUNT][8];

static void action_preset_number_chosen(int index) {
    if (synth_panel_config()->moogStyleDump) {
        synth_backup_patch_by_number((uint32_t)(index + 1));
    } else {
        synth_backup_patch_by_number_korg((uint8_t)(index / 128), (uint32_t)((index % 128) + 1));
    }
}

static void build_preset_number_items(uint32_t * outColumns) {
    bool           moog             = synth_panel_config()->moogStyleDump;
    uint32_t       count            = moog ? 128 : 256;

    const uint32_t maxRowsPerColumn = 12;

    for (uint32_t i = 0; i < count; i++) {
        if (moog) {
            snprintf(gPresetNumberLabels[i], sizeof(gPresetNumberLabels[i]), "%u", (unsigned)(i + 1));
        } else {
            uint8_t  bank = (uint8_t)(i / 128);
            uint32_t prog = (i % 128) + 1;

            snprintf(gPresetNumberLabels[i], sizeof(gPresetNumberLabels[i]), "%c%03u", bank ? 'B' : 'A', (unsigned)prog);
        }
        gPresetNumberItems[i] = (tMenuItem){
            gPresetNumberLabels[i], (tRgb)RGB_GREY_3, action_preset_number_chosen, i, NULL, 0, 0.0
        };
    }

    gPresetNumberItems[count] = (tMenuItem){
        NULL, (tRgb)RGB_BLACK, NULL, 0, NULL, 0, 0.0
    };
    *outColumns               = (count + maxRowsPerColumn - 1) / maxRowsPerColumn;
}

static void open_file_menu(tCoord anchor) {
    static tMenuItem items[8];
    uint32_t         presetColumns;
    int              i = 0;

    build_preset_number_items(&presetColumns);

    items[i++] = (tMenuItem){
        "Open File...", (tRgb)RGB_GREY_3, action_open_file, 0, NULL, 0, 0.0
    };
    items[i++] = (tMenuItem){
        "Load Patch from Bank...", (tRgb)RGB_GREY_3, action_load_patch_from_bank, 0, NULL, 0, 0.0
    };
    items[i++] = (tMenuItem){
        "Save Patch to File...", (tRgb)RGB_GREY_3, action_save_patch, 0, NULL, 0, 0.0
    };
    items[i++] = (tMenuItem){
        "Store Patch to Bank...", (tRgb)RGB_GREY_3, action_store_patch_to_bank, 0, NULL, 0, 0.0
    };
    items[i++] = (tMenuItem){
        "Save Patch by Number to File...", (tRgb)RGB_GREY_3, NULL, 0, gPresetNumberItems, presetColumns, 0.0
    };
    items[i++] = (tMenuItem){
        "Load Patch by Number from File...", (tRgb)RGB_GREY_3, action_restore_patch, 0, NULL, 0, 0.0
    };
    items[i++] = (tMenuItem){
        "Load Patch File to Bank Slot...", (tRgb)RGB_GREY_3, action_restore_patch_to_bank, 0, NULL, 0, 0.0
    };
    items[i++] = (tMenuItem){
        NULL, (tRgb)RGB_BLACK, NULL, 0, NULL, 0, 0.0
    };

    open_context_menu(anchor, items, 0, 0.0);
}

// ── Device menu ───────────────────────────────────────────────────────────────
// "Scan Devices" plus a freshly-scanned device list every time this opens (scan_panel_configs(),
// panelConfig.h) — replaces the old NSMenu's cached gDevicesMenu/rebuild_devices_menu() (misc.mm,
// retired): SynthLib's menu bar already rebuilds each dropdown fresh on every open, so there's no
// separate rebuild-on-folder-change hook needed any more.

static char gDeviceCandidateFilenames[PANEL_MAX_CANDIDATES][64];
static char gDeviceCandidateLabels[PANEL_MAX_CANDIDATES][160];

static void action_scan_devices(int index) {
    (void)index;
    midi_request_reconnect();
    wake_glfw();
}

static void action_switch_device(int index) {
    // index is this item's POSITION within the dropdown (contextMenu.c's handle_context_menu_click()
    // calls action(index) with the array index it hit-tested against) — NOT the candidate's own
    // index into gDeviceCandidateFilenames, since "Scan Devices" occupies position 0, shifting every
    // candidate's position one past its actual gDeviceCandidateFilenames slot. Real bug found
    // 2026-07-17 (owner report: device selector picking "the next one in the list") — must read the
    // candidate index back out of gContextMenu.items[index].param instead, same as every other
    // per-item-data menu action in this codebase (e.g. src/menus.c's own action_set_dial_value()).
    uint32_t candidateIndex = gContextMenu.items[index].param;

    synth_switch_device_config(gDeviceCandidateFilenames[candidateIndex]);
    wake_glfw();
}

static void open_device_menu(tCoord anchor) {
    static tMenuItem      items[PANEL_MAX_CANDIDATES + 2];
    tPanelConfigCandidate candidates[PANEL_MAX_CANDIDATES];
    uint32_t              count   = scan_panel_configs(synth_layouts_dir(), candidates, PANEL_MAX_CANDIDATES);
    const char *          current = synth_current_device_config();
    int                   i       = 0;

    items[i++] = (tMenuItem){
        "Scan Devices", (tRgb)RGB_GREY_3, action_scan_devices, 0, NULL, 0, 0.0
    };

    for (uint32_t c = 0; c < count; c++) {
        // "* " checkmark prefix, same trick open_controls_menu() below uses — tMenuItem has no
        // separate "checked" flag.
        bool isActive = current && (strcmp(candidates[c].filename, current) == 0);

        strncpy(gDeviceCandidateFilenames[c], candidates[c].filename, sizeof(gDeviceCandidateFilenames[c]) - 1);

        if (candidates[c].description[0] != '\0') {
            snprintf(gDeviceCandidateLabels[c], sizeof(gDeviceCandidateLabels[c]), "%s%s \xe2\x80\x94 %s",
                     isActive ? "* " : "", candidates[c].deviceName, candidates[c].description);
        } else {
            snprintf(gDeviceCandidateLabels[c], sizeof(gDeviceCandidateLabels[c]), "%s%s",
                     isActive ? "* " : "", candidates[c].deviceName);
        }
        items[i++] = (tMenuItem){
            gDeviceCandidateLabels[c], (tRgb)RGB_GREY_3, action_switch_device, c, NULL, 0, 0.0
        };
    }

    items[i++] = (tMenuItem){
        NULL, (tRgb)RGB_BLACK, NULL, 0, NULL, 0, 0.0
    };

    open_context_menu(anchor, items, 0, 0.0);
}

// ── Controls menu ─────────────────────────────────────────────────────────────

static void action_dial_mode_rotary(int index) {
    (void)index;
    gDialMode = eDialModeRotary;
    save_dial_mode(gDialMode);
}

static void action_dial_mode_vertical(int index) {
    (void)index;
    gDialMode = eDialModeVertical;
    save_dial_mode(gDialMode);
}

static void action_dial_mode_horizontal(int index) {
    (void)index;
    gDialMode = eDialModeHorizontal;
    save_dial_mode(gDialMode);
}

static void open_controls_menu(tCoord anchor) {
    static tMenuItem items[]      = {
        {"* Rotary",     (tRgb)RGB_GREY_3, action_dial_mode_rotary,     0, NULL, 0, 0.0},
        {"* Vertical",   (tRgb)RGB_GREY_3, action_dial_mode_vertical,   0, NULL, 0, 0.0},
        {"* Horizontal", (tRgb)RGB_GREY_3, action_dial_mode_horizontal, 0, NULL, 0, 0.0},
        {NULL,           (tRgb)RGB_BLACK,  NULL,                        0, NULL, 0, 0.0},
    };
    static char *    checked[3]   = {"* Rotary", "* Vertical", "* Horizontal"};
    static char *    unchecked[3] = {"Rotary", "Vertical", "Horizontal"};
    int              i;

    for (i = 0; i < 3; i++) {
        items[i].label = ((int)gDialMode == i) ? checked[i] : unchecked[i];
    }

    open_context_menu(anchor, items, 0, 0.0);
}

// ── Layouts menu ──────────────────────────────────────────────────────────────

static void action_choose_layouts_folder(int index) {
    (void)index;
    prompt_choose_layouts_folder(); // menuActions.c
}

static void open_layouts_menu(tCoord anchor) {
    static tMenuItem items[] = {
        {"Choose Layouts Folder...", (tRgb)RGB_GREY_3, action_choose_layouts_folder, 0, NULL, 0, 0.0},
        {NULL,                       (tRgb)RGB_BLACK,  NULL,                         0, NULL, 0, 0.0},
    };

    open_context_menu(anchor, items, 0, 0.0);
}

// ── Backup / Restore menus ────────────────────────────────────────────────────
// Bulk (whole-bank) operations only — every single-patch operation lives in File above. "Bank..."
// (a single opaque whole-bank blob, Voyager's own All Presets Dump) has no equivalent on a
// Korg-style device — greyed out rather than removed from the menu when the connected device isn't
// Moog-style, same as the pre-port NSMenu's own validateMenuItem: behaviour (misc.mm, retired).
// "Bank (Individual Files)..." works for both device families, so it's never greyed.

static void on_backup_folder_chosen(const char * path) {
    if (path == NULL) {
        LOG_DEBUG("Choose Backup Folder: cancelled\n");
        return;
    }
    set_last_backup_folder(synth_current_device_config(), path);
    LOG_DEBUG("Choose Backup Folder: set to %s (for %s)\n", path, synth_current_device_config());
}

static void action_choose_backup_folder(int index) {
    (void)index;
    open_file_browser_folder(on_backup_folder_chosen, "Choose Backup Folder");
}

static void action_backup_bank(int index) {
    (void)index;
    synth_backup_bank();
}

static void action_backup_bank_to_folder(int index) {
    (void)index;
    synth_backup_bank_to_folder();
}

static void open_backup_menu(tCoord anchor) {
    static tMenuItem items[4];
    bool             moog = synth_panel_config()->moogStyleDump;
    int              i    = 0;

    items[i++] = (tMenuItem){
        "Choose Backup Folder...", (tRgb)RGB_GREY_3, action_choose_backup_folder, 0, NULL, 0, 0.0
    };
    items[i++] = (tMenuItem){
        "Bank...", moog ? (tRgb)RGB_GREY_3 : (tRgb)RGB_GREY_5, moog ? action_backup_bank : NULL, 0, NULL, 0, 0.0
    };
    items[i++] = (tMenuItem){
        "Bank (Individual Files)...", (tRgb)RGB_GREY_3, action_backup_bank_to_folder, 0, NULL, 0, 0.0
    };
    items[i++] = (tMenuItem){
        NULL, (tRgb)RGB_BLACK, NULL, 0, NULL, 0, 0.0
    };

    open_context_menu(anchor, items, 0, 0.0);
}

static void action_restore_bank(int index) {
    (void)index;
    synth_backup_restore_bank();
}

static void action_restore_folder(int index) {
    (void)index;
    synth_backup_restore_folder();
}

static void open_restore_menu(tCoord anchor) {
    static tMenuItem items[4];
    bool             moog = synth_panel_config()->moogStyleDump;
    int              i    = 0;

    items[i++] = (tMenuItem){
        "Choose Backup Folder...", (tRgb)RGB_GREY_3, action_choose_backup_folder, 0, NULL, 0, 0.0
    };
    items[i++] = (tMenuItem){
        "Bank...", moog ? (tRgb)RGB_GREY_3 : (tRgb)RGB_GREY_5, moog ? action_restore_bank : NULL, 0, NULL, 0, 0.0
    };
    items[i++] = (tMenuItem){
        "Bank (Individual Files)...", (tRgb)RGB_GREY_3, action_restore_folder, 0, NULL, 0, 0.0
    };
    items[i++] = (tMenuItem){
        NULL, (tRgb)RGB_BLACK, NULL, 0, NULL, 0, 0.0
    };

    open_context_menu(anchor, items, 0, 0.0);
}

tMenuBarItem gAppMenuBar[] = {
    {"File",     open_file_menu    },
    {"Device",   open_device_menu  },
    {"Controls", open_controls_menu},
    {"Layouts",  open_layouts_menu },
    {"Backup",   open_backup_menu  },
    {"Restore",  open_restore_menu },
    {NULL,       NULL              },
};

tRectangle app_menu_bar_rect(void) {
    return (tRectangle){
        {
            0.0, 0.0
        }, {
            (get_render_width() / gGlobalGuiScale), MENU_BAR_HEIGHT
        }
    };
}

#ifdef __cplusplus
}
#endif
