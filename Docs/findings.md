SynthEdit FINDINGS

Completed work, measurements, and the traps that cost real time.
One entry per finding, newest first.


2026-09-12  CURRENT PRESET FROM THE PATCH NAME, AND STORE TO CURRENT SLOT
------------------------------------------------------------------------------------------------------
The Voyager never says which preset it is on. SynthEdit only knew when a Program Change went past, so
Prev/Next were often dead. Now each Panel Dump's name is also matched against the name cache. A name
that belongs to exactly one preset makes that preset current; two or more leave it unknown. The panel
shows "Preset N (by name)" in grey beside Sync, and Prev/Next work from it.

What that match is worth is tracked as a certainty level (types.h notes §5). A match by name alone is
a guess: a patch loaded from a file, or a stale cache, can carry the same name. A preset counts as
Confirmed only when a Program Change is seen AND the next dump's name agrees with that preset's cached
name. The label is then white "Preset N".

Store Patch to Current Slot... (File menu, beside Store Patch to Bank...) writes the edit buffer back
to that preset. Following the owner's "not 100% sure → don't", it refuses anything short of Confirmed,
and says why. After the confirm dialog it checks again on the dump it is about to write: the same
preset must still be Confirmed, and the dump must carry the same name. The rules are in synthComms.c
notes §87-§88 and synthBackup.c notes §135-§136. Built, not yet checked on the hardware (todo.md).


2026-09-02  RACE CONDITION REVIEW - PARTIAL, AND NOTHING BROKEN FOUND YET
------------------------------------------------------------
Same method as the GenBridge and G2-Edit reviews: establish the threads, then map shared state onto
them. This one is INCOMPLETE and the coverage is stated at the end - it found no defect, which is a
weaker claim than "there is none".

THE ARCHITECTURE, which is the thing to know before reading any of the rest:
  midi_read_cb() DISPATCHES INLINE. dispatch_sysex(), dispatch_cc() and dispatch_program_change() all
  run on the CoreMIDI callback thread, and there is NO MIDI -> UI queue at all - only gToMidiThread
  going the other way. So every incoming message mutates shared state directly on the CoreMIDI
  thread while the render thread reads it. That is the same shape as EmuUtility, and deliberate for
  the same reason: what SynthEdit reports upward is coalescing dirty state, which must collapse N
  updates into one redraw, and a queue would enqueue N.

CHECKED, AND SOUND - recorded so nobody "fixes" them:
  - gDevice.progName is written on the CoreMIDI thread by three separate paths
    (set_prog_name_display, extract_prog_info, synth_decode_moog_name) and read by synth_render on
    the render thread. It IS raced, and the effect is cosmetic rather than dangerous: every one of
    the three bounds its characters at < size-1 and only ever writes '\0' at the last index, and the
    array starts zero-initialised - so the final byte is permanently NUL and a reader can never walk
    off the end. The worst case is one frame showing a new prefix with an old tail.
    That safety is a consequence of the bounds discipline rather than of anything that says so; a
    fourth writer that filled the array to the brim would remove it silently.
  - The backup/sweep state machine spans both threads BY DESIGN and says so
    (synthBackup.c:174-181): gBackupBatchActive is _Atomic, the CoreMIDI thread copies, and the
    main thread branches. Not an oversight.

NOT EXAMINED, and worth saying plainly:
  - The 154 file-statics in synthBackup.c individually - only the batch flag and the sweep entry
    points were traced.
  - The panel dial VALUES, written from dispatch_cc/dump decode on the CoreMIDI thread and read by
    the renderer. Expected to be the benign scalar case, but not verified dial by dial.
  - The name cache and its on-disk half.
  - synthComms.c's 46 statics.

WHY IT LOOKS BETTER THAN GENBRIDGE DID: the two-thread split here was designed in from the start and
is commented at the points where it matters, where GenBridge grew its worker and its editor around a
processor that began single-threaded. The seven races fixed in GenBridge were all in state that
predated the thread that came to touch it.

2026-09-09  METAL ONLY ON macOS - THE WHOLE PROJECT, NOT JUST THE PLUG-IN

SynthLib's renderBackendSelect.h now defines SYNTHLIB_NO_GL_BACKEND on every Apple target, so the
OpenGL backend is left out of the build entirely: renderBackendGL.c compiles to nothing (the guard is
inside the file, because SynthLib/src is a synchronized folder in the Xcode projects and a build
script cannot exclude it), renderBackend.c does not declare its table, and gfx_backend_available()
answers false for it. The default backend follows the platform: Metal on Apple, OpenGL elsewhere.

THE MENU ITEM IS GONE with it. "Use Metal Renderer (on restart)" / "Use OpenGL Renderer (on restart)"
offered a switch to a backend that is no longer linked. Only the greyed "Renderer: <name>" readout
stays, so there is still a way to see what drew the window.

THE WAY BACK IS A REBUILD, not a preference, and that is a deliberate step down in convenience:
prefs.txt's renderBackend key cannot select a backend that is not in the binary. SYNTHLIB_ALLOW_GL_ON_APPLE
restores it. That switch also answers renderBackendGL.c's own argument for staying alive on macOS -
running the two renderers against each other on one machine is the cheap way to prove the Metal port
moved no pixel, and it is still possible.

NOTHING IN renderBackendGL.c WAS DELETED and nothing should be. It is the whole renderer for the
Windows and Linux versions to come, it is OpenGL 1.1 with no platform in it, and it is what #else
selects everywhere that is not Apple.

AND THE MENU THAT HELD IT WENT TOO (2026-09-09). The Experimental menu existed for one thing - the
OpenGL/Metal choice - so once macOS became Metal only there was a whole top-level menu carrying a
single greyed "Renderer: <name>" line. Both are gone; the About box has printed the renderer all
along (synthlib_about_text()), which is where that information belongs and where it now lives alone.


2026-09-12  MIDI PORTS STATUS: HONEST FOR NO-IDENTITY SYNTHS, AND THE INPUT FALLBACK
------------------------------------------------------------------------------------------------------
A synth that answers no identity request (Voyager, Minitaur) used to report "Connected" the moment an
output was chosen, with nothing verified. The status now says "Sending to X - not verified" until the
synth's own traffic - a CC, a program change or SysEx - arrives, then "Connected: heard from it on Y".

An identity reply from an input other than the chosen one was dropped silently; it is now a fallback,
taken only if nothing answers on the chosen input, and said so in the status. The same fix got
EmuUtility's E5000 connecting through a Cirklon, which delivers its MIDI 2 input as "Port 2".
Both built, not yet checked on the hardware (todo.md).


2026-09-12  PREV / NEXT LOOKED MORE CLICKABLE WHEN THEY WERE NOT
------------------------------------------------------------------------------------------------------
With the program unknown (a Voyager never reports one until a Program Change is seen) Prev and Next
are rightly inert - but they were drawn with the "disabled" face RGB_GREY_3, which on the dark palette
IS the background grey, and draw_button() picks its label colour for contrast with the face: white. So
an unavailable button showed a brighter label than an enabled one's black. Unavailable nav buttons are
now their label alone in RGB_GREY_5, no face and no outline (draw_nav_button(), synthGraphics.c); Sync
from synth gets the same treatment when disconnected.

