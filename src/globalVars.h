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

#ifndef __GLOBAL_VARS_H__
#define __GLOBAL_VARS_H__

#include "sysIncludes.h"
#include "types.h"
#include "msgQueue.h"
#include "synthlibGlobals.h" // synthlib_quit_requested()/synthlib_request_redraw()/synthlib_window()/synthlib_dial_mode() etc.

// ── MIDI / device ─────────────────────────────────────────────────────────────
extern tSynthDevice    gDevice;
extern tNameEdit       gProgNameEdit;
extern MIDIClientRef   gMidiClient;
extern MIDIPortRef     gMidiInPort;
extern MIDIPortRef     gMidiOutPort;
extern MIDIEndpointRef gMidiSource;
extern MIDIEndpointRef gMidiDest;

// CoreMIDI read callback thread -> MIDI thread. Initialised by start_midi_thread() before the
// thread is created. See msgQueue.h for what does and doesn't belong on it.
extern tMessageQueue   gToMidiThread;

#endif // __GLOBAL_VARS_H__
