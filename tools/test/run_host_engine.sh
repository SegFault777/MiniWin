#!/usr/bin/env bash
# Host-side tests for the HTML5 engine (dom/css/layout/render), then a burst of the ASan/UBSan fuzzer.
#   tools/test/run_host_engine.sh [fuzz-iterations]
set -euo pipefail
cd "$(dirname "$0")"
FLAGS="-O1 -g -Wall -Wextra -Wno-unused-function -Wno-misleading-indentation -Wno-unused-parameter -Wno-unused-variable -Wno-unused-but-set-variable -fsanitize=address,undefined -fno-sanitize-recover=undefined"
gcc $FLAGS -o /tmp/host_engine_test host_engine_test.c
/tmp/host_engine_test
gcc $FLAGS -o /tmp/host_rd_fuzz host_rd_fuzz.c
for seed in 1 2 3; do /tmp/host_rd_fuzz "${1:-300}" "$seed"; done
