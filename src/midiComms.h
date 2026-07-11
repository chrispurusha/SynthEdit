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

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int start_midi_thread(void);
int midi_scan_devices(void);

// Returns false (and logs why) if the message couldn't be sent — e.g. too
// large for the internal packet-list buffer (SYSEX_BUF_SIZE, midiComms.c —
// a whole-bank restore is the one message in this app big enough to hit
// that) or CoreMIDI itself rejected it. Every other existing caller (CC/
// parameter-change sends, dial patch-and-resend, etc.) predates this
// return value and still compiles fine ignoring it; Restore (synthBackup.c)
// is what actually checks it, added 2026-07-11 after a bank restore logged
// "sent" despite MIDIPacketListAdd having silently failed on the old
// 512-byte buffer.
bool midi_send(const uint8_t * data, uint32_t length);
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
void register_midi_wake_cb(void ( *cb )(void));

#ifdef __cplusplus
}
#endif

#endif // __MIDI_COMMS_H__
