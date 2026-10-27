#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Paging and fault containment (1.1-alpha-11), end to end in QEMU: boots the image, starts sdk/examples/crash.c (CRASHTST.MWP) in a
# window, presses each of its six keys in a fresh boot -- null write, null read, write into the kernel's code, runaway recursion
# (double fault through the task gate), divide by zero, invalid instruction -- and requires, from the serial log, that the OS reported
# exactly that fault for the program and did NOT reset (no triple fault: QEMU's own `-d cpu_reset` log must show only the two power-on resets).
# Takes about 7 minutes; the image must have the demos installed (tools/install_all.sh).
#   tools/test/qemu_paging.sh
set -uo pipefail
cd "$(dirname "$0")/../.."
fail=0
check() {      # key  expected-text
    QEMU_EXTRA="-d cpu_reset -D /tmp/qp_reset_$1.log" LINES_OUT=1 timeout 150 tools/test/qemu_shot.sh ui_page.html \
        "wait 22" "move 50 202" "click" "click" "wait 2" "move 300 307" "click" "type run CRASHTST.MWP" "key ret" "wait 3" \
        "key $1" "wait 3" "type mem" "key ret" "wait 2" "shot /tmp/qp_$1.ppm" >/dev/null 2>&1
    if grep -aq "PROGRAM STOPPED: $2" /tmp/serial.log; then echo "ok   key $1: $2"; else echo "FAIL key $1: expected '$2'"; grep -a "MWP\|FATAL" /tmp/serial.log | tail -2; fail=1; fi
    # QEMU itself logs two "CPU Reset" lines while it powers the machine on (an idle boot has exactly two); a third is the guest resetting
    # the CPU, which is what a triple fault looks like from outside.
    if [ "$(grep -ac 'CPU Reset' /tmp/qp_reset_$1.log 2>/dev/null)" -gt 2 ]; then echo "FAIL key $1: the machine reset (triple fault)"; fail=1; fi
}
check 1 "PAGE FAULT AT 00000000"
check 2 "PAGE FAULT AT 00000000"
check 3 "PAGE FAULT AT 00010000"
check 4 "DOUBLE FAULT"
check 5 "DIVIDE ERROR"
check 6 "INVALID OPCODE"
[ "$fail" = 0 ] && echo "paging: all faults contained" || echo "paging: FAILED"
exit $fail
