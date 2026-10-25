#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Boots the OS in QEMU on every disk topology the storage layer is meant to handle, and checks the serial log.
   Needs build/os-image.img (./build.sh && tools/install_all.sh) and `qemu-system-i386`.
     tools/test/qemu_storage.py            # all scenarios
   Scenarios: boot from IDE master / IDE slave / an AHCI SATA disk / the second of two SATA disks / IDE + SATA mixed,
   and `os-inst sata200` driven through the Terminal (write + read-back through the AHCI driver), after which the
   installed disk must boot by itself. (Subprocess.Popen + kill, not pkill -f: the latter kills its own shell.)"""
import sys
import os, subprocess, sys, time
HERE = os.path.dirname(os.path.abspath(__file__)); ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
IMG = os.path.join(ROOT, "build", "os-image.img"); DISK2 = "/tmp/mw_disk2.img"; SER = "/tmp/mw_storage.log"; MON = "/tmp/mw_storage.sock"
fails = 0

def qemu(args, secs=0, mon_cmds=None):
    for f in (SER, MON):
        if os.path.exists(f): os.remove(f)
    cmd = ["qemu-system-i386", "-m", "128", "-display", "none", "-nic", "none", "-serial", "file:" + SER, "-no-reboot",
           "-monitor", "unix:%s,server,nowait" % MON] + args
    q = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        for _ in range(50):
            if os.path.exists(MON): break
            time.sleep(0.2)
        if mon_cmds:
            subprocess.run(["python3", os.path.join(HERE, "qemu_mon.py"), MON] + mon_cmds, timeout=240, capture_output=True)
        else:
            time.sleep(secs)
    finally:
        q.kill(); q.wait()
    return open(SER, "rb").read().decode("latin1")

def ahci(img, port, boot=False):
    n = "d%d" % port
    a = ["-drive", "file=%s,if=none,id=%s,format=raw" % (img, n), "-device", "ide-hd,drive=%s,bus=ahci.%d%s" % (n, port, ",bootindex=0" if boot else "")]
    return a

def check(name, ok, detail=""):
    global fails
    print(("PASS  " if ok else "FAIL  ") + name + ((" -- " + detail) if (detail and not ok) else ""))
    if not ok: fails += 1

def boot_line(log):
    for l in log.splitlines():
        if "installation base LBA" in l: return l.strip()
    return "(no boot line)"

if not os.path.exists(IMG): sys.exit("build/os-image.img missing: run ./build.sh && tools/install_all.sh first")
if os.path.exists(DISK2): os.remove(DISK2)
subprocess.check_call(["truncate", "-s", "8M", DISK2])

log = qemu(["-drive", "file=%s,format=raw" % IMG], 10)
check("IDE master boot", "LBA 0 on ide1" in boot_line(log), boot_line(log))

log = qemu(["-device", "ich9-ahci,id=ahci"] + ahci(IMG, 0, True), 10)
check("AHCI: the kernel lists the disk", "[AHCI] sata1 = port 0, 2048 sectors" in log)
check("AHCI: ...and finds its own boot disk", "LBA 0 on sata1" in boot_line(log), boot_line(log))

cmds = ["wait 14", "move 50 202", "click", "click", "wait 2", "move 300 307", "click", "type os-inst list", "key ret", "wait 2",
        "type os-inst sata200", "key ret", "wait 2", "type YES", "key ret", "wait 25", "shot /tmp/mw_storage_done.ppm"]
log = qemu(["-device", "ich9-ahci,id=ahci"] + ahci(IMG, 0, True) + ahci(DISK2, 1), mon_cmds=cmds)
check("AHCI: two disks are both found", "sata2 = port 1, 16384 sectors" in log)
a = open(DISK2, "rb").read(1048576); b = open(IMG, "rb").read()
diff = sorted({i // 512 for i in range(1048576) if a[i] != b[i]})
check("os-inst sata200: the whole system was written through AHCI and verified (only sector 0 = install id, and the freshly formatted root filesystem differ)",
      diff[:1] == [0] and all(d == 0 or 1733 <= d <= 2047 for d in diff) and a[436:440] != b"\0\0\0\0", str(diff[:10]))

log = qemu(["-device", "ich9-ahci,id=ahci"] + ahci(IMG, 0) + ahci(DISK2, 1, True), 12)
check("installed disk on the 2nd SATA port is found by its install id", "LBA 0 on sata2" in boot_line(log), boot_line(log))

log = qemu(["-drive", "file=%s,format=raw" % IMG, "-device", "ich9-ahci,id=ahci"] + ahci(DISK2, 0), 12)
check("IDE boot with an unrelated AHCI disk present stays on ide1", "LBA 0 on ide1" in boot_line(log), boot_line(log))

log = qemu(["-drive", "file=%s,if=none,id=x,format=raw" % DISK2, "-device", "ide-hd,drive=x,bus=ide.0,unit=0",
            "-drive", "file=%s,if=none,id=y,format=raw" % IMG, "-device", "ide-hd,drive=y,bus=ide.0,unit=1,bootindex=0"], 12)
check("IDE slave (ide2) boot", "LBA 0 on ide2" in boot_line(log), boot_line(log))

# ---- SCSI (LSI Logic / MPT: QEMU's mptsas1068, the same message protocol VMware and VirtualBox emulate) ----
def scsi(img, ident, boot=False):
    n = "s%d" % ident
    return ["-drive", "file=%s,if=none,id=%s,format=raw" % (img, n),
            "-device", "scsi-hd,drive=%s,bus=scsi0.0,scsi-id=%d%s" % (n, ident, ",bootindex=0" if boot else "")]
MPT = ["-device", "mptsas1068,id=scsi0"]

log = qemu(MPT + scsi(IMG, 0, True), 12)
check("SCSI: the kernel lists the disk", "[MPT] scsi1 = controller 0, target 0, 2048 sectors, QEMU QEMU HARDDISK" in log, log[-300:])
check("SCSI: ...and finds its own boot disk", "LBA 0 on scsi1" in boot_line(log), boot_line(log))

if os.path.exists(DISK2): os.remove(DISK2)
subprocess.check_call(["truncate", "-s", "8M", DISK2])
cmds = ["wait 14", "move 50 202", "click", "click", "wait 2", "move 300 307", "click", "type os-inst list", "key ret", "wait 2",
        "type os-inst scsi200", "key ret", "wait 2", "type YES", "key ret", "wait 25", "shot /tmp/mw_storage_scsi_done.ppm"]
log = qemu(MPT + scsi(IMG, 0, True) + scsi(DISK2, 3), mon_cmds=cmds)
check("SCSI: two targets (0 and 3) are both found", "target 3, 16384 sectors" in log, log[-300:])
a = open(DISK2, "rb").read(1048576)
diff = sorted({i // 512 for i in range(1048576) if a[i] != b[i]})
check("os-inst scsi200: written through the MPT controller and verified", diff[:1] == [0] and all(d == 0 or 1733 <= d <= 2047 for d in diff) and a[436:440] != b"\0\0\0\0", str(diff[:10]))
log = qemu(MPT + scsi(IMG, 0) + scsi(DISK2, 3, True), 14)
check("installed disk on SCSI target 3 is found by its install id", "LBA 0 on scsi2" in boot_line(log), boot_line(log))

log = qemu(["-drive", "file=%s,format=raw" % IMG] + MPT + scsi(DISK2, 1), 14)
check("IDE boot with an unrelated SCSI disk present stays on ide1", "LBA 0 on ide1" in boot_line(log), boot_line(log))

# ---- ATAPI CD/DVD: a known 128 KB "disc" in the secondary-master drive (cd3); the kernel logs the byte sums of sectors 0 and 5 ----
import random
random.seed(7)
ISO = "/tmp/mw_test.iso"
disc = bytes(random.randrange(256) for _ in range(2048 * 64))
open(ISO, "wb").write(disc)
log = qemu(["-drive", "file=%s,format=raw" % IMG, "-cdrom", ISO], 12)
check("ATAPI: the disc is found as cd3 with its size", "[ATAPI] cd3 = QEMU DVD-ROM, 256 sectors" in log, log[-300:])
check("ATAPI: sector 0 reads back exactly", "sector 0 byte sum %d" % sum(disc[0:512]) in log, log[-300:])
check("ATAPI: sector 5 (second quarter of block 1) reads back exactly", "sector 5 byte sum %d" % sum(disc[5 * 512:6 * 512]) in log, log[-300:])
check("ATAPI: the boot disk is still ide1", "LBA 0 on ide1" in boot_line(log), boot_line(log))

# ---- floppy: the same image padded to 1.44 MB, booted as drive A: (VMware / VirtualBox "floppy image file") ----
FLOPPY = "/tmp/mw_floppy.img"
subprocess.check_call(["cp", IMG, FLOPPY]); subprocess.check_call(["truncate", "-s", "1474560", FLOPPY])
fargs = ["-drive", "file=%s,format=raw,if=floppy" % FLOPPY, "-boot", "a"]
log = qemu(fargs, 40)
check("floppy: the kernel reads itself in by CHS and registers the boot floppy", "[FDC] fd1 = the boot floppy (drive 0), 2880 sectors" in log, log[-300:])
check("floppy: ...and finds its own boot disk", "LBA 0 on fd1" in boot_line(log), boot_line(log))
before = open(FLOPPY, "rb").read()
cmds = ["wait 40", "move 50 25", "click", "click", "wait 2", "type Floppy write test", "wait 1", "move 40 38", "click", "wait 1", "move 40 66", "click", "wait 8"]
qemu(fargs, mon_cmds=cmds)
after = open(FLOPPY, "rb").read()
changed = sorted({i // 512 for i in range(len(before)) if before[i] != after[i]})
sys.path.insert(0, "tools")
import mwfs
try:
    fv = mwfs.Vol(FLOPPY, 1733, 315); fv.open()
    docs = [n for n, _, _ in fv.entries(fv.walk("/docs"))]
except Exception as e:
    docs = ["error: %s" % e]
check("floppy: a Notepad save is written to the floppy (only the root volume changes, and the document is in /docs)",
      bool(changed) and min(changed) >= 1733 and "DOC.TXT" in docs, str(changed[:10]) + " " + str(docs))

# ---- NVMe (QEMU's `nvme` device: one controller per -device, namespace 1 is the drive) ----
def nvme(img, n, boot=False):
    return ["-drive", "file=%s,if=none,id=n%d,format=raw" % (img, n), "-device", "nvme,drive=n%d,serial=mw%d%s" % (n, n, ",bootindex=0" if boot else "")]

if os.path.exists(DISK2): os.remove(DISK2)
subprocess.check_call(["truncate", "-s", "8M", DISK2])
log = qemu(nvme(IMG, 0, True), 12)
check("NVMe: the kernel lists the SSD", "[NVMe] nvme1 = controller 0, namespace 1, 2048 sectors" in log, log[-300:])
check("NVMe: ...and finds its own boot disk", "LBA 0 on nvme1" in boot_line(log), boot_line(log))

cmds = ["wait 14", "move 50 202", "click", "click", "wait 2", "move 300 307", "click", "type os-inst list", "key ret", "wait 2",
        "type os-inst nvme200", "key ret", "wait 2", "type YES", "key ret", "wait 25", "shot /tmp/mw_storage_nvme_done.ppm"]
log = qemu(nvme(IMG, 0, True) + nvme(DISK2, 1), mon_cmds=cmds)
check("NVMe: two controllers are both found", "nvme2 = controller 1, namespace 1, 16384 sectors" in log, log[-300:])
a = open(DISK2, "rb").read(1048576)
diff = sorted({i // 512 for i in range(1048576) if a[i] != b[i]})
check("os-inst nvme200: written through NVMe and verified", diff[:1] == [0] and all(d == 0 or 1733 <= d <= 2047 for d in diff) and a[436:440] != b"\0\0\0\0", str(diff[:10]))
log = qemu(nvme(IMG, 0) + nvme(DISK2, 1, True), 14)
check("installed disk on the 2nd NVMe controller is found by its install id", "LBA 0 on nvme2" in boot_line(log), boot_line(log))

log = qemu(["-drive", "file=%s,format=raw" % IMG] + nvme(DISK2, 1), 14)
check("IDE boot with an unrelated NVMe SSD present stays on ide1", "LBA 0 on ide1" in boot_line(log), boot_line(log))

print("%d failure(s)" % fails); sys.exit(1 if fails else 0)
