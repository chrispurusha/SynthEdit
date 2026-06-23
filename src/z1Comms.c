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

#include <stdint.h>
#include <string.h>
#include "defs.h"
#include "types.h"
#include "globalVars.h"
#include "midiComms.h"
#include "z1Comms.h"

// ── SysEx header helpers ──────────────────────────────────────────────────────

// Minimum length of a valid Z1 SysEx: F0 42 3g 46 <func> F7
#define Z1_HDR_LEN    5

static bool is_z1_sysex(const uint8_t * data, uint32_t length) {
    if (length < Z1_HDR_LEN) {
        return false;
    }
    return (data[0] == MIDI_SYSEX_START)
        && (data[1] == KORG_MANUFACTURER_ID)
        && ((data[2] & 0xF0) == 0x30)
        && (data[3] == Z1_FAMILY_ID);
}

// ── 7-to-8 bit decoding ───────────────────────────────────────────────────────
// Korg packs 7 data bytes into 8 MIDI bytes.
// Byte 0 of each group holds the MSBs (bit6 = MSB of byte1, bit5 = MSB of byte2, …)
// Returns number of decoded bytes written.
static uint32_t decode_7to8(const uint8_t * midi, uint32_t midiLen, uint8_t * out, uint32_t outMax) {
    uint32_t outLen = 0;

    for (uint32_t i = 0; (i + 7) < midiLen && outLen < outMax; i += 8) {
        uint8_t msbs = midi[i];
        for (int j = 0; j < 7 && outLen < outMax; j++) {
            out[outLen++] = (uint8_t)(midi[i + 1 + j] | (((msbs >> (6 - j)) & 1) << 7));
        }
    }
    return outLen;
}

// ── 8-to-7 bit encoding ───────────────────────────────────────────────────────
// Returns number of MIDI bytes written (always ceil(dataLen/7)*8).
static uint32_t encode_8to7(const uint8_t * data, uint32_t dataLen, uint8_t * out, uint32_t outMax) {
    uint32_t outLen = 0;

    for (uint32_t i = 0; i < dataLen && (outLen + 8) <= outMax; i += 7) {
        uint8_t  msbs     = 0;
        uint32_t groupSz  = (dataLen - i >= 7) ? 7 : (dataLen - i);
        for (uint32_t j = 0; j < groupSz; j++) {
            msbs |= (uint8_t)(((data[i + j] >> 7) & 1) << (6 - j));
        }
        out[outLen++] = msbs;
        for (uint32_t j = 0; j < groupSz; j++) {
            out[outLen++] = data[i + j] & 0x7F;
        }
        // pad incomplete group
        for (uint32_t j = groupSz; j < 7; j++) {
            out[outLen++] = 0x00;
        }
    }
    return outLen;
}

// ── Build outgoing Z1 SysEx header ───────────────────────────────────────────
static uint32_t build_header(uint8_t * buf, uint8_t funcId) {
    buf[0] = MIDI_SYSEX_START;
    buf[1] = KORG_MANUFACTURER_ID;
    buf[2] = Z1_SYSEX_CHANNEL_BYTE(gDevice.id);
    buf[3] = Z1_FAMILY_ID;
    buf[4] = funcId;
    return 5;
}

// ── Category name table ───────────────────────────────────────────────────────
// Matches Z1 program category parameter (ID 17), values 0-17
static const char * kCategoryNames[] = {
    "Synth-Hard", "Synth-Soft", "E.Piano",    "Organ",
    "Strings",    "Brass",      "Wind",        "Bell/Mallt",
    "Guitar",     "Bass",       "Perc/Drums",  "Vocal",
    "S.E./Natrl", "Synth-Lead", "Synth-Pad",   "Synth-Comp",
    "Digital",    "User",
};
#define kCategoryCount    (sizeof(kCategoryNames) / sizeof(kCategoryNames[0]))

static const char * kVoiceModeNames[] = {"Mono Multi", "Mono Single", "Poly"};
static const char * kUnisonTypeNames[] = {"OFF", "2 voices", "3 voices", "6 voices"};

// ── Program info extraction ───────────────────────────────────────────────────
// Byte layout (derived from Z1 MIDI spec parameter table, packed fields noted):
//   0-15  : Program Name chars (params 1-16)
//     16  : Category          (param 17)
//     17  : User Group        (param 18)
//     18  : Hold[0] + KeyPriority[1:2] + VoiceAssignMode[3:4]  (params 19-21, packed)
//     19  : Retrigger Controller     (param 22)
//     20  : Retrigger Threshold      (param 23)
//     21  : UnisonType[0:1] + UnisonSW[2] + UnisonMode[3]      (params 24-26, packed)
//     22  : Unison Detune            (param 27)
static void extract_prog_info(const uint8_t * decoded, uint32_t decodedLen) {
    // Name (bytes 0-15)
    uint32_t nameLen = (decodedLen >= Z1_PROG_NAME_LEN) ? Z1_PROG_NAME_LEN : decodedLen;
    uint32_t i;

    for (i = 0; i < nameLen; i++) {
        char c = (char)decoded[i];
        gDevice.progName[i] = ((c >= 0x20) && (c <= 0x7F)) ? c : '?';
    }
    while ((i > 0) && (gDevice.progName[i - 1] == ' ')) {
        i--;
    }
    gDevice.progName[i] = '\0';

    // Category (byte 16)
    if (decodedLen > 16) {
        gDevice.category = decoded[16] & 0x1F;
        if (gDevice.category >= kCategoryCount) {
            gDevice.category = 0;
        }
    }

    // Voice assign mode (byte 18, bits 3-4)
    if (decodedLen > 18) {
        gDevice.voiceMode  = (decoded[18] >> 3) & 0x03;
    }

    // Unison (byte 21: bits 0-1 = type, bit 2 = SW; byte 22 = detune)
    if (decodedLen > 21) {
        gDevice.unisonType = decoded[21] & 0x03;
        gDevice.unisonOn   = (decoded[21] >> 2) & 0x01;
    }
    if (decodedLen > 22) {
        gDevice.unisonDetune = decoded[22];
    }

    LOG_DEBUG("Z1 prog: \"%s\"  cat=%s  voice=%s  unison=%s(%u cents)\n",
              gDevice.progName,
              kCategoryNames[gDevice.category],
              kVoiceModeNames[gDevice.voiceMode < 3 ? gDevice.voiceMode : 2],
              gDevice.unisonOn ? kUnisonTypeNames[gDevice.unisonType & 3] : "OFF",
              (unsigned)gDevice.unisonDetune);
}

// ── Message handlers ──────────────────────────────────────────────────────────

static void handle_curr_prog_dump(const uint8_t * data, uint32_t length) {
    // Format: F0 42 3g 46 40 01 [7-bit encoded data...] F7
    // Payload starts at data[5] (skip header 4 bytes + func byte + 0x01 byte)
    if (length < 7) {
        LOG_ERROR("CURR_PROG_DUMP too short (%u)\n", (unsigned)length);
        return;
    }
    const uint8_t * payload    = data + 6;            // skip F0 42 3g 46 40 01
    uint32_t        payloadLen = length - 7;           // exclude leading 6 + trailing F7

    static uint8_t decoded[4096];
    uint32_t       decodedLen = decode_7to8(payload, payloadLen, decoded, sizeof(decoded));

    LOG_DEBUG("CURR_PROG_DUMP: %u MIDI bytes → %u decoded bytes\n",
              (unsigned)payloadLen, (unsigned)decodedLen);

    extract_prog_info(decoded, decodedLen);
}

static void handle_parameter_change(const uint8_t * data, uint32_t length) {
    // Format: F0 42 3g 46 41 0mm pp pp vv vv F7
    // group(m), paramLSB, paramMSB, valueLSB, valueMSB
    if (length < 10) {
        return;
    }
    uint8_t  group   = data[5] & 0x0F;
    uint16_t paramId = (uint16_t)(data[6] | ((uint16_t)(data[7] & 0x7F) << 7));
    uint16_t value   = (uint16_t)(data[8] | ((uint16_t)(data[9] & 0x7F) << 7));

    LOG_DEBUG("PARAM_CHANGE group=%u param=%u value=%u\n",
              (unsigned)group, (unsigned)paramId, (unsigned)value);

    if ((group == Z1_PARAM_GROUP_PROG) && (paramId >= 1) && (paramId <= Z1_PROG_NAME_LEN)) {
        // Single character of the program name changed
        char c = (char)(value & 0x7F);
        gDevice.progName[paramId - 1] = ((c >= 0x20) && (c <= 0x7F)) ? c : '?';
        gDevice.progName[Z1_PROG_NAME_LEN] = '\0';
        LOG_DEBUG("Program name updated: \"%s\"\n", gDevice.progName);
    }
}

// ── Public API ────────────────────────────────────────────────────────────────

void z1_on_connected(void) {
    LOG_DEBUG("Z1 connected (channel byte 0x%02X)\n", Z1_SYSEX_CHANNEL_BYTE(gDevice.id));
    memset(gDevice.progName, 0, sizeof(gDevice.progName));
    gDevice.category     = 0;
    gDevice.voiceMode    = 2;    // default POLY
    gDevice.unisonOn     = false;
    gDevice.unisonType   = 0;
    gDevice.unisonDetune = 0;
    gDevice.filter1Cutoff = 64;
    atomic_store(&gReDraw, true);
    z1_request_current_program();
}

void z1_request_current_program(void) {
    // F0 42 3g 46 10 00 F7
    uint8_t msg[7];
    uint32_t pos = build_header(msg, Z1_FUNC_CURR_PROG_DUMP_REQ);
    msg[pos++] = 0x00;
    msg[pos++] = MIDI_SYSEX_END;
    midi_send(msg, pos);
    LOG_DEBUG("Sent CURR_PROG_DUMP_REQ\n");
}

void z1_send_parameter_change(uint8_t group, uint16_t paramId, uint16_t value) {
    // F0 42 3g 46 41 0mm pp pp vv vv F7
    uint8_t msg[11];
    uint32_t pos = build_header(msg, Z1_FUNC_PARAMETER_CHANGE);
    msg[pos++] = (uint8_t)(group & 0x0F);
    msg[pos++] = (uint8_t)(paramId & 0x7F);
    msg[pos++] = (uint8_t)((paramId >> 7) & 0x7F);
    msg[pos++] = (uint8_t)(value & 0x7F);
    msg[pos++] = (uint8_t)((value >> 7) & 0x7F);
    msg[pos++] = MIDI_SYSEX_END;
    midi_send(msg, pos);
}

void z1_handle_message(const uint8_t * data, uint32_t length) {
    if (!is_z1_sysex(data, length)) {
        LOG_DEBUG("Ignoring non-Z1 SysEx (len=%u)\n", (unsigned)length);
        return;
    }
    uint8_t funcId = data[4];
    LOG_DEBUG("Z1 SysEx func=0x%02X len=%u\n", (unsigned)funcId, (unsigned)length);

    switch (funcId) {
    case Z1_FUNC_CURR_PROG_DUMP:
        handle_curr_prog_dump(data, length);
        break;
    case Z1_FUNC_PARAMETER_CHANGE:
        handle_parameter_change(data, length);
        break;
    case Z1_FUNC_DATA_LOAD_COMPLETED:
        LOG_DEBUG("Data load completed\n");
        break;
    case Z1_FUNC_DATA_LOAD_ERROR:
        LOG_ERROR("Data load error\n");
        break;
    case Z1_FUNC_WRITE_COMPLETED:
        LOG_DEBUG("Write completed\n");
        break;
    case Z1_FUNC_WRITE_ERROR:
        LOG_ERROR("Write error\n");
        break;
    case Z1_FUNC_DATA_FORMAT_ERROR:
        LOG_ERROR("Data format error\n");
        break;
    default:
        LOG_DEBUG("Z1 unhandled func 0x%02X\n", (unsigned)funcId);
        break;
    }
    atomic_store(&gReDraw, true);
}
