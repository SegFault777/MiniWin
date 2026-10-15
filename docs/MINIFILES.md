# MiniFiles

*`MINIFILES.MWP`, added in 1.1-alpha-1. Implementation: `kernel/apps/minifiles.h` (state, geometry, drawing, clicks);
window plumbing in `window.h`, `windows.h`, `icons.h`, `startmenu.h`, `taskbar.h`, `compose.h`, `input.h`.*

MiniFiles is MiniWin's file manager. Open it by **double-clicking the folder icon** at the end of the desktop's icon row
(labelled `MINIFILES`) or from the **start menu** (`MiniFiles.mwp`). It is an ordinary window: drag it by the title bar,
resize it by its edges, minimise (to the task bar), maximise, close.

It has two tabs.

## FILES

Lists everything MiniWin keeps on its disk, one row per item:

| column | meaning |
|---|---|
| icon + name | a document icon and `NEWDOC.TXT` ... (the four Notepad document slots), or a program icon and the program's name (`GREETER.MWP`); names are cut to 12 characters in the column |
| size | bytes |
| `DOC` / `PRG` | a document or a loadable program |
| remark | empty, or `DAMAGED` |

**DAMAGED is real**: every file is read through completely and its CRC-32 checked when the list is built
(`fs_verify_slot()` / `prog_verify_slot()`, see [DISK-LAYOUT.md](DISK-LAYOUT.md)), so a file that would open as garbage is
flagged instead of looking fine. Files saved before CRCs existed (no commit record) are accepted if every sector reads.

Buttons (the first changes with what is selected):

* **OPEN** (a document) opens it in Notepad. If it is already open in a Notepad window that window is brought to the front
  instead (a second editable copy would race on Save). A damaged document is refused.
* **RUN** (a program) loads and runs it, the same as the Terminal's `run`; the screen is repainted when it returns.
* **DELETE** erases the selected file. It asks first: the first click turns the button into **SURE?** and the status line
  says *Click DELETE again: NAME*; the second click deletes. Clicking anywhere else, or selecting another row, cancels.
  A document that is open in Notepad cannot be deleted (*It is open in Notepad*).
* **REFRESH** rereads the disk (the list is built when the window opens, when you switch tabs and after your own actions,
  not continuously: a file saved in Notepad shows up after REFRESH).

## DISKS

Lists the IDE disks and their partitions, with the **id** each is called by in `os-inst`
([STORAGE-IDS.md](STORAGE-IDS.md)):

```
[disk icon] ide100     1 MB   DISK  RUNNING     <- the disk MiniWin runs from
[disk icon] ide200    16 MB   DISK
   [partition icon] ide201     2 MB   PART
   [partition icon] ide202     4 MB   PART
```

(the icons are the catalog's `IDE_Drive` for disks and `Drive` for partitions)

Remarks: `RUNNING` (MiniWin is running from there, not installable), `EXTENDED` (a container, pick a logical partition inside
it), `TOO SMALL` (under 1 MB).

**INSTALL** does *not* install by itself. It opens the Terminal with `os-inst <id>` already typed in; you press Enter,
read the summary, and type `YES` ([OS-INSTALL.md](OS-INSTALL.md)). A row that cannot be installed on says why in the
status line (*That is where MiniWin runs from.*, *Pick a logical partition inside it.*, *Too small: needs at least 1 MB.*).

## Mouse and keyboard

| | |
|---|---|
| click a row | select it |
| double-click a row (FILES) | OPEN / RUN |
| click a tab | switch tab (Left/Right arrows too) |
| Up / Down | move the selection (the list scrolls to keep it visible) |
| Enter | the primary button: OPEN / RUN / INSTALL |

## Limits (alpha)

* The Notepad has four document slots and there are four program slots, so there are at most eight files; there are no
  folders, copy, move or rename yet ([ROADMAP.md](ROADMAP.md)).
* The window's labels and messages are English only (the rest of the system is bilingual; the task bar and status line
  entries for the window are translated).
* Only IDE disks are listed (the only storage driver this build has).
