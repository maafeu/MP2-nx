#!/usr/bin/env python3
"""contact_sheet.py OUT.png FOLDER [COLS] [SCALE]: the PC build's or the port's shots (NNNN.bmp, not *_screen.bmp) in
FOLDER as one PNG, COLS a row (default 4), each shrunk by SCALE (default 4), the shot's number under none. No
libraries: tools/imgdiff.py reads the BMPs."""
import os
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from imgdiff import load  # noqa: E402

out, folder = sys.argv[1], sys.argv[2]
cols = int(sys.argv[3]) if len(sys.argv) > 3 else 4
scale = int(sys.argv[4]) if len(sys.argv) > 4 else 4
names = sorted(n for n in os.listdir(folder) if n.lower().endswith('.bmp') and '_screen' not in n)
tiles = []
for n in names:
    w, h, px = load(os.path.join(folder, n))
    tiles.append([[px[y][x] for x in range(0, w, scale)] for y in range(0, h, scale)])
tw = max(len(t[0]) for t in tiles)
th = max(len(t) for t in tiles)
gap = 4
rows = (len(tiles) + cols - 1) // cols
W = cols * tw + (cols - 1) * gap
H = rows * th + (rows - 1) * gap
img = [[(255, 255, 255)] * W for _ in range(H)]
for i, t in enumerate(tiles):
    ox = (i % cols) * (tw + gap)
    oy = (i // cols) * (th + gap)
    for y, row in enumerate(t):
        img[oy + y][ox:ox + len(row)] = row
raw = bytearray()
for r in img:
    raw.append(0)
    for p in r:
        raw += bytes((p[2], p[1], p[0]))


def chunk(tag, data):
    return struct.pack('>I', len(data)) + tag + data + struct.pack('>I', zlib.crc32(tag + data) & 0xFFFFFFFF)


png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', W, H, 8, 2, 0, 0, 0)) + \
    chunk(b'IDAT', zlib.compress(bytes(raw), 6)) + chunk(b'IEND', b'')
open(out, 'wb').write(png)
print(out, W, H, len(tiles), 'shots')
