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

// Called from synthComms.c's dump handlers (handle_moog_panel_dump(),
// handle_curr_prog_dump(), handle_moog_single_preset_dump()) with the
// complete raw SysEx message exactly as received off the wire (including the
// F0/F7 framing), before any decoding, tagged with which of those three it
// is. A no-op unless `kind` matches what synth_backup_current_patch()/
// synth_backup_patch_by_number() is currently waiting for — every other
// incoming dump (e.g. the one connect_without_identity() already requests at
// startup) passes through untouched.
void synth_backup_capture_dump(const uint8_t * data, uint32_t length, tBackupExpect kind);

#ifdef __cplusplus
}
#endif

#endif // __SYNTH_BACKUP_H__
