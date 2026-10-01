#!/usr/bin/env bash
# Host-side tests for the HTTP response handling and the HTML renderer.
set -euo pipefail
cd "$(dirname "$0")"
gcc -O1 -g -Wall -Wextra -Wno-unused-function -fsanitize=address,undefined -o /tmp/host_web_test host_web_test.c
/tmp/host_web_test
