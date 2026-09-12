# main.c notes

The longer comments from `main.c`, moved here 2026-09-12 so the code reads cleanly. The code points at each as `// notes §k`. Verbatim and in file order; each is titled by what it documents.

## 1. in `main()`

Must run before init_graphics() — its own synth_init_graphics() call (at its tail) reads
get_saved_layouts_dir() (misc.h) to resolve the layouts folder for the very first frame, and
that read needs the prefs file already loaded or it silently falls back to the built-in
default every launch. See init_settings()'s own comment (misc.h) for the bug this fixes.
