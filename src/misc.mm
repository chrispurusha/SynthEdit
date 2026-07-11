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
- (void)backupBank:(id)sender;
- (void)backupBankToFolder:(id)sender;
- (void)restorePanel:(id)sender;
- (void)restorePatch:(id)sender;
- (void)restoreBank:(id)sender;
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

- (void)backupBank:(id)sender {
    synth_backup_bank();
}

- (void)restorePanel:(id)sender {
    synth_backup_restore_panel();
}

- (void)restorePatch:(id)sender {
    synth_backup_restore_patch();
}

- (void)restoreBank:(id)sender {
    synth_backup_restore_bank();
}

- (void)backupBankToFolder:(id)sender {
    synth_backup_bank_to_folder();
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
    // File menu — Open/Save for the live edit buffer, same top-level
    // position and Open/Save naming G2-Edit's own File menu uses
    // (misc.mm there: "Open Patch/Perf File…" ⌘O, "Save Patch to File…"
    // ⌘S) rather than burying these two under Backup/Restore alongside the
    // stored-preset/bank actions — moved here 2026-07-11 at the owner's
    // request, having started out in Backup/Restore (that's still where
    // synth_backup_current_patch()/synth_backup_restore_panel() are
    // declared/documented, synthBackup.h — this menu just points at them
    // from a different, more familiar location).
    NSMenuItem * fileMI           = [[NSMenuItem alloc] init];
    NSMenu *     fileMenu         = [[NSMenu alloc] initWithTitle:@"File"];
    NSMenuItem * openPanelItem    = [[NSMenuItem alloc] initWithTitle:@"Open Panel File…"
                                     action:@selector(restorePanel:)
                                     keyEquivalent:@"o"];
    [openPanelItem setTarget:target];
    [fileMenu addItem:openPanelItem];
    NSMenuItem * savePanelItem    = [[NSMenuItem alloc] initWithTitle:@"Save Panel to File…"
                                     action:@selector(backupCurrentPatch:)
                                     keyEquivalent:@"s"];
    [savePanelItem setTarget:target];
    [fileMenu addItem:savePanelItem];
    [fileMI setSubmenu:fileMenu];
    [menuBar insertItem:fileMI atIndex:1];

    NSMenuItem * devMI            = [[NSMenuItem alloc] init];
    NSMenu *     devMenu          = [[NSMenu alloc] initWithTitle:@"Device"];
    NSMenuItem * scanItem         = [[NSMenuItem alloc] initWithTitle:@"Scan Devices"
                                     action:@selector(scanDevices:)
                                     keyEquivalent:@"r"];
    [scanItem setTarget:target];
    [devMenu addItem:scanItem];
    [devMI setSubmenu:devMenu];
    [menuBar insertItem:devMI atIndex:2];

    NSMenuItem * ctrlMI           = [[NSMenuItem alloc] init];
    NSMenu *     ctrlMenu         = [[NSMenu alloc] initWithTitle:@"Controls"];

    NSMenuItem * rotaryItem       = [[NSMenuItem alloc] initWithTitle:@"Rotary"
                                     action:@selector(setDialModeRotary:)
                                     keyEquivalent:@""];
    [rotaryItem setTarget:target];
    [ctrlMenu addItem:rotaryItem];

    NSMenuItem * vertItem         = [[NSMenuItem alloc] initWithTitle:@"Vertical"
                                     action:@selector(setDialModeVertical:)
                                     keyEquivalent:@""];
    [vertItem setTarget:target];
    [ctrlMenu addItem:vertItem];

    NSMenuItem * horizItem        = [[NSMenuItem alloc] initWithTitle:@"Horizontal"
                                     action:@selector(setDialModeHorizontal:)
                                     keyEquivalent:@""];
    [horizItem setTarget:target];
    [ctrlMenu addItem:horizItem];

    [ctrlMI setSubmenu:ctrlMenu];
    [menuBar insertItem:ctrlMI atIndex:3];

    NSMenuItem * layoutsMI        = [[NSMenuItem alloc] init];
    NSMenu *     layoutsMenu      = [[NSMenu alloc] initWithTitle:@"Layouts"];
    NSMenuItem * chooseItem       = [[NSMenuItem alloc] initWithTitle:@"Choose Layouts Folder…"
                                     action:@selector(chooseLayoutsFolder:)
                                     keyEquivalent:@""];
    [chooseItem setTarget:target];
    [layoutsMenu addItem:chooseItem];
    [layoutsMI setSubmenu:layoutsMenu];
    [menuBar insertItem:layoutsMI atIndex:4];

    // Modeled on G2-Edit's Backup menu (misc.mm there), scaled down to
    // match what's actually implemented. Restore (below, its own top-level
    // menu) added 2026-07-11 once sending a captured dump back was
    // confirmed on real hardware (see [[project_voyager_restore_mechanism]]
    // in the assistant's own memory notes) — G2-Edit has its own separate
    // Backup/Restore top-level menu pair too, followed here. Current Panel
    // (Edit Buffer) moved out to the File menu above, same day — it isn't
    // a stored-preset/bank action like the rest of this menu.
    NSMenuItem * backupMI         = [[NSMenuItem alloc] init];
    NSMenu *     backupMenu       = [[NSMenu alloc] initWithTitle:@"Backup"];
    NSMenuItem * numberItem       = [[NSMenuItem alloc] initWithTitle:@"Patch by Number…"
                                     action:@selector(backupPatchByNumber:)
                                     keyEquivalent:@""];
    [numberItem setTarget:target];
    [backupMenu addItem:numberItem];
    // Whatever bank the connected unit's front panel currently has selected
    // — see synth_backup_bank()'s own comment (synthBackup.h) for why this
    // can't yet target a specific bank on an expanded (more-than-128-preset)
    // unit.
    NSMenuItem * bankItem         = [[NSMenuItem alloc] initWithTitle:@"Bank…"
                                     action:@selector(backupBank:)
                                     keyEquivalent:@""];
    [bankItem setTarget:target];
    [backupMenu addItem:bankItem];
    // Requests every preset one at a time and saves each as its own file —
    // see synth_backup_bank_to_folder()'s own comment (synthBackup.h) for
    // why this needs a separate action from "Bank…" above (that one saves
    // the whole bank as a single opaque blob).
    NSMenuItem * bankFolderItem   = [[NSMenuItem alloc] initWithTitle:@"Bank (Individual Files)…"
                                     action:@selector(backupBankToFolder:)
                                     keyEquivalent:@""];
    [bankFolderItem setTarget:target];
    [backupMenu addItem:bankFolderItem];

    [backupMI setSubmenu:backupMenu];
    [menuBar insertItem:backupMI atIndex:5];

    // Restore — its own top-level menu, not nested inside Backup, matching
    // G2-Edit's own Backup/Restore split (misc.mm there) rather than
    // burying destructive/overwriting actions inside the read-only Backup
    // menu. Patch/Bank both confirm before sending, since they overwrite
    // stored memory with no undo — see synth_backup_restore_patch()/_bank()'s
    // own comments (synthBackup.h) for exactly what each confirms. Panel
    // (Edit Buffer) moved out to the File menu above, same day as Backup's
    // own Current Panel — it isn't a stored-preset/bank action like the
    // rest of this menu, and has no overwrite risk to confirm in the first
    // place (loads the live edit buffer only).
    NSMenuItem * restoreMI        = [[NSMenuItem alloc] init];
    NSMenu *     restoreMenu      = [[NSMenu alloc] initWithTitle:@"Restore"];
    NSMenuItem * restorePatchItem = [[NSMenuItem alloc] initWithTitle:@"Patch by Number…"
                                     action:@selector(restorePatch:)
                                     keyEquivalent:@""];
    [restorePatchItem setTarget:target];
    [restoreMenu addItem:restorePatchItem];
    NSMenuItem * restoreBankItem  = [[NSMenuItem alloc] initWithTitle:@"Bank…"
                                     action:@selector(restoreBank:)
                                     keyEquivalent:@""];
    [restoreBankItem setTarget:target];
    [restoreMenu addItem:restoreBankItem];
    [restoreMI setSubmenu:restoreMenu];
    [menuBar insertItem:restoreMI atIndex:6];
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
