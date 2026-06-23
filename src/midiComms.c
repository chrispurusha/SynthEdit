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

#include "sysIncludes.h"
#include "defs.h"
#include "types.h"
#include "globalVars.h"
#include "z1Comms.h"
#include "midiComms.h"

static void            (*gWakeCb)(void) = NULL;
static pthread_t       gMidiThread      = 0;
static pthread_mutex_t gSendMutex       = PTHREAD_MUTEX_INITIALIZER;

// SysEx reassembly — CoreMIDI may fragment large messages across packets
#define SYSEX_BUF_SIZE    8192
static uint8_t         gSysExBuf[SYSEX_BUF_SIZE];
static uint32_t        gSysExLen = 0;
static MIDIEndpointRef gSysExSrc = 0;

// ── Internal send ─────────────────────────────────────────────────────────────

static void midi_send_to(const uint8_t * data, uint32_t length, MIDIEndpointRef dest) {
    if ((gMidiOutPort == 0) || (dest == 0) || (data == NULL) || (length == 0)) {
        return;
    }
    uint8_t          buf[512 + sizeof(MIDIPacketList)];
    MIDIPacketList * pktList = (MIDIPacketList *)buf;
    MIDIPacket *     pkt     = MIDIPacketListInit(pktList);

    pkt = MIDIPacketListAdd(pktList, sizeof(buf), pkt, 0, length, data);

    if (pkt == NULL) {
        LOG_ERROR("MIDIPacketListAdd failed (message too long?)\n");
        return;
    }
    pthread_mutex_lock(&gSendMutex);
    OSStatus err = MIDISend(gMidiOutPort, dest, pktList);
    pthread_mutex_unlock(&gSendMutex);

    if (err != noErr) {
        LOG_ERROR("MIDISend error %d\n", (int)err);
    }
}

// ── Identity reply ────────────────────────────────────────────────────────────

static void handle_identity_reply(MIDIEndpointRef src, const uint8_t * data, uint32_t length) {
    // F0 7E <device_id> 06 02 <mfr_id> <fam_lsb> <fam_msb> <mem_lsb> <mem_msb> ... F7
    if (length < 10) {
        return;
    }
    if (data[5] != KORG_MANUFACTURER_ID) {
        LOG_DEBUG("identity reply mfr 0x%02X != Korg 0x%02X, ignoring\n", data[5], KORG_MANUFACTURER_ID);
        return;
    }
    uint8_t  deviceId = data[2];
    uint16_t family   = (uint16_t)(data[6] | ((uint16_t)data[7] << 7));
    uint16_t member   = (uint16_t)(data[8] | ((uint16_t)data[9] << 7));

    LOG_DEBUG("Korg identity reply: device_id=0x%02X family=%u member=%u\n",
              deviceId, (unsigned)family, (unsigned)member);

    MIDIEntityRef   entity = 0;
    MIDIEndpointRef dest   = 0;

    if (MIDIEndpointGetEntity(src, &entity) == noErr && entity != 0) {
        ItemCount dests = MIDIEntityGetNumberOfDestinations(entity);
        if (dests > 0) {
            dest = MIDIEntityGetDestination(entity, 0);
        }
    }
    if (dest == 0) {
        LOG_ERROR("No destination found for Korg identity reply source\n");
        return;
    }
    gDevice.id        = deviceId;
    gDevice.family    = family;
    gDevice.member    = member;
    gDevice.connected = true;
    gMidiSource       = src;
    gMidiDest         = dest;

    LOG_DEBUG("Locked onto Korg Z1 device\n");

    z1_on_connected();

    if (gWakeCb != NULL) {
        gWakeCb();
    }
}

// ── MIDI notification callback ────────────────────────────────────────────────

static void midi_notify_cb(const MIDINotification * msg, void * refCon) {
    (void)refCon;
    if (msg->messageID == kMIDIMsgSetupChanged) {
        LOG_DEBUG("CoreMIDI setup changed\n");
        midi_scan_devices();
        atomic_store(&gReDraw, true);
        if (gWakeCb != NULL) {
            gWakeCb();
        }
    }
}

// ── SysEx dispatch ────────────────────────────────────────────────────────────

static void dispatch_sysex(MIDIEndpointRef src, const uint8_t * data, uint32_t length) {
    if (  (length >= 5)
       && (data[1] == MIDI_NON_REALTIME)
       && (data[3] == MIDI_IDENTITY_REQUEST_SUB1)
       && (data[4] == MIDI_IDENTITY_REPLY_SUB2)) {
        handle_identity_reply(src, data, length);
    } else {
        z1_handle_message(data, length);
    }
    if (gWakeCb != NULL) {
        gWakeCb();
    }
}

// ── MIDI read callback (CoreMIDI thread) ──────────────────────────────────────

static void midi_read_cb(const MIDIPacketList * pktList, void * readProcRefCon, void * srcConnRefCon) {
    (void)readProcRefCon;
    MIDIEndpointRef    src = (MIDIEndpointRef)(uintptr_t)srcConnRefCon;
    const MIDIPacket * pkt = &pktList->packet[0];

    for (uint32_t i = 0; i < pktList->numPackets; i++) {
        for (uint16_t b = 0; b < pkt->length; b++) {
            uint8_t byte = pkt->data[b];
            if (byte == MIDI_SYSEX_START) {
                gSysExBuf[0] = byte;
                gSysExLen    = 1;
                gSysExSrc    = src;
            } else if (byte == MIDI_SYSEX_END) {
                if (gSysExLen > 0) {
                    if (gSysExLen < SYSEX_BUF_SIZE) {
                        gSysExBuf[gSysExLen++] = byte;
                    }
                    dispatch_sysex(gSysExSrc, gSysExBuf, gSysExLen);
                    gSysExLen = 0;
                }
            } else if (byte >= 0xF8) {
                // Realtime — ignore
            } else if (byte >= 0x80) {
                if (gSysExLen > 0) {
                    LOG_DEBUG("SysEx aborted by status 0x%02X\n", byte);
                    gSysExLen = 0;
                }
            } else {
                if (gSysExLen > 0) {
                    if (gSysExLen < SYSEX_BUF_SIZE) {
                        gSysExBuf[gSysExLen++] = byte;
                    } else {
                        LOG_ERROR("SysEx buffer overflow, discarding\n");
                        gSysExLen = 0;
                    }
                }
            }
        }
        pkt = MIDIPacketNext(pkt);
    }
}

// ── Device scanning ───────────────────────────────────────────────────────────

int midi_scan_devices(void) {
    static const uint8_t idReq[] = {
        MIDI_SYSEX_START,
        MIDI_NON_REALTIME,
        MIDI_DEVICE_INQUIRY,
        MIDI_IDENTITY_REQUEST_SUB1,
        MIDI_IDENTITY_REQUEST_SUB2,
        MIDI_SYSEX_END
    };

    ItemCount srcCount  = MIDIGetNumberOfSources();
    ItemCount destCount = MIDIGetNumberOfDestinations();

    gMidiSource = 0;
    gMidiDest   = 0;
    memset(&gDevice, 0, sizeof(gDevice));

    for (ItemCount i = 0; i < srcCount; i++) {
        MIDIEndpointRef src  = MIDIGetSource(i);
        CFStringRef     name = NULL;
        MIDIObjectGetStringProperty(src, kMIDIPropertyName, &name);
        if (name != NULL) {
            char buf[128] = {0};
            CFStringGetCString(name, buf, sizeof(buf), kCFStringEncodingUTF8);
            CFRelease(name);
            LOG_DEBUG("MIDI source %lu: %s\n", (unsigned long)i, buf);
        }
        MIDIPortConnectSource(gMidiInPort, src, (void *)(uintptr_t)src);
    }

    for (ItemCount i = 0; i < destCount; i++) {
        MIDIEndpointRef dest = MIDIGetDestination(i);
        CFStringRef     name = NULL;
        MIDIObjectGetStringProperty(dest, kMIDIPropertyName, &name);
        if (name != NULL) {
            char buf[128] = {0};
            CFStringGetCString(name, buf, sizeof(buf), kCFStringEncodingUTF8);
            CFRelease(name);
            LOG_DEBUG("MIDI dest %lu: %s\n", (unsigned long)i, buf);
        }
        midi_send_to(idReq, sizeof(idReq), dest);
    }

    if ((srcCount > 0) && (destCount > 0)) {
        return EXIT_SUCCESS;
    }
    LOG_DEBUG("No MIDI sources/destinations found\n");
    return EXIT_FAILURE;
}

// ── Public send ───────────────────────────────────────────────────────────────

void midi_send_identity_request(void) {
    static const uint8_t idReq[] = {
        MIDI_SYSEX_START,
        MIDI_NON_REALTIME,
        MIDI_DEVICE_INQUIRY,
        MIDI_IDENTITY_REQUEST_SUB1,
        MIDI_IDENTITY_REQUEST_SUB2,
        MIDI_SYSEX_END
    };
    midi_send_to(idReq, sizeof(idReq), gMidiDest);
}

void midi_send(const uint8_t * data, uint32_t length) {
    midi_send_to(data, length, gMidiDest);
}

// ── MIDI poll thread ──────────────────────────────────────────────────────────

static void * midi_thread(void * arg) {
    (void)arg;
    LOG_DEBUG("MIDI thread started\n");
    midi_scan_devices();

    while (!atomic_load(&gQuitAll)) {
        struct timespec ts = {0, 33000000};   // ~30 Hz idle poll
        nanosleep(&ts, NULL);
    }
    LOG_DEBUG("MIDI thread exiting\n");
    return NULL;
}

// ── Startup ───────────────────────────────────────────────────────────────────

int start_midi_thread(void) {
    CFStringRef clientName = CFSTR("Z1Edit");
    OSStatus    err;

    err = MIDIClientCreate(clientName, midi_notify_cb, NULL, &gMidiClient);
    if (err != noErr) {
        LOG_ERROR("MIDIClientCreate failed: %d\n", (int)err);
        return EXIT_FAILURE;
    }
    CFStringRef inName = CFSTR("Z1Edit In");
    err = MIDIInputPortCreate(gMidiClient, inName, midi_read_cb, NULL, &gMidiInPort);
    if (err != noErr) {
        LOG_ERROR("MIDIInputPortCreate failed: %d\n", (int)err);
        return EXIT_FAILURE;
    }
    CFStringRef outName = CFSTR("Z1Edit Out");
    err = MIDIOutputPortCreate(gMidiClient, outName, &gMidiOutPort);
    if (err != noErr) {
        LOG_ERROR("MIDIOutputPortCreate failed: %d\n", (int)err);
        return EXIT_FAILURE;
    }
    if (pthread_create(&gMidiThread, NULL, midi_thread, NULL) != 0) {
        LOG_ERROR("pthread_create for MIDI thread failed\n");
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

void register_midi_wake_cb(void (*cb)(void)) {
    gWakeCb = cb;
}
