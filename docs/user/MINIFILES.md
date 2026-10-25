# MiniFiles

*`MINIFILES.MWP`, added in 1.1-alpha-1, rebuilt as an explorer in 1.1-alpha-10. Implementation: `kernel/apps/minifiles.h` (state,
geometry, drawing, clicks); window plumbing in `window.h`, `windows.h`, `icons.h`, `startmenu.h`, `taskbar.h`, `compose.h`,
`input.h`; the small icons in `ui/icon16.h`.*

MiniFiles is MiniWin's file manager, laid out like the Windows 95 Explorer. Open it by **double-clicking the folder icon** on the desktop
(labelled `MINIFILES`) or from the **start menu** (`MiniFiles.mwp`). It is an ordinary window: drag it by the title bar, resize it by
its edges, minimise (to the task bar), maximise, close.

```
 File  Edit  View  Tools  Help
 [up][new] [Copy][Cut][Paste] [delete][refresh][disks]
 All Folders          | Contents of /docs
 [-] MiniWin          | Name         Size     Type     Modified
   [+] docs           | DOC.TXT      12 B     Text     26-10-25
   [+] mnt            | NOTES.TXT    1 KB     Text     26-10-24
   [+] progs          |
   [+] sys            |
 2 object(s)                         | 1 KB | Free: 149 KB
```

## The two panes

* **Left, All Folders:** the folder tree of the root file system. `[+]` / `[-]` (or a double-click on a name) opens and closes a
  folder; clicking a name shows its contents on the right. The volumes mounted under `/mnt` show with a disk icon. The tree has its own
  scroll bar and wheel scrolling.
* **Right, Contents of ...:** the entries of the selected folder, folders first. Columns: **Name**, **Size**, **Type** (`Folder`, `Text`,
  `Prog`, `HTML`, `CSS`, `Script`, `C src`, `Asm`, `File`, from the file's ending), **Modified** (`YY-MM-DD`; a dash for a file written
  before alpha-10, or when the clock could not be read). **Click a column header to sort**; click it again to reverse. In a narrow
  window the Type column goes first, then Modified, then Size. Up to 192 entries of one folder are listed.

The **status bar** shows the number of objects, or (with a file selected) its date and time and size; at the right the **free space** of
the volume. Messages ("Deleted.", "That is not a valid name.", ...) replace the object count until you do something else.

## The tool bar and the menus

| button | menu | what it does |
|---|---|---|
| up arrow | | the parent folder |
| folder | File > New Folder | makes `NewFolder` (then `NewFolder2`, ...) in the open folder and starts renaming it |
| Copy / Cut / Paste | Edit | copy a **file** (folders can be moved with Cut, not copied), then open another folder and Paste there; a name that is already used is refused, never overwritten |
| trash | File > Delete | the first click arms it (*Click DELETE again: NAME*), the second deletes; a folder must be empty; the folders the system stands on (`/docs`, `/progs`, `/mnt`, `/sys`, the volumes) cannot be deleted or renamed; a document open in Notepad cannot be deleted or renamed |
| refresh | File > Refresh | rereads the disk (the lists are built when you open a folder and after your own actions, not continuously) |
| computer | View > Disks | switches to the disk list (below); press again for the folders |
| | File > Rename | an inline name box on the selected row: type, **Enter** accepts, **Esc** cancels; a click elsewhere accepts |
| | File > Open | a folder: go into it; a program in `/progs`: run it (the screen is repainted when it returns); a document in `/docs`: open it in Notepad |
| | Tools > MiniShell here | opens MiniShell with `cd <this folder>` typed in (press Enter) |

A file elsewhere than `/docs` and `/progs` cannot be opened from here yet (*Only files in /docs open in Notepad.*, *Programs run from
/progs.*); MiniShell's `cat` and `cp` work on any path. Changes under `/docs` and `/progs` also update the desktop icons.

A file that fails its CRC-32 check when you select it (small files only) is flagged **DAMAGED** and refuses to open
(see [DISK-LAYOUT.md](../dev/DISK-LAYOUT.md)).

## Disks (View > Disks)

Lists the disks and their partitions, with the **id** each is called by in `os-inst`
([STORAGE-IDS.md](../dev/STORAGE-IDS.md)):

```
[disk icon] ide100     1 MB   Disk  RUNNING     <- the disk MiniWin runs from
[disk icon] ide200    16 MB   Disk
   [partition icon] ide201     2 MB   Part
   [partition icon] ide202     4 MB   Part
```

Remarks: `RUNNING` (MiniWin is running from there, not installable), `EXTENDED` (a container, pick a logical partition inside it),
`TOO SMALL` (under 1 MB).

**Install** (the button, or Enter / double-click on a row) does *not* install by itself. It opens MiniShell with `os-inst <id>` already
typed in; you press Enter, read the summary, and type `YES` ([INSTALLING.md](INSTALLING.md)). A row that cannot be installed on says why
in the status line.

## Mouse and keyboard

| | |
|---|---|
| click a row / a tree name | select it / show that folder |
| double-click a row | open it (a folder, a program, a document) |
| double-click a tree name | open or close it |
| Up / Down | move the selection |
| Enter or Right | open the selection |
| Backspace or Left | up one folder |
| wheel | scroll the pane under the pointer |

## Limits (alpha)

* No overwrite or "Copy of" naming on Paste, no copying of folders, no multiple selection, no drag and drop, no undo.
* The column order and the tree width are fixed (the tree takes 30% of the window).
* The window's labels are bilingual; its messages are English only.
