# Windows port

Living note, started 2026-11-07 (CT and Claude, on the Parallels VM: Windows 11 on ARM). Modelled on
G2Alike's `Docs/windows-port-plan.md`, which went first and found most of the traps.

## Building

**On Windows** - llvm-mingw only (no MSYS2, CMake or Python), unpacked under `%USERPROFILE%\Developer`
(`llvm-mingw-<date>-ucrt-aarch64` on ARM, `-ucrt-x86_64` on Intel) or wherever `LLVM_MINGW` points:

```
powershell -ExecutionPolicy Bypass -File tools\do-windows.ps1            # Release, this machine's architecture
powershell -ExecutionPolicy Bypass -File tools\do-windows.ps1 -Debug     # -O1, console window with the log
powershell -ExecutionPolicy Bypass -File tools\do-windows.ps1 -Arch x64  # the other architecture
```

**On the Mac** - the same build cross-compiled, llvm-mingw in `~/Developer/llvm-mingw` (see G2Alike's
`Docs/windows-port-plan.md`, "Cross-building on the Mac", for fetching it):

```
tools/do-windows              # x64 and arm64: build-cross-<arch>/SynthEdit.exe
tools/do-windows arm64 --debug
```

Either way the result is one self-contained `SynthEdit.exe` with `layouts\` copied beside it: the
application looks for `layouts` in its working folder until one is chosen (Layouts > Choose Layouts
Folder), and a double-clicked .exe starts in its own folder. It needs only DLLs Windows ships (OpenGL32,
WinMM, the universal C runtime). GLFW and FreeType are compiled from `SynthLib/ThirdParty` once per
architecture into `build-*/deps`; `-Clean` / `--clean` builds them again.

**Sources.** The Xcode project's synchronized folders - every `.c` and `.cpp` in `src/` and
`../SynthLib/src/` - less what `platform/windows/swaps.txt` names, plus every `.c` in `platform/windows/`.
Both scripts read that one file. A new source file in either folder needs nothing doing here. SynthLib
is compiled from source: Windows does not use `libSynthLib.a` (`SynthLib/Docs/build.md`).

The Mac script was written on Windows; it first ran on the Mac on 2026-11-07, building both
architectures through `do-release-windows` (it had been committed without its executable bit).

**Releases** - `./do-release-windows beta` on the Mac, with the same argument as `./do-release` and before
the tag: a clean Release build per architecture, checked to be a GUI program carrying its version, zipped
to the Desktop as `SynthEdit-<version>-windows-<x64|arm64>.zip` - a folder holding `SynthEdit.exe`,
`layouts\`, `Read Me First.txt` (`platform/windows/`, with the version and SynthLib commit added) and
`LICENSE`. Unsigned: SmartScreen warns once.

## What is Apple-only, and its Windows version

| Mac | Windows |
|---|---|
| CoreMIDI, in `src/midiComms.c` | winmm: `platform/windows/synthlibMidiWin.c` (send, inputs) and `synthlibMidiPortsWin.c` (the port list), behind `coreMidiWin.h`; the few CoreMIDI-proper parts of midiComms.c have `#if defined (_WIN32)` versions beside them |
| SynthLib's `synthlibMidi.c`, `synthlibMidiPorts.c` | the two files above |
| `src/misc.mm` | `platform/windows/miscWin.c` - temp folder, settings; sleep/wake is a stub |
| Metal | OpenGL 1.1, `SynthLib/src/renderBackendGL.c`, selected off Apple by `renderBackendSelect.h` |
| fonts in `/System/Library/Fonts` | `C:\Windows\Fonts\arial.ttf` (`src/graphics.c`) |
| prefs in `~/Library/Application Support/SynthEdit` | `%APPDATA%\SynthEdit` (SynthLib's `prefs.cpp`) |

Rule while the port is young (CT, from G2Alike): Windows differences are CONDITIONAL - `#if defined
(_WIN32)` or `platform/windows/` - and the Mac compiles what it did. Two changes are not, and both are
for the Mac too: `midiComms.c`'s byte parser moved out of `midi_read_cb()` into `midi_receive_bytes()`
unchanged, and its run-loop waits go through `midi_wait()` (midiComms notes §37). **Not yet built on the
Mac** - build there before pushing.

The winmm MIDI layer belongs in SynthLib eventually: G2Alike and the AllAlike plug-ins still link silent
stubs in its place (G2Alike's `platform/windows/synthlibMidi*Win.c`). Moving it means those builds
compiling it instead of their stubs, so it waits until it has been used here for a while.

## Progress

**2026-11-07, Windows (native, ARM64 under Parallels).** `tools\do-windows.ps1` builds SynthEdit
(43 files, one warning: `synthComms.c`'s unused `encode_8to7`, the same on the Mac). It runs: the window
opens, the layouts folder is found, the Choose Device dialogue and the Z1 panel draw.

MIDI through an Elektron TM-1 (passed through to the VM: Parallels > Devices > USB & Bluetooth):

- winmm lists it as "Elektron TM-1", in and out. The editor opens the input, scans, and sends the identity
  request to every output.
- Z1 -> Windows WORKS: notes, CCs and the Z1's own parameter-change SysEx (`F0 42 33 46 ...`, channel 4)
  all arrive, checked with a standalone winmm probe.
- Windows -> Z1 WORKS, once the MIDI thru box between the TM-1 and the Z1 was powered: the identity reply
  comes back (channel 4 detected), and the current program, the global, MIDI, multi and arpeggio blocks
  and the 256-program name sweep all load.

Window size: GLFW makes a Windows window in pixels, a Mac one in points, so the default opened at half
size on the VM's 200% display. `GLFW_SCALE_TO_MONITOR` in SynthLib's `synthlibWindow.c` (its notes §8)
fixes that: 666x431 became 1306x791. A size saved by dragging still wins.

Fixed on the way, for both platforms: the first-run Choose Device dialogue did not reconnect MIDI for the
device chosen, so the connection made meanwhile for the default configuration stayed
(`synthGraphics.c`, `on_startup_device_chosen()`); `synthGraphics.c` used `fmin` without `<math.h>`;
the "Fetching preset names..." status was drawn over the current program's label (`A001`) - both started
at the same x (synthGraphics notes §24).

From G2Alike's Windows work: dial drags hide the pointer instead of locking it (mouseHandle notes §26);
SynthEdit already redrew during Windows' modal window resize (`windowRefresh`); stdout is unbuffered on
Windows so a redirected log survives `_exit()` (`main.c`).

**Open:** sleep/wake (`WM_POWERBROADCAST` -> `midi_request_reconnect()`); the minimum window size is not
scaled for DPI; -Werror (the one warning left: `encode_8to7()` in synthComms.c is unused).
