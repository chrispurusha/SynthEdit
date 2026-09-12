# appMenuBar.c notes

The longer comments from `appMenuBar.c`, moved here 2026-09-12 so the code reads cleanly. The code points at each as `// notes §k`. Verbatim and in file order; each is titled by what it documents.

## 1. `action_open_file()`

── File menu ────────────────────────────────────────────────────────────────
Every single-patch operation lives here regardless of whether the other end is a file or a bank
slot, matching this menu's own pre-port structure (misc.mm, retired): Open/Load paired, then
Save/Store paired, then the three "no edit buffer involved" file<->slot operations.

## 2. `on_clear_name_cache_confirmed()`

A stale on-disk cache (e.g. one written before a category/sort bug fix)
won't self-correct just by relaunching — synth_backup_reload_name_cache_
for_device() happily reloads it right back. This is the manual escape
hatch: wipe it and let the next Load/Store Patch from Bank… re-sweep the
device fresh.

## 3. `PRESET_NUMBER_ITEM_COUNT`

"Save Patch by Number to File..." flyout — a flat 1-128 (Moog) or A001-B128 (Korg) grid of bare
numbers, no names (contrast the richer Load/Store Patch pickers, synthBackup.c, which use
SynthLib's bankBrowser.h instead — this one is meant to be quick). Rebuilt fresh every time the
File menu itself opens (build_preset_number_items(), called from open_file_menu() below) so it
always reflects whichever device is currently connected. Same multi-column-list-of-many-items
pattern src/menus.c's own open_dial_value_menu() already uses (12 rows per column before
wrapping into more columns) for exactly this shape of problem.

## 4. `gDeviceCandidateFilenames`

── Device menu ───────────────────────────────────────────────────────────────
"MIDI Ports..." plus a freshly-scanned device list every time this opens (scan_panel_configs(),
panelConfig.h) — replaces the old NSMenu's cached gDevicesMenu/rebuild_devices_menu() (misc.mm,
retired): SynthLib's menu bar already rebuilds each dropdown fresh on every open, so there's no
separate rebuild-on-folder-change hook needed any more.

## 5. in `action_switch_device()`

index is this item's POSITION within the dropdown (contextMenu.c's handle_context_menu_click()
calls action(index) with the array index it hit-tested against) — NOT the candidate's own
index into gDeviceCandidateFilenames, since "MIDI Ports..." occupies position 0, shifting every
candidate's position one past its actual gDeviceCandidateFilenames slot. Real bug found
2026-07-17 (owner report: device selector picking "the next one in the list") — must read the
candidate index back out of gContextMenu.items[index].param instead, same as every other
per-item-data menu action in this codebase (e.g. src/menus.c's own action_set_dial_value()).

## 6. `on_backup_folder_chosen()`

── Backup / Restore menus ────────────────────────────────────────────────────
Bulk (whole-bank) operations only — every single-patch operation lives in File above. "Bank..."
(a single opaque whole-bank blob, Voyager's own All Presets Dump) has no equivalent on a
Korg-style device — greyed out rather than removed from the menu when the connected device isn't
Moog-style, same as the pre-port NSMenu's own validateMenuItem: behaviour (misc.mm, retired).
"Bank (Individual Files)..." works for both device families, so it's never greyed.

## 7. `action_about()`

NO EXPERIMENTAL MENU ANY MORE (2026-09-09). It held one thing - the OpenGL/Metal choice - and
macOS is Metal only now, so the switch went and the greyed "Renderer: <name>" readout beneath it
was the only item left. A whole top-level menu for one line of information is not worth the width,
and the About box prints the renderer anyway (see synthlib_about_text()).

If something genuinely experimental turns up again, G2-Edit still has the pattern to copy.
