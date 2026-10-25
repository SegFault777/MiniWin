#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# kernel/mm.h (page allocator + heap) on an ordinary array, with the address and undefined-behaviour sanitizers on.
set -euo pipefail
cd "$(dirname "$0")"
gcc -O1 -g -Wall -Wextra -Wno-unused-function -Wno-unused-variable -Wno-unused-parameter -fsanitize=undefined,address -o /tmp/host_mm_test host_mm_test.c
/tmp/host_mm_test
