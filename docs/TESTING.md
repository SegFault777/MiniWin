# Testing

MiniWin has no continuous-integration service; everything below runs locally. The **host** suites compile the real kernel
headers with `gcc` and need no emulator; the **QEMU** suites boot the real image. Run from the repository root.

## Host suites (seconds)

| suite | command | what it covers |
|---|---|---|
| web | `tools/test/run_host_web.sh` | HTTP response parsing (Content-Length, chunked, token lists, oversize), POST, HTML engine basics |
| engine | `tools/test/run_host_engine.sh [N]` | HTML/CSS/layout engine, form encoding, incomplete-page detection, fuzz loop (`N` iterations) |
| crypto | `tools/test/run_host_crypto.sh` | AES/GCM/SHA/HMAC/RSA/ECC/X25519 against OpenSSL & hashlib vectors, the embedded trust store |
| critical | `tools/test/run_host_critical.sh` | ATA timeouts, crash-safe saves (a power cut injected after *every* possible sector write), CRCs, TCP/IP/UDP checksums, TCP send failures, RST/window, ARP, DNS, DHCP (incl. lease renew/rebind/expire), TLS record headers and ServerHello. Runs the real headers against a fake ATA controller, NIC and RTC |
| x509 | `tools/test/run_host_x509.sh` | certificate extensions: 19 OpenSSL-generated variants (critical/non-critical, keyUsage, CA flags) |
| storage | `tools/test/run_host_storage.sh` | storage ids, IDENTIFY, MBR/EBR/GPT parsing, `st_resolve` refusals, and **the installer**: whole disk, partition, existing boot code, logical, GPT, write failures, silently lost writes, read failures, self-install. Fake four-position IDE controller; ASan + UBSan |

Counts at 1.1-alpha-2: web 134, engine 68, crypto 952, critical 416, x509 22, storage 157, font 15 (`run_host_font.sh`); `tools/test/qemu_storage.py` boots fourteen IDE/AHCI/SCSI disk topologies in QEMU; `tools/test/qemu_power.py` checks that `off` powers the VM off. A suite prints
`N checks, M failures` and exits non-zero on any failure.

## QEMU suites (minutes)

| suite | command | what it covers |
|---|---|---|
| UI goldens | `tools/test/ui_golden.sh check classic` / `check dark` | 15 screenshots of a scripted tour, compared pixel for pixel; `record` rewrites them (only for an *intended* visual change) |
| MiniFiles goldens | `tools/test/ui_golden_files.sh check` | 6 screenshots of the file manager (list, selection, delete armed, disks tab, no selection, install refused) |
| plain HTTP | `NIC=rtl8139\|e1000 tools/test/qemu_plain_http.sh 30 small.html` | DHCP, ARP, TCP and the HTTP client against `tools/test/webserver.py` |
| TLS | `mkdir -p /tmp/pki; tools/test/tls_e2e.sh rsa rsa 45 -tls1_2 -cipher ECDHE-RSA-AES128-GCM-SHA256 -curves X25519` | the whole stack to an `openssl s_server` (success = `RESULT https-ok`); `LEAF_KU=` / `LEAF_EXTRA=` make negative certificates |
| os-inst | `tools/test/qemu_osinst.sh` | installs onto a second disk (whole disk, then a partition behind a blank MBR), boots the result alone on the primary and the secondary channel, and checks the disk bytes (about 4 minutes) |

Tools you need: `nasm`, `qemu-system-x86`, `gcc` with 32-bit support (`gcc-multilib`), `python3` with Pillow (screenshots)
and `cryptography` (crypto vectors), `openssl`.

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
