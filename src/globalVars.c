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

#include "sysIncludes.h"
#include "types.h"
#include "globalVars.h"

tDialMode       gDialMode       = eDialModeVertical;

_Atomic bool    gQuitAll        = false;
_Atomic bool    gReDraw         = true;

void *          gWindow         = NULL;
double          gGlobalGuiScale = 1.0;
tScrollState    gScrollState    = {0};

tZ1Device       gDevice         = {0};
MIDIClientRef   gMidiClient     = 0;
MIDIPortRef     gMidiInPort     = 0;
MIDIPortRef     gMidiOutPort    = 0;
MIDIEndpointRef gMidiSource     = 0;
MIDIEndpointRef gMidiDest       = 0;
