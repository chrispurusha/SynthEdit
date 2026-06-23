/*
 * The Z1-Edit application.
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
#include "globalVars.h"
#include "graphics.h"
#include "midiComms.h"

@interface Z1MenuTarget : NSObject
- (void)scanDevices:(id)sender;
- (void)setDialModeRotary:(id)sender;
- (void)setDialModeVertical:(id)sender;
- (void)setDialModeHorizontal:(id)sender;
- (BOOL)validateMenuItem:(NSMenuItem *)item;
@end

@implementation Z1MenuTarget

- (void)scanDevices:(id)sender {
    midi_scan_devices();
    wake_glfw();
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
    NSMenu *              menuBar    = [[NSApplication sharedApplication] mainMenu];
    static Z1MenuTarget * target     = nil;

    if (target == nil) {
        target = [[Z1MenuTarget alloc] init];
    }

    if (menuBar == nil) {
        menuBar = [[NSMenu alloc] init];
        [[NSApplication sharedApplication] setMainMenu:menuBar];
    }
    NSUserDefaults *      defaults   = [NSUserDefaults standardUserDefaults];

    if ([defaults objectForKey:@"dialMode"] != nil) {
        gDialMode = (tDialMode)[defaults integerForKey:@"dialMode"];
    }
    NSMenuItem *          devMI      = [[NSMenuItem alloc] init];
    NSMenu *              devMenu    = [[NSMenu alloc] initWithTitle:@"Device"];
    NSMenuItem *          scanItem   = [[NSMenuItem alloc] initWithTitle:@"Scan Devices"
                                        action:@selector(scanDevices:)
                                        keyEquivalent:@"r"];
    [scanItem setTarget:target];
    [devMenu addItem:scanItem];
    [devMI setSubmenu:devMenu];
    [menuBar insertItem:devMI atIndex:1];

    NSMenuItem *          ctrlMI     = [[NSMenuItem alloc] init];
    NSMenu *              ctrlMenu   = [[NSMenu alloc] initWithTitle:@"Controls"];

    NSMenuItem *          rotaryItem = [[NSMenuItem alloc] initWithTitle:@"Rotary"
                                        action:@selector(setDialModeRotary:)
                                        keyEquivalent:@""];
    [rotaryItem setTarget:target];
    [ctrlMenu addItem:rotaryItem];

    NSMenuItem *          vertItem   = [[NSMenuItem alloc] initWithTitle:@"Vertical"
                                        action:@selector(setDialModeVertical:)
                                        keyEquivalent:@""];
    [vertItem setTarget:target];
    [ctrlMenu addItem:vertItem];

    NSMenuItem *          horizItem  = [[NSMenuItem alloc] initWithTitle:@"Horizontal"
                                        action:@selector(setDialModeHorizontal:)
                                        keyEquivalent:@""];
    [horizItem setTarget:target];
    [ctrlMenu addItem:horizItem];

    [ctrlMI setSubmenu:ctrlMenu];
    [menuBar insertItem:ctrlMI atIndex:2];
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
