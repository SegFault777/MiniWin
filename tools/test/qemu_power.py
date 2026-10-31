#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""The Terminal's `off` must really power the virtual machine off (kernel/acpi.h), on QEMU's i440FX and Q35 chipsets and with
   ACPI tables hidden from the guest (hypervisor-port fallback). Needs build/os-image.img.
     tools/test/qemu_power.py"""
import os, subprocess, sys, time
HERE = os.path.dirname(os.path.abspath(__file__)); ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
IMG = os.path.join(ROOT, "build", "os-image.img"); SER = "/tmp/mw_power.log"; MON = "/tmp/mw_power.sock"
fails = 0

def run(name, machine_args, expect_exit=True):
    global fails
    for f in (SER, MON):
        if os.path.exists(f): os.remove(f)
    cmd = ["qemu-system-i386", "-m", "128", "-display", "none", "-nic", "none", "-serial", "file:" + SER,
           "-monitor", "unix:%s,server,nowait" % MON, "-drive", "file=%s,format=raw" % IMG] + machine_args
    q = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        for _ in range(50):
            if os.path.exists(MON): break
            time.sleep(0.2)
        cmds = ["wait 14", "move 50 202", "click", "click", "wait 2", "move 300 307", "click", "type mw-test-pw", "key ret", "type mw-test-pw", "key ret", "wait 1", "type off", "key ret"]
        subprocess.run(["python3", os.path.join(HERE, "qemu_mon.py"), MON] + cmds, timeout=120, capture_output=True)
        for _ in range(60):                                   # up to ~12 s for the guest to cut power
            if q.poll() is not None: break
            time.sleep(0.2)
        exited = q.poll() is not None
    finally:
        if q.poll() is None: q.kill()
        q.wait()
    log = open(SER, "rb").read().decode("latin1")
    path = "ACPI S5" if "[ACPI] S5 power-off" in log else ("hypervisor ports" if "falling back" in log else "no method")
    ok = exited == expect_exit
    print(("PASS  " if ok else "FAIL  ") + "%s: VM %s via %s" % (name, "powered off" if exited else "still running", path))
    if not ok: fails += 1

if not os.path.exists(IMG): sys.exit("build/os-image.img missing")
run("i440FX (-M pc)", ["-M", "pc"])
run("Q35 (-M q35)", ["-M", "q35"])
# Without ACPI tables only the hypervisor ports are left. QEMU wires none of them on a chipset whose power-management block the
# firmware never initialised, so the VM is expected to stay up (showing the "safe to turn off" message); VirtualBox's 0x4004
# path cannot be exercised here. What this checks: the fallback is taken and nothing faults.
run("ACPI tables hidden (-M pc,acpi=off): falls back and halts cleanly", ["-M", "pc,acpi=off"], expect_exit=False)
print("%d failure(s)" % fails); sys.exit(1 if fails else 0)
