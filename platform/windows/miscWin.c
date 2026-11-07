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

// Windows version of src/misc.mm (Docs/windows-port.md).

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>

#include "misc.h"
#include "midiComms.h"

// With its trailing separator, as NSTemporaryDirectory() has one on the Mac
const char * synth_temp_dir(void) {
    static char buf[MAX_PATH + 1];

    if (GetTempPathA(sizeof(buf), buf) == 0) {
        snprintf(buf, sizeof(buf), ".\\");
    }
    return buf;
}

// No native menu bar to make (the application draws its own)
void setup_main_menu(void) {
    load_saved_settings();
}

// TODO: WM_POWERBROADCAST / PBT_APMRESUMEAUTOMATIC -> midi_request_reconnect()
void register_sleep_wake_notifications(void) {
}
