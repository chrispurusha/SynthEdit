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

// Generic parser for the panel-descriptor text format used by z1.txt (and,
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

#define PANEL_ID_LEN            16
#define PANEL_LABEL_LEN         32
#define PANEL_MAX_NAMES         8
#define PANEL_MAX_COLOURS       16
#define PANEL_MAX_DIALS         32
#define PANEL_MAX_SECTIONS      8
#define PANEL_MAX_LIST_ITEMS    32
#define PANEL_MAX_LISTS         8

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
    // given dial *means*. All parsed from the file; application code only
    // resolves valuePtr/nativeValuePtr (see e.g. synth_bind_panel_dials()).
    int32_t   storageOffset;     // storage_value = display_value + storageOffset (e.g. 1-5 vs 0-4 for "type")
    uint32_t  paramGroup;        // SysEx parameter group
    uint32_t  paramId;           // SysEx parameter ID
    uint32_t  ccNumber;          // MIDI CC number; 0 = not CC-controlled (send SysEx param change instead)
    uint32_t  nativeMax;         // native/SysEx value range when paired with a CC (0 = no native pairing)
    uint8_t * valuePtr;          // bound at runtime to the live storage-space value; NULL until resolved
    uint8_t * nativeValuePtr;    // bound at runtime to the live native value, if any; NULL if unused
} tPanelDial;

typedef struct {
    char         page[PANEL_ID_LEN];
    char         section[PANEL_ID_LEN];
    double       dialSize;
    double       spacing;
    tPanelColour colours[PANEL_MAX_COLOURS];
    uint32_t     colourCount;
    tPanelDial   dials[PANEL_MAX_DIALS];
    uint32_t     dialCount;
} tPanelSection;

typedef struct {
    char          deviceName[PANEL_LABEL_LEN];
    uint32_t      manufacturerId;
    uint32_t      familyId;
    uint32_t      memberId;
    tPanelSection sections[PANEL_MAX_SECTIONS];
    uint32_t      sectionCount;
    tPanelList    lists[PANEL_MAX_LISTS];
    uint32_t      listCount;
} tPanelConfig;

// Parses the file at `path` into `config` (which is zeroed first). Malformed
// lines are logged via LOG_ERROR and skipped rather than aborting the parse.
// Returns false only if the file couldn't be opened.
bool load_panel_config(const char * path, tPanelConfig * config);

// Computes each dial's `rect` in `section`, flowing left to right from
// `origin` using the section's dialSize/spacing and each dial's gapBefore.
void layout_panel_section(tPanelSection * section, tRectangle origin);

tPanelSection * find_panel_section(tPanelConfig * config, const char * page, const char * section);
tPanelDial * find_panel_dial(tPanelSection * section, const char * id);

// Returns the index into section->dials[] under `point`, or -1 if none.
// Call after layout_panel_section() has populated the dials' rects.
int32_t hit_test_panel_section(tPanelSection * section, tCoord point);

// Display-space value (0..max-1), read via the bound valuePtr/nativeValuePtr —
// pure pointer arithmetic, no protocol knowledge. Returns 0 if unbound.
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
