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

#import "misc.h"
#import <Cocoa/Cocoa.h>
#include "defs.h"
#include "globalVars.h"
#include "graphics.h"
#include "midiComms.h"
#include "synthGraphics.h"
#include "synthBackup.h"
#include "fileDialogue.h"

// Kept alive (and security-scope-accessing) for the app's lifetime once a
// layouts folder has been resolved — either from a saved bookmark at launch,
// or from a fresh pick via the menu. macOS revokes access on process exit,
// so there's no matching stopAccessingSecurityScopedResource on quit.
static NSURL * gLayoutsDirURL = nil;

static void start_accessing_layouts_dir(NSURL * url) {
    if (gLayoutsDirURL) {
        [gLayoutsDirURL stopAccessingSecurityScopedResource];
    }
    [url startAccessingSecurityScopedResource];
    gLayoutsDirURL = url;
}

// Under App Sandbox, a plain saved path string doesn't carry access rights
// across launches — only the security-scoped bookmark created at pick time
// does (see chooseLayoutsFolder: below). Returns NULL if there's no saved
// bookmark, or it couldn't be resolved (folder moved/deleted, permission
// revoked, etc.) — callers fall back to the built-in default in that case.
const char * get_saved_layouts_dir(void) {
    static char buf[1024];
    NSData *    bookmark = [[NSUserDefaults standardUserDefaults] dataForKey:@"layoutsDirBookmark"];

    if (!bookmark) {
        return NULL;
    }
    BOOL        stale    = NO;
    NSError *   error    = nil;
    NSURL *     url      = [NSURL URLByResolvingBookmarkData:bookmark
                            options:NSURLBookmarkResolutionWithSecurityScope
                            relativeToURL:nil
                            bookmarkDataIsStale:&stale
                            error:&error];

    if (!url || ![url startAccessingSecurityScopedResource]) {
        LOG_ERROR("couldn't resolve saved layouts folder bookmark: %s\n",
                  error ? [[error localizedDescription] UTF8String] : "unknown error");
        return NULL;
    }
    gLayoutsDirURL       = url;

    strncpy(buf, [[url path] UTF8String], sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    return buf;
}

// Shared by the "Choose Layouts Folder…" menu action and the automatic
// prompt synth_choose_config_file() triggers when the current folder has no
// valid <device>.txt in it.
static void do_choose_layouts_folder(void) {
    NSOpenPanel * panel = [NSOpenPanel openPanel];

    [panel setCanChooseFiles:NO];
    [panel setCanChooseDirectories:YES];
    [panel setAllowsMultipleSelection:NO];
    [panel setTitle:@"Choose Layouts Folder"];

    [panel beginWithCompletionHandler:^(NSModalResponse result) {
         if (result != NSModalResponseOK) {
             return;
         }
         NSURL * url = panel.URL;
         NSError * error = nil;
         // Security-scoped bookmark, not just the path — this is what lets the
         // choice survive across app launches under App Sandbox.
         NSData * bookmark = [url bookmarkDataWithOptions:NSURLBookmarkCreationWithSecurityScope
                              includingResourceValuesForKeys:nil
                              relativeToURL:nil
                              error:&error];

         if (!bookmark) {
             LOG_ERROR("couldn't create bookmark for chosen layouts folder: %s\n",
                       error ? [[error localizedDescription] UTF8String] : "unknown error");
             return;
         }
         start_accessing_layouts_dir(url);
         [[NSUserDefaults standardUserDefaults] setObject:bookmark forKey:@"layoutsDirBookmark"];
         [[NSUserDefaults standardUserDefaults] synchronize];

         synth_set_layouts_dir([[url path] UTF8String]);
         wake_glfw();
     }];
}

void prompt_choose_layouts_folder(void) {
    do_choose_layouts_folder();
}

@interface SynthMenuTarget : NSObject
- (void)scanDevices:(id)sender;
- (void)setDialModeRotary:(id)sender;
- (void)setDialModeVertical:(id)sender;
- (void)setDialModeHorizontal:(id)sender;
- (void)chooseLayoutsFolder:(id)sender;
- (void)backupCurrentPatch:(id)sender;
- (void)backupPatchByNumber:(id)sender;
- (BOOL)validateMenuItem:(NSMenuItem *)item;
@end

@implementation SynthMenuTarget

- (void)scanDevices:(id)sender {
    midi_scan_devices();
    wake_glfw();
}

- (void)backupCurrentPatch:(id)sender {
    synth_backup_current_patch();
}

- (void)backupPatchByNumber:(id)sender {
    // 128 locations — a base Voyager with no VX-... memory expansion (see
    // synth_request_single_preset_dump()'s own range check in synthComms.c).
    const uint32_t kPresetCount = 128;
    char           labelStorage[128][4]; // "1".."128", 3 digits + NUL
    const char *   labels[128];

    for (uint32_t i = 0; i < kPresetCount; i++) {
        snprintf(labelStorage[i], sizeof(labelStorage[i]), "%u", (unsigned)(i + 1));
        labels[i] = labelStorage[i];
    }

    int32_t        chosen       = show_device_choice_dialogue("Backup Patch",
                                                              "Choose a preset number to back up:", labels, kPresetCount);

    if (chosen >= 0) {
        synth_backup_patch_by_number((uint32_t)(chosen + 1));
    }
}

- (void)chooseLayoutsFolder:(id)sender {
    do_choose_layouts_folder();
}

- (void)setDialModeRotary:(id)sender {
    gDialMode = eDialModeRotary;
    [[NSUserDefaults standardUserDefaults] setInteger:gDialMode forKey:@"dialMode"];
}

- (void)setDialModeVertical:(id)sender {
    gDialMode = eDialModeVertical;
    [[NSUserDefaults standardUserDefaults] setInteger:gDialMode forKey:@"dialMode"];
}

- (void)setDialModeHorizontal:(id)sender {
    gDialMode = eDialModeHorizontal;
    [[NSUserDefaults standardUserDefaults] setInteger:gDialMode forKey:@"dialMode"];
}

- (BOOL)validateMenuItem:(NSMenuItem *)item {
    SEL action = [item action];

    if (action == @selector(setDialModeRotary:)) {
        [item setState:(gDialMode == eDialModeRotary) ? NSControlStateValueOn : NSControlStateValueOff];
    } else if (action == @selector(setDialModeVertical:)) {
        [item setState:(gDialMode == eDialModeVertical) ? NSControlStateValueOn : NSControlStateValueOff];
    } else if (action == @selector(setDialModeHorizontal:)) {
        [item setState:(gDialMode == eDialModeHorizontal) ? NSControlStateValueOn : NSControlStateValueOff];
    }
    return YES;
}

@end

void setup_main_menu(void) {
    NSMenu *                 menuBar  = [[NSApplication sharedApplication] mainMenu];
    static SynthMenuTarget * target   = nil;

    if (target == nil) {
        target = [[SynthMenuTarget alloc] init];
    }

    if (menuBar == nil) {
        menuBar = [[NSMenu alloc] init];
        [[NSApplication sharedApplication] setMainMenu:menuBar];
    }
    NSUserDefaults *         defaults = [NSUserDefaults standardUserDefaults];

    if ([defaults objectForKey:@"dialMode"] != nil) {
        gDialMode = (tDialMode)[defaults integerForKey:@"dialMode"];
    }

    if ([defaults objectForKey:@"windowWidth"] != nil) {
        int savedW = (int)[defaults integerForKey:@"windowWidth"];
        int savedH = savedW * TARGET_FRAME_BUFF_HEIGHT / TARGET_FRAME_BUFF_WIDTH;

        if (savedW > 0) {
            resize_window(savedW, savedH);
        }
    }

    if ([defaults objectForKey:@"windowX"] != nil && [defaults objectForKey:@"windowY"] != nil) {
        int savedX = (int)[defaults integerForKey:@"windowX"];
        int savedY = (int)[defaults integerForKey:@"windowY"];

        reposition_window(savedX, savedY);
    }
    NSMenuItem * devMI       = [[NSMenuItem alloc] init];
    NSMenu *     devMenu     = [[NSMenu alloc] initWithTitle:@"Device"];
    NSMenuItem * scanItem    = [[NSMenuItem alloc] initWithTitle:@"Scan Devices"
                                action:@selector(scanDevices:)
                                keyEquivalent:@"r"];
    [scanItem setTarget:target];
    [devMenu addItem:scanItem];
    [devMI setSubmenu:devMenu];
    [menuBar insertItem:devMI atIndex:1];

    NSMenuItem * ctrlMI      = [[NSMenuItem alloc] init];
    NSMenu *     ctrlMenu    = [[NSMenu alloc] initWithTitle:@"Controls"];

    NSMenuItem * rotaryItem  = [[NSMenuItem alloc] initWithTitle:@"Rotary"
                                action:@selector(setDialModeRotary:)
                                keyEquivalent:@""];
    [rotaryItem setTarget:target];
    [ctrlMenu addItem:rotaryItem];

    NSMenuItem * vertItem    = [[NSMenuItem alloc] initWithTitle:@"Vertical"
                                action:@selector(setDialModeVertical:)
                                keyEquivalent:@""];
    [vertItem setTarget:target];
    [ctrlMenu addItem:vertItem];

    NSMenuItem * horizItem   = [[NSMenuItem alloc] initWithTitle:@"Horizontal"
                                action:@selector(setDialModeHorizontal:)
                                keyEquivalent:@""];
    [horizItem setTarget:target];
    [ctrlMenu addItem:horizItem];

    [ctrlMI setSubmenu:ctrlMenu];
    [menuBar insertItem:ctrlMI atIndex:2];

    NSMenuItem * layoutsMI   = [[NSMenuItem alloc] init];
    NSMenu *     layoutsMenu = [[NSMenu alloc] initWithTitle:@"Layouts"];
    NSMenuItem * chooseItem  = [[NSMenuItem alloc] initWithTitle:@"Choose Layouts Folder…"
                                action:@selector(chooseLayoutsFolder:)
                                keyEquivalent:@""];
    [chooseItem setTarget:target];
    [layoutsMenu addItem:chooseItem];
    [layoutsMI setSubmenu:layoutsMenu];
    [menuBar insertItem:layoutsMI atIndex:3];

    // Single-patch only for now, no Restore yet — see synthBackup.h. Modeled
    // on G2-Edit's Backup menu (misc.mm there), scaled down to match what's
    // actually implemented.
    NSMenuItem * backupMI    = [[NSMenuItem alloc] init];
    NSMenu *     backupMenu  = [[NSMenu alloc] initWithTitle:@"Backup"];
    // Live edit buffer, not a stored preset — see synth_backup_current_patch()
    // vs. synth_backup_patch_by_number() in synthBackup.h.
    NSMenuItem * liveItem    = [[NSMenuItem alloc] initWithTitle:@"Current Panel (Edit Buffer)…"
                                action:@selector(backupCurrentPatch:)
                                keyEquivalent:@""];
    [liveItem setTarget:target];
    [backupMenu addItem:liveItem];
    NSMenuItem * numberItem  = [[NSMenuItem alloc] initWithTitle:@"Patch by Number…"
                                action:@selector(backupPatchByNumber:)
                                keyEquivalent:@""];
    [numberItem setTarget:target];
    [backupMenu addItem:numberItem];
    [backupMI setSubmenu:backupMenu];
    [menuBar insertItem:backupMI atIndex:4];
}

void save_window_size(int w) {
    NSUserDefaults * defaults = [NSUserDefaults standardUserDefaults];

    [defaults setInteger:w forKey:@"windowWidth"];
    [defaults synchronize];
}

void save_window_pos(int x, int y) {
    NSUserDefaults * defaults = [NSUserDefaults standardUserDefaults];

    [defaults setInteger:x forKey:@"windowX"];
    [defaults setInteger:y forKey:@"windowY"];
    [defaults synchronize];
}

void register_sleep_wake_notifications(void) {
    [[[NSWorkspace sharedWorkspace] notificationCenter]
     addObserverForName:NSWorkspaceDidWakeNotification
     object:nil
     queue:nil
     usingBlock:^(NSNotification * note) {
         midi_scan_devices();
     }];
}
