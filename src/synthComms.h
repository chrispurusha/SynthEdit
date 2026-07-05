/*
 * The Z1-Edit application.
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

#ifdef __cplusplus
}
#endif

#endif // __SYNTH_COMMS_H__
