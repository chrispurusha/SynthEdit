/*
 * The Z1-Edit application.
 *
 * Copyright (C) 2025 Chris Turner <chris_purusha@icloud.com>
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

#ifndef __MIDI_COMMS_H__
#define __MIDI_COMMS_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int  start_midi_thread(void);
int  midi_scan_devices(void);
void midi_send(const uint8_t * data, uint32_t length);
void midi_send_cc(uint8_t channelIndex, uint8_t cc, uint8_t value);
void midi_send_identity_request(void);
void register_midi_wake_cb(void (*cb)(void));

#ifdef __cplusplus
}
#endif

#endif // __MIDI_COMMS_H__
