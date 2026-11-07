# synthlibMidiWin.c notes

`platform/windows/synthlibMidiWin.c`, the Windows version of SynthLib's `synthlibMidi.c`, plus the
MIDI inputs. The code points at each as `// notes §k`.

## 1. Outputs

winmm has nothing like CoreMIDI's output port, through which one sends to any destination: each output
device is opened by itself. One is opened on the first send to it and kept open; a send that fails closes
it, so an interface unplugged and plugged back in is opened afresh on the next send. Most winmm drivers
let only one application hold an output at once - `midiOutOpen` then fails with `MMSYSERR_ALLOCATED`
(4), logged, and the editor sees a failed send exactly as on a Mac with no destination.

## 2. in `send_long()`

`midiOutLongMsg` returns before the bytes have gone; the header (and the caller's buffer) must stay put
until winmm marks it `MHDR_DONE`. The send mutex is held meanwhile, so sends stay in order as CoreMIDI's
do. At 31,250 baud a 64 KB message takes about 21 seconds, hence the 10-second wait being per message
rather than a total: the editor sends nothing that large.

## 3. in `synthlib_midi_send_to()`

CoreMIDI takes any run of MIDI bytes in one packet; winmm wants each SysEx as a long message and each
other message packed into one `DWORD`. The data is split accordingly. Running status is not expanded:
the editor never sends it.

## 4. Inputs

Each open input has eight 16 KB SysEx buffers queued. winmm hands a SysEx back in pieces as buffers fill
(a Z1 bank dump arrives in several), and the parser is a byte stream, so pieces need no reassembly here.
Short messages arrive one per `MIM_DATA`.

## 5. `gReceiveLock`

winmm may call back on a different thread per input device, and midiComms.c's parser keeps one SysEx
capture and per-source running status in plain globals, as the Mac's single CoreMIDI read thread
allows. The lock gives Windows the same one-at-a-time delivery.

## 6. in `input_cb()`

Microsoft's documentation lists the functions a callback may call and `midiInAddBuffer` is not among
them. Re-queueing the buffer from the callback is nevertheless what every widely used MIDI library on
Windows does (RtMidi included), and the alternative - a thread of our own to do it - adds latency to
every SysEx piece. A buffer returned during `midiInReset` (closing) is not re-queued.

## 7. in `synthlib_midi_win_open_inputs()`

An input whose port has gone is closed here, at the next scan, rather than when winmm notices: a device
that vanishes mid-session simply stops delivering until then.
