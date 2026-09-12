# panelConfig.c notes

The longer comments from `panelConfig.c`, moved here 2026-09-12 so the code reads cleanly. The code points at each as `// notes §k`. Verbatim and in file order; each is titled by what it documents.

## 1. `PANEL_TOKEN_LEN`

Raised from 64 to 512 (2026-07-10), then 512 to 1024 (2026-07-11) — a
single token can be as long as a whole `names=` list (e.g. Voyager's
pgmShaping1Src/pgmShaping2Src, 43 quoted names each, 804 bytes for the
whole line — the 512 ceiling silently truncated these two lines entirely
on startup, breaking panel load). tokenize() below silently drops
anything past PANEL_TOKEN_LEN-1 with no error at all — that's what let
the earlier 64-byte ceiling go unnoticed: no parse error, just names
beyond a certain point permanently reading "?" in the UI. Matches
PANEL_LINE_LEN since no single token can ever exceed the length of the
line it came from anyway. This is a per-line stack buffer during parsing
only (PANEL_MAX_TOKENS entries, not stored in the persisted config), so
the memory cost of raising it is negligible and one-time per parsed
line, not per dial.

## 2. `tokenize()`

── Line tokenizer ────────────────────────────────────────────────────────────
Splits on whitespace, except inside "..." (which may itself contain spaces,
e.g. label="F1 Cut"). Quotes are stripped from the output. '#' outside
quotes starts a comment and ends tokenizing for the rest of the line.

## 3. in `parse_dial_line()`

"disableUnless=<dialId>:<value>" — see disabledUnlessDialId's
own comment in panelConfig.h. Split on ':' here rather than
reusing split_csv() (that's for a variable-length names= list;
this is always exactly two fixed fields).

## 4. in `process_line()`

Page comes from the section currently open, same as how a dial
line implicitly belongs to whichever page/section is open when
it's parsed — a columnLabel isn't tied to any one section, but
still needs to know which page's columns it's labelling.

## 5. in `load_panel_config()`

A physical line longer than PANEL_LINE_LEN-1 leaves fgets() without
its trailing newline — the remainder would otherwise be read as a
bogus separate "line" (garbage directive) by the next fgets() call.
Detect that and discard the rest of the real line instead, so an
over-long line degrades to "this one line didn't fully load"
rather than corrupting parsing of everything after it.

## 6. `panel_dial_hit_rect()`

WHERE A DIAL CAN ACTUALLY BE CLICKED, which is not always dial->rect.

A dial drawn as a BUTTON is drawn larger than its rect: draw_button() keeps the origin and adds
2 * DRAW_BUTTON_MARGIN to the width and the height, so the button extends four units past its rect
at the bottom and the right. Three places needed to know that and only one of them did — the click
region registered the expanded bounds (synthGraphics.c), while this hit test and the press/release
pair in mouseHandle.c both used the raw rect. The visible result was a dead strip along the bottom
and right edge of every toggle and every value-menu button: the press armed the dial through the
click region, then the release re-tested against the raw rect, failed, and the button did nothing.
You had to click "further into" it.

One function, so the answer cannot differ between the code that draws it, the code that registers
it and the code that decides whether the release counted.

## 7. in `panel_dial_is_toggle()`

Case-insensitive — Voyager's own device file writes "Off"/"On", the
Z1's writes "OFF"/"ON" (matching the Korg manual's own convention).
Was strcmp() (exact-case) until 2026-07-13: silently excluded every
single one of the Z1's 11 genuine on/off dials from the green-when-
on/label-only/no-OFF-ON-text button styling this function exists to
grant — they all rendered as plain 2-name buttons literally showing
"OFF"/"ON" instead, discovered via the owner's own screenshot review.

## 8. in `panel_dial_needs_value_menu()`

dumpBitWidth>0 covers a Moog-style dump-only dial (its original
2026-07-08 use case, e.g. Voyager's Filter A/B Pole Select — see this
function's own header comment). paramId!=0 covers the OLDER Korg-style
path a device like the Z1 uses instead: a real SysEx parameter-change
dial with no CC (ccNumber==0, already required below) and no Moog dump
field either, wired via group=/param= alone. Both are equally "this
dial has real, working protocol wiring, not just a placeholder" —
dumpBitWidth alone missed every Z1 dial with >2 names (voiceMode,
unisonType), silently falling back to knob rendering for them. Z1's
own param table is 1-indexed (Program Name starts at param 1), so
paramId==0 reliably means "no param wiring" here, same as dumpBitWidth
==0 meaning "no dump wiring" already did.

nameCount>2 OR asMenu — asMenu (see its own comment in panelConfig.h)
opts a 2-name dial (otherwise panel_dial_is_binary()'s territory) into
this same value-menu path, e.g. the Z1's Porta on/off wanting its
section colour instead of a flat grey/green.
