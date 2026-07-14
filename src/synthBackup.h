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

#include <stdbool.h>
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
    eBackupExpectLive,        // live edit buffer — Voyager Panel Dump or Korg Current Program Dump
    eBackupExpectPreset,      // a specific stored preset, by number — Voyager Single Preset Dump only
    eBackupExpectBank,        // every stored preset in one message — Voyager All Presets Dump only
    // A specific stored Program, by bank+number — Korg-style PROGRAM DATA
    // DUMP (func 0x4C) reply to a PROGRAM DATA DUMP REQUEST (func 0x1C).
    // Added 2026-07-14 for the Z1 (2 banks x 128 programs) — see
    // synth_backup_start_name_sweep()'s own comment for how this feeds the
    // Load/Store picker. Distinct from eBackupExpectPreset (Moog-only,
    // single implicit bank) since the two protocols' wire shapes and
    // decode paths are entirely different.
    eBackupExpectKorgProgram,
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

// Per-frame poll for an in-progress Korg-style name sweep (Z1: 2 banks x
// 128 programs) — the Korg counterpart to synth_backup_flush_bank_to_folder()
// above, kept fully separate (see that sweep's own header comment in
// synthBackup.c for why). Paced (KORG_SWEEP_PACING_MS, synthBackup.c), not
// as-fast-as-possible — the picker no longer waits on this sweep at all
// (see korg_sweep_show_picker()'s own comment), so there's no reason to
// hurry. A no-op unless a Korg name sweep is actually active. Call once per
// frame from the render loop, alongside synth_backup_flush_bank_to_folder().
// Added 2026-07-14.
void synth_backup_flush_korg_name_sweep(void);

// True only while a single name-sweep request (Korg OR Moog, whichever the
// connected device uses) is awaiting its reply/timeout — NOT during the
// slower paced gap between requests, and NOT during a Moog bank-to-folder
// EXPORT (out of scope, see this function's own comment in synthBackup.c
// for why). synthComms.c's synth_set_panel_dial_value() checks this to
// defer an outgoing CC or Parameter Change rather than sending it into the
// same narrow window a sweep reply is expected in (2026-07-14 owner report
// of spurious "(no response)" entries traced to exactly this collision on
// the Z1, then generalized to also cover Voyager: "these should be common
// mechanisms with Voyager and any other device").
bool synth_backup_sweep_request_in_flight(void);

// Per-frame poll that silently STARTS a name sweep (Korg or Moog, whichever
// the connected device uses) once it's stayed eligible for a moment —
// pre-fetches program names in the background so a later explicit "Load/
// Store Patch from Bank…" click finds the picker already partly (or fully)
// populated instead of all "---". NOT gated on user inactivity — an earlier
// version was, but that meant a normal pause between dial tweaks fired the
// sweep mid-session; see this function's own comment in synthBackup.c for
// why that was dropped in favour of always-slow pacing instead. Common to
// every device family (including Voyager, as of 2026-07-14 — see this
// function's own comment in synthBackup.c for the known accepted risk that
// choice carries), one-shot per connection (gNameCacheValid/
// gKorgNameCacheValid latches true on completion), a no-op whenever any
// other backup/sweep/restore operation is already in flight. Call once per
// frame from the render loop, alongside the other flush functions. Added
// 2026-07-14.
void synth_backup_flush_background_prefetch(void);

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

// Triggered by "Restore > Bank (Individual Files)…" — restores an entire
// Backup > Bank (Individual Files) export back to the connected device,
// one preset at a time (Single Preset Dump can only address one preset per
// send — same constraint as the export itself, see
// synth_backup_bank_to_folder()'s own comment above). Driven by the
// chosen folder's own Patches.txt index (which preset numbers it lists, in
// order) rather than just scanning for numbered files — a listed number
// with no matching file in the folder is skipped and counted as missing.
// Shows a confirmation naming how many presets will be overwritten before
// sending anything, then a summary once finished. A no-op if no device is
// connected, it isn't Moog-style, or another backup/restore operation is
// already in progress.
void synth_backup_restore_folder(void);

// Per-frame poll for an in-progress synth_backup_restore_folder() sweep —
// paces sends (RESTORE_FOLDER_SEND_PACING_MS apart, synthBackup.c) rather
// than firing them all at once, giving the device time to actually write
// each preset before the next arrives. Unlike synth_backup_flush_bank_to_folder()
// above, this never touches the CoreMIDI thread — a restore send has no
// reply to wait for, so the whole sweep lives on the main/render thread
// this is called from. A no-op if no sweep is in progress. Call once per
// frame from the render loop, alongside synth_backup_flush_bank_to_folder().
void synth_backup_flush_restore_folder(void);

// ── Load/Store (explicit bank-location transfer, G2-Edit naming) ────────────
// G2-Edit's File menu has "Load Patch from Bank…" (device → edit buffer, by
// number) and "Store Patch to Bank…" (edit buffer → device, by number) —
// added here 2026-07-11 at the owner's explicit request to follow that same
// naming/placement convention, distinct from both Open/Save (disk files) and
// Restore/Backup by Number (which round-trip through an actual .syx file on
// disk rather than acting directly against a chosen device location).

// Which action to take once synth_backup_start_name_sweep() below finishes
// fetching every preset's name and shows the resulting picker.
typedef enum {
    eNameSweepPurposeLoad = 0,
    eNameSweepPurposeStore,
} tNameSweepPurpose;

// Triggered by "File > Load Patch from Bank…" / "File > Store Patch to
// Bank…" (misc.mm) — rather than a bare 1-128 number picker (Backup Patch by
// Number's own, simpler style), these fetch every preset's NAME first (owner
// request, 2026-07-11: "we need to display the patch names in those slots")
// so the picker actually reads like a patch list. Sequentially requests
// preset 1..128 (Single Preset Dump can only address one at a time, same
// constraint synth_backup_bank_to_folder() already works around) and decodes
// each reply's name via synth_decode_moog_name() (synthComms.c) into an
// in-memory label — no files touched. Reuses synth_backup_bank_to_folder()'s
// OWN sequencing mechanism under the hood (same gBackupBatchActive state,
// same per-preset timeout, same CoreMIDI-thread-copies/main-thread-sequences
// split) via a mode flag, so synth_backup_flush_bank_to_folder() (already
// called once per frame from the render loop) drives this too — no separate
// flush function needed. Once the sweep finishes (or every remaining slot
// times out), shows show_device_choice_dialogue() with "N: Name" entries and
// acts on the chosen one: eNameSweepPurposeLoad calls
// synth_load_patch_from_bank(), eNameSweepPurposeStore calls
// synth_store_patch_to_bank() — for Store, the picker defaults to
// gDevice.currentProgram + 1 (the slot the CURRENT panel was originally
// loaded from), a second 2026-07-11 owner request ("default to write to the
// slot... the one we're working on came from"). A no-op if no device is
// connected, it isn't Moog-style, or another backup/restore/sweep operation
// is already in progress.
void synth_backup_start_name_sweep(tNameSweepPurpose purpose);

// Updates the name cache above for ONE preset — called from
// handle_moog_single_preset_dump() (synthComms.c) for EVERY incoming Single
// Preset Dump reply, regardless of why it was requested (Backup > Patch by
// Number, a future patch browser, anything at all) — 2026-07-11 owner
// observation: "the gap will be closed if we have to read the patch in
// question from the synth for any reason." presetNumber is 1-based
// (1-128); name is whatever was just decoded for that reply (already
// collapses any embedded '\n' the same way the sweep's own labels do). A
// no-op if presetNumber is out of range — safe to call unconditionally
// from a generic reply handler that doesn't know or care whether a name
// sweep has ever run this session.
void synth_backup_note_preset_name(uint32_t presetNumber, const char * name);

// "File > Load Patch from Bank…" is now declared in synthComms.h (see
// synth_load_patch_from_bank() there) — was duplicated here until
// 2026-07-14, when the Korg-style bank parameter was added; kept the
// synthComms.h copy since that's where the implementation actually lives
// (synthComms.c), not here.

// Triggered by "File > Store Patch to Bank…" — commits the CURRENT live
// panel to a chosen stored preset location, OVERWRITING whatever is there.
// Shows a confirmation before doing anything. Mechanism is entirely
// protocol-dependent (branches on cfg->moogStyleDump):
//   - Moog-style (Voyager): request a FRESH Panel Dump (not a possibly-
//     stale cache) so the exact current edit-buffer state is what gets
//     stored, convert it to Single Preset Dump shape addressed to
//     presetNumber (the inverse of Restore's own preset-dump -> panel-dump
//     conversion, synthBackup.c), and send — the same "SEND PRESET(S)"
//     mechanism Restore > Patch by Number already proved works on real
//     hardware, just sourced from a live fetch instead of a file.
//   - Korg-style (Z1): a single PROGRAM WRITE REQUEST (func 0x11,
//     synth_send_korg_program_write_request(), synthComms.c) — no local
//     fetch/convert step at all; the device commits whatever's currently
//     in its OWN live edit buffer. Added 2026-07-14.
// presetNumber is 1-based (1-128). bank is Korg-style only (0=A, 1=B) —
// ignored for Moog-style, where presetNumber alone already fully addresses
// a location. A no-op if no device is connected, the number is out of
// range, or the user cancels the confirmation.
void synth_store_patch_to_bank(uint8_t bank, uint32_t presetNumber);

// Per-frame poll for an in-progress synth_store_patch_to_bank() fetch — the
// fresh Panel Dump reply lands on the CoreMIDI thread
// (synth_backup_capture_dump()), which only copies the bytes and publishes
// them; the actual convert+send+result-dialog work happens here instead,
// once per frame from the render loop, because show_confirm_dialogue()/
// show_info_dialogue() (fileDialogue.mm) both require the main thread. A
// no-op unless a Store fetch's reply has actually arrived.
void synth_backup_flush_store(void);

// ── Progress reporting for the two bulk sweeps above ─────────────────────────
// Read by synth_render() (synthGraphics.cpp) each frame to draw a progress
// overlay — same idea as G2-Edit's own render_bank_backup_progress()/
// render_bank_restore_progress() (graphics.cpp there), collapsed into ONE
// overlay in SynthEdit's own render code since only one of these sweeps can
// ever be active at a time (synth_backup_bank_to_folder()/
// synth_backup_restore_folder() both guard against starting while the
// other is already running). Each returns false (leaves the out params
// untouched) if that particular sweep isn't currently active.
// outCurrent/outTotal: 1-based "current of total" progress. outActionCount:
// how many have actually succeeded so far (written for export, sent for
// restore) — can lag behind outCurrent, since a missing/failed entry still
// advances outCurrent without incrementing this.
bool synth_backup_get_export_progress(uint32_t * outCurrent, uint32_t * outTotal, uint32_t * outActionCount);
bool synth_backup_get_restore_progress(uint32_t * outCurrent, uint32_t * outTotal, uint32_t * outActionCount);

// True while the CURRENTLY active synth_backup_get_export_progress() sweep
// (if any) is actually a synth_backup_start_name_sweep() run rather than a
// real synth_backup_bank_to_folder() export — both share the same
// underlying progress state (gBackupBatchActive et al.), so
// synth_render_backup_progress() (synthGraphics.cpp) needs this to show an
// accurate title ("Fetching Preset Names" vs "Backing Up Bank") instead of
// always assuming an export is running.
bool synth_backup_export_progress_is_name_sweep(void);

#ifdef __cplusplus
}
#endif

#endif // __SYNTH_BACKUP_H__
