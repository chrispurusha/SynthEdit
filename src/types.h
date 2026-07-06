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

// ── Context menu ──────────────────────────────────────────────────────────────

typedef struct tMenuItem {
    const char *       label;
    tRgb               colour;
    void (*action)(int index);
    int                index;
    struct tMenuItem * subItems;
} tMenuItem;

typedef struct {
    bool        active;
    tCoord      coord;
    tMenuItem * items;
    uint32_t    count;
    uint32_t    columns;
    double      cellWidth;
} tContextMenu;

// ── Dial mouse mode ───────────────────────────────────────────────────────────

typedef enum {
    eDialModeRotary     = 0,   // circular arc around dial centre; cursor visible
    eDialModeVertical   = 1,   // drag up/down; cursor hidden
    eDialModeHorizontal = 2,   // drag left/right; cursor hidden
} tDialMode;

// ── Scroll state ──────────────────────────────────────────────────────────────

// ── MIDI device ───────────────────────────────────────────────────────────────

#define SYNTH_PROG_NAME_MAXLEN    17    // 16 chars + null terminator

typedef struct {
    bool     connected;
    uint8_t  id;            // MIDI global channel 0-indexed; SYNTH_SYSEX_CHANNEL_BYTE(id) for header
    uint16_t family;
    uint16_t member;
    // Program info (decoded from CURR_PROG_DUMP)
    char     progName[SYNTH_PROG_NAME_MAXLEN];
    uint8_t  category;            // 0-17, names in xxxx.txt's "category" list
    uint8_t  voiceMode;           // 0=MONO_MULTI 1=MONO_SINGLE 2=POLY
    bool     unisonOn;            // Unison SW
    uint8_t  unisonType;          // 0=OFF 1=2voices 2=3voices 3=6voices
    uint8_t  unisonDetune;        // 0-99 cents
    // Real-time CC values (0-127 dial position) and native SysEx values (0-99)
    uint8_t  filterRouting;       // SysEx param 258, value 0-2: SERI1/SERI2/PARA
    uint8_t  filter2Link;         // SysEx param 259, value 0-1: OFF/ON
    uint8_t  filter1InputTrim;    // SysEx param 262, value 0-99
    uint8_t  filter1Cutoff;       // CC 85
    uint8_t  filter1CutoffNative; // SysEx param 263
    uint8_t  filter1Resonance;    // CC 86
    uint8_t  filter1ResNative;    // SysEx param 274
    uint8_t  filter1Type;         // SysEx param 261, value 1-5: LPF/HPF/BPF/BRF/2BPF
    uint8_t  filter2InputTrim;    // SysEx param 289, value 0-99
    uint8_t  filter2Cutoff;       // CC 88
    uint8_t  filter2CutoffNative; // SysEx param 290
    uint8_t  filter2Resonance;    // CC 89
    uint8_t  filter2ResNative;    // SysEx param 301
    uint8_t  filter2Type;         // SysEx param 288, value 1-5: LPF/HPF/BPF/BRF/2BPF
    // Oscillator section (SysEx params 163-257, sits above Filter above in the
    // device's own parameter table order). Signed-range raw values below are
    // stored exactly as they arrive off the wire/dump (see layouts/z1.txt's
    // Oscillator section comment for the offset conventions and why they're
    // unconfirmed against real hardware).
    uint8_t pitchBendIntPlus;      // SysEx param 163, raw 0-84 (display -60~+24)
    uint8_t pitchBendIntMinus;     // SysEx param 164, raw 0-84 (display -60~+24)
    uint8_t pitchBendStepPlus;     // SysEx param 165, raw 0-15: 0=off,1=/8,2=/4,3=/2,4-15=1-12 semi
    uint8_t pitchBendStepMinus;    // SysEx param 166, raw 0-15, same enum as StepPlus
    uint8_t portamentoSW;          // SysEx param 169, value 0-1: OFF/ON
    uint8_t portamentoMode;        // SysEx param 170, value 0-1: NORMAL/FINGERED
    uint8_t portamentoTime;        // SysEx param 171, value 0-99
    uint8_t osc1Type;              // SysEx param 174, value 0-12: see "o1type" names in z1.txt
    uint8_t osc1Octave;            // SysEx param 175, value 0-3: 32'/16'/8'/4'
    uint8_t osc1SemiTone;          // SysEx param 176, raw 0-24 (display -12~+12)
    uint8_t osc1FineTune;          // SysEx param 177, raw 0-100 (display -50~+50 cent)
    uint8_t osc1FreqOffset;        // SysEx param 178, raw 0-200 (display -10.0~+10.0Hz, 0.1Hz/step)
    uint8_t osc2Type;              // SysEx param 188, value 0-8 (fewer types than OSC1)
    uint8_t osc2Octave;            // SysEx param 189, value 0-3: 32'/16'/8'/4'
    uint8_t osc2SemiTone;          // SysEx param 190, raw 0-24 (display -12~+12)
    uint8_t osc2FineTune;          // SysEx param 191, raw 0-100 (display -50~+50 cent)
    uint8_t osc2FreqOffset;        // SysEx param 192, raw 0-200 (display -10.0~+10.0Hz, 0.1Hz/step)
    uint8_t subOscOctave;          // SysEx param 202, value 0-3: 32'/16'/8'/4'
    uint8_t subOscSemiTone;        // SysEx param 203, raw 0-24 (display -12~+12)
    uint8_t subOscFineTune;        // SysEx param 204, raw 0-100 (display -50~+50 cent)
    uint8_t subOscFreqOffset;      // SysEx param 205, raw 0-200 (display -10.0~+10.0Hz, 0.1Hz/step)
    uint8_t subOscWaveForm;        // SysEx param 215, value 0-3: SAW/SQU/TRI/SIN
    uint8_t noiseFilterType;       // SysEx param 216, value 0-3: THRU/LPF/HPF/BPF
    uint8_t noiseFilterTrim;       // SysEx param 217, value 0-99
    uint8_t noiseFilterCutoff;     // SysEx param 218, value 0-99
    uint8_t noiseFilterResonance;  // SysEx param 223, value 0-99
    uint8_t mixerOsc1Out1;         // SysEx param 224, value 0-99
    uint8_t mixerOsc1Out2;         // SysEx param 227, value 0-99
    uint8_t mixerOsc2Out1;         // SysEx param 230, value 0-99
    uint8_t mixerOsc2Out2;         // SysEx param 233, value 0-99
    uint8_t mixerSubOut1;          // SysEx param 236, value 0-99
    uint8_t mixerSubOut2;          // SysEx param 239, value 0-99
    uint8_t mixerNoiseOut1;        // SysEx param 242, value 0-99
    uint8_t mixerNoiseOut2;        // SysEx param 245, value 0-99
    uint8_t mixerFeedbackOut1;     // SysEx param 248, value 0-99
    uint8_t mixerFeedbackOut2;     // SysEx param 251, value 0-99
    uint8_t mixerOsc1SW;           // SysEx param 254, value 0-1: OFF/ON
    uint8_t mixerOsc2SW;           // SysEx param 255, value 0-1: OFF/ON
    uint8_t mixerSubSW;            // SysEx param 256, value 0-1: OFF/ON
    uint8_t mixerNoiseSW;          // SysEx param 257, value 0-1: OFF/ON
} tSynthDevice;

#endif // __TYPES_H__
