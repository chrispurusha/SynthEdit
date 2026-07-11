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
#include <time.h>

#include "defs.h"
#include "synthlibDefs.h"
#include "types.h"
#include "globalVars.h"
#include "panelConfig.h"
#include "synthComms.h"
#include "synthGraphics.h"
#include "fileDialogue.h"
#include "synthBackup.h"

// gBackupExpect is written from the main thread (synth_backup_current_patch()/
// synth_backup_patch_by_number(), both menu actions) and read/cleared from
// the CoreMIDI callback thread (synth_backup_capture_dump(), called out of
// synthComms.c's dump handlers) — see midi_read_cb() in midiComms.c for
// where that thread comes from. _Atomic for that reason, matching gReDraw's
// own treatment elsewhere in this codebase. Stored as plain int, not
// tBackupExpect, since not every compiler accepts an enum as an atomic type.
static _Atomic int gBackupExpect      = eBackupExpectNone;

// Valid only while gBackupExpect == eBackupExpectPreset — which preset number
// (1-based) the pending request was for, purely so the save dialog can
// suggest a filename that says so.
static uint32_t    gBackupPresetNum   = 0;

// Set on the CoreMIDI thread just before opening the save dialog, read once
// on the main thread inside the dialog's completion callback. No lock needed
// for that handoff: open_file_write_dialogue_async() below dispatch_asyncs
// onto the main queue, and GCD guarantees everything written on the enqueuing
// thread before a dispatch_async is visible to the block it runs — so these
// only need to be set before that call, not atomic themselves.
static uint8_t *   gPendingBackupData = NULL;
static uint32_t    gPendingBackupLen  = 0;

// ── Bank-to-folder batch export ──────────────────────────────────────────────
// Sequentially requests every preset (1..kBackupBatchPresetCount) and saves
// each as its own file — see synth_backup_bank_to_folder()'s own comment
// (synthBackup.h) for why this needs a whole state machine rather than the
// single fire-and-forget shape every other Backup action uses: Single Preset
// Dump can only address one preset per request, and the Voyager answers at
// most one outstanding request at a time (won't queue a second reply while
// still busy honouring the first — real hardware finding, see
// midi_arm_state_dump_debounce()'s own comment in midiComms.c), so the next
// request can't go out until either the previous one's reply lands or a
// timeout gives up on it.
//
// Threading: gBackupExpect above is already _Atomic and already shared
// between the CoreMIDI thread (synth_backup_capture_dump(), called from
// synthComms.c's dump handlers) and whichever thread arms a request — that
// same discipline extends naturally to gBackupBatchActive/gBackupExpect
// here. Everything else below (gBackupBatchCurrentPreset,
// gBackupBatchRequestSinceMs, gBackupBatchFolder, the counts) is owned
// EXCLUSIVELY by the main/render thread (set up once in
// backup_batch_folder_chosen(), then only ever touched inside
// synth_backup_flush_bank_to_folder() and its own helpers, called once per
// frame from do_graphics_loop()) — the CoreMIDI thread never reads or
// writes any of it directly. Instead, a reply is handed off the same way
// gPendingBackupData above already is: the CoreMIDI thread copies the bytes
// into gBackupBatchReplyData/Len and publishes them with a single
// gBackupBatchReplyReady=true store; the render thread's flush consumes
// them (and clears the flag) before doing any sequencing. This keeps all
// the actual state-machine mutation (and all the file I/O) on one thread,
// avoiding a two-writer race between "a reply arrived" and "this preset
// timed out" advancing the same state concurrently.
static _Atomic bool gBackupBatchActive         = false;
static _Atomic bool gBackupBatchReplyReady     = false;
static uint8_t *    gBackupBatchReplyData      = NULL;  // valid only while gBackupBatchReplyReady
static uint32_t     gBackupBatchReplyLen       = 0;

static char         gBackupBatchFolder[1024]   = {0};
static uint32_t     gBackupBatchCurrentPreset  = 0;     // 1-based; which preset the outstanding request is for
static double       gBackupBatchRequestSinceMs = 0.0;
static uint32_t     gBackupBatchRepliedCount   = 0;
static uint32_t     gBackupBatchMissingCount   = 0;

#define BACKUP_BATCH_PRESET_COUNT    128    // matches misc.mm's own "Patch by Number" range / synth_request_single_preset_dump()'s own range check (synthComms.c) — a base Voyager's single bank
#define BACKUP_BATCH_TIMEOUT_MS      1500.0 // generous relative to a real reply's actual latency (well under 100ms in practice) — a fallback for a genuinely non-responding location, not the normal-case wait

// Sanitizes gDevice.progName into a filename-safe single line — shared by
// the single "Patch by Number" save dialog's default name (below) and the
// bank-to-folder batch export (synth_backup_flush_bank_to_folder() below).
// gDevice.progName may contain embedded '\n's (nameLineWidth — see the
// tPanelConfig field comment in panelConfig.h) marking where the source
// device's own display wraps to a new line. A filename has no such notion
// of lines, so each '\n' becomes exactly one space — UNLESS one's already
// there (a short first line like "TIME FOR" already has a real trailing
// space from its own padding — see extract_moog_name()'s comment,
// synthComms.c), in which case the '\n' is just dropped rather than
// doubling it up to two. Either way the two lines always end up separated
// by exactly one space in the filename, even for a name like "Floating
// Mod"/"Steel Guitar" whose raw data has nothing at all between them (both
// lines fill their full width) — a filename reads better with a word break
// there even though the sysex itself doesn't have one; the byte-exact raw
// name is preserved in the saved file regardless. out must be at least
// sizeof(gDevice.progName) bytes; writes "" if gDevice.progName is empty.
static void backup_sanitize_name_for_file(char * out, size_t outSize) {
    out[0] = '\0';

    if (gDevice.progName[0] == '\0') {
        return;
    }
    char * o = out;

    for (const char * p = gDevice.progName; (*p != '\0') && (o < out + outSize - 1); p++) {
        if (*p == '\n') {
            if ((o == out) || (*(o - 1) != ' ')) {
                *o++ = ' ';
            }
        } else {
            *o++ = *p;
        }
    }

    *o     = '\0';
}

void synth_backup_current_patch(void) {
    if (!gDevice.connected) {
        LOG_ERROR("Backup: no device connected\n");
        return;
    }
    gBackupExpect = eBackupExpectLive;
    synth_request_state_dump();
    LOG_DEBUG("Backup: requested a fresh state dump to capture\n");
}

void synth_backup_patch_by_number(uint32_t presetNumber) {
    if (!gDevice.connected) {
        LOG_ERROR("Backup: no device connected\n");
        return;
    }
    gBackupPresetNum = presetNumber;
    gBackupExpect    = eBackupExpectPreset;
    synth_request_single_preset_dump(presetNumber); // logs its own error and leaves gBackupExpect armed-but-unfulfilled if out of range/wrong device
}

void synth_backup_bank(void) {
    if (!gDevice.connected) {
        LOG_ERROR("Backup: no device connected\n");
        return;
    }
    gBackupExpect = eBackupExpectBank;
    synth_request_all_presets_dump(); // logs its own error and leaves gBackupExpect armed-but-unfulfilled if not Moog-style
}

static double backup_monotonic_ms(void) {
    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC, &now);
    return ((double)now.tv_sec * 1000.0) + ((double)now.tv_nsec / 1e6);
}

// Appends one line to <folder>/Patches.txt — reopened in append mode per
// call rather than held open across the whole sweep, so a crash or force
// quit mid-export leaves whatever's been captured so far intact and
// readable rather than an unflushed/truncated file.
static void backup_batch_append_index_line(uint32_t presetNumber, const char * name) {
    char   indexPath[1280];

    snprintf(indexPath, sizeof(indexPath), "%s/Patches.txt", gBackupBatchFolder);
    FILE * f = fopen(indexPath, "a");

    if (f != NULL) {
        fprintf(f, "%03u  %s\n", (unsigned)presetNumber, name);
        fclose(f);
    } else {
        LOG_ERROR("Backup: couldn't append to %s\n", indexPath);
    }
}

// Moves on to the next preset, or finishes the sweep once every preset has
// either been captured or timed out. Called only from the main/render
// thread (synth_backup_flush_bank_to_folder() below and its own helpers) —
// see the batch state block's own comment above for why.
static void backup_batch_advance(void) {
    gBackupBatchCurrentPreset++;

    if (gBackupBatchCurrentPreset > BACKUP_BATCH_PRESET_COUNT) {
        gBackupBatchActive = false;
        LOG_DEBUG("Backup: bank-to-folder export finished — %u captured, %u missing, folder %s\n",
                  (unsigned)gBackupBatchRepliedCount, (unsigned)gBackupBatchMissingCount, gBackupBatchFolder);
        // handle_moog_single_preset_dump() (synthComms.c) only ever touches
        // gDevice.progName (extract_moog_name() at presetNameOffset) — the
        // sweep above leaves it showing the LAST preset's stored name
        // instead of the live edit buffer's. Re-requesting the live state
        // restores it (and re-syncs every dial, belt and braces), same
        // "ask for current state" idea as synth_navigate_preset()'s own
        // post-navigation refresh.
        synth_request_state_dump();
        return;
    }
    gBackupExpect              = eBackupExpectPreset;
    synth_request_single_preset_dump(gBackupBatchCurrentPreset);
    gBackupBatchRequestSinceMs = backup_monotonic_ms();
}

// Writes the just-captured reply to its own file and appends the index
// line, then advances. Called only from the main/render thread.
static void backup_batch_write_capture(const uint8_t * data, uint32_t length) {
    char   nameForFile[sizeof(gDevice.progName)];

    backup_sanitize_name_for_file(nameForFile, sizeof(nameForFile));

    char   filePath[1280];

    if (nameForFile[0] != '\0') {
        snprintf(filePath, sizeof(filePath), "%s/%03u %s.syx", gBackupBatchFolder, (unsigned)gBackupBatchCurrentPreset, nameForFile);
    } else {
        snprintf(filePath, sizeof(filePath), "%s/%03u.syx", gBackupBatchFolder, (unsigned)gBackupBatchCurrentPreset);
    }
    FILE * f = fopen(filePath, "wb");

    if (f != NULL) {
        fwrite(data, 1, length, f);
        fclose(f);
        gBackupBatchRepliedCount++;
        backup_batch_append_index_line(gBackupBatchCurrentPreset, (nameForFile[0] != '\0') ? nameForFile : "(unnamed)");
    } else {
        LOG_ERROR("Backup: couldn't open %s for writing\n", filePath);
        gBackupBatchMissingCount++;
        backup_batch_append_index_line(gBackupBatchCurrentPreset, "(write failed)");
    }
}

// Runs on the main thread once the user has chosen (or cancelled) a backup
// folder — see synth_backup_bank_to_folder() below for where the picker is
// opened.
static void backup_batch_folder_chosen(const char * path) {
    if (path == NULL) {
        LOG_DEBUG("Backup: bank-to-folder export cancelled\n");
        return;
    }
    strncpy(gBackupBatchFolder, path, sizeof(gBackupBatchFolder) - 1);
    gBackupBatchFolder[sizeof(gBackupBatchFolder) - 1] = '\0';

    // Fresh index file each run — truncates any previous one from an
    // earlier export into the same folder rather than appending onto
    // stale content. Header identifies which device/when this export is
    // from (a bare list of numbers+names on its own gave no way to tell
    // two exports' Patches.txt files apart, or a genuine device patch list
    // apart from one of these) — backup_batch_append_index_line() (below)
    // only ever appends after this, never touches the header itself.
    char   indexPath[1280];
    snprintf(indexPath, sizeof(indexPath), "%s/Patches.txt", gBackupBatchFolder);
    FILE * f = fopen(indexPath, "w");

    if (f != NULL) {
        const char * deviceName = synth_panel_config()->deviceName;
        time_t       now        = time(NULL);
        char         timestamp[32];

        strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M", localtime(&now));
        fprintf(f, "%s Bank Backup — %s\n", (deviceName[0] != '\0') ? deviceName : "Patch", timestamp);
        fprintf(f, "%u presets requested from device\n\n", (unsigned)BACKUP_BATCH_PRESET_COUNT);
        fclose(f);
    }
    gBackupBatchActive         = true;
    gBackupBatchCurrentPreset  = 1;
    gBackupBatchRepliedCount   = 0;
    gBackupBatchMissingCount   = 0;
    gBackupExpect              = eBackupExpectPreset;
    synth_request_single_preset_dump(gBackupBatchCurrentPreset);
    gBackupBatchRequestSinceMs = backup_monotonic_ms();
    LOG_DEBUG("Backup: starting bank-to-folder export of %u presets to %s\n",
              (unsigned)BACKUP_BATCH_PRESET_COUNT, gBackupBatchFolder);
}

void synth_backup_bank_to_folder(void) {
    if (!gDevice.connected) {
        LOG_ERROR("Backup: no device connected\n");
        return;
    }

    if (!synth_panel_config()->moogStyleDump) {
        LOG_ERROR("Backup: bank-to-folder export only supports Moog-style devices so far\n");
        return;
    }

    if (gBackupBatchActive) {
        LOG_ERROR("Backup: a bank-to-folder export is already in progress\n");
        return;
    }

    if (gBackupExpect != eBackupExpectNone) {
        LOG_ERROR("Backup: another backup operation is already in progress\n");
        return;
    }
    open_folder_choose_dialogue_async(backup_batch_folder_chosen, "Choose Backup Folder");
}

void synth_backup_flush_bank_to_folder(void) {
    if (!gBackupBatchActive) {
        return;
    }

    if (gBackupBatchReplyReady) {
        gBackupBatchReplyReady = false; // clear before using — see the batch state block's own comment on why this is safe without a lock
        backup_batch_write_capture(gBackupBatchReplyData, gBackupBatchReplyLen);
        free(gBackupBatchReplyData);
        gBackupBatchReplyData  = NULL;
        gBackupBatchReplyLen   = 0;
        backup_batch_advance();
        return;
    }

    if ((backup_monotonic_ms() - gBackupBatchRequestSinceMs) >= BACKUP_BATCH_TIMEOUT_MS) {
        // This preset location didn't answer in time (unresponsive, or an
        // unpopulated location — Voyager's own behaviour for one of those
        // is unconfirmed, see synth_backup_bank_to_folder()'s own comment,
        // synthBackup.h). Log it as missing and move on rather than hanging
        // the whole export on one slot. Clearing gBackupExpect here (rather
        // than leaving it armed for THIS preset) means a very-late reply
        // arriving after this point gets attributed to whichever preset
        // backup_batch_advance() re-arms next instead — a narrow, accepted
        // edge case given the Voyager only ever has one request outstanding
        // at a time in practice (see the batch state block's own comment).
        LOG_ERROR("Backup: preset %u did not reply within %ums, skipping\n",
                  (unsigned)gBackupBatchCurrentPreset, (unsigned)BACKUP_BATCH_TIMEOUT_MS);
        gBackupBatchMissingCount++;
        backup_batch_append_index_line(gBackupBatchCurrentPreset, "(no response)");
        gBackupExpect = eBackupExpectNone;
        backup_batch_advance();
    }
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

void synth_backup_capture_dump(const uint8_t * data, uint32_t length, tBackupExpect kind) {
    if (gBackupExpect != kind) {
        return;
    }
    gBackupExpect = eBackupExpectNone;

    if ((kind == eBackupExpectPreset) && gBackupBatchActive) {
        // Bank-to-folder sweep in progress — hand the bytes off to the
        // main/render thread rather than doing any file I/O or sequencing
        // here (this runs on the CoreMIDI thread — see the batch state
        // block's own comment for why). gBackupBatchReplyReady published
        // LAST, after the plain writes above it, is what makes this a safe
        // handoff without a lock.
        uint8_t * batchCopy = (uint8_t *)malloc(length);

        if (batchCopy == NULL) {
            LOG_ERROR("Backup: out of memory copying %u byte dump (bank-to-folder)\n", (unsigned)length);
            return;
        }
        memcpy(batchCopy, data, length);
        gBackupBatchReplyData  = batchCopy;
        gBackupBatchReplyLen   = length;
        gBackupBatchReplyReady = true;
        return;
    }
    uint8_t *    copy       = (uint8_t *)malloc(length);

    if (copy == NULL) {
        LOG_ERROR("Backup: out of memory copying %u byte dump\n", (unsigned)length);
        return;
    }
    memcpy(copy, data, length);
    gPendingBackupData = copy;
    gPendingBackupLen  = length;

    const char * deviceName = synth_panel_config()->deviceName;
    char         defaultName[96];

    if (kind == eBackupExpectPreset) {
        // extract_moog_name() (synthComms.c) just decoded this, if the
        // device's file declares a presetNameOffset — fall back to "<device>
        // Preset <n>" if it doesn't (or decoded empty).
        char nameForFile[sizeof(gDevice.progName)];

        backup_sanitize_name_for_file(nameForFile, sizeof(nameForFile));

        if (nameForFile[0] != '\0') {
            snprintf(defaultName, sizeof(defaultName), "%s.syx", nameForFile);
        } else {
            snprintf(defaultName, sizeof(defaultName), "%s Preset %u.syx",
                     (deviceName[0] != '\0') ? deviceName : "patch", (unsigned)gBackupPresetNum);
        }
    } else if (kind == eBackupExpectBank) {
        snprintf(defaultName, sizeof(defaultName), "%s Bank.syx", (deviceName[0] != '\0') ? deviceName : "patch");
    } else {
        snprintf(defaultName, sizeof(defaultName), "%s.syx", (deviceName[0] != '\0') ? deviceName : "patch");
    }
    LOG_DEBUG("Backup: captured %u byte dump, opening save dialog\n", (unsigned)length);
    open_file_write_dialogue_async(backup_save_callback, defaultName);
}
