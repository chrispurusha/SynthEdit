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
static _Atomic int  gBackupExpect           = eBackupExpectNone;

// Valid only while gBackupExpect == eBackupExpectPreset — which preset number
// (1-based) the pending request was for, purely so the save dialog can
// suggest a filename that says so.
static uint32_t     gBackupPresetNum        = 0;

// Set on the CoreMIDI thread just before opening the save dialog, read once
// on the main thread inside the dialog's completion callback. No lock needed
// for that handoff: open_file_write_dialogue_async() below dispatch_asyncs
// onto the main queue, and GCD guarantees everything written on the enqueuing
// thread before a dispatch_async is visible to the block it runs — so these
// only need to be set before that call, not atomic themselves.
static uint8_t *    gPendingBackupData      = NULL;
static uint32_t     gPendingBackupLen       = 0;

// ── Store Patch to Bank ──────────────────────────────────────────────────────
// synth_store_patch_to_bank() (main thread) arms this with the CONFIRMED
// destination preset number (1-based; 0 = no Store pending) before requesting
// a fresh live dump under the SAME gBackupExpect==eBackupExpectLive banner
// synth_backup_current_patch() uses — synth_backup_capture_dump() checks
// this first (see its own body) so a fresh reply gets routed to the Store
// path instead of opening a save-file dialog. Plain uint32_t, not atomic:
// only ever written by the main thread (armed here, cleared by
// synth_backup_capture_dump() on the CoreMIDI thread the moment it consumes
// it) — same "single owner at a time, flag consumed atomically via the
// *Ready bool below" discipline as gBackupBatchReplyReady's own handoff.
static uint32_t     gStoreArmedPresetNumber = 0;

// CoreMIDI-thread -> main-thread handoff for the fetched bytes, once the arm
// above is satisfied — synth_backup_capture_dump() (CoreMIDI thread) copies
// the bytes and publishes gStoreReplyReady=true LAST, after the plain writes
// above it; synth_backup_flush_store() (main/render thread, called once per
// frame) consumes and clears it before doing the actual convert+send, which
// needs the main thread (show_confirm_dialogue()/show_info_dialogue() both
// assume that — see their own comments, fileDialogue.mm). Same shape as
// gBackupBatchReplyReady/Data/Len below, just for a single fetch rather than
// a 128-preset sweep.
static _Atomic bool gStoreReplyReady        = false;
static uint8_t *    gStoreReplyData         = NULL;
static uint32_t     gStoreReplyLen          = 0;
static uint32_t     gStoreReplyPresetNumber = 0;

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
static _Atomic bool      gBackupBatchActive         = false;
static _Atomic bool      gBackupBatchReplyReady     = false;
static uint8_t *         gBackupBatchReplyData      = NULL; // valid only while gBackupBatchReplyReady
static uint32_t          gBackupBatchReplyLen       = 0;

static char              gBackupBatchFolder[1024]   = {0};
static uint32_t          gBackupBatchCurrentPreset  = 0; // 1-based; which preset the outstanding request is for
static double            gBackupBatchRequestSinceMs = 0.0;
static uint32_t          gBackupBatchRepliedCount   = 0;
static uint32_t          gBackupBatchMissingCount   = 0;
static uint32_t          gBackupBatchRetryCount     = 0; // resets to 0 whenever gBackupBatchCurrentPreset genuinely advances — see NAME_SWEEP_MAX_RETRIES above. Used by BOTH modes.
// >0.0 while paced-waiting for the NAME-SWEEP mode's next request to go out
// (see NAME_SWEEP_PACING_MS) — never used by eBatchModeExportFiles, which
// always re-requests immediately, same as before pacing existed. 0.0 means
// "not waiting" (either a request IS currently in flight, tracked by
// gBackupBatchRequestSinceMs above, the sweep isn't active, or this is an
// export). This is exactly what synth_backup_sweep_request_in_flight()
// below reports for the Moog side.
static double            gBackupBatchNextRequestMs  = 0.0;

// Added 2026-07-11 for the Load/Store Patch to Bank name-sweep pickers
// (synth_backup_start_name_sweep() below) — reuses this WHOLE sequencing
// mechanism (gBackupBatchActive, the CoreMIDI-thread-copies/main-thread-
// sequences reply handoff, the per-preset timeout) rather than duplicating
// it, since the only real difference is what happens with each reply: write
// a file (eBatchModeExportFiles, the original behaviour) or just decode a
// name into gNameSweepLabels (eBatchModeNameSweep). synth_backup_capture_dump()'s
// existing `gBackupBatchActive` check (CoreMIDI thread) doesn't need to know
// which mode is active at all — only backup_batch_write_capture() and
// backup_batch_advance()'s completion step (both main-thread) branch on it.
typedef enum {
    eBatchModeExportFiles = 0,
    eBatchModeNameSweep,
} tBackupBatchMode;

static tBackupBatchMode  gBackupBatchMode           = eBatchModeExportFiles;
static tNameSweepPurpose gNameSweepPurpose          = eNameSweepPurposeLoad;

#define BACKUP_BATCH_PRESET_COUNT    128    // matches misc.mm's own "Patch by Number" range / synth_request_single_preset_dump()'s own range check (synthComms.c) — a base Voyager's single bank
// Lowered from 1500 to 1000ms 2026-07-14 (owner, comparing against an
// independently-developed third-party Voyager editor — moogvoyagereditor.
// pistolinstruments.com — whose own bank-fetch loop uses just a 250ms max
// wait per request): still generous relative to a real reply's actual
// latency (well under 100ms in practice) — a fallback for a genuinely
// non-responding location or one that's slower than usual, not the
// normal-case wait. Retry (NAME_SWEEP_MAX_RETRIES) means a single slow
// reply now costs at most ~1s extra rather than ~1.5s before a slot is
// retried or given up on.
#define BACKUP_BATCH_TIMEOUT_MS    1000.0
// "128: " (5) + a generously-truncated single-line name + " — " + a
// Category name (up to "Guitar/Plucked", 15 chars) + NUL — widened from 40
// to 64 on 2026-07-14 once the picker started showing category alongside
// the name (synth_decode_korg_category()/synth_decode_moog_category(),
// synthComms.h) for BOTH device families; shared by gNameSweepLabels
// (Moog) and gKorgSweepLabels (Korg) below — one constant, one width, no
// reason for the two mechanisms to disagree on it.
#define NAME_SWEEP_LABEL_LEN    64

// Shared by BOTH name-sweep mechanisms (Moog gBackupBatchMode==
// eBatchModeNameSweep below, and the Korg-only gKorgSweep* block further
// down) — made common 2026-07-14 (owner: "these should be common
// mechanisms with Voyager and any other device... should be common and
// generic"). Originally Korg-only, added the same day: this sweep no
// longer has anything to hurry for (the picker opens immediately
// regardless of progress, see name_sweep_show_picker()'s own comment), so
// it trickles one request every NAME_SWEEP_PACING_MS rather than firing as
// fast as replies come back, staying out of the way of whatever MIDI
// traffic normal interactive use (dial drags, etc.) is already generating.
// 128-256 requests x 600ms ≈ 1.3-2.5 minutes to fully populate — fine for
// a background fill nobody's waiting on. Deliberately NOT applied to the
// Moog bank-to-folder EXPORT mode — that's a foreground action with its
// own progress modal the user is actively watching complete, not a quiet
// background fill; pacing it would just make a real backup take longer
// for no benefit.
#define NAME_SWEEP_PACING_MS    600.0

// How many times a timed-out request gets resent before its slot is
// finally marked missing/"(no response)" — shared by both name-sweep
// mechanisms AND the Moog bank-to-folder export (a genuine backup
// benefits from this too: fewer real "(write failed)" entries from what
// might just be one slow reply, not a truly unresponsive location).
// Defence in depth alongside the mutual-exclusion fix
// (synth_backup_sweep_request_in_flight()): a real "(no response)" was
// traced 2026-07-14 (owner report) to dial-tweak traffic colliding with an
// in-flight sweep request, which that fix targets directly, but a genuine
// one-off miss (packet loss, a slow reply for some other reason) is still
// possible — worth one or two retries before giving up rather than none.
#define NAME_SWEEP_MAX_RETRIES    2

static char gNameSweepLabels[BACKUP_BATCH_PRESET_COUNT][NAME_SWEEP_LABEL_LEN];

// True once a full 128-preset name sweep has completed at least once this
// session — synth_backup_start_name_sweep() below skips straight to
// showing the picker with the cached gNameSweepLabels instead of re-running
// the whole ~128-request sweep every single time Load/Store Patch to Bank
// is opened (owner request, 2026-07-11: "we could consider caching the
// names once pulled from the bank"). Kept up to date for everything THIS
// APP can write to a known slot — see name_cache_update_from_preset_dump()
// below, called after a successful synth_store_patch_to_bank() send,
// Restore > Patch by Number, and each per-file send in Restore > Bank
// (Individual Files); a whole-bank Restore (single opaque 18KB blob, no
// safe way to extract 128 individual names from it — see this session's
// own Pot Map lesson on not hand-deriving unconfirmed byte layouts)
// invalidates the ENTIRE cache instead, forcing a fresh sweep next time.
// Session-scoped only (not persisted to disk, reset on relaunch) — matches
// gLastMoogDump and every other cache this file already keeps in memory
// only. Explicit gap, not silently assumed away: a name changed via the
// device's OWN front panel (SAVE PRESET with a different name than before)
// has no notification mechanism this app can observe without polling —
// and this codebase already found the hard way that polling interrupts
// the Voyager's own front-panel menus (see
// [[project_voyager_selector_dial_audit]] in the assistant's own memory
// notes) — so that particular staleness is accepted, not solved.
static bool gNameCacheValid = false;

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
// out must be at least as big as `name`'s own buffer; writes "" if name is
// NULL/empty. Takes the name explicitly (not just gDevice.progName) since
// the Korg export sweep (korg_sweep_write_capture_file() below) needs to
// sanitize a SPECIFIC preset's own decoded name — handle_prog_dump() never
// touches gDevice.progName at all (see korg_sweep_capture_reply()'s own
// comment for why), so that global has nothing useful to read during a Korg
// sweep the way it does for Moog's own bank-to-folder export.
static void backup_sanitize_name_for_file(const char * name, char * out, size_t outSize) {
    out[0] = '\0';

    if ((name == NULL) || (name[0] == '\0')) {
        return;
    }
    char * o = out;

    for (const char * p = name; (*p != '\0') && (o < out + outSize - 1); p++) {
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

// Builds "<folder>/Patches-<DeviceName>.txt" — shared by every Backup >
// Bank (Individual Files)… writer and Restore > Bank (Individual Files)…
// reader, Moog and Korg alike. Device-specific rather than a flat
// "Patches.txt" so backing up a SECOND device into a folder that already
// holds a first device's export doesn't silently truncate that first
// device's own index — found 2026-07-14 (owner report): the actual .syx
// filenames already don't collide across device families (Moog's "001
// Name.syx" vs Korg's "A001 Name.syx"), but a single hardcoded index
// filename meant the SECOND backup's own fopen(..., "w") destroyed the
// FIRST one's index outright, orphaning its files — still physically on
// disk, but with nothing left that could find them again. Reuses
// backup_sanitize_name_for_file() above for the same '/'-is-a-path-
// separator reasoning that already applies to a preset name landing in a
// filename. Does NOT disambiguate two physical units of the SAME model
// (e.g. two Voyagers) backed into one folder — there's no per-unit ID in
// either protocol to key on, so that case still needs separate folders,
// same as it always has.
static void backup_index_file_path(char * outPath, size_t outPathSize, const char * folder) {
    const char * deviceName = synth_panel_config()->deviceName;
    char         sanitized[64];

    backup_sanitize_name_for_file((deviceName[0] != '\0') ? deviceName : "Device", sanitized, sizeof(sanitized));
    snprintf(outPath, outPathSize, "%s/Patches-%s.txt", folder, (sanitized[0] != '\0') ? sanitized : "Device");
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

void synth_store_patch_to_bank(uint8_t bank, uint32_t presetNumber) {
    if (!gDevice.connected) {
        LOG_ERROR("Store: no device connected\n");
        return;
    }

    if ((presetNumber < 1) || (presetNumber > 128)) {
        LOG_ERROR("Store: preset number %u out of range (1-128)\n", (unsigned)presetNumber);
        return;
    }
    char message[160];

    // Korg-style (Z1): a single PROGRAM WRITE REQUEST (func 0x11) — no
    // local fetch/convert step at all, so there's no async reply to wait
    // for before showing a result (see synth_send_korg_program_write_
    // request()'s own comment, synthComms.h, on why the device's own
    // WRITE COMPLETED/ERROR reply isn't surfaced here yet). Branches
    // BEFORE the confirmation dialog's own wording, since the two
    // protocols address a slot differently (bank+number vs number alone).
    if (!synth_panel_config()->moogStyleDump) {
        snprintf(message, sizeof(message),
                 "This will overwrite Bank %c, Program %u on the connected device with the CURRENT panel. This cannot be undone.",
                 bank ? 'B' : 'A', (unsigned)presetNumber);

        if (!show_confirm_dialogue("Store Patch to Bank", message)) {
            LOG_DEBUG("Store: cancelled at confirmation\n");
            return;
        }
        synth_send_korg_program_write_request(bank, presetNumber);
        snprintf(message, sizeof(message), "Sent — Bank %c, Program %u should now match the current panel.",
                 bank ? 'B' : 'A', (unsigned)presetNumber);
        show_info_dialogue("Store Patch to Bank", message);
        return;
    }
    snprintf(message, sizeof(message),
             "This will overwrite Preset %u on the connected device with the CURRENT panel. This cannot be undone.",
             (unsigned)presetNumber);

    if (!show_confirm_dialogue("Store Patch to Bank", message)) {
        LOG_DEBUG("Store: cancelled at confirmation\n");
        return;
    }
    gStoreArmedPresetNumber = presetNumber;
    gBackupExpect           = eBackupExpectLive;
    synth_request_state_dump();
    LOG_DEBUG("Store: requested a fresh state dump to store as preset %u\n", (unsigned)presetNumber);
}
// synth_backup_flush_store() is defined further down, right after
// convert_panel_dump_to_preset_dump() (which it calls) — this file's
// existing convention is helpers-before-use with no forward declarations,
// not scattered function order, so it lives next to that helper rather than
// up here with the other single-shot Backup/Store triggers.

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

    backup_index_file_path(indexPath, sizeof(indexPath), gBackupBatchFolder);
    FILE * f = fopen(indexPath, "a");

    if (f != NULL) {
        fprintf(f, "%03u  %s\n", (unsigned)presetNumber, name);
        fclose(f);
    } else {
        LOG_ERROR("Backup: couldn't append to %s\n", indexPath);
    }
}

// Defined further down, after synth_store_patch_to_bank() (which it may
// call) — forward-declared here since backup_batch_advance()'s completion
// step below is the only caller and needs to reach it regardless of
// definition order.
static void name_sweep_show_picker(void);

// Sends (or resends, for a retry) the Single Preset Dump Request for
// gBackupBatchCurrentPreset — shared by backup_batch_advance() (export
// mode's own immediate re-request) and synth_backup_flush_bank_to_folder()
// below (the paced name-sweep re-request, and any mode's timeout retry),
// so all three send exactly the same way rather than three near-duplicate
// copies of these three lines.
static void backup_batch_request_current(void) {
    gBackupExpect              = eBackupExpectPreset;
    synth_request_single_preset_dump(gBackupBatchCurrentPreset);
    gBackupBatchRequestSinceMs = backup_monotonic_ms();
}

// Starts a fresh Moog name sweep — every label initialised to "N: ---" so
// name_sweep_show_picker() has something sane to show for whatever hasn't
// been swept yet, whichever way it was opened. Shared by
// synth_backup_start_name_sweep() (an explicit Load/Store click that found
// nothing running yet) and synth_backup_flush_background_prefetch() below
// (silently, soon after connecting) — the Moog counterpart to
// korg_sweep_start() further down. Caller is responsible for having already
// confirmed nothing else is using gBackupBatchActive/gBackupExpect.
static void moog_name_sweep_start(void) {
    gBackupBatchMode          = eBatchModeNameSweep;
    gBackupBatchActive        = true;
    gBackupBatchCurrentPreset = 1;
    gBackupBatchRetryCount    = 0;
    gBackupBatchNextRequestMs = 0.0;
    gBackupBatchRepliedCount  = 0;
    gBackupBatchMissingCount  = 0;

    for (uint32_t i = 0; i < BACKUP_BATCH_PRESET_COUNT; i++) {
        snprintf(gNameSweepLabels[i], NAME_SWEEP_LABEL_LEN, "%u: ---", (unsigned)(i + 1));
    }

    backup_batch_request_current();
    LOG_DEBUG("Load/Store: starting a %u-preset name sweep\n", (unsigned)BACKUP_BATCH_PRESET_COUNT);
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
    gReDraw                = true;
    gBackupBatchCurrentPreset++;
    gBackupBatchRetryCount = 0; // fresh slot, fresh retry budget — see NAME_SWEEP_MAX_RETRIES's own comment

    if (gBackupBatchCurrentPreset > BACKUP_BATCH_PRESET_COUNT) {
        gBackupBatchActive = false;

        if (gBackupBatchMode == eBatchModeNameSweep) {
            LOG_DEBUG("Name sweep finished — %u replied, %u missing\n",
                      (unsigned)gBackupBatchRepliedCount, (unsigned)gBackupBatchMissingCount);
            gNameCacheValid = true; // even a slot that timed out keeps its "N: (no response)" label — good enough to skip re-sweeping; a future Load/Store on that slot will just show that placeholder rather than silently retrying
            // No auto-popup here (unlike before 2026-07-14) — Load/Store
            // Patch from/to Bank now opens the picker immediately whether
            // or not this sweep has finished, see name_sweep_show_picker()'s
            // own comment; popping a native modal unprompted, whenever this
            // sweep happens to finish (background or not), would steal
            // focus while the user's doing something completely unrelated.
            return;
        }
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

    if (gBackupBatchMode == eBatchModeNameSweep) {
        // Paced, not immediate — see NAME_SWEEP_PACING_MS's own comment.
        // The actual send happens on a later
        // synth_backup_flush_bank_to_folder() tick once this elapses.
        // Export mode (below) is unaffected — always re-requests right away.
        gBackupBatchNextRequestMs = backup_monotonic_ms() + NAME_SWEEP_PACING_MS;
        return;
    }
    backup_batch_request_current();
}

// Writes gNameSweepLabels[presetNumber-1] as "N: Name — Category" (or just
// "N: Name" on a device with no category dial, or "N: (unnamed)"), ready to
// hand straight to show_device_choice_dialogue(). Shared by every path that
// learns a preset's current name: the name sweep itself
// (name_sweep_capture_name() below), and every KEEP-THE-CACHE-CURRENT call
// site (name_cache_update_from_preset_dump() below) — see gNameCacheValid's
// own comment for the full list. category may be NULL/empty (no category
// dial on this device, see synth_decode_moog_category()'s own comment).
static void name_cache_set_label(uint32_t presetNumber, const char * name, const char * category) {
    if ((presetNumber < 1) || (presetNumber > BACKUP_BATCH_PRESET_COUNT)) {
        return;
    }
    char   cleaned[sizeof(gDevice.progName)];

    strncpy(cleaned, name ? name : "", sizeof(cleaned) - 1);
    cleaned[sizeof(cleaned) - 1] = '\0';

    // A Voyager name can carry an embedded '\n' (nameLineWidth's forced line
    // break, matching the front panel's own 2-line LCD — see
    // synth_decode_moog_name()'s own comment) — fine for gDevice.progName's
    // on-screen multi-line display, but this is a SINGLE-LINE dropdown item;
    // a literal newline in an NSPopUpButton title just renders broken.
    // Collapsed to a space, same substitution backup_sanitize_name_for_file()
    // already does for the same underlying reason (a different output
    // format — filenames — but the same "no embedded newline" constraint).
    for (char * p = cleaned; *p != '\0'; p++) {
        if (*p == '\n') {
            *p = ' ';
        }
    }

    char * label = gNameSweepLabels[presetNumber - 1];

    if (cleaned[0] == '\0') {
        snprintf(label, NAME_SWEEP_LABEL_LEN, "%u: (unnamed)", (unsigned)presetNumber);
    } else if (category && (category[0] != '\0')) {
        snprintf(label, NAME_SWEEP_LABEL_LEN, "%u: %s \xe2\x80\x94 %s", (unsigned)presetNumber, cleaned, category);
    } else {
        snprintf(label, NAME_SWEEP_LABEL_LEN, "%u: %s", (unsigned)presetNumber, cleaned);
    }
}

// Decodes the NAME and CATEGORY out of a Single-Preset-Dump-shaped buffer
// (mode 0x03 — a raw file about to be sent, or one already sent) and
// updates the name cache for presetNumber via name_cache_set_label() above.
// Shared by every write path that knows exactly which slot it just wrote
// and has the preset-dump bytes on hand: synth_backup_flush_store() (this
// app's own live-panel write), restore_patch_file_chosen() (Restore >
// Patch by Number), and each per-file send in
// synth_backup_flush_restore_folder() (Restore > Bank Individual Files).
// Does NOT touch gDevice.progName — same "don't disturb what the live
// buffer is showing" reasoning as name_sweep_capture_name() below.
static void name_cache_update_from_preset_dump(uint32_t presetNumber, const uint8_t * data, uint32_t length) {
    tPanelConfig *  cfg        = synth_panel_config();
    const uint8_t * payload    = data + 1;                      // skip F0, matches every other Moog dump handler
    uint32_t        payloadLen = (length > 2) ? length - 2 : 0; // exclude leading skip + trailing F7
    char            name[sizeof(gDevice.progName)];
    char            category[32];

    name[0]     = '\0';
    category[0] = '\0';
    synth_decode_moog_name(payload, payloadLen, cfg->presetNameOffset, cfg->presetNameBitOffset, cfg->presetNameLen, cfg->nameLineWidth, name, sizeof(name));
    synth_decode_moog_category(data, length, category, sizeof(category));
    name_cache_set_label(presetNumber, name, category);
}

// Decodes just the NAME out of a Single Preset Dump reply into
// gNameSweepLabels[gBackupBatchCurrentPreset-1] — the eBatchModeNameSweep
// counterpart to backup_batch_write_capture() below. Uses
// synth_decode_moog_name() directly (not gDevice.progName) so this doesn't
// disturb whatever the live edit buffer's own name is currently showing
// on-screen while the sweep runs.
static void name_sweep_capture_name(const uint8_t * data, uint32_t length) {
    name_cache_update_from_preset_dump(gBackupBatchCurrentPreset, data, length);
    gBackupBatchRepliedCount++;
}

// Writes the just-captured reply to its own file and appends the index
// line, then advances. Called only from the main/render thread.
static void backup_batch_write_capture(const uint8_t * data, uint32_t length) {
    if (gBackupBatchMode == eBatchModeNameSweep) {
        name_sweep_capture_name(data, length);
        return;
    }
    char   nameForFile[sizeof(gDevice.progName)];

    backup_sanitize_name_for_file(gDevice.progName, nameForFile, sizeof(nameForFile));

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
    set_last_backup_folder(synth_current_device_config(), gBackupBatchFolder); // so a later single-file Backup save defaults here too — see its own comment (fileDialogue.h)

    // Fresh index file each run — truncates any previous one from an
    // earlier export into the same folder rather than appending onto
    // stale content. Header identifies which device/when this export is
    // from (a bare list of numbers+names on its own gave no way to tell
    // two exports' Patches.txt files apart, or a genuine device patch list
    // apart from one of these) — backup_batch_append_index_line() (below)
    // only ever appends after this, never touches the header itself.
    char   indexPath[1280];
    backup_index_file_path(indexPath, sizeof(indexPath), gBackupBatchFolder);
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
    gBackupBatchActive        = true;
    gBackupBatchCurrentPreset = 1;
    gBackupBatchRetryCount    = 0;
    gBackupBatchNextRequestMs = 0.0;
    gBackupBatchRepliedCount  = 0;
    gBackupBatchMissingCount  = 0;
    backup_batch_request_current();
    LOG_DEBUG("Backup: starting bank-to-folder export of %u presets to %s\n",
              (unsigned)BACKUP_BATCH_PRESET_COUNT, gBackupBatchFolder);
}

// synth_backup_bank_to_folder() itself lives further down, after the Korg
// name-sweep block (needs gKorgSweepActive/korg_batch_folder_chosen(), both
// declared there) — its own comment there explains why, mirroring
// name_sweep_show_picker()'s own forward-declaration precedent above.

void synth_backup_note_preset_name(uint32_t presetNumber, const char * name) {
    name_cache_set_label(presetNumber, name, NULL); // no category on hand at this call site — see synth_backup_note_preset_name()'s own comment (synthBackup.h)
}

// ── Korg-style name sweep (Z1: 2 banks x 128 programs) ───────────────────────
// Deliberately a SEPARATE, parallel mechanism from the gBackupBatch* state
// above, not a retrofit of it — that machinery is built entirely around
// Voyager's own Single Preset Dump request/reply shape
// (synth_request_single_preset_dump(), BACKUP_BATCH_PRESET_COUNT fixed at a
// single 128-slot bank) and is ALSO shared with the Moog-only folder
// export/restore sweeps; bumping BACKUP_BATCH_PRESET_COUNT to 256 to fit a
// second bank would make every Moog-only sweep spend half its time
// requesting presets 129-256 that don't exist on a single-bank Voyager.
// Keeping this fully isolated means the working, hardware-confirmed Moog
// mechanisms above are completely unaffected. Added 2026-07-14.
#define KORG_SWEEP_PRESET_COUNT    256 // 2 banks x 128 — index i is bank (i/128, 0=A/1=B), program ((i%128)+1)

// Same duality as tBackupBatchMode (Moog, above) — reusing this SAME sweep
// for Backup > Bank (Individual Files)… on a Korg-style device rather than
// building a third parallel mechanism just to write files instead of
// decoding names. eKorgSweepModeExportFiles branches korg_sweep_capture_reply()
// below into ALSO writing the just-captured reply to its own file (still
// decoding the name either way — needed for the filename itself, and it's
// a free bonus for gKorgSweepLabels/the Load-Store picker afterwards).
// Added 2026-07-14.
typedef enum {
    eKorgSweepModeNameSweep = 0,
    eKorgSweepModeExportFiles,
} tKorgSweepMode;

static tKorgSweepMode gKorgSweepMode                 = eKorgSweepModeNameSweep;
static char           gKorgSweepFolder[1024]         = {0}; // only meaningful while gKorgSweepMode == eKorgSweepModeExportFiles

static bool           gKorgSweepActive               = false;
static uint32_t       gKorgSweepIndex                = 0; // 0-based, 0..(KORG_SWEEP_PRESET_COUNT-1)
static uint32_t       gKorgSweepRetryCount           = 0; // resets to 0 whenever gKorgSweepIndex genuinely advances — see NAME_SWEEP_MAX_RETRIES above
static double         gKorgSweepRequestSinceMs       = 0.0;
// >0.0 while paced-waiting for the next request to go out (see
// NAME_SWEEP_PACING_MS) — no request is in flight during this wait, so
// synth_backup_flush_korg_name_sweep()'s reply/timeout checks don't apply
// until it elapses. 0.0 means "not waiting" (either a request IS currently
// in flight, tracked by gKorgSweepRequestSinceMs above, or the sweep isn't
// active at all) — this is exactly what synth_backup_sweep_request_in_flight()
// below reports.
static double         gKorgSweepNextRequestMs        = 0.0;
static uint32_t       gKorgSweepRepliedCount         = 0;
static uint32_t       gKorgSweepMissingCount         = 0;
static bool           gKorgNameCacheValid            = false;
static char           gKorgSweepLabels[KORG_SWEEP_PRESET_COUNT][NAME_SWEEP_LABEL_LEN];

// State for the Korg restore-folder mechanism (Restore > Bank (Individual
// Files), in reverse) — declared here, alongside the rest of the Korg
// sweep's own state, rather than down next to that mechanism's own
// functions (which is where the equivalent Moog state lives, right above
// its own functions) purely so synth_backup_start_name_sweep()'s Korg
// guard above can reference gKorgRestoreFolderActive without a forward-
// declaration; the functions using the rest of this block still live down
// with the Moog restore-folder mechanism's own counterpart, see that
// section's own header comment.
static bool           gKorgRestoreFolderActive       = false;
static char           gKorgRestoreFolderFolder[1024] = {0};
static uint32_t       gKorgRestoreFolderEntries[KORG_SWEEP_PRESET_COUNT]; // sweep-style indices (0..255, bank*128+prog-1), in Patches.txt order, filtered to ones a matching file was actually found for
static uint32_t       gKorgRestoreFolderCount        = 0;
static uint32_t       gKorgRestoreFolderIndex        = 0;
static double         gKorgRestoreFolderNextSendMs   = 0.0;
static uint32_t       gKorgRestoreFolderSentCount    = 0;
static uint32_t       gKorgRestoreFolderMissingCount = 0;

// True only for the narrow window between sending a sweep request and
// either its reply arriving or its timeout firing — NOT true during the
// slow paced gap between requests (see gKorgSweepNextRequestMs/
// gBackupBatchNextRequestMs's own comments). Covers BOTH name-sweep
// mechanisms (Korg gKorgSweep* here, Moog gBackupBatchMode==
// eBatchModeNameSweep) — made common 2026-07-14 (owner: "these should be
// common mechanisms with Voyager and any other device... should be common
// and generic"), originally Korg-only, added the same day after an owner
// report of spurious "(no response)" entries traced to dial-tweak traffic
// colliding with an in-flight sweep request. Exposed so
// synthComms.c's synth_set_panel_dial_value() can defer an outgoing CC or
// Parameter Change while this is true, rather than sending it right into
// the collision window. Deliberately does NOT cover a sweep's WHOLE
// lifetime — that would add real (if small) latency to every dial tweak
// for the 1.3-2.5 minutes a background sweep runs, when only the brief
// per-request round trip (well under 100ms in practice, same figure
// BACKUP_BATCH_TIMEOUT_MS's own comment already relies on) actually needs
// it. Also deliberately does NOT cover the Moog bank-to-folder EXPORT mode
// — see NAME_SWEEP_PACING_MS's own comment for why that's out of scope.
bool synth_backup_sweep_request_in_flight(void) {
    if (gKorgSweepActive) {
        return gKorgSweepNextRequestMs == 0.0;
    }

    if (gBackupBatchActive && (gBackupBatchMode == eBatchModeNameSweep)) {
        return gBackupBatchNextRequestMs == 0.0;
    }
    return false;
}

// CoreMIDI-thread-copies/main-thread-decodes handoff — same reasoning and
// shape as gBackupBatchReplyReady/Data/Len above (name decoding isn't safe
// to do off the main thread; see that block's own comment).
static _Atomic bool gKorgSweepReplyReady = false;
static uint8_t *    gKorgSweepReplyData  = NULL;
static uint32_t     gKorgSweepReplyLen   = 0;

static void korg_sweep_request_current(void) {
    uint8_t  bank = (uint8_t)(gKorgSweepIndex / 128);
    uint32_t prog = (gKorgSweepIndex % 128) + 1;

    gBackupExpect            = eBackupExpectKorgProgram;
    synth_request_korg_program_dump(bank, prog);
    gKorgSweepRequestSinceMs = backup_monotonic_ms();
}

// Advances to the next (bank, program), or finishes the sweep — the Korg
// counterpart to backup_batch_advance() above, called only from the main/
// render thread (synth_backup_flush_korg_name_sweep() below). Never opens
// the picker itself (unlike the Moog counterpart below) — see
// korg_sweep_show_picker()'s own comment for why that's now a fully
// separate concern from sweep progress.
static void korg_sweep_advance(void) {
    gReDraw              = true;
    gKorgSweepIndex++;
    gKorgSweepRetryCount = 0;    // fresh slot, fresh retry budget — see NAME_SWEEP_MAX_RETRIES's own comment

    if (gKorgSweepIndex >= KORG_SWEEP_PRESET_COUNT) {
        gKorgSweepActive    = false;

        if (gKorgSweepMode == eKorgSweepModeExportFiles) {
            LOG_DEBUG("Backup: Korg bank-to-folder export finished — %u captured, %u missing, folder %s\n",
                      (unsigned)gKorgSweepRepliedCount, (unsigned)gKorgSweepMissingCount, gKorgSweepFolder);
        } else {
            LOG_DEBUG("Korg name sweep finished — %u replied, %u missing\n",
                      (unsigned)gKorgSweepRepliedCount, (unsigned)gKorgSweepMissingCount);
        }
        // Even a slot that timed out keeps its "(no response)" label — good
        // enough to skip re-sweeping, same acceptance as gNameCacheValid's
        // own comment above. True either way: an export sweep decodes every
        // name/category exactly like a name-only one (korg_sweep_capture_reply()'s
        // own comment), so it's a free, valid cache-populating side effect,
        // not something only a "real" name sweep should set.
        gKorgNameCacheValid = true;
        return;
    }
    // Paced, not immediate — the actual send happens on a later
    // synth_backup_flush_korg_name_sweep() tick once this elapses.
    gKorgSweepNextRequestMs = backup_monotonic_ms() + NAME_SWEEP_PACING_MS;
}

// Appends one line to <folder>/Patches.txt in the Korg sweep's own "A001  Name"
// format (bank letter + 3-digit zero-padded program, TWO spaces, then name)
// — the Korg counterpart to backup_batch_append_index_line() above (Moog).
// Reopened per call rather than held open across the whole sweep, same "a
// crash or force quit mid-export still leaves what's captured so far intact
// and readable" reasoning as that function's own comment.
static void korg_sweep_append_index_line(uint8_t bank, uint32_t prog, const char * name) {
    char   indexPath[1280];

    backup_index_file_path(indexPath, sizeof(indexPath), gKorgSweepFolder);
    FILE * f = fopen(indexPath, "a");

    if (f != NULL) {
        fprintf(f, "%c%03u  %s\n", bank ? 'B' : 'A', (unsigned)prog, name);
        fclose(f);
    } else {
        LOG_ERROR("Backup: couldn't append to %s\n", indexPath);
    }
}

// Writes a just-captured Program Data Dump reply to its own file — the Korg
// counterpart to backup_batch_write_capture() above (Moog). Called only
// when gKorgSweepMode == eKorgSweepModeExportFiles, from
// korg_sweep_capture_reply() below (which has already decoded `name`, so
// this doesn't need its own separate decode step). Owns Replied/Missing
// counting for this reply itself — the caller does NOT also increment
// those for export mode, only for the name-sweep-only path.
static void korg_sweep_write_capture_file(uint8_t bank, uint32_t prog, const char * name, const uint8_t * data, uint32_t length) {
    char   nameForFile[sizeof(gDevice.progName)];

    backup_sanitize_name_for_file(name, nameForFile, sizeof(nameForFile));

    char   filePath[1280];

    if (nameForFile[0] != '\0') {
        snprintf(filePath, sizeof(filePath), "%s/%c%03u %s.syx", gKorgSweepFolder, bank ? 'B' : 'A', (unsigned)prog, nameForFile);
    } else {
        snprintf(filePath, sizeof(filePath), "%s/%c%03u.syx", gKorgSweepFolder, bank ? 'B' : 'A', (unsigned)prog);
    }
    FILE * f = fopen(filePath, "wb");

    if (f != NULL) {
        fwrite(data, 1, length, f);
        fclose(f);
        gKorgSweepRepliedCount++;
        korg_sweep_append_index_line(bank, prog, nameForFile[0] != '\0' ? nameForFile : "(unnamed)");
    } else {
        LOG_ERROR("Backup: couldn't open %s for writing\n", filePath);
        gKorgSweepMissingCount++;
        korg_sweep_append_index_line(bank, prog, "(write failed)");
    }
}

// Writes gKorgSweepLabels[(bank?128:0)+(prog-1)] as "A1: Name — Category"
// (or just "A1: Name" on a device with no category dial, or "A1: (unnamed)")
// — the Korg counterpart to name_cache_set_label() above (Moog). Shared by
// korg_sweep_capture_reply() below (a sweep in progress) and
// korg_restore_patch_file_chosen() further down (keeping the cache accurate
// after a Restore write, without needing a full re-sweep — same "no extra
// guard needed, a no-op-safe write either way" reasoning as
// name_cache_update_from_preset_dump()'s own comment).
static void korg_sweep_set_label(uint8_t bank, uint32_t prog, const char * name, const char * category) {
    if ((prog < 1) || (prog > 128)) {
        return;
    }
    char * label = gKorgSweepLabels[(bank ? 128 : 0) + (prog - 1)];

    if ((name == NULL) || (name[0] == '\0')) {
        snprintf(label, NAME_SWEEP_LABEL_LEN, "%c%u: (unnamed)", bank ? 'B' : 'A', (unsigned)prog);
    } else if ((category != NULL) && (category[0] != '\0')) {
        snprintf(label, NAME_SWEEP_LABEL_LEN, "%c%u: %s \xe2\x80\x94 %s", bank ? 'B' : 'A', (unsigned)prog, name, category);
    } else {
        snprintf(label, NAME_SWEEP_LABEL_LEN, "%c%u: %s", bank ? 'B' : 'A', (unsigned)prog, name);
    }
}

// Decodes name+category from an arbitrary Program Data Dump buffer (not
// necessarily gKorgSweepIndex's own — a Restore write, e.g.) and updates
// its label via korg_sweep_set_label() above. Shared by
// korg_restore_patch_file_chosen() and synth_backup_flush_korg_restore_folder()
// further down, both of which need this exact "decode this one buffer,
// touch the cache for this one slot" step outside the sweep itself.
static void korg_name_cache_update_from_dump(uint8_t bank, uint32_t prog, const uint8_t * data, uint32_t length) {
    char name[sizeof(gDevice.progName)];
    char category[32];

    name[0]     = '\0';
    category[0] = '\0';
    synth_decode_korg_name(data, length, name, sizeof(name));
    synth_decode_korg_category(data, length, category, sizeof(category));

    for (char * p = name; *p != '\0'; p++) {
        if (*p == '\n') {
            *p = ' ';
        }
    }

    korg_sweep_set_label(bank, prog, name, category);
}

// Decodes the name AND category from a captured Program Data Dump reply and
// updates its gKorgSweepLabels entry via korg_sweep_set_label() above — the
// Korg counterpart to name_sweep_capture_name() above. Uses
// synth_decode_korg_name()/synth_decode_korg_category() directly (not
// gDevice.progName) so this doesn't disturb whatever the live edit
// buffer's own name is currently showing on-screen while the sweep runs.
// Also drives the Backup > Bank (Individual Files)… write when
// gKorgSweepMode == eKorgSweepModeExportFiles — the name/category decode
// above is needed either way (for the label AND the export filename), so
// there's no separate "export capture" entry point; this is it for both.
static void korg_sweep_capture_reply(const uint8_t * data, uint32_t length) {
    uint8_t  bank = (uint8_t)(gKorgSweepIndex / 128);
    uint32_t prog = (gKorgSweepIndex % 128) + 1;
    char     name[sizeof(gDevice.progName)];
    char     category[32];

    name[0]     = '\0';
    category[0] = '\0';
    synth_decode_korg_name(data, length, name, sizeof(name));
    synth_decode_korg_category(data, length, category, sizeof(category));

    for (char * p = name; *p != '\0'; p++) {
        if (*p == '\n') {
            *p = ' '; // same single-line dropdown constraint name_cache_set_label() already handles for Moog
        }
    }

    korg_sweep_set_label(bank, prog, name, category);

    if (gKorgSweepMode == eKorgSweepModeExportFiles) {
        korg_sweep_write_capture_file(bank, prog, name, data, length); // owns its own Replied/Missing counting
    } else {
        gKorgSweepRepliedCount++;
    }
}

// Per-frame poll — the Korg counterpart to synth_backup_flush_bank_to_folder()
// above, but only ever drives THIS sweep (no folder-export mode to share
// with, unlike the Moog version). A no-op unless a Korg name sweep is
// actually active. Call once per frame from the render loop.
void synth_backup_flush_korg_name_sweep(void) {
    if (!gKorgSweepActive) {
        return;
    }

    // Paced gap between requests (see NAME_SWEEP_PACING_MS) — nothing is in
    // flight yet, so skip the reply/timeout checks below until it elapses.
    if (gKorgSweepNextRequestMs > 0.0) {
        if (backup_monotonic_ms() < gKorgSweepNextRequestMs) {
            return;
        }
        gKorgSweepNextRequestMs = 0.0;
        korg_sweep_request_current();
        return;
    }

    if (gKorgSweepReplyReady) {
        gKorgSweepReplyReady = false;
        uint8_t * data   = gKorgSweepReplyData;
        uint32_t  length = gKorgSweepReplyLen;

        gKorgSweepReplyData  = NULL;
        gKorgSweepReplyLen   = 0;
        korg_sweep_capture_reply(data, length);
        free(data);
        korg_sweep_advance();
        return;
    }

    if ((backup_monotonic_ms() - gKorgSweepRequestSinceMs) >= BACKUP_BATCH_TIMEOUT_MS) {
        uint8_t  bank = (uint8_t)(gKorgSweepIndex / 128);
        uint32_t prog = (gKorgSweepIndex % 128) + 1;

        // Retry the SAME slot a couple of times before giving up on it — see
        // NAME_SWEEP_MAX_RETRIES's own comment. gKorgSweepIndex deliberately
        // doesn't advance here; korg_sweep_request_current() just re-sends
        // for the current one with a fresh timestamp.
        if (gKorgSweepRetryCount < NAME_SWEEP_MAX_RETRIES) {
            gKorgSweepRetryCount++;
            LOG_DEBUG("Korg name sweep: %c%u timed out after %.0fms, retrying (%u/%u)\n",
                      bank ? 'B' : 'A', (unsigned)prog, BACKUP_BATCH_TIMEOUT_MS,
                      (unsigned)gKorgSweepRetryCount, (unsigned)NAME_SWEEP_MAX_RETRIES);
            korg_sweep_request_current();
            return;
        }
        LOG_DEBUG("Korg name sweep: %c%u timed out after %u attempt(s)\n",
                  bank ? 'B' : 'A', (unsigned)prog, (unsigned)(NAME_SWEEP_MAX_RETRIES + 1));
        snprintf(gKorgSweepLabels[gKorgSweepIndex], NAME_SWEEP_LABEL_LEN,
                 "%c%u: (no response)", bank ? 'B' : 'A', (unsigned)prog);
        gKorgSweepMissingCount++;
        korg_sweep_advance();
    }
}

// Starts (or restarts, on a fresh connection) the Korg name sweep — called
// either by synth_backup_flush_background_prefetch() below (silently, soon
// after connecting) or synth_backup_start_name_sweep() (an explicit Load/
// Store click that found no sweep running yet). Every label starts as
// "A1: ---" so korg_sweep_show_picker() has something sane to show for
// whatever hasn't been swept yet, whichever way the picker was opened.
static void korg_sweep_start(void) {
    gKorgSweepMode          = eKorgSweepModeNameSweep; // always the name-only entry point — korg_batch_folder_chosen() below sets up export mode itself, without going through here
    gKorgSweepActive        = true;
    gKorgSweepIndex         = 0;
    gKorgSweepRetryCount    = 0;
    gKorgSweepRepliedCount  = 0;
    gKorgSweepMissingCount  = 0;
    gKorgSweepNextRequestMs = 0.0;

    for (uint32_t i = 0; i < KORG_SWEEP_PRESET_COUNT; i++) {
        uint8_t  bank = (uint8_t)(i / 128);
        uint32_t prog = (i % 128) + 1;

        snprintf(gKorgSweepLabels[i], NAME_SWEEP_LABEL_LEN, "%c%u: ---", bank ? 'B' : 'A', (unsigned)prog);
    }

    korg_sweep_request_current(); // first request goes out right away; every one after this is paced (korg_sweep_advance())
    LOG_DEBUG("Load/Store: starting a %u-program Korg name sweep (2 banks x 128), paced %.0fms/request\n",
              (unsigned)KORG_SWEEP_PRESET_COUNT, NAME_SWEEP_PACING_MS);
}

// Runs on the main thread once the user has chosen (or cancelled) a backup
// folder — the Korg counterpart to backup_batch_folder_chosen() above
// (Moog). Deliberately does NOT call korg_sweep_start() (that's always the
// plain name-only entry point — see its own comment) — sets up the SAME
// underlying gKorgSweep* state directly, in export mode, mirroring
// backup_batch_folder_chosen()'s own inline setup rather than sharing a
// helper neither side really needs.
static void korg_batch_folder_chosen(const char * path) {
    if (path == NULL) {
        LOG_DEBUG("Backup: Korg bank-to-folder export cancelled\n");
        return;
    }
    strncpy(gKorgSweepFolder, path, sizeof(gKorgSweepFolder) - 1);
    gKorgSweepFolder[sizeof(gKorgSweepFolder) - 1] = '\0';
    set_last_backup_folder(synth_current_device_config(), gKorgSweepFolder);

    // Fresh index file each run — see backup_batch_folder_chosen()'s own
    // identical comment for why (truncates any previous export into the
    // same folder rather than appending onto stale content).
    char   indexPath[1280];
    backup_index_file_path(indexPath, sizeof(indexPath), gKorgSweepFolder);
    FILE * f = fopen(indexPath, "w");

    if (f != NULL) {
        const char * deviceName = synth_panel_config()->deviceName;
        time_t       now        = time(NULL);
        char         timestamp[32];

        strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M", localtime(&now));
        fprintf(f, "%s Bank Backup — %s\n", (deviceName[0] != '\0') ? deviceName : "Patch", timestamp);
        fprintf(f, "%u programs requested from device (2 banks x 128)\n\n", (unsigned)KORG_SWEEP_PRESET_COUNT);
        fclose(f);
    }
    gKorgSweepMode                                 = eKorgSweepModeExportFiles;
    gKorgSweepActive                               = true;
    gKorgSweepIndex                                = 0;
    gKorgSweepRetryCount                           = 0;
    gKorgSweepRepliedCount                         = 0;
    gKorgSweepMissingCount                         = 0;
    gKorgSweepNextRequestMs                        = 0.0;

    for (uint32_t i = 0; i < KORG_SWEEP_PRESET_COUNT; i++) {
        uint8_t  bank = (uint8_t)(i / 128);
        uint32_t prog = (i % 128) + 1;

        snprintf(gKorgSweepLabels[i], NAME_SWEEP_LABEL_LEN, "%c%u: ---", bank ? 'B' : 'A', (unsigned)prog);
    }

    korg_sweep_request_current();
    LOG_DEBUG("Backup: starting Korg bank-to-folder export of %u programs to %s\n",
              (unsigned)KORG_SWEEP_PRESET_COUNT, gKorgSweepFolder);
}

void synth_backup_bank_to_folder(void) {
    if (!gDevice.connected) {
        LOG_ERROR("Backup: no device connected\n");
        return;
    }

    // Korg-style (Z1): fully separate sweep/folder-chosen path — see the
    // Korg name-sweep block's own header comment for why this reuses that
    // SAME sweep mechanism (in export mode) rather than gBackupBatch* below,
    // which is built entirely around Voyager's own Single Preset Dump
    // request/reply shape and a fixed 128-slot bank. Added 2026-07-14.
    if (!synth_panel_config()->moogStyleDump) {
        if (gKorgSweepActive) {
            LOG_ERROR("Backup: a Korg sweep is already in progress\n");
            return;
        }

        if (gBackupExpect != eBackupExpectNone) {
            LOG_ERROR("Backup: another backup operation is already in progress\n");
            return;
        }
        open_folder_choose_dialogue_async(korg_batch_folder_chosen, "Choose Backup Folder", get_last_backup_folder(synth_current_device_config()));
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
    open_folder_choose_dialogue_async(backup_batch_folder_chosen, "Choose Backup Folder", get_last_backup_folder(synth_current_device_config()));
}

// Shows the "A1: Name — Category" picker and acts on whatever the user
// chose — the Korg counterpart to name_sweep_show_picker() below (kept
// fully separate rather than parameterizing that one, same isolation
// reasoning as the rest of this block). Opens IMMEDIATELY regardless of
// sweep progress — 2026-07-14 user request ("allow the picker, with '---'
// unpopulated names before the full set is gleaned") — showing real names
// for whatever korg_sweep_capture_reply() has filled in so far and "---"
// (from korg_sweep_start()'s own initialisation) for anything not reached
// yet. The sweep itself (if still running) simply pauses while this native
// modal blocks the main thread — see synth_backup_flush_korg_name_sweep(),
// only ever driven from the same per-frame render-loop call this modal
// blocks — and resumes filling in the rest once the user dismisses it,
// whether they chose an entry or cancelled.
static void korg_sweep_show_picker(void) {
    const char * labelPtrs[KORG_SWEEP_PRESET_COUNT];

    for (uint32_t i = 0; i < KORG_SWEEP_PRESET_COUNT; i++) {
        labelPtrs[i] = gKorgSweepLabels[i];
    }

    // Store defaults to the slot the CURRENT panel was originally loaded
    // from — gDevice.currentProgram is 0-based WITHIN a bank (0-127) and
    // has no bank information of its own, so this can only default to a
    // Bank A slot (index == currentProgram) or fall back to the first
    // entry, same "no equivalent natural default" reasoning as Load
    // already has below for the Moog picker; not worth a bigger tracking
    // change just for this cosmetic default.
    uint32_t     defaultIndex = 0;

    if (  (gNameSweepPurpose == eNameSweepPurposeStore)
       && (gDevice.currentProgram >= 0) && (gDevice.currentProgram < 128)) {
        defaultIndex = (uint32_t)gDevice.currentProgram;
    }
    const char * title        = (gNameSweepPurpose == eNameSweepPurposeLoad) ? "Load Patch from Bank" : "Store Patch to Bank";
    const char * message      = (gNameSweepPurpose == eNameSweepPurposeLoad)
                ? "Choose a program to load into the live panel:"
                : "Choose a program to store the current panel to:";
    int32_t      chosen       = show_device_choice_dialogue(title, message, labelPtrs, KORG_SWEEP_PRESET_COUNT, defaultIndex);

    // No synth_request_state_dump() here (removed 2026-07-14) — handle_prog_dump()
    // never touches gDevice.progName/the live dials in the first place (see
    // its own comment, synthComms.c), so there's nothing to restore on
    // cancel; on Load, synth_change_program() (called via
    // synth_korg_select_program(), the tail of synth_load_patch_from_bank()'s
    // Korg branch) already arms its own debounced state-dump refresh, so an
    // immediate extra request here was pure duplicate traffic — and, worse,
    // one that could land right as a background sweep's own request/reply
    // was in flight (owner report: one observed "patch select failing"
    // while a sweep was running).
    if (chosen < 0) {
        LOG_DEBUG("Load/Store: picker cancelled\n");
        return;
    }
    uint8_t      bank         = (uint8_t)((uint32_t)chosen / 128);
    uint32_t     prog         = ((uint32_t)chosen % 128) + 1;

    if (gNameSweepPurpose == eNameSweepPurposeLoad) {
        synth_load_patch_from_bank(bank, prog);
    } else {
        synth_store_patch_to_bank(bank, prog);
    }
}

void synth_backup_start_name_sweep(tNameSweepPurpose purpose) {
    if (!gDevice.connected) {
        LOG_ERROR("Load/Store: no device connected\n");
        return;
    }
    gNameSweepPurpose = purpose;

    // Korg-style (Z1): fully separate sweep/picker — see this whole
    // block's own header comment for why. Added 2026-07-14.
    if (!synth_panel_config()->moogStyleDump) {
        // Blocks a genuinely CONFLICTING operation — a Korg restore-folder
        // sweep (gKorgRestoreFolderActive) or this SAME gKorgSweep*
        // mechanism actually mid-EXPORT (writing files, not just decoding
        // names for the cache) — but deliberately NOT a plain name sweep
        // (gKorgSweepActive with gKorgSweepMode still eKorgSweepModeNameSweep)
        // or this sweep's own eBackupExpectKorgProgram: the picker below
        // opens even while a background NAME sweep is mid-flight, so seeing
        // either of those here is completely normal, not a conflict.
        // gBackupBatchActive (Moog) can't actually be true on a Korg-style
        // device at all — every Moog entry point already refuses to start
        // on one — kept here anyway as defence in depth.
        if (  gBackupBatchActive || gKorgRestoreFolderActive
           || (gKorgSweepActive && (gKorgSweepMode == eKorgSweepModeExportFiles))
           || ((gBackupExpect != eBackupExpectNone) && (gBackupExpect != eBackupExpectKorgProgram))) {
            LOG_ERROR("Load/Store: another backup operation is already in progress\n");
            return;
        }

        if (!gKorgSweepActive && !gKorgNameCacheValid) {
            // Nothing running yet (e.g. clicked in the first moment after
            // connecting, before synth_backup_flush_background_prefetch()'s
            // own settle delay elapsed) — start it now so labels begin
            // filling in, but don't wait: the picker opens right below
            // regardless.
            korg_sweep_start();
        }
        korg_sweep_show_picker();
        return;
    }
    // true only while OUR OWN name-sweep-mode batch has the floor —
    // gBackupBatchActive/gBackupExpect==eBackupExpectPreset are also both
    // used by bank-to-folder EXPORT mode and by a lone "Backup > Patch by
    // Number" fetch, neither of which this click should be allowed to
    // barge in on (synth_backup_capture_dump()'s own eBackupExpectPreset
    // branch disambiguates those from a sweep reply purely by
    // gBackupBatchActive, so letting a stray gBackupExpect==eBackupExpectPreset
    // through here when gBackupBatchActive is false could steal a lone
    // fetch's own reply).
    bool ownSweepInFlight = gBackupBatchActive && (gBackupBatchMode == eBatchModeNameSweep);

    // Blocks a genuinely CONFLICTING operation (export mode, a lone
    // Patch-by-Number fetch, a Live/Bank backup) but deliberately NOT this
    // sweep's own in-progress request — the picker below opens even while
    // a background sweep is mid-flight, mirroring the Korg branch above.
    // 2026-07-14 (owner: "these should be common mechanisms with Voyager
    // and any other device... should be common and generic").
    if (!ownSweepInFlight && (gBackupBatchActive || (gBackupExpect != eBackupExpectNone))) {
        LOG_ERROR("Load/Store: another backup operation is already in progress\n");
        return;
    }

    if (!ownSweepInFlight && !gNameCacheValid) {
        // Nothing running yet (e.g. clicked in the first moment after
        // connecting, before synth_backup_flush_background_prefetch()'s
        // own settle delay elapsed) — start it now so labels begin
        // filling in, but don't wait: the picker opens right below
        // regardless.
        moog_name_sweep_start();
    } else {
        // Already have every name from a previous sweep this session (kept
        // current by name_cache_update_from_preset_dump() at every write
        // this app makes since — see gNameCacheValid's own comment for the
        // accepted staleness gap), OR one is already running in the
        // background — either way, skip straight to the picker below
        // instead of starting a redundant sweep.
        LOG_DEBUG("Load/Store: %s preset names (purpose=%d)\n",
                  ownSweepInFlight ? "reusing in-progress" : "using cached", (int)purpose);
    }
    name_sweep_show_picker();
}

// Shows the "N: Name — Category" picker and acts on whatever the user
// chose. Opens IMMEDIATELY regardless of sweep progress — 2026-07-14 (owner:
// "we should allow the picker, with '---' unpopulated names before the full
// set is gleaned") — showing real names for whatever
// name_cache_update_from_preset_dump()/name_sweep_capture_name() has filled
// in so far and "---" (from synth_backup_start_name_sweep()'s own
// initialisation) for anything not reached yet. The sweep itself (if still
// running) simply pauses while this native modal blocks the main thread —
// see synth_backup_flush_bank_to_folder(), only ever driven from the same
// per-frame render-loop call this modal blocks — and resumes filling in the
// rest once the user dismisses it, whether they chose an entry or
// cancelled. Called directly from synth_backup_start_name_sweep() now
// (never automatically on sweep completion — see backup_batch_advance()'s
// own comment for why), same "picker decoupled from sweep progress" design
// as the Korg-side korg_sweep_show_picker() above.
static void name_sweep_show_picker(void) {
    const char * labelPtrs[BACKUP_BATCH_PRESET_COUNT];

    for (uint32_t i = 0; i < BACKUP_BATCH_PRESET_COUNT; i++) {
        labelPtrs[i] = gNameSweepLabels[i];
    }

    // Store defaults to the slot the CURRENT panel was originally loaded
    // from (gDevice.currentProgram, 0-based; -1 = unknown) — owner request,
    // 2026-07-11: "default to write to the slot... the one we're working on
    // came from". Load has no equivalent natural default, so it always
    // starts at the first entry (index 0).
    uint32_t     defaultIndex = 0;

    if (  (gNameSweepPurpose == eNameSweepPurposeStore)
       && (gDevice.currentProgram >= 0) && (gDevice.currentProgram < (int32_t)BACKUP_BATCH_PRESET_COUNT)) {
        defaultIndex = (uint32_t)gDevice.currentProgram;
    }
    const char * title        = (gNameSweepPurpose == eNameSweepPurposeLoad) ? "Load Patch from Bank" : "Store Patch to Bank";
    const char * message      = (gNameSweepPurpose == eNameSweepPurposeLoad)
                ? "Choose a preset to load into the live panel:"
                : "Choose a preset to store the current panel to:";
    int32_t      chosen       = show_device_choice_dialogue(title, message, labelPtrs, BACKUP_BATCH_PRESET_COUNT, defaultIndex);

    // No synth_request_state_dump() here (removed 2026-07-14) — the sweep
    // no longer leaves gDevice.progName showing the last-swept preset's
    // name at all (handle_moog_single_preset_dump() now skips that write
    // during name-sweep mode specifically, see its own comment in
    // synthComms.c), so there's nothing to restore on cancel; on Load,
    // synth_change_program() (the tail of synth_load_patch_from_bank()'s
    // Moog branch) already arms its own debounced state-dump refresh, and
    // Store already fetches its own fresh dump as part of capturing what to
    // write — so an immediate extra request here was pure duplicate
    // traffic, and, worse, one that could land right as a background
    // sweep's own request/reply was in flight (owner report: one observed
    // "patch select failing" while a sweep was running).
    if (chosen < 0) {
        LOG_DEBUG("Load/Store: picker cancelled\n");
        return;
    }

    if (gNameSweepPurpose == eNameSweepPurposeLoad) {
        synth_load_patch_from_bank(0, (uint32_t)(chosen + 1)); // bank ignored for Moog-style — see synth_load_patch_from_bank()'s own comment (synthComms.h)
    } else {
        synth_store_patch_to_bank(0, (uint32_t)(chosen + 1));  // bank ignored for Moog-style — see synth_store_patch_to_bank()'s own comment (synthBackup.h)
    }
}

void synth_backup_flush_bank_to_folder(void) {
    if (!gBackupBatchActive) {
        return;
    }

    // Paced gap between requests (name-sweep mode only — see
    // NAME_SWEEP_PACING_MS) — nothing is in flight yet, so skip the reply/
    // timeout checks below until it elapses. Never set for export mode
    // (backup_batch_advance()'s own comment), so this is a no-op there.
    if (gBackupBatchNextRequestMs > 0.0) {
        if (backup_monotonic_ms() < gBackupBatchNextRequestMs) {
            return;
        }
        gBackupBatchNextRequestMs = 0.0;
        backup_batch_request_current();
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
        // Retry the SAME slot a couple of times before giving up — see
        // NAME_SWEEP_MAX_RETRIES's own comment. gBackupBatchCurrentPreset
        // deliberately doesn't advance here; backup_batch_request_current()
        // just re-sends for the current one with a fresh timestamp.
        if (gBackupBatchRetryCount < NAME_SWEEP_MAX_RETRIES) {
            gBackupBatchRetryCount++;
            LOG_DEBUG("Backup: preset %u timed out after %ums, retrying (%u/%u)\n",
                      (unsigned)gBackupBatchCurrentPreset, (unsigned)BACKUP_BATCH_TIMEOUT_MS,
                      (unsigned)gBackupBatchRetryCount, (unsigned)NAME_SWEEP_MAX_RETRIES);
            backup_batch_request_current();
            return;
        }
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
        LOG_ERROR("Backup: preset %u did not reply after %u attempt(s), skipping\n",
                  (unsigned)gBackupBatchCurrentPreset, (unsigned)(NAME_SWEEP_MAX_RETRIES + 1));
        gBackupBatchMissingCount++;

        if (gBackupBatchMode == eBatchModeNameSweep) {
            snprintf(gNameSweepLabels[gBackupBatchCurrentPreset - 1], NAME_SWEEP_LABEL_LEN,
                     "%u: (no response)", (unsigned)gBackupBatchCurrentPreset);
        } else {
            backup_batch_append_index_line(gBackupBatchCurrentPreset, "(no response)");
        }
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
                set_last_backup_folder(synth_current_device_config(), folder);
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

    if ((kind == eBackupExpectLive) && (gStoreArmedPresetNumber != 0)) {
        // A "Store Patch to Bank…" fetch, not a "Save Panel to File…" one —
        // see gStoreArmedPresetNumber's own comment above for why this check
        // comes before anything file-related. Same CoreMIDI-thread-copies/
        // main-thread-processes handoff as the bank-to-folder batch export
        // just below, for the same reason (this runs on the CoreMIDI thread,
        // and the eventual convert+send+result-dialog work needs the main
        // thread — see synth_backup_flush_store()).
        uint8_t * storeCopy = (uint8_t *)malloc(length);

        if (storeCopy == NULL) {
            LOG_ERROR("Store: out of memory copying %u byte dump\n", (unsigned)length);
            gStoreArmedPresetNumber = 0;
            return;
        }
        memcpy(storeCopy, data, length);
        gStoreReplyData         = storeCopy;
        gStoreReplyLen          = length;
        gStoreReplyPresetNumber = gStoreArmedPresetNumber;
        gStoreArmedPresetNumber = 0;
        gStoreReplyReady        = true;
        return;
    }

    if ((kind == eBackupExpectPreset) && gBackupBatchActive) {
        // Discard a corrupt reply outright rather than handing it off — see
        // synth_moog_single_preset_dump_intact()'s own comment (synthComms.h)
        // for what this catches and why. Deliberately just drops it and
        // returns rather than doing anything else here: gBackupBatchRequestSinceMs
        // is untouched, so synth_backup_flush_bank_to_folder()'s own timeout
        // check (purely elapsed-time-based, doesn't care whether gBackupExpect
        // — already cleared above — is still armed) fires exactly as if
        // nothing had arrived at all, reusing the EXISTING retry-then-give-up
        // logic (NAME_SWEEP_MAX_RETRIES) rather than needing separate
        // corruption-specific retry handling. Added 2026-07-14.
        if (!synth_moog_single_preset_dump_intact(data, length)) {
            LOG_DEBUG("Backup: preset %u reply looked corrupt (dropped MIDI byte?), discarding — will retry on timeout\n",
                      (unsigned)gBackupBatchCurrentPreset);
            return;
        }
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

    if ((kind == eBackupExpectKorgProgram) && gKorgSweepActive) {
        // Discard a corrupt reply outright — same reasoning as the Moog
        // branch just above (synth_moog_single_preset_dump_intact()'s own
        // comment), reusing gKorgSweepRequestSinceMs's own timeout+retry
        // instead of separate corruption-specific handling. Added 2026-07-14.
        if (!synth_korg_program_dump_intact(data, length)) {
            LOG_DEBUG("Load/Store: Korg sweep reply looked corrupt, discarding — will retry on timeout\n");
            return;
        }
        // Korg name sweep in progress — same CoreMIDI-thread-copies/main-
        // thread-decodes handoff as the Moog bank-to-folder sweep just
        // above, for the same reason (this runs on the CoreMIDI thread —
        // see the Korg sweep block's own header comment). No plain
        // "Backup > Patch by Number" equivalent yet for Korg (see
        // [[project_z1_load_from_bank_todo]]), so unlike eBackupExpectPreset
        // above, there's no generic fallthrough needed here if
        // gKorgSweepActive is false — that would mean a stray/unexpected
        // reply, safe to just drop.
        uint8_t * korgCopy = (uint8_t *)malloc(length);

        if (korgCopy == NULL) {
            LOG_ERROR("Backup: out of memory copying %u byte dump (Korg name sweep)\n", (unsigned)length);
            return;
        }
        memcpy(korgCopy, data, length);
        gKorgSweepReplyData  = korgCopy;
        gKorgSweepReplyLen   = length;
        gKorgSweepReplyReady = true;
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

        backup_sanitize_name_for_file(gDevice.progName, nameForFile, sizeof(nameForFile));

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
    open_file_write_dialogue_async(backup_save_callback, defaultName, get_last_backup_folder(synth_current_device_config()));
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

// Korg-style counterpart to restore_validate_moog_dump() above — validates
// a raw F0...F7 SysEx capture as a genuine Program Data Dump (func 0x4C) for
// the connected device (mfrId/familyId/channel-nibble all checked, same
// header shape korg_decode_prog_dump() in synthComms.c already reads for
// the exact same message type). Re-implemented independently here rather
// than exposing that static function, same reasoning
// restore_validate_moog_dump() already gives for its own Moog equivalent.
// outBank/outProg (if non-NULL) receive the dump's own embedded destination
// address on success — a Program Data Dump carries its own bank/program in
// the header (unlike a Moog Panel Dump), so there's nothing to separately
// track the way gBackupPresetNum is for the Moog "Backup > Patch by
// Number" flow.
static bool restore_validate_korg_dump(const uint8_t * data, uint32_t length, char * reason, size_t reasonSize,
                                       uint8_t * outBank, uint32_t * outProg) {
    tPanelConfig * cfg     = synth_panel_config();

    if (cfg->moogStyleDump) {
        snprintf(reason, reasonSize, "The connected device isn't Korg-style — this Restore action doesn't support it.");
        LOG_ERROR("Restore: connected device isn't Korg-style\n");
        return false;
    }
    uint32_t       n       = cfg->manufacturerIdLen;
    uint32_t       funcPos = 3 + n; // F0 + mfrId(n) + (0x30|channel) + familyId, THEN func — see is_synth_sysex()'s own comment (synthComms.c)

    if ((length < funcPos + 4) || (data[0] != MIDI_SYSEX_START) || (data[length - 1] != MIDI_SYSEX_END)) {
        snprintf(reason, reasonSize, "This file doesn't look like a raw SysEx capture (%u bytes) — was it saved by this app's own Backup, unmodified?", (unsigned)length);
        LOG_ERROR("Restore: Patch by Number dump doesn't look like a raw SysEx capture (%u bytes)\n", (unsigned)length);
        return false;
    }

    if ((memcmp(&data[1], cfg->manufacturerId, n) != 0) || ((data[n + 1] & 0xF0) != 0x30) || (data[n + 2] != cfg->familyId)) {
        snprintf(reason, reasonSize, "This file isn't a dump for the connected device (manufacturer/family ID mismatch).");
        LOG_ERROR("Restore: Patch by Number dump isn't for the connected device (mfrId/familyId mismatch)\n");
        return false;
    }

    if (data[funcPos] != SYNTH_FUNC_PROG_DUMP) {
        snprintf(reason, reasonSize, "This file is a func 0x%02X message, not a Program Data Dump — pick a file saved with Backup > Bank (Individual Files).", (unsigned)data[funcPos]);
        LOG_ERROR("Restore: Patch by Number dump func 0x%02X, expected 0x%02X\n", (unsigned)data[funcPos], (unsigned)SYNTH_FUNC_PROG_DUMP);
        return false;
    }

    if (outBank != NULL) {
        *outBank = data[funcPos + 1] & 0x01;
    }

    if (outProg != NULL) {
        *outProg = (uint32_t)data[funcPos + 2] + 1;
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

// Inverse of convert_preset_dump_to_panel_dump() above — inserts a
// preset-number byte and flips the mode byte the other way, turning a Panel
// Dump into a Single Preset Dump addressed to a chosen destination. Used by
// synth_store_patch_to_bank() below ("Store Patch to Bank…", G2-Edit
// naming): the ONLY way to write the current live panel to a specific
// stored location is the same "SEND PRESET(S)" mechanism Restore > Patch by
// Number already proved works (see [[project_voyager_restore_mechanism]] in
// the assistant's own memory notes) — there's no separate "commit edit
// buffer to slot N" SysEx command in the manual, just "send a Single Preset
// Dump addressed to N and the device stores it there". presetNumber0based
// is 0-127 (caller subtracts 1 from the 1-based UI number, same convention
// synth_request_single_preset_dump() uses on the request side). Returns a
// newly malloc'd buffer (caller frees) and writes its length to *outLen, or
// NULL if srcLength is too short to be a genuine Panel Dump.
static uint8_t * convert_panel_dump_to_preset_dump(const uint8_t * src, uint32_t srcLength, uint8_t presetNumber0based, uint32_t * outLen) {
    if (srcLength < 6) { // F0 mfrId productId deviceId mode ...data... F7, minimum shape
        return NULL;
    }
    uint32_t  dstLength = srcLength + 1;
    uint8_t * dst       = (uint8_t *)malloc(dstLength);

    if (dst == NULL) {
        return NULL;
    }
    memcpy(dst, src, 4);                          // F0 mfrId productId deviceId
    dst[4]  = 0x03;                               // mode: Single Preset Dump (was 0x02, Panel Dump)
    dst[5]  = presetNumber0based;                 // inserted preset-number byte
    memcpy(&dst[6], &src[5], srcLength - 5);      // everything after the mode byte, including the trailing F7
    *outLen = dstLength;
    return dst;
}

// Per-frame poll for synth_store_patch_to_bank()'s own pending fetch — see
// gStoreReplyReady's own comment above for why this work (convert+send+
// result dialog) happens here on the main/render thread rather than inside
// synth_backup_capture_dump() (CoreMIDI thread) where the reply itself
// lands.
void synth_backup_flush_store(void) {
    if (!gStoreReplyReady) {
        return;
    }
    gStoreReplyReady = false;

    uint8_t * data         = gStoreReplyData;
    uint32_t  length       = gStoreReplyLen;
    uint32_t  presetNumber = gStoreReplyPresetNumber;

    gStoreReplyData  = NULL;
    gStoreReplyLen   = 0;

    uint32_t  convertedLen = 0;
    uint8_t * converted    = convert_panel_dump_to_preset_dump(data, length, (uint8_t)(presetNumber - 1), &convertedLen);

    free(data);

    if (converted == NULL) {
        LOG_ERROR("Store: failed to convert %u byte Panel Dump for preset %u\n", (unsigned)length, (unsigned)presetNumber);
        show_info_dialogue("Store Patch Failed", "Couldn't prepare the current panel for sending — see the debug log.");
        return;
    }
    char      message[160];

    if (midi_send(converted, convertedLen)) {
        LOG_DEBUG("Store: sent %u byte Single Preset Dump (stored to preset %u)\n",
                  (unsigned)convertedLen, (unsigned)presetNumber);
        // Keeps the name cache (gNameCacheValid) accurate for this slot
        // without needing a full re-sweep — see that flag's own comment.
        name_cache_update_from_preset_dump(presetNumber, converted, convertedLen);
        snprintf(message, sizeof(message), "Sent — Preset %u should now match the current panel.", (unsigned)presetNumber);
        show_info_dialogue("Store Patch to Bank", message);
    } else {
        LOG_ERROR("Store: failed to send %u byte Single Preset Dump for preset %u\n",
                  (unsigned)convertedLen, (unsigned)presetNumber);
        show_info_dialogue("Store Patch Failed", "The message couldn't be sent — see the debug log for the exact MIDI error.");
    }
    free(converted);
}

// Korg-style counterpart to the Moog branch below — Z1 (and any other
// Korg-style device) has no single "load this whole dump" SysEx the way a
// Moog-style device's own Panel Dump does; the closest equivalent is
// replaying every dial's own value as an individual live Parameter Change
// (group=/param=), which only ever touches the live edit buffer, never
// flash — this deliberately never sends a PROGRAM WRITE REQUEST, matching
// the owner's own "wouldn't store to Z1 flash yet" caution (2026-07-14).
// Paced with CFRunLoopRunInMode (not usleep — restore_panel_file_chosen()
// below runs on the main thread's own CFRunLoop, dispatched via open_file_
// read_dialogue_async()'s NSOpenPanel completion handler, so a blocking
// sleep would stall event processing for no reason; same reasoning
// midi_connect_all()'s own identity-request pacing already follows,
// midiComms.c) between sends — NOT yet verified against real hardware how
// much margin the Z1's own SysEx receiver needs for a burst this size
// (~150-250 messages depending on the device); chosen conservatively,
// tighten if a full restore proves reliable at a faster pace or lengthen if
// any values land wrong/missing. Only handles the plain single-byte dumpOffset/
// dumpShift/dumpMask model (no Z1 dial uses dumpBitWidth — that's Kronos-only
// so far, and Kronos itself never reaches this function since
// restore_validate_korg_dump() below requires a genuine func 0x4C Program
// Data Dump, which Kronos doesn't speak).
#define RESTORE_PANEL_PACE_SECONDS    0.01

static void restore_panel_korg_file(const uint8_t * data, uint32_t length, const char * path, char * reason, size_t reasonSize) {
    if (!restore_validate_korg_dump(data, length, reason, reasonSize, NULL, NULL)) {
        show_info_dialogue("Restore Panel Failed", reason);
        return;
    }
    static uint8_t decoded[4096];
    uint32_t       decodedLen = 0;

    if (!synth_decode_korg_prog_dump(data, length, decoded, sizeof(decoded), &decodedLen)) {
        snprintf(reason, reasonSize, "Couldn't decode this file's Program Data Dump payload.");
        show_info_dialogue("Restore Panel Failed", reason);
        return;
    }
    tPanelConfig * cfg        = synth_panel_config();

    if (decodedLen < cfg->progNameLen) {
        snprintf(reason, reasonSize, "This file's decoded payload (%u bytes) is too short for this device's own program name field.", (unsigned)decodedLen);
        show_info_dialogue("Restore Panel Failed", reason);
        return;
    }
    uint32_t       sent       = 0;

    // Pause the background Korg name-sweep for the duration of the send
    // burst below — a real-hardware test found the sound/setup genuinely
    // wrong after restoring WHILE the sweep was mid-flight (owner: "I did
    // the load whilst fetching preset names, so there's a chance that
    // mechanisms might have clashed"). Exactly the same class of collision
    // synth_set_panel_dial_value()'s own synth_backup_sweep_request_in_
    // flight() check already guards against for a single dial drag — this
    // bulk send never went through that gate (it calls synth_send_
    // parameter_change() directly), so it never got that protection either.
    // Deliberately NOT resumed at the end: the sweep's own existing
    // auto-start logic (synth_backup_flush_background_prefetch()) picks it
    // back up on its own next idle trigger, restarting cleanly from
    // scratch — forcing an immediate resume here would just risk the exact
    // same collision again on the very next frame.
    gKorgSweepActive = false;

    // Program name first (params 1..progNameLen) — same group=/paramId=
    // convention the Korg-style branch of the app's own "type a new name"
    // feature already uses (synthComms.c), just replaying decoded bytes
    // instead of a freshly-typed string.
    for (uint32_t c = 0; c < cfg->progNameLen; c++) {
        synth_send_parameter_change(SYNTH_PARAM_GROUP_PROG, (uint16_t)(c + 1), (uint16_t)decoded[c]);
        sent++;
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, RESTORE_PANEL_PACE_SECONDS, false);
    }

    // Every other dial with both live (param=) and dump (dumpOffset=)
    // wiring — no per-parameter knowledge here, same "walk every section,
    // hidden or not" philosophy extract_prog_info() (synthComms.c) already
    // follows for the RECEIVE direction; this is its send-direction mirror.
    for (uint32_t s = 0; s < cfg->sectionCount; s++) {
        tPanelSection * section = &cfg->sections[s];

        for (uint32_t d = 0; d < section->dialCount; d++) {
            tPanelDial * dial      = &section->dials[d];

            if ((dial->paramId == 0) || (dial->dumpOffset < 0) || (decodedLen <= (uint32_t)dial->dumpOffset)) {
                continue;
            }
            uint32_t     raw       = (decoded[dial->dumpOffset] >> dial->dumpShift) & dial->dumpMask;
            uint16_t     wireValue = synth_korg_dump_raw_to_param_wire_value(dial, raw);

            synth_send_parameter_change((uint8_t)dial->paramGroup, (uint16_t)dial->paramId, wireValue);
            sent++;
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, RESTORE_PANEL_PACE_SECONDS, false);
        }
    }

    // Update the LOCAL GUI/dial state from the exact same decoded buffer
    // just replayed to the device, instead of leaving it stale until (or
    // unless) a later Sync from synth happens to refresh it — a real
    // hardware test found the GUI simply didn't change at all after a
    // restore, only the wire sends happened. Reuses the identical apply
    // logic a genuine incoming dump reply already goes through.
    synth_apply_korg_prog_dump_locally(decoded, decodedLen);
    gReDraw = true;

    // Belt-and-suspenders: also request a fresh dump from the device itself
    // once the burst is done, so any message that genuinely didn't land
    // (dropped, corrupted, real hardware couldn't keep up with the pacing)
    // gets caught and corrected by the SAME mechanism "Sync from synth"
    // already relies on, rather than trusting the local apply above alone
    // to be the last word on what the device actually ended up with.
    if (!synth_dump_patch_in_flight()) {
        synth_request_state_dump();
    }
    char message[128];

    snprintf(message, sizeof(message), "Sent %u Parameter Change message(s) — the connected device's live edit buffer should now match this file.", (unsigned)sent);
    LOG_DEBUG("Restore: Korg-style panel restore from %s sent %u Parameter Change message(s)\n", path, (unsigned)sent);
    show_info_dialogue("Restore Panel", message);
}

// Runs on the main thread once the user has chosen (or cancelled) a file to
// load into the live edit buffer — see synth_backup_restore_panel() below.
// Accepts EITHER a genuine Panel Dump (mode 0x02, "Backup > Current Panel"
// / "Save Panel to File…") or a Single Preset Dump (mode 0x03, "Backup >
// Patch by Number" or a Bank (Individual Files) export) — the latter is
// converted via convert_preset_dump_to_panel_dump() above before sending.
// Korg-style devices (Z1) branch off to restore_panel_korg_file() above
// instead — an entirely different mechanism (see its own comment) since
// there's no Korg equivalent of a Moog Panel Dump to just forward as-is.
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

    if (!synth_panel_config()->moogStyleDump) {
        restore_panel_korg_file(data, length, path, reason, sizeof(reason));
        free(data);
        return;
    }

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

void synth_backup_restore_panel_from_path(const char * path) {
    if (!gDevice.connected) {
        LOG_ERROR("Restore: no device connected\n");
        return;
    }
    restore_panel_file_chosen(path);
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
            // Keeps the name cache (gNameCacheValid) accurate for this slot
            // without needing a full re-sweep — see that flag's own comment.
            name_cache_update_from_preset_dump(presetNumber, data, length);
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

// Korg-style counterpart to restore_patch_file_chosen() above (Moog). See
// restore_validate_korg_dump()'s own comment for the header/address this
// reads — sending an addressed Program Data Dump straight to the device is
// UNCONFIRMED against real Z1 hardware as a way to write a specific bank/
// program (contrast synth_send_korg_program_write_request(), synthComms.h,
// the one Korg write mechanism that IS confirmed — committing the live
// edit buffer, not arbitrary addressed data). Standard convention across
// Korg's own product line for a message carrying its own destination
// address, and structurally consistent with how this app already reads the
// SAME message shape back (korg_decode_prog_dump(), synthComms.c) — but
// worth a real test with a low-stakes preset before trusting it the way
// the Moog mechanism (independently hardware-confirmed 2026-07-11) is
// trusted.
static void korg_restore_patch_file_chosen(const char * path) {
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
    uint8_t   bank   = 0;
    uint32_t  prog   = 0;

    if (!restore_validate_korg_dump(data, length, reason, sizeof(reason), &bank, &prog)) {
        show_info_dialogue("Restore Patch Failed", reason);
        free(data);
        return;
    }
    char      message[192];

    snprintf(message, sizeof(message),
             "This will overwrite Bank %c, Program %u on the connected device with the contents of this file. This cannot be undone.",
             bank ? 'B' : 'A', (unsigned)prog);

    if (show_confirm_dialogue("Restore Patch", message)) {
        if (midi_send(data, length)) {
            LOG_DEBUG("Restore: sent %u byte Program Data Dump from %s (overwrote Bank %c, Program %u)\n",
                      (unsigned)length, path, bank ? 'B' : 'A', (unsigned)prog);
            // Keeps the Korg name cache (gKorgNameCacheValid) accurate for
            // this slot without needing a full re-sweep — see
            // korg_name_cache_update_from_dump()'s own comment.
            korg_name_cache_update_from_dump(bank, prog, data, length);
            snprintf(message, sizeof(message), "Sent — Bank %c, Program %u should now match this file.", bank ? 'B' : 'A', (unsigned)prog);
            show_info_dialogue("Restore Patch", message);
        } else {
            LOG_ERROR("Restore: failed to send %u byte Program Data Dump from %s\n", (unsigned)length, path);
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

    if (synth_panel_config()->moogStyleDump) {
        open_file_read_dialogue_async(restore_patch_file_chosen);
    } else {
        open_file_read_dialogue_async(korg_restore_patch_file_chosen);
    }
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
            // Invalidates the WHOLE name cache rather than trying to update
            // it — a whole-bank dump is one opaque ~18KB blob with no
            // confirmed per-preset name offset to extract 128 individual
            // names from safely (see gNameCacheValid's own comment). The
            // next Load/Store Patch to Bank will just re-sweep.
            gNameCacheValid = false;
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

    backup_index_file_path(indexPath, sizeof(indexPath), folder);
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
                           "No index for the connected device found in this folder (or it has no entries) — pick a folder created by Backup > Bank (Individual Files) for this same device.");
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
        show_info_dialogue("Restore Folder Failed", "The index lists presets, but none of their files could be found in this folder.");
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

// synth_backup_restore_folder() itself lives further down, after the Korg
// restore-folder block (needs gKorgRestoreFolderActive/korg_restore_folder_chosen(),
// both declared there) — same forward-reference reasoning as
// synth_backup_bank_to_folder()'s own comment above.

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
                // Keeps the name cache (gNameCacheValid) accurate for this
                // slot without needing a full re-sweep — see that flag's
                // own comment. Only meaningful if a sweep has already run
                // this session (gNameCacheValid true) — name_cache_set_label()
                // is a no-op-safe write either way, so no extra guard needed
                // here; the NEXT Load/Store just won't see a cache at all
                // yet if this is the first bank-scale operation this
                // session, same as before this existed.
                name_cache_update_from_preset_dump(presetNumber, data, length);
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

// ── Korg restore-folder (Restore > Bank (Individual Files), in reverse) ────
// The Korg counterpart to the Moog restore-folder mechanism above — fully
// separate state (declared up near the rest of the Korg sweep's own state,
// not here, so synth_backup_start_name_sweep()'s Korg guard further up can
// reference it too — see gKorgRestoreFolderActive's own comment there),
// same reasoning as the rest of this file's Korg/Moog split (a fixed
// 128-slot single bank vs 256 across 2 banks, a different filename/index
// format). Same threading model too: a restore SEND has no reply to wait
// for, so this lives entirely on the main/render thread, driven once per
// frame by synth_backup_flush_korg_restore_folder() below.

// Parses <folder>/Patches.txt for the ordered list of (bank, program) slots
// it records (korg_sweep_append_index_line()'s own "A001  Name\n"/"B045  Name\n"
// format — bank letter, 3-digit zero-padded program, TWO spaces) — the Korg
// counterpart to restore_folder_parse_index() above (Moog). Ignores the
// recorded name text, just the leading address. Returns how many were
// found (capped at maxCount), as sweep-style indices (0..255, matching
// gKorgSweepLabels' own indexing).
static uint32_t korg_restore_folder_parse_index(const char * folder, uint32_t * outIndices, uint32_t maxCount) {
    char     indexPath[1280];

    backup_index_file_path(indexPath, sizeof(indexPath), folder);
    FILE *   f     = fopen(indexPath, "r");

    if (f == NULL) {
        return 0;
    }
    uint32_t count = 0;
    char     line[512];

    while ((count < maxCount) && (fgets(line, sizeof(line), f) != NULL)) {
        if (  ((line[0] == 'A') || (line[0] == 'B'))
           && isdigit((unsigned char)line[1]) && isdigit((unsigned char)line[2]) && isdigit((unsigned char)line[3])
           && (line[4] == ' ') && (line[5] == ' ')) {
            uint8_t  bank = (line[0] == 'B') ? 1 : 0;
            uint32_t prog = (uint32_t)((line[1] - '0') * 100 + (line[2] - '0') * 10 + (line[3] - '0'));

            if ((prog >= 1) && (prog <= 128)) {
                outIndices[count++] = (bank ? 128 : 0) + (prog - 1);
            }
        }
    }
    fclose(f);
    return count;
}

// Finds the file in `folder` whose name starts with the exact "A001"/"B045"
// style prefix (matching korg_sweep_write_capture_file()'s own naming) —
// the Korg counterpart to restore_folder_find_file() above (Moog).
// Deliberately NOT reconstructed from Patches.txt's own recorded name text,
// same "a renamed file still gets found" reasoning as that function's own
// comment. Writes the full path into outPath. Returns false if no matching
// file was found.
static bool korg_restore_folder_find_file(const char * folder, uint32_t index, char * outPath, size_t outPathSize) {
    DIR *           dp    = opendir(folder);

    if (dp == NULL) {
        return false;
    }
    uint8_t         bank  = (uint8_t)(index / 128);
    uint32_t        prog  = (index % 128) + 1;
    char            prefix[5];

    snprintf(prefix, sizeof(prefix), "%c%03u", bank ? 'B' : 'A', prog);
    bool            found = false;
    struct dirent * entry;

    while ((entry = readdir(dp)) != NULL) {
        if (  (strlen(entry->d_name) >= 5) && (strncmp(entry->d_name, prefix, 4) == 0)
           && ((entry->d_name[4] == ' ') || (entry->d_name[4] == '.'))) {
            snprintf(outPath, outPathSize, "%s/%s", folder, entry->d_name);
            found = true;
            break;
        }
    }
    closedir(dp);
    return found;
}

// Runs on the main thread once the user has chosen (or cancelled) a folder
// to restore from — the Korg counterpart to restore_folder_chosen() above
// (Moog). See synth_backup_restore_folder() below.
static void korg_restore_folder_chosen(const char * path) {
    if (path == NULL) {
        LOG_DEBUG("Restore: folder restore cancelled\n");
        return;
    }
    uint32_t indices[KORG_SWEEP_PRESET_COUNT];
    uint32_t indexCount = korg_restore_folder_parse_index(path, indices, KORG_SWEEP_PRESET_COUNT);

    if (indexCount == 0) {
        show_info_dialogue("Restore Folder Failed",
                           "No index for the connected device found in this folder (or it has no entries) — pick a folder created by Backup > Bank (Individual Files) for this same device.");
        return;
    }
    gKorgRestoreFolderCount                                        = 0;

    for (uint32_t i = 0; i < indexCount; i++) {
        char filePath[1280];

        if (korg_restore_folder_find_file(path, indices[i], filePath, sizeof(filePath))) {
            gKorgRestoreFolderEntries[gKorgRestoreFolderCount++] = indices[i];
        }
    }

    if (gKorgRestoreFolderCount == 0) {
        show_info_dialogue("Restore Folder Failed", "The index lists programs, but none of their files could be found in this folder.");
        return;
    }
    strncpy(gKorgRestoreFolderFolder, path, sizeof(gKorgRestoreFolderFolder) - 1);
    gKorgRestoreFolderFolder[sizeof(gKorgRestoreFolderFolder) - 1] = '\0';

    char message[256];

    snprintf(message, sizeof(message),
             "This will restore %u program(s) found in this folder, overwriting their exact matching slots on the connected device. This cannot be undone.",
             (unsigned)gKorgRestoreFolderCount);

    if (!show_confirm_dialogue("Restore Folder", message)) {
        LOG_DEBUG("Restore: folder restore cancelled at confirmation\n");
        return;
    }
    gKorgRestoreFolderIndex                                        = 0;
    gKorgRestoreFolderSentCount                                    = 0;
    gKorgRestoreFolderMissingCount                                 = indexCount - gKorgRestoreFolderCount; // entries listed but never found on disk
    gKorgRestoreFolderActive                                       = true;
    gKorgRestoreFolderNextSendMs                                   = backup_monotonic_ms();                // send the first one on the very next flush tick
    LOG_DEBUG("Restore: starting Korg folder restore of %u program(s) from %s\n", (unsigned)gKorgRestoreFolderCount, path);
}

void synth_backup_restore_folder(void) {
    if (!gDevice.connected) {
        LOG_ERROR("Restore: no device connected\n");
        return;
    }

    // Korg-style (Z1): fully separate folder-chosen/flush path — see this
    // whole block's own header comment for why. Added 2026-07-14.
    if (!synth_panel_config()->moogStyleDump) {
        if (gKorgRestoreFolderActive || (gBackupExpect != eBackupExpectNone)) {
            LOG_ERROR("Restore: another backup/restore operation is already in progress\n");
            return;
        }
        open_folder_choose_dialogue_async(korg_restore_folder_chosen, "Choose Folder to Restore From", get_last_backup_folder(synth_current_device_config()));
        return;
    }

    if (gRestoreFolderActive || gBackupBatchActive || (gBackupExpect != eBackupExpectNone)) {
        LOG_ERROR("Restore: another backup/restore operation is already in progress\n");
        return;
    }
    open_folder_choose_dialogue_async(restore_folder_chosen, "Choose Folder to Restore From", get_last_backup_folder(synth_current_device_config()));
}

// Per-frame poll — the Korg counterpart to synth_backup_flush_restore_folder()
// above (Moog). A no-op unless a Korg folder restore is actually active.
// Call once per frame from the render loop. Unlike that function's own
// end-of-sweep synth_request_state_dump() call, this doesn't need one —
// handle_prog_dump() never touches gDevice.progName/the live dials in the
// first place (see korg_sweep_show_picker()'s own comment above), so
// there's nothing stale to refresh once the sweep's done.
void synth_backup_flush_korg_restore_folder(void) {
    if (!gKorgRestoreFolderActive) {
        return;
    }

    if (backup_monotonic_ms() < gKorgRestoreFolderNextSendMs) {
        return; // still pacing since the last send
    }
    gReDraw = true;
    uint32_t index = gKorgRestoreFolderEntries[gKorgRestoreFolderIndex];
    uint8_t  bank  = (uint8_t)(index / 128);
    uint32_t prog  = (index % 128) + 1;
    char     filePath[1280];

    if (korg_restore_folder_find_file(gKorgRestoreFolderFolder, index, filePath, sizeof(filePath))) {
        uint32_t  length = 0;
        uint8_t * data   = restore_read_file(filePath, &length);

        if (data != NULL) {
            char reason[192];

            if (restore_validate_korg_dump(data, length, reason, sizeof(reason), NULL, NULL) && midi_send(data, length)) {
                gKorgRestoreFolderSentCount++;
                korg_name_cache_update_from_dump(bank, prog, data, length);
                LOG_DEBUG("Restore: sent Bank %c, Program %u from %s (%u/%u)\n", bank ? 'B' : 'A', (unsigned)prog, filePath,
                          (unsigned)(gKorgRestoreFolderIndex + 1), (unsigned)gKorgRestoreFolderCount);
            } else {
                gKorgRestoreFolderMissingCount++;
                LOG_ERROR("Restore: failed to send Bank %c, Program %u from %s (%s)\n", bank ? 'B' : 'A', (unsigned)prog, filePath, reason);
            }
            free(data);
        } else {
            gKorgRestoreFolderMissingCount++;
        }
    } else {
        gKorgRestoreFolderMissingCount++; // file listed a moment ago at korg_restore_folder_chosen() time but gone now — race with something else touching the folder mid-sweep
    }
    gKorgRestoreFolderIndex++;

    if (gKorgRestoreFolderIndex >= gKorgRestoreFolderCount) {
        gKorgRestoreFolderActive = false;
        char summary[192];
        snprintf(summary, sizeof(summary), "Restored %u program(s), %u missing/failed.",
                 (unsigned)gKorgRestoreFolderSentCount, (unsigned)gKorgRestoreFolderMissingCount);
        show_info_dialogue("Restore Folder", summary);
        LOG_DEBUG("Restore: Korg folder restore finished — %u sent, %u missing/failed\n",
                  (unsigned)gKorgRestoreFolderSentCount, (unsigned)gKorgRestoreFolderMissingCount);
        return;
    }
    gKorgRestoreFolderNextSendMs = backup_monotonic_ms() + RESTORE_FOLDER_SEND_PACING_MS;
}

bool synth_backup_get_export_progress(uint32_t * outCurrent, uint32_t * outTotal, uint32_t * outActionCount) {
    // Korg name sweep checked first — a fully separate state machine (see
    // its own header comment above) that shares this same progress-overlay
    // reporting function purely for UI reuse, not because it shares any
    // state with gBackupBatchActive below. Added 2026-07-14.
    if (gKorgSweepActive) {
        *outCurrent     = gKorgSweepIndex + 1; // 0-based index -> 1-based "Nth of M"
        *outTotal       = KORG_SWEEP_PRESET_COUNT;
        *outActionCount = gKorgSweepRepliedCount;
        return true;
    }

    if (!gBackupBatchActive) {
        return false;
    }
    *outCurrent     = gBackupBatchCurrentPreset;
    *outTotal       = BACKUP_BATCH_PRESET_COUNT;
    *outActionCount = gBackupBatchRepliedCount;
    return true;
}

bool synth_backup_export_progress_is_name_sweep(void) {
    // gKorgSweepActive alone isn't enough any more — that same flag also
    // covers a real Backup > Bank (Individual Files) export now (export
    // mode reuses this exact sweep, see its own header comment), which
    // deserves the blocking progress modal same as the Moog export path
    // below, not the quiet background-fill status row a plain name sweep
    // gets. Found while adding Korg export mode, 2026-07-14 — before this
    // fix, an in-progress Korg bank export would have been misreported as
    // a name sweep here.
    return (gKorgSweepActive && (gKorgSweepMode == eKorgSweepModeNameSweep))
           || (gBackupBatchActive && (gBackupBatchMode == eBatchModeNameSweep));
}

bool synth_backup_get_restore_progress(uint32_t * outCurrent, uint32_t * outTotal, uint32_t * outActionCount) {
    // Korg restore-folder checked first — a fully separate state machine
    // (see its own header comment above) that shares this same progress-
    // overlay reporting function purely for UI reuse, same pattern
    // synth_backup_get_export_progress() already uses for the Korg sweep.
    if (gKorgRestoreFolderActive) {
        *outCurrent     = gKorgRestoreFolderIndex + 1; // 0-based index -> 1-based "Nth of M"
        *outTotal       = gKorgRestoreFolderCount;
        *outActionCount = gKorgRestoreFolderSentCount;
        return true;
    }

    if (!gRestoreFolderActive) {
        return false;
    }
    *outCurrent     = gRestoreFolderIndex + 1; // 0-based index -> 1-based "Nth of M"
    *outTotal       = gRestoreFolderCount;
    *outActionCount = gRestoreFolderSentCount;
    return true;
}

// How long a connection has to stay eligible (connected, no cache yet,
// nothing else in flight) before the background name-prefetch sweep
// silently starts. NOT an inactivity timer — an earlier version of this
// gated on mouse/keyboard idle time (mouse_idle_ms(), mouseHandle.c), but
// that meant a completely normal few-second pause between dial tweaks
// triggered a sweep mid-session (2026-07-14 user report: "when I start
// tweaking dials, names are being polled, so it's not really an
// inactivity mechanism as-is"). Dropped that entirely: the sweep is now
// always paced slowly enough (NAME_SWEEP_PACING_MS above) to stay out of
// the way of normal interactive use regardless of when it runs, so there's
// no need to wait for genuine inactivity at all — this settle delay just
// lets connect-time traffic (the initial state dump, etc.) clear first.
#define BACKGROUND_PREFETCH_SETTLE_MS    2000.0

static double gBackgroundPrefetchEligibleSinceMs = 0.0;

// Silently starts a name sweep (Korg or Moog, whichever the connected
// device uses) once it's stayed eligible for a moment (see
// BACKGROUND_PREFETCH_SETTLE_MS above), so by the time the user actually
// clicks "Load/Store Patch from Bank…" some (or all, if they took a while
// to get there) of the picker is already populated instead of showing
// "---" everywhere. Common to every device family as of 2026-07-14 (owner:
// "these should be common mechanisms with Voyager and any other device...
// should be common and generic") — Voyager included, DESPITE the following
// known risk, which the owner explicitly accepted rather than have this
// stay Korg-only: an earlier idle-poll mechanism for the Voyager was built
// and then fully reverted (see project_voyager_restore_mechanism /
// project_voyager_extlevel_and_precision memory notes) because ANY
// unsolicited dump request kicks the Voyager's front panel out of whatever
// menu it's showing, and a Single Preset Dump Request (what this sweep
// uses on a Moog-style device) is exactly that kind of request — watch the
// Voyager's own front-panel display the first few times this fires. One-
// shot in practice: gNameCacheValid/gKorgNameCacheValid latches true on
// completion and this codebase has no reconnect/disconnect hook that
// clears it back to false (only Restore Bank explicitly invalidates the
// Moog one, see synth_backup_restore_bank_chosen()'s own comment above) —
// not something newly introduced here. Call once per frame from the render
// loop, alongside the other flush functions.
void synth_backup_flush_background_prefetch(void) {
    if (!gDevice.connected) {
        gBackgroundPrefetchEligibleSinceMs = 0.0;
        return;
    }
    bool moog = synth_panel_config()->moogStyleDump;

    // A Korg-style device that doesn't speak Z1's specific Program Data
    // Dump Request protocol (supportsKorgProgramDump == false — Kronos,
    // whose own SysEx protocol is entirely different, see
    // KRONOS_MIDI_Implementation's own field comment in panelConfig.h) has
    // nothing this whole sweep mechanism could ever fetch — every request
    // it would send times out forever. Skip entirely rather than spin.
    if (!moog && !synth_panel_config()->supportsKorgProgramDump) {
        return;
    }

    if (moog ? gNameCacheValid : gKorgNameCacheValid) {
        return;
    }

    // Already running (a sweep this function itself started earlier, an
    // explicit Load/Store click's own sweep, or — Moog only — someone
    // mid-export) — nothing to (re)start either way.
    if (moog ? gBackupBatchActive : gKorgSweepActive) {
        return;
    }
    // Mirrors synth_backup_start_name_sweep()'s own "nothing else in
    // flight" guard — a background prefetch must never contend with an
    // explicit user action already using the same reply-capture state.
    // Deliberately does NOT exclude eBackupExpectKorgProgram/
    // eBackupExpectPreset — those are this very sweep's OWN in-flight
    // request once it's running (already excluded above via
    // gBackupBatchActive/gKorgSweepActive being false at this point), not a
    // conflict from something else.
    bool blocked = (moog ? gRestoreFolderActive : gKorgRestoreFolderActive)
                   || (moog ? (gBackupExpect != eBackupExpectNone)
                           : ((gBackupExpect != eBackupExpectNone) && (gBackupExpect != eBackupExpectKorgProgram)));

    if (blocked) {
        gBackgroundPrefetchEligibleSinceMs = 0.0; // reset — start counting again once whatever's running finishes
        return;
    }

    if (gBackgroundPrefetchEligibleSinceMs == 0.0) {
        gBackgroundPrefetchEligibleSinceMs = backup_monotonic_ms();
        return;
    }

    if ((backup_monotonic_ms() - gBackgroundPrefetchEligibleSinceMs) < BACKGROUND_PREFETCH_SETTLE_MS) {
        return;
    }

    if (moog) {
        LOG_DEBUG("Load/Store: starting background name prefetch (Moog-style)\n");
        moog_name_sweep_start();
    } else {
        LOG_DEBUG("Load/Store: starting background name prefetch (Korg-style)\n");
        korg_sweep_start();
    }
}
