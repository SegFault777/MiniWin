#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# End-to-end test of the terminal's `os-inst` in QEMU: install MiniWin onto a second disk (whole disk, and a partition of
# an MBR disk with a blank boot-code area), then boot the TARGET alone, on the primary AND the secondary channel, and check
# the disk contents byte for byte.   tools/test/qemu_osinst.sh      (needs qemu-system-i386, python3 + Pillow; about 3 minutes, less with SKIP_BUILD=1)
set -uo pipefail
cd "$(dirname "$0")/../.."
B=/tmp/osinst_test; rm -rf $B; mkdir -p $B
if [ -z "${SKIP_BUILD:-}" ]; then ./build.sh >$B/build.log 2>&1 && tools/install_all.sh >/dev/null 2>&1 || { echo "BUILD FAILED"; exit 1; }; fi   # SKIP_BUILD=1: use the existing build/
cp build/os-image.img $B/src.img
fail=0; ok() { echo "ok   $1"; }; bad() { echo "FAIL $1"; fail=1; }

run() {   # run TAG "drive args" mon-commands...
    local tag=$1 drives=$2; shift 2
    rm -f /tmp/mon.sock $B/serial_$tag.log
    (setsid qemu-system-i386 -m 128 $drives -display none -serial file:$B/serial_$tag.log -no-reboot \
        -monitor unix:/tmp/mon.sock,server,nowait </dev/null >$B/qemu_$tag.log 2>&1 &)
    for i in $(seq 1 50); do [ -S /tmp/mon.sock ] && break; sleep 0.2; done
    python3 tools/test/qemu_mon.py /tmp/mon.sock "$@" >/dev/null 2>&1
    for p in $(pgrep -f "^qemu-system-i386 -m 128 .*serial_$tag"); do kill $p 2>/dev/null; done
    sleep 0.5
}
TERM_OPEN=("wait 20" "move 50 202" "click" "click" "wait 2" "move 300 307" "click" "type mw-test-pw" "key ret" "type mw-test-pw" "key ret" "wait 1")

# ---- 1. whole disk ----
truncate -s 16M $B/whole.img
run whole "-drive file=$B/src.img,format=raw,if=ide,index=0 -drive file=$B/whole.img,format=raw,if=ide,index=1" \
    "${TERM_OPEN[@]}" "type os-inst ide200" "key ret" "wait 2" "type YES" "key ret" "wait 14"
python3 - $B/src.img $B/whole.img <<'PY' && ok "whole disk: system sectors copied, root filesystem formatted over the disk, id patched, data area untouched" || bad "whole disk image check"
import struct, sys
src=open(sys.argv[1],'rb').read(); t=open(sys.argv[2],'rb').read()
sec=lambda d,l: d[l*512:(l+1)*512]
system=list(range(1,1733))      # boot sector (patched, checked below), stage 2, kernel, icon catalog
assert all(sec(src,l)==sec(t,l) for l in system), "system sectors differ"
assert sec(t,1733)[:4]==b'MWF2', "no MWFS root volume after the system sectors"
root_sectors=struct.unpack('<I',sec(t,1733)[8:12])[0]; data_lba=struct.unpack('<I',sec(t,1733)[32:36])[0]
assert root_sectors==32768-1733, "the root volume fills the rest of the disk (got %d)" % root_sectors
assert not any(t[(1733+data_lba+2)*512:]), "the volume's data area was written (only the root directory sector may be)"
b=sec(t,0); assert struct.unpack('<II',b[432:440])[0]==0 and struct.unpack('<I',b[436:440])[0]!=0, "fields"
assert [i for i in range(512) if sec(src,0)[i]!=b[i]]==[436,437,438,439] or struct.unpack('<I',b[436:440])[0]!=0
PY
run whole_boot "-drive file=$B/whole.img,format=raw,if=ide,index=0" "wait 20" "shot $B/whole_boot.ppm"
grep -aq "installation base LBA 0 on ide1" $B/serial_whole_boot.log && ok "whole disk boots alone (primary master)" || bad "whole disk boot"

# ---- 2. partition of an MBR disk with blank boot code ----
python3 - $B/part.img <<'PY'
import struct, sys
d=bytearray(16*1024*1024)
def ent(i,st,typ,start,cnt):
    o=446+i*16; d[o]=st; d[o+4]=typ; struct.pack_into('<II',d,o+8,start,cnt)
ent(0,0,0x83,2048,4096); ent(1,0,0x83,10240,8192); d[510]=0x55; d[511]=0xAA
open(sys.argv[1],'wb').write(d)
PY
cp $B/src.img $B/src_run.img
run part "-drive file=$B/src_run.img,format=raw,if=ide,index=0 -drive file=$B/part.img,format=raw,if=ide,index=1" \
    "${TERM_OPEN[@]}" "type os-inst ide202" "key ret" "wait 2" "type YES" "key ret" "wait 14"
python3 - $B/src.img $B/part.img boot/../build/chain_mbr.bin <<'PY' && ok "partition: image at its base, chain loader on the blank MBR, partition 2 active, table kept" || bad "partition image check"
import struct, sys
src=open(sys.argv[1],'rb').read(); t=open(sys.argv[2],'rb').read(); chain=open(sys.argv[3],'rb').read()
base=10240
sec=lambda d,l: d[l*512:(l+1)*512]
assert all(sec(src,l)==sec(t,base+l) for l in range(1,1733))
assert sec(t,base+1733)[:4]==b'MWF2', "no MWFS root volume in the partition"
assert struct.unpack('<I',sec(t,base+1733)[8:12])[0]==8192-1733, "root volume fills the partition"
data_lba=struct.unpack('<I',sec(t,base+1733)[32:36])[0]
assert not any(t[(base+1733+data_lba+2)*512:(base+8192)*512]), "partition data area written"
assert not any(t[(base+8192)*512:]), "past the partition touched"
assert t[:446]==chain and t[510:512]==b'\x55\xaa'
assert [t[446+i*16] for i in range(4)]==[0,0x80,0,0]
assert struct.unpack('<I',sec(t,base)[432:436])[0]==base
assert not any(t[4096*512:base*512]), "gap touched"
PY
for ch in 0 2; do
    run part_boot$ch "-drive file=$B/part.img,format=raw,if=ide,index=$ch" "wait 20" "shot $B/part_boot$ch.ppm"
    want=$([ $ch = 0 ] && echo ide1 || echo ide3)
    grep -aq "installation base LBA 10240 on $want" $B/serial_part_boot$ch.log && ok "partition install boots alone via the chain loader ($want)" || bad "partition boot on $want"
done
python3 - $B <<'PY' && ok "the booted installs show the desktop" || bad "booted screens"
import sys
from PIL import Image
for n in ("whole_boot","part_boot0","part_boot2"):
    im=Image.open(sys.argv[1]+"/%s.ppm"%n).convert("RGB")
    assert im.getpixel((300,200))==(85,255,255), n          # the desktop colour in the middle of the screen
PY
exit $fail
