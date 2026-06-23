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

// ── Identity reply buffer ─────────────────────────────────────────────────────
// The CoreMIDI read callback fires on the CoreMIDI thread; the MIDI thread
// processes replies after a timeout.  Having the callback do nothing except
// store data eliminates all races with gDevice and rescan logic.
#define MAX_IDENTITY_REPLIES    16

static struct {
    MIDIEndpointRef src;
    uint8_t         deviceId;    // data[2]
    uint8_t         mfrId;       // data[5]
    uint8_t         familyLSB;   // data[6]
    uint8_t         memberLSB;   // data[8]
} gIdReplies[MAX_IDENTITY_REPLIES];

static _Atomic uint32_t gIdReplyCount = 0;

// Notification from notify thread; polled by the MIDI thread.
static _Atomic bool gRescanNeeded = false;

// ── SysEx reassembly ──────────────────────────────────────────────────────────
#define SYSEX_BUF_SIZE    8192
static uint8_t         gSysExBuf[SYSEX_BUF_SIZE];
static uint32_t        gSysExLen = 0;
static MIDIEndpointRef gSysExSrc = 0;

// ── Non-SysEx message state (running status) ──────────────────────────────────
static uint8_t gMsgStatus  = 0;
static uint8_t gMsgData[2];
static uint8_t gMsgDataLen = 0;

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

// ── Destination lookup ────────────────────────────────────────────────────────
// Called from the MIDI thread (not the callback thread), so CoreMIDI API calls
// are safe and the device list is fully settled by the time we get here.
//
// Strategies tried in order:
//   1. Source's own entity → entity's destinations
//   2. All global destinations for entity match (catches some virtual drivers)
//   3. All global destinations by display-name match (last resort)

static MIDIEndpointRef find_dest_for_source(MIDIEndpointRef src) {
    MIDIEntityRef entity = 0;
    MIDIEndpointGetEntity(src, &entity);    // may fail — entity stays 0

    // Strategy 1
    if (entity != 0) {
        ItemCount n = MIDIEntityGetNumberOfDestinations(entity);
        for (ItemCount d = 0; d < n; d++) {
            MIDIEndpointRef r = MIDIEntityGetDestination(entity, d);
            if (r != 0) {
                LOG_DEBUG("dest found via entity (strategy 1)\n");
                return r;
            }
        }
    }

    // Strategies 2 & 3 share one pass over all destinations
    CFStringRef     srcDisplayName = NULL;
    MIDIObjectGetStringProperty(src, kMIDIPropertyDisplayName, &srcDisplayName);
    if (srcDisplayName == NULL) {
        MIDIObjectGetStringProperty(src, kMIDIPropertyName, &srcDisplayName);
    }

    MIDIEndpointRef dest      = 0;
    ItemCount       destCount = MIDIGetNumberOfDestinations();

    for (ItemCount d = 0; d < destCount && dest == 0; d++) {
        MIDIEndpointRef candidate = MIDIGetDestination(d);

        // Strategy 2: entity match
        if (entity != 0) {
            MIDIEntityRef candEntity = 0;
            if (MIDIEndpointGetEntity(candidate, &candEntity) == noErr && candEntity == entity) {
                LOG_DEBUG("dest found via entity scan (strategy 2)\n");
                dest = candidate;
                break;
            }
        }

        // Strategy 3: display-name match
        if (srcDisplayName != NULL) {
            CFStringRef destDisplayName = NULL;
            MIDIObjectGetStringProperty(candidate, kMIDIPropertyDisplayName, &destDisplayName);
            if (destDisplayName == NULL) {
                MIDIObjectGetStringProperty(candidate, kMIDIPropertyName, &destDisplayName);
            }
            if (destDisplayName != NULL) {
                if (CFStringCompare(srcDisplayName, destDisplayName, 0) == kCFCompareEqualTo) {
                    LOG_DEBUG("dest found via display-name match (strategy 3)\n");
                    dest = candidate;
                }
                CFRelease(destDisplayName);
            }
        }
    }

    if (srcDisplayName != NULL) {
        CFRelease(srcDisplayName);
    }
    return dest;
}

// ── Process buffered identity replies (MIDI thread only) ──────────────────────
// Scans the reply buffer collected since the last scan, selects the first Z1,
// and calls z1_on_connected().  All CoreMIDI lookups happen here — no races.

static void process_identity_replies(void) {
    uint32_t count = atomic_load(&gIdReplyCount);

    LOG_DEBUG("Processing %u identity replies\n", (unsigned)count);

    for (uint32_t i = 0; i < count; i++) {
        LOG_DEBUG("  reply[%u]: mfr=0x%02X fam=0x%02X mem=0x%02X src=0x%08X\n",
                  (unsigned)i,
                  gIdReplies[i].mfrId,
                  gIdReplies[i].familyLSB,
                  gIdReplies[i].memberLSB,
                  (unsigned)gIdReplies[i].src);

        if ((gIdReplies[i].mfrId     != KORG_MANUFACTURER_ID) ||
            (gIdReplies[i].familyLSB != Z1_FAMILY_ID) ||
            (gIdReplies[i].memberLSB != Z1_MEMBER_ID)) {
            continue;
        }

        MIDIEndpointRef src  = gIdReplies[i].src;
        MIDIEndpointRef dest = find_dest_for_source(src);

        if (dest == 0) {
            LOG_ERROR("Z1 found but no matching destination for src=0x%08X\n", (unsigned)src);
            continue;
        }

        gDevice.id        = gIdReplies[i].deviceId;
        gDevice.family    = (uint16_t)gIdReplies[i].familyLSB;
        gDevice.member    = (uint16_t)gIdReplies[i].memberLSB;
        gDevice.connected = true;
        gMidiSource       = src;
        gMidiDest         = dest;

        LOG_DEBUG("Z1 connected: deviceId=0x%02X src=0x%08X dest=0x%08X\n",
                  gDevice.id, (unsigned)src, (unsigned)dest);

        z1_on_connected();

        if (gWakeCb != NULL) {
            gWakeCb();
        }
        return;
    }
    LOG_DEBUG("No Z1 found in this batch of identity replies\n");
}

// ── Identity reply callback handler ──────────────────────────────────────────
// Runs on the CoreMIDI callback thread.  Do the absolute minimum: validate
// the packet is a well-formed identity reply and store it.  All analysis
// happens later in process_identity_replies() on the MIDI thread.

static void handle_identity_reply(MIDIEndpointRef src, const uint8_t * data, uint32_t length) {
    // F0 7E <device_id> 06 02 <mfr_id> <fam_lsb> <fam_msb> <mem_lsb> <mem_msb> ... F7
    if (length < 10) {
        return;
    }
    uint32_t idx = atomic_fetch_add(&gIdReplyCount, 1);
    if (idx < MAX_IDENTITY_REPLIES) {
        gIdReplies[idx].src       = src;
        gIdReplies[idx].deviceId  = data[2];
        gIdReplies[idx].mfrId     = data[5];
        gIdReplies[idx].familyLSB = data[6];
        gIdReplies[idx].memberLSB = data[8];
    }
}

// ── MIDI notification callback ────────────────────────────────────────────────

static void midi_notify_cb(const MIDINotification * msg, void * refCon) {
    (void)refCon;
    if (msg->messageID == kMIDIMsgSetupChanged) {
        LOG_DEBUG("CoreMIDI setup changed — scheduling rescan\n");
        atomic_store(&gRescanNeeded, true);
    }
}

// ── CC dispatch ───────────────────────────────────────────────────────────────
// Only called from midi_read_cb for messages arriving from the Z1's source.

static void dispatch_cc(uint8_t cc, uint8_t value) {
    LOG_DEBUG("CC 0x%02X val=%u\n", (unsigned)cc, (unsigned)value);
    if (cc == 0x55) {    // CC 85 = Filter 1 Cutoff
        gDevice.filter1Cutoff = value;
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
                gMsgStatus   = byte;
                gMsgDataLen  = 0;
            } else {
                if (gSysExLen > 0) {
                    if (gSysExLen < SYSEX_BUF_SIZE) {
                        gSysExBuf[gSysExLen++] = byte;
                    } else {
                        LOG_ERROR("SysEx buffer overflow, discarding\n");
                        gSysExLen = 0;
                    }
                } else if (gMsgStatus != 0) {
                    if (gMsgDataLen < 2) {
                        gMsgData[gMsgDataLen++] = byte;
                    }
                    // CC is a 3-byte message (status + 2 data bytes)
                    if (((gMsgStatus & 0xF0) == 0xB0) && (gMsgDataLen == 2)) {
                        if (gDevice.connected && (src == gMidiSource)) {
                            dispatch_cc(gMsgData[0], gMsgData[1]);
                        }
                        gMsgDataLen = 0;    // ready for running status
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

    // Reset reply buffer and connection state before a fresh scan
    atomic_store(&gIdReplyCount, 0);
    gMidiSource = 0;
    gMidiDest   = 0;
    memset(&gDevice, 0, sizeof(gDevice));

    for (ItemCount i = 0; i < srcCount; i++) {
        MIDIEndpointRef src  = MIDIGetSource(i);
        CFStringRef     name = NULL;
        MIDIObjectGetStringProperty(src, kMIDIPropertyDisplayName, &name);
        if (name != NULL) {
            char buf[128] = {0};
            CFStringGetCString(name, buf, sizeof(buf), kCFStringEncodingUTF8);
            CFRelease(name);
            LOG_DEBUG("MIDI source %lu: %s (ref=0x%08X)\n", (unsigned long)i, buf, (unsigned)src);
        }
        MIDIPortConnectSource(gMidiInPort, src, (void *)(uintptr_t)src);
    }

    for (ItemCount i = 0; i < destCount; i++) {
        MIDIEndpointRef dest = MIDIGetDestination(i);
        CFStringRef     name = NULL;
        MIDIObjectGetStringProperty(dest, kMIDIPropertyDisplayName, &name);
        if (name != NULL) {
            char buf[128] = {0};
            CFStringGetCString(name, buf, sizeof(buf), kCFStringEncodingUTF8);
            CFRelease(name);
            LOG_DEBUG("MIDI dest %lu: %s (ref=0x%08X)\n", (unsigned long)i, buf, (unsigned)dest);
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

void midi_send_cc(uint8_t channelIndex, uint8_t cc, uint8_t value) {
    uint8_t msg[3] = {
        (uint8_t)(0xB0 | (channelIndex & 0x0F)),
        (uint8_t)(cc    & 0x7F),
        (uint8_t)(value & 0x7F),
    };
    midi_send_to(msg, 3, gMidiDest);
}

// ── MIDI poll thread ──────────────────────────────────────────────────────────
// Owns all scanning and connection logic.  The CoreMIDI callback thread only
// stores raw data; no state mutations happen there.

static void * midi_thread(void * arg) {
    (void)arg;
    LOG_DEBUG("MIDI thread started\n");

    struct timespec reply_wait = {0, 500000000};   // 500 ms — collect all responses
    struct timespec retry_wait = {2, 0};            // 2 s between scan attempts

    while (!atomic_load(&gQuitAll)) {
        if (!gDevice.connected) {
            // (Re)scan: send identity requests to all destinations
            atomic_store(&gRescanNeeded, false);
            midi_scan_devices();

            // Wait for every device — including slow ones via MIDI thru — to reply
            nanosleep(&reply_wait, NULL);

            // Now process the complete, settled set of replies
            process_identity_replies();

            if (!gDevice.connected) {
                // Nothing found; wait before trying again.
                // Wake early if a setup change signals a new device appeared.
                for (int t = 0; t < 20 && !atomic_load(&gRescanNeeded); t++) {
                    struct timespec s = {0, 100000000};   // 100 ms slices
                    nanosleep(&s, NULL);
                }
            }
        } else {
            struct timespec ts = {0, 33000000};   // 33 ms idle
            nanosleep(&ts, NULL);
        }
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
