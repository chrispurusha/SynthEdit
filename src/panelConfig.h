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

// Generic parser for the panel-descriptor text format used by xxxx.txt (and,
// eventually, other <device>.txt files). Deliberately has no application-
// specific dependencies — candidate for promotion into SynthLib once a
// second project wants it.

#ifndef __PANEL_CONFIG_H__
#define __PANEL_CONFIG_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "geometry.h"

#define PANEL_ID_LEN               16
#define PANEL_LABEL_LEN            32
#define PANEL_MAX_NAMES            20
#define PANEL_MAX_COLOURS          16
#define PANEL_MAX_DIALS            32
#define PANEL_MAX_SECTIONS         32
#define PANEL_MAX_LIST_ITEMS       32
#define PANEL_MAX_LISTS            8
#define PANEL_MAX_COLUMN_LABELS    32

typedef enum {
    dialDisplayRaw = 0,
    dialDisplayCcNative,
    dialDisplayNames,
} tDialDisplay;

typedef struct {
    char name[PANEL_ID_LEN];
    tRgb colour;
} tPanelColour;

// A named string list not tied to any dial — e.g. a device's patch-category
// or voice-mode names, shown as plain text rather than driving a control.
typedef struct {
    char     name[PANEL_ID_LEN];
    char     items[PANEL_MAX_LIST_ITEMS][PANEL_LABEL_LEN];
    uint32_t itemCount;
} tPanelList;

// A title printed above one grid column (e.g. "Osc 1", "Mixer") —
// "columnLabel <col> <text>" in the device's own .txt, one line per
// labelled column. Page-scoped, not global: col=/row= positions are only
// unique within a single page (see tPanelDial.gridCol's own comment), so
// "column 3" on one page and "column 3" on another can have entirely
// different labels, or only one of them labelled at all. Purely cosmetic —
// synth_render() (synthGraphics.cpp) reserves a header row above a grid
// page's dials only if the page has at least one of these; a page with
// none renders exactly as it did before this existed.
typedef struct {
    char    page[PANEL_ID_LEN];
    int32_t col;
    char    label[PANEL_LABEL_LEN];
} tColumnLabel;

typedef struct {
    char         id[PANEL_ID_LEN];                        // e.g. "f1cut" — looked up by find_panel_dial()
    char         label[PANEL_LABEL_LEN];                  // e.g. "F1 Cut"
    char         colourName[PANEL_ID_LEN];                // as written in the file, e.g. "f1"
    tRgb         colour;                                  // resolved against the section's colour table at parse time
    uint32_t     max;                                     // count of valid display-space positions: 0..max-1
    tDialDisplay display;
    char         names[PANEL_MAX_NAMES][PANEL_LABEL_LEN]; // populated when display == dialDisplayNames
    uint32_t     nameCount;
    double       gapBefore;                               // extra flow-space inserted before this dial
    tRectangle   rect;                                    // populated by layout_panel_section(); used for render + hit-test

    // Protocol wiring — describes how to read/write/send this control's value,
    // so generic code (mouse handling, rendering) never needs to know what a
    // given dial *means*. All parsed from the file. The dial owns its own
    // live value/nativeValue directly (see get_panel_dial_value() etc.) —
    // there is no separate application-side struct/binding step for any of
    // this, which is what makes a new device just a new <device>.txt file.
    int32_t  storageOffset;     // storage_value = display_value + storageOffset (e.g. 1-5 vs 0-4 for "type")
    uint32_t paramGroup;        // SysEx parameter group
    uint32_t paramId;           // SysEx parameter ID
    uint32_t ccNumber;          // MIDI CC number; 0 = not CC-controlled (send SysEx param change instead)
    // MIDI's own 14-bit CC convention: controller N (0-31) carries the coarse/
    // MSB half, N+32 the fine/LSB half — combined value = (msb<<7)|lsb, giving
    // 0..16383 instead of a plain CC's 0..127. 0 = ccNumber alone is a plain
    // 7-bit CC (default; existing single-cc dials are unaffected). Set via the
    // file's "ccLsb=" dial attribute alongside "cc=".
    uint32_t ccLsbNumber;
    uint8_t  ccMsbLatched;      // last raw byte seen on ccNumber; only meaningful when ccLsbNumber != 0
    uint8_t  ccLsbLatched;      // last raw byte seen on ccLsbNumber; only meaningful when ccLsbNumber != 0
    uint32_t nativeMax;         // native/SysEx value range when paired with a CC (0 = no native pairing)
    uint32_t value;             // live storage-space value (display_value + storageOffset); wide enough
                                // for a 14-bit CC pair, not just a single byte
    uint8_t  nativeValue;       // live native value, if nativeMax != 0; unused otherwise

    // Where this dial's value lives in a full program-dump byte buffer (a
    // different wire format from individual parameter-change messages, but
    // still just data the file describes). -1 = not present in a dump.
    int32_t  dumpOffset;
    uint32_t dumpShift;          // bits to shift right before masking (default 0)
    uint32_t dumpMask;           // mask applied after shifting (default 0xFF = whole byte)

    // Alternative to dumpShift/dumpMask above for a dump format where a
    // value's bits are packed continuously across MULTIPLE bytes at 7 usable
    // bits each (Moog's Panel/Preset Dump SysEx — see moogStyleDump in
    // tPanelConfig below), rather than living inside one byte. dumpBitWidth=0
    // (the default) means "not this kind of field" — use dumpShift/dumpMask
    // on the single dumpOffset byte instead, unchanged from before this
    // existed. When dumpBitWidth > 0: dumpOffset is still this field's first
    // relevant byte, and dumpBitOffset (0-6) is which of that byte's 7 usable
    // bits holds the field's LEAST significant bit — confirmed empirically
    // against real Voyager hardware (set Filter Cutoff/Resonance to known CC
    // values, requested a Panel Dump, decoded bit-for-bit) that a field's
    // bits then continue in strict ascending significance through the
    // 7-bit-per-byte stream (byte's bit 6 done -> next byte's bit 0), with
    // no padding except where a genuinely separate field's bits interleave.
    uint32_t dumpBitOffset;
    uint32_t dumpBitWidth;

    // Explicit grid position ("col="/"row=" in the file), for a device whose
    // real front panel groups controls into fixed columns rather than
    // flowing left-to-right within a section (see gridColWidth/gridRowHeight
    // below). 0-based; -1 (the default) means "not grid-positioned" — this
    // dial keeps going through the ordinary left-to-right auto-flow within
    // its section, exactly as every dial did before this existed, so a
    // device file with no col=/row= anywhere (Z1, Novation Supernova 2)
    // renders identically to before. A section can freely mix grid and
    // auto-flow dials, though in practice a device either commits one whole
    // page to the grid or doesn't use it at all. row defaults to 0 if col is
    // set but row isn't.
    //
    // double, not int — 2026-07-07: aligning a short column's controls to a
    // taller neighbour (Mod Wheel/Pedal's 4 controls against Mixer's 5-tall
    // Level/On columns) needs its middle rows evenly spread across a
    // non-integer gap, not just whole grid steps. "row=2.67" places a dial
    // two-thirds of the way between grid rows 2 and 3.
    double gridCol;
    double gridRow;
} tPanelDial;

typedef struct {
    char   page[PANEL_ID_LEN];
    char   section[PANEL_ID_LEN];
    double dialSize;
    double spacing;
    bool   hidden;               // true: not a rendered control/page-tab target, just named
                                 // device state (e.g. program category, voice mode, unison) —
                                 // shown as plain "label: value" text instead of a dial widget.
                                 // Still parsed/bound/dump-scanned exactly like any other section.
    tPanelColour colours[PANEL_MAX_COLOURS];
    uint32_t     colourCount;
    tPanelDial   dials[PANEL_MAX_DIALS];
    uint32_t     dialCount;
} tPanelSection;

typedef struct {
    char deviceName[PANEL_LABEL_LEN];
    char description[128]; // one-line summary from the file's "description"
                           // directive — shown in the startup device chooser
                           // (see scan_panel_configs()); empty if the file
                           // doesn't declare one.
    // 1 byte for a classic manufacturer ID (e.g. Korg 0x42), or 3 bytes for an
    // "extended" ID (e.g. likely Novation — companies registered after
    // single-byte IDs ran out; MIDI spec signals this with a leading 0x00).
    // manufacturerIdLen is always 1 or 3, set by how many values the file's
    // "manufacturerId" line gives.
    uint8_t  manufacturerId[3];
    uint32_t manufacturerIdLen;
    uint32_t familyId;
    uint32_t memberId;
    uint32_t progNameLen;                     // count of leading dump bytes that are program-name ASCII chars
                                              // (also param IDs 1..progNameLen on the live parameter-change path)
    char     scrollDialId[PANEL_ID_LEN];      // dial id (found in any section) that plain mouse-wheel
    // scroll nudges when nothing is being dragged; empty = no shortcut

    // Some hardware (e.g. Moog's Minitaur/Voyager) never answers a Universal
    // Device Inquiry at all — confirmed by capturing the vendor's own editor,
    // which talks proprietary SysEx from the first message with no identity
    // handshake step. For such a device, set "identityQuery no" in the file:
    // this skips the inquiry/wait/reply cycle entirely rather than polling
    // and timing out. Defaults to true (load_panel_config() sets it right
    // after the zeroing memset), so existing files that never mention it are
    // unaffected. "midiChannel" (1-indexed, as written by a human) is then
    // the only way to know which channel to talk on, since there's no
    // identity reply to read gDevice.id from; ignored when identityQuery is
    // true, where the reply's own channel byte is authoritative instead.
    bool     supportsIdentity;
    uint32_t midiChannel;                     // 1-indexed; only meaningful when !supportsIdentity
    // With no identity reply, there's also nothing to correlate the right MIDI
    // destination from — the port a device without identity support sits on
    // (e.g. "Elektron TM-1") isn't necessarily whatever CoreMIDI happens to
    // enumerate first (e.g. an unrelated "IAC Driver Bus 1"). "midiPort <name
    // substring>" names it explicitly; connect_without_identity() in
    // midiComms.c matches it case-insensitively against each destination's
    // display name. Empty (the default) falls back to the first destination
    // found, same as before this existed.
    char midiPortName[64];                    // only meaningful when !supportsIdentity

    // Optional SysEx sent right after connecting (see connect_without_identity()
    // in midiComms.c) — asks the device to report its own current state instead
    // of leaving every dial showing a stale/default value until physically
    // touched. Device-specific (e.g. Moog's own "dump current CC values"
    // command); set via the file's "stateRequestSysEx <hex> <hex> ..." line.
    // 0 length (the default) means don't send anything, unchanged from before
    // this existed.
    uint8_t  stateRequestSysEx[32];
    uint32_t stateRequestSysExLen;

    // Moog's own dump SysEx has a completely different header shape from the
    // Korg-style one is_synth_sysex()/synth_handle_message() assume by
    // default (F0 <mfrId> <0x30|channel> <familyId> <func> ...): it's
    // F0 <mfrId> <productId> <deviceId> <mode> ... instead — see
    // "Voyager System Exclusive Panel Dump Format" (lintronics.de). Set via
    // "dumpFormat moog" + "productId <hex>" in the file; false/0 (the
    // default) leaves every existing Korg-style device (Z1) unaffected.
    // productId is Moog's own proprietary header byte (e.g. 0x01 Voyager,
    // 0x08 Minitaur) — distinct from familyId/memberId (the Universal
    // Identity Reply scheme), which is moot anyway for a device with
    // identityQuery no.
    bool    moogStyleDump;
    uint8_t productId;

    // Where a name field lives in each of the two Moog-style dump replies
    // (moogStyleDump devices only — see extract_moog_name() in
    // synthComms.c). Same continuous 7-bit-per-byte bitstream
    // dumpOffset/dumpBitOffset/dumpBitWidth already use for the numeric
    // panel fields — the Offset/BitOffset pair below is just that scheme's
    // byte/bit position for the name's first (of Len) 8-bit characters, not
    // a separate encoding. Two separate fields, not one shared offset,
    // because the two dump types' name fields don't live at the same
    // position — Single Preset Dump's is 1 byte later than Panel Dump's,
    // presumably for a preset-number byte Panel Dump has no reason to carry.
    // Reverse-engineered from real Voyager captures (2026-07-07): Panel Dump
    // from whatever the currently-loaded patch was ("FROM A DISTANCE"),
    // Single Preset Dump from preset 1 ("FILTER BUBBLES") — not from any
    // published spec. Both offsets default to -1 ("no name field declared"),
    // set in load_panel_config(), same convention as each dial's own
    // dumpOffset default.
    int32_t  panelNameOffset;
    uint32_t panelNameBitOffset;
    uint32_t panelNameLen;
    int32_t  presetNameOffset;
    uint32_t presetNameBitOffset;
    uint32_t presetNameLen;

    // How many characters of the name field above make up one line of the
    // device's own display, if it has a multi-line one (0 = no forced break
    // — the whole field is one line, the default for every device that
    // doesn't set this). extract_moog_name() (synthComms.c) inserts a '\n'
    // in gDevice.progName every nameLineWidth characters so the on-screen
    // "Program name" row (synth_render() in synthGraphics.cpp) can show the
    // same line breaks the real hardware does, rather than running both
    // lines together. Needed because the line boundary itself carries no
    // reliable marker in the raw data — see presetNameLen's comment in
    // voyager.txt for why a byte-value-based guess (e.g. "insert a break
    // wherever there's already whitespace") isn't enough: "Floating Mod" /
    // "Steel Guitar" fills both 12-char lines exactly, with no whitespace at
    // the boundary at all.
    uint32_t nameLineWidth;

    // How many banks of presets the connected unit has — 1 for a base
    // Voyager, more for one with a memory expansion (VX-352 or similar; per
    // the Voyager manual, not confirmed against real hardware since nobody
    // testing this app owns an expanded unit). Documentation only right
    // now, not wired into anything: Backup > Bank
    // (synth_request_all_presets_dump() in synthComms.c, mode 0x04) has no
    // known way to select a non-default bank — the "Voyager System
    // Exclusive Panel Dump Format" doc this app's Moog SysEx handling is
    // otherwise built from doesn't cover multi-bank addressing, and mode
    // 0x04 itself is unconfirmed even for the single-bank case. Once a real
    // Bank backup file's size/structure can be inspected (see
    // synth_backup_bank() in synthBackup.h), there may be a DI-no variant, a
    // bank-select byte, or a second SysEx mode this field ends up feeding —
    // deliberately not guessed at here.
    uint32_t presetBankCount;

    // Pitch (in px) between adjacent grid cells for any dial using col=/row=
    // (see tPanelDial.gridCol above) — "gridColWidth"/"gridRowHeight" in the
    // file. 0 (the default) means "not configured"; synth_render()
    // (synthGraphics.cpp) falls a grid dial back to ordinary auto-flow
    // rather than stacking every grid dial at the same spot if a device
    // declares col=/row= but forgets to set these. One pitch for the whole
    // device, not per-page or per-section — every grid-using page is
    // expected to want the same column/row rhythm.
    double        gridColWidth;
    double        gridRowHeight;
    tPanelSection sections[PANEL_MAX_SECTIONS];
    uint32_t      sectionCount;
    tPanelList    lists[PANEL_MAX_LISTS];
    uint32_t      listCount;
    tColumnLabel  columnLabels[PANEL_MAX_COLUMN_LABELS];
    uint32_t      columnLabelCount;
} tPanelConfig;

// Parses the file at `path` into `config` (which is zeroed first). Malformed
// lines are logged via LOG_ERROR and skipped rather than aborting the parse.
// Returns false only if the file couldn't be opened.
bool load_panel_config(const char * path, tPanelConfig * config);

// Computes each dial's `rect` in `section`. A dial with gridCol >= 0 (and
// both grid pitches > 0) is placed directly at
// origin + (gridCol*gridColWidth, gridRow*gridRowHeight) — every grid dial
// on a page shares the same `origin`, which is what turns per-dial col/row
// into a single page-wide grid rather than one grid per section. Every
// other dial flows left to right from `origin` using the section's
// dialSize/spacing and its own gapBefore, exactly as before col=/row=
// existed.
void layout_panel_section(tPanelSection * section, tRectangle origin, double gridColWidth, double gridRowHeight);

// True for a dial whose names= is exactly {"Off","On"} — a genuine on/off
// dial, as opposed to any other 2-position selector (Filters' Mode "Dual
// LP"/"HP/LP", Osc 3's Freq Range "Lo"/"Hi", ...). Shared by
// synthGraphics.cpp (renders these as a power button, not a knob — see
// draw_power_button() in SynthLib) and mouseHandle.c (a single click
// toggles one of these outright, rather than needing the drag gesture
// every other dial uses) so the two stay in lockstep — a dial that LOOKS
// like a button should also BEHAVE like one.
bool panel_dial_is_toggle(const tPanelDial * dial);

// True for ANY 2-position named dial — panel_dial_is_toggle()'s Off/On
// case included, plus 2-way selectors that aren't semantically on/off
// (Filters' Mode "Dual LP"/"HP/LP", Osc 3's Freq Range "Lo"/"Hi", Env Gate
// "Keyb"/"On/Ext"). All of these are still a single click-to-flip button
// (mouseHandle.c) — only the on/off ones get the green/grey highlight;
// the rest render as a plain button showing the current state's name (same
// idea as G2-Edit's keyboard-tracking "KB" button), since colouring, say,
// Filters' Mode green for "HP/LP" would imply an on/off meaning it doesn't
// have.
bool panel_dial_is_binary(const tPanelDial * dial);

tPanelSection * find_panel_section(tPanelConfig * config, const char * page, const char * section);
tPanelDial * find_panel_dial(tPanelSection * section, const char * id);

// Same as find_panel_dial(), but searches every section in the config rather
// than one already-known section — for generic code (an info-text row, a
// scroll-shortcut) that only has a dial id and no reason to know which
// section it lives in.
tPanelDial * find_panel_dial_anywhere(tPanelConfig * config, const char * id);

// Looks up the dial wired to a given SysEx parameter group/ID (see the
// "group="/"param=" file attributes), or NULL if none matches.
tPanelDial * find_panel_dial_by_param(tPanelSection * section, uint32_t group, uint32_t paramId);

// Looks up the dial wired to a given MIDI CC number (see the "cc=" file
// attribute) across every section in the config, or NULL if none matches —
// for real-time CC dispatch, which (unlike a SysEx parameter change) carries
// no section/group context to narrow the search.
tPanelDial * find_panel_dial_by_cc(tPanelConfig * config, uint8_t cc);

#define PANEL_MAX_CANDIDATES    16

// One <device>.txt found by scan_panel_configs() — just enough to present a
// choice, not a full parsed config (that only happens for whichever one gets
// picked).
typedef struct {
    char filename[64];         // e.g. "z1.txt" — relative to the scanned dir
    char deviceName[PANEL_LABEL_LEN];
    char description[128];
} tPanelConfigCandidate;

// Scans `dir` for every "*.txt" file, fully parsing each (they're small — no
// separate lightweight-header-only parser) into a scratch config just to
// pull out its device/description, and returns how many were found (capped
// at maxCandidates). Used to build a startup device chooser when more than
// one config is present; a single match needs no chooser at all.
uint32_t scan_panel_configs(const char * dir, tPanelConfigCandidate * outCandidates, uint32_t maxCandidates);

// Returns the index into section->dials[] under `point`, or -1 if none.
// Call after layout_panel_section() has populated the dials' rects.
int32_t hit_test_panel_section(tPanelSection * section, tCoord point);

// Display-space value (0..max-1) — pure arithmetic on the dial's own stored
// value, no protocol knowledge.
uint32_t get_panel_dial_value(const tPanelDial * dial);
uint32_t get_panel_dial_native_value(const tPanelDial * dial);

// Looks up item `index` in the named list `listName` (device-wide, not
// section-scoped). Returns "?" if the list or index doesn't exist, rather
// than requiring every call site to bounds-check.
const char * get_panel_list_item(const tPanelConfig * config, const char * listName, uint32_t index);

// Number of items in the named list, or 0 if it doesn't exist — e.g. for
// validating/clamping a value parsed off the wire against the list it names.
uint32_t get_panel_list_count(const tPanelConfig * config, const char * listName);

#ifdef __cplusplus
}
#endif

#endif // __PANEL_CONFIG_H__
