# SynthEdit

A macOS GUI editor built to support a variety of synthesizers. Synth-specific data — panel layout, controls, SysEx identity — lives in per-device layout files (see `layouts/`), not hardcoded in the app. Currently supports the Korg Z1 (`layouts/z1.txt`). Work in progress.

If anyone is interested in helping, especially with the GUI side of things, please drop me a line.

Since I'm now incurring costs (I recently started using LLMs) which would be good to at least cover, I now have a Buy Me a Coffee page:

https://buymeacoffee.com/chrispurusha

Thanks for any donations!

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

### 1. Clone with submodules

The third-party libraries (glfw, freetype) are nested submodules inside SynthLib. The `--recurse-submodules` flag is required — without it the build will fail.

```
git clone --recurse-submodules https://github.com/chrispurusha/Z1-Edit.git
```

If you already cloned without `--recurse-submodules`:

```
git submodule update --init --recursive
```

### 2. Update SynthLib (contributors / returning developers)

SynthLib is a shared library submodule pinned to a specific commit. If SynthLib has been updated since you last pulled, advance the pin before building:

```
git submodule update --remote SynthLib
git add SynthLib && git commit -m "Update SynthLib"
```

Do not manually copy files into the `SynthLib/` directory — this will cause conflicts on the next update.

### 3. Build the third-party libraries

All commands run from the root of the cloned repository.

**glfw:**

```
cmake -S SynthLib/ThirdParty/glfw -B SynthLib/ThirdParty/glfw/build \
  -DBUILD_SHARED_LIBS=OFF \
  -DGLFW_BUILD_DOCS=OFF \
  -DGLFW_BUILD_EXAMPLES=OFF \
  -DGLFW_BUILD_TESTS=OFF \
  -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=15.1
cmake --build SynthLib/ThirdParty/glfw/build
```

**freetype:**

```
cmake -S SynthLib/ThirdParty/freetype -B SynthLib/ThirdParty/freetype/build \
  -DBUILD_SHARED_LIBS=OFF \
  -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=15.1 \
  -DFT_DISABLE_BROTLI=ON \
  -DFT_DISABLE_BZIP2=ON \
  -DFT_DISABLE_HARFBUZZ=ON \
  -DFT_DISABLE_HVF=ON \
  -DFT_DISABLE_PNG=ON \
  -DFT_DISABLE_ZLIB=ON
cmake --build SynthLib/ThirdParty/freetype/build
```

### 4. Build with Xcode

Open `SynthEdit.xcodeproj` and build normally.

### 5. Code formatting (optional)

After editing source files, run from the repository root:

```
./do-uncrustify
```

See [THIRD_PARTY.md](./THIRD_PARTY.md) for open-source acknowledgments.
