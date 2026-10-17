#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Boots the OS in QEMU on every disk topology the storage layer is meant to handle, and checks the serial log.
   Needs build/os-image.img (./build.sh && tools/install_all.sh) and `qemu-system-i386`.
     tools/test/qemu_storage.py            # all scenarios
   Scenarios: boot from IDE master / IDE slave / an AHCI SATA disk / the second of two SATA disks / IDE + SATA mixed,
   and `os-inst sata200` driven through the Terminal (write + read-back through the AHCI driver), after which the
   installed disk must boot by itself. (Subprocess.Popen + kill, not pkill -f: the latter kills its own shell.)"""
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

cmds = ["wait 14", "move 343 25", "click", "click", "wait 2", "move 300 307", "click", "type os-inst list", "key ret", "wait 2",
        "type os-inst sata200", "key ret", "wait 2", "type YES", "key ret", "wait 25", "shot /tmp/mw_storage_done.ppm"]
log = qemu(["-device", "ich9-ahci,id=ahci"] + ahci(IMG, 0, True) + ahci(DISK2, 1), mon_cmds=cmds)
check("AHCI: two disks are both found", "sata2 = port 1, 16384 sectors" in log)
a = open(DISK2, "rb").read(1048576); b = open(IMG, "rb").read()
diff = sorted({i // 512 for i in range(1048576) if a[i] != b[i]})
check("os-inst sata200: the whole system was written through AHCI and verified (only sector 0 = install id, and user-data slots differ)",
      diff[:1] == [0] and all(d == 0 or 1029 <= d <= 2047 for d in diff) and a[436:440] != b"\0\0\0\0", str(diff[:10]))

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
cmds = ["wait 14", "move 343 25", "click", "click", "wait 2", "move 300 307", "click", "type os-inst list", "key ret", "wait 2",
        "type os-inst scsi200", "key ret", "wait 2", "type YES", "key ret", "wait 25", "shot /tmp/mw_storage_scsi_done.ppm"]
log = qemu(MPT + scsi(IMG, 0, True) + scsi(DISK2, 3), mon_cmds=cmds)
check("SCSI: two targets (0 and 3) are both found", "target 3, 16384 sectors" in log, log[-300:])
a = open(DISK2, "rb").read(1048576)
diff = sorted({i // 512 for i in range(1048576) if a[i] != b[i]})
check("os-inst scsi200: written through the MPT controller and verified", diff[:1] == [0] and all(d == 0 or 1029 <= d <= 2047 for d in diff) and a[436:440] != b"\0\0\0\0", str(diff[:10]))
log = qemu(MPT + scsi(IMG, 0) + scsi(DISK2, 3, True), 14)
check("installed disk on SCSI target 3 is found by its install id", "LBA 0 on scsi2" in boot_line(log), boot_line(log))

log = qemu(["-drive", "file=%s,format=raw" % IMG] + MPT + scsi(DISK2, 1), 14)
check("IDE boot with an unrelated SCSI disk present stays on ide1", "LBA 0 on ide1" in boot_line(log), boot_line(log))

print("%d failure(s)" % fails); sys.exit(1 if fails else 0)
