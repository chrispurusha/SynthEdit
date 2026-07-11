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

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*tFileDialogueCallback)(const char * path);

void open_file_read_dialogue_async(tFileDialogueCallback callback);
void open_file_write_dialogue_async(tFileDialogueCallback callback, const char * defaultName);

// Folder variant of the two above — NSOpenPanel configured to choose (or
// create) a directory rather than a file. Used by Backup > Bank (Individual
// Files)… (synthBackup.c) to pick where the per-preset .syx files + index
// land. Deliberately separate from misc.mm's own layouts-folder chooser,
// which persists its choice as a security-scoped bookmark for reuse across
// launches — a backup destination is a one-off, nothing to remember.
// callback receives NULL if the panel was cancelled, same convention as the
// two above.
void open_folder_choose_dialogue_async(tFileDialogueCallback callback, const char * title);

// Synchronous (unlike the two above) — blocks until the user responds, using
// [NSAlert runModal] directly rather than the async dispatch pattern the
// others use. Meant for the one-time startup device chooser, called before
// GLFW's Cocoa run loop is pumping events (an async completion handler
// wouldn't fire yet at that point); NSApplication/the window already exist
// by then, which is all a modal alert needs. Presents `labels[0..count-1]`
// in a dropdown; returns the chosen index, or -1 if cancelled.
int32_t show_device_choice_dialogue(const char * title, const char * message, const char *const * labels, uint32_t count);

#ifdef __cplusplus
}
#endif

#endif // FILE_DIALOG_H
