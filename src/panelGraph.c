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
// Notes: Docs/code-notes/panelGraph.c.md - "// notes §k" refers there.

#ifdef __cplusplus
extern "C" {
#endif

#include <math.h>
#include <string.h>

#include "synthlibDefs.h"
#include "synthlibGlobals.h"
#include "utilsGraphics.h"
#include "panelGraph.h"
#include "synthComms.h"
#include "synthGraphics.h"

static tPanelSection * gDragSection = NULL;
static int32_t         gDragPoint   = -1;
static tCoord          gDragStart   = {0.0, 0.0};
static int32_t         gDragAxis    = -1;   // when locked: 0 = x only, 1 = y only, -1 = not decided yet

static tPanelDial * graph_dial(const char * id) {
    return (id[0] != '\0') ? find_panel_dial_anywhere(synth_panel_config(), id) : NULL;
}

static uint32_t dial_steps(const tPanelDial * dial) {
    uint32_t max = synth_dial_max(dial);

    return (max > 1) ? max - 1 : 1;
}

static double dial_share(const tPanelDial * dial) {
    return dial ? (double)get_panel_dial_value(dial) / (double)dial_steps(dial) : 0.0;
}

// notes §2
static uint32_t graph_points(const tPanelSection * section, tGraphPoint * out, double * outSegments) {
    const tPanelGraph * graph = &section->graph;
    double              xs    = 0.0;

    for (uint32_t i = 0; i < graph->pointCount; i++) {
        const tGraphPointSpec * spec = &graph->points[i];
        tPanelDial *            t    = graph_dial(spec->tDial);
        tPanelDial *            xd   = graph_dial(spec->xDial);
        tPanelDial *            yd   = graph_dial(spec->yDial);

        if (xd) {
            xs = dial_share(xd) * graph->segments;
        } else if (t) {
            xs += dial_share(t);
        } else {
            xs = spec->xSegments;
        }
        outSegments[i] = xs;
        out[i]         = (tGraphPoint){
            xs / graph->segments, yd ? dial_share(yd) : spec->yConst, !graph->readOnly && (t || xd), !graph->readOnly && (yd != NULL)
        };
    }

    return graph->pointCount;
}

// The height of a bipolar level's zero: the first level dial that shows a sign.
static double graph_axis(const tPanelSection * section) {
    for (uint32_t i = 0; i < section->graph.pointCount; i++) {
        tPanelDial * yd = graph_dial(section->graph.points[i].yDial);

        if (yd && (yd->displayOffset > 0)) {
            return (double)yd->displayOffset / (double)dial_steps(yd);
        }
    }

    return -1.0;
}

double panel_graph_render(tArea area, tPanelSection * section, tCoord origin) {
    tGraphPoint points[PANEL_GRAPH_MAX_POINTS];
    double      segments[PANEL_GRAPH_MAX_POINTS];
    uint32_t    count = graph_points(section, points, segments);
    tRgb        line  = (section->colourCount > 0) ? section->colours[0].colour : (tRgb)RGB_WHITE;
    tGraphStyle style = {(tRgb)RGB_GREY_5, line, (tRgb)RGB_GREY_7, (tRgb)RGB_WHITE, graph_axis(section)};

    section->graph.rect = (tRectangle){
        origin, {
            section->graph.width, section->graph.height
        }
    };
    graph_draw(area, section->graph.rect, points, count, &style, (gDragSection == section) ? gDragPoint : -1);
    return section->graph.height;
}

bool panel_graph_press(tPanelSection ** sections, uint32_t sectionCount, tCoord at) {
    for (uint32_t s = 0; s < sectionCount; s++) {
        tPanelSection * section = sections[s];
        tGraphPoint     points[PANEL_GRAPH_MAX_POINTS];
        double          segments[PANEL_GRAPH_MAX_POINTS];

        if (!section->graph.present || (section->graph.rect.size.w <= 0.0)) {
            continue;
        }
        uint32_t        count   = graph_points(section, points, segments);
        int32_t         hit     = graph_hit_point(section->graph.rect, points, count, at);

        if (hit >= 0) {
            gDragSection = section;
            gDragPoint   = hit;
            gDragStart   = at;
            gDragAxis    = -1;
            return true;
        }
    }

    return false;
}

static void set_share(tPanelDial * dial, double share) {
    if (!dial || dial->readOnly) {
        return;
    }
    share = (share < 0.0) ? 0.0 : (share > 1.0) ? 1.0 : share;
    synth_set_panel_dial_value(dial, (uint32_t)lround(share * dial_steps(dial)));
}

void panel_graph_drag(tCoord at, bool lockToOneAxis) {
    if (!gDragSection || (gDragPoint < 0)) {
        return;
    }
    const tPanelGraph *     graph = &gDragSection->graph;
    const tGraphPointSpec * spec  = &graph->points[gDragPoint];
    tGraphPoint             points[PANEL_GRAPH_MAX_POINTS];
    double                  segments[PANEL_GRAPH_MAX_POINTS];
    tCoord                  pos   = graph_position(graph->rect, at);

    graph_points(gDragSection, points, segments);

    // notes §3
    if (lockToOneAxis && (gDragAxis < 0) && (hypot(at.x - gDragStart.x, at.y - gDragStart.y) > 4.0)) {
        gDragAxis = (fabs(at.x - gDragStart.x) >= fabs(at.y - gDragStart.y)) ? 0 : 1;
    }
    bool                    moveX = !lockToOneAxis || (gDragAxis == 0);
    bool                    moveY = !lockToOneAxis || (gDragAxis == 1);

    if (moveX && points[gDragPoint].movesX) {
        double xs = pos.x * graph->segments;

        if (spec->xDial[0] != '\0') {
            set_share(graph_dial(spec->xDial), xs / graph->segments);
        } else {
            set_share(graph_dial(spec->tDial), xs - ((gDragPoint > 0) ? segments[gDragPoint - 1] : 0.0));
        }
    }

    if (moveY && points[gDragPoint].movesY) {
        set_share(graph_dial(spec->yDial), pos.y);
    }
    synthlib_request_redraw();
}

void panel_graph_release(void) {
    gDragSection = NULL;
    gDragPoint   = -1;
    gDragAxis    = -1;
}

bool panel_graph_dragging(void) {
    return gDragSection != NULL;
}

#ifdef __cplusplus
}
#endif
