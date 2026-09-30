#!/usr/bin/env bash
# Same idea as tls_e2e.sh but plain HTTP to a python http.server on :80 (regression check for the
# TCP layer). usage: tools/test/qemu_plain_http.sh [secs] [page]
set -uo pipefail
cd "$(dirname "$0")/../.."
T=${1:-30}; PAGE=${2:-small.html}
mkdir -p /tmp/www; [ -f /tmp/www/small.html ] || python3 -c "print('<html><body>'+''.join('<p>line %d</p>\n'%i for i in range(40))+'</body></html>')" > /tmp/www/small.html
IMG=/tmp/mw_plain; rm -rf $IMG
BUILD=$IMG EXTRA_CFLAGS="-DMW_AUTOTEST_URL=\\\"http://10.0.2.2/$PAGE\\\" -DMW_AUTOTEST_DUMP=100" ./build.sh >$IMG.log 2>&1 || { echo BUILD FAILED; exit 1; }
pkill -x python3 2>/dev/null; pkill -x qemu-system-i386 2>/dev/null; sleep 0.5
(cd /tmp/www && setsid python3 -m http.server 80 --bind 0.0.0.0 </dev/null >/tmp/http80.log 2>&1 &)
sleep 1; rm -f /tmp/serial.log
(setsid timeout "$T" qemu-system-i386 -m 128 -drive file=$IMG/os-image.img,format=raw -netdev user,id=n0 -device rtl8139,netdev=n0 \
   -display none -serial file:/tmp/serial.log -no-reboot -monitor none </dev/null >/tmp/qemu.log 2>&1 &)
sleep $((T + 3))
grep -a "AUTOTEST\|TCP\]" /tmp/serial.log | tail -12
pkill -x python3; true
