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

// register_sleep_wake_notifications() and setup_main_menu() are implemented in misc.mm — the only
// two things left in this codebase that genuinely need Objective-C/Cocoa. Everything else declared
// below is plain C: the File/Device/Controls/Layouts/Backup/Restore menus live in appMenuBar.c,
// menu actions in menuActions.c, and settings persistence (backed by SynthLib's cross-platform
// prefs.h rather than NSUserDefaults) lives in persistence.c.
void register_sleep_wake_notifications(void);
void setup_main_menu(void);

// Loads the prefs.txt file into memory (prefs_init(), prefs.h) — must run before ANY prefs_get_*/
// prefs_set_*/prefs_has_key call, including get_saved_layouts_dir()/get_saved_device_config() below.
// Called from main() BEFORE init_graphics(), not from setup_main_menu() (which itself runs AFTER
// init_graphics()) — init_graphics() calls synth_init_graphics() (synthGraphics.cpp) at its own
// tail, which reads get_saved_layouts_dir() to resolve the layouts folder for the very first frame;
// that read has to already see a loaded prefs file, or it silently falls back to the built-in
// default every single launch. Real bug found 2026-07-17: prefs_init() used to run inside
// setup_main_menu(), strictly after init_graphics() had already made that read against an empty,
// not-yet-loaded prefs store — the saved layouts folder was never actually being restored.
// Implemented in persistence.c.
void init_settings(void);

// Applies saved window size/position and dial mode — called once by setup_main_menu(), which runs
// after init_graphics() (needs gWindow to already exist for resize_window()/reposition_window()).
// Layouts dir and last device config are NOT restored here — see synth_init_graphics()
// (synthGraphics.cpp), which reads them directly and needs init_settings() above to have already
// run by then. Implemented in persistence.c.
void load_saved_settings(void);

void save_dial_mode(int mode);
void save_window_size(int w);
void save_window_pos(int x, int y);

// NSTemporaryDirectory() is still the simplest cross-launch-stable temp dir on macOS (App Sandbox
// or not — sandbox was only ever the reason this couldn't just be a hardcoded "/tmp/..." literal,
// not the reason this function exists at all), so this stays in misc.mm rather than moving to
// persistence.c. Valid until the next call — copy it if you need to keep it.
const char * synth_temp_dir(void);

// Returns the persisted layouts-folder path, or NULL if never set. No security-scoped bookmark
// needed any more (App Sandbox removed) — a plain saved path is all cross-launch access ever
// required. Valid until the next call — copy it if you need to keep it. Implemented in
// persistence.c.
const char * get_saved_layouts_dir(void);
void set_saved_layouts_dir(const char * path);

// Returns the persisted "lastDeviceConfig" preference (a <device>.txt filename, e.g. "z1.txt"), or
// NULL if never set. Valid until the next call — copy it if you need to keep it. Implemented in
// persistence.c.
const char * get_saved_device_config(void);

// Persists filename as the device config to default to on next launch — called from
// synth_choose_config_file() (synthGraphics.cpp) whenever a device is actually settled on,
// whichever of its 3 paths that came from (the single-candidate auto-pick, the startup chooser
// dialogue, or the "Device" menu's own live switch, synth_switch_device_config()). Implemented in
// persistence.c.
void set_saved_device_config(const char * filename);

// Opens the same "Choose Layouts Folder…" browser as the Layouts menu item, asynchronously —
// called from synth_choose_config_file() when the current layouts folder (saved pref, or the
// built-in default) has zero valid <device>.txt files in it, covering both "never set a folder
// yet" and "the folder moved/was emptied" without special-casing either. No Cocoa needed any more
// (App Sandbox removed) — implemented in menuActions.c over SynthLib's fileBrowser.h, the same way
// as every other picker action there.
void prompt_choose_layouts_folder(void);

// Small persisted-string helper (SynthLib's prefs.h-backed) so multiple backup actions (Current
// Panel/Patch by Number/Bank/Bank-to-Folder, all in synthBackup.c) can share ONE "last folder used
// for a backup" setting instead of each starting from an unrelated system default — added after
// real confusion: a Bank (Individual Files) export picks its own folder explicitly, but a
// single-file Backup save (Current Panel/Patch by Number/Bank) previously had no memory of it at
// all, so the two scattered across different locations with no obvious connection between them.
//
// deviceKey scopes the remembered folder PER CONNECTED DEVICE — added 2026-07-14 once "Device"
// menu switching (synth_switch_device_config(), synthGraphics.h) made it realistic to back up Z1
// and Voyager patches in the same session; a single shared folder meant switching devices could
// silently point a backup at the WRONG synth's own folder. Callers pass
// synth_current_device_config() (synthGraphics.h — already included by every synthBackup.c call
// site) as deviceKey, e.g. "z1.txt"/"voyager.txt" — stable and unique per device file, unlike the
// human-readable device NAME (which nothing here actually needs to be unique). NULL/empty
// deviceKey falls back to one unscoped shared key (keeps this usable by any FUTURE caller that
// doesn't have a device context yet, and is a safe no-op for pre-2026-07-14 saved prefs — they
// simply become that fallback key's own value on first read, not silently lost).
// get_last_backup_folder() returns NULL if none has been set yet (first run for that device, or
// the saved path no longer exists). Implemented over prefs_get_string()/prefs_set_string()
// (prefs.h) in persistence.c.
const char * get_last_backup_folder(const char * deviceKey);
void set_last_backup_folder(const char * deviceKey, const char * path);

#ifdef __cplusplus
}
#endif

#endif // __MISC_H__
