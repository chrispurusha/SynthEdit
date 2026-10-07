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
// Notes: Docs/code-notes/synthComms.c.md - "// notes §k" refers there.

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include "defs.h"
#include "synthlibDefs.h"
#include "types.h"
#include "globalVars.h"
#include "midiComms.h"
#include "synthGraphics.h"
#include "synthComms.h"
#include "synthBackup.h"

// notes §1

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

// notes §2
static bool is_moog_sysex(const uint8_t * data, uint32_t length) {
    tPanelConfig * cfg = synth_panel_config();

    if (length < 5) { // F0 + mfrId(1) + productId + deviceId + mode
        return false;
    }
    return (data[0] == MIDI_SYSEX_START)
           && (data[1] == cfg->manufacturerId[0])
           && (data[2] == cfg->productId);
}

// notes §3
static void moog_learn_device_id(const uint8_t * data) {
    if (data[3] != gDevice.moogDeviceId) {
        LOG_DEBUG("Moog device ID learned from traffic: 0x%02X (was 0x%02X) — future requests will use it\n",
                  (unsigned)data[3], (unsigned)gDevice.moogDeviceId);
        gDevice.moogDeviceId = data[3];
    }
}

// notes §4
static void moog_apply_device_id(uint8_t * msg) {
    msg[3] = gDevice.moogDeviceId;
}

// notes §5
static uint32_t decode_7to8(const uint8_t * midi, uint32_t midiLen, uint8_t * out, uint32_t outMax) {
    uint32_t outLen = 0;

    for (uint32_t i = 0; (i + 7) < midiLen && outLen < outMax; i += 8) {
        uint8_t msbs = midi[i];

        for (int j = 0; j < 7 && outLen < outMax; j++) {
            out[outLen++] = (uint8_t)(midi[i + 1 + j] | (((msbs >> j) & 1) << 7));
        }
    }

    return outLen;
}

// notes §6
static uint32_t encode_8to7(const uint8_t * data, uint32_t dataLen, uint8_t * out, uint32_t outMax) {
    uint32_t outLen = 0;

    for (uint32_t i = 0; i < dataLen && (outLen + 8) <= outMax; i += 7) {
        uint8_t  msbs    = 0;
        uint32_t groupSz = (dataLen - i >= 7) ? 7 : (dataLen - i);

        for (uint32_t j = 0; j < groupSz; j++) {
            msbs |= (uint8_t)(((data[i + j] >> 7) & 1) << j);
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

// notes §7
static uint32_t read_korg_bitpacked_field(const uint8_t * decoded, uint32_t decodedLen,
                                          int32_t byteOffset, uint32_t bitOffset, uint32_t bitWidth) {
    uint32_t value       = 0;
    uint32_t globalStart = ((uint32_t)byteOffset * 8) + bitOffset;

    for (uint32_t k = 0; k < bitWidth; k++) {
        uint32_t globalBit = globalStart + k;
        uint32_t byteIdx   = globalBit / 8;
        uint32_t column    = globalBit % 8;

        if (byteIdx >= decodedLen) {
            break;
        }
        uint32_t bit       = (decoded[byteIdx] >> column) & 1;
        value |= bit << k;
    }

    return value;
}

static void write_korg_bitpacked_field(uint8_t * decoded, uint32_t decodedLen,
                                       int32_t byteOffset, uint32_t bitOffset, uint32_t bitWidth, uint32_t value) {
    uint32_t globalStart = ((uint32_t)byteOffset * 8) + bitOffset;

    for (uint32_t k = 0; k < bitWidth; k++) {
        uint32_t globalBit = globalStart + k;
        uint32_t byteIdx   = globalBit / 8;
        uint8_t  mask      = (uint8_t)(1u << (globalBit % 8));

        if (byteIdx >= decodedLen) {
            break;
        }
        decoded[byteIdx] = (uint8_t)(((value >> k) & 1u) ? (decoded[byteIdx] | mask) : (decoded[byteIdx] & ~mask));
    }
}

// notes §90
static pthread_mutex_t gProgCacheLock = PTHREAD_MUTEX_INITIALIZER;
static uint8_t         gProgCache[8192];
static uint32_t        gProgCacheLen  = 0;
static _Atomic int32_t gActiveVariant = -1;

// The pageVariant `variant` applies to `section` (its pages are under the variant's base), or NULL.
static const tPageVariant * section_variant(const tPanelSection * section, int32_t variant) {
    const tPanelConfig * cfg = synth_panel_config();

    if (  (variant < 0) || ((uint32_t)variant >= cfg->pageVariantCount) || !section
       || !panel_page_is_under(section->page, cfg->pageVariants[variant].base)) {
        return NULL;
    }
    return &cfg->pageVariants[variant];
}

// The pageVariant in force for `section` right now: the active one if it covers the section, else -1.
static int32_t section_active_variant(const tPanelSection * section) {
    int32_t active = atomic_load(&gActiveVariant);

    return section_variant(section, active) ? active : -1;
}

static int32_t section_dump_delta(const tPanelSection * section, int32_t variant) {
    const tPageVariant * v = section_variant(section, variant);

    return v ? v->dumpDelta : 0;
}

// notes §92
static int32_t section_param_delta(const tPanelSection * section, int32_t variant) {
    const tPageVariant * v = section_variant(section, variant);

    if (!v) {
        return 0;
    }
    return section->hasVariantParamDelta ? section->variantParamDelta : v->paramDelta;
}

// The Parameter Change ID `dial` edits right now: its own, moved by the active pageVariant.
static uint32_t dial_param_id(const tPanelDial * dial) {
    const tPanelSection * section = panel_section_of_dial(synth_panel_config(), dial);

    return (uint32_t)((int32_t)dial->paramId + section_param_delta(section, section_active_variant(section)));
}

uint32_t synth_dial_max(const tPanelDial * dial) {
    const tPanelSection * section = panel_section_of_dial(synth_panel_config(), dial);

    return ((dial->variantMax != 0) && (section_active_variant(section) >= 0)) ? dial->variantMax : dial->max;
}

// Records a dial's value in the cached dump at the place `variant` reads it from, so switching variant or
// re-reading the cache shows it.
static void cache_store_dial_value(const tPanelDial * dial, const tPanelSection * section, int32_t variant, int32_t value) {
    if ((dial->dumpOffset >= 0) && (dial->dumpBitWidth == 0) && !dial->wireSigned) {
        // notes §93
        uint32_t at = (uint32_t)(dial->dumpOffset + section_dump_delta(section, variant));

        pthread_mutex_lock(&gProgCacheLock);

        if (at < gProgCacheLen) {
            uint32_t mask = (uint32_t)dial->dumpMask << dial->dumpShift;

            gProgCache[at] = (uint8_t)((gProgCache[at] & ~mask) | (((uint32_t)value << dial->dumpShift) & mask));
        }
        pthread_mutex_unlock(&gProgCacheLock);
        return;
    }

    if ((dial->dumpOffset < 0) || (dial->dumpBitWidth == 0) || (dial->dumpBitWidth >= 32)) {
        return;
    }
    uint32_t bits = (uint32_t)value & ((1u << dial->dumpBitWidth) - 1u);

    pthread_mutex_lock(&gProgCacheLock);

    if (gProgCacheLen > 0) {
        write_korg_bitpacked_field(gProgCache, gProgCacheLen, dial->dumpOffset + section_dump_delta(section, variant),
                                   dial->dumpBitOffset, dial->dumpBitWidth, bits);
    }
    pthread_mutex_unlock(&gProgCacheLock);
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

// notes §8
static void apply_dial_wire_value(tPanelDial * dial, uint32_t rawValue, uint32_t nativeMax) {
    if (!dial) {
        return;
    }

    if (nativeMax != 0) {
        uint32_t native = (rawValue <= nativeMax) ? rawValue : nativeMax;

        dial->nativeValue = (uint8_t)native;
        dial->value       = (dial->max > 1)
                            ? (uint8_t)(((native * (dial->max - 1)) + (nativeMax / 2)) / nativeMax)
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
        // notes §9
        dial->value = (uint32_t)v;
    }
}

// notes §10
static uint32_t decode_signed_param_wire_value(tPanelDial * dial, uint16_t wireValue) {
    int32_t signedValue = (wireValue >= 8192) ? ((int32_t)wireValue - 16384) : (int32_t)wireValue;

    return (uint32_t)(signedValue + dial->displayOffset);
}

// notes §11
static uint16_t encode_signed_param_wire_value(tPanelDial * dial, uint32_t storageValue) {
    int32_t signedValue = (int32_t)storageValue - dial->displayOffset;

    return (uint16_t)((signedValue < 0) ? (signedValue + 16384) : signedValue);
}

// notes §12
static uint32_t decode_signed_dump_byte(tPanelDial * dial, uint32_t rawByte) {
    int32_t signedValue = (rawByte >= 128) ? ((int32_t)rawByte - 256) : (int32_t)rawByte;

    return (uint32_t)(signedValue + dial->displayOffset);
}

// notes §13
static void extract_prog_info(const uint8_t * decoded, uint32_t decodedLen) {
    tPanelConfig * cfg     = synth_panel_config();
    uint32_t       nameLen = (decodedLen >= cfg->progNameLen) ? cfg->progNameLen : decodedLen;

    if (nameLen >= sizeof(gDevice.progName)) {
        nameLen = sizeof(gDevice.progName) - 1;
    }
    uint32_t       i;

    for (i = 0; i < nameLen; i++) {
        char c = (char)decoded[i];

        if (c == '\0') {
            break; // the Kronos pads its name with NULs, the Z1 with spaces
        }
        gDevice.progName[i] = ((c >= 0x20) && (c <= 0x7F)) ? c : '?';
    }

    while ((i > 0) && (gDevice.progName[i - 1] == ' ')) {
        i--;
    }
    gDevice.progName[i] = '\0';

    // notes §14
    uint32_t       updated = 0;

    for (uint32_t s = 0; s < cfg->sectionCount; s++) {
        tPanelSection * dumpSection = &cfg->sections[s];

        for (uint32_t d = 0; d < dumpSection->dialCount; d++) {
            tPanelDial * dial       = &dumpSection->dials[d];

            int32_t      dumpOffset = (dial->dumpOffset >= 0)
                                          ? dial->dumpOffset + section_dump_delta(dumpSection, section_active_variant(dumpSection))
                                          : -1;

            if ((dumpOffset >= 0) && (decodedLen > (uint32_t)dumpOffset)) {
                // notes §15
                uint32_t raw = (dial->dumpBitWidth > 0)
                              ? read_korg_bitpacked_field(decoded, decodedLen, dumpOffset, dial->dumpBitOffset, dial->dumpBitWidth)
                              : (decoded[dumpOffset] >> dial->dumpShift) & dial->dumpMask;

                if (dial->dumpSigned && (dial->dumpBitWidth > 0) && (dial->dumpBitWidth < 32) && (raw >> (dial->dumpBitWidth - 1))) {
                    raw |= ~0u << dial->dumpBitWidth;
                }

                if (dial->wireSigned) {
                    // notes §16
                    raw = decode_signed_dump_byte(dial, raw);
                }
                // notes §17
                apply_dial_wire_value(dial, raw, (dial->dumpNativeMax != 0) ? dial->dumpNativeMax : dial->nativeMax);
                updated++;
            }
        }
    }

    LOG_DEBUG("Synth prog: \"%s\" — %u dial(s) updated from dump\n", gDevice.progName, (unsigned)updated);
}

// notes §18
void synth_apply_korg_prog_dump_locally(const uint8_t * decoded, uint32_t decodedLen) {
    extract_prog_info(decoded, decodedLen);
}

// notes §19
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

// notes §20
static void write_bitpacked_field(uint8_t * payload, uint32_t payloadLen,
                                  int32_t byteOffset, uint32_t bitOffset, uint32_t bitWidth, uint32_t value) {
    uint32_t globalStart = (uint32_t)byteOffset * 7 + bitOffset;

    for (uint32_t k = 0; k < bitWidth; k++) {
        uint32_t globalBit = globalStart + k;
        uint32_t byteIdx   = globalBit / 7;
        uint32_t column    = globalBit % 7;

        if (byteIdx >= payloadLen) {
            break; // matches read_bitpacked_field()'s own truncation behaviour
        }

        if ((value >> k) & 1) {
            payload[byteIdx] |= (uint8_t)(1 << column);
        } else {
            payload[byteIdx] &= (uint8_t) ~(1 << column);
        }
    }
}

// notes §21
static uint32_t synth_encode_dump_raw_value(tPanelDial * dial, uint32_t displayValue) {
    uint32_t totalWidth = dial->dumpBitWidth + dial->dumpBitWidth2;
    uint32_t dumpMax    = (dial->dumpNativeMax != 0) ? dial->dumpNativeMax : dial->nativeMax;
    uint32_t native;

    if ((dumpMax != 0) && (dial->max > 1)) {
        native = ((displayValue * dumpMax) + ((dial->max - 1) / 2)) / (dial->max - 1);
    } else {
        // notes §22
        native = (uint32_t)((int32_t)displayValue + dial->storageOffset);
    }

    if (dial->dumpInvert) {
        native = (~native) & ((totalWidth < 32) ? ((1u << totalWidth) - 1) : 0xFFFFFFFFu);
    }
    return native;
}

// notes §23
void synth_decode_moog_name(const uint8_t * payload, uint32_t payloadLen, int32_t offset, uint32_t bitOffset, uint32_t len, uint32_t lineWidth, char * outName, size_t outNameSize) {
    if ((offset < 0) || (len == 0) || (outNameSize == 0)) {
        return;
    }
    uint32_t globalBit    = (uint32_t)offset * 7 + bitOffset;
    uint32_t outLen       = 0;
    uint32_t lineChars    = 0;
    uint32_t width        = (lineWidth > 0) ? lineWidth : len;
    bool     lastWasSpace = false;

    for (uint32_t i = 0; (i < len) && (outLen < outNameSize - 1); i++) {
        uint32_t byteOffset = globalBit / 7;
        uint32_t bo         = globalBit % 7;
        uint32_t raw        = read_bitpacked_field(payload, payloadLen, (int32_t)byteOffset, bo, 8);
        uint8_t  ch         = (uint8_t)(raw & 0x7F); // strip the line-boundary marker's high bit — see comment above
        bool     printable  = (ch >= 0x20) && (ch < 0x7F) && (ch != ' ');

        if (printable) {
            outName[outLen++] = (char)ch;
            lastWasSpace      = false;
        } else if (!lastWasSpace && (outLen > 0)) {
            outName[outLen++] = ' ';
            lastWasSpace      = true;
        }
        lineChars++;
        globalBit += 8;

        if ((lineChars == width) && ((i + 1) < len) && (outLen < outNameSize - 1)) {
            outName[outLen++] = '\n';
            lastWasSpace      = true;  // a forced break also suppresses a leading collapsed space on the next line
            lineChars         = 0;
        }
    }

    while ((outLen > 0) && ((outName[outLen - 1] == ' ') || (outName[outLen - 1] == '\n'))) {
        outLen--;
    }
    outName[outLen] = '\0';
    LOG_DEBUG("Decoded name: \"%s\"\n", outName);
}

// notes §24
bool synth_moog_single_preset_dump_intact(const uint8_t * data, uint32_t length) {
    tPanelConfig *  cfg        = synth_panel_config();

    if (cfg->presetNameOffset < 0) {
        return true;
    }

    if (length < 2) {
        return false;
    }
    const uint8_t * payload    = data + 1;   // skip F0, matches every other Moog dump handler
    uint32_t        payloadLen = length - 2; // exclude leading skip + trailing F7
    uint32_t        globalBit  = (uint32_t)cfg->presetNameOffset * 7 + cfg->presetNameBitOffset;
    uint32_t        endBit     = globalBit + cfg->presetNameLen * 8;
    uint32_t        lastByte   = (endBit == 0) ? 0 : (endBit - 1) / 7;

    if (payloadLen <= lastByte) {
        return false; // truncated before the name field even finishes
    }
    uint32_t        bad        = 0;

    for (uint32_t i = 0; i < cfg->presetNameLen; i++) {
        uint32_t byteOffset = globalBit / 7;
        uint32_t bo         = globalBit % 7;
        uint32_t raw        = read_bitpacked_field(payload, payloadLen, (int32_t)byteOffset, bo, 8);
        uint8_t  ch         = (uint8_t)(raw & 0x7F);

        if ((ch != 0) && (ch != ' ') && ((ch < 0x20) || (ch >= 0x7F))) {
            bad++;
        }
        globalBit += 8;
    }

    return bad < 4;
}

// notes §25
void synth_decode_moog_category(const uint8_t * data, uint32_t length, char * outCategory, size_t outCategorySize, uint8_t * outIndex) {
    if (outIndex != NULL) {
        *outIndex = 0xFF; // "no category" (bankBrowser.h) — overwritten below only on a fully successful decode
    }

    if (outCategorySize == 0) {
        return;
    }
    tPanelDial *    dial       = find_panel_dial_by_label(synth_panel_config(), "Category");

    if (!dial || (dial->dumpBitWidth == 0)) {
        return;
    }

    if (length < 2) {
        return;
    }
    tPanelConfig *  cfg        = synth_panel_config();
    // See this function's own header comment — 0.0 (no shift) if either
    // offset isn't configured on this device, rather than a nonsense
    // negative-vs-unset arithmetic result.
    int32_t         shift      = ((cfg->presetNameOffset >= 0) && (cfg->panelNameOffset >= 0))
                                ? (cfg->presetNameOffset - cfg->panelNameOffset) : 0;
    const uint8_t * payload    = data + 1;    // skip F0, matches every other Moog dump handler
    uint32_t        payloadLen = length - 2;  // exclude leading skip + trailing F7
    uint32_t        raw        = read_bitpacked_field(payload, payloadLen, dial->dumpOffset + shift, dial->dumpBitOffset, dial->dumpBitWidth);
    uint32_t        totalWidth = dial->dumpBitWidth;

    if (dial->dumpBitWidth2 > 0) {
        uint32_t chunk2 = read_bitpacked_field(payload, payloadLen, dial->dumpOffset2 + shift, dial->dumpBitOffset2, dial->dumpBitWidth2);
        raw        |= chunk2 << dial->dumpBitWidth;
        totalWidth += dial->dumpBitWidth2;
    }

    if (dial->dumpInvert) {
        raw = (~raw) & ((totalWidth < 32) ? ((1u << totalWidth) - 1) : 0xFFFFFFFFu);
    }

    if (raw < dial->nameCount) {
        strncpy(outCategory, dial->names[raw], outCategorySize - 1);
        outCategory[outCategorySize - 1] = '\0';

        if (outIndex != NULL) {
            *outIndex = (uint8_t)raw; // dial->nameCount is realistically well under 256 for any device's Category list
        }
    }
}

// notes §87
static int32_t gProgramChangeSeen      = -1;
static bool    gProgramChangeFromSynth = false;

static void normalise_prog_name(const char * name, char * out, size_t outSize) {
    size_t o            = 0;
    bool   spacePending = false;

    for (const char * p = name; (p != NULL) && (*p != '\0'); p++) {
        if ((*p == ' ') || (*p == '\n')) {
            spacePending = (o > 0);
            continue;
        }

        if (spacePending && ((o + 1) < outSize)) {
            out[o++] = ' ';
        }
        spacePending = false;

        if ((o + 1) < outSize) {
            out[o++] = *p;
        }
    }

    out[o] = '\0';
}

bool synth_prog_names_equal(const char * a, const char * b) {
    char na[SYNTH_PROG_NAME_MAXLEN * 2];
    char nb[SYNTH_PROG_NAME_MAXLEN * 2];

    normalise_prog_name(a, na, sizeof(na));
    normalise_prog_name(b, nb, sizeof(nb));
    return (na[0] != '\0') && (strcmp(na, nb) == 0);
}

static void remember_confirmed_name(const char * name) {
    normalise_prog_name(name, gDevice.confirmedSlotName, sizeof(gDevice.confirmedSlotName));
}

static void set_current_program(int32_t program, tProgramCertainty certainty, const char * confirmedName) {
    if ((gDevice.currentProgram != program) || (gDevice.programCertainty != certainty)) {
        LOG_DEBUG("Current program %d, certainty %d\n", (int)program, (int)certainty);
    }
    gDevice.currentProgram   = program;
    gDevice.programCertainty = certainty;
    remember_confirmed_name((certainty == eProgramConfirmed) ? confirmedName : NULL);
}

// notes §96
static int32_t gBankSelectMsb       = -1; // CC0 / CC32 received since the last Program Change
static int32_t gBankSelectLsb       = -1;
static int32_t gCurrentBank         = -1; // the bank the last Program Change landed in
static int32_t gProgramChangeInBank = -1; // its number within the bank, when the bank is not known

static int32_t bank_from_select(int32_t msb, int32_t lsb) {
    const tPanelConfig * cfg = synth_panel_config();

    if ((msb < 0) && (lsb < 0)) {
        return -1;
    }

    for (uint32_t b = 0; b < cfg->bankCount; b++) {
        const tBankSelect * bank = &cfg->banks[b];

        if (  ((bank->msb >= 0) || (bank->lsb >= 0))
           && ((bank->msb < 0) || (msb < 0) || (bank->msb == msb))
           && ((bank->lsb < 0) || (lsb < 0) || (bank->lsb == lsb))) {
            return (int32_t)b;
        }
    }

    return -1;
}

// The one bank the device selects with no Bank Select at all (both its values Off), or -1.
static int32_t silent_bank(void) {
    const tPanelConfig * cfg   = synth_panel_config();
    int32_t              found = -1;

    for (uint32_t b = 0; b < cfg->bankCount; b++) {
        if ((cfg->banks[b].msb < 0) && (cfg->banks[b].lsb < 0)) {
            if (found >= 0) {
                return -1;
            }
            found = (int32_t)b;
        }
    }

    return found;
}

void synth_note_bank_select(uint8_t cc, uint8_t value) {
    if (cc == 0) {
        gBankSelectMsb = value;
    } else if (cc == 32) {
        gBankSelectLsb = value;
    }
}

void synth_note_program_change(uint8_t program, bool fromSynth) {
    int32_t slot = program;

    gProgramChangeFromSynth = fromSynth;
    gProgramChangeInBank    = -1;

    if (synth_panel_config()->bankCount > 0) {
        int32_t bank = bank_from_select(gBankSelectMsb, gBankSelectLsb);

        if ((bank < 0) && (gBankSelectMsb < 0) && (gBankSelectLsb < 0)) {
            bank = silent_bank(); // a bank picked by sending no Bank Select at all
        }

        if (bank < 0) {
            bank = gCurrentBank; // no Bank Select and none silent: the device stayed in its bank
        }

        if (bank >= 0) {
            gCurrentBank = bank;
            slot         = (bank * 128) + program;
        } else {
            gProgramChangeInBank = program; // reconcile_current_program() looks for its bank by name
            slot                 = -1;
        }
    }
    gBankSelectMsb     = -1;
    gBankSelectLsb     = -1;
    gProgramChangeSeen = slot;
    set_current_program(slot, eProgramFromProgramChange, NULL);
}

// notes §88
static void reconcile_current_program(const char * editBufferName) {
    uint32_t uniquePreset = synth_backup_unique_preset_named(editBufferName);

    if (gProgramChangeSeen >= 0) {
        uint32_t presetNumber = (uint32_t)gProgramChangeSeen + 1;

        if (  synth_backup_cached_name_is(presetNumber, editBufferName)
           && (gProgramChangeFromSynth || (uniquePreset == presetNumber))) {
            set_current_program(gProgramChangeSeen, eProgramConfirmed, editBufferName);
            return;
        }
    }

    if ((gDevice.programCertainty == eProgramConfirmed) && synth_prog_names_equal(gDevice.confirmedSlotName, editBufferName)) {
        return;
    }

    // notes §97
    for (uint32_t b = 0; (gProgramChangeInBank >= 0) && (b < synth_panel_config()->bankCount); b++) {
        uint32_t slot = (b * 128) + (uint32_t)gProgramChangeInBank;

        if (synth_backup_cached_name_is(slot + 1, editBufferName)) {
            gCurrentBank       = (int32_t)b;
            gProgramChangeSeen = (int32_t)slot;
            set_current_program((int32_t)slot, eProgramConfirmed, editBufferName);
            return;
        }
    }

    if (uniquePreset > 0) {
        set_current_program((int32_t)uniquePreset - 1, eProgramMatchedByName, NULL);
    } else if (gProgramChangeSeen >= 0) {
        set_current_program(gProgramChangeSeen, eProgramFromProgramChange, NULL);
    } else {
        set_current_program(-1, eProgramUnknown, NULL);
    }
}

// notes §26
static bool     gProgNameAwaitingFreshData                  = false;
static uint8_t  gPendingProgNameRaw[SYNTH_PROG_NAME_MAXLEN] = {0};
static uint32_t gPendingProgNameLen                         = 0;

static void extract_moog_panel_info(const uint8_t * payload, uint32_t payloadLen) {
    tPanelConfig * cfg                                 = synth_panel_config();
    uint32_t       updated                             = 0;

    char           synthName[sizeof(gDevice.progName)] = "";

    synth_decode_moog_name(payload, payloadLen, cfg->panelNameOffset, cfg->panelNameBitOffset, cfg->panelNameLen, cfg->nameLineWidth, synthName, sizeof(synthName));

    if (!gProgNameAwaitingFreshData && (cfg->panelNameOffset >= 0) && (cfg->panelNameLen > 0)) {
        // notes §27
        memcpy(gDevice.progName, synthName, sizeof(gDevice.progName));
    }
    reconcile_current_program(synthName);

    for (uint32_t s = 0; s < cfg->sectionCount; s++) {
        tPanelSection * section = &cfg->sections[s];

        for (uint32_t d = 0; d < section->dialCount; d++) {
            tPanelDial * dial       = &section->dials[d];

            if (dial->dumpBitWidth == 0) {
                continue;
            }

            if (dial->dumpSendAwaitingFreshData || dial->hasPendingDumpSend) {
                // notes §28
                continue;
            }
            uint32_t     raw        = read_bitpacked_field(payload, payloadLen, dial->dumpOffset,
                                                           dial->dumpBitOffset, dial->dumpBitWidth);

            uint32_t     totalWidth = dial->dumpBitWidth;

            if (dial->dumpBitWidth2 > 0) {
                // Non-contiguous field (see dumpBitWidth2's comment in
                // panelConfig.h) — chunk2 contributes the next-significant
                // bits above chunk1, same shape as a CC MSB/LSB pair.
                uint32_t chunk2 = read_bitpacked_field(payload, payloadLen, dial->dumpOffset2,
                                                       dial->dumpBitOffset2, dial->dumpBitWidth2);
                raw        |= chunk2 << dial->dumpBitWidth;
                totalWidth += dial->dumpBitWidth2;
            }

            if (dial->dumpInvert) {
                // notes §29
                raw = (~raw) & ((totalWidth < 32) ? ((1u << totalWidth) - 1) : 0xFFFFFFFFu);
            }
            // notes §30
            uint32_t     oldValue   = dial->value;

            apply_dial_wire_value(dial, raw, (dial->dumpNativeMax != 0) ? dial->dumpNativeMax : dial->nativeMax);
            updated++;

            if (dial->value != oldValue) {
                LOG_DEBUG("  %-16s CC-tracked=%u -> dump-decoded=%u (raw=%u)\n",
                          dial->id, (unsigned)oldValue, (unsigned)dial->value, (unsigned)raw);
            }
        }
    }

    LOG_DEBUG("Moog panel dump: %u dial(s) updated (%u payload bytes)\n",
              (unsigned)updated, (unsigned)payloadLen);
}

// ── Message handlers ──────────────────────────────────────────────────────────

static void probe_startup_program(void);
static bool korg_decode_prog_dump(const uint8_t * data, uint32_t length, uint8_t * decoded, uint32_t decodedCap, uint32_t * outDecodedLen);

static void handle_curr_prog_dump(const uint8_t * data, uint32_t length) {
    // Format: F0 <mfrId> 3g 46 40 01 [7-bit encoded data...] F7
    // Payload starts right after the header (F0+mfrId+chan+fam+func = 4+n
    // bytes) plus the extra "01" sub-byte this dump function has.
    synth_backup_capture_dump(data, length, eBackupExpectLive); // no-op unless a live-edit-buffer Backup is pending — see synthBackup.c
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

    // notes §95
    pthread_mutex_lock(&gProgCacheLock);
    memcpy(gProgCache, decoded, decodedLen);
    gProgCacheLen = decodedLen;
    extract_prog_info(gProgCache, gProgCacheLen);
    pthread_mutex_unlock(&gProgCacheLock);
    reconcile_current_program(gDevice.progName); // notes §98
    probe_startup_program();
}

// notes §31
// notes §101
static bool gStartupProbeSent    = false;
static bool gStartupProbePending = false;

static void probe_startup_program(void) {
    const tPanelConfig * cfg = synth_panel_config();

    if ((cfg->startupSlot < 0) || gStartupProbeSent || (gDevice.currentProgram >= 0) || (gDevice.progName[0] == '\0')) {
        return;
    }
    gStartupProbeSent    = true;
    gStartupProbePending = true;
    synth_request_korg_program_dump((uint8_t)(cfg->startupSlot / 128), (uint32_t)(cfg->startupSlot % 128) + 1);
}

static void check_startup_probe(const uint8_t * data, uint32_t length) {
    const tPanelConfig * cfg                          = synth_panel_config();
    uint32_t             funcPos                      = 3 + cfg->manufacturerIdLen;
    static uint8_t       decoded[4096];
    uint32_t             decodedLen;

    if (  !gStartupProbePending || (length < funcPos + 3)
       || ((int32_t)((data[funcPos + 1] & 0x0F) * 128 + data[funcPos + 2]) != cfg->startupSlot)) {
        return;
    }
    gStartupProbePending = false;

    if (!korg_decode_prog_dump(data, length, decoded, sizeof(decoded), &decodedLen) || (decodedLen < cfg->progNameLen)) {
        return;
    }
    char                 name[SYNTH_PROG_NAME_MAXLEN] = "";
    uint32_t             n                            = (cfg->progNameLen < sizeof(name) - 1) ? cfg->progNameLen : (uint32_t)sizeof(name) - 1;

    memcpy(name, decoded, n);

    if ((gDevice.currentProgram < 0) && synth_prog_names_equal(name, gDevice.progName)) {
        gCurrentBank = cfg->startupSlot / 128;
        set_current_program(cfg->startupSlot, eProgramMatchedByName, NULL);
        LOG_DEBUG("Current program is the startup program by name\n");
    }
}

static void handle_prog_dump(const uint8_t * data, uint32_t length) {
    synth_backup_capture_dump(data, length, eBackupExpectKorgProgram);
    check_startup_probe(data, length);
    LOG_DEBUG("Received Program Data Dump (len=%u)\n", (unsigned)length);
}

// notes §32
static bool korg_decode_prog_dump(const uint8_t * data, uint32_t length, uint8_t * decoded, uint32_t decodedCap, uint32_t * outDecodedLen) {
    if (!is_synth_sysex(data, length)) {
        return false;
    }
    tPanelConfig *  cfg        = synth_panel_config();
    uint32_t        funcPos    = 3 + cfg->manufacturerIdLen;
    uint8_t         funcId     = data[funcPos];
    // notes §33
    uint32_t        extra;

    if (funcId == SYNTH_FUNC_CURR_PROG_DUMP) {
        extra = 1;
    } else if (funcId == SYNTH_FUNC_PROG_DUMP) {
        extra = 3;
    } else {
        return false;
    }
    uint32_t        skip       = funcPos + 1 + extra;

    if (length < skip + 1) {
        return false;
    }
    const uint8_t * payload    = data + skip;
    uint32_t        payloadLen = length - skip - 1; // exclude trailing F7

    *outDecodedLen = decode_7to8(payload, payloadLen, decoded, decodedCap);
    return true;
}

// notes §34
bool synth_decode_korg_prog_dump(const uint8_t * data, uint32_t length, uint8_t * decoded, uint32_t decodedCap, uint32_t * outDecodedLen) {
    return korg_decode_prog_dump(data, length, decoded, decodedCap, outDecodedLen);
}

bool synth_korg_program_dump_intact(const uint8_t * data, uint32_t length) {
    tPanelConfig * cfg        = synth_panel_config();
    static uint8_t decoded[4096];
    uint32_t       decodedLen = 0;

    if (!korg_decode_prog_dump(data, length, decoded, sizeof(decoded), &decodedLen)) {
        return false;
    }

    if (decodedLen < cfg->progNameLen) {
        return false;
    }
    uint32_t       bad        = 0;

    for (uint32_t i = 0; i < cfg->progNameLen; i++) {
        uint8_t ch = decoded[i];

        if ((ch != 0) && (ch != ' ') && ((ch < 0x20) || (ch >= 0x7F))) {
            bad++;
        }
    }

    return bad < 4;
}

void synth_decode_korg_name(const uint8_t * data, uint32_t length, char * outName, size_t outNameSize) {
    if (outNameSize == 0) {
        return;
    }
    tPanelConfig * cfg        = synth_panel_config();
    static uint8_t decoded[4096];
    uint32_t       decodedLen = 0;

    if (!korg_decode_prog_dump(data, length, decoded, sizeof(decoded), &decodedLen)) {
        return;
    }
    uint32_t       nameLen    = (decodedLen >= cfg->progNameLen) ? cfg->progNameLen : decodedLen;

    if (nameLen >= outNameSize) {
        nameLen = (uint32_t)(outNameSize - 1);
    }
    uint32_t       i;

    for (i = 0; i < nameLen; i++) {
        char c = (char)decoded[i];
        outName[i] = ((c >= 0x20) && (c <= 0x7F)) ? c : '?';
    }

    outName[i] = '\0';

    // notes §35
    while ((i > 0) && (outName[i - 1] == ' ')) {
        outName[--i] = '\0';
    }
}

// notes §36
void synth_decode_korg_category(const uint8_t * data, uint32_t length, char * outCategory, size_t outCategorySize, uint8_t * outIndex) {
    if (outIndex != NULL) {
        *outIndex = 0xFF; // "no category" (bankBrowser.h) — overwritten below only on a fully successful decode
    }

    if (outCategorySize == 0) {
        return;
    }
    tPanelDial *   dial       = find_panel_dial_by_label(synth_panel_config(), "Category");

    if (!dial || (dial->dumpOffset < 0)) {
        return;
    }
    static uint8_t decoded[4096];
    uint32_t       decodedLen = 0;

    if (!korg_decode_prog_dump(data, length, decoded, sizeof(decoded), &decodedLen)) {
        return;
    }

    if (decodedLen <= (uint32_t)dial->dumpOffset) {
        return;
    }
    uint32_t       raw        = (decoded[dial->dumpOffset] >> dial->dumpShift) & dial->dumpMask;

    if (raw < dial->nameCount) {
        strncpy(outCategory, dial->names[raw], outCategorySize - 1);
        outCategory[outCategorySize - 1] = '\0';

        if (outIndex != NULL) {
            *outIndex = (uint8_t)raw; // dial->nameCount is realistically well under 256 for any device's Category list
        }
    }
}

// notes §37
static uint8_t  gLastMoogDump[256];
static uint32_t gLastMoogDumpLen           = 0;

// notes §38
static bool     gAwaitingFreshDumpForPatch = false;

bool synth_dump_patch_in_flight(void) {
    return gAwaitingFreshDumpForPatch;
}

// notes §39
static bool synth_patch_moog_dump_cache(tPanelDial * dial, uint32_t rawValue) {
    if ((gLastMoogDumpLen == 0) || (dial->dumpBitWidth == 0)) {
        return false;
    }
    uint8_t * payload    = gLastMoogDump + 1;               // skip F0 — see handle_moog_panel_dump()'s own comment on why
    uint32_t  payloadLen = gLastMoogDumpLen - 1 - 1;        // exclude leading skip + trailing F7

    write_bitpacked_field(payload, payloadLen, dial->dumpOffset, dial->dumpBitOffset,
                          dial->dumpBitWidth, rawValue);

    if (dial->dumpBitWidth2 > 0) {
        write_bitpacked_field(payload, payloadLen, dial->dumpOffset2, dial->dumpBitOffset2,
                              dial->dumpBitWidth2, rawValue >> dial->dumpBitWidth);
    }
    return true;
}

// notes §40
static void synth_apply_pending_dump_patches(void) {
    tPanelConfig * cfg        = synth_panel_config();
    bool           anyPatched = false;

    for (uint32_t s = 0; s < cfg->sectionCount; s++) {
        tPanelSection * section = &cfg->sections[s];

        for (uint32_t d = 0; d < section->dialCount; d++) {
            tPanelDial * dial = &section->dials[d];

            if (!dial->dumpSendAwaitingFreshData) {
                continue;
            }
            dial->dumpSendAwaitingFreshData = false;

            if (synth_patch_moog_dump_cache(dial, dial->pendingDumpRawValue)) {
                anyPatched = true;
                LOG_DEBUG("Patched %s=%u into freshly-fetched Panel Dump\n",
                          dial->id, (unsigned)dial->pendingDumpRawValue);
            }
        }
    }

    if (gProgNameAwaitingFreshData) {
        // notes §41
        gProgNameAwaitingFreshData = false;
        uint8_t * payload    = gLastMoogDump + 1;
        uint32_t  payloadLen = gLastMoogDumpLen - 1 - 1;
        uint32_t  globalBit  = (uint32_t)cfg->panelNameOffset * 7 + cfg->panelNameBitOffset;

        for (uint32_t c = 0; c < gPendingProgNameLen; c++) {
            uint32_t byteOffset = globalBit / 7;
            uint32_t bo         = globalBit % 7;

            write_bitpacked_field(payload, payloadLen, (int32_t)byteOffset, bo, 8, gPendingProgNameRaw[c]);
            globalBit += 8;
        }

        anyPatched                 = true;
        LOG_DEBUG("Patched program name into freshly-fetched Panel Dump\n");

        if (gDevice.programCertainty == eProgramConfirmed) {
            char renamed[sizeof(gDevice.progName)] = "";

            synth_decode_moog_name(payload, payloadLen, cfg->panelNameOffset, cfg->panelNameBitOffset, cfg->panelNameLen, cfg->nameLineWidth, renamed, sizeof(renamed));
            remember_confirmed_name(renamed); // our own rename of the confirmed slot's patch
        }
    }

    if (anyPatched) {
        midi_send(gLastMoogDump, gLastMoogDumpLen);
        LOG_DEBUG("Resent freshly-patched Panel Dump (%u bytes)\n", (unsigned)gLastMoogDumpLen);
    }
    gAwaitingFreshDumpForPatch = false;
}

static void handle_moog_panel_dump(const uint8_t * data, uint32_t length) {
    const uint32_t  skip       = 1; // F0 only

    if (length < skip + 1) {
        LOG_ERROR("Moog panel dump too short (%u)\n", (unsigned)length);
        return;
    }
    const uint8_t * payload    = data + skip;
    uint32_t        payloadLen = length - skip - 1; // exclude trailing F7

    if (length <= sizeof(gLastMoogDump)) {
        memcpy(gLastMoogDump, data, length);
        gLastMoogDumpLen = length;
    } else {
        LOG_ERROR("Moog panel dump (%u bytes) too big to cache for resend (max %u)\n",
                  (unsigned)length, (unsigned)sizeof(gLastMoogDump));
    }
    extract_moog_panel_info(payload, payloadLen);
    // notes §42
    synth_backup_capture_dump(data, length, eBackupExpectLive);
    synth_apply_pending_dump_patches();
}

// notes §43
void synth_apply_moog_panel_dump_locally(const uint8_t * data, uint32_t length) {
    const uint32_t skip = 1;    // F0 only, matching handle_moog_panel_dump()

    if ((data == NULL) || (length < (skip + 2))) {
        return;
    }
    extract_moog_panel_info(data + skip, length - skip - 1);   // trailing F7 excluded
}

// notes §44
static void handle_moog_single_preset_dump(const uint8_t * data, uint32_t length) {
    const uint32_t  skip            = 1; // F0 only — see handle_moog_panel_dump()'s comment on why

    if (length < skip + 1) {
        LOG_ERROR("Moog single preset dump too short (%u)\n", (unsigned)length);
        return;
    }
    const uint8_t * payload         = data + skip;
    uint32_t        payloadLen      = length - skip - 1; // exclude trailing F7
    tPanelConfig *  cfg             = synth_panel_config();

    // notes §45
    bool            nameSweepActive = synth_backup_export_progress_is_name_sweep();

    if (!nameSweepActive) {
        char name[sizeof(gDevice.progName)];

        name[0]                                        = '\0';
        synth_decode_moog_name(payload, payloadLen, cfg->presetNameOffset, cfg->presetNameBitOffset, cfg->presetNameLen, cfg->nameLineWidth, name, sizeof(name));

        // notes §46
        strncpy(gDevice.progName, name, sizeof(gDevice.progName) - 1);
        gDevice.progName[sizeof(gDevice.progName) - 1] = '\0';

        // notes §47
        if (length > 5) {
            synth_backup_note_preset_name((uint32_t)data[5] + 1, name);
        }
    }
    synth_backup_capture_dump(data, length, eBackupExpectPreset); // no-op unless a by-number Backup is pending — see synthBackup.c; after the name decode so a by-number backup's default filename can use it
}

// notes §48
static void handle_moog_all_presets_dump(const uint8_t * data, uint32_t length) {
    synth_backup_capture_dump(data, length, eBackupExpectBank);
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
        // notes §49
        tPanelDial * dial    = NULL;

        int32_t      variant = -1;

        for (uint32_t s = 0; (s < cfg->sectionCount) && !dial; s++) {
            dial = find_panel_dial_by_param(&cfg->sections[s], group, paramId);
        }

        // notes §94
        for (uint32_t v = 0; !dial && (v < cfg->pageVariantCount); v++) {
            for (uint32_t s = 0; (s < cfg->sectionCount) && !dial; s++) {
                tPanelSection * section = &cfg->sections[s];

                if (section_variant(section, (int32_t)v)) {
                    dial    = find_panel_dial_by_param(section, group, (uint32_t)((int32_t)paramId - section_param_delta(section, (int32_t)v)));
                    variant = dial ? (int32_t)v : -1;
                }
            }
        }

        if (dial) {
            // notes §50
            uint32_t        rawForDial = dial->wireSigned ? decode_signed_param_wire_value(dial, value) : value;
            tPanelSection * section    = panel_section_of_dial(cfg, dial);

            cache_store_dial_value(dial, section, variant, (int32_t)rawForDial);

            if (variant == section_active_variant(section)) {
                apply_dial_wire_value(dial, rawForDial, (dial->dumpNativeMax != 0) ? dial->dumpNativeMax : dial->nativeMax);
            }
            synthlib_request_redraw();
            LOG_DEBUG("Param %u (%s) = %u\n", (unsigned)paramId, dial->label, (unsigned)value);
        }
    }
}

// ── Public API ────────────────────────────────────────────────────────────────

void synth_on_connected(void) {
    LOG_DEBUG("Synth connected (channel byte 0x%02X)\n", SYNTH_SYSEX_CHANNEL_BYTE(gDevice.id));
    memset(gDevice.progName, 0, sizeof(gDevice.progName));
    gProgramChangeSeen               = -1;
    gProgramChangeInBank             = -1;
    gCurrentBank                     = -1;
    gBankSelectMsb                   = -1;
    gBankSelectLsb                   = -1;
    gDevice.programChangeTransmitOff = false;
    gStartupProbeSent                = false;
    gStartupProbePending             = false;
    set_current_program(-1, eProgramUnknown, NULL); // types.h notes §2
    // notes §51

    // notes §52
    tPanelConfig * cfg = synth_panel_config();

    for (uint32_t s = 0; s < cfg->sectionCount; s++) {
        tPanelSection * section = &cfg->sections[s];

        for (uint32_t d = 0; d < section->dialCount; d++) {
            apply_dial_wire_value(&section->dials[d], 0, section->dials[d].nativeMax);
        }
    }

    // notes §53
    synth_request_state_dump();
    synth_request_bank_map();
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

// notes §54
static void synth_request_kronos_current_object_dump(uint8_t obj) {
    uint8_t  msg[8];
    uint32_t pos = build_header(msg, 0x74);

    msg[pos++] = obj;
    msg[pos++] = MIDI_SYSEX_END;
    midi_send(msg, pos);
    LOG_DEBUG("Sent Kronos Current Object Dump Request (obj=0x%02X)\n", (unsigned)obj);
}

// notes §55
static void synth_send_kronos_parameter_change(uint32_t typ, uint32_t soc, uint32_t sub, uint32_t pid, uint32_t idx, int32_t value) {
    uint8_t  msg[14];
    uint32_t pos = build_header(msg, 0x43);
    uint32_t v21 = (uint32_t)value & 0x1FFFFF; // 21-bit 2's complement — see *4 in KRONOS_MIDI_SysEx.txt

    msg[pos++] = (uint8_t)(typ & 0x7F);
    msg[pos++] = (uint8_t)(soc & 0x7F);
    msg[pos++] = (uint8_t)(sub & 0x7F);
    msg[pos++] = (uint8_t)(pid & 0x7F);
    msg[pos++] = (uint8_t)(idx & 0x7F);
    msg[pos++] = (uint8_t)((v21 >> 14) & 0x7F); // valueH: bit14-20
    msg[pos++] = (uint8_t)((v21 >> 7) & 0x7F);  // valueM: bit7-13
    msg[pos++] = (uint8_t)(v21 & 0x7F);         // valueL: bit0-6
    msg[pos++] = MIDI_SYSEX_END;
    midi_send(msg, pos);
    LOG_DEBUG("Sent Kronos Parameter Change: TYP=%u SOC=%u SUB=%u PID=%u IDX=%u value=%d\n",
              (unsigned)typ, (unsigned)soc, (unsigned)sub, (unsigned)pid, (unsigned)idx, (int)value);
}

int32_t synth_dump_byte(int32_t offset) {
    int32_t value = -1;

    pthread_mutex_lock(&gProgCacheLock);

    if ((offset >= 0) && ((uint32_t)offset < gProgCacheLen)) {
        value = gProgCache[offset];
    }
    pthread_mutex_unlock(&gProgCacheLock);
    return value;
}

void synth_set_active_page_variant(int32_t variant) {
    if (atomic_exchange(&gActiveVariant, variant) == variant) {
        return;
    }
    pthread_mutex_lock(&gProgCacheLock);

    if (gProgCacheLen > 0) {
        extract_prog_info(gProgCache, gProgCacheLen);
    }
    pthread_mutex_unlock(&gProgCacheLock);
    synthlib_request_redraw();
}

// notes §89
static _Atomic int32_t gReportedMode = -1;

int32_t synth_take_reported_mode(void) {
    return atomic_exchange(&gReportedMode, -1);
}

static bool synth_speaks_kronos_protocol(void) {
    return !synth_panel_config()->moogStyleDump && !synth_panel_config()->supportsKorgProgramDump;
}

void synth_send_device_mode(uint32_t mode) {
    if (!synth_speaks_kronos_protocol()) {
        return;
    }
    uint8_t  msg[8];
    uint32_t pos = build_header(msg, 0x4E); // Mode Change

    msg[pos++] = (uint8_t)(mode & 0x0F);
    msg[pos++] = MIDI_SYSEX_END;
    midi_send(msg, pos);
    LOG_DEBUG("Sent Kronos Mode Change (mode=%u)\n", (unsigned)mode);
}

static void synth_request_kronos_mode(void) {
    uint8_t  msg[8];
    uint32_t pos = build_header(msg, 0x12); // Mode Request

    msg[pos++] = MIDI_SYSEX_END;
    midi_send(msg, pos);
}

void synth_request_state_dump(void) {
    tPanelConfig * cfg = synth_panel_config();

    if (!cfg->moogStyleDump && !cfg->supportsKorgProgramDump) {
        // notes §56
        synth_request_kronos_mode();
        synth_request_kronos_current_object_dump(0x00); // Program
        return;
    }

    if (cfg->stateRequestSysExLen > 0) {
        if (cfg->moogStyleDump && (cfg->stateRequestSysExLen > 3)) {
            // notes §57
            uint8_t msg[sizeof(cfg->stateRequestSysEx)];

            memcpy(msg, cfg->stateRequestSysEx, cfg->stateRequestSysExLen);
            moog_apply_device_id(msg);
            midi_send(msg, cfg->stateRequestSysExLen);
        } else {
            midi_send(cfg->stateRequestSysEx, cfg->stateRequestSysExLen);
        }
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
    // notes §58
    uint8_t        msg[sizeof(cfg->stateRequestSysEx) + 2];
    uint32_t       prefixLen = cfg->stateRequestSysExLen - 1; // everything up to (not including) the trailing F7

    memcpy(msg, cfg->stateRequestSysEx, prefixLen);
    moog_apply_device_id(msg);
    msg[prefixLen - 1] = 0x06;                        // mode byte: Single Preset Dump REQUEST
    msg[prefixLen]     = (uint8_t)(presetNumber - 1); // 0-based on the wire — see the header comment
    msg[prefixLen + 1] = MIDI_SYSEX_END;
    midi_send(msg, prefixLen + 2);
    LOG_DEBUG("Sent Single Preset Dump Request for preset %u\n", (unsigned)presetNumber);
}

void synth_request_all_presets_dump(void) {
    tPanelConfig * cfg       = synth_panel_config();

    if (!cfg->moogStyleDump) {
        LOG_ERROR("All Presets Dump Request: not a Moog-style device\n");
        return;
    }

    if (cfg->stateRequestSysExLen < 2) {
        LOG_ERROR("All Presets Dump Request: no stateRequestSysEx declared\n");
        return;
    }
    // notes §59
    uint8_t        msg[sizeof(cfg->stateRequestSysEx)];
    uint32_t       prefixLen = cfg->stateRequestSysExLen - 1; // everything up to (not including) the trailing F7

    memcpy(msg, cfg->stateRequestSysEx, prefixLen);
    moog_apply_device_id(msg);
    msg[prefixLen - 1] = 0x04; // mode byte: All Presets Dump REQUEST
    msg[prefixLen]     = MIDI_SYSEX_END;
    midi_send(msg, prefixLen + 1);
    LOG_DEBUG("Sent All Presets Dump Request\n");
}

// notes §60
static void synth_change_program(uint8_t program) {
    midi_send_program_change(gDevice.id, program);
    synth_note_program_change(program, false); // optimistic - types.h notes §2
    LOG_DEBUG("Preset navigation: sent Program Change %d\n", (int)program);
    // notes §61
    midi_arm_state_dump_debounce();
}

void synth_navigate_preset(int32_t delta) {
    if (!gDevice.connected) {
        LOG_ERROR("Preset navigation: no device connected\n");
        return;
    }
    // notes §62
    int32_t next = gDevice.currentProgram + delta;

    if (!synth_panel_config()->moogStyleDump && (synth_panel_config()->bankCount > 0)) {
        int32_t last = ((int32_t)synth_panel_config()->bankCount * 128) - 1; // notes §99

        next = (next < 0) ? 0 : (next > last) ? last : next;
        synth_korg_select_program((uint8_t)(next / 128), (uint32_t)(next % 128) + 1);
        return;
    }

    if (next < 0) {
        next = 0;
    }

    if (next > 127) {
        next = 127;
    }
    synth_change_program((uint8_t)next);
}

void synth_load_patch_from_bank(uint8_t bank, uint32_t presetNumber) {
    if (!gDevice.connected) {
        LOG_ERROR("Load Patch: no device connected\n");
        return;
    }

    if ((presetNumber < 1) || (presetNumber > 128)) {
        LOG_ERROR("Load Patch: preset number %u out of range (1-128)\n", (unsigned)presetNumber);
        return;
    }

    // notes §63
    if (synth_panel_config()->moogStyleDump) {
        synth_change_program((uint8_t)(presetNumber - 1));
        return;
    }
    synth_korg_select_program(bank, presetNumber);
}

void synth_korg_select_program(uint8_t bank, uint32_t progNumber) {
    if (!gDevice.connected) {
        LOG_ERROR("Select Program: no device connected\n");
        return;
    }

    if (synth_panel_config()->moogStyleDump) {
        LOG_ERROR("Select Program: connected device isn't Korg-style\n");
        return;
    }

    if ((progNumber < 1) || (progNumber > 128)) {
        LOG_ERROR("Select Program: program number %u out of range (1-128)\n", (unsigned)progNumber);
        return;
    }
    // notes §64
    const tPanelConfig * cfg = synth_panel_config();

    if (bank < cfg->bankCount) {
        const tBankSelect * b   = &cfg->banks[bank];
        bool                off = (b->msb < 0) && (b->lsb < 0);
        int32_t             msb = off ? b->declaredMsb : b->msb; // notes §102
        int32_t             lsb = off ? b->declaredLsb : b->lsb;

        if (msb >= 0) {
            midi_send_cc(gDevice.id, 0, (uint8_t)msb);
        }

        if (lsb >= 0) {
            midi_send_cc(gDevice.id, 32, (uint8_t)lsb);
        }
    } else {
        midi_send_cc(gDevice.id, 0, 0);
        midi_send_cc(gDevice.id, 32, bank);
    }
    gCurrentBank   = bank;
    gBankSelectMsb = (bank < cfg->bankCount) ? cfg->banks[bank].msb : 0;    // what went out, read back by
    gBankSelectLsb = (bank < cfg->bankCount) ? cfg->banks[bank].lsb : bank; // synth_note_program_change()
    synth_change_program((uint8_t)(progNumber - 1));
}

// notes §100
void synth_request_bank_map(void) {
    const tPanelConfig * cfg = synth_panel_config();

    if (!gDevice.connected || (cfg->bankMapRequestLen == 0) || (cfg->bankMapRequestLen > 12)) {
        return;
    }
    uint8_t              msg[32];
    uint32_t             pos = build_header(msg, cfg->bankMapRequest[0]);

    for (uint32_t b = 1; b < cfg->bankMapRequestLen; b++) {
        msg[pos++] = cfg->bankMapRequest[b];
    }

    msg[pos++] = MIDI_SYSEX_END;
    midi_send(msg, pos);
    LOG_DEBUG("Sent bank map request\n");
}

static void handle_bank_map_reply(const uint8_t * data, uint32_t length) {
    tPanelConfig * cfg        = synth_panel_config();
    uint32_t       skip       = 5 + cfg->manufacturerIdLen; // header + func + sub

    if ((length < skip + 2) || ((int32_t)data[skip - 1] != cfg->bankMapReplySub)) {
        return;
    }
    static uint8_t decoded[1024];
    uint32_t       decodedLen = decode_7to8(data + skip, length - skip - 1, decoded, sizeof(decoded));

    for (uint32_t b = 0; b < cfg->bankCount; b++) {
        tBankSelect * bank = &cfg->banks[b];

        if ((bank->msbDumpOffset >= 0) && ((uint32_t)bank->msbDumpOffset < decodedLen)) {
            bank->msb = (int8_t)decoded[bank->msbDumpOffset]; // -1 = Off: not sent
        }

        if ((bank->lsbDumpOffset >= 0) && ((uint32_t)bank->lsbDumpOffset < decodedLen)) {
            bank->lsb = (int8_t)decoded[bank->lsbDumpOffset];
        }
        LOG_DEBUG("Bank %s: Bank Select %d / %d\n", bank->name, (int)bank->msb, (int)bank->lsb);
    }

    if ((cfg->pcTransmitOffset >= 0) && ((uint32_t)cfg->pcTransmitOffset < decodedLen)) {
        gDevice.programChangeTransmitOff = (((decoded[cfg->pcTransmitOffset] >> cfg->pcTransmitShift) & cfg->pcTransmitMask) == 0);
        LOG_DEBUG("Program Change Transmit %s on the device\n", gDevice.programChangeTransmitOff ? "OFF" : "on");
    }
}

void synth_request_korg_program_dump(uint8_t bank, uint32_t progNumber) {
    if (!gDevice.connected) {
        LOG_ERROR("Request Program Dump: no device connected\n");
        return;
    }

    if (synth_panel_config()->moogStyleDump) {
        LOG_ERROR("Request Program Dump: connected device isn't Korg-style\n");
        return;
    }

    if (!synth_panel_config()->supportsKorgProgramDump) {
        LOG_ERROR("Request Program Dump: connected device doesn't speak this specific (Z1-shaped) dump protocol\n");
        return;
    }

    if ((progNumber < 1) || (progNumber > 128)) {
        LOG_ERROR("Request Program Dump: program number %u out of range (1-128)\n", (unsigned)progNumber);
        return;
    }
    // F0 <mfrId> 3g 46 1C ub pp 00 F7 — ub: Unit(00:Prog)/Bank(0:A,1:B) in
    // the low bit, pp: 0-based program number, per the Z1 MIDI
    // Implementation doc's own PROGRAM DATA DUMP REQUEST table.
    uint8_t  msg[16];
    uint32_t pos = build_header(msg, SYNTH_FUNC_PROG_DUMP_REQ);

    msg[pos++] = (uint8_t)(bank & 0x01); // Unit=00 (Prog) in bits 4-5, Bank in bit 0 — Unit=00 is already all-zero bits, so this reduces to just the bank bit
    msg[pos++] = (uint8_t)(progNumber - 1);
    msg[pos++] = 0x00;
    msg[pos++] = MIDI_SYSEX_END;
    midi_send(msg, pos);
    LOG_DEBUG("Sent Program Data Dump Request (bank=%c, program=%u)\n", bank ? 'B' : 'A', (unsigned)progNumber);
}

void synth_send_korg_program_write_request(uint8_t bank, uint32_t progNumber) {
    if (!gDevice.connected) {
        LOG_ERROR("Program Write Request: no device connected\n");
        return;
    }

    if (synth_panel_config()->moogStyleDump) {
        LOG_ERROR("Program Write Request: connected device isn't Korg-style\n");
        return;
    }

    if (!synth_panel_config()->supportsKorgProgramDump) {
        LOG_ERROR("Program Write Request: connected device doesn't speak this specific (Z1-shaped) dump protocol\n");
        return;
    }

    if ((progNumber < 1) || (progNumber > 128)) {
        LOG_ERROR("Program Write Request: program number %u out of range (1-128)\n", (unsigned)progNumber);
        return;
    }
    // notes §65
    uint8_t  msg[16];
    uint32_t pos = build_header(msg, SYNTH_FUNC_PROG_WRITE_REQ);

    msg[pos++] = (uint8_t)(bank & 0x01);
    msg[pos++] = (uint8_t)(progNumber - 1);
    msg[pos++] = MIDI_SYSEX_END;
    midi_send(msg, pos);
    LOG_DEBUG("Sent Program Write Request (bank=%c, program=%u)\n", bank ? 'B' : 'A', (unsigned)progNumber);
}

void synth_send_parameter_change(uint8_t group, uint16_t paramId, uint16_t value) {
    // F0 42 3g 46 41 0mm pp pp vv vv F7
    uint8_t  msg[11];

    LOG_DEBUG("Parameter Change out: group=%u param=%u value=%u\n", (unsigned)group, (unsigned)paramId, (unsigned)value);
    uint32_t pos = build_header(msg, SYNTH_FUNC_PARAMETER_CHANGE);

    msg[pos++] = (uint8_t)(group & 0x0F);
    msg[pos++] = (uint8_t)(paramId & 0x7F);
    msg[pos++] = (uint8_t)((paramId >> 7) & 0x7F);
    msg[pos++] = (uint8_t)(value & 0x7F);
    msg[pos++] = (uint8_t)((value >> 7) & 0x7F);
    msg[pos++] = MIDI_SYSEX_END;
    midi_send(msg, pos);
}

// notes §66
void synth_send_korg_current_program_dump(const uint8_t * rawPayload, uint32_t rawPayloadLen) {
    static uint8_t msg[2048];
    uint32_t       pos = build_header(msg, SYNTH_FUNC_CURR_PROG_DUMP);

    msg[pos++] = 0x01; // fixed sub-byte func 0x40's own wire format always carries — see handle_curr_prog_dump()'s own comment above

    if (rawPayloadLen > sizeof(msg) - pos - 1) {
        LOG_ERROR("Current Program Data Dump payload too large (%u bytes) — not sending\n", (unsigned)rawPayloadLen);
        return;
    }
    memcpy(msg + pos, rawPayload, rawPayloadLen);
    pos       += rawPayloadLen;
    msg[pos++] = MIDI_SYSEX_END;
    midi_send(msg, pos);
    LOG_DEBUG("Sent Current Program Data Dump (func 0x40), %u byte payload — loads live edit buffer only\n", (unsigned)rawPayloadLen);
}

// notes §67
void synth_send_korg_program_data_dump(uint8_t bank, uint32_t progNumber, const uint8_t * rawPayload, uint32_t rawPayloadLen) {
    static uint8_t msg[2048];
    uint32_t       pos = build_header(msg, SYNTH_FUNC_PROG_DUMP);

    msg[pos++] = (uint8_t)(bank ? 0x01 : 0x00);      // Unit=00 (Prog) | Bank(0:A/1:B)
    msg[pos++] = (uint8_t)((progNumber - 1) & 0x7F); // 0-based on the wire, same convention synth_request_korg_program_dump() already uses
    msg[pos++] = 0x00;                               // fixed sub-byte func 0x4C's own wire format always carries

    if (rawPayloadLen > sizeof(msg) - pos - 1) {
        LOG_ERROR("Program Data Dump payload too large (%u bytes) — not sending\n", (unsigned)rawPayloadLen);
        return;
    }
    memcpy(msg + pos, rawPayload, rawPayloadLen);
    pos       += rawPayloadLen;
    msg[pos++] = MIDI_SYSEX_END;
    midi_send(msg, pos);
    LOG_DEBUG("Sent Program Data Dump (func 0x4C), bank=%c program=%u, %u byte payload — writes a stored slot directly\n",
              bank ? 'B' : 'A', (unsigned)progNumber, (unsigned)rawPayloadLen);
}

// notes §68
#define CC_DEBOUNCE_MS    150.0

static double monotonic_ms(void) {
    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC, &now);
    return ((double)now.tv_sec * 1000.0) + ((double)now.tv_nsec / 1e6);
}

bool synth_handle_cc(uint8_t cc, uint8_t value) {
    tPanelDial * dial = find_panel_dial_by_cc(synth_panel_config(), cc);

    if (!dial) {
        return false;
    }

    if (dial->ccLsbNumber == 0) {
        // notes §69
        if ((dial->nativeMax != 0) && (dial->display == dialDisplayNames)) {
            dial->hasPendingCc    = true;
            dial->pendingRawValue = value;
            dial->pendingSinceMs  = monotonic_ms();
        } else {
            apply_dial_wire_value(dial, value, dial->nativeMax);
        }
    } else if (cc == dial->ccNumber) {
        // notes §70
        dial->ccMsbLatched = value;
    } else {
        dial->ccLsbLatched = value;
        dial->value        = ((uint32_t)dial->ccMsbLatched << 7) | dial->ccLsbLatched;
    }
    // notes §71
    midi_arm_state_dump_debounce();
    return true;
}

// notes §72
void synth_flush_pending_cc(void) {
    tPanelConfig * cfg = synth_panel_config();
    double         now = monotonic_ms();

    for (uint32_t s = 0; s < cfg->sectionCount; s++) {
        tPanelSection * section = &cfg->sections[s];

        for (uint32_t d = 0; d < section->dialCount; d++) {
            tPanelDial * dial = &section->dials[d];

            if (dial->hasPendingCc && ((now - dial->pendingSinceMs) >= CC_DEBOUNCE_MS)) {
                dial->hasPendingCc = false;
                apply_dial_wire_value(dial, dial->pendingRawValue, dial->nativeMax);
                synthlib_request_redraw();
            }
        }
    }
}

// notes §73
void synth_flush_pending_dump_sends(void) {
    tPanelConfig * cfg = synth_panel_config();
    double         now = monotonic_ms();

    for (uint32_t s = 0; s < cfg->sectionCount; s++) {
        tPanelSection * section = &cfg->sections[s];

        for (uint32_t d = 0; d < section->dialCount; d++) {
            tPanelDial * dial = &section->dials[d];

            if (dial->hasPendingDumpSend && ((now - dial->pendingDumpSinceMs) >= CC_DEBOUNCE_MS)) {
                dial->hasPendingDumpSend        = false;
                dial->dumpSendAwaitingFreshData = true;

                if (!gAwaitingFreshDumpForPatch) {
                    gAwaitingFreshDumpForPatch = true;
                    synth_request_state_dump();
                }
            }
        }
    }
}

// notes §74
static void handle_kronos_parameter_change(const uint8_t * data, uint32_t length) {
    uint32_t       base    = 4 + synth_panel_config()->manufacturerIdLen;

    if (length < base + 8 + 1) { // 5 addressing bytes + 3 value bytes + trailing F7
        LOG_DEBUG("Kronos Parameter Change too short (len=%u)\n", (unsigned)length);
        return;
    }
    uint32_t       typ     = data[base] & 0x7F;
    uint32_t       soc     = data[base + 1] & 0x7F;
    uint32_t       sub     = data[base + 2] & 0x7F;
    uint32_t       pid     = data[base + 3] & 0x7F;
    uint32_t       idx     = data[base + 4] & 0x7F;
    uint32_t       v21     = ((uint32_t)(data[base + 5] & 0x7F) << 14)
                             | ((uint32_t)(data[base + 6] & 0x7F) << 7)
                             | (uint32_t)(data[base + 7] & 0x7F);

    if (v21 & 0x100000) {
        v21 |= 0xFFE00000; // sign-extend 21-bit two's complement
    }
    int32_t        value   = (int32_t)v21;
    tPanelConfig * cfg     = synth_panel_config();
    tPanelDial *   dial    = find_panel_dial_by_kronos_param(cfg, typ, soc, sub, pid, idx);
    int32_t        variant = -1;

    // notes §91
    for (uint32_t v = 0; !dial && (v < cfg->pageVariantCount); v++) {
        tPanelDial * base = find_panel_dial_by_kronos_param(cfg, typ - (uint32_t)cfg->pageVariants[v].typDelta, soc, sub, pid, idx);

        if (base && section_variant(panel_section_of_dial(cfg, base), (int32_t)v)) {
            dial    = base;
            variant = (int32_t)v;
        }
    }

    if (dial) {
        tPanelSection * section = panel_section_of_dial(cfg, dial);

        cache_store_dial_value(dial, section, variant, value);

        if (variant == section_active_variant(section)) {
            apply_dial_wire_value(dial, (uint32_t)value, dial->nativeMax);
        }
        synthlib_request_redraw();
        LOG_DEBUG("Kronos Parameter Change: TYP=%u SOC=%u SUB=%u PID=%u IDX=%u value=%d -> dial \"%s\"\n",
                  (unsigned)typ, (unsigned)soc, (unsigned)sub, (unsigned)pid, (unsigned)idx, (int)value, dial->label);
    } else {
        LOG_DEBUG("Kronos Parameter Change (no matching dial): TYP=%u SOC=%u SUB=%u PID=%u IDX=%u value=%d\n",
                  (unsigned)typ, (unsigned)soc, (unsigned)sub, (unsigned)pid, (unsigned)idx, (int)value);
    }
}

static void handle_kronos_message(const uint8_t * data, uint32_t length, uint8_t funcId) {
    if (funcId == 0x43) {
        handle_kronos_parameter_change(data, length);
        return;
    }

    if ((funcId == 0x42) || (funcId == 0x4E)) { // Mode Data (our request) / Mode Change (front panel)
        uint32_t modeAt = 4 + synth_panel_config()->manufacturerIdLen;

        if (length > modeAt + 1) {
            atomic_store(&gReportedMode, (int32_t)(data[modeAt] & 0x0F));
            LOG_DEBUG("Kronos mode %u\n", (unsigned)(data[modeAt] & 0x0F));

            if (funcId == 0x4E) {
                midi_arm_state_dump_debounce(); // the edit object changed with the mode
            }
        }
        return;
    }

    if (funcId != 0x75) {
        LOG_DEBUG("Kronos SysEx unhandled func 0x%02X (len=%u)\n", (unsigned)funcId, (unsigned)length);
        return;
    }
    // notes §75
    uint32_t        headerLen  = 4 + synth_panel_config()->manufacturerIdLen;

    if (length < headerLen + 2 + 1) { // +2 (obj, version) +1 (trailing F7)
        LOG_DEBUG("Kronos Current Object Dump too short (len=%u)\n", (unsigned)length);
        return;
    }
    uint8_t         obj        = data[headerLen];
    uint8_t         version    = data[headerLen + 1];

    if (obj != 0x00) {
        LOG_DEBUG("Kronos Current Object Dump: obj=0x%02X (not Program), ignoring for now\n", (unsigned)obj);
        return;
    }
    const uint8_t * payload    = data + headerLen + 2;
    uint32_t        payloadLen = length - (headerLen + 2) - 1; // exclude trailing F7
    static uint8_t  decoded[8192];
    uint32_t        decodedLen = decode_7to8(payload, payloadLen, decoded, sizeof(decoded));

    if (decodedLen < 24) {
        LOG_DEBUG("Kronos Program dump too short after decode (decodedLen=%u)\n", (unsigned)decodedLen);
        return;
    }
    // notes §76
    pthread_mutex_lock(&gProgCacheLock);
    memcpy(gProgCache, decoded, decodedLen);
    gProgCacheLen = decodedLen;
    extract_prog_info(gProgCache, gProgCacheLen);
    pthread_mutex_unlock(&gProgCacheLock);
    LOG_DEBUG("Kronos Current Object Dump decoded: version=%u decodedLen=%u\n", (unsigned)version, (unsigned)decodedLen);
}

void synth_handle_message(const uint8_t * data, uint32_t length) {
    if (synth_panel_config()->moogStyleDump) {
        // notes §77
        if (!is_moog_sysex(data, length)) {
            LOG_DEBUG("Ignoring non-target SysEx (len=%u)\n", (unsigned)length);
            return;
        }
        moog_learn_device_id(data);
        uint8_t mode = data[4];

        if (mode == 0x02) {
            handle_moog_panel_dump(data, length);
        } else if (mode == 0x03) {
            handle_moog_single_preset_dump(data, length);
        } else if (mode == 0x01) {
            handle_moog_all_presets_dump(data, length);
        } else {
            LOG_DEBUG("Moog SysEx unhandled mode 0x%02X\n", (unsigned)mode);
        }
        synthlib_request_redraw();
        return;
    }

    if (!is_synth_sysex(data, length)) {
        LOG_DEBUG("Ignoring non-target SysEx (len=%u)\n", (unsigned)length);
        return;
    }
    uint8_t funcId = data[3 + synth_panel_config()->manufacturerIdLen];

    if (!synth_panel_config()->supportsKorgProgramDump) {
        handle_kronos_message(data, length, funcId);
        synthlib_request_redraw();
        return;
    }
    LOG_DEBUG("Synth SysEx func=0x%02X len=%u\n", (unsigned)funcId, (unsigned)length);

    switch (funcId) {
        case SYNTH_FUNC_CURR_PROG_DUMP:
            handle_curr_prog_dump(data, length);
            break;
        case SYNTH_FUNC_PROG_DUMP:
            handle_prog_dump(data, length);
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

            if ((synth_panel_config()->bankMapReplyFunc >= 0) && (funcId == (uint8_t)synth_panel_config()->bankMapReplyFunc)) {
                handle_bank_map_reply(data, length);
                break;
            }
            LOG_DEBUG("Synth unhandled func 0x%02X\n", (unsigned)funcId);
            break;
    }
    synthlib_request_redraw();
}

// notes §78
static tPanelDial * gPendingCcDial     = NULL;
static uint8_t      gPendingCcValue    = 0;

static tPanelDial * gPendingCcPairDial = NULL;
static uint8_t      gPendingCcPairMsb  = 0;
static uint8_t      gPendingCcPairLsb  = 0;

static tPanelDial * gPendingParamDial  = NULL;
static uint32_t     gPendingParamValue = 0;

void synth_flush_pending_param_send(void) {
    if (synth_backup_sweep_request_in_flight()) {
        return;
    }

    if (gPendingCcDial) {
        midi_send_cc(gDevice.id, (uint8_t)gPendingCcDial->ccNumber, gPendingCcValue);
        gPendingCcDial = NULL;
    }

    if (gPendingCcPairDial) {
        midi_send_cc(gDevice.id, (uint8_t)gPendingCcPairDial->ccNumber, gPendingCcPairMsb);
        midi_send_cc(gDevice.id, (uint8_t)gPendingCcPairDial->ccLsbNumber, gPendingCcPairLsb);
        gPendingCcPairDial = NULL;
    }

    if (gPendingParamDial) {
        uint16_t wireValue = gPendingParamDial->wireSigned
                            ? encode_signed_param_wire_value(gPendingParamDial, gPendingParamValue)
                            : (uint16_t)gPendingParamValue;

        synth_send_parameter_change((uint8_t)gPendingParamDial->paramGroup, (uint16_t)dial_param_id(gPendingParamDial), wireValue);
        gPendingParamDial = NULL;
    }
}

void synth_set_panel_dial_value(tPanelDial * dial, uint32_t displayValue) {
    if (!dial) {
        return;
    }
    uint32_t max = synth_dial_max(dial);

    if ((max > 0) && (displayValue >= max)) {
        displayValue = max - 1;
    }

    // notes §79
    if (dial->linkedMaxDialId[0] != '\0') {
        tPanelDial * other = find_panel_dial_anywhere(synth_panel_config(), dial->linkedMaxDialId);

        if (other) {
            uint32_t otherVal = get_panel_dial_value(other);

            if (displayValue > otherVal) {
                displayValue = otherVal;
            }
        }
    }

    if (dial->linkedMinDialId[0] != '\0') {
        tPanelDial * other = find_panel_dial_anywhere(synth_panel_config(), dial->linkedMinDialId);

        if (other) {
            uint32_t otherVal = get_panel_dial_value(other);

            if (displayValue < otherVal) {
                displayValue = otherVal;
            }
        }
    }
    uint32_t storageValue = (uint32_t)((int32_t)displayValue + dial->storageOffset);

    if (storageValue == dial->value) {
        return;
    }
    dial->value = storageValue;

    // notes §80
    if ((dial->nativeMax != 0) && (dial->max > 1)) {
        dial->nativeValue = (uint8_t)(displayValue * dial->nativeMax / (dial->max - 1));
    }

    if (dial->ccNumber != 0) {
        bool sweepInFlight = synth_backup_sweep_request_in_flight();

        if (dial->ccLsbNumber != 0) {
            // 14-bit CC pair — see the ccLsbNumber comment in panelConfig.h.
            // Keep the latches in sync so a later single-half incoming update
            // recombines against the half we just sent, not a stale one.
            dial->ccMsbLatched = (uint8_t)((storageValue >> 7) & 0x7F);
            dial->ccLsbLatched = (uint8_t)(storageValue & 0x7F);

            if (sweepInFlight) {
                // Defer — see gPendingCcPairDial's own comment above.
                gPendingCcPairDial = dial;
                gPendingCcPairMsb  = dial->ccMsbLatched;
                gPendingCcPairLsb  = dial->ccLsbLatched;
            } else {
                midi_send_cc(gDevice.id, (uint8_t)dial->ccNumber, dial->ccMsbLatched);
                midi_send_cc(gDevice.id, (uint8_t)dial->ccLsbNumber, dial->ccLsbLatched);
            }
        } else {
            // notes §81
            uint8_t wireValue = (dial->nativeMax != 0) ? dial->nativeValue : (uint8_t)storageValue;

            if (sweepInFlight) {
                // Defer — see gPendingCcDial's own comment above.
                gPendingCcDial  = dial;
                gPendingCcValue = wireValue;
            } else {
                midi_send_cc(gDevice.id, (uint8_t)dial->ccNumber, wireValue);
            }
        }

        // notes §82
        if (synth_panel_config()->moogStyleDump && (dial->dumpBitWidth > 0)) {
            synth_patch_moog_dump_cache(dial, synth_encode_dump_raw_value(dial, displayValue));
        }
    } else if (synth_panel_config()->moogStyleDump && (dial->dumpBitWidth > 0)) {
        // notes §83
        uint32_t rawValue = synth_encode_dump_raw_value(dial, displayValue);

        dial->hasPendingDumpSend  = true;
        dial->pendingDumpRawValue = rawValue;
        dial->pendingDumpSinceMs  = monotonic_ms();
    } else if (dial->hasKronosParam) {
        // notes §84
        tPanelSection *      section = panel_section_of_dial(synth_panel_config(), dial);
        int32_t              variant = section_active_variant(section);
        const tPageVariant * v       = section_variant(section, variant);

        synth_send_kronos_parameter_change(dial->kronosTyp + (uint32_t)(v ? v->typDelta : 0), dial->kronosSoc, dial->kronosSub,
                                           dial->kronosPid, dial->kronosIdx, (int32_t)storageValue);
        cache_store_dial_value(dial, section, variant, (int32_t)storageValue);
    } else if (synth_backup_sweep_request_in_flight()) {
        // Defer — see gPendingParamDial's own comment above.
        gPendingParamDial  = dial;
        gPendingParamValue = storageValue;
    } else {
        uint16_t        wireValue = dial->wireSigned ? encode_signed_param_wire_value(dial, storageValue) : (uint16_t)storageValue;
        tPanelSection * section   = panel_section_of_dial(synth_panel_config(), dial);

        synth_send_parameter_change((uint8_t)dial->paramGroup, (uint16_t)dial_param_id(dial), wireValue);
        cache_store_dial_value(dial, section, section_active_variant(section), (int32_t)storageValue);
    }
    synthlib_request_redraw();
}

uint32_t synth_effective_name_maxlen(void) {
    tPanelConfig * cfg    = synth_panel_config();
    uint32_t       maxLen = cfg->moogStyleDump ? cfg->panelNameLen : cfg->progNameLen;

    return (maxLen < (SYNTH_PROG_NAME_MAXLEN - 1)) ? maxLen : (SYNTH_PROG_NAME_MAXLEN - 1);
}

// notes §85
static void set_prog_name_display(const char * flat, uint32_t lineWidth) {
    uint32_t outLen    = 0;
    uint32_t lineChars = 0;

    for (const char * p = flat; (*p != '\0') && (outLen < sizeof(gDevice.progName) - 1); p++) {
        gDevice.progName[outLen++] = *p;
        lineChars++;

        if ((lineWidth > 0) && (lineChars == lineWidth) && (*(p + 1) != '\0') && (outLen < sizeof(gDevice.progName) - 1)) {
            gDevice.progName[outLen++] = '\n';
            lineChars                  = 0;
        }
    }

    gDevice.progName[outLen] = '\0';
}

void synth_set_program_name(const char * newName) {
    if (!newName) {
        return;
    }
    tPanelConfig * cfg    = synth_panel_config();
    uint32_t       maxLen = synth_effective_name_maxlen();

    if (maxLen == 0) {
        return; // connected device's config declares no name field to send
    }
    char           padded[SYNTH_PROG_NAME_MAXLEN];
    uint32_t       i;

    for (i = 0; (i < maxLen) && (newName[i] != '\0'); i++) {
        padded[i] = newName[i];
    }

    for ( ; i < maxLen; i++) {
        padded[i] = ' '; // pad to the field's fixed wire width
    }

    padded[maxLen] = '\0';

    set_prog_name_display(padded, cfg->nameLineWidth); // optimistic local update — see this function's own header comment (synthComms.h)

    if (cfg->moogStyleDump) {
        if ((cfg->panelNameOffset < 0) || (gLastMoogDumpLen == 0)) {
            // notes §86
            return;
        }
        gPendingProgNameLen        = maxLen;
        memcpy(gPendingProgNameRaw, padded, maxLen);
        gProgNameAwaitingFreshData = true;

        if (!gAwaitingFreshDumpForPatch) {
            gAwaitingFreshDumpForPatch = true;
            synth_request_state_dump();
        }
    } else {
        for (uint32_t c = 0; c < maxLen; c++) {
            synth_send_parameter_change(SYNTH_PARAM_GROUP_PROG, (uint16_t)(c + 1), (uint16_t)(uint8_t)padded[c]);
        }
    }
}
