/*
 * The Z1-Edit application.
 *
 * Copyright (C) 2025 Chris Turner <chris_purusha@icloud.com>
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
#include "globalVars.h"
#include "graphics.h"
#include "midiComms.h"

@interface Z1MenuTarget : NSObject
- (void)scanDevices:(id)sender;
@end

@implementation Z1MenuTarget

- (void)scanDevices:(id)sender {
    midi_scan_devices();
    wake_glfw();
}

@end

void setup_main_menu(void) {
    NSMenu *              menuBar  = [[NSApplication sharedApplication] mainMenu];
    static Z1MenuTarget * target   = nil;

    if (target == nil) {
        target = [[Z1MenuTarget alloc] init];
    }
    if (menuBar == nil) {
        menuBar = [[NSMenu alloc] init];
        [[NSApplication sharedApplication] setMainMenu:menuBar];
    }
    NSMenuItem * devMI    = [[NSMenuItem alloc] init];
    NSMenu *     devMenu  = [[NSMenu alloc] initWithTitle:@"Device"];
    NSMenuItem * scanItem = [[NSMenuItem alloc] initWithTitle:@"Scan Devices"
                              action:@selector(scanDevices:)
                              keyEquivalent:@"r"];
    [scanItem setTarget:target];
    [devMenu addItem:scanItem];
    [devMI setSubmenu:devMenu];
    [menuBar insertItem:devMI atIndex:1];
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
