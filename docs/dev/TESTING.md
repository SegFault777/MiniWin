# Testing

MiniWin has no continuous-integration service; everything below runs locally. The **host** suites compile the real kernel
headers with `gcc` and need no emulator; the **QEMU** suites boot the real image. Run from the repository root.

## Host suites (seconds)

| suite | command | what it covers |
|---|---|---|
| web | `tools/test/run_host_web.sh` | HTTP response parsing (Content-Length, chunked, token lists, oversize), POST, HTML engine basics |
| engine | `tools/test/run_host_engine.sh [N]` | HTML/CSS/layout engine, form encoding, incomplete-page detection, fuzz loop (`N` iterations) |
| crypto | `tools/test/run_host_crypto.sh` | AES/GCM/SHA/HMAC/RSA/ECC/X25519 against OpenSSL & hashlib vectors, the embedded trust store |
| critical | `tools/test/run_host_critical.sh` | ATA timeouts, a partly-readable program never counting as loaded (read errors, CRC mismatch), TCP/IP/UDP checksums, TCP send failures, RST/window, ARP, DNS, DHCP (incl. lease renew/rebind/expire), TLS record headers and ServerHello. Runs the real headers against a fake ATA controller, NIC and RTC |
| x509 | `tools/test/run_host_x509.sh` | certificate extensions: 19 OpenSSL-generated variants (critical/non-critical, keyUsage, CA flags) |
| storage | `tools/test/run_host_storage.sh` | storage ids, IDENTIFY, MBR/EBR/GPT parsing, `st_resolve` refusals, and **the installer**: whole disk, partition, existing boot code, logical, GPT, write failures (during the copy, during the filesystem format, at the boot sector), silently lost writes, read failures, self-install; the new root volume must open and check clean. Fake four-position IDE controller; ASan + UBSan |
| mwfs | `tools/test/run_host_mwfs.sh` | the file system (`mwfs.h`, `vfs.h`) on a RAM disk: files from 0 bytes past the double-indirect boundary, nested directories, a full volume, path normalisation, mounts and cross-volume copy/move, a 4000-step randomized run checked against a model, a **crash sweep** (every operation cut off after every possible number of sector writes: old-or-new, nothing referenced-but-unmarked, clean after `check fix`) and an image built by `tools/mwfs.py`. ASan + UBSan |
| mm | `tools/test/run_host_mm.sh` | the memory manager (`mm.h`) on an 8 MB array: page runs and exhaustion, refused bad frees, heap alignment, split/merge, chunk growth and release, `krealloc`, double free, a smashed header found by `mm_check`, and a 60,000-step random run where every live block carries a pattern checked when it is freed. ASan + UBSan |

Counts at 1.1-alpha-10: web 134, engine fuzz 300 iterations, crypto 952, critical 204 (the rest of the old 416 tested the retired slot journals; their job is `mwfs` now), x509 22, storage 166, mwfs 6224, mm 29647, font + cursor 15 (`run_host_font.sh`).
Other checks:

| script | what it checks | time |
|---|---|---|
| `tools/test/check_docs.py` | every relative link in the README and `docs/` resolves, every `docs/...` path mentioned in the sources exists, `SOURCE-MAP.md` is current | seconds |
| `tools/test/check_version.sh` | `kernel/version.h`, the README ("Current version") and the first CHANGELOG entry name the same version | seconds |
| `tools/test/qemu_storage.py` | boots IDE, AHCI, SCSI, NVMe, floppy and ATAPI topologies in QEMU: boot disk found, `os-inst` onto a second disk and booting the result, a Notepad save landing on the floppy, a known disc read back (the root filesystem it writes is compared too) | about 10 minutes |
| `tools/test/qemu_power.py` | MiniShell's `shutdown` powers QEMU off (i440FX, Q35; ACPI hidden -> fallback) | 1 minute |
| `tools/test/ui_golden.sh`, `ui_golden_files.sh` | pixel-exact screenshot tours (classic and dark themes, MiniFiles) | 3 minutes each |

A suite prints
`N checks, M failures` and exits non-zero on any failure.

## QEMU suites (minutes)

| suite | command | what it covers |
|---|---|---|
| UI goldens | `tools/test/ui_golden.sh check classic` / `check dark` | 15 screenshots of a scripted tour, compared pixel for pixel; `record` rewrites them (only for an *intended* visual change) |
| MiniFiles goldens | `tools/test/ui_golden_files.sh check` | 6 screenshots of the file manager (list, selection, delete armed, disks tab, no selection, install refused) |
| plain HTTP | `NIC=rtl8139\|e1000 tools/test/qemu_plain_http.sh 30 small.html` | DHCP, ARP, TCP and the HTTP client against `tools/test/webserver.py` |
| TLS | `mkdir -p /tmp/pki; tools/test/tls_e2e.sh rsa rsa 45 -tls1_2 -cipher ECDHE-RSA-AES128-GCM-SHA256 -curves X25519` | the whole stack to an `openssl s_server` (success = `RESULT https-ok`); `LEAF_KU=` / `LEAF_EXTRA=` make negative certificates |
| os-inst | `tools/test/qemu_osinst.sh` | installs onto a second disk (whole disk, then a partition behind a blank MBR), boots the result alone on the primary and the secondary channel, and checks the disk bytes: the copied sectors, the freshly formatted root filesystem filling the target, nothing else touched (about 4 minutes) |
| USB | by hand, see below | the three controller types and the input devices on them |

Tools you need: `nasm`, `qemu-system-x86`, `gcc` with 32-bit support (`gcc-multilib`), `python3` with Pillow (screenshots)
and `cryptography` (crypto vectors), `openssl`.

## Testing USB by hand

There is no scripted USB suite yet; this is how the driver was verified, and how to repeat it. QEMU's PS/2 controller can be removed so the
USB path is the only way in (the kernel must then treat the empty `0x60/0x64` ports as "no controller"):

    qemu-system-i386 -machine pc,i8042=off -drive file=build/os-image.img,format=raw \
        -device qemu-xhci,id=x -device usb-kbd,bus=x.0 -device usb-mouse,bus=x.0 -device usb-tablet,bus=x.0
    # or  -device usb-ehci,id=e ... bus=e.0     or  -device pci-ohci,id=o ... bus=o.0
    # the serial log (-serial file:...) says "[USB] keyboard ready on xhci0 port 5 ...";  lsusb in MiniShell shows the same

Drive it with the monitor (`tools/test/qemu_mon.py`): `move`/`click` for a relative mouse and `key`/`type` for the keyboard. A tablet takes
absolute positions through QMP `input-send-event` (0..32767), its buttons through the HMP `mouse_button`; the wheel is `mouse_move 0 0 <dz>`.
Another QEMU already holding the image gives "Failed to get write lock": kill it first. Not testable on QEMU: the EHCI-to-companion hand-off,
because QEMU's EHCI models accept full-speed devices on their own ports.

## How the tests are kept honest

* Each fix was verified **against the code without it**: the same test is run on the pre-fix tree (it fails or hangs) and on
  the fixed one (it passes).
* **Mutation checks.** Take a finished fix, revert *one line* of it in a scratch copy of the tree, run the suite: it must
  fail. If it still passes, the test does not actually protect that line. (A small script that copies `kernel/` and
  `tools/` to `/tmp`, applies one textual replacement and runs a suite is enough.) The suites above caught every mutation
  tried: about 60 across the 1.0 hardening work and the installer.
* The fault injection in the fake controllers (refused writes, silently dropped writes, unreadable sectors, a power cut
  after N writes, a stuck-busy drive, a floating bus) is how failure paths are exercised: they can't be produced by a
  healthy emulated disk.

## Things the tests do not cover

Real hardware (everything is QEMU and fake controllers), real-world TLS chains from public sites (only the embedded roots
were checked for compatibility), a drive that reorders or half-writes a sector, and the BIOS of a real PC booting an
installed disk.
