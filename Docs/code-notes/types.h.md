# types.h notes

The longer comments from `types.h`, moved here 2026-09-12 so the code reads cleanly. The code points at each as `// notes §k`. Verbatim and in file order; each is titled by what it documents.

## 1. `SYNTH_PROG_NAME_MAXLEN`

Ceiling on program-name length across any supported device — the actual
length used when parsing/sending is tPanelConfig.progNameLen, from the
device's own <device>.txt ("progNameLen N"); this is just how big the
buffer needs to be to hold the longest name any config is likely to
specify. Every other piece of device state (filter/oscillator/mixer/
category/voice-mode/unison/... or anything else a given synth has) lives
entirely in that device's own panel-config dials (see panelConfig.h) —
nothing synth-specific belongs here, so that adding a new device is just a
new <device>.txt, no C changes.

## 2. file scope

Best-known current Program Change number (0-127, MIDI wire numbering),
or -1 if unknown. There's no reliable way to ask a device "what
program are you on" — a dump reply (Panel Dump, Current Program Dump)
reports the live edit buffer's contents, not which stored slot (if
any) it started from, so this is only ever learned from an actual
Program Change message: one arriving from elsewhere on the bus
(dispatch_program_change() in midiComms.c), or one this app itself
just sent (synth_navigate_preset() in synthComms.c, for the Prev/Next
patch buttons — see synth_hit_test_patch_nav() in synthGraphics.h).
Reset to -1 on every fresh connect (synth_on_connected()): a value
learned from a previous session/device isn't trustworthy for a new one.

## 3. file scope

Moog-style protocol's own SysEx "Device ID" byte (0-127) — see
moogStyleDump in panelConfig.h. Distinct from `id` above (the MIDI
CHANNEL): the Voyager's front panel exposes Device ID and MIDI Channel
as two separate settings, and a dump REQUEST addressed to the wrong
Device ID is silently ignored by the hardware regardless of channel.
Seeded from the device's own <device>.txt (stateRequestSysEx's own
deviceId byte, at a fixed offset — see moog_learn_device_id()'s own
comment, synthComms.c) as a first guess matching the factory default,
then kept in sync with reality by moog_learn_device_id() on every
accepted incoming Moog SysEx message, the same way an independently-
developed reference implementation for this exact hardware
(moogvoyagereditor.pistolinstruments.com's midi.js) already does — its
own comment there names the exact failure this avoids: a request built
with a stale/wrong Device ID is silently ignored by the synth, which
just looks like "Fetch" doing nothing. Meaningless (left at whatever
the config seeded) for a device that doesn't set moogStyleDump — only
this dump-request SysEx format has a Device ID byte at all; the
Minitaur, for instance, dumps state as plain CC traffic instead (see
minitaur.txt's own stateRequestSysEx comment) and never reaches
moog_learn_device_id() in the first place.

## 4. `tNameEdit`

Inline-editable program name field state (click the name to start, type,
Enter commits via synth_set_program_name() (synthComms.h), Escape
cancels) — modelled on G2-Edit's own tNameEdit (mouseHandle.c there),
minus the multi-slot bookkeeping G2's 4-slot patch browser needs: this
app only ever edits the one currently-loaded program name. buffer holds
the FLAT name (no line-wrap '\n's — those are synth_render()'s own
display concern, see wrap_name_for_display() in synthGraphics.cpp),
capped at SYNTH_PROG_NAME_MAXLEN regardless of which connected device's
progNameLen/panelNameLen is actually shorter (synth_effective_name_maxlen(),
synthComms.h, is what enforces the real per-device cap while typing).
