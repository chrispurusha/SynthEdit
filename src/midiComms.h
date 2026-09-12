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
// Notes: Docs/code-notes/midiComms.h.md - "// notes §k" refers there.

#ifndef __MIDI_COMMS_H__
#define __MIDI_COMMS_H__

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

int start_midi_thread(void);

// notes §1
void midi_request_reconnect(void);

// UI thread. The MIDI Ports dialogue's choice is kept per device configuration; call this with the
// configuration's file name whenever it changes (synth_reload_panel_config() does).
void midi_set_port_scope(const char * configFile);

// UI thread. One line saying what is connected, for the MIDI Ports dialogue's status row.
void midi_port_status(char * text, size_t size);

// UI thread: the device's channel once connected, 1-16, or 0; for the dialogue's Auto cell.
uint32_t midi_channel_in_use(void);

// notes §2
bool midi_send(const uint8_t * data, uint32_t length);
void midi_send_cc(uint8_t channelIndex, uint8_t cc, uint8_t value);
void midi_send_program_change(uint8_t channelIndex, uint8_t program);
void midi_send_identity_request(void);

// notes §3
void midi_arm_state_dump_debounce(void);
void register_midi_wake_cb(void ( *cb )(void));

#ifdef __cplusplus
}
#endif

#endif // __MIDI_COMMS_H__
