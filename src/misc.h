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

#ifndef __MISC_H__
#define __MISC_H__

#ifdef __cplusplus
extern "C" {
#endif

void register_sleep_wake_notifications(void);
void setup_main_menu(void);
void save_window_size(int w);
void save_window_pos(int x, int y);

// Returns the persisted "layoutsDir" preference, or NULL if never set.
// Valid until the next call — copy it if you need to keep it.
const char * get_saved_layouts_dir(void);

// Puts up the same "Choose Layouts Folder…" NSOpenPanel as the Layouts menu
// item, asynchronously. Called from synth_choose_config_file() when the
// current layouts folder (saved bookmark, or the built-in default) has zero
// valid <device>.txt files in it — covers both "never set a folder yet" and
// "the folder moved/was emptied" without special-casing either.
void prompt_choose_layouts_folder(void);

#ifdef __cplusplus
}
#endif

#endif // __MISC_H__
