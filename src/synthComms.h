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

// Send a parameter change to the synth
// group: SYNTH_PARAM_GROUP_*; paramId: 1-based ID from spec; value: raw value
void synth_send_parameter_change(uint8_t group, uint16_t paramId, uint16_t value);

// Applies `displayValue` (clamped to [0, dial->max-1]) to a dial: writes
// storage_value = displayValue + dial->storageOffset, computes the native
// value if the dial pairs one, and sends the appropriate protocol message
// (CC if dial->ccNumber is set, else a SysEx parameter change) — entirely
// driven by the dial's descriptor, no per-dial code required at call sites.
void synth_set_panel_dial_value(tPanelDial * dial, uint32_t displayValue);

#ifdef __cplusplus
}
#endif

#endif // __SYNTH_COMMS_H__
