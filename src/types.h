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
    // Best-known current Program Change number (0-127, MIDI wire numbering),
    // or -1 if unknown. There's no reliable way to ask a device "what
    // program are you on" — a dump reply (Panel Dump, Current Program Dump)
    // reports the live edit buffer's contents, not which stored slot (if
    // any) it started from, so this is only ever learned from an actual
    // Program Change message: one arriving from elsewhere on the bus
    // (dispatch_program_change() in midiComms.c), or one this app itself
    // just sent (synth_navigate_preset() in synthComms.c, for the Prev/Next
    // patch buttons — see synth_hit_test_patch_nav() in synthGraphics.h).
    // Reset to -1 on every fresh connect (synth_on_connected()): a value
    // learned from a previous session/device isn't trustworthy for a new one.
    int32_t currentProgram;
    // Moog-style protocol's own SysEx "Device ID" byte (0-127) — see
    // moogStyleDump in panelConfig.h. Distinct from `id` above (the MIDI
    // CHANNEL): the Voyager's front panel exposes Device ID and MIDI Channel
    // as two separate settings, and a dump REQUEST addressed to the wrong
    // Device ID is silently ignored by the hardware regardless of channel.
    // Seeded from the device's own <device>.txt (stateRequestSysEx's own
    // deviceId byte, at a fixed offset — see moog_learn_device_id()'s own
    // comment, synthComms.c) as a first guess matching the factory default,
    // then kept in sync with reality by moog_learn_device_id() on every
    // accepted incoming Moog SysEx message, the same way an independently-
    // developed reference implementation for this exact hardware
    // (moogvoyagereditor.pistolinstruments.com's midi.js) already does — its
    // own comment there names the exact failure this avoids: a request built
    // with a stale/wrong Device ID is silently ignored by the synth, which
    // just looks like "Fetch" doing nothing. Meaningless (left at whatever
    // the config seeded) for a device that doesn't set moogStyleDump — only
    // this dump-request SysEx format has a Device ID byte at all; the
    // Minitaur, for instance, dumps state as plain CC traffic instead (see
    // minitaur.txt's own stateRequestSysEx comment) and never reaches
    // moog_learn_device_id() in the first place.
    uint8_t moogDeviceId;
} tSynthDevice;

// Inline-editable program name field state (click the name to start, type,
// Enter commits via synth_set_program_name() (synthComms.h), Escape
// cancels) — modelled on G2-Edit's own tNameEdit (mouseHandle.c there),
// minus the multi-slot bookkeeping G2's 4-slot patch browser needs: this
// app only ever edits the one currently-loaded program name. buffer holds
// the FLAT name (no line-wrap '\n's — those are synth_render()'s own
// display concern, see wrap_name_for_display() in synthGraphics.cpp),
// capped at SYNTH_PROG_NAME_MAXLEN regardless of which connected device's
// progNameLen/panelNameLen is actually shorter (synth_effective_name_maxlen(),
// synthComms.h, is what enforces the real per-device cap while typing).
typedef struct {
    bool     active;
    char     buffer[SYNTH_PROG_NAME_MAXLEN];
    uint32_t cursorPos;
} tNameEdit;

#endif // __TYPES_H__
