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

#ifndef __TYPES_H__
#define __TYPES_H__

#include "defs.h"
#include "geometry.h"

// ── Colour ────────────────────────────────────────────────────────────────────

typedef struct {
    double red;
    double green;
    double blue;
} tRgb;

typedef struct {
    double red;
    double green;
    double blue;
    double alpha;
} tRgba;

#define RGB_WHITE              {1.0, 1.0, 1.0}
#define RGB_BLACK              {0.0, 0.0, 0.0}
#define RGB_GREY               {0.5, 0.5, 0.5}
#define RGB_BACKGROUND_GREY    {0.30, 0.30, 0.30}
#define RGB_GREY_2             {0.20, 0.20, 0.20}
#define RGB_GREY_3             {0.30, 0.30, 0.30}
#define RGB_GREY_5             {0.50, 0.50, 0.50}
#define RGB_GREY_7             {0.70, 0.70, 0.70}
#define RGB_GREEN_ON           {0.00, 0.80, 0.00}

// ── Geometry primitives ───────────────────────────────────────────────────────

typedef struct {
    tCoord coord1;
    tCoord coord2rel;
    tCoord coord3rel;
} tTriangle;

typedef struct {
    double u1;
    double v1;
    double u2;
    double v2;
    double advance_x;
    int    width;
    int    height;
    int    offset_x;
    int    offset_y;
} GlyphInfo;

// ── Render area tags ──────────────────────────────────────────────────────────

typedef enum {
    mainArea   = 0,
    moduleArea = 1,
} tArea;

typedef enum {
    eNoCache = 0,
    eCache   = 1,
} tCache;

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

typedef struct {
    double     xBar;
    bool       xBarDragging;
    double     xGrabOffset;
    tRectangle xThumb;
    double     yBar;
    bool       yBarDragging;
    double     yGrabOffset;
    tRectangle yThumb;
} tScrollState;

// ── MIDI device ───────────────────────────────────────────────────────────────

#define Z1_PROG_NAME_MAXLEN    17    // 16 chars + null terminator

typedef struct {
    bool     connected;
    uint8_t  id;            // MIDI global channel 0-indexed; Z1_SYSEX_CHANNEL_BYTE(id) for header
    uint16_t family;
    uint16_t member;
    // Program info (decoded from CURR_PROG_DUMP)
    char     progName[Z1_PROG_NAME_MAXLEN];
    uint8_t  category;            // 0-17, see kCategoryNames in z1Comms.c
    uint8_t  voiceMode;           // 0=MONO_MULTI 1=MONO_SINGLE 2=POLY
    bool     unisonOn;            // Unison SW
    uint8_t  unisonType;          // 0=OFF 1=2voices 2=3voices 3=6voices
    uint8_t  unisonDetune;        // 0-99 cents
    // Real-time CC values (0-127 dial position) and native SysEx values (0-99)
    uint8_t  filter1Cutoff;       // CC 85
    uint8_t  filter1CutoffNative; // SysEx param 263
    uint8_t  filter1Resonance;    // CC 86
    uint8_t  filter1ResNative;    // SysEx param 274
    uint8_t  filter1Type;         // SysEx param 261, value 1-5: LPF/HPF/BPF/BRF/2BPF
    uint8_t  filter2Cutoff;       // CC 88
    uint8_t  filter2CutoffNative; // SysEx param 290
    uint8_t  filter2Resonance;    // CC 89
    uint8_t  filter2ResNative;    // SysEx param 301
    uint8_t  filter2Type;         // SysEx param 288, value 1-5: LPF/HPF/BPF/BRF/2BPF
} tZ1Device;

#endif // __TYPES_H__
