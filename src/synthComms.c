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
#include "synthBackup.h"

// ── SysEx header helpers ──────────────────────────────────────────────────────
// Header shape: F0 <manufacturerId: 1 or 3 bytes> <0x30|channel> <familyId>
// <func> ... F7. manufacturerIdLen (1 for a classic ID like Korg's 0x42, 3 for
// an extended one like Novation's) shifts every offset after it — nothing
// here hardcodes "1 byte", so a device with either length works unchanged.

static bool is_synth_sysex(const uint8_t * data, uint32_t length) {
    tPanelConfig * cfg = synth_panel_config();
    uint32_t       n   = cfg->manufacturerIdLen;

    if (length < (uint32_t)(4 + n)) { // F0 + mfr(n) + channel + familyId
        return false;
    }
    return (data[0] == MIDI_SYSEX_START)
           && (memcmp(&data[1], cfg->manufacturerId, n) == 0)
           && ((data[n + 1] & 0xF0) == 0x30)
           && (data[n + 2] == cfg->familyId);
}

// Moog's own dump SysEx header shape (see moogStyleDump in panelConfig.h):
// F0 <manufacturerId> <productId> <deviceId> <mode> ... F7 — nothing like the
// Korg-style header above (no "0x30|channel" byte, no familyId in that
// position). deviceId isn't checked here — it's whatever the unit's own
// front-panel SysEx ID is set to, not something this app assigns.
static bool is_moog_sysex(const uint8_t * data, uint32_t length) {
    tPanelConfig * cfg = synth_panel_config();

    if (length < 5) { // F0 + mfrId(1) + productId + deviceId + mode
        return false;
    }
    return (data[0] == MIDI_SYSEX_START)
           && (data[1] == cfg->manufacturerId[0])
           && (data[2] == cfg->productId);
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
    uint32_t       pos = 0;

    buf[pos++] = MIDI_SYSEX_START;

    for (uint32_t b = 0; b < cfg->manufacturerIdLen; b++) {
        buf[pos++] = cfg->manufacturerId[b];
    }

    buf[pos++] = SYNTH_SYSEX_CHANNEL_BYTE(gDevice.id);
    buf[pos++] = (uint8_t)cfg->familyId;
    buf[pos++] = funcId;
    return pos;
}

// ── Generic wire value application ───────────────────────────────────────────
// Applies a raw value received off the wire (either a decoded program-dump
// byte or a parameter-change value) to a dial's own storage. If the dial
// pairs a native value, `rawValue` is the native representation and the
// storage/CC value is derived from it; otherwise `rawValue` is the storage
// value directly, clamped to the dial's own [storageOffset,
// storageOffset+max-1] range. No per-dial knowledge here — it's all driven
// by the dial's fields, which is what lets any device's dials (not just the
// ones a particular <device>.txt happens to declare) go through this
// unchanged.
static void apply_dial_wire_value(tPanelDial * dial, uint32_t rawValue) {
    if (!dial) {
        return;
    }

    if (dial->nativeMax != 0) {
        uint32_t native = (rawValue <= dial->nativeMax) ? rawValue : dial->nativeMax;

        dial->nativeValue = (uint8_t)native;
        dial->value       = (dial->max > 1)
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
        // dial->value is uint32_t (wide enough for a 14-bit CC pair or a
        // 16-bit Moog dump field, per its own field comment in
        // panelConfig.h) — truncating to uint8_t here was a latent bug that
        // never bit anything before now: Z1's Korg-style dump values and
        // parameter-change values are always <=255, and the CC-pair path in
        // synth_handle_cc() below sets dial->value directly rather than
        // going through this function. Moog's 16-bit bit-packed dump fields
        // are the first caller that actually needs the width.
        dial->value = (uint32_t)v;
    }
}

// ── Program info extraction ───────────────────────────────────────────────────
// Everything about what a decoded program dump contains — name length, every
// other field's byte offset/bit-packing/native scaling — comes from the
// device's own <device>.txt (progNameLen, and each dial's dumpOffset/
// dumpShift/dumpMask/nativeMax); nothing device-specific lives here. Category,
// voice mode, unison and the like aren't special-cased: on the Z1 they're
// just dials in a `hidden` section (rendered as plain text rather than a
// circular control — see synth_render()), wired up exactly like Filter's or
// Oscillator's dials.
static void extract_prog_info(const uint8_t * decoded, uint32_t decodedLen) {
    tPanelConfig * cfg     = synth_panel_config();
    uint32_t       nameLen = (decodedLen >= cfg->progNameLen) ? cfg->progNameLen : decodedLen;

    if (nameLen >= sizeof(gDevice.progName)) {
        nameLen = sizeof(gDevice.progName) - 1;
    }
    uint32_t       i;

    for (i = 0; i < nameLen; i++) {
        char c = (char)decoded[i];
        gDevice.progName[i] = ((c >= 0x20) && (c <= 0x7F)) ? c : '?';
    }

    while ((i > 0) && (gDevice.progName[i - 1] == ' ')) {
        i--;
    }
    gDevice.progName[i] = '\0';

    // Every other value from the decoded program dump — a full-dump byte
    // buffer, a different wire format from param= change messages. Byte
    // offsets, bit-packing and native/CC scaling all come from the device's
    // own <device>.txt (dumpOffset/dumpShift/dumpMask/nativeMax); no per-dial
    // knowledge lives here. Every section gets scanned, hidden or not.
    uint32_t       updated = 0;

    for (uint32_t s = 0; s < cfg->sectionCount; s++) {
        tPanelSection * dumpSection = &cfg->sections[s];

        for (uint32_t d = 0; d < dumpSection->dialCount; d++) {
            tPanelDial * dial = &dumpSection->dials[d];

            if ((dial->dumpOffset >= 0) && (decodedLen > (uint32_t)dial->dumpOffset)) {
                uint32_t raw = (decoded[dial->dumpOffset] >> dial->dumpShift) & dial->dumpMask;
                apply_dial_wire_value(dial, raw);
                updated++;
            }
        }
    }

    LOG_DEBUG("Synth prog: \"%s\" — %u dial(s) updated from dump\n", gDevice.progName, (unsigned)updated);
}

// ── Moog-style bit-packed dump extraction ─────────────────────────────────────
// Moog's Panel/Preset Dump SysEx packs values as a continuous bitstream, 7
// usable bits per byte (byte's bit 6 done -> next byte's bit 0), rather than
// Korg's one-value-per-byte layout above. A dial opts into this by setting
// dumpBitWidth > 0 (see panelConfig.h); dumpOffset is still its first byte,
// dumpBitOffset (0-6) is which bit of that byte holds the value's LSB.
// Confirmed byte-for-byte against real Voyager hardware (2026-07-06): set
// Filter Cutoff/Resonance to known CC values, requested a Panel Dump,
// decoded — exact match both times.
static uint32_t read_bitpacked_field(const uint8_t * payload, uint32_t payloadLen,
                                     int32_t byteOffset, uint32_t bitOffset, uint32_t bitWidth) {
    uint32_t value       = 0;
    uint32_t globalStart = (uint32_t)byteOffset * 7 + bitOffset;

    for (uint32_t k = 0; k < bitWidth; k++) {
        uint32_t globalBit = globalStart + k;
        uint32_t byteIdx   = globalBit / 7;
        uint32_t column    = globalBit % 7;

        if (byteIdx >= payloadLen) {
            break; // truncated capture — leave remaining (higher) bits at 0
        }
        uint32_t bit       = (payload[byteIdx] >> column) & 1;
        value |= bit << k;
    }

    return value;
}

// Same role as extract_prog_info() above, but for Moog's bit-packed dump
// shape — every dial with dumpBitWidth > 0 gets its value from
// read_bitpacked_field() instead of the single-byte dumpShift/dumpMask path.
// No prog-name handling (Moog's Panel Dump isn't tied to a named preset slot
// the way a Single Preset Dump is — see the format's own spec comment in
// voyager.txt).
static void extract_moog_panel_info(const uint8_t * payload, uint32_t payloadLen) {
    tPanelConfig * cfg     = synth_panel_config();
    uint32_t       updated = 0;

    for (uint32_t s = 0; s < cfg->sectionCount; s++) {
        tPanelSection * section = &cfg->sections[s];

        for (uint32_t d = 0; d < section->dialCount; d++) {
            tPanelDial * dial = &section->dials[d];

            if (dial->dumpBitWidth == 0) {
                continue;
            }
            uint32_t     raw  = read_bitpacked_field(payload, payloadLen, dial->dumpOffset,
                                                     dial->dumpBitOffset, dial->dumpBitWidth);
            apply_dial_wire_value(dial, raw);
            updated++;
        }
    }

    LOG_DEBUG("Moog panel dump: %u dial(s) updated (%u payload bytes)\n",
              (unsigned)updated, (unsigned)payloadLen);
}

// ── Message handlers ──────────────────────────────────────────────────────────

static void handle_curr_prog_dump(const uint8_t * data, uint32_t length) {
    // Format: F0 <mfrId> 3g 46 40 01 [7-bit encoded data...] F7
    // Payload starts right after the header (F0+mfrId+chan+fam+func = 4+n
    // bytes) plus the extra "01" sub-byte this dump function has.
    synth_backup_capture_dump(data, length, eBackupExpectLive); // no-op unless a live-panel Backup is pending — see synthBackup.c
    tPanelConfig *  cfg        = synth_panel_config();
    uint32_t        skip       = 5 + cfg->manufacturerIdLen;

    if (length < skip + 1) {
        LOG_ERROR("CURR_PROG_DUMP too short (%u)\n", (unsigned)length);
        return;
    }
    const uint8_t * payload    = data + skip;         // skip header + func + 0x01
    uint32_t        payloadLen = length - skip - 1;   // exclude leading skip + trailing F7

    static uint8_t  decoded[4096];
    uint32_t        decodedLen = decode_7to8(payload, payloadLen, decoded, sizeof(decoded));

    LOG_DEBUG("CURR_PROG_DUMP: %u MIDI bytes → %u decoded bytes\n",
              (unsigned)payloadLen, (unsigned)decodedLen);

    extract_prog_info(decoded, decodedLen);
}

// Format: F0 <mfrId(1)> <productId> <deviceId> <mode> <payload...> F7 — see
// moogStyleDump in panelConfig.h. No 7-to-8 decode needed here (unlike Korg's
// handle_curr_prog_dump() above) — Moog's payload bytes are already usable
// directly, each one individually 7-bit safe rather than 8 groups of 7 real
// data bytes plus an MSB-collector byte.
//
// skip is 1 (just F0), NOT 5 (F0+mfrId+productId+deviceId+mode) — every
// dumpOffset/dumpBitOffset in voyager.txt was derived directly from "Voyager
// System Exclusive Panel Dump Format"'s OWN byte numbering, which counts
// "byte 1" as the manufacturer ID itself (immediately after F0), so mfrId/
// productId/deviceId/mode are bytes 1-4 of that scheme, not header bytes to
// be skipped before it starts. Using skip=5 here silently shifted every
// field read 4 bytes deeper into the stream than every dumpOffset assumed —
// found by setting Cutoff/Resonance/Spacing/KB Amount all to hardware
// maximum and noticing two of the four dials still showed old, unrelated
// values (reading into a neighboring field's bytes) while the other two
// only happened to look right because a neighboring field was also
// coincidentally near-max at the time.
static void handle_moog_panel_dump(const uint8_t * data, uint32_t length) {
    synth_backup_capture_dump(data, length, eBackupExpectLive); // no-op unless a live-panel Backup is pending — see synthBackup.c
    const uint32_t  skip       = 1;                             // F0 only

    if (length < skip + 1) {
        LOG_ERROR("Moog panel dump too short (%u)\n", (unsigned)length);
        return;
    }
    const uint8_t * payload    = data + skip;
    uint32_t        payloadLen = length - skip - 1; // exclude trailing F7

    extract_moog_panel_info(payload, payloadLen);
}

// Decodes a Single Preset Dump's name field into gDevice.progName, if the
// device's file declared one (presetNameOffset >= 0 — see the tPanelConfig
// field comment in panelConfig.h). Each character is 8 bits read from the
// same continuous 7-bit-per-byte bitstream the numeric panel fields use
// (read_bitpacked_field() above), one after another starting at
// presetNameOffset/presetNameBitOffset.
//
// Reverse-engineered against one real capture (Voyager preset 1, "FILTER
// BUBBLES"): the raw field turned out to be two fixed-width, space-padded
// lines — matching the Voyager's 2-line LCD — with one non-ASCII byte
// (0xA0, a space with the high bit set) marking the line break, not a
// single contiguous name string. Rather than also declaring where that
// break falls (which would only make this MORE Voyager-specific), every
// non-printable byte is treated as whitespace and runs of whitespace are
// collapsed to one space — turning "FILTER     <0xA0>BUBBLES " back into
// "FILTER BUBBLES" generically, for any device whose name field turns out
// to follow the same padded-lines shape.
static void extract_moog_preset_name(const uint8_t * payload, uint32_t payloadLen) {
    tPanelConfig * cfg          = synth_panel_config();

    if ((cfg->presetNameOffset < 0) || (cfg->presetNameLen == 0)) {
        return;
    }
    uint32_t       globalBit    = (uint32_t)cfg->presetNameOffset * 7 + cfg->presetNameBitOffset;
    uint32_t       outLen       = 0;
    bool           lastWasSpace = false;

    for (uint32_t i = 0; (i < cfg->presetNameLen) && (outLen < sizeof(gDevice.progName) - 1); i++) {
        uint32_t byteOffset = globalBit / 7;
        uint32_t bitOffset  = globalBit % 7;
        uint32_t raw        = read_bitpacked_field(payload, payloadLen, (int32_t)byteOffset, bitOffset, 8);
        bool     printable  = (raw >= 0x20) && (raw < 0x7F);

        if (printable) {
            gDevice.progName[outLen++] = (char)raw;
            lastWasSpace               = false;
        } else if (!lastWasSpace && (outLen > 0)) {
            gDevice.progName[outLen++] = ' ';
            lastWasSpace               = true;
        }
        globalBit += 8;
    }

    while ((outLen > 0) && (gDevice.progName[outLen - 1] == ' ')) {
        outLen--;
    }
    gDevice.progName[outLen] = '\0';
    LOG_DEBUG("Preset name: \"%s\"\n", gDevice.progName);
}

// Format: F0 <mfrId> <productId> <deviceId> 03 <payload...> F7 — the reply to
// synth_request_single_preset_dump()'s mode 0x06 request.
static void handle_moog_single_preset_dump(const uint8_t * data, uint32_t length) {
    const uint32_t  skip       = 1; // F0 only — see handle_moog_panel_dump()'s comment on why

    if (length < skip + 1) {
        LOG_ERROR("Moog single preset dump too short (%u)\n", (unsigned)length);
        return;
    }
    const uint8_t * payload    = data + skip;
    uint32_t        payloadLen = length - skip - 1; // exclude trailing F7

    extract_moog_preset_name(payload, payloadLen);
    synth_backup_capture_dump(data, length, eBackupExpectPreset); // no-op unless a by-number Backup is pending — see synthBackup.c; after the name decode so a by-number backup's default filename can use it
}

static void handle_parameter_change(const uint8_t * data, uint32_t length) {
    // Format: F0 <mfrId> 3g 46 41 0mm pp pp vv vv F7
    // group(m), paramLSB, paramMSB, valueLSB, valueMSB start right after the
    // header (F0+mfrId+chan+fam+func = 4+n bytes).
    tPanelConfig * cfg     = synth_panel_config();
    uint32_t       base    = 4 + cfg->manufacturerIdLen;

    if (length < base + 5) {
        return;
    }
    uint8_t        group   = data[base] & 0x0F;
    uint16_t       paramId = (uint16_t)(data[base + 1] | ((uint16_t)(data[base + 2] & 0x7F) << 7));
    uint16_t       value   = (uint16_t)(data[base + 3] | ((uint16_t)(data[base + 4] & 0x7F) << 7));

    LOG_DEBUG("PARAM_CHANGE group=%u param=%u value=%u\n",
              (unsigned)group, (unsigned)paramId, (unsigned)value);

    if (  (group == SYNTH_PARAM_GROUP_PROG) && (paramId >= 1) && (paramId <= cfg->progNameLen)
       && (paramId < sizeof(gDevice.progName))) {
        char c = (char)(value & 0x7F);
        gDevice.progName[paramId - 1]                                                                                   = ((c >= 0x20) && (c <= 0x7F)) ? c : '?';
        gDevice.progName[cfg->progNameLen < sizeof(gDevice.progName) ? cfg->progNameLen : sizeof(gDevice.progName) - 1] = '\0';
        LOG_DEBUG("Program name updated: \"%s\"\n", gDevice.progName);
    } else if (group == SYNTH_PARAM_GROUP_PROG) {
        // Generic dispatch: whichever dial (if any) is wired to this
        // group/paramId in xxxx.txt gets the value — no per-param knowledge
        // of what it controls lives here. Searches every section, hidden or
        // not (Filters, Oscillator's several sections, Z1's hidden
        // category/voice/unison section, ...).
        tPanelDial * dial = NULL;

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

    // Reset every dial, in every section, to its own display-space default
    // (0) — apply_dial_wire_value() already knows how to turn that into the
    // right storage/native representation per dial, so no per-field defaults
    // here; there's nothing device-specific left to reset in gDevice itself.
    tPanelConfig * cfg = synth_panel_config();

    for (uint32_t s = 0; s < cfg->sectionCount; s++) {
        tPanelSection * section = &cfg->sections[s];

        for (uint32_t d = 0; d < section->dialCount; d++) {
            apply_dial_wire_value(&section->dials[d], 0);
        }
    }

    gReDraw = true;
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

void synth_request_state_dump(void) {
    tPanelConfig * cfg = synth_panel_config();

    if (cfg->stateRequestSysExLen > 0) {
        midi_send(cfg->stateRequestSysEx, cfg->stateRequestSysExLen);
        LOG_DEBUG("Re-sent device state request (%u bytes)\n", (unsigned)cfg->stateRequestSysExLen);
    } else {
        synth_request_current_program();
    }
}

void synth_request_single_preset_dump(uint32_t presetNumber) {
    tPanelConfig * cfg       = synth_panel_config();

    if (!cfg->moogStyleDump) {
        LOG_ERROR("Single Preset Dump Request: not a Moog-style device\n");
        return;
    }

    if ((presetNumber < 1) || (presetNumber > 128) || (cfg->stateRequestSysExLen < 2)) {
        LOG_ERROR("Single Preset Dump Request: preset %u out of range\n", (unsigned)presetNumber);
        return;
    }
    // Built from stateRequestSysEx (the Panel Dump Request Moog-style header —
    // F0 <mfrId> <productId> <deviceId> 05 F7) rather than a second fixed
    // constant in the file: same header prefix, just mode 0x06 (Single Preset
    // Dump REQUEST) instead of 0x05, with the requested preset number
    // inserted before the trailing F7 — see "byte4 = <SysExMode>" in
    // voyager.txt's own header comment for where 0x06 + "program number byte"
    // comes from.
    uint8_t        msg[sizeof(cfg->stateRequestSysEx) + 2];
    uint32_t       prefixLen = cfg->stateRequestSysExLen - 1; // everything up to (not including) the trailing F7

    memcpy(msg, cfg->stateRequestSysEx, prefixLen);
    msg[prefixLen - 1] = 0x06;                        // mode byte: Single Preset Dump REQUEST
    msg[prefixLen]     = (uint8_t)(presetNumber - 1); // 0-based on the wire — see the header comment
    msg[prefixLen + 1] = MIDI_SYSEX_END;
    midi_send(msg, prefixLen + 2);
    LOG_DEBUG("Sent Single Preset Dump Request for preset %u\n", (unsigned)presetNumber);
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

bool synth_handle_cc(uint8_t cc, uint8_t value) {
    tPanelDial * dial = find_panel_dial_by_cc(synth_panel_config(), cc);

    if (!dial) {
        return false;
    }

    if (dial->ccLsbNumber == 0) {
        // A live CC message carries a raw 0-127 byte. For a plain continuous
        // dial (nativeMax == 0) that byte already IS the storage/display
        // value, so write it straight through. But some switches/selectors
        // are wired as a CC whose hardware only ever sends a handful of
        // evenly-spaced raw values across that range for their N positions
        // (nativeMax != 0, display == dialDisplayNames — e.g. Voyager's
        // Glide switch: CC65, 0-63=Off, 64-127=On) — those need the same
        // native->display quantization apply_dial_wire_value() already does
        // for SysEx-sourced raw values, or every position past the first
        // would render as "?" (index >= nameCount).
        apply_dial_wire_value(dial, value);
    } else {
        // 14-bit CC pair (MIDI's own coarse/fine convention: controller N is
        // the MSB, N+32 the LSB — see the ccLsbNumber comment in
        // panelConfig.h). Each half arrives as its own ordinary CC message,
        // so latch whichever half just came in and recombine against the
        // other half's last-known value.
        if (cc == dial->ccNumber) {
            dial->ccMsbLatched = value;
        } else {
            dial->ccLsbLatched = value;
        }
        dial->value = ((uint32_t)dial->ccMsbLatched << 7) | dial->ccLsbLatched;
    }
    return true;
}

void synth_handle_message(const uint8_t * data, uint32_t length) {
    if (synth_panel_config()->moogStyleDump) {
        // Entirely separate header shape and dispatch from the Korg-style
        // path below (see is_moog_sysex()/handle_moog_panel_dump()). Modes
        // 0x02 (Panel Dump) and 0x03 (Single Preset Dump) are the two replies
        // this app requests (see stateRequestSysEx/"Panel Dump Request" and
        // synth_request_single_preset_dump() respectively) — anything else is
        // logged and ignored.
        if (!is_moog_sysex(data, length)) {
            LOG_DEBUG("Ignoring non-target SysEx (len=%u)\n", (unsigned)length);
            return;
        }
        uint8_t mode = data[4];

        if (mode == 0x02) {
            handle_moog_panel_dump(data, length);
        } else if (mode == 0x03) {
            handle_moog_single_preset_dump(data, length);
        } else {
            LOG_DEBUG("Moog SysEx unhandled mode 0x%02X\n", (unsigned)mode);
        }
        gReDraw = true;
        return;
    }

    if (!is_synth_sysex(data, length)) {
        LOG_DEBUG("Ignoring non-target SysEx (len=%u)\n", (unsigned)length);
        return;
    }
    uint8_t funcId = data[3 + synth_panel_config()->manufacturerIdLen];
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

void synth_set_panel_dial_value(tPanelDial * dial, uint32_t displayValue) {
    if (!dial) {
        return;
    }

    if ((dial->max > 0) && (displayValue >= dial->max)) {
        displayValue = dial->max - 1;
    }
    uint32_t storageValue = (uint32_t)((int32_t)displayValue + dial->storageOffset);

    if (storageValue == dial->value) {
        return;
    }
    dial->value = storageValue;

    if (dial->ccNumber != 0) {
        if ((dial->nativeMax != 0) && (dial->max > 1)) {
            dial->nativeValue = (uint8_t)(displayValue * dial->nativeMax / (dial->max - 1));
        }

        if (dial->ccLsbNumber != 0) {
            // 14-bit CC pair — see the ccLsbNumber comment in panelConfig.h.
            // Keep the latches in sync so a later single-half incoming update
            // recombines against the half we just sent, not a stale one.
            dial->ccMsbLatched = (uint8_t)((storageValue >> 7) & 0x7F);
            dial->ccLsbLatched = (uint8_t)(storageValue & 0x7F);
            midi_send_cc(gDevice.id, (uint8_t)dial->ccNumber, dial->ccMsbLatched);
            midi_send_cc(gDevice.id, (uint8_t)dial->ccLsbNumber, dial->ccLsbLatched);
        } else {
            // nativeMax != 0: the hardware expects its own scaled raw byte
            // for this display position (e.g. Glide's Off/On sends 0/127,
            // not 0/1) — nativeValue above was just computed for exactly
            // this. Without nativeMax, storageValue already IS that byte
            // (plain continuous CC dial).
            uint8_t wireValue = (dial->nativeMax != 0) ? dial->nativeValue : (uint8_t)storageValue;
            midi_send_cc(gDevice.id, (uint8_t)dial->ccNumber, wireValue);
        }
    } else {
        synth_send_parameter_change((uint8_t)dial->paramGroup, (uint16_t)dial->paramId, (uint8_t)storageValue);
    }
    gReDraw = true;
}
