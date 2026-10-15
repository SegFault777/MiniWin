#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# kernel/storage.h (+ the generalized ata.h) against a fake four-position IDE controller.
set -euo pipefail
cd "$(dirname "$0")"
gcc -O1 -g -Wall -Wextra -Wno-unused-function -Wno-unused-variable -Wno-unused-parameter -Wno-pointer-sign -Wno-missing-field-initializers \
    -fsanitize=undefined,address -o /tmp/host_storage_test host_storage_test.c
/tmp/host_storage_test
