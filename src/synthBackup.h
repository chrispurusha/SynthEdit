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

// Single-patch backup: capture whatever the connected device reports —
// either its live edit buffer (Voyager's Panel Dump, or a Korg-style
// device's Current Program Dump) or a specific STORED preset by number
// (Voyager's Single Preset Dump) — and save the exact bytes off the wire to
// a file the user picks, verbatim, as a generic .syx SysEx capture.
// Deliberately opaque — this file has no idea what any of those bytes mean,
// same spirit as G2-Edit's bank backup treating patch content as an opaque
// blob rather than re-serializing it.
//
// No restore yet, and no whole-bank iteration — see the "how much work"
// scoping discussion this followed. This is the first slice: prove the
// request/capture/save round trip on one device (Voyager) before building
// out request/reply timeout handling for iterating many patches.

#ifndef __SYNTH_BACKUP_H__
#define __SYNTH_BACKUP_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Which reply synth_backup_capture_dump() is currently waiting for — lets it
// tell an unrelated dump (e.g. a stray live-panel reply arriving while a
// by-number capture is pending) apart from the one that was actually
// requested, rather than just "is *a* backup pending at all".
typedef enum {
    eBackupExpectNone = 0,
    eBackupExpectLive,   // live edit buffer — Voyager Panel Dump or Korg Current Program Dump
    eBackupExpectPreset, // a specific stored preset, by number — Voyager Single Preset Dump only
    eBackupExpectBank,   // every stored preset in one message — Voyager All Presets Dump only
} tBackupExpect;

// Triggered by the "Backup > Current Patch..." menu action (misc.mm). Arms
// the capture and sends a fresh state-dump request; the save dialog opens
// once the reply arrives (see synth_backup_capture_dump() below) — this
// function itself returns immediately, it does not block waiting for a
// reply. A no-op (logs and returns) if no device is connected.
void synth_backup_current_patch(void);

// Triggered by the "Backup > Patch by Number..." menu action (misc.mm).
// presetNumber is 1-based (1-128 on a base Voyager with no memory
// expansion — see synth_request_single_preset_dump() in synthComms.c for
// the wire format and its caveats). Same fire-and-forget shape as
// synth_backup_current_patch() above; a no-op if no device is connected or
// it isn't Moog-style (this request has no Korg-style equivalent yet).
void synth_backup_patch_by_number(uint32_t presetNumber);

// Triggered by the "Backup > Bank..." menu action (misc.mm) — every stored
// preset in one file (see synth_request_all_presets_dump() in synthComms.c
// for the request itself, now CONFIRMED against real hardware). Same
// fire-and-forget shape as the two above; a no-op if no device is connected
// or it isn't Moog-style. Only covers whatever bank the connected unit's own
// front panel currently has selected — see the field comment on
// tPanelConfig.presetBankCount (panelConfig.h) for why a unit with a memory
// expansion (more than one bank) needs repeating per-bank, and why this app
// doesn't yet know how to select a non-default bank itself.
void synth_backup_bank(void);

// Triggered by the "Backup > Bank (Individual Files)…" menu action
// (misc.mm) — requests every stored preset ONE AT A TIME (1..128; Single
// Preset Dump can only address one preset per request, unlike
// synth_backup_bank()'s single All Presets Dump message above) and saves
// each as its own .syx file in a folder the user picks, plus a "Patches.txt"
// index listing every preset number + decoded name once the whole sweep
// finishes — the file-per-patch-plus-list-file shape G2-Edit's own patch
// browser uses, adapted here since Moog's wire format has nothing like
// G2's SUB_COMMAND_LIST_NAMES sweep to lean on (no batch name-only reply —
// see synth_request_single_preset_dump()'s own comment, synthComms.h).
// Fire-and-forget (opens the folder picker and returns) — the sweep itself
// is driven by synth_backup_flush_bank_to_folder() below, called once per
// frame from the render loop, plus synth_backup_capture_dump() advancing it
// on each reply. A no-op if no device is connected, the connected device
// isn't Moog-style, or another backup operation (of any kind) is already in
// progress.
void synth_backup_bank_to_folder(void);

// Per-frame poll for an in-progress synth_backup_bank_to_folder() sweep —
// advances to the next preset once BACKUP_BATCH_TIMEOUT_MS has passed with
// no reply for the current one (an unresponsive/unpopulated location
// shouldn't hang the whole export), same per-frame-check idiom as
// synth_flush_pending_dump_sends() (synthComms.h) uses for a dial's own
// pending send. A no-op if no sweep is in progress. Call once per frame
// from the render loop.
void synth_backup_flush_bank_to_folder(void);

// Called from synthComms.c's dump handlers (handle_moog_panel_dump(),
// handle_curr_prog_dump(), handle_moog_single_preset_dump(),
// handle_moog_all_presets_dump()) with the complete raw SysEx message
// exactly as received off the wire (including the F0/F7 framing), before
// any decoding, tagged with which of those four it is. A no-op unless
// `kind` matches what synth_backup_current_patch()/
// synth_backup_patch_by_number()/synth_backup_bank() is currently waiting
// for — every other incoming dump (e.g. the one connect_without_identity()
// already requests at startup) passes through untouched.
void synth_backup_capture_dump(const uint8_t * data, uint32_t length, tBackupExpect kind);

// ── Restore ───────────────────────────────────────────────────────────────────
// CONFIRMED against real hardware 2026-07-11: sending a captured dump back
// to the device is the whole mechanism — no separate "store" SysEx exists.
// A Panel Dump only ever loads the live edit buffer (no overwrite risk,
// proven earlier — see [[project_voyager_dial_menu_send]] in the
// assistant's own memory notes). A Single Preset Dump OVERWRITES the exact
// stored slot its own embedded preset-number byte names — verified via a
// safe modify-then-restore round trip on a real Voyager (see
// [[project_voyager_restore_mechanism]]). All three functions below open a
// file picker, validate the file actually is the expected dump type/device
// before doing anything with it, and — for the two that overwrite stored
// memory — show an explicit confirmation naming exactly what will be
// overwritten before sending. Each is a no-op (logs and returns) if no
// device is connected or it isn't Moog-style — none of this has a
// Korg-style equivalent yet.

// Triggered by "File > Open Panel File…" — loads a previously-saved dump
// into the live edit buffer, same as physically turning every knob to
// match. Accepts EITHER a genuine Panel Dump (mode 0x02 — "Save Panel to
// File…"/"Backup > Current Panel") or a Single Preset Dump (mode 0x03 —
// "Backup > Patch by Number" or a Bank (Individual Files) export),
// converting the latter to the former first (synthBackup.c) — added
// 2026-07-11 so any backed-up patch can be auditioned this way, not just
// ones saved specifically as a Panel Dump. Does NOT touch any stored
// preset either way. No confirmation prompt (nothing to lose — the live
// buffer is exactly what Sync/preset navigation already overwrite freely).
void synth_backup_restore_panel(void);

// Triggered by "Backup > Restore > Patch by Number…" — loads a previously-
// saved Single Preset Dump and sends it back verbatim, OVERWRITING the
// exact preset slot number embedded in the file itself (not something the
// user picks — see this file's own comment on how that's decoded). Shows a
// confirmation naming that slot number before sending; a no-op if the
// chosen file doesn't decode as a valid Single Preset Dump for the
// connected device.
void synth_backup_restore_patch(void);

// Triggered by "Backup > Restore > Bank…" — loads a previously-saved whole-
// bank dump (synth_backup_bank()'s own output) and sends it back verbatim,
// OVERWRITING ALL 128 presets in the current bank at once. Shows a loud
// confirmation before sending (this is the single most destructive action
// in the whole app — not individually hardware-tested yet, only inferred
// from the same "SEND PRESET(S)" mechanism the single-preset case proved,
// per the owner's manual describing both under one mechanism).
void synth_backup_restore_bank(void);

#ifdef __cplusplus
}
#endif

#endif // __SYNTH_BACKUP_H__
