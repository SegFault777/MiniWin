# MiniWin

A dependency-free 32-bit x86 desktop OS that boots from a single MBR
disk image straight into a Windows-95-flavored GUI: real mode → protected
mode by hand, a from-scratch VGA/mouse/keyboard/ATA stack, and a windowing
system with no libc, no bootloader framework, and no borrowed kernel code.

![Desktop](screenshots/01-boot-desktop.png)

## What's actually in here

- **Boot**: a 512-byte MBR bootloader (`boot/boot.asm`) that sets a real
  VBE (VESA) video mode via a genuine BIOS call, switches to protected
  mode, and loads the kernel via BIOS INT13h extended (LBA) reads, 512KB
  budget, all real addressing under 1MB so it works with no A20
  shenanigans during load.
- **Display**: 640x400, 256-color linear framebuffer -- VBE mode 0100h,
  set by a real BIOS `INT 10h` call in the bootloader (VBE mode-setting
  can only happen in real mode, before protected mode takes over). This
  went from the original 320x200 mode 13h; every UI element (fonts,
  icons, windows, taskbar) deliberately kept its original absolute pixel
  size, so the desktop now has real breathing room instead of everything
  scaling up to fill the bigger canvas. The kernel reads back wherever
  the BIOS actually put the framebuffer (`PhysBasePtr`) and its real
  scanline pitch, rather than assuming a fixed address the way mode 13h
  allowed.
- **Kernel**: freestanding C (`kernel/kernel.c` + headers), no libc. A
  256-color palette (a 6x6x6 color cube + grayscale ramp on top of the
  classic 16), a PS/2 mouse + keyboard driver, PC speaker beep, a tiny
  4-slot ATA-backed filesystem, PCI enumeration, and a COM1 serial debug
  log.
- **Desktop**: draggable/resizable/minimizable/maximizable windows with
  real overlapping z-order (click a window, it comes to front), edge/
  corner resize handles that swap the cursor to a matching directional
  arrow (horizontal, vertical, or either diagonal) while hovering, a
  Windows-95-style Start Menu with a cascading Shut Down/Restart flyout
  (both genuinely halt/reboot the machine), and a taskbar that lists
  minimized windows in the order you minimized them.
- **Clock**: bottom-right of the taskbar, reading the real CMOS hardware
  clock (`kernel/rtc.h`) -- not a simulated counter. Shows "3:45 PM"
  (English) or "오후 3:45" (Korean, meridiem-first as is conventional);
  click it for a small popup with the full date ("Monday, September 14,
  2026" / "2026년 9월 14일 월요일"). Timezone is a manual UTC offset set
  in SETTING.EXE > SYSTEM > Time Zone -- there's deliberately no
  "automatic by location" here yet, since that would need a DNS
  resolver (to find a geolocation service by name) this kernel doesn't
  have, on top of the HTTP client it now does (see Networking below).
  Honest manual setting now, real automatic detection once DNS exists
  to ask a service by hostname instead of a hardcoded IP.
- **Apps**: NOTEPAD.EXE (supports opening several documents at once, each
  in its own window, saving to real persistent disk storage) and
  SETTING.EXE (System language switch between English/한국어 -- actually
  retranslates the whole UI live -- and a multi-select IME picker that
  controls what Right Alt cycles through while typing).
- **Hangul**: a real IME (2-beolsik-style jamo composition) backed by a
  full modern-Hangul-syllable bitmap font (11,172 glyphs, generated from
  the bundled Dalmoori TTF -- see `tools/gen_hangul_font.py`).

## Building

Needs `nasm`, a 32-bit-capable `gcc` (`gcc-multilib` on Debian/Ubuntu if
you're on a 64-bit host), and `ld`.

```bash
./build.sh
```

Produces `build/os-image.img`, a raw disk image -- exactly 256KB
(262,144 bytes), a deliberately round size rather than arbitrary
padding. `build.sh` derives the kernel's boot budget and the
file-storage layout from the same constants `boot/boot.asm` and
`kernel/fs.h` use (instead of duplicating them as separate magic
numbers), and fails loudly instead of silently shipping something
broken if: the kernel's `.bss` ever grows enough to collide with the
boot stack, the compiled kernel exceeds its boot-loader read budget, or
the file-storage slots would overlap the kernel or overflow the image.

## Running

```bash
qemu-system-i386 -drive file=build/os-image.img,format=raw
```

To actually **hear** the PC-speaker beep (Notepad's unsaved-changes
warning), QEMU needs an audio backend explicitly attached -- it isn't on
by default:

```bash
qemu-system-i386 -drive file=build/os-image.img,format=raw \
  -audiodev pa,id=snd0 -machine pcspk-audiodev=snd0
```

(swap `pa` for `dsound`, `coreaudio`, `sdl`, or whatever backend your
platform's QEMU build supports -- run `qemu-system-i386 -audio-help` to
list them.)

To attach a NIC (for PCI/driver work in progress -- see Roadmap):

```bash
qemu-system-i386 -drive file=build/os-image.img,format=raw \
  -netdev user,id=n0 -device rtl8139,netdev=n0 \
  -serial file:serial.log
```

## Controls

- Mouse: click, drag title bars, click the `_`/`□`/`X` buttons.
- **Right Alt**: cycle input method (only meaningful languages enabled in
  SETTING.EXE > SYSTEM > IME get cycled through).
- **Ctrl+S / Ctrl+N / Ctrl+W** inside Notepad: Save / New / Close.

## Project layout

```
boot/boot.asm       MBR bootloader (real mode -> protected mode, disk load)
kernel/kentry.asm   32-bit entry stub (BSS clear, calls kmain)
kernel/kernel.c     the OS itself: GUI, window manager, Notepad, Settings
kernel/*.h          one subsystem per header (vga, keyboard, mouse, ata,
                    fs, font, font_ko, hangul_ime, speaker, serial, pci, io)
kernel/nic.h        common NIC driver interface (rtl8139.h, e1000.h implement it)
kernel/net.h        shared endianness/checksum helpers for the network stack
kernel/arp.h        ARP cache + request/reply
kernel/ip.h         IPv4 header build/parse, routing, ARP-miss pending queue
kernel/icmp.h       ping (echo request/reply)
kernel/udp.h        UDP send + port listener dispatch
kernel/dhcp.h       DHCP client (DISCOVER/OFFER/REQUEST/ACK)
kernel/tcp.h        TCP (single connection, active open, retransmit timer)
kernel/http.h       HTTP/1.1 GET client on top of tcp.h
kernel/net_stack.h  wires all of the above into one init()/poll() pair
tools/gen_hangul_font.py   generates font_ko_data.h from the Dalmoori TTF
third_party/        bundled font source + its own license/notice
build.sh            nasm + gcc + ld pipeline -> build/os-image.img
screenshots/        yep
```

## Roadmap / known limitations

This is being actively built out. Current honest state of the bigger
asks:

- **Persistence**: real, already working -- saved files live on the ATA
  disk image itself (`kernel/fs.h`), and survive across QEMU runs as long
  as `os-image.img` isn't rebuilt from scratch.
- **Networking**: a real, layered TCP/IP stack, built from raw Ethernet
  all the way up to HTTP, running on top of two independently verified
  NIC drivers sharing one common interface (`kernel/nic.h`):
  - **RTL8139** (`kernel/rtl8139.h`) -- pure port I/O, ring-buffer RX,
    4-slot round-robin TX.
  - **e1000 / 82540EM** (`kernel/e1000.h`) -- memory-mapped registers
    (no port I/O at all) plus real RX/TX descriptor rings the card DMAs
    through on its own. Works because this kernel runs with paging
    disabled, so a physical address and a C pointer are the same
    number -- no page-table plumbing needed to talk to the card's MMIO
    space.

  Whichever chip QEMU (or real hardware) actually presents on the PCI
  bus gets picked up automatically (RTL8139 tried first, e1000 as
  fallback). Both are polled (no interrupts). Above the driver layer,
  one header per protocol (`kernel/net_stack.h` wires them all
  together):
  - **ARP** (`kernel/arp.h`) -- an 8-entry cache, request/reply, and an
    ARP-miss packet queue that auto-flushes the instant a reply lands
    instead of making every caller implement its own retry.
  - **IPv4** (`kernel/ip.h`) -- header build/parse, checksums, and a
    one-line "same subnet or gateway" routing decision (no
    fragmentation -- this kernel never needs to send anything bigger
    than one Ethernet frame).
  - **ICMP** (`kernel/icmp.h`) -- echo request/reply (ping).
  - **UDP** (`kernel/udp.h`) -- checksummed send, port-based listener
    dispatch.
  - **DHCP** (`kernel/dhcp.h`) -- a real client: DISCOVER, OFFER,
    REQUEST, ACK. MiniWin gets its IP, subnet mask, gateway, and DNS
    server from whatever network it's plugged into, instead of a
    hardcoded address that only ever worked inside one specific QEMU
    invocation.
  - **TCP** (`kernel/tcp.h`) -- one connection at a time, active opens
    only, a textbook state machine (SYN_SENT -> ESTABLISHED ->
    FIN_WAIT -> closed), one segment in flight with a retransmit timer.
    No sliding window, no congestion control -- enough TCP to reliably
    fetch a web page, not enough to replace a stack you'd trust with
    anything that matters.
  - **HTTP** (`kernel/http.h`) -- a GET client on top of that TCP, built
    and polled as a small state machine (connect -> send request ->
    drain response -> close) so nothing in this single-threaded kernel
    ever blocks waiting on the network.

  Every layer was verified for real, not just compiled: DHCP against
  QEMU SLIRP's actual DHCP server, TCP's 3-way handshake and HTTP GET
  against a real Internet host (a full HTTP/1.1 response, headers and
  all, from pypi.org, over MiniWin's own from-scratch TCP), logged over
  the serial port (`kernel/serial.h`):
  ```
  [DHCP] -> DISCOVER
  [DHCP] <- OFFER of 10.0.2.15 from server 10.0.2.2
  [DHCP] -> REQUEST for 10.0.2.15
  [DHCP] <- ACK, bound to 10.0.2.15 mask=255.255.255.0 gw=10.0.2.2 dns=10.0.2.3
  [HTTP] GET / from <server ip>
  [TCP] connecting to <server ip>:80
  [TCP] established
  [TCP] peer closed their side
  [HTTP] response complete, 1042 bytes
  [TCP] closing
  ```
  What's still missing: DNS (targets above are raw IPs, not hostnames),
  TLS/HTTPS, and a browser (MiniWeb) to put on top of the HTTP client
  that now exists.
- **File Manager**: not built yet.
- Full HTML4/5/XHTML rendering and "SSE3 support" are not realistic
  targets for a 320x200, 16-/256-color, no-libc kernel like this one --
  if/when MiniWeb happens, it'll be an honestly-scoped local hypertext
  viewer, not a general-purpose browser engine.
- No dynamic program loading/execution exists (everything is compiled
  into the one kernel binary) -- a real "install and run third-party
  .MPI programs" system would need a loader and some kind of process
  model that doesn't exist yet.
- No real internet-backed accounts or third-party (Google/Microsoft)
  sign-in are planned -- this kernel has a working TCP/IP stack now, but
  still no TLS (so no HTTPS), no DNS resolver, and no registered OAuth
  credentials to talk to those services with -- any "sign in" UI here
  would need to be honestly local-only.

## License

MiniWin's own code is MIT-licensed -- see `LICENSE`. The bundled Dalmoori
font (`third_party/dalmoori-font/`) is Apache-2.0 licensed by its own
authors; see the `LICENSE`/`NOTICE.md` in that directory.
