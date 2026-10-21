# The desktop and UI framework

How the windowing layer is put together, so a new app or a visual change goes in the right place. Source files are listed with their
own descriptions in [SOURCE-MAP.md](SOURCE-MAP.md).

## The frame

`kmain()` (in `kernel/kernel.c`) loops forever; one pass is:

1. **input** -- `handle_mouse()` and keyboard routing (`ui/input.h`): clicks, drags, menus, icons, window controls, typing;
2. **background work** -- the network stack tick, an open web fetch, a running install;
3. **`render_frame()`** (`ui/compose.h`) -- draw everything into the 640x480 back buffer, back to front, then the pointer, then copy
   the buffer to the screen;
4. a short pacing spin (`delay()`), after which the loop starts again. `sysinfo_account()` measures how much of the pass was work
   and how much pacing, which is what `fetch` shows as CPU load.

There are no events, callbacks or threads: a window is a struct whose fields the input code updates and the drawing code reads.

## Windows

`ui/window.h` defines `Window_t`, the window slots, z-order, maximise/restore and geometry; each app keeps its own state next to it
(`apps/notepad/windows.h`, `apps/web/state.h`, ...). A window is only drawn when open and not minimised; clicking brings it to the front.
Edge and corner resizing is `ui/resize.h` (with the matching cursors in `ui/cursor.h`).

## The look: themes, roles and widgets

Nothing outside the theme files may name a colour. `ui/theme.h` selects one of `ui/themes/{classic,dark}.h` at build time
(`THEME=dark ./build.sh`), and each defines **roles** (`TH_DESKTOP`, `TH_TEXT`, `TH_FIELD`, `TH_CURSOR_OUTLINE`, ...). `ui/widgets.h`
draws every reusable piece of chrome (title bar, buttons, fields, frames) from those roles. The dark theme is partly a diagnostic: anything
that still looks "classic" under it hard-coded a colour. To add a visual element, add a role if you need a new colour, draw it with an
existing widget if one fits, and look at the result under **both** themes.

## The desktop grid and shortcuts

`ui/icons.h` defines the grid: one vertical list of cells at a constant pitch (`ICON_H`), top to bottom from `ICON_Y`, continuing at the
top of the next column (`DESK_COL_W` to the right) when the next cell would end below `DESK_BOTTOM_Y`. Cell *n* is at
`(DESK_CELL_X(n), DESK_CELL_Y(n))`. The first `DESK_BUILTIN_COUNT` (5) cells are the apps; `ui/desktop_files.h` adds a shortcut for each
saved document and each installed program after them (`desk_item()` enumerates them). Hit-testing for double-clicks is in `ui/input.h`.
The existence of documents and programs is **cached** (`desktop_file_exists[]`, `desktop_prog_exists[]`) because asking the disk every frame
would be far too slow -- anything that changes a slot (save, delete) must update the cache.

## Icons and cursors

The 32x32 and 16x16 pictures come from the icon pack, installed into a disk-based **catalog** by `tools/install_icons.py` and read at
boot into small caches (`ui/icons.h`: seven 32x32 icons; MiniFiles keeps four 16x16 ones). The pack has far more icons than the UI
uses today. The mouse cursors are the exception: they are compiled into the kernel from `Images/Cursor/` by `tools/gen_cursors.py`
(`kernel/cursor_data.h`). Both are CC BY-SA 4.0; see [LICENSING.md](../project/LICENSING.md).

## Text and input methods

`font.h` draws the Latin font (11x11 cells), `font_ko.h` the Hangul glyphs (inflated into memory at boot), `hangul_ime.h` composes
syllables as you type; **Right Alt** cycles the active input method. UI strings live in `sys/lang.h` as English/Korean tables indexed by
`STR_*`; add the English and the Korean entry together, or the Korean UI shows an empty string.

## Adding an app

1. A header `kernel/apps/<name>.h` (or a folder) with state, drawing and click handling; include it from `kernel.c` in dependency order.
2. A cell: bump `DESK_BUILTIN_COUNT`, add `ICONn_*` and the draw/hit-test in `ui/icons.h`/`ui/input.h`, an icon in the cache
   (`ICONC_*`, `memmap.h` cache size) and a start-menu entry.
3. Strings in both languages; a golden screenshot step in `tools/test/ui_golden.sh` and a re-recorded golden (see
   [TESTING.md](TESTING.md)).
