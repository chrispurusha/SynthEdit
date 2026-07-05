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

// Called when a Korg Z1 is identified on the MIDI bus
void synth_on_connected(void);

// Dispatch an incoming Z1 SysEx message (full message including F0 header)
void synth_handle_message(const uint8_t * data, uint32_t length);

// Request the currently loaded program from the Z1
void synth_request_current_program(void);

// Send a parameter change to the Z1
// group: SYNTH_PARAM_GROUP_*; paramId: 1-based ID from spec; value: raw value
void synth_send_parameter_change(uint8_t group, uint16_t paramId, uint16_t value);

// Resolves each dial's valuePtr/nativeValuePtr in `section` to the live
// gDevice field it names in the layout file (by id) — the one place that
// still knows "f1cut means gDevice.filter1Cutoff". Call once after a
// successful load_panel_config(). Unknown ids are left unbound (get/set on
// them become no-ops rather than crashing).
void synth_bind_panel_dials(tPanelSection * section);

// Applies `displayValue` (clamped to [0, dial->max-1]) to a bound dial: writes
// storage_value = displayValue + dial->storageOffset, computes the native
// value if the dial pairs one, and sends the appropriate protocol message
// (CC if dial->ccNumber is set, else a SysEx parameter change) — entirely
// driven by the dial's descriptor, no per-dial code required at call sites.
void synth_set_panel_dial_value(tPanelDial * dial, uint32_t displayValue);

#ifdef __cplusplus
}
#endif

#endif // __SYNTH_COMMS_H__
