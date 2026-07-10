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

#ifndef __MIDI_COMMS_H__
#define __MIDI_COMMS_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int start_midi_thread(void);
int midi_scan_devices(void);
void midi_send(const uint8_t * data, uint32_t length);
void midi_send_cc(uint8_t channelIndex, uint8_t cc, uint8_t value);
void midi_send_program_change(uint8_t channelIndex, uint8_t program);
void midi_send_identity_request(void);

// Arms (or re-arms, restarting the countdown) a debounced "request a fresh
// state dump" — fires exactly once, ~SYNTH_STATE_DUMP_DEBOUNCE_TICKS *
// MIDI_IDLE_TICK_SECONDS after the last call, from the MIDI thread's own
// idle loop (see midi_thread() in midiComms.c). Callers: dispatch_program_change()
// (a Bank/Program Change arriving from elsewhere on the bus) and
// synth_navigate_preset() (synthComms.c, the Prev/Next patch buttons) —
// both used to call synth_request_state_dump() directly, which real
// hardware couldn't always keep up with under a rapid burst of changes (see
// the comment above gStateDumpDebounceTicks' definition for the capture
// that showed this).
void midi_arm_state_dump_debounce(void);

// Resets the periodic low-frequency state-poll timer (gTicksSinceLastArm,
// midiComms.c) WITHOUT arming a new debounced request — call this whenever a
// fresh state dump reply actually arrives, from ANY source (Sync button,
// Backup, a CC-triggered or periodic-triggered request, anything), so the
// poll doesn't fire again for another ~15s when we already just got current
// data. Deliberately separate from midi_arm_state_dump_debounce() above:
// that one both resets the poll timer AND schedules an outgoing request;
// this one only means "our data is fresh now," it must NOT also queue a
// request 264ms from now.
void midi_note_state_dump_received(void);
void register_midi_wake_cb(void ( *cb )(void));

#ifdef __cplusplus
}
#endif

#endif // __MIDI_COMMS_H__
