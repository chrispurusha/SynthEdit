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

// The one menu action with enough of its own logic (and a public misc.h declaration other files
// call, synthGraphics.cpp's synth_choose_config_file()) to warrant living outside appMenuBar.c —
// everything else is a thin tMenuItem-signature wrapper and lives there instead, next to the menu
// structure it serves.

#include "defs.h"
#include "synthlibDefs.h"
#include "misc.h"
#include "synthGraphics.h"
#include "fileBrowser.h"
#include "graphics.h"

static void on_layouts_folder_chosen(const char * path) {
    if (path == NULL) {
        LOG_DEBUG("Choose Layouts Folder: cancelled\n");
        return;
    }
    set_saved_layouts_dir(path);
    synth_set_layouts_dir(path);
    wake_glfw();
}

void prompt_choose_layouts_folder(void) {
    open_file_browser_folder(on_layouts_folder_chosen, "Choose Layouts Folder");
}
