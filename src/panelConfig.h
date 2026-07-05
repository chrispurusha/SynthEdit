/*
 * The Z1-Edit application.
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

#define PANEL_ID_LEN          16
#define PANEL_LABEL_LEN       32
#define PANEL_MAX_NAMES       8
#define PANEL_MAX_COLOURS     16
#define PANEL_MAX_DIALS       32
#define PANEL_MAX_SECTIONS    8

typedef enum {
    dialDisplayRaw = 0,
    dialDisplayCcNative,
    dialDisplayNames,
} tDialDisplay;

typedef struct {
    char name[PANEL_ID_LEN];
    tRgb colour;
} tPanelColour;

typedef struct {
    char         id[PANEL_ID_LEN];              // e.g. "f1cut" — looked up by find_panel_dial()
    char         label[PANEL_LABEL_LEN];        // e.g. "F1 Cut"
    char         colourName[PANEL_ID_LEN];      // as written in the file, e.g. "f1"
    tRgb         colour;                        // resolved against the section's colour table at parse time
    uint32_t     max;
    tDialDisplay display;
    char         names[PANEL_MAX_NAMES][PANEL_LABEL_LEN]; // populated when display == dialDisplayNames
    uint32_t     nameCount;
    double       gapBefore;                               // extra flow-space inserted before this dial
    tRectangle   rect;                                    // populated by layout_panel_section(); used for render + hit-test
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

#ifdef __cplusplus
}
#endif

#endif // __PANEL_CONFIG_H__
