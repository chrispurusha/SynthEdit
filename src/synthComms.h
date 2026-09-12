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
// Notes: Docs/code-notes/synthComms.h.md - "// notes §k" refers there.

#ifndef __SYNTH_COMMS_H__
#define __SYNTH_COMMS_H__

#include <stddef.h>
#include <stdint.h>

#include "panelConfig.h"

#ifdef __cplusplus
extern "C" {
#endif

// Called when a synth is identified on the MIDI bus
void synth_on_connected(void);

// notes §1
void synth_decode_moog_name(const uint8_t * payload, uint32_t payloadLen, int32_t offset, uint32_t bitOffset, uint32_t len, uint32_t lineWidth, char * outName, size_t outNameSize);

// notes §2
bool synth_moog_single_preset_dump_intact(const uint8_t * data, uint32_t length);

// Korg counterpart to synth_moog_single_preset_dump_intact() above, for a
// Program Data Dump/Current Program Dump reply instead of a Single Preset
// Dump. Added 2026-07-14.
bool synth_korg_program_dump_intact(const uint8_t * data, uint32_t length);

// notes §3
void synth_decode_moog_category(const uint8_t * data, uint32_t length, char * outCategory, size_t outCategorySize, uint8_t * outIndex);

// notes §4
void synth_decode_korg_name(const uint8_t * data, uint32_t length, char * outName, size_t outNameSize);

// notes §5
void synth_decode_korg_category(const uint8_t * data, uint32_t length, char * outCategory, size_t outCategorySize, uint8_t * outIndex);

// notes §6
bool synth_decode_korg_prog_dump(const uint8_t * data, uint32_t length, uint8_t * decoded, uint32_t decodedCap, uint32_t * outDecodedLen);

// notes §7
void synth_send_korg_current_program_dump(const uint8_t * rawPayload, uint32_t rawPayloadLen);

// notes §8
void synth_send_korg_program_data_dump(uint8_t bank, uint32_t progNumber, const uint8_t * rawPayload, uint32_t rawPayloadLen);

// notes §9
void synth_apply_moog_panel_dump_locally(const uint8_t * data, uint32_t length);
void synth_apply_korg_prog_dump_locally(const uint8_t * decoded, uint32_t decodedLen);

// Dispatch an incoming synth SysEx message (full message including F0 header)
void synth_handle_message(const uint8_t * data, uint32_t length);

// notes §10
bool synth_handle_cc(uint8_t cc, uint8_t value);

// Commits any quantized switch/selector dial's debounced CC value once it's
// been quiet for CC_DEBOUNCE_MS — see hasPendingCc's own comment in
// panelConfig.h. Call once per frame from the render loop.
void synth_flush_pending_cc(void);

// Sends any dump-only dial's debounced patch-and-resend once it's been quiet
// for CC_DEBOUNCE_MS — see hasPendingDumpSend's own comment in panelConfig.h.
// Call once per frame from the render loop, alongside synth_flush_pending_cc().
void synth_flush_pending_dump_sends(void);

// notes §11
void synth_flush_pending_param_send(void);

// Request the currently loaded program from the synth
void synth_request_current_program(void);

// notes §12
void synth_request_state_dump(void);

// notes §13
bool synth_dump_patch_in_flight(void);

// notes §14
void synth_request_single_preset_dump(uint32_t presetNumber);

// notes §15
void synth_request_all_presets_dump(void);

// notes §16
void synth_navigate_preset(int32_t delta);

// A Program Change this app sent (fromSynth false) or heard from the synth - types.h notes §2.
void synth_note_program_change(uint8_t program, bool fromSynth);

// Program names compared with whitespace collapsed - synthComms.c notes §87.
bool synth_prog_names_equal(const char * a, const char * b);

// notes §17
void synth_load_patch_from_bank(uint8_t bank, uint32_t presetNumber);

// notes §18
void synth_korg_select_program(uint8_t bank, uint32_t progNumber);

// notes §19
void synth_request_korg_program_dump(uint8_t bank, uint32_t progNumber);

// notes §20
void synth_send_korg_program_write_request(uint8_t bank, uint32_t progNumber);

// Send a parameter change to the synth
// group: SYNTH_PARAM_GROUP_*; paramId: 1-based ID from spec; value: raw value
void synth_send_parameter_change(uint8_t group, uint16_t paramId, uint16_t value);

// notes §21
void synth_set_panel_dial_value(tPanelDial * dial, uint32_t displayValue);

// notes §22
uint32_t synth_effective_name_maxlen(void);

// notes §23
void synth_set_program_name(const char * newName);

#ifdef __cplusplus
}
#endif

#endif // __SYNTH_COMMS_H__
