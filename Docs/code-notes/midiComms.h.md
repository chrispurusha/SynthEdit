# midiComms.h notes

The longer comments from `midiComms.h`, moved here 2026-09-12 so the code reads cleanly. The code points at each as `// notes §k`. Verbatim and in file order; each is titled by what it documents.

## 1. `midi_request_reconnect()`

Requests a fresh connection attempt from any thread — the Device-switch
menu, the Scan Devices menu action, and the sleep/wake notification
handler all use this instead of touching gDevice/gMidiSource/gMidiDest or
the (now file-local) scan/connect logic directly, which used to race
unsynchronized against the MIDI thread's own ownership of that state.

## 2. `midi_send()`

Returns false (and logs why) if the message couldn't be sent — e.g. too
large for the internal packet-list buffer (SYSEX_BUF_SIZE, midiComms.c —
a whole-bank restore is the one message in this app big enough to hit
that) or CoreMIDI itself rejected it. Every other existing caller (CC/
parameter-change sends, dial patch-and-resend, etc.) predates this
return value and still compiles fine ignoring it; Restore (synthBackup.c)
is what actually checks it, added 2026-07-11 after a bank restore logged
"sent" despite MIDIPacketListAdd having silently failed on the old
512-byte buffer.

## 3. `midi_arm_state_dump_debounce()`

Arms (or re-arms, restarting the countdown) a debounced "request a fresh
state dump" — fires exactly once, ~SYNTH_STATE_DUMP_DEBOUNCE_TICKS *
MIDI_IDLE_TICK_SECONDS after the last call, from the MIDI thread's own
idle loop (see midi_thread() in midiComms.c). Callers: dispatch_program_change()
(a Bank/Program Change arriving from elsewhere on the bus) and
synth_navigate_preset() (synthComms.c, the Prev/Next patch buttons) —
both used to call synth_request_state_dump() directly, which real
hardware couldn't always keep up with under a rapid burst of changes (see
the comment above gStateDumpDebounceTicks' definition for the capture
that showed this).
