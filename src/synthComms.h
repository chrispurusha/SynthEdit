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

#ifndef __SYNTH_COMMS_H__
#define __SYNTH_COMMS_H__

#include <stdint.h>

#include "panelConfig.h"

#ifdef __cplusplus
extern "C" {
#endif

// Called when a synth is identified on the MIDI bus
void synth_on_connected(void);

// Dispatch an incoming synth SysEx message (full message including F0 header)
void synth_handle_message(const uint8_t * data, uint32_t length);

// Applies an incoming real-time MIDI CC value to whichever dial (in any
// section) has a matching ccNumber= in the device's <device>.txt — generic
// over any device's CC assignments, not a fixed per-device CC list. Returns
// true if a dial was found and updated (so the caller knows whether to
// redraw), false as a harmless no-op otherwise.
bool synth_handle_cc(uint8_t cc, uint8_t value);

// Commits any quantized switch/selector dial's debounced CC value once it's
// been quiet for CC_DEBOUNCE_MS — see hasPendingCc's own comment in
// panelConfig.h. Call once per frame from the render loop.
void synth_flush_pending_cc(void);

// Sends any dump-only dial's debounced patch-and-resend once it's been quiet
// for CC_DEBOUNCE_MS — see hasPendingDumpSend's own comment in panelConfig.h.
// Call once per frame from the render loop, alongside synth_flush_pending_cc().
void synth_flush_pending_dump_sends(void);

// Request the currently loaded program from the synth
void synth_request_current_program(void);

// Re-sends whichever "report your current state" request the connected
// device actually understands — the file-declared stateRequestSysEx if one
// exists (e.g. Moog's Panel Dump Request), else the generic Korg-style
// Current Program Dump Request. Same request connect_without_identity()
// sends once at connect time (midiComms.c), callable again on demand — e.g.
// so Backup can capture a fresh dump rather than replaying a stale cached
// one.
void synth_request_state_dump(void);

// True while a Panel Dump Request is outstanding SPECIFICALLY to merge in a
// pending dump-only dial change or program-name edit (gAwaitingFreshDumpForPatch,
// synthComms.c) — a caller wanting "get me a fresh dump" for its own separate
// reason (e.g. the manual Sync button) should check this first and skip
// firing its own request when true, rather than sending a redundant second
// one: the outstanding request's reply will deliver fresh data to every
// consumer once it lands (extract_moog_panel_info() decodes into the shared
// dial/gDevice state regardless of who asked), so a second simultaneous
// request buys nothing and risks racing the first reply's own
// synth_apply_pending_dump_patches() — a second, later reply can land AFTER
// that call has already cleared the dial's dumpSendAwaitingFreshData flag,
// get decoded normally, and silently overwrite the just-applied value back
// to its pre-change state on screen (found 2026-07-11: dragging Headphone
// Volume then immediately pressing "Sync from synth" reverted the display,
// because the Sync button's own synth_request_state_dump() call had no such
// guard).
bool synth_dump_patch_in_flight(void);

// Moog-style devices only (cfg->moogStyleDump — see panelConfig.h): requests
// a specific STORED preset by number, as opposed to synth_request_state_dump()
// above, which only ever reads the live edit buffer (Voyager's Panel Dump).
// presetNumber is 1-based, matching the manual's own "locations are numbered
// 1 to 128" convention; sent on the wire as presetNumber-1 (0-127) — a
// best-guess at MIDI's usual 0-indexed-on-the-wire/1-indexed-on-the-panel
// convention, UNCONFIRMED against real hardware (unlike the Panel Dump
// Request this is built from — see voyager.txt's stateRequestSysEx comment).
// A no-op with a logged error if the connected device isn't Moog-style, or
// if presetNumber is out of the current bank's 128-location range (the
// wire format's single program-number byte can't address more than that
// without a separate bank-select step this doesn't implement — fine for a
// base Voyager with no VX-... memory expansion, per voyager.txt).
void synth_request_single_preset_dump(uint32_t presetNumber);

// Moog-style devices only: requests every stored preset in one message
// (mode 0x04, "All Presets Dump REQUEST" — see voyager.txt's header
// comment), for Backup > Bank. CONFIRMED against real hardware (2026-07-07):
// a captured reply was a single well-formed 18734-byte message (exactly one
// F0...F7, header F0 04 01 00 01 exactly as expected), and preset 1 and
// preset 3's names ("FILTER BUBBLES", "TIME FOR SURFIN'") both decode
// correctly at byte offsets 100 and 396 respectively within it — the same
// name-field shape a Panel Dump uses, just repeated once per preset with no
// framing in between. Per-preset byte stride isn't perfectly uniform
// though (148 bytes between presets 1 and 3, but presets 2/4/5's data
// didn't decode as cleanly at the naive halfway/proportional points) —
// unconfirmed whether that's real per-preset size variation or just this
// analysis not yet finding the right alignment; either way, not something
// Backup > Bank needs solved, since it saves the whole reply as one opaque
// blob rather than splitting it into individual presets. A no-op with a
// logged error if the connected device isn't Moog-style.
void synth_request_all_presets_dump(void);

// Prev/Next patch buttons (see synth_hit_test_patch_nav() in
// synthGraphics.h): sends a Program Change of gDevice.currentProgram+delta
// (clamped to MIDI's 0-127 range, defaulting the base to 0 if
// currentProgram is still unknown rather than refusing to send anything)
// and optimistically records that as the new currentProgram — there's no
// reply that confirms the device actually switched, so this can drift from
// reality if e.g. the target program number doesn't exist on the device.
// delta is typically +-1; a no-op only if the device isn't connected at
// all. Requests a fresh state dump right after sending, same as
// dispatch_program_change() does for a Program Change arriving from
// elsewhere on the bus (midiComms.c) — refreshes both the dial positions
// and gDevice.progName from whatever the device now has loaded.
void synth_navigate_preset(int32_t delta);

// Send a parameter change to the synth
// group: SYNTH_PARAM_GROUP_*; paramId: 1-based ID from spec; value: raw value
void synth_send_parameter_change(uint8_t group, uint16_t paramId, uint16_t value);

// Applies `displayValue` (clamped to [0, dial->max-1]) to a dial: writes
// storage_value = displayValue + dial->storageOffset, computes the native
// value if the dial pairs one, and sends the appropriate protocol message
// (CC if dial->ccNumber is set, else a SysEx parameter change) — entirely
// driven by the dial's descriptor, no per-dial code required at call sites.
void synth_set_panel_dial_value(tPanelDial * dial, uint32_t displayValue);

// Effective on-wire name length for the connected device's protocol —
// cfg->panelNameLen for a Moog-style dump, cfg->progNameLen for a Korg-style
// live parameter-change name (see synth_set_program_name()'s own comment for
// why the two protocols encode a name so differently), capped at
// SYNTH_PROG_NAME_MAXLEN-1 (types.h) so callers never need their own second
// clamp. 0 if the connected device's config declares neither.
uint32_t synth_effective_name_maxlen(void);

// Commits a new program name typed by the user (see gProgNameEdit,
// globalVars.h) to the connected device. Truncates/space-pads newName to
// synth_effective_name_maxlen() first — both protocols below expect a fixed-
// width field, not a variable-length string. Branches entirely on
// cfg->moogStyleDump, same protocol split synth_set_panel_dial_value()
// already does for a dump-only dial vs. a CC/parameter-change one:
//   - Korg-style: one SYNTH_FUNC_PARAMETER_CHANGE per character (paramGroup
//     SYNTH_PARAM_GROUP_PROG, paramId 1..len), mirroring the incoming
//     per-char decode already in handle_parameter_change() (synthComms.c).
//   - Moog-style: no per-parameter name write exists (same "whole-dump load
//     only" constraint as a dump-only dial — see gLastMoogDump's own
//     comment) — encodes the name into a freshly-fetched Panel Dump and
//     resends, reusing the exact fetch-then-patch protection built for
//     dump-only dials (gProgNameAwaitingFreshData mirrors a dial's own
//     dumpSendAwaitingFreshData) rather than patching straight into
//     whatever gLastMoogDump happens to hold. UNCONFIRMED against real
//     hardware which bytes this actually writes are correct beyond matching
//     extract_moog_name()'s own decode addressing exactly in reverse — that
//     decode is hardware-confirmed, this encode has not itself been
//     round-tripped through a real Voyager yet.
// Also updates gDevice.progName immediately (optimistic, same reasoning as
// synth_navigate_preset()'s own comment) so the UI reflects the new name
// without waiting for a round trip. A no-op if synth_effective_name_maxlen()
// is 0 (connected device's config declares no name field), or — Moog-style
// only — if no Panel Dump has been received yet this session to patch into.
void synth_set_program_name(const char * newName);

#ifdef __cplusplus
}
#endif

#endif // __SYNTH_COMMS_H__
