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

#include <stdint.h>
#include <string.h>

#include "defs.h"
#include "synthlibDefs.h"
#include "types.h"
#include "globalVars.h"
#include "midiComms.h"
#include "synthGraphics.h"
#include "synthComms.h"

// ── SysEx header helpers ──────────────────────────────────────────────────────

// Minimum length of a valid synth SysEx: F0 42 3g 46 <func> F7
#define SYNTH_HDR_LEN    5

static bool is_synth_sysex(const uint8_t * data, uint32_t length) {
    if (length < SYNTH_HDR_LEN) {
        return false;
    }
    tPanelConfig * cfg = synth_panel_config();

    return (data[0] == MIDI_SYSEX_START)
           && (data[1] == cfg->manufacturerId)
           && ((data[2] & 0xF0) == 0x30)
           && (data[3] == cfg->familyId);
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
        uint8_t  msbs    = 0;
        uint32_t groupSz = (dataLen - i >= 7) ? 7 : (dataLen - i);

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

// ── Build outgoing synth SysEx header ───────────────────────────────────────────
static uint32_t build_header(uint8_t * buf, uint8_t funcId) {
    tPanelConfig * cfg = synth_panel_config();

    buf[0] = MIDI_SYSEX_START;
    buf[1] = (uint8_t)cfg->manufacturerId;
    buf[2] = SYNTH_SYSEX_CHANNEL_BYTE(gDevice.id);
    buf[3] = (uint8_t)cfg->familyId;
    buf[4] = funcId;
    return 5;
}

// ── Generic wire value application ───────────────────────────────────────────
// Applies a raw value received off the wire (either a decoded program-dump
// byte or a parameter-change value) to a bound dial. If the dial pairs a
// native value, `rawValue` is the native representation and the storage/CC
// value is derived from it; otherwise `rawValue` is the storage value
// directly, clamped to the dial's own [storageOffset, storageOffset+max-1]
// range. No per-dial knowledge here — it's all driven by the dial's fields.
static void apply_dial_wire_value(tPanelDial * dial, uint32_t rawValue) {
    if (!dial || !dial->valuePtr) {
        return;
    }

    if (dial->nativeValuePtr && (dial->nativeMax != 0)) {
        uint32_t native = (rawValue <= dial->nativeMax) ? rawValue : dial->nativeMax;

        *dial->nativeValuePtr = (uint8_t)native;
        *dial->valuePtr       = (dial->max > 1)
                                ? (uint8_t)(((native * (dial->max - 1)) + (dial->nativeMax / 2)) / dial->nativeMax)
                                : 0;
    } else {
        int32_t lo = dial->storageOffset;
        int32_t hi = dial->storageOffset + (int32_t)dial->max - 1;
        int32_t v  = (int32_t)rawValue;

        if (v < lo) {
            v = lo;
        }

        if (v > hi) {
            v = hi;
        }
        *dial->valuePtr = (uint8_t)v;
    }
}

// ── Program info extraction ───────────────────────────────────────────────────
// Byte layout (originally derived from Korg Z1 MIDI spec parameter table, packed fields noted):
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
    uint32_t nameLen = (decodedLen >= SYNTH_PROG_NAME_LEN) ? SYNTH_PROG_NAME_LEN : decodedLen;
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

        if (gDevice.category >= get_panel_list_count(synth_panel_config(), "category")) {
            gDevice.category = 0;
        }
    }

    // Voice assign mode (byte 18, bits 3-4)
    if (decodedLen > 18) {
        gDevice.voiceMode = (decoded[18] >> 3) & 0x03;
    }

    // Unison (byte 21: bits 0-1 = type, bit 2 = SW; byte 22 = detune)
    if (decodedLen > 21) {
        gDevice.unisonType = decoded[21] & 0x03;
        gDevice.unisonOn   = (decoded[21] >> 2) & 0x01;
    }

    if (decodedLen > 22) {
        gDevice.unisonDetune = decoded[22];
    }
    // Values from decoded program dump — a full-dump byte buffer, a different
    // wire format from param= change messages. Byte offsets, bit-packing and
    // native/CC scaling all come from synth.txt (dumpOffset/dumpShift/
    // dumpMask/nativeMax); no per-dial knowledge lives here. Every section
    // gets scanned, not just Filters — Oscillator's dials live in their own
    // sections (oscCommon/osc1/osc2/subOsc/noise/mixer) alongside Filters.
    tPanelConfig *  cfg     = synth_panel_config();

    for (uint32_t s = 0; s < cfg->sectionCount; s++) {
        tPanelSection * dumpSection = &cfg->sections[s];

        for (uint32_t d = 0; d < dumpSection->dialCount; d++) {
            tPanelDial * dial = &dumpSection->dials[d];

            if ((dial->dumpOffset >= 0) && (decodedLen > (uint32_t)dial->dumpOffset)) {
                uint32_t raw = (decoded[dial->dumpOffset] >> dial->dumpShift) & dial->dumpMask;
                apply_dial_wire_value(dial, raw);
            }
        }
    }

    tPanelSection * section = synth_filters_section();
    tPanelDial *    f1type  = section ? find_panel_dial(section, "f1type") : NULL;
    tPanelDial *    f2type  = section ? find_panel_dial(section, "f2type") : NULL;
    tPanelDial *    f1cut   = section ? find_panel_dial(section, "f1cut") : NULL;
    tPanelDial *    f1res   = section ? find_panel_dial(section, "f1res") : NULL;
    tPanelDial *    f2cut   = section ? find_panel_dial(section, "f2cut") : NULL;
    tPanelDial *    f2res   = section ? find_panel_dial(section, "f2res") : NULL;
    uint32_t        f1tVal  = f1type ? get_panel_dial_value(f1type) : 0;
    uint32_t        f2tVal  = f2type ? get_panel_dial_value(f2type) : 0;

    LOG_DEBUG("Synth prog: \"%s\"  cat=%s  voice=%s  unison=%s(%u cents)"
              "  f1type=%s f1cut=%u(%u) f1res=%u(%u)"
              "  f2type=%s f2cut=%u(%u) f2res=%u(%u)\n",
              gDevice.progName,
              get_panel_list_item(cfg, "category", gDevice.category),
              get_panel_list_item(cfg, "voiceMode", gDevice.voiceMode),
              gDevice.unisonOn ? get_panel_list_item(cfg, "unisonType", gDevice.unisonType - 1) : "OFF",
              (unsigned)gDevice.unisonDetune,
              (f1type && (f1tVal < f1type->nameCount)) ? f1type->names[f1tVal] : "?",
              (unsigned)get_panel_dial_value(f1cut), (unsigned)get_panel_dial_native_value(f1cut),
              (unsigned)get_panel_dial_value(f1res), (unsigned)get_panel_dial_native_value(f1res),
              (f2type && (f2tVal < f2type->nameCount)) ? f2type->names[f2tVal] : "?",
              (unsigned)get_panel_dial_value(f2cut), (unsigned)get_panel_dial_native_value(f2cut),
              (unsigned)get_panel_dial_value(f2res), (unsigned)get_panel_dial_native_value(f2res));
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
    uint32_t        payloadLen = length - 7;          // exclude leading 6 + trailing F7

    static uint8_t  decoded[4096];
    uint32_t        decodedLen = decode_7to8(payload, payloadLen, decoded, sizeof(decoded));

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

    if ((group == SYNTH_PARAM_GROUP_PROG) && (paramId >= 1) && (paramId <= SYNTH_PROG_NAME_LEN)) {
        char c = (char)(value & 0x7F);
        gDevice.progName[paramId - 1]         = ((c >= 0x20) && (c <= 0x7F)) ? c : '?';
        gDevice.progName[SYNTH_PROG_NAME_LEN] = '\0';
        LOG_DEBUG("Program name updated: \"%s\"\n", gDevice.progName);
    } else if (group == SYNTH_PARAM_GROUP_PROG) {
        // Generic dispatch: whichever dial (if any) is wired to this
        // group/paramId in xxxx.txt gets the value — no per-param knowledge
        // of what it controls lives here. Searches every section (Filters,
        // Oscillator's several sections, ...), not just Filters.
        tPanelConfig * cfg  = synth_panel_config();
        tPanelDial *   dial = NULL;

        for (uint32_t s = 0; (s < cfg->sectionCount) && !dial; s++) {
            dial = find_panel_dial_by_param(&cfg->sections[s], group, paramId);
        }

        if (dial) {
            apply_dial_wire_value(dial, value);
            LOG_DEBUG("Param %u (%s) = %u\n", (unsigned)paramId, dial->label, (unsigned)value);
        }
    }
}

// ── Public API ────────────────────────────────────────────────────────────────

void synth_on_connected(void) {
    LOG_DEBUG("Synth connected (channel byte 0x%02X)\n", SYNTH_SYSEX_CHANNEL_BYTE(gDevice.id));
    memset(gDevice.progName, 0, sizeof(gDevice.progName));
    gDevice.category     = 0;
    gDevice.voiceMode    = 2; // default POLY
    gDevice.unisonOn     = false;
    gDevice.unisonType   = 0;
    gDevice.unisonDetune = 0;

    // Reset every bound dial, in every section, to its display-space default
    // (0) — apply_dial_wire_value() already knows how to turn that into the
    // right storage/native representation per dial, so no per-field defaults
    // here.
    tPanelConfig * cfg = synth_panel_config();

    for (uint32_t s = 0; s < cfg->sectionCount; s++) {
        tPanelSection * section = &cfg->sections[s];

        for (uint32_t d = 0; d < section->dialCount; d++) {
            apply_dial_wire_value(&section->dials[d], 0);
        }
    }

    gReDraw              = true;
    synth_request_current_program();
}

void synth_request_current_program(void) {
    // F0 42 3g 46 10 00 F7
    uint8_t  msg[7];
    uint32_t pos = build_header(msg, SYNTH_FUNC_CURR_PROG_DUMP_REQ);

    msg[pos++] = 0x00;
    msg[pos++] = MIDI_SYSEX_END;
    midi_send(msg, pos);
    LOG_DEBUG("Sent CURR_PROG_DUMP_REQ\n");
}

void synth_send_parameter_change(uint8_t group, uint16_t paramId, uint16_t value) {
    // F0 42 3g 46 41 0mm pp pp vv vv F7
    uint8_t  msg[11];
    uint32_t pos = build_header(msg, SYNTH_FUNC_PARAMETER_CHANGE);

    msg[pos++] = (uint8_t)(group & 0x0F);
    msg[pos++] = (uint8_t)(paramId & 0x7F);
    msg[pos++] = (uint8_t)((paramId >> 7) & 0x7F);
    msg[pos++] = (uint8_t)(value & 0x7F);
    msg[pos++] = (uint8_t)((value >> 7) & 0x7F);
    msg[pos++] = MIDI_SYSEX_END;
    midi_send(msg, pos);
}

void synth_handle_message(const uint8_t * data, uint32_t length) {
    if (!is_synth_sysex(data, length)) {
        LOG_DEBUG("Ignoring non-target SysEx (len=%u)\n", (unsigned)length);
        return;
    }
    uint8_t funcId = data[4];
    LOG_DEBUG("Synth SysEx func=0x%02X len=%u\n", (unsigned)funcId, (unsigned)length);

    switch (funcId) {
        case SYNTH_FUNC_CURR_PROG_DUMP:
            handle_curr_prog_dump(data, length);
            break;
        case SYNTH_FUNC_PARAMETER_CHANGE:
            handle_parameter_change(data, length);
            break;
        case SYNTH_FUNC_DATA_LOAD_COMPLETED:
            LOG_DEBUG("Data load completed\n");
            break;
        case SYNTH_FUNC_DATA_LOAD_ERROR:
            LOG_ERROR("Data load error\n");
            break;
        case SYNTH_FUNC_WRITE_COMPLETED:
            LOG_DEBUG("Write completed\n");
            break;
        case SYNTH_FUNC_WRITE_ERROR:
            LOG_ERROR("Write error\n");
            break;
        case SYNTH_FUNC_DATA_FORMAT_ERROR:
            LOG_ERROR("Data format error\n");
            break;
        default:
            LOG_DEBUG("Synth unhandled func 0x%02X\n", (unsigned)funcId);
            break;
    }
    gReDraw = true;
}

// ── Panel dial <-> gDevice binding ────────────────────────────────────────────
// The only place that still knows a dial id like "f1cut" means
// gDevice.filter1Cutoff — everything else (mouseHandle.c, rendering) works
// generically off the tPanelDial it was resolved into.
void synth_bind_panel_dials(tPanelSection * section) {
    if (!section) {
        return;
    }
    struct {
        const char * id;
        uint8_t *    value;
        uint8_t *    native;
    } bindings[] = {
        {"route",       &gDevice.filterRouting,        NULL                        },
        {"f2link",      &gDevice.filter2Link,          NULL                        },
        {"f1type",      &gDevice.filter1Type,          NULL                        },
        {"f1trim",      &gDevice.filter1InputTrim,     NULL                        },
        {"f1cut",       &gDevice.filter1Cutoff,        &gDevice.filter1CutoffNative},
        {"f1res",       &gDevice.filter1Resonance,     &gDevice.filter1ResNative   },
        {"f2type",      &gDevice.filter2Type,          NULL                        },
        {"f2trim",      &gDevice.filter2InputTrim,     NULL                        },
        {"f2cut",       &gDevice.filter2Cutoff,        &gDevice.filter2CutoffNative},
        {"f2res",       &gDevice.filter2Resonance,     &gDevice.filter2ResNative   },
        // Oscillator section (oscCommon/osc1/osc2/subOsc/noise/mixer) — same
        // "id in the file means this gDevice field" convention as Filters
        // above. find_panel_dial() below is a no-op for any id not present in
        // whichever section is passed in, so it's harmless to list every
        // oscillator id here even though this function is called once per
        // section, not once for the whole device.
        {"pbIntPlus",   &gDevice.pitchBendIntPlus,     NULL                        },
        {"pbIntMinus",  &gDevice.pitchBendIntMinus,    NULL                        },
        {"pbStepPlus",  &gDevice.pitchBendStepPlus,    NULL                        },
        {"pbStepMinus", &gDevice.pitchBendStepMinus,   NULL                        },
        {"portSW",      &gDevice.portamentoSW,         NULL                        },
        {"portMode",    &gDevice.portamentoMode,       NULL                        },
        {"portTime",    &gDevice.portamentoTime,       NULL                        },
        {"o1type",      &gDevice.osc1Type,             NULL                        },
        {"o1oct",       &gDevice.osc1Octave,           NULL                        },
        {"o1semi",      &gDevice.osc1SemiTone,         NULL                        },
        {"o1fine",      &gDevice.osc1FineTune,         NULL                        },
        {"o1freq",      &gDevice.osc1FreqOffset,       NULL                        },
        {"o2type",      &gDevice.osc2Type,             NULL                        },
        {"o2oct",       &gDevice.osc2Octave,           NULL                        },
        {"o2semi",      &gDevice.osc2SemiTone,         NULL                        },
        {"o2fine",      &gDevice.osc2FineTune,         NULL                        },
        {"o2freq",      &gDevice.osc2FreqOffset,       NULL                        },
        {"suboct",      &gDevice.subOscOctave,         NULL                        },
        {"subsemi",     &gDevice.subOscSemiTone,       NULL                        },
        {"subfine",     &gDevice.subOscFineTune,       NULL                        },
        {"subfreq",     &gDevice.subOscFreqOffset,     NULL                        },
        {"subwave",     &gDevice.subOscWaveForm,       NULL                        },
        {"noisetype",   &gDevice.noiseFilterType,      NULL                        },
        {"noisetrim",   &gDevice.noiseFilterTrim,      NULL                        },
        {"noisecut",    &gDevice.noiseFilterCutoff,    NULL                        },
        {"noiseres",    &gDevice.noiseFilterResonance, NULL                        },
        {"mo1o1",       &gDevice.mixerOsc1Out1,        NULL                        },
        {"mo1o2",       &gDevice.mixerOsc1Out2,        NULL                        },
        {"mo2o1",       &gDevice.mixerOsc2Out1,        NULL                        },
        {"mo2o2",       &gDevice.mixerOsc2Out2,        NULL                        },
        {"msubo1",      &gDevice.mixerSubOut1,         NULL                        },
        {"msubo2",      &gDevice.mixerSubOut2,         NULL                        },
        {"mnoiseo1",    &gDevice.mixerNoiseOut1,       NULL                        },
        {"mnoiseo2",    &gDevice.mixerNoiseOut2,       NULL                        },
        {"mfbo1",       &gDevice.mixerFeedbackOut1,    NULL                        },
        {"mfbo2",       &gDevice.mixerFeedbackOut2,    NULL                        },
        {"mo1sw",       &gDevice.mixerOsc1SW,          NULL                        },
        {"mo2sw",       &gDevice.mixerOsc2SW,          NULL                        },
        {"msubsw",      &gDevice.mixerSubSW,           NULL                        },
        {"mnoisesw",    &gDevice.mixerNoiseSW,         NULL                        },
    };

    for (size_t i = 0; i < (sizeof(bindings) / sizeof(bindings[0])); i++) {
        tPanelDial * dial = find_panel_dial(section, bindings[i].id);

        if (dial) {
            dial->valuePtr       = bindings[i].value;
            dial->nativeValuePtr = bindings[i].native;
        }
    }
}

void synth_set_panel_dial_value(tPanelDial * dial, uint32_t displayValue) {
    if (!dial || !dial->valuePtr) {
        return;
    }

    if ((dial->max > 0) && (displayValue >= dial->max)) {
        displayValue = dial->max - 1;
    }
    uint8_t storageValue = (uint8_t)((int32_t)displayValue + dial->storageOffset);

    if (storageValue == *dial->valuePtr) {
        return;
    }
    *dial->valuePtr = storageValue;

    if (dial->ccNumber != 0) {
        if (dial->nativeValuePtr && (dial->nativeMax != 0) && (dial->max > 1)) {
            *dial->nativeValuePtr = (uint8_t)(displayValue * dial->nativeMax / (dial->max - 1));
        }
        midi_send_cc(gDevice.id, (uint8_t)dial->ccNumber, storageValue);
    } else {
        synth_send_parameter_change((uint8_t)dial->paramGroup, (uint16_t)dial->paramId, storageValue);
    }
    gReDraw         = true;
}
