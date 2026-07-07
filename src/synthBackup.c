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

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "defs.h"
#include "synthlibDefs.h"
#include "types.h"
#include "globalVars.h"
#include "panelConfig.h"
#include "synthComms.h"
#include "synthGraphics.h"
#include "fileDialogue.h"
#include "synthBackup.h"

// gBackupArmed is written from the main thread (synth_backup_current_patch(),
// a menu action) and read/cleared from the CoreMIDI callback thread
// (synth_backup_capture_dump(), called out of handle_moog_panel_dump()/
// handle_curr_prog_dump() in synthComms.c) — see midi_read_cb() in
// midiComms.c for where that thread comes from. _Atomic for that reason,
// matching gReDraw's own treatment elsewhere in this codebase.
static _Atomic bool gBackupArmed       = false;

// Set on the CoreMIDI thread just before opening the save dialog, read once
// on the main thread inside the dialog's completion callback. No lock needed
// for that handoff: open_file_write_dialogue_async() below dispatch_asyncs
// onto the main queue, and GCD guarantees everything written on the enqueuing
// thread before a dispatch_async is visible to the block it runs — so these
// only need to be set before that call, not atomic themselves.
static uint8_t *    gPendingBackupData = NULL;
static uint32_t     gPendingBackupLen  = 0;

void synth_backup_current_patch(void) {
    if (!gDevice.connected) {
        LOG_ERROR("Backup: no device connected\n");
        return;
    }
    gBackupArmed = true;
    synth_request_state_dump();
    LOG_DEBUG("Backup: requested a fresh state dump to capture\n");
}

// Runs on the main thread once the user has chosen (or cancelled) a save
// location — see synth_backup_capture_dump() below for where gPendingBackup*
// gets set just before this dialog is opened.
static void backup_save_callback(const char * path) {
    if (path != NULL) {
        FILE * f = fopen(path, "wb");

        if (f != NULL) {
            fwrite(gPendingBackupData, 1, gPendingBackupLen, f);
            fclose(f);
            LOG_DEBUG("Backup: wrote %u bytes to %s\n", (unsigned)gPendingBackupLen, path);
        } else {
            LOG_ERROR("Backup: couldn't open %s for writing\n", path);
        }
    } else {
        LOG_DEBUG("Backup: save dialog cancelled\n");
    }
    free(gPendingBackupData);
    gPendingBackupData = NULL;
    gPendingBackupLen  = 0;
}

void synth_backup_capture_dump(const uint8_t * data, uint32_t length) {
    if (!gBackupArmed) {
        return;
    }
    gBackupArmed       = false;

    uint8_t *    copy       = (uint8_t *)malloc(length);

    if (copy == NULL) {
        LOG_ERROR("Backup: out of memory copying %u byte dump\n", (unsigned)length);
        return;
    }
    memcpy(copy, data, length);
    gPendingBackupData = copy;
    gPendingBackupLen  = length;

    const char * deviceName = synth_panel_config()->deviceName;
    char         defaultName[80];

    snprintf(defaultName, sizeof(defaultName), "%s.syx", (deviceName[0] != '\0') ? deviceName : "patch");
    LOG_DEBUG("Backup: captured %u byte dump, opening save dialog\n", (unsigned)length);
    open_file_write_dialogue_async(backup_save_callback, defaultName);
}
