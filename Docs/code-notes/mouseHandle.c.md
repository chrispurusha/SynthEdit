# mouseHandle.c notes

The longer comments from `mouseHandle.c`, moved here 2026-09-12 so the code reads cleanly. The code points at each as `// notes §k`. Verbatim and in file order; each is titled by what it documents.

## 1. `gDraggedDial`

── Dial drag state ───────────────────────────────────────────────────────────
Deliberately just a pointer into whichever tPanelDial was hit — this file
has no knowledge of what any given dial *is* (cutoff, resonance, routing,
...). That all comes from the descriptor parsed out of the layout file
(see panelConfig.h/synthComms.c) and is looked up generically by rect.

## 2. `gDragPrevX`

LOGICAL coordinates, not window pixels — the cursor handler is handed logical ones now.
window_to_logical is a linear scale with no offset, so the difference of two logical coordinates
equals the converted difference of two window ones, which is exactly what delta_to_logical()
computed here; the drag rate is unchanged.

## 3. `gPressedToggleDial`

A panel_dial_is_binary() dial (rendered as a button, not a knob — either
a power button or a plain named one, see synthGraphics.cpp) takes a
single click rather than the drag gesture every other dial uses:
pressing one arms it here instead of gDraggedDial, and releasing still
over the same dial flips it (0<->1 — the only two states a binary dial
has), same press-on-mouse-up convention as gPressedTab/gPressedPatchNav
above. A dial that looks like a button but only responded to a knob's
drag gesture wasn't actually clickable in practice with no synth
connected to confirm otherwise.

## 4. `gPressedValueMenuDial`

Same press-arms/release-fires shape as gPressedToggleDial above, for a
panel_dial_needs_value_menu() dial — a discrete selector with no CC at
all. Must open the menu on RELEASE, not press: opening it on press means
the very same click's own release immediately dismisses it again (the
"Dismiss context menu" check at the top of the release path closes
whatever's active on ANY release) before a second click could ever reach
it. Real bug hit and fixed 2026-07-08 building this.

## 5. `arm_dial_press()`

Given a dial that was just hit on press — via panel_dial_press_click_handler,
registered for both the main per-page grid and the Info Row (see
synthGraphics.cpp's synth_render()) — arms whichever interaction it needs: a
value-menu dropdown (3+ named positions, no CC — or, as of 2026-07-13, a
2-position dial explicitly opted in via asMenu=, e.g. the Z1's Porta
on/off, to pick up its section colour instead of the toggle styling
below), a click-to-toggle (2 named positions, e.g. On/Off or, as of
2026-07-11, any 2-way enum like triggerMode's Single/Multi Trigger —
panel_dial_is_binary() doesn't require the names to literally be
"Off"/"On"), or a plain drag (anything
else, e.g. a raw-numeric Info Row dial like midiClkDivider). Which
hit-test found the dial doesn't change which of these three it needs —
that's entirely the dial's own descriptor — so both call sites share
this instead of each re-implementing the same 3-way branch. Previously
only the per-page grid path had all three; the Info Row path only had
the value-menu case, silently leaving a 2-option Info Row dial (like
triggerMode) or a raw-numeric one (like midiClkDivider) with no click
behaviour at all.

## 6. in `arm_dial_press()`

A readOnly dial (panelConfig.h — e.g. Voyager's hPhoneVolume, which
just mirrors a real analog pot's live position) takes no interaction
at all: no drag, no toggle, no dropdown. Checked first, before any of
the three interactive branches below, so it's a true no-op rather
than accidentally falling into one of them.

## 7. in `arm_dial_press()`

Same no-op treatment for a dial currently gated off by disableUnless=
(panelConfig.h) — e.g. Filter 2's own controls while Filter 1&2 Link
is ON, or Filter-B's controls while that filter's own Type isn't
2BPF. Checked right after readOnly, before any interactive branch.

## 8. `synth_commit_prog_name_edit()`

── Program name edit (gProgNameEdit, globalVars.h) ──────────────────────────
Shared by the Enter-key commit path (handle_key() below) and the
click-outside-the-field commit path (handle_mouse_button() below) — see
the latter's own comment for why a stray click commits rather than
silently discarding or being ignored.

## 9. `get_global_gui_scaled_mouse_coord()`

window_to_logical() moved into SynthLib (declared in inputState.h, implemented in
inputStateGlfw.c): it was character-identical here and in the other editor, and inlined in
the third — where it had lost the divide-by-zero guard both copies here kept. SynthLib owns
the window, so the shared one takes no window argument.

## 10. `clamp_dial_value()`

shift_held(win) USED TO BE HERE, polling glfwGetKey() through a hand-declared extern. It is now
SynthLib's shift_modifier_held(), reading state that graphics.c pushes from the `mods` argument
GLFW already hands every key and mouse-button callback. G2-Edit and the G2 VST3 plug-in read the
same predicate, the plug-in pushing from an NSEvent instead — see SynthLib/src/inputState.h.

THE OLD COMMENT ARGUED FOR THE POLL, so here is why that argument does not survive. It said a
drag's per-pixel resolution must react to Shift changing MID-DRAG "without requiring the key event
to have fired through this specific window/callback first". Pressing Shift during a drag IS a key
event, and it is delivered to the window being dragged in, because that window has the keyboard
focus — there is no ordering to lose. The one case the poll genuinely covered better was Shift
released while ANOTHER application had focus, and that is now handled properly rather than
incidentally: graphics.c clears the state on focus loss, where the poll would instead have kept
reporting a key this process could no longer see.

## 11. `clamp_dial_value()`

The Shift-slows-the-drag policy is SynthLib's dial_drag_pixels_for_full_range() (geometry.h) now,
shared with G2-Edit's canvas dials so that "finer" cannot come to mean two different things. It was
local here first, including the reasoning for its floor and the Clock Div bug that shaped it; that
reasoning went with it.

## 12. in `prog_name_click_handler()`

Seed from the FLAT name — gDevice.progName may contain
nameLineWidth's own display-only '\n's (see extract_moog_name(),
synthComms.c), stripped back out here since the wire format has
no real line breaks, only synth_render()'s own wrap-for-display.

## 13. `recover_lost_dial_drag()`

A drag whose release never arrived — the button came up outside the window, or focus was lost
mid-gesture — leaves gDraggedDial pointing at a dial FOREVER. That is not merely a stuck cursor:
every later cursor_pos event keeps changing that dial's value AND sending it to the synth, so
simply moving the mouse silently edits real hardware. synthComms.c also relies on the invariant
that only one dial is ever under an active drag at a time.

Called once per frame from the render loop; a no-op unless the pointer really is stuck. G2-Edit
does the same with recover_lost_cursor(); EmuUtility with recover_lost_dial_drag().

## 14. in `handle_mouse_button()`

The modal cascade — file browser, bank browser, alert dialog, each with its own early return
and mouse-down/up gating, plus the alert's routing around its bank-picker dropdown — is
SynthLib's now. See synthlibPopups.h. It runs before anything else here, including the
drag-release handling below, so nothing underneath (a dial drag, the menu bar, the page tabs)
can start or continue while a modal popup is up.

This app carries GLFW's button/action pair rather than a tMouseButton, so the normalisation
happens here at the one call site that needs it.

## 15. in `handle_mouse_button()`

Release: end drag; restore cursor only for modes that hid it. GLFW's
cocoa backend already restores the cursor to wherever it was when
CURSOR_DISABLED was entered (see updateCursorMode() in
cocoa_window.m) as soon as we switch back to NORMAL — an explicit
glfwSetCursorPos() here on top of that was redundant, and two
independent warps in a row (GLFW's automatic one, then ours) risked
landing a pixel or two off from each other, enough to spill into a
neighbouring dial given how tightly packed these are (40px dial,
10px gap). Clear gDraggedDial before switching cursor mode, not
after — belt and braces against any reentrant callback.

## 16. in `handle_mouse_button()`

THE BAR'S OWN CLICK TEST USED TO BE HERE. It is dispatched by the coordinator above now, at
the lowest layer there is, which is what "ahead of everything else on mouse-down" meant when
this app had nothing ranked below it. Leaving the call as well would not have double-fired —
dispatch_click() returns true for a bar hit and this line was already unreachable — which is
precisely why it had to go rather than stay as belt and braces: an unreachable copy of a
dispatch rule is the thing that lets the two rules drift apart unnoticed.

EmuUtility keeps its equivalent call deliberately: it does not route clicks through the
coordinator at all, so there the bar is still the host's to test.

## 17. in `handle_mouse_button()`

Same click's mouse-down just opened/switched/closed this dropdown via
handle_menu_bar_click() above — landing back on the bar itself on mouse-up is not a
dropdown-item selection, so leave the state exactly as mouse-down left it. Must be
checked before handle_context_menu_click(): that call closes the menu itself whenever
coord doesn't land on any open item, which a bar click never does.

## 18. in `handle_mouse_button()`

A press outside the program-name field while mid-edit commits
whatever's typed so far — Enter/Escape (handle_key() below) are the
deliberate paths, but a stray click elsewhere in the UI shouldn't
strand the user in a half-finished edit with no visible way out.
Consumes the click (returns rather than falling through to whatever
else it landed on) — matches common text-field-blur convention, and
avoids the same click both committing a name AND, say, pressing Next.
A press back on the field itself falls through unhandled here; the
"Program name" press-check further down just re-arms the same active
state, a harmless no-op.

## 19. in `handle_mouse_button()`

Same press-and-release-on-the-same-target convention as above —
flips the dial only if the release also lands back on it.
panel_dial_hit_rect(), not ->rect: the press was accepted through the button's DRAWN
bounds, so the release has to be judged by the same ones or the bottom/right edge strip
arms the dial and then silently does nothing. See panelConfig.c.

## 20. in `handle_mouse_button()`

Opening the menu here, on release, is required, not just
convention-matching — the very next line of this same function
(the "Dismiss context menu" check above) unconditionally closes
whatever menu is gContextMenu.active on ANY release. Opening it
during press instead meant the release ending that same click
immediately dismissed the menu before it was ever visible to a
second click. Real bug hit and fixed 2026-07-08 building this.

## 21. in `handle_mouse_button()`

Fast path: every interactive widget on this press — page tabs,
Prev/Next/Sync, the program name field, Info Row dials, and the
current page's dial grid — registers its click region at render time
(see synthGraphics.cpp's render_page_tabs()/synth_render()). Priority
among them (page tabs before Prev/Next/Sync before the program name
field before Info Row before the grid, matching the old sequential
checks this replaced) doesn't need replicating via registration order:
none of these widgets ever occupy overlapping screen space, so
whichever one dispatch_click_region() matches is unambiguously the
right one regardless of registration order. Falls through to the
legacy generic-section loop below for anything not matched (should be
nothing today).

## 22. in `handle_mouse_button()`

Hit-test every section on the current panel page generically —
whatever dial (if any) is under the cursor, by rect alone. No dial ids
referenced here. Not gated on gDevice.connected: dials are bound to
gDevice regardless of whether a real synth is talking to us, so the GUI
stays testable (dragging updates gDevice/tries a MIDI send that quietly
no-ops) even with nothing plugged in.

## 23. in `handle_cursor_pos()`

Accumulates the fractional remainder across calls (same idiom as
the discrete/stepped branch above, gDragTypeAccum) rather than
truncating each individual cursor-move event's own delta and
discarding the remainder — the accumulator-less version silently
dropped any movement smaller than one whole unit, so several slow
sub-unit movements in a row could add up and then jump by more
than 1 at once on whichever event finally crossed a whole-unit
boundary, rather than advancing smoothly. Reusing gDragTypeAccum
is safe here: only one of the stepped/continuous branches is ever
reachable for a single dial (display type doesn't change
mid-drag), and it's reset to 0 at the start of every drag
(arm_dial_press()).

## 24. in `handle_scroll()`

Deliberate remaining exception: with no drag active, scroll always
nudges one shortcut dial — not something generalizable from a
rect-based hit-test, since handle_scroll isn't given a cursor position
to test against. Which dial (if any) is entirely up to the device's own
<device>.txt ("scrollDial <id>" — empty/absent means no shortcut).
Only applies while the page holding that dial's section is actually
active — no-ops harmlessly otherwise.
