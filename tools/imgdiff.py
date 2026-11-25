#!/usr/bin/env python3
"""imgdiff.py a.bmp b.bmp [--box]: how many pixels differ between two of the PC build's shots, and where."""
import struct
import sys


def load(p):
    d = open(p, "rb").read()
    off = struct.unpack_from("<I", d, 10)[0]
    w, h = struct.unpack_from("<ii", d, 18)
    step = struct.unpack_from("<H", d, 28)[0] // 8
    rowb = ((w * step) + 3) & ~3
    rows = []
    for y in range(abs(h)):
        sy = (abs(h) - 1 - y) if h > 0 else y
        r = d[off + sy * rowb: off + sy * rowb + w * step]
        rows.append([tuple(r[x * step: x * step + 3]) for x in range(w)])
    return w, abs(h), rows


def main():
    w, h, a = load(sys.argv[1])
    _, _, b = load(sys.argv[2])
    n = 0
    x0, y0, x1, y1 = w, h, -1, -1
    for y in range(h):
        for x in range(w):
            if max(abs(i - j) for i, j in zip(a[y][x], b[y][x])) > 24:
                n += 1
                x0, y0, x1, y1 = min(x0, x), min(y0, y), max(x1, x), max(y1, y)
    print("%s vs %s: %d pixels differ%s" % (sys.argv[1], sys.argv[2], n,
                                             " in (%d,%d)-(%d,%d)" % (x0, y0, x1, y1) if n else ""))


if __name__ == "__main__":
    main()
