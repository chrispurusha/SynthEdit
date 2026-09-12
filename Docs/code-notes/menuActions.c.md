# menuActions.c notes

The longer comments from `menuActions.c`, moved here 2026-09-12 so the code reads cleanly. The code points at each as `// notes §k`. Verbatim and in file order; each is titled by what it documents.

## 1. file scope

The one menu action with enough of its own logic (and a public misc.h declaration other files
call, synthGraphics.cpp's synth_choose_config_file()) to warrant living outside appMenuBar.c —
everything else is a thin tMenuItem-signature wrapper and lives there instead, next to the menu
structure it serves.
