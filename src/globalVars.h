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

#ifndef __GLOBAL_VARS_H__
#define __GLOBAL_VARS_H__

#include "sysIncludes.h"
#include "types.h"

// ── Lifecycle ─────────────────────────────────────────────────────────────────
extern _Atomic bool     gQuitAll;
extern _Atomic bool     gReDraw;

// ── GLFW window ───────────────────────────────────────────────────────────────
extern void *           gWindow;           // GLFWwindow*; void* avoids pulling GLFW into C headers
extern double           gGlobalGuiScale;

// ── MIDI / device ─────────────────────────────────────────────────────────────
extern tZ1Device        gDevice;
extern MIDIClientRef    gMidiClient;
extern MIDIPortRef      gMidiInPort;
extern MIDIPortRef      gMidiOutPort;
extern MIDIEndpointRef  gMidiSource;
extern MIDIEndpointRef  gMidiDest;

#endif // __GLOBAL_VARS_H__
