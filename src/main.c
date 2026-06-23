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

#ifdef __cplusplus
extern "C" {
#endif

#include <signal.h>

#include "defs.h"
#include "globalVars.h"
#include "graphics.h"
#include "midiComms.h"
#include "misc.h"
#include "main.h"

static void signal_handler(int sigraised) {
    LOG_DEBUG("\nSig Handler!!! %d\n", sigraised);
    atomic_store(&gQuitAll, true);
    _exit(0);
}

static void init_signals(void) {
    signal(SIGINT, signal_handler);
    signal(SIGBUS, signal_handler);
    signal(SIGSEGV, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGABRT, signal_handler);
}

int main(int argc, char ** argv) {
    (void)argc;
    (void)argv;

    init_signals();
    register_sleep_wake_notifications();

    init_graphics();
    start_midi_thread();

    do_graphics_loop();

    clean_up_graphics();

    exit(EXIT_SUCCESS);
}

#ifdef __cplusplus
}
#endif
