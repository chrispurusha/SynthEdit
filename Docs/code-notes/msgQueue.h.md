# msgQueue.h notes

The longer comments from `msgQueue.h`, moved here 2026-09-12 so the code reads cleanly. The code points at each as `// notes §k`. Verbatim and in file order; each is titled by what it documents.

## 1. `eMsgCmd`

gToMidiThread carries work from the CoreMIDI read callback thread to the MIDI thread, which owns
gDevice / gMidiSource / gMidiDest and all scan/connect logic (see midi_request_reconnect()'s
comment in midiComms.c for the bug that ownership rule exists to prevent).

This replaces the hand-rolled gIdReplies[MAX_IDENTITY_REPLIES] buffer + gIdReplyCount that used to
do exactly this job. That buffer was a fixed-capacity queue with two problems the shared mechanism
simply doesn't have: replies past the 16th were dropped, and — because the callback bumped the
atomic count unconditionally but only stored when the index was in range — a 17th reply left the
count above the array bound, so process_identity_replies()'s `for (i = 0; i < count; i++)` read
off the end of gIdReplies. Reachable with enough devices answering one interface; the owner
already runs 4 synths on a shared TM-1.

NOT migrated, deliberately: gRescanNeeded and gReconnectRequested stay atomic flags. Both are
coalescing state ("a rescan is wanted"), not discrete events — N of them must collapse to one,
which a flag does for free and a queue would get wrong. gRescanNeeded is additionally used as a
wait-break condition inside the connect loops' 100ms slices, which is a flag's job, not a
message's. Same rule reverse-queue-design.md gives for gotPatchChangeIndication et al.
gStateDumpDebounceTicks stays a counter for the same reason — it is a debounce, not a message.
