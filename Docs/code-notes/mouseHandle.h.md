# mouseHandle.h notes

The longer comments from `mouseHandle.h`, moved here 2026-09-12 so the code reads cleanly. The code points at each as `// notes §k`. Verbatim and in file order; each is titled by what it documents.

## 1. `panel_dial_press_click_handler()`

Arms a panel dial's press state (gDraggedDial/gPressedToggleDial/
gPressedValueMenuDial, private to mouseHandle.c) via arm_dial_press() —
registered by synthGraphics.cpp's synth_render() as each visible, enabled
dial's click region (both the main per-page grid and the Info Row). Release
is handled entirely by the existing global armed-state check in
handle_mouse_button(), not by this handler — that logic resolves whatever
was pressed regardless of what's under the cursor now, so it isn't a
per-widget dispatch target.

## 2. `page_tab_click_handler()`

Same shape as panel_dial_press_click_handler above, for the three other
press-arms-release-fires widgets — gPressedTab/gPressedPatchNav are private
to mouseHandle.c, same reason these live here rather than in
synthGraphics.cpp alongside the render code that registers them. userData
carries the tab/nav index as a plain integer value (not a pointer
dereference) via (void *)(intptr_t)index — synth_render() doesn't keep a
stable, addressable per-tab/per-button context struct the way a dial does.
