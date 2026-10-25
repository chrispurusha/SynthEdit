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
// Notes: Docs/code-notes/panelSteps.c.md - "// notes §k" refers there.

#ifdef __cplusplus
extern "C" {
#endif

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "synthlibDefs.h"
#include "synthlibGlobals.h"
#include "utilsGraphics.h"
#include "panelSteps.h"
#include "synthComms.h"
#include "synthGraphics.h"

#define LABEL_WIDTH    44.0
#define BAR_HEIGHT     36.0
#define BAR_GAP        6.0
#define CELL_GAP       1.0

typedef enum {
    eStepsNone = 0,
    eStepsCell,
    eStepsBar
} tStepsHit;

static tPanelSection * gDragSection = NULL;
static tStepsHit       gDragKind    = eStepsNone;
static uint32_t        gDragRow     = 0;      // the bit row or bar painted
static bool            gPaintOn     = false;  // cells: the state the drag paints
static uint32_t        gLastStep    = 0;      // notes §3: where the drag last painted, to fill the steps it jumped
static double          gLastShare   = 0.0;

// notes §1
static tPanelDial * step_dial(const char * firstId, uint32_t step, uint32_t stride) {
    char   id[PANEL_ID_LEN];
    size_t len   = strlen(firstId);
    size_t digit = len;

    while ((digit > 0) && isdigit((unsigned char)firstId[digit - 1])) {
        digit--;
    }

    if (digit == len) {
        return (step == 0) ? find_panel_dial_anywhere(synth_panel_config(), firstId) : NULL;
    }
    snprintf(id, sizeof(id), "%.*s%lu", (int)digit, firstId, strtoul(firstId + digit, NULL, 10) + ((unsigned long)step * stride));
    return find_panel_dial_anywhere(synth_panel_config(), id);
}

static uint32_t total_rows(const tPanelSteps * steps) {
    uint32_t rows = 0;

    for (uint32_t f = 0; f < steps->bitFieldCount; f++) {
        rows += steps->bitFields[f].bits;
    }

    return rows;
}

// Which field and bit a row (0 = top) is: the top row is the first field's highest bit.
static bool row_bit(const tPanelSteps * steps, uint32_t row, uint32_t * field, uint32_t * bit) {
    for (uint32_t f = 0; f < steps->bitFieldCount; f++) {
        uint32_t bits = steps->bitFields[f].bits;

        if (row < bits) {
            *field = f;
            *bit   = bits - 1 - row;
            return true;
        }
        row -= bits;
    }

    return false;
}

static uint32_t played_steps(const tPanelSteps * steps) {
    tPanelDial * length = (steps->length[0] != '\0') ? find_panel_dial_anywhere(synth_panel_config(), steps->length) : NULL;

    if (!length || (get_panel_dial_value(length) >= synth_dial_max(length))) {   // not loaded yet: nothing dimmed
        return steps->count;
    }
    int32_t      n      = (int32_t)get_panel_dial_value(length) + length->storageOffset;

    return (n < 0) ? 0 : ((uint32_t)n > steps->count) ? steps->count : (uint32_t)n;
}

// notes §2
typedef struct {
    double   gridTop;
    double   cellH;
    double   colW;
    double   left;
    uint32_t rows;
} tStepsGeometry;

static tStepsGeometry geometry(const tPanelSteps * steps) {
    tStepsGeometry g;
    double         barsH = (double)steps->barCount * (BAR_HEIGHT + BAR_GAP);
    double         gridH = steps->height - barsH;

    g.rows    = total_rows(steps);
    g.left    = steps->rect.coord.x + LABEL_WIDTH;
    g.gridTop = steps->rect.coord.y;
    g.colW    = (steps->width - LABEL_WIDTH) / (double)steps->count;
    g.cellH   = (g.rows > 0) ? (gridH / (double)g.rows) : 0.0;
    return g;
}

static double bar_top(const tPanelSteps * steps, const tStepsGeometry * g, uint32_t bar) {
    return g->gridTop + (g->cellH * g->rows) + BAR_GAP + ((double)bar * (BAR_HEIGHT + BAR_GAP));
}

// A value outside the dial's range (one not loaded yet) shows as an empty bar.
static double dial_share(const tPanelDial * dial) {
    uint32_t max   = synth_dial_max(dial);
    uint32_t value = get_panel_dial_value(dial);

    return ((max > 1) && (value < max)) ? (double)value / (double)(max - 1) : 0.0;
}

// A bar's name: step 1's dial label without its leading step number ("1 Vel" -> "Vel").
static const char * bar_label(const tPanelDial * dial) {
    const char * p = dial->label;

    while (isdigit((unsigned char)*p)) {
        p++;
    }

    while (*p == ' ') {
        p++;
    }
    return (*p != '\0') ? p : dial->label;
}

static tRgb scaled(tRgb c, double k) {
    return (tRgb){
        c.red * k, c.green * k, c.blue * k
    };
}

double panel_steps_render(tArea area, tPanelSection * section, tCoord origin) {
    tPanelSteps *  steps  = &section->steps;
    tRgb           on     = (section->colourCount > 0) ? section->colours[0].colour : (tRgb)RGB_WHITE;
    uint32_t       played = played_steps(steps);

    steps->rect = (tRectangle){
        origin, {
            steps->width, steps->height
        }
    };
    tStepsGeometry g      = geometry(steps);
    char           text[16];

    // rows, top to bottom: the bit's number across all fields, highest first
    for (uint32_t row = 0; row < g.rows; row++) {
        snprintf(text, sizeof(text), "%u", g.rows - 1 - row);
        set_rgb_colour((tRgb)RGB_GREY_7);
        render_text(area, (tRectangle){{origin.x, g.gridTop + (row * g.cellH) + ((g.cellH - 12.0) / 2.0)}, {LABEL_WIDTH - 8.0, 12.0}}, text);
    }

    for (uint32_t step = 0; step < steps->count; step++) {
        double x       = g.left + (step * g.colW);
        bool   playing = step < played;
        double dim     = playing ? 1.0 : 0.35;

        for (uint32_t row = 0; row < g.rows; row++) {
            uint32_t     field = 0;
            uint32_t     bit   = 0;
            row_bit(steps, row, &field, &bit);
            tPanelDial * dial  = step_dial(steps->bitFields[field].dial, step, steps->stride);
            bool         set   = dial && ((get_panel_dial_value(dial) >> bit) & 1u);

            set_rgb_colour(set ? scaled(on, dim) : scaled((tRgb)RGB_GREY_3, dim));
            render_rectangle(area, (tRectangle){{x + CELL_GAP, g.gridTop + (row * g.cellH) + CELL_GAP}, {g.colW - (2.0 * CELL_GAP), g.cellH - (2.0 * CELL_GAP)}});
        }

        for (uint32_t b = 0; b < steps->barCount; b++) {
            tPanelDial * dial  = step_dial(steps->bars[b].dial, step, steps->stride);
            double       top   = bar_top(steps, &g, b);
            double       share = dial ? dial_share(dial) : 0.0;

            set_rgb_colour(scaled((tRgb)RGB_GREY_2, dim));
            render_rectangle(area, (tRectangle){{x + CELL_GAP, top}, {g.colW - (2.0 * CELL_GAP), BAR_HEIGHT}});
            set_rgb_colour(scaled(on, dim));
            render_rectangle(area, (tRectangle){{x + CELL_GAP, top + (BAR_HEIGHT * (1.0 - share))}, {g.colW - (2.0 * CELL_GAP), BAR_HEIGHT * share}});
        }

        // every fourth step numbered under the bars
        if ((step % 4) == 0) {
            snprintf(text, sizeof(text), "%u", step + 1);
            set_rgb_colour((tRgb)RGB_GREY_7);
            render_text(area, (tRectangle){{x, bar_top(steps, &g, steps->barCount) - BAR_GAP + 2.0}, {g.colW * 3.0, 11.0}}, text);
        }
    }

    for (uint32_t b = 0; b < steps->barCount; b++) {
        tPanelDial * dial = step_dial(steps->bars[b].dial, 0, steps->stride);

        if (dial) {
            set_rgb_colour((tRgb)RGB_GREY_7);
            render_text(area, (tRectangle){{origin.x, bar_top(steps, &g, b) + (BAR_HEIGHT / 2.0) - 6.0}, {LABEL_WIDTH - 8.0, 12.0}}, bar_label(dial));
        }
    }

    return steps->height + 14.0;
}

// notes §3
static bool locate(tPanelSection * section, tCoord at, tStepsHit * kind, uint32_t * step, uint32_t * row, double * barShare) {
    tPanelSteps *  steps = &section->steps;
    tStepsGeometry g     = geometry(steps);

    if ((steps->rect.size.w <= 0.0) || (at.x < g.left) || (at.x >= g.left + (g.colW * steps->count))) {
        return false;
    }
    *step = (uint32_t)((at.x - g.left) / g.colW);

    if ((at.y >= g.gridTop) && (at.y < g.gridTop + (g.cellH * g.rows))) {
        *kind = eStepsCell;
        *row  = (uint32_t)((at.y - g.gridTop) / g.cellH);
        return true;
    }

    for (uint32_t b = 0; b < steps->barCount; b++) {
        double top = bar_top(steps, &g, b);

        if ((at.y >= top) && (at.y < top + BAR_HEIGHT)) {
            *kind     = eStepsBar;
            *row      = b;
            *barShare = 1.0 - ((at.y - top) / BAR_HEIGHT);
            return true;
        }
    }

    return false;
}

static void set_value(tPanelDial * dial, uint32_t value) {
    if (dial && !dial->readOnly && (get_panel_dial_value(dial) != value)) {
        synth_set_panel_dial_value(dial, value);
    }
}

static void paint_cell(const tPanelSteps * steps, uint32_t step, uint32_t row, bool on) {
    uint32_t     field = 0;
    uint32_t     bit   = 0;

    if (!row_bit(steps, row, &field, &bit)) {
        return;
    }
    tPanelDial * dial  = step_dial(steps->bitFields[field].dial, step, steps->stride);

    if (dial) {
        uint32_t value = get_panel_dial_value(dial);

        set_value(dial, on ? (value | (1u << bit)) : (value & ~(1u << bit)));
    }
}

static void paint_bar(const tPanelSteps * steps, uint32_t step, uint32_t bar, double share) {
    tPanelDial * dial = step_dial(steps->bars[bar].dial, step, steps->stride);

    if (dial) {
        uint32_t max = synth_dial_max(dial);

        share = (share < 0.0) ? 0.0 : (share > 1.0) ? 1.0 : share;
        set_value(dial, (uint32_t)lround(share * (double)((max > 1) ? (max - 1) : 0)));
    }
}

bool panel_steps_press(tPanelSection ** sections, uint32_t sectionCount, tCoord at) {
    for (uint32_t s = 0; s < sectionCount; s++) {
        tPanelSection * section = sections[s];
        tStepsHit       kind    = eStepsNone;
        uint32_t        step    = 0;
        uint32_t        row     = 0;
        double          share   = 0.0;

        if (!section->steps.present || !locate(section, at, &kind, &step, &row, &share)) {
            continue;
        }
        gDragSection = section;
        gDragKind    = kind;
        gDragRow     = row;
        gLastStep    = step;
        gLastShare   = share;

        if (kind == eStepsCell) {
            uint32_t     field = 0;
            uint32_t     bit   = 0;
            row_bit(&section->steps, row, &field, &bit);
            tPanelDial * dial  = step_dial(section->steps.bitFields[field].dial, step, section->steps.stride);

            gPaintOn = !(dial && ((get_panel_dial_value(dial) >> bit) & 1u));
            paint_cell(&section->steps, step, row, gPaintOn);
        } else {
            paint_bar(&section->steps, step, row, share);
        }
        synthlib_request_redraw();
        return true;
    }

    return false;
}

// notes §3: a drag paints - cells along the pressed row take the press's new state, a bar follows the pointer
void panel_steps_drag(tCoord at) {
    tStepsHit      kind  = eStepsNone;
    uint32_t       step  = 0;
    uint32_t       row   = 0;
    double         share = 0.0;

    if (!gDragSection) {
        return;
    }
    tStepsGeometry g     = geometry(&gDragSection->steps);

    if (gDragKind == eStepsBar) {
        double top = bar_top(&gDragSection->steps, &g, gDragRow);

        at.y = (at.y < top) ? top : (at.y > top + BAR_HEIGHT - 0.001) ? (top + BAR_HEIGHT - 0.001) : at.y;
    } else {
        at.y = g.gridTop + ((gDragRow + 0.5) * g.cellH);
    }

    if (!locate(gDragSection, at, &kind, &step, &row, &share) || (kind != gDragKind)) {
        return;
    }
    // every step between the last one painted and this one, the bar's level drawn in a straight line across them
    int32_t        from  = (int32_t)gLastStep;
    int32_t        to    = (int32_t)step;
    int32_t        dir   = (to >= from) ? 1 : -1;

    for (int32_t k = from; ; k += dir) {
        double t = (to == from) ? 1.0 : (double)(k - from) / (double)(to - from);

        if (kind == eStepsCell) {
            paint_cell(&gDragSection->steps, (uint32_t)k, gDragRow, gPaintOn);
        } else {
            paint_bar(&gDragSection->steps, (uint32_t)k, gDragRow, gLastShare + (t * (share - gLastShare)));
        }

        if (k == to) {
            break;
        }
    }

    gLastStep  = step;
    gLastShare = share;
    synthlib_request_redraw();
}

void panel_steps_release(void) {
    gDragSection = NULL;
    gDragKind    = eStepsNone;
}

bool panel_steps_dragging(void) {
    return gDragSection != NULL;
}

#ifdef __cplusplus
}
#endif
