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

#ifndef __DEFS_H__
#define __DEFS_H__

// ── Build toggles ─────────────────────────────────────────────────────────────
#define ENABLE_DEBUG                1
#define ENABLE_LOG_DEBUG            1

// ── Application ──────────────────────────────────────────────────────────────
#define WINDOW_TITLE                "Synth Edit"
#define TARGET_FRAME_BUFF_WIDTH     (2560)
#define TARGET_FRAME_BUFF_HEIGHT    (1440)

// ── Logging ──────────────────────────────────────────────────────────────────


// ── MIDI constants ────────────────────────────────────────────────────────────
#define MIDI_SYSEX_START              0xF0
#define MIDI_SYSEX_END                0xF7
#define MIDI_NON_REALTIME             0x7E
#define MIDI_DEVICE_INQUIRY           0x7F
#define MIDI_IDENTITY_REQUEST_SUB1    0x06
#define MIDI_IDENTITY_REQUEST_SUB2    0x01
#define MIDI_IDENTITY_REPLY_SUB2      0x02

// ── SysEx constants ───────────────────────────────────────────────────────────
// Manufacturer/family/member IDs live in xxxx.txt (manufacturerId/familyId/
// memberId), read via synth_panel_config() — not hardcoded here.

// Header: F0 <manufacturerId> (0x30|ch) <familyId> ...
// ch = MIDI global channel 0-indexed (channel 4 on synth panel = 0x03)
#define SYNTH_SYSEX_CHANNEL_BYTE(ch)    (0x30 | ((ch) & 0x0F))

// Synth function IDs (receive from synth)
#define SYNTH_FUNC_CURR_PROG_DUMP          0x40
#define SYNTH_FUNC_PROG_DUMP               0x4C
#define SYNTH_FUNC_CURR_MULTI_DUMP         0x49
#define SYNTH_FUNC_MULTI_DUMP              0x4D
#define SYNTH_FUNC_GLOBAL_MIDI_DUMP        0x51
#define SYNTH_FUNC_PARAMETER_CHANGE        0x41
#define SYNTH_FUNC_DATA_FORMAT_ERROR       0x26
#define SYNTH_FUNC_DATA_LOAD_COMPLETED     0x23
#define SYNTH_FUNC_DATA_LOAD_ERROR         0x24
#define SYNTH_FUNC_WRITE_COMPLETED         0x21
#define SYNTH_FUNC_WRITE_ERROR             0x22

// Synth function IDs (send to synth)
#define SYNTH_FUNC_CURR_PROG_DUMP_REQ      0x10
#define SYNTH_FUNC_PROG_DUMP_REQ           0x1C
#define SYNTH_FUNC_CURR_MULTI_DUMP_REQ     0x19
#define SYNTH_FUNC_GLOBAL_MIDI_DUMP_REQ    0x0E
#define SYNTH_FUNC_PROG_WRITE_REQ          0x11

// Synth parameter groups (for PARAMETER CHANGE, function 0x41)
#define SYNTH_PARAM_GROUP_GLOBAL           0x00
#define SYNTH_PARAM_GROUP_PROG             0x01
#define SYNTH_PARAM_GROUP_PATTERN          0x10
#define SYNTH_PARAM_GROUP_MULTI            0x11

// Program name length (parameters 1..progNameLen), CC assignments, and every
// per-control SysEx parameter ID/dump offset all live in <device>.txt
// (progNameLen/group=/param=/cc=/dumpOffset=), not here — see panelConfig.h
// and synthComms.c's generic dial dispatch/decode.


// ── Graphics / layout constants (used by utilsGraphics) ──────────────────────
#define MENU_BAR_HEIGHT    (24.0)

// ── Colour macros ─────────────────────────────────────────────────────────────


#endif // __DEFS_H__
