# defs.h notes

The longer comments from `defs.h`, moved here 2026-09-12 so the code reads cleanly. The code points at each as `// notes §k`. Verbatim and in file order; each is titled by what it documents.

## 1. `MENU_BAR_HEIGHT`

Program name length (parameters 1..progNameLen), CC assignments, and every
per-control SysEx parameter ID/dump offset all live in <device>.txt
(progNameLen/group=/param=/cc=/dumpOffset=), not here — see panelConfig.h
and synthComms.c's generic dial dispatch/decode.
