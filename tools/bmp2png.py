#!/usr/bin/env python3
"""bmp2png.py <file.bmp>... : the PC build's shots (24/32-bit BMP) as PNG beside them, with no libraries."""
import struct
import sys
import zlib


def convert(src, dst):
    d = open(src, "rb").read()
    off = struct.unpack_from("<I", d, 10)[0]
    w, h = struct.unpack_from("<ii", d, 18)
    bpp = struct.unpack_from("<H", d, 28)[0]
    step = bpp // 8
    rowb = ((w * step) + 3) & ~3
    raw = bytearray()
    for y in range(abs(h)):
        sy = (abs(h) - 1 - y) if h > 0 else y
        row = d[off + sy * rowb: off + sy * rowb + w * step]
        raw.append(0)
        for x in range(w):
            b, g, r = row[x * step: x * step + 3]
            raw += bytes((r, g, b))

    def chunk(t, c):
        return struct.pack(">I", len(c)) + t + c + struct.pack(">I", zlib.crc32(t + c) & 0xFFFFFFFF)

    png = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, abs(h), 8, 2, 0, 0, 0)) +
           chunk(b"IDAT", zlib.compress(bytes(raw), 6)) + chunk(b"IEND", b""))
    open(dst, "wb").write(png)


if __name__ == "__main__":
    for f in sys.argv[1:]:
        convert(f, f[:-4] + ".png")
