#!/usr/bin/env python3
"""stick_shift.py SHOTS_DIR FIRST LAST [ROWS]: how far the picture moved between consecutive PC screenshots
(build/mp2-pc/shots/NNNN.bmp), found as the shift with the least mean difference over a band of rows (default the
top 45%, above Max), coarse at a quarter size and then at full size: the horizontal and vertical shift in pixels of
the 640x480 frame, for measuring the camera's answer to the right stick (tools/scripts/mp2_stick_measure.txt).
PIL only (no numpy on this PC)."""
import os
import sys

from PIL import Image, ImageChops, ImageStat


def load(d, i, rows):
    im = Image.open(os.path.join(d, "%04d.bmp" % i)).convert("L")
    return im.crop((0, 0, im.width, int(im.height * rows)))


def cost(a, b, dx, dy):
    """mean difference of b moved by (dx, dy) against a, over their overlap (at least half the width)"""
    w, h = a.size
    if abs(dx) > w // 2 or abs(dy) > h // 2:
        return 1e9
    ax0, bx0 = max(0, dx), max(0, -dx)
    ay0, by0 = max(0, dy), max(0, -dy)
    ow, oh = w - abs(dx), h - abs(dy)
    pa = a.crop((ax0, ay0, ax0 + ow, ay0 + oh))
    pb = b.crop((bx0, by0, bx0 + ow, by0 + oh))
    return ImageStat.Stat(ImageChops.difference(pa, pb)).mean[0]


def best(a, b, xs, ys):
    return min(((cost(a, b, x, y), x, y) for x in xs for y in ys))


def shift(a, b):
    """(dx, dy): the content of b lies dx right and dy down of where it was in a"""
    q = 4
    sa = a.resize((a.width // q, a.height // q), Image.BILINEAR)
    sb = b.resize((b.width // q, b.height // q), Image.BILINEAR)
    c, x, y = best(sa, sb, range(-sa.width // 2 + 1, sa.width // 2), range(-30, 31))
    c, x, y = best(a, b, range(x * q - q, x * q + q + 1), range(y * q - q, y * q + q + 1))
    still = cost(a, b, 0, 0)
    return -x, -y, c, still


def main():
    d, first, last = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
    rows = float(sys.argv[4]) if len(sys.argv) > 4 else 0.45
    for i in range(first, last):
        x, y, c, still = shift(load(d, i, rows), load(d, i + 1, rows))
        print("%04d -> %04d: dx %+4d dy %+4d (difference %.1f, unmoved %.1f)" % (i, i + 1, x, y, c, still))


if __name__ == "__main__":
    main()
