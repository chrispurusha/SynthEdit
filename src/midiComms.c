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
// Notes: Docs/code-notes/midiComms.c.md - "// notes §k" refers there.

#include "sysIncludes.h"
#include "defs.h"
#include "synthlibDefs.h"
#include "synthlibMidi.h"   // AFTER defs.h — synthlibDefs.h gates its colours on the app macro
#include "types.h"
#include "globalVars.h"
#include "msgQueue.h"
#include "synthComms.h"
#include "synthGraphics.h"
#include "midiComms.h"

static void                    (*gWakeCb)(void) = NULL;
static pthread_t               gMidiThread             = 0;
// gSendMutex moved into SynthLib with the send primitive it guarded — see synthlibMidi.c.

// notes §1

// Notification from notify thread; polled by the MIDI thread.
static _Atomic bool            gRescanNeeded           = false;

// WHAT THE MIDI PORTS DIALOGUE SHOWS, published by this thread for the UI to read. Copies rather than
// gMidiSource/gMidiDest, which only this thread may touch - see midi_port_status(). A zero source with
// a non-zero destination is a device connected without an identity reply: heard on any input.
static _Atomic MIDIEndpointRef gShownSource            = 0;
static _Atomic MIDIEndpointRef gShownDest              = 0;

// notes §2
static _Atomic bool            gReconnectRequested     = false;

// notes §3
#define SYNTH_STATE_DUMP_DEBOUNCE_TICKS    8 // * MIDI_IDLE_TICK_SECONDS below ~= 264ms
#define MIDI_IDLE_TICK_SECONDS             0.033
static _Atomic int             gStateDumpDebounceTicks = 0;

// notes §4

void midi_arm_state_dump_debounce(void) {
    gStateDumpDebounceTicks = SYNTH_STATE_DUMP_DEBOUNCE_TICKS;
}

// notes §5
#define SYSEX_BUF_SIZE    65536
static uint8_t         gSysExBuf[SYSEX_BUF_SIZE];
static uint32_t        gSysExLen = 0;
static MIDIEndpointRef gSysExSrc = 0;

// notes §6
#define MAX_MIDI_PARSE_SOURCES    16

typedef struct {
    MIDIEndpointRef src;
    uint8_t         msgStatus;
    uint8_t         msgData[2];
    uint8_t         msgDataLen;
} tMidiChannelParseState;

static tMidiChannelParseState gChannelParseState[MAX_MIDI_PARSE_SOURCES] = {0};
static uint32_t               gChannelParseStateCount                    = 0;

// notes §7
static tMidiChannelParseState * channel_parse_state_for(MIDIEndpointRef src) {
    for (uint32_t i = 0; i < gChannelParseStateCount; i++) {
        if (gChannelParseState[i].src == src) {
            return &gChannelParseState[i];
        }
    }

    if (gChannelParseStateCount < MAX_MIDI_PARSE_SOURCES) {
        gChannelParseState[gChannelParseStateCount].src = src;
        return &gChannelParseState[gChannelParseStateCount++];
    }
    return &gChannelParseState[MAX_MIDI_PARSE_SOURCES - 1];
}

// ── Internal send ─────────────────────────────────────────────────────────────

// notes §8
static bool midi_send_to(const uint8_t * data, uint32_t length, MIDIEndpointRef dest) {
    // notes §9
    return synthlib_midi_send_to(data, length, dest);
}

// notes §10

static MIDIEndpointRef find_dest_for_source(MIDIEndpointRef src) {
    MIDIEntityRef   entity         = 0;

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
    MIDIEndpointRef dest           = 0;
    ItemCount       destCount      = MIDIGetNumberOfDestinations();

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

// Declared below (its own comment there covers what it does) — needed here
// too now that process_identity_replies() can also fall back to it.
static MIDIEndpointRef find_destination_by_name(const char * substr);

// notes §11

static void discard_queued_identity_replies(void) {
    tMessageContent msg     = {0};
    uint32_t        dropped = 0;

    while (msg_receive(&gToMidiThread, eRcvPoll, &msg) == EXIT_SUCCESS) {
        dropped++;
    }

    if (dropped > 0) {
        LOG_DEBUG("Discarded %u stale identity replies before rescan\n", (unsigned)dropped);
    }
}

// ── Process buffered identity replies (MIDI thread only) ──────────────────────
// Scans the reply buffer collected since the last scan, selects the first synth
// and calls synth_on_connected().  All CoreMIDI lookups happen here — no races.

static void process_identity_replies(void) {
    tMessageContent      msg                                  = {0};
    tIdentityReplyData * reply                                = &msg.identityReplyData;
    uint32_t             index                                = 0;
    bool                 connected                            = false;
    char                 wantIn[SYNTHLIB_MIDI_PORT_NAME_MAX]  = {0};
    char                 wantOut[SYNTHLIB_MIDI_PORT_NAME_MAX] = {0};

    // The MIDI Ports dialogue's choice (synthlibMidi.h), read once for the whole batch.
    synthlib_midi_ports_chosen(wantIn, sizeof(wantIn), wantOut, sizeof(wantOut));

    // notes §12
    while (msg_receive(&gToMidiThread, eRcvPoll, &msg) == EXIT_SUCCESS) {
        if (msg.cmd != eMsgCmdIdentityReply) {
            LOG_ERROR("Unknown MIDI-thread command %u\n", (unsigned)msg.cmd);
            continue;
        }
        // notes §13
        LOG_DEBUG("  reply[%u]: mfrLen=%u mfr=%02X:%02X:%02X fam=0x%02X mem=0x%02X src=0x%08X\n",
                  (unsigned)index,
                  (unsigned)reply->mfrIdLen,
                  reply->mfrId[0],
                  reply->mfrId[1],
                  reply->mfrId[2],
                  reply->familyLSB,
                  reply->memberLSB,
                  (unsigned)reply->source);
        index++;

        tPanelConfig *  cfg = synth_panel_config();

        if (  connected
           || (reply->mfrIdLen != cfg->manufacturerIdLen)
           || (memcmp(reply->mfrId, cfg->manufacturerId, cfg->manufacturerIdLen) != 0)
           || (reply->familyLSB != cfg->familyId)
           || (reply->memberLSB != cfg->memberId)) {
            continue;
        }
        MIDIEndpointRef src = (MIDIEndpointRef)reply->source;

        // notes §14
        if ((wantIn[0] != '\0') && (src != synthlib_midi_find_port(true, wantIn))) {
            LOG_DEBUG("Identity reply from a source other than the chosen input '%s' - ignored\n", wantIn);
            continue;
        }
        MIDIEndpointRef dest = (wantOut[0] != '\0') ? synthlib_midi_find_port(false, wantOut)
                               : (cfg->midiPortName[0] != '\0') ? find_destination_by_name(cfg->midiPortName)
                               : find_dest_for_source(src);

        if (dest == 0) {
            LOG_ERROR("Synth found but no matching destination for src=0x%08X\n", (unsigned)src);
            continue;
        }
        gDevice.id        = reply->deviceId;
        gDevice.family    = (uint16_t)reply->familyLSB;
        gDevice.member    = (uint16_t)reply->memberLSB;
        gDevice.connected = true;
        gMidiSource       = src;
        gMidiDest         = dest;
        atomic_store(&gShownSource, src);
        atomic_store(&gShownDest, dest);

        LOG_DEBUG("Synth connected: deviceId=0x%02X src=0x%08X dest=0x%08X\n",
                  gDevice.id, (unsigned)src, (unsigned)dest);

        synth_on_connected();

        if (gWakeCb != NULL) {
            gWakeCb();
        }
        connected         = true;
    }
    LOG_DEBUG("Processed %u identity replies\n", (unsigned)index);

    if (!connected) {
        LOG_DEBUG("No Synth found in this batch of identity replies\n");
    }
}

// notes §15

static void handle_identity_reply(MIDIEndpointRef src, const uint8_t * data, uint32_t length) {
    // notes §16
    if (length < 10) {
        return;
    }
    uint32_t        mfrLen = (data[5] == 0x00) ? 3 : 1;

    if (length < (uint32_t)(5 + mfrLen + 4)) {
        return;
    }
    tMessageContent msg    = {0};

    msg.cmd                         = eMsgCmdIdentityReply;
    msg.identityReplyData.source    = (uint32_t)src;
    msg.identityReplyData.deviceId  = data[2];
    msg.identityReplyData.mfrIdLen  = mfrLen;
    // mfrId's unused bytes stay zeroed by msg's own initialiser, for a clean display in the
    // mfrLen==1 case — see process_identity_replies()'s log line.
    memcpy(msg.identityReplyData.mfrId, &data[5], mfrLen);
    msg.identityReplyData.familyLSB = data[5 + mfrLen];
    msg.identityReplyData.memberLSB = data[5 + mfrLen + 2];

    msg_send(&gToMidiThread, &msg);
}

// notes §17

static void connect_all_midi_sources(void) {
    ItemCount srcCount = MIDIGetNumberOfSources();

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
}

// notes §18

static MIDIEndpointRef find_destination_by_name(const char * substr) {
    if ((substr == NULL) || (substr[0] == '\0')) {
        return 0;
    }
    CFStringRef     needle    = CFStringCreateWithCString(NULL, substr, kCFStringEncodingUTF8);

    if (needle == NULL) {
        return 0;
    }
    MIDIEndpointRef found     = 0;
    ItemCount       destCount = MIDIGetNumberOfDestinations();

    for (ItemCount i = 0; i < destCount && found == 0; i++) {
        MIDIEndpointRef dest = MIDIGetDestination(i);
        CFStringRef     name = NULL;
        MIDIObjectGetStringProperty(dest, kMIDIPropertyDisplayName, &name);

        if (name == NULL) {
            MIDIObjectGetStringProperty(dest, kMIDIPropertyName, &name);
        }

        if (name != NULL) {
            if (CFStringFind(name, needle, kCFCompareCaseInsensitive).location != kCFNotFound) {
                found = dest;
            }
            CFRelease(name);
        }
    }

    CFRelease(needle);
    return found;
}

// notes §19

static void connect_without_identity(void) {
    ItemCount       destCount                            = MIDIGetNumberOfDestinations();

    if (destCount == 0) {
        return; // nothing to send to yet — caller retries after a short wait
    }
    tPanelConfig *  cfg                                  = synth_panel_config();
    MIDIEndpointRef dest                                 = 0;
    MIDIEndpointRef onlySrc                              = 0;
    char            wantIn[SYNTHLIB_MIDI_PORT_NAME_MAX]  = {0};
    char            wantOut[SYNTHLIB_MIDI_PORT_NAME_MAX] = {0};

    atomic_store(&gShownSource, 0);
    atomic_store(&gShownDest, 0);

    // The MIDI Ports dialogue's choice first. Here it matters more than anywhere: with no identity
    // reply there is nothing to infer a port from, and the fallback below is simply the FIRST
    // destination CoreMIDI lists. A chosen port that is not plugged in is waited for like midiPort.
    synthlib_midi_ports_chosen(wantIn, sizeof(wantIn), wantOut, sizeof(wantOut));

    if (wantIn[0] != '\0') {
        onlySrc = synthlib_midi_find_port(true, wantIn);

        if (onlySrc == 0) {
            return; // chosen input not visible yet — caller retries after a short wait
        }
    }

    if (wantOut[0] != '\0') {
        dest = synthlib_midi_find_port(false, wantOut);

        if (dest == 0) {
            return; // chosen output not visible yet — caller retries after a short wait
        }
    } else if (cfg->midiPortName[0] != '\0') {
        dest = find_destination_by_name(cfg->midiPortName);

        if (dest == 0) {
            return; // named port not visible yet — caller retries after a short wait
        }
    } else {
        dest = MIDIGetDestination(0);
    }
    connect_all_midi_sources();

    gMidiDest         = dest;
    gMidiSource       = onlySrc;   // 0 still means "any source", as below; a chosen input narrows it
    gDevice.id        = (uint8_t)((cfg->midiChannel > 0) ? (cfg->midiChannel - 1) : 0);
    gDevice.family    = 0;
    gDevice.member    = 0;
    gDevice.connected = true;
    atomic_store(&gShownSource, onlySrc);
    atomic_store(&gShownDest, dest);

    LOG_DEBUG("Synth connected without identity query: channel=%u dest=0x%08X\n",
              (unsigned)(gDevice.id + 1), (unsigned)gMidiDest);

    synth_on_connected();

    // notes §20
    if (cfg->stateRequestSysExLen > 0) {
        midi_send(cfg->stateRequestSysEx, cfg->stateRequestSysExLen);
        LOG_DEBUG("Sent device state request (%u bytes)\n", (unsigned)cfg->stateRequestSysExLen);
    }

    if (gWakeCb != NULL) {
        gWakeCb();
    }
}

// ── MIDI notification callback ────────────────────────────────────────────────

static void midi_notify_cb(const MIDINotification * msg, void * refCon) {
    (void)refCon;

    if (msg->messageID == kMIDIMsgSetupChanged) {
        LOG_DEBUG("CoreMIDI setup changed — scheduling rescan\n");
        gRescanNeeded = true;
    }
}

// ── CC dispatch ───────────────────────────────────────────────────────────────
// Only called from midi_read_cb for messages arriving from the Synth's source.

// notes §21
static double debug_elapsed_ms(void) {
    static struct timespec start         = {0};
    static bool            startCaptured = false;
    struct timespec        now;

    clock_gettime(CLOCK_MONOTONIC, &now);

    if (!startCaptured) {
        start         = now;
        startCaptured = true;
    }
    double                 seconds       = (double)(now.tv_sec - start.tv_sec) + (double)(now.tv_nsec - start.tv_nsec) / 1e9;

    return seconds * 1000.0;
}

static void dispatch_cc(uint8_t channel, uint8_t cc, uint8_t value) {
    LOG_DEBUG("[%.1fms] CC ch=%u 0x%02X val=%u\n", debug_elapsed_ms(),
              (unsigned)(channel + 1), (unsigned)cc, (unsigned)value);

    // notes §22
    if (!synth_panel_config()->supportsIdentity && (gDevice.id != channel)) {
        LOG_DEBUG("Auto-detected device MIDI channel: %u (was %u)\n",
                  (unsigned)(channel + 1), (unsigned)(gDevice.id + 1));
        gDevice.id = channel;
    }
    // Generic: whichever dial (if any) has this cc= in the device's own
    // <device>.txt gets the value — no per-device CC list here.
    bool handled = synth_handle_cc(cc, value);

    if (handled) {
        synthlib_request_redraw();

        if (gWakeCb != NULL) {
            gWakeCb();
        }
    }
}

// notes §23
static void dispatch_program_change(uint8_t channel, uint8_t program) {
    LOG_DEBUG("Program Change ch=%u program=%u — reloading current state\n",
              (unsigned)(channel + 1), (unsigned)program);

    if (!synth_panel_config()->supportsIdentity && (gDevice.id != channel)) {
        gDevice.id = channel;
    }
    gDevice.currentProgram = program; // see the tSynthDevice field comment in types.h — this is the only way it's ever learned

    if (gDevice.connected) {
        // notes §24
        midi_arm_state_dump_debounce();
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
        synth_handle_message(data, length);
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
                if ((gSysExLen > 0) && (src != gSysExSrc)) {
                    // notes §25
                    LOG_DEBUG("SysEx start from a second source (src=0x%08X) ignored — already capturing from 0x%08X\n",
                              (unsigned)src, (unsigned)gSysExSrc);
                } else {
                    gSysExBuf[0] = byte;
                    gSysExLen    = 1;
                    gSysExSrc    = src;
                }
            } else if (byte == MIDI_SYSEX_END) {
                // Only the source that opened this capture can close it — an
                // F7 from anyone else is either a stray terminator or
                // belongs to a start this app already dropped just above.
                if ((gSysExLen > 0) && (src == gSysExSrc)) {
                    if (gSysExLen < SYSEX_BUF_SIZE) {
                        gSysExBuf[gSysExLen++] = byte;
                    }
                    dispatch_sysex(gSysExSrc, gSysExBuf, gSysExLen);
                    gSysExLen = 0;
                }
            } else if (byte >= 0xF8) {
                // Realtime — ignore. Spec-legal to interleave mid-SysEx from
                // ANY source, so this deliberately never touches gSysExLen.
            } else if (byte >= 0x80) {
                if ((gSysExLen > 0) && (src == gSysExSrc)) {
                    // notes §26
                    LOG_DEBUG("SysEx aborted by status 0x%02X\n", byte);
                    gSysExLen = 0;
                }
                tMidiChannelParseState * state = channel_parse_state_for(src);
                state->msgStatus  = byte;
                state->msgDataLen = 0;
            } else {
                if ((gSysExLen > 0) && (src == gSysExSrc)) {
                    if (gSysExLen < SYSEX_BUF_SIZE) {
                        gSysExBuf[gSysExLen++] = byte;
                    } else {
                        LOG_ERROR("SysEx buffer overflow, discarding\n");
                        gSysExLen = 0;
                    }
                } else if (gSysExLen == 0) {
                    tMidiChannelParseState * state = channel_parse_state_for(src);

                    if (state->msgStatus != 0) {
                        if (state->msgDataLen < 2) {
                            state->msgData[state->msgDataLen++] = byte;
                        }

                        // CC is a 3-byte message (status + 2 data bytes)
                        if (((state->msgStatus & 0xF0) == 0xB0) && (state->msgDataLen == 2)) {
                            // gMidiSource == 0 means "accept from any source" —
                            // set by connect_without_identity() for a device with
                            // no identity reply to correlate a specific one from.
                            if (gDevice.connected && ((gMidiSource == 0) || (src == gMidiSource))) {
                                dispatch_cc((uint8_t)(state->msgStatus & 0x0F), state->msgData[0], state->msgData[1]);
                            }
                            state->msgDataLen = 0;    // ready for running status
                        }

                        // notes §27
                        if (((state->msgStatus & 0xF0) == 0xC0) && (state->msgDataLen == 1)) {
                            if (gDevice.connected && ((gMidiSource == 0) || (src == gMidiSource))) {
                                dispatch_program_change((uint8_t)(state->msgStatus & 0x0F), state->msgData[0]);
                            }
                            state->msgDataLen = 0;
                        }
                    }
                }
                // notes §28
            }
        }

        pkt = MIDIPacketNext(pkt);
    }
}

// ── Device scanning ───────────────────────────────────────────────────────────

static int midi_scan_devices(void) {
    static const uint8_t idReq[]   = {
        MIDI_SYSEX_START,
        MIDI_NON_REALTIME,
        MIDI_DEVICE_INQUIRY,
        MIDI_IDENTITY_REQUEST_SUB1,
        MIDI_IDENTITY_REQUEST_SUB2,
        MIDI_SYSEX_END
    };

    ItemCount            destCount = MIDIGetNumberOfDestinations();

    // notes §29
    discard_queued_identity_replies();
    gMidiSource       = 0;
    gMidiDest         = 0;
    gDevice.connected = false;
    gDevice.id        = 0;
    gDevice.family    = 0;
    gDevice.member    = 0;

    atomic_store(&gShownSource, 0);
    atomic_store(&gShownDest, 0);

    connect_all_midi_sources();

    // notes §30
    char            wantOut[SYNTHLIB_MIDI_PORT_NAME_MAX] = {0};
    MIDIEndpointRef onlyDest                             = 0;

    synthlib_midi_ports_chosen(NULL, 0, wantOut, sizeof(wantOut));

    if (wantOut[0] != '\0') {
        onlyDest = synthlib_midi_find_port(false, wantOut);

        if (onlyDest == 0) {
            LOG_DEBUG("MIDI output '%s' was chosen and is not present - waiting for it\n", wantOut);
            return EXIT_FAILURE;
        }
    }

    for (ItemCount i = 0; i < destCount; i++) {
        MIDIEndpointRef dest = MIDIGetDestination(i);
        CFStringRef     name = NULL;

        if ((onlyDest != 0) && (dest != onlyDest)) {
            continue;
        }
        MIDIObjectGetStringProperty(dest, kMIDIPropertyDisplayName, &name);

        if (name != NULL) {
            char buf[128] = {0};
            CFStringGetCString(name, buf, sizeof(buf), kCFStringEncodingUTF8);
            CFRelease(name);
            LOG_DEBUG("MIDI dest %lu: %s (ref=0x%08X)\n", (unsigned long)i, buf, (unsigned)dest);
        }
        midi_send_to(idReq, sizeof(idReq), dest);

        // notes §31
        if ((i + 1) < destCount) {
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.015, false);
        }
    }

    if ((MIDIGetNumberOfSources() > 0) && (destCount > 0)) {
        return EXIT_SUCCESS;
    }
    LOG_DEBUG("No MIDI sources/destinations found\n");
    return EXIT_FAILURE;
}

// notes §32

void midi_request_reconnect(void) {
    atomic_store(&gReconnectRequested, true);
}

// ── MIDI Ports dialogue (UI thread) ──────────────────────────────────────────

// notes §33
void midi_set_port_scope(const char * configFile) {
    synthlib_midi_ports_set_scope(configFile);
}

void midi_port_status(char * text, size_t size) {
    char            wantIn[SYNTHLIB_MIDI_PORT_NAME_MAX]  = {0};
    char            wantOut[SYNTHLIB_MIDI_PORT_NAME_MAX] = {0};
    char            srcName[SYNTHLIB_MIDI_PORT_NAME_MAX] = {0};
    char            dstName[SYNTHLIB_MIDI_PORT_NAME_MAX] = {0};
    MIDIEndpointRef src                                  = atomic_load(&gShownSource);
    MIDIEndpointRef dest                                 = atomic_load(&gShownDest);

    synthlib_midi_ports_chosen(wantIn, sizeof(wantIn), wantOut, sizeof(wantOut));
    synthlib_midi_port_name(src, srcName, sizeof(srcName));
    synthlib_midi_port_name(dest, dstName, sizeof(dstName));

    if ((dest != 0) && (src != 0)) {
        snprintf(text, size, "Connected: heard on %s, played through %s", srcName, dstName);
    } else if (dest != 0) {
        snprintf(text, size, "Connected: played through %s, heard on any input", dstName);
    } else if ((wantOut[0] != '\0') && (synthlib_midi_find_port(false, wantOut) == 0)) {
        snprintf(text, size, "Waiting for %s to be plugged in", wantOut);
    } else if ((wantIn[0] != '\0') && (synthlib_midi_find_port(true, wantIn) == 0)) {
        snprintf(text, size, "Waiting for %s to be plugged in", wantIn);
    } else {
        snprintf(text, size, "Not connected - still looking");
    }
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

bool midi_send(const uint8_t * data, uint32_t length) {
    return midi_send_to(data, length, gMidiDest);
}

void midi_send_cc(uint8_t channelIndex, uint8_t cc, uint8_t value) {
    uint8_t msg[3] = {
        (uint8_t)(0xB0 | (channelIndex & 0x0F)),
        (uint8_t)(cc & 0x7F),
        (uint8_t)(value & 0x7F),
    };

    midi_send_to(msg, 3, gMidiDest);
}

void midi_send_program_change(uint8_t channelIndex, uint8_t program) {
    uint8_t msg[2] = {
        (uint8_t)(0xC0 | (channelIndex & 0x0F)),
        (uint8_t)(program & 0x7F),
    };

    midi_send_to(msg, 2, gMidiDest);
}

// ── MIDI poll thread ──────────────────────────────────────────────────────────
// Owns all scanning and connection logic.  The CoreMIDI callback thread only
// stores raw data; no state mutations happen there.

static void * midi_thread(void * arg) {
    (void)arg;
    LOG_DEBUG("MIDI thread started\n");

    // Create MIDI client here (not on main thread) so MIDIClientCreate does not
    // block app startup.  The notification callback is tied to this thread's
    // CFRunLoop, which we drive with CFRunLoopRunInMode in place of nanosleep.
    OSStatus err;
    err = MIDIClientCreate(CFSTR("SynthEdit"), midi_notify_cb, NULL, &gMidiClient);

    if (err != noErr) {
        LOG_ERROR("MIDIClientCreate failed: %d\n", (int)err);
        return NULL;
    }
    err = MIDIInputPortCreate(gMidiClient, CFSTR("SynthEdit In"), midi_read_cb, NULL, &gMidiInPort);

    if (err != noErr) {
        LOG_ERROR("MIDIInputPortCreate failed: %d\n", (int)err);
        return NULL;
    }
    err = MIDIOutputPortCreate(gMidiClient, CFSTR("SynthEdit Out"), &gMidiOutPort);

    // Hand the port to the shared send primitive (synthlibMidi.h); creating and naming it stays
    // here, inside the connect logic that is deliberately not shared.
    synthlib_midi_set_out_port(gMidiOutPort);

    if (err != noErr) {
        LOG_ERROR("MIDIOutputPortCreate failed: %d\n", (int)err);
        return NULL;
    }

    while (!synthlib_quit_requested()) {
        if (atomic_exchange(&gReconnectRequested, false)) {
            // notes §34
            gDevice.connected = false;
        }

        if (!gDevice.connected) {
            gRescanNeeded = false;

            if (!synth_panel_config()->supportsIdentity) {
                // notes §35
                connect_without_identity();

                if (!gDevice.connected) {
                    // No destination visible yet — wait up to 2 s in 100 ms
                    // slices, same shape as the "nothing found" wait below.
                    for (int t = 0; t < 20 && !gRescanNeeded; t++) {
                        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.1, false);
                    }
                }
            } else {
                midi_scan_devices();
                // Pump run loop for 500 ms — collects all identity replies and
                // services any CoreMIDI notifications that arrive during the wait.
                CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.5, false);
                process_identity_replies();

                if (!gDevice.connected) {
                    // notes §36
                    for (int t = 0; t < 5 && !gRescanNeeded; t++) {
                        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.1, false);
                    }
                }
            }
        } else {
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, MIDI_IDLE_TICK_SECONDS, false);

            // Debounced state dump request — see gStateDumpDebounceTicks'
            // own comment for why this doesn't just fire immediately from
            // midi_arm_state_dump_debounce()'s callers.
            if (gStateDumpDebounceTicks > 0) {
                gStateDumpDebounceTicks--;

                if (gStateDumpDebounceTicks == 0) {
                    synth_request_state_dump();
                }
            }
        }
    }
    LOG_DEBUG("MIDI thread exiting\n");
    return NULL;
}

// ── Startup ───────────────────────────────────────────────────────────────────

int start_midi_thread(void) {
    // Before pthread_create: the CoreMIDI read callback can only fire once the input port exists,
    // which the thread itself creates, but initialising here keeps the queue's lifetime tied to the
    // thread's owner rather than to its body.
    msg_init(&gToMidiThread, "toMidiThread", sizeof(tMessageContent));

    if (pthread_create(&gMidiThread, NULL, midi_thread, NULL) != 0) {
        LOG_ERROR("pthread_create for MIDI thread failed\n");
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

void register_midi_wake_cb(void ( *cb )(void)) {
    gWakeCb = cb;
}
