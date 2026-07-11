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

#include <ctype.h>
#include <dirent.h>
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
#include "midiComms.h"
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
// name is preserved in the saved file regardless.
//
// '/' becomes '-' — REAL bug found+fixed 2026-07-11: a patch literally
// named "Tiny w/o Mod" made backup_batch_write_capture()'s own fopen() call
// silently fail (macOS treats '/' as a path separator, so the constructed
// path implied a subdirectory that doesn't exist), leaving preset 23's
// slot with no exported file at all and just a "(write failed)" line in
// Patches.txt — invisible until a later Restore > Bank (Individual Files)
// tried to restore that folder and correctly skipped the missing file,
// which is what actually surfaced the gap. No other character a decoded
// name can contain is unsafe at the POSIX/fopen() level (extract_moog_name()
// already collapses anything below 0x20 to a space during decode, so only
// printable ASCII 0x20-0x7E ever reaches here — '/' is the one member of
// that range the filesystem itself rejects).
//
// out must be at least sizeof(gDevice.progName) bytes; writes "" if
// gDevice.progName is empty.
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
        } else if (*p == '/') {
            *o++ = '-';
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
    // Without this, the progress overlay (synth_render_backup_progress(),
    // synthGraphics.cpp) would only repaint whenever something UNRELATED
    // happened to set gReDraw (a mouse move, etc.) — do_graphics_loop()
    // only calls render_frame() at all when gReDraw is true, so a sweep
    // this function drives entirely on its own otherwise looks frozen on
    // screen even while genuinely progressing. Found 2026-07-11 while
    // adding the overlay itself, not from a bug report.
    gReDraw                    = true;
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
    set_last_backup_folder(gBackupBatchFolder); // so a later single-file Backup save defaults here too — see its own comment (fileDialogue.h)

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
    open_folder_choose_dialogue_async(backup_batch_folder_chosen, "Choose Backup Folder", get_last_backup_folder());
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

            // Remember the containing folder so the NEXT Backup save (of
            // any kind — this one, Bank, or the Bank-to-Folder picker)
            // defaults here too, instead of each one starting from an
            // unrelated system default — see get_last_backup_folder()'s
            // own comment (fileDialogue.h).
            const char * lastSlash = strrchr(path, '/');

            if (lastSlash != NULL) {
                char   folder[1024];
                size_t len = (size_t)(lastSlash - path);

                if (len >= sizeof(folder)) {
                    len = sizeof(folder) - 1;
                }
                memcpy(folder, path, len);
                folder[len] = '\0';
                set_last_backup_folder(folder);
            }
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

    // Bank has no single current-patch name to reflect (it's 128 of them at
    // once) — handled on its own, before touching gDevice.progName at all.
    // Preset and Live ("Save Panel to File…", added 2026-07-11 at the
    // owner's request — "should default to a name reflecting the patch
    // name") both just want gDevice.progName if extract_moog_name()
    // (synthComms.c) managed to decode one, falling back to a kind-specific
    // default name when it didn't. Live needed handle_moog_panel_dump()
    // (synthComms.c) reordered to decode the name BEFORE calling this
    // function — it used to call this first, so gDevice.progName was still
    // whatever the PREVIOUS dump had left it as, not this one's.
    if (kind == eBackupExpectBank) {
        snprintf(defaultName, sizeof(defaultName), "%s Bank.syx", (deviceName[0] != '\0') ? deviceName : "patch");
    } else {
        char nameForFile[sizeof(gDevice.progName)];

        backup_sanitize_name_for_file(nameForFile, sizeof(nameForFile));

        if (nameForFile[0] != '\0') {
            snprintf(defaultName, sizeof(defaultName), "%s.syx", nameForFile);
        } else if (kind == eBackupExpectPreset) {
            snprintf(defaultName, sizeof(defaultName), "%s Preset %u.syx",
                     (deviceName[0] != '\0') ? deviceName : "patch", (unsigned)gBackupPresetNum);
        } else {
            snprintf(defaultName, sizeof(defaultName), "%s.syx", (deviceName[0] != '\0') ? deviceName : "patch");
        }
    }
    LOG_DEBUG("Backup: captured %u byte dump, opening save dialog\n", (unsigned)length);
    open_file_write_dialogue_async(backup_save_callback, defaultName, get_last_backup_folder());
}

// ── Restore ───────────────────────────────────────────────────────────────────
// See synthBackup.h's own comment on this whole section for the mechanism
// and its 2026-07-11 hardware confirmation.

// Reads an entire file into a malloc'd buffer. Returns NULL (and logs) on
// any failure; caller owns the returned buffer. *outLen receives its size.
static uint8_t * restore_read_file(const char * path, uint32_t * outLen) {
    FILE *    f       = fopen(path, "rb");

    if (f == NULL) {
        LOG_ERROR("Restore: couldn't open %s\n", path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long      size    = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0) {
        LOG_ERROR("Restore: %s is empty or unreadable\n", path);
        fclose(f);
        return NULL;
    }
    uint8_t * data    = (uint8_t *)malloc((size_t)size);

    if (data == NULL) {
        LOG_ERROR("Restore: out of memory reading %s (%ld bytes)\n", path, size);
        fclose(f);
        return NULL;
    }
    size_t    readLen = fread(data, 1, (size_t)size, f);

    fclose(f);

    if (readLen != (size_t)size) {
        LOG_ERROR("Restore: short read on %s\n", path);
        free(data);
        return NULL;
    }
    *outLen = (uint32_t)size;
    return data;
}

// Validates data is a raw F0...F7 SysEx matching the connected device's own
// mfrId/productId (same check is_moog_sysex() does in synthComms.c, re-done
// here independently rather than exposing that static function — Restore
// is the only place outside synthComms.c that needs to validate a dump's
// header before trusting it) and carries the given mode byte. Returns
// false (and logs why) otherwise, so callers can bail out before sending
// anything to the device.
// reason/reasonSize receive a user-facing explanation on failure — shown
// via show_info_dialogue() by each caller below, since LOG_ERROR alone
// (stderr) is invisible to anyone not watching a console, which made an
// earlier version of this validation fail completely silently from the
// user's point of view (a wrong file picked just did nothing, with no way
// to tell why — reported 2026-07-11, fixed by adding this).
static bool restore_validate_moog_dump(const uint8_t * data, uint32_t length, uint8_t expectedMode, const char * what,
                                       char * reason, size_t reasonSize) {
    tPanelConfig * cfg = synth_panel_config();

    if (!cfg->moogStyleDump) {
        snprintf(reason, reasonSize, "The connected device isn't Moog-style — this Restore action doesn't support it yet.");
        LOG_ERROR("Restore: connected device isn't Moog-style\n");
        return false;
    }

    if ((length < 6) || (data[0] != MIDI_SYSEX_START) || (data[length - 1] != MIDI_SYSEX_END)) {
        snprintf(reason, reasonSize, "This file doesn't look like a raw SysEx capture (%u bytes) — was it saved by this app's own Backup, unmodified?", (unsigned)length);
        LOG_ERROR("Restore: %s doesn't look like a raw SysEx capture (%u bytes)\n", what, (unsigned)length);
        return false;
    }

    if ((data[1] != cfg->manufacturerId[0]) || (data[2] != cfg->productId)) {
        snprintf(reason, reasonSize, "This file isn't a dump for the connected device (manufacturer/product ID mismatch).");
        LOG_ERROR("Restore: %s isn't a dump for the connected device (mfrId/productId mismatch)\n", what);
        return false;
    }

    if (data[4] != expectedMode) {
        snprintf(reason, reasonSize,
                 "This file is a %s, not a %s — pick a file saved with the matching Backup action.",
                 (data[4] == 0x01) ? "whole Bank dump" : (data[4] == 0x02) ? "Panel (Edit Buffer) dump" : (data[4] == 0x03) ? "Patch by Number dump" : "dump of an unrecognized type",
                 what);
        LOG_ERROR("Restore: %s is mode 0x%02X, expected 0x%02X\n", what, (unsigned)data[4], (unsigned)expectedMode);
        return false;
    }
    return true;
}

// Converts a captured Single Preset Dump (mode 0x03 — "Patch by Number" or
// a Bank (Individual Files) export) into an equivalent Panel Dump (mode
// 0x02), so any backed-up patch can be loaded into the live edit buffer
// via "Open Panel File…" too, not just restored-by-overwrite via
// "Restore > Patch by Number…" — added 2026-07-11, owner's own request
// ("we should be able to use backup patches to load to panel").
//
// The two formats are otherwise byte-for-byte identical: voyager.txt's own
// presetNameOffset (101) vs. panelNameOffset (100) — derived independently
// on real hardware, see each field's own comment there — differ by exactly
// one byte, and the header comment on presetNameOffset already documents
// why: "Moog's own doc lists an extra byte in the 0x03 reply's header that
// 0x02's doesn't have" — the preset number, at index 5 (F0/mfrId/
// productId/deviceId/mode/THEN this), shifting everything after it by one.
// Stripping that byte and changing the mode byte back to 0x02 reconstructs
// a valid Panel Dump. Structurally sound from that derivation; NOT yet
// independently round-tripped against real hardware the way the Restore
// mechanism itself was (see [[project_voyager_restore_mechanism]] in the
// assistant's own memory notes) — worth a real test before fully trusting
// it. Returns a newly malloc'd buffer (caller frees) and writes its length
// to *outLen, or NULL if srcLength is too short to contain the byte being
// removed.
static uint8_t * convert_preset_dump_to_panel_dump(const uint8_t * src, uint32_t srcLength, uint32_t * outLen) {
    if (srcLength < 7) { // F0 mfrId productId deviceId mode presetNum ...data... F7, minimum shape
        return NULL;
    }
    uint32_t  dstLength = srcLength - 1;
    uint8_t * dst       = (uint8_t *)malloc(dstLength);

    if (dst == NULL) {
        return NULL;
    }
    memcpy(dst, src, 4);                          // F0 mfrId productId deviceId
    dst[4]  = 0x02;                               // mode: Panel Dump (was 0x03, Single Preset Dump)
    memcpy(&dst[5], &src[6], srcLength - 6);      // everything after the removed preset-number byte, including the trailing F7
    *outLen = dstLength;
    return dst;
}

// Runs on the main thread once the user has chosen (or cancelled) a file to
// load into the live edit buffer — see synth_backup_restore_panel() below.
// Accepts EITHER a genuine Panel Dump (mode 0x02, "Backup > Current Panel"
// / "Save Panel to File…") or a Single Preset Dump (mode 0x03, "Backup >
// Patch by Number" or a Bank (Individual Files) export) — the latter is
// converted via convert_preset_dump_to_panel_dump() above before sending.
static void restore_panel_file_chosen(const char * path) {
    if (path == NULL) {
        LOG_DEBUG("Restore: panel restore cancelled\n");
        return;
    }
    uint32_t  length = 0;
    uint8_t * data   = restore_read_file(path, &length);

    if (data == NULL) {
        return;
    }
    char      reason[192];

    // A raw Single Preset Dump (mode 0x03) is converted to a Panel Dump
    // (mode 0x02) shape BEFORE the mode-0x02 validation below, so both file
    // kinds end up validated (and sent) the exact same way — the mfrId/
    // productId check still guards against a file from the wrong device
    // either way.
    if ((length >= 5) && (data[0] == MIDI_SYSEX_START) && (data[4] == 0x03)) {
        uint32_t  convertedLen = 0;
        uint8_t * converted    = convert_preset_dump_to_panel_dump(data, length, &convertedLen);

        if (converted != NULL) {
            free(data);
            data   = converted;
            length = convertedLen;
            LOG_DEBUG("Restore: converted a Single Preset Dump (%s) to a Panel Dump for loading\n", path);
        }
    }

    if (!restore_validate_moog_dump(data, length, 0x02, "Panel Dump", reason, sizeof(reason))) {
        show_info_dialogue("Restore Panel Failed", reason);
        free(data);
        return;
    }

    if (midi_send(data, length)) {
        LOG_DEBUG("Restore: sent %u byte Panel Dump from %s (loads live edit buffer only)\n", (unsigned)length, path);
        show_info_dialogue("Restore Panel", "Sent — the connected device's live edit buffer should now match this file.");
    } else {
        LOG_ERROR("Restore: failed to send %u byte Panel Dump from %s\n", (unsigned)length, path);
        show_info_dialogue("Restore Panel Failed", "The message couldn't be sent — see the debug log for the exact MIDI error.");
    }
    free(data);
}

void synth_backup_restore_panel(void) {
    if (!gDevice.connected) {
        LOG_ERROR("Restore: no device connected\n");
        return;
    }
    open_file_read_dialogue_async(restore_panel_file_chosen);
}

// Runs on the main thread once the user has chosen (or cancelled) a Single
// Preset Dump file to restore — see synth_backup_restore_patch() below.
static void restore_patch_file_chosen(const char * path) {
    if (path == NULL) {
        LOG_DEBUG("Restore: patch restore cancelled\n");
        return;
    }
    uint32_t  length = 0;
    uint8_t * data   = restore_read_file(path, &length);

    if (data == NULL) {
        return;
    }
    char      reason[192];

    if (!restore_validate_moog_dump(data, length, 0x03, "Patch by Number dump", reason, sizeof(reason))) {
        show_info_dialogue("Restore Patch Failed", reason);
        free(data);
        return;
    }
    // The preset number is the one extra byte a Single Preset Dump's own
    // header has that a Panel Dump's doesn't (see presetNameOffset's own
    // comment in voyager.txt for that byte-count difference) — byte index
    // 5 (F0, mfrId, productId, deviceId, mode, THEN this), 0-based on the
    // wire same as the request side (synth_request_single_preset_dump(),
    // synthComms.c). CONFIRMED against real hardware 2026-07-11 (see
    // [[project_voyager_restore_mechanism]] in the assistant's own memory
    // notes): captured preset 128, decoded this byte as 127, matched
    // exactly.
    uint32_t  presetNumber = (uint32_t)data[5] + 1;
    char      message[160];

    snprintf(message, sizeof(message),
             "This will overwrite Preset %u on the connected device with the contents of this file. This cannot be undone.",
             (unsigned)presetNumber);

    if (show_confirm_dialogue("Restore Patch", message)) {
        if (midi_send(data, length)) {
            LOG_DEBUG("Restore: sent %u byte Single Preset Dump from %s (overwrote preset %u)\n",
                      (unsigned)length, path, (unsigned)presetNumber);
            snprintf(message, sizeof(message), "Sent — Preset %u should now match this file.", (unsigned)presetNumber);
            show_info_dialogue("Restore Patch", message);
        } else {
            LOG_ERROR("Restore: failed to send %u byte Single Preset Dump from %s\n", (unsigned)length, path);
            show_info_dialogue("Restore Patch Failed", "The message couldn't be sent — see the debug log for the exact MIDI error.");
        }
    } else {
        LOG_DEBUG("Restore: patch restore cancelled at confirmation\n");
    }
    free(data);
}

void synth_backup_restore_patch(void) {
    if (!gDevice.connected) {
        LOG_ERROR("Restore: no device connected\n");
        return;
    }
    open_file_read_dialogue_async(restore_patch_file_chosen);
}

// Runs on the main thread once the user has chosen (or cancelled) a whole-
// bank dump file to restore — see synth_backup_restore_bank() below.
static void restore_bank_file_chosen(const char * path) {
    if (path == NULL) {
        LOG_DEBUG("Restore: bank restore cancelled\n");
        return;
    }
    uint32_t  length = 0;
    uint8_t * data   = restore_read_file(path, &length);

    if (data == NULL) {
        return;
    }
    char      reason[192];

    if (!restore_validate_moog_dump(data, length, 0x01, "Bank dump", reason, sizeof(reason))) {
        show_info_dialogue("Restore Bank Failed", reason);
        free(data);
        return;
    }

    if (show_confirm_dialogue("Restore Bank",
                              "This will overwrite ALL 128 presets in the current bank on the connected device with the contents of this file. This cannot be undone.")) {
        if (midi_send(data, length)) {
            LOG_DEBUG("Restore: sent %u byte Bank dump from %s (overwrote entire current bank)\n", (unsigned)length, path);
            show_info_dialogue("Restore Bank", "Sent — the current bank should now match this file.");
        } else {
            LOG_ERROR("Restore: failed to send %u byte Bank dump from %s\n", (unsigned)length, path);
            show_info_dialogue("Restore Bank Failed", "The message couldn't be sent — see the debug log for the exact MIDI error.");
        }
    } else {
        LOG_DEBUG("Restore: bank restore cancelled at confirmation\n");
    }
    free(data);
}

void synth_backup_restore_bank(void) {
    if (!gDevice.connected) {
        LOG_ERROR("Restore: no device connected\n");
        return;
    }
    open_file_read_dialogue_async(restore_bank_file_chosen);
}

// ── Restore from folder (Bank Individual Files, in reverse) ─────────────────
// Sequentially reads back a Backup > Bank (Individual Files) export and
// sends each file it finds, restoring every matching stored slot on the
// connected device — added 2026-07-11, owner's own request ("we should be
// able to restore... the individual files based on the .txt file list").
//
// Threading: unlike gBackupBatch* above, this never touches the CoreMIDI
// thread at all — a restore SEND has no reply to wait for (see
// synth_backup_restore_patch()'s own comment on the Single Preset Dump
// mechanism), so the whole sweep lives entirely on the main/render thread:
// synth_backup_flush_restore_folder(), called once per frame from
// do_graphics_loop() same as synth_backup_flush_bank_to_folder(), reads the
// next file off disk and sends it directly, no gBackupBatchReplyReady-style
// cross-thread handoff needed.
static bool     gRestoreFolderActive       = false;
static char     gRestoreFolderFolder[1024] = {0};
static uint32_t gRestoreFolderEntries[BACKUP_BATCH_PRESET_COUNT]; // preset numbers, in Patches.txt order, filtered to ones a matching file was actually found for
static uint32_t gRestoreFolderCount        = 0;                   // how many entries above are valid
static uint32_t gRestoreFolderIndex        = 0;                   // which entry synth_backup_flush_restore_folder() sends next
static double   gRestoreFolderNextSendMs   = 0.0;
static uint32_t gRestoreFolderSentCount    = 0;
static uint32_t gRestoreFolderMissingCount = 0;

// Paced, not fired all at once — the device needs real time to actually
// write each preset to its own storage before the next one arrives (same
// "won't queue a second reply/request while still busy" real-hardware
// finding that drives BACKUP_BATCH_TIMEOUT_MS above, applied here to the
// SEND side instead of the request side). No hardware-measured minimum
// yet; generous relative to what a single small SysEx send itself takes.
#define RESTORE_FOLDER_SEND_PACING_MS    150.0

// Parses <folder>/Patches.txt for the ordered list of preset numbers it
// records (backup_batch_append_index_line() above writes each data line as
// "%03u  %s\n" — exactly 3 digits then TWO spaces) — ignores the recorded
// name text, just the leading number. The two-space check is what
// distinguishes a real entry from the header's own "128 presets requested
// from device" line, which also starts with digits but has only one space
// after them. Returns how many were found (capped at maxCount).
static uint32_t restore_folder_parse_index(const char * folder, uint32_t * outNumbers, uint32_t maxCount) {
    char     indexPath[1280];

    snprintf(indexPath, sizeof(indexPath), "%s/Patches.txt", folder);
    FILE *   f     = fopen(indexPath, "r");

    if (f == NULL) {
        return 0;
    }
    uint32_t count = 0;
    char     line[512];

    while ((count < maxCount) && (fgets(line, sizeof(line), f) != NULL)) {
        if (  (strlen(line) >= 5) && isdigit((unsigned char)line[0]) && isdigit((unsigned char)line[1])
           && isdigit((unsigned char)line[2]) && (line[3] == ' ') && (line[4] == ' ')) {
            uint32_t num = (uint32_t)((line[0] - '0') * 100 + (line[1] - '0') * 10 + (line[2] - '0'));

            if ((num >= 1) && (num <= BACKUP_BATCH_PRESET_COUNT)) {
                outNumbers[count++] = num;
            }
        }
    }
    fclose(f);
    return count;
}

// Finds the file in `folder` whose name starts with the exact zero-padded
// 3-digit preset number (matching backup_batch_write_capture()'s own
// "%03u %s.syx"/"%03u.syx" naming) — deliberately NOT reconstructed from
// Patches.txt's own recorded name text, so a file renamed (or one whose
// name has drifted from what the index remembers) since the export still
// gets found correctly; only the leading number has to match. Writes the
// full path into outPath (sized to match this file's other path buffers).
// Returns false if no matching file was found.
static bool restore_folder_find_file(const char * folder, uint32_t presetNumber, char * outPath, size_t outPathSize) {
    DIR *           dp    = opendir(folder);

    if (dp == NULL) {
        return false;
    }
    char            prefix[4];

    snprintf(prefix, sizeof(prefix), "%03u", presetNumber);
    bool            found = false;
    struct dirent * entry;

    while ((entry = readdir(dp)) != NULL) {
        if (  (strlen(entry->d_name) >= 4) && (strncmp(entry->d_name, prefix, 3) == 0)
           && ((entry->d_name[3] == ' ') || (entry->d_name[3] == '.'))) {
            snprintf(outPath, outPathSize, "%s/%s", folder, entry->d_name);
            found = true;
            break;
        }
    }
    closedir(dp);
    return found;
}

// Runs on the main thread once the user has chosen (or cancelled) a folder
// to restore from — see synth_backup_restore_folder() below.
static void restore_folder_chosen(const char * path) {
    if (path == NULL) {
        LOG_DEBUG("Restore: folder restore cancelled\n");
        return;
    }
    uint32_t numbers[BACKUP_BATCH_PRESET_COUNT];
    uint32_t indexCount = restore_folder_parse_index(path, numbers, BACKUP_BATCH_PRESET_COUNT);

    if (indexCount == 0) {
        show_info_dialogue("Restore Folder Failed",
                           "No Patches.txt index found in this folder (or it has no entries) — pick a folder created by Backup > Bank (Individual Files).");
        return;
    }
    // Resolve each listed preset number to an actual file in the folder —
    // one whose file has since been deleted/moved/renamed-past-recognition
    // is just skipped (counted as missing below), not treated as a hard
    // failure for the whole operation.
    gRestoreFolderCount                                    = 0;

    for (uint32_t i = 0; i < indexCount; i++) {
        char filePath[1280];

        if (restore_folder_find_file(path, numbers[i], filePath, sizeof(filePath))) {
            gRestoreFolderEntries[gRestoreFolderCount++] = numbers[i];
        }
    }

    if (gRestoreFolderCount == 0) {
        show_info_dialogue("Restore Folder Failed", "Patches.txt lists presets, but none of their files could be found in this folder.");
        return;
    }
    strncpy(gRestoreFolderFolder, path, sizeof(gRestoreFolderFolder) - 1);
    gRestoreFolderFolder[sizeof(gRestoreFolderFolder) - 1] = '\0';

    char message[256];

    snprintf(message, sizeof(message),
             "This will restore %u preset(s) found in this folder, overwriting their exact matching slots on the connected device. This cannot be undone.",
             (unsigned)gRestoreFolderCount);

    if (!show_confirm_dialogue("Restore Folder", message)) {
        LOG_DEBUG("Restore: folder restore cancelled at confirmation\n");
        return;
    }
    gRestoreFolderIndex                                    = 0;
    gRestoreFolderSentCount                                = 0;
    gRestoreFolderMissingCount                             = indexCount - gRestoreFolderCount; // entries listed but never found on disk
    gRestoreFolderActive                                   = true;
    gRestoreFolderNextSendMs                               = backup_monotonic_ms();            // send the first one on the very next flush tick
    LOG_DEBUG("Restore: starting folder restore of %u preset(s) from %s\n", (unsigned)gRestoreFolderCount, path);
}

void synth_backup_restore_folder(void) {
    if (!gDevice.connected) {
        LOG_ERROR("Restore: no device connected\n");
        return;
    }

    if (!synth_panel_config()->moogStyleDump) {
        LOG_ERROR("Restore: folder restore only supports Moog-style devices so far\n");
        return;
    }

    if (gRestoreFolderActive || gBackupBatchActive || (gBackupExpect != eBackupExpectNone)) {
        LOG_ERROR("Restore: another backup/restore operation is already in progress\n");
        return;
    }
    open_folder_choose_dialogue_async(restore_folder_chosen, "Choose Folder to Restore From", get_last_backup_folder());
}

void synth_backup_flush_restore_folder(void) {
    if (!gRestoreFolderActive) {
        return;
    }

    if (backup_monotonic_ms() < gRestoreFolderNextSendMs) {
        return; // still pacing since the last send
    }
    // Without this, the progress overlay (synth_render_backup_progress(),
    // synthGraphics.cpp) would only repaint whenever something UNRELATED
    // happened to set gReDraw — see backup_batch_advance()'s own identical
    // comment above for the full reasoning; same fix, same day, same cause.
    gReDraw = true;
    uint32_t presetNumber = gRestoreFolderEntries[gRestoreFolderIndex];
    char     filePath[1280];

    if (restore_folder_find_file(gRestoreFolderFolder, presetNumber, filePath, sizeof(filePath))) {
        uint32_t  length = 0;
        uint8_t * data   = restore_read_file(filePath, &length);

        if (data != NULL) {
            char reason[192];

            if (restore_validate_moog_dump(data, length, 0x03, "Single Preset Dump", reason, sizeof(reason)) && midi_send(data, length)) {
                gRestoreFolderSentCount++;
                LOG_DEBUG("Restore: sent preset %u from %s (%u/%u)\n", (unsigned)presetNumber, filePath,
                          (unsigned)(gRestoreFolderIndex + 1), (unsigned)gRestoreFolderCount);
            } else {
                gRestoreFolderMissingCount++;
                LOG_ERROR("Restore: failed to send preset %u from %s (%s)\n", (unsigned)presetNumber, filePath, reason);
            }
            free(data);
        } else {
            gRestoreFolderMissingCount++;
        }
    } else {
        gRestoreFolderMissingCount++; // file listed a moment ago at restore_folder_chosen() time but gone now — race with something else touching the folder mid-sweep
    }
    gRestoreFolderIndex++;

    if (gRestoreFolderIndex >= gRestoreFolderCount) {
        gRestoreFolderActive = false;
        // Refresh gDevice.progName/every dial from the live edit buffer
        // once the sweep's done — same reasoning as
        // backup_batch_advance()'s own end-of-sweep synth_request_state_dump()
        // call, applied here since sending 100+ presets doesn't itself
        // change what's showing, but it's easy to forget the display is
        // now stale after a sweep this size.
        synth_request_state_dump();
        char summary[192];
        snprintf(summary, sizeof(summary), "Restored %u preset(s), %u missing/failed.",
                 (unsigned)gRestoreFolderSentCount, (unsigned)gRestoreFolderMissingCount);
        show_info_dialogue("Restore Folder", summary);
        LOG_DEBUG("Restore: folder restore finished — %u sent, %u missing/failed\n",
                  (unsigned)gRestoreFolderSentCount, (unsigned)gRestoreFolderMissingCount);
        return;
    }
    gRestoreFolderNextSendMs = backup_monotonic_ms() + RESTORE_FOLDER_SEND_PACING_MS;
}

bool synth_backup_get_export_progress(uint32_t * outCurrent, uint32_t * outTotal, uint32_t * outActionCount) {
    if (!gBackupBatchActive) {
        return false;
    }
    *outCurrent     = gBackupBatchCurrentPreset;
    *outTotal       = BACKUP_BATCH_PRESET_COUNT;
    *outActionCount = gBackupBatchRepliedCount;
    return true;
}

bool synth_backup_get_restore_progress(uint32_t * outCurrent, uint32_t * outTotal, uint32_t * outActionCount) {
    if (!gRestoreFolderActive) {
        return false;
    }
    *outCurrent     = gRestoreFolderIndex + 1; // 0-based index -> 1-based "Nth of M"
    *outTotal       = gRestoreFolderCount;
    *outActionCount = gRestoreFolderSentCount;
    return true;
}
