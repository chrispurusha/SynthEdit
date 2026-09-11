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
- No MIDI channel choice yet, deliberately deferred: the channel comes from the identity reply, else the layout's midiChannel, then is learned from incoming CCs - add Automatic/1-16 to the MIDI Ports dialogue if a no-identity device's first guess proves wrong in practice
- Finish the race review: synthBackup.c's 154 statics, the panel dial values and the name cache were not examined - see findings 2026-09-02
- SynthEdit's layouts default is "layouts" RELATIVE TO CWD, which is / for a Finder-launched app - do-release ships the folder in the .dmg as a workaround, but bundling it in Resources and defaulting there would be the real fix
