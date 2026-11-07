/*
 * SynthLib - common library for synthesizer editor applications.
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
// Notes: Docs/code-notes/synthlibMidiPortsWin.c.md - "// notes §k" refers there.

// Windows version of SynthLib/src/synthlibMidiPorts.c, on winmm. The port CHOICE is the Mac's code
// unchanged (prefs only); the ports PRESENT come from midiIn/midiOutGetDevCaps.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>

#include <stdio.h>
#include <string.h>
#include <pthread.h>

#include "synthlibMidiPorts.h"
#include "coreMidiWin.h"
#include "prefs.h"

static pthread_mutex_t gChoiceMutex   = PTHREAD_MUTEX_INITIALIZER;
static char            gChoiceScope[SYNTHLIB_MIDI_PORT_NAME_MAX];
static char            gChoiceInput[SYNTHLIB_MIDI_PORT_NAME_MAX];
static char            gChoiceOutput[SYNTHLIB_MIDI_PORT_NAME_MAX];
static uint32_t        gChoiceChannel = SYNTHLIB_MIDI_CHANNEL_AUTOMATIC;

static uint32_t valid_channel(long channel) {
    return ((channel >= 1) && (channel <= 16)) ? (uint32_t)channel : SYNTHLIB_MIDI_CHANNEL_AUTOMATIC;
}

static void choice_key(char * out, size_t size, const char * base, const char * scope) {
    if (scope[0] == '\0') {
        snprintf(out, size, "%s", base);
    } else {
        snprintf(out, size, "%s.%s", base, scope);
    }
}

void synthlib_midi_ports_set_scope(const char * scope) {
    char         scopeCopy[SYNTHLIB_MIDI_PORT_NAME_MAX]       = {0};
    char         keyIn[SYNTHLIB_MIDI_PORT_NAME_MAX + 16]      = {0};
    char         keyOut[SYNTHLIB_MIDI_PORT_NAME_MAX + 16]     = {0};
    const char * input                                        = NULL;
    const char * output                                       = NULL;
    char         keyChannel[SYNTHLIB_MIDI_PORT_NAME_MAX + 16] = {0};
    uint32_t     channel                                      = SYNTHLIB_MIDI_CHANNEL_AUTOMATIC;

    snprintf(scopeCopy, sizeof(scopeCopy), "%s", (scope != NULL) ? scope : "");
    choice_key(keyIn, sizeof(keyIn), "midiInput", scopeCopy);
    choice_key(keyOut, sizeof(keyOut), "midiOutput", scopeCopy);
    choice_key(keyChannel, sizeof(keyChannel), "midiChannel", scopeCopy);
    channel        = valid_channel(prefs_get_int(keyChannel, SYNTHLIB_MIDI_CHANNEL_AUTOMATIC));
    input          = prefs_get_string(keyIn, "");
    output         = prefs_get_string(keyOut, "");

    pthread_mutex_lock(&gChoiceMutex);
    snprintf(gChoiceScope, sizeof(gChoiceScope), "%s", scopeCopy);
    snprintf(gChoiceInput, sizeof(gChoiceInput), "%s", (input != NULL) ? input : "");
    snprintf(gChoiceOutput, sizeof(gChoiceOutput), "%s", (output != NULL) ? output : "");
    gChoiceChannel = channel;
    pthread_mutex_unlock(&gChoiceMutex);
}

void synthlib_midi_ports_choose(const char * input, const char * output) {
    char scope[SYNTHLIB_MIDI_PORT_NAME_MAX]       = {0};
    char keyIn[SYNTHLIB_MIDI_PORT_NAME_MAX + 16]  = {0};
    char keyOut[SYNTHLIB_MIDI_PORT_NAME_MAX + 16] = {0};

    pthread_mutex_lock(&gChoiceMutex);
    snprintf(gChoiceInput, sizeof(gChoiceInput), "%s", (input != NULL) ? input : "");
    snprintf(gChoiceOutput, sizeof(gChoiceOutput), "%s", (output != NULL) ? output : "");
    snprintf(scope, sizeof(scope), "%s", gChoiceScope);
    pthread_mutex_unlock(&gChoiceMutex);

    choice_key(keyIn, sizeof(keyIn), "midiInput", scope);
    choice_key(keyOut, sizeof(keyOut), "midiOutput", scope);
    prefs_set_string(keyIn, (input != NULL) ? input : "");
    prefs_set_string(keyOut, (output != NULL) ? output : "");
}

void synthlib_midi_ports_chosen(char * input, size_t inputSize, char * output, size_t outputSize) {
    pthread_mutex_lock(&gChoiceMutex);

    if ((input != NULL) && (inputSize > 0)) {
        snprintf(input, inputSize, "%s", gChoiceInput);
    }

    if ((output != NULL) && (outputSize > 0)) {
        snprintf(output, outputSize, "%s", gChoiceOutput);
    }
    pthread_mutex_unlock(&gChoiceMutex);
}

void synthlib_midi_channel_choose(uint32_t channel) {
    char     scope[SYNTHLIB_MIDI_PORT_NAME_MAX]           = {0};
    char     keyChannel[SYNTHLIB_MIDI_PORT_NAME_MAX + 16] = {0};
    uint32_t valid                                        = valid_channel((long)channel);

    pthread_mutex_lock(&gChoiceMutex);
    gChoiceChannel = valid;
    snprintf(scope, sizeof(scope), "%s", gChoiceScope);
    pthread_mutex_unlock(&gChoiceMutex);

    choice_key(keyChannel, sizeof(keyChannel), "midiChannel", scope);
    prefs_set_int(keyChannel, (long)valid);
}

uint32_t synthlib_midi_channel_chosen(void) {
    uint32_t channel = SYNTHLIB_MIDI_CHANNEL_AUTOMATIC;

    pthread_mutex_lock(&gChoiceMutex);
    channel = gChoiceChannel;
    pthread_mutex_unlock(&gChoiceMutex);
    return channel;
}

// ── The ports present: a registry of every port seen, so a handle outlives winmm's renumbering ──

// notes §1
#define WIN_PORT_MAX      64
#define WIN_INPUT_BIT     0x40000000u
#define WIN_OUTPUT_BIT    0x80000000u

typedef struct {
    char     name[SYNTHLIB_MIDI_PORT_NAME_MAX];
    uint32_t occurrence;   // 0 for the first port of this name, 1 for the second...
} tWinPort;

static pthread_mutex_t gPortMutex = PTHREAD_MUTEX_INITIALIZER;
static tWinPort        gPorts[2][WIN_PORT_MAX];
static uint32_t        gPortCount[2];

static void device_name(bool input, uint32_t index, char * out, size_t size) {
    WCHAR wide[MAXPNAMELEN] = {0};
    out[0] = '\0';

    if (input) {
        MIDIINCAPSW caps = {0};

        if (midiInGetDevCapsW(index, &caps, sizeof(caps)) != MMSYSERR_NOERROR) {
            return;
        }
        memcpy(wide, caps.szPname, sizeof(wide));
    } else {
        MIDIOUTCAPSW caps = {0};

        if (midiOutGetDevCapsW(index, &caps, sizeof(caps)) != MMSYSERR_NOERROR) {
            return;
        }
        memcpy(wide, caps.szPname, sizeof(wide));
    }
    wide[MAXPNAMELEN - 1] = 0;

    if (WideCharToMultiByte(CP_UTF8, 0, wide, -1, out, (int)size, NULL, NULL) == 0) {
        out[0] = '\0';
    }
}

uint32_t synthlib_midi_win_count(bool inputs) {
    return inputs ? midiInGetNumDevs() : midiOutGetNumDevs();
}

// The name of device `index`, and how many devices before it carry the same name
static void device_identity(bool input, uint32_t index, char * name, size_t size, uint32_t * occurrence) {
    char other[SYNTHLIB_MIDI_PORT_NAME_MAX] = {0};

    device_name(input, index, name, size);
    *occurrence = 0;

    for (uint32_t i = 0; i < index; i++) {
        device_name(input, i, other, sizeof(other));

        if (strcmp(other, name) == 0) {
            (*occurrence)++;
        }
    }
}

MIDIEndpointRef synthlib_midi_win_endpoint(bool inputs, uint32_t index) {
    char            name[SYNTHLIB_MIDI_PORT_NAME_MAX] = {0};
    uint32_t        occurrence                        = 0;
    uint32_t        dir                               = inputs ? 0u : 1u;
    MIDIEndpointRef endpoint                          = 0;

    if (index >= synthlib_midi_win_count(inputs)) {
        return 0;
    }
    device_identity(inputs, index, name, sizeof(name), &occurrence);

    if (name[0] == '\0') {
        return 0;
    }
    pthread_mutex_lock(&gPortMutex);

    for (uint32_t slot = 0; slot < gPortCount[dir]; slot++) {
        if ((gPorts[dir][slot].occurrence == occurrence) && (strcmp(gPorts[dir][slot].name, name) == 0)) {
            endpoint = slot + 1u;
            break;
        }
    }

    if ((endpoint == 0) && (gPortCount[dir] < WIN_PORT_MAX)) {
        tWinPort * port = &gPorts[dir][gPortCount[dir]];
        snprintf(port->name, sizeof(port->name), "%s", name);
        port->occurrence = occurrence;
        endpoint         = ++gPortCount[dir];
    }
    pthread_mutex_unlock(&gPortMutex);

    return (endpoint == 0) ? 0 : (endpoint | (inputs ? WIN_INPUT_BIT : WIN_OUTPUT_BIT));
}

static bool registered(MIDIEndpointRef endpoint, bool * input, tWinPort * port) {
    uint32_t slot = (endpoint & 0xFFFFu);
    bool     ok   = false;

    *input = (endpoint & WIN_INPUT_BIT) != 0;

    if ((slot == 0) || ((endpoint & (WIN_INPUT_BIT | WIN_OUTPUT_BIT)) == 0)) {
        return false;
    }
    pthread_mutex_lock(&gPortMutex);

    if (slot <= gPortCount[*input ? 0 : 1]) {
        *port = gPorts[*input ? 0 : 1][slot - 1u];
        ok    = true;
    }
    pthread_mutex_unlock(&gPortMutex);
    return ok;
}

bool synthlib_midi_win_index(MIDIEndpointRef endpoint, bool * input, uint32_t * index) {
    tWinPort port  = {0};
    uint32_t count = 0;

    if (!registered(endpoint, input, &port)) {
        return false;
    }
    count = synthlib_midi_win_count(*input);

    for (uint32_t i = 0; i < count; i++) {
        char     name[SYNTHLIB_MIDI_PORT_NAME_MAX] = {0};
        uint32_t occurrence                        = 0;

        device_identity(*input, i, name, sizeof(name), &occurrence);

        if ((occurrence == port.occurrence) && (strcmp(name, port.name) == 0)) {
            *index = i;
            return true;
        }
    }

    return false;
}

// notes §2
bool synthlib_midi_win_ports_changed(void) {
    static char     seen[2 * WIN_PORT_MAX * 8];
    static bool     first = true;
    char            now[sizeof(seen)] = {0};
    size_t          used  = 0;

    for (uint32_t dir = 0; dir < 2; dir++) {
        uint32_t count = synthlib_midi_win_count(dir == 0);

        for (uint32_t i = 0; (i < count) && (used + SYNTHLIB_MIDI_PORT_NAME_MAX + 2 < sizeof(now)); i++) {
            device_name(dir == 0, i, now + used, SYNTHLIB_MIDI_PORT_NAME_MAX);
            used        += strlen(now + used);
            now[used++]  = '\n';
        }
        now[used++] = '|';
    }

    bool changed = !first && (memcmp(now, seen, sizeof(seen)) != 0);

    memcpy(seen, now, sizeof(seen));
    first = false;
    return changed;
}

void synthlib_midi_port_name(MIDIEndpointRef endpoint, char * out, size_t size) {
    tWinPort port  = {0};
    bool     input = false;

    if ((out == NULL) || (size == 0)) {
        return;
    }
    out[0] = '\0';

    if (registered(endpoint, &input, &port)) {
        snprintf(out, size, "%s", port.name);
    }
}

uint32_t synthlib_midi_port_names(bool inputs, char names[][SYNTHLIB_MIDI_PORT_NAME_MAX], uint32_t max) {
    uint32_t count   = synthlib_midi_win_count(inputs);
    uint32_t written = 0;

    for (uint32_t i = 0; (i < count) && (written < max); i++) {
        synthlib_midi_port_name(synthlib_midi_win_endpoint(inputs, i), names[written], SYNTHLIB_MIDI_PORT_NAME_MAX);

        if (names[written][0] != '\0') {
            written++;
        }
    }

    return written;
}

MIDIEndpointRef synthlib_midi_find_port(bool input, const char * name) {
    uint32_t count = synthlib_midi_win_count(input);

    if ((name == NULL) || (name[0] == '\0')) {
        return 0;
    }

    for (uint32_t i = 0; i < count; i++) {
        MIDIEndpointRef endpoint                          = synthlib_midi_win_endpoint(input, i);
        char            here[SYNTHLIB_MIDI_PORT_NAME_MAX] = {0};

        synthlib_midi_port_name(endpoint, here, sizeof(here));

        if (strcmp(here, name) == 0) {
            return endpoint;
        }
    }

    return 0;
}
