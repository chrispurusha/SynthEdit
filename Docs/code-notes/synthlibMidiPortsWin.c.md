# synthlibMidiPortsWin.c notes

`platform/windows/synthlibMidiPortsWin.c`, the Windows version of SynthLib's `synthlibMidiPorts.c`.
The code points at each as `// notes §k`.

## 1. The port registry

See coreMidiWin.h notes §2 for why a Windows `MIDIEndpointRef` is a registry slot rather than a winmm
index. Up to 64 ports per direction; the slot is the low 16 bits, `WIN_INPUT_BIT` or `WIN_OUTPUT_BIT`
says which list. Names are winmm's `szPname`, which is at most 31 characters - so a long USB device name
is cut short there, by Windows, and that is the name the MIDI Ports dialogue shows and saves.

The port CHOICE half of the file (`synthlib_midi_ports_*`, `synthlib_midi_channel_*`) is SynthLib's
own code, copied unchanged: it is prefs only, and the Mac file cannot be compiled here because its other
half is CoreMIDI.

## 2. `synthlib_midi_win_ports_changed()`

Builds one string of every input name and every output name and compares it with the last one. Cheap
enough to poll: two `midi*GetNumDevs()` and one `GetDevCaps` per port.
