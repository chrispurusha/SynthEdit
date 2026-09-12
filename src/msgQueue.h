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
// Notes: Docs/code-notes/msgQueue.h.md - "// notes §k" refers there.

#ifndef __MSG_QUEUE_H__
#define __MSG_QUEUE_H__

#include "sysIncludes.h"
#include "synthlibQueue.h" // generic queue mechanism: tMessageQueue / eRcv / msg_init / msg_send / ...

// notes §1
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
