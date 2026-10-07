SynthEdit TODO

Things to do. ONE LINE PER ITEM - keep it that way.
Measurements, reasoning and completed-work narrative go in findings.md, NOT here.
Built-but-unchecked work goes in to-test.md.

CT (priority order)

- The to test items below, need to go into a to-test.md, similar to G2-Edit.
- Corruption on Voyager panel - corrupted name string. Possible when merging MIDI note data from Ableton whilst changing parameters.

Bugs

- Ultimately, a plugin similar to Minitaur, which could restore the full state of a synth from Ableton/Cubase project would be great. Long term plan...
- For voyager, controls are being sent, but not sure we're sending patches from files correctly. ...or when we request a patch to save, it's not the edit buffer patch maybe. I pulled a patch (SubAqWithPascal.syx - currently on my desktop, but when I send back to Voyager, the filter Voyager shows is 
- Voyager APPLIES Filter Cutoff from an incoming Panel Dump (audible) but goes on REPORTING the old value - a Panel Dump reports the pot, not the loaded patch, so a restore must show the file rather than re-read
- Device > MIDI Ports... (SynthLib's midiPortDialog, 2026-09-11) is the device UI SynthEdit lacked: input and output chosen per device configuration, overriding the layout's midiPort - built, NOT yet opened on screen or tried against a synth
- MIDI channel is now chosen in the MIDI Ports dialogue (Auto/1-16, SynthLib 1a1b4fa, 2026-09-12): a fixed channel overrides the identity reply's and the layout's, and stops the CC auto-correct; Auto keeps both - check it with a no-identity device whose first guess is wrong
- Scan to FIND the synth, not just the loaded layout's (owner likes it, wants more thought): send the identity request, match the reply against EVERY identity-capable layout and switch to the one that answers. Open questions: several layouts answering at once; the port choice is saved per layout scope, so which scope a scan uses; switching layout under unsaved edits; the no-identity devices (Voyager, Minitaur) can only be found by listening for their own traffic
- CHECK ON HARDWARE (built 2026-09-12): a no-identity synth (Voyager) shows 'Sending to X - not verified' in MIDI Ports until a knob is turned on it, then 'Connected: heard from it on Y'
- CHECK ON HARDWARE (built 2026-09-12): an identity synth whose reply arrives on another input than the chosen one now connects, and MIDI Ports says 'heard on Y (not the chosen Z)'
- Finish the race review: synthBackup.c's 154 statics, the panel dial values and the name cache were not examined - see findings 2026-09-02
- SynthEdit's layouts default is "layouts" RELATIVE TO CWD, which is / for a Finder-launched app - do-release ships the folder in the .dmg as a workaround, but bundling it in Resources and defaulting there would be the real fix
- CHECK BY EYE (built 2026-09-12): with the Voyager loaded and no Program Change seen, Prev and Next show as dim labels with no button face; after a program change on the synth they become buttons
- CHECK ON HARDWARE (built 2026-09-12): Voyager with a complete name cache, connect without a Program Change - a uniquely named patch shows 'Preset N (by name)' in grey and Prev/Next work; a duplicated name (e.g. two INITs) stays unknown
- CHECK ON HARDWARE (built 2026-09-12): change preset on the Voyager, or with Prev/Next - the label turns white 'Preset N' once the dump's name agrees with the cache; Store Patch to Current Slot... is refused in every other state, with the reason
- CHECK ON HARDWARE (built 2026-09-12, ONE flash write): Store Patch to Current Slot... on a confirmed preset, after an edit and after a rename in SynthEdit - confirm dialog names the slot, the write lands, the cache takes the new name
- CHECK ON HARDWARE (Z1, built 2026-10-07): change program on the Z1's panel - the label shows e.g. "B042 (unconfirmed)", then "B042" once the dump's name agrees with the name cache; Prev/Next step across A127 -> B000
- CHECK ON HARDWARE (Z1, built 2026-10-07): connect to a Z1 just powered up (on A000) with no name cache - the label shows "A000 (by name)" after one Program Dump; with Program Change Transmit off it says so instead
- CHECK ON HARDWARE (Z1, built 2026-10-07): set a non-default Program Bank Select Map on the Z1 (e.g. B = 0/5), reconnect - Prev/Next and Load from Bank still land on the right bank
- CHECK ON HARDWARE (Z1, built 2026-10-07, ONE flash write): Store Patch to Current Slot... on a confirmed program (e.g. after Prev/Next) - the dialog names the slot and its cached name, the write lands there
- CHECK ON HARDWARE (Z1, built 2026-10-07): the Model and Insert pages follow the type selector (showIf) after a Sync, and offline from the selector's own value
- Z1: Multi Set and Arpeggio pattern pages - the parameter tables exist; they need their own edit-buffer dumps as dumpBlocks (19 00 -> 49 00, 36 00 -> 6B 00) and the Parameter Change group confirmed (Multi 2 or 3?)
- Z1: Global's User Scale 2 (128 notes), the 16 user group names and the MIDI program-select maps - tables too big for dials, want a grid/table control
- CHECK ON HARDWARE (Z1, built 2026-10-07, writes Global): edit a Global or MIDI setting from SynthEdit (group 0) and read it back - reading is confirmed, writing not tried
- CHECK BY HAND (Z1, built 2026-10-07): drag the envelope points on EG 1-4 and Amp > EG - times move sideways, levels up and down, Shift locks to one direction; the dials follow
- Z1: the PE knobs' Parameter list beyond entry 158 names the current oscillator model's own parameters; it shows "OSC Model n" for now

Kronos

- CHECK ON HARDWARE: press COMBI / PROG on the Kronos - the top tab row should follow (Mode Change, func 4E); clicking a top tab should switch the Kronos's mode
- CHECK ON HARDWARE: AL-1 dials beyond Filter A/B Cutoff (only those two are hardware-confirmed); try a few per page, both EXi slots
- CHECK ON HARDWARE: turn a slot-1 knob on the Kronos while the EXi 2 tab is open - slot 2's display must not move; switching back shows the new slot-1 value
- Graphical envelopes: a generic `graph type=envelope` layout element built from existing dial ids, drag handles editing those dials (model: G2-Edit render_envelope_graph); AL-1 EG pages (one row tall, across the top, dials down a row), AMP, and the Z1 EGs
- Step Seq: a draggable 32-bar graph for the step values (room on the Step Seq 2 page)
- LFO waveform, keyboard-track curve and filter-routing pictures as simple line drawings
- Note-length values (LFO MIDI Sync Base Note, Step Duration: 0..9 = 1/32..1/1) show as numbers - find the names
- Pitch intensities show the raw -151..151 sent; the Kronos displays -48.00..+48.00 - scale the display
- Global pages: request + decode the Global dump (Current Object Dump obj 3, 24620 bytes) into its own cache, dials tied to it; generate ~220 params (TYP 15) from Korg's Global table, laid out on the Korg editor's own 13 Global pages
- Remaining EXi engines' pages (MOD-7, CX-3, STR-1, MS-20EX, PolysixEX, SGX-1, EP-1, HD-1), then Combi and Global
- Raise PANEL_MAX_SECTIONS (64; 45 used) before more engines - first move each dial's own copy of its value names to shared names=@list references, as the memory cost scales with it
- Store Patch / bank operations for the Kronos (Object Dump protocol), not just the edit buffer
