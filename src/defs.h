/*
 * The Z1-Edit application.
 *
 * Copyright (C) 2025 Chris Turner <chris_purusha@icloud.com>
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

#ifndef __DEFS_H__
#define __DEFS_H__

// ── Build toggles ─────────────────────────────────────────────────────────────
#define ENABLE_DEBUG    1

// ── Application ──────────────────────────────────────────────────────────────
#define WINDOW_TITLE                  "Z1 Edit"
#define TARGET_FRAME_BUFF_WIDTH       (2560)
#define TARGET_FRAME_BUFF_HEIGHT      (1440)

// ── Logging ──────────────────────────────────────────────────────────────────
#if ENABLE_DEBUG
    #define LOG_DEBUG(fmt, ...)    fprintf(stderr, "[DBG] " fmt, ##__VA_ARGS__)
#else
    #define LOG_DEBUG(fmt, ...)    do {} while (0)
#endif
#define LOG_ERROR(fmt, ...)        fprintf(stderr, "[ERR] " fmt, ##__VA_ARGS__)

// ── MIDI constants ────────────────────────────────────────────────────────────
#define MIDI_SYSEX_START              0xF0
#define MIDI_SYSEX_END                0xF7
#define MIDI_NON_REALTIME             0x7E
#define MIDI_DEVICE_INQUIRY           0x7F
#define MIDI_IDENTITY_REQUEST_SUB1    0x06
#define MIDI_IDENTITY_REQUEST_SUB2    0x01
#define MIDI_IDENTITY_REPLY_SUB2      0x02

// ── Korg Z1 SysEx constants ───────────────────────────────────────────────────
#define KORG_MANUFACTURER_ID          0x42

#endif // __DEFS_H__
