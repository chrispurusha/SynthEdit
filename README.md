# Z1-Edit

A macOS GUI editor for the Korg Z1 synthesizer. Work in progress.

If anyone is interested in helping, especially with the GUI side of things, please drop me a line.

Since I'm now incurring costs (I recently started using LLMs) which would be good to at least cover, I now have a Buy Me a Coffee page:

https://buymeacoffee.com/chrispurusha

Thanks for any donations!

## Building

### 1. Clone with submodules

```
git clone --recurse-submodules https://github.com/chrispurusha/Z1-Edit.git
```

If you already cloned without `--recurse-submodules`:

```
git submodule update --init --recursive
```

### 2. Build the third-party libraries

glfw and freetype are in SynthLib/ThirdParty and shared across all editor projects:

```
cd SynthLib/ThirdParty/glfw
cmake -S . -B build
cmake --build build

cd ../freetype
cmake -S . -B build
cmake --build build
```

### 3. Build with Xcode

Open `Z1Edit.xcodeproj` and build normally.

### 4. Code formatting (optional)

After editing source files:

```
./do-uncrustify
```

Install uncrustify if needed: `brew install uncrustify`

See [THIRD_PARTY.md](./THIRD_PARTY.md) for open-source acknowledgments.
