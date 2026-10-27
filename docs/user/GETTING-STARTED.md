# Getting started with MiniWin

MiniWin is a small 32-bit operating system for PCs. It starts from a disk image, shows a 640x480 desktop with windows, a start menu,
a text editor, a file manager, a terminal and a tiny web browser, and keeps your documents on its own disk. It is a hobby/teaching
OS written from scratch -- no Linux or Windows code underneath -- so it is meant for virtual machines and experiments, not for
your daily work.

## What you need

* A computer or **virtual machine with a legacy BIOS** ("BIOS" or "Legacy" boot, **not UEFI**).
* About **64 MB of RAM** (the system refuses to start below roughly 9.2 MB).
* To **build the disk image yourself** (there are no prebuilt downloads): a Linux shell with `nasm`, `gcc` that can build 32-bit
  code (on Debian/Ubuntu: `gcc-multilib`), `python3` with Pillow (`python3-pil`), and for trying it out `qemu-system-x86`.

## Build and run it (the quick way)

```sh
git clone https://github.com/SegFault777/MiniWin.git
cd MiniWin
./build.sh                  # builds the kernel and the 1 MB disk image: build/os-image.img
tools/install_all.sh        # puts the icon pack and the demo programs (GREETER and the SDK examples) on it (always run this after build.sh)
                            # add --no-demos (or MW_NO_DEMOS=1) to leave the demos out: the icons only, /progs empty
qemu-system-i386 -drive format=raw,file=build/os-image.img
```

`install_all.sh` also writes `build/os-image-floppy.img`: the same system padded to a 1.44 MB **floppy** image, for virtual machines
that want a floppy. To give the guest network access add `-nic user,model=e1000`.

## Other virtual machines (VMware, VirtualBox)

The image is a raw disk. Convert it, attach it as a hard disk (or use the floppy image as a floppy), pick the legacy BIOS and boot:
see [VIRTUAL-MACHINES.md](VIRTUAL-MACHINES.md) for the exact commands, the supported disk controllers and network cards. If the screen
shows a message starting with `MiniWin ERROR MW-...`, [TROUBLESHOOTING.md](TROUBLESHOOTING.md) explains each one in plain words.

## Your first five minutes

1. After a few seconds the desktop appears: a column of icons on the left, a taskbar at the bottom.
2. **Double-click NOTEPAD**, type something, press **Ctrl+S**. The document is saved on the disk image and a new icon for it appears
   on the desktop. (File > Save As lets you choose its name.)
3. **Double-click MINISHELL** and type `fetch` and Enter: it lists what MiniWin found about the machine (CPU, graphics, memory, disk,
   network). Type `?` for all commands.
4. Open the **start menu** (bottom-left) for the apps and for Shut Down / Restart. Shut Down really powers a virtual machine off.
5. **Double-click MINIFILES** to see your documents, programs and disks.

Everything you saved is stored in the disk image file: keep a copy of `build/os-image.img` to keep your documents, and note that
running `tools/install_all.sh` or `./build.sh` again starts from a fresh image.

## Where to go next

* [DESKTOP-AND-APPS.md](DESKTOP-AND-APPS.md) -- the desktop, windows and every app
* [TERMINAL.md](TERMINAL.md) -- all MiniShell commands
* [FILES-AND-DISKS.md](FILES-AND-DISKS.md) -- documents, programs, disks, installing MiniWin onto another disk
* [FAQ.md](FAQ.md) -- short answers (real hardware? USB? 64-bit?)
