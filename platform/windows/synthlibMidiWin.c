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
// Notes: Docs/code-notes/synthlibMidiWin.c.md - "// notes §k" refers there.

// Windows version of SynthLib/src/synthlibMidi.c, on winmm: the send primitive, and the inputs that
// CoreMIDI's input port stands for on the Mac.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>

#include "synthlibMidi.h"
#include "synthlibDefs.h"
#include "coreMidiWin.h"

#define WIN_PORT_SLOTS    64

static pthread_mutex_t gSendMutex = PTHREAD_MUTEX_INITIALIZER;

// ── Output ───────────────────────────────────────────────────────────────────────────────────────

// notes §1
static HMIDIOUT        gOut[WIN_PORT_SLOTS];

void synthlib_midi_set_out_port(MIDIPortRef port) {
    (void)port;    // winmm has no output port; each destination is opened on first use
}

static HMIDIOUT out_handle(MIDIEndpointRef dest) {
    uint32_t slot  = (dest & 0xFFFFu);
    bool     input = false;
    uint32_t index = 0;

    if ((slot == 0) || (slot > WIN_PORT_SLOTS) || !synthlib_midi_win_index(dest, &input, &index) || input) {
        return NULL;
    }

    if (gOut[slot - 1] == NULL) {
        MMRESULT r = midiOutOpen(&gOut[slot - 1], index, 0, 0, CALLBACK_NULL);

        if (r != MMSYSERR_NOERROR) {
            // MMSYSERR_ALLOCATED: another application holds this port (most drivers are single-client)
            LOG_ERROR("midiOutOpen(%u) failed: %u\n", (unsigned)index, (unsigned)r);
            gOut[slot - 1] = NULL;
        }
    }
    return gOut[slot - 1];
}

static void close_out(MIDIEndpointRef dest) {
    uint32_t slot = (dest & 0xFFFFu);

    if ((slot > 0) && (slot <= WIN_PORT_SLOTS) && (gOut[slot - 1] != NULL)) {
        midiOutClose(gOut[slot - 1]);
        gOut[slot - 1] = NULL;
    }
}

static bool send_long(HMIDIOUT h, const uint8_t * data, uint32_t length) {
    MIDIHDR  hdr = {0};
    MMRESULT r   = MMSYSERR_NOERROR;

    hdr.lpData         = (LPSTR)data;
    hdr.dwBufferLength = length;
    hdr.dwBytesRecorded = length;

    if ((r = midiOutPrepareHeader(h, &hdr, sizeof(hdr))) != MMSYSERR_NOERROR) {
        LOG_ERROR("midiOutPrepareHeader failed: %u\n", (unsigned)r);
        return false;
    }

    if ((r = midiOutLongMsg(h, &hdr, sizeof(hdr))) == MMSYSERR_NOERROR) {
        // notes §2
        for (int ms = 0; ((hdr.dwFlags & MHDR_DONE) == 0) && (ms < 10000); ms++) {
            Sleep(1);
        }
    } else {
        LOG_ERROR("midiOutLongMsg failed: %u\n", (unsigned)r);
    }

    while (midiOutUnprepareHeader(h, &hdr, sizeof(hdr)) == MIDIERR_STILLPLAYING) {
        Sleep(1);
    }
    return r == MMSYSERR_NOERROR;
}

// The data bytes a channel or system common status takes
static uint32_t data_bytes(uint8_t status) {
    switch (status & 0xF0) {
    case 0xC0: case 0xD0: return 1;
    case 0xF0:
        return (status == 0xF1 || status == 0xF3) ? 1 : (status == 0xF2) ? 2 : 0;
    default: return 2;
    }
}

bool synthlib_midi_send_to(const uint8_t * data, uint32_t length, MIDIEndpointRef dest) {
    if ((dest == 0) || (data == NULL) || (length == 0)) {
        return false;
    }

    if (length > SYNTHLIB_MIDI_MAX_MESSAGE) {
        LOG_ERROR("MIDI message of %u bytes exceeds the %u byte maximum, not sent\n",
                  (unsigned)length, (unsigned)SYNTHLIB_MIDI_MAX_MESSAGE);
        return false;
    }
    pthread_mutex_lock(&gSendMutex);

    HMIDIOUT h  = out_handle(dest);
    bool     ok = (h != NULL);

    // notes §3
    for (uint32_t i = 0; ok && (i < length);) {
        if (data[i] == 0xF0) {
            uint32_t end = i;

            while ((end < length) && (data[end] != 0xF7)) {
                end++;
            }
            end = (end < length) ? end + 1 : length;
            ok  = send_long(h, data + i, end - i);
            i   = end;
        } else if (data[i] >= 0x80) {
            uint32_t n   = (data[i] >= 0xF8) ? 0 : data_bytes(data[i]);
            DWORD    msg = data[i];

            for (uint32_t b = 1; (b <= n) && (i + b < length); b++) {
                msg |= (DWORD)data[i + b] << (8 * b);
            }
            MMRESULT r = midiOutShortMsg(h, msg);

            if (r != MMSYSERR_NOERROR) {
                LOG_ERROR("midiOutShortMsg failed: %u\n", (unsigned)r);
                ok = false;
            }
            i += 1 + n;
        } else {
            i++;    // a stray data byte: no running status is sent from here
        }
    }

    if (!ok && (h != NULL)) {
        close_out(dest);    // unplugged, most likely: opened afresh on the next send
    }
    pthread_mutex_unlock(&gSendMutex);

    return ok;
}

// ── Input ────────────────────────────────────────────────────────────────────────────────────────

// notes §4
#define WIN_SYSEX_BUFFERS    8
#define WIN_SYSEX_SIZE       16384

typedef struct {
    HMIDIIN         handle;
    MIDIEndpointRef endpoint;
    MIDIHDR         hdr[WIN_SYSEX_BUFFERS];
    char            buf[WIN_SYSEX_BUFFERS][WIN_SYSEX_SIZE];
    volatile bool   closing;
} tWinInput;

static tWinInput *             gIn[WIN_PORT_SLOTS];
static tSynthLibMidiWinReceive gReceive;
static CRITICAL_SECTION        gReceiveLock;    // notes §5
static INIT_ONCE               gReceiveLockOnce = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK init_receive_lock(PINIT_ONCE once, PVOID param, PVOID * context) {
    (void)once;
    (void)param;
    (void)context;
    InitializeCriticalSection(&gReceiveLock);
    return TRUE;
}

static void deliver(MIDIEndpointRef src, const uint8_t * data, uint32_t length) {
    EnterCriticalSection(&gReceiveLock);

    if (gReceive != NULL) {
        gReceive(src, data, length);
    }
    LeaveCriticalSection(&gReceiveLock);
}

static void CALLBACK input_cb(HMIDIIN h, UINT msg, DWORD_PTR instance, DWORD_PTR p1, DWORD_PTR p2) {
    tWinInput * in = (tWinInput *)instance;
    (void)p2;

    if (msg == MIM_DATA) {
        uint8_t  bytes[3] = {(uint8_t)(p1 & 0xFF), (uint8_t)((p1 >> 8) & 0xFF), (uint8_t)((p1 >> 16) & 0xFF)};
        uint32_t n        = (bytes[0] >= 0xF8) ? 0 : data_bytes(bytes[0]);

        deliver(in->endpoint, bytes, 1 + n);
    } else if (msg == MIM_LONGDATA) {
        MIDIHDR * hdr = (MIDIHDR *)p1;

        if (hdr->dwBytesRecorded > 0) {
            deliver(in->endpoint, (const uint8_t *)hdr->lpData, hdr->dwBytesRecorded);
        }

        if (!in->closing) {
            midiInAddBuffer(h, hdr, sizeof(*hdr));    // notes §6
        }
    }
}

static void close_input(uint32_t slot) {
    tWinInput * in = gIn[slot];

    if (in == NULL) {
        return;
    }
    in->closing = true;
    midiInReset(in->handle);    // returns every buffer, marked done

    for (int b = 0; b < WIN_SYSEX_BUFFERS; b++) {
        midiInUnprepareHeader(in->handle, &in->hdr[b], sizeof(in->hdr[b]));
    }
    midiInClose(in->handle);
    free(in);
    gIn[slot] = NULL;
}

static void open_input(uint32_t index, MIDIEndpointRef endpoint) {
    uint32_t    slot = (endpoint & 0xFFFFu) - 1u;
    tWinInput * in   = calloc(1, sizeof(tWinInput));

    if (in == NULL) {
        return;
    }
    in->endpoint = endpoint;
    MMRESULT r = midiInOpen(&in->handle, index, (DWORD_PTR)input_cb, (DWORD_PTR)in, CALLBACK_FUNCTION);

    if (r != MMSYSERR_NOERROR) {
        LOG_ERROR("midiInOpen(%u) failed: %u\n", (unsigned)index, (unsigned)r);
        free(in);
        return;
    }

    for (int b = 0; b < WIN_SYSEX_BUFFERS; b++) {
        in->hdr[b].lpData         = in->buf[b];
        in->hdr[b].dwBufferLength = WIN_SYSEX_SIZE;
        midiInPrepareHeader(in->handle, &in->hdr[b], sizeof(in->hdr[b]));
        midiInAddBuffer(in->handle, &in->hdr[b], sizeof(in->hdr[b]));
    }
    midiInStart(in->handle);
    gIn[slot] = in;
}

void synthlib_midi_win_open_inputs(tSynthLibMidiWinReceive receive) {
    uint32_t count = synthlib_midi_win_count(true);
    bool     present[WIN_PORT_SLOTS] = {0};

    InitOnceExecuteOnce(&gReceiveLockOnce, init_receive_lock, NULL, NULL);
    EnterCriticalSection(&gReceiveLock);
    gReceive = receive;
    LeaveCriticalSection(&gReceiveLock);

    for (uint32_t i = 0; i < count; i++) {
        MIDIEndpointRef endpoint = synthlib_midi_win_endpoint(true, i);
        uint32_t        slot     = (endpoint & 0xFFFFu);

        if ((slot == 0) || (slot > WIN_PORT_SLOTS)) {
            continue;
        }
        present[slot - 1] = true;

        if (gIn[slot - 1] == NULL) {
            open_input(i, endpoint);
        }
    }

    // notes §7
    for (uint32_t slot = 0; slot < WIN_PORT_SLOTS; slot++) {
        if (!present[slot]) {
            close_input(slot);
        }
    }
}
