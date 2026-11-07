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
// Notes: Docs/code-notes/coreMidiWin.h.md - "// notes §k" refers there.

#ifndef __CORE_MIDI_WIN_H__
#define __CORE_MIDI_WIN_H__

#include <stdbool.h>
#include <stdint.h>

#include "synthlibMidiPorts.h"   // MIDIEndpointRef, MIDIPortRef off Apple

#ifdef __cplusplus
extern "C" {
#endif

// notes §1
typedef uint32_t   MIDIClientRef;
typedef uint32_t   ItemCount;
typedef int32_t    OSStatus;
#define noErr    0

// ── winmm, behind SynthLib's port names (platform/windows/synthlibMidi*Win.c) ───────────────────

// notes §2
uint32_t        synthlib_midi_win_count(bool inputs);
MIDIEndpointRef synthlib_midi_win_endpoint(bool inputs, uint32_t index);

// Resolves an endpoint to the winmm device index carrying it now; false if it is not plugged in.
bool            synthlib_midi_win_index(MIDIEndpointRef endpoint, bool * input, uint32_t * index);

// notes §3
typedef void (*tSynthLibMidiWinReceive)(MIDIEndpointRef src, const uint8_t * data, uint32_t length);
void            synthlib_midi_win_open_inputs(tSynthLibMidiWinReceive receive);

// notes §4
bool            synthlib_midi_win_ports_changed(void);

// The four CoreMIDI calls midiComms.c makes that need no Windows-specific code around them
static inline ItemCount MIDIGetNumberOfSources(void) {
    return synthlib_midi_win_count(true);
}

static inline ItemCount MIDIGetNumberOfDestinations(void) {
    return synthlib_midi_win_count(false);
}

static inline MIDIEndpointRef MIDIGetSource(ItemCount i) {
    return synthlib_midi_win_endpoint(true, i);
}

static inline MIDIEndpointRef MIDIGetDestination(ItemCount i) {
    return synthlib_midi_win_endpoint(false, i);
}

#ifdef __cplusplus
}
#endif

#endif // __CORE_MIDI_WIN_H__
