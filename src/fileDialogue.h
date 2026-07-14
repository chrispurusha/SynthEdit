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

#ifndef FILE_DIALOG_H
#define FILE_DIALOG_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*tFileDialogueCallback)(const char * path);

void open_file_read_dialogue_async(tFileDialogueCallback callback);

// startingDir points the panel at that folder if non-NULL and it still
// exists; NULL falls back to whatever the system/sandbox considers the
// default (previously the only option — every Backup save used to open
// wherever macOS felt like, with no memory of the last location, which is
// exactly what synthBackup.c's own get_last_backup_folder()/
// set_last_backup_folder() (below) now fix for that specific caller).
void open_file_write_dialogue_async(tFileDialogueCallback callback, const char * defaultName, const char * startingDir);

// Folder variant of the two above — NSOpenPanel configured to choose (or
// create) a directory rather than a file. Used by Backup > Bank (Individual
// Files)… (synthBackup.c) to pick where the per-preset .syx files + index
// land. callback receives NULL if the panel was cancelled, same convention
// as the two above. startingDir, same meaning as open_file_write_dialogue_async's
// own parameter above.
void open_folder_choose_dialogue_async(tFileDialogueCallback callback, const char * title, const char * startingDir);

// Small persisted-string helper (NSUserDefaults-backed) so multiple backup
// actions (Current Panel/Patch by Number/Bank/Bank-to-Folder, all in
// synthBackup.c) can share ONE "last folder used for a backup" default
// instead of each starting from an unrelated system default — added after
// real confusion: a Bank (Individual Files) export picks its own folder
// explicitly, but a single-file Backup save (Current Panel/Patch by
// Number/Bank) previously had no memory of it at all, so the two scattered
// across different locations with no obvious connection between them.
//
// deviceKey scopes the remembered folder PER CONNECTED DEVICE — added
// 2026-07-14 once "Device" menu switching (synth_switch_device_config(),
// synthGraphics.h) made it realistic to back up Z1 and Voyager patches in
// the same session; a single shared folder meant switching devices could
// silently point a backup at the WRONG synth's own folder. Callers pass
// synth_current_device_config() (synthGraphics.h — already included by
// every synthBackup.c call site) as deviceKey, e.g. "z1.txt"/"voyager.txt"
// — stable and unique per device file, unlike the human-readable device
// NAME (which nothing here actually needs to be unique). NULL/empty
// deviceKey falls back to one unscoped shared key (keeps this usable by
// any FUTURE caller that doesn't have a device context yet, and is a safe
// no-op for pre-2026-07-14 saved prefs — they simply become that fallback
// key's own value on first read, not silently lost).
// get_last_backup_folder() returns NULL if none has been set yet (first
// run for that device, or the saved path no longer exists).
const char * get_last_backup_folder(const char * deviceKey);
void set_last_backup_folder(const char * deviceKey, const char * path);

// Synchronous (unlike the two above) — blocks until the user responds, using
// [NSAlert runModal] directly rather than the async dispatch pattern the
// others use. Meant for the one-time startup device chooser, called before
// GLFW's Cocoa run loop is pumping events (an async completion handler
// wouldn't fire yet at that point); NSApplication/the window already exist
// by then, which is all a modal alert needs. Presents `labels[0..count-1]`
// in a dropdown, pre-selecting defaultIndex (clamped to a valid index if out
// of range); returns the chosen index, or -1 if cancelled. defaultIndex
// added 2026-07-11 for Store Patch to Bank, which defaults to the slot the
// current panel was originally loaded from (gDevice.currentProgram) rather
// than always starting at the first entry.
int32_t show_device_choice_dialogue(const char * title, const char * message, const char *const * labels, uint32_t count, uint32_t defaultIndex);

// Synchronous Yes/No confirmation (same [NSAlert runModal] shape as
// show_device_choice_dialogue() above, no accessory view) — for a
// destructive action (Restore Patch/Bank, synthBackup.c) that needs an
// explicit "are you sure" before sending, since there's no undo once the
// device has overwritten a stored slot. Returns true only if the user
// picked the affirmative button.
bool show_confirm_dialogue(const char * title, const char * message);

// Synchronous single-button ("OK") informational alert — for Restore
// (synthBackup.c) to surface a validation failure or a success confirmation
// that would otherwise only ever reach LOG_ERROR/LOG_DEBUG's stderr/stdout,
// invisible to anyone not watching a console. Added after a real report of
// Restore "not seeming to work" that was very likely exactly this: a
// silent validation failure (wrong file type/device chosen) with no way
// for the user to tell what happened.
void show_info_dialogue(const char * title, const char * message);

#ifdef __cplusplus
}
#endif

#endif // FILE_DIALOG_H
