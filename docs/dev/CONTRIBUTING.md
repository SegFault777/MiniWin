# Contributing

## Ground rules

* **Dependency-free.** No libc, no third-party code in the kernel. Bundled data (fonts, root certificates, icons) lives in
  `third_party/` with its own license and notice.
* **freestanding `-m32`; no 64-bit division** (it pulls in a runtime the kernel does not have). Use 32-bit arithmetic or shifts.
* **Everything is bounded.** Loops that wait for hardware or parse untrusted data have limits; failure is a return value.
* **Comments are part of the product**: they say *why*, and may be funny, but must be true. When code changes, its comment changes.
* New source files start with `SPDX-License-Identifier: GPL-3.0-only` (generated data and third-party files excepted; art-derived data
  carries `CC-BY-SA-4.0`, see [LICENSING.md](../project/LICENSING.md)).

## Build and test

```sh
./build.sh && tools/install_all.sh          # always both: install_all writes the sample program and the icon catalog
```

Run what your change can affect ([TESTING.md](TESTING.md) has the full list and what each covers):

| you changed | run |
|---|---|
| anything | `tools/test/check_docs.py`, `tools/test/check_version.sh` |
| network, crypto, HTML engine, storage, fonts | the matching `tools/test/run_host_*.sh` |
| anything visible | `tools/test/ui_golden.sh check classic` and `dark`, `ui_golden_files.sh check` |
| a disk driver, `os-inst`, the boot path | `python3 tools/test/qemu_storage.py` (takes about 8 minutes) |
| shutdown | `python3 tools/test/qemu_power.py` |

A visual change **intends** to change the goldens: record them (`record`) and look at the new pictures before committing. Do not run two
QEMU test scripts at the same time (they share temporary files and the timing-based ones fail under load).

## Commits and versions

* One commit per version: title `MiniWin 1.1-alpha-N`, a body that says what changed and why, what was verified (with numbers) and what
  is known not to work or was not run. Author `SegFault777`.
* The version number lives in **`kernel/version.h`**; `README.md` ("Current version") and the first entry of
  `docs/project/CHANGELOG.md` must name the same one (`tools/test/check_version.sh` checks). Update the docs a change touches in the
  same commit -- [docs/README.md](../README.md) says where each kind of fact lives.
* The README is re-read against the code at every version: stale sentences about older versions are bugs.

## Documentation layout

`docs/user/` is for people who run MiniWin, `docs/dev/` for people who change it, `docs/project/` for history, plan and licensing.
`docs/dev/SOURCE-MAP.md` is generated (`tools/gen_source_map.py`). `tools/test/check_docs.py` fails on a broken link or a mention of a
`docs/` path that does not exist.

## Honest reporting

Say what was run and on what. Everything hardware-related so far has only been run on QEMU; a claim about VMware, VirtualBox or real
machines needs an actual run on them.
