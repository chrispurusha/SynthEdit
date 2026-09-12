# synthBackup.c notes

The longer comments from `synthBackup.c`, moved here 2026-09-12 so the code reads cleanly. The code points at each as `// notes §k`. Verbatim and in file order; each is titled by what it documents.

## 1. `gBackupExpect`

gBackupExpect is written from the main thread (synth_backup_current_patch()/
synth_backup_patch_by_number(), both menu actions) and read/cleared from
the CoreMIDI callback thread (synth_backup_capture_dump(), called out of
synthComms.c's dump handlers) — see midi_read_cb() in midiComms.c for
where that thread comes from. _Atomic for that reason, matching gReDraw's
own treatment elsewhere in this codebase. Stored as plain int, not
tBackupExpect, since not every compiler accepts an enum as an atomic type.

## 2. `gBackupKorgBank`

Korg-style counterpart to gBackupPresetNum above — valid only while
gBackupExpect == eBackupExpectKorgProgram AND no sweep is active (a
standalone synth_backup_patch_by_number_korg() request, not the Load/
Store name-sweep, which tracks its own gKorgSweepIndex instead). Same
"just for the save dialog's default filename" purpose.

## 3. `gKorgSweepActive`

Forward-declared (tentative definition, legal in C — merges with the real
one) so synth_backup_patch_by_number_korg()'s busy-guard just below can
see it; the actual definition and the rest of the Korg sweep state lives
together further down, next to korg_sweep_start().

## 4. `gPendingBackupData`

Set on the CoreMIDI thread just before opening the save dialog, read once
on the main thread inside the dialog's completion callback. Previously
(fileDialogue.mm) no lock was needed here because open_file_write_dialogue_
async() itself dispatch_async'd onto the main queue, and GCD guarantees
everything written on the enqueuing thread before a dispatch_async is
visible to the block it runs. SynthLib's open_file_browser_write()
(fileBrowser.h) has no such internal thread-hop — it's a GLFW/OpenGL
widget whose state may only be touched from the main/render thread (same
requirement as gReDraw/render_file_browser() etc, see fileBrowser.h's own
header comment) — so opening the panel itself now has to move to the
main thread too, via the SAME gPendingBackupSaveReady handoff pattern as
gStoreReplyReady/gBackupBatchReplyReady below: synth_backup_capture_dump()
(CoreMIDI thread) only sets these plain fields and gPendingBackupSaveDefaultName,
then publishes gPendingBackupSaveReady=true LAST; synth_backup_flush_
pending_save() (main/render thread, called once per frame like every
other flush_* function here) is what actually calls open_file_browser_write().

## 5. `gStoreArmedPresetNumber`

── Store Patch to Bank ──────────────────────────────────────────────────────
synth_store_patch_to_bank() (main thread) arms this with the CONFIRMED
destination preset number (1-based; 0 = no Store pending) before requesting
a fresh live dump under the SAME gBackupExpect==eBackupExpectLive banner
synth_backup_current_patch() uses — synth_backup_capture_dump() checks
this first (see its own body) so a fresh reply gets routed to the Store
path instead of opening a save-file dialog. Plain uint32_t, not atomic:
only ever written by the main thread (armed here, cleared by
synth_backup_capture_dump() on the CoreMIDI thread the moment it consumes
it) — same "single owner at a time, flag consumed atomically via the
Ready bool below" discipline as gBackupBatchReplyReady's own handoff.

## 6. `gStoreReplyReady`

CoreMIDI-thread -> main-thread handoff for the fetched bytes, once the arm
above is satisfied — synth_backup_capture_dump() (CoreMIDI thread) copies
the bytes and publishes gStoreReplyReady=true LAST, after the plain writes
above it; synth_backup_flush_store() (main/render thread, called once per
frame) consumes and clears it before doing the actual convert+send, which
needs the main thread (show_confirm()/show_alert(), alertDialog.h, both
assume that). Same shape as
gBackupBatchReplyReady/Data/Len below, just for a single fetch rather than
a 128-preset sweep.

## 7. `gBackupBatchActive`

── Bank-to-folder batch export ──────────────────────────────────────────────
Sequentially requests every preset (1..kBackupBatchPresetCount) and saves
each as its own file — see synth_backup_bank_to_folder()'s own comment
(synthBackup.h) for why this needs a whole state machine rather than the
single fire-and-forget shape every other Backup action uses: Single Preset
Dump can only address one preset per request, and the Voyager answers at
most one outstanding request at a time (won't queue a second reply while
still busy honouring the first — real hardware finding, see
midi_arm_state_dump_debounce()'s own comment in midiComms.c), so the next
request can't go out until either the previous one's reply lands or a
timeout gives up on it.

Threading: gBackupExpect above is already _Atomic and already shared
between the CoreMIDI thread (synth_backup_capture_dump(), called from
synthComms.c's dump handlers) and whichever thread arms a request — that
same discipline extends naturally to gBackupBatchActive/gBackupExpect
here. Everything else below (gBackupBatchCurrentPreset,
gBackupBatchRequestSinceMs, gBackupBatchFolder, the counts) is owned
EXCLUSIVELY by the main/render thread (set up once in
backup_batch_folder_chosen(), then only ever touched inside
synth_backup_flush_bank_to_folder() and its own helpers, called once per
frame from do_graphics_loop()) — the CoreMIDI thread never reads or
writes any of it directly. Instead, a reply is handed off the same way
gPendingBackupData above already is: the CoreMIDI thread copies the bytes
into gBackupBatchReplyData/Len and publishes them with a single
gBackupBatchReplyReady=true store; the render thread's flush consumes
them (and clears the flag) before doing any sequencing. This keeps all
the actual state-machine mutation (and all the file I/O) on one thread,
avoiding a two-writer race between "a reply arrived" and "this preset
timed out" advancing the same state concurrently.

## 8. `gBackupBatchNextRequestMs`

>0.0 while paced-waiting for the NAME-SWEEP mode's next request to go out
(see NAME_SWEEP_PACING_MS) — never used by eBatchModeExportFiles, which
always re-requests immediately, same as before pacing existed. 0.0 means
"not waiting" (either a request IS currently in flight, tracked by
gBackupBatchRequestSinceMs above, the sweep isn't active, or this is an
export). This is exactly what synth_backup_sweep_request_in_flight()
below reports for the Moog side.

## 9. `tBackupBatchMode`

Added 2026-07-11 for the Load/Store Patch to Bank name-sweep pickers
(synth_backup_start_name_sweep() below) — reuses this WHOLE sequencing
mechanism (gBackupBatchActive, the CoreMIDI-thread-copies/main-thread-
sequences reply handoff, the per-preset timeout) rather than duplicating
it, since the only real difference is what happens with each reply: write
a file (eBatchModeExportFiles, the original behaviour) or just decode a
name into gNameSweepLabels (eBatchModeNameSweep). synth_backup_capture_dump()'s
existing `gBackupBatchActive` check (CoreMIDI thread) doesn't need to know
which mode is active at all — only backup_batch_write_capture() and
backup_batch_advance()'s completion step (both main-thread) branch on it.

## 10. `BACKUP_BATCH_TIMEOUT_MS`

Lowered from 1500 to 1000ms 2026-07-14 (owner, comparing against an
independently-developed third-party Voyager editor — moogvoyagereditor.
pistolinstruments.com — whose own bank-fetch loop uses just a 250ms max
wait per request): still generous relative to a real reply's actual
latency (well under 100ms in practice) — a fallback for a genuinely
non-responding location or one that's slower than usual, not the
normal-case wait. Retry (NAME_SWEEP_MAX_RETRIES) means a single slow
reply now costs at most ~1s extra rather than ~1.5s before a slot is
retried or given up on.

## 11. `NAME_SWEEP_LABEL_LEN`

"128: " (5) + a generously-truncated single-line name + " — " + a
Category name (up to "Guitar/Plucked", 15 chars) + NUL — widened from 40
to 64 on 2026-07-14 once the picker started showing category alongside
the name (synth_decode_korg_category()/synth_decode_moog_category(),
synthComms.h) for BOTH device families; shared by gNameSweepLabels
(Moog) and gKorgSweepLabels (Korg) below — one constant, one width, no
reason for the two mechanisms to disagree on it.

## 12. `NAME_CACHE_JOIN_CHAR`

Joins gNameSweepLabels/gKorgSweepLabels entries into one prefs.h string
value for on-disk caching (name_cache_save_to_disk()/
name_cache_load_from_disk() below) — an ASCII record separator, not '\n':
prefs.cpp's own file format is one "key=value" per line (std::getline()),
so an embedded real newline in a VALUE would corrupt the line-based
parse; this byte never collides with that (each entry is already
guaranteed newline-free — name_cache_set_label()/korg_sweep_set_label()
collapse any embedded '\n' from the device to a space) nor with '=' (only
the FIRST '=' on a line ends the key, so one appearing inside the joined
value, e.g. a patch name containing "=", is harmless).

## 13. `NAME_CACHE_FIELD_CHAR`

Separates a slot's name from its category index WITHIN one NAME_CACHE_JOIN_CHAR-delimited
record (an ASCII unit separator, one level down from the record separator above) — added
2026-07-24, since gNameSweepCategoryIndex/gKorgSweepCategoryIndex previously lived in memory
only, so Category sort/grouping quietly reverted to "no category" every relaunch even though
the name cache itself survived. Category is stored as 2 hex digits (0x00-0xFF); a record with
no field char at all (an older on-disk cache written before this fix) is still readable — it
just decodes as "no category" (0xFF) for that slot, same as this fix's own initial default.

## 14. `NAME_SWEEP_PACING_MS`

Shared by BOTH name-sweep mechanisms (Moog gBackupBatchMode==
eBatchModeNameSweep below, and the Korg-only gKorgSweep* block further
down) — made common 2026-07-14 (owner: "these should be common
mechanisms with Voyager and any other device... should be common and
generic"). Originally Korg-only, added the same day: this sweep no
longer has anything to hurry for (the picker opens immediately
regardless of progress, see name_sweep_show_picker()'s own comment), so
it trickles one request every NAME_SWEEP_PACING_MS rather than firing as
fast as replies come back, staying out of the way of whatever MIDI
traffic normal interactive use (dial drags, etc.) is already generating.
128-256 requests x 500ms ≈ 1.1-2.1 minutes to fully populate — fine for
a background fill nobody's waiting on. Deliberately NOT applied to the
Moog bank-to-folder EXPORT mode — that's a foreground action with its
own progress modal the user is actively watching complete, not a quiet
background fill; pacing it would just make a real backup take longer
for no benefit. (Korg's own foreground export, eKorgSweepModeExportFiles,
DOES still share this pacing rather than getting the same fast-path
treatment — an existing asymmetry with the Moog export, not something
this change addresses.)

Lowered 600ms -> 500ms 2026-07-14, owner's own request ("We could
probably read the banks a little faster") after confirming a real Z1
restore's apparent data problem was actually just a too-soon read-back
racing the device's own flash-write settling (see [[project_z1_restore_folder_pacing]])
— a READ (this constant) has no equivalent flash-commit wait on the
device side, just formulate-and-send-a-reply, so it never carried the
same risk the restore-side pacing did. Still a guess, not hardware-
measured — tighten further or back off based on how this actually
behaves.

## 15. `NAME_SWEEP_MAX_RETRIES`

How many times a timed-out request gets resent before its slot is
finally marked missing/"(no response)" — shared by both name-sweep
mechanisms AND the Moog bank-to-folder export (a genuine backup
benefits from this too: fewer real "(write failed)" entries from what
might just be one slow reply, not a truly unresponsive location).
Defence in depth alongside the mutual-exclusion fix
(synth_backup_sweep_request_in_flight()): a real "(no response)" was
traced 2026-07-14 (owner report) to dial-tweak traffic colliding with an
in-flight sweep request, which that fix targets directly, but a genuine
one-off miss (packet loss, a slow reply for some other reason) is still
possible — worth one or two retries before giving up rather than none.

## 16. `gNameSweepCategoryIndex`

Parallel to gNameSweepLabels above — the raw Category dial index for the
SAME preset (0xFF = none/unknown, matching bankBrowser.h's own "no
category" sentinel), fed into tBankBrowserItem.category so
name_sweep_show_picker() below can offer a real Category sort mode
instead of the disabled placeholder that shipped with the initial
open_bank_browser() port (owner report, 2026-07-24: "can't select
Category on Load Patch from Bank for voyager"). gNameSweepLabels itself
no longer bakes "N: "/"— Category" into the string (see
name_cache_set_label()'s own comment for why: the browser already
prepends "Bank X, Loc Y: " for display, and an embedded location number
as the FIRST characters of every label was quietly breaking the
browser's own A-Z sort mode — it compares tBankBrowserItem.name as plain
text, so "10: ..." sorted before "2: ..." lexicographically, i.e. by
location number, not name).

## 17. `gNameCacheValid`

True once a full 128-preset name sweep has completed at least once this
session — synth_backup_start_name_sweep() below skips straight to
showing the picker with the cached gNameSweepLabels instead of re-running
the whole ~128-request sweep every single time Load/Store Patch to Bank
is opened (owner request, 2026-07-11: "we could consider caching the
names once pulled from the bank"). Kept up to date for everything THIS
APP can write to a known slot — see name_cache_update_from_preset_dump()
below, called after a successful synth_store_patch_to_bank() send,
Restore > Patch by Number, and each per-file send in Restore > Bank
(Individual Files); a whole-bank Restore (single opaque 18KB blob, no
safe way to extract 128 individual names from it — see this session's
own Pot Map lesson on not hand-deriving unconfirmed byte layouts)
invalidates the ENTIRE cache instead, forcing a fresh sweep next time.
Session-scoped only (not persisted to disk, reset on relaunch) — matches
gLastMoogDump and every other cache this file already keeps in memory
only. Explicit gap, not silently assumed away: a name changed via the
device's OWN front panel (SAVE PRESET with a different name than before)
has no notification mechanism this app can observe without polling —
and this codebase already found the hard way that polling interrupts
the Voyager's own front-panel menus (see
[[project_voyager_selector_dial_audit]] in the assistant's own memory
notes) — so that particular staleness is accepted, not solved.

## 18. `backup_sanitize_name_for_file()`

Sanitizes gDevice.progName into a filename-safe single line — shared by
the single "Patch by Number" save dialog's default name (below) and the
bank-to-folder batch export (synth_backup_flush_bank_to_folder() below).
gDevice.progName may contain embedded '\n's (nameLineWidth — see the
tPanelConfig field comment in panelConfig.h) marking where the source
device's own display wraps to a new line. A filename has no such notion
of lines, so each '\n' becomes exactly one space — UNLESS one's already
there (a short first line like "TIME FOR" already has a real trailing
space from its own padding — see extract_moog_name()'s comment,
synthComms.c), in which case the '\n' is just dropped rather than
doubling it up to two. Either way the two lines always end up separated
by exactly one space in the filename, even for a name like "Floating
Mod"/"Steel Guitar" whose raw data has nothing at all between them (both
lines fill their full width) — a filename reads better with a word break
there even though the sysex itself doesn't have one; the byte-exact raw
name is preserved in the saved file regardless.

'/' becomes '-' — REAL bug found+fixed 2026-07-11: a patch literally
named "Tiny w/o Mod" made backup_batch_write_capture()'s own fopen() call
silently fail (macOS treats '/' as a path separator, so the constructed
path implied a subdirectory that doesn't exist), leaving preset 23's
slot with no exported file at all and just a "(write failed)" line in
Patches.txt — invisible until a later Restore > Bank (Individual Files)
tried to restore that folder and correctly skipped the missing file,
which is what actually surfaced the gap. No other character a decoded
name can contain is unsafe at the POSIX/fopen() level (extract_moog_name()
already collapses anything below 0x20 to a space during decode, so only
printable ASCII 0x20-0x7E ever reaches here — '/' is the one member of
that range the filesystem itself rejects).

out must be at least as big as `name`'s own buffer; writes "" if name is
NULL/empty. Takes the name explicitly (not just gDevice.progName) since
the Korg export sweep (korg_sweep_write_capture_file() below) needs to
sanitize a SPECIFIC preset's own decoded name — handle_prog_dump() never
touches gDevice.progName at all (see korg_sweep_capture_reply()'s own
comment for why), so that global has nothing useful to read during a Korg
sweep the way it does for Moog's own bank-to-folder export.

## 19. `backup_index_file_path()`

Builds "<folder>/Patches-<DeviceName>.txt" — shared by every Backup >
Bank (Individual Files)… writer and Restore > Bank (Individual Files)…
reader, Moog and Korg alike. Device-specific rather than a flat
"Patches.txt" so backing up a SECOND device into a folder that already
holds a first device's export doesn't silently truncate that first
device's own index — found 2026-07-14 (owner report): the actual .syx
filenames already don't collide across device families (Moog's "001
Name.syx" vs Korg's "A001 Name.syx"), but a single hardcoded index
filename meant the SECOND backup's own fopen(..., "w") destroyed the
FIRST one's index outright, orphaning its files — still physically on
disk, but with nothing left that could find them again. Reuses
backup_sanitize_name_for_file() above for the same '/'-is-a-path-
separator reasoning that already applies to a preset name landing in a
filename. Does NOT disambiguate two physical units of the SAME model
(e.g. two Voyagers) backed into one folder — there's no per-unit ID in
either protocol to key on, so that case still needs separate folders,
same as it always has.

## 20. `warn_if_not_connected()`

── "No device" guard for the File/Backup menu entry points ──────────────────
Every one of those actions needs the device: there is nothing to read a patch from, and nowhere
to send one. Each used to LOG_ERROR and return, which made the menu item look broken — you click
"Open File...", no browser appears, no message, nothing (owner report 2026-07-26; the file
browser genuinely never opens because synth_backup_restore_edit_buffer() bails before
open_file_browser_read()). Same silent-failure class the G2-Edit reverse-queue work closed by
routing op results to show_alert().

Returns TRUE when there is NO device, so call sites read as `if (warn_if_not_connected(...)) {
return; }` — the truthy case is the bail-out, matching the shape of the guard it replaced.

Deliberately NOT applied to three kinds of caller: the scripted *_from_path variants (they run
unattended from the command-file harness in graphics.cpp, where a modal would hang the run),
synth_backup_flush_background_prefetch() (a background tick — an alert per tick would be
unusable), and the synthComms.c helpers reached only from the bank browser after a successful
sweep, which by construction had a device moments earlier and would otherwise double-report.

## 21. `gPendingStoreBank`

Stashed by synth_store_patch_to_bank() immediately before opening the
(now-asynchronous) confirmation dialog, for its two confirmed-callbacks
below to pick back up — same CoreMIDI-thread/main-thread-style handoff
convention this file already uses for gStoreArmedPresetNumber/
gPendingBackupData above, just for a confirm-dialog completion here
instead of a CoreMIDI reply. Both branches of synth_store_patch_to_bank()
(Korg/Moog) are only ever entered from the main thread (a menu action, or
the Load/Store bank-browser picker's own confirmed-callback), so these
don't need to be atomic themselves.

## 22. `on_store_patch_to_bank_korg_confirmed()`

Confirmed-callback for the Korg-style (Z1) branch — a single PROGRAM
WRITE REQUEST (func 0x11), no local fetch/convert step or async reply to
wait for (see synth_send_korg_program_write_request()'s own comment,
synthComms.h). "Store..." rather than a generic "Continue" — matches
what this action actually does.

## 23. `on_store_patch_to_bank_moog_confirmed()`

Confirmed-callback for the Moog-style branch — arms gStoreArmedPresetNumber
and requests a fresh state dump, exactly as the old synchronous
continuation did; synth_backup_flush_store() takes it from here once that
dump's reply lands.

## 24. in `synth_store_patch_to_bank()`

Korg-style (Z1): a single PROGRAM WRITE REQUEST (func 0x11) — no
local fetch/convert step at all, so there's no async reply to wait
for before showing a result (see synth_send_korg_program_write_
request()'s own comment, synthComms.h, on why the device's own
WRITE COMPLETED/ERROR reply isn't surfaced here yet). Branches
BEFORE the confirmation dialog's own wording, since the two
protocols address a slot differently (bank+number vs number alone).

## 25. `synth_backup_patch_by_number()`

synth_backup_flush_store() is defined further down, right after
convert_panel_dump_to_preset_dump() (which it calls) — this file's
existing convention is helpers-before-use with no forward declarations,
not scattered function order, so it lives next to that helper rather than
up here with the other single-shot Backup/Store triggers.

## 26. in `synth_backup_patch_by_number()`

gBackupBatchActive here catches the background name-sweep (both it and
this lone fetch share eBackupExpectPreset) — without this, a reply
arriving while the sweep owns the floor gets routed into the sweep's
own branch in synth_backup_capture_dump() (matched purely by kind,
not by WHICH preset was asked for) instead of this fetch's save-
dialog path, silently discarding the request. Found 2026-07-14 while
building the Korg twin of this function just below. Same reasoning
as korg_sweep_show_picker()'s own guard against the reverse case
(comment further down, "gBackupBatchActive/gBackupExpect==eBackupExpectPreset...").

## 27. in `synth_backup_patch_by_number_korg()`

gKorgSweepActive here catches the background name-sweep — same race
as synth_backup_patch_by_number()'s own guard just above (it and this
fetch share eBackupExpectKorgProgram), found the same way. Without
this, a reply arriving mid-sweep gets misattributed to whatever
program the sweep itself is currently waiting on, and this fetch's
own request just silently never gets its save dialog.

## 28. `backup_batch_append_index_line()`

Appends one line to <folder>/Patches.txt — reopened in append mode per
call rather than held open across the whole sweep, so a crash or force
quit mid-export leaves whatever's been captured so far intact and
readable rather than an unflushed/truncated file.

## 29. `name_sweep_show_picker()`

Defined further down, after synth_store_patch_to_bank() (which it may
call) — forward-declared here since backup_batch_advance()'s completion
step below is the only caller and needs to reach it regardless of
definition order.

## 30. `name_cache_save_to_disk()`

Defined further down, alongside name_cache_set_label() — forward-declared
here since backup_batch_advance()'s completion step below (and
name_cache_update_from_preset_dump(), also further down) both need to
reach it regardless of definition order.

## 31. `backup_batch_request_current()`

Sends (or resends, for a retry) the Single Preset Dump Request for
gBackupBatchCurrentPreset — shared by backup_batch_advance() (export
mode's own immediate re-request) and synth_backup_flush_bank_to_folder()
below (the paced name-sweep re-request, and any mode's timeout retry),
so all three send exactly the same way rather than three near-duplicate
copies of these three lines.

## 32. `moog_name_sweep_start()`

Starts a fresh Moog name sweep — every label initialised to "N: ---" so
name_sweep_show_picker() has something sane to show for whatever hasn't
been swept yet, whichever way it was opened. Shared by
synth_backup_start_name_sweep() (an explicit Load/Store click that found
nothing running yet) and synth_backup_flush_background_prefetch() below
(silently, soon after connecting) — the Moog counterpart to
korg_sweep_start() further down. Caller is responsible for having already
confirmed nothing else is using gBackupBatchActive/gBackupExpect.

## 33. `backup_batch_advance()`

Moves on to the next preset, or finishes the sweep once every preset has
either been captured or timed out. Called only from the main/render
thread (synth_backup_flush_bank_to_folder() below and its own helpers) —
see the batch state block's own comment above for why.

## 34. in `backup_batch_advance()`

Without this, the progress overlay (synth_render_backup_progress(),
synthGraphics.cpp) would only repaint whenever something UNRELATED
happened to set gReDraw (a mouse move, etc.) — do_graphics_loop()
only calls render_frame() at all when gReDraw is true, so a sweep
this function drives entirely on its own otherwise looks frozen on
screen even while genuinely progressing. Found 2026-07-11 while
adding the overlay itself, not from a bug report.

## 35. in `backup_batch_advance()`

Flush what has been collected so far, so quitting or crashing part-way through a sweep does
not throw away every name fetched to that point. Batched rather than per-reply because each
save rewrites the whole cache file; at 16 the worst case loses 15 slots' work, which is a few
seconds of sweeping. Only the name sweep does this - a full bank export has its own file
output and does not want the extra writes.

## 36. in `backup_batch_advance()`

No auto-popup here (unlike before 2026-07-14) — Load/Store
Patch from/to Bank now opens the picker immediately whether
or not this sweep has finished, see name_sweep_show_picker()'s
own comment; popping a native modal unprompted, whenever this
sweep happens to finish (background or not), would steal
focus while the user's doing something completely unrelated.

## 37. in `backup_batch_advance()`

handle_moog_single_preset_dump() (synthComms.c) only ever touches
gDevice.progName (extract_moog_name() at presetNameOffset) — the
sweep above leaves it showing the LAST preset's stored name
instead of the live edit buffer's. Re-requesting the live state
restores it (and re-syncs every dial, belt and braces), same
"ask for current state" idea as synth_navigate_preset()'s own
post-navigation refresh.

## 38. in `backup_batch_advance()`

Paced, not immediate — see NAME_SWEEP_PACING_MS's own comment.
The actual send happens on a later
synth_backup_flush_bank_to_folder() tick once this elapses.
Export mode (below) is unaffected — always re-requests right away.

## 39. `name_cache_set_label()`

Writes gNameSweepLabels[presetNumber-1] as just the bare (unnamed)-or-real
patch name, and gNameSweepCategoryIndex[presetNumber-1] with categoryIndex
— ready to hand straight to open_bank_browser() (a tBankBrowserItem's own
name/category fields). Deliberately NOT "N: Name — Category" any more
(2026-07-24 fix, see gNameSweepCategoryIndex's own comment for the two
bugs that format caused) — the browser already shows "Bank X, Loc Y: "
itself, and Category is now conveyed structurally via categoryIndex
instead of baked into display text. Shared by every path that learns a
preset's current name: the name sweep itself (name_sweep_capture_name()
below), and every KEEP-THE-CACHE-CURRENT call site
(name_cache_update_from_preset_dump() below) — see gNameCacheValid's own
comment for the full list. categoryIndex is 0xFF (bankBrowser.h's own "no
category" sentinel) wherever this device has no Category dial, or the
caller has none on hand (synth_backup_note_preset_name()'s own comment).

## 40. in `name_cache_set_label()`

A Voyager name can carry an embedded '\n' (nameLineWidth's forced line
break, matching the front panel's own 2-line LCD — see
synth_decode_moog_name()'s own comment) — fine for gDevice.progName's
on-screen multi-line display, but this is a SINGLE-LINE dropdown item;
a literal newline in an NSPopUpButton title just renders broken.
Collapsed to a space, same substitution backup_sanitize_name_for_file()
already does for the same underlying reason (a different output
format — filenames — but the same "no embedded newline" constraint).

## 41. `name_cache_set_complete()`

Persists gNameSweepLabels to disk (prefs.h — a plain per-OS key=value
settings file, see prefs.cpp), keyed by the currently-loaded device's own
deviceName so switching devices (synth_backup_reload_name_cache_for_device()
below) never shows one device's cached names for another. Called both at
full-sweep completion (backup_batch_advance()) and from
name_cache_update_from_preset_dump() below on every single-slot
opportunistic update — the latter is deliberately not rate-limited: a
single prefs.h rewrite is cheap relative to this app's own
NAME_SWEEP_PACING_MS, and correctness (never showing a stale on-disk name
for a slot this app just confirmed) matters more here than avoiding a
handful of extra small writes during a background sweep.
The cache is now flushed part-way through a sweep (see NAME_CACHE_SAVE_INTERVAL at its use site),
so "there is a blob on disk" no longer implies "the sweep finished". This separate key records
that, and gates gNameCacheValid on load — otherwise an interrupted sweep would come back looking
complete and its missing slots would never be re-fetched.

## 42. `name_cache_load_from_disk()`

Counterpart to name_cache_save_to_disk() above — populates gNameSweepLabels
from whatever was last saved for the currently-loaded device, and marks
the cache valid (gNameCacheValid) if anything was actually found, so
synth_backup_start_name_sweep() can skip straight to showing the picker
without a live sweep, the same as an in-session cache already does. A
no-op (cache stays invalid) if this device has never been swept before —
cache_get_string()'s own "" default reads as "nothing saved yet".

## 43. `name_cache_clear_disk()`

Clears the on-disk cache for the current device (name_cache_save_to_disk()
above) — called wherever gNameCacheValid itself is set back to false (a
whole-bank Restore, whose own comment explains why the in-memory names are
no longer trustworthy). Without this, a later relaunch would otherwise
still find yesterday's now-stale prefs.h entry and happily load it back
in via name_cache_load_from_disk(), silently undoing the invalidation the
moment this session ends.

## 44. `name_cache_update_from_preset_dump()`

Decodes the NAME and CATEGORY out of a Single-Preset-Dump-shaped buffer
(mode 0x03 — a raw file about to be sent, or one already sent) and
updates the name cache for presetNumber via name_cache_set_label() above.
Shared by every write path that knows exactly which slot it just wrote
and has the preset-dump bytes on hand: synth_backup_flush_store() (this
app's own live-edit-buffer write), restore_patch_file_chosen() (Restore >
Patch by Number), and each per-file send in
synth_backup_flush_restore_folder() (Restore > Bank Individual Files).
Does NOT touch gDevice.progName — same "don't disturb what the live
buffer is showing" reasoning as name_sweep_capture_name() below.

## 45. `name_sweep_capture_name()`

Decodes just the NAME out of a Single Preset Dump reply into
gNameSweepLabels[gBackupBatchCurrentPreset-1] — the eBatchModeNameSweep
counterpart to backup_batch_write_capture() below. Uses
synth_decode_moog_name() directly (not gDevice.progName) so this doesn't
disturb whatever the live edit buffer's own name is currently showing
on-screen while the sweep runs.

## 46. in `backup_batch_folder_chosen()`

Fresh index file each run — truncates any previous one from an
earlier export into the same folder rather than appending onto
stale content. Header identifies which device/when this export is
from (a bare list of numbers+names on its own gave no way to tell
two exports' Patches.txt files apart, or a genuine device patch list
apart from one of these) — backup_batch_append_index_line() (below)
only ever appends after this, never touches the header itself.

## 47. `synth_backup_note_preset_name()`

synth_backup_bank_to_folder() itself lives further down, after the Korg
name-sweep block (needs gKorgSweepActive/korg_batch_folder_chosen(), both
declared there) — its own comment there explains why, mirroring
name_sweep_show_picker()'s own forward-declaration precedent above.

## 48. `KORG_SWEEP_PRESET_COUNT`

── Korg-style name sweep (Z1: 2 banks x 128 programs) ───────────────────────
Deliberately a SEPARATE, parallel mechanism from the gBackupBatch* state
above, not a retrofit of it — that machinery is built entirely around
Voyager's own Single Preset Dump request/reply shape
(synth_request_single_preset_dump(), BACKUP_BATCH_PRESET_COUNT fixed at a
single 128-slot bank) and is ALSO shared with the Moog-only folder
export/restore sweeps; bumping BACKUP_BATCH_PRESET_COUNT to 256 to fit a
second bank would make every Moog-only sweep spend half its time
requesting presets 129-256 that don't exist on a single-bank Voyager.
Keeping this fully isolated means the working, hardware-confirmed Moog
mechanisms above are completely unaffected. Added 2026-07-14.

## 49. `tKorgSweepMode`

Same duality as tBackupBatchMode (Moog, above) — reusing this SAME sweep
for Backup > Bank (Individual Files)… on a Korg-style device rather than
building a third parallel mechanism just to write files instead of
decoding names. eKorgSweepModeExportFiles branches korg_sweep_capture_reply()
below into ALSO writing the just-captured reply to its own file (still
decoding the name either way — needed for the filename itself, and it's
a free bonus for gKorgSweepLabels/the Load-Store picker afterwards).
Added 2026-07-14.

## 50. `gKorgSweepNextRequestMs`

>0.0 while paced-waiting for the next request to go out (see
NAME_SWEEP_PACING_MS) — no request is in flight during this wait, so
synth_backup_flush_korg_name_sweep()'s reply/timeout checks don't apply
until it elapses. 0.0 means "not waiting" (either a request IS currently
in flight, tracked by gKorgSweepRequestSinceMs above, or the sweep isn't
active at all) — this is exactly what synth_backup_sweep_request_in_flight()
below reports.

## 51. `gKorgRestoreFolderActive`

State for the Korg restore-folder mechanism (Restore > Bank (Individual
Files), in reverse) — declared here, alongside the rest of the Korg
sweep's own state, rather than down next to that mechanism's own
functions (which is where the equivalent Moog state lives, right above
its own functions) purely so synth_backup_start_name_sweep()'s Korg
guard above can reference gKorgRestoreFolderActive without a forward-
declaration; the functions using the rest of this block still live down
with the Moog restore-folder mechanism's own counterpart, see that
section's own header comment.

## 52. `synth_backup_sweep_request_in_flight()`

True only for the narrow window between sending a sweep request and
either its reply arriving or its timeout firing — NOT true during the
slow paced gap between requests (see gKorgSweepNextRequestMs/
gBackupBatchNextRequestMs's own comments). Covers BOTH name-sweep
mechanisms (Korg gKorgSweep* here, Moog gBackupBatchMode==
eBatchModeNameSweep) — made common 2026-07-14 (owner: "these should be
common mechanisms with Voyager and any other device... should be common
and generic"), originally Korg-only, added the same day after an owner
report of spurious "(no response)" entries traced to dial-tweak traffic
colliding with an in-flight sweep request. Exposed so
synthComms.c's synth_set_panel_dial_value() can defer an outgoing CC or
Parameter Change while this is true, rather than sending it right into
the collision window. Deliberately does NOT cover a sweep's WHOLE
lifetime — that would add real (if small) latency to every dial tweak
for the 1.3-2.5 minutes a background sweep runs, when only the brief
per-request round trip (well under 100ms in practice, same figure
BACKUP_BATCH_TIMEOUT_MS's own comment already relies on) actually needs
it. Also deliberately does NOT cover the Moog bank-to-folder EXPORT mode
— see NAME_SWEEP_PACING_MS's own comment for why that's out of scope.

## 53. `korg_sweep_advance()`

Advances to the next (bank, program), or finishes the sweep — the Korg
counterpart to backup_batch_advance() above, called only from the main/
render thread (synth_backup_flush_korg_name_sweep() below). Never opens
the picker itself (unlike the Moog counterpart below) — see
korg_sweep_show_picker()'s own comment for why that's now a fully
separate concern from sweep progress.

## 54. in `korg_sweep_advance()`

Even a slot that timed out keeps its "(no response)" label — good
enough to skip re-sweeping, same acceptance as gNameCacheValid's
own comment above. True either way: an export sweep decodes every
name/category exactly like a name-only one (korg_sweep_capture_reply()'s
own comment), so it's a free, valid cache-populating side effect,
not something only a "real" name sweep should set.

## 55. `korg_sweep_append_index_line()`

Appends one line to <folder>/Patches.txt in the Korg sweep's own "A001  Name"
format (bank letter + 3-digit zero-padded program, TWO spaces, then name)
— the Korg counterpart to backup_batch_append_index_line() above (Moog).
Reopened per call rather than held open across the whole sweep, same "a
crash or force quit mid-export still leaves what's captured so far intact
and readable" reasoning as that function's own comment.

## 56. `korg_sweep_write_capture_file()`

Writes a just-captured Program Data Dump reply to its own file — the Korg
counterpart to backup_batch_write_capture() above (Moog). Called only
when gKorgSweepMode == eKorgSweepModeExportFiles, from
korg_sweep_capture_reply() below (which has already decoded `name`, so
this doesn't need its own separate decode step). Owns Replied/Missing
counting for this reply itself — the caller does NOT also increment
those for export mode, only for the name-sweep-only path.

## 57. `korg_sweep_set_label()`

Writes gKorgSweepLabels[(bank?128:0)+(prog-1)] as just the bare
(unnamed)-or-real patch name, and gKorgSweepCategoryIndex at the same
index with categoryIndex — the Korg counterpart to name_cache_set_label()
above (Moog); see that function's own comment for why this no longer
bakes "A1: "/"— Category" into the label text. Shared by
korg_sweep_capture_reply() below (a sweep in progress) and
korg_restore_patch_file_chosen() further down (keeping the cache accurate
after a Restore write, without needing a full re-sweep — same "no extra
guard needed, a no-op-safe write either way" reasoning as
name_cache_update_from_preset_dump()'s own comment).

## 58. `korg_name_cache_set_complete()`

Korg counterpart to name_cache_save_to_disk() above — see that function's
own comment for the reasoning (prefs.h, keyed by device, called at both
sweep completion and every single-slot opportunistic update).
Korg counterparts to name_cache_set_complete()/name_cache_is_complete() above — same reasoning:
the cache is flushed mid-sweep now, so a blob on disk no longer implies the sweep finished.

## 59. `synth_backup_reload_name_cache_for_device()`

Called from synth_reload_panel_config() (synthGraphics.cpp) — once at
startup and again every time the user picks a different device from the
Device menu. Resets both in-memory caches first: the arrays/valid flags
just populated (or left over from) whichever device was loaded a moment
ago describe THAT device's presets, not the new one, and must never be
shown against it. Then attempts to load whichever on-disk cache exists for
the newly-loaded device — harmless to attempt both Moog- and Korg-style
unconditionally, since a real device is only ever one or the other, so
the wrong one's prefs.h lookup just finds nothing and no-ops.

## 60. `synth_backup_clear_name_cache_for_device()`

Menu-driven equivalent of the above, minus the disk reload — wipes both
in-memory caches AND the on-disk entry for the current device (harmless to
clear both Moog- and Korg-style keys unconditionally, same reasoning as
synth_backup_reload_name_cache_for_device()'s own comment), then leaves
both caches empty/invalid so the very next Load/Store Patch from Bank…
falls back to a fresh live sweep instead of quietly resurrecting whatever
was just cleared. Added 2026-07-24 so a stale cache written before a
category/sort bug fix (e.g. one baked with the old "N: Name — Category"
label format) can be discarded without deleting prefs.txt by hand.

## 61. `korg_name_cache_update_from_dump()`

Decodes name+category from an arbitrary Program Data Dump buffer (not
necessarily gKorgSweepIndex's own — a Restore write, e.g.) and updates
its label via korg_sweep_set_label() above. Shared by
korg_restore_patch_file_chosen() and synth_backup_flush_korg_restore_folder()
further down, both of which need this exact "decode this one buffer,
touch the cache for this one slot" step outside the sweep itself.

## 62. `korg_sweep_capture_reply()`

Decodes the name AND category from a captured Program Data Dump reply and
updates its gKorgSweepLabels entry via korg_sweep_set_label() above — the
Korg counterpart to name_sweep_capture_name() above. Uses
synth_decode_korg_name()/synth_decode_korg_category() directly (not
gDevice.progName) so this doesn't disturb whatever the live edit
buffer's own name is currently showing on-screen while the sweep runs.
Also drives the Backup > Bank (Individual Files)… write when
gKorgSweepMode == eKorgSweepModeExportFiles — the name/category decode
above is needed either way (for the label AND the export filename), so
there's no separate "export capture" entry point; this is it for both.

## 63. `synth_backup_flush_korg_name_sweep()`

Per-frame poll — the Korg counterpart to synth_backup_flush_bank_to_folder()
above, but only ever drives THIS sweep (no folder-export mode to share
with, unlike the Moog version). A no-op unless a Korg name sweep is
actually active. Call once per frame from the render loop.

## 64. in `synth_backup_flush_korg_name_sweep()`

Retry the SAME slot a couple of times before giving up on it — see
NAME_SWEEP_MAX_RETRIES's own comment. gKorgSweepIndex deliberately
doesn't advance here; korg_sweep_request_current() just re-sends
for the current one with a fresh timestamp.

## 65. `korg_sweep_start()`

Starts (or restarts, on a fresh connection) the Korg name sweep — called
either by synth_backup_flush_background_prefetch() below (silently, soon
after connecting) or synth_backup_start_name_sweep() (an explicit Load/
Store click that found no sweep running yet). Every label starts as
"A1: ---" so korg_sweep_show_picker() has something sane to show for
whatever hasn't been swept yet, whichever way the picker was opened.

## 66. `korg_batch_folder_chosen()`

Runs on the main thread once the user has chosen (or cancelled) a backup
folder — the Korg counterpart to backup_batch_folder_chosen() above
(Moog). Deliberately does NOT call korg_sweep_start() (that's always the
plain name-only entry point — see its own comment) — sets up the SAME
underlying gKorgSweep* state directly, in export mode, mirroring
backup_batch_folder_chosen()'s own inline setup rather than sharing a
helper neither side really needs.

## 67. in `synth_backup_bank_to_folder()`

Korg-style (Z1): fully separate sweep/folder-chosen path — see the
Korg name-sweep block's own header comment for why this reuses that
SAME sweep mechanism (in export mode) rather than gBackupBatch* below,
which is built entirely around Voyager's own Single Preset Dump
request/reply shape and a fixed 128-slot bank. Added 2026-07-14.

## 68. `on_korg_sweep_picked()`

Confirmed-callback for korg_sweep_show_picker()'s bank browser below — the
picker hands back the chosen bank/location directly (no flat index to
re-derive bank/prog from any more, unlike the old show_device_choice_
dialogue()'s single chosen int). No synth_request_state_dump() here
(removed 2026-07-14) — handle_prog_dump() never touches gDevice.progName/
the live dials in the first place (see its own comment, synthComms.c), so
there's nothing to restore on cancel; on Load, synth_change_program()
(called via synth_korg_select_program(), the tail of synth_load_patch_
from_bank()'s Korg branch) already arms its own debounced state-dump
refresh, so an immediate extra request here was pure duplicate traffic —
and, worse, one that could land right as a background sweep's own
request/reply was in flight (owner report: one observed "patch select
failing" while a sweep was running).

## 69. `korg_sweep_show_picker()`

Shows the picker and acts on whatever the user chose — the Korg
counterpart to name_sweep_show_picker() below (kept fully separate rather
than parameterizing that one, same isolation reasoning as the rest of
this block). Opens IMMEDIATELY regardless of sweep progress — 2026-07-14
user request ("allow the picker, with '---' unpopulated names before the
full set is gleaned") — showing real names for whatever
korg_sweep_capture_reply() has filled in so far and "---" (from
korg_sweep_start()'s own initialisation) for anything not reached yet.
Ported from a synchronous NSAlert+dropdown (show_device_choice_
dialogue()) to SynthLib's open_bank_browser() (a scrollable named list —
better than a raw dropdown for 256 entries). Also, being asynchronous,
this no longer pauses the background sweep while the picker is open the
way the old blocking modal did (see synth_backup_flush_korg_name_sweep(),
still driven every frame while this panel is up) — labels now keep
filling in live underneath it, an improvement rather than a regression.
Lost along with the synchronous dialog: open_bank_browser() has no
"default selected row" parameter, so Store no longer pre-selects
gDevice.currentProgram's own slot — a minor, accepted UX simplification
(see this function's git history for the dropped defaultIndex
calculation).

Real categoryNames/per-item category wired in 2026-07-24 — see
name_sweep_show_picker()'s own comment (this function's Moog
counterpart) for the two bugs the original category=0/categoryNameCount=0
port caused and why this fixes both.

## 70. in `synth_backup_start_name_sweep()`

Blocks a genuinely CONFLICTING operation — a Korg restore-folder
sweep (gKorgRestoreFolderActive) or this SAME gKorgSweep*
mechanism actually mid-EXPORT (writing files, not just decoding
names for the cache) — but deliberately NOT a plain name sweep
(gKorgSweepActive with gKorgSweepMode still eKorgSweepModeNameSweep)
or this sweep's own eBackupExpectKorgProgram: the picker below
opens even while a background NAME sweep is mid-flight, so seeing
either of those here is completely normal, not a conflict.
gBackupBatchActive (Moog) can't actually be true on a Korg-style
device at all — every Moog entry point already refuses to start
on one — kept here anyway as defence in depth.

## 71. in `synth_backup_start_name_sweep()`

Nothing running yet (e.g. clicked in the first moment after
connecting, before synth_backup_flush_background_prefetch()'s
own settle delay elapsed) — start it now so labels begin
filling in, but don't wait: the picker opens right below
regardless.

## 72. in `synth_backup_start_name_sweep()`

true only while OUR OWN name-sweep-mode batch has the floor —
gBackupBatchActive/gBackupExpect==eBackupExpectPreset are also both
used by bank-to-folder EXPORT mode and by a lone "Backup > Patch by
Number" fetch, neither of which this click should be allowed to
barge in on (synth_backup_capture_dump()'s own eBackupExpectPreset
branch disambiguates those from a sweep reply purely by
gBackupBatchActive, so letting a stray gBackupExpect==eBackupExpectPreset
through here when gBackupBatchActive is false could steal a lone
fetch's own reply).

## 73. in `synth_backup_start_name_sweep()`

Blocks a genuinely CONFLICTING operation (export mode, a lone
Patch-by-Number fetch, a Live/Bank backup) but deliberately NOT this
sweep's own in-progress request — the picker below opens even while
a background sweep is mid-flight, mirroring the Korg branch above.
2026-07-14 (owner: "these should be common mechanisms with Voyager
and any other device... should be common and generic").

## 74. in `synth_backup_start_name_sweep()`

Nothing running yet (e.g. clicked in the first moment after
connecting, before synth_backup_flush_background_prefetch()'s
own settle delay elapsed) — start it now so labels begin
filling in, but don't wait: the picker opens right below
regardless.

## 75. in `synth_backup_start_name_sweep()`

Already have every name from a previous sweep this session (kept
current by name_cache_update_from_preset_dump() at every write
this app makes since — see gNameCacheValid's own comment for the
accepted staleness gap), OR one is already running in the
background — either way, skip straight to the picker below
instead of starting a redundant sweep.

## 76. `on_name_sweep_picked()`

Confirmed-callback for name_sweep_show_picker()'s bank browser below —
bank1Indexed is always 1 (a single implicit bank, Moog-style has no bank
concept — see items[i].bank1Indexed's own comment below), location1Indexed
is the 1-based preset number. No synth_request_state_dump() here (removed
2026-07-14) — the sweep no longer leaves gDevice.progName showing the
last-swept preset's name at all (handle_moog_single_preset_dump() now
skips that write during name-sweep mode specifically, see its own comment
in synthComms.c), so there's nothing to restore on cancel; on Load,
synth_change_program() (the tail of synth_load_patch_from_bank()'s Moog
branch) already arms its own debounced state-dump refresh, and Store
already fetches its own fresh dump as part of capturing what to write —
so an immediate extra request here was pure duplicate traffic, and,
worse, one that could land right as a background sweep's own
request/reply was in flight (owner report: one observed "patch select
failing" while a sweep was running).

## 77. `name_sweep_show_picker()`

Shows the picker and acts on whatever the user chose. Opens IMMEDIATELY
regardless of sweep progress — 2026-07-14 (owner: "we should allow the
picker, with '---' unpopulated names before the full set is gleaned") —
showing real names for whatever
name_cache_update_from_preset_dump()/name_sweep_capture_name() has filled
in so far and "---" (from synth_backup_start_name_sweep()'s own
initialisation) for anything not reached yet. Called directly from
synth_backup_start_name_sweep() now (never automatically on sweep
completion — see backup_batch_advance()'s own comment for why). Ported
from a synchronous NSAlert+dropdown (show_device_choice_dialogue()) to
SynthLib's open_bank_browser(), same isolation from
korg_sweep_show_picker() above; no more default-selected row for Store
(open_bank_browser() has no such parameter — dropped the
gDevice.currentProgram-based defaultIndex calculation this used to have),
and the background sweep (synth_backup_flush_bank_to_folder(), still
driven every frame while this panel is up) now keeps filling in labels
live instead of pausing behind a blocking modal.

Real categoryNames/per-item category wired in 2026-07-24 (owner report:
"can't select Category on Load Patch from Bank for voyager") — the
original port passed category 0/categoryNameCount 0 unconditionally
(baking "Name — Category" into the label text instead), which
bankBrowser.cpp's own handle_bank_browser_click() explicitly disables the
Category sort BUTTON for (categoryNames.empty()), not just leaves it
inert — the click was silently swallowed, reading as "can't select" at
all. find_panel_dial_by_label() with no match (a device with no Category
dial at all) falls back to the same categoryNames=NULL/categoryNameCount=0
this always passed, so Category sort mode simply isn't offered there,
same as before.

## 78. in `synth_backup_flush_bank_to_folder()`

Paced gap between requests (name-sweep mode only — see
NAME_SWEEP_PACING_MS) — nothing is in flight yet, so skip the reply/
timeout checks below until it elapses. Never set for export mode
(backup_batch_advance()'s own comment), so this is a no-op there.

## 79. in `synth_backup_flush_bank_to_folder()`

Retry the SAME slot a couple of times before giving up — see
NAME_SWEEP_MAX_RETRIES's own comment. gBackupBatchCurrentPreset
deliberately doesn't advance here; backup_batch_request_current()
just re-sends for the current one with a fresh timestamp.

## 80. in `synth_backup_flush_bank_to_folder()`

This preset location didn't answer in time (unresponsive, or an
unpopulated location — Voyager's own behaviour for one of those
is unconfirmed, see synth_backup_bank_to_folder()'s own comment,
synthBackup.h). Log it as missing and move on rather than hanging
the whole export on one slot. Clearing gBackupExpect here (rather
than leaving it armed for THIS preset) means a very-late reply
arriving after this point gets attributed to whichever preset
backup_batch_advance() re-arms next instead — a narrow, accepted
edge case given the Voyager only ever has one request outstanding
at a time in practice (see the batch state block's own comment).

## 81. in `backup_save_callback()`

Remember the containing folder so the NEXT Backup save (of
any kind — this one, Bank, or the Bank-to-Folder picker)
defaults here too, instead of each one starting from an
unrelated system default — see get_last_backup_folder()'s
own comment (misc.h).

## 82. in `synth_backup_capture_dump()`

A "Store Patch to Bank…" fetch, not a "Save Patch to File…" one —
see gStoreArmedPresetNumber's own comment above for why this check
comes before anything file-related. Same CoreMIDI-thread-copies/
main-thread-processes handoff as the bank-to-folder batch export
just below, for the same reason (this runs on the CoreMIDI thread,
and the eventual convert+send+result-dialog work needs the main
thread — see synth_backup_flush_store()).

## 83. in `synth_backup_capture_dump()`

Discard a corrupt reply outright rather than handing it off — see
synth_moog_single_preset_dump_intact()'s own comment (synthComms.h)
for what this catches and why. Deliberately just drops it and
returns rather than doing anything else here: gBackupBatchRequestSinceMs
is untouched, so synth_backup_flush_bank_to_folder()'s own timeout
check (purely elapsed-time-based, doesn't care whether gBackupExpect
— already cleared above — is still armed) fires exactly as if
nothing had arrived at all, reusing the EXISTING retry-then-give-up
logic (NAME_SWEEP_MAX_RETRIES) rather than needing separate
corruption-specific retry handling. Added 2026-07-14.

## 84. in `synth_backup_capture_dump()`

Bank-to-folder sweep in progress — hand the bytes off to the
main/render thread rather than doing any file I/O or sequencing
here (this runs on the CoreMIDI thread — see the batch state
block's own comment for why). gBackupBatchReplyReady published
LAST, after the plain writes above it, is what makes this a safe
handoff without a lock.

## 85. in `synth_backup_capture_dump()`

Discard a corrupt reply outright — same reasoning as the Moog
branch just above (synth_moog_single_preset_dump_intact()'s own
comment), reusing gKorgSweepRequestSinceMs's own timeout+retry
instead of separate corruption-specific handling. Added 2026-07-14.

## 86. in `synth_backup_capture_dump()`

Korg name sweep in progress — same CoreMIDI-thread-copies/main-
thread-decodes handoff as the Moog bank-to-folder sweep just
above, for the same reason (this runs on the CoreMIDI thread —
see the Korg sweep block's own header comment). If gKorgSweepActive
is false, control falls through past this block to the generic
capture-and-save path below — same as eBackupExpectPreset above —
which is exactly how the standalone "Save Patch by Number to
File…" feature (synth_backup_patch_by_number_korg(), added
2026-07-14) gets its reply handled, no sweep involved.

## 87. in `synth_backup_capture_dump()`

Bank has no single current-patch name to reflect (it's 128 of them at
once) — handled on its own, before touching gDevice.progName at all.
Preset and Live ("Save Patch to File…", added 2026-07-11 at the
owner's request — "should default to a name reflecting the patch
name") both just want gDevice.progName if extract_moog_name()
(synthComms.c) managed to decode one, falling back to a kind-specific
default name when it didn't. Live needed handle_moog_panel_dump()
(synthComms.c) reordered to decode the name BEFORE calling this
function — it used to call this first, so gDevice.progName was still
whatever the PREVIOUS dump had left it as, not this one's.

## 88. in `synth_backup_capture_dump()`

nameForFile above is empty for this kind — handle_prog_dump()
(synthComms.c) deliberately never touches gDevice.progName
(would corrupt the on-screen edit buffer with some OTHER
program's name). Decode straight from the just-captured bytes
instead, same as korg_name_cache_update_from_dump() does.

## 89. in `synth_backup_capture_dump()`

Hands off to the main thread rather than opening the panel directly
from here (the CoreMIDI thread) — see gPendingBackupSaveReady's own
comment above for why. Plain writes first, atomic bool published
LAST, same discipline as every other CoreMIDI-thread handoff in this
file.

## 90. `synth_backup_flush_pending_save()`

Per-frame poll for synth_backup_capture_dump()'s pending "open the save
dialog" request — see gPendingBackupSaveReady's own comment above for why
this hop through the main/render thread is needed now (SynthLib's
open_file_browser_write(), unlike the old NSSavePanel-based dialog, has no
internal dispatch_async of its own). Call once per frame from the render
loop, alongside the other synth_backup_flush_*() functions.

## 91. `restore_validate_moog_dump()`

Validates data is a raw F0...F7 SysEx matching the connected device's own
mfrId/productId (same check is_moog_sysex() does in synthComms.c, re-done
here independently rather than exposing that static function — Restore
is the only place outside synthComms.c that needs to validate a dump's
header before trusting it) and carries the given mode byte. Returns
false (and logs why) otherwise, so callers can bail out before sending
anything to the device.
reason/reasonSize receive a user-facing explanation on failure — shown
via show_alert() by each caller below, since LOG_ERROR alone
(stderr) is invisible to anyone not watching a console, which made an
earlier version of this validation fail completely silently from the
user's point of view (a wrong file picked just did nothing, with no way
to tell why — reported 2026-07-11, fixed by adding this).

## 92. `restore_validate_korg_dump()`

Korg-style counterpart to restore_validate_moog_dump() above — validates
a raw F0...F7 SysEx capture as a genuine Program Data Dump (func 0x4C) for
the connected device (mfrId/familyId/channel-nibble all checked, same
header shape korg_decode_prog_dump() in synthComms.c already reads for
the exact same message type). Re-implemented independently here rather
than exposing that static function, same reasoning
restore_validate_moog_dump() already gives for its own Moog equivalent.
outBank/outProg (if non-NULL) receive the dump's own embedded destination
address on success — a Program Data Dump carries its own bank/program in
the header (unlike a Moog Panel Dump), so there's nothing to separately
track the way gBackupPresetNum is for the Moog "Backup > Patch by
Number" flow.

## 93. `convert_preset_dump_to_panel_dump()`

Converts a captured Single Preset Dump (mode 0x03 — "Patch by Number" or
a Bank (Individual Files) export) into an equivalent Panel Dump (mode
0x02), so any backed-up patch can be loaded into the live edit buffer
via "Open File…" too, not just restored-by-overwrite via
"Restore > Patch by Number…" — added 2026-07-11, owner's own request
("we should be able to use backup patches to load to panel").

The two formats are otherwise byte-for-byte identical: voyager.txt's own
presetNameOffset (101) vs. panelNameOffset (100) — derived independently
on real hardware, see each field's own comment there — differ by exactly
one byte, and the header comment on presetNameOffset already documents
why: "Moog's own doc lists an extra byte in the 0x03 reply's header that
0x02's doesn't have" — the preset number, at index 5 (F0/mfrId/
productId/deviceId/mode/THEN this), shifting everything after it by one.
Stripping that byte and changing the mode byte back to 0x02 reconstructs
a valid Panel Dump. Structurally sound from that derivation; NOT yet
independently round-tripped against real hardware the way the Restore
mechanism itself was (see [[project_voyager_restore_mechanism]] in the
assistant's own memory notes) — worth a real test before fully trusting
it. Returns a newly malloc'd buffer (caller frees) and writes its length
to *outLen, or NULL if srcLength is too short to contain the byte being
removed.

## 94. `convert_panel_dump_to_preset_dump()`

Inverse of convert_preset_dump_to_panel_dump() above — inserts a
preset-number byte and flips the mode byte the other way, turning a Panel
Dump into a Single Preset Dump addressed to a chosen destination. Used by
synth_store_patch_to_bank() below ("Store Patch to Bank…", G2-Edit
naming): the ONLY way to write the current live edit buffer to a specific
stored location is the same "SEND PRESET(S)" mechanism Restore > Patch by
Number already proved works (see [[project_voyager_restore_mechanism]] in
the assistant's own memory notes) — there's no separate "commit edit
buffer to slot N" SysEx command in the manual, just "send a Single Preset
Dump addressed to N and the device stores it there". presetNumber0based
is 0-127 (caller subtracts 1 from the 1-based UI number, same convention
synth_request_single_preset_dump() uses on the request side). Returns a
newly malloc'd buffer (caller frees) and writes its length to *outLen, or
NULL if srcLength is too short to be a genuine Panel Dump.

## 95. `synth_backup_flush_store()`

Per-frame poll for synth_store_patch_to_bank()'s own pending fetch — see
gStoreReplyReady's own comment above for why this work (convert+send+
result dialog) happens here on the main/render thread rather than inside
synth_backup_capture_dump() (CoreMIDI thread) where the reply itself
lands.

## 96. `restore_edit_buffer_korg_file()`

Korg-style counterpart to the Moog branch below — Z1 (and any other
Korg-style device) has no single "load this whole dump" SysEx the way a
Moog-style device's own Panel Dump does; the closest equivalent is
replaying every dial's own value as an individual live Parameter Change
(group=/param=), which only ever touches the live edit buffer, never
flash — this deliberately never sends a PROGRAM WRITE REQUEST, matching
the owner's own "wouldn't store to Z1 flash yet" caution (2026-07-14).
Paced with CFRunLoopRunInMode (not usleep — restore_edit_buffer_file_chosen()
below runs on the main thread's own CFRunLoop, dispatched via open_file_
read_dialogue_async()'s NSOpenPanel completion handler.

REWRITTEN 2026-07-14 (same day as the first version): a real-hardware
test of the original approach — replaying every dial as its own live
Parameter Change message — sounded "like an init patch, every time".
Owner asked the right question: "We're sending 119 parameter change
messages, rather than a holistic patch sysex... should we not be sending
the sysex?" Checked the Z1 MIDI Implementation doc's own func 0x40/0x4C
descriptions: func 0x40 (CURRENT PROGRAM DATA DUMP) carries NO bank/
program address at all — sending it TO the device loads the payload
straight into the LIVE EDIT BUFFER, the Z1's own direct equivalent of a
Moog-style Panel Dump. Func 0x4C (PROGRAM DATA DUMP, what a backup file
actually contains) DOES carry a bank/program address, so sending THAT
verbatim targets a specific STORED slot instead — not what "load to edit
buffer" wants at all, and not what the original 119-message approach did
either, which is why it needed reverse-engineering every dial's own
address in the first place. Sending 0x40 needs none of that: no per-dial
knowledge, no pacing, no sweep-collision risk, and critically, it never
goes through the individual Parameter Change write path at all — which
sidesteps an ALREADY-KNOWN real Z1 hardware bug (see feedback memory
project_z1_int_positive_write_clamp) where a live Parameter Change write
of a positive value in the "-99..+99 Int" family silently clamps to 0.
Most of z1.txt's dials are exactly that family, which is almost
certainly why the old approach reliably produced a near-init-sounding
result: every positive envelope level/mod amount/etc. got clamped away.

## 97. in `restore_edit_buffer_korg_file()`

Re-slice the file's own RAW (still 7-bit-packed, NOT the decode_7to8()
result above — that's only needed for the local GUI update below)
payload bytes straight out of the func 0x4C message — same header
arithmetic restore_validate_korg_dump() above and korg_decode_prog_
dump() (synthComms.c) both already use: F0 + mfrId(n) + channel +
familyId + func (funcPos+1 bytes) + Unit/Bank + ProgramNo + a fixed
00 byte (3 more) is where func 0x4C's own data starts. No decode/
re-encode needed at all — func 0x40's payload is bit-for-bit
identical to func 0x4C's, only the header differs.

## 98. in `restore_edit_buffer_korg_file()`

Pause the background Korg name-sweep around the send — cheap
insurance against the exact collision class synth_set_panel_dial_
value()'s own synth_backup_sweep_request_in_flight() check already
guards a single dial drag against, even though this is now one
message instead of a 119-message burst so the risk window is far
smaller than it was. Deliberately not resumed — the sweep's own
existing auto-start logic picks it back up on its own next idle
trigger.

## 99. in `restore_edit_buffer_korg_file()`

Update the LOCAL GUI/dial state from the exact same decoded buffer
just sent to the device, instead of leaving it stale until (or
unless) a later Sync from synth happens to refresh it — a real
hardware test of the original approach found the GUI simply didn't
change at all after a restore.

## 100. in `restore_edit_buffer_korg_file()`

Belt-and-suspenders: also request a fresh dump from the device itself
once sent, so if anything genuinely didn't land (dropped, real
hardware rejected it — func 0x40 replies with a DATA LOAD ERROR in
that case, not currently surfaced to the user) it gets caught by the
SAME mechanism "Sync from synth" already relies on, rather than
trusting the local apply above alone to be the last word on what the
device actually ended up with.

## 101. `restore_edit_buffer_file_chosen()`

Runs on the main thread once the user has chosen (or cancelled) a file to
load into the live edit buffer — see synth_backup_restore_edit_buffer() below.
Accepts EITHER a genuine Panel Dump (mode 0x02, "Backup > Current Panel"
/ "Save Patch to File…") or a Single Preset Dump (mode 0x03, "Backup >
Patch by Number" or a Bank (Individual Files) export) — the latter is
converted via convert_preset_dump_to_panel_dump() above before sending.
Korg-style devices (Z1) branch off to restore_edit_buffer_korg_file() above
instead — an entirely different mechanism (see its own comment) since
there's no Korg equivalent of a Moog Panel Dump to just forward as-is.

## 102. in `restore_edit_buffer_file_chosen()`

A raw Single Preset Dump (mode 0x03) is converted to a Panel Dump
(mode 0x02) shape BEFORE the mode-0x02 validation below, so both file
kinds end up validated (and sent) the exact same way — the mfrId/
productId check still guards against a file from the wrong device
either way.

## 103. in `restore_edit_buffer_file_chosen()`

SHOW WHAT WAS JUST LOADED. Sending a Panel Dump changes the DEVICE; it does not change a
single dial on screen, so without this the panel went on showing whatever it showed
before and the restore looked like it had done nothing — owner-reported against A27
Subaquaeous, whose filter cutoff was audibly correct on the synth while the GUI never
moved.

FROM THE FILE, NOT FROM A RE-READ, and that is the opposite of what it looks like it
should be. Re-reading sounds safer — let the device be the authority on what it actually
ended up with — and for the Korg path above it is. A Voyager will not answer that way: a
Panel Dump is the state of the PANEL, and for a parameter backed by a physical pot the
synth goes on reporting the pot rather than the value the patch just put there. Measured
2026-09-02, both halves in the same session: a dump differing only in Filter Cutoff
changed the sound while every later read still returned the old value, and a dump-only
field in the same message (Clock Div, 24 -> 30 -> 24) round-tripped exactly. So a re-read
would quietly replace the value the user just loaded with the position of a knob they
have not touched.

Nothing is lost by not re-reading: for every field the synth does report faithfully, the
file and the device agree, because the file is what we just sent it. "Sync from synth" is
still there, unchanged, for anyone who wants the panel's own account of itself.

## 104. `gPendingRestorePatchData`

Stashed by restore_patch_file_chosen() immediately before opening the now-
asynchronous confirmation dialog, for on_restore_patch_confirmed() below
to pick back up — path is copied into a fixed buffer rather than keeping
the pointer the file browser handed in, since it's only guaranteed valid
for the duration of that original callback, not until this LATER
confirm-callback fires.

## 105. in `restore_patch_file_chosen()`

The preset number is the one extra byte a Single Preset Dump's own
header has that a Panel Dump's doesn't (see presetNameOffset's own
comment in voyager.txt for that byte-count difference) — byte index
5 (F0, mfrId, productId, deviceId, mode, THEN this), 0-based on the
wire same as the request side (synth_request_single_preset_dump(),
synthComms.c). CONFIRMED against real hardware 2026-07-11 (see
[[project_voyager_restore_mechanism]] in the assistant's own memory
notes): captured preset 128, decoded this byte as 127, matched
exactly.

## 106. `gPendingKorgRestorePatchData`

Korg-style counterpart to restore_patch_file_chosen() above (Moog). See
restore_validate_korg_dump()'s own comment for the header/address this
reads — sending an addressed Program Data Dump straight to the device is
UNCONFIRMED against real Z1 hardware as a way to write a specific bank/
program (contrast synth_send_korg_program_write_request(), synthComms.h,
the one Korg write mechanism that IS confirmed — committing the live
edit buffer, not arbitrary addressed data). Standard convention across
Korg's own product line for a message carrying its own destination
address, and structurally consistent with how this app already reads the
SAME message shape back (korg_decode_prog_dump(), synthComms.c) — but
worth a real test with a low-stakes preset before trusting it the way
the Moog mechanism (independently hardware-confirmed 2026-07-11) is
trusted.
Stashed by korg_restore_patch_file_chosen() immediately before opening the
now-asynchronous confirmation dialog, for on_korg_restore_patch_confirmed()
below to pick back up — same "copy path into a fixed buffer" reasoning as
gPendingRestorePatchPath above (the Moog counterpart).

## 107. `korg_restore_patch_to_bank_send()`

Runs on the main thread once the user has chosen (or cancelled) a Program
Data Dump file to load into a SLOT THEY CHOOSE — see synth_backup_
restore_patch_to_bank() below. Unlike korg_restore_patch_file_chosen()
above (restores to the file's OWN embedded bank/program), this shows the
same named-slot picker Store Patch to Bank uses and re-addresses the
message to whatever the owner picks. Owner's own framing (2026-07-14):
"We can already save from a flash slot to a file... this would be the
reverse" — and correctly pushed back on an earlier, more roundabout
edit-buffer-then-Store-Patch two-step plan in favour of this direct
file-to-slot send, which is exactly what korg_restore_patch_file_chosen()
already does, just always to the file's own address instead of a chosen
one. Writes DIRECTLY to the chosen stored slot, bypassing the live edit
buffer entirely (synth_send_korg_program_data_dump(), synthComms.c).
UNCONFIRMED against real Z1 hardware — same caveat korg_restore_patch_
file_chosen() above already carries (never independently tested, unlike
the Moog restore-to-slot mechanism).
Shared by korg_restore_patch_to_bank_file_chosen() (after the picker+
confirm below) and synth_backup_restore_patch_to_bank_from_path() (the
backdoor test entry point, synthBackup.h) — re-slices the file's own RAW
(still 7-bit-packed) payload bytes and re-addresses them to bank/prog.

## 108. in `korg_restore_patch_to_bank_send()`

Name/category decode reads the PAYLOAD only, unaffected by which
bank/program was in the header — passing the file's own original
data here (not a reconstructed message) is correct, since bank/prog
are supplied explicitly as the CACHE slot to update.

## 109. `gPendingRestoreToBankData`

Stashed by korg_restore_patch_to_bank_file_chosen() across its now-two
chained async steps (open_bank_browser() then show_confirm()) — same
CoreMIDI-thread/main-thread-style handoff convention this file already
uses for confirm/callback chains elsewhere, just chained twice here since
this flow has two dialogs in sequence. Both callbacks below only ever run
on the main thread (the browser/dialog completion itself), so these don't
need to be atomic.

## 110. in `korg_restore_patch_to_bank_file_chosen()`

Same named-slot picker Store Patch to Bank uses (korg_sweep_show_
picker() above) — opens immediately with whatever names are already
known ("---" for the rest), same "don't block on a full sweep" UX.
Ported from show_device_choice_dialogue() to open_bank_browser(), same
reasoning/trade-offs as korg_sweep_show_picker()'s own conversion
above (this is, in fact, the exact same "pick a named preset from a
256-entry list" shape that call already uses) — including the real
categoryNames/per-item category wiring added there 2026-07-24 (see
name_sweep_show_picker()'s own comment for the bugs that fixed).

## 111. `gPendingRestoreBankData`

Stashed by restore_bank_file_chosen() immediately before opening the now-
asynchronous confirmation dialog, for on_restore_bank_confirmed() below to
pick back up — same "copy path into a fixed buffer" reasoning as
gPendingRestorePatchPath above.

## 112. in `on_restore_bank_confirmed()`

Invalidates the WHOLE name cache rather than trying to update
it — a whole-bank dump is one opaque ~18KB blob with no
confirmed per-preset name offset to extract 128 individual
names from safely (see gNameCacheValid's own comment). The
next Load/Store Patch to Bank will just re-sweep.

## 113. `gRestoreFolderActive`

── Restore from folder (Bank Individual Files, in reverse) ─────────────────
Sequentially reads back a Backup > Bank (Individual Files) export and
sends each file it finds, restoring every matching stored slot on the
connected device — added 2026-07-11, owner's own request ("we should be
able to restore... the individual files based on the .txt file list").

Threading: unlike gBackupBatch* above, this never touches the CoreMIDI
thread at all — a restore SEND has no reply to wait for (see
synth_backup_restore_patch()'s own comment on the Single Preset Dump
mechanism), so the whole sweep lives entirely on the main/render thread:
synth_backup_flush_restore_folder(), called once per frame from
do_graphics_loop() same as synth_backup_flush_bank_to_folder(), reads the
next file off disk and sends it directly, no gBackupBatchReplyReady-style
cross-thread handoff needed.

## 114. `RESTORE_FOLDER_SEND_PACING_MS`

Paced, not fired all at once — the device needs real time to actually
write each preset to its own storage before the next one arrives (same
"won't queue a second reply/request while still busy" real-hardware
finding that drives BACKUP_BATCH_TIMEOUT_MS above, applied here to the
SEND side instead of the request side). CONFIRMED on real Voyager
hardware at this value 2026-07-12 (both export and restore directions,
see [[project_voyager_bank_to_folder_export]]) — don't raise this one
without a real reason, it isn't the value that needed fixing.

## 115. `KORG_RESTORE_FOLDER_SEND_PACING_MS`

Korg-style (Z1) counterpart to RESTORE_FOLDER_SEND_PACING_MS above — was
the SAME shared 150ms constant until 2026-07-14, when a real Korg folder
restore at that value left some slots blank (owner: "Looks like the bank
restore is too fast. Some slots are blank."). Split into its own constant
rather than raising the shared one, since Voyager's restore was already
independently confirmed working at 150ms and there's no reason to slow
that down just because Z1 needs more room. 500ms is a bigger, still-a-
guess step in the safe direction, not a verified minimum either: each
Korg-style Program Data Dump send here is a genuinely larger message
(~650+ bytes, most of a full patch) than the small requests BACKUP_
BATCH_TIMEOUT_MS's own 600ms already treats as needing that much margin,
and it also has to be physically committed to flash on the device side
before the next one arrives — plausibly needs AT LEAST that much room,
maybe more. A full 256-slot Z1 restore now takes ~128s instead of ~38s —
a real cost, but a blank slot from a too-fast restore is worse than a
slower one that actually lands. Retest and tighten (if fully reliable at
this value) or lengthen further (if still blank) from here, rather than
guessing again from scratch.

## 116. `restore_folder_parse_index()`

Parses <folder>/Patches.txt for the ordered list of preset numbers it
records (backup_batch_append_index_line() above writes each data line as
"%03u  %s\n" — exactly 3 digits then TWO spaces) — ignores the recorded
name text, just the leading number. The two-space check is what
distinguishes a real entry from the header's own "128 presets requested
from device" line, which also starts with digits but has only one space
after them. Returns how many were found (capped at maxCount).

## 117. `restore_folder_find_file()`

Finds the file in `folder` whose name starts with the exact zero-padded
3-digit preset number (matching backup_batch_write_capture()'s own
"%03u %s.syx"/"%03u.syx" naming) — deliberately NOT reconstructed from
Patches.txt's own recorded name text, so a file renamed (or one whose
name has drifted from what the index remembers) since the export still
gets found correctly; only the leading number has to match. Writes the
full path into outPath (sized to match this file's other path buffers).
Returns false if no matching file was found.

## 118. `gPendingRestoreFolderMissingCount`

Stashed by restore_folder_chosen() immediately before opening the now-
asynchronous confirmation dialog, for on_restore_folder_confirmed() below
to pick back up — gRestoreFolderMissingCount is computed here (rather than
stashing indexCount itself) since that's the only piece the old inline
continuation actually derived from it.

## 119. in `restore_folder_chosen()`

Resolve each listed preset number to an actual file in the folder —
one whose file has since been deleted/moved/renamed-past-recognition
is just skipped (counted as missing below), not treated as a hard
failure for the whole operation.

## 120. `synth_backup_flush_restore_folder()`

synth_backup_restore_folder() itself lives further down, after the Korg
restore-folder block (needs gKorgRestoreFolderActive/korg_restore_folder_chosen(),
both declared there) — same forward-reference reasoning as
synth_backup_bank_to_folder()'s own comment above.

## 121. in `synth_backup_flush_restore_folder()`

Without this, the progress overlay (synth_render_backup_progress(),
synthGraphics.cpp) would only repaint whenever something UNRELATED
happened to set gReDraw — see backup_batch_advance()'s own identical
comment above for the full reasoning; same fix, same day, same cause.

## 122. in `synth_backup_flush_restore_folder()`

Keeps the name cache (gNameCacheValid) accurate for this
slot without needing a full re-sweep — see that flag's
own comment. Only meaningful if a sweep has already run
this session (gNameCacheValid true) — name_cache_set_label()
is a no-op-safe write either way, so no extra guard needed
here; the NEXT Load/Store just won't see a cache at all
yet if this is the first bank-scale operation this
session, same as before this existed.

## 123. in `synth_backup_flush_restore_folder()`

Refresh gDevice.progName/every dial from the live edit buffer
once the sweep's done — same reasoning as
backup_batch_advance()'s own end-of-sweep synth_request_state_dump()
call, applied here since sending 100+ presets doesn't itself
change what's showing, but it's easy to forget the display is
now stale after a sweep this size.

## 124. `korg_restore_folder_parse_index()`

── Korg restore-folder (Restore > Bank (Individual Files), in reverse) ────
The Korg counterpart to the Moog restore-folder mechanism above — fully
separate state (declared up near the rest of the Korg sweep's own state,
not here, so synth_backup_start_name_sweep()'s Korg guard further up can
reference it too — see gKorgRestoreFolderActive's own comment there),
same reasoning as the rest of this file's Korg/Moog split (a fixed
128-slot single bank vs 256 across 2 banks, a different filename/index
format). Same threading model too: a restore SEND has no reply to wait
for, so this lives entirely on the main/render thread, driven once per
frame by synth_backup_flush_korg_restore_folder() below.

## 125. `korg_restore_folder_parse_index()`

Parses <folder>/Patches.txt for the ordered list of (bank, program) slots
it records (korg_sweep_append_index_line()'s own "A001  Name\n"/"B045  Name\n"
format — bank letter, 3-digit zero-padded program, TWO spaces) — the Korg
counterpart to restore_folder_parse_index() above (Moog). Ignores the
recorded name text, just the leading address. Returns how many were
found (capped at maxCount), as sweep-style indices (0..255, matching
gKorgSweepLabels' own indexing).

## 126. `korg_restore_folder_find_file()`

Finds the file in `folder` whose name starts with the exact "A001"/"B045"
style prefix (matching korg_sweep_write_capture_file()'s own naming) —
the Korg counterpart to restore_folder_find_file() above (Moog).
Deliberately NOT reconstructed from Patches.txt's own recorded name text,
same "a renamed file still gets found" reasoning as that function's own
comment. Writes the full path into outPath. Returns false if no matching
file was found.

## 127. `synth_backup_flush_korg_restore_folder()`

Per-frame poll — the Korg counterpart to synth_backup_flush_restore_folder()
above (Moog). A no-op unless a Korg folder restore is actually active.
Call once per frame from the render loop. Unlike that function's own
end-of-sweep synth_request_state_dump() call, this doesn't need one —
handle_prog_dump() never touches gDevice.progName/the live dials in the
first place (see korg_sweep_show_picker()'s own comment above), so
there's nothing stale to refresh once the sweep's done.

## 128. in `synth_backup_get_export_progress()`

Korg name sweep checked first — a fully separate state machine (see
its own header comment above) that shares this same progress-overlay
reporting function purely for UI reuse, not because it shares any
state with gBackupBatchActive below. Added 2026-07-14.

## 129. in `synth_backup_export_progress_is_name_sweep()`

gKorgSweepActive alone isn't enough any more — that same flag also
covers a real Backup > Bank (Individual Files) export now (export
mode reuses this exact sweep, see its own header comment), which
deserves the blocking progress modal same as the Moog export path
below, not the quiet background-fill status row a plain name sweep
gets. Found while adding Korg export mode, 2026-07-14 — before this
fix, an in-progress Korg bank export would have been misreported as
a name sweep here.

## 130. in `synth_backup_get_restore_progress()`

Korg restore-folder checked first — a fully separate state machine
(see its own header comment above) that shares this same progress-
overlay reporting function purely for UI reuse, same pattern
synth_backup_get_export_progress() already uses for the Korg sweep.

## 131. `BACKGROUND_PREFETCH_SETTLE_MS`

How long a connection has to stay eligible (connected, no cache yet,
nothing else in flight) before the background name-prefetch sweep
silently starts. NOT an inactivity timer — an earlier version of this
gated on mouse/keyboard idle time (mouse_idle_ms(), mouseHandle.c), but
that meant a completely normal few-second pause between dial tweaks
triggered a sweep mid-session (2026-07-14 user report: "when I start
tweaking dials, names are being polled, so it's not really an
inactivity mechanism as-is"). Dropped that entirely: the sweep is now
always paced slowly enough (NAME_SWEEP_PACING_MS above) to stay out of
the way of normal interactive use regardless of when it runs, so there's
no need to wait for genuine inactivity at all — this settle delay just
lets connect-time traffic (the initial state dump, etc.) clear first.

## 132. `synth_backup_flush_background_prefetch()`

Silently starts a name sweep (Korg or Moog, whichever the connected
device uses) once it's stayed eligible for a moment (see
BACKGROUND_PREFETCH_SETTLE_MS above), so by the time the user actually
clicks "Load/Store Patch from Bank…" some (or all, if they took a while
to get there) of the picker is already populated instead of showing
"---" everywhere. Common to every device family as of 2026-07-14 (owner:
"these should be common mechanisms with Voyager and any other device...
should be common and generic") — Voyager included, DESPITE the following
known risk, which the owner explicitly accepted rather than have this
stay Korg-only: an earlier idle-poll mechanism for the Voyager was built
and then fully reverted (see project_voyager_restore_mechanism /
project_voyager_extlevel_and_precision memory notes) because ANY
unsolicited dump request kicks the Voyager's front panel out of whatever
menu it's showing, and a Single Preset Dump Request (what this sweep
uses on a Moog-style device) is exactly that kind of request — watch the
Voyager's own front-panel display the first few times this fires. One-
shot in practice: gNameCacheValid/gKorgNameCacheValid latches true on
completion and this codebase has no reconnect/disconnect hook that
clears it back to false (only Restore Bank explicitly invalidates the
Moog one, see synth_backup_restore_bank_chosen()'s own comment above) —
not something newly introduced here. Call once per frame from the render
loop, alongside the other flush functions.

## 133. in `synth_backup_flush_background_prefetch()`

A Korg-style device that doesn't speak Z1's specific Program Data
Dump Request protocol (supportsKorgProgramDump == false — Kronos,
whose own SysEx protocol is entirely different, see
KRONOS_MIDI_Implementation's own field comment in panelConfig.h) has
nothing this whole sweep mechanism could ever fetch — every request
it would send times out forever. Skip entirely rather than spin.

## 134. in `synth_backup_flush_background_prefetch()`

Mirrors synth_backup_start_name_sweep()'s own "nothing else in
flight" guard — a background prefetch must never contend with an
explicit user action already using the same reply-capture state.
Deliberately does NOT exclude eBackupExpectKorgProgram/
eBackupExpectPreset — those are this very sweep's OWN in-flight
request once it's running (already excluded above via
gBackupBatchActive/gKorgSweepActive being false at this point), not a
conflict from something else.

## 135. `gStoreVerifyCurrentSlot`

Set when the store in flight came from Store Patch to Current Slot rather than Store Patch to Bank,
so synth_backup_flush_store() checks the slot again before writing (§136). `gStoreExpectedName` is the
confirmed name at the moment the user asked.

## 136. `synth_store_patch_to_current_slot()`

Owner, 2026-09-12: "Don't allow save to current patch if we're not 100% sure we're on the right
current index." So it runs only when the current preset is Confirmed (types.h notes §5). Every other
state is refused with its reason, and Store Patch to Bank... is offered instead. A preset found by name
alone is not enough, and neither is a Program Change the name has not agreed with.

After the confirm dialog, the store asks for a fresh Panel Dump as Store Patch to Bank does. Before
that dump is written, store_target_still_current() checks it again: the preset must still be Confirmed
as the same number, and the dump's own name must still be the name the user saw. If the synth moved to
another preset in between without a Program Change, its name gives it away, and nothing is written.

The one case this cannot catch: the synth moves silently to a preset with the same name. The rule
that a Program Change this app sent must match a unique name (synthComms.c notes §88) makes that
unlikely.

Moog-style devices only. A Korg device's current bank is not tracked (todo.md).
