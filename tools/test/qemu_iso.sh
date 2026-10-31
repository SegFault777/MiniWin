#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# End-to-end test of the LIVE ISO in QEMU (1.1-alpha-13):
#   1. boot build/os-image.iso alone (-cdrom): the kernel must say it runs from RAM and the desktop must come up;
#   2. install from the live system onto a blank disk (terminal `os-inst`), then check that disk byte for byte -- in particular that the
#      LIVE stamp and the partition entry the ISO's boot image carries (tools/mkiso.py) did NOT leak onto the installed disk;
#   3. boot that disk alone: it is an ordinary installation (not live).
# tools/test/qemu_iso.sh      (needs qemu-system-i386, python3 + Pillow; about 2 minutes, less with SKIP_BUILD=1)
set -uo pipefail
cd "$(dirname "$0")/../.."
B=/tmp/iso_test; rm -rf $B; mkdir -p $B
if [ -z "${SKIP_BUILD:-}" ]; then ./build.sh >$B/build.log 2>&1 && tools/install_all.sh >/dev/null 2>&1 || { echo "BUILD FAILED"; exit 1; }; fi
fail=0; ok() { echo "ok   $1"; }; bad() { echo "FAIL $1"; fail=1; }

run() {   # run TAG "drive args" mon-commands...
    local tag=$1 drives=$2; shift 2
    rm -f /tmp/mon.sock $B/serial_$tag.log
    (setsid qemu-system-i386 -m 128 $drives -display none -serial file:$B/serial_$tag.log -no-reboot -rtc base=2026-01-02T03:04:05,clock=vm \
        -monitor unix:/tmp/mon.sock,server,nowait </dev/null >$B/qemu_$tag.log 2>&1 &)
    for i in $(seq 1 50); do [ -S /tmp/mon.sock ] && break; sleep 0.2; done
    python3 tools/test/qemu_mon.py /tmp/mon.sock "$@" >/dev/null 2>&1
    for p in $(pgrep -f "^qemu-system-i386 -m 128 .*serial_$tag"); do kill $p 2>/dev/null; done
    sleep 0.5
}
TERM_OPEN=("wait 20" "move 50 202" "click" "click" "wait 2" "move 300 307" "click" "type mw-test-pw" "key ret" "type mw-test-pw" "key ret" "wait 1")

# ---- 0. the artifacts and their hard limits ----
python3 - build/os-image.iso build/os-image-floppy.img <<'PY' && ok "ISO <= 5 MB, floppy image <= 1.44 MB, boot catalog says hard-disk emulation (media 4)" || bad "artifact checks"
import struct, sys
iso=open(sys.argv[1],'rb').read(); fl=open(sys.argv[2],'rb').read()
assert len(iso) <= 5*1024*1024, len(iso)
assert len(fl) <= 1474560, len(fl)
blk=lambda n: iso[n*2048:(n+1)*2048]
cat=struct.unpack_from('<I',blk(17),0x47)[0]; e=blk(cat)[32:64]
assert e[0]==0x88 and e[1]==4, "boot catalog entry: bootable=%x media=%d" % (e[0], e[1])
PY

# ---- 1. the ISO alone ----
run live "-cdrom build/os-image.iso -boot d" "wait 22" "shot $B/live.ppm"
grep -aq "\[LIVE\] running from RAM" $B/serial_live.log && ok "the kernel runs from the RAM copy of the disc" || bad "no '[LIVE] running from RAM' in the serial log"
python3 - $B/live.ppm <<'PY' && ok "the live desktop comes up, with the INSTALL icon bottom right" || bad "live desktop"
import sys
from PIL import Image
im=Image.open(sys.argv[1]).convert("RGB")
assert im.getpixel((300,200))==(85,255,255)
icon=[im.getpixel((x,y)) for x in range(560,620) for y in range(362,394)]
assert any(p!=(85,255,255) for p in icon), "no INSTALL icon in the bottom-right cell"
PY

# ---- 2. install from the live system onto a blank disk ----
truncate -s 16M $B/target.img
run inst "-drive file=$B/target.img,format=raw,if=ide,index=0 -cdrom build/os-image.iso -boot d" \
    "${TERM_OPEN[@]}" "type os-inst ide100" "key ret" "wait 2" "type YES" "key ret" "wait 14"
python3 - build/os-image.img $B/target.img <<'PY' && ok "installed disk: system sectors identical to the stock image, no LIVE stamp, no partition entry, id patched" || bad "installed disk check"
import struct, sys
src=open(sys.argv[1],'rb').read(); t=open(sys.argv[2],'rb').read()
sec=lambda d,l: d[l*512:(l+1)*512]
assert all(sec(src,l)==sec(t,l) for l in range(1,1733)), "system sectors differ"
b=sec(t,0)
assert b[424:432]==bytes(8), "the LIVE stamp leaked onto the installed disk"
assert not any(b[446:510]), "the ISO's partition entry leaked onto the installed disk"
assert b[510:512]==b'\x55\xaa'
assert struct.unpack('<I',b[432:436])[0]==0 and struct.unpack('<I',b[436:440])[0]!=0, "install fields"
assert sec(t,1733)[:4]==b'MWF2', "no MWFS root volume"
PY

# ---- 2b. the same install through the INSTALL window (double-click the icon, pick, type YES, install) ----
truncate -s 16M $B/gui.img
run gui "-drive file=$B/gui.img,format=raw,if=ide,index=0 -cdrom build/os-image.iso -boot d" \
    "wait 20" "move 590 378" "click" "click" "wait 3" "shot $B/gui_pick.ppm" "move 483 396" "click" "wait 1" "type YES" "wait 1" "shot $B/gui_confirm.ppm" \
    "click" "wait 20" "shot $B/gui_done.ppm"
python3 - $B/gui_pick.ppm $B/gui_confirm.ppm $B/gui_done.ppm <<'PY' && ok "INSTALL window: disk list, YES confirmation page, finished page (screens differ page to page)" || bad "INSTALL window screens"
import sys
from PIL import Image
a,b,c=[Image.open(p).convert("RGB") for p in sys.argv[1:4]]
win=(95,45,520,380)
d=lambda x,y: sum(1 for px,py in zip(x.crop(win).getdata(), y.crop(win).getdata()) if px!=py)
assert d(a,b)>500 and d(b,c)>500, (d(a,b), d(b,c))
PY
python3 - build/os-image.img $B/gui.img <<'PY' && ok "GUI-installed disk: system sectors identical, no LIVE stamp, no partition entry, id patched, fresh MWFS" || bad "GUI-installed disk check"
import struct, sys
src=open(sys.argv[1],'rb').read(); t=open(sys.argv[2],'rb').read()
sec=lambda d,l: d[l*512:(l+1)*512]
assert all(sec(src,l)==sec(t,l) for l in range(1,1733)), "system sectors differ"
b=sec(t,0)
assert b[424:432]==bytes(8) and not any(b[446:510]) and b[510:512]==b'\x55\xaa'
assert struct.unpack('<I',b[432:436])[0]==0 and struct.unpack('<I',b[436:440])[0]!=0
assert sec(t,1733)[:4]==b'MWF2'
PY
run gui_boot "-drive file=$B/gui.img,format=raw,if=ide,index=0" "wait 20" "shot $B/gui_boot.ppm"
python3 - $B/gui_boot.ppm <<'PY' && ok "the GUI-installed system boots alone and its desktop has NO INSTALL icon" || bad "GUI-installed boot"
import sys
from PIL import Image
im=Image.open(sys.argv[1]).convert("RGB")
assert im.getpixel((300,200))==(85,255,255)
icon=[im.getpixel((x,y)) for x in range(546,634) for y in range(362,420)]
assert all(p==(85,255,255) for p in icon), "something is drawn where the INSTALL icon would be"
PY
grep -aq "installation base LBA 0 on ide1" $B/serial_gui_boot.log || bad "GUI-installed disk did not boot as an installation"

# ---- 3. boot the installed disk alone ----
run inst_boot "-drive file=$B/target.img,format=raw,if=ide,index=0" "wait 20" "shot $B/inst_boot.ppm"
grep -aq "installation base LBA 0 on ide1" $B/serial_inst_boot.log && ok "the installed disk boots alone" || bad "installed disk boot"
grep -aq "\[LIVE\]" $B/serial_inst_boot.log && bad "the installed disk thinks it is live" || ok "the installed disk is not a live system"
python3 - $B/inst_boot.ppm <<'PY' && ok "the installed system shows the desktop" || bad "installed desktop"
import sys
from PIL import Image
im=Image.open(sys.argv[1]).convert("RGB")
assert im.getpixel((300,200))==(85,255,255)
icon=[im.getpixel((x,y)) for x in range(546,634) for y in range(362,420)]
assert all(p==(85,255,255) for p in icon), "something is drawn where the INSTALL icon would be"
PY
exit $fail
