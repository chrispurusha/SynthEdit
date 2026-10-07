# panelConfig.h notes

The longer comments from `panelConfig.h`, moved here 2026-09-12 so the code reads cleanly. The code points at each as `// notes §k`. Verbatim and in file order; each is titled by what it documents.

## 1. file scope

Generic parser for the panel-descriptor text format used by xxxx.txt (and,
eventually, other <device>.txt files). Deliberately has no application-
specific dependencies — candidate for promotion into SynthLib once a
second project wants it.

## 2. file scope

"display=hiLo" — for a dial whose raw dump bits are a single 16-bit
TWO'S-COMPLEMENT signed value that a real front panel splits into two
separately-adjustable coarse/fine controls sharing one storage word
(Voyager's PGM Shaping 1/2 "Fixed Value" HIGH/LOW, confirmed
2026-07-11/12: signed_raw = 512 + HIGH*1024 + LOW*8, HIGH/LOW both
-64..+63, with LOW overflowing carrying into HIGH exactly like a
coarse/fine odometer pair — see synth_decode_hilo_dial() in
synthGraphics.cpp for the derivation. Genuinely NOT a plain bit-slice:
confirmed 2026-07-12 that the wire bits alone cannot always
distinguish HIGH from HIGH+64 — this isn't a narrow boundary case,
EVERY HIGH in -64..-1 is bit-for-bit identical on the wire to HIGH+64
(0..63) for the same LOW. The device's own front panel apparently
tracks HIGH/LOW as separate UI state that a dump can't always fully
recover. synth_decode_hilo_dial() picks the plain two's-complement
reading as a deterministic, self-consistent display convention —
accepted by the owner as a known display-only limitation (the actual
value sent to the device is always correct regardless of which
labeling is shown; only the on-screen HIGH number can disagree with
the real hardware's own screen when the true HIGH is negative). The
dial's own dumpOffset/dumpBitWidth still reads
the raw 16-bit word normally via the existing generic bitfield
extraction (no engine change needed there) — this display mode only
changes how that raw value is FORMATTED ("High: -2  Low: +63") and
how a drag maps back to storage (see mouseHandle.c). Generic, not
Voyager-specific — any device with a real hi/lo coarse+fine pair
sharing one signed dump word can use it the same way; the specific
512/1024/8 constants are declared per-dial via
hiLoOffset=/hiLoCoarseScale=/hiLoFineScale= (no hidden defaults —
every dial using this display mode must declare all three
explicitly, same "no magic numbers in generic code" reasoning as
dumpNativeMax/nativeMax already follow).

## 3. file scope

"display=note" — this dial's raw value IS a MIDI note number (0-127)
rather than an arbitrary quantity, so it's shown as a note name
(C-1..G9, note 0 = C-1, matching the synth's own front-panel
convention) instead of a bare integer. Added 2026-07-13 for the Z1's
Filter Lo/Hi Key keyboard-track boundaries (f1lowkey/f1highkey and
the same shape ×3 more — f1b/f2/f2b, see linkedMaxDialId/
linkedMinDialId above), whose CC in/out were confirmed already
correct — this only changes on-screen FORMATTING, nothing sent or
received on the wire (dial otherwise behaves exactly like a plain
max=128 dialDisplayRaw dial).

## 4. file scope

"display=signed" — this dial's raw wire/dump value is an unsigned
0..max-1 count that represents a SIGNED quantity centred somewhere in
that range on the real front panel (e.g. the Z1's Filter Lo/Hi
Int/ModEG Int/Mod1 Int/Mod2 Int/Res Mod Int family: raw 0-198 on the
wire, shown as -99..+99 on the synth's own display). Shown as
`(int)dialVal - displayOffset` rather than the bare raw count.
displayOffset must be declared explicitly per-dial (no hidden
default, same "no magic numbers in generic code" reasoning as
dialDisplaySignedHiLo's hiLoOffset/scale attributes) — purely a
display-time subtraction, doesn't touch storageOffset or anything
sent/received on the wire.

## 5. `tColumnLabel`

A title printed above one grid column (e.g. "Osc 1", "Mixer") —
"columnLabel <col> <text>" in the device's own .txt, one line per
labelled column. Page-scoped, not global: col=/row= positions are only
unique within a single page (see tPanelDial.gridCol's own comment), so
"column 3" on one page and "column 3" on another can have entirely
different labels, or only one of them labelled at all. Purely cosmetic —
synth_render() (synthGraphics.cpp) reserves a header row above a grid
page's dials only if the page has at least one of these; a page with
none renders exactly as it did before this existed.

## 6. file scope

"linkedMaxDial=<id>"/"linkedMinDial=<id>" — this dial's value can
never be set (from ANY source that goes through synth_set_panel_
dial_value(): drag, value-menu click, or a programmatic set) above/
below the NAMED dial's own CURRENT value. Added 2026-07-13 for the
Z1's Filter keyboard-track Lo/Hi Key pairs (f1lowkey/f1highkey and
the same shape ×3 more — f1b/f2/f2b): real hardware enforces Lo Key
<= Hi Key itself, silently clamping whichever one crosses — owner
wanted the app to enforce the SAME constraint locally rather than
let an invalid combination get sent at all and rely on a later Sync
to reveal the hardware's own correction. Deliberately NOT applied on
the DECODE side (apply_dial_wire_value()/extract_prog_info() etc.) —
an incoming dump/CC is already the hardware's own resolved truth,
nothing to re-clamp there. Resolved by id via find_panel_dial_
anywhere() at the moment of each value-set (not cached, so it always
reflects whichever value the linked dial most recently held) — empty
string (the default) means no constraint, so this is a no-op for
every dial in every OTHER device file.

## 7. file scope

"disableUnless=<dialId>:<value>" — this dial is greyed out and takes
no interaction (same as readOnly — see arm_dial_press() in
mouseHandle.c) unless the NAMED dial's own CURRENT display value
equals <value>. Added 2026-07-13 per the Z1 Owner's Manual (p.52/53):
Filter 2's own controls are only real when Filter 1&2 Link is OFF
("When this is ON, filter 2 settings cannot be made" — the manual's
own words), and Filter-B's controls only matter when that filter's
own Type is 2BPF ("If 2BPF is selected, the parameters explained in
'Filter B settings...' will be displayed"). Purely a UI convenience —
deliberately NOT a write-side block in synth_set_panel_dial_value()
(a disabled dial simply can't be dragged/clicked in the first place,
per arm_dial_press(), so there's nothing left to guard there) and NOT
a substitute for reading back a full dump to confirm a value actually
took — it just stops the user from attempting an edit the real
hardware would ignore anyway, rather than the app silently sending
something the synth won't apply. Resolved by id via find_panel_dial_
anywhere() at render/hit-test time (not cached), same pattern as
linkedMaxDialId/linkedMinDialId above — empty string (the default)
means always-enabled, a no-op for every dial in every OTHER device
file. See panel_dial_is_disabled() below.

## 8. file scope

Protocol wiring — describes how to read/write/send this control's value,
so generic code (mouse handling, rendering) never needs to know what a
given dial *means*. All parsed from the file. The dial owns its own
live value/nativeValue directly (see get_panel_dial_value() etc.) —
there is no separate application-side struct/binding step for any of
this, which is what makes a new device just a new <device>.txt file.

## 9. file scope

"wireSigned=1" — true if a live Parameter Change (group=/param=) value
for THIS dial is genuine 14-bit two's-complement signed on the wire,
NOT the plain unsigned-count-plus-displayOffset scheme dialDisplaySigned's
own comment describes (and which the Z1's Filter/EG/Amp/LFO "Int"
family, -99..+99, was confirmed to use via SET+SYNC+compare). Found
2026-07-14 on real hardware for PB Int+/PB Int- (param 163/164):
turning the real dial to a genuine "-2 semitones" state sent
value=16382 (16384-2, i.e. -2 in 14-bit two's complement) — decoding
that as a plain unsigned count and clamping into [0, max-1] pinned the
dial at its max, not anywhere near the correct value. A genuine
positive value (+2 semitones -> value=2) passes through unchanged
either way, which is why this can look fine until something actually
goes negative. When true: handle_parameter_change() sign-extends the
incoming 14-bit value (subtract 16384 if >= 8192) before adding
displayOffset and handing off to apply_dial_wire_value(), and
synth_set_panel_dial_value()'s outgoing send does the inverse (subtract
displayOffset, re-encode as 14-bit two's complement if negative) —
see encode_signed_param_wire_value(), synthComms.c. Default false:
every dial that doesn't set this keeps exactly today's unsigned
behaviour. NOT yet verified for any OTHER "-N~+N" dial in z1.txt
(Semi Tone/Fine Tune/Frequency Offset were added "by analogy" the same
day as PB Int and might have the identical bug — untested) — only
enable this where it's actually been hardware-confirmed.

## 10. file scope

Kronos-style Parameter Change addressing (func 0x43) — an entirely
different, richer scheme from paramGroup/paramId above (Z1's own 2-
field group/paramId): TYP/SOC/SUB/PID/IDX, per KRONOS_MIDI_SysEx.txt
("Most of the instrument's parameters... can be edited with this
kind of message"). Set via the file's own "typ=/soc=/sub=/pid=/idx="
dial attributes (kronos.txt) — see synth_send_kronos_parameter_change()'s
own comment, synthComms.c, including how far this got confirmed
against real hardware as of 2026-07-14 (sent successfully, effect
unconfirmed — this dial exists so the owner can test it directly on
the real front panel rather than guessing at it blind). hasKronosParam
distinguishes "these are all legitimately 0" from "not a Kronos-param
dial at all" — unlike paramId==0 (never a real Z1 value), TYP/SOC/
SUB/PID/IDX genuinely can all be 0 for a real parameter.

## 11. file scope

MIDI's own 14-bit CC convention: controller N (0-31) carries the coarse/
MSB half, N+32 the fine/LSB half — combined value = (msb<<7)|lsb, giving
0..16383 instead of a plain CC's 0..127. 0 = ccNumber alone is a plain
7-bit CC (default; existing single-cc dials are unaffected). Set via the
file's "ccLsb=" dial attribute alongside "cc=".

## 12. file scope

Debounce for a live CC-driven quantized switch/selector (nativeMax != 0
&& display == dialDisplayNames) — a real detented rotary/toggle switch
was confirmed 2026-07-08 (Voyager's LFO Sync, watching timestamped
dispatch_cc() output) to send several transitional raw CC bytes within
20-60ms of each other as its wiper physically clicks between detents,
before settling — applying each one immediately made the on-screen
value visibly flicker through intermediate/wrong positions on every
switch flip. hasPendingCc/pendingRawValue/pendingSinceMs let
synth_handle_cc() (synthComms.c) hold the latest raw byte without
applying it, resetting the timestamp on every new CC for this dial;
synth_flush_pending_cc() (called once per frame from the render loop)
only commits it once CC_DEBOUNCE_MS have passed with no further
message. A plain continuous dial (nativeMax == 0) is untouched by any
of this — its live CC stream is a genuine real-time sweep, not bounce,
so it still applies immediately.

## 13. file scope

Debounce for the OUTGOING side of a dump-only dial (no CC at all,
dumpBitWidth > 0 — e.g. Voyager's Headphone Volume/Filter Pole Select).
synth_set_panel_dial_value() (synthComms.c) used to call
synth_patch_and_resend_moog_dump() immediately on every value change —
fine for a click-through selector, but a mouse-dragged continuous dial
(see mouseHandle.c) can call that many times a second while dragging,
each one resending the ENTIRE ~147-byte cached dump (unlike a plain CC
send, 3 bytes). hasPendingDumpSend/pendingDumpRawValue/
pendingDumpSinceMs hold the latest value without sending, resetting
the timestamp on every further change; synth_flush_pending_dump_sends()
(called once per frame, same as synth_flush_pending_cc()) waits until
CC_DEBOUNCE_MS have passed with no further change, THEN moves on to
the second phase below (dumpSendAwaitingFreshData) rather than sending
directly — same trailing-edge idiom as hasPendingCc above for settling
the value, just followed by an extra fetch-fresh-data step before the
actual send.

## 14. file scope

Second phase after the debounce above settles — added 2026-07-10,
owner's own idea: patching straight into gLastMoogDump (synthComms.c)
and resending risked carrying stale data for every OTHER field in that
cached dump, not just the one the user meant to change (gLastMoogDump
is only as fresh as the last Panel Dump reply, which could be from
connect time or the last Sync). synth_flush_pending_dump_sends() sets
dumpSendAwaitingFreshData=true instead of sending immediately once the
debounce above elapses, and requests a fresh Panel Dump
(synth_request_state_dump()) if one isn't already in flight for this
reason (see gAwaitingFreshDumpForPatch, synthComms.c). When that fresh
reply arrives, extract_moog_panel_info() skips overwriting this dial's
own display value (it would otherwise stomp the user's pending choice
with the OLD, pre-change hardware value), and
synth_apply_pending_dump_patches() patches pendingDumpRawValue into
the now-fresh gLastMoogDump and sends once, clearing this flag.

## 15. file scope

Alternative to dumpShift/dumpMask above for a dump format where a
value's bits are packed continuously across MULTIPLE bytes, rather than
living inside one byte. dumpBitWidth=0 (the default) means "not this
kind of field" — use dumpShift/dumpMask on the single dumpOffset byte
instead, unchanged from before this existed. When dumpBitWidth > 0:
dumpOffset is still this field's first relevant byte, and dumpBitOffset
is which bit of that byte holds the field's LEAST significant bit, with
the remaining bits continuing in strict ascending significance into
subsequent bytes.

TWO DIFFERENT STRIDES share these same three fields, depending which
extraction function a device ends up in (moogStyleDump in tPanelConfig
decides which):
```
  - Moog (extract_moog_panel_info(), read_bitpacked_field()): the RAW
    wire bytes, 7 usable bits each (dumpBitOffset 0-6) — confirmed
    2026-07-08 against real Voyager hardware (Filter Cutoff/Resonance,
    before/after Panel Dump captures).
  - Korg-style (extract_prog_info()): bytes already decoded from the
    wire's 7-in-8 packing (decode_7to8()), so a normal 8 usable bits
    each (dumpBitOffset 0-7) — confirmed 2026-07-14 against real Kronos
    hardware (AL-1 Filter A/B Cutoff, before/after Current Object Dump
    captures via tools/kronos_dump_diff.py) — same continuous-bitstream
    shape as Moog's, just one stride wider since there's no leftover
    wire-encoding bit to skip once decoded.
```

## 16. file scope

Second bit location for a value whose bits are NOT contiguous in the
dump — e.g. Voyager's Filter A/B "Pole Select" (1/2/4-pole) each pack
their 2-bit value across two unrelated bytes, confirmed 2026-07-08 via
before/after hardware captures (tools/moog_dump + tools/syx_diff.py):
Filter A's two bits happen to be adjacent (ordinary dumpBitWidth=2
suffices), but Filter B's live in two bytes nowhere near each other.
dumpBitWidth2=0 (the default) means "no second chunk" — every dial
behaves exactly as before this existed. When set, the dial's full raw
value is chunk1 (dumpOffset/dumpBitOffset/dumpBitWidth, the LOW bits)
with chunk2 (dumpOffset2/dumpBitOffset2/dumpBitWidth2) contributing the
next-significant bits above it — same "two locations combine into one
value" shape ccLsbNumber above already uses for a CC pair, just for
dump bits instead.

## 17. file scope

dumpNativeMax/dumpInvert — a dump-decoded raw value can need different
handling from the SAME dial's CC-decoded one. Confirmed 2026-07-08
physically toggling Voyager's Ext On/Osc 1-3 On switches and diffing
the dump both ways:
```
  - The dump bit is INVERTED relative to the CC's own On/Off sense
    (raw dump bit 0 = On, 1 = Off) — dumpInvert=1 flips the raw value
    (across dumpBitWidth+dumpBitWidth2 bits) before native/display
    scaling, in extract_moog_panel_info()/synthComms.c.
  - nativeMax=127 (needed to decode the CC's own 0-63/64-127 threshold)
    would wrongly crush a raw dump bit of 0/1 straight down to display
    0 regardless of which one it was, if reused as-is for the dump
    path. dumpNativeMax=0 (the default) falls back to nativeMax,
    unchanged for a dial with only ONE wire representation (e.g.
    Filter A/B Pole Select, dump-only, where nativeMax alone already
    means the right thing) — set it explicitly (e.g. dumpNativeMax=1
    for a plain toggle bit) only when a dial has both a CC and a dump
    bit needing different scales.
```

## 18. file scope

Explicit grid position ("col="/"row=" in the file), for a device whose
real front panel groups controls into fixed columns rather than
flowing left-to-right within a section (see gridColWidth/gridRowHeight
below). 0-based; -1 (the default) means "not grid-positioned" — this
dial keeps going through the ordinary left-to-right auto-flow within
its section, exactly as every dial did before this existed, so a
device file with no col=/row= anywhere (Z1, Novation Supernova 2)
renders identically to before. A section can freely mix grid and
auto-flow dials, though in practice a device either commits one whole
page to the grid or doesn't use it at all. row defaults to 0 if col is
set but row isn't.

double, not int — 2026-07-07: aligning a short column's controls to a
taller neighbour (Mod Wheel/Pedal's 4 controls against Mixer's 5-tall
Level/On columns) needs its middle rows evenly spread across a
non-integer gap, not just whole grid steps. "row=2.67" places a dial
two-thirds of the way between grid rows 2 and 3.

## 19. file scope

"noLabel" in the file — for a binary/value-menu button (see
panel_dial_is_binary()/panel_dial_needs_value_menu()) whose own
current-value text is ALREADY self-explanatory without its dial's
label underneath (e.g. Filter A/B Pole Select showing "2 Pole",
Filter Mode showing "Dual LP"/"HP/LP") — suppresses the separate
label line synth_render() would otherwise draw beneath the button,
saving vertical space with no loss of clarity. Default false (show
the label) because most value-menu buttons are NOT self-explanatory
on their own — e.g. Voyager's "Menu Settings" page dials show things
like "Lower Key"/"Single Trigger" with no photo-realistic panel
position to supply context the way the main panel page's Filters
column does for Pole Select, so those need the label kept. A genuine
Off/On toggle (panel_dial_is_toggle()) is unaffected either way — it
always shows just its own label on the button face (colour conveys
on/off), a separate, older mechanism this doesn't change.

## 20. file scope

"readOnly" in the file — for a dial whose dump/CC field genuinely
cannot be changed by this app, so mouse interaction (drag, click,
dropdown) should do nothing at all rather than optimistically update
the display and attempt a write that will never actually take effect.
Added 2026-07-11 for Voyager's hPhoneVolume: its dump field turned out
to just mirror the REAL physical Headphone Volume pot's live position
(a real hardware finding, not a placement bug — confirmed by writing
an arbitrary value, then re-requesting a fresh dump: it read back
whatever the physical knob was ACTUALLY sitting at, 16380/near-max,
regardless of what had just been sent). An earlier "confirmed both
read and write" claim for this same dial (see
[[project_voyager_sysex_pdf_gap_audit]] in the assistant's own memory
notes) turned out to be a false positive — that test happened to drag
to max while the real knob ALSO happened to already be at/near max,
so the read-back looked like confirmation of a write that never
actually did anything. Default false (interactive) — every other
dump-only dial in this app (Filter A/B Pole Select, the PGM
wheel/pedal/shaping menus, etc.) genuinely IS a stored firmware
setting, not a live analog readout, so this should stay rare.

## 21. file scope

"asDial" in the file — forces panel_dial_needs_value_menu() to false
for a names= dial that would otherwise qualify (>2 positions, no CC,
has a dumpBitWidth) and default to a click-to-open dropdown button
(the 2026-07-08 "a menu-select control reads as a button, not
something you'd drag" call — see that function's own comment). Added
2026-07-13 for Voyager's tsGateCtrl: a 65-position named enum (Off,
64-127) that owner explicitly wanted to keep behaving like every
other continuous CC dial (drag a knob) rather than open a 65-row
dropdown, once the plain `display=raw` version's unlabelled 0-63 dead
zone got fixed by switching to names=+offset= — the earlier
button-only rule assumed every >2-name dump-only dial was a discrete
"pick one of these labelled things" selector (true for every other
one so far — destinations, sources, categories), not a numeric range
that just happens to render itself with named steps. Rendering
(synthGraphics.cpp) and click routing (mouseHandle.c's
arm_dial_press()) both key off panel_dial_needs_value_menu() alone,
so this one flag automatically fixes both without a separate check in
each. Default false — every other names= dial keeps today's
dropdown-button behaviour unchanged.

## 22. file scope

"asMenu" in the file — the opposite pull from asDial above: forces
panel_dial_needs_value_menu() to TRUE for a names= dial that wouldn't
otherwise qualify because it only has 2 positions (panel_dial_
is_binary()'s territory, normally a click-to-cycle button or, for a
literal Off/On pair, panel_dial_is_toggle()'s label+green/grey
styling). Added 2026-07-13 for the Z1's Porta on/off, Porta Mode,
Unison SW/Mode, F2 Link, and LFO 1-4 MIDI Sync — owner wanted these to
read their section's own colour (dial->colour) the same way every
3+-position value-menu dial already does, rather than a flat grey (or,
for the Off/On ones, green-when-on) that ignores it. synthGraphics.cpp
ANDs isToggle with !asMenu before using it, so an asMenu dial falls
straight into the existing >2-name value-menu code path (colour, text,
and — via arm_dial_press() in mouseHandle.c, which already checks
panel_dial_needs_value_menu() before panel_dial_is_binary() — click-to-
open-dropdown routing) with no separate special-casing needed anywhere.
Default false — every other 2-name dial keeps today's behaviour.

## 23. file scope

"hiLoOffset="/"hiLoCoarseScale="/"hiLoFineScale=" — only meaningful
when display == dialDisplaySignedHiLo (see that enum value's own long
comment for the derivation). The raw dump value read via the normal
dumpOffset/dumpBitWidth bitfield extraction is interpreted as:
```
  signed_raw = (raw >= 2^(dumpBitWidth-1)) ? raw - 2^dumpBitWidth : raw
  adjusted   = signed_raw - hiLoOffset
  HIGH       = coarse component of adjusted (see synth_decode_hilo() —
               floor(adjusted/hiLoCoarseScale), then a boundary
               adjustment so LOW always lands in its own signed range)
  LOW        = fine component, same signed range as HIGH
```
All three required (0 is not a meaningful default for any of them —
hiLoCoarseScale=0/hiLoFineScale=0 would divide by zero) whenever this
display mode is used; the parser logs an error rather than silently
defaulting if any is missing.

## 24. file scope

directive — shown in the startup device chooser
(see scan_panel_configs()); empty if the file
doesn't declare one.
1 byte for a classic manufacturer ID (e.g. Korg 0x42), or 3 bytes for an
"extended" ID (e.g. likely Novation — companies registered after
single-byte IDs ran out; MIDI spec signals this with a leading 0x00).
manufacturerIdLen is always 1 or 3, set by how many values the file's
"manufacturerId" line gives.

## 25. file scope

Some hardware (e.g. Moog's Minitaur/Voyager) never answers a Universal
Device Inquiry at all — confirmed by capturing the vendor's own editor,
which talks proprietary SysEx from the first message with no identity
handshake step. For such a device, set "identityQuery no" in the file:
this skips the inquiry/wait/reply cycle entirely rather than polling
and timing out. Defaults to true (load_panel_config() sets it right
after the zeroing memset), so existing files that never mention it are
unaffected. "midiChannel" (1-indexed, as written by a human) is then
the only way to know which channel to talk on, since there's no
identity reply to read gDevice.id from; ignored when identityQuery is
true, where the reply's own channel byte is authoritative instead.

## 26. file scope

With no identity reply, there's also nothing to correlate the right MIDI
destination from — the port a device without identity support sits on
(e.g. "Elektron TM-1") isn't necessarily whatever CoreMIDI happens to
enumerate first (e.g. an unrelated "IAC Driver Bus 1"). "midiPort <name
substring>" names it explicitly; connect_without_identity() in
midiComms.c matches it case-insensitively against each destination's
display name. Empty (the default) falls back to the first destination
found, same as before this existed.

Also honoured for an identity-capable device (process_identity_replies(),
midiComms.c), where it solves a related but different problem: the
normal path there (find_dest_for_source()) infers the send destination
from whichever source the identity REPLY came back on, assuming both
directions share one physical interface. That breaks for a real setup
this app's owner runs — sending out one interface (e.g. an Elektron
TM-1) while the synth's own MIDI OUT returns through an entirely
different, unrelated-by-name box (e.g. a Cirklon) — where nothing
about the reply's source resembles the actual send port's name/entity,
so inference finds no match at all and the app never finishes
connecting. Setting midiPortName pins the send side explicitly
regardless of which source the identity/CC/SysEx traffic actually
arrives on (that side was already source-agnostic where it matters —
dispatch_sysex() has no source filter, and gMidiSource just tracks
whichever source the identity reply came back on for the CC/PC gate).
Found 2026-07-14.

## 27. file scope

Optional SysEx sent right after connecting (see connect_without_identity()
in midiComms.c) — asks the device to report its own current state instead
of leaving every dial showing a stale/default value until physically
touched. Device-specific (e.g. Moog's own "dump current CC values"
command); set via the file's "stateRequestSysEx <hex> <hex> ..." line.
0 length (the default) means don't send anything, unchanged from before
this existed.

## 28. file scope

Moog's own dump SysEx has a completely different header shape from the
Korg-style one is_synth_sysex()/synth_handle_message() assume by
default (F0 <mfrId> <0x30|channel> <familyId> <func> ...): it's
F0 <mfrId> <productId> <deviceId> <mode> ... instead — see
"Voyager System Exclusive Panel Dump Format" (lintronics.de). Set via
"dumpFormat moog" + "productId <hex>" in the file; false/0 (the
default) leaves every existing Korg-style device (Z1) unaffected.
productId is Moog's own proprietary header byte (e.g. 0x01 Voyager,
0x08 Minitaur) — distinct from familyId/memberId (the Universal
Identity Reply scheme), which is moot anyway for a device with
identityQuery no.

## 29. file scope

Every Korg-style device (moogStyleDump == false) used to be assumed
to speak the SAME Z1-shaped protocol this codebase originally built
for: Program Data Dump Request/Reply (func 0x1C/0x4C), swept across
128 slots for the Load/Store Patch to Bank picker and Backup/Restore
Bank (Individual Files) — see synthBackup.c's own Korg name-sweep
section header comment. True for Z1; NOT true for every "Korg-style"
device in general — found 2026-07-14 connecting a real Kronos (an
entirely different, much richer object-based SysEx protocol, see
KRONOS_MIDI_SysEx.txt) under kronos.txt: the app immediately started
sweeping it with Z1's own Program Data Dump Request, forever, with
zero replies, since Kronos doesn't speak that specific protocol at
all. Defaults to true (set in load_panel_config(), same pattern as
supportsIdentity) so Z1 and any future plain-Korg-shaped device are
unaffected; kronos.txt sets "supportsKorgProgramDump no" to opt out
of the whole Z1-shaped sweep/backup/restore/Load-Store mechanism
until (if ever) Kronos gets its own, correctly-shaped equivalent.

## 30. file scope

Where a name field lives in each of the two Moog-style dump replies
(moogStyleDump devices only — see extract_moog_name() in
synthComms.c). Same continuous 7-bit-per-byte bitstream
dumpOffset/dumpBitOffset/dumpBitWidth already use for the numeric
panel fields — the Offset/BitOffset pair below is just that scheme's
byte/bit position for the name's first (of Len) 8-bit characters, not
a separate encoding. Two separate fields, not one shared offset,
because the two dump types' name fields don't live at the same
position — Single Preset Dump's is 1 byte later than Panel Dump's,
presumably for a preset-number byte Panel Dump has no reason to carry.
Reverse-engineered from real Voyager captures (2026-07-07): Panel Dump
from whatever the currently-loaded patch was ("FROM A DISTANCE"),
Single Preset Dump from preset 1 ("FILTER BUBBLES") — not from any
published spec. Both offsets default to -1 ("no name field declared"),
set in load_panel_config(), same convention as each dial's own
dumpOffset default.

## 31. file scope

How many characters of the name field above make up one line of the
device's own display, if it has a multi-line one (0 = no forced break
— the whole field is one line, the default for every device that
doesn't set this). extract_moog_name() (synthComms.c) inserts a '\n'
in gDevice.progName every nameLineWidth characters so the on-screen
"Program name" row (synth_render() in synthGraphics.cpp) can show the
same line breaks the real hardware does, rather than running both
lines together. Needed because the line boundary itself carries no
reliable marker in the raw data — see presetNameLen's comment in
voyager.txt for why a byte-value-based guess (e.g. "insert a break
wherever there's already whitespace") isn't enough: "Floating Mod" /
"Steel Guitar" fills both 12-char lines exactly, with no whitespace at
the boundary at all.

## 32. file scope

How many banks of presets the connected unit has — 1 for a base
Voyager, more for one with a memory expansion (VX-352 or similar; per
the Voyager manual, not confirmed against real hardware since nobody
testing this app owns an expanded unit). Documentation only right
now, not wired into anything: Backup > Bank
(synth_request_all_presets_dump() in synthComms.c, mode 0x04) has no
known way to select a non-default bank — the "Voyager System
Exclusive Panel Dump Format" doc this app's Moog SysEx handling is
otherwise built from doesn't cover multi-bank addressing, and mode
0x04 itself is unconfirmed even for the single-bank case. Once a real
Bank backup file's size/structure can be inspected (see
synth_backup_bank() in synthBackup.h), there may be a DI-no variant, a
bank-select byte, or a second SysEx mode this field ends up feeding —
deliberately not guessed at here.

## 33. file scope

Pitch (in px) between adjacent grid cells for any dial using col=/row=
(see tPanelDial.gridCol above) — "gridColWidth"/"gridRowHeight" in the
file. 0 (the default) means "not configured"; synth_render()
(synthGraphics.cpp) falls a grid dial back to ordinary auto-flow
rather than stacking every grid dial at the same spot if a device
declares col=/row= but forgets to set these. One pitch for the whole
device, not per-page or per-section — every grid-using page is
expected to want the same column/row rhythm.

## 34. `layout_panel_section()`

Computes each dial's `rect` in `section`. A dial with gridCol >= 0 (and
both grid pitches > 0) is placed directly at
origin + (gridCol*gridColWidth, gridRow*gridRowHeight) — every grid dial
on a page shares the same `origin`, which is what turns per-dial col/row
into a single page-wide grid rather than one grid per section. Every
other dial flows left to right from `origin` using the section's
dialSize/spacing and its own gapBefore, exactly as before col=/row=
existed.

## 35. `panel_dial_is_toggle()`

True for a dial whose names= is exactly {"Off","On"} — a genuine on/off
dial, as opposed to any other 2-position selector (Filters' Mode "Dual
LP"/"HP/LP", Osc 3's Freq Range "Lo"/"Hi", ...). Shared by
synthGraphics.cpp (renders these as a power button, not a knob — see
draw_power_button() in SynthLib) and mouseHandle.c (a single click
toggles one of these outright, rather than needing the drag gesture
every other dial uses) so the two stay in lockstep — a dial that LOOKS
like a button should also BEHAVE like one.

## 36. `panel_dial_is_binary()`

True for ANY 2-position named dial — panel_dial_is_toggle()'s Off/On
case included, plus 2-way selectors that aren't semantically on/off
(Filters' Mode "Dual LP"/"HP/LP", Osc 3's Freq Range "Lo"/"Hi", Env Gate
"Keyb"/"On/Ext"). All of these are still a single click-to-flip button
(mouseHandle.c) — only the on/off ones get the green/grey highlight;
the rest render as a plain button showing the current state's name (same
idea as G2-Edit's keyboard-tracking "KB" button), since colouring, say,
Filters' Mode green for "HP/LP" would imply an on/off meaning it doesn't
have.

## 37. `panel_dial_needs_value_menu()`

A discrete selector (>2 positions) with no CC at all — the only way to set
it is patching its bits into a freshly-fetched Moog dump and resending the
whole thing (synth_apply_pending_dump_patches() in synthComms.c), which
should happen exactly once with the FINAL chosen value, not once per
intermediate step a drag gesture would pass through. mouseHandle.c uses
this to open a value-picker menu (menus.c) instead of starting a drag —
added 2026-07-08 for Voyager's Filter A/B Pole Select, the first dials of
this kind (see fltAPole/fltBPole's own comment in voyager.txt). Broadened
2026-07-13 to also cover a param=-wired dial with no dump field at all
(e.g. the Z1's voiceMode/unisonType) — see this function's own comment in
panelConfig.c for why dumpBitWidth alone wasn't a complete "has real
protocol wiring" test.

## 38. `panel_dial_is_disabled()`

See disabledUnlessDialId/disabledUnlessValue's own comment above — false
(always enabled) if disabledUnlessDialId is empty, or if the named dial
can't be found. Takes config explicitly (rather than reaching for a
global) since panelConfig.c has no dependency on synthGraphics.h — same
reasoning find_panel_dial_anywhere() below already follows; callers pass
synth_panel_config().

## 39. `find_panel_dial_anywhere()`

Same as find_panel_dial(), but searches every section in the config rather
than one already-known section — for generic code (an info-text row, a
scroll-shortcut) that only has a dial id and no reason to know which
section it lives in.

## 40. `find_panel_dial_by_cc()`

Looks up the dial wired to a given MIDI CC number (see the "cc=" file
attribute) across every section in the config, or NULL if none matches —
for real-time CC dispatch, which (unlike a SysEx parameter change) carries
no section/group context to narrow the search.

## 41. `find_panel_dial_by_kronos_param()`

Looks up the dial wired to a given Kronos Parameter Change address (see
the "typ="/"soc="/"sub="/"pid="/"idx=" file attributes), across every
section — for dispatching an incoming func 0x43 the same generic way
find_panel_dial_by_param() dispatches Z1's group/param addressing.

## 42. `find_panel_dial_by_label()`

Looks up a dial by its display LABEL (case-insensitive), across every
section — for generic code that needs to find a dial by what it MEANS
rather than by its short internal id, because different device families'
own layout files give the same concept different ids: the Z1's Category
dial is `id=category`, the Voyager's is `id=soundCategory`, but both set
`label="Category"`. Added 2026-07-14 for synth_decode_korg_category()/
synth_decode_moog_category() (synthComms.h) so the Load/Store Patch from
Bank picker's category column works generically across device families
with zero per-device C code, matching this whole file's own "nothing
device-specific lives here" philosophy. Returns NULL if no dial's label
matches (a device with no category concept at all, say).

## 43. `scan_panel_configs()`

Scans `dir` for every "*.txt" file, fully parsing each (they're small — no
separate lightweight-header-only parser) into a scratch config just to
pull out its device/description, and returns how many were found (capped
at maxCandidates). Used to build a startup device chooser when more than
one config is present; a single match needs no chooser at all.

## 44. `panel_dial_hit_rect()`

Returns the index into section->dials[] under `point`, or -1 if none.
Call after layout_panel_section() has populated the dials' rects.
The rectangle a dial can actually be clicked in — larger than dial->rect for one drawn as a
button. Use this rather than dial->rect anywhere a click is being tested; see the definition.

## 45. `dumpSigned`

The packed dump field (dumpBitOffset/dumpBitWidth) holds a two's-complement
number dumpBitWidth bits wide. It is sign-extended before the dial sees it, so a
range such as -99..99 is written `storageOffset=-99 max=199 display=signed
displayOffset=99`, the same way the dial's Parameter Change value is signed.
Kronos dump tables give these as hex ranges like `9D~63`. Unlike `wireSigned`
(synthComms.c notes §12, §16), this adds no displayOffset of its own.

## 46. `PANEL_PAGE_LEN`

A page name may be a path of up to PANEL_PAGE_LEVELS names separated by `|`, written with or without
spaces round the separator (`page Program | EXi 1 | LFO 1/2`; stored as `Program|EXi 1|LFO 1/2`). Each
level is a row of tabs, and a row below the top shows only the children of the tab selected above it -
the Kronos's own Mode > area > page structure. `|` and not `/` because page names already contain `/`
("LFO 1/2"). A file whose page names have no separator gets the single row of tabs it always had.

## 47. `modeTabs`

"modeTab <mode> <top-level tab>" ties a device-reported mode number to a top-level page tab, both ways:
when the device reports the mode the editor moves to that tab, and clicking the tab sets the device's
mode. Kronos only so far - see synthComms.c notes §89.

## 48. `tPageVariant`

`pageVariant "<variant>" "<base>" <typDelta> <dumpDelta>` makes the tab `<variant>` show the pages under
`<base>` again, with every dial on them retargeted: its Kronos TYP plus typDelta, its dump offset plus
dumpDelta. The Kronos's two EXi slots are the case: one set of AL-1 pages written for slot 1 (TYP 11,
dump block at 2908) serves slot 2 too (TYP 12, block at 3960, so dumpDelta 1052). The dials keep one
value each; switching variant re-reads them from the cached dump, which edits and incoming changes for
either slot keep current - see synthComms.c notes §90.

An optional fifth field, `paramDelta`, does the same for the Korg Parameter Change ID of a
`group=/param=` dial (2026-10-07). The Z1 is the case: its oscillator-model and effect parameters carry
their slot in the ID's top bits (ExID, bits 11-13: 1/2 = OSC1/OSC2, 3/4 = Effect 1/2, 5 = Master), so
OSC 2 is OSC 1's pages with every model ID +2048 and every dump offset +52, and Effect 2 is Effect 1's
with +2048 and +23. A section whose step differs says so with `variantParamDelta` (§52).

## 49. `showIfOffset`

`showIf <dumpOffset> <value>` inside a page shows that section only while the cached dump's byte at
dumpOffset holds value (before any dump has arrived, it shows). On a pageVariant's tab the offset moves
with the variant's dumpDelta. The Kronos case: the AL-1 sections carry `showIf 2857 2`, EXi 1's
Algorithm Type being AL-1, and on the EXi 2 tab that reads 3909, EXi 2's - so AL-1 dials never show,
reading another engine's bytes as AL-1 parameters, for a slot holding MOD-7 or nothing.

## 50. `tTabLabel`

`tabLabel "<tab>" <dumpOffset> <name,name,...>` appends ": <name>" to that tab's text, indexed by the
cached dump's byte at dumpOffset (moved by dumpDelta on a pageVariant's tab): "EXi 1: AL-1".

## 51. `variantMax`

`variantMax=<n>` on a dial is the count of positions it offers while its page is shown as a pageVariant
(§48), where its base slot offers `max`. The Z1's OSC 2 offers 9 of OSC 1's 13 oscillator types and
Effect 2 11 of Effect 1's 15, so the type dials on the OSC 1 and Effect 1 pages carry `variantMax=9` and
`variantMax=11`. `synth_dial_max()` (synthComms.h) is the one place that decides; the drag range, the
clamps, the dial's sweep and the value menu all ask it.

## 52. `variantParamDelta`

`variantParamDelta <n>` inside a page overrides the pageVariant's paramDelta (§48) for that section.
On the Z1's OSC page the model sections move by ExID (+2048), but the pitch section moves to OSC 2's own
plain IDs, 14 further on (Oscillator Type 174 -> 188); Effect Select moves by 1 (359 -> 360).

## 53. `PANEL_MAX_NAMES` and the dial's `names`

A dial's value names live on the heap, exactly `nameCount` of them, allocated as the line is parsed
(panelConfig.c notes §11). Until 2026-10-07 every dial carried a fixed `[PANEL_MAX_NAMES][PANEL_LABEL_LEN]`
array, 2,080 of its 2,496 bytes whether it had names or not, which made the cap expensive to raise (it went
20 -> 32 -> 48 -> 65 for the Voyager) and every added section cost 80 KB. The Z1's formatted value tables
need up to 252 entries (delay times, LFO frequency, EQ frequency), so the cap is now 256 and costs nothing
until used. `names[i]` reads as before; a dial with none has `names == NULL` and `nameCount == 0`, which every
reader already checks. Reloading a configuration leaks the previous names (a few kilobytes) rather than
freeing them, because `load_panel_config()` is also handed uninitialised scratch structs to scan with.

## 54. `PANEL_MAX_SECTIONS`

256 since 2026-10-07. Raised 32 -> 48 -> 64 in July as the Z1's Amp/EG/LFO pages were split, each time
for a real failure: past the cap, dials land silently in the wrong section (panelConfig.c's "too many
sections" error). The Z1's complete program needs about 175 - a section per row of every oscillator model
and effect type, each shown only while selected. A section is about 14.5 KB since the dial names moved to
the heap (§53), so the whole configuration stays under 4 MB.

## 55. Banks: `bankSelect`, `bankMapRequest`, `bankMapReply`, `programChangeTransmit`, `startupProgram`

A device with more than one bank of 128 programs declares them, in bank order, as
`bankSelect <name> <msb> <lsb>`: the name prefixes the program number in the current-program label
("A042"), and msb/lsb are the Bank Select (CC0/CC32) values that pick the bank, -1 for one not sent. The
current program is then a slot, bank x 128 + program - the same numbering as the Korg name cache.

Where the device keeps its Bank Select values as a user setting, `bankMapRequest <bytes>` is the request to
send after the SysEx header at connect, and `bankMapReply <func> <sub> <msb offset> <lsb offset> ...` says
where the reply holds each declared bank's values (signed bytes, -1 = Off), replacing the defaults.
`programChangeTransmit <offset> <shift> <mask>` names the field in that reply which is 0 when the device
sends no Program Change; the label then says so instead of leaving the program blank.

`startupProgram <bank index> <program>` is the program the device powers up on. If nothing else has
settled the current program after the first edit-buffer dump, that one program is requested and its name
compared with the edit buffer's (synthComms.c notes §101).

The Z1 is the case: banks A and B, its Program Bank Select Map in the MIDI half of the Global/MIDI dump,
and A000 at power-up.

## 56. `graph` - a breakpoint graph bound to dials

A section may hold one `graph` line instead of dials:

    graph width=420 height=110 segments=5 points="x=0,y=eg1startlvl;t=eg1atk,y=eg1atklvl;...;x=4,y=eg1suslvl;..."

`points` lists the points left to right, `;` between points, `,` between parts:
- `x=<n>` - at n segments from the left (a fixed point, e.g. an envelope's sustain end)
- `t=<dial>` - the previous point's x plus this dial's share of one segment (an envelope time)
- `X=<dial>` - this dial's share of the whole width (a key-tracking break point)
- `y=<dial>` - this dial's share of the height; `y=<number>` - a fixed height, 0 to 1

A dial's share is its position over its range (value / (positions - 1)), so a bipolar level puts its zero
in the middle, where the graph draws a reference line. A point moves sideways if it has `t=` or `X=`, up
and down if it has `y=<dial>`; dragging it sets those dials exactly as turning them would, sending to the
device. `readOnly=1` makes every point fixed - a graph that shows, not edits. The drawing and picking are
SynthLib's (breakpointGraph.h); the binding is panelGraph.c.

## 57. `variantShowIf`

`variantShowIf "<variant>" <dumpOffset> <min> <max>` shows a pageVariant's tab only while that dump byte
(the base's, not moved) is within min..max; before any dump, the dial that edits the byte decides
(synthGraphics.c notes §58). The Z1 is the case: OSC 2 exists only while OSC 1 is not a physical model
(byte 154 within 0..8), Effect 2 only while Effect 1 is a single-size type (byte 410 within 0..10). Sending
to the missing slot does nothing on the device, so the tab should not be there to edit.

## 58. Dump blocks: `dumpBlock` and `fromDump`

Not every parameter lives in the program dump. `dumpBlock <name> <request bytes> reply <func> <sub>`
declares another dump: the request (after the SysEx header, without F7) and the function and sub byte its
reply carries. A section that says `fromDump <name>` reads its dials' dumpOffsets from that reply instead of
the program dump. Every block is requested at connect and by Sync from synth. The Z1 is the case: its Global
settings come in `51 00` (asked with `0E 00`), its MIDI settings in `51 01` (`0E 01`), both edited with
Parameter Change group 0.
