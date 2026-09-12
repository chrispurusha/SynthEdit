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
// Notes: Docs/code-notes/types.h.md - "// notes §k" refers there.

#ifndef __TYPES_H__
#define __TYPES_H__

#include "defs.h"
#include "synthlibDefs.h"
#include "geometry.h"

// ── Colour ────────────────────────────────────────────────────────────────────

// ── UI buttons ────────────────────────────────────────────────────────────────

typedef enum {
    pkNone  = 0,
    pkEnter = 1,
    pkExit  = 2,
    pkUp    = 3,
    pkDown  = 4,
    pkLeft  = 5,
    pkRight = 6,
    pkInc   = 7,
    pkDec   = 8,
    pkF1    = 10,
    pkF2    = 11,
    pkF3    = 12,
    pkF4    = 13,
    pkF5    = 14,
    pkF6    = 15,
} tButtonKey;

typedef struct {
    tButtonKey key;
    char       label[32];
    bool       pressed;
    tRectangle rect;
} tButton;

// tDialMode now lives in SynthLib's synthlibTypes.h (identical across all three apps) — pulled in
// transitively via geometry.h above.

// ── Scroll state ──────────────────────────────────────────────────────────────

// ── MIDI device ───────────────────────────────────────────────────────────────

// notes §1
#define SYNTH_PROG_NAME_MAXLEN    32

typedef struct {
    bool     connected;
    uint8_t  id;            // MIDI global channel 0-indexed; SYNTH_SYSEX_CHANNEL_BYTE(id) for header
    uint16_t family;
    uint16_t member;
    // Program info (decoded from CURR_PROG_DUMP) — see comment above.
    char     progName[SYNTH_PROG_NAME_MAXLEN];
    // notes §2
    int32_t currentProgram;
    // notes §3
    uint8_t moogDeviceId;
} tSynthDevice;

// notes §4
typedef struct {
    bool     active;
    char     buffer[SYNTH_PROG_NAME_MAXLEN];
    uint32_t cursorPos;
} tNameEdit;

#endif // __TYPES_H__
