#!/usr/bin/env bash
# Boots the OS in QEMU with the autotest URL pointing at a page served from /tmp/www, runs a monitor script
# (waits, key presses, mouse clicks, screenshots) and prints the serial log's [AUTOTEST] lines.
# usage: [NIC=e1000] tools/test/qemu_shot.sh PAGE "wait 25" "shot /tmp/a.ppm" ["click" ...]
set -uo pipefail
cd "$(dirname "$0")/../.."
PAGE=${1#/}; shift
IMG=/tmp/mw_shot; rm -rf $IMG
BUILD=$IMG EXTRA_CFLAGS="-DMW_AUTOTEST_URL=\\\"http://10.0.2.2/$PAGE\\\" -DMW_AUTOTEST_DUMP=${DUMP:-300} ${XCFLAGS:-}" ./build.sh >$IMG.log 2>&1 || { echo BUILD FAILED; tail -20 $IMG.log; exit 1; }
tools/install_all.sh $IMG/os-image.img >>$IMG.log 2>&1 || { echo INSTALL FAILED; tail -5 $IMG.log; exit 1; }
pkill -f 'webserver.py' 2>/dev/null; pkill -f 'qemu-system-i386' 2>/dev/null; sleep 0.5
(cd /tmp/www && setsid python3 "$OLDPWD/tools/test/webserver.py" --port 80 </dev/null >/tmp/http80.log 2>&1 &)
sleep 1; rm -f /tmp/serial.log /tmp/mon.sock
(setsid qemu-system-i386 -m 128 -drive file=$IMG/os-image.img,format=raw -netdev user,id=n0 -device ${NIC:-rtl8139},netdev=n0 \
   -display none -serial file:/tmp/serial.log -no-reboot -monitor unix:/tmp/mon.sock,server,nowait </dev/null >/tmp/qemu.log 2>&1 &)
for i in $(seq 1 50); do [ -S /tmp/mon.sock ] && break; sleep 0.2; done
python3 tools/test/qemu_mon.py /tmp/mon.sock "$@"
sed -n '/\[AUTOTEST\] go/,$p' /tmp/serial.log | grep -av "^\[NET\]\|\[PCI\]\|\[DHCP\]\|\[ARP\]\|\.\. tcp" | tail -${LINES_OUT:-40}
pkill -f 'qemu-system-i386'; pkill -f 'webserver.py'; true
