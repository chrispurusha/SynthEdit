# midiComms.c notes

The longer comments from `midiComms.c`, moved here 2026-09-12 so the code reads cleanly. The code points at each as `// notes §k`. Verbatim and in file order; each is titled by what it documents.

## 1. `gRescanNeeded`

── Identity replies ──────────────────────────────────────────────────────────
The CoreMIDI read callback fires on the CoreMIDI thread; the MIDI thread
processes replies after a timeout.  Having the callback do nothing except
post the raw data eliminates all races with gDevice and rescan logic.

This used to be a local gIdReplies[16] array + an atomic count; it is now
SynthLib's gToMidiThread (msgQueue.h), which has no capacity limit and no
count/array skew — see msgQueue.h for the over-read that skew could cause.

## 2. `gReconnectRequested`

Set by midi_request_reconnect() (any thread — UI menu actions, sleep/wake
notifications, the Device-switch menu) and consumed only by midi_thread()
itself. gDevice/gMidiSource/gMidiDest/gToMidiThread and the connect/scan
logic are documented above (the identity-reply comment) as MIDI-thread-only —
a caller on another thread must never touch midi_scan_devices() or
gDevice.connected directly, only flag that a reconnect is wanted and let
the MIDI thread tear down/rebuild its own connection state. Found
2026-07-14: synth_switch_device_config() (synthGraphics.cpp) and two
call sites here (scanDevices:/sleep-wake, misc.mm) used to call
midi_scan_devices() straight from the main thread, racing unsynchronized
against this same state from the MIDI thread's own loop — the visible
symptom was a device switch sometimes leaving gDevice.connected stuck
true with a stale/zeroed gMidiDest, silently dropping every send until
the app was restarted (see midi_send_to()'s dest==0 short-circuit above).

## 3. `SYNTH_STATE_DUMP_DEBOUNCE_TICKS`

── State dump request debounce ─────────────────────────────────────────────
A Program Change followed immediately by a state dump request works for a
single, isolated patch change (dispatch_program_change() below already did
this safely). It falls over under a rapid burst of changes though — real
hardware capture (2026-07-07), clicking Prev/Next twice quickly: both
Program Change 13 and 14 went out, both "Re-sent device state request"
logs fired, but only ONE Panel Dump reply ever came back (for whichever
program the Voyager had settled on by the time it got around to answering
— apparently it can't/won't queue a second reply while still busy honouring
the first request). Debouncing the REQUEST side — not the Program Change
sends themselves, which should all still go out immediately and in order,
same as any other MIDI controller sending a burst of PCs — fixes this: a
burst of navigation clicks (or bank/PC messages arriving from elsewhere on
the bus) keeps resetting this counter rather than firing a request per
click, so exactly one state dump gets requested, only once, ~250ms after
the LAST change in the burst rather than after each individual one.

## 4. `midi_arm_state_dump_debounce()`

A periodic low-frequency state poll (re-request a Panel Dump every ~5s of
quiet, so no-CC dials like Headphone Volume eventually pick up a hardware
change without a manual Sync) was added and then REMOVED again 2026-07-10,
same day — real hardware testing found that ANY state dump request,
including this poll's own, kicks the Voyager's own front-panel display
OUT of whatever menu it's currently showing (e.g. browsing Sound
Category) back to normal. A poll firing every 5s while the owner is
mid-browse on the hardware itself is actively disruptive, not just
unnecessary traffic — worth remembering if this idea comes up again: it
needs to be gated on "the owner is not currently interacting with the
hardware's own front panel," which this app has no way to detect, not
just "no CC has arrived in N seconds." Manual Sync (the button, renamed
"Sync from synth" the same day) is the deliberate, user-initiated
equivalent — the owner chose to explicitly ask for it and cause the same
display kick, rather than have it happen as a surprise.

## 5. `SYSEX_BUF_SIZE`

── SysEx reassembly ──────────────────────────────────────────────────────────
8192 was plenty for a single Panel/Preset Dump (~150 bytes) but not for a
Moog-style All Presets Dump (mode 0x01 — see voyager.txt's header comment
and synth_request_all_presets_dump() in synthComms.c), all inside one
F0...F7 message with no per-preset framing to split it up. CONFIRMED
against real hardware (2026-07-07): a captured Bank backup was exactly
18734 bytes — comfortably under this buffer, with headroom to spare for a
unit with more presets than a base Voyager's single 128-location bank
(see tPanelConfig.presetBankCount's comment in panelConfig.h). A
too-small buffer would silently truncate the very backup this exists to
support (LOG_ERROR("SysEx buffer overflow...") below).

## 6. `MAX_MIDI_PARSE_SOURCES`

── Non-SysEx message state (running status), per source ──────────────────────
Channel Voice messages (Note On/Off, CC, Program Change, ...) reassemble
from running status the same way SysEx does above, but unlike SysEx this
app routinely has MULTIPLE sources connected at once —
connect_all_midi_sources() (below) wires up every visible MIDI source to
this same callback, and sharing one merged interface across several real
synths is a tested, real configuration here (see midi_scan_devices()'s own
comment: 4 real synths on one Elektron TM-1). A single shared
status/data/dataLen used to mean a Note On (or any other channel message)
from ANY connected source — not necessarily the device actually being
talked to — could silently splice into whatever running-status stream
another source was mid-way through, misparsing its next message. Found
2026-07-14 investigating a related report; each entry here is only a few
bytes, so giving every source its own costs nothing.

## 7. `channel_parse_state_for()`

Finds (or allocates) src's own running-status slot. Falls back to the LAST
slot rather than NULL if the table's ever actually full — sharing state
with whatever already occupies that slot (same failure mode this whole
mechanism exists to avoid, just narrowed to two sources instead of all of
them) is a far smaller blast radius than a crash or a silently dropped
message, and 16 simultaneously active sources is already well beyond
anything this app's been run against.

## 8. `midi_send_to()`

Returns false (and logs why) if the message couldn't be built or sent —
callers that need to know whether it actually went out (Restore,
synthBackup.c) check this rather than assuming success; every other
existing caller just discards it unchanged from before this had a return
value at all.

## 9. in `midi_send_to()`

THE PACKING AND THE SEND ARE SHARED NOW — see SynthLib's synthlibMidi.h. Both editors carried
their own copy of this, character-identical apart from a log string, and differing only in the
two ways that had already caused a real fault here: a 512-byte stack buffer that silently
failed a whole-bank restore, and no return value for the caller to notice with.

## 10. `find_dest_for_source()`

── Destination lookup ────────────────────────────────────────────────────────
Called from the MIDI thread (not the callback thread), so CoreMIDI API calls
are safe and the device list is fully settled by the time we get here.

Strategies tried in order:
```
  1. Source's own entity → entity's destinations
  2. All global destinations for entity match (catches some virtual drivers)
  3. All global destinations by display-name match (last resort)
```

## 11. `discard_queued_identity_replies()`

── Discard stale replies (MIDI thread only) ─────────────────────────────────
Called at the top of every scan, replacing the old `gIdReplyCount = 0` reset. Same purpose: a
reply left over from the previous attempt must not be matched against this one's connection
state. The queue makes the discard explicit rather than implicit in a counter reset.

## 12. in `process_identity_replies()`

Drains to empty even after a match: the queue must not carry this scan's leftovers into the
next one (the old array got the same effect by resetting gIdReplyCount at the top of every
midi_scan_devices()). `connected` makes the post-match iterations log-and-discard rather than
re-run the connect, preserving the old first-match-wins behaviour.

## 13. in `process_identity_replies()`

Full mfrId dump (not just byte 0) — this is the line to read off
when bringing up a new <device>.txt's manufacturerId/familyId/
memberId from a real device, matched or not (see sn2.txt's own
comments for how the Supernova 2's placeholders were meant to be
replaced this way).

## 14. in `process_identity_replies()`

find_dest_for_source() assumes send and receive share one physical
interface (same entity, or at least matching display names) —
true for most single-cable setups but not for a real one this
owner runs: send out one interface (e.g. an Elektron TM-1) while
the synth's own MIDI OUT comes back through an entirely different,
unrelated-by-name box (e.g. a Cirklon) sitting in the return path.
There, find_dest_for_source(src) has nothing to match (the reply's
source and the real send destination aren't the same or even
similarly-named device) and returns 0, so the app would log
"no matching destination" and never finish connecting even though
the synth genuinely just answered. cfg->midiPortName — already
used the same way by connect_without_identity() below for devices
with no identity reply at all — lets the file pin the send
destination explicitly for exactly this case; only reached for a
device that actually sets "midiPort <name>", so a normal
single-cable setup (empty midiPortName) is unaffected and still
goes through the name/entity inference below. Found 2026-07-14.
A CHOSEN INPUT: the same device answering on another port is the broadcast reaching it by
a route the user did not pick. A CHOSEN OUTPUT beats both of the inferences below - the
layout file's midiPort and the source's entity - since it is the user's own word on the
exact case both exist to guess at.

Since 2026-09-12 a matching reply from another input is not dropped: it is kept as a fallback and
taken only if nothing answers on the chosen input, and the MIDI Ports status says where it was heard
(an interface can deliver a port's input under another name - a Cirklon's "Port 2" for its MIDI 2).

## 15. `handle_identity_reply()`

── Identity reply callback handler ──────────────────────────────────────────
Runs on the CoreMIDI callback thread.  Do the absolute minimum: validate
the packet is a well-formed identity reply and store it.  All analysis
happens later in process_identity_replies() on the MIDI thread.

## 16. in `handle_identity_reply()`

F0 7E <device_id> 06 02 <mfr_id: 1 or 3 bytes> <fam_lsb> <fam_msb>
<mem_lsb> <mem_msb> ... F7 — a leading 0x00 in the mfr_id field (a
standard MIDI convention, same one <device>.txt's manufacturerId uses)
means an "extended" 3-byte ID follows rather than a classic 1-byte one;
this shifts family/member the same way a device's own manufacturerIdLen
shifts synthComms.c's per-message offsets.

## 17. `connect_all_midi_sources()`

── Source connection ─────────────────────────────────────────────────────────
Connects every currently-visible MIDI source to gMidiInPort so its data
reaches midi_read_cb(). Shared by midi_scan_devices() (which also fires off
an identity request to every destination) and connect_without_identity()
(which skips that request but still needs to hear incoming CC).

## 18. `find_destination_by_name()`

── Destination lookup by name ────────────────────────────────────────────────
For a no-identity device with "midiPort <name>" set (see panelConfig.h) —
case-insensitive substring match against each destination's display name.
Returns 0 if none matches (including when there's nothing to match against
yet, e.g. the interface hasn't enumerated over USB at startup).

## 19. `connect_without_identity()`

── Connect without an identity query ─────────────────────────────────────────
For a device whose <device>.txt sets "identityQuery no" (see panelConfig.h) —
some hardware (confirmed for Moog's Minitaur, presumably also the Voyager)
never answers a Universal Device Inquiry at all, so midi_scan_devices()'s
request would just go unanswered forever. There is no reply to correlate a
specific source/destination pair or a channel from, so this connects using
the file's own "midiChannel" directive for the channel and, if given,
"midiPort" to pick the right destination out of possibly several (e.g. an
unrelated "IAC Driver Bus 1" enumerating before the real interface) —
otherwise falls back to the first destination found, same as before
midiPort existed. Leaves gMidiSource at 0 — midi_read_cb()'s CC gate treats
that as "accept from any connected source" rather than requiring a specific
one, since there's nothing to correlate a source from either.

## 20. in `connect_without_identity()`

Ask the device to report its own current state, if the file declares
one (see the tPanelConfig field comment in panelConfig.h) — e.g. Moog's
"dump current CC values" command. The replies arrive as ordinary CC
messages through the normal dispatch_cc() path below, which is also
where the real MIDI channel gets learned from them (midiChannel above
is only ever a first guess for a device with no identity reply to read
it from).

## 21. `debug_elapsed_ms()`

Milliseconds since this function's own first call (an arbitrary but stable
reference point, not wall-clock time) — added 2026-07-08 purely to measure
real inter-message timing between a switch's own mechanical bounce and its
settled value, to size a debounce window from actual data instead of a
guess. CLOCK_MONOTONIC, not a GLFW time call, since this runs on the MIDI
thread, not the render thread.

## 22. in `dispatch_cc()`

For a device with no identity reply to read a channel from (see
"identityQuery no" in panelConfig.h), midiChannel in the file is only
ever a first guess — the device's own outgoing traffic is the real
source of truth. Once anything arrives, lock onto whatever channel it
actually used, so gDevice.id (and therefore every future midi_send_cc())
tracks the real hardware instead of a possibly-stale config value. Devices
that DO support identity already got a trustworthy channel from the
identity reply itself (handle_identity_reply()), so leave those alone.

## 23. `dispatch_program_change()`

── Program Change dispatch ─────────────────────────────────────────────────
Only called from midi_read_cb for messages arriving from the Synth's
source. A front-panel (or MIDI-driven) bank/patch change on the device
shows up here — a real MIDI monitor capture on a Voyager going from PANEL
Preset to Preset 3 was Bank Select MSB=0, Bank Select LSB=0, then Program
Change=3, in that order. Bank Select alone (CC0/CC32 — dispatch_cc()
above already receives these, there's no separate handling needed) only
sets which bank the *next* Program Change pulls from, per general MIDI
convention; the patch doesn't actually change until Program Change
arrives, so that's the one trigger point for a reload rather than acting
on Bank Select too.

## 24. in `dispatch_program_change()`

Panel Dump (or Korg's Current Program Dump) alone refreshes both
the dial positions and gDevice.progName — see panelNameOffset in
extract_moog_panel_info() (synthComms.c). No need to also chase a
Single Preset Dump by number. Debounced, not requested immediately
— see the comment above gStateDumpDebounceTicks: a burst of Bank/
Program Change messages (e.g. a footswitch stepping through
several patches) can otherwise request faster than the device can
reply to.

## 25. in `midi_read_cb()`

A second source started its own SysEx while a capture
from a DIFFERENT source is already in flight — there's
only one capture buffer (this app only ever needs to
talk to one target device at a time), so drop the
newcomer rather than clobbering whichever capture
already has a head start.

## 26. in `midi_read_cb()`

The device actually being captured from broke off its
own SysEx early — a real abort, not cross-source noise
from some other connected source's own traffic (e.g. a
MIDI keyboard sharing the same interface sending a
Note On mid-transmission — connect_all_midi_sources()
below wires up every visible source to this same
callback, and running several real synths off one
shared interface is a tested configuration here, not a
hypothetical one). Found 2026-07-14: this branch used
to fire for a status byte from ANY source, so an
unrelated device's Note On could silently drop
whatever Bank/Panel dump was mid-capture from the
device actually being talked to.

## 27. in `midi_read_cb()`

Program Change is a 2-byte message (status + 1 data
byte) — unlike CC above, only one data byte ever
arrives, so dispatch as soon as it does rather than
waiting for msgDataLen == 2 (a second byte here would
already be the next running-status message's first
data byte, not part of this one).

## 28. in `midi_read_cb()`

else: a plain data byte from a source OTHER than whoever's
SysEx capture is currently in flight — genuinely stray
(no legal interpretation without that source's own
preceding status byte), same as msgStatus==0 already
silently dropped before this per-source split existed.

## 29. in `midi_scan_devices()`

Reset reply buffer and connection-tracking state before a fresh scan.
Deliberately NOT a full memset(&gDevice, ...) — this runs every ~2.5s
from the background scan loop whenever nothing is connected (i.e.
continuously when testing standalone), and gDevice also holds the
current patch/filter values the GUI is showing/editing; wiping the
whole struct here was resetting every dial on a timer, independent of
anything the user did, which looked like it was caused by clicking.

## 30. in `midi_scan_devices()`

A CHOSEN OUTPUT is the only one asked, and one that is not plugged in is waited for rather than
replaced by whatever else is on the rig - the retry loop in midi_thread() comes back here.
Every source is still connected: the chosen input is applied to the replies, where the source
each one came from is known (process_identity_replies()).

## 31. in `midi_scan_devices()`

Small stagger between each destination's identity request —
confirmed 2026-07-13 with 4 real synths (Korg Z1, Moog Minitaur,
Waldorf Pulse, ASM Hydrasynth) sharing one MIDI interface (an
Elektron TM-1): blasting every destination's request back-to-back
with zero gap made every device's reply land at nearly the same
instant, and the Z1 specifically almost never got through cleanly
(repeated attempts, sometimes 20+ seconds) while unplugging the
other three made it connect in well under a second every time.
Not a device-matching bug — the manufacturer/family/member filter
in process_identity_replies() was already provably correct, just
never SEEING a usable Z1 reply to filter. Most likely explanation:
several devices' SysEx replies converging on the interface's
merged input at once collide/corrupt rather than interleaving
cleanly. 15 ms per destination (e.g. 60 ms total for 4 devices) is
negligible next to the multi-second retry cycle around this
function, but should measurably cut how often replies actually
collide. CFRunLoopRunInMode, not usleep/nanosleep — this thread is
CFRunLoop-driven throughout (see MIDI_IDLE_TICK_SECONDS's own
comment below), not the platform-thread model those assume.

## 32. `midi_request_reconnect()`

── Reconnect request (any thread) ──────────────────────────────────────────
See gReconnectRequested's own comment above — the only thread-safe way for
callers outside midiComms.c (Device-switch menu, Scan Devices menu, sleep/
wake notification) to ask for a fresh connection attempt. The actual
teardown/rescan still happens entirely on the MIDI thread.

## 33. `midi_set_port_scope()`

The choice is kept PER DEVICE CONFIGURATION: a Z1 and a Voyager are on different interfaces, so a
choice made for one must not follow the switch to the other. synth_reload_panel_config() calls
this whenever the configuration changes, which includes the first one at start-up - before the
MIDI thread exists, so its first scan already sees the right choice.

## 34. in `midi_thread()`

Only this thread ever writes gDevice.connected/gMidiSource/
gMidiDest (see gReconnectRequested's own comment above) — drop
the current connection here, on the MIDI thread, so the branch
below picks it straight back up this same iteration instead of
a caller elsewhere touching that state directly.

## 35. in `midi_thread()`

Some hardware (confirmed for Moog's Minitaur, presumably
also the Voyager) never answers a Universal Device Inquiry
at all — sending one and waiting would just hang forever.
Skip the poll entirely per the device's own "identityQuery
no" directive and connect directly instead.

## 36. in `midi_thread()`

Nothing found — wait up to 0.5 s in 100 ms slices before
retrying (was 2 s). Some real hardware (confirmed for a
Korg Z1, 2026-07-13) doesn't reliably answer a Universal
Device Inquiry every time it's asked — most attempts get
no reply at all within the 500 ms collection window
above, then occasionally one does, essentially a coin
flip per attempt rather than a fixed latency. With that
shape, retrying MORE often (not waiting longer per
attempt) is what actually shortens the expected time to
connect — shrinking this from 2 s to 0.5 s takes each
full attempt (0.5 s collect + this wait) from ~2.5 s
down to ~1 s, roughly 2.5x more attempts per minute, cut
a real ~20-25 s connect time down substantially without
touching the 500 ms collection window fast-responding
devices already rely on. Wakes early if a setup-change
notification fires (via gRescanNeeded).
