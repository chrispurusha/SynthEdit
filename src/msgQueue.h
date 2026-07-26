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

#ifndef __MSG_QUEUE_H__
#define __MSG_QUEUE_H__

#include "sysIncludes.h"
#include "synthlibQueue.h" // generic queue mechanism: tMessageQueue / eRcv / msg_init / msg_send / ...

// gToMidiThread carries work from the CoreMIDI read callback thread to the MIDI thread, which owns
// gDevice / gMidiSource / gMidiDest and all scan/connect logic (see midi_request_reconnect()'s
// comment in midiComms.c for the bug that ownership rule exists to prevent).
//
// This replaces the hand-rolled gIdReplies[MAX_IDENTITY_REPLIES] buffer + gIdReplyCount that used to
// do exactly this job. That buffer was a fixed-capacity queue with two problems the shared mechanism
// simply doesn't have: replies past the 16th were dropped, and — because the callback bumped the
// atomic count unconditionally but only stored when the index was in range — a 17th reply left the
// count above the array bound, so process_identity_replies()'s `for (i = 0; i < count; i++)` read
// off the end of gIdReplies. Reachable with enough devices answering one interface; the owner
// already runs 4 synths on a shared TM-1.
//
// NOT migrated, deliberately: gRescanNeeded and gReconnectRequested stay atomic flags. Both are
// coalescing state ("a rescan is wanted"), not discrete events — N of them must collapse to one,
// which a flag does for free and a queue would get wrong. gRescanNeeded is additionally used as a
// wait-break condition inside the connect loops' 100ms slices, which is a flag's job, not a
// message's. Same rule reverse-queue-design.md gives for gotPatchChangeIndication et al.
// gStateDumpDebounceTicks stays a counter for the same reason — it is a debounce, not a message.
typedef enum {
    eMsgCmdIdentityReply // identityReplyData
} eMsgCmd;

// Raw, unanalysed identity-reply fields. The callback thread does nothing but validate framing and
// copy these out; all CoreMIDI lookups and all matching against the current tPanelConfig happen on
// the MIDI thread in process_identity_replies().
typedef struct {
    uint32_t source;     // MIDIEndpointRef the reply arrived on
    uint8_t  deviceId;   // data[2]
    uint8_t  mfrId[3];   // data[5..] — 1 or 3 bytes, see mfrIdLen
    uint32_t mfrIdLen;   // 1 (classic) or 3 (extended, data[5] == 0x00)
    uint8_t  familyLSB;  // data[5 + mfrIdLen]
    uint8_t  memberLSB;  // data[5 + mfrIdLen + 2]
} tIdentityReplyData;

typedef struct {
    uint32_t cmd;
    union {
        tIdentityReplyData identityReplyData;
    };
} tMessageContent;

#endif // __MSG_QUEUE_H__
