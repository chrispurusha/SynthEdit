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
- No-identity devices report 'Connected' with nothing verified: say 'Sending to X - not verified' until a CC or program change arrives from the device, then 'Heard from it on Y' - the parse loop already sees each message's source (owner likes it)
- Identity replies from a source other than the chosen input are dropped silently (process_identity_replies) - give it EmuUtility's fallback: take a matching device on another input when none answers on the chosen one, and say so in the status line (a Cirklon delivers a port's input under another name)
- Finish the race review: synthBackup.c's 154 statics, the panel dial values and the name cache were not examined - see findings 2026-09-02
- SynthEdit's layouts default is "layouts" RELATIVE TO CWD, which is / for a Finder-launched app - do-release ships the folder in the .dmg as a workaround, but bundling it in Resources and defaulting there would be the real fix
