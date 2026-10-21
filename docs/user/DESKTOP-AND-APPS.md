# The desktop and the apps

## The desktop

The desktop is a single list of icons at even spacing, running **top to bottom** on the left. When the next icon would not fit above
the taskbar, the list continues at the **top of the next column to the right**. In order:

1. the five apps: **NOTEPAD, SETTING, WEB, TERMINAL, MINIFILES**;
2. one icon for every **saved document** (a page icon, named after the document);
3. one icon for every **installed program** (a "program" icon).

**Double-click** an icon to open it: an app or document opens a window, a program runs. Long names are shortened to seven letters and
a `~`; the part after the dot is shown on a second line.

At the very bottom: a **status line** (hints and the result of what you just did) and the **taskbar** with the start button, one
button per open window and the clock.

## Windows

* **Move** a window by dragging its title bar; **resize** by dragging an edge or corner (the pointer changes to a double arrow).
* The three buttons at the right of the title bar **minimise**, **maximise/restore** and **close**. A minimised window stays on the
  taskbar; click its taskbar button to bring it back.
* Click anywhere on a window to bring it to the front.

## The start menu

Click the button at the bottom left. It lists the apps and a **Shut Down / Restart** pair. *Restart* reboots the machine; *Shut Down*
shows the classic "safe to turn off" screen and then powers the machine off where the firmware allows it (virtual machines and
ordinary PCs do; otherwise the screen simply stays).

## Notepad

A plain-text editor; you can have several windows open at once.

* **Typing:** the keyboard works as expected. **Right Alt** switches to the next enabled input method (English / Korean Hangul).
* **File menu:** *Save As* (choose a name), *Save* (keep the document's own name), *New* (empty page; asks before throwing away
  unsaved text). Shortcuts: **Ctrl+S** save, **Ctrl+N** new, **Ctrl+W** close the window.
* **Save As:** a small dialog asks for the file name (1-31 letters, digits, `.`, `_`, `-`). Enter or *Save* saves a copy under the
  new name and leaves the original alone; *Cancel* or **Esc** changes nothing. A name that another document already has is refused.
* **Limits:** four documents can be stored at the same time (a limit of the current disk layout, planned to go away); each up to
  4 KB of text. If all four are used, saving says so instead of overwriting anything.
* Saved documents get a desktop icon; double-click it to open the document again.

## Setting

System settings in three pages: **Language** (menus and messages in English or Korean), **IME** (which input methods Right Alt
cycles through) and **Time Zone** (for the clock and the `tick` command).

## Web (MiniWeb)

A small web browser: an address bar with a **GO** button, links you can click, and an HTML/CSS renderer that is deliberately
limited (no images, no scripts, no floating or absolutely positioned layout). It needs a network card ([FILES-AND-DISKS.md](FILES-AND-DISKS.md)
lists the supported ones) and a DHCP network such as QEMU's `-nic user`. HTTPS is supported with a built-in list of root certificates.
It is a demonstration of the network stack, not a replacement for a real browser.

## Terminal

A text console with a few commands for documents, programs, system information, installing and power: see [TERMINAL.md](TERMINAL.md).

## MiniFiles

The file manager: your documents and programs, and a **DISKS** tab with every disk and partition MiniWin can see: see
[MINIFILES.md](MINIFILES.md).
