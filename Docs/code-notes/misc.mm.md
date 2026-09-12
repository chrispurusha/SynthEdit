# misc.mm notes

The longer comments from `misc.mm`, moved here 2026-09-12 so the code reads cleanly. The code points at each as `// notes §k`. Verbatim and in file order; each is titled by what it documents.

## 1. file scope

Everything that doesn't strictly need Objective-C/Cocoa has moved out of this file — the
File/Device/Controls/Layouts/Backup/Restore menus live in appMenuBar.c (actions in menuActions.c
and, for the Load/Store Patch pickers, synthBackup.c), and settings persistence lives in
persistence.c (backed by SynthLib's cross-platform prefs.h rather than NSUserDefaults, and no
longer needing a security-scoped bookmark for the Layouts folder now that App Sandbox is off).
What's left here is genuinely Mac-only: the minimal native app menu Cocoa itself requires,
sleep/wake notifications (NSWorkspace has no cross-platform equivalent in this codebase), and
NSTemporaryDirectory() (still the simplest cross-launch-stable temp dir on macOS regardless of
sandbox).

## 2. `setup_main_menu()`

Sets up the minimal native Cocoa app menu (Quit/About/Hide/Services — GLFW's Cocoa backend
already populates these at index 0), then restores window/dial-mode state from the prefs file
(see load_saved_settings() in persistence.c; settings used to live in NSUserDefaults, now a plain
text file via SynthLib's prefs.h so the same mechanism can work on Windows/Linux too).
File/Device/Controls/Layouts/Backup/Restore menus used to be constructed here too; they're now
the in-window bar built in src/appMenuBar.c on top of SynthLib's menuBar engine. Does NOT call
prefs_init() itself — that now happens much earlier, from main() via init_settings() (misc.h),
before init_graphics() — see that function's own comment for why (synth_init_graphics() needs
the prefs file already loaded well before setup_main_menu() ever runs).
