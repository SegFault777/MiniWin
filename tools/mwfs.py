#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""mwfs.py -- the offline twin of kernel/mwfs.h: format an MWFS volume inside an image file and put files in it.

The build uses it to give the stock image its root filesystem ("/" with /docs /progs /mnt /sys) and to install programs
(tools/install_mwp.py); the kernel does the same things at run time with the C code. The two are kept byte-compatible --
tools/test/run_host_mwfs.sh formats an image with THIS script and then opens, lists and checks it with the C code.

    mwfs.py format IMAGE --lba N --sectors S [--label L] [--dirs a,b,c]
    mwfs.py put    IMAGE --lba N --sectors S --path /progs/NAME [--aux N] FILE
    mwfs.py ls     IMAGE --lba N --sectors S [--path /]
"""
import argparse, struct, sys, zlib

MAGIC = 0x3246574D
INO_SIZE, NDIRECT, PTRS, DENT, NAME_MAX = 64, 8, 128, 64, 31
T_FILE, T_DIR = 1, 2


class Vol:
    def __init__(self, path, lba, sectors):
        self.f = open(path, "r+b")
        self.base = lba
        self.sectors = sectors

    def rd(self, s):
        self.f.seek((self.base + s) * 512)
        b = self.f.read(512)
        return bytearray(b + b"\0" * (512 - len(b)))

    def wr(self, s, data):
        assert 0 <= s < self.sectors and len(data) == 512
        self.f.seek((self.base + s) * 512)
        self.f.write(bytes(data))

    def geometry(self):
        n = self.sectors
        inodes = max(32, min(16384, n // 8))
        inodes = (inodes + 7) & ~7
        self.inode_count = inodes
        self.bitmap_lba = 1
        self.bitmap_sectors = (n + 4095) // 4096
        self.itable_lba = 1 + self.bitmap_sectors
        self.itable_sectors = inodes // 8
        self.data_lba = self.itable_lba + self.itable_sectors
        assert self.data_lba + 8 <= n, "volume too small"

    def mkfs(self, label):
        self.geometry()
        self.wr(0, bytearray(512))
        for s in range(self.bitmap_sectors):
            b = bytearray(512)
            for bit in range(4096):
                lba = s * 4096 + bit
                if lba < self.data_lba or lba >= self.sectors:
                    b[bit >> 3] |= 1 << (bit & 7)
            self.wr(self.bitmap_lba + s, b)
        for s in range(self.itable_sectors):
            self.wr(self.itable_lba + s, bytearray(512))
        self.put_ino(1, dict(type=T_DIR))
        sb = bytearray(512)
        struct.pack_into("<IIIIIIIII", sb, 0, MAGIC, 1, self.sectors, self.inode_count, self.bitmap_lba,
                         self.bitmap_sectors, self.itable_lba, self.itable_sectors, self.data_lba)
        struct.pack_into("<I", sb, 36, 1)
        struct.pack_into("<I", sb, 44, zlib.crc32(bytes(sb[:44])))
        lab = label.encode()[:15]
        for i, c in enumerate(lab):
            sb[48 + i] = c
        self.wr(0, sb)

    def open(self):
        sb = self.rd(0)
        assert struct.unpack_from("<I", sb, 0)[0] == MAGIC, "no MWFS here"
        (_, _, total, self.inode_count, self.bitmap_lba, self.bitmap_sectors, self.itable_lba,
         self.itable_sectors, self.data_lba) = struct.unpack_from("<IIIIIIIII", sb, 0)
        self.sectors = total

    # ---- inodes: type,flags,size,crc,aux,nblocks,direct[8],ind,dind,gen
    def get_ino(self, n):
        b = self.rd(self.itable_lba + n // 8)
        v = struct.unpack_from("<HHIIII8IIII", b, (n % 8) * INO_SIZE)
        return dict(type=v[0], flags=v[1], size=v[2], crc=v[3], aux=v[4], nblocks=v[5], direct=list(v[6:14]), ind=v[14], dind=v[15], gen=v[16])

    def put_ino(self, n, d):
        d = dict(dict(type=0, flags=0, size=0, crc=0, aux=0, nblocks=0, direct=[0] * 8, ind=0, dind=0, gen=0), **d)
        b = self.rd(self.itable_lba + n // 8)
        struct.pack_into("<HHIIII8IIII", b, (n % 8) * INO_SIZE, d["type"], d["flags"], d["size"], d["crc"], d["aux"],
                         d["nblocks"], *d["direct"], d["ind"], d["dind"], d["gen"])
        self.wr(self.itable_lba + n // 8, b)

    def alloc_ino(self, typ):
        for n in range(2, self.inode_count):
            if self.get_ino(n)["type"] == 0:
                self.put_ino(n, dict(type=typ))
                return n
        raise SystemExit("mwfs.py: out of inodes")

    def alloc(self):
        for s in range(self.bitmap_sectors):
            b = self.rd(self.bitmap_lba + s)
            for i in range(512):
                if b[i] == 0xFF:
                    continue
                for k in range(8):
                    if not b[i] & (1 << k):
                        lba = s * 4096 + i * 8 + k
                        if lba >= self.sectors:
                            raise SystemExit("mwfs.py: volume full")
                        b[i] |= 1 << k
                        self.wr(self.bitmap_lba + s, b)
                        return lba
        raise SystemExit("mwfs.py: volume full")

    def bmap_alloc(self, ino, idx):
        if idx < NDIRECT:
            if not ino["direct"][idx]:
                ino["direct"][idx] = self.alloc(); ino["nblocks"] += 1
            return ino["direct"][idx]
        idx -= NDIRECT
        assert idx < PTRS, "mwfs.py handles files up to 136 sectors"
        if not ino["ind"]:
            ino["ind"] = self.alloc(); self.wr(ino["ind"], bytearray(512))
        t = self.rd(ino["ind"])
        p = struct.unpack_from("<I", t, idx * 4)[0]
        if not p:
            p = self.alloc(); ino["nblocks"] += 1
            struct.pack_into("<I", t, idx * 4, p); self.wr(ino["ind"], t)
        return p

    def block(self, ino, idx):
        if idx < NDIRECT:
            return ino["direct"][idx]
        t = self.rd(ino["ind"])
        return struct.unpack_from("<I", t, (idx - NDIRECT) * 4)[0]

    def write_data(self, n, data):
        ino = self.get_ino(n)
        for off in range(0, len(data), 512):
            lba = self.bmap_alloc(ino, off // 512)
            chunk = bytearray(data[off:off + 512]); chunk += b"\0" * (512 - len(chunk))
            self.wr(lba, chunk)
        ino["size"] = max(ino["size"], len(data))
        return ino

    # ---- directories
    def entries(self, dino):
        d = self.get_ino(dino)
        out = []
        for s in range((d["size"] + 511) // 512):
            b = self.rd(self.block(d, s))
            for e in range(8):
                ino, typ, nl = struct.unpack_from("<IBB", b, e * DENT)
                if ino:
                    out.append((b[e * DENT + 6:e * DENT + 6 + nl].decode(), ino, typ))
        return out

    def lookup(self, dino, name):
        for n, ino, typ in self.entries(dino):
            if n.lower() == name.lower():
                return ino, typ
        return None

    def add_entry(self, dino, name, ino, typ):
        assert 0 < len(name) <= NAME_MAX and not name.startswith(".")
        d = self.get_ino(dino)
        ent = bytearray(DENT); struct.pack_into("<IBB", ent, 0, ino, typ, len(name))
        for i, c in enumerate(name.encode()):
            ent[6 + i] = c
        for s in range((d["size"] + 511) // 512):
            lba = self.block(d, s); b = self.rd(lba)
            for e in range(8):
                if not struct.unpack_from("<I", b, e * DENT)[0]:
                    for i in range(DENT):
                        b[e * DENT + i] = ent[i]
                    self.wr(lba, b); return
        lba = self.bmap_alloc(d, d["size"] // 512)
        b = bytearray(512)
        for i in range(DENT):
            b[i] = ent[i]
        self.wr(lba, b)
        d["size"] += 512
        self.put_ino(dino, d)

    def mkdir(self, dino, name):
        r = self.lookup(dino, name)
        if r:
            return r[0]
        n = self.alloc_ino(T_DIR)
        self.add_entry(dino, name, n, T_DIR)
        return n

    def walk(self, path):
        cur = 1
        for part in [p for p in path.split("/") if p]:
            r = self.lookup(cur, part)
            if not r:
                raise SystemExit(f"mwfs.py: {path}: no such directory")
            cur = r[0]
        return cur

    def free_sector(self, lba):
        b = self.rd(self.bitmap_lba + lba // 4096)
        off = lba % 4096
        b[off >> 3] &= ~(1 << (off & 7)) & 0xFF
        self.wr(self.bitmap_lba + lba // 4096, b)

    def unlink(self, path):
        parent, _, name = path.rstrip("/").rpartition("/")
        dino = self.walk(parent)
        r = self.lookup(dino, name)
        if not r:
            return
        ino_n, typ = r
        assert typ == T_FILE, "mwfs.py only removes files"
        ino = self.get_ino(ino_n)
        for lba in ino["direct"]:
            if lba:
                self.free_sector(lba)
        if ino["ind"]:
            t = self.rd(ino["ind"])
            for i in range(PTRS):
                p = struct.unpack_from("<I", t, i * 4)[0]
                if p:
                    self.free_sector(p)
            self.free_sector(ino["ind"])
        self.put_ino(ino_n, dict(type=0))
        d = self.get_ino(dino)
        for s_ in range((d["size"] + 511) // 512):
            lba = self.block(d, s_); b = self.rd(lba)
            for e in range(8):
                if struct.unpack_from("<I", b, e * DENT)[0] == ino_n:
                    for i in range(DENT):
                        b[e * DENT + i] = 0
                    self.wr(lba, b)
                    return

    def put(self, path, data, aux=0):
        parent, _, name = path.rstrip("/").rpartition("/")
        dino = self.walk(parent)
        if self.lookup(dino, name):
            raise SystemExit(f"mwfs.py: {path} already exists")
        n = self.alloc_ino(T_FILE)
        ino = self.write_data(n, data)
        ino.update(type=T_FILE, crc=zlib.crc32(data) & 0xFFFFFFFF, aux=aux)
        self.put_ino(n, ino)
        self.add_entry(dino, name, n, T_FILE)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("cmd", choices=["format", "put", "ls"])
    ap.add_argument("image")
    ap.add_argument("file", nargs="?")
    ap.add_argument("--lba", type=int, required=True)
    ap.add_argument("--sectors", type=int, required=True)
    ap.add_argument("--label", default="MINIWIN")
    ap.add_argument("--dirs", default="docs,progs,mnt,sys")
    ap.add_argument("--path", default="/")
    ap.add_argument("--aux", type=int, default=0)
    a = ap.parse_args()
    v = Vol(a.image, a.lba, a.sectors)
    if a.cmd == "format":
        v.mkfs(a.label)
        for d in [x for x in a.dirs.split(",") if x]:
            v.mkdir(1, d)
        print(f"mwfs.py: formatted {a.sectors} sectors at LBA {a.lba} ({v.inode_count} inodes)")
    else:
        v.open()
        if a.cmd == "put":
            data = open(a.file, "rb").read()
            v.put(a.path, data, a.aux)
            print(f"mwfs.py: {a.path} <- {a.file} ({len(data)} bytes)")
        else:
            for n, ino, typ in v.entries(v.walk(a.path)):
                print(("d " if typ == T_DIR else "f ") + n)


if __name__ == "__main__":
    main()
