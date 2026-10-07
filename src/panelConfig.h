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
#define PANEL_PAGE_LEN             64 // a page is a path of up to PANEL_PAGE_LEVELS names - notes §46
#define PANEL_PAGE_LEVELS          3
#define PANEL_PAGE_SEPARATOR       '|'
#define PANEL_MAX_MODE_TABS        16
#define PANEL_MAX_PAGE_VARIANTS    8
#define PANEL_MAX_TAB_LABELS       8
#define PANEL_MAX_NAMES            256 // notes §53 (was 65: each dial carried a fixed array of this many names)
#define PANEL_MAX_COLOURS          16
#define PANEL_MAX_DIALS            32
#define PANEL_MAX_SECTIONS         256             // notes §54
#define PANEL_MAX_LIST_ITEMS       PANEL_MAX_NAMES // a list can feed a dial's names=@list
#define PANEL_MAX_BANKS            8               // notes §55
#define PANEL_MAX_LISTS            128             // notes §53 (the Z1's value tables are shared lists)
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
    char    page[PANEL_PAGE_LEN];
    int32_t col;
    char    label[PANEL_LABEL_LEN];
} tColumnLabel;

typedef struct {
    char         id[PANEL_ID_LEN];          // e.g. "f1cut" — looked up by find_panel_dial()
    char         label[PANEL_LABEL_LEN];    // e.g. "F1 Cut"
    char         colourName[PANEL_ID_LEN];  // as written in the file, e.g. "f1"
    tRgb         colour;                    // resolved against the section's colour table at parse time
    uint32_t     max;                       // count of valid display-space positions: 0..max-1
    tDialDisplay display;
    char (*names)[PANEL_LABEL_LEN];         // notes §53: nameCount labels on the heap, when display == dialDisplayNames
    uint32_t     nameCount;
    double       gapBefore;                 // extra flow-space inserted before this dial
    tRectangle   rect;                      // populated by layout_panel_section(); used for render + hit-test

    // notes §6
    char         linkedMaxDialId[PANEL_ID_LEN];
    char         linkedMinDialId[PANEL_ID_LEN];

    // notes §7
    char         disabledUnlessDialId[PANEL_ID_LEN];
    uint32_t     disabledUnlessValue;

    // notes §8
    int32_t      storageOffset; // storage_value = display_value + storageOffset (e.g. 1-5 vs 0-4 for "type")
    int32_t      displayOffset; // dialDisplaySigned only — shown_value = display_value - displayOffset; see that enum value's own comment above. Unrelated to storageOffset: never touches the wire.
    uint32_t     paramGroup;    // SysEx parameter group
    uint32_t     paramId;       // SysEx parameter ID

    // notes §9
    bool         wireSigned;

    // notes §51
    uint32_t     variantMax;    // 0 = the same range on a pageVariant's tab as on its base

    // notes §10
    bool         hasKronosParam;
    uint32_t     kronosTyp;
    uint32_t     kronosSoc;
    uint32_t     kronosSub;
    uint32_t     kronosPid;
    uint32_t     kronosIdx;
    uint32_t     ccNumber;      // MIDI CC number; 0 = not CC-controlled (send SysEx param change instead)
    // notes §11
    uint32_t     ccLsbNumber;
    uint8_t      ccMsbLatched;  // last raw byte seen on ccNumber; only meaningful when ccLsbNumber != 0
    uint8_t      ccLsbLatched;  // last raw byte seen on ccLsbNumber; only meaningful when ccLsbNumber != 0
    uint32_t     nativeMax;     // native/SysEx value range when paired with a CC (0 = no native pairing)
    uint32_t     value;         // live storage-space value (display_value + storageOffset); wide enough
                                // for a 14-bit CC pair, not just a single byte
    uint8_t      nativeValue;   // live native value, if nativeMax != 0; unused otherwise

    // notes §12
    bool         hasPendingCc;
    uint32_t     pendingRawValue;
    double       pendingSinceMs;

    // notes §13
    bool         hasPendingDumpSend;
    uint32_t     pendingDumpRawValue;
    double       pendingDumpSinceMs;

    // notes §14
    bool         dumpSendAwaitingFreshData;

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
    bool     dumpSigned;         // dumpBitWidth-bit two's complement - notes §45

    // notes §18
    double   gridCol;
    double   gridRow;

    // notes §19
    bool     noLabel;

    // notes §20
    bool     readOnly;

    // notes §21
    bool     asDial;

    // notes §22
    bool     asMenu;

    // notes §23
    int32_t  hiLoOffset;
    uint32_t hiLoCoarseScale;
    uint32_t hiLoFineScale;
} tPanelDial;

// notes §56
#define PANEL_GRAPH_MAX_POINTS    12

typedef struct {
    double xSegments;            // a fixed x, in segments from the left, when neither dial below is named
    char   tDial[PANEL_ID_LEN];  // x moves on from the previous point by this dial's share of one segment
    char   xDial[PANEL_ID_LEN];  // x is this dial's share of the whole width
    char   yDial[PANEL_ID_LEN];  // y is this dial's share of the height
    double yConst;               // y (0..1) when no yDial
} tGraphPointSpec;

typedef struct {
    bool            present;
    bool            readOnly;
    double          width;
    double          height;
    uint32_t        segments;
    tGraphPointSpec points[PANEL_GRAPH_MAX_POINTS];
    uint32_t        pointCount;
    tRectangle      rect;        // where it was last drawn
} tPanelGraph;

typedef struct {
    char     page[PANEL_PAGE_LEN];
    char     section[PANEL_ID_LEN];
    double   dialSize;
    double   spacing;
    int32_t  showIfOffset;       // notes §49: shown only while this dump byte (-1 = always) ...
    uint32_t showIfValue;        // ... holds this value
    // notes §52
    bool     hasVariantParamDelta;
    int32_t  variantParamDelta;
    bool     hidden;             // true: not a rendered control/page-tab target, just named
                                 // device state (e.g. program category, voice mode, unison) —
                                 // shown as plain "label: value" text instead of a dial widget.
                                 // Still parsed/bound/dump-scanned exactly like any other section.
    tPanelColour colours[PANEL_MAX_COLOURS];
    uint32_t     colourCount;
    tPanelDial   dials[PANEL_MAX_DIALS];
    uint32_t     dialCount;
    tPanelGraph  graph;          // notes §56
} tPanelSection;

typedef struct {
    uint32_t mode;
    char     tab[PANEL_ID_LEN];
} tModeTab;

// notes §48
typedef struct {
    char    variant[PANEL_PAGE_LEN]; // e.g. "Prog|EXi 2"
    char    base[PANEL_PAGE_LEN];    // e.g. "Prog|EXi 1"
    int32_t typDelta;
    int32_t dumpDelta;
    int32_t paramDelta;   // notes §48: Korg Parameter Change ID shift (e.g. +2048, the next ExID slot)
    int32_t showIfOffset; // notes §57: the variant's tab shows only while this dump byte (-1 = always) ...
    int32_t showIfMin;    // ... is within min..max
    int32_t showIfMax;
} tPageVariant;

// notes §55
typedef struct {
    char    name[8];       // shown before the program number, e.g. "A" -> "A042"
    int32_t msb;           // Bank Select CC0 value, -1 = not sent
    int32_t lsb;           // Bank Select CC32 value, -1 = not sent
    int32_t declaredMsb;   // the layout's own values, kept when the device's map replaces msb/lsb
    int32_t declaredLsb;
    int32_t msbDumpOffset; // where bankMapReply finds the device's own values, -1 = not there
    int32_t lsbDumpOffset;
} tBankSelect;

// notes §50
typedef struct {
    char     tab[PANEL_PAGE_LEN];
    int32_t  dumpOffset;
    char     names[PANEL_MAX_NAMES][PANEL_LABEL_LEN];
    uint32_t nameCount;
} tTabLabel;

typedef struct {
    char     deviceName[PANEL_LABEL_LEN];
    char     description[128]; // one-line summary from the file's "description"
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
    bool          supportsIdentity;
    uint32_t      midiChannel;                // 1-indexed; only meaningful when !supportsIdentity
    // notes §26
    char          midiPortName[64];

    // notes §27
    uint8_t       stateRequestSysEx[32];
    uint32_t      stateRequestSysExLen;

    // notes §28
    bool          moogStyleDump;
    uint8_t       productId;

    // notes §29
    bool          supportsKorgProgramDump;

    // notes §30
    int32_t       panelNameOffset;
    uint32_t      panelNameBitOffset;
    uint32_t      panelNameLen;
    int32_t       presetNameOffset;
    uint32_t      presetNameBitOffset;
    uint32_t      presetNameLen;

    // notes §31
    uint32_t      nameLineWidth;

    // notes §32
    uint32_t      presetBankCount;
    // notes §55
    tBankSelect   banks[PANEL_MAX_BANKS];
    uint32_t      bankCount;
    uint8_t       bankMapRequest[16];      // the request after the SysEx header, without F7
    uint32_t      bankMapRequestLen;
    int32_t       bankMapReplyFunc;        // -1 = no bank map to read
    int32_t       bankMapReplySub;
    int32_t       pcTransmitOffset;        // -1 = not known; else byte in the bank-map reply ...
    uint32_t      pcTransmitShift;         // ... >> shift & mask, 0 = the device sends no Program Change
    uint32_t      pcTransmitMask;
    int32_t       startupSlot;             // notes §55: the program a device powers up on (bank * 128 + program), -1 = none

    // notes §33
    double        gridColWidth;
    double        gridRowHeight;
    tPanelSection sections[PANEL_MAX_SECTIONS];
    uint32_t      sectionCount;
    tPanelList    lists[PANEL_MAX_LISTS];
    uint32_t      listCount;
    tColumnLabel  columnLabels[PANEL_MAX_COLUMN_LABELS];
    uint32_t      columnLabelCount;
    // notes §47
    tModeTab      modeTabs[PANEL_MAX_MODE_TABS];
    uint32_t      modeTabCount;
    tPageVariant  pageVariants[PANEL_MAX_PAGE_VARIANTS];
    uint32_t      pageVariantCount;
    tTabLabel     tabLabels[PANEL_MAX_TAB_LABELS];
    uint32_t      tabLabelCount;
} tPanelConfig;

// True if `page` is `prefix` or a page below it in the path.
bool panel_page_is_under(const char * page, const char * prefix);

// The section holding `dial`, or NULL.
tPanelSection * panel_section_of_dial(tPanelConfig * config, const tPanelDial * dial);

// The top-level page tab ("modeTab" lines) for a device-reported mode, or NULL.
const char * panel_mode_tab_name(const tPanelConfig * config, uint32_t mode);

// The device mode whose "modeTab" names this top-level tab, or -1.
int32_t panel_mode_for_tab(const tPanelConfig * config, const char * tab);

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
