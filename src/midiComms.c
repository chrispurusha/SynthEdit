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

#include "sysIncludes.h"
#include "defs.h"
#include "synthlibDefs.h"
#include "types.h"
#include "globalVars.h"
#include "synthComms.h"
#include "synthGraphics.h"
#include "midiComms.h"

static void             (*gWakeCb)(void) = NULL;
static pthread_t        gMidiThread             = 0;
static pthread_mutex_t  gSendMutex              = PTHREAD_MUTEX_INITIALIZER;

// ── Identity reply buffer ─────────────────────────────────────────────────────
// The CoreMIDI read callback fires on the CoreMIDI thread; the MIDI thread
// processes replies after a timeout.  Having the callback do nothing except
// store data eliminates all races with gDevice and rescan logic.
#define MAX_IDENTITY_REPLIES    16

static struct {
    MIDIEndpointRef src;
    uint8_t         deviceId;    // data[2]
    uint8_t         mfrId[3];    // data[5..] — 1 or 3 bytes, see mfrIdLen
    uint32_t        mfrIdLen;    // 1 (classic) or 3 (extended, data[5]==0x00)
    uint8_t         familyLSB;   // data[5+mfrIdLen]
    uint8_t         memberLSB;   // data[5+mfrIdLen+2]
}                       gIdReplies[MAX_IDENTITY_REPLIES];

static _Atomic uint32_t gIdReplyCount           = 0;

// Notification from notify thread; polled by the MIDI thread.
static _Atomic bool     gRescanNeeded           = false;

// ── State dump request debounce ─────────────────────────────────────────────
// A Program Change followed immediately by a state dump request works for a
// single, isolated patch change (dispatch_program_change() below already did
// this safely). It falls over under a rapid burst of changes though — real
// hardware capture (2026-07-07), clicking Prev/Next twice quickly: both
// Program Change 13 and 14 went out, both "Re-sent device state request"
// logs fired, but only ONE Panel Dump reply ever came back (for whichever
// program the Voyager had settled on by the time it got around to answering
// — apparently it can't/won't queue a second reply while still busy honouring
// the first request). Debouncing the REQUEST side — not the Program Change
// sends themselves, which should all still go out immediately and in order,
// same as any other MIDI controller sending a burst of PCs — fixes this: a
// burst of navigation clicks (or bank/PC messages arriving from elsewhere on
// the bus) keeps resetting this counter rather than firing a request per
// click, so exactly one state dump gets requested, only once, ~250ms after
// the LAST change in the burst rather than after each individual one.
#define SYNTH_STATE_DUMP_DEBOUNCE_TICKS    8 // * MIDI_IDLE_TICK_SECONDS below ~= 264ms
#define MIDI_IDLE_TICK_SECONDS             0.033
static _Atomic int      gStateDumpDebounceTicks = 0;

// A periodic low-frequency state poll (re-request a Panel Dump every ~5s of
// quiet, so no-CC dials like Headphone Volume eventually pick up a hardware
// change without a manual Sync) was added and then REMOVED again 2026-07-10,
// same day — real hardware testing found that ANY state dump request,
// including this poll's own, kicks the Voyager's own front-panel display
// OUT of whatever menu it's currently showing (e.g. browsing Sound
// Category) back to normal. A poll firing every 5s while the owner is
// mid-browse on the hardware itself is actively disruptive, not just
// unnecessary traffic — worth remembering if this idea comes up again: it
// needs to be gated on "the owner is not currently interacting with the
// hardware's own front panel," which this app has no way to detect, not
// just "no CC has arrived in N seconds." Manual Sync (the button, renamed
// "Sync from synth" the same day) is the deliberate, user-initiated
// equivalent — the owner chose to explicitly ask for it and cause the same
// display kick, rather than have it happen as a surprise.

void midi_arm_state_dump_debounce(void) {
    gStateDumpDebounceTicks = SYNTH_STATE_DUMP_DEBOUNCE_TICKS;
}

// ── SysEx reassembly ──────────────────────────────────────────────────────────
// 8192 was plenty for a single Panel/Preset Dump (~150 bytes) but not for a
// Moog-style All Presets Dump (mode 0x01 — see voyager.txt's header comment
// and synth_request_all_presets_dump() in synthComms.c), all inside one
// F0...F7 message with no per-preset framing to split it up. CONFIRMED
// against real hardware (2026-07-07): a captured Bank backup was exactly
// 18734 bytes — comfortably under this buffer, with headroom to spare for a
// unit with more presets than a base Voyager's single 128-location bank
// (see tPanelConfig.presetBankCount's comment in panelConfig.h). A
// too-small buffer would silently truncate the very backup this exists to
// support (LOG_ERROR("SysEx buffer overflow...") below).
#define SYSEX_BUF_SIZE    65536
static uint8_t          gSysExBuf[SYSEX_BUF_SIZE];
static uint32_t         gSysExLen               = 0;
static MIDIEndpointRef  gSysExSrc               = 0;

// ── Non-SysEx message state (running status) ──────────────────────────────────
static uint8_t          gMsgStatus              = 0;
static uint8_t          gMsgData[2];
static uint8_t          gMsgDataLen             = 0;

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
    OSStatus         err     = MIDISend(gMidiOutPort, dest, pktList);
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

// ── Process buffered identity replies (MIDI thread only) ──────────────────────
// Scans the reply buffer collected since the last scan, selects the first synth
// and calls synth_on_connected().  All CoreMIDI lookups happen here — no races.

static void process_identity_replies(void) {
    uint32_t count = gIdReplyCount;

    LOG_DEBUG("Processing %u identity replies\n", (unsigned)count);

    for (uint32_t i = 0; i < count; i++) {
        // Full mfrId dump (not just byte 0) — this is the line to read off
        // when bringing up a new <device>.txt's manufacturerId/familyId/
        // memberId from a real device, matched or not (see sn2.txt's own
        // comments for how the Supernova 2's placeholders were meant to be
        // replaced this way).
        LOG_DEBUG("  reply[%u]: mfrLen=%u mfr=%02X:%02X:%02X fam=0x%02X mem=0x%02X src=0x%08X\n",
                  (unsigned)i,
                  (unsigned)gIdReplies[i].mfrIdLen,
                  gIdReplies[i].mfrId[0],
                  gIdReplies[i].mfrId[1],
                  gIdReplies[i].mfrId[2],
                  gIdReplies[i].familyLSB,
                  gIdReplies[i].memberLSB,
                  (unsigned)gIdReplies[i].src);

        tPanelConfig *  cfg  = synth_panel_config();

        if (  (gIdReplies[i].mfrIdLen != cfg->manufacturerIdLen)
           || (memcmp(gIdReplies[i].mfrId, cfg->manufacturerId, cfg->manufacturerIdLen) != 0)
           || (gIdReplies[i].familyLSB != cfg->familyId)
           || (gIdReplies[i].memberLSB != cfg->memberId)) {
            continue;
        }
        MIDIEndpointRef src  = gIdReplies[i].src;
        MIDIEndpointRef dest = find_dest_for_source(src);

        if (dest == 0) {
            LOG_ERROR("Synth found but no matching destination for src=0x%08X\n", (unsigned)src);
            continue;
        }
        gDevice.id        = gIdReplies[i].deviceId;
        gDevice.family    = (uint16_t)gIdReplies[i].familyLSB;
        gDevice.member    = (uint16_t)gIdReplies[i].memberLSB;
        gDevice.connected = true;
        gMidiSource       = src;
        gMidiDest         = dest;

        LOG_DEBUG("Synth connected: deviceId=0x%02X src=0x%08X dest=0x%08X\n",
                  gDevice.id, (unsigned)src, (unsigned)dest);

        synth_on_connected();

        if (gWakeCb != NULL) {
            gWakeCb();
        }
        return;
    }

    LOG_DEBUG("No Synth found in this batch of identity replies\n");
}

// ── Identity reply callback handler ──────────────────────────────────────────
// Runs on the CoreMIDI callback thread.  Do the absolute minimum: validate
// the packet is a well-formed identity reply and store it.  All analysis
// happens later in process_identity_replies() on the MIDI thread.

static void handle_identity_reply(MIDIEndpointRef src, const uint8_t * data, uint32_t length) {
    // F0 7E <device_id> 06 02 <mfr_id: 1 or 3 bytes> <fam_lsb> <fam_msb>
    // <mem_lsb> <mem_msb> ... F7 — a leading 0x00 in the mfr_id field (a
    // standard MIDI convention, same one <device>.txt's manufacturerId uses)
    // means an "extended" 3-byte ID follows rather than a classic 1-byte one;
    // this shifts family/member the same way a device's own manufacturerIdLen
    // shifts synthComms.c's per-message offsets.
    if (length < 10) {
        return;
    }
    uint32_t mfrLen = (data[5] == 0x00) ? 3 : 1;

    if (length < (uint32_t)(5 + mfrLen + 4)) {
        return;
    }
    uint32_t idx    = atomic_fetch_add(&gIdReplyCount, 1);

    if (idx < MAX_IDENTITY_REPLIES) {
        gIdReplies[idx].src       = src;
        gIdReplies[idx].deviceId  = data[2];
        gIdReplies[idx].mfrIdLen  = mfrLen;
        memset(gIdReplies[idx].mfrId, 0, sizeof(gIdReplies[idx].mfrId)); // clean display for the mfrLen==1 case — see process_identity_replies()'s log line
        memcpy(gIdReplies[idx].mfrId, &data[5], mfrLen);
        gIdReplies[idx].familyLSB = data[5 + mfrLen];
        gIdReplies[idx].memberLSB = data[5 + mfrLen + 2];
    }
}

// ── Source connection ─────────────────────────────────────────────────────────
// Connects every currently-visible MIDI source to gMidiInPort so its data
// reaches midi_read_cb(). Shared by midi_scan_devices() (which also fires off
// an identity request to every destination) and connect_without_identity()
// (which skips that request but still needs to hear incoming CC).

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

// ── Destination lookup by name ────────────────────────────────────────────────
// For a no-identity device with "midiPort <name>" set (see panelConfig.h) —
// case-insensitive substring match against each destination's display name.
// Returns 0 if none matches (including when there's nothing to match against
// yet, e.g. the interface hasn't enumerated over USB at startup).

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

// ── Connect without an identity query ─────────────────────────────────────────
// For a device whose <device>.txt sets "identityQuery no" (see panelConfig.h) —
// some hardware (confirmed for Moog's Minitaur, presumably also the Voyager)
// never answers a Universal Device Inquiry at all, so midi_scan_devices()'s
// request would just go unanswered forever. There is no reply to correlate a
// specific source/destination pair or a channel from, so this connects using
// the file's own "midiChannel" directive for the channel and, if given,
// "midiPort" to pick the right destination out of possibly several (e.g. an
// unrelated "IAC Driver Bus 1" enumerating before the real interface) —
// otherwise falls back to the first destination found, same as before
// midiPort existed. Leaves gMidiSource at 0 — midi_read_cb()'s CC gate treats
// that as "accept from any connected source" rather than requiring a specific
// one, since there's nothing to correlate a source from either.

static void connect_without_identity(void) {
    ItemCount       destCount = MIDIGetNumberOfDestinations();

    if (destCount == 0) {
        return; // nothing to send to yet — caller retries after a short wait
    }
    tPanelConfig *  cfg       = synth_panel_config();
    MIDIEndpointRef dest      = 0;

    if (cfg->midiPortName[0] != '\0') {
        dest = find_destination_by_name(cfg->midiPortName);

        if (dest == 0) {
            return; // named port not visible yet — caller retries after a short wait
        }
    } else {
        dest = MIDIGetDestination(0);
    }
    connect_all_midi_sources();

    gMidiDest         = dest;
    gMidiSource       = 0;
    gDevice.id        = (uint8_t)((cfg->midiChannel > 0) ? (cfg->midiChannel - 1) : 0);
    gDevice.family    = 0;
    gDevice.member    = 0;
    gDevice.connected = true;

    LOG_DEBUG("Synth connected without identity query: channel=%u dest=0x%08X\n",
              (unsigned)(gDevice.id + 1), (unsigned)gMidiDest);

    synth_on_connected();

    // Ask the device to report its own current state, if the file declares
    // one (see the tPanelConfig field comment in panelConfig.h) — e.g. Moog's
    // "dump current CC values" command. The replies arrive as ordinary CC
    // messages through the normal dispatch_cc() path below, which is also
    // where the real MIDI channel gets learned from them (midiChannel above
    // is only ever a first guess for a device with no identity reply to read
    // it from).
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

// Milliseconds since this function's own first call (an arbitrary but stable
// reference point, not wall-clock time) — added 2026-07-08 purely to measure
// real inter-message timing between a switch's own mechanical bounce and its
// settled value, to size a debounce window from actual data instead of a
// guess. CLOCK_MONOTONIC, not a GLFW time call, since this runs on the MIDI
// thread, not the render thread.
static double debug_elapsed_ms(void) {
    static struct timespec start          = {0};
    static bool            startCaptured  = false;
    struct timespec        now;

    clock_gettime(CLOCK_MONOTONIC, &now);

    if (!startCaptured) {
        start         = now;
        startCaptured = true;
    }
    double seconds = (double)(now.tv_sec - start.tv_sec) + (double)(now.tv_nsec - start.tv_nsec) / 1e9;

    return seconds * 1000.0;
}

static void dispatch_cc(uint8_t channel, uint8_t cc, uint8_t value) {
    LOG_DEBUG("[%.1fms] CC ch=%u 0x%02X val=%u\n", debug_elapsed_ms(),
              (unsigned)(channel + 1), (unsigned)cc, (unsigned)value);

    // For a device with no identity reply to read a channel from (see
    // "identityQuery no" in panelConfig.h), midiChannel in the file is only
    // ever a first guess — the device's own outgoing traffic is the real
    // source of truth. Once anything arrives, lock onto whatever channel it
    // actually used, so gDevice.id (and therefore every future midi_send_cc())
    // tracks the real hardware instead of a possibly-stale config value. Devices
    // that DO support identity already got a trustworthy channel from the
    // identity reply itself (handle_identity_reply()), so leave those alone.
    if (!synth_panel_config()->supportsIdentity && (gDevice.id != channel)) {
        LOG_DEBUG("Auto-detected device MIDI channel: %u (was %u)\n",
                  (unsigned)(channel + 1), (unsigned)(gDevice.id + 1));
        gDevice.id = channel;
    }
    // Generic: whichever dial (if any) has this cc= in the device's own
    // <device>.txt gets the value — no per-device CC list here.
    bool handled = synth_handle_cc(cc, value);

    if (handled) {
        gReDraw = true;

        if (gWakeCb != NULL) {
            gWakeCb();
        }
    }
}

// ── Program Change dispatch ─────────────────────────────────────────────────
// Only called from midi_read_cb for messages arriving from the Synth's
// source. A front-panel (or MIDI-driven) bank/patch change on the device
// shows up here — a real MIDI monitor capture on a Voyager going from PANEL
// Preset to Preset 3 was Bank Select MSB=0, Bank Select LSB=0, then Program
// Change=3, in that order. Bank Select alone (CC0/CC32 — dispatch_cc()
// above already receives these, there's no separate handling needed) only
// sets which bank the *next* Program Change pulls from, per general MIDI
// convention; the patch doesn't actually change until Program Change
// arrives, so that's the one trigger point for a reload rather than acting
// on Bank Select too.
static void dispatch_program_change(uint8_t channel, uint8_t program) {
    LOG_DEBUG("Program Change ch=%u program=%u — reloading current state\n",
              (unsigned)(channel + 1), (unsigned)program);

    if (!synth_panel_config()->supportsIdentity && (gDevice.id != channel)) {
        gDevice.id = channel;
    }
    gDevice.currentProgram = program; // see the tSynthDevice field comment in types.h — this is the only way it's ever learned

    if (gDevice.connected) {
        // Panel Dump (or Korg's Current Program Dump) alone refreshes both
        // the dial positions and gDevice.progName — see panelNameOffset in
        // extract_moog_panel_info() (synthComms.c). No need to also chase a
        // Single Preset Dump by number. Debounced, not requested immediately
        // — see the comment above gStateDumpDebounceTicks: a burst of Bank/
        // Program Change messages (e.g. a footswitch stepping through
        // several patches) can otherwise request faster than the device can
        // reply to.
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
                gMsgStatus  = byte;
                gMsgDataLen = 0;
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
                        // gMidiSource == 0 means "accept from any source" —
                        // set by connect_without_identity() for a device with
                        // no identity reply to correlate a specific one from.
                        if (gDevice.connected && ((gMidiSource == 0) || (src == gMidiSource))) {
                            dispatch_cc((uint8_t)(gMsgStatus & 0x0F), gMsgData[0], gMsgData[1]);
                        }
                        gMsgDataLen = 0;    // ready for running status
                    }

                    // Program Change is a 2-byte message (status + 1 data
                    // byte) — unlike CC above, only one data byte ever
                    // arrives, so dispatch as soon as it does rather than
                    // waiting for gMsgDataLen == 2 (a second byte here would
                    // already be the next running-status message's first
                    // data byte, not part of this one).
                    if (((gMsgStatus & 0xF0) == 0xC0) && (gMsgDataLen == 1)) {
                        if (gDevice.connected && ((gMidiSource == 0) || (src == gMidiSource))) {
                            dispatch_program_change((uint8_t)(gMsgStatus & 0x0F), gMsgData[0]);
                        }
                        gMsgDataLen = 0;
                    }
                }
            }
        }

        pkt = MIDIPacketNext(pkt);
    }
}

// ── Device scanning ───────────────────────────────────────────────────────────

int midi_scan_devices(void) {
    static const uint8_t idReq[]   = {
        MIDI_SYSEX_START,
        MIDI_NON_REALTIME,
        MIDI_DEVICE_INQUIRY,
        MIDI_IDENTITY_REQUEST_SUB1,
        MIDI_IDENTITY_REQUEST_SUB2,
        MIDI_SYSEX_END
    };

    ItemCount            destCount = MIDIGetNumberOfDestinations();

    // Reset reply buffer and connection-tracking state before a fresh scan.
    // Deliberately NOT a full memset(&gDevice, ...) — this runs every ~2.5s
    // from the background scan loop whenever nothing is connected (i.e.
    // continuously when testing standalone), and gDevice also holds the
    // current patch/filter values the GUI is showing/editing; wiping the
    // whole struct here was resetting every dial on a timer, independent of
    // anything the user did, which looked like it was caused by clicking.
    gIdReplyCount     = 0;
    gMidiSource       = 0;
    gMidiDest         = 0;
    gDevice.connected = false;
    gDevice.id        = 0;
    gDevice.family    = 0;
    gDevice.member    = 0;

    connect_all_midi_sources();

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

    if ((MIDIGetNumberOfSources() > 0) && (destCount > 0)) {
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

    if (err != noErr) {
        LOG_ERROR("MIDIOutputPortCreate failed: %d\n", (int)err);
        return NULL;
    }

    while (!gQuitAll) {
        if (!gDevice.connected) {
            gRescanNeeded = false;

            if (!synth_panel_config()->supportsIdentity) {
                // Some hardware (confirmed for Moog's Minitaur, presumably
                // also the Voyager) never answers a Universal Device Inquiry
                // at all — sending one and waiting would just hang forever.
                // Skip the poll entirely per the device's own "identityQuery
                // no" directive and connect directly instead.
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
                    // Nothing found — wait up to 2 s in 100 ms slices.  Wakes early
                    // if a setup-change notification fires (via gRescanNeeded).
                    for (int t = 0; t < 20 && !gRescanNeeded; t++) {
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
    if (pthread_create(&gMidiThread, NULL, midi_thread, NULL) != 0) {
        LOG_ERROR("pthread_create for MIDI thread failed\n");
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

void register_midi_wake_cb(void ( *cb )(void)) {
    gWakeCb = cb;
}
