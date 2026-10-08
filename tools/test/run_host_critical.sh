#!/usr/bin/env bash
# Host-side regression tests for the critical findings of the 2026-10-08 audit
# (ATA timeouts, partial program loads, TCP SYN-ACK validation + RX checksum).
set -euo pipefail
cd "$(dirname "$0")"
gcc -O1 -g -Wall -Wextra -Wno-unused-function -Wno-unused-variable -Wno-pointer-sign -Wno-unused-parameter \
    -fsanitize=undefined -o /tmp/host_critical_test host_critical_test.c
/tmp/host_critical_test
