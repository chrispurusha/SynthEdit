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
