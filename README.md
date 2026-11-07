# SynthEdit

A GUI editor for macOS and Windows (experimental), built to support a variety of synthesizers. Synth-specific data — panel layout, controls, SysEx identity — lives in per-device layout files (see `layouts/`), not hardcoded in the app. Work in progress.

## What it supports today

| Synth | State |
|---|---|
| Korg Z1 | The most complete: every Program, Multi Set, Arpeggio pattern and Global/MIDI parameter on its own page, envelope and key-track graphs you can drag, an arpeggio step grid, patch names, Prev/Next across both banks, and backup/restore of programs |
| Moog Voyager | Most controls checked on the hardware, patch names, Prev/Next, backup/restore |
| Korg Kronos | Live sync and every AL-1 parameter in EXi slot 1; only Filter A/B Cutoff in slot 2 |
| Moog Minitaur | State request confirmed on the hardware; most of the CC map checked, the rest not yet |
| Novation Supernova II | Identity and a few CCs (filter cutoff and resonance); no SysEx yet |

## Still to do

What a user would notice is missing, by synth. Things built but not yet tried on a real synth are marked *untested*.

**All synths**
- Find the synth automatically: send one identity request and pick the matching layout, instead of choosing it first
- Ship the layouts inside the app on macOS (they currently sit beside it, and Layouts > Choose Layouts Folder... points elsewhere)
- A plug-in that restores the synth's whole state from a DAW project (long term)
- Some long rows of controls run off the right edge of a narrow window

**Korg Z1**
- Show and edit the Multi Set name and the arpeggio pattern name
- Tables too big for dials: User Scale 2 (128 notes), the 16 user group names, the program and multi select maps
- The performance editor knobs' parameter list names the oscillator model's own parameters only as "OSC Model n"
- Backup, restore, load and store for Multi Sets, arpeggio patterns and the Global settings (programs only so far)
- Confirm each write: wait for the Z1's "data load completed" or "error" reply instead of assuming it landed
- Edit a bank file offline: reorder, copy and rename programs without the synth, search by name, print a list
- Start a new program from a template
- An on-screen keyboard and a controller panel that sends CCs
- *Untested*: Store Patch to Current Slot, writing Global/MIDI settings, a non-default bank select map

**Moog Voyager**
- Restoring a patch from a file may not set every control (one filter setting has been seen to differ)
- The Voyager's Panel Dump reports where the knobs are, not the loaded patch, so after a restore the editor shows the file rather than re-reading the synth
- The Pot Map settings are not yet confirmed on the hardware

**Korg Kronos**
- Pages for the other EXi engines (MOD-7, CX-3, STR-1, MS-20EX, PolysixEX, SGX-1, EP-1, HD-1), then Combi and Global
- Store and bank operations (only the edit buffer today)
- Pitch intensities show the raw value rather than the Kronos's own -48.00..+48.00

**Moog Minitaur** - the rest of the CC map to check on the hardware.

**Novation Supernova II** - SysEx: patch dumps, names and the full parameter set.

**Waldorf Pulse 2** - planned: a layout from Waldorf's published CC table, and sound dumps.

**Windows (experimental)**
- Reconnect MIDI after the PC sleeps
- The minimum window size does not yet allow for display scaling

Suggestions and reports are welcome as GitHub issues.

## Working with LLMs

Development started as a solo project. As the project has evolved, I've recently incorporated LLM-assisted development into the workflow, using it as a tool for research, coding, debugging, and experimentation.

## Building

### Prerequisites

Install the following via Homebrew (https://brew.sh):

```
brew install cmake uncrustify
```

- **cmake** — builds glfw and freetype
- **uncrustify** — code formatter (optional, only needed when editing source)

Xcode and its command line tools are also required:

```
xcode-select --install
```

### 1. Clone beside SynthLib

SynthLib is a shared checkout beside this project, not part of it:

```
git clone --recurse-submodules https://github.com/chrispurusha/SynthLib.git
git clone https://github.com/chrispurusha/SynthEdit.git
```

A project builds against whatever SynthLib is checked out, so keep it up to date with `git -C ../SynthLib pull`.

### 2. Build SynthLib

Nothing to do by hand: each build runs `SynthLib/do-build` first, which builds the library and its third-party
libraries when they are out of date. See `SynthLib/Docs/build.md`.

### 3. Build with Xcode

Open `SynthEdit.xcodeproj` and build normally.

### 4. Code formatting (optional)

After editing source files, run from the repository root:

```
./do-uncrustify
```

### Windows

Built on the Mac by cross-compiling, or on Windows itself - see `Docs/windows-port.md`. `./do-release-windows beta`
makes the release zips.

See [THIRD_PARTY.md](./THIRD_PARTY.md) for open-source acknowledgments.
