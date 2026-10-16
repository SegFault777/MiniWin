# Developer guide

## Orientation

* One translation unit: `kernel/kernel.c` includes every module in dependency order (see its header comment and
  [ARCHITECTURE.md](ARCHITECTURE.md)); there is no linker-level modularity, and **no heap**. Everything is a fixed-size
  static table, which is why the checks for "table is full" exist and why they matter.
* `build.sh` produces `build/os-image.img` (1 MB, the layout in [DISK-LAYOUT.md](DISK-LAYOUT.md)); `tools/install_all.sh`
  adds the sample program and the icon catalog. QEMU: `qemu-system-i386 -drive format=raw,file=build/os-image.img`.
* Budgets that bite (all checked by `build.sh`): the **kernel image** has 512 KB (1024 sectors, 524,288 bytes) and
  image + `.bss` + stack must end below `0x9FC00`. At 1.1-alpha-2 about **187 KB of image and 186 KB of `.bss`** remain (the Korean font is stored compressed); see
  [ROADMAP.md](ROADMAP.md) for the history of that budget. Check `build.sh`'s last lines before and after a feature.
* The Terminal is **40 columns wide** and shows 14 rows (Up/Down scroll the history); every `term_print()` string must fit
  40 columns (it truncates silently).
* Warnings are tracked: the build currently has 8 (2 come from the linker, the rest are old). A change should not add one.

## Adding a window application (the checklist)

There is no application framework: each app is hand-wired into the same places. MiniFiles is the most recent example
(`git show` the commit that added it); Terminal is the template. For an app `Foo` with window id `WIN_ID_FOO`:

| file | what to add |
|---|---|
| `kernel/ui/window.h` | `foo_win`, a `window_t` with its default geometry (copy `term_win`) |
| `kernel/apps/notepad/windows.h` | `WIN_ID_FOO`, `WIN_ID_COUNT + 1`, and `foo` in `win_is_open()`, `win_is_minimized()` and `win_ptr()` |
| `kernel/apps/foo.h` | the app: state, hit-test functions for its title-bar buttons, `draw_foo_window()`, click/key handlers |
| `kernel/kernel.c` | `#include "apps/foo.h"` (after anything it calls, before `ui/input.h`) |
| `kernel/ui/icons.h`, `kernel/memmap.h` | a desktop icon: a catalog name in the icon-cache list, `ICONC_*` index and count, `ICONn_X/Y` and `draw_desktop_iconN()`; grow `MW_ICON_CACHE_SIZE` (4 KB per icon). The icon must exist in the catalog: `tools/install_icons.py`, art in `third_party/icon-bundle/` |
| `kernel/ui/compose.h` | call the icon's draw function; add the window to the draw dispatch |
| `kernel/ui/taskbar.h` | the short taskbar label for the window |
| `kernel/ui/startmenu.h` | a menu item (item count, height and the label table) and `kernel/ui/input.h`'s start-menu click handler |
| `kernel/ui/input.h` | (1) taskbar restore status text, (2) taskbar close, (3) window click dispatch (buttons, drag, content), (4) desktop icon double-click, (5) title-bar button release (`still_over` and the min/max/close actions), (6) keyboard routing |
| `kernel/sys/lang.h` | status strings (`STR_FOO_OPENED`, `..._MINIMIZED`, `..._MAXIMIZED`, `..._RESTORED`) in **both** language tables |
| `tools/test/golden*` | re-record the UI goldens (the desktop and the start menu changed) and add a tour for the app |

Window geometry, dragging, maximise/restore and edge resizing are generic (`win_ptr()`), so the app only supplies its own
content. Draw with the `ui_*` widgets in `kernel/ui/widgets.h` and the `TH_*` colours from `theme.h` so the dark theme
works for free.

## Conventions

* New source files begin with `SPDX-License-Identifier: GPL-3.0-only` ([LICENSING.md](LICENSING.md)).
* Comments explain *why* (and what used to go wrong); the existing code is the style reference.
* Anything that talks to hardware or parses data from outside gets a bounded loop, a size check and a defined failure
  result, and a test that injects the failure. "It can't happen" has been wrong in this code base more than once.
* Never write to a disk from a path that has not first proved its target safe (`st_resolve()`), and write the sector that
  makes something bootable *last*.
* Commit messages: a title `MiniWin <version>`, then what changed, why, and a verification section that says what was
  *not* covered.
