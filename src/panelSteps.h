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

#ifndef __PANEL_STEPS_H__
#define __PANEL_STEPS_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>

#include "panelConfig.h"

// Draws `section`'s step grid (panelConfig.h notes §60) with its top-left at `origin`; returns its height.
double panel_steps_render(tArea area, tPanelSection * section, tCoord origin);

// notes §3
bool panel_steps_press(tPanelSection ** sections, uint32_t sectionCount, tCoord at);
void panel_steps_drag(tCoord at);
void panel_steps_release(void);
bool panel_steps_dragging(void);

#ifdef __cplusplus
}
#endif

#endif // __PANEL_STEPS_H__
