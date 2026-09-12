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
// Notes: Docs/code-notes/panelConfig.h.md - "// notes §k" refers there.

// notes §1

#ifndef __PANEL_CONFIG_H__
#define __PANEL_CONFIG_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "geometry.h"

#define PANEL_ID_LEN               24 // raised from 16 2026-07-13 — discovered via the backdoor DUMP command (graphics.cpp): 19 real Z1 Amp/EG/LFO dial ids (e.g. "ampegnodetimemodsrc", 19 chars) silently truncated to 15+NUL on load (parse_dial_line()'s strncpy() in panelConfig.c has no length check/warning), losing up to 4 trailing characters with no error at all. No two truncated ids happened to collide this time (checked: `grep dial layouts/z1.txt | awk '{print substr($2,1,15)}' | sort | uniq -d` was empty) — pure luck, not a guarantee for any future id added to any device file. 24 gives real headroom over the current worst case (19) rather than tuning to the exact number again.
#define PANEL_LABEL_LEN            32
#define PANEL_MAX_NAMES            65 // raised from 20 to 32 2026-07-10 (Voyager's soundCategory, a full 32-value enum, 0-31); raised from 32 to 48 2026-07-11 — Voyager's pgmShaping1Src/pgmShaping2Src are 43-value enums (0-42), values >= the old 32 cap silently had no stored name and rendered as "?"; raised from 48 to 65 2026-07-13 — Voyager's tsGateCtrl uses storageOffset to give TS Gate's MIDI Ctrl No (64-127 plus a distinct Off state) a proper named 65-value enum instead of a raw 0-128 dial with an unlabeled dead zone below 64
#define PANEL_MAX_COLOURS          16
#define PANEL_MAX_DIALS            32
#define PANEL_MAX_SECTIONS         64 // raised 32->48 2026-07-13 (Amp/EG pages split narrow enough to hit the old ceiling at 34 sections — see panelConfig.c's own "too many sections" error for the actual failure mode, dials silently landing in the wrong section rather than a page just going missing); 48->64 same day adding LFO1-4 pushed close to 48 again — Z1's own remaining unbuilt pages (Effects, OSC-type sub-pages) will need more still, so raised with real headroom this time rather than tuning to the exact count again
#define PANEL_MAX_LIST_ITEMS       32
#define PANEL_MAX_LISTS            8
#define PANEL_MAX_COLUMN_LABELS    32

typedef enum {
    dialDisplayRaw = 0,
    dialDisplayCcNative,
    dialDisplayNames,
    // notes §2
    dialDisplaySignedHiLo,

    // notes §3
    dialDisplayNote,

    // notes §4
    dialDisplaySigned,
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

// notes §5
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

    // notes §6
    char linkedMaxDialId[PANEL_ID_LEN];
    char linkedMinDialId[PANEL_ID_LEN];

    // notes §7
    char     disabledUnlessDialId[PANEL_ID_LEN];
    uint32_t disabledUnlessValue;

    // notes §8
    int32_t  storageOffset;     // storage_value = display_value + storageOffset (e.g. 1-5 vs 0-4 for "type")
    int32_t  displayOffset;     // dialDisplaySigned only — shown_value = display_value - displayOffset; see that enum value's own comment above. Unrelated to storageOffset: never touches the wire.
    uint32_t paramGroup;        // SysEx parameter group
    uint32_t paramId;           // SysEx parameter ID

    // notes §9
    bool wireSigned;

    // notes §10
    bool     hasKronosParam;
    uint32_t kronosTyp;
    uint32_t kronosSoc;
    uint32_t kronosSub;
    uint32_t kronosPid;
    uint32_t kronosIdx;
    uint32_t ccNumber;          // MIDI CC number; 0 = not CC-controlled (send SysEx param change instead)
    // notes §11
    uint32_t ccLsbNumber;
    uint8_t  ccMsbLatched;      // last raw byte seen on ccNumber; only meaningful when ccLsbNumber != 0
    uint8_t  ccLsbLatched;      // last raw byte seen on ccLsbNumber; only meaningful when ccLsbNumber != 0
    uint32_t nativeMax;         // native/SysEx value range when paired with a CC (0 = no native pairing)
    uint32_t value;             // live storage-space value (display_value + storageOffset); wide enough
                                // for a 14-bit CC pair, not just a single byte
    uint8_t  nativeValue;       // live native value, if nativeMax != 0; unused otherwise

    // notes §12
    bool     hasPendingCc;
    uint32_t pendingRawValue;
    double   pendingSinceMs;

    // notes §13
    bool     hasPendingDumpSend;
    uint32_t pendingDumpRawValue;
    double   pendingDumpSinceMs;

    // notes §14
    bool dumpSendAwaitingFreshData;

    // Where this dial's value lives in a full program-dump byte buffer (a
    // different wire format from individual parameter-change messages, but
    // still just data the file describes). -1 = not present in a dump.
    int32_t  dumpOffset;
    uint32_t dumpShift;          // bits to shift right before masking (default 0)
    uint32_t dumpMask;           // mask applied after shifting (default 0xFF = whole byte)

    // notes §15
    uint32_t dumpBitOffset;
    uint32_t dumpBitWidth;

    // notes §16
    int32_t  dumpOffset2;
    uint32_t dumpBitOffset2;
    uint32_t dumpBitWidth2;

    // notes §17
    uint32_t dumpNativeMax;
    bool     dumpInvert;

    // notes §18
    double gridCol;
    double gridRow;

    // notes §19
    bool noLabel;

    // notes §20
    bool readOnly;

    // notes §21
    bool asDial;

    // notes §22
    bool asMenu;

    // notes §23
    int32_t  hiLoOffset;
    uint32_t hiLoCoarseScale;
    uint32_t hiLoFineScale;
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
                           // notes §24
    uint8_t  manufacturerId[3];
    uint32_t manufacturerIdLen;
    uint32_t familyId;
    uint32_t memberId;
    uint32_t progNameLen;                     // count of leading dump bytes that are program-name ASCII chars
                                              // (also param IDs 1..progNameLen on the live parameter-change path)
    char     scrollDialId[PANEL_ID_LEN];      // dial id (found in any section) that plain mouse-wheel
    // scroll nudges when nothing is being dragged; empty = no shortcut

    // notes §25
    bool     supportsIdentity;
    uint32_t midiChannel;                     // 1-indexed; only meaningful when !supportsIdentity
    // notes §26
    char midiPortName[64];

    // notes §27
    uint8_t  stateRequestSysEx[32];
    uint32_t stateRequestSysExLen;

    // notes §28
    bool    moogStyleDump;
    uint8_t productId;

    // notes §29
    bool supportsKorgProgramDump;

    // notes §30
    int32_t  panelNameOffset;
    uint32_t panelNameBitOffset;
    uint32_t panelNameLen;
    int32_t  presetNameOffset;
    uint32_t presetNameBitOffset;
    uint32_t presetNameLen;

    // notes §31
    uint32_t nameLineWidth;

    // notes §32
    uint32_t presetBankCount;

    // notes §33
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

// notes §34
void layout_panel_section(tPanelSection * section, tRectangle origin, double gridColWidth, double gridRowHeight);

// notes §35
bool panel_dial_is_toggle(const tPanelDial * dial);

// notes §36
bool panel_dial_is_binary(const tPanelDial * dial);

// notes §37
bool panel_dial_needs_value_menu(const tPanelDial * dial);

// notes §38
bool panel_dial_is_disabled(const tPanelDial * dial, tPanelConfig * config);

tPanelSection * find_panel_section(tPanelConfig * config, const char * page, const char * section);
tPanelDial * find_panel_dial(tPanelSection * section, const char * id);

// notes §39
tPanelDial * find_panel_dial_anywhere(tPanelConfig * config, const char * id);

// Looks up the dial wired to a given SysEx parameter group/ID (see the
// "group="/"param=" file attributes), or NULL if none matches.
tPanelDial * find_panel_dial_by_param(tPanelSection * section, uint32_t group, uint32_t paramId);

// notes §40
tPanelDial * find_panel_dial_by_cc(tPanelConfig * config, uint8_t cc);

// notes §41
tPanelDial * find_panel_dial_by_kronos_param(tPanelConfig * config, uint32_t typ, uint32_t soc, uint32_t sub, uint32_t pid, uint32_t idx);

// notes §42
tPanelDial * find_panel_dial_by_label(tPanelConfig * config, const char * label);

#define PANEL_MAX_CANDIDATES    16

// One <device>.txt found by scan_panel_configs() — just enough to present a
// choice, not a full parsed config (that only happens for whichever one gets
// picked).
typedef struct {
    char filename[64];         // e.g. "z1.txt" — relative to the scanned dir
    char deviceName[PANEL_LABEL_LEN];
    char description[128];
} tPanelConfigCandidate;

// notes §43
uint32_t scan_panel_configs(const char * dir, tPanelConfigCandidate * outCandidates, uint32_t maxCandidates);

// notes §44
tRectangle panel_dial_hit_rect(const tPanelDial * dial);

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
