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

#ifndef __DEFS_H__
#define __DEFS_H__

// ── Build toggles ─────────────────────────────────────────────────────────────
#define ENABLE_DEBUG                1

// ── Application ──────────────────────────────────────────────────────────────
#define WINDOW_TITLE                "Z1 Edit"
#define TARGET_FRAME_BUFF_WIDTH     (2560)
#define TARGET_FRAME_BUFF_HEIGHT    (1440)

// ── Logging ──────────────────────────────────────────────────────────────────
#if ENABLE_DEBUG
#define LOG_DEBUG(fmt, ...)    fprintf(stderr, "[DBG] " fmt, ## __VA_ARGS__)
#else
#define LOG_DEBUG(fmt, ...)    do {} while (0)
#endif
#define LOG_ERROR(fmt, ...)    fprintf(stderr, "[ERR] " fmt, ## __VA_ARGS__)

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
#define Z1_FAMILY_ID                  0x46    // Z1 Series Family ID (LSB)
#define Z1_MEMBER_ID                  0x01    // Z1 Member ID (LSB)

// Header: F0 42 (0x30|ch) 46 ...
// ch = MIDI global channel 0-indexed (channel 4 on Z1 panel = 0x03)
#define Z1_SYSEX_CHANNEL_BYTE(ch)    (0x30 | ((ch) & 0x0F))

// Z1 function IDs (receive from Z1)
#define Z1_FUNC_CURR_PROG_DUMP          0x40
#define Z1_FUNC_PROG_DUMP               0x4C
#define Z1_FUNC_CURR_MULTI_DUMP         0x49
#define Z1_FUNC_MULTI_DUMP              0x4D
#define Z1_FUNC_GLOBAL_MIDI_DUMP        0x51
#define Z1_FUNC_PARAMETER_CHANGE        0x41
#define Z1_FUNC_DATA_FORMAT_ERROR       0x26
#define Z1_FUNC_DATA_LOAD_COMPLETED     0x23
#define Z1_FUNC_DATA_LOAD_ERROR         0x24
#define Z1_FUNC_WRITE_COMPLETED         0x21
#define Z1_FUNC_WRITE_ERROR             0x22

// Z1 function IDs (send to Z1)
#define Z1_FUNC_CURR_PROG_DUMP_REQ      0x10
#define Z1_FUNC_PROG_DUMP_REQ           0x1C
#define Z1_FUNC_CURR_MULTI_DUMP_REQ     0x19
#define Z1_FUNC_GLOBAL_MIDI_DUMP_REQ    0x0E
#define Z1_FUNC_PROG_WRITE_REQ          0x11

// Z1 parameter groups (for PARAMETER CHANGE, function 0x41)
#define Z1_PARAM_GROUP_GLOBAL           0x00
#define Z1_PARAM_GROUP_PROG             0x01
#define Z1_PARAM_GROUP_PATTERN          0x10
#define Z1_PARAM_GROUP_MULTI            0x11

// Z1 program name length (parameters 1-16)
#define Z1_PROG_NAME_LEN                16

// Z1 program parameter IDs (group Z1_PARAM_GROUP_PROG)
#define Z1_PARAM_FILTER_ROUTING         258 // value 0-2: SERI1/SERI2/PARA; decoded[311] bits 0-1
#define Z1_PARAM_FILTER2_LINK           259 // value 0-1: OFF/ON; decoded[311] bit 2
#define Z1_PARAM_FILTER1_TYPE           261 // value 1-5: LPF/HPF/BPF/BRF/2BPF; decoded[312]
#define Z1_PARAM_FILTER1_CUTOFF         263 // value 0-99; confirmed from SysEx capture; decoded[314]
#define Z1_PARAM_FILTER1_RESONANCE      274 // value 0-99; decoded[325]
#define Z1_PARAM_FILTER2_TYPE           288 // value 1-5: LPF/HPF/BPF/BRF/2BPF; decoded[339]
#define Z1_PARAM_FILTER2_CUTOFF         290 // value 0-99; decoded[341]
#define Z1_PARAM_FILTER2_RESONANCE      301 // value 0-99; decoded[352]


// ── Graphics / layout constants (used by utilsGraphics) ──────────────────────
#define MAX_GLYPH_CHAR          (127)
#define BORDER_LINE_WIDTH       (2.0)
#define STANDARD_TEXT_HEIGHT    (12.0)
#define BLANK_SIZE              (0.0)
#define NO_ZOOM                 (1.0)
#define SCROLLBAR_WIDTH         (15.0)
#define SCROLLBAR_LENGTH        (100.0)
#define SCROLLBAR_MARGIN        SCROLLBAR_WIDTH
#define TOP_BAR_HEIGHT          (0.0)
#define MODULE_MARGIN           (5.0)
#define MODULE_WIDTH            (350.0)
#define MODULE_HEIGHT           (38.0)
#define MODULE_X_GAP            (10.0)
#define MODULE_X_SPAN           (MODULE_WIDTH + MODULE_X_GAP)
#define MODULE_Y_GAP            (5.0)
#define MODULE_Y_SPAN           (MODULE_HEIGHT + MODULE_Y_GAP)
#define MAX_COLUMNS             (127)
#define MAX_ROWS                (127)
#define MAX_ROWS_MODULE         (12)

// ── Colour macros ─────────────────────────────────────────────────────────────
#define RGB_ORANGE_1            {1.00, 0.50, 0.00}
#define RGB_ORANGE_2            {1.00, 0.70, 0.00}

#endif // __DEFS_H__
