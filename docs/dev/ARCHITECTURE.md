# MiniWin architecture

MiniWin is a 32-bit x86 OS written from scratch: BIOS boot, protected mode, a truecolor 640x480 desktop,
a network stack with TLS 1.2, and a small browser. There is no libc and no malloc.

## One translation unit, layered by include order

The whole kernel compiles from `kernel/kernel.c` as a single translation unit; every module is a header full of
`static` functions. That keeps the binary small (the linker never has to guess what is dead) and means **the
order of the `#include`s in `kernel.c` is the dependency order**: a module may use anything included above it,
nothing below. Read that file's header comment for the layer map.

| layer | where | what |
|---|---|---|
| hardware & protocols | `kernel/*.h` | video, input, disk, PCI, NIC drivers, ARP/IP/UDP/TCP, DNS, DHCP, TLS + crypto, HTTP |
| HTML5 engine | `kernel/dom.h css.h layout.h render.h` | tokenizer/tree builder, CSS cascade, pixel layout, painter (see README) |
| **design** | `kernel/ui/theme.h`, `ui/themes/*.h`, `ui/widgets.h` | color roles + metrics, and every reusable piece of chrome |
| system | `kernel/sys/` | `util` (string/number helpers), `lang` (language + IME state), `power`, `boot` |
| shell | `kernel/ui/` | `icons clock window taskbar startmenu desktop_files resize cursor compose input` |
| apps | `kernel/apps/` | `notepad/*`, `setting`, `web/*` (state, address, fetch, page, autotest, view), `terminal` |

`kmain()` is only `kernel_boot()` followed by the loop `handle_mouse(); handle_keyboard(); poll network; render_frame();`.
Anything that decides *what a click means* is in `ui/input.h`; anything that decides *what a window looks like* is
in the app's own module, built from widgets.

## The design module

Nothing outside `kernel/ui/themes/` names a raw color or a chrome size.

* **`ui/themes/<name>.h`** — the whole look: color *roles* (`TH_FACE`, `TH_ACCENT`, `TH_LIGHT`, `TH_SHADOW`,
  `TH_FIELD`, `TH_CONSOLE`, ...) and metrics (`TH_TITLEBAR_H`, `TH_BTN_W`, `TH_TASKBAR_H`, shadow offsets).
  `classic.h` is the original gray-on-cyan look; `dark.h` is a deliberately different one.
* **`ui/theme.h`** — selects the theme (`THEME=dark ./build.sh`) and exposes the metrics under their historical
  names (`TITLEBAR_H`, `BTN_W`, `TASKBAR_H`...).
* **`ui/widgets.h`** — `ui_bevel`, `ui_panel`, `ui_window_frame`, `ui_titlebar_buttons`, `ui_field`, `ui_well`,
  `ui_item`, `ui_button_flat`, `ui_button`, `ui_pill`, `ui_readout`, `ui_scrollbar`, `ui_hline/vline`.
  Widgets only draw: no state, no decisions. Hover/pressed/focus arrive as arguments; a widget that picks a
  text color (`ui_item`, `ui_button_flat`) returns it.
* Web **page content** is deliberately outside the theme: pages are styled by their own CSS and the engine's UA
  stylesheet (`css.h`), not by the OS look.

Adding a theme: copy `classic.h`, change the numbers, build with `THEME=yourname ./build.sh`.
Adding chrome: write the widget once in `widgets.h`, call it from the apps.
If a screenshot under `THEME=dark` shows anything still in classic colors, a module hard-coded a color — fix it
there and record new goldens (`tools/test/ui_golden.sh record dark`).

## Window geometry

`ui/window.h` holds `window_t` and the shared title-bar button geometry (`win_btn_*`), used by every window's
hit-testing and by `ui_titlebar_buttons` for drawing, so the two can never drift apart.

## Memory

`kernel/memmap.h` is the one place that lists fixed physical addresses (network arena, HTML engine arena, icon
cache, program slot). Compile-time asserts keep regions from overlapping. `build.sh` checks that `.bss` stays
below the stack's guard band.

## Disk layout and crash safety

The disk map lives in the header comment of `kernel/fs.h`: boot sector, stage 2, the kernel, the icon catalog, and then the
**root volume** -- an **MWFS** file system (`kernel/mwfs.h`, namespace and mounts in `kernel/vfs.h`) with directories of any depth,
files that grow and a free-space bitmap. There is no journal: a save builds the new file in a fresh inode and one directory-entry
sector write switches the name over, so an interruption leaves the old file or the whole new one and, at worst, garbage that
`part check <id> fix` collects; every file carries a CRC-32 that loads verify. The old "slot" API survives in `fs.h` as an index of
`/docs` and `/progs` for the desktop, Notepad and MiniFiles. Details: [DISK-LAYOUT.md](DISK-LAYOUT.md).

## Network input is validated at every layer

IPv4 header checksum (`ip_parse`), UDP and TCP checksums, ARP replies accepted only for requests we sent, DNS
answers bound to our question (name/type/class, owner name, CNAME chain), DHCP replies bound to our xid/MAC/server,
TCP ACK/RST/window checks, TLS record headers, ServerHello parameters and certificate extensions. Each of those
checks has a negative test in `tools/test/host_critical_test.c` (or `run_host_x509.sh`).

## Testing

| what | how |
|---|---|
| HTML engine (DOM, CSS, layout, forms) | `tools/test/run_host_engine.sh` — assertions + an ASan/UBSan fuzzer, all on the build host |
| HTTP response/request handling | `tools/test/run_host_web.sh` |
| crypto + trust store | `tools/test/run_host_crypto.sh` |
| ATA timeouts, program loads that must not accept a partial read, TCP/ARP/DNS/DHCP/TLS-record validation | `tools/test/run_host_critical.sh` — the real kernel headers against a fake ATA controller / NIC / RTC |
| the file system: files, directories, mounts, a model-checked random run and a power cut after every possible sector write of every operation | `tools/test/run_host_mwfs.sh` |
| certificate extensions (critical, keyUsage, CA) | `tools/test/run_host_x509.sh` — OpenSSL-generated certificate variants |
| TLS end to end | `tools/test/tls_e2e.sh` (QEMU + openssl s_server) |
| **what the user sees** | `tools/test/ui_golden.sh check [theme]` — boots the OS in QEMU, drives a fixed tour (open every app, type, menus, drag, resize, maximize, minimize, restore, dialogs) and compares 15 screenshots **pixel for pixel** with `tools/test/golden*/` |

The golden test is what made the pre-29 refactor safe: kernel.c was split into modules (byte-identical binary),
the design was pulled into the theme + widgets and `kmain` was decomposed — and every step was checked against
the same goldens. Record new goldens only when a visual change is *intended*.
