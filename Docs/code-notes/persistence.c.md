# persistence.c notes

The longer comments from `persistence.c`, moved here 2026-09-12 so the code reads cleanly. The code points at each as `// notes §k`. Verbatim and in file order; each is titled by what it documents.

## 1. file scope

Small settings persistence that doesn't need Objective-C/Cocoa — goes
through SynthLib's prefs.h (a plain "key=value" text file under a per-OS
standard config directory) instead of NSUserDefaults, same reasoning as
every other native-Cocoa-mechanism retirement in this pass.

## 2. `backup_folder_key()`

Builds the actual per-device prefs key — see deviceKey's own comment in
misc.h. NULL/empty deviceKey collapses to the bare base key
(pre-2026-07-14 behaviour, and the fallback for any caller with no device
context).
