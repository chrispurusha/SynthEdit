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

// Generic parser for the panel-descriptor text format used by xxxx.txt (and,
// eventually, other <device>.txt files). Deliberately has no application-
// specific dependencies — candidate for promotion into SynthLib once a
// second project wants it.

#ifndef __PANEL_CONFIG_H__
#define __PANEL_CONFIG_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "geometry.h"

#define PANEL_ID_LEN               24 // raised from 16 2026-07-13 — discovered via the backdoor DUMP command (graphics.cpp): 19 real Z1 Amp/EG/LFO dial ids (e.g. "ampegnodetimemodsrc", 19 chars) silently truncated to 15+NUL on load (parse_dial_line()'s strncpy() in panelConfig.c has no length check/warning), losing up to 4 trailing characters with no error at all. No two truncated ids happened to collide this time (checked: `grep dial layouts/z1.txt | awk '{print substr($2,1,15)}' | sort | uniq -d` was empty) — pure luck, not a guarantee for any future id added to any device file. 24 gives real headroom over the current worst case (19) rather than tuning to the exact number again.
#define PANEL_LABEL_LEN            32
#define PANEL_MAX_NAMES            65 // raised from 20 to 32 2026-07-10 (Voyager's soundCategory, a full 32-value enum, 0-31); raised from 32 to 48 2026-07-11 — Voyager's pgmShaping1Src/pgmShaping2Src are 43-value enums (0-42), values >= the old 32 cap silently had no stored name and rendered as "?"; raised from 48 to 65 2026-07-13 — Voyager's tsGateCtrl uses storageOffset to give TS Gate's MIDI Ctrl No (64-127 plus a distinct Off state) a proper named 65-value enum instead of a raw 0-128 dial with an unlabeled dead zone below 64
#define PANEL_MAX_COLOURS          16
#define PANEL_MAX_DIALS            32
#define PANEL_MAX_SECTIONS         64 // raised 32->48 2026-07-13 (Amp/EG pages split narrow enough to hit the old ceiling at 34 sections — see panelConfig.c's own "too many sections" error for the actual failure mode, dials silently landing in the wrong section rather than a page just going missing); 48->64 same day adding LFO1-4 pushed close to 48 again — Z1's own remaining unbuilt pages (Effects, OSC-type sub-pages) will need more still, so raised with real headroom this time rather than tuning to the exact count again
#define PANEL_MAX_LIST_ITEMS       32
#define PANEL_MAX_LISTS            8
#define PANEL_MAX_COLUMN_LABELS    32

typedef enum {
    dialDisplayRaw = 0,
    dialDisplayCcNative,
    dialDisplayNames,
    // "display=hiLo" — for a dial whose raw dump bits are a single 16-bit
    // TWO'S-COMPLEMENT signed value that a real front panel splits into two
    // separately-adjustable coarse/fine controls sharing one storage word
    // (Voyager's PGM Shaping 1/2 "Fixed Value" HIGH/LOW, confirmed
    // 2026-07-11/12: signed_raw = 512 + HIGH*1024 + LOW*8, HIGH/LOW both
    // -64..+63, with LOW overflowing carrying into HIGH exactly like a
    // coarse/fine odometer pair — see synth_decode_hilo_dial() in
    // synthGraphics.cpp for the derivation. Genuinely NOT a plain bit-slice:
    // confirmed 2026-07-12 that the wire bits alone cannot always
    // distinguish HIGH from HIGH+64 — this isn't a narrow boundary case,
    // EVERY HIGH in -64..-1 is bit-for-bit identical on the wire to HIGH+64
    // (0..63) for the same LOW. The device's own front panel apparently
    // tracks HIGH/LOW as separate UI state that a dump can't always fully
    // recover. synth_decode_hilo_dial() picks the plain two's-complement
    // reading as a deterministic, self-consistent display convention —
    // accepted by the owner as a known display-only limitation (the actual
    // value sent to the device is always correct regardless of which
    // labeling is shown; only the on-screen HIGH number can disagree with
    // the real hardware's own screen when the true HIGH is negative). The
    // dial's own dumpOffset/dumpBitWidth still reads
    // the raw 16-bit word normally via the existing generic bitfield
    // extraction (no engine change needed there) — this display mode only
    // changes how that raw value is FORMATTED ("High: -2  Low: +63") and
    // how a drag maps back to storage (see mouseHandle.c). Generic, not
    // Voyager-specific — any device with a real hi/lo coarse+fine pair
    // sharing one signed dump word can use it the same way; the specific
    // 512/1024/8 constants are declared per-dial via
    // hiLoOffset=/hiLoCoarseScale=/hiLoFineScale= (no hidden defaults —
    // every dial using this display mode must declare all three
    // explicitly, same "no magic numbers in generic code" reasoning as
    // dumpNativeMax/nativeMax already follow).
    dialDisplaySignedHiLo,

    // "display=note" — this dial's raw value IS a MIDI note number (0-127)
    // rather than an arbitrary quantity, so it's shown as a note name
    // (C-1..G9, note 0 = C-1, matching the synth's own front-panel
    // convention) instead of a bare integer. Added 2026-07-13 for the Z1's
    // Filter Lo/Hi Key keyboard-track boundaries (f1lowkey/f1highkey and
    // the same shape ×3 more — f1b/f2/f2b, see linkedMaxDialId/
    // linkedMinDialId above), whose CC in/out were confirmed already
    // correct — this only changes on-screen FORMATTING, nothing sent or
    // received on the wire (dial otherwise behaves exactly like a plain
    // max=128 dialDisplayRaw dial).
    dialDisplayNote,

    // "display=signed" — this dial's raw wire/dump value is an unsigned
    // 0..max-1 count that represents a SIGNED quantity centred somewhere in
    // that range on the real front panel (e.g. the Z1's Filter Lo/Hi
    // Int/ModEG Int/Mod1 Int/Mod2 Int/Res Mod Int family: raw 0-198 on the
    // wire, shown as -99..+99 on the synth's own display). Shown as
    // `(int)dialVal - displayOffset` rather than the bare raw count.
    // displayOffset must be declared explicitly per-dial (no hidden
    // default, same "no magic numbers in generic code" reasoning as
    // dialDisplaySignedHiLo's hiLoOffset/scale attributes) — purely a
    // display-time subtraction, doesn't touch storageOffset or anything
    // sent/received on the wire.
    dialDisplaySigned,
} tDialDisplay;

typedef struct {
    char name[PANEL_ID_LEN];
    tRgb colour;
} tPanelColour;

// A named string list not tied to any dial — e.g. a device's patch-category
// or voice-mode names, shown as plain text rather than driving a control.
typedef struct {
    char     name[PANEL_ID_LEN];
    char     items[PANEL_MAX_LIST_ITEMS][PANEL_LABEL_LEN];
    uint32_t itemCount;
} tPanelList;

// A title printed above one grid column (e.g. "Osc 1", "Mixer") —
// "columnLabel <col> <text>" in the device's own .txt, one line per
// labelled column. Page-scoped, not global: col=/row= positions are only
// unique within a single page (see tPanelDial.gridCol's own comment), so
// "column 3" on one page and "column 3" on another can have entirely
// different labels, or only one of them labelled at all. Purely cosmetic —
// synth_render() (synthGraphics.cpp) reserves a header row above a grid
// page's dials only if the page has at least one of these; a page with
// none renders exactly as it did before this existed.
typedef struct {
    char    page[PANEL_ID_LEN];
    int32_t col;
    char    label[PANEL_LABEL_LEN];
} tColumnLabel;

typedef struct {
    char         id[PANEL_ID_LEN];                        // e.g. "f1cut" — looked up by find_panel_dial()
    char         label[PANEL_LABEL_LEN];                  // e.g. "F1 Cut"
    char         colourName[PANEL_ID_LEN];                // as written in the file, e.g. "f1"
    tRgb         colour;                                  // resolved against the section's colour table at parse time
    uint32_t     max;                                     // count of valid display-space positions: 0..max-1
    tDialDisplay display;
    char         names[PANEL_MAX_NAMES][PANEL_LABEL_LEN]; // populated when display == dialDisplayNames
    uint32_t     nameCount;
    double       gapBefore;                               // extra flow-space inserted before this dial
    tRectangle   rect;                                    // populated by layout_panel_section(); used for render + hit-test

    // "linkedMaxDial=<id>"/"linkedMinDial=<id>" — this dial's value can
    // never be set (from ANY source that goes through synth_set_panel_
    // dial_value(): drag, value-menu click, or a programmatic set) above/
    // below the NAMED dial's own CURRENT value. Added 2026-07-13 for the
    // Z1's Filter keyboard-track Lo/Hi Key pairs (f1lowkey/f1highkey and
    // the same shape ×3 more — f1b/f2/f2b): real hardware enforces Lo Key
    // <= Hi Key itself, silently clamping whichever one crosses — owner
    // wanted the app to enforce the SAME constraint locally rather than
    // let an invalid combination get sent at all and rely on a later Sync
    // to reveal the hardware's own correction. Deliberately NOT applied on
    // the DECODE side (apply_dial_wire_value()/extract_prog_info() etc.) —
    // an incoming dump/CC is already the hardware's own resolved truth,
    // nothing to re-clamp there. Resolved by id via find_panel_dial_
    // anywhere() at the moment of each value-set (not cached, so it always
    // reflects whichever value the linked dial most recently held) — empty
    // string (the default) means no constraint, so this is a no-op for
    // every dial in every OTHER device file.
    char linkedMaxDialId[PANEL_ID_LEN];
    char linkedMinDialId[PANEL_ID_LEN];

    // "disableUnless=<dialId>:<value>" — this dial is greyed out and takes
    // no interaction (same as readOnly — see arm_dial_press() in
    // mouseHandle.c) unless the NAMED dial's own CURRENT display value
    // equals <value>. Added 2026-07-13 per the Z1 Owner's Manual (p.52/53):
    // Filter 2's own controls are only real when Filter 1&2 Link is OFF
    // ("When this is ON, filter 2 settings cannot be made" — the manual's
    // own words), and Filter-B's controls only matter when that filter's
    // own Type is 2BPF ("If 2BPF is selected, the parameters explained in
    // 'Filter B settings...' will be displayed"). Purely a UI convenience —
    // deliberately NOT a write-side block in synth_set_panel_dial_value()
    // (a disabled dial simply can't be dragged/clicked in the first place,
    // per arm_dial_press(), so there's nothing left to guard there) and NOT
    // a substitute for reading back a full dump to confirm a value actually
    // took — it just stops the user from attempting an edit the real
    // hardware would ignore anyway, rather than the app silently sending
    // something the synth won't apply. Resolved by id via find_panel_dial_
    // anywhere() at render/hit-test time (not cached), same pattern as
    // linkedMaxDialId/linkedMinDialId above — empty string (the default)
    // means always-enabled, a no-op for every dial in every OTHER device
    // file. See panel_dial_is_disabled() below.
    char     disabledUnlessDialId[PANEL_ID_LEN];
    uint32_t disabledUnlessValue;

    // Protocol wiring — describes how to read/write/send this control's value,
    // so generic code (mouse handling, rendering) never needs to know what a
    // given dial *means*. All parsed from the file. The dial owns its own
    // live value/nativeValue directly (see get_panel_dial_value() etc.) —
    // there is no separate application-side struct/binding step for any of
    // this, which is what makes a new device just a new <device>.txt file.
    int32_t  storageOffset;     // storage_value = display_value + storageOffset (e.g. 1-5 vs 0-4 for "type")
    int32_t  displayOffset;     // dialDisplaySigned only — shown_value = display_value - displayOffset; see that enum value's own comment above. Unrelated to storageOffset: never touches the wire.
    uint32_t paramGroup;        // SysEx parameter group
    uint32_t paramId;           // SysEx parameter ID
    uint32_t ccNumber;          // MIDI CC number; 0 = not CC-controlled (send SysEx param change instead)
    // MIDI's own 14-bit CC convention: controller N (0-31) carries the coarse/
    // MSB half, N+32 the fine/LSB half — combined value = (msb<<7)|lsb, giving
    // 0..16383 instead of a plain CC's 0..127. 0 = ccNumber alone is a plain
    // 7-bit CC (default; existing single-cc dials are unaffected). Set via the
    // file's "ccLsb=" dial attribute alongside "cc=".
    uint32_t ccLsbNumber;
    uint8_t  ccMsbLatched;      // last raw byte seen on ccNumber; only meaningful when ccLsbNumber != 0
    uint8_t  ccLsbLatched;      // last raw byte seen on ccLsbNumber; only meaningful when ccLsbNumber != 0
    uint32_t nativeMax;         // native/SysEx value range when paired with a CC (0 = no native pairing)
    uint32_t value;             // live storage-space value (display_value + storageOffset); wide enough
                                // for a 14-bit CC pair, not just a single byte
    uint8_t  nativeValue;       // live native value, if nativeMax != 0; unused otherwise

    // Debounce for a live CC-driven quantized switch/selector (nativeMax != 0
    // && display == dialDisplayNames) — a real detented rotary/toggle switch
    // was confirmed 2026-07-08 (Voyager's LFO Sync, watching timestamped
    // dispatch_cc() output) to send several transitional raw CC bytes within
    // 20-60ms of each other as its wiper physically clicks between detents,
    // before settling — applying each one immediately made the on-screen
    // value visibly flicker through intermediate/wrong positions on every
    // switch flip. hasPendingCc/pendingRawValue/pendingSinceMs let
    // synth_handle_cc() (synthComms.c) hold the latest raw byte without
    // applying it, resetting the timestamp on every new CC for this dial;
    // synth_flush_pending_cc() (called once per frame from the render loop)
    // only commits it once CC_DEBOUNCE_MS have passed with no further
    // message. A plain continuous dial (nativeMax == 0) is untouched by any
    // of this — its live CC stream is a genuine real-time sweep, not bounce,
    // so it still applies immediately.
    bool     hasPendingCc;
    uint32_t pendingRawValue;
    double   pendingSinceMs;

    // Debounce for the OUTGOING side of a dump-only dial (no CC at all,
    // dumpBitWidth > 0 — e.g. Voyager's Headphone Volume/Filter Pole Select).
    // synth_set_panel_dial_value() (synthComms.c) used to call
    // synth_patch_and_resend_moog_dump() immediately on every value change —
    // fine for a click-through selector, but a mouse-dragged continuous dial
    // (see mouseHandle.c) can call that many times a second while dragging,
    // each one resending the ENTIRE ~147-byte cached dump (unlike a plain CC
    // send, 3 bytes). hasPendingDumpSend/pendingDumpRawValue/
    // pendingDumpSinceMs hold the latest value without sending, resetting
    // the timestamp on every further change; synth_flush_pending_dump_sends()
    // (called once per frame, same as synth_flush_pending_cc()) waits until
    // CC_DEBOUNCE_MS have passed with no further change, THEN moves on to
    // the second phase below (dumpSendAwaitingFreshData) rather than sending
    // directly — same trailing-edge idiom as hasPendingCc above for settling
    // the value, just followed by an extra fetch-fresh-data step before the
    // actual send.
    bool     hasPendingDumpSend;
    uint32_t pendingDumpRawValue;
    double   pendingDumpSinceMs;

    // Second phase after the debounce above settles — added 2026-07-10,
    // owner's own idea: patching straight into gLastMoogDump (synthComms.c)
    // and resending risked carrying stale data for every OTHER field in that
    // cached dump, not just the one the user meant to change (gLastMoogDump
    // is only as fresh as the last Panel Dump reply, which could be from
    // connect time or the last Sync). synth_flush_pending_dump_sends() sets
    // dumpSendAwaitingFreshData=true instead of sending immediately once the
    // debounce above elapses, and requests a fresh Panel Dump
    // (synth_request_state_dump()) if one isn't already in flight for this
    // reason (see gAwaitingFreshDumpForPatch, synthComms.c). When that fresh
    // reply arrives, extract_moog_panel_info() skips overwriting this dial's
    // own display value (it would otherwise stomp the user's pending choice
    // with the OLD, pre-change hardware value), and
    // synth_apply_pending_dump_patches() patches pendingDumpRawValue into
    // the now-fresh gLastMoogDump and sends once, clearing this flag.
    bool dumpSendAwaitingFreshData;

    // Where this dial's value lives in a full program-dump byte buffer (a
    // different wire format from individual parameter-change messages, but
    // still just data the file describes). -1 = not present in a dump.
    int32_t  dumpOffset;
    uint32_t dumpShift;          // bits to shift right before masking (default 0)
    uint32_t dumpMask;           // mask applied after shifting (default 0xFF = whole byte)

    // Alternative to dumpShift/dumpMask above for a dump format where a
    // value's bits are packed continuously across MULTIPLE bytes at 7 usable
    // bits each (Moog's Panel/Preset Dump SysEx — see moogStyleDump in
    // tPanelConfig below), rather than living inside one byte. dumpBitWidth=0
    // (the default) means "not this kind of field" — use dumpShift/dumpMask
    // on the single dumpOffset byte instead, unchanged from before this
    // existed. When dumpBitWidth > 0: dumpOffset is still this field's first
    // relevant byte, and dumpBitOffset (0-6) is which of that byte's 7 usable
    // bits holds the field's LEAST significant bit — confirmed empirically
    // against real Voyager hardware (set Filter Cutoff/Resonance to known CC
    // values, requested a Panel Dump, decoded bit-for-bit) that a field's
    // bits then continue in strict ascending significance through the
    // 7-bit-per-byte stream (byte's bit 6 done -> next byte's bit 0), with
    // no padding except where a genuinely separate field's bits interleave.
    uint32_t dumpBitOffset;
    uint32_t dumpBitWidth;

    // Second bit location for a value whose bits are NOT contiguous in the
    // dump — e.g. Voyager's Filter A/B "Pole Select" (1/2/4-pole) each pack
    // their 2-bit value across two unrelated bytes, confirmed 2026-07-08 via
    // before/after hardware captures (tools/moog_dump + tools/syx_diff.py):
    // Filter A's two bits happen to be adjacent (ordinary dumpBitWidth=2
    // suffices), but Filter B's live in two bytes nowhere near each other.
    // dumpBitWidth2=0 (the default) means "no second chunk" — every dial
    // behaves exactly as before this existed. When set, the dial's full raw
    // value is chunk1 (dumpOffset/dumpBitOffset/dumpBitWidth, the LOW bits)
    // with chunk2 (dumpOffset2/dumpBitOffset2/dumpBitWidth2) contributing the
    // next-significant bits above it — same "two locations combine into one
    // value" shape ccLsbNumber above already uses for a CC pair, just for
    // dump bits instead.
    int32_t  dumpOffset2;
    uint32_t dumpBitOffset2;
    uint32_t dumpBitWidth2;

    // dumpNativeMax/dumpInvert — a dump-decoded raw value can need different
    // handling from the SAME dial's CC-decoded one. Confirmed 2026-07-08
    // physically toggling Voyager's Ext On/Osc 1-3 On switches and diffing
    // the dump both ways:
    //   - The dump bit is INVERTED relative to the CC's own On/Off sense
    //     (raw dump bit 0 = On, 1 = Off) — dumpInvert=1 flips the raw value
    //     (across dumpBitWidth+dumpBitWidth2 bits) before native/display
    //     scaling, in extract_moog_panel_info()/synthComms.c.
    //   - nativeMax=127 (needed to decode the CC's own 0-63/64-127 threshold)
    //     would wrongly crush a raw dump bit of 0/1 straight down to display
    //     0 regardless of which one it was, if reused as-is for the dump
    //     path. dumpNativeMax=0 (the default) falls back to nativeMax,
    //     unchanged for a dial with only ONE wire representation (e.g.
    //     Filter A/B Pole Select, dump-only, where nativeMax alone already
    //     means the right thing) — set it explicitly (e.g. dumpNativeMax=1
    //     for a plain toggle bit) only when a dial has both a CC and a dump
    //     bit needing different scales.
    uint32_t dumpNativeMax;
    bool     dumpInvert;

    // Explicit grid position ("col="/"row=" in the file), for a device whose
    // real front panel groups controls into fixed columns rather than
    // flowing left-to-right within a section (see gridColWidth/gridRowHeight
    // below). 0-based; -1 (the default) means "not grid-positioned" — this
    // dial keeps going through the ordinary left-to-right auto-flow within
    // its section, exactly as every dial did before this existed, so a
    // device file with no col=/row= anywhere (Z1, Novation Supernova 2)
    // renders identically to before. A section can freely mix grid and
    // auto-flow dials, though in practice a device either commits one whole
    // page to the grid or doesn't use it at all. row defaults to 0 if col is
    // set but row isn't.
    //
    // double, not int — 2026-07-07: aligning a short column's controls to a
    // taller neighbour (Mod Wheel/Pedal's 4 controls against Mixer's 5-tall
    // Level/On columns) needs its middle rows evenly spread across a
    // non-integer gap, not just whole grid steps. "row=2.67" places a dial
    // two-thirds of the way between grid rows 2 and 3.
    double gridCol;
    double gridRow;

    // "noLabel" in the file — for a binary/value-menu button (see
    // panel_dial_is_binary()/panel_dial_needs_value_menu()) whose own
    // current-value text is ALREADY self-explanatory without its dial's
    // label underneath (e.g. Filter A/B Pole Select showing "2 Pole",
    // Filter Mode showing "Dual LP"/"HP/LP") — suppresses the separate
    // label line synth_render() would otherwise draw beneath the button,
    // saving vertical space with no loss of clarity. Default false (show
    // the label) because most value-menu buttons are NOT self-explanatory
    // on their own — e.g. Voyager's "Menu Settings" page dials show things
    // like "Lower Key"/"Single Trigger" with no photo-realistic panel
    // position to supply context the way the main panel page's Filters
    // column does for Pole Select, so those need the label kept. A genuine
    // Off/On toggle (panel_dial_is_toggle()) is unaffected either way — it
    // always shows just its own label on the button face (colour conveys
    // on/off), a separate, older mechanism this doesn't change.
    bool noLabel;

    // "readOnly" in the file — for a dial whose dump/CC field genuinely
    // cannot be changed by this app, so mouse interaction (drag, click,
    // dropdown) should do nothing at all rather than optimistically update
    // the display and attempt a write that will never actually take effect.
    // Added 2026-07-11 for Voyager's hPhoneVolume: its dump field turned out
    // to just mirror the REAL physical Headphone Volume pot's live position
    // (a real hardware finding, not a placement bug — confirmed by writing
    // an arbitrary value, then re-requesting a fresh dump: it read back
    // whatever the physical knob was ACTUALLY sitting at, 16380/near-max,
    // regardless of what had just been sent). An earlier "confirmed both
    // read and write" claim for this same dial (see
    // [[project_voyager_sysex_pdf_gap_audit]] in the assistant's own memory
    // notes) turned out to be a false positive — that test happened to drag
    // to max while the real knob ALSO happened to already be at/near max,
    // so the read-back looked like confirmation of a write that never
    // actually did anything. Default false (interactive) — every other
    // dump-only dial in this app (Filter A/B Pole Select, the PGM
    // wheel/pedal/shaping menus, etc.) genuinely IS a stored firmware
    // setting, not a live analog readout, so this should stay rare.
    bool readOnly;

    // "asDial" in the file — forces panel_dial_needs_value_menu() to false
    // for a names= dial that would otherwise qualify (>2 positions, no CC,
    // has a dumpBitWidth) and default to a click-to-open dropdown button
    // (the 2026-07-08 "a menu-select control reads as a button, not
    // something you'd drag" call — see that function's own comment). Added
    // 2026-07-13 for Voyager's tsGateCtrl: a 65-position named enum (Off,
    // 64-127) that owner explicitly wanted to keep behaving like every
    // other continuous CC dial (drag a knob) rather than open a 65-row
    // dropdown, once the plain `display=raw` version's unlabelled 0-63 dead
    // zone got fixed by switching to names=+offset= — the earlier
    // button-only rule assumed every >2-name dump-only dial was a discrete
    // "pick one of these labelled things" selector (true for every other
    // one so far — destinations, sources, categories), not a numeric range
    // that just happens to render itself with named steps. Rendering
    // (synthGraphics.cpp) and click routing (mouseHandle.c's
    // arm_dial_press()) both key off panel_dial_needs_value_menu() alone,
    // so this one flag automatically fixes both without a separate check in
    // each. Default false — every other names= dial keeps today's
    // dropdown-button behaviour unchanged.
    bool asDial;

    // "asMenu" in the file — the opposite pull from asDial above: forces
    // panel_dial_needs_value_menu() to TRUE for a names= dial that wouldn't
    // otherwise qualify because it only has 2 positions (panel_dial_
    // is_binary()'s territory, normally a click-to-cycle button or, for a
    // literal Off/On pair, panel_dial_is_toggle()'s label+green/grey
    // styling). Added 2026-07-13 for the Z1's Porta on/off, Porta Mode,
    // Unison SW/Mode, F2 Link, and LFO 1-4 MIDI Sync — owner wanted these to
    // read their section's own colour (dial->colour) the same way every
    // 3+-position value-menu dial already does, rather than a flat grey (or,
    // for the Off/On ones, green-when-on) that ignores it. synthGraphics.cpp
    // ANDs isToggle with !asMenu before using it, so an asMenu dial falls
    // straight into the existing >2-name value-menu code path (colour, text,
    // and — via arm_dial_press() in mouseHandle.c, which already checks
    // panel_dial_needs_value_menu() before panel_dial_is_binary() — click-to-
    // open-dropdown routing) with no separate special-casing needed anywhere.
    // Default false — every other 2-name dial keeps today's behaviour.
    bool asMenu;

    // "hiLoOffset="/"hiLoCoarseScale="/"hiLoFineScale=" — only meaningful
    // when display == dialDisplaySignedHiLo (see that enum value's own long
    // comment for the derivation). The raw dump value read via the normal
    // dumpOffset/dumpBitWidth bitfield extraction is interpreted as:
    //   signed_raw = (raw >= 2^(dumpBitWidth-1)) ? raw - 2^dumpBitWidth : raw
    //   adjusted   = signed_raw - hiLoOffset
    //   HIGH       = coarse component of adjusted (see synth_decode_hilo() —
    //                floor(adjusted/hiLoCoarseScale), then a boundary
    //                adjustment so LOW always lands in its own signed range)
    //   LOW        = fine component, same signed range as HIGH
    // All three required (0 is not a meaningful default for any of them —
    // hiLoCoarseScale=0/hiLoFineScale=0 would divide by zero) whenever this
    // display mode is used; the parser logs an error rather than silently
    // defaulting if any is missing.
    int32_t  hiLoOffset;
    uint32_t hiLoCoarseScale;
    uint32_t hiLoFineScale;
} tPanelDial;

typedef struct {
    char   page[PANEL_ID_LEN];
    char   section[PANEL_ID_LEN];
    double dialSize;
    double spacing;
    bool   hidden;               // true: not a rendered control/page-tab target, just named
                                 // device state (e.g. program category, voice mode, unison) —
                                 // shown as plain "label: value" text instead of a dial widget.
                                 // Still parsed/bound/dump-scanned exactly like any other section.
    tPanelColour colours[PANEL_MAX_COLOURS];
    uint32_t     colourCount;
    tPanelDial   dials[PANEL_MAX_DIALS];
    uint32_t     dialCount;
} tPanelSection;

typedef struct {
    char deviceName[PANEL_LABEL_LEN];
    char description[128]; // one-line summary from the file's "description"
                           // directive — shown in the startup device chooser
                           // (see scan_panel_configs()); empty if the file
                           // doesn't declare one.
    // 1 byte for a classic manufacturer ID (e.g. Korg 0x42), or 3 bytes for an
    // "extended" ID (e.g. likely Novation — companies registered after
    // single-byte IDs ran out; MIDI spec signals this with a leading 0x00).
    // manufacturerIdLen is always 1 or 3, set by how many values the file's
    // "manufacturerId" line gives.
    uint8_t  manufacturerId[3];
    uint32_t manufacturerIdLen;
    uint32_t familyId;
    uint32_t memberId;
    uint32_t progNameLen;                     // count of leading dump bytes that are program-name ASCII chars
                                              // (also param IDs 1..progNameLen on the live parameter-change path)
    char     scrollDialId[PANEL_ID_LEN];      // dial id (found in any section) that plain mouse-wheel
    // scroll nudges when nothing is being dragged; empty = no shortcut

    // Some hardware (e.g. Moog's Minitaur/Voyager) never answers a Universal
    // Device Inquiry at all — confirmed by capturing the vendor's own editor,
    // which talks proprietary SysEx from the first message with no identity
    // handshake step. For such a device, set "identityQuery no" in the file:
    // this skips the inquiry/wait/reply cycle entirely rather than polling
    // and timing out. Defaults to true (load_panel_config() sets it right
    // after the zeroing memset), so existing files that never mention it are
    // unaffected. "midiChannel" (1-indexed, as written by a human) is then
    // the only way to know which channel to talk on, since there's no
    // identity reply to read gDevice.id from; ignored when identityQuery is
    // true, where the reply's own channel byte is authoritative instead.
    bool     supportsIdentity;
    uint32_t midiChannel;                     // 1-indexed; only meaningful when !supportsIdentity
    // With no identity reply, there's also nothing to correlate the right MIDI
    // destination from — the port a device without identity support sits on
    // (e.g. "Elektron TM-1") isn't necessarily whatever CoreMIDI happens to
    // enumerate first (e.g. an unrelated "IAC Driver Bus 1"). "midiPort <name
    // substring>" names it explicitly; connect_without_identity() in
    // midiComms.c matches it case-insensitively against each destination's
    // display name. Empty (the default) falls back to the first destination
    // found, same as before this existed.
    //
    // Also honoured for an identity-capable device (process_identity_replies(),
    // midiComms.c), where it solves a related but different problem: the
    // normal path there (find_dest_for_source()) infers the send destination
    // from whichever source the identity REPLY came back on, assuming both
    // directions share one physical interface. That breaks for a real setup
    // this app's owner runs — sending out one interface (e.g. an Elektron
    // TM-1) while the synth's own MIDI OUT returns through an entirely
    // different, unrelated-by-name box (e.g. a Cirklon) — where nothing
    // about the reply's source resembles the actual send port's name/entity,
    // so inference finds no match at all and the app never finishes
    // connecting. Setting midiPortName pins the send side explicitly
    // regardless of which source the identity/CC/SysEx traffic actually
    // arrives on (that side was already source-agnostic where it matters —
    // dispatch_sysex() has no source filter, and gMidiSource just tracks
    // whichever source the identity reply came back on for the CC/PC gate).
    // Found 2026-07-14.
    char midiPortName[64];

    // Optional SysEx sent right after connecting (see connect_without_identity()
    // in midiComms.c) — asks the device to report its own current state instead
    // of leaving every dial showing a stale/default value until physically
    // touched. Device-specific (e.g. Moog's own "dump current CC values"
    // command); set via the file's "stateRequestSysEx <hex> <hex> ..." line.
    // 0 length (the default) means don't send anything, unchanged from before
    // this existed.
    uint8_t  stateRequestSysEx[32];
    uint32_t stateRequestSysExLen;

    // Moog's own dump SysEx has a completely different header shape from the
    // Korg-style one is_synth_sysex()/synth_handle_message() assume by
    // default (F0 <mfrId> <0x30|channel> <familyId> <func> ...): it's
    // F0 <mfrId> <productId> <deviceId> <mode> ... instead — see
    // "Voyager System Exclusive Panel Dump Format" (lintronics.de). Set via
    // "dumpFormat moog" + "productId <hex>" in the file; false/0 (the
    // default) leaves every existing Korg-style device (Z1) unaffected.
    // productId is Moog's own proprietary header byte (e.g. 0x01 Voyager,
    // 0x08 Minitaur) — distinct from familyId/memberId (the Universal
    // Identity Reply scheme), which is moot anyway for a device with
    // identityQuery no.
    bool    moogStyleDump;
    uint8_t productId;

    // Every Korg-style device (moogStyleDump == false) used to be assumed
    // to speak the SAME Z1-shaped protocol this codebase originally built
    // for: Program Data Dump Request/Reply (func 0x1C/0x4C), swept across
    // 128 slots for the Load/Store Patch to Bank picker and Backup/Restore
    // Bank (Individual Files) — see synthBackup.c's own Korg name-sweep
    // section header comment. True for Z1; NOT true for every "Korg-style"
    // device in general — found 2026-07-14 connecting a real Kronos (an
    // entirely different, much richer object-based SysEx protocol, see
    // KRONOS_MIDI_SysEx.txt) under kronos.txt: the app immediately started
    // sweeping it with Z1's own Program Data Dump Request, forever, with
    // zero replies, since Kronos doesn't speak that specific protocol at
    // all. Defaults to true (set in load_panel_config(), same pattern as
    // supportsIdentity) so Z1 and any future plain-Korg-shaped device are
    // unaffected; kronos.txt sets "supportsKorgProgramDump no" to opt out
    // of the whole Z1-shaped sweep/backup/restore/Load-Store mechanism
    // until (if ever) Kronos gets its own, correctly-shaped equivalent.
    bool    supportsKorgProgramDump;

    // Where a name field lives in each of the two Moog-style dump replies
    // (moogStyleDump devices only — see extract_moog_name() in
    // synthComms.c). Same continuous 7-bit-per-byte bitstream
    // dumpOffset/dumpBitOffset/dumpBitWidth already use for the numeric
    // panel fields — the Offset/BitOffset pair below is just that scheme's
    // byte/bit position for the name's first (of Len) 8-bit characters, not
    // a separate encoding. Two separate fields, not one shared offset,
    // because the two dump types' name fields don't live at the same
    // position — Single Preset Dump's is 1 byte later than Panel Dump's,
    // presumably for a preset-number byte Panel Dump has no reason to carry.
    // Reverse-engineered from real Voyager captures (2026-07-07): Panel Dump
    // from whatever the currently-loaded patch was ("FROM A DISTANCE"),
    // Single Preset Dump from preset 1 ("FILTER BUBBLES") — not from any
    // published spec. Both offsets default to -1 ("no name field declared"),
    // set in load_panel_config(), same convention as each dial's own
    // dumpOffset default.
    int32_t  panelNameOffset;
    uint32_t panelNameBitOffset;
    uint32_t panelNameLen;
    int32_t  presetNameOffset;
    uint32_t presetNameBitOffset;
    uint32_t presetNameLen;

    // How many characters of the name field above make up one line of the
    // device's own display, if it has a multi-line one (0 = no forced break
    // — the whole field is one line, the default for every device that
    // doesn't set this). extract_moog_name() (synthComms.c) inserts a '\n'
    // in gDevice.progName every nameLineWidth characters so the on-screen
    // "Program name" row (synth_render() in synthGraphics.cpp) can show the
    // same line breaks the real hardware does, rather than running both
    // lines together. Needed because the line boundary itself carries no
    // reliable marker in the raw data — see presetNameLen's comment in
    // voyager.txt for why a byte-value-based guess (e.g. "insert a break
    // wherever there's already whitespace") isn't enough: "Floating Mod" /
    // "Steel Guitar" fills both 12-char lines exactly, with no whitespace at
    // the boundary at all.
    uint32_t nameLineWidth;

    // How many banks of presets the connected unit has — 1 for a base
    // Voyager, more for one with a memory expansion (VX-352 or similar; per
    // the Voyager manual, not confirmed against real hardware since nobody
    // testing this app owns an expanded unit). Documentation only right
    // now, not wired into anything: Backup > Bank
    // (synth_request_all_presets_dump() in synthComms.c, mode 0x04) has no
    // known way to select a non-default bank — the "Voyager System
    // Exclusive Panel Dump Format" doc this app's Moog SysEx handling is
    // otherwise built from doesn't cover multi-bank addressing, and mode
    // 0x04 itself is unconfirmed even for the single-bank case. Once a real
    // Bank backup file's size/structure can be inspected (see
    // synth_backup_bank() in synthBackup.h), there may be a DI-no variant, a
    // bank-select byte, or a second SysEx mode this field ends up feeding —
    // deliberately not guessed at here.
    uint32_t presetBankCount;

    // Pitch (in px) between adjacent grid cells for any dial using col=/row=
    // (see tPanelDial.gridCol above) — "gridColWidth"/"gridRowHeight" in the
    // file. 0 (the default) means "not configured"; synth_render()
    // (synthGraphics.cpp) falls a grid dial back to ordinary auto-flow
    // rather than stacking every grid dial at the same spot if a device
    // declares col=/row= but forgets to set these. One pitch for the whole
    // device, not per-page or per-section — every grid-using page is
    // expected to want the same column/row rhythm.
    double        gridColWidth;
    double        gridRowHeight;
    tPanelSection sections[PANEL_MAX_SECTIONS];
    uint32_t      sectionCount;
    tPanelList    lists[PANEL_MAX_LISTS];
    uint32_t      listCount;
    tColumnLabel  columnLabels[PANEL_MAX_COLUMN_LABELS];
    uint32_t      columnLabelCount;
} tPanelConfig;

// Parses the file at `path` into `config` (which is zeroed first). Malformed
// lines are logged via LOG_ERROR and skipped rather than aborting the parse.
// Returns false only if the file couldn't be opened.
bool load_panel_config(const char * path, tPanelConfig * config);

// Computes each dial's `rect` in `section`. A dial with gridCol >= 0 (and
// both grid pitches > 0) is placed directly at
// origin + (gridCol*gridColWidth, gridRow*gridRowHeight) — every grid dial
// on a page shares the same `origin`, which is what turns per-dial col/row
// into a single page-wide grid rather than one grid per section. Every
// other dial flows left to right from `origin` using the section's
// dialSize/spacing and its own gapBefore, exactly as before col=/row=
// existed.
void layout_panel_section(tPanelSection * section, tRectangle origin, double gridColWidth, double gridRowHeight);

// True for a dial whose names= is exactly {"Off","On"} — a genuine on/off
// dial, as opposed to any other 2-position selector (Filters' Mode "Dual
// LP"/"HP/LP", Osc 3's Freq Range "Lo"/"Hi", ...). Shared by
// synthGraphics.cpp (renders these as a power button, not a knob — see
// draw_power_button() in SynthLib) and mouseHandle.c (a single click
// toggles one of these outright, rather than needing the drag gesture
// every other dial uses) so the two stay in lockstep — a dial that LOOKS
// like a button should also BEHAVE like one.
bool panel_dial_is_toggle(const tPanelDial * dial);

// True for ANY 2-position named dial — panel_dial_is_toggle()'s Off/On
// case included, plus 2-way selectors that aren't semantically on/off
// (Filters' Mode "Dual LP"/"HP/LP", Osc 3's Freq Range "Lo"/"Hi", Env Gate
// "Keyb"/"On/Ext"). All of these are still a single click-to-flip button
// (mouseHandle.c) — only the on/off ones get the green/grey highlight;
// the rest render as a plain button showing the current state's name (same
// idea as G2-Edit's keyboard-tracking "KB" button), since colouring, say,
// Filters' Mode green for "HP/LP" would imply an on/off meaning it doesn't
// have.
bool panel_dial_is_binary(const tPanelDial * dial);

// A discrete selector (>2 positions) with no CC at all — the only way to set
// it is patching its bits into a freshly-fetched Moog dump and resending the
// whole thing (synth_apply_pending_dump_patches() in synthComms.c), which
// should happen exactly once with the FINAL chosen value, not once per
// intermediate step a drag gesture would pass through. mouseHandle.c uses
// this to open a value-picker menu (menus.c) instead of starting a drag —
// added 2026-07-08 for Voyager's Filter A/B Pole Select, the first dials of
// this kind (see fltAPole/fltBPole's own comment in voyager.txt). Broadened
// 2026-07-13 to also cover a param=-wired dial with no dump field at all
// (e.g. the Z1's voiceMode/unisonType) — see this function's own comment in
// panelConfig.c for why dumpBitWidth alone wasn't a complete "has real
// protocol wiring" test.
bool panel_dial_needs_value_menu(const tPanelDial * dial);

// See disabledUnlessDialId/disabledUnlessValue's own comment above — false
// (always enabled) if disabledUnlessDialId is empty, or if the named dial
// can't be found. Takes config explicitly (rather than reaching for a
// global) since panelConfig.c has no dependency on synthGraphics.h — same
// reasoning find_panel_dial_anywhere() below already follows; callers pass
// synth_panel_config().
bool panel_dial_is_disabled(const tPanelDial * dial, tPanelConfig * config);

tPanelSection * find_panel_section(tPanelConfig * config, const char * page, const char * section);
tPanelDial * find_panel_dial(tPanelSection * section, const char * id);

// Same as find_panel_dial(), but searches every section in the config rather
// than one already-known section — for generic code (an info-text row, a
// scroll-shortcut) that only has a dial id and no reason to know which
// section it lives in.
tPanelDial * find_panel_dial_anywhere(tPanelConfig * config, const char * id);

// Looks up the dial wired to a given SysEx parameter group/ID (see the
// "group="/"param=" file attributes), or NULL if none matches.
tPanelDial * find_panel_dial_by_param(tPanelSection * section, uint32_t group, uint32_t paramId);

// Looks up the dial wired to a given MIDI CC number (see the "cc=" file
// attribute) across every section in the config, or NULL if none matches —
// for real-time CC dispatch, which (unlike a SysEx parameter change) carries
// no section/group context to narrow the search.
tPanelDial * find_panel_dial_by_cc(tPanelConfig * config, uint8_t cc);

// Looks up a dial by its display LABEL (case-insensitive), across every
// section — for generic code that needs to find a dial by what it MEANS
// rather than by its short internal id, because different device families'
// own layout files give the same concept different ids: the Z1's Category
// dial is `id=category`, the Voyager's is `id=soundCategory`, but both set
// `label="Category"`. Added 2026-07-14 for synth_decode_korg_category()/
// synth_decode_moog_category() (synthComms.h) so the Load/Store Patch from
// Bank picker's category column works generically across device families
// with zero per-device C code, matching this whole file's own "nothing
// device-specific lives here" philosophy. Returns NULL if no dial's label
// matches (a device with no category concept at all, say).
tPanelDial * find_panel_dial_by_label(tPanelConfig * config, const char * label);

#define PANEL_MAX_CANDIDATES    16

// One <device>.txt found by scan_panel_configs() — just enough to present a
// choice, not a full parsed config (that only happens for whichever one gets
// picked).
typedef struct {
    char filename[64];         // e.g. "z1.txt" — relative to the scanned dir
    char deviceName[PANEL_LABEL_LEN];
    char description[128];
} tPanelConfigCandidate;

// Scans `dir` for every "*.txt" file, fully parsing each (they're small — no
// separate lightweight-header-only parser) into a scratch config just to
// pull out its device/description, and returns how many were found (capped
// at maxCandidates). Used to build a startup device chooser when more than
// one config is present; a single match needs no chooser at all.
uint32_t scan_panel_configs(const char * dir, tPanelConfigCandidate * outCandidates, uint32_t maxCandidates);

// Returns the index into section->dials[] under `point`, or -1 if none.
// Call after layout_panel_section() has populated the dials' rects.
int32_t hit_test_panel_section(tPanelSection * section, tCoord point);

// Display-space value (0..max-1) — pure arithmetic on the dial's own stored
// value, no protocol knowledge.
uint32_t get_panel_dial_value(const tPanelDial * dial);
uint32_t get_panel_dial_native_value(const tPanelDial * dial);

// Looks up item `index` in the named list `listName` (device-wide, not
// section-scoped). Returns "?" if the list or index doesn't exist, rather
// than requiring every call site to bounds-check.
const char * get_panel_list_item(const tPanelConfig * config, const char * listName, uint32_t index);

// Number of items in the named list, or 0 if it doesn't exist — e.g. for
// validating/clamping a value parsed off the wire against the list it names.
uint32_t get_panel_list_count(const tPanelConfig * config, const char * listName);

#ifdef __cplusplus
}
#endif

#endif // __PANEL_CONFIG_H__
