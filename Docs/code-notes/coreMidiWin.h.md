# coreMidiWin.h notes

`platform/windows/coreMidiWin.h`, which `src/sysIncludes.h` includes off Apple in place of CoreMIDI.
The code points at each as `// notes §k`.

## 1. file scope

midiComms.c is written against CoreMIDI. Rather than a second, Windows copy of its connect logic -
which would drift, as device.c's two copies once did - the Windows build gives it the handful of
CoreMIDI names it uses with no platform code around them (`ItemCount`, `OSStatus`, `noErr`,
`MIDIClientRef`, and the four `MIDIGet...` calls below), and puts the few functions that DO need
CoreMIDI proper (names through CFString, entities, the client and its ports, the run loop) behind
`#if defined (_WIN32)` in midiComms.c itself. Nothing here pretends to be more of CoreMIDI than that.

## 2. `synthlib_midi_win_count()` / `synthlib_midi_win_endpoint()`

winmm numbers its devices 0..n-1 and renumbers them whenever one is plugged in or out, while CoreMIDI's
endpoint references stay put. So a `MIDIEndpointRef` on Windows is not a winmm index: it is a slot in a
registry of every port seen this session, keyed by the port's name and by which occurrence of that name
it is (two identical interfaces are "X" and "X" #2), with a bit for input or output. A slot is never
reused, and `synthlib_midi_win_index()` resolves it back to whatever winmm index carries that port now.
The effect is CoreMIDI's: a reference taken at connect time still means the same port after an unrelated
device comes or goes, and means nothing (resolves to no index) once its own port is unplugged.

## 3. `synthlib_midi_win_open_inputs()`

Stands for CoreMIDI's input port and `MIDIPortConnectSource()`: opens every winmm input not yet open,
closes those that have gone, and routes everything that arrives to `receive` - midiComms.c's
`midi_receive_bytes()`, the same parser the Mac's read callback feeds. Safe to call at every scan.

## 4. `synthlib_midi_win_ports_changed()`

winmm has no setup-changed notification (CoreMIDI's `kMIDIMsgSetupChanged`); this compares the port
names now with those at the last call. midiComms.c's `midi_wait()` polls it. The first call only
records the list.
