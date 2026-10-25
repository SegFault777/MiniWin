#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# kernel/mwfs.h on a RAM disk: files, directories, a full volume, a randomized run against a model, and a crash sweep.
set -euo pipefail
cd "$(dirname "$0")"
gcc -O1 -g -Wall -Wextra -Wno-unused-function -Wno-unused-variable -Wno-unused-parameter -Wno-pointer-sign -Wno-missing-field-initializers \
    -fsanitize=undefined,address -o /tmp/host_mwfs_test host_mwfs_test.c
# an image built by the build's own formatter (tools/mwfs.py) must be readable by the C code
python3 ../mwfs.py format /tmp/mwfs_py.img --lba 1733 --sectors 315 >/dev/null 2>&1 || { dd if=/dev/zero of=/tmp/mwfs_py.img bs=512 count=2048 2>/dev/null; python3 ../mwfs.py format /tmp/mwfs_py.img --lba 1733 --sectors 315 >/dev/null; }
printf 'GREETER-TEST-PROGRAM-BYTES-%.0s' $(seq 1 40) > /tmp/mwfs_prog.bin
python3 ../mwfs.py put /tmp/mwfs_py.img /tmp/mwfs_prog.bin --lba 1733 --sectors 315 --path /progs/GREETER.MWP >/dev/null
MWFS_PY_IMAGE=/tmp/mwfs_py.img /tmp/host_mwfs_test
