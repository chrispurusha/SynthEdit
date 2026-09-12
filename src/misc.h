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
// Notes: Docs/code-notes/misc.h.md - "// notes §k" refers there.

#ifndef __MISC_H__
#define __MISC_H__

#ifdef __cplusplus
extern "C" {
#endif

// notes §1
void register_sleep_wake_notifications(void);
void setup_main_menu(void);

// notes §2
void init_settings(void);

// notes §3
void load_saved_settings(void);

// notes §4
const char * synth_temp_dir(void);

// notes §5
const char * get_saved_layouts_dir(void);
void set_saved_layouts_dir(const char * path);

// Returns the persisted "lastDeviceConfig" preference (a <device>.txt filename, e.g. "z1.txt"), or
// NULL if never set. Valid until the next call — copy it if you need to keep it. Implemented in
// persistence.c.
const char * get_saved_device_config(void);

// notes §6
void set_saved_device_config(const char * filename);

// notes §7
void prompt_choose_layouts_folder(void);

// notes §8
const char * get_last_backup_folder(const char * deviceKey);
void set_last_backup_folder(const char * deviceKey, const char * path);

#ifdef __cplusplus
}
#endif

#endif // __MISC_H__
