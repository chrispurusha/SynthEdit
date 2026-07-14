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
#include <time.h>

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
// Byte 0 of each group holds the MSBs — bit0 = MSB of data byte 7n+0 (the
// FIRST data byte in the group, right after this MSB byte), bit6 = MSB of
// data byte 7n+6 (the LAST). Bit index == in-group data-byte index. Fixed
// 2026-07-13 — was previously (6-j) (mirrored: bit6 for the first data byte,
// bit0 for the last), confirmed backwards against the MIDI Implementation
// PDF's own diagram (p.19: MSB byte's boxes read left-to-right as bit6..bit0,
// labelled "7n+6,5,4,3,2,1,0" — i.e. bit6 pairs with 7n+6, bit0 with 7n+0).
// Real-world symptom that led here: F1 Mod EG (dumpOffset 319, group n=45,
// in-group index 4) always read back as A.EG (its max/clamp value) after a
// GUI write that hardware confirmed took (e.g. wrote EG1, hardware displayed
// EG1) — root cause was F1 Lo Int's (dumpOffset 317, in-group index 2) own
// true MSB landing on the WRONG bit under the old mirrored mapping, so
// whenever Lo Int held a large-magnitude value, its stray MSB got OR'd onto
// F1 Mod EG's byte instead of its own, pushing Mod EG's raw value past its
// valid range and clamping it to A.EG. Every dump-decoded byte near another
// byte needing bit7 (any raw value >=128 — e.g. the "Int" -99..+99 family,
// stored as 0-198) was equally at risk, not just this one field. Both
// encode_8to7() below and this function were wrong the same mirrored way, so
// they round-tripped correctly against EACH OTHER (masking the bug for
// anything only ever built and read back within this app) — only breaks
// against the real synth's own byte-for-byte-correct dumps/writes.
// Returns number of decoded bytes written.
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

// ── 8-to-7 bit encoding ───────────────────────────────────────────────────────
// See decode_7to8()'s own comment above — bit index == in-group data-byte
// index, fixed the same day/same reason (was (6-j), mirrored).
// Returns number of MIDI bytes written (always ceil(dataLen/7)*8).
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
// nativeMax is an explicit parameter, not read from dial->nativeMax directly,
// because a dial can have TWO independent wire representations needing two
// different native scales: a CC byte (0-127, needs nativeMax=127 threshold
// quantization down to `max` positions) and a Moog dump bit-field (already a
// direct 0..2^dumpBitWidth-1 index, 1:1 with `max` positions for a simple
// toggle — nativeMax=127 would wrongly crush a raw 0/1 down to display 0
// every time, discovered 2026-07-08 trying to add dumpOffset to an existing
// CC-driven toggle). extract_moog_panel_info() below passes dial->
// dumpNativeMax (falling back to dial->nativeMax when that's 0, e.g.
// Filter A/B Pole Select's own dump-only dials, which only ever have ONE
// wire path so nativeMax alone already means the right thing) — every other
// caller keeps passing dial->nativeMax exactly as before this existed.
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
                // dumpNativeMax falls back to nativeMax exactly like
                // extract_moog_panel_info()'s own identical fallback already
                // does (synthComms.c, below) — added here 2026-07-13, a real
                // gap found live on the Z1's f1cut/f1res/f2cut/f2res: their
                // CC wire byte genuinely spans the dial's own full 0-127
                // range (confirmed against real hardware), but the DUMPED
                // byte is a separate, genuinely-narrower 0-99 native value
                // per the official parameter table — two different native
                // scales for the same dial, exactly what dumpNativeMax
                // exists for, just never wired into this (the Korg-style,
                // non-Moog) decode path when it was first added for
                // Voyager. Purely additive: any dial that doesn't set
                // dumpNativeMax (every Z1 dial except those 4, and any
                // other device using this same generic decode) falls
                // through to dial->nativeMax exactly as before.
                apply_dial_wire_value(dial, raw, (dial->dumpNativeMax != 0) ? dial->dumpNativeMax : dial->nativeMax);
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

// Inverse of read_bitpacked_field() above — same continuous 7-bit-per-byte
// bitstream addressing, writing bits into an existing captured dump instead
// of reading them out. Used to patch a single dial's value into a cached
// live dump before resending the whole thing (see gLastMoogDump / synth_
// patch_and_resend_moog_dump() below) — the Voyager has no per-parameter
// "set this one value" SysEx, only a whole-dump load, confirmed 2026-07-08
// by capturing, patching just Filter A's 2 bits, sending it back, and
// re-requesting a dump: the patched value round-tripped exactly.
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

// Inverse of extract_moog_panel_info()'s own decode (raw -> [dumpInvert] ->
// apply_dial_wire_value's native/display scaling) — turns a dial's current
// display value back into the RAW bits its dump field expects. Needed by
// both synth_apply_pending_dump_patches() (a dump-only dial) and
// synth_patch_moog_dump_cache()'s CC-side use below (added 2026-07-09,
// keeping the cached dump in sync with CC changes too) — a dial can have a
// DIFFERENT native scale on its dump side (dumpNativeMax) than its CC side
// (nativeMax), same reasoning as apply_dial_wire_value()'s own
// dumpNativeMax comment, and dumpInvert (bitwise NOT across the field's full
// width) is its own inverse, so re-applying it here undoes the same flip
// the decode applied. Checked 2026-07-09 against mwDestination's own
// hardware-confirmed raw values: display 0 ("Pitch") -> raw 7, display 5
// ("LFO / PGM") -> raw 2, both round-trip exactly through this formula.
static uint32_t synth_encode_dump_raw_value(tPanelDial * dial, uint32_t displayValue) {
    uint32_t totalWidth = dial->dumpBitWidth + dial->dumpBitWidth2;
    uint32_t dumpMax    = (dial->dumpNativeMax != 0) ? dial->dumpNativeMax : dial->nativeMax;
    uint32_t native;

    if ((dumpMax != 0) && (dial->max > 1)) {
        native = ((displayValue * dumpMax) + ((dial->max - 1) / 2)) / (dial->max - 1);
    } else {
        // plain continuous/named dial — dump value is the display value,
        // shifted by storageOffset for a dial whose wire range doesn't
        // start at 0 (e.g. Voyager tsGateCtrl's 64-127+Off, storageOffset=
        // 64). Mirrors apply_dial_wire_value()'s decode side and the Z1
        // param-change encode path below, both of which already add
        // storageOffset — this branch just never had a dial exercise
        // BOTH storageOffset and a dump-only Moog field until tsGateCtrl,
        // 2026-07-13: found live, dragging the dial changed the on-screen
        // display but never reached the hardware, because the un-offset
        // displayValue (0-64) was being sent instead of the actual wire
        // value (64-128) tsGateCtrl's own dumpOffset field expects.
        native = (uint32_t)((int32_t)displayValue + dial->storageOffset);
    }

    if (dial->dumpInvert) {
        native = (~native) & ((totalWidth < 32) ? ((1u << totalWidth) - 1) : 0xFFFFFFFFu);
    }
    return native;
}

// Decodes a name field into gDevice.progName, if `offset` >= 0 (the
// tPanelConfig field comment explains why Panel Dump and Single Preset Dump
// each need their own offset/bitOffset/len rather than sharing one). Each
// character is 8 bits read from the same continuous 7-bit-per-byte
// bitstream the numeric panel fields use (read_bitpacked_field() above), one
// after another starting at offset/bitOffset.
//
// Reverse-engineered against five real captures (Voyager preset 1, "FILTER
// BUBBLES"; a Panel Dump, "FROM A DISTANCE"; preset 2, "Really Heavy"; a
// Panel Dump, "Velocity"/"Temple Bells"; and a Panel Dump, "Floating Mod"/
// "Steel Guitar"): the raw field is two fixed-width 12-char lines — matching
// the Voyager's 2-line LCD — with NO dedicated separator byte anywhere in
// it. Each line's 12th byte (field index 11 and 23) is really just that
// line's own 12th character, with its high bit set (e.g. 'y' 0x79 -> 0xF9,
// 's' 0x73 -> 0xF3) — "Really Heavy" and "Temple Bells" are what exposed
// this: each has a real (non-space) 12th character on one of its lines,
// which an earlier version of this code/config mistook for a fixed 0xA0
// separator marker (silently dropped) or ran past the field's then-assumed
// 20-char length (silently truncated) respectively. "FILTER BUBBLES" and
// "FROM A DISTANCE" both worked under that wrong model purely by
// coincidence: neither line runs past 11 real characters, so each line's
// 12th byte is always plain padding. Masking off bit 7 before checking
// printability — rather than treating any high-bit byte as whitespace —
// decodes those four correctly.
//
// "Floating Mod"/"Steel Guitar" (both lines exactly 12 real characters, no
// padding on either) exposed a second problem: with no separator byte and
// no whitespace at the boundary either, there's nothing byte-level to hang a
// line break on — decoding straight through produces "Floating ModSteel
// Guitar" on one line. `lineWidth` (from tPanelConfig.nameLineWidth — 0 for
// non-Voyager Moog-style devices, meaning "one line, no forced break") fixes
// that by forcing a '\n' after every lineWidth characters regardless of
// content, so synth_render() (synthGraphics.cpp) can show gDevice.progName
// as separate lines matching the device's own display, and
// synth_backup_capture_dump() (synthBackup.c) can build a filename by just
// dropping the '\n' (not substituting a space — see that function's own
// comment for why).
//
// The forced '\n' is inserted on top of the SAME whitespace-collapsing this
// function already does for every other non-printable byte (runs collapse
// to one space) — it does NOT additionally strip a real trailing space that
// happens to land right before it. That distinction matters: a short first
// line like "TIME FOR" (8 real characters) still has 4 bytes of real 0x20
// padding out to the 12-char line width, and collapsing that run keeps
// exactly one of those spaces — so the decoded field is "TIME FOR \nSURFIN'",
// and dropping just the '\n' for a filename correctly yields "TIME FOR
// SURFIN'". A full first line like "Floating Mod" (exactly 12 real
// characters, no padding at all) has no such byte to collapse, so nothing
// survives before its '\n' and the filename is "Floating ModSteel Guitar"
// — also correct, because that's genuinely what the raw data contains: real
// hardware capture (2026-07-07) confirms there is no dedicated separator
// byte anywhere in the field, in either case. An earlier version of this
// function unconditionally trimmed each line's trailing whitespace down to
// nothing before its forced '\n', which was fine for on-screen display
// (trailing whitespace before a line break is invisible either way) but
// silently discarded the real space "TIME FOR"'s filename needed.
// outName/outNameSize added 2026-07-11 (was hardcoded to gDevice.progName) —
// synth_backup_flush_name_sweep() (synthBackup.c) needs to decode a name
// from a SEPARATE, non-current-live-buffer preset's reply (a Load/Store
// Patch to Bank picker's own name sweep) without touching gDevice.progName,
// which is reserved for whatever the live edit buffer actually shows. Every
// existing call site below still passes gDevice.progName/sizeof(...),
// unchanged in behaviour.
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

// Extracts just the Category value's display name from a captured Moog-style
// dump (Panel Dump OR Single Preset Dump — both share the same continuous
// bit-packed payload shape, skip=1 byte for F0 either way, unlike Korg's
// funcId-dependent header) — the Moog counterpart to synth_decode_korg_category()
// (synthComms.h), and the other half of what makes the Load/Store Patch
// from Bank picker's category column generic across both device families
// (2026-07-14). Same find_panel_dial_by_label() lookup as the Korg version
// (matches label="Category" regardless of the dial's own id — Voyager's is
// `soundCategory`, not `category`) but reads it via read_bitpacked_field()
// (dumpOffset/dumpBitOffset/dumpBitWidth[2]/dumpInvert), the same bit-packed
// shape extract_moog_panel_info() above already uses for every other dial.
// outCategory left untouched (caller should default it to "" first) if this
// device has no such dial, or the decoded value is out of range of that
// dial's own names= list.
void synth_decode_moog_category(const uint8_t * data, uint32_t length, char * outCategory, size_t outCategorySize) {
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
    const uint8_t * payload    = data + 1;    // skip F0, matches every other Moog dump handler
    uint32_t        payloadLen = length - 2;  // exclude leading skip + trailing F7
    uint32_t        raw        = read_bitpacked_field(payload, payloadLen, dial->dumpOffset, dial->dumpBitOffset, dial->dumpBitWidth);
    uint32_t        totalWidth = dial->dumpBitWidth;

    if (dial->dumpBitWidth2 > 0) {
        uint32_t chunk2 = read_bitpacked_field(payload, payloadLen, dial->dumpOffset2, dial->dumpBitOffset2, dial->dumpBitWidth2);
        raw        |= chunk2 << dial->dumpBitWidth;
        totalWidth += dial->dumpBitWidth2;
    }

    if (dial->dumpInvert) {
        raw = (~raw) & ((totalWidth < 32) ? ((1u << totalWidth) - 1) : 0xFFFFFFFFu);
    }

    if (raw < dial->nameCount) {
        strncpy(outCategory, dial->names[raw], outCategorySize - 1);
        outCategory[outCategorySize - 1] = '\0';
    }
}

// Same role as extract_prog_info() above, but for Moog's bit-packed dump
// shape — every dial with dumpBitWidth > 0 gets its value from
// read_bitpacked_field() instead of the single-byte dumpShift/dumpMask path.
// Also decodes the name field (see extract_moog_name() above) — a Panel
// Dump IS tied to a name after all (unlike what its own spec comment
// suggests: it just doesn't call it a "preset" name since Panel Dump
// reflects the live edit buffer, not necessarily an unmodified stored
// preset) — confirmed by capture, see panelNameOffset's own comment in
// panelConfig.h.
// Same role as a dial's own dumpSendAwaitingFreshData (panelConfig.h), for a
// pending program-name edit (synth_set_program_name() below) instead of a
// dial — there's no per-dial struct to hang this off of for something that
// isn't a dial at all. Unlike a dial's own two-phase debounce-then-fetch
// (hasPendingDumpSend), a name edit only ever fires once per commit (Enter
// key, not a drag), so there's no rapid-fire case to debounce — this goes
// straight to "awaiting fresh data" the moment synth_set_program_name() is
// called, requesting a fetch immediately (folding into one already in
// flight via gAwaitingFreshDumpForPatch, same as a dial would). Declared
// here, above extract_moog_panel_info() (its own first reader) rather than
// alongside gAwaitingFreshDumpForPatch further down — this file has no
// header-declared forward prototypes for file-scope statics, so a reader
// earlier in the file can't see one declared later.
static bool     gProgNameAwaitingFreshData                  = false;
static uint8_t  gPendingProgNameRaw[SYNTH_PROG_NAME_MAXLEN] = {0};
static uint32_t gPendingProgNameLen                         = 0;

static void extract_moog_panel_info(const uint8_t * payload, uint32_t payloadLen) {
    tPanelConfig * cfg     = synth_panel_config();
    uint32_t       updated = 0;

    if (!gProgNameAwaitingFreshData) {
        // Skip re-decoding the name field while a just-typed edit is queued
        // to be merged into THIS very dump and sent back (see
        // gProgNameAwaitingFreshData's own comment above) — same "don't
        // stomp a pending edit with the old, pre-change value this fresh
        // reply still carries" reasoning as the per-dial skip below,
        // applied to the name field instead of a dial's value.
        synth_decode_moog_name(payload, payloadLen, cfg->panelNameOffset, cfg->panelNameBitOffset, cfg->panelNameLen, cfg->nameLineWidth, gDevice.progName, sizeof(gDevice.progName));
    }

    for (uint32_t s = 0; s < cfg->sectionCount; s++) {
        tPanelSection * section = &cfg->sections[s];

        for (uint32_t d = 0; d < section->dialCount; d++) {
            tPanelDial * dial = &section->dials[d];

            if (dial->dumpBitWidth == 0) {
                continue;
            }

            if (dial->dumpSendAwaitingFreshData || dial->hasPendingDumpSend) {
                // A user-set value for this dial is either already queued to
                // be merged into THIS very dump (dumpSendAwaitingFreshData —
                // see its own comment, panelConfig.h) or still settling in
                // the debounce window that precedes that (hasPendingDumpSend
                // — see its own comment, panelConfig.h) — don't let the
                // decode below stomp dial->value with the old, pre-change
                // value this fresh reply still carries for this one field in
                // EITHER case. A dump can legitimately arrive mid-debounce
                // (e.g. preset navigation's own state-dump request, or a
                // manual Sync, landing within the same ~150ms window as an
                // in-flight dump-only dial edit) — previously only the
                // second phase was covered here, so that race visibly
                // reverted the dial on screen even though
                // pendingDumpRawValue (captured once at edit time,
                // untouched by this) meant the value eventually SENT to
                // hardware was always correct regardless. Found 2026-07-11
                // auditing this mechanism, no hardware repro needed — the
                // gap was evident from reading the guard against
                // hasPendingDumpSend's own debounce window. synth_apply_
                // pending_dump_patches() (called right after this function
                // returns) is what actually applies the pending value, once
                // for every dial in the second phase — a dial still only in
                // the first phase here just keeps waiting for its own
                // debounce to elapse, unaffected by this dump.
                continue;
            }
            uint32_t raw        = read_bitpacked_field(payload, payloadLen, dial->dumpOffset,
                                                       dial->dumpBitOffset, dial->dumpBitWidth);

            uint32_t totalWidth = dial->dumpBitWidth;

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
                // Some toggles report inverted polarity in the dump vs their
                // own CC's On/Off sense (e.g. Ext On/Osc On: dump bit 0 means
                // On) — confirmed against real hardware 2026-07-08, physically
                // toggling each switch and diffing the dump both ways.
                raw = (~raw) & ((totalWidth < 32) ? ((1u << totalWidth) - 1) : 0xFFFFFFFFu);
            }
            // dumpNativeMax lets a dial with BOTH a CC and a dump bit use a
            // different native scale for each — see apply_dial_wire_value()'s
            // own comment for why nativeMax alone (sized for the CC byte)
            // would wrongly crush a raw dump bit. 0 (unset) falls back to
            // nativeMax, unchanged for dump-only dials like Filter A/B Pole
            // Select that only ever have this one wire path.
            //
            // oldValue/oldRaw captured before the decode below overwrites
            // them — added 2026-07-09 to make it visible when a dial's
            // last-known CC-tracked value doesn't match what a fresh Panel
            // Dump says (owner noticed a small drift after turning a
            // physical knob directly, not present when the GUI itself sends
            // the CC — logged only when they actually differ, so a normal
            // Sync with nothing changed stays quiet).
            uint32_t oldValue = dial->value;

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

// Format: F0 <mfrId> 3g 46 4C ub pp 00 [7-bit encoded data...] F7 — a
// SPECIFIC stored preset's data (contrast handle_curr_prog_dump() above,
// always the live edit buffer). Deliberately does NOT call
// extract_prog_info() — that writes straight into the live dial state,
// which would silently corrupt the on-screen panel with some OTHER
// preset's values every time a name-sweep or by-number backup touches this
// reply. Just forwards the raw bytes to synth_backup_capture_dump() (a
// no-op unless a Korg program-by-number fetch is actually pending) — any
// decoding a caller needs happens on ITS OWN copy of these bytes via
// synth_decode_korg_name() below, same "forward raw bytes, let the backup
// layer decode what it needs" split handle_moog_single_preset_dump() (and
// its own eBackupExpectPreset) already uses for Voyager.
static void handle_prog_dump(const uint8_t * data, uint32_t length) {
    synth_backup_capture_dump(data, length, eBackupExpectKorgProgram);
    LOG_DEBUG("Received Program Data Dump (len=%u)\n", (unsigned)length);
}

// Shared plumbing for synth_decode_korg_name()/synth_decode_korg_category()
// below — both need the same funcId-dependent header skip and 7-to-8 decode
// of a captured Program Data Dump (0x4C) or Current Program Dump (0x40)
// reply before extracting anything from it. Returns false (decoded/
// *outDecodedLen left untouched) for anything that isn't recognisably one
// of those two replies.
static bool korg_decode_prog_dump(const uint8_t * data, uint32_t length, uint8_t * decoded, uint32_t decodedCap, uint32_t * outDecodedLen) {
    if (!is_synth_sysex(data, length)) {
        return false;
    }
    tPanelConfig *  cfg        = synth_panel_config();
    uint32_t        funcPos    = 3 + cfg->manufacturerIdLen;
    uint8_t         funcId     = data[funcPos];
    // 0x40's own extra sub-byte ("01") is 1 byte; 0x4C's own header (Unit/
    // Bank byte, Program No. byte, a fixed "00" byte) is 3 — see
    // handle_curr_prog_dump()/handle_prog_dump()'s own comments above for
    // the full wire shapes this mirrors.
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
}

// Extracts just the Category value's display name from a captured Program
// Data Dump reply — the picker counterpart to synth_decode_korg_name()
// above, used so Load/Store Patch from Bank can show category alongside
// each program's name (2026-07-14 user request, later made a common
// mechanism shared with Voyager — see synth_decode_moog_category() below).
// Fully generic, same "nothing device-specific lives here" reasoning as
// extract_prog_info()'s own comment above: looks up whichever dial the
// connected device's own <device>.txt labels "Category"
// (find_panel_dial_by_label(), panelConfig.h — matches BOTH the Z1's
// `dial category label="Category" ...` and, via synth_decode_moog_category(),
// the Voyager's differently-ID'd `dial soundCategory label="Category" ...`)
// and reads ITS dumpOffset/dumpShift/dumpMask/names — a device with no such
// dial (or a Moog-style one, which never reaches this function at all) just
// leaves outCategory untouched. Caller should default it to "" first.
void synth_decode_korg_category(const uint8_t * data, uint32_t length, char * outCategory, size_t outCategorySize) {
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
    }
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
// Raw bytes of the most recently received Panel Dump (full message, F0..F7
// inclusive) — kept purely so a single dial change can patch its own bits in
// and resend the whole thing (synth_apply_pending_dump_patches() below).
// There's no per-parameter "set this one value" SysEx for a Moog-style
// device, only a whole-dump load — confirmed against real Voyager hardware
// (2026-07-08, see tools/moog_send.swift's own use in that investigation).
static uint8_t  gLastMoogDump[256];
static uint32_t gLastMoogDumpLen           = 0;

// True while a fresh Panel Dump has been requested specifically to merge in
// one or more dials' pending dump-only changes (see
// dumpSendAwaitingFreshData's own comment, panelConfig.h) — set by
// synth_flush_pending_dump_sends() below, cleared by
// synth_apply_pending_dump_patches() once that reply arrives and is
// processed. Guards against requesting a second overlapping fetch if
// another dial's own debounce settles while the first fetch is still in
// flight — every pending dial just gets folded into whichever fetch is
// already outstanding.
static bool     gAwaitingFreshDumpForPatch = false;

bool synth_dump_patch_in_flight(void) {
    return gAwaitingFreshDumpForPatch;
}

// Patches dial's RAW dump value into the cached Panel Dump (gLastMoogDump)
// WITHOUT sending anything — used both by synth_apply_pending_dump_patches()
// below (a dump-only dial, once fresh data has arrived) and by its own
// CC-side use in synth_set_panel_dial_value() (added 2026-07-09, keeps this
// cache in sync with CC-driven changes too — see that function's own
// comment for why: without this, a CC change was invisible to gLastMoogDump,
// so a later dump-only-dial edit would patch-and-resend a stale snapshot and
// silently revert the CC change on the hardware). No-ops (returns false) if
// no dump has been received yet to patch.
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

// Applies every dial currently awaiting fresh data (dumpSendAwaitingFreshData
// — set by synth_flush_pending_dump_sends() once a dump-only dial's value
// has settled) into the dump that JUST arrived, then sends once for the
// whole batch — added 2026-07-10, owner's own idea: patching directly into
// whatever gLastMoogDump happened to hold (possibly stale — only as fresh as
// the last connect/Sync) risked silently reverting every OTHER field in that
// cached dump back to old values when a dump-only dial got edited. Fetching
// fresh data first and merging into THAT means only the field(s) the user
// actually changed differ from the hardware's own current truth. Called from
// handle_moog_panel_dump() right after extract_moog_panel_info() — which
// itself skips re-decoding any dial in this state, so the user's own chosen
// display value is never overwritten by the fresh (pre-change) reply.
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
        // Same addressing as extract_moog_name()'s own decode (synthComms.c
        // above), run in reverse: each character is 8 bits in the same
        // continuous 7-bit-per-byte bitstream write_bitpacked_field() uses
        // for every other dump field, starting at panelNameOffset/
        // panelNameBitOffset and advancing 8 bits per character (NOT 7 —
        // see extract_moog_name()'s own comment on why a char's 8 bits
        // straddle byte boundaries in this bitstream).
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
    // AFTER extract_moog_panel_info(), not before (as this used to be) — so
    // gDevice.progName is already decoded by the time synth_backup_capture_dump()
    // builds a default filename from it, same "decode the name first" order
    // handle_moog_single_preset_dump() below already uses for the same
    // reason. No-op unless a live-panel Backup is pending — see synthBackup.c.
    synth_backup_capture_dump(data, length, eBackupExpectLive);
    synth_apply_pending_dump_patches();
}

// Format: F0 <mfrId> <productId> <deviceId> 03 <payload...> F7 — the reply to
// synth_request_single_preset_dump()'s mode 0x06 request. Only decodes the
// name (extract_moog_name() above, at presetNameOffset rather than
// panelNameOffset — see the tPanelConfig field comment for why they
// differ) — Backup > Patch by Number is this reply's only other consumer
// (synth_backup_capture_dump() below), and that treats the dial data as an
// opaque blob rather than decoding it into gDevice/the dials, so there's no
// dumpOffset table to feed it through the way handle_moog_panel_dump() does.
static void handle_moog_single_preset_dump(const uint8_t * data, uint32_t length) {
    const uint32_t  skip       = 1; // F0 only — see handle_moog_panel_dump()'s comment on why

    if (length < skip + 1) {
        LOG_ERROR("Moog single preset dump too short (%u)\n", (unsigned)length);
        return;
    }
    const uint8_t * payload    = data + skip;
    uint32_t        payloadLen = length - skip - 1; // exclude trailing F7
    tPanelConfig *  cfg        = synth_panel_config();
    char            name[sizeof(gDevice.progName)];

    name[0] = '\0';
    synth_decode_moog_name(payload, payloadLen, cfg->presetNameOffset, cfg->presetNameBitOffset, cfg->presetNameLen, cfg->nameLineWidth, name, sizeof(name));

    // Only reflect this onto the live on-screen display OUTSIDE name-sweep
    // mode. Bank-to-folder EXPORT mode still needs it — backup_batch_write_
    // capture()'s own per-file naming reads gDevice.progName right after
    // this handler runs — and so does a genuine standalone Backup > Patch
    // by Number fetch. But the Load/Store Patch from/to Bank name sweep
    // must NOT: it already decodes into its own separate cache
    // (name_cache_update_from_preset_dump(), synthBackup.c) without ever
    // touching gDevice.progName, and letting this write through too
    // flickered the live displayed program name through every OTHER
    // preset's name for the whole ~1.3-2.5 minute sweep (2026-07-14 owner
    // report: "Voyager is displaying names as they come in, not the panel
    // name") — masked before today by the sweep running fast behind a
    // full-screen blocking modal that hid the panel name display entirely;
    // today's slower, sometimes-background sweep exposed it. Same "don't
    // disturb the live display" reasoning handle_prog_dump() already
    // follows for Korg, just narrower here since export mode genuinely
    // needs the old behaviour. synth_backup_export_progress_is_name_sweep()
    // (synthBackup.h) is exactly "Korg sweep active, OR Moog batch active
    // AND specifically in name-sweep mode" — false during export mode.
    if (!synth_backup_export_progress_is_name_sweep()) {
        strncpy(gDevice.progName, name, sizeof(gDevice.progName) - 1);
        gDevice.progName[sizeof(gDevice.progName) - 1] = '\0';
    }

    // Keeps the Load/Store Patch to Bank name cache (synthBackup.c) accurate
    // for this ONE slot regardless of why this reply arrived — Backup >
    // Patch by Number, the name sweep itself, or anything else that ever
    // requests a Single Preset Dump. 2026-07-11 owner observation: "the gap
    // will be closed if we have to read the patch in question from the
    // synth for any reason." The preset number is the same header byte
    // restore_patch_file_chosen()/synth_load_patch_from_bank() already use
    // (data[5], 0-based on the wire) — guarded by the length check above,
    // which already requires at least 2 bytes; a genuinely truncated reply
    // shorter than 6 bytes couldn't have decoded a real name above either,
    // so this only ever fires with a real preset number in hand. Passes the
    // freshly-decoded `name`, NOT gDevice.progName — during a name sweep the
    // latter is deliberately left untouched now (see above), so it would be
    // stale here, not this reply's own preset name.
    if (length > 5) {
        synth_backup_note_preset_name((uint32_t)data[5] + 1, name);
    }
    synth_backup_capture_dump(data, length, eBackupExpectPreset); // no-op unless a by-number Backup is pending — see synthBackup.c; after the name decode so a by-number backup's default filename can use it
}

// Format: F0 <mfrId> <productId> <deviceId> 01 <every preset's data...> F7 —
// the reply to synth_request_all_presets_dump()'s mode 0x04 request. Backup
// > Bank is this reply's only consumer, and — same as Backup > Patch by
// Number above — treats the whole thing as an opaque blob to save as-is
// rather than decoding it (there's no per-preset dumpOffset table to decode
// 128 presets' worth of dials into even if it wanted to; gDevice only ever
// holds ONE preset's worth of dial state at a time). No name decode either:
// unlike a Single Preset Dump, there's no one name to show — it's the whole
// bank.
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
            // dumpNativeMax falls back to nativeMax exactly like extract_
            // prog_info()'s own identical fallback (synthComms.c, above) —
            // an incoming Parameter Change message carries a value in the
            // parameter's own native units (e.g. Filter Cutoff: 0-99), the
            // SAME units the full program dump uses, not necessarily the
            // dial's own CC-side native range if those two differ (f1cut's
            // own case: CC wire is 0-127, param/dump native is 0-99). Fixed
            // 2026-07-13 alongside discovering the Z1's own hardware sends
            // a Parameter Change immediately after every CC when a knob
            // turns — without this fallback, an incoming param=263 value
            // of 61 was being stored as display=61 directly (dial->
            // nativeMax was 0 once f1cut stopped using it for CC-write
            // scaling), instead of the correctly scaled ~78 matching what
            // the CC and dump paths both already show for that same
            // native value.
            apply_dial_wire_value(dial, value, (dial->dumpNativeMax != 0) ? dial->dumpNativeMax : dial->nativeMax);
            LOG_DEBUG("Param %u (%s) = %u\n", (unsigned)paramId, dial->label, (unsigned)value);
        }
    }
}

// ── Public API ────────────────────────────────────────────────────────────────

void synth_on_connected(void) {
    LOG_DEBUG("Synth connected (channel byte 0x%02X)\n", SYNTH_SYSEX_CHANNEL_BYTE(gDevice.id));
    memset(gDevice.progName, 0, sizeof(gDevice.progName));
    gDevice.currentProgram = -1; // unknown until an actual Program Change is seen — see the tSynthDevice field comment in types.h

    // Reset every dial, in every section, to its own display-space default
    // (0) — apply_dial_wire_value() already knows how to turn that into the
    // right storage/native representation per dial, so no per-field defaults
    // here; there's nothing device-specific left to reset in gDevice itself.
    // Guards against stale values bleeding through from a previous device
    // (switching devices via the startup chooser reuses the same in-memory
    // dial structs) — real dials on a fresh launch are already zeroed by C's
    // own static initialization, so this is a no-op there.
    tPanelConfig * cfg = synth_panel_config();

    for (uint32_t s = 0; s < cfg->sectionCount; s++) {
        tPanelSection * section = &cfg->sections[s];

        for (uint32_t d = 0; d < section->dialCount; d++) {
            apply_dial_wire_value(&section->dials[d], 0, section->dials[d].nativeMax);
        }
    }

    // No gReDraw=true here (2026-07-08 fix) — this reset used to force an
    // immediate render before the state dump request below even went out,
    // producing a visible flash to 0 on every connect before the real values
    // arrived a moment later. The dump reply's own handler already redraws
    // once real data lands (extract_moog_panel_info()/handle_moog_message()),
    // so deferring to that is enough — nothing was actually relying on this
    // reset being visible.
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
    // Same construction as synth_request_single_preset_dump() above — built
    // from stateRequestSysEx's header, mode byte swapped to 0x04 — but with
    // no extra data byte before the trailing F7: unlike mode 0x06 (Single
    // Preset Dump REQUEST), the header comment's mode table doesn't list one
    // for 0x04, and there'd be nothing to number anyway (this asks for every
    // preset in the bank, not one).
    uint8_t        msg[sizeof(cfg->stateRequestSysEx)];
    uint32_t       prefixLen = cfg->stateRequestSysExLen - 1; // everything up to (not including) the trailing F7

    memcpy(msg, cfg->stateRequestSysEx, prefixLen);
    msg[prefixLen - 1] = 0x04; // mode byte: All Presets Dump REQUEST
    msg[prefixLen]     = MIDI_SYSEX_END;
    midi_send(msg, prefixLen + 1);
    LOG_DEBUG("Sent All Presets Dump Request\n");
}

// Shared tail for both synth_navigate_preset() and
// synth_load_patch_from_bank() below — the actual "make this program the
// live one" mechanism (send Program Change, optimistically record it,
// debounce a state-dump refresh) is identical either way; only how the
// destination program number is computed differs (relative delta vs. an
// absolute 1-based preset number). program is 0-based (0-127), matching
// midi_send_program_change()'s own wire convention.
static void synth_change_program(uint8_t program) {
    midi_send_program_change(gDevice.id, program);
    gDevice.currentProgram = program; // optimistic — see the tSynthDevice field comment in types.h
    LOG_DEBUG("Preset navigation: sent Program Change %d\n", (int)program);
    // Debounced, not synth_request_state_dump() directly — a rapid burst of
    // clicks (real hardware capture, 2026-07-07: Program Change 13 then 14
    // sent less than a MIDI thread tick apart) only ever got ONE Panel Dump
    // reply back, for whichever program the Voyager had settled on by the
    // time it got around to answering. See midi_arm_state_dump_debounce()'s
    // comment (midiComms.h) for the full story.
    midi_arm_state_dump_debounce();
}

void synth_navigate_preset(int32_t delta) {
    if (!gDevice.connected) {
        LOG_ERROR("Preset navigation: no device connected\n");
        return;
    }
    // A "default to slot 0 when unknown" fallback lived here 2026-07-11 to
    // 2026-07-13 — removed once synth_hit_test_patch_nav() (synthGraphics.cpp)
    // started disabling Prev/Next at the hit-test level (not just cosmetic
    // greying) whenever gDevice.currentProgram is unknown, making this
    // function uncallable in that state via the button at all. The fallback
    // itself was the real bug behind the owner's 2026-07-13 report ("Prev/
    // Next always starts at the first patch") — since Load Patch from Bank
    // wasn't working either (a separate, still-open issue), currentProgram
    // routinely stayed unknown for an entire session, so EVERY press
    // guessed from slot 0 instead of stepping relative to whatever was
    // actually loaded. Genuinely unreachable with currentProgram<0 now (the
    // button can't be clicked), so no clamp-to-0 fallback is needed here —
    // if some OTHER future caller reaches this with currentProgram still
    // negative, that's a bug at the CALL SITE worth surfacing, not
    // something to silently paper over here again.
    int32_t next = gDevice.currentProgram + delta;

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

    // bank is Korg-style only — see this function's own comment in
    // synthComms.h. A Moog-style device (Voyager) has no bank concept the
    // app knows how to select, so bank is simply ignored there; every
    // existing Moog-only caller already passes bank=0.
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
    // Standard MIDI Bank Select — CC0 (MSB) always 0, CC32 (LSB) = bank
    // (0=A, 1=B). The Z1 only ever needs the LSB half; sending an explicit
    // MSB=0 first matches the standard two-CC convention rather than
    // relying on the device defaulting it.
    midi_send_cc(gDevice.id, 0, 0);
    midi_send_cc(gDevice.id, 32, bank);
    synth_change_program((uint8_t)(progNumber - 1));
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

    if ((progNumber < 1) || (progNumber > 128)) {
        LOG_ERROR("Program Write Request: program number %u out of range (1-128)\n", (unsigned)progNumber);
        return;
    }
    // F0 <mfrId> 3g 46 11 0b pp F7 — 0b: Destination Program Bank(0:A,1:B),
    // pp: 0-based destination program number, per the Z1 MIDI
    // Implementation doc's own PROGRAM WRITE REQUEST table. No payload at
    // all — the device commits whatever's currently in its OWN live edit
    // buffer, unlike Voyager's fetch+relabel+resend mechanism (see this
    // function's own comment in synthComms.h).
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
    uint32_t pos = build_header(msg, SYNTH_FUNC_PARAMETER_CHANGE);

    msg[pos++] = (uint8_t)(group & 0x0F);
    msg[pos++] = (uint8_t)(paramId & 0x7F);
    msg[pos++] = (uint8_t)((paramId >> 7) & 0x7F);
    msg[pos++] = (uint8_t)(value & 0x7F);
    msg[pos++] = (uint8_t)((value >> 7) & 0x7F);
    msg[pos++] = MIDI_SYSEX_END;
    midi_send(msg, pos);
}

// Real detented switch bounce (confirmed 2026-07-08, Voyager's LFO Sync)
// stays within this window between transitional messages, while a genuinely
// separate switch flip is always seconds apart — see hasPendingCc's own
// comment in panelConfig.h.
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
        // A live CC message carries a raw 0-127 byte. For a plain continuous
        // dial (nativeMax == 0) that byte already IS the storage/display
        // value, so write it straight through — no debounce, this stream is
        // a genuine real-time sweep, not switch bounce. But some switches/
        // selectors are wired as a CC whose hardware only ever sends a
        // handful of evenly-spaced raw values across that range for their N
        // positions (nativeMax != 0, display == dialDisplayNames — e.g.
        // Voyager's Glide switch: CC65, 0-63=Off, 64-127=On) — those need
        // the same native->display quantization apply_dial_wire_value()
        // already does for SysEx-sourced raw values, or every position past
        // the first would render as "?" (index >= nameCount). They ALSO get
        // debounced (hold the raw byte, defer applying it — see
        // synth_flush_pending_cc() below) since a real detented switch's own
        // mechanical bounce sends several transitional bytes within tens of
        // milliseconds before settling.
        if ((dial->nativeMax != 0) && (dial->display == dialDisplayNames)) {
            dial->hasPendingCc    = true;
            dial->pendingRawValue = value;
            dial->pendingSinceMs  = monotonic_ms();
        } else {
            apply_dial_wire_value(dial, value, dial->nativeMax);
        }
    } else if (cc == dial->ccNumber) {
        // 14-bit CC pair (MIDI's own coarse/fine convention: controller N is
        // the MSB, N+32 the LSB — see the ccLsbNumber comment in
        // panelConfig.h). Each half arrives as its own separate CC message —
        // confirmed against real hardware (2026-07-08, timestamped
        // dispatch_cc() output turning Cutoff): MSB consistently arrives
        // ~0.5-0.6ms before its matching LSB, every single step. Recombining
        // dial->value on EVERY message (the previous behaviour) meant the
        // MSB's arrival paired a brand-new MSB with the OLD LSB for that
        // ~0.5ms window — a real torn/wrong value, not just a theoretical
        // risk, briefly shown and redrawn before the LSB corrected it a
        // moment later. Only latch the MSB here; the LSB branch below is
        // what actually recomputes dial->value, so a torn combination is
        // never computed at all, not just never (usually) seen.
        dial->ccMsbLatched = value;
    } else {
        dial->ccLsbLatched = value;
        dial->value        = ((uint32_t)dial->ccMsbLatched << 7) | dial->ccLsbLatched;
    }
    // Re-arms the existing state-dump-request debounce (midiComms.c, already
    // used for Program Change/preset navigation) on every genuinely
    // hardware-originated CC — added 2026-07-09, owner's own idea: a
    // physical knob's true position can differ slightly from what its CC
    // message conveys (e.g. Cutoff measured 12928 via CC vs 12936 via a
    // Panel Dump for the same real turn — two independent quantizations of
    // one continuous pot, not a decode bug), so requesting a fresh Panel
    // Dump ~264ms after the LAST CC in a turn
    // (SYNTH_STATE_DUMP_DEBOUNCE_TICKS, coalesces a whole turn into one
    // request the same way it already coalesces a burst of Program Changes)
    // lets the display settle on the dump's own more-precise reading shortly
    // after the user's hand leaves the knob, without spamming a request per
    // CC byte while actively turning. Only reachable here for INCOMING CC
    // (this function is dispatch_cc()'s own handler) — a GUI-driven change
    // goes out via midi_send_cc() on a separate path that never calls this,
    // so turning an on-screen dial doesn't also trigger a redundant refresh.
    midi_arm_state_dump_debounce();
    return true;
}

// Commits any dial's debounced CC (see hasPendingCc's own comment in
// panelConfig.h) once CC_DEBOUNCE_MS have passed since the last raw byte
// arrived for it. Called once per frame from the render loop
// (do_graphics_loop()/graphics.cpp) — cheap enough (every dial in every
// section, a handful of integer comparisons each) to not need its own timer.
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
                gReDraw            = true;
            }
        }
    }
}

// Same trailing-edge debounce as synth_flush_pending_cc() above, but for the
// OUTGOING side of a dump-only dial (see hasPendingDumpSend's own comment in
// panelConfig.h) — reuses CC_DEBOUNCE_MS's window (no reason for a different
// settle time) but is a separate flag/timestamp since it debounces a send,
// not an apply, and fires at most once per settled value rather than once
// per drag tick. Once settled, this does NOT patch-and-send directly (see
// dumpSendAwaitingFreshData's own comment) — it requests a fresh Panel Dump
// first (unless one's already in flight for this same reason) and hands off
// to synth_apply_pending_dump_patches(), which does the actual patch+send
// once that reply arrives.
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

void synth_handle_message(const uint8_t * data, uint32_t length) {
    if (synth_panel_config()->moogStyleDump) {
        // Entirely separate header shape and dispatch from the Korg-style
        // path below (see is_moog_sysex()/handle_moog_panel_dump()). Modes
        // 0x02 (Panel Dump), 0x03 (Single Preset Dump), and 0x01 (All
        // Presets Dump) are the replies this app requests (see
        // stateRequestSysEx/"Panel Dump Request", synth_request_single_preset_dump(),
        // and synth_request_all_presets_dump() respectively) — anything else
        // is logged and ignored.
        if (!is_moog_sysex(data, length)) {
            LOG_DEBUG("Ignoring non-target SysEx (len=%u)\n", (unsigned)length);
            return;
        }
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
            LOG_DEBUG("Synth unhandled func 0x%02X\n", (unsigned)funcId);
            break;
    }
    gReDraw = true;
}

// Holds at most ONE deferred outgoing send PER WIRE SHAPE (single CC, 14-bit
// CC pair, Korg Parameter Change) — a single slot each, not per-dial, is
// enough because only one dial can ever be under an active GUI drag at a
// time (mouseHandle.c's own gDraggedDial is a single pointer), and a given
// dial only ever uses exactly one of the three shapes. Set only when
// synth_backup_sweep_request_in_flight() (synthBackup.h) is true at send
// time — the mutual-exclusion fix for 2026-07-14's owner report ("seeing
// some items saying 'No Response', likely due to me tweaking a dial"),
// generalized the same day to cover Voyager's CC-mapped dials too (owner:
// "these should be common mechanisms with Voyager and any other device"):
// rather than sending into the same narrow window a name-sweep reply is
// expected in, hold it here and let synth_flush_pending_param_send() below
// send it the moment that window clears. NULL means "nothing pending" —
// the overwhelmingly common case (no sweep running, or its slow paced gap
// between requests, not the brief in-flight window), where
// synth_set_panel_dial_value() below still sends immediately with zero
// added latency, exactly as before this existed. Only the actual outbound
// MIDI bytes are deferred — synth_set_panel_dial_value() still updates
// dial->value/ccMsbLatched/ccLsbLatched/the Moog dump cache immediately
// either way, so the on-screen dial and internal state never lag; only the
// wire message does.
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
        synth_send_parameter_change((uint8_t)gPendingParamDial->paramGroup, (uint16_t)gPendingParamDial->paramId, (uint8_t)gPendingParamValue);
        gPendingParamDial = NULL;
    }
}

void synth_set_panel_dial_value(tPanelDial * dial, uint32_t displayValue) {
    if (!dial) {
        return;
    }

    if ((dial->max > 0) && (displayValue >= dial->max)) {
        displayValue = dial->max - 1;
    }

    // Linked min/max constraint (see linkedMaxDialId/linkedMinDialId's own
    // comment in panelConfig.h) — clamps against the OTHER dial's CURRENT
    // value, resolved fresh here rather than cached, so it always reflects
    // whatever that dial most recently held (including a change from
    // earlier in this same user action, e.g. dragging Hi Key down past Lo
    // Key first). Applied before storageOffset/dedup below so a clamped
    // value that happens to equal what's already stored correctly takes
    // the early-return path just like any other unchanged value.
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

    // Computed regardless of ccNumber now — a dump-only dial (no CC at all,
    // e.g. Voyager's Filter A/B Pole Select) still needs its nativeValue to
    // patch into a resent dump below, not just a CC-bound one. Previously
    // this only ran inside the ccNumber!=0 branch, which was fine when every
    // ccNumber==0 dial was Z1's own paramGroup/paramId path (no native
    // scaling concept there) — no longer true once a Moog dump-only selector
    // exists.
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
            // nativeMax != 0: the hardware expects its own scaled raw byte
            // for this display position (e.g. Glide's Off/On sends 0/127,
            // not 0/1) — nativeValue above was just computed for exactly
            // this. Without nativeMax, storageValue already IS that byte
            // (plain continuous CC dial).
            uint8_t wireValue = (dial->nativeMax != 0) ? dial->nativeValue : (uint8_t)storageValue;

            if (sweepInFlight) {
                // Defer — see gPendingCcDial's own comment above.
                gPendingCcDial  = dial;
                gPendingCcValue = wireValue;
            } else {
                midi_send_cc(gDevice.id, (uint8_t)dial->ccNumber, wireValue);
            }
        }

        // Mirror this change into the cached Panel Dump too (added
        // 2026-07-09) — a dial can have BOTH a CC and a dump field (most of
        // this file's dials do), and without this the cache only reflected
        // whatever the LAST full Panel Dump said, going stale the moment a
        // CC-driven dial changed. A later dump-only-dial edit (e.g.
        // Headphone Volume) patches-and-resends that cache — if it were
        // stale, the resend would silently revert this CC change back to
        // its old value on the hardware. Cache-only, no MIDI send here —
        // the CC message above already told the hardware.
        if (synth_panel_config()->moogStyleDump && (dial->dumpBitWidth > 0)) {
            synth_patch_moog_dump_cache(dial, synth_encode_dump_raw_value(dial, displayValue));
        }
    } else if (synth_panel_config()->moogStyleDump && (dial->dumpBitWidth > 0)) {
        // No CC exists for this dial (e.g. Voyager's Filter A/B Pole Select)
        // — patch-and-resend the cached dump instead of falling through to
        // Z1's Korg-style parameter-change SysEx below, which would send a
        // meaningless message shaped for an entirely different protocol.
        // Debounced (see hasPendingDumpSend's own comment in panelConfig.h)
        // rather than sent immediately — a dragged continuous dial (e.g.
        // Headphone Volume) can call this many times a second, and unlike a
        // 3-byte CC each send here resends the whole cached dump.
        //
        // synth_encode_dump_raw_value() (not the old nativeValue-or-
        // storageValue computation) as of 2026-07-09 — the old computation
        // never applied dumpInvert on the way out, silently wrong for any
        // dump-only dial with dumpInvert=1 (none existed yet when it was
        // written, but the CC-side cache-sync above now shares this same
        // path for dials that DO use it, e.g. mwDestination).
        uint32_t rawValue = synth_encode_dump_raw_value(dial, displayValue);

        dial->hasPendingDumpSend  = true;
        dial->pendingDumpRawValue = rawValue;
        dial->pendingDumpSinceMs  = monotonic_ms();
    } else if (synth_backup_sweep_request_in_flight()) {
        // Defer — see gPendingParamDial's own comment above.
        gPendingParamDial  = dial;
        gPendingParamValue = storageValue;
    } else {
        synth_send_parameter_change((uint8_t)dial->paramGroup, (uint16_t)dial->paramId, (uint8_t)storageValue);
    }
    gReDraw = true;
}

uint32_t synth_effective_name_maxlen(void) {
    tPanelConfig * cfg    = synth_panel_config();
    uint32_t       maxLen = cfg->moogStyleDump ? cfg->panelNameLen : cfg->progNameLen;

    return (maxLen < (SYNTH_PROG_NAME_MAXLEN - 1)) ? maxLen : (SYNTH_PROG_NAME_MAXLEN - 1);
}

// Mirrors extract_moog_name()'s own line-wrap-for-display insertion (its own
// comment above explains why: nameLineWidth is a display-only concept, no
// real separator exists in the wire format), applied here to the flat name
// this app is about to send rather than one just decoded — so
// gDevice.progName reads correctly immediately after a commit, without
// waiting for the round trip through hardware and back. Deliberately not
// shared code with extract_moog_name(): that function also collapses
// whitespace runs and strips a hardware line-boundary quirk out of RAW
// decoded bytes, neither of which applies to a clean user-typed string.
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
            // No dump cached yet to patch into (not connected, or no Panel
            // Dump received this session) — nothing to send. Matches
            // synth_patch_moog_dump_cache()'s own no-op guard for the same
            // reason.
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
