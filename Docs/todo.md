SynthEdit TODO

Things to do. ONE LINE PER ITEM - keep it that way.
Measurements, reasoning and completed-work narrative go in findings.md, NOT here.
Built-but-unchecked work goes in to-test.md.

CT

- Ultimately, a plugin similar to Minitaur, which could restore the full state of a synth from Ableton/Cubase project would be great. Long term plan...

Bugs

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
- Store Patch to Current Slot... for the Korg devices: needs the current bank tracked (Bank Select before the Program Change), which nothing records yet
