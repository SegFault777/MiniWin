#!/usr/bin/env python3
"""Drives a running QEMU through its monitor socket: a tiny script language, one command per argument.
   wait N          sleep N seconds
   shot FILE.ppm   screendump
   key K           sendkey K (qemu key names: a, ret, spc, down, shift-a ...)
   type TEXT       sendkey for each character
   move X Y        absolute mouse position in screen pixels (640x480 screen)
   click           left click at the current position
   drag X1 Y1 X2 Y2  press at (X1,Y1), move RELATIVELY to (X2,Y2) with the button down, release
   raw CMD...      any monitor command
usage: qemu_mon.py SOCKET "wait 20" "shot /tmp/a.ppm" ..."""
import socket, sys, time
sock = socket.socket(socket.AF_UNIX); sock.connect(sys.argv[1]); sock.settimeout(2)
def rd():
    try: return sock.recv(65536)
    except Exception: return b""
rd()
def cmd(c):
    sock.sendall((c + "\n").encode()); time.sleep(0.15); rd()
KEYS = {" ": "spc", "\n": "ret", ".": "dot", "/": "slash", ":": "shift-semicolon", "?": "shift-slash", "=": "equal", "-": "minus", "_": "shift-minus", "&": "shift-7", "%": "shift-5"}
cur = [320, 240]
def rel(dx, dy):                        # a PS/2 packet carries at most +-255, and the guest's cursor math is happier with less
    while dx or dy:
        sx = max(-60, min(60, dx)); sy = max(-60, min(60, dy))
        cmd("mouse_move %d %d" % (sx, sy)); dx -= sx; dy -= sy
def absmove(x, y):                      # slam into the top-left corner, then walk to the target
    rel(-700, -600)
    time.sleep(0.2)
    rel(x, y)
for a in sys.argv[2:]:
    p = a.split(" ", 1)
    c = p[0]; arg = p[1] if len(p) > 1 else ""
    if c == "wait": time.sleep(float(arg))
    elif c == "shot": cmd("screendump " + arg)
    elif c == "key": cmd("sendkey " + arg)
    elif c == "type":
        for ch in arg:
            cmd("sendkey " + KEYS.get(ch, ("shift-" + ch.lower()) if ch.isupper() else ch))
    elif c == "move": x, y = map(int, arg.split()); absmove(x, y); cur[:] = [x, y]
    elif c == "drag":
        x1, y1, x2, y2 = map(int, arg.split()); absmove(x1, y1); cmd("mouse_button 1"); time.sleep(0.2); rel(x2 - x1, y2 - y1); time.sleep(0.2); cmd("mouse_button 0")
    elif c == "click": cmd("mouse_button 1"); time.sleep(0.15); cmd("mouse_button 0")
    elif c == "raw": cmd(arg)
    time.sleep(0.1)
