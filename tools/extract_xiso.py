#!/usr/bin/env python3
"""Extract an original-Xbox disc image (XDVDFS; redump XGD1 or plain xiso).

usage: extract_xiso.py <image.iso> <out_dir> [--only NAME ...]
"""
import os
import struct
import sys

MAGIC = b"MICROSOFT*XBOX*MEDIA"
SECTOR = 2048


def find_base(f):
    for off in (0, 0x18300000, 0xFD90000, 0x2080000):
        f.seek(off + 0x10000)
        if f.read(20) == MAGIC:
            return off
    raise SystemExit("no XDVDFS volume found")


def read_dir(f, base, sec, size):
    f.seek(base + sec * SECTOR)
    d = f.read(size)
    out = []
    seen = set()

    def node(off):
        if off in seen or off * 4 + 14 > len(d):
            return
        seen.add(off)
        l, r, s, sz, attr, nl = struct.unpack_from("<HHIIBB", d, off * 4)
        if l == 0xFFFF and r == 0xFFFF:
            return
        name = d[off * 4 + 14:off * 4 + 14 + nl].decode("latin1")
        if l:
            node(l)
        out.append((name, s, sz, attr))
        if r:
            node(r)

    if size:
        node(0)
    return out


def extract(f, base, sec, size, dst, only, rel=""):
    os.makedirs(dst, exist_ok=True)
    n = 0
    for name, s, sz, attr in read_dir(f, base, sec, size):
        p = os.path.join(dst, name)
        r = (rel + "/" + name).lstrip("/")
        if attr & 0x10:
            n += extract(f, base, s, sz, p, only, r)
            continue
        if only and r not in only:
            continue
        f.seek(base + s * SECTOR)
        left = sz
        with open(p, "wb") as o:
            while left:
                chunk = f.read(min(left, 1 << 22))
                if not chunk:
                    raise SystemExit("short read in " + r)
                o.write(chunk)
                left -= len(chunk)
        n += 1
    return n


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    img, out = sys.argv[1], sys.argv[2]
    only = set(sys.argv[sys.argv.index("--only") + 1:]) if "--only" in sys.argv else None
    with open(img, "rb") as f:
        base = find_base(f)
        f.seek(base + 0x10000)
        h = f.read(SECTOR)
        root_sec, root_size = struct.unpack_from("<II", h, 20)
        n = extract(f, base, root_sec, root_size, out, only)
    print(f"extracted {n} files to {out}")


if __name__ == "__main__":
    main()
