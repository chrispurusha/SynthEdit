/*
 * The SynthEdit application.
 *
 * Copyright (C) 2026 Chris Turner <chris_purusha@icloud.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <stdint.h>

#include "defs.h"
#include "synthlibPopups.h"
#include "synthlibDefs.h"
#include "types.h"
#include "globalVars.h"
#include "utilsGraphics.h"
#include "menus.h"
#include "synthComms.h"
#include "synthGraphics.h"
#include "mouseHandle.h"
#include "appMenuBar.h"
#include "fileBrowser.h"
#include "bankBrowser.h"
#include "alertDialog.h"
#include "geometry.h"    // dial_drag_pixels_for_full_range() — the shared Shift-slows-the-drag policy
#include "inputState.h"

// ── GLFW constants (avoids pulling GLFW header into C) ────────────────────────
#define GLFW_CURSOR             0x00033001
#define GLFW_CURSOR_NORMAL      0x00034001
#define GLFW_CURSOR_DISABLED    0x00034003
#define GLFW_PRESS              1
#define GLFW_RELEASE            0
#define GLFW_REPEAT             2
#define GLFW_KEY_ESCAPE         256
#define GLFW_KEY_ENTER          257
#define GLFW_KEY_KP_ENTER       335
#define GLFW_KEY_BACKSPACE      259
#define GLFW_KEY_DELETE         261
#define GLFW_KEY_RIGHT          262
#define GLFW_KEY_LEFT           263
#define GLFW_KEY_HOME           268
#define GLFW_KEY_END            269

extern void glfwSetInputMode(void *, int, int);
extern int glfwGetMouseButton(void *, int);   // for recover_lost_dial_drag()
extern void glfwGetWindowSize(void *, int *, int *);
extern void glfwGetCursorPos(void *, double *, double *);

// ── Dial drag state ───────────────────────────────────────────────────────────
// Deliberately just a pointer into whichever tPanelDial was hit — this file
// has no knowledge of what any given dial *is* (cutoff, resonance, routing,
// ...). That all comes from the descriptor parsed out of the layout file
// (see panelConfig.h/synthComms.c) and is looked up generically by rect.
static tPanelDial * gDraggedDial          = NULL;
// LOGICAL coordinates, not window pixels — the cursor handler is handed logical ones now.
// window_to_logical is a linear scale with no offset, so the difference of two logical coordinates
// equals the converted difference of two window ones, which is exactly what delta_to_logical()
// computed here; the drag rate is unchanged.
static double       gDragPrevX            = 0.0; // cursor position at previous cursor_pos call — incremental delta
static double       gDragPrevY            = 0.0;
static int          gDragSkipCount        = 0;   // skip first N cursor_pos events after CURSOR_DISABLED — covers stale events + transition event

// Page tab press state — actions on mouse-up, not mouse-down (standard
// button behaviour: press-and-drag-off cancels the click). -1 = no tab
// currently pressed.
static int32_t      gPressedTab           = -1;
static double       gDragTypeAccum        = 0.0; // sub-step accumulator for discrete (named) dials

// Same press-on-mouse-up convention as gPressedTab above, for the Prev/Next
// patch buttons (see synth_hit_test_patch_nav() in synthGraphics.h).
static int32_t      gPressedPatchNav      = -1;

// A panel_dial_is_binary() dial (rendered as a button, not a knob — either
// a power button or a plain named one, see synthGraphics.cpp) takes a
// single click rather than the drag gesture every other dial uses:
// pressing one arms it here instead of gDraggedDial, and releasing still
// over the same dial flips it (0<->1 — the only two states a binary dial
// has), same press-on-mouse-up convention as gPressedTab/gPressedPatchNav
// above. A dial that looks like a button but only responded to a knob's
// drag gesture wasn't actually clickable in practice with no synth
// connected to confirm otherwise.
static tPanelDial * gPressedToggleDial    = NULL;

// Same press-arms/release-fires shape as gPressedToggleDial above, for a
// panel_dial_needs_value_menu() dial — a discrete selector with no CC at
// all. Must open the menu on RELEASE, not press: opening it on press means
// the very same click's own release immediately dismisses it again (the
// "Dismiss context menu" check at the top of the release path closes
// whatever's active on ANY release) before a second click could ever reach
// it. Real bug hit and fixed 2026-07-08 building this.
static tPanelDial * gPressedValueMenuDial = NULL;

// Given a dial that was just hit on press — via panel_dial_press_click_handler,
// registered for both the main per-page grid and the Info Row (see
// synthGraphics.cpp's synth_render()) — arms whichever interaction it needs: a
// value-menu dropdown (3+ named positions, no CC — or, as of 2026-07-13, a
// 2-position dial explicitly opted in via asMenu=, e.g. the Z1's Porta
// on/off, to pick up its section colour instead of the toggle styling
// below), a click-to-toggle (2 named positions, e.g. On/Off or, as of
// 2026-07-11, any 2-way enum like triggerMode's Single/Multi Trigger —
// panel_dial_is_binary() doesn't require the names to literally be
// "Off"/"On"), or a plain drag (anything
// else, e.g. a raw-numeric Info Row dial like midiClkDivider). Which
// hit-test found the dial doesn't change which of these three it needs —
// that's entirely the dial's own descriptor — so both call sites share
// this instead of each re-implementing the same 3-way branch. Previously
// only the per-page grid path had all three; the Info Row path only had
// the value-menu case, silently leaving a 2-option Info Row dial (like
// triggerMode) or a raw-numeric one (like midiClkDivider) with no click
// behaviour at all.
static void arm_dial_press(tPanelDial * dial, tCoord coord) {
    // A readOnly dial (panelConfig.h — e.g. Voyager's hPhoneVolume, which
    // just mirrors a real analog pot's live position) takes no interaction
    // at all: no drag, no toggle, no dropdown. Checked first, before any of
    // the three interactive branches below, so it's a true no-op rather
    // than accidentally falling into one of them.
    if (dial->readOnly) {
        return;
    }

    // Same no-op treatment for a dial currently gated off by disableUnless=
    // (panelConfig.h) — e.g. Filter 2's own controls while Filter 1&2 Link
    // is ON, or Filter-B's controls while that filter's own Type isn't
    // 2BPF. Checked right after readOnly, before any interactive branch.
    if (panel_dial_is_disabled(dial, synth_panel_config())) {
        return;
    }

    if (panel_dial_needs_value_menu(dial)) {
        // Opens on RELEASE, not here — see gPressedValueMenuDial's own
        // comment above for why.
        gPressedValueMenuDial = dial;
        return;
    }

    if (panel_dial_is_binary(dial)) {
        gPressedToggleDial = dial;
        return;
    }
    gDraggedDial   = dial;
    gDragPrevX     = coord.x;
    gDragPrevY     = coord.y;
    gDragTypeAccum = 0.0;

    if (synthlib_dial_mode() != eDialModeRotary) {
        gDragSkipCount = 3;
        glfwSetInputMode(synthlib_window(), GLFW_CURSOR, GLFW_CURSOR_DISABLED);
    }
}

// ── Program name edit (gProgNameEdit, globalVars.h) ──────────────────────────
// Shared by the Enter-key commit path (handle_key() below) and the
// click-outside-the-field commit path (handle_mouse_button() below) — see
// the latter's own comment for why a stray click commits rather than
// silently discarding or being ignored.
static void synth_commit_prog_name_edit(void) {
    gProgNameEdit.active = false;
    synth_set_program_name(gProgNameEdit.buffer);
}

// ── Coordinate helpers ────────────────────────────────────────────────────────

// window_to_logical() moved into SynthLib (declared in inputState.h, implemented in
// inputStateGlfw.c): it was character-identical here and in the other editor, and inlined in
// the third — where it had lost the divide-by-zero guard both copies here kept. SynthLib owns
// the window, so the shared one takes no window argument.

// Supplied for SynthLib's contextMenu.c to link against — see mouseHandle.h.
void get_global_gui_scaled_mouse_coord(tCoord * coord) {
    synthlib_mouse_coord(coord);   // see inputState.h
}

// Scale a window-space delta to logical-space delta
// delta_to_logical() removed: the cursor handler receives logical coordinates now, so a delta of
// two of them is already logical. See gDragPrevX/gDragPrevY.

// shift_held(win) USED TO BE HERE, polling glfwGetKey() through a hand-declared extern. It is now
// SynthLib's shift_modifier_held(), reading state that graphics.c pushes from the `mods` argument
// GLFW already hands every key and mouse-button callback. G2-Edit and the G2 VST3 plug-in read the
// same predicate, the plug-in pushing from an NSEvent instead — see SynthLib/src/inputState.h.
//
// THE OLD COMMENT ARGUED FOR THE POLL, so here is why that argument does not survive. It said a
// drag's per-pixel resolution must react to Shift changing MID-DRAG "without requiring the key event
// to have fired through this specific window/callback first". Pressing Shift during a drag IS a key
// event, and it is delivered to the window being dragged in, because that window has the keyboard
// focus — there is no ordering to lose. The one case the poll genuinely covered better was Shift
// released while ANOTHER application had focus, and that is now handled properly rather than
// incidentally: graphics.c clears the state on focus loss, where the poll would instead have kept
// reporting a key this process could no longer see.

// The Shift-slows-the-drag policy is SynthLib's dial_drag_pixels_for_full_range() (geometry.h) now,
// shared with G2-Edit's canvas dials so that "finer" cannot come to mean two different things. It was
// local here first, including the reasoning for its floor and the Clock Div bug that shaped it; that
// reasoning went with it.

// Clamps to the dial's own display-space range [0, max-1] — the one thing
// every dial has in common, regardless of what it controls.
static uint32_t clamp_dial_value(int32_t v, uint32_t max) {
    if (v < 0) {
        return 0;
    }

    if ((max > 0) && ((uint32_t)v >= max)) {
        return max - 1;
    }
    return (uint32_t)v;
}

void panel_dial_press_click_handler(tCoord coord, eClickPhase phase, void * userData) {
    (void)coord;

    if (phase != eClickPress) {
        return; // release is handled entirely by the global armed-state check in handle_mouse_button()
    }
    tCoord at = {0};

    get_global_gui_scaled_mouse_coord(&at);   // logical, to match what the drag stores
    arm_dial_press((tPanelDial *)userData, at);
}

void page_tab_click_handler(tCoord coord, eClickPhase phase, void * userData) {
    (void)coord;

    if (phase != eClickPress) {
        return; // action fires on release, via the global armed-state check in handle_mouse_button()
    }
    int32_t index = (int32_t)(intptr_t)userData;

    gPressedTab = index;
    synth_set_pressed_page_tab(index);
}

void patch_nav_click_handler(tCoord coord, eClickPhase phase, void * userData) {
    (void)coord;

    if (phase != eClickPress) {
        return; // action fires on release, via the global armed-state check in handle_mouse_button()
    }
    int32_t index = (int32_t)(intptr_t)userData;

    gPressedPatchNav = index;
    synth_set_pressed_patch_nav(index);
}

void prog_name_click_handler(tCoord coord, eClickPhase phase, void * userData) {
    (void)coord;
    (void)userData;

    if (phase != eClickPress) {
        return;
    }
    gProgNameEdit.active    = true;

    // Seed from the FLAT name — gDevice.progName may contain
    // nameLineWidth's own display-only '\n's (see extract_moog_name(),
    // synthComms.c), stripped back out here since the wire format has
    // no real line breaks, only synth_render()'s own wrap-for-display.
    uint32_t o = 0;

    for (const char * p = gDevice.progName; (*p != '\0') && ((o + 1) < sizeof(gProgNameEdit.buffer)); p++) {
        if (*p != '\n') {
            gProgNameEdit.buffer[o++] = *p;
        }
    }

    gProgNameEdit.buffer[o] = '\0';
    gProgNameEdit.cursorPos = o;
}

// ── Public handlers ───────────────────────────────────────────────────────────

// Ends a dial drag. Both the real mouse release and recover_lost_dial_drag() call this, so the two
// cannot drift apart. Clears gDraggedDial BEFORE switching cursor mode, not after — belt and braces
// against any reentrant callback.
static void end_dial_drag(void * win) {
    gDragSkipCount = 0;
    gDraggedDial   = NULL;

    if (synthlib_dial_mode() != eDialModeRotary) {
        glfwSetInputMode(win, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
    }
}

// A drag whose release never arrived — the button came up outside the window, or focus was lost
// mid-gesture — leaves gDraggedDial pointing at a dial FOREVER. That is not merely a stuck cursor:
// every later cursor_pos event keeps changing that dial's value AND sending it to the synth, so
// simply moving the mouse silently edits real hardware. synthComms.c also relies on the invariant
// that only one dial is ever under an active drag at a time.
//
// Called once per frame from the render loop; a no-op unless the pointer really is stuck. G2-Edit
// does the same with recover_lost_cursor(); EmuUtility with recover_lost_dial_drag().
void recover_lost_dial_drag(void * win) {
    if (gDraggedDial == NULL) {
        return;
    }

    if (glfwGetMouseButton(win, 0) == GLFW_PRESS) {   // left button genuinely still held
        return;
    }
    LOG_DEBUG("Recovering a dial drag whose release never arrived\n");
    end_dial_drag(win);
}

// The coordinate arrives already scaled and the button already decoded — SynthLib's shim does both,
// and updates the modifier state before this runs. See tSynthLibInputHandlers in synthlibWindow.h.
void handle_mouse_button(tCoord coord, tMouseButton button, int mods) {
    (void)mods;

    if ((button != mouseButtonLeftDown) && (button != mouseButtonLeftUp)) {
        return;   // left button only
    }
    bool pressed = (button == mouseButtonLeftDown);

    // The modal cascade — file browser, bank browser, alert dialog, each with its own early return
    // and mouse-down/up gating, plus the alert's routing around its bank-picker dropdown — is
    // SynthLib's now. See synthlibPopups.h. It runs before anything else here, including the
    // drag-release handling below, so nothing underneath (a dial drag, the menu bar, the page tabs)
    // can start or continue while a modal popup is up.
    //
    // This app carries GLFW's button/action pair rather than a tMouseButton, so the normalisation
    // happens here at the one call site that needs it.
    if (synthlib_popups_dispatch_click(coord, pressed ? mouseButtonLeftDown : mouseButtonLeftUp)) {
        synthlib_request_redraw();
        return;
    }

    // Release: end drag; restore cursor only for modes that hid it. GLFW's
    // cocoa backend already restores the cursor to wherever it was when
    // CURSOR_DISABLED was entered (see updateCursorMode() in
    // cocoa_window.m) as soon as we switch back to NORMAL — an explicit
    // glfwSetCursorPos() here on top of that was redundant, and two
    // independent warps in a row (GLFW's automatic one, then ours) risked
    // landing a pixel or two off from each other, enough to spill into a
    // neighbouring dial given how tightly packed these are (40px dial,
    // 10px gap). Clear gDraggedDial before switching cursor mode, not
    // after — belt and braces against any reentrant callback.
    if (!pressed && gDraggedDial) {
        end_dial_drag(synthlib_window());
        return;
    }
    // THE BAR'S OWN CLICK TEST USED TO BE HERE. It is dispatched by the coordinator above now, at
    // the lowest layer there is, which is what "ahead of everything else on mouse-down" meant when
    // this app had nothing ranked below it. Leaving the call as well would not have double-fired —
    // dispatch_click() returns true for a bar hit and this line was already unreachable — which is
    // precisely why it had to go rather than stay as belt and braces: an unreachable copy of a
    // dispatch rule is the thing that lets the two rules drift apart unnoticed.
    //
    // EmuUtility keeps its equivalent call deliberately: it does not route clicks through the
    // coordinator at all, so there the bar is still the host's to test.

    // Dismiss context menu
    if (gContextMenu.active) {
        // Same click's mouse-down just opened/switched/closed this dropdown via
        // handle_menu_bar_click() above — landing back on the bar itself on mouse-up is not a
        // dropdown-item selection, so leave the state exactly as mouse-down left it. Must be
        // checked before handle_context_menu_click(): that call closes the menu itself whenever
        // coord doesn't land on any open item, which a bar click never does.
        if (!pressed && within_rectangle(coord, app_menu_bar_rect())) {
            return;
        }
        handle_context_menu_click(coord); // closes the menu whether the click landed on an item or outside it
        return;
    }

    // A press outside the program-name field while mid-edit commits
    // whatever's typed so far — Enter/Escape (handle_key() below) are the
    // deliberate paths, but a stray click elsewhere in the UI shouldn't
    // strand the user in a half-finished edit with no visible way out.
    // Consumes the click (returns rather than falling through to whatever
    // else it landed on) — matches common text-field-blur convention, and
    // avoids the same click both committing a name AND, say, pressing Next.
    // A press back on the field itself falls through unhandled here; the
    // "Program name" press-check further down just re-arms the same active
    // state, a harmless no-op.
    if (pressed && gProgNameEdit.active && !synth_hit_test_prog_name(coord)) {
        synth_commit_prog_name_edit();
        return;
    }

    if (!pressed) {
        // Action a pressed tab only if release lands back on it — matches
        // standard button behaviour (press-and-drag-off cancels the click).
        if ((gPressedTab >= 0) && (synth_hit_test_page_tab(coord) == gPressedTab)) {
            synth_action_page_tab(gPressedTab);
        }
        gPressedTab           = -1;
        synth_set_pressed_page_tab(-1);

        if ((gPressedPatchNav >= 0) && (synth_hit_test_patch_nav(coord) == gPressedPatchNav)) {
            synth_action_patch_nav(gPressedPatchNav);
        }
        gPressedPatchNav      = -1;
        synth_set_pressed_patch_nav(-1);

        // Same press-and-release-on-the-same-target convention as above —
        // flips the dial only if the release also lands back on it.
        // panel_dial_hit_rect(), not ->rect: the press was accepted through the button's DRAWN
        // bounds, so the release has to be judged by the same ones or the bottom/right edge strip
        // arms the dial and then silently does nothing. See panelConfig.c.
        if (gPressedToggleDial && within_rectangle(coord, panel_dial_hit_rect(gPressedToggleDial))) {
            uint32_t current = get_panel_dial_value(gPressedToggleDial);

            synth_set_panel_dial_value(gPressedToggleDial, current ? 0 : 1);
        }
        gPressedToggleDial    = NULL;

        // Opening the menu here, on release, is required, not just
        // convention-matching — the very next line of this same function
        // (the "Dismiss context menu" check above) unconditionally closes
        // whatever menu is gContextMenu.active on ANY release. Opening it
        // during press instead meant the release ending that same click
        // immediately dismissed the menu before it was ever visible to a
        // second click. Real bug hit and fixed 2026-07-08 building this.
        if (gPressedValueMenuDial && within_rectangle(coord, panel_dial_hit_rect(gPressedValueMenuDial))) {
            open_dial_value_menu(coord, gPressedValueMenuDial);
        }
        gPressedValueMenuDial = NULL;
        return;
    }

    // Fast path: every interactive widget on this press — page tabs,
    // Prev/Next/Sync, the program name field, Info Row dials, and the
    // current page's dial grid — registers its click region at render time
    // (see synthGraphics.cpp's render_page_tabs()/synth_render()). Priority
    // among them (page tabs before Prev/Next/Sync before the program name
    // field before Info Row before the grid, matching the old sequential
    // checks this replaced) doesn't need replicating via registration order:
    // none of these widgets ever occupy overlapping screen space, so
    // whichever one dispatch_click_region() matches is unambiguously the
    // right one regardless of registration order. Falls through to the
    // legacy generic-section loop below for anything not matched (should be
    // nothing today).
    if (dispatch_click_region(coord, eClickPress)) {
        return;
    }
    // Hit-test every section on the current panel page generically —
    // whatever dial (if any) is under the cursor, by rect alone. No dial ids
    // referenced here. Not gated on gDevice.connected: dials are bound to
    // gDevice regardless of whether a real synth is talking to us, so the GUI
    // stays testable (dragging updates gDevice/tries a MIDI send that quietly
    // no-ops) even with nothing plugged in.
    tPanelDial *    hit          = NULL;
    tPanelSection * sections[PANEL_MAX_SECTIONS];
    uint32_t        sectionCount = synth_current_page_sections(sections, PANEL_MAX_SECTIONS);

    for (uint32_t s = 0; (s < sectionCount) && !hit; s++) {
        int32_t hitIdx = hit_test_panel_section(sections[s], coord);

        if (hitIdx >= 0) {
            hit = &sections[s]->dials[hitIdx];
        }
    }

    if (hit) {
        arm_dial_press(hit, coord);
    }
}

void handle_cursor_pos(tCoord coord) {
    if (!gDraggedDial) {
        return;
    }

    if (gDragSkipCount > 0) {
        gDragPrevX = coord.x;
        gDragPrevY = coord.y;
        gDragSkipCount--;
        return;
    }
    uint32_t range  = gDraggedDial->max;
    int32_t  newVal = (int32_t)get_panel_dial_value(gDraggedDial);

    if (synthlib_dial_mode() == eDialModeRotary) {
        double angle = calculate_mouse_angle(coord, gDraggedDial->rect);
        newVal = (int32_t)angle_to_value(angle, range);
    } else if (gDraggedDial->display == dialDisplayNames) {
        // Discrete/stepped control (few positions): accumulate delta into
        // whole-step increments rather than mapping delta directly to value.
        double  delta = 0.0;

        if (synthlib_dial_mode() == eDialModeHorizontal) {
            delta      = coord.x - gDragPrevX;
            gDragPrevX = coord.x;
        } else {
            delta      = gDragPrevY - coord.y;
            gDragPrevY = coord.y;
        }
        gDragTypeAccum += delta / 30.0;
        int32_t step  = (int32_t)gDragTypeAccum;
        gDragTypeAccum -= (double)step;
        newVal         += step;
    } else if (synthlib_dial_mode() == eDialModeVertical) {
        // Shift = a slower drag over more pixels — see pixels_for_full_range() for the mapping and
        // for why its floor is what it is. The Clock Div bug that shaped it (Shift speeding the drag
        // up on a narrow dial, 2026-07-12) is recorded there too.
        double  pixelsForFullRange = dial_drag_pixels_for_full_range(range);
        double  dy                 = gDragPrevY - coord.y;

        gDragPrevY      = coord.y;
        // Accumulates the fractional remainder across calls (same idiom as
        // the discrete/stepped branch above, gDragTypeAccum) rather than
        // truncating each individual cursor-move event's own delta and
        // discarding the remainder — the accumulator-less version silently
        // dropped any movement smaller than one whole unit, so several slow
        // sub-unit movements in a row could add up and then jump by more
        // than 1 at once on whichever event finally crossed a whole-unit
        // boundary, rather than advancing smoothly. Reusing gDragTypeAccum
        // is safe here: only one of the stepped/continuous branches is ever
        // reachable for a single dial (display type doesn't change
        // mid-drag), and it's reset to 0 at the start of every drag
        // (arm_dial_press()).
        gDragTypeAccum += dy * (double)(range - 1) / pixelsForFullRange;
        int32_t step               = (int32_t)gDragTypeAccum;

        gDragTypeAccum -= (double)step;
        newVal         += step;
    } else {
        double  pixelsForFullRange = dial_drag_pixels_for_full_range(range);
        double  dx                 = coord.x - gDragPrevX;

        gDragPrevX      = coord.x;
        gDragTypeAccum += dx * (double)(range - 1) / pixelsForFullRange;
        int32_t step               = (int32_t)gDragTypeAccum;

        gDragTypeAccum -= (double)step;
        newVal         += step;
    }
    synth_set_panel_dial_value(gDraggedDial, clamp_dial_value(newVal, range));
}

void handle_key(int key, int scancode, int action, int mods) {
    (void)scancode;

    // Same cascade as the clicks, and gone the same way — including the Escape precedence that let
    // the alert's bank-picker dropdown close before the dialog under it. See synthlibPopups.h.
    if (synthlib_popups_dispatch_key(key, mods, action)) {
        synthlib_request_redraw();
        return;
    }

    if (!gProgNameEdit.active) {
        return;
    }

    if ((action != GLFW_PRESS) && (action != GLFW_REPEAT)) {
        return;
    }
    size_t   len       = strlen(gProgNameEdit.buffer);
    uint32_t cursorPos = (gProgNameEdit.cursorPos <= len) ? gProgNameEdit.cursorPos : (uint32_t)len;

    if (key == GLFW_KEY_BACKSPACE) {
        if (cursorPos > 0) {
            memmove(&gProgNameEdit.buffer[cursorPos - 1], &gProgNameEdit.buffer[cursorPos], len - cursorPos + 1);
            gProgNameEdit.cursorPos = cursorPos - 1;
        }
    } else if (key == GLFW_KEY_DELETE) {
        if (cursorPos < len) {
            memmove(&gProgNameEdit.buffer[cursorPos], &gProgNameEdit.buffer[cursorPos + 1], len - cursorPos);
        }
    } else if (key == GLFW_KEY_LEFT) {
        if (cursorPos > 0) {
            gProgNameEdit.cursorPos = cursorPos - 1;
        }
    } else if (key == GLFW_KEY_RIGHT) {
        if (cursorPos < len) {
            gProgNameEdit.cursorPos = cursorPos + 1;
        }
    } else if (key == GLFW_KEY_HOME) {
        gProgNameEdit.cursorPos = 0;
    } else if (key == GLFW_KEY_END) {
        gProgNameEdit.cursorPos = (uint32_t)len;
    } else if ((key == GLFW_KEY_ENTER) || (key == GLFW_KEY_KP_ENTER)) {
        synth_commit_prog_name_edit();
    } else if (key == GLFW_KEY_ESCAPE) {
        // Cancel — discard edits
        gProgNameEdit.active = false;
    }
    synthlib_request_redraw();
}

void handle_char(unsigned int codepoint) {
    if (synthlib_popups_dispatch_char(codepoint)) {
        synthlib_request_redraw();
        return;
    }

    if (!gProgNameEdit.active) {
        return;
    }
    uint32_t maxLen    = synth_effective_name_maxlen();
    size_t   len       = strlen(gProgNameEdit.buffer);

    if ((codepoint < 0x20) || (codepoint > 0x7E) || (len >= maxLen) || ((len + 1) >= sizeof(gProgNameEdit.buffer))) {
        return;
    }
    uint32_t cursorPos = (gProgNameEdit.cursorPos <= len) ? gProgNameEdit.cursorPos : (uint32_t)len;

    memmove(&gProgNameEdit.buffer[cursorPos + 1], &gProgNameEdit.buffer[cursorPos], len - cursorPos + 1);
    gProgNameEdit.buffer[cursorPos] = (char)codepoint;
    gProgNameEdit.cursorPos         = cursorPos + 1;
    synthlib_request_redraw();
}

void handle_scroll(double dx, double dy) {
    (void)dx;

    if (synthlib_popups_dispatch_scroll(dy)) {
        return;
    }

    if (gDraggedDial) {
        return;
    }
    // Deliberate remaining exception: with no drag active, scroll always
    // nudges one shortcut dial — not something generalizable from a
    // rect-based hit-test, since handle_scroll isn't given a cursor position
    // to test against. Which dial (if any) is entirely up to the device's own
    // <device>.txt ("scrollDial <id>" — empty/absent means no shortcut).
    // Only applies while the page holding that dial's section is actually
    // active — no-ops harmlessly otherwise.
    tPanelConfig *  cfg          = synth_panel_config();

    if (cfg->scrollDialId[0] == '\0') {
        return;
    }
    tPanelSection * sections[PANEL_MAX_SECTIONS];
    uint32_t        sectionCount = synth_current_page_sections(sections, PANEL_MAX_SECTIONS);
    tPanelDial *    dial         = NULL;

    for (uint32_t s = 0; (s < sectionCount) && !dial; s++) {
        dial = find_panel_dial(sections[s], cfg->scrollDialId);
    }

    if (dial && !dial->readOnly && !panel_dial_is_disabled(dial, cfg)) {
        int32_t newVal = (int32_t)get_panel_dial_value(dial) + (int32_t)dy;
        synth_set_panel_dial_value(dial, clamp_dial_value(newVal, dial->max));
    }
}
