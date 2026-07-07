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

// ── Dial mouse mode ───────────────────────────────────────────────────────────

typedef enum {
    eDialModeRotary     = 0,   // circular arc around dial centre; cursor visible
    eDialModeVertical   = 1,   // drag up/down; cursor hidden
    eDialModeHorizontal = 2,   // drag left/right; cursor hidden
} tDialMode;

// ── Scroll state ──────────────────────────────────────────────────────────────

// ── MIDI device ───────────────────────────────────────────────────────────────

// Ceiling on program-name length across any supported device — the actual
// length used when parsing/sending is tPanelConfig.progNameLen, from the
// device's own <device>.txt ("progNameLen N"); this is just how big the
// buffer needs to be to hold the longest name any config is likely to
// specify. Every other piece of device state (filter/oscillator/mixer/
// category/voice-mode/unison/... or anything else a given synth has) lives
// entirely in that device's own panel-config dials (see panelConfig.h) —
// nothing synth-specific belongs here, so that adding a new device is just a
// new <device>.txt, no C changes.
#define SYNTH_PROG_NAME_MAXLEN    32

typedef struct {
    bool     connected;
    uint8_t  id;            // MIDI global channel 0-indexed; SYNTH_SYSEX_CHANNEL_BYTE(id) for header
    uint16_t family;
    uint16_t member;
    // Program info (decoded from CURR_PROG_DUMP) — see comment above.
    char     progName[SYNTH_PROG_NAME_MAXLEN];
} tSynthDevice;

#endif // __TYPES_H__
