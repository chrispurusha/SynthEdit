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

#import "fileDialogue.h"
#import <Cocoa/Cocoa.h>
#import <AppKit/AppKit.h>

void open_file_read_dialogue_async(tFileDialogueCallback callback) {
    dispatch_async(dispatch_get_main_queue(), ^{
        NSOpenPanel * panel = [NSOpenPanel openPanel];
        [panel setCanChooseFiles:YES];
        [panel setCanChooseDirectories:NO];
        [panel setAllowsMultipleSelection:NO];
        [panel setTitle:@"Select a File"];

        [panel beginWithCompletionHandler:^(NSModalResponse result) {
             if (result == NSModalResponseOK) {
                 NSString * path = [panel.URL path];

                 if (callback) {
                     callback([path UTF8String]);
                 }
             } else {
                 if (callback) {
                     callback(NULL);
                 }
             }
         }];
    });
}

void open_file_write_dialogue_async(tFileDialogueCallback callback, const char * defaultName, const char * startingDir) {
    // Capture defaultName/startingDir before dispatching — either may be
    // stack-allocated
    NSString * nameString = (defaultName && defaultName[0] != '\0')
                          ? [NSString stringWithUTF8String:defaultName]
                          : @"patch.pch2";
    NSURL *    dirURL     = (startingDir && startingDir[0] != '\0')
                          ? [NSURL fileURLWithPath:[NSString stringWithUTF8String:startingDir] isDirectory:YES]
                          : nil;

    dispatch_async(dispatch_get_main_queue(), ^{
        NSSavePanel * panel = [NSSavePanel savePanel];
        [panel setTitle:@"Save File As"];
        [panel setPrompt:@"Save"];
        [panel setCanCreateDirectories:YES];
        [panel setNameFieldStringValue:nameString];
        [panel setMessage:@"Choose where to save your file."];
        [panel setShowsTagField:NO];
        [panel setExtensionHidden:NO];

        if (dirURL) {
            [panel setDirectoryURL:dirURL];
        }
        [panel beginWithCompletionHandler:^(NSModalResponse result) {
             if (result == NSModalResponseOK) {
                 NSString * path = panel.URL.path;

                 if (callback) {
                     callback([path UTF8String]);
                 }
             } else {
                 if (callback) {
                     callback(NULL);
                 }
             }
         }];
    });
}

void open_folder_choose_dialogue_async(tFileDialogueCallback callback, const char * title, const char * startingDir) {
    NSString * titleString = [NSString stringWithUTF8String:(title && title[0] != '\0') ? title : "Choose a Folder"];
    NSURL *    dirURL      = (startingDir && startingDir[0] != '\0')
                          ? [NSURL fileURLWithPath:[NSString stringWithUTF8String:startingDir] isDirectory:YES]
                          : nil;

    dispatch_async(dispatch_get_main_queue(), ^{
        NSOpenPanel * panel = [NSOpenPanel openPanel];
        [panel setCanChooseFiles:NO];
        [panel setCanChooseDirectories:YES];
        [panel setCanCreateDirectories:YES];
        [panel setAllowsMultipleSelection:NO];
        [panel setTitle:titleString];

        if (dirURL) {
            [panel setDirectoryURL:dirURL];
        }
        [panel beginWithCompletionHandler:^(NSModalResponse result) {
             if (result == NSModalResponseOK) {
                 NSString * path = [panel.URL path];

                 if (callback) {
                     callback([path UTF8String]);
                 }
             } else {
                 if (callback) {
                     callback(NULL);
                 }
             }
         }];
    });
}

// NSUserDefaults key for get_last_backup_folder()/set_last_backup_folder()
// (fileDialogue.h) — plain string, not a security-scoped bookmark like
// misc.mm's layouts-folder chooser: a backup destination doesn't need
// access rights restored across launches the way a folder this app reads
// FROM every startup does — [NSSavePanel setDirectoryURL:] works fine with
// just a path even after a relaunch (it's the user picking where to write
// next, not this app reopening something on its own).
static NSString *const kLastBackupFolderKey = @"lastBackupFolder";

const char * get_last_backup_folder(void) {
    static char buf[1024];
    NSString *  saved = [[NSUserDefaults standardUserDefaults] stringForKey:kLastBackupFolderKey];

    if (!saved) {
        return NULL;
    }
    BOOL        isDir = NO;

    if (![[NSFileManager defaultManager] fileExistsAtPath:saved isDirectory:&isDir] || !isDir) {
        return NULL; // moved/deleted since last use — fall back to the system default rather than pointing at nothing
    }
    strncpy(buf, [saved UTF8String], sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    return buf;
}

void set_last_backup_folder(const char * path) {
    if (!path || (path[0] == '\0')) {
        return;
    }
    [[NSUserDefaults standardUserDefaults] setObject:[NSString stringWithUTF8String:path] forKey:kLastBackupFolderKey];
}

int32_t show_device_choice_dialogue(const char * title, const char * message, const char *const * labels, uint32_t count, uint32_t defaultIndex) {
    if (count == 0) {
        return -1;
    }
    NSString *      titleString   = [NSString stringWithUTF8String:(title ? title : "")];
    NSString *      messageString = [NSString stringWithUTF8String:(message ? message : "")];

    NSAlert *       alert         = [[NSAlert alloc] init];

    [alert setAlertStyle:NSAlertStyleInformational];
    [alert setMessageText:titleString];
    [alert setInformativeText:messageString];
    [alert addButtonWithTitle:@"Choose"];
    [alert addButtonWithTitle:@"Cancel"];

    NSPopUpButton * popup         = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(0, 0, 360, 24) pullsDown:NO];

    for (uint32_t i = 0; i < count; i++) {
        [popup addItemWithTitle:[NSString stringWithUTF8String:labels[i]]];
    }

    if (defaultIndex < count) {
        [popup selectItemAtIndex:defaultIndex];
    }
    [alert setAccessoryView:popup];
    [[alert window] setInitialFirstResponder:popup];

    // Synchronous: no dispatch_async wrapper. Called at startup, before
    // GLFW's Cocoa run loop is pumping — an async block's completion handler
    // wouldn't fire yet, but NSApplication/the window already exist by this
    // point (init_graphics() creates them before calling here), which is all
    // a modal alert's own private run loop needs.
    NSModalResponse response      = [alert runModal];

    if (response != NSAlertFirstButtonReturn) {
        return -1;
    }
    return (int32_t)[popup indexOfSelectedItem];
}

bool show_confirm_dialogue(const char * title, const char * message) {
    NSString *      titleString   = [NSString stringWithUTF8String:(title ? title : "")];
    NSString *      messageString = [NSString stringWithUTF8String:(message ? message : "")];

    NSAlert *       alert         = [[NSAlert alloc] init];

    [alert setAlertStyle:NSAlertStyleWarning];
    [alert setMessageText:titleString];
    [alert setInformativeText:messageString];
    [alert addButtonWithTitle:@"Continue"];
    [alert addButtonWithTitle:@"Cancel"];

    // Synchronous, same reasoning as show_device_choice_dialogue() above —
    // every caller of this (Restore Patch/Bank, synthBackup.c) is already
    // running inside a menu action or a file-open dialog's own completion
    // handler, both on the main thread, so a blocking runModal here is
    // safe and simpler than threading a callback through one more layer.
    NSModalResponse response      = [alert runModal];

    return response == NSAlertFirstButtonReturn;
}

void show_info_dialogue(const char * title, const char * message) {
    NSString * titleString   = [NSString stringWithUTF8String:(title ? title : "")];
    NSString * messageString = [NSString stringWithUTF8String:(message ? message : "")];

    NSAlert *  alert         = [[NSAlert alloc] init];

    [alert setAlertStyle:NSAlertStyleInformational];
    [alert setMessageText:titleString];
    [alert setInformativeText:messageString];
    [alert addButtonWithTitle:@"OK"];

    // Synchronous, same reasoning as show_confirm_dialogue() above.
    [alert runModal];
}
