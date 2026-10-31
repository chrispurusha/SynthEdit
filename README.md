# SynthEdit

A macOS GUI editor built to support a variety of synthesizers. Synth-specific data — panel layout, controls, SysEx identity — lives in per-device layout files (see `layouts/`), not hardcoded in the app. Currently supports the Korg Z1 and Moog Voyager. Work in progress.

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
git clone https://github.com/chrispurusha/Z1-Edit.git
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

See [THIRD_PARTY.md](./THIRD_PARTY.md) for open-source acknowledgments.
